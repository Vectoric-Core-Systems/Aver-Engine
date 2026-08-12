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
//
//   3. meshopt_SimplifyLockBorder was set unconditionally on every group at every level, which is
//      correct for a solid mesh but flattens a foliage mesh's ladder to nearly nothing (a fir sapling
//      is thousands of separate leaf cards, each almost entirely boundary edge -- see
//      buildLodHierarchy's own comment, and the shell/open-edge measurement further down this file,
//      for the numbers). Fixed not by removing the flag (that measurably breaks five solid meshes --
//      same comment) but by ROUTING it per group: computeShellIds classifies the source mesh into
//      connected shells below; buildClusters (task step 5) splits LOD-0 triangles into a small-shell
//      stream (built directly, one cluster per shell) and a large-shell stream (the ordinary
//      meshopt_buildMeshlets path, order-preserving so a zero-small-shell mesh's output is untouched);
//      groupClusters (task step 6) partitions each stream separately so a group never mixes them; and
//      buildLodHierarchy's meshopt_simplify call (task step 7) drops LockBorder only for a group made
//      entirely of small-shell lineage, a fact PendingGroup carries forward every level (task step 8)
//      so it does not silently stop being true above level 1. See Cluster::smallShellLineage's own
//      comment (ClusterBuilder.hpp) for the field this all turns on, and the meshopt_simplify call
//      site below for the corpus numbers this routing actually achieved.
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

// ---- connected-shell classification (task steps 3-4) -------------------------------------------
//
// WHY THIS EXISTS. meshopt_SimplifyLockBorder (see the file-level comment above) locks any edge used
// by exactly one triangle in the buffer it is handed. On a SOLID mesh -- one shell -- an edge on the
// group's true outside really is single-use in that buffer, so LockBorder correctly holds the group
// boundary and interior detail still simplifies away underneath it. A FOLIAGE mesh is hundreds of
// DISCONNECTED shells, one sheet per leaf/needle card, so almost every edge looks single-use from
// inside any buffer that only contains some of those shells, and nothing collapses: measured over the
// demo corpus with meshopt_SimplifyLockBorder unconditionally set (before this classification
// existed), fir_sapling's coarsest level was 393,157 triangles out of 433,021 at LOD 0 -- a 1.1x
// ladder on a mesh that should reduce by orders of magnitude. Dropping the flag entirely fixes that
// (13,121x) but breaks five solid, single-shell meshes the flag was protecting correctly (see the
// commit this file's header names). The fix has to be PER SHELL: keep LockBorder for groups made of
// shells too big to ever fit in one meshlet (a group boundary can genuinely cut through such a shell,
// so the crack-free guarantee still needs it), drop it for groups made entirely of shells too small
// to ever be split by a group boundary in the first place (see LodDag::isSmallShell's own comment for
// why "fits in one meshlet" is the exact, provable line).
//
// THIS SECTION COMPUTES WHICH IS WHICH; NOTHING YET ACTS ON IT. buildClusters records the result
// (Cluster::shellId, LodDag::smallShells) but still builds every LOD-0 cluster through
// meshopt_buildMeshlets over the whole mesh unconditionally, and buildLodHierarchy's meshopt_simplify
// call still sets LockBorder unconditionally too -- see Cluster::shellId's STAGE STATUS comment
// (ClusterBuilder.hpp) for why: this task's own step 1 asked for the five previously-regressed
// meshes to be confirmed single-shell before any routing got built on that assumption, and one of the
// five (rock_moss_set_02) is not -- it is seven independently-large shells. Every one of the seven
// clears the "large" bar by itself, so the routing this task describes would still be safe for this
// particular mesh, but the premise the task's design leans on is not universally what it was assumed
// to be, and per the task's own instruction that finding that out is worth more than shipping routing
// built on it, the routing (steps 5-8) stops here for this stage.

// Union-find over `count` elements, path-halved on find(), union by attaching the second root to the
// first (no rank/size heuristic). `count` here is at most a mesh's vertex count -- a few hundred
// thousand at the outside for this engine's demo corpus -- and this runs ONCE per mesh, at LOD-0
// build time, not per level or per group; a plain compressing find is more than fast enough, and
// every line of it is auditable, which matters more for a correctness-load-bearing routine than
// shaving a one-off pass.
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

// THE NUMBER THE SHELL THEORY WAS FIRST MISREAD AS NEEDING, and the one this function answers instead.
//
// computeShellIds above answers "how many connected components", and an early pass over the whole demo
// corpus was misread as proving the shell-routing design could not work: "every one of the 33 meshes
// reports ZERO small shells, so a rule keyed on small shells never fires." That reading was wrong -- a
// shell listing and a ladder listing were compared side by side without checking the rows still lined
// up after a filter shifted one of them -- and the corrected re-run is what steps 5-8 (buildClusters'
// routing, buildLodHierarchy's meshopt_simplify call) are actually built on; see Cluster::shellId's
// comment in the header for the corrected measurement in full, and this file's own header for the
// routing's own numbers.
//
// The MECHANISM below answers a related but different, and genuinely more useful, question: not
// "how many shells" but "what predicts whether LockBorder freezes a mesh". meshopt_SimplifyLockBorder
// locks an edge
// used by exactly ONE triangle in the buffer it is given -- it has no notion of "shell" at all. So
// what predicts whether the flag freezes a mesh is not how many pieces the mesh is in, it is what
// FRACTION of its edges are open. A leaf card is a thin sheet: four perimeter edges around two
// triangles, so it is almost entirely boundary even when welded into a large connected component.
// A rock is a closed solid: every edge shared by two triangles, so almost nothing is boundary and
// LockBorder costs it nothing.
//
// Edges are canonicalised through the SAME meshopt_generatePositionRemap that computeShellIds uses,
// for the same reason: two triangles meeting across a UV seam share a POSITION but not an index, and
// counting raw indices would call that shared edge two open edges instead of one closed one --
// inflating exactly the statistic this exists to measure, and by most on the assets that matter.
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

// Per-vertex shell id (dense, 0..shellCount-1) and, per shell, whether it is SMALL -- task step 4:
// fits inside kMaxClusterVertices/kMaxClusterTriangles, counted over the vertices/triangles the
// shell's geometry actually references (see the loop below for why "actually references" and not
// "unioned into" is what gets counted).
struct ShellIds {
    std::vector<u32> vertexShell;   // vertexShell[v] -- dense shell id of source-mesh vertex v
    std::vector<u8>  isSmall;       // isSmall[s] -- true iff shell s is small (see LodDag::smallShells)
};

// Union-find over triangle edges, THEN union every vertex v with remap[v] from
// meshopt_generatePositionRemap. That second union is the load-bearing part, not a tidy-up: without
// it, a UV seam -- two triangles that share a POSITION through DUPLICATED (not shared) vertex
// indices, which every real asset with a UV island boundary has -- would look disconnected under
// triangle-edge unioning alone and split into two shells, and this function would then tell the
// routing below it is safe to drop LockBorder on what is genuinely one continuous surface.
// meshopt_generatePositionRemap is the exact same position-coincidence hashing meshopt_simplify's own
// LockBorder decision is built on (both hash the raw vertex_positions bytes -- compare
// indexgenerator.cpp's meshopt_generatePositionRemap against simplifier.cpp's border classification),
// so the shells this function computes are never NARROWER than meshoptimizer's own notion of
// "connected" -- see this file's header comment for why that direction of error, and not the other
// one, is the safe one to risk.
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

    // "Small" is counted over vertices/triangles the shell's geometry ACTUALLY references, not every
    // position that happened to union into it: nothing in this codebase produces an unreferenced
    // stray position, but nothing guarantees a source asset never will, and such a position must not
    // make an otherwise-tiny shell look large (or, worse, hide a genuinely-oversized shell as small).
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

// meshopt_partitionClusters guarantees actual partition sizes of target..target+target/3
// (meshoptimizer.h:850) -- for kTargetGroupSize=6 that is exactly 6..8, which is where
// kMaxGroupSize=8 came from in the first place. Not a coincidence to re-derive at every call site;
// asserted once here so a change to kTargetGroupSize that silently breaks the "8 is the hard cap"
// assumption fails to compile instead of quietly producing an oversized group.
static_assert(kTargetGroupSize + kTargetGroupSize / 3 == kMaxGroupSize,
              "kMaxGroupSize documents meshopt_partitionClusters' own target..target+target/3 bound "
              "for kTargetGroupSize -- keep them in sync");

// Runs ONE meshopt_partitionClusters call over exactly the clusters named by `ids`, using `positions`
// (a buffer already indexed the same way `clusterIndices` is -- groupClusters below is the only
// caller, and builds that pairing two different ways: global mesh-vertex ids for the large-shell
// bucket, locally-compacted ids for the small-shell bucket) as the spatial-proximity input. Factored
// out of groupClusters (task step 6) so the two-bucket split there can call this once per bucket
// without duplicating the meshopt_partitionClusters call shape or its degenerate-size special cases.
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
// TASK STEP 6: TWO SEPARATE meshopt_partitionClusters CALLS, one per small/large-shell-lineage
// bucket (Cluster::smallShellLineage), NEVER one call over the concatenation of both. The reason is
// the same spatial-proximity fallback the paragraph above just credited: meshopt_partitionClusters
// (and, upstream of it, meshopt_buildMeshlets -- see buildDirectCluster's comment) will merge
// otherwise-unrelated geometry once real adjacency runs out, picking the closest candidate
// IRRESPECTIVE of what it is (third_party/meshoptimizer/src/clusterizer.cpp's own comment on this).
// A single call over both buckets would therefore be free to place a large-shell cluster in an
// otherwise-all-small-shell group -- and task step 7's routing decides LockBorder per GROUP, so that
// one misplaced cluster would silently take its whole group through the flag-dropped path, right back
// into the crack this feature exists to prevent. Partitioning each bucket separately makes that
// impossible BY CONSTRUCTION: meshopt_partitionClusters never sees the other bucket's clusters at all,
// so it cannot place one of them into a group it has no way to know exists.
//
// `mesh` is needed here (the old hand-rolled grouper did not take it) because
// meshopt_partitionClusters' spatial-fallback pass wants vertex positions, not just the shared-vertex
// topology dag.clusters[].vertices already carries.
std::vector<std::vector<u32>> groupClusters(const LodDag& dag, const std::vector<u32>& levelClusterIds,
                                             const fmt::OcMeshData& mesh) {
    if (levelClusterIds.empty()) return {};

    std::vector<u32> largeIds, smallIds;
    largeIds.reserve(levelClusterIds.size());
    smallIds.reserve(levelClusterIds.size());
    for (u32 cid : levelClusterIds)
        (dag.clusters[cid].smallShellLineage ? smallIds : largeIds).push_back(cid);

    std::vector<std::vector<u32>> groups;

    // Large-shell bucket: unchanged from before this routing existed -- clusterIndices are GLOBAL
    // vertex ids straight into `mesh.positions`, exactly as the single-bucket call used to build them.
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

    // Small-shell bucket: positions are COMPACTED to exactly the vertices this bucket's clusters
    // reference, and clusterIndices are remapped to that compacted, LOCAL index space, rather than
    // passing `mesh.positions`/the whole mesh's vertexCount a second time. This is the scratch-cost
    // bound task step 6 asks for: meshopt_partitionClusters takes a vertex buffer sized to
    // `vertexCount`, and with two calls per level instead of one, passing the full mesh both times
    // would pay that O(whole-mesh-vertex-count) cost TWICE at every level for a bucket whose own
    // clusters, on the meshes this feature exists for (fir_sapling: 48,991 small shells), reference a
    // tiny fraction of the mesh's actual vertices.
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

// TASK STEP 5, the small-shell half of the routing. Builds ONE cluster directly from a single small
// shell's own triangles -- `shellVertices` (global vertex ids, in first-seen order) and
// `localTriangles` (indices into `shellVertices`, the on-disk MLET shape) -- WITHOUT going through
// meshopt_buildMeshlets at all.
//
// WHY NOT JUST CALL splitIntoClusters ON THE SHELL'S OWN TRIANGLES, which would also work and would
// reuse more code: a small shell (LodDag::isSmallShell) is DEFINED as fitting inside one meshlet's
// kMaxClusterVertices/kMaxClusterTriangles limits, so meshopt_buildMeshlets' partitioning search --
// scoring candidate triangles, growing a meshlet, deciding when to start a new one -- has nothing to
// decide: the answer is always "everything in one meshlet". Paying for that search on what will always
// be a single-meshlet answer, once per small shell, across meshes with tens of thousands of them
// (fir_sapling: 48,991), is pure overhead with no output it could ever change.
//
// THE OTHER REASON IS NOT PERFORMANCE, IT IS SAFETY, and it is the one that actually matters. If this
// function instead concatenated several small shells' triangles into one buffer and called
// meshopt_buildMeshlets on THAT (the way splitIntoClusters is used for the large-shell stream),
// nothing would stop meshopt_buildMeshlets from putting two DIFFERENT small shells' triangles in the
// same meshlet -- it has no notion of "shell" and, per groupClusters' comment on
// meshopt_partitionClusters' identical fallback, actively will once an individual shell's own
// adjacency runs out. A cluster is not a group, so this would not by itself put LockBorder at risk --
// but it WOULD break shellId's "exact for a small-shell cluster" guarantee (Cluster::shellId's own
// comment), which task step 8's lineage propagation is built on trusting without re-deriving. Building
// one cluster per shell, from exactly that shell's own triangles and nothing else, makes "this
// cluster's geometry belongs to exactly one shell" true BY CONSTRUCTION rather than by an argument
// about what meshopt_buildMeshlets happens to do today.
//
// Bounds/cone are computed IDENTICALLY to splitIntoClusters -- same meshopt_computeMeshletBounds call
// shape (global vertex ids + local triangle indices + the WHOLE mesh's position buffer and vertex
// count, never a local copy of either) and the same quantizeConeConservative rounding -- because the
// cone culler downstream has no idea whether a cluster came from meshopt_buildMeshlets or from here,
// and a cheaper or different bounds computation on this path would silently make it get culled wrong.
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
    // line per cooked mesh (RelodTool surfaces it for free) rather than nothing: a future asset change
    // that silently made one of the five previously-regressed meshes (see this file's header comment)
    // multi-shell, or turned a currently-large shell small, would be exactly the kind of thing that
    // reopens this feature's safety argument, and this is the cheapest possible tripwire for it.
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
    // A triangle's shell is its first vertex's shell -- computeShellIds unions all three of a
    // triangle's vertices into the same shell as its very first step (the triangle-edge union, before
    // the position-remap union that closes UV seams), so every vertex of a given triangle names the
    // identical shell and any one of them is a valid representative.
    //
    // `largeIndices` is built by walking mesh.indices ONCE, in order, and keeping only the triangles
    // whose shell is NOT small -- so for a mesh with zero small shells (every one of the five
    // previously-regressed meshes on the real corpus; see this file's header) not a single triangle is
    // ever removed, and `largeIndices` ends up holding mesh.indices' exact values in their exact
    // order. That is the order-preservation guarantee ClusterBuilder.hpp's buildClusters doc comment
    // promises: splitIntoClusters below then receives a buffer identical to mesh.indices and calls the
    // SAME meshopt_buildMeshlets this function always called on it, so such a mesh's LOD-0 output is
    // not merely equivalent to what this function produced before this routing existed -- it is the
    // identical function call on the identical input, byte for byte.
    const usize triangleCount = mesh.indices.size() / 3;
    const u32 shellCount = static_cast<u32>(shellIds.isSmall.size());
    std::vector<u32> largeIndices;
    largeIndices.reserve(mesh.indices.size());
    // Indexed by shellId; holds a small shell's own triangles (global vertex ids, 3 per triangle, in
    // mesh order) until buildDirectCluster below consumes them. Stays empty for every LARGE shell, and
    // for a small shell that (per computeShellIds' own comment on stray unreferenced positions) turns
    // out to have no triangles of its own -- both cases are skipped by the `continue` in the loop that
    // consumes this.
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
        // A representative, not necessarily this cluster's ONLY shell (meshopt_buildMeshlets may
        // merge triangles from several distinct LARGE shells into one meshlet) -- see Cluster::shellId's
        // comment. smallShellLineage is left at its default (false), which is exact here: every
        // triangle in this cluster came from largeIndices, and largeIndices never holds a small-shell
        // triangle, so "large" is correct regardless of which specific shell is named.
        c.shellId = shellIds.vertexShell[c.vertices[c.triangles[0]]];
    }

    // Small-shell stream: one direct cluster PER small shell (buildDirectCluster, above in this file),
    // in increasing shellId order -- not the arrival order of some hash container -- so that
    // buildClusters' output is a deterministic function of `mesh` alone, exactly like every other path
    // through this file.
    for (u32 shell = 0; shell < shellCount; ++shell) {
        const std::vector<u32>& triIndices = smallShellTriangles[shell];
        if (triIndices.empty()) continue;

        // A small shell has at most kMaxClusterVertices (64) distinct referenced vertices by
        // definition (LodDag::isSmallShell), so a linear scan to de-duplicate is a handful of
        // comparisons per triangle, not a complexity concern -- and it keeps this loop free of another
        // hash container, which matters here specifically: this function's whole output must be
        // order-independent of anything BUT `mesh` itself (see the paragraph above).
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

    // Computed ONCE for the whole hierarchy (this is worldExtentScale(mesh) -- the same public
    // function ConvertTool calls after this returns), and reused below to rescale every group's
    // meshopt_SimplifySparse-relative error back into the whole-mesh-relative units Cluster::error is
    // documented to hold. See the meshopt_simplify call site below for why that rescale exists at
    // all.
    const f32 meshScale = worldExtentScale(mesh);

    // ---- WHERE THE COOK TIME ACTUALLY GOES ----
    //
    // NOTHING IN THIS MODULE WAS TIMED. Not one chrono/steady_clock/elapsed anywhere in
    // modules/trifactor -- yet this file's own header quotes "13m35s/8m40s real cook times a prior
    // profiling pass reported" and "~98% of a synthetic 498K-triangle mesh's total buildLodHierarchy
    // time". Those numbers are narrated, not produced by any code here, so nobody could reproduce
    // them and nobody could tell whether an optimisation had helped.
    //
    // THIS EXISTS TO DECIDE A QUESTION RATHER THAN TO DECORATE A LOG. The question is whether the
    // loops this module actually owns -- appendGlobalTriangles' index gather and groupExtentScale's
    // position copy -- are worth hand-vectorising, or whether they are lost inside meshopt_simplify,
    // which is vendored, scalar (zero SIMD intrinsics in simplifier.cpp), and not ours to change.
    // Optimising the wrong one of those is how effort gets spent for no measurable result, so the
    // breakdown is split three ways and reported as PERCENTAGES, which is the form the decision
    // needs.
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

        // Pass 1: simplify every group. Nothing is written to `dag` yet, so if NO group reduced,
        // this level can be abandoned cleanly -- `level` stays the DAG's topmost (root) level rather
        // than growing an identical, pointless level + 1 on top of it.
        struct PendingGroup {
            std::vector<u32> members;
            std::vector<u32> simplifiedIndices;
            f32 resultError = 0.0f;
            // TASK STEP 8: this group's small-vs-large-shell LINEAGE, carried forward so Pass 2 below
            // can tag every cluster it creates -- which is what lets groupClusters (task step 6) make
            // the SAME bucketing decision again at the NEXT level, without ever falling back to
            // Cluster::shellId (populated at level 0 only; see its own comment for why that would be
            // unsafe past level 0). True iff EVERY member of this group is itself small-shell lineage
            // -- see the computation just above the meshopt_simplify call below, which is also what
            // decides whether this group keeps LockBorder (task step 7). Two-bucket grouping
            // guarantees a group's members are homogeneous in this field, so "every member" and "any
            // member" agree in practice; this is computed as "every member" anyway, matching the
            // task's own wording, rather than trusting that invariant silently.
            bool allLargeShell = true;
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
            const auto tGather = tick();
            for (u32 cid : group) appendGlobalTriangles(dag.clusters[cid], mergedIndices);
            phase.gather += msSince(tGather);

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
            // TWO THINGS TRIED HERE AND MEASURED AND REVERTED, so nobody spends the day again.
            // The problem being attacked: this ladder barely reduces foliage. fir_sapling goes
            // 433,021 triangles at LOD 0 to 393,157 at its COARSEST of 13 levels -- 9% across the
            // whole hierarchy -- because LockBorder locks any edge used by exactly one triangle, and
            // a fir sapling is thousands of separate needle cards whose every edge is a border edge.
            //
            // meshopt_SimplifyPrune (meshoptimizer.h:474), which removes whole disconnected
            // components "regardless of the topological restrictions inside components" and is
            // documented for exactly this shape of mesh: measured with tools/RelodTool over all 33
            // demo meshes, fir_sapling's coarsest level went 393,157 -> 391,012. Half a percent.
            // 5.6% summed across every mesh. Not worth changing cook output for.
            //
            // target_error = FLT_MAX, which the OTHER meshopt_simplify call in this file uses with
            // the comment that a tight bound "would silently return far more triangles than
            // requested": it does not terminate. TrifactorTest hangs inside buildLodHierarchy with
            // no error bound to stop the descent, so 1e-2 is load-bearing, not incidental.
            //
            // AND A THIRD, WHICH WORKED, and is the reason this comment is no longer a dead end.
            // Dropping LockBorder here (keeping SimplifySparse, keeping target_error at 1e-2) was
            // measured with tools/RelodTool over the same 33 demo meshes:
            //
            //     fir_sapling         393,157 -> 33          (1.1x -> 13,121x, 13 levels -> 23)
            //     pine_sapling_small  315,120 -> 27          (1.3x -> 14,746x)
            //     grass_medium_01      24,514 -> 21          (had TWO levels; now 14)
            //     pine_tree_01        274,734 -> 1,442       (had ONE level -- no ladder at all)
            //     corpus coarsest   1,195,431 -> 75,860      (15.8x; 28 of 33 meshes improve)
            //
            // So the 1e-2 error bound was NEVER the ceiling -- LockBorder was, exactly as the
            // paragraph above suspected but could not price. For scale, SimplifyPrune recovered
            // 5.6% across the same corpus; this recovers 93.7%.
            //
            // IT IS NOT SHIPPABLE AS A BARE FLAG REMOVAL, and the same measurement shows why: five
            // meshes got WORSE, all of them solid rather than shelled -- dead_tree_trunk 100 -> 142,
            // dead_tree_trunk_02 700 -> 959, rock_07 218 -> 245, rock_09 204 -> 249,
            // rock_moss_set_02 325 -> 842. On a mesh that IS one connected surface, LockBorder is
            // doing its real job of holding the group boundary, and removing it lets the simplifier
            // spend its error budget wrecking seams instead of collapsing interiors. The fix has to
            // ROUTE: keep this call exactly as it is for groups touching a large shell, and take the
            // flag off only for buffers that provably contain whole isolated shells and nothing
            // else.
            //
            // TASK STEP 7, THE ACTUAL FIX: LockBorder stays ON iff EVERY member of this group is
            // large-shell lineage (Cluster::smallShellLineage, propagated -- see PendingGroup's own
            // comment); it comes OFF only for a group made ENTIRELY of small-shell clusters. This is
            // the routing task steps 5-6 exist to feed: task step 5 guarantees a small-shell cluster's
            // triangles belong to exactly one shell too small to ever be split across a group boundary
            // (LodDag::isSmallShell's own comment), and task step 6 guarantees a group never mixes a
            // small-shell cluster with a large-shell one -- so "all members small-shell" is exactly
            // the condition under which dropping LockBorder here cannot cut through a boundary that
            // still needs protecting. target_error stays at 1e-2 for both branches -- see the header
            // comment two paragraphs up for why raising it to FLT_MAX does not terminate; an isolated
            // small-shell group's own buffer is orders of magnitude smaller than the whole-mesh buffer
            // that hang was measured on, so the risk may not transfer, but that has not been swept and
            // measured here, so the constant is left where it was proven safe rather than guessed at.
            //
            // MEASURED, with tools/RelodTool over the same 33 demo meshes the bare-removal numbers
            // above came from -- this is the routing collecting the win those numbers priced without
            // breaking what they broke:
            //
            //     mesh                  coarsest before -> after routing   (ladder before -> after)
            //     fir_sapling             393,157 -> 1,969                 ( 1.1x  -> 219.9x )
            //     pine_sapling_small      315,120 -> 1,152                 ( 1.3x  -> 345.6x )
            //     pine_tree_01            274,734 -> 1,791                 ( 1.0x  -> 153.4x ) *
            //     grass_medium_01          24,514 -> 2,661                 ( 1.0x  ->   9.3x )
            //     corpus coarsest       1,195,431 -> 140,489               ( -- summed, all 33 meshes )
            //
            //     dead_tree_trunk             100 -> 100    dead_tree_trunk_02   700 -> 700
            //     rock_07                     218 -> 218    rock_09              204 -> 204
            //     rock_moss_set_02            325 -> 325
            //
            // Every one of the five previously-regressed meshes is BYTE-IDENTICAL to what it was
            // before this routing existed -- same triangle count, same level count, same ladder --
            // confirmed by diffing RelodTool's own per-mesh output line, not by re-deriving it from
            // the shell counts. That is the routing's whole point delivering: the corpus-wide win
            // 481ee05 measured by dropping the flag outright, MINUS the five-mesh regression that
            // measurement also found.
            //
            // * pine_tree_01's "before" here is 274,734 (1 lv, 1.0x), the figure meshopt_SimplifySparse
            //   alone produces (RelodTool's own "was" column, the currently-cooked .ocmesh on disk,
            //   still reads 274,734/1 level -- this asset predates that fix too and has not been
            //   recooked); the corpus-coarsest total above sums RelodTool's "was" column for
            //   consistency with 481ee05's 1,195,431 reference figure, which carries that same stale
            //   entry on both sides of the comparison.
            //
            // 1,195,431 -> 140,489 does not reach 75,860 (the bare-removal figure), and should not.
            // Sorted by why, over all 33 meshes (RelodTool's per-mesh shells: line, cross-checked
            // against its own coarsest-level line for every one of them, not eyeballed):
            //
            //   - 14 meshes report ZERO small shells and are therefore UNCHANGED TO THE TRIANGLE, on
            //     top of the five protected meshes above: bark_debris_01, boulder_01,
            //     dry_branches_medium_01, nettle_plant, pine_roots, rock_face_01, rock_moss_set_01,
            //     root_cluster_01, root_cluster_02, single_root, stone_01, tree_stump_01,
            //     tree_stump_02, weed_plant_02. LockBorder is correctly still protecting the only
            //     shells they have -- there is nothing for this routing to do here, by the same
            //     construction that protects the five.
            //   - 3 meshes (grass_medium_02, moss_01, shrub_sorrel_01) are ALL small shells (0 large)
            //     and get the FULL benefit, every group on the flag-dropped path: grass_medium_02
            //     5,476 -> 29 (188x), shrub_sorrel_01 1,807 -> 123 (14.7x). moss_01's own move (116 ->
            //     92) looks modest only because the mesh itself is tiny (204 triangles, 3 levels) with
            //     little left to remove, not because any group of it kept LockBorder.
            //   - 11 meshes MIX small and large shells (celandine_01, dandelion_01, fern_02,
            //     fir_sapling, grass_medium_01, pine_sapling_small, pine_tree_01, shrub_01, shrub_02,
            //     shrub_03, shrub_04): only the groups that end up entirely small-shell ever lose the
            //     flag, and every group still touching the large shell keeps it -- exactly as
            //     designed, and the reason this corpus total sits between the two reference points
            //     instead of matching either one.
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

            // meshopt_SimplifySparse makes `resultError` relative to THIS GROUP's own subset extent
            // (meshoptimizer.h:471's own doc: "error becomes relative to subset extents"), not the
            // whole mesh's -- rescale it back to whole-mesh-relative units (what every OTHER path
            // that touches Cluster::error assumes -- worldExtentScale/toScreenErrorThreshold,
            // ClusterBuilder.hpp's own comment on the field) before it is compared against another
            // group's error or propagated to a parent. Absolute error is scale-invariant (subset
            // extent * subset-relative error == mesh extent * mesh-relative error, both being the
            // same physical distance), so this is an exact unit conversion, not an approximation.
            const auto tExtent = tick();
            const f32 groupScale = groupExtentScale(mesh, mergedIndices);
            phase.extent += msSince(tExtent);
            const f32 rescaledError = (meshScale > 0.0f) ? resultError * (groupScale / meshScale) : resultError;

            pending.push_back({group, std::move(simplified), rescaledError, allLargeShell});
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
                // TASK STEP 8: carry the group's small-vs-large-shell lineage forward onto every
                // cluster it produced, so groupClusters can bucket THIS level's output correctly when
                // it runs again one level up -- without this, Cluster::smallShellLineage would stay at
                // its default (false/large) for every cluster past level 0, which groupClusters would
                // read as "large-shell" regardless of what actually produced it. See PendingGroup's own
                // comment for why this field, not a re-derivation from shellId, is what gets read.
                dag.clusters[parentId].smallShellLineage = !pg.allLargeShell;
                for (u32 childId : pg.members) {
                    dag.clusters[parentId].children.push_back(childId);
                    dag.clusters[childId].parents.push_back(parentId);
                }
            }
        }
    }

    // ONE LINE, ONCE PER COOKED MESH, AS PERCENTAGES -- which is the form the only decision this
    // supports actually needs: is any loop this module owns worth hand-vectorising, or is it all
    // inside vendored meshopt_simplify? A breakdown in milliseconds alone would not answer that.
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

namespace {

// Converts one LOD level of a DAG into the on-disk OcMeshMeshlet shape. LIFTED FROM
// tests/formats/src/ConvertTool.cpp's own toMeshlets (see packLodDag's doc comment in
// ClusterBuilder.hpp for why this now lives here instead) -- byte-for-byte the same conversion,
// unchanged, so the corpus numbers this task measured with the old ConvertTool-private copy still
// apply to this one. `errorBounds` is computeClusterErrorBounds(dag, scale)'s output, indexed by
// Cluster::id exactly like dag.clusters -- this is where ownError/parentError cross from Trifactor's
// Cluster into Formats' OcMeshMeshlet, same as it always was.
std::vector<fmt::OcMeshMeshlet> toMeshlets(const LodDag& dag, u32 level,
                                            const std::vector<ClusterErrorBounds>& errorBounds) {
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
        out.push_back(std::move(ml));
    }
    return out;
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
    // every caller that wants to pack a DAG -- worldExtentScale reads mesh.positions/vertexCount only,
    // never mesh.indices/submeshes/materialSlots/joints/weights, which is what lets a caller (RelodTool's
    // write path) hand this a full copy of an original mesh and trust every OTHER stream stays untouched.
    const f32 scale = worldExtentScale(mesh);

    std::string monoWhy;
    if (!validateScreenErrorMonotonic(dag, scale, &monoWhy)) {
        if (why) *why = "screen-error monotonicity broke after conversion: " + monoWhy;
        return false;
    }

    // ownError/parentError -- computed once for the whole DAG, then validated BEFORE anything is
    // packed: a violation here is exactly the "holes in the mesh" failure mode the local cut test
    // cannot detect on its own, so it must fail the cook loudly rather than reach a file. Same
    // ordering addMeshlets always used.
    const std::vector<ClusterErrorBounds> errorBounds = computeClusterErrorBounds(dag, scale);
    std::string boundsWhy;
    if (!validateClusterErrorBounds(dag, errorBounds, &boundsWhy)) {
        if (why) *why = "per-cluster error bounds invalid: " + boundsWhy;
        return false;
    }

    mesh.meshlets = toMeshlets(dag, 0, errorBounds);
    mesh.coarserLods.clear();
    for (u32 level = 1; level < dag.levelCount(); ++level) {
        fmt::OcMeshLod lod;
        lod.indices  = toIndices(dag, level);
        lod.meshlets = toMeshlets(dag, level, errorBounds);
        // Every cluster newly created at this level shares the SAME propagatedError (buildLodHierarchy
        // assigns it once per group, to every cluster the group's re-split produced), so max() over
        // the level is defensive rather than strictly necessary -- it stays correct even if a future
        // change to buildLodHierarchy ever let that stop being true.
        f32 rawError = 0.0f;
        for (u32 cid : dag.levels[level]) rawError = std::max(rawError, dag.clusters[cid].error);
        lod.screenErrorThreshold = toScreenErrorThreshold(rawError, scale);
        mesh.coarserLods.push_back(std::move(lod));
    }

    // PART B: stamp which builder cooked this ladder. Only reached once everything above has
    // succeeded, matching mesh.meshlets/coarserLods themselves only being reachable on success -- a
    // failed pack leaves builderVersion exactly as it found it, same as every other stream.
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
