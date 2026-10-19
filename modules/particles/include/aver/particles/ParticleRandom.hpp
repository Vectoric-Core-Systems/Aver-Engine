// The particle module's counter-based randomness: a value is a pure function of (seed, stream), so the CPU
// executor (EmitterSim) and the GPU kernels (particles_common.hlsli carries the same five lines) draw identical
// numbers in any call order. Each use of randomness has a fixed named stream below; adding a use means adding a
// stream id at the END, never renumbering.
#pragma once
#include "aver/core/Types.hpp"

#include <cmath>

namespace aver::particles {

// splitmix32: the mixing constants modules/render.pcg's hash32 uses.
inline u32 hash32(u32 x) {
    u32 z = x + 0x9E3779B9u;
    z = (z ^ (z >> 16)) * 0x21F0AAADu;
    z = (z ^ (z >> 15)) * 0x735A2D97u;
    return z ^ (z >> 15);
}

// Uniform in [0, 1), 24 bits.
inline f32 rand01(u32 seed, u32 stream) {
    return static_cast<f32>(hash32(seed + 0x9E3779B9u * (stream + 1u)) >> 8) * (1.0f / 16777216.0f);
}

// A particle's own seed from its emitter's seed and its spawn id.
inline u32 particleSeed(u32 emitterSeed, u32 id) { return hash32(emitterSeed ^ (id * 0x85EBCA6Bu)); }

enum RandStream : u32 {
    kStreamShapeA = 0,      // shape sampling (three draws)
    kStreamShapeB = 1,
    kStreamShapeC = 2,
    kStreamConeCos = 3,
    kStreamConePhi = 4,
    kStreamSpeed = 5,
    kStreamLifetime = 6,
    kStreamStartFrame = 7,
    kStreamSizeScale = 8,   // spawn.sizescale
    kStreamTint = 9,        // spawn.colortint
    kStreamRotation = 10,   // spawn.rotation, initial angle
    kStreamSpin = 11,       // spawn.rotation, angular velocity
};

// 32 pinned (seed, stream, top 24 bits) triples: ParticleModulesTest asserts them on the CPU, ParticleGpuParityTest reads
// the same draws back from the GPU. If hash32/rand01 change, regenerate with the formula above (never by hand).
struct RandGolden { u32 seed; u32 stream; u32 bits24; };
inline constexpr RandGolden kRandGolden[32] = {
    {0x00000000u, 0u, 0xD9C079u}, {0xFFFFFFFFu, 1u, 0x4DA2A8u}, {0x3C6EF37Fu, 2u, 0xA536C7u}, {0xDAA66D53u, 3u, 0x932A6Cu},
    {0x78DDE735u, 4u, 0xA8E5FCu}, {0x17156125u, 5u, 0xA2133Au}, {0xB54CDB23u, 6u, 0x25D34Bu}, {0x5384552Fu, 7u, 0x88D025u},
    {0xF1BBCF49u, 8u, 0xC2B01Au}, {0x8FF34971u, 9u, 0xF50569u}, {0x2E2AC3A7u, 10u, 0xB7B988u}, {0xCC623DEBu, 11u, 0x0D548Cu},
    {0x6A99B83Du, 0u, 0xB5BC08u}, {0x08D1329Du, 1u, 0x082205u}, {0xA708AD0Bu, 2u, 0xA002FAu}, {0x45402787u, 3u, 0xC14A1Fu},
    {0xE377A211u, 4u, 0x265D2Bu}, {0x81AF1CA9u, 5u, 0x0D0E86u}, {0x1FE6974Fu, 6u, 0xC59771u}, {0xBE1E1203u, 7u, 0xE3AB54u},
    {0x5C558CC5u, 8u, 0x21B521u}, {0xFA8D0795u, 9u, 0x3F07A3u}, {0x98C48273u, 10u, 0x41909Cu}, {0x36FBFD5Fu, 11u, 0x99EEF1u},
    {0xD5337859u, 0u, 0x4B0A9Cu}, {0x736AF361u, 1u, 0x06C7CFu}, {0x11A26E77u, 2u, 0x57D7E8u}, {0xAFD9E99Bu, 3u, 0x5B753Du},
    {0x4E1164CDu, 4u, 0x0BD37Fu}, {0xEC48E00Du, 5u, 0xC64037u}, {0x8A805B5Bu, 6u, 0xCD39E0u}, {0x28B7D6B7u, 7u, 0x82262Bu},
};

// ---- lattice gradient noise (curl noise's potential) ---------------------------------------------------------------
// Classic gradient noise written from its definition: a hashed gradient at each lattice corner, quintic fade,
// trilinear blend. Not simplex. Range about [-1, 1].
inline void noiseGradient(u32 h, f32& gx, f32& gy, f32& gz) {
    // The twelve edge-midpoint directions of a cube, picked by hash.
    static const f32 kG[12][3] = {{1, 1, 0}, {-1, 1, 0}, {1, -1, 0}, {-1, -1, 0}, {1, 0, 1}, {-1, 0, 1},
                                  {1, 0, -1}, {-1, 0, -1}, {0, 1, 1}, {0, -1, 1}, {0, 1, -1}, {0, -1, -1}};
    const u32 i = (h >> 8) % 12u;
    gx = kG[i][0]; gy = kG[i][1]; gz = kG[i][2];
}

inline f32 noiseCorner(i32 ix, i32 iy, i32 iz, f32 fx, f32 fy, f32 fz, u32 seed) {
    const u32 h = hash32(hash32(hash32(seed + static_cast<u32>(ix)) + static_cast<u32>(iy)) + static_cast<u32>(iz));
    f32 gx, gy, gz;
    noiseGradient(h, gx, gy, gz);
    return gx * fx + gy * fy + gz * fz;
}

inline f32 noiseFade(f32 t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); }

inline f32 gradNoise3(f32 x, f32 y, f32 z, u32 seed) {
    const f32 fxl = std::floor(x), fyl = std::floor(y), fzl = std::floor(z);
    const i32 ix = static_cast<i32>(fxl), iy = static_cast<i32>(fyl), iz = static_cast<i32>(fzl);
    const f32 fx = x - fxl, fy = y - fyl, fz = z - fzl;
    const f32 u = noiseFade(fx), v = noiseFade(fy), w = noiseFade(fz);
    const f32 c000 = noiseCorner(ix,     iy,     iz,     fx,        fy,        fz,        seed);
    const f32 c100 = noiseCorner(ix + 1, iy,     iz,     fx - 1.0f, fy,        fz,        seed);
    const f32 c010 = noiseCorner(ix,     iy + 1, iz,     fx,        fy - 1.0f, fz,        seed);
    const f32 c110 = noiseCorner(ix + 1, iy + 1, iz,     fx - 1.0f, fy - 1.0f, fz,        seed);
    const f32 c001 = noiseCorner(ix,     iy,     iz + 1, fx,        fy,        fz - 1.0f, seed);
    const f32 c101 = noiseCorner(ix + 1, iy,     iz + 1, fx - 1.0f, fy,        fz - 1.0f, seed);
    const f32 c011 = noiseCorner(ix,     iy + 1, iz + 1, fx,        fy - 1.0f, fz - 1.0f, seed);
    const f32 c111 = noiseCorner(ix + 1, iy + 1, iz + 1, fx - 1.0f, fy - 1.0f, fz - 1.0f, seed);
    const f32 x00 = c000 + (c100 - c000) * u, x10 = c010 + (c110 - c010) * u;
    const f32 x01 = c001 + (c101 - c001) * u, x11 = c011 + (c111 - c011) * u;
    const f32 y0 = x00 + (x10 - x00) * v, y1 = x01 + (x11 - x01) * v;
    return y0 + (y1 - y0) * w;
}

} // namespace aver::particles
