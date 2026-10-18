// Pose arithmetic: rest, hierarchy resolve, skinning matrices, and the two blends.
#include "aver/anim/Pose.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>

namespace aver::anim {

namespace {

f32 clamp01(f32 v) { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); }
Vec3 lerp3(const Vec3& a, const Vec3& b, f32 t) { return a + (b - a) * t; }

bool curveLess(const PoseCurve& a, const PoseCurve& b) { return a.kind != b.kind ? a.kind < b.kind : a.id < b.id; }

// Inserts or overwrites in a sorted curve list; false when a new curve would exceed the cap.
bool upsertCurve(std::vector<PoseCurve>& v, u32 kind, u64 id, f32 value) {
    PoseCurve key;
    key.kind = kind; key.id = id;
    const auto it = std::lower_bound(v.begin(), v.end(), key, curveLess);
    if (it != v.end() && it->kind == kind && it->id == id) { it->value = value; return true; }
    if (v.size() >= kMaxPoseCurves) return false;
    key.value = value;
    v.insert(it, key);
    return true;
}

// The curve blend behind blendPose and blendPoseMasked. Both inputs are sorted; the result is their union.
// A curve only in `a`: Crossfade fades it by (1 - s), Overlay keeps it. A curve only in `b`: b * s either way
// (a lerp from 0). In both: lerp.
void blendCurves(const std::vector<PoseCurve>& a, const std::vector<PoseCurve>& b, f32 s, CurveBlend mode,
                 std::vector<PoseCurve>& out) {
    out.clear();
    usize i = 0, j = 0;
    while ((i < a.size() || j < b.size()) && out.size() < kMaxPoseCurves) {
        PoseCurve r;
        if (j >= b.size() || (i < a.size() && curveLess(a[i], b[j]))) {
            r = a[i++];
            if (mode == CurveBlend::Crossfade) r.value *= (1.0f - s);
        } else if (i >= a.size() || curveLess(b[j], a[i])) {
            r = b[j++];
            r.value *= s;
        } else {
            r = a[i];
            r.value = a[i].value + (b[j].value - a[i].value) * s;
            ++i; ++j;
        }
        out.push_back(r);
    }
}

} // namespace

void setPoseCurve(Pose& p, u32 kind, u64 id, f32 value) {
    if (!upsertCurve(p.curves, kind, id, value)) {
        static bool warned = false;
        if (!warned) { warned = true; AVER_WARN("[Anim] a pose carries more than {} curves; the extras are dropped", kMaxPoseCurves); }
    }
}

const PoseCurve* findPoseCurve(const Pose& p, u32 kind, u64 id) {
    PoseCurve key;
    key.kind = kind; key.id = id;
    const auto it = std::lower_bound(p.curves.begin(), p.curves.end(), key, curveLess);
    return (it != p.curves.end() && it->kind == kind && it->id == id) ? &*it : nullptr;
}

void restPose(const fmt::OcSkeleton& skel, Pose& out) {
    out.curves.clear();
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

bool socketModelMatrix(const std::vector<Mat4>& model, const fmt::OcSocket& sock, Mat4& out) {
    if (static_cast<usize>(sock.bone) >= model.size()) return false;
    // ROW-VECTOR, offset first: v * socket * bone, matching poseToModel's own `localM * parent`
    // exactly. Reversing these two is the classic way to get a socket that orbits the character
    // instead of sitting on it -- and it still looks like a plausible transform while doing so.
    const Transform xf{sock.translation, sock.rotation, sock.scale};
    out = xf.toMatrix() * model[static_cast<usize>(sock.bone)];
    return true;
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

void blendPose(const Pose& a, const Pose& b, f32 t, Pose& out, CurveBlend mode) {
    const bool same = a.local.size() == b.local.size();
    const f32 s = clamp01(t);
    // Built before `out` is written: the curves of a mismatched pair are `a`'s, as the bones are.
    std::vector<PoseCurve> curves;
    if (same) blendCurves(a.curves, b.curves, s, mode, curves); else curves = a.curves;
    out.local = a.local;
    out.curves = std::move(curves);
    if (!same) return;
    for (usize i = 0; i < out.local.size(); ++i) {
        out.local[i].position = lerp3(a.local[i].position, b.local[i].position, s);
        out.local[i].scale    = lerp3(a.local[i].scale,    b.local[i].scale,    s);
        out.local[i].rotation = Quat::slerp(a.local[i].rotation, b.local[i].rotation, s);
    }
}

void blendPoseMasked(const Pose& a, const Pose& b, const std::vector<f32>& w, Pose& out) {
    const bool ok = a.local.size() == b.local.size() && w.size() == a.local.size();
    std::vector<PoseCurve> curves;
    if (ok) {
        f32 s = 0.0f;
        for (const f32 x : w) s = std::fmax(s, clamp01(x));
        blendCurves(a.curves, b.curves, s, CurveBlend::Overlay, curves);
    } else {
        curves = a.curves;
    }
    out.local = a.local;
    out.curves = std::move(curves);
    if (!ok) return;
    for (usize i = 0; i < out.local.size(); ++i) {
        const f32 s = clamp01(w[i]);
        if (s <= 0.0f) continue;
        out.local[i].position = lerp3(a.local[i].position, b.local[i].position, s);
        out.local[i].scale    = lerp3(a.local[i].scale,    b.local[i].scale,    s);
        out.local[i].rotation = Quat::slerp(a.local[i].rotation, b.local[i].rotation, s);
    }
}

u32 buildBranchMask(const fmt::OcSkeleton& skel, const std::vector<BranchEntry>& entries,
                    std::vector<f32>& outWeights) {
    const usize n = skel.bones.size();
    outWeights.assign(n, 0.0f);
    std::vector<i32> entryOf(n, -1);
    u32 matched = 0;
    for (usize e = 0; e < entries.size(); ++e) {
        for (usize b = 0; b < n; ++b) {
            if (skel.bones[b].name != entries[e].bone) continue;
            entryOf[b] = static_cast<i32>(e);   // a later entry on the same bone wins
            ++matched;
            break;
        }
    }
    for (usize b = 0; b < n; ++b) {
        i32 cur = static_cast<i32>(b);
        // The hop cap keeps a malformed (cyclic) parent chain from spinning; valid skeletons end at -1.
        for (u32 k = 0; cur >= 0 && static_cast<usize>(cur) < n && k <= n; ++k) {
            if (entryOf[static_cast<usize>(cur)] >= 0) {
                const BranchEntry& en = entries[static_cast<usize>(entryOf[static_cast<usize>(cur)])];
                const f32 ramp = en.depth > 0 ? std::fmin(1.0f, static_cast<f32>(k + 1) / static_cast<f32>(en.depth))
                                              : 1.0f;
                outWeights[b] = en.weight * ramp;
                break;
            }
            cur = skel.bones[static_cast<usize>(cur)].parent;
        }
    }
    return matched;
}

void addPose(const Pose& base, const Pose& additive, const Pose& additiveRest, f32 weight, Pose& out) {
    const bool ok = additive.local.size() == base.local.size() && additiveRest.local.size() == base.local.size();
    std::vector<PoseCurve> curves = base.curves;
    if (ok) {
        const f32 cw = clamp01(weight);
        for (const PoseCurve& c : additive.curves) {
            const PoseCurve* rf = findPoseCurve(additiveRest, c.kind, c.id);
            f32 cur = 0.0f;
            for (const PoseCurve& e : curves) if (e.kind == c.kind && e.id == c.id) { cur = e.value; break; }
            upsertCurve(curves, c.kind, c.id, cur + (c.value - (rf ? rf->value : 0.0f)) * cw);
        }
    }
    out.local = base.local;
    out.curves = std::move(curves);
    if (!ok) return;
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

void boneRestBounds(const std::vector<f32>& restPositions,
                    const std::vector<u16>& joints, const std::vector<f32>& weights,
                    u32 boneCount, f32 minWeight,
                    std::vector<Vec3>& outMin, std::vector<Vec3>& outMax, std::vector<u8>& outUsed) {
    outMin.assign(boneCount, Vec3{0, 0, 0});
    outMax.assign(boneCount, Vec3{0, 0, 0});
    outUsed.assign(boneCount, 0);

    const usize n = restPositions.size() / 3;
    const usize k = fmt::kOcMeshInfluences;
    if (boneCount == 0 || n == 0 || joints.size() != n * k || weights.size() != n * k) return;

    for (usize v = 0; v < n; ++v) {
        const Vec3 p{restPositions[v * 3 + 0], restPositions[v * 3 + 1], restPositions[v * 3 + 2]};
        for (usize i = 0; i < k; ++i) {
            const f32 w = weights[v * k + i];
            const u32 j = joints[v * k + i];
            // A ZERO weight is skipped unconditionally, not merely when it falls under the
            // threshold: that is skinVertices' own rule, and the two must agree exactly or a box
            // built here can exclude a vertex the skinning still moves. With minWeight at zero the
            // two predicates are then identical, which is what makes the bound provably safe.
            if (w <= 0.0f || w < minWeight || j >= boneCount) continue;
            if (!outUsed[j]) { outMin[j] = p; outMax[j] = p; outUsed[j] = 1; continue; }
            outMin[j].x = outMin[j].x < p.x ? outMin[j].x : p.x;
            outMin[j].y = outMin[j].y < p.y ? outMin[j].y : p.y;
            outMin[j].z = outMin[j].z < p.z ? outMin[j].z : p.z;
            outMax[j].x = outMax[j].x > p.x ? outMax[j].x : p.x;
            outMax[j].y = outMax[j].y > p.y ? outMax[j].y : p.y;
            outMax[j].z = outMax[j].z > p.z ? outMax[j].z : p.z;
        }
    }
}

void posedBounds(const std::vector<Vec3>& boneMin, const std::vector<Vec3>& boneMax,
                 const std::vector<u8>& used, const Mat4* skin, u32 boneCount,
                 const Vec3& restMin, const Vec3& restMax, Vec3& outMin, Vec3& outMax, const Vec3& pad) {
    // Seeded with the REST box rather than with infinities. A vertex whose weights do not sum to
    // one is not a convex combination of its influences, and a fully unweighted one stays exactly
    // at its rest position -- both are covered by keeping the rest extent in the union. The blend-shape
    // pad widens it (and every bone box below) because deltas apply in rest space, before skinning.
    outMin = restMin - pad;
    outMax = restMax + pad;
    if (!skin || boneCount == 0) return;

    const u32 bones = boneCount < static_cast<u32>(boneMin.size())
                    ? boneCount : static_cast<u32>(boneMin.size());
    for (u32 b = 0; b < bones; ++b) {
        if (b >= used.size() || !used[b]) continue;
        const Mat4& m = skin[b];
        const Vec3 lo = boneMin[b] - pad;
        const Vec3 hi = boneMax[b] + pad;

        // All eight corners, because a rotation turns a box into something whose extent is not
        // recoverable from two transformed corners.
        for (u32 c = 0; c < 8; ++c) {
            const Vec3 p{(c & 1) ? hi.x : lo.x, (c & 2) ? hi.y : lo.y, (c & 4) ? hi.z : lo.z};
            const Vec3 t{p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
                         p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
                         p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
            outMin.x = outMin.x < t.x ? outMin.x : t.x;
            outMin.y = outMin.y < t.y ? outMin.y : t.y;
            outMin.z = outMin.z < t.z ? outMin.z : t.z;
            outMax.x = outMax.x > t.x ? outMax.x : t.x;
            outMax.y = outMax.y > t.y ? outMax.y : t.y;
            outMax.z = outMax.z > t.z ? outMax.z : t.z;
        }
    }
}

} // namespace aver::anim
