#pragma once
// The underwater half of Aver.Water: a pure function that swaps an authored rhi::SkyAtmosphere's
// fog parameters for underwater ones as the camera crosses the water surface, with a short blend
// band so the swap does not pop.
//
// This file's ONLY dependency on modules/rhi is rhi::SkyAtmosphere itself, taken by const-ref in
// and returned by value -- never an IDevice, never a texture or pipeline handle. That is deliberate:
// the whole job here is "read six fog fields, write six fog fields", and doing that against the
// struct rather than against a device is what let the water CMakeLists put UnderwaterFogTest on a
// target that links Aver.RHI for the struct definition alone, with no GPU, no window, and no
// device involved anywhere in the test binary.
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

namespace aver::water {

// The underwater look this slice ships with. Every numeric default here is an UNVALIDATED STARTING
// POINT, not a measured value -- there is no equivalent of the furnace/SH-integral oracles the sky
// and GI code got, because there is nothing to validate a "looks murky" target against except an
// eyeball once the surface is actually rendering. The postfx scout's own risk note is the reason
// for this warning: averFogFactor/averFogInscatter (RHIShaders.cpp) were tuned and measured only at
// kilometre-scale outdoor fog distances, and a few-metres-of-visibility underwater look is a
// genuinely different magnitude of the same knobs, not a smaller version of the same problem. Treat
// every field below as a slider to move once this is on screen, not as a claim about how water looks.
struct UnderwaterFogTuning {
    f32 color[3]     = {0.02f, 0.09f, 0.11f};   // a dark teal-black; centimetre-scale, NOT measured
    f32 densityPerCm = 6e-4f;                    // extinction per cm; NOT measured
    f32 falloff      = 0.0f;                     // 0 = uniform with depth; NOT measured
    f32 startCm      = 0.0f;                     // fog begins right at the camera; NOT measured
    f32 maxOpacity   = 1.0f;                     // fog can fully hide what is behind it
    f32 fadeBandCm   = 60.0f;                    // half-width of the surface-crossing blend, below
};

// Returns `authored` with its fog parameters overridden for how it should look when the camera is
// at world-Z `cameraZCm`, given a water surface at `waterLevelCm`, blended across a `fadeBandCm`
// band straddling the surface so entering or leaving the water is a fade rather than a pop.
//
// Pure function: no globals, no device, no persisted state -- every call is independent, and the
// same inputs always produce the same output. That purity is what makes it callable from
// UnderwaterFogTest with nothing but a value-constructed rhi::SkyAtmosphere.
//
// WHAT THIS DOES NOT DO, stated explicitly because it would be easy to assume otherwise from the
// name: it does NOT sample scene depth (no IDevice::sceneDepthTexture() read, no screen-space
// anything), does NOT add caustics, ripple distortion, or any other underwater-specific visual, and
// does NOT touch ambient, sky-light, sun, or cloud fields -- only the six fog fields named below
// change. It works at all because the existing fog machinery (averFogFactor / averFogInscatter /
// averApplyFog, in RHIShaders.cpp) already composites per-pixel against the shaded pixel's own
// world position with no screen-space read of its own -- so once the density is high enough at
// metre scale to dominate over a few metres of visibility distance, swapping the fog PARAMETERS is
// sufficient to make the same machinery read as "underwater" with no new machinery of its own.
//
// The six overridden fields, and the tuning source for each: fogColor <- tuning.color, fogDensity
// <- tuning.densityPerCm, fogFalloff <- tuning.falloff, fogStart <- tuning.startCm, fogMaxOpacity
// <- tuning.maxOpacity. The sixth, fogHeight, has no matching tuning field on purpose: fogHeight is
// "the world Z at which density equals fogDensity" (RHI.hpp), and with tuning.falloff defaulted to
// 0 (uniform fog) it is inert regardless of its value -- so rather than add a field to
// UnderwaterFogTuning purely to hold a number that does nothing under the shipped defaults, this
// function anchors it at `waterLevelCm`, the one value already in scope that is an honest answer to
// "what world Z is special here". That keeps the override correct (not just inert) if a caller ever
// gives fogFalloff a non-zero value without touching this function.
rhi::SkyAtmosphere applyUnderwaterFog(const rhi::SkyAtmosphere& authored,
                                       f32 cameraZCm,
                                       f32 waterLevelCm,
                                       const UnderwaterFogTuning& tuning);

}   // namespace aver::water
