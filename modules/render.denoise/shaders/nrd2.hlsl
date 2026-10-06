// NRD2 phase 1 (docs/rendering/NRD2.md): single-frame denoiser of Stage B's demodulated lighting.
// One file, one pass per compile (AVER_NRD2_PASS):
//   0 CSNrd2Pyramid -- this frame's D and S reduced to 1/2, 1/4, 1/8 (edge-aware 2x2, fixed)
//   1 CSNrd2Params  -- fills the per-8x8-tile parameter buffer with the defaults (phase 4: the network)
//   2 CSNrd2Resolve -- per pixel D' and S' (nrd2_resolve.hlsli), remodulated: D' * Rd + S' * Rs
//                      (temporal stage on: D' and S' go to u1/u2 instead and lit is left to pass 7)
//   3 VSNrd2Compose / PSNrd2Compose -- adds that into the scene colour (additive blend)
//   4 CSNrd2Features -- the network's 12 input channels at half resolution (phase 3 capture, phase 4)
//   5 CSNrd2Reproject, 6 CSNrd2Prefilter, 7 CSNrd2Temporal -- jitter-free frames only: the temporal stage, after
//                      the resolve (docs/rendering/NRD2.md "Temporal stabiliser"): history reprojection with a sample
//                      count, noise estimate and per-tile anchor; a fixed 15-tap prefilter; the min/max-clipped blend
//                      with history, remodulated into lit

// Portable (modules/render.neural/README.md rules): fp32, no wave intrinsics, no atomics, groupshared
// <= 16 KB, every barrier in uniform control flow, constants at b3 (the Vulkan backend folds b1 into push
// constants). Texel coordinates of D/S/guides are render-target pixels; the pyramid is viewport-local.
#include "nrd2_resolve.hlsli"

#ifndef AVER_NRD2_PASS
#define AVER_NRD2_PASS 0
#endif

cbuffer Nrd2CB : register(b3) {
    uint4  gNrd2Rect;     // scene viewport x, y, w, h (render-target pixels)
    uint4  gNrd2Tiles;    // x tiles, y tiles, flags (1 own pixel only, 2 stabilise, 4 history valid, 8 standardise), unused
    float4 gNrd2Def[3];   // default tile parameters, planes 0..11 (D then S)
    float4 gNrd2View[3];  // world -> view rows: right, up, forward (xyz); w of right and up: tan of the half FOV
    float4 gNrd2InScale[3];   // features: the network's input standardisation x * scale + bias (flag 8)
    float4 gNrd2InBias[3];
    float4 gNrd2PrevVP[4];    // stabiliser: previous view-projection about the previous eye (row vectors)
    float4 gNrd2CamDelta;     // eye - previous eye (xyz)
    float4 gNrd2Stab;         // history frames at rest, cap at speed
};

uint2 nrd2LevelSize(uint shift) { return ((gNrd2Rect.zw + 7u) / 8u) * (8u >> shift); }

#if AVER_NRD2_PASS == 0   // ---- pyramid ----

Texture2D<float4> gNrd2D      : register(t0);   // rgb demodulated diffuse, a = albedo usable (0/1)
Texture2D<float4> gNrd2S      : register(t1);   // rgb demodulated specular, a = hit distance (cm)
Texture2D<float>  gNrd2ViewZ  : register(t2);   // G-buffer view Z, cm
Texture2D<float4> gNrd2Normal : register(t3);   // G-buffer averPackNormalRoughness
RWTexture2D<float4> gNrd2G1 : register(u0);     // guides: xyz averaged normal, w view Z (m)
RWTexture2D<float4> gNrd2G2 : register(u1);
RWTexture2D<float4> gNrd2G3 : register(u2);
RWTexture2D<float4> gNrd2D1 : register(u3);     // values: rgb, a validity fraction
RWTexture2D<float4> gNrd2D2 : register(u4);
RWTexture2D<float4> gNrd2D3 : register(u5);
RWTexture2D<float4> gNrd2S1 : register(u6);
RWTexture2D<float4> gNrd2S2 : register(u7);
RWTexture2D<float4> gNrd2S3 : register(u8);

// Each 2x2 keeps the nearest surface in its block: the others weigh by how far behind it they sit,
// so a thin object in front keeps its own texel. Fixed falloff.
static const float kNrd2ReduceDepth = 23.0;   // exp2(-23 * dz/z): 1/2 at 3% behind

groupshared float4 gsG[64];
groupshared float4 gsD[64];
groupshared float4 gsS[64];

void nrd2Reduce(uint a, uint b, uint c, uint d, out float4 g, out float4 dv, out float4 sv) {
    const uint idx[4] = {a, b, c, d};
    float zmin = 1.0e30;
    [unroll] for (uint i = 0u; i < 4u; ++i) if (gsG[idx[i]].w > 0.0) zmin = min(zmin, gsG[idx[i]].w);
    g = 0.0; dv = 0.0; sv = 0.0;
    if (zmin >= 1.0e30) return;
    float3 nsum = 0.0, dsum = 0.0, ssum = 0.0;
    float  zsum = 0.0, wsum = 0.0, dw = 0.0, sw = 0.0;
    [unroll] for (uint j = 0u; j < 4u; ++j) {
        const float4 G = gsG[idx[j]];
        if (G.w <= 0.0) continue;
        const float w = exp2(-min((G.w - zmin) / zmin, 64.0) * kNrd2ReduceDepth);
        const float4 D = gsD[idx[j]];
        const float4 S = gsS[idx[j]];
        nsum += w * G.xyz; zsum += w * G.w; wsum += w;
        dsum += w * D.a * D.rgb; dw += w * D.a;
        ssum += w * S.a * S.rgb; sw += w * S.a;
    }
    g  = float4(nsum / wsum, zsum / wsum);
    dv = float4(dw > 0.0 ? dsum / dw : 0.0, dw / wsum);
    sv = float4(sw > 0.0 ? ssum / sw : 0.0, sw / wsum);
}

bool nrd2Finite(float3 v) { return all(v == v) && all(abs(v) < 6.0e4); }

[numthreads(8, 8, 1)]
void CSNrd2Pyramid(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID) {
    const uint gi = gtid.y * 8u + gtid.x;
    const uint2 q = dtid.xy;                       // viewport-local
    const bool inside = all(q < gNrd2Rect.zw);
    const int2 p = int2(gNrd2Rect.xy + min(q, max(gNrd2Rect.zw, 1u) - 1u));
    const float  z = gNrd2ViewZ.Load(int3(p, 0));
    const float4 d = gNrd2D.Load(int3(p, 0));
    const float4 s = gNrd2S.Load(int3(p, 0));
    const bool surf = inside && z > 0.0 && z < 1.0e6;
    gsG[gi] = surf ? float4(nrd2DecodeNormal(gNrd2Normal.Load(int3(p, 0))), z * 0.01) : 0.0;
    gsD[gi] = (surf && d.a > 0.5 && nrd2Finite(d.rgb)) ? float4(d.rgb, 1.0) : 0.0;
    gsS[gi] = (surf && nrd2Finite(s.rgb)) ? float4(s.rgb, 1.0) : 0.0;
    GroupMemoryBarrierWithGroupSync();

    // In place: each level's results land on even-even slots no other reducer of that level reads.
    float4 g, dv, sv;
    if (((gtid.x | gtid.y) & 1u) == 0u) {
        nrd2Reduce(gi, gi + 1u, gi + 8u, gi + 9u, g, dv, sv);
        const uint2 t = dtid.xy / 2u;
        gNrd2G1[t] = g; gNrd2D1[t] = dv; gNrd2S1[t] = sv;
        gsG[gi] = g; gsD[gi] = dv; gsS[gi] = sv;
    }
    GroupMemoryBarrierWithGroupSync();
    if (((gtid.x | gtid.y) & 3u) == 0u) {
        nrd2Reduce(gi, gi + 2u, gi + 16u, gi + 18u, g, dv, sv);
        const uint2 t = dtid.xy / 4u;
        gNrd2G2[t] = g; gNrd2D2[t] = dv; gNrd2S2[t] = sv;
        gsG[gi] = g; gsD[gi] = dv; gsS[gi] = sv;
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u) {
        nrd2Reduce(0u, 4u, 32u, 36u, g, dv, sv);
        const uint2 t = dtid.xy / 8u;
        gNrd2G3[t] = g; gNrd2D3[t] = dv; gNrd2S3[t] = sv;
    }
}

#elif AVER_NRD2_PASS == 1   // ---- default tile parameters ----

RWStructuredBuffer<float> gNrd2ParamsOut : register(u0);

[numthreads(64, 1, 1)]
void CSNrd2Params(uint3 dtid : SV_DispatchThreadID) {
    const uint tiles = gNrd2Tiles.x * gNrd2Tiles.y;
    if (dtid.x >= tiles) return;
    [unroll] for (uint k = 0u; k < NRD2_PLANES; ++k) gNrd2ParamsOut[k * tiles + dtid.x] = gNrd2Def[k >> 2][k & 3u];
}

#elif AVER_NRD2_PASS == 2   // ---- resolve ----

Texture2D<float4> gNrd2D      : register(t0);
Texture2D<float4> gNrd2S      : register(t1);
Texture2D<float>  gNrd2ViewZ  : register(t2);
Texture2D<float4> gNrd2Normal : register(t3);
Texture2D<float4> gNrd2RemodA : register(t4);   // rgb = Rd, a = Rs.r
Texture2D<float2> gNrd2RemodB : register(t5);   // Rs.gb
Texture2D<float4> gNrd2G1 : register(t6);
Texture2D<float4> gNrd2G2 : register(t7);
Texture2D<float4> gNrd2G3 : register(t8);
Texture2D<float4> gNrd2D1 : register(t9);
Texture2D<float4> gNrd2D2 : register(t10);
Texture2D<float4> gNrd2D3 : register(t11);
Texture2D<float4> gNrd2S1 : register(t12);
Texture2D<float4> gNrd2S2 : register(t13);
Texture2D<float4> gNrd2S3 : register(t14);
StructuredBuffer<float> gNrd2Params : register(t15);
RWTexture2D<float4> gNrd2Lit : register(u0);    // D' * Rd + S' * Rs (left to the stabiliser when it runs)
RWTexture2D<float4> gNrd2DRes : register(u1);   // temporal stage input: D' and S', a = 1 on surfaces
RWTexture2D<float4> gNrd2SRes : register(u2);

Nrd2TileParams nrd2LoadParams(uint tile, uint tiles, uint signal) {
    float v[6], d[6];
    [unroll] for (uint k = 0u; k < 6u; ++k) {
        const uint plane = signal * 6u + k;
        v[k] = gNrd2Params[plane * tiles + tile];
        d[k] = gNrd2Def[plane >> 2][plane & 3u];
    }
    return nrd2SanitiseParams(v, d);
}

[numthreads(8, 8, 1)]
void CSNrd2Resolve(uint3 dtid : SV_DispatchThreadID) {
    const uint2 q = dtid.xy;
    if (any(q >= gNrd2Rect.zw)) return;   // no barriers in this pass
    const int2 p = int2(gNrd2Rect.xy + q);
    const float z = gNrd2ViewZ.Load(int3(p, 0));
    const bool stab = (gNrd2Tiles.z & 2u) != 0u;
    if (!(z > 0.0 && z < 1.0e6)) {
        if (stab) { gNrd2DRes[p] = 0.0; gNrd2SRes[p] = 0.0; }
        else gNrd2Lit[p] = 0.0;
        return;
    }
    const float4 nr = gNrd2Normal.Load(int3(p, 0));
    const float3 n  = nrd2DecodeNormal(nr);
    const float4 d0 = gNrd2D.Load(int3(p, 0));
    const float4 s0 = gNrd2S.Load(int3(p, 0));
    const float4 ra = gNrd2RemodA.Load(int3(p, 0));
    const float2 rb = gNrd2RemodB.Load(int3(p, 0));
    const float3 Rd = ra.rgb;
    const float3 Rs = float3(ra.a, rb);
    const bool dOwn = d0.a > 0.5 && all(d0.rgb == d0.rgb);
    const bool sOwn = all(s0.rgb == s0.rgb);

    float3 dRes = dOwn ? d0.rgb : 0.0;
    float3 sRes = sOwn ? s0.rgb : 0.0;
    if ((gNrd2Tiles.z & 1u) == 0u) {
        const uint tiles = gNrd2Tiles.x * gNrd2Tiles.y;
        const uint tile  = (q.y / 8u) * gNrd2Tiles.x + q.x / 8u;
        const float zm = z * 0.01;
        const uint2 s1 = nrd2LevelSize(1u), s2 = nrd2LevelSize(2u), s3 = nrd2LevelSize(3u);
        float zn[4];
        [unroll] for (uint k = 0u; k < 4u; ++k) {
            const int2 qn = int2(q) + (k == 0u ? int2(-1, 0) : k == 1u ? int2(1, 0) : k == 2u ? int2(0, -1) : int2(0, 1));
            const bool inb = all(qn >= 0) && all(qn < int2(gNrd2Rect.zw));
            const float v = inb ? gNrd2ViewZ.Load(int3(int2(gNrd2Rect.xy) + qn, 0)) : 0.0;
            zn[k] = (v > 0.0 && v < 1.0e6) ? v * 0.01 : 0.0;
        }
        const float2 zg = nrd2DepthSlope(zm, zn[0], zn[1], zn[2], zn[3]);

        const Nrd2TileParams pd = nrd2LoadParams(tile, tiles, 0u);
        float cf1, cf2, cf3;
        const float dS = exp2(pd.log2Depth), nP = exp2(pd.log2Normal);
        const float3 d1 = nrd2Upsample(gNrd2G1, gNrd2D1, 1u, q, s1, zg, zm, n, dS, nP, cf1);
        const float3 d2 = nrd2Upsample(gNrd2G2, gNrd2D2, 2u, q, s2, zg, zm, n, dS, nP, cf2);
        const float3 d3 = nrd2Upsample(gNrd2G3, gNrd2D3, 3u, q, s3, zg, zm, n, dS, nP, cf3);
        dRes = nrd2Combine(d0.rgb, dOwn, d1, d2, d3, cf1, cf2, cf3, pd, 0.0);

        const Nrd2TileParams ps = nrd2LoadParams(tile, tiles, 1u);
        const float sS = exp2(ps.log2Depth), sP = exp2(ps.log2Normal);
        const float3 e1 = nrd2Upsample(gNrd2G1, gNrd2S1, 1u, q, s1, zg, zm, n, sS, sP, cf1);
        const float3 e2 = nrd2Upsample(gNrd2G2, gNrd2S2, 2u, q, s2, zg, zm, n, sS, sP, cf2);
        const float3 e3 = nrd2Upsample(gNrd2G3, gNrd2S3, 3u, q, s3, zg, zm, n, sS, sP, cf3);
        sRes = nrd2Combine(s0.rgb, sOwn, e1, e2, e3, cf1, cf2, cf3, ps,
                           nrd2SpecularExtraLogit(nr.z, s0.a, z));
    }
    if (stab) {
        gNrd2DRes[p] = float4(nrd2StabSane(dRes), 1.0);
        gNrd2SRes[p] = float4(nrd2StabSane(sRes), 1.0);
        return;
    }
    float3 lit = dRes * Rd + sRes * Rs;
    if (!all(lit == lit)) lit = 0.0;
    gNrd2Lit[p] = float4(clamp(lit, 0.0, 6.0e4), 0.0);
}

#elif AVER_NRD2_PASS == 3   // ---- compose into the scene colour ----

Texture2D<float4> gNrd2Lit : register(t0);

struct Nrd2VsOut { float4 pos : SV_Position; };

Nrd2VsOut VSNrd2Compose(uint id : SV_VertexID) {
    Nrd2VsOut o;
    const float2 uv = float2((id << 1) & 2u, id & 2u);
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Additive: target 0 gets the denoised lighting; the G-buffer targets bound beside it are masked.
float4 PSNrd2Compose(Nrd2VsOut i) : SV_Target0 {
    return float4(gNrd2Lit.Load(int3(int2(i.pos.xy), 0)).rgb, 0.0);
}

#elif AVER_NRD2_PASS == 4   // ---- network input features ----

// 12 channels per half-resolution texel (a 2x2 block of the viewport), NCHW fp32, plane = 4 tilesX x
// 4 tilesY (whole tiles; texels past the viewport are 0). From this frame only: no history, no frame
// time (NEURAA_NRD.md section 7, rule 10). Luminance only, relative to the tile's 1/8 pyramid texel.
//   0, 1   log2 luminance of D, S (2x2 mean) minus log2 luminance of the 1/8 texel
//   2, 3   2x2 contrast log2(max / mean) of D, S
//   4      log2(view Z / the 1/8 texel's view Z), clamped +-4
//   5..7   view-space normal (2x2 mean, normalised)
//   8      roughness (2x2 mean)
//   9      diffuse albedo luminance (Rd, 2x2 mean)
//   10     log2(1 + specular hit distance / view Z), clamped [0, 8] (hits only)
//   11     validity: share of the 2x2 with a surface and a usable albedo
Texture2D<float4> gNrd2D      : register(t0);
Texture2D<float4> gNrd2S      : register(t1);
Texture2D<float>  gNrd2ViewZ  : register(t2);
Texture2D<float4> gNrd2Normal : register(t3);
Texture2D<float4> gNrd2RemodA : register(t4);
Texture2D<float4> gNrd2G3     : register(t5);
Texture2D<float4> gNrd2D3     : register(t6);
Texture2D<float4> gNrd2S3     : register(t7);
RWStructuredBuffer<float> gNrd2Feat : register(u0);

float nrd2RelLog(float l, float ref) {
    const float e = max(ref, 0.0) * 1.0e-3 + 1.0e-7;
    return clamp(log2(max(l, 0.0) + e) - log2(max(ref, 0.0) + e), -16.0, 16.0);
}

[numthreads(8, 8, 1)]
void CSNrd2Features(uint3 dtid : SV_DispatchThreadID) {
    const uint hw = gNrd2Tiles.x * 4u, hh = gNrd2Tiles.y * 4u;
    const uint2 h = dtid.xy;
    if (h.x >= hw || h.y >= hh) return;   // no barriers in this pass
    float f[NRD2_FEATURES];
    [unroll] for (uint c = 0u; c < NRD2_FEATURES; ++c) f[c] = 0.0;

    float dSum = 0.0, dMax = 0.0, sSum = 0.0, sMax = 0.0, zSum = 0.0, rSum = 0.0, aSum = 0.0, hSum = 0.0;
    float nD = 0.0, nS = 0.0, nZ = 0.0, nH = 0.0;
    float3 nSum = 0.0;
    [unroll] for (uint i = 0u; i < 4u; ++i) {
        const uint2 q = h * 2u + uint2(i & 1u, i >> 1);
        if (any(q >= gNrd2Rect.zw)) continue;
        const int3 p = int3(int2(gNrd2Rect.xy + q), 0);
        const float z = gNrd2ViewZ.Load(p);
        if (!(z > 0.0 && z < 1.0e6)) continue;
        const float4 nr = gNrd2Normal.Load(p);
        const float4 d = gNrd2D.Load(p);
        const float4 s = gNrd2S.Load(p);
        zSum += z; nZ += 1.0;
        nSum += nrd2DecodeNormal(nr);
        rSum += nr.z;
        const float ra = nrd2Lum(gNrd2RemodA.Load(p).rgb);
        aSum += (ra == ra) ? clamp(ra, 0.0, 4.0) : 0.0;
        if (d.a > 0.5 && all(d.rgb == d.rgb) && all(abs(d.rgb) < 6.0e4)) {
            const float l = max(nrd2Lum(d.rgb), 0.0);
            dSum += l; dMax = max(dMax, l); nD += 1.0;
        }
        if (all(s.rgb == s.rgb) && all(abs(s.rgb) < 6.0e4)) {
            const float l = max(nrd2Lum(s.rgb), 0.0);
            sSum += l; sMax = max(sMax, l); nS += 1.0;
            if (s.a > 0.0 && s.a == s.a) { hSum += clamp(log2(1.0 + s.a / z), 0.0, 8.0); nH += 1.0; }
        }
    }
    if (nZ > 0.0) {
        const int3 t = int3(int2(h / 4u), 0);   // this block's tile = the 1/8 texel
        const float4 g3 = gNrd2G3.Load(t);
        const float4 d3 = gNrd2D3.Load(t);
        const float4 s3 = gNrd2S3.Load(t);
        if (nD > 0.0) {
            const float m = dSum / nD;
            f[0] = d3.a > 0.0 ? nrd2RelLog(m, nrd2Lum(d3.rgb)) : 0.0;
            f[2] = nrd2RelLog(dMax, m);
        }
        if (nS > 0.0) {
            const float m = sSum / nS;
            f[1] = s3.a > 0.0 ? nrd2RelLog(m, nrd2Lum(s3.rgb)) : 0.0;
            f[3] = nrd2RelLog(sMax, m);
        }
        const float zm = zSum / nZ * 0.01;
        f[4] = g3.w > 0.0 ? clamp(log2(zm / g3.w), -4.0, 4.0) : 0.0;
        const float nl = length(nSum);
        const float3 n = nl > 1.0e-4 ? nSum / nl : 0.0;
        f[5] = dot(n, gNrd2View[0].xyz);
        f[6] = dot(n, gNrd2View[1].xyz);
        f[7] = dot(n, gNrd2View[2].xyz);
        f[8] = rSum / nZ;
        f[9] = aSum / nZ;
        f[10] = nH > 0.0 ? hSum / nH : 0.0;
        f[11] = nD * 0.25;
    }
    // Flag 8 (the live network path): the network's input standardisation folded into the store. The
    // capture leaves it off and gets the raw features.
    const bool standardise = (gNrd2Tiles.z & 8u) != 0u;
    const uint plane = hw * hh, at = h.y * hw + h.x;
    [unroll] for (uint c2 = 0u; c2 < NRD2_FEATURES; ++c2) {
        float v = (f[c2] == f[c2]) ? f[c2] : 0.0;
        if (standardise)
            v = abs(v) < 3.0e38 ? v * gNrd2InScale[c2 >> 2][c2 & 3u] + gNrd2InBias[c2 >> 2][c2 & 3u] : 0.0;
        gNrd2Feat[c2 * plane + at] = v;
    }
}

#elif AVER_NRD2_PASS == 5   // ---- temporal 1/3: reproject ----

// FidelityFX's reflections-denoiser temporal design (MIT), rewritten (docs/rendering/NRD2.md "Temporal
// stabiliser"). Per pixel: D' and S' against last frame's stabilised ones at the velocity-reprojected position,
// by reprojected view depth and normal; a failed bilinear set falls back to the best-matching 3x3 neighbour.
// Out: the reprojected history with its sample count n, the noise estimate, and per 8x8 tile a
// brightness-weighted anchor (bright outliers down-weighted relative to the tile).
Texture2D<float4> gNrd2DRes   : register(t0);   // D', a = 1 on surfaces
Texture2D<float4> gNrd2SRes   : register(t1);
Texture2D<float>  gNrd2ViewZ  : register(t2);
Texture2D<float4> gNrd2Normal : register(t3);
Texture2D<float2> gNrd2Vel    : register(t4);   // G-buffer velocity: texels per frame, destination minus source
Texture2D<float4> gNrd2HistD  : register(t5);   // last frame's D'' rgb, a = sample count (0 = none)
Texture2D<float4> gNrd2HistS  : register(t6);
Texture2D<float4> gNrd2HistG  : register(t7);   // x view Z (m), yz the G-buffer's packed normal xy
Texture2D<float2> gNrd2HistV  : register(t8);   // noise estimate of D'', S''
RWTexture2D<float4> gNrd2RpD   : register(u0);  // reprojected D history rgb, a = sample count n (1 = none)
RWTexture2D<float4> gNrd2RpS   : register(u1);
RWTexture2D<float2> gNrd2RpV   : register(u2);  // noise estimate after this frame's reprojection
RWTexture2D<float4> gNrd2AnchD : register(u3);  // per-tile anchor rgb, a = 1 when valid
RWTexture2D<float4> gNrd2AnchS : register(u4);

groupshared float4 gsA[64];   // tile sums: luminance and count of D and S
groupshared float4 gsB[64];   // anchor sums: w * D, w
groupshared float4 gsC[64];   // w * S, w

struct Nrd2HistSet {
    float  wsum, score;   // bilinear weight kept, its sum of search scores
    float3 d, s;          // weighted sums
    float2 v;
    float2 n;             // youngest count among the taps that carry weight: D, S
};

// The four history texels from `base`, bilinear weights f, each kept by normal and reprojected-depth agreement.
Nrd2HistSet nrd2GatherHistory(int2 base, float2 f, float3 n, float zExp, float zTol) {
    Nrd2HistSet h;
    h.wsum = 0.0; h.score = 0.0; h.d = 0.0; h.s = 0.0; h.v = 0.0; h.n = 1.0e9;
    const int2 lo = int2(gNrd2Rect.xy), hi = int2(gNrd2Rect.xy + gNrd2Rect.zw);
    [unroll] for (uint t = 0u; t < 4u; ++t) {
        const int2 o = int2(t & 1u, t >> 1);
        const int3 tc = int3(base + o, 0);
        if (any(tc.xy < lo) || any(tc.xy >= hi)) continue;
        const float4 hd = gNrd2HistD.Load(tc);
        if (!(hd.a > 0.0)) continue;
        const float4 hg = gNrd2HistG.Load(tc);
        const float cosN = dot(n, nrd2DecodeNormal(float4(hg.yz, 0.0, 0.0)));
        const float dz = abs(hg.x - zExp);
        if (cosN < kNrd2StabNormalCos || dz > zTol) continue;
        const float4 hs = gNrd2HistS.Load(tc);
        const float b = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
        h.wsum += b;
        h.score += b * nrd2StabScore(cosN, dz, zExp);
        h.d += b * hd.rgb; h.s += b * hs.rgb;
        h.v += b * gNrd2HistV.Load(tc);
        if (b > kNrd2StabAgeWeight) h.n = min(h.n, float2(hd.a, hs.a));
    }
    return h;
}

// View Z (cm) at a viewport-local texel, or `fallback` outside the viewport or off a surface.
float nrd2StabZ(int2 q, float fallback) {
    if (any(q < 0) || any(q >= int2(gNrd2Rect.zw))) return fallback;
    const float z = gNrd2ViewZ.Load(int3(int2(gNrd2Rect.xy) + q, 0));
    return (z > 0.0 && z < 1.0e6) ? z : fallback;
}

[numthreads(8, 8, 1)]
void CSNrd2Reproject(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    const uint gi = gtid.y * 8u + gtid.x;
    const uint2 q = dtid.xy;
    const bool inside = all(q < gNrd2Rect.zw);
    const int2 p = int2(gNrd2Rect.xy + min(q, max(gNrd2Rect.zw, 1u) - 1u));
    const float z = gNrd2ViewZ.Load(int3(p, 0));
    const bool surf = inside && z > 0.0 && z < 1.0e6;

    float3 curD = 0.0, curS = 0.0, hD = 0.0, hS = 0.0;
    float2 vPrev = 1.0;
    float  nD = 1.0, nS = 1.0;
    bool   ok = false;
    if (surf) {
        curD = nrd2StabSane(gNrd2DRes.Load(int3(p, 0)).rgb);
        curS = nrd2StabSane(gNrd2SRes.Load(int3(p, 0)).rgb);
        if ((gNrd2Tiles.z & 4u) != 0u) {
            const float4 nr = gNrd2Normal.Load(int3(p, 0));
            const float3 n  = nrd2DecodeNormal(nr);
            const float2 mv = gNrd2Vel.Load(int3(p, 0));
            const float nMaxD = nrd2StabMaxFrames(length(mv), gNrd2Stab.x, gNrd2Stab.y);
            const float nMaxS = nrd2StabSpecMaxFrames(nMaxD, nr.z);
            // Where this surface point was last frame: the velocity (exact for rigid motion; no jitter on these
            // frames), and the view depth it should have there (camera motion only, so a mover fails the depth test).
            const float zm = z * 0.01;
            const float2 ndc = float2((float(q.x) + 0.5) / float(gNrd2Rect.z) * 2.0 - 1.0,
                                      1.0 - (float(q.y) + 0.5) / float(gNrd2Rect.w) * 2.0);
            const float3 off = z * (gNrd2View[2].xyz + ndc.x * gNrd2View[0].w * gNrd2View[0].xyz +
                                    ndc.y * gNrd2View[1].w * gNrd2View[1].xyz) + gNrd2CamDelta.xyz;
            const float zExp = (off.x * gNrd2PrevVP[0] + off.y * gNrd2PrevVP[1] + off.z * gNrd2PrevVP[2] + gNrd2PrevVP[3]).w * 0.01;
            // Depth slope, per axis the smaller one-sided difference so a silhouette does not widen the test.
            const int2 qi = int2(q);
            float slope = 0.0;
            [unroll] for (uint a = 0u; a < 2u; ++a) {
                const int2 st = a == 0u ? int2(1, 0) : int2(0, 1);
                const float zl = nrd2StabZ(qi - st, -1.0), zr = nrd2StabZ(qi + st, -1.0);
                const float dm = min(zl > 0.0 ? abs(zl - z) : 1.0e30, zr > 0.0 ? abs(zr - z) : 1.0e30);
                slope = max(slope, dm < 1.0e30 ? dm * 0.01 : 0.0);
            }
            const float zTol = zExp > 0.0 ? kNrd2StabDepthRel * zExp + kNrd2StabDepthAbs + slope : -1.0;

            if (nMaxD > 0.0) {
                const float2 hp = float2(p) - mv;   // p + 0.5 - mv, as a texel corner
                const int2 hb = int2(floor(hp));
                const float2 hf = hp - float2(hb);
                Nrd2HistSet h = nrd2GatherHistory(hb, hf, n, zExp, zTol);
                if (h.wsum < kNrd2StabMinWeight) {
                    // Disocclusion fallback: the best-matching of the 3x3 neighbouring sets, each already
                    // dropping the taps that do not match.
                    float best = 0.0;
                    [loop] for (uint k = 0u; k < 9u; ++k) {
                        if (k == 4u) continue;
                        const Nrd2HistSet c = nrd2GatherHistory(hb + int2(int(k % 3u) - 1, int(k / 3u) - 1), hf, n, zExp, zTol);
                        if (c.wsum >= kNrd2StabMinWeight && c.score > best) { best = c.score; h = c; }
                    }
                }
                ok = h.wsum >= kNrd2StabMinWeight;
                if (ok) {
                    hD = h.d / h.wsum; hS = h.s / h.wsum; vPrev = h.v / h.wsum;
                    nD = nrd2StabSamples(h.n.x, nMaxD);
                    nS = nrd2StabSamples(h.n.y, nMaxS);
                }
            }
        }
    }

    // Tile scale: the plain mean luminance of the tile's surface pixels (fixed-order tree sum).
    const float3 rD = ok ? lerp(curD, hD, kNrd2AnchorMix) : curD;
    const float3 rS = ok ? lerp(curS, hS, kNrd2AnchorMix) : curS;
    const float lD = surf ? max(nrd2Lum(rD), 0.0) : 0.0, lS = surf ? max(nrd2Lum(rS), 0.0) : 0.0;
    gsA[gi] = surf ? float4(lD, 1.0, lS, 1.0) : 0.0;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s1 = 32u; s1 > 0u; s1 >>= 1u) {
        if (gi < s1) gsA[gi] += gsA[gi + s1];
        GroupMemoryBarrierWithGroupSync();
    }
    const float scaleD = gsA[0].x / max(gsA[0].y, 1.0) + 1.0e-6;
    const float scaleS = gsA[0].z / max(gsA[0].w, 1.0) + 1.0e-6;
    const float wD = surf ? nrd2AnchorWeight(lD, scaleD) : 0.0, wS = surf ? nrd2AnchorWeight(lS, scaleS) : 0.0;
    gsB[gi] = float4(wD * rD, wD);
    gsC[gi] = float4(wS * rS, wS);
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s2 = 32u; s2 > 0u; s2 >>= 1u) {
        if (gi < s2) { gsB[gi] += gsB[gi + s2]; gsC[gi] += gsC[gi + s2]; }
        GroupMemoryBarrierWithGroupSync();
    }
    const float4 aD = gsB[0], aS = gsC[0];
    if (gi == 0u) {
        gNrd2AnchD[gid.xy] = aD.w > 0.0 ? float4(aD.rgb / aD.w, 1.0) : 0.0;
        gNrd2AnchS[gid.xy] = aS.w > 0.0 ? float4(aS.rgb / aS.w, 1.0) : 0.0;
    }
    if (!inside) return;   // after the last barrier
    if (!surf) { gNrd2RpD[p] = 0.0; gNrd2RpS[p] = 0.0; gNrd2RpV[p] = 0.0; return; }

    // Noise estimate: this frame's luminance against the reprojected history's, 1/n of the old estimate mixed in.
    float2 var = 1.0;
    if (ok) {
        const float sD = max(aD.w > 0.0 ? nrd2Lum(aD.rgb / aD.w) : 0.0, 0.0) + 1.0e-6;
        const float sS = max(aS.w > 0.0 ? nrd2Lum(aS.rgb / aS.w) : 0.0, 0.0) + 1.0e-6;
        var = float2(lerp(nrd2StabRelVar(nrd2Lum(curD), nrd2Lum(hD), sD), vPrev.x, 1.0 / nD),
                     lerp(nrd2StabRelVar(nrd2Lum(curS), nrd2Lum(hS), sS), vPrev.y, 1.0 / nS));
    }
    gNrd2RpD[p] = float4(ok ? hD : 0.0, nD);
    gNrd2RpS[p] = float4(ok ? hS : 0.0, nS);
    gNrd2RpV[p] = var;
}

#elif AVER_NRD2_PASS == 6   // ---- temporal 2/3: prefilter ----

// FidelityFX's prefilter in NRD2's terms: a fixed 15-tap pattern (radius 3, never rotated) over the resolved D'
// and S'. Weights are normal, relative-depth and radiance stops (the radiance one measured against the tile
// anchor, in units of the pixel's luminance) times a gain that grows with the noise estimate. The estimate
// only weights; it never sets a radius or a level. a of the outputs = the filtered noise estimate, -1 = no surface.
Texture2D<float4> gNrd2DRes   : register(t0);
Texture2D<float4> gNrd2SRes   : register(t1);
Texture2D<float>  gNrd2ViewZ  : register(t2);
Texture2D<float4> gNrd2Normal : register(t3);
Texture2D<float2> gNrd2RpV    : register(t4);
Texture2D<float4> gNrd2AnchD  : register(t5);
Texture2D<float4> gNrd2AnchS  : register(t6);
RWTexture2D<float4> gNrd2PrefD : register(u0);
RWTexture2D<float4> gNrd2PrefS : register(u1);

#define NRD2_PF_R 3
#define NRD2_PF_W (8 + 2 * NRD2_PF_R)
#if NRD2_PF_W * NRD2_PF_W * 56 > 16384
#error nrd2 prefilter groupshared exceeds 16 KB
#endif
groupshared float4 gsPfD[NRD2_PF_W * NRD2_PF_W];   // D' rgb, view Z (m); z <= 0 = no surface (11 KB in all)
groupshared float4 gsPfS[NRD2_PF_W * NRD2_PF_W];   // S' rgb, roughness
groupshared float4 gsPfN[NRD2_PF_W * NRD2_PF_W];   // normal xyz
groupshared float2 gsPfV[NRD2_PF_W * NRD2_PF_W];   // noise estimate D, S

// FidelityFX's 15 Halton(2,3) offsets stretched to [-3, 3], the centre skipped.
static const int2 kNrd2PfTaps[15] = {int2(0, 1),  int2(-2, 1),  int2(2, -3), int2(-3, 0),  int2(1, 2), int2(-1, -2), int2(3, 0), int2(-3, 3),
                                     int2(0, -3), int2(-1, -1), int2(2, 1),  int2(-2, -2), int2(1, 0), int2(0, 2),   int2(3, -1)};

void nrd2PrefilterSignal(uint signal, int li, float4 anchor, float3 n, float zm, float2 grad, out float3 outV,
                         out float outVar) {
    const float3 c0 = signal == 0u ? gsPfD[li].rgb : gsPfS[li].rgb;
    const float var = signal == 0u ? gsPfV[li].x : gsPfV[li].y;
    outV = c0; outVar = var;
    if (!(var > 0.0) || (signal == 1u && gsPfS[li].w < kNrd2PfMirror)) return;
    const float3 a = anchor.a > 0.0 ? anchor.rgb : c0;
    const float scale = max(nrd2Lum(a), 0.0) + 1.0e-6;
    const float wc = nrd2PfCentreWeight(length(a - c0) / scale, var);
    float3 acc = c0 * wc;
    float  accW = wc, accVar = var * wc * wc;
    [unroll] for (uint i = 0u; i < 15u; ++i) {
        const int2 o = kNrd2PfTaps[i];
        const int idx = li + o.y * NRD2_PF_W + o.x;
        const float zn = gsPfD[idx].w;
        if (!(zn > 0.0)) continue;
        const float dzRel = abs(zn - (zm + clamp(dot(grad, float2(o)), -0.5 * zm, 0.5 * zm))) / max(zm, 1.0e-4);
        const float3 v = signal == 0u ? gsPfD[idx].rgb : gsPfS[idx].rgb;
        const float w = nrd2PfWeight(dot(n, gsPfN[idx].xyz), dzRel, length(a - v) / scale, var);
        acc += w * v;
        accW += w;
        accVar += w * w * (signal == 0u ? gsPfV[idx].x : gsPfV[idx].y);
    }
    outV = acc / accW;
    outVar = accVar / (accW * accW);
}

[numthreads(8, 8, 1)]
void CSNrd2Prefilter(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    const int2 org = int2(gid.xy) * 8 - NRD2_PF_R;   // halo origin, viewport-local
    const uint gi = gtid.y * 8u + gtid.x;
    [unroll] for (uint k = 0u; k < 4u; ++k) {
        const uint i = gi + k * 64u;
        if (i < uint(NRD2_PF_W * NRD2_PF_W)) {
            const int2 qq = org + int2(i % uint(NRD2_PF_W), i / uint(NRD2_PF_W));
            float4 d = 0.0, s = 0.0, nn = 0.0;
            float2 v = 0.0;
            if (all(qq >= 0) && all(qq < int2(gNrd2Rect.zw))) {
                const int3 pp = int3(int2(gNrd2Rect.xy) + qq, 0);
                const float z = gNrd2ViewZ.Load(pp);
                if (z > 0.0 && z < 1.0e6) {
                    const float4 nr = gNrd2Normal.Load(pp);
                    d  = float4(nrd2StabSane(gNrd2DRes.Load(pp).rgb), z * 0.01);
                    s  = float4(nrd2StabSane(gNrd2SRes.Load(pp).rgb), nr.z);
                    nn = float4(nrd2DecodeNormal(nr), 0.0);
                    v  = gNrd2RpV.Load(pp);
                }
            }
            gsPfD[i] = d; gsPfS[i] = s; gsPfN[i] = nn; gsPfV[i] = v;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const uint2 q = dtid.xy;
    if (any(q >= gNrd2Rect.zw)) return;   // after the barrier
    const int2 p = int2(gNrd2Rect.xy + q);
    const int li = int((gtid.y + uint(NRD2_PF_R)) * uint(NRD2_PF_W) + gtid.x + uint(NRD2_PF_R));
    const float zm = gsPfD[li].w;
    if (!(zm > 0.0)) { gNrd2PrefD[p] = float4(0.0, 0.0, 0.0, -1.0); gNrd2PrefS[p] = float4(0.0, 0.0, 0.0, -1.0); return; }

    const float2 grad = nrd2DepthSlope(zm, gsPfD[li - 1].w, gsPfD[li + 1].w, gsPfD[li - NRD2_PF_W].w, gsPfD[li + NRD2_PF_W].w);
    const float3 n = gsPfN[li].xyz;
    const uint2 ls = nrd2LevelSize(3u);
    float3 outD, outS;
    float  varD, varS;
    nrd2PrefilterSignal(0u, li, nrd2SampleAnchor(gNrd2AnchD, q, ls), n, zm, grad, outD, varD);
    nrd2PrefilterSignal(1u, li, nrd2SampleAnchor(gNrd2AnchS, q, ls), n, zm, grad, outS, varS);
    gNrd2PrefD[p] = float4(nrd2StabSane(outD), varD);
    gNrd2PrefS[p] = float4(nrd2StabSane(outS), varS);
}

#elif AVER_NRD2_PASS == 7   // ---- temporal 3/3: resolve ----

// Per pixel: the prefiltered value pulled toward the tile anchor by 1/(n + 1), clipped to this frame's 5x5
// min/max united with the anchor; the reprojected history clipped to the same box and blended in with weight
// 1 - 1/n (nrd2TemporalBlend). Never a mean or variance in a clip (NEURAA_NRD.md section 7, rules 1 and 7);
// the firefly clamp runs on the current value first. Writes the new history and the remodulated lit target.
Texture2D<float4> gNrd2PrefD  : register(t0);   // a = noise estimate, -1 = no surface
Texture2D<float4> gNrd2PrefS  : register(t1);
Texture2D<float>  gNrd2ViewZ  : register(t2);
Texture2D<float4> gNrd2Normal : register(t3);
Texture2D<float4> gNrd2RemodA : register(t4);
Texture2D<float2> gNrd2RemodB : register(t5);
Texture2D<float4> gNrd2RpD    : register(t6);   // reprojected history rgb, a = n
Texture2D<float4> gNrd2RpS    : register(t7);
Texture2D<float4> gNrd2AnchD  : register(t8);
Texture2D<float4> gNrd2AnchS  : register(t9);
RWTexture2D<float4> gNrd2Lit   : register(u0);
RWTexture2D<float4> gNrd2OutD  : register(u1);  // new history: D'' rgb, a = n
RWTexture2D<float4> gNrd2OutS  : register(u2);
RWTexture2D<float4> gNrd2OutG  : register(u3);  // x view Z (m), yz the G-buffer's packed normal xy
RWTexture2D<float2> gNrd2OutV  : register(u4);  // noise estimate

#define NRD2_TP_R 2   // box radius: 5x5, inside the prefilter's footprint
#define NRD2_TP_W (8 + 2 * NRD2_TP_R)
#if NRD2_TP_W * NRD2_TP_W * 32 > 16384
#error nrd2 temporal groupshared exceeds 16 KB
#endif
groupshared float4 gsTpD[NRD2_TP_W * NRD2_TP_W];   // prefiltered D' rgb, a = noise estimate (< 0 = no surface)
groupshared float4 gsTpS[NRD2_TP_W * NRD2_TP_W];

[numthreads(8, 8, 1)]
void CSNrd2Temporal(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    const int2 org = int2(gid.xy) * 8 - NRD2_TP_R;   // halo origin, viewport-local
    const uint gi = gtid.y * 8u + gtid.x;
    [unroll] for (uint k = 0u; k < 3u; ++k) {
        const uint i = gi + k * 64u;
        if (i < uint(NRD2_TP_W * NRD2_TP_W)) {
            const int2 qq = org + int2(i % uint(NRD2_TP_W), i / uint(NRD2_TP_W));
            const bool ok = all(qq >= 0) && all(qq < int2(gNrd2Rect.zw));
            const int3 pp = int3(int2(gNrd2Rect.xy) + qq, 0);
            gsTpD[i] = ok ? gNrd2PrefD.Load(pp) : float4(0.0, 0.0, 0.0, -1.0);
            gsTpS[i] = ok ? gNrd2PrefS.Load(pp) : float4(0.0, 0.0, 0.0, -1.0);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const uint2 q = dtid.xy;
    if (any(q >= gNrd2Rect.zw)) return;   // after the barrier
    const int2 p = int2(gNrd2Rect.xy + q);
    const float z = gNrd2ViewZ.Load(int3(p, 0));
    if (!(z > 0.0 && z < 1.0e6)) {
        gNrd2Lit[p] = 0.0; gNrd2OutD[p] = 0.0; gNrd2OutS[p] = 0.0; gNrd2OutG[p] = 0.0; gNrd2OutV[p] = 0.0;
        return;
    }

    // This frame's 5x5 (a sliding window, so it never steps at tile borders): the firefly clamp's neighbour
    // maximum, then the per-channel min/max with the clamped centre.
    const int li = int((gtid.y + uint(NRD2_TP_R)) * uint(NRD2_TP_W) + gtid.x + uint(NRD2_TP_R));
    float nmD = 0.0, nmS = 0.0, nvD = 0.0, nvS = 0.0;
    [unroll] for (uint f = 0u; f < 9u; ++f) {
        if (f == 4u) continue;
        const int fi = li + (int(f / 3u) - 1) * NRD2_TP_W + int(f % 3u) - 1;
        if (gsTpD[fi].a >= 0.0) { nmD = max(nmD, nrd2Lum(gsTpD[fi].rgb)); nvD += 1.0; }
        if (gsTpS[fi].a >= 0.0) { nmS = max(nmS, nrd2Lum(gsTpS[fi].rgb)); nvS += 1.0; }
    }
    const float3 curD = nvD > 0.0 ? nrd2StabFirefly(gsTpD[li].rgb, nmD) : gsTpD[li].rgb;
    const float3 curS = nvS > 0.0 ? nrd2StabFirefly(gsTpS[li].rgb, nmS) : gsTpS[li].rgb;
    float3 loD = curD, hiD = curD, loS = curS, hiS = curS;
    const uint nb = 2u * NRD2_TP_R + 1u;
    [unroll] for (uint n = 0u; n < nb * nb; ++n) {
        if (n == (nb * nb) / 2u) continue;   // the centre is curD/curS, already clamped
        const int ni = li + (int(n / nb) - NRD2_TP_R) * NRD2_TP_W + int(n % nb) - NRD2_TP_R;
        if (gsTpD[ni].a >= 0.0) { loD = min(loD, gsTpD[ni].rgb); hiD = max(hiD, gsTpD[ni].rgb); }
        if (gsTpS[ni].a >= 0.0) { loS = min(loS, gsTpS[ni].rgb); hiS = max(hiS, gsTpS[ni].rgb); }
    }

    const uint2 ls = nrd2LevelSize(3u);
    const float4 aD4 = nrd2SampleAnchor(gNrd2AnchD, q, ls), aS4 = nrd2SampleAnchor(gNrd2AnchS, q, ls);
    const float3 aD = aD4.a > 0.0 ? aD4.rgb : curD, aS = aS4.a > 0.0 ? aS4.rgb : curS;
    const float4 rpD = gNrd2RpD.Load(int3(p, 0)), rpS = gNrd2RpS.Load(int3(p, 0));
    const float nD = max(rpD.a, 1.0), nS = max(rpS.a, 1.0);

    float3 curD2, histD2, curS2, histS2;
    const float3 outD = nrd2StabSane(nrd2TemporalBlend(curD, aD, loD, hiD, rpD.rgb, nD, curD2, histD2));
    const float3 outS = nrd2StabSane(nrd2TemporalBlend(curS, aS, loS, hiS, rpS.rgb, nS, curS2, histS2));

    // The noise estimate to carry: the prefiltered one, mixed with this frame's change against the history.
    float2 var = 1.0;
    if (nD > 1.0) var.x = lerp(nrd2StabRelVar(nrd2Lum(curD2), nrd2Lum(histD2), max(nrd2Lum(aD), 0.0) + 1.0e-6),
                               max(gsTpD[li].a, 0.0), nrd2StabBlend(nD));
    if (nS > 1.0) var.y = lerp(nrd2StabRelVar(nrd2Lum(curS2), nrd2Lum(histS2), max(nrd2Lum(aS), 0.0) + 1.0e-6),
                               max(gsTpS[li].a, 0.0), nrd2StabBlend(nS));
    if (!(var.x == var.x)) var.x = 1.0;
    if (!(var.y == var.y)) var.y = 1.0;

    const float4 nr = gNrd2Normal.Load(int3(p, 0));
    gNrd2OutD[p] = float4(outD, nD);
    gNrd2OutS[p] = float4(outS, nS);
    gNrd2OutG[p] = float4(z * 0.01, nr.x, nr.y, 0.0);
    gNrd2OutV[p] = var;

    const float4 ra = gNrd2RemodA.Load(int3(p, 0));
    const float3 Rs = float3(ra.a, gNrd2RemodB.Load(int3(p, 0)));
    float3 lit = outD * ra.rgb + outS * Rs;
    if (!all(lit == lit)) lit = 0.0;
    gNrd2Lit[p] = float4(clamp(lit, 0.0, 6.0e4), 0.0);
}

#endif
