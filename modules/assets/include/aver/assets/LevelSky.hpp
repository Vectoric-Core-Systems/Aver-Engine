#pragma once
// ONE MAPPING FROM A LEVEL'S WEATHER TO A LIVE ATMOSPHERE, shared by both hosts.
//
// The editor and the shipped game each used to carry their own copy of this -- SandboxApp's
// applyLevelSky and GameApp's applyLevelSky, written months apart, agreeing on five fields and
// silently disagreeing about the rest. That arrangement has a specific failure mode and it kept
// happening: a field is added to .ocworld, wired into the editor's Details panel, and the game
// never reads it, so the level looks one way while you author it and another way when you run it.
// Nothing catches that, because both halves compile and both halves are self-consistent.
//
// It lives in Aver.Assets.Gpu because that target exists for exactly this shape and says so: "the
// decode-to-GPU join", Aver.Formats plus the generic Aver.RHI, kept separate so Aver.Assets can
// stay a leaf. TextureUpload turns a decoded image into a GPU resource; this turns a parsed level
// record into a live atmosphere. Same direction, same pair of dependencies, no new edge.
//
// TWO HOMES WERE REJECTED. Aver.World -- the module that already shares level instantiation between
// the two hosts -- refuses Aver.Render.PBR in its own CMakeLists on the grounds that it "only tells
// them when to do it", and an RHI dependency would be the same mistake one module along.
// Aver.Render.Voxi.Renderer links both and owns the sky, but AVER_MODULE_VOXI is genuinely
// switchable (the root CMakeLists forces it OFF when PBR is off), and a build without it still has
// an rhi::SkyAtmosphere to fill -- so putting the mapping there would have made a level's weather
// silently stop applying in a configuration that still renders one.
//
// WHAT THE CALLER STILL OWNS. Both hosts mirror a handful of these into their own members and copy
// them back into the atmosphere every frame -- the editor because its Details sliders bind to them,
// the game because it has no sliders but inherited the shape. So a host that keeps mirrors must
// RESEED THEM from the atmosphere after calling this, or the next frame overwrites what the level
// just said. applyLevelEnv deliberately does not know which fields those are; it writes the
// atmosphere and leaves the host's own bookkeeping to the host.

#include "aver/core/Types.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/rhi/RHI.hpp"

#include <algorithm>
#include <cmath>

namespace aver::assets {

// Applies a level's SUN, SKY, FOG and CLOUDS records to `sky`.
//
// Each record is applied only when the level actually carried it, so a level with no SKY line is
// left with whatever atmosphere the host had -- which is what every level written before these
// records existed depends on.
// The fog density that makes a level view from height `viewZ` reach `opacity` at `distanceCm`, under
// `sky`'s fog start, height falloff and max opacity (averFogFactor solved backwards). The falloff
// boost is bounded (20x) so a camera high above the fog does not drown the ground. 0 when no density
// can (the distance is inside the fog start).
inline f32 fogDensityReaching(const rhi::SkyAtmosphere& sky, f32 distanceCm, f32 opacity, f32 viewZ) {
    const f32 seg = distanceCm - sky.fogStart;
    if (seg <= 1.0f) return 0.0f;
    const f32 maxOpacity = sky.fogMaxOpacity > 0.01f ? sky.fogMaxOpacity : 0.01f;
    const f32 t = std::clamp(opacity / maxOpacity, 0.01f, 0.999f);
    const f32 atView = sky.fogFalloff > 0.0f
        ? std::max(std::exp(-(viewZ - sky.fogHeight) * sky.fogFalloff), 0.05f) : 1.0f;
    return -std::log(1.0f - t) / (seg * atView);
}

// A streamed level's fog over its load edge (OcStreamSettings::fogCells), or `authored` when it asks
// for none. Never thinner than the authored fog.
inline f32 fogForStreamEdge(const fmt::OcStreamSettings& st, const rhi::SkyAtmosphere& sky, f32 authored,
                            f32 viewZ) {
    if (!st.enabled || st.fogCells <= 0.0f) return authored;
    const f32 edge = st.loadCm - st.fogCells * st.cellCm;
    const f32 d = fogDensityReaching(sky, edge, st.fogOpacity, viewZ);
    return d > authored ? d : authored;
}

inline void applyLevelEnv(const fmt::OcWorldEnv& w, rhi::SkyAtmosphere& sky) {
    if (w.hasSun) {
        for (int i = 0; i < 3; ++i) {
            sky.sunDirection[i] = static_cast<f32>(w.sunDir[i]);
            sky.sunColor[i]     = static_cast<f32>(w.sunColor[i]);
        }
        // The divisor makes the two defaults agree (OcWorldEnv's 100000 lux against
        // SkyAtmosphere's intensity of 3.0) rather than inventing a constant, so a level that
        // omits SUN renders at exactly the brightness it did before the record was read at all.
        sky.sunIntensity          = static_cast<f32>(w.sunLux / (100000.0 / 3.0));
        sky.sunTemperatureK       = static_cast<f32>(w.sunTemperatureK);
        sky.sunAngularDiameterDeg = static_cast<f32>(w.sunAngularDeg);
    }

    if (w.hasSky) {
        sky.model = w.skyPhysical ? rhi::SkyModel::Physical : rhi::SkyModel::Authored;
        for (int i = 0; i < 3; ++i) {
            sky.zenith[i]       = static_cast<f32>(w.skyZenith[i]);
            sky.horizon[i]      = static_cast<f32>(w.skyHorizon[i]);
            sky.groundAlbedo[i] = static_cast<f32>(w.skyGroundAlbedo[i]);
        }
        sky.atmosphereHeight  = static_cast<f32>(w.skyDomeExponent);
        sky.groundBlend       = static_cast<f32>(w.skyGroundBlend);
        sky.skyLightIntensity = static_cast<f32>(w.skyLight);
        // THE AIR IS THE ONE PLACE A SENTINEL SURVIVES, and it is guarded here as well as in the
        // writer: every one of these is a strictly positive physical quantity, so a negative means
        // the level declined to override and the engine's calibrated profile must stand. Assigning
        // a -1 straight through would put a negative extinction into the scattering integral, and
        // acesTonemap of a negative reads BRIGHT rather than dark, so it would not even look wrong.
        if (w.skyMieScatter    >= 0.0) sky.air.mieScatter         = static_cast<f32>(w.skyMieScatter);
        if (w.skyMieExtinction >= 0.0) sky.air.mieExtinction      = static_cast<f32>(w.skyMieExtinction);
        if (w.skyMiePhaseG     >= 0.0) sky.air.miePhaseG          = static_cast<f32>(w.skyMiePhaseG);
        if (w.skyRayleighKm    >= 0.0) sky.air.rayleighScaleKm    = static_cast<f32>(w.skyRayleighKm);
        if (w.skyMieKm         >= 0.0) sky.air.mieScaleKm         = static_cast<f32>(w.skyMieKm);
        if (w.skyPlanetKm      >= 0.0) sky.air.planetRadiusKm     = static_cast<f32>(w.skyPlanetKm);
        if (w.skyAirDepthKm    >= 0.0) sky.air.atmosphereHeightKm = static_cast<f32>(w.skyAirDepthKm);
        if (w.skyMultiScatter  >= 0.0) sky.air.multiScatterGain   = static_cast<f32>(w.skyMultiScatter);
        if (w.skyViewSteps     >  0)   sky.air.viewSteps          = w.skyViewSteps;
        if (w.skyAerialSteps   >  0)   sky.air.aerialSteps        = w.skyAerialSteps;
    }

    if (w.hasFog) {
        for (int i = 0; i < 3; ++i) sky.fogColor[i] = static_cast<f32>(w.fogColor[i]);
        sky.fogDensity    = static_cast<f32>(w.fogDensity);
        sky.fogFalloff    = static_cast<f32>(w.fogFalloff);
        sky.fogHeight     = static_cast<f32>(w.fogHeight);
        sky.fogStart      = static_cast<f32>(w.fogStart);
        sky.fogMaxOpacity = static_cast<f32>(w.fogMaxOpacity);
    }

    if (w.hasClouds) {
        sky.cloudsEnabled = w.cloudsEnabled;
        sky.cloudCoverage = static_cast<f32>(w.cloudCoverage);
        sky.cloudDensity  = static_cast<f32>(w.cloudDensity);
        sky.cloudBottom   = static_cast<f32>(w.cloudBottom);
        sky.cloudTop      = static_cast<f32>(w.cloudTop);
        // AUTHORED AS A WIDTH, STORED AS ITS RECIPROCAL. The Details panel edits "Feature Size" in
        // world units because that is the quantity a person can picture; SkyAtmosphere keeps
        // 1/width because that is what the noise lookup multiplies by. The file follows the panel.
        sky.cloudScale    = w.cloudFeatureSize > 1.0 ? static_cast<f32>(1.0 / w.cloudFeatureSize) : 0.00002f;
        sky.cloudWind[0]  = static_cast<f32>(w.cloudWind[0]);
        sky.cloudWind[1]  = static_cast<f32>(w.cloudWind[1]);
    }
}

// The inverse, for a host writing a level back out. Fills only the records `w` already claims to
// have, so a level that never carried a CLOUDS line does not grow one just by being saved -- see
// OcWorldEnv::hasClouds for why "off" and "no opinion" have to stay distinguishable.
inline void captureLevelEnv(const rhi::SkyAtmosphere& sky, fmt::OcWorldEnv& w) {
    if (w.hasSun) {
        for (int i = 0; i < 3; ++i) {
            w.sunDir[i]   = static_cast<f64>(sky.sunDirection[i]);
            w.sunColor[i] = static_cast<f64>(sky.sunColor[i]);
        }
        w.sunLux          = static_cast<f64>(sky.sunIntensity) * (100000.0 / 3.0);
        w.sunTemperatureK = static_cast<f64>(sky.sunTemperatureK);
        w.sunAngularDeg   = static_cast<f64>(sky.sunAngularDiameterDeg);
    }

    if (w.hasSky) {
        w.skyPhysical = sky.model == rhi::SkyModel::Physical;
        for (int i = 0; i < 3; ++i) {
            w.skyZenith[i]       = static_cast<f64>(sky.zenith[i]);
            w.skyHorizon[i]      = static_cast<f64>(sky.horizon[i]);
            w.skyGroundAlbedo[i] = static_cast<f64>(sky.groundAlbedo[i]);
        }
        w.skyDomeExponent = static_cast<f64>(sky.atmosphereHeight);
        w.skyGroundBlend  = static_cast<f64>(sky.groundBlend);
        w.skyLight        = static_cast<f64>(sky.skyLightIntensity);
        // WRITTEN AS REAL VALUES, never back as the -1 they may have been read as. Once a level
        // carries a SKY record the editor has shown these on screen and the author has had the
        // chance to move them, so what is live IS the authored value; keeping the sentinel here
        // would mean an edit to Mie Scatter on a level that never overrode it was discarded.
        w.skyMieScatter    = static_cast<f64>(sky.air.mieScatter);
        w.skyMieExtinction = static_cast<f64>(sky.air.mieExtinction);
        w.skyMiePhaseG     = static_cast<f64>(sky.air.miePhaseG);
        w.skyRayleighKm    = static_cast<f64>(sky.air.rayleighScaleKm);
        w.skyMieKm         = static_cast<f64>(sky.air.mieScaleKm);
        w.skyPlanetKm      = static_cast<f64>(sky.air.planetRadiusKm);
        w.skyAirDepthKm    = static_cast<f64>(sky.air.atmosphereHeightKm);
        w.skyMultiScatter  = static_cast<f64>(sky.air.multiScatterGain);
        w.skyViewSteps     = sky.air.viewSteps;
        w.skyAerialSteps   = sky.air.aerialSteps;
    }

    if (w.hasFog) {
        for (int i = 0; i < 3; ++i) w.fogColor[i] = static_cast<f64>(sky.fogColor[i]);
        w.fogDensity    = static_cast<f64>(sky.fogDensity);
        w.fogFalloff    = static_cast<f64>(sky.fogFalloff);
        w.fogHeight     = static_cast<f64>(sky.fogHeight);
        w.fogStart      = static_cast<f64>(sky.fogStart);
        w.fogMaxOpacity = static_cast<f64>(sky.fogMaxOpacity);
    }

    if (w.hasClouds) {
        w.cloudsEnabled    = sky.cloudsEnabled;
        w.cloudCoverage    = static_cast<f64>(sky.cloudCoverage);
        w.cloudDensity     = static_cast<f64>(sky.cloudDensity);
        w.cloudBottom      = static_cast<f64>(sky.cloudBottom);
        w.cloudTop         = static_cast<f64>(sky.cloudTop);
        w.cloudFeatureSize = sky.cloudScale > 1e-9f ? 1.0 / static_cast<f64>(sky.cloudScale) : 50000.0;
        w.cloudWind[0]     = static_cast<f64>(sky.cloudWind[0]);
        w.cloudWind[1]     = static_cast<f64>(sky.cloudWind[1]);
    }
}

}   // namespace aver::assets
