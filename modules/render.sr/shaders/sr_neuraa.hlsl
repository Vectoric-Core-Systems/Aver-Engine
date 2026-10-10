// NeuRAA (docs/rendering/NEURAA_NRD.md section 3), two passes selected by AVER_NEURAA_PASS:
//   0 CSNeuRaaDetect  -- per pixel: an edge code and, toward each edge, the distance to its own
//                        triangle's edge; per 8x8 tile a flag; the edge-class debug image.
//   1 CSNeuRaaResolve -- the baseline blend: each edge pixel mixes with the neighbour across the
//                        edge by the covered fraction (distance-to-edge AA, Malan 2010 / Persson 2011).
//   2 CSNeuRaaAccum   -- training capture: the running mean of jittered frames' colour (the reference).
//
// Edge code: bits 0-3 = an edge toward the left, right, up, down neighbour; bits 4-5 = class,
// 1 silhouette (another object or the sky), 2 depth step on one object, 3 crease (normal turn).

#ifndef AVER_NEURAA_PASS
#define AVER_NEURAA_PASS 0
#endif

cbuffer AverNeuRaaCB : register(b3) {
    uint4 gVp;     // scene viewport x, y, w, h: the pixels the visibility buffer holds this frame
    uint4 gInfo;   // visibility row pitch, debug view flag, tiles per row, unused
    uint4 gOrg;    // resolve: the first pixel its dispatch covers (x, y), a multiple of 8
};

static const int2 kDir[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};

bool inViewport(int2 p) { return all(p >= int2(gVp.xy)) && all(p < int2(gVp.xy + gVp.zw)); }

#if AVER_NEURAA_PASS == 0

Texture2D<float>  gViewZ  : register(t0);   // G-buffer linear view depth (sky 1e7)
Texture2D<float4> gNormal : register(t1);   // G-buffer octahedral normal + roughness
Texture2D<float4> gColor  : register(t2);   // scene colour, read by the debug view only

RWStructuredBuffer<uint4> gVis   : register(u0);   // (instance ref, triangle, bary.x, bary.y)
RWTexture2D<uint>         gEdges : register(u1);
RWTexture2D<float4>       gDebug : register(u2);
RWStructuredBuffer<uint>  gTiles : register(u3);
RWTexture2D<float4>       gDist  : register(u4);   // per direction: own-triangle edge distance, px (1 = none)

#include "aver_visibility.hlsli"
static const uint  kRefMiss      = kAverRefMiss;   // aver_visibility.hlsli, shared with NRD2
static const float kDepthStep    = 0.03;          // 1/z second difference, relative
static const float kCreaseCos    = 0.866;         // ~30 degrees

uint objectOf(uint ref) { return averObjectOf(ref); }

// voxi.hlsl's averPackNormalRoughness, decoded (as aver_denoise.hlsl's dnsrDecodeNormal).
float3 decodeNormal(float4 e) {
    const float2 f = e.xy * 2.0 - 1.0;
    float3 n = float3(f, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

struct Px { uint4 rec; uint obj; float invZ; float3 n; float3 bary; };

Px loadPx(int2 p) {
    Px o;
    o.rec  = gVis[p.y * gInfo.x + p.x];
    o.obj  = objectOf(o.rec.x);
    o.invZ = 1.0 / max(gViewZ.Load(int3(p, 0)), 1.0e-4);
    o.n    = decodeNormal(gNormal.Load(int3(p, 0)));
    const float2 uv = asfloat(o.rec.zw);
    o.bary = float3(1.0 - uv.x - uv.y, uv.x, uv.y);
    return o;
}

bool sameTriangle(Px a, Px b) { return a.rec.x != kRefMiss && a.rec.x == b.rec.x && a.rec.y == b.rec.y; }

// Class of the edge between a and its neighbour b; `o` is a's other neighbour on the same axis.
// 1/z is linear across a plane in screen space, so its second difference flags a step, not a slope.
uint edgeClass(Px a, Px b, Px o) {
    if (a.obj != b.obj) return 1u;
    if (a.obj == kRefMiss) return 0u;
    if (abs(b.invZ - 2.0 * a.invZ + o.invZ) > kDepthStep * a.invZ) return 2u;
    if (dot(a.n, b.n) < kCreaseCos) return 3u;
    return 0u;
}

// Distance in pixels from the pixel centre, along a unit step whose barycentric change is `g`, to
// where the own triangle ends (a barycentric reaches 0). 1 when it does not end within a pixel.
float edgeDistance(float3 bary, float3 g) {
    float t = 1.0;
    [unroll] for (uint i = 0; i < 3u; ++i)
        if (g[i] < -1.0e-6) t = min(t, bary[i] / -g[i]);
    return saturate(t);
}

float3 classColour(uint cls) {
    return cls == 1u ? float3(1.0, 0.15, 0.1) : cls == 2u ? float3(1.0, 0.85, 0.1) : float3(0.1, 0.8, 1.0);
}

groupshared uint gsTileHasEdge;

[numthreads(8, 8, 1)]
void CSNeuRaaDetect(uint2 gid : SV_GroupID, uint2 gtid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    if (gi == 0u) gsTileHasEdge = 0u;
    GroupMemoryBarrierWithGroupSync();

    const int2 p = int2(gVp.xy) + int2(gid * 8u + gtid);
    if (inViewport(p)) {
        const Px c = loadPx(p);
        Px nb[4];
        bool have[4];
        [unroll] for (uint i = 0; i < 4u; ++i) {
            have[i] = inViewport(p + kDir[i]);
            nb[i] = c;
            if (have[i]) nb[i] = loadPx(p + kDir[i]);
        }
        // Barycentric change per pixel, from a neighbour on the same triangle (none: unknown).
        bool gxOk = true, gyOk = true;
        float3 gx = 0.0, gy = 0.0;
        if (have[1] && sameTriangle(c, nb[1]))      gx = nb[1].bary - c.bary;
        else if (have[0] && sameTriangle(c, nb[0])) gx = c.bary - nb[0].bary;
        else gxOk = false;
        if (have[3] && sameTriangle(c, nb[3]))      gy = nb[3].bary - c.bary;
        else if (have[2] && sameTriangle(c, nb[2])) gy = c.bary - nb[2].bary;
        else gyOk = false;

        uint code = 0u, cls = 0u;
        float dist[4] = {1.0, 1.0, 1.0, 1.0};
        [unroll] for (uint d = 0; d < 4u; ++d) {
            if (!have[d]) continue;
            // The opposite neighbour, unless it is missing or another object: then a first difference.
            const uint od = d ^ 1u;
            Px o = c;
            if (have[od] && nb[od].obj == c.obj) o = nb[od];
            const uint k = edgeClass(c, nb[d], o);
            if (k == 0u) continue;
            code |= 1u << d;
            cls = (cls == 0u || k < cls) ? k : cls;   // silhouette over step over crease
            if (c.rec.x == kRefMiss) continue;        // the sky has no triangle edge
            const bool axisOk = d < 2u ? gxOk : gyOk;
            const float3 g = d < 2u ? gx * float(kDir[d].x) : gy * float(kDir[d].y);
            dist[d] = axisOk ? edgeDistance(c.bary, g) : 0.5;   // unknown: no blend either way
        }
        code |= cls << 4;
        gEdges[p] = code;
        gDist[p] = float4(dist[0], dist[1], dist[2], dist[3]);
        if (code != 0u) InterlockedOr(gsTileHasEdge, 1u);

        if (gInfo.y != 0u) {
            const float3 col = gColor.Load(int3(p, 0)).rgb;
            const float lum = dot(col, float3(0.2126, 0.7152, 0.0722));
            float lumMax = lum;
            [unroll] for (uint j = 0; j < 4u; ++j)
                if (have[j]) lumMax = max(lumMax, dot(gColor.Load(int3(p + kDir[j], 0)).rgb, float3(0.2126, 0.7152, 0.0722)));
            // Scaled by the scene's own brightness so the view survives exposure.
            gDebug[p] = float4(cls != 0u ? classColour(cls) * max(lumMax, 1.0e-4) * 1.5 : lum.xxx * 0.35, 1.0);
        }
    }

    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u) gTiles[gid.y * gInfo.z + gid.x] = gsTileHasEdge;
}

#elif AVER_NEURAA_PASS == 2   // ---- training capture: reference accumulation ----

Texture2D<float4>   gColor : register(t0);
RWTexture2D<float4> gAccum : register(u0);   // RGBA16F running mean; gInfo.z = frames so far, gInfo.w = 1 restarts

[numthreads(8, 8, 1)]
void CSNeuRaaAccum(uint2 dtid : SV_DispatchThreadID) {
    uint w, h;
    gColor.GetDimensions(w, h);
    if (dtid.x >= w || dtid.y >= h) return;
    const float4 c = gColor.Load(int3(dtid, 0));
    gAccum[dtid] = gInfo.w != 0u ? c : gAccum[dtid] + (c - gAccum[dtid]) / float(max(gInfo.z, 1u));
}

#else   // AVER_NEURAA_PASS == 1 ---- baseline resolve ----

Texture2D<float>  gViewZ : register(t0);
Texture2D<float4> gColor : register(t1);
Texture2D<uint>   gEdges : register(t2);
Texture2D<float4> gDist  : register(t3);
StructuredBuffer<float> gNet : register(t4);   // the trained weights (gInfo.w = 1 uses them)
RWTexture2D<float4> gOut : register(u0);   // scene colour with edges resolved

// ---- the network (docs/rendering/NEURAA_NRD.md section 3; trained offline on oracle weights) ----
// gNet holds the weights file body: input mean[36], std[36], W1[32x36], b1, W2[32x32], b2, W3[9x32], b3.
// The 36 features must match the training script's order exactly.
#define NRAA_IN 36
#define NRAA_H1 32
#define NRAA_H2 32
#define NRAA_OUT 9
static const uint kNetMean = 0, kNetStd = NRAA_IN, kNetW1 = 2 * NRAA_IN;
static const uint kNetB1 = kNetW1 + NRAA_H1 * NRAA_IN, kNetW2 = kNetB1 + NRAA_H1;
static const uint kNetB2 = kNetW2 + NRAA_H2 * NRAA_H1, kNetW3 = kNetB2 + NRAA_H2, kNetB3 = kNetW3 + NRAA_OUT * NRAA_H2;
static const float kNetDeltaMax = 4.0;
static const int2 kNb9[9] = {int2(-1, -1), int2(0, -1), int2(1, -1), int2(-1, 0), int2(0, 0), int2(1, 0),
                             int2(-1, 1), int2(0, 1), int2(1, 1)};
static const uint kDir9[4] = {3u, 5u, 1u, 7u};

float nraaLum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// Blend weights over the 3x3 (index 4 is the pixel itself) from the baseline weights `wb`.
void nraaNetwork(int2 p, uint code, float4 dp, float zp, float wb[5], out float w9[9]) {
    float f[NRAA_IN];
    [unroll] for (uint d = 0; d < 4u; ++d) f[d] = dp[d];
    [unroll] for (uint d2 = 0; d2 < 4u; ++d2) {
        const int2 q = p + kDir[d2];
        const uint od = d2 ^ 1u;
        f[4 + d2]  = gDist.Load(int3(q, 0))[od];
        f[8 + d2]  = float((code >> d2) & 1u);
        f[15 + d2] = float((gEdges.Load(int3(q, 0)) >> od) & 1u);
        f[19 + d2] = clamp(log(max(gViewZ.Load(int3(q, 0)), 1.0e-3) / max(zp, 1.0e-3)), -2.0, 2.0);
    }
    const uint cls = code >> 4;
    f[12] = cls == 1u ? 1.0 : 0.0; f[13] = cls == 2u ? 1.0 : 0.0; f[14] = cls == 3u ? 1.0 : 0.0;
    float3 c9[9];
    [unroll] for (uint i = 0; i < 9u; ++i) c9[i] = gColor.Load(int3(p + kNb9[i], 0)).rgb;
    const float lp = nraaLum(c9[4]) + 1.0e-3;
    uint k = 23u;
    [unroll] for (uint j = 0; j < 9u; ++j) {
        if (j == 4u) continue;
        f[k++] = clamp(log((nraaLum(c9[j]) + 1.0e-3) / lp), -3.0, 3.0);
    }
    [unroll] for (uint b = 0; b < 5u; ++b) f[31 + b] = wb[b];

    [unroll] for (uint n = 0; n < NRAA_IN; ++n) f[n] = (f[n] - gNet[kNetMean + n]) / gNet[kNetStd + n];
    float h1[NRAA_H1];
    for (uint a = 0; a < NRAA_H1; ++a) {
        float v = gNet[kNetB1 + a];
        for (uint n2 = 0; n2 < NRAA_IN; ++n2) v += gNet[kNetW1 + a * NRAA_IN + n2] * f[n2];
        h1[a] = max(v, 0.0);
    }
    float h2[NRAA_H2];
    for (uint a2 = 0; a2 < NRAA_H2; ++a2) {
        float v = gNet[kNetB2 + a2];
        for (uint n3 = 0; n3 < NRAA_H1; ++n3) v += gNet[kNetW2 + a2 * NRAA_H1 + n3] * h1[n3];
        h2[a2] = max(v, 0.0);
    }
    float base9[9] = {0.0, 0.0, 0.0, 0.0, wb[0], 0.0, 0.0, 0.0, 0.0};
    [unroll] for (uint d3 = 0; d3 < 4u; ++d3) base9[kDir9[d3]] = wb[d3 + 1];
    float l[9];
    float lmax = -1.0e30;
    [unroll] for (uint o = 0; o < NRAA_OUT; ++o) {
        float v = gNet[kNetB3 + o];
        for (uint n4 = 0; n4 < NRAA_H2; ++n4) v += gNet[kNetW3 + o * NRAA_H2 + n4] * h2[n4];
        l[o] = log(base9[o] + 1.0e-3) + kNetDeltaMax * tanh(v);
        lmax = max(lmax, l[o]);
    }
    float sum = 0.0;
    [unroll] for (uint o2 = 0; o2 < 9u; ++o2) { w9[o2] = exp(l[o2] - lmax); sum += w9[o2]; }
    [unroll] for (uint o3 = 0; o3 < 9u; ++o3) w9[o3] /= sum;
}

// Only the nearer surface's triangle edge marks where the boundary is (GBAA): its pixel blends the
// uncovered part toward the far side, and the far pixel takes the part it overlaps.
[numthreads(8, 8, 1)]
void CSNeuRaaResolve(uint2 dtid : SV_DispatchThreadID) {
    const int2 p = int2(dtid) + int2(gOrg.xy);
    uint w, h;
    gColor.GetDimensions(w, h);
    if (p.x >= int(w) || p.y >= int(h)) return;
    const float4 c = gColor.Load(int3(p, 0));
    const uint code = inViewport(p) ? gEdges.Load(int3(p, 0)) : 0u;
    if ((code & 0xFu) == 0u) { gOut[p] = c; return; }

    const float zp = gViewZ.Load(int3(p, 0));
    const float4 dp = gDist.Load(int3(p, 0));
    float wsum = 0.0;
    float3 acc = 0.0;
    float wd4[4] = {0.0, 0.0, 0.0, 0.0};
    [unroll] for (uint d = 0; d < 4u; ++d) {
        if ((code & (1u << d)) == 0u) continue;
        const int2 q = p + kDir[d];
        const uint od = d ^ 1u;
        float wd;
        if (zp <= gViewZ.Load(int3(q, 0))) {
            wd = max(0.0, 0.5 - dp[d]);
        } else {
            const bool qSees = (gEdges.Load(int3(q, 0)) & (1u << od)) != 0u;
            wd = qSees ? max(0.0, gDist.Load(int3(q, 0))[od] - 0.5) : 0.0;
        }
        wd4[d] = wd;
        wsum += wd;
        acc += wd * gColor.Load(int3(q, 0)).rgb;
    }
    // Two edges at a corner can each claim half a pixel; keep at least a quarter of the pixel's own.
    const float k = wsum > 0.75 ? 0.75 / wsum : 1.0;
    if (gInfo.w == 0u) {
        gOut[p] = float4(c.rgb * (1.0 - wsum * k) + acc * k, c.a);
        return;
    }
    // The network adjusts the baseline's weights (self, left, right, up, down) over the whole 3x3.
    float wb[5];
    wb[0] = 1.0 - wsum * k;
    [unroll] for (uint d2 = 0; d2 < 4u; ++d2) wb[1 + d2] = wd4[d2] * k;
    float w9[9];
    nraaNetwork(p, code, dp, zp, wb, w9);
    float3 outC = 0.0;
    [unroll] for (uint i = 0; i < 9u; ++i) outC += w9[i] * gColor.Load(int3(p + kNb9[i], 0)).rgb;
    gOut[p] = float4(outC, c.a);
}

#endif
