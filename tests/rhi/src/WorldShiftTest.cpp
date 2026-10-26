// WorldShiftTest -- the renderer's floating-origin math (rhi/WorldShift.hpp), with no device.
//
// LARGE_WORLD 2.2.6: when the scene origin moves every local position changes by -delta, so the
// previous-frame matrix is composed with a translation, the wave phase and the cloud offset take an
// f64-reduced origin term, and PerFrameCB grows two rows. Each property below is one no screenshot can
// decide: the row-vector convention, f32 rounding of a large translation row, a phase that must stay
// continuous across 2*pi, and "identity at origin zero" as a bit-level statement.
//
// Links Aver.RHI, Aver.Core and Aver.Platform like AtmosphereTest and nothing else.
#include "aver/core/Log.hpp"
#include "aver/rhi/FrameConstants.hpp"
#include "aver/rhi/WorldShift.hpp"

// The CPU twin of the wave phase lives in the fluids module's header-only math (Core only). It is reached by a path from this
// file, not a target include directory, so the test keeps linking Aver.RHI and Aver.Core alone; the agreement section below
// is skipped, loudly, if the header is not where it was.
#if __has_include("../../../modules/fluids/include/aver/fluids/GerstnerWave.hpp")
#include "../../../modules/fluids/include/aver/fluids/GerstnerWave.hpp"
#define WORLDSHIFT_HAS_GERSTNER 1
#endif

#include <cmath>
#include <cstddef>
#include <cstring>
#include <random>
#include <string>

using namespace aver;
using namespace aver::rhi;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace {

constexpr f64 kStep = 1'638'400.0;   // one region, the only legal delta unit
constexpr f64 kPi = 3.14159265358979323846;

struct M64 { f64 m[4][4]; };

M64 mul(const M64& a, const M64& b) {
    M64 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            f64 s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

M64 translation64(f64 x, f64 y, f64 z) {
    M64 r{};
    for (int i = 0; i < 4; ++i) r.m[i][i] = 1.0;
    r.m[3][0] = x; r.m[3][1] = y; r.m[3][2] = z;
    return r;
}

// Mat4::lookAtDirLH * Mat4::perspectiveLH in f64: row-vector, translation in row 3.
M64 viewProj64(const f64 eye[3], f64 yaw, f64 pitch, f64 fovY, f64 aspect, f64 zn, f64 zf) {
    const f64 d[3] = {std::cos(pitch) * std::cos(yaw), std::cos(pitch) * std::sin(yaw), std::sin(pitch)};
    const f64 up[3] = {0, 0, 1};
    auto cross = [](const f64 a[3], const f64 b[3], f64 o[3]) {
        o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
    };
    auto norm = [](f64 v[3]) { const f64 l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); v[0] /= l; v[1] /= l; v[2] /= l; };
    auto dotp = [](const f64 a[3], const f64 b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    f64 f[3] = {d[0], d[1], d[2]}, s[3], u[3];
    norm(f);
    cross(up, f, s); norm(s);
    cross(f, s, u);
    M64 v{};
    for (int i = 0; i < 3; ++i) { v.m[i][0] = s[i]; v.m[i][1] = u[i]; v.m[i][2] = f[i]; }
    v.m[3][0] = -dotp(s, eye); v.m[3][1] = -dotp(u, eye); v.m[3][2] = -dotp(f, eye); v.m[3][3] = 1.0;
    const f64 ys = 1.0 / std::tan(fovY * 0.5), xs = ys / aspect;
    M64 p{};
    p.m[0][0] = xs; p.m[1][1] = ys;
    p.m[2][2] = zf / (zf - zn); p.m[2][3] = 1.0;
    p.m[3][2] = -zn * zf / (zf - zn);
    return mul(v, p);
}

void toF32(const M64& a, f32 out[16]) {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<f32>(a.m[i][j]);
}

f64 circularDiff(f64 a, f64 b) {
    f64 d = std::fmod(a - b, 2.0 * kPi);
    if (d > kPi) d -= 2.0 * kPi;
    if (d < -kPi) d += 2.0 * kPi;
    return std::fabs(d);
}

f64 ulpOf(f64 v) {
    const f32 a = static_cast<f32>(std::fabs(v));
    return static_cast<f64>(std::nextafter(a, 3.4e38f)) - static_cast<f64>(a);
}

}   // namespace

// ---- the row-vector convention, pinned by hand --------------------------------------------------
static void testHandComputed() {
    AVER_INFO("-- composePrevViewProj: two hand-computed cases --");
    // Identity: a new-frame point (1,1,1) moves to the old frame (6,7,8) under delta (5,6,7).
    f32 id[16] = {1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1};
    auto a = composePrevViewProj(id, ShiftVec3d{5, 6, 7});
    check(a[12] == 5 && a[13] == 6 && a[14] == 7 && a[15] == 1, "identity: row 3 becomes (5,6,7,1)");
    bool rowsKept = true;
    for (int i = 0; i < 12; ++i) rowsKept = rowsKept && a[i] == id[i];
    check(rowsKept, "identity: rows 0..2 are untouched");

    // clip = (2x + 10, 3y + 20, 4z + 30, 1). Origin (0,0,0) of the new frame is (1,2,3) of the old one,
    // which projects to (2+10, 6+20, 12+30) = (12, 26, 42).
    f32 m[16] = {2, 0, 0, 0,  0, 3, 0, 0,  0, 0, 4, 0,  10, 20, 30, 1};
    auto b = composePrevViewProj(m, ShiftVec3d{1, 2, 3});
    check(b[12] == 12 && b[13] == 26 && b[14] == 42 && b[15] == 1, "scale + translation: row 3 is (12,26,42,1)");

    // Negative delta, and the in-place form.
    f32 c[16];
    std::memcpy(c, m, sizeof(c));
    composePrevViewProjInPlace(c, ShiftVec3d{-1, -2, -3});
    check(c[12] == 8 && c[13] == 14 && c[14] == 18, "a negative delta subtracts delta . rows");
}

// ---- identity -----------------------------------------------------------------------------------
static void testIdentity() {
    AVER_INFO("-- zero delta / zero origin is the identity, bit for bit --");
    std::mt19937_64 rng(7);
    std::uniform_real_distribution<f64> u(-1e6, 1e6);
    bool same = true;
    for (int n = 0; n < 200; ++n) {
        f32 m[16];
        for (f32& v : m) v = static_cast<f32>(u(rng));
        const auto o = composePrevViewProj(m, ShiftVec3d{});
        same = same && std::memcmp(o.data(), m, sizeof(m)) == 0;
    }
    check(same, "composePrevViewProj with a zero delta returns the input unchanged");

    f32 row[4] = {1, 2, 3, 4};
    worldOriginRow(ShiftOrigin{}, 0, row);
    check(row[0] == 0 && row[1] == 0 && row[2] == 0 && row[3] == 0, "worldOriginRow is zero at origin zero, generation zero");

    PerFrameCB cb{};
    wavePhaseRow(cb.wave, ShiftOrigin{}, cb.wavePhase);
    f32 zero[8] = {};
    check(std::memcmp(cb.worldOrigin, zero, sizeof(cb.worldOrigin)) == 0, "a value-initialised PerFrameCB has a zero worldOrigin row");
    check(std::memcmp(cb.wavePhase, zero, sizeof(cb.wavePhase)) == 0, "wavePhaseRow with no live wave and origin zero leaves zeros");

    // Live waves, origin zero: every offset is exactly +0.0, never -0.0 (a negative direction times a zero origin).
    const f32 waves[3][4] = {{0.6f, 0.8f, 0.01f, 1.0f}, {-0.6f, 0.8f, 0.02f, 1.0f}, {-1.0f, -0.0f, 0.05f, 1.0f}};
    f32 ph[4] = {9, 9, 9, 9};
    wavePhaseRow(waves, ShiftOrigin{}, ph);
    bool zeros = true;
    for (int i = 0; i < 4; ++i) zeros = zeros && std::memcmp(&ph[i], &zero[0], sizeof(f32)) == 0;
    check(zeros, "live waves at origin zero give bit-exact +0.0 offsets");

    // Cloud fold: the same f32 back, including a negative zero.
    bool foldSame = true;
    for (int n = 0; n < 200; ++n) {
        const f32 wx = static_cast<f32>(u(rng) * 1e-2), wy = static_cast<f32>(u(rng) * 1e-2);
        const auto f = cloudMotionFold(ShiftVec3d{wx, wy, 0}, ShiftOrigin{});
        foldSame = foldSame && std::memcmp(&f.x, &wx, sizeof(f32)) == 0 && std::memcmp(&f.y, &wy, sizeof(f32)) == 0;
    }
    const auto nz = cloudMotionFold(ShiftVec3d{-0.0, -0.0, 0}, ShiftOrigin{});
    f32 negZero = -0.0f;
    check(foldSame && std::memcmp(&nz.x, &negZero, sizeof(f32)) == 0, "cloudMotionFold at origin zero returns the wind offset bit for bit");
}

// ---- composition against independent f64 references ----------------------------------------------
static void testComposeAgainstReferences() {
    AVER_INFO("-- composePrevViewProj: T(+delta)*M in f64, and the matrix rebuilt at eye - delta --");
    std::mt19937_64 rng(1234);
    std::uniform_real_distribution<f64> u01(0.0, 1.0);
    const int kCameras = 400;
    int exactFailures = 0, rebuildFailures = 0, projectFailures = 0;
    for (int n = 0; n < kCameras; ++n) {
        // A camera that just crossed the rebase trigger: 0.92..1.2e6 cm out on a shifted axis.
        const f64 sx = (u01(rng) < 0.5 ? -1.0 : 1.0), sy = (u01(rng) < 0.5 ? -1.0 : 1.0);
        const bool shiftX = u01(rng) < 0.7, shiftY = !shiftX || u01(rng) < 0.5;
        f64 eye[3] = {sx * (shiftX ? 921'601.0 + u01(rng) * 280'000.0 : u01(rng) * 600'000.0),
                      sy * (shiftY ? 921'601.0 + u01(rng) * 280'000.0 : u01(rng) * 600'000.0),
                      (u01(rng) - 0.5) * 10'000.0};
        const f64 dx = shiftX ? sx * kStep : 0.0, dy = shiftY ? sy * kStep : 0.0;
        const f64 eyeNew[3] = {eye[0] - dx, eye[1] - dy, eye[2]};
        const f64 yaw = u01(rng) * 2.0 * kPi, pitch = (u01(rng) - 0.5) * 2.4;
        const M64 vpOld = viewProj64(eye, yaw, pitch, kPi / 3.0, 16.0 / 9.0, 2.0, 1.0e6);
        const M64 vpNew = viewProj64(eyeNew, yaw, pitch, kPi / 3.0, 16.0 / 9.0, 2.0, 1.0e6);

        f32 in[16];
        toF32(vpOld, in);
        const auto out = composePrevViewProj(in, ShiftVec3d{dx, dy, 0.0});

        // (1) Against T * M in f64 with the stored f32 entries: equal after one f32 rounding.
        M64 m32{};
        for (int i = 0; i < 4; ++i) for (int j = 0; j < 4; ++j) m32.m[i][j] = static_cast<f64>(in[i * 4 + j]);
        const M64 ref = mul(translation64(dx, dy, 0.0), m32);
        bool exact = true;
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) exact = exact && std::fabs(static_cast<f64>(out[i * 4 + j]) - ref.m[i][j]) <= ulpOf(ref.m[i][j]);
        if (!exact) ++exactFailures;

        // (2) Against the matrix rebuilt at eye - delta. The only differences are the f32 rounding of the
        // INPUT entries, so the row-3 tolerance is relative to the terms that were summed.
        bool rebuilt = true;
        for (int j = 0; j < 4; ++j) {
            for (int i = 0; i < 3; ++i)
                rebuilt = rebuilt && std::fabs(static_cast<f64>(out[i * 4 + j]) - vpNew.m[i][j]) <= 1.5e-7 * std::fabs(vpNew.m[i][j]) + 1e-12;
            const f64 terms = std::fabs(static_cast<f64>(in[12 + j])) + std::fabs(dx * in[j]) + std::fabs(dy * in[4 + j]);
            rebuilt = rebuilt && std::fabs(static_cast<f64>(out[12 + j]) - vpNew.m[3][j]) <= 2.0e-7 * terms + 1.5e-7 * std::fabs(vpNew.m[3][j]) + 1e-12;
        }
        if (!rebuilt) ++rebuildFailures;

        // (3) Projecting: a point near the new camera (new frame) lands where its old-frame twin did.
        bool projected = true;
        for (int k = 0; k < 4; ++k) {
            f64 pNew[3] = {(u01(rng) - 0.5) * 20'000.0, (u01(rng) - 0.5) * 20'000.0, (u01(rng) - 0.5) * 2'000.0};
            f64 pOld[3] = {pNew[0] + dx, pNew[1] + dy, pNew[2]};
            for (int c = 0; c < 4; ++c) {
                f64 clipOld = in[12 + c], clipNew = out[12 + c], scale = std::fabs(static_cast<f64>(out[12 + c]));
                for (int r = 0; r < 3; ++r) {
                    clipOld += pOld[r] * in[r * 4 + c];
                    clipNew += pNew[r] * out[r * 4 + c];
                    scale += std::fabs(pNew[r] * out[r * 4 + c]);
                }
                projected = projected && std::fabs(clipOld - clipNew) <= 2.0e-7 * scale + 1e-8;
            }
        }
        if (!projected) ++projectFailures;
    }
    check(exactFailures == 0, "matches the f64 product T(+delta)*M to one f32 rounding for " + std::to_string(kCameras) + " cameras");
    check(rebuildFailures == 0, "matches the matrix rebuilt at eye - delta within the input's own rounding");
    check(projectFailures == 0, "a point projects to the same clip position before (old frame) and after (new frame, shifted matrix)");
}

// ---- waves --------------------------------------------------------------------------------------
static void testWavePhase() {
    AVER_INFO("-- wavePhaseOffset: phase continuity across a rebase --");
    std::mt19937_64 rng(99);
    std::uniform_real_distribution<f64> u01(0.0, 1.0);
    int bad = 0;
    f64 worst = 0;
    const int kPoints = 100'000;
    for (int n = 0; n < kPoints; ++n) {
        const f32 a = static_cast<f32>(u01(rng) * 2.0 * kPi);
        const f32 dirX = std::cos(a), dirY = std::sin(a);
        const f32 k = static_cast<f32>(2.0 * kPi / (50.0 + u01(rng) * 4000.0));   // wavelength 0.5 m .. 40 m
        const i64 steps = static_cast<i64>(u01(rng) * 12.0) - 6;
        const ShiftOrigin o{static_cast<i64>(kStep) * steps, static_cast<i64>(kStep) * static_cast<i64>(u01(rng) * 12.0 - 6.0)};
        const f64 lx = (u01(rng) - 0.5) * 2.0 * 921'600.0, ly = (u01(rng) - 0.5) * 2.0 * 921'600.0;   // local position
        const f64 absPhase = static_cast<f64>(k) * (static_cast<f64>(dirX) * (lx + static_cast<f64>(o.x)) + static_cast<f64>(dirY) * (ly + static_cast<f64>(o.y)));
        const f32 off = wavePhaseOffset(ShiftVec3d{static_cast<f64>(dirX), static_cast<f64>(dirY), 0}, static_cast<f64>(k), o);
        const f64 newPhase = static_cast<f64>(k) * (static_cast<f64>(dirX) * lx + static_cast<f64>(dirY) * ly) + static_cast<f64>(off);
        const f64 d = circularDiff(absPhase, newPhase);
        worst = std::fmax(worst, d);
        if (d > 1e-3 || off < 0.0f || off > 6.2832f) ++bad;
    }
    AVER_INFO("  worst phase error {:.3e} rad over {} points", worst, kPoints);
    check(bad == 0, "phase at the absolute position equals the local phase + offset mod 2*pi (1e-3 rad), offset in [0, 2*pi]");

    // The offset is periodic in the origin: a whole number of wavelengths along the direction gives zero.
    const f64 lambda = 1'638'400.0 / 8.0;   // a wavelength that divides the step exactly
    const f32 off = wavePhaseOffset(ShiftVec3d{1.0, 0.0, 0.0}, 2.0 * kPi / lambda, ShiftOrigin{static_cast<i64>(kStep) * 3, 0});
    check(off < 1e-4f || off > 6.2831f, "an origin that is a whole number of wavelengths adds no phase");

    // wavePhaseRow reads the f32 wave table PerFrameCB carries and leaves dead waves at zero.
    PerFrameCB cb{};
    cb.wave[0][0] = 1.0f; cb.wave[0][1] = 0.0f; cb.wave[0][2] = 0.00123f; cb.wave[0][3] = 1.0f;
    cb.wave[1][0] = 0.0f; cb.wave[1][1] = 1.0f; cb.wave[1][2] = 0.00456f; cb.wave[1][3] = 1.0f;
    const ShiftOrigin o{static_cast<i64>(kStep) * 2, static_cast<i64>(kStep) * -3};
    wavePhaseRow(cb.wave, o, cb.wavePhase);
    const f32 w0 = wavePhaseOffset(ShiftVec3d{1.0, 0.0, 0}, static_cast<f64>(cb.wave[0][2]), o);
    const f32 w1 = wavePhaseOffset(ShiftVec3d{0.0, 1.0, 0}, static_cast<f64>(cb.wave[1][2]), o);
    check(cb.wavePhase[0] == w0 && cb.wavePhase[1] == w1 && cb.wavePhase[2] == 0.0f && cb.wavePhase[3] == 0.0f,
          "wavePhaseRow fills waves 0 and 1 and leaves the dead wave 2 and w at zero");
}

// ---- clouds -------------------------------------------------------------------------------------
static void testCloudFold() {
    AVER_INFO("-- cloudMotionFold: wpos_local + fold equals wpos_abs + wind --");
    std::mt19937_64 rng(5);
    std::uniform_real_distribution<f64> u01(0.0, 1.0);
    int bad = 0;
    const int kPoints = 100'000;
    for (int n = 0; n < kPoints; ++n) {
        const f32 wind = static_cast<f32>((u01(rng) - 0.5) * 8000.0);   // wind * time, plus the per-seed hash offset
        const i64 steps = (u01(rng) < 0.5 ? -1 : 1) * (1 + static_cast<i64>(u01(rng) * 60.0));   // 16 .. 1000 km out
        const ShiftOrigin o{static_cast<i64>(kStep) * steps, 0};
        const f32 local = static_cast<f32>((u01(rng) - 0.5) * 2.0 * 921'600.0);
        const auto fold = cloudMotionFold(ShiftVec3d{static_cast<f64>(wind), 0, 0}, o);
        const f32 sumNew = local + fold.x;                                                // what the shader adds
        const f64 sumRef = static_cast<f64>(local) + static_cast<f64>(o.x) + static_cast<f64>(wind);
        if (std::fabs(static_cast<f64>(sumNew) - sumRef) > 2.0 * ulpOf(sumRef)) ++bad;
    }
    check(bad == 0, "the folded sample position is within 2 ULP of the absolute sum for " + std::to_string(kPoints) + " points");

    const auto fx = cloudMotionFold(ShiftVec3d{12.5, -3.25, 0}, ShiftOrigin{static_cast<i64>(kStep), -static_cast<i64>(kStep)});
    check(fx.x == static_cast<f32>(kStep + 12.5) && fx.y == static_cast<f32>(-kStep - 3.25), "the fold adds the origin to each axis, x and y independently");
}

// ---- rows ---------------------------------------------------------------------------------------
static void testRowsAndLayout() {
    AVER_INFO("-- worldOriginRow, ShiftWorldInfo and the PerFrameCB layout --");
    f32 row[4];
    worldOriginRow(ShiftOrigin{static_cast<i64>(kStep) * 3, -static_cast<i64>(kStep) * 2}, 5, row);
    check(row[0] == 4'915'200.0f && row[1] == -3'276'800.0f && row[2] == 0.0f && row[3] == 5.0f, "worldOriginRow: O.x, O.y, 0, generation");
    check(static_cast<i64>(row[0]) == static_cast<i64>(kStep) * 3, "O is exact in f32");

    ShiftWorldInfo info;
    check(info.deltaCm.x == 0 && info.deltaCm.y == 0 && info.deltaCm.z == 0 && info.newOrigin.x == 0 && info.newOrigin.y == 0 && info.generation == 0,
          "a default ShiftWorldInfo is the identity shift");

    check(sizeof(PerFrameCB) == 784, "PerFrameCB is 784 bytes");
    check(offsetof(PerFrameCB, worldOrigin) == 752 && offsetof(PerFrameCB, wavePhase) == 768, "worldOrigin at 752, wavePhase at 768");
    check(offsetof(PerFrameCB, viewProjNoJitter) == 672 && offsetof(PerFrameCB, jitter) == 736, "no earlier offset moved");
}

// ---- points, matrices and the Gerstner phase the hosts' renderers share ----------------------------
static void testPointsAndGerstner() {
    AVER_INFO("-- shiftPoint / shiftWorldMatrix / shiftSteps / gerstnerPhaseOffset --");
    // A kept scene-frame position takes the SAME plain f32 subtraction the scene's roots do, so a renderer's copy stays
    // bit-equal to the entity it was taken from.
    f32 p[3] = {1.0f, 2.0f, 3.0f};
    shiftPoint(p, ShiftVec3d{5, 6, 7});
    check(p[0] == -4.0f && p[1] == -4.0f && p[2] == -4.0f, "shiftPoint subtracts the delta per axis");

    std::mt19937_64 rng(31);
    std::uniform_real_distribution<f64> u(-1.2e6, 1.2e6);
    bool bitEqualToScene = true, zeroKeeps = true;
    for (int n = 0; n < 1000; ++n) {
        f32 q[3] = {static_cast<f32>(u(rng)), static_cast<f32>(u(rng)), static_cast<f32>(u(rng) * 1e-3)};
        const f32 d[3] = {static_cast<f32>(kStep) * static_cast<f32>(static_cast<int>(n % 7) - 3), static_cast<f32>(kStep) * static_cast<f32>(static_cast<int>(n % 5) - 2), 0.0f};
        const f32 want[3] = {q[0] - d[0], q[1] - d[1], q[2] - d[2]};
        f32 got[3] = {q[0], q[1], q[2]};
        shiftPoint(got, ShiftVec3d{static_cast<f64>(d[0]), static_cast<f64>(d[1]), static_cast<f64>(d[2])});
        bitEqualToScene = bitEqualToScene && std::memcmp(got, want, sizeof got) == 0;
        f32 z[3] = {q[0], q[1], q[2]};
        shiftPoint(z, ShiftVec3d{});
        zeroKeeps = zeroKeeps && std::memcmp(z, q, sizeof z) == 0;
    }
    check(bitEqualToScene, "shiftPoint equals the scene's `p - (f32)delta`, bit for bit, for region-multiple deltas");
    check(zeroKeeps, "a zero delta leaves a point bit for bit (including a negative zero)");

    f32 m[16];
    for (int i = 0; i < 16; ++i) m[i] = static_cast<f32>(i + 1);
    f32 mm[16];
    std::memcpy(mm, m, sizeof m);
    shiftWorldMatrix(mm, ShiftVec3d{10, 20, 30});
    bool onlyRow3 = true;
    for (int i = 0; i < 16; ++i) onlyRow3 = onlyRow3 && (i >= 12 && i <= 14 ? mm[i] == m[i] - (i == 12 ? 10.0f : i == 13 ? 20.0f : 30.0f) : mm[i] == m[i]);
    check(onlyRow3, "shiftWorldMatrix moves the translation row (12..14) and nothing else");

    check(shiftSteps(0.0) == 0 && shiftSteps(kStep) == 1 && shiftSteps(-2.0 * kStep) == -2 && shiftSteps(0.4 * kStep) == 0 &&
          shiftSteps(6.0 * kStep) == 6, "shiftSteps counts whole regions, signed, rounding to the nearest");

    // The Gerstner phase as water.hlsl computes it: omega * dot(normalize(dir), p) with omega = 2*pi / max(wavelength, 1e-3).
    std::uniform_real_distribution<f64> u01(0.0, 1.0);
    int bad = 0;
    f64 worst = 0;
    for (int n = 0; n < 50'000; ++n) {
        const f32 a = static_cast<f32>(u01(rng) * 2.0 * kPi);
        const f32 dx = 3.0f * std::cos(a), dy = 3.0f * std::sin(a);                     // not pre-normalised, as the wave table holds it
        const f32 wl = static_cast<f32>(50.0 + u01(rng) * 6000.0);
        const ShiftOrigin o{static_cast<i64>(kStep) * (static_cast<i64>(u01(rng) * 14.0) - 7), static_cast<i64>(kStep) * (static_cast<i64>(u01(rng) * 14.0) - 7)};
        const f64 lx = (u01(rng) - 0.5) * 2.0 * 921'600.0, ly = (u01(rng) - 0.5) * 2.0 * 921'600.0;
        const f64 len = std::sqrt(static_cast<f64>(dx) * dx + static_cast<f64>(dy) * dy);
        const f64 ux = dx / len, uy = dy / len, omega = 2.0 * kPi / static_cast<f64>(wl);
        const f64 absPhase = omega * (ux * (lx + static_cast<f64>(o.x)) + uy * (ly + static_cast<f64>(o.y)));
        const f32 off = gerstnerPhaseOffset(dx, dy, wl, o);
        const f64 d = circularDiff(absPhase, omega * (ux * lx + uy * ly) + static_cast<f64>(off));
        worst = std::fmax(worst, d);
        if (d > 2e-3 || off < 0.0f || off > 6.2832f) ++bad;
    }
    AVER_INFO("  worst Gerstner phase error {:.3e} rad", worst);
    check(bad == 0, "gerstnerPhaseOffset keeps the Gerstner phase continuous across a rebase (2e-3 rad)");
    const f32 zero = gerstnerPhaseOffset(0.6f, 0.8f, 1000.0f, ShiftOrigin{});
    f32 pz = 0.0f;
    check(std::memcmp(&zero, &pz, sizeof zero) == 0, "origin zero gives a bit-exact +0.0 (identity)");
    check(gerstnerPhaseOffset(0.0f, 0.0f, 1000.0f, ShiftOrigin{static_cast<i64>(kStep), 0}) == 0.0f,
          "a degenerate direction (water.hlsl's zero wave) adds no phase");
    const f32 tiny = gerstnerPhaseOffset(1.0f, 0.0f, 0.0f, ShiftOrigin{static_cast<i64>(kStep), 0});
    check(std::isfinite(tiny) && tiny >= 0.0f && tiny <= 6.2832f, "a zero wavelength is clamped like the shader's max(wavelength, 1e-3)");

#ifdef WORLDSHIFT_HAS_GERSTNER
    // The CPU twin (fluids::gerstnerSetOriginPhase) is the same number the shader is given, bit for bit, and a body that
    // asks for the surface height at a scene-frame position gets the height the shader draws there.
    {
        bool same = true;
        for (int n = 0; n < 2000; ++n) {
            fluids::GerstnerWave w;
            const f32 a = static_cast<f32>(u01(rng) * 2.0 * kPi);
            w.dirX = 2.0f * std::cos(a); w.dirZ = 2.0f * std::sin(a);
            w.wavelengthCm = static_cast<f32>(50.0 + u01(rng) * 6000.0);
            const ShiftOrigin o{static_cast<i64>(kStep) * (static_cast<i64>(u01(rng) * 14.0) - 7), static_cast<i64>(kStep) * (static_cast<i64>(u01(rng) * 14.0) - 7)};
            fluids::gerstnerSetOriginPhase(&w, 1, o.x, o.y);
            const f32 want = gerstnerPhaseOffset(w.dirX, w.dirZ, w.wavelengthCm, o);
            same = same && std::memcmp(&w.originPhaseRad, &want, sizeof want) == 0;
        }
        check(same, "fluids::gerstnerSetOriginPhase equals rhi::gerstnerPhaseOffset bit for bit (the CPU and the GPU add one number)");

        fluids::GerstnerWave waves[2];
        waves[0].dirX = 1.0f; waves[0].dirZ = 0.3f; waves[0].wavelengthCm = 900.0f; waves[0].amplitudeCm = 30.0f;
        waves[1].dirX = -0.4f; waves[1].dirZ = 1.0f; waves[1].wavelengthCm = 410.0f; waves[1].amplitudeCm = 12.0f;
        fluids::GerstnerWave abs2[2] = {waves[0], waves[1]};
        fluids::gerstnerSetOriginPhase(waves, 2, static_cast<i64>(kStep) * 2, static_cast<i64>(kStep));
        f32 worstH = 0.0f;
        for (int n = 0; n < 2000; ++n) {
            const f32 lx = static_cast<f32>((u01(rng) - 0.5) * 20000.0), ly = static_cast<f32>((u01(rng) - 0.5) * 20000.0);
            const f32 t = static_cast<f32>(u01(rng) * 50.0);
            const f32 hLocal = fluids::gerstnerHeightCm(waves, 2, lx, ly, t);
            const f32 hAbs = fluids::gerstnerHeightCm(abs2, 2, static_cast<f32>(static_cast<f64>(lx) + 2.0 * kStep),
                                                     static_cast<f32>(static_cast<f64>(ly) + kStep), t);
            worstH = std::fmax(worstH, std::fabs(hLocal - hAbs));
        }
        check(worstH < 0.5f, "the CPU surface height at a scene-frame position equals the absolute one (worst " + std::to_string(worstH) + " cm)");
        fluids::GerstnerWave zero[2] = {abs2[0], abs2[1]};
        fluids::gerstnerSetOriginPhase(zero, 2, 0, 0);
        const f32 h0 = fluids::gerstnerHeightCm(zero, 2, 123.0f, 456.0f, 7.0f), h1 = fluids::gerstnerHeightCm(abs2, 2, 123.0f, 456.0f, 7.0f);
        check(std::memcmp(&h0, &h1, sizeof h0) == 0 && zero[0].originPhaseRad == 0.0f && zero[1].originPhaseRad == 0.0f,
              "origin zero: the CPU surface is unchanged (identity)");
    }
#else
    AVER_WARN("  (skipped: modules/fluids/include/aver/fluids/GerstnerWave.hpp was not found relative to this file)");
#endif
}

int main() {
    AVER_INFO("WorldShiftTest -- the renderer's floating-origin math");
    testHandComputed();
    testIdentity();
    testComposeAgainstReferences();
    testWavePhase();
    testCloudFold();
    testRowsAndLayout();
    testPointsAndGerstner();

    if (g_failures == 0) {
        AVER_INFO("WorldShiftTest: {} checks, all passed", g_checks);
        return 0;
    }
    AVER_ERROR("WorldShiftTest: {} of {} checks FAILED", g_failures, g_checks);
    return 1;
}
