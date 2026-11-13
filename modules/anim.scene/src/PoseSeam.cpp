// The pose seam: per-entity bone overrides and reads of the finished pose. See PoseSeam.hpp.
#include "aver/anim/PoseSeam.hpp"

#include "aver/anim/Pose.hpp"
#include "aver/core/Assert.hpp"
#include "aver/core/Hash.hpp"
#include "aver/scene/World.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

namespace aver::anim {

namespace {

u32 g_type = 0;

f32 clamp01f(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }

bool finite3(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
bool finiteT(const Transform& t) {
    return finite3(t.position) && finite3(t.scale) && std::isfinite(t.rotation.x) && std::isfinite(t.rotation.y) &&
           std::isfinite(t.rotation.z) && std::isfinite(t.rotation.w);
}

// `want` is `cur` with the override's channels; the result is amount of the way from cur to want.
Transform blendTo(const Transform& cur, const Transform& ov, u32 channels, f32 amount) {
    Transform want = cur;
    if (channels & kPoseSeamPosition) want.position = ov.position;
    if (channels & kPoseSeamRotation) want.rotation = ov.rotation.normalized();
    if (channels & kPoseSeamScale)    want.scale = ov.scale;
    if (amount >= 1.0f) return want;
    Transform out;
    out.position = cur.position + (want.position - cur.position) * amount;
    out.scale = cur.scale + (want.scale - cur.scale) * amount;
    out.rotation = Quat::slerp(cur.rotation, want.rotation, amount).normalized();
    return out;
}

void putTransform(const Transform& t, f32* out) {
    out[0] = t.position.x; out[1] = t.position.y; out[2] = t.position.z;
    out[3] = t.rotation.x; out[4] = t.rotation.y; out[5] = t.rotation.z; out[6] = t.rotation.w;
    out[7] = t.scale.x; out[8] = t.scale.y; out[9] = t.scale.z;
}

Transform getTransform(const f32* in) {
    Transform t;
    t.position = Vec3{in[0], in[1], in[2]};
    t.rotation = Quat{in[3], in[4], in[5], in[6]};
    t.scale = Vec3{in[7], in[8], in[9]};
    return t;
}

} // namespace

u32 PoseSeamSystem::registerComponent(scene::World& world) {
    auto b = world.registerComponent<CPoseSeam>("CPoseSeam");
    b.field("weight", scene::FieldKind::F32, static_cast<u16>(offsetof(CPoseSeam, weight)))
        .field("flags", scene::FieldKind::I32, static_cast<u16>(offsetof(CPoseSeam, flags)));
    AVER_ASSERTM(b.verify(sizeof(CPoseSeam)), "CPoseSeam");
    g_type = b.typeId();
    return g_type;
}

u32 PoseSeamSystem::typeId() { return g_type; }

bool PoseSeamSystem::setBone(scene::Entity e, u32 bone, const Transform& t, u32 flags, f32 amount) {
    if (!world_ || !anim_ || g_type == 0 || !world_->valid(e)) return false;
    const u32 channels = flags & kPoseSeamAll;
    if (channels == 0 || !finiteT(t) || !std::isfinite(amount)) return false;
    const fmt::OcSkeleton* skel = anim_->posedSkeleton(e);
    if (!skel) {
        const auto* sm = world_->component<scene::CSkeletalMesh>(e, scene::kComponentSkeletalMesh);
        if (sm && sm->skeleton != 0) skel = anim_->skeleton(sm->skeleton);
    }
    if (!skel || bone >= skel->bones.size()) return false;

    if (!world_->component<CPoseSeam>(e, g_type)) {
        auto* c = static_cast<CPoseSeam*>(world_->addComponent(e, g_type));
        if (!c) return false;
        c->weight = 1.0f;
        c->flags = 0;
    }
    EntityRt& r = ents_[e];
    if (r.bones.size() != skel->bones.size()) {
        r.bones.assign(skel->bones.size(), BoneOv{});
        r.count = 0;
    }
    BoneOv& o = r.bones[bone];
    if (o.flags == 0) ++r.count;
    o.flags = flags & (kPoseSeamAll | kPoseSeamModel | kPoseSeamOnce);
    o.t = t;
    o.amount = clamp01f(amount);
    return true;
}

bool PoseSeamSystem::clearBone(scene::Entity e, i32 bone) {
    const auto it = ents_.find(e);
    if (it == ents_.end()) return false;
    EntityRt& r = it->second;
    if (bone < 0) {
        for (BoneOv& o : r.bones) o.flags = 0;
        r.count = 0;
        return true;
    }
    if (static_cast<usize>(bone) >= r.bones.size()) return false;
    if (r.bones[static_cast<usize>(bone)].flags != 0) { r.bones[static_cast<usize>(bone)].flags = 0; --r.count; }
    return true;
}

bool PoseSeamSystem::setWeight(scene::Entity e, f32 weight) {
    if (!world_ || g_type == 0 || !std::isfinite(weight)) return false;
    auto* c = world_->component<CPoseSeam>(e, g_type);
    if (!c) return false;
    // A zero-filled component reads 0 as 1, so an explicit 0 (a script fading the seam out) is stored as -1: otherwise
    // the last step of a fade snapped every override to full strength.
    const f32 w = clamp01f(weight);
    c->weight = w > 0.0f ? w : -1.0f;
    return true;
}

bool PoseSeamSystem::setEnabled(scene::Entity e, bool enabled) {
    if (!world_ || g_type == 0) return false;
    auto* c = world_->component<CPoseSeam>(e, g_type);
    if (!c) return false;
    c->flags = enabled ? (c->flags & ~kPoseSeamOff) : (c->flags | kPoseSeamOff);
    return true;
}

u32 PoseSeamSystem::overrideCount(scene::Entity e) const {
    const auto it = ents_.find(e);
    return it == ents_.end() ? 0 : it->second.count;
}

u32 PoseSeamSystem::boneCount(scene::Entity e) const {
    if (!anim_) return 0;
    const fmt::OcSkeleton* s = anim_->posedSkeleton(e);
    return s ? static_cast<u32>(s->bones.size()) : 0;
}

i32 PoseSeamSystem::findBone(scene::Entity e, const std::string& name) const {
    const fmt::OcSkeleton* s = anim_ ? anim_->posedSkeleton(e) : nullptr;
    if (!s) return -1;
    for (usize i = 0; i < s->bones.size(); ++i)
        if (s->bones[i].name == name) return static_cast<i32>(i);
    return -1;
}

const std::string* PoseSeamSystem::boneName(scene::Entity e, u32 bone) const {
    const fmt::OcSkeleton* s = anim_ ? anim_->posedSkeleton(e) : nullptr;
    return (s && bone < s->bones.size()) ? &s->bones[bone].name : nullptr;
}

bool PoseSeamSystem::restBone(scene::Entity e, u32 bone, Transform& out, i32& parent) const {
    const fmt::OcSkeleton* s = anim_ ? anim_->posedSkeleton(e) : nullptr;
    if (!s || bone >= s->bones.size()) return false;
    const fmt::OcBone& b = s->bones[bone];
    out = Transform{b.translation, b.rotation, b.scale};
    parent = b.parent;
    return true;
}

bool PoseSeamSystem::readBone(scene::Entity e, u32 bone, PoseReadSpace space, bool beforeSeam, Transform& out) const {
    if (!anim_) return false;
    const fmt::OcSkeleton* skel = anim_->posedSkeleton(e);
    const Pose* p = anim_->pose(e);
    if (!skel || !p || bone >= skel->bones.size() || !p->matches(*skel)) return false;
    if (beforeSeam) {
        const auto it = ents_.find(e);
        if (it != ents_.end() && it->second.before.matches(*skel) && it->second.seen == anim_->tickIndex())
            p = &it->second.before;
    }
    if (space == PoseReadSpace::Local) { out = p->local[bone]; return true; }
    std::vector<Mat4> model;
    poseToModel(*skel, *p, model);
    out = transformFromMatrix(model[bone]);
    return true;
}

void PoseSeamSystem::prune() {
    if (!world_) return;
    for (auto it = ents_.begin(); it != ents_.end();) {
        if (!world_->valid(it->first) || world_->destroyPending(it->first)) it = ents_.erase(it);
        else ++it;
    }
}

bool PoseSeamSystem::apply(scene::Entity e, const fmt::OcSkeleton& skel, Pose& pose) {
    if (!world_ || !anim_ || g_type == 0) return false;
    const auto it = ents_.find(e);
    if (it == ents_.end()) return false;
    const CPoseSeam* c = world_->component<CPoseSeam>(e, g_type);
    if (!c) return false;
    EntityRt& r = it->second;
    r.before = pose;
    r.seen = anim_->tickIndex();
    if ((c->flags & kPoseSeamOff) || r.count == 0 || !pose.matches(skel) || r.bones.size() != skel.bones.size()) return false;

    const f32 weight = c->weight > 0.0f ? c->weight : (c->weight < 0.0f ? 0.0f : 1.0f);   // -1: set to 0 (setWeight)
    const usize n = skel.bones.size();
    model_.assign(n, Mat4::identity());
    bool changed = false;
    for (usize i = 0; i < n; ++i) {
        const i32 parent = skel.bones[i].parent;
        const Mat4 parentModel = (parent >= 0 && static_cast<usize>(parent) < i) ? model_[static_cast<usize>(parent)]
                                                                                   : Mat4::identity();
        BoneOv& o = r.bones[i];
        if (o.flags != 0) {
            const f32 amount = clamp01f(o.amount * weight);
            const u32 channels = o.flags & kPoseSeamAll;
            if (o.flags & kPoseSeamModel) {
                const Transform curModel = transformFromMatrix(pose.local[i].toMatrix() * parentModel);
                const Transform target = blendTo(curModel, o.t, channels, amount);
                pose.local[i] = transformFromMatrix(target.toMatrix() * parentModel.inverse());
            } else {
                pose.local[i] = blendTo(pose.local[i], o.t, channels, amount);
            }
            changed = true;
            if (o.flags & kPoseSeamOnce) { o.flags = 0; --r.count; }
        }
        model_[i] = pose.local[i].toMatrix() * parentModel;
    }
    return changed;
}

// ---- provider ----

i32 poseProvider(i32 op, i32 entity, i64 h0, i64 h1, i64 h2, const f32* in, i32 inCount, f32* outF, i64* outI,
                 char* outStr, i32 outStrLen, void* user) {
    (void)h2;
    if (!user) return 0;
    PoseSeamSystem& sys = *static_cast<PoseSeamSystem*>(user);
    const scene::Entity e = static_cast<scene::Entity>(entity);
    // framework_pose_abi.h
    constexpr i32 kCount = 1, kFind = 2, kInfo = 3, kRead = 4, kWrite = 5, kClear = 6, kWeight = 7, kEnable = 8,
                  kOverrides = 9, kWriteRange = 10;
    switch (op) {
        case kCount: return static_cast<i32>(sys.boneCount(e));
        case kFind: {
            const u32 n = sys.boneCount(e);
            for (u32 i = 0; i < n; ++i) {
                const std::string* name = sys.boneName(e, i);
                if (name && fnv1a64(std::string_view(*name)) == static_cast<u64>(h0)) {
                    if (outI) outI[0] = i;
                    return 1;
                }
            }
            return 0;
        }
        case kInfo: {
            Transform rest;
            i32 parent = -1;
            if (h0 < 0 || !sys.restBone(e, static_cast<u32>(h0), rest, parent)) return 0;
            if (outF) putTransform(rest, outF);
            if (outI) outI[0] = parent;
            if (outStr && outStrLen > 0) {
                const std::string* name = sys.boneName(e, static_cast<u32>(h0));
                const usize len = name ? std::min<usize>(name->size(), static_cast<usize>(outStrLen) - 1) : 0;
                if (len) std::memcpy(outStr, name->data(), len);
                outStr[len] = '\0';
            }
            return 1;
        }
        case kRead: {
            Transform t;
            if (h0 < 0 || !outF) return 0;
            if (!sys.readBone(e, static_cast<u32>(h0), (h1 & 1) ? PoseReadSpace::Model : PoseReadSpace::Local, (h1 & 2) != 0, t))
                return 0;
            putTransform(t, outF);
            return 1;
        }
        case kWrite: {
            if (h0 < 0 || !in || inCount < 10) return 0;
            return sys.setBone(e, static_cast<u32>(h0), getTransform(in), static_cast<u32>(h1), inCount > 10 ? in[10] : 1.0f) ? 1 : 0;
        }
        case kWriteRange: {
            if (h1 < 0 || !in || inCount < 10) return 0;
            const i32 count = inCount / 10;
            bool all = true;
            for (i32 k = 0; k < count; ++k)
                all = sys.setBone(e, static_cast<u32>(h1 + k), getTransform(in + 10 * k), static_cast<u32>(h0), 1.0f) && all;
            return all ? 1 : 0;
        }
        case kClear: return sys.clearBone(e, static_cast<i32>(h0)) ? 1 : 0;
        case kWeight: return (in && inCount >= 1 && sys.setWeight(e, in[0])) ? 1 : 0;
        case kEnable: return sys.setEnabled(e, h0 != 0) ? 1 : 0;
        case kOverrides: return static_cast<i32>(sys.overrideCount(e));
        default: return 0;
    }
}

} // namespace aver::anim
