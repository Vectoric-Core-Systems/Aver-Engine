// modules/trifactor/src/ClusterAdapt.cpp -- see ClusterAdapt.hpp for the full design rationale
// (per-LEVEL selection, why per-cluster is not buildable from today's format, what that gives up).
#include "aver/trifactor/ClusterAdapt.hpp"

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

} // namespace aver::trifactor
