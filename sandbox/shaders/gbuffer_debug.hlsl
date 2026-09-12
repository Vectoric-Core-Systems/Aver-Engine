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
        // NORMAL + ROUGHNESS -- IDevice::gBufferNormalRoughnessTexture's own encoding is NRD's
        // NRD_NORMAL_ENCODING_R10G10B10A2_UNORM (see that accessor's own comment in RHI.hpp), NOT a
        // plain n*0.5+0.5 this view used to assume it could show unmodified. xyz jointly carry the
        // normal AND the roughness now, so a raw sample is meaningless as a colour -- it has to be
        // decoded first.
        //
        // _NRD_DecodeNormalRoughness101010, transcribed from third_party/nrd/Shaders/NRD.hlsli (see
        // averPackNormalRoughness's own comment in voxi.hlsl for why transcribed, not included --
        // this is the SAME transcription a second time, and the two are a matched pair: this decode
        // is only correct against that encode).
        const float3 p = gGBufferDebugTex.Sample(gGBufferDebugSamp, i.uv).xyz;
        const float t = p.z * 2.0 - 1.0;   // signed roughness; its SIGN is N.z's sign
        float3 n;
        n.x = p.x - p.y;
        n.y = p.x + p.y - 1.0;
        n.z = (t < 0.0 ? -1.0 : 1.0) * (1.0 - abs(n.x) - abs(n.y));
        // _NRD_SafeNormalize, transcribed likewise: rsqrt(dot+eps) rather than a bare normalize(),
        // so a grazing sample that decodes to a near-zero vector stays finite instead of NaN-ing the
        // whole debug view.
        n *= rsqrt(dot(n, n) + 1e-9);
        // roughness itself is abs(t) (NRD's own decode) -- unused here, because this view answers
        // "does the normal decode right", the same question it answered before this change, and
        // there is no separate roughness debug mode to feed it into.
        return float4(n * 0.5 + 0.5, 1.0);
    }
    return float4(1.0, 0.0, 1.0, 1.0);   // unreached while `mode` is set from GBufferDebugFeature::Mode
}
