// voxi_radiance_cache_resolve.hlsl -- the radiance cache's once-per-frame RESOLVE (and its rare clear).
// See RadianceCache.hpp / docs/rendering/RADIANCE_CACHE.md.
//
// WHAT IT DOES: the trace twins scatter fixed-point SH sums into gRcAccum (voxi_radiance_cache_io.hlsli);
// this pass, one thread per cell, turns those sums into the running mean the lookup reads in gRcCells,
// then zeroes the sums for the next frame. The ONLY thing it shares with the scatter is the layout in
// voxi_radiance_cache.hlsli, which is pure maths and declares no resources, so it can be included here
// BEFORE this file's own declarations (the Voxi table's t22/u20/u21 do not exist in this pipeline; this
// pass has its own layout: t0 info, u0 accumulator, u1 cells).
//
// ACCUMULATOR LAYOUT (RadianceCacheLayout.hpp): counts region [0, cells), then 15 ints per cell at
// cells + cell*15 + k (k 0..11 SH [c0.rgb, cY.rgb, cZ.rgb, cX.rgb], 12..14 summed normal).
//
// WHY THE COUNTS ARE READ FIRST: ~90% of cells are empty on any frame. Reading one int per cell is a
// contiguous 3 MB stream; touching the cell and payload lines of an empty cell would not be.
#include "voxi_radiance_cache.hlsli"

StructuredBuffer<RcInfo>      gRcInfo  : register(t0);
RWStructuredBuffer<int>       gRcAccum : register(u0);
RWStructuredBuffer<RcCell>    gRcCells : register(u1);

// This file's own copies of the geometry numbers (RadianceCacheLayout.hpp is the source; RCR_ prefix so
// they cannot collide with whatever the shared header #defines).
#define RCR_RES      64u
#define RCR_PER_CASC 262144u     // 64^3
#define RCR_CELLS    786432u     // 3 * 64^3
#define RCR_PAYLOAD  15u         // ints per cell after the counts region

#define RCR_FLAG_CLEAR_ALL 1u

// n_eff and age live in the meta word beside the tag (RcCell.b.w): tag bits 0-23, n_eff 24-27, age 28-31.
uint rcrMeta(uint tag, uint nEff, uint age) { return (tag & 0xFFFFFFu) | ((nEff & 15u) << 24) | ((age & 15u) << 28); }

[numthreads(64, 1, 1)]
void CSRcResolve(uint3 dtid : SV_DispatchThreadID)
{
    const uint cell = dtid.x;
    if (cell >= RCR_CELLS) return;

    const RcInfo info = gRcInfo[0];
    const uint payloadBase = RCR_CELLS + cell * RCR_PAYLOAD;

    // CLEAR_ALL: first frame / requestReset(). Zero the cell, its count and its payload and stop.
    if ((info.hdr0.x & RCR_FLAG_CLEAR_ALL) != 0u)
    {
        RcCell zero;
        zero.a = uint4(0, 0, 0, 0);
        zero.b = uint4(0, 0, 0, 0);
        gRcCells[cell] = zero;
        gRcAccum[cell] = 0;
        [unroll] for (uint k = 0; k < RCR_PAYLOAD; ++k) gRcAccum[payloadBase + k] = 0;
        return;
    }

    // Contributing samples: the scatter counts every attempt but only the first `cap` add payload.
    const uint cap = min(info.hdr0.w, (uint)AVER_RC_MAX_CAP);
    const uint n   = min((uint)gRcAccum[cell], cap);

    // Aging runs on a fixed cadence so the per-frame cost of an idle cell is the single count read.
    const uint ageStep   = info.hdr0.z;
    const bool agingFrame = ageStep != 0u && (info.hdr0.y % ageStep) == 0u;

    if (n == 0u && !agingFrame) return;

    // Which world cell this texel stands for NOW: the window slides one cell per camera step, and a texel
    // is world & 63 per axis, so the world cell is the window's origin plus the texel's offset from it.
    // int & on a negative value is two's complement, which is exactly the modulo wanted.
    const uint cascade = cell / RCR_PER_CASC;
    const uint local   = cell - cascade * RCR_PER_CASC;
    const int3 texel   = int3(local & 63u, (local >> 6) & 63u, local >> 12);
    const int4 cas     = info.cas[cascade];
    const int3 world   = cas.xyz + ((texel - cas.xyz) & 63);
    const uint expectedTag = rcTagPack(world);

    RcCell stored = gRcCells[cell];
    const uint meta  = stored.b.w;
    const uint nEff  = (meta >> 24) & 15u;
    const uint age   = meta >> 28;
    const bool valid = nEff > 0u && (meta & 0xFFFFFFu) == expectedTag;

    if (n == 0u)
    {
        // Aging frame, no samples. An empty cell has nothing to age; an occupied one (even one carrying
        // a stale tag from before the window slid) gets one step older and is emptied at the limit.
        if (nEff == 0u) return;
        const uint nextAge = age + 1u;
        if (nextAge >= (uint)AVER_RC_AGE_MAX)
        {
            RcCell empty;
            empty.a = uint4(0, 0, 0, 0);
            empty.b = uint4(0, 0, 0, 0);
            gRcCells[cell] = empty;
        }
        else
        {
            gRcCells[cell].b.w = rcrMeta(meta & 0xFFFFFFu, nEff, nextAge);   // meta only
        }
        return;
    }

    // Sample mean: the sums are fixed point (x scale) over n contributing samples.
    const float shInv = 1.0 / ((float)n * (float)AVER_RC_SH_SCALE);
    float3 mean[4];
    [unroll] for (uint j = 0; j < 4; ++j)
    {
        mean[j] = float3((float)gRcAccum[payloadBase + j * 3u + 0u],
                         (float)gRcAccum[payloadBase + j * 3u + 1u],
                         (float)gRcAccum[payloadBase + j * 3u + 2u]) * shInv;
    }
    const float nInv = 1.0 / ((float)n * (float)AVER_RC_N_SCALE);
    float3 nMean = float3((float)gRcAccum[payloadBase + 12u],
                          (float)gRcAccum[payloadBase + 13u],
                          (float)gRcAccum[payloadBase + 14u]) * nInv;

    float3 sh[4];
    [unroll] for (uint c = 0; c < 4; ++c) sh[c] = mean[c];
    uint newNEff = 1u;
    if (valid)
    {
        // alpha = max(1/(n_eff+1), alphaMin): a fresh cell converges at 1/k (the exact running mean),
        // a mature one keeps a floor so a lighting change still shows. Per frame, not per sample.
        const float alpha = max(1.0 / ((float)nEff + 1.0), info.hdr1.x);
        float3 old[4];
        rcUnpackSh(stored.a, stored.b.xy, old);
        float oldLen;
        const float3 oldDir = rcUnpackNormal(stored.b.z, oldLen);
        [unroll] for (uint j = 0; j < 4; ++j) sh[j] = lerp(old[j], mean[j], alpha);
        nMean = lerp(oldDir * oldLen, nMean, alpha);
        newNEff = min(nEff + 1u, (uint)AVER_RC_NEFF_MAX);
    }
    // An invalid cell (empty, or a previous occupant's tag from before the window slid) is replaced
    // outright: blending a different world cell's light into this one would be a leak.

    uint w6[6];
    rcPackSh(sh, w6);
    RcCell outCell;
    outCell.a = uint4(w6[0], w6[1], w6[2], w6[3]);
    outCell.b = uint4(w6[4], w6[5], rcPackNormal(nMean), rcrMeta(expectedTag, newNEff, 0u));
    gRcCells[cell] = outCell;

    // Re-arm the accumulator for the next frame's scatter: count and all 15 payload ints.
    gRcAccum[cell] = 0;
    [unroll] for (uint k = 0; k < RCR_PAYLOAD; ++k) gRcAccum[payloadBase + k] = 0;
}
