#pragma once

// Aver.Render.UI's own HLSL, and deliberately NOT compiled against rhi::sharedShaderPrelude().
//
// The prelude declares the scene's cbuffer layouts, its vertex structures and the BRDF -- a UI
// shader reads none of it and could not: VSIn there is the 32-byte MeshVertex, and naming b0's
// PerFrame block here would put gViewProj in a root signature that has no camera in it. This pass
// stands where the post chain stands, downstream of everything the prelude describes, so it is
// self-contained for the same reason postShaderSource() is.
namespace aver::render::ui {

inline constexpr const char* kUiHLSL = R"(
// Pixels to clip space, and the one thing this shader knows about the screen. Held in b3 because
// b0/b1/b2 and b4 are reserved by RHIResources.hpp for the engine frame block, the per-draw object
// block, the per-draw binding's constants and a feature's own frame constants; b3 is the only slot
// a feature may claim outright.
cbuffer UiConstants : register(b3) {
    // xy = the scale that maps a pixel onto NDC, zw = the translate that puts the ORIGIN TOP-LEFT.
    // y is negative: NDC runs up the screen and a UI runs down it, and doing that flip here rather
    // than in the layout is what lets every widget coordinate stay a plain screen pixel.
    float4 gUiProj;
};

Texture2D    gUiTexture : register(t0);
SamplerState gUiSampler : register(s0);

struct UiVSIn {
    float2 pos : POSITION;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

struct UiVSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

UiVSOut UiVS(UiVSIn i) {
    UiVSOut o;
    o.pos = float4(i.pos.x * gUiProj.x + gUiProj.z, i.pos.y * gUiProj.y + gUiProj.w, 0.0, 1.0);
    o.uv  = i.uv;
    o.col = i.col;
    return o;
}

// BOTH inputs are PREMULTIPLIED, which is what makes this a plain product. The vertex colour is
// premultiplied on the CPU by uiPremultiply, and a texture bound here is required to be too --
// stated rather than detected, because a straight-alpha texture multiplied by a premultiplied
// colour produces a halo that looks like a filtering artefact and is not one.
//
// No sRGB conversion anywhere: the backbuffer is R8G8B8A8_UNORM and the composite that precedes
// this pass has already gamma-encoded the scene into it, so a UI colour authored as an sRGB byte
// arrives on screen as that byte. The blend is therefore in display space, which is technically
// the wrong space for compositing light and exactly the space every designer picking a 50% alpha
// expects it to be in.
float4 UiPS(UiVSOut i) : SV_Target {
    return i.col * gUiTexture.Sample(gUiSampler, i.uv);
}
)";

} // namespace aver::render::ui
