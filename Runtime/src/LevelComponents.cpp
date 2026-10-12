#include "aver/game/LevelComponents.hpp"
#include "aver/core/CharConv.hpp"   // floating from_chars where libc++ has none

#if AVER_MODULE_SCENE

#include "aver/anim/AnimSystem.hpp"
#include "aver/anim/ControlRig.hpp"
#include "aver/anim/MotionMatchSystem.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/scene_abi.h"
#if AVER_MODULE_SYNAPSE_SCENE
#  include "aver/synapse/SynapseBt.hpp"
#  include "aver/synapse/SynapseSmartObject.hpp"
#endif

#include <algorithm>
#include <charconv>
#include <cstdio>
#include <iterator>

namespace aver::game {

namespace {

enum TypeIx : int {
    kSkeletalMesh, kAnimator, kRootMotion, kMotionMatcher, kControlRig, kIkRig, kBrain, kSmartObject, kSoftBody, kTypeCount,
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

bool applyOne(scene::World& w, scene::Entity e, const fmt::OcPlacedComponent& r, const fmt::OcWorldPlacement* placement, std::string& why) {
    const int ix = typeIndex(r.type);
    if (ix == kUnknownType) { why = "unknown type"; return false; }
#if !AVER_MODULE_SYNAPSE_SCENE
    if (ix == kBrain || ix == kSmartObject) { why = "unknown to this build"; return false; }
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
    return w.removeComponent(e, id);
}

std::optional<fmt::OcPlacedComponent> capturePlacedComponent(scene::World& w, scene::Entity e, std::string_view type, const PathOfIdFn& pathOf) {
    return captureOne(w, e, typeIndex(type), pathOf);
}

std::vector<fmt::OcPlacedComponent> capturePlacedComponents(scene::World& w, scene::Entity e, const PathOfIdFn& pathOf) {
    std::vector<fmt::OcPlacedComponent> out;
    for (int ix = 0; ix < kTypeCount; ++ix)
        if (auto rec = captureOne(w, e, ix, pathOf)) out.push_back(std::move(*rec));
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
        if (!p.className.empty() && !p.components.empty())
            AVER_WARN("[Level] placement {} is a graph class: its {} COMPONENT record(s) are not applied", i, p.components.size());
    }
    if (t.suppressed) AVER_WARN("[Level] {} more COMPONENT record(s) refused (not listed)", t.suppressed);
    if (t.applied > 0) AVER_INFO("[Level] {} placed component(s) on {} placement(s)", t.applied, touched);
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE
