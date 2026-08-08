// modules/trifactor/src/ClusterBuilder.cpp
//
// ============================================================================================
// Trifactor slice 0: cluster formation and the crack-free LOD DAG. CPU only -- no RHI, no scene,
// no GPU. That is deliberate: this is the part that can be tested headlessly, and everything the
// GPU eventually does depends on it being right first. See docs/VIRTUALIZED_GEOMETRY.md §7.
//
// A note on how this file came to be, because it bears on how much to trust it. It was written
// blind, against meshoptimizer's documented public API, at a point when third_party/meshoptimizer
// did not exist -- so for a while the algorithm here was an argument rather than a measurement.
// meshoptimizer v1.2 has since been vendored and this file compiles and passes TrifactorTest's 42
// checks (LOD-0 coverage, the 64/124 limits, >= 2 levels, error monotonicity, MLET round-trip
// determinism, meshlet-free compatibility, and two degenerate inputs). The reasoning below about
// the crack-free invariant is therefore now backed by a run, not only by the argument.
//
// One thing the blind authoring cost, and it is worth fixing later: the grouping step below is a
// hand-rolled greedy region-grow over a shared-vertex adjacency graph, written that way because
// meshoptimizer's own meshopt_partitionClusters could not be seen at the time. v1.2 ships it
// (meshoptimizer.h:852), it solves exactly this problem, and it is very likely better than the
// heuristic here. Swapping to it is a contained change to buildLodHierarchy's grouping loop and
// nothing else -- the locked-border reasoning below does not depend on HOW clusters are grouped,
// only on all of a group's triangles landing in one merged index buffer.
// ============================================================================================
//
// ---- The crack-free invariant, and how this file actually holds it ---------------------------
//
// meshopt_simplify is documented to never move or synthesize vertex positions: an edge collapse
// remaps some vertex indices onto others that already exist in the input vertex buffer, and leaves
// the vertex buffer itself untouched. That is the property this whole file leans on. Every call
// into meshoptimizer below -- at LOD 0 and at every coarser level -- passes the SAME `mesh.positions`
// array and the SAME vertex count; no function here ever builds a per-group or per-cluster local
// vertex buffer. A "global vertex id" (an index into `mesh.positions`) therefore names the exact
// same point in space at every LOD level, forever. Two clusters -- at the same level or different
// levels -- that reference the same global vertex id are, by construction, touching at a point that
// has never moved. That is what makes the boundary-locking below sufficient rather than merely
// intended.
//
// meshopt_SimplifyLockBorder locks any edge that is used by exactly one triangle IN THE INDEX
// BUFFER IT IS GIVEN. The group-simplification step below always builds ONE merged index buffer for
// the whole group (see appendGlobalTriangles / the loop in buildLodHierarchy) before calling
// meshopt_simplify on it once. An edge between two clusters INSIDE the group is used by two
// triangles that are BOTH present in that merged buffer, so LockBorder does not lock it, and it can
// be simplified away -- interior detail is removed. An edge on the group's true outside is used by
// only one triangle in that buffer (its other side belongs to a cluster outside the group, which is
// therefore outside this buffer entirely), so LockBorder does lock it -- the group's outer boundary
// is preserved exactly, vertex-for-vertex. This is the "lock the GROUP's boundary, not each
// cluster's" distinction the task calls out: if this file instead called meshopt_simplify once per
// individual cluster with only that cluster's own triangles, every shared edge would look
// single-use from inside that call and get locked, and nothing would ever simplify.

#include "aver/trifactor/ClusterBuilder.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <unordered_map>

namespace aver::trifactor {

namespace {

// ---- Conservative snorm8 cone quantization -----------------------------------------------------
//
// The cone (apex, axis, cutoff) is a backface-style culler: a cluster can be skipped once the
// viewer is far enough around the back of its normal cone that NONE of its triangles can face the
// camera. Quantizing axis/cutoff to i8 snorm necessarily perturbs both, and the failure mode of
// perturbing WRONG is asymmetric and much worse in one direction: if the stored cone ends up
// TIGHTER than the true cone, the culler discards clusters that were actually visible -- geometry
// popping in and out -- and by the time anyone traces that back to a rounding direction in this
// function it looks like an occlusion bug or a depth bug, not a quantization bug. Discarding too FEW
// clusters only costs a little overdraw. So every rounding decision below is pushed toward
// "cull less", never "cull more":
//
//   1. axis is quantized to the nearest snorm8 direction, like any vector quantization -- but that
//      necessarily rotates the stored axis away from the true axis by some angle thetaErr. A cone
//      that is honest about direction `axis` and half-angle acos(cutoff) is no longer a superset of
//      the true visible set once the rotated axis is swapped in, unless the half-angle is widened by
//      thetaErr to compensate. So cutoff is loosened (half-angle increased, i.e. the cos value moved
//      toward -1) by thetaErr BEFORE quantizing it.
//   2. cutoff is then quantized by FLOORING toward -1, never rounding to nearest, because any
//      residual quantization error on top of the already-widened value must also fall on the
//      "wider cone" side.
//   3. the result is clamped into the representable range, and the case where the true cone cannot
//      cull anything at all (widened past a full hemisphere) is stored as -127 -- the conservative
//      "never cull" sentinel, symmetric with +127 rather than the asymmetric -128 some snorm8
//      conventions reserve.
struct QuantizedCone {
    i8 axis[3];
    i8 cutoff;
};

i8 quantizeSnorm8Floor(f32 v) {
    v = std::max(-1.0f, std::min(1.0f, v));
    return static_cast<i8>(std::floor(v * 127.0f));
}

i8 quantizeSnorm8Nearest(f32 v) {
    v = std::max(-1.0f, std::min(1.0f, v));
    return static_cast<i8>(std::lround(v * 127.0f));
}

QuantizedCone quantizeConeConservative(const Vec3& rawAxis, f32 cutoff) {
    QuantizedCone q{};

    const Vec3 a = rawAxis.getSafeNormal();
    if (a.sizeSquared() < 0.5f) {
        // Degenerate/zero axis (e.g. a near-planar-both-ways cluster): no direction is safe to cull
        // on. Store the "never cull" cone rather than guess one.
        q.axis[0] = 0; q.axis[1] = 0; q.axis[2] = 127;
        q.cutoff  = -127;
        return q;
    }

    const i8 qx = quantizeSnorm8Nearest(a.x);
    const i8 qy = quantizeSnorm8Nearest(a.y);
    const i8 qz = quantizeSnorm8Nearest(a.z);
    const Vec3 qaNorm = Vec3{qx / 127.0f, qy / 127.0f, qz / 127.0f}.getSafeNormal();

    if (qaNorm.sizeSquared() < 0.5f) {
        // Quantization collapsed the axis toward zero -- maximally conservative: cull nothing.
        q.axis[0] = qx; q.axis[1] = qy; q.axis[2] = qz;
        q.cutoff  = -127;
        return q;
    }

    const f32 thetaErr = std::acos(std::clamp(dot(a, qaNorm), -1.0f, 1.0f));
    const f32 trueHalfAngle = std::acos(std::clamp(cutoff, -1.0f, 1.0f));
    const f32 widenedHalfAngle = trueHalfAngle + thetaErr;
    const f32 widenedCutoff = (widenedHalfAngle >= kPi) ? -1.0f : std::cos(widenedHalfAngle);

    q.axis[0] = qx; q.axis[1] = qy; q.axis[2] = qz;
    q.cutoff  = std::max<i8>(quantizeSnorm8Floor(widenedCutoff), -127);
    return q;
}

// ---- grouping (task step 2a) -------------------------------------------------------------------
//
// ~4-8 clusters per group per the task; 6 is the middle of that range, 8 the hard cap.
constexpr u32 kTargetGroupSize = 6;
constexpr u32 kMaxGroupSize    = 8;

// Adjacency by SHARED VERTEX, not strictly shared edge: two clusters that share an original mesh
// vertex are adjacent, weighted by how many vertices they share. Sharing an edge implies sharing
// its two endpoint vertices, so this is a superset of "shares a boundary edge" -- it can find a
// small number of vertex-touching-but-not-edge-touching pairs a stricter test would not.
// Deliberately fine to be loose here: this adjacency is only the grouping HEURISTIC (which clusters
// get simplified together). The correctness mechanism -- what actually stays locked -- is
// meshopt_SimplifyLockBorder on the group's own merged index buffer, which is exact regardless of
// how the group was chosen (see the file-level comment).
std::vector<std::vector<std::pair<u32, u32>>> buildAdjacency(const LodDag& dag,
                                                               const std::vector<u32>& levelClusterIds) {
    std::unordered_map<u32, std::vector<u32>> vertexToClusters;
    for (u32 cid : levelClusterIds)
        for (u32 v : dag.clusters[cid].vertices) vertexToClusters[v].push_back(cid);

    std::unordered_map<u32, std::unordered_map<u32, u32>> weight; // clusterId -> neighborId -> sharedVerts
    for (auto& [vertex, clusters] : vertexToClusters) {
        for (usize i = 0; i < clusters.size(); ++i)
            for (usize j = i + 1; j < clusters.size(); ++j) {
                weight[clusters[i]][clusters[j]]++;
                weight[clusters[j]][clusters[i]]++;
            }
    }

    std::vector<std::vector<std::pair<u32, u32>>> adjacency(levelClusterIds.size());
    std::unordered_map<u32, u32> idToPos;
    for (u32 pos = 0; pos < levelClusterIds.size(); ++pos) idToPos[levelClusterIds[pos]] = pos;
    for (u32 pos = 0; pos < levelClusterIds.size(); ++pos) {
        auto it = weight.find(levelClusterIds[pos]);
        if (it == weight.end()) continue;
        for (auto& [nb, w] : it->second) adjacency[pos].push_back({nb, w});
        std::sort(adjacency[pos].begin(), adjacency[pos].end(),
                  [](auto& lhs, auto& rhs) { return lhs.second > rhs.second; });
    }
    return adjacency;
}

// Greedy region growing: start a new group at the lowest-position unvisited cluster, then
// repeatedly pull in the highest-weight unvisited neighbor of ANY cluster currently in the group,
// until the group reaches kTargetGroupSize, runs out of reachable unvisited neighbors, or hits
// kMaxGroupSize. An isolated cluster (no unvisited neighbor reachable at all) becomes its own group
// of size 1 -- meshopt_simplify on a group of 1, with its entire boundary therefore locked, is
// correctly a no-op rather than an error.
std::vector<std::vector<u32>> groupClusters(const LodDag& dag, const std::vector<u32>& levelClusterIds) {
    const auto adjacency = buildAdjacency(dag, levelClusterIds);
    const u32 n = static_cast<u32>(levelClusterIds.size());
    std::vector<bool> visited(n, false);
    std::unordered_map<u32, u32> idToPos;
    for (u32 pos = 0; pos < n; ++pos) idToPos[levelClusterIds[pos]] = pos;

    std::vector<std::vector<u32>> groups;
    for (u32 startPos = 0; startPos < n; ++startPos) {
        if (visited[startPos]) continue;
        std::vector<u32> group{levelClusterIds[startPos]};
        visited[startPos] = true;

        while (group.size() < kTargetGroupSize) {
            u32 bestPos = std::numeric_limits<u32>::max();
            u32 bestWeight = 0;
            for (u32 memberId : group) {
                for (auto& [nbId, w] : adjacency[idToPos[memberId]]) {
                    const u32 nbPos = idToPos[nbId];
                    if (!visited[nbPos] && w > bestWeight) { bestWeight = w; bestPos = nbPos; }
                }
            }
            if (bestPos == std::numeric_limits<u32>::max()) break;
            group.push_back(levelClusterIds[bestPos]);
            visited[bestPos] = true;
            if (group.size() >= kMaxGroupSize) break;
        }
        groups.push_back(std::move(group));
    }
    return groups;
}

// A cluster's triangles as GLOBAL vertex ids. dag.clusters[].triangles stores LOCAL indices into
// dag.clusters[].vertices (the MLET on-disk shape); this undoes that for callers that need a plain
// global index buffer to hand to meshopt_simplify / meshopt_buildMeshlets.
void appendGlobalTriangles(const Cluster& c, std::vector<u32>& out) {
    for (usize t = 0; t < c.triangles.size(); t += 3) {
        out.push_back(c.vertices[c.triangles[t + 0]]);
        out.push_back(c.vertices[c.triangles[t + 1]]);
        out.push_back(c.vertices[c.triangles[t + 2]]);
    }
}

// Runs meshopt_buildMeshlets + meshopt_computeMeshletBounds over `indices` (a plain global index
// buffer) and appends the resulting clusters to `dag` at `level`, returning their new ids. Shared by
// buildClusters (level 0, the source index buffer) and buildLodHierarchy (level k+1, a
// post-simplification buffer) -- both pass the SAME mesh.positions/vertexCount, which is what keeps
// vertex identity global across every level (see the file-level comment).
std::vector<u32> splitIntoClusters(const fmt::OcMeshData& mesh, const std::vector<u32>& indices,
                                    u32 level, LodDag& dag) {
    std::vector<u32> newIds;
    if (indices.empty()) return newIds;

    const usize vertexCount = mesh.positions.size() / 3;
    const usize maxMeshlets = meshopt_buildMeshletsBound(indices.size(), kMaxClusterVertices, kMaxClusterTriangles);

    std::vector<meshopt_Meshlet> meshlets(maxMeshlets);
    std::vector<u32> meshletVertices(maxMeshlets * kMaxClusterVertices);
    std::vector<u8>  meshletTriangles(maxMeshlets * kMaxClusterTriangles * 3);

    const usize meshletCount = meshopt_buildMeshlets(
        meshlets.data(), meshletVertices.data(), meshletTriangles.data(),
        indices.data(), indices.size(),
        mesh.positions.data(), vertexCount, sizeof(f32) * 3,
        kMaxClusterVertices, kMaxClusterTriangles, /*cone_weight=*/0.25f);

    newIds.reserve(meshletCount);
    for (usize m = 0; m < meshletCount; ++m) {
        const meshopt_Meshlet& mlet = meshlets[m];

        Cluster c;
        c.level = level;
        c.vertices.assign(meshletVertices.begin() + mlet.vertex_offset,
                           meshletVertices.begin() + mlet.vertex_offset + mlet.vertex_count);
        c.triangles.assign(meshletTriangles.begin() + mlet.triangle_offset,
                            meshletTriangles.begin() + mlet.triangle_offset + static_cast<usize>(mlet.triangle_count) * 3);

        const meshopt_Bounds b = meshopt_computeMeshletBounds(
            &meshletVertices[mlet.vertex_offset], &meshletTriangles[mlet.triangle_offset],
            mlet.triangle_count, mesh.positions.data(), vertexCount, sizeof(f32) * 3);

        c.bounds.sphereCenter = {b.center[0], b.center[1], b.center[2]};
        c.bounds.sphereRadius = b.radius;
        c.bounds.coneApex     = {b.cone_apex[0], b.cone_apex[1], b.cone_apex[2]};
        const QuantizedCone q = quantizeConeConservative({b.cone_axis[0], b.cone_axis[1], b.cone_axis[2]}, b.cone_cutoff);
        c.bounds.coneAxis[0] = q.axis[0];
        c.bounds.coneAxis[1] = q.axis[1];
        c.bounds.coneAxis[2] = q.axis[2];
        c.bounds.coneCutoff  = q.cutoff;

        c.id = static_cast<u32>(dag.clusters.size());
        newIds.push_back(c.id);
        dag.clusters.push_back(std::move(c));
    }

    if (dag.levels.size() <= level) dag.levels.resize(level + 1);
    for (u32 id : newIds) dag.levels[level].push_back(id);
    return newIds;
}

} // namespace

bool buildClusters(const fmt::OcMeshData& mesh, LodDag& dag, std::string* why) {
    if (mesh.positions.empty() || mesh.indices.empty()) {
        if (why) *why = "buildClusters: mesh has no positions or no indices";
        return false;
    }
    if (mesh.indices.size() % 3 != 0) {
        if (why) *why = "buildClusters: index count is not a multiple of 3";
        return false;
    }
    const u32 vertexCount = mesh.vertexCount();
    for (u32 idx : mesh.indices) {
        if (idx >= vertexCount) {
            if (why) *why = "buildClusters: index " + std::to_string(idx) + " out of range for " +
                             std::to_string(vertexCount) + " vertices";
            return false;
        }
    }

    dag = LodDag{};
    splitIntoClusters(mesh, mesh.indices, /*level=*/0, dag);
    return true;
}

bool buildLodHierarchy(const fmt::OcMeshData& mesh, LodDag& dag, std::string* why) {
    if (dag.levels.empty() || dag.levels[0].empty()) {
        if (why) *why = "buildLodHierarchy: dag has no LOD-0 clusters (call buildClusters first)";
        return false;
    }

    const usize vertexCount = mesh.positions.size() / 3;
    constexpr u32 kMaxLevels = 32; // safety cap against a non-converging loop, not an expected case

    for (u32 level = 0; level < kMaxLevels; ++level) {
        const std::vector<u32> current = dag.levels[level]; // copy: dag.levels grows below
        if (current.size() <= 1) break;                     // already a single root cluster

        const auto groups = groupClusters(dag, current);

        // Pass 1: simplify every group. Nothing is written to `dag` yet, so if NO group reduced,
        // this level can be abandoned cleanly -- `level` stays the DAG's topmost (root) level rather
        // than growing an identical, pointless level + 1 on top of it.
        struct PendingGroup {
            std::vector<u32> members;
            std::vector<u32> simplifiedIndices;
            f32 resultError = 0.0f;
        };
        std::vector<PendingGroup> pending;
        pending.reserve(groups.size());
        bool anyReduction = false;

        for (const auto& group : groups) {
            std::vector<u32> mergedIndices;
            for (u32 cid : group) appendGlobalTriangles(dag.clusters[cid], mergedIndices);

            // Target: halve the group's triangle count, floored to a whole number of triangles, with
            // a floor of 2 triangles (6 indices) so a target of zero is never asked for -- UNLESS the
            // merged group itself has fewer than 6 indices (a single locked-border triangle, which
            // happens on real assets: coastal_cliff_04's LOD hierarchy hits this), in which case that
            // floor would ask meshopt_simplify for MORE indices than the group has, tripping its
            // `target_index_count <= index_count` precondition (asserts/aborts in debug). Clamp to
            // the group's own size: such a group cannot be reduced further, so the honest target is
            // "leave it alone", not a floor that overshoots what exists.
            const usize targetIndexCount =
                std::min(mergedIndices.size(), std::max<usize>(6, (mergedIndices.size() / 2 / 3) * 3));

            std::vector<u32> simplified(mergedIndices.size());
            f32 resultError = 0.0f;
            const usize simplifiedCount = meshopt_simplify(
                simplified.data(), mergedIndices.data(), mergedIndices.size(),
                mesh.positions.data(), vertexCount, sizeof(f32) * 3,
                targetIndexCount, /*target_error=*/1e-2f,
                meshopt_SimplifyLockBorder, &resultError);
            simplified.resize(simplifiedCount);

            if (simplifiedCount < mergedIndices.size()) anyReduction = true;

            pending.push_back({group, std::move(simplified), resultError});
        }

        if (!anyReduction) break;

        // Pass 2: every group produced SOMETHING usable (even groups that individually did not
        // reduce still re-split into a valid, if unchanged, next level -- consistent DAG structure
        // matters more here than trimming one group's non-reduction as a special case).
        const u32 newLevel = level + 1;
        for (const auto& pg : pending) {
            const std::vector<u32> newIds = splitIntoClusters(mesh, pg.simplifiedIndices, newLevel, dag);

            // Error monotonicity is enforced EXPLICITLY here, not assumed from meshopt_simplify's
            // result_error: every new cluster's error is the max of (a) this group's own
            // simplification error and (b) the largest error already recorded on any child it
            // replaces. (b) alone is what makes it monotone across the DAG edge; (a) is what makes
            // it reflect the work this level actually did.
            f32 childMaxError = 0.0f;
            for (u32 cid : pg.members) childMaxError = std::max(childMaxError, dag.clusters[cid].error);
            const f32 propagatedError = std::max(pg.resultError, childMaxError);

            for (u32 parentId : newIds) {
                dag.clusters[parentId].error = propagatedError;
                for (u32 childId : pg.members) {
                    dag.clusters[parentId].children.push_back(childId);
                    dag.clusters[childId].parents.push_back(parentId);
                }
            }
        }
    }

    return true;
}

ValidationReport validateLodDag(const fmt::OcMeshData& mesh, const LodDag& dag) {
    ValidationReport report;

    if (dag.levels.empty()) {
        report.fail("structure", "dag has no levels");
        return report;
    }

    // ---- every source triangle appears in exactly one LOD-0 cluster (no gaps, no duplicates) ----
    {
        std::vector<std::array<u32, 3>> sourceTris;
        sourceTris.reserve(mesh.indices.size() / 3);
        for (usize t = 0; t + 2 < mesh.indices.size(); t += 3)
            sourceTris.push_back({mesh.indices[t], mesh.indices[t + 1], mesh.indices[t + 2]});

        std::vector<std::array<u32, 3>> clusterTris;
        for (u32 cid : dag.levels[0]) {
            const Cluster& c = dag.clusters[cid];
            for (usize t = 0; t + 2 < c.triangles.size(); t += 3)
                clusterTris.push_back({c.vertices[c.triangles[t]], c.vertices[c.triangles[t + 1]],
                                        c.vertices[c.triangles[t + 2]]});
        }

        std::sort(sourceTris.begin(), sourceTris.end());
        std::sort(clusterTris.begin(), clusterTris.end());

        if (sourceTris.size() != clusterTris.size()) {
            report.fail("lod0-coverage", "source has " + std::to_string(sourceTris.size()) +
                                              " triangles, LOD-0 clusters have " + std::to_string(clusterTris.size()));
        } else if (sourceTris != clusterTris) {
            usize mismatches = 0;
            for (usize i = 0; i < sourceTris.size(); ++i)
                if (sourceTris[i] != clusterTris[i]) ++mismatches;
            report.fail("lod0-coverage", std::to_string(mismatches) +
                                              " triangle(s) present on only one side (a gap or a duplicate)");
        }
    }

    // ---- size limits, and bounds/cone validity, over EVERY cluster at EVERY level ----------------
    for (const Cluster& c : dag.clusters) {
        if (c.vertices.size() > kMaxClusterVertices)
            report.fail("cluster-limits", "cluster " + std::to_string(c.id) + " has " +
                                               std::to_string(c.vertices.size()) + " vertices");
        if (c.triangleCount() > kMaxClusterTriangles)
            report.fail("cluster-limits", "cluster " + std::to_string(c.id) + " has " +
                                               std::to_string(c.triangleCount()) + " triangles");

        const bool sphereFinite = std::isfinite(c.bounds.sphereCenter.x) && std::isfinite(c.bounds.sphereCenter.y) &&
                                   std::isfinite(c.bounds.sphereCenter.z) && std::isfinite(c.bounds.sphereRadius) &&
                                   c.bounds.sphereRadius >= 0.0f;
        if (!sphereFinite)
            report.fail("bounds", "cluster " + std::to_string(c.id) + " has a non-finite or negative bounding sphere");

        const bool coneApexFinite = std::isfinite(c.bounds.coneApex.x) && std::isfinite(c.bounds.coneApex.y) &&
                                     std::isfinite(c.bounds.coneApex.z);
        if (!coneApexFinite) report.fail("bounds", "cluster " + std::to_string(c.id) + " has a non-finite cone apex");

        const bool coneAxisNonzero = c.bounds.coneAxis[0] != 0 || c.bounds.coneAxis[1] != 0 || c.bounds.coneAxis[2] != 0;
        const bool cutoffInRange = c.bounds.coneCutoff >= -127 && c.bounds.coneCutoff <= 127;
        if (!coneAxisNonzero || !cutoffInRange)
            report.fail("bounds", "cluster " + std::to_string(c.id) + " has an invalid cone");
    }

    // ---- error monotonic child -> parent, and acyclic, across every DAG edge ---------------------
    for (const Cluster& c : dag.clusters) {
        for (u32 parentId : c.parents) {
            const Cluster& p = dag.clusters[parentId];
            if (p.error + 1e-6f < c.error)
                report.fail("error-monotonicity", "cluster " + std::to_string(c.id) + " (error " +
                                                        std::to_string(c.error) + ") has parent " +
                                                        std::to_string(parentId) + " with smaller error " +
                                                        std::to_string(p.error));

            // Acyclic proof: every DAG edge is added by buildLodHierarchy going from level k to
            // level k+1 (see splitIntoClusters's call sites -- there are exactly two, buildClusters
            // at level 0 and buildLodHierarchy at level+1, and only the latter ever records an
            // edge). `level` is therefore a topological order by construction; checking that every
            // edge strictly increases under it, as done here, is a complete and sufficient
            // acyclicity proof (a graph with a total order that strictly increases along every edge
            // cannot contain a cycle) -- not a shortcut for a DFS that was skipped.
            if (p.level <= c.level)
                report.fail("acyclic", "cluster " + std::to_string(c.id) + " (level " + std::to_string(c.level) +
                                            ") has parent " + std::to_string(parentId) + " at level " +
                                            std::to_string(p.level) + " (not coarser)");
        }
    }

    const u32 topLevel = dag.levelCount() - 1;
    for (const Cluster& c : dag.clusters) {
        if (c.level != topLevel && c.parents.empty())
            report.fail("dag-connectivity", "non-root cluster " + std::to_string(c.id) + " (level " +
                                                 std::to_string(c.level) + ") has no parent");
    }

    return report;
}

f32 worldExtentScale(const fmt::OcMeshData& mesh) {
    const usize vertexCount = mesh.positions.size() / 3;
    if (vertexCount == 0) return 0.0f;
    return meshopt_simplifyScale(mesh.positions.data(), vertexCount, sizeof(f32) * 3);
}

f32 toScreenErrorThreshold(f32 clusterError, f32 scale) {
    const f32 absoluteErrorCm = clusterError * scale;
    return absoluteErrorCm * kReferenceProjScale;
}

bool validateScreenErrorMonotonic(const LodDag& dag, f32 scale, std::string* why) {
    for (const Cluster& c : dag.clusters) {
        const f32 cScreen = toScreenErrorThreshold(c.error, scale);
        for (u32 parentId : c.parents) {
            const Cluster& p = dag.clusters[parentId];
            const f32 pScreen = toScreenErrorThreshold(p.error, scale);
            if (pScreen + 1e-6f < cScreen) {
                if (why) *why = "cluster " + std::to_string(c.id) + " (screen error " +
                                 std::to_string(cScreen) + ") has parent " + std::to_string(parentId) +
                                 " with smaller screen error " + std::to_string(pScreen);
                return false;
            }
        }
    }
    return true;
}

bool simplifyMesh(fmt::OcMeshData& mesh, f32 ratio, std::string* why) {
    const auto fail = [&](const char* m) { if (why) *why = m; return false; };

    if (!(ratio > 0.0f) || !(ratio < 1.0f)) return fail("ratio must be strictly between 0 and 1");
    const u32 vcount = mesh.vertexCount();
    if (vcount == 0 || mesh.indices.empty() || mesh.indices.size() % 3 != 0)
        return fail("mesh has no triangles to simplify");

    const usize targetIndices = usize(f64(mesh.indices.size()) * f64(ratio)) / 3 * 3;
    if (targetIndices < 3) return fail("ratio leaves fewer than one triangle");

    // FLT_MAX rather than a small bound, deliberately: the caller asked for a triangle COUNT, and a
    // tight error bound would silently return far more triangles than requested while reporting
    // success. Letting error float and reporting what it cost is the honest shape.
    f32 resultError = 0.0f;
    std::vector<u32> out(mesh.indices.size());
    const usize got = meshopt_simplify(
        out.data(), mesh.indices.data(), mesh.indices.size(),
        mesh.positions.data(), vcount, sizeof(f32) * 3,
        targetIndices, std::numeric_limits<f32>::max(), 0, &resultError);
    out.resize(got);

    if (got < 3) return fail("the simplifier returned no triangles");
    // A simplifier that barely moved has usually hit a mesh it cannot collapse (every edge on a
    // border, or degenerate topology). Saying so beats writing a "simplified" mesh that is not.
    if (got > mesh.indices.size() * 9 / 10 && got > targetIndices * 2)
        return fail("the simplifier could not get near the requested ratio");

    // The reduced index buffer still addresses the ORIGINAL vertex array, so most of those vertices
    // are now unreferenced. Compact, or the file keeps every vertex of the source mesh and the whole
    // point -- less data -- is lost while the triangle count alone goes down.
    std::vector<u32> remap(vcount);
    const usize newVerts = meshopt_optimizeVertexFetchRemap(remap.data(), out.data(), got, vcount);

    std::vector<f32> pos(newVerts * 3), nrm, uv;
    meshopt_remapVertexBuffer(pos.data(), mesh.positions.data(), vcount, sizeof(f32) * 3, remap.data());
    if (mesh.normals.size() == usize(vcount) * 3) {
        nrm.resize(newVerts * 3);
        meshopt_remapVertexBuffer(nrm.data(), mesh.normals.data(), vcount, sizeof(f32) * 3, remap.data());
    }
    if (mesh.uvs.size() == usize(vcount) * 2) {
        uv.resize(newVerts * 2);
        meshopt_remapVertexBuffer(uv.data(), mesh.uvs.data(), vcount, sizeof(f32) * 2, remap.data());
    }
    meshopt_remapIndexBuffer(out.data(), out.data(), got, remap.data());

    // Committed only now: every step above could fail, and a half-rewritten mesh is worse than an
    // untouched one. The header promises the mesh is untouched on failure; this is where that holds.
    mesh.positions = std::move(pos);
    mesh.normals   = std::move(nrm);
    mesh.uvs       = std::move(uv);
    mesh.indices   = std::move(out);
    mesh.meshlets.clear();   // stale the moment the triangles change; rebuild after simplifying

    if (why) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%zu tris, %zu verts, error %.4f",
                      mesh.indices.size() / 3, usize(newVerts), double(resultError));
        *why = buf;
    }
    return true;
}

} // namespace aver::trifactor
