// voxi_froxel.hlsl -- volumetric fog: a camera-aligned froxel volume of sun light scattered by the air.
// Compiled by VoxiFroxelFog.cpp on its own (never part of voxi.hlsl's translation unit); the engine prelude
// supplies PerFrame. docs/rendering/VOLUMETRIC_FOG.md.
//
//   CSFroxelScatter   one thread per froxel: the medium's density at a jittered point inside the slice, the sun
//                     through it (ray query against the TLAS, else the cascade shadow map, else unshadowed),
//                     a Henyey-Greenstein phase and the sky ambient. Blended with last frame's reprojected
//                     result. Writes (source radiance per unit extinction, extinction) to a ping-pong volume.
//   CSFroxelIntegrate one thread per column: front to back over z, writing (in-scatter, transmittance) to the
//                     volume the shade passes read (voxi.hlsl, u24) -- see voxi_froxel.hlsli.
//   PSFroxelSky       the same volume applied to the sky, which no shade site reaches (VSky from the prelude).
//
// Register map (the compute layout is the Voxi main table 0 plus one SRV and one UAV in table 1, so it shares
// the TLAS, shadow map, air-visibility volume and the output volume with the scene shaders):
//   t1 shadow map, t2 TLAS, t17 air visibility, u24 output volume (table 0); t25 history, u25 scratch (table 1).
//   The sky draw has its own layout: u0 = the output volume.
#ifndef AVER_RT
#define AVER_RT 0
#endif
#ifndef AVER_FX_SKY
#define AVER_FX_SKY 0
#endif

#include "voxi_froxel.hlsli"

// Mirrors FroxelConstants (VoxiFroxelFog.hpp) byte for byte. Feature-frame register, bound per dispatch.
cbuffer FroxelCB : register(AVER_CB_JOIN(b, AVER_FEATURE_FRAME_CB)) {
    float4x4 gFxCascadeViewProj[4];  // world -> each shadow cascade's clip box (copied from Voxi's frame block)
    float4   gFxCascadeSplit[4];     // x = radius at which the cascade stops applying
    float4   gFxGrid;                // xyz = volume dimensions, w = frame number
    float4   gFxDepth;               // x = near cm, y = far cm, z = ln(far / near), w = 1 / z
    float4   gFxMedium;              // x = share of the height fog, y = uniform extinction per cm,
                                     // z = sun intensity, w = Henyey-Greenstein g
    float4   gFxTemporal;            // x = weight of the reprojected history (0 = none)
    float4x4 gFxPrevViewProj;        // last frame's view-projection (jitter-free)
    float4   gFxShadow;              // x = sun visibility source (0 none, 1 cascades, 2 ray query),
                                     // y = 1 / shadow atlas size, z = cascade count, w = ray bias cm
    float4   gFxVoxel;               // xyz = GI volume min corner, w = 1 / world size
    float4   gFxVoxelParams;         // x = the air-visibility volume is real
};

#if AVER_FX_SKY

RWTexture3D<float4> gFxSkyVol : register(u0);

// Premultiplied: rgb + dst * (1 - a). a carries 1 - transmittance, so dst ends up scaled by the transmittance.
float4 PSFroxelSky(SkyOut i) : SV_TARGET {
    const float2 uv = saturate(float2(i.ndc.x * 0.5 + 0.5, 0.5 - i.ndc.y * 0.5));
    const float4 v = froxelLookup(gFxSkyVol, uv, gFxDepth.y, gFxDepth.x, gFxDepth.w);
    return float4(v.rgb, 1.0 - v.a);
}

#else

SamplerState            gVoxelSamp  : register(s0);   // linear, clamp
SamplerComparisonState  gShadowSamp : register(s1);   // comparison, linear
Texture2D<float>        gShadowTex  : register(t1);
#if AVER_RT
RaytracingAccelerationStructure gScene : register(t2);
// Opaque instances and the owner-hidden lane: what casts the sun's shadow (mask lanes of voxi.hlsl).
#define AVER_FX_RT_MASK 0x05
#endif
Texture3D<float>        gAirVis     : register(t17);
RWTexture3D<float4>     gFxFinal    : register(u24);   // the output volume
Texture3D<float4>       gFxHist     : register(t25);   // last frame's scatter volume
RWTexture3D<float4>     gFxCur      : register(u27);   // this frame's scatter volume (table 1 starts past table 0's u0..u26)

float fxJitter(uint3 id, uint frame) {
    uint h = id.x * 0x9E3779B1u ^ (id.y * 0x85EBCA77u) ^ (id.z * 0xC2B2AE3Du);
    h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12; h *= 0x297A2D39u; h ^= h >> 15;
    // A per-froxel offset advanced by the golden ratio each frame: the history averages low-discrepancy depths.
    return frac((float)(h >> 8) * (1.0 / 16777216.0) + (float)(frame & 1023u) * 0.6180339887);
}

// Henyey-Greenstein, normalised over the sphere. c = cosine between the view ray and the direction to the sun
// (c = 1 looks at the sun: forward scattering, the shafts).
float fxHG(float c, float g) {
    const float g2 = g * g;
    return (1.0 - g2) / (4.0 * PI * pow(max(1.0 + g2 - 2.0 * g * c, 1e-4), 1.5));
}

// Sun visibility at a point in the air: 1 lit, 0 shadowed.
float fxSunVisibility(float3 p, float dist, float3 L) {
#if AVER_RT
    if (gFxShadow.x > 1.5) {
        RayDesc r;
        r.Origin = p;
        r.Direction = L;
        r.TMin = gFxShadow.w;
        r.TMax = 100000.0;
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, AVER_FX_RT_MASK, r);
        // Cutouts count as solid: the volume is too coarse to see through foliage anyway.
        while (q.Proceed()) {
            if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE) q.CommitNonOpaqueTriangleHit();
        }
        return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
    }
#endif
    if (gFxShadow.x > 0.5) {
        const uint count = min((uint)gFxShadow.z, 4u);
        if (count == 0u) return 1.0;
        // Same cascade pick, fade and 2x2 atlas layout as shadowFactor in voxi.hlsl.
        const float fadeSpan = max(gFxCascadeSplit[count - 1].x * (1.0 - 0.84), 1e-3);
        const float fade = saturate((dist - gFxCascadeSplit[count - 1].x * 0.84) / fadeSpan);
        [loop] for (uint c = 0; c < count; ++c) {
            if (dist > gFxCascadeSplit[c].x) continue;
            const float4 lp = mul(float4(p, 1.0), gFxCascadeViewProj[c]);
            const float3 q = lp.xyz / lp.w;
            float2 uv = float2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
            if (any(uv < 0.0) || any(uv > 1.0) || q.z > 1.0 || q.z < 0.0) continue;
            const float2 quad = float2(c & 1u, c >> 1u) * 0.5;
            const float inset = gFxShadow.y;
            uv = quad + clamp(uv * 0.5, float2(inset, inset), float2(0.5 - inset, 0.5 - inset));
            const float s = gShadowTex.SampleCmpLevelZero(gShadowSamp, uv, q.z);
            return lerp(s, 1.0, fade);
        }
    }
    return 1.0;
}

// The sky light the uniform medium scatters: the same reference colour the analytic fog tends to, scaled by how
// much sky the air can see (the GI-volume-sized air-visibility volume, when it exists).
float3 fxAmbient(float3 p) {
    float3 a = averAtmoOn() ? averFogInscatterRef() : skyColorFull(float3(0.0, 0.0, 0.25));
    a *= srgbToLin(gFogColor.rgb);
    float av = 1.0;
    if (gFxVoxelParams.x > 0.5) {
        const float3 uvw = (p - gFxVoxel.xyz) * gFxVoxel.w;
        if (all(uvw >= 0.0) && all(uvw <= 1.0)) av = gAirVis.SampleLevel(gVoxelSamp, uvw, 0.0);
    }
    return a * av;
}

// View ray and the factor from view depth to distance along it, for the froxel column (x, y).
void fxColumn(uint2 xy, uint2 dims, out float3 dir, out float invCos) {
    const float2 uv = (float2(xy) + 0.5) / float2(dims);
    dir = averViewRayDir(float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0));
    // clip.w is the view depth, and its gradient in world space is the camera's forward axis.
    const float3 fwd = float3(gViewProj[0][3], gViewProj[1][3], gViewProj[2][3]);
    invCos = 1.0 / max(dot(dir, fwd), 1e-4);
}

[numthreads(8, 8, 1)]
void CSFroxelScatter(uint3 id : SV_DispatchThreadID) {
    const uint3 dim = uint3(gFxGrid.xyz);
    if (any(id >= dim)) return;
    const float nearCm = gFxDepth.x, logRange = gFxDepth.z;

    float3 dir; float invCos;
    fxColumn(id.xy, dim.xy, dir, invCos);

    // The medium is read at a depth jittered inside the slice, the history is looked up at its centre.
    const float wJ = (float(id.z) + fxJitter(id, (uint)gFxGrid.w)) / float(dim.z);
    const float wC = (float(id.z) + 0.5) / float(dim.z);
    const float tJ = nearCm * exp(wJ * logRange) * invCos;
    const float3 pJ = gCamPos.xyz + dir * tJ;

    // The authored exponential height fog, from the same constants averFogFactor integrates.
    float sigmaH = 0.0;
    if (tJ > gFogParams.z) {
        const float k = gFogParams.x;
        sigmaH = gFogColor.a * (k <= 1e-8 ? 1.0 : exp(clamp(-(pJ.z - gFogParams.y) * k, -30.0, 30.0)));
    }
    sigmaH *= gFxMedium.x;
    const float sigmaG = gFxMedium.y;
    const float sigma = sigmaH + sigmaG;

    // Source radiance per unit extinction: sunlight scattered toward the eye, plus the sky light the uniform
    // medium scatters. (The height fog's own sky light is already in the shaded colour.)
    float3 src = float3(0.0, 0.0, 0.0);
    if (sigma > 1e-12) {
        const float3 L = normalize(gLightDir.xyz);
        const float3 sunE = averSunRadiance();
        if (any(sunE > 0.0)) {
            const float vis = fxSunVisibility(pJ, tJ, L);
            src = sunE * (fxHG(dot(dir, L), gFxMedium.w) * vis * gFxMedium.z);
        }
        if (sigmaG > 0.0) src += fxAmbient(pJ) * (sigmaG / sigma);
        src = min(src, 6.0e4);
    }

    float4 cur = float4(src, sigma);

    if (gFxTemporal.x > 0.0) {
        const float tC = nearCm * exp(wC * logRange) * invCos;
        const float4 pc = mul(float4(gCamPos.xyz + dir * tC, 1.0), gFxPrevViewProj);
        if (pc.w > 1e-3) {
            const float2 pn = pc.xy / pc.w;
            const float3 puvw = float3(pn.x * 0.5 + 0.5, 0.5 - pn.y * 0.5, log(pc.w / nearCm) / logRange);
            if (all(puvw >= 0.0) && all(puvw <= 1.0)) {
                const float4 h = gFxHist.SampleLevel(gVoxelSamp, puvw, 0.0);
                if (all(isfinite(h))) cur = lerp(cur, h, gFxTemporal.x);
            }
        }
    }
    gFxCur[id] = cur;
}

[numthreads(8, 8, 1)]
void CSFroxelIntegrate(uint3 id : SV_DispatchThreadID) {
    const uint3 dim = uint3(gFxGrid.xyz);
    if (any(id.xy >= dim.xy)) return;
    const float nearCm = gFxDepth.x, logRange = gFxDepth.z;

    float3 dir; float invCos;
    fxColumn(id.xy, dim.xy, dir, invCos);

    float3 scatter = float3(0.0, 0.0, 0.0);
    float tTotal = 1.0, tUniform = 1.0;
    float zNear = nearCm * invCos;
    [loop] for (uint z = 0u; z < dim.z; ++z) {
        const float zFar = nearCm * exp(float(z + 1u) / float(dim.z) * logRange) * invCos;
        const float dt = zFar - zNear;
        zNear = zFar;
        const float4 c = gFxCur[uint3(id.xy, z)];
        // A constant source across the slice, attenuated by everything in front of it.
        const float stepT = exp(-c.a * dt);
        scatter += tTotal * c.rgb * (1.0 - stepT);
        tTotal *= stepT;
        tUniform *= exp(-gFxMedium.y * dt);
        gFxFinal[uint3(id.xy, z)] = float4(min(scatter, 6.0e4), tUniform);
    }
}

#endif
