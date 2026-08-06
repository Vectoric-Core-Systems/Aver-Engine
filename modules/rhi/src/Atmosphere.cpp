// The atmosphere model declared in Atmosphere.hpp. Mirrored, function for function, by the HLSL in
// RHIShaders.cpp.
#include "aver/rhi/Atmosphere.hpp"

#include <cmath>

namespace aver::rhi {

namespace {

constexpr f32 kPi = 3.14159265358979f;

f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
f32 sat(f32 v) { return clampf(v, 0.0f, 1.0f); }

// sin of an angle given its cosine, guarded against a cosine outside [-1,1].
f32 sinFromCos(f32 c) { return std::sqrt(sat(1.0f - c * c)); }

// Unit-peak density of the ozone tent at an altitude.
f32 ozoneDensity(const AtmosphereProfile& a, f32 altKm) {
    const f32 w = a.ozoneWidthKm > 1e-4f ? a.ozoneWidthKm : 1e-4f;
    const f32 d = std::fabs(altKm - a.ozoneCentreKm) / w;
    return d < 1.0f ? 1.0f - d : 0.0f;
}

// Vertical column of that tent from `altKm` upward, in kilometres.
f32 ozoneColumnAbove(const AtmosphereProfile& a, f32 altKm) {
    const f32 w = a.ozoneWidthKm > 1e-4f ? a.ozoneWidthKm : 1e-4f;
    const f32 c = a.ozoneCentreKm;
    if (altKm >= c + w) return 0.0f;
    if (altKm >= c)     { const f32 s = (c + w - altKm) / w; return w * s * s * 0.5f; }
    if (altKm >= c - w) { const f32 u = (c - altKm) / w;     return w * (u - u * u * 0.5f) + w * 0.5f; }
    return w;
}

// Slant factor for the ozone layer treated as a thin shell, capped at tangency.
f32 ozoneAirmass(const AtmosphereProfile& a, f32 rKm, f32 cosZenith) {
    const f32 w  = a.ozoneWidthKm > 1e-4f ? a.ozoneWidthKm : 1e-4f;
    const f32 ro = a.planetRadiusKm + a.ozoneCentreKm;
    const f32 s  = rKm * sinFromCos(cosZenith) / ro;
    const f32 maxAir = 1.2f * std::sqrt(2.0f * ro * w) / w;
    const f32 capped = clampf(1.0f / std::sqrt(1.0f - clampf(s * s, 0.0f, 0.999999f)), 1.0f, maxAir);
    return cosZenith >= 0.0f ? capped : 2.0f * maxAir - capped;
}

// Rayleigh phase function.
f32 phaseRayleigh(f32 cosTheta) { return 3.0f / (16.0f * kPi) * (1.0f + cosTheta * cosTheta); }

// Cornette-Shanks Mie phase function.
f32 phaseMie(f32 cosTheta, f32 g) {
    const f32 g2 = g * g;
    const f32 denom = 1.0f + g2 - 2.0f * g * cosTheta;
    const f32 d15 = denom * std::sqrt(denom > 1e-4f ? denom : 1e-4f);
    return 3.0f * (1.0f - g2) * (1.0f + cosTheta * cosTheta) /
           (8.0f * kPi * (2.0f + g2) * (d15 > 1e-6f ? d15 : 1e-6f));
}

// Distance from a point at radius r0 heading at cosZenith to the sphere of radius `rad`.
// Negative when the ray never reaches it.
f32 raySphere(f32 r0, f32 cosZenith, f32 rad, bool wantNear) {
    const f32 b = r0 * cosZenith;
    const f32 disc = b * b - (r0 * r0 - rad * rad);
    if (disc < 0.0f) return -1.0f;
    const f32 s = std::sqrt(disc);
    return wantNear ? -b - s : -b + s;
}

} // namespace

// exp(y*y) * erfc(y) for y >= 0, from Numerical Recipes' Chebyshev fit.
f32 atmoErfcx(f32 y) {
    const f32 z = y > 0.0f ? y : 0.0f;
    const f32 t = 2.0f / (2.0f + z);
    const f32 p = -1.26551223f + t * (1.00002368f + t * (0.37409196f + t * (0.09678418f +
                   t * (-0.18628806f + t * (0.27886807f + t * (-1.13520398f + t * (1.48851587f +
                   t * (-0.82215223f + t * 0.17087277f))))))));
    return t * std::exp(p);
}

// Chapman airmass: the slant column as a multiple of the vertical column at the same altitude.
f32 atmoChapman(f32 xr, f32 cosZenith) {
    const f32 half = std::sqrt(xr * 0.5f);
    const f32 up   = std::sqrt(kPi * xr * 0.5f);
    if (cosZenith >= 0.0f) return up * atmoErfcx(cosZenith * half);
    const f32 sinChi = sinFromCos(cosZenith);
    return 2.0f * std::sqrt(kPi * xr * sinChi * 0.5f) * std::exp(xr * (1.0f - sinChi)) -
           up * atmoErfcx(-cosZenith * half);
}

// Per-channel optical depth from an altitude out to space. Sets `hitsGround` when the planet blocks.
void atmoOpticalDepthToSpace(const AtmosphereProfile& a, f32 altitudeKm, f32 cosZenith,
                             f32 outTau[3], bool* hitsGround) {
    const f32 z = altitudeKm > 0.0f ? altitudeKm : 0.0f;
    const f32 r = a.planetRadiusKm + z;
    const bool blocked = cosZenith < 0.0f && r * sinFromCos(cosZenith) < a.planetRadiusKm;
    if (hitsGround) *hitsGround = blocked;
    if (blocked) { outTau[0] = outTau[1] = outTau[2] = 1e9f; return; }

    const f32 colR = a.rayleighScaleKm * std::exp(-z / a.rayleighScaleKm) *
                     atmoChapman(r / a.rayleighScaleKm, cosZenith);
    const f32 colM = a.mieScaleKm * std::exp(-z / a.mieScaleKm) *
                     atmoChapman(r / a.mieScaleKm, cosZenith);
    const f32 colO = ozoneColumnAbove(a, z) * ozoneAirmass(a, r, cosZenith);

    for (int i = 0; i < 3; ++i)
        outTau[i] = a.rayleighScatter[i] * colR + a.mieExtinction * colM + a.ozoneAbsorb[i] * colO;
}

// What the air leaves of the sun at an altitude. Zero in the planet's shadow, softened across the
// sun's own angular radius.
void atmoSunTransmittance(const AtmosphereProfile& a, f32 altitudeKm, f32 sunCosZenith,
                          f32 sunAngularRadiusRad, f32 outT[3]) {
    const f32 z = altitudeKm > 0.0f ? altitudeKm : 0.0f;
    const f32 ratio = a.planetRadiusKm / (a.planetRadiusKm + z);
    const f32 cosHorizon = -std::sqrt(sat(1.0f - ratio * ratio));
    const f32 halfWidth = std::sin(sunAngularRadiusRad) + 1e-5f;
    const f32 c = sat((sunCosZenith - (cosHorizon - halfWidth)) / (2.0f * halfWidth));
    const f32 shadow = c * c * (3.0f - 2.0f * c);
    if (shadow <= 0.0f) { outT[0] = outT[1] = outT[2] = 0.0f; return; }

    f32 tau[3];
    atmoOpticalDepthToSpace(a, z, sunCosZenith > cosHorizon ? sunCosZenith : cosHorizon, tau, nullptr);
    for (int i = 0; i < 3; ++i) outT[i] = std::exp(-tau[i]) * shadow;
}

// The scattering integral along one bounded segment: in-scattered radiance and transmittance out.
void atmoScatterSegment(const AtmosphereProfile& a, f32 r0,
                        f32 viewCosZenith, f32 sunCosZenith, f32 cosViewSun,
                        f32 spanKm, i32 steps, const f32 sunIrradiance[3], f32 sunAngularRadiusRad,
                        f32 outRgb[3], f32 outTransmittance[3]) {
    outRgb[0] = outRgb[1] = outRgb[2] = 0.0f;
    outTransmittance[0] = outTransmittance[1] = outTransmittance[2] = 1.0f;
    if (spanKm <= 0.0f) return;

    const i32 n = steps > 1 ? steps : 1;
    const f32 pR = phaseRayleigh(cosViewSun);
    const f32 pM = phaseMie(cosViewSun, a.miePhaseG);

    // Samples are placed as u^p with p = 1 + |cos|: uniform along the horizon, quadratic straight up.
    const f32 p = 1.0f + std::fabs(viewCosZenith);

    for (i32 i = 0; i < n; ++i) {
        const f32 lo = spanKm * std::pow(static_cast<f32>(i) / static_cast<f32>(n), p);
        const f32 hi = spanKm * std::pow(static_cast<f32>(i + 1) / static_cast<f32>(n), p);
        const f32 dt = hi - lo;
        if (dt <= 0.0f) continue;
        const f32 t = 0.5f * (lo + hi);

        const f32 r = std::sqrt(r0 * r0 + 2.0f * r0 * viewCosZenith * t + t * t);
        const f32 alt = r - a.planetRadiusKm > 0.0f ? r - a.planetRadiusKm : 0.0f;
        const f32 sunCosHere = (r0 * sunCosZenith + t * cosViewSun) / (r > 1e-4f ? r : 1e-4f);

        const f32 dR = std::exp(-alt / a.rayleighScaleKm);
        const f32 dM = std::exp(-alt / a.mieScaleKm);
        const f32 dO = ozoneDensity(a, alt);

        f32 sunT[3];
        atmoSunTransmittance(a, alt, sunCosHere, sunAngularRadiusRad, sunT);

        for (int c = 0; c < 3; ++c) {
            const f32 scatR = a.rayleighScatter[c] * dR;
            const f32 scatM = a.mieScatter * dM;
            const f32 ext = scatR + a.mieExtinction * dM + a.ozoneAbsorb[c] * dO;
            const f32 source = ((scatR * pR + scatM * pM) +
                                (scatR + scatM) * a.multiScatterGain / (4.0f * kPi)) *
                               sunT[c] * sunIrradiance[c];
            const f32 stepT = std::exp(-ext * dt);
            outRgb[c] += outTransmittance[c] * (ext > 1e-12f ? source * (1.0f - stepT) / ext
                                                             : source * dt);
            outTransmittance[c] *= stepT;
        }
    }
}

// Sky radiance along one view ray, with the lit ground added when the ray hits the planet.
void atmoSkyRadiance(const AtmosphereProfile& a, f32 altitudeKm,
                     f32 viewCosZenith, f32 sunCosZenith, f32 cosViewSun,
                     const f32 sunIrradiance[3], f32 sunAngularRadiusRad, f32 outRgb[3]) {
    outRgb[0] = outRgb[1] = outRgb[2] = 0.0f;

    const f32 rTop = a.planetRadiusKm + a.atmosphereHeightKm;
    const f32 r0   = a.planetRadiusKm + (altitudeKm > 0.0f ? altitudeKm : 0.0f);

    f32 tMax = raySphere(r0, viewCosZenith, rTop, false);
    if (tMax <= 0.0f) return;
    const f32 tGround = raySphere(r0, viewCosZenith, a.planetRadiusKm, true);
    const bool hitsGround = viewCosZenith < 0.0f && tGround > 0.0f;
    if (hitsGround) tMax = tGround;

    f32 transmit[3];
    atmoScatterSegment(a, r0, viewCosZenith, sunCosZenith, cosViewSun, tMax, a.viewSteps,
                       sunIrradiance, sunAngularRadiusRad, outRgb, transmit);
    if (!hitsGround) return;

    // The shader's averSkyPhysical deliberately omits this ground term; the scene brings its own.
    const f32 sunCosGround = clampf((r0 * sunCosZenith + tMax * cosViewSun) / a.planetRadiusKm, -1.0f, 1.0f);
    f32 groundT[3];
    atmoSunTransmittance(a, 0.0f, sunCosGround, sunAngularRadiusRad, groundT);
    const f32 ndl = sunCosGround > 0.0f ? sunCosGround : 0.0f;
    for (int c = 0; c < 3; ++c)
        outRgb[c] += transmit[c] * a.groundAlbedo / kPi * sunIrradiance[c] * groundT[c] * ndl;
}

// Aerial perspective: the same integral bounded by a surface `distanceKm` away.
void atmoAerialPerspective(const AtmosphereProfile& a, f32 altitudeKm,
                           f32 viewCosZenith, f32 sunCosZenith, f32 cosViewSun, f32 distanceKm,
                           const f32 sunIrradiance[3], f32 sunAngularRadiusRad,
                           f32 outInscatter[3], f32 outTransmittance[3]) {
    atmoScatterSegment(a, a.planetRadiusKm + (altitudeKm > 0.0f ? altitudeKm : 0.0f),
                       viewCosZenith, sunCosZenith, cosViewSun, distanceKm, a.aerialSteps,
                       sunIrradiance, sunAngularRadiusRad, outInscatter, outTransmittance);
}

// Fits the two-colour dome and its exponent to the model at this sun elevation.
void atmoFitDome(const AtmosphereProfile& a, f32 altitudeKm, f32 sunCosZenith,
                 const f32 sunIrradiance[3], f32 sunAngularRadiusRad, AtmosphereDome& out) {
    const f32 sunSin = sinFromCos(sunCosZenith);

    // Sky radiance at one view elevation, averaged over four azimuths.
    auto ring = [&](f32 viewCos, f32 rgb[3]) {
        const f32 viewSin = sinFromCos(viewCos);
        const f32 axial = sunCosZenith * viewCos;
        const f32 swing = sunSin * viewSin;
        const f32 cosines[4] = {axial + swing, axial, axial - swing, axial};
        rgb[0] = rgb[1] = rgb[2] = 0.0f;
        for (f32 cvs : cosines) {
            f32 s[3];
            atmoSkyRadiance(a, altitudeKm, viewCos, sunCosZenith, clampf(cvs, -1.0f, 1.0f),
                            sunIrradiance, sunAngularRadiusRad, s);
            for (int c = 0; c < 3; ++c) rgb[c] += s[c] * 0.25f;
        }
    };

    ring(1.0f, out.zenith);
    ring(0.0f, out.horizon);

    auto lum = [](const f32 c[3]) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; };

    // The exponent is fitted to cosine-weighted hemispherical irradiance, which is monotone in k.
    f32 modelE = 0.0f;
    const int kBands = 8;
    for (int i = 0; i < kBands; ++i) {
        const f32 z = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(kBands);
        f32 band[3];
        ring(z, band);
        modelE += 2.0f * lum(band) * z / static_cast<f32>(kBands);
    }

    // I(k) = 2 * integral over z in [0,1] of ((1+z)/2)^k * z, in closed form.
    auto domeI = [](f32 k) {
        const f32 hp = std::pow(0.5f, k + 1.0f);
        return 4.0f * (2.0f / (k + 2.0f) - 1.0f / (k + 1.0f) - hp / (k + 2.0f) + hp / (k + 1.0f));
    };

    const f32 lz = lum(out.zenith), lh = lum(out.horizon);
    out.exponent = 0.65f;
    if (std::fabs(lz - lh) > 1e-6f) {
        const f32 target = (modelE - lh) / (lz - lh);
        if (target > domeI(8.0f) && target < domeI(0.05f)) {
            f32 lo = 0.05f, hi = 8.0f;
            for (int it = 0; it < 24; ++it) {
                const f32 mid = 0.5f * (lo + hi);
                if (domeI(mid) > target) lo = mid; else hi = mid;
            }
            out.exponent = 0.5f * (lo + hi);
        } else {
            out.exponent = target >= domeI(0.05f) ? 0.05f : 8.0f;
        }
    }

    atmoSunTransmittance(a, altitudeKm, sunCosZenith, sunAngularRadiusRad, out.sunTransmittance);
}

// Mirrors RHIShaders.cpp's averFogInscatterRef() exactly: a zenith march and a level march toward
// the sun's own azimuth, blended 0.4 toward the second. Kept in lockstep with that function
// deliberately -- if its blend weight or step count ever changes, this must change with it, the same
// discipline every other function in this file already keeps with its own HLSL twin.
void atmoFogInscatterRef(const AtmosphereProfile& a, f32 altitudeKm, const f32 sunDir[3],
                         const f32 sunIrradiance[3], f32 sunAngularRadiusRad, f32 outRgb[3]) {
    const f32 len = std::sqrt(sunDir[0] * sunDir[0] + sunDir[1] * sunDir[1] + sunDir[2] * sunDir[2]);
    const f32 Lx = len > 1e-6f ? sunDir[0] / len : 0.0f;
    const f32 Ly = len > 1e-6f ? sunDir[1] / len : 0.0f;
    const f32 Lz = len > 1e-6f ? sunDir[2] / len : 1.0f;

    const f32 rTop = a.planetRadiusKm + a.atmosphereHeightKm;
    const f32 r0   = a.planetRadiusKm + (altitudeKm > 0.0f ? altitudeKm : 0.0f);

    f32 zenith[3], zenithT[3];
    atmoScatterSegment(a, r0, 1.0f, Lz, Lz, rTop - r0, a.viewSteps,
                       sunIrradiance, sunAngularRadiusRad, zenith, zenithT);

    const f32 lenLxy = std::sqrt(Lx * Lx + Ly * Ly);
    const f32 tMax    = raySphere(r0, 0.0f, rTop, false);
    f32 towardSun[3], towardSunT[3];
    atmoScatterSegment(a, r0, 0.0f, Lz, lenLxy, tMax > 0.0f ? tMax : 0.0f, a.viewSteps,
                       sunIrradiance, sunAngularRadiusRad, towardSun, towardSunT);

    for (int c = 0; c < 3; ++c) outRgb[c] = zenith[c] + (towardSun[c] - zenith[c]) * 0.4f;
}

} // namespace aver::rhi
