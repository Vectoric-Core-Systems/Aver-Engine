#pragma once
// ClusterSelect -- the virtualized-geometry SELECTION maths: given a camera and a bag of clusters,
// decide which clusters to draw and at which level. Pure functions, CPU only, no GPU/RHI/ImGui.
//
// DELIBERATELY DECOUPLED, twice over, and this is the single most important thing about this file.
// It does not include ClusterBuilder.hpp (this module's own cluster/DAG builder) and it does not
// include anything under modules/formats. Both are mid-restructure by concurrent workflows, so this
// file defines its OWN minimal input types below -- ClusterView, not trifactor::Cluster; View, not
// anything from fmt:: or modules/render. An adapter that fills a ClusterView from a real LodDag (or
// from the on-disk per-LOD MLET layout that storage is moving to) is later, separate work. What such
// an adapter must produce is documented at each ClusterView field below; the short version is: decode
// the cone from its on-disk snorm8 form before filling coneAxis/coneCutoff (this file only sees plain
// floats), and index `parents`/`children` as positions into the SAME array the ClusterView came from
// (i.e. `clusters[id].id == id`, exactly like LodDag::clusters today).
//
// WHERE THIS AGREES WITH THE REST OF THE ENGINE (verified read-only against the real source; see the
// scouting report this file's commit/PR is attached to for the exact citations):
//   - Coordinate conventions: +Z up, centimetres, LEFT-HANDED, row-major aver::Mat4 multiplied as
//     v * M (row vectors) -- modules/core/include/aver/core/Math.hpp's own header comment, and
//     SandboxApp.cpp's `view * proj` order.
//   - The screen-space error formula (`screenSpaceErrorPx` below) is EXACTLY
//     modules/landscape/src/LandscapeTree.cpp's `descend()`: errorCm * projScale / distanceToSurface,
//     projScale = viewportHeightPx / (2*tan(fovY/2)). Landscape already turned "geometric error in
//     cm" into "pixels" once for this engine; this file reuses that formula rather than inventing a
//     second one, per this slice's brief.
//   - The six-plane frustum test (`Frustum` below) is EXACTLY
//     modules/landscape/include/aver/landscape/LandscapeTree.hpp's `Frustum` /
//     modules/landscape/src/LandscapeTree.cpp's `fromViewProj`/`intersectsSphere`, which is in turn
//     the same plane-extraction arithmetic (same row/column indexing, same "sum or difference of two
//     columns", same inward-positive sign) SandboxApp.cpp's scene-entity draw loop already computes
//     per frame for AABB culling. Three independent call sites converging on the same formula is why
//     it is reproduced here rather than imported: this file cannot depend on modules/landscape (a
//     sibling module, owned by other work) or on sandbox/src (never a library), so the only way to
//     "use the existing convention instead of inventing a second one" is to copy the arithmetic, not
//     the header. See the .cpp for the derivation in full.
//   - The backface (normal cone) test is the exact apex-based formula
//     third_party/meshoptimizer/src/meshoptimizer.h documents at meshopt_computeClusterBounds's
//     comment (`dot(normalize(cone_apex - camera_position), cone_axis) >= cone_cutoff`), and this file
//     does not just cite that comment -- tests/trifactor/src/ClusterSelectTest.cpp calls
//     meshopt_computeClusterBounds on a real hand-built mesh and checks coneCull's answer against the
//     physically obvious one (a camera on the front side is not culled, one on the back side is), so
//     the sign convention is measured, not just read.
//
//     THAT MEASUREMENT FOUND A REAL DISAGREEMENT, and it is worth being loud about it rather than
//     quietly working around it. ClusterBuilder.cpp's quantizeConeConservative USED TO store -127
//     (decodes to -1.0f) as its "this cone can never safely cull anything" sentinel, on the reasoning
//     that -127 was the most CONSERVATIVE end of the encoding. Under the formula above, that reasoning
//     was backwards: the cull region `{d : dot(d,axis) >= cutoff}` GROWS as cutoff falls toward -1 and
//     SHRINKS to a single unreachable direction as cutoff rises toward +1, so cutoff = -1 is actually
//     "cull from every direction" and +1 is "never cull". ClusterView's own coneCutoff default is
//     +1.0f for exactly this reason (see its comment). This was the root cause of a real, shipped
//     defect (every pine_tree_01 cluster whose axis quantized near-degenerate, or whose true cone
//     plus quantization error exceeded a hemisphere, went permanently backface-culled -- the mesh
//     rendered with zero foliage in the GPU cluster path).
//
//     FIXED IN TWO PLACES, DELIBERATELY, not one. ClusterBuilder.cpp's quantizeConeConservative was
//     corrected (its whole widen-then-floor-toward-(-1) scheme was the same backwards sign, not just
//     the three sentinel constants -- see that function's own header comment for the corrected
//     derivation) so FUTURE cooks consistently push cutoff toward +1, never -1. That alone does
//     nothing for an .ocmesh already sitting on disk with the old encoding baked in -- and re-cooking
//     every already-cooked asset (irreplaceable source art, a slow and irreversible-feeling operation)
//     just to pick up a sign fix is not a cost this bug should force on anyone. So coneCull below ALSO
//     honours cutoff <= -1.0f as "never cull", symmetrically with the +1.0f default that already
//     worked with no special case -- see coneCull's own comment for exactly what that does and does
//     not change. This is not a violation of this file's decoupling from ClusterBuilder.cpp/
//     ClusterAdapt.cpp (the file still does not include either): it is this file's own runtime
//     interpretation of a value that arrives through its own decoupled ClusterView::coneCutoff field,
//     same as the DECODE (value/127.0, straight through, no sign flip) already documented above, which
//     was and remains correct on its own. clusterConeCull (modules/rhi/src/RHIShaders.cpp) carries the
//     identical extra check, byte-for-byte, since it is a port of this file's coneCull.
//
// THE CUT PREDICATE, and what it actually relies on. `inCut()` is the standard "does this cluster
// belong in today's cut through the DAG" rule: a cluster is in the cut iff its own projected error is
// within budget AND no parent's projected error is ALSO within budget (a parent that already qualifies
// is coarser and cheaper, so it wins). It reads only `c` and `c.parents` -- O(1) lookups into the same
// flat array, never the whole DAG, never a sort, never another cluster's own selection result. That
// locality is what lets every cluster (and, eventually, every GPU thread processing one cluster) decide
// independently and still land on a consistent, crack-free cut -- which is the property the task
// brief calls the reason the parent-error->=child-error invariant exists at all.
//
// What that locality argument is FOUNDED on, honestly:
//   1. Error is monotonic non-decreasing child -> parent. GIVEN -- this is buildLodHierarchy's own
//      invariant (modules/trifactor/src/ClusterBuilder.cpp) and validateLodDag checks it explicitly.
//      Not re-derived or re-checked here; this file trusts its caller the same way the task brief says
//      to trust it ("preserve that reasoning", not re-prove it).
//   2. Projected (screen-space) error is monotonic child -> parent TOO, not just the raw geometric
//      error. This is the step that actually makes the local rule crack-free, and it needs one MORE
//      thing than (1) alone: that a parent's bounding sphere is not, for some camera position, closer
//      or smaller than its children's -- informally, "coarser also means at-least-as-enclosing".
//      buildLodHierarchy computes each level's bounding sphere fresh from that level's own simplified
//      geometry (meshopt_computeMeshletBounds); nothing in ClusterBuilder.cpp or validateLodDag
//      currently PROVES sphere containment across a DAG edge, only error monotonicity. In practice a
//      simplified group's footprint tracks its children's footprint closely (same locked outer
//      boundary, see ClusterBuilder.cpp's top-of-file comment), so this is expected to hold, but it is
//      NOT a checked invariant today. THIS IS A REAL GAP, not a hedge: if an adapter or a future
//      builder change ever produces a parent whose sphere sits unusually close to the camera relative
//      to a child's, this predicate can pick two clusters along one path (an overlap: costs overdraw,
//      not a crack) or, in a more contrived case, zero (a genuine gap). Flagging this explicitly rather
//      than asserting correctness un-earned is the point of the brief's HONESTY section; the fix, if
//      this ever bites, belongs in the builder (make parent bounds conservatively contain children's),
//      not here. The tests in tests/trifactor exercise this file's rule against synthetic DAGs built
//      WITH sphere containment (the expected case) and demonstrate the rule is at least correctly
//      implemented against its own contract; they cannot and do not prove the general case, because
//      that property is the builder's to guarantee, not this file's to assume.
//   3. LOD 0 clusters always have error 0.0 (ClusterBuilder.cpp never sets error on level-0 clusters;
//      the field's default is 0.0f). That is what stops the recursion needing an explicit "leaf, so
//      draw regardless of budget" special case the way modules/landscape's descend() has one for its
//      quadtree: a level-0 cluster's own projected error is always 0, which is <= any non-negative
//      threshold, so it always passes its own test. Whether it actually gets DRAWN still depends on
//      whether a parent also qualifies (coarser is cheaper and wins) -- exactly the same as any other
//      level.
//
// CULLING COMPOSES with the cut decision by simple AND, and does not need to be threaded through it.
// The cut computed while ignoring visibility is well-defined and independent of the camera's frustum
// (it only depends on projected SCREEN error, which frustum-culled clusters still have a well-defined
// value for); frustum/backface culling then removes off-screen or invisible members of that cut from
// what is actually drawn. Neither test ever needs to look at a NEIGHBOURING cluster's cull result, so
// this composition is exactly as local as the cut rule alone.
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"

#include <vector>

namespace aver::trifactor {

inline constexpr u32 kInvalidClusterId = 0xFFFFFFFFu;

// ---------------------------------------------------------------------------------------------- View
// The camera state this slice needs, and nothing else. See the file header for exactly which engine
// conventions this matches.
struct View {
    Vec3 eye{0, 0, 0};

    // view * projection, in the engine's row-vector convention (a clip coordinate is `[x y z 1] * M`).
    // SandboxApp.cpp builds its own viewProj_ the same way (`view * proj`) before handing it to the
    // device and to its own frustum cull, so a caller can pass that value straight through.
    Mat4 viewProj = Mat4::identity();

    f32 viewportHeightPx = 1080.0f;
    f32 verticalFovRadians = 60.0f * kDegToRad;   // matches SandboxApp's editor camera (`radians(60.0f)`)
};

// viewportHeightPx / (2 * tan(fovY/2)) -- see modules/landscape's SelectParams::projScale, which this
// mirrors exactly so a geometric error converts to screen pixels the same way both systems do it.
f32 projScale(const View& view);

// Screen-space projected error, in pixels, of a geometric error `errorCm` measured at a bounding
// sphere `sphereCenter`/`sphereRadius`. Distance is to the sphere's SURFACE (radial distance minus
// radius, floored at 0 for a camera inside it); at effectively zero distance this returns a large
// sentinel (unbounded error: always wants refining), matching
// modules/landscape/src/LandscapeTree.cpp's `descend()` exactly, including its 1e-3f distance
// threshold.
f32 screenSpaceErrorPx(const Vec3& sphereCenter, f32 sphereRadius, f32 errorCm, const View& view);

// -------------------------------------------------------------------------------------------- Frustum
// Six planes, `a*x + b*y + c*z + d >= 0` INSIDE, normalised so a plane evaluation is a true Euclidean
// distance (comparable directly against a bounding-sphere radius). Same convention, same derivation,
// as modules/landscape's Frustum -- see the file header for why this is a second copy of the
// arithmetic rather than a shared dependency.
struct Frustum {
    f32 plane[6][4] = {};

    // A default-constructed Frustum (all-zero planes) intersects every sphere of radius >= 0 -- see
    // the .cpp for why that is exact, not approximate. That makes `Frustum{}` a correct, deliberate
    // "cull nothing" value for a caller that does not want frustum culling at all, with no separate
    // enable flag needed on this type (selectVisibleClusters below still takes one, for clarity at
    // the call site).
    static Frustum fromViewProj(const Mat4& viewProj);

    // Conservative: a sphere straddling a plane counts as inside. False means "definitely fully
    // outside at least one plane" -- safe to cull.
    bool intersectsSphere(const Vec3& center, f32 radius) const;
};

// ---------------------------------------------------------------------------------------- ClusterView
// One cluster, as this slice needs to see it. NOT aver::trifactor::Cluster (ClusterBuilder.hpp) --
// see the file header. An adapter fills this from a real LodDag; the comments below say exactly what
// each field must already have done to it by the time it lands here.
struct ClusterView {
    u32 id = kInvalidClusterId;   // this cluster's own index into the array it lives in

    Vec3 sphereCenter{0, 0, 0};
    f32  sphereRadius = 0.0f;

    // Normal cone, DECODED to plain floats -- an adapter unpacks ClusterBounds' snorm8 axis/cutoff
    // (value/127.0) before filling these in; this file never sees the on-disk quantised form. `axis`
    // need not be pre-normalised (coneCull re-normalises defensively); `cutoff` is cos(halfAngle) in
    // the same sign convention as meshopt_Bounds::cone_cutoff.
    //
    // +1.0f is "never cull" HERE -- and this is worth being exact about, because it is the OPPOSITE
    // of what ClusterBuilder.cpp's own comments USED TO say its -127 (decoded -1.0f) sentinel means.
    // Verified empirically (tests/trifactor/src/ClusterSelectTest.cpp's first block, against REAL
    // meshopt_computeClusterBounds output, not re-derived by hand): coneCull's cull region is
    // `{d : dot(d,axis) >= cutoff}`, a spherical cap around `axis` that SHRINKS to a single
    // unreachable-in-practice direction as cutoff -> +1 and GROWS to cover every possible view
    // direction as cutoff -> -1. So -1.0f is actually "cull from every direction", and +1.0f is the
    // sentinel that means "never cull" -- see the file header's HONESTY note for the full account.
    // ClusterBuilder.cpp's encoder now agrees (it emits +127, never -127, as of the same fix); coneCull
    // itself ALSO now treats an incoming -1.0f as "never cull" directly, so that an .ocmesh cooked
    // before that fix -- still carrying the old -127 encoding on disk -- decodes safely here too,
    // without needing a re-cook. See coneCull's own comment for the exact boundary of that check.
    Vec3 coneApex{0, 0, 0};
    Vec3 coneAxis{0, 0, 1};
    f32  coneCutoff = 1.0f;

    // Monotonic parent >= max(children) -- see ClusterBuilder.hpp's Cluster::error. Units are
    // whatever the builder's simplifier produced (NOT yet screen-space pixels); screenSpaceErrorPx
    // above is what converts it.
    f32 error = 0.0f;

    u32 triangleCount = 0;

    // INFORMATIONAL ONLY -- no selection function below reads this. An adapter may fill it from
    // aver::trifactor::Cluster::level so a caller of selectVisibleClustersWithStats can report which
    // LOD a drawn cluster came from (`clusters[id].level`) without this file needing any concept of
    // "level" in its own decision logic -- the cut predicate never compares levels, only errors and
    // parent links, exactly per the brief's "the DAG's monotonic error is what makes this safe" note.
    u32 level = 0;

    // Ids into the SAME array this ClusterView came from (`clusters[id].id == id`), mirroring
    // Cluster::parents/children exactly -- including that a child can have MORE THAN ONE parent
    // (ClusterBuilder.cpp's buildLodHierarchy connects every cluster a re-split group produces as a
    // parent of every member of that group). Only `parents` is read by inCut/selectCluster; `children`
    // is carried because the brief's minimal-cluster shape asks for both and a future consumer (e.g. a
    // validator, or a "how deep is this subtree" debug query) may want it -- selection itself never
    // needs to look downward.
    std::vector<u32> parents;
    std::vector<u32> children;
};

// -------------------------------------------------------------------------------------- Backface cull
// True if EVERY triangle in `c`'s normal cone faces away from `eye` -- i.e. `c` is safe to skip
// entirely. Exact formula from third_party/meshoptimizer/src/meshoptimizer.h's
// meshopt_computeClusterBounds documentation (the cone-apex perspective variant), verified against
// real library output in tests/trifactor/src/ClusterSelectTest.cpp rather than trusted on the
// citation alone. Returns false (never culls) on a degenerate/near-zero axis, on `eye` coincident
// with the apex, or on `cutoff <= -1.0f` -- a caller feeding this file's OWN default (axis {0,0,1},
// cutoff +1.0f -- see ClusterView) gets the safe "never cull" answer without special-casing it, and so
// does a caller decoding an .ocmesh cooked before ClusterBuilder.cpp's sign fix, whose on-disk cone
// data still carries the old -127/-1.0f "never cull" encoding: this function now honours BOTH ends of
// the encoding as "never cull", not just the one ClusterBuilder.cpp currently emits, precisely so an
// already-cooked asset does not need a re-cook to render correctly. See ClusterView's coneCutoff
// comment and the file header's HONESTY note for the full account, and clusterConeCull
// (modules/rhi/src/RHIShaders.cpp) for the byte-for-byte GPU port of this exact check.
bool coneCull(const ClusterView& c, const Vec3& eye);

// ---------------------------------------------------------------------------------------- LOD + visibility
// Does `c`, and only `c`, belong in today's cut through the DAG? PURELY LOCAL: reads only `c` and its
// immediate parents (O(1) lookups into `clusters`, which must be indexed by id -- see ClusterView's
// comment). See the file header's three-point argument for what this relies on and what it does not
// (yet) prove.
bool inCut(const ClusterView& c, const std::vector<ClusterView>& clusters, f32 thresholdPx,
           const View& view);

// The full per-cluster decision this slice exists to answer: draw `c` this frame, or not. `inCut(...)`
// AND frustum-visible AND not backface-culled -- see the file header's "culling composes" note for why
// ANDing these three independent, local tests is sound.
bool selectCluster(const ClusterView& c, const std::vector<ClusterView>& clusters, f32 thresholdPx,
                    const View& view, const Frustum& frustum);

// Convenience: runs selectCluster over every entry of `clusters`, returning the ids selected. This is
// NOT the "global pass" the brief warns a correct design should not need -- every iteration is still
// the same O(1), independent-of-every-other-cluster decision; the loop exists only so a test (or an
// early single-threaded adapter) does not have to hand-roll it. A real GPU path calls the equivalent
// of selectCluster once per cluster/thread and never needs this function.
std::vector<u32> selectVisibleClusters(const std::vector<ClusterView>& clusters, f32 thresholdPx,
                                        const View& view, bool useFrustum = true);

// ---------------------------------------------------------------------------------------- Counted select
// The counts are as much the deliverable of this slice as the selection itself -- this is how anyone
// (a reviewer, a future GPU-indirect-draw path, a regression test) proves the selector is actually
// rejecting triangles rather than just compiling. Every cluster passed in falls into EXACTLY ONE of
// the four buckets below, in this priority order (matching selectCluster's own AND-chain, tested in
// the same sequence so the two never disagree):
//   1. frustumCulled -- sphere entirely outside the view frustum.
//   2. coneCulled     -- (survived 1) every triangle in the cluster's normal cone faces away from eye.
//   3. lodCollapsed   -- (survived 1,2) not in today's cut: either this cluster's own projected error
//                        already exceeds budget (something finer must stand in) or a coarser parent
//                        already meets budget too (that parent is cheaper and wins) -- both are cases
//                        of "this exact cluster does not get drawn because of the LOD decision", the
//                        thing item 5 of the brief calls LOD-collapsed.
//   4. drawn          -- survived all three; this is what selectCluster would also return true for.
// `tested == frustumCulled + coneCulled + lodCollapsed + drawn` holds by construction (one increment
// per cluster, always exactly one bucket), not by coincidence -- see the .cpp.
struct SelectionStats {
    u32 tested = 0;
    u32 frustumCulled = 0;
    u32 coneCulled = 0;
    u32 lodCollapsed = 0;
    u32 drawn = 0;

    // "Triangles before": the full-detail baseline, summing triangleCount over every LEAF cluster
    // (children.empty() -- this file carries no separate "is LOD 0" flag, and a leaf is exactly a
    // cluster nothing else feeds into). This is what rendering the whole input at maximum resolution,
    // with no culling and no LOD collapse at all, would have cost -- the number this slice exists to
    // shrink. "Triangles after": triangleCount summed over only the clusters actually drawn (bucket 4
    // above) -- the number of triangles this frame actually pays for. The gap between the two is the
    // triangles-rejected number the brief's economics section (frame time ~2.1ms/million triangles)
    // makes this slice's whole point.
    u64 trianglesBefore = 0;
    u64 trianglesAfter = 0;
};

struct SelectionResult {
    std::vector<u32> drawnIds;   // same contents selectVisibleClusters would return
    SelectionStats stats;
};

// Same selection as selectVisibleClusters, plus the counts above. Still not a "global pass": the
// stats are accumulated incrementally, one cluster at a time, from decisions each cluster already
// makes independently -- no second reconciliation step, no sort.
SelectionResult selectVisibleClustersWithStats(const std::vector<ClusterView>& clusters, f32 thresholdPx,
                                                const View& view, bool useFrustum = true);

} // namespace aver::trifactor
