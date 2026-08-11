#pragma once
// The pixel shader for the GPU per-cluster mesh-shader path, with materials.
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
// WHAT IT DELIBERATELY DOES NOT DO. No shadow-map lookup and no GI cone trace: sun.visibility is
// 1.0 and the indirect diffuse term is zero. Those live in Voxi's own SRV table, and this pipeline
// binds the cluster buffers in table 0 and the material textures in table 1 -- both tables the RHI
// has. Reaching Voxi's volume and cascade atlas as well would need a third, which
// rhi::kBindingTableCount does not offer. The result is textured, sun-lit, sky-ambient foliage:
// correct in albedo and correct in direct light, but unshadowed. That is a large step up from black
// and still NOT parity with the ordinary path -- see the header comment on lodMeshShaderEnabled_.
#include <string_view>

namespace aver::sandbox {

// Compiled as: rhi::sharedShaderPrelude() + pbr::materialShaderPrelude() + this.
// Every symbol used below comes from one of those two, and none of them sit behind a feature
// macro: averSunRadiance/averSkyIrradiance/skyColor/averApplyFog and VSOut are unguarded in the
// shared prelude, the aver* material entry points are unguarded in the material prelude.
inline constexpr std::string_view kClusterMaterialPS = R"HLSL(
// ================= per-cluster mesh-shader path: the lit pixel shader =================
//
// The same shading sequence PSMainVoxi runs -- averEvalMaterial, then direct, then indirect, then
// fog -- with the two terms this pipeline cannot reach held at their neutral values.
float4 PSClusterMain(VSOut i) : SV_TARGET {
    AverVertex vtx = averVertexOf(i);

    AverLight sun;
    sun.direction  = normalize(gLightDir.xyz);
    sun.radiance   = averSunRadiance();
    // NO SHADOW MAP ON THIS PATH. 1.0 means "fully lit", which is what the old one-liner passed
    // too -- the black foliage was never a shadow problem, it was a missing albedo.
    sun.visibility = 1.0;

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
    // NO VOXEL CONE TRACE. Zero rather than an invented approximation: a wrong indirect term is
    // harder to spot than a missing one, and the sky irradiance above already keeps shadowed sides
    // from going black.
    ind.diffuse      = float3(0, 0, 0);
    ind.occlusion    = 1.0;
    ind.specular     = skyColor(reflect(-V, N));

    float3 radiance = 0.0;
    radiance = averShadeDirect(radiance, s, sun);
    radiance = averShadeIndirect(radiance, s, ind);
    radiance = averApplyFog(radiance, i.wpos);
    return float4(radiance, averOpacity(s));
}
)HLSL";

} // namespace aver::sandbox
