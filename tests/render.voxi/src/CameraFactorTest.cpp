// CameraFactor -- recovering NRD's separate worldToView/viewToClip from the engine's combined
// viewProj, checked against real cameras built from the engine's own Mat4::lookAtLH/perspectiveLH.
//
// NO GPU, NO RHI: the property under test is pure arithmetic on a 4x4 matrix, exactly the shape of
// test this tree already keeps CPU-only (VoxiRtSeqTest is the sibling in this same directory). The
// header itself (aver/voxi/CameraFactor.hpp) depends on nothing but aver/core/Types.hpp; this test
// additionally links Aver.Core for Mat4/Vec3 to BUILD the cameras it feeds it, which the header
// itself deliberately does not depend on (see its own top comment).
//
// WHAT WOULD HAVE CAUGHT THE BUG THIS HEADER FIXES. Before CameraFactor existed,
// VoxiRenderer::beginShadowHistory handed NRD identity for worldToView and the FULL combined
// viewProj for viewToClip -- self-consistent as a product, but wrong the moment NRD decomposes
// viewToClip alone (see that function's own rewritten comment for the mechanism). A test that only
// checked "V * P == vp" could not have told the two encodings apart, because they were never
// unequal there. Sections 1-2 below check the RECOVERED V and P individually, elementwise, against
// the exact matrices lookAtLH/perspectiveLH produced -- the thing an identity-worldToView encoding
// would have failed instantly, had anything been asserting it.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"
#include "aver/voxi/CameraFactor.hpp"

#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace aver;
using aver::voxi::CameraFactor;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Every element of `vp` is `m.m[r][c]` at index r*4+c -- Mat4 is already exactly that flat layout
// (f32 m[4][4], row-major), so this is a reinterpretation, not a conversion.
const f32* flat(const Mat4& m) { return &m.m[0][0]; }

// Worst elementwise difference between two 16-float arrays, absolute-plus-relative against `b` --
// the same shape of tolerance CameraFactor::factor uses internally for its own recompose check, so
// a pass here is held to the same standard the header holds itself to.
f32 worstDiff(const f32 a[16], const f32 b[16]) {
    f32 worst = 0.0f;
    for (int i = 0; i < 16; ++i) {
        const f32 tol = 1e-3f + 1e-3f * std::fabs(b[i]);
        const f32 d = std::fabs(a[i] - b[i]);
        if (d > tol && d > worst) worst = d;
    }
    return worst;
}

// A camera built exactly the way the engine's own camera push does (game::pushCamera, the one copy
// both hosts call): lookAtDirLH(eye, direction, up) composed with perspectiveLH. `yawDeg`/`pitchDeg`
// describe the look direction in the engine's own +X-forward, +Z-up, left-handed frame (Math.hpp's banner
// comment): yaw rotates in the XY plane, pitch tilts toward +Z (up).
struct TestCamera {
    Mat4 view, proj, vp;
    Vec3 eye;
};

TestCamera buildCamera(Vec3 eye, f32 yawDeg, f32 pitchDeg, f32 fovYDeg, f32 aspect, f32 zn, f32 zf) {
    const f32 yaw = radians(yawDeg), pitch = radians(pitchDeg);
    const Vec3 dir{std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
    TestCamera c;
    c.eye  = eye;
    // lookAtDirLH, matching what pushCamera does. Built as lookAtLH(eye, eye + dir, ...) -- which
    // is what this test did, and what the engine did -- the far eye below absorbs a near-vertical
    // dir's horizontal components outright and the view matrix collapses, so four of these cameras
    // were never the cameras this sweep meant to build. See Mat4::lookAtDirLH's own comment.
    c.view = Mat4::lookAtDirLH(eye, dir, Vec3{0, 0, 1});
    c.proj = Mat4::perspectiveLH(radians(fovYDeg), aspect, zn, zf);
    c.vp   = c.view * c.proj;
    return c;
}

// Runs factor() against one camera and checks every property a real recovery must have: it
// succeeds, the recomposed product reproduces vp, and -- the check an "identity worldToView, full
// viewProj as viewToClip" encoding could never pass -- the two recovered matrices individually
// equal the ones the camera was actually built from.
void checkCamera(const TestCamera& c, const std::string& label) {
    f32 recV[16], recP[16];
    const bool ok = CameraFactor::factor(flat(c.vp), recV, recP);
    check(ok, label + ": factor() succeeds on a genuine lookAtLH*perspectiveLH product");
    if (!ok) return;

    Mat4 recVm{}, recPm{};
    std::memcpy(&recVm.m[0][0], recV, sizeof(recV));
    std::memcpy(&recPm.m[0][0], recP, sizeof(recP));
    const Mat4 recomposed = recVm * recPm;
    const f32 vpDiff = worstDiff(flat(recomposed), flat(c.vp));
    check(vpDiff == 0.0f, label + ": recomposed V*P reproduces vp within tolerance (worst " +
                          std::to_string(vpDiff) + ")");

    const f32 vDiff = worstDiff(recV, flat(c.view));
    check(vDiff == 0.0f, label + ": recovered worldToView equals the ORIGINAL lookAtLH matrix, "
                         "elementwise (worst " + std::to_string(vDiff) + ")");
    const f32 pDiff = worstDiff(recP, flat(c.proj));
    check(pDiff == 0.0f, label + ": recovered viewToClip equals the ORIGINAL perspectiveLH matrix, "
                         "elementwise (worst " + std::to_string(pDiff) + ")");
}

} // namespace

int main() {
    AVER_INFO("[CameraFactor] recovering worldToView/viewToClip from a combined viewProj");

    // ---- 1-2. a spread of genuine cameras, across eye position, look direction and lens ----
    //
    // EYES include the origin and large centimetre coordinates (the engine is centimetres; a scene
    // a few hundred metres across already puts the eye in the tens of thousands) -- this is exactly
    // where the translation recovery (tx = (vp[12] - cx*tz)/a) has the most to lose if a and cx are
    // even slightly off, since the error scales with the eye position itself.
    const Vec3 eyes[] = {
        {0, 0, 0},
        {5000, -3000, 800},
        {-42000, 17000, -900},
        {120, -80, 40},
    };
    // YAW/PITCH pairs: the four horizontal cardinal directions (yaw only, pitch 0 -- these are
    // "looking straight along a world axis" for the two axes that are never singular), a handful of
    // oblique angles, and NEAR the up/down pole (89.9 / -89.9 degrees) without ever reaching it --
    // AT the pole, up and the look direction coincide and lookAtLH's cross(up, f) is genuinely
    // undefined (Vec3::getSafeNormal returns zero), which is a real singularity, not a bug this
    // header could paper over. Near it is exactly the ill-conditioned case that would expose a
    // shortcut taken in single precision, which is why the class comment insists on double.
    struct YawPitch { f32 yaw, pitch; };
    const YawPitch dirs[] = {
        {0, 0}, {90, 0}, {180, 0}, {-90, 0},          // straight along +X/+Y/-X/-Y
        {37, 12}, {-124, -21}, {200, 5},               // oblique
        {15, 89.9f}, {-60, -89.9f},                    // near the pole, not at it
    };
    const struct { f32 fov, aspect; } lenses[] = {
        {60, 16.0f / 9.0f}, {90, 1.0f}, {35, 21.0f / 9.0f}, {100, 4.0f / 3.0f},
    };
    // The engine's own near/far (VoxiRenderer's callers): 2 cm and 200000 cm.
    constexpr f32 kZn = 2.0f, kZf = 200000.0f;

    u32 n = 0;
    for (const Vec3& eye : eyes)
        for (const YawPitch& yp : dirs)
            for (const auto& lens : lenses) {
                const TestCamera c = buildCamera(eye, yp.yaw, yp.pitch, lens.fov, lens.aspect, kZn, kZf);
                checkCamera(c, "camera " + std::to_string(n++));
            }
    AVER_INFO("[CameraFactor] checked {} cameras across {} eyes x {} directions x {} lenses",
              n, sizeof(eyes) / sizeof(eyes[0]), sizeof(dirs) / sizeof(dirs[0]),
              sizeof(lenses) / sizeof(lenses[0]));

    // ---- 3. rejection: an orthographic matrix ----
    // Identity has no perspective row at all (column 3, rows 0-2, is (0,0,0) exactly like a real
    // orthographic projection's would be -- P.m[2][3] is the ONLY source of a nonzero entry there,
    // and an orthographic P never sets it). |f| reads 0, nowhere near the required 1.
    {
        const Mat4 ortho = Mat4::identity();
        f32 recV[16], recP[16];
        check(!CameraFactor::factor(flat(ortho), recV, recP),
              "an orthographic (no perspective row) matrix is REFUSED, not misread as a degenerate perspective one");
    }

    // ---- 4. rejection: a mirrored camera ----
    // Takes a genuine view matrix and negates its `s` (right) row only -- u and f untouched. The
    // result is still orthonormal (each row is still unit length and mutually perpendicular), which
    // is exactly why orthogonality alone cannot catch a mirror: cross(s, u) now points opposite f,
    // and only the handedness check (factor()'s step 4) is built to notice that.
    {
        const TestCamera c = buildCamera({300, -150, 60}, 25, 10, 70, 16.0f / 9.0f, kZn, kZf);
        Mat4 mirroredView = c.view;
        for (int row = 0; row < 4; ++row) mirroredView.m[row][0] = -mirroredView.m[row][0];
        const Mat4 mirroredVp = mirroredView * c.proj;
        f32 recV[16], recP[16];
        check(!CameraFactor::factor(flat(mirroredVp), recV, recP),
              "a mirrored (reflected) camera is REFUSED rather than guessed at");
    }

    // ---- 5. rejection: non-finite input ----
    {
        const TestCamera c = buildCamera({0, 0, 0}, 0, 0, 60, 16.0f / 9.0f, kZn, kZf);
        f32 bad[16];
        std::memcpy(bad, flat(c.vp), sizeof(bad));
        bad[5] = std::numeric_limits<f32>::quiet_NaN();
        f32 recV[16], recP[16];
        check(!CameraFactor::factor(bad, recV, recP), "a NaN anywhere in the input is REFUSED");
        bad[5] = std::numeric_limits<f32>::infinity();
        check(!CameraFactor::factor(bad, recV, recP), "an infinity anywhere in the input is REFUSED");
    }

    // ---- 6. rejection: a uniformly scaled viewProj ----
    // Every element of a genuine vp scaled by the same k != 1 is still a real 4x4 that could be
    // *some* transform's matrix -- just not a lookAtLH*perspectiveLH product any more, since that
    // product's row-3/column-3 entry is pinned to exactly 1 (V's own translation row) and scaling
    // breaks that. |f| reads k instead of 1, which is precisely the check this trips.
    {
        const TestCamera c = buildCamera({10, 20, 30}, 45, -15, 60, 16.0f / 9.0f, kZn, kZf);
        f32 scaled[16];
        const f32* vp = flat(c.vp);
        for (int i = 0; i < 16; ++i) scaled[i] = vp[i] * 2.0f;
        f32 recV[16], recP[16];
        check(!CameraFactor::factor(scaled, recV, recP),
              "a uniformly rescaled viewProj (k=2) is REFUSED, not treated as a valid camera at the wrong scale");
    }

    if (g_failures == 0) {
        AVER_INFO("[CameraFactor] PASS: every genuine camera factorises back to its own worldToView "
                  "and viewToClip exactly, and every non-camera input is refused rather than guessed at");
        return 0;
    }
    AVER_ERROR("[CameraFactor] FAIL: {} check(s)", g_failures);
    return 1;
}
