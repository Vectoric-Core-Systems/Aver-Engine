// Blend-shape builder, remap and merge helpers (MORPH_TARGETS.md 2.2).

#include "aver/formats/MorphTarget.hpp"

#include "aver/core/Hash.hpp"

#include <algorithm>
#include <cmath>

namespace aver::fmt {

namespace {
constexpr usize kNameMax = 128;
constexpr u32 kGone = ~0u;
}

std::string morphCurveName(std::string_view target) {
    std::string s(kOcCurvePrefixMorph);
    s.append(target);
    return s;
}

MorphTargetBuilder::MorphTargetBuilder(u32 vertexCount) : vertexCount_(vertexCount) {}

u32 MorphTargetBuilder::addTarget(std::string name, f32 defaultWeight) {
    if (targets_.size() >= kOcMeshMaxMorphTargets) return kGone;
    if (name.empty()) name = "Target_" + std::to_string(targets_.size());
    if (name.size() > kNameMax) name.resize(kNameMax);
    const auto taken = [&](const std::string& n) {
        for (const Tgt& t : targets_) if (t.name == n) return true;
        return false;
    };
    if (taken(name)) {
        const std::string base = name.size() + 12 > kNameMax ? name.substr(0, kNameMax - 12) : name;
        for (u32 k = 2;; ++k) {
            std::string cand = base + "_" + std::to_string(k);
            if (!taken(cand)) { name = std::move(cand); break; }
        }
    }
    Tgt t;
    t.name = std::move(name);
    t.defaultWeight = std::isfinite(defaultWeight) ? defaultWeight : 0.0f;
    targets_.push_back(std::move(t));
    return static_cast<u32>(targets_.size() - 1);
}

void MorphTargetBuilder::add(u32 target, u32 vertex, Vec3 dPos, const Vec3* dNrm) {
    if (target >= targets_.size() || vertex >= vertexCount_) return;
    Raw r{};
    r.vertex = vertex;
    r.dp[0] = dPos.x; r.dp[1] = dPos.y; r.dp[2] = dPos.z;
    if (dNrm) { r.dn[0] = dNrm->x; r.dn[1] = dNrm->y; r.dn[2] = dNrm->z; r.hasN = true; }
    targets_[target].raw.push_back(r);
}

void MorphTargetBuilder::setSourceMap(std::vector<u32> outVertexToSource) {
    u32 maxSrc = 0;
    for (const u32 s : outVertexToSource) maxSrc = std::max(maxSrc, s);
    srcStart_.assign(usize(maxSrc) + 2, 0);
    for (const u32 s : outVertexToSource) ++srcStart_[usize(s) + 1];
    for (usize i = 1; i < srcStart_.size(); ++i) srcStart_[i] += srcStart_[i - 1];
    srcList_.assign(outVertexToSource.size(), 0);
    std::vector<u32> cursor(srcStart_.begin(), srcStart_.end() - 1);
    for (u32 v = 0; v < outVertexToSource.size(); ++v) srcList_[cursor[outVertexToSource[v]]++] = v;
    hasSourceMap_ = true;
}

void MorphTargetBuilder::addForSource(u32 target, u32 sourceVertex, Vec3 dPos, const Vec3* dNrm) {
    if (!hasSourceMap_ || usize(sourceVertex) + 1 >= srcStart_.size()) return;
    for (u32 k = srcStart_[sourceVertex]; k < srcStart_[usize(sourceVertex) + 1]; ++k)
        add(target, srcList_[k], dPos, dNrm);
}

usize MorphTargetBuilder::finish(OcMeshData& mesh, f32 epsilonCm) {
    mesh.morphTargets.clear();
    usize dropped = 0;
    const f32 eps2 = epsilonCm * epsilonCm;
    for (Tgt& t : targets_) {
        std::stable_sort(t.raw.begin(), t.raw.end(), [](const Raw& a, const Raw& b) { return a.vertex < b.vertex; });
        OcMorphTarget out;
        out.name = std::move(t.name);
        out.defaultWeight = t.defaultWeight;
        bool anyN = false;
        // Merge duplicates (accumulate), then sparsify.
        std::vector<Raw> merged;
        merged.reserve(t.raw.size());
        for (const Raw& r : t.raw) {
            if (!merged.empty() && merged.back().vertex == r.vertex) {
                Raw& m = merged.back();
                for (int c = 0; c < 3; ++c) { m.dp[c] += r.dp[c]; m.dn[c] += r.dn[c]; }
                m.hasN = m.hasN || r.hasN;
            } else {
                merged.push_back(r);
            }
        }
        for (const Raw& r : merged) {
            const f32 p2 = r.dp[0] * r.dp[0] + r.dp[1] * r.dp[1] + r.dp[2] * r.dp[2];
            const f32 n2 = r.dn[0] * r.dn[0] + r.dn[1] * r.dn[1] + r.dn[2] * r.dn[2];
            if (p2 < eps2 && n2 < eps2) continue;
            if (!std::isfinite(p2) || !std::isfinite(n2)) continue;
            out.vertices.push_back(r.vertex);
            out.positionDeltas.insert(out.positionDeltas.end(), {r.dp[0], r.dp[1], r.dp[2]});
            out.normalDeltas.insert(out.normalDeltas.end(), {r.dn[0], r.dn[1], r.dn[2]});
            anyN = anyN || r.hasN;
        }
        if (out.vertices.empty()) { ++dropped; continue; }
        if (!anyN) out.normalDeltas.clear();
        out.finalize();
        mesh.morphTargets.push_back(std::move(out));
    }
    targets_.clear();
    if (mesh.morphTargets.empty()) mesh.flags &= ~kOcMeshHasMorph;
    else                           mesh.flags |= kOcMeshHasMorph;
    return dropped;
}

void remapMorphTargets(OcMeshData& mesh, const std::vector<u32>& oldToNew, u32 newCount) {
    for (OcMorphTarget& t : mesh.morphTargets) {
        const bool nrm = !t.normalDeltas.empty();
        struct E { u32 v; u32 old; };
        std::vector<E> keep;
        keep.reserve(t.vertices.size());
        for (usize i = 0; i < t.vertices.size(); ++i) {
            const u32 ov = t.vertices[i];
            if (ov >= oldToNew.size()) continue;
            const u32 nv = oldToNew[ov];
            if (nv == kGone || nv >= newCount) continue;
            keep.push_back({nv, static_cast<u32>(i)});
        }
        // Lower old vertex wins when two land together: entries are ascending in old order, and the sort is stable.
        std::stable_sort(keep.begin(), keep.end(), [](const E& a, const E& b) { return a.v < b.v; });
        std::vector<u32> verts;
        std::vector<f32> pd, nd;
        verts.reserve(keep.size());
        for (usize k = 0; k < keep.size(); ++k) {
            if (k > 0 && keep[k].v == keep[k - 1].v) continue;
            const usize i = keep[k].old;
            verts.push_back(keep[k].v);
            pd.insert(pd.end(), {t.positionDeltas[i * 3], t.positionDeltas[i * 3 + 1], t.positionDeltas[i * 3 + 2]});
            if (nrm) nd.insert(nd.end(), {t.normalDeltas[i * 3], t.normalDeltas[i * 3 + 1], t.normalDeltas[i * 3 + 2]});
        }
        t.vertices = std::move(verts);
        t.positionDeltas = std::move(pd);
        t.normalDeltas = std::move(nd);
        t.finalize();
    }
    if (mesh.morphTargets.empty()) mesh.flags &= ~kOcMeshHasMorph;
}

void appendMorphTargets(OcMeshData& dst, const OcMeshData& src, u32 dstVertexBase) {
    for (const OcMorphTarget& s : src.morphTargets) {
        OcMorphTarget* d = nullptr;
        for (OcMorphTarget& t : dst.morphTargets) if (t.name == s.name) { d = &t; break; }
        if (!d) {
            if (dst.morphTargets.size() >= kOcMeshMaxMorphTargets) continue;
            dst.morphTargets.emplace_back();
            d = &dst.morphTargets.back();
            d->name = s.name;
            d->defaultWeight = s.defaultWeight;
        }
        const bool sN = !s.normalDeltas.empty();
        const bool dN = !d->normalDeltas.empty();
        // Normal deltas are all-or-none per target: zeros stand in for the side that has none.
        if (sN && !dN) d->normalDeltas.assign(d->vertices.size() * 3, 0.0f);
        for (usize i = 0; i < s.vertices.size(); ++i) {
            d->vertices.push_back(s.vertices[i] + dstVertexBase);
            d->positionDeltas.insert(d->positionDeltas.end(),
                                     {s.positionDeltas[i * 3], s.positionDeltas[i * 3 + 1], s.positionDeltas[i * 3 + 2]});
            if (sN)            d->normalDeltas.insert(d->normalDeltas.end(),
                                     {s.normalDeltas[i * 3], s.normalDeltas[i * 3 + 1], s.normalDeltas[i * 3 + 2]});
            else if (dN || sN) d->normalDeltas.insert(d->normalDeltas.end(), {0.0f, 0.0f, 0.0f});
        }
        d->finalize();
    }
    if (!dst.morphTargets.empty()) dst.flags |= kOcMeshHasMorph;
}

OcCurve& addMorphCurve(OcAnimation& clip, std::string_view target, OcInterp interp, const std::vector<f32>& times,
                       const std::vector<f32>& values, const std::vector<f32>* inTan, const std::vector<f32>* outTan) {
    OcCurve c;
    c.name = morphCurveName(target);
    c.interp = interp;
    c.times = times;
    c.values = values;
    if (inTan && outTan && interp == OcInterp::CubicSpline) { c.inTangents = *inTan; c.outTangents = *outTan; }
    c.poseId = fnv1a64(target);
    if (!c.times.empty() && c.times.back() > clip.duration) clip.duration = c.times.back();
    clip.curves.push_back(std::move(c));
    return clip.curves.back();
}

} // namespace aver::fmt
