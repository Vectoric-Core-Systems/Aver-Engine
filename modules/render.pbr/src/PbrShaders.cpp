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
// The SECOND LAYER, blended in by slope under AVER_MAT_SLOPE_BLEND. Same slot-index-is-register
// rule as above, continuing pbr::TextureSlot's order.
Texture2D gL1BaseColorMap  : register(AVER_MAT_JOIN(t, AVER_MATERIAL_SRV_5));
Texture2D gL1MetalRoughMap : register(AVER_MAT_JOIN(t, AVER_MATERIAL_SRV_6));
Texture2D gL1NormalMap     : register(AVER_MAT_JOIN(t, AVER_MATERIAL_SRV_7));
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
    // Second layer. gSlopeBlendLo/Hi are world-normal Z -- 1 flat, 0 vertical -- so `Lo` is the
    // STEEPER end and layer 1 wins there. Read only under AVER_MAT_SLOPE_BLEND.
    float  gSlopeBlendLo;
    float  gSlopeBlendHi;
    float  gL1UvScale;
    // Mirrors MaterialConstants::graphId. 0 means "no graph", which is the arm the generated
    // averEvalMaterial's `default:` takes -- see pbr::materialGraphHlsl(). Declared here, in the
    // block every material path already binds, so a graph-shaded draw needs nothing extra bound.
    uint   gMaterialGraphId;
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
#define AVER_MAT_L1_BASECOLOR   (1u << 9)
#define AVER_MAT_L1_METALROUGH  (1u << 10)
#define AVER_MAT_L1_NORMAL      (1u << 11)
#define AVER_MAT_SLOPE_BLEND    (1u << 12)

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
    // Two-sided shading, matching plainShadeSurface in rhi::sharedShaderPrelude -- see its comment
    // for why this is dot(N, V) and not SV_IsFrontFace. It belongs in BOTH preludes because a
    // material-shaded draw never goes through the plain path, and foliage is exactly what the
    // material path is for: a one-sheet leaf drawn with culling off shades its back side black
    // without this.
    if (dot(v.N, v.V) < 0.0) v.N = -v.N;
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
    // The DIELECTRIC F0 this surface was built from. Carried here rather than read back off
    // gMatReflectance, because a RAY HIT HAS NO MATERIAL CONSTANT BUFFER BOUND -- PSRayDriven says
    // exactly that where it defaults this to 0.04 by hand. averShadeIndirect reaching for the
    // global instead measured a white dielectric at 1.030 on the ray path against 1.000 on the
    // raster one: with no material bound the global is not 0.04, the diffuse lobe stopped being
    // charged for the specular reflectance it takes off the top, and the surface read too bright.
    float  reflectance;
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
// Blends the second layer over the first by SLOPE, and returns the combined maps.
//
// SLOPE IS TAKEN FROM THE GEOMETRIC NORMAL, deliberately, not from the normal-mapped one: the
// question is "is this part of the terrain a cliff", which is a property of the surface, and
// feeding a normal map into it would make the layer choice flicker with every bump in the detail.
AverMaps averBlendLayers(AverMaps m, float2 uv, float3 geoN) {
#ifdef AVER_MATERIAL_SRV
    if (!(gMaterialFlags & AVER_MAT_SLOPE_BLEND)) return m;

    // World normal Z: 1 on flat ground, 0 on a vertical face. smoothstep(lo, hi, .) is 0 at the
    // steep end, so `w` is how much of LAYER 1 to take.
    float flat01 = saturate(abs(geoN.z));
    float w = 1.0 - smoothstep(gSlopeBlendLo, gSlopeBlendHi, flat01);
    if (w <= 0.001) return m;

    float2 uv1 = uv * gL1UvScale;
    if (gMaterialFlags & AVER_MAT_L1_BASECOLOR)
        m.baseColor = lerp(m.baseColor, gL1BaseColorMap.Sample(gMaterialSampler, uv1), w);
    if (gMaterialFlags & AVER_MAT_L1_METALROUGH) {
        float4 mr1 = gL1MetalRoughMap.Sample(gMaterialSampler, uv1);
        m.metalRough = lerp(m.metalRough, float2(mr1.g, mr1.b), w);
    }
    if (gMaterialFlags & AVER_MAT_L1_NORMAL) {
        float3 n1 = gL1NormalMap.Sample(gMaterialSampler, uv1).xyz * 2.0 - 1.0;
        // Blended in tangent space then renormalised -- cheap, and correct enough for two layers
        // that share a tangent frame, which they do here because they share the mesh.
        m.normalTS = normalize(lerp(m.normalTS, float3(n1.xy * gNormalScale, n1.z), w));
    }
#endif
    return m;
}

// EVERYTHING A MATERIAL AUTHORS, and nothing else. Every remaining field of AverSurface is DERIVED
// from these by averBuildSurface below: F0 from albedo and metallic, F from F0 and the half vector,
// kdAlbedo from albedo and metallic, ndv from the shading normal.
//
// THIS SPLIT IS WHAT LETS A NODE GRAPH DRIVE A MATERIAL WITHOUT RESTATING THE BRDF. A generated
// averEvalMaterial starts from averStockAuthored(), overwrites only the fields its graph actually
// drives, and hands the result to averBuildSurface -- so a graph that sets nothing but base colour
// still gets the right F0, the right energy split and the right alpha clip, and a change to how a
// surface is DERIVED is made in one place instead of once per generated shader. Without it, every
// generated material would carry its own copy of the twelve lines below and would silently stop
// matching the stock path the first time one of them changed.
//
// THESE ARE THE AUTHORED HALVES ONLY -- the per-material factor times the map. The per-DRAW terms
// (gMaterial, gBaseColor, gEmissive, gShadingModel) stay in averBuildSurface, because they are the
// renderer's to apply and not the material's to author: a graph overriding them would be overriding
// the entity's own tint, which is not what an author asking for "red" means.
struct AverAuthored {
    float3 baseColor;   // linear; gBaseColorFactor.rgb * the base colour map
    float  opacity;     // gBaseColorFactor.a * the base colour map's alpha
    float  metallic;    // gMetallicFactor * the map, before saturate()
    float  roughness;   // gRoughnessFactor * the map, before the 0.045 floor
    float3 normalTS;    // tangent space; (0, 0, 1) is no perturbation
    float3 emissive;    // gEmissiveFactor * the map
    float  occlusion;   // the material's OWN occlusion map, before gOcclusionStrength
    float  alphaCutoff; // read only under AVER_MAT_ALPHA_MASK
};

// What the stock material path authors: the five maps, blended by slope, times the b2 factors.
AverAuthored averStockAuthored(float2 uv, float3 geoN) {
    AverMaps map = averSampleMaps(uv);
    map = averBlendLayers(map, uv, geoN);

    float4 base = gBaseColorFactor * map.baseColor;

    AverAuthored a;
    a.baseColor   = base.rgb;
    a.opacity     = base.a;
    a.metallic    = gMetallicFactor * map.metalRough.y;
    a.roughness   = gRoughnessFactor * map.metalRough.x;
    a.normalTS    = map.normalTS;
    a.emissive    = gEmissiveFactor * map.emissive;
    a.occlusion   = map.occlusion;
    a.alphaCutoff = gAlphaCutoff;
    return a;
}

// Derives the whole surface from what the material authored. This is the material SYSTEM's
// arithmetic, not any one material's, which is exactly why a graph never gets to restate it.
AverSurface averBuildSurface(AverVertex v, AverLight l, AverAuthored a, float2 uv) {
    AverSurface s;
    s.N = averPerturbNormal(v.N, v.wpos, uv, a.normalTS);
    s.V = v.V;
    s.H = normalize(v.V + l.direction);
    s.metallic = saturate(gMaterial.x * a.metallic);
    s.rough = clamp(gMaterial.y * a.roughness, 0.045, 1.0);
    s.alpha = gBaseColor.a * a.opacity;
    s.model = gShadingModel;
    s.emissive = gEmissive.rgb + a.emissive;
    s.occlusion = lerp(1.0, a.occlusion, gOcclusionStrength);
    s.f90 = gMatF90;
    s.reflectance = gMatReflectance;
    s.display = gShadingModel == AVER_MODEL_UNLIT;
    s.displayColor = float4(gBaseColor.rgb, gBaseColor.a);
    s.albedo = srgbToLin(gBaseColor.rgb) * a.baseColor;
    if (gMaterialFlags & AVER_MAT_ALPHA_MASK) clip(s.alpha - a.alphaCutoff);
    s.ndv = saturate(dot(s.N, v.V));
    s.F0 = lerp(gMatReflectance.xxx, s.albedo, s.metallic);
    s.F = fresnelSchlick(saturate(dot(s.H, v.V)), s.F0, s.f90);
    s.kdAlbedo = (1.0 - s.metallic) * s.albedo;
    return s;
}

// The STOCK material: b2 constants and the five maps, no graph.
//
// COMPILED OUT WHEN AVER_MATERIAL_GRAPH IS DEFINED, and that one define is the whole switch. A
// caller wanting graph-driven materials compiles the prelude with it and appends its OWN
// averEvalMaterial -- identical signature, same position in the translation unit -- immediately
// after. Everything below this point only reads an AverSurface and never calls averEvalMaterial
// (grep it: the three call sites are all in renderer sources, outside this prelude), so nothing
// else here has to change and the stock path is never deleted, only not compiled.
#ifndef AVER_MATERIAL_GRAPH
AverSurface averEvalMaterial(AverVertex v, AverLight l) {
    float2 uv = averSurfaceUV(v);
    return averBuildSurface(v, l, averStockAuthored(uv, v.N), uv);
}
#endif

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

// SPECULAR OCCLUSION. Ambient occlusion answers "how much of the HEMISPHERE is blocked", which is
// the right question for a diffuse lobe gathering from all of it and the wrong one for a specular
// lobe gathering from a narrow cone about the reflection vector. Feeding raw AO to specular, as
// this file did, darkens a grazing mirror that can see straight past its own occluder and barely
// touches a rough surface that genuinely is enclosed.
//
// Lagarde's form: the exponent collapses toward 1 as roughness rises, so a rough lobe converges
// on the diffuse answer (AO itself) while a smooth one is progressively freed from it. At ao = 1 it
// returns 1 for every roughness, which is what keeps the white furnace readable -- an occlusion
// term that dimmed an unoccluded surface would fail the oracle before any of the energy maths did.
float averSpecularOcclusion(float ndv, float ao, float rough) {
    // NO saturate() INSIDE THE pow. The whole mechanism is that ndv + ao EXCEEDS 1 for a smooth,
    // face-on surface, so raising it to a small exponent returns something above 1 and the -1 + ao
    // that follows lands ABOVE plain ao. Clamping the base to 1 makes pow(1, x) == 1 for every
    // roughness and the function collapses to `ao` exactly -- which is the bug it exists to fix.
    // Written that way first, and the furnace caught it: the metal row read 1.000 * ao, unchanged.
    // The base cannot go negative (ao and abs(ndv) are both non-negative), so nothing needs a guard.
    return saturate(pow(abs(ndv) + ao, exp2(-16.0 * rough - 1.0)) - 1.0 + ao);
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
        // ENERGY LOST TO MASKING, PUT BACK. A single-scatter GGX lobe drops every ray the
        // microsurface would have bounced a second time, and the loss grows with roughness: a white
        // metal at roughness 1 returned 45% of the light it received, measured in this engine's own
        // white furnace before this line existed. The compensation is the standard single-term
        // approximation and it reuses the split-sum term the indirect path already computes -- see
        // averShadeIndirect, which corrects the same loss for the environment.
        float2 dfg = averEnvBRDF(s.ndv, s.rough);
        // Ess -- THE SUM -- NOT dfg.x. dfg.x is only the SCALE half of "F0*scale + bias"; what this
        // compensation inverts is the single-scatter DIRECTIONAL ALBEDO, which is what the pair sums
        // to. Using dfg.x alone looks nearly right head-on (0.452 against 0.450 at roughness 1) and
        // comes apart at grazing incidence: there the fit sends the scale term toward 0.077 while the
        // sum stays near 0.97, so 1/scale drove the factor to THIRTEEN. Terrain is seen at grazing
        // angles across most of a frame, and it read about 15% too bright everywhere until this was
        // tracked down -- bisected to this one line by compiling the three parts of the change out
        // one at a time.
        float  Ess = max(dfg.x + dfg.y, 1e-3);
        spec *= 1.0 + s.F0 * (1.0 / Ess - 1.0);
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
        // THE SPLIT SUM, WITH THE MULTIPLE SCATTERING PUT BACK (Fdez-Aguera). The old form was
        // `ind.specular * (F0*dfg.x + dfg.y)` plus a full-strength diffuse term, which is wrong in
        // two directions at once and this engine's white furnace measured both:
        //
        //   - a white METAL kept only 45% of its energy at roughness 1 (97% at 0.05), because the
        //     light GGX loses to masking was never returned;
        //   - a DIELECTRIC read 1.5-4.5% too BRIGHT, because the diffuse lobe was handed the whole
        //     albedo while the specular lobe took its reflectance off the top of the same budget.
        //
        // Both are the same omission: the split sum accounts for one bounce off the microsurface and
        // nothing else, so the energy balance never closes. Ess is what a single bounce returns, Ems
        // is what it dropped, Favg is the Fresnel averaged over the hemisphere, and Fms*Ems sums every
        // further bounce. kD is then what is genuinely left for diffuse -- which is what makes the
        // dielectric stop over-reading, with no separate fudge for it.
        //
        // Worked through by hand before it was written and then confirmed in the furnace: every cell
        // of the 6x3 roughness/metallic grid lands on 1.000 rather than 0.45-1.045.
        float2 dfg    = averEnvBRDF(s.ndv, s.rough);
        float3 FssEss = s.F0 * dfg.x + dfg.y;
        float  Ess    = dfg.x + dfg.y;
        float  Ems    = 1.0 - Ess;
        // The 1/21 is the analytic hemispherical average of the Schlick term, not a tuned number.
        float3 Favg   = s.F0 + (1.0 - s.F0) / 21.0;
        // Guarded because Ems*Favg reaches 1 only when a single bounce returns nothing at all, and a
        // division by zero here would paint NaN across every rough pixel in the frame.
        float3 FmsEms = Ems * FssEss * Favg / max(1.0 - Ems * Favg, 1e-4);

        // WHAT THE DIFFUSE LOBE LOSES IS THE DIELECTRIC RELECTANCE, NOT THE BLENDED ONE. The diffuse
        // lobe belongs to the dielectric substrate -- a metal has none, which is what kdAlbedo's own
        // (1 - metallic) factor already expresses. Subtracting the METAL-blended FssEss from it as
        // well charges the substrate for reflectance it never had, and the furnace caught it: taking
        // the blended term dropped metallic 0.5 from 1.005 to 0.757 while both pure ends stayed at
        // 1.000. Recomputing the pair against the dielectric F0 restores it to 0.98.
        //
        // INTERMEDIATE METALLIC STILL DOES NOT CLOSE, and no arrangement of these terms makes it.
        // "Half metal" is not a material; it is a blend of PARAMETERS, and the multiple-scatter series
        // is strongly non-linear in F0 -- a surface at F0 = 0.52 returns much less than the mean of one
        // at 0.04 and one at 1.0. The physical claim this code makes is at the two ENDS, which now
        // measure 1.000 across every roughness. The middle is a documented approximation.
        float3 F0d     = s.reflectance.xxx;
        float3 FssEssD = F0d * dfg.x + dfg.y;
        float3 FavgD   = F0d + (1.0 - F0d) / 21.0;
        float3 FmsEmsD = Ems * FssEssD * FavgD / max(1.0 - Ems * FavgD, 1e-4);
        float3 kD      = s.kdAlbedo * saturate(1.0 - FssEssD - FmsEmsD);

        // Occlusion is applied per lobe now: AO answers a hemisphere question and belongs with the
        // diffuse terms, while the specular lobe gets averSpecularOcclusion. Sending raw AO into a
        // mirror cost 21% of its energy in the furnace with GI on -- measured against the same grid.
        float  diffOcc = ind.occlusion * s.occlusion;
        float  specOcc = averSpecularOcclusion(s.ndv, ind.occlusion, s.rough);

        // FssEss multiplies RADIANCE (the reflection); everything else multiplies IRRADIANCE (the
        // sky and the bounce), which is why they are not folded into one factor.
        radiance += FssEss * ind.specular * specOcc;
        radiance += (FmsEms + kD) * ind.ambient * ind.ambientScale * diffOcc;
        radiance += kD * ind.diffuse;
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
