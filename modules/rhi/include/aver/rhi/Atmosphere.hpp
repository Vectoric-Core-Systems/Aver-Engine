// Physically-based sky: Rayleigh + Mie + ozone single scattering against a spherical shell.
// The HLSL in RHIShaders.cpp mirrors this file function for function.
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

    f32 multiScatterGain = 1.70f;   // isotropic stand-in for multiple scattering and ground bounce

    f32 groundAlbedo = 0.22f;

    i32 viewSteps = 32;    // steps in the view-ray integral
    i32 aerialSteps = 4;   // steps for aerial perspective
};

// exp(y*y) * erfc(y) for y >= 0.
f32 atmoErfcx(f32 y);

// Chapman airmass: the slant column along a ray as a multiple of the vertical column at the same
// altitude. `xr` is radius/scaleHeight, `cosZenith` the ray's cosine against local up.
f32 atmoChapman(f32 xr, f32 cosZenith);

// Per-channel optical depth from `altitudeKm` along `cosZenith` out to space. Sets `hitsGround`
// when the ray meets the planet first, in which case the depth is meaningless.
void atmoOpticalDepthToSpace(const AtmosphereProfile& a, f32 altitudeKm, f32 cosZenith,
                             f32 outTau[3], bool* hitsGround = nullptr);

// Transmittance of the whole air column between `altitudeKm` and the sun.
void atmoSunTransmittance(const AtmosphereProfile& a, f32 altitudeKm, f32 sunCosZenith,
                          f32 sunAngularRadiusRad, f32 outT[3]);

// The scattering integral along one bounded segment: in-scattered radiance out, transmittance out.
// `r0` is the viewer's radius from the planet centre.
void atmoScatterSegment(const AtmosphereProfile& a, f32 r0,
                        f32 viewCosZenith, f32 sunCosZenith, f32 cosViewSun,
                        f32 spanKm, i32 steps, const f32 sunIrradiance[3], f32 sunAngularRadiusRad,
                        f32 outRgb[3], f32 outTransmittance[3]);

// Sky radiance along one view ray for a viewer `altitudeKm` above the ground, with the lit ground
// added when the ray hits it. Result is in the units of `sunIrradiance`.
void atmoSkyRadiance(const AtmosphereProfile& a, f32 altitudeKm,
                     f32 viewCosZenith, f32 sunCosZenith, f32 cosViewSun,
                     const f32 sunIrradiance[3], f32 sunAngularRadiusRad, f32 outRgb[3]);

// Aerial perspective: the same segment bounded by a surface `distanceKm` away. A shaded pixel gets
// `surface * transmittance + inscatter`.
void atmoAerialPerspective(const AtmosphereProfile& a, f32 altitudeKm,
                           f32 viewCosZenith, f32 sunCosZenith, f32 cosViewSun, f32 distanceKm,
                           const f32 sunIrradiance[3], f32 sunAngularRadiusRad,
                           f32 outInscatter[3], f32 outTransmittance[3]);

// The two-colour dome plus its blend exponent, fitted to the model so cheap consumers of the sky
// need not march per pixel. All three colours are LINEAR radiance; the caller sRGB-encodes.
struct AtmosphereDome {
    f32 zenith[3];            // radiance straight up
    f32 horizon[3];           // radiance at the horizon, averaged over azimuth
    f32 exponent;             // the k in pow(dir.z*0.5+0.5, k)
    f32 sunTransmittance[3];  // what the air leaves of the DIRECT sun
};

// Fits the two-colour dome and its exponent to the model at this sun elevation.
void atmoFitDome(const AtmosphereProfile& a, f32 altitudeKm, f32 sunCosZenith,
                 const f32 sunIrradiance[3], f32 sunAngularRadiusRad, AtmosphereDome& out);

} // namespace aver::rhi
