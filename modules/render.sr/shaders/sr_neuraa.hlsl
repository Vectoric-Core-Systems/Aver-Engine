// NeuRAA edge detection (docs/rendering/NEURAA_NRD.md section 3). One 8x8 group per tile of the
// scene viewport: an edge code per pixel, a flag per tile, and in the debug view an edge-class image.
//
// Edge code: bits 0-3 = an edge toward the left, right, up, down neighbour; bits 4-5 = class,
// 1 silhouette (another object or the sky), 2 depth step on one object, 3 crease (normal turn).

cbuffer AverNeuRaaCB : register(b1) {
    uint4 gVp;     // scene viewport x, y, w, h: the pixels the visibility buffer holds this frame
    uint4 gInfo;   // visibility row pitch, debug view flag, tiles per row, unused
};

Texture2D<float>  gViewZ  : register(t0);   // G-buffer linear view depth (sky 1e7)
Texture2D<float4> gNormal : register(t1);   // G-buffer octahedral normal + roughness
Texture2D<float4> gColor  : register(t2);   // scene colour, read by the debug view only

RWStructuredBuffer<uint4> gVis   : register(u0);   // (instance ref, triangle, bary.x, bary.y)
RWTexture2D<uint>         gEdges : register(u1);
RWTexture2D<float4>       gDebug : register(u2);
RWStructuredBuffer<uint>  gTiles : register(u3);

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

bool inViewport(int2 p) { return all(p >= int2(gVp.xy)) && all(p < int2(gVp.xy + gVp.zw)); }

struct Px { uint obj; float invZ; float3 n; };

Px loadPx(int2 p) {
    Px o;
    o.obj  = objectOf(gVis[p.y * gInfo.x + p.x].x);
    o.invZ = 1.0 / max(gViewZ.Load(int3(p, 0)), 1.0e-4);
    o.n    = decodeNormal(gNormal.Load(int3(p, 0)));
    return o;
}

// Class of the edge between a and its neighbour b; `o` is a's other neighbour on the same axis.
// 1/z is linear across a plane in screen space, so its second difference flags a step, not a slope.
uint edgeClass(Px a, Px b, Px o) {
    if (a.obj != b.obj) return 1u;
    if (a.obj == kRefMiss) return 0u;
    if (abs(b.invZ - 2.0 * a.invZ + o.invZ) > kDepthStep * a.invZ) return 2u;
    if (dot(a.n, b.n) < kCreaseCos) return 3u;
    return 0u;
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
        static const int2 kDir[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
        const Px c = loadPx(p);
        Px nb[4];
        bool have[4];
        [unroll] for (uint i = 0; i < 4u; ++i) {
            have[i] = inViewport(p + kDir[i]);
            nb[i] = c;
            if (have[i]) nb[i] = loadPx(p + kDir[i]);
        }
        uint code = 0u, cls = 0u;
        [unroll] for (uint d = 0; d < 4u; ++d) {
            if (!have[d]) continue;
            // The opposite neighbour, unless it is missing or another object: then a first difference.
            const uint od = d ^ 1u;
            Px o = c;
            if (have[od] && nb[od].obj == c.obj) o = nb[od];
            const uint k = edgeClass(c, nb[d], o);
            if (k != 0u) {
                code |= 1u << d;
                cls = (cls == 0u || k < cls) ? k : cls;   // silhouette over step over crease
            }
        }
        code |= cls << 4;
        gEdges[p] = code;
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
