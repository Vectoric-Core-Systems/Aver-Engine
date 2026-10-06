// NRD2 phase 4 (docs/rendering/NRD2.md): the network's data paths around Aver.Render.Neural's ConvNet.
// One file, one pass per compile (AVER_NRD2_NET_PASS):
//   0 CSNrd2Gather  -- one training/validation record from a GPU pose slot (CPU twin nrd2ExtractPatch)
//   1 CSNrd2NetOut  -- the network's standardised tile parameters -> the resolve's parameter buffer
//                      (y * outScale + outBias, clamped, non-finite -> defaults)
// The input standardisation (x * inScale + inBias) is folded into nrd2.hlsl's CSNrd2Features on the live path.
// Parameter space only: nothing here runs the resolve on the network's output (patent rule 4).
// Portable: fp32 (fp16 only as packed storage), no wave intrinsics, no atomics, no groupshared, 64-thread
// groups, constants at b3, explicit bounds checks.

#ifndef AVER_NRD2_NET_PASS
#define AVER_NRD2_NET_PASS 0
#endif

#define NRD2_NET_CH 12

float nrd2NetPick(float4 v[3], uint c) { return v[c >> 2][c & 3u]; }
bool  nrd2NetFinite(float v) { return v == v && abs(v) < 3.0e38; }

#if AVER_NRD2_NET_PASS == 0   // ---- gather one record ----

#define NRD2_PATCH 56u   // texels (14 tiles)
#define NRD2_PTILES 14u
#define NRD2_CORE0 3u
#define NRD2_CORE1 11u

cbuffer Nrd2GatherCB : register(b3) {
    uint4  gGeo;        // halfW, halfH, tilesX, tilesY of the pose in this slot
    uint4  gBase;       // first half of the frame's features, theta base (uint), weight base (uint), record
    int4   gOrigin;     // patch top-left tile x, y
    float4 gInScale[3];
    float4 gInBias[3];
    float4 gTScale[3];  // target standardisation: theta * scale + bias
    float4 gTBias[3];
};

// Slot: [features fp16 pairs][theta f32 bits 12 x tiles][weights f32 bits 2 x tiles].
StructuredBuffer<uint>    gSlot   : register(t0);
RWStructuredBuffer<float> gIn     : register(u0);   // [record][12][56][56]
RWStructuredBuffer<float> gTarget : register(u1);   // [record][12][14][14]
RWStructuredBuffer<float> gWeight : register(u2);   // [record][12][14][14]

float nrd2Half(uint idx) {
    const uint w = gSlot[idx >> 1];
    return f16tof32((idx & 1u) != 0u ? (w >> 16) : (w & 0xFFFFu));
}

[numthreads(64, 1, 1)]
void CSNrd2Gather(uint3 dtid : SV_DispatchThreadID) {
    const uint t = dtid.x;
    const uint record = gBase.w;
    if (t < NRD2_PATCH * NRD2_PATCH) {
        const uint y = t / NRD2_PATCH, x = t - y * NRD2_PATCH;
        const int hx = gOrigin.x * 4 + (int)x, hy = gOrigin.y * 4 + (int)y;
        const bool inside = hx >= 0 && hy >= 0 && hx < (int)gGeo.x && hy < (int)gGeo.y;
        const uint plane = gGeo.x * gGeo.y;
        [unroll] for (uint c = 0u; c < NRD2_NET_CH; ++c) {
            float v = 0.0;
            if (inside) {
                const float f = nrd2Half(gBase.x + c * plane + (uint)hy * gGeo.x + (uint)hx);
                v = nrd2NetFinite(f) ? f * nrd2NetPick(gInScale, c) + nrd2NetPick(gInBias, c) : 0.0;
            }
            gIn[((record * NRD2_NET_CH + c) * NRD2_PATCH + y) * NRD2_PATCH + x] = v;
        }
    }
    if (t < NRD2_PTILES * NRD2_PTILES) {
        const uint ty = t / NRD2_PTILES, tx = t - ty * NRD2_PTILES;
        const int gx = gOrigin.x + (int)tx, gy = gOrigin.y + (int)ty;
        const bool inside = gx >= 0 && gy >= 0 && gx < (int)gGeo.z && gy < (int)gGeo.w;
        const bool core = tx >= NRD2_CORE0 && tx < NRD2_CORE1 && ty >= NRD2_CORE0 && ty < NRD2_CORE1;
        const uint tiles = gGeo.z * gGeo.w;
        const uint tile = inside ? (uint)gy * gGeo.z + (uint)gx : 0u;
        [unroll] for (uint p = 0u; p < NRD2_NET_CH; ++p) {
            float target = 0.0, weight = 0.0;
            if (inside) {
                const float th = asfloat(gSlot[gBase.y + p * tiles + tile]);
                if (nrd2NetFinite(th)) {
                    target = th * nrd2NetPick(gTScale, p) + nrd2NetPick(gTBias, p);
                    const float w = asfloat(gSlot[gBase.z + (p / 6u) * tiles + tile]);
                    weight = (core && nrd2NetFinite(w) && w > 0.0) ? w : 0.0;
                }
            }
            const uint o = ((record * NRD2_NET_CH + p) * NRD2_PTILES + ty) * NRD2_PTILES + tx;
            gTarget[o] = target;
            gWeight[o] = weight;
        }
    }
}

#else   // ---- inference: the network's output -> tile parameters ----

cbuffer Nrd2NetCB : register(b3) {
    uint4  gNetTiles;   // x: tiles
    float4 gNetScale[3];
    float4 gNetBias[3];
    float4 gNetDef[3];  // default parameters
};

StructuredBuffer<float>   gNetOut : register(t0);   // [12][tilesY][tilesX], standardised
RWStructuredBuffer<float> gParams : register(u0);   // the resolve's buffer, plane-major

[numthreads(64, 1, 1)]
void CSNrd2NetOut(uint3 dtid : SV_DispatchThreadID) {
    const uint tiles = gNetTiles.x;
    if (dtid.x >= tiles) return;
    [unroll] for (uint p = 0u; p < NRD2_NET_CH; ++p) {
        const float y = gNetOut[p * tiles + dtid.x];
        const float v = y * nrd2NetPick(gNetScale, p) + nrd2NetPick(gNetBias, p);
        const float lim = (p % 6u) < 3u ? 8.0 : 6.0;   // logits +-8, log2 sensitivities +-6
        gParams[p * tiles + dtid.x] = nrd2NetFinite(v) ? clamp(v, -lim, lim) : nrd2NetPick(gNetDef, p);
    }
}

#endif
