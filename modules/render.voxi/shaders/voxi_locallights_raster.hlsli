// Raster local lights (G4 R2): omni and spot lights shaded in PSMainVoxi from the capped list in the AVER_LOCAL_RASTER tail of the frame block
// (gRlHeader, gRlLight, gRlTileVP, gRlTileUv), with PCF shadow maps from an atlas. No ray query, no bindless table: it runs on Vulkan and with
// ray tracing off. Included by voxi.hlsl only under AVER_LOCAL_RASTER; nothing here is in any other variant.
// Host side and layout: modules/render.voxi/include/aver/voxi/RasterLights.hpp. Design: docs/plans/fabric/G4_RENDER.md R2.
//
// One light = AVER_RL_LIGHT_VECS float4:
//   0 pos.xyz (cm), emitter radius cm         1 colour * irradiance at 1 m (engine units, fade applied), range cm
//   2 axis.xyz, 1 for a spot                  3 decay exponent, cos outer, (Aver: cos inner | Godot: 1 / angle attenuation), flags
//   4 specular scale, shadow weight, light cull mask (inverted), shadow caster mask (inverted; the CPU shadow pass reads it)
//                                             5 first tile, tile count, constant bias cm, normal bias texels
//   6 world texel size per unit distance, filter radius texels, near cm, far cm
#ifndef AVER_VOXI_LOCALLIGHTS_RASTER_HLSLI
#define AVER_VOXI_LOCALLIGHTS_RASTER_HLSLI

#define AVER_RL_F_GODOT    1u
#define AVER_RL_F_NEGATIVE 2u
#define AVER_RL_F_SHADOWED 4u
#define AVER_RL_F_CUBE     8u

// The atlas: the local-light slot t19 (the ray-driven history's slot; nothing else reads it in these variants). In the ray-traced twin that slot
// is already declared, as a float4 texture; the comparison works on either.
#if AVER_RT
#define gRlShadowTex gRdLocalHist
#else
Texture2D<float> gRlShadowTex : register(t19);
#endif

// Cube face of a light->receiver offset: +X, -X, +Y, -Y, +Z, -Z. The host builds the tile matrices in this order (rl::faceForward).
uint rlCubeFace(float3 d) {
    const float3 a = abs(d);
    if (a.x >= a.y && a.x >= a.z) return d.x >= 0.0 ? 0u : 1u;
    if (a.y >= a.z) return d.y >= 0.0 ? 2u : 3u;
    return d.z >= 0.0 ? 4u : 5u;
}

// Fraction of the light that reaches wpos: 3x3 PCF in the light's tile. The receiver is moved along its normal (texels) and toward the light
// (cm) before it is projected, so the bias is in world units whatever the depth distribution.
float rlShadowVis(uint b, uint flags, float3 wpos, float3 Nf, float3 toL, float dist) {
    const float4 v4 = gRlLight[b + 4u];
    const float4 v5 = gRlLight[b + 5u];
    const float4 v6 = gRlLight[b + 6u];
    uint tile = (uint)(v5.x + 0.5);
    if ((flags & AVER_RL_F_CUBE) != 0u) tile += rlCubeFace(-toL);
    const float3 p = wpos + Nf * (v5.w * v6.x * dist) + toL * v5.z;
    const float4 lp = mul(float4(p, 1.0), gRlTileVP[tile]);
    if (lp.w <= 1e-4) return 1.0;
    const float3 q = lp.xyz / lp.w;
    const float2 uv = float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || q.z > 1.0 || q.z < 0.0) return 1.0;

    const float4 tu = gRlTileUv[tile];
    const float texel = gRlHeader.z * v6.y;                 // filter step, in atlas uv
    const float inset = gRlHeader.z * (v6.y + 1.0);         // keep every tap inside the tile
    const float2 auv = tu.xy + clamp(uv * tu.zw, float2(inset, inset), tu.zw - float2(inset, inset));
    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        s += gRlShadowTex.SampleCmpLevelZero(gShadowSamp, auv + float2(x, y) * texel, q.z);
    return lerp(1.0, s / 9.0, v4.y);
}

// Godot's and Aver's falloff, as rl::distanceFactor.
float rlDistanceFactor(float d, float range, float radius, bool godot, float decay) {
    const float x = d / range;
    if (godot) return max(1.0 - x, 0.0) * pow(100.0 / max(d, max(radius, 0.01)), decay);
    float w = saturate(1.0 - x * x * x * x);
    w *= w;
    return w * 1e4 / max(d * d, radius * radius);
}

// The cone, as rl::coneFactor.
float rlConeFactor(float cosAxis, float cosOuter, float cosInnerOrInvAtt, bool godot) {
    if (cosAxis <= cosOuter) return 0.0;
    if (godot) {
        const float rim = max(1e-4, (1.0 - cosAxis) / max(1.0 - cosOuter, 1e-6));
        return 1.0 - pow(rim, cosInnerOrInvAtt);
    }
    const float t = saturate((cosAxis - cosOuter) / max(cosInnerOrInvAtt - cosOuter, 1e-3));
    return t * t * (3.0 - 2.0 * t);
}

// Every listed light through the surface's own BRDF (Cook-Torrance GGX, the sun's terms), diffuse and specular apart so `specular scale`
// (Godot's light_specular) can scale the second. Nf is the face-forwarded surface normal, for the shadow offset. May be negative for a
// negative light; the caller clamps the finished pixel.
float3 rlShade(AverSurface s, float3 wpos, float3 Nf) {
    if (s.model == AVER_MODEL_UNLIT) return float3(0.0, 0.0, 0.0);
    float3 acc = float3(0.0, 0.0, 0.0);
    const uint n = min((uint)(gRlHeader.x + 0.5), (uint)AVER_RL_MAX_LIGHTS);
    [loop] for (uint i = 0u; i < n; ++i) {
        const uint b = i * (uint)AVER_RL_LIGHT_VECS;
        const float4 v0 = gRlLight[b];
        const float4 v1 = gRlLight[b + 1u];
        const float3 toC = v0.xyz - wpos;
        const float  d2 = dot(toC, toC);
        if (d2 >= v1.w * v1.w) continue;
        // Godot's light_cull_mask (G4 R4): both masks are stored inverted (0 = every layer), so a light that excludes nothing skips this test.
        // _objPad carries the instance's inverted layers (IDevice::setDrawLayers); zero for everything that never set any.
        const float4 v4 = gRlLight[b + 4u];
        const uint   lightCullInv = (uint)(v4.z + 0.5);
        if (lightCullInv != 0u && (((~asuint(_objPad)) & (~lightCullInv)) & 0xFFFFFu) == 0u) continue;
        const float4 v2 = gRlLight[b + 2u];
        const float4 v3 = gRlLight[b + 3u];
        const uint   flags = (uint)(v3.w + 0.5);
        const bool   godot = (flags & AVER_RL_F_GODOT) != 0u;
        const float  d = sqrt(d2);
        const float3 toL = toC / max(d, 1e-4);

        float f = rlDistanceFactor(d, v1.w, v0.w, godot, v3.x);
        if (v2.w > 0.5) f *= rlConeFactor(dot(-toL, v2.xyz), v3.y, v3.z, godot);
        if (f <= 0.0) continue;

        float vis = 1.0;
        if ((flags & AVER_RL_F_SHADOWED) != 0u) vis = rlShadowVis(b, flags, wpos, Nf, toL, d);

        AverLight l;
        l.direction  = toL;
        l.radiance   = v1.rgb * f;
        l.visibility = float3(vis, vis, vis);
        // Aim the surface's half vector and Fresnel at THIS light (they were built for the sun) and widen the lobe by the emitter's size.
        AverSurface sL = s;
        sL.rough = clamp(s.rough + v0.w * 0.5 / max(d, 1e-4), s.rough, 1.0);
        sL.H     = normalize(s.V + toL);
        sL.F     = fresnelSchlick(saturate(dot(sL.H, s.V)), s.F0, s.f90);

        float3 dD, dS, dSss;
        float  ndl;
        averDirectTerms(sL, l, dD, dS, dSss, ndl);
        const float3 lt = l.radiance * (ndl * vis);
        const float3 contrib = dD * lt + dSss * l.radiance * vis + dS * lt * v4.x;
        acc += (flags & AVER_RL_F_NEGATIVE) != 0u ? -contrib : contrib;
    }
    return acc;
}

#endif
