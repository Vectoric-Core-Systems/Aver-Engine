// game::applyPlacedComponent / capturePlacedComponent / applyLevelComponents (aver/game/LevelComponents.hpp): the COMPONENT record
// -> live components on a placement's entity, and back. A real scene::World; no device, no window.
//
// What these pin: every authored field is written (storage arrives zero-filled, so an IkRig weight left 0 would attach and do
// nothing), asset ids are fnv1a64 of the forward-slash path, capture is the canonical record (defaults omitted, one spelling of a
// number), dependency order is by type and not by file order, and applying twice or undoing and redoing leaves the same bytes.
#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/ControlRig.hpp"
#include "aver/anim/MotionMatchSystem.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/game/LevelComponents.hpp"
#include "aver/game/ColliderShapes.hpp"
#include "aver/scene/ComponentPool.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#include "aver/scene/scene_abi.h"
#include "aver/world/LevelInstance.hpp"
#if AVER_MODULE_SYNAPSE_SCENE
#  include "aver/synapse/AiPaths.hpp"
#  include "aver/synapse/BtAsset.hpp"
#  include "aver/synapse/StateTree.hpp"
#  include "aver/synapse/StateTreeAsset.hpp"
#  include "aver/synapse/SynapseBt.hpp"
#  include "aver/synapse/SynapseSmartObject.hpp"
#endif

#include <cmath>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <map>
#include <string>
#include <utility>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}
static bool near(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static fmt::OcPlacedComponent rec(const char* type, std::initializer_list<std::pair<const char*, const char*>> kv = {}) {
    fmt::OcPlacedComponent r;
    r.type = type;
    for (const auto& p : kv) r.kv.emplace_back(p.first, p.second);
    return r;
}

// An entity that carries a mesh, like every level placement.
static scene::Entity mesh(scene::World& w, const char* name) {
    const scene::Entity e = w.create(name, scene::kInvalidEntity, Transform{});
    auto* mr = static_cast<scene::CMeshRenderer*>(w.addComponent(e, scene::kComponentMeshRenderer));
    *mr = scene::CMeshRenderer{};
    mr->mesh = fnv1a64(std::string_view("Meshes/Char.ocmesh"));
    return e;
}
// ...and a skeleton, applied through the same code under test.
static scene::Entity skinned(scene::World& w, const char* name) {
    const scene::Entity e = mesh(w, name);
    game::applyPlacedComponent(w, e, rec("SkeletalMesh", {{"skeleton", "Meshes/Char.ocskel"}}));
    return e;
}

static std::string bytesOf(scene::World& w, scene::Entity e, u32 type) {
    const void* p = w.getComponent(e, type);
    return p ? std::string(static_cast<const char*>(p), w.componentSize(type)) : std::string{};
}
static u32 poolSize(scene::World& w, u32 type) {
    scene::ComponentPool* p = w.pool(type);
    return p ? static_cast<u32>(p->size()) : 0u;
}

static u64 idOf(const char* path) { return fnv1a64(std::string_view(path)); }

int main() {
    AVER_INFO("LevelComponentsTest");
    scene::World& w = scene::World::instance();
    anim::AnimSystem::registerComponents(w);
    anim::ControlRigSystem::registerComponents(w);
    anim::MotionMatchSystem::registerComponents(w);
    const u32 rootType = w.componentId("CRootMotion");
    const u32 mmType = w.componentId("CMotionMatcher");
    const u32 rigType = w.componentId("CControlRig");
    const u32 ikType = w.componentId("CIkRig");
    check(rootType && mmType && rigType && ikType, "the four dynamic anim components are registered");

#if AVER_MODULE_SYNAPSE_SCENE
    synapse::btSystem().registerComponents(w);
    synapse::SmartObjectSystem smartObjects;
    smartObjects.registerComponents(w);
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver_level_components_test";
    std::filesystem::create_directories(dir / "AI");
    synapse::setAiContentRoot(dir.generic_string());
#endif

    // The paths capture can name; anything else comes back in the `#=` hex form.
    std::map<u64, std::string> known;
    for (const char* p : {"Meshes/Char.ocskel", "Anim/Idle.ocanim", "Motion/Loco.ocmotion", "Rigs/Look.ocrig", "Rigs/Feet.ocikrig"})
        known[idOf(p)] = p;
    const game::PathOfIdFn pathOf = [&](u64 id) { const auto it = known.find(id); return it == known.end() ? std::string{} : it->second; };
    const game::PathOfIdFn none = [](u64) { return std::string{}; };

    // ---- 1. each type: fields written, ids are fnv1a64 of the path ----
    {
        AVER_INFO("apply: every authored field");
        const scene::Entity e = mesh(w, "One");
        check(game::applyPlacedComponent(w, e, rec("SkeletalMesh", {{"skeleton", "Meshes/Char.ocskel"}})), "SkeletalMesh applies");
        const auto* sm = w.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
        check(sm && sm->skeleton == idOf("Meshes/Char.ocskel") && sm->dirty == 1, "...skeleton is fnv1a64 of the path, dirty until the first tick resolves it");

        check(game::applyPlacedComponent(w, e, rec("Animator", {{"clip", "Anim/Idle.ocanim"}, {"speed", "2"}, {"once", "1"}})), "Animator applies");
        const auto* an = w.component<scene::CAnimator>(e, scene::kComponentAnimator);
        check(an && an->clip == idOf("Anim/Idle.ocanim") && near(an->speed, 2.0f) && near(an->blendWeight, 1.0f) && an->flags == scene::kAnimatorOnce,
              "...clip, speed, default weight 1, once but not paused");

        check(game::applyPlacedComponent(w, e, rec("RootMotion", {{"mode", "accumulate"}, {"axes", "x,z"}, {"sources", "montage"}, {"scale", "3"}, {"external", "1"}})),
              "RootMotion applies");
        const auto* rm = w.component<anim::CRootMotion>(e, rootType);
        check(rm && rm->mode == anim::kRootMotionAccumulate && rm->axes == (anim::kRootAxisX | anim::kRootAxisZ) && rm->sources == anim::kRootSrcMontage &&
                  near(rm->scale, 3.0f) && rm->flags == anim::kRootMotionExternal,
              "...mode, axes, sources, scale and the external flag");
        const scene::Entity e2 = skinned(w, "OneB");
        game::applyPlacedComponent(w, e2, rec("RootMotion", {{"mode", "apply"}}));
        rm = w.component<anim::CRootMotion>(e2, rootType);
        check(rm && rm->mode == anim::kRootMotionApply && rm->axes == anim::kRootAxesDefault && rm->sources == (anim::kRootSrcGraph | anim::kRootSrcMontage) &&
                  near(rm->scale, 1.0f) && rm->flags == 0,
              "...a bare mode=apply writes the defaults explicitly (x,y,yaw; both sources; scale 1)");

        check(game::applyPlacedComponent(w, e, rec("MotionMatcher", {{"database", "Motion/Loco.ocmotion"}, {"input", "velocity"}, {"facing", "yaw"}, {"playrate", "1.5"},
                                                                      {"interval", "0.25"}, {"velhalflife", "0.1"}, {"turnhalflife", "0.2"}, {"paused", "1"}})),
              "MotionMatcher applies");
        const auto* mm = w.component<anim::CMotionMatcher>(e, mmType);
        check(mm && mm->database == idOf("Motion/Loco.ocmotion") && mm->inputSource == anim::kMotionInputEntityVelocity &&
                  (mm->flags & anim::kMotionFaceYaw) && (mm->flags & anim::kMotionPaused) && near(mm->playRate, 1.5f) && near(mm->searchInterval, 0.25f) &&
                  near(mm->velHalfLife, 0.1f) && near(mm->turnHalfLife, 0.2f),
              "...database, inputSource == 1, facing yaw, paused, rates");
        game::applyPlacedComponent(w, e2, rec("MotionMatcher", {{"database", "Motion/Loco.ocmotion"}}));
        mm = w.component<anim::CMotionMatcher>(e2, mmType);
        check(mm && mm->inputSource == anim::kMotionInputScript && near(mm->playRate, 1.0f) && !(mm->flags & anim::kMotionFaceYaw),
              "...a record without input keeps today's script-driven behaviour, play rate 1");

        check(game::applyPlacedComponent(w, e, rec("ControlRig", {{"rig", "Rigs/Look.ocrig"}, {"weight", "0.5"}})), "ControlRig applies");
        const auto* cr = w.component<anim::CControlRig>(e, rigType);
        check(cr && cr->rig == idOf("Rigs/Look.ocrig") && near(cr->weight, 0.5f), "...rig and weight");

        check(game::applyPlacedComponent(w, e, rec("IkRig", {{"asset", "Rigs/Feet.ocikrig"}})), "IkRig applies");
        const auto* ik = w.component<anim::CIkRig>(e, ikType);
        check(ik && ik->asset == idOf("Rigs/Feet.ocikrig") && near(ik->weight, 1.0f) && ik->flags == 0,
              "...weight is 1 after a record with no weight key (a zero-filled one would do nothing), nothing switched off");
        game::applyPlacedComponent(w, e, rec("IkRig", {{"asset", "Rigs/Feet.ocikrig"}, {"weight", "0.8"}, {"goals", "0"}, {"body", "0"}}));
        ik = w.component<anim::CIkRig>(e, ikType);
        check(ik && near(ik->weight, 0.8f) && ik->flags == (anim::kIkRigGoalsOff | anim::kIkRigBodyOff), "...goals=0 body=0 set the two Off bits (the Feet-only preset)");

        check(game::applyPlacedComponent(w, e, rec("SoftBody", {{"maxdist", "12"}, {"compliance", "0.001"}, {"simulated", "0"}})), "SoftBody applies");
        const auto* sb = w.component<scene::CSoftBody>(e, scene::kComponentSoftBody);
        check(sb && near(sb->maxDistanceCm, 12.0f) && near(sb->compliance, 0.001f, 1e-6f) && sb->flags == scene::kSoftBodyDisabled && sb->body == 0,
              "...maxdist, compliance, simulated=0 is the Disabled bit; the process-local body handle stays 0");

#if AVER_MODULE_SYNAPSE_SCENE
        check(game::applyPlacedComponent(w, e, rec("SmartObject", {{"asset", "AI/Bench.ocso"}, {"enabled", "0"}})), "SmartObject applies (scene string-field ABI, no DLL)");
        const u32 soType = w.componentId("CSmartObject");
        const auto* so = w.component<synapse::CSmartObject>(e, soType);
        const u32 field = w.fieldId("CSmartObject.asset");
        check(so && so->enabled == 0 && std::string(aver_scene_get_str(static_cast<int32_t>(e), static_cast<int32_t>(field))) == "AI/Bench.ocso",
              "...asset string reads back through aver_scene_get_str, enabled 0");
#endif
        w.destroy(e); w.destroy(e2); w.flush();
    }

    // ---- 2. capture is the canonical record; ids with no known path use the hex form ----
    {
        AVER_INFO("capture: canonical form and the hex fallback");
        const scene::Entity e = mesh(w, "Cap");
        const std::vector<fmt::OcPlacedComponent> canon = {
            rec("SkeletalMesh", {{"skeleton", "Meshes/Char.ocskel"}}),
            rec("Animator", {{"clip", "Anim/Idle.ocanim"}, {"speed", "2"}, {"weight", "0.5"}, {"once", "1"}, {"paused", "1"}}),
            rec("RootMotion", {{"mode", "accumulate"}, {"axes", "x,z"}, {"sources", "montage"}, {"scale", "3"}, {"external", "1"}}),
            rec("MotionMatcher", {{"database", "Motion/Loco.ocmotion"}, {"input", "velocity"}, {"facing", "yaw"}, {"playrate", "1.5"}, {"interval", "0.25"},
                                  {"velhalflife", "0.1"}, {"turnhalflife", "0.2"}, {"paused", "1"}}),
            rec("ControlRig", {{"rig", "Rigs/Look.ocrig"}, {"weight", "0.5"}}),
            rec("IkRig", {{"asset", "Rigs/Feet.ocikrig"}, {"weight", "0.8"}, {"feet", "0"}, {"goals", "0"}, {"body", "0"}}),
            rec("SoftBody", {{"maxdist", "12"}, {"compliance", "0.001"}, {"simulated", "0"}}),
        };
        for (const auto& r : canon) game::applyPlacedComponent(w, e, r);
        for (const auto& r : canon) {
            const auto got = game::capturePlacedComponent(w, e, r.type, pathOf);
            check(got && *got == r, "capture(" + r.type + ") == the canonical input");
        }
        const std::vector<fmt::OcPlacedComponent> all = game::capturePlacedComponents(w, e, pathOf);
        check(all == canon, "capturePlacedComponents returns all of them in canonical order");

        // Defaults are omitted: a bare apply captures to its minimal line.
        const scene::Entity b = skinned(w, "CapB");
        game::applyPlacedComponent(w, b, rec("RootMotion", {{"mode", "apply"}, {"axes", "x,y,yaw"}, {"scale", "1"}}));
        game::applyPlacedComponent(w, b, rec("IkRig", {{"asset", "Rigs/Feet.ocikrig"}, {"weight", "1"}, {"feet", "1"}}));
        check(game::capturePlacedComponent(w, b, "RootMotion", pathOf) == rec("RootMotion", {{"mode", "apply"}}), "defaults are omitted: RootMotion mode=apply alone");
        check(game::capturePlacedComponent(w, b, "IkRig", pathOf) == rec("IkRig", {{"asset", "Rigs/Feet.ocikrig"}}), "...and IkRig with just its asset");
        check(!game::capturePlacedComponent(w, b, "Animator", pathOf), "a component the entity lacks captures to nullopt");

        // The hex form: a resolver that knows nothing still captures, and the record re-applies to identical bytes.
        const auto hex = game::capturePlacedComponent(w, e, "ControlRig", none);
        const std::string* hexKey = hex ? hex->find("rig#") : nullptr;
        check(hexKey && hexKey->size() == 16 && !hex->find("rig"), "unresolved id -> rig#=<16 hex digits>");
        const scene::Entity h = skinned(w, "CapHex");
        check(hex && game::applyPlacedComponent(w, h, *hex), "the hex record applies");
        check(bytesOf(w, h, rigType) == bytesOf(w, e, rigType), "...to bytes identical to the original");
        const auto hexSkel = game::capturePlacedComponent(w, h, "SkeletalMesh", none);
        const scene::Entity h2 = mesh(w, "CapHex2");
        check(hexSkel && hexSkel->find("skeleton#") && game::applyPlacedComponent(w, h2, *hexSkel) &&
                  bytesOf(w, h2, scene::kComponentSkeletalMesh) == bytesOf(w, h, scene::kComponentSkeletalMesh),
              "...and the same for a skeleton");
        w.destroy(e); w.destroy(b); w.destroy(h); w.destroy(h2); w.flush();
    }

    // ---- 3. dependency order is by type, not by file order; requirements refuse cleanly ----
    {
        AVER_INFO("dependency order and refusals");
        fmt::OcWorldPlacement p;
        p.components = {rec("RootMotion", {{"mode", "apply"}}), rec("Animator", {{"clip", "Anim/Idle.ocanim"}}), rec("SkeletalMesh", {{"skeleton", "Meshes/Char.ocskel"}})};
        const scene::Entity rev = mesh(w, "Rev");
        check(game::applyPlacedComponents(w, rev, p) == 3, "records in reverse order all apply (sorted into canonical order first)");
        fmt::OcWorldPlacement q;
        q.components = {p.components[2], p.components[1], p.components[0]};
        const scene::Entity fwd = mesh(w, "Fwd");
        game::applyPlacedComponents(w, fwd, q);
        check(bytesOf(w, rev, scene::kComponentAnimator) == bytesOf(w, fwd, scene::kComponentAnimator) &&
                  bytesOf(w, rev, rootType) == bytesOf(w, fwd, rootType) && bytesOf(w, rev, scene::kComponentSkeletalMesh) == bytesOf(w, fwd, scene::kComponentSkeletalMesh),
              "...and end byte-identical to canonical order");

        const scene::Entity bare = mesh(w, "Bare");
        check(!game::applyPlacedComponent(w, bare, rec("RootMotion", {{"mode", "apply"}})), "RootMotion without a skeleton is refused");
        check(!w.hasComponent(bare, rootType), "...and leaves no component behind");
        check(!game::applyPlacedComponent(w, bare, rec("Animator", {{"clip", "Anim/Idle.ocanim"}})) && !w.hasComponent(bare, scene::kComponentAnimator),
              "Animator without a skeleton is refused too");
        const scene::Entity noMesh = w.create("NoMesh", scene::kInvalidEntity, Transform{});
        check(!game::applyPlacedComponent(w, noMesh, rec("SkeletalMesh", {{"skeleton", "Meshes/Char.ocskel"}})) && !w.hasComponent(noMesh, scene::kComponentSkeletalMesh),
              "a record on an entity with no mesh is refused");
        check(!game::applyPlacedComponent(w, bare, rec("Wobble", {{"x", "1"}})), "an unknown type is refused (kept in the file by the format, not applied)");
        check(!game::applyPlacedComponent(w, bare, rec("RootMotion", {{"mode", "sideways"}})), "an unknown mode is refused");
        w.destroy(rev); w.destroy(fwd); w.destroy(bare); w.destroy(noMesh); w.flush();
    }

    // ---- 4. idempotent ----
    {
        AVER_INFO("idempotent");
        const scene::Entity e = skinned(w, "Idem");
        const auto r = rec("MotionMatcher", {{"database", "Motion/Loco.ocmotion"}, {"input", "velocity"}});
        game::applyPlacedComponent(w, e, r);
        const std::string once = bytesOf(w, e, mmType);
        const u32 before = poolSize(w, mmType);
        game::applyPlacedComponent(w, e, r);
        check(bytesOf(w, e, mmType) == once && poolSize(w, mmType) == before, "applying the same record twice leaves one component and the same bytes");
        game::applyPlacedComponent(w, e, rec("MotionMatcher", {{"database", "Motion/Loco.ocmotion"}, {"input", "velocity"}, {"playrate", "2"}}));
        const auto* mm = w.component<anim::CMotionMatcher>(e, mmType);
        check(mm && near(mm->playRate, 2.0f) && poolSize(w, mmType) == before, "applying a changed record updates in place");
        // A script's one-shot request survives an authored update.
        w.component<anim::CMotionMatcher>(e, mmType)->flags |= anim::kMotionForceSearch;
        game::applyPlacedComponent(w, e, r);
        check((w.component<anim::CMotionMatcher>(e, mmType)->flags & anim::kMotionForceSearch) != 0, "kMotionForceSearch (a script's, not authored) is left alone");
        w.destroy(e); w.flush();
    }

    // ---- 5. remove ----
    {
        AVER_INFO("remove");
        const scene::Entity e = skinned(w, "Rem");
        game::applyPlacedComponent(w, e, rec("IkRig", {{"asset", "Rigs/Feet.ocikrig"}}));
        check(game::removePlacedComponent(w, e, "IkRig"), "removePlacedComponent removes");
        check(!w.hasComponent(e, ikType) && !game::capturePlacedComponent(w, e, "IkRig", pathOf), "...capture then returns nullopt");
        check(!game::removePlacedComponent(w, e, "IkRig"), "a second remove returns false");
        check(!game::removePlacedComponent(w, e, "Wobble"), "an unknown type returns false");
        w.destroy(e); w.flush();
    }

    // ---- 6. the post-pass over a hand-built LevelInstance ----
    {
        AVER_INFO("applyLevelComponents");
        fmt::OcWorldData lvl;
        lvl.placements.resize(4);
        lvl.placements[0].components = {rec("SkeletalMesh", {{"skeleton", "Meshes/Char.ocskel"}}), rec("Animator", {{"clip", "Anim/Idle.ocanim"}})};
        lvl.placements[1].className = "AN_Thing";   // a class placement: no entity, so nothing to apply to
        lvl.placements[1].components = {rec("SkeletalMesh", {{"skeleton", "Meshes/Char.ocskel"}})};
        lvl.placements[2].animClip = "Anim/Spin.ocanim";
        lvl.placements[2].components = {rec("SkeletalMesh", {{"skeleton", "Meshes/Char.ocskel"}})};
        const scene::Entity e0 = mesh(w, "P0"), e2 = mesh(w, "P2"), e3 = mesh(w, "P3");
        world::LevelInstance inst;
        inst.entities = {e0, e2, e3};
        inst.placementIndex = {0, 2, 3};
        game::applyLevelComponents(w, lvl, inst);
        check(w.hasComponent(e0, scene::kComponentSkeletalMesh) && w.hasComponent(e0, scene::kComponentAnimator), "components land on the entity of their own placement");
        check(!w.hasComponent(e2, scene::kComponentSkeletalMesh), "a placement with an anim clip refuses a SkeletalMesh record (the two meanings of CAnimator)");
        check(!w.hasComponent(e3, scene::kComponentSkeletalMesh) && !w.hasComponent(e3, scene::kComponentAnimator), "a placement without records is untouched");
        world::LevelInstance empty;
        game::applyLevelComponents(w, lvl, empty);   // a streamed level: nothing resident yet
        check(true, "an empty instance (streamed level) is a no-op");
        w.destroy(e0); w.destroy(e2); w.destroy(e3); w.flush();
    }

    // ---- 7. undo / redo equivalence the editor relies on ----
    {
        AVER_INFO("undo / redo");
        const scene::Entity e = skinned(w, "Undo");
        const auto before = rec("IkRig", {{"asset", "Rigs/Feet.ocikrig"}, {"goals", "0"}});
        const auto after = rec("IkRig", {{"asset", "Rigs/Feet.ocikrig"}, {"weight", "0.3"}, {"body", "0"}});
        game::applyPlacedComponent(w, e, before);
        const std::string start = bytesOf(w, e, ikType);
        game::applyPlacedComponent(w, e, after);
        const std::string mid = bytesOf(w, e, ikType);
        game::applyPlacedComponent(w, e, before);
        check(mid != start && bytesOf(w, e, ikType) == start, "apply(before), apply(after), apply(before) ends byte-identical to the start");
        game::applyPlacedComponent(w, e, after);
        check(bytesOf(w, e, ikType) == mid, "...and redo reaches the same bytes as the first apply(after)");

        game::applyPlacedComponent(w, e, rec("Animator", {{"clip", "Anim/Idle.ocanim"}}));
        const std::string a0 = bytesOf(w, e, scene::kComponentAnimator);
        game::applyPlacedComponent(w, e, rec("Animator", {{"clip", "Anim/Idle.ocanim"}, {"speed", "2"}, {"paused", "1"}}));
        game::applyPlacedComponent(w, e, rec("Animator", {{"clip", "Anim/Idle.ocanim"}}));
        check(bytesOf(w, e, scene::kComponentAnimator) == a0, "an Animator edit and its undo restore the flags and speed");

        // before = nullopt means the component did not exist: the editor removes it.
        check(game::removePlacedComponent(w, e, "IkRig") && !w.hasComponent(e, ikType), "before=nullopt -> removePlacedComponent");
        w.destroy(e); w.flush();
    }

#if AVER_MODULE_SYNAPSE_SCENE
    // ---- 8. brains: a behaviour tree and a state tree, loaded through the content root ----
    {
        AVER_INFO("Brain");
        std::string why;
        check(synapse::saveBtAsset((dir / "AI" / "T.ocbt").generic_string(), synapse::btAssetStarter(), &why), "wrote a minimal .ocbt: " + why);
        check(synapse::saveStateTree((dir / "AI" / "S.ocst").generic_string(), synapse::stStarter(), &why), "wrote a minimal .ocst: " + why);
        synapse::BtSystem& bt = synapse::btSystem();
        const u32 bType = bt.componentType();

        const scene::Entity e = mesh(w, "Brain");
        const auto input = rec("Brain", {{"asset", "AI/T.ocbt"}, {"team", "guards"}});
        check(game::applyPlacedComponent(w, e, input), "Brain asset=AI/T.ocbt applies");
        const auto* b = w.component<synapse::CSynapseBehavior>(e, bType);
        check(b && b->treeAssetId == idOf("AI/T.ocbt") && bt.brainKind(e) == 1, "...treeAssetId is the loaded id, brainKind == 1 (behaviour tree)");
        check(b && b->teamId != 0 && bt.teamName(b->teamId) && *bt.teamName(b->teamId) == "guards", "...team is set");
        const auto got = game::capturePlacedComponent(w, e, "Brain", pathOf);
        check(got && *got == input, "capture returns the same record, the path via findTree(id)->path");

        const scene::Entity s = mesh(w, "StateBrain");
        check(game::applyPlacedComponent(w, s, rec("Brain", {{"asset", "AI/S.ocst"}})) && bt.brainKind(s) == 2, "an .ocst applies as a state tree (brainKind == 2)");
        const auto sGot = game::capturePlacedComponent(w, s, "Brain", pathOf);
        check(sGot && *sGot == rec("Brain", {{"asset", "AI/S.ocst"}}), "...and captures through findStateTree(id)->path");

        const scene::Entity m = mesh(w, "MissingBrain");
        check(!game::applyPlacedComponent(w, m, rec("Brain", {{"asset", "AI/Missing.ocbt"}})) && !w.hasComponent(m, bType), "a tree that does not load is refused and leaves no component");
        const bool emptyOk = game::applyPlacedComponent(w, m, rec("Brain")) && w.hasComponent(m, bType);
        const auto emptyGot = game::capturePlacedComponent(w, m, "Brain", pathOf);
        check(emptyOk && emptyGot && !emptyGot->find("asset"), "a Brain with no asset attaches empty (what the menu creates) and captures with no asset key");
        check(game::applyPlacedComponent(w, e, rec("Brain")) && w.component<synapse::CSynapseBehavior>(e, bType)->treeAssetId == 0,
              "applying an empty Brain over an assigned one clears the tree (undo of an assignment)");

        check(game::removePlacedComponent(w, s, "Brain") && !w.hasComponent(s, bType), "removing a Brain stops it, then removes the component");

        // Smart object: capture and re-apply, and a duplicate owns its own string slot only through the editor's snapshot fix (not tested here).
        const scene::Entity so = mesh(w, "SO");
        const auto soRec = rec("SmartObject", {{"asset", "AI/Bench.ocso"}, {"enabled", "0"}});
        check(game::applyPlacedComponent(w, so, soRec), "SmartObject applies");
        const auto soGot = game::capturePlacedComponent(w, so, "SmartObject", pathOf);
        check(soGot && *soGot == soRec, "...and captures to the same record");
        fmt::OcPlacedComponent badSo = rec("SmartObject");
        badSo.kv.emplace_back("asset#", "00000000000000ff");
        check(!game::applyPlacedComponent(w, so, badSo), "a smart object has no hex form: refused");
        w.destroy(e); w.destroy(s); w.destroy(m); w.destroy(so); w.flush();
    }
#endif

    // ---- Collision Shape (SHAPES_MATERIALS phase E): the record -> CCollisionShape + a Rigid Body through the level applier, and back ----
    {
        AVER_INFO("apply: CollisionShape");
        const u32 csType = game::registerCollisionShapeComponent(w);
        known[idOf("Physics/Chair.ocshape")] = "Physics/Chair.ocshape";
        const scene::Entity e = mesh(w, "Crate");
        const auto input = rec("CollisionShape", {{"asset", "Physics/Chair.ocshape"}, {"motion", "dynamic"}, {"noscale", "1"}});
        check(csType != 0 && game::applyPlacedComponent(w, e, input), "CollisionShape applies through the level applier");
        const auto* c = w.component<game::CCollisionShape>(e, csType);
        check(c && c->asset == idOf("Physics/Chair.ocshape") && c->flags == game::kCollisionShapeIgnoreScale, "...asset id and the no-scale flag");
        const auto* rb = w.component<scene::CRigidBody>(e, scene::kComponentRigidBody);
        check(rb && rb->motionType == 2, "...and a Rigid Body carrying the motion type");
        const auto got = game::capturePlacedComponent(w, e, "CollisionShape", pathOf);
        check(got && *got == input, "capture returns the same record");
        check(game::removePlacedComponent(w, e, "CollisionShape") && !w.hasComponent(e, csType) && !w.hasComponent(e, scene::kComponentRigidBody),
              "removing it also takes back the zero-size Rigid Body the record attached");
        w.destroy(e); w.flush();
    }
    if (g_failures) { AVER_ERROR("LevelComponentsTest: {} check(s) failed", g_failures); return 1; }
    AVER_INFO("LevelComponentsTest: all checks passed");
    return 0;
}
