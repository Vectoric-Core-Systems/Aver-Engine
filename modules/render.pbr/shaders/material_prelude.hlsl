
#ifdef AVER_MATERIAL_SRV
#define AVER_MAT_JOIN2(a, b) a##b
#define AVER_MAT_JOIN(a, b) AVER_MAT_JOIN2(a, b)
// Slot order is pbr::TextureSlot's, so a slot index IS its register offset.
Texture2D gBaseColorMap  : register(AVER_MAT_JOIN(t, AVER_MATERIAL_SRV));
Texture2D gMetalRoughMap : register(AVER_MAT_JOIN(t, AVER_MATERIAL_SRV_1));
Texture2D gNormalMap     : register(AVER_MAT_JOIN(t, AVER_MATERIAL_SRV_2));
Texture2D gOcclusionMap  : register(AVER_MAT_JOIN(t, AVER_MATERIAL_SRV_3));
Texture2D gEmissiveMap   : register(AVER_MAT_JOIN(t, AVER_MATERIAL_SRV_4));
// Second layer, blended in by slope under AVER_MAT_SLOPE_BLEND. Same slot-is-register rule as
// above, continuing pbr::TextureSlot's order.
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
    // Mirrors MaterialConstants::graphId; 0 means "no graph" (averEvalMaterial's generated
    // `default:`, see pbr::materialGraphHlsl()). Lives in the block every material path already
    // binds, so a graph-shaded draw needs nothing extra.
    uint   gMaterialGraphId;

    // Mirrors MaterialConstants::ior/transmission (this comment has been wrong twice before -- once
    // claiming gIor drove averTotalInternalReflection's critical angle, once "read by nothing" --
    // trust the code, not history). gIor is read directly by voxi.hlsl's averRefractedBackdropUV
    // (`max(gIor, 1.0001)`) to bend the refracted backdrop UV on both the screen-space and
    // ray-traced paths, whenever refractionMode is not Off (the DEFAULT) -- this prelude is
    // prepended to voxi.hlsl, so it is the same global. F0 (the physically correct consumer,
    // F0 = ((1-n)/(1+n))^2) is authored SEPARATELY as `reflectance`, so the two can contradict
    // each other (M_Glass.ocmat's comment syncs them by hand). gTransmission feeds
    // AVER_MAT_ALPHA_BLEND's coverage term in averBuildSurface. Both copy into AverAuthored so a
    // material graph can drive either per pixel.
    float  gIor;
    float  gTransmission;
    // Mirrors MaterialConstants::_pad0/_pad1, since spent on these two fields -- no padding left on
    // either side. Not load-bearing alone (HLSL rounds a cbuffer to its last 16-byte register
    // regardless, so gIor/gTransmission alone already reserve to byte 96), kept so this block still
    // mirrors that struct byte for byte.
    float  gSubsurfaceWeight;
    float  gSubsurfaceRadius;
    // The coat row. Mirrors coatWeight/coatRoughness/coatF0/_coatPad, in that order -- the row that
    // takes the block to 112 bytes.
    float  gCoatWeight;
    float  gCoatRoughness;
    float  gCoatF0;
    float  _gCoatPad;

    // Mirrors MaterialConstants::texIndex, kept even though the raster path never reads it:
    // shortening the cbuffer would silently break MaterialGpu.hpp's byte-for-byte assertion while
    // still working (the tail just goes unaddressed). Raster resolves textures via a per-draw
    // descriptor table; these indices exist for the ray path, which has no such table and shades
    // every material in one pass.
    uint4  gTexIndex0;   // slots 0..3: BaseColor, MetalRough, Normal, Occlusion
    uint4  gTexIndex1;   // slots 4..7: Emissive, Layer1BaseColor, Layer1MetalRough, Layer1Normal

    // Volume absorption, mirroring attenuationColor/Distance (row 144->160 bytes). Transmittance
    // after exactly `attenuationDistance` CENTIMETRES; <= 0 means no volume (every material before
    // this row). Read only through averVolumeTransmittance below.
    float3 gAttenuationColor;
    float  gAttenuationDistance;

    // Lamp light, mirroring lightIntensity (row 160->176 bytes). Multiplier on the light the
    // material's glow/size physically casts at 1 m, in the sun's own units (VoxiRenderer::
    // buildLocalLights does that math), not a brightness itself. Read only by the ray-driven
    // local-light pass (CSRdLocalLights); this prelude only transports it.
    float  gLightIntensity;
    // Mirrors MaterialConstants::subsurfaceColor, LINEAR -- the tint light takes inside a subsurface
    // material. Read through AverAuthored::subsurfaceColor, never directly (see that field).
    float3 gSubsurfaceColor;
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
// MaterialDesc::castShadow. Mirrors MaterialFlag_CastShadow; read by rtShadow's traversal loop in
// VoxiShaders.hpp, which is the field's first and only consumer anywhere in the engine.
#define AVER_MAT_CAST_SHADOW    (1u << 13)
#define AVER_MAT_SUBSURFACE     (1u << 14)
#define AVER_MAT_COAT           (1u << 15)
// MaterialDesc::lightIntensity > 0. Read by the ray-driven local-light pass (promotes a draw to a
// sphere light, CSRdLocalLights) and by voxi_restir.hlsli (skips a promoted emitter's own emission
// in GI candidate hits); this prelude itself never branches on the bit.
#define AVER_MAT_LIGHT          (1u << 16)

// Shading model ids. The id arrives per draw in gShadingModel and is dispatched by a uniform switch.
#define AVER_MODEL_STANDARD 0u   // metallic / roughness, Cook-Torrance GGX
#define AVER_MODEL_UNLIT    1u   // authored colour, no lighting and no camera post

// Schlick Fresnel. f90 is the grazing-angle reflectance.
float3 fresnelSchlick(float ct, float3 F0, float f90){ return F0 + (f90-F0)*pow(saturate(1.0-ct),5.0); }

// ---- volume absorption: Beer-Lambert across a known thickness ----
//
// THE ONE IMPLEMENTATION BOTH PATHS CALL, taking attenuationColor/Distance as PARAMETERS rather than
// reading the globals: PSRayDriven's ray hit has no material cbuffer bound (RtMaterial comes from a
// StructuredBuffer instead), and reaching for gAttenuationColor would silently shade every
// ray-driven pixel with whatever material the last raster draw left in b2 -- a mistake made once.
//
// attenuationColor is the transmittance after `attenuationDistance` cm, so extinction is
// -log(colour)/distance and transmittance over `thicknessCm` is exp(-extinction*thickness), written
// as the equivalent pow() (one fewer transcendental, no overflowing intermediate):
//     exp(log(c) * (t / d))  ==  pow(c, t / d)
//
// Returns float3(1,1,1) (no volume) for any material that did not author one; guarded on
// distance <= 0 rather than a flag bit, since a zero-initialised material is already correct.
float3 averVolumeTransmittance(float3 attenuationColor, float attenuationDistance, float thicknessCm) {
    if (attenuationDistance <= 0.0) return float3(1.0, 1.0, 1.0);
    // Negative thickness (no exit found, or a depth sample behind the near plane) means "no path
    // through the medium", not a negative exponent that would AMPLIFY the light into a glowing pool.
    const float t = max(thicknessCm, 0.0);
    return pow(max(attenuationColor, 1e-4), t / attenuationDistance);
}
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
    // True where the geometric normal pointed away and was flipped: this pixel is the BACK of the
    // surface, eye inside the volume the front face encloses. averVertexOf already computes this to
    // flip the normal; keeping it is the whole plumbing cost of total internal reflection.
    bool backFace;
};

// One light's contribution as the renderer resolved it.
struct AverLight {
    float3 direction;   // unit vector TO the light
    float3 radiance;    // linear radiance arriving along `direction`
    // Per channel, so light through a tinted medium keeps that medium's colour. (0,0,0) = fully
    // occluded, (1,1,1) = fully lit; a scalar promotes, so a caller with only scalar visibility
    // (the raster shadow-map path) needed no edit -- it is only ever multiplied in below.
    float3 visibility;
};

// Everything reaching the surface that did not come straight from a light, as RAW radiance.
struct AverIndirect {
    float3 ambient;      // sky-hemisphere irradiance the renderer sampled for this surface
    float  ambientScale; // weight for `ambient`, applied after the diffuse response
    float3 diffuse;      // bounced radiance (zero when the renderer traces none)
    float  occlusion;    // ambient occlusion; weights `ambient` only, never `diffuse`
    float3 specular;     // environment radiance along the reflection vector
    float  specularTraced; // 1: `specular` came from a ray, which already met its occluders
};

// Builds an AverVertex from the shared vertex/mesh shaders' output.
AverVertex averVertexOf(VSOut i) {
    AverVertex v;
    v.wpos = i.wpos;
    v.N    = normalize(i.nrmWS);
    v.V    = normalize(gCamPos.xyz - i.wpos);
    // Two-sided shading, matching plainShadeSurface in rhi::sharedShaderPrelude (see its comment
    // for why dot(N, V) and not SV_IsFrontFace). Duplicated in both preludes because a
    // material-shaded draw never goes through the plain path; without it a one-sheet leaf drawn
    // with culling off shades its back side black.
    v.backFace = dot(v.N, v.V) < 0.0;
    if (v.backFace) v.N = -v.N;
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
    // The DIELECTRIC F0 this surface was built from. Carried here, not read back off gMatReflectance,
    // because a RAY HIT HAS NO MATERIAL CBUFFER BOUND (PSRayDriven defaults this to 0.04 by hand);
    // reaching for the global measured a white dielectric at 1.030 on the ray path vs 1.000 on raster
    // -- diffuse stopped being charged for the specular reflectance it takes off the top.
    float  reflectance;
    // Subsurface, weight 0 where not asked for, so averDirectTerms' term is identically zero.
    // PSRayDriven hand-builds this struct and must set them -- HLSL does not zero one for you.
    float  sssWeight, sssRadius;
    float3 sssColor;     // linear tint of the scattered light, multiplied onto kdAlbedo
#ifdef AVER_LAYERED_BSDF
    // On the surface, not read from the cbuffer at the use site, for the same reason as
    // sssWeight/sssRadius: PSRayDriven has no material cbuffer bound and hand-builds this struct
    // from gRtMaterials, so reading gCoatWeight would get whatever the cbuffer last held.
    float  coatWeight, coatRough, coatF0;
#endif
    // The volume this surface belongs to, same reason as sssWeight/sssRadius: a graph may have
    // overridden these per pixel, and reading gAttenuationColor directly would ignore that.
    float3 attenuationColor;
    float  attenuationDistance;
    // Carried through from AverVertex -- see its own comment. Read only by the transmissive branch
    // below, for total internal reflection.
    bool   backFace;
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
    float  normalLen;   // the filtered map normal's length before scaling: < 1 where its mips averaged varied normals
    float  occlusion;
    float3 emissive;
};

// World-aligned UV (AVER_MAT_WORLD_UV): a planar projection in the object's frame, onto the plane the
// normal faces most. THE ONE COPY: raster (averSurfaceUV, the draw's gWorld) and every ray hit
// (voxi_rt.hlsli's averRtSurfaceUV, the hit instance's objectToWorld) call it with their own frame.
float2 averWorldUV(float4x4 objectToWorld, float3 wpos, float3 N, float tilesPerCm) {
    const float3 ax = normalize(objectToWorld[0].xyz);
    const float3 ay = normalize(objectToWorld[1].xyz);
    const float3 az = normalize(objectToWorld[2].xyz);
    const float3 d  = wpos - objectToWorld[3].xyz;
    const float3 op = float3(dot(d, ax), dot(d, ay), dot(d, az));   // position in the object's frame
    const float3 a  = abs(float3(dot(N, ax), dot(N, ay), dot(N, az)));
    const float2 p  = (a.z >= a.x && a.z >= a.y) ? op.xy : ((a.x >= a.y) ? op.yz : op.xz);
    return p * tilesPerCm;
}

// How much of a material's second layer a point takes: 0 flat ... 1 steep, by the GEOMETRIC normal's
// slope (a normal-mapped one would make the layer flicker with every bump). 0 without AVER_MAT_SLOPE_BLEND.
float averSlopeLayerWeight(uint flags, float lo, float hi, float3 geoN) {
    if (!(flags & AVER_MAT_SLOPE_BLEND)) return 0.0;
    return 1.0 - smoothstep(lo, hi, saturate(abs(geoN.z)));
}

// The texture coordinate this surface is sampled at: the mesh's own, or a planar projection.
float2 averSurfaceUV(AverVertex v) {
    if (gMaterialFlags & AVER_MAT_WORLD_UV) return averWorldUV(gWorld, v.wpos, v.N, gUvTilesPerCm);
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
    m.normalLen  = (gMaterialFlags & AVER_MAT_NORMAL_MAP) ? length(n) : 1.0;
    m.occlusion  = gOcclusionMap.Sample(gMaterialSampler, uv).r;
    m.emissive   = gEmissiveMap.Sample(gMaterialSampler, uv).rgb;
#else
    m.baseColor  = float4(1, 1, 1, 1);
    m.metalRough = float2(1, 1);
    m.normalTS   = float3(0, 0, 1);
    m.normalLen  = 1.0;
    m.occlusion  = 1.0;
    // White, not black: a.emissive = gEmissiveFactor * map.emissive multiplies, so the identity is 1
    // (black zeroed every emissiveFactor-only material, e.g. a lamp bulb with no emissive texture).
    m.emissive   = float3(1, 1, 1);
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
    if (m <= 0.0) {
        // NO USABLE UV FRAME (ddx/ddy of constant UVs collapse T and B to zero -- common on
        // generated geometry). Used to fall back to the geometric normal, silently dropping the
        // perturbation: a Normal-driven graph could compile, dispatch, and still change nothing
        // (measured 0 differing pixels, graph on vs off, on the pool's fluid shell). Instead build a
        // WORLD-ANCHORED frame -- not from dp1/dp2, which are screen-space and would rotate with the
        // camera and make a pattern swim -- from the world axis least parallel to N. The tangent
        // direction is arbitrary (no authored one exists without UVs); what matters is that the
        // frame stays orthonormal, continuous, and stable in world space.
        const float3 up = abs(N.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
        const float3 T2 = normalize(cross(up, N));
        const float3 B2 = cross(N, T2);
        return normalize(T2 * nTS.x + B2 * nTS.y + N * nTS.z);
    }
    float invmax = rsqrt(m);
    return normalize(T * (nTS.x * invmax) + B * (nTS.y * invmax) + N * nTS.z);
}

// Blends the second layer over the first by SLOPE and returns the combined maps.
//
// Slope is taken from the GEOMETRIC normal, deliberately, not the normal-mapped one: the question
// is "is this part of the terrain a cliff", a property of the surface -- feeding a normal map into
// it would make the layer choice flicker with every bump in the detail.
AverMaps averBlendLayers(AverMaps m, float2 uv, float3 geoN) {
#ifdef AVER_MATERIAL_SRV
    // How much of LAYER 1 to take, by slope: the one weight every path uses.
    const float w = averSlopeLayerWeight(gMaterialFlags, gSlopeBlendLo, gSlopeBlendHi, geoN);
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
        m.normalLen = lerp(m.normalLen, length(n1), w);
    }
#endif
    return m;
}

// ================= what a material GRAPH can reach =================
// These exist for generated code and are used by nothing else in this file. They are here rather
// than emitted inline into every graph for the reason averSampleMaps is here: each one wraps a
// conditional or a constant table that a generator would otherwise have to restate, correctly, once
// per node instance -- and the first one it got wrong would be a material that shades subtly wrong
// with nothing to point at.

// One material map, sampled at an arbitrary UV rather than the surface's own.
//
// The slot is a SWITCH, not an array index: the eight maps are eight separately declared Texture2Ds
// at eight registers (see the AVER_MATERIAL_SRV block at the top of this file), and HLSL cannot
// index that without a resource array the root signature does not describe. `slot` is constant per
// generated node, so the compiler folds the switch away entirely -- it costs nothing at runtime.
//
// Returns white with no material table at all, matching averSampleMaps' own #else branch: a graph
// sampling a map in a build with no material SRVs gets the identity, not a compile error.
float4 averSampleSlot(uint slot, float2 uv) {
#ifdef AVER_MATERIAL_SRV
    switch (slot) {
    case 0u: return gBaseColorMap.Sample(gMaterialSampler, uv);
    case 1u: return gMetalRoughMap.Sample(gMaterialSampler, uv);
    case 2u: return gNormalMap.Sample(gMaterialSampler, uv);
    case 3u: return gOcclusionMap.Sample(gMaterialSampler, uv);
    case 4u: return gEmissiveMap.Sample(gMaterialSampler, uv);
    case 5u: return gL1BaseColorMap.Sample(gMaterialSampler, uv);
    case 6u: return gL1MetalRoughMap.Sample(gMaterialSampler, uv);
    case 7u: return gL1NormalMap.Sample(gMaterialSampler, uv);
    default: break;
    }
#endif
    return float4(1.0, 1.0, 1.0, 1.0);
}

// A hash of a 2D cell to [0,1). The usual sin-based one-liner, chosen over a bit-mixing integer hash
// for a reason that outlives taste: this prelude compiles under FXC (SM 5.1) as well as DXC when
// dxcompiler.dll is missing, and the integer ops a good hash wants are not uniformly available
// there. Precision is adequate for surface detail and is not adequate for anything that has to
// AGREE with a hash computed on the CPU -- do not use it for that.
float averHash21(float2 p) {
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453123);
}

// Value noise on a 2D domain, smoothed with the quintic fade so its second derivative is continuous
// and a normal derived from it does not show the lattice.
float averValueNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    float a = averHash21(i);
    float b = averHash21(i + float2(1.0, 0.0));
    float c = averHash21(i + float2(0.0, 1.0));
    float d = averHash21(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}

// A two-colour checker over a 2D domain, as 0 or 1.
float averChecker(float2 p) {
    float2 c = floor(p);
    return frac((c.x + c.y) * 0.5) * 2.0;
}

// Rotates a UV about a centre, by an angle in RADIANS. Radians rather than degrees because every
// other angle in a shader is, and a node that silently disagreed with sin/cos beside it would be a
// trap; the editor is where a degrees affordance belongs if one is ever wanted.
float2 averRotateUv(float2 uv, float2 centre, float angle) {
    float s = sin(angle), c = cos(angle);
    float2 d = uv - centre;
    return centre + float2(d.x * c - d.y * s, d.x * s + d.y * c);
}

// Whiteout blend of two tangent-space normals: keeps the detail of both rather than letting the
// second flatten the first, which is what a plain lerp does and why one is not offered.
float3 averBlendNormals(float3 a, float3 b) {
    return normalize(float3(a.xy + b.xy, a.z * b.z));
}

// Everything a material AUTHORS, nothing else. Every remaining AverSurface field is DERIVED from
// these by averBuildSurface below (F0 from albedo/metallic, F from F0 and the half vector, kdAlbedo
// from albedo/metallic, ndv from the shading normal).
//
// This split is what lets a node graph drive a material without restating the BRDF: a generated
// averEvalMaterial starts from averStockAuthored(), overwrites only the fields its graph drives, and
// hands the result to averBuildSurface -- so a graph setting only base colour still gets the right
// F0, energy split and alpha clip, and a change to how a surface is DERIVED is made once instead of
// once per generated shader.
//
// AUTHORED HALVES ONLY -- the per-material factor times the map. The per-DRAW terms (gMaterial,
// gBaseColor, gEmissive, gShadingModel) stay in averBuildSurface: they are the renderer's to apply,
// not the material's to author, so a graph overriding them would override the entity's own tint.
struct AverAuthored {
    float3 baseColor;   // linear; gBaseColorFactor.rgb * the base colour map
    float  opacity;     // gBaseColorFactor.a * the base colour map's alpha
    float  metallic;    // gMetallicFactor * the map, before saturate()
    float  roughness;   // gRoughnessFactor * the map, before the 0.045 floor
    float3 normalTS;    // tangent space; (0, 0, 1) is no perturbation
    float  normalLen;   // AverMaps::normalLen: the specular anti-aliasing's variance
    float3 emissive;    // gEmissiveFactor * the map
    float  occlusion;   // the material's OWN occlusion map, before gOcclusionStrength
    float  alphaCutoff; // read only under AVER_MAT_ALPHA_MASK
    // AUTHORED, NOT READ STRAIGHT OFF THE CBUFFER: a field read from the constant buffer directly is
    // one no material GRAPH can ever drive. Subsurface earns this the most -- a thickness mask
    // driving the radius is the difference between a uniformly waxy object and thin parts that glow.
    float  subsurfaceWeight;
    float  subsurfaceRadius;
    float3 subsurfaceColor;   // linear; the colour light takes inside the material
    // Dielectric pair, same reason. Driving transmission from a mask is how one mesh becomes a
    // window with a frosted band, or a bottle with a label.
    float  ior;
    float  transmission;
    // THE VOLUME, AUTHORED PER PIXEL. Mirrors gAttenuationColor/Distance, here so a GRAPH can drive
    // them: real glass's tint is its iron content and thickness, both varying across a pane, which a
    // constant could express neither of. attenuationDistance <= 0 still means "no volume".
    float3 attenuationColor;
    float  attenuationDistance;
    // NOT BEHIND AVER_LAYERED_BSDF, though only that define reads it: gating it would make
    // AverAuthored's shape depend on a render setting, so graph-generated HLSL compiled for one
    // setting could fail to compile under the other. Three dead floats is the cheaper trade.
    float  coatWeight;
    float  coatRoughness;
    float  coatF0;
};

// ================= ONE MATERIAL, EVERY PATH =================
// A material is the same value whichever path shades it. AverMaterialData is pbr::MaterialConstants
// (MaterialGpu.hpp) field for field: raster reads it out of the b2 cbuffer (averMaterialData), a ray
// hit reads the identical struct out of the material table (voxi_rt.hlsli's gRtMaterials, whose
// RtMaterial is this type). Everything below that turns it into a surface takes it as a PARAMETER,
// so raster and every ray path run one composition, and a material graph edits the same
// AverAuthored in both.
struct AverMaterialData {
    float4 baseColorFactor;   // rgb LINEAR
    float3 emissiveFactor;
    float  metallicFactor;
    float  roughnessFactor;
    float  normalScale;
    float  occlusionStrength;
    float  alphaCutoff;
    uint   flags;
    float  reflectance;
    float  f90;
    float  uvTilesPerCm;
    float  slopeBlendLo;
    float  slopeBlendHi;
    float  layer1UvScale;
    uint   graphId;
    float  ior;
    float  transmission;
    float  subsurfaceWeight;
    float  subsurfaceRadius;
    float  coatWeight;
    float  coatRoughness;
    float  coatF0;
    float  _coatPad;
    // BaseColor, MetalRough, Normal, Occlusion, Emissive, L1 BaseColor, L1 MetalRough, L1 Normal;
    // 0xFFFFFFFF unbound. Read by the ray path's bindless table; raster binds the maps per draw.
    uint   texIndex[8];
    float3 attenuationColor;
    float  attenuationDistance;
    float  lightIntensity;
    float3 subsurfaceColor;
};

// The b2 cbuffer as one AverMaterialData.
AverMaterialData averMaterialData() {
    AverMaterialData m;
    m.baseColorFactor = gBaseColorFactor;
    m.emissiveFactor = gEmissiveFactor;
    m.metallicFactor = gMetallicFactor;
    m.roughnessFactor = gRoughnessFactor;
    m.normalScale = gNormalScale;
    m.occlusionStrength = gOcclusionStrength;
    m.alphaCutoff = gAlphaCutoff;
    m.flags = gMaterialFlags;
    m.reflectance = gMatReflectance;
    m.f90 = gMatF90;
    m.uvTilesPerCm = gUvTilesPerCm;
    m.slopeBlendLo = gSlopeBlendLo;
    m.slopeBlendHi = gSlopeBlendHi;
    m.layer1UvScale = gL1UvScale;
    m.graphId = gMaterialGraphId;
    m.ior = gIor;
    m.transmission = gTransmission;
    m.subsurfaceWeight = gSubsurfaceWeight;
    m.subsurfaceRadius = gSubsurfaceRadius;
    m.coatWeight = gCoatWeight;
    m.coatRoughness = gCoatRoughness;
    m.coatF0 = gCoatF0;
    m._coatPad = 0.0;
    m.texIndex[0] = gTexIndex0.x; m.texIndex[1] = gTexIndex0.y;
    m.texIndex[2] = gTexIndex0.z; m.texIndex[3] = gTexIndex0.w;
    m.texIndex[4] = gTexIndex1.x; m.texIndex[5] = gTexIndex1.y;
    m.texIndex[6] = gTexIndex1.z; m.texIndex[7] = gTexIndex1.w;
    m.attenuationColor = gAttenuationColor;
    m.attenuationDistance = gAttenuationDistance;
    m.lightIntensity = gLightIntensity;
    m.subsurfaceColor = gSubsurfaceColor;
    return m;
}

// What the renderer applies per DRAW on top of the material: the entity's tint (LINEAR), its
// metal/rough scales, added glow and shading model. Raster has them in the per-draw cbuffer
// (averDrawTerms); a ray hit has them in its RtInstance (voxi_rt.hlsli's rtDrawTerms).
struct AverDrawTerms {
    float3 tint;
    float  alpha;
    float  metallic;
    float  roughness;
    float3 emissive;
    uint   model;
};

AverDrawTerms averDrawTerms() {
    AverDrawTerms d;
    d.tint      = srgbToLin(gBaseColor.rgb);
    d.alpha     = gBaseColor.a;
    d.metallic  = gMaterial.x;
    d.roughness = gMaterial.y;
    d.emissive  = gEmissive.rgb;
    d.model     = gShadingModel;
    return d;
}

// How much of the second layer a point takes: 0 flat ... 1 steep, by the GEOMETRIC normal's slope
// (a normal-mapped one would make the layer flicker with every bump). 0 without AVER_MAT_SLOPE_BLEND.
float averLayerWeight(AverMaterialData m, float3 geoN) {
    return averSlopeLayerWeight(m.flags, m.slopeBlendLo, m.slopeBlendHi, geoN);
}

// The material's factors times its (already sampled and layer-blended) maps.
AverAuthored averAuthoredFrom(AverMaterialData m, AverMaps map) {
    const float4 base = m.baseColorFactor * map.baseColor;
    AverAuthored a;
    a.baseColor   = base.rgb;
    a.opacity     = base.a;
    a.metallic    = m.metallicFactor * map.metalRough.y;
    a.roughness   = m.roughnessFactor * map.metalRough.x;
    a.normalTS    = map.normalTS;
    a.normalLen   = map.normalLen;
    a.emissive    = m.emissiveFactor * map.emissive;
    a.occlusion   = map.occlusion;
    a.alphaCutoff = m.alphaCutoff;
    // GATED HERE ONCE: nothing below re-tests the flag, so a graph pin written after the stock path
    // runs is not vetoed by it.
    a.subsurfaceWeight = (m.flags & AVER_MAT_SUBSURFACE) ? saturate(m.subsurfaceWeight) : 0.0;
    a.subsurfaceRadius = (m.flags & AVER_MAT_SUBSURFACE) ? saturate(m.subsurfaceRadius) : 0.0;
    a.subsurfaceColor  = m.subsurfaceColor;
    a.ior              = m.ior;
    a.transmission     = m.transmission;
    a.attenuationColor    = m.attenuationColor;
    a.attenuationDistance = m.attenuationDistance;
    a.coatWeight       = (m.flags & AVER_MAT_COAT) ? saturate(m.coatWeight)    : 0.0;
    a.coatRoughness    = (m.flags & AVER_MAT_COAT) ? saturate(m.coatRoughness) : 0.0;
    a.coatF0           = (m.flags & AVER_MAT_COAT) ? saturate(m.coatF0)        : 0.0;
    return a;
}

// What the stock material path authors on raster: the five maps, blended by slope, times the b2 factors.
AverAuthored averStockAuthored(float2 uv, float3 geoN) {
    AverMaps map = averSampleMaps(uv);
    map = averBlendLayers(map, uv, geoN);
    return averAuthoredFrom(averMaterialData(), map);
}

// THE SURFACE, from authored values, the material and the draw's terms, at shading normal N (already
// perturbed by whichever path has the tangent frame). The one place every renderer derives F0, the
// energy split, coverage and the rest. No clip(): raster's averBuildSurface does that (compute can't).
AverSurface averComposeSurface(AverVertex v, AverLight l, AverAuthored a, AverMaterialData m,
                               AverDrawTerms d, float3 N) {
    AverSurface s;
    s.N = N;
    s.V = v.V;
    s.H = normalize(v.V + l.direction);
    s.metallic = saturate(d.metallic * a.metallic);
    s.rough = clamp(d.roughness * a.roughness, 0.045, 1.0);
    // SPECULAR ANTI-ALIASING (Toksvig 2005): a mip-filtered normal shorter than 1 averaged normals
    // that disagree inside this pixel; widening the GGX lobe by that variance turns sub-pixel glints
    // (a wet road under a city of small lights) into the sheen they average to.
    {
        const float len = clamp(a.normalLen, 0.25, 1.0);
        const float a2  = s.rough * s.rough * s.rough * s.rough;
        s.rough = sqrt(sqrt(min(a2 + (1.0 - len) / len, 1.0)));
    }
    s.alpha = d.alpha * a.opacity;
    s.model = d.model;
    s.emissive = d.emissive + a.emissive;
    s.occlusion = lerp(1.0, a.occlusion, m.occlusionStrength);
    s.f90 = m.f90;
    s.reflectance = m.reflectance;
    s.display = d.model == AVER_MODEL_UNLIT;
    s.albedo = d.tint * a.baseColor;
    // s.albedo, not gBaseColor alone -- that was two bugs, both rendering as WHITE: draw loops
    // neutralise gBaseColor to 1,1,1 for a material carrying colour in a texture (SandboxApp's
    // authored branch, GameRender.cpp), dropping a.baseColor drops the whole colour, and skipping
    // srgbToLin hands an sRGB triple to a linear-expecting tonemap (as plainShadeSurface's selection
    // outline once did). s.alpha, not gBaseColor.a, so unlit fades with opacity like everything else.
    // Plus s.emissive, since Unlit (the only user of displayColor -- setUnlit; SandboxRender.cpp
    // clears it first on overlay/chrome) removes lighting, not a lamp's own glow.
    s.displayColor = float4(s.albedo + s.emissive, s.alpha);
    s.ndv = saturate(dot(s.N, v.V));
    s.F0 = lerp(m.reflectance.xxx, s.albedo, s.metallic);
    s.F = fresnelSchlick(saturate(dot(s.H, v.V)), s.F0, s.f90);
    // TRANSMISSION REMOVES LIGHT FROM THE DIFFUSE LOBE (it did not, before this line): light that
    // passed THROUGH the substrate is the same photons as light scattered back out, so a material
    // transmitting 92% and diffusely reflecting its full base colour emits energy it never received
    // -- every other use of transmission here was on s.alpha (coverage) alone.
    //
    // MEASURED on PTTest's M_Glass (0.86 0.93 0.88 0.12, transmission 0.92, over the dark pool): the
    // undimmed diffuse term alone contributed a warm wash five times the size of the reflection
    // (specular was 4 codes of a 103,124,130 pixel; coverage-only matched the water behind it within
    // 1-3 codes) -- the "milky, cartoonish glass" this fixes; the reflection, Fresnel, alpha and
    // blend state were each verified correct first, so it was a diffuse lobe nobody dimmed. (M_Glass
    // has since moved its tint into attenuationColor; these numbers are the original evidence, not a
    // value to reproduce.)
    //
    // (1 - gTransmission), matching glTF KHR_materials_transmission. NOT gated on AVER_MAT_ALPHA_BLEND
    // -- transmission is a substrate property, not a blend mode -- and a no-op by default (0).
    s.kdAlbedo = (1.0 - s.metallic) * s.albedo * (1.0 - saturate(a.transmission));
    s.attenuationColor    = a.attenuationColor;
    s.attenuationDistance = a.attenuationDistance;
    s.backFace  = v.backFace;
    // FROM THE AUTHORED STRUCT, not the cbuffer: reading gSubsurfaceWeight directly would work for
    // the stock material but silently ignore any graph that drove the pin -- the defect this struct
    // exists to prevent.
    s.sssWeight = saturate(a.subsurfaceWeight);
    s.sssRadius = saturate(a.subsurfaceRadius);
    s.sssColor  = max(a.subsurfaceColor, 0.0);
#ifdef AVER_LAYERED_BSDF
    // FROM THE AUTHORED STRUCT, same reason as subsurface above (it read the cbuffer when the lobe
    // first landed, before the pins existed; fixed here rather than left as a second wrong precedent).
    // averStockAuthored already applied AVER_MAT_COAT, so a coat-less material arrives as zero and
    // every coat term below is identically zero.
    s.coatWeight = saturate(a.coatWeight);
    s.coatRough  = saturate(a.coatRoughness);
    s.coatF0     = saturate(a.coatF0);
#endif

    // FRESNEL-AWARE ALPHA FOR BLENDED SURFACES. Plain "over" (dst = src*alpha + dst*(1-alpha)) treats
    // alpha as one uniform attenuation, hitting specular as hard as everything else. Real dielectrics
    // do not: Fresnel climbs toward 1 at grazing incidence, so a pane at a transparent alpha (~0.2)
    // would draw its own reflection at only 20% strength -- too dim to read, so glass looks like
    // dirty plastic. Raising alpha toward 1 by the same view-angle Fresnel term this file already
    // computes fixes it with no second BRDF: at grazing incidence alpha follows the term to 1 and the
    // draw goes fully opaque there, correctly, since virtually all the light reaching the eye at that
    // angle IS the reflection; face-on the term sits near s.reflectance (glass ~0.04).
    //
    // GATED ON AVER_MAT_ALPHA_BLEND: an unaffected material gets exactly the two lines above
    // (gBaseColor.a * a.opacity, optionally clip()'d), provably bit-identical -- Material.cpp reported
    // Feature::AlphaBlend as Status::NotImplemented until this change, so nothing shipped a material
    // expecting a renderer to act on the bit.
    if (m.flags & AVER_MAT_ALPHA_BLEND) {
        // TRANSMISSION SETS THE FLOOR FIRST; FRESNEL LIFTS IT SECOND. gTransmission > 0 is a fact
        // about the substrate regardless of camera position, so it must apply before anything
        // view-dependent -- a floor the view term then lifts away from. Reversed, a transmissive
        // pane's grazing brightening would get pulled back down by transmission afterwards, which is
        // backwards: the Fresnel reflection is real light reaching the eye, and transmission has no
        // say over light that never entered the substrate.
        //
        // lerp toward (1 - gTransmission), not a multiply: at transmission == 1 the 0-coverage target
        // wins outright regardless of authored alpha; at 0 the lerp weight is zero and s.alpha passes
        // through unchanged (a no-op for material predating the field, which defaults to 0).
        float baseAlpha = lerp(s.alpha, 1.0 - a.transmission, saturate(a.transmission));

        // s.F is at the HALF VECTOR for the current light, swinging per light -- a poor knob for how
        // mirror-like the surface reads to the CAMERA regardless of lighting. View-angle Fresnel
        // (fresnelSchlick at s.ndv = dot(N,V)) depends only on the surface and the eye.
        float3 viewFresnel = fresnelSchlick(s.ndv, s.F0, s.f90);
        // By luminance, not a channel pick: F0 is achromatic here (metallic 0 gives every channel
        // gMatReflectance), and luminance is the principled reduction for the tinted/metallic case.
        float fresnelLum = dot(viewFresnel, float3(0.2126, 0.7152, 0.0722));
        // NO TOTAL-INTERNAL-REFLECTION OVERRIDE HERE, DELIBERATELY: there WAS one, gated on
        // s.backFace, and it turned every pane of glass into a dark slab at grazing angles. TIR needs
        // the ray already INSIDE the denser medium; backFace does not mean that -- on a two-sided
        // pane (glass is routinely CULL none) it is true both for an ordinary outside view of the far
        // surface AND for a genuine inside-the-medium view, indistinguishable in a pixel shader. SNELL
        // FORBIDS THE FIRST OUTRIGHT: light reaching that far surface refracted TOWARD the normal
        // getting in, so the internal angle maxes at the critical angle (asin(1/n) = 41.1 deg at
        // n=1.52) and can never exceed it -- a parallel-sided pane viewed from outside cannot TIR at
        // any angle. MEASURED on PTTest's glass rail at grazing incidence: the override read
        // 127,140,141 against a 203,215,215 background vs 174,187,188 with it gone (fired from
        // ndv < 0.753; face-on unaffected, 195,206,206 either way). The correct pattern is
        // `bool underwater = gCamPos.z < gWaterState.x` in WaterShaders.hpp, which asks whether the
        // EYE is inside the medium using geometry that actually answers it; a material has no
        // equivalent, so glass gets ordinary view Fresnel here -- the honest answer.
        s.alpha = lerp(baseAlpha, 1.0, saturate(fresnelLum));
    }

    return s;
}

// Raster: the draw's own shading normal (screen-space tangent frame), the composition above with b2
// and the per-draw cbuffer, then the alpha mask's clip.
AverSurface averBuildSurface(AverVertex v, AverLight l, AverAuthored a, float2 uv) {
    const AverSurface s = averComposeSurface(v, l, a, averMaterialData(), averDrawTerms(),
                                             averPerturbNormal(v.N, v.wpos, uv, a.normalTS));
    if (gMaterialFlags & AVER_MAT_ALPHA_MASK) clip(s.alpha - a.alphaCutoff);
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

// SPECULAR OCCLUSION. AO answers "how much of the HEMISPHERE is blocked" -- right for diffuse, wrong
// for a specular lobe gathering from a narrow cone: feeding it raw darkens a grazing mirror that can
// see past its own occluder and barely touches a genuinely enclosed rough surface.
//
// Lagarde's form: the exponent collapses toward 1 as roughness rises, so rough converges on plain AO
// while smooth is freed from it. At ao = 1 it returns 1 for every roughness (the white furnace check).
float averSpecularOcclusion(float ndv, float ao, float rough) {
    // NO saturate() inside the pow: ndv+ao EXCEEDS 1 for a smooth face-on surface, so the small
    // exponent lands ABOVE plain ao. Clamping the base to 1 would collapse the function to `ao`
    // exactly -- the bug this exists to fix (caught in the furnace: metal read 1.000 * ao, unchanged).
    // Base cannot go negative (ao, abs(ndv) both non-negative), so no guard is needed.
    return saturate(pow(abs(ndv) + ao, exp2(-16.0 * rough - 1.0)) - 1.0 + ao);
}

// ================= shared BRDF terms: ONE copy of the maths, three consumers =================
// averShadeDirect/averShadeIndirect below are the ORIGINAL accumulating entry points, unchanged in
// what they compute or the order they add it. averShadeSplit calls the same two helpers but keeps
// their outputs in separate registers instead of summing them, for a PREMULTIPLIED-ALPHA blended
// draw (see the shading contract at the top of this file and averBlendedOutput) -- splitting at the
// helper boundary, not by duplicating either lobe's arithmetic, so a future BRDF change is made once.

// ---- SUBSURFACE: how deep light scatters, and which side its shadow is asked from ----
// subsurfaceRadius [0,1] as a depth in centimetres: how far light travels inside before it is spent.
// Leaves, paper and curtains sit near 0, an ear or a candle's wax mid-range, marble and jade at 1.
float averSubsurfaceDepthCm(float radius) { return lerp(0.25, 5.0, saturate(radius)); }

// Where a subsurface pixel's ONE sun-shadow query should start, as an offset from the surface: zero for
// every other material, and for a subsurface pixel lit from its own side. Lit from BEHIND, the pixel's
// only direct light is what comes through it, and the ordinary query -- leaving the surface on the side
// `N` faces -- crosses the surface itself and always read shadowed, so transmission could never light.
// Pushed the scatter depth through to the light-facing side instead: a sheet thinner than that (leaf,
// curtain, ear) sees the light, a body thicker than it starts inside and meets its own far side. No
// extra ray -- the same query from a different point. `N` is whichever normal the caller's shadow query
// offsets along. Callers add this to the RAY ORIGIN only, so shadow history still reprojects and
// filters at the real surface.
float3 averSubsurfaceShadowPush(uint flags, float radius, float3 N, float3 L) {
    if ((flags & AVER_MAT_SUBSURFACE) == 0u || dot(N, L) >= 0.0) return float3(0.0, 0.0, 0.0);
    return -N * averSubsurfaceDepthCm(radius);
}

// How much of the light on a subsurface surface's far side comes through: thin, short-radius
// materials pass more, a deep scatterer spends more of it inside. Shared by the direct and ambient
// transmission terms so the two cannot disagree.
float averSubsurfaceTransmit(AverSurface s) { return s.sssWeight * lerp(0.6, 0.3, s.sssRadius); }

// Cook-Torrance GGX for one light, returned as its two UNWEIGHTED lobes (before radiance/NdotL/
// visibility) plus the NdotL both callers need. Splitting the return here, not after the light term
// is applied, lets averShadeDirect reconstruct the exact original expression, provably unaffected by
// this function existing. SUBSURFACE IS A FOURTH OUTPUT, not part of diffuseLobe: both callers
// multiply diffuseLobe by ndl, which is ZERO exactly where subsurface light is the only thing there
// is (past the terminator), so folding it in would zero the effect where it matters most.
void averDirectTerms(AverSurface s, AverLight l, out float3 diffuseLobe, out float3 specularLobe,
                     out float3 subsurfaceLobe, out float ndl) {
    float a = s.rough * s.rough;
    ndl = saturate(dot(s.N, l.direction));
    float D = distGGX(saturate(dot(s.N, s.H)), a);
    float V = visSmithCorrelated(s.ndv, ndl, a);
    float3 spec = D * V * s.F;
    // ENERGY LOST TO MASKING, PUT BACK. Single-scatter GGX drops every ray the microsurface would
    // have bounced again; loss grows with roughness (a white metal at roughness 1 returned 45% of
    // its light, measured in this engine's furnace). Reuses the split-sum term averIndirectTerms
    // already computes for the same loss in the environment.
    float2 dfg = averEnvBRDF(s.ndv, s.rough);
    // Ess is THE SUM, not dfg.x (only the SCALE half of "F0*scale + bias"): the single-scatter
    // DIRECTIONAL ALBEDO this compensation inverts is what the pair sums to. dfg.x alone looks right
    // head-on (0.452 vs 0.450 at roughness 1) but at grazing incidence the scale term falls to 0.077
    // while the sum stays near 0.97 -- 1/scale drove the factor to THIRTEEN, and terrain read ~15%
    // too bright everywhere until bisected to this one line.
    float  Ess = max(dfg.x + dfg.y, 1e-3);
    spec *= 1.0 + s.F0 * (1.0 / Ess - 1.0);
    diffuseLobe  = s.kdAlbedo / PI;
    specularLobe = spec;

    // ---- subsurface: wrap, transmission and forward scatter, in the scatter colour ----
    // All three are zero at weight 0, bit-identical to a material without the feature.
    //   WRAP, ONLY THE EXTRA: the DIFFERENCE between a wrapped N.L and the plain one (the caller already
    //   pays kdAlbedo/PI * ndl). (1+w)^2 is energy normalisation -- without it a widened lobe hands the
    //   surface more light than fell on it (skin at weight 1 would read emissive).
    //   TRANSMISSION: light arriving on the FAR side (Lambert against -N) and diffusing through -- a
    //   leaf, curtain or lampshade with the sun behind it. It lights at all only because the caller's
    //   shadow query for this pixel was made from the light-facing side (averSubsurfaceShadowPush).
    //   FORWARD SCATTER: keyed on dot(V, -L) rather than the normal -- the bright rim of an ear or leaf
    //   seen against the sun; radius sharpens (thin) or widens (thick) it.
    float sssW = s.sssWeight;
    float ndlWrap = saturate((dot(s.N, l.direction) + sssW) / ((1.0 + sssW) * (1.0 + sssW)));
    float wrapExtra = max(ndlWrap - ndl, 0.0);
    float transmit = saturate(dot(-s.N, l.direction)) * averSubsurfaceTransmit(s);
    float backScatter = pow(saturate(dot(s.V, -l.direction)), lerp(12.0, 2.0, s.sssRadius)) * s.sssRadius;
    subsurfaceLobe = s.kdAlbedo * s.sssColor / PI * (wrapExtra + transmit + backScatter * sssW);
}

// THE SKY AND THE BOUNCE, THROUGH: a thin subsurface surface is lit from its far side by roughly what
// reaches its near side, so a share of the same diffuse irradiance comes through in the scatter colour --
// why foliage in shade reads lighter and greener than an opaque leaf would. Halved: the far side's
// hemisphere is not this one, and the sky is above rather than behind. Callers skip it at weight 0, so
// no other material's sum moves.
float3 averSubsurfaceAmbient(AverSurface s, AverIndirect ind) {
    const float3 irradiance = ind.ambient * ind.ambientScale * ind.occlusion * s.occlusion + ind.diffuse;
    return s.kdAlbedo * s.sssColor * irradiance * (0.5 * averSubsurfaceTransmit(s));
}

// Adds one light's direct contribution: Cook-Torrance GGX, with NdotL and visibility applied.
float3 averShadeDirect(float3 radiance, AverSurface s, AverLight l) {
    switch (s.model) {
    case AVER_MODEL_UNLIT:
        return radiance;
    default: {
        float3 diffuseLobe, specularLobe, subsurfaceLobe;
        float  ndl;
        averDirectTerms(s, l, diffuseLobe, specularLobe, subsurfaceLobe, ndl);
        // NOT scaled by ndl -- see averDirectTerms. Still scaled by visibility: subsurface light
        // comes from the sun, so a surface in shadow does not glow.
        return radiance + (diffuseLobe + specularLobe) * l.radiance * ndl * l.visibility
                        + subsurfaceLobe * l.radiance * l.visibility;
    }
    }
}

// The four terms averShadeIndirect sums, returned UNSUMMED rather than pre-combined: float addition
// is not associative and this refactor may not move averShadeIndirect's bit-for-bit output, so it
// must add these four in EXACTLY the original sequence. averShadeSplit, with no prior output to
// match, sums the same four terms into its two buckets instead -- see its own comment for why that
// makes it a documented approximation rather than a provable identity.
#ifdef AVER_LAYERED_BSDF
// ================= THE COAT: a second specular layer over everything the base returns =================
//
// A clear film on top of the base material -- car paint, varnish, a wet stone. The base keeps its own
// metallic/roughness response; the coat adds a smoother GGX lobe over it and ATTENUATES what shows
// through by its own Fresnel, which is the only thing that makes this energy-conserving rather than
// energy-adding.
//
// COMPILED ONLY UNDER AVER_LAYERED_BSDF: Settings::layeredBsdf Off gets a shader with none of this in
// it (no branch, no register pressure), so "costs nothing when off" is a fact about the compiled code.
//
// ONE FACTOR OF (1 - Fc), NOT TWO, AND IT IS THE WHOLE CORRECTION. The obvious composition attenuates
// by (1 - Fc(ndv)) twice -- once entering the coat, once leaving -- which is right for a transmitted
// path through a slab and WRONG here: averEnvBRDF already integrates the full hemisphere-to-eye
// response, so a second factor double-charges the base for the same interface. It was written that
// way first and tests/render.pbr/src/CoatEnergyTest.cpp caught it (a furnace plate visibly darker
// than its background at every roughness) before this reached a GPU, and that test mirrors the
// arithmetic below and asserts this function exists -- change one, change both, or the test will say so.
void averCoatTerms(AverSurface s, AverIndirect ind, float coatWeight, float coatRough, float coatF0,
                   out float3 coatEnv, out float baseAttenuation) {
    coatEnv = 0.0;
    baseAttenuation = 1.0;
    if (coatWeight <= 0.0) return;

    // The coat's own environment lobe, through the same split-sum fit the base uses -- averEnvBRDF is
    // not re-derived for the coat, it is the same function asked about a different roughness.
    float2 cdfg = averEnvBRDF(s.ndv, coatRough);
    float  cEnv = coatF0 * cdfg.x + cdfg.y;
    // averSpecularOcclusion for the same reason the base gets it: a smooth lobe gathers from a narrow
    // cone and must not be dimmed by a hemisphere-shaped answer.
    coatEnv = cEnv * coatWeight * ind.specular *
              (ind.specularTraced > 0.5 ? 1.0 : averSpecularOcclusion(s.ndv, ind.occlusion, coatRough));

    // What the base is allowed to return: fresnelSchlick at the VIEW angle, not the half vector -- a
    // property of the surface and the eye alone, the same argument averBuildSurface's alpha branch
    // makes for its own view-angle Fresnel.
    // .x EXPLICITLY, not an implicit float3->float truncation: a scalar coatF0 promotes to three
    // identical channels, so any channel happened to be right before, but DXC warns about it,
    // correctly -- the day the coat gets a coloured F0 that silently keeps only red. Taking .x states
    // that the coat is a colourless dielectric film, which is what a clear coat is.
    baseAttenuation = 1.0 - fresnelSchlick(s.ndv, coatF0, 1.0).x * coatWeight;
}
#endif   // AVER_LAYERED_BSDF

void averIndirectTerms(AverSurface s, AverIndirect ind,
                       out float3 specEnv, out float3 diffAmbient, out float3 diffBounce) {
    // THE SPLIT SUM, WITH THE MULTIPLE SCATTERING PUT BACK (Fdez-Aguera). The old form,
    // `ind.specular * (F0*dfg.x + dfg.y)` plus a full-strength diffuse term, measured wrong in two
    // directions in this engine's white furnace: a white METAL kept only 45% of its energy at
    // roughness 1 (97% at 0.05, light GGX loses to masking never returned), and a DIELECTRIC read
    // 1.5-4.5% too BRIGHT (diffuse handed the whole albedo while specular took its reflectance off
    // the same budget). Both are the same omission: the split sum accounts for one microsurface
    // bounce and nothing else. Ess is what a single bounce returns, Ems what it dropped, Favg the
    // Fresnel averaged over the hemisphere, Fms*Ems every further bounce; kD is what is genuinely
    // left for diffuse -- which is what stops the dielectric over-reading, with no separate fudge for
    // it. Confirmed in the furnace: every cell of the 6x3 roughness/metallic grid lands on 1.000
    // rather than 0.45-1.045.
    float2 dfg    = averEnvBRDF(s.ndv, s.rough);
    float3 FssEss = s.F0 * dfg.x + dfg.y;
    float  Ess    = dfg.x + dfg.y;
    float  Ems    = 1.0 - Ess;
    // The 1/21 is the analytic hemispherical average of the Schlick term, not a tuned number.
    float3 Favg   = s.F0 + (1.0 - s.F0) / 21.0;
    // Guarded because Ems*Favg reaches 1 only when a single bounce returns nothing at all, and a
    // division by zero here would paint NaN across every rough pixel in the frame.
    float3 FmsEms = Ems * FssEss * Favg / max(1.0 - Ems * Favg, 1e-4);

    // WHAT THE DIFFUSE LOBE LOSES IS THE DIELECTRIC REFLECTANCE, NOT THE BLENDED ONE: the diffuse
    // lobe belongs to the dielectric substrate (a metal has none, per kdAlbedo's own (1-metallic)
    // factor), and subtracting the METAL-blended FssEss too charges it for reflectance it never had
    // -- the furnace caught metallic 0.5 dropping from 1.005 to 0.757 that way, while both pure ends
    // stayed at 1.000. Recomputing against the dielectric F0 restores it to 0.98.
    //
    // INTERMEDIATE METALLIC STILL DOES NOT CLOSE, and no arrangement of these terms makes it: "half
    // metal" is a blend of PARAMETERS, not a material, and the multiple-scatter series is strongly
    // non-linear in F0 (F0 = 0.52 returns much less than the mean of 0.04 and 1.0). The physical claim
    // here is at the two ENDS, which measure 1.000 across every roughness; the middle is a documented
    // approximation.
    float3 F0d     = s.reflectance.xxx;
    float3 FssEssD = F0d * dfg.x + dfg.y;
    float3 FavgD   = F0d + (1.0 - F0d) / 21.0;
    float3 FmsEmsD = Ems * FssEssD * FavgD / max(1.0 - Ems * FavgD, 1e-4);
    float3 kD      = s.kdAlbedo * saturate(1.0 - FssEssD - FmsEmsD);

    // Occlusion is applied per lobe now: AO answers a hemisphere question and belongs with the
    // diffuse terms, while the specular lobe gets averSpecularOcclusion. Sending raw AO into a
    // mirror cost 21% of its energy in the furnace with GI on -- measured against the same grid.
    float  diffOcc = ind.occlusion * s.occlusion;
    // A traced reflection saw its occluders; AO on top cost the night wet road 4% of the frame.
    float  specOcc = ind.specularTraced > 0.5 ? 1.0 : averSpecularOcclusion(s.ndv, ind.occlusion, s.rough);

    // FssEss multiplies RADIANCE (the reflection); everything else multiplies IRRADIANCE (the
    // sky and the bounce), which is why they are not folded into one factor.
    specEnv     = FssEss * ind.specular * specOcc;
    diffAmbient = (FmsEms + kD) * ind.ambient * ind.ambientScale * diffOcc;
    diffBounce  = kD * ind.diffuse;

#ifdef AVER_LAYERED_BSDF
    // THE COAT GOES ON LAST, taking its share out of all three terms, not only specular: light
    // stopped at the coat's surface never reaches the base's mirror, diffuse or bounce, so
    // attenuating only specular would let a coated surface keep more diffuse than an uncoated one --
    // energy from nowhere. The multi-scatter block above still applies to the base exactly as before;
    // the coat layers on after it. A coat's own multi-scatter compensation is deliberately NOT added:
    // the masking loss it corrects scales with roughness, and a coat's typical roughness (car paint,
    // 0.05-0.3) makes it small -- a stated approximation, not an oversight.
    if (s.coatWeight > 0.0) {
        float3 coatEnv; float baseAtten;
        averCoatTerms(s, ind, s.coatWeight, s.coatRough, s.coatF0, coatEnv, baseAtten);
        specEnv     *= baseAtten;
        diffAmbient *= baseAtten;
        diffBounce  *= baseAtten;
        specEnv     += coatEnv;
    }
#endif
}

// Adds ambient, bounce, environment specular and self-emission, in that order.
float3 averShadeIndirect(float3 radiance, AverSurface s, AverIndirect ind) {
    switch (s.model) {
    case AVER_MODEL_UNLIT:
        return radiance + s.emissive;
    default: {
        float3 specEnv, diffAmbient, diffBounce;
        averIndirectTerms(s, ind, specEnv, diffAmbient, diffBounce);
        // Four separate += in the order averIndirectTerms promises, not one grouped sum: IEEE 754
        // addition is not associative, and re-grouping would make this function's own claim to being
        // untouched false.
        radiance += specEnv;
        radiance += diffAmbient;
        radiance += diffBounce;
        radiance += s.emissive;
        // After the four, and skipped at weight 0, so their sum stays bit-identical for every other material.
        if (s.sssWeight > 0.0) radiance += averSubsurfaceAmbient(s, ind);
        return radiance;
    }
    }
}

// Shades exactly as averShadeDirect + averShadeIndirect together do, but keeps the two lobes apart
// instead of summing them -- see the shading contract at the top of this file for why a translucent
// surface needs that: PREMULTIPLIED-ALPHA COMPOSITING WANTS SPECULAR AT FULL STRENGTH AND DIFFUSE
// WEIGHTED BY COVERAGE, and a single accumulated radiance has already forgotten which photons left
// the microsurface at the reflection angle vs. were diffusely re-emitted by the time it is computed.
//
// Calls the same averDirectTerms/averIndirectTerms as averShadeDirect/averShadeIndirect -- one copy
// of the BRDF, identical lobes from identical expressions. What is NOT identical is the ORDER terms
// are finally added: the direct lobe keeps the exact original grouping, but the indirect side sums
// its four terms into two buckets here instead of averShadeIndirect's fixed four-term sequence, and
// IEEE 754 addition is not associative, so this can move the last bit of the result. That is a
// genuine, understood approximation, stated here rather than found by diffing a furnace capture --
// which is why the bit-identical INVARIANT this file guarantees is stated about
// averShadeDirect/averShadeIndirect themselves, not about this function matching them past the last
// ULP; only the first claim is actually provable from the source.
void averShadeSplit(AverSurface s, AverLight l, AverIndirect ind, out float3 diffuse, out float3 specular) {
    switch (s.model) {
    case AVER_MODEL_UNLIT:
        // averShadeDirect contributes nothing on the unlit path and averShadeIndirect adds only
        // s.emissive, so an unlit surface has no specular lobe; its one "diffuse" contribution, in
        // averBlendedOutput's coverage-weighted sense, is its authored emissive colour.
        diffuse  = s.emissive;
        specular = 0.0;
        return;
    default: {
        float3 dDiffuse, dSpecular, dSubsurface;
        float  ndl;
        averDirectTerms(s, l, dDiffuse, dSpecular, dSubsurface, ndl);
        float3 lightTerm = l.radiance * ndl * l.visibility;
        diffuse  = dDiffuse * lightTerm;
        specular = dSpecular * lightTerm;
        // Subsurface is DIFFUSE for this split and carries no ndl (see averDirectTerms), so it joins
        // the diffuse bucket and composites with the coverage that half gets, not specular's full
        // strength.
        diffuse += dSubsurface * l.radiance * l.visibility;

        float3 specEnv, diffAmbient, diffBounce;
        averIndirectTerms(s, ind, specEnv, diffAmbient, diffBounce);
        specular += specEnv;
        diffuse  += diffAmbient;
        diffuse  += diffBounce;
        diffuse  += s.emissive;
        // Diffuse, like the direct subsurface term above.
        if (s.sssWeight > 0.0) diffuse += averSubsurfaceAmbient(s, ind);
        return;
    }
    }
}

// Packs a blended surface for a PREMULTIPLIED-ALPHA blend state (rhi::BlendMode::PremultipliedAlpha,
// SrcBlend=ONE/DestBlend=INV_SRC_ALPHA) -- the whole fix the shading contract exists for: straight
// "over" blending would multiply `specular` by s.alpha too, so a pane at a transparent alpha of 0.2
// would show its own reflection at 20% strength instead of full strength regardless of transmission.
//
//     rgb = specular + diffuse * s.alpha ;  a = s.alpha
//
// diffuse already carries emissive and the ambient/bounce terms (see averShadeSplit), so folding it
// through s.alpha is what lets a thin, mostly-transmissive pane emit and tint proportionally less
// while its reflection stays full strength -- the asymmetry straight-alpha glass was missing, which
// made it read as tinted plastic instead of glass.
float4 averBlendedOutput(AverSurface s, float3 diffuse, float3 specular) {
    return float4(specular + diffuse * s.alpha, s.alpha);
}

// The same composite, for a surface that also has a VOLUME behind it. `T` is the per-channel
// transmittance across the path the light actually travelled (averVolumeTransmittance of a measured
// thickness, not an authored guess).
//
// DERIVATION: blend state is PremultipliedAlpha (out = src + dst*(1-a)), and background light must
// survive with weight (1-s.alpha)*T instead of (1-s.alpha), giving a = 1 - (1-s.alpha)*Tavg -- the
// medium only removes light, never adds any. REDUCES EXACTLY to averBlendedOutput with no volume: at
// T=1 the term is zero and a collapses to s.alpha (bit-identical, keeping every existing blended
// draw off the gate baselines); at T=0 it goes fully opaque, a column deep enough to swallow
// everything behind it.
//
// ONE LIMITATION, same as the old fluid shader's: a single SCALAR hardware alpha can only attenuate
// the background by the AVERAGE transmittance, so attenuationColor controls HOW FAST a volume goes
// opaque with depth -- the dominant cue, and one the old fluid shader could not have at all -- but
// its HUE does not yet tint what is behind it. A volume's own colour still comes from its authored
// base colour (as gFluidBody did). Per-channel removal needs the background: either a scene-colour
// SRV (IRenderContext::copyTexture, proven, one caller today), or a per-channel destination blend
// factor -- INV_SRC_COLOR multiplies dst by (1 - src.rgb) per channel, nearly free but changes the
// blend state for every blended draw. Both are real follow-ups, neither a change to make in passing.
float4 averBlendedOutputVolume(AverSurface s, float3 diffuse, float3 specular, float3 T) {
    const float Tavg  = dot(T, float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0));
    const float alpha = saturate(1.0 - (1.0 - s.alpha) * Tavg);
    // Same composite as averBlendedOutput, with the volume-corrected coverage in place of the
    // surface's own -- deliberately the whole change, nothing more.
    //
    // TRIED AND WRONG, recorded so it is not re-attempted: an in-scattering term
    // `diffuse * (1 - s.alpha) * (1 - T)`. Plausible (the medium fills in as background is absorbed)
    // but (1-T) is LARGEST in the channel the medium absorbs MOST, so a green-transmitting glass
    // gains red and blue -- the complement of the right colour. Measured on an 8 cm M_Glass pane
    // authored (0.15, 0.85, 0.35) at 4 cm: darkened uniformly by (29, 28, 27), no green anywhere, the
    // tint cancelling itself against the term meant to produce it. glTF's volume is pure ABSORPTION
    // (no scattering albedo), so there is nothing for a correct in-scattering term to be made of.
    return float4(specular + diffuse * alpha, alpha);
}
