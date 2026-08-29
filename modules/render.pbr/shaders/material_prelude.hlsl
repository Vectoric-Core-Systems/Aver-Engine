
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

    // Mirrors MaterialConstants::ior/transmission -- see that struct's comment for why the two
    // fields are not independent, and averBuildSurface below for the one thing gTransmission
    // currently feeds (the AVER_MAT_ALPHA_BLEND coverage term).
    //
    // gIor IS READ BY NOTHING, and this comment used to claim otherwise -- it said gIor "is no
    // longer unread: it sets the critical angle in averTotalInternalReflection". That was true
    // while the total-internal-reflection override existed; the override was removed because it
    // could not fire legitimately from a rasterised back face (see averBuildSurface's alpha branch
    // for the Snell argument and the measured cost), and this line went back to being false with it.
    // The honest state: ior is authored, packed, transported to the GPU and copied into
    // AverAuthored, and no shading term consumes it. The physically correct consumer is F0 --
    // F0 = ((1-n)/(1+n))^2 -- which today is authored SEPARATELY as `reflectance`, so a material
    // can state an ior and a reflectance that contradict each other (M_Glass.ocmat's own comment
    // warns about exactly that and keeps them in sync by hand). Deriving one from the other would
    // change F0 for every material that does not already agree, so it is a decision, not a tidy-up.
    //
    // Both are copied into AverAuthored rather than read directly at their use sites, so a material
    // graph can drive either per pixel; still nothing here does refraction.
    float  gIor;
    float  gTransmission;
    // EXPLICIT PADDING, MIRRORING MaterialConstants::_pad0/_pad1. Not load-bearing for THIS cbuffer
    // -- HLSL rounds a constant buffer's footprint up to its last 16-byte register regardless of
    // what is declared in it, so gIor/gTransmission alone would already reserve through byte 96 on
    // the GPU. It is declared anyway so this block keeps mirroring MaterialConstants field for
    // field, byte offset for byte offset, which is the whole discipline this comment block is
    // asking of whoever edits either side next.
    // These two ARE the bytes _matPad used to declare -- MaterialConstants spent its _pad0/_pad1 on
    // them, and this block mirrors that struct field for field, byte offset for byte offset. There is
    // no padding left on either side.
    float  gSubsurfaceWeight;
    float  gSubsurfaceRadius;
    // The coat row. MIRRORS MaterialConstants::coatWeight/coatRoughness/coatF0/_coatPad, in that
    // order -- the block is 112 bytes and this is the row that took it there.
    float  gCoatWeight;
    float  gCoatRoughness;
    float  gCoatF0;
    float  _gCoatPad;
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
    // TRUE WHERE THE GEOMETRIC NORMAL POINTED AWAY AND WAS FLIPPED, i.e. this pixel is the BACK of
    // the surface -- the eye is inside the volume the front face encloses.
    //
    // averVertexOf has always computed this, used it to flip the normal, and then thrown it away.
    // Keeping it is the entire plumbing cost of total internal reflection: TIR only happens on the
    // way OUT of the denser medium, so a shader with no way to know which side it is on cannot
    // express it at all, however good its Fresnel term is.
    bool backFace;
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
    // The DIELECTRIC F0 this surface was built from. Carried here rather than read back off
    // gMatReflectance, because a RAY HIT HAS NO MATERIAL CONSTANT BUFFER BOUND -- PSRayDriven says
    // exactly that where it defaults this to 0.04 by hand. averShadeIndirect reaching for the
    // global instead measured a white dielectric at 1.030 on the ray path against 1.000 on the
    // raster one: with no material bound the global is not 0.04, the diffuse lobe stopped being
    // charged for the specular reflectance it takes off the top, and the surface read too bright.
    float  reflectance;
    // Subsurface, both 0 where the material did not ask for it -- which makes averDirectTerms'
    // subsurface term identically zero rather than merely small. PSRayDriven hand-builds this struct
    // and must set them too; HLSL does not zero a struct for you.
    float  sssWeight, sssRadius;
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

// ================= what a material GRAPH can reach =================
// These exist for generated code and are used by nothing else in this file. They are here rather
// than emitted inline into every graph for the reason averSampleMaps is here: each one wraps a
// conditional or a constant table that a generator would otherwise have to restate, correctly, once
// per node instance -- and the first one it got wrong would be a material that shades subtly wrong
// with nothing to point at.

// One material map, sampled at an arbitrary UV rather than the surface's own.
//
// THE SLOT IS A SWITCH, NOT AN ARRAY INDEX, because the eight maps are eight separately declared
// Texture2Ds at eight registers (see the AVER_MATERIAL_SRV block at the top of this file) and HLSL
// has no way to index that without a resource array the root signature does not describe. The switch
// is over a value that is CONSTANT for a given generated node, so it costs nothing at runtime: the
// compiler folds it away entirely.
//
// Returns white where there is no material table at all, matching averSampleMaps' own #else branch:
// a graph that samples a map in a build with no material SRVs gets the identity, not a compile
// error, exactly as the stock path does.
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
    // AUTHORED, NOT READ STRAIGHT OFF THE CBUFFER, and that is the whole point of them living here.
    // Everything in this struct is a value a material GRAPH may override per pixel; a field the
    // surface build reads from the constant buffer directly is a field no graph can ever drive.
    // Subsurface is exactly where per-pixel authoring earns its keep -- a thickness mask driving the
    // radius is the difference between a uniformly waxy object and one whose thin parts glow.
    float  subsurfaceWeight;
    float  subsurfaceRadius;
    // The dielectric pair, here for the same reason subsurface is: a value read straight off the
    // constant buffer is a value no material GRAPH can ever drive. Driving transmission from a mask
    // is how one mesh becomes a window with a frosted band, or a bottle with a label.
    float  ior;
    float  transmission;
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
    // GATED HERE, ONCE. Below this point nothing re-tests the flag: a graph that drives these pins
    // writes them after the stock path has run, and it would be wrong for the flag to then veto a
    // value the author explicitly asked for.
    a.subsurfaceWeight = (gMaterialFlags & AVER_MAT_SUBSURFACE) ? saturate(gSubsurfaceWeight) : 0.0;
    a.subsurfaceRadius = (gMaterialFlags & AVER_MAT_SUBSURFACE) ? saturate(gSubsurfaceRadius) : 0.0;
    a.ior              = gIor;
    a.transmission     = gTransmission;
    return a;
}


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
    // TRANSMISSION REMOVES LIGHT FROM THE DIFFUSE LOBE, and until this line it did not.
    //
    // gTransmission is how much light passes THROUGH the substrate instead of scattering back out of
    // it. Light that went through cannot also come back as diffuse -- the two are the same photons,
    // and a material that both transmits 92% and diffusely reflects its full base colour is emitting
    // energy it never received. Every other use of transmission in this file was on s.alpha
    // (coverage) alone, so the diffuse lobe kept its full albedo no matter how see-through the author
    // said the surface was.
    //
    // MEASURED, on PTTest's M_Glass (baseColorFactor 0.86 0.93 0.88 0.12, transmission 0.92) over the
    // dark pool. Probing one pixel of pane against the water beside it, and bisecting this function's
    // output to attribute the pale wash:
    //     full output ......... 103,124,130
    //     specular removed .....  99,120,125   -> the whole specular term is FOUR codes
    //     coverage only ........  27,56,84     -> vs 0.88 * water(32,69,94) = 28,61,83. Correct.
    // so `diffuse * s.alpha` alone contributed (72,64,41) -- a warm wash five times the size of the
    // reflection, on a pane authored to be 92% transmissive. That is the "milky, cartoonish glass"
    // this whole exercise started from, and it is not the reflection, the Fresnel, the alpha or the
    // blend state: all four were verified correct first. It is a diffuse lobe nobody dimmed.
    //
    // (1 - gTransmission), matching glTF KHR_materials_transmission, which splits the same budget the
    // same way. NOT gated on AVER_MAT_ALPHA_BLEND: an opaque material may author transmission too --
    // it is a substrate property, not a blend mode -- and this must stay one rule rather than a glass
    // special case. It is a no-op for every material authored before the field existed, because
    // MaterialDesc::transmission defaults to 0.
    s.kdAlbedo = (1.0 - s.metallic) * s.albedo * (1.0 - saturate(a.transmission));
    // GATED ON THE FLAG, not on the float, so the whole subsurface branch folds away for every
    // material that does not want it.
    s.backFace  = v.backFace;
    // FROM THE AUTHORED STRUCT, not from the cbuffer. Reading gSubsurfaceWeight here instead would
    // work identically for the stock material and silently ignore every material graph that drove
    // the pin -- the defect this whole struct exists to prevent.
    s.sssWeight = saturate(a.subsurfaceWeight);
    s.sssRadius = saturate(a.subsurfaceRadius);

    // FRESNEL-AWARE ALPHA FOR BLENDED SURFACES. Plain "over" compositing --
    // dst = src.rgb*alpha + dst.rgb*(1-alpha) -- treats alpha as one UNIFORM attenuation and so
    // applies it to the specular reflection exactly as hard as to everything else. Real dielectrics
    // do not attenuate uniformly: the Fresnel term climbs toward 1 at grazing incidence, so a glass
    // pane goes from nearly invisible face-on to nearly a mirror edge-on, hiding whatever is behind
    // it. This is not a cosmetic nicety -- without it, a pane authored at a usefully transparent
    // alpha of ~0.2 would draw ITS OWN REFLECTION at 20% strength, which is too dim to read as a
    // reflection at all and instead reads as a smudge: the glass looks like dirty plastic, not glass.
    //
    // Raising alpha toward 1 by the same view-angle Fresnel term this file already computes fixes it
    // with no second BRDF: at grazing incidence the term approaches 1, alpha follows it to 1, and the
    // blended draw becomes fully opaque there -- correctly, because at grazing incidence virtually
    // all the light reaching the eye from that pixel IS the reflection and there is nothing left to
    // blend the background into. Face-on, the term sits near s.reflectance (glass is commonly ~0.04)
    // and alpha stays close to its authored value.
    //
    // GATED ON AVER_MAT_ALPHA_BLEND so this is provably a no-op for every material that does not ask
    // for it: unless the bit is set, execution never enters the branch below, so s.alpha for an
    // opaque or masked material is exactly the two lines above it always was (gBaseColor.a *
    // a.opacity, optionally clip()'d) and nothing downstream changes. That is also why the render is
    // bit-identical for every material authored today and not merely intended to be: Material.cpp
    // reported Feature::AlphaBlend as Status::NotImplemented until this same change, so nothing ever
    // shipped a material that both set this bit AND expected a renderer to act on it -- the bit has
    // only ever been inert storage in content authored so far.
    if (gMaterialFlags & AVER_MAT_ALPHA_BLEND) {
        // TRANSMISSION SETS THE FLOOR FIRST; FRESNEL LIFTS IT SECOND -- and not the other way round.
        // gTransmission > 0 is an author's statement that this surface is OPTICALLY see-through: a
        // fact about the substrate that holds regardless of where the camera is standing. That has
        // to be applied before anything view-dependent touches alpha, so it reads as a FLOOR the
        // view term then lifts away from, not as one more multiplier competing with it.
        //
        // Doing it in the other order -- Fresnel first, transmission second -- would let a highly
        // transmissive pane's own grazing-angle brightening get pulled back down by the transmission
        // term afterwards, which is backwards: the reason real glass reads nearly opaque at a shallow
        // angle is that the Fresnel reflection is real light actually reaching the eye, and the
        // substrate's transmission has no say over light that never entered it in the first place.
        // Transmission describes the SUBSTRATE; Fresnel describes the VIEW ANGLE; the view angle has
        // to be the last word because it is the last thing standing between the surface and the eye.
        //
        // lerp toward (1 - gTransmission) rather than multiplying s.alpha by it: at gTransmission ==
        // 1 the author has declared the surface fully see-through, and the target of 0 coverage wins
        // outright regardless of whatever alpha was separately authored -- a transmission of 1 on an
        // alpha-0.9 pane should not leave it reading 90% solid. At gTransmission == 0 the lerp weight
        // is zero and s.alpha passes through completely unchanged, which is what keeps this whole
        // branch a no-op for every material authored before this field existed (their transmission
        // defaults to 0 -- see MaterialDesc::transmission).
        float baseAlpha = lerp(s.alpha, 1.0 - a.transmission, saturate(a.transmission));

        // s.F above is evaluated at the HALF VECTOR for the current light's direct specular term, so
        // it swings with every light in the scene and with l.direction, which is a poor knob for
        // something that must describe how mirror-like the surface reads to the CAMERA regardless of
        // lighting. The VIEW-angle Fresnel -- fresnelSchlick at s.ndv, i.e. dot(N, V) -- is what
        // answers that: it depends on nothing but the surface and the eye, so it is stable across
        // every light in the draw and across an unlit scene too.
        float3 viewFresnel = fresnelSchlick(s.ndv, s.F0, s.f90);
        // Reduced to a scalar by luminance, not by picking a channel: F0 is achromatic for the
        // dielectrics this exists for (F0 = gMatReflectance.xxx when metallic is 0, so every channel
        // already agrees), and luminance is the principled reduction for the rarer case of a
        // translucent, partially metallic surface where F0 is tinted and the channels disagree.
        float fresnelLum = dot(viewFresnel, float3(0.2126, 0.7152, 0.0722));
        // THERE IS NO TOTAL-INTERNAL-REFLECTION OVERRIDE HERE, AND THERE CANNOT BE ONE, which is a
        // correction: there WAS one, gated on s.backFace, and it turned every pane of glass in the
        // engine into a dark slab at 41 degrees off normal.
        //
        // TIR needs the ray to be INSIDE the denser medium already. The test used backFace as the
        // proxy for that, and backFace does not mean it. On a two-sided pane -- and glass is
        // routinely CULL none, because you walk round it -- backFace is true for the far surface of
        // the pane as seen from OUTSIDE, which is an ordinary air-to-glass view with the normal
        // flipped to face the eye. It is also true for a genuine inside-the-medium view. The two
        // are indistinguishable from a pixel shader, and only the second one can total-internally-
        // reflect.
        //
        // SNELL FORBIDS THE FIRST OUTRIGHT, so this is not a tuning question. Light reaching that
        // far surface got in through the front one, refracting TOWARD the normal on the way:
        // sin(t_inside) = sin(t_outside)/n, so t_inside maxes out at asin(1/n) -- 41.1 degrees at
        // n = 1.52 -- which IS the critical angle. The internal angle can equal it and never exceed
        // it. A parallel-sided pane viewed from outside cannot produce TIR at any view angle.
        //
        // WHAT IT COST, measured on PTTest's glass rail at grazing incidence: the pane read
        // 127,140,141 against a 203,215,215 background -- a dark sheet where a mirror belongs --
        // and 174,187,188 with the override gone. The override fired from ndv < 0.753, so it
        // covered most of the viewing hemisphere, and because it slammed alpha to 1 it replaced the
        // background with whatever the glass's own reflection happened to be. Face-on was
        // unaffected (195,206,206 either way), which is why this read as "dark patches" appearing
        // at an angle rather than as glass being wrong everywhere.
        //
        // THE REAL TEST LOOKS LIKE modules/fluids/src/WaterShaders.hpp'S, and that one is correct
        // and untouched: `bool underwater = gCamPos.z < gWaterState.x` asks whether the EYE is
        // inside the medium, using the one piece of geometry that answers it, and only then applies
        // the same critical-angle maths. A material has no equivalent -- there is no per-pixel fact
        // that says which side of a closed surface the camera is on -- so glass gets ordinary view
        // Fresnel here, and that is the honest answer rather than a plausible-looking wrong one.
        s.alpha = lerp(baseAlpha, 1.0, saturate(fresnelLum));
    }

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

// ================= shared BRDF terms: ONE copy of the maths, three consumers =================
// averShadeDirect and averShadeIndirect below are the ORIGINAL accumulating entry points, unchanged
// in what they compute or in what order they add it -- every renderer that shades an opaque surface
// through them gets exactly the radiance it always did. averShadeSplit, further down, calls the same
// two helpers and keeps their outputs in separate registers instead of summing them, which is what a
// PREMULTIPLIED-ALPHA blended draw needs (see the shading contract at the top of this file and
// averBlendedOutput's own comment). Splitting happened HERE, at the helper boundary, rather than by
// writing a second copy of either lobe's arithmetic, so a future change to the BRDF or to the
// multiscatter compensation is made once and every consumer sees it.

// Cook-Torrance GGX for one light, returned as its two UNWEIGHTED lobes (before the light's own
// radiance/NdotL/visibility are multiplied in) plus the NdotL both callers need. Splitting the
// return here, rather than after the light term is applied, is what lets averShadeDirect reconstruct
// `(diffuseLobe + specularLobe) * l.radiance * ndl * l.visibility` -- THE EXACT ORIGINAL EXPRESSION,
// same grouping, same order -- so its output is provably unaffected by this function existing at all.
// SUBSURFACE IS A FOURTH OUTPUT, NOT PART OF diffuseLobe, and that is the whole reason this
// signature changed. Both callers multiply diffuseLobe by ndl, and ndl is ZERO exactly where
// subsurface light is the only thing there is -- past the terminator. Folding the term into the
// diffuse lobe would therefore multiply the effect by zero precisely where it is the effect.
// subsurfaceLobe carries its own angular dependence and must NOT be scaled by ndl.
void averDirectTerms(AverSurface s, AverLight l, out float3 diffuseLobe, out float3 specularLobe,
                     out float3 subsurfaceLobe, out float ndl) {
    float a = s.rough * s.rough;
    ndl = saturate(dot(s.N, l.direction));
    float D = distGGX(saturate(dot(s.N, s.H)), a);
    float V = visSmithCorrelated(s.ndv, ndl, a);
    float3 spec = D * V * s.F;
    // ENERGY LOST TO MASKING, PUT BACK. A single-scatter GGX lobe drops every ray the
    // microsurface would have bounced a second time, and the loss grows with roughness: a white
    // metal at roughness 1 returned 45% of the light it received, measured in this engine's own
    // white furnace before this line existed. The compensation is the standard single-term
    // approximation and it reuses the split-sum term the indirect path already computes -- see
    // averIndirectTerms, which corrects the same loss for the environment.
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
    diffuseLobe  = s.kdAlbedo / PI;
    specularLobe = spec;

    // ---- subsurface: wrapped diffuse + view-dependent back-scatter ----
    //
    // TWO TERMS, AND ONLY THE EXTRA. The caller already pays kdAlbedo/PI * ndl, so the wrap term
    // contributes the DIFFERENCE between a wrapped N.L and the plain one; at weight 0 the wrapped
    // form reduces to (ndl + 0) / 1 == ndl, the difference is exactly 0, and this is bit-identical
    // to the code before it existed. That identity is why the term is written as a difference
    // rather than as a replacement lobe.
    //
    // The (1+w)^2 denominator is the energy normalisation, not a fudge: widening the lobe without
    // it hands the surface more light than fell on it, and skin authored at weight 1 would read as
    // emissive.
    //
    // The second term is light that entered the far side and travelled toward the eye, so it is
    // keyed on dot(V, -L) rather than on the normal -- that is what makes an ear or a leaf light up
    // when the sun is BEHIND it, which is the whole visual point. Radius sharpens or widens it:
    // a thin surface transmits a tight forward beam, a thick one a broad wash.
    float sssW = s.sssWeight;
    float ndlWrap = saturate((dot(s.N, l.direction) + sssW) / ((1.0 + sssW) * (1.0 + sssW)));
    float wrapExtra = max(ndlWrap - ndl, 0.0);
    float backScatter = pow(saturate(dot(s.V, -l.direction)), lerp(12.0, 2.0, s.sssRadius)) * s.sssRadius;
    subsurfaceLobe = s.kdAlbedo / PI * (wrapExtra + backScatter * sssW);
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

// The four terms averShadeIndirect sums, returned UNSUMMED rather than pre-combined into a
// diffuse/specular pair: float addition is not associative, and averShadeIndirect's own bit-for-bit
// output is the one thing this refactor is not allowed to move, so it has to add these four in
// EXACTLY the sequence it always did rather than in two pre-grouped batches. averShadeSplit, which
// has no prior output to match, sums the same four terms into its two buckets instead -- see its own
// comment for why that makes it the one place in this pair that is a documented approximation rather
// than a provable identity.
#ifdef AVER_LAYERED_BSDF
// ================= THE COAT: a second specular layer over everything the base returns =================
//
// A clear film on top of the base material -- car paint, varnish, a wet stone. The base keeps its own
// metallic/roughness response; the coat adds a smoother GGX lobe over it and ATTENUATES what shows
// through by its own Fresnel, which is the only thing that makes this energy-conserving rather than
// energy-adding.
//
// COMPILED ONLY UNDER AVER_LAYERED_BSDF. A project whose Settings::layeredBsdf is Off gets a shader
// with none of this in it -- not a branch that evaluates to zero, no register pressure, nothing. That
// is what makes "costs nothing when off" a fact about the compiled code rather than a hope.
//
// ONE FACTOR OF (1 - Fc), NOT TWO, AND IT IS THE WHOLE CORRECTION. The obvious composition attenuates
// by (1 - Fc(ndv)) twice, once for light entering the coat and once for it leaving. That is right for
// a transmitted path through a slab and WRONG here: averEnvBRDF already integrates the full
// hemisphere-to-eye response, so a second factor charges the base twice for the same interface. It was
// written that way first and tests/render.pbr/src/CoatEnergyTest.cpp caught it -- a furnace plate
// visibly darker than its background at every roughness -- before any of this reached a GPU.
//
// The arithmetic below is mirrored in that test, which also asserts this function exists. If you
// change the composition here, change it there; the test reads this file and will say so if you do not.
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
    coatEnv = cEnv * coatWeight * ind.specular * averSpecularOcclusion(s.ndv, ind.occlusion, coatRough);

    // What the base is allowed to return. fresnelSchlick at the VIEW angle, not the half vector: this
    // describes how mirror-like the coat is to the CAMERA, which is a property of the surface and the
    // eye and nothing else -- the same argument averBuildSurface's alpha branch makes for its own use
    // of a view-angle Fresnel.
    baseAttenuation = 1.0 - fresnelSchlick(s.ndv, coatF0, 1.0) * coatWeight;
}
#endif   // AVER_LAYERED_BSDF

void averIndirectTerms(AverSurface s, AverIndirect ind,
                       out float3 specEnv, out float3 diffAmbient, out float3 diffBounce) {
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
    specEnv     = FssEss * ind.specular * specOcc;
    diffAmbient = (FmsEms + kD) * ind.ambient * ind.ambientScale * diffOcc;
    diffBounce  = kD * ind.diffuse;

#ifdef AVER_LAYERED_BSDF
    // THE COAT GOES ON LAST, over everything the base just computed, and takes its share out of all
    // three terms rather than only the specular one. Light stopped at the coat's surface never reaches
    // the base at all -- not its mirror, not its diffuse, not its bounce -- so attenuating only the
    // specular would let a coated surface keep more diffuse than an uncoated one, which is energy from
    // nowhere.
    //
    // The multi-scatter block above is untouched and still applies to the base exactly as before; the
    // coat is layered on after it, not folded into it. A coat's own multi-scatter compensation is
    // deliberately NOT added: the masking loss it corrects scales with roughness, and a coat's typical
    // roughness (car paint, 0.05-0.3) makes it small. That is a stated approximation, in the same
    // style as the intermediate-metallic case this file already documents -- not an oversight.
    if (gMaterialFlags & AVER_MAT_COAT) {
        float3 coatEnv; float baseAtten;
        averCoatTerms(s, ind, saturate(gCoatWeight), saturate(gCoatRoughness), saturate(gCoatF0),
                      coatEnv, baseAtten);
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
        // Four separate += in the SAME order averIndirectTerms' comment promises, not
        // `radiance + (specEnv + diffAmbient + diffBounce + s.emissive)`: IEEE 754 addition is not
        // associative, and re-grouping these four terms is exactly the kind of change that would make
        // this function's own claim to being untouched false.
        radiance += specEnv;
        radiance += diffAmbient;
        radiance += diffBounce;
        radiance += s.emissive;
        return radiance;
    }
    }
}

// Shades exactly as averShadeDirect + averShadeIndirect together do, but keeps the two lobes apart
// instead of summing them -- see the shading contract at the top of this file for why a translucent
// surface needs that: PREMULTIPLIED-ALPHA COMPOSITING WANTS SPECULAR AT FULL STRENGTH AND DIFFUSE
// WEIGHTED BY COVERAGE, and a single accumulated radiance has already forgotten which photons left the
// microsurface at the reflection angle and which were diffusely re-emitted by the time it is computed.
//
// BOTH averDirectTerms AND averIndirectTerms ARE THE SAME CODE averShadeDirect/averShadeIndirect call
// -- there is exactly one copy of this BRDF, and an opaque draw run through this function computes
// the identical lobes from the identical expressions those two do. What is NOT identical, and cannot
// be while keeping that one copy, is the ORDER the terms are finally added in: the direct lobe's
// output is the exact original grouping (see averShadeDirect), but the indirect side sums its four
// terms into two buckets here instead of averShadeIndirect's fixed four-term sequence, and IEEE 754
// addition is not associative -- summing the same values in a different grouping can move the last
// bit of the result. That is a genuine, understood approximation, stated here rather than left for
// someone to find by diffing a furnace capture against this path. It is also why the INVARIANT this
// file guarantees is stated about averShadeDirect/averShadeIndirect themselves (untouched, calling
// these same helpers, same grouping, therefore bit-identical to before this change) and not about
// this function matching them past the last ULP -- the two claims are different, and only the first
// one is actually provable from the source.
void averShadeSplit(AverSurface s, AverLight l, AverIndirect ind, out float3 diffuse, out float3 specular) {
    switch (s.model) {
    case AVER_MODEL_UNLIT:
        // averShadeDirect contributes nothing on the unlit path (its own switch returns `radiance`
        // untouched) and averShadeIndirect adds only `s.emissive` -- so an unlit surface has no
        // specular lobe at all, and its one "diffuse" contribution, in the coverage-weighted sense
        // averBlendedOutput gives that word, is its authored emissive colour.
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
        // Subsurface is DIFFUSE for the purposes of this split, and carries no ndl -- see
        // averDirectTerms. It joins the diffuse bucket so a blended surface composites it with the
        // coverage the diffuse half gets rather than with the specular half's full strength.
        diffuse += dSubsurface * l.radiance * l.visibility;

        float3 specEnv, diffAmbient, diffBounce;
        averIndirectTerms(s, ind, specEnv, diffAmbient, diffBounce);
        specular += specEnv;
        diffuse  += diffAmbient;
        diffuse  += diffBounce;
        diffuse  += s.emissive;
        return;
    }
    }
}

// Packs a blended surface for a PREMULTIPLIED-ALPHA blend state (rhi::BlendMode::PremultipliedAlpha
// -- SrcBlend=ONE, DestBlend=INV_SRC_ALPHA), which is the whole fix the shading contract exists for:
// straight "over" blending would multiply `specular` by s.alpha along with everything else, so a pane
// authored at a usefully transparent alpha of 0.2 would show its own reflection at 20% strength
// instead of the full strength a real dielectric reflects at regardless of how much it transmits.
//
//     rgb = specular + diffuse * s.alpha ;  a = s.alpha
//
// diffuse already carries emissive and the ambient/bounce terms (see averShadeSplit), so folding it
// through s.alpha here is what makes a thin, mostly-transmissive pane emit and diffusely tint
// proportionally less while its reflection stays full strength -- exactly the asymmetry that made
// straight-alpha glass read as tinted plastic instead of glass in the first place.
float4 averBlendedOutput(AverSurface s, float3 diffuse, float3 specular) {
    return float4(specular + diffuse * s.alpha, s.alpha);
}
