// CameraProjectionTest -- G4 R4: camera projections (aver/core/CameraProjection.hpp), render-layer masks and visibility ranges
// (aver/scene/RenderMasks.hpp), and the component fields that carry them. Headless and header-only: no device, no engine run.
#include "aver/core/CameraProjection.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/platform/FileSystem.hpp"
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

static void testShaderText() {
    AVER_INFO("=== shader text ===");
    std::string hl;
    check(readFileText(std::string(AVER_REPO_ROOT) + "/modules/render.voxi/shaders/voxi_locallights_raster.hlsli", hl), "voxi_locallights_raster.hlsli is readable");
    check(hl.find("asuint(_objPad)") != std::string::npos && hl.find("lightCullInv") != std::string::npos, "the raster light loop tests the instance layers against the light's cull mask");
    std::string pre;
    check(readFileText(std::string(AVER_REPO_ROOT) + "/modules/rhi/shaders/shared_prelude.hlsl", pre) && pre.find("float    _objPad;") != std::string::npos,
          "the per-object block still has the _objPad word the layers travel in");
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

int main() {
    AVER_INFO("CameraProjectionTest (G4 R4)");
    testMatrices();
    testViewProjRoundTrips();
    testMasks();
    testRanges();
    testShaderText();
    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    if (g_failures) { AVER_ERROR("=== FAILED ==="); return 1; }
    AVER_INFO("=== ALL PASSED ===");
    return 0;
}
