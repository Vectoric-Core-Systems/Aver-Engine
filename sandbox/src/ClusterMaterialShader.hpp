#pragma once
// The pixel shader for the GPU per-cluster mesh-shader path, with materials, and (Stage 3, when
// AVER_MODULE_VOXI is compiled in) real shadows and real GI.
//
// WHY THIS IS NOT IN THE SHARED PRELUDE, where its predecessor lived. rhi::sharedShaderPrelude()
// is compiled FIRST, before pbr::materialShaderPrelude(), so anything written inside it cannot call
// averEvalMaterial -- the material functions do not exist yet at that point in the translation
// unit. The old PSClusterMain was a one-liner into plainShadeSurface for exactly that reason, and
// that is why every plant on this path drew as a black silhouette: plainShadeSurface shades from
// gBaseColor/gMaterial, per-object constants, and never samples a texture.
//
// So this source lives with the feature that owns the pipeline and is appended AFTER both preludes,
// which is the composition order PbrShaders.hpp documents and the one Voxi already uses for
// VoxiShaders.hpp.
//
// THE HALF STAGE 3 DID NOT BUY, NOW CLOSED -- BUT NOT FROM THIS FILE. This shader SAMPLES the cascade
// map and the voxel volume, and it does. The geometry it draws USED TO be absent FROM both, because
// IRenderFeature::submitDraw is called from exactly one place -- D3D12Device::drawMesh -- and this
// path dispatches clusters itself and skips that call by design. A cluster-drawn plant RECEIVED
// shadow and GI and CAST neither. That was a scene-submission gap, not a shading one, so nothing in
// this file could fix it -- and nothing in this file did: SandboxApp.cpp's cluster-dispatch branch now
// calls VoxiRenderer::submit() itself, right after dispatchMeshClusters (search "DEFECT 2's FIX" in
// SandboxApp.cpp), handing Voxi the same (mesh, world, material) shape any ordinary drawMesh() instance
// already does. That gets a cheap depth-only proxy of this geometry into the shadow and voxelise
// passes without this pixel shader, or PSClusterMain's own lit-pass shading, changing at all.
//
// WHAT IT DELIBERATELY DOES NOT DO, STILL, EVEN AFTER STAGE 3. No ray tracing, ever -- shadowFactor()
// and coneTracedIndirect() (borrowed from Voxi via VoxiGiShaders.hpp's giShaderPrelude(), merged
// into this pipeline's own table 0 rather than needing a third -- see D3D12Device.cpp's nullFill for
// why that merge is D3D12 only) are Voxi's OWN non-ray-traced fallback path, the same one PSMainVoxi
// itself runs when ray tracing is off or its acceleration structure is not built. On a project that
// DOES have ray tracing on, ordinary draws get ray-traced shadows/reflections and cluster-path draws
// still get the cascade map and the voxel cone -- a real, visible difference, not merely an
// unmeasured one. And without AVER_MODULE_VOXI compiled in at all, this file still has no shadow
// lookup and no GI cone trace to fall back on: sun.visibility is 1.0 and the indirect diffuse term
// is zero, exactly Stage 2's neutral stand-in. Either way, the result is textured, sun-lit, sky-
// ambient foliage that is closer to the ordinary path than Stage 2 left it, and still NOT full
// parity with it -- see the header comment on lodMeshShaderEnabled_.
#include <string_view>

namespace aver::sandbox {

// Compiled as: rhi::sharedShaderPrelude() + pbr::materialShaderPrelude() +
// (voxi::giShaderPrelude(), only when AVER_MODULE_VOXI) + this. Every symbol used below comes from
// one of those, and none of them sit behind a feature macro of their own: averSunRadiance/
// averSkyIrradiance/skyColor/averApplyFog and VSOut are unguarded in the shared prelude, the aver*
// material entry points are unguarded in the material prelude, and shadowFactor()/
// coneTracedIndirect() are unguarded in Voxi's borrowed one -- AVER_CLUSTER_VOXI below is what
// SandboxApp.cpp defines only when that third prelude was actually appended, so this file is the
// one place that decides whether to call them at all.
inline constexpr std::string_view kClusterMaterialPS = R"HLSL(
// ================= per-cluster mesh-shader path: the lit pixel shader =================
//
// The same shading sequence PSMainVoxi runs -- averEvalMaterial, then direct, then indirect, then
// fog. With AVER_CLUSTER_VOXI, sun.visibility and the indirect diffuse term come from the SAME
// shadowFactor()/coneTracedIndirect() calls PSMainVoxi's own non-ray-traced fallback makes; without
// it (no Voxi module compiled in), both stay the neutral values Stage 2 shipped.
float4 PSClusterMain(VSOut i) : SV_TARGET {
    AverVertex vtx = averVertexOf(i);
    // AVER_CLUSTER_PS_DEBUG isolates one term at a time when this path renders wrong. It stays
    // because it earned its keep: it is what found the black foliage, and the answer turned out NOT
    // to be in this file at all.
    //
    //   THE BUG WAS ON THE C++ SIDE. dispatchMeshClusters applies the CONTEXT's sticky per-draw
    //   binding, and the caller was setting the DEVICE's -- which only reaches the context from
    //   inside drawMesh(), a call this path never makes. Every draw here therefore ran with some
    //   earlier draw's material still bound. See the fix at SandboxApp's `if (matSet)
    //   ctx->setDrawBinding(...)`, right beside the dispatch.
    //
    // WHY THE LADDER READ AS INNOCENT ALL THE WAY DOWN, which is the part worth remembering: the
    // wrong material was a REAL material, so nothing looked broken until the very end.
    //   1 = raw base-colour texture             -> textured brown. True, and someone else's texture.
    //   2 = gBaseColorFactor (b2 constants)     -> white. True, and someone else's constants.
    //   3 = albedo after averEvalMaterial       -> plausible brown. Same.
    //   4 = N.L on the RAW normal, sun radiance -> both good. Genuinely fine, and never the problem.
    //   5 = direct | +indirect | +fog stripes   -> ALL BLACK, so averShadeDirect was already zero.
    //   6 = kdAlbedo | N.L perturbed | normal   -> kdAlbedo BLACK. THIS is the one that pinned it:
    //       kdAlbedo = (1 - metallic) * albedo, so metallic had resolved to 1 and killed the whole
    //       diffuse lobe, leaving only a specular term that at this roughness reads as black.
    //   7 = gMaterial.x | gMetallicFactor | metalRough.y -- which factor is the 1. Never needed:
    //       gMaterial.x is deliberately 1 (a neutral multiplier, see `metallic = roughness = 1.0f`),
    //       and the other two came from the stale binding, whose metal-rough map is white.
    //
    // The lesson for the next reader: an input that measures CORRECT is not the same as an input
    // that is the RIGHT ONE. Four modes confirmed live plausible values from the wrong source.
#if AVER_CLUSTER_PS_DEBUG == 1
    return float4(averSampleMaps(averSurfaceUV(vtx)).baseColor.rgb, 1.0);
#elif AVER_CLUSTER_PS_DEBUG == 2
    return float4(gBaseColorFactor.rgb, 1.0);
#elif AVER_CLUSTER_PS_DEBUG == 3
    return float4(averEvalMaterial(vtx, (AverLight)0).albedo, 1.0);
#elif AVER_CLUSTER_PS_DEBUG == 5
    // Every stage of the shade in ONE image, as 200px vertical stripes, because each of these
    // costs a three-minute build-and-capture to test on its own and all the INPUTS have already
    // been cleared. Stripe 0 direct only, stripe 1 direct+indirect, stripe 2 the full result with
    // fog -- whichever stripe first goes black names the function that is eating it.
    {
        // Its own light, because the real `sun` is declared below this ladder.
        AverLight dsun;
        dsun.direction  = normalize(gLightDir.xyz);
        dsun.radiance   = averSunRadiance();
        dsun.visibility = 1.0;

        AverSurface sd = averEvalMaterial(vtx, dsun);
        AverIndirect id;
        id.ambient      = averSkyIrradiance(averShadingNormal(sd));
        id.ambientScale = gAmbient.r;
        id.diffuse      = float3(0, 0, 0);
        id.occlusion    = 1.0;
        id.specular     = skyColor(reflect(-sd.V, averShadingNormal(sd)));

        float3 a = averShadeDirect(float3(0, 0, 0), sd, dsun);
        float3 b = averShadeIndirect(a, sd, id);
        float3 c = averApplyFog(b, i.wpos);

        uint band = (uint(i.pos.x) / 200u) % 3u;
        if (band == 0u) return float4(a, 1.0);
        if (band == 1u) return float4(b, 1.0);
        return float4(c, 1.0);
    }
#elif AVER_CLUSTER_PS_DEBUG == 6
    // averShadeDirect is `(kdAlbedo/PI + spec) * radiance * ndl * visibility`, and stripe 0 of
    // debug 5 proved it returns zero even though albedo, sun radiance and the RAW normal are all
    // good. Only two factors are left, and debug 4 tested the wrong normal: it used vtx.N, while
    // the shade uses s.N from averPerturbNormal, which builds a tangent basis out of ddx/ddy.
    //   stripe 0: kdAlbedo -- black means metallic resolved to 1 and killed the diffuse lobe
    //   stripe 1: N.L using the PERTURBED normal -- black means averPerturbNormal is the culprit
    //   stripe 2: the perturbed normal itself, remapped -- flat/black means it is degenerate
    {
        AverLight dl;
        dl.direction  = normalize(gLightDir.xyz);
        dl.radiance   = averSunRadiance();
        dl.visibility = 1.0;
        AverSurface sd = averEvalMaterial(vtx, dl);

        uint band = (uint(i.pos.x) / 200u) % 3u;
        if (band == 0u) return float4(sd.kdAlbedo, 1.0);
        if (band == 1u) return float4(saturate(dot(sd.N, dl.direction)).xxx, 1.0);
        return float4(sd.N * 0.5 + 0.5, 1.0);
    }
#elif AVER_CLUSTER_PS_DEBUG == 7
    // kdAlbedo came back zero, so metallic resolved to 1 and killed the diffuse lobe entirely.
    // It is a product of three things -- which one is 1?
    //   s.metallic = saturate(gMaterial.x * gMetallicFactor * map.metalRough.y)
    //   stripe 0: gMaterial.x    -- PerObject, the neutral multiplier the caller packs at dword 20
    //   stripe 1: gMetallicFactor -- the material constant from b2
    //   stripe 2: map.metalRough.y -- the ARM texture's blue channel
    {
        uint band = (uint(i.pos.x) / 200u) % 3u;
        if (band == 0u) return float4(gMaterial.xxx, 1.0);
        if (band == 1u) return float4(gMetallicFactor.xxx, 1.0);
        return float4(averSampleMaps(averSurfaceUV(vtx)).metalRough.yyy, 1.0);
    }
#elif AVER_CLUSTER_PS_DEBUG == 4
    // Albedo proved good, so the loss is in the lighting inputs. Both remaining candidates in one
    // image: RED is N.L (geometry -- is the surface facing the sun at all), GREEN is whether the
    // sun carries any radiance (the PerFrame constants). Black means both are zero.
    {
        float3 dbgN = normalize(vtx.N);
        float3 dbgL = normalize(gLightDir.xyz);
        float  ndl  = saturate(dot(dbgN, dbgL));
        float  lit  = any(averSunRadiance() > 0.0) ? 1.0 : 0.0;
        return float4(ndl, lit, 0.0, 1.0);
    }
#endif

    AverLight sun;
    sun.direction  = normalize(gLightDir.xyz);
    sun.radiance   = averSunRadiance();
#if AVER_CLUSTER_VOXI
    // THE REAL SHADOW LOOKUP: Voxi's own shadowFactor(), borrowed via VoxiGiShaders.hpp at whatever
    // base register ensureLodMeshPipeline's merge gave it (see its register-map comment). Bias uses
    // vtx.N -- the geometric normal AverVertex already resolved (normalized, and flipped for
    // two-sided shading; see averVertexOf's own comment) -- rather than the SHADING normal computed
    // below from averEvalMaterial, because shadowFactor's bias offsets along the surface the shadow
    // map was rendered against, not the normal-mapped one. gShadowParams.y (no atlas yet / shadows
    // off) and running out of cascades both degrade shadowFactor() to 1.0 internally -- this call
    // never needs to re-check either.
    sun.visibility = shadowFactor(vtx.wpos, vtx.N, saturate(dot(vtx.N, sun.direction)));
#else
    // NO SHADOW MAP ON THIS PATH. 1.0 means "fully lit", which is what the old one-liner passed
    // too -- the black foliage was never a shadow problem, it was a missing albedo. True whenever
    // this file was compiled without Voxi's GI/shadow prelude appended (AVER_MODULE_VOXI off).
    sun.visibility = 1.0;
#endif

    AverSurface s = averEvalMaterial(vtx, sun);

    // Unlit debug views (albedo-only, normal-only and friends) short-circuit here exactly as they
    // do on the ordinary path, so a debug mode reads the same on both.
    float4 display;
    if (averDisplayColour(s, display)) return display;

    float3 N = averShadingNormal(s);
    float3 V = normalize(gCamPos.xyz - i.wpos);

    AverIndirect ind;
    ind.ambient      = averSkyIrradiance(N);
    ind.ambientScale = gAmbient.r;
#if AVER_CLUSTER_VOXI
    // THE REAL GI CONE TRACE: Voxi's own coneTracedIndirect(), gated on gVoxelParams.w exactly the
    // way PSMainVoxi's own call site gates it (coneTracedIndirect itself does not check) -- so a
    // volume that has not been built yet, or GI that is off in settings, degrades to the SAME
    // sky-irradiance-only look this path already had before Stage 3, not to a black or garbage one.
    if (gVoxelParams.w > 0.5) {
        float ao;
        ind.diffuse   = coneTracedIndirect(vtx.wpos, N, ao);
        ind.occlusion = ao;
    } else {
        ind.diffuse   = float3(0, 0, 0);
        ind.occlusion = 1.0;
    }
#else
    // NO VOXEL CONE TRACE. Zero rather than an invented approximation: a wrong indirect term is
    // harder to spot than a missing one, and the sky irradiance above already keeps shadowed sides
    // from going black. True whenever this file was compiled without Voxi's prelude appended.
    ind.diffuse      = float3(0, 0, 0);
    ind.occlusion    = 1.0;
#endif
    ind.specular     = skyColor(reflect(-V, N));

    float3 radiance = 0.0;
    radiance = averShadeDirect(radiance, s, sun);
    radiance = averShadeIndirect(radiance, s, ind);
    radiance = averApplyFog(radiance, i.wpos);
    return float4(radiance, averOpacity(s));
}
)HLSL";

} // namespace aver::sandbox
