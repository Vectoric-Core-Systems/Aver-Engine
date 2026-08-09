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
// Two things the blind authoring cost, both since fixed and re-verified against TrifactorTest,
// not just argued:
//
//   1. Grouping used to be a hand-rolled greedy region-grow over a shared-vertex adjacency graph,
//      written that way because meshoptimizer's own meshopt_partitionClusters could not be seen at
//      the time. It now calls that function (meshoptimizer.h:852) instead -- see groupClusters'
//      own comment for what that changes (a different DAG topology) and what it does not (crack-
//      freeness, error-monotonicity).
//
//   2. Every meshopt_simplify call used to pay setup cost proportional to the WHOLE MESH's vertex
//      count on every group at every level, regardless of how few vertices the group's own merged
//      buffer actually referenced -- meshopt_SimplifySparse existed for exactly this and was never
//      set. Measured this session (see the call site in buildLodHierarchy) as ~98% of a synthetic
//      498K-triangle mesh's total buildLodHierarchy time, and a fully sufficient explanation for the
//      13m35s/8m40s real cook times a prior profiling pass reported. Fixed by setting the flag.
//      IMPORTANT: this does NOT touch the vertex-buffer-identity argument below -- the flag only
//      skips setup work sized to vertex_count; the actual simplify call still receives the SAME
//      global mesh.positions/vertexCount as before, and meshoptimizer remaps its sparse-internal
//      indices back to global ids before returning them (verified by reading simplifier.cpp), so
//      every "global vertex id" propagated by this file is exactly as global as it always was. The
//      one real side effect is that meshopt_simplify's returned `result_error` becomes relative to
//      the group's own subset extent instead of the whole mesh's (meshoptimizer.h:471's own doc)
//      -- groupExtentScale (above appendGlobalTriangles) and the rescale at the call site correct
//      for that before the error goes anywhere near worldExtentScale/toScreenErrorThreshold.
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

// meshopt_partitionClusters guarantees actual partition sizes of target..target+target/3
// (meshoptimizer.h:850) -- for kTargetGroupSize=6 that is exactly 6..8, which is where
// kMaxGroupSize=8 came from in the first place. Not a coincidence to re-derive at every call site;
// asserted once here so a change to kTargetGroupSize that silently breaks the "8 is the hard cap"
// assumption fails to compile instead of quietly producing an oversized group.
static_assert(kTargetGroupSize + kTargetGroupSize / 3 == kMaxGroupSize,
              "kMaxGroupSize documents meshopt_partitionClusters' own target..target+target/3 bound "
              "for kTargetGroupSize -- keep them in sync");

// Groups clusters for joint simplification via meshopt_partitionClusters (meshoptimizer.h:852),
// which solves exactly this problem -- it was not available when this file was first written (see
// the file-level comment) and is used here now that meshoptimizer is vendored.
//
// WHY THIS IS SAFE, and what it changes. The correctness mechanism this whole file leans on --
// meshopt_SimplifyLockBorder on ONE merged index buffer per group (see appendGlobalTriangles's call
// site in buildLodHierarchy and the file-level comment) -- depends only on which triangles end up
// in the SAME group, not on how the grouping decision was made. meshopt_partitionClusters is a
// different algorithm from the hand-rolled greedy region-grow this replaced (a proper agglomerative
// merge over a flat-array adjacency graph, plus a spatial-proximity fallback pass for otherwise
// unconnected clusters, per its own source) and DOES produce a measurably different partition on the
// same input -- verified this session (a 1854-LOD-0-cluster case grouped into 337 groups by the old
// code and 289 by this one). That changes the DAG's topology (which clusters coarsen together, level
// count, per-cluster geometry) but not its correctness: crack-freeness and error-monotonicity are
// both enforced independently of grouping choice (LockBorder on the merged buffer; the explicit
// max() in buildLodHierarchy's error propagation, respectively) -- confirmed by re-running
// TrifactorTest's full invariant suite (LOD-0 coverage, cluster limits, error monotonicity,
// acyclicity, MLET round-trip, the skinned-mesh regression) after this swap, not just argued.
//
// `mesh` is needed here (the old hand-rolled grouper did not take it) because
// meshopt_partitionClusters' spatial-fallback pass wants vertex positions, not just the shared-vertex
// topology dag.clusters[].vertices already carries.
std::vector<std::vector<u32>> groupClusters(const LodDag& dag, const std::vector<u32>& levelClusterIds,
                                             const fmt::OcMeshData& mesh) {
    const u32 n = static_cast<u32>(levelClusterIds.size());
    if (n == 0) return {};
    if (n == 1) return {{levelClusterIds[0]}};   // meshopt_partitionClusters needs no help with this

    std::vector<u32> clusterIndices;
    std::vector<u32> clusterIndexCounts(n);
    clusterIndices.reserve(n * kMaxClusterVertices);   // worst case every cluster is full
    for (u32 pos = 0; pos < n; ++pos) {
        const std::vector<u32>& verts = dag.clusters[levelClusterIds[pos]].vertices;
        clusterIndexCounts[pos] = static_cast<u32>(verts.size());
        clusterIndices.insert(clusterIndices.end(), verts.begin(), verts.end());
    }

    const usize vertexCount = mesh.positions.size() / 3;
    std::vector<u32> partitionOf(n);
    const usize partitionCount = meshopt_partitionClusters(
        partitionOf.data(), clusterIndices.data(), clusterIndices.size(),
        clusterIndexCounts.data(), n,
        mesh.positions.data(), vertexCount, sizeof(f32) * 3,
        kTargetGroupSize);

    std::vector<std::vector<u32>> groups(partitionCount);
    for (u32 pos = 0; pos < n; ++pos) groups[partitionOf[pos]].push_back(levelClusterIds[pos]);
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

// The world-extent scale (meshopt_simplifyScale's own units, see meshoptimizer.h:606) of exactly the
// DISTINCT global vertices `mergedIndices` references -- i.e. one group's own subset, not the whole
// mesh. This is what buildLodHierarchy's meshopt_simplify call below now measures its "relative"
// error against once meshopt_SimplifySparse is set (its doc: "error becomes relative to subset
// extents", meshoptimizer.h:471), because meshopt_SimplifySparse's internal sparse_remap collapses
// the effective vertex set to exactly this same distinct-referenced-vertex set before computing the
// scale (verified by reading simplifier.cpp: buildSparseRemap at line 242 produces the identical set
// this function recomputes, and rescalePositions at line 549 takes its min/max over exactly that set
// -- a bounding-box extent, so recomputing it from an unordered copy of the same positions is exact,
// not approximate). A caller needs this to convert that per-group-relative result_error back into
// the whole-mesh-relative units Cluster::error is documented to hold (ClusterBuilder.hpp) -- see the
// call site.
f32 groupExtentScale(const fmt::OcMeshData& mesh, const std::vector<u32>& mergedIndices) {
    std::vector<u32> unique(mergedIndices);
    std::sort(unique.begin(), unique.end());
    unique.erase(std::unique(unique.begin(), unique.end()), unique.end());

    std::vector<f32> positions(unique.size() * 3);
    for (usize i = 0; i < unique.size(); ++i) {
        const u32 v = unique[i];
        positions[i * 3 + 0] = mesh.positions[usize(v) * 3 + 0];
        positions[i * 3 + 1] = mesh.positions[usize(v) * 3 + 1];
        positions[i * 3 + 2] = mesh.positions[usize(v) * 3 + 2];
    }
    return meshopt_simplifyScale(positions.data(), positions.size() / 3, sizeof(f32) * 3);
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

    // Computed ONCE for the whole hierarchy (this is worldExtentScale(mesh) -- the same public
    // function ConvertTool calls after this returns), and reused below to rescale every group's
    // meshopt_SimplifySparse-relative error back into the whole-mesh-relative units Cluster::error is
    // documented to hold. See the meshopt_simplify call site below for why that rescale exists at
    // all.
    const f32 meshScale = worldExtentScale(mesh);

    for (u32 level = 0; level < kMaxLevels; ++level) {
        const std::vector<u32> current = dag.levels[level]; // copy: dag.levels grows below
        if (current.size() <= 1) break;                     // already a single root cluster

        const auto groups = groupClusters(dag, current, mesh);

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
            // Sized once instead of letting push_back inside appendGlobalTriangles grow it by
            // doubling: this loop runs once per group at every level (thousands of times across a
            // real hierarchy), and with the O(vertex_count) meshopt_simplify cost above gone, this
            // reallocation churn stopped being invisible. The exact total is known up front -- every
            // member cluster's triangles all land in this one merged buffer -- so there's nothing
            // approximate about sizing to it exactly.
            usize mergedTriIndices = 0;
            for (u32 cid : group) mergedTriIndices += dag.clusters[cid].triangles.size();
            std::vector<u32> mergedIndices;
            mergedIndices.reserve(mergedTriIndices);
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

            // meshopt_SimplifySparse: WITHOUT this flag, meshopt_simplify pays setup cost
            // (buildPositionRemap's vertex hash table, vertex_kind/quadric/loop buffers, ...)
            // proportional to `vertexCount` -- the WHOLE MESH's vertex count -- on EVERY group at
            // EVERY level, even though a group's own merged buffer references only a few hundred of
            // them at most (kMaxGroupSize*kMaxClusterVertices = 512). Measured this session with an
            // isolated single-call comparison (one real 578-triangle/338-referenced-vertex group,
            // simplified repeatedly against a padded vertex buffer of growing total size): the
            // current-shape call's cost scaled from 0.05ms to 35.08ms as the MESH's vertex count grew
            // from 1,000 to 1,000,000 for the IDENTICAL group, while meshopt_SimplifySparse held flat
            // at 0.03-0.04ms throughout -- i.e. this flag alone turns an O(whole-mesh-vertex-count)
            // cost that is repeated thousands of times across a hierarchy into an O(group-size) cost
            // paid once per group. See groupExtentScale's comment just above appendGlobalTriangles
            // for the one thing this flag changes that this call site has to correct for.
            std::vector<u32> simplified(mergedIndices.size());
            f32 resultError = 0.0f;
            const usize simplifiedCount = meshopt_simplify(
                simplified.data(), mergedIndices.data(), mergedIndices.size(),
                mesh.positions.data(), vertexCount, sizeof(f32) * 3,
                targetIndexCount, /*target_error=*/1e-2f,
                meshopt_SimplifyLockBorder | meshopt_SimplifySparse, &resultError);
            simplified.resize(simplifiedCount);

            if (simplifiedCount < mergedIndices.size()) anyReduction = true;

            // meshopt_SimplifySparse makes `resultError` relative to THIS GROUP's own subset extent
            // (meshoptimizer.h:471's own doc: "error becomes relative to subset extents"), not the
            // whole mesh's -- rescale it back to whole-mesh-relative units (what every OTHER path
            // that touches Cluster::error assumes -- worldExtentScale/toScreenErrorThreshold,
            // ClusterBuilder.hpp's own comment on the field) before it is compared against another
            // group's error or propagated to a parent. Absolute error is scale-invariant (subset
            // extent * subset-relative error == mesh extent * mesh-relative error, both being the
            // same physical distance), so this is an exact unit conversion, not an approximation.
            const f32 groupScale = groupExtentScale(mesh, mergedIndices);
            const f32 rescaledError = (meshScale > 0.0f) ? resultError * (groupScale / meshScale) : resultError;

            pending.push_back({group, std::move(simplified), rescaledError});
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

std::vector<ClusterErrorBounds> computeClusterErrorBounds(const LodDag& dag, f32 scale) {
    std::vector<ClusterErrorBounds> out(dag.clusters.size());
    if (dag.clusters.empty()) return out;

    // Same "root" definition validateLodDag and LodDag's own doc comment use -- NOT
    // c.parents.empty(), so a dag-connectivity bug (a non-root cluster wrongly missing a parent)
    // surfaces as an ownError > parentError violation below instead of silently matching the root
    // sentinel. See this function's header comment.
    const u32 topLevel = dag.levelCount() > 0 ? dag.levelCount() - 1 : 0;

    for (const Cluster& c : dag.clusters) {
        ClusterErrorBounds& b = out[c.id];
        b.ownError = toScreenErrorThreshold(c.error, scale);

        if (c.level == topLevel) {
            b.parentError = std::numeric_limits<f32>::max();
            continue;
        }

        // MAX over every parent, not parents[0] -- see this function's header comment for why that
        // matters even though, today, every parent from the same simplified group carries the exact
        // same propagatedError.
        f32 maxParentError = 0.0f;
        for (u32 parentId : c.parents)
            maxParentError = std::max(maxParentError, toScreenErrorThreshold(dag.clusters[parentId].error, scale));
        b.parentError = maxParentError;
    }
    return out;
}

bool validateClusterErrorBounds(const LodDag& dag, const std::vector<ClusterErrorBounds>& bounds, std::string* why) {
    if (bounds.size() != dag.clusters.size()) {
        if (why) *why = "validateClusterErrorBounds: bounds.size() (" + std::to_string(bounds.size()) +
                         ") does not match dag.clusters.size() (" + std::to_string(dag.clusters.size()) +
                         ") -- bounds must come from computeClusterErrorBounds(dag, ...) for this same dag";
        return false;
    }

    const u32 topLevel = dag.levelCount() > 0 ? dag.levelCount() - 1 : 0;
    constexpr f32 kFltMax = std::numeric_limits<f32>::max();

    for (const Cluster& c : dag.clusters) {
        const ClusterErrorBounds& b = bounds[c.id];

        // The local test's whole soundness argument: a cluster the test would ever draw is fine
        // enough on its own (ownError < budget) and its parent is NOT (parentError < budget is what
        // the test rejects), so ownError <= parentError is what keeps every budget's cut a genuine
        // partition of the surface rather than one with holes.
        if (b.ownError > b.parentError + 1e-6f) {
            if (why) *why = "cluster " + std::to_string(c.id) + " (level " + std::to_string(c.level) +
                             ") has ownError " + std::to_string(b.ownError) + " greater than parentError " +
                             std::to_string(b.parentError) +
                             " -- the local cut test's coverage guarantee requires ownError <= parentError "
                             "for every cluster";
            return false;
        }

        const bool isRoot = (c.level == topLevel);
        if (isRoot && b.parentError != kFltMax) {
            if (why) *why = "root cluster " + std::to_string(c.id) + " has a FINITE parentError " +
                             std::to_string(b.parentError) +
                             " -- a root must have parentError == FLT_MAX or it silently stops drawing "
                             "at distance once no finer cluster qualifies";
            return false;
        }
        if (!isRoot && b.parentError == kFltMax) {
            if (why) *why = "non-root cluster " + std::to_string(c.id) + " (level " + std::to_string(c.level) +
                             ") has an INFINITE parentError -- only the root level may, or this cluster and "
                             "its ancestor can both be drawn at the same budget";
            return false;
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

    // THE SKIN STREAMS, and forgetting them made this function unusable on any rigged asset.
    // positions/normals/uvs were remapped and resized to newVerts while joints/weights kept the OLD
    // vertex count, so OcMeshData::hasSkin() -- which requires both to be exactly v*4 for the NEW v --
    // went false, valid() failed, and writeOcMesh refused to save the mesh at all. The visible
    // symptom was a successful "simplified to 50%" line followed by "mesh has no vertices, no
    // indices, or mismatched attribute counts", which points at everything except the real cause.
    //
    // No blending is needed and none would be correct. meshopt_optimizeVertexFetchRemap does not
    // merge vertices -- meshopt_simplify already did that, by rewriting the INDEX buffer -- it only
    // compacts away the vertices no surviving triangle references. Every destination vertex
    // therefore comes from exactly one source vertex, so its influences carry across unchanged.
    // Averaging weights here would corrupt a rig that the remap reproduces exactly.
    std::vector<u16> jnt;
    std::vector<f32> wgt;
    if (mesh.hasSkin()) {
        jnt.resize(newVerts * fmt::kOcMeshInfluences);
        wgt.resize(newVerts * fmt::kOcMeshInfluences);
        meshopt_remapVertexBuffer(jnt.data(), mesh.joints.data(), vcount,
                                  sizeof(u16) * fmt::kOcMeshInfluences, remap.data());
        meshopt_remapVertexBuffer(wgt.data(), mesh.weights.data(), vcount,
                                  sizeof(f32) * fmt::kOcMeshInfluences, remap.data());
    }

    meshopt_remapIndexBuffer(out.data(), out.data(), got, remap.data());

    // Committed only now: every step above could fail, and a half-rewritten mesh is worse than an
    // untouched one. The header promises the mesh is untouched on failure; this is where that holds.
    mesh.positions = std::move(pos);
    mesh.normals   = std::move(nrm);
    mesh.uvs       = std::move(uv);
    // Both, together, or neither: hasSkin() is an all-or-nothing predicate, and a mesh carrying
    // joints without matching weights is exactly the invalid state this bug produced.
    mesh.joints    = std::move(jnt);
    mesh.weights   = std::move(wgt);
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
