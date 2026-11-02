// Parity E6: where a world label ends up for a camera (billboards, constant screen size, depth, placement), with a camera built
// the way the engine's is (row-major, row vectors, left-handed). Pure maths: no device.
#include "TestCamera.hpp"

#include "aver/core/Log.hpp"
#include "aver/render/ui/WorldTextPlace.hpp"

#include <cmath>
#include <string>

using namespace aver;
using aver::render::ui::WorldTextCamera;
using testcam::makeCamera;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(f32 a, f32 b, f32 eps = 1.0e-3f) { return std::fabs(a - b) <= eps; }

static f32 len3(const f32* v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
static f32 dot3(const f32* a, const f32* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

int main() {
    const f32 eye[3] = {0, 0, 0}, target[3] = {10, 0, 0}, up[3] = {0, 0, 1};
    const f32 fov = 1.0f;   // radians
    const WorldTextCamera cam = makeCamera(eye, target, up, fov, 16.0f / 9.0f, 720);

    AVER_INFO("=== the camera's axes ===");
    {
        f32 r[3], u[3], f[3];
        check(render::ui::worldTextCameraAxes(cam, r, u, f), "axes are recovered from invViewProjRel");
        check(near(f[0], 1) && near(f[1], 0) && near(f[2], 0), "forward is +x, where the camera looks");
        check(near(u[2], 1) && near(std::fabs(r[1]), 1), "up is +z and right lies along y");
        WorldTextCamera bad;
        f32 a[3], b[3], c[3];
        check(!render::ui::worldTextCameraAxes(bad, a, b, c), "a zero camera is degenerate, not a crash");
    }

    AVER_INFO("=== pixel size ===");
    {
        const f32 p10[3] = {10, 0, 0}, p20[3] = {20, 0, 0}, behind[3] = {-5, 0, 0};
        const f32 expect10 = 2.0f * 10.0f * std::tan(fov * 0.5f) / 720.0f;
        check(near(render::ui::worldTextUnitsPerPixel(cam, p10), expect10, expect10 * 0.01f), "at 10 m a pixel spans 2 d tan(fov/2) / height");
        check(near(render::ui::worldTextUnitsPerPixel(cam, p20), expect10 * 2.0f, expect10 * 0.02f), "and twice that at 20 m");
        check(render::ui::worldTextUnitsPerPixel(cam, behind) == 0.0f, "a point behind the camera has no pixel size");
        WorldTextCamera noVp = cam; noVp.viewportHeight = 0;
        check(render::ui::worldTextUnitsPerPixel(noVp, p10) == 0.0f, "nor does an empty viewport");
        const f32 w10[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 10, 0, 0, 1};
        check(near(render::ui::worldTextDepth(w10, cam), 10.0f, 0.01f), "depth is the distance along the view");
    }

    AVER_INFO("=== a plain label is left alone ===");
    {
        aver::ui::UiWorldText t;
        t.world[12] = 10; t.world[0] = 2;
        f32 m[16];
        check(render::ui::worldTextMatrix(t, cam, m), "placed");
        bool same = true;
        for (int i = 0; i < 16; ++i) same = same && m[i] == t.world[i];
        check(same, "its matrix is exactly its own");
        // Even behind the camera: a plain label is not culled here, the GPU clips it.
        t.world[12] = -10;
        check(render::ui::worldTextMatrix(t, cam, m), "a plain label behind the camera is still placed (clipping is the GPU's)");
    }

    AVER_INFO("=== billboards ===");
    {
        aver::ui::UiWorldText t;
        t.flags = aver::ui::kWorldTextBillboard;
        // Turned and scaled: 3 on x, 2 on y, a quarter turn about z.
        const f32 w[16] = {0, 3, 0, 0, -2, 0, 0, 0, 0, 0, 1, 0, 10, 4, 1, 1};
        for (int i = 0; i < 16; ++i) t.world[i] = w[i];
        f32 m[16], r[3], u[3], f[3];
        render::ui::worldTextCameraAxes(cam, r, u, f);
        check(render::ui::worldTextMatrix(t, cam, m), "a billboard in front of the camera is placed");
        check(near(len3(m), 3.0f) && near(len3(m + 4), 2.0f), "it keeps its own scale (3 x 2)");
        check(near(dot3(m, r), 3.0f) && near(dot3(m + 4, u), 2.0f), "its x runs along the camera's right and its y along the camera's up");
        check(near(dot3(m + 8, f), -1.0f), "and its plane faces the camera");
        check(m[12] == 10 && m[13] == 4 && m[14] == 1, "the origin does not move");
        t.world[12] = -10;
        check(!render::ui::worldTextMatrix(t, cam, m), "a billboard behind the camera is skipped for the frame");
        t.world[12] = 10;

        // About the up axis only.
        t.flags = aver::ui::kWorldTextBillboardY;
        const f32 wy[16] = {1, 0, 0, 0, 0, 0, 1, 0, 0, 1, 0, 0, 10, 4, 1, 1};   // its y axis is the world's +z
        for (int i = 0; i < 16; ++i) t.world[i] = wy[i];
        check(render::ui::worldTextMatrix(t, cam, m), "a Y billboard is placed");
        check(near(m[4], 0) && near(m[5], 0) && near(m[6], 1), "its up axis is kept");
        check(near(dot3(m, m + 4), 0.0f) && near(dot3(m, r), 1.0f), "its x is the camera's right with the up component removed");
        t.flags = aver::ui::kWorldTextBillboard;
        for (int i = 0; i < 16; ++i) t.world[i] = 0;
        t.world[15] = 1; t.world[12] = 10;
        f32 mz[16];
        render::ui::worldTextMatrix(t, cam, mz);
        check(mz[0] == 0 && mz[5] == 0, "a label with no scale stays with no scale");
    }

    AVER_INFO("=== constant screen size ===");
    {
        aver::ui::UiWorldText t;
        t.fixedPxPerUnit = 0.5f;          // a plane unit is half a pixel
        t.world[0] = t.world[5] = t.world[10] = 1;
        t.world[15] = 1;
        t.world[12] = 10;
        f32 m[16];
        const f32 p10[3] = {10, 0, 0}, p30[3] = {30, 0, 0};
        const f32 upp10 = render::ui::worldTextUnitsPerPixel(cam, p10), upp30 = render::ui::worldTextUnitsPerPixel(cam, p30);
        check(render::ui::worldTextMatrix(t, cam, m), "placed");
        check(near(len3(m), upp10 * 0.5f, upp10 * 0.01f), "at 10 m a plane unit spans half a pixel's world width");
        t.world[12] = 30;
        render::ui::worldTextMatrix(t, cam, m);
        check(near(len3(m), upp30 * 0.5f, upp30 * 0.01f) && upp30 > upp10 * 2.5f, "and it grows with distance, so the screen size is constant");
        t.world[0] = 4;
        render::ui::worldTextMatrix(t, cam, m);
        check(near(len3(m), upp30 * 0.5f * 4.0f, upp30 * 0.05f), "the node's own scale multiplies");
        WorldTextCamera noVp = cam; noVp.viewportHeight = 0;
        check(!render::ui::worldTextMatrix(t, noVp, m), "with no viewport height a fixed-size label is skipped, not drawn at a guess");
        t.world[12] = -3;
        check(!render::ui::worldTextMatrix(t, cam, m), "and so is one behind the camera");
    }

    AVER_INFO("=== placing vertices ===");
    {
        const aver::ui::UiVertex in[2] = {{1, 2, 0.25f, 0.5f, 0xFF112233u}, {-1, 0, 0, 1, 0x80FFFFFFu}};
        render::ui::WorldTextVertex out[2];
        const f32 m[16] = {0, 1, 0, 0, -1, 0, 0, 0, 0, 0, 1, 0, 5, 6, 7, 1};   // a quarter turn about z, moved
        render::ui::worldTextPlace(in, 2, m, out);
        check(near(out[0].x, 5 - 2) && near(out[0].y, 6 + 1) && near(out[0].z, 7), "(1, 2) -> x*row0 + y*row1 + origin");
        check(out[0].u == 0.25f && out[0].v == 0.5f && out[0].rgba == 0xFF112233u, "uv and colour pass through");
        check(near(out[1].x, 5 + 0) && near(out[1].y, 6 - 1), "the second vertex too");
    }

    if (g_failures == 0) AVER_INFO("=== all world text placement tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
