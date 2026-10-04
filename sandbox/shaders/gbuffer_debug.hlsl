// The editor's G-buffer debug view: velocity, viewZ, normal+roughness.
//
// Moved out of a C++ raw-string literal. Its mode ids are still kept in sync BY HAND with
// GBufferDebugFeature::Mode in SandboxApp.cpp -- moving the text does not fix that, and the
// comment inside says so where a reader will meet it.

// mode (gGBufferDebugParams.x): 1 velocity, 2 viewZ, 3 normal+roughness -- GBufferDebugFeature::Mode
// in SandboxApp.cpp, kept in sync by hand since this string has no access to that C++ enum.
cbuffer GBufferDebugCB : register(b1) {
    float4 gGBufferDebugParams;
};

Texture2D    gGBufferDebugTex  : register(t0);
SamplerState gGBufferDebugSamp : register(s0);

struct GBufferDebugVSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// The identical fullscreen-triangle-from-SV_VertexID trick AverSrFxaaVS/AverSrSpatialVS already use
// (modules/render.sr) -- three vertices, no vertex buffer, one triangle that overshoots the [-1,1]
// clip-space square (cheaper than two triangles: one draw call, no shared diagonal for the
// rasteriser to seam).
GBufferDebugVSOut VSGBufferDebug(uint id : SV_VertexID) {
    GBufferDebugVSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSGBufferDebug(GBufferDebugVSOut i) : SV_TARGET {
    const int mode = (int)(gGBufferDebugParams.x + 0.5);
    if (mode == 1) {
        // VELOCITY -- IDevice::gBufferVelocityTexture's own units (RHI.hpp): texels/frame,
        // destination minus source. Raw values are a handful of texels at most -- invisible against
        // an 8-bit backbuffer with no remap at all -- so this maps
        // [-kVelocityFullScaleTexels, +kVelocityFullScaleTexels] to [0,1] with a 0.5 BIAS, not a bare
        // multiply: a bare multiply clips a small NEGATIVE component to the same black an EXACT zero
        // produces, which would make a bug (nonzero velocity where a still camera should read exactly
        // zero) indistinguishable from a correct frame. The bias keeps zero at an unambiguous
        // mid-grey (0.5, 0.5) and keeps sign visible on either side of it -- see
        // GBufferDebugFeature::kVelocityFullScaleTexels for the exact number "fully saturated"
        // corresponds to.
        float2 v = gGBufferDebugTex.Sample(gGBufferDebugSamp, i.uv).rg;
        float2 shown = saturate(v / max(gGBufferDebugParams.y, 1e-5) * 0.5 + 0.5);
        return float4(shown, 0.5, 1.0);
    }
    if (mode == 2) {
        // VIEW-SPACE LINEAR DEPTH -- IDevice::gBufferViewZTexture's own units: NOT the post-
        // projection [0,1] depth-buffer value (see that accessor's own comment for why the two
        // disagree by a projection-dependent curve). Normalised here by a documented reference
        // distance PURELY for display; nothing about this remap touches what a real consumer would
        // read from the texture.
        float z = gGBufferDebugTex.Sample(gGBufferDebugSamp, i.uv).r;
        float shown = saturate(z / max(gGBufferDebugParams.z, 1e-5));
        return float4(shown, shown, shown, 1.0);
    }
    if (mode == 3) {
        // NORMAL + ROUGHNESS -- IDevice::gBufferNormalRoughnessTexture packs an octahedral normal
        // (Cigolle 2014) in xy and the roughness in z, so a raw sample is meaningless as a colour
        // and has to be decoded first. This is the matched pair of averPackNormalRoughness
        // (voxi.hlsl): xy are the folded octahedral coordinates remapped to [0,1].
        const float2 e = gGBufferDebugTex.Sample(gGBufferDebugSamp, i.uv).xy;
        const float2 f = e * 2.0 - 1.0;
        float3 n = float3(f, 1.0 - abs(f.x) - abs(f.y));
        if (n.z < 0.0) {
            const float2 s = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
            n.xy = (1.0 - abs(n.yx)) * s;
        }
        // rsqrt(dot+eps) rather than a bare normalize(), so a degenerate sample stays finite instead
        // of NaN-ing the whole debug view. Roughness (z) is unused here: this view answers "does
        // the normal decode right", and there is no separate roughness debug mode.
        n *= rsqrt(dot(n, n) + 1e-9);
        return float4(n * 0.5 + 0.5, 1.0);
    }
    return float4(1.0, 0.0, 1.0, 1.0);   // unreached while `mode` is set from GBufferDebugFeature::Mode
}
