// Fabric G6 SH2: Godot spatial shaders on Aver materials (docs/plans/fabric/G6_SHADERS.md).
//
// Added to the translation unit by pbr::materialGraphHlsl() ONLY when a registered material graph carries a GodotShader node, after material_prelude.hlsl
// and before the converted shaders (tools/fabric emits them with gdShaderEmitHlsl in slot-sampler mode). It defines
//   - the AVER_GD_* hook macros the emitted code reads and writes (one context struct, gGd, shared by the vertex and fragment functions),
//   - the Godot <-> Aver basis change (Godot: right-handed, +Y up, -Z forward, metres; Aver: left-handed, +Z up, +X forward, centimetres:
//     aver::fmt::axis, AxisMap.hpp) so the shader sees view space, model space and world space as Godot defines them,
//   - the uniform table (a RGBA32F texture in material slot 7) and the texture slots with Godot's filter / repeat hints emulated on the one shared sampler,
//   - the fragment() -> AverAuthored mapping and the vertex() -> VSOut mapping.
// Two flavours of everything that samples or differentiates: the raster flavour (gdSlot*, uses implicit derivatives) and the ray flavour (gdrSlot*, reads the
// bindless table through averRtGd*, no derivative anywhere, so a compute pass can compile it). The converted source is included once per flavour with the
// name macro GD_NS_CUR and the gd_/ddx names redirected (MaterialGraphHlsl.cpp).

#ifndef AVER_GD_MAX_U
#define AVER_GD_MAX_U 1
#endif
#ifndef AVER_GD_MAX_I
#define AVER_GD_MAX_I 1
#endif
#ifndef AVER_GD_MAX_G
#define AVER_GD_MAX_G 1
#endif

#define GD_CAT2(a, b) a##b
#define GD_CAT(a, b) GD_CAT2(a, b)

// ---- the basis change ---------------------------------------------------------------------------------------------------------------------------
// engine = (-g.z, g.x, g.y) for a direction; positions also scale by 100 (cm).
float3 gdE2G(float3 e) { return float3(e.y, e.z, -e.x); }
float3 gdG2E(float3 g) { return float3(-g.z, g.x, g.y); }

static float3 gGdCamR = float3(0.0, 1.0, 0.0);
static float3 gGdCamU = float3(0.0, 0.0, 1.0);
static float3 gGdCamF = float3(1.0, 0.0, 0.0);
void gdCamInit() {
    const float3 f = averViewRayDir(float2(0.0, 0.0));
    float3 r = averViewRayDir(float2(1.0, 0.0)) - averViewRayDir(float2(-1.0, 0.0));
    float3 u = averViewRayDir(float2(0.0, 1.0)) - averViewRayDir(float2(0.0, -1.0));
    r = normalize(r - f * dot(r, f));
    u = normalize(u - f * dot(u, f) - r * dot(u, r));
    gGdCamF = f; gGdCamR = r; gGdCamU = u;
}
// Engine world (cm) -> Godot view space (m; x right, y up, -z forward).
float3 gdWorldPosToView(float3 p) { const float3 d = (p - gCamPos.xyz) * 0.01; return float3(dot(d, gGdCamR), dot(d, gGdCamU), -dot(d, gGdCamF)); }
float3 gdDirToView(float3 d) { return float3(dot(d, gGdCamR), dot(d, gGdCamU), -dot(d, gGdCamF)); }
float3 gdViewDirToWorld(float3 v) { return gGdCamR * v.x + gGdCamU * v.y - gGdCamF * v.z; }
float3 gdViewPosToWorld(float3 v) { return gCamPos.xyz + gdViewDirToWorld(v) * 100.0; }

// GLSL matrices keep their memory order: an HLSL float4x4 holds GLSL column i in row i (the emitter's contract), so these build rows from columns.
float4x4 gdViewMatrix() {
    const float3 R = gdE2G(gGdCamR), U = gdE2G(gGdCamU), B = gdE2G(-gGdCamF), c = gdE2G(gCamPos.xyz) * 0.01;
    return float4x4(float4(R.x, U.x, B.x, 0.0), float4(R.y, U.y, B.y, 0.0), float4(R.z, U.z, B.z, 0.0), float4(-dot(R, c), -dot(U, c), -dot(B, c), 1.0));
}
float4x4 gdInvViewMatrix() {
    const float3 R = gdE2G(gGdCamR), U = gdE2G(gGdCamU), B = gdE2G(-gGdCamF), c = gdE2G(gCamPos.xyz) * 0.01;
    return float4x4(float4(R, 0.0), float4(U, 0.0), float4(B, 0.0), float4(c, 1.0));
}
float3 gdModelAxis(float3 godotAxis) { return gdE2G(mul(float4(gdG2E(godotAxis), 0.0), gWorld).xyz); }
float4x4 gdModelMatrix() {
    return float4x4(float4(gdModelAxis(float3(1.0, 0.0, 0.0)), 0.0), float4(gdModelAxis(float3(0.0, 1.0, 0.0)), 0.0), float4(gdModelAxis(float3(0.0, 0.0, 1.0)), 0.0),
                    float4(gdE2G(gWorld[3].xyz) * 0.01, 1.0));
}
float3x3 gdModelNormalMatrix() {
    return float3x3(normalize(gdModelAxis(float3(1.0, 0.0, 0.0))), normalize(gdModelAxis(float3(0.0, 1.0, 0.0))), normalize(gdModelAxis(float3(0.0, 0.0, 1.0))));
}
float4x4 gdIdentity4() { return float4x4(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0); }

// ---- the context ----------------------------------------------------------------------------------------------------------------------------------
struct GdCtx {
    // inputs / inouts of both stages (vertex: model or world space, Godot axes; fragment: view space)
    float3 vertex, normal, tangent, binormal, normalIn;
    float2 uv, uv2;
    float4 color;
    // fragment inputs
    float4 fragcoord;
    bool   frontFacing;
    float3 view, lightVertex;
    float2 screenUv, pointCoord;
    // fragment outputs
    float3 albedo, emission, normalMap, bentNormalMap, backlight;
    float  alpha, alphaScissor, alphaHashScale, alphaAAEdge, premulFactor;
    float  metallic, roughness, specular, ao, aoLightAffect, normalMapDepth, depth;
    float  rim, rimTint, clearcoat, clearcoatGloss, anisotropy;
    float  sssStrength, sssDepth, sssBoost;
    float2 anisotropyFlow, alphaTexCoord;
    float4 sssColor, fog, radiance, irradiance;
    // vertex-only
    float4   position;
    float    pointSize, zClipScale, vRoughness;
    float4x4 modelview, projection;
    float3x3 modelviewNormal;
};
static GdCtx gGd;
static uint  gGdFx = 0u;      // gdfx bits of the shader the arm just ran (read by gdPost)

#define AVER_GD_VIEWPORT_W 1920.0
#define AVER_GD_VIEWPORT_H 1080.0

// Built-in variables as macros (SH1 contract: the names are AVER_GD_ + the Godot name).
#define AVER_GD_TIME gTime.x
#define AVER_GD_OUTPUT_IS_SRGB false
#define AVER_GD_VIEWPORT_SIZE float2(AVER_GD_VIEWPORT_W, AVER_GD_VIEWPORT_H)
#define AVER_GD_VIEW_MATRIX gdViewMatrix()
#define AVER_GD_INV_VIEW_MATRIX gdInvViewMatrix()
#define AVER_GD_MAIN_CAM_INV_VIEW_MATRIX gdInvViewMatrix()
#define AVER_GD_INV_PROJECTION_MATRIX gdIdentity4()
#define AVER_GD_PROJECTION_MATRIX gGd.projection
#define AVER_GD_NODE_POSITION_WORLD (gdE2G(gWorld[3].xyz) * 0.01)
#define AVER_GD_NODE_POSITION_VIEW gdWorldPosToView(gWorld[3].xyz)
#define AVER_GD_CAMERA_POSITION_WORLD (gdE2G(gCamPos.xyz) * 0.01)
#define AVER_GD_CAMERA_DIRECTION_WORLD gdE2G(gGdCamF)
#define AVER_GD_CAMERA_VISIBLE_LAYERS 0xFFFFFu
#define AVER_GD_INSTANCE_ID 0
#define AVER_GD_INSTANCE_CUSTOM float4(0.0, 0.0, 0.0, 0.0)
#define AVER_GD_VIEW_INDEX 0
#define AVER_GD_VIEW_MONO_LEFT 0
#define AVER_GD_VIEW_RIGHT 0
#define AVER_GD_EYE_OFFSET float3(0.0, 0.0, 0.0)
#define AVER_GD_VERTEX gGd.vertex
#define AVER_GD_VERTEX_ID 0
#define AVER_GD_POSITION gGd.position
#define AVER_GD_ROUGHNESS gGd.roughness
#define AVER_GD_POINT_SIZE gGd.pointSize
#define AVER_GD_MODELVIEW_MATRIX gGd.modelview
#define AVER_GD_MODELVIEW_NORMAL_MATRIX gGd.modelviewNormal
#define AVER_GD_MODEL_MATRIX gdModelMatrix()
#define AVER_GD_MODEL_NORMAL_MATRIX gdModelNormalMatrix()
#define AVER_GD_BONE_INDICES uint4(0u, 0u, 0u, 0u)
#define AVER_GD_BONE_WEIGHTS float4(0.0, 0.0, 0.0, 0.0)
#define AVER_GD_CUSTOM0 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_GD_CUSTOM1 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_GD_CUSTOM2 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_GD_CUSTOM3 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_GD_Z_CLIP_SCALE gGd.zClipScale
#define AVER_GD_NORMAL gGd.normal
#define AVER_GD_TANGENT gGd.tangent
#define AVER_GD_BINORMAL gGd.binormal
#define AVER_GD_UV gGd.uv
#define AVER_GD_UV2 gGd.uv2
#define AVER_GD_COLOR gGd.color
#define AVER_GD_FRAGCOORD gGd.fragcoord
#define AVER_GD_FRONT_FACING gGd.frontFacing
#define AVER_GD_VIEW gGd.view
#define AVER_GD_POINT_COORD gGd.pointCoord
#define AVER_GD_LIGHT_VERTEX gGd.lightVertex
#define AVER_GD_SCREEN_UV gGd.screenUv
#define AVER_GD_DEPTH gGd.depth
#define AVER_GD_NORMAL_MAP gGd.normalMap
#define AVER_GD_NORMAL_MAP_DEPTH gGd.normalMapDepth
#define AVER_GD_BENT_NORMAL_MAP gGd.bentNormalMap
#define AVER_GD_ALBEDO gGd.albedo
#define AVER_GD_ALPHA gGd.alpha
#define AVER_GD_ALPHA_SCISSOR_THRESHOLD gGd.alphaScissor
#define AVER_GD_ALPHA_HASH_SCALE gGd.alphaHashScale
#define AVER_GD_ALPHA_ANTIALIASING_EDGE gGd.alphaAAEdge
#define AVER_GD_ALPHA_TEXTURE_COORDINATE gGd.alphaTexCoord
#define AVER_GD_PREMUL_ALPHA_FACTOR gGd.premulFactor
#define AVER_GD_METALLIC gGd.metallic
#define AVER_GD_SPECULAR gGd.specular
#define AVER_GD_RIM gGd.rim
#define AVER_GD_RIM_TINT gGd.rimTint
#define AVER_GD_CLEARCOAT gGd.clearcoat
#define AVER_GD_CLEARCOAT_GLOSS gGd.clearcoatGloss
#define AVER_GD_ANISOTROPY gGd.anisotropy
#define AVER_GD_ANISOTROPY_FLOW gGd.anisotropyFlow
#define AVER_GD_SSS_STRENGTH gGd.sssStrength
#define AVER_GD_SSS_TRANSMITTANCE_COLOR gGd.sssColor
#define AVER_GD_SSS_TRANSMITTANCE_DEPTH gGd.sssDepth
#define AVER_GD_SSS_TRANSMITTANCE_BOOST gGd.sssBoost
#define AVER_GD_BACKLIGHT gGd.backlight
#define AVER_GD_AO gGd.ao
#define AVER_GD_AO_LIGHT_AFFECT gGd.aoLightAffect
#define AVER_GD_EMISSION gGd.emission
#define AVER_GD_FOG gGd.fog
#define AVER_GD_RADIANCE gGd.radiance
#define AVER_GD_IRRADIANCE gGd.irradiance

// The uniform blocks, as register arrays the emitted code indexes. Filled by the arm that runs the shader.
static float4 gGdU[AVER_GD_MAX_U];
static float4 gGdI[AVER_GD_MAX_I];
static float4 gGdG[AVER_GD_MAX_G];
#define AVER_GD_UBUF gGdU
#define AVER_GD_IBUF gGdI
#define AVER_GD_GBUF gGdG

// ---- texture slots ---------------------------------------------------------------------------------------------------------------------------------
// Slot s is pbr::TextureSlot s. The mode word of a sampler value: bit 0 nearest, 1 repeat, 2 mipmaps, bits 8-9 the colour of an unbound texture
// (0 white, 1 black, 2 transparent, 3 flat normal).
#define GD_SLOT_SWITCH(CALL) \
    switch (slot) { \
    case 0u: return gBaseColorMap.CALL; case 1u: return gMetalRoughMap.CALL; case 2u: return gNormalMap.CALL; case 3u: return gOcclusionMap.CALL; \
    case 4u: return gEmissiveMap.CALL; case 5u: return gL1BaseColorMap.CALL; case 6u: return gL1MetalRoughMap.CALL; default: return gL1NormalMap.CALL; }

bool gdSlotBound(uint slot) {
    const uint bit = slot < 5u ? slot : slot + 4u;   // MaterialFlag bits: 0-4 the five maps, 9-11 the layer-1 maps
    return ((gMaterialFlags >> bit) & 1u) != 0u;
}
float4 gdSlotDefault(uint mode) {
    const uint c = (mode >> 8) & 3u;
    return c == 1u ? float4(0.0, 0.0, 0.0, 1.0) : (c == 2u ? float4(0.0, 0.0, 0.0, 0.0) : (c == 3u ? float4(0.5, 0.5, 1.0, 1.0) : float4(1.0, 1.0, 1.0, 1.0)));
}
int2 gdSlotSize(uint slot, int lod) {
    uint w = 1u, h = 1u, l = 1u;
    switch (slot) {
    case 0u: gBaseColorMap.GetDimensions((uint)lod, w, h, l); break;
    case 1u: gMetalRoughMap.GetDimensions((uint)lod, w, h, l); break;
    case 2u: gNormalMap.GetDimensions((uint)lod, w, h, l); break;
    case 3u: gOcclusionMap.GetDimensions((uint)lod, w, h, l); break;
    case 4u: gEmissiveMap.GetDimensions((uint)lod, w, h, l); break;
    case 5u: gL1BaseColorMap.GetDimensions((uint)lod, w, h, l); break;
    case 6u: gL1MetalRoughMap.GetDimensions((uint)lod, w, h, l); break;
    default: gL1NormalMap.GetDimensions((uint)lod, w, h, l); break;
    }
    return int2((int)w, (int)h);
}
int gdSlotLevels(uint slot) {
    uint w = 1u, h = 1u, l = 1u;
    switch (slot) {
    case 0u: gBaseColorMap.GetDimensions(0u, w, h, l); break;
    case 1u: gMetalRoughMap.GetDimensions(0u, w, h, l); break;
    case 2u: gNormalMap.GetDimensions(0u, w, h, l); break;
    case 3u: gOcclusionMap.GetDimensions(0u, w, h, l); break;
    case 4u: gEmissiveMap.GetDimensions(0u, w, h, l); break;
    case 5u: gL1BaseColorMap.GetDimensions(0u, w, h, l); break;
    case 6u: gL1MetalRoughMap.GetDimensions(0u, w, h, l); break;
    default: gL1NormalMap.GetDimensions(0u, w, h, l); break;
    }
    return (int)l;
}
float4 gdSlotFetch(uint slot, int2 p, int lod) { GD_SLOT_SWITCH(Load(int3(p, lod))) }
float4 gdSlotLoadReg(int i) { return gL1NormalMap.Load(int3(i, 0, 0)); }

// repeat_disable: clamp to the edge texels (the engine's sampler wraps).
float2 gdSlotUv(uint mode, float2 uv, int2 sz) {
    if ((mode & 2u) != 0u) return uv;
    const float2 h = 0.5 / float2(sz);
    return clamp(uv, h, 1.0 - h);
}
float4 gdSlotNearestAt(uint slot, uint mode, float2 uv, int level) {
    const int2 sz = gdSlotSize(slot, level);
    int2 p = int2(floor(uv * float2(sz)));
    p = (mode & 2u) != 0u ? ((p % sz) + sz) % sz : clamp(p, int2(0, 0), sz - 1);
    return gdSlotFetch(slot, p, level);
}
#ifndef AVER_GD_NO_DERIVATIVES
float gdSlotCalcLod(uint slot, float2 uv) {
    switch (slot) {
    case 0u: return gBaseColorMap.CalculateLevelOfDetail(gMaterialSampler, uv);
    case 1u: return gMetalRoughMap.CalculateLevelOfDetail(gMaterialSampler, uv);
    case 2u: return gNormalMap.CalculateLevelOfDetail(gMaterialSampler, uv);
    case 3u: return gOcclusionMap.CalculateLevelOfDetail(gMaterialSampler, uv);
    case 4u: return gEmissiveMap.CalculateLevelOfDetail(gMaterialSampler, uv);
    case 5u: return gL1BaseColorMap.CalculateLevelOfDetail(gMaterialSampler, uv);
    case 6u: return gL1MetalRoughMap.CalculateLevelOfDetail(gMaterialSampler, uv);
    default: return gL1NormalMap.CalculateLevelOfDetail(gMaterialSampler, uv);
    }
}
#else
float gdSlotCalcLod(uint slot, float2 uv) { return 0.0; }
#endif

float4 gdSlotSample(uint slot, uint mode, float2 uv, float bias) {
    if (!gdSlotBound(slot)) return gdSlotDefault(mode);
    const int2 sz = gdSlotSize(slot, 0);
    if ((mode & 1u) != 0u) {
        const int level = (mode & 4u) != 0u ? clamp((int)round(gdSlotCalcLod(slot, uv)), 0, gdSlotLevels(slot) - 1) : 0;
        return gdSlotNearestAt(slot, mode, uv, level);
    }
    uv = gdSlotUv(mode, uv, sz);
#ifdef AVER_GD_NO_DERIVATIVES
    GD_SLOT_SWITCH(SampleLevel(gMaterialSampler, uv, 0.0))
#else
    if (bias != 0.0) { GD_SLOT_SWITCH(SampleBias(gMaterialSampler, uv, bias)) }
    GD_SLOT_SWITCH(Sample(gMaterialSampler, uv))
#endif
}
float4 gdSlotSampleLod(uint slot, uint mode, float2 uv, float lod) {
    if (!gdSlotBound(slot)) return gdSlotDefault(mode);
    const int2 sz = gdSlotSize(slot, 0);
    if ((mode & 1u) != 0u) return gdSlotNearestAt(slot, mode, uv, (mode & 4u) != 0u ? clamp((int)round(lod), 0, gdSlotLevels(slot) - 1) : 0);
    uv = gdSlotUv(mode, uv, sz);
    GD_SLOT_SWITCH(SampleLevel(gMaterialSampler, uv, lod))
}
float4 gdSlotSampleGrad(uint slot, uint mode, float2 uv, float2 dx, float2 dy) {
    if (!gdSlotBound(slot)) return gdSlotDefault(mode);
    const int2 sz = gdSlotSize(slot, 0);
    if ((mode & 1u) != 0u) return gdSlotNearestAt(slot, mode, uv, 0);
    uv = gdSlotUv(mode, uv, sz);
    GD_SLOT_SWITCH(SampleGrad(gMaterialSampler, uv, dx, dy))
}
float2 gdSlotQueryLod(uint slot, float2 uv) { const float l = gdSlotCalcLod(slot, uv); return float2(l, l); }
float4 gdSlotGather(uint slot, float2 uv, int comp) {
    if (comp == 1) { GD_SLOT_SWITCH(GatherGreen(gMaterialSampler, uv)) }
    if (comp == 2) { GD_SLOT_SWITCH(GatherBlue(gMaterialSampler, uv)) }
    if (comp == 3) { GD_SLOT_SWITCH(GatherAlpha(gMaterialSampler, uv)) }
    GD_SLOT_SWITCH(GatherRed(gMaterialSampler, uv))
}

// The ray flavour: the same contract over the bindless table (voxi_rt.hlsli). Without RT_BINDLESS nothing calls these.
#ifdef AVER_RT_BINDLESS
float4 averRtGdSample(uint slot, float2 uv);
float4 averRtGdSampleLod(uint slot, float2 uv, float lod);
float4 averRtGdLoad(uint slot, int2 p, int mip);
int2   averRtGdSize(uint slot);
bool   averRtGdBound(uint slot);
#else
float4 averRtGdSample(uint slot, float2 uv) { return float4(0.0, 0.0, 0.0, 0.0); }
float4 averRtGdSampleLod(uint slot, float2 uv, float lod) { return float4(0.0, 0.0, 0.0, 0.0); }
float4 averRtGdLoad(uint slot, int2 p, int mip) { return float4(0.0, 0.0, 0.0, 0.0); }
int2   averRtGdSize(uint slot) { return int2(1, 1); }
bool   averRtGdBound(uint slot) { return false; }
#endif
float4 gdrSlotNearestAt(uint slot, uint mode, float2 uv) {
    const int2 sz = averRtGdSize(slot);
    int2 p = int2(floor(uv * float2(sz)));
    p = (mode & 2u) != 0u ? ((p % sz) + sz) % sz : clamp(p, int2(0, 0), sz - 1);
    return averRtGdLoad(slot, p, 0);
}
float4 gdrSlotSample(uint slot, uint mode, float2 uv, float bias) {
    if (!averRtGdBound(slot)) return gdSlotDefault(mode);
    if ((mode & 1u) != 0u) return gdrSlotNearestAt(slot, mode, uv);
    return averRtGdSample(slot, gdSlotUv(mode, uv, averRtGdSize(slot)));
}
float4 gdrSlotSampleLod(uint slot, uint mode, float2 uv, float lod) {
    if (!averRtGdBound(slot)) return gdSlotDefault(mode);
    if ((mode & 1u) != 0u) return gdrSlotNearestAt(slot, mode, uv);
    return averRtGdSampleLod(slot, gdSlotUv(mode, uv, averRtGdSize(slot)), lod);
}
float4 gdrSlotSampleGrad(uint slot, uint mode, float2 uv, float2 dx, float2 dy) {
    const float2 sz = float2(averRtGdSize(slot));
    const float2 a = dx * sz, b = dy * sz;
    return gdrSlotSampleLod(slot, mode, uv, 0.5 * log2(max(max(dot(a, a), dot(b, b)), 1.0e-8)));
}
float2 gdrSlotQueryLod(uint slot, float2 uv) { return float2(0.0, 0.0); }
float4 gdrSlotFetch(uint slot, int2 p, int lod) { return averRtGdLoad(slot, p, lod); }
int2   gdrSlotSize(uint slot, int lod) { return averRtGdSize(slot) >> lod; }
int    gdrSlotLevels(uint slot) { return 1; }
float4 gdrSlotGather(uint slot, float2 uv, int comp) { return gdrSlotSample(slot, 0u, uv, 0.0).xxxx; }

// ray flavour derivatives: a hit has no screen-space neighbours, so the derivative built-ins read zero.
#define GDR_DERIV_FN(NAME) \
    float  NAME(float x)  { return 0.0; } \
    float2 NAME(float2 x) { return float2(0.0, 0.0); } \
    float3 NAME(float3 x) { return float3(0.0, 0.0, 0.0); } \
    float4 NAME(float4 x) { return float4(0.0, 0.0, 0.0, 0.0); }
GDR_DERIV_FN(gdrDdx)
GDR_DERIV_FN(gdrDdy)
GDR_DERIV_FN(gdrDdxCoarse)
GDR_DERIV_FN(gdrDdyCoarse)
GDR_DERIV_FN(gdrDdxFine)
GDR_DERIV_FN(gdrDdyFine)
GDR_DERIV_FN(gdrFwidth)

// ---- the uniform table (material slot 7, RGBA32F, one register per texel) -------------------------------------------------------------------------------
float4 gdLoadReg(uint i) { return gdSlotLoadReg((int)i); }
float4 gdrLoadReg(uint i) { return averRtGdLoad(7u, int2((int)i, 0), 0); }

// ---- fragment(): Aver -> Godot -------------------------------------------------------------------------------------------------------------------------
void gdWorldFrame(float3 N, out float3 T, out float3 B) {
    const float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}
void gdFragmentCommon(AverVertex v, bool deriv) {
    gdCamInit();
    GdCtx c = (GdCtx)0;
    c.vertex = gdWorldPosToView(v.wpos);
    c.normal = gdDirToView(v.N);
    float3 T, B;
    gdWorldFrame(v.N, T, B);
    c.uv = v.uv;
    c.uv2 = float2(0.0, 0.0);
    c.color = float4(1.0, 1.0, 1.0, 1.0);
#if AVER_MESH_EXTRA
    if (v.tangentWS.w != 0.0) {
        const float3 geoN = v.backFace ? -v.N : v.N;
        float3 t = v.tangentWS.xyz - geoN * dot(v.tangentWS.xyz, geoN);
        if (dot(t, t) > 1e-12) { T = normalize(t); B = cross(geoN, T) * v.tangentWS.w; }
    }
    c.uv2 = v.uv2;
    c.color = v.color;
#endif
    c.tangent = gdDirToView(T);
    c.binormal = gdDirToView(B);
    c.frontFacing = !v.backFace;
    c.view = normalize(-c.vertex);
    c.lightVertex = c.vertex;
    const float4 clip = mul(float4(v.wpos, 1.0), gViewProj);
    const float2 ndc = clip.xy / max(abs(clip.w), 1.0e-6);
    c.screenUv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    c.fragcoord = float4(c.screenUv * float2(AVER_GD_VIEWPORT_W, AVER_GD_VIEWPORT_H), clip.z / max(abs(clip.w), 1.0e-6), 1.0 / max(abs(clip.w), 1.0e-6));
    c.pointCoord = float2(0.5, 0.5);
    // Godot's defaults for everything fragment() may write.
    c.albedo = float3(1.0, 1.0, 1.0);
    c.alpha = 1.0;
    c.alphaScissor = 0.0;
    c.alphaHashScale = 1.0;
    c.alphaAAEdge = 0.3;
    c.premulFactor = 0.0;
    c.metallic = 0.0;
    c.roughness = 1.0;
    c.specular = 0.5;
    c.ao = 1.0;
    c.normalMap = float3(0.5, 0.5, 1.0);
    c.normalMapDepth = 1.0;
    c.clearcoatGloss = 0.5;
    c.sssColor = float4(1.0, 1.0, 1.0, 1.0);
    c.normalIn = c.normal;
    gGd = c;
}
void gdFragmentBegin(AverVertex v, float2 uv) {
    gdFragmentCommon(v, true);
#ifndef AVER_GD_NO_DERIVATIVES
    // The tangent frame the engine's own normal mapping uses: screen-space derivatives of position and UV.
    const float3 dp1 = ddx(v.wpos), dp2 = ddy(v.wpos);
    const float2 du1 = ddx(v.uv), du2 = ddy(v.uv);
    const float3 dp2perp = cross(dp2, v.N), dp1perp = cross(v.N, dp1);
    const float3 Td = dp2perp * du1.x + dp1perp * du2.x, Bd = dp2perp * du1.y + dp1perp * du2.y;
    if (max(dot(Td, Td), dot(Bd, Bd)) > 0.0) {
        bool authored = false;
#if AVER_MESH_EXTRA
        authored = v.tangentWS.w != 0.0;
#endif
        if (!authored) { gGd.tangent = gdDirToView(normalize(Td)); gGd.binormal = gdDirToView(normalize(Bd)); }
    }
#endif
}
void gdrFragmentBegin(AverVertex v, float2 uv) { gdFragmentCommon(v, false); }

// A world-space normal in the tangent frame averPerturbNormal builds, so averBuildSurface reproduces it (an approximation: the frames are rebuilt from
// the same derivatives, orthonormalised).
float3 gdWorldNormalToTS(AverVertex v, float3 Nw) {
    const float3 T = normalize(gdViewDirToWorld(gGd.tangent));
    const float3 B = normalize(gdViewDirToWorld(gGd.binormal));
    return normalize(float3(dot(Nw, T), dot(Nw, B), dot(Nw, v.N)));
}

// fragment() -> AverAuthored.
void gdFragmentEnd(inout AverAuthored a, AverVertex v, uint fx) {
    a.baseColor = max(gGd.albedo, 0.0);
    a.opacity = saturate(gGd.alpha);
    a.metallic = saturate(gGd.metallic);
    a.roughness = saturate(gGd.roughness);
    a.emissive = max(gGd.emission, 0.0);
    a.occlusion = saturate(gGd.ao);
    a.alphaCutoff = gGd.alphaScissor;
    a.normalLen = 1.0;
    a.normalTS = float3(0.0, 0.0, 1.0);
    if (any(abs(gGd.normalMap - float3(0.5, 0.5, 1.0)) > 1.0e-5)) {
        const float3 t = gGd.normalMap * 2.0 - 1.0;
        a.normalTS = normalize(float3(t.xy * gGd.normalMapDepth, t.z));
    } else if (any(abs(gGd.normal - gGd.normalIn) > 1.0e-6)) {
        a.normalTS = gdWorldNormalToTS(v, normalize(gdViewDirToWorld(gGd.normal)));
    }
    // clearcoat -> the layered BSDF's coat (live with RENDER.LAYEREDBSDF), subsurface -> the wrap / transmission terms.
    a.coatWeight = saturate(gGd.clearcoat);
    a.coatRoughness = saturate(1.0 - gGd.clearcoatGloss);
    a.coatF0 = 0.04;
    a.subsurfaceWeight = saturate(gGd.sssStrength);
    a.subsurfaceRadius = saturate(gGd.sssDepth * 0.25);
    a.subsurfaceColor = max(gGd.sssColor.rgb, 0.0);
    // The anisotropy lobe is widened, as the BaseMaterial3D fx does.
    a.roughness = saturate(a.roughness * (1.0 + 0.5 * abs(gGd.anisotropy)));
}

// What needs the finished surface: unshaded, the sun's rim and backlight terms, SPECULAR, ambient and specular modes (the fx evaluator's formulas).
void gdPost(inout AverSurface s, AverVertex v, AverLight l, uint fx) {
    s.reflectance = 0.08 * gGd.specular;
    s.F0 = lerp(s.reflectance.xxx, s.albedo, s.metallic);
    s.F = fresnelSchlick(saturate(dot(s.H, v.V)), s.F0, s.f90);
    if ((fx & 4u) != 0u) {   // specular_disabled
        s.F0 = float3(0.0, 0.0, 0.0); s.F = float3(0.0, 0.0, 0.0); s.f90 = 0.0; s.reflectance = 0.0;
    }
    if ((fx & 8u) != 0u) { s.sssWeight = max(s.sssWeight, 0.5); s.sssRadius = max(s.sssRadius, 0.25); s.sssColor = float3(1.0, 1.0, 1.0); }   // diffuse_lambert_wrap
    if ((fx & 2u) != 0u) s.occlusion = 0.0;   // ambient_light_disabled
    {
        const float3 sunE = l.radiance * l.visibility * (1.0 / PI);
        float3 add = float3(0.0, 0.0, 0.0);
        if (gGd.rim > 0.0) {
            const float rimLight = pow(max(0.0, 1.0 - s.ndv), max(0.0, (1.0 - s.rough) * 16.0));
            add += rimLight * gGd.rim * lerp(float3(1.0, 1.0, 1.0), s.albedo, gGd.rimTint) * sunE;
        }
        if (any(gGd.backlight > 0.0)) add += gGd.backlight * s.albedo * saturate(dot(-s.N, l.direction)) * sunE;
        s.emissive += add;
    }
    if ((fx & 1u) != 0u) {   // unshaded
        s.display = true;
        s.displayColor = float4(s.albedo + s.emissive, s.alpha);
        if (gMaterialFlags & AVER_MAT_ALPHA_BLEND) s.displayColor.rgb *= s.displayColor.a;
    }
}

// ---- vertex(): Godot -> Aver and back --------------------------------------------------------------------------------------------------------------------
void gdVertexBegin(VSIn i, VSOut o, uint fx) {
    gdCamInit();
    GdCtx c = (GdCtx)0;
    if ((fx & 16u) != 0u) {   // world_vertex_coords: VERTEX and NORMAL are in world space
        c.vertex = gdE2G(o.wpos) * 0.01;
        c.normal = gdE2G(normalize(o.nrmWS));
    } else {
        c.vertex = gdE2G(i.pos) * 0.01;
        c.normal = gdE2G(normalize(i.nrm));
    }
    c.uv = i.uv;
    c.uv2 = float2(0.0, 0.0);
    c.color = float4(1.0, 1.0, 1.0, 1.0);
#if AVER_MESH_EXTRA
    c.uv2 = i.uv2;
    c.color = i.col;
    if (abs(i.tng.w * 2.0 - 1.0) > 0.5) {
        c.tangent = gdE2G(i.tng.xyz * 2.0 - 1.0);
        c.binormal = cross(c.normal, c.tangent) * ((i.tng.w * 2.0 - 1.0) > 0.0 ? 1.0 : -1.0);
    }
#endif
    const float4x4 mv = mul(gdModelMatrix(), gdViewMatrix());   // GLSL view * model, in the emitter's memory order (rows hold columns)
    c.modelview = mv;
    c.modelviewNormal = float3x3(normalize(mv[0].xyz), normalize(mv[1].xyz), normalize(mv[2].xyz));
    c.projection = gdIdentity4();
    c.zClipScale = 1.0;
    gGd = c;
}
void gdVertexEnd(VSIn i, inout VSOut o, uint fx) {
    float3 wpos, nrm;
    if ((fx & 32u) != 0u) {   // skip_vertex_transform: the shader left VERTEX and NORMAL in view space
        wpos = gdViewPosToWorld(gGd.vertex);
        nrm = gdViewDirToWorld(gGd.normal);
    } else if ((fx & 16u) != 0u) {
        wpos = gdG2E(gGd.vertex) * 100.0;
        nrm = gdG2E(gGd.normal);
    } else {
        const float3 local = gdG2E(gGd.vertex) * 100.0;
        wpos = mul(float4(local, 1.0), gWorld).xyz;
        nrm = averTransformNormal(gdG2E(gGd.normal), gWorld);
    }
    o.wpos = wpos;
    o.nrmWS = normalize(nrm);
    o.pos = mul(float4(wpos, 1.0), gViewProj);
    o.uv = gGd.uv;
#if AVER_MESH_EXTRA
    o.vcol = gGd.color;
    o.uv2 = gGd.uv2;
#endif
}
