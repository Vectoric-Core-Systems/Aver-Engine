// Pose arithmetic: rest, hierarchy resolve, skinning matrices, and the two blends.
#include "aver/anim/Pose.hpp"

#include <cmath>

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

void skinVertices(const std::vector<Mat4>& skin,
                  const std::vector<f32>& restPositions, const std::vector<f32>& restNormals,
                  const std::vector<u16>& joints, const std::vector<f32>& weights,
                  std::vector<f32>& outPositions, std::vector<f32>& outNormals) {
    outPositions.clear();
    outNormals.clear();

    const usize n = restPositions.size() / 3;
    const usize k = fmt::kOcMeshInfluences;
    if (n == 0 || restPositions.size() != n * 3 || restNormals.size() != n * 3) return;
    if (joints.size() != n * k || weights.size() != n * k) return;

    outPositions.resize(n * 3);
    outNormals.resize(n * 3);
    const u32 bones = static_cast<u32>(skin.size());

    for (usize v = 0; v < n; ++v) {
        const Vec3 rp{restPositions[v * 3 + 0], restPositions[v * 3 + 1], restPositions[v * 3 + 2]};
        const Vec3 rn{restNormals[v * 3 + 0], restNormals[v * 3 + 1], restNormals[v * 3 + 2]};

        Vec3 p{0, 0, 0}, nrm{0, 0, 0};
        f32  used = 0.0f;
        for (usize i = 0; i < k; ++i) {
            const f32 w = weights[v * k + i];
            const u32 j = joints[v * k + i];
            if (w == 0.0f || j >= bones) continue;
            const Mat4& m = skin[j];

            p.x += w * (rp.x * m.m[0][0] + rp.y * m.m[1][0] + rp.z * m.m[2][0] + m.m[3][0]);
            p.y += w * (rp.x * m.m[0][1] + rp.y * m.m[1][1] + rp.z * m.m[2][1] + m.m[3][1]);
            p.z += w * (rp.x * m.m[0][2] + rp.y * m.m[1][2] + rp.z * m.m[2][2] + m.m[3][2]);

            nrm.x += w * (rn.x * m.m[0][0] + rn.y * m.m[1][0] + rn.z * m.m[2][0]);
            nrm.y += w * (rn.x * m.m[0][1] + rn.y * m.m[1][1] + rn.z * m.m[2][1]);
            nrm.z += w * (rn.x * m.m[0][2] + rn.y * m.m[1][2] + rn.z * m.m[2][2]);
            used += w;
        }

        // No surviving influence means the vertex is unrigged, not at the origin.
        if (used == 0.0f) { p = rp; nrm = rn; }

        const f32 len = std::sqrt(nrm.x * nrm.x + nrm.y * nrm.y + nrm.z * nrm.z);
        if (len > 1e-8f) nrm = nrm * (1.0f / len);

        outPositions[v * 3 + 0] = p.x; outPositions[v * 3 + 1] = p.y; outPositions[v * 3 + 2] = p.z;
        outNormals[v * 3 + 0] = nrm.x; outNormals[v * 3 + 1] = nrm.y; outNormals[v * 3 + 2] = nrm.z;
    }
}

} // namespace aver::anim
