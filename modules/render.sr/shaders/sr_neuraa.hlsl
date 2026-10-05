// NeuRAA (docs/rendering/NEURAA_NRD.md section 3), two passes selected by AVER_NEURAA_PASS:
//   0 CSNeuRaaDetect  -- per pixel: an edge code and, toward each edge, the distance to its own
//                        triangle's edge; per 8x8 tile a flag; the edge-class debug image.
//   1 CSNeuRaaResolve -- the baseline blend: each edge pixel mixes with the neighbour across the
//                        edge by the covered fraction (distance-to-edge AA, Malan 2010 / Persson 2011).
//
// Edge code: bits 0-3 = an edge toward the left, right, up, down neighbour; bits 4-5 = class,
// 1 silhouette (another object or the sky), 2 depth step on one object, 3 crease (normal turn).

#ifndef AVER_NEURAA_PASS
#define AVER_NEURAA_PASS 0
#endif

cbuffer AverNeuRaaCB : register(b1) {
    uint4 gVp;     // scene viewport x, y, w, h: the pixels the visibility buffer holds this frame
    uint4 gInfo;   // visibility row pitch, debug view flag, tiles per row, unused
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

static const uint  kRefMiss      = 0xFFFFFFFFu;
static const uint  kRefFoliage   = 0x80000000u;   // voxi_rt.hlsli's AVER_RT_REF_FOLIAGE
static const uint  kRefIndexMask = 0x07FFFFFFu;   // ... AVER_RT_REF_INDEX_MASK
static const float kDepthStep    = 0.03;          // 1/z second difference, relative
static const float kCreaseCos    = 0.866;         // ~30 degrees

// A foliage ref also carries its part (trunk, leaves) in bits 27-30; parts of one tree are one object.
uint objectOf(uint ref) {
    return (ref != kRefMiss && (ref & kRefFoliage) != 0u) ? (ref & (kRefFoliage | kRefIndexMask)) : ref;
}

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

#else   // AVER_NEURAA_PASS == 1 ---- baseline resolve ----

Texture2D<float>  gViewZ : register(t0);
Texture2D<float4> gColor : register(t1);
Texture2D<uint>   gEdges : register(t2);
Texture2D<float4> gDist  : register(t3);
RWTexture2D<float4> gOut : register(u0);   // scene colour with edges resolved

// Only the nearer surface's triangle edge marks where the boundary is (GBAA): its pixel blends the
// uncovered part toward the far side, and the far pixel takes the part it overlaps.
[numthreads(8, 8, 1)]
void CSNeuRaaResolve(uint2 dtid : SV_DispatchThreadID) {
    const int2 p = int2(dtid);
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
        wsum += wd;
        acc += wd * gColor.Load(int3(q, 0)).rgb;
    }
    // Two edges at a corner can each claim half a pixel; keep at least a quarter of the pixel's own.
    const float k = wsum > 0.75 ? 0.75 / wsum : 1.0;
    gOut[p] = float4(c.rgb * (1.0 - wsum * k) + acc * k, c.a);
}

#endif
