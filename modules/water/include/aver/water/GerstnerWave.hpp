#pragma once

// Gerstner ocean-wave math, in centimetres, with ZERO knowledge of rendering or any device. Pure
// C++, no rhi:: and no JPH:: -- the same "small, dependency-free math file" spirit
// modules/physics/src/Convert.hpp is written in, except this one is PUBLIC rather than a private
// implementation detail of one module's boundary: unlike the Jolt <-> engine axis conversion, which
// exists ONLY because Jolt's own types must never cross out of modules/physics, the shape of the
// water surface is not a boundary anyone needs walled off. Buoyancy, a shoreline foam mask, or a
// boat AI that just wants "how high is the water here" can all call straight into this header
// without linking Aver.RHI or touching a device -- see modules/water/README.md's own argument for
// why this separation is load-bearing, not incidental.
#include "aver/core/Types.hpp"

#include <cmath>
#include <cstddef>

namespace aver::water {

// One Gerstner wave's authored parameters.
//
// `dirX`/`dirZ` NEED NOT BE PRE-NORMALISED -- gerstnerHeightCm/gerstnerDisplaceCm normalise
// internally, matching the convention rhi::SkyAtmosphere::sunDirection already documents ("need
// not be normalised": RHI.hpp's own comment on that field) so an author dragging a direction widget
// never has to re-normalise it by hand between edits.
//
// `steepness` is the AUTHORED Qi (GPU Gems' name for it), read and clamped per call -- see
// detail::clampedSteepness's comment for why the clamp happens at call time rather than once when
// the struct is written, and gerstnerDisplaceCm's own comment for the derivation the clamp exists
// to satisfy.
struct GerstnerWave {
    f32 dirX = 1.0f;
    f32 dirZ = 0.0f;
    f32 wavelengthCm = 800.0f;
    f32 amplitudeCm  = 25.0f;
    f32 steepness    = 0.6f;
};

// Four waves is enough for the sum to read as chop rather than a single ripple, without a
// per-vertex cost that starts competing with everything else on the frame. This is also the cbuffer
// array size WaterShaders.hpp's kWaterHLSL cbuffer declares -- WaterRenderer.cpp passes it to the
// shader compiler as a -D so the two never drift apart silently (see that file's own comment on
// materialShaderDefines' precedent for computing a register/size define in C++ rather than
// hardcoding the same number twice, once per language).
constexpr size_t kMaxGerstnerWaves = 4;

// Gravity in CENTIMETRES PER SECOND SQUARED -- 981.0, not Jolt's 9.81 m/s^2 -- and that is
// deliberate, not a typo waiting to be "fixed" back to the SI value.
//
// modules/physics/src/Convert.hpp exists precisely because Jolt's own convention (metres) and the
// engine's (centimetres, stated up front in physics_abi.h's own top-of-file comment) disagree, and
// Convert.hpp's answer is to translate units AT the Jolt boundary and keep everything on the engine
// side of it in centimetres, unconditionally. GerstnerWave.hpp has no Jolt boundary anywhere near
// it: WaterRenderer.cpp feeds it camera positions and grid coordinates that are ALREADY
// centimetres, straight from IDevice::camera() and the grid's own authored extent. Introducing a
// second, metric g here would mean either converting xCm/zCm to metres and back on every call (pure
// waste, and a second place a scale factor could be gotten wrong), or silently mixing units inside
// one expression -- so the one gravitational constant this file needs is Convert.hpp's own
// kCmPerMetre (100) applied once to 9.81 m/s^2, written out as a literal so this header does not
// have to include Convert.hpp (and, transitively, anything Jolt) just to compute it.
constexpr f32 kGravityCmPerS2 = 981.0f;

namespace detail {

// Per-wave angular frequency and unit direction, factored out because gerstnerHeightCm and
// gerstnerDisplaceCm both need it and MUST agree on it bit-for-bit -- a normal computed from a
// direction normalised one way and a height computed from the same direction normalised another way
// would disagree about which way is "up" at exactly the pixels where it matters most.
inline void gerstnerWaveTerms(const GerstnerWave& w, f32& outOmega, f32& outDirX, f32& outDirZ) {
    const f32 lenSq = w.dirX * w.dirX + w.dirZ * w.dirZ;
    const f32 invLen = lenSq > 1e-12f ? 1.0f / std::sqrt(lenSq) : 0.0f;   // a zero direction contributes nothing
    outDirX = w.dirX * invLen;
    outDirZ = w.dirZ * invLen;
    // A wavelength of zero (or negative, from a bad author edit) would divide by zero below; floored
    // rather than asserted, because this is read every frame from data a level author controls, and
    // a degenerate wave should draw as a flat, motionless one rather than crash the renderer.
    const f32 wavelengthCm = w.wavelengthCm > 1e-3f ? w.wavelengthCm : 1e-3f;
    outOmega = 2.0f * 3.14159265358979f / wavelengthCm;
}

// Clamps one wave's authored steepness to at most 1/count, so the SUM of every wave's steepness in
// the set never exceeds 1.
//
// THIS IS THE WELL-KNOWN GPU GEMS CRITERION, cited here as its SOURCE rather than as something
// measured against this engine's own geometry: Finch, "Effective Water Simulation from Physical
// Models", GPU Gems 1, chapter 1. Past a per-wave Qi*wi*Ai product of 1, the horizontal component
// of a single Gerstner wave's own displacement derivative can locally invert -- the crest folds
// over itself and the "surface" briefly has three x values for one x0, an unrenderable
// self-intersecting loop rather than a sharp peak. Bounding the SUM of every wave's Qi to at most 1
// (by giving each of `count` waves an equal 1/count share) is the standard, conservative way to keep
// an N-wave sum inside that bound without computing each wave's own wi*Ai product and solving for
// the tightest per-wave limit -- GPU Gems' own worked example does the same equal-share division.
// Nothing below was independently derived or tuned; it is that same rule, applied unchanged.
inline f32 clampedSteepness(f32 steepness, size_t count) {
    const f32 maxQi = count > 0 ? 1.0f / static_cast<f32>(count) : 0.0f;
    const f32 q = steepness < 0.0f ? 0.0f : steepness;
    return q < maxQi ? q : maxQi;
}

} // namespace detail

// Summed Gerstner wave height at (xCm, zCm) at time tSeconds.
//
// Zero waves, or every wave at amplitudeCm == 0, returns EXACTLY 0 -- the sum below has no term
// that survives an empty wave list or a zero amplitude, so there is no spurious DC offset for a
// caller (buoyancy, a foam mask) to trip over when "no waves" is meant to mean "flat water".
inline f32 gerstnerHeightCm(const GerstnerWave* waves, size_t count, f32 xCm, f32 zCm, f32 tSeconds) {
    f32 height = 0.0f;
    for (size_t i = 0; i < count; ++i) {
        const GerstnerWave& w = waves[i];
        f32 omega, dx, dz;
        detail::gerstnerWaveTerms(w, omega, dx, dz);
        // Deep-water dispersion relation: phase speed Si = sqrt(g / omega) (GPU Gems again), so the
        // phase constant Phi_i = Si * omega reduces to sqrt(g * omega) -- see kGravityCmPerS2's own
        // comment for why g is 981, not 9.81, here.
        const f32 phaseSpeed = std::sqrt(kGravityCmPerS2 * omega);
        const f32 phase = omega * (dx * xCm + dz * zCm) + phaseSpeed * tSeconds;
        height += w.amplitudeCm * std::sin(phase);
    }
    return height;
}

// Full Gerstner displacement AND its analytic normal at (xCm, zCm, tSeconds).
//
// outPosCm is {x0 + horizontal displacement along dirX, z0 + horizontal displacement along dirZ,
// height}. Index 2 is a VERTICAL offset in this file's own generic (x, z, height) frame, NOT a
// world Z coordinate -- this header has no concept of which world axis is "up". WaterRenderer.cpp's
// own top-of-file comment is where that gets resolved: the engine is +Z-up, so its caller reads
// index 0 as world X, index 1 as world Y (this file's "z" is a generic second horizontal axis, not
// the engine's Z), and adds index 2 to a world Z base. outNormal follows the SAME {x-component,
// z-component, vertical-component} ordering as outPosCm, unit length.
inline void gerstnerDisplaceCm(const GerstnerWave* waves, size_t count, f32 xCm, f32 zCm,
                                f32 tSeconds, f32 outPosCm[3], f32 outNormal[3]) {
    f32 dispX = 0.0f, dispZ = 0.0f, height = 0.0f;
    // Analytic partial derivatives of the sum with respect to the UNDISPLACED (xCm, zCm),
    // accumulated in the SAME loop as the displacement rather than recovered afterward by a finite
    // difference: GPU Gems gives the derivative in closed form, and a closed form is both cheaper
    // per vertex and immune to the step-size tuning a numeric normal would need. It is also what
    // keeps this file and WaterShaders.hpp's hand-ported HLSL twin (see that file's own comment)
    // trivially in agreement -- a mismatched finite-difference epsilon between a CPU and a GPU
    // implementation could never even arise, because neither one uses finite differences at all.
    f32 dHdx = 0.0f, dHdz = 0.0f;   // d(height)/dx0, d(height)/dz0
    f32 qWaSin = 0.0f;              // sum(Qi * omega_i * Ai * sin(phase_i)) -- the vertical-normal term

    for (size_t i = 0; i < count; ++i) {
        const GerstnerWave& w = waves[i];
        f32 omega, dx, dz;
        detail::gerstnerWaveTerms(w, omega, dx, dz);
        const f32 phaseSpeed = std::sqrt(kGravityCmPerS2 * omega);
        const f32 phase = omega * (dx * xCm + dz * zCm) + phaseSpeed * tSeconds;
        const f32 s = std::sin(phase), c = std::cos(phase);
        const f32 qi = detail::clampedSteepness(w.steepness, count);

        dispX  += qi * w.amplitudeCm * dx * c;
        dispZ  += qi * w.amplitudeCm * dz * c;
        height += w.amplitudeCm * s;

        const f32 wa = omega * w.amplitudeCm;
        dHdx   += wa * dx * c;
        dHdz   += wa * dz * c;
        qWaSin += qi * wa * s;
    }

    outPosCm[0] = xCm + dispX;
    outPosCm[1] = zCm + dispZ;
    outPosCm[2] = height;

    // GPU Gems' closed-form normal: N = (-dHeight/dx0, -dHeight/dz0, 1 - sum(Qi*wi*Ai*sin(phase))).
    // The vertical component starting from 1 (rather than 0) is what keeps the normal reading
    // straight up over calm water -- as every wave's amplitude goes to 0, dHdx/dHdz/qWaSin all go to
    // 0 and this collapses to exactly {0, 0, 1}, the flat-water case double-checked by
    // GerstnerWaveTest's zero-amplitude case.
    const f32 nx = -dHdx, nz = -dHdz, ny = 1.0f - qWaSin;
    const f32 lenSq = nx * nx + nz * nz + ny * ny;
    const f32 invLen = lenSq > 1e-12f ? 1.0f / std::sqrt(lenSq) : 1.0f;
    outNormal[0] = nx * invLen;
    outNormal[1] = nz * invLen;
    outNormal[2] = ny * invLen;
}

// Floors `worldCm` to the nearest multiple of `cellSizeCm` at or below it -- the ocean grid's own
// vertices never move (see WaterRenderer.hpp's top-of-class comment for why), so what keeps the
// grid from shimmering as the camera moves is recentring its ORIGIN on a cell boundary rather than
// letting it drift to an arbitrary sub-cell camera position; every vertex then lands on the same
// world-space lattice point it would have if the grid had simply always been centred there. Pure
// floor-to-cell: no rounding, no wrapping, nothing wave-related.
inline f32 snapWorldToGridCm(f32 worldCm, f32 cellSizeCm) {
    const f32 cell = cellSizeCm > 1e-6f ? cellSizeCm : 1e-6f;   // guards a degenerate/zero cell size
    return std::floor(worldCm / cell) * cell;
}

} // namespace aver::water
