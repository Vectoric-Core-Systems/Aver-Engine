#pragma once

// Voxi's own HLSL. Compiled as the TAIL of rhi::sharedShaderPrelude(): the cbuffer layouts, VSIn /
// VSOut / SkyOut, the BRDF helpers, the Aver* material contract, VSMain, VSky and MSMain all live in the prelude
// and must not be repeated here — duplicating them would make reordering one copy corrupt the other
// with no diagnostic anywhere.
//
// Declaration order below is load-bearing: HLSL has no forward declarations, so every helper is
// defined before the entry point that calls it.
namespace aver::voxi {

inline constexpr const char* kVoxiHLSL = R"(
// Feature-owned frame constants, at the register RHIResources.hpp reserves for exactly this. Split
// out of b0 so the engine block carries nothing a render feature introduced, and so a pass that
// recomputes only these (the cascade matrices are not known until the shadow pass runs) re-uploads
// this block alone rather than the whole engine one -- which the shadow pass now does once per
// cascade.
#define AVER_SHADOW_CASCADES 4

cbuffer VoxiFrame : register(b4) {
    float4   gVoxelOrigin; // xyz = volume min corner, w = 1/volumeWorldSize
    float4   gVoxelParams; // x = resolution, y = intensity, z = maxDistance, w = enabled|debug<<1
    // One per cascade, tightest first. Each maps world space into its own [-1,1] clip box; the
    // remap into the atlas quadrant happens in the sampler below, not in the matrix, so the SAME
    // matrix serves both the depth-only fill and the lookup.
    float4x4 gCascadeViewProj[AVER_SHADOW_CASCADES];
    // x = the RADIUS at which this cascade stops applying, y = its normal-offset bias in world units
    float4   gCascadeSplit[AVER_SHADOW_CASCADES];
    float4   gShadowParams; // x = 1/atlasSize, y = enabled, z = accel structure built, w = cascades
    float4   gShadowDraw;   // x = the cascade the depth-only pass is filling right now
};

// ---- Voxi: voxel cone traced GI ----
RWTexture3D<float4> gVoxelUAV : register(u0);
Texture3D<float4>   gVoxelTex : register(t0);
SamplerState        gVoxelSamp : register(s0);

// Injection accumulator. Several triangles legitimately cover one voxel -- the cube's bottom face
// is coplanar with the ground quad, and every box edge puts two differently-lit faces in the same
// cell -- and a plain `gVoxelUAV[c] = radiance` store resolves that contest by whichever fragment
// retires last, which the GPU does not promise to keep stable from frame to frame. Summing in
// fixed-point integers instead makes the result depend only on the SET of covering fragments, never
// on their order, because integer addition is associative and commutative where float addition and
// last-writer-wins are not.
//
// R32_UINT is the only typed format D3D12 guarantees atomics on, so the four accumulators are
// interleaved along x: channel k of voxel c lives at (c.x * 4 + k, c.y, c.z), with k == 3 the
// fragment count that CSResolve divides by.
RWTexture3D<uint> gVoxelAccum : register(u1);

// Radiance is scaled by this before being added, and divided out again in CSResolve. 1/16384 is two
// orders of magnitude finer than the 8-bit backbuffer can express, while leaving headroom for ~16k
// fragments per voxel before a 32-bit accumulator could wrap.
#define AVER_VOX_FIXED 16384.0
#define AVER_VOX_MAXRAD 16.0

// Directional shadow map. Core feature-level 11_0 (no optional caps), so it works on every
// DX12 GPU - which is why shadowed light injection uses this rather than ray-traced shadows.
Texture2D<float>          gShadowTex  : register(t1);
SamplerComparisonState    gShadowSamp : register(s1);

#if AVER_RT
// DXR 1.1 inline ray tracing. Traced from the pixel shader itself - no state objects, no shader
// binding tables, no DispatchRays - so it drops into the existing raster pipeline.
RaytracingAccelerationStructure gScene : register(t2);

// Exact hard shadow: one occlusion ray toward the sun. ACCEPT_FIRST_HIT_AND_END_SEARCH makes it a
// pure any-hit visibility query, which is much cheaper than finding the closest hit.
float rtShadow(float3 wpos, float3 N, float3 L) {
    RayDesc r;
    r.Origin    = wpos + N * 0.02;   // offset along the normal so we do not hit ourselves
    r.Direction = L;
    r.TMin      = 0.001;
    r.TMax      = 100000.0;
    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, 0xFF, r);
    q.Proceed();
    return q.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}
#endif

// 3x3 PCF inside ONE cascade's quadrant of the atlas. Returns 1 = fully lit, 0 = fully shadowed,
// -1 when the point falls outside this cascade so the caller can try the next one.
float shadowSampleCascade(float3 wpos, uint c) {
    float4 lp = mul(float4(wpos, 1.0), gCascadeViewProj[c]);
    float3 p = lp.xyz / lp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0 || p.z < 0.0) return -1.0;

    // Into the atlas. The 2x2 layout is the C++ side's (quadrant x = c&1, y = c>>1) and the halving
    // here is the only place that has to agree with it.
    float2 quad = float2(c & 1u, c >> 1u) * 0.5;
    // Inset by one texel so a PCF tap at the edge of a cascade cannot reach into its neighbour's
    // quadrant, which would read a completely unrelated depth and punch a hard line across the
    // shadow. This is the one cost of an atlas over a texture array, and it is one saturate.
    float inset = gShadowParams.x;
    uv = quad + clamp(uv * 0.5, float2(inset, inset), float2(0.5 - inset, 0.5 - inset));

    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        s += gShadowTex.SampleCmpLevelZero(gShadowSamp, uv + float2(x, y) * gShadowParams.x, p.z);
    return s / 9.0;
}

// Pick a cascade by distance and sample it. `N` is the surface normal: the bias is applied by
// OFFSETTING ALONG IT rather than by subtracting a constant from the depth.
//
// That is the whole reason this takes a normal now. A depth bias has to be tuned against the texel
// size, and a cascaded map's texels differ by two orders of magnitude between the near and far
// cascade -- tuned for one it either does nothing or detaches every shadow from its caster in the
// other. Offsetting the SAMPLE POSITION by a fraction of the cascade's own world-space texel is
// scale-correct by construction, and it moves the sample off the surface rather than lying about
// how far away the light is, so contact shadows stay attached.
// Where the shadowed distance starts giving way to the unshadowed one, as a fraction of the last
// cascade's reach. The tail has to be long enough that the ramp is slower than the eye's ability to
// notice a change in brightness sweeping across a surface, and short enough that it does not eat
// shadow everyone can see. A sixth of the range is about two cascades' worth of the far end.
#define AVER_SHADOW_FADE_START 0.84

float shadowFactor(float3 wpos, float3 N, float ndl) {
    if (gShadowParams.y < 0.5) return 1.0;
    // CLAMPED AT BOTH ENDS before it is ever used as an index, and both ends are reachable.
    //
    // Zero: fitCascades returns 0 when there is no camera to fit to and does not clear the
    // "shadow map usable" flag when it does, so count - 1 would wrap to 4294967295.
    //
    // Above the array: gShadowParams.w is a float carrying a count, and this reads it back through a
    // conversion. Anything that leaves that lane stale, garbage or simply larger than the array --
    // an uninitialised constant block, a partially-filled upload, a future cascade count raised in
    // C++ without raising AVER_SHADOW_CASCADES here -- indexes a cbuffer out of bounds. That is
    // undefined behaviour rather than a clamped read, and on a real driver it is an access violation
    // inside the driver's own DLL, which is a crash with no line of this file anywhere near it. The
    // first version guarded the underflow and not the overflow, which is half a guard.
    uint count = (uint)gShadowParams.w;
    if (count == 0) return 1.0;
    count = min(count, (uint)AVER_SHADOW_CASCADES);

    // Grazing surfaces need more offset: at ndl ~ 0 a texel of depth spans a long way along the
    // surface, which is exactly where acne appears.
    float slope = saturate(1.0 - ndl);
    float dist  = distance(wpos, gCamPos.xyz);

    // HOW THE SHADOWED WORLD ENDS. Past the last cascade this returned 1.0 -- fully lit, in one
    // step, with nothing between shadowed and not. Walk away from anything and at a fixed distance
    // its shadowing vanished outright; because the boundary is a sphere about the camera, it crossed
    // a surface as a moving edge, so what you saw was a wave of light washing over the object rather
    // than the object simply changing. It is the most visible artefact the shadow path had, and it
    // was entirely a matter of there being no ramp.
    //
    // The reach is not the problem and is not what changed: it is camNear * 4000, so eighty metres
    // at a two-centimetre near plane. Extending it would buy a more distant cliff at the cost of
    // texel density everywhere nearer, which is the trade cascades exist to avoid. What was missing
    // was the fade.
    float fadeSpan = max(gCascadeSplit[count - 1].x * (1.0 - AVER_SHADOW_FADE_START), 1e-3);
    float fade = saturate((dist - gCascadeSplit[count - 1].x * AVER_SHADOW_FADE_START) / fadeSpan);

    [loop] for (uint c = 0; c < count; ++c) {
        if (dist > gCascadeSplit[c].x) continue;
        // Up to TWO texels at grazing incidence, not four and a half. The offset moves the sample
        // position along the normal, so every texel of it erodes the shadow's edge inward -- a
        // generous bias buys freedom from acne by paying in crispness, and 4.5 texels was paying
        // far more than the acne was worth.
        float bias = gCascadeSplit[c].y * (1.0 + slope);
        float s = shadowSampleCascade(wpos + N * bias, c);
        // Toward lit as the far edge approaches, so the boundary is met already unshadowed and
        // crossing it changes nothing.
        if (s >= 0.0) return lerp(s, 1.0, fade);
    }
    // Past the last cascade, or in none of them. Lit: an unshadowed distance is far less visible
    // than a black horizon, and the alternative would be a shadow that is simply wrong. Reached with
    // fade already at 1.0 for the distance case, which is what makes it continuous.
    return 1.0;
}

// Mip level being read by CSMip (b3: b0/b1 are taken by the graphics root signature).
cbuffer MipCB : register(b3) { uint gSrcMip; uint3 _mipPad; };

// world -> [0,1] volume coords
float3 voxelUVW(float3 wp) { return (wp - gVoxelOrigin.xyz) * gVoxelOrigin.w; }
bool insideVolume(float3 uvw) { return all(uvw >= 0.0) && all(uvw <= 1.0); }

// ---- cone tracing (defined before its callers: HLSL needs definition before use) ----
// March a cone through the volume, widening with distance and reading a coarser mip each step so
// one sample covers the cone's footprint. Front-to-back alpha compositing.
float4 traceCone(float3 originWS, float3 dir, float aperture) {
    float voxelWorld = 1.0 / (gVoxelOrigin.w * gVoxelParams.x); // one voxel, world units
    float dist = voxelWorld * 2.0;                              // start off-surface to avoid self-hit
    float4 acc = 0;
    [loop] for (int step = 0; step < 24; ++step) {
        if (acc.a >= 0.95 || dist > gVoxelParams.z) break;
        float diameter = max(voxelWorld, 2.0 * aperture * dist);
        float mip = log2(diameter / voxelWorld);
        float3 uvw = voxelUVW(originWS + dir * dist);
        if (!insideVolume(uvw)) break;
        float4 s = gVoxelTex.SampleLevel(gVoxelSamp, uvw, mip);
        acc += (1.0 - acc.a) * s;
        // A FULL footprint per step. Half a diameter samples every voxel of travel twice, and
        // front-to-back compositing is not idempotent -- each sample eats (1 - acc.a) again -- so
        // occlusion compounded at twice the physical rate and every cone came back too dark and too
        // short. Halving the step is only correct with an opacity correction that is not here.
        dist += diameter;
    }
    return acc;
}

// Six cones over the hemisphere: one along the normal, five in a ring. Enough for smooth bounce
// lighting without the cost of a full irradiance gather.
float3 coneTracedIndirect(float3 wpos, float3 N, out float ao) {
    float3 up = abs(N.z) < 0.9 ? float3(0,0,1) : float3(1,0,0);
    float3 T = normalize(cross(up, N)), B = cross(N, T);
    const float aperture = 0.577;              // ~60 degree cone

    // COSINE WEIGHTED, not a flat average.
    //
    // Diffuse irradiance is the integral of incoming radiance times cos(theta), so a cone sixty
    // degrees off the normal delivers about HALF what the one along it does. Averaging the six
    // equally -- which is what this did -- overstates everything arriving at a grazing angle, which
    // is precisely the direction most bounce light comes from in a room. The result was indirect
    // light that was too flat and too strong near walls, in a way that reads as a washed-out ambient
    // term rather than as light that came from somewhere.
    //
    // The weights are the cosines themselves: 1 along the normal, and cos(60 deg) = 0.5 for the ring.
    // Dividing by their SUM rather than by the count keeps the overall level unchanged for a surface
    // seeing uniform radiance, so this redistributes energy without also brightening or darkening
    // the whole image.
    float4 sum = traceCone(wpos, N, aperture);   // weight 1.0, straight up the normal
    float occ = sum.a;
    float wsum = 1.0;
    [unroll] for (int k = 0; k < 5; ++k) {
        float ang = 1.2566 * k;                // 2*pi/5
        float3 d = normalize(N * 0.5 + (T * cos(ang) + B * sin(ang)) * 0.866);
        float w = saturate(dot(N, d));         // the cosine this cone actually subtends
        float4 c = traceCone(wpos, d, aperture);
        sum += c * w; occ += c.a * w; wsum += w;
    }
    sum /= wsum; occ /= wsum;
    ao = saturate(1.0 - occ);
    return sum.rgb * gVoxelParams.y;
}

// The Voxi scene variant. Everything below is LIGHT TRANSPORT, which is the renderer's half of the
// bargain: which direction the sun is, how much of it reaches this point, how much sky the point
// sees, and what bounced onto it. The material is asked to shade that; Voxi never reads a field of
// AverSurface and never writes a BRDF term.
//
// Every radiance handed over is raw - the material owns the diffuse response.
float4 PSMainVoxi(VSOut i) : SV_TARGET {
    float3 N = normalize(i.nrmWS);
    float3 L = normalize(gLightDir.xyz);
    float ndl = saturate(dot(N, L));
#if AVER_RT
    // gShadowParams.z is the feature's "I built the acceleration structure for THIS frame" flag.
    // The RayQuery pipeline can be selected before the first build (the replay list runs a frame
    // behind) or while no draw yielded a usable BLAS, and tracing a structure nothing filled
    // reports no hit everywhere: a fully lit scene, with nothing for the debug layer to say.
    // The fallback therefore lives here, where the fact is known, rather than in a pipeline choice
    // the backend would have to keep in step.
    float sunVis;
    if (gShadowParams.z > 0.5) sunVis = rtShadow(i.wpos, N, L);   // exact ray-traced occlusion
    else                       sunVis = shadowFactor(i.wpos, N, ndl); // cascaded shadow map + PCF
#else
    const float sunVis = shadowFactor(i.wpos, N, ndl);   // cascaded shadow map + PCF
#endif
    // Voxi indirect bounce: cone-traced diffuse GI + the ambient occlusion that falls out of it.
    float ao = 1.0;
    float3 ind = 0;
    if (gVoxelParams.w > 0.5) ind = coneTracedIndirect(i.wpos, N, ao);

    AverVertex vtx = averVertexOf(i);
    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    sun.visibility = sunVis;

    AverSurface s = averEvalMaterial(vtx, sun);
    float4 display;
    if (averDisplayColour(s, display)) return display;   // authored colour, bypasses camera post

    // The sky hemisphere is light transport too, so Voxi supplies it rather than leaving the BRDF
    // to reach into the engine constants for it. With GI off this IS the whole indirect term.
    // R is built from the SHADING normal -- the one the material perturbed with its normal map --
    // and not from the geometric one. Reflecting about the geometric normal means every normal map
    // in the scene is invisible to the environment: a bumped surface reflects as though it were
    // flat while its diffuse shading says otherwise, which reads as the bump being "painted on".
    float3 V = normalize(gCamPos.xyz - i.wpos);
    float3 R = reflect(-V, averShadingNormal(s));
    AverIndirect ind4;
    // The cosine-weighted hemisphere integral of the sky dome, not one tap along the normal. See
    // averSkyIrradiance: a single sample is the radiance from ONE direction standing in for the
    // integral of every direction the surface can see, and for a two-colour dome it is wrong by up
    // to the full horizon-to-zenith difference on any surface that is not facing straight up.
    ind4.ambient      = averSkyIrradiance(averShadingNormal(s));
    ind4.ambientScale = gAmbient.r;
    ind4.diffuse      = ind;
    ind4.occlusion    = ao;
    // Specular indirect: with GI on, a single cone along the reflection vector reads the SCENE's own
    // bounced radiance out of the voxel volume — the floor mirrored in a metal, the cube reflected in the
    // ground — instead of only the sky. The aperture opens with roughness (a mirror stays tight, a rough
    // surface blurs), and where the cone leaves the volume (coverage < 1) it fades back to the sky so a
    // reflection off the top of the world is sky, not black. With GI off it is the sky reflection, as before.
    if (gVoxelParams.w > 0.5) {
        float  specAperture = clamp(s.rough * 0.5 + 0.02, 0.02, 0.4);
        float4 sceneSpec    = traceCone(i.wpos, R, specAperture);
        // ADDED, not lerped. traceCone returns PREMULTIPLIED radiance -- rgb is already weighted by
        // the coverage in a -- so feeding it to lerp() with its own alpha as the blend factor
        // multiplies the coverage in twice and darkens every partially-covered reflection by that
        // factor again. The sky fills exactly the uncovered remainder.
        ind4.specular       = sceneSpec.rgb * gVoxelParams.y + skyColor(R) * (1.0 - sceneSpec.a);
    } else {
        ind4.specular       = skyColor(R);
    }

    float3 radiance = 0.0;
    radiance = averShadeDirect(radiance, s, sun);
    radiance = averShadeIndirect(radiance, s, ind4);
    radiance = averApplyFog(radiance, i.wpos);
    // LINEAR RADIANCE. The scene target is HDR and the frame is tonemapped once, at the end of the
    // post chain — see PostSettings. Tonemapping here would clamp exactly the above-white range
    // that bloom and eye adaptation exist to read, and would then be tonemapped a second time.
    return float4(radiance, averOpacity(s));
}

// Depth-only pass from the sun's point of view (VSIn comes from the prelude). Runs once per
// cascade; which one is in gShadowDraw.x, and the viewport confines the output to that cascade's
// quadrant of the atlas.
float4 VSShadow(VSIn i) : SV_POSITION {
    return mul(mul(float4(i.pos, 1.0), gWorld), gCascadeViewProj[(uint)gShadowDraw.x]);
}

// ================= Voxi: voxelisation =================
// The scene is rasterised once per frame with no render target; the pixel shader computes direct
// lighting and writes radiance straight into the 3D volume. Merging "voxelise" and "inject light"
// into one pass avoids a second full scene traversal.
struct VoxOut { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : NORMAL; float2 uv : TEXCOORD1; };

// The voxelisation adapter. V is EXACTLY zero because there is no camera here: the voxelise
// vertex/mesh shaders write a dominant-axis projection, so any fabricated view vector would inject
// view-dependent specular into a volume the cone trace reads from every direction. Materials read
// it through averDiffuseAlbedo, which is contractually view-independent.
// Deliberately NOT an overload of the material prelude's averVertexOf(VSOut). FXC resolves overloads
// by implicit conversion between structurally compatible types, so a second averVertexOf makes EVERY
// call ambiguous — `error X3067`, on every shader in the translation unit, including the ones that
// never call it. DXC is stricter and accepted it, so the SM 5.1 path was the only thing that could
// find this. A distinct name is the whole fix, and it costs nothing.
AverVertex voxelVertexOf(VoxOut i) {
    AverVertex v;
    v.wpos = i.wpos;
    v.N    = normalize(i.nrm);
    v.V    = float3(0, 0, 0);
    v.uv   = i.uv;
    return v;
}

VoxOut VSVoxel(VSIn i) {
    VoxOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos = wp.xyz;
    o.nrm  = mul(float4(i.nrm, 0.0), gWorld).xyz;
    o.uv   = i.uv;
    o.pos  = wp;                     // world space; the GS picks a projection axis
    return o;
}

// Project each triangle along its dominant axis so it covers the most pixels (and therefore the
// most voxels). The voxel index is recomputed from world position in the PS, so the choice of
// axis does not affect correctness - only coverage.
[maxvertexcount(3)]
void GSVoxel(triangle VoxOut inp[3], inout TriangleStream<VoxOut> os) {
    float3 n = abs(cross(inp[1].wpos - inp[0].wpos, inp[2].wpos - inp[0].wpos));
    int axis = (n.x > n.y && n.x > n.z) ? 0 : ((n.y > n.z) ? 1 : 2);
    [unroll] for (int k = 0; k < 3; ++k) {
        VoxOut o = inp[k];
        float3 v = voxelUVW(o.wpos);
        float2 p = (axis == 0) ? v.yz : ((axis == 1) ? v.xz : v.xy);
        o.pos = float4(p * 2.0 - 1.0, 0.5, 1.0);
        os.Append(o);
    }
}

#if AVER_MS
// Voxelisation without a geometry shader. The dominant-axis choice GSVoxel made per primitive is
// made here instead - the mesh shader is already per-primitive, so the GS stage disappears. That
// matters because GS is emulated on every AMD GCN part and is markedly slower there.
// MeshVtx / gVerts / gIndices / msTriCount / AVER_MS_TRIS all come from the PRELUDE, at the
// registers the backend binds geometry to.
[numthreads(AVER_MS_TRIS, 1, 1)]
[outputtopology("triangle")]
void MSVoxel(uint gid : SV_GroupID, uint gtid : SV_GroupThreadID,
             out vertices VoxOut verts[AVER_MS_TRIS * 3],
             out indices uint3 tris[AVER_MS_TRIS]) {
    uint count = msTriCount(gid);
    SetMeshOutputCounts(count * 3, count);
    if (gtid >= count) return;

    uint3 idx = gIndices.Load3((gid * AVER_MS_TRIS + gtid) * 12);
    float3 wp[3], nr[3];
    float2 uv[3];
    [unroll] for (uint k = 0; k < 3; ++k) {
        MeshVtx v = gVerts[idx[k]];
        wp[k] = mul(float4(v.pos, 1.0), gWorld).xyz;
        nr[k] = mul(float4(v.nrm, 0.0), gWorld).xyz;
        uv[k] = v.uv;
    }
    float3 n = abs(cross(wp[1] - wp[0], wp[2] - wp[0]));
    int axis = (n.x > n.y && n.x > n.z) ? 0 : ((n.y > n.z) ? 1 : 2);
    uint o = gtid * 3;
    [unroll] for (uint k = 0; k < 3; ++k) {
        float3 v = voxelUVW(wp[k]);
        float2 p = (axis == 0) ? v.yz : ((axis == 1) ? v.xz : v.xy);
        VoxOut ov;
        ov.wpos = wp[k];
        ov.nrm  = nr[k];
        ov.uv   = uv[k];
        ov.pos  = float4(p * 2.0 - 1.0, 0.5, 1.0);
        verts[o + k] = ov;
    }
    tris[gtid] = uint3(o, o + 1, o + 2);
}
#endif // AVER_MS

void PSVoxel(VoxOut i) {
    float3 uvw = voxelUVW(i.wpos);
    if (!insideVolume(uvw)) return;
    float3 N = normalize(i.nrm);
    float3 L = normalize(gLightDir.xyz);
    // SHADOWED injection: a surface in shadow must not emit sun radiance into the volume, or the
    // bounce lighting leaks through walls and shadowed areas glow.
    float ndl = saturate(dot(N, L));

    // The albedo injected here is the SAME albedo the lit pass shades with, because it is asked for
    // rather than recomputed: two copies of srgbToLin(gBaseColor.rgb) agreed only for as long as
    // nobody touched one of them, and the day a material gains a base-colour map the copy that was
    // not updated would light the scene with the wrong bounce colour and no gate would say so.
    //
    // averDiffuseAlbedo is contractually view-independent, which is what makes this legal: there is
    // no camera in a voxelisation pass, so the vertex adapter hands the material a zero view vector
    // and any view-dependent term would be meaningless here.
    AverLight sun;
    sun.direction  = L;
    // The SAME radiance the lit pass shades with. It used to be this expression without the factor
    // of three the lit pass applied, so the radiance injected into the volume -- and therefore every
    // bounce, every bit of colour bleeding, all of the indirect light -- was a third of the light
    // that actually fell on the surface.
    sun.radiance   = averSunRadiance();
    sun.visibility = shadowFactor(i.wpos, N, ndl);
    AverSurface s = averEvalMaterial(voxelVertexOf(i), sun);
    float3 albedo = averDiffuseAlbedo(s);
    // EXITANT RADIANCE, which is what a cone gathering this voxel later must read -- not radiosity.
    // A Lambertian surface receiving irradiance E reflects albedo * E / PI in every direction. The
    // sun term is an irradiance (radiance times the cosine), so it needs the 1/PI; the sky term is
    // already a radiance times a scalar, so it does not. Without the split, the sun addend was PI
    // times too large AND the sky addend PI times too small relative to it -- the bounce was both
    // over-bright and the wrong colour, sunward far more than skyward.
    float3 radiance = albedo * (sun.radiance * ndl * sun.visibility / PI
                                + averSkyIrradiance(N) * gAmbient.r);
    // Bounded before it is quantised so a pathological light colour cannot overflow the 32-bit
    // accumulator; nothing in a physically sane scene comes close to this.
    radiance = clamp(radiance, 0.0, AVER_VOX_MAXRAD);

    // insideVolume() is INCLUSIVE of 1.0, so a fragment landing exactly on the far face of the
    // volume truncates to index `res`, one past the last cell. Conservative rasterisation is what
    // makes that reachable: it generates fragments for pixels the triangle only partly covers and
    // extrapolates their attributes to the pixel CENTRE, so a mesh whose extent matches the volume's
    // own bounds does produce uvw == 1.0 exactly. A typed-UAV write out of bounds is discarded by the
    // hardware, which is why this was invisible on RDNA3. Clamp rather than reject: the fragment's
    // radiance genuinely belongs to the last cell.
    uint3 c = min(uint3(uvw * gVoxelParams.x), (uint)gVoxelParams.x - 1);
    uint3 a = uint3(c.x * 4, c.y, c.z);
    uint prev;
    InterlockedAdd(gVoxelAccum[a],                (uint)(radiance.r * AVER_VOX_FIXED), prev);
    InterlockedAdd(gVoxelAccum[a + uint3(1,0,0)], (uint)(radiance.g * AVER_VOX_FIXED), prev);
    InterlockedAdd(gVoxelAccum[a + uint3(2,0,0)], (uint)(radiance.b * AVER_VOX_FIXED), prev);
    InterlockedAdd(gVoxelAccum[a + uint3(3,0,0)], 1u, prev);   // fragments covering this voxel
}

// Zero the accumulator before injection. Without it a voxel covered on one frame keeps its sum for
// ever and moving geometry drags a radiance trail behind it. Mip 0 of the volume itself needs no
// clear any more: CSResolve writes every cell unconditionally, and CSMip overwrites every coarser
// level.
[numthreads(4,4,4)]
void CSClear(uint3 id : SV_DispatchThreadID) {
    uint3 a = uint3(id.x * 4, id.y, id.z);
    [unroll] for (uint k = 0; k < 4; ++k) gVoxelAccum[a + uint3(k,0,0)] = 0;
}

// Turn the fixed-point sums into the filterable RGBA16F volume the cone trace samples. Dividing by
// the fragment count makes a contested voxel hold the mean radiance of the surfaces covering it,
// which is both order-independent and a better answer than an arbitrary winner: a voxel straddling
// the floor and the cube now reads as a blend rather than flickering between the two.
[numthreads(4,4,4)]
void CSResolve(uint3 id : SV_DispatchThreadID) {
    uint3 a = uint3(id.x * 4, id.y, id.z);
    uint n = gVoxelAccum[a + uint3(3,0,0)];
    if (n == 0) { gVoxelUAV[id] = 0.0; return; }
    float3 s = float3(gVoxelAccum[a], gVoxelAccum[a + uint3(1,0,0)], gVoxelAccum[a + uint3(2,0,0)]);
    gVoxelUAV[id] = float4(s / (AVER_VOX_FIXED * (float)n), 1.0);   // alpha = occupancy
}

// ================= Voxi: mip filtering =================
// Box-filter one mip into the next. Averaging radiance AND occupancy is what lets a wide cone step
// read a single blurry sample instead of marching every voxel. The mip binding set puts a
// SINGLE-MIP view of the source level at t0 and the destination level at u0, so gSrcMip indexes
// within that view and the two levels can be read and written at once.
[numthreads(4,4,4)]
void CSMip(uint3 id : SV_DispatchThreadID) {
    int3 s = int3(id) * 2;
    float4 a = 0;
    [unroll] for (int x=0;x<2;++x)
    [unroll] for (int y=0;y<2;++y)
    [unroll] for (int z=0;z<2;++z)
        a += gVoxelTex.Load(int4(s + int3(x,y,z), gSrcMip));
    gVoxelUAV[id] = a * 0.125;
}

// Debug: raymarch the volume straight to screen so voxelisation can be inspected on its own.
// Shares the prelude's fullscreen-triangle vertex shader (VSky) and its SkyOut.
float4 PSVoxelDebug(SkyOut i) : SV_TARGET {
    float4 far = mul(float4(i.ndc, 1.0, 1.0), gInvViewProj);
    float3 ray = normalize(far.xyz / far.w - gCamPos.xyz);
    float voxelWorld = 1.0 / (gVoxelOrigin.w * gVoxelParams.x);
    float4 acc = 0;
    float t = 0;
    [loop] for (int s = 0; s < 256; ++s) {
        if (acc.a >= 0.98) break;
        float3 uvw = voxelUVW(gCamPos.xyz + ray * t);
        t += voxelWorld;
        if (t > gVoxelParams.z * 2.0) break;
        if (!insideVolume(uvw)) continue;
        float4 v = gVoxelTex.SampleLevel(gVoxelSamp, uvw, 0);
        acc += (1.0 - acc.a) * v;
    }
    float3 bg = skyColor(ray);
    float3 col = acc.rgb + bg * (1.0 - acc.a);
    return float4(col, 1.0);   // linear; the post chain tonemaps (see PSLit above)
}
)";

} // namespace aver::voxi
