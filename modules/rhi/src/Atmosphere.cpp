// The atmosphere model declared in Atmosphere.hpp. Mirrored, function for function, by the HLSL in
// RHIShaders.cpp; the derivations and the accuracy budget are in docs/rendering/ATMOSPHERE.md.
#include "aver/rhi/Atmosphere.hpp"

#include <cmath>

namespace aver::rhi {

namespace {

constexpr f32 kPi = 3.14159265358979f;

f32 clampf(f32 v, f32 lo, f32 hi) { return v < lo ? lo : (v > hi ? hi : v); }
f32 sat(f32 v) { return clampf(v, 0.0f, 1.0f); }

// sin of an angle given its cosine, guarded against a cosine that drifted outside [-1,1].
f32 sinFromCos(f32 c) { return std::sqrt(sat(1.0f - c * c)); }

// Ozone lives in a tent centred on ozoneCentreKm; this is its unit-peak density at an altitude.
f32 ozoneDensity(const AtmosphereProfile& a, f32 altKm) {
    const f32 w = a.ozoneWidthKm > 1e-4f ? a.ozoneWidthKm : 1e-4f;
    const f32 d = std::fabs(altKm - a.ozoneCentreKm) / w;
    return d < 1.0f ? 1.0f - d : 0.0f;
}

// Vertical column of that tent from `altKm` upward, in kilometres. Piecewise-quadratic and exact.
f32 ozoneColumnAbove(const AtmosphereProfile& a, f32 altKm) {
    const f32 w = a.ozoneWidthKm > 1e-4f ? a.ozoneWidthKm : 1e-4f;
    const f32 c = a.ozoneCentreKm;
    if (altKm >= c + w) return 0.0f;
    if (altKm >= c)     { const f32 s = (c + w - altKm) / w; return w * s * s * 0.5f; }
    if (altKm >= c - w) { const f32 u = (c - altKm) / w;     return w * (u - u * u * 0.5f) + w * 0.5f; }
    return w;
}

// Slant factor for a thin shell at the ozone centre. The tent has no Chapman form, so the layer is
// treated as a shell and the divergence at tangency is capped by the chord a ray really travels.
f32 ozoneAirmass(const AtmosphereProfile& a, f32 rKm, f32 cosZenith) {
    const f32 w  = a.ozoneWidthKm > 1e-4f ? a.ozoneWidthKm : 1e-4f;
    const f32 ro = a.planetRadiusKm + a.ozoneCentreKm;
    const f32 s  = rKm * sinFromCos(cosZenith) / ro;
    const f32 maxAir = 1.2f * std::sqrt(2.0f * ro * w) / w;
    const f32 capped = clampf(1.0f / std::sqrt(1.0f - clampf(s * s, 0.0f, 0.999999f)), 1.0f, maxAir);
    // Downward and still escaping crosses the layer twice: the shape Chapman uses, applied here too.
    return cosZenith >= 0.0f ? capped : 2.0f * maxAir - capped;
}

f32 phaseRayleigh(f32 cosTheta) { return 3.0f / (16.0f * kPi) * (1.0f + cosTheta * cosTheta); }

// Cornette-Shanks: Henyey-Greenstein's forward lobe with the symmetry that keeps a Mie backscatter
// from collapsing to nothing, which is what makes the sky opposite the sun still read as air.
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

f32 atmoErfcx(f32 y) {
    const f32 z = y > 0.0f ? y : 0.0f;
    const f32 t = 2.0f / (2.0f + z);
    const f32 p = -1.26551223f + t * (1.00002368f + t * (0.37409196f + t * (0.09678418f +
                   t * (-0.18628806f + t * (0.27886807f + t * (-1.13520398f + t * (1.48851587f +
                   t * (-0.82215223f + t * 0.17087277f))))))));
    return t * std::exp(p);
}

f32 atmoChapman(f32 xr, f32 cosZenith) {
    const f32 half = std::sqrt(xr * 0.5f);
    const f32 up   = std::sqrt(kPi * xr * 0.5f);
    if (cosZenith >= 0.0f) return up * atmoErfcx(cosZenith * half);
    // Below the local horizon but still escaping: reflected about the tangent point. The exponential
    // is bounded because the caller only reaches this branch when the ray misses the planet, which
    // caps xr*(1-sinChi) at the altitude measured in scale heights.
    const f32 sinChi = sinFromCos(cosZenith);
    return 2.0f * std::sqrt(kPi * xr * sinChi * 0.5f) * std::exp(xr * (1.0f - sinChi)) -
           up * atmoErfcx(-cosZenith * half);
}

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

void atmoSunTransmittance(const AtmosphereProfile& a, f32 altitudeKm, f32 sunCosZenith,
                          f32 sunAngularRadiusRad, f32 outT[3]) {
    const f32 z = altitudeKm > 0.0f ? altitudeKm : 0.0f;
    const f32 ratio = a.planetRadiusKm / (a.planetRadiusKm + z);
    // How far below level the sun still clears the planet, which is not zero once you have altitude.
    const f32 cosHorizon = -std::sqrt(sat(1.0f - ratio * ratio));
    // Softened across the sun's own angular radius rather than switched: a point sun puts a hard
    // edge on every marched ray and the terminator then bands.
    const f32 halfWidth = std::sin(sunAngularRadiusRad) + 1e-5f;
    const f32 c = sat((sunCosZenith - (cosHorizon - halfWidth)) / (2.0f * halfWidth));
    const f32 shadow = c * c * (3.0f - 2.0f * c);
    if (shadow <= 0.0f) { outT[0] = outT[1] = outT[2] = 0.0f; return; }

    f32 tau[3];
    atmoOpticalDepthToSpace(a, z, sunCosZenith > cosHorizon ? sunCosZenith : cosHorizon, tau, nullptr);
    for (int i = 0; i < 3; ++i) outT[i] = std::exp(-tau[i]) * shadow;
}

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

    // The multiple-scattering term below is sigma_s times a SPECTRALLY FLAT field, and that is a
    // measured choice rather than a lazy one.
    //
    // Second-order scattering formally goes as sigma_s squared, so shaping the field by the column's
    // own scattering optical depth looks more principled. It was tried and it is wrong here: it puts
    // the zenith at a blue/red radiance ratio of 7.2 where a real clear zenith is 2.9 to 4, and it
    // needs the gain at 3.7 to still carry the right fill light. Flat lands the ratio at 3.7 AND the
    // fill light at a gain of 1.7. Two independent measurements agree on flat; AtmosphereTest holds
    // both, so this cannot be quietly "improved" back.
    //
    // The reason is saturation: blue is optically thick, so its multiply-scattered field does not
    // keep growing with sigma_s the way a thin-medium argument says it should.

    // WHERE THE SAMPLES GO, which at these step counts matters more than how many there are.
    //
    // Density falls exponentially with altitude, so uniform steps spend most of their samples where
    // there is nothing and skip the part that carries the light. How badly depends on the ray: a
    // vertical one climbs a scale height in 8 km, a horizontal one takes 300, and at 32 uniform
    // steps the vertical case was 7% off its own converged answer while the horizontal one was fine.
    // Sampling t as u^p with p = 1 + |cos| interpolates between the two: uniform along the horizon,
    // quadratic straight up.
    const f32 p = 1.0f + std::fabs(viewCosZenith);

    for (i32 i = 0; i < n; ++i) {
        const f32 lo = spanKm * std::pow(static_cast<f32>(i) / static_cast<f32>(n), p);
        const f32 hi = spanKm * std::pow(static_cast<f32>(i + 1) / static_cast<f32>(n), p);
        const f32 dt = hi - lo;
        if (dt <= 0.0f) continue;
        const f32 t = 0.5f * (lo + hi);

        const f32 r = std::sqrt(r0 * r0 + 2.0f * r0 * viewCosZenith * t + t * t);
        const f32 alt = r - a.planetRadiusKm > 0.0f ? r - a.planetRadiusKm : 0.0f;
        // The sun's zenith angle where the sample IS, from the two cosines at the viewer: the
        // sample's up is P/|P|, and P is the viewer plus t along the view ray.
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
            // Single scattering, plus one isotropic term standing in for every further bounce.
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

    // The lit ground closes the ray, so looking down is the planet seen through its own air rather
    // than a hole in the dome.
    //
    // THE SHADER'S averSkyPhysical DELIBERATELY DOES NOT DO THIS, and the difference is not a
    // divergence to repair. This is the model on its own, which is what atmoFitDome samples and
    // what AtmosphereTest checks. The shader draws into a scene that brings its OWN ground, so a
    // second horizon from this sphere would float above the floor as a hard-edged false one.
    const f32 sunCosGround = clampf((r0 * sunCosZenith + tMax * cosViewSun) / a.planetRadiusKm, -1.0f, 1.0f);
    f32 groundT[3];
    atmoSunTransmittance(a, 0.0f, sunCosGround, sunAngularRadiusRad, groundT);
    const f32 ndl = sunCosGround > 0.0f ? sunCosGround : 0.0f;
    for (int c = 0; c < 3; ++c)
        outRgb[c] += transmit[c] * a.groundAlbedo / kPi * sunIrradiance[c] * groundT[c] * ndl;
}

void atmoAerialPerspective(const AtmosphereProfile& a, f32 altitudeKm,
                           f32 viewCosZenith, f32 sunCosZenith, f32 cosViewSun, f32 distanceKm,
                           const f32 sunIrradiance[3], f32 sunAngularRadiusRad,
                           f32 outInscatter[3], f32 outTransmittance[3]) {
    atmoScatterSegment(a, a.planetRadiusKm + (altitudeKm > 0.0f ? altitudeKm : 0.0f),
                       viewCosZenith, sunCosZenith, cosViewSun, distanceKm, a.aerialSteps,
                       sunIrradiance, sunAngularRadiusRad, outInscatter, outTransmittance);
}

void atmoFitDome(const AtmosphereProfile& a, f32 altitudeKm, f32 sunCosZenith,
                 const f32 sunIrradiance[3], f32 sunAngularRadiusRad, AtmosphereDome& out) {
    const f32 sunSin = sinFromCos(sunCosZenith);

    // Averaged over four azimuths, because a two-colour dome has no azimuth and the sunward horizon
    // differs from the one opposite it by more than the horizon differs from the zenith.
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

    // The exponent is chosen so the dome delivers the RIGHT AMOUNT OF FILL LIGHT, not so it passes
    // through one more sample of the sky.
    //
    // Matching a 45-degree sample was the obvious thing and it was wrong: with the sun high the
    // forward-scattered ring is brighter than both anchors, so no power curve passes through all
    // three and the fit falls back. Sweeping the sun then walked the exponent 0.22 -> 2.46
    // non-monotonically, and since this same curve is what every ambient and every environment
    // reflection reads, the fill light would visibly breathe as the sun moved.
    //
    // Cosine-weighted hemispherical irradiance is monotone in k, has a closed form, and is the one
    // property the dome exists to carry. E/pi for an up-facing surface is 2 * integral of L(z)*z.
    f32 modelE = 0.0f;
    const int kBands = 8;
    for (int i = 0; i < kBands; ++i) {
        const f32 z = (static_cast<f32>(i) + 0.5f) / static_cast<f32>(kBands);
        f32 band[3];
        ring(z, band);
        modelE += 2.0f * lum(band) * z / static_cast<f32>(kBands);
    }

    // I(k) = 2 * integral over z in [0,1] of ((1+z)/2)^k * z, in closed form. It falls from 1 at
    // k = 0 (a dome that is all zenith colour) toward 0, so a bisection cannot miss.
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

} // namespace aver::rhi
