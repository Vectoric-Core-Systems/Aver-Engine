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

#include <string>

namespace aver::rhi {

namespace {

// Colour space and tonemapping, factored out so BOTH the scene prelude and the post chain below are
// built from the same text. They are the two ends of one round trip -- the scene writes linear
// radiance, the post chain tonemaps and encodes it, and an editor line inverts the pair to name a
// display colour -- so a second copy of any of these would be a divergence that shows up as a grid
// whose grey no longer matches the one it was authored as, and nothing to attribute it to.
const char* kColorHlsl = R"(
float3 acesTonemap(float3 x){ return saturate((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14)); }
float3 toGamma(float3 c){ return pow(max(c,0.0), 1.0/2.2); }
float3 srgbToLin(float3 c){ return pow(max(c,0.0), 2.2); }

// The EXACT inverse of acesTonemap, for the handful of things authored as a DISPLAY colour rather
// than as radiance: editor lines, gizmos, and the clear colour behind them all name the pixel they
// want on screen. The scene target is HDR and the tonemap runs at the end of the frame, so writing
// such a colour straight into it would put it through a curve it was never meant to see.
//
// The fit is a ratio of quadratics, so inverting it is a quadratic in x:
//   (2.43y - 2.51)x^2 + (0.59y - 0.03)x + 0.14y = 0
// and the root wanted is the positive one. Not an approximation and not a lookup: a display colour
// that survives the round trip only APPROXIMATELY is a grid whose grey drifts every time the post
// chain is touched.
float3 averInverseTonemap(float3 y) {
    // The fit asymptotes at 2.51/2.43; a value at or past it has no finite pre-image, so clamp just
    // below rather than divide by a vanishing discriminant.
    y = clamp(y, 0.0, 1.0329 - 1e-4);
    float3 a = 2.43 * y - 2.51;
    float3 b = 0.59 * y - 0.03;
    float3 c = 0.14 * y;
    float3 disc = sqrt(max(b * b - 4.0 * a * c, 0.0));
    // a is strictly negative over the clamped range, so (-b - disc) / (2a) is the positive root.
    return (-b - disc) / (2.0 * a);
}

// Rec. 709 relative luminance. Used by the bloom threshold, the Karis firefly weight and the
// exposure histogram, which must agree on what "bright" means or a highlight can be above the
// bloom threshold and below the histogram's.
float averLuminance(float3 c){ return dot(c, float3(0.2126, 0.7152, 0.0722)); }
)";

} // namespace

// The HLSL below hardcodes these; the C++ side reads the constants. Neither can move alone.
static_assert(kMeshShaderTrisPerGroup == 64, "AVER_MS_TRIS in the prelude is written out as 64");
static_assert(kMeshGeometryConstantRegister == 5, "MeshCB in the prelude is written out as b5");
static_assert(kObjectConstantRegister == 1, "PerObject in the prelude is written out as b1");
static_assert(kObjectConstantDwords == 32,
              "PerObject below is 32 dwords: world 16, base colour 4, material 4, model 4, emissive 4");

const char* sharedShaderPrelude() {
    // Composed once and cached. The pointer is held for the life of the process by every
    // ShaderDesc::prelude that borrows it, so the storage has to outlive every pipeline build.
    static const std::string s = std::string(kColorHlsl) + R"(
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
    // DEAD as of the material system owning reflectance: the shading model reads gMatReflectance /
    // gMatF90 out of b2, and the frozen path below hardcodes 0.04 with F(90) = 1. They are left
    // declared, and still written, only because removing them moves kObjectConstantDwords and
    // therefore every root signature that declares b1. Reclaim them together, not one at a time.
    float    gReflectance;   // unread
    float    gF90;           // unread
    float    _objPad;
    float4   gEmissive;      // rgb, radiance this surface emits on its own
};

static const float PI = 3.14159265;
float3 skyColor(float3 dir){ float3 c = lerp(gSkyHorizon.rgb, gSkyZenith.rgb, pow(saturate(dir.z*0.5+0.5), 0.65)); return srgbToLin(c); }

// ---- PBR mesh with sky ambient + distance fog ----
struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 pos : SV_POSITION; float3 nrmWS : NORMAL; float3 wpos : TEXCOORD0; float2 uv : TEXCOORD1; };

VSOut VSMain(VSIn i) {
    VSOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos = wp.xyz;
    o.pos = mul(wp, gViewProj);
    o.nrmWS = mul(float4(i.nrm, 0.0), gWorld).xyz;
    o.uv = i.uv;
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

    // LINEAR RADIANCE, not a display colour. The scene target is HDR and the tonemap is the last
    // thing that happens to the frame, in the post chain — a shader that tonemapped here would be
    // tonemapping twice, and would also destroy the range bloom and eye adaptation exist to read.
    return float4(color, gBaseColor.a);
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

// The element size of this struct IS the vertex stride on the mesh-shader path: gVerts is bound as
// a root SRV by raw GPU address, so nothing but this declaration tells the shader how far apart the
// vertices are. It must match rhi::MeshVertex byte for byte.
struct MeshVtx { float3 pos; float3 nrm; float2 uv; };
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
        ov.uv    = v.uv;
        verts[o + k] = ov;
    }
    tris[gtid] = uint3(o, o + 1, o + 2);
}
#endif // AVER_MS
)";
    return s.c_str();
}

// One owner for both halves of the reserved-register contract: the prelude above consumes these
// macros, the root-signature builder places the matching root SRVs at the same two indices, and
// both read declaredSrvCount(layout) rather than a number written down twice. It is the count
// across BOTH tables: table 1 is based at t(srvCount), so anything anchored to srvCount alone would
// now land on top of it.
std::string meshGeometryDefines(const PipelineLayout& layout) {
    const u32 base = declaredSrvCount(layout);
    return "AVER_MS_VTX_REG=" + std::to_string(base) +
           ";AVER_MS_IDX_REG=" + std::to_string(base + 1);
}

const char* postShaderSource() {
    static const std::string s = std::string(kColorHlsl) + R"(
// ================= the camera post chain =================
// Deliberately NOT built on the scene prelude. Every pass here runs with a root signature of its
// own, and declaring `cbuffer PerFrame : register(b0)` alongside this one would be two different
// layouts at the same register in one translation unit.
//
// One constant buffer serves every pass. A pass reads the fields it needs and ignores the rest,
// which is what lets the whole chain share one root signature and one upload -- seven small
// constant buffers would each be a separate suballocation and a separate root binding for what is
// never more than a dozen floats.
cbuffer AverPost : register(b0) {
    float4 gPostTone;    // x exposure, y bloom intensity, z bloom threshold, w bloom knee
    float4 gPostDst;     // xy destination size in texels, zw its reciprocal
    float4 gPostSrc;     // xy source size in texels,      zw its reciprocal
    float4 gPostAdapt;   // x min log2 luminance, y 1/log2 range, z adaption alpha, w pixels sampled
    float4 gPostLimit;   // x exposure min, y exposure max, z histogram low cut, w high cut
    float4 gPostMisc;    // x middle grey, y auto-exposure on, z bloom filter radius, w unused
};

Texture2D<float4>     gPostSceneTex : register(t0);
Texture2D<float4>     gPostBloomTex : register(t1);
ByteAddressBuffer     gPostExpRead  : register(t2);
SamplerState          gPostSamp     : register(s0);

// Only the two adaptation passes write, and both write BUFFERS -- see the note on the bloom chain
// for why nothing here writes a texture through a UAV.
RWByteAddressBuffer   gPostHist     : register(u0);
RWByteAddressBuffer   gPostExp      : register(u1);

// ---- bloom ---------------------------------------------------------------------------------
// The pyramid is a HALF-resolution RGBA16F chain: a bloom halo is low-frequency by definition, so
// building it at full resolution buys nothing visible and costs four times the bandwidth. Every
// step below reads through a bilinear sampler, which is what makes a 13-tap downsample cost 13
// samples instead of 52.
//
// GRAPHICS passes, not compute, and that is a portability decision rather than a stylistic one.
// The upsample is additive, and a compute version would have to READ its own RWTexture2D — but a
// typed UAV load of RGBA16F is an optional D3D12 feature (this engine has a whole `no-typed-uav`
// gate configuration for hardware without it), so that shader would simply not run on some of the
// devices the oracle covers. The fixed-function blender adds for free and is universal.
//
// Each pass reads ONE mip through its own single-mip SRV and writes the next through an RTV, which
// is what makes reading and writing the same resource in one draw legal: the two subresources are
// in different states.

// Karis' firefly weight. A single pixel far brighter than its neighbours survives every box filter
// in the chain and re-emerges as a large flickering blob; weighting each tap by 1/(1+luma) averages
// in a space where that pixel cannot dominate. Applied ONLY on the first downsample, because after
// it the energy is already spread and further weighting would just darken the halo.
float3 averBloomKaris(float3 c) { return c / (1.0 + averLuminance(c)); }

// Soft-knee threshold, the standard quadratic ramp. A hard cut makes bloom appear and disappear as
// a highlight crosses it, which reads as flicker in motion and is far more objectionable than the
// halo the threshold exists to control.
float3 averBloomPrefilter(float3 c) {
    float br = max(c.r, max(c.g, c.b));
    float knee = max(gPostTone.w, 1e-4);
    float soft = clamp(br - gPostTone.z + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float weight = max(soft, br - gPostTone.z) / max(br, 1e-4);
    return c * weight;
}

struct AverPostVSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

AverPostVSOut PostVS(uint id : SV_VertexID) {
    AverPostVSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float4 PSBloomPrefilter(AverPostVSOut i) : SV_TARGET {
    // Sampling at the destination texel centre with a bilinear tap over a source twice as large is
    // exactly a 2x2 box, so four taps here cover a 4x4 source neighbourhood.
    float2 o = gPostSrc.zw;
    float3 a = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2(-o.x, -o.y), 0).rgb;
    float3 b = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2( o.x, -o.y), 0).rgb;
    float3 c = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2(-o.x,  o.y), 0).rgb;
    float3 d = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2( o.x,  o.y), 0).rgb;
    float3 sum = averBloomKaris(a) + averBloomKaris(b) + averBloomKaris(c) + averBloomKaris(d);
    return float4(averBloomPrefilter(sum * 0.25), 1.0);
}

// Jimenez' 13-tap downsample (Call of Duty: Advanced Warfare, SIGGRAPH 2014). The extra centre box
// over a plain 4-tap is what stops the chain aliasing into a shimmering mess as the camera moves.
float4 PSBloomDown(AverPostVSOut i) : SV_TARGET {
    float2 uv = i.uv;
    float2 o = gPostSrc.zw;

    float3 a = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-2*o.x,  2*o.y), 0).rgb;
    float3 b = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(     0,  2*o.y), 0).rgb;
    float3 c = gPostSceneTex.SampleLevel(gPostSamp, uv + float2( 2*o.x,  2*o.y), 0).rgb;
    float3 d = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-2*o.x,      0), 0).rgb;
    float3 e = gPostSceneTex.SampleLevel(gPostSamp, uv,                          0).rgb;
    float3 f = gPostSceneTex.SampleLevel(gPostSamp, uv + float2( 2*o.x,      0), 0).rgb;
    float3 g = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-2*o.x, -2*o.y), 0).rgb;
    float3 h = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(     0, -2*o.y), 0).rgb;
    float3 j = gPostSceneTex.SampleLevel(gPostSamp, uv + float2( 2*o.x, -2*o.y), 0).rgb;
    float3 k = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(  -o.x,    o.y), 0).rgb;
    float3 l = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(   o.x,    o.y), 0).rgb;
    float3 m = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(  -o.x,   -o.y), 0).rgb;
    float3 n = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(   o.x,   -o.y), 0).rgb;

    float3 sum = e * 0.125;
    sum += (a + c + g + j) * 0.03125;
    sum += (b + d + f + h) * 0.0625;
    sum += (k + l + m + n) * 0.125;
    return float4(sum, 1.0);
}

// 9-tap tent upsample, ADDED into the level above by the blender rather than replacing it. Additive
// is what makes the pyramid a sum of scales -- a halo with a tight core and a wide skirt -- instead
// of only the coarsest level smeared back over the frame.
float4 PSBloomUp(AverPostVSOut i) : SV_TARGET {
    // The radius is in DESTINATION uv, not source texels, so the skirt keeps a constant angular
    // width as the chain gets coarser instead of doubling with every level.
    float2 o = gPostMisc.z * gPostDst.zw;
    float2 uv = i.uv;

    float3 sum = gPostSceneTex.SampleLevel(gPostSamp, uv, 0).rgb * 0.25;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-o.x,  0.0), 0).rgb * 0.125;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2( o.x,  0.0), 0).rgb * 0.125;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2( 0.0, -o.y), 0).rgb * 0.125;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2( 0.0,  o.y), 0).rgb * 0.125;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-o.x, -o.y), 0).rgb * 0.0625;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2( o.x, -o.y), 0).rgb * 0.0625;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-o.x,  o.y), 0).rgb * 0.0625;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2( o.x,  o.y), 0).rgb * 0.0625;
    return float4(sum, 1.0);
}

// ---- eye adaptation ------------------------------------------------------------------------
// A 256-bin histogram of log2 luminance, reduced to one exposure value that is damped towards over
// time. A histogram rather than an average because an average is dominated by whatever occupies the
// most pixels: a bright sky closes the aperture on an interior, and one specular highlight closes
// it on everything.
groupshared uint gHistLocal[256];

[numthreads(16, 16, 1)]
void CSHistogram(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    gHistLocal[gi] = 0;
    GroupMemoryBarrierWithGroupSync();

    // Sampled over a REDUCED grid rather than one thread per pixel. A histogram is a statistic, and
    // bilinear taps at quarter resolution give the same answer for a sixteenth of the threads --
    // which at a 2750x1711 viewport is the difference between 4.7 million threads a frame and
    // 300 thousand. gPostDst is the grid, gPostDst.zw its reciprocal.
    if (tid.x < (uint)gPostDst.x && tid.y < (uint)gPostDst.y) {
        float2 uv = (tid.xy + 0.5) * gPostDst.zw;
        float lum = averLuminance(gPostSceneTex.SampleLevel(gPostSamp, uv, 0).rgb);
        // Bin 0 is reserved for "black". Without it, log2 of a near-zero luminance floors into the
        // first real bin and a dark scene reads as if it were full of very dim detail, which drives
        // the adaptation to its maximum and blows out the first thing that lights up.
        uint bin = 0;
        if (lum > 1e-4) {
            float t = saturate((log2(lum) - gPostAdapt.x) * gPostAdapt.y);
            bin = (uint)(t * 254.0 + 1.0);
        }
        InterlockedAdd(gHistLocal[bin], 1);
    }
    GroupMemoryBarrierWithGroupSync();

    // One global atomic per BIN per group rather than one per pixel. That is the whole reason for
    // the groupshared pass above: 256 atomics per 256 pixels instead of 65536.
    uint local = gHistLocal[gi];
    if (local > 0) gPostHist.InterlockedAdd(gi * 4, local);
}

// Single-threaded on purpose. It is 256 iterations once per frame against a parallel reduction's
// several dispatches and barriers -- the reduction would be the slower of the two here, and it is
// the one with somewhere to hide an off-by-one.
[numthreads(1, 1, 1)]
void CSExposure() {
    float target = gPostTone.x;

    if (gPostMisc.y > 0.5) {
        uint total = 0;
        uint counts[256];
        for (uint i = 0; i < 256; ++i) { counts[i] = gPostHist.Load(i * 4); total += counts[i]; }

        // Trim both tails before averaging. Bin 0 (black) is excluded outright: sky-free interiors
        // are mostly it, and letting it into the average drags the exposure to the maximum.
        float lowCut  = total * gPostLimit.z;
        float highCut = total * gPostLimit.w;
        float seen = 0.0, weighted = 0.0, weight = 0.0;
        for (uint b = 1; b < 256; ++b) {
            float c = counts[b];
            float lo = max(lowCut - seen, 0.0);
            float hi = max(seen + c - highCut, 0.0);
            float take = max(c - lo - hi, 0.0);
            weighted += take * (float)b;
            weight += take;
            seen += c;
        }

        if (weight > 0.0) {
            float avgBin = weighted / weight;
            float avgLum = exp2((avgBin - 1.0) / 254.0 / gPostAdapt.y + gPostAdapt.x);
            target = gPostMisc.x / max(avgLum, 1e-4);
        }
        target = clamp(target, gPostLimit.x, gPostLimit.y);

        // Zeroed here rather than in a clear pass of its own: this is the one thread that has
        // finished reading it, so there is no barrier to get wrong.
        for (uint z = 0; z < 256; ++z) gPostHist.Store(z * 4, 0);
    }

    // Damped towards, in LOG space, so the same alpha takes the same time per stop whether the
    // scene got brighter or darker. Linear damping adapts to darkness far slower than to light and
    // reads as the eye sticking.
    float prev = asfloat(gPostExp.Load(0));
    uint  seeded = gPostExp.Load(4);
    float next = (seeded == 0 || prev <= 0.0)
               ? target
               : exp2(lerp(log2(prev), log2(max(target, 1e-4)), saturate(gPostAdapt.z)));
    gPostExp.Store(0, asuint(next));
    gPostExp.Store(4, 1);
}

// ---- composite -----------------------------------------------------------------------------
// The one place the frame becomes a display image. Exposure, bloom, tonemap and gamma in a single
// pass: each is a handful of instructions, and splitting them would cost a full-resolution
// round trip through memory per stage for no benefit at all.
float4 PSComposite(AverPostVSOut i) : SV_TARGET {
    float3 c = gPostSceneTex.SampleLevel(gPostSamp, i.uv, 0).rgb;
#ifdef AVER_POST_BLOOM
    c += gPostBloomTex.SampleLevel(gPostSamp, i.uv, 0).rgb * gPostTone.y;
#endif
#ifdef AVER_POST_AUTOEXPOSURE
    c *= asfloat(gPostExpRead.Load(0));
#else
    c *= gPostTone.x;
#endif
    return float4(toGamma(acesTonemap(c)), 1.0);
}
)";
    return s.c_str();
}

} // namespace aver::rhi
