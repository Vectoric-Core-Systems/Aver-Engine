// modules/trifactor/src/ClusterSelect.cpp -- see ClusterSelect.hpp for the full design rationale.
// This file only implements what the header already documents; new reasoning does not belong here.
#include "aver/trifactor/ClusterSelect.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aver::trifactor {

f32 projScale(const View& view) {
    return view.viewportHeightPx / (2.0f * std::tan(view.verticalFovRadians * 0.5f));
}

f32 screenSpaceErrorPx(const Vec3& sphereCenter, f32 sphereRadius, f32 errorCm, const View& view) {
    // Radial distance to the sphere's SURFACE, floored at 0 for a camera inside it -- identical to
    // modules/landscape/src/LandscapeTree.cpp's descend().
    const f32 dist = std::fmax(aver::dist(view.eye, sphereCenter) - sphereRadius, 0.0f);
    // Same 1e-3f threshold and the same "unbounded, so always refine" sentinel as descend()'s
    // `dist > 1e-3f ? ... : 1e30f` -- a camera effectively inside the cluster cannot be given a
    // meaningful screen-space error by this formula (it would divide by ~0), and treating that as
    // "definitely too coarse" is the conservative answer: refine rather than risk under-drawing
    // something the camera is sitting inside of.
    if (dist <= 1e-3f) return 1e30f;
    return errorCm * projScale(view) / dist;
}

Frustum Frustum::fromViewProj(const Mat4& m) {
    // ROW VECTORS: clip = [x y z 1] * m, so clip.x comes from COLUMN 0 of m, clip.w from column 3.
    // left:  clip.x >= -clip.w  <=>  clip.x + clip.w >= 0  -> plane = column0 + column3
    // right: clip.x <=  clip.w  <=>  clip.w - clip.x >= 0  -> plane = column3 - column0
    // ...and so on for y (bottom/top) and z (near/far), with the LH [0,1] depth range meaning
    // "near" is simply clip.z >= 0 (column2 alone) rather than clip.z >= -clip.w.
    // Exactly modules/landscape/src/LandscapeTree.cpp's Frustum::fromViewProj and
    // sandbox/src/SandboxApp.cpp's per-entity cull (:1571-1586) -- same columns, same signs.
    Frustum f;
    for (int i = 0; i < 4; ++i) {
        f.plane[0][i] = m.m[i][3] + m.m[i][0];   // left
        f.plane[1][i] = m.m[i][3] - m.m[i][0];   // right
        f.plane[2][i] = m.m[i][3] + m.m[i][1];   // bottom
        f.plane[3][i] = m.m[i][3] - m.m[i][1];   // top
        f.plane[4][i] = m.m[i][2];               // near
        f.plane[5][i] = m.m[i][3] - m.m[i][2];   // far
    }
    // Normalise by the (a,b,c) length so `dot(normal,p)+d` is a true Euclidean distance, comparable
    // directly against a bounding-sphere radius. A default-constructed Mat4::identity() passed here
    // (not expected in practice, but worth being exact about) produces finite, if degenerate, planes;
    // the only way `len` is ~0 is a genuinely singular viewProj, in which case the plane is left
    // un-normalised rather than divided by ~0 -- still finite, just not distance-calibrated for that
    // one degenerate plane.
    for (int p = 0; p < 6; ++p) {
        const f32 len = std::sqrt(f.plane[p][0] * f.plane[p][0] + f.plane[p][1] * f.plane[p][1] +
                                   f.plane[p][2] * f.plane[p][2]);
        if (len > 1e-20f)
            for (int i = 0; i < 4; ++i) f.plane[p][i] /= len;
    }
    return f;
}

bool Frustum::intersectsSphere(const Vec3& center, f32 radius) const {
    // A default Frustum{} (all-zero planes) evaluates every plane to exactly 0, and `0 < -radius` is
    // false for any radius >= 0 -- so it intersects everything, by construction, with no special case
    // needed. That is the exact property the header promises for a caller that wants "cull nothing".
    for (int p = 0; p < 6; ++p) {
        const f32 d = plane[p][0] * center.x + plane[p][1] * center.y + plane[p][2] * center.z + plane[p][3];
        if (d < -radius) return false;
    }
    return true;
}

bool coneCull(const ClusterView& c, const Vec3& eye) {
    const Vec3 axis = c.coneAxis.getSafeNormal();
    if (axis.sizeSquared() < 0.5f) return false;   // degenerate/never-cull axis: don't cull

    // THE RUNTIME HALF OF THE SIGN-INVERSION FIX (see ClusterSelect.hpp's file header and
    // ClusterView::coneCutoff for the full account). cutoff <= -1.0f is reachable in exactly two ways:
    // (1) a genuinely degenerate cone that ClusterBuilder.cpp's quantizeConeConservative could not
    // safely widen/shrink a real half-angle for, which -- BEFORE that function's own fix -- it stored
    // as -127 believing that to be the conservative "never cull" end; and (2) the mathematical limit
    // case of a real (non-degenerate) cone whose true half-angle is exactly pi, which provides zero
    // culling information for the same reason a fully degenerate cone does (some triangle in the
    // cluster faces every possible direction, so no viewpoint is "definitely behind all of them").
    // Both cases mean the same thing: this cone cannot tell you anything, so don't cull on it.
    //
    // Fixed HERE, not only at the cook (ClusterBuilder.cpp), because assets already baked to disk with
    // the OLD (backwards) encoding still carry -127 -- ClusterBuilder.cpp's own fix only changes what
    // FUTURE cooks produce, and re-cooking irreplaceable source art on every machine that has an old
    // .ocmesh is not a cost this fix should force. Honouring the sentinel here, symmetrically with the
    // +1.0f "never cull" that already works with no special case, repairs every already-cooked asset
    // with no re-cook required, while leaving every value in between (-127 < cutoff < 127, i.e. every
    // REAL, non-degenerate cone) running through the exact same formula as before -- this does not
    // widen or otherwise weaken cone culling for any cluster whose cutoff isn't sitting on this exact
    // boundary. clusterConeCull (modules/rhi/src/RHIShaders.cpp) is a byte-for-byte port and carries
    // the identical check; tests/trifactor/src/ClusterSelectTest.cpp asserts this exact behaviour.
    if (c.coneCutoff <= -1.0f) return false;

    const Vec3 toApex = c.coneApex - eye;
    const f32 lenSq = toApex.sizeSquared();
    // The apex-based formula divides by |toApex|; guard the same singularity meshoptimizer's own
    // documentation notes (an eye at/near the apex) by ceding to "don't cull" instead.
    if (lenSq < 1e-8f) return false;

    const Vec3 dir = toApex * (1.0f / std::sqrt(lenSq));
    // third_party/meshoptimizer/src/meshoptimizer.h: dot(normalize(cone_apex - camera_position),
    // cone_axis) >= cone_cutoff  =>  every triangle in the cone faces away from `eye`: cull.
    return dot(dir, axis) >= c.coneCutoff;
}

bool inCut(const ClusterView& c, const std::vector<ClusterView>& clusters, f32 thresholdPx,
           const View& view) {
    const f32 ownErrorPx = screenSpaceErrorPx(c.sphereCenter, c.sphereRadius, c.error, view);
    // Too coarse on its own: something finer (this cluster's children, or -- at LOD 0, where error is
    // always 0.0 and this branch can never trigger -- nothing, since LOD 0 needs no finer fallback)
    // must stand in instead.
    if (ownErrorPx > thresholdPx) return false;

    // A coarser ancestor that ALSO already meets budget is cheaper and wins -- excluding `c` here is
    // what keeps exactly one cluster per root-to-leaf path selected, not one per level.
    for (u32 parentId : c.parents) {
        // An out-of-range parent id is treated as absent rather than as a hard error: this file's
        // contract is "clusters indexed by id" (see ClusterView's comment), but failing safe on a
        // malformed adapter output means AT WORST an extra cluster gets drawn alongside a valid
        // ancestor (overdraw), never a gap.
        if (parentId >= clusters.size()) continue;
        const ClusterView& parent = clusters[parentId];
        const f32 parentErrorPx =
            screenSpaceErrorPx(parent.sphereCenter, parent.sphereRadius, parent.error, view);
        if (parentErrorPx <= thresholdPx) return false;
    }
    return true;
}

bool selectCluster(const ClusterView& c, const std::vector<ClusterView>& clusters, f32 thresholdPx,
                    const View& view, const Frustum& frustum) {
    if (!frustum.intersectsSphere(c.sphereCenter, c.sphereRadius)) return false;
    if (coneCull(c, view.eye)) return false;
    return inCut(c, clusters, thresholdPx, view);
}

std::vector<u32> selectVisibleClusters(const std::vector<ClusterView>& clusters, f32 thresholdPx,
                                        const View& view, bool useFrustum) {
    // Frustum{} (all-zero planes) intersects everything -- see Frustum::intersectsSphere's comment --
    // so building one only when useFrustum is requested, and leaving it default otherwise, is enough;
    // no separate "skip the test" branch is needed in the loop below.
    const Frustum frustum = useFrustum ? Frustum::fromViewProj(view.viewProj) : Frustum{};

    std::vector<u32> selected;
    selected.reserve(clusters.size());
    for (const ClusterView& c : clusters)
        if (selectCluster(c, clusters, thresholdPx, view, frustum)) selected.push_back(c.id);
    return selected;
}

SelectionResult selectVisibleClustersWithStats(const std::vector<ClusterView>& clusters, f32 thresholdPx,
                                                const View& view, bool useFrustum) {
    const Frustum frustum = useFrustum ? Frustum::fromViewProj(view.viewProj) : Frustum{};

    SelectionResult result;
    result.drawnIds.reserve(clusters.size());

    // Full-detail baseline: every leaf (nothing feeds into it) at its own triangle count, regardless
    // of culling or LOD -- see the header comment on SelectionStats::trianglesBefore.
    for (const ClusterView& c : clusters)
        if (c.children.empty()) result.stats.trianglesBefore += c.triangleCount;

    // Same three tests as selectCluster, same order, but classifying into exactly one bucket instead
    // of collapsing to a bool -- this is why tested == frustumCulled+coneCulled+lodCollapsed+drawn
    // holds unconditionally: every loop iteration increments `tested` once and exactly one other
    // counter once, never zero, never two.
    for (const ClusterView& c : clusters) {
        ++result.stats.tested;
        if (!frustum.intersectsSphere(c.sphereCenter, c.sphereRadius)) {
            ++result.stats.frustumCulled;
            continue;
        }
        if (coneCull(c, view.eye)) {
            ++result.stats.coneCulled;
            continue;
        }
        if (!inCut(c, clusters, thresholdPx, view)) {
            ++result.stats.lodCollapsed;
            continue;
        }
        ++result.stats.drawn;
        result.stats.trianglesAfter += c.triangleCount;
        result.drawnIds.push_back(c.id);
    }
    return result;
}

} // namespace aver::trifactor
