// NRD2 phase 1 (docs/rendering/NRD2.md): single-frame denoiser of Stage B's demodulated lighting.
// One file, one pass per compile (AVER_NRD2_PASS):
//   0 CSNrd2Pyramid -- this frame's D and S reduced to 1/2, 1/4, 1/8 (edge-aware 2x2, fixed)
//   1 CSNrd2Params  -- fills the per-8x8-tile parameter buffer with the defaults (phase 4: the network)
//   2 CSNrd2Resolve -- per pixel D' and S' (nrd2_resolve.hlsli), remodulated: D' * Rd + S' * Rs
//                      (stabiliser on: D' and S' go to u1/u2 instead and lit is left to pass 5)
//   3 VSNrd2Compose / PSNrd2Compose -- adds that into the scene colour (additive blend)
//   4 CSNrd2Features -- the network's 12 input channels at half resolution (phase 3 capture, phase 4)
//   5 CSNrd2Stabilise -- jitter-free frames only: D' and S' blended with their reprojected history inside a
//                      min/max box of this frame's values, then remodulated into lit
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
RWTexture2D<float4> gNrd2DRes : register(u1);   // stabiliser input: D' and S', a = 1 on surfaces
RWTexture2D<float4> gNrd2SRes : register(u2);

float3 nrd2StabSane(float3 v) { return all(v == v) ? clamp(v, -6.0e4, 6.0e4) : 0.0; }

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

#elif AVER_NRD2_PASS == 5   // ---- temporal stabiliser ----

// Per pixel: this frame's D' and S' (the resolve's, spatially filtered) against last frame's stabilised ones
// at the velocity-reprojected position, in irradiance so albedo detail never blurs. History is accepted per
// bilinear tap by reprojected view depth, blended with a weight from motion/disocclusion age alone, and
// clamped to the min/max of this frame's 3x3 of D' (S') united with the four 1/8-level texels the resolve's
// own taps read. Never a mean or variance, never a network input (NEURAA_NRD.md section 7).
Texture2D<float4> gNrd2DRes   : register(t0);   // D', a = 1 on surfaces
Texture2D<float4> gNrd2SRes   : register(t1);
Texture2D<float>  gNrd2ViewZ  : register(t2);
Texture2D<float4> gNrd2Normal : register(t3);
Texture2D<float2> gNrd2Vel    : register(t4);   // G-buffer velocity: texels per frame, destination minus source
Texture2D<float4> gNrd2RemodA : register(t5);
Texture2D<float2> gNrd2RemodB : register(t6);
Texture2D<float4> gNrd2HistD  : register(t7);   // last frame's D'' rgb, a = age (frames)
Texture2D<float4> gNrd2HistS  : register(t8);   // last frame's S'' rgb, a = view Z (m)
Texture2D<float4> gNrd2D3     : register(t9);   // 1/8 level values
Texture2D<float4> gNrd2S3     : register(t10);
RWTexture2D<float4> gNrd2Lit  : register(u0);
RWTexture2D<float4> gNrd2OutD : register(u1);
RWTexture2D<float4> gNrd2OutS : register(u2);

groupshared float4 gsStabD[100];   // 10x10 halo of D' and S'
groupshared float4 gsStabS[100];

// View Z (cm) at a viewport-local texel, or `fallback` outside the viewport or off a surface.
float nrd2StabZ(int2 q, float fallback) {
    if (any(q < 0) || any(q >= int2(gNrd2Rect.zw))) return fallback;
    const float z = gNrd2ViewZ.Load(int3(int2(gNrd2Rect.xy) + q, 0));
    return (z > 0.0 && z < 1.0e6) ? z : fallback;
}

[numthreads(8, 8, 1)]
void CSNrd2Stabilise(uint3 dtid : SV_DispatchThreadID, uint3 gtid : SV_GroupThreadID, uint3 gid : SV_GroupID) {
    const int2 org = int2(gid.xy) * 8 - 1;   // halo origin, viewport-local
    const uint gi = gtid.y * 8u + gtid.x;
    [unroll] for (uint k = 0u; k < 2u; ++k) {
        const uint i = gi + k * 64u;
        if (i < 100u) {
            const int2 qq = org + int2(i % 10u, i / 10u);
            const bool ok = all(qq >= 0) && all(qq < int2(gNrd2Rect.zw));
            const int3 pp = int3(int2(gNrd2Rect.xy) + qq, 0);
            gsStabD[i] = ok ? gNrd2DRes.Load(pp) : 0.0;
            gsStabS[i] = ok ? gNrd2SRes.Load(pp) : 0.0;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    const uint2 q = dtid.xy;
    if (any(q >= gNrd2Rect.zw)) return;   // after the barrier
    const int2 p = int2(gNrd2Rect.xy + q);
    const float z = gNrd2ViewZ.Load(int3(p, 0));
    if (!(z > 0.0 && z < 1.0e6)) { gNrd2Lit[p] = 0.0; gNrd2OutD[p] = 0.0; gNrd2OutS[p] = 0.0; return; }
    const float zm = z * 0.01;

    // The clamp box: this frame's valid 3x3, united with the resolve's own 1/8 taps.
    const int li = int((gtid.y + 1u) * 10u + gtid.x + 1u);
    float nmD = 0.0, nmS = 0.0;
    [unroll] for (uint f = 0u; f < 9u; ++f) {
        if (f == 4u) continue;
        const int fi = li + (int(f / 3u) - 1) * 10 + int(f % 3u) - 1;
        if (gsStabD[fi].a > 0.5) nmD = max(nmD, dot(gsStabD[fi].rgb, float3(0.2126, 0.7152, 0.0722)));
        if (gsStabS[fi].a > 0.5) nmS = max(nmS, dot(gsStabS[fi].rgb, float3(0.2126, 0.7152, 0.0722)));
    }
    const float3 curD = nrd2StabFirefly(gsStabD[li].rgb, nmD), curS = nrd2StabFirefly(gsStabS[li].rgb, nmS);
    float3 loD = curD, hiD = curD, loS = curS, hiS = curS;
    [unroll] for (uint n = 0u; n < 9u; ++n) {
        if (n == 4u) continue;   // the centre is curD/curS, already clamped
        const int ni = li + (int(n / 3u) - 1) * 10 + int(n % 3u) - 1;
        if (gsStabD[ni].a > 0.5) { loD = min(loD, gsStabD[ni].rgb); hiD = max(hiD, gsStabD[ni].rgb); }
        if (gsStabS[ni].a > 0.5) { loS = min(loS, gsStabS[ni].rgb); hiS = max(hiS, gsStabS[ni].rgb); }
    }
    {
        const uint2 ls = nrd2LevelSize(3u);
        const int2 cbase = int2(floor((float2(q) + 0.5) / 8.0 - 0.5));
        [unroll] for (uint c = 0u; c < 4u; ++c) {
            const int3 ct = int3(clamp(cbase + int2(c & 1u, c >> 1), int2(0, 0), int2(ls) - 1), 0);
            const float4 cd = gNrd2D3.Load(ct), cs = gNrd2S3.Load(ct);
            if (cd.a > 0.0) { loD = min(loD, cd.rgb); hiD = max(hiD, cd.rgb); }
            if (cs.a > 0.0) { loS = min(loS, cs.rgb); hiS = max(hiS, cs.rgb); }
        }
    }

    // Where this surface point was last frame: the velocity (exact for rigid motion; no jitter on these
    // frames), and the view depth it should have there (camera motion only, so a mover fails the depth test).
    const float2 mv = gNrd2Vel.Load(int3(p, 0));
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

    const float2 hp = float2(p) - mv;   // p + 0.5 - mv, as a texel corner
    const int2 hb = int2(floor(hp));
    const float2 hf = hp - float2(hb);
    Nrd2StabTap tap[4];
    [unroll] for (uint t = 0u; t < 4u; ++t) {
        const int2 o = int2(t & 1u, t >> 1);
        const int2 tc = hb + o;
        const bool inside = all(tc >= int2(gNrd2Rect.xy)) && all(tc < int2(gNrd2Rect.xy + gNrd2Rect.zw));
        const float4 hd = inside ? gNrd2HistD.Load(int3(tc, 0)) : 0.0;
        const float4 hs = inside ? gNrd2HistS.Load(int3(tc, 0)) : 0.0;
        tap[t].b = inside ? (o.x ? hf.x : 1.0 - hf.x) * (o.y ? hf.y : 1.0 - hf.y) : 0.0;
        tap[t].age = hd.a; tap[t].zm = hs.a; tap[t].d = hd.rgb; tap[t].s = hs.rgb;
    }

    float3 outD, outS;
    float  outAge;
    nrd2StabCombine(curD, curS, loD, hiD, loS, hiS, tap, zExp, zTol,
                    nrd2StabMaxFrames(length(mv), gNrd2Stab.x, gNrd2Stab.y), gNrd2Normal.Load(int3(p, 0)).z,
                    (gNrd2Tiles.z & 4u) != 0u, outD, outS, outAge);
    gNrd2OutD[p] = float4(outD, outAge);
    gNrd2OutS[p] = float4(outS, zm);

    const float4 ra = gNrd2RemodA.Load(int3(p, 0));
    const float3 Rs = float3(ra.a, gNrd2RemodB.Load(int3(p, 0)));
    float3 lit = outD * ra.rgb + outS * Rs;
    if (!all(lit == lit)) lit = 0.0;
    gNrd2Lit[p] = float4(clamp(lit, 0.0, 6.0e4), 0.0);
}

#endif
