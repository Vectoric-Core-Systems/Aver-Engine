// Radiance cache lookup core, shared by the trace twins (rcLookup in voxi_neurac_io.hlsli) and the
// NeuRaC guide pass. The including shader declares the resources by name BEFORE this include:
//   StructuredBuffer<RcInfo> gRcInfo;  RWStructuredBuffer<RcCell> gRcCells;
// (twin: t22/u21, guide: t0/u0). Keeps this file free of gAmbientParams and every other twin global.

#ifndef AVER_NEURAC_LOOKUP_HLSLI
#define AVER_NEURAC_LOOKUP_HLSLI

#include "voxi_neurac.hlsli"

// One cascade's contribution at q, the sample point in that cascade's window cells ([0, 64) per axis),
// for normal N. Callers build q as (p + N * 0.5 * size) / size - org (twin), or the guide's
// eye-relative equivalent; the half-cell normal offset is part of q because scatter bins the same way.
// value = cosine-convolved irradiance / PI; a = this cascade's blend weight (0: skipped or empty).
void rcLookupCascade(uint c, float3 q, float3 N, out float3 value, out float a)
{
    value = 0.0;
    a     = 0.0;
    const RcInfo info = gRcInfo[0];
    const int3   org  = info.cas[c].xyz;
    const float3 g    = q - 0.5;
    const float3 fl   = floor(g);
    const float3 f    = g - fl;
    const int3   b    = (int3)fl;                                         // window-relative base corner
    // Both corners (b and b + 1) must sit inside margin.
    if (any(b < AVER_RC_MARGIN_CELLS) || any(b > AVER_RC_RES - 2 - AVER_RC_MARGIN_CELLS)) return;
    const float dEdge = min(min(q.x, min(q.y, q.z)), min(AVER_RC_RES - q.x, min(AVER_RC_RES - q.y, AVER_RC_RES - q.z)));
    const float aEdge = saturate((dEdge - (float)AVER_RC_MARGIN_CELLS) / (float)AVER_RC_CROSSFADE_CELLS);
    // Network trust scales each corner; 0 strength (network off) is the stage-1 weight exactly.
    const float netStrength = info.hdr1.w;
    float3 acc[4] = { (float3)0, (float3)0, (float3)0, (float3)0 };
    float  wsum = 0.0;
    float  tsum = 0.0;   // trilinear weight of the corners that contributed
    [unroll] for (int i = 0; i < 8; ++i) {
        const int3   o  = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        const int3   wc = b + o + org;
        const float3 tw = lerp(1.0 - f, f, (float3)o);
        const float  tri = tw.x * tw.y * tw.z;
        const RcCell cell = gRcCells[rcCellIndex(c, wc)];
        const uint   meta = cell.b.w;
        const uint   neff = (meta >> 24) & 15u;
        const uint   age  = meta >> 28;
        if (neff == 0u || (meta & 0xFFFFFFu) != rcTagPack(wc) || age >= (uint)AVER_RC_AGE_MAX) continue;
        float len, trust;
        const float3 cn = rcUnpackNormal(cell.b.z, len, trust);
        const float  w  = tri * saturate(dot(cn, N) * AVER_RC_NORMAL_K) * len
                        * min((float)neff * 0.25, 1.0) * (1.0 - (float)age / (float)AVER_RC_AGE_MAX)
                        * lerp(1.0, trust, netStrength);
        if (w <= 0.0) continue;
        tsum += tri;
        float3 cc[4];
        rcUnpackSh(cell.a, cell.b.xy, cc);
        [unroll] for (int k = 0; k < 4; ++k) acc[k] += w * cc[k];
        wsum += w;
    }
    if (wsum <= 1e-5) return;
    float3 blended[4];
    [unroll] for (int k = 0; k < 4; ++k) blended[k] = acc[k] / wsum;
    value = rcShIrradianceOverPi(blended, N);
    // Confidence is the quality of the corners that hold this surface's light, not their share of all eight: beside a wall
    // or a floor the corners behind it or out in the air never get samples, so a fully trained surface read at most
    // ~0.5 and paths never ended in the cache. Corners holding under a quarter of the trilinear weight still fade it out.
    a     = aEdge * saturate(wsum / tsum) * saturate(tsum * (1.0 / AVER_RC_MIN_COVER));
}

#endif  // AVER_NEURAC_LOOKUP_HLSLI
