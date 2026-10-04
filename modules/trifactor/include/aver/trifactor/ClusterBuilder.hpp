#pragma once
// Trifactor -- the virtualized-geometry cluster builder and LOD DAG.
// docs/VIRTUALIZED_GEOMETRY.md section 7 "Slice 0" is the plan (see src/ClusterBuilder.cpp's header
// for the three corrections). meshoptimizer, used by every function below, is NOT vendored in this
// tree yet -- read that comment before assuming this compiles.
//
// SCOPE: offline, cook-time only. Reads a full-resolution (LOD 0) OcMeshData and produces an
// in-memory cluster/DAG model; packing it into MLET chunk bytes (FORMAT_SPECS.md 5.7) and writing
// tests against it is the next phase's job -- nothing here touches modules/formats/src/OcMesh.cpp.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcMesh.hpp"

#include <limits>
#include <string>
#include <vector>

namespace aver::trifactor {

// MLET spec limits (FORMAT_SPECS.md 5.7); both meshopt_buildMeshlets call sites are sized to these
// exactly -- they ARE the mesh-shader/DXR contract, not a tunable.
//
// Aliases, not literals: this module depends on Aver.Formats, so the on-disk contract owns the
// number. Used to be independent 64/124 in two (really three, counting an HLSL #define) files,
// agreeing only by nobody changing one -- keep this the single source.
inline constexpr u32 kMaxClusterVertices  = fmt::kMaxMeshletVertices;
inline constexpr u32 kMaxClusterTriangles = fmt::kMaxMeshletTriangles;

// ---- builder version stamp (Stage 2, Part B) ----------------------------------------------------
//
// Names which cook-algorithm revision produced a mesh's meshlets/coarserLods; persisted in
// .ocmesh's MHDR.Reserved (FORMAT_SPECS.md 5.1, repurposed as BuilderVersion -- see
// OcMeshData::builderVersion for read/write, packLodDag for where it is stamped). Exists because
// commit 14ba2b7 changed buildClusters/buildLodHierarchy's output without changing any existing
// file's bytes, leaving no way to tell a ladder's currency without re-cooking and diffing; also a
// future DDC key ("recompute" vs "reuse" without touching the simplifier).
//
// BUMP whenever a change would produce DIFFERENT meshlets/coarserLods for some mesh (routing,
// simplifier flag/target, grouping, quantization) -- not for comments, logging, an
// output-identical refactor (e.g. lifting toMeshlets/toIndices into packLodDag: same bytes in,
// same bytes out), or changes confined to validateLodDag/validateClusterErrorBounds (they check
// the DAG, they do not build it).
//
// STARTS AT 1: every pre-existing .ocmesh has Reserved == 0 (FORMAT_SPECS.md 2.3), so 0 already
// means "no version-aware builder touched this ladder". Version 1 is commit 14ba2b7's
// shell-routing-aware algorithm (corpus coarsest 1,195,431 -> 140,489; five protected meshes
// unmoved) -- the first version this field can record.
inline constexpr u32 kBuilderVersion = 2;

// ---- Stage 4: streaming topology (fallbackAncestorId, ownerGroupId, ClusterGroupNode) ----------
//
// kBuilderVersion is NOT bumped for this stage: the new fields are computed ALONGSIDE the existing
// ladder from the same decisions version 1 already made, changing no triangle/sphere/error value
// (measured: coarsest total and the five protected meshes unchanged to the triangle). Whether a
// file's ladder carries group topology is the MLET chunk version instead
// (kMlChunkVersionTopology, modules/formats/src/OcMesh.cpp), readable from the chunk header.
//
// NO LOCAL ALIAS FOR fmt::kInvalidClusterId here: ClusterSelect.hpp already declares its own
// `aver::trifactor::kInvalidClusterId` for an unrelated id space (ClusterView::id), and a second
// `inline constexpr` of the same name in the same namespace is a hard MSVC error (C2374/C2086) in
// any TU including both headers (ClusterAdapt.cpp does). The sentinels share a value (0xFFFFFFFFu)
// by coincidence -- every use below spells out `fmt::kInvalidClusterId` to avoid a second collision.

// FORMAT_SPECS.md 5.7 MeshletBounds (32 B): Sphere (16 B) + ConeApex f32[3] (12 B) + ConeAxis i8[3]
// snorm + ConeCutoff i8 snorm. A distinct struct (not the vendor's meshopt_Bounds) so the ON-DISK
// shape is visible at the type level; the next phase's serializer writes these fields byte for byte.
struct ClusterBounds {
    Vec3 sphereCenter{0, 0, 0};
    f32  sphereRadius = 0.0f;
    Vec3 coneApex{0, 0, 0};
    i8   coneAxis[3] = {0, 0, 0};   // snorm8: value/127.0 -> [-1,1]
    i8   coneCutoff  = 127;         // snorm8; +127 (not +128, unrepresentable) is "never cull".
                                     // Must agree with ClusterBuilder.cpp's quantizeConeConservative:
                                     // +1 means "cull from nowhere", NOT -1 -- this default had the
                                     // wrong sign until the commit that added this comment.
};

// One meshlet/cluster. `vertices`/`triangles` are stored EXACTLY as FORMAT_SPECS.md 5.7 wants them
// on disk: `vertices[i]` is a GLOBAL index into the source mesh's vertex buffer (mesh.positions,
// the same buffer at every LOD level -- see ClusterBuilder.cpp for why that is load-bearing), and
// `triangles` holds LOCAL indices (0..vertices.size()-1), three per triangle, into `vertices`.
struct Cluster {
    u32 id    = 0;   // index into LodDag::clusters
    u32 level = 0;   // LOD level; 0 = source resolution

    // Which connected SHELL (position-coincidence-closed component of the source mesh --
    // computeShellIds) this cluster is associated with: at level 0, the shell of its first
    // triangle's first vertex.
    //
    // EXACT for a small-shell cluster, a REPRESENTATIVE for a large one: buildClusters routes small
    // shells into their own cluster whole, but large-shell triangles share a buffer handed to
    // meshopt_buildMeshlets, which can mix more than one large shell into a meshlet. Harmless:
    // routing only asks "is every shell touching this cluster large?", and a large cluster can only
    // ever contain large-shell triangles -- see smallShellLineage below for what routing actually reads.
    //
    // Populated at level 0 ONLY; a level >= 1 cluster's shellId stays at its default (0) and MUST
    // NOT be read as meaningful -- smallShellLineage is propagated instead.
    //
    // BUILD-TIME ONLY: re-derivable from mesh.positions/indices; never serialized.
    u32 shellId = 0;

    // TRUE iff this cluster is, or descends ENTIRELY from, small-shell geometry -- consulted at
    // EVERY level, not just level 0. Separate from shellId (not a `dag.isSmallShell(shellId)`
    // lookup) because shellId is unpopulated past level 0 and would read a stale default.
    //
    // AT LEVEL 0: true for a cluster built directly from one small shell, false for one built by
    // meshopt_buildMeshlets from the large-shell stream. AT LEVEL >= 1 (buildLodHierarchy Pass 2):
    // copied from the producing PendingGroup's `!pg.allLargeShell` (see buildLodHierarchy's own
    // comment on PendingGroup) -- a group's members are always homogeneous in this field (the two
    // buckets partition separately, never concatenate).
    //
    // DEFAULT (false) IS THE SAFE DIRECTION (matching shellId's own "no meaning yet" default and
    // validateClusterErrorBounds' root-sentinel convention): a skipped propagation would default to LockBorder
    // regardless of real lineage (costing back some of the win), never the dangerous direction (a
    // large-shell descendant read as small-shell, losing LockBorder). TrifactorTest's mixed-shell
    // multi-level fixture traces lineage to level 3+ to prove propagation actually happens.
    //
    // BUILD-TIME ONLY, like shellId: never serialized.
    bool smallShellLineage = false;

    std::vector<u32> vertices;    // global vertex indices, size() <= kMaxClusterVertices
    std::vector<u8>  triangles;   // local indices, 3 per triangle, count <= kMaxClusterTriangles*3

    ClusterBounds bounds;

    // Geometric simplification error accumulated from this cluster down to LOD 0 (max of this
    // group's own error and every child's -- see buildLodHierarchy). NOT YET a screen-space pixel
    // value: converting to FORMAT_SPECS' ScreenErrorThreshold needs a reference resolution/FOV
    // projection this offline slice does not own (see toScreenErrorThreshold below). Monotonic
    // (non-decreasing child -> parent) in whatever units meshopt_simplify returns.
    f32 error = 0.0f;

    std::vector<u32> parents;    // cluster ids one level coarser that this cluster feeds into
    std::vector<u32> children;   // cluster ids one level finer that feed into this cluster

    // ---- Stage 4: streaming topology -----------------------------------------------------------
    //
    // fallbackAncestorId: the SINGLE coarser cluster to draw instead of this one when its page is
    // not resident. fmt::kInvalidClusterId at the root level (no coarser level to fall back to).
    //
    // NOT "walk parents[0]": `parents` is many-to-many, not a tree (every group member gets the
    // group's ENTIRE output-cluster set as parents), so parents[0] is just whichever output
    // splitIntoClusters pushed first -- iteration order, not geometry. Picking wrong isn't a crash,
    // it's a silent hole/crack that only shows under streaming memory pressure -- decided ONCE
    // offline in buildLodHierarchy Pass 2, where the full group geometry is in scope.
    //
    // THE CHOICE: the group's own output cluster (newIds) whose bounding-sphere CENTRE is nearest
    // this child's centre -- newIds[0] is arbitrary since a group can re-split into multiple outputs
    // partitioning its space unevenly; nearest-centre keeps every fallback spatially co-located
    // with what it replaces, so no visible gap opens.
    //
    // BUILD-TIME like parents/children, but UNLIKE them this IS serialized -- see
    // OcMeshMeshlet::fallbackAncestorId for the on-disk, level-local form.
    u32 fallbackAncestorId = fmt::kInvalidClusterId;

    // ownerGroupId: the id of the ClusterGroupNode (LodDag::groupNodes) whose ownClusterRange
    // contains THIS cluster -- the group that produced it, not one it feeds into. The back-reference
    // a top-down traversal needs to recurse past this cluster: pop a child id off a coarser group's
    // childClusterRange, look up ITS ownerGroupId, and walk into THAT group's childClusterRange next.
    //
    // fmt::kInvalidClusterId for a LOD-0 cluster, BY CONSTRUCTION: buildLodHierarchy Pass 2 is the
    // only code that creates a ClusterGroupNode, starting at level 0 -> level 1, so none can ever
    // own a level-0 cluster -- "no group produced this" is the true state, not an unset placeholder.
    u32 ownerGroupId = fmt::kInvalidClusterId;

    u32 triangleCount() const { return static_cast<u32>(triangles.size() / 3); }
};

// One ClusterGroupNode: the record buildLodHierarchy Pass 2 produces once per GROUP (one
// PendingGroup -> one ClusterGroupNode), whose ownClusterRange names the group's own output clusters
// (newIds) and childClusterRange names every cluster it replaced (pg.members). What a
// page-residency-aware traversal walks: Cluster::fallbackAncestorId answers "what do I draw instead
// of ONE missing cluster"; this answers "what is the whole next, finer slice to stream in".
//
// Every field is load-bearing for a two-hop walk: ownClusterRange to read a descended-to cluster,
// ownerGroupId to recurse past it -- without both, a walker can descend once but neither draw nor
// recurse a second hop.
struct ClusterGroupNode {
    u32 id    = 0;   // index into LodDag::groupNodes
    u32 level = 0;   // the level of clusters THIS group produced (ownClusterRange); children live at level-1

    // A TRUE SPHERE-OF-SPHERES: contains every CHILD cluster's own bounding sphere in full, computed
    // by iteratively merging pg.members' existing PRE-simplification Cluster::bounds spheres
    // (mergeSphere) -- NOT the group's post-simplification meshopt_computeMeshletBounds result,
    // unlike every other bound in this file.
    //
    // WHY: simplification is a reduction, so a bound from the simplified result can be smaller
    // than the children's true extent (detail sticking out past the smoothed surface), and a
    // traversal culling by it would cull a finer child whose geometry sticks out beyond the
    // silhouette -- invisible head-on, only appearing at a grazing angle. Building the bound from
    // the CHILDREN's own spheres avoids this by construction.
    Vec3 sphereCenter{0, 0, 0};
    f32  sphereRadius = 0.0f;

    // The span of newIds (dag.clusters ids) this group produced. CONTIGUOUS by construction:
    // splitIntoClusters assigns each new id as dag.clusters.size() at push time and pushes a call's
    // whole output back-to-back before anything else can append -- so newIds is always exactly
    // [ownClusterStart, ownClusterStart + ownClusterCount), a plain range unlike childClusterRange.
    u32 ownClusterStart = 0;
    u32 ownClusterCount = 0;

    // The span, into LodDag::groupChildren, of pg.members -- the cluster ids (one level finer) this
    // group replaced. NOT a direct [start,count) into dag.clusters like ownClusterRange: partitioning
    // gives no guarantee a group's members are contiguous in the finer level's id space, so
    // groupChildren holds pg.members verbatim instead (the same [start,count)-into-a-copy shape
    // on-disk MeshletDesc uses for a meshlet's own vertex/triangle lists).
    u32 childClusterStart = 0;
    u32 childClusterCount = 0;
};

// The whole hierarchy for one mesh. `clusters` is flat across all levels; `levels[k]` lists the
// cluster ids belonging to LOD k. The coarsest (root) level is `levels.back()`.
struct LodDag {
    std::vector<Cluster> clusters;
    std::vector<std::vector<u32>> levels;

    // Per-shell classification from computeShellIds, indexed by Cluster::shellId: smallShells[s] is
    // true iff shell s fits inside a single meshlet (<= kMaxClusterVertices/kMaxClusterTriangles).
    // Chosen because it is PROVABLE: such a shell can never be split across a group boundary by
    // meshopt_buildMeshlets, so a group of only small shells contains each WHOLE -- the condition
    // that makes dropping meshopt_SimplifyLockBorder on that group safe.
    //
    // BUILD-TIME ONLY, never written to a file. CONSULTED ONLY AT LEVEL 0 by buildClusters, to route
    // each triangle and set Cluster::smallShellLineage -- buildLodHierarchy's own routing reads
    // smallShellLineage instead, since a level >= 1 group can span several small shells at once.
    std::vector<u8> smallShells;

    // Bounds-checked so a stale/out-of-range shellId (should never happen, but this is where a
    // routing bug would show as an out-of-bounds read) reads as "not small" -- the SAFE direction:
    // keep LockBorder, never drop it.
    bool isSmallShell(u32 shellId) const { return shellId < smallShells.size() && smallShells[shellId] != 0; }

    // Stage 4: flat, global streaming-topology storage -- one ClusterGroupNode appended per group by
    // buildLodHierarchy Pass 2, read by packLodDag when converting into OcMeshLod::groupNodes/
    // groupChildren. Empty for a DAG that never grew past level 0 -- the correct state, not a gap.
    std::vector<ClusterGroupNode> groupNodes;
    // Flat, GLOBAL (dag.clusters-indexed) concatenation of every group's pg.members, in groupNodes'
    // append order -- see ClusterGroupNode::childClusterRange for why this indirection exists.
    std::vector<u32> groupChildren;

    u32  levelCount() const { return static_cast<u32>(levels.size()); }
    bool empty() const { return clusters.empty(); }
};

// Partitions `mesh`'s full-resolution triangle list into LOD-0 clusters (meshopt_buildMeshlets) and
// computes each cluster's bounding sphere + cone (meshopt_computeMeshletBounds), quantized
// CONSERVATIVELY into the spec's snorm8 cone encoding (quantizeConeConservative in the .cpp).
// Populates dag.levels[0] only; does not build LOD > 0.
//
// ALSO CLASSIFIES `mesh` into connected shells (computeShellIds) and ROUTES on the result: every
// small shell (LodDag::isSmallShell) is built into its own cluster directly, bypassing
// meshopt_buildMeshlets (which is otherwise free to pull a topologically-unrelated but
// spatially-close shell into the same meshlet -- see buildDirectCluster); large-shell triangles go
// through the same meshopt_buildMeshlets call this always made, ORDER-PRESERVING w.r.t.
// `mesh.indices`. For a mesh with ZERO small shells that stream is mesh.indices verbatim, so output
// is EXACTLY what it was before this routing existed -- the identical call on the identical buffer,
// which TrifactorTest's zero-small-shell fixture keeps true.
//
// Returns false and sets `why` on a malformed mesh (empty positions/indices, an index count not a
// multiple of 3, or an index out of range for the vertex buffer). Otherwise see validateLodDag for
// the full invariant check.
bool buildClusters(const fmt::OcMeshData& mesh, LodDag& dag, std::string* why = nullptr);

// Extends `dag` (already holding LOD 0 from buildClusters) with coarser LOD levels, iterating
// GROUP -> SIMPLIFY (locked group boundary) -> RE-SPLIT until a level stops reducing triangle count
// or a hard safety cap is hit.
//
// `mesh` must be the SAME mesh buildClusters was called with. Every level is simplified/re-split
// against `mesh.positions` directly, never a per-group copy -- keeping a boundary vertex's position
// bit-identical at every LOD, the mechanism the crack-free invariant rests on (ClusterBuilder.cpp).
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

// ---- Stage 4: validating the streaming topology ------------------------------------------------
//
// SEPARATE from validateLodDag (same reason as validateClusterErrorBounds): checks a hierarchy
// validateLodDag knows nothing about (groupNodes/groupChildren, fallbackAncestorId, ownerGroupId),
// so a topology regression names its own invariant instead of a generic "DAG invalid".
//
// Checks, over every cluster and group node in `dag`:
//  - every NON-ROOT cluster has a VALID fallbackAncestorId: not fmt::kInvalidClusterId, in range,
//    and at EXACTLY c.level + 1 (the level Pass 2 always assigns).
//  - every group node's sphere GENUINELY CONTAINS every child's sphere -- checked NUMERICALLY
//    (distance + child.radius <= group.radius, with tolerance for merge float error, see
//    mergeSphere), never eyeballed. Backstop for this stage's likeliest silent bug
//    (ClusterGroupNode::sphereCenter).
//  - ownerGroupId round-trips: every cluster a group owns has ownerGroupId == g.id, and every
//    level >= 1 cluster has SOME valid ownerGroupId (level 0 must have none).
//  - every group node's own+child ranges are in range for `dag`, and every childClusterRange id is
//    at EXACTLY node.level - 1 (children are always one level finer).
//
// Returns false and sets `why` to the first violation, loudly, before it reaches a file (packLodDag
// calls this before packing, same as validateClusterErrorBounds).
bool validateClusterHierarchy(const LodDag& dag, std::string* why = nullptr);

// ---- geometric error -> FORMAT_SPECS' ScreenErrorThreshold ------------------------------------
//
// Cluster::error stays in meshopt's own relative units; this is the deferred conversion into
// FORMAT_SPECS.md 5.5's ScreenErrorThreshold (distance-independent, screen-space-px).
//
// Runtime formula this assumes (already shipping, LandscapeTree.cpp's `descend()`):
//     screenErrorPx = worldErrorCm * projScale / distanceCm
//     projScale     = viewportHeightPx / (2 * tan(fovY / 2))
// `distanceCm` is per-frame/per-camera, unknown at cook time, so what is stored is the
// distance-independent half: `worldErrorCm * projScale`. Under the SAME reference projScale a
// runtime recovers screen error with one divide; a different viewport/FOV rescales first by
// `(actualProjScale / kReferenceProjScale)`.
//
// REFERENCE CONDITIONS (a threshold means nothing without them): 1080 px viewport height,
// 90-degree vertical FOV:
//     kReferenceProjScale = 1080 / (2 * tan(45 deg)) = 1080 / 2 = 540.0f
// chosen to EQUAL LandscapeTree.hpp's `SelectParams::projScale` default -- the one LOD metric
// already shipping for this problem shape, avoiding a second, inconsistent reference.
inline constexpr f32 kReferenceViewportHeightPx = 1080.0f;
inline constexpr f32 kReferenceFovYRadians       = kPi / 2.0f;   // 90 degrees
inline constexpr f32 kReferenceProjScale         = 540.0f;       // see the derivation above

// meshopt_simplify's `result_error` (Cluster::error) is RELATIVE to the mesh's own bounding-box
// max-axis extent, not an absolute distance (meshoptimizer.h's meshopt_simplifyScale;
// buildLodHierarchy never sets meshopt_SimplifyErrorAbsolute). Every call is against the SAME
// `mesh.positions`/vertexCount (never a per-group subset), so this scale is ONE constant for a
// whole mesh's DAG -- safe to compute once and reuse for every cluster's error.
f32 worldExtentScale(const fmt::OcMeshData& mesh);

// Converts one Cluster::error into FORMAT_SPECS' ScreenErrorThreshold units, given `scale` from
// worldExtentScale(mesh) (the SAME mesh):
//     absoluteErrorCm      = clusterError * scale
//     screenErrorThreshold = absoluteErrorCm * kReferenceProjScale
// A multiply by two positive constants, so strictly monotonic in `clusterError` -- the DAG's
// `parent.error >= child.error` invariant survives automatically. validateScreenErrorMonotonic
// still re-checks this on the CONVERTED values, asserted after conversion, not inferred from raw.
f32 toScreenErrorThreshold(f32 clusterError, f32 scale);

// Re-checks error-monotonicity (parent's converted screen error >= every child's) AFTER projecting
// through toScreenErrorThreshold -- mirrors validateLodDag's check but on the value a runtime
// actually compares against a pixel budget, not the raw error. A future conversion change (a
// non-linear projection, per-cluster scale) should not be trusted to preserve monotonicity by proxy.
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
// this inherits that guarantee rather than asserting a new one.
//
// ROOT-ness is `c.level == dag.levelCount() - 1` (same definition as validateLodDag), not
// `c.parents.empty()`, so the split cannot silently agree with a dag-connectivity bug (a non-root
// with no parents) instead of exposing it. A root gets parentError = +FLT_MAX (finite, not IEEE
// +inf -- see OcMeshMeshlet) so the local cut test always accepts it once nothing finer qualified;
// a finite value here would let a distant root silently stop drawing.
//
// For a non-root with MORE THAN ONE parent (splitIntoClusters can produce several per group, e.g.
// when meshopt_buildMeshlets' 64-vertex/124-triangle limits force a re-split), parentError is the
// MAX over every parent's converted error, not parents[0]. All parents from the same group carry the
// same propagatedError today, so the two agree numerically; max() is used anyway so it stays correct
// if a future per-newId computation breaks that equality -- parents[0] could under-report, the
// "holes in the mesh" failure mode the local cut test cannot detect on its own; validateClusterErrorBounds
// is the backstop that catches it.
//
// If `dag` is empty this returns an empty vector.
std::vector<ClusterErrorBounds> computeClusterErrorBounds(const LodDag& dag, f32 scale);

// Validates the local cut test's correctness argument, over EVERY cluster in `dag`:
//   - ownError <= parentError (what the "covers every surface exactly once" claim rests on)
//   - a cluster is a root (c.level == dag.levelCount() - 1) IFF parentError is exactly +FLT_MAX
//     (checks the SENTINEL made it through, not merely that some large value did, and flags a
//     non-root wrongly given the root sentinel)
// `bounds` must come from computeClusterErrorBounds(dag, scale) for this same `dag`, indexed the
// same way. Returns false and sets `why` to the first violation, loudly: ownError > parentError is
// a hole at some pixel budget, a wrongly finite root vanishes at distance, a wrongly infinite
// non-root double-draws.
bool validateClusterErrorBounds(const LodDag& dag, const std::vector<ClusterErrorBounds>& bounds,
                                 std::string* why = nullptr);

// ---- packing a built LodDag back into the on-disk OcMeshData shape (Stage 2, Part A) --------
//
// Lifted from ConvertTool's private toMeshlets/toIndices/addMeshlets; lives in Aver.Trifactor, not
// Aver.Formats, because Aver.Formats sits BELOW it in the module DAG (cmake/AvModule.cmake,
// aver_check_module_dag) and must stay loadable with AVER_MODULE_TRIFACTOR=OFF (so it cannot name
// LodDag/Cluster) -- Aver.Trifactor already depends on Aver.Formats and is the one module allowed
// to see both types. Keeping the conversion here is what lets ConvertTool, RelodTool's write path,
// and TrifactorTest's MLET round-trip fixtures call the SAME code instead of drifting copies.
//
// `dag` must already hold LOD 0 (buildClusters) and any coarser levels (buildLodHierarchy) -- this
// calls neither. `mesh` must be the SAME mesh (or an exact positions/indices copy) `dag` was built
// from: reads mesh.positions ONLY for worldExtentScale(mesh), and never touches mesh.positions/
// indices/submeshes/materialSlots/joints/weights -- so RelodTool can hand it a full mesh copy and
// get every other stream back untouched.
//
// Populates mesh.meshlets (LOD 0) and mesh.coarserLods (LOD 1+, each with its own index buffer and
// ScreenErrorThreshold), validating per-cluster ownError/parentError (computeClusterErrorBounds +
// validateClusterErrorBounds) and re-checking screen-error monotonicity (validateScreenErrorMonotonic)
// BEFORE anything is packed -- matching addMeshlets' original ordering, so a broken hierarchy cannot
// reach the output.
//
// STAGE 4: also populates OcMeshLod::groupNodes/groupChildren and, per meshlet,
// OcMeshMeshlet::fallbackAncestorId/ownerGroupId (translated from GLOBAL cluster ids into the
// on-disk LEVEL-LOCAL indices MLET chunk-version 3 stores). validateClusterHierarchy runs BEFORE
// packing, same position as validateClusterErrorBounds -- a broken hierarchy (an invalid
// fallbackAncestorId, a group sphere not genuinely containing its children) must not reach
// mesh.coarserLods any more than a broken error bound may.
//
// On success, stamps mesh.builderVersion = kBuilderVersion (Part B) -- the one call site in the
// engine that cooks a ladder into a mesh's on-disk streams. On failure, mesh.meshlets/coarserLods/
// builderVersion are left EXACTLY as on entry -- a caller that only conditionally wants clustering
// (ConvertTool saving "without meshlets" on a pathological input) does not need to roll anything back.
//
// Returns false and sets `why` when `dag` is empty, error-bound validation fails, or (Stage 4)
// validateClusterHierarchy fails -- refusals made once here rather than by every caller.
bool packLodDag(const LodDag& dag, fmt::OcMeshData& mesh, std::string* why = nullptr);

// Reduces `mesh` in place to roughly `ratio` of its triangles (0 < ratio < 1), rewriting positions,
// normals, UVs, skin, indices, bounds, and the submesh table. Returns false and leaves the mesh
// UNTOUCHED if the input is unusable or the simplifier could not reach near the target.
//
// NOT virtualized geometry -- the blunt instrument that makes photogrammetry usable before it lands.
// Measured on this tree: frame time is linear in drawn triangles at ~2.1 ms/million, so a
// 6.95M-triangle scan placed fourteen times costs ~200 ms/frame on its own; frustum culling cannot
// help since the triangles are genuinely on screen, just sub-pixel (docs/VIRTUALIZED_GEOMETRY.md
// §3.5: the rasterizer shades in 2x2 quads, so a sub-pixel triangle wastes three quarters of its
// work). The real fix is per-cluster GPU LOD selection (slices 1-5, using buildLodHierarchy's
// hierarchy/error metric above); this is the stopgap needing none of it: ONE decimation at cook time.
//
// PER SUBMESH, NOT WHOLE-MESH: each OcMeshSubmesh range (one per material) simplifies on its own and
// the table is rewritten to match, so no triangle changes material; a mesh with no table, or with
// one submesh (drawn whole whatever its range says), is one range. A larger table must pass
// fmt::submeshesPartitionIndices or this refuses -- a single unchanged-table pass once let triangles
// shuffle between materials (NewSponza's curtains drew entirely under metal_door after --lod).
//
// Borders BETWEEN submeshes are locked (shared-position vertices), or the two sides would simplify
// apart and crack. Every other open border simplifies freely: buildLodHierarchy locks group borders
// because neighbouring clusters must still meet, but a mesh's outer border has nothing outside it to
// meet, so locking it would only stop the silhouette from simplifying -- an assumption that stops
// holding if this is ever used on geometry that tiles against another mesh.
bool simplifyMesh(fmt::OcMeshData& mesh, f32 ratio, std::string* why = nullptr);

} // namespace aver::trifactor
