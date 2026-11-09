#pragma once
// The arithmetic of a streamed mip window: byte sizes, which mips may be the top of a window, and the
// footprint-to-mip conversion. Pure integer/float maths with no device, no file and no state, so the
// planner (TexturePlanner.hpp) and the .octex reader (TextureCache.hpp) share one definition and a
// test can check each against hand-computed numbers. See docs/plans/wave4/TEXTURE_STREAMING.md 2.2.
#include "aver/core/Types.hpp"

#include <cmath>

namespace aver::assets {

// A chain never has more than this many levels (a 16384 texture has 15); the .octex reader refuses more.
inline constexpr u32 kMaxTextureMips = 32;

// Size of one BC7 mip level: 4x4 blocks of 16 bytes (every BC7 variant the engine writes).
inline constexpr u64 bc7MipBytes(u32 w, u32 h) {
    const u64 bw = (static_cast<u64>(w) + 3) / 4;
    const u64 bh = (static_cast<u64>(h) + 3) / 4;
    return bw * bh * 16;
}

inline constexpr u32 mipExtent(u32 full, u32 mip) {
    const u32 e = mip >= 32 ? 0u : (full >> mip);
    return e > 0 ? e : 1u;
}

// Bytes of the BC7 mips [first, mips) of a w x h chain: the resident size of a window whose top is `first`.
inline constexpr u64 windowBytes(u32 w, u32 h, u32 first, u32 mips) {
    u64 total = 0;
    for (u32 m = first; m < mips; ++m) total += bc7MipBytes(mipExtent(w, m), mipExtent(h, m));
    return total;
}

// Bit m is set when mip m may be the top of a window: both extents are multiples of 4 and at least 4 (the BC7
// createTexture rule). Mip 0 is always set, so the full chain is always representable. Powers of two qualify down
// to 4x4; 1500x1500 has only mip 0.
inline constexpr u32 allowedTopMask(u32 w, u32 h, u32 mips) {
    u32 mask = 1u;
    for (u32 m = 1; m < mips && m < 32; ++m) {
        const u32 ew = w >> m, eh = h >> m;
        if (ew >= 4 && eh >= 4 && (ew % 4) == 0 && (eh % 4) == 0) mask |= 1u << m;
    }
    return mask;
}

// The largest allowed mip index that is <= `first` (so at least as detailed as asked), clamped into the chain.
inline constexpr u32 snapToAllowed(u32 first, u32 allowedMask, u32 mips) {
    u32 m = first < mips ? first : (mips > 0 ? mips - 1 : 0);
    while (m > 0 && !(allowedMask & (1u << m))) --m;
    return m;
}

// The smallest mip whose larger extent is <= maxDim, as a legal top (snapped); the last mip when none is.
inline constexpr u32 firstMipWithin(u32 w, u32 h, u32 mips, u32 allowedMask, u32 maxDim) {
    u32 m = 0;
    while (m + 1 < mips) {
        const u32 ew = mipExtent(w, m), eh = mipExtent(h, m);
        if ((ew > eh ? ew : eh) <= maxDim) break;
        ++m;
    }
    return snapToAllowed(m, allowedMask, mips);
}

// The footprint is the smallest UV change per pixel any sampled pixel needed, so fp * max(w, h) is the
// texels-per-pixel ratio and log2 of it is the mip the sampler reads. Floor, because trilinear reads floor and
// floor+1. `biasMips` shifts the answer (negative buys detail). A non-positive or non-finite footprint carries
// no information and wants the least detail.
inline u32 wantedFirstMip(f32 footprint, u32 fullW, u32 fullH, u32 mips, i32 biasMips, u32 allowedMask) {
    if (mips == 0) return 0;
    if (!(footprint > 0.0f) || !std::isfinite(footprint)) return snapToAllowed(mips - 1, allowedMask, mips);
    const u32 dim = fullW > fullH ? fullW : fullH;
    const f64 lod = std::log2(static_cast<f64>(footprint) * static_cast<f64>(dim));
    f64 first = std::floor(lod) + static_cast<f64>(biasMips);
    if (first < 0.0) first = 0.0;
    if (first > static_cast<f64>(mips - 1)) first = static_cast<f64>(mips - 1);
    return snapToAllowed(static_cast<u32>(first), allowedMask, mips);
}

// Which textures may be streamed at all: BC7, big enough to matter (larger extent >= 512), a real chain, and at
// least one legal top above mip 0. Everything else stays fully resident (pinned) exactly as it is today.
inline constexpr u32 kStreamableMinDim = 512;
inline constexpr bool textureStreamable(bool isBc7, u32 w, u32 h, u32 mips) {
    if (!isBc7 || mips < 2) return false;
    if ((w > h ? w : h) < kStreamableMinDim) return false;
    return allowedTopMask(w, h, mips) > 1u;
}

} // namespace aver::assets
