// RagdollSystem: see RagdollSystem.hpp.
#include "aver/ragdoll/RagdollSystem.hpp"

#include "aver/anim/Pose.hpp"
#include "aver/core/Log.hpp"
#include "aver/physics/physics_ragdoll_abi.h"
#include "aver/physics/physics_abi.h"

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace aver::ragdoll {

namespace {

// The modifier already on `a`, when AnimSystem can say what it is. A template so a build whose
// AnimSystem predates the getters still compiles: the call is only formed if it is valid.
template <class A>
void captureChain(A& a, anim::PoseModifierFn& fn, void*& user) {
    if constexpr (requires { a.poseModifier(); a.poseModifierUser(); }) {
        fn = a.poseModifier();
        user = a.poseModifierUser();
    } else {
        fn = nullptr;
        user = nullptr;
    }
}

f32 uniformScale(const Mat4& w) {
    f32 s = 0.0f;
    for (int r = 0; r < 3; ++r)
        s += std::sqrt(w.m[r][0] * w.m[r][0] + w.m[r][1] * w.m[r][1] + w.m[r][2] * w.m[r][2]);
    s /= 3.0f;
    return s > 1e-6f ? s : 1.0f;
}

bool allFinite(const std::vector<f32>& v) {
    for (f32 f : v) if (!std::isfinite(f)) return false;
    return true;
}

// Bone world transforms (cm, xyzw) of `pose` placed by the entity's world matrix.
void poseToWorld(const fmt::OcSkeleton& skel, const anim::Pose& pose, const Mat4& world,
                 std::vector<f32>& pos, std::vector<f32>& rot) {
    std::vector<Mat4> model;
    anim::poseToModel(skel, pose, model);
    const usize n = model.size();
    pos.resize(n * 3);
    rot.resize(n * 4);
    for (usize i = 0; i < n; ++i) {
        const Transform t = transformFromMatrix(model[i] * world);
        const Quat q = t.rotation.normalized();
        pos[i * 3 + 0] = t.position.x; pos[i * 3 + 1] = t.position.y; pos[i * 3 + 2] = t.position.z;
        rot[i * 4 + 0] = q.x; rot[i * 4 + 1] = q.y; rot[i * 4 + 2] = q.z; rot[i * 4 + 3] = q.w;
    }
}

// Blends simulated bone world transforms into `pose` by `w`. Bone scales are the animation's.
void blendWorldIntoPose(const fmt::OcSkeleton& skel, anim::Pose& pose, const Mat4& world, f32 scale,
                        const std::vector<f32>& pos, const std::vector<f32>& rot, f32 w) {
    const usize n = skel.bones.size();
    // The unit-scale world matrices of the simulated bones, carrying the entity's scale so the
    // chain below cancels it: a local transform is relative, and relative is scale-free.
    std::vector<Mat4> mw(n);
    const Mat4 s = Mat4::scale({scale, scale, scale});
    for (usize i = 0; i < n; ++i) {
        const Quat q{rot[i * 4 + 0], rot[i * 4 + 1], rot[i * 4 + 2], rot[i * 4 + 3]};
        mw[i] = s * Mat4::fromQuat(q.normalized()) *
                Mat4::translation({pos[i * 3 + 0], pos[i * 3 + 1], pos[i * 3 + 2]});
    }
    const Mat4 worldInv = world.inverse();
    for (usize i = 0; i < n; ++i) {
        const i32 p = skel.bones[i].parent;
        const Mat4 local = (p >= 0) ? mw[i] * mw[static_cast<usize>(p)].inverse() : mw[i] * worldInv;
        const Transform t = transformFromMatrix(local);
        Transform& dst = pose.local[i];
        dst.position = lerp(dst.position, t.position, w);
        dst.rotation = Quat::slerp(dst.rotation, t.rotation.normalized(), w);
    }
}

} // namespace

bool RagdollSystem::install(anim::AnimSystem& anim, scene::World& world,
                            anim::PoseModifierFn prev, void* prevUser) {
    anim_ = &anim;
    world_ = &world;
    if (!prev) captureChain(anim, prev, prevUser);
    // Already installed: chaining ourselves would call ourselves.
    if (prev == &RagdollSystem::modifierThunk) { installed_ = true; return true; }
    if (!prev && anim.hasPoseModifier()) {
        AVER_WARN("[Ragdoll] a pose modifier is installed but cannot be chained (AnimSystem::poseModifier "
                  "is missing); ragdolls are not installed");
        return false;
    }
    prev_ = prev;
    prevUser_ = prevUser;
    anim.setPoseModifier(&RagdollSystem::modifierThunk, this);
    installed_ = true;
    return true;
}

void RagdollSystem::uninstall() {
    if (!anim_) return;
    anim_->setPoseModifier(prev_, prevUser_);
    installed_ = false;
    prev_ = nullptr;
    prevUser_ = nullptr;
}

void RagdollSystem::modifierThunk(scene::Entity e, const fmt::OcSkeleton& skel, anim::Pose& pose, void* user) {
    auto* self = static_cast<RagdollSystem*>(user);
    if (self->prev_) self->prev_(e, skel, pose, self->prevUser_);
    self->apply(e, skel, pose);
}

void RagdollSystem::apply(scene::Entity e, const fmt::OcSkeleton& skel, anim::Pose& pose) {
    const auto it = states_.find(e);
    if (it == states_.end() || !world_) return;
    State& s = it->second;
    if (pose.local.size() != skel.bones.size()) return;

    if (s.handle == 0 && !build(e, s, skel, pose)) { states_.erase(it); return; }
    if (s.def->boneCount != skel.bones.size()) {
        destroyPhysics(s);
        states_.erase(it);
        return;
    }

    const Mat4 world = world_->worldMatrix(e);
    s.scale = uniformScale(world);

    // The animated pose is the target for kinematic and powered parts, and the baseline the
    // simulated pose is blended over.
    poseToWorld(skel, pose, world, s.tpos, s.trot);
    aver_phys_ragdoll_set_target(s.handle, s.tpos.data(), s.trot.data());

    if (s.weight <= 0.0f || s.physMode == AVER_PHYS_RAGDOLL_KINEMATIC) return;
    s.ppos.resize(skel.bones.size() * 3);
    s.prot.resize(skel.bones.size() * 4);
    if (!aver_phys_ragdoll_get_pose(s.handle, s.ppos.data(), s.prot.data())) return;
    if (!allFinite(s.ppos) || !allFinite(s.prot)) return;
    // A bone with no part above it (a transform node over the pelvis) has no simulated transform:
    // it stays where the animation put it, whatever the physics ABI reports for it.
    for (u32 b = 0; b < s.def->boneCount; ++b) {
        if (s.def->partOf(b) >= 0) continue;
        std::copy_n(&s.tpos[b * 3], 3, &s.ppos[b * 3]);
        std::copy_n(&s.trot[b * 4], 4, &s.prot[b * 4]);
    }
    blendWorldIntoPose(skel, pose, world, s.scale, s.ppos, s.prot, std::min(s.weight, 1.0f));
}

const RagdollDef* RagdollSystem::defFor(u64 skeletonId, const fmt::OcSkeleton& skel) {
    if (const auto hit = defs_.find(skeletonId); hit != defs_.end()) return &hit->second;

    RagdollOverrides ov;
    bool haveOv = false;
    const std::string skelPath = anim_ ? anim_->assetPath(skeletonId) : std::string{};
    if (!skelPath.empty()) {
        const std::string path = overridePathFor(skelPath);
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            std::string why;
            if (loadRagdollOverrides(path, ov, &why)) {
                haveOv = true;
            } else {
                AVER_WARN("[Ragdoll] ignoring '{}': {}", path, why);
                ov = {};
            }
        }
    }
    RagdollDef def = buildRagdoll(skel, opt_, haveOv ? &ov : nullptr);
    AVER_INFO("[Ragdoll] skeleton {}: {} part(s) over {} bone(s){}", skeletonId, def.partCount, def.boneCount,
              haveOv ? " (override file applied)" : "");
    return &(defs_[skeletonId] = std::move(def));
}

bool RagdollSystem::build(scene::Entity e, State& s, const fmt::OcSkeleton& skel, const anim::Pose& pose) {
    const RagdollDef* def = defFor(s.skeleton, skel);
    if (!def || def->partCount == 0 || def->boneCount != skel.bones.size()) {
        AVER_WARN("[Ragdoll] entity {}: skeleton has no bone large enough for a physics part", e);
        return false;
    }
    s.def = def;

    const Mat4 world = world_->worldMatrix(e);
    s.scale = uniformScale(world);
    poseToWorld(skel, pose, world, s.tpos, s.trot);

    // The definition is in the skeleton's own centimetres; the entity may be scaled.
    std::vector<f32> caps = def->capsules;
    for (usize i = 0; i < caps.size(); ++i) caps[i] *= s.scale;

    s.handle = aver_phys_ragdoll_create(static_cast<i32>(def->boneCount), def->parents.data(), s.tpos.data(),
                                        s.trot.data(), caps.data(), def->limitsDeg.data(), def->totalMassKg,
                                        userLayer_);
    if (s.handle == 0) {
        AVER_WARN("[Ragdoll] entity {}: physics refused the ragdoll (physics error {})", e, aver_phys_last_error());
        return false;
    }
    for (u32 b = 0; b < def->boneCount; ++b) {
        if (!def->hasPart(b)) continue;
        if (const i32 body = aver_phys_ragdoll_body_of_bone(s.handle, static_cast<i32>(b))) {
            aver_phys_set_entity(body, static_cast<i32>(e));
            // Authored per-part materials (an .ocragdoll override file); absent, the parts keep the world's. The authored
            // masses wait for the first simulated mode: a kinematic body has no mass to set.
            if (!def->massKg.empty()) {
                if (def->friction[b] >= 0.0f) aver_phys_body_set_friction(body, def->friction[b]);
                if (def->bounce[b] >= 0.0f) aver_phys_body_set_restitution(body, def->bounce[b]);
            }
        }
    }
    s.massApplied = false;
    // Inherit the animated pose before any simulated mode, so going limp starts from where it stands.
    aver_phys_ragdoll_set_pose(s.handle, s.tpos.data(), s.trot.data());
    aver_phys_ragdoll_set_target(s.handle, s.tpos.data(), s.trot.data());
    aver_phys_ragdoll_set_strength(s.handle, s.strength);
    s.physMode = -1;
    syncPhysMode(s);
    return true;
}

void RagdollSystem::syncPhysMode(State& s) {
    if (s.handle == 0) return;
    i32 want = static_cast<i32>(s.mode);
    // Going kinematic keeps the simulation until the blend back to animation has finished, or the
    // parts would snap to the animated pose while the skin is still showing the simulated one.
    if (s.mode == RagdollMode::Kinematic && s.weight > 0.0f && s.physMode > 0) want = s.physMode;
    if (want == s.physMode) return;
    aver_phys_ragdoll_set_mode(s.handle, want);
    s.physMode = want;
    // The parts are dynamic now: give them their authored masses (once; Jolt keeps them across later mode switches).
    if (want > 0 && !s.massApplied && s.def && !s.def->massKg.empty()) {
        s.massApplied = true;
        for (u32 b = 0; b < s.def->boneCount; ++b) {
            if (!s.def->hasPart(b) || s.def->massKg[b] <= 0.0f) continue;
            if (const i32 body = aver_phys_ragdoll_body_of_bone(s.handle, static_cast<i32>(b)))
                aver_phys_body_set_mass(body, s.def->massKg[b]);
        }
    }
}

void RagdollSystem::destroyPhysics(State& s) {
    if (s.handle) aver_phys_ragdoll_destroy(s.handle);
    s.handle = 0;
}

void RagdollSystem::setRate(State& s, f32 blendSeconds) {
    s.rate = blendSeconds > 1e-4f ? 1.0f / blendSeconds : 1e9f;
}

bool RagdollSystem::start(scene::Entity e, RagdollMode mode, f32 blendSeconds) {
    if (!installed_ || !world_->valid(e)) return false;
    if (static_cast<i32>(mode) < 0 || static_cast<i32>(mode) > 2) return false;
    const auto* sm = world_->component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
    if (!sm || sm->skeleton == 0) return false;

    const bool fresh = states_.find(e) == states_.end();
    State& s = states_[e];
    if (fresh) {
        s.skeleton = sm->skeleton;
        s.weight = 0.0f;
    }
    s.mode = mode;
    s.stopping = false;
    // Kinematic output is the animation itself: nothing to blend in.
    s.goal = (mode == RagdollMode::Kinematic) ? 0.0f : 1.0f;
    setRate(s, blendSeconds);
    if (s.rate >= 1e8f) s.weight = s.goal;

    // Build now when the entity has already been posed, so a hit that starts the ragdoll can add its
    // impulse in the same call sequence; otherwise the first pose the seam sees builds it.
    if (fresh) {
        const fmt::OcSkeleton* skel = anim_->posedSkeleton(e);
        const anim::Pose* pose = anim_->pose(e);
        if (skel && pose && pose->local.size() == skel->bones.size() && !build(e, s, *skel, *pose)) {
            states_.erase(e);
            return false;
        }
    }
    syncPhysMode(s);
    return true;
}

bool RagdollSystem::stop(scene::Entity e, f32 blendSeconds) {
    const auto it = states_.find(e);
    if (it == states_.end()) return false;
    State& s = it->second;
    if (blendSeconds <= 1e-4f || s.weight <= 0.0f || s.handle == 0) {
        destroyPhysics(s);
        states_.erase(it);
        return true;
    }
    s.stopping = true;
    s.goal = 0.0f;
    setRate(s, blendSeconds);
    return true;
}

i32 RagdollSystem::mode(scene::Entity e) const {
    const auto it = states_.find(e);
    return it == states_.end() ? -1 : static_cast<i32>(it->second.mode);
}

bool RagdollSystem::setStrength(scene::Entity e, f32 strength) {
    const auto it = states_.find(e);
    if (it == states_.end()) return false;
    State& s = it->second;
    s.strength = std::clamp(strength, 0.0f, 1.0f);
    if (s.handle) aver_phys_ragdoll_set_strength(s.handle, s.strength);
    return true;
}

bool RagdollSystem::addImpulse(scene::Entity e, u64 boneNameHash, const Vec3& impulse) {
    const auto it = states_.find(e);
    if (it == states_.end() || it->second.handle == 0 || !it->second.def) return false;
    State& s = it->second;
    const RagdollDef& d = *s.def;

    const i32 bone = boneNameHash == 0 ? d.rootPart : d.boneByHash(boneNameHash);
    if (bone < 0) return false;
    const i32 part = d.partOf(static_cast<u32>(bone));
    if (part < 0) return false;

    s.ppos.resize(d.boneCount * 3);
    s.prot.resize(d.boneCount * 4);
    if (!aver_phys_ragdoll_get_pose(s.handle, s.ppos.data(), s.prot.data())) return false;

    // The part's centre: the middle of its capsule segment, carried by the bone's simulated frame.
    const f32* c = &d.capsules[static_cast<usize>(part) * 7];
    const Vec3 mid{(c[0] + c[3]) * 0.5f * s.scale, (c[1] + c[4]) * 0.5f * s.scale, (c[2] + c[5]) * 0.5f * s.scale};
    const f32* q = &s.prot[static_cast<usize>(part) * 4];
    const Vec3 at = Vec3{s.ppos[static_cast<usize>(part) * 3], s.ppos[static_cast<usize>(part) * 3 + 1],
                         s.ppos[static_cast<usize>(part) * 3 + 2]} +
                    Quat{q[0], q[1], q[2], q[3]}.rotate(mid);
    return aver_phys_ragdoll_add_impulse(s.handle, part, impulse.x, impulse.y, impulse.z, at.x, at.y, at.z) != 0;
}

f32 RagdollSystem::weight(scene::Entity e) const {
    const auto it = states_.find(e);
    return it == states_.end() ? -1.0f : it->second.weight;
}

i32 RagdollSystem::physicsHandle(scene::Entity e) const {
    const auto it = states_.find(e);
    return it == states_.end() ? 0 : it->second.handle;
}

const RagdollDef* RagdollSystem::definition(scene::Entity e) const {
    const auto it = states_.find(e);
    return it == states_.end() ? nullptr : it->second.def;
}

void RagdollSystem::tick(f32 dt) {
    if (!world_) return;
    for (auto it = states_.begin(); it != states_.end();) {
        const scene::Entity e = it->first;
        State& s = it->second;
        if (!world_->valid(e) || world_->destroyPending(e)) {
            destroyPhysics(s);
            it = states_.erase(it);
            continue;
        }
        if (s.weight != s.goal) {
            const f32 step = s.rate >= 1e8f ? 1.0f : dt * s.rate;
            s.weight = s.weight < s.goal ? std::min(s.goal, s.weight + step) : std::max(s.goal, s.weight - step);
        }
        syncPhysMode(s);
        if (s.stopping && s.weight <= 0.0f) {
            destroyPhysics(s);
            it = states_.erase(it);
            continue;
        }
        ++it;
    }
}


void RagdollSystem::clear() {
    for (auto& [e, s] : states_) destroyPhysics(s);
    states_.clear();
}

RagdollSystem& ragdollSystem() {
    static RagdollSystem s;
    return s;
}

i32 providerStart(i32 entity, i32 mode, f32 blendSeconds, void* user) {
    return static_cast<RagdollSystem*>(user)->start(static_cast<scene::Entity>(entity),
                                                    static_cast<RagdollMode>(mode), blendSeconds) ? 1 : 0;
}

i32 providerStop(i32 entity, f32 blendSeconds, void* user) {
    return static_cast<RagdollSystem*>(user)->stop(static_cast<scene::Entity>(entity), blendSeconds) ? 1 : 0;
}

i32 providerMode(i32 entity, void* user) {
    return static_cast<RagdollSystem*>(user)->mode(static_cast<scene::Entity>(entity));
}

i32 providerSetStrength(i32 entity, f32 strength, void* user) {
    return static_cast<RagdollSystem*>(user)->setStrength(static_cast<scene::Entity>(entity), strength) ? 1 : 0;
}

i32 providerAddImpulse(i32 entity, i64 boneNameHash, f32 ix, f32 iy, f32 iz, void* user) {
    return static_cast<RagdollSystem*>(user)->addImpulse(static_cast<scene::Entity>(entity),
                                                         static_cast<u64>(boneNameHash), Vec3{ix, iy, iz}) ? 1 : 0;
}

} // namespace aver::ragdoll
