// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
//
// The GI/shadow slice of Voxi's HLSL a foreign pipeline may borrow. See the public header's own
// comment for what this is and, more importantly, what it deliberately is not: PSMainVoxi and every
// compute pass stay in VoxiShaders.hpp, private to this module, never textually reachable from here.
#include "aver/voxi/VoxiGiShaders.hpp"

namespace aver::voxi {

const char* giShaderPrelude() {
    // A CUSTOM DELIMITER (HLSL, matching ClusterMaterialShader.hpp's own kClusterMaterialPS), NOT the
    // bare R"(...)" PbrShaders.cpp uses: this text's own #error message calls giShaderDefines(),
    // and the plain form's terminator is exactly the two characters `)"` that call's closing `()"`
    // ends with -- which silently truncates the raw string right there, turning every #if/#endif
    // below the truncation point into real (and unbalanced) preprocessor directives. Caught by DXC
    // never even running: MSVC's own C1020 "unexpected #endif" on the very next line said so first.
    static const std::string s = R"HLSL(
#if !defined(AVER_GI_SRV) || !defined(AVER_GI_SAMPLER) || !defined(AVER_GI_FRAME_REG)
#error "aver/voxi/VoxiGiShaders.hpp needs AVER_GI_SRV / AVER_GI_SAMPLER / AVER_GI_FRAME_REG from giShaderDefines()"
#endif
#define AVER_GI_JOIN2(a, b) a##b
#define AVER_GI_JOIN(a, b) AVER_GI_JOIN2(a, b)

// Cascade count is fixed at 4 on Voxi's own side too (VoxiShaders.hpp's identically-named define);
// never seen in the same compile as that one, so redefining it here is not a collision.
#define AVER_SHADOW_CASCADES 4

// The ceiling on voxel radiance, and the same arrangement as the line above: VoxiShaders.hpp
// defines this identically for the raster path, the two preludes are never in one compile, and
// the value has to match. It bounds BOTH ends of the volume -- what PSVoxel injects and what
// coneTracedIndirect hands back -- so a change here is a change to VoxiShaders.hpp too.
#define AVER_VOX_MAXRAD 16.0

// BYTE FOR BYTE aver::voxi::VoxiRenderer::FrameConstants (VoxiRenderer.hpp), same as Voxi's own
// `cbuffer VoxiFrame` in VoxiShaders.hpp -- deliberately the FULL block, not a trimmed one, so a
// caller can bind VoxiRenderer::giFrameConstants()/giFrameConstantBytes() verbatim instead of the
// engine needing a second, easy-to-drift mirror of the same struct. shadowFactor()/
// coneTracedIndirect() below only ever read the first five fields; the rest (RT history, GI-only
// shadow) sit here unread, exactly as over-provisioned as the SRV table above them.
cbuffer VoxiFrame : register(AVER_GI_JOIN(b, AVER_GI_FRAME_REG)) {
    float4   gVoxelOrigin; // xyz = volume min corner, w = 1/volumeWorldSize
    float4   gVoxelParams; // x = resolution, y = intensity, z = maxDistance, w = enabled|debug<<1
    float4x4 gCascadeViewProj[AVER_SHADOW_CASCADES];
    float4   gCascadeSplit[AVER_SHADOW_CASCADES];
    float4   gShadowParams; // x = 1/atlasSize, y = enabled, z = accel structure built, w = cascades
    float4   gShadowDraw;
    float4   gRtParams;
    float4   gRtHistParams;
    float4x4 gPrevViewProj;
    float4   gSceneViewport;
    float4x4 gGiShadowViewProj;
    float4   gGiShadowParams;
    // THE TAIL THIS MIRROR WAS MISSING. The comment above calls this block byte for byte
    // FrameConstants, and it stopped three float4s short of being that -- harmless while nothing
    // here read past gGiShadowParams, and exactly the drift that comment exists to prevent. The
    // gather below now needs gGiParams, so the tail is declared rather than assumed.
    float4   gRtDenoiseParams;
    float4   gPtBounceParams;
    float4   gGiParams;
};

// t(AVER_GI_SRV) the GI volume, t(AVER_GI_SRV_1) the shadow map -- the only two of Voxi's table-0
// union this prelude declares a symbol for; see giShaderDefines()'s own comment on why the rest of
// kGiSrvCount/kGiUavCount are reserved by the caller's layout but never named here.
Texture3D<float4>      gVoxelTex  : register(AVER_GI_JOIN(t, AVER_GI_SRV));
SamplerState            gVoxelSamp : register(AVER_GI_JOIN(s, AVER_GI_SAMPLER));
Texture2D<float>        gShadowTex  : register(AVER_GI_JOIN(t, AVER_GI_SRV_1));
SamplerComparisonState  gShadowSamp : register(AVER_GI_JOIN(s, AVER_GI_SAMPLER_1));

// 3x3 PCF inside ONE cascade's quadrant of the atlas. Returns 1 = lit, 0 = shadowed, -1 = outside
// this cascade so the caller can try the next one. VERBATIM from VoxiShaders.hpp's own
// shadowSampleCascade -- see that file for how it was arrived at; nothing about borrowing it at a
// different base register changes what it computes.
float shadowSampleCascade(float3 wpos, uint c) {
    float4 lp = mul(float4(wpos, 1.0), gCascadeViewProj[c]);
    float3 p = lp.xyz / lp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0 || p.z < 0.0) return -1.0;

    float2 quad = float2(c & 1u, c >> 1u) * 0.5;
    float inset = gShadowParams.x;
    uv = quad + clamp(uv * 0.5, float2(inset, inset), float2(0.5 - inset, 0.5 - inset));

    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        s += gShadowTex.SampleCmpLevelZero(gShadowSamp, uv + float2(x, y) * gShadowParams.x, p.z);
    return s / 9.0;
}

// Where shadowing starts fading to unshadowed, as a fraction of the last cascade's reach.
#define AVER_SHADOW_FADE_START 0.84

// Picks a cascade by distance and samples it, offsetting along N by that cascade's bias. Returns
// sun visibility, 1 = fully lit. VERBATIM from VoxiShaders.hpp's own shadowFactor -- the SAME
// non-ray-traced fallback PSMainVoxi itself falls back to when AVER_RT is off or the acceleration
// structure is not built, so a cluster drawn through this prelude is shadowed exactly as an ordinary
// draw is whenever Voxi is not ray tracing.
float shadowFactor(float3 wpos, float3 N, float ndl) {
    if (gShadowParams.y < 0.5) return 1.0;
    uint count = (uint)gShadowParams.w;
    if (count == 0) return 1.0;
    count = min(count, (uint)AVER_SHADOW_CASCADES);

    float slope = saturate(1.0 - ndl);
    float dist  = distance(wpos, gCamPos.xyz);

    float fadeSpan = max(gCascadeSplit[count - 1].x * (1.0 - AVER_SHADOW_FADE_START), 1e-3);
    float fade = saturate((dist - gCascadeSplit[count - 1].x * AVER_SHADOW_FADE_START) / fadeSpan);

    [loop] for (uint c = 0; c < count; ++c) {
        if (dist > gCascadeSplit[c].x) continue;
        float bias = gCascadeSplit[c].y * (1.0 + slope);
        float s = shadowSampleCascade(wpos + N * bias, c);
        if (s >= 0.0) return lerp(s, 1.0, fade);
    }
    return 1.0;
}

// world -> [0,1] volume coords. VERBATIM from VoxiShaders.hpp.
float3 voxelUVW(float3 wp) { return (wp - gVoxelOrigin.xyz) * gVoxelOrigin.w; }
bool insideVolume(float3 uvw) { return all(uvw >= 0.0) && all(uvw <= 1.0); }

// Marches a cone through the volume, widening with distance and reading a coarser mip each step.
// Returns front-to-back composited, premultiplied radiance; alpha is coverage. VERBATIM from
// VoxiShaders.hpp's own traceCone.
float4 traceCone(float3 originWS, float3 dir, float aperture) {
    float voxelWorld = 1.0 / (gVoxelOrigin.w * gVoxelParams.x); // one voxel, world units
    float dist = voxelWorld * 2.0;
    float4 acc = 0;
    [loop] for (int step = 0; step < 24; ++step) {
        if (acc.a >= 0.95 || dist > gVoxelParams.z) break;
        float diameter = max(voxelWorld, 2.0 * aperture * dist);
        float mip = log2(diameter / voxelWorld);
        float3 uvw = voxelUVW(originWS + dir * dist);
        if (!insideVolume(uvw)) break;
        float4 s = gVoxelTex.SampleLevel(gVoxelSamp, uvw, mip);
        acc += (1.0 - acc.a) * s;
        dist += diameter;
    }
    return acc;
}

// Cosine-weighted gather of six cones over the hemisphere: one along the normal, five in a ring.
// Returns the indirect diffuse radiance and, through `ao`, the ambient occlusion. VERBATIM from
// VoxiShaders.hpp's own coneTracedIndirect -- the SAME cone trace PSMainVoxi itself runs when
// gVoxelParams.w says GI is on; a caller gates that check itself, the same way PSMainVoxi's own
// call site does, rather than this function re-testing it.
float3 coneTracedIndirect(float3 wpos, float3 N, out float ao) {
    float3 up = abs(N.z) < 0.9 ? float3(0,0,1) : float3(1,0,0);
    float3 T = normalize(cross(up, N)), B = cross(N, T);
    const float aperture = 0.577;              // ~60 degree cone

    float4 sum = traceCone(wpos, N, aperture);
    float occ = sum.a;
    float wsum = 1.0;
    // Mirrors VoxiShaders.hpp's coneTracedIndirect -- see the note there.
    const uint  ring = (uint)max(gGiParams.x, 1.0) - 1u;
    const float dphi = ring > 0u ? 6.2831853 / (float)ring : 0.0;
    [loop] for (uint k = 0; k < ring; ++k) {
        float ang = dphi * (float)k;
        float3 d = normalize(N * 0.5 + (T * cos(ang) + B * sin(ang)) * 0.866);
        float w = saturate(dot(N, d));
        float4 c = traceCone(wpos, d, aperture);
        sum += c * w; occ += c.a * w; wsum += w;
    }
    sum /= wsum; occ /= wsum;
    ao = saturate(1.0 - occ);
    // Bounded exactly as VoxiShaders.hpp's coneTracedIndirect is -- read the long note there
    // for why the ceiling is the injection's own constant. Two copies of this gather exist, and
    // clamping one of them only would be a difference between the raster and cluster paths that
    // nothing in the build would report.
    return min(sum.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
}
)HLSL";
    return s.c_str();
}

std::string giShaderDefines(u32 srvBase, u32 samplerBase, u32 frameConstantRegister) {
    return "AVER_GI_SRV=" + std::to_string(srvBase) +
           ";AVER_GI_SRV_1=" + std::to_string(srvBase + 1) +
           ";AVER_GI_SAMPLER=" + std::to_string(samplerBase) +
           ";AVER_GI_SAMPLER_1=" + std::to_string(samplerBase + 1) +
           ";AVER_GI_FRAME_REG=" + std::to_string(frameConstantRegister);
}

} // namespace aver::voxi
