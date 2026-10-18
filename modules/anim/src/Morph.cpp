// CPU blend-shape reference. See Morph.hpp.
#include "aver/anim/Morph.hpp"

#include <algorithm>
#include <cmath>

namespace aver::anim {

void applyMorphTargets(const std::vector<fmt::OcMorphTarget>& targets, const MorphTargetWeight* w, u32 n,
                       const std::vector<f32>& restPositions, const std::vector<f32>& restNormals,
                       std::vector<f32>& outPositions, std::vector<f32>& outNormals) {
    outPositions.clear();
    outNormals.clear();
    const usize vc = restPositions.size() / 3;
    if (restPositions.size() != vc * 3 || restNormals.size() != vc * 3) return;
    for (u32 i = 0; i < n; ++i) if (w[i].target >= targets.size()) return;

    outPositions = restPositions;
    outNormals = restNormals;
    if (n == 0) return;

    for (u32 i = 0; i < n; ++i) {
        const fmt::OcMorphTarget& t = targets[w[i].target];
        const f32 wt = w[i].weight;
        const bool nrm = !t.normalDeltas.empty();
        for (usize k = 0; k < t.vertices.size(); ++k) {
            const usize v = t.vertices[k];
            if (v >= vc) continue;
            for (int c = 0; c < 3; ++c) {
                outPositions[v * 3 + c] += wt * t.positionDeltas[k * 3 + c];
                if (nrm) outNormals[v * 3 + c] += wt * t.normalDeltas[k * 3 + c];
            }
        }
    }
    for (usize v = 0; v < vc; ++v) {
        f32* nv = &outNormals[v * 3];
        const f32 l = std::sqrt(nv[0] * nv[0] + nv[1] * nv[1] + nv[2] * nv[2]);
        if (l > 1e-8f) { nv[0] /= l; nv[1] /= l; nv[2] /= l; }
    }
}

Vec3 morphBoundsPad(const std::vector<fmt::OcMorphTarget>& targets, const MorphTargetWeight* w, u32 n) {
    Vec3 pad{0, 0, 0};
    for (u32 i = 0; i < n; ++i) {
        if (w[i].target >= targets.size()) continue;
        const fmt::OcMorphTarget& t = targets[w[i].target];
        const f32 a = std::fabs(w[i].weight);
        pad.x += a * t.maxAbs[0];
        pad.y += a * t.maxAbs[1];
        pad.z += a * t.maxAbs[2];
    }
    return pad;
}

u32 selectActiveTargets(const f32* maxLen, u32 targetCount, const MorphTargetWeight* in, u32 n,
                        MorphTargetWeight* out) {
    struct C { u32 target; f32 weight; f32 importance; };
    std::vector<C> keep;
    keep.reserve(n);
    for (u32 i = 0; i < n; ++i) {
        const MorphTargetWeight& e = in[i];
        if (e.target >= targetCount || !(std::fabs(e.weight) >= kMorphActiveEps)) continue;
        const f32 imp = std::fabs(e.weight) * maxLen[e.target];
        if (!(imp > 0.0f)) continue;
        keep.push_back({e.target, e.weight, imp});
    }
    // A repeated target: the later entry wins.
    std::stable_sort(keep.begin(), keep.end(), [](const C& a, const C& b) { return a.target < b.target; });
    usize w = 0;
    for (usize r = 0; r < keep.size(); ++r) {
        if (r + 1 < keep.size() && keep[r + 1].target == keep[r].target) continue;
        keep[w++] = keep[r];
    }
    keep.resize(w);
    if (keep.size() > kMorphMaxActive) {
        std::stable_sort(keep.begin(), keep.end(), [](const C& a, const C& b) { return a.importance > b.importance; });
        keep.resize(kMorphMaxActive);
        std::sort(keep.begin(), keep.end(), [](const C& a, const C& b) { return a.target < b.target; });
    }
    for (usize i = 0; i < keep.size(); ++i) out[i] = MorphTargetWeight{keep[i].target, keep[i].weight};
    return static_cast<u32>(keep.size());
}

} // namespace aver::anim
