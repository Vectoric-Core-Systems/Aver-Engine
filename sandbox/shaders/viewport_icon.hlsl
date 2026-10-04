// The editor's 3D-viewport icons: a textured, camera-facing quad drawn as EDITOR CHROME, in
// overlayPass, after the camera post chain -- see aver/rhi/EditorLines.hpp's header comment for why
// editor chrome moved there (bloom/auto-exposure/tonemap made every glow-free colour glow anyway).
//
// WHAT MAKES THIS DIFFERENT FROM A PLAIN ImGui OVERLAY, which is the obvious cheaper way to put a
// picture at a world position and is what this deliberately is not: a plain overlay has no notion of
// the scene's geometry, so it would show through walls. overlayPass keeps the scene depth
// (IDevice::sceneDepthTexture()) readable as an ordinary shader resource, so IconPS below runs a
// manual depth test against it -- the same DISTANCE-FROM-EYE comparison PSEditorLine performs
// (editor_lines.hlsl), for the same reason: the display target and the (possibly render-scaled)
// scene depth are different sizes, so there is no hardware depth attachment to test against here.
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

// Table 1: the scene depth ViewportIconRenderer's depthSet_ ring binds, exactly the same
// MS/non-MS split as editor_lines.hlsl's AVER_EDITOR_LINE_MS (this pass's own AVER_ICON_DEPTH_MS
// picks it, compiled in by ensurePipeline).
#if AVER_ICON_DEPTH_MS
Texture2DMS<float> gIconSceneDepth : register(t1);
float iconSceneDepth(int2 px) { return gIconSceneDepth.Load(px, 0); }
float2 iconSceneDepthSize() { uint w, h, n; gIconSceneDepth.GetDimensions(w, h, n); return float2(w, h); }
#else
Texture2D<float> gIconSceneDepth : register(t1);
float iconSceneDepth(int2 px) { return gIconSceneDepth.Load(int3(px, 0)); }
float2 iconSceneDepthSize() { uint w, h; gIconSceneDepth.GetDimensions(w, h); return float2(w, h); }
#endif

IconVSOut IconVS(IconVSIn i) {
    IconVSOut o;
    o.pos = mul(float4(float3(i.posXY, i.posZ), 1.0), gViewProjNoJitter);
    o.uv = i.uv;
    o.color = i.color;
    return o;
}

// A camera-relative point on this pixel's ray at NDC depth z -- copied from editor_lines.hlsl's
// editorRelPoint (same maths, same reason: the two shaders check occlusion the same way).
float3 iconRelPoint(float2 ndc, float z) {
    const float4 p = mul(float4(ndc, z, 1.0), gInvViewProjRel);
    return p.xyz / p.w;
}

// DISPLAY colour, premultiplied, NO INVERSE TONEMAP. overlayPass writes straight to the display
// target after the post chain has already run, unlike the old transparentPass write into pre-tonemap
// HDR radiance -- so the artist's PNG bytes (tinted by the vertex colour) ARE the colour shown,
// exactly like editor_lines.hlsl's authored line colour needs no round trip either.
float4 IconPS(IconVSOut i) : SV_Target {
    if (gBaseColor.y > 0.5) {
        // PSEditorLine's own technique: the scene depth covers the (possibly render-scaled) whole
        // target, so a display pixel maps to it by one uniform scale; the NDC of the pixel comes from
        // the 3D view's rect inside the display target.
        const float2 depthSize = iconSceneDepthSize();
        const float2 scale = depthSize / max(gEmissive.xy, float2(1.0, 1.0));
        const int2 px = int2(min(i.pos.xy * scale, depthSize - 1.0));
        const float zs = iconSceneDepth(px);
        if (zs < 1.0) {   // 1.0 is the clear value: nothing there, the icon shows against the sky
            const float2 uv = (i.pos.xy - gMaterial.zw) / max(gBaseColor.zw, float2(1.0, 1.0));
            const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
            const float dScene = length(iconRelPoint(ndc, zs));
            const float dIcon  = length(iconRelPoint(ndc, i.pos.z));
            // 0.2% + 1 cm behind the surface still counts as visible, matching PSEditorLine's own
            // tolerance (a billboard sitting exactly at a marker's position, on its own floor).
            if (dIcon > dScene * 1.002 + 1.0) discard;
        }
    }
    float4 tex = gIconTexture.Sample(gIconSampler, i.uv);
    float  a   = tex.a * i.color.a;
    const float3 straight = tex.rgb * i.color.rgb;
    float3 rgb = gMaterial.x > 0.5 ? srgbToLin(straight) : straight;
    return float4(rgb * a, a);
}
