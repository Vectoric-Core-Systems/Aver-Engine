// RadianceCacheLayout -- every constant, struct layout and bit-packing rule of the Voxi radiance cache
// (docs/rendering/RADIANCE_CACHE.md), in ONE header-only place.
//
// WHY A HEADER OF ITS OWN, AND WHY IT DEPENDS ON NOTHING BUT Types.hpp: three parties must agree
// byte for byte -- RadianceCache.cpp (buffer sizes, the RcInfo it writes), voxi_radiance_cache*.hlsli /
// voxi_radiance_cache_resolve.hlsl (the cell layout they read and write), and RadianceCacheTest (which
// runs the arithmetic on the CPU with no GPU, linking Aver.Core alone, exactly like GiVisibilityTest).
// HLSL cannot include a C++ header, so the HLSL mirrors the AVER_RC_* numbers as #defines and the test
// greps those literals against the constants here; a number that drifts on either side fails a test
// instead of silently corrupting a fixed-point sum.
//
// WHAT THE CACHE IS, IN ONE PARAGRAPH: kCascades camera-centred 64^3 grids of cells (0.25 m / 1 m / 4 m).
// ReSTIR GI's traced second-bounce rays scatter fixed-point first-order SH of incident radiance into an
// ACCUMULATOR (integer InterlockedAdd, so the sum is order-independent); a once-per-frame RESOLVE turns
// that into the running mean stored in 32-byte CELLS; pixels that did not trace read the cells.
//
// UNITS: the engine's world unit is the centimetre (the resolve and the scatter both take cell sizes in
// cm), so a "cell" is kCellSizeCm[c] centimetres on a side.
#pragma once

#include "aver/core/Types.hpp"

#include <cmath>
#include <cstring>

namespace aver::voxi::radiancecache {

// ---------------------------------------------------------------- geometry
inline constexpr u32 kCascades         = 3;
inline constexpr u32 kRes              = 64;                       // cells per axis per cascade
inline constexpr u32 kResMask          = kRes - 1;                 // texel = worldCell & kResMask
inline constexpr u32 kCellsPerCascade  = kRes * kRes * kRes;       // 262,144
inline constexpr u32 kCells            = kCascades * kCellsPerCascade;   // 786,432
inline constexpr f32 kCellSizeCm[kCascades] = {25.0f, 100.0f, 400.0f};
// The window is centred on the camera: its min-corner world cell is floor(cam / cellSize) - kRes/2.
inline constexpr i32 kHalfRes          = static_cast<i32>(kRes / 2);
// Cells at the window edge that a lookup refuses to use (a corner outside the window is skipped), and
// the width over which a cascade's weight fades to zero so two cascades cross-fade without a seam.
inline constexpr u32 kMarginCells      = 1;
inline constexpr u32 kCrossFadeCells   = 4;

// ---------------------------------------------------------------- tunables the HLSL mirrors (AVER_RC_*)
// Fixed-point scale of an SH sum: sample * kShScale is rounded to an int and InterlockedAdd'ed.
inline constexpr f64 kShScale    = 32768.0;     // 2^15      -> AVER_RC_SH_SCALE
// Fixed-point scale of the summed unit normal (planarity + direction of the cell's surface).
inline constexpr f64 kNScale     = 65536.0;     // 2^16      -> AVER_RC_N_SCALE
// Samples that may contribute to one cell per accumulation epoch (frame). The scatter bumps a count
// atomic first and only the first kMaxCap attempts add payload, which is what bounds the headroom below.
inline constexpr u32 kMaxCap     = 64;          //           -> AVER_RC_MAX_CAP
// Per-component radiance clamp before projection; a fireflied sample cannot dominate a cell.
inline constexpr f64 kLMax       = 32.0;        //           -> AVER_RC_LMAX
// Floor on the cosine in the PI/cos pdf weight. A bias (1% of cosine-sampled directions lie below 0.1),
// not a rejection: it bounds the worst-case sample weight, which is what the headroom needs.
inline constexpr f64 kMinCos     = 0.1;         //           -> AVER_RC_MIN_COS
inline constexpr u32 kNEffMax    = 15;          // 4-bit field -> AVER_RC_NEFF_MAX
inline constexpr u32 kAgeMax     = 15;          // 4-bit field -> AVER_RC_AGE_MAX
// How hard the lookup rejects corner cells facing away from the shaded normal: weight *=
// saturate(dot(cellDir, N) * kNormalK).
inline constexpr f64 kNormalK    = 2.0;         //           -> AVER_RC_NORMAL_K

// ---------------------------------------------------------------- accumulator layout (int32 elements)
// counts region [0, kCells): one count per cell. Payload region from kCells: kPayloadInts per cell,
// k = 0..11 SH (order [c0.rgb, cY.rgb, cZ.rgb, cX.rgb]), k = 12..14 summed normal xyz.
// WHY the counts are a separate contiguous region and not the first int of a 16-int record: the resolve
// early-outs on count == 0, and most cells are empty. Streaming 3 MB of counts is far cheaper than
// touching 786k sparse 64-byte lines to find that out.
inline constexpr u32 kShFloats       = 12;
inline constexpr u32 kPayloadInts    = 15;
inline constexpr u32 kAccumInts      = kCells + kCells * kPayloadInts;       // 12,582,912
inline constexpr u64 kAccumBytes     = static_cast<u64>(kAccumInts) * 4ull;  // 50,331,648
inline constexpr u32 kCellStride     = 32;
inline constexpr u64 kCellsBytes     = static_cast<u64>(kCells) * kCellStride;   // 25,165,824
inline constexpr u32 kInfoStride     = 80;
inline constexpr u32 kResolveGroupSize   = 64;                               // [numthreads(64,1,1)]
inline constexpr u32 kResolveGroups      = kCells / kResolveGroupSize;       // 12,288, one thread per cell
inline constexpr u32 kInfoRing           = 3;     // Upload buffers; the renderer's kRtInstanceRing

inline constexpr u32 kFlagClearAll = 1u;           // RcInfo.hdr0.x bit 0: resolve zeroes everything

// ---------------------------------------------------------------- shared structs
// RcInfo: what the trace twins and the resolve both read, once per frame (80 B, StructuredBuffer).
struct RcInfo {
    u32 flags;            // hdr0.x
    u32 frameIndex;       // hdr0.y
    u32 ageStepFrames;    // hdr0.z   a cell ages one step per this many frames of no samples
    u32 sampleCap;        // hdr0.w   clamped to kMaxCap by the shader
    f32 alphaMin;         // hdr1.x   floor on the temporal blend factor
    f32 reserved[3];      // hdr1.yzw, zero
    struct Cascade {
        i32 originCell[3];   // window min-corner world cell
        f32 cellSizeCm;      // .w of the HLSL int4, read there with asfloat() / written as asint(cellSizeCm)
    } cas[kCascades];
};
static_assert(sizeof(RcInfo) == kInfoStride, "RcInfo is uint4 + float4 + int4[3] = 80 B");
static_assert(sizeof(RcInfo::Cascade) == 16, "one int4 per cascade");

// RcCell: 32 B. a.xyzw + b.xy = 12 fp16 SH values (6 uints, low half first, coefficient order as the
// accumulator), b.z = normal word, b.w = meta word.
struct RcCell {
    u32 a[4];
    u32 b[4];
};
static_assert(sizeof(RcCell) == kCellStride, "RcCell is two uint4s");

// ---------------------------------------------------------------- headroom (the fixed-point proof)
// Worst single sample of one SH component: L = kLMax, basis <= 0.488603 (the L1 terms), weight
// PI / kMinCos. The cap bounds how many such samples one cell sums per epoch; times the scale gives the
// largest value an int32 InterlockedAdd ever sees. static_assert so changing a constant cannot silently
// overflow: a wrapped sum would be a wrong colour, not a crash.
inline constexpr f64 kPi = 3.14159265358979323846;
inline constexpr f64 kWorstSample     = kLMax * 0.488603 * kPi / kMinCos;       // ~491
inline constexpr f64 kWorstShSum      = kWorstSample * kMaxCap * kShScale;      // ~1.03e9
inline constexpr f64 kWorstNormalSum  = 1.0 * kMaxCap * kNScale;                // 4.2e6
static_assert(kWorstShSum * 2.0 < 2147483647.0, "SH sum needs a 2x margin under int32 max");
static_assert(kWorstNormalSum * 2.0 < 2147483647.0, "normal sum needs a 2x margin under int32 max");
static_assert(kCells % kResolveGroupSize == 0, "one resolve thread per cell, no remainder group");
static_assert(kNEffMax < 16 && kAgeMax < 16, "n_eff and age are 4-bit fields");

// ---------------------------------------------------------------- addressing (mirrors rcTagPack / rcCellIndex)
// Arithmetic shift on a negative int: defined since C++20, and HLSL's int >> is arithmetic too, so a
// negative world cell (west of the origin) tags and indexes the same way on both sides.
constexpr u32 tagPack(i32 x, i32 y, i32 z) {
    return (static_cast<u32>(x >> 6) & 255u) | ((static_cast<u32>(y >> 6) & 255u) << 8) |
           ((static_cast<u32>(z >> 6) & 255u) << 16);
}
constexpr u32 cellIndex(u32 cascade, i32 x, i32 y, i32 z) {
    const u32 tx = static_cast<u32>(x) & kResMask, ty = static_cast<u32>(y) & kResMask,
              tz = static_cast<u32>(z) & kResMask;
    return cascade * kCellsPerCascade + ((tz * kRes) + ty) * kRes + tx;
}
// The window's min-corner world cell for a camera position (cm) and cell size (cm).
inline i32 snapOrigin(f32 camCm, f32 cellSizeCm) {
    return static_cast<i32>(std::floor(camCm / cellSizeCm)) - kHalfRes;
}

// ---------------------------------------------------------------- meta word
// tag (bits 0-23) | n_eff (24-27, 0 = empty) | age (28-31, in units of ageStepFrames).
constexpr u32 metaPack(u32 tag, u32 nEff, u32 age) {
    return (tag & 0xFFFFFFu) | ((nEff & 15u) << 24) | ((age & 15u) << 28);
}
constexpr u32 metaTag(u32 m)  { return m & 0xFFFFFFu; }
constexpr u32 metaNEff(u32 m) { return (m >> 24) & 15u; }
constexpr u32 metaAge(u32 m)  { return m >> 28; }
// A cell is valid for lookup iff it holds samples, belongs to the world cell the texel currently stands
// for (a scrolled-in cell still carries its previous occupant's tag), and has not aged out.
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
// Cosine-convolved irradiance / PI for normal n, clamped >= 0 (Ramamoorthi-Hanrahan: A0 = PI, A1 = 2PI/3).
// c[j][ch]: coefficient j, channel ch.
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

// 12 SH floats (k = 3*j + ch) <-> the six uints of a cell (a.xyzw, b.xy): low half = even k.
inline void packSh(const f32 sh[kShFloats], u32 w[6]) {
    for (u32 i = 0; i < 6; ++i) w[i] = floatToHalf(sh[2 * i]) | (floatToHalf(sh[2 * i + 1]) << 16);
}
inline void unpackSh(const u32 w[6], f32 sh[kShFloats]) {
    for (u32 i = 0; i < 6; ++i) { sh[2 * i] = halfToFloat(w[i] & 0xFFFFu); sh[2 * i + 1] = halfToFloat(w[i] >> 16); }
}

// ---------------------------------------------------------------- normal word
// octU (11 bits) | octV (11) << 11 | len (10) << 22: an octahedral direction plus the mean-normal vector's
// length (1 = every sample agreed on one flat surface, ~0 = a corner or noise), unorm 10.
inline u32 packNormal(const f32 v[3]) {
    const f32 len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    f32 n[3] = {0.0f, 1.0f, 0.0f};
    if (len > 1e-6f) { n[0] = v[0] / len; n[1] = v[1] / len; n[2] = v[2] / len; }
    const f32 l1 = std::fabs(n[0]) + std::fabs(n[1]) + std::fabs(n[2]);
    f32 ox = n[0] / l1, oy = n[1] / l1;
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
    return q(ox, 2047.0f) | (q(oy, 2047.0f) << 11) | (static_cast<u32>(lc * 1023.0f + 0.5f) << 22);
}
inline void unpackNormal(u32 w, f32 dir[3], f32& len) {
    const f32 ox = static_cast<f32>(w & 2047u) / 2047.0f * 2.0f - 1.0f;
    const f32 oy = static_cast<f32>((w >> 11) & 2047u) / 2047.0f * 2.0f - 1.0f;
    len = static_cast<f32>(w >> 22) / 1023.0f;
    f32 x = ox, y = oy, z = 1.0f - std::fabs(ox) - std::fabs(oy);
    if (z < 0.0f) {
        const f32 tx = (1.0f - std::fabs(y)) * (x >= 0.0f ? 1.0f : -1.0f);
        const f32 ty = (1.0f - std::fabs(x)) * (y >= 0.0f ? 1.0f : -1.0f);
        x = tx; y = ty;
    }
    const f32 l = std::sqrt(x * x + y * y + z * z);
    dir[0] = x / l; dir[1] = y / l; dir[2] = z / l;
}

}  // namespace aver::voxi::radiancecache
