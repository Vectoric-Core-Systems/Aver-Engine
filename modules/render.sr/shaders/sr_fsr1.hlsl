// AverSR: AMD FidelityFX Super Resolution 1 (third_party/fidelityfx-fsr, MIT) as an upscaler.
//
// Three fullscreen passes (FsrUpscaler::execute):
//   AverSrFsrPrepPS  source size: optional FXAA-class edge AA, then squash HDR into [0,1)
//   AverSrFsrEasuPS  output size: EASU, the edge-adaptive upscale
//   AverSrFsrRcasPS  output size: RCAS sharpening, then un-squash back to HDR
// FSR 1 expects display-range input; this chain runs before the tonemap, so it works on the
// reversible c / (1 + max(c)) and undoes it at the end.

#define A_GPU 1
#define A_HLSL 1
#include "FidelityFX/ffx_a.h"

cbuffer AverSrFsrCB : register(b1) {
    uint4  gCon0;      // EASU constants (FsrEasuCon); gCon0.x is RCAS's for the RCAS pass
    uint4  gCon1;
    uint4  gCon2;
    uint4  gCon3;
    float4 gSrcSize;   // prep pass: xy source size, zw reciprocal
};

Texture2D<float4> gSrc  : register(t0);
SamplerState      gSamp : register(s0);   // linear, clamp

struct AverSrFsrVSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

AverSrFsrVSOut AverSrFsrVS(uint id : SV_VertexID) {
    AverSrFsrVSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float3 averFsrSquash(float3 c)   { c = max(c, 0.0); return c / (1.0 + max(c.r, max(c.g, c.b))); }
float3 averFsrUnsquash(float3 t) { t = saturate(t); return t / max(1.0 - max(t.r, max(t.g, t.b)), 1.0 / 1024.0); }

// ---- EASU / RCAS callbacks ----
#define FSR_EASU_F 1
AF4 FsrEasuRF(AF2 p) { return gSrc.GatherRed(gSamp, p); }
AF4 FsrEasuGF(AF2 p) { return gSrc.GatherGreen(gSamp, p); }
AF4 FsrEasuBF(AF2 p) { return gSrc.GatherBlue(gSamp, p); }
#define FSR_RCAS_F 1
AF4 FsrRcasLoadF(ASU2 p) { return gSrc.Load(int3(p, 0)); }
void FsrRcasInputF(inout AF1 r, inout AF1 g, inout AF1 b) {}
#include "FidelityFX/ffx_fsr1.h"

// ---- edge AA (FXAA-class), at source resolution before the upscale ----
float averFsrLuma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float3 averFsrEdgeAa(float2 uv) {
    const float2 px = gSrcSize.zw;
    float3 m  = averFsrSquash(gSrc.SampleLevel(gSamp, uv, 0).rgb);
    float3 n  = averFsrSquash(gSrc.SampleLevel(gSamp, uv + float2(0.0, -px.y), 0).rgb);
    float3 s  = averFsrSquash(gSrc.SampleLevel(gSamp, uv + float2(0.0,  px.y), 0).rgb);
    float3 e  = averFsrSquash(gSrc.SampleLevel(gSamp, uv + float2( px.x, 0.0), 0).rgb);
    float3 w  = averFsrSquash(gSrc.SampleLevel(gSamp, uv + float2(-px.x, 0.0), 0).rgb);
    float3 nw = averFsrSquash(gSrc.SampleLevel(gSamp, uv + float2(-px.x, -px.y), 0).rgb);
    float3 ne = averFsrSquash(gSrc.SampleLevel(gSamp, uv + float2( px.x, -px.y), 0).rgb);
    float3 sw = averFsrSquash(gSrc.SampleLevel(gSamp, uv + float2(-px.x,  px.y), 0).rgb);
    float3 se = averFsrSquash(gSrc.SampleLevel(gSamp, uv + float2( px.x,  px.y), 0).rgb);

    float lM = averFsrLuma(m), lN = averFsrLuma(n), lS = averFsrLuma(s), lE = averFsrLuma(e), lW = averFsrLuma(w);
    float lNW = averFsrLuma(nw), lNE = averFsrLuma(ne), lSW = averFsrLuma(sw), lSE = averFsrLuma(se);
    float lMin = min(lM, min(min(lN, lS), min(lE, lW)));
    float lMax = max(lM, max(max(lN, lS), max(lE, lW)));
    float range = lMax - lMin;
    // FXAA 3.11's default thresholds; the squashed values are display-range, which they assume.
    if (range < max(0.0312, lMax * 0.125)) return m;

    float edgeVert = abs(lNW + lSW - 2.0 * lW) + 2.0 * abs(lN + lS - 2.0 * lM) + abs(lNE + lSE - 2.0 * lE);
    float edgeHorz = abs(lNW + lNE - 2.0 * lN) + 2.0 * abs(lW + lE - 2.0 * lM) + abs(lSW + lSE - 2.0 * lS);
    bool  horz = edgeHorz >= edgeVert;
    float g1 = abs((horz ? lN : lW) - lM), g2 = abs((horz ? lS : lE) - lM);
    bool  side1 = g1 >= g2;
    float3 dirC = side1 ? (horz ? n : w) : (horz ? s : e);
    float3 edge = lerp(m, dirC, saturate(max(g1, g2) / max(range, 1e-4)) * 0.5);
    float3 box  = (n + s + e + w + nw + ne + sw + se) * (1.0 / 8.0);
    return lerp(edge, box, saturate(abs(averFsrLuma(box) - lM) / max(range, 1e-4)) * 0.5);
}

float4 AverSrFsrPrepPS(AverSrFsrVSOut i) : SV_TARGET {
#if AVER_FSR_EDGE_AA
    return float4(averFsrEdgeAa(i.uv), 1.0);
#else
    return float4(averFsrSquash(gSrc.SampleLevel(gSamp, i.uv, 0).rgb), 1.0);
#endif
}

float4 AverSrFsrEasuPS(AverSrFsrVSOut i) : SV_TARGET {
    AF3 c;
    FsrEasuF(c, AU2(i.pos.xy), gCon0, gCon1, gCon2, gCon3);
    return float4(c, 1.0);
}

float4 AverSrFsrRcasPS(AverSrFsrVSOut i) : SV_TARGET {
    AF3 c;
    FsrRcasF(c.r, c.g, c.b, AU2(i.pos.xy), gCon0);
    return float4(averFsrUnsquash(c), 1.0);
}
