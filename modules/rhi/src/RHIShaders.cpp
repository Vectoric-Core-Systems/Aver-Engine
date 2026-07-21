// The HLSL every GPU consumer shares: the constant-buffer layouts the backend uploads, the vertex
// structures it feeds, the colour-space helpers, and the camera post. The BRDF is NOT here any
// more - pbr::materialShaderPrelude() owns it, because a material system is a module and not a
// property of the hardware interface.
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
static_assert(kObjectConstantRegister == 1, "PerObject in the prelude is written out as b1");
static_assert(kObjectConstantDwords == 32,
              "PerObject below is 32 dwords: world 16, base colour 4, material 4, model 4, emissive 4");

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
// Per draw. The shading model is an ID rather than a shader permutation: a uniform branch costs one
// scalar compare per wave, where a permutation would multiply the pipeline count of every renderer
// that draws scene geometry -- Voxi already builds eleven and rebuilds four of them on every MSAA
// change.
cbuffer PerObject : register(b1) {
    float4x4 gWorld;
    float4   gBaseColor;
    // z is the unlit flag the FROZEN no-material path below still reads. The material system reads
    // gShadingModel instead; the two are not kept in step, because the frozen path exists precisely
    // so that it never has to be.
    float4   gMaterial;      // x=metallic, y=roughness, z=unlit(0/1) for plainShadeSurface only
    uint     gShadingModel;  // which shading model evaluates this draw
    float    gReflectance;   // normal-incidence reflectance of the dielectric base
    float    gF90;           // grazing-angle reflectance
    float    _objPad;
    float4   gEmissive;      // rgb, radiance this surface emits on its own
};

static const float PI = 3.14159265;
float3 acesTonemap(float3 x){ return saturate((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14)); }
float3 toGamma(float3 c){ return pow(max(c,0.0), 1.0/2.2); }
float3 srgbToLin(float3 c){ return pow(max(c,0.0), 2.2); }
float3 skyColor(float3 dir){ float3 c = lerp(gSkyHorizon.rgb, gSkyZenith.rgb, pow(saturate(dir.z*0.5+0.5), 0.65)); return srgbToLin(c); }

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

// ---- the image you get with NO material module: FROZEN, do not evolve ----
// Aver.Render.PBR.Materials owns the BRDF now, and this backend cannot link it: the material system
// depends on Aver.RHI, so a backend that depended on the material system would invert the layering.
// What stays here is therefore the shading exactly as it stood before the contract was split out,
// under private names so this string and pbr::materialShaderPrelude() can be concatenated.
//
// It is a verbatim copy on purpose. A hand-written lambert or unlit substitute would be a silent
// divergence with no oracle behind it and nothing to catch it: PSMainPlain is unreachable in any
// Voxi-enabled build, so no gate would ever look at the result.
float3 plainFresnelSchlick(float ct, float3 F0){ return F0 + (1.0-F0)*pow(saturate(1.0-ct),5.0); }
float plainDistGGX(float ndh, float a){ float a2=a*a; float d=ndh*ndh*(a2-1.0)+1.0; return a2/(PI*d*d+1e-6); }
float plainGeomSchlick(float nd, float k){ return nd/(nd*(1.0-k)+k); }

float4 plainShadeSurface(VSOut i, float sunVis, float3 indirectRadiance, float ao) {
    float3 N = normalize(i.nrmWS);
    float3 V = normalize(gCamPos.xyz - i.wpos);
    float3 L = normalize(gLightDir.xyz);
    float3 H = normalize(V + L);
    float metallic = saturate(gMaterial.x);
    float rough = clamp(gMaterial.y, 0.045, 1.0);

    if (gMaterial.z > 0.5) { // unlit (gizmo/grid): authored display colour, no lighting
        return float4(gBaseColor.rgb, gBaseColor.a);
    }

    float3 albedo = srgbToLin(gBaseColor.rgb);
    float3 lightC = srgbToLin(gLightColor.rgb) * 3.0; // sun radiance
    float ndv = saturate(dot(N, V));
    float ndl = saturate(dot(N, L));
    float3 F0 = lerp(0.04.xxx, albedo, metallic);

    // direct (Cook-Torrance GGX)
    float a = rough * rough;
    float k = (rough + 1.0); k = k * k / 8.0;
    float D = plainDistGGX(saturate(dot(N, H)), a);
    float G = plainGeomSchlick(ndv, k) * plainGeomSchlick(ndl, k);
    float3 F = plainFresnelSchlick(saturate(dot(H, V)), F0);
    float3 spec = (D * G * F) / (4.0 * ndv * ndl + 1e-4);
    float3 kd = (1.0 - F) * (1.0 - metallic);
    float3 direct = (kd * albedo / PI + spec) * lightC * ndl * sunVis;

    // ambient: sky hemisphere irradiance (linear) + crude spec reflection of the sky
    float3 ambient = kd * albedo * skyColor(N) * gAmbient.r;
    float3 R = reflect(-V, N);
    float3 envSpec = skyColor(R) * plainFresnelSchlick(ndv, F0) * (1.0 - rough);

    float3 indirect = kd * albedo * indirectRadiance;
    ambient *= ao;   // whatever produced the indirect term already knows what is occluded
    float3 color = direct + ambient + indirect + envSpec * 0.35;

    // distance fog (linear space)
    float dist = length(i.wpos - gCamPos.xyz);
    float fog = 1.0 - exp(-dist * gFogColor.a);
    color = lerp(color, srgbToLin(gFogColor.rgb), saturate(fog));

    return float4(toGamma(acesTonemap(color)), gBaseColor.a);
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
