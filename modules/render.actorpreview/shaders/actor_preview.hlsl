// The asset preview's own camera, vertex shader and PreviewPS.
//
// Compiled as the tail of rhi::sharedShaderPrelude(). Moved out of a C++ raw-string literal.

// The preview's own camera. Must be b4, the feature register: the backend rebinds b0 per setPipeline.
cbuffer PreviewFrame : register(b4) {
    float4x4 gPreviewViewProj;
    float4   gPreviewEye;      // xyz = eye, w = unused
    float4   gPreviewKey;      // xyz = direction TO the key light, w = its intensity
    float4   gPreviewAmbient;  // rgb = sky fill, w = selection highlight strength
};

// What the preview vertex shader hands the pixel shader.
struct PreviewOut {
    float4 pos   : SV_POSITION;
    float3 nrmWS : NORMAL;
    float3 wpos  : TEXCOORD0;
    // ADDED FOR THE MATERIAL PATH: AverVertex (PbrShaders.cpp) carries a uv, and a graph that
    // samples a map needs one to sample it with. PreviewPS below still ignores it -- the simple
    // shader has no texture to sample -- so this costs it one unread interpolant, not a behaviour
    // change.
    float2 uv    : TEXCOORD1;
};

// Transforms a vertex to clip space and its normal to world space.
PreviewOut PreviewVS(VSIn i) {
    PreviewOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos  = wp.xyz;
    o.pos   = mul(wp, gPreviewViewProj);
    o.nrmWS = normalize(averTransformNormal(i.nrm, gWorld));
    // Taken straight from VSIn, exactly like VSMain does in RHIShaders.cpp -- VSIn already carries
    // it (every mesh in this engine does), so nothing upstream of this shader has to change.
    o.uv    = i.uv;
    return o;
}

// Shades a pixel with one key light, a hemisphere fill and a selection rim.
float4 PreviewPS(PreviewOut i) : SV_TARGET {
    float3 n = normalize(i.nrmWS);
    float3 l = normalize(gPreviewKey.xyz);

    float ndl = saturate(dot(n, l));
    float3 base = gBaseColor.rgb;

    float up = n.z * 0.5 + 0.5;
    float3 fill = lerp(gPreviewAmbient.rgb * 0.35, gPreviewAmbient.rgb, up);

    float3 lit = base * (fill + ndl * gPreviewKey.w);

    float rim = pow(1.0 - saturate(dot(n, normalize(gPreviewEye.xyz - i.wpos))), 3.0);
    lit += gPreviewAmbient.w * rim * float3(1.0, 0.62, 0.2);

    return float4(toGamma(acesTonemap(lit)), 1.0);
}
