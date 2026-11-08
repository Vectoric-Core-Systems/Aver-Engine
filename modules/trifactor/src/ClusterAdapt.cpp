// modules/trifactor/src/ClusterAdapt.cpp -- see ClusterAdapt.hpp for the full design rationale
// (per-LEVEL selection, why per-cluster is not buildable from today's format, what that gives up --
// and, further down, per-CLUSTER selection now that the format carries ownError/parentError).
#include "aver/trifactor/ClusterAdapt.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

namespace aver::trifactor {

f32 levelWorldErrorCm(const fmt::OcMeshData& mesh, u32 level) {
    if (level == 0) return 0.0f;   // base LOD: always full detail, error 0.0 by convention
    const u32 idx = level - 1;
    if (idx >= mesh.coarserLods.size()) return 0.0f;   // out of range: fail safe to "always in budget"
    // Inverse of the Cook's toScreenErrorThreshold (ClusterBuilder.hpp): screenErrorThreshold was
    // stored as worldErrorCm * kReferenceProjScale, so recovering worldErrorCm is one divide.
    return mesh.coarserLods[idx].screenErrorThreshold / kReferenceProjScale;
}

u32 levelTriangleCount(const fmt::OcMeshData& mesh, u32 level) {
    if (level == 0) return static_cast<u32>(mesh.indices.size() / 3);
    const u32 idx = level - 1;
    if (idx >= mesh.coarserLods.size()) return 0;
    return static_cast<u32>(mesh.coarserLods[idx].indices.size() / 3);
}

// Decodes ClusterBounds' on-disk snorm8 cone axis/cutoff into plain floats, per ClusterView's own
// documented contract (value/127.0 -> [-1,1]). Shared by every meshlet this file touches.
static void decodeMeshletBounds(const fmt::OcMeshMeshlet& m, ClusterView& out) {
    out.sphereCenter = m.sphereCenter;
    out.sphereRadius = m.sphereRadius;
    out.coneApex = m.coneApex;
    out.coneAxis = Vec3{m.coneAxis[0] / 127.0f, m.coneAxis[1] / 127.0f, m.coneAxis[2] / 127.0f};
    out.coneCutoff = static_cast<f32>(m.coneCutoff) / 127.0f;
    out.triangleCount = m.triangleCount();
}

void buildLevelClusterViews(const fmt::OcMeshData& mesh, u32 level, std::vector<ClusterView>& out) {
    const std::vector<fmt::OcMeshMeshlet>* meshlets = nullptr;
    if (level == 0) {
        meshlets = &mesh.meshlets;
    } else {
        const u32 idx = level - 1;
        if (idx >= mesh.coarserLods.size()) return;
        meshlets = &mesh.coarserLods[idx].meshlets;
    }
    out.reserve(out.size() + meshlets->size());
    for (const fmt::OcMeshMeshlet& m : *meshlets) {
        ClusterView cv;
        cv.id = static_cast<u32>(out.size());   // position in `out` -- clusters[id].id == id, as
                                                 // ClusterSelect.hpp's contract requires.
        decodeMeshletBounds(m, cv);
        // error/parents/children left at ClusterView's own defaults (0.0f / empty / empty) -- see
        // the header: LOD collapse already happened in chooseLevel, one level up. Every cluster this
        // produces trivially passes inCut.
        out.push_back(std::move(cv));
    }
}

u32 chooseLevel(const fmt::OcMeshData& mesh, const Vec3& worldSphereCenter, f32 worldSphereRadius,
                 f32 thresholdPx, const View& view) {
    const u32 levels = mesh.lodCount();
    std::vector<f32> errs(levels);
    for (u32 lvl = 0; lvl < levels; ++lvl) errs[lvl] = levelWorldErrorCm(mesh, lvl);
    return chooseLevelCached(errs, worldSphereCenter, worldSphereRadius, thresholdPx, view);
}

u32 chooseLevelCached(const std::vector<f32>& levelErrorCmTable, const Vec3& worldSphereCenter,
                       f32 worldSphereRadius, f32 thresholdPx, const View& view) {
    u32 chosen = 0;   // level 0 always qualifies (0.0f error <= any non-negative threshold)
    const u32 levels = static_cast<u32>(levelErrorCmTable.size());
    for (u32 lvl = 1; lvl < levels; ++lvl) {
        const f32 projPx = screenSpaceErrorPx(worldSphereCenter, worldSphereRadius,
                                               levelErrorCmTable[lvl], view);
        if (projPx <= thresholdPx) chosen = lvl;   // still within budget at this (coarser) level;
                                                    // keep looking for an even coarser one that also
                                                    // qualifies -- levels are assumed monotonic, so a
                                                    // simple forward scan taking the last qualifier is
                                                    // equivalent to "coarsest that still qualifies".
        // NOT a `break` on first failure: guards against a single out-of-range/malformed level (see
        // levelWorldErrorCm's fail-safe 0.0f) from prematurely stopping a scan that would otherwise
        // have found a valid coarser level beyond it. Cheap either way -- lodCount() is small.
    }
    return chosen;
}

// ---------------------------------------------------------------------------------- per-cluster ----

void buildMeshClusterViews(const fmt::OcMeshData& mesh, std::vector<MeshClusterView>& outViews,
                            std::vector<std::vector<u32>>& outIndices) {
    const u32 levels = mesh.lodCount();
    for (u32 lvl = 0; lvl < levels; ++lvl) {
        const std::vector<fmt::OcMeshMeshlet>* meshlets = nullptr;
        if (lvl == 0) {
            meshlets = &mesh.meshlets;
        } else {
            const u32 idx = lvl - 1;
            if (idx >= mesh.coarserLods.size()) continue;
            meshlets = &mesh.coarserLods[idx].meshlets;
        }
        outViews.reserve(outViews.size() + meshlets->size());
        outIndices.reserve(outIndices.size() + meshlets->size());
        for (const fmt::OcMeshMeshlet& m : *meshlets) {
            MeshClusterView cv;
            cv.sphereCenter = m.sphereCenter;
            cv.sphereRadius = m.sphereRadius;
            cv.coneApex = m.coneApex;
            cv.coneAxis = Vec3{m.coneAxis[0] / 127.0f, m.coneAxis[1] / 127.0f, m.coneAxis[2] / 127.0f};
            cv.coneCutoff = static_cast<f32>(m.coneCutoff) / 127.0f;
            cv.triangleCount = m.triangleCount();
            cv.level = lvl;
            // ownError/parentError are on-disk ScreenErrorThreshold units (worldErrorCm *
            // kReferenceProjScale -- toScreenErrorThreshold's own convention, ClusterBuilder.hpp) --
            // the same inverse levelWorldErrorCm above performs for a whole level's threshold.
            // FLT_MAX / 540.0f stays a large but perfectly finite float (~6.3e35), never inf/NaN.
            cv.ownErrorCm = m.ownError / kReferenceProjScale;
            cv.parentErrorCm = m.parentError / kReferenceProjScale;
            outViews.push_back(cv);

            std::vector<u32> idx;
            idx.reserve(m.triangles.size());
            for (u8 t : m.triangles)
                idx.push_back(t < m.vertices.size() ? m.vertices[t] : 0u);   // fail safe, not OOB
            outIndices.push_back(std::move(idx));
        }
    }
}

void buildMeshClusterGpuData(const fmt::OcMeshData& mesh, std::vector<MeshClusterView>& outBounds,
                              std::vector<GpuMeshletDesc>& outDesc, std::vector<u32>& outVertices,
                              std::vector<u32>& outTriangles) {
    const u32 levels = mesh.lodCount();
    for (u32 lvl = 0; lvl < levels; ++lvl) {
        const std::vector<fmt::OcMeshMeshlet>* meshlets = nullptr;
        if (lvl == 0) {
            meshlets = &mesh.meshlets;
        } else {
            const u32 idx = lvl - 1;
            if (idx >= mesh.coarserLods.size()) continue;
            meshlets = &mesh.coarserLods[idx].meshlets;
        }
        outBounds.reserve(outBounds.size() + meshlets->size());
        outDesc.reserve(outDesc.size() + meshlets->size());
        for (const fmt::OcMeshMeshlet& m : *meshlets) {
            // Bounds/error: byte-for-byte the same decode buildMeshClusterViews uses (same function,
            // duplicated rather than shared, because that one also builds the CPU-path's expanded
            // global index list this function deliberately does NOT build -- see the file header).
            MeshClusterView cv;
            cv.sphereCenter = m.sphereCenter;
            cv.sphereRadius = m.sphereRadius;
            cv.coneApex = m.coneApex;
            cv.coneAxis = Vec3{m.coneAxis[0] / 127.0f, m.coneAxis[1] / 127.0f, m.coneAxis[2] / 127.0f};
            cv.coneCutoff = static_cast<f32>(m.coneCutoff) / 127.0f;
            cv.triangleCount = m.triangleCount();
            cv.level = lvl;
            cv.ownErrorCm = m.ownError / kReferenceProjScale;
            cv.parentErrorCm = m.parentError / kReferenceProjScale;
            outBounds.push_back(cv);

            GpuMeshletDesc d;
            d.vertexOffset = static_cast<u32>(outVertices.size());
            d.triangleOffset = static_cast<u32>(outTriangles.size());
            d.vertexCount = static_cast<u32>(m.vertices.size());
            d.triangleCount = cv.triangleCount;
            outDesc.push_back(d);

            outVertices.insert(outVertices.end(), m.vertices.begin(), m.vertices.end());

            for (u32 t = 0; t < d.triangleCount; ++t) {
                const usize base = usize(t) * 3;
                u8 a = base + 0 < m.triangles.size() ? m.triangles[base + 0] : 0;
                u8 b = base + 1 < m.triangles.size() ? m.triangles[base + 1] : 0;
                u8 c = base + 2 < m.triangles.size() ? m.triangles[base + 2] : 0;
                // Fail safe, not out-of-bounds -- same discipline buildMeshClusterViews uses.
                if (a >= d.vertexCount) a = 0;
                if (b >= d.vertexCount) b = 0;
                if (c >= d.vertexCount) c = 0;
                outTriangles.push_back(static_cast<u32>(a) | (static_cast<u32>(b) << 8) |
                                        (static_cast<u32>(c) << 16));
            }
        }
    }
}

bool inLocalCut(const MeshClusterView& c, f32 thresholdPx, const View& view) {
    // THE BUDGET CLAMP BUG: at thresholdPx <= 0, `ownPx >= thresholdPx` is true for every cluster at
    // every level (ownPx is never negative), so the unclamped test rejects the entire DAG and the
    // whole mesh instance draws nothing -- see kMinClusterBudgetPx's own comment in ClusterAdapt.hpp.
    // Clamped HERE, the single place every caller (selectClusterLocal, selectClusterCut, and any
    // future one) funnels through, rather than at each call site.
    thresholdPx = thresholdPx > kMinClusterBudgetPx ? thresholdPx : kMinClusterBudgetPx;
    const f32 ownPx = screenSpaceErrorPx(c.sphereCenter, c.sphereRadius, c.ownErrorCm, view);
    if (ownPx >= thresholdPx) return false;   // too coarse on its own: something finer must stand in
    const f32 parentPx = screenSpaceErrorPx(c.sphereCenter, c.sphereRadius, c.parentErrorCm, view);
    // A parent that ALSO already meets budget is coarser and cheaper, and wins -- this is what keeps
    // exactly one cluster per root-to-leaf path selected. A root's parentPx is astronomically large
    // (parentErrorCm ~6.3e35), so this is always true for a root once ownPx qualifies.
    return parentPx >= thresholdPx;
}

bool selectClusterLocal(const MeshClusterView& c, f32 thresholdPx, const View& view,
                         const Frustum& frustum) {
    if (!frustum.intersectsSphere(c.sphereCenter, c.sphereRadius)) return false;
    ClusterView forCull;   // coneCull only reads apex/axis/cutoff; adapt without a full ClusterView
    forCull.coneApex = c.coneApex;
    forCull.coneAxis = c.coneAxis;
    forCull.coneCutoff = c.coneCutoff;
    if (coneCull(forCull, view.eye)) return false;
    return inLocalCut(c, thresholdPx, view);
}

ClusterCutResult selectClusterCut(const std::vector<MeshClusterView>& clusters, f32 thresholdPx,
                                   const View& view, bool useFrustum) {
    const Frustum frustum = useFrustum ? Frustum::fromViewProj(view.viewProj) : Frustum{};

    ClusterCutResult result;
    result.drawnIds.reserve(clusters.size());
    u64 levelMask = 0;

    // Same one-bucket-per-cluster discipline as ClusterSelect.hpp's selectVisibleClustersWithStats:
    // `tested == frustumCulled + coneCulled + lodRejected + drawn` holds unconditionally.
    for (u32 i = 0; i < static_cast<u32>(clusters.size()); ++i) {
        const MeshClusterView& c = clusters[i];
        ++result.stats.tested;
        if (!frustum.intersectsSphere(c.sphereCenter, c.sphereRadius)) {
            ++result.stats.frustumCulled;
            continue;
        }
        ClusterView forCull;
        forCull.coneApex = c.coneApex;
        forCull.coneAxis = c.coneAxis;
        forCull.coneCutoff = c.coneCutoff;
        if (coneCull(forCull, view.eye)) {
            ++result.stats.coneCulled;
            continue;
        }
        if (!inLocalCut(c, thresholdPx, view)) {
            ++result.stats.lodRejected;
            continue;
        }
        ++result.stats.drawn;
        result.stats.trianglesAfter += c.triangleCount;
        result.drawnIds.push_back(i);
        if (c.level < 64) levelMask |= (1ull << c.level);
    }
    result.stats.distinctLevels = static_cast<u32>(std::popcount(levelMask));
    return result;
}

// ---------------------------------------------------------------------- instance-level shortcut ----

void buildMeshClusterLevelBounds(const std::vector<MeshClusterView>& clusters,
                                  std::vector<MeshClusterLevelBounds>& outLevels) {
    outLevels.clear();
    for (const MeshClusterView& c : clusters) {
        if (c.level >= outLevels.size()) outLevels.resize(c.level + 1);
        MeshClusterLevelBounds& b = outLevels[c.level];
        if (b.count == 0) {
            b.minOwnErrorCm = b.maxOwnErrorCm = c.ownErrorCm;
            b.minParentErrorCm = b.maxParentErrorCm = c.parentErrorCm;
        } else {
            b.minOwnErrorCm = std::min(b.minOwnErrorCm, c.ownErrorCm);
            b.maxOwnErrorCm = std::max(b.maxOwnErrorCm, c.ownErrorCm);
            b.minParentErrorCm = std::min(b.minParentErrorCm, c.parentErrorCm);
            b.maxParentErrorCm = std::max(b.maxParentErrorCm, c.parentErrorCm);
        }
        b.triangleCount += c.triangleCount;
        ++b.count;
    }
}

bool provablySingleLevelCut(const std::vector<MeshClusterLevelBounds>& levels, u32 candidateLevel,
                             const Vec3& instanceSphereCenter, f32 instanceSphereRadius,
                             f32 maxClusterSphereRadius, f32 thresholdPx, const View& view) {
    if (candidateLevel >= levels.size() || levels[candidateLevel].count == 0) return false;
    // Same clamp inLocalCut applies, and for the same reason (kMinClusterBudgetPx's own comment) --
    // the envelope below must reason about the SAME threshold the real scan would have used.
    thresholdPx = thresholdPx > kMinClusterBudgetPx ? thresholdPx : kMinClusterBudgetPx;

    const f32 eyeDist = aver::dist(view.eye, instanceSphereCenter);
    const f32 dMin = std::fmax(0.0f, eyeDist - instanceSphereRadius - maxClusterSphereRadius);
    const f32 dMax = eyeDist + instanceSphereRadius;

    // Cannot bound distance away from zero: the near-zero regime screenSpaceErrorPx special-cases (a
    // 1e30 sentinel, "always refine") is not something this linear model reasons about safely --
    // refuse to prove anything and let the caller fall back to the real scan. 1.0f (one centimetre),
    // not 1e-3f: a generous margin against that sentinel's own threshold, not a tight one.
    if (dMin <= 1.0f) return false;

    const f32 ps = projScale(view);

    // `candidateLevel` must be drawn WHOLLY -- see the header derivation. Strict/non-strict boundaries
    // mirror inLocalCut's own `ownPx >= thresholdPx` / `parentPx >= thresholdPx` exactly, so this can
    // never disagree with the real scan at the margin.
    const MeshClusterLevelBounds& L = levels[candidateLevel];
    if (L.maxOwnErrorCm * ps >= thresholdPx * dMin) return false;      // worst-case own might not clear
    if (L.minParentErrorCm * ps < thresholdPx * dMax) return false;    // worst-case parent might not clear

    // Every OTHER level must be provably unable to contribute anything, at any distance in the
    // envelope -- checked directly against each level's own real data, not inferred from adjacency.
    for (u32 lvl = 0; lvl < levels.size(); ++lvl) {
        if (lvl == candidateLevel) continue;
        const MeshClusterLevelBounds& o = levels[lvl];
        if (o.count == 0) continue;
        const bool ownAlwaysTooCoarse   = o.minOwnErrorCm * ps >= thresholdPx * dMax;
        const bool parentAlwaysTooSmall = o.maxParentErrorCm * ps < thresholdPx * dMin;
        if (!ownAlwaysTooCoarse && !parentAlwaysTooSmall) return false;   // this level MIGHT contribute
    }
    return true;
}

// ---------------------------------------------------------------------------------------- group LOD spheres

namespace {

// Grows `a` to also contain (c2, r2): the standard two-sphere enclosure.
void encloseSphere(LodSphere& a, const Vec3& c2, f32 r2) {
    const Vec3 diff = c2 - a.center;
    const f32 d = diff.size();
    if (d + r2 <= a.radius) return;
    if (d + a.radius <= r2) { a.center = c2; a.radius = r2; return; }
    const f32 r = (d + a.radius + r2) * 0.5f;
    if (d > 1e-8f) a.center = a.center + diff * ((r - a.radius) / d);
    a.radius = r;
}

// A hair of slack so float rounding in the containment checks cannot flip a boundary case.
void inflate(LodSphere& s) { s.radius = s.radius * 1.00001f + 1e-5f; }

bool contains(const LodSphere& outer, const Vec3& c, f32 r) {
    const f32 tol = 1e-4f * (outer.radius + r) + 1e-4f;
    return dist(outer.center, c) + r <= outer.radius + tol;
}

const std::vector<fmt::OcMeshMeshlet>& meshletsAt(const fmt::OcMeshData& m, u32 level) {
    return level == 0 ? m.meshlets : m.coarserLods[level - 1].meshlets;
}

} // namespace

bool deriveLodSpheres(const fmt::OcMeshData& mesh, LodSphereTable& out, std::string* why) {
    out = LodSphereTable{};
    const u32 levels = mesh.lodCount();
    out.cluster.resize(levels);
    out.group.resize(levels);
    for (u32 L = 0; L < levels; ++L)
        for (const fmt::OcMeshMeshlet& m : meshletsAt(mesh, L))
            out.cluster[L].push_back({m.sphereCenter, m.sphereRadius});

    bool topology = levels > 1 && !mesh.meshlets.empty();
    for (u32 L = 1; L < levels && topology; ++L)
        if (mesh.coarserLods[L - 1].groupNodes.empty()) topology = false;
    if (!topology) return true;
    out.hasTopology = true;

    for (u32 L = 1; L < levels; ++L) {
        const fmt::OcMeshLod& lod = mesh.coarserLods[L - 1];
        const std::vector<fmt::OcMeshMeshlet>& own = lod.meshlets;
        const std::vector<fmt::OcMeshMeshlet>& kids = meshletsAt(mesh, L - 1);
        std::vector<LodSphere>& groups = out.group[L];
        groups.resize(lod.groupNodes.size());
        for (usize g = 0; g < lod.groupNodes.size(); ++g) {
            const fmt::OcMeshClusterGroup& n = lod.groupNodes[g];
            if (u64(n.ownClusterStart) + n.ownClusterCount > own.size() ||
                u64(n.childClusterStart) + n.childClusterCount > lod.groupChildren.size()) {
                if (why) *why = "LOD " + std::to_string(L) + " group " + std::to_string(g) + " has a range outside its arrays";
                return false;
            }
            LodSphere s{n.sphereCenter, n.sphereRadius};
            for (u32 k = 0; k < n.childClusterCount; ++k) {
                const u32 child = lod.groupChildren[n.childClusterStart + k];
                if (child >= kids.size()) {
                    if (why) *why = "LOD " + std::to_string(L) + " group " + std::to_string(g) + " names a child past its level";
                    return false;
                }
                encloseSphere(s, kids[child].sphereCenter, kids[child].sphereRadius);
                const LodSphere& cl = out.cluster[L - 1][child];
                encloseSphere(s, cl.center, cl.radius);
            }
            for (u32 k = 0; k < n.ownClusterCount; ++k) {
                const fmt::OcMeshMeshlet& o = own[n.ownClusterStart + k];
                encloseSphere(s, o.sphereCenter, o.sphereRadius);
            }
            inflate(s);
            groups[g] = s;
        }
        for (usize i = 0; i < own.size(); ++i) {
            const u32 gid = own[i].ownerGroupId;
            if (gid != fmt::kInvalidClusterId && gid < groups.size()) out.cluster[L][i] = groups[gid];
        }
    }
    return true;
}

bool validateLodSpheres(const fmt::OcMeshData& mesh, const LodSphereTable& t, std::string* why) {
    auto fail = [&](std::string m) { if (why) *why = std::move(m); return false; };
    const u32 levels = mesh.lodCount();
    if (t.cluster.size() != levels) return fail("table has the wrong level count");
    for (u32 L = 0; L < levels; ++L) {
        const auto& ms = meshletsAt(mesh, L);
        if (t.cluster[L].size() != ms.size()) return fail("table cluster count mismatch at LOD " + std::to_string(L));
        for (usize i = 0; i < ms.size(); ++i)
            if (!contains(t.cluster[L][i], ms[i].sphereCenter, ms[i].sphereRadius))
                return fail("LOD " + std::to_string(L) + " cluster " + std::to_string(i) + "'s lod sphere does not contain its own bounds");
    }
    if (!t.hasTopology) return true;
    for (u32 L = 1; L < levels; ++L) {
        const fmt::OcMeshLod& lod = mesh.coarserLods[L - 1];
        const auto& kids = meshletsAt(mesh, L - 1);
        for (usize g = 0; g < lod.groupNodes.size(); ++g) {
            const fmt::OcMeshClusterGroup& n = lod.groupNodes[g];
            const LodSphere& gs = t.group[L][g];
            const std::string at = "LOD " + std::to_string(L) + " group " + std::to_string(g);
            f32 groupErr = -1.0f;
            for (u32 k = 0; k < n.ownClusterCount; ++k) {
                const u32 oi = n.ownClusterStart + k;
                const fmt::OcMeshMeshlet& o = lod.meshlets[oi];
                if (!contains(gs, o.sphereCenter, o.sphereRadius)) return fail(at + " does not contain output " + std::to_string(oi));
                if (o.ownerGroupId != g) return fail(at + " output " + std::to_string(oi) + " names another owner group");
                if (groupErr < 0.0f) groupErr = o.ownError;
                else if (std::fabs(o.ownError - groupErr) > 1e-4f * std::fabs(groupErr) + 1e-6f)
                    return fail(at + " outputs disagree on error");
            }
            for (u32 k = 0; k < n.childClusterCount; ++k) {
                const u32 ci = lod.groupChildren[n.childClusterStart + k];
                const fmt::OcMeshMeshlet& c = kids[ci];
                const LodSphere& cl = t.cluster[L - 1][ci];
                if (!contains(gs, c.sphereCenter, c.sphereRadius)) return fail(at + " does not contain child " + std::to_string(ci));
                if (!contains(gs, cl.center, cl.radius)) return fail(at + " does not contain child " + std::to_string(ci) + "'s lod sphere");
                if (groupErr >= 0.0f && c.ownError > groupErr * (1.0f + 1e-4f) + 1e-6f)
                    return fail(at + " has a child with larger error than the group");
            }
        }
    }
    return true;
}

bool buildClusterDagViews(const fmt::OcMeshData& mesh, std::vector<ClusterView>& out, std::string* why) {
    LodSphereTable t;
    if (!deriveLodSpheres(mesh, t, why)) return false;
    if (!t.hasTopology) return false;
    const u32 levels = mesh.lodCount();

    std::vector<u32> base(levels + 1, 0);
    for (u32 L = 0; L < levels; ++L) base[L + 1] = base[L] + static_cast<u32>(meshletsAt(mesh, L).size());

    std::vector<ClusterView> v(base[levels]);
    for (u32 L = 0; L < levels; ++L) {
        const auto& ms = meshletsAt(mesh, L);
        for (usize i = 0; i < ms.size(); ++i) {
            ClusterView& cv = v[base[L] + i];
            cv.id = base[L] + static_cast<u32>(i);
            decodeMeshletBounds(ms[i], cv);
            cv.level = L;
            cv.lodCenter = t.cluster[L][i].center;
            cv.lodRadius = t.cluster[L][i].radius;
            cv.error = L == 0 ? 0.0f : ms[i].ownError / kReferenceProjScale;
        }
    }
    for (u32 L = 1; L < levels; ++L) {
        const fmt::OcMeshLod& lod = mesh.coarserLods[L - 1];
        for (const fmt::OcMeshClusterGroup& n : lod.groupNodes) {
            for (u32 k = 0; k < n.childClusterCount; ++k) {
                const u32 child = base[L - 1] + lod.groupChildren[n.childClusterStart + k];
                for (u32 o = 0; o < n.ownClusterCount; ++o) {
                    const u32 parent = base[L] + n.ownClusterStart + o;
                    v[child].parents.push_back(parent);
                    v[parent].children.push_back(child);
                }
            }
        }
    }
    out = std::move(v);
    return true;
}

} // namespace aver::trifactor
