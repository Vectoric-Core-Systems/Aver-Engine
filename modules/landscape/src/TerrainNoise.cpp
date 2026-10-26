// Terrain height noise: see TerrainNoise.hpp. Ported verbatim from
// tests/landscape/src/TerrainGenTool.cpp's hash2/valueNoise/fbm -- do not let this drift from that
// file; the byte-identical regeneration TerrainNoise.hpp's header describes only holds while the two
// stay the same formula.
#include "aver/landscape/TerrainNoise.hpp"

#include <cmath>

namespace aver::landscape {
namespace {

// A 2D integer hash -> [-1, 1]. Deterministic across runs, platforms and build types: integer ops
// and one divide, no floating-point accumulation whose rounding could differ.
f32 hash2(i32 x, i32 y, u32 seed) {
    u32 h = static_cast<u32>(x) * 374761393u + static_cast<u32>(y) * 668265263u + seed * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return static_cast<f32>(h & 0xFFFFu) / 32767.5f - 1.0f;
}

// Value noise with a smoothstep fade -- keeps the surface C1-continuous; linear interpolation would
// leave a visible crease along every lattice line once it is lit.
f32 valueNoise(f32 x, f32 y, u32 seed) {
    const f32 fx = std::floor(x), fy = std::floor(y);
    const i32 ix = static_cast<i32>(fx), iy = static_cast<i32>(fy);
    f32 tx = x - fx, ty = y - fy;
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    const f32 a = hash2(ix, iy, seed),     b = hash2(ix + 1, iy, seed);
    const f32 c = hash2(ix, iy + 1, seed), d = hash2(ix + 1, iy + 1, seed);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

// Ridged fBm: folding each octave through 1-|n| turns the zero crossings into creases, which is what
// reads as eroded ground rather than a bedsheet.
f32 fbm(f32 x, f32 y, u32 seed, i32 octaves) {
    f32 sum = 0.0f, amp = 0.5f, freq = 1.0f, norm = 0.0f;
    for (i32 o = 0; o < octaves; ++o) {
        const f32 n = valueNoise(x * freq, y * freq, seed + static_cast<u32>(o) * 7919u);
        sum  += amp * (1.0f - std::fabs(n));
        norm += amp;
        freq *= 2.0f;
        amp  *= 0.5f;
    }
    return norm > 0.0f ? (sum / norm) * 2.0f - 1.0f : 0.0f;
}

// valueNoise / fbm for a position that is already in noise units but too large for f32 (the floating origin's absolute
// position over the feature size). Same hash, fade and fold; only the lattice split is done in f64, so the cell is exact
// and the f32 part is the small fraction inside it.
f32 valueNoiseD(f64 x, f64 y, u32 seed) {
    const f64 fx = std::floor(x), fy = std::floor(y);
    const i32 ix = static_cast<i32>(static_cast<i64>(fx)), iy = static_cast<i32>(static_cast<i64>(fy));
    f32 tx = static_cast<f32>(x - fx), ty = static_cast<f32>(y - fy);
    tx = tx * tx * (3.0f - 2.0f * tx);
    ty = ty * ty * (3.0f - 2.0f * ty);
    const f32 a = hash2(ix, iy, seed),     b = hash2(ix + 1, iy, seed);
    const f32 c = hash2(ix, iy + 1, seed), d = hash2(ix + 1, iy + 1, seed);
    return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

f32 fbmD(f64 x, f64 y, u32 seed, i32 octaves) {
    f32 sum = 0.0f, amp = 0.5f, norm = 0.0f;
    f64 freq = 1.0;
    for (i32 o = 0; o < octaves; ++o) {
        const f32 n = valueNoiseD(x * freq, y * freq, seed + static_cast<u32>(o) * 7919u);
        sum  += amp * (1.0f - std::fabs(n));
        norm += amp;
        freq *= 2.0;
        amp  *= 0.5f;
    }
    return norm > 0.0f ? (sum / norm) * 2.0f - 1.0f : 0.0f;
}

} // namespace

f32 terrainHeightAt(f32 worldXCm, f32 worldYCm, const TerrainNoiseParams& p) {
    if (!(p.featureSizeCm > 0.0f)) return 0.0f;
    return fbm(worldXCm / p.featureSizeCm, worldYCm / p.featureSizeCm, p.seed, p.octaves) * p.amplitudeCm;
}

f32 terrainHeightAtFrom(f32 localXCm, f32 localYCm, i64 originXCm, i64 originYCm, const TerrainNoiseParams& p) {
    if (originXCm == 0 && originYCm == 0) return terrainHeightAt(localXCm, localYCm, p);   // identity at origin zero
    if (!(p.featureSizeCm > 0.0f)) return 0.0f;
    const f64 feature = static_cast<f64>(p.featureSizeCm);
    const f64 x = (static_cast<f64>(originXCm) + static_cast<f64>(localXCm)) / feature;
    const f64 y = (static_cast<f64>(originYCm) + static_cast<f64>(localYCm)) / feature;
    return fbmD(x, y, p.seed, p.octaves) * p.amplitudeCm;
}

} // namespace aver::landscape
