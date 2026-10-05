// NRD2 phase 1 (docs/rendering/NRD2.md): single-frame denoiser of Stage B's demodulated lighting.
// One file, one pass per compile (AVER_NRD2_PASS):
//   0 CSNrd2Pyramid -- this frame's D and S reduced to 1/2, 1/4, 1/8 (edge-aware 2x2, fixed)
//   1 CSNrd2Params  -- fills the per-8x8-tile parameter buffer with the defaults (phase 4: the network)
//   2 CSNrd2Resolve -- per pixel D' and S' (nrd2_resolve.hlsli), remodulated: D' * Rd + S' * Rs
//   3 VSNrd2Compose / PSNrd2Compose -- adds that into the scene colour (additive blend)
//
// Portable (modules/render.neural/README.md rules): fp32, no wave intrinsics, no atomics, groupshared
// 3 KB, every barrier in uniform control flow, constants at b3 (the Vulkan backend folds b1 into push
// constants). Texel coordinates of D/S/guides are render-target pixels; the pyramid is viewport-local.
#include "nrd2_resolve.hlsli"

#ifndef AVER_NRD2_PASS
#define AVER_NRD2_PASS 0
#endif

cbuffer Nrd2CB : register(b3) {
    uint4  gNrd2Rect;     // scene viewport x, y, w, h (render-target pixels)
    uint4  gNrd2Tiles;    // x tiles, y tiles, flags (bit 0: own pixel only), unused
    float4 gNrd2Def[3];   // default tile parameters, planes 0..11 (D then S)
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
RWTexture2D<float4> gNrd2Lit : register(u0);    // D' * Rd + S' * Rs

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
    if (!(z > 0.0 && z < 1.0e6)) { gNrd2Lit[p] = 0.0; return; }
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

        const Nrd2TileParams pd = nrd2LoadParams(tile, tiles, 0u);
        float cf1, cf2, cf3;
        const float dS = exp2(pd.log2Depth), nP = exp2(pd.log2Normal);
        const float3 d1 = nrd2Upsample(gNrd2G1, gNrd2D1, 1u, q, s1, zm, n, dS, nP, cf1);
        const float3 d2 = nrd2Upsample(gNrd2G2, gNrd2D2, 2u, q, s2, zm, n, dS, nP, cf2);
        const float3 d3 = nrd2Upsample(gNrd2G3, gNrd2D3, 3u, q, s3, zm, n, dS, nP, cf3);
        dRes = nrd2Combine(d0.rgb, dOwn, d1, d2, d3, cf1, cf2, cf3, pd, 0.0);

        const Nrd2TileParams ps = nrd2LoadParams(tile, tiles, 1u);
        const float sS = exp2(ps.log2Depth), sP = exp2(ps.log2Normal);
        const float3 e1 = nrd2Upsample(gNrd2G1, gNrd2S1, 1u, q, s1, zm, n, sS, sP, cf1);
        const float3 e2 = nrd2Upsample(gNrd2G2, gNrd2S2, 2u, q, s2, zm, n, sS, sP, cf2);
        const float3 e3 = nrd2Upsample(gNrd2G3, gNrd2S3, 3u, q, s3, zm, n, sS, sP, cf3);
        sRes = nrd2Combine(s0.rgb, sOwn, e1, e2, e3, cf1, cf2, cf3, ps,
                           nrd2SpecularExtraLogit(nr.z, s0.a, z));
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

#endif
