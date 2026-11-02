// Aver.Render.UI's world-text shaders (parity E6): the glyph quads of a Label3D, drawn in the scene's transparent pass.
//
// PLAIN RASTER, ON PURPOSE. One texture sample per pixel, no ray query, no acceleration structure, no loop, no lighting, no
// reads of the scene's material or voxel resources. The vertices are already in the scene frame (the CPU placed them), so the
// vertex shader is one matrix multiply. Self-contained: not compiled against the scene prelude.

// b3 is the only slot a feature may claim.
cbuffer WorldTextConstants : register(b3) {
    float4x4 gPVP;       // the scene camera's view-projection (row-vector convention, as the particle pass)
    // x alpha cut (0 = none), y intensity (colour multiplier), z 1 when the page holds distance fields, w edge softness (0 = from the derivative)
    float4 gParams;
};

Texture2D    gGlyphPage : register(t0);
SamplerState gGlyphSampler : register(s0);

struct WorldVSIn {
    float2 posXY : POSITION0;
    float  posZ  : POSITION1;
    float2 uv    : TEXCOORD0;
    float4 col   : COLOR0;       // premultiplied
};

struct WorldVSOut {
    float4 pos : SV_Position;
    float2 uv  : TEXCOORD0;
    float4 col : COLOR0;
};

WorldVSOut WorldTextVS(WorldVSIn i) {
    WorldVSOut o;
    o.pos = mul(float4(i.posXY, i.posZ, 1.0), gPVP);
    o.uv = i.uv;
    o.col = i.col;
    return o;
}

// The page holds premultiplied white coverage (c, c, c, c), or a distance field in the same channels (128 on the edge, rising
// inside). A distance field is turned into coverage with a smoothstep around the edge, as wide as one pixel of the screen
// unless the label asks for a fixed softness.
float4 WorldTextPS(WorldVSOut i) : SV_Target {
    float c = gGlyphPage.Sample(gGlyphSampler, i.uv).a;
    if (gParams.z > 0.5) {
        float w = gParams.w > 0.0 ? gParams.w : max(fwidth(c) * 0.75, 0.02);
        c = smoothstep(0.5 - w, 0.5 + w, c);
    }
    float4 s = i.col * c;
    if (gParams.x > 0.0 && s.a < gParams.x) discard;
    s.rgb *= gParams.y;
    return s;
}
