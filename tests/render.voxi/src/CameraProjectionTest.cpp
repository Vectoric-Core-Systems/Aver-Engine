// CameraProjectionTest -- G4 R4: camera projections (aver/core/CameraProjection.hpp), render-layer masks and visibility ranges
// (aver/scene/RenderMasks.hpp), and the component fields that carry them. Headless and header-only: no device, no engine run.
#include "aver/core/CameraProjection.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/game/GameCamera.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/RenderMasks.hpp"
#include "aver/voxi/RasterLights.hpp"

#include <cmath>
#include <cstring>
#include <string>

using namespace aver;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool near(f64 a, f64 b, f64 tol = 1e-4) { return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b)); }

static bool sameMatrix(const Mat4& a, const Mat4& b, f64 tol = 1e-5) {
    for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) if (!near(a.m[i][j], b.m[i][j], tol)) return false;
    return true;
}

static void testMatrices() {
    AVER_INFO("=== projection matrices ===");
    // The default camera is perspectiveLH, bit for bit.
    {
        CameraProjection p;
        p.fovRad = radians(60.0f); p.nearCm = 2.0f; p.farCm = 200000.0f;
        const Mat4 a = projectionMatrix(p, 16.0f / 9.0f);
        const Mat4 b = Mat4::perspectiveLH(radians(60.0f), 16.0f / 9.0f, 2.0f, 200000.0f);
        check(std::memcmp(a.m, b.m, sizeof a.m) == 0, "the default projection is Mat4::perspectiveLH, byte for byte");
        check(p.isDefaultPerspective(), "a default CameraProjection reports itself as the old camera");
    }
    // A symmetric off-centre window is the symmetric matrix.
    {
        const f32 zn = 5.0f, zf = 900.0f, t = zn * std::tan(0.5f * radians(70.0f)), r = t * 1.5f;
        check(sameMatrix(perspectiveOffCenterLH(-r, r, -t, t, zn, zf), Mat4::perspectiveLH(radians(70.0f), 1.5f, zn, zf)), "a symmetric off-centre frustum equals perspectiveLH");
    }
    // Orthographic against hand values: window 16 x 9 cm, depth 1..101.
    {
        const Mat4 o = orthographicOffCenterLH(-8, 8, -4.5f, 4.5f, 1, 101);
        check(near(o.m[0][0], 0.125) && near(o.m[1][1], 1.0 / 4.5) && near(o.m[2][2], 0.01) && near(o.m[3][2], -0.01) && near(o.m[3][3], 1) && near(o.m[2][3], 0),
              "orthographic matrix entries match the D3D reference");
        f32 x, y, z;
        check(projectToNdc(o, Vec3{8, 4.5f, 101}, x, y, z) && near(x, 1) && near(y, 1) && near(z, 1), "the far top-right corner maps to (1, 1, 1)");
        check(projectToNdc(o, Vec3{-8, -4.5f, 1}, x, y, z) && near(x, -1) && near(y, -1) && near(z, 0), "the near bottom-left corner maps to (-1, -1, 0)");
        check(projectToNdc(o, Vec3{0, 0, 51}, x, y, z) && near(x, 0) && near(y, 0) && near(z, 0.5), "depth is linear");
    }
    // keep_aspect: ortho size 100 at aspect 2.
    {
        CameraProjection p;
        p.kind = ProjectionKind::Orthographic; p.sizeCm = 100.0f; p.nearCm = 1.0f; p.farCm = 101.0f;
        f32 hw, hh;
        projectionHalfExtents(p, 2.0f, hw, hh);
        check(near(hw, 100) && near(hh, 50), "keep height: size is the height, the width follows the aspect");
        p.keepWidth = true;
        projectionHalfExtents(p, 2.0f, hw, hh);
        check(near(hw, 50) && near(hh, 25), "keep width: size is the width, the height follows the aspect");
        const Mat4 m = projectionMatrix(p, 2.0f);
        check(near(m.m[0][0], 1.0 / 50) && near(m.m[1][1], 1.0 / 25), "the orthographic matrix uses the kept dimension");
    }
    // keep_aspect on perspective: a 90 degree HORIZONTAL fov at aspect 2 has a vertical tangent of 0.5.
    {
        CameraProjection p;
        p.fovRad = radians(90.0f); p.keepWidth = true; p.nearCm = 10.0f; p.farCm = 1000.0f;
        const Mat4 m = projectionMatrix(p, 2.0f);
        check(near(m.m[0][0], 1.0) && near(m.m[1][1], 2.0), "keep width: the horizontal fov sets x, the aspect sets y");
        check(sameMatrix(m, Mat4::perspectiveLH(2.0f * std::atan(0.5f), 2.0f, 10.0f, 1000.0f), 1e-4), "and it equals the vertical-fov matrix of the same view");
        CameraProjection q = p;
        q.keepWidth = false;
        const Mat4 mq = projectionMatrix(q, 2.0f);
        check(near(mq.m[1][1], 1.0) && near(mq.m[0][0], 0.5), "keep height: the vertical fov sets y");
    }
    // frustum with an offset: size 100 (height) at aspect 1, near 10, offset (20, -10) => window x -30..70, y -60..40 at the near plane.
    {
        CameraProjection p;
        p.kind = ProjectionKind::Frustum; p.sizeCm = 100.0f; p.offsetXCm = 20.0f; p.offsetYCm = -10.0f; p.nearCm = 10.0f; p.farCm = 1000.0f;
        const Mat4 m = projectionMatrix(p, 1.0f);
        f32 x, y, z;
        check(projectToNdc(m, Vec3{70, 40, 10}, x, y, z) && near(x, 1) && near(y, 1) && near(z, 0), "a frustum's top-right near corner maps to (1, 1, 0)");
        check(projectToNdc(m, Vec3{-30, -60, 10}, x, y, z) && near(x, -1) && near(y, -1), "its bottom-left near corner maps to (-1, -1)");
        check(projectToNdc(m, Vec3{700, 400, 100}, x, y, z) && near(x, 1) && near(y, 1) && near(z, (1000.0 / 990.0) * (1.0 - 10.0 / 100.0)), "the edge rays scale with depth, with the perspective depth law");
        check(projectToNdc(m, Vec3{20, -10, 10}, x, y, z) && near(x, 0, 1e-4) && near(y, 0, 1e-4), "the window centre at the near plane is (20, -10), the offset");
        check(projectToNdc(m, Vec3{0, 0, 10}, x, y, z) && near(x, -0.4) && near(y, 0.2), "the axis itself sits off-centre in the window");
        check(m.m[2][3] == 1.0f && m.m[3][3] == 0.0f, "a frustum is still a perspective divide");
    }
}

static void testViewProjRoundTrips() {
    AVER_INFO("=== view-projection round trips ===");
    const Vec3 eye{1200, -800, 340};
    const Vec3 fwd = Vec3{0.8f, 0.5f, -0.2f}.getSafeNormal();
    const Mat4 view = Mat4::lookAtDirLH(eye, fwd, Vec3{0, 0, 1});
    struct Case { const char* name; CameraProjection p; bool ortho; };
    Case cases[3];
    cases[0].name = "perspective"; cases[0].ortho = false; cases[0].p.fovRad = radians(70.0f); cases[0].p.nearCm = 5; cases[0].p.farCm = 50000;
    cases[1].name = "orthographic"; cases[1].ortho = true; cases[1].p.kind = ProjectionKind::Orthographic; cases[1].p.sizeCm = 600; cases[1].p.nearCm = 5; cases[1].p.farCm = 50000;
    cases[2].name = "frustum"; cases[2].ortho = false; cases[2].p.kind = ProjectionKind::Frustum; cases[2].p.sizeCm = 80; cases[2].p.offsetXCm = 15; cases[2].p.offsetYCm = -9;
    cases[2].p.nearCm = 10; cases[2].p.farCm = 50000;

    for (const Case& c : cases) {
        const Mat4 vp = view * projectionMatrix(c.p, 16.0f / 9.0f);
        const Mat4 inv = vp.inverse();
        check(isOrthographicViewProj(&vp.m[0][0]) == c.ortho, std::string(c.name) + ": isOrthographicViewProj says " + (c.ortho ? "yes" : "no"));
        // Ray through every NDC of a grid: the point on the near plane stays on the ray, and the ray is parallel (ortho) or through the eye.
        int bad = 0, parallelBad = 0, eyeBad = 0;
        Vec3 firstDir;
        bool haveFirst = false;
        for (int iy = -2; iy <= 2; ++iy) for (int ix = -2; ix <= 2; ++ix) {
            const f32 nx = ix * 0.45f, ny = iy * 0.45f;
            Vec3 o, d;
            if (!unprojectRay(inv, nx, ny, o, d)) { ++bad; continue; }
            // a point 1500 cm down the ray projects back to the same NDC
            f32 px, py, pz;
            if (!projectToNdc(vp, o + d * 1500.0f, px, py, pz) || std::fabs(px - nx) > 2e-3f || std::fabs(py - ny) > 2e-3f) ++bad;
            if (c.ortho) {
                if (!haveFirst) { firstDir = d; haveFirst = true; }
                else if (dot(firstDir, d) < 0.99999f) ++parallelBad;
            } else {
                // the line passes through the eye: |cross(d, eye - o)| = 0
                const Vec3 e = eye - o;
                const Vec3 cr{d.y * e.z - d.z * e.y, d.z * e.x - d.x * e.z, d.x * e.y - d.y * e.x};
                if (cr.size() > 0.5f) ++eyeBad;   // centimetres; the absolute f32 inverse is the imprecise one (see PerFrameCB::invViewProjRel)
            }
        }
        check(bad == 0, std::string(c.name) + ": unprojecting an NDC and projecting a point along the ray returns the same NDC");
        if (c.ortho) {
            check(parallelBad == 0 && dot(firstDir, fwd) > 0.99999f, "orthographic: every ray is parallel to the view axis");
        } else {
            check(eyeBad == 0, std::string(c.name) + ": every ray passes through the eye");
        }
        // the world point at the centre of the near plane is near-distance along the view axis from the eye (perspective, ortho) or off it (frustum)
        Vec3 o0, d0;
        unprojectRay(inv, 0, 0, o0, d0);
        const f32 depth = dot(o0 - eye, fwd);
        check(near(depth, c.p.nearCm, 1e-3), std::string(c.name) + ": the near plane is nearCm in front of the eye");
    }
    // A zeroed matrix is neither projection.
    f32 z[16] = {};
    check(!isOrthographicViewProj(z), "an unset (all-zero) view-projection is not called orthographic");
    // A camera far from the origin (the view's translation can make m[3][3] anything for a perspective camera).
    const Mat4 farView = Mat4::lookAtDirLH(Vec3{0, 0, 0}, Vec3{1, 0, 0}, Vec3{0, 0, 1}) * Mat4::perspectiveLH(1.0f, 1.0f, 1.0f, 100.0f);
    check(!isOrthographicViewProj(&farView.m[0][0]), "a perspective view-projection with the eye at the origin is not orthographic");
}

static void testMasks() {
    AVER_INFO("=== layer masks ===");
    using namespace aver::scene;
    check(storeMask(kAllLayers) == 0 && loadMask(0) == kAllLayers, "every layer is stored as zero (the zero-fill default)");
    check(storeMask(1) == 0xFFFFEu && loadMask(0xFFFFEu) == 1u, "layer 1 round-trips");
    check(storeMask(0) == kAllLayers && loadMask(kAllLayers) == 0u, "no layer at all is representable");
    check(storeMask(0xFFFFFFFFu) == 0, "bits above 20 are dropped");
    check(masksOverlap(0, 0), "an entity that never heard of layers is drawn by a camera that never heard of them");
    check(masksOverlap(0, storeMask(2)), "an unmasked entity (every layer) is drawn by a camera that renders layer 2 only");
    check(!masksOverlap(storeMask(1), storeMask(2)), "layer 1 is not drawn by a camera that renders layer 2 only (default layers 1 against cull mask 2)");
    check(masksOverlap(storeMask(1), storeMask(3)), "layer 1 is drawn by a camera that renders layers 1 and 2");
    check(!masksOverlap(storeMask(0), 0), "an instance on no layer is drawn by no camera");
    check(!masksOverlap(0, storeMask(0)), "a camera that renders no layer draws nothing");
    // The renderer's repeat of the rule agrees with the scene's on a sweep.
    int disagree = 0;
    for (u32 a = 0; a < 0x100000u; a += 4097u)
        for (u32 b = 0; b < 0x100000u; b += 2999u)
            if (voxi::rl::layersOverlap(a, b) != masksOverlap(a, b)) ++disagree;
    check(disagree == 0, "voxi::rl::layersOverlap agrees with scene::masksOverlap");
    // Components: the zero-filled struct means "everything, no range".
    CMeshRenderer zeroMr;
    std::memset(&zeroMr, 0, sizeof zeroMr);
    check(masksOverlap(zeroMr.layersInv, 0) && zeroMr.visBeginCm == 0.0f && zeroMr.visEndCm == 0.0f, "a zero-filled CMeshRenderer is on every layer with no visibility range");
    CCamera zeroCam;
    std::memset(&zeroCam, 0, sizeof zeroCam);
    check((zeroCam.flags & kCameraDrivesView) == 0 && zeroCam.projection == 0 && zeroCam.cullInv == 0, "a zero-filled CCamera does not drive the view and is perspective");
    check(sizeof(CMeshRenderer) % 8 == 0 && sizeof(CCamera) == 48, "component sizes: CMeshRenderer a multiple of 8, CCamera 48");
    // P5 / F32 / P20: the appended per-instance flags are zero-safe (a zero-filled renderer casts shadows, is static for GI, tests occlusion, adds no margin or bias).
    check(zeroMr.renderFlags == 0 && renderShadowMode(zeroMr.renderFlags) == 0 && renderGiMode(zeroMr.renderFlags) == 0 && zeroMr.cullMarginCm == 0.0f && zeroMr.lodBias == 0.0f &&
              zeroMr.fadeAmount == 0.0f,
          "a zero-filled CMeshRenderer is the ordinary mesh: casts shadows, static GI, no margin, no LOD bias, no fade");
    const u32 packed = kRenderShadowsOnly | (kRenderGiDisabled << kRenderGiShift) | kRenderIgnoreOcclusion;
    check(renderShadowMode(packed) == kRenderShadowsOnly && renderGiMode(packed) == kRenderGiDisabled && (packed & kRenderIgnoreOcclusion) != 0 && (packed & ~kRenderFlagsKnown) == 0,
          "the render flag fields pack and read back independently");
    check(renderShadowMode(kRenderShadowsOff) == kRenderShadowsOff && renderGiMode(kRenderShadowsOff) == 0, "a shadow mode leaves the GI mode alone");
}

static void testRangeFade() {
    AVER_INFO("=== visibility fade amount (P20) ===");
    using namespace aver::scene;
    check(rangeFadeAmount(50.0f, 0, 0, 0, 0) == 0.0f, "no range: never fades");
    check(rangeFadeAmount(600.0f, 0, 500, 0, 0) == 0.0f, "an end with no margin is a hard cut: no fade amount");
    check(rangeFadeAmount(499.0f, 0, 500, 0, 100) == 0.0f && rangeFadeAmount(500.0f, 0, 500, 0, 100) == 0.0f, "up to the end threshold: shown");
    check(rangeFadeAmount(550.0f, 0, 500, 0, 100) == 0.5f && rangeFadeAmount(600.0f, 0, 500, 0, 100) == 1.0f && rangeFadeAmount(900.0f, 0, 500, 0, 100) == 1.0f, "past the end it ramps over the margin and stays 1");
    check(rangeFadeAmount(95.0f, 100, 0, 10, 0) == 0.5f && rangeFadeAmount(100.0f, 100, 0, 10, 0) == 0.0f && rangeFadeAmount(50.0f, 100, 0, 10, 0) == 1.0f, "before the begin it ramps over the begin margin");
}

static void testRanges() {
    AVER_INFO("=== visibility ranges ===");
    using namespace aver::scene;
    u32 st = 0;
    check(rangeShown(1e6f, 0, 0, 0, 0, st) && st == 0, "no range: always shown, no state kept");
    // begin only
    st = 0;
    check(!rangeShown(99.0f, 100, 0, 0, 0, st), "begin 100: hidden at 99");
    check(rangeShown(100.0f, 100, 0, 0, 0, st), "shown at 100 (no margin)");
    check(rangeShown(1e6f, 100, 0, 0, 0, st), "no end: shown far away");
    // end only
    st = 0;
    check(rangeShown(499.0f, 0, 500, 0, 0, st) && !rangeShown(500.0f, 0, 500, 0, 0, st), "end 500: shown below, hidden at 500");
    // begin hysteresis (margin 10): a hidden instance appears at begin + margin, a shown one stays until begin - margin
    st = 0;
    check(!rangeShown(95.0f, 100, 0, 10, 0, st), "begin 100 margin 10: the first verdict is the plain threshold (hidden at 95)");
    check(!rangeShown(105.0f, 100, 0, 10, 0, st), "still hidden at 105 (it must cross 110)");
    check(rangeShown(111.0f, 100, 0, 10, 0, st), "shown at 111");
    check(rangeShown(95.0f, 100, 0, 10, 0, st), "stays shown at 95 (it must drop below 90)");
    check(!rangeShown(85.0f, 100, 0, 10, 0, st), "hidden at 85");
    // end hysteresis (margin 20)
    st = 0;
    check(rangeShown(490.0f, 0, 500, 0, 20, st), "end 500 margin 20: shown at 490");
    check(rangeShown(510.0f, 0, 500, 0, 20, st), "stays shown at 510 (it must pass 520)");
    check(!rangeShown(521.0f, 0, 500, 0, 20, st), "hidden at 521");
    check(!rangeShown(505.0f, 0, 500, 0, 20, st), "stays hidden at 505 (it must come back under 480)");
    check(rangeShown(479.0f, 0, 500, 0, 20, st), "shown again at 479");
    // the walk is evaluated twice a frame (depth prepass and colour): the second verdict must equal the first
    int unstable = 0;
    for (f32 d = 0.0f; d < 700.0f; d += 7.0f) {
        for (u32 start : {0u, kRangeKnown, kRangeKnown | kRangeHidden}) {
            u32 s1 = start;
            const bool a = rangeShown(d, 100, 500, 10, 20, s1);
            u32 s2 = s1;
            const bool b = rangeShown(d, 100, 500, 10, 20, s2);
            if (a != b || s1 != s2) ++unstable;
        }
    }
    check(unstable == 0, "evaluating the same distance twice (prepass then colour) gives the same verdict and state");
    // negative margins read as none
    st = 0;
    check(!rangeShown(95.0f, 100, 0, -5, 0, st) && rangeShown(100.0f, 100, 0, -5, 0, st), "a negative margin is no margin");
}

// Records what pushCamera hands the device, in order.
namespace {
struct RecordingDevice final : rhi::IDevice {
    f32 vp[16] = {}, fpVp[16] = {};
    int cameraCalls = 0, fpCalls = 0;
    bool fpAfterCamera = false;
    rhi::Backend backend() const override { return rhi::Backend::D3D12; }
    const char* adapterName() const override { return "recording"; }
    rhi::ISwapchain* createSwapchain(const rhi::SwapchainDesc&) override { return nullptr; }
    void beginFrame() override {}
    void endFrame() override {}
    void setCamera(const f32 viewProj[16], const f32[16], const f32[3]) override { std::memcpy(vp, viewProj, sizeof vp); ++cameraCalls; }
    void setFirstPersonCamera(const f32 m[16]) override { std::memcpy(fpVp, m, sizeof fpVp); fpAfterCamera = cameraCalls > 0; ++fpCalls; }
};
} // namespace

static void testFirstPerson() {
    AVER_INFO("=== first-person view-projection (docs/plans/FIRST_PERSON.md) ===");
    const Mat4 view = Mat4::lookAtDirLH(Vec3{100, -50, 30}, Vec3{1, 0, 0}, Vec3{0, 0, 1});
    CameraProjection mainLens;
    mainLens.fovRad = radians(60.0f); mainLens.nearCm = 2.0f; mainLens.farCm = 200000.0f;
    CameraProjection fpLens = mainLens;
    fpLens.fovRad = radians(70.0f); fpLens.farCm = 300.0f;
    const f32 aspect = 16.0f / 9.0f, slab = 0.01f;
    const Mat4 fp = firstPersonViewProj(view, fpLens, aspect, slab);
    const Mat4 mainVp = view * projectionMatrix(mainLens, aspect);
    // A point `d` cm along the view direction, slightly off axis (the view looks along +X from (100, -50, 30)).
    auto at = [](f32 d) { return Vec3{100.0f + d, -50.0f + d * 0.1f, 30.0f - d * 0.05f}; };
    bool inSlab = true;
    for (f32 d : {2.0f, 2.5f, 10.0f, 40.0f, 150.0f, 299.0f}) {
        f32 x, y, z;
        if (!projectToNdc(fp, at(d), x, y, z) || z < -1e-6f || z > slab) inSlab = false;
    }
    check(inSlab, "a point between the first-person near and far lands in z_ndc [0, slab]");
    f32 x, y, z;
    check(projectToNdc(fp, at(2.0f), x, y, z) && near(z, 0.0, 1e-5), "a point on the first-person near plane lands at z_ndc 0");
    check(projectToNdc(fp, at(299.9f), x, y, z) && z <= slab && z > slab * 0.99f, "a point at the first-person far plane lands at the back of the slab");
    // The slab sits in front of everything the main camera draws: from the near plane out, a main-camera point is behind it.
    bool behind = true;
    for (f32 d : {2.03f, 2.5f, 5.0f, 100.0f, 5000.0f}) {
        f32 mx, my, mz;
        if (!projectToNdc(mainVp, at(d), mx, my, mz) || !(mz > slab)) behind = false;
    }
    check(behind, "main-camera points from 2.03 cm out have z_ndc past the slab");
    // The slab does not touch x or y: the first-person lens's own projection decides the screen position.
    {
        const Mat4 full = view * projectionMatrix(fpLens, aspect);
        f32 ax, ay, az, bx, by, bz;
        check(projectToNdc(full, at(40.0f), ax, ay, az) && projectToNdc(fp, at(40.0f), bx, by, bz) && near(ax, bx) && near(ay, by),
              "squeezing the depth leaves the screen position alone");
        check(near(az * slab, bz, 1e-4), "z_ndc scales by exactly the slab");
    }
    // Lens equal to the main one with slab 1: the main view-projection, entry for entry. To a part in a million, not exactly: GCC may fuse
    // the multiply-adds of the two inlined products differently (-ffp-contract), which moves the last bit.
    {
        const Mat4 same = firstPersonViewProj(view, mainLens, aspect, 1.0f);
        bool equal = true;
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) equal = equal && near(same.m[r][c], mainVp.m[r][c], 1e-6);
        check(equal,
          "with the first-person lens equal to the main lens and slab 1, the matrix is the main view-projection");
    }
    // A slab that starts later: [zmin, zmin + slab].
    {
        const Mat4 s2 = firstPersonViewProj(view, fpLens, aspect, 0.1f, 0.2f);
        f32 a, b, c0, c1;
        check(projectToNdc(s2, at(2.0f), a, b, c0) && projectToNdc(s2, at(299.9f), a, b, c1) && near(c0, 0.2, 1e-4) && c1 > 0.2f && c1 <= 0.3f + 1e-4f,
              "a slab with zmin 0.2 occupies [0.2, 0.3]");
    }
    // Flags: the two new bits pack beside the old ones and are known.
    using namespace aver::scene;
    check(kRenderFirstPerson == 0x20u && kRenderFirstPersonCasts == 0x40u, "kRenderFirstPerson is 0x20 and kRenderFirstPersonCasts is 0x40");
    const u32 packed = kRenderShadowsOff | (kRenderGiDynamic << kRenderGiShift) | kRenderIgnoreOcclusion | kRenderFirstPerson | kRenderFirstPersonCasts;
    check((packed & ~kRenderFlagsKnown) == 0 && renderShadowMode(packed) == kRenderShadowsOff && renderGiMode(packed) == kRenderGiDynamic,
          "the first-person flags are in the known mask and leave the shadow and GI modes alone");
    check((kRenderFirstPerson & (kRenderShadowMask | kRenderGiMask | kRenderIgnoreOcclusion)) == 0 && (kRenderFirstPersonCasts & kRenderFirstPerson) == 0,
          "the first-person flags overlap no other flag");
    check((0x80u & ~kRenderFlagsKnown) != 0, "a bit above the first-person flags is still unknown");
    // PlayView defaults: the lens a held weapon draws through.
    const game::PlayView pv;
    check(pv.fpFovRad == 0.0f && pv.fpNearCm == 0.0f && near(pv.fpFarCm, 300.0f) && near(pv.fpDepthSlab, 0.01) && pv.firstPersonRoot == 0,
          "PlayView first-person defaults: the main FOV and near, far 300, slab 0.01, no root");
    check(near(pv.fpLens().fovRad, pv.lens().fovRad), "a zero fpFovRad uses the main FOV");
    {
        game::PlayView wide = pv;
        wide.fpFovRad = radians(90.0f);
        check(near(wide.fpLens().fovRad, radians(90.0f)), "a set fpFovRad is the first-person lens's own");
    }
    check(near(pv.fpLens().nearCm, pv.nearCm), "a zero fpNearCm uses the main near");
    // pushCamera sends the device the main camera first, then the first-person matrix built from the PlayView.
    {
        RecordingDevice dev;
        game::PlayView v;
        v.fpNearCm = 3.0f;
        const Vec3 pos{100, -50, 30}, fwd{1, 0, 0};
        const game::CameraMatrices m = game::pushCamera(dev, pos, fwd, aspect, v);
        const Mat4 want = firstPersonViewProj(Mat4::lookAtDirLH(pos, fwd, Vec3{0, 0, 1}), v.fpLens(), aspect, v.fpDepthSlab);
        Mat4 got{};
        std::memcpy(got.m, dev.fpVp, sizeof got.m);
        check(dev.cameraCalls == 1 && dev.fpCalls == 1 && dev.fpAfterCamera, "pushCamera calls setCamera, then setFirstPersonCamera once");
        check(std::memcmp(dev.vp, m.viewProj.m, sizeof dev.vp) == 0 && sameMatrix(got, want), "the matrices it sends are the main one and the first-person one");
        f32 ax, ay, az;
        check(projectToNdc(got, pos + fwd * 3.0f, ax, ay, az) && near(az, 0.0, 1e-5), "the PlayView's own fpNearCm puts its near plane at z_ndc 0");
    }
}

static void testObjectPad() {
    AVER_INFO("=== object block pad word: layers and the first-person bit ===");
    using namespace aver::rhi;
    check(packObjectPad(0, false) == 0u, "no layers and not first person is zero, so the block is written exactly as before");
    check(packObjectPad(0xABCDEu, false) == 0xABCDEu, "the layer bits pass through unchanged");
    check(packObjectPad(0xABCDEu, true) == (0xABCDEu | 0x80000000u), "the first-person bit sits above the layer bits and does not disturb them");
    check(packObjectPad(0, true) == 0x80000000u, "first person alone is bit 31");
    check(packObjectPad(0xFFFFFFFFu, false) == 0xFFFFFu, "a stray high bit in the layers cannot fake the first-person flag");
    check(((~packObjectPad(0x12345u, true)) & 0xFFFFFu) == ((~0x12345u) & 0xFFFFFu), "the raster light loop's 0xFFFFF mask sees only the layers");
}

static void testShaderText() {
    AVER_INFO("=== shader text ===");
    std::string hl;
    check(readFileText(std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders/voxi_locallights_raster.hlsli", hl), "voxi_locallights_raster.hlsli is readable");
    check(hl.find("asuint(_objPad)") != std::string::npos && hl.find("lightCullInv") != std::string::npos, "the raster light loop tests the instance layers against the light's cull mask");
    std::string pre;
    check(readFileText(std::string(AVER_REPO_ROOT) + "/modules/rhi/shaders/shared_prelude.hlsl", pre) && pre.find("float    _objPad;") != std::string::npos,
          "the per-object block still has the _objPad word the layers travel in");
    // First person: every scene vertex entry point routes its clip position through averScenePos; nothing in the prelude multiplies scene geometry by gViewProj.
    check(pre.find("float4 averScenePos(float4 wp)") != std::string::npos && pre.find("gFpViewProj") != std::string::npos && pre.find("0x80000000u") != std::string::npos,
          "the prelude defines averScenePos over gFpViewProj and bit 31 of _objPad");
    check(pre.find("mul(wp, gViewProj)") == pre.rfind("mul(wp, gViewProj)") &&
              pre.find("o.pos = averScenePos(wp)") != std::string::npos &&
              pre.find("ov.pos   = averScenePos(wp)") != std::string::npos,
          "no prelude vertex stage multiplies a scene vertex by gViewProj directly (only the helper does)");
    std::string voxi, emb;
    check(readFileText(std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders/voxi.hlsl", voxi) && voxi.find("o.pos = averScenePos(") != std::string::npos &&
              voxi.find("o.pos = mul(") == std::string::npos,
          "VSMainFx projects through averScenePos");
    check(readFileText(std::string(AVER_REPO_ROOT) + "/modules/render.pbr/shaders/material_embedded.hlsl", emb) && emb.find("o.pos = averScenePos(") != std::string::npos &&
              emb.find("o.pos = mul(") == std::string::npos,
          "the embedded vertex stage projects through averScenePos");
    // The pack: v[4][2] = cull mask, v[4][3] = shadow caster mask.
    voxi::SceneLight s;
    s.kind = 0; s.pos[2] = 200; s.intensityCd = 33333.0f; s.rangeCm = 1000.0f; s.castShadows = false;
    s.cullInv = scene::storeMask(2); s.shadowCasterInv = scene::storeMask(4);
    voxi::rl::Input in;
    in.lights = &s; in.count = 1; in.shadows = false;
    voxi::rl::Result r;
    voxi::rl::build(in, r);
    check(r.lightCount == 1 && static_cast<u32>(r.block.light[0][4][2]) == scene::storeMask(2) && static_cast<u32>(r.block.light[0][4][3]) == scene::storeMask(4),
          "rl::build packs the light and shadow-caster masks into the light record");
}

// The blended replay re-issues a draw long after drawMesh returned, when the sticky layers and first-person bit are gone: the record must carry the pad word.
static void testBlendedReplayKeepsThePad() {
    AVER_INFO("=== blended replay record ===");
    for (const char* file : {"/modules/rhi.d3d12/src/D3D12Device.cpp", "/modules/rhi.vulkan/src/VulkanDevice.cpp"}) {
        std::string src;
        check(readFileText(std::string(AVER_REPO_ROOT) + file, src), std::string(file) + " is readable");
        check(src.find("bd.objPad = packObjectPad(drawLayersInv_, drawFirstPerson_);") != std::string::npos,
              std::string(file) + ": drawMesh stores the layers and the first-person bit in the blended record");
        check(src.find("std::memcpy(fc + 27, &bd.objPad, sizeof(u32));") != std::string::npos,
              std::string(file) + ": the replay writes them back into the object block");
    }
    std::string vk;
    check(readFileText(std::string(AVER_REPO_ROOT) + "/modules/rhi.vulkan/src/VulkanCommon.hpp", vk) && vk.find("u32 objPad = 0;") != std::string::npos,
          "the Vulkan blended record has the pad word (D3D12's is declared beside its own)");
}

int main() {
    AVER_INFO("CameraProjectionTest (G4 R4)");
    testMatrices();
    testViewProjRoundTrips();
    testMasks();
    testRanges();
    testRangeFade();
    testFirstPerson();
    testBlendedReplayKeepsThePad();
    testObjectPad();
    testShaderText();
    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    if (g_failures) { AVER_ERROR("=== FAILED ==="); return 1; }
    AVER_INFO("=== ALL PASSED ===");
    return 0;
}
