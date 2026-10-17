// Hand-run test for sandbox/src/PlacedComponentModel.hpp: the rules behind Details > Add Component for the animation and AI
// components, with no window. Which component may be attached or removed where (and the reason shown when it may not), the
// asset candidates a picker offers, the creation defaults, and the "skeleton next to the mesh" preselect. The creation
// defaults are also pushed through the real applier (game::applyPlacedComponent) on a live scene::World: a default that the
// applier refuses, or that captures back as something else, would make the menu's Add a no-op or a phantom edit.
// ImGui layout is not testable headless (ADD_COMPONENT.md 9.5). Exit code = failure count.
#include "PlacedComponentModel.hpp"

#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/ControlRig.hpp"
#include "aver/anim/MotionMatchSystem.hpp"
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/game/LevelComponents.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/World.hpp"
#if AVER_MODULE_SYNAPSE_SCENE
#  include "aver/synapse/SynapseBt.hpp"
#  include "aver/synapse/SynapseSmartObject.hpp"
#endif

#include <string>
#include <vector>

using namespace aver;
using namespace aver::editor;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) return;
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static const PlacedKind& kind(std::string_view type) {
    const PlacedKind* k = findPlacedKind(type);
    static const PlacedKind kNone{};
    return k ? *k : kNone;
}

// A level placement with a mesh and (optionally) a skeleton: the context most kinds are happy with.
static AttachContext baseCtx(bool skeleton) {
    AttachContext c;
    c.levelOwned = true;
    c.hasMesh = true;
    c.hasSkeleton = skeleton;
    return c;
}

static bool attachable(const AttachContext& c, std::string_view type, std::string* why = nullptr) {
    return canAttach(c, kind(type), why);
}

int main() {
    AVER_INFO("PlacedComponentModel test");

    // ---- the table: canonical order, labels, registration ----
    {
        static const char* const kOrder[] = {"SkeletalMesh", "Animator", "RootMotion", "MotionMatcher", "ControlRig", "IkRig", "Brain", "SmartObject", "Cloth", "Destructible"};
        static const char* const kLabels[] = {"Skeletal Mesh", "Animator", "Root Motion", "Motion Matching", "Control Rig", "IK Rig", "AI Brain", "Smart Object", "Cloth", "Destructible"};
        check(kPlacedKindCount == 10, "ten kinds in the Add Component menu (Soft Body keeps its own entry)");
        for (usize i = 0; i < kPlacedKindCount && i < 10; ++i) {
            check(std::string_view(kPlacedKinds[i].type) == kOrder[i], std::string("kind ") + std::to_string(i) + " is " + kOrder[i] + " (canonical order)");
            check(std::string_view(kPlacedKinds[i].menuLabel) == kLabels[i], std::string("menu label of ") + kOrder[i]);
            // kPlacedComponentTypes carries "SoftBody" at index 8 (it keeps its own menu entry), so Cloth/Destructible sit one place later.
            const usize at = i < 8 ? i : i + 1;
            check(at < std::size(game::kPlacedComponentTypes) && game::kPlacedComponentTypes[at] == kOrder[i],
                  std::string(kOrder[i]) + " sits where the applier's canonical order puts it");
            check(kPlacedKinds[i].tip && *kPlacedKinds[i].tip && kPlacedKinds[i].header && *kPlacedKinds[i].header,
                  std::string(kOrder[i]) + " has a tooltip and a section header");
        }
        check(kind("SkeletalMesh").builtinId == scene::kComponentSkeletalMesh && kind("Animator").builtinId == scene::kComponentAnimator,
              "SkeletalMesh and Animator are the two built-in ids");
        check(std::string_view(kind("RootMotion").componentName) == "CRootMotion" && kind("RootMotion").builtinId == 0,
              "RootMotion registers by name");
        check(std::string_view(kind("MotionMatcher").componentName) == "CMotionMatcher", "MotionMatcher registers as CMotionMatcher");
        check(std::string_view(kind("ControlRig").componentName) == "CControlRig", "ControlRig registers as CControlRig");
        check(std::string_view(kind("IkRig").componentName) == "CIkRig", "IkRig registers as CIkRig");
        check(std::string_view(kind("Brain").componentName) == "CSynapseBehavior", "Brain registers as CSynapseBehavior");
        check(std::string_view(kind("SmartObject").componentName) == "CSmartObject", "SmartObject registers as CSmartObject");
        check(findPlacedKind("Nope") == nullptr && findPlacedKind("SoftBody") == nullptr, "an unknown type, and Soft Body, are not placed kinds");
        check(placedAssetExts(kind("Brain")).size() == 2 && placedAssetExts(kind("RootMotion")).empty(),
              "Brain takes two extensions, Root Motion takes no asset");
    }

    // ---- canAttach: every row of the "Needs" lines ----
    {
        std::string why;
        // A plain level mesh without a skeleton: only the kinds that need none.
        const AttachContext plain = baseCtx(false);
        check(attachable(plain, "SkeletalMesh"), "a Skeletal Mesh attaches to a level mesh");
        check(attachable(plain, "Brain") && attachable(plain, "SmartObject"), "a Brain and a Smart Object need only a mesh placement");
        for (const char* t : {"Animator", "RootMotion", "MotionMatcher", "ControlRig", "IkRig"}) {
            why.clear();
            check(!attachable(plain, t, &why), std::string(t) + " is refused without a skeleton");
            check(why.find("Skeletal Mesh") != std::string::npos, std::string(t) + " names the missing Skeletal Mesh: '" + why + "'");
        }
        const AttachContext skinned = baseCtx(true);
        for (const char* t : {"Animator", "RootMotion", "MotionMatcher", "ControlRig", "IkRig", "Brain", "SmartObject", "SkeletalMesh"})
            check(attachable(skinned, t), std::string(t) + " attaches once the mesh has a skeleton");

        // No mesh: nothing attaches, and the reason says so.
        AttachContext noMesh = skinned;
        noMesh.hasMesh = false;
        for (const PlacedKind& k : kPlacedKinds) {
            why.clear();
            check(!canAttach(noMesh, k, &why) && !why.empty(), std::string(k.type) + " is refused on an entity with no mesh");
        }

        // A graph-class root or COMP part is not a level placement.
        AttachContext notOwned = skinned;
        notOwned.levelOwned = false;
        for (const PlacedKind& k : kPlacedKinds) {
            why.clear();
            check(!canAttach(notOwned, k, &why) && why == "Not a level placement",
                  std::string(k.type) + " says 'Not a level placement' off the level: '" + why + "'");
        }

        AttachContext legacy = skinned;
        legacy.legacyOcmap = true;
        why.clear();
        check(!attachable(legacy, "Animator", &why) && why.find(".ocmap") != std::string::npos, "a legacy .ocmap level cannot store components: '" + why + "'");

        AttachContext prefab = skinned;
        prefab.prefabLinked = true;
        why.clear();
        check(!attachable(prefab, "Brain", &why) && why.find("prefab") != std::string::npos, "a prefab member is refused with the prefab reason: '" + why + "'");

        AttachContext inst = skinned;
        inst.instanceMember = true;
        why.clear();
        check(!attachable(inst, "Brain", &why) && why.find("level instance") != std::string::npos, "a level-instance member is refused: '" + why + "'");

        AttachContext streamed = skinned;
        streamed.streamedOnlyRecord = true;
        check(!attachable(streamed, "SmartObject"), "a streamed-out record has no entity to attach to");

        // The two meanings of CAnimator: an object clip and a skeleton cannot share a placement.
        AttachContext objAnim = baseCtx(false);
        objAnim.hasObjectAnim = true;
        why.clear();
        check(!attachable(objAnim, "SkeletalMesh", &why) && why.find("object animation") != std::string::npos,
              "a placement with an object animation refuses a Skeletal Mesh: '" + why + "'");
        check(attachable(objAnim, "Brain"), "an object animation does not stop a Brain");

        AttachContext present = skinned;
        present.alreadyPresent = true;
        why.clear();
        check(!attachable(present, "IkRig", &why) && why == "This entity already has one.", "already present is refused: '" + why + "'");

        // A passing call leaves *why untouched.
        why = "untouched";
        check(attachable(skinned, "Animator", &why) && why == "untouched", "a successful canAttach does not write the reason");
    }

    // ---- canRemove ----
    {
        std::string why;
        check(!canRemove(true, false, false, false, false, kind("SkeletalMesh"), &why) && why.find("Animator") != std::string::npos,
              "the skeleton is held by the Animator: '" + why + "'");
        why.clear();
        check(!canRemove(true, true, true, true, true, kind("SkeletalMesh"), &why), "the skeleton is held by all five dependents");
        for (const char* n : {"Animator", "Root Motion", "Motion Matching", "Control Rig", "IK Rig"})
            check(why.find(n) != std::string::npos, std::string("the message names ") + n + ": '" + why + "'");
        check(canRemove(false, false, false, false, false, kind("SkeletalMesh")), "a Skeletal Mesh with no dependents removes");
        for (const char* t : {"Animator", "RootMotion", "MotionMatcher", "ControlRig", "IkRig", "Brain", "SmartObject"})
            check(canRemove(true, true, true, true, true, kind(t)), std::string(t) + " is always removable");
    }

    // ---- listAssets ----
    {
        const std::unordered_map<u64, std::string> index = {
            {1, "C:/P/Content/Skeletons/Zed.ocskel"}, {2, "C:/P/Content/Meshes/KenneyChar.OCSKEL"}, {3, "C:/P/Content/Meshes/a.ocanim"},
            {4, "C:/P/Content/AI/Patrol.ocbt"},       {5, "C:/P/Content/AI/Guard.ocst"},            {6, "C:/P/Content/Meshes/a.ocmesh"},
            {7, "C:/P/Content/Anim/Car.ocanim"},      {8, "C:/P/Content/AI/readme.txt"},            {9, "C:/P/Content/AI/Bench.ocso"},
        };
        const auto rel = [](const std::string& abs) { return abs.substr(std::string("C:/P/Content/").size()); };
        const auto skels = listAssets(index, placedAssetExts(kind("SkeletalMesh")), rel);
        check(skels.size() == 2 && skels[0].label == "Meshes/KenneyChar.OCSKEL" && skels[1].label == "Skeletons/Zed.ocskel",
              "skeleton candidates are the .ocskel files (any case), sorted by relative label");
        check(skels.size() == 2 && skels[0].id == 2 && skels[1].id == 1, "candidates carry the content-index id");
        const auto brains = listAssets(index, placedAssetExts(kind("Brain")), rel);
        check(brains.size() == 2 && brains[0].label == "AI/Guard.ocst" && brains[1].label == "AI/Patrol.ocbt",
              "a Brain is offered both .ocbt and .ocst");
        check(listAssets(index, placedAssetExts(kind("RootMotion")), rel).empty(), "a kind with no asset extension lists nothing");
        // Animator: object clips (the car) are excluded by the injected predicate.
        const auto clips = listAssets(index, placedAssetExts(kind("Animator")), rel,
                                      [](u64 id, const std::string&) { return id != 7; });
        check(clips.size() == 1 && clips[0].label == "Meshes/a.ocanim", "the accept predicate drops object clips");
        check(listAssets(index, placedAssetExts(kind("Animator")), rel).size() == 2, "without a predicate every .ocanim is offered");
        check(listAssets({}, placedAssetExts(kind("IkRig")), rel).empty(), "an empty index lists nothing");
    }

    // ---- presetSkeletonFor ----
    {
        const std::vector<std::string> both = {"Meshes/KenneyChar.ocskel", "Skeletons/Character.ocskel"};
        check(presetSkeletonFor("Meshes/KenneyChar.ocmesh", both) == "Meshes/KenneyChar.ocskel", "the .ocskel next to the mesh with the same stem is picked");
        check(presetSkeletonFor("Meshes/KenneyChar.ocmesh", {"Skeletons/Character.ocskel"}).empty(), "a skeleton elsewhere with another stem is not guessed");
        check(presetSkeletonFor("Meshes/Character.ocmesh", {"Skeletons/Character.ocskel"}).empty(), "the same stem in another folder is not 'next to' the mesh");
        check(presetSkeletonFor("meshes/kenneychar.ocmesh", both) == "Meshes/KenneyChar.ocskel", "the match ignores case and returns the listing's spelling");
        check(presetSkeletonFor("Meshes/KenneyChar.ocmesh", {"Meshes/KenneyChar.ocanim"}).empty(), "only .ocskel counts");
        check(presetSkeletonFor("", both).empty() && presetSkeletonFor("Meshes/X.ocmesh", {}).empty(), "empty inputs give nothing");
    }

    // ---- makeNewRecord, through the real applier ----
    {
        scene::World& w = scene::World::instance();
        anim::AnimSystem::registerComponents(w);
        anim::ControlRigSystem::registerComponents(w);
        anim::MotionMatchSystem::registerComponents(w);
#if AVER_MODULE_SYNAPSE_SCENE
        synapse::btSystem().registerComponents(w);
        synapse::SmartObjectSystem smart;
        smart.registerComponents(w);
#endif
        const scene::Entity e = w.create("Meshes/PlacedComponentModelTest.ocmesh", scene::kInvalidEntity, Transform{});
        check(e != scene::kInvalidEntity, "test entity created");
        if (auto* mr = static_cast<scene::CMeshRenderer*>(w.addComponent(e, scene::kComponentMeshRenderer))) mr->mesh = 1;

        const game::PathOfIdFn noPaths = [](u64) { return std::string(); };
        for (const PlacedKind& k : kPlacedKinds) {
            const u32 id = k.builtinId != 0 ? k.builtinId : w.componentId(k.componentName);
            if (id == 0) continue;   // a module this build does not have (the AI components without Synapse.Scene)
            const fmt::OcPlacedComponent rec = makeNewRecord(k);
            check(rec.type == k.type, std::string(k.type) + ": the record carries its type");
            const bool ok = game::applyPlacedComponent(w, e, rec);
            check(ok, std::string(k.type) + ": the creation defaults apply cleanly");
            check(w.hasComponent(e, id), std::string(k.type) + ": the component is attached");
            const auto back = game::capturePlacedComponent(w, e, k.type, noPaths);
            check(back.has_value() && *back == rec,
                  std::string(k.type) + ": what the menu creates captures back as the same record (no phantom edit on the first gesture)");
            // The dependants need the skeleton the first kind created to be non-zero.
            if (std::string_view(k.type) == "SkeletalMesh") {
                if (auto* sm = w.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh)) sm->skeleton = 1;
            }
        }
        // The documented creation defaults, read off the live components.
        if (const u32 rid = w.componentId("CRootMotion"); rid != 0)
            if (const auto* rm = w.component<anim::CRootMotion>(e, rid))
                check(rm->mode == anim::kRootMotionApply, "Root Motion is created in Apply mode");
        if (const u32 mid = w.componentId("CMotionMatcher"); mid != 0)
            if (const auto* mm = w.component<anim::CMotionMatcher>(e, mid))
                check(mm->inputSource == anim::kMotionInputEntityVelocity, "Motion Matching is created following the entity's velocity");
        if (const u32 iid = w.componentId("CIkRig"); iid != 0)
            if (const auto* ik = w.component<anim::CIkRig>(e, iid))
                check(ik->weight == 1.0f, "an IK Rig is created at weight 1 (a zero-filled one would do nothing)");
        if (const auto* an = w.component<scene::CAnimator>(e, scene::kComponentAnimator))
            check(an->speed == 1.0f && an->blendWeight == 1.0f && an->flags == 0, "an Animator is created playing, looping, at speed and weight 1");
    }

    AVER_INFO("PlacedComponentModel test: {} checks, {} failure(s)", g_checks, g_failures);
    return g_failures;
}
