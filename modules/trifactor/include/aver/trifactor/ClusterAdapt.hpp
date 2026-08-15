#pragma once
// ClusterAdapt -- the bridge THE COOK -> THE MATHS the task brief asks for: turns an fmt::OcMeshData
// (modules/formats/include/aver/formats/OcMesh.hpp, on disk per the Cook, commit 81a4bb0) into what
// ClusterSelect.hpp needs (View / ClusterView), and answers the one question that pairing did not
// yet answer: for ONE mesh instance, THIS frame, which LOD level should actually be drawn.
//
// NEW FILE, per house rule 2: modules/trifactor/src/ClusterBuilder.cpp and its header are owned by a
// concurrent workflow replacing the grouping algorithm and are never touched here (only read, for the
// kReferenceProjScale constant and the ScreenErrorThreshold convention its header already documents).
//
// ================================================================================================
// TWO SELECTORS LIVE HERE. Know which one you are calling.
// ================================================================================================
// PER-CLUSTER (selectClusterCut, inLocalCut, buildMeshClusterViews) is the real thing: every cluster
// across EVERY level is tested independently, so one mesh draws at several levels at once -- the
// near face of a rock fine while its far side is coarser, in a single draw. Measured on a bumpy
// 96x24 plank: 3 distinct levels selected at a 2px budget, 5 at 4px, from one instance.
//
// The test is purely local, and needs no DAG walk at runtime:
//     draw this cluster  iff  ownError < budget  AND  parentError >= budget
// A cluster is drawn exactly when it is fine enough to be worth drawing and its parent is not, so
// across the whole DAG every surface is covered exactly once. That holds because the Cook guarantees
// ownError <= parentError for every cluster (validateClusterErrorBounds fails the cook otherwise),
// and TrifactorTest checks the covering property directly over 17 distinct budgets on a real DAG.
//
// PER-LEVEL (chooseLevel, chooseLevelCached, buildLevelClusterViews) picks ONE level for a whole
// mesh instance. That is ordinary discrete LOD with generated levels. It is kept because it is
// cheap, because it is what the landscape's descend() already does for terrain chunks, and because
// it is the honest baseline to measure per-cluster against -- not because it is the destination.
//
// AN EARLIER VERSION OF THIS HEADER said per-cluster selection was "NOT buildable from today's
// .ocmesh", and at the time that was true: fmt::OcMeshMeshlet carried no per-cluster error and no
// parent link, so only fmt::OcMeshLod's single per-level screenErrorThreshold was available. The
// format has since gained ownError/parentError per meshlet (MLET chunk version 2; version 1 files
// still load with safe defaults), which is what made the local test above possible. The claim is
// left recorded rather than deleted because it explains why chooseLevel exists at all.
//
// THE DEFAULTS ARE A TRAP, and it is worth knowing before debugging a slow frame. A version-1 file,
// or any mesh cooked before the format change, has ownError = 0 and parentError = FLT_MAX on EVERY
// cluster at EVERY level -- so the local test says yes to all of them and the whole hierarchy draws
// at once. That is not a subtle slowdown; it is every level of the DAG on screen simultaneously.
// Re-cook such assets rather than reaching for the per-level path to hide it.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/trifactor/ClusterSelect.hpp"
#include "aver/trifactor/ClusterBuilder.hpp"   // kReferenceProjScale; see the note below

#include <vector>

namespace aver::trifactor {

// kReferenceProjScale comes from ClusterBuilder.hpp, and is NOT redeclared here.
//
// It was, briefly, and that was a real compile error rather than a style point: the same
// `inline constexpr f32 kReferenceProjScale` in the same namespace aver::trifactor, in two headers,
// makes any translation unit that includes both fail on MSVC (C2374/C2086). It survived only
// because nothing in the tree happened to include both yet -- the first consumer to want the Cook's
// constant AND the runtime's adapter would have hit it.
//
// The duplicate was introduced to avoid depending on a header another workflow was editing. That
// was a reasonable instinct about churn and the wrong answer about correctness: two definitions of
// one physical constant can silently disagree after an edit to either, and this value is the shared
// reference frame the Cook's stored errors and the runtime's projection BOTH have to agree on. If
// they ever differed, every LOD decision would be wrong by exactly that ratio, everywhere, with
// nothing to indicate why.

// The world-space geometric error (centimetres) FORMAT_SPECS' ScreenErrorThreshold for LOD level
// `level` decodes to, i.e. the inverse of the Cook's `toScreenErrorThreshold`
// (worldErrorCm * kReferenceProjScale). Level 0 (the mesh's base/finest LOD, `mesh.indices` +
// `mesh.meshlets`) has no on-disk threshold at all -- its error is 0.0f by the same convention
// ClusterBuilder.cpp uses for level-0 Clusters (ClusterSelect.hpp's file header, point 3), which is
// exactly what makes level 0 always satisfy any non-negative pixel budget. `level` >= `mesh.lodCount()`
// is a caller error; this returns 0.0f for it rather than reading out of range.
f32 levelWorldErrorCm(const fmt::OcMeshData& mesh, u32 level);

// This level's own index count / 3. Level 0 is `mesh.indices`; level i>=1 is
// `mesh.coarserLods[i-1].indices`. Out-of-range `level` returns 0.
u32 levelTriangleCount(const fmt::OcMeshData& mesh, u32 level);

// Builds one ClusterView per meshlet of LOD `level`, decoding the on-disk snorm8 cone
// (value/127.0 -> [-1,1]) into the plain floats ClusterView's contract requires -- this file's only
// point of contact with the Cook's on-disk quantised form (see ClusterView's own comment for why that
// decode must happen before a ClusterView is filled at all). `id` is the meshlet's position in `out`
// (`out[id].id == out.size() ... id`, satisfying ClusterSelect.hpp's `clusters[id].id == id`
// requirement) -- but see the file header: `error`, `parents` and `children` are all left at their
// defaults (0.0f / empty / empty) on purpose, because LOD collapse already happened one level up, in
// chooseLevel. Every ClusterView this produces trivially passes `inCut` (error 0.0 <= any threshold,
// no parents to disqualify it); running it through selectVisibleClusters/selectVisibleClustersWithStats
// therefore does PURE frustum + cone culling, nothing else. Appends to `out`; does not clear it first.
void buildLevelClusterViews(const fmt::OcMeshData& mesh, u32 level, std::vector<ClusterView>& out);

// Chooses which whole LOD level to draw for ONE mesh instance this frame: the COARSEST level (largest
// index; coarserLods.back() is the DAG root) whose projected screen error -- using the INSTANCE's
// world-space bounding sphere, `screenSpaceErrorPx` from ClusterSelect.hpp, and `mesh`'s own per-level
// error from levelWorldErrorCm -- is still within `thresholdPx`. Levels are assumed monotonic
// non-decreasing in error with level index (the Cook's own invariant, validateScreenErrorMonotonic in
// ClusterBuilder.hpp -- not re-checked here, same trust relationship ClusterSelect.hpp's file header
// already states for the DAG's error monotonicity). Level 0 always qualifies (0.0f error), so this
// never returns an index >= mesh.lodCount(). A single-LOD mesh (mesh.lodCount() == 1) always returns 0.
u32 chooseLevel(const fmt::OcMeshData& mesh, const Vec3& worldSphereCenter, f32 worldSphereRadius,
                 f32 thresholdPx, const View& view);

// Same decision as chooseLevel above, but against a CACHED per-level error table instead of a live
// fmt::OcMeshData -- for a caller (SandboxApp is the one this slice has) that builds its per-LOD
// MeshHandles once at load time and does not keep the source OcMeshData resident afterwards.
// `levelWorldErrorCm[i]` must be `levelWorldErrorCm(mesh, i)` for the same mesh/level, `[0]` included
// (expected to be 0.0f, per that function's own contract, but not asserted here -- a caller populating
// this table already went through the real function once at load time).
u32 chooseLevelCached(const std::vector<f32>& levelErrorCmTable, const Vec3& worldSphereCenter,
                       f32 worldSphereRadius, f32 thresholdPx, const View& view);

// ================================================================================================
// PER-CLUSTER, at last -- the gap the file header above documents is CLOSED, because the format now
// carries what it was missing. modules/trifactor/src/ClusterBuilder.cpp's computeClusterErrorBounds
// (a concurrent phase's work, landed since the paragraph above was written) packs, on disk, exactly
// the two floats a purely-local cut test needs: OcMeshMeshlet::ownError/parentError (OcMesh.hpp).
// That means the per-cluster decision no longer needs `ClusterView::parents` or a DAG walk through
// `inCut` at all -- every cluster already carries its own answer to "how coarse am I" and "how coarse
// is whatever I'd collapse into", so the standard formulation applies directly, per cluster, with
// NOTHING but that one cluster's own two stored scalars and its own bounding sphere:
//     draw this cluster  iff  ownError < pixelBudget  AND  parentError >= pixelBudget
// This is what actually mixes LOD levels within one mesh instance's draw -- the thing chooseLevel
// above, by construction, cannot do. `inLocalCut`/`selectClusterCut` below are the implementation;
// `screenSpaceErrorPx`, `coneCull` and `Frustum` are reused UNCHANGED from ClusterSelect.hpp/.cpp,
// per the task's own instruction not to rewrite tested maths.
// ================================================================================================

// One cluster, from ANY level of a mesh's LOD DAG, as the per-cluster cut test needs to see it.
// Distinct from ClusterSelect.hpp's ClusterView (which is per-LEVEL and DAG-parent-id shaped): this
// carries the two PRE-CONVERTED screen-space-threshold scalars a local test reads directly, instead
// of an `error` field that needs a parent lookup to mean anything. A cluster's position in the
// array it lives in IS its id (mirrors OcMeshMeshlet's own array position) -- no separate id field,
// and no parents/children: the local test does not need them.
struct MeshClusterView {
    Vec3 sphereCenter{0, 0, 0};
    f32  sphereRadius = 0.0f;

    // Cone, already decoded to plain floats -- same convention as ClusterView::coneAxis/coneCutoff
    // (see that struct's comment for the +1.0f "never cull" / meshoptimizer sign-convention note).
    Vec3 coneApex{0, 0, 0};
    Vec3 coneAxis{0, 0, 1};
    f32  coneCutoff = 1.0f;

    // WORLD-SPACE-CENTIMETRE error, i.e. already divided by kReferenceProjScale -- the same units
    // levelWorldErrorCm returns for a whole level, but per cluster. screenSpaceErrorPx (reused
    // as-is) is what turns this into a pixel value for a given camera/distance; this file does that
    // conversion at selection time, never bakes a projected pixel value into the struct, because the
    // same cluster is selected fresh every frame against that frame's camera.
    f32 ownErrorCm = 0.0f;
    f32 parentErrorCm = std::numeric_limits<f32>::max();   // root: +FLT_MAX in, +FLT_MAX out (no
                                                            // rescale changes what a sentinel means)

    u32 triangleCount = 0;
    u32 level = 0;   // which LOD level this cluster came from -- informational for callers wanting
                      // per-level counts, and what the "distinct levels drawn" stat below counts.
};

// Builds one MeshClusterView PLUS its expanded GLOBAL triangle-index list (ready to concatenate
// straight into a CPU-assembled index buffer: `outIndices[i][k]` is a global index into `mesh`'s
// OWN vertex array, exactly what IDevice::createMesh's `indices` parameter wants), for EVERY
// meshlet across EVERY LOD level of `mesh` -- LOD 0's `mesh.meshlets` first, then each
// `mesh.coarserLods[j].meshlets` in ascending coarseness. Every level shares LOD 0's vertex buffer
// (OcMeshData::coarserLods' own documented contract), so a cluster from ANY level indexes the SAME
// vertex array -- no per-level remap needed, and a caller can freely mix clusters from different
// levels into one index buffer for one draw call, which is the entire point of this file existing.
//
// `outViews[i]` and `outIndices[i]` describe the SAME cluster (position `i` is that cluster's id in
// both arrays) -- appends to both, in lockstep, never clears either first. A malformed cluster (a
// triangle-local index pointing past `vertices.size()`) fails safe to global index 0 rather than
// reading out of bounds; it is not this function's job to validate the Cook's own output, only to
// not crash on it.
void buildMeshClusterViews(const fmt::OcMeshData& mesh, std::vector<MeshClusterView>& outViews,
                            std::vector<std::vector<u32>>& outIndices);

// ================================================================================================
// GPU-FACING cluster data, for an amplification+mesh-shader pair that culls and expands a cluster
// PER THREAD/GROUP instead of the CPU assembling one concatenated index buffer (buildMeshClusterViews'
// outIndices, above) every time the selected cut changes. Where buildMeshClusterViews EXPANDS every
// meshlet's triangles into global vertex indices up front (the CPU-assembly cost this whole design is
// trying to get off the critical path), this function keeps each meshlet's OWN local vertex/triangle
// block intact, only concatenating the blocks across levels and rebasing their offsets -- the flat,
// all-levels MeshletDesc/MeshletVertices/MeshletTriangles shape FORMAT_SPECS 5.7 already describes per
// LOD level, just spliced across every level of one mesh the same way buildMeshClusterViews splices
// MeshClusterView. `outBounds[i]`/`outDesc[i]` describe the SAME cluster i as buildMeshClusterViews'
// `outViews[i]` would for the same mesh -- same per-level order (LOD 0 first, then coarserLods
// ascending) -- so a cluster id means the same cluster whichever of the two functions produced it.
// ================================================================================================

// Where one cluster's geometry lives in the flat outVertices/outTriangles arrays below, rebased from
// the on-disk per-LOD-level MeshletDesc offsets (FORMAT_SPECS 5.7) to this mesh's whole, all-levels-
// concatenated buffers.
struct GpuMeshletDesc {
    u32 vertexOffset = 0;     // index into outVertices (element index, not bytes)
    u32 triangleOffset = 0;   // index into outTriangles (one element per TRIANGLE, not bytes)
    u32 vertexCount = 0;      // <= kMaxClusterVertices (64, ClusterBuilder.hpp)
    u32 triangleCount = 0;    // <= kMaxClusterTriangles (124, ClusterBuilder.hpp)
};

// outVertices: GLOBAL indices into the mesh's own (LOD-0-shared) vertex buffer, copied verbatim from
// OcMeshMeshlet::vertices -- already global, per OcMesh.hpp's own contract, so no decode happens here.
//
// outTriangles: one u32 per triangle, its three LOCAL indices (0..vertexCount-1, i.e. positions in
// THIS cluster's own outVertices slice, exactly what OcMeshMeshlet::triangles already stores) packed
// as `a | (b << 8) | (c << 16)`. A cluster's vertex count is capped at 64 (kMaxClusterVertices), so an
// 8-bit field never truncates a valid local index. A malformed on-disk triangle (a local index past
// this meshlet's own vertex count) fails safe to local index 0, the same discipline
// buildMeshClusterViews already uses for its own out-of-range guard -- it is not this function's job
// to validate the Cook's output, only to not read out of bounds because of it.
void buildMeshClusterGpuData(const fmt::OcMeshData& mesh, std::vector<MeshClusterView>& outBounds,
                              std::vector<GpuMeshletDesc>& outDesc, std::vector<u32>& outVertices,
                              std::vector<u32>& outTriangles);

// THE local cut test, exactly as the task brief states it, and PURELY LOCAL: reads only `c`'s own
// two stored scalars and its own bounding sphere -- no `clusters` array, no parent lookup, unlike
// ClusterSelect.hpp's inCut (which has to walk `c.parents` because its ClusterView was never given
// a precomputed parent error to read directly). That locality is exactly what makes this the right
// shape for a per-thread GPU test (an amplification shader could run this unmodified, one thread per
// cluster) even though this slice runs it on the CPU -- see the task report for why.
//
// `c.sphereCenter`/`c.sphereRadius` must already be in the SAME space as `view` compares against
// (world space for a real draw) -- this function does no transform of its own, same convention
// ClusterSelect.hpp's own selection functions use.
bool inLocalCut(const MeshClusterView& c, f32 thresholdPx, const View& view);


// THE BUDGET CLAMP BUG, fixed here and nowhere else -- inLocalCut is the only place that reads a
// caller-supplied threshold, so clamping here fixes every caller (selectClusterLocal,
// selectClusterCut, and the GPU amplification shader's own copy of this same constant, which the
// caller-side code that packs its per-instance budget must apply too -- see SandboxApp.cpp).
//
// AT thresholdPx == 0.0f, `ownPx >= thresholdPx` is true for EVERY cluster at EVERY level: ownPx is
// never negative (screenSpaceErrorPx floors distance at 0 and errorCm is never negative), so ownPx>=0
// always holds and inLocalCut rejects the entire DAG -- the whole mesh instance draws nothing. This
// is not a hypothetical: LOD 0's ownError is exactly 0.0f BY CONVENTION (OcMesh.hpp), so this is the
// FIRST budget a naive caller would try. kMinClusterBudgetPx is small enough to be visually
// indistinguishable from "no error tolerated" (a tenth of a pixel) while staying strictly positive.
inline constexpr f32 kMinClusterBudgetPx = 0.05f;

// inLocalCut, ANDed with frustum visibility and backface (cone) culling -- the full per-cluster
// draw/no-draw decision, mirroring ClusterSelect.hpp's selectCluster exactly in shape (three
// independent local tests composed by AND) but against the local cut instead of inCut.
bool selectClusterLocal(const MeshClusterView& c, f32 thresholdPx, const View& view,
                         const Frustum& frustum);

// The counts the task brief calls "the number that proves the feature": every cluster tested falls
// into EXACTLY ONE bucket (same discipline as ClusterSelect.hpp's SelectionStats, same priority
// order: frustum, then cone, then LOD), and `distinctLevels` is what actually answers "is this still
// discrete LOD with extra steps" -- the count of DIFFERENT `level` values among the clusters actually
// drawn. If a mesh instance's cut ever draws clusters from only one level, `distinctLevels == 1` and
// this is not yet virtualized geometry for that instance, whatever else it does.
struct ClusterCutStats {
    u32 tested = 0;
    u32 frustumCulled = 0;
    u32 coneCulled = 0;
    u32 lodRejected = 0;   // survived frustum+cone but failed inLocalCut (too coarse, or a cheaper
                            // parent already qualifies)
    u32 drawn = 0;
    u64 trianglesAfter = 0;         // sum of triangleCount over drawn clusters only
    u32 distinctLevels = 0;         // popcount of the level bitmask over DRAWN clusters -- see above
};

struct ClusterCutResult {
    std::vector<u32> drawnIds;   // positions into the `clusters` array passed in (== cluster ids)
    ClusterCutStats stats;
};

// Runs selectClusterLocal over every entry of `clusters` (which must already be in WORLD space --
// see selectClusterLocal), accumulating the stats above incrementally, one cluster at a time, from
// each cluster's own independent decision -- no second reconciliation pass, no sort, matching
// ClusterSelect.hpp's selectVisibleClustersWithStats' own "not a global pass" discipline exactly.
// A real per-cluster GPU path (an amplification shader) calls the equivalent of selectClusterLocal
// once per thread and never needs this loop; it exists for the CPU-assembly draw path and for tests.
ClusterCutResult selectClusterCut(const std::vector<MeshClusterView>& clusters, f32 thresholdPx,
                                   const View& view, bool useFrustum = true);

// ================================================================================================
// THE INSTANCE-LEVEL SHORTCUT -- skipping the O(all-DAG-clusters) scan above without approximating
// its answer. selectClusterCut is a flat scan over EVERY cluster in a mesh's whole DAG (129,666 for a
// re-cooked pine_tree_01), and on Electric Dreams it accounts for the large majority of a ~1.9 SECOND
// per-frame CPU cost in the per-cluster path, while the large majority of instances paying that cost
// get an answer chooseLevelCached (a handful of float ops) could have produced for a fraction of a
// percent of the price (measured: 95.3% of instances' cuts collapsed to one distinct level anyway).
// THIS SECTION EXISTS TO SKIP THE SCAN ONLY WHEN IT IS PROVABLE THE SCAN WOULD HAVE PRODUCED THE SAME
// ANSWER -- never as a similarity heuristic. See provablySingleLevelCut's own comment for the
// derivation and exactly what it refuses to assume.
// ================================================================================================

// Per LOD level, the actual per-cluster error range THIS MESH'S OWN clusters at that level carry --
// not levelWorldErrorCm's single representative scalar (which per-LEVEL selection uses, and which
// nothing here re-derives), the REAL min/max of ownErrorCm/parentErrorCm read directly off the
// cluster data buildMeshClusterViews produced. Computed once, at mesh load time, from data already
// resident -- never touched again per frame. `count == 0` means this mesh has no cluster at that
// level (should not happen for a level within range, but a malformed/short DAG fails safe by making
// the shortcut refuse to fire around that level rather than reading a meaningless min/max).
struct MeshClusterLevelBounds {
    u32 count = 0;
    f32 minOwnErrorCm = 0.0f, maxOwnErrorCm = 0.0f;
    f32 minParentErrorCm = 0.0f, maxParentErrorCm = 0.0f;
    u64 triangleCount = 0;   // sum of triangleCount over every cluster at this level -- what drawing
                             // the WHOLE level costs, the number a caller taking the shortcut needs to
                             // report the same triangle telemetry the real scan would have.
};

// Fills one entry per LOD level, indexed by level (`out[c.level]` for every `c` in `clusters`) --
// `out.size() == 1 + (the highest level any cluster carries)`. O(clusters.size()), meant to run ONCE
// per mesh at load time, immediately after buildMeshClusterViews, never per frame. Clears `outLevels`
// first.
void buildMeshClusterLevelBounds(const std::vector<MeshClusterView>& clusters,
                                  std::vector<MeshClusterLevelBounds>& outLevels);

// Can the caller SAFELY skip selectClusterCut's full DAG scan and just draw `candidateLevel` (e.g.
// chooseLevelCached's own answer) whole, trusting that is EXACTLY what the real per-cluster scan
// would have produced for this instance this frame? Returns true only when that is PROVEN, never when
// it merely seems likely -- a caller that gets false back MUST run the real scan; this function is
// allowed to be conservative (refuse to prove a true case) but must never be wrong in the other
// direction (never claim a false case is provably true).
//
// THE PROOF, in full, because asserting it without showing it is exactly what this codebase's own
// house rules exist to catch:
//
// inLocalCut(c) reduces, algebraically, to one inequality chain against a SINGLE per-cluster
// quantity (see ClusterAdapt.cpp's inLocalCut): c is drawn iff
//     ownErrorCm(c)  <  thresholdPx * dist(c) / projScale  <=  parentErrorCm(c)
// where dist(c) is c's OWN distance to the camera (screenSpaceErrorPx's radial distance-to-surface).
// Both halves compare against the SAME dist(c) -- ownPx and parentPx in inLocalCut share one sphere,
// one distance, differing only in which error scalar they multiply -- so nothing here needs to reason
// about a cluster's position beyond bounding where dist(c) can possibly fall.
//
// Every cluster in this mesh's DAG lies inside the INSTANCE's own world bounding sphere (the same
// sphere the per-level path already computes from the entity's world AABB), give or take that
// cluster's own radius, so for ANY cluster c, regardless of which level or exactly where in the mesh:
//     dMin <= dist(c) <= dMax
//     dMin = max(0, dist(eye, instanceCenter) - instanceRadius - maxClusterSphereRadius)
//     dMax =        dist(eye, instanceCenter) + instanceRadius
// (dMax needs no cluster-radius term: distance-to-surface only ever shrinks with radius, so the
// centre-to-centre distance alone is already a valid upper bound on it.)
//
// That turns the per-cluster inequality into a per-LEVEL one, because thresholdPx*dist(c)/projScale
// is monotonic in dist(c) and dist(c) is now bounded, not exact:
//   - `candidateLevel` is drawn ENTIRELY (every one of its clusters, unconditionally) if even the
//     WORST-case own-error (maxOwnErrorCm, at the closest possible distance dMin) still clears the
//     budget, AND even the WORST-case parent-error (minParentErrorCm, at the farthest possible
//     distance dMax) still exceeds it:
//         maxOwnErrorCm[L]    * projScale  <  thresholdPx * dMin
//         minParentErrorCm[L] * projScale  >= thresholdPx * dMax
//   - every OTHER level L' contributes NOTHING if either its own error is too large everywhere in the
//     envelope (minOwnErrorCm[L'] * projScale >= thresholdPx * dMax -- even the BEST case, farthest
//     distance, still fails the own-error test) or its parent error is too small everywhere in the
//     envelope (maxParentErrorCm[L'] * projScale < thresholdPx * dMin -- even the BEST case, closest
//     distance, still fails the parent-error test).
// Checked for EVERY level (not just neighbours of `candidateLevel`): the per-level min/max table is
// small (a few dozen entries even after today's re-cook) next to the per-cluster scan it replaces, so
// checking all of them costs nothing next to that -- there is no reason to lean on an unproven "only
// adjacent levels can matter" assumption to save a few dozen comparisons.
//
// WHAT THIS DOES NOT ASSUME, on purpose: nothing about which level a cluster's parent lives in, no DAG
// topology, no "error is monotonic across levels" beyond what is already documented as a GIVEN
// invariant elsewhere in this module (not leaned on here at all -- every level is checked
// independently, on its own actual min/max). If dMin cannot be bounded away from zero (the camera is
// within `maxClusterSphereRadius` of the instance) this returns false rather than reasoning about the
// near-zero regime screenSpaceErrorPx special-cases with its own sentinel.
bool provablySingleLevelCut(const std::vector<MeshClusterLevelBounds>& levels, u32 candidateLevel,
                             const Vec3& instanceSphereCenter, f32 instanceSphereRadius,
                             f32 maxClusterSphereRadius, f32 thresholdPx, const View& view);

} // namespace aver::trifactor
