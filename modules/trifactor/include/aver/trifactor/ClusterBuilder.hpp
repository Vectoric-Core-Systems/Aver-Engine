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
//
// ALIASES, NOT A SECOND PAIR OF LITERALS, which is the same thing this header already does for
// fmt::kInvalidClusterId and for the same stated reason: this module depends on Aver.Formats, so
// the on-disk contract can own the number and be named from here. They used to be independent 64
// and 124 in two files (three, counting a #define in a shared HLSL prelude), agreeing only because
// nobody had changed one.
inline constexpr u32 kMaxClusterVertices  = fmt::kMaxMeshletVertices;
inline constexpr u32 kMaxClusterTriangles = fmt::kMaxMeshletTriangles;

// ---- builder version stamp (task: "Stage 2, Part B") -------------------------------------------
//
// A single number naming WHICH REVISION of this module's cook algorithm produced a given mesh's
// meshlets/coarserLods -- persisted on disk in .ocmesh's MHDR.Reserved field (FORMAT_SPECS.md 5.1,
// repurposed as BuilderVersion; see OcMeshData::builderVersion's own comment in
// aver/formats/OcMesh.hpp for the write/read side, and packLodDag below for the one place that
// stamps it). The problem this exists to solve: commit 14ba2b7 changed buildClusters/
// buildLodHierarchy's own output (the shell-routing fix) without changing one byte of any .ocmesh
// already on disk -- every cooked file still carries the LADDER IT WAS COOKED WITH, silently. There
// was no way to ask a file "is your ladder current?" short of re-running buildClusters/
// buildLodHierarchy on it and diffing the result, which is exactly the expensive round trip a
// version stamp exists to make unnecessary -- and, per this task's own PART B, "later it is the DDC
// key": a future derived-data cache needs a cheap comparable fact to decide "recompute" vs "reuse"
// without ever touching the simplifier.
//
// BUMP THIS whenever a change to buildClusters or buildLodHierarchy would produce DIFFERENT
// meshlets/coarserLods for at least one mesh that used to build cleanly -- a new routing decision
// (like 14ba2b7's), a changed simplifier flag or target, a changed grouping strategy, a changed
// quantization. Do NOT bump it for a change that cannot affect cook output: a comment, a log line, a
// refactor proven output-identical (like this task's own PART A lifting toMeshlets/toIndices into
// packLodDag below -- same bytes in, same bytes out, so the version they were cooked at is still the
// same version), or a change confined to validateLodDag/validateClusterErrorBounds (those check the
// DAG, they do not build it).
//
// STARTS AT 1, DELIBERATELY NOT 0. Every .ocmesh written before this field existed has Reserved == 0
// (FORMAT_SPECS.md 2.3: "Reserved fields are zero", upheld by every prior writer), so 0 is already,
// unavoidably, "some file from before version stamps existed" -- and this task's own README-in-the-
// task-block says it plainly: "an OLD file, whose Reserved is 0, reads as unknown/stale and never as
// current". Starting the real version numbering at 0 would make that impossible to tell apart from a
// current build that happened to be at version 0; starting at 1 means 0 can ONLY ever mean "no
// version-aware builder touched this file's ladder", by construction, not by a convention a future
// bump could accidentally violate.
//
// version 1 (this one) IS commit 14ba2b7's shell-routing-aware buildClusters/buildLodHierarchy --
// the algorithm whose own header comment quotes the corpus numbers (corpus coarsest 1,195,431 ->
// 140,489; the five protected meshes unmoved) that a file stamped with this version was cooked
// under. It is also, not incidentally, the FIRST version this field is able to record at all: nothing
// before this task ever wrote anything but 0 here, so there is no "version 0 algorithm" to distinguish
// this one from on disk -- 0 already carries that meaning by the paragraph above.
inline constexpr u32 kBuilderVersion = 1;

// ---- Stage 4: streaming topology (fallbackAncestorId, ownerGroupId, ClusterGroupNode) ----------
//
// kBuilderVersion is NOT bumped for this stage, and that is a deliberate reading of the rule two
// paragraphs up, not an oversight of it. That rule bumps on a change that would produce DIFFERENT
// meshlets/coarserLods -- different triangles, different bounds, a different ladder -- for some
// mesh. Stage 4 adds fields (below) that buildClusters/buildLodHierarchy compute ALONGSIDE the
// existing ladder, from the exact same grouping/simplification decisions version 1 already made; it
// does not change one triangle, one bounding sphere, or one error value any file cooked at version 1
// already carries (the corpus-wide proof of that is this stage's own measured numbers: the coarsest
// total and the five protected meshes are unchanged to the triangle). A file's MLET chunk version
// (kMlChunkVersionTopology in modules/formats/src/OcMesh.cpp) is the correct, orthogonal signal for
// "does this file's ladder carry group topology" -- it is readable straight from the chunk header
// without inferring it from builderVersion, which is what "orthogonal" is buying here: builderVersion
// keeps meaning exactly what its own comment above says, and does not grow a second meaning by proxy.
//
// NO LOCAL ALIAS FOR fmt::kInvalidClusterId IS DECLARED HERE, on purpose: ClusterSelect.hpp (this
// same module, a concurrent workflow's file this task does not touch -- see this file's own header
// on why) already declares its OWN `aver::trifactor::kInvalidClusterId` for an unrelated in-memory id
// space (ClusterView::id), and a second `inline constexpr` of the identical name in the identical
// namespace is a hard MSVC error (C2374/C2086) in any translation unit that ends up including both
// headers (ClusterAdapt.cpp does, today). The two sentinels share a numeric value (0xFFFFFFFFu) and a
// purpose ("no such id") by coincidence of both being the natural u32 "invalid" sentinel, not because
// either file depends on the other -- so every use of that sentinel below is spelled out fully as
// `fmt::kInvalidClusterId` (OcMesh.hpp, aver::fmt -- a namespace ClusterSelect.hpp does not touch at
// all) rather than risk a second collision the next time this module grows another consumer.

// FORMAT_SPECS.md 5.7 MeshletBounds (32 B): Sphere (16 B) + ConeApex f32[3] (12 B) + ConeAxis i8[3]
// snorm + ConeCutoff i8 snorm. A distinct struct (not the vendor's meshopt_Bounds) so the ON-DISK
// shape is visible at the type level; the next phase's serializer writes these fields byte for byte.
struct ClusterBounds {
    Vec3 sphereCenter{0, 0, 0};
    f32  sphereRadius = 0.0f;
    Vec3 coneApex{0, 0, 0};
    i8   coneAxis[3] = {0, 0, 0};   // snorm8: value/127.0 -> [-1,1]
    i8   coneCutoff  = 127;         // snorm8; +127 (not +128, which snorm8 cannot represent anyway)
                                     // is the conservative "never cull" value -- see
                                     // ClusterBuilder.cpp's quantizeConeConservative for the runtime
                                     // cull formula this must agree with (cutoff -> +1 means "cull
                                     // from nowhere", NOT -1; this default was the wrong sign until
                                     // the commit that added this comment)
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

    // ---- Stage 4: streaming topology -----------------------------------------------------------
    //
    // fallbackAncestorId: the SINGLE coarser cluster a streaming system should draw instead of this
    // one when this cluster's own page is not resident. fmt::kInvalidClusterId for a cluster in
    // dag.levels.back() (the root level -- there is no coarser level to fall back to).
    //
    // WHY THIS IS NOT "walk parents[0]", or any runtime walk of `parents` at all -- the question this
    // stage's own task opens with. `parents` is a MANY-TO-MANY edge set, not a tree: buildLodHierarchy
    // Pass 2 gives every one of a group's own output clusters (newIds) the group's ENTIRE `pg.members`
    // list as children, so every one of pg.members ends up with as many parents as the group produced
    // output clusters -- there is no "the" parent to pick, and parents[0] is simply whichever one
    // splitIntoClusters happened to push first, a fact about iteration order, not about geometry. A
    // runtime substituting the wrong one is not a crash: it draws SOME coarser cluster, so nothing
    // looks broken until the substituted geometry does not cover the same space as the one that went
    // missing -- a hole or a crack, and only when streaming is actually under memory pressure, which
    // is exactly the condition under which nobody is watching for a rendering bug. That failure mode
    // is why this is decided ONCE, offline, in buildLodHierarchy Pass 2, where both pg.members and
    // newIds are in scope together with the FULL group geometry, rather than deferred to a runtime
    // that only ever sees one cluster id at a time and has no principled way to break the tie.
    //
    // THE CHOICE: for each child in pg.members, the member of newIds (this group's own output
    // clusters) whose bounding-sphere CENTRE is nearest that child's own centre. This is what
    // "fallback" needs to mean geometrically: the coarser cluster that best approximates the SAME
    // region of space the missing child covered, not merely a member of the same group. Nearest-
    // centre is preferred here over "first parent" (i.e. newIds[0], unconditionally) for exactly the
    // property first-parent cannot offer: a group re-splits into MULTIPLE output clusters whenever
    // the simplified geometry still exceeds one meshlet's 64-vertex/124-triangle limit
    // (splitIntoClusters, called on pg.simplifiedIndices), and those outputs partition the group's
    // SPACE, not its triangle budget evenly -- newIds[0] is an arbitrary one of them with no reason to
    // sit anywhere near this particular child. Nearest-centre guarantees every child's fallback is
    // spatially co-located with what it is substituting for, which is the one property a streaming
    // system actually needs from a fallback: drawing it in place of a resident page should not leave
    // a visible gap where the missing detail was.
    //
    // BUILD-TIME ONLY like parents/children above, but UNLIKE them, this field IS serialized -- see
    // OcMeshMeshlet::fallbackAncestorId (aver/formats/OcMesh.hpp) for the on-disk, level-local-index
    // form packLodDag converts this global cluster id into.
    u32 fallbackAncestorId = fmt::kInvalidClusterId;

    // ownerGroupId: the id of the ClusterGroupNode (LodDag::groupNodes, below) whose ownClusterRange
    // contains THIS cluster -- i.e. the group that produced this cluster as one of its own outputs,
    // not a group this cluster feeds INTO. The back-reference a top-down traversal needs to recurse
    // past this cluster once it is reached: pop a cluster id off a coarser group's childClusterRange,
    // look up ITS ownerGroupId, and that group's own childClusterRange is the next, finer set to walk
    // into. Without this field a walker that only has a coarser group's child-descend range has
    // nowhere to go from a child cluster id alone -- it can name the child, but not find what
    // produced it, and so cannot recurse a second hop.
    //
    // fmt::kInvalidClusterId for a LOD-0 cluster, BY CONSTRUCTION: buildLodHierarchy's Pass 2 is the
    // ONLY code that ever creates a ClusterGroupNode, and it starts at level 0 -> level 1, so no
    // ClusterGroupNode's ownClusterRange can ever contain a level-0 cluster (buildClusters, which
    // builds level 0, has no notion of "group" at all). This is the same "no meaning yet" default
    // shellId and smallShellLineage's build-time fields already use, applied to a field where "no
    // group produced this, because none could have" is the true state, not a placeholder for one that
    // has not run yet.
    u32 ownerGroupId = fmt::kInvalidClusterId;

    u32 triangleCount() const { return static_cast<u32>(triangles.size() / 3); }
};

// One ClusterGroupNode: the record buildLodHierarchy Pass 2 produces once per GROUP (not once per
// cluster) -- one PendingGroup in that loop yields exactly one ClusterGroupNode, whose ownClusterRange
// names every one of that group's own output clusters (newIds) and whose childClusterRange names
// every cluster the group replaced (pg.members). This is the record a page-residency-aware traversal
// actually walks: Cluster::fallbackAncestorId (above) answers "what do I draw instead of ONE missing
// cluster"; ClusterGroupNode answers "what is the whole next, finer slice of the mesh I should stream
// in", which a per-cluster field alone cannot -- see childClusterRange's own comment for why a
// traversal needs the WHOLE group's children in one place, not one fallback id at a time.
//
// AN ADVERSARIAL DESIGN REVIEW OF THIS STAGE'S FIRST DRAFT CAUGHT THIS TYPE MISSING HALF ITS FIELDS --
// a draft that recorded only the sphere bound and childClusterRange, with no way to find the group's
// OWN output clusters (ownClusterRange) or to find a group AT ALL starting from a cluster id
// (Cluster::ownerGroupId). Both omissions have the same shape: a walker that can descend one hop
// (follow childClusterRange down to a finer cluster) has nowhere to write a draw call for what it just
// reached (no ownClusterRange to read bounds/geometry from) and cannot recurse a SECOND hop (no
// ownerGroupId to find that finer cluster's own group). Every field below exists because removing it
// reproduces one of those two failures.
struct ClusterGroupNode {
    u32 id    = 0;   // index into LodDag::groupNodes
    u32 level = 0;   // the level of clusters THIS group produced (ownClusterRange); children live at level-1

    // A TRUE SPHERE-OF-SPHERES: a sphere containing every CHILD cluster's OWN bounding sphere in
    // full, computed over pg.members' existing Cluster::bounds (the PRE-simplification geometry this
    // group is about to replace), by iteratively merging those spheres (mergeSphere, ClusterBuilder.cpp)
    // rather than by taking the group's post-simplification meshopt_computeMeshletBounds result.
    //
    // THE SINGLE MOST LIKELY THING IN THIS STAGE TO BE SILENTLY WRONG, per the task that specified it,
    // and worth spelling out exactly why meshopt_computeMeshletBounds on the SIMPLIFIED geometry is
    // not a substitute even though every OTHER bound in this file (Cluster::bounds itself) comes from
    // exactly that call: simplification is a reduction. The coarser cluster(s) this group produces are
    // a SMALLER, smoothed-out approximation of the region pg.members covered -- meshopt_simplify's
    // whole job is to remove detail, including detail that stuck OUT past where the smoothed surface
    // ends up. A bound computed from that reduced result is therefore a bound of the SIMPLIFIED
    // shape, not of everything that fed into it, and can be smaller than the true extent of the
    // children it is meant to summarize. A traversal that culls a group using that bound would then
    // cull away a finer child whose own geometry sticks out beyond the simplified silhouette -- and
    // because the error is "the bound is too small", not "too large", the failure is invisible in the
    // common case (looking straight at the object, everything present) and only appears at a grazing
    // angle where the missing child's contribution would have been visible at the object's silhouette.
    // That is precisely the "everything still looks fine until geometry pops out" failure mode the
    // task named -- caught by construction here by building the bound from the CHILDREN's own
    // spheres, never from the group's own (already coarser) output.
    Vec3 sphereCenter{0, 0, 0};
    f32  sphereRadius = 0.0f;

    // The span of newIds (dag.clusters ids) this group produced. CONTIGUOUS by construction, not
    // merely in practice: splitIntoClusters assigns a new cluster's id as dag.clusters.size() at the
    // moment it is pushed, and every one of a single splitIntoClusters call's outputs is pushed back
    // to back before any OTHER code can append to dag.clusters -- so newIds is always exactly
    // [ownClusterStart, ownClusterStart + ownClusterCount). This is what lets ownClusterRange be a
    // plain [start,count) rather than needing the same indirection childClusterRange (below) does.
    u32 ownClusterStart = 0;
    u32 ownClusterCount = 0;

    // The span, into LodDag::groupChildren (below), of pg.members -- the cluster ids (one level
    // finer) this group replaced. NOT a direct [start,count) into dag.clusters, unlike
    // ownClusterRange: pg.members is whatever meshopt_partitionClusters (via groupClusters) assigned
    // to this group, and partitioning gives no guarantee that a group's members are a contiguous
    // slice of the finer level's own cluster-id space -- two clusters from opposite ends of that level
    // can land in the same group. LodDag::groupChildren exists specifically to hold pg.members
    // verbatim, one group's worth at a time, so this range can still be a cheap [start,count) into
    // SOMETHING, the same shape the on-disk MeshletDesc's VertexIndexOffset/TriangleOffset already use
    // to solve the identical problem for a meshlet's own vertex/triangle lists.
    u32 childClusterStart = 0;
    u32 childClusterCount = 0;
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

    // Stage 4: flat, global storage for the streaming topology -- populated by buildLodHierarchy's
    // Pass 2 (one ClusterGroupNode appended per group, at every level, in the same order groups are
    // processed), read by packLodDag when converting this DAG into OcMeshLod::groupNodes/
    // groupChildren (see packLodDag's own doc comment). Empty for a DAG that never grew past level 0
    // (buildLodHierarchy either was not called, or found nothing to reduce) -- there being no group
    // to record is the correct state there, not a gap.
    std::vector<ClusterGroupNode> groupNodes;
    // Flat, GLOBAL (dag.clusters-indexed) concatenation of every group's own pg.members, in the same
    // order groupNodes is appended, one group's worth at a time -- see ClusterGroupNode::
    // childClusterRange's own comment for why this indirection exists instead of a direct range.
    std::vector<u32> groupChildren;

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

// ---- Stage 4: validating the streaming topology ------------------------------------------------
//
// A SEPARATE function from validateLodDag above, for the same reason validateClusterErrorBounds is
// separate from it (see that function's own comment): this checks a hierarchy validateLodDag knows
// nothing about (groupNodes/groupChildren, fallbackAncestorId, ownerGroupId did not exist when that
// function was written), and folding it in would make a topology regression read as a generic "DAG
// invalid" instead of naming the specific new invariant that broke.
//
// Checks, over every cluster and every group node in `dag`:
//  - every NON-ROOT cluster (c.level != dag.levelCount() - 1) has a VALID fallbackAncestorId: not
//    fmt::kInvalidClusterId, in range for dag.clusters, and at EXACTLY c.level + 1 -- the level
//    buildLodHierarchy Pass 2 always assigns it at, so anything else means the id was computed
//    against the wrong group's newIds.
//  - every group node's sphere GENUINELY CONTAINS every child cluster's own sphere -- checked
//    NUMERICALLY (distance(group.center, child.center) + child.radius <= group.radius, within a
//    small tolerance for the floating-point error a handful of sequential sphere merges can
//    introduce -- see mergeSphere's own comment in the .cpp), never eyeballed. This is the backstop
//    for the single most likely thing in this stage to be silently wrong (see ClusterGroupNode::
//    sphereCenter's own comment) -- a bound that LOOKS plausible but does not actually contain what
//    it claims to is exactly the failure mode this check exists to catch before it reaches a file.
//  - ownerGroupId round-trips: for every group `g`, every cluster in [g.ownClusterStart,
//    g.ownClusterStart + g.ownClusterCount) has ownerGroupId == g.id -- and every cluster with
//    level >= 1 has SOME valid ownerGroupId (level 0 must have none -- see Cluster::ownerGroupId's
//    own comment for why that direction is the one that must hold).
//  - every group node's own+child ranges are in range for `dag` (ownClusterRange inside
//    dag.clusters, childClusterRange inside dag.groupChildren, and every id it names inside
//    dag.clusters at EXACTLY node.level - 1 -- children are always one level finer, by construction).
//
// Returns false and sets `why` to the first violation found, in the same "loudly, before it reaches
// a file" spirit as validateClusterErrorBounds -- packLodDag calls this before packing, exactly like
// that function.
bool validateClusterHierarchy(const LodDag& dag, std::string* why = nullptr);

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

// ---- packing a built LodDag back into the on-disk OcMeshData shape (task: "Stage 2, Part A") --------
//
// LIFTED FROM tests/formats/src/ConvertTool.cpp's anonymous namespace, where this conversion used to
// live as ConvertTool's own private toMeshlets/toIndices/addMeshlets -- see this function's own git
// history for the ORIGINAL comment explaining why it was left there rather than duplicated into
// RelodTool "in a hurry". It belongs in Aver.Trifactor, not Aver.Formats, for the same reason
// OcMeshMeshlet's own doc comment gives for why Aver.Formats cannot define a Cluster-shaped type
// itself: Aver.Formats sits BELOW Aver.Trifactor in the module DAG (cmake/AvModule.cmake,
// aver_check_module_dag) and must stay loadable with AVER_MODULE_TRIFACTOR=OFF, so it cannot name
// LodDag/Cluster -- whereas Aver.Trifactor already depends on Aver.Formats (this very header includes
// aver/formats/OcMesh.hpp) and is the one module allowed to see both types. Putting the conversion
// here, rather than leaving it to whichever caller needs it first, is what keeps ConvertTool and
// RelodTool's write path (and TrifactorTest's own MLET round-trip fixtures) calling the SAME code
// instead of three copies that drift the moment one of them fixes a bug the other two do not know
// about -- which is the exact failure this lift exists to prevent.
//
// `dag` must already hold LOD 0 (buildClusters) and, for more than a single-level result, the coarser
// levels too (buildLodHierarchy) -- this function does not call either; a caller like RelodTool that
// already built `dag` for its own reporting is not asked to build it twice just to persist it.
// `mesh` must be the SAME mesh (or an exact positions/indices-identical copy of it) `dag` was built
// from: this function reads mesh.positions ONLY to compute worldExtentScale(mesh) for the error
// conversion, and never touches mesh.positions/indices/submeshes/materialSlots/joints/weights --
// which is what lets RelodTool's write path (PART C) hand it a full copy of an original mesh and get
// every OTHER stream back untouched, with only meshlets/coarserLods/builderVersion replaced.
//
// Populates mesh.meshlets (LOD 0) and mesh.coarserLods (LOD 1+, each with its own index buffer and
// ScreenErrorThreshold), computing and validating per-cluster ownError/parentError internally
// (computeClusterErrorBounds + validateClusterErrorBounds) and re-checking screen-error monotonicity
// (validateScreenErrorMonotonic) BEFORE anything is packed -- exactly the ordering addMeshlets always
// used, so a broken hierarchy still cannot reach mesh.meshlets/coarserLods through this path either.
//
// STAGE 4: also populates, on every LOD 1+ entry, OcMeshLod::groupNodes/groupChildren (from
// dag.groupNodes/dag.groupChildren, translated from this DAG's GLOBAL cluster ids into the on-disk
// LEVEL-LOCAL indices MLET chunk-version 3 stores -- see the .cpp for that translation) and, on every
// meshlet at every level, OcMeshMeshlet::fallbackAncestorId/ownerGroupId (same translation, from
// Cluster::fallbackAncestorId/ownerGroupId). validateClusterHierarchy runs BEFORE any of this is
// packed, in the same "loudly, before it reaches a file" position validateClusterErrorBounds already
// occupies -- a broken hierarchy (an invalid fallbackAncestorId, a group sphere that does not
// genuinely contain its children) must not reach mesh.coarserLods any more than a broken error bound
// may.
//
// On success, also stamps mesh.builderVersion = kBuilderVersion (PART B): this is the ONE call site
// in the engine that actually cooks a ladder into a mesh's on-disk streams, so it is the one place
// that gets to say which builder cooked it. On failure, mesh.meshlets/coarserLods/builderVersion are
// left EXACTLY as they were on entry -- a caller that only conditionally wants clustering (ConvertTool
// saving "without meshlets" on a pathological input) does not need to remember to roll anything back.
//
// Returns false and sets `why` when `dag` is empty (nothing to pack), when the converted error bounds
// fail validation, or (Stage 4) when validateClusterHierarchy fails; all three are refusals made once
// here instead of independently by every caller that packs a DAG.
bool packLodDag(const LodDag& dag, fmt::OcMeshData& mesh, std::string* why = nullptr);

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
