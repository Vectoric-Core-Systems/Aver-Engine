// The HLSL every GPU consumer shares: the constant-buffer layouts the backend uploads, the vertex
// structures it feeds, the colour-space and BRDF helpers, and the surface shading itself.
//
// It lives here rather than in a backend or a feature module because it is a cross-module ABI with
// no compiler behind it: `cbuffer PerFrame` must match PerFrameCB field for field, and the mesh
// geometry registers must match what dispatchMeshFor() binds. Duplicating any of it would make
// reordering one copy corrupt the other with no diagnostic anywhere.
#include "aver/rhi/RHIResources.hpp"

namespace aver::rhi {

// The HLSL below hardcodes these; the C++ side reads the constants. Neither can move alone.
static_assert(kMeshShaderTrisPerGroup == 64, "AVER_MS_TRIS in the prelude is written out as 64");
static_assert(kMeshGeometryConstantRegister == 5, "MeshCB in the prelude is written out as b5");

const char* sharedShaderPrelude() {
    return R"(
cbuffer PerFrame : register(b0) {
    float4x4 gViewProj;
    float4x4 gInvViewProj;
    float4   gCamPos;      // xyz
    float4   gLightDir;    // xyz = direction TO light
    float4   gLightColor;  // rgb
    float4   gAmbient;     // rgb
    float4   gSkyZenith;   // rgb
    float4   gSkyHorizon;  // rgb
    float4   gFogColor;    // rgb, a = density
};
cbuffer PerObject : register(b1) {
    float4x4 gWorld;
    float4   gBaseColor;
    float4   gMaterial;   // x=metallic, y=roughness, z=unlit(0/1)
};

static const float PI = 3.14159265;
float3 acesTonemap(float3 x){ return saturate((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14)); }
float3 toGamma(float3 c){ return pow(max(c,0.0), 1.0/2.2); }
float3 srgbToLin(float3 c){ return pow(max(c,0.0), 2.2); }
float3 skyColor(float3 dir){ float3 c = lerp(gSkyHorizon.rgb, gSkyZenith.rgb, pow(saturate(dir.z*0.5+0.5), 0.65)); return srgbToLin(c); }
float3 fresnelSchlick(float ct, float3 F0){ return F0 + (1.0-F0)*pow(saturate(1.0-ct),5.0); }
float distGGX(float ndh, float a){ float a2=a*a; float d=ndh*ndh*(a2-1.0)+1.0; return a2/(PI*d*d+1e-6); }
float geomSchlick(float nd, float k){ return nd/(nd*(1.0-k)+k); }

// ---- PBR mesh with sky ambient + distance fog ----
struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; };
struct VSOut { float4 pos : SV_POSITION; float3 nrmWS : NORMAL; float3 wpos : TEXCOORD0; };

VSOut VSMain(VSIn i) {
    VSOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos = wp.xyz;
    o.pos = mul(wp, gViewProj);
    o.nrmWS = mul(float4(i.nrm, 0.0), gWorld).xyz;
    return o;
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
    float  metallic, rough, ndv;
    float  alpha;
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
    s.display = gMaterial.z > 0.5;          // unlit (gizmo/grid)
    s.displayColor = float4(gBaseColor.rgb, gBaseColor.a);
    s.albedo = srgbToLin(gBaseColor.rgb);
    s.ndv = saturate(dot(v.N, v.V));
    s.F0 = lerp(0.04.xxx, s.albedo, s.metallic);
    s.F = fresnelSchlick(saturate(dot(s.H, v.V)), s.F0);
    s.kdAlbedo = ((1.0 - s.F) * (1.0 - s.metallic)) * s.albedo;
    return s;
}

// True when the shading model produces an authored display colour that must reach the backbuffer
// untouched - no lighting, no fog, no tonemap. A renderer MUST honour this before shading:
// averShadeDirect / averShadeIndirect do not re-test it, because for every lit pixel that test is
// dead work in the inner loop.
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
    float a = s.rough * s.rough;
    float k = (s.rough + 1.0); k = k * k / 8.0;
    float ndl = saturate(dot(s.N, l.direction));
    float D = distGGX(saturate(dot(s.N, s.H)), a);
    float G = geomSchlick(s.ndv, k) * geomSchlick(ndl, k);
    float3 spec = (D * G * s.F) / (4.0 * s.ndv * ndl + 1e-4);
    return radiance + (s.kdAlbedo / PI + spec) * l.radiance * ndl * l.visibility;
}

// Ambient, bounce and environment specular, in that order.
float3 averShadeIndirect(float3 radiance, AverSurface s, AverIndirect ind) {
    float3 ambient = s.kdAlbedo * ind.ambient * ind.ambientScale;
    float3 envSpec = ind.specular * fresnelSchlick(s.ndv, s.F0) * (1.0 - s.rough);
    float3 indirect = s.kdAlbedo * ind.diffuse;
    ambient *= ind.occlusion;   // whatever produced the bounce already knows what is occluded
    radiance += ambient;
    radiance += indirect;
    radiance += envSpec * 0.35;
    return radiance;
}

// ---- camera / post. Deliberately NOT the material's: fog, tonemap and gamma are properties of
// the camera looking at the scene, and a material system that owned them would make every shading
// model reimplement them identically. ----
float3 averApplyFog(float3 color, float3 wpos) {
    float dist = length(wpos - gCamPos.xyz);
    float fog = 1.0 - exp(-dist * gFogColor.a);
    return lerp(color, srgbToLin(gFogColor.rgb), saturate(fog));
}

// ---- procedural sky (fullscreen triangle via SV_VertexID) ----
struct SkyOut { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };
SkyOut VSky(uint id : SV_VertexID) {
    SkyOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.ndc = uv * 2.0 - 1.0;
    o.pos = float4(o.ndc, 1.0, 1.0);
    return o;
}

#if AVER_MS
// ================= Mesh shader geometry path =================
// A mesh shader has no input assembler, so it reads the vertex and index buffers itself. Both are
// bound as ROOT SRVs (raw/structured buffers are allowed there), which keeps the per-mesh cost at
// two GPU virtual addresses and needs no descriptor heap slots at all.
//
// One thread group per 64 triangles, expanded unindexed: 192 vertices / 64 primitives, inside the
// 256/256 per-group limit. Skipping vertex reuse costs a little duplicate transform work and buys
// a much simpler shader; meshlet building belongs with the asset pipeline, not here.
//
// These three registers are the ones RHIResources.hpp reserves for geometry the backend binds
// itself. A feature module declaring anything at them collides silently.
//
// The two SRV registers sit just PAST whatever SRV table the pipeline declared, so they depend on
// layout.srvCount and cannot be written here as literals: a literal agrees with the root signature
// only while that count happens to match, and the day a layout grows an SRV the shader keeps asking
// for a register the root signature has moved something else into. That failure is invisible — at
// best CreateRootSignature rejects the overlap, at worst the mesh shader reads a texture as a
// vertex buffer, with no compile error and nothing for the debug layer to say. Hence -D macros,
// computed by the backend from the same field, and a hard error rather than a default if a caller
// forgets them.
#if !defined(AVER_MS_VTX_REG) || !defined(AVER_MS_IDX_REG)
#error "AVER_MS needs AVER_MS_VTX_REG / AVER_MS_IDX_REG from rhi::meshGeometryDefines"
#endif
// Two-step so the argument is expanded before it is pasted; one step pastes the macro NAME.
#define AVER_REG_JOIN2(a, b) a##b
#define AVER_REG_JOIN(a, b) AVER_REG_JOIN2(a, b)

struct MeshVtx { float3 pos; float3 nrm; };
StructuredBuffer<MeshVtx> gVerts   : register(AVER_REG_JOIN(t, AVER_MS_VTX_REG));
ByteAddressBuffer         gIndices : register(AVER_REG_JOIN(t, AVER_MS_IDX_REG));
cbuffer MeshCB : register(b5) { uint gTriCount; uint3 _msPad; };

#define AVER_MS_TRIS 64

// Triangles this group owns, and where its slice of the output arrays starts.
uint msTriCount(uint gid) { return min(AVER_MS_TRIS, gTriCount - gid * AVER_MS_TRIS); }

[numthreads(AVER_MS_TRIS, 1, 1)]
[outputtopology("triangle")]
void MSMain(uint gid : SV_GroupID, uint gtid : SV_GroupThreadID,
            out vertices VSOut verts[AVER_MS_TRIS * 3],
            out indices uint3 tris[AVER_MS_TRIS]) {
    uint count = msTriCount(gid);
    SetMeshOutputCounts(count * 3, count);   // must run for the whole group, before any output
    if (gtid >= count) return;

    uint3 idx = gIndices.Load3((gid * AVER_MS_TRIS + gtid) * 12);
    uint o = gtid * 3;
    [unroll] for (uint k = 0; k < 3; ++k) {
        MeshVtx v = gVerts[idx[k]];
        float4 wp = mul(float4(v.pos, 1.0), gWorld);
        VSOut ov;
        ov.wpos  = wp.xyz;
        ov.pos   = mul(wp, gViewProj);
        ov.nrmWS = mul(float4(v.nrm, 0.0), gWorld).xyz;
        verts[o + k] = ov;
    }
    tris[gtid] = uint3(o, o + 1, o + 2);
}
#endif // AVER_MS
)";
}

// One owner for both halves of the reserved-register contract: the prelude above consumes these
// macros, the root-signature builder places the matching root SRVs at the same two indices, and
// both read layout.srvCount rather than a number written down twice.
std::string meshGeometryDefines(const PipelineLayout& layout) {
    return "AVER_MS_VTX_REG=" + std::to_string(layout.srvCount) +
           ";AVER_MS_IDX_REG=" + std::to_string(layout.srvCount + 1);
}

} // namespace aver::rhi
