#include "aver/game/LevelComponents.hpp"
#include "aver/core/CharConv.hpp"   // floating from_chars where libc++ has none

#if AVER_MODULE_SCENE

#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/ControlRig.hpp"
#include "aver/anim/MotionMatchSystem.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/game/ColliderShapes.hpp"   // CollisionShape: registration, apply, capture, remove (wave 11 SHAPES_MATERIALS)
#include "aver/scene/Components.hpp"
#include "aver/scene/scene_abi.h"
#if AVER_MODULE_SYNAPSE_SCENE
#  include "aver/synapse/SynapseBt.hpp"
#  include "aver/synapse/SynapseSmartObject.hpp"
#endif
#if AVER_MODULE_SOFTBODY_SCENE
#  include "aver/softbody/SoftBodySystem.hpp"

#  include "aver/softbody/SoftShapeSystem.hpp"
#endif

#if AVER_MODULE_VEHICLE_SCENE
#  include "aver/vehicle/VehiclePhysicsSystem.hpp"
#  include "aver/vehicle/VehiclePresets.hpp"
#endif

#if AVER_MODULE_CAMERA
#  include "aver/camera/CameraRigSystem.hpp"
#endif

#if AVER_MODULE_HAIR_SCENE
#  include "aver/hair/HairSystem.hpp"
#endif

#if AVER_MODULE_FRAMEWORK
#  include "aver/game/PlacementProps.hpp"   // Groups and Props: records the framework keeps, not scene components
#endif

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <iterator>

namespace aver::game {

namespace {

enum TypeIx : int {
    kSkeletalMesh, kAnimator, kRootMotion, kMotionMatcher, kControlRig, kIkRig, kBrain, kSmartObject, kSoftBody, kCloth, kDestructible, kMorphTargets, kCameraRig, kVehicle, kParticleEmitter, kSoftField, kJelly, kRope, kCollisionShape, kHair, kTypeCount,
    kUnknownType = -1
};
static_assert(std::size(kPlacedComponentTypes) == kTypeCount, "kPlacedComponentTypes and TypeIx must stay in step");

// Warnings per call before the rest are only counted: a level of thousands of placements with one missing skeleton must not flood the log.
constexpr u32 kWarnCap = 16;

std::string lowered(std::string_view s) {
    std::string o(s);
    for (char& c : o) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}

int typeIndex(std::string_view t) {
    const std::string l = lowered(t);
    for (int i = 0; i < kTypeCount; ++i)
        if (lowered(kPlacedComponentTypes[i]) == l) return i;
    return kUnknownType;
}

// ---- numbers: the format keeps values as opaque strings, so both directions live here (%.6g out, from_chars in) ----

std::string numStr(double v) {
    char b[40];
    std::snprintf(b, sizeof b, "%.6g", v);
    return b;
}

bool parseNum(std::string_view s, double& out) {
    if (s.empty()) return false;
    const char* first = s.data();
    const char* last = s.data() + s.size();
    double v = 0.0;
    const auto r = aver::fromChars(first, last, v);
    if (r.ec != std::errc{} || r.ptr != last) return false;
    out = v;
    return true;
}

f32 numKey(const fmt::OcPlacedComponent& r, std::string_view key, f32 def) {
    const std::string* v = r.find(key);
    double d = 0.0;
    return v && parseNum(*v, d) ? static_cast<f32>(d) : def;
}

bool boolKey(const fmt::OcPlacedComponent& r, std::string_view key, bool def) {
    const std::string* v = r.find(key);
    double d = 0.0;
    return v && parseNum(*v, d) ? d != 0.0 : def;
}

std::string textKey(const fmt::OcPlacedComponent& r, std::string_view key) {
    const std::string* v = r.find(key);
    return v ? *v : std::string{};
}

// `value` is a comma list of words, any order, repeats allowed.
bool listHas(std::string_view value, std::string_view word) {
    usize pos = 0;
    while (pos <= value.size()) {
        usize end = value.find(',', pos);
        if (end == std::string_view::npos) end = value.size();
        if (lowered(value.substr(pos, end - pos)) == word) return true;
        pos = end + 1;
    }
    return false;
}

std::string hex16(u64 v) {
    char b[24];
    std::snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(v));
    return b;
}

std::string slashed(std::string_view p) {
    std::string o(p);
    std::replace(o.begin(), o.end(), '\\', '/');
    return o;
}

// An asset key is a content-relative path hashed with fnv1a64, or `<key>#` holding the 16 hex digits of an id the editor could not turn
// back into a path. Absent (or an empty path) is id 0 = none.
enum class AssetKey { Absent, Ok, Bad };
AssetKey readAsset(const fmt::OcPlacedComponent& r, std::string_view key, u64& id) {
    id = 0;
    if (const std::string* v = r.find(key); v && !v->empty()) {
        id = fnv1a64(std::string_view(slashed(*v)));
        return AssetKey::Ok;
    }
    const std::string hexKey = std::string(key) + "#";
    if (const std::string* v = r.find(hexKey); v && !v->empty()) {
        u64 h = 0;
        const auto res = std::from_chars(v->data(), v->data() + v->size(), h, 16);
        if (res.ec != std::errc{} || res.ptr != v->data() + v->size()) return AssetKey::Bad;
        id = h;
        return AssetKey::Ok;
    }
    return AssetKey::Absent;
}

void putAsset(fmt::OcPlacedComponent& rec, std::string_view key, u64 id, const PathOfIdFn& pathOf) {
    if (id == 0) return;
    const std::string p = pathOf ? pathOf(id) : std::string{};
    if (!p.empty()) rec.kv.emplace_back(std::string(key), p);
    else rec.kv.emplace_back(std::string(key) + "#", hex16(id));
}

void put(fmt::OcPlacedComponent& rec, std::string_view key, std::string value) {
    rec.kv.emplace_back(std::string(key), std::move(value));
}
void putNum(fmt::OcPlacedComponent& rec, std::string_view key, f32 v) { put(rec, key, numStr(static_cast<double>(v))); }

// ---- component ids and requirements ----

u32 typeId(scene::World& w, int ix) {
    switch (ix) {
        case kSkeletalMesh: return scene::kComponentSkeletalMesh;
        case kAnimator: return scene::kComponentAnimator;
        case kRootMotion: return w.componentId("CRootMotion");
        case kMotionMatcher: return w.componentId("CMotionMatcher");
        case kControlRig: return w.componentId("CControlRig");
        case kIkRig: return w.componentId("CIkRig");
#if AVER_MODULE_SYNAPSE_SCENE
        case kBrain: return w.componentId("CSynapseBehavior");
        case kSmartObject: return w.componentId("CSmartObject");
#endif
        case kSoftBody: return scene::kComponentSoftBody;
#if AVER_MODULE_SOFTBODY_SCENE
        case kCloth: return w.componentId("CCloth");
        case kDestructible: return w.componentId("CDestructible");
        case kSoftField: return w.componentId("CSoftField");

        case kJelly: return w.componentId("CSoftVolume");
        case kRope: return w.componentId("CSoftRope");
#endif
        case kMorphTargets: return w.componentId("CMorphTargets");

#if AVER_MODULE_VEHICLE_SCENE
        case kVehicle: return w.componentId("CVehicle");
#endif

        case kParticleEmitter: return scene::kComponentParticleEmitter;
#if AVER_MODULE_HAIR_SCENE
        case kHair: return w.componentId("CHair");
#endif

        case kCameraRig: return w.componentId("CCameraRig");
        case kCollisionShape: return collisionShapeType(w);
        default: return 0;
    }
}

bool hasSkeleton(scene::World& w, scene::Entity e) {
    const auto* sm = w.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
    return sm && sm->skeleton != 0;
}

// Everything a record needs from the entity before any component is touched, so a refusal never leaves half of one.
bool requirementsMet(scene::World& w, scene::Entity e, int ix, const fmt::OcWorldPlacement* placement, std::string& why) {
    if (!w.valid(e)) { why = "the entity is gone"; return false; }
    if (!w.hasComponent(e, scene::kComponentMeshRenderer)) { why = "the entity has no mesh"; return false; }
    switch (ix) {
        case kSkeletalMesh:
            // CAnimator means an object clip when there is no skeleton: the two meanings cannot share an entity.
            if ((placement && !placement->animClip.empty()) ||
                (w.hasComponent(e, scene::kComponentAnimator) && !w.hasComponent(e, scene::kComponentSkeletalMesh))) {
                why = "the placement already plays an object animation";
                return false;
            }
            break;
        case kAnimator: case kRootMotion: case kMotionMatcher: case kControlRig: case kIkRig:
            if (!hasSkeleton(w, e)) { why = "needs a SkeletalMesh with a skeleton"; return false; }
            break;
        case kJelly: case kRope: {
            // One body per entity root: a soft shape cannot share an entity with a Cloth, a Destructible or the other shape.
            for (const int other : {kCloth, kDestructible, ix == kJelly ? kRope : kJelly}) {
                const u32 oid = typeId(w, other);
                if (oid != 0 && w.hasComponent(e, oid)) { why = std::string("the entity is already ") + std::string(kPlacedComponentTypes[other]); return false; }
            }
            break;
        }
        case kCloth: case kDestructible: {
            // One body per entity root: a cloth and a destructible cannot share an entity.
            const u32 other = typeId(w, ix == kCloth ? kDestructible : kCloth);
            if (other != 0 && w.hasComponent(e, other)) { why = ix == kCloth ? "the entity is already Destructible" : "the entity is already Cloth"; return false; }

            for (const int shape : {kJelly, kRope}) {
                const u32 sid = typeId(w, shape);
                if (sid != 0 && w.hasComponent(e, sid)) { why = std::string("the entity is already ") + std::string(kPlacedComponentTypes[shape]); return false; }
            }
            if (ix == kDestructible) {
                // A vehicle's chassis is one body: a Destructible belongs on a child (the spec's DAMAGE shell).
                const u32 veh = typeId(w, kVehicle);
                if (veh != 0 && w.hasComponent(e, veh)) { why = "the entity is already a Vehicle"; return false; }
            }
            break;
        }
        case kHair:
            // A rigid head works (the scalp then follows one bone); a skeleton is not required. One hair per entity root; it does not mix with a cloth or a destructible body.
            if (typeId(w, kCloth) != 0 && w.hasComponent(e, typeId(w, kCloth))) { why = "the entity is already Cloth"; return false; }
            if (typeId(w, kDestructible) != 0 && w.hasComponent(e, typeId(w, kDestructible))) { why = "the entity is already Destructible"; return false; }
            break;
        case kVehicle: {
            // The fights VehiclePhysicsSystem::buildEntry and GameLevel::beginVehicles refuse: a rigid body, an object clip, a traffic `vehicle` token.
            if (w.hasComponent(e, scene::kComponentRigidBody)) { why = "the entity is a Rigid Body: a Vehicle is its own body"; return false; }
            if (placement && !placement->vehiclePreset.empty()) { why = "the placement is a traffic car (vehicle token)"; return false; }
            if ((placement && !placement->animClip.empty()) || (w.hasComponent(e, scene::kComponentAnimator) && !w.hasComponent(e, scene::kComponentSkeletalMesh))) {
                why = "the placement plays an object animation";
                return false;
            }
            const u32 dest = typeId(w, kDestructible);
            if (dest != 0 && w.hasComponent(e, dest)) { why = "the entity is already Destructible"; return false; }
            break;
        }
        default: break;
    }
    return true;
}

// ---- apply, one function per type; each validates, then attaches through the owning system, then writes every authored field ----

bool applySkeletalMesh(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    u64 skeleton = 0;
    if (readAsset(r, "skeleton", skeleton) == AssetKey::Bad) { why = "malformed skeleton#"; return false; }
    auto* c = w.component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
    if (!c) {
        c = static_cast<scene::CSkeletalMesh*>(w.addComponent(e, scene::kComponentSkeletalMesh));
        if (!c) { why = "the world refused the component"; return false; }
        *c = scene::CSkeletalMesh{};
    }
    if (c->skeleton != skeleton) { c->skeleton = skeleton; c->boneCount = 0; c->dirty = 1; }
    return true;
}

bool applyAnimator(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    u64 clip = 0;
    if (readAsset(r, "clip", clip) == AssetKey::Bad) { why = "malformed clip#"; return false; }
    auto* c = w.component<scene::CAnimator>(e, scene::kComponentAnimator);
    if (!c) {
        c = static_cast<scene::CAnimator*>(w.addComponent(e, scene::kComponentAnimator));
        if (!c) { why = "the world refused the component"; return false; }
        *c = scene::CAnimator{};
    }
    c->clip = clip;
    c->speed = numKey(r, "speed", 1.0f);
    c->blendWeight = numKey(r, "weight", 1.0f);
    c->flags = (boolKey(r, "once", false) ? scene::kAnimatorOnce : 0u) | (boolKey(r, "paused", false) ? scene::kAnimatorPaused : 0u);
    return true;   // `time` is not authored: a new animator starts at 0, an existing one keeps its clock
}

bool applyRootMotion(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    const u32 type = typeId(w, kRootMotion);
    if (type == 0) { why = "CRootMotion is not registered"; return false; }
    const std::string modeText = r.find("mode") ? lowered(*r.find("mode")) : std::string("apply");
    i32 mode = anim::kRootMotionApply;
    if (modeText == "apply") mode = anim::kRootMotionApply;
    else if (modeText == "ignore") mode = anim::kRootMotionIgnore;
    else if (modeText == "accumulate") mode = anim::kRootMotionAccumulate;
    else if (modeText == "none") mode = anim::kRootMotionNone;   // not in the menu; lets a raw-attached component capture losslessly
    else { why = "unknown mode '" + modeText + "'"; return false; }

    u32 axes = anim::kRootAxesDefault;
    if (const std::string* v = r.find("axes")) {
        axes = (listHas(*v, "x") ? anim::kRootAxisX : 0u) | (listHas(*v, "y") ? anim::kRootAxisY : 0u) |
               (listHas(*v, "z") ? anim::kRootAxisZ : 0u) | (listHas(*v, "yaw") ? anim::kRootAxisYaw : 0u);
        if (axes == 0) axes = anim::kRootAxesDefault;
    }
    u32 sources = anim::kRootSrcGraph | anim::kRootSrcMontage;
    if (const std::string* v = r.find("sources")) {
        sources = (listHas(*v, "graph") ? anim::kRootSrcGraph : 0u) | (listHas(*v, "montage") ? anim::kRootSrcMontage : 0u);
        if (sources == 0) sources = anim::kRootSrcGraph | anim::kRootSrcMontage;
    }
    f32 scale = numKey(r, "scale", 1.0f);
    if (scale == 0.0f) scale = 1.0f;

    auto* c = w.component<anim::CRootMotion>(e, type);
    if (!c) {
        c = anim::AnimSystem::attach(w, e, mode, axes, scale);
        if (!c) { why = "the world refused the component"; return false; }
    }
    c->mode = mode;
    c->axes = axes;
    c->sources = sources;
    c->scale = scale;
    c->flags = boolKey(r, "external", false) ? anim::kRootMotionExternal : 0u;
    return true;
}

bool applyMotionMatcher(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    const u32 type = typeId(w, kMotionMatcher);
    if (type == 0) { why = "CMotionMatcher is not registered"; return false; }
    u64 database = 0;
    if (readAsset(r, "database", database) == AssetKey::Bad) { why = "malformed database#"; return false; }
    auto* c = w.component<anim::CMotionMatcher>(e, type);
    if (!c) {
        c = anim::MotionMatchSystem::attach(w, e, 0);
        if (!c) { why = "the world refused the component"; return false; }
    }
    const std::string input = r.find("input") ? lowered(*r.find("input")) : std::string("script");
    c->inputSource = input == "velocity" ? anim::kMotionInputEntityVelocity : anim::kMotionInputScript;
    // kMotionForceSearch is a script's one-shot request, not authored state: leave it alone.
    const bool faceYaw = r.find("facing") && lowered(*r.find("facing")) == "yaw";
    c->flags = (c->flags & ~(anim::kMotionPaused | anim::kMotionFaceYaw)) | (boolKey(r, "paused", false) ? anim::kMotionPaused : 0u) |
               (faceYaw ? anim::kMotionFaceYaw : 0u);
    c->playRate = numKey(r, "playrate", 1.0f);
    c->searchInterval = numKey(r, "interval", 0.0f);
    c->velHalfLife = numKey(r, "velhalflife", 0.0f);
    c->turnHalfLife = numKey(r, "turnhalflife", 0.0f);
    c->database = database;   // LAST: a database set before the rest would be searched with a half-written config (MotionMatching.cs)
    return true;
}

bool applyControlRig(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    const u32 type = typeId(w, kControlRig);
    if (type == 0) { why = "CControlRig is not registered"; return false; }
    u64 rig = 0;
    if (readAsset(r, "rig", rig) == AssetKey::Bad) { why = "malformed rig#"; return false; }
    auto* c = w.component<anim::CControlRig>(e, type);
    if (!c) {
        c = anim::ControlRigSystem::attach(w, e, 0, 1.0f);
        if (!c) { why = "the world refused the component"; return false; }
    }
    c->rig = rig;
    c->weight = numKey(r, "weight", 1.0f);
    return true;
}

bool applyIkRig(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    const u32 type = typeId(w, kIkRig);
    if (type == 0) { why = "CIkRig is not registered"; return false; }
    u64 asset = 0;
    if (readAsset(r, "asset", asset) == AssetKey::Bad) { why = "malformed asset#"; return false; }
    auto* c = w.component<anim::CIkRig>(e, type);
    if (!c) {
        c = anim::ControlRigSystem::attachIk(w, e, 0, 1.0f);
        if (!c) { why = "the world refused the component"; return false; }
    }
    c->asset = asset;
    c->weight = numKey(r, "weight", 1.0f);
    constexpr u32 kAuthored = anim::kIkRigFeetOff | anim::kIkRigGoalsOff | anim::kIkRigBodyOff;
    c->flags = (c->flags & ~kAuthored) | (boolKey(r, "feet", true) ? 0u : anim::kIkRigFeetOff) |
               (boolKey(r, "goals", true) ? 0u : anim::kIkRigGoalsOff) | (boolKey(r, "body", true) ? 0u : anim::kIkRigBodyOff);
    return true;
}

#if AVER_MODULE_SYNAPSE_SCENE
bool applyBrain(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    synapse::BtSystem& bt = synapse::btSystem();
    if (bt.componentType() == 0) { why = "CSynapseBehavior is not registered"; return false; }
    if (r.find("asset#")) { why = "a brain needs a path (no hex form)"; return false; }
    const std::string path = slashed(textKey(r, "asset"));
    if (!path.empty()) {
        if (!bt.setBrain(w, e, path)) { why = "could not load '" + path + "'"; return false; }
    } else {
        auto* have = w.component<synapse::CSynapseBehavior>(e, bt.componentType());
        if (!have) {
            if (!bt.attach(w, e)) { why = "the world refused the component"; return false; }
        } else if (have->treeAssetId != 0) {
            bt.brainStop(e);
            have->treeAssetId = 0;
            have->lastStatus = 0;
        }
    }
    const std::string team = textKey(r, "team");
    if (!team.empty()) bt.setTeam(w, e, team);
    else if (auto* b = w.component<synapse::CSynapseBehavior>(e, bt.componentType())) b->teamId = 0;
    return true;
}

// Through the scene string-field ABI, never the Synapse DLL: Game.Core does not link it (the DLL's SmartObjectSystem owns the registry).
bool applySmartObject(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    const u32 type = typeId(w, kSmartObject);
    const u32 assetField = w.fieldId("CSmartObject.asset");
    if (type == 0 || assetField == 0) { why = "CSmartObject is not registered"; return false; }
    if (r.find("asset#")) { why = "a smart object needs a path (no hex form)"; return false; }
    const std::string path = slashed(textKey(r, "asset"));
    auto* c = w.component<synapse::CSmartObject>(e, type);
    const bool fresh = c == nullptr;
    if (fresh) {
        c = static_cast<synapse::CSmartObject*>(w.addComponent(e, type));
        if (!c) { why = "the world refused the component"; return false; }
        *c = synapse::CSmartObject{};
    }
    // set_str reuses the slot the field already owns, so an update never grows the string pool.
    if (!aver_scene_set_str(static_cast<int32_t>(e), static_cast<int32_t>(assetField), path.c_str())) {
        if (fresh) w.removeComponent(e, type);
        why = "the string field was refused";
        return false;
    }
    c->enabled = boolKey(r, "enabled", true) ? 1 : 0;
    return true;
}
#endif

bool applySoftBody(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    auto* c = w.component<scene::CSoftBody>(e, scene::kComponentSoftBody);
    if (!c) {
        c = static_cast<scene::CSoftBody*>(w.addComponent(e, scene::kComponentSoftBody));
        if (!c) { why = "the world refused the component"; return false; }
        *c = scene::CSoftBody{};
    }
    c->maxDistanceCm = numKey(r, "maxdist", 0.0f);
    c->compliance = numKey(r, "compliance", 0.0f);
    c->flags = (c->flags & ~scene::kSoftBodyDisabled) | (boolKey(r, "simulated", true) ? 0u : scene::kSoftBodyDisabled);
    return true;   // `body` is the physics handle: process-local, never authored
}

#if AVER_MODULE_SOFTBODY_SCENE
// COMPONENT Cloth / Destructible (docs/SOFTBODY.md). Runtime state (damage, pieces) is never authored: a loaded level starts intact.
bool softPreset(const fmt::OcPlacedComponent& r, softbody::SoftPreset def, softbody::SoftPreset& out, std::string& why) {
    out = def;
    if (const std::string* v = r.find("preset"))
        if (!softbody::presetFromKey(lowered(*v), out)) { why = "unknown preset '" + *v + "'"; return false; }
    return true;
}

bool applyCloth(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    const u32 type = typeId(w, kCloth);
    if (type == 0) { why = "CCloth is not registered"; return false; }
    u64 asset = 0;
    if (readAsset(r, "asset", asset) == AssetKey::Bad) { why = "malformed asset#"; return false; }
    softbody::SoftPreset preset;
    if (!softPreset(r, softbody::SoftPreset::ClothLight, preset, why)) return false;
    auto* c = w.component<softbody::CCloth>(e, type);
    if (!c) {
        c = softbody::SoftBodySystem::attachCloth(w, e, preset, asset);
        if (!c) { why = "the world refused the component"; return false; }
    }
    c->asset = asset;
    c->preset = static_cast<u32>(preset);
    c->windScale = numKey(r, "wind", 1.0f);
    c->stiffnessScale = numKey(r, "stiffness", 1.0f);
    c->reachScale = numKey(r, "reach", 1.0f);
    c->freezeDistanceCm = numKey(r, "freeze", 0.0f);
    // kClothRelease is a one-shot request from a script, not authored state: leave it alone.
    c->flags = (c->flags & softbody::kClothRelease) | (boolKey(r, "tearable", false) ? softbody::kClothTearable : 0u) |
               (boolKey(r, "world", true) ? 0u : softbody::kClothNoWorld) | (boolKey(r, "proxy", true) ? 0u : softbody::kClothNoCharacterProxy) |
               (boolKey(r, "windon", true) ? 0u : softbody::kClothNoWind);
    return true;
}

bool applyDestructible(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    const u32 type = typeId(w, kDestructible);
    if (type == 0) { why = "CDestructible is not registered"; return false; }
    u64 asset = 0;
    if (readAsset(r, "asset", asset) == AssetKey::Bad) { why = "malformed asset#"; return false; }
    softbody::SoftPreset preset;
    if (!softPreset(r, softbody::SoftPreset::Metal, preset, why)) return false;
    auto* c = w.component<softbody::CDestructible>(e, type);
    if (!c) {
        c = softbody::SoftBodySystem::attachDestructible(w, e, preset, asset);
        if (!c) { why = "the world refused the component"; return false; }
    }
    c->asset = asset;
    c->preset = static_cast<u32>(preset);
    c->toughnessScale = numKey(r, "toughness", 1.0f);
    c->impactScale = numKey(r, "impact", 1.0f);
    c->debrisLifetimeS = numKey(r, "lifetime", 0.0f);
    c->maxPieces = static_cast<u32>(std::max(0.0f, numKey(r, "maxpieces", 0.0f)));
    c->flags = (boolKey(r, "indestructible", false) ? softbody::kDestructIndestructible : 0u) |
               (boolKey(r, "nodebris", false) ? softbody::kDestructNoDebris : 0u);
    return true;
}

// "x,y,z" -> out; false (out untouched) when absent or malformed.
bool vec3Key(const fmt::OcPlacedComponent& r, std::string_view key, f32 out[3]) {
    const std::string* v = r.find(key);
    if (!v) return false;
    f32 t[3];
    usize at = 0;
    for (int i = 0; i < 3; ++i) {
        const usize comma = i < 2 ? v->find(',', at) : v->size();
        if (comma == std::string::npos) return false;
        double d = 0.0;
        if (!parseNum(v->substr(at, comma - at), d)) return false;
        t[i] = static_cast<f32>(d);
        at = comma + 1;
    }
    out[0] = t[0]; out[1] = t[1]; out[2] = t[2];
    return true;
}

// COMPONENT SoftField (docs/SOFTBODY.md): kind shape extent=x,y,z strength energy falloff dir=x,y,z turbulence targets once off.
bool applySoftField(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    const u32 type = typeId(w, kSoftField);
    if (type == 0) { why = "CSoftField is not registered"; return false; }
    softbody::FieldKind kind = softbody::FieldKind::RadialForce;
    if (const std::string* k = r.find("kind"); k && !softbody::fieldKindFromKey(*k, kind)) { why = "unknown field kind"; return false; }
    softbody::FieldShape shape = softbody::FieldShape::Sphere;
    if (const std::string* k = r.find("shape"); k && !softbody::fieldShapeFromKey(*k, shape)) { why = "unknown field shape"; return false; }
    u32 targets = 0;
    if (const std::string* k = r.find("targets"); k && !softbody::fieldTargetsFromText(*k, targets)) { why = "unknown field targets"; return false; }
    auto* c = w.component<softbody::CSoftField>(e, type);
    if (!c) {
        c = softbody::SoftBodySystem::attachSoftField(w, e);
        if (!c) { why = "the world refused the component"; return false; }
    }
    c->kind = static_cast<u32>(kind);
    c->shape = static_cast<u32>(shape);
    c->targets = targets;
    f32 ext[3] = {0, 0, 0}, dir[3] = {0, 0, 0};
    vec3Key(r, "extent", ext);
    vec3Key(r, "dir", dir);
    c->extentX = ext[0]; c->extentY = ext[1]; c->extentZ = ext[2];
    c->dirX = dir[0]; c->dirY = dir[1]; c->dirZ = dir[2];
    c->strength = numKey(r, "strength", 0.0f);
    c->energyJPerS = numKey(r, "energy", 0.0f);
    c->falloff = numKey(r, "falloff", 0.0f);
    c->turbulence = numKey(r, "turbulence", 0.0f);
    c->flags = (boolKey(r, "once", false) ? softbody::kFieldOnce : 0u) | (boolKey(r, "off", false) ? softbody::kFieldDisabled : 0u);
    return true;
}
#endif

#if AVER_MODULE_SOFTBODY_SCENE
// COMPONENT Jelly (docs/SOFTBODY.md section 12): asset stiffness=1 cell=0 gravity=1 damping=1 world=1 gravityon=1. The lattice and the OUT fields are runtime state, never authored.
bool applyJelly(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    const u32 type = typeId(w, kJelly);
    if (type == 0) { why = "CSoftVolume is not registered"; return false; }
    u64 asset = 0;
    if (readAsset(r, "asset", asset) == AssetKey::Bad) { why = "malformed asset#"; return false; }
    auto* c = w.component<softbody::CSoftVolume>(e, type);
    if (!c) {
        c = softbody::SoftShapeSystem::attachJelly(w, e, softbody::SoftPreset::Jelly, asset);
        if (!c) { why = "the world refused the component"; return false; }
    }
    c->asset = asset;
    c->stiffnessScale = numKey(r, "stiffness", 1.0f);
    c->cellCm = std::max(0.0f, numKey(r, "cell", 0.0f));
    c->gravityScale = numKey(r, "gravity", 1.0f);
    c->dampingScale = numKey(r, "damping", 1.0f);
    c->flags = (boolKey(r, "world", true) ? 0u : softbody::kVolNoWorld) | (boolKey(r, "gravityon", true) ? 0u : softbody::kVolNoGravity);
    return true;
}

// COMPONENT Rope: asset start=x,y,z end=x,y,z (WORLD cm; absent: the placement's position and 250 cm ahead of it) length=0 radius=0 world=1 startpin=1 endpin=1.
// An end anchored to an entity is script state a level cannot name: apply always writes world points, capture writes an anchored end's current world point.
bool applyRope(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    const u32 type = typeId(w, kRope);
    if (type == 0) { why = "CSoftRope is not registered"; return false; }
    u64 asset = 0;
    if (readAsset(r, "asset", asset) == AssetKey::Bad) { why = "malformed asset#"; return false; }
    const Mat4& host = w.worldMatrix(e);
    f32 start[3] = {host.m[3][0], host.m[3][1], host.m[3][2]};
    f32 end[3] = {start[0] + 250.0f, start[1], start[2]};
    vec3Key(r, "start", start);
    vec3Key(r, "end", end);
    const f32 length = std::max(0.0f, numKey(r, "length", 0.0f)), radius = std::max(0.0f, numKey(r, "radius", 0.0f));
    auto* c = w.component<softbody::CSoftRope>(e, type);
    if (!c) {
        c = softbody::SoftShapeSystem::attachRope(w, e, Vec3{start[0], start[1], start[2]}, Vec3{end[0], end[1], end[2]}, asset, length, radius);
        if (!c) { why = "the world refused the component"; return false; }
    }
    c->asset = asset;
    c->startEntity = 0;
    c->endEntity = 0;
    for (int i = 0; i < 3; ++i) { c->startOffset[i] = start[i]; c->endOffset[i] = end[i]; }
    c->lengthCm = length;
    c->radiusCm = radius;
    // kRopeTail is runtime state (a cut piece): leave it alone.
    c->flags = (c->flags & softbody::kRopeTail) | (boolKey(r, "world", true) ? 0u : softbody::kRopeNoWorld) |
               (boolKey(r, "startpin", true) ? 0u : softbody::kRopeNoStartPin) | (boolKey(r, "endpin", true) ? 0u : softbody::kRopeNoEndPin);
    return true;
}
#endif
// COMPONENT MorphTargets (docs/ANIM_MORPH_FACE.md): rig=<.ocface> weight=1 inherit=1 off=1 ignoreanim=1. Weights set from script,
// the sequencer or lip-sync are runtime state and never authored.
bool applyMorphTargets(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    const u32 type = typeId(w, kMorphTargets);
    if (type == 0) { why = "CMorphTargets is not registered"; return false; }
    u64 rig = 0;
    if (readAsset(r, "rig", rig) == AssetKey::Bad) { why = "malformed rig#"; return false; }
    auto* c = w.component<anim::CMorphTargets>(e, type);
    if (!c) {
        c = anim::MorphSystem::attach(w, e, 0, 0);
        if (!c) { why = "the world refused the component"; return false; }
    }
    c->faceRig = rig;
    c->weight = numKey(r, "weight", 1.0f);
    c->flags = (boolKey(r, "inherit", false) ? anim::kMorphInheritParent : 0u) | (boolKey(r, "off", false) ? anim::kMorphOff : 0u) |
               (boolKey(r, "ignoreanim", false) ? anim::kMorphIgnoreAnim : 0u);
    return true;
}

#if AVER_MODULE_CAMERA
// ---- CameraRig: apply and capture share one key table, in canonical order (docs/CAMERAS.md; sandbox/src/CameraRigDetailsModel.hpp restates it) ----

enum class CamSlot : u8 { Float, Vec3, Curve, Mask };
struct CamKey { const char* key; CamSlot kind; usize offset; };
#define AVER_CAMKEY(k, kind, member) CamKey{k, CamSlot::kind, offsetof(camera::CCameraRig, member)}
constexpr CamKey kCamKeys[] = {
    AVER_CAMKEY("arm", Float, armLengthCm), AVER_CAMKEY("minarm", Float, minArmCm), AVER_CAMKEY("probe", Float, probeRadiusCm),
    AVER_CAMKEY("recover", Float, recoverHalfLifeS), AVER_CAMKEY("socket", Vec3, socketX), AVER_CAMKEY("pivot", Vec3, pivotX),
    AVER_CAMKEY("lag", Float, posLagSpeed), AVER_CAMKEY("lagmax", Float, lagMaxDistCm), AVER_CAMKEY("rotlag", Float, rotLagSpeed),
    AVER_CAMKEY("rotlagmax", Float, rotLagMaxDeg), AVER_CAMKEY("lagstep", Float, lagMaxStepS), AVER_CAMKEY("ahead", Float, lookAheadS),
    AVER_CAMKEY("aheadmax", Float, lookAheadMaxCm), AVER_CAMKEY("pitchmin", Float, pitchMinDeg), AVER_CAMKEY("pitchmax", Float, pitchMaxDeg),
    AVER_CAMKEY("yaw", Float, yawDeg), AVER_CAMKEY("pitch", Float, pitchDeg), AVER_CAMKEY("recenter", Float, recenterDelayS),
    AVER_CAMKEY("blend", Float, blendTimeS), AVER_CAMKEY("curve", Curve, blendCurve), AVER_CAMKEY("fovkick", Float, fovKickDeg),
    AVER_CAMKEY("fovkickspeed", Float, fovKickSpeedCmS), AVER_CAMKEY("fadestart", Float, fadeStartCm), AVER_CAMKEY("fadeend", Float, fadeEndCm),
    AVER_CAMKEY("bob", Float, bobAmount), AVER_CAMKEY("seatyaw", Float, seatYawLimitDeg), AVER_CAMKEY("seatpitch", Float, seatPitchLimitDeg),
    AVER_CAMKEY("mask", Mask, collisionMask),
};
#undef AVER_CAMKEY
// A flag key: `1` (the default) means the bit is CLEAR for the negative-sense flags, `0` for the positive ones.
struct CamFlag { const char* key; u32 bit; bool negative; };
constexpr CamFlag kCamFlags[] = {
    {"collision", camera::kRigNoCollision, true}, {"inheritpitch", camera::kRigNoInheritPitch, true}, {"inherityaw", camera::kRigNoInheritYaw, true},
    {"inheritroll", camera::kRigNoInheritRoll, true}, {"fade", camera::kRigNoFade, true}, {"lookat", camera::kRigLookAtTarget, false},
    {"disabled", camera::kRigDisabled, false},
};
constexpr const char* kCamPresetKeys[] = {"chase", "vehiclechase", "orbit", "firstperson", "seat", "topdown", "fixed"};
constexpr const char* kCamModeKeys[] = {"chase", "orbit", "firstperson", "seat", "topdown", "fixed"};
constexpr const char* kCamCurveKeys[] = {"easeinout", "linear", "easein", "easeout"};

template <usize N>
int camWord(const char* const (&words)[N], std::string_view w) {
    const std::string l = lowered(w);
    for (usize i = 0; i < N; ++i)
        if (l == words[i]) return static_cast<int>(i);
    return -1;
}
f32 camFloatAt(const camera::CCameraRig& r, usize off) { f32 v; std::memcpy(&v, reinterpret_cast<const unsigned char*>(&r) + off, sizeof v); return v; }
void camSetFloat(camera::CCameraRig& r, usize off, f32 v) { std::memcpy(reinterpret_cast<unsigned char*>(&r) + off, &v, sizeof v); }

bool applyCameraRig(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    const u32 type = typeId(w, kCameraRig);
    if (type == 0) { why = "CCameraRig is not registered"; return false; }
    // Every word is checked before the component is touched, so a refusal leaves nothing half-written.
    int preset = 0;
    if (const std::string* v = r.find("preset")) {
        preset = camWord(kCamPresetKeys, *v);
        if (preset < 0) { why = "unknown preset '" + *v + "'"; return false; }
    }
    camera::CCameraRig rig = camera::presetRig(static_cast<camera::CameraPreset>(preset));
    if (const std::string* v = r.find("mode")) {
        const int m = camWord(kCamModeKeys, *v);
        if (m < 0) { why = "unknown mode '" + *v + "'"; return false; }
        rig.mode = static_cast<u32>(m);
    }
    for (const CamKey& k : kCamKeys) {
        const std::string* v = r.find(k.key);
        if (!v) continue;
        switch (k.kind) {
            case CamSlot::Float: camSetFloat(rig, k.offset, numKey(r, k.key, camFloatAt(rig, k.offset))); break;
            case CamSlot::Vec3: {   // x,y,z; an axis that does not parse keeps the preset's value
                usize at = 0;
                for (int axis = 0; axis < 3 && at <= v->size(); ++axis) {
                    const usize comma = v->find(',', at);
                    double d = 0.0;
                    if (parseNum(std::string_view(*v).substr(at, comma == std::string::npos ? std::string::npos : comma - at), d))
                        camSetFloat(rig, k.offset + static_cast<usize>(axis) * sizeof(f32), static_cast<f32>(d));
                    if (comma == std::string::npos) break;
                    at = comma + 1;
                }
                break;
            }
            case CamSlot::Curve: {
                const int c = camWord(kCamCurveKeys, *v);
                if (c < 0) { why = "unknown curve '" + *v + "'"; return false; }
                rig.blendCurve = static_cast<u32>(c);
                break;
            }
            case CamSlot::Mask: {
                u32 m = 0;
                const auto res = std::from_chars(v->data(), v->data() + v->size(), m, 16);
                if (res.ec == std::errc{} && res.ptr == v->data() + v->size()) rig.collisionMask = m;
                break;
            }
        }
    }
    for (const CamFlag& f : kCamFlags) {
        if (!r.find(f.key)) continue;
        const bool on = boolKey(r, f.key, f.negative);
        const bool setBit = f.negative ? !on : on;
        rig.flags = setBit ? (rig.flags | f.bit) : (rig.flags & ~f.bit);
    }
    // `target=self` (the default) follows the rig's own entity; `parent` its parent. Anything else is runtime.
    rig.target = e;
    if (const std::string* v = r.find("target"); v && lowered(*v) == "parent") {
        const scene::Entity p = w.parent(e);
        if (p != scene::kInvalidEntity) rig.target = p;
    }
    const auto* prev = w.component<camera::CCameraRig>(e, type);
    const i32 body = prev ? prev->ignoreBody : 0;   // process-local, not authored: an update keeps it
    camera::CCameraRig* c = camera::CameraRigSystem::attach(w, e, static_cast<camera::CameraPreset>(preset), rig.target);
    if (!c) { why = "the world refused the component"; return false; }
    *c = rig;
    c->ignoreBody = body;
    return true;
}

void captureCameraRig(scene::World& w, scene::Entity e, const camera::CCameraRig& c, fmt::OcPlacedComponent& rec) {
    const int preset = c.preset < camera::kCameraPresetCount ? static_cast<int>(c.preset) : 0;
    const camera::CCameraRig base = camera::presetRig(static_cast<camera::CameraPreset>(preset));
    put(rec, "preset", kCamPresetKeys[preset]);
    if (c.mode != base.mode) put(rec, "mode", kCamModeKeys[c.mode < camera::kCameraModeCount ? c.mode : 0u]);
    if (c.target != 0 && c.target != e && c.target == w.parent(e)) put(rec, "target", "parent");
    for (const CamKey& k : kCamKeys) {
        switch (k.kind) {
            case CamSlot::Float:
                if (camFloatAt(c, k.offset) != camFloatAt(base, k.offset)) putNum(rec, k.key, camFloatAt(c, k.offset));
                break;
            case CamSlot::Vec3: {
                bool differs = false;
                for (int axis = 0; axis < 3; ++axis) {
                    const usize off = k.offset + static_cast<usize>(axis) * sizeof(f32);
                    differs = differs || camFloatAt(c, off) != camFloatAt(base, off);
                }
                if (!differs) break;
                std::string v;
                for (int axis = 0; axis < 3; ++axis) {
                    if (axis) v += ',';
                    v += numStr(static_cast<double>(camFloatAt(c, k.offset + static_cast<usize>(axis) * sizeof(f32))));
                }
                put(rec, k.key, v);
                break;
            }
            case CamSlot::Curve:
                if (c.blendCurve != base.blendCurve) put(rec, k.key, kCamCurveKeys[c.blendCurve < 4u ? c.blendCurve : 0u]);
                break;
            case CamSlot::Mask:
                if (c.collisionMask != base.collisionMask) { char b[16]; std::snprintf(b, sizeof b, "%x", c.collisionMask); put(rec, k.key, b); }
                break;
        }
    }
    for (const CamFlag& f : kCamFlags) {
        const bool isSet = (c.flags & f.bit) != 0, baseSet = (base.flags & f.bit) != 0;
        if (isSet != baseSet) put(rec, f.key, (f.negative ? !isSet : isSet) ? "1" : "0");
    }
}
#endif
// COMPONENT ParticleEmitter (docs/PARTICLES.md): effect=<.ocparticle path> sim=auto|cpu|gpu playing=1 autodestroy=0 seed=0. The clock,
// the emit accumulator and a seed the system assigned itself are runtime state and are never authored. An absent effect is allowed
// (the emitter is inert until one is picked), the way an Animator without a clip is.
bool applyParticleEmitter(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    u64 effect = 0;
    if (readAsset(r, "effect", effect) == AssetKey::Bad) { why = "malformed effect#"; return false; }
    auto* c = w.component<scene::CParticleEmitter>(e, scene::kComponentParticleEmitter);
    if (!c) {
        c = static_cast<scene::CParticleEmitter*>(w.addComponent(e, scene::kComponentParticleEmitter));
        if (!c) { why = "the world refused the component"; return false; }
        *c = scene::CParticleEmitter{};
    }
    const std::string sim = lowered(textKey(r, "sim"));
    double seed = 0.0;
    if (const std::string* sv = r.find("seed")) parseNum(*sv, seed);
    c->effect = effect;
    c->age = 0.0f;
    c->emitAccum = 0.0f;
    c->seed = (seed >= 1.0 && seed <= 4294967295.0) ? static_cast<u32>(seed) : 0u;
    c->flags = (boolKey(r, "playing", true) ? 0u : scene::kParticleEmitterStopped) |
               (boolKey(r, "autodestroy", false) ? scene::kParticleEmitterAutoDestroy : 0u) |
               (sim == "cpu" ? scene::kParticleEmitterForceCpu : 0u) | (sim == "gpu" ? scene::kParticleEmitterForceGpu : 0u);
    return true;
}
#if AVER_MODULE_VEHICLE_SCENE
// COMPONENT Vehicle (docs/VEHICLES.md): asset=<.ocvehicle> preset=car power= grip= brake= damage= noactions= noaudio= noeffects= nowheels= indestructible=.
// Runtime state (driver, health, gear, damage taken) is never authored: a loaded level starts intact and unoccupied. A zero scale reads as 1.
bool applyVehicle(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    const u32 type = typeId(w, kVehicle);
    if (type == 0) { why = "CVehicle is not registered"; return false; }
    u64 asset = 0;
    if (readAsset(r, "asset", asset) == AssetKey::Bad) { why = "malformed asset#"; return false; }
    int preset = 0;
    if (const std::string* v = r.find("preset")) {
        preset = vehicle::presetIndex(lowered(*v));
        if (preset < 0) { why = "unknown preset '" + *v + "'"; return false; }
    }
    auto* c = w.component<vehicle::CVehicle>(e, type);
    if (!c) {
        c = vehicle::VehiclePhysicsSystem::attach(w, e, asset, static_cast<u32>(preset));
        if (!c) { why = "the world refused the component"; return false; }
    }
    c->asset = asset;
    c->preset = static_cast<u32>(preset);
    c->powerScale = numKey(r, "power", 1.0f);
    c->gripScale = numKey(r, "grip", 1.0f);
    c->brakeScale = numKey(r, "brake", 1.0f);
    c->damageScale = numKey(r, "damage", 1.0f);
    // kVehicleFlipRequest / kVehicleRepairRequest are one-shot requests from a script, not authored state: leave them alone.
    c->flags = (c->flags & (vehicle::kVehicleFlipRequest | vehicle::kVehicleRepairRequest)) |
               (boolKey(r, "noactions", false) ? vehicle::kVehicleNoActions : 0u) | (boolKey(r, "noaudio", false) ? vehicle::kVehicleNoAudio : 0u) |
               (boolKey(r, "noeffects", false) ? vehicle::kVehicleNoEffects : 0u) | (boolKey(r, "nowheels", false) ? vehicle::kVehicleNoWheelVisuals : 0u) |
               (boolKey(r, "indestructible", false) ? vehicle::kVehicleIndestructible : 0u);
    return true;
}
#endif
#if AVER_MODULE_HAIR_SCENE
// COMPONENT Hair (docs/HAIR.md): asset=<.ochair> width density lodbias wind stiffness teleport collision shadow windon hidden. The OUT fields are runtime state.
bool applyHair(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, std::string& why) {
    const u32 type = typeId(w, kHair);
    if (type == 0) { why = "CHair is not registered"; return false; }
    u64 asset = 0;
    const AssetKey ak = readAsset(r, "asset", asset);
    if (ak == AssetKey::Bad) { why = "malformed asset#"; return false; }
    if (asset == 0) { why = "asset key is absent"; return false; }
    auto* c = w.component<hair::CHair>(e, type);
    if (!c) {
        c = hair::HairSystem::attachHair(w, e, asset);
        if (!c) { why = "the world refused the component"; return false; }
    }
    c->asset = asset;
    c->widthScale = numKey(r, "width", 1.0f);
    c->densityScale = numKey(r, "density", 1.0f);
    c->lodBias = numKey(r, "lodbias", 0.0f);
    c->windScale = numKey(r, "wind", 1.0f);
    c->stiffnessScale = numKey(r, "stiffness", 1.0f);
    c->teleportCm = std::max(0.0f, numKey(r, "teleport", 0.0f));
    c->flags = (boolKey(r, "collision", true) ? 0u : hair::kHairNoCollision) | (boolKey(r, "shadow", true) ? 0u : hair::kHairNoShadow) |
               (boolKey(r, "windon", true) ? 0u : hair::kHairNoWind) | (boolKey(r, "hidden", false) ? hair::kHairHidden : 0u);
    return true;
}
#endif
bool applyOne(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, const fmt::OcWorldPlacement* placement, std::string& why) {
#if AVER_MODULE_FRAMEWORK
    if (isGroupsComponent(r.type)) {
        if (!w.valid(e)) { why = "no entity"; return false; }
        applyGroupsRecord(static_cast<int32_t>(e), r);
        return true;
    }
#endif
    const int ix = typeIndex(r.type);
    if (ix == kUnknownType) { why = "unknown type"; return false; }
#if !AVER_MODULE_SYNAPSE_SCENE
    if (ix == kBrain || ix == kSmartObject) { why = "unknown to this build"; return false; }
#endif
#if !AVER_MODULE_SOFTBODY_SCENE
    if (ix == kCloth || ix == kDestructible || ix == kSoftField || ix == kJelly || ix == kRope) { why = "unknown to this build"; return false; }
#endif

#if !AVER_MODULE_VEHICLE_SCENE
    if (ix == kVehicle) { why = "unknown to this build"; return false; }
#endif
#if !AVER_MODULE_HAIR_SCENE
    if (ix == kHair) { why = "unknown to this build"; return false; }
#endif

#if !AVER_MODULE_CAMERA
    if (ix == kCameraRig) { why = "unknown to this build"; return false; }
#endif
    if (!requirementsMet(w, e, ix, placement, why)) return false;
    switch (ix) {
        case kSkeletalMesh: return applySkeletalMesh(w, e, r, why);
        case kAnimator: return applyAnimator(w, e, r, why);
        case kRootMotion: return applyRootMotion(w, e, r, why);
        case kMotionMatcher: return applyMotionMatcher(w, e, r, why);
        case kControlRig: return applyControlRig(w, e, r, why);
        case kIkRig: return applyIkRig(w, e, r, why);
#if AVER_MODULE_SYNAPSE_SCENE
        case kBrain: return applyBrain(w, e, r, why);
        case kSmartObject: return applySmartObject(w, e, r, why);
#endif
        case kSoftBody: return applySoftBody(w, e, r, why);
#if AVER_MODULE_SOFTBODY_SCENE
        case kCloth: return applyCloth(w, e, r, why);
        case kDestructible: return applyDestructible(w, e, r, why);
        case kSoftField: return applySoftField(w, e, r, why);

        case kJelly: return applyJelly(w, e, r, why);
        case kRope: return applyRope(w, e, r, why);
#endif
        case kMorphTargets: return applyMorphTargets(w, e, r, why);

#if AVER_MODULE_VEHICLE_SCENE
        case kVehicle: return applyVehicle(w, e, r, why);
#endif

        case kParticleEmitter: return applyParticleEmitter(w, e, r, why);
        case kCollisionShape: return applyCollisionShape(w, e, r, why);
#if AVER_MODULE_HAIR_SCENE
        case kHair: return applyHair(w, e, r, why);
#endif

#if AVER_MODULE_CAMERA
        case kCameraRig: return applyCameraRig(w, e, r, why);
#endif
        default: break;
    }
    why = "unknown type";
    return false;
}

// ---- capture ----

std::optional<fmt::OcPlacedComponent> captureOne(scene::World& w, scene::Entity e, int ix, const PathOfIdFn& pathOf) {
    if (ix == kUnknownType || !w.valid(e)) return std::nullopt;
    const u32 type = typeId(w, ix);
    if (type == 0 || !w.hasComponent(e, type)) return std::nullopt;
    fmt::OcPlacedComponent rec;
    rec.type = std::string(kPlacedComponentTypes[ix]);
    switch (ix) {
        case kSkeletalMesh: {
            const auto* c = w.component<scene::CSkeletalMesh>(e, type);
            putAsset(rec, "skeleton", c->skeleton, pathOf);
            break;
        }
        case kAnimator: {
            const auto* c = w.component<scene::CAnimator>(e, type);
            putAsset(rec, "clip", c->clip, pathOf);
            const f32 speed = c->speed == 0.0f ? 1.0f : c->speed;          // 0 reads as 1
            const f32 weight = c->blendWeight == 0.0f ? 1.0f : c->blendWeight;
            if (speed != 1.0f) putNum(rec, "speed", speed);
            if (weight != 1.0f) putNum(rec, "weight", weight);
            if (c->flags & scene::kAnimatorOnce) put(rec, "once", "1");
            if (c->flags & scene::kAnimatorPaused) put(rec, "paused", "1");
            break;
        }
        case kRootMotion: {
            const auto* c = w.component<anim::CRootMotion>(e, type);
            const char* mode = c->mode == anim::kRootMotionIgnore ? "ignore" : c->mode == anim::kRootMotionAccumulate ? "accumulate"
                             : c->mode == anim::kRootMotionNone ? "none" : "apply";
            put(rec, "mode", mode);
            const u32 axes = c->axes ? c->axes : anim::kRootAxesDefault;
            if (axes != anim::kRootAxesDefault) {
                std::string list;
                const auto add = [&](u32 bit, const char* word) {
                    if (!(axes & bit)) return;
                    if (!list.empty()) list += ',';
                    list += word;
                };
                add(anim::kRootAxisX, "x"); add(anim::kRootAxisY, "y"); add(anim::kRootAxisZ, "z"); add(anim::kRootAxisYaw, "yaw");
                put(rec, "axes", list);
            }
            const u32 sources = c->sources ? c->sources : (anim::kRootSrcGraph | anim::kRootSrcMontage);
            if (sources != (anim::kRootSrcGraph | anim::kRootSrcMontage)) {
                std::string list;
                if (sources & anim::kRootSrcGraph) list += "graph";
                if (sources & anim::kRootSrcMontage) list += list.empty() ? "montage" : ",montage";
                put(rec, "sources", list);
            }
            const f32 scale = c->scale == 0.0f ? 1.0f : c->scale;   // a stored 0 is written as 1
            if (scale != 1.0f) putNum(rec, "scale", scale);
            if (c->flags & anim::kRootMotionExternal) put(rec, "external", "1");
            break;
        }
        case kMotionMatcher: {
            const auto* c = w.component<anim::CMotionMatcher>(e, type);
            putAsset(rec, "database", c->database, pathOf);
            put(rec, "input", c->inputSource == anim::kMotionInputEntityVelocity ? "velocity" : "script");
            if (c->flags & anim::kMotionFaceYaw) put(rec, "facing", "yaw");
            const f32 rate = c->playRate == 0.0f ? 1.0f : c->playRate;
            if (rate != 1.0f) putNum(rec, "playrate", rate);
            if (c->searchInterval > 0.0f) putNum(rec, "interval", c->searchInterval);
            if (c->velHalfLife > 0.0f) putNum(rec, "velhalflife", c->velHalfLife);
            if (c->turnHalfLife > 0.0f) putNum(rec, "turnhalflife", c->turnHalfLife);
            if (c->flags & anim::kMotionPaused) put(rec, "paused", "1");
            break;
        }
        case kControlRig: {
            const auto* c = w.component<anim::CControlRig>(e, type);
            putAsset(rec, "rig", c->rig, pathOf);
            if (c->weight != 1.0f) putNum(rec, "weight", c->weight);   // 0 is meaningful here: the whole rig off
            break;
        }
        case kIkRig: {
            const auto* c = w.component<anim::CIkRig>(e, type);
            putAsset(rec, "asset", c->asset, pathOf);
            if (c->weight != 1.0f) putNum(rec, "weight", c->weight);
            if (c->flags & anim::kIkRigFeetOff) put(rec, "feet", "0");
            if (c->flags & anim::kIkRigGoalsOff) put(rec, "goals", "0");
            if (c->flags & anim::kIkRigBodyOff) put(rec, "body", "0");
            break;
        }
#if AVER_MODULE_SYNAPSE_SCENE
        case kBrain: {
            synapse::BtSystem& bt = synapse::btSystem();
            const auto* c = w.component<synapse::CSynapseBehavior>(e, type);
            if (c->treeAssetId != 0) {
                const synapse::BtRuntimeTree* tree = bt.findTree(c->treeAssetId);
                const synapse::StateTreeRuntime* st = tree ? nullptr : bt.findStateTree(c->treeAssetId);
                const std::string& path = tree ? tree->path : st ? st->path : std::string();
                if (path.empty()) {
                    AVER_WARN("[Level] Brain on entity {} names a tree that is not loaded: not captured", static_cast<u32>(e));
                    return std::nullopt;
                }
                put(rec, "asset", path);
            }
            if (c->teamId != 0) {
                if (const std::string* team = bt.teamName(c->teamId); team && !team->empty()) put(rec, "team", *team);
            }
            break;
        }
        case kSmartObject: {
            const auto* c = w.component<synapse::CSmartObject>(e, type);
            const u32 assetField = w.fieldId("CSmartObject.asset");
            const char* path = assetField ? aver_scene_get_str(static_cast<int32_t>(e), static_cast<int32_t>(assetField)) : "";
            if (path && *path) put(rec, "asset", path);
            if (c->enabled == 0) put(rec, "enabled", "0");
            break;
        }
#endif
        case kSoftBody: {
            const auto* c = w.component<scene::CSoftBody>(e, type);
            if (c->maxDistanceCm != 0.0f) putNum(rec, "maxdist", c->maxDistanceCm);
            if (c->compliance != 0.0f) putNum(rec, "compliance", c->compliance);
            if (c->flags & scene::kSoftBodyDisabled) put(rec, "simulated", "0");
            break;
        }
#if AVER_MODULE_SOFTBODY_SCENE
        case kCloth: {
            const auto* c = w.component<softbody::CCloth>(e, type);
            putAsset(rec, "asset", c->asset, pathOf);
            put(rec, "preset", softbody::presetKey(c->preset < softbody::kSoftSolidPresetCount ? static_cast<softbody::SoftPreset>(c->preset) : softbody::SoftPreset::ClothLight));
            if (c->windScale > 0.0f && c->windScale != 1.0f) putNum(rec, "wind", c->windScale);
            if (c->stiffnessScale > 0.0f && c->stiffnessScale != 1.0f) putNum(rec, "stiffness", c->stiffnessScale);
            if (c->reachScale > 0.0f && c->reachScale != 1.0f) putNum(rec, "reach", c->reachScale);
            if (c->freezeDistanceCm > 0.0f) putNum(rec, "freeze", c->freezeDistanceCm);
            if (c->flags & softbody::kClothTearable) put(rec, "tearable", "1");
            if (c->flags & softbody::kClothNoWorld) put(rec, "world", "0");
            if (c->flags & softbody::kClothNoCharacterProxy) put(rec, "proxy", "0");
            if (c->flags & softbody::kClothNoWind) put(rec, "windon", "0");
            break;
        }
        case kDestructible: {
            const auto* c = w.component<softbody::CDestructible>(e, type);
            putAsset(rec, "asset", c->asset, pathOf);
            put(rec, "preset", softbody::presetKey(c->preset < softbody::kSoftSolidPresetCount ? static_cast<softbody::SoftPreset>(c->preset) : softbody::SoftPreset::Metal));
            if (c->toughnessScale > 0.0f && c->toughnessScale != 1.0f) putNum(rec, "toughness", c->toughnessScale);
            if (c->impactScale > 0.0f && c->impactScale != 1.0f) putNum(rec, "impact", c->impactScale);
            if (c->debrisLifetimeS > 0.0f) putNum(rec, "lifetime", c->debrisLifetimeS);
            if (c->maxPieces > 0) putNum(rec, "maxpieces", static_cast<f32>(c->maxPieces));
            if (c->flags & softbody::kDestructIndestructible) put(rec, "indestructible", "1");
            if (c->flags & softbody::kDestructNoDebris) put(rec, "nodebris", "1");
            break;
        }
        case kJelly: {
            const auto* c = w.component<softbody::CSoftVolume>(e, type);
            putAsset(rec, "asset", c->asset, pathOf);
            if (c->stiffnessScale > 0.0f && c->stiffnessScale != 1.0f) putNum(rec, "stiffness", c->stiffnessScale);
            if (c->cellCm > 0.0f) putNum(rec, "cell", c->cellCm);
            if (c->gravityScale > 0.0f && c->gravityScale != 1.0f) putNum(rec, "gravity", c->gravityScale);
            if (c->dampingScale > 0.0f && c->dampingScale != 1.0f) putNum(rec, "damping", c->dampingScale);
            if (c->flags & softbody::kVolNoWorld) put(rec, "world", "0");
            if (c->flags & softbody::kVolNoGravity) put(rec, "gravityon", "0");
            break;
        }
        case kRope: {
            const auto* c = w.component<softbody::CSoftRope>(e, type);
            putAsset(rec, "asset", c->asset, pathOf);
            f32 ends[2][3];
            for (int end = 0; end < 2; ++end) {
                const scene::Entity anchor = end == 0 ? c->startEntity : c->endEntity;
                const f32* o = end == 0 ? c->startOffset : c->endOffset;
                if (anchor != 0 && w.valid(anchor)) {   // an entity-anchored end is written as the world point it is at
                    const Mat4& am = w.worldMatrix(anchor);
                    for (int k = 0; k < 3; ++k) ends[end][k] = o[0] * am.m[0][k] + o[1] * am.m[1][k] + o[2] * am.m[2][k] + am.m[3][k];
                } else {
                    for (int k = 0; k < 3; ++k) ends[end][k] = o[k];
                }
            }
            put(rec, "start", numStr(ends[0][0]) + "," + numStr(ends[0][1]) + "," + numStr(ends[0][2]));
            put(rec, "end", numStr(ends[1][0]) + "," + numStr(ends[1][1]) + "," + numStr(ends[1][2]));
            if (c->lengthCm > 0.0f) putNum(rec, "length", c->lengthCm);
            if (c->radiusCm > 0.0f) putNum(rec, "radius", c->radiusCm);
            if (c->flags & softbody::kRopeNoWorld) put(rec, "world", "0");
            if (c->flags & softbody::kRopeNoStartPin) put(rec, "startpin", "0");
            if (c->flags & softbody::kRopeNoEndPin) put(rec, "endpin", "0");
            break;
        }
        case kSoftField: {
            const auto* c = w.component<softbody::CSoftField>(e, type);
            const auto vec = [&](const char* key, f32 x, f32 y, f32 z) {
                if (x != 0.0f || y != 0.0f || z != 0.0f)
                    put(rec, key, numStr(x) + "," + numStr(y) + "," + numStr(z));
            };
            const auto kind = c->kind < softbody::kFieldKindCount ? static_cast<softbody::FieldKind>(c->kind) : softbody::FieldKind::RadialForce;
            const auto shape = c->shape < softbody::kFieldShapeCount ? static_cast<softbody::FieldShape>(c->shape) : softbody::FieldShape::Sphere;
            put(rec, "kind", softbody::fieldKindKey(kind));
            put(rec, "shape", softbody::fieldShapeKey(shape));
            vec("extent", c->extentX, c->extentY, c->extentZ);
            if (c->strength != 0.0f) putNum(rec, "strength", c->strength);
            if (c->energyJPerS != 0.0f) putNum(rec, "energy", c->energyJPerS);
            if (c->falloff != 0.0f) putNum(rec, "falloff", c->falloff);
            vec("dir", c->dirX, c->dirY, c->dirZ);
            if (c->turbulence != 0.0f) putNum(rec, "turbulence", c->turbulence);
            if (c->targets != 0) put(rec, "targets", softbody::fieldTargetsToText(c->targets));
            if (c->flags & softbody::kFieldOnce) put(rec, "once", "1");
            if (c->flags & softbody::kFieldDisabled) put(rec, "off", "1");
            break;
        }
#endif
        case kMorphTargets: {
            const auto* c = w.component<anim::CMorphTargets>(e, type);
            putAsset(rec, "rig", c->faceRig, pathOf);
            const f32 weight = c->weight == 0.0f ? 1.0f : c->weight;       // 0 reads as 1
            if (weight != 1.0f) putNum(rec, "weight", weight);
            if (c->flags & anim::kMorphInheritParent) put(rec, "inherit", "1");
            if (c->flags & anim::kMorphOff) put(rec, "off", "1");
            if (c->flags & anim::kMorphIgnoreAnim) put(rec, "ignoreanim", "1");
            break;
        }

#if AVER_MODULE_VEHICLE_SCENE
        case kVehicle: {
            const auto* c = w.component<vehicle::CVehicle>(e, type);
            putAsset(rec, "asset", c->asset, pathOf);
            // `preset` is always written without an asset (it is what the vehicle is), and with one only when it is not the default car.
            if (c->asset == 0 || c->preset != 0) put(rec, "preset", vehicle::presetName(c->preset < vehicle::kPresetCount ? c->preset : 0u));
            if (c->powerScale > 0.0f && c->powerScale != 1.0f) putNum(rec, "power", c->powerScale);
            if (c->gripScale > 0.0f && c->gripScale != 1.0f) putNum(rec, "grip", c->gripScale);
            if (c->brakeScale > 0.0f && c->brakeScale != 1.0f) putNum(rec, "brake", c->brakeScale);
            if (c->damageScale > 0.0f && c->damageScale != 1.0f) putNum(rec, "damage", c->damageScale);
            if (c->flags & vehicle::kVehicleNoActions) put(rec, "noactions", "1");
            if (c->flags & vehicle::kVehicleNoAudio) put(rec, "noaudio", "1");
            if (c->flags & vehicle::kVehicleNoEffects) put(rec, "noeffects", "1");
            if (c->flags & vehicle::kVehicleNoWheelVisuals) put(rec, "nowheels", "1");
            if (c->flags & vehicle::kVehicleIndestructible) put(rec, "indestructible", "1");
            break;
        }
#endif

#if AVER_MODULE_HAIR_SCENE
        case kHair: {
            const auto* c = w.component<hair::CHair>(e, type);
            putAsset(rec, "asset", c->asset, pathOf);
            if (c->widthScale > 0.0f && c->widthScale != 1.0f) putNum(rec, "width", c->widthScale);
            if (c->densityScale > 0.0f && c->densityScale != 1.0f) putNum(rec, "density", c->densityScale);
            if (c->lodBias != 0.0f) putNum(rec, "lodbias", c->lodBias);
            if (c->windScale > 0.0f && c->windScale != 1.0f) putNum(rec, "wind", c->windScale);
            if (c->stiffnessScale > 0.0f && c->stiffnessScale != 1.0f) putNum(rec, "stiffness", c->stiffnessScale);
            if (c->teleportCm > 0.0f) putNum(rec, "teleport", c->teleportCm);
            if (c->flags & hair::kHairNoCollision) put(rec, "collision", "0");
            if (c->flags & hair::kHairNoShadow) put(rec, "shadow", "0");
            if (c->flags & hair::kHairNoWind) put(rec, "windon", "0");
            if (c->flags & hair::kHairHidden) put(rec, "hidden", "1");
            break;
        }
#endif
        case kCollisionShape:
            return captureCollisionShape(w, e, pathOf);   // builds its own record: asset, surface, motion, noscale; the OUT fields are never read
        case kParticleEmitter: {
            const auto* c = w.component<scene::CParticleEmitter>(e, type);
            putAsset(rec, "effect", c->effect, pathOf);
            if (c->flags & scene::kParticleEmitterForceCpu) put(rec, "sim", "cpu");
            else if (c->flags & scene::kParticleEmitterForceGpu) put(rec, "sim", "gpu");
            if (c->flags & scene::kParticleEmitterStopped) put(rec, "playing", "0");
            if (c->flags & scene::kParticleEmitterAutoDestroy) put(rec, "autodestroy", "1");
            break;   // the seed is deliberately not captured: 0 means the system derives one per entity, so replays match
        }
#if AVER_MODULE_CAMERA
        case kCameraRig:
            captureCameraRig(w, e, *w.component<camera::CCameraRig>(e, type), rec);
            break;
#endif
        default: return std::nullopt;
    }
    return rec;
}

// ---- the batch applier shared by the load paths ----

struct Tally {
    u32 applied = 0;
    u32 warned = 0;
    u32 suppressed = 0;
};

void refuse(Tally& t, const std::string& type, const std::string& where, const std::string& why) {
    if (t.warned >= kWarnCap) { ++t.suppressed; return; }
    ++t.warned;
    AVER_WARN("[Level] COMPONENT {} refused on placement {}: {}", type, where, why);
}

std::string placementLabel(const fmt::OcWorldPlacement& p, i64 index) {
    return index >= 0 ? std::to_string(index) : "'" + p.asset + "'";
}

// Canonical order = dependency order, so a level written by hand in any order applies the same.
void applyAll(scene::World& w, scene::Entity e, const fmt::OcWorldPlacement& p, i64 index, Tally& t) {
    std::vector<std::pair<int, const fmt::OcPlacedComponent*>> order;
    order.reserve(p.components.size());
    for (const fmt::OcPlacedComponent& r : p.components) order.emplace_back(typeIndex(r.type), &r);
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
        // Unknown types sort last; they are refused below.
        const int ka = a.first < 0 ? kTypeCount : a.first, kb = b.first < 0 ? kTypeCount : b.first;
        return ka < kb;
    });
    for (const auto& [ix, r] : order) {
        std::string why;
        if (applyOne(w, e, *r, &p, why)) ++t.applied;
        else refuse(t, r->type, placementLabel(p, index), why);
    }
}

} // namespace

bool applyPlacedComponent(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r) {
    std::string why;
    if (applyOne(w, e, r, nullptr, why)) return true;
    AVER_WARN("[Level] COMPONENT {} refused: {}", r.type, why);
    return false;
}

bool removePlacedComponent(scene::World& w, scene::Entity e, std::string_view type) {
    const int ix = typeIndex(type);
    if (ix == kUnknownType || !w.valid(e)) return false;
    const u32 id = typeId(w, ix);
    if (id == 0 || !w.hasComponent(e, id)) return false;
#if AVER_MODULE_SYNAPSE_SCENE
    // Stops a running tree first so a removal in edit mode leaves no instance for the next Play to find.
    if (ix == kBrain) synapse::btSystem().brainStop(e);
#endif
    if (ix == kCollisionShape) return removeCollisionShape(w, e);   // also takes back the zero-size Rigid Body the record attached, so Add Component's undo is exact
    return w.removeComponent(e, id);
}

std::optional<fmt::OcPlacedComponent> capturePlacedComponent(scene::World& w, scene::Entity e, std::string_view type, const PathOfIdFn& pathOf) {
    return captureOne(w, e, typeIndex(type), pathOf);
}

std::vector<fmt::OcPlacedComponent> capturePlacedComponents(scene::World& w, scene::Entity e, const PathOfIdFn& pathOf) {
    std::vector<fmt::OcPlacedComponent> out;
    for (int ix = 0; ix < kTypeCount; ++ix)
        if (auto rec = captureOne(w, e, ix, pathOf)) out.push_back(std::move(*rec));
#if AVER_MODULE_FRAMEWORK
    fmt::OcPlacedComponent groups;
    if (w.valid(e) && captureGroupsRecord(static_cast<int32_t>(e), groups)) out.push_back(std::move(groups));
#endif
    return out;
}

u32 applyPlacedComponents(scene::World& w, scene::Entity e, const fmt::OcWorldPlacement& p, i64 placementIndex) {
    Tally t;
    applyAll(w, e, p, placementIndex, t);
    if (t.suppressed) AVER_WARN("[Level] {} more COMPONENT record(s) refused (not listed)", t.suppressed);
    return t.applied;
}

void applyLevelComponents(scene::World& w, const fmt::OcWorldData& level, const world::LevelInstance& inst) {
    Tally t;
    u32 touched = 0;
    const usize n = std::min(inst.entities.size(), inst.placementIndex.size());
    for (usize k = 0; k < n; ++k) {
        const u32 pi = inst.placementIndex[k];
        if (pi >= level.placements.size()) continue;
        const fmt::OcWorldPlacement& p = level.placements[pi];
        if (p.components.empty()) continue;
        const u32 before = t.applied;
        applyAll(w, inst.entities[k], p, pi, t);
        if (t.applied > before) ++touched;
    }
    // A class placement has no placement entity (LevelInstance.cpp), so its records can be kept in the file but never applied.
    for (usize i = 0; i < level.placements.size(); ++i) {
        const fmt::OcWorldPlacement& p = level.placements[i];
        if (p.className.empty()) continue;
        // Props and Groups are applied by the host as it spawns the actor; anything else is not.
        usize unused = 0;
#if AVER_MODULE_FRAMEWORK
        for (const fmt::OcPlacedComponent& c : p.components) unused += !isPropsComponent(c.type) && !isGroupsComponent(c.type);
#else
        unused = p.components.size();
#endif
        if (unused > 0)
            AVER_WARN("[Level] placement {} is a class placement: {} of its COMPONENT records are not applied", i, unused);
    }
    if (t.suppressed) AVER_WARN("[Level] {} more COMPONENT record(s) refused (not listed)", t.suppressed);
    if (t.applied > 0) AVER_INFO("[Level] {} placed component(s) on {} placement(s)", t.applied, touched);
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE
