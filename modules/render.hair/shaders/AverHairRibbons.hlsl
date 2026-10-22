// AverHairRibbons.hlsl -- expands Jolt's render-strand positions into camera-facing ribbons (docs/plans/wave11/SOFTBODY_HAIR.md 2.16).
//
// One thread per ribbon point (a point of a render strand in RANK order, see RibbonPack.hpp). It reads the simulated position of the point and of the
// points before and after it, and writes two rhi::MeshVertex into the instance's skin-target vertex buffer: the left and right edge of the ribbon at
// that point. The draw is an ordinary indexed mesh (drawMesh), so shadows, the depth pass and the lighting see hair as geometry.
//
// SPACE. Jolt's render positions are in hair-local JOLT space (metres, Y up, right-handed): the engine's hair-local position is (-z, x, y) * 100
// centimetres (Convert.hpp fromJolt). The ribbon is written in that HAIR-LOCAL ENGINE space and the draw applies the entity's world matrix like any
// other mesh, so the camera arrives already inverse-transformed (gCamPix.xyz) and a pixel's footprint is distance * pixelAt1cm in the same local units.
//
// SUB-PIXEL STRANDS. A strand thinner than a pixel is drawn one pixel wide (W = max(w, pixel)) and carries the lost width as COVERAGE c = w / W < 1.
// Coverage is spent per STRAND, stochastically: the strand is kept this frame when a hash of (strand, frame) falls below c, else its ribbon collapses
// to a line of zero area. Temporal anti-aliasing integrates the frames, so the sum of what is kept equals the coverage; with TAA off the hair shows
// as a flicker at a distance (a known cost, docs/HAIR.md). It needs no extra vertex attribute and no alpha test, so the hair stays opaque and early-z.
// Strands whose rank is at or beyond gKeepRank are collapsed too (the density scale and the LOD prefix share this one rule).
//
// OUTPUT VERTEX: position = ribbon edge; normal slot = the unit strand TANGENT (root to tip) in local space; uv = (-1 or +1 across the ribbon,
// hash8 + fraction * 0.999). AVER_MODEL_HAIR in material_prelude.hlsl reads exactly that.
//
// MIRRORED by RibbonPack.hpp (ribbonVertices); tests/render.hair/RibbonPackTest.cpp pins the C++ twin, so change both.
struct RibbonPoint {
    uint  vtx;          // render vertex index of this point
    uint  prev;         // render vertex index of the point before it (itself at the root)
    uint  next;         // and after it (itself at the tip)
    float fraction;     // 0 root .. 1 tip along the strand
    float widthCm;      // the drawn width before scaling, centimetres
    uint  strandHash;   // a hash of the strand's identity; the low 8 bits colour the strand
    float rankFrac;     // the strand's rank / strand count
};

struct HairVertex {     // rhi::MeshVertex
    float3 pos;
    float3 nrm;
    float2 uv;
};

StructuredBuffer<RibbonPoint>   gPoints : register(t0);
StructuredBuffer<float3>        gRender : register(t1);   // Jolt render positions, 12 bytes each
RWStructuredBuffer<HairVertex>  gOut    : register(u0);

cbuffer RibbonCB : register(b3) {
    float4 gCamPix;       // xyz: camera in hair-local engine centimetres; w: pixel footprint at 1 cm distance (local cm per pixel per cm)
    uint   gPointCount;
    uint   gFrameSeed;
    float  gWidthScale;   // CHair::widthScale times the level's width boost
    float  gMinPixels;    // the narrowest a ribbon is drawn, in pixels
    float  gKeepRank;     // strands with rankFrac >= this collapse
    float  gPad0;
    float  gPad1;
    float  gPad2;
};

float3 joltToLocal(float3 j) { return float3(-j.z, j.x, j.y) * 100.0f; }

uint hairMix(uint x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float hairUnit(uint h) { return float(h >> 8) * (1.0f / 16777216.0f); }

[numthreads(64, 1, 1)]
void CSRibbons(uint3 tid : SV_DispatchThreadID) {
    const uint k = tid.x;
    if (k >= gPointCount) return;
    const RibbonPoint rp = gPoints[k];

    const float3 p = joltToLocal(gRender[rp.vtx]);
    const float3 a = joltToLocal(gRender[rp.prev]);
    const float3 b = joltToLocal(gRender[rp.next]);

    float3 T = b - a;
    const float tl = length(T);
    T = tl > 1.0e-6f ? T / tl : float3(0.0f, 0.0f, -1.0f);

    const float3 toCam = gCamPix.xyz - p;
    const float dist = max(length(toCam), 1.0e-3f);

    const float w = rp.widthCm * gWidthScale;
    const float wPx = dist * gCamPix.w * gMinPixels;
    const float W = max(w, wPx);
    const float cover = w / max(W, 1.0e-8f);

    bool dropped = rp.rankFrac >= gKeepRank;
    if (!dropped && cover < 1.0f) dropped = hairUnit(hairMix(rp.strandHash ^ (gFrameSeed * 0x9E3779B9u))) >= cover;

    // The side the ribbon spreads along: perpendicular to the strand and to the view. Looking straight along the strand falls back to a fixed axis.
    float3 S = cross(T, toCam);
    const float sl = length(S);
    if (sl > 1.0e-6f) {
        S /= sl;
    } else {
        const float3 axis = abs(T.z) < 0.9f ? float3(0.0f, 0.0f, 1.0f) : float3(1.0f, 0.0f, 0.0f);
        S = normalize(cross(T, axis));
    }

    const float hw = dropped ? 0.0f : 0.5f * W;
    const float v = float(rp.strandHash & 255u) + saturate(rp.fraction) * 0.999f;

    HairVertex l;
    HairVertex r;
    l.pos = p - S * hw;
    r.pos = p + S * hw;
    l.nrm = T;
    r.nrm = T;
    l.uv = float2(-1.0f, v);
    r.uv = float2(1.0f, v);
    gOut[2u * k] = l;
    gOut[2u * k + 1u] = r;
}
