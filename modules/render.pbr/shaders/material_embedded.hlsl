// Embedded HLSL spatial shaders on Aver materials.
//
// Added to the translation unit by pbr::materialGraphHlsl() ONLY when a registered material graph carries an EmbeddedHlsl node, after material_prelude.hlsl
// and before the embedded shaders (a tool translates them against these hooks, in slot-sampler mode). It defines
//   - the AVER_CUSTOM_* hook macros the emitted code reads and writes (one context struct, gEmb, shared by the vertex and fragment functions),
//   - the source <-> Aver basis change (source: right-handed, +Y up, -Z forward, metres; Aver: left-handed, +Z up, +X forward, centimetres:
//     aver::fmt::axis, AxisMap.hpp) so the shader sees view space, model space and world space as its own language defines them,
//   - the uniform table (a RGBA32F texture in material slot 7) and the texture slots with the source's filter / repeat hints emulated on the one shared sampler,
//   - the fragment() -> AverAuthored mapping and the vertex() -> VSOut mapping.
// Two flavours of everything that samples or differentiates: the raster flavour (embSlot*, uses implicit derivatives) and the ray flavour (embrSlot*, reads the
// bindless table through averRtEmb*, no derivative anywhere, so a compute pass can compile it). The embedded source is included once per flavour with the
// name macro EMB_NS_CUR and the emb_/ddx names redirected (MaterialGraphHlsl.cpp).

#ifndef AVER_CUSTOM_MAX_U
#define AVER_CUSTOM_MAX_U 1
#endif
#ifndef AVER_CUSTOM_MAX_I
#define AVER_CUSTOM_MAX_I 1
#endif
#ifndef AVER_CUSTOM_MAX_G
#define AVER_CUSTOM_MAX_G 1
#endif

#define EMB_CAT2(a, b) a##b
#define EMB_CAT(a, b) EMB_CAT2(a, b)

// ---- the basis change ---------------------------------------------------------------------------------------------------------------------------
// engine = (-s.z, s.x, s.y) for a direction; positions also scale by 100 (cm).
float3 embE2S(float3 e) { return float3(e.y, e.z, -e.x); }
float3 embS2E(float3 s) { return float3(-s.z, s.x, s.y); }

static float3 gEmbCamR = float3(0.0, 1.0, 0.0);
static float3 gEmbCamU = float3(0.0, 0.0, 1.0);
static float3 gEmbCamF = float3(1.0, 0.0, 0.0);
void embCamInit() {
    const float3 f = averViewRayDir(float2(0.0, 0.0));
    float3 r = averViewRayDir(float2(1.0, 0.0)) - averViewRayDir(float2(-1.0, 0.0));
    float3 u = averViewRayDir(float2(0.0, 1.0)) - averViewRayDir(float2(0.0, -1.0));
    r = normalize(r - f * dot(r, f));
    u = normalize(u - f * dot(u, f) - r * dot(u, r));
    gEmbCamF = f; gEmbCamR = r; gEmbCamU = u;
}
// Engine world (cm) -> source view space (m; x right, y up, -z forward).
float3 embWorldPosToView(float3 p) { const float3 d = (p - gCamPos.xyz) * 0.01; return float3(dot(d, gEmbCamR), dot(d, gEmbCamU), -dot(d, gEmbCamF)); }
float3 embDirToView(float3 d) { return float3(dot(d, gEmbCamR), dot(d, gEmbCamU), -dot(d, gEmbCamF)); }
float3 embViewDirToWorld(float3 v) { return gEmbCamR * v.x + gEmbCamU * v.y - gEmbCamF * v.z; }
float3 embViewPosToWorld(float3 v) { return gCamPos.xyz + embViewDirToWorld(v) * 100.0; }

// GLSL matrices keep their memory order: an HLSL float4x4 holds GLSL column i in row i (the emitter's contract), so these build rows from columns.
float4x4 embViewMatrix() {
    const float3 R = embE2S(gEmbCamR), U = embE2S(gEmbCamU), B = embE2S(-gEmbCamF), c = embE2S(gCamPos.xyz) * 0.01;
    return float4x4(float4(R.x, U.x, B.x, 0.0), float4(R.y, U.y, B.y, 0.0), float4(R.z, U.z, B.z, 0.0), float4(-dot(R, c), -dot(U, c), -dot(B, c), 1.0));
}
float4x4 embInvViewMatrix() {
    const float3 R = embE2S(gEmbCamR), U = embE2S(gEmbCamU), B = embE2S(-gEmbCamF), c = embE2S(gCamPos.xyz) * 0.01;
    return float4x4(float4(R, 0.0), float4(U, 0.0), float4(B, 0.0), float4(c, 1.0));
}
float3 embModelAxis(float3 srcAxis) { return embE2S(mul(float4(embS2E(srcAxis), 0.0), gWorld).xyz); }
float4x4 embModelMatrix() {
    return float4x4(float4(embModelAxis(float3(1.0, 0.0, 0.0)), 0.0), float4(embModelAxis(float3(0.0, 1.0, 0.0)), 0.0), float4(embModelAxis(float3(0.0, 0.0, 1.0)), 0.0),
                    float4(embE2S(gWorld[3].xyz) * 0.01, 1.0));
}
float3x3 embModelNormalMatrix() {
    return float3x3(normalize(embModelAxis(float3(1.0, 0.0, 0.0))), normalize(embModelAxis(float3(0.0, 1.0, 0.0))), normalize(embModelAxis(float3(0.0, 0.0, 1.0))));
}
float4x4 embIdentity4() { return float4x4(1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 1.0); }

// ---- the context ----------------------------------------------------------------------------------------------------------------------------------
struct EmbCtx {
    // inputs / inouts of both stages (vertex: model or world space, source axes; fragment: view space)
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
static EmbCtx gEmb;
static uint  gEmbFx = 0u;      // hlslfx bits of the shader the arm just ran (read by embPost)

#define AVER_CUSTOM_VIEWPORT_W 1920.0
#define AVER_CUSTOM_VIEWPORT_H 1080.0

// Built-in variables as macros (the names are AVER_CUSTOM_ + the source's built-in name).
#define AVER_CUSTOM_TIME gTime.x
#define AVER_CUSTOM_OUTPUT_IS_SRGB false
#define AVER_CUSTOM_VIEWPORT_SIZE float2(AVER_CUSTOM_VIEWPORT_W, AVER_CUSTOM_VIEWPORT_H)
#define AVER_CUSTOM_VIEW_MATRIX embViewMatrix()
#define AVER_CUSTOM_INV_VIEW_MATRIX embInvViewMatrix()
#define AVER_CUSTOM_MAIN_CAM_INV_VIEW_MATRIX embInvViewMatrix()
#define AVER_CUSTOM_INV_PROJECTION_MATRIX embIdentity4()
#define AVER_CUSTOM_PROJECTION_MATRIX gEmb.projection
#define AVER_CUSTOM_NODE_POSITION_WORLD (embE2S(gWorld[3].xyz) * 0.01)
#define AVER_CUSTOM_NODE_POSITION_VIEW embWorldPosToView(gWorld[3].xyz)
#define AVER_CUSTOM_CAMERA_POSITION_WORLD (embE2S(gCamPos.xyz) * 0.01)
#define AVER_CUSTOM_CAMERA_DIRECTION_WORLD embE2S(gEmbCamF)
#define AVER_CUSTOM_CAMERA_VISIBLE_LAYERS 0xFFFFFu
#define AVER_CUSTOM_INSTANCE_ID 0
#define AVER_CUSTOM_INSTANCE_CUSTOM float4(0.0, 0.0, 0.0, 0.0)
#define AVER_CUSTOM_VIEW_INDEX 0
#define AVER_CUSTOM_VIEW_MONO_LEFT 0
#define AVER_CUSTOM_VIEW_RIGHT 0
#define AVER_CUSTOM_EYE_OFFSET float3(0.0, 0.0, 0.0)
#define AVER_CUSTOM_VERTEX gEmb.vertex
#define AVER_CUSTOM_VERTEX_ID 0
#define AVER_CUSTOM_POSITION gEmb.position
#define AVER_CUSTOM_ROUGHNESS gEmb.roughness
#define AVER_CUSTOM_POINT_SIZE gEmb.pointSize
#define AVER_CUSTOM_MODELVIEW_MATRIX gEmb.modelview
#define AVER_CUSTOM_MODELVIEW_NORMAL_MATRIX gEmb.modelviewNormal
#define AVER_CUSTOM_MODEL_MATRIX embModelMatrix()
#define AVER_CUSTOM_MODEL_NORMAL_MATRIX embModelNormalMatrix()
#define AVER_CUSTOM_BONE_INDICES uint4(0u, 0u, 0u, 0u)
#define AVER_CUSTOM_BONE_WEIGHTS float4(0.0, 0.0, 0.0, 0.0)
#define AVER_CUSTOM_CUSTOM0 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_CUSTOM_CUSTOM1 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_CUSTOM_CUSTOM2 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_CUSTOM_CUSTOM3 float4(0.0, 0.0, 0.0, 0.0)
#define AVER_CUSTOM_Z_CLIP_SCALE gEmb.zClipScale
#define AVER_CUSTOM_NORMAL gEmb.normal
#define AVER_CUSTOM_TANGENT gEmb.tangent
#define AVER_CUSTOM_BINORMAL gEmb.binormal
#define AVER_CUSTOM_UV gEmb.uv
#define AVER_CUSTOM_UV2 gEmb.uv2
#define AVER_CUSTOM_COLOR gEmb.color
#define AVER_CUSTOM_FRAGCOORD gEmb.fragcoord
#define AVER_CUSTOM_FRONT_FACING gEmb.frontFacing
#define AVER_CUSTOM_VIEW gEmb.view
#define AVER_CUSTOM_POINT_COORD gEmb.pointCoord
#define AVER_CUSTOM_LIGHT_VERTEX gEmb.lightVertex
#define AVER_CUSTOM_SCREEN_UV gEmb.screenUv
#define AVER_CUSTOM_DEPTH gEmb.depth
#define AVER_CUSTOM_NORMAL_MAP gEmb.normalMap
#define AVER_CUSTOM_NORMAL_MAP_DEPTH gEmb.normalMapDepth
#define AVER_CUSTOM_BENT_NORMAL_MAP gEmb.bentNormalMap
#define AVER_CUSTOM_ALBEDO gEmb.albedo
#define AVER_CUSTOM_ALPHA gEmb.alpha
#define AVER_CUSTOM_ALPHA_SCISSOR_THRESHOLD gEmb.alphaScissor
#define AVER_CUSTOM_ALPHA_HASH_SCALE gEmb.alphaHashScale
#define AVER_CUSTOM_ALPHA_ANTIALIASING_EDGE gEmb.alphaAAEdge
#define AVER_CUSTOM_ALPHA_TEXTURE_COORDINATE gEmb.alphaTexCoord
#define AVER_CUSTOM_PREMUL_ALPHA_FACTOR gEmb.premulFactor
#define AVER_CUSTOM_METALLIC gEmb.metallic
#define AVER_CUSTOM_SPECULAR gEmb.specular
#define AVER_CUSTOM_RIM gEmb.rim
#define AVER_CUSTOM_RIM_TINT gEmb.rimTint
#define AVER_CUSTOM_CLEARCOAT gEmb.clearcoat
#define AVER_CUSTOM_CLEARCOAT_GLOSS gEmb.clearcoatGloss
#define AVER_CUSTOM_ANISOTROPY gEmb.anisotropy
#define AVER_CUSTOM_ANISOTROPY_FLOW gEmb.anisotropyFlow
#define AVER_CUSTOM_SSS_STRENGTH gEmb.sssStrength
#define AVER_CUSTOM_SSS_TRANSMITTANCE_COLOR gEmb.sssColor
#define AVER_CUSTOM_SSS_TRANSMITTANCE_DEPTH gEmb.sssDepth
#define AVER_CUSTOM_SSS_TRANSMITTANCE_BOOST gEmb.sssBoost
#define AVER_CUSTOM_BACKLIGHT gEmb.backlight
#define AVER_CUSTOM_AO gEmb.ao
#define AVER_CUSTOM_AO_LIGHT_AFFECT gEmb.aoLightAffect
#define AVER_CUSTOM_EMISSION gEmb.emission
#define AVER_CUSTOM_FOG gEmb.fog
#define AVER_CUSTOM_RADIANCE gEmb.radiance
#define AVER_CUSTOM_IRRADIANCE gEmb.irradiance

// The uniform blocks, as register arrays the emitted code indexes. Filled by the arm that runs the shader.
static float4 gEmbU[AVER_CUSTOM_MAX_U];
static float4 gEmbI[AVER_CUSTOM_MAX_I];
static float4 gEmbG[AVER_CUSTOM_MAX_G];
#define AVER_CUSTOM_UBUF gEmbU
#define AVER_CUSTOM_IBUF gEmbI
#define AVER_CUSTOM_GBUF gEmbG

// ---- texture slots ---------------------------------------------------------------------------------------------------------------------------------
// Slot s is pbr::TextureSlot s. The mode word of a sampler value: bit 0 nearest, 1 repeat, 2 mipmaps, bits 8-9 the colour of an unbound texture
// (0 white, 1 black, 2 transparent, 3 flat normal).
#define EMB_SLOT_SWITCH(CALL) \
    switch (slot) { \
    case 0u: return gBaseColorMap.CALL; case 1u: return gMetalRoughMap.CALL; case 2u: return gNormalMap.CALL; case 3u: return gOcclusionMap.CALL; \
    case 4u: return gEmissiveMap.CALL; case 5u: return gL1BaseColorMap.CALL; case 6u: return gL1MetalRoughMap.CALL; default: return gL1NormalMap.CALL; }

bool embSlotBound(uint slot) {
    const uint bit = slot < 5u ? slot : slot + 4u;   // MaterialFlag bits: 0-4 the five maps, 9-11 the layer-1 maps
    return ((gMaterialFlags >> bit) & 1u) != 0u;
}
float4 embSlotDefault(uint mode) {
    const uint c = (mode >> 8) & 3u;
    return c == 1u ? float4(0.0, 0.0, 0.0, 1.0) : (c == 2u ? float4(0.0, 0.0, 0.0, 0.0) : (c == 3u ? float4(0.5, 0.5, 1.0, 1.0) : float4(1.0, 1.0, 1.0, 1.0)));
}
int2 embSlotSize(uint slot, int lod) {
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
int embSlotLevels(uint slot) {
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
float4 embSlotFetch(uint slot, int2 p, int lod) { EMB_SLOT_SWITCH(Load(int3(p, lod))) }
float4 embSlotLoadReg(int i) { return gL1NormalMap.Load(int3(i, 0, 0)); }

// repeat_disable: clamp to the edge texels (the engine's sampler wraps).
float2 embSlotUv(uint mode, float2 uv, int2 sz) {
    if ((mode & 2u) != 0u) return uv;
    const float2 h = 0.5 / float2(sz);
    return clamp(uv, h, 1.0 - h);
}
float4 embSlotNearestAt(uint slot, uint mode, float2 uv, int level) {
    const int2 sz = embSlotSize(slot, level);
    int2 p = int2(floor(uv * float2(sz)));
    p = (mode & 2u) != 0u ? ((p % sz) + sz) % sz : clamp(p, int2(0, 0), sz - 1);
    return embSlotFetch(slot, p, level);
}
#ifndef AVER_CUSTOM_NO_DERIVATIVES
float embSlotCalcLod(uint slot, float2 uv) {
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
float embSlotCalcLod(uint slot, float2 uv) { return 0.0; }
#endif

float4 embSlotSample(uint slot, uint mode, float2 uv, float bias) {
    if (!embSlotBound(slot)) return embSlotDefault(mode);
    const int2 sz = embSlotSize(slot, 0);
    if ((mode & 1u) != 0u) {
        const int level = (mode & 4u) != 0u ? clamp((int)round(embSlotCalcLod(slot, uv)), 0, embSlotLevels(slot) - 1) : 0;
        return embSlotNearestAt(slot, mode, uv, level);
    }
    uv = embSlotUv(mode, uv, sz);
#ifdef AVER_CUSTOM_NO_DERIVATIVES
    EMB_SLOT_SWITCH(SampleLevel(gMaterialSampler, uv, 0.0))
#else
    if (bias != 0.0) { EMB_SLOT_SWITCH(SampleBias(gMaterialSampler, uv, bias)) }
    EMB_SLOT_SWITCH(Sample(gMaterialSampler, uv))
#endif
}
float4 embSlotSampleLod(uint slot, uint mode, float2 uv, float lod) {
    if (!embSlotBound(slot)) return embSlotDefault(mode);
    const int2 sz = embSlotSize(slot, 0);
    if ((mode & 1u) != 0u) return embSlotNearestAt(slot, mode, uv, (mode & 4u) != 0u ? clamp((int)round(lod), 0, embSlotLevels(slot) - 1) : 0);
    uv = embSlotUv(mode, uv, sz);
    EMB_SLOT_SWITCH(SampleLevel(gMaterialSampler, uv, lod))
}
float4 embSlotSampleGrad(uint slot, uint mode, float2 uv, float2 dx, float2 dy) {
    if (!embSlotBound(slot)) return embSlotDefault(mode);
    const int2 sz = embSlotSize(slot, 0);
    if ((mode & 1u) != 0u) return embSlotNearestAt(slot, mode, uv, 0);
    uv = embSlotUv(mode, uv, sz);
    EMB_SLOT_SWITCH(SampleGrad(gMaterialSampler, uv, dx, dy))
}
float2 embSlotQueryLod(uint slot, float2 uv) { const float l = embSlotCalcLod(slot, uv); return float2(l, l); }
float4 embSlotGather(uint slot, float2 uv, int comp) {
    if (comp == 1) { EMB_SLOT_SWITCH(GatherGreen(gMaterialSampler, uv)) }
    if (comp == 2) { EMB_SLOT_SWITCH(GatherBlue(gMaterialSampler, uv)) }
    if (comp == 3) { EMB_SLOT_SWITCH(GatherAlpha(gMaterialSampler, uv)) }
    EMB_SLOT_SWITCH(GatherRed(gMaterialSampler, uv))
}

// The ray flavour: the same contract over the bindless table (voxi_rt.hlsli). Without RT_BINDLESS nothing calls these.
#ifdef AVER_RT_BINDLESS
float4 averRtEmbSample(uint slot, float2 uv);
float4 averRtEmbSampleLod(uint slot, float2 uv, float lod);
float4 averRtEmbLoad(uint slot, int2 p, int mip);
int2   averRtEmbSize(uint slot);
bool   averRtEmbBound(uint slot);
#else
float4 averRtEmbSample(uint slot, float2 uv) { return float4(0.0, 0.0, 0.0, 0.0); }
float4 averRtEmbSampleLod(uint slot, float2 uv, float lod) { return float4(0.0, 0.0, 0.0, 0.0); }
float4 averRtEmbLoad(uint slot, int2 p, int mip) { return float4(0.0, 0.0, 0.0, 0.0); }
int2   averRtEmbSize(uint slot) { return int2(1, 1); }
bool   averRtEmbBound(uint slot) { return false; }
#endif
float4 embrSlotNearestAt(uint slot, uint mode, float2 uv) {
    const int2 sz = averRtEmbSize(slot);
    int2 p = int2(floor(uv * float2(sz)));
    p = (mode & 2u) != 0u ? ((p % sz) + sz) % sz : clamp(p, int2(0, 0), sz - 1);
    return averRtEmbLoad(slot, p, 0);
}
float4 embrSlotSample(uint slot, uint mode, float2 uv, float bias) {
    if (!averRtEmbBound(slot)) return embSlotDefault(mode);
    if ((mode & 1u) != 0u) return embrSlotNearestAt(slot, mode, uv);
    return averRtEmbSample(slot, embSlotUv(mode, uv, averRtEmbSize(slot)));
}
float4 embrSlotSampleLod(uint slot, uint mode, float2 uv, float lod) {
    if (!averRtEmbBound(slot)) return embSlotDefault(mode);
    if ((mode & 1u) != 0u) return embrSlotNearestAt(slot, mode, uv);
    return averRtEmbSampleLod(slot, embSlotUv(mode, uv, averRtEmbSize(slot)), lod);
}
float4 embrSlotSampleGrad(uint slot, uint mode, float2 uv, float2 dx, float2 dy) {
    const float2 sz = float2(averRtEmbSize(slot));
    const float2 a = dx * sz, b = dy * sz;
    return embrSlotSampleLod(slot, mode, uv, 0.5 * log2(max(max(dot(a, a), dot(b, b)), 1.0e-8)));
}
float2 embrSlotQueryLod(uint slot, float2 uv) { return float2(0.0, 0.0); }
float4 embrSlotFetch(uint slot, int2 p, int lod) { return averRtEmbLoad(slot, p, lod); }
int2   embrSlotSize(uint slot, int lod) { return averRtEmbSize(slot) >> lod; }
int    embrSlotLevels(uint slot) { return 1; }
float4 embrSlotGather(uint slot, float2 uv, int comp) { return embrSlotSample(slot, 0u, uv, 0.0).xxxx; }

// ray flavour derivatives: a hit has no screen-space neighbours, so the derivative built-ins read zero.
#define EMBR_DERIV_FN(NAME) \
    float  NAME(float x)  { return 0.0; } \
    float2 NAME(float2 x) { return float2(0.0, 0.0); } \
    float3 NAME(float3 x) { return float3(0.0, 0.0, 0.0); } \
    float4 NAME(float4 x) { return float4(0.0, 0.0, 0.0, 0.0); }
EMBR_DERIV_FN(embrDdx)
EMBR_DERIV_FN(embrDdy)
EMBR_DERIV_FN(embrDdxCoarse)
EMBR_DERIV_FN(embrDdyCoarse)
EMBR_DERIV_FN(embrDdxFine)
EMBR_DERIV_FN(embrDdyFine)
EMBR_DERIV_FN(embrFwidth)

// ---- the uniform table (material slot 7, RGBA32F, one register per texel) -------------------------------------------------------------------------------
float4 embLoadReg(uint i) { return embSlotLoadReg((int)i); }
float4 embrLoadReg(uint i) { return averRtEmbLoad(7u, int2((int)i, 0), 0); }

// ---- fragment(): Aver -> source ------------------------------------------------------------------------------------------------------------------------
void embWorldFrame(float3 N, out float3 T, out float3 B) {
    const float3 up = abs(N.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    T = normalize(cross(up, N));
    B = cross(N, T);
}
void embFragmentCommon(AverVertex v, bool deriv) {
    embCamInit();
    EmbCtx c = (EmbCtx)0;
    c.vertex = embWorldPosToView(v.wpos);
    c.normal = embDirToView(v.N);
    float3 T, B;
    embWorldFrame(v.N, T, B);
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
    c.tangent = embDirToView(T);
    c.binormal = embDirToView(B);
    c.frontFacing = !v.backFace;
    c.view = normalize(-c.vertex);
    c.lightVertex = c.vertex;
    const float4 clip = mul(float4(v.wpos, 1.0), gViewProj);
    const float2 ndc = clip.xy / max(abs(clip.w), 1.0e-6);
    c.screenUv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    c.fragcoord = float4(c.screenUv * float2(AVER_CUSTOM_VIEWPORT_W, AVER_CUSTOM_VIEWPORT_H), clip.z / max(abs(clip.w), 1.0e-6), 1.0 / max(abs(clip.w), 1.0e-6));
    c.pointCoord = float2(0.5, 0.5);
    // The source's defaults for everything fragment() may write.
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
    gEmb = c;
}
void embFragmentBegin(AverVertex v, float2 uv) {
    embFragmentCommon(v, true);
#ifndef AVER_CUSTOM_NO_DERIVATIVES
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
        if (!authored) { gEmb.tangent = embDirToView(normalize(Td)); gEmb.binormal = embDirToView(normalize(Bd)); }
    }
#endif
}
void embrFragmentBegin(AverVertex v, float2 uv) { embFragmentCommon(v, false); }

// A world-space normal in the tangent frame averPerturbNormal builds, so averBuildSurface reproduces it (an approximation: the frames are rebuilt from
// the same derivatives, orthonormalised).
float3 embWorldNormalToTS(AverVertex v, float3 Nw) {
    const float3 T = normalize(embViewDirToWorld(gEmb.tangent));
    const float3 B = normalize(embViewDirToWorld(gEmb.binormal));
    return normalize(float3(dot(Nw, T), dot(Nw, B), dot(Nw, v.N)));
}

// fragment() -> AverAuthored.
void embFragmentEnd(inout AverAuthored a, AverVertex v, uint fx) {
    a.baseColor = max(gEmb.albedo, 0.0);
    a.opacity = saturate(gEmb.alpha);
    a.metallic = saturate(gEmb.metallic);
    a.roughness = saturate(gEmb.roughness);
    a.emissive = max(gEmb.emission, 0.0);
    a.occlusion = saturate(gEmb.ao);
    a.alphaCutoff = gEmb.alphaScissor;
    a.normalLen = 1.0;
    a.normalTS = float3(0.0, 0.0, 1.0);
    if (any(abs(gEmb.normalMap - float3(0.5, 0.5, 1.0)) > 1.0e-5)) {
        const float3 t = gEmb.normalMap * 2.0 - 1.0;
        a.normalTS = normalize(float3(t.xy * gEmb.normalMapDepth, t.z));
    } else if (any(abs(gEmb.normal - gEmb.normalIn) > 1.0e-6)) {
        a.normalTS = embWorldNormalToTS(v, normalize(embViewDirToWorld(gEmb.normal)));
    }
    // clearcoat -> the layered BSDF's coat (live with RENDER.LAYEREDBSDF), subsurface -> the wrap / transmission terms.
    a.coatWeight = saturate(gEmb.clearcoat);
    a.coatRoughness = saturate(1.0 - gEmb.clearcoatGloss);
    a.coatF0 = 0.04;
    a.subsurfaceWeight = saturate(gEmb.sssStrength);
    a.subsurfaceRadius = saturate(gEmb.sssDepth * 0.25);
    a.subsurfaceColor = max(gEmb.sssColor.rgb, 0.0);
    // The anisotropy lobe is widened, as the BaseMaterial3D fx does.
    a.roughness = saturate(a.roughness * (1.0 + 0.5 * abs(gEmb.anisotropy)));
}

// What needs the finished surface: unshaded, the sun's rim and backlight terms, SPECULAR, ambient and specular modes (the fx evaluator's formulas).
void embPost(inout AverSurface s, AverVertex v, AverLight l, uint fx) {
    s.reflectance = 0.08 * gEmb.specular;
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
        if (gEmb.rim > 0.0) {
            const float rimLight = pow(max(0.0, 1.0 - s.ndv), max(0.0, (1.0 - s.rough) * 16.0));
            add += rimLight * gEmb.rim * lerp(float3(1.0, 1.0, 1.0), s.albedo, gEmb.rimTint) * sunE;
        }
        if (any(gEmb.backlight > 0.0)) add += gEmb.backlight * s.albedo * saturate(dot(-s.N, l.direction)) * sunE;
        s.emissive += add;
    }
    if ((fx & 1u) != 0u) {   // unshaded
        s.display = true;
        s.displayColor = float4(s.albedo + s.emissive, s.alpha);
        if (gMaterialFlags & AVER_MAT_ALPHA_BLEND) s.displayColor.rgb *= s.displayColor.a;
    }
}

// ---- vertex(): source -> Aver and back --------------------------------------------------------------------------------------------------------------------
void embVertexBegin(VSIn i, VSOut o, uint fx) {
    embCamInit();
    EmbCtx c = (EmbCtx)0;
    if ((fx & 16u) != 0u) {   // world_vertex_coords: VERTEX and NORMAL are in world space
        c.vertex = embE2S(o.wpos) * 0.01;
        c.normal = embE2S(normalize(o.nrmWS));
    } else {
        c.vertex = embE2S(i.pos) * 0.01;
        c.normal = embE2S(normalize(i.nrm));
    }
    c.uv = i.uv;
    c.uv2 = float2(0.0, 0.0);
    c.color = float4(1.0, 1.0, 1.0, 1.0);
#if AVER_MESH_EXTRA
    c.uv2 = i.uv2;
    c.color = i.col;
    if (abs(i.tng.w * 2.0 - 1.0) > 0.5) {
        c.tangent = embE2S(i.tng.xyz * 2.0 - 1.0);
        c.binormal = cross(c.normal, c.tangent) * ((i.tng.w * 2.0 - 1.0) > 0.0 ? 1.0 : -1.0);
    }
#endif
    const float4x4 mv = mul(embModelMatrix(), embViewMatrix());   // GLSL view * model, in the emitter's memory order (rows hold columns)
    c.modelview = mv;
    c.modelviewNormal = float3x3(normalize(mv[0].xyz), normalize(mv[1].xyz), normalize(mv[2].xyz));
    c.projection = embIdentity4();
    c.zClipScale = 1.0;
    gEmb = c;
}
void embVertexEnd(VSIn i, inout VSOut o, uint fx) {
    float3 wpos, nrm;
    if ((fx & 32u) != 0u) {   // skip_vertex_transform: the shader left VERTEX and NORMAL in view space
        wpos = embViewPosToWorld(gEmb.vertex);
        nrm = embViewDirToWorld(gEmb.normal);
    } else if ((fx & 16u) != 0u) {
        wpos = embS2E(gEmb.vertex) * 100.0;
        nrm = embS2E(gEmb.normal);
    } else {
        const float3 local = embS2E(gEmb.vertex) * 100.0;
        wpos = mul(float4(local, 1.0), gWorld).xyz;
        nrm = averTransformNormal(embS2E(gEmb.normal), gWorld);
    }
    o.wpos = wpos;
    o.nrmWS = normalize(nrm);
    o.pos = mul(float4(wpos, 1.0), gViewProj);
    o.uv = gEmb.uv;
#if AVER_MESH_EXTRA
    o.vcol = gEmb.color;
    o.uv2 = gEmb.uv2;
#endif
}
