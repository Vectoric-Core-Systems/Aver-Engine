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
// The one sampler all five maps are read through. A static sampler is a property of the pipeline
// LAYOUT, so the consuming renderer declares the hardware behind this register and tells us which
// register it landed at -- same arrangement as the SRVs above, and for the same reason: a shader
// naming a sampler register its root signature never declared fails at pipeline creation.
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
    float  gUvTilesPerCm;   // reciprocal of the authored cm-per-tile; see MaterialConstants
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

// ---- shading models. The id arrives per draw in gShadingModel and is dispatched by a UNIFORM
// switch, not by a pipeline permutation: the branch is scalar and free, while a permutation would
// multiply every scene pipeline a renderer builds by the number of models. ----
#define AVER_MODEL_STANDARD 0u   // metallic / roughness, Cook-Torrance GGX
#define AVER_MODEL_UNLIT    1u   // authored colour, no lighting and no camera post

// ---- BRDF terms. Private to the material system: the renderer has no business calling these. ----
// f90 is the grazing-angle reflectance. At 1.0 this is the textbook Schlick term.
float3 fresnelSchlick(float ct, float3 F0, float f90){ return F0 + (f90-F0)*pow(saturate(1.0-ct),5.0); }
float distGGX(float ndh, float a){ float a2=a*a; float d=ndh*ndh*(a2-1.0)+1.0; return a2/(PI*d*d+1e-6); }

// Height-correlated Smith VISIBILITY, not a masking-shadowing term: the 4*NdotV*NdotL denominator
// of the Cook-Torrance specular is folded in here, so the caller multiplies D * V * F and divides
// by nothing. That is the point of the change. The separable pair this replaces had to be divided
// by (4*ndv*ndl + 1e-4), and that epsilon was not cosmetic -- it is an additive bias on every
// pixel, largest exactly where the denominator is smallest, so it darkened grazing angles across
// the whole image.
//
// CONVENTION, and it is easy to get wrong: this takes ALPHA (rough*rough), the same parameter
// distGGX takes. The Schlick pair it replaces took k remapped from PERCEPTUAL roughness, so
// handing it s.rough would compile, run, and shift the whole roughness response by a squaring --
// which reads as "the material authoring feels off" rather than as a bug.
float visSmithCorrelated(float ndv, float ndl, float a) {
    float a2 = a * a;
    float lv = ndl * sqrt(ndv * ndv * (1.0 - a2) + a2);
    float ll = ndv * sqrt(ndl * ndl * (1.0 - a2) + a2);
    // max(), NOT an added epsilon. The sum is zero only where ndv and ndl are BOTH exactly zero,
    // and there the caller's ndl factor already makes the term vanish -- so this floor exists purely
    // to stop 0.5/0 producing the inf that 0 * inf turns into a NaN pixel. Everywhere else it is
    // orders of magnitude below the real value and biases nothing, which is precisely what the
    // additive guard could not say for itself.
    return 0.5 / max(lv + ll, 1e-7);
}

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
    float2 uv;   // surface parameterisation; the material's, so the renderer only carries it
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
    v.uv   = i.uv;
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
    float  occlusion;    // the material's OWN occlusion map, distinct from the renderer's AO
    uint   model;        // AVER_MODEL_*
    bool   display;      // authored colour, bypassing lighting and camera post
    float4 displayColor;
};

// ---- the five maps, read through the one wrapping sampler. ----
// Every value returned here is the IDENTITY for its slot on a pipeline that declares no material
// table, so exactly one expression shades a textured surface, an untextured one and a pass that
// cannot see textures at all. There is no branch and no permutation: the fallback textures the
// material system binds for an unset slot carry these same identity values, so "no map" reaches
// the maths as a multiply by one rather than as a condition.
struct AverMaps {
    float4 baseColor;   // linear rgb (sRGB VIEW, decoded by the texture unit) + alpha
    float2 metalRough;  // x = roughness (glTF G), y = metallic (glTF B)
    float3 normalTS;    // tangent-space normal, already scaled and re-centred
    float  occlusion;
    float3 emissive;
};

// The texture coordinate this surface is sampled at. Either the mesh's own, or a world-space planar
// projection onto the dominant axis of the normal -- see pbr::UvMode for why a blockout needs the
// second one.
//
// The branch is on a MATERIAL flag, so it is uniform across every pixel of a draw and costs nothing
// beyond the compare. Both sides are computed cheaply enough that no permutation is warranted.
float2 averSurfaceUV(AverVertex v) {
    if (gMaterialFlags & AVER_MAT_WORLD_UV) {
        // PROJECTED IN THE OBJECT'S OWN FRAME, not the world's.
        //
        // This projected world position onto the dominant world axis, which nails the texture to the
        // world and lets the object slide through it. On anything that moves, the surface swims: turn
        // the character and the pattern on the gun's grip and sight crawls across them, because the
        // geometry rotated and the projection did not. Every prop a physics step nudges does it too.
        //
        // The axes come from the world matrix's basis rows -- this is a row-vector convention, so
        // rows 0..2 are the object's X, Y and Z in world space and row 3 is its origin. NORMALISED,
        // which is what keeps texel density in world centimetres: the offset from the origin is
        // measured in world units and merely resolved ALONG the object's axes, so a unit cube scaled
        // to sixteen metres still gets its texture tiled every uvTiling centimetres rather than
        // stretched once across the whole face.
        //
        // No inverse is needed and none is available: HLSL has no matrix-inverse intrinsic, and a
        // TRS matrix has no shear, so three dot products against an orthonormal basis IS the inverse
        // rotation.
        float3 ax = normalize(gWorld[0].xyz);
        float3 ay = normalize(gWorld[1].xyz);
        float3 az = normalize(gWorld[2].xyz);
        float3 d  = v.wpos - gWorld[3].xyz;
        float3 op = float3(dot(d, ax), dot(d, ay), dot(d, az));      // position in the object's frame
        float3 on = float3(dot(v.N, ax), dot(v.N, ay), dot(v.N, az)); // normal likewise

        float3 a = abs(on);
        // The plane the surface most faces. Ties go to Z, then X, which only matters on a perfect
        // 45-degree edge where either answer is equally arbitrary -- but it must be DECIDED rather
        // than left to floating-point luck, or adjacent pixels on such a face pick different planes
        // and the seam shimmers.
        float2 p = (a.z >= a.x && a.z >= a.y) ? op.xy
                 : ((a.x >= a.y) ? op.yz : op.xz);
        // No per-face sign flip, so opposite faces of a box MIRROR when seen from outside. That is
        // inherent to a dominant-axis projection and is not what changed here.
        //
        // WHAT IS GIVEN UP: two separate wall objects meeting at a corner no longer continue one
        // pattern across the seam, because each is now projected about its own origin. That was the
        // stated reason this mode existed, and it is worth less than the alternative -- a static
        // seam is a detail you have to go and look for, while a texture crawling over a weapon in
        // the middle of the screen is the first thing anyone sees.
        return p * gUvTilesPerCm;
    }
    return v.uv;
}

AverMaps averSampleMaps(float2 uv) {
    AverMaps m;
#ifdef AVER_MATERIAL_SRV
    m.baseColor  = gBaseColorMap.Sample(gMaterialSampler, uv);
    float4 mr    = gMetalRoughMap.Sample(gMaterialSampler, uv);
    m.metalRough = float2(mr.g, mr.b);
    // glTF scales only the tangential components; scaling z as well would tilt a flat normal.
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

// The tangent frame, solved from the screen-space derivatives of world position and UV rather than
// carried on the vertex. That is a decision, not an omission: a tangent attribute has to be
// generated by the importer, kept in step with the UV set the normal map was baked against, and
// stored on every vertex of every mesh -- and it is derivable exactly where it is needed.
//
// Schuler's cotangent-frame derivation. The two cross products give the perpendiculars without ever
// forming the UV Jacobian's determinant, so the only degenerate case left is a face with NO uv
// gradient at all, which is caught below.
float3 averPerturbNormal(float3 N, float3 wpos, float2 uv, float3 nTS) {
    float3 dp1 = ddx(wpos), dp2 = ddy(wpos);
    float2 du1 = ddx(uv),   du2 = ddy(uv);
    float3 dp2perp = cross(dp2, N);
    float3 dp1perp = cross(N, dp1);
    float3 T = dp2perp * du1.x + dp1perp * du2.x;
    float3 B = dp2perp * du1.y + dp1perp * du2.y;
    // An unwrapped mesh, or a face collapsed to a point in UV space, leaves both of these zero.
    // rsqrt(0) is +inf and the frame comes out NaN, which propagates through the whole shade and
    // reaches the backbuffer as a black or white pixel -- this project already has a TDR to the
    // name of one normalize() of a zero vector. Fall back to the geometric normal instead.
    float m = max(dot(T, T), dot(B, B));
    if (m <= 0.0) return N;
    float invmax = rsqrt(m);
    return normalize(T * (nTS.x * invmax) + B * (nTS.y * invmax) + N * nTS.z);
}

// Evaluate the material. The light is still an argument, but only for the half vector the specular
// lobe needs: the diffuse response no longer depends on it at all, which is the point of the change
// below. That makes averDiffuseAlbedo() and everything derived from kdAlbedo genuinely
// view-independent rather than view-independent by convention, so the voxelisation pass -- which
// hands this a zero view vector and a fabricated light -- gets the same answer the lit pass does.
AverSurface averEvalMaterial(AverVertex v, AverLight l) {
    // Resolved ONCE and passed to both the sampler and the tangent frame. Two calls would compile to
    // the same thing today, but the frame must be solved against the very coordinates the normal map
    // was sampled at -- deriving it from v.uv while sampling at a world-aligned uv would tilt every
    // normal by the difference between the two parameterisations.
    float2 uv = averSurfaceUV(v);
    AverMaps map = averSampleMaps(uv);

    // Base colour is the product of three things and NONE of them is decoded here. The map arrives
    // linear because its view format is sRGB and the TEXTURE UNIT decoded it -- doing it after the
    // Sample would be wrong, because filtering and mip averaging have already happened in encoded
    // space by then, and that reads as slightly dark, slightly desaturated midtones that get worse
    // with distance as coarser mips average more. Nobody reports that as a bug. The authored factor
    // arrives linear because packMaterial() decoded it on the CPU. What is left is gBaseColor, the
    // RENDERER's per-draw instance colour out of b1 -- a different block with a different owner and
    // a frozen sRGB convention that the no-material path still depends on.
    float4 base = gBaseColorFactor * map.baseColor;

    AverSurface s;
    s.N = averPerturbNormal(v.N, v.wpos, uv, map.normalTS);
    s.V = v.V;
    s.H = normalize(v.V + l.direction);
    // Factors MULTIPLY their maps, which is glTF's rule and also what makes an unset map free: the
    // fallback texture reads exactly 1 in these channels, so a factors-only material and a textured
    // one are the same instruction stream.
    s.metallic = saturate(gMaterial.x * gMetallicFactor * map.metalRough.y);
    s.rough = clamp(gMaterial.y * gRoughnessFactor * map.metalRough.x, 0.045, 1.0);
    s.alpha = gBaseColor.a * base.a;
    s.model = gShadingModel;
    s.emissive = gEmissive.rgb + gEmissiveFactor * map.emissive;
    // glTF's occlusion rule: strength 0 disables the map entirely rather than zeroing the surface.
    s.occlusion = lerp(1.0, map.occlusion, gOcclusionStrength);
    // The MATERIAL's, out of b2 -- not the renderer's b1 defaults, which is the whole point. A
    // surface's reflectance is a property of what it is made of, and the b1 pair can only ever say
    // what the renderer assumed on its behalf.
    s.f90 = gMatF90;
    s.display = gShadingModel == AVER_MODEL_UNLIT;
    s.displayColor = float4(gBaseColor.rgb, gBaseColor.a);
    s.albedo = srgbToLin(gBaseColor.rgb) * base.rgb;
    // Masked blending is the material's rule, so the clip is here and no renderer has to remember
    // it. The test is on the flag, which is uniform per draw, so an opaque material pays nothing.
    if (gMaterialFlags & AVER_MAT_ALPHA_MASK) clip(s.alpha - gAlphaCutoff);
    s.ndv = saturate(dot(s.N, v.V));
    // The dielectric base reflectance is authored rather than the usual hardcoded 0.04, because
    // 0.04 is right for common dielectrics and wrong for water, gemstones and coated surfaces.
    // A metal has no dielectric base at all, which is why this lerps to the albedo.
    s.F0 = lerp(gMatReflectance.xxx, s.albedo, s.metallic);
    s.F = fresnelSchlick(saturate(dot(s.H, v.V)), s.F0, s.f90);
    // The diffuse response is the NON-METAL FRACTION and nothing else. It used to be
    // (1 - F) * (1 - metallic), which weighted diffuse by the SPECULAR Fresnel evaluated at HdotV
    // -- a hand-wave at energy conservation, and a directional one: H depends on the dominant
    // light, so it says nothing at all about how much energy the sky or a bounce delivered. The
    // error compounded rather than cancelling, because this one kd is reused by averShadeDirect's
    // diffuse, by the sky ambient and by the GI term, so a single over-darkening was applied THREE
    // times to every surface, worst at grazing angles where (1 - F) falls away fastest.
    //
    // Energy is conserved by the specular lobe being normalised (D and the height-correlated V
    // integrate to the reflected fraction on their own), not by subtracting it from diffuse twice.
    s.kdAlbedo = (1.0 - s.metallic) * s.albedo;
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

// The SHADING normal, after normal mapping. A renderer needs it to build the reflection vector for
// its environment lookup: building R from the geometric normal means the specular reflection ignores
// every normal map in the scene, so a bumped surface reflects as though it were flat while its
// diffuse shading says otherwise. AverSurface stays opaque -- this is a function, like every other
// thing a renderer is allowed to know about a shaded point.
float3 averShadingNormal(AverSurface s) { return s.N; }

// The split-sum environment BRDF: the DFG integral of the GGX lobe over the hemisphere, as Karis'
// analytic fit rather than a lookup texture. Returns (A, B) such that the reflected fraction of
// prefiltered environment radiance is F0 * A + B.
//
// This is the term that was MISSING, and its absence was not a small error. What stood in for it was
// `* (1.0 - rough)`, a linear ramp that drives the reflected fraction to zero as a surface roughens.
// Roughness does not delete reflected energy, it SPREADS it: A stays in the 0.4..0.9 band across the
// whole roughness range and never approaches zero. The consequence was worst exactly where it is
// least affordable -- a metal has kdAlbedo = (1 - metallic) * albedo = 0, so the environment term is
// the ONLY indirect light it receives, and a rough metal was receiving a fifth of it.
//
// A fit and not a LUT because a LUT needs a texture, an SRV and a slot in a root signature that
// currently declares no descriptor table at all -- see the cloud noise for the same trade.
float2 averEnvBRDF(float ndv, float rough) {
    const float4 c0 = float4(-1.0, -0.0275, -0.572,  0.022);
    const float4 c1 = float4( 1.0,  0.0425,  1.04,  -0.04);
    float4 r = rough * c0 + c1;
    float a004 = min(r.x * r.x, exp2(-9.28 * ndv)) * r.x + r.y;
    return float2(-1.04, 1.04) * a004 + r.zw;
}

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
        float ndl = saturate(dot(s.N, l.direction));
        float D = distGGX(saturate(dot(s.N, s.H)), a);
        // V already carries the 1/(4*ndv*ndl), so there is no division here and no epsilon to bias
        // it. Both D and V take the same alpha, which is the whole reason `a` is computed once.
        float V = visSmithCorrelated(s.ndv, ndl, a);
        float3 spec = D * V * s.F;
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
        // SPLIT-SUM, not a Fresnel times a roughness ramp. `ind.specular` is the prefiltered
        // environment radiance; what multiplies it is the environment BRDF, F0 * A + B. The old
        // expression -- fresnelSchlick(ndv) * (1 - rough) -- was wrong twice over: it used the raw
        // Fresnel of a single microfacet where the integrated D and G terms belong, and it deleted
        // energy with roughness instead of spreading it.
        float2 dfg = averEnvBRDF(s.ndv, s.rough);
        float3 envSpec = ind.specular * (s.F0 * dfg.x + dfg.y);
        float3 indirect = s.kdAlbedo * ind.diffuse;
        // Two occlusions, deliberately both: ind.occlusion is what the RENDERER resolved for this
        // point (the cone trace's AO), s.occlusion is what the MATERIAL authored into its map, and
        // neither can stand in for the other -- a baked crevice is invisible to a cone trace at
        // voxel resolution, and a cone trace knows about geometry the map was never baked against.
        ambient *= ind.occlusion * s.occlusion;
        // The environment is occluded too. A surface inside a room does not see the outdoor sky, and
        // giving the specular term no occlusion at all is what makes an interior reflect a bright sky
        // through its own walls. The renderer's cone-traced AO is the honest answer available here;
        // the material's baked map is deliberately NOT applied, because a crevice map describes
        // diffuse self-shadowing and a mirror in a crevice still reflects.
        radiance += ambient;
        radiance += indirect;
        // Weight ONE. It was 0.35, which discarded 65% of the reflected energy of every surface in
        // the engine -- a global dimmer with no derivation behind it, compensating for the missing
        // DFG term above by making everything too dark instead of only rough metals.
        radiance += envSpec * ind.occlusion;
        radiance += s.emissive;
        return radiance;
    }
    }
}
)";
}

// One define per slot rather than one base plus arithmetic: the HLSL preprocessor pastes tokens but
// cannot evaluate `t##(base+1)`, so the addition has to happen here.
std::string materialShaderDefines(u32 tableBaseRegister, u32 samplerRegister) {
    std::string s = "AVER_MATERIAL_SRV=" + std::to_string(tableBaseRegister);
    for (u32 i = 1; i < kTextureSlotCount; ++i)
        s += ";AVER_MATERIAL_SRV_" + std::to_string(i) + "=" + std::to_string(tableBaseRegister + i);
    s += ";AVER_MATERIAL_SAMPLER=" + std::to_string(samplerRegister);
    return s;
}

} // namespace aver::pbr
