// CControlRig and the system that applies it. See ControlRig.hpp for the shape and the rules.
#include "aver/anim/ControlRig.hpp"

#include "aver/anim/Ik.hpp"
#include "aver/core/Assert.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcAnim.hpp"
#include "aver/scene/World.hpp"

#include <cstddef>

namespace aver::anim {

namespace {
// The registered type id. File-scope rather than a member because registerComponents is static --
// the world owns the registry, and a second ControlRigSystem must find the same type rather than
// register a rival one.
u32 g_type = 0;
}

bool findBone(const fmt::OcSkeleton& skel, const std::string& name, u32& out) {
    for (usize i = 0; i < skel.bones.size(); ++i) {
        if (skel.bones[i].name == name) { out = static_cast<u32>(i); return true; }
    }
    return false;
}

bool applyRigOp(const fmt::OcSkeleton& skel, Pose& pose, const fmt::OcRigOp& op, f32 weight) {
    const f32 w = op.weight * weight;
    if (w <= 0.0f) return false;              // off is not a failure

    u32 root = 0;
    if (!findBone(skel, op.root, root)) return false;

    // BLENDED BY REPLAYING, NOT BY LERPING TRANSFORMS. The op is solved on a COPY at full strength
    // and the result slerped back toward the original per bone. Scaling the op's inputs instead --
    // moving the goal a fraction of the way -- would not be a partial version of the same solve: IK
    // is not linear in its goal, and half a goal produces a pose that is nobody's answer.
    Pose solved = pose;

    bool ok = false;
    if (op.kind == fmt::OcRigOpKind::TwoBoneIk) {
        u32 mid = 0, tip = 0;
        if (!findBone(skel, op.mid, mid) || !findBone(skel, op.tip, tip)) return false;
        ok = twoBoneIk(skel, solved, root, mid, tip, op.target, op.hint);
    } else {
        ok = aimAt(skel, solved, root, op.target, op.hint);
    }
    if (!ok) return false;

    if (w >= 1.0f) { pose = std::move(solved); return true; }

    for (usize i = 0; i < pose.local.size() && i < solved.local.size(); ++i) {
        pose.local[i].rotation = Quat::slerp(pose.local[i].rotation, solved.local[i].rotation, w);
        // Position and scale are not touched by either op today; blending them anyway would quietly
        // start mattering the day an op does move a bone, and be wrong in a way nothing tests.
    }
    return true;
}

u32 ControlRigSystem::registerComponents(scene::World& world) {
    auto b = world.registerComponent<CControlRig>("CControlRig");
    b.field("rig", scene::FieldKind::I64, static_cast<u16>(offsetof(CControlRig, rig)))
        .field("weight", scene::FieldKind::F32, static_cast<u16>(offsetof(CControlRig, weight)));
    AVER_ASSERTM(b.verify(sizeof(CControlRig)), "CControlRig");
    g_type = b.typeId();
    return g_type;
}

CControlRig* ControlRigSystem::attach(scene::World& world, scene::Entity e, u64 rigAsset, f32 weight) {
    if (g_type == 0) return nullptr;
    auto* c = static_cast<CControlRig*>(world.addComponent(e, g_type));
    if (!c) return nullptr;
    // A FRESH VALUE OVER THE ZERO-FILLED BYTES. addComponent runs no constructor, so without this
    // `weight` would be 0 and the rig would attach and do nothing -- see the header.
    *c = CControlRig{};
    c->rig = rigAsset;
    c->weight = weight;
    return c;
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

void ControlRigSystem::modifierThunk(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose,
                                     void* user) {
    static_cast<ControlRigSystem*>(user)->applyTo(e, skel, pose);
}

void ControlRigSystem::applyTo(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose) {
    if (!world_ || !anim_ || g_type == 0) return;
    const auto* c = world_->component<CControlRig>(e, g_type);
    if (!c || c->weight <= 0.0f) return;

    const fmt::OcRigData* rig = rigFor(c->rig, *anim_);
    if (!rig) return;

    // IN FILE ORDER, and that is the contract: a rig is a list, later ops see what earlier ones did.
    // An aim on the head after an IK on the arm should see the arm where the IK put it.
    bool any = false;
    for (const fmt::OcRigOp& op : rig->ops)
        any = applyRigOp(skel, pose, op, c->weight) || any;
    if (any) ++applied_;
}

void ControlRigSystem::install(AnimSystem& anim, scene::World& world) {
    world_ = &world;
    anim_ = &anim;
    applied_ = 0;
    anim.setPoseModifier(&ControlRigSystem::modifierThunk, this);
}

void ControlRigSystem::uninstall(AnimSystem& anim) { anim.setPoseModifier(nullptr, nullptr); }

} // namespace aver::anim
