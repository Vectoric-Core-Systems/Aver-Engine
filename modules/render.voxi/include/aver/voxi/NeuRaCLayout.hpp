// NeuRaCLayout: constants, layouts, and packing rules for the Voxi radiance cache (docs/rendering/NEURAC.md).
// Must stay synchronized: NeuRaC.cpp, voxi_neurac*.hlsli, NeuRaCTest.
#pragma once

#include "aver/core/Types.hpp"

#include <cmath>
#include <cstring>

namespace aver::voxi::neurac {

// ---------------------------------------------------------------- geometry
inline constexpr u32 kCascades         = 3;
inline constexpr u32 kRes              = 64;                       // cells per axis per cascade
inline constexpr u32 kResMask          = kRes - 1;                 // texel = worldCell & kResMask
inline constexpr u32 kCellsPerCascade  = kRes * kRes * kRes;       // 262,144
inline constexpr u32 kCells            = kCascades * kCellsPerCascade;   // 786,432
inline constexpr f32 kCellSizeCm[kCascades] = {25.0f, 100.0f, 400.0f};
// Window centered on camera.
inline constexpr i32 kHalfRes          = static_cast<i32>(kRes / 2);
// Edge margin and cascade cross-fade width.
inline constexpr u32 kMarginCells      = 1;
inline constexpr u32 kCrossFadeCells   = 4;

// ---------------------------------------------------------------- tunables the HLSL mirrors (AVER_RC_*)
inline constexpr f64 kShScale    = 32768.0;     // 2^15      -> AVER_RC_SH_SCALE
inline constexpr f64 kNScale     = 65536.0;     // 2^16      -> AVER_RC_N_SCALE
inline constexpr f64 kSqScale    = 2048.0;      // 2^11      -> AVER_RC_SQ_SCALE
inline constexpr f64 kDcToE      = 1.0 / (4.0 * 3.14159265358979323846);   // 0.282095^2 -> AVER_RC_DC_TO_E
inline constexpr u32 kMaxCap     = 64;          //           -> AVER_RC_MAX_CAP
inline constexpr f64 kLMax       = 32.0;        //           -> AVER_RC_LMAX
inline constexpr f64 kMinCos     = 0.1;         // pdf floor  -> AVER_RC_MIN_COS
inline constexpr u32 kNEffMax    = 15;          // 4-bit field -> AVER_RC_NEFF_MAX
inline constexpr u32 kAgeMax     = 15;          // 4-bit field -> AVER_RC_AGE_MAX
inline constexpr f64 kNormalK    = 2.0;         // corner weight -> AVER_RC_NORMAL_K
inline constexpr f64 kMinAcceptRate = 1.0 / 32.0;  // floor on the load-adaptive accept rate -> AVER_RC_MIN_ACCEPT_RATE
inline constexpr u32 kTrustMax   = 63;          // 6-bit trust, 63 = 1.0 -> AVER_RC_TRUST_MAX
inline constexpr u32 kLenMax     = 63;          // 6-bit planarity       -> AVER_RC_LEN_MAX
inline constexpr u32 kDefaultCoarseCap1 = 16;   // per-cell cap, cascade 1 (cascade 0 uses sampleCap)
inline constexpr u32 kDefaultCoarseCap2 = 8;    // per-cell cap, cascade 2

// ---------------------------------------------------------------- accumulator layout (int32 elements)
// Counts [0, kCells) = accepted | prevArrivals << 16; arrivals [kCells, 2 kCells);
// payload from kPayloadBase: 12 SH values + 3 normal sums per cell; then one block of kCells sums of squares
// (kSqBase + cell), the per-sample scalar squared in fixed point, which the stage-2a label reads for the noise of Ef.
inline constexpr u32 kShFloats       = 12;
inline constexpr u32 kPayloadInts    = 15;
inline constexpr u32 kArrivalsBase   = kCells;                               // -> AVER_RC_CELLS
inline constexpr u32 kPayloadBase    = 2 * kCells;                           // -> AVER_RC_PAYLOAD_BASE
inline constexpr u32 kSqBase         = kPayloadBase + kCells * kPayloadInts; // 17 * kCells -> AVER_RC_SQ_BASE
inline constexpr u32 kAccumInts      = kSqBase + kCells;                     // 18 * kCells = 14,155,776
inline constexpr u64 kAccumBytes     = static_cast<u64>(kAccumInts) * 4ull;  // 56,623,104
inline constexpr u32 kCellStride     = 32;
inline constexpr u64 kCellsBytes     = static_cast<u64>(kCells) * kCellStride;   // 25,165,824
inline constexpr u32 kInfoStride     = 80;
inline constexpr u32 kResolveGroupSize   = 64;                               // [numthreads(64,1,1)]
inline constexpr u32 kResolveGroups      = kCells / kResolveGroupSize;       // 12,288
inline constexpr u32 kInfoRing           = 3;     // Upload buffers; matches kRtInstanceRing

inline constexpr u32 kFlagClearAll = 1u;           // RcInfo.hdr0.x bit 0: resolve clears all
// RcInfo.flags bit 1: the floating origin moved; the resolve re-labels cascade 2's live cells by the tag units in bits 8..15
// (x) and 16..23 (y). Cascades 0 and 1 need nothing: see the static_asserts below shiftTagUnits.
inline constexpr u32 kFlagRetagCoarse = 2u;

// ---------------------------------------------------------------- shared structs
// RcInfo: what the trace twins and the resolve both read, once per frame (80 B, StructuredBuffer).
struct RcInfo {
    u32 flags;            // hdr0.x
    u32 frameIndex;       // hdr0.y
    u32 ageStepFrames;    // hdr0.z   one age step per this many frames of no samples
    u32 sampleCap;        // hdr0.w   clamped to kMaxCap by the shader
    f32 alphaMin;         // hdr1.x   floor on the temporal blend factor
    f32 relaxAlpha;       // hdr1.y   temporary higher floor after a lighting event (0: none)
    u32 coarseCaps;       // hdr1.z   cap1 | cap2 << 8 (HLSL reads asuint of the float); cascade 0 uses sampleCap
    f32 netStrength;      // hdr1.w   stage-2 trust strength, 0 = stage-1 weights exactly
    struct Cascade {
        i32 originCell[3];   // window min-corner world cell
        f32 cellSizeCm;      // .w of the HLSL int4, read there with asfloat() / written as asint(cellSizeCm)
    } cas[kCascades];
};
static_assert(sizeof(RcInfo) == kInfoStride, "RcInfo is uint4 + float4 + int4[3] = 80 B");
static_assert(sizeof(RcInfo::Cascade) == 16, "one int4 per cascade");

// RcCell: 32 B. a.xyzw + b.xy = 12 fp16 SH values, b.z = normal word, b.w = meta word.
struct RcCell {
    u32 a[4];
    u32 b[4];
};
static_assert(sizeof(RcCell) == kCellStride, "RcCell is two uint4s");

// ---------------------------------------------------------------- headroom (the fixed-point proof)
// Worst-case sample * kMaxCap * scale: SH fits in int32 with 2x margin, normal fits with 2x margin.
inline constexpr f64 kPi = 3.14159265358979323846;
inline constexpr f64 kWorstSample     = kLMax * 0.488603 * kPi / kMinCos;       // ~491
inline constexpr f64 kWorstShSum      = kWorstSample * kMaxCap * kShScale;      // ~1.03e9
inline constexpr f64 kWorstNormalSum  = 1.0 * kMaxCap * kNScale;                // 4.2e6
static_assert(kWorstShSum * 2.0 < 2147483647.0, "SH sum needs a 2x margin under int32 max");
static_assert(kWorstNormalSum * 2.0 < 2147483647.0, "normal sum needs a 2x margin under int32 max");
// The per-sample scalar of the variance term: luminance(L) * weight / (4 pi), the sample's DC contribution to
// irradiance / PI. Luminance weights sum to 1, so it is bounded by kLMax * (PI / kMinCos) / (4 PI) = 80.
inline constexpr f64 kWorstScalar     = kLMax * (kPi / kMinCos) * kDcToE;       // 80
inline constexpr f64 kWorstSqSum      = kWorstScalar * kWorstScalar * kMaxCap * kSqScale;   // ~8.4e8
static_assert(kWorstSqSum * 2.0 < 2147483647.0, "sum of squares needs a 2x margin under int32 max");
static_assert(kCells % kResolveGroupSize == 0, "one resolve thread per cell, no remainder group");
static_assert(kNEffMax < 16 && kAgeMax < 16, "n_eff and age are 4-bit fields");
static_assert(kAccumInts == 18 * kCells, "counts + arrivals + 15 payload ints + 1 sum of squares per cell");
static_assert(kSqBase == 17 * kCells, "the sum-of-squares block follows the payload");
static_assert(kMaxCap < 256, "coarse caps travel in 8-bit fields");

// ---------------------------------------------------------------- addressing (mirrors rcTagPack / rcCellIndex)
// Arithmetic shift on negative int is defined since C++20, same as HLSL, so negative world cells tag/index consistently.
constexpr u32 tagPack(i32 x, i32 y, i32 z) {
    return (static_cast<u32>(x >> 6) & 255u) | ((static_cast<u32>(y >> 6) & 255u) << 8) |
           ((static_cast<u32>(z >> 6) & 255u) << 16);
}
constexpr u32 cellIndex(u32 cascade, i32 x, i32 y, i32 z) {
    const u32 tx = static_cast<u32>(x) & kResMask, ty = static_cast<u32>(y) & kResMask,
              tz = static_cast<u32>(z) & kResMask;
    return cascade * kCellsPerCascade + ((tz * kRes) + ty) * kRes + tx;
}
// ---------------------------------------------------------------- the floating origin (LARGE_WORLD 2.2.6)
// A rebase moves every local position by -(k * 1,638,400) cm. A world cell (local) therefore moves by -(k * 1,638,400 / size),
// its toroidal texel (cell & 63) by a multiple of 64 (none), and its tag byte ((cell >> 6) & 255) by 1,638,400 / size / 64 per step.
// That is 1024 for the 25 cm cascade and 256 for the 100 cm one (both 0 mod 256: those caches are untouched) and 64 for the
// 400 cm one, which the resolve re-labels once (kFlagRetagCoarse). Everything below is that arithmetic, pinned at compile time.
inline constexpr i64 kOriginStepCm = 1'638'400;
inline constexpr i64 tagStepPerRegion(u32 cascade) {
    constexpr i64 cell[kCascades] = {25, 100, 400};
    return kOriginStepCm / cell[cascade] / 64;
}
static_assert(kOriginStepCm % 25 == 0 && kOriginStepCm % 100 == 0 && kOriginStepCm % 400 == 0, "a step is a whole number of cells");
static_assert((kOriginStepCm / 25) % 64 == 0 && (kOriginStepCm / 100) % 64 == 0 && (kOriginStepCm / 400) % 64 == 0,
              "a step is a whole number of 64-cell tori, so the toroidal texel of a cell never changes");
static_assert(tagStepPerRegion(0) % 256 == 0 && tagStepPerRegion(1) % 256 == 0, "cascades 0 and 1 keep their tags");
static_assert(tagStepPerRegion(2) == 64, "cascade 2's tag byte moves by 64 per region step");
// Tag units to subtract from cascade 2's tag byte after `steps` region steps along one axis (steps may be negative).
constexpr u32 shiftTagUnits(i64 steps) { return static_cast<u32>(((steps % 4) + 4) % 4) * 64u; }
// A meta word re-labelled by the resolve: dx / dy are shiftTagUnits of the x / y steps; tag z and the rest are untouched.
constexpr u32 retagMeta(u32 meta, u32 dx, u32 dy) {
    return (meta & 0xFF000000u) | (meta & 0x00FF0000u) | ((((meta >> 8) & 255u) - dy) & 255u) << 8 | (((meta & 255u) - dx) & 255u);
}
// The RcInfo flags word a frame carries after the origin moved by (dxSteps, dySteps) region steps; 0 when nothing to relabel.
constexpr u32 originShiftFlags(i64 dxSteps, i64 dySteps) {
    const u32 dx = shiftTagUnits(dxSteps), dy = shiftTagUnits(dySteps);
    return (dx | dy) ? (kFlagRetagCoarse | (dx << 8) | (dy << 16)) : 0u;
}

// Window min-corner world cell from camera position and cell size, in world units.
inline i32 snapOrigin(f32 camCm, f32 cellSizeCm) {
    return static_cast<i32>(std::floor(camCm / cellSizeCm)) - kHalfRes;
}

// ---------------------------------------------------------------- meta word
// tag (bits 0-23) | n_eff (24-27, 0 = empty) | age (28-31).
constexpr u32 metaPack(u32 tag, u32 nEff, u32 age) {
    return (tag & 0xFFFFFFu) | ((nEff & 15u) << 24) | ((age & 15u) << 28);
}
constexpr u32 metaTag(u32 m)  { return m & 0xFFFFFFu; }
constexpr u32 metaNEff(u32 m) { return (m >> 24) & 15u; }
constexpr u32 metaAge(u32 m)  { return m >> 28; }
// Valid iff it has samples, matches the expected world cell tag, and has not aged out.
constexpr bool cellValid(u32 meta, u32 expectedTag) {
    return metaNEff(meta) > 0 && metaTag(meta) == expectedTag && metaAge(meta) < kAgeMax;
}

// ---------------------------------------------------------------- SH basis (real, L1, order [c0, cY, cZ, cX])
inline void shBasis(const f32 dir[3], f32 y[4]) {
    y[0] = 0.282095f;
    y[1] = 0.488603f * dir[1];
    y[2] = 0.488603f * dir[2];
    y[3] = 0.488603f * dir[0];
}
// Cosine-convolved irradiance / PI for normal n, clamped >= 0 (Ramamoorthi-Hanrahan).
inline void shIrradianceOverPi(const f32 c[4][3], const f32 n[3], f32 out[3]) {
    for (int ch = 0; ch < 3; ++ch) {
        const f32 v = 0.282095f * c[0][ch] +
                      (2.0f / 3.0f) * 0.488603f * (n[1] * c[1][ch] + n[2] * c[2][ch] + n[0] * c[3][ch]);
        out[ch] = v > 0.0f ? v : 0.0f;
    }
}

// ---------------------------------------------------------------- fp16 (IEEE binary16, round to nearest even)
inline u32 floatToHalf(f32 f) {
    u32 x; std::memcpy(&x, &f, 4);
    const u32 sign = (x >> 16) & 0x8000u;
    const u32 exp  = (x >> 23) & 0xFFu;
    u32 mant = x & 0x7FFFFFu;
    if (exp == 0xFFu) return sign | 0x7C00u | (mant ? 0x200u : 0u);   // inf / nan
    const i32 e = static_cast<i32>(exp) - 127 + 15;
    if (e >= 31) return sign | 0x7C00u;                               // overflow -> inf
    if (e <= 0) {
        if (e < -10) return sign;                                     // underflow -> 0
        mant |= 0x800000u;
        const u32 shift = static_cast<u32>(14 - e);
        u32 h = mant >> shift;
        const u32 rem = mant & ((1u << shift) - 1u), half = 1u << (shift - 1u);
        if (rem > half || (rem == half && (h & 1u))) ++h;
        return sign | h;
    }
    u32 h = (static_cast<u32>(e) << 10) | (mant >> 13);
    const u32 rem = mant & 0x1FFFu;
    if (rem > 0x1000u || (rem == 0x1000u && (h & 1u))) ++h;           // may carry into the exponent: correct
    return sign | h;
}
inline f32 halfToFloat(u32 h) {
    const u32 sign = (h & 0x8000u) << 16;
    u32 exp = (h >> 10) & 31u, mant = h & 0x3FFu, bits;
    if (exp == 0) {
        if (mant == 0) bits = sign;
        else {   // subnormal half -> normal float
            exp = 127 - 15 + 1;
            while (!(mant & 0x400u)) { mant <<= 1; --exp; }
            mant &= 0x3FFu;
            bits = sign | (exp << 23) | (mant << 13);
        }
    } else if (exp == 31) bits = sign | 0x7F800000u | (mant << 13);
    else bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    f32 f; std::memcpy(&f, &bits, 4);
    return f;
}

// 12 SH floats (k = 3*j + ch) <-> six uints of a cell (a.xyzw, b.xy): low half = even k.
inline void packSh(const f32 sh[kShFloats], u32 w[6]) {
    for (u32 i = 0; i < 6; ++i) w[i] = floatToHalf(sh[2 * i]) | (floatToHalf(sh[2 * i + 1]) << 16);
}
inline void unpackSh(const u32 w[6], f32 sh[kShFloats]) {
    for (u32 i = 0; i < 6; ++i) { sh[2 * i] = halfToFloat(w[i] & 0xFFFFu); sh[2 * i + 1] = halfToFloat(w[i] >> 16); }
}

// ---------------------------------------------------------------- normal word
// octU (10 bits) | octV (10) << 10 | len (6) << 20 | trust (6) << 26: octahedral direction, mean-normal
// length (1 = flat, ~0 = noise; resolution 1/63) and stage-2 trust (63 = 1.0).
inline u32 packNormal(const f32 v[3], u32 trust6 = kTrustMax) {
    const f32 len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    f32 n[3] = {0.0f, 0.0f, 1.0f};
    if (len > 1e-6f) { n[0] = v[0] / len; n[1] = v[1] / len; n[2] = v[2] / len; }
    const f32 l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
    const f32 l1c = l1 > 1e-6f ? l1 : 1e-6f;
    f32 ox = n[0] / l1c, oy = n[1] / l1c;
    if (n[2] < 0.0f) {
        const f32 px = (1.0f - std::fabs(oy)) * (ox >= 0.0f ? 1.0f : -1.0f);
        const f32 py = (1.0f - std::fabs(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
        ox = px; oy = py;
    }
    auto q = [](f32 x, f32 steps) {
        f32 t = (x * 0.5f + 0.5f) * steps + 0.5f;
        if (t < 0.0f) t = 0.0f;
        if (t > steps) t = steps;
        return static_cast<u32>(t);
    };
    const f32 lc = len > 1.0f ? 1.0f : len;
    const u32 t6 = trust6 > kTrustMax ? kTrustMax : trust6;
    return q(ox, 1023.0f) | (q(oy, 1023.0f) << 10) |
           (static_cast<u32>(lc * static_cast<f32>(kLenMax) + 0.5f) << 20) | (t6 << 26);
}
inline u32 normalTrust6(u32 w) { return w >> 26; }
inline void unpackNormal(u32 w, f32 dir[3], f32& len, f32& trust) {
    const f32 ox = static_cast<f32>(w & 1023u) * (1.0f / 1023.0f) * 2.0f - 1.0f;
    const f32 oy = static_cast<f32>((w >> 10) & 1023u) * (1.0f / 1023.0f) * 2.0f - 1.0f;
    len   = static_cast<f32>((w >> 20) & 63u) * (1.0f / static_cast<f32>(kLenMax));
    trust = static_cast<f32>(w >> 26) * (1.0f / static_cast<f32>(kTrustMax));
    f32 x = ox, y = oy, z = 1.0f - std::fabs(ox) - std::fabs(oy);
    if (z < 0.0f) {
        const f32 tx = (1.0f - std::fabs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const f32 ty = (1.0f - std::fabs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = tx; y = ty;
    }
    const f32 l = std::sqrt(x * x + y * y + z * z);
    dir[0] = x / l; dir[1] = y / l; dir[2] = z / l;
}
inline void unpackNormal(u32 w, f32 dir[3], f32& len) { f32 trust; unpackNormal(w, dir, len, trust); }

// ---------------------------------------------------------------- scatter and lookup (mirrors voxi_neurac_io / _lookup.hlsli)
// Per-cell per-frame sample cap of a cascade (mirrors rcCapFor): cascade 0 from sampleCap, the coarse two from coarseCaps.
inline u32 capForCascade(const RcInfo& info, u32 c) {
    const u32 cap = c == 0 ? info.sampleCap : (c == 1 ? (info.coarseCaps & 255u) : ((info.coarseCaps >> 8) & 255u));
    return cap < kMaxCap ? cap : kMaxCap;
}

// Load-adaptive accept rate from last frame's arrival count.
inline f32 acceptRate(u32 prev, u32 cap) {
    if (prev <= cap) return 1.0f;
    const f32 r = static_cast<f32>(cap) / static_cast<f32>(prev), lo = static_cast<f32>(kMinAcceptRate);
    return r > lo ? r : lo;
}

// Murmur3 finalizer over the sample's position and direction bits, top 16 bits (mirrors rcHash16).
inline u32 hash16(const f32 hitPos[3], const f32 dir[3], u32 cascade) {
    u32 a[3], b[3];
    std::memcpy(a, hitPos, 12);
    std::memcpy(b, dir, 12);
    u32 h = 0x9E3779B9u * (cascade + 1u);
    h = (h ^ a[0]) * 0x85EBCA6Bu;
    h = (h ^ a[1]) * 0xC2B2AE35u;
    h = (h ^ a[2]) * 0x85EBCA6Bu;
    h = (h ^ b[0]) * 0xC2B2AE35u;
    h = (h ^ b[1]) * 0x85EBCA6Bu;
    h = (h ^ b[2]) * 0xC2B2AE35u;
    h ^= h >> 16;  h *= 0x85EBCA6Bu;
    h ^= h >> 13;  h *= 0xC2B2AE35u;
    h ^= h >> 16;
    return h >> 16;
}

// The cell a sample trains in this cascade: binned at the lookup's own cell, floor((hit + N size/2) / size).
// Returns false when that cell is outside the interior [kMarginCells, kRes - 1 - kMarginCells] of the window.
inline bool scatterCell(const f32 hitPos[3], const f32 N[3], u32 /*cascade*/, const i32 origin[3], f32 size,
                        i32 outCell[3]) {
    bool inside = true;
    for (int a = 0; a < 3; ++a) {
        outCell[a] = static_cast<i32>(std::floor((hitPos[a] + N[a] * (0.5f * size)) / size));
        const i32 rel = outCell[a] - origin[a];
        if (rel < static_cast<i32>(kMarginCells) || rel > static_cast<i32>(kRes - 1 - kMarginCells)) inside = false;
    }
    return inside;
}

// One corner's weight in a cascade lookup (mirrors rcLookupCascade). dotNN = dot(cellNormal, N), unsaturated;
// trust in [0,1]. netStrength 0 gives the stage-1 weight exactly.
inline f32 lookupCornerWeight(f32 tri, f32 dotNN, f32 len, u32 nEff, u32 age, f32 trust, f32 netStrength) {
    const f32 nd = dotNN * static_cast<f32>(kNormalK);
    const f32 sat = nd < 0.0f ? 0.0f : (nd > 1.0f ? 1.0f : nd);
    const f32 ne = static_cast<f32>(nEff) * 0.25f;
    const f32 trustF = 1.0f + (trust - 1.0f) * netStrength;   // lerp(1, trust, netStrength)
    return tri * sat * len * (ne < 1.0f ? ne : 1.0f) *
           (1.0f - static_cast<f32>(age) / static_cast<f32>(kAgeMax)) * trustF;
}

}  // namespace aver::voxi::neurac
