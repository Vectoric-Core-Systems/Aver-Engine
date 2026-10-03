// voxi_radiance_cache.hlsli -- the radiance cache's PURE maths: constants, the RcInfo/RcCell layouts,
// the cell addressing and tag, the L1 spherical-harmonics (SH) basis and its cosine convolution, and
// the normal / SH packing. It declares NO resource and reads NO cbuffer field, so the resolve shader
// (voxi_radiance_cache_resolve.hlsl, own t0/u0/u1) can include it exactly as the Voxi scene shaders do.
// The resource-bound half (scatter + lookup, over t22/u20/u21) is voxi_radiance_cache_io.hlsli.
//
// WHO INCLUDES IT: voxi_restir.hlsli, under `#if AVER_RADIANCE_CACHE` only (the four lazily compiled
// staged-compute twin pipelines), and the resolve shader. Every other variant never sees this text.
//
// THE CONSTANTS BELOW ARE MIRRORED BY RadianceCacheLayout.hpp (the single source); RadianceCacheTest
// greps these literals against the header, so change both together.
//
// WHAT THE CACHE IS: three camera-centred 64^3 cascades (cell size 25 / 100 / 400 world units), each cell
// holding the first-order SH of INCIDENT radiance at that point, trained live from the second-bounce
// rays ReSTIR GI already traces (design: docs/rendering/RADIANCE_CACHE.md). Training accumulates in
// integer fixed point (an InterlockedAdd per term, order-independent), the resolve folds the
// per-frame mean into the stored cell, and the lookup reads the stored cells.

#ifndef AVER_RADIANCE_CACHE_PURE_HLSLI
#define AVER_RADIANCE_CACHE_PURE_HLSLI

#define AVER_RC_CASCADES        3
#define AVER_RC_RES             64
#define AVER_RC_CELLS_PER_CASC  262144      // 64^3
#define AVER_RC_CELLS           786432      // 3 * 64^3

// Fixed-point scales of the accumulator (signed ints, because D3D12 has only u32/s32 atomic add):
// SH terms * 2^15, summed normals * 2^16. Headroom (static_assert in RadianceCacheLayout.hpp): the worst
// sample |c_i| <= LMAX * 0.488603 * PI / MIN_COS = 491; times the 64-sample cap = 31.4k; times 2^15 =
// 1.03e9 < 2^31.
#define AVER_RC_SH_SCALE        32768.0
#define AVER_RC_N_SCALE         65536.0
#define AVER_RC_MAX_CAP         64          // hard ceiling on the per-cell per-frame sample cap
#define AVER_RC_LMAX            32.0        // a sample's radiance is clamped here before projection
#define AVER_RC_MIN_COS         0.1         // pdf floor: weight = PI / max(cos, MIN_COS)
#define AVER_RC_NEFF_MAX        15          // n_eff and age are 4 bits each
#define AVER_RC_AGE_MAX         15
#define AVER_RC_NORMAL_K        2.0         // corner weight = saturate(dot(cellNormal, N) * K)
#define AVER_RC_MARGIN_CELLS    1           // a cascade is skipped if a lookup corner is within this of its edge
#define AVER_RC_CROSSFADE_CELLS 4           // edge fade width in cells

// ---- RESOURCE ELEMENT LAYOUTS (shared with the resolve shader and the C++ side) ----
// RcInfo, stride 80, ONE element, CPU-written Upload ring:
//   hdr0 = (flags, frameIndex, ageStepFrames, sampleCap); flags bit 0 = CLEAR_ALL (the resolve zeroes
//          the accumulator and every cell and returns).
//   hdr1.x = alphaMin; yzw reserved 0.
//   cas[c] = (originCell.xyz, asint(cellSize)); originCell is the window's MIN-corner world cell, so a
//          cascade covers world cells [origin, origin + 64) per axis.
struct RcInfo { uint4 hdr0; float4 hdr1; int4 cas[3]; };
// RcCell, stride 32, cascade-major (rcCellIndex): a.xyzw + b.xy = the 12 SH values as fp16 pairs
// (rcPackSh), b.z = the normal word (rcPackNormal), b.w = the meta word:
//   tag (bits 0-23) | n_eff (bits 24-27, 0 = empty) | age (bits 28-31, in ageStepFrames units).
// A cell is VALID iff n_eff > 0 && tag == rcTagPack(its expected world cell) && age < AVER_RC_AGE_MAX.
struct RcCell { uint4 a; uint4 b; };

// Accumulator layout (RWStructuredBuffer<int>, stride 4): the per-cell sample COUNTS are their own
// region [0, AVER_RC_CELLS) so the resolve can early-out by streaming 3 MB contiguously; the payload
// follows at AVER_RC_CELLS + cell * 15 + k, k = 0..11 the SH sums ([c0.rgb, cY.rgb, cZ.rgb, cX.rgb]),
// k = 12..14 the summed surface normal.
#define AVER_RC_PAYLOAD_INTS    15

// 3 x 8 bits of (worldCell >> 6) per axis. The toroidal address (worldCell & 63) cannot tell two world
// cells 64 apart from each other; the tag can (up to 256 * 64 cells, where it aliases -- bounded by age).
// `>>` on a negative int is arithmetic in HLSL, so negative cells tag consistently with their `& 63`.
uint rcTagPack(int3 worldCell) {
    const uint3 t = (uint3)((worldCell >> 6) & 255);
    return t.x | (t.y << 8) | (t.z << 16);
}

// Cascade-major toroidal index; (x & 63) is well defined on negative ints (two's complement).
uint rcCellIndex(uint cascade, int3 worldCell) {
    const uint3 t = (uint3)(worldCell & 63);
    return cascade * AVER_RC_CELLS_PER_CASC + ((t.z * 64u) + t.y) * 64u + t.x;
}

// Real L1 SH basis, coefficient order [Y0, Y1(y), Y1(z), Y1(x)].
void rcShBasis(float3 dir, out float y[4]) {
    y[0] = 0.282095;
    y[1] = 0.488603 * dir.y;
    y[2] = 0.488603 * dir.z;
    y[3] = 0.488603 * dir.x;
}

// Cosine-convolved irradiance divided by PI (Ramamoorthi-Hanrahan: A0 = PI, A1 = 2PI/3), i.e. in the
// units F2 multiplies by kdAlbedo. A constant L over the hemisphere around N gives exactly L back.
float3 rcShIrradianceOverPi(float3 c[4], float3 N) {
    const float3 e = 0.282095 * c[0]
                   + (2.0 / 3.0) * 0.488603 * (N.y * c[1] + N.z * c[2] + N.x * c[3]);
    return max(e, 0.0);
}

// Octahedral unit-vector encode/decode, 11 bits per axis. The length of the mean normal (planarity: 1 on
// a flat patch, short where two surfaces meet in one cell) rides in the top 10 bits as a unorm.
float2 rcOctEncode(float3 n) {
    float2 p = n.xy / max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    if (n.z < 0.0) p = (1.0 - abs(p.yx)) * float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return p;
}
float3 rcOctDecode(float2 p) {
    float3 n = float3(p.x, p.y, 1.0 - abs(p.x) - abs(p.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}
uint rcPackNormal(float3 meanVec) {
    const float len = length(meanVec);
    const float3 dir = len > 1e-6 ? meanVec / len : float3(0.0, 0.0, 1.0);
    const float2 o = saturate(rcOctEncode(dir) * 0.5 + 0.5);
    const uint u = (uint)(o.x * 2047.0 + 0.5);
    const uint v = (uint)(o.y * 2047.0 + 0.5);
    const uint l = (uint)(saturate(len) * 1023.0 + 0.5);
    return u | (v << 11) | (l << 22);
}
float3 rcUnpackNormal(uint w, out float len) {
    const float2 o = float2((float)(w & 2047u), (float)((w >> 11) & 2047u)) * (1.0 / 2047.0);
    len = (float)(w >> 22) * (1.0 / 1023.0);
    return rcOctDecode(o * 2.0 - 1.0);
}

// 12 SH values (order [c0.rgb, cY.rgb, cZ.rgb, cX.rgb]) as 6 words of two fp16.
void rcPackSh(float3 c[4], out uint w[6]) {
    const float v[12] = { c[0].x, c[0].y, c[0].z, c[1].x, c[1].y, c[1].z,
                          c[2].x, c[2].y, c[2].z, c[3].x, c[3].y, c[3].z };
    [unroll] for (int i = 0; i < 6; ++i) w[i] = f32tof16(v[2 * i]) | (f32tof16(v[2 * i + 1]) << 16);
}
void rcUnpackSh(uint4 a, uint2 b, out float3 c[4]) {
    const uint w[6] = { a.x, a.y, a.z, a.w, b.x, b.y };
    float v[12];
    [unroll] for (int i = 0; i < 6; ++i) {
        v[2 * i]     = f16tof32(w[i] & 0xFFFFu);
        v[2 * i + 1] = f16tof32(w[i] >> 16);
    }
    c[0] = float3(v[0], v[1], v[2]);   c[1] = float3(v[3], v[4], v[5]);
    c[2] = float3(v[6], v[7], v[8]);   c[3] = float3(v[9], v[10], v[11]);
}

#endif  // AVER_RADIANCE_CACHE_PURE_HLSLI
