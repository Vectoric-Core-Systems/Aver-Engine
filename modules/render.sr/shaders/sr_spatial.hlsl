// AverSR: the spatial upscale.
//
// Moved out of a C++ raw-string literal; composed exactly as before -- see the call site.

// ================= AverSR: the built-in spatial upscaler =================
// Scene-resolution colour in (t0), present-resolution colour out. Bicubic (Catmull-Rom, a = -0.5)
// resample, separable in x and y over the 4x4 texel neighbourhood around the destination pixel's
// mapped source position. Sharper than a bilinear stretch without reading anything but the source
// texture: no depth, no motion vectors, no jitter, no history -- see SpatialUpscaler::needs().

cbuffer AverSrSpatialCB : register(b1) {
    float4 gUpscaleSrc;   // xy source size in texels, zw its reciprocal
};

Texture2D<float4> gUpscaleSceneTex : register(t0);
SamplerState       gUpscaleSamp    : register(s0);

struct AverSrVSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// Emits one vertex of the fullscreen upscale triangle from SV_VertexID -- no vertex buffer bound.
AverSrVSOut AverSrSpatialVS(uint id : SV_VertexID) {
    AverSrVSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// The four Catmull-Rom cubic-convolution weights (a = -0.5) for a fractional offset t in [0, 1),
// for the samples at relative texel offsets -1, 0, +1, +2. Sums to exactly 1 for any t, so the
// four taps in each of x and y are already a partition of unity -- no separate normalisation.
float4 averSrCatmullRomWeights(float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return float4(
        -0.5 * t3 +       t2 - 0.5 * t,
         1.5 * t3 - 2.5 * t2 + 1.0,
        -1.5 * t3 + 2.0 * t2 + 0.5 * t,
         0.5 * t3 - 0.5 * t2);
}

float4 AverSrSpatialMain(AverSrVSOut i) : SV_TARGET {
    float2 srcTexel = i.uv * gUpscaleSrc.xy - 0.5;
    float2 f    = frac(srcTexel);
    float2 base = floor(srcTexel);

    float4 wx = averSrCatmullRomWeights(f.x);
    float4 wy = averSrCatmullRomWeights(f.y);

    float3 sum = float3(0.0, 0.0, 0.0);
    [unroll] for (int y = -1; y <= 2; ++y) {
        [unroll] for (int x = -1; x <= 2; ++x) {
            // Clamped to the source's edge, matching this pass's own AddressMode::Clamp sampler.
            float2 texel = clamp(base + float2(x, y), 0.0, gUpscaleSrc.xy - 1.0);
            float2 uv = (texel + 0.5) * gUpscaleSrc.zw;
            sum += gUpscaleSceneTex.SampleLevel(gUpscaleSamp, uv, 0).rgb * (wx[x + 1] * wy[y + 1]);
        }
    }
    // Catmull-Rom can ring (overshoot) at hard edges; radiance below zero is meaningless, so clamp
    // the low end and leave the high end alone -- this runs before the post chain's own tonemap
    // when wired in, and that is what is responsible for compressing an overshoot back into range.
    return float4(max(sum, 0.0), 1.0);
}
