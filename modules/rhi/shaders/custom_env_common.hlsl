// What custom sky, fog and particles programs share.
//
// Composed by aver::rhi::custom*Program (CustomEnvPrograms.cpp) in front of the hook file of each program type, ONLY when a program of that type is
// registered. The helper library that follows (material_embedded_lib.hlsl, renamed emb_* -> cenv_*, Emb* -> CEnv*, embSlot* -> cenvSlot*) is a private copy so this code
// can share a translation unit with an embedded material shader (material_embedded.hlsl owns the unprefixed names). Nothing here is compiled otherwise.

#define AVER_CENV_SLOT_SAMPLERS 1
#define AVER_CENV_NO_DERIVATIVES 1

// The basis change (aver::fmt::axis, AxisMap.hpp): engine = (-s.z, s.x, s.y) for a direction. Positions also scale by 0.01 (cm -> m).
float3 cenvE2S(float3 e) { return float3(e.y, e.z, -e.x); }
float3 cenvS2E(float3 s) { return float3(-s.z, s.x, s.y); }

// Sampler uniforms are unbound here: every access reads the colour the hint names (mode bits 8-9: 0 white, 1 black, 2 transparent, 3 flat normal).
float4 cenvSlotDefault(uint mode) {
    const uint c = (mode >> 8) & 3u;
    return c == 1u ? float4(0.0, 0.0, 0.0, 1.0) : (c == 2u ? float4(0.0, 0.0, 0.0, 0.0) : (c == 3u ? float4(0.5, 0.5, 1.0, 1.0) : float4(1.0, 1.0, 1.0, 1.0)));
}
#define CENV_UNBOUND_SLOTS(PRE) \
    int2   PRE##Size(uint slot, int lod) { return int2(1, 1); } \
    int    PRE##Levels(uint slot) { return 1; } \
    float4 PRE##Sample(uint slot, uint mode, float2 uv, float bias) { return cenvSlotDefault(mode); } \
    float4 PRE##SampleLod(uint slot, uint mode, float2 uv, float lod) { return cenvSlotDefault(mode); } \
    float4 PRE##SampleGrad(uint slot, uint mode, float2 uv, float2 dx, float2 dy) { return cenvSlotDefault(mode); } \
    float2 PRE##QueryLod(uint slot, float2 uv) { return float2(0.0, 0.0); } \
    float4 PRE##Fetch(uint slot, int2 p, int lod) { return cenvSlotDefault(0u); } \
    float4 PRE##Gather(uint slot, float2 uv, int comp) { return cenvSlotDefault(0u); }
CENV_UNBOUND_SLOTS(cenvSlot)
CENV_UNBOUND_SLOTS(cenvrSlot)

// Derivatives read zero (a compute or ray-miss evaluation has no screen-space neighbours); the macros are lifted again at the end of the program.
#define CENV_DERIV_FN(NAME) \
    float  NAME(float x)  { return 0.0; } \
    float2 NAME(float2 x) { return float2(0.0, 0.0); } \
    float3 NAME(float3 x) { return float3(0.0, 0.0, 0.0); } \
    float4 NAME(float4 x) { return float4(0.0, 0.0, 0.0, 0.0); }
CENV_DERIV_FN(cenvDdx)
CENV_DERIV_FN(cenvDdy)
CENV_DERIV_FN(cenvDdxCoarse)
CENV_DERIV_FN(cenvDdyCoarse)
CENV_DERIV_FN(cenvDdxFine)
CENV_DERIV_FN(cenvDdyFine)
CENV_DERIV_FN(cenvFwidth)
#define ddx cenvDdx
#define ddy cenvDdy
#define ddx_coarse cenvDdxCoarse
#define ddy_coarse cenvDdyCoarse
#define ddx_fine cenvDdxFine
#define ddy_fine cenvDdyFine
#define fwidth cenvFwidth
