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

} // namespace aver::trifactor
