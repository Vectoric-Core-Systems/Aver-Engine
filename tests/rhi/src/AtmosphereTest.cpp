// The atmosphere model, against brute force.
//
// Nothing here needs a GPU, and nothing here is decidable by looking at a screenshot: an airmass
// that is 20% low still renders a blue sky, a transmittance that forgets ozone still reddens at
// sunset, and a phase function missing its normalisation is just a slightly different exposure.
// Each of those is checked against a numeric integral of the same physics instead.
#include "aver/rhi/Atmosphere.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static void checkNear(f64 got, f64 want, f64 relTol, const std::string& what) {
    const f64 err = std::fabs(got - want) / (std::fabs(want) > 1e-12 ? std::fabs(want) : 1.0);
    if (err <= relTol) { AVER_INFO("  ok    {} ({:.6g} vs {:.6g}, {:.3g} rel)", what, got, want, err); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {} ({:.6g} vs {:.6g}, {:.3g} rel > {:.3g})", what, got, want, err, relTol);
}

// exp(x*x) * erfc(x) in double, for x >= 0. The library's erfc underflows past about x = 27 and the
// exp that would undo it overflows first, so the scaled form is computed directly out there.
static f64 erfcxRef(f64 x) {
    if (x < 6.0) return std::exp(x * x) * std::erfc(x);
    // Asymptotic series: 1/(x sqrt(pi)) * sum (-1)^n (2n-1)!! / (2x^2)^n. At x >= 6 the terms fall
    // by a factor of 72 or better, so a dozen of them are past double precision.
    const f64 inv = 1.0 / (2.0 * x * x);
    f64 term = 1.0, sum = 1.0;
    for (int n = 1; n < 14; ++n) {
        term *= -(2.0 * n - 1.0) * inv;
        sum += term;
    }
    return sum / (x * std::sqrt(3.14159265358979323846));
}

// The slant column along a ray divided by the vertical column where it starts -- the Chapman
// function's definition, integrated rather than approximated.
static f64 chapmanRef(f64 planetR, f64 scaleH, f64 altitude, f64 cosZenith) {
    const f64 r0 = planetR + altitude;
    const f64 farR = planetR + 400.0 * scaleH;      // Chapman assumes an unbounded atmosphere
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

static f32 lum(const f32 c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

int main() {
    AVER_INFO("AtmosphereTest");
    const rhi::AtmosphereProfile air{};
    const f32 sunRadius = 0.545f * 0.5f * 3.14159265f / 180.0f;

    AVER_INFO("erfcx fit");
    for (f64 y : {0.0, 0.1, 0.5, 1.0, 2.0, 4.0, 8.0, 15.0, 20.0, 30.0})
        checkNear(rhi::atmoErfcx(static_cast<f32>(y)), erfcxRef(y), 2e-6,
                  "erfcx(" + std::to_string(y) + ")");

    AVER_INFO("Chapman airmass against a numeric integral");
    // The two scale heights the model actually uses, at the two altitudes it is evaluated at.
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
    // Continuous across the branch, which is where a sign error would show and nowhere else. The
    // two sides differ by 2*up*(1 - erfcx(eps)), so the offset has to be small for the claim to
    // mean anything: at 1e-4 they are legitimately 0.45% apart.
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
        // Altitude buys elevation: the sun clears the planet from higher up for longer.
        f32 ground[3], high[3];
        rhi::atmoSunTransmittance(air, 0.0f, -0.02f, sunRadius, ground);
        rhi::atmoSunTransmittance(air, 20.0f, -0.02f, sunRadius, high);
        check(ground[1] <= 0.0f && high[1] > 0.0f, "at 20 km the sun is still up when it has set below");
    }

    AVER_INFO("sky radiance");
    {
        f32 sun[3] = {3.0f, 2.88f, 2.70f};   // the engine's default sun colour x intensity
        f32 zenith[3], horizon[3];
        rhi::atmoSkyRadiance(air, 0.0f, 1.0f, 1.0f, 1.0f, sun, sunRadius, zenith);
        rhi::atmoSkyRadiance(air, 0.0f, 0.0f, 1.0f, 0.0f, sun, sunRadius, horizon);
        check(zenith[0] > 0.0f && zenith[1] > 0.0f && zenith[2] > 0.0f, "the midday zenith is lit");
        check(zenith[2] > zenith[0], "and it is blue: the zenith's blue beats its red");
        check(zenith[2] / zenith[0] > horizon[2] / horizon[0],
              "the horizon is paler than the zenith, which is what makes a sky read as depth");

        // Sunset: the sky toward a 1-degree sun is redder than the same sky at noon. This is the
        // property the whole model exists for -- nobody authored it.
        const f32 lowSun = std::sin(1.0f * 3.14159265f / 180.0f);
        f32 sunset[3];
        rhi::atmoSkyRadiance(air, 0.0f, 0.03f, lowSun, 0.999f, sun, sunRadius, sunset);
        check(sunset[0] / (sunset[2] + 1e-9f) > zenith[0] / (zenith[2] + 1e-9f) * 3.0f,
              "the sunset horizon is more than 3x redder, relative to blue, than the midday zenith");
    }
    {
        // Looking down closes the ray on the ground rather than leaving a hole in the dome.
        f32 sun[3] = {3.0f, 2.88f, 2.70f};
        f32 down[3];
        rhi::atmoSkyRadiance(air, 0.5f, -0.8f, 0.7f, -0.5f, sun, sunRadius, down);
        check(down[0] > 0.0f && down[1] > 0.0f && down[2] > 0.0f, "a downward ray still returns light");
    }

    AVER_INFO("the view march has converged at the shipped step count");
    {
        // A step count too low does not look broken -- it looks like a slightly different sky, and
        // there is nothing on screen to compare it against. So it is compared against itself.
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
        // The aerial segment is kilometres rather than hundreds of kilometres, which is the whole
        // reason it can afford four steps. That claim is checked, not assumed -- and it is checked
        // through the same distance-bounded entry point the shader calls, not a stand-in.
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
        // With Mie, ozone and the multiple-scattering term all switched off, the zenith radiance of
        // a pure Rayleigh atmosphere has an exact solution, and this is the check that says whether
        // the whole march is right at the LEVEL of magnitude rather than only in shape.
        //
        // Substituting -dtau for beta*dh along a vertical view ray:
        //   L/E = P(cos) * (1 - exp(-tau0 * (1 + 1/mu0))) / (1 + 1/mu0)
        // It is plane-parallel where the model is spherical, so it is only quoted for a sun well
        // clear of the horizon, where Chapman and 1/mu0 agree to better than a tenth of a percent.
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
                // 2%, because the two differ by more than discretisation: the model is spherical
                // and stops at a 60 km shell, the closed form is plane-parallel and unbounded. Blue
                // sits at 1.2-1.8% low for exactly that reason and red at 0.1%, which is the right
                // way round -- the effect scales with optical depth.
                checkNear(got[c], want, 0.02,
                          "pure Rayleigh zenith at " + std::to_string(int(elev)) + " deg, channel " +
                          std::to_string(c));
            }
        }
    }

    AVER_INFO("the fill light is calibrated against a measured clear sky");
    {
        // THE ONE NUMBER THAT PINS multiScatterGain, which is otherwise a term with no scale of its
        // own. Single scattering alone leaves a clear sky roughly half as bright as the real thing;
        // the isotropic stand-in for every further bounce closes that, and this is what says by how
        // much rather than leaving it to taste.
        //
        // Target: for a clear sky with the sun at 30-60 degrees, diffuse horizontal illuminance is
        // 15-30% of direct horizontal. (Typical clear-day figures are ~15 klux diffuse against
        // ~70 klux direct at 48 degrees; the photometric ratio runs higher than the radiometric one
        // because skylight is blue-rich and blue carries more luminous efficacy.)
        f32 sun[3] = {3.0f, 2.88f, 2.70f};
        for (f32 elev : {30.0f, 48.0f, 60.0f}) {
            const f32 mu = std::sin(elev * 3.14159265f / 180.0f);
            rhi::AtmosphereDome d{};
            rhi::atmoFitDome(air, 0.0f, mu, sun, sunRadius, d);
            // The dome's own hemispherical irradiance, from the exponent that was fitted to carry it.
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
        // THE SECOND MEASUREMENT THAT PINS multiScatterGain, and the one that decides the term is
        // spectrally FLAT. A real clear zenith runs about 15000-25000 K, which is a blue-to-red
        // radiance ratio of roughly 2.9 to 4. Shaping the multiple-scattering term by sigma_s
        // squared -- the formally tidier choice -- puts it at 7.2, a sky bluer than any real one.
        //
        // Brightness alone cannot catch that: the gain can always be retuned to hit the irradiance
        // target while the hue goes wherever it likes. It takes both numbers to hold the term down.
        f32 sun[3] = {3.0f, 2.88f, 2.70f};
        for (f32 elev : {40.0f, 60.0f}) {
            f32 zen[3];
            const f32 mu = std::sin(elev * 3.14159265f / 180.0f);
            rhi::atmoSkyRadiance(air, 0.0f, 1.0f, mu, mu, sun, sunRadius, zen);
            // Divided out of the sun's own colour, so this measures the AIR and not the light.
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
        // The exponent is what every ambient term and every environment reflection reads, so it has
        // to move SMOOTHLY as the sun does. Fitting it to a 45-degree sample did not -- it walked
        // 0.22 to 2.46 non-monotonically across a sunset and the fill light breathed with it.
        f32 sun[3] = {3.0f, 2.88f, 2.70f};
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
        // The first few degrees are looser on purpose: the sky really does change fast there, and
        // pretending otherwise would mean smoothing over a sunrise. The bound still has to hold,
        // because the failure this guards against was a 2.2 swing, not a 0.2 one.
        sweep(0, 6, worst, worstAt);
        check(worst < 0.35f,
              "and through sunrise it never jumps more than 0.35 "
              "(worst " + std::to_string(worst) + " at " + std::to_string(int(worstAt)) + " deg)");
    }

    // Not an assertion: the table a person reads when the sky looks wrong. Every number here is a
    // consequence of one input, the elevation on the left.
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

    AVER_INFO(g_failures ? "AtmosphereTest: {} FAILURES" : "AtmosphereTest: all checks passed ({})",
              g_failures);
    return g_failures ? 1 : 0;
}
