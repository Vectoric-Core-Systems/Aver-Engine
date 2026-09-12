// FoliageAlignTest -- "Align to slope" actually tilts a placed foliage instance to the terrain
// normal, and does NOT when the option is off.
//
// THE DEFECT THIS PROVES FIXED. foliageAlignToNormal_ (SandboxApp.cpp) used to be read nowhere:
// foliagePlaceOne computed a random yaw and nothing else, so the checkbox was dead UI regardless of
// its state. foliagePlacementRotation (FoliageAlign.hpp) is the extracted fix -- see that header's
// own comment for why the normal is sampled fresh at each instance's own (x, y) rather than reusing
// the brush's single centre-hit (which the heightfield ray API does not even carry a normal for).
//
// THE FIXTURE is a synthetic ramp, not a loaded .ocland: OcLandData is plain data (sampleCount,
// spacingCm, a heights vector), so a known linear slope can be built directly in memory and its
// normal known analytically ahead of time -- the standard heightfield identity
// n ~ (-dh/dx, -dh/dy, 1), normalised -- rather than trusting the function under test to grade
// itself.
#include "FoliageAlign.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool nearlyEqual(f32 a, f32 b, f32 tol) { return std::fabs(a - b) < tol; }
static bool nearlyEqual(const Vec3& a, const Vec3& b, f32 tol) {
    return nearlyEqual(a.x, b.x, tol) && nearlyEqual(a.y, b.y, tol) && nearlyEqual(a.z, b.z, tol);
}

// A section that ramps linearly along X (height = ix * riseCmPerSample) and is flat along Y, so its
// analytic slope and normal are known exactly: dh/dx = riseCmPerSample / spacingCm, dh/dy = 0.
static fmt::OcLandData rampFixture(u32 sampleCount, f32 spacingCm, f32 riseCmPerSample) {
    fmt::OcLandData d;
    d.sampleCount = sampleCount;
    d.spacingCm = spacingCm;
    d.originCm[0] = d.originCm[1] = d.originCm[2] = 0.0f;
    d.heights.assign(static_cast<usize>(sampleCount) * sampleCount, 0.0f);
    for (u32 iy = 0; iy < sampleCount; ++iy)
        for (u32 ix = 0; ix < sampleCount; ++ix)
            d.heights[static_cast<usize>(iy) * sampleCount + ix] = static_cast<f32>(ix) * riseCmPerSample;
    return d;
}

int main() {
    AVER_INFO("FoliageAlignTest");

    constexpr f32 kSpacingCm = 100.0f, kRiseCm = 50.0f, kEpsCm = 10.0f;
    const fmt::OcLandData land = rampFixture(9, kSpacingCm, kRiseCm);
    check(land.valid(), "the synthetic ramp is an internally consistent heightfield");

    const f32 slope = kRiseCm / kSpacingCm;   // dh/dx
    const Vec3 expectedNormal = Vec3{-slope, 0.0f, 1.0f}.getSafeNormal();
    const f32 midX = 400.0f, midY = 400.0f;   // well inside the 800 cm-square footprint

    AVER_INFO("foliageSurfaceNormal matches the ramp's known analytic normal");
    {
        Vec3 n;
        check(foliageSurfaceNormal(land, midX, midY, kEpsCm, n), "a probe well inside the section succeeds");
        check(nearlyEqual(n, expectedNormal, 0.01f),
              "and the sampled normal matches -(dh/dx, dh/dy, -1) normalised, within tolerance");
    }

    AVER_INFO("Align to slope ON: the instance's up-vector matches the surface normal");
    {
        const Quat rot = foliagePlacementRotation(land, midX, midY, kEpsCm, /*yaw=*/0.0f,
                                                    /*alignToNormal=*/true);
        const Vec3 up = rot.rotate(Vec3{0.0f, 0.0f, 1.0f});
        check(nearlyEqual(up, expectedNormal, 0.01f), "up-vector == the sampled surface normal");
    }

    AVER_INFO("Align to slope OFF: the instance stands straight up regardless of the slope underneath");
    {
        const Quat rot = foliagePlacementRotation(land, midX, midY, kEpsCm, 0.0f, /*alignToNormal=*/false);
        const Vec3 up = rot.rotate(Vec3{0.0f, 0.0f, 1.0f});
        check(nearlyEqual(up, Vec3{0.0f, 0.0f, 1.0f}, 1e-4f),
              "up-vector stays world +Z on the SAME slope the ON case just tilted for");
    }

    AVER_INFO("yaw is preserved as azimuth, not overridden by the tilt");
    {
        // A quarter-turn yaw must still leave the up-vector AT THE NORMAL: yaw only ever spins the
        // instance about its own eventual up-axis, so the tilt result cannot depend on it.
        const Quat rot = foliagePlacementRotation(land, midX, midY, kEpsCm, kPi * 0.5f, true);
        const Vec3 up = rot.rotate(Vec3{0.0f, 0.0f, 1.0f});
        check(nearlyEqual(up, expectedNormal, 0.01f), "up-vector is unaffected by which yaw was drawn");

        // But it DOES still turn the instance -- a fixed reference axis (local +X here) must rotate
        // with yaw, or "random azimuth" would be a lie once alignment is on.
        const Quat rotOtherYaw = foliagePlacementRotation(land, midX, midY, kEpsCm, kPi, true);
        const Vec3 fwd0 = rot.rotate(Vec3{1.0f, 0.0f, 0.0f});
        const Vec3 fwd1 = rotOtherYaw.rotate(Vec3{1.0f, 0.0f, 0.0f});
        check(!nearlyEqual(fwd0, fwd1, 0.05f), "two different yaws still produce two different facings");
    }

    AVER_INFO("off the section's footprint, alignment falls back to yaw-only rather than misbehaving");
    {
        const Quat rot = foliagePlacementRotation(land, -100000.0f, -100000.0f, kEpsCm, 0.0f, true);
        const Vec3 up = rot.rotate(Vec3{0.0f, 0.0f, 1.0f});
        check(nearlyEqual(up, Vec3{0.0f, 0.0f, 1.0f}, 1e-4f),
              "falls back to standing straight up rather than asserting or returning garbage");

        Vec3 n;
        check(!foliageSurfaceNormal(land, -100000.0f, -100000.0f, kEpsCm, n),
              "and foliageSurfaceNormal itself reports false off the footprint, matching surfaceHeightAt");
    }

    AVER_INFO("flat ground (zero slope) is a no-op tilt in both modes");
    {
        const fmt::OcLandData flat = rampFixture(5, kSpacingCm, 0.0f);
        const Quat rotOn = foliagePlacementRotation(flat, 200.0f, 200.0f, kEpsCm, 0.7f, true);
        const Quat rotOff = foliagePlacementRotation(flat, 200.0f, 200.0f, kEpsCm, 0.7f, false);
        const Vec3 upOn = rotOn.rotate(Vec3{0.0f, 0.0f, 1.0f});
        const Vec3 upOff = rotOff.rotate(Vec3{0.0f, 0.0f, 1.0f});
        check(nearlyEqual(upOn, Vec3{0.0f, 0.0f, 1.0f}, 1e-3f), "ON matches OFF on flat ground");
        check(nearlyEqual(upOff, Vec3{0.0f, 0.0f, 1.0f}, 1e-4f), "and both stand straight up");
    }

    AVER_INFO(g_failures ? "FoliageAlignTest: {} FAILURES" : "FoliageAlignTest: all checks passed ({})",
              g_failures);
    return g_failures ? 1 : 0;
}
