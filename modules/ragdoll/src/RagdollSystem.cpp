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
                        const std::vector<f32>& pos, const std::vector<f32>& rot, f32 w, const std::vector<f32>& boneW) {
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
        const f32 wi = boneW.empty() ? w : w * std::clamp(boneW[i], 0.0f, 1.0f);
        if (wi <= 0.0f) continue;
        const Transform t = transformFromMatrix(local);
        Transform& dst = pose.local[i];
        dst.position = lerp(dst.position, t.position, wi);
        dst.rotation = Quat::slerp(dst.rotation, t.rotation.normalized(), wi);
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
    blendWorldIntoPose(skel, pose, world, s.scale, s.ppos, s.prot, std::min(s.weight, 1.0f), s.boneBlend);
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
    s.rootBone = def->rootPart;
    if (def->rootPart >= 0) {
        const usize r = static_cast<usize>(def->rootPart) * 4;
        s.rootRot0 = Quat{s.trot[r], s.trot[r + 1], s.trot[r + 2], s.trot[r + 3]}.normalized();
    }
    {
        const Quat re = transformFromMatrix(world).rotation.normalized();
        s.up0 = re.rotate(Vec3{0, 0, 1});
        s.fwd0 = re.rotate(Vec3{1, 0, 0});
    }
    ensureDrive(s);
    // Inherit the animated pose before any simulated mode, so going limp starts from where it stands.
    aver_phys_ragdoll_set_pose(s.handle, s.tpos.data(), s.trot.data());
    aver_phys_ragdoll_set_target(s.handle, s.tpos.data(), s.trot.data());
    pushStrength(s);
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
        if (const auto pp = pendingProfiles_.find(e); pp != pendingProfiles_.end()) {
            s.profile = std::move(pp->second);
            pendingProfiles_.erase(pp);
        }
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
    pushStrength(s);
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
        if (s.life.state() == Life::Dying) {
            s.life.step(dt);
            pushStrength(s);
            if (s.life.state() == Life::Dead) s.mode = RagdollMode::Limp;
        }
        if (s.reaction && s.handle) {
            stepReaction(s, dt);
            if (!s.hr.active() && s.pending.empty()) {   // every envelope ran out: back to the animation alone
                destroyPhysics(s);
                it = states_.erase(it);
                continue;
            }
        }
        syncPhysMode(s);
        if (s.handle) pushDrive(s);
        if (s.stopping && s.weight <= 0.0f) {
            destroyPhysics(s);
            it = states_.erase(it);
            continue;
        }
        ++it;
    }
}


// ---- Physical animation ---------------------------------------------------------------------------------

namespace {
// The part-owning bones of a built ragdoll.
template <class F>
void forEachPartBone(const RagdollDef& d, F&& f) {
    for (u32 b = 0; b < d.boneCount; ++b)
        if (d.hasPart(b)) f(static_cast<i32>(b));
}
} // namespace

void RagdollSystem::ensureDrive(State& s) {
    if (!s.def) return;
    const usize n = s.def->boneCount;
    if (s.pStrength.size() == n) return;
    s.boneBlend.assign(n, 1.0f);
    s.pStrength.assign(n, -1.0f);
    s.pDamping.assign(n, -1.0f);
    s.pTorque.assign(n, 1.0f);
    s.sentStrength = s.pStrength;
    s.sentDamping = s.pDamping;
    s.sentTorque = s.pTorque;
    s.pinned.assign(n, 0);
    s.sentPinned = s.pinned;
}

// Sends what changed since the last push; most frames that is nothing.
void RagdollSystem::pushDrive(State& s) {
    if (!s.handle || !s.def || s.pStrength.size() != s.def->boneCount) return;
    forEachPartBone(*s.def, [&](i32 b) {
        const usize i = static_cast<usize>(b);
        if (s.pDamping[i] != s.sentDamping[i]) { aver_phys_ragdoll_set_part_damping(s.handle, b, s.pDamping[i]); s.sentDamping[i] = s.pDamping[i]; }
        if (s.pTorque[i] != s.sentTorque[i]) { aver_phys_ragdoll_set_part_torque_scale(s.handle, b, s.pTorque[i]); s.sentTorque[i] = s.pTorque[i]; }
        if (s.pStrength[i] != s.sentStrength[i]) { aver_phys_ragdoll_set_part_strength(s.handle, b, s.pStrength[i]); s.sentStrength[i] = s.pStrength[i]; }
        if (s.pinned[i] != s.sentPinned[i]) { aver_phys_ragdoll_set_part_pinned(s.handle, b, s.pinned[i]); s.sentPinned[i] = s.pinned[i]; }
    });
    // The impulses of hits wait for the pins to be let go of: a pinned part is kinematic and takes none.
    for (const State::Pending& p : s.pending) {
        const i32 part = s.def->partOf(static_cast<u32>(p.bone));
        if (part < 0) continue;
        const f32* c = &s.def->capsules[static_cast<usize>(part) * 7];
        const Vec3 mid{(c[0] + c[3]) * 0.5f * s.scale, (c[1] + c[4]) * 0.5f * s.scale, (c[2] + c[5]) * 0.5f * s.scale};
        s.ppos.resize(s.def->boneCount * 3);
        s.prot.resize(s.def->boneCount * 4);
        if (!aver_phys_ragdoll_get_pose(s.handle, s.ppos.data(), s.prot.data())) continue;
        const f32* q = &s.prot[static_cast<usize>(part) * 4];
        const Vec3 at = Vec3{s.ppos[static_cast<usize>(part) * 3], s.ppos[static_cast<usize>(part) * 3 + 1],
                             s.ppos[static_cast<usize>(part) * 3 + 2]} + Quat{q[0], q[1], q[2], q[3]}.rotate(mid);
        aver_phys_ragdoll_add_impulse(s.handle, part, p.impulse.x, p.impulse.y, p.impulse.z, at.x, at.y, at.z);
    }
    s.pending.clear();
}

void RagdollSystem::pushStrength(State& s) {
    if (s.handle) aver_phys_ragdoll_set_strength(s.handle, s.strength * s.life.strengthScale());
}

bool RagdollSystem::resolveFor(scene::Entity e, State& s) {
    if (s.haveResolved) return true;
    const fmt::OcSkeleton* skel = anim_ ? anim_->posedSkeleton(e) : nullptr;
    if (!skel) return false;
    static const PhysAnimProfile kDefault;
    s.resolved = resolvePhysAnim(s.profile ? *s.profile : kDefault, *skel);
    s.hr.configure(s.resolved);
    s.haveResolved = true;
    return true;
}

// Copies the envelopes into the per-bone blend, strength and pins: a bone is simulated while it is reacting.
static void envelopesToDrive(const RagdollDef& d, const HitReaction& hr, std::vector<f32>& blend,
                             std::vector<f32>& strength, std::vector<char>& pinned) {
    for (u32 b = 0; b < d.boneCount; ++b) {
        const i32 bi = static_cast<i32>(b);
        blend[b] = hr.blend(bi);
        if (!d.hasPart(b)) continue;
        const bool on = hr.reacting(bi);
        pinned[b] = on ? 0 : 1;
        strength[b] = on ? hr.strength(bi) : -1.0f;
    }
}

void RagdollSystem::stepReaction(State& s, f32 dt) {
    if (!s.def || s.pStrength.size() != s.def->boneCount) return;
    s.hr.step(dt);
    envelopesToDrive(*s.def, s.hr, s.boneBlend, s.pStrength, s.pinned);
}

bool RagdollSystem::setBoneBlend(scene::Entity e, i32 bone, f32 blend) {
    const auto it = states_.find(e);
    if (it == states_.end() || !it->second.def || bone < 0 || bone >= static_cast<i32>(it->second.def->boneCount)) return false;
    ensureDrive(it->second);
    it->second.boneBlend[static_cast<usize>(bone)] = std::clamp(blend, 0.0f, 1.0f);
    return true;
}

f32 RagdollSystem::boneBlend(scene::Entity e, i32 bone) const {
    const auto it = states_.find(e);
    if (it == states_.end() || bone < 0 || bone >= static_cast<i32>(it->second.boneBlend.size())) return -1.0f;
    return it->second.boneBlend[static_cast<usize>(bone)];
}

// The part a bone is or rides on in a built ragdoll, or -1.
static i32 partOfBone(const RagdollSystem*, const RagdollDef* d, i32 bone) {
    if (!d || bone < 0 || bone >= static_cast<i32>(d->boneCount)) return -1;
    return d->partOf(static_cast<u32>(bone));
}

bool RagdollSystem::setPartStrength(scene::Entity e, i32 bone, f32 strength) {
    const auto it = states_.find(e);
    if (it == states_.end()) return false;
    const i32 part = partOfBone(this, it->second.def, bone);
    if (part < 0) return false;
    ensureDrive(it->second);
    it->second.pStrength[static_cast<usize>(part)] = strength < 0.0f ? -1.0f : std::min(strength, 1.0f);
    pushDrive(it->second);
    return true;
}

bool RagdollSystem::setPartDamping(scene::Entity e, i32 bone, f32 damping) {
    const auto it = states_.find(e);
    if (it == states_.end()) return false;
    const i32 part = partOfBone(this, it->second.def, bone);
    if (part < 0) return false;
    ensureDrive(it->second);
    it->second.pDamping[static_cast<usize>(part)] = damping < 0.0f ? -1.0f : damping;
    pushDrive(it->second);
    return true;
}

bool RagdollSystem::setPartTorqueScale(scene::Entity e, i32 bone, f32 scale) {
    const auto it = states_.find(e);
    if (it == states_.end()) return false;
    const i32 part = partOfBone(this, it->second.def, bone);
    if (part < 0) return false;
    ensureDrive(it->second);
    it->second.pTorque[static_cast<usize>(part)] = std::max(scale, 0.0f);
    pushDrive(it->second);
    return true;
}

bool RagdollSystem::setPinned(scene::Entity e, i32 bone, bool pinned) {
    const auto it = states_.find(e);
    if (it == states_.end()) return false;
    const i32 part = partOfBone(this, it->second.def, bone);
    if (part < 0) return false;
    ensureDrive(it->second);
    it->second.pinned[static_cast<usize>(part)] = pinned ? 1 : 0;
    pushDrive(it->second);
    return true;
}

bool RagdollSystem::simulateSubtree(scene::Entity e, u64 boneNameHash) {
    const auto it = states_.find(e);
    if (it == states_.end() || !it->second.def) return false;
    State& s = it->second;
    const RagdollDef& d = *s.def;
    const i32 root = boneNameHash == 0 ? -1 : d.boneByHash(boneNameHash);
    if (boneNameHash != 0 && root < 0) return false;
    ensureDrive(s);
    s.reaction = false;
    // Parents precede children, so one forward pass marks the subtree.
    std::vector<char> in(d.boneCount, root < 0 ? 1 : 0);
    if (root >= 0) {
        in[static_cast<usize>(root)] = 1;
        for (u32 b = static_cast<u32>(root) + 1; b < d.boneCount; ++b)
            if (d.parents[b] >= 0 && in[static_cast<usize>(d.parents[b])]) in[b] = 1;
    }
    for (u32 b = 0; b < d.boneCount; ++b) {
        s.boneBlend[b] = in[b] ? 1.0f : 0.0f;
        if (d.hasPart(b)) s.pinned[b] = in[b] ? 0 : 1;
    }
    pushDrive(s);
    return true;
}

bool RagdollSystem::blendPhysicalAnimation(scene::Entity e, f32 weight, f32 seconds) {
    if (!installed_ || !std::isfinite(weight) || !std::isfinite(seconds)) return false;
    weight = std::clamp(weight, 0.0f, 1.0f);
    if (states_.find(e) == states_.end()) {
        if (!start(e, RagdollMode::Powered, 0.0f)) return false;
        states_[e].weight = 0.0f;
    }
    State& s = states_[e];
    if (s.mode == RagdollMode::Kinematic) s.mode = RagdollMode::Powered;
    s.stopping = false;
    s.goal = weight;
    setRate(s, seconds);
    if (s.rate >= 1e8f) s.weight = s.goal;
    syncPhysMode(s);
    return true;
}

void RagdollSystem::setProfile(scene::Entity e, std::shared_ptr<const PhysAnimProfile> profile) {
    const auto it = states_.find(e);
    if (it == states_.end()) {
        pendingProfiles_[e] = std::move(profile);   // adopted by the ragdoll applyHit starts
        return;
    }
    it->second.profile = std::move(profile);
    it->second.haveResolved = false;
}

bool RagdollSystem::applyHit(const HitEvent& hit) {
    if (!installed_ || !world_) return false;
    const scene::Entity e = static_cast<scene::Entity>(hit.entity);
    if (!world_->valid(e)) return false;

    auto it = states_.find(e);
    if (it == states_.end()) {
        if (!start(e, RagdollMode::Powered, 0.0f)) return false;
        it = states_.find(e);
        State& fresh = it->second;
        fresh.reaction = true;
        fresh.weight = fresh.goal = 1.0f;
        ensureDrive(fresh);
        if (!fresh.def) {   // not posed yet, so not built: nothing to react with
            destroyPhysics(fresh);
            states_.erase(it);
            return false;
        }
        std::fill(fresh.boneBlend.begin(), fresh.boneBlend.end(), 0.0f);
        forEachPartBone(*fresh.def, [&](i32 b) { fresh.pinned[static_cast<usize>(b)] = 1; });
    }
    State& s = it->second;
    if (!s.def || !resolveFor(e, s)) {
        if (s.reaction) { destroyPhysics(s); states_.erase(it); }
        return false;
    }
    ensureDrive(s);
    const RagdollDef& d = *s.def;
    const i32 bone = hit.boneHash == 0 ? d.rootPart : d.boneByHash(hit.boneHash);

    if (!s.reaction) {
        // A ragdoll the caller (or a death) owns: the hit only pushes.
        if (bone < 0 || s.life.state() == Life::GettingUp) return false;
        return addImpulse(e, hit.boneHash, hit.impulse);
    }

    // A reaction this call started that came to nothing is not left running.
    auto abandon = [&]() {
        if (s.reaction && !s.hr.active() && s.pending.empty()) { destroyPhysics(s); states_.erase(e); }
        return false;
    };
    if (hit.radius > 0.0f) {
        // Radial: every part inside the reach reacts, weaker with distance from the point; hit.impulse is
        // the total at the centre. Positions come from the last simulated pose.
        s.ppos.resize(d.boneCount * 3);
        s.prot.resize(d.boneCount * 4);
        if (!aver_phys_ragdoll_get_pose(s.handle, s.ppos.data(), s.prot.data())) return abandon();
        const f32 base = std::min(1.0f, hit.impulse.size() / std::max(s.resolved.fullImpulse, 1e-6f));
        bool any = false;
        forEachPartBone(d, [&](i32 b) {
            const usize i = static_cast<usize>(b);
            const Vec3 at{s.ppos[i * 3], s.ppos[i * 3 + 1], s.ppos[i * 3 + 2]};
            const f32 dist = (at - hit.point).size();
            if (dist >= hit.radius) return;
            const f32 k = 1.0f - dist / hit.radius;
            s.hr.triggerRadial(b, base * k);
            const Vec3 dir = (at - hit.point).getSafeNormal();
            s.pending.push_back({b, dir * (hit.impulse.size() * k * s.resolved.drive[i].impulse)});
            any = true;
        });
        if (!any) return abandon();
    } else {
        if (bone < 0) return abandon();
        s.pending.push_back({bone, s.hr.trigger(bone, hit.impulse)});
    }
    // Bring pins, strengths and blends up to this instant so the impulse lands on a simulated part.
    envelopesToDrive(d, s.hr, s.boneBlend, s.pStrength, s.pinned);
    syncPhysMode(s);
    pushDrive(s);
    return true;
}

const HitReaction* RagdollSystem::hitReaction(scene::Entity e) const {
    const auto it = states_.find(e);
    return it == states_.end() || !it->second.reaction ? nullptr : &it->second.hr;
}

Life RagdollSystem::life(scene::Entity e) const {
    const auto it = states_.find(e);
    return it == states_.end() ? Life::Alive : it->second.life.state();
}

bool RagdollSystem::die(scene::Entity e) {
    if (!installed_ || !world_ || !world_->valid(e)) return false;
    if (states_.find(e) == states_.end() && !start(e, RagdollMode::Powered, 0.0f)) return false;
    State& s = states_[e];
    if (!s.def) return false;
    resolveFor(e, s);
    const f32 secs = s.haveResolved ? s.resolved.deathSeconds : PhysAnimProfile{}.deathSeconds;
    if (!s.life.die(secs)) return false;
    // The death owns the ragdoll now: whole body, full weight, the global strength ramps down.
    ensureDrive(s);
    s.reaction = false;
    s.stopping = false;
    s.hr.reset();
    if (s.mode == RagdollMode::Kinematic) s.mode = RagdollMode::Powered;
    s.goal = 1.0f;
    setRate(s, 0.1f);
    for (u32 b = 0; b < s.def->boneCount; ++b) {
        s.boneBlend[b] = 1.0f;
        s.pStrength[b] = -1.0f;
        s.pinned[b] = 0;
    }
    if (s.life.state() == Life::Dead) s.mode = RagdollMode::Limp;
    syncPhysMode(s);
    pushStrength(s);
    pushDrive(s);
    return true;
}

Facing RagdollSystem::facingOf(const State& s) const {
    if (!s.handle || !s.def || s.rootBone < 0) return Facing::Upright;
    std::vector<f32> pos(s.def->boneCount * 3), rot(s.def->boneCount * 4);
    if (!aver_phys_ragdoll_get_pose(s.handle, pos.data(), rot.data())) return Facing::Upright;
    const f32* q = &rot[static_cast<usize>(s.rootBone) * 4];
    const Quat cur = Quat{q[0], q[1], q[2], q[3]}.normalized();
    const Quat delta = cur * Quat{-s.rootRot0.x, -s.rootRot0.y, -s.rootRot0.z, s.rootRot0.w};
    return classifyFacing(delta.rotate(s.up0), delta.rotate(s.fwd0));
}

Facing RagdollSystem::facing(scene::Entity e) const {
    const auto it = states_.find(e);
    return it == states_.end() ? Facing::Upright : facingOf(it->second);
}

bool RagdollSystem::getUp(scene::Entity e) {
    const auto it = states_.find(e);
    if (it == states_.end()) return false;
    State& s = it->second;
    if (s.life.state() != Life::Dead) return false;
    resolveFor(e, s);
    const u64 montage = chooseGetUp(s.resolved, facingOf(s));
    s.life.getUp();
    const f32 blend = s.resolved.getUpBlend;
    if (montage != 0) {
        if (getUpFn_) getUpFn_(e, montage, blend, getUpUser_);
        else if (anim_) anim_->montages().play(*anim_, *world_, e, montage, 0, 0, 1.0f, blend);
    }
    // The simulation fades out under the montage; the state goes (and the life is Alive again) when it has.
    s.stopping = true;
    s.goal = 0.0f;
    setRate(s, blend);
    return true;
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
