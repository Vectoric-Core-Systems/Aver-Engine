// modules/trifactor/src/ClusterBuilder.cpp
//
// ============================================================================================
// Trifactor slice 0: cluster formation and the crack-free LOD DAG. CPU only -- no RHI, no scene,
// no GPU by design: this is the part that can be tested headlessly, and the GPU work depends on it
// being right first. See docs/VIRTUALIZED_GEOMETRY.md §7.
//
// Written before third_party/meshoptimizer was vendored, so early reasoning here was argument, not
// measurement; now backed (meshoptimizer v1.2 vendored) by TrifactorTest's 42 checks (LOD-0
// coverage, 64/124 limits, >=2 levels, error monotonicity, MLET round-trip determinism,
// meshlet-free compatibility, two degenerate inputs). Three things that blind authoring got wrong:
//
//   1. Grouping now calls meshopt_partitionClusters (meshoptimizer.h:852) instead of a hand-rolled
//      greedy region-grow -- see groupClusters for what changes (DAG topology) and what does not
//      (crack-freeness, error-monotonicity).
//
//   2. meshopt_simplify paid setup cost proportional to the WHOLE MESH's vertex count on every group
//      at every level; meshopt_SimplifySparse skips it. Measured ~98% of a synthetic 498K-triangle
//      mesh's buildLodHierarchy time (see buildLodHierarchy) -- explains previously-reported
//      13m35s/8m40s cook times. Vertex identity is unchanged: simplify still gets the SAME global
//      mesh.positions/vertexCount, and meshoptimizer remaps sparse-internal indices back to global
//      ids before returning (verified in simplifier.cpp). The one real effect: `result_error`
//      becomes relative to the group's own subset extent, not the whole mesh's (meshoptimizer.h:471)
//      -- groupExtentScale and the call-site rescale correct for that before it reaches
//      toScreenErrorThreshold.
//
//   3. meshopt_SimplifyLockBorder was unconditional, correct for solid meshes but flattening a
//      foliage mesh's ladder to nearly nothing (thousands of leaf cards, almost entirely boundary
//      edge -- numbers at buildLodHierarchy). Fixed by ROUTING per group instead of removing the flag
//      (removal breaks five solid meshes): computeShellIds classifies the mesh into connected
//      shells; buildClusters (step 5) splits LOD-0 triangles into a small-shell stream (direct, one
//      cluster per shell) and a large-shell stream (ordinary meshopt_buildMeshlets, order-preserving);
//      groupClusters (step 6) partitions each stream separately so a group never mixes them;
//      buildLodHierarchy (step 7) drops LockBorder only for a group entirely small-shell lineage,
//      which PendingGroup carries forward every level (step 8). See Cluster::smallShellLineage
//      (ClusterBuilder.hpp) and the meshopt_simplify call site for the corpus numbers.
// ============================================================================================
//
// ---- The crack-free invariant, and how this file holds it -------------------------------------
//
// meshopt_simplify never moves or synthesizes vertex positions: a collapse remaps indices onto
// others already in the input vertex buffer, leaving the buffer untouched. Every meshoptimizer call
// below -- LOD 0 and every coarser level -- passes the SAME mesh.positions array and vertex count,
// never a per-group or per-cluster local buffer. So a "global vertex id" names the same point in
// space at every level, forever, and two clusters sharing one are touching at a point that has never
// moved -- what makes the boundary-locking below sufficient.
//
// meshopt_SimplifyLockBorder locks any edge used by exactly one triangle IN THE BUFFER IT IS GIVEN.
// Group simplification builds ONE merged index buffer per group (appendGlobalTriangles / the loop in
// buildLodHierarchy) before calling meshopt_simplify once: an edge between two clusters INSIDE the
// group is used by two triangles both in that buffer, so LockBorder does not lock it and interior
// detail simplifies away; an edge on the group's true outside is single-use in that buffer (its
// other side is outside the group entirely), so LockBorder locks it and the boundary is preserved
// exactly. This is "lock the GROUP's boundary, not each cluster's": simplifying per individual
// cluster would make every shared edge look single-use and lock everything.

#include "aver/trifactor/ClusterBuilder.hpp"
#include "aver/core/Log.hpp"

#include <chrono>

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
// The cone (apex, axis, cutoff) is a backface-style culler: skip a cluster once the viewer is far
// enough around the back of its normal cone that NONE of its triangles can face the camera. The
// runtime test (aver::trifactor::coneCull / the GPU port clusterConeCull, both PORTED from
// meshoptimizer.h's documented formula, verified in ClusterSelectTest.cpp) culls iff
// `dot(dirToApexFromEye, axis) >= cutoff`: +1.0f is "never cull", -1.0f "cull from everywhere"
// (ClusterSelect.hpp's file header). An earlier version had this backwards, backface-culling every
// degenerate/hemisphere-exceeding cluster from EVERY direction instead of none (see the commit that
// fixed it for the write-up).
//
// Quantizing axis/cutoff to i8 snorm perturbs both, and perturbing WRONG is asymmetric: a cone too
// LARGE discards clusters that were actually visible (popping, or a whole mesh vanishing) and reads
// like an occlusion/depth bug, not a quantization one; too SMALL only costs overdraw. So every
// rounding below is pushed toward shrinking the cull region (cutoff -> +1), never toward -1:
//
//   1. axis quantizes to the nearest snorm8 direction, rotating it from the true axis by some angle
//      thetaErr; cutoff is TIGHTENED by thetaErr BEFORE quantizing (triangle inequality on the
//      sphere) so the rotated cone stays a subset of the true one.
//   2. cutoff is then quantized by CEILING toward +1, never rounded to nearest, so residual error
//      also falls on the "smaller cull region" side.
//   3. clamped into range; if thetaErr alone consumes the whole true half-angle, store +127 -- the
//      "never cull" sentinel, symmetric with -127 rather than the asymmetric -128 some snorm8
//      conventions reserve.
struct QuantizedCone {
    i8 axis[3];
    i8 cutoff;
};

i8 quantizeSnorm8Ceil(f32 v) {
    v = std::max(-1.0f, std::min(1.0f, v));
    return static_cast<i8>(std::ceil(v * 127.0f));
}

i8 quantizeSnorm8Nearest(f32 v) {
    v = std::max(-1.0f, std::min(1.0f, v));
    return static_cast<i8>(std::lround(v * 127.0f));
}

QuantizedCone quantizeConeConservative(const Vec3& rawAxis, f32 cutoff) {
    QuantizedCone q{};

    const Vec3 a = rawAxis.getSafeNormal();
    if (a.sizeSquared() < 0.5f) {
        // Degenerate/zero axis (e.g. near-planar-both-ways cluster): no direction is safe to cull on,
        // so store "never cull" rather than guess. +127, not -127: see header comment, point 3.
        q.axis[0] = 0; q.axis[1] = 0; q.axis[2] = 127;
        q.cutoff  = 127;
        return q;
    }

    const i8 qx = quantizeSnorm8Nearest(a.x);
    const i8 qy = quantizeSnorm8Nearest(a.y);
    const i8 qz = quantizeSnorm8Nearest(a.z);
    const Vec3 qaNorm = Vec3{qx / 127.0f, qy / 127.0f, qz / 127.0f}.getSafeNormal();

    if (qaNorm.sizeSquared() < 0.5f) {
        // Quantization collapsed the axis toward zero -- cull nothing. +127, not -127: header
        // comment, point 3.
        q.axis[0] = qx; q.axis[1] = qy; q.axis[2] = qz;
        q.cutoff  = 127;
        return q;
    }

    const f32 thetaErr = std::acos(std::clamp(dot(a, qaNorm), -1.0f, 1.0f));
    const f32 trueHalfAngle = std::acos(std::clamp(cutoff, -1.0f, 1.0f));
    // Shrink, not widen: header comment, point 1. thetaErr can exceed trueHalfAngle outright (tight
    // cone + large axis error) -- the "no safe margin" case from point 3, handled like the
    // degenerate-axis branches above: store the sentinel rather than a negative half-angle.
    const f32 shrunkHalfAngle = trueHalfAngle - thetaErr;
    const f32 shrunkCutoff = (shrunkHalfAngle <= 0.0f) ? 1.0f : std::cos(shrunkHalfAngle);

    q.axis[0] = qx; q.axis[1] = qy; q.axis[2] = qz;
    q.cutoff  = std::min<i8>(quantizeSnorm8Ceil(shrunkCutoff), 127);
    return q;
}

// ---- sphere-of-spheres merge (Stage 4, ClusterGroupNode::sphereCenter/sphereRadius) ------------
//
// Grows (center, radius) -- seeded with the FIRST child's own sphere -- to also fully contain (c2,
// r2): the standard "smallest sphere enclosing two spheres" construction. One sphere already inside
// the other makes the merge a no-op; otherwise the new sphere sits on the segment joining the two
// centres, touching the FAR side of each input, so it provably contains both in full.
//
// NOT the minimal bounding sphere of a point set (needs Welzl's algorithm); ClusterGroupNode::
// sphereCenter requires only CONTAINMENT (too big costs overdraw, too SMALL drops visible geometry).
// At most kMaxGroupSize-1 (7) sequential two-sphere merges; containment composes across them.
void mergeSphere(Vec3& center, f32& radius, const Vec3& c2, f32 r2) {
    const Vec3 diff = c2 - center;
    const f32 d = diff.size();
    if (d + r2 <= radius) return;                                   // c2's sphere already lies inside
    if (d + radius <= r2) { center = c2; radius = r2; return; }      // this sphere lies inside c2's

    const f32 newRadius = (d + radius + r2) * 0.5f;
    // Move `center` toward `c2` by (newRadius - radius). d > 1e-8f is guaranteed: a near-zero d with
    // differing radii would already have hit one of the two early-outs above, so reaching here with
    // near-degenerate `diff` means the radii were near-equal too and no move is needed -- the guard
    // just keeps the divide well-defined.
    if (d > 1e-8f) center = center + diff * ((newRadius - radius) / d);
    radius = newRadius;
}

// ---- connected-shell classification (task steps 3-4) -------------------------------------------
//
// WHY: meshopt_SimplifyLockBorder locks any edge used by exactly one triangle in its buffer. On a
// SOLID (one-shell) mesh that correctly holds the boundary; a FOLIAGE mesh is hundreds of
// DISCONNECTED shells (one sheet per leaf/needle card), so almost every edge looks single-use and
// nothing collapses: LockBorder unconditional measured fir_sapling's coarsest at 393,157 of 433,021
// LOD-0 triangles (1.1x ladder). Dropping the flag fixes that (13,121x) but breaks five solid,
// single-shell meshes (file header). Fix must be PER SHELL: keep LockBorder for shells too big to fit
// one meshlet (a group boundary can genuinely cut through one), drop it for shells too small to ever
// be split by a group boundary (LodDag::isSmallShell).
//
// THIS SECTION ONLY COMPUTES WHICH IS WHICH; buildClusters/buildLodHierarchy still run unconditionally
// (Cluster::shellId's STAGE STATUS comment): step 1 found one of the five previously-regressed meshes
// (rock_moss_set_02) is NOT single-shell -- seven independently-large shells, each still clearing the
// "large" bar so routing would be safe for it, but the premise was not universally true as assumed.
// Routing (steps 5-8) stops here for this stage.

// Union-find over `count` elements, path-halved on find(), no rank/size heuristic. `count` is at
// most a mesh's vertex count (a few hundred thousand at the outside) and this runs ONCE per mesh, at
// LOD-0 build time, not per level or per group -- a plain compressing find is fast enough, and
// simplicity matters more than shaving a one-off pass.
struct UnionFind {
    std::vector<u32> parent;
    explicit UnionFind(usize count) : parent(count) {
        for (usize i = 0; i < count; ++i) parent[i] = static_cast<u32>(i);
    }
    u32 find(u32 x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];   // path halving
            x = parent[x];
        }
        return x;
    }
    void unite(u32 a, u32 b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[a] = b;
    }
};

// Answers a more useful question than computeShellIds: not "how many shells" but what FRACTION of
// edges are open (LockBorder has no notion of "shell", only single-use-triangle edges). A leaf card
// is four perimeter edges around two triangles, almost entirely boundary even in a large component; a
// rock is a closed solid, almost nothing boundary, so LockBorder costs it nothing.
//
// Edges are canonicalised through the SAME meshopt_generatePositionRemap computeShellIds uses: a UV
// seam shares a POSITION but not an index, and counting raw indices would call that edge open twice
// instead of closed once, inflating this statistic most on the assets that matter. (An early corpus
// pass misread this as "zero small shells everywhere" from a row-alignment bug -- see
// Cluster::shellId's comment for the corrected numbers steps 5-8 are built on.)
f32 openEdgeFraction(const fmt::OcMeshData& mesh, u64& outOpen, u64& outTotal) {
    outOpen = outTotal = 0;
    const usize vertexCount = mesh.positions.size() / 3;
    if (vertexCount == 0 || mesh.indices.size() < 3) return 0.0f;

    std::vector<u32> remap(vertexCount);
    meshopt_generatePositionRemap(remap.data(), mesh.positions.data(), vertexCount, sizeof(f32) * 3);

    std::unordered_map<u64, u32> edgeUse;
    edgeUse.reserve(mesh.indices.size());
    for (usize t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const u32 v[3] = {remap[mesh.indices[t + 0]], remap[mesh.indices[t + 1]], remap[mesh.indices[t + 2]]};
        for (u32 e = 0; e < 3; ++e) {
            u32 a = v[e], b = v[(e + 1) % 3];
            if (a == b) continue;                 // a degenerate triangle contributes no real edge
            if (a > b) { const u32 tmp = a; a = b; b = tmp; }
            ++edgeUse[(static_cast<u64>(a) << 32) | static_cast<u64>(b)];
        }
    }

    for (const auto& kv : edgeUse) if (kv.second == 1) ++outOpen;
    outTotal = edgeUse.size();
    return outTotal ? static_cast<f32>(outOpen) / static_cast<f32>(outTotal) : 0.0f;
}

// Per-vertex shell id (dense, 0..shellCount-1) and, per shell, whether SMALL -- step 4: fits inside
// kMaxClusterVertices/kMaxClusterTriangles, counted over vertices/triangles the shell actually
// references (see the loop below for why, not "unioned into").
struct ShellIds {
    std::vector<u32> vertexShell;   // vertexShell[v] -- dense shell id of source-mesh vertex v
    std::vector<u8>  isSmall;       // isSmall[s] -- true iff shell s is small (see LodDag::smallShells)
};

// Union-find over triangle edges, THEN union every vertex v with remap[v] from
// meshopt_generatePositionRemap -- load-bearing, not a tidy-up: without it, a UV seam (triangles
// sharing a POSITION through DUPLICATED, not shared, indices) looks disconnected under edge-unioning
// alone, wrongly telling the routing below it is safe to drop LockBorder on one continuous surface.
// This is the same position-coincidence hashing meshopt_simplify's own LockBorder decision uses
// (indexgenerator.cpp vs. simplifier.cpp's border classification, both hash raw vertex_positions
// bytes), so shells here are never NARROWER than meshoptimizer's "connected" (file header: the safe
// error direction).
ShellIds computeShellIds(const fmt::OcMeshData& mesh) {
    const usize vertexCount = mesh.positions.size() / 3;

    UnionFind uf(vertexCount);
    for (usize t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const u32 i0 = mesh.indices[t + 0], i1 = mesh.indices[t + 1], i2 = mesh.indices[t + 2];
        uf.unite(i0, i1);
        uf.unite(i1, i2);
    }

    if (vertexCount > 0) {
        std::vector<u32> remap(vertexCount);
        meshopt_generatePositionRemap(remap.data(), mesh.positions.data(), vertexCount, sizeof(f32) * 3);
        for (usize v = 0; v < vertexCount; ++v) uf.unite(static_cast<u32>(v), remap[v]);
    }

    // Dense-pack the union-find roots into 0..shellCount-1 so LodDag::smallShells can be a plain
    // vector indexed by shellId instead of a sparse map keyed by an arbitrary root vertex index.
    ShellIds out;
    out.vertexShell.resize(vertexCount);
    std::vector<u32> rootToShell(vertexCount, std::numeric_limits<u32>::max());
    u32 shellCount = 0;
    for (usize v = 0; v < vertexCount; ++v) {
        const u32 root = uf.find(static_cast<u32>(v));
        if (rootToShell[root] == std::numeric_limits<u32>::max()) rootToShell[root] = shellCount++;
        out.vertexShell[v] = rootToShell[root];
    }

    // "Small" is counted over vertices/triangles the shell ACTUALLY references, not every position
    // unioned into it: nothing here produces an unreferenced stray position, but nothing guarantees a
    // source asset never will, and such a position must not misclassify the shell's size either way.
    std::vector<u8> referenced(vertexCount, 0);
    for (u32 idx : mesh.indices) referenced[idx] = 1;

    std::vector<u32> shellVerts(shellCount, 0), shellTris(shellCount, 0);
    for (usize v = 0; v < vertexCount; ++v)
        if (referenced[v]) ++shellVerts[out.vertexShell[v]];
    for (usize t = 0; t + 2 < mesh.indices.size(); t += 3)
        ++shellTris[out.vertexShell[mesh.indices[t]]];   // a triangle's 3 vertices share one shell

    out.isSmall.resize(shellCount);
    for (u32 s = 0; s < shellCount; ++s)
        out.isSmall[s] = (shellVerts[s] <= kMaxClusterVertices && shellTris[s] <= kMaxClusterTriangles) ? 1 : 0;

    return out;
}

// ---- grouping (task step 2a) -------------------------------------------------------------------
//
// ~4-8 clusters per group per the task; 6 is the middle of that range, 8 the hard cap.
constexpr u32 kTargetGroupSize = 6;
constexpr u32 kMaxGroupSize    = 8;

// meshopt_partitionClusters guarantees partition sizes of target..target+target/3 (meshoptimizer.h:
// 850) -- for kTargetGroupSize=6 that is exactly 6..8, which is where kMaxGroupSize=8 came from.
// Asserted once here so a change to kTargetGroupSize that breaks the "8 is the hard cap" assumption
// fails to compile instead of quietly producing an oversized group.
static_assert(kTargetGroupSize + kTargetGroupSize / 3 == kMaxGroupSize,
              "kMaxGroupSize documents meshopt_partitionClusters' own target..target+target/3 bound "
              "for kTargetGroupSize -- keep them in sync");

// Runs ONE meshopt_partitionClusters call over the clusters named by `ids`, using `positions`
// (indexed like `clusterIndices` -- groupClusters, the only caller, builds that pairing two ways:
// global ids for the large-shell bucket, locally-compacted ids for the small-shell one). Factored out
// so groupClusters' two-bucket split can call this once per bucket.
std::vector<std::vector<u32>> partitionClusterIds(const std::vector<u32>& ids,
                                                    const std::vector<u32>& clusterIndices,
                                                    const std::vector<u32>& clusterIndexCounts,
                                                    const f32* positions, usize vertexCount) {
    const u32 n = static_cast<u32>(ids.size());
    if (n == 0) return {};
    if (n == 1) return {{ids[0]}};   // meshopt_partitionClusters needs no help with this

    std::vector<u32> partitionOf(n);
    const usize partitionCount = meshopt_partitionClusters(
        partitionOf.data(), clusterIndices.data(), clusterIndices.size(),
        clusterIndexCounts.data(), n,
        positions, vertexCount, sizeof(f32) * 3,
        kTargetGroupSize);

    std::vector<std::vector<u32>> groups(partitionCount);
    for (u32 pos = 0; pos < n; ++pos) groups[partitionOf[pos]].push_back(ids[pos]);
    return groups;
}

// Groups clusters for joint simplification via meshopt_partitionClusters (meshoptimizer.h:852),
// available now that meshoptimizer is vendored (file header).
//
// WHY SAFE: crack-freeness comes from LockBorder on ONE merged index buffer per group
// (appendGlobalTriangles; file-level comment) and depends only on which triangles land in the SAME
// group, not on how grouping was decided. Measurably different partition than the greedy region-grow
// it replaced (a 1854-cluster case: 337 groups old, 289 new) -- changes DAG topology but not
// correctness, confirmed by rerunning TrifactorTest's full invariant suite (including the
// skinned-mesh regression) after the swap.
//
// TASK STEP 6: TWO SEPARATE calls, one per small/large-shell-lineage bucket, NEVER one over both
// concatenated. meshopt_partitionClusters (and meshopt_buildMeshlets) merges otherwise-unrelated
// geometry once adjacency runs out, picking the closest candidate IRRESPECTIVE of what it is
// (clusterizer.cpp) -- one call could place a large-shell cluster into an all-small-shell group, and
// step 7 decides LockBorder per GROUP, reopening the crack this prevents. Separate calls rule that
// out BY CONSTRUCTION.
//
// `mesh` is needed (the old grouper did not take it) for the spatial fallback's vertex positions, not
// just the topology dag.clusters[].vertices already carries.
std::vector<std::vector<u32>> groupClusters(const LodDag& dag, const std::vector<u32>& levelClusterIds,
                                             const fmt::OcMeshData& mesh) {
    if (levelClusterIds.empty()) return {};

    std::vector<u32> largeIds, smallIds;
    largeIds.reserve(levelClusterIds.size());
    smallIds.reserve(levelClusterIds.size());
    for (u32 cid : levelClusterIds)
        (dag.clusters[cid].smallShellLineage ? smallIds : largeIds).push_back(cid);

    std::vector<std::vector<u32>> groups;

    // Large-shell bucket: clusterIndices are GLOBAL vertex ids straight into `mesh.positions`, as
    // before this routing existed.
    if (!largeIds.empty()) {
        const u32 n = static_cast<u32>(largeIds.size());
        std::vector<u32> clusterIndices;
        std::vector<u32> clusterIndexCounts(n);
        clusterIndices.reserve(n * kMaxClusterVertices);
        for (u32 pos = 0; pos < n; ++pos) {
            const std::vector<u32>& verts = dag.clusters[largeIds[pos]].vertices;
            clusterIndexCounts[pos] = static_cast<u32>(verts.size());
            clusterIndices.insert(clusterIndices.end(), verts.begin(), verts.end());
        }
        const usize vertexCount = mesh.positions.size() / 3;
        auto largeGroups = partitionClusterIds(largeIds, clusterIndices, clusterIndexCounts,
                                                mesh.positions.data(), vertexCount);
        groups.insert(groups.end(), largeGroups.begin(), largeGroups.end());
    }

    // Small-shell bucket: positions COMPACTED to the vertices this bucket references, clusterIndices
    // remapped to that LOCAL space -- meshopt_partitionClusters' cost scales with `vertexCount`, and
    // passing the full mesh here too would pay that cost TWICE per level for clusters (fir_sapling:
    // 48,991 small shells) referencing only a tiny fraction of it.
    if (!smallIds.empty()) {
        const u32 n = static_cast<u32>(smallIds.size());
        std::vector<u32> uniqueVerts;
        std::unordered_map<u32, u32> globalToLocal;
        globalToLocal.reserve(n * kMaxClusterVertices);
        std::vector<u32> clusterIndices;
        std::vector<u32> clusterIndexCounts(n);
        clusterIndices.reserve(n * kMaxClusterVertices);
        for (u32 pos = 0; pos < n; ++pos) {
            const std::vector<u32>& verts = dag.clusters[smallIds[pos]].vertices;
            clusterIndexCounts[pos] = static_cast<u32>(verts.size());
            for (u32 v : verts) {
                const auto [it, inserted] = globalToLocal.try_emplace(v, static_cast<u32>(uniqueVerts.size()));
                if (inserted) uniqueVerts.push_back(v);
                clusterIndices.push_back(it->second);
            }
        }

        std::vector<f32> compactPositions(uniqueVerts.size() * 3);
        for (usize i = 0; i < uniqueVerts.size(); ++i) {
            const u32 v = uniqueVerts[i];
            compactPositions[i * 3 + 0] = mesh.positions[usize(v) * 3 + 0];
            compactPositions[i * 3 + 1] = mesh.positions[usize(v) * 3 + 1];
            compactPositions[i * 3 + 2] = mesh.positions[usize(v) * 3 + 2];
        }
        auto smallGroups = partitionClusterIds(smallIds, clusterIndices, clusterIndexCounts,
                                                compactPositions.data(), uniqueVerts.size());
        groups.insert(groups.end(), smallGroups.begin(), smallGroups.end());
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

// The world-extent scale (meshopt_simplifyScale units, meshoptimizer.h:606) of the DISTINCT global
// vertices `mergedIndices` references -- one group's subset, not the whole mesh. This is what
// meshopt_simplify's "relative" error is measured against once SimplifySparse is set
// (meshoptimizer.h:471), since its internal sparse_remap collapses to this same set before computing
// scale (verified in simplifier.cpp: buildSparseRemap:242, rescalePositions:549), so recomputing it
// here is exact. Lets the call site rescale result_error to the whole-mesh units Cluster::error holds.
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
// buffer), appending the resulting clusters to `dag` at `level`. Shared by buildClusters (level 0)
// and buildLodHierarchy (level k+1) -- both pass the SAME mesh.positions/vertexCount, keeping vertex
// identity global across every level (file-level comment).
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

// TASK STEP 5, the small-shell half of the routing. Builds ONE cluster directly from a single small
// shell's own triangles (`shellVertices` global ids, `localTriangles` indices into them) WITHOUT
// meshopt_buildMeshlets: a small shell (LodDag::isSmallShell) is DEFINED as fitting one meshlet's
// limits, so its partitioning search has nothing to decide, and paying for it per shell across tens
// of thousands (fir_sapling: 48,991) is pure overhead.
//
// THE REAL REASON IS SAFETY: concatenating several small shells into one buffer for
// meshopt_buildMeshlets would let it merge DIFFERENT shells into one meshlet once adjacency runs out
// (no notion of "shell" -- same fallback groupClusters describes), breaking shellId's "exact for a
// small-shell cluster" guarantee that step 8's lineage propagation trusts. One cluster per shell
// makes that true BY CONSTRUCTION.
//
// Bounds/cone computed IDENTICALLY to splitIntoClusters (same meshopt_computeMeshletBounds shape,
// whole-mesh buffer, never a local copy) since the cone culler downstream cannot tell which path
// produced a cluster, and a cheaper bounds computation here would silently miscull it.
u32 buildDirectCluster(const fmt::OcMeshData& mesh, std::vector<u32> shellVertices,
                        std::vector<u8> localTriangles, u32 shellId, LodDag& dag) {
    const usize vertexCount = mesh.positions.size() / 3;
    const u32 triangleCount = static_cast<u32>(localTriangles.size() / 3);

    Cluster c;
    c.level = 0;
    c.shellId = shellId;
    c.smallShellLineage = true;   // task step 5: every direct cluster is, definitionally, one small shell

    const meshopt_Bounds b = meshopt_computeMeshletBounds(
        shellVertices.data(), localTriangles.data(), triangleCount,
        mesh.positions.data(), vertexCount, sizeof(f32) * 3);

    c.vertices  = std::move(shellVertices);
    c.triangles = std::move(localTriangles);

    c.bounds.sphereCenter = {b.center[0], b.center[1], b.center[2]};
    c.bounds.sphereRadius = b.radius;
    c.bounds.coneApex     = {b.cone_apex[0], b.cone_apex[1], b.cone_apex[2]};
    const QuantizedCone q = quantizeConeConservative({b.cone_axis[0], b.cone_axis[1], b.cone_axis[2]}, b.cone_cutoff);
    c.bounds.coneAxis[0] = q.axis[0];
    c.bounds.coneAxis[1] = q.axis[1];
    c.bounds.coneAxis[2] = q.axis[2];
    c.bounds.coneCutoff  = q.cutoff;

    c.id = static_cast<u32>(dag.clusters.size());
    const u32 id = c.id;
    dag.clusters.push_back(std::move(c));

    if (dag.levels.empty()) dag.levels.resize(1);
    dag.levels[0].push_back(id);
    return id;
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

    // Task steps 2-4: classify `mesh` into connected shells and record the result on the DAG. One log
    // line per cooked mesh (RelodTool surfaces it for free): a future asset change silently making one
    // of the five previously-regressed meshes (file header) multi-shell, or shrinking a large shell to
    // small, would reopen this feature's safety argument -- this is the cheapest tripwire for it.
    const ShellIds shellIds = computeShellIds(mesh);
    dag.smallShells = shellIds.isSmall;
    u32 smallShellCount = 0;
    for (u8 s : dag.smallShells) smallShellCount += s ? 1 : 0;
    u64 openEdges = 0, totalEdges = 0;
    const f32 openFrac = openEdgeFraction(mesh, openEdges, totalEdges);
    AVER_INFO("[Trifactor] shells: {} ({} small, {} large) | open edges: {}/{} = {:.1f}%",
              dag.smallShells.size(), smallShellCount, dag.smallShells.size() - smallShellCount,
              openEdges, totalEdges, openFrac * 100.0f);

    // TASK STEP 5: route LOD-0 triangles by their shell's size, BEFORE any clustering call sees them.
    // A triangle's shell is its first vertex's shell -- computeShellIds unions all three into one
    // shell as its first step, so any one vertex is a valid representative.
    //
    // `largeIndices` walks mesh.indices ONCE, in order, keeping only triangles whose shell is NOT
    // small -- so a zero-small-shell mesh (the five previously-regressed meshes; file header) leaves
    // largeIndices byte-identical to mesh.indices, and splitIntoClusters makes the identical
    // meshopt_buildMeshlets call it always made: the order-preservation guarantee ClusterBuilder.hpp's
    // buildClusters doc comment promises.
    const usize triangleCount = mesh.indices.size() / 3;
    const u32 shellCount = static_cast<u32>(shellIds.isSmall.size());
    std::vector<u32> largeIndices;
    largeIndices.reserve(mesh.indices.size());
    // Indexed by shellId; holds a small shell's own triangles (global vertex ids, in mesh order) until
    // buildDirectCluster consumes them. Empty for every LARGE shell and for a small shell with no
    // triangles of its own (stray unreferenced position, per computeShellIds) -- both skipped by the
    // `continue` in the consuming loop below.
    std::vector<std::vector<u32>> smallShellTriangles(shellCount);
    for (usize t = 0; t < triangleCount; ++t) {
        const u32 i0 = mesh.indices[t * 3 + 0], i1 = mesh.indices[t * 3 + 1], i2 = mesh.indices[t * 3 + 2];
        const u32 shell = shellIds.vertexShell[i0];
        if (shellIds.isSmall[shell]) {
            std::vector<u32>& tris = smallShellTriangles[shell];
            tris.push_back(i0);
            tris.push_back(i1);
            tris.push_back(i2);
        } else {
            largeIndices.push_back(i0);
            largeIndices.push_back(i1);
            largeIndices.push_back(i2);
        }
    }

    // Large-shell stream: the ordinary meshopt_buildMeshlets path, completely unaware that a routing
    // decision was ever made -- it never sees a small-shell triangle, because largeIndices never
    // contains one.
    const std::vector<u32> largeIds = splitIntoClusters(mesh, largeIndices, /*level=*/0, dag);
    for (u32 id : largeIds) {
        Cluster& c = dag.clusters[id];
        // A representative, not necessarily this cluster's ONLY shell (meshopt_buildMeshlets may merge
        // several distinct LARGE shells into one meshlet -- Cluster::shellId). smallShellLineage stays
        // at its default (false), which is exact: every triangle came from largeIndices, which never
        // holds a small-shell triangle.
        c.shellId = shellIds.vertexShell[c.vertices[c.triangles[0]]];
    }

    // Small-shell stream: one direct cluster PER small shell, in increasing shellId order (not hash-
    // container arrival order), so buildClusters' output is a deterministic function of `mesh` alone.
    for (u32 shell = 0; shell < shellCount; ++shell) {
        const std::vector<u32>& triIndices = smallShellTriangles[shell];
        if (triIndices.empty()) continue;

        // A small shell has at most kMaxClusterVertices (64) distinct referenced vertices by
        // definition, so a linear de-dup scan is cheap and keeps this loop free of another hash
        // container -- this function's output must depend on nothing but `mesh` itself (see above).
        std::vector<u32> shellVertices;
        std::vector<u8> localTriangles;
        localTriangles.reserve(triIndices.size());
        for (u32 v : triIndices) {
            u8 local = 0;
            bool found = false;
            for (usize i = 0; i < shellVertices.size(); ++i) {
                if (shellVertices[i] == v) { local = static_cast<u8>(i); found = true; break; }
            }
            if (!found) {
                local = static_cast<u8>(shellVertices.size());
                shellVertices.push_back(v);
            }
            localTriangles.push_back(local);
        }

        buildDirectCluster(mesh, std::move(shellVertices), std::move(localTriangles), shell, dag);
    }

    return true;
}

bool buildLodHierarchy(const fmt::OcMeshData& mesh, LodDag& dag, std::string* why) {
    if (dag.levels.empty() || dag.levels[0].empty()) {
        if (why) *why = "buildLodHierarchy: dag has no LOD-0 clusters (call buildClusters first)";
        return false;
    }

    const usize vertexCount = mesh.positions.size() / 3;
    constexpr u32 kMaxLevels = 32; // safety cap against a non-converging loop, not an expected case

    // Computed ONCE for the whole hierarchy (worldExtentScale(mesh) -- the same public function
    // ConvertTool calls after this returns), reused below to rescale every group's
    // meshopt_SimplifySparse-relative error back into the whole-mesh-relative units Cluster::error
    // holds. See the meshopt_simplify call site for why that rescale exists.
    const f32 meshScale = worldExtentScale(mesh);

    // ---- WHERE THE COOK TIME ACTUALLY GOES ----
    //
    // Before this, nothing in this module was timed (no chrono/steady_clock anywhere in
    // modules/trifactor), yet the file header quotes cook times and a 98% figure nobody could
    // reproduce. EXISTS TO DECIDE A QUESTION, NOT DECORATE A LOG: whether the loops this module owns
    // (appendGlobalTriangles' gather, groupExtentScale's copy) are worth hand-vectorising, or are lost
    // inside vendored, scalar meshopt_simplify (zero SIMD intrinsics in simplifier.cpp). Reported as
    // PERCENTAGES, the form that decision needs.
    struct PhaseMs { f64 group = 0, gather = 0, simplify = 0, extent = 0, rest = 0; } phase;
    const auto tick = []() { return std::chrono::steady_clock::now(); };
    const auto msSince = [](std::chrono::steady_clock::time_point t0) {
        return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
    };
    const auto tWhole = tick();

    for (u32 level = 0; level < kMaxLevels; ++level) {
        const std::vector<u32> current = dag.levels[level]; // copy: dag.levels grows below
        if (current.size() <= 1) break;                     // already a single root cluster

        const auto tGroup = tick();
        const auto groups = groupClusters(dag, current, mesh);
        phase.group += msSince(tGroup);

        // Pass 1: simplify every group. Nothing is written to `dag` yet, so if NO group reduced, this
        // level is abandoned cleanly -- `level` stays the DAG's topmost level, no pointless level+1.
        struct PendingGroup {
            std::vector<u32> members;
            std::vector<u32> simplifiedIndices;
            f32 resultError = 0.0f;
            // TASK STEP 8: small-vs-large-shell LINEAGE, carried forward so Pass 2 tags every cluster
            // it creates, letting groupClusters bucket the same way one level up (Cluster::shellId is
            // level-0-only). True iff EVERY member is small-shell lineage (also decides LockBorder,
            // step 7). Two-bucket grouping keeps members homogeneous, so "every"/"any" agree anyway.
            bool allLargeShell = true;
        };
        std::vector<PendingGroup> pending;
        pending.reserve(groups.size());
        bool anyReduction = false;
        usize levelIndicesIn = 0, levelIndicesOut = 0;

        for (const auto& group : groups) {
            // Sized once instead of letting push_back grow it by doubling: this runs once per group
            // at every level (thousands of times per hierarchy), and the exact total is known up
            // front -- every member's triangles land in this one merged buffer.
            usize mergedTriIndices = 0;
            for (u32 cid : group) mergedTriIndices += dag.clusters[cid].triangles.size();
            std::vector<u32> mergedIndices;
            mergedIndices.reserve(mergedTriIndices);
            const auto tGather = tick();
            for (u32 cid : group) appendGlobalTriangles(dag.clusters[cid], mergedIndices);
            phase.gather += msSince(tGather);

            // Target: halve the triangle count, floored to 6 indices so zero is never asked for --
            // UNLESS the merged group itself has fewer than 6 (coastal_cliff_04: a single
            // locked-border triangle), where that floor would exceed meshopt_simplify's
            // `target_index_count <= index_count` precondition (asserts/aborts in debug). Clamp to
            // the group's own size instead.
            const usize targetIndexCount =
                std::min(mergedIndices.size(), std::max<usize>(6, (mergedIndices.size() / 2 / 3) * 3));

            // meshopt_SimplifySparse: without it, setup cost scales with the WHOLE MESH's vertex
            // count on every group at every level, though a group's merged buffer references at most
            // a few hundred (kMaxGroupSize*kMaxClusterVertices = 512). Measured (one real
            // 578-tri/338-vertex group, mesh vertex count 1,000 to 1,000,000): unflagged scaled
            // 0.05ms -> 35.08ms for the IDENTICAL group; SimplifySparse held flat at 0.03-0.04ms --
            // an O(whole-mesh) cost paid thousands of times across a hierarchy becomes O(group-size),
            // paid once per group. groupExtentScale corrects the one thing this flag changes.
            //
            // THREE THINGS TRIED AGAINST FOLIAGE'S BARELY-REDUCING LADDER (fir_sapling: 433,021 tris
            // at LOD 0 -> 393,157 at its coarsest of 13 levels, 9% total) -- LockBorder locks any edge
            // used by exactly one triangle, and a fir sapling is thousands of needle cards whose every
            // edge is a border edge:
            //   1. meshopt_SimplifyPrune (meshoptimizer.h:474, removes components "regardless of the
            //      topological restrictions inside components" -- documented for this exact mesh
            //      shape): fir_sapling 393,157 -> 391,012 (0.5%; 5.6% summed over 33 demo meshes).
            //      REVERTED.
            //   2. target_error = FLT_MAX (used elsewhere in this file so a tight bound doesn't
            //      "silently return far more triangles than requested"): hangs here -- TrifactorTest
            //      never returns with no error bound to stop the descent. 1e-2 is load-bearing.
            //      REVERTED.
            //   3. Dropping LockBorder outright (SimplifySparse, 1e-2 unchanged): fir_sapling
            //      393,157 -> 33 (13,121x, 13 levels -> 23); pine_sapling_small 315,120 -> 27
            //      (14,746x); grass_medium_01 24,514 -> 21 (2 levels -> 14); pine_tree_01 274,734 ->
            //      1,442 (had NO ladder before); corpus coarsest 1,195,431 -> 75,860 (15.8x, 28/33
            //      meshes improve; 481ee05) -- confirms LockBorder, not 1e-2, was the real ceiling
            //      (93.7% recovered vs SimplifyPrune's 5.6%). NOT SHIPPABLE: five solid meshes got
            //      WORSE (dead_tree_trunk 100->142, dead_tree_trunk_02 700->959, rock_07 218->245,
            //      rock_09 204->249, rock_moss_set_02 325->842) -- LockBorder was doing its real job
            //      on a truly solid mesh. REVERTED; must ROUTE instead of remove.
            //
            // TASK STEP 7, THE ACTUAL FIX: LockBorder stays ON iff EVERY member is large-shell lineage
            // (Cluster::smallShellLineage, propagated -- PendingGroup); OFF only for a group ENTIRELY
            // small-shell -- safe because step 5 keeps a small-shell cluster inside one shell too
            // small to be split by a group boundary (LodDag::isSmallShell's comment), and step 6
            // never mixes the two in one group.
            // target_error stays 1e-2 (FLT_MAX hangs, above); the risk may not transfer to a
            // small-shell buffer's much smaller scale, but that has not been swept, so it stays put.
            //
            // MEASURED, same 33-mesh corpus as the bare-removal numbers -- the win those collected,
            // minus the five-mesh regression:
            //
            //     mesh                  coarsest before -> after routing   (ladder before -> after)
            //     fir_sapling             393,157 -> 1,969                 ( 1.1x  -> 219.9x )
            //     pine_sapling_small      315,120 -> 1,152                 ( 1.3x  -> 345.6x )
            //     pine_tree_01            274,734 -> 1,791                 ( 1.0x  -> 153.4x ) *
            //     grass_medium_01          24,514 -> 2,661                 ( 1.0x  ->   9.3x )
            //     corpus coarsest       1,195,431 -> 140,489               ( -- summed, all 33 meshes )
            //     dead_tree_trunk/_02, rock_07/_09, rock_moss_set_02: BYTE-IDENTICAL to before (100,
            //     700, 218, 204, 325 -- unchanged), confirmed by diffing RelodTool's per-mesh line.
            //
            // * pine_tree_01's "before" (274,734, 1 level) is SimplifySparse alone; the cooked .ocmesh
            //   predates that fix and is unrecooked, and the corpus total sums that stale entry on
            //   both sides for consistency.
            //
            // 1,195,431 -> 140,489 sits between 75,860 (bare-removal) and a no-op, by shell mix: 14
            // meshes have ZERO small shells (unchanged, on top of the five protected), 3 are ALL small
            // shells (grass_medium_02 5,476->29, 188x; shrub_sorrel_01 1,807->123, 14.7x; moss_01
            // 116->92, modest only because the mesh is tiny (204 tri, 3 levels), not because any
            // group of it kept LockBorder), 11 MIX -- only their all-small-shell groups lose the flag.
            bool allLargeShell = true;
            for (u32 cid : group) {
                if (dag.clusters[cid].smallShellLineage) { allLargeShell = false; break; }
            }
            const u32 simplifyFlags = allLargeShell
                ? (meshopt_SimplifyLockBorder | meshopt_SimplifySparse)
                : meshopt_SimplifySparse;

            std::vector<u32> simplified(mergedIndices.size());
            f32 resultError = 0.0f;
            const auto tSimplify = tick();
            const usize simplifiedCount = meshopt_simplify(
                simplified.data(), mergedIndices.data(), mergedIndices.size(),
                mesh.positions.data(), vertexCount, sizeof(f32) * 3,
                targetIndexCount, /*target_error=*/1e-2f,
                simplifyFlags, &resultError);
            phase.simplify += msSince(tSimplify);
            simplified.resize(simplifiedCount);

            if (simplifiedCount < mergedIndices.size()) anyReduction = true;
            levelIndicesIn  += mergedIndices.size();
            levelIndicesOut += simplifiedCount;

            // meshopt_SimplifySparse makes `resultError` relative to THIS GROUP's own subset extent
            // (meshoptimizer.h:471: "error becomes relative to subset extents"), not the whole
            // mesh's -- rescale to whole-mesh-relative units (what every other path touching
            // Cluster::error assumes -- worldExtentScale/toScreenErrorThreshold) before comparing
            // against another group's error or propagating to a parent. Absolute error is
            // scale-invariant (subset extent * subset-relative error
            // == mesh extent * mesh-relative error, the same physical distance), so this is an exact
            // unit conversion, not an approximation.
            const auto tExtent = tick();
            const f32 groupScale = groupExtentScale(mesh, mergedIndices);
            phase.extent += msSince(tExtent);
            const f32 rescaledError = (meshScale > 0.0f) ? resultError * (groupScale / meshScale) : resultError;

            pending.push_back({group, std::move(simplified), rescaledError, allLargeShell});
        }

        if (!anyReduction) break;

        // ---- and a level that BARELY reduced is a level not worth storing ----------------------
        //
        // `anyReduction` is right for "is the simplifier stuck" but wrong for "is another level worth
        // its bytes": on FOLIAGE a leaf card is a disconnected quad, so a plant is thousands of shells
        // almost entirely boundary edge, each level shedding a handful of triangles but never zero, so
        // the loop ran to kMaxLevels storing thirty-odd near-identical copies of the mesh. MEASURED on
        // Intel's Jungle Ruins: JR_riverforest cooked an 845 MB .ocmesh for 998,981 triangles (~846
        // bytes/tri vs ~80 for vertex+index streams alone); JR_grass_B used 24 extra levels to remove
        // 15% of one small mesh (7,842 -> 6,668 tris).
        //
        // AN EIGHTH IS THE BAR, deliberately generous (classic ladder target is half): still admits
        // every solid mesh's ladder while refusing a level that costs a full mesh copy to save a
        // rounding error. THE LEVEL JUST BUILT IS KEPT (it did reduce, first rung to fail); measured
        // BEFORE Pass 2, acted on after, so the decision is about the level, not the last group.
        constexpr f32 kMinLevelReduction = 0.125f;
        const bool converged =
            levelIndicesIn > 0 &&
            f32(levelIndicesIn - levelIndicesOut) / f32(levelIndicesIn) < kMinLevelReduction;

        // Pass 2: every group produced SOMETHING usable -- even a non-reducing group re-splits into
        // a valid, unchanged next level; consistent DAG structure matters more than special-casing it.
        const u32 newLevel = level + 1;
        for (const auto& pg : pending) {
            const std::vector<u32> newIds = splitIntoClusters(mesh, pg.simplifiedIndices, newLevel, dag);

            // Error monotonicity enforced EXPLICITLY, not assumed from result_error: each new
            // cluster's error is max(this group's own simplification error, largest error already on
            // any child replaced) -- the former reflects this level's own work, the latter makes it
            // monotone across the DAG edge.
            f32 childMaxError = 0.0f;
            for (u32 cid : pg.members) childMaxError = std::max(childMaxError, dag.clusters[cid].error);
            const f32 propagatedError = std::max(pg.resultError, childMaxError);

            for (u32 parentId : newIds) {
                dag.clusters[parentId].error = propagatedError;
                // TASK STEP 8: carry lineage forward so groupClusters buckets this output correctly
                // one level up -- without it, smallShellLineage defaults to false/large past level 0.
                dag.clusters[parentId].smallShellLineage = !pg.allLargeShell;
                for (u32 childId : pg.members) {
                    dag.clusters[parentId].children.push_back(childId);
                    dag.clusters[childId].parents.push_back(parentId);
                }
            }

            // ---- STAGE 4: this SAME pass has everything needed for the streaming topology too --
            // pg.members and newIds are both in scope with full group geometry, which
            // Cluster::fallbackAncestorId's comment says this must be decided with, not deferred to a
            // runtime seeing one cluster id at a time. Skipped only if newIds ended up empty (never
            // observed here, though not proven impossible for a pathological input; there is no
            // principled group node for zero output clusters, and pg.members would already fail
            // validateLodDag's parent check anyway).
            if (!newIds.empty()) {
                // A TRUE sphere-of-spheres over pg.members' OWN, PRE-simplification bounds
                // (mergeSphere above) -- never the group's own coarser, post-simplification clusters
                // (the mistake ClusterGroupNode::sphereCenter's comment warns about). Seeded with the
                // first child's own sphere, not a degenerate (origin, 0), so a single-member group's
                // node gets that child's EXACT sphere back.
                Vec3 groupCenter = dag.clusters[pg.members[0]].bounds.sphereCenter;
                f32  groupRadius = dag.clusters[pg.members[0]].bounds.sphereRadius;
                for (usize mi = 1; mi < pg.members.size(); ++mi) {
                    const Cluster& child = dag.clusters[pg.members[mi]];
                    mergeSphere(groupCenter, groupRadius, child.bounds.sphereCenter, child.bounds.sphereRadius);
                }

                ClusterGroupNode node;
                node.id           = static_cast<u32>(dag.groupNodes.size());
                node.level        = newLevel;
                node.sphereCenter = groupCenter;
                node.sphereRadius = groupRadius;
                // CONTIGUOUS by construction (ClusterGroupNode::ownClusterRange): newIds is exactly
                // what splitIntoClusters just appended to dag.clusters, back to back, so
                // [newIds.front(), newIds.front()+newIds.size()) names precisely this group's output.
                node.ownClusterStart   = newIds.front();
                node.ownClusterCount   = static_cast<u32>(newIds.size());
                node.childClusterStart = static_cast<u32>(dag.groupChildren.size());
                node.childClusterCount = static_cast<u32>(pg.members.size());
                dag.groupChildren.insert(dag.groupChildren.end(), pg.members.begin(), pg.members.end());
                dag.groupNodes.push_back(node);

                for (u32 parentId : newIds) dag.clusters[parentId].ownerGroupId = node.id;

                // PART A: fallbackAncestorId -- for each child this group replaced, the group's own
                // output cluster (newIds) whose bounding-sphere CENTRE is nearest that child's own
                // centre. Comparing squared distances avoids a sqrt per (child, candidate) pair; the
                // comparison's ORDER is unaffected since both sides are non-negative.
                for (u32 childId : pg.members) {
                    const Vec3& childCenter = dag.clusters[childId].bounds.sphereCenter;
                    u32 nearest = newIds[0];
                    f32 nearestDistSq = (dag.clusters[nearest].bounds.sphereCenter - childCenter).sizeSquared();
                    for (usize ni = 1; ni < newIds.size(); ++ni) {
                        const f32 distSq =
                            (dag.clusters[newIds[ni]].bounds.sphereCenter - childCenter).sizeSquared();
                        if (distSq < nearestDistSq) { nearestDistSq = distSq; nearest = newIds[ni]; }
                    }
                    dag.clusters[childId].fallbackAncestorId = nearest;
                }
            }
        }

        if (converged) break;
    }

    // ONE LINE PER COOKED MESH, AS PERCENTAGES: the form the only decision this supports needs -- is
    // any loop this module owns worth hand-vectorising, or is it all inside vendored meshopt_simplify?
    // Milliseconds alone would not answer that.
    {
        const f64 whole = msSince(tWhole);
        phase.rest = whole - (phase.group + phase.gather + phase.simplify + phase.extent);
        const auto pct = [whole](f64 v) { return whole > 0.0 ? (v * 100.0 / whole) : 0.0; };
        AVER_INFO("[Trifactor] buildLodHierarchy {:.1f}ms -- simplify {:.1f}% | group {:.1f}% | "
                  "gather {:.1f}% | extent {:.1f}% | rest {:.1f}%",
                  whole, pct(phase.simplify), pct(phase.group), pct(phase.gather),
                  pct(phase.extent), pct(phase.rest));
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

            // Acyclic proof: every DAG edge is added by buildLodHierarchy going level k to k+1
            // (splitIntoClusters has exactly two call sites -- buildClusters at level 0, this one --
            // and only this one records an edge), so `level` is a topological order by
            // construction. Checking every edge strictly increases under it is a complete acyclicity
            // proof (a total order strictly increasing along every edge cannot cycle), not a
            // shortcut for a skipped DFS.
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

// Containment tolerance for validateClusterHierarchy's sphere check. mergeSphere's arithmetic is
// exact (only f32 rounding across at most 7 sequential merges), so this only absorbs accumulated
// rounding -- but a FIXED epsilon like this file's 1e-6f error checks would be wrong here: those
// compare mesh-relative error (near [0,1]), while sphere radii/centres are absolute world units (cm),
// centimetres to thousands. Scaling to the sphere's own radius stays correct at both ends: never so
// tight that f32 rounding on a large sphere false-flags, never so loose it hides a real bug on a
// tiny one.
constexpr f32 kContainmentEpsilonRel = 1e-4f;
constexpr f32 kContainmentEpsilonAbs = 1e-3f;

bool validateClusterHierarchy(const LodDag& dag, std::string* why) {
    const u32 topLevel = dag.levelCount() > 0 ? dag.levelCount() - 1 : 0;

    // ---- every non-root cluster has a VALID fallbackAncestorId, at EXACTLY one level coarser -----
    for (const Cluster& c : dag.clusters) {
        const bool isRoot = (c.level == topLevel);
        if (isRoot) continue;   // a root's fallbackAncestorId is checked implicitly: nothing sets it,
                                 // and it is never read as a level-coarser reference by anything below

        if (c.fallbackAncestorId == fmt::kInvalidClusterId) {
            if (why) *why = "cluster " + std::to_string(c.id) + " (level " + std::to_string(c.level) +
                             ") is non-root but has no fallbackAncestorId -- a streaming system with "
                             "this cluster's own page missing would have nothing coarser to draw instead";
            return false;
        }
        if (c.fallbackAncestorId >= dag.clusters.size()) {
            if (why) *why = "cluster " + std::to_string(c.id) + "'s fallbackAncestorId " +
                             std::to_string(c.fallbackAncestorId) + " is out of range for " +
                             std::to_string(dag.clusters.size()) + " clusters";
            return false;
        }
        const Cluster& ancestor = dag.clusters[c.fallbackAncestorId];
        if (ancestor.level != c.level + 1) {
            if (why) *why = "cluster " + std::to_string(c.id) + " (level " + std::to_string(c.level) +
                             ")'s fallbackAncestorId " + std::to_string(c.fallbackAncestorId) +
                             " is at level " + std::to_string(ancestor.level) + ", not " +
                             std::to_string(c.level + 1) + " -- fallbackAncestorId must be one of THIS "
                             "child's own group's output clusters, one level coarser, never further";
            return false;
        }
    }

    // ---- every group node's sphere GENUINELY CONTAINS every child's sphere; every range is in bounds
    for (const ClusterGroupNode& g : dag.groupNodes) {
        if (u64(g.ownClusterStart) + g.ownClusterCount > dag.clusters.size()) {
            if (why) *why = "group " + std::to_string(g.id) + "'s ownClusterRange runs past " +
                             std::to_string(dag.clusters.size()) + " clusters";
            return false;
        }
        if (u64(g.childClusterStart) + g.childClusterCount > dag.groupChildren.size()) {
            if (why) *why = "group " + std::to_string(g.id) + "'s childClusterRange runs past " +
                             std::to_string(dag.groupChildren.size()) + "-entry groupChildren";
            return false;
        }

        const f32 eps = kContainmentEpsilonAbs + g.sphereRadius * kContainmentEpsilonRel;
        for (u32 k = 0; k < g.childClusterCount; ++k) {
            const u32 childId = dag.groupChildren[usize(g.childClusterStart) + k];
            if (childId >= dag.clusters.size()) {
                if (why) *why = "group " + std::to_string(g.id) + " references child cluster " +
                                 std::to_string(childId) + ", out of range for " +
                                 std::to_string(dag.clusters.size()) + " clusters";
                return false;
            }
            const Cluster& child = dag.clusters[childId];
            if (child.level + 1 != g.level) {
                if (why) *why = "group " + std::to_string(g.id) + " (level " + std::to_string(g.level) +
                                 ") references child cluster " + std::to_string(childId) + " at level " +
                                 std::to_string(child.level) + ", not " + std::to_string(g.level - 1) +
                                 " -- a group's children must be exactly one level finer than the group";
                return false;
            }

            const f32 centreDist = (g.sphereCenter - child.bounds.sphereCenter).size();
            if (centreDist + child.bounds.sphereRadius > g.sphereRadius + eps) {
                if (why) *why = "group " + std::to_string(g.id) + "'s sphere (centre (" +
                                 std::to_string(g.sphereCenter.x) + ", " + std::to_string(g.sphereCenter.y) +
                                 ", " + std::to_string(g.sphereCenter.z) + "), radius " +
                                 std::to_string(g.sphereRadius) + ") does not contain child cluster " +
                                 std::to_string(childId) + "'s own sphere (radius " +
                                 std::to_string(child.bounds.sphereRadius) + ", centre distance " +
                                 std::to_string(centreDist) + ") -- centreDist + childRadius exceeds "
                                 "groupRadius by " +
                                 std::to_string(centreDist + child.bounds.sphereRadius - g.sphereRadius);
                return false;
            }
        }

        // ---- ownerGroupId round-trips: every cluster THIS group produced points back at it --------
        for (u32 k = 0; k < g.ownClusterCount; ++k) {
            const u32 ownId = g.ownClusterStart + k;
            if (dag.clusters[ownId].ownerGroupId != g.id) {
                if (why) *why = "cluster " + std::to_string(ownId) + " is in group " + std::to_string(g.id) +
                                 "'s ownClusterRange but its own ownerGroupId reads " +
                                 std::to_string(dag.clusters[ownId].ownerGroupId) + ", not " +
                                 std::to_string(g.id);
                return false;
            }
        }
    }

    // ---- converse: ownerGroupId is valid for its level -- kInvalidClusterId at level 0, a real group id at level >= 1
    for (const Cluster& c : dag.clusters) {
        if (c.level == 0) {
            if (c.ownerGroupId != fmt::kInvalidClusterId) {
                if (why) *why = "LOD-0 cluster " + std::to_string(c.id) + " has ownerGroupId " +
                                 std::to_string(c.ownerGroupId) + " set, but LOD 0 is never produced by a group";
                return false;
            }
            continue;
        }
        if (c.ownerGroupId == fmt::kInvalidClusterId || c.ownerGroupId >= dag.groupNodes.size()) {
            if (why) *why = "cluster " + std::to_string(c.id) + " (level " + std::to_string(c.level) +
                             ") has an invalid ownerGroupId (" + std::to_string(c.ownerGroupId) + " of " +
                             std::to_string(dag.groupNodes.size()) + " groups) -- every cluster above LOD 0 "
                             "must have been produced by some group";
            return false;
        }
        if (dag.groupNodes[c.ownerGroupId].level != c.level) {
            if (why) *why = "cluster " + std::to_string(c.id) + " (level " + std::to_string(c.level) +
                             ")'s ownerGroupId " + std::to_string(c.ownerGroupId) + " names a group at level " +
                             std::to_string(dag.groupNodes[c.ownerGroupId].level) + ", not its own";
            return false;
        }
    }

    return true;
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

namespace {

// Global cluster id -> the on-disk, LEVEL-LOCAL index OcMeshMeshlet's fallbackAncestorId and
// OcMeshClusterGroup's ownClusterRange/ClusterGroupChildren[] use. Valid because a level's cluster
// ids are a CONTIGUOUS range by construction (ClusterGroupNode::ownClusterRange): every cluster at a
// level is pushed to dag.clusters back to back (buildClusters at level 0, buildLodHierarchy's Pass 2
// per level above), so dag.levels[level] is exactly [front(), front() + size()).
u32 toLevelLocalIndex(const LodDag& dag, u32 globalClusterId) {
    const u32 level = dag.clusters[globalClusterId].level;
    return globalClusterId - dag.levels[level].front();
}

// Per-level ClusterGroupNode::id bookkeeping, the group-side counterpart to dag.levels -- built with
// one pass over dag.groupNodes (cheap: a few thousand groups at most). Group ids are contiguous
// within a level for the same reason cluster ids are: Pass 2 pushes one node per group, in order, for
// a WHOLE level before advancing.
struct GroupLevelIndex {
    std::vector<u32> firstId;   // per level; fmt::kInvalidClusterId if that level has no group nodes
    std::vector<u32> count;     // per level; 0 if none
};
GroupLevelIndex indexGroupsByLevel(const LodDag& dag) {
    GroupLevelIndex idx;
    idx.firstId.assign(dag.levelCount(), fmt::kInvalidClusterId);
    idx.count.assign(dag.levelCount(), 0);
    for (const ClusterGroupNode& g : dag.groupNodes) {
        if (idx.firstId[g.level] == fmt::kInvalidClusterId) idx.firstId[g.level] = g.id;
        ++idx.count[g.level];
    }
    return idx;
}
// Global ClusterGroupNode id -> the on-disk, level-local OcMeshClusterGroup[] index
// OcMeshMeshlet::ownerGroupId stores. Mirrors toLevelLocalIndex above, for groups instead of clusters.
u32 toGroupLocalIndex(const LodDag& dag, const GroupLevelIndex& groupIdx, u32 globalGroupId) {
    const u32 level = dag.groupNodes[globalGroupId].level;
    return globalGroupId - groupIdx.firstId[level];
}

// Converts one LOD level of a DAG into the on-disk OcMeshMeshlet shape. LIFTED FROM ConvertTool.cpp's
// own toMeshlets (packLodDag's doc comment in ClusterBuilder.hpp), byte-for-byte the same conversion,
// so corpus numbers measured against the old copy still apply here. `errorBounds` is
// computeClusterErrorBounds(dag, scale)'s output, indexed by Cluster::id -- where
// ownError/parentError cross into Formats' OcMeshMeshlet. `groupIdx` translates ownerGroupId only
// (fallbackAncestorId names a CLUSTER, so it goes through toLevelLocalIndex alone).
std::vector<fmt::OcMeshMeshlet> toMeshlets(const LodDag& dag, u32 level,
                                            const std::vector<ClusterErrorBounds>& errorBounds,
                                            const GroupLevelIndex& groupIdx) {
    std::vector<fmt::OcMeshMeshlet> out;
    if (level >= dag.levels.size()) return out;
    out.reserve(dag.levels[level].size());
    for (const u32 cid : dag.levels[level]) {
        const Cluster& c = dag.clusters[cid];
        fmt::OcMeshMeshlet ml;
        ml.vertices     = c.vertices;
        ml.triangles    = c.triangles;
        ml.sphereCenter = c.bounds.sphereCenter;
        ml.sphereRadius = c.bounds.sphereRadius;
        ml.coneApex     = c.bounds.coneApex;
        ml.coneAxis[0]  = c.bounds.coneAxis[0];
        ml.coneAxis[1]  = c.bounds.coneAxis[1];
        ml.coneAxis[2]  = c.bounds.coneAxis[2];
        ml.coneCutoff   = c.bounds.coneCutoff;
        ml.ownError     = errorBounds[cid].ownError;
        ml.parentError  = errorBounds[cid].parentError;
        // Stage 4: fmt::kInvalidClusterId passes through UNTRANSLATED -- it is not a real cluster/group id
        // to look up a level for, it is the sentinel itself, and toLevelLocalIndex/toGroupLocalIndex
        // would read dag.clusters[0xFFFFFFFF]/dag.groupNodes[0xFFFFFFFF] if handed it directly.
        ml.fallbackAncestorId = (c.fallbackAncestorId == fmt::kInvalidClusterId)
                                     ? fmt::kInvalidClusterId : toLevelLocalIndex(dag, c.fallbackAncestorId);
        ml.ownerGroupId = (c.ownerGroupId == fmt::kInvalidClusterId)
                               ? fmt::kInvalidClusterId : toGroupLocalIndex(dag, groupIdx, c.ownerGroupId);
        out.push_back(std::move(ml));
    }
    return out;
}

// Converts one LOD level's ClusterGroupNode entries (dag.groupNodes, restricted to this level via
// `groupIdx`) into the on-disk OcMeshClusterGroup[] + flat ClusterGroupChildren[] shape -- the group-
// side counterpart to toMeshlets/toIndices above. Empty for level 0 (no group ever owns a LOD-0
// cluster) and for any level `groupIdx` recorded no groups at (the DAG never grew past level 0).
void toGroupNodes(const LodDag& dag, u32 level, const GroupLevelIndex& groupIdx,
                   std::vector<fmt::OcMeshClusterGroup>& outNodes, std::vector<u32>& outChildren) {
    outNodes.clear();
    outChildren.clear();
    if (level >= groupIdx.count.size() || groupIdx.count[level] == 0) return;

    const u32 first = groupIdx.firstId[level];
    const u32 count = groupIdx.count[level];
    outNodes.reserve(count);
    for (u32 k = 0; k < count; ++k) {
        const ClusterGroupNode& g = dag.groupNodes[first + k];
        fmt::OcMeshClusterGroup out;
        out.sphereCenter = g.sphereCenter;
        out.sphereRadius = g.sphereRadius;
        // ownClusterRange is already a contiguous GLOBAL range at level `g.level` (this same level);
        // its level-local start is just its offset from that level's own first cluster id.
        out.ownClusterStart = toLevelLocalIndex(dag, g.ownClusterStart);
        out.ownClusterCount = g.ownClusterCount;
        // childClusterRange indexes dag.groupChildren (GLOBAL cluster ids, one level finer); each one
        // is translated and appended to THIS level's own outChildren, which is what
        // OcMeshLod::groupChildren (a per-level array, not a whole-mesh one) actually stores.
        out.childClusterStart = static_cast<u32>(outChildren.size());
        out.childClusterCount = g.childClusterCount;
        for (u32 ci = 0; ci < g.childClusterCount; ++ci)
            outChildren.push_back(toLevelLocalIndex(dag, dag.groupChildren[g.childClusterStart + ci]));
        outNodes.push_back(out);
    }
}

// Converts one LOD level's clusters back into a plain GLOBAL-index triangle list -- the level's own
// index buffer, for OcMeshLod::indices. LIFTED FROM ConvertTool.cpp's own toIndices, unchanged: a
// cluster's `triangles` are LOCAL indices into its own `vertices` (the on-disk MLET shape); this
// undoes that, mirroring appendGlobalTriangles above in this same file (private to this TU).
std::vector<u32> toIndices(const LodDag& dag, u32 level) {
    std::vector<u32> out;
    if (level >= dag.levels.size()) return out;
    for (const u32 cid : dag.levels[level]) {
        const Cluster& c = dag.clusters[cid];
        for (usize t = 0; t + 2 < c.triangles.size(); t += 3) {
            out.push_back(c.vertices[c.triangles[t + 0]]);
            out.push_back(c.vertices[c.triangles[t + 1]]);
            out.push_back(c.vertices[c.triangles[t + 2]]);
        }
    }
    return out;
}

} // namespace

bool packLodDag(const LodDag& dag, fmt::OcMeshData& mesh, std::string* why) {
    if (dag.empty()) {
        if (why) *why = "packLodDag: dag has no clusters (call buildClusters first)";
        return false;
    }

    // Same scale computation ConvertTool's addMeshlets always made, now made once here instead of by
    // every caller. worldExtentScale reads mesh.positions/vertexCount only, never
    // indices/submeshes/materialSlots/joints/weights, so a caller (RelodTool's write path) can hand
    // this a full mesh copy and trust every OTHER stream stays untouched.
    const f32 scale = worldExtentScale(mesh);

    std::string monoWhy;
    if (!validateScreenErrorMonotonic(dag, scale, &monoWhy)) {
        if (why) *why = "screen-error monotonicity broke after conversion: " + monoWhy;
        return false;
    }

    // ownError/parentError computed once for the whole DAG, validated BEFORE anything is packed: a
    // violation here is the "holes in the mesh" failure mode the local cut test cannot detect on its
    // own, so it must fail the cook loudly rather than reach a file. Same ordering addMeshlets used.
    const std::vector<ClusterErrorBounds> errorBounds = computeClusterErrorBounds(dag, scale);
    std::string boundsWhy;
    if (!validateClusterErrorBounds(dag, errorBounds, &boundsWhy)) {
        if (why) *why = "per-cluster error bounds invalid: " + boundsWhy;
        return false;
    }

    // STAGE 4: the streaming topology's correctness re-checked here for the same reason
    // validateClusterErrorBounds is checked above rather than trusted from the caller -- a broken
    // fallbackAncestorId or non-containing group sphere must not reach mesh.coarserLods either.
    std::string hierarchyWhy;
    if (!validateClusterHierarchy(dag, &hierarchyWhy)) {
        if (why) *why = "cluster hierarchy invalid: " + hierarchyWhy;
        return false;
    }

    const GroupLevelIndex groupIdx = indexGroupsByLevel(dag);

    mesh.meshlets = toMeshlets(dag, 0, errorBounds, groupIdx);
    mesh.coarserLods.clear();
    for (u32 level = 1; level < dag.levelCount(); ++level) {
        fmt::OcMeshLod lod;
        lod.indices  = toIndices(dag, level);
        lod.meshlets = toMeshlets(dag, level, errorBounds, groupIdx);
        toGroupNodes(dag, level, groupIdx, lod.groupNodes, lod.groupChildren);
        // Every cluster newly created at this level shares the SAME propagatedError (assigned once
        // per group), so max() over the level is defensive, not strictly necessary -- stays correct
        // if a future buildLodHierarchy change ever lets that stop being true.
        f32 rawError = 0.0f;
        for (u32 cid : dag.levels[level]) rawError = std::max(rawError, dag.clusters[cid].error);
        lod.screenErrorThreshold = toScreenErrorThreshold(rawError, scale);
        mesh.coarserLods.push_back(std::move(lod));
    }

    // PART B: stamp which builder cooked this ladder. Only reached once everything above succeeded --
    // a failed pack leaves builderVersion exactly as it found it, same as every other stream.
    mesh.builderVersion = kBuilderVersion;
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
    // Checked, not assumed: every array below is indexed by these, and meshopt only asserts.
    for (const u32 i : mesh.indices)
        if (i >= vcount) return fail("an index points past the end of the vertex buffer");

    // ONE SIMPLIFICATION PER SUBMESH, never across the whole buffer: meshopt_simplify sees positions
    // only, so simplifying in one pass shuffled triangles between mesh.submeshes' per-material ranges
    // under an unchanged table -- buildMeshParts (Runtime/src/GameContent.cpp, one draw per material)
    // dropped the overshooting ranges and drew the whole mesh under slot 0 (NewSponza's
    // curtains+cloth+metal_door mesh went dark/glossy after --lod 0.25). Simplifying each range on
    // its own keeps every triangle in its material; table rewritten below to match.
    if (mesh.submeshes.size() > 1) {
        std::string partWhy;
        if (!fmt::submeshesPartitionIndices(mesh, &partWhy)) {
            if (why) *why = "cannot simplify per submesh: " + partWhy;
            return false;
        }
    }

    // No table, or one submesh (drawn whole regardless of its range -- fmt::submeshesPartitionIndices)
    // means nothing to keep apart: the whole buffer is one range, the old behaviour; a lone submesh
    // is rewritten below to cover the result.
    struct Range { usize start, count; };
    std::vector<Range> ranges;
    if (mesh.submeshes.size() <= 1) {
        ranges.push_back({0, mesh.indices.size()});
    } else {
        ranges.reserve(mesh.submeshes.size());
        for (const fmt::OcMeshSubmesh& s : mesh.submeshes) ranges.push_back({s.indexStart, s.indexCount});
    }

    // MATERIAL BORDERS ARE LOCKED, or separate simplification cracks the mesh open where two
    // submeshes meet: each range would see that seam as its own open border and collapse it on its
    // own schedule. Every vertex whose POSITION (not index -- glTF primitives never share those) is
    // used by more than one range is locked, via the same meshopt_generatePositionRemap
    // computeShellIds uses. A true open border (one range only) stays free to simplify.
    std::vector<u8> lock;
    if (ranges.size() > 1) {
        std::vector<u32> posRemap(vcount);
        meshopt_generatePositionRemap(posRemap.data(), mesh.positions.data(), vcount, sizeof(f32) * 3);
        constexpr u32 kUnused = ~0u, kShared = ~0u - 1u;
        std::vector<u32> user(vcount, kUnused);   // per canonical position: the one range using it
        for (usize r = 0; r < ranges.size(); ++r)
            for (usize k = ranges[r].start; k < ranges[r].start + ranges[r].count; ++k) {
                u32& u = user[posRemap[mesh.indices[k]]];
                if (u == kUnused) u = static_cast<u32>(r);
                else if (u != static_cast<u32>(r)) u = kShared;
            }
        bool anyShared = false;
        lock.assign(vcount, 0);
        for (u32 v = 0; v < vcount; ++v)
            if (user[posRemap[v]] == kShared) { lock[v] = meshopt_SimplifyVertex_Lock; anyShared = true; }
        if (!anyShared) lock.clear();
    }

    // FLT_MAX rather than a small bound, deliberately: the caller asked for a triangle COUNT, and a
    // tight error bound would silently return far more triangles than requested while reporting
    // success. Letting error float and reporting what it cost is the honest shape.
    f32 resultError = 0.0f;
    std::vector<u32> out;
    out.reserve(mesh.indices.size());
    std::vector<u32> newIndexStart(ranges.size()), newIndexCount(ranges.size());
    for (usize r = 0; r < ranges.size(); ++r) {
        const usize start = ranges[r].start, count = ranges[r].count;
        const usize target = usize(f64(count) * f64(ratio)) / 3 * 3;
        std::vector<u32> rOut(count);
        usize got = count;
        if (target >= 3 && count >= 3) {
            f32 rErr = 0.0f;
            // meshopt_simplify is exactly this call with no attributes and no locks.
            got = meshopt_simplifyWithAttributes(
                rOut.data(), mesh.indices.data() + start, count,
                mesh.positions.data(), vcount, sizeof(f32) * 3, nullptr, 0, nullptr, 0,
                lock.empty() ? nullptr : lock.data(),
                target, std::numeric_limits<f32>::max(), 0, &rErr);
            resultError = std::max(resultError, rErr);
        } else {
            // Too small a range to ask for even one triangle at this ratio (a small accent
            // submesh): kept whole rather than emptied.
            std::copy_n(mesh.indices.data() + start, count, rOut.data());
        }
        rOut.resize(got);
        newIndexStart[r] = static_cast<u32>(out.size());
        newIndexCount[r] = static_cast<u32>(got);
        out.insert(out.end(), rOut.begin(), rOut.end());
    }

    if (out.size() < 3) return fail("the simplifier returned no triangles");
    // A simplifier that barely moved has usually hit a mesh it cannot collapse (every edge on a
    // border, or degenerate topology). Saying so beats writing a "simplified" mesh that is not.
    if (out.size() > mesh.indices.size() * 9 / 10 && out.size() > targetIndices * 2)
        return fail("the simplifier could not get near the requested ratio");
    const usize got = out.size();

    // The reduced index buffer still addresses the ORIGINAL vertex array, so most vertices are now
    // unreferenced. Compact, or the file keeps every source vertex while only the triangle count goes
    // down. Whole-mesh compaction is safe: it renumbers vertices and never moves a triangle between
    // submesh ranges.
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

    // THE SKIN STREAMS: forgetting them made this unusable on any rigged asset -- positions/normals/
    // uvs remapped to newVerts while joints/weights kept the OLD count, so hasSkin() (both must be
    // exactly v*4 for the NEW v) went false, valid() failed, and writeOcMesh refused to save -- the
    // symptom was a misleading "simplified" line followed by "mesh has no vertices, no indices, or
    // mismatched attribute counts", pointing at everything except the real cause. No blending needed:
    // meshopt_optimizeVertexFetchRemap only compacts away vertices no surviving triangle references,
    // so each destination vertex has exactly one source and its
    // influences carry across unchanged; averaging weights would corrupt the rig.
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
    mesh.computeBounds();    // positions changed; writeOcMesh recomputes this too, but an in-memory
                              // caller that reads boundsMin/Max before saving deserves a live value

    // Each range was reassembled in table order, so its new place is known exactly. baseVertex/
    // vertexCount become the importers' "whole vertex buffer" form (0, newVerts): after compaction
    // any narrower vertex span a table carried no longer describes anything real.
    for (usize i = 0; i < mesh.submeshes.size(); ++i) {
        mesh.submeshes[i].indexStart  = newIndexStart[i];
        mesh.submeshes[i].indexCount  = newIndexCount[i];
        mesh.submeshes[i].baseVertex  = 0;
        mesh.submeshes[i].vertexCount = static_cast<u32>(newVerts);
    }

    if (why) {
        char buf[128];
        std::snprintf(buf, sizeof(buf), "%zu tris, %zu verts, error %.4f",
                      mesh.indices.size() / 3, usize(newVerts), double(resultError));
        *why = buf;
    }
    return true;
}

} // namespace aver::trifactor
