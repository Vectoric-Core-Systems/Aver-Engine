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
// PER-LEVEL, NOT PER-CLUSTER -- read this before wiring anything else to this file.
// ================================================================================================
// Two scouts independently read the on-disk format against ClusterSelect.hpp's actual requirements
// and reached the same conclusion: true per-CLUSTER selection (ClusterSelect's `inCut`, which needs
// `ClusterView::parents` to name specific coarser clusters and a genuinely per-cluster `error`) is
// NOT buildable from today's .ocmesh. `fmt::OcMeshMeshlet` (OcMesh.hpp) carries no parent/child id
// and no per-cluster error at all; `fmt::OcMeshLod` carries exactly ONE `screenErrorThreshold` for
// its WHOLE LEVEL. Reconstructing per-cluster parent links from geometry alone (sphere containment
// across adjacent levels) is not a proven property of buildLodHierarchy's re-split groups and was
// explicitly flagged as an unproven heuristic, not a design this slice may lean on.
//
// So this file implements the OTHER option the scouts left buildable today: PER-LEVEL selection.
// `chooseLevel` below picks ONE whole LOD level per mesh INSTANCE per frame -- the coarsest level
// whose projected screen error, using the mesh's own bounding sphere, is still within budget -- the
// exact same shape of decision modules/landscape/src/LandscapeTree.cpp's descend() already makes for
// terrain chunks, and the exact metric (screenErrorPx = worldErrorCm * projScale / distanceCm,
// projScale = kReferenceProjScale = 540) the Cook computed ScreenErrorThreshold against.
//
// WHAT THIS BUYS: real triangle reduction, real frame-time reduction, LOD granularity that changes
// with distance -- the whole point of the Cook's coarserLods existing at all.
// WHAT THIS DOES NOT DO, and does not pretend to: it does not mix clusters from different LOD levels
// within one instance (an object near the camera on one side and far on the other still moves as one
// LOD step), and `inCut`'s per-cluster DAG-cut logic is exercised WITHIN one already-chosen level with
// every cluster's own error fixed at 0.0f and no parents wired up -- i.e. every meshlet of the chosen
// level trivially passes the cut test, and only frustum + cone culling (buildLevelClusterViews'
// ClusterView output, run through the real, tested selectVisibleClustersWithStats) does any per-
// cluster rejection. Call this what it is: LEVEL selection plus CLUSTER culling, not cluster-level LOD
// collapse. A future change adding real per-cluster parent links to the on-disk format (a Cook change,
// out of this file's reach) is what closes that gap; it is not done here.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcMesh.hpp"
#include "aver/trifactor/ClusterSelect.hpp"

#include <vector>

namespace aver::trifactor {

// The Cook's own reference projScale, reproduced here as a plain constant rather than included from
// ClusterBuilder.hpp's constant of the same name/value (kReferenceProjScale = 540.0f, derived from a
// 1080px/90deg reference camera -- see ClusterBuilder.hpp's own derivation comment). This file COULD
// include ClusterBuilder.hpp for it (reading it is allowed; only ClusterBuilder.cpp/.hpp EDITS are
// off limits) but keeping the value local avoids this adapter depending on a header a concurrent
// workflow is actively restructuring for anything beyond documentation.
inline constexpr f32 kReferenceProjScale = 540.0f;

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

} // namespace aver::trifactor
