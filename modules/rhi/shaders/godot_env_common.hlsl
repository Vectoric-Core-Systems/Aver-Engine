// Fabric G6 SH3: what Godot sky, fog and particles shaders share (docs/plans/fabric/G6_SHADERS.md, section SH3).
//
// Composed by aver::rhi::godot*Program (GodotEnvPrograms.cpp) in front of the hook file of each shader type, ONLY when a converted shader of that type is
// registered. The Godot helper library that follows (material_godot_lib.hlsl, renamed gd_* -> gde_*, Gd* -> GdE*, gdSlot* -> gdeSlot*) is a private copy so this code
// can share a translation unit with a converted spatial shader (material_godot.hlsl owns the unprefixed names). Nothing here is compiled otherwise.

#define AVER_GDE_SLOT_SAMPLERS 1
#define AVER_GDE_NO_DERIVATIVES 1

// The basis change (aver::fmt::axis, AxisMap.hpp): engine = (-g.z, g.x, g.y) for a direction. Positions also scale by 0.01 (cm -> m).
float3 gdeE2G(float3 e) { return float3(e.y, e.z, -e.x); }
float3 gdeG2E(float3 g) { return float3(-g.z, g.x, g.y); }

// Sampler uniforms are unbound here: every access reads the colour the hint names (mode bits 8-9: 0 white, 1 black, 2 transparent, 3 flat normal).
float4 gdeSlotDefault(uint mode) {
    const uint c = (mode >> 8) & 3u;
    return c == 1u ? float4(0.0, 0.0, 0.0, 1.0) : (c == 2u ? float4(0.0, 0.0, 0.0, 0.0) : (c == 3u ? float4(0.5, 0.5, 1.0, 1.0) : float4(1.0, 1.0, 1.0, 1.0)));
}
#define GDE_UNBOUND_SLOTS(PRE) \
    int2   PRE##Size(uint slot, int lod) { return int2(1, 1); } \
    int    PRE##Levels(uint slot) { return 1; } \
    float4 PRE##Sample(uint slot, uint mode, float2 uv, float bias) { return gdeSlotDefault(mode); } \
    float4 PRE##SampleLod(uint slot, uint mode, float2 uv, float lod) { return gdeSlotDefault(mode); } \
    float4 PRE##SampleGrad(uint slot, uint mode, float2 uv, float2 dx, float2 dy) { return gdeSlotDefault(mode); } \
    float2 PRE##QueryLod(uint slot, float2 uv) { return float2(0.0, 0.0); } \
    float4 PRE##Fetch(uint slot, int2 p, int lod) { return gdeSlotDefault(0u); } \
    float4 PRE##Gather(uint slot, float2 uv, int comp) { return gdeSlotDefault(0u); }
GDE_UNBOUND_SLOTS(gdeSlot)
GDE_UNBOUND_SLOTS(gderSlot)

// Derivatives read zero (a compute or ray-miss evaluation has no screen-space neighbours); the macros are lifted again at the end of the program.
#define GDE_DERIV_FN(NAME) \
    float  NAME(float x)  { return 0.0; } \
    float2 NAME(float2 x) { return float2(0.0, 0.0); } \
    float3 NAME(float3 x) { return float3(0.0, 0.0, 0.0); } \
    float4 NAME(float4 x) { return float4(0.0, 0.0, 0.0, 0.0); }
GDE_DERIV_FN(gdeDdx)
GDE_DERIV_FN(gdeDdy)
GDE_DERIV_FN(gdeDdxCoarse)
GDE_DERIV_FN(gdeDdyCoarse)
GDE_DERIV_FN(gdeDdxFine)
GDE_DERIV_FN(gdeDdyFine)
GDE_DERIV_FN(gdeFwidth)
#define ddx gdeDdx
#define ddy gdeDdy
#define ddx_coarse gdeDdxCoarse
#define ddy_coarse gdeDdyCoarse
#define ddx_fine gdeDdxFine
#define ddy_fine gdeDdyFine
#define fwidth gdeFwidth
