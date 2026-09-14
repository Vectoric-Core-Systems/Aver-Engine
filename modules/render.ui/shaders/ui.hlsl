// Aver.Render.UI's own HLSL: the editor's 2D draw-list pass.
//
// Moved out of a C++ raw-string literal. It is compiled the same way it always was -- see the call
// site for which prelude it is composed with -- but it is now a file the shader watcher can see,
// scripts/check-code-unchanged.py can normalise, and a payload can ship.
//
// SELF-CONTAINED, and deliberately NOT compiled against rhi::sharedShaderPrelude() -- this pass
// stands where the post chain stands.

// b0/b1/b2/b4 are reserved by RHIResources.hpp; b3 is the only slot a feature may claim.
cbuffer UiConstants : register(b3) {
    // xy = pixel-to-NDC scale, zw = the translate that puts the ORIGIN TOP-LEFT. y is negative.
    float4 gUiProj;
};

Texture2D    gUiTexture : register(t0);
SamplerState gUiSampler : register(s0);

// One UI vertex as the input assembler reads it.
struct UiVSIn {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

// One UI vertex after projection.
struct UiVSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

// Projects a screen-pixel position into clip space.
UiVSOut UiVS(UiVSIn i) {
    UiVSOut o;
    o.pos = float4(i.pos.x * gUiProj.x + gUiProj.z, i.pos.y * gUiProj.y + gUiProj.w, 0.0, 1.0);
    o.uv  = i.uv;
    o.col = i.col;
    return o;
}

// Modulates the texture by the vertex colour. BOTH are required to be PREMULTIPLIED.
// No sRGB conversion anywhere: the backbuffer is R8G8B8A8_UNORM and this blend is in display space.
float4 UiPS(UiVSOut i) : SV_Target {
    return i.col * gUiTexture.Sample(gUiSampler, i.uv);
}
