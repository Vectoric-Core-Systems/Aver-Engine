// Pose arithmetic: rest, hierarchy resolve, skinning matrices, and the two blends.
#include "aver/anim/Pose.hpp"

namespace aver::anim {

namespace {

f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
Vec3 lerp3(const Vec3& a, const Vec3& b, f32 t) { return a + (b - a) * t; }

} // namespace

void restPose(const fmt::OcSkeleton& skel, Pose& out) {
    out.local.resize(skel.bones.size());
    for (usize i = 0; i < skel.bones.size(); ++i) {
        const fmt::OcBone& b = skel.bones[i];
        out.local[i].position = b.translation;
        out.local[i].rotation = b.rotation;
        out.local[i].scale    = b.scale;
    }
}

void poseToModel(const fmt::OcSkeleton& skel, const Pose& p, std::vector<Mat4>& outModel) {
    const usize n = skel.bones.size();
    outModel.assign(n, Mat4::identity());
    if (p.local.size() != n) return;

    for (usize i = 0; i < n; ++i) {
        const Mat4 localM = p.local[i].toMatrix();
        const i32 parent = skel.bones[i].parent;
        // Parents precede children by contract, so the parent's model matrix is already final.
        // A forward or out-of-range parent would read an identity rather than corrupt memory.
        outModel[i] = (parent >= 0 && static_cast<usize>(parent) < i)
                    ? localM * outModel[static_cast<usize>(parent)]
                    : localM;
    }
}

void poseToSkinning(const fmt::OcSkeleton& skel, const Pose& p, std::vector<Mat4>& outSkin) {
    std::vector<Mat4> model;
    poseToModel(skel, p, model);
    outSkin.assign(model.size(), Mat4::identity());
    for (usize i = 0; i < model.size(); ++i) {
        Mat4 inv;
        const f32* src = skel.bones[i].inverseBind;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) inv.m[r][c] = src[r * 4 + c];
        outSkin[i] = inv * model[i];
    }
}

void blendPose(const Pose& a, const Pose& b, f32 t, Pose& out) {
    out.local = a.local;
    if (a.local.size() != b.local.size()) return;
    const f32 s = clamp01(t);
    for (usize i = 0; i < out.local.size(); ++i) {
        out.local[i].position = lerp3(a.local[i].position, b.local[i].position, s);
        out.local[i].scale    = lerp3(a.local[i].scale,    b.local[i].scale,    s);
        out.local[i].rotation = Quat::slerp(a.local[i].rotation, b.local[i].rotation, s);
    }
}

void addPose(const Pose& base, const Pose& additive, const Pose& additiveRest, f32 weight, Pose& out) {
    out.local = base.local;
    if (additive.local.size() != base.local.size() || additiveRest.local.size() != base.local.size())
        return;
    for (usize i = 0; i < out.local.size(); ++i) {
        const Transform& bs = base.local[i];
        const Transform& ad = additive.local[i];
        const Transform& rf = additiveRest.local[i];

        const f32 w = clamp01(weight);
        out.local[i].position = bs.position + (ad.position - rf.position) * w;

        // Scale is a RATIO, so its "difference" is a quotient and its weighting is a lerp from one.
        // A rest scale of zero contributes nothing rather than dividing by it.
        const Vec3 one{1, 1, 1};
        Vec3 ratio = one;
        if (rf.scale.x != 0.0f) ratio.x = ad.scale.x / rf.scale.x;
        if (rf.scale.y != 0.0f) ratio.y = ad.scale.y / rf.scale.y;
        if (rf.scale.z != 0.0f) ratio.z = ad.scale.z / rf.scale.z;
        const Vec3 k = lerp3(one, ratio, w);
        out.local[i].scale = Vec3{bs.scale.x * k.x, bs.scale.y * k.y, bs.scale.z * k.z};

        // The delta rotation is additive-relative-to-its-own-rest, applied on top of the base and
        // scaled by weight through a slerp from identity.
        const Quat invRest{-rf.rotation.x, -rf.rotation.y, -rf.rotation.z, rf.rotation.w};
        const Quat delta = (invRest * ad.rotation).normalized();
        out.local[i].rotation = (bs.rotation * Quat::slerp(Quat::identity(), delta, w)).normalized();
    }
}

} // namespace aver::anim
