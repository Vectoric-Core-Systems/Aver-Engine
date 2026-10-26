// Resolve: SH sums to running mean, zero accumulator for next frame.
// ACCUMULATOR: counts [0, K) = accepted | prevArrivals << 16, arrivals [K, 2K), then 15 ints per cell, then K sums of squares.
#include "voxi_neurac.hlsli"

StructuredBuffer<RcInfo>      gRcInfo  : register(t0);
RWStructuredBuffer<int>       gRcAccum : register(u0);
RWStructuredBuffer<RcCell>    gRcCells : register(u1);

// RCR_ prefix prevents collision with shared header #defines.
#define RCR_RES      64u
#define RCR_PER_CASC 262144u     // 64^3
#define RCR_CELLS    786432u     // 3 * 64^3
#define RCR_PAYLOAD  15u         // ints per cell after the counts and arrivals regions
#define RCR_PAYLOAD_BASE 1572864u // 2 * RCR_CELLS
#define RCR_SQ_BASE  13369344u   // 17 * RCR_CELLS: the sum-of-squares block

#define RCR_FLAG_CLEAR_ALL 1u
// The floating origin moved by whole regions (1,638,400 cm): cascade 2's world cells (400 cm) moved by 4096 per step, so
// its tag (worldCell >> 6) & 255 moved by 64 per step; cascades 0 and 1 move by 1024 / 256 per step, which is 0 mod 256.
// Bits 8..15 / 16..23 are what to SUBTRACT from the stored tag's x / y byte (NeuRaCLayout.hpp shiftTagUnits).
#define RCR_FLAG_RETAG_COARSE 2u

// n_eff and age live in the meta word beside the tag (RcCell.b.w): tag bits 0-23, n_eff 24-27, age 28-31.
uint rcrMeta(uint tag, uint nEff, uint age) { return (tag & 0xFFFFFFu) | ((nEff & 15u) << 24) | ((age & 15u) << 28); }

[numthreads(64, 1, 1)]
void CSRcResolve(uint3 dtid : SV_DispatchThreadID)
{
    const uint cell = dtid.x;
    if (cell >= RCR_CELLS) return;

    const RcInfo info = gRcInfo[0];
    const uint payloadBase = RCR_PAYLOAD_BASE + cell * RCR_PAYLOAD;

    // CLEAR_ALL flag: zero cell and return.
    if ((info.hdr0.x & RCR_FLAG_CLEAR_ALL) != 0u)
    {
        RcCell zero;
        zero.a = uint4(0, 0, 0, 0);
        zero.b = uint4(0, 0, 0, 0);
        gRcCells[cell] = zero;
        gRcAccum[cell] = 0;
        gRcAccum[RCR_CELLS + cell] = 0;
        [unroll] for (uint k = 0; k < RCR_PAYLOAD; ++k) gRcAccum[payloadBase + k] = 0;
        gRcAccum[RCR_SQ_BASE + cell] = 0;
        return;
    }

    // RETAG_COARSE: re-label cascade 2's live cells under the shifted frame so the cache survives the rebase.
    if ((info.hdr0.x & RCR_FLAG_RETAG_COARSE) != 0u && cell >= 2u * RCR_PER_CASC)
    {
        const uint m = gRcCells[cell].b.w;
        if (((m >> 24) & 15u) != 0u)
        {
            const uint dx = (info.hdr0.x >> 8) & 255u;
            const uint dy = (info.hdr0.x >> 16) & 255u;
            const uint tx = ((m & 255u) - dx) & 255u;
            const uint ty = (((m >> 8) & 255u) - dy) & 255u;
            gRcCells[cell].b.w = (m & 0xFF000000u) | (m & 0x00FF0000u) | (ty << 8) | tx;
        }
    }

    // Limit samples by this cascade's cap.
    const uint cascade = cell / RCR_PER_CASC;
    const uint cap = rcCapFor(info, cascade);
    const uint countsWord = (uint)gRcAccum[cell];
    const uint arrivals   = (uint)gRcAccum[RCR_CELLS + cell];
    const uint n          = min(countsWord & 0xFFFFu, cap);

    // Arrivals become next frame's load hint and reset, before any early-out so a stale hint never lingers.
    const uint nextCounts = min(arrivals, 65535u) << 16;
    if (countsWord != nextCounts) gRcAccum[cell] = (int)nextCounts;
    if (arrivals != 0u) gRcAccum[RCR_CELLS + cell] = 0;

    // Aging: fixed cadence per frame.
    const uint ageStep   = info.hdr0.z;
    const bool agingFrame = ageStep != 0u && (info.hdr0.y % ageStep) == 0u;

    if (n == 0u && !agingFrame) return;

    // World cell: window origin + (texel - origin) & 63.
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
        // Aging frame with no samples: occupied cells age and empty at limit.
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

    // Sample mean: fixed point (scale AVER_RC_SH_SCALE) over n samples.
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
    uint trust6  = (uint)AVER_RC_TRUST_MAX;   // a new or replaced cell starts fully trusted
    if (valid)
    {
        // Blend rate: 1/(n_eff+1) with minimum for lighting changes (per frame); relaxAlpha raises the
        // floor for a few frames after an event that makes the old light wrong (sun jump, GI rebuild).
        const float alpha = max(1.0 / ((float)nEff + 1.0), max(info.hdr1.x, info.hdr1.y));
        float3 old[4];
        rcUnpackSh(stored.a, stored.b.xy, old);
        float oldLen;
        const float3 oldDir = rcUnpackNormal(stored.b.z, oldLen);
        trust6 = rcNormalTrust6(stored.b.z);
        [unroll] for (uint j = 0; j < 4; ++j) sh[j] = lerp(old[j], mean[j], alpha);
        nMean = lerp(oldDir * oldLen, nMean, alpha);
        newNEff = min(nEff + 1u, (uint)AVER_RC_NEFF_MAX);
    }
    // Invalid cell (stale tag): replace to avoid light leak.

    uint w6[6];
    rcPackSh(sh, w6);
    RcCell outCell;
    outCell.a = uint4(w6[0], w6[1], w6[2], w6[3]);
    outCell.b = uint4(w6[4], w6[5], rcPackNormal(nMean, trust6), rcrMeta(expectedTag, newNEff, 0u));
    gRcCells[cell] = outCell;

    // Reset the payload for next frame's scatter (counts and arrivals were handled above).
    [unroll] for (uint k = 0; k < RCR_PAYLOAD; ++k) gRcAccum[payloadBase + k] = 0;
    gRcAccum[RCR_SQ_BASE + cell] = 0;
}
