// Pose-to-pose retarget on entities. See PoseFollow.hpp.
#include "aver/anim/PoseFollow.hpp"

#include "aver/anim/Pose.hpp"
#include "aver/core/Assert.hpp"
#include "aver/core/Log.hpp"
#include "aver/scene/World.hpp"

#include <cmath>
#include <cstddef>

namespace aver::anim {

namespace {
u32 g_type = 0;
}

u32 PoseFollowSystem::registerComponent(scene::World& world) {
    auto b = world.registerComponent<CPoseFollow>("CPoseFollow");
    b.field("asset", scene::FieldKind::I64, static_cast<u16>(offsetof(CPoseFollow, asset)))
        .field("source", scene::FieldKind::Entity, static_cast<u16>(offsetof(CPoseFollow, source)))
        .field("weight", scene::FieldKind::F32, static_cast<u16>(offsetof(CPoseFollow, weight)))
        .field("flags", scene::FieldKind::I32, static_cast<u16>(offsetof(CPoseFollow, flags)))
        .field("reserved", scene::FieldKind::I32, static_cast<u16>(offsetof(CPoseFollow, reserved)), 0, true);
    AVER_ASSERTM(b.verify(sizeof(CPoseFollow)), "CPoseFollow");
    g_type = b.typeId();
    return g_type;
}

u32 PoseFollowSystem::typeId() { return g_type; }

CPoseFollow* PoseFollowSystem::attach(scene::World& world, scene::Entity e, u64 retargetAsset, scene::Entity source, f32 weight) {
    if (g_type == 0) return nullptr;
    auto* c = static_cast<CPoseFollow*>(world.addComponent(e, g_type));
    if (!c) return nullptr;
    c->asset = retargetAsset;
    c->source = source;
    c->weight = weight;
    c->flags = 0;
    c->reserved = 0;
    return c;
}

void PoseFollowSystem::install(AnimSystem& anim, scene::World& world) {
    world_ = &world;
    anim_ = &anim;
    if (anim.poseSource() == &PoseFollowSystem::sourceThunk && anim.poseSourceUser() == this) return;   // already in
    prev_ = anim.poseSource();
    prevUser_ = anim.poseSourceUser();
    anim.setPoseSource(&PoseFollowSystem::sourceThunk, this);
}

void PoseFollowSystem::uninstall(AnimSystem& anim) {
    anim.setPoseSource(nullptr, nullptr);
}

PoseFollowSystem::Binding* PoseFollowSystem::bindingFor(u64 asset, u64 srcSkelId, u64 dstSkelId) {
    if (epoch_ != anim_->contentEpoch()) { bindings_.clear(); epoch_ = anim_->contentEpoch(); }
    const Key key{asset, srcSkelId, dstSkelId};
    if (const auto it = bindings_.find(key); it != bindings_.end()) return it->second.get();

    auto b = std::make_unique<Binding>();
    const std::string path = anim_->assetPath(asset);
    RetargetAsset ra;
    std::string why;
    const fmt::OcSkeleton* src = anim_->skeleton(srcSkelId);
    const fmt::OcSkeleton* dst = anim_->skeleton(dstSkelId);
    if (path.empty() || !loadRetarget(path, ra, &why)) {
        AVER_WARN("[PoseFollow] could not read the bone map 0x{:016X}: {}", asset, why.empty() ? path : why);
    } else if (!src || !dst) {
        AVER_WARN("[PoseFollow] {}: the source or target skeleton does not load", path);
    } else {
        std::vector<RetargetIssue> issues;
        if (!b->rt.bind(ra, *src, *dst, &issues)) {
            for (const RetargetIssue& i : issues)
                if (i.kind == RetargetIssue::Kind::Error) AVER_WARN("[PoseFollow] {}: {}", path, i.text);
        } else {
            b->ok = true;
        }
    }
    Binding* raw = b.get();
    bindings_[key] = std::move(b);   // a failure is kept too, so it is reported once
    return raw;
}

bool PoseFollowSystem::sourceThunk(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose, void* user) {
    PoseFollowSystem& self = *static_cast<PoseFollowSystem*>(user);
    if (self.apply(e, skel, pose)) return true;
    return self.prev_ ? self.prev_(e, skel, pose, self.prevUser_) : false;
}

bool PoseFollowSystem::apply(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose) {
    if (!world_ || !anim_ || g_type == 0) return false;
    if (anim_->tickIndex() != tickSeen_) { tickSeen_ = anim_->tickIndex(); applied_ = appliedThis_; appliedThis_ = 0; }
    const auto* c = world_->component<CPoseFollow>(e, g_type);
    if (!c || c->weight <= 0.0f || c->asset == 0 || c->source == e || !world_->valid(c->source)) return false;
    const auto* dstSm = world_->component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
    const auto* srcSm = world_->component<scene::CSkeletalMesh>(c->source, scene::kComponentSkeletalMesh);
    if (!dstSm || !srcSm || dstSm->skeleton == 0 || srcSm->skeleton == 0) return false;
    const fmt::OcSkeleton* srcSkel = anim_->posedSkeleton(c->source);
    const Pose* srcPose = anim_->pose(c->source);
    if (!srcSkel || !srcPose || !srcPose->matches(*srcSkel)) return false;   // the source has not been posed yet
    Binding* b = bindingFor(c->asset, srcSm->skeleton, dstSm->skeleton);
    if (!b || !b->ok) return false;

    if (c->weight >= 1.0f) {
        b->rt.retarget(*srcPose, pose);
    } else {
        b->rt.retarget(*srcPose, tmp_);
        const Pose seed = pose;   // the pose underneath (the rest pose, seeded by the caller)
        blendPose(seed, tmp_, c->weight, pose, CurveBlend::Crossfade);
    }
    ++appliedThis_;
    (void)skel;
    return true;
}

PoseFollowSystem& poseFollowSystem() {
    static PoseFollowSystem s;
    return s;
}

} // namespace aver::anim
