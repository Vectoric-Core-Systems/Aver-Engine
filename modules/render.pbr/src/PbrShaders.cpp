// The HLSL half of Aver.Render.PBR.Materials: the BRDF, and the Aver* contract a renderer fills in.
//
// It lives in the material system rather than in the RHI prelude because that is the architecture:
// PBR is a material system, and a renderer is the thing that renders it. The renderer supplies
// visibility and irradiance and calls the shading model; what a surface does with them is here and
// nowhere else. The link line from Aver.Render.Voxi.Renderer to this target IS that statement.
//
// Concatenated AFTER rhi::sharedShaderPrelude(), which owns the constant-buffer layouts, the vertex
// structures, the colour-space helpers and the camera post. Nothing below may be duplicated there:
// the two strings become one translation unit, so a name defined twice is a redefinition error and
// a name defined once but differently is a silent divergence.
#include "aver/pbr/PbrShaders.hpp"

#include "aver/pbr/Material.hpp"   // kTextureSlotCount: one define per slot, and only that many

namespace aver::pbr {

// ---- the C++ counterpart of `cbuffer AverMaterial` below is pbr::MaterialConstants, in
// MaterialGpu.hpp. Field order, field count and total size must match it byte for byte: nothing
// checks this at compile time, and a mismatch shades plausibly with the wrong parameters rather
// than failing. Change one and change the other. ----
//
// The texture declarations are gated because they name registers that only exist in a layout
// declaring a second binding table. Every pipeline compiled without that table -- Voxi's volume
// clear, its mip filter, its resolve -- shares this same string, and a shader naming a register its
// root signature never declared is a pipeline-creation failure, not a warning. AVER_MATERIAL_SRV
// therefore comes from materialShaderDefines(), off the very layout the root signature is built
// from, exactly as the mesh-geometry registers do.
const char* materialShaderPrelude() {
    return R"(
#ifdef AVER_MATERIAL_SRV
#define AVER_MAT_JOIN2(a, b) a##b
#define AVER_MAT_JOIN(a, b) AVER_MAT_JOIN2(a, b)
// Slot order is pbr::TextureSlot's, so a slot index IS its register offset.
Texture2D gBaseColorMap  : register(AVER_MAT_JOIN(t, AVER_MATERIAL_SRV));
Texture2D gMetalRoughMap : register(AVER_MAT_JOIN(t, AVER_MATERIAL_SRV_1));
Texture2D gNormalMap     : register(AVER_MAT_JOIN(t, AVER_MATERIAL_SRV_2));
Texture2D gOcclusionMap  : register(AVER_MAT_JOIN(t, AVER_MATERIAL_SRV_3));
Texture2D gEmissiveMap   : register(AVER_MAT_JOIN(t, AVER_MATERIAL_SRV_4));
#endif

// MIRRORS pbr::MaterialConstants (aver/pbr/MaterialGpu.hpp) FIELD FOR FIELD.
cbuffer AverMaterial : register(b2) {
    float4 gBaseColorFactor;
    float3 gEmissiveFactor;
    float  gMetallicFactor;
    float  gRoughnessFactor;
    float  gNormalScale;
    float  gOcclusionStrength;
    float  gAlphaCutoff;
    uint   gMaterialFlags;
    uint3  _materialPad;
};

// gMaterialFlags bits, mirroring pbr::MaterialFlag.
#define AVER_MAT_BASECOLOR_MAP  (1u << 0)
#define AVER_MAT_METALROUGH_MAP (1u << 1)
#define AVER_MAT_NORMAL_MAP     (1u << 2)
#define AVER_MAT_OCCLUSION_MAP  (1u << 3)
#define AVER_MAT_EMISSIVE_MAP   (1u << 4)
#define AVER_MAT_ALPHA_MASK     (1u << 5)
#define AVER_MAT_ALPHA_BLEND    (1u << 6)
#define AVER_MAT_TWO_SIDED      (1u << 7)

// ---- shading models. The id arrives per draw in gShadingModel and is dispatched by a UNIFORM
// switch, not by a pipeline permutation: the branch is scalar and free, while a permutation would
// multiply every scene pipeline a renderer builds by the number of models. ----
#define AVER_MODEL_STANDARD 0u   // metallic / roughness, Cook-Torrance GGX
#define AVER_MODEL_UNLIT    1u   // authored colour, no lighting and no camera post

// ---- BRDF terms. Private to the material system: the renderer has no business calling these. ----
// f90 is the grazing-angle reflectance. At 1.0 this is the textbook Schlick term.
float3 fresnelSchlick(float ct, float3 F0, float f90){ return F0 + (f90-F0)*pow(saturate(1.0-ct),5.0); }
float distGGX(float ndh, float a){ float a2=a*a; float d=ndh*ndh*(a2-1.0)+1.0; return a2/(PI*d*d+1e-6); }
float geomSchlick(float nd, float k){ return nd/(nd*(1.0-k)+k); }

// ================= the Aver material contract =================
// The split that makes "PBR is a material system, the renderer is the thing that renders it" a
// compilable statement rather than a slogan. The material owns the BRDF; the renderer owns light
// transport (which lights reach this surface, how much sky it sees, what bounced onto it) and the
// camera (fog, tonemap, gamma).
//
// AverSurface is OPAQUE to the renderer. No renderer may read a field of it: everything a renderer
// legitimately needs is a function below, so that the day a shading model gains state there is
// nothing outside the material system to update.

// What the renderer knows about the point being shaded, and all it has to supply.
struct AverVertex {
    float3 wpos;
    float3 N;    // unit surface normal, world space
    float3 V;    // unit vector towards the camera; EXACTLY zero where there is no camera
};

// One light's contribution as the renderer resolved it. `visibility` is the renderer's shadowing
// answer - shadow map, ray query, nothing at all - because which of those applies is a property of
// the renderer, not of the material.
struct AverLight {
    float3 direction;   // unit vector TO the light
    float3 radiance;    // linear radiance arriving along `direction`
    float  visibility;  // 0 = fully occluded, 1 = fully lit
};

// Everything reaching the surface that did not come straight from a light. All of it is RAW
// radiance: the diffuse response is applied by the material, so a renderer that premultiplied it
// would square the albedo - which reads as "GI is a bit dark and oversaturated", never as a bug.
//
// `ambientScale` is a separate field rather than folded into `ambient` on purpose. Float
// multiplication is not associative, and (kd*albedo*ambient)*scale is not bit-identical to
// kd*albedo*(ambient*scale); folding it would silently move an image that has an exact oracle
// behind it.
struct AverIndirect {
    float3 ambient;      // sky-hemisphere irradiance the renderer sampled for this surface
    float  ambientScale; // weight for `ambient`, applied after the diffuse response
    float3 diffuse;      // bounced radiance (zero when the renderer traces none)
    float  occlusion;    // ambient occlusion; weights `ambient` only, never `diffuse`
    float3 specular;     // environment radiance along the reflection vector
};

// Adapter for the geometry the shared vertex/mesh shaders produce. It is the renderer's job to say
// where the camera is, so this lives with the vertex structure rather than with the material.
AverVertex averVertexOf(VSOut i) {
    AverVertex v;
    v.wpos = i.wpos;
    v.N    = normalize(i.nrmWS);
    v.V    = normalize(gCamPos.xyz - i.wpos);
    return v;
}

// The evaluated material at one point. Every field is the material system's business; see the
// note on AverSurface being opaque above.
struct AverSurface {
    float3 N, V, H;
    float3 albedo;       // linear base colour
    float3 F0;
    float3 F;            // Fresnel against the dominant light
    float3 kdAlbedo;     // the diffuse response, (1-F)(1-metallic) * albedo
    float3 emissive;     // self-emitted radiance
    float  metallic, rough, ndv, f90;
    float  alpha;
    uint   model;        // AVER_MODEL_*
    bool   display;      // authored colour, bypassing lighting and camera post
    float4 displayColor;
};

// Evaluate the material. The light is an argument because this shading model's diffuse response is
// Fresnel-weighted against the dominant light and is then SHARED by the direct, ambient and bounce
// terms. Resolving it once here is what keeps those three terms bit-identical to the single
// expression they used to be; giving each term its own Fresnel would be a different (arguably
// better) image with no oracle behind it.
AverSurface averEvalMaterial(AverVertex v, AverLight l) {
    AverSurface s;
    s.N = v.N;
    s.V = v.V;
    s.H = normalize(v.V + l.direction);
    s.metallic = saturate(gMaterial.x);
    s.rough = clamp(gMaterial.y, 0.045, 1.0);
    s.alpha = gBaseColor.a;
    s.model = gShadingModel;
    s.emissive = gEmissive.rgb;
    s.f90 = gF90;
    s.display = gShadingModel == AVER_MODEL_UNLIT;
    s.displayColor = float4(gBaseColor.rgb, gBaseColor.a);
    s.albedo = srgbToLin(gBaseColor.rgb);
    s.ndv = saturate(dot(v.N, v.V));
    // The dielectric base reflectance is authored rather than the usual hardcoded 0.04, because
    // 0.04 is right for common dielectrics and wrong for water, gemstones and coated surfaces.
    s.F0 = lerp(gReflectance.xxx, s.albedo, s.metallic);
    s.F = fresnelSchlick(saturate(dot(s.H, v.V)), s.F0, s.f90);
    s.kdAlbedo = ((1.0 - s.F) * (1.0 - s.metallic)) * s.albedo;
    return s;
}

// True when the shading model produces an authored display colour that must reach the backbuffer
// untouched - no lighting, no fog, no tonemap. A renderer MUST honour this before shading; the
// shade calls below then contribute nothing for that model, so forgetting it costs the image
// rather than corrupting it.
bool averDisplayColour(AverSurface s, out float4 rgba) {
    rgba = s.displayColor;
    return s.display;
}

// The material's opacity, so a renderer never has to reach into the material constants for it.
float averOpacity(AverSurface s) { return s.alpha; }

// The view-INDEPENDENT diffuse albedo. Contractual: anything that has no camera (voxelisation, a
// light-map bake, an irradiance probe) shades with this, and it must never read s.V.
float3 averDiffuseAlbedo(AverSurface s) { return s.albedo; }

// Radiance ACCUMULATES rather than being summed by the caller. That is not stylistic: the terms
// below are added in one left-associated chain, and float addition is not associative, so letting
// the renderer combine partial sums would change the image by a last bit or two - exactly the kind
// of drift this decomposition exists to prove it does not cause.

// Direct lighting: Cook-Torrance GGX. NdotL and the light's visibility are applied here, so a
// renderer never multiplies radiance by a cosine it does not own the convention for.
float3 averShadeDirect(float3 radiance, AverSurface s, AverLight l) {
    switch (s.model) {
    case AVER_MODEL_UNLIT:
        return radiance;   // an unlit surface receives nothing; it only emits, in averShadeIndirect
    default: {
        float a = s.rough * s.rough;
        float k = (s.rough + 1.0); k = k * k / 8.0;
        float ndl = saturate(dot(s.N, l.direction));
        float D = distGGX(saturate(dot(s.N, s.H)), a);
        float G = geomSchlick(s.ndv, k) * geomSchlick(ndl, k);
        float3 spec = (D * G * s.F) / (4.0 * s.ndv * ndl + 1e-4);
        return radiance + (s.kdAlbedo / PI + spec) * l.radiance * ndl * l.visibility;
    }
    }
}

// Ambient, bounce and environment specular, in that order, then self-emission. Emissive rides with
// the view-independent terms because it is neither direct nor bounced, and a third entry point for
// one addition would put the ordering of the sum back into the renderer's hands.
float3 averShadeIndirect(float3 radiance, AverSurface s, AverIndirect ind) {
    switch (s.model) {
    case AVER_MODEL_UNLIT:
        return radiance + s.emissive;
    default: {
        float3 ambient = s.kdAlbedo * ind.ambient * ind.ambientScale;
        float3 envSpec = ind.specular * fresnelSchlick(s.ndv, s.F0, s.f90) * (1.0 - s.rough);
        float3 indirect = s.kdAlbedo * ind.diffuse;
        ambient *= ind.occlusion;   // whatever produced the bounce already knows what is occluded
        radiance += ambient;
        radiance += indirect;
        radiance += envSpec * 0.35;
        radiance += s.emissive;
        return radiance;
    }
    }
}
)";
}

// One define per slot rather than one base plus arithmetic: the HLSL preprocessor pastes tokens but
// cannot evaluate `t##(base+1)`, so the addition has to happen here.
std::string materialShaderDefines(u32 tableBaseRegister) {
    std::string s = "AVER_MATERIAL_SRV=" + std::to_string(tableBaseRegister);
    for (u32 i = 1; i < kTextureSlotCount; ++i)
        s += ";AVER_MATERIAL_SRV_" + std::to_string(i) + "=" + std::to_string(tableBaseRegister + i);
    return s;
}

} // namespace aver::pbr
