// The HLSL half of Aver.Render.PBR.Materials: the BRDF and the Aver* contract a renderer fills in.
// Concatenated AFTER rhi::sharedShaderPrelude(), which owns the shared layouts and helpers.
#include "aver/pbr/PbrShaders.hpp"

#include "aver/pbr/Material.hpp"

namespace aver::pbr {

// The material HLSL, as one string with static storage duration.
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
// The one sampler all five maps are read through; the consuming layout declares it and says where.
SamplerState gMaterialSampler : register(AVER_MAT_JOIN(s, AVER_MATERIAL_SAMPLER));
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
    float  gMatReflectance;
    float  gMatF90;
    float  gUvTilesPerCm;   // tiles per centimetre
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
#define AVER_MAT_WORLD_UV       (1u << 8)

// Shading model ids. The id arrives per draw in gShadingModel and is dispatched by a uniform switch.
#define AVER_MODEL_STANDARD 0u   // metallic / roughness, Cook-Torrance GGX
#define AVER_MODEL_UNLIT    1u   // authored colour, no lighting and no camera post

// Schlick Fresnel. f90 is the grazing-angle reflectance.
float3 fresnelSchlick(float ct, float3 F0, float f90){ return F0 + (f90-F0)*pow(saturate(1.0-ct),5.0); }
// GGX normal distribution. Takes alpha (rough*rough).
float distGGX(float ndh, float a){ float a2=a*a; float d=ndh*ndh*(a2-1.0)+1.0; return a2/(PI*d*d+1e-6); }

// Height-correlated Smith VISIBILITY: the specular 4*NdotV*NdotL denominator is folded in, so the
// caller multiplies D * V * F. Takes ALPHA (rough*rough), not a perceptual roughness.
float visSmithCorrelated(float ndv, float ndl, float a) {
    float a2 = a * a;
    float lv = ndl * sqrt(ndv * ndv * (1.0 - a2) + a2);
    float ll = ndv * sqrt(ndl * ndl * (1.0 - a2) + a2);
    return 0.5 / max(lv + ll, 1e-7);
}

// ================= the Aver material contract =================
// The material owns the BRDF; the renderer owns light transport. AverSurface is OPAQUE to it.

// What the renderer knows about the point being shaded, and all it has to supply.
struct AverVertex {
    float3 wpos;
    float3 N;    // unit surface normal, world space
    float3 V;    // unit vector towards the camera; EXACTLY zero where there is no camera
    float2 uv;   // surface parameterisation
};

// One light's contribution as the renderer resolved it.
struct AverLight {
    float3 direction;   // unit vector TO the light
    float3 radiance;    // linear radiance arriving along `direction`
    float  visibility;  // 0 = fully occluded, 1 = fully lit
};

// Everything reaching the surface that did not come straight from a light, as RAW radiance.
struct AverIndirect {
    float3 ambient;      // sky-hemisphere irradiance the renderer sampled for this surface
    float  ambientScale; // weight for `ambient`, applied after the diffuse response
    float3 diffuse;      // bounced radiance (zero when the renderer traces none)
    float  occlusion;    // ambient occlusion; weights `ambient` only, never `diffuse`
    float3 specular;     // environment radiance along the reflection vector
};

// Builds an AverVertex from the shared vertex/mesh shaders' output.
AverVertex averVertexOf(VSOut i) {
    AverVertex v;
    v.wpos = i.wpos;
    v.N    = normalize(i.nrmWS);
    v.V    = normalize(gCamPos.xyz - i.wpos);
    v.uv   = i.uv;
    return v;
}

// The evaluated material at one point. Opaque to the renderer.
struct AverSurface {
    float3 N, V, H;
    float3 albedo;       // linear base colour
    float3 F0;
    float3 F;            // Fresnel against the dominant light
    float3 kdAlbedo;     // the diffuse response
    float3 emissive;     // self-emitted radiance
    float  metallic, rough, ndv, f90;
    float  alpha;
    float  occlusion;    // the material's OWN occlusion map, distinct from the renderer's AO
    uint   model;        // AVER_MODEL_*
    bool   display;      // authored colour, bypassing lighting and camera post
    float4 displayColor;
};

// The five maps at one point. Every value is the identity for its slot where no map is bound.
struct AverMaps {
    float4 baseColor;   // linear rgb (sRGB VIEW, decoded by the texture unit) + alpha
    float2 metalRough;  // x = roughness (glTF G), y = metallic (glTF B)
    float3 normalTS;    // tangent-space normal, already scaled and re-centred
    float  occlusion;
    float3 emissive;
};

// The texture coordinate this surface is sampled at: the mesh's own, or a planar projection.
float2 averSurfaceUV(AverVertex v) {
    if (gMaterialFlags & AVER_MAT_WORLD_UV) {
        float3 ax = normalize(gWorld[0].xyz);
        float3 ay = normalize(gWorld[1].xyz);
        float3 az = normalize(gWorld[2].xyz);
        float3 d  = v.wpos - gWorld[3].xyz;
        float3 op = float3(dot(d, ax), dot(d, ay), dot(d, az));      // position in the object's frame
        float3 on = float3(dot(v.N, ax), dot(v.N, ay), dot(v.N, az)); // normal likewise

        float3 a = abs(on);
        float2 p = (a.z >= a.x && a.z >= a.y) ? op.xy
                 : ((a.x >= a.y) ? op.yz : op.xz);
        return p * gUvTilesPerCm;
    }
    return v.uv;
}

// Samples the five material maps, or returns the identity values where there is no material table.
AverMaps averSampleMaps(float2 uv) {
    AverMaps m;
#ifdef AVER_MATERIAL_SRV
    m.baseColor  = gBaseColorMap.Sample(gMaterialSampler, uv);
    float4 mr    = gMetalRoughMap.Sample(gMaterialSampler, uv);
    m.metalRough = float2(mr.g, mr.b);
    float3 n     = gNormalMap.Sample(gMaterialSampler, uv).xyz * 2.0 - 1.0;
    m.normalTS   = float3(n.xy * gNormalScale, n.z);
    m.occlusion  = gOcclusionMap.Sample(gMaterialSampler, uv).r;
    m.emissive   = gEmissiveMap.Sample(gMaterialSampler, uv).rgb;
#else
    m.baseColor  = float4(1, 1, 1, 1);
    m.metalRough = float2(1, 1);
    m.normalTS   = float3(0, 0, 1);
    m.occlusion  = 1.0;
    m.emissive   = float3(0, 0, 0);
#endif
    return m;
}

// Applies a tangent-space normal, solving the tangent frame from screen-space derivatives.
float3 averPerturbNormal(float3 N, float3 wpos, float2 uv, float3 nTS) {
    float3 dp1 = ddx(wpos), dp2 = ddy(wpos);
    float2 du1 = ddx(uv),   du2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * du1.x + dp1perp * du2.x;
    float3 B = dp2perp * du1.y + dp1perp * du2.y;
    float m = max(dot(T, T), dot(B, B));
    if (m <= 0.0) return N;
    float invmax = rsqrt(m);
    return normalize(T * (nTS.x * invmax) + B * (nTS.y * invmax) + N * nTS.z);
}

// Evaluates the material at one point. The light is used only for the half vector.
AverSurface averEvalMaterial(AverVertex v, AverLight l) {
    float2 uv = averSurfaceUV(v);
    AverMaps map = averSampleMaps(uv);

    float4 base = gBaseColorFactor * map.baseColor;

    AverSurface s;
    s.N = averPerturbNormal(v.N, v.wpos, uv, map.normalTS);
    s.V = v.V;
    s.H = normalize(v.V + l.direction);
    s.metallic = saturate(gMaterial.x * gMetallicFactor * map.metalRough.y);
    s.rough = clamp(gMaterial.y * gRoughnessFactor * map.metalRough.x, 0.045, 1.0);
    s.alpha = gBaseColor.a * base.a;
    s.model = gShadingModel;
    s.emissive = gEmissive.rgb + gEmissiveFactor * map.emissive;
    s.occlusion = lerp(1.0, map.occlusion, gOcclusionStrength);
    s.f90 = gMatF90;
    s.display = gShadingModel == AVER_MODEL_UNLIT;
    s.displayColor = float4(gBaseColor.rgb, gBaseColor.a);
    s.albedo = srgbToLin(gBaseColor.rgb) * base.rgb;
    if (gMaterialFlags & AVER_MAT_ALPHA_MASK) clip(s.alpha - gAlphaCutoff);
    s.ndv = saturate(dot(s.N, v.V));
    s.F0 = lerp(gMatReflectance.xxx, s.albedo, s.metallic);
    s.F = fresnelSchlick(saturate(dot(s.H, v.V)), s.F0, s.f90);
    s.kdAlbedo = (1.0 - s.metallic) * s.albedo;
    return s;
}

// True when the surface has an authored display colour that must reach the backbuffer untouched.
bool averDisplayColour(AverSurface s, out float4 rgba) {
    rgba = s.displayColor;
    return s.display;
}

// The material's opacity.
float averOpacity(AverSurface s) { return s.alpha; }

// The view-INDEPENDENT diffuse albedo.
float3 averDiffuseAlbedo(AverSurface s) { return s.albedo; }

// The SHADING normal, after normal mapping. A renderer builds its reflection vector from this.
float3 averShadingNormal(AverSurface s) { return s.N; }

// The split-sum environment BRDF as Karis' analytic fit. Returns (A, B) such that the reflected
// fraction of prefiltered environment radiance is F0 * A + B.
float2 averEnvBRDF(float ndv, float rough) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572,  0.022);
    const float4 c1 = float4( 1.0,  0.0425,  1.04,  -0.04);
    float4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

// Adds one light's direct contribution: Cook-Torrance GGX, with NdotL and visibility applied.
float3 averShadeDirect(float3 radiance, AverSurface s, AverLight l) {
    switch (s.model) {
    case AVER_MODEL_UNLIT:
        return radiance;
    default: {
        float a = s.rough * s.rough;
        float ndl = saturate(dot(s.N, l.direction));
        float D = distGGX(saturate(dot(s.N, s.H)), a);
        float V = visSmithCorrelated(s.ndv, ndl, a);
        float3 spec = D * V * s.F;
        return radiance + (s.kdAlbedo / PI + spec) * l.radiance * ndl * l.visibility;
    }
    }
}

// Adds ambient, bounce, environment specular and self-emission, in that order.
float3 averShadeIndirect(float3 radiance, AverSurface s, AverIndirect ind) {
    switch (s.model) {
    case AVER_MODEL_UNLIT:
        return radiance + s.emissive;
    default: {
        float3 ambient = s.kdAlbedo * ind.ambient * ind.ambientScale;
        float2 dfg = averEnvBRDF(s.ndv, s.rough);
        float3 envSpec = ind.specular * (s.F0 * dfg.x + dfg.y);
        float3 indirect = s.kdAlbedo * ind.diffuse;
        ambient *= ind.occlusion * s.occlusion;
        radiance += ambient;
        radiance += indirect;
        radiance += envSpec * ind.occlusion;
        radiance += s.emissive;
        return radiance;
    }
    }
}
)";
}

// The -D list pinning the material textures to the registers the root signature declared. One
// define per slot: the HLSL preprocessor pastes tokens but cannot evaluate `t##(base+1)`.
std::string materialShaderDefines(u32 tableBaseRegister, u32 samplerRegister) {
    std::string s = "AVER_MATERIAL_SRV=" + std::to_string(tableBaseRegister);
    for (u32 i = 1; i < kTextureSlotCount; ++i)
        s += ";AVER_MATERIAL_SRV_" + std::to_string(i) + "=" + std::to_string(tableBaseRegister + i);
    s += ";AVER_MATERIAL_SAMPLER=" + std::to_string(samplerRegister);
    return s;
}

} // namespace aver::pbr
