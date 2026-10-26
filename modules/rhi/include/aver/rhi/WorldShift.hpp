#pragma once
// The renderer's half of the floating origin (docs/plans/wave11/LARGE_WORLD.md 2.2.6): the CPU math
// that keeps history and procedural patterns continuous when every scene-frame position moves by
// -delta. Header-only and Core-only, so it is unit-testable with no device (tests/rhi WorldShiftTest).
//
// Local positions change as x_new = x_old - delta (delta is a whole number of regions, 1,638,400 cm,
// exact in f32), so a previous-frame matrix, which maps OLD-frame points, is composed with a translation
// by +delta, and a pattern that sampled absolute position adds the origin back as an f64-reduced offset.
//
// Standalone types, not aver::world's: Aver.RHI depends on Core and Platform only, and Aver.World sits
// above the renderer. The hosts' origin participant copies a world::ShiftEvent field for field:
//   rhi::ShiftWorldInfo{ {ev.deltaCm.x, ev.deltaCm.y, ev.deltaCm.z}, {ev.newOrigin.x, ev.newOrigin.y}, ev.generation }.
#include "aver/core/Types.hpp"

#include <array>
#include <cmath>

namespace aver::rhi {

struct ShiftVec3d  { f64 x = 0, y = 0, z = 0; };
struct ShiftOrigin { i64 x = 0, y = 0; };   // the NEW origin; both always multiples of 1,638,400 cm

struct ShiftWorldInfo {
    ShiftVec3d  deltaCm;        // new origin minus old; every local position changes by -deltaCm
    ShiftOrigin newOrigin;
    u32         generation = 0; // the value AFTER the shift
};

// prevViewProj' = T(+delta) * prevViewProj in the row-vector convention the shaders and Mat4 use (a
// new-frame point moves into the old frame, then projects). Rows 0..2 are unchanged; row 3 gains
// delta . rows 0..2. Composed in f64 from the stored f32 entries and rounded once, so the large
// translation row does not pick up a second rounding. A zero delta returns the input bit for bit.
inline std::array<f32, 16> composePrevViewProj(const f32 m[16], const ShiftVec3d& delta) {
    std::array<f32, 16> o;
    for (int i = 0; i < 16; ++i) o[i] = m[i];
    if (delta.x == 0.0 && delta.y == 0.0 && delta.z == 0.0) return o;
    for (int c = 0; c < 4; ++c)
        o[12 + c] = static_cast<f32>(static_cast<f64>(m[12 + c]) + delta.x * static_cast<f64>(m[c]) +
                                     delta.y * static_cast<f64>(m[4 + c]) + delta.z * static_cast<f64>(m[8 + c]));
    return o;
}

inline void composePrevViewProjInPlace(f32 m[16], const ShiftVec3d& delta) {
    const std::array<f32, 16> o = composePrevViewProj(m, delta);
    for (int i = 0; i < 16; ++i) m[i] = o[i];
}

// One Gerstner/sine wave's phase offset for an origin-relative position: the shader's
// dot(dir, p) * k + t * w, with p now local, equals the absolute phase when k * dot(dir, O.xy) is added.
// Reduced mod 2*pi in f64 so the f32 result keeps full precision however far O is. `dirXY` and `k` are
// the f32 values the shader reads, widened. Zero for a zero origin (and never -0).
inline f32 wavePhaseOffset(const ShiftVec3d& dirXY, f64 k, ShiftOrigin o) {
    constexpr f64 kTwoPi = 6.283185307179586476925286766559;
    f64 ph = std::fmod(k * (dirXY.x * static_cast<f64>(o.x) + dirXY.y * static_cast<f64>(o.y)), kTwoPi);
    if (ph == 0.0) return 0.0f;
    if (ph < 0.0) ph += kTwoPi;
    return static_cast<f32>(ph);
}

// PerFrameCB::wavePhase: xyz = the offset of each of the (up to three) waves of PerFrameCB::wave
// ({dirX, dirY, k, speed}); w unused. A wave with a zero direction or k (not live) gets zero.
inline void wavePhaseRow(const f32 wave[3][4], ShiftOrigin o, f32 out[4]) {
    for (int i = 0; i < 3; ++i)
        out[i] = wavePhaseOffset(ShiftVec3d{static_cast<f64>(wave[i][0]), static_cast<f64>(wave[i][1]), 0.0},
                                 static_cast<f64>(wave[i][2]), o);
    out[3] = 0.0f;
}

// PerFrameCB::worldOrigin: xyz = O in cm (exact in f32 for multiples of 1,638,400), w = the generation
// as a float, for debugging. All zero at origin zero and generation zero.
inline void worldOriginRow(ShiftOrigin o, u32 generation, f32 out[4]) {
    out[0] = static_cast<f32>(o.x);
    out[1] = static_cast<f32>(o.y);
    out[2] = 0.0f;
    out[3] = static_cast<f32>(generation);
}

// The cloud noise samples (wpos + cloudMotion.xy) * scale. With wpos origin-relative the CPU folds the
// origin into the wind offset, in f64, so wpos_local + fold == wpos_abs + wind up to f32 rounding of the
// fold. `windOffset` is the f32 offset the device computes today (wind * time, plus the per-seed hash
// when the clock is off). Origin zero returns it unchanged.
struct CloudMotionFold { f32 x, y; };
inline CloudMotionFold cloudMotionFold(const ShiftVec3d& windOffset, ShiftOrigin o) {
    return CloudMotionFold{o.x == 0 ? static_cast<f32>(windOffset.x) : static_cast<f32>(windOffset.x + static_cast<f64>(o.x)),
                           o.y == 0 ? static_cast<f32>(windOffset.y) : static_cast<f32>(windOffset.y + static_cast<f64>(o.y))};
}

// A scene-frame POINT after a shift: plain f32 `p - (f32)delta` per axis, the subtraction World::shiftRoots makes,
// so a renderer's copy of a position stays bit-equal to the entity it was taken from. Z is a no-op (delta.z is 0).
inline void shiftPoint(f32 p[3], const ShiftVec3d& delta) {
    p[0] -= static_cast<f32>(delta.x);
    p[1] -= static_cast<f32>(delta.y);
    p[2] -= static_cast<f32>(delta.z);
}

// A row-vector 4x4 world matrix: the translation row (12..14) is a point, the rest is untouched.
inline void shiftWorldMatrix(f32 m[16], const ShiftVec3d& delta) { shiftPoint(m + 12, delta); }

// One region step (CHUNKS.md slice 10) and the number of steps a delta spans (signed; 0 for a zero delta).
inline constexpr f64 kShiftStepCm = 1'638'400.0;
inline i64 shiftSteps(f64 deltaCm) { return static_cast<i64>(std::llround(deltaCm / kShiftStepCm)); }

// A Gerstner wave's phase offset, as water.hlsl / water_displace.hlsl compute the wave: dir = normalize(dirX, dirY)
// (zero when degenerate), omega = 2*pi / max(wavelength, 1e-3), phase = omega * dot(dir, p) + ... With p origin-relative the
// shader adds this per-wave number (gWaveLenAmp[i].z); zero at origin zero.
inline f32 gerstnerPhaseOffset(f32 dirX, f32 dirY, f32 wavelengthCm, ShiftOrigin o) {
    constexpr f64 kTwoPi = 6.283185307179586476925286766559;
    const f64 dx = static_cast<f64>(dirX), dy = static_cast<f64>(dirY);
    const f64 lenSq = dx * dx + dy * dy;
    if (!(lenSq > 1e-12)) return 0.0f;
    const f64 inv = 1.0 / std::sqrt(lenSq);
    const f64 wl = static_cast<f64>(wavelengthCm) > 1e-3 ? static_cast<f64>(wavelengthCm) : 1e-3;
    return wavePhaseOffset(ShiftVec3d{dx * inv, dy * inv, 0.0}, kTwoPi / wl, o);
}

// ---- compile-time proofs ----------------------------------------------------------------------------
static_assert(static_cast<i64>(static_cast<f32>(i64(1'638'400))) == 1'638'400, "one region step is exact in f32 (centimetres)");
static_assert(static_cast<i64>(static_cast<f32>(i64(1'638'400) * 6)) == 1'638'400 * 6, "so are small multiples");

} // namespace aver::rhi
