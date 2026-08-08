#pragma once
// Trifactor -- the virtualized-geometry cluster builder and LOD DAG.
// docs/VIRTUALIZED_GEOMETRY.md section 7 "Slice 0" is the plan; this header follows it under three
// corrections recorded at the call site that spawned this module (the full text is repeated at the
// top of src/ClusterBuilder.cpp, which is also where the single most important thing about this
// file lives: meshoptimizer, which every function below calls into, is NOT vendored in this tree
// yet -- read that comment before assuming this compiles).
//
// SCOPE: this is the offline, cook-time half only. It reads an OcMeshData already at full
// resolution (LOD 0) and produces an in-memory cluster/DAG model. Packing that model into the MLET
// chunk bytes (FORMAT_SPECS.md 5.7) and writing tests against it is the NEXT phase's job; nothing
// here touches modules/formats/src/OcMesh.cpp.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcMesh.hpp"

#include <string>
#include <vector>

namespace aver::trifactor {

// MLET spec limits (FORMAT_SPECS.md 5.7). Both meshopt_buildMeshlets call sites in this module are
// sized to these exactly -- they ARE the mesh-shader/DXR contract, not a tunable.
inline constexpr u32 kMaxClusterVertices  = 64;
inline constexpr u32 kMaxClusterTriangles = 124;

// FORMAT_SPECS.md 5.7 MeshletBounds (32 B): Sphere (16 B) + ConeApex f32[3] (12 B) + ConeAxis i8[3]
// snorm + ConeCutoff i8 snorm. A distinct struct (not the vendor's meshopt_Bounds) so the ON-DISK
// shape is visible at the type level; the next phase's serializer writes these fields byte for byte.
struct ClusterBounds {
    Vec3 sphereCenter{0, 0, 0};
    f32  sphereRadius = 0.0f;
    Vec3 coneApex{0, 0, 0};
    i8   coneAxis[3] = {0, 0, 0};   // snorm8: value/127.0 -> [-1,1]
    i8   coneCutoff  = -127;        // snorm8; -127 (not -128) is the conservative "never cull" value,
                                     // symmetric with +127 so it decodes back to exactly -1.0
};

// One meshlet/cluster. `vertices`/`triangles` are stored EXACTLY as FORMAT_SPECS.md 5.7 wants them
// on disk: `vertices[i]` is a GLOBAL index into the source mesh's vertex buffer (mesh.positions,
// the same buffer at every LOD level -- see ClusterBuilder.cpp for why that is load-bearing), and
// `triangles` holds LOCAL indices (0..vertices.size()-1), three per triangle, into `vertices`.
struct Cluster {
    u32 id    = 0;   // index into LodDag::clusters
    u32 level = 0;   // LOD level; 0 = source resolution

    std::vector<u32> vertices;    // global vertex indices, size() <= kMaxClusterVertices
    std::vector<u8>  triangles;   // local indices, 3 per triangle, count <= kMaxClusterTriangles*3

    ClusterBounds bounds;

    // Geometric simplification error accumulated from this cluster down to LOD 0 (max of this
    // group's own simplification error and every child's error -- see buildLodHierarchy). THIS IS
    // NOT YET A SCREEN-SPACE PIXEL VALUE: converting it to FORMAT_SPECS' ScreenErrorThreshold needs
    // a reference resolution/FOV projection this offline slice does not own (that conversion is
    // slice 5's "Screen-space error compute" in VIRTUALIZED_GEOMETRY.md). It is monotonic
    // (guaranteed non-decreasing child -> parent) in whatever units meshopt_simplify returns, which
    // is what the DAG-correctness invariant in the task actually requires.
    f32 error = 0.0f;

    std::vector<u32> parents;    // cluster ids one level coarser that this cluster feeds into
    std::vector<u32> children;   // cluster ids one level finer that feed into this cluster

    u32 triangleCount() const { return static_cast<u32>(triangles.size() / 3); }
};

// The whole hierarchy for one mesh. `clusters` is flat across all levels; `levels[k]` lists the
// cluster ids belonging to LOD k. The coarsest (root) level is `levels.back()`.
struct LodDag {
    std::vector<Cluster> clusters;
    std::vector<std::vector<u32>> levels;

    u32  levelCount() const { return static_cast<u32>(levels.size()); }
    bool empty() const { return clusters.empty(); }
};

// Partitions `mesh`'s full-resolution triangle list into LOD-0 clusters (meshopt_buildMeshlets,
// CORRECTION 1) and computes each cluster's bounding sphere + cone (meshopt_computeMeshletBounds),
// quantized CONSERVATIVELY into the spec's snorm8 cone encoding (see quantizeConeConservative in
// the .cpp for the rounding argument -- it is the part of this task most likely to be silently
// gotten wrong). Populates dag.levels[0] only; does not build LOD > 0.
//
// Returns false and sets `why` on a malformed mesh (empty positions/indices, an index count not a
// multiple of 3, or an index out of range for the vertex buffer). Does not otherwise validate mesh
// content -- see validateLodDag for the full invariant check.
bool buildClusters(const fmt::OcMeshData& mesh, LodDag& dag, std::string* why = nullptr);

// Extends `dag` (which must already hold LOD 0, i.e. came out of buildClusters) with coarser LOD
// levels, iterating GROUP -> SIMPLIFY (locked group boundary) -> RE-SPLIT until a level stops
// reducing triangle count or a hard safety cap on level count is hit.
//
// `mesh` must be the SAME mesh buildClusters was called with. Every level this function builds is
// simplified and re-split against `mesh.positions` directly, never a per-group copy -- that single
// decision is what keeps a boundary vertex's position bit-identical at every LOD, which is the
// mechanism the crack-free invariant rests on. See the top of ClusterBuilder.cpp.
bool buildLodHierarchy(const fmt::OcMeshData& mesh, LodDag& dag, std::string* why = nullptr);

// One validation failure: `where` names which invariant, `detail` is the specific instance.
struct ValidationIssue {
    std::string where;
    std::string detail;
};

struct ValidationReport {
    bool ok = true;
    std::vector<ValidationIssue> issues;
    void fail(std::string where, std::string detail) {
        ok = false;
        issues.push_back({std::move(where), std::move(detail)});
    }
};

// Checks every invariant the task lists:
//  - every triangle of `mesh` appears in exactly one LOD-0 cluster (no gaps, no duplicates)
//  - no cluster exceeds kMaxClusterVertices / kMaxClusterTriangles
//  - every cluster has a finite bounding sphere and a valid (non-degenerate, in-range) cone
//  - error is monotonic child -> parent across every DAG edge
//  - the DAG is acyclic and every non-root cluster (every cluster not in dag.levels.back()) has at
//    least one parent
ValidationReport validateLodDag(const fmt::OcMeshData& mesh, const LodDag& dag);

// ---- geometric error -> FORMAT_SPECS' ScreenErrorThreshold ------------------------------------
//
// Cluster::error (above) is deliberately left in meshopt's own relative units. This is the
// conversion that field's own comment said was deferred: the projection into FORMAT_SPECS.md 5.5's
// ScreenErrorThreshold (a distance-independent, screen-space-px quantity a runtime LOD selector can
// compare a threshold against).
//
// The runtime formula this assumes -- already shipping for landscape chunks, see
// modules/landscape/src/LandscapeTree.cpp's `descend()` -- is
//     screenErrorPx = worldErrorCm * projScale / distanceCm
//     projScale     = viewportHeightPx / (2 * tan(fovY / 2))
// i.e. a perspective-projection falloff: a fixed-size defect subtends fewer pixels the farther away
// it is. `distanceCm` is per-frame, per-camera and NOT known at cook time, so what gets stored on
// disk is the distance-independent half of that product: `worldErrorCm * projScale`. A runtime under
// the SAME reference projScale this was computed with can then recover the actual screen error with
// a single divide (`screenErrorPx = ScreenErrorThreshold / distanceCm`); one under a different
// viewport/FOV rescales first by `(actualProjScale / kReferenceProjScale)`.
//
// REFERENCE CONDITIONS, pinned here because (per the task that added this) "a threshold means
// nothing without them": 1080 px reference viewport height, 90-degree reference vertical FOV, which
// gives
//     kReferenceProjScale = 1080 / (2 * tan(45 deg)) = 1080 / 2 = 540.0f
// chosen to EQUAL modules/landscape/include/aver/landscape/LandscapeTree.hpp's own
// `SelectParams::projScale` default (540.0f) on purpose -- this is the one metric already shipping
// in this engine for the same problem shape (bounding sphere + precomputed error, projected via
// distance-to-near-surface and projScale), and inventing a second reference here would be exactly
// the "two LOD metrics in one engine" trap a mesh-cluster LOD selector must not fall into.
inline constexpr f32 kReferenceViewportHeightPx = 1080.0f;
inline constexpr f32 kReferenceFovYRadians       = kPi / 2.0f;   // 90 degrees
inline constexpr f32 kReferenceProjScale         = 540.0f;       // see the derivation above

// meshopt_simplify's `result_error` (what Cluster::error holds) is RELATIVE to the mesh's own
// bounding-box max-axis extent, not an absolute distance -- see meshoptimizer.h's
// meshopt_simplifyScale doc and ClusterBuilder.cpp's buildLodHierarchy, which never sets
// meshopt_SimplifyErrorAbsolute. Every meshopt_simplify call in buildLodHierarchy is against the
// SAME `mesh.positions`/vertexCount (the whole mesh, never a per-group subset -- see the comment on
// Cluster::error), so this scaling factor is ONE constant for a whole mesh's DAG, safe to compute
// once and reuse for every cluster's error.
f32 worldExtentScale(const fmt::OcMeshData& mesh);

// Converts one Cluster::error value into FORMAT_SPECS' ScreenErrorThreshold units, given `scale`
// from worldExtentScale(mesh) (the SAME mesh the error was computed against). This is
//     absoluteErrorCm      = clusterError * scale
//     screenErrorThreshold = absoluteErrorCm * kReferenceProjScale
// A single multiply by two positive constants, so it is strictly monotonic in `clusterError`: the
// DAG's `parent.error >= child.error` invariant (buildLodHierarchy, validateLodDag) therefore
// survives the conversion automatically. validateScreenErrorMonotonic below re-checks this on the
// CONVERTED values regardless, per the rule that this must be asserted AFTER conversion, not
// inferred from the raw values' own invariant.
f32 toScreenErrorThreshold(f32 clusterError, f32 scale);

// Re-checks error-monotonicity (parent's converted screen error >= every child's, across every DAG
// edge) AFTER projecting every cluster's Cluster::error through toScreenErrorThreshold. Mirrors
// validateLodDag's "error-monotonicity" check exactly, but on the value a runtime will actually
// compare against a pixel budget, not on the raw geometric error -- the two are computed by
// different code and a future change to the conversion (a non-linear projection, a per-cluster
// scale, etc.) should not be trusted to preserve monotonicity just because the raw error does.
bool validateScreenErrorMonotonic(const LodDag& dag, f32 scale, std::string* why = nullptr);

// Reduces `mesh` in place to roughly `ratio` of its triangles (0 < ratio < 1), rewriting positions,
// normals, UVs and indices. Returns false and leaves the mesh UNTOUCHED if the input is unusable or
// the simplifier could not reach anywhere near the target.
//
// WHY THIS EXISTS, and what it is not. It is not virtualized geometry -- it is the blunt instrument
// that makes photogrammetry usable before virtualized geometry lands. Measured on this tree: frame
// time is linear in drawn triangles at roughly 2.1 ms per million, so a 6.95-million-triangle scan
// placed fourteen times costs about 200 ms a frame on its own, and no amount of frustum culling
// helps because the triangles are genuinely on screen. They are just far smaller than a pixel,
// which is precisely the case docs/VIRTUALIZED_GEOMETRY.md §3.5 is about: the hardware rasterizer
// shades in 2x2 quads, so a sub-pixel triangle wastes three quarters of the work it triggers.
//
// The real fix is picking a LOD per cluster on the GPU (that plan's slices 1-5), for which
// buildLodHierarchy above already computes the hierarchy and the error metric. This function is the
// stopgap that does not need any of it: ONE decimation, at cook time, for the whole mesh.
//
// SEAMS ARE NOT PROTECTED HERE, deliberately. buildLodHierarchy locks group borders because
// neighbouring clusters must still meet; a whole-mesh decimation has no neighbour to meet, so
// locking its outer border would only prevent the silhouette from ever simplifying. If this is ever
// used on something that tiles against another mesh, that assumption stops holding.
bool simplifyMesh(fmt::OcMeshData& mesh, f32 ratio, std::string* why = nullptr);

} // namespace aver::trifactor
