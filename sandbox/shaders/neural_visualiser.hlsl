// The editor's NeuraFI visualisation overlay (Window > Neural Visualiser; NeuraFiVizFeature in
// SandboxApp.hpp). Draws NeuraFI's visualisation image -- already display-ready colours, written by
// neurafi.hlsl's gather -- over the 3D viewport, after the tonemap, alpha-blended at the chosen opacity.
//
// gVizParams.x: opacity (0..1). yzw unused.
cbuffer NeuralVizCB : register(b1) {
    float4 gVizParams;
};

Texture2D    gVizTex  : register(t0);
SamplerState gVizSamp : register(s0);

struct NeuralVizVSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// One fullscreen triangle from SV_VertexID (the same trick as gbuffer_debug.hlsl).
NeuralVizVSOut VSNeuralViz(uint id : SV_VertexID) {
    NeuralVizVSOut o;
    const float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSNeuralViz(NeuralVizVSOut i) : SV_TARGET {
    const float4 c = gVizTex.Sample(gVizSamp, i.uv);
    return float4(saturate(c.rgb), saturate(c.a * gVizParams.x));
}
