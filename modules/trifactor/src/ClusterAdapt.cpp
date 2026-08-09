// modules/trifactor/src/ClusterAdapt.cpp -- see ClusterAdapt.hpp for the full design rationale
// (per-LEVEL selection, why per-cluster is not buildable from today's format, what that gives up --
// and, further down, per-CLUSTER selection now that the format carries ownError/parentError).
#include "aver/trifactor/ClusterAdapt.hpp"

#include <bit>

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

bool inLocalCut(const MeshClusterView& c, f32 thresholdPx, const View& view) {
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

} // namespace aver::trifactor
