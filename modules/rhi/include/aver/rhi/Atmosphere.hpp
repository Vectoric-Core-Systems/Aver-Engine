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

    // WAS 3.996e-3 / 4.440e-3. Mie is nearly grey (unlike Rayleigh, which is what makes the sky blue
    // in the first place), so every bit of it dilutes that blue toward white -- less of it, at the
    // same ratio of scatter to extinction the old values held (roughly 0.9), is a purer, more
    // saturated sky for the same reason a clearer day reads bluer than a hazy one. Bounded by
    // AtmosphereTest's own "measured clear sky" calibration, not picked freehand: that suite hard-
    // checks the zenith blue/red ratio (2.5-5) and the diffuse/direct ratio (15-30%) against real-sky
    // references, and this is as far toward blue as both stay satisfied together.
    f32 mieScatter    = 2.8e-3f;
    f32 mieExtinction = 3.1e-3f;
    f32 mieScaleKm    = 1.2f;
    f32 miePhaseG     = 0.80f;

    f32 ozoneAbsorb[3] = {0.650e-3f, 1.881e-3f, 0.085e-3f};
    f32 ozoneCentreKm  = 25.0f;
    f32 ozoneWidthKm   = 15.0f;

    // WAS 1.70. Same calibration ceiling as the Mie values above: 2.0 already fails AtmosphereTest's
    // 30-degree diffuse/direct check (30.1%, over the 30% bound the measured-sky reference sets), so
    // 1.90 is the richest multi-scatter fill this profile can claim while still matching that
    // reference, not a number chosen for its own sake.
    f32 multiScatterGain = 1.90f;   // isotropic stand-in for multiple scattering and ground bounce

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

// Nine L2 spherical-harmonic coefficients of the sky, in the same LINEAR radiance units
// atmoSkyRadiance returns. Order is the usual one: 00, 1-1, 10, 11, 2-2, 2-1, 20, 21, 22.
//
// WHY THE DOME ABOVE CANNOT DO THIS JOB. AtmosphereDome::horizon is documented as "averaged
// over azimuth", and that is not an implementation detail -- a two-colour dome blended by
// dir.z is azimuthally symmetric BY CONSTRUCTION. No lookup into it can tell a wall facing
// the rising sun from one facing away, which is exactly the difference the ambient term was
// throwing away. Nine coefficients can, at about twenty ALU and no texture.
struct AtmosphereSkySH { f32 c[9][3]; };
void atmoSkyRadianceSH(const AtmosphereProfile& a, f32 altitudeKm, const f32 sunDir[3],
                       const f32 sunIrradiance[3], f32 sunAngularRadiusRad, AtmosphereSkySH& out);

// The fog/dome-veil reference colour: ambient (straight up) blended with a horizontal march toward
// the sun's own azimuth. Mirrors RHIShaders.cpp's averFogInscatterRef() function for function -- see
// its own comment for why two terms and why that blend. `sunDir` need not be normalised.
//
// PROVABLY FRAME-CONSTANT: no view direction, no world position enters this calculation anywhere,
// only the sun direction, the profile and the viewer's altitude -- all already fixed for the whole
// frame by the time any pixel shades. That is exactly why it is computed HERE, once, instead of in
// the shader per pixel: the GPU side used to pay for this same answer up to three times per pixel
// (once for fog directly, up to twice more inside averSkyPhysical's ground veil), a real, measured
// cost this function exists to remove without changing a single rendered value.
void atmoFogInscatterRef(const AtmosphereProfile& a, f32 altitudeKm, const f32 sunDir[3],
                         const f32 sunIrradiance[3], f32 sunAngularRadiusRad, f32 outRgb[3]);

} // namespace aver::rhi
