// Physically-based sky: Rayleigh + Mie + ozone single scattering against a spherical shell.
// Derivations, coefficient sources and the accuracy budget are in docs/rendering/ATMOSPHERE.md.
// The HLSL in RHIShaders.cpp mirrors this file function for function and has no compiler behind it.
#pragma once

#include "aver/core/Types.hpp"

namespace aver::rhi {

// The medium itself, in kilometres and per-kilometre coefficients. Defaults are Earth.
struct AtmosphereProfile {
    f32 planetRadiusKm     = 6360.0f;
    f32 atmosphereHeightKm = 60.0f;

    f32 rayleighScatter[3] = {5.802e-3f, 13.558e-3f, 33.100e-3f};
    f32 rayleighScaleKm    = 8.0f;

    f32 mieScatter    = 3.996e-3f;
    f32 mieExtinction = 4.440e-3f;
    f32 mieScaleKm    = 1.2f;
    f32 miePhaseG     = 0.80f;

    f32 ozoneAbsorb[3] = {0.650e-3f, 1.881e-3f, 0.085e-3f};
    f32 ozoneCentreKm  = 25.0f;
    f32 ozoneWidthKm   = 15.0f;

    // Isotropic stand-in for the light that scatters more than once, and for the ground bouncing
    // sunlight back into the air. It is a FIT, not a derivation, and the number comes from a
    // measurement: AtmosphereTest holds it to a clear sky's diffuse/direct illuminance ratio of
    // 15-30%. At zero -- single scattering alone -- a clear sky is about half as bright as reality.
    f32 multiScatterGain = 1.70f;

    // Fraction of the sun's irradiance the ground returns into the sky, as one grey number.
    f32 groundAlbedo = 0.22f;

    // Steps in the view-ray integral. The sun ray is analytic, so this is the whole cost.
    i32 viewSteps = 32;
    // Steps for aerial perspective, which runs per SCENE pixel over a segment kilometres long
    // rather than hundreds, so it needs far fewer.
    i32 aerialSteps = 4;
};

// exp(y*y) * erfc(y) for y >= 0. Numerical Recipes' Chebyshev fit, rearranged so the exp(-y*y)
// it carries cancels: one exp and ten multiply-adds, fractional error below 1.2e-7.
f32 atmoErfcx(f32 y);

// Chapman airmass: the slant column along a ray, as a multiple of the vertical column at the same
// altitude. `xr` is radius/scaleHeight, `cosZenith` the ray's cosine against local up.
f32 atmoChapman(f32 xr, f32 cosZenith);

// Optical depth from `altitudeKm` along `cosZenith` out to space. Returns per-channel extinction.
// `hitsGround` is set when the ray meets the planet first, in which case the depth is meaningless.
void atmoOpticalDepthToSpace(const AtmosphereProfile& a, f32 altitudeKm, f32 cosZenith,
                             f32 outTau[3], bool* hitsGround = nullptr);

// Transmittance of the whole air column between `altitudeKm` and the sun. Zero in the planet's
// shadow, softened across `sunAngularRadiusRad` because the sun is a disk and not a point.
void atmoSunTransmittance(const AtmosphereProfile& a, f32 altitudeKm, f32 sunCosZenith,
                          f32 sunAngularRadiusRad, f32 outT[3]);

// The scattering integral along one bounded segment: in-scattered radiance out, transmittance out.
// The SAME function serves the sky and aerial perspective, which is what makes the two agree by
// construction. `r0` is the viewer's radius from the planet centre.
void atmoScatterSegment(const AtmosphereProfile& a, f32 r0,
                        f32 viewCosZenith, f32 sunCosZenith, f32 cosViewSun,
                        f32 spanKm, i32 steps, const f32 sunIrradiance[3], f32 sunAngularRadiusRad,
                        f32 outRgb[3], f32 outTransmittance[3]);

// Sky radiance along one view ray, for a viewer `altitudeKm` above the ground: the segment above,
// bounded by the atmosphere's outer shell or by the ground, with the lit ground added when hit.
// `sunIrradiance` is the sun's colour x intensity and the result is in those units.
void atmoSkyRadiance(const AtmosphereProfile& a, f32 altitudeKm,
                     f32 viewCosZenith, f32 sunCosZenith, f32 cosViewSun,
                     const f32 sunIrradiance[3], f32 sunAngularRadiusRad, f32 outRgb[3]);

// Aerial perspective: the same segment bounded by a SURFACE `distanceKm` away instead of by the
// sky. What a shaded pixel gets is `surface * transmittance + inscatter`.
void atmoAerialPerspective(const AtmosphereProfile& a, f32 altitudeKm,
                           f32 viewCosZenith, f32 sunCosZenith, f32 cosViewSun, f32 distanceKm,
                           const f32 sunIrradiance[3], f32 sunAngularRadiusRad,
                           f32 outInscatter[3], f32 outTransmittance[3]);

// The two-colour dome plus its blend exponent, fitted to the model so every cheap consumer of the
// sky -- the ambient term, environment reflections, the fog in-scatter target, the cloud fill --
// reads a physical sky without marching one per pixel. All three colours are LINEAR radiance; the
// constant-buffer fields they feed are sRGB-encoded, so the caller encodes.
struct AtmosphereDome {
    f32 zenith[3];            // radiance straight up
    f32 horizon[3];           // radiance at the horizon, averaged over azimuth
    f32 exponent;             // the k in pow(dir.z*0.5+0.5, k) that reproduces the 45-degree sample
    f32 sunTransmittance[3];  // what the air leaves of the DIRECT sun; multiplies the authored colour
};

void atmoFitDome(const AtmosphereProfile& a, f32 altitudeKm, f32 sunCosZenith,
                 const f32 sunIrradiance[3], f32 sunAngularRadiusRad, AtmosphereDome& out);

} // namespace aver::rhi
