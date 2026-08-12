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

#include <limits>
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

    // Which connected SHELL (a position-coincidence-closed connected component of the SOURCE mesh --
    // see computeShellIds in ClusterBuilder.cpp) this cluster is associated with: at level 0, the
    // shell its FIRST triangle's first vertex belongs to.
    //
    // EXACT FOR A SMALL-SHELL CLUSTER, A REPRESENTATIVE FOR A LARGE ONE -- and that asymmetry is by
    // construction, not an oversight. Task step 5's routing (buildClusters, ClusterBuilder.cpp) splits
    // LOD-0 triangles into two disjoint streams before either ever reaches a clustering call: every
    // small shell (LodDag::isSmallShell) is built into its OWN cluster directly, containing that
    // shell's triangles and nothing else, so shellId names its single shell exactly. Every large-shell
    // triangle instead goes into one shared buffer handed to meshopt_buildMeshlets, which has no
    // notion of "shell" and is free to (and, for spatially-close large shells, will) put triangles
    // from more than one LARGE shell in the same meshlet -- so a large cluster's shellId is only ONE
    // of the shells it may contain. That is harmless for what shellId is actually used for: the
    // routing below only ever asks "is every shell touching this cluster large?", and a large cluster
    // can only ever contain LARGE-shell triangles (the small-shell stream never reaches
    // meshopt_buildMeshlets at all), so the representative's own classification -- large -- is always
    // the right answer even when the specific shell named is not the only one present. See
    // smallShellLineage just below for the field that actually drives the routing decision, rather
    // than relying on this exactness distinction being re-derived at every call site.
    //
    // shellId is populated at level 0 ONLY (by buildClusters). A level >= 1 cluster's shellId stays at
    // its default (0) and MUST NOT be read as meaning anything -- unlike shellId, smallShellLineage
    // IS propagated to every level (see its own comment), which is what buildLodHierarchy's routing
    // actually consults above level 0.
    //
    // BUILD-TIME ONLY: a shell is a property of the source mesh's own topology, re-derivable from
    // mesh.positions/indices at any time, not a fact about the cooked cluster hierarchy that needs to
    // outlive this build. Never serialized; nothing on disk changes because this field exists.
    u32 shellId = 0;

    // TRUE iff this cluster is, or descends ENTIRELY from, small-shell geometry -- the field task step
    // 7's meshopt_simplify call and task step 6's two-bucket grouping actually consult, at EVERY level,
    // not just level 0. This is deliberately a separate field from shellId above rather than a
    // "dag.isSmallShell(shellId)" lookup, for the reason step 8 exists to guard against: past level 0,
    // shellId is not populated (see its own comment), so looking it up at level >= 1 would silently
    // read a stale default -- and getting the DEFAULT direction of that mistake right is exactly why
    // this field, not shellId's, is the one the routing reads.
    //
    // AT LEVEL 0 (buildClusters): true for a cluster built directly from one small shell (task step
    // 5), false for a cluster built by meshopt_buildMeshlets from the large-shell stream (which, per
    // shellId's comment above, can only ever hold large-shell triangles).
    //
    // AT LEVEL >= 1 (buildLodHierarchy's Pass 2): copied from the PendingGroup that produced this
    // cluster -- specifically, `!pg.allLargeShell` (see buildLodHierarchy's own comment on
    // PendingGroup). A group's members are always homogeneous in this field by construction (task
    // step 6's two buckets are partitioned separately and never concatenated), so "the group's
    // lineage" is a single well-defined value, not a per-member vote.
    //
    // THE DEFAULT (false) IS THE SAFE DIRECTION, on purpose, matching shellId's own "no meaning yet"
    // default and validateClusterErrorBounds' root-sentinel convention of failing toward "keep
    // protecting" rather than "start dropping protection". If this propagation step were ever skipped
    // or got a level wrong, every affected cluster would default to false -- i.e. get routed through
    // the LockBorder path regardless of its real lineage -- which only costs back some of the win this
    // feature exists for. The dangerous direction (a large-shell descendant silently read as
    // small-shell, and having LockBorder dropped under it) would require this field to default to
    // TRUE, which it does not. TrifactorTest's mixed-shell multi-level fixture exists specifically to
    // prove the propagation itself is happening -- not merely relying on this default -- by tracing a
    // large-shell descendant's lineage down to level 3+ and confirming it never flips.
    //
    // BUILD-TIME ONLY, like shellId: never serialized.
    bool smallShellLineage = false;

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

    // Per-shell classification from computeShellIds (ClusterBuilder.cpp), indexed by Cluster::shellId:
    // smallShells[s] is true iff shell s is SMALL (task step 4 -- fits inside a single meshlet, i.e.
    // at most kMaxClusterVertices vertices and kMaxClusterTriangles triangles). That definition is
    // chosen because it is PROVABLE, not tunable: such a shell can never be split across a group
    // boundary by meshopt_buildMeshlets, so a group containing only small shells contains each of
    // them WHOLE -- exactly the condition that makes dropping meshopt_SimplifyLockBorder on that
    // group safe (see the comment at buildLodHierarchy's meshopt_simplify call site).
    //
    // BUILD-TIME ONLY: populated by buildClusters, and never written to a file -- like Cluster::shellId
    // above, a shell is re-derivable from the source mesh at any time, not a fact the cooked hierarchy
    // needs to carry. CONSULTED ONLY AT LEVEL 0, by buildClusters itself, to decide which stream (the
    // direct small-shell path or the meshopt_buildMeshlets large-shell path) each triangle takes and
    // to set the resulting cluster's Cluster::smallShellLineage -- buildLodHierarchy's own routing
    // (task steps 6-7) reads smallShellLineage, not this vector, precisely because a level >= 1 group
    // can span several small shells at once and "is small" stops being a single shellId lookup once
    // that happens (see Cluster::smallShellLineage's comment for the propagation that field carries
    // instead).
    std::vector<u8> smallShells;

    // Bounds-checked so a stale or out-of-range shellId (there should never be one, but this is the
    // one place a routing bug would show up as an out-of-bounds read instead of a wrong LockBorder
    // decision) reads as "not small" -- the SAFE direction, since it means "keep LockBorder", never
    // "drop it".
    bool isSmallShell(u32 shellId) const { return shellId < smallShells.size() && smallShells[shellId] != 0; }

    u32  levelCount() const { return static_cast<u32>(levels.size()); }
    bool empty() const { return clusters.empty(); }
};

// Partitions `mesh`'s full-resolution triangle list into LOD-0 clusters (meshopt_buildMeshlets,
// CORRECTION 1) and computes each cluster's bounding sphere + cone (meshopt_computeMeshletBounds),
// quantized CONSERVATIVELY into the spec's snorm8 cone encoding (see quantizeConeConservative in
// the .cpp for the rounding argument -- it is the part of this task most likely to be silently
// gotten wrong). Populates dag.levels[0] only; does not build LOD > 0.
//
// ALSO CLASSIFIES `mesh` into connected shells (computeShellIds in the .cpp) and ROUTES on the result
// (task step 5): every small shell (LodDag::isSmallShell) is built into its own cluster directly,
// bypassing meshopt_buildMeshlets entirely (it is defined to fit inside one meshlet, so partitioning
// machinery sized for the whole mesh has nothing to add and, worse, meshopt_buildMeshlets is free to
// pull in a spatially-close but topologically-unrelated shell once a shell's own adjacency runs out --
// see buildDirectCluster's comment in the .cpp); every large-shell triangle instead goes through the
// SAME meshopt_buildMeshlets call this function always made, on a triangle stream that is
// ORDER-PRESERVING with respect to `mesh.indices` (large-shell triangles keep their original relative
// order; only small-shell triangles are pulled out of it). For a mesh with ZERO small shells -- which
// covers every one of this engine's demo corpus's previously-LockBorder-protected solid meshes, see
// the file header's measured table -- that stream is therefore mesh.indices verbatim, so this
// function's clustering output for such a mesh is EXACTLY what it would have been before this routing
// existed: not merely equivalent, the identical meshopt_buildMeshlets call on the identical buffer.
// That equivalence, not a runtime check, is what protects those meshes; TrifactorTest's
// zero-small-shell fixture exists to keep it true rather than merely argued.
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

// ---- per-cluster error, the pair OcMeshMeshlet::ownError/parentError (OcMesh.hpp) persists --------
//
// One cluster's packed screen-space error pair, indexed exactly like dag.clusters (bounds[c.id]
// belongs to dag.clusters[c.id]). This is what turns per-LEVEL selection (LodDesc.ScreenErrorThreshold,
// one value for a whole LOD) into a per-CLUSTER local cut test:
//     draw this cluster  iff  ownError < pixelBudget  AND  parentError >= pixelBudget
struct ClusterErrorBounds {
    f32 ownError    = 0.0f;
    f32 parentError = std::numeric_limits<f32>::max();
};

// Computes ownError/parentError for every cluster in `dag`, converting Cluster::error through
// toScreenErrorThreshold(_, scale) -- the SAME conversion validateScreenErrorMonotonic re-checks, so
// this inherits its monotonicity guarantee rather than asserting a new one.
//
// A cluster's ROOT-ness is `c.level == dag.levelCount() - 1` -- the same definition validateLodDag
// and LodDag's own doc comment use for "coarsest level" -- not "c.parents.empty()": that keeps this
// function's root/non-root split from silently agreeing with a dag-connectivity bug (a non-root
// cluster that wrongly has no parents) instead of exposing it. A root gets
// parentError = +FLT_MAX (a finite sentinel, not IEEE +inf -- see OcMeshMeshlet's own comment on
// why): the local cut test must always accept a root once nothing finer already qualified, and a
// finite value here would make a distant root silently stop drawing.
//
// For a non-root cluster with MORE THAN ONE parent (splitIntoClusters, called from
// buildLodHierarchy, can produce more than one new cluster per simplified group when
// meshopt_buildMeshlets' 64-vertex/124-triangle limits force a re-split), parentError is the MAX
// over every parent's converted error, not parents[0]. Today every parent from the same group
// carries the exact SAME propagatedError -- buildLodHierarchy assigns one shared local variable to
// every one of a group's newIds (see its own comment) -- so max() and parents[0] agree numerically.
// max() is used anyway because it stays correct even if that equality ever stops holding (a future
// per-newId error computation): picking parents[0] blindly could then under-report parentError by
// grabbing a smaller sibling's error, which is exactly the "holes in the mesh" failure mode the
// local cut test has no way to detect on its own -- see validateClusterErrorBounds, which is the
// backstop that catches ownError > parentError before it reaches a file.
//
// If `dag` is empty this returns an empty vector.
std::vector<ClusterErrorBounds> computeClusterErrorBounds(const LodDag& dag, f32 scale);

// Validates the local cut test's entire correctness argument, over EVERY cluster in `dag`:
//   - ownError <= parentError (the property the local test's "covers every surface exactly once"
//     claim rests on -- see computeClusterErrorBounds's doc comment)
//   - a cluster is a root (c.level == dag.levelCount() - 1) IFF its parentError is exactly +FLT_MAX
//     (checks the SENTINEL actually made it through, not merely that some large value did, and
//     equally flags a non-root that was wrongly given the "always draw" root sentinel)
// `bounds` must come from computeClusterErrorBounds(dag, scale) for this same `dag` (indexed the
// same way). Returns false and sets `why` to the first violation found, loudly, rather than letting
// a violation reach a file: per the local test's own definition, a cluster with ownError >
// parentError could be skipped alongside its ancestor at some pixel budget (a hole), or a wrongly
// non-infinite root could vanish at distance, or a wrongly infinite non-root could double-draw
// alongside its ancestor.
bool validateClusterErrorBounds(const LodDag& dag, const std::vector<ClusterErrorBounds>& bounds,
                                 std::string* why = nullptr);

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
