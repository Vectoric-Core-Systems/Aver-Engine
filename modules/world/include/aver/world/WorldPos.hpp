#pragma once
// The one world position type: a global chunk index (i32 x3) plus an f32 offset inside that chunk.
//
// It is SplitPos under a stable name (docs/plans/wave11/LARGE_WORLD.md 2.1): 1.2 micrometres at any
// distance, and the same identity the streamer, the partitioner and the region files already key by.
// Authoring text carries f64 absolute centimetres; everything per-frame (scene, physics, renderer,
// audio, ABI) carries origin-relative f32 centimetres. The floating origin O is the bridge: it is
// always a whole number of regions (kOriginStepCm) on X and Y, and Z is never shifted.
//
// FROZEN by phase 0 of LARGE_WORLD: the other areas code against this header.
// Header-only, Core-only, no I/O, like Vec3.
#include "aver/world/ChunkCoord.hpp"

#include <cmath>

namespace aver::world {

inline constexpr i32 kWorldChunkCm  = kDefaultChunkSizeCm;                        // 1600; the floating origin requires it
inline constexpr i64 kOriginStepCm  = i64(kChunksPerRegionAxis) * kWorldChunkCm;  // 1'638'400 = one region = 16.384 km
inline constexpr i64 kRegionHalfCm  = kOriginStepCm / 2;                          //   819'200
inline constexpr i64 kSubMmCliffCm  = i64(1) << 20;                               // 1'048'576: above this an f32 grid is 1.25 mm
inline constexpr i32 kRebaseMarginDefaultCm = 102'400;                            // 64 chunks, 1,024 m
inline constexpr i32 kRebaseMarginMaxCm     = i32(kSubMmCliffCm - kRegionHalfCm); // 229'376

struct Vec3d    { f64 x = 0, y = 0, z = 0; };
struct OriginCm {                                  // Z is never shifted. Both always multiples of kOriginStepCm.
    i64 x = 0, y = 0;
    friend constexpr bool operator==(const OriginCm& a, const OriginCm& b) { return a.x == b.x && a.y == b.y; }
    friend constexpr bool operator!=(const OriginCm& a, const OriginCm& b) { return !(a == b); }
};
using  WorldPos = SplitPos;                        // chunk + local, kWorldChunkCm

namespace detail {
// splitCm with the invariant `local` on [0, size) enforced: f64 division can land a value one chunk off
// (x/s rounds up to an integer), and the f32 narrowing of a remainder just under `size` can round to
// `size`. Both are repaired here, so every WorldPos this header makes is canonical.
inline void splitAxis(f64 v, i32& chunk, f32& local) {
    const f64 s = static_cast<f64>(kWorldChunkCm);
    f64 q = std::floor(v / s);
    f64 r = v - q * s;
    if (r < 0.0)  { q -= 1.0; r += s; }
    if (r >= s)   { q += 1.0; r -= s; }
    f32 l = static_cast<f32>(r);
    if (l >= static_cast<f32>(kWorldChunkCm)) { q += 1.0; l = 0.0f; }
    chunk = static_cast<i32>(q);
    local = l;
}
inline WorldPos splitXyz(f64 x, f64 y, f64 z) {
    WorldPos p;
    splitAxis(x, p.chunk.x, p.local.x);
    splitAxis(y, p.chunk.y, p.local.y);
    splitAxis(z, p.chunk.z, p.local.z);
    return p;
}
} // namespace detail

// An authored f64 absolute (centimetres) as a WorldPos.
inline WorldPos worldPosOfAbs(f64 xCm, f64 yCm, f64 zCm) { return detail::splitXyz(xCm, yCm, zCm); }

// A scene-frame (origin-relative f32) position as a WorldPos: the add is in f64.
inline WorldPos worldPosOfLocal(const Vec3& localCm, OriginCm o) {
    return detail::splitXyz(static_cast<f64>(localCm.x) + static_cast<f64>(o.x),
                            static_cast<f64>(localCm.y) + static_cast<f64>(o.y),
                            static_cast<f64>(localCm.z));
}

// The scene-frame position of a WorldPos: the chunk-origin-minus-O part is an i64 difference (exact,
// and a multiple of 64 so exact in f32 below 2^30 cm); the one f32 rounding is the sum with p.local.
inline Vec3 localOf(const WorldPos& p, OriginCm o) {
    return Vec3{static_cast<f32>(chunkOriginCmAxis(p.chunk.x, kWorldChunkCm) - o.x) + p.local.x,
                static_cast<f32>(chunkOriginCmAxis(p.chunk.y, kWorldChunkCm) - o.y) + p.local.y,
                static_cast<f32>(chunkOriginCmAxis(p.chunk.z, kWorldChunkCm)) + p.local.z};
}

// f64 absolute centimetres; exact to 2^53 cm.
inline Vec3d absOf(const WorldPos& p) {
    return Vec3d{static_cast<f64>(chunkOriginCmAxis(p.chunk.x, kWorldChunkCm)) + static_cast<f64>(p.local.x),
                 static_cast<f64>(chunkOriginCmAxis(p.chunk.y, kWorldChunkCm)) + static_cast<f64>(p.local.y),
                 static_cast<f64>(chunkOriginCmAxis(p.chunk.z, kWorldChunkCm)) + static_cast<f64>(p.local.z)};
}

// Straight-line distance: the chunk delta in i64, then the local difference in f64.
inline f64 distanceCm(const WorldPos& a, const WorldPos& b) {
    const f64 dx = static_cast<f64>(chunkOriginCmAxis(a.chunk.x, kWorldChunkCm) - chunkOriginCmAxis(b.chunk.x, kWorldChunkCm))
                 + (static_cast<f64>(a.local.x) - static_cast<f64>(b.local.x));
    const f64 dy = static_cast<f64>(chunkOriginCmAxis(a.chunk.y, kWorldChunkCm) - chunkOriginCmAxis(b.chunk.y, kWorldChunkCm))
                 + (static_cast<f64>(a.local.y) - static_cast<f64>(b.local.y));
    const f64 dz = static_cast<f64>(chunkOriginCmAxis(a.chunk.z, kWorldChunkCm) - chunkOriginCmAxis(b.chunk.z, kWorldChunkCm))
                 + (static_cast<f64>(a.local.z) - static_cast<f64>(b.local.z));
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// p moved by an f64 delta; the chunk index absorbs whole chunks, so precision never degrades with distance.
inline WorldPos offsetBy(const WorldPos& p, const Vec3d& deltaCm) {
    WorldPos out;
    f64 v[3] = {static_cast<f64>(p.local.x) + deltaCm.x, static_cast<f64>(p.local.y) + deltaCm.y,
                static_cast<f64>(p.local.z) + deltaCm.z};
    const i32 c[3] = {p.chunk.x, p.chunk.y, p.chunk.z};
    i32 oc[3]; f32 ol[3];
    for (int i = 0; i < 3; ++i) {
        i32 dc = 0;
        detail::splitAxis(v[i], dc, ol[i]);     // splits the sum into whole chunks + remainder
        oc[i] = c[i] + dc;
    }
    out.chunk = ChunkCoord{oc[0], oc[1], oc[2]};
    out.local = Vec3{ol[0], ol[1], ol[2]};
    return out;
}

// The region view (ChunkCoord.hpp regionOfAxis).
inline RegionCoord regionOf(const WorldPos& p) { return regionOf(p.chunk); }

// ---- compile-time proofs of the origin arithmetic -------------------------------------------------
static_assert(kWorldChunkCm == 1600, "the floating origin is defined for 16 m chunks");
static_assert(kOriginStepCm == 1'638'400 && kRegionHalfCm == 819'200, "one region is 16.384 km");
static_assert(kOriginStepCm % 64 == 0 && static_cast<i64>(static_cast<f32>(kOriginStepCm)) == kOriginStepCm,
              "a shift step is exactly representable in f32 (24 significant bits)");
static_assert(kRebaseMarginMaxCm == 229'376, "margin cap keeps every resident object inside the sub-mm cliff");
static_assert(kRegionHalfCm + kRebaseMarginDefaultCm == 921'600, "the default rebase trigger is 9.216 km");

} // namespace aver::world
