// The editor's 3D-viewport icons: a textured, camera-facing quad drawn INSIDE the scene.
//
// WHAT MAKES THIS DIFFERENT FROM AN ImGui OVERLAY, which is the obvious cheaper way to put a picture
// at a world position and is what this deliberately is not: an overlay is pasted over the finished
// image, so it shows through walls. These quads are drawn in transparentPass, against the scene's
// real depth buffer, so a Player Start behind a crate is behind the crate.
//
// The quad's four corners arrive ALREADY IN WORLD SPACE -- the CPU bakes the billboard from the
// camera basis (ViewportIconRenderer.cpp's cameraBasis) rather than the vertex shader doing it, the
// same division of labour the particle renderer settled on.

struct IconVSIn {
    // Split across two POSITION slots rather than one float3, matching ParticleVertex for the same
    // reason: this RHI's vertex Format enum has no three-component float format (RG32Float is its
    // widest), so a caller-owned layout cannot declare one.
    float2 posXY : POSITION0;
    float  posZ  : POSITION1;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;   // STRAIGHT, not premultiplied -- see IconPS
};

struct IconVSOut {
    float4 pos   : SV_Position;
    float2 uv    : TEXCOORD0;
    float4 color : COLOR0;
};

Texture2D    gIconTexture : register(t0);
SamplerState gIconSampler : register(s0);

IconVSOut IconVS(IconVSIn i) {
    IconVSOut o;
    o.pos = mul(float4(float3(i.posXY, i.posZ), 1.0), gViewProj);
    o.uv = i.uv;
    o.color = i.color;
    return o;
}

// THE INVERSE TONEMAP IS THE WHOLE POINT OF THIS SHADER, and leaving it out is the mistake that
// makes an icon look like a muddy sticker.
//
// transparentPass draws into the SCENE colour target, which is pre-tonemap linear radiance -- the
// camera's exposure, bloom, ACES curve and sRGB encode all still run over the top of whatever is
// written here. Writing the PNG's bytes straight in would hand a display-referred colour to a chain
// that assumes scene-referred radiance, and every icon would come out darker and less saturated than
// its own artwork, by exactly the amount the tonemap compresses.
//
// averInverseTonemap(srgbToLin(c)) is the radiance that tonemaps BACK to c -- the identical round
// trip PSLine (scene.hlsl) performs on an authored line colour, and for the identical reason. The
// icon then lands on screen as the colour the artist drew, not a shaded version of it.
//
// PREMULTIPLIED HERE, NOT ON THE CPU. The pipeline blends PremultipliedAlpha, but the vertex colour
// and the texture both carry STRAIGHT alpha, so the multiply has to happen after the inverse tonemap
// -- premultiplying first would push the transparent border's rgb toward black BEFORE the curve saw
// it, and the curve is not linear, so the edge would pick up a dark fringe.
float4 IconPS(IconVSOut i) : SV_Target {
    float4 tex = gIconTexture.Sample(gIconSampler, i.uv);
    float  a   = tex.a * i.color.a;
    float3 rgb = averInverseTonemap(srgbToLin(tex.rgb * i.color.rgb));
    return float4(rgb * a, a);
}
