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
// recomputes only these (the shadow matrix is not known until the shadow pass runs) re-uploads 112
// bytes instead of the whole engine block.
cbuffer VoxiFrame : register(b4) {
    float4   gVoxelOrigin; // xyz = volume min corner, w = 1/volumeWorldSize
    float4   gVoxelParams; // x = resolution, y = intensity, z = maxDistance, w = enabled|debug<<1
    float4x4 gLightViewProj;
    float4   gShadowParams; // x = 1/shadowMapSize, y = enabled, z = acceleration structure built
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

// 3x3 PCF. Returns 1 = fully lit, 0 = fully shadowed.
float shadowFactor(float3 wpos, float ndl) {
    if (gShadowParams.y < 0.5) return 1.0;
    float4 lp = mul(float4(wpos, 1.0), gLightViewProj);
    float3 p = lp.xyz / lp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0) return 1.0;  // outside the map = lit
    float bias = max(0.0015 * (1.0 - ndl), 0.0003);               // depth bias, light-space units
    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        s += gShadowTex.SampleCmpLevelZero(gShadowSamp, uv + float2(x, y) * gShadowParams.x, p.z - bias);
    return s / 9.0;
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
        dist += diameter * 0.5;
    }
    return acc;
}

// Six cones over the hemisphere: one along the normal, five in a ring. Enough for smooth bounce
// lighting without the cost of a full irradiance gather.
float3 coneTracedIndirect(float3 wpos, float3 N, out float ao) {
    float3 up = abs(N.z) < 0.9 ? float3(0,0,1) : float3(1,0,0);
    float3 T = normalize(cross(up, N)), B = cross(N, T);
    const float aperture = 0.577;              // ~60 degree cone
    float4 sum = traceCone(wpos, N, aperture);
    float occ = sum.a;
    [unroll] for (int k = 0; k < 5; ++k) {
        float ang = 1.2566 * k;                // 2*pi/5
        float3 d = normalize(N * 0.5 + (T * cos(ang) + B * sin(ang)) * 0.866);
        float4 c = traceCone(wpos, d, aperture);
        sum += c; occ += c.a;
    }
    sum /= 6.0; occ /= 6.0;
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
    else                       sunVis = shadowFactor(i.wpos, ndl); // shadow map + PCF
#else
    const float sunVis = shadowFactor(i.wpos, ndl);       // shadow map + PCF
#endif
    // Voxi indirect bounce: cone-traced diffuse GI + the ambient occlusion that falls out of it.
    float ao = 1.0;
    float3 ind = 0;
    if (gVoxelParams.w > 0.5) ind = coneTracedIndirect(i.wpos, N, ao);

    AverVertex vtx = averVertexOf(i);
    AverLight sun;
    sun.direction  = L;
    sun.radiance   = srgbToLin(gLightColor.rgb) * 3.0;   // sun radiance
    sun.visibility = sunVis;

    AverSurface s = averEvalMaterial(vtx, sun);
    float4 display;
    if (averDisplayColour(s, display)) return display;   // authored colour, bypasses camera post

    // The sky hemisphere is light transport too, so Voxi supplies it rather than leaving the BRDF
    // to reach into the engine constants for it. With GI off this IS the whole indirect term.
    float3 V = normalize(gCamPos.xyz - i.wpos);
    AverIndirect ind4;
    ind4.ambient      = skyColor(N);
    ind4.ambientScale = gAmbient.r;
    ind4.diffuse      = ind;
    ind4.occlusion    = ao;
    ind4.specular     = skyColor(reflect(-V, N));

    float3 radiance = 0.0;
    radiance = averShadeDirect(radiance, s, sun);
    radiance = averShadeIndirect(radiance, s, ind4);
    radiance = averApplyFog(radiance, i.wpos);
    return float4(toGamma(acesTonemap(radiance)), averOpacity(s));
}

// Depth-only pass from the sun's point of view (VSIn comes from the prelude).
float4 VSShadow(VSIn i) : SV_POSITION {
    return mul(mul(float4(i.pos, 1.0), gWorld), gLightViewProj);
}

// ================= Voxi: voxelisation =================
// The scene is rasterised once per frame with no render target; the pixel shader computes direct
// lighting and writes radiance straight into the 3D volume. Merging "voxelise" and "inject light"
// into one pass avoids a second full scene traversal.
struct VoxOut { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : NORMAL; };

// The voxelisation adapter. V is EXACTLY zero because there is no camera here: the voxelise
// vertex/mesh shaders write a dominant-axis projection, so any fabricated view vector would inject
// view-dependent specular into a volume the cone trace reads from every direction. Materials read
// it through averDiffuseAlbedo, which is contractually view-independent.
AverVertex averVertexOf(VoxOut i) {
    AverVertex v;
    v.wpos = i.wpos;
    v.N    = normalize(i.nrm);
    v.V    = float3(0, 0, 0);
    return v;
}

VoxOut VSVoxel(VSIn i) {
    VoxOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos = wp.xyz;
    o.nrm  = mul(float4(i.nrm, 0.0), gWorld).xyz;
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
    [unroll] for (uint k = 0; k < 3; ++k) {
        MeshVtx v = gVerts[idx[k]];
        wp[k] = mul(float4(v.pos, 1.0), gWorld).xyz;
        nr[k] = mul(float4(v.nrm, 0.0), gWorld).xyz;
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
    sun.radiance   = srgbToLin(gLightColor.rgb);
    sun.visibility = shadowFactor(i.wpos, ndl);
    AverSurface s = averEvalMaterial(averVertexOf(i), sun);
    float3 albedo = averDiffuseAlbedo(s);
    float3 radiance = albedo * (sun.radiance * ndl * sun.visibility
                                + skyColor(N) * gAmbient.r);
    // Bounded before it is quantised so a pathological light colour cannot overflow the 32-bit
    // accumulator; nothing in a physically sane scene comes close to this.
    radiance = clamp(radiance, 0.0, AVER_VOX_MAXRAD);

    uint3 c = uint3(uvw * gVoxelParams.x);
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
    return float4(toGamma(acesTonemap(col)), 1.0);
}
)";

} // namespace aver::voxi
