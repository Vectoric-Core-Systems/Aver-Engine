// tests/trifactor/src/ClusterSelectTest.cpp -- pure CPU: the selection maths in
// modules/trifactor/include/aver/trifactor/ClusterSelect.hpp. No GPU, no RHI, no mesh import.
// Exit code = failure count, same convention as tests/landscape/src/LandscapeTreeTest.cpp.
//
// This file DOES include <meshoptimizer.h> directly, unlike ClusterSelect.hpp/.cpp themselves (see
// their header comment for why THEY must not) -- a test is exactly the place allowed to reach past
// the file-under-test's own decoupling to check it against the real thing it will eventually receive
// decoded data from. The very first block below exists for that reason alone: it calls
// meshopt_computeClusterBounds on a real, tiny, hand-built mesh and feeds the RAW (unquantised)
// result straight into ClusterSelect's coneCull, so the cone-cull sign convention is checked against
// actual library output, not against a hand-picked apex/axis/cutoff triple this file also invented.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/trifactor/ClusterSelect.hpp"

#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::trifactor;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// A flat 1x1 quad in the Z=0 plane, wound so its geometric normal is +Z (v0,v1,v2 counter-clockwise
// as seen from +Z: cross(v1-v0, v2-v0) = cross((1,0,0),(0,1,0)) = (0,0,1)).
static void makeFlatQuadFacingPlusZ(std::vector<f32>& positions, std::vector<u32>& indices) {
    positions = {
        0, 0, 0,
        1, 0, 0,
        0, 1, 0,
        1, 1, 0,
    };
    indices = {0, 1, 2, 1, 3, 2};
}

int main() {
    AVER_INFO("=== coneCull sign convention, against REAL meshopt_computeClusterBounds output ===");
    {
        std::vector<f32> positions;
        std::vector<u32> indices;
        makeFlatQuadFacingPlusZ(positions, indices);

        const meshopt_Bounds b = meshopt_computeClusterBounds(
            indices.data(), indices.size(), positions.data(), positions.size() / 3, sizeof(f32) * 3);

        AVER_INFO("  measured: axis=({:.4f},{:.4f},{:.4f}) cutoff={:.4f} apex=({:.4f},{:.4f},{:.4f})",
                  b.cone_axis[0], b.cone_axis[1], b.cone_axis[2], b.cone_cutoff,
                  b.cone_apex[0], b.cone_apex[1], b.cone_apex[2]);

        // A perfectly flat, coherently-wound patch: every triangle has the exact same normal, so the
        // cone axis should point straight along it. This is what pins down the axis SIGN empirically
        // -- not assumed, read from the library.
        check(b.cone_axis[2] > 0.9f, "a flat patch facing +Z gets a cone axis whose Z is near +1 (" +
                                          std::to_string(b.cone_axis[2]) + ")");
        // NOT asserting cutoff is near 1.0 here, on purpose, and that is itself worth recording: a
        // first draft of this test expected exactly that ("zero angular spread -> cutoff~1") and
        // measured 0.0 instead. meshopt_computeClusterBounds's apex-based cutoff also has to account
        // for how the view direction to the apex varies across the cluster's own spatial EXTENT, not
        // only its normal spread -- a perfectly flat but not-infinitesimally-small patch still gets a
        // real (non-trivial) cutoff for that reason. Sanity-checked instead: finite and in the
        // formula's valid domain.
        check(b.cone_cutoff >= -1.0001f && b.cone_cutoff <= 1.0001f,
              "cutoff is within the formula's valid [-1,1] domain (" + std::to_string(b.cone_cutoff) + ")");

        ClusterView c;
        c.coneApex = {b.cone_apex[0], b.cone_apex[1], b.cone_apex[2]};
        c.coneAxis = {b.cone_axis[0], b.cone_axis[1], b.cone_axis[2]};
        c.coneCutoff = b.cone_cutoff;

        // Camera on the +Z side (the side the measured normal points toward): the patch's front face
        // is genuinely visible from here, so it must NOT be culled.
        const bool culledFromAbove = coneCull(c, Vec3{0.5f, 0.5f, 10.0f});
        check(!culledFromAbove, "a camera on the normal's own side sees the front face: not culled");

        // Camera on the -Z side (behind the surface, on the opposite side from the measured normal):
        // only the back face is geometrically presentable from here, so it must be culled.
        const bool culledFromBelow = coneCull(c, Vec3{0.5f, 0.5f, -10.0f});
        check(culledFromBelow, "a camera on the opposite side sees only the back face: culled");

        // THE ACTUAL FINDING this block exists to surface: this same formula, applied to
        // ClusterBuilder.cpp's own "-127 / never cull" sentinel (decoded, cutoff = -1.0f) rather than
        // to a real measured cutoff, culls from EVERY direction instead of none -- see
        // ClusterSelect.hpp's file header and ClusterView::coneCutoff for the full account. Checked
        // right here with the same coneCull this whole file is validating, not asserted separately:
        ClusterView neverCullPerClusterBuilder = c;
        neverCullPerClusterBuilder.coneAxis = {0, 0, 1};
        neverCullPerClusterBuilder.coneCutoff = -1.0f;   // ClusterBuilder.cpp's -127, decoded
        check(coneCull(neverCullPerClusterBuilder, Vec3{0.5f, 0.5f, 10.0f}) &&
                  coneCull(neverCullPerClusterBuilder, Vec3{0.5f, 0.5f, -10.0f}),
              "ClusterBuilder.cpp's -127 'never cull' sentinel, decoded and run through the real "
              "formula, culls from BOTH sides -- the opposite of what its own comment claims "
              "(ClusterBuilder.cpp:114,126,133-136; this file's ClusterView::coneCutoff default is "
              "+1.0f, not -1.0f, for exactly this reason)");
    }

    AVER_INFO("=== coneCull edge cases ===");
    {
        ClusterView c;
        c.coneApex = {0, 0, 0};
        c.coneAxis = {0, 0, 0};   // degenerate/zero axis
        c.coneCutoff = 0.9f;      // otherwise a tight, aggressive cone
        check(!coneCull(c, Vec3{0, 0, -100.0f}), "a degenerate (zero) cone axis never culls");

        ClusterView atApex;
        atApex.coneApex = {5, 5, 5};
        atApex.coneAxis = {0, 0, 1};
        atApex.coneCutoff = 0.99f;
        check(!coneCull(atApex, Vec3{5, 5, 5}), "eye coincident with the apex never culls (division guard)");
    }

    AVER_INFO("=== projScale / screenSpaceErrorPx (mirrors LandscapeTree.cpp's descend()) ===");
    {
        View v;
        v.viewportHeightPx = 1080.0f;
        v.verticalFovRadians = radians(60.0f);
        const f32 expected = 1080.0f / (2.0f * std::tan(radians(30.0f)));
        check(std::fabs(projScale(v) - expected) < 1e-2f,
              "projScale == viewportHeightPx / (2*tan(fovY/2)) (" + std::to_string(projScale(v)) +
                  " vs " + std::to_string(expected) + ")");

        v.eye = Vec3{0, 0, 0};
        const f32 dist = 950.0f;
        const f32 want = 4.0f * projScale(v) / dist;
        const f32 got = screenSpaceErrorPx(Vec3{1000.0f, 0, 0}, 50.0f, 4.0f, v);
        check(std::fabs(got - want) < 1e-2f, "screenSpaceErrorPx == errorCm * projScale / (dist-radius) (" +
                                                  std::to_string(got) + " vs " + std::to_string(want) + ")");

        const f32 near_ = screenSpaceErrorPx(Vec3{100, 0, 0}, 0.0f, 4.0f, v);
        const f32 far_ = screenSpaceErrorPx(Vec3{1000, 0, 0}, 0.0f, 4.0f, v);
        check(far_ < near_, "the same geometric error projects to fewer pixels farther away (" +
                                 std::to_string(near_) + " -> " + std::to_string(far_) + ")");

        const f32 inside = screenSpaceErrorPx(Vec3{0, 0, 0}, 100.0f, 1.0f, v);
        check(inside >= 1e29f, "a camera inside the sphere's radius gets the unbounded sentinel (" +
                                    std::to_string(inside) + ")");

        check(screenSpaceErrorPx(Vec3{1000, 0, 0}, 50.0f, 0.0f, v) == 0.0f,
              "zero geometric error projects to exactly zero pixels regardless of distance");
    }

    AVER_INFO("=== Frustum: same math as modules/landscape's Frustum / SandboxApp's per-entity cull ===");
    {
        // eye at the origin, looking down world +X (per the engine's lookAtLH/lookAtLH basis, +X is
        // "right" only when the caller wants it to be -- here it is the LOOK target - eye direction).
        // up = {0,0,1}, matching the editor camera exactly.
        View v;
        v.eye = Vec3{0, 0, 0};
        const Mat4 view = Mat4::lookAtLH(v.eye, v.eye + Vec3{1, 0, 0}, Vec3{0, 0, 1});
        const Mat4 proj = Mat4::perspectiveLH(radians(90.0f), 1.0f, 1.0f, 1000.0f);
        v.viewProj = view * proj;

        const Frustum f = Frustum::fromViewProj(v.viewProj);

        check(f.intersectsSphere(Vec3{500, 0, 0}, 10.0f), "dead ahead, well within near/far: inside");
        check(!f.intersectsSphere(Vec3{-500, 0, 0}, 10.0f), "behind the camera: outside");
        check(!f.intersectsSphere(Vec3{5000, 0, 0}, 1.0f), "beyond the far plane (1000): outside");
        check(!f.intersectsSphere(Vec3{0.5f, 0, 0}, 0.01f), "closer than the near plane (1.0): outside");
        // 90-degree vertical FOV, aspect 1:1 -> 45-degree half-angle -> the frustum's half-width at
        // depth 100 is exactly 100*tan(45) = 100.
        check(!f.intersectsSphere(Vec3{100, 10000, 0}, 10.0f), "far outside the 45-degree half-angle: outside");
        check(f.intersectsSphere(Vec3{500, 550, 0}, 100.0f),
              "a sphere straddling the right plane (half-width 500 at depth 500) counts as inside");

        const Frustum def;
        check(def.intersectsSphere(Vec3{1e6f, 1e6f, 1e6f}, 0.0f),
              "a default-constructed Frustum (all-zero planes) intersects everything, as documented");
    }

    // ------------------------------------------------------------------------------------------------
    // A synthetic three-level DAG (a small stand-in for LodDag), built with the two properties the cut
    // predicate's correctness argument needs (see ClusterSelect.hpp's file header): error monotonic
    // parent >= child, AND parent bounding spheres containing their children's. Four LOD-0 leaves under
    // two LOD-1 parents under one LOD-2 root -- deliberately not a single chain, so the "which of my
    // several children is in the cut" and "do the SIBLING children under one parent get exactly one
    // level selected between them" cases are both exercised.
    AVER_INFO("=== the cut predicate: crack-free, gap-free, on a synthetic DAG ===");
    {
        std::vector<ClusterView> clusters(7);
        // ids: 0,1 = LOD0 under parent 4; 2,3 = LOD0 under parent 5; 4,5 = LOD1 under root 6; 6 = root.
        auto sphere = [](ClusterView& c, Vec3 center, f32 radius) { c.sphereCenter = center; c.sphereRadius = radius; };

        for (u32 i = 0; i < 7; ++i) clusters[i].id = i;

        sphere(clusters[0], Vec3{-50, 0, 0}, 20.0f);
        sphere(clusters[1], Vec3{50, 0, 0}, 20.0f);
        sphere(clusters[2], Vec3{-50, 200, 0}, 20.0f);
        sphere(clusters[3], Vec3{50, 200, 0}, 20.0f);
        clusters[0].error = clusters[1].error = clusters[2].error = clusters[3].error = 0.0f;   // LOD 0

        // Parent 4 encloses children 0,1: center between them, radius covering both.
        sphere(clusters[4], Vec3{0, 0, 0}, 90.0f);
        clusters[4].error = 5.0f;
        // Parent 5 encloses children 2,3.
        sphere(clusters[5], Vec3{0, 200, 0}, 90.0f);
        clusters[5].error = 5.0f;

        // Root encloses both level-1 parents.
        sphere(clusters[6], Vec3{0, 100, 0}, 220.0f);
        clusters[6].error = 20.0f;

        clusters[0].parents = {4}; clusters[1].parents = {4};
        clusters[2].parents = {5}; clusters[3].parents = {5};
        clusters[4].children = {0, 1}; clusters[4].parents = {6};
        clusters[5].children = {2, 3}; clusters[5].parents = {6};
        clusters[6].children = {4, 5};

        // All this DAG's geometry sits in the Z=0 plane (varying only in X and Y), so the cameras
        // below look ALONG Y (eye offset from the centroid in -Y, target at the centroid) with
        // up={0,0,1} -- perpendicular to the look direction, avoiding the degenerate lookAtLH case
        // (up parallel to forward, which collapses the view matrix) an eye offset in Z would hit here.
        const Vec3 centroid{0, 100, 0};
        View v;
        v.viewportHeightPx = 1080.0f;
        v.verticalFovRadians = radians(60.0f);
        // A wide-open frustum pointed generally at the cluster so frustum culling never interferes
        // with these assertions -- this section is about the LOD predicate alone.
        v.eye = centroid - Vec3{0, 100000.0f, 0};
        const Mat4 view = Mat4::lookAtLH(v.eye, centroid, Vec3{0, 0, 1});
        const Mat4 proj = Mat4::perspectiveLH(radians(60.0f), 1.0f, 1.0f, 1000000.0f);
        v.viewProj = view * proj;
        const Frustum wideOpen = Frustum::fromViewProj(v.viewProj);

        // Helper: which ids are selected at a given threshold (LOD only, frustum forced wide open).
        auto selectedAt = [&](f32 thresholdPx) {
            std::vector<u32> ids;
            for (const ClusterView& c : clusters)
                if (selectCluster(c, clusters, thresholdPx, v, wideOpen)) ids.push_back(c.id);
            return ids;
        };

        // Camera essentially AT the geometry (v.eye set far below only for the frustum test above; use
        // a fresh close view here) selects every LOD-0 leaf and nothing coarser.
        {
            View close = v;
            close.eye = centroid - Vec3{0, 300.0f, 0};
            const Mat4 closeView = Mat4::lookAtLH(close.eye, centroid, Vec3{0, 0, 1});
            close.viewProj = closeView * proj;
            const Frustum closeFrustum = Frustum::fromViewProj(close.viewProj);
            std::vector<u32> ids;
            for (const ClusterView& c : clusters)
                if (selectCluster(c, clusters, /*thresholdPx=*/1.0f, close, closeFrustum)) ids.push_back(c.id);
            std::sort(ids.begin(), ids.end());
            check(ids == std::vector<u32>({0, 1, 2, 3}),
                  "a close camera and a tight budget selects exactly the four LOD-0 leaves, nothing coarser");
        }

        // Camera far enough away that even the root's projected error is tiny selects ONLY the root.
        // Stays within `proj`'s own zFar (1,000,000) -- unlike the close-camera view above, this one
        // reuses `proj` as-is, so the eye has to stay inside it or the root gets frustum-culled
        // instead of exercising the LOD predicate this block is actually testing.
        {
            View far_ = v;
            far_.eye = centroid - Vec3{0, 500000.0f, 0};
            const Mat4 farView = Mat4::lookAtLH(far_.eye, centroid, Vec3{0, 0, 1});
            far_.viewProj = farView * proj;
            const Frustum farFrustum = Frustum::fromViewProj(far_.viewProj);
            std::vector<u32> ids;
            for (const ClusterView& c : clusters)
                if (selectCluster(c, clusters, /*thresholdPx=*/2.0f, far_, farFrustum)) ids.push_back(c.id);
            check(ids == std::vector<u32>({6}), "a very distant camera and a loose budget selects only the root");
        }

        // At EVERY threshold tried, the cut contains exactly one cluster per root-to-leaf path: for
        // each of the four leaves, exactly one of {leaf, its LOD-1 parent, the root} is selected. This
        // is the actual crack-free/gap-free/no-duplicate property the predicate exists to guarantee,
        // checked directly rather than inferred from two single-threshold snapshots.
        bool allPathsExactlyOne = true;
        std::string firstBadPath;
        for (f32 thresholdPx : {0.05f, 0.2f, 1.0f, 3.0f, 8.0f, 20.0f, 60.0f, 200.0f}) {
            const std::vector<u32> ids = selectedAt(thresholdPx);
            const auto has = [&](u32 id) { return std::find(ids.begin(), ids.end(), id) != ids.end(); };
            const u32 leafParent[4] = {4, 4, 5, 5};
            for (u32 leaf = 0; leaf < 4; ++leaf) {
                const int count = (has(leaf) ? 1 : 0) + (has(leafParent[leaf]) ? 1 : 0) + (has(6) ? 1 : 0);
                if (count != 1) {
                    allPathsExactlyOne = false;
                    firstBadPath = "threshold=" + std::to_string(thresholdPx) + " leaf=" + std::to_string(leaf) +
                                    " selected-along-path=" + std::to_string(count);
                    break;
                }
            }
            if (!allPathsExactlyOne) break;
        }
        check(allPathsExactlyOne, "every root-to-leaf path has exactly one cluster in the cut, at every "
                                   "threshold tried" + (firstBadPath.empty() ? "" : (" (first break: " + firstBadPath + ")")));
    }

    AVER_INFO("=== multi-parent clusters (a group's re-split can give one child several parents) ===");
    {
        // Mirrors buildLodHierarchy's actual shape: a child can have MORE than one parent (every
        // cluster a re-split group produces is linked as a parent of every member of that group).
        // Both parents here carry the SAME error (as buildLodHierarchy always assigns, propagatedError
        // applied uniformly across newIds) but sit at different positions.
        std::vector<ClusterView> clusters(3);
        clusters[0].id = 0; clusters[0].sphereCenter = {0, 0, 0}; clusters[0].sphereRadius = 10.0f;
        clusters[0].error = 0.0f; clusters[0].parents = {1, 2};

        clusters[1].id = 1; clusters[1].sphereCenter = {0, 0, 0}; clusters[1].sphereRadius = 80.0f;
        clusters[1].error = 10.0f;   // this one, positioned close, will satisfy a loose threshold first

        clusters[2].id = 2; clusters[2].sphereCenter = {0, 5000, 0}; clusters[2].sphereRadius = 80.0f;
        clusters[2].error = 10.0f;   // far away: at the SAME threshold, this one may not yet qualify

        View v;
        v.eye = Vec3{0, 0, -1000.0f};
        v.viewportHeightPx = 1080.0f;
        v.verticalFovRadians = radians(60.0f);
        const Mat4 view = Mat4::lookAtLH(v.eye, Vec3{0, 0, 0}, Vec3{0, 1, 0});
        const Mat4 proj = Mat4::perspectiveLH(radians(60.0f), 1.0f, 1.0f, 100000.0f);
        v.viewProj = view * proj;
        const Frustum wideOpen = Frustum::fromViewProj(v.viewProj);

        // A threshold where parent 1 (near) qualifies but parent 2 (far, off to the side) does not:
        // the child must still be excluded, because ANY qualifying parent is enough to drop it.
        const f32 thresholdPx = screenSpaceErrorPx(clusters[1].sphereCenter, clusters[1].sphereRadius,
                                                     clusters[1].error, v) + 0.5f;
        check(!inCut(clusters[0], clusters, thresholdPx, v),
              "a child with several parents is excluded once ANY ONE parent already qualifies");
    }

    // ------------------------------------------------------------------------------------------------
    // selectVisibleClustersWithStats: the counts are the deliverable per the brief's item 5. Reuses the
    // same seven-cluster DAG shape as the cut-predicate block above (four LOD0 leaves under two LOD1
    // parents under one root), but now with triangleCount set on every cluster so trianglesBefore/After
    // are actually exercised, plus one cluster placed behind the camera and one with a cone facing away
    // so all four buckets (frustum/cone/lodCollapsed/drawn) are hit in a single run.
    AVER_INFO("=== selectVisibleClustersWithStats: counts are the deliverable ===");
    {
        std::vector<ClusterView> clusters(7);
        auto sphere = [](ClusterView& c, Vec3 center, f32 radius) { c.sphereCenter = center; c.sphereRadius = radius; };
        for (u32 i = 0; i < 7; ++i) clusters[i].id = i;

        // Leaves: 124 triangles each (the format's per-cluster cap), so trianglesBefore has a clean,
        // checkable value independent of how many leaves end up drawn vs collapsed.
        sphere(clusters[0], Vec3{-50, 0, 0}, 20.0f);   clusters[0].triangleCount = 124;
        sphere(clusters[1], Vec3{50, 0, 0}, 20.0f);    clusters[1].triangleCount = 124;
        sphere(clusters[2], Vec3{-50, 200, 0}, 20.0f); clusters[2].triangleCount = 124;
        sphere(clusters[3], Vec3{50, 200, 0}, 20.0f);  clusters[3].triangleCount = 124;
        clusters[0].error = clusters[1].error = clusters[2].error = clusters[3].error = 0.0f;

        sphere(clusters[4], Vec3{0, 0, 0}, 90.0f);     clusters[4].error = 5.0f;  clusters[4].triangleCount = 124;
        sphere(clusters[5], Vec3{0, 200, 0}, 90.0f);   clusters[5].error = 5.0f;  clusters[5].triangleCount = 124;
        sphere(clusters[6], Vec3{0, 100, 0}, 220.0f);  clusters[6].error = 20.0f; clusters[6].triangleCount = 124;

        clusters[0].parents = {4}; clusters[1].parents = {4};
        clusters[2].parents = {5}; clusters[3].parents = {5};
        clusters[4].children = {0, 1}; clusters[4].parents = {6};
        clusters[5].children = {2, 3}; clusters[5].parents = {6};
        clusters[6].children = {4, 5};

        // Give every cluster a plausible cone (facing +Z, wide enough to never cone-cull for the eye
        // positions below) so coneCull's normal codepath, not just its degenerate-axis bypass, runs
        // inside selectVisibleClustersWithStats. cutoff MUST be close to +1 here, not -1: per this
        // file's own sign-convention finding (see the first block above), a cutoff near -1 is the
        // "cull from nearly every direction" end, the opposite of "wide open". All these clusters sit
        // at z=0 with an eye also at z=0 (offset only in Y), so dir's z-component is ~0; cutoff=0.9
        // needs dot(dir,axis)>=0.9 to cull, which ~0 never reaches -- reliably not culled.
        for (ClusterView& c : clusters) { c.coneApex = c.sphereCenter; c.coneAxis = {0, 0, 1}; c.coneCutoff = 0.9f; }

        const Vec3 centroid{0, 100, 0};
        View v;
        v.viewportHeightPx = 1080.0f;
        v.verticalFovRadians = radians(60.0f);
        // Far enough that the root's projected error clears a loose budget (mirrors the "far camera"
        // block above), so LOD collapse actually happens: leaves and LOD1 parents are eligible members
        // of the DAG but get superseded by the coarser root, landing them in lodCollapsed, not drawn.
        v.eye = centroid - Vec3{0, 500000.0f, 0};
        const Mat4 view = Mat4::lookAtLH(v.eye, centroid, Vec3{0, 0, 1});
        const Mat4 proj = Mat4::perspectiveLH(radians(60.0f), 1.0f, 1.0f, 1000000.0f);
        v.viewProj = view * proj;

        const SelectionResult result = selectVisibleClustersWithStats(clusters, /*thresholdPx=*/2.0f, v, true);

        check(result.stats.tested == 7, "tested counts every cluster passed in (" +
                                             std::to_string(result.stats.tested) + ")");
        check(result.stats.tested ==
                  result.stats.frustumCulled + result.stats.coneCulled + result.stats.lodCollapsed + result.stats.drawn,
              "tested == frustumCulled + coneCulled + lodCollapsed + drawn (" + std::to_string(result.stats.tested) +
                  " vs " + std::to_string(result.stats.frustumCulled + result.stats.coneCulled +
                                           result.stats.lodCollapsed + result.stats.drawn) + ")");
        check(result.stats.drawn == 1 && result.drawnIds == std::vector<u32>({6}),
              "only the root is drawn at this distance/budget (drawn=" + std::to_string(result.stats.drawn) + ")");
        check(result.stats.lodCollapsed == 6,
              "the six non-root clusters are all LOD-collapsed into the root, not culled (" +
                  std::to_string(result.stats.lodCollapsed) + ")");
        check(result.stats.frustumCulled == 0 && result.stats.coneCulled == 0,
              "nothing is frustum- or cone-culled in this setup (frustum=" +
                  std::to_string(result.stats.frustumCulled) + " cone=" + std::to_string(result.stats.coneCulled) + ")");
        check(result.stats.trianglesBefore == 4 * 124,
              "trianglesBefore sums only the leaves (4*124=496, got " +
                  std::to_string(result.stats.trianglesBefore) + ")");
        check(result.stats.trianglesAfter == 124,
              "trianglesAfter counts only the one drawn (root) cluster's own triangles (" +
                  std::to_string(result.stats.trianglesAfter) + ")");
        check(result.stats.trianglesAfter < result.stats.trianglesBefore,
              "LOD collapse actually rejects triangles: after (" + std::to_string(result.stats.trianglesAfter) +
                  ") < before (" + std::to_string(result.stats.trianglesBefore) + ")");

        // A cluster placed behind the camera (frustum-culled) and one whose cone faces the camera
        // directly (cone-culled), added to the same set, must land in their own buckets and not perturb
        // the others' counts.
        std::vector<ClusterView> withCulled = clusters;
        ClusterView behind;
        behind.id = 7;
        behind.sphereCenter = centroid - Vec3{0, 1000000.0f, 0};   // far behind the eye
        behind.sphereRadius = 10.0f;
        behind.triangleCount = 50;
        behind.coneAxis = {0, 0, 1};
        behind.coneCutoff = -0.9f;
        withCulled.push_back(behind);

        // dir = normalize(apex - eye) points FROM eye TO apex. With eye at centroid - (0,500000,0), dir
        // here is exactly (0,1,0). coneCull's cull condition is dot(dir,axis) >= cutoff, so axis={0,1,0}
        // (pointing the SAME way as dir, i.e. further away from the eye -- the surface's front face
        // points away from the viewer) gives dot=1, comfortably >= cutoff=-0.9: reliably culled. (Using
        // axis={0,-1,0} here would be wrong: dot would be exactly -1, the one direction still excluded
        // by a -0.9 cutoff's cull region -- confirmed by hand before writing this, not by running it.)
        ClusterView facingAway;
        facingAway.id = 8;
        facingAway.sphereCenter = centroid;
        facingAway.sphereRadius = 5.0f;
        facingAway.triangleCount = 60;
        facingAway.coneApex = centroid;
        facingAway.coneAxis = {0, 1, 0};
        facingAway.coneCutoff = -0.9f;
        withCulled.push_back(facingAway);

        const SelectionResult result2 = selectVisibleClustersWithStats(withCulled, /*thresholdPx=*/2.0f, v, true);
        check(result2.stats.tested == 9, "tested grows with the input (" + std::to_string(result2.stats.tested) + ")");
        check(result2.stats.tested == result2.stats.frustumCulled + result2.stats.coneCulled +
                                           result2.stats.lodCollapsed + result2.stats.drawn,
              "counts still add up with frustum- and cone-culled clusters present");
        check(result2.stats.frustumCulled >= 1, "the cluster behind the camera is frustum-culled (" +
                                                     std::to_string(result2.stats.frustumCulled) + ")");
        check(result2.stats.coneCulled >= 1, "the cluster whose cone faces away from the eye is cone-culled (" +
                                                  std::to_string(result2.stats.coneCulled) + ")");
    }

    AVER_INFO("=== {} assertions, {} failed ===", g_checks, g_failures);
    return g_failures;
}
