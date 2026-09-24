// The atmosphere model, checked against numeric integrals and closed forms of the same physics.
#include "aver/rhi/Atmosphere.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;

static int g_failures = 0;

// Records one assertion. Counts a failure and logs it when the condition is false.
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Records one assertion that `got` is within `relTol` relative error of `want`.
static void checkNear(f64 got, f64 want, f64 relTol, const std::string& what) {
    const f64 err = std::fabs(got - want) / (std::fabs(want) > 1e-12 ? std::fabs(want) : 1.0);
    if (err <= relTol) { AVER_INFO("  ok    {} ({:.6g} vs {:.6g}, {:.3g} rel)", what, got, want, err); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} ({:.6g} vs {:.6g}, {:.3g} rel > {:.3g})", what, got, want, err, relTol);
}

// Reference exp(x*x) * erfc(x) in double, for x >= 0. Asymptotic series past x = 6.
static f64 erfcxRef(f64 x) {
    if (x < 6.0) return std::exp(x * x) * std::erfc(x);
    const f64 inv = 1.0 / (2.0 * x * x);
    f64 term = 1.0, sum = 1.0;
    for (int n = 1; n < 14; ++n) {
        term *= -(2.0 * n - 1.0) * inv;
        sum += term;
    }
    return sum / (x * std::sqrt(3.14159265358979323846));
}

// Reference Chapman function: the slant column along a ray over the vertical column where it starts,
// integrated numerically.
static f64 chapmanRef(f64 planetR, f64 scaleH, f64 altitude, f64 cosZenith) {
    const f64 r0 = planetR + altitude;
    const f64 farR = planetR + 400.0 * scaleH;
    const f64 b = r0 * cosZenith;
    const f64 disc = b * b - (r0 * r0 - farR * farR);
    if (disc <= 0.0) return 0.0;
    const f64 tMax = -b + std::sqrt(disc);

    const int steps = 2000000;
    const f64 dt = tMax / steps;
    f64 sum = 0.0;
    for (int i = 0; i < steps; ++i) {
        const f64 t = (i + 0.5) * dt;
        const f64 r = std::sqrt(r0 * r0 + 2.0 * r0 * cosZenith * t + t * t);
        sum += std::exp(-(r - planetR) / scaleH) * dt;
    }
    return sum / (scaleH * std::exp(-altitude / scaleH));
}

// Rec. 709 luminance of a linear RGB triple.
static f32 lum(const f32 c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// Runs every atmosphere check. Returns 1 if any failed.
int main() {
    AVER_INFO("AtmosphereTest");
    const rhi::AtmosphereProfile air{};
    const f32 sunRadius = 0.545f * 0.5f * 3.14159265f / 180.0f;

    AVER_INFO("erfcx fit");
    for (f64 y : {0.0, 0.1, 0.5, 1.0, 2.0, 4.0, 8.0, 15.0, 20.0, 30.0})
        checkNear(rhi::atmoErfcx(static_cast<f32>(y)), erfcxRef(y), 2e-6,
                  "erfcx(" + std::to_string(y) + ")");

    AVER_INFO("Chapman airmass against a numeric integral");
    for (f32 H : {8.0f, 1.2f}) {
        for (f32 alt : {0.0f, 10.0f}) {
            for (f32 zenithDeg : {0.0f, 30.0f, 60.0f, 80.0f, 88.0f, 90.0f}) {
                const f64 cz = std::cos(zenithDeg * 3.14159265358979 / 180.0);
                const f64 want = chapmanRef(air.planetRadiusKm, H, alt, cz);
                const f32 got = rhi::atmoChapman((air.planetRadiusKm + alt) / H, static_cast<f32>(cz));
                checkNear(got, want, 0.01,
                          "Ch(H=" + std::to_string(int(H * 10)) + "/10 alt=" + std::to_string(int(alt)) +
                          " chi=" + std::to_string(int(zenithDeg)) + ")");
            }
        }
    }

    AVER_INFO("Chapman limits");
    checkNear(rhi::atmoChapman(air.planetRadiusKm / air.rayleighScaleKm, 1.0f), 1.0, 2e-3,
              "straight up is one vertical column");
    checkNear(rhi::atmoChapman(air.planetRadiusKm / air.rayleighScaleKm, 0.0f),
              std::sqrt(3.14159265358979 * (air.planetRadiusKm / air.rayleighScaleKm) * 0.5), 1e-4,
              "horizontal is sqrt(pi x / 2)");
    {
        const f32 x = (air.planetRadiusKm + 10.0f) / air.rayleighScaleKm;
        checkNear(rhi::atmoChapman(x, -1e-6f), rhi::atmoChapman(x, 1e-6f), 1e-3,
                  "the two Chapman branches meet at the horizon");
    }

    AVER_INFO("optical depth and sun transmittance");
    {
        f32 prev = -1.0f;
        bool monotone = true;
        for (int deg = 0; deg <= 89; deg += 1) {
            f32 tau[3];
            rhi::atmoOpticalDepthToSpace(air, 0.0f, std::cos(deg * 3.14159265f / 180.0f), tau, nullptr);
            if (tau[1] < prev) monotone = false;
            prev = tau[1];
        }
        check(monotone, "optical depth rises as the ray tips toward the horizon");
    }
    {
        f32 noon[3], low[3], under[3];
        rhi::atmoSunTransmittance(air, 0.0f, 1.0f, sunRadius, noon);
        rhi::atmoSunTransmittance(air, 0.0f, std::sin(2.0f * 3.14159265f / 180.0f), sunRadius, low);
        rhi::atmoSunTransmittance(air, 0.0f, -0.2f, sunRadius, under);
        check(noon[0] > 0.85f && noon[0] < 1.0f, "the noon sun keeps most of its red");
        check(noon[2] < noon[0], "even at noon blue is attenuated more than red");
        check(low[0] < noon[0] && low[2] < noon[2], "a low sun is dimmer in every channel");
        check(low[0] / (low[2] + 1e-9f) > 6.0f * (noon[0] / (noon[2] + 1e-9f)),
              "and far redder: the R/B ratio grows by more than 6x from noon to 2 degrees");
        check(under[0] == 0.0f && under[1] == 0.0f && under[2] == 0.0f,
              "a sun below the planet's edge delivers nothing");
    }
    {
        f32 ground[3], high[3];
        rhi::atmoSunTransmittance(air, 0.0f, -0.02f, sunRadius, ground);
        rhi::atmoSunTransmittance(air, 20.0f, -0.02f, sunRadius, high);
        check(ground[1] <= 0.0f && high[1] > 0.0f, "at 20 km the sun is still up when it has set below");
    }

    AVER_INFO("sky radiance");
    {
        f32 sun[3] = {3.0f, 2.88f, 2.70f};
        f32 zenith[3], horizon[3];
        rhi::atmoSkyRadiance(air, 0.0f, 1.0f, 1.0f, 1.0f, sun, sunRadius, zenith);
        rhi::atmoSkyRadiance(air, 0.0f, 0.0f, 1.0f, 0.0f, sun, sunRadius, horizon);
        check(zenith[0] > 0.0f && zenith[1] > 0.0f && zenith[2] > 0.0f, "the midday zenith is lit");
        check(zenith[2] > zenith[0], "and it is blue: the zenith's blue beats its red");
        check(zenith[2] / zenith[0] > horizon[2] / horizon[0],
              "the horizon is paler than the zenith, which is what makes a sky read as depth");

        const f32 lowSun = std::sin(1.0f * 3.14159265f / 180.0f);
        f32 sunset[3];
        rhi::atmoSkyRadiance(air, 0.0f, 0.03f, lowSun, 0.999f, sun, sunRadius, sunset);
        check(sunset[0] / (sunset[2] + 1e-9f) > zenith[0] / (zenith[2] + 1e-9f) * 3.0f,
              "the sunset horizon is more than 3x redder, relative to blue, than the midday zenith");
    }
    {
        f32 sun[3] = {3.0f, 2.88f, 2.70f};
        f32 down[3];
        rhi::atmoSkyRadiance(air, 0.5f, -0.8f, 0.7f, -0.5f, sun, sunRadius, down);
        check(down[0] > 0.0f && down[1] > 0.0f && down[2] > 0.0f, "a downward ray still returns light");
    }

    AVER_INFO("the view march has converged at the shipped step count");
    {
        f32 sun[3] = {3.0f, 2.88f, 2.70f};
        rhi::AtmosphereProfile fine = air;
        fine.viewSteps = 512;
        const f32 dirs[5][3] = {   // view cos, sun cos, cos between
            {1.0f, 1.0f, 1.0f}, {1.0f, 0.5f, 0.5f}, {0.05f, 0.5f, 0.4f},
            {0.0f, 0.06f, 0.99f}, {0.3f, 0.26f, -0.9f},
        };
        for (int i = 0; i < 5; ++i) {
            f32 coarse[3], exact[3];
            rhi::atmoSkyRadiance(air, 0.0f, dirs[i][0], dirs[i][1], dirs[i][2], sun, sunRadius, coarse);
            rhi::atmoSkyRadiance(fine, 0.0f, dirs[i][0], dirs[i][1], dirs[i][2], sun, sunRadius, exact);
            for (int c = 0; c < 3; ++c)
                checkNear(coarse[c], exact[c], 0.03,
                          "view march " + std::to_string(i) + " channel " + std::to_string(c) +
                          " is within 3% of 512 steps");
        }
    }

    AVER_INFO("aerial perspective converges too, at its own much lower step count");
    {
        f32 sun[3] = {3.0f, 2.88f, 2.70f};
        rhi::AtmosphereProfile fine = air;
        fine.aerialSteps = 256;
        for (f32 km : {0.05f, 0.5f, 5.0f, 40.0f}) {
            f32 ci[3], ct[3], fi[3], ft[3];
            rhi::atmoAerialPerspective(air, 0.0f, 0.05f, 0.6f, 0.4f, km, sun, sunRadius, ci, ct);
            rhi::atmoAerialPerspective(fine, 0.0f, 0.05f, 0.6f, 0.4f, km, sun, sunRadius, fi, ft);
            for (int c = 0; c < 3; ++c) {
                const std::string at = std::to_string(int(km * 100)) + "/100 km, channel " + std::to_string(c);
                checkNear(ci[c], fi[c], 0.02, "aerial inscatter over " + at);
                checkNear(ct[c], ft[c], 0.005, "aerial transmittance over " + at);
            }
        }
    }

    AVER_INFO("single scattering matches its closed form");
    {
        // Plane-parallel closed form with Mie, ozone and multiple scattering off:
        //   L/E = P(cos) * (1 - exp(-tau0 * (1 + 1/mu0))) / (1 + 1/mu0)
        rhi::AtmosphereProfile pure{};
        pure.mieScatter = 0.0f;
        pure.mieExtinction = 0.0f;
        pure.ozoneAbsorb[0] = pure.ozoneAbsorb[1] = pure.ozoneAbsorb[2] = 0.0f;
        pure.multiScatterGain = 0.0f;
        pure.viewSteps = 512;
        const f32 one[3] = {1.0f, 1.0f, 1.0f};
        for (f32 elev : {90.0f, 60.0f, 40.0f}) {
            const f32 mu0 = std::sin(elev * 3.14159265f / 180.0f);
            f32 got[3];
            rhi::atmoSkyRadiance(pure, 0.0f, 1.0f, mu0, mu0, one, sunRadius, got);
            const f32 phase = 3.0f / (16.0f * 3.14159265f) * (1.0f + mu0 * mu0);
            for (int c = 0; c < 3; ++c) {
                const f32 tau0 = pure.rayleighScatter[c] * pure.rayleighScaleKm;
                const f32 k = 1.0f + 1.0f / mu0;
                const f32 want = phase * (1.0f - std::exp(-tau0 * k)) / k;
                checkNear(got[c], want, 0.02,
                          "pure Rayleigh zenith at " + std::to_string(int(elev)) + " deg, channel " +
                          std::to_string(c));
            }
        }
    }

    AVER_INFO("the fill light is calibrated against a measured clear sky");
    {
        f32 sun[3] = {3.0f, 2.88f, 2.70f};
        for (f32 elev : {30.0f, 48.0f, 60.0f}) {
            const f32 mu = std::sin(elev * 3.14159265f / 180.0f);
            rhi::AtmosphereDome d{};
            rhi::atmoFitDome(air, 0.0f, mu, sun, sunRadius, d);
            const f32 k = d.exponent;
            const f32 hp = std::pow(0.5f, k + 1.0f);
            const f32 I = 4.0f * (2.0f / (k + 2.0f) - 1.0f / (k + 1.0f) - hp / (k + 2.0f) + hp / (k + 1.0f));
            const f32 lz = lum(d.zenith), lh = lum(d.horizon);
            const f32 diffuse = 3.14159265f * (lh + (lz - lh) * I);
            f32 sunT[3];
            rhi::atmoSunTransmittance(air, 0.0f, mu, sunRadius, sunT);
            const f32 direct = lum(sun) * lum(sunT) * mu;
            const f32 ratio = diffuse / direct;
            AVER_INFO("  {:>4.0f} deg: diffuse/direct horizontal = {:.1f}%", elev, ratio * 100.0f);
            check(ratio > 0.15f && ratio < 0.30f,
                  "diffuse is 15-30% of direct at " + std::to_string(int(elev)) + " degrees");
        }
    }

    AVER_INFO("and the sky it produces is the right COLOUR, not only the right brightness");
    {
        f32 sun[3] = {3.0f, 2.88f, 2.70f};
        for (f32 elev : {40.0f, 60.0f}) {
            f32 zen[3];
            const f32 mu = std::sin(elev * 3.14159265f / 180.0f);
            rhi::atmoSkyRadiance(air, 0.0f, 1.0f, mu, mu, sun, sunRadius, zen);
            const f32 br = (zen[2] / sun[2]) / (zen[0] / sun[0] + 1e-9f);
            AVER_INFO("  {:>4.0f} deg: zenith blue/red = {:.2f}", elev, br);
            check(br > 2.5f && br < 5.0f,
                  "the zenith's blue/red is 2.5-5 at " + std::to_string(int(elev)) + " degrees");
        }
    }

    AVER_INFO("dome fit");
    {
        f32 sun[3] = {3.0f, 2.88f, 2.70f};
        rhi::AtmosphereDome noon{}, dusk{};
        rhi::atmoFitDome(air, 0.0f, 0.75f, sun, sunRadius, noon);
        rhi::atmoFitDome(air, 0.0f, std::sin(3.0f * 3.14159265f / 180.0f), sun, sunRadius, dusk);
        check(noon.exponent > 0.05f && noon.exponent < 8.0f, "the fitted exponent is in range");
        check(lum(noon.zenith) > 0.0f && lum(noon.horizon) > 0.0f, "both dome colours are lit");
        check(lum(noon.zenith) > lum(dusk.zenith), "the dusk sky is darker than the midday one");
        check(noon.sunTransmittance[0] > dusk.sunTransmittance[0],
              "and the direct sun is dimmer with it");
        check(dusk.sunTransmittance[0] / (dusk.sunTransmittance[2] + 1e-9f) >
              noon.sunTransmittance[0] / (noon.sunTransmittance[2] + 1e-9f),
              "the direct sun reddens as it sets, with nothing authored to say so");
        for (int c = 0; c < 3; ++c)
            check(noon.sunTransmittance[c] >= 0.0f && noon.sunTransmittance[c] <= 1.0f,
                  "transmittance stays a fraction");
    }
    {
        f32 sun[3] = {3.0f, 2.88f, 2.70f};
        // Fits the dome across an elevation range and reports the largest step in the exponent.
        auto sweep = [&](int from, int to, f32& worst, f32& worstAt) {
            worst = 0.0f; worstAt = 0.0f;
            f32 prev = -1.0f;
            for (int deg = from; deg <= to; deg += 2) {
                rhi::AtmosphereDome d{};
                rhi::atmoFitDome(air, 0.0f, std::sin(deg * 3.14159265f / 180.0f), sun, sunRadius, d);
                if (prev >= 0.0f && std::fabs(d.exponent - prev) > worst) {
                    worst = std::fabs(d.exponent - prev);
                    worstAt = static_cast<f32>(deg);
                }
                prev = d.exponent;
            }
        };
        f32 worst = 0.0f, worstAt = 0.0f;
        sweep(6, 70, worst, worstAt);
        check(worst < 0.12f,
              "in daylight the exponent never jumps more than 0.12 across two degrees "
              "(worst " + std::to_string(worst) + " at " + std::to_string(int(worstAt)) + " deg)");
        sweep(0, 6, worst, worstAt);
        check(worst < 0.35f,
              "and through sunrise it never jumps more than 0.35 "
              "(worst " + std::to_string(worst) + " at " + std::to_string(int(worstAt)) + " deg)");
    }

    AVER_INFO("derived dome by sun elevation (linear radiance, sun irradiance 3.00/2.88/2.70)");
    {
        f32 sun[3] = {3.0f, 2.88f, 2.70f};
        for (f32 elev : {90.0f, 60.0f, 48.0f, 30.0f, 15.0f, 6.0f, 2.0f, 0.0f, -2.0f}) {
            rhi::AtmosphereDome d{};
            rhi::atmoFitDome(air, 0.0f, std::sin(elev * 3.14159265f / 180.0f), sun, sunRadius, d);
            AVER_INFO("  {:>5.1f} deg  zenith {:.4f} {:.4f} {:.4f}  horizon {:.4f} {:.4f} {:.4f}  "
                      "k {:.2f}  sunT {:.4f} {:.4f} {:.4f}",
                      elev, d.zenith[0], d.zenith[1], d.zenith[2],
                      d.horizon[0], d.horizon[1], d.horizon[2], d.exponent,
                      d.sunTransmittance[0], d.sunTransmittance[1], d.sunTransmittance[2]);
        }
    }

    AVER_INFO("fog inscatter reference (mirrors RHIShaders.cpp's averFogInscatterRef)");
    {
        // WHY THIS TEST EXISTS: gates.ps1 already proves atmoFogInscatterRef's answer is byte-
        // identical to what the HLSL used to compute per pixel -- that is the strongest evidence
        // there is, since it compares actual rendered pixels. What gates cannot do is run headless,
        // in milliseconds, with no GPU, or catch a future edit that changes the HLSL's blend weight
        // or step count without updating this CPU mirror -- exactly the drift this file's own header
        // comment warns every other mirror function about. This checks the properties that edit
        // would break, not the physics atmoScatterSegment's own tests already cover.
        f32 sun[3] = {3.0f, 2.88f, 2.70f};
        f32 dirNoon[3]   = {0.01f, 0.0f, 1.0f};
        f32 dirSunset[3] = {1.0f, 0.0f, 0.05f};

        f32 noon[3], sunset[3];
        rhi::atmoFogInscatterRef(air, 0.05f, dirNoon, sun, sunRadius, noon);
        rhi::atmoFogInscatterRef(air, 0.05f, dirSunset, sun, sunRadius, sunset);

        auto finite = [](const f32 c[3]) {
            for (int i = 0; i < 3; ++i) if (!(c[i] >= 0.0f) || !std::isfinite(c[i])) return false;
            return true;
        };
        check(finite(noon), "finite and non-negative with the sun near zenith");
        check(finite(sunset), "finite and non-negative with the sun near the horizon");

        // MEASURED, per this repo's own convention: sun near the horizon warms the reference (higher
        // red/blue) relative to sun near zenith. Not asserting a specific ratio -- only that the
        // documented direction of the effect (RHIShaders.cpp: "near 1 ... as it nears the horizon")
        // actually holds, which is the qualitative behaviour that shipped and was screenshotted.
        const f32 rbNoon   = noon[0]   / std::fmax(noon[2],   1e-6f);
        const f32 rbSunset = sunset[0] / std::fmax(sunset[2], 1e-6f);
        check(rbSunset > rbNoon,
              "reference warms (red/blue rises) as the sun drops toward the horizon ("
              + std::to_string(rbNoon) + " -> " + std::to_string(rbSunset) + ")");

        // CONTINUOUS, NOT A THRESHOLD: the same property the dome exponent is checked for above, and
        // for the same reason -- this function replaced a threshold-shaped bug (c28d125) with a
        // blend specifically to avoid a seam, and a future edit could reintroduce one silently.
        //
        // TRACKS THE RED CHANNEL, NOT A RATIO. An early version of this check used red/blue and
        // failed on real, expected, physically-smooth behaviour: blue is heavily Rayleigh-extincted
        // near the horizon, so red/blue grows steeply as blue approaches zero even though red and
        // blue individually vary smoothly -- a ratio blows up nonlinearly right where a division by a
        // near-zero denominator would, which is a property of ratios, not evidence of a seam. Red
        // alone stays well away from zero across this whole sweep and is what "no single step
        // dominates" can actually mean here: FORTY steps (fine enough that a genuine c28d125-style
        // jump would stand out as an outlier even against real curvature near the horizon), checked
        // against the AVERAGE step size rather than the total swing -- the total swing is dominated
        // by the same near-horizon steepness a smooth function is allowed to have, so bounding a
        // single step by a fraction of it would fail on smoothness exactly like the ratio version did.
        const int kSweepSteps = 40;
        f32 prevRed = noon[0];
        f32 worstStep = 0.0f, sumStep = 0.0f;
        for (int i = 1; i <= kSweepSteps; ++i) {
            const f32 t = static_cast<f32>(i) / static_cast<f32>(kSweepSteps);   // 0 (noon) .. 1 (sunset)
            f32 dir[3] = {1.0f, 0.0f, 1.0f - t * 0.95f};
            f32 c[3];
            rhi::atmoFogInscatterRef(air, 0.05f, dir, sun, sunRadius, c);
            const f32 step = std::fabs(c[0] - prevRed);
            worstStep = std::fmax(worstStep, step);
            sumStep += step;
            prevRed = c[0];
        }
        const f32 avgStep = sumStep / static_cast<f32>(kSweepSteps);
        check(worstStep < 6.0f * avgStep,
              "no single step (of " + std::to_string(kSweepSteps) + ") in the red channel is an outlier "
              "against the average step (worst " + std::to_string(worstStep) + " vs avg "
              + std::to_string(avgStep) + ")");

        // ALTITUDE READS THE SAME FIELDS averAtmoCamAlt() DOES: a camera at a plausible eye height
        // (5 m) should differ only slightly from one floored at the 1 mm minimum -- both are
        // negligible next to the 8 km / 1.2 km Rayleigh/Mie scale heights this model runs on.
        f32 dirMid[3] = {0.3f, 0.0f, 0.6f};
        f32 floor_[3], eyeHeight[3];
        rhi::atmoFogInscatterRef(air, 1e-6f, dirMid, sun, sunRadius, floor_);
        rhi::atmoFogInscatterRef(air, 0.005f, dirMid, sun, sunRadius, eyeHeight);
        for (int i = 0; i < 3; ++i)
            checkNear(eyeHeight[i], floor_[i], 0.05,
                      "5 m eye height vs the altitude floor, channel " + std::to_string(i));
    }

    AVER_INFO("sky irradiance as spherical harmonics");
    {
        // MIRRORS averShIrradiance IN RHIShaders.cpp, constants and all. That duplication is the
        // point rather than a smell: these five numbers are the cosine convolution with the SH
        // basis folded in, they are the one place the CPU projection and the GPU reconstruction
        // have to agree, and nothing else in the build would notice if one of them were retyped.
        auto irradiance = [](const rhi::AtmosphereSkySH& sh, f32 x, f32 y, f32 z, f32 out[3]) {
            for (int c = 0; c < 3; ++c) {
                f32 e = sh.c[0][c] * 0.282095f;
                e += (sh.c[1][c] * y + sh.c[2][c] * z + sh.c[3][c] * x) * 0.325735f;
                e += (sh.c[4][c] * (x * y) + sh.c[5][c] * (y * z) + sh.c[7][c] * (x * z)) * 0.273137f;
                e += sh.c[6][c] * ((3.0f * z * z - 1.0f) * 0.078848f);
                e += sh.c[8][c] * ((x * x - y * y) * 0.136569f);
                out[c] = e > 0.0f ? e : 0.0f;
            }
        };

        f32 sun[3] = {3.0f, 2.88f, 2.70f};

        // A LOW SUN ALONG +X. This is the case the whole change exists for: before it, the ambient
        // term read only N.z, so these two walls were handed identical light.
        f32 lowSun[3] = {0.995f, 0.0f, 0.1f};
        rhi::AtmosphereSkySH low{};
        rhi::atmoSkyRadianceSH(air, 0.0f, lowSun, sun, sunRadius, low);

        f32 toward[3], away[3], up[3];
        irradiance(low,  1.0f, 0.0f, 0.0f, toward);
        irradiance(low, -1.0f, 0.0f, 0.0f, away);
        irradiance(low,  0.0f, 0.0f, 1.0f, up);

        for (int c = 0; c < 3; ++c)
            check(toward[c] >= 0.0f && away[c] >= 0.0f && up[c] >= 0.0f,
                  "reconstructed irradiance is never negative, channel " + std::to_string(c));
        AVER_INFO("  low sun along +X: facing it {} vs facing away {} ({}x)",
                  lum(toward), lum(away), lum(toward) / (lum(away) + 1e-9f));
        check(lum(toward) > lum(away) * 1.1f,
              "a wall facing a low sun receives more sky light than one facing away");
        check(lum(up) > 0.0f, "and the sky still lights an upward-facing surface");

        // THE OTHER SIDE OF THE SAME TEST, and it is the one that would catch a projection that
        // merely invented a gradient: with the sun at the ZENITH the sky IS azimuthally symmetric,
        // so the same two walls must come back together again.
        f32 highSun[3] = {0.0f, 0.0f, 1.0f};
        rhi::AtmosphereSkySH high{};
        rhi::atmoSkyRadianceSH(air, 0.0f, highSun, sun, sunRadius, high);
        f32 hEast[3], hWest[3];
        irradiance(high,  1.0f, 0.0f, 0.0f, hEast);
        irradiance(high, -1.0f, 0.0f, 0.0f, hWest);
        checkNear(lum(hEast), lum(hWest), 0.02,
                  "with the sun overhead the azimuth stops mattering again");

        // AGAINST A BRUTE-FORCE HEMISPHERE INTEGRAL, which is the only check here that can tell a
        // plausible reconstruction from a correct one. The nine coefficients are supposed to encode
        // (1/PI) * integral over the hemisphere of L(w) * max(0, w.n) dw -- so compute exactly that,
        // by summation over the sphere, and compare. Nothing else in this file would notice if the
        // cosine convolution factors were wrong by a constant, and a constant is precisely the kind
        // of error that reads as "the scene got darker" rather than as a fault.
        auto reference = [&](const f32 sunD[3], f32 nx, f32 ny, f32 nz, f32 out[3]) {
            const int   kRef   = 4096;
            const f32   golden = 2.399963229728653f;
            const f32   w      = 4.0f * 3.14159265358979f / static_cast<f32>(kRef);
            const f32   len    = std::sqrt(sunD[0]*sunD[0] + sunD[1]*sunD[1] + sunD[2]*sunD[2]);
            const f32   sx = sunD[0]/len, sy = sunD[1]/len, sz = sunD[2]/len;
            out[0] = out[1] = out[2] = 0.0f;
            for (int i = 0; i < kRef; ++i) {
                const f32 z   = 1.0f - (2.0f * static_cast<f32>(i) + 1.0f) / static_cast<f32>(kRef);
                const f32 rad = std::sqrt(std::fmax(1.0f - z * z, 0.0f));
                const f32 phi = golden * static_cast<f32>(i);
                const f32 x = rad * std::cos(phi), y = rad * std::sin(phi);
                const f32 cosine = x * nx + y * ny + z * nz;
                if (cosine <= 0.0f) continue;
                f32 rgb[3];
                rhi::atmoSkyRadiance(air, 0.0f, z, sz,
                                     std::fmax(-1.0f, std::fmin(1.0f, x*sx + y*sy + z*sz)),
                                     sun, sunRadius, rgb);
                for (int c = 0; c < 3; ++c) out[c] += rgb[c] * cosine * w;
            }
            for (int c = 0; c < 3; ++c) out[c] /= 3.14159265358979f;
        };

        const f32 normals[3][3] = {{1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
        const char* names[3] = {"toward the sun", "away from the sun", "straight up"};
        for (int t = 0; t < 3; ++t) {
            f32 got[3], want[3];
            irradiance(low, normals[t][0], normals[t][1], normals[t][2], got);
            reference(lowSun, normals[t][0], normals[t][1], normals[t][2], want);
            for (int c = 0; c < 3; ++c)
                // 0.15, and the slack is L2 TRUNCATION, not tolerance for a bug. Nine
                // coefficients cannot represent a sky with a strong low sun exactly; the two
                // horizontal normals land within 1.4% and the zenith red channel is the worst
                // case at about 12%. That is the documented price of nine numbers over a
                // per-pixel march, and it is far smaller than the error it replaces.
                checkNear(got[c], want[c], 0.15,
                          std::string("SH irradiance matches the hemisphere integral, ") + names[t] +
                          ", channel " + std::to_string(c));
        }

        // A zero sun direction is the one input that cannot be projected; it must return zeroes
        // rather than a NaN that would then be multiplied into every lit pixel in the frame.
        f32 noSun[3] = {0.0f, 0.0f, 0.0f};
        rhi::AtmosphereSkySH none{};
        rhi::atmoSkyRadianceSH(air, 0.0f, noSun, sun, sunRadius, none);
        bool allZero = true;
        for (int k = 0; k < 9; ++k)
            for (int c = 0; c < 3; ++c) if (none.c[k][c] != 0.0f) allZero = false;
        check(allZero, "a degenerate sun direction projects to zeroes, not to NaN");
    }

    AVER_INFO("luminance diffuse/direct share at calibration 1x (shipped since 2026-09-24) vs the old 8x "
              "(R8, contrast-fix plan section 8; this is a plausibility probe, not a regression gate, "
              "and IS ALLOWED TO FAIL)");
    {
        // e0 = (1,1,1), the plan's own choice for T4: an achromatic sun isolates the SHAPE of the
        // diffuse/direct split (how much of a Lambertian receiver's light is sky vs sun) from any
        // authored sun colour. Every AtmosphereTest check above this one deliberately uses a coloured
        // sun (3.00/2.88/2.70) to also exercise chroma; this one wants the plan's own achromatic case
        // instead, so a passing or failing D(8) means what the plan says it means.
        const f32 e0[3] = {1.0f, 1.0f, 1.0f};
        for (f32 elevDeg : {30.0f, 45.0f, 60.0f}) {
            const f32 h  = elevDeg * 3.14159265f / 180.0f;
            const f32 mu = std::sin(h);                        // sunCosZenith, this file's own convention
            // Azimuth-free: an "up"-facing receiver sees the sky symmetrically about the vertical
            // axis (checked explicitly by "sky irradiance as spherical harmonics" above, the
            // "azimuth stops mattering" case), so which horizontal direction the sun sits in cannot
            // change the irradiance computed below -- only elevation can. x/z chosen unit so
            // atmoSkyRadianceSH receives a normalised sun direction.
            const f32 sunDir[3] = {std::cos(h), 0.0f, mu};

            rhi::AtmosphereSkySH sh{};
            rhi::atmoSkyRadianceSH(air, 0.0f, sunDir, e0, sunRadius, sh);
            // E/pi at "up" (nx=0, ny=0, nz=1): the general SH reconstruction ("sky irradiance as
            // spherical harmonics" above) restricted to this one normal -- every term that multiplies
            // by x or y drops out at x=y=0, leaving only c[0] (the 0.282095 constant term), c[2] (the
            // 0.325735 z term) and c[6] (the 0.078848 (3z^2-1) term, which is 2*0.078848 at z=1).
            // These three constants are shared_prelude.hlsl:810-814's own SH basis, read-only
            // reference for this test.
            f32 eOverPi[3];
            for (int c = 0; c < 3; ++c)
                eOverPi[c] = sh.c[0][c] * 0.282095f + sh.c[2][c] * 0.325735f + sh.c[6][c] * (2.0f * 0.078848f);

            rhi::AtmosphereDome dome{};
            rhi::atmoFitDome(air, 0.0f, mu, e0, sunRadius, dome);

            const f32 ambientLum = lum(eOverPi);
            const f32 sunLum     = lum(dome.sunTransmittance);
            // D(k) = pi*k*(E/pi) / (pi*k*(E/pi) + T*sin(h)) -- the plan's own formula, kept exactly
            // as written (the pi cancels algebraically, but transcribing it verbatim is what makes
            // this traceable back to section 5's T4 spec rather than to a simplification of it).
            auto diffuseFraction = [&](f32 calibration) {
                const f32 numerator = 3.14159265f * calibration * ambientLum;
                return numerator / (numerator + sunLum * mu);
            };
            const f32 d1 = diffuseFraction(1.0f);
            const f32 d8 = diffuseFraction(8.0f);
            AVER_INFO("  {:>4.0f} deg: D(1x) = {:.4f}   D(8x) = {:.4f}", elevDeg, d1, d8);
            // REPORTED, NOT CHECKED. kSkyIrradianceCalibration is 1 now (D3D12Device.cpp's packAtmosphere
            // has why the old 8 was removed), so D(1x) is the shipped share and the one the band applies
            // to. The band's bounds are still UNCONFIRMED, so an out-of-band value is evidence, not a
            // regression -- and a check() here would make the whole suite read FAIL for it.
            if (d1 < 0.08f || d1 > 0.30f)
                AVER_WARN("  {:>4.0f} deg: D(1x) = {:.4f} is outside the clear-sky plausibility band [0.08, 0.30] "
                          "(informational: evidence about the atmosphere model's diffuse share, not a failure)",
                          elevDeg, d1);
        }
    }

    AVER_INFO(g_failures ? "AtmosphereTest: {} FAILURES" : "AtmosphereTest: all checks passed ({})",
              g_failures);
    return g_failures ? 1 : 0;
}
