// NRD2 phase 3 (docs/rendering/NRD2.md): training capture and the per-tile oracle fit. One pass per
// compile (AVER_NRD2C_PASS):
//   0 CSNrd2CapGeo       -- the pose's guides per pixel (view Z, normal, roughness, loss mask), once
//   1 CSNrd2CapAccum     -- split-half running means of D and S over fresh pixels (the target)
//   2 CSNrd2CapTileStats -- per tile and signal: loss normaliser, tile weight; resets the fit state
//   3 CSNrd2CapEval      -- data loss of a candidate set (27-start grid or pattern step), one snapshot
//   4 CSNrd2CapGrad      -- data loss and d(loss)/d(theta) at the current theta, one snapshot
//   5 CSNrd2CapStep      -- per tile and signal: grid select, Adam step, pattern select or finalize
// CPU twin: Nrd2ResolveReference.cpp (nrd2TileLoss, nrd2FitTile). KEEP IN SYNC.
//
// Patent rule 4 (NEURAA_NRD.md section 7): only free per-tile variables go through the filter here; no
// model is involved. Portable: fp32, no wave intrinsics, no atomics, 64-thread groups (one group = one
// 8x8 tile in passes 2-4), barriers in compile-time loops only, groupshared 3.5 KB, constants at b3.
#include "nrd2_resolve.hlsli"

#ifndef AVER_NRD2C_PASS
#define AVER_NRD2C_PASS 0
#endif

cbuffer Nrd2CapCB : register(b3) {
    uint4  gCapRect;    // scene viewport x, y, w, h (render-target pixels)
    uint4  gCapTiles;   // tiles x, tiles y, flags, unused
    uint4  gCapMode;    // step mode or candidate set, candidate count, frames K, unused
    float4 gCapDef[3];  // theta0, planes 0..11 (D then S)
    float4 gCapAdam;    // learning rate, beta1, beta2, epsilon
    float4 gCapAdam2;   // bias corrections 1 - beta1^t and 1 - beta2^t, lambda, split-half r0
};

// Flags (gCapTiles.z). Accum: reset, half, then half-rate bits. Eval/Grad: first snapshot.
#define NRD2C_RESET       1u
#define NRD2C_HALF        2u
#define NRD2C_D_HALFRATE  4u
#define NRD2C_D_PARITY    8u
#define NRD2C_S_HALFRATE  16u
#define NRD2C_S_PARITY    32u
#define NRD2C_FIRST       1u

// Per pixel buffers are viewport-local, plane-major: value = buf[plane * w * h + y * w + x].
#define NRD2C_GEO_PLANES  6   // view Z (cm, 0 none), normal xyz, roughness, mask (1 D usable, 2 surface)
#define NRD2C_MEAN_PLANES 16  // (half * 2 + signal) * 4 + {r, g, b, count}
// Fit state per (tile, signal), 32 floats at (tile * 2 + signal) * 32.
#define NRD2C_REC         32
#define NRD2C_THETA       0
#define NRD2C_M           6
#define NRD2C_V           12
#define NRD2C_BEST        18
#define NRD2C_BESTOBJ     24
#define NRD2C_BESTDATA    25
#define NRD2C_DEFDATA     26
#define NRD2C_WEIGHT      27
#define NRD2C_DENOM       28
#define NRD2C_COUNT       29
#define NRD2C_STEP        30
#define NRD2C_GRID        27
#define NRD2C_GRID_DEFAULT 13
#define NRD2C_PATTERN     12
#define NRD2C_OUT_PLANES  18  // theta 12, weight 2, oracle loss 2, default loss 2

static const float kNrd2LossEps = 1.0e-8;
static const float kNrd2BigObj  = 3.0e38;

uint nrd2cPixels() { return gCapRect.z * gCapRect.w; }
uint nrd2cTiles() { return gCapTiles.x * gCapTiles.y; }
float nrd2cDef(uint signal, uint k) { const uint plane = signal * 6u + k; return gCapDef[plane >> 2][plane & 3u]; }

void nrd2cClamp(inout float t[6]) {
    [unroll] for (uint k = 0u; k < 3u; ++k) t[k] = clamp(t[k], -16.0, 16.0);
    [unroll] for (uint k2 = 3u; k2 < 6u; ++k2) t[k2] = clamp(t[k2], -8.0, 8.0);
}

void nrd2cGridCandidate(uint signal, uint c, out float t[6]) {
    static const float kLogit[3] = {-3.0, 0.0, 3.0};
    static const float kDepth[3] = {-1.5, 0.0, 1.5};
    static const float kLum[3]   = {-2.0, 0.0, 2.0};
    [unroll] for (uint k = 0u; k < 6u; ++k) t[k] = nrd2cDef(signal, k);
    [unroll] for (uint k2 = 0u; k2 < 3u; ++k2) t[k2] += kLogit[c % 3u];
    t[3] += kDepth[(c / 3u) % 3u];
    t[5] += kLum[(c / 9u) % 3u];
    nrd2cClamp(t);
}

void nrd2cPatternCandidate(float base[6], float step, uint c, out float t[6]) {
    [unroll] for (uint k = 0u; k < 6u; ++k) t[k] = base[k] + ((k == (c / 2u) % 6u) ? ((c & 1u) ? -step : step) : 0.0);
    nrd2cClamp(t);
}

float nrd2cRegulariser(float t[6], uint signal) {
    float r = 0.0;
    [unroll] for (uint k = 0u; k < 6u; ++k) { const float d = t[k] - nrd2cDef(signal, k); r += d * d; }
    return r;
}

Nrd2TileParams nrd2cParams(float t[6], uint signal) {
    float d[6];
    [unroll] for (uint k = 0u; k < 6u; ++k) d[k] = nrd2cDef(signal, k);
    return nrd2SanitiseParams(t, d);
}

#if AVER_NRD2C_PASS == 0   // ---- pose guides ----

Texture2D<float>  gCapViewZ  : register(t0);
Texture2D<float4> gCapNormal : register(t1);
Texture2D<float4> gCapD      : register(t2);
RWStructuredBuffer<float> gCapGeo : register(u0);

[numthreads(8, 8, 1)]
void CSNrd2CapGeo(uint3 dtid : SV_DispatchThreadID) {
    const uint2 q = dtid.xy;
    if (any(q >= gCapRect.zw)) return;   // no barriers in this pass
    const int3 p = int3(int2(gCapRect.xy + q), 0);
    const float z = gCapViewZ.Load(p);
    const bool surf = z > 0.0 && z < 1.0e6;
    const float4 nr = gCapNormal.Load(p);
    const float3 n = surf ? nrd2DecodeNormal(nr) : 0.0;
    const uint mask = surf ? (2u | (gCapD.Load(p).a > 0.5 ? 1u : 0u)) : 0u;
    const uint at = q.y * gCapRect.z + q.x, np = nrd2cPixels();
    gCapGeo[0u * np + at] = surf ? z : 0.0;
    gCapGeo[1u * np + at] = n.x;
    gCapGeo[2u * np + at] = n.y;
    gCapGeo[3u * np + at] = n.z;
    gCapGeo[4u * np + at] = surf ? nr.z : 0.0;
    gCapGeo[5u * np + at] = float(mask);
}

#elif AVER_NRD2C_PASS == 1   // ---- split-half running means ----

Texture2D<float4> gCapD     : register(t0);
Texture2D<float4> gCapS     : register(t1);
Texture2D<float>  gCapViewZ : register(t2);
RWStructuredBuffer<float> gCapMean : register(u0);

void nrd2cAccumulate(uint plane0, uint at, uint np, float3 v) {
    const float n = gCapMean[(plane0 + 3u) * np + at] + 1.0;
    [unroll] for (uint c = 0u; c < 3u; ++c) {
        const float m = gCapMean[(plane0 + c) * np + at];
        gCapMean[(plane0 + c) * np + at] = m + (v[c] - m) / n;
    }
    gCapMean[(plane0 + 3u) * np + at] = n;
}

[numthreads(8, 8, 1)]
void CSNrd2CapAccum(uint3 dtid : SV_DispatchThreadID) {
    const uint2 q = dtid.xy;
    if (any(q >= gCapRect.zw)) return;   // no barriers in this pass
    const uint flags = gCapTiles.z;
    const uint at = q.y * gCapRect.z + q.x, np = nrd2cPixels();
    if ((flags & NRD2C_RESET) != 0u)
        for (uint k = 0u; k < NRD2C_MEAN_PLANES; ++k) gCapMean[k * np + at] = 0.0;
    const uint2 p = gCapRect.xy + q;
    const float z = gCapViewZ.Load(int3(p, 0));
    if (!(z > 0.0 && z < 1.0e6)) return;
    const uint hf = (flags & NRD2C_HALF) != 0u ? 1u : 0u;
    // Fresh = traced this frame: D follows ReSTIR GI's checkerboard, S the glossy reflections'.
    const uint dPar = (flags & NRD2C_D_PARITY) != 0u ? 1u : 0u;
    const uint sPar = (flags & NRD2C_S_PARITY) != 0u ? 1u : 0u;
    const bool dFresh = (flags & NRD2C_D_HALFRATE) == 0u || ((p.x ^ p.y ^ dPar) & 1u) == 0u;
    const bool sFresh = (flags & NRD2C_S_HALFRATE) == 0u || ((p.x ^ p.y ^ sPar) & 1u) == 0u;
    const float4 d = gCapD.Load(int3(p, 0));
    const float4 s = gCapS.Load(int3(p, 0));
    if (dFresh && d.a > 0.5 && all(d.rgb == d.rgb) && all(abs(d.rgb) < 6.0e4))
        nrd2cAccumulate((hf * 2u + 0u) * 4u, at, np, d.rgb);
    if (sFresh && all(s.rgb == s.rgb) && all(abs(s.rgb) < 6.0e4))
        nrd2cAccumulate((hf * 2u + 1u) * 4u, at, np, s.rgb);
}

#else   // ---- passes over tiles and fit state ----

// The converged target for one pixel and signal (both halves pooled); false without a sample.
bool nrd2cTarget(StructuredBuffer<float> mean, uint at, uint np, uint signal, out float3 t, out float3 te,
                 out float3 to, out float ne, out float no) {
    const uint e = (0u * 2u + signal) * 4u, o = (1u * 2u + signal) * 4u;
    te = float3(mean[e * np + at], mean[(e + 1u) * np + at], mean[(e + 2u) * np + at]);
    to = float3(mean[o * np + at], mean[(o + 1u) * np + at], mean[(o + 2u) * np + at]);
    ne = mean[(e + 3u) * np + at];
    no = mean[(o + 3u) * np + at];
    const float n = ne + no;
    t = n > 0.0 ? (te * ne + to * no) / n : 0.0;
    return n > 0.0;
}

#if AVER_NRD2C_PASS == 2   // ---- tile statistics, fit reset ----

StructuredBuffer<float> gCapGeo  : register(t0);
StructuredBuffer<float> gCapMean : register(t1);
RWStructuredBuffer<float> gCapState : register(u0);

groupshared float gsT[8][64];

[numthreads(64, 1, 1)]
void CSNrd2CapTileStats(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint gi = gtid.x;
    const uint tile = gid.y * gCapTiles.x + gid.x;
    const uint2 q = gid.xy * 8u + uint2(gi & 7u, gi >> 3);
    const bool inside = all(q < gCapRect.zw);
    const uint np = nrd2cPixels();
    const uint at = inside ? q.y * gCapRect.z + q.x : 0u;
    const uint mask = inside ? uint(gCapGeo[5u * np + at]) : 0u;
    [unroll] for (uint sig = 0u; sig < 2u; ++sig) {
        float3 t, te, to;
        float ne, no;
        const bool has = nrd2cTarget(gCapMean, at, np, sig, t, te, to, ne, no);
        const bool valid = inside && (mask & (sig == 0u ? 1u : 2u)) != 0u && has;
        const bool both = valid && ne > 0.0 && no > 0.0;
        const float dl = nrd2Lum(te) - nrd2Lum(to);
        gsT[sig * 4u + 0u][gi] = valid ? 1.0 : 0.0;
        gsT[sig * 4u + 1u][gi] = valid ? nrd2Lum(t) : 0.0;
        gsT[sig * 4u + 2u][gi] = both ? dl * dl : 0.0;
        gsT[sig * 4u + 3u][gi] = both ? 1.0 : 0.0;
    }
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 32u; s > 0u; s >>= 1u) {
        if (gi < s) { [unroll] for (uint v = 0u; v < 8u; ++v) gsT[v][gi] += gsT[v][gi + s]; }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi < 2u && tile < nrd2cTiles()) {
        const uint sig = gi;
        const uint rec = (tile * 2u + sig) * NRD2C_REC;
        const float count = gsT[sig * 4u][0];
        const float meanLum = count > 0.0 ? gsT[sig * 4u + 1u][0] / count : 0.0;
        const float nb = gsT[sig * 4u + 3u][0];
        const float r = nb > 0.0 ? sqrt(gsT[sig * 4u + 2u][0] / nb) / (meanLum + 1.0e-6) : 0.0;
        [unroll] for (uint k = 0u; k < 6u; ++k) {
            const float d = nrd2cDef(sig, k);
            gCapState[rec + NRD2C_THETA + k] = d;
            gCapState[rec + NRD2C_M + k] = 0.0;
            gCapState[rec + NRD2C_V + k] = 0.0;
            gCapState[rec + NRD2C_BEST + k] = d;
        }
        gCapState[rec + NRD2C_BESTOBJ]  = kNrd2BigObj;
        gCapState[rec + NRD2C_BESTDATA] = 0.0;
        gCapState[rec + NRD2C_DEFDATA]  = 0.0;
        gCapState[rec + NRD2C_WEIGHT]   = saturate(count / 64.0) / (1.0 + max(r, 0.0) / gCapAdam2.w);
        gCapState[rec + NRD2C_DENOM]    = meanLum * meanLum + kNrd2LossEps;
        gCapState[rec + NRD2C_COUNT]    = count;
        gCapState[rec + NRD2C_STEP]     = 1.0;
        gCapState[rec + 31u]            = 0.0;
    }
}

#elif AVER_NRD2C_PASS == 3 || AVER_NRD2C_PASS == 4   // ---- loss (and gradient) over one snapshot ----

Texture2D<float4> gCapD  : register(t0);   // this snapshot's D and S (render-target pixels)
Texture2D<float4> gCapS  : register(t1);
Texture2D<float4> gCapG1 : register(t2);   // the pose's guides (viewport-local levels)
Texture2D<float4> gCapG2 : register(t3);
Texture2D<float4> gCapG3 : register(t4);
Texture2D<float4> gCapD1 : register(t5);   // this snapshot's levels
Texture2D<float4> gCapD2 : register(t6);
Texture2D<float4> gCapD3 : register(t7);
Texture2D<float4> gCapS1 : register(t8);
Texture2D<float4> gCapS2 : register(t9);
Texture2D<float4> gCapS3 : register(t10);
StructuredBuffer<float> gCapGeo  : register(t11);
StructuredBuffer<float> gCapMean : register(t12);
RWStructuredBuffer<float> gCapState : register(u0);
RWStructuredBuffer<float> gCapAcc   : register(u1);   // 32 per (tile, signal)

uint2 nrd2cLevelSize(uint shift) { return ((gCapRect.zw + 7u) / 8u) * (8u >> shift); }

struct Nrd2cPixel {
    bool   valid[2];
    float3 c0[2];
    bool   own[2];
    float3 target[2];
    float3 extra;
    float  zm;
    float3 n;
    uint2  q;   // viewport-local, clamped inside
};

Nrd2cPixel nrd2cLoadPixel(uint2 q, bool inside) {
    Nrd2cPixel px;
    const uint np = nrd2cPixels();
    const uint at = inside ? q.y * gCapRect.z + q.x : 0u;
    px.q = inside ? q : uint2(0, 0);
    const int3 p = int3(int2(gCapRect.xy + px.q), 0);
    const float z = gCapGeo[at];
    const uint mask = inside ? uint(gCapGeo[5u * np + at]) : 0u;
    px.zm = z * 0.01;
    px.n = float3(gCapGeo[np + at], gCapGeo[2u * np + at], gCapGeo[3u * np + at]);
    const float rough = gCapGeo[4u * np + at];
    const float4 d0 = gCapD.Load(p);
    const float4 s0 = gCapS.Load(p);
    px.c0[0] = d0.rgb;
    px.c0[1] = s0.rgb;
    px.own[0] = d0.a > 0.5 && all(d0.rgb == d0.rgb);
    px.own[1] = all(s0.rgb == s0.rgb);
    px.extra = nrd2SpecularExtraLogit(rough, s0.a, z);
    [unroll] for (uint sig = 0u; sig < 2u; ++sig) {
        float3 te, to;
        float ne, no;
        const bool has = nrd2cTarget(gCapMean, at, np, sig, px.target[sig], te, to, ne, no);
        px.valid[sig] = inside && (mask & (sig == 0u ? 1u : 2u)) != 0u && has && z > 0.0;
    }
    return px;
}

// The pixel's taps on one signal's three levels (loaded once, reused by every candidate).
void nrd2cLoadTaps(uint sig, uint2 q, out Nrd2Taps t[3]) {
    const uint2 s1 = nrd2cLevelSize(1u), s2 = nrd2cLevelSize(2u), s3 = nrd2cLevelSize(3u);
    if (sig == 0u) {
        t[0] = nrd2LoadTaps(gCapG1, gCapD1, 1u, q, s1);
        t[1] = nrd2LoadTaps(gCapG2, gCapD2, 2u, q, s2);
        t[2] = nrd2LoadTaps(gCapG3, gCapD3, 3u, q, s3);
    } else {
        t[0] = nrd2LoadTaps(gCapG1, gCapS1, 1u, q, s1);
        t[1] = nrd2LoadTaps(gCapG2, gCapS2, 2u, q, s2);
        t[2] = nrd2LoadTaps(gCapG3, gCapS3, 3u, q, s3);
    }
}

float3 nrd2cForward(Nrd2cPixel px, Nrd2Taps tp[3], uint sig, float t[6]) {
    const Nrd2TileParams p = nrd2cParams(t, sig);
    const float dS = exp2(p.log2Depth), nP = exp2(p.log2Normal);
    float cf1, cf2, cf3;
    const float3 c1 = nrd2UpsampleTaps(tp[0], px.zm, px.n, dS, nP, cf1);
    const float3 c2 = nrd2UpsampleTaps(tp[1], px.zm, px.n, dS, nP, cf2);
    const float3 c3 = nrd2UpsampleTaps(tp[2], px.zm, px.n, dS, nP, cf3);
    return nrd2Combine(px.c0[sig], px.own[sig], c1, c2, c3, cf1, cf2, cf3, p, sig == 1u ? px.extra : 0.0);
}

groupshared float gsL[14][64];

// Fixed-order tree sum of the first `lanes` rows into gsL[v][0]. Starts and ends with a barrier.
void nrd2cReduce(uint gi, uint lanes) {
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 32u; s > 0u; s >>= 1u) {
        if (gi < s) { for (uint v = 0u; v < lanes; ++v) gsL[v][gi] += gsL[v][gi + s]; }
        GroupMemoryBarrierWithGroupSync();
    }
}

void nrd2cStore(uint at, float v) {
    gCapAcc[at] = ((gCapTiles.z & NRD2C_FIRST) != 0u ? 0.0 : gCapAcc[at]) + v;
}

#if AVER_NRD2C_PASS == 3
// gCapMode: x candidate set (0 the 27-start grid around theta0, 1 +-step around theta), y count.
[numthreads(64, 1, 1)]
void CSNrd2CapEval(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint gi = gtid.x;
    const uint tile = gid.y * gCapTiles.x + gid.x;
    const uint2 q = gid.xy * 8u + uint2(gi & 7u, gi >> 3);
    const Nrd2cPixel px = nrd2cLoadPixel(q, all(q < gCapRect.zw));
    const bool liveTile = tile < nrd2cTiles();
    const uint rec0 = liveTile ? tile * 2u * NRD2C_REC : 0u;
    [unroll] for (uint sig = 0u; sig < 2u; ++sig) {
        const uint rec = rec0 + sig * NRD2C_REC;
        float base[6];
        [unroll] for (uint k = 0u; k < 6u; ++k) base[k] = gCapState[rec + NRD2C_THETA + k];
        const float step = gCapState[rec + NRD2C_STEP];
        const float denom = gCapState[rec + NRD2C_DENOM];
        Nrd2Taps tp[3];
        nrd2cLoadTaps(sig, px.q, tp);
        for (uint c = 0u; c < NRD2C_GRID; ++c) {   // constant bound: every barrier is uniform
            const bool live = c < gCapMode.y;
            float l = 0.0;
            if (live && px.valid[sig]) {
                float t[6];
                if (gCapMode.x == 0u) nrd2cGridCandidate(sig, c, t);
                else nrd2cPatternCandidate(base, step, c, t);
                const float3 r = nrd2cForward(px, tp, sig, t) - px.target[sig];
                l = dot(r, r) / denom;
                if (!(l == l) || l > 1.0e30) l = 0.0;
            }
            gsL[0][gi] = l;
            nrd2cReduce(gi, 1u);
            if (gi == 0u && live && liveTile) nrd2cStore(rec + c, gsL[0][0]);
            GroupMemoryBarrierWithGroupSync();
        }
    }
}
#else
[numthreads(64, 1, 1)]
void CSNrd2CapGrad(uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID) {
    const uint gi = gtid.x;
    const uint tile = gid.y * gCapTiles.x + gid.x;
    const uint2 q = gid.xy * 8u + uint2(gi & 7u, gi >> 3);
    const Nrd2cPixel px = nrd2cLoadPixel(q, all(q < gCapRect.zw));
    const bool liveTile = tile < nrd2cTiles();
    const uint rec0 = liveTile ? tile * 2u * NRD2C_REC : 0u;
    [unroll] for (uint sig = 0u; sig < 2u; ++sig) {
        float v[7] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
        if (px.valid[sig]) {
            const uint rec = rec0 + sig * NRD2C_REC;
            float t[6];
            [unroll] for (uint k = 0u; k < 6u; ++k) t[k] = gCapState[rec + NRD2C_THETA + k];
            const float denom = gCapState[rec + NRD2C_DENOM];
            const Nrd2TileParams p = nrd2cParams(t, sig);
            Nrd2Taps tp[3];
            nrd2cLoadTaps(sig, px.q, tp);
            Nrd2Level lv[3];
            [unroll] for (uint l = 0u; l < 3u; ++l) lv[l] = nrd2UpsampleTapsGrad(tp[l], px.zm, px.n, p.log2Depth, p.log2Normal);
            float3 dOut[6];
            const float3 o = nrd2ResolveBackward(px.c0[sig], px.own[sig], lv, p, sig == 1u ? px.extra : 0.0, dOut);
            const float3 r = o - px.target[sig];
            v[0] = dot(r, r) / denom;
            [unroll] for (uint j = 0u; j < 6u; ++j) v[1u + j] = 2.0 * dot(r, dOut[j]) / denom;
            bool ok = v[0] < 1.0e30;
            [unroll] for (uint j2 = 0u; j2 < 7u; ++j2) ok = ok && v[j2] == v[j2];
            if (!ok) { [unroll] for (uint j3 = 0u; j3 < 7u; ++j3) v[j3] = 0.0; }
        }
        [unroll] for (uint j = 0u; j < 7u; ++j) gsL[sig * 7u + j][gi] = v[j];
    }
    nrd2cReduce(gi, 14u);
    if (gi < 14u && liveTile) nrd2cStore(rec0 + (gi / 7u) * NRD2C_REC + gi % 7u, gsL[gi][0]);
}
#endif

#elif AVER_NRD2C_PASS == 5   // ---- per (tile, signal) step ----

RWStructuredBuffer<float> gCapState : register(u0);
RWStructuredBuffer<float> gCapAcc   : register(u1);
RWStructuredBuffer<float> gCapOut   : register(u2);   // NRD2C_OUT_PLANES x tiles

// Modes (gCapMode.x): 0 grid select, 1 Adam step, 2 pattern select, 3 finalize after a loss pass,
// 4 finalize as is.
[numthreads(64, 1, 1)]
void CSNrd2CapStep(uint3 dtid : SV_DispatchThreadID) {
    const uint tiles = nrd2cTiles();
    if (dtid.x >= tiles * 2u) return;   // no barriers in this pass
    const uint tile = dtid.x >> 1, sig = dtid.x & 1u;
    const uint rec = dtid.x * NRD2C_REC, mode = gCapMode.x;
    const float count = gCapState[rec + NRD2C_COUNT];
    const float lambda = gCapAdam2.z;
    float th[6], best[6];
    [unroll] for (uint k = 0u; k < 6u; ++k) { th[k] = gCapState[rec + NRD2C_THETA + k]; best[k] = gCapState[rec + NRD2C_BEST + k]; }
    float bestObj = gCapState[rec + NRD2C_BESTOBJ], bestData = gCapState[rec + NRD2C_BESTDATA];
    const float norm = max(float(gCapMode.z) * count, 1.0);

    if (count > 0.0) {
        if (mode == 0u) {
            for (uint c = 0u; c < NRD2C_GRID; ++c) {
                float t[6];
                nrd2cGridCandidate(sig, c, t);
                const float data = gCapAcc[rec + c] / norm;
                const float obj = data + lambda * nrd2cRegulariser(t, sig);
                if (c == NRD2C_GRID_DEFAULT) gCapState[rec + NRD2C_DEFDATA] = data;
                if (obj < bestObj) { bestObj = obj; bestData = data; [unroll] for (uint k = 0u; k < 6u; ++k) best[k] = t[k]; }
            }
            [unroll] for (uint k2 = 0u; k2 < 6u; ++k2) {
                th[k2] = best[k2];
                gCapState[rec + NRD2C_M + k2] = 0.0;
                gCapState[rec + NRD2C_V + k2] = 0.0;
            }
        } else if (mode == 1u || mode == 3u) {
            const float data = gCapAcc[rec] / norm;
            const float obj = data + lambda * nrd2cRegulariser(th, sig);
            if (obj < bestObj) { bestObj = obj; bestData = data; [unroll] for (uint k = 0u; k < 6u; ++k) best[k] = th[k]; }
            if (mode == 1u) {
                [unroll] for (uint k = 0u; k < 6u; ++k) {
                    const float g = gCapAcc[rec + 1u + k] / norm + 2.0 * lambda * (th[k] - nrd2cDef(sig, k));
                    const float m = gCapAdam.y * gCapState[rec + NRD2C_M + k] + (1.0 - gCapAdam.y) * g;
                    const float v = gCapAdam.z * gCapState[rec + NRD2C_V + k] + (1.0 - gCapAdam.z) * g * g;
                    gCapState[rec + NRD2C_M + k] = m;
                    gCapState[rec + NRD2C_V + k] = v;
                    th[k] -= gCapAdam.x * (m / gCapAdam2.x) / (sqrt(v / gCapAdam2.y) + gCapAdam.w);
                }
                nrd2cClamp(th);
            }
        } else if (mode == 2u) {
            const float step = gCapState[rec + NRD2C_STEP];
            float roundObj = kNrd2BigObj, roundData = 0.0, roundT[6] = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};
            for (uint c = 0u; c < NRD2C_PATTERN; ++c) {
                float t[6];
                nrd2cPatternCandidate(th, step, c, t);
                const float data = gCapAcc[rec + c] / norm;
                const float obj = data + lambda * nrd2cRegulariser(t, sig);
                if (obj < roundObj) { roundObj = obj; roundData = data; [unroll] for (uint k = 0u; k < 6u; ++k) roundT[k] = t[k]; }
            }
            if (roundObj < bestObj) {
                bestObj = roundObj; bestData = roundData;
                [unroll] for (uint k = 0u; k < 6u; ++k) { best[k] = roundT[k]; th[k] = roundT[k]; }
            } else {
                gCapState[rec + NRD2C_STEP] = step * 0.5;
            }
        }
    }
    [unroll] for (uint k3 = 0u; k3 < 6u; ++k3) {
        gCapState[rec + NRD2C_THETA + k3] = th[k3];
        gCapState[rec + NRD2C_BEST + k3] = best[k3];
    }
    gCapState[rec + NRD2C_BESTOBJ] = bestObj;
    gCapState[rec + NRD2C_BESTDATA] = bestData;

    if (mode >= 3u) {
        const bool fitted = count > 0.0;
        [unroll] for (uint k = 0u; k < 6u; ++k)
            gCapOut[(sig * 6u + k) * tiles + tile] = fitted ? best[k] : nrd2cDef(sig, k);
        gCapOut[(12u + sig) * tiles + tile] = fitted ? gCapState[rec + NRD2C_WEIGHT] : 0.0;
        gCapOut[(14u + sig) * tiles + tile] = fitted ? bestData : 0.0;
        gCapOut[(16u + sig) * tiles + tile] = fitted ? gCapState[rec + NRD2C_DEFDATA] : 0.0;
    }
}

#endif
#endif
