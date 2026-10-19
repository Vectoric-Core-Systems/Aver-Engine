// CControlRig and the system that applies it. See ControlRig.hpp for the shape and the rules.
#include "aver/anim/ControlRig.hpp"

#include "aver/anim/Ik.hpp"
#include "aver/anim/IkSolvers.hpp"
#include "aver/core/Assert.hpp"
#include "aver/core/Hash.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <cstddef>

namespace aver::anim {

namespace {
// The registered type id. File-scope rather than a member because registerComponents is static --
// the world owns the registry, and a second ControlRigSystem must find the same type rather than
// register a rival one.
u32 g_type = 0;

Vec3 xformPoint(const Vec3& v, const Mat4& m) {
    return Vec3{v.x * m.m[0][0] + v.y * m.m[1][0] + v.z * m.m[2][0] + m.m[3][0],
                v.x * m.m[0][1] + v.y * m.m[1][1] + v.z * m.m[2][1] + m.m[3][1],
                v.x * m.m[0][2] + v.y * m.m[1][2] + v.z * m.m[2][2] + m.m[3][2]};
}

const fmt::OcRigInput* declared(const RigEval& ev, const std::string& name, fmt::OcRigInputType type) {
    const fmt::OcRigInput* in = ev.rig ? ev.rig->input(name) : nullptr;
    return (in && in->type == type) ? in : nullptr;
}

void ensureModel(const RigEval& ev) {
    if (ev.modelValid || !ev.skel || !ev.pose) return;
    poseToModel(*ev.skel, *ev.pose, ev.model);
    ev.modelValid = true;
}
} // namespace

// ---- RigEval ----

bool RigEval::vec(const fmt::OcRigRef& ref, const Vec3& literal, bool point, Vec3& out) const {
    switch (ref.kind) {
        case fmt::OcRigRef::Kind::Literal:
            out = literal;
            return true;
        case fmt::OcRigRef::Kind::Input: {
            const fmt::OcRigInput* in = declared(*this, ref.name, fmt::OcRigInputType::Vec3);
            if (!in) return false;
            f32 v[4] = {in->defVec.x, in->defVec.y, in->defVec.z, 0};
            if (input) input(fnv1a64(std::string_view(ref.name)), v, inputUser);
            Vec3 r{v[0], v[1], v[2]};
            out = (point && in->world) ? xformPoint(r, worldToModel) : r;
            return true;
        }
        case fmt::OcRigRef::Kind::Bone: {
            if (!point || !skel) return false;
            u32 b = 0;
            if (!findBone(*skel, ref.name, b)) return false;
            ensureModel(*this);
            if (b >= model.size()) return false;
            out = Vec3{model[b].m[3][0], model[b].m[3][1], model[b].m[3][2]} + ref.offset;
            return true;
        }
    }
    return false;
}

bool RigEval::quat(const fmt::OcRigRef& ref, const Quat& literal, bool convert, Quat& out) const {
    if (ref.kind == fmt::OcRigRef::Kind::Literal) { out = literal; return true; }
    if (ref.kind != fmt::OcRigRef::Kind::Input) return false;
    const fmt::OcRigInput* in = declared(*this, ref.name, fmt::OcRigInputType::Quat);
    if (!in) return false;
    f32 v[4] = {in->defQuat.x, in->defQuat.y, in->defQuat.z, in->defQuat.w};
    if (input) input(fnv1a64(std::string_view(ref.name)), v, inputUser);
    const Quat q = Quat{v[0], v[1], v[2], v[3]}.normalized();
    out = (convert && in->world) ? (worldRotInv * q).normalized() : q;
    return true;
}

bool RigEval::weightOf(const fmt::OcRigOp& op, f32& out) const {
    if (op.weightInput.empty()) { out = op.weight; return true; }
    const fmt::OcRigInput* in = declared(*this, op.weightInput, fmt::OcRigInputType::Float);
    if (!in) return false;
    f32 v[4] = {in->defFloat, 0, 0, 0};
    if (input) input(fnv1a64(std::string_view(op.weightInput)), v, inputUser);
    out = v[0] < 0.0f ? 0.0f : (v[0] > 1.0f ? 1.0f : v[0]);
    return true;
}

bool findBone(const fmt::OcSkeleton& skel, const std::string& name, u32& out) {
    for (usize i = 0; i < skel.bones.size(); ++i) {
        if (skel.bones[i].name == name) { out = static_cast<u32>(i); return true; }
    }
    return false;
}

namespace {

// Tip rotation for the analytic two-bone op: slerped toward the wanted MODEL rotation of the tip.
void applyTipRot(const fmt::OcSkeleton& skel, Pose& pose, u32 tip, const Quat& want, f32 w) {
    std::vector<Mat4> m;
    poseToModel(skel, pose, m);
    const Quat cur = transformFromMatrix(m[tip]).rotation.normalized();
    const Quat q = Quat::slerp(cur, want, w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w)).normalized();
    setBoneModelTransform(skel, pose, tip, nullptr, &q);
}

} // namespace

bool applyRigOp(const fmt::OcSkeleton& skel, Pose& pose, const fmt::OcRigOp& op, f32 weight, const RigEval& ev) {
    ev.skel = &skel;
    ev.pose = &pose;
    ev.modelValid = false;

    f32 opWeight = 0.0f;
    if (!ev.weightOf(op, opWeight)) return false;
    const f32 w = opWeight * weight;
    if (w <= 0.0f) return false;              // off is not a failure

    u32 root = 0;
    if (!findBone(skel, op.root, root)) return false;

    // BLENDED BY REPLAYING, NOT BY LERPING TRANSFORMS. The op is solved on a COPY at full strength
    // and the result slerped back toward the original per bone. Scaling the op's inputs instead --
    // moving the goal a fraction of the way -- would not be a partial version of the same solve: IK
    // is not linear in its goal, and half a goal produces a pose that is nobody's answer.
    Pose solved = pose;
    bool ok = false;

    switch (op.kind) {
        case fmt::OcRigOpKind::TwoBoneIk: {
            u32 mid = 0, tip = 0;
            if (!findBone(skel, op.mid, mid) || !findBone(skel, op.tip, tip)) return false;
            Vec3 goal, pole;
            if (!ev.vec(op.targetRef, op.target, true, goal) || !ev.vec(op.hintRef, op.hint, true, pole)) return false;
            ok = twoBoneIk(skel, solved, root, mid, tip, goal, pole);
            if (ok && op.hasTipRot) {
                Quat q;
                if (ev.quat(op.rotRef, op.rotation, true, q)) applyTipRot(skel, solved, tip, q, op.rotWeight);
            }
            break;
        }
        case fmt::OcRigOpKind::AimAt: {
            Vec3 at, axis;
            if (!ev.vec(op.targetRef, op.target, true, at) || !ev.vec(op.hintRef, op.hint, false, axis)) return false;
            ok = aimAt(skel, solved, root, at, axis);
            break;
        }
        case fmt::OcRigOpKind::Fabrik:
        case fmt::OcRigOpKind::Ccd: {
            u32 tip = 0;
            if (!findBone(skel, op.tip, tip)) return false;
            IkChain chain;
            if (!buildChain(skel, root, tip, chain)) return false;
            IkGoal goal;
            if (!ev.vec(op.targetRef, op.target, true, goal.position)) return false;
            if (op.hasPole) {
                if (!ev.vec(op.hintRef, op.hint, true, goal.pole)) return false;
                goal.hasPole = true;
            }
            if (op.hasTipRot) {
                if (!ev.quat(op.rotRef, op.rotation, true, goal.rotation)) return false;
                goal.hasRotation = true;
                goal.rotationWeight = op.rotWeight;
            }
            goal.weight = w;
            IkParams params;
            params.kind = op.kind == fmt::OcRigOpKind::Ccd ? IkSolverKind::Ccd : IkSolverKind::Fabrik;
            params.maxIterations = op.iterations ? op.iterations : (op.kind == fmt::OcRigOpKind::Ccd ? 12u : 10u);
            params.toleranceCm = op.tolerance > 0.0f ? op.tolerance : (op.kind == fmt::OcRigOpKind::Ccd ? 0.5f : 0.1f);
            params.maxStepDeg = op.maxStepDeg > 0.0f ? op.maxStepDeg : 30.0f;
            if (op.limits) {
                // The rig's own numbers when a CIkRig supplies them, the ragdoll defaults (45/45/30) otherwise:
                // limits=1 is never a silent no-op.
                chain.limits.assign(chain.bones.size(), IkJointLimit{});
                for (usize i = 0; i < chain.bones.size(); ++i) {
                    IkJointLimit& l = chain.limits[i];
                    defaultLimitAxis(skel, chain.bones[i], l.axis);
                    if (!ev.limitDefs) {
                        l.enabled = true;
                        l.swing1Deg = 45.0f; l.swing2Deg = 45.0f; l.twistDeg = 30.0f;
                        continue;
                    }
                    for (const IkRigLimitDef& d : *ev.limitDefs) {
                        if (d.bone != skel.bones[chain.bones[i]].name) continue;
                        l.enabled = true;
                        l.swing1Deg = d.swing1; l.swing2Deg = d.swing2; l.twistDeg = d.twist;
                        if (d.hasAxis) l.axis = d.axis;
                        break;
                    }
                }
            }
            static thread_local IkScratch scratch;
            return solveChain(skel, pose, chain, goal, params, scratch).solved;     // weight handled inside
        }
        case fmt::OcRigOpKind::CopyBone: {
            u32 dst = 0;
            if (!findBone(skel, op.dst, dst)) return false;
            if (op.modelSpace) {
                std::vector<Mat4> m;
                poseToModel(skel, pose, m);
                const Vec3 p{m[root].m[3][0], m[root].m[3][1], m[root].m[3][2]};
                const Quat q = transformFromMatrix(m[root]).rotation.normalized();
                ok = setBoneModelTransform(skel, solved, dst, op.copyPos ? &p : nullptr, op.copyRot ? &q : nullptr);
            } else {
                if (op.copyPos) solved.local[dst].position = pose.local[root].position;
                if (op.copyRot) solved.local[dst].rotation = pose.local[root].rotation;
                ok = true;
            }
            break;
        }
        case fmt::OcRigOpKind::SetBone: {
            Vec3 p;
            Quat q;
            if (op.copyPos && !ev.vec(op.targetRef, op.target, op.modelSpace, p)) return false;
            if (op.copyRot && !ev.quat(op.rotRef, op.rotation, op.modelSpace, q)) return false;
            if (op.modelSpace) {
                ok = setBoneModelTransform(skel, solved, root, op.copyPos ? &p : nullptr, op.copyRot ? &q : nullptr);
            } else {
                if (op.copyPos) solved.local[root].position = p;
                if (op.copyRot) solved.local[root].rotation = q.normalized();
                ok = true;
            }
            break;
        }
    }
    if (!ok) return false;

    if (w >= 1.0f) { pose = std::move(solved); return true; }

    const bool movesBones = op.kind == fmt::OcRigOpKind::CopyBone || op.kind == fmt::OcRigOpKind::SetBone;
    for (usize i = 0; i < pose.local.size() && i < solved.local.size(); ++i) {
        pose.local[i].rotation = Quat::slerp(pose.local[i].rotation, solved.local[i].rotation, w);
        // Position is only edited by copybone / setbone; the IK ops write rotations alone, and blending the
        // others would start to matter the day an op does move a bone, and be wrong in a way nothing tests.
        if (movesBones) pose.local[i].position = lerp(pose.local[i].position, solved.local[i].position, w);
    }
    return true;
}

bool applyRigOp(const fmt::OcSkeleton& skel, Pose& pose, const fmt::OcRigOp& op, f32 weight) {
    const RigEval empty;
    return applyRigOp(skel, pose, op, weight, empty);
}

u32 ControlRigSystem::registerComponents(scene::World& world) {
    auto b = world.registerComponent<CControlRig>("CControlRig");
    b.field("rig", scene::FieldKind::I64, static_cast<u16>(offsetof(CControlRig, rig)))
        .field("weight", scene::FieldKind::F32, static_cast<u16>(offsetof(CControlRig, weight)));
    AVER_ASSERTM(b.verify(sizeof(CControlRig)), "CControlRig");
    g_type = b.typeId();
    IkRigRuntime::registerComponent(world);       // CIkRig rides along: both hosts already call this
    return g_type;
}

CControlRig* ControlRigSystem::attach(scene::World& world, scene::Entity e, u64 rigAsset, f32 weight) {
    if (g_type == 0) return nullptr;
    auto* c = static_cast<CControlRig*>(world.addComponent(e, g_type));
    if (!c) return nullptr;
    // Every field written over the zero-filled bytes: addComponent runs no constructor, so without this
    // `weight` would be 0 and the rig would attach and do nothing -- see the header. No `*c = CControlRig{}`:
    // copying a temporary may carry stack bytes into the padding after `weight`, and snapshots copy raw bytes.
    c->rig = rigAsset;
    c->weight = weight;
    return c;
}

CIkRig* ControlRigSystem::attachIk(scene::World& world, scene::Entity e, u64 ikRigAsset, f32 weight) {
    return IkRigRuntime::attach(world, e, ikRigAsset, weight);
}

const fmt::OcRigData* ControlRigSystem::rigFor(u64 asset, AnimSystem& anim) {
    if (asset == 0) return nullptr;
    const auto hit = rigs_.find(asset);
    if (hit != rigs_.end()) return &hit->second;
    if (loadFailed_.count(asset)) return nullptr;

    const std::string path = anim.assetPath(asset);
    fmt::OcRigData data;
    std::string why;
    if (path.empty() || !fmt::loadOcRig(path, data, &why)) {
        // Cached as failed so a missing rig is reported ONCE rather than every frame for every
        // entity that names it -- the shape of log spam this editor has been bitten by before.
        loadFailed_[asset] = true;
        AVER_WARN("[ControlRig] could not load rig asset {}: {}", asset, why.empty() ? path : why);
        return nullptr;
    }
    return &(rigs_[asset] = std::move(data));
}

bool ControlRigSystem::setInput(scene::Entity e, u64 nameHash, const f32* v, u32 n) {
    if (!world_ || !world_->valid(e) || !v || n == 0 || n > 4) return false;
    Inputs& in = inputs_[e];
    for (u32 i = 0; i < in.count; ++i) {
        if (in.slots[i].hash != nameHash) continue;
        for (u32 k = 0; k < 4; ++k) in.slots[i].v[k] = k < n ? v[k] : 0.0f;
        return true;
    }
    if (in.count >= kMaxInputsPerEntity) {
        AVER_WARN("[ControlRig] entity {} already has {} distinct rig inputs; 0x{:016X} is refused", e, kMaxInputsPerEntity, nameHash);
        return false;
    }
    Inputs::Slot& s = in.slots[in.count++];
    s.hash = nameHash;
    for (u32 k = 0; k < 4; ++k) s.v[k] = k < n ? v[k] : 0.0f;
    return true;
}

void ControlRigSystem::clearInput(scene::Entity e, u64 nameHash) {
    const auto it = inputs_.find(e);
    if (it == inputs_.end()) return;
    if (nameHash == 0) { inputs_.erase(it); return; }
    Inputs& in = it->second;
    for (u32 i = 0; i < in.count; ++i) {
        if (in.slots[i].hash != nameHash) continue;
        in.slots[i] = in.slots[in.count - 1];
        in.slots[in.count - 1] = Inputs::Slot{};
        --in.count;
        break;
    }
}

void ControlRigSystem::clearRuntime() {
    inputs_.clear();
    ik_.clearRuntime();
}

namespace {
struct InputCtx {
    const void* inputs;     // the entity's Inputs, or null
};
}

bool ControlRigSystem::inputThunk(u64 nameHash, f32* out4, void* user) {
    const auto* in = static_cast<const Inputs*>(static_cast<InputCtx*>(user)->inputs);
    if (!in) return false;
    for (u32 i = 0; i < in->count; ++i) {
        if (in->slots[i].hash != nameHash) continue;
        for (int k = 0; k < 4; ++k) out4[k] = in->slots[i].v[k];
        return true;
    }
    return false;
}

void ControlRigSystem::modifierThunk(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose,
                                     void* user) {
    static_cast<ControlRigSystem*>(user)->applyTo(e, skel, pose);
}

void ControlRigSystem::applyTo(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose) {
    if (!world_ || !anim_ || g_type == 0) return;

    // Once per AnimSystem tick: reset the probe counter and drop state of entities that died.
    if (anim_->tickIndex() != pruneTick_) {
        pruneTick_ = anim_->tickIndex();
        ik_.beginTick(pruneTick_);
        for (auto it = inputs_.begin(); it != inputs_.end();) {
            if (!world_->valid(it->first) || world_->destroyPending(it->first)) it = inputs_.erase(it);
            else ++it;
        }
    }

    bool any = false;
    const auto* c = world_->component<CControlRig>(e, g_type);
    if (c && c->weight > 0.0f) {
        if (const fmt::OcRigData* rig = rigFor(c->rig, *anim_)) {
            RigEval ev;
            InputCtx ictx{nullptr};
            if (rig->needsV2()) {
                ev.rig = rig;
                const auto in = inputs_.find(e);
                ictx.inputs = in == inputs_.end() ? nullptr : &in->second;
                ev.input = &ControlRigSystem::inputThunk;
                ev.inputUser = &ictx;
                const Mat4& W = world_->worldMatrix(e);
                ev.worldToModel = W.inverse();
                const Quat wrot = transformFromMatrix(W).rotation.normalized();
                ev.worldRotInv = Quat{-wrot.x, -wrot.y, -wrot.z, wrot.w};
                if (IkRigRuntime::typeId() != 0) {
                    if (const auto* ik = world_->component<CIkRig>(e, IkRigRuntime::typeId()))
                        if (const IkRigAsset* a = ik_.assetFor(ik->asset)) ev.limitDefs = &a->limits;
                }
            }
            // IN FILE ORDER, and that is the contract: a rig is a list, later ops see what earlier ones did.
            // An aim on the head after an IK on the arm should see the arm where the IK put it.
            for (const fmt::OcRigOp& op : rig->ops)
                any = applyRigOp(skel, pose, op, c->weight, ev) || any;
        }
    }

    // Then the entity's IK Rig: body, chains by priority, feet.
    if (IkRigRuntime::typeId() != 0) {
        if (const auto* ik = world_->component<CIkRig>(e, IkRigRuntime::typeId()))
            if (ik->weight > 0.0f) any = ik_.apply(e, skel, pose, *ik, maxFootProbesPerTick) || any;
    }
    if (any) ++applied_;
}

void ControlRigSystem::install(AnimSystem& anim, scene::World& world) {
    world_ = &world;
    anim_ = &anim;
    applied_ = 0;
    ik_.init(world, anim);
    anim.setPoseModifier(&ControlRigSystem::modifierThunk, this);
}

void ControlRigSystem::uninstall(AnimSystem& anim) { anim.setPoseModifier(nullptr, nullptr); }

ControlRigSystem& controlRigSystem() {
    static ControlRigSystem s;
    return s;
}

} // namespace aver::anim
