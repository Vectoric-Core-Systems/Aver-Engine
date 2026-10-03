#pragma once
// GiVisibility.hpp -- U1's shared bit definitions and reconstruction arithmetic for ReSTIR GI
// visibility modes and half-resolution reconstruction, unified with voxi_restir.hlsli to prevent
// divergent bit definitions.
#include "aver/core/Types.hpp"

#include <cmath>

namespace aver::voxi::givis {

// These five tunables are mirrored in voxi_restir.hlsli's AVER_GI_VIS_* #defines and validated
// against the shader source by GiVisibilityTest.
inline constexpr f32 kHistWeight  = 0.8f;    // Half-res EMA weight at rest.
inline constexpr f32 kRhoMax      = 4.0f;    // Ceiling on reconstructed sky ratio.
inline constexpr f32 kNormalPow   = 8.0f;    // Exponent on normal-agreement term.
inline constexpr f32 kPlaneTolRel = 0.02f;   // Plane-test tolerance, per unit depth.
inline constexpr f32 kPlaneTolCm  = 1.0f;    // Plane-test tolerance, flat floor.

// (d + 1) / 2 -- half-resolution edge length for a full-resolution edge `d`.
constexpr u32 halfDim(u32 d) { return (d + 1u) / 2u; }

// Does pixel (x, y) trace for real on frame `frame`? Cycles {(0,0), (1,1), (1,0), (0,1)}.
constexpr bool tracedPixel(u32 x, u32 y, u32 frame) {
    const u32 idx = frame & 3u;
    const u32 phaseX = (idx == 0u) ? 0u : (idx == 1u) ? 1u : (idx == 2u) ? 1u : 0u;
    const u32 phaseY = (idx == 0u) ? 0u : (idx == 1u) ? 1u : (idx == 2u) ? 0u : 1u;
    return (x & 1u) == phaseX && (y & 1u) == phaseY;
}

// gAmbientParams.w's bit packing. See notes file for complete field documentation.
// bits 0-1 mode; bits 2-3 reserved; bit 4 histBound; bit 5-7 reserved; bit 8 neuracView[0];
// bit 9-11 neuracView[1-3]; bit 12-15 spatialSamples; bit 16 blendedCone; bit 17 reserved;
// bit 18-22 maxHistory; bit 23 reserved; bit 32 blendedReplay; bit 64 pathView; bit 128 neurac;
// bit 8 histValid. Arguments by name so bit reordering changes only this function.
// neurac and neuracView default to false/0 for backward compatibility.
constexpr u32 packAmbientW(u32 mode, bool histBound, bool histValid, bool blendedCone,
                            bool blendedReplay, bool pathView, u32 spatialSamples, u32 maxHistory,
                            bool neurac = false, u32 neuracView = 0u) {
    u32 w = mode & 3u;
    if (neurac)  w |= 128u;
    w |= (neuracView & 15u) << 8;
    if (histBound)     w |= 4u;
    if (histValid)      w |= 8u;
    if (blendedCone)    w |= 16u;
    if (blendedReplay)  w |= 32u;
    if (pathView)       w |= 64u;
    w |= (spatialSamples & 15u) << 12;
    // bits 18-22 maxHistory. Float32 holds integers exactly only to 2^24; bit 24 is the boundary.
    // Bits beyond 23 cause silent rounding of low fields.
    w |= (maxHistory & 31u) << 18;
    return w;
}

// Mirror of voxi_restir.hlsli's giVisReconstruct: given bilinear weight, pre-saturated normal dot,
// plane distance, and neighbour depth, returns the tap's final weight. Returns 0.0 for rejected
// taps. nDot pre-saturated ensures pow() never faces negative/undefined values.
inline f32 reconstructWeight(f32 bilinear, f32 nDot, f32 planeDist, f32 viewDepth) {
    if (!(bilinear > 0.0f)) return 0.0f;   // NaN-safe comparison.
    const f32 tol = kPlaneTolCm + kPlaneTolRel * viewDepth;
    if (std::fabs(planeDist) > tol) return 0.0f;
    if (!(nDot > 0.0f)) return 0.0f;       // Grazing or opposite normal: no weight.
    return bilinear * std::pow(nDot, kNormalPow);
}

} // namespace aver::voxi::givis
