
// Feature-owned frame constants, at the register RHIResources.hpp reserves for a render feature.
#define AVER_SHADOW_CASCADES 4

// register(AVER_CB_JOIN(b, AVER_FEATURE_FRAME_CB)), never a literal b4: rhi::kFeatureFrameConstantRegister
// is the one definition (shaderConstantsHlsl() emits this #define; ClusterFrameCB binds the same way). A
// literal would keep compiling against a renumbered slot while the C++ side moved to the new one.
cbuffer VoxiFrame : register(AVER_CB_JOIN(b, AVER_FEATURE_FRAME_CB)) {
    float4   gVoxelOrigin; // xyz = volume min corner, w = 1/volumeWorldSize
    float4   gVoxelParams; // x = resolution, y = intensity, z = maxDistance, w = enabled|debug<<1
    // One per cascade, tightest first: world space into that cascade's own [-1,1] clip box.
    float4x4 gCascadeViewProj[AVER_SHADOW_CASCADES];
    // x = the RADIUS at which this cascade stops applying, y = its normal-offset bias in world units
    float4   gCascadeSplit[AVER_SHADOW_CASCADES];
    float4   gShadowParams; // x = 1/atlasSize, y = enabled, z = accel structure built, w = cascades
    float4   gShadowDraw;   // x = the cascade the depth-only pass is filling right now
    // x = tan of the sun's ANGULAR RADIUS, which is what sets how fast a shadow edge softens;
    // y = occlusion rays per pixel; z = ray bias in world units at the near plane;
    // w = 1 once the flat geometry table is ready for a reflection ray's hit lookup.
    float4   gRtParams;
    // x = 1 while t6/u2 (gRtShadowHist / gRtShadowHistOut) are bound to real textures this frame;
    // y = 1 once gRtShadowHist ALSO holds a real previous frame (0 right after creation/resize);
    // z = the current frame index, a pure per-frame count, never wall-clock;
    // w = the pixels-per-ray tile edge as its BIT COUNT (0 = no tiling, every pixel traces).
    float4   gRtHistParams;
    // LAST frame's camera view-projection, for reprojecting a pixel's world position into
    // gRtShadowHist. Only meaningful while gRtHistParams.y is set.
    float4x4 gPrevViewProj;
    // LAST frame's scene viewport rect (x, y, w, h in target pixels): the reprojected NDC lands
    // here, not at [0,1] of the whole history texture -- the editor docks the 3D view in a sub-rect
    // of the backbuffer. Same validity as gPrevViewProj.
    float4   gSceneViewport;
    // THIS frame's scene viewport rect, same (x, y, w, h) in target pixels. Paired with gViewProj,
    // where gSceneViewport above is paired with gPrevViewProj. w == 0 means the device had none.
    float4   gSceneViewportCur;
    // x = 1 when the eye is inside a blended single-sided volume, y = that medium's ior.
    // Computed once per frame on the CPU -- a pixel cannot know whether its own volume encloses the
    // camera. See VoxiRenderer's own comment for why a loose bounding-sphere test is safe here.
    float4   gCameraMedium;
    // The caustic caster's world box: min.xyz / max.xyz, min.w = 1 when one exists, max.w strength.
    // max.z is the surface light refracts through. See VoxiRenderer for why a box and not a sphere.
    float4   gCausticMin;
    float4   gCausticMax;
    // The GI-ONLY shadow map's light view-projection: one box fitted to the GI VOLUME, not to the
    // camera. Read only by giShadowFactor (PSVoxel); the cascades above stay camera-fitted and are
    // what PSMainVoxi samples.
    float4x4 gGiShadowViewProj;
    // x = 1/kGiShadowSize, y = 1 once the GI-only map is usable (0 = fall back to unshadowed
    // indirect), z = normal-offset bias in world units, w unused.
    float4   gGiShadowParams;
    // The SPATIAL shadow denoiser. x = filter radius in pixels (0 = off); y = how much of the
    // filtered value to take (0 = none: taps still run and the result is discarded -- the
    // cost-measurement configuration; lerp(v, f, 0) is v exactly for any finite f); z, w unused.
    // Radius lives in a CONSTANT, not a #define, so the tap loop stays dynamic and can't be
    // unrolled away at zero taps -- a compile-time 0 would measure nothing and call it free.
    float4   gRtDenoiseParams;
    float4   gPtBounceParams;
    // x = total cones the diffuse gather traces, including the axial one.
    // y/z/w are REFRACTION, not spare: y = mode (Settings::refractionMode, 0 = off),
    // z = strength, w = edge fade. averRefractedBackdropUV below reads all three. They said
    // "unused" for as long as refraction has existed -- the feature landed in a row this
    // comment still described as free, which is exactly how the next person adding a field
    // near here would have overwritten it.
    float4   gGiParams;
    // x = sky-visibility rays the ambient term traces per pixel; 0 means "use the cone gather's own
    // occlusion instead", which is every tier below the top and is what this shader did before the
    // rays existed.
    // y = the COHERENCE TILE EDGE those rays share a direction across (1 = per-pixel). Claimed within
    // days of this row being written down as spare, which is what gGiParams directly above warns
    // happens; z/w are what is left, and the warning still applies to them.
    float4   gAmbientParams;
    // Editor view modes the ray-driven path honours itself. x = unlit; y/z/w spare.
    // Mirrors FrameConstants::viewParams -- appended at the END, so every offset above is
    // untouched. See VoxiRenderer.hpp's static_assert for the guard that makes that a rule.
    float4   gViewParams;
};

// ---- Voxi: voxel cone traced GI ----
RWTexture3D<float4> gVoxelUAV : register(u0);
Texture3D<float4>   gVoxelTex : register(t0);
SamplerState        gVoxelSamp : register(s0);

// Injection accumulator: channel k of voxel c lives at (c.x * 4 + k, c.y, c.z), k == 3 being the
// fragment count. R32_UINT is the only typed format D3D12 guarantees UAV atomics on.
RWTexture3D<uint> gVoxelAccum : register(u1);

// Fixed-point scale radiance is multiplied by before accumulation and divided by in CSResolve.
#define AVER_VOX_FIXED 16384.0
#define AVER_VOX_MAXRAD 16.0
// The aperture of the single cone PSVoxel traces into the previous bake, as tan(half-angle).
// 0.577 is tan(30), a 60-degree cone -- the same shape the forward gather's own axial cone uses, so
// the two agree about how much of the hemisphere a voxel can see rather than being two different
// estimates of the same quantity. Wide on purpose: this is a hemisphere-coverage question, not a
// directional one, and a narrow cone would answer it with a single corridor.
#define AVER_VOX_INJECT_APERTURE 0.577
// How much of the light already in the room is re-emitted on the next bake. 1.0 is the physically
// honest answer -- the surface reflects what reaches it, and `albedo` outside the bracket is what
// makes the series converge. Kept as a named constant because it is the first dial anyone will
// reach for if a scene ever blows up, and because 0 turns the second bounce off for an A/B without
// touching anything else.
#define AVER_VOX_FEEDBACK 3.0

// Edge, in pixels, of the tile that shares one sky-occlusion ray direction. See rtSkyOcclusion for
// why coherence rather than ray count is the lever here. 1 = a fresh rotation per pixel, which is
// what this shader did before the dial existed and is bit-identical to it.
#ifndef AVER_AO_COHERENCE_TILE
#define AVER_AO_COHERENCE_TILE 1.0
#endif

// AVER_AO_UNIFIED: let the ambient ray answer more than one question.
//
// THE OBSERVATION. rtSkyOcclusion fires a full hemisphere BVH traversal per sample and keeps ONE BIT
// of what it learns -- `open += 1.0` on a miss. On a miss it has just seen the sky, in a direction it
// knows, and throws that away while skyColor() marches the atmosphere 32 steps elsewhere in this same
// shader for the same information. On a hit it carries ACCEPT_FIRST_HIT_AND_END_SEARCH, so it does
// not even learn WHICH surface stopped it -- while thirteen cones march the voxel volume estimating
// the bounced light off exactly those surfaces.
//
// So one ray is paying for three answers and returning a third of one. At 1 this file behaves exactly
// as it always has; at 1 the ray keeps all three:
//
//   MISS   -> averSkyRadianceCheap(dir), the SH sky evaluated along the ray. This is strictly better
//             than what it replaces, and not only cheaper: the current term is an unoccluded
//             hemispherical mean scaled by a scalar, so a room with one window is lit by an average
//             of the whole sky dimmed to taste. Per-direction sampling knows WHICH sky got in.
//   HIT    -> the voxel volume sampled AT THE HIT POINT, mip 0. The cone gather's answer to the same
//             question is a widening cone that starts leaking through thin walls as it climbs mips;
//             a ray that actually traversed the geometry cannot leak, because it stopped.
//
// WHAT IT COSTS, AND WHY THIS IS A HYPOTHESIS RATHER THAN AN IMPROVEMENT. Dropping
// ACCEPT_FIRST_HIT_AND_END_SEARCH turns an any-hit query into a closest-hit one, which is typically
// 1.5-2x per ray -- the query can no longer stop at the first thing it touches. The bet is that this
// buys the removal of the GI cone gather (4.05 ms), the specular cone (2.63 ms) and both atmosphere
// marches (2.69 + 2.21 ms). If the closest-hit ray costs more than the ~11.6 ms of marching it
// replaces, the idea is simply wrong and this define should be deleted rather than defaulted on.
// MEASURE IT ON A MOVING CAMERA: every share quoted above is a still-camera share.
//
// AT 1 THE TWO PRIMARY-VISIBILITY PATHS DELIBERATELY DISAGREE, and that is the one thing about this
// switch that must not surprise anyone. Only PSRayDriven is wired to the unified gather; PSMainVoxi
// still takes the cone gather plus a scalar occlusion. Elsewhere this file insists the two must
// match ("they currently agree to 2.23 MAD and that is worth keeping") and that is still the rule --
// it is simply suspended inside a measurement mode that is off in every shipped configuration.
// Wiring PSMainVoxi follows once the trade is measured, NOT before: doing both at once would mean
// the first number came from a build with no unchanged path left to compare against.
//
// AND IT IS NOT ON A QUALITY TIER, for the same reason AVER_RD_ABLATE is not: this changes what the
// image MEANS, not how much of it there is. A tier that silently swapped the estimator would make
// two tiers of the same scene incomparable.
#ifndef AVER_AO_UNIFIED
#define AVER_AO_UNIFIED 0
#endif


// The roughness below which a reflective surface is treated as a MIRROR: no cone, no temporal
// history, no spatial filter. See rtReflectionTemporal's own comment for why all three must be
// derived from this one number rather than each choosing its own threshold.
#define AVER_REFL_MIRROR_ROUGH 0.1

// TLAS instance-mask lanes. MUST MATCH kRtMaskOpaque/kRtMaskTranslucent in VoxiRenderer.cpp -- no
// shared-source mechanism ties C++ and HLSL, so a value changed on one side only is an image bug
// with no build error, like the cbuffer mirrors this file already warns about.
//
// Every ray but the shadow ray asks for OPAQUE only. Translucent panes sit in the structure marked
// FORCE_NON_OPAQUE; a RayQuery meeting one does not commit it, so a single Proceed()+CommittedStatus
// traversal (every non-shadow ray here) would stop AT the pane and miss whatever is behind it.
// Narrowing the mask keeps those rays from ever seeing translucent geometry, so their single-Proceed
// stays valid.
#define AVER_RT_MASK_OPAQUE      0x01
#define AVER_RT_MASK_TRANSLUCENT 0x02
// THE VIEWER'S OWN BODY: an opaque instance every ray may hit EXCEPT the ray-driven primary one. A
// first-person camera sits inside its own character's head, so an unfiltered primary ray hits the
// inward-facing mesh and fills the screen with the character's own skin.
//
// scene::kMeshRendererHiddenFromOwner already exists for this; the raster walk in SandboxApp skips
// drawMesh() for it but deliberately keeps it in the acceleration structure (still a shadow caster,
// still in the GI volume) so raster primary visibility is unaffected. Ray-driven primary visibility
// -- the DEFAULT -- traces the TLAS directly, so "hidden" never reaches it. Measured: identical pose
// and log, probe 43,33,28 (ray-driven) vs 206,215,218 (--rt-render-mode 0).
//
// A third lane rather than removing the instance: every ray except the primary one still wants this
// geometry (shadow, GI bounce, mirrors, glass). The shadow ray masks AVER_RT_MASK_ALL and picks up
// this lane for free.
#define AVER_RT_MASK_OWNER_HIDDEN 0x04
// Opaque geometry as a SECONDARY ray sees it: solid surfaces including the viewer's own body. Every
// opaque traversal but the ray-driven primary ray uses this.
#define AVER_RT_MASK_OPAQUE_ALL  (AVER_RT_MASK_OPAQUE | AVER_RT_MASK_OWNER_HIDDEN)
#define AVER_RT_MASK_ALL         0xFF

// Directional shadow map. Core feature level 11_0, so it works on every DX12 GPU.
Texture2D<float>          gShadowTex  : register(t1);
SamplerComparisonState    gShadowSamp : register(s1);

// The GI-only shadow map. OUTSIDE the AVER_RT guard below on purpose: its only reader is PSVoxel,
// which is compiled without ray tracing, so a declaration inside the guard would vanish exactly
// where it is needed. Shares gShadowSamp -- same comparison state, different texture.
Texture2D<float>          gGiShadowTex : register(t8);
// THE OPAQUE SCENE, COPIED BEFORE TRANSLUCENCY REPLAYS -- see IDevice::sceneColorBackdropTexture.
// It exists so a blended surface can tint what is behind it PER CHANNEL. Hardware blending gives one
// scalar (1 - src.a) for the destination, and volume absorption is per-channel by definition, so
// without this a pane of glass can only get darker with depth, never greener.
//
// MAY BE NULL-FILLED: before the first resize, under MSAA (the copy is invalid from a multisampled
// target), or on a backend that does not implement it. averBlendBackdropValid() below is how a
// caller asks, and every use falls back to the scalar composite when it says no.
Texture2D<float4>         gBlendBackdrop : register(t10);

// Placed above AVER_RT (used to sit inside it, silently losing the definition for the shadow and
// GI-shadow pipelines and breaking 4 of them): pure arithmetic on the clock and a box, no rays needed.
// ---- CAUSTICS: light focused by the water surface onto what lies under it ----
// Projected from the volume (gCausticMin/Max), not painted into a material, so it stops exactly
// where the water stops rather than bleeding onto any surface sharing that material.
// Approximates caustic brightness as the Laplacian of the height field, analytic for a sum of sines
// (-k^2*sin(phase) per term) -- differentiates the same three sines the ripple graph uses, twice.
// NOT real caustics: no light-path/sun-angle/wall dependence, just a focus term under a flat pool.
// Shares one wave set with the ripple graph via averWaveFocus/averWaveNormal (shared_prelude.hlsl).
float averCausticFocus(float3 wpos) {
    if (gCausticMin.w < 0.5 || gCausticMax.w <= 0.0) return 0.0;
    // Inside the footprint, and below the surface. A point above the water gets nothing.
    if (wpos.x < gCausticMin.x || wpos.x > gCausticMax.x ||
        wpos.y < gCausticMin.y || wpos.y > gCausticMax.y ||
        wpos.z > gCausticMax.z) return 0.0;

    float focus = averWaveFocus(wpos.xy);

    // Sharpened: real caustics are thin bright lines, not a broad glow. The power turns a smooth
    // curvature field into the filigree the eye recognises.
    focus = pow(focus, 4.0);

    // Deeper water spreads the focus out and dims it. 200 cm is a soft falloff, not a physical depth.
    const float depth = max(gCausticMax.z - wpos.z, 0.0);
    focus *= exp(-depth / 200.0);

    // Fade out at the box edge so the pattern doesn't end on a hard line.
    const float2 edge = min(wpos.xy - gCausticMin.xy, gCausticMax.xy - wpos.xy);
    focus *= saturate(min(edge.x, edge.y) / 20.0);

    return focus * gCausticMax.w;
}

#if AVER_RT
// DXR 1.1 inline ray tracing: traced from the pixel shader, no state objects or binding tables.
RaytracingAccelerationStructure gScene : register(t2);

// The flat geometry a reflection ray reads after it hits something. Four descriptors for the whole
// scene rather than one per mesh or per material, because this RHI uses explicit descriptor tables
// and not bindless: dynamic indexing INTO one bound resource (a StructuredBuffer read at a runtime
// index, below) is a Tier 1 feature everywhere -- indexing into a descriptor HEAP, which none of
// these are, is the only thing "bindless" actually means in this codebase.
struct RtVertex   { float3 pos; float3 nrm; float2 uv; };
struct RtInstance { float4x4 objectToWorld; uint firstIndex; uint firstVertex; float3 albedo;
                    float metallic; float roughness; uint materialIndex; };
StructuredBuffer<RtVertex>   gRtVerts     : register(t3);
StructuredBuffer<uint>       gRtIndices   : register(t4);
StructuredBuffer<RtInstance> gRtInstances : register(t5);

// ---- per-material data for a ray hit, keyed by RtInstance::materialIndex ----
//
// WHAT materialIndex REPLACES: `pad`, a u32 nothing on either side of this ABI ever read (grep
// confirmed zero references before this change). A ray hit therefore had no per-material data
// reachable at all. Repurposing an already-unread field costs zero bytes: RtInstance stays 96 bytes,
// its C++-side static_assert (VoxiRenderer.hpp) is unchanged, and no existing pixel that never read
// `pad` is affected by it meaning something now.
//
// RtMaterial mirrors pbr::MaterialConstants (MaterialGpu.hpp) field-for-field, same order as
// `cbuffer AverMaterial` (PbrShaders.cpp) -- a mismatched order reads a neighbour's bytes with no
// compile error. Unread fields stay declared in order for the same reason: no partial
// StructuredBuffer element in HLSL.
#ifdef AVER_RT_BINDLESS
// Ray path's texture array, in space1 to avoid colliding with space0's descriptor tables/SRVs/matrices.
// AVER_RT_TEX_CAPACITY must equal PipelineLayout::bindlessTextureCount exactly (declaring more reads
// past the root signature's range); fixed-size because this backend serialises root signature 1.0.
#ifndef AVER_RT_TEX_CAPACITY
#error "AVER_RT_TEX_CAPACITY must be defined by the pipeline that declares the bindless table"
#endif
Texture2D gRtTextures[AVER_RT_TEX_CAPACITY] : register(t0, space1);
#define AVER_TEX_UNBOUND 0xFFFFFFFFu

// Ray-differential mip selection. 1 = SampleGrad from a footprint this pass derives; 0 = the mip-0
// SampleLevel the textured path shipped with. See averRtUvGrad for where the footprint comes from.
#ifndef AVER_RT_SAMPLEGRAD
#define AVER_RT_SAMPLEGRAD 1
#endif

#endif

struct RtMaterial {
    float4 baseColorFactor;   // rgb LINEAR already (MaterialConstants::baseColorFactor) -- no
                               // srgbToLin, unlike the raster path's gBaseColor (PbrShaders.cpp).
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
    // These two mirror MaterialConstants::subsurfaceWeight/subsurfaceRadius, which spent the
    // _pad0/_pad1 this used to declare. Same order, same offsets: the struct still ends at 96 bytes.
    float  subsurfaceWeight;
    float  subsurfaceRadius;
    // Coat row, same order as MaterialConstants and the material_prelude.hlsl cbuffer -- three
    // hand-maintained copies, order checked only by tests/formats/src/MaterialTest.cpp.
    float  coatWeight;
    float  coatRoughness;
    float  coatF0;
    float  _coatPad;

    // Mirrors MaterialConstants::texIndex order: BaseColor, MetalRough, Normal, Occlusion, Emissive,
    // Layer1BaseColor, Layer1MetalRough, Layer1Normal. 0xFFFFFFFF = no texture (0 is a real index).
    uint   texIndex[8];

    // Volume absorption, mirroring MaterialConstants::attenuationColor/attenuationDistance (took the
    // struct from 144 to 160 bytes); offsets asserted by tests/formats/src/MaterialTest.cpp.
    float3 attenuationColor;
    float  attenuationDistance;
};

#ifdef AVER_RT_BINDLESS
// One material map at a ray hit, or `fallback` where nothing bound. SampleLevel, never Sample: a
// fullscreen ray pass's neighbour pixels may hit unrelated triangles, so implicit derivatives (and
// the mip they pick) are garbage at every silhouette -- mip 0 aliases in the distance, but predictably.
// NonUniformResourceIndex because neighbouring pixels genuinely hit different materials; without it
// the hardware may broadcast one lane's index across the wave.
// ---- AVER_RD_ABLATE: a measurement switch, not a feature ----
// Ray-driven primary costs ~6.7ms of 14.55ms on PTTest vs raster's 7.82ms (RT on in both), strongly
// pixel-bound, but no RT quality dial moves it (--rt-rays 4/2/1: 14.49/14.46/14.44). GPU spans bracket
// draws not terms, so the only way to attribute cost is to neutralise one term at a time and diff.
// EVERY NON-ZERO VALUE RENDERS A DELIBERATELY WRONG FRAME -- never wire one to a quality tier; 0 is
// the only correct value and the default everywhere. Terms should roughly sum to the raster gap
// (7.82ms); if ablating everything doesn't approach that, the ablation isn't measuring what it claims.
#ifndef AVER_RD_ABLATE
#define AVER_RD_ABLATE 0
#endif
#define AVER_RD_ABL_NONE    0
#define AVER_RD_ABL_SHADOW  1   // the sun-visibility ray (rtShadowTemporal)
#define AVER_RD_ABL_GI      2   // the diffuse cone gather (coneTracedIndirect)
#define AVER_RD_ABL_REFL    3   // the mirror ray (rtReflectionTemporal)
#define AVER_RD_ABL_SKY     4   // the atmosphere march (skyColor) in the reflection branch
#define AVER_RD_ABL_TEX     5   // material texture sampling
// ALL = shadow+GI+reflection+sky, NOT textures: ablating textures makes averRtSampleSlot return its
// fallback, so roughness jumps past the `s.rough <= 0.75` gate and REROUTES onto the voxel-cone
// fallback -- why mode 5 combined with others was SLOWER than sky alone, and deltas didn't add up.
#define AVER_RD_ABL_ALL     6   // shadow + GI + reflection + sky -- the method check
// 7 differs from the others: restores ACCEPT_FIRST_HIT_AND_END_SEARCH on the sun-shadow ray (dropped
// so glass could attenuate rather than stop a shadow), measuring that decision's cost. Knowingly
// breaks tinted shadows through glass; never wire it to a quality tier either.
#define AVER_RD_ABL_SHADOW_FIRSTHIT 7
// 8..11 CLOSE THIS HARNESS'S OWN BLIND SPOTS. Modes 1-6 above between them leave four of the pass's
// larger terms unmeasurable, which matters more than it sounds: a sweep that reports 1-6 looks
// complete and silently attributes none of the cost below, so the residual gets blamed on whatever
// mode happened to be biggest.
#define AVER_RD_ABL_SKYOCC   8  // the sky-visibility ray (rtSkyOcclusion) -- ambient occlusion
// The SPECULAR cone, traced in the rough-surface branch. NOT covered by AVER_RD_ABL_GI, which only
// skips coneTracedIndirect -- so mode 2 has always left a 14th cone running and called it "GI off".
#define AVER_RD_ABL_SPECCONE 9
// skyColor in the ROUGH branch. Mode 4 ablates only the reflection branch's march; this is the other
// call site, and it is the one an ENCLOSED scene actually takes.
#define AVER_RD_ABL_ROUGHSKY 10
// averApplyFog: a 4-step aerial march plus a possible second 32-step atmosphere march, run
// unconditionally per pixel. This function is recorded elsewhere in the tree as having once been
// 41% of a frame. MEASURED on Sponza it is now 0.22 ms of a 16.83 ms pass -- 1.3% -- so that
// history is a reason to keep it attributable, not a reason to assume it is still expensive.
#define AVER_RD_ABL_FOG      11
// The AERIAL HALF of averApplyFog alone -- the 4-step atmosphere march between camera and
// surface -- with height fog left running, since mode 11 removes both and cannot separate them.
// EXISTS BECAUSE A GATE WAS PROPOSED FOR THAT MARCH and there was no way to price it: it is the
// only term in averApplyFog with no magnitude threshold, which makes it look like the eager-lerp
// bug this codebase has fixed twice. It measured 0.19 ms, 1.1% of the pass, while changing 28.6%
// of the frame by up to 37 codes -- used, not discarded -- so no gate was added. Kept so the next
// person to notice the missing threshold can re-run the number instead of re-deriving it.
#define AVER_RD_ABL_AERIAL   12

// READ THIS BEFORE SUBTRACTING TWO ABLATION NUMBERS. Several modes REROUTE work rather than removing
// it, so the deltas are not additive and a term can measure NEGATIVE:
//   - Mode 3 (REFL) leaves specHit false, which makes `!specHit || skyW > 0.0` always true and forces
//     a full 32-step skyColor(R) march that a committed hit would have skipped. Mode 3's delta is
//     therefore (reflection cost) MINUS (added sky march).
//   - Mode 5 (TEX) is the documented one, above -- and additionally makes every alpha-masked cutout
//     candidate commit, terminating traversal EARLIER than reality, so it can read faster than truth.
//   - Mode 1 (SHADOW) replaces only the primary rtShadowTemporal call. The reflection's inner shadow
//     ray and the bounce loop's shadow ray both keep running, so it measures the primary shadow
//     estimator, NOT all shadow rays in the pass.

float4 averRtSampleSlot(RtMaterial mat, uint slot, float2 uv, float2 gx, float2 gy, float4 fallback) {
    const uint idx = mat.texIndex[slot];
    if (idx == AVER_TEX_UNBOUND) return fallback;
#if AVER_RD_ABLATE == AVER_RD_ABL_TEX
    return fallback;   // ablated: the slot is bound, but nothing is sampled
#endif
#if AVER_RT_SAMPLEGRAD
    // SampleGrad with a derived footprint, not SampleLevel(0). Zero gradients degrade to mip 0
    // exactly, so a degenerate triangle or unsolvable gradient lands on old behaviour, not undefined.
    return gRtTextures[NonUniformResourceIndex(idx)].SampleGrad(gMaterialSampler, uv, gx, gy);
#else
    return gRtTextures[NonUniformResourceIndex(idx)].SampleLevel(gMaterialSampler, uv, 0);
#endif
}

// ---- the material-graph adapter for this pass ---------------------------------------------------
//
// A generated material graph emits `averSampleSlot(slot, uv)`, which reads the eight BOUND texture
// registers through the material cbuffer's sampler. A ray hit has neither: its maps live in the
// bindless gRtTextures array, indexed through the hit's own RtMaterial. So the generated body cannot
// be used here verbatim -- and that, not any missing feature, is why "no material GRAPH runs on any
// ray path" was true for the renderer that is the DEFAULT path.
//
// MaterialGraphHlsl.cpp emits a second copy of every graph body with `averSampleSlot(` rewritten to
// `averRtSampleSlotGraph(`, so the two copies differ in exactly one token and cannot drift.
//
// THE MATERIAL TRAVELS IN A STATIC, NOT A PARAMETER, and that is the part worth explaining. The
// generated body is written by a module that cannot see RtMaterial -- Aver.Render.PBR.Materials has
// no idea this backend exists, and teaching it would invert the dependency. Threading `mat` through
// would mean the emitter naming a type it must not know. A per-thread static costs nothing (HLSL
// statics are per-invocation, not shared) and keeps the emitter ignorant, which is the seam that
// matters. Set these immediately before calling the graph; nothing else reads them.
static RtMaterial gAverGraphMat;
static float2     gAverGraphGx;
static float2     gAverGraphGy;

// WHITE, NOT BLACK, for an unbound slot -- matching averSampleSlot's own #else branch in
// material_prelude.hlsl. A graph that multiplies by an unbound map must get the identity, or every
// material without (say) an occlusion map would shade to nothing the moment a graph sampled one.
float4 averRtSampleSlotGraph(uint slot, float2 uv) {
    return averRtSampleSlot(gAverGraphMat, slot, uv, gAverGraphGx, gAverGraphGy,
                            float4(1.0, 1.0, 1.0, 1.0));
}

// The UV-space footprint of one pixel's primary ray, for SampleGrad.
//
// WHY THIS IS NOT ddx(uv)/ddy(uv). A pixel shader's implicit derivatives describe the SCREEN
// coordinate; in a fullscreen ray pass the neighbouring lane may have hit a different triangle, a
// different object, or nothing, so those derivatives are meaningless here. What IS available is
// rdRayDx/rdRayDy -- the neighbouring pixels' own primary rays, reconstructed analytically from
// gInvViewProj and scaled by this ray's hitT, already built in this shader for the shadow disc.
// That is a real world-space footprint; this turns it into a UV-space one.
void averRtUvGrad(RtMaterial mat, RtInstance inst, float3 N,
                  float3 p0, float3 p1, float3 p2,
                  float2 t0, float2 t1, float2 t2,
                  float3 dx, float3 dy,
                  out float2 gx, out float2 gy) {
    gx = 0.0;
    gy = 0.0;

    if (mat.flags & AVER_MAT_WORLD_UV) {
        // A world-aligned material's UV is a planar projection of world position, so its gradient is
        // just that same projection of the world delta -- no Jacobian to invert. Same axes and
        // dominant-axis choice as averRtSurfaceUV, or the two would describe different planes.
        const float3 ax = normalize(inst.objectToWorld[0].xyz);
        const float3 ay = normalize(inst.objectToWorld[1].xyz);
        const float3 az = normalize(inst.objectToWorld[2].xyz);
        const float3 a  = abs(float3(dot(N, ax), dot(N, ay), dot(N, az)));
        if (a.z >= a.x && a.z >= a.y) {
            gx = float2(dot(dx, ax), dot(dx, ay));
            gy = float2(dot(dy, ax), dot(dy, ay));
        } else if (a.x >= a.y) {
            gx = float2(dot(dx, ay), dot(dx, az));
            gy = float2(dot(dy, ay), dot(dy, az));
        } else {
            gx = float2(dot(dx, ax), dot(dx, az));
            gy = float2(dot(dy, ax), dot(dy, az));
        }
        gx *= mat.uvTilesPerCm;
        gy *= mat.uvTilesPerCm;
        return;
    }

    // Mesh UV: invert the triangle's position-to-UV map. T/B are dP/du, dP/dv, deliberately
    // UNNORMALISED -- their lengths are centimetres per UV unit, the scale being inverted.
    const float3 e1 = p1 - p0, e2 = p2 - p0;
    const float2 d1 = t1 - t0, d2 = t2 - t0;
    const float  r  = d1.x * d2.y - d2.x * d1.y;
    if (abs(r) < 1e-12) return;   // degenerate UVs: no footprint, so mip 0, as before

    const float3 T = mul(float4((e1 * d2.y - e2 * d1.y) / r, 0.0), inst.objectToWorld).xyz;
    const float3 B = mul(float4((e2 * d1.x - e1 * d2.x) / r, 0.0), inst.objectToWorld).xyz;

    // Dual basis of (T, B, N): the rows of the inverse. A world delta in the tangent plane projects
    // onto these to give its (du, dv).
    const float3 cbn = cross(B, N);
    const float3 cnt = cross(N, T);
    const float  det = dot(T, cbn);
    if (abs(det) < 1e-12) return;   // T parallel to B: no usable frame

    gx = float2(dot(cbn, dx), dot(cnt, dx)) / det;
    gy = float2(dot(cbn, dy), dot(cnt, dy)) / det;
}

// The texture coordinate a ray hit samples at: mesh UV, or a planar projection for world-aligned UV.
// The first version of this path sampled every material at mesh UV, which read as noise on any mesh
// whose UVs don't match its projection (PTTest's floor/concrete, worlduv=1) -- missed because
// ElectricDreams terrain, where this was first measured, doesn't set the flag.
// Mirrors averSurfaceUV (material_prelude.hlsl), inst.objectToWorld standing in for gWorld.
float2 averRtSurfaceUV(RtMaterial mat, RtInstance inst, float3 wpos, float3 N, float2 meshUV) {
    if (!(mat.flags & AVER_MAT_WORLD_UV)) return meshUV;
    const float3 ax = normalize(inst.objectToWorld[0].xyz);
    const float3 ay = normalize(inst.objectToWorld[1].xyz);
    const float3 az = normalize(inst.objectToWorld[2].xyz);
    const float3 d  = wpos - inst.objectToWorld[3].xyz;
    const float3 op = float3(dot(d, ax), dot(d, ay), dot(d, az));
    const float3 on = float3(dot(N, ax), dot(N, ay), dot(N, az));
    const float3 a  = abs(on);
    const float2 pp = (a.z >= a.x && a.z >= a.y) ? op.xy
                    : ((a.x >= a.y) ? op.yz : op.xz);
    return pp * mat.uvTilesPerCm;
}

// A tangent frame for a ray hit, solved from the triangle's own positions and UVs -- the raster
// path's ddx/ddy trick is invalid at a ray hit, and RtVertex carries no tangent stream (despite
// docs/formats/FORMAT_SPECS.md claiming .ocmesh has a MikkTSpace QTangent: normalToQTangent() only
// computes a shortest-arc rotation from the normal, so no real tangent has ever been stored).
// Derived per triangle, so it is CONSTANT across the face -- slightly flatter than an interpolated
// frame on curved surfaces, exact on the flat ones normal maps are mostly used on.
float3 averRtPerturbNormal(RtMaterial mat, RtInstance inst, float3 N, float3 nTS,
                           float3 p0, float3 p1, float3 p2,
                           float2 t0, float2 t1, float2 t2) {
    float3 T, B;

    if (mat.flags & AVER_MAT_WORLD_UV) {
        // The frame must match the parametrisation it samples: solving a tangent from mesh UVs here
        // would rotate it arbitrarily against a world-aligned projection, lighting a normal map from
        // the wrong direction rather than failing obviously. Same axes/dominant-axis as averRtSurfaceUV.
        const float3 ax = normalize(inst.objectToWorld[0].xyz);
        const float3 ay = normalize(inst.objectToWorld[1].xyz);
        const float3 az = normalize(inst.objectToWorld[2].xyz);
        const float3 a  = abs(float3(dot(N, ax), dot(N, ay), dot(N, az)));
        if (a.z >= a.x && a.z >= a.y) { T = ax; B = ay; }        // uv = op.xy
        else if (a.x >= a.y)          { T = ay; B = az; }        // uv = op.yz
        else                          { T = ax; B = az; }        // uv = op.xz
    } else {
        const float3 e1 = p1 - p0, e2 = p2 - p0;
        const float2 d1 = t1 - t0, d2 = t2 - t0;
        const float  r  = d1.x * d2.y - d2.x * d1.y;

        // Degenerate UVs are common, not exotic (an untextured/collapsed shell gives r==0, and
        // dividing by it produces NaN that reads as a lighting bug). Mirrors averPerturbNormal's
        // `if (m <= 0.0) return N;`.
        if (abs(r) < 1e-12) return N;

        const float3 tObj = (e1 * d2.y - e2 * d1.y) / r;
        const float3 bObj = (e2 * d1.x - e1 * d2.x) / r;
        // Row-vector multiply, the same convention the shading normal uses.
        T = mul(float4(tObj, 0.0), inst.objectToWorld).xyz;
        B = mul(float4(bObj, 0.0), inst.objectToWorld).xyz;
    }

    if (dot(T, T) <= 0.0) return N;

    // Gram-Schmidt against the SHADING normal, so the frame stays orthonormal where the interpolated
    // normal has been bent away from the triangle's own plane.
    T = normalize(T - N * dot(N, T));
    if (!(dot(T, T) > 0.0)) return N;

    // Handedness taken from the solved bitangent rather than assumed: a mirrored UV shell has the
    // opposite one, and guessing lights half of a symmetrical model from the wrong side.
    const float3 Bo = cross(N, T);
    B = Bo * (dot(Bo, B) < 0.0 ? -1.0 : 1.0);

    return normalize(T * nTS.x + B * nTS.y + N * nTS.z);
}
#endif
// SLOT t9 IS A GUESS: table 0's next free SRV after t8. kGiSrvCount (VoxiGiShaders.hpp) must become
// 10 for table 1 (material textures) to auto-rebase from t9 to t10 (see giLayout()). Follows the
// t3/t4/t5 recipe: one more setSrvBuffer call, built like gRtInstances. C++ side (kGiSrvCount,
// giTableKinds, the per-frame buffer) owned by a concurrent agent on VoxiRenderer.hpp/.cpp -- if
// their actual slot differs from t9, move THIS line.
StructuredBuffer<RtMaterial> gRtMaterials : register(t9);

// ---- ALPHA-TESTED GEOMETRY, SEEN BY A RAY --------------------------------------------------------
//
// THE HOLE A LEAF CARD IS MADE OF DID NOT EXIST FOR ANY RAY. createBlas marks every geometry OPAQUE
// and, until this change, only a BLENDED material un-opaqued its instance -- so an alpha-MASKED
// material (foliage, chain-link, grates: opaque where it is opaque, absent where it is not) was
// traced as a solid sheet. The raster depth prepass clips it correctly (material_prelude.hlsl's
// `clip(s.alpha - a.alphaCutoff)`), so the two paths disagreed, and the ray-driven path -- the
// standing default -- was the wrong one. Every leaf rendered as its bounding rectangle.
//
// WHAT THE FIX COSTS: an alpha-masked instance is now FORCE_NON_OPAQUE, which gives up the
// hardware's right to skip any-hit on it. Each candidate on such an instance pays an index fetch,
// three vertex reads and one texture sample. Opaque geometry is untouched and keeps the fast path.
//
// TAKES THE RayQuery BY REFERENCE and is written against the ONE template argument every trace in
// this file uses. A second flag set would need its own copy -- HLSL has no way to be generic over
// the flags, which is exactly why the loops below call this rather than inlining it six times.
// GUARDED ON AVER_RT_BINDLESS, which is what gates the material and texture tables this reads
// (averRtSampleSlot and averRtSurfaceUV are both inside that block). Without them a cutout cannot be
// expressed at all -- there is no alpha to fetch -- so every candidate is exactly as solid as its
// geometry says, which is the behaviour this file had before any of this.
bool averRtCandidateOpaque(inout RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q) {
#ifndef AVER_RT_BINDLESS
    return true;
#else
    const RtInstance inst = gRtInstances[q.CandidateInstanceID()];
    const RtMaterial mat  = gRtMaterials[inst.materialIndex];
    // NOT ALPHA-MASKED MEANS OPAQUE. A translucent pane also arrives here as a candidate; it is not
    // this function's business and must not be committed by it -- the callers that care about
    // transmittance test the material themselves before consulting this.
    if ((mat.flags & AVER_MAT_ALPHA_MASK) == 0) return true;

    const uint tri = inst.firstIndex + q.CandidatePrimitiveIndex() * 3;
    const uint i0  = inst.firstVertex + gRtIndices[tri + 0];
    const uint i1  = inst.firstVertex + gRtIndices[tri + 1];
    const uint i2  = inst.firstVertex + gRtIndices[tri + 2];

    const float2 bary = q.CandidateTriangleBarycentrics();
    const float3 w    = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float2 uv = gRtVerts[i0].uv * w.x + gRtVerts[i1].uv * w.y + gRtVerts[i2].uv * w.z;

    // WORLD-ALIGNED UV IS RESOLVED, NOT IGNORED. A masked material using the planar projection is
    // unusual, but sampling its mesh UV instead would cut holes in the wrong places -- silently, and
    // only on that one material. The geometric normal is enough for the projection's axis choice,
    // so no interpolated normal is needed here.
    if (mat.flags & AVER_MAT_WORLD_UV) {
        const float3 p0 = gRtVerts[i0].pos, p1 = gRtVerts[i1].pos, p2 = gRtVerts[i2].pos;
        const float3 nObj = cross(p1 - p0, p2 - p0);
        const float3 N    = normalize(mul(float4(nObj, 0.0), inst.objectToWorld).xyz);
        const float3 wpos = q.WorldRayOrigin() + q.WorldRayDirection() * q.CandidateTriangleRayT();
        uv = averRtSurfaceUV(mat, inst, wpos, N, uv);
    }

    // SampleLevel, via averRtSampleSlot -- ddx/ddy is undefined at a ray hit, and an UNBOUND slot
    // returns opaque white, so a masked material whose atlas failed to load stays solid rather than
    // vanishing entirely. Slot 0 is BaseColor (pbr::TextureSlot::BaseColor).
    const float alpha = averRtSampleSlot(mat, 0, uv, float2(0, 0), float2(0, 0),
                                         float4(1, 1, 1, 1)).a * mat.baseColorFactor.a;
    return alpha >= mat.alphaCutoff;
#endif
}

// Runs a query to its nearest genuinely-solid hit, honouring cutouts on the way.
//
// REPLACES `RAY_FLAG_FORCE_OPAQUE` + a single `Proceed()`. That idiom was correct while the only
// non-opaque instances were translucent ones, which these rays exclude by mask -- its own comment
// called FORCE_OPAQUE "provably a no-op here", and it was, right up until an alpha-masked instance
// could appear in the opaque lane. With the flag left on, the hardware commits the leaf card and
// never asks; without it, a candidate arrives and this decides.
//
// STILL BOUNDED IN PRACTICE by the mask: only alpha-masked instances produce candidates on these
// rays, so a scene with no cutout materials loops exactly as many times as it used to.
void averRtProceedSolid(inout RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q) {
    while (q.Proceed()) {
        if (q.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE && averRtCandidateOpaque(q))
            q.CommitNonOpaqueTriangleHit();
    }
}

// Ray-traced sun-shadow history: last frame's resolved (visibility, depth) (t6) and this frame's
// (u2, becomes t6 next frame). Ping-ponged in VoxiRenderer.hpp's rtShadowHist_, never the same
// texture in one frame. x = visibility [0,1], y = linear depth in cm, for rtReprojectHistory.
Texture2D<float2>   gRtShadowHist    : register(t6);
RWTexture2D<float2> gRtShadowHistOut : register(u2);
// The SKY-OCCLUSION history pair. Same RG32Float shape as the shadow pair above (x = openness,
// y = linear depth) and bound in lockstep with it, so gRtHistParams.x/.y speak for all three
// pairs at once and no fourth constant is needed.
Texture2D<float2>   gAoHist    : register(t11);
RWTexture2D<float2> gAoHistOut : register(u4);

// Ray-traced reflection history: same ping-pong as the shadow history above, its own pair of
// textures. rgb = shaded colour, a = linear hit depth, OR NEGATIVE meaning the ray missed.
Texture2D<float4>   gRtReflHist    : register(t7);
RWTexture2D<float4> gRtReflHistOut : register(u3);

// ---- Last frame's normal-roughness G-buffer, read-only, for the spatial denoisers' crease term ----
// NOT the same resource as SV_TARGET3 below: that's THIS frame's, bound as an RTV by the same draw
// that would need it as an SRV, which no backend allows and no pixel shader could see anyway
// (not-yet-written neighbour output). Same ping-pong fix as rtShadowHist_/rtReflHist_, applied to
// this channel -- SV_TARGET3 stays the "write" half, this is the "read last frame" half.
//
// Gated behind its OWN define, not AVER_GBUFFER: that only proves the single non-ping-ponged target
// exists. Reading an unbound register is a silent null-descriptor read on real hardware and a
// Vulkan validation failure, so this stays off until the ping-ponged pair (VoxiRenderer.hpp/.cpp,
// D3D12Device.cpp, VulkanDevice.cpp) exists -- compiled in only with AVER_GBUFFER_HISTORY=1 on top
// of AVER_GBUFFER=1, a host-side contract this file does not itself enforce.
//
// SLOT t10 IS A GUESS (same reasoning as gRtMaterials' t9): move this line if the real slot differs.
// Assumed the same resolution as gRtShadowHist -- tap loops below reuse ITS GetDimensions()/bounds
// check rather than querying this texture separately; unverified here since allocation is C++-side.
#if AVER_GBUFFER_HISTORY
Texture2D<float4> gGBufNormalHist : register(t10);
#endif

// A hash of the pixel, for rotating each pixel's sample pattern. SPATIAL ONLY, deliberately: the
// gate oracle compares 8-bit probe codes bit-exactly, and some gates sample a penumbra, so a
// per-frame seed would make those a coin flip. Still true now that rtShadow layers its own explicit
// `frameJitter` on top (see rtShadow) -- frame variance is added per call site, not baked in here.
float rtHash(float2 p) {
    float3 q = frac(float3(p.xyx) * float3(0.1031, 0.1030, 0.0973));
    q += dot(q, q.yzx + 33.33);
    return frac((q.x + q.y) * q.z);
}


// Radical inverse of `i` in base 2 (bits reflected about the binary point) in [0,1). Makes the
// sample sequence NESTED, unlike the sqrt((k+0.5)/n) it replaced: that put sample k at a radius
// depending on the TOTAL ray count, so n=2 and n=4 were unrelated estimators with no shared samples.
// phi(k) depends on k alone, so raising the ray count keeps every ray already traced and fills in
// between -- what lets a ray-count sweep read as convergence and one baseline serve every count.
// EXACT on every adapter (unlike a hash): reversebits + IEEE round-to-nearest + 2^-32 being a power
// of two make the multiply exact, which the gate oracle's bit-exact 9-configuration comparison needs.
float rtRadicalInverse2(uint i) {
    return (float)reversebits(i) * 2.3283064365386963e-10;   // 1 / 2^32
}

// Sample `k` of the disc sequence the shadow loop walks, as a point in the unit disc. `ang0` turns
// the pattern by a per-pixel angle so neighbours don't share one set of directions.
// The signature has no ray count, deliberately: a sample is a function of its index alone (what
// "nested" means, unlike the sqrt((k+0.5)/n) it replaced) -- kept as its own function so that claim
// is checkable in one place. Golden angle around, radical inverse outward, sqrt maps radius onto
// AREA so samples don't crowd the centre and bias toward the middle of the sun.
float2 rtDiscSample(uint k, float ang0) {
    float rad = sqrt(rtRadicalInverse2(k + 1));
    float a   = ang0 + (float)k * 2.39996323;
    return float2(cos(a), sin(a)) * rad;
}

// Traces occlusion rays toward the sun's DISC and returns the fraction that reached it: 0 fully
// shadowed, 1 fully lit, everything between a real penumbra.
//
// The single ray this replaced returned exactly 0.0 or 1.0 -- a hard aliased edge next to the
// cascade path's 3x3 filter, so ray tracing ON made shadows look worse. The sun subtends about half
// a degree, and that angle sets how fast an edge softens; gRtParams.x carries its tangent so the
// softening is the SUN's property, not a tuned constant.
//
// Bias scales with camera distance: a fixed 0.02cm offset is ~300 float ulp at 1000cm but under 3
// at 100000cm, so distant geometry self-intersects and speckles like flickering rather than acne.
//
// `dpx`/`dpy` (the receiver's screen-space footprint) are PASSED IN rather than taken via
// ddx/ddy(wpos) here: that is only correct for a primary surface, and wrong for a reflected hit,
// whose neighbouring pixels land on different triangles metres apart (and a derivative inside
// divergent flow is undefined in HLSL regardless). A reflected caller passes zero for a point
// sample.
//
// `rays` is explicit, not read from the cbuffer, so a secondary ray can ask for fewer than a
// primary one -- a reflection is already an approximation, and a full disc sweep on its shadow buys
// detail nobody can resolve.
//
// `frameJitter` is ADDED to the per-pixel rotation and is NOT part of rtHash, which stays a pure
// function of the pixel for the gate oracle's bit-exact probes. Only rtShadowTemporal passes a
// nonzero value, and only when pixel tiling is on -- that's what lets tiling converge instead of
// repeating one sample forever.
float3 rtShadow(float3 wpos, float3 N, float3 L, float2 pixel, float3 dpx, float3 dpy, uint rays,
               float frameJitter) {
    const uint  n    = max(rays, 1u);
    const float tanR = max(gRtParams.x, 0.0);
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);

    // A frame around the light direction, to spread samples across the disc.
    float3 up = abs(L.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T  = normalize(cross(up, L));
    float3 B  = cross(L, T);

    // THE PIXEL'S OWN FOOTPRINT ON THE SURFACE, from the screen-space derivatives of world
    // position -- this is what actually fixes the jagged edge, not the sun's disc: the sun's
    // angular radius (~a quarter degree) makes the true penumbra far narrower than a pixel at
    // contact distances, so spreading rays across the disc alone gives the same binary answer
    // everywhere except a single edge pixel. Meanwhile the shadow term is computed once per pixel
    // while geometry beside it resolves at 8x MSAA, so the boundary stair-steps against smooth
    // silhouettes. Jittering the ray ORIGIN across the footprint turns the per-pixel test into an
    // area estimate -- antialiasing the shadow rather than blurring it.
    const float ang0 = rtHash(pixel) * 6.2831853 + frameJitter;
    float3 vis = float3(0.0, 0.0, 0.0);

    [loop] for (uint k = 0; k < n; ++k) {
        // Sample k, independent of n: it sits in the same place whatever the ray count, so raising
        // the count refines the estimate rather than replacing it. The same rotated pattern serves
        // both the sun disc and the pixel footprint.
        float2 disc = rtDiscSample(k, ang0);

        float3 dir = normalize(L + (T * disc.x + B * disc.y) * tanR);
        // Half the footprint, so samples stay inside the pixel they are estimating.
        float3 org = wpos + (dpx * disc.x + dpy * disc.y) * 0.5;

        RayDesc r;
        // Offset along the NORMAL and along the ray -- the normal alone leaves acne at grazing angles.
        r.Origin    = org + N * bias + dir * bias;
        r.Direction = dir;
        r.TMin      = bias;
        r.TMax      = 100000.0;
        // NO ACCEPT_FIRST_HIT: right for a binary shadow, wrong for a transmissive one -- glass IS
        // the first thing touched, so stopping there makes it a wall. The ray instead runs its own
        // traversal, multiplying a running transmittance by each translucent surface it crosses.
        // THE COST IS REAL: every shadow ray now walks to an opaque hit or the structure's end,
        // including rays that never meet a pane. Measure before assuming it's small.
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        // BOTH LANES: this is the one ray that wants to see translucent geometry.
#if AVER_RD_ABLATE == AVER_RD_ABL_SHADOW_FIRSTHIT
        // Measurement only -- see AVER_RD_ABL_SHADOW_FIRSTHIT: opaque lane only, stop at the first
        // thing touched, matching this ray's behaviour before transmissive shadows existed.
        q.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_FORCE_OPAQUE,
                         AVER_RT_MASK_OPAQUE_ALL, r);
#else
        q.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_ALL, r);
#endif

        // ---- Two models, material picks which ----
        // A VOLUME material (attenuationDistance > 0) is Beer-Lambert attenuated over distance
        // travelled inside it; otherwise the per-crossing surface rule applies (a thin sheet).
        // Gathers spans rather than multiplying as it goes, since a path length needs both ends and
        // the exit candidate can arrive anywhere in the walk.
        // PAIRED BY MIN/MAX t, never arrival order or facing: DXR doesn't guarantee non-opaque
        // candidates arrive nearest-first, and CandidateTriangleFrontFace() is worse -- winding is
        // exactly what broke volume absorption on the pool (fluid box winds opposite the cube).
        //   one hit  -> ray started inside the medium, t is the distance out (pool floor under water)
        //   two hits -> entered and exited; the span between is the thickness (a pane)
        //   more     -> concave/overlapping geometry; the outer span is the honest estimate
        // TWO SLOTS AS SCALARS, NOT AN ARRAY -- MEASURED: loop-indexed local arrays spilled out of
        // registers on the wave-bound sun shadow, 7.96ms -> 9.95ms (25% regression). Two slots is
        // what the scene needs (glass over water); a third medium falls through to the surface rule.
        uint  med0Iid = 0xffffffffu, med1Iid = 0xffffffffu;
        float med0Min = 0.0, med0Max = 0.0, med1Min = 0.0, med1Max = 0.0;
        uint  med0Hits = 0u, med1Hits = 0u;

        float3 through = float3(1, 1, 1);   // running transmittance along this ray
        // Bounded at 8 crossings: well past where transmittance is visually zero anyway.
        [loop] for (uint step = 0; step < 8u && q.Proceed(); ++step) {
            if (q.CandidateType() != CANDIDATE_NON_OPAQUE_TRIANGLE) break;

            // The material of the thing we just passed through -- what RtInstance::materialIndex and
            // the t9 material table were built for, and until now never used by a shadow ray.
            const uint iid = q.CandidateInstanceID();
            const RtMaterial m = gRtMaterials[gRtInstances[iid].materialIndex];

            // ---- A CUTOUT IS NOT A MEDIUM -------------------------------------------------------
            //
            // Alpha-masked instances are non-opaque now, so they arrive here alongside glass. They
            // must NOT fall into the transmittance walk below: a leaf is not a pane, it has no
            // thickness and no attenuation colour, and treating it as one would tint the shadow it
            // casts by whatever the fallback medium happens to be.
            //
            // The rule is binary. Above the cutoff the leaf is solid: commit it and the ray is
            // blocked, exactly as an opaque hit would have been. Below it, the ray is passing
            // through a hole and the leaf is not there at all -- continue without touching
            // transmittance, so a gap between leaves casts no shadow.
            if (m.flags & AVER_MAT_ALPHA_MASK) {
                if ((m.flags & AVER_MAT_CAST_SHADOW) && averRtCandidateOpaque(q)) {
                    q.CommitNonOpaqueTriangleHit();
                    break;                       // fully blocked; nothing past it can matter
                }
                continue;
            }

            if ((m.flags & AVER_MAT_CAST_SHADOW) == 0) continue;   // casts nothing at all

            bool handled = false;
            if (m.attenuationDistance > 0.0) {
                const float ct = q.CandidateTriangleRayT();
                if (med0Iid == iid) {
                    med0Min = min(med0Min, ct); med0Max = max(med0Max, ct); ++med0Hits; handled = true;
                } else if (med1Iid == iid) {
                    med1Min = min(med1Min, ct); med1Max = max(med1Max, ct); ++med1Hits; handled = true;
                } else if (med0Hits == 0u) {
                    med0Iid = iid; med0Min = ct; med0Max = ct; med0Hits = 1u; handled = true;
                } else if (med1Hits == 0u) {
                    med1Iid = iid; med1Min = ct; med1Max = ct; med1Hits = 1u; handled = true;
                }
            }

            if (!handled) {
                // NO VOLUME (or no slot left): the per-crossing surface rule, HLSL's twin of
                // pbr::shadowTransmittance (Material.cpp) -- diverges since this can measure a distance.
                const float k = max(1.0 - m.baseColorFactor.a, saturate(m.transmission));
                through *= saturate(m.baseColorFactor.rgb) * k;

                // Early-out once nothing gets through. Only the surface path may take it: a volume's
                // contribution isn't applied yet, so a walk carrying one must run to the end.
                if (max(through.r, max(through.g, through.b)) < 0.01 && med0Hits == 0u) {
                    through = float3(0, 0, 0);
                    break;
                }
            }

            // NOT committed: committing would end traversal here, so Proceed() reaches what's behind.
        }

        // Resolve gathered spans via the same Beer-Lambert averVolumeTransmittance the VIEW path uses
        // (reads no globals) -- keeps light going down through a medium agree with light coming up.
        if (med0Hits > 0u) {
            const RtMaterial m0 = gRtMaterials[gRtInstances[med0Iid].materialIndex];
            const float th0 = (med0Hits == 1u) ? med0Max : (med0Max - med0Min);
            through *= averVolumeTransmittance(m0.attenuationColor, m0.attenuationDistance, th0);
        }
        if (med1Hits > 0u) {
            const RtMaterial m1 = gRtMaterials[gRtInstances[med1Iid].materialIndex];
            const float th1 = (med1Hits == 1u) ? med1Max : (med1Max - med1Min);
            through *= averVolumeTransmittance(m1.attenuationColor, m1.attenuationDistance, th1);
        }

        // An opaque hit anywhere along the way blocks everything, whatever the panes in front said.
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) through = float3(0, 0, 0);

        // PER CHANNEL now: used to reduce to luminance here, so a shadow could DARKEN but never take
        // COLOUR (water lit the pool floor grey, not cyan). Reduction moved to rtShadowTemporal.
        vis += through;
    }
    return vis / (float)n;
}

// The fraction of the hemisphere above `N` from which the SKY is actually reachable: 1 fully open,
// 0 fully enclosed. This is the scalar `diffAmbient` multiplies the sky irradiance by, traced
// instead of estimated.
//
// WHY IT EXISTS. coneTracedIndirect's `ao` is six 60-degree cones marching the voxel volume, and a
// cone that widens into a coarse mip averages a thin wall with the empty space beside it and passes
// through. In an OPEN scene that estimate is approximately right and this function is a waste of
// four rays -- which is exactly why it is Epic-only. In an enclosed one it is optimistic, MEASURED
// on Sponza against a converged path-traced reference as shadowed pixels reading [25,26,30] where
// the reference says [7,7,7], blue-biased because what leaks in is sky.
//
// COSINE-WEIGHTED BY CONSTRUCTION, via Malley's method: a uniform point on the unit disc lifted onto
// the hemisphere IS a cosine-weighted direction, so the average of a binary visibility test over
// these directions is already the cosine-weighted integral `diffAmbient` wants. No per-sample weight
// and no normalisation beyond the count. rtDiscSample supplies the disc point, so this shares the
// shadow ray's NESTED sequence -- raising the ray count refines the estimate rather than replacing it.
//
// NO FRAME TERM, DELIBERATELY, and this is a hard constraint rather than an oversight: every RT
// sampler in this file is a pure function of pixel position because the gate oracle compares exact
// pixels across runs. A frame-varying sample set would decorrelate beautifully, denoise well, and
// make 181 gates non-reproducible. The cost is that the noise here is FIXED per pixel -- stable and
// non-flickering, but structured, and it does not average away over time the way a jittered one
// would. If this ever needs to be smoother, the answer is more rays or a spatial filter, not a
// frame counter.
//
// A MISS IS SKY. The ray is a plain occlusion query against the opaque lane with
// ACCEPT_FIRST_HIT_AND_END_SEARCH: unlike the sun ray directly above, which walks past glass
// accumulating transmittance, this one only asks whether anything is in the way at all.
// FORWARD-DECLARED because the voxel helpers are defined with the cone tracer, roughly nine hundred
// lines below this ray, and HLSL needs a declaration before the call. Moving their definitions up
// instead would drag the whole clipmap block above the RT section for one caller's benefit.
float3 voxelUVW(float3 wp);
bool   insideVolume(float3 uvw);

// What one hemisphere gather learned. `sky` and `bounce` are written only under AVER_AO_UNIFIED and
// are zero otherwise, so a caller that ignores them gets exactly the old behaviour and exactly the
// old cost -- the branches that fill them are compiled out, not merely unread.
struct AverAmbientTraced {
    float  open;    // fraction of samples that reached the sky. THE ONLY FIELD the legacy path uses.
    float3 sky;     // mean radiance of the sky actually visible, per direction rather than averaged
    float3 bounce;  // mean radiance of whatever stopped the rays that did not escape
};

AverAmbientTraced rtAmbientTraced(float3 wpos, float3 N, float2 pixel, uint rays) {
    const uint n = clamp(rays, 1u, 32u);
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
    // ROTATION SHARED ACROSS A TILE, NOT PER PIXEL -- a coherence dial, and the only lever the
    // measurements actually support for this ray.
    //
    // WHY NOT "USE SIMD": this already runs under SIMT, 32-64 lanes in lockstep, and the cost is not
    // arithmetic. Adding three more SUN rays costs 0.05 ms while removing the shadow ray entirely
    // saves 11.95 ms -- rays 2-4 are nearly free because they all point at the sun and walk the same
    // BVH nodes, so ray one pays the traversal and the rest ride its cache. This ray is the opposite:
    // cosine-distributed over the hemisphere, so every lane in a wave descends a different part of
    // the tree and the wave runs at the speed of its unluckiest lane. That is why it costs 5.37 ms
    // for one ray where the sun gets four for less.
    //
    // Sharing the azimuth across an NxN tile makes neighbouring lanes trace near-PARALLEL rays, which
    // touch the same nodes and the same cache lines. The price is correlated noise inside a tile
    // rather than independent noise per pixel -- acceptable for a low-frequency term like ambient
    // occlusion, and exactly the wrong trade for anything with sharp detail.
    //
    // HARDWARE AGNOSTIC BY CONSTRUCTION: plain HLSL, no wave intrinsics, no vendor extension, no
    // capability gate. Shader Execution Reordering would attack the same problem more directly and is
    // deliberately NOT used -- ReorderThread is vendor-specific and DXR 1.2's MaybeReorderThread needs
    // a tier this engine does not require, so either would make this path exist on some GPUs only.
    //
    // 1 REPRODUCES THE PREVIOUS BEHAVIOUR EXACTLY (floor(pixel/1) == pixel), so this is a dial with a
    // no-op setting rather than a rewrite, and the tile size can be measured rather than argued.
    // RUNTIME, from gAmbientParams.y, with the compile-time define as the floor. It was a #define
    // alone, set nowhere, which made the one lever this ray's own comment names unmeasurable -- the
    // same shape of gap as the ray count beside it.
    // max() rather than a branch: 1 reproduces the per-pixel rotation exactly (floor(p/1) == floor(p)),
    // so there is no "off" case to test for.
    const float aoTileEdge = max(gAmbientParams.y, AVER_AO_COHERENCE_TILE);
    const float2 aoTile = floor(pixel / aoTileEdge);
    // A DIFFERENT DIRECTION EVERY FRAME, and without this the history pair below buys nothing.
    //
    // rtHash(aoTile) is a function of the PIXEL and nothing else, so before this term every pixel
    // traced the same hemisphere direction on every frame it ever rendered. That makes the estimator
    // deterministic rather than noisy -- and a deterministic wrong answer is exactly what temporal
    // accumulation cannot fix, because averaging ten identical samples returns the sample. A pixel
    // whose one ray happened to escape read fully open forever, next to neighbours whose ray happened
    // to hit, which is the white salt-and-pepper on shadowed surfaces: not noise that settles, a
    // fixed per-pixel pattern that no weight could touch. It is also why raising the accumulation
    // weight from 0.9 to 0.95 measured no change at all -- there was nothing different to average.
    //
    // THE GOLDEN ANGLE, the same 2.39996323 the tiled shadow path already spends for the same reason:
    // successive frames land far apart on the disc rather than drifting, so ~10 frames of history is
    // ~10 well-spread samples instead of one sample counted ten times.
    const float ang0 = rtHash(aoTile) * 6.2831853 + gRtHistParams.z * 2.39996323;
    float3 T, B;
    // Matches ptBasis/averBasis convention: any orthonormal pair about N will do, since the disc
    // sample is rotated by ang0 anyway.
    const float3 up = abs(N.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);

    AverAmbientTraced res;
    res.open = 0.0; res.sky = float3(0, 0, 0); res.bounce = float3(0, 0, 0);
    [loop] for (uint k = 0; k < n; ++k) {
        const float2 d = rtDiscSample(k, ang0);
        // Malley: lift the disc point onto the hemisphere. r^2 + z^2 == 1 by construction, so the
        // result is unit length without a normalize() that would perturb the very cosine
        // distribution being relied on.
        const float3 dir = T * d.x + B * d.y + N * sqrt(saturate(1.0 - dot(d, d)));

        RayDesc r;
        // Offset along BOTH the normal and the ray, as the shadow ray does: the normal alone leaves
        // acne at grazing angles, and a cosine-weighted set contains plenty of grazing directions.
        r.Origin    = wpos + N * bias + dir * bias;
        r.Direction = dir;
        r.TMin      = bias;
        // BOUNDED, NOT 1 km -- AND MEASURED TO CHANGE NOTHING HERE, which is written down because
        // the obvious assumption is that it should. The sun ray above runs to 100000 because a
        // shadow caster can be any distance away and missing one is a visibly wrong hard edge.
        // Ambient is a smooth local quantity, so bounding it LOOKS like the optimisation. On Sponza
        // it is worth exactly nothing: 65.716 ms bounded against 65.715 ms unbounded, and a
        // byte-identical viewport mean. Every ray in an enclosed scene hits something long before
        // 40 m, so there was no traversal to save.
        //
        // THE COST IS RAY INCOHERENCE, NOT RAY LENGTH. These directions are cosine-distributed over
        // the hemisphere, so neighbouring lanes walk unrelated parts of the BVH -- unlike the sun
        // rays, which all point one way and traverse together. That is why four of these cost far
        // more than the four sun rays already in the frame, and why the lever is ray COUNT or
        // amortisation, not distance.
        //
        // Kept anyway, because an unbounded ambient ray is wrong in principle and this scene simply
        // cannot show it: gVoxelParams.z is Settings::giMaxDistance, already the authored answer to
        // "how far does indirect light travel here" and already where traceCone stops, so the traced
        // estimate and the cone estimate it replaces measure the same extent of world.
        r.TMax      = max(gVoxelParams.z, 1.0);

        // THE TEMPLATE ARGUMENT LOST ACCEPT_FIRST_HIT_AND_END_SEARCH, and the flag moved to the
        // TraceRayInline call where the mode needs it. Two reasons, and the first applies even at
        // AVER_AO_UNIFIED 0:
        //
        //   averRtProceedSolid is written against exactly one RayQuery template argument, and says so
        //   ("A second flag set would need its own copy -- HLSL has no way to be generic over the
        //   flags"). This ray was the ONE ray in this file still calling a bare Proceed() with no
        //   candidate test, and the bug that left is the OPPOSITE of the obvious guess -- it is not
        //   that a leaf occluded with its bounding rectangle instead of its cutout. Alpha-masked
        //   instances stay in the OPAQUE lane but carry FORCE_NON_OPAQUE (VoxiRenderer.cpp, "NOT THE
        //   MASK, only the flags"), and this file's own mask comment states the consequence: "a
        //   RayQuery meeting one does not commit it, so a single Proceed()+CommittedStatus traversal
        //   would stop AT the pane and miss whatever is behind it." CommittedStatus then reads
        //   COMMITTED_NOTHING and the sample was counted as HAVING REACHED THE SKY.
        //
        //   So a leaf did not over-occlude, it made ambient light LEAK: any direction whose traversal
        //   surfaced a cutout candidate first was scored fully open no matter what solid wall stood
        //   behind it, brightening exactly the enclosed, foliage-heavy interiors this ray was added
        //   to darken. Running the shared loop resolves each candidate against its alpha and keeps
        //   traversing, which is what every other ray in this file already did.
        //
        //   And under AVER_AO_UNIFIED the flag has to go anyway: "any hit will do" cannot tell you
        //   WHICH surface you hit, and the hit is half the point.
        //
        // Runtime flags OR with template flags, so the non-unified path below is the same query it
        // has always been, just spelled at the call rather than in the type.
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
#if AVER_AO_UNIFIED
        q.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE_ALL, r);
#else
        q.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, AVER_RT_MASK_OPAQUE_ALL, r);
#endif
        averRtProceedSolid(q);

        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            res.open += 1.0;
#if AVER_AO_UNIFIED
            // The sky this sample actually saw. averSkyRadianceCheap is the SH reconstruction, ~20
            // ALU, against skyColor's 32-step march -- affordable once per hemisphere sample in a way
            // the march never could be. It cannot represent the sun disc, which is correct here: the
            // sun is a separate direct term and must not be gathered twice.
            res.sky += averSkyRadianceCheap(dir);
#endif
        }
#if AVER_AO_UNIFIED
        else {
            // FIRST-BOUNCE RADIANCE AT AN EXACT HIT, which is the same quantity traceCone estimates
            // and a strictly better estimate of it. A cone widens with distance and climbs mips, and
            // a coarse mip averages across a thin wall -- that is the leak that makes enclosed
            // shadows read too bright. This ray stopped ON the geometry, so there is nothing to
            // average across; mip 0 at the hit point is what the cone was approximating all along.
            //
            // Clamped exactly as every other consumer of this volume clamps it: the volume can hold
            // a physically large value and gVoxelParams.y scales it further, so AVER_VOX_MAXRAD is
            // the same firefly bound traceCone's callers already apply.
            const float3 hp  = r.Origin + dir * q.CommittedRayT();
            const float3 uvw = voxelUVW(hp);
            // OUTSIDE THE VOLUME CONTRIBUTES NOTHING, deliberately, and this is the one place the
            // unified path is DARKER than the cone gather rather than brighter: traceCone treats
            // leaving the volume as "unoccluded" and hands back the sky, which is why shrinking the
            // GI volume once made shadows brighter instead of darker. A ray that hit real geometry
            // outside the voxelised region is genuinely occluded; crediting it with sky would
            // reintroduce the leak this whole change exists to remove.
            if (insideVolume(uvw))
                res.bounce += min(gVoxelTex.SampleLevel(gVoxelSamp, uvw, 0).rgb * gVoxelParams.y,
                                  AVER_VOX_MAXRAD);
        }
#endif
    }

    // ALL THREE DIVIDE BY n, NOT BY THEIR OWN HIT COUNTS. These are Monte Carlo estimates of
    // hemisphere integrals, so a sample that missed contributes zero to `bounce` and a sample that
    // hit contributes zero to `sky` -- that is the estimate, not a gap in it. Dividing `sky` by the
    // miss count instead would return the mean brightness of the visible sky and silently drop the
    // occlusion, which is the very thing being measured.
    const float inv = 1.0 / (float)n;
    res.open   *= inv;
    res.sky    *= inv;
    res.bounce *= inv;
    return res;
}

// The scalar-only form every existing caller wants, kept so that turning AVER_AO_UNIFIED on does not
// require every call site to change at once. Under the define the extra fields are computed and
// discarded here, which is exactly the waste this whole idea is about -- so a call site that means to
// use them must call rtAmbientTraced directly.
float rtSkyOcclusion(float3 wpos, float3 N, float2 pixel, uint rays) {
    return rtAmbientTraced(wpos, N, pixel, rays).open;
}

// Reprojects wpos through LAST frame's camera to sample the ray-traced shadow history. False when
// unusable: off-screen, behind last frame's near plane, or a DISOCCLUSION (stored depth disagrees
// with the reprojected texel). `hist`/`velocityPx` are untouched on a false return.
// `velocityPx` is the reprojection's screen-space displacement from `pixel`, so the caller can
// discount a moved sample -- the depth test alone can't tell "same surface, slid since last frame".
// NDC -> LAST frame's VIEWPORT rect (gSceneViewport), not [0,1] of the whole texture: the editor
// docks the 3D view in a sub-rect, and plain ndc*0.5+0.5 lands on the wrong texel otherwise. Caught
// by the shadow-rt/penumbra-rt gates (moved by a full shade), not shipped.
// NEAREST, not bilinear -- REVERSED from an earlier version, because who calls it changed: that
// version was read every frame in sync, so a bilinear tap blended in something nearly identical.
// This tiled path is deliberately out of sync, so bilinear mixed in a stale neighbour across a
// penumbra: MEASURED to converge to a stable but WRONG value (a 4x4 tile settled at 46,46,47 vs a
// true ~23,27,32, unmoved 300-1500 frames). Nearest guarantees reading this pixel's own last write.
bool rtReprojectHistory(float3 wpos, float2 pixel, out float hist, out float2 velocityPx) {
    hist = 0.0;
    velocityPx = 0.0;
    float4 clip = mul(float4(wpos, 1.0), gPrevViewProj);
    // Taken HERE, above every early-out, so the derivative is never evaluated in divergent flow --
    // see the tolerance below for what it is for.
    const float dzdx = ddx(clip.w);
    const float dzdy = ddy(clip.w);
    if (clip.w <= 1e-4) return false;
    float3 ndc = clip.xyz / clip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0) return false;
    float texW, texH;
    gRtShadowHist.GetDimensions(texW, texH);
    float2 px = gSceneViewport.xy +
                float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewport.zw;
    // FLOOR, not round: for a static pixel, `px` lands on index+0.5, exactly the tie round() breaks
    // inconsistently (round-half-to-even) by index parity, sending ~half of pixels to the wrong
    // neighbour. floor() of that centre is exactly `index`, matching how the WRITE side indexes
    // (gRtShadowHistOut[uint2(pixel)] truncates SV_Position) -- MEASURED: round() converged to a
    // stable but wrong value (penumbra-rt settled at 78,75,70 vs a true ~23,27,32).
    int2 texel = int2(floor(px));
    if (any(texel < 0) || texel.x >= (int)texW || texel.y >= (int)texH) return false;

    const float2 stored = gRtShadowHist.Load(int3(texel, 0));   // x = visibility, y = linear depth
    // clip.w is the expected depth (same as VSMain's o.pos.w for wpos, through LAST frame's camera).
    // Comparing it to the stored depth catches a disocclusion a screen-position check alone can't: a
    // silhouette edge can reproject onto an already-populated texel at a completely different depth.
    // THE TOLERANCE FOLLOWS THE DEPTH GRADIENT, and a flat 3% is what made grazing surfaces speckle.
    //
    // `stored.y` is the depth recorded at the TEXEL WE LANDED ON, up to half a texel from where this
    // pixel actually reprojected to. On a surface seen face-on that is a few millimetres and 3%
    // covers it easily. On one seen EDGE-ON -- a floor stretching away, the base of a column -- half
    // a texel of screen space is metres of depth, so the test rejects a history that was perfectly
    // good, the pixel falls back to its raw ONE-RAY estimate, and at one ray that estimate is
    // BINARY. A field of pixels each independently choosing 0 or 1 is precisely salt and pepper, and
    // it appears exactly where the geometry is grazing, which is where the user reported it.
    //
    // (|ddz/dx| + |ddz/dy|) is how much depth legitimately changes across one pixel here, so
    // allowing two pixels of it turns "3% of the depth" into "3% of the depth, plus whatever this
    // surface's own slope makes unavoidable". Face-on geometry has a gradient near zero and keeps
    // exactly the old tolerance, so this loosens the test only where it was wrong.
    //
    // ddx/ddy ARE SAFE HERE, and that is worth stating because rtShadowSpatial carries a warning
    // about the opposite case: derivatives in DIVERGENT flow difference against a lane that never
    // ran. The branch above this call is on gAmbientParams.x, a constant-buffer value, so it is
    // uniform across the wave -- and the gradient is taken before any of this function's own
    // early-outs.
    const float tol = max(clip.w, stored.y) * 0.03 + 1.0 + (abs(dzdx) + abs(dzdy)) * 2.0;   // 3% relative, +1cm floor at grazing distances
    if (abs(clip.w - stored.y) > tol) return false;

    hist = stored.x;
    velocityPx = px - pixel;
    return true;
}

// The AMBIENT twin of rtReprojectHistory, against gAoHist. A near-copy on purpose: HLSL below
// SM 6.6 cannot take a Texture2D parameter, and the alternative -- folding both into one function
// behind a flag -- would put a branch in the hot path of every pixel to save nine lines. The
// arithmetic is deliberately IDENTICAL, including floor() over round() and the 3% depth tolerance;
// see the original for why each of those is what it is. Change one, change both.
bool rtReprojectAo(float3 wpos, float2 pixel, out float hist, out float2 velocityPx) {
    hist = 0.0;
    velocityPx = 0.0;
    float4 clip = mul(float4(wpos, 1.0), gPrevViewProj);
    // Taken HERE, above every early-out, so the derivative is never evaluated in divergent flow --
    // see the tolerance below for what it is for.
    const float dzdx = ddx(clip.w);
    const float dzdy = ddy(clip.w);
    if (clip.w <= 1e-4) return false;
    float3 ndc = clip.xyz / clip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0) return false;
    float texW, texH;
    gAoHist.GetDimensions(texW, texH);
    float2 px = gSceneViewport.xy +
                float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewport.zw;
    int2 texel = int2(floor(px));
    if (any(texel < 0) || texel.x >= (int)texW || texel.y >= (int)texH) return false;
    const float2 stored = gAoHist.Load(int3(texel, 0));   // x = openness, y = linear depth
    // THE TOLERANCE FOLLOWS THE DEPTH GRADIENT, and a flat 3% is what made grazing surfaces speckle.
    //
    // `stored.y` is the depth recorded at the TEXEL WE LANDED ON, up to half a texel from where this
    // pixel actually reprojected to. On a surface seen face-on that is a few millimetres and 3%
    // covers it easily. On one seen EDGE-ON -- a floor stretching away, the base of a column -- half
    // a texel of screen space is metres of depth, so the test rejects a history that was perfectly
    // good, the pixel falls back to its raw ONE-RAY estimate, and at one ray that estimate is
    // BINARY. A field of pixels each independently choosing 0 or 1 is precisely salt and pepper, and
    // it appears exactly where the geometry is grazing, which is where the user reported it.
    //
    // (|ddz/dx| + |ddz/dy|) is how much depth legitimately changes across one pixel here, so
    // allowing two pixels of it turns "3% of the depth" into "3% of the depth, plus whatever this
    // surface's own slope makes unavoidable". Face-on geometry has a gradient near zero and keeps
    // exactly the old tolerance, so this loosens the test only where it was wrong.
    //
    // ddx/ddy ARE SAFE HERE, and that is worth stating because rtShadowSpatial carries a warning
    // about the opposite case: derivatives in DIVERGENT flow difference against a lane that never
    // ran. The branch above this call is on gAmbientParams.x, a constant-buffer value, so it is
    // uniform across the wave -- and the gradient is taken before any of this function's own
    // early-outs.
    const float tol = max(clip.w, stored.y) * 0.03 + 1.0 + (abs(dzdx) + abs(dzdy)) * 2.0;
    if (abs(clip.w - stored.y) > tol) return false;
    hist = stored.x;
    velocityPx = px - pixel;
    return true;
}

// AMBIENT OCCLUSION, ACCUMULATED OVER TIME instead of over rays.
//
// At one ray per pixel this estimator is `open = hit ? 0 : 1` -- a binary mask, the noisiest thing
// a Monte Carlo estimate can be, and its MEAN is already correct (a probe read the same value at 1
// ray and at 4, which is exactly why no probe could see the problem: a probe cannot measure
// variance). The previous answer was four rays sharing one azimuth across a 4x4 tile, which buys
// five quantisation levels instead of two and correlates them into visible BLOCKS -- the tile
// comment above says as much, and names this function's approach as the real fix.
//
// Accumulating gets the sample count from FRAMES rather than from rays: at weight 0.9 the history
// is an exponential average over ~10 of them, so one ray behaves like ten and the tile can go back
// to 1 (independent noise per pixel, then averaged away). That is strictly cheaper than what it
// replaces -- one coherent-free ray instead of four -- which is what lets sky occlusion come down
// off the Epic-only rung it was priced onto.
//
// THE VELOCITY TERM AND THE 32-PIXEL BUDGET ARE THE SHADOW PATH'S, DELIBERATELY. This history is
// also exactly one frame old and also guarded by a depth test, so the thing velocity still has to
// pay for is sub-texel alignment -- bounded at half a texel at any speed by the nearest-neighbour
// lookup. Ambient occlusion tolerates a stale sample better than a shadow edge does, not worse.
// THE SPATIAL HALF OF THE DENOISER, WHICH THE AMBIENT TERM NEVER HAD.
//
// The sun shadow is denoised twice: rtShadowTemporal accumulates over frames, and rtShadowSpatial
// then filters across pixels on read. Sky occlusion got only the first of those -- so its residual
// variance had nowhere to go but the screen, and it is ~6/7 of this renderer's measured frame-to-
// frame flicker (still camera, two captures one frame apart: 0.158% of channels past 8 codes with
// it on, 0.026% with it off, 0.000% with ray tracing off entirely).
//
// A NEAR-COPY OF rtShadowSpatial, ON PURPOSE AND FOR THE REASON ALREADY DOCUMENTED at rtReprojectAo:
// HLSL below SM 6.6 cannot take a Texture2D as a parameter, and folding the two behind a flag would
// put a branch in a hot per-pixel path to save a page. The arithmetic is deliberately IDENTICAL --
// the same plane-distance rejection expressed in last frame's depth, the same Gaussian with
// sigma = radius/2, the same normal crease test, the same velocity taper. Change one, change both.
//
// It reads gAoHist -- LAST frame's openness -- exactly as the shadow filter reads last frame's
// visibility, which is what makes the reprojected centre the right place to gather around.
float rtAoSpatial(float centre, float3 wpos, float3 N, float2 pixel, float curDepth) {
    const int radius = (int)gRtDenoiseParams.x;
    if (radius <= 0 || gRtHistParams.y < 0.5 || gRtDenoiseParams.w < 0.5) return centre;

    float texW, texH;
    gAoHist.GetDimensions(texW, texH);

    const float4 pclip  = mul(float4(wpos, 1.0), gPrevViewProj);
    const float  pdepth = pclip.w;
    // Hoisted out of the branch for the same undefined-derivative reason the shadow filter spells
    // out: ddx/ddy in divergent flow differences against a lane that never ran.
    const float  dpdx   = ddx(pdepth);
    const float  dpdy   = ddy(pdepth);
    const float  cdx    = ddx(curDepth);
    const float  cdy    = ddy(curDepth);

    float2 centrePx = pixel;
    bool   reproj   = false;
    if (pdepth > 1e-4) {
        const float3 pndc = pclip.xyz / pdepth;
        if (pndc.z >= 0.0 && pndc.z <= 1.0) {
            centrePx = gSceneViewport.xy +
                       float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5) * gSceneViewport.zw;
            reproj = true;
        }
    }
    const int2 base = int2(floor(centrePx));

    const float planeDepth = reproj ? pdepth : curDepth;
    const float dzdx       = reproj ? dpdx   : cdx;
    const float dzdy       = reproj ? dpdy   : cdy;

    const float sigma  = max((float)radius * 0.5, 0.5);
    const float inv2s2 = 1.0 / (2.0 * sigma * sigma);

    float acc = centre;
    float wsum = 1.0;
    // NEIGHBOURHOOD STATISTICS, gathered alongside the blur, for the clamp below. Deliberately
    // EXCLUDING the centre: the whole question is whether the centre disagrees with its neighbours,
    // and a statistic contaminated by the outlier cannot answer it.
    float nSum = 0.0, nSum2 = 0.0, nCount = 0.0;
    [loop] for (int oy = -radius; oy <= radius; ++oy) {
        [loop] for (int ox = -radius; ox <= radius; ++ox) {
            if (ox == 0 && oy == 0) continue;
            const int2 t = base + int2(ox, oy);
            if (any(t < 0) || t.x >= (int)texW || t.y >= (int)texH) continue;
            const float2 st = gAoHist.Load(int3(t, 0));
            const float predicted = planeDepth + dzdx * (float)ox + dzdy * (float)oy;
            const float tol = max(abs(predicted), 1.0) * 0.02 + 1.0;
            if (abs(st.y - predicted) > tol) continue;
#if AVER_GBUFFER_HISTORY
            const float3 nb = gGBufNormalHist.Load(int3(t, 0)).xyz * 2.0 - 1.0;
            if (dot(N, nb) < 0.5) continue;
#endif
            const float w = exp(-(float)(ox * ox + oy * oy) * inv2s2);
            acc  += st.x * w;
            wsum += w;
            nSum += st.x; nSum2 += st.x * st.x; nCount += 1.0;
        }
    }

    // ---- THE CLAMP, WHICH IS WHAT ACTUALLY REMOVES SALT AND PEPPER ---------------------------
    //
    // The Gaussian above is the wrong instrument for an isolated extreme and always was: a LINEAR
    // filter SPREADS an impulse over its kernel rather than removing it, turning one bright pixel
    // into a bright smudge and lowering the peak just enough to look like progress. MEASURED on a
    // still frame as deviation from the 3x3 MEDIAN -- the quantity "salt and pepper" actually names,
    // and a different one from the frame-to-frame flicker measured earlier: 0.248% of pixels past 64
    // codes with ray tracing on, against 0.019% with it off. Raising the SHADOW ray count to 4
    // changed it to 0.242%, i.e. not the shadow; switching traced sky occlusion off took it to
    // 0.082%, i.e. mostly this term.
    //
    // Clamping to the neighbourhood's own mean +/- k*sigma is what temporal antialiasing has used
    // against fireflies for years, and it works here for the same reason: an impulse is BY
    // DEFINITION a value its neighbours do not share, so a statistic taken from the neighbours
    // bounds it without knowing anything about the scene. A genuine feature -- a real shadow edge,
    // a real crease -- is supported by neighbours on one side and survives, which is why this is not
    // simply a blur with extra steps.
    //
    // 2 SIGMA, and the two failure directions are not symmetric. Tighter starts eating real
    // gradients, which is the artefact this renderer has already paid for once by over-filtering.
    // Looser stops catching anything: the outliers here are many sigma out, not marginal. The
    // minimum tap count is what stops a pixel with two surviving neighbours -- a silhouette, where
    // the depth and normal rejects have thrown most of the kernel away -- from being clamped to a
    // statistic built out of nothing.
    float clamped = centre;
    if (nCount >= 3.0) {
        const float mean  = nSum / nCount;
        const float sigma = sqrt(max(nSum2 / nCount - mean * mean, 0.0));
        // A FLOOR UNDER SIGMA. A perfectly flat neighbourhood has zero variance, and clamping to
        // [mean, mean] there would erase the centre's own legitimate detail along with its noise.
        const float k = 2.0 * max(sigma, 0.02);
        clamped = clamp(centre, mean - k, mean + k);
        // The blurred average carried the UNCLAMPED centre at weight 1; correct it in place rather
        // than re-running the loop, so the filter and the clamp agree about what the centre is.
        acc += clamped - centre;
    }

    const float velPx = reproj ? length(centrePx - pixel) : 0.0;
    const float trust = gRtDenoiseParams.z > 0.0 ? saturate((3.0 - velPx) * gRtDenoiseParams.z) : 1.0;
    // CLAMPED FIRST, THEN BLENDED, so the impulse is gone before the linear filter ever sees it --
    // and so that a run with the blur turned down to nothing still gets the clamp, which is the part
    // that actually addresses this artefact.
    return saturate(lerp(clamped, acc / wsum, saturate(gRtDenoiseParams.y) * trust));
}

float rtSkyOcclusionTemporal(float3 wpos, float3 N, float2 pixel, uint rays) {
    const float fresh = rtSkyOcclusion(wpos, N, pixel, rays);
    // gRtDenoiseParams.w, NOT gRtHistParams.x, and the difference matters: the shadow and
    // reflection pairs exist at every ray-tracing tier, but this one is allocated only where the
    // ray is actually traced -- High and Epic. At Low and Medium the slots are genuinely absent,
    // and touching a null UAV is undefined rather than merely wasteful. Returning the fresh trace
    // is also the correct answer there: with no history there is nothing to accumulate against.
    if (gRtDenoiseParams.w < 0.5) return fresh;

    const float curDepth = mul(float4(wpos, 1.0), gViewProj).w;
    float vis = fresh;
    float histV = 0.0;
    float2 velocityPx = 0.0;
    if (gRtHistParams.y > 0.5 && rtReprojectAo(wpos, pixel, histV, velocityPx)) {
        const float t      = saturate(length(velocityPx) / 32.0);
        // THE SHADOW PATH'S OWN WEIGHTS, and 0.95 was tried rather than assumed. Doubling the
        // effective sample count to ~20 moved dark-region local roughness from 0.4509 to 0.4486 --
        // nothing, because what is left in a still frame is Sponza's stone TEXTURE, not sampling
        // noise, and that floor is the same in every configuration measured (the old four-ray tile
        // scored 0.3929 on it). Deeper history is not free -- it is lag on a disocclusion the depth
        // test does not catch -- so the value that buys nothing is not the one to ship.
        // 0.97, NOT 0.9, AND THE PARAGRAPH ABOVE IS OUT OF DATE RATHER THAN WRONG. It records that
        // 0.95 "measured no change", and that was true when it was measured: what remained in a
        // still frame was Sponza's stone TEXTURE, not sampling noise, so a deeper history had
        // nothing left to average. The sky irradiance calibration removed that premise -- the
        // ambient this occlusion multiplies is now eight times larger, so the same relative variance
        // is eight times more visible, and this term rather than the texture is the floor.
        // ~33 frames of history against ~10.
        const float weight = lerp(0.97, 0.5, t);
        vis = lerp(fresh, histV, weight);
    }
    // The ACCUMULATED value, not the fresh one -- writing `fresh` here would restart the average
    // every frame and buy nothing, the same trap the shadow path documents.
    //
    // AND RAW, NEVER FILTERED, which is the same single most important line the shadow path has:
    // feeding the spatially filtered result back into the history turns this into an IIR filter
    // whose artefacts compound every frame. The filter applies on the way OUT, below.
    gAoHistOut[uint2(pixel)] = float2(vis, curDepth);
    return rtAoSpatial(vis, wpos, N, pixel, curDepth);
}

// The SPATIAL denoiser: average this pixel's shadow with its neighbours' from the history texture,
// weighted by how well each neighbour's surface agrees with this one's.
// READS LAST FRAME'S TEXTURE SAFELY: t6/u2 are different ping-ponged textures, t6 resting in
// ShaderResource for the whole colour pass, so this is a plain load needing no barrier -- unlike the
// TEMPORAL path above, which reuses a value up to 2^(2*tileBits) frames old AS the answer, here the
// centre always contributes its own fresh trace and a failing neighbour is DROPPED, never substituted.
// AVERAGING IS AN ESTIMATE, NOT A BLUR: rtShadow jitters the ray origin across the pixel's own
// footprint, so neighbours on one flat receiver already sample different points of the same surface.
float rtShadowSpatial(float centre, float3 wpos, float3 N, float2 pixel, float curDepth) {
    const int radius = (int)gRtDenoiseParams.x;
    if (radius <= 0 || gRtHistParams.y < 0.5) return centre;

    float texW, texH;
    gRtShadowHist.GetDimensions(texW, texH);

    // GATHER AROUND WHERE THIS PIXEL WAS LAST FRAME, NOT WHERE IT IS NOW: gRtShadowHist is last
    // frame's texture. MEASURED: within one code of the sixteen-ray answer while still, but under a
    // six-degree wobble drifted 12-32 codes darker, worsening with radius -- a kernel walking off
    // its own surface. Same arithmetic as rtReprojectHistory (last frame's VIEWPORT RECT, FLOOR not
    // round) -- inherited landmines, not re-derived ones.
    // WHERE THIS PIXEL WAS LAST FRAME, AND AT WHAT DEPTH. Hoisted out of the branch and evaluated
    // unconditionally because the plane gradients below are ddx/ddy of pdepth: a derivative taken in
    // divergent flow differences against a lane that never ran the branch, which is undefined and
    // reads as noise along exactly the silhouettes this filter is trying to respect.
    const float4 pclip  = mul(float4(wpos, 1.0), gPrevViewProj);
    const float  pdepth = pclip.w;
    const float  dpdx   = ddx(pdepth);
    const float  dpdy   = ddy(pdepth);
    const float  cdx    = ddx(curDepth);
    const float  cdy    = ddy(curDepth);

    float2 centrePx = pixel;
    bool   reproj   = false;
    if (gRtHistParams.y > 0.5 && pdepth > 1e-4) {
        const float3 pndc = pclip.xyz / pdepth;
        if (pndc.z >= 0.0 && pndc.z <= 1.0) {
            centrePx = gSceneViewport.xy +
                       float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5) * gSceneViewport.zw;
            reproj = true;
        }
    }
    const int2 base = int2(floor(centrePx));

    // Plane-distance rejection, not a raw depth delta: centre depth + its screen-space gradient
    // defines the receiver's plane, so a neighbour on it is kept regardless of depth while one at
    // the same depth on a different surface is dropped. A plain |dz| test fails on a grazing floor.
    //
    // EXPRESSED IN LAST FRAME'S DEPTH WHENEVER THE GATHER REPROJECTED, and that is the motion fix.
    // Every tap below loads gRtShadowHist, whose .y is LAST frame's depth, but this predictor was
    // built from curDepth -- THIS frame's. Standing still the two are the same number and the
    // mismatch cannot be seen; under camera motion they diverge with the yaw rate, so the 2%
    // tolerance is spent asymmetrically across the kernel, accepting taps on the receding side and
    // rejecting them on the approaching one. A one-sided accept set is a biased average, which is
    // why the artefact is a region shifting brightness rather than noise, and why it worsened with
    // radius: the further the tap, the larger the frame-to-frame depth disagreement.
    const float planeDepth = reproj ? pdepth : curDepth;
    const float dzdx       = reproj ? dpdx   : cdx;
    const float dzdy       = reproj ? dpdy   : cdy;

    // A GAUSSIAN falloff (used to give every accepted neighbour weight 1.0 -- a flat kernel rings in
    // frequency response, a visible square halo around a bright feature). sigma = radius/2, matched
    // to rtReflectionSpatial's kernel so two filters over the same geometry don't disagree in shape.
    //
    // LIVE AT EVERY REAL TIER -- a comment that used to sit here claimed the opposite (shipped inert
    // because rtShadowDenoiseForQuality returns 0 at every tier). FALSE: Voxi.cpp returns 2 for
    // Low/Medium/High, 1 for Epic (only Off/unknown return 0), and Medium is the DEFAULT tier.
    //
    // CAUGHT BY THE ORACLE, NOT BY READING: re-recording the gates moved 15 values, three in every
    // ray-tracing-capable configuration including WARP:
    //   penumbra-rt   33,39,48 -> 62,64,66      partially occluded, much brighter
    //   rt-penumbra   91,88,85 -> 96,93,89      partially occluded, brighter
    //   ms-rt-gi      52,19,13 -> 38,15,11      same sunVis feeding the GI-composited path
    // shadow-rt/shadow-ms-rt moved NOT AT ALL, the signature of a reweighting: full umbra pins every
    // tap at 0, while a penumbra's old flat kernel dragged the estimate toward far neighbours that
    // the Gaussian now discounts (0.135 at distance 2, 0.018 at the corner) -- bit-identical across
    // hardware and WARP, over a record and an independent verify pass.
    //
    // THE LESSON IS THE FALSE CLAIM, NOT THE FILTER: an "inert" change is one nobody reviews visually.
    const float sigma  = max((float)radius * 0.5, 0.5);
    const float inv2s2 = 1.0 / (2.0 * sigma * sigma);

    float acc = centre;
    float wsum = 1.0;
    [loop] for (int oy = -radius; oy <= radius; ++oy) {
        [loop] for (int ox = -radius; ox <= radius; ++ox) {
            if (ox == 0 && oy == 0) continue;
            const int2 t = base + int2(ox, oy);
            if (any(t < 0) || t.x >= (int)texW || t.y >= (int)texH) continue;
            const float2 st = gRtShadowHist.Load(int3(t, 0));
            // What this neighbour's depth WOULD be if it sat on the centre's plane -- in the same
            // frame st.y was recorded in, see planeDepth.
            const float predicted = planeDepth + dzdx * (float)ox + dzdy * (float)oy;
            const float tol = max(abs(predicted), 1.0) * 0.02 + 1.0;
            if (abs(st.y - predicted) > tol) continue;
#if AVER_GBUFFER_HISTORY
            // THE CREASE TERM: a depth-plane test can't see a normal DISCONTINUITY -- two surfaces at
            // similar depth (a wall meeting a floor) can pass it and bleed shadow across a crease.
            // Sampled at LAST frame's normal (same tap `t`): comparing to THIS frame's would mix
            // moments, same reason gRtShadowHist itself reads last frame.
            const float3 nb = gGBufNormalHist.Load(int3(t, 0)).xyz * 2.0 - 1.0;
            // HARD REJECT: a disagreeing normal is the wrong surface, not noise. cos(60 deg), not
            // tighter: averPackNormalRoughness quantises to RGB10A2, and tighter would reject a FLAT
            // surface's own quantisation noise as a crease.
            if (dot(N, nb) < 0.5) continue;
#endif
            const float w = exp(-(float)(ox * ox + oy * oy) * inv2s2);
            acc  += st.x * w;
            wsum += w;
        }
    }

    // gRtDenoiseParams.y: blend weight of the filtered value. At 0 taps still run (radius is a
    // runtime constant, not optimised away) but this returns `centre` exactly -- the
    // cost-measurement configuration.
    //
    // TAPERED BY THIS FILTER'S OWN REPROJECTION VELOCITY, which is what stops the shadows flickering
    // while the camera moves. MEASURED on Sponza, comparing a still camera against a six-degree
    // wobble AT A MATCHED POSE (both at sin(phase)=0, so the only difference is motion history),
    // reading the population of differing pixels rather than a mean -- a mean is 0.01 of a code here
    // because the affected pixels are a few percent of the frame changing a lot:
    //
    //   radius 0 (filter off)   9-32 codes: 0.000%   33+: 0.000%   max delta   4
    //   radius 1                9-32 codes: 0.187%   33+: 0.002%   max delta  60
    //   radius 2 (the default)  9-32 codes: 2.445%   33+: 0.177%   max delta  95
    //   radius 2, 8 rays/pixel  9-32 codes: 0.205%   33+: 0.003%   max delta  64
    //
    // Read those together and the mechanism is not staleness and not the plane test -- it is THE
    // KERNEL'S SAMPLE SET SLIDING. rtShadow jitters by rtHash(pixel), anchored to the pixel index,
    // so the raw one-ray estimate at a given pose is the SAME every frame: with the filter off, a
    // moving camera and a still one agree to 4 codes. Switch the filter on and the gather centre
    // reprojects, so each frame averages a DIFFERENT 25 taps out of one static noise field, and a
    // spatially noisy but temporally stable estimate becomes a temporally unstable one. That is why
    // it scales with radius (more reach, more resampling) and why eight rays largely fix it (a
    // quieter field to resample). Eight rays are not affordable; this is.
    //
    // NOT A DISOCCLUSION TEST: rtReprojectHistory already rejects taps whose depth disagrees. This
    // is about the taps that are all individually VALID and still average to a different number.
    // Full strength below half a pixel of drift, so a stationary camera is bit-for-bit unchanged and
    // the recorded gates do not move; gone by three, where a fresh unfiltered trace is the more
    // stable answer anyway.
    // gRtDenoiseParams.z is the taper's falloff rate, and 0 means NO TAPER (the pre-existing
    // behaviour) rather than "taper instantly to nothing" -- a knob whose off position silently
    // disables the whole filter is the kind of default that gets measured by accident.
    const float velPx = reproj ? length(centrePx - pixel) : 0.0;
    const float trust = gRtDenoiseParams.z > 0.0 ? saturate((3.0 - velPx) * gRtDenoiseParams.z) : 1.0;
    return lerp(centre, acc / wsum, saturate(gRtDenoiseParams.y) * trust);
}

// The PRIMARY sun-shadow call only -- rtReflection's inner rtShadow() call stays one ray with no
// footprint, frameJitter=0, and never touches history: blending in a reflected surface's shadow
// would overwrite this pixel's history with a value unrelated to the primary ray's estimate.
// gRtHistParams.w is the pixels-per-ray TILE EDGE as a bit count (0 = off, every pixel traces every
// frame). At 0 it now ACCUMULATES: the fresh trace is blended against reprojected history and the
// blended value is what gets stored. It used to be bit-for-bit rtShadow() alone with a plain write to
// keep the buffer live for when tiling turned on -- which meant that at every shipped tier (the tile
// edge is 1 everywhere) every previous measurement was discarded and the one-ray estimate stood
// alone. Tiling still cuts ray COUNT; blending is what makes one ray behave like ten.
// Splits a tinted visibility into the scalar the denoiser filters and the colour it does not.
//
// WHY THE HISTORY DIDN'T GROW: occlusion is binary/noisy (what the filters smooth); a medium's tint
// is smooth and near-noise-free, so filtering it buys nothing -- the scalar keeps the existing
// RG32Float history bit-for-bit and the tint rides unfiltered on top. RGBA32Float would double two
// ~56MB buffers; RGBA16Float would drop the depth channel (clip-space w in cm, tens of thousands on
// terrain) to 8cm precision where the disocclusion test reads it. Not worth it for an unfiltered value.
float averShadowLum(float3 v) { return dot(v, float3(0.2126, 0.7152, 0.0722)); }

// The normalised colour of a tinted visibility. White when there is effectively nothing to tint --
// a fully occluded pixel has no medium colour to speak of, and dividing by its luminance would be a
// 0/0 that spreads NaN through the filter.
float3 averShadowTint(float3 v, float lum) {
    return (lum > 1e-4) ? (v / lum) : float3(1.0, 1.0, 1.0);
}

float3 rtShadowTemporal(float3 wpos, float3 N, float3 L, float2 pixel, float3 dpx, float3 dpy, uint rays) {
    // gRtHistParams.x is 0 when t6/u2 aren't bound this frame (VoxiRenderer::beginShadowHistory) --
    // an unbound slot is Tier 1 null-filled, so touching either would hit a null descriptor.
    if (gRtHistParams.x < 0.5) return rtShadow(wpos, N, L, pixel, dpx, dpy, rays, 0.0);

    // THIS frame's linear depth at wpos (mul(wp, gViewProj).w, as VSMain computes it) since the
    // shadow pass has no depth buffer to read back. Written alongside visibility below so next
    // frame's rtReprojectHistory always sees a fresh depth, not one stale since this pixel's last turn.
    const float curDepth = mul(float4(wpos, 1.0), gViewProj).w;

    const uint tileBits = (uint)gRtHistParams.w;
    if (tileBits == 0u) {
        // THE SAME GOLDEN-ANGLE FRAME JITTER THE TILED PATH BELOW PASSES, and passing 0.0 here
        // instead is what made this whole branch's accumulation buy nothing.
        //
        // rtShadow builds its sample angle as `ang0 = rtHash(pixel) * 2pi + frameJitter`. With a
        // jitter of zero that is a pure function of the PIXEL: one fixed direction on the sun's
        // disc, traced identically every frame for the life of the process. The exponential average
        // below then averages ten copies of one sample, which is one sample -- so the comment that
        // used to say the estimate "converges toward the many-ray answer" was describing something
        // this branch could not do. At rays = 1, which is every shipped tier below Epic, a partially
        // occluded pixel therefore reported a single hard 0/1-ish sample forever.
        //
        // MEASURED at the rt-penumbra probe (gates.ps1:296, --sun-angle 8.0, 1204 frames), against a
        // converged reference of 16 rays at tile 1 -- which is motion-invariant and so is a real
        // ground truth, 139,136,133 still and 137,134,133 moving:
        //
        //   tile 1, 1 ray (EVERY SHIPPED TIER)   88,92,99    error -51
        //   tile 2, 1 ray                        132,129,128 error  -7
        //   tile 4, 1 ray                        137,134,132 error  -2
        //
        // The amortised paths are an order of magnitude more accurate than the default, and the only
        // thing they do differently to the estimate is pass this jitter. The other tell is in the
        // same table: tile 1 reads 126 with the camera MOVING against 88 still -- motion was
        // accidentally supplying the decorrelation, because reprojection walked the estimate across
        // pixels with different rtHash values. An estimator that gets better when you shake the
        // camera is not sampling.
        //
        // WHY THE FRAME INDEX IS SAFE HERE, given this file's standing rule that RT sampling must be
        // a pure function of pixel position for the gate oracle: the rule exists so a probe is
        // reproducible, and frameIdx is deterministic -- a --frames N run always ends on the same
        // index, so the same run gives the same pixel. It is only a hazard when indexed INTO the
        // radical-inverse sequence on the tiled path, where a pixel traces every 2^(2*tileBits)
        // frames and that power-of-two stride pins the sequence's low bits (measured, and recorded
        // as a disproven fix). This is an angular offset, not a sequence index, and here the stride
        // is 1. Recorded RT gate values move; they are re-recorded, not suppressed.
        const float frameJitter = (float)((uint)gRtHistParams.z) * 2.39996323;
        const float3 fresh3 = rtShadow(wpos, N, L, pixel, dpx, dpy, rays, frameJitter);
        const float  fresh   = averShadowLum(fresh3);
        const float3 tint    = averShadowTint(fresh3, fresh);

        // TEMPORAL ACCUMULATION, and the reason it belongs on THIS branch specifically.
        //
        // The early-return above conflates two separate things under one flag. Tiling is RAY
        // AMORTISATION -- trace one pixel in N and reuse the rest -- and switching it off correctly
        // means "trace every pixel every frame". It does not mean "throw away every previous
        // measurement", but that is what this branch did: it wrote the raw one-ray trace to history
        // and never read history back, so at rtPixelsPerRayTile 1 (every shipped tier) the entire
        // reprojection path below was dead code and each frame's estimate stood alone.
        //
        // One ray per pixel is a very noisy estimate, and MEASURED, that noise is what the shadows
        // were flickering with: the spatial filter gathers 25 taps around a centre that REPROJECTS,
        // so a moving camera averages a different subset of one static, pixel-anchored noise field
        // every frame. Sponza, still camera against a six-degree wobble at a matched pose, counting
        // pixels rather than averaging them -- filter off, the moving and still images agree to 4
        // codes; radius 1, 60; radius 2 (the default), 95, with 2.4% of the frame past 9 codes. The
        // raw trace is temporally STABLE and spatially noisy; the filter converts the one into the
        // other. Eight rays per pixel cut it ~12x, which names the cause as variance and prices the
        // obvious fix out of reach.
        //
        // Accumulating instead buys the same variance reduction for one texture read: at rest the
        // 0.9 weight is an exponential average over ~10 frames, so the estimate converges toward the
        // many-ray answer rather than resampling noise. rtReprojectHistory's depth test is what
        // keeps it honest across a disocclusion, and the velocity term discounts history as the
        // reprojection gets less trustworthy -- both already written and, until now, unreachable.
        //
        // THE ACCUMULATED VALUE IS WHAT GETS STORED, not the fresh trace. That is what makes it
        // compound; writing `fresh` here would restart the average every frame and buy nothing.
        float vis = fresh;
        float histV = 0.0;
        float2 velocityPx = 0.0;
        // A FAR LOOSER VELOCITY BUDGET THAN THE TILED PATH BELOW, and the difference is not a tuning
        // preference. There, history can be 2^(2*tileBits) frames old, so its own staleness compounds
        // with motion and 6 pixels is a fair place to stop trusting it. Here it is always EXACTLY one
        // frame old, and the thing that can go wrong -- landing on a different surface -- is caught by
        // rtReprojectHistory's depth test, not by the velocity. What velocity still costs is
        // sub-texel alignment, and that error is bounded by half a texel at ANY speed because the
        // lookup is nearest-neighbour. Reusing 6 here throttled the weight to 0.1 at the six-degree
        // wobble's 24.7 px/frame, i.e. switched accumulation off in exactly the case it was added for.
        if (gRtHistParams.y > 0.5 && rtReprojectHistory(wpos, pixel, histV, velocityPx)) {
            // 0.9 is an exponential average over ~1/(1-w) = 10 frames, the effective sample count
            // that makes a one-ray trace behave roughly like a ten-ray one, falling to 0.5 (two
            // samples) as the reprojection stretches. BOTH ENDS MEASURED rather than reasoned:
            // 0.8 at the far end holds MORE history and came out worse, not better (9-32 codes
            // 0.951% against 0.902%, worst delta 108 against 92), because past a point the extra
            // depth buys less than the sub-texel misalignment it drags in.
            const float t      = saturate(length(velocityPx) / 32.0);
            const float weight = lerp(0.9, 0.5, t);
            vis = lerp(fresh, histV, weight);
        }
        gRtShadowHistOut[uint2(pixel)] = float2(vis, curDepth);
        // FILTERED HERE TOO -- this branch is what Medium (the DEFAULT tier) runs, and used to
        // return `fresh` unfiltered, leaving the default's hard 0/1 shadow untouched: the penumbra
        // probe read an unchanged 61,59,59 at every radius, which is what caught it. Two returns,
        // two call sites -- easy for a later edit to drop one again.
        return rtShadowSpatial(vis, wpos, N, pixel, curDepth) * tint;
    }

    // Which pixel in its tileBits x tileBits tile traces THIS frame -- a bitmask, not a modulo, since
    // tileBits is always a power of two (setPixelsPerRayTile). Over 2^(2*tileBits) frames every pixel
    // gets one staggered turn, so a whole tile is never traced together (which would show as blocks).
    const uint frameIdx = (uint)gRtHistParams.z;
    const uint tileMask = (1u << tileBits) - 1u;
    const uint turnMask  = (1u << (2u * tileBits)) - 1u;
    const uint localIdx  = (((uint)pixel.y & tileMask) << tileBits) | ((uint)pixel.x & tileMask);
    const bool myTurn    = localIdx == (frameIdx & turnMask);

    float hist = 0.0;
    float2 velocityPx = 0.0;
    const bool haveHist = gRtHistParams.y > 0.5 && rtReprojectHistory(wpos, pixel, hist, velocityPx);

    float vis;
    // WHITE WHEN NOT TRACED: a reused-history frame has no fresh colour and the history stores none,
    // so tint falls back to untinted. Only reachable on the amortised path, dead at every shipped
    // tier (rtPixelsPerRayTile is 1 everywhere).
    float3 tint = float3(1.0, 1.0, 1.0);
    if (myTurn || !haveHist) {
        // The golden-angle offset spends a DIFFERENT sample of the sequence each turn, so
        // 2^(2*tileBits) turns converge toward what that many spatial rays give in one frame
        // (rtDiscSample). frameJitter=0 would repeat the same ray forever -- why tiling needs this.
        const float frameJitter = (float)frameIdx * 2.39996323;
        const float3 vis3 = rtShadow(wpos, N, L, pixel, dpx, dpy, rays, frameJitter);
        vis  = averShadowLum(vis3);
        tint = averShadowTint(vis3, vis);
        if (haveHist) {
            // ADAPTIVE blend weight: a barely-moved reprojection is close to a repeated measurement
            // and earns high weight; one that moved several pixels likely samples the wrong part of
            // the surface even past the depth test, so it's trusted less the faster it moves. The
            // budget SHRINKS as the tile grows -- a bigger tile's history is staler even before
            // motion, so the same velocity should discount it sooner.
            const float budget = max(6.0 - 1.5 * (float)tileBits, 1.0);
            const float t = saturate(length(velocityPx) / budget);
            const float weight = lerp(0.9, 0.1, t);
            vis = lerp(vis, hist, weight);
        }
    } else {
        // Not this pixel's turn, and reprojection is valid: reuse it outright. No ray at all this
        // frame -- this is the actual saving tiling exists for.
        vis = hist;
    }

    // WRITE THE RAW VALUE, NEVER FILTERED -- the single most important line in the denoiser. Feeding
    // a filtered value back into gRtShadowHistOut makes this an IIR filter (a temporal filter by
    // another name) whose artefacts compound every frame. The filter applies on READ, below; writing
    // it here to "save work" is the bug, not the optimisation.
    gRtShadowHistOut[uint2(pixel)] = float2(vis, curDepth);
    return rtShadowSpatial(vis, wpos, N, pixel, curDepth) * tint;
}

// Traces one reflection ray and shades what it hits. Global, unlike the cone tracer it replaced,
// which stopped dead at the voxel volume's boundary and popped objects in/out of reflections. Shades
// one bounce of Lambertian light plus sky ambient from the hit's own albedo -- no textures, no
// second bounce, so a reflection reads slightly flatter than the surface seen directly.
//
// ---- THE LOBE: why `rough` is a parameter, and what it fixed ----
// Used to trace R exactly, faking roughness by refusing reflections above 0.5 and fading the rest
// toward flat sky at 2x roughness -- a quarter-rough surface got a half-strength mirror mixed with
// half flat sky, neither a glossy reflection.
// Fix: `rough` widens the ray into a cone, the same way gRtParams.x widens the shadow ray into the
// sun's disc (same rtDiscSample sequence, per-pixel rotation, frameJitter decorrelation).
// tan(cone) = rough*rough is the GGX alpha (standard remap; the lobe's half-angle tangent IS alpha
// for small angles), not tuned: roughness 0.05 gets tan=0.0025 (2.5cm over a 10m reflection, still a
// mirror), roughness 0.5 gets tan=0.25 (~14 degrees, a real blur).
// ONE RAY, NOT A SWEEP: widening the cone costs VARIANCE, paid down by rtReflectionTemporal's
// history and rtReflectionSpatial's roughness-scaled kernel, not by a second ray (which would double
// the cost of the most expensive term in the path).
// `frameJitter` is added to the per-pixel rotation, zero for callers with no history -- same
// contract as rtShadow's. rtHash stays pure per-pixel so a single-frame capture is bit-exact.
float3 rtReflection(float3 wpos, float3 N, float3 R, float3 L, float2 pixel, float rough,
                    float frameJitter, out bool hit) {
    hit = false;

    // At rough=0, tanCone is exactly zero and the arithmetic reduces to `dir = R` bit-for-bit, so a
    // mirror surface (chrome ball, glass) is untouched by this change.
    const float tanCone = rough * rough;
    float3 dir = R;
    if (tanCone > 0.0) {
        float3 up = abs(R.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
        float3 T  = normalize(cross(up, R));
        float3 B  = cross(R, T);
        // Sample index 0 always (not per-pixel): the sequence's RADIUS depends on the index, so a
        // varying index would give neighbours systematically different cone widths and bias the
        // spatial filter's average across lobes. One fixed radius, rotated per pixel and frame, keeps
        // every sample on the same ring of the same lobe.
        const float2 d = rtDiscSample(0, rtHash(pixel) * 6.2831853 + frameJitter);
        dir = normalize(R + (T * d.x + B * d.y) * tanCone);
        // A cone wide enough to swing below the surface would reflect the receiver into itself.
        // Clamp back into the upper hemisphere rather than drop the sample -- dropping biases the
        // estimate dark exactly where the lobe is widest.
        if (dot(dir, N) <= 0.0) dir = normalize(dir - N * (dot(dir, N) - 1e-3));
    }

    RayDesc r;
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
    r.Origin    = wpos + N * bias;
    r.Direction = dir;
    r.TMin      = bias;
    r.TMax      = 100000.0;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // Opaque lane only (AVER_RT_MASK_OPAQUE_ALL): this ray wants solid surfaces, and excludes the
    // translucent lane by mask rather than by flag.
    //
    // FORCE_OPAQUE USED TO BE A PROVABLE NO-OP HERE and is now provably WRONG. The old proof was
    // that createBlas marks every geometry OPAQUE and only the translucent lane -- excluded by this
    // mask -- was ever un-opaqued. Alpha-masked instances broke that: they stay in the OPAQUE lane
    // (they occlude, they cast shadow) and are non-opaque so a ray can see the holes in them. With
    // the flag on, the hardware committed the leaf card and never asked.
    q.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE_ALL, r);
    averRtProceedSolid(q);
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return 0.0;

    RtInstance inst = gRtInstances[q.CommittedInstanceID()];
    uint tri = inst.firstIndex + q.CommittedPrimitiveIndex() * 3;
    uint i0 = inst.firstVertex + gRtIndices[tri + 0];
    uint i1 = inst.firstVertex + gRtIndices[tri + 1];
    uint i2 = inst.firstVertex + gRtIndices[tri + 2];

    float2 bary = q.CommittedTriangleBarycentrics();
    float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float3 nObj = normalize(gRtVerts[i0].nrm * w.x + gRtVerts[i1].nrm * w.y + gRtVerts[i2].nrm * w.z);
    // Rotation only: row-vector, direction = vector * upper 3x3. No inverse transpose (needed for
    // non-uniform scale) since reflections here are of rigid instances.
    float3 nWS = normalize(mul(float4(nObj, 0.0), inst.objectToWorld).xyz);
    // `dir`, NOT `R`: these used to be the same vector. Testing facing / marching along R once the
    // lobe widens would place the hit where the ray never went -- lit from the wrong side, sampled
    // at the wrong distance, worst where the cone is widest.
    if (dot(nWS, dir) > 0.0) nWS = -nWS;   // face the ray, so a back-facing hit is not lit from behind

    float3 hitPos = wpos + dir * q.CommittedRayT();
    // Whether the SUN reaches the reflected surface -- without this every reflection glows as if
    // nothing could shadow it. Seeded from the PIXEL with NO footprint: seeding from hitPos.xy would
    // make the pattern depend on ray distance, which is what's most likely to differ between a
    // hardware adapter and WARP. ONE ray, not the full disc -- the single largest saving in the ray
    // path (was 1 reflection + 4-ray disc, 5 rays where 2 do now; a reflected penumbra isn't
    // resolvable in a one-bounce mirror image anyway). float3: a tinted medium tints this too.
    float3 shadow = rtShadow(hitPos, nWS, L, pixel, float3(0,0,0), float3(0,0,0), 1u, 0.0);

    // LAMBERTIAN EXITANT RADIANCE, and the /PI is the whole point. averGroundRadiance's reference:
    //     E = sunIrradiance*ndl + PI*skyRadiance*ambient;   return albedo * E / PI;
    // so radiance is albedo*sunIrradiance*ndl/PI (sun) + albedo*skyRadiance*ambient (sky, PI cancels).
    // Omitting the divide made every SUNLIT reflection 3.14x too bright (shaded ones stayed correct,
    // reading as an exposure bug) -- moved the lit ray-traced gates from 94,27,14 to 131,58,40.
    // THE WHITE FURNACE DOES NOT CATCH THIS: it turns the sun off, testing only the ambient half.
    // A furnace with a sun is a second mode worth having.
    float3 direct = averSunRadiance() * saturate(dot(nWS, L)) * shadow / PI;
    float3 ambient = averSkyIrradiance(nWS) * gAmbient.r;
    hit = true;

    // `inst.albedo` is the per-draw FLAT colour, fine when everything was flat but reading as "the
    // reflection is fake" once directly-viewed surfaces carry real base-colour maps. Everything
    // needed is already here, so a reflected surface resolves its UV the same way a direct one does.
    float3 reflAlbedo = inst.albedo;
#ifdef AVER_RT_BINDLESS
    {
        const RtMaterial rmat = gRtMaterials[inst.materialIndex];
        const float2 meshUV = gRtVerts[i0].uv * w.x + gRtVerts[i1].uv * w.y + gRtVerts[i2].uv * w.z;
        const float2 ruv    = averRtSurfaceUV(rmat, inst, hitPos, nWS, meshUV);

        // A CONE FOOTPRINT, NOT MIP 0: mip-0 sampling here was a THROUGHPUT problem, not just
        // quality -- proper mip selection took the whole frame 11.96 -> 8.14ms on ElectricDreams. No
        // screen derivatives on a reflection ray, but tanCone*CommittedRayT gives a footprint, so
        // rougher reflections read wider mips for free.
        const float  rad = max(tanCone, 1e-3) * q.CommittedRayT();
        // The same up-vector trick used above to build the cone basis, applied to the hit normal
        // instead of R: two orthogonal in-plane directions are all averRtUvGrad needs.
        const float3 rup = abs(nWS.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
        const float3 rt  = normalize(cross(rup, nWS));
        const float3 rb  = cross(nWS, rt);
        float2 rgx, rgy;
        averRtUvGrad(rmat, inst, nWS,
                     gRtVerts[i0].pos, gRtVerts[i1].pos, gRtVerts[i2].pos,
                     gRtVerts[i0].uv,  gRtVerts[i1].uv,  gRtVerts[i2].uv,
                     rt * rad, rb * rad, rgx, rgy);
        reflAlbedo *= averRtSampleSlot(rmat, 0, ruv, rgx, rgy, float4(1, 1, 1, 1)).rgb
                    * rmat.baseColorFactor.rgb;
    }
#endif
    return reflAlbedo * (direct + ambient);
}

// How far a view ray travels INSIDE a volume before something stops it, in centimetres --
// averVolumeTransmittance needs a path length, and a blended surface has no idea how thick it is.
// The old fluid shader guessed with `depthCm / max(abs(V.z), 0.15)` from a CONSTANT floor height
// (FluidShaders.hpp), right only on a box's TOP face; on a side face at pitch 0 the clamp gave an
// ~8.7m path, so a pool's side came out fully opaque and bright (the PTTest pit's blown-out white
// slab). A ray doesn't have to guess; it measures.
//
// COMMITS EVERY CANDIDATE EXCEPT A FAILED CUTOUT, which is what FORCE_OPAQUE used to do here and
// why it stood: translucent instances sit as FORCE_NON_OPAQUE, so ordinary traversal would stop AT
// a candidate uncommitted. averRtProceedSolid commits any non-alpha-masked candidate on sight, so
// this ray still answers "what is the first thing along it, of any kind" -- with the one refinement
// that a hole in a cutout material is no longer a thing.
//
// BOTH LANES, because both can end the path: the volume's own BACK FACE, or an opaque object inside
// it (a rock, the pool floor). Nearest-of-the-two makes absorption respond to real geometry instead
// of an authored box height.
//
// Returns 0 when nothing is hit; averVolumeTransmittance reads that as "no path through the medium"
// and answers with full transmission -- an unbounded volume should absorb nothing until given a
// boundary, not absorb infinitely.
// Is a real backdrop bound? A null-filled Texture2D reports zero dimensions -- the only signal
// available in-shader, avoiding a spare cbuffer component for something the descriptor already
// tells us. Tested, not assumed: forcing the null case (MSAA on) and confirming glass falls back
// instead of going black is part of this feature's verification.
bool averBlendBackdropValid(out float2 invSize) {
    uint w = 0, h = 0;
    gBlendBackdrop.GetDimensions(w, h);
    const bool ok = (w > 0u && h > 0u);
    invSize = ok ? (1.0 / float2((float)w, (float)h)) : float2(0.0, 0.0);
    return ok;
}

// THE VOLUME COMPOSITE, background as a CORRECTION not a replacement. Want physically:
// final = specular + diffuse*alpha + dst*T*(1-alpha). Hardware premultiplied blend gives
// final = src.rgb + dst*(1-src.a); setting src.a = alpha and solving:
//     src.rgb = specular + diffuse*alpha + bg * (1 - alpha) * (T - 1)
// THE LAST TERM IS THE TRICK: (T-1) is negative, subtracting exactly the light the medium absorbed
// per channel -- what one blend alpha can't express. Hardware still adds the REAL destination after.
// T==1 makes the correction exactly zero (bit-for-bit identity, what the no-absorption regression
// check leans on). STACKED TRANSLUCENCY DEGRADES GENTLY: `bg` is the scene copied BEFORE any
// translucent draw (stale for a second layer, used only in the correction, base composite still
// blends against the true `dst`) -- replacing the background outright would let a nearer pane erase
// a farther one, and glass over water makes that not hypothetical.
// Falls back to the scalar composite when no backdrop is bound -- the honest answer, not black.
// WHERE THE BACKGROUND IS READ FROM, once the surface bends it. Absorption decides what COLOUR
// survives the medium; refraction decides where it comes FROM -- gIor's first reader (uploaded and
// unread since it was added). Returns the UV to sample the backdrop at; gGiParams.y is the mode,
// .z strength, .w edge fade (see Settings::refractionMode).
float2 averRefractedBackdropUV(AverSurface s, float3 wpos, float thicknessCm,
                               float2 invSize, float2 screenPos, out bool tir) {
    tir = false;
    const float2 uv0  = screenPos * invSize;
    const uint   mode = (uint)(gGiParams.y + 0.5);
    if (mode == 0u || gGiParams.z <= 0.0) return uv0;

    // WHICH WAY THE LIGHT IS CROSSING decides everything below. eta = index LEFT / index ENTERED:
    // air->medium is 1/n, medium->air is n -- reversed, TIR becomes unreachable (needs eta > 1).
    // GATED ON gCameraMedium, NOT s.backFace -- already cost this engine once. material_prelude.hlsl's
    // post-mortem: a TIR override gated on backFace "turned every pane of glass into a dark slab at
    // 41 degrees off normal", since backFace is also true for a two-sided pane's FAR surface seen
    // from outside (where Snell forbids TIR), indistinguishable from a pixel shader at the time.
    // gCameraMedium.x is now computed CPU-side against the volume's bounds, which a pixel can't
    // recover; glass is twosided=1 and never a medium by that test, so it can't reach this branch.
    const float  ior      = max(gIor, 1.0001);
    const bool   eyeInside = gCameraMedium.x > 0.5;
    const float  eta      = eyeInside ? ior : (1.0 / ior);
    float3 R = refract(-s.V, s.N, eta);
    if (dot(R, R) < 1e-6) {
        // refract() returns 0 to say "no transmitted ray exists". Normalising it would be a NaN.
        if (!eyeInside) return uv0;   // from outside this is unreachable; sampling straight through
                                      // stays the honest fallback rather than a fabricated bend.
        // GENUINE TIR: past the critical angle (48.75 degrees at n=1.33) the underside of the water
        // stops being a window and becomes a MIRROR, showing the pool floor instead of the sky.
        // Reflect about the same normal and let the machinery below project it like a refracted target.
        tir = true;
        R = reflect(-s.V, s.N);
    }

    // Where the bent ray leaves the medium: one thickness along the BENT path, not the straight one
    // -- why this needs ray-measured thickness rather than an authored constant.
    float3 target = wpos + R * max(thicknessCm, 0.0);

#if AVER_RT
    // RAY-TRACED: follow the bent ray to what it ACTUALLY reaches and project THAT, unlike the
    // screen-space mode below which can only offset within the image already captured. Still reads
    // colour from the backdrop rather than shading the hit (a second full material eval on the
    // frame's most expensive pass) -- an off-screen/occluded hit falls back to whatever's there.
    if (mode >= 2u) {
        RayDesc rr;
        rr.Origin = target;
        rr.Direction = R;
        rr.TMin = 0.0;
        rr.TMax = 100000.0;
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> rq;
        rq.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE_ALL, rr);
        averRtProceedSolid(rq);   // cutouts, not cards -- see averRtProceedSolid
        if (rq.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            target = target + R * rq.CommittedRayT();
    }
#endif

    const float4 clip = mul(float4(target, 1.0), gViewProj);
    if (clip.w <= 1e-4) return uv0;               // behind the eye: nothing sensible to sample
    const float2 ndc = clip.xy / clip.w;

    // NDC -> THE VIEWPORT RECT, NOT [0,1] OF THE WHOLE TARGET -- the bug that made refraction look
    // like a wrecked image, not a bent one. The editor docks the 3D view in a SUB-RECT, so NDC maps
    // to gSceneViewportCur while uv0 (screenPos*invSize) is a full-target UV; plain ndc*0.5+0.5 mixes
    // the spaces, an affine error that walks the pane's image off-screen near the right edge -- the
    // black block in the glass rail.
    // MEASURED by forcing target = wpos (answer HAD to be uv0 exactly): 100% of the rail's pixels
    // still landed >60px away, proving this was the projection, not refraction (an 8cm pane can't
    // bend 60px). The file already knew: rtReprojectHistory and three other sites do this conversion
    // correctly, one warning the plain form "lands every reprojection on the wrong texel" --
    // refraction was written later and missed it.
    //
    // A ZERO-WIDTH RECT means no viewport was reported this frame -- sample straight through, the
    // same fallback used for TIR and a target behind the eye.
    if (gSceneViewportCur.z <= 0.0 || gSceneViewportCur.w <= 0.0) return uv0;
    const float2 pxR = gSceneViewportCur.xy +
                       float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewportCur.zw;
    float2 uvR = pxR * invSize;

    // THE EDGE FADE, not cosmetic: an offset walking off-screen samples nothing meaningful, and one
    // onto a foreground object shows that object through the glass. Fades to zero near the border
    // instead of a hard wrong pixel.
    // FADED AGAINST THE VIEWPORT RECT, NOT THE TARGET: outside the 3D view the backdrop holds
    // whatever the rest of the frame is -- "still on the part the camera drew" is the real test.
    const float fadePx = max(gGiParams.w, 0.0);
    float edge = 1.0;
    if (fadePx > 0.0) {
        const float2 vpMin = gSceneViewportCur.xy * invSize;
        const float2 vpMax = (gSceneViewportCur.xy + gSceneViewportCur.zw) * invSize;
        const float2 d = min(uvR - vpMin, vpMax - uvR);   // to the nearest viewport border, in UV
        edge = saturate(min(d.x, d.y) / fadePx);
    }
    return lerp(uv0, uvR, saturate(gGiParams.z) * edge);
}

float4 averBlendedOutputBackdrop(AverSurface s, float3 diffuse, float3 specular, float3 T,
                                 float2 screenPos, float3 wpos, float thicknessCm) {
    float2 invSize;
    if (!averBlendBackdropValid(invSize))
        return averBlendedOutputVolume(s, diffuse, specular, T);

    // TWO SAMPLES, AND THE SECOND IS WHAT MAKES THE CORRECTION CANCEL. The HARDWARE adds
    // dst*(1-alpha) after this returns; a correction only works if it subtracts EXACTLY that, at uv0.
    //
    // The single-sample version subtracted the REFRACTED sample instead, so nothing cancelled and the
    // residue (dst - bgRefracted) went NEGATIVE wherever the bent ray landed on something BRIGHTER
    // than what's really behind -- a negative channel through the tonemap reads as a hue, not dark.
    // That's what the magenta blocks in the pool were. MEASURED: 41773 magenta pixels at the
    // 45-degree pool camera with refraction on, 0 with --refraction 0, 0 after this fix.
    //
    // The algebra, with a == alpha:
    //     final = specular + diffuse*a + (bgRefr*T - bgStraight)*(1-a) + dst*(1-a)
    //   and dst IS bgStraight, so the last two collapse and leave
    //     final = specular + diffuse*a + bgRefr*T*(1-a)
    //   the bent background, absorbed over the path, behind a Fresnel-weighted surface. Refraction
    //   OFF makes bgRefr == bgStraight, collapsing to bg*(T-1)*(1-a) (bit-identical to before); T==1
    //   leaves (bgRefr - bgStraight)*(1-a) -- pure bending, so refraction with no volume isn't
    //   silently wrong any more.
    //
    // dst == bgStraight only for the FIRST translucent surface over a pixel; a second one behind
    // glass composites against a backdrop not yet containing the first -- the known cost of capturing
    // the backdrop once per frame. Bounded to that overlap, unlike the misregistration above.
    bool tir = false;
    const float2 uvR   = averRefractedBackdropUV(s, wpos, thicknessCm, invSize, screenPos, tir);
    const float2 uv0   = screenPos * invSize;
    const float3 bgR   = gBlendBackdrop.SampleLevel(gMaterialSampler, uvR, 0).rgb;
    const float3 bg0   = gBlendBackdrop.SampleLevel(gMaterialSampler, uv0, 0).rgb;
    const float  alpha = saturate(s.alpha);

    // TIR IS NOT A WINDOW WITH A DIFFERENT UV -- treating it as one made it black. Past the critical
    // angle nothing transmits, so everything the eye gets is REFLECTED; the window form's (1-alpha)
    // background weight, with alpha pushed toward 1 by view Fresnel at TIR's grazing angles, was
    // multiplying the mirror by roughly zero. MEASURED underwater at 25 degrees: mirrored region
    // read 0.59 mean against the 18.8 of the pit wall it should show -- dark because cancelled, not
    // because the pool is dark.
    //
    // alpha=1 is correct HERE, unlike material_prelude.hlsl's post-mortem case: that slammed alpha
    // to 1 on a PANE seen from outside (where Snell forbids TIR) with no reflected image, leaving a
    // dark slab. This fires only when gCameraMedium says the eye is inside a single-sided volume,
    // handing back the reflected scene as the surface's radiance.
    if (tir) return float4(specular + bgR * T, 1.0);

    return float4(specular + diffuse * alpha + (bgR * T - bg0) * (1.0 - alpha), alpha);
}

float averVolumeThickness(float3 wpos, float3 N, float3 viewDir) {
    RayDesc r;
    // SAME bias as the reflection ray, same reason: starting exactly on the shaded surface re-hits
    // it at t~0. Pushed along the VIEW direction, not N: this ray heads INTO the surface, and
    // offsetting along the normal would push it out of the volume being measured.
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
    r.Origin    = wpos + viewDir * bias;
    r.Direction = viewDir;
    r.TMin      = 0.0;
    r.TMax      = 100000.0;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // BOTH LANES, and every candidate committed on sight EXCEPT a failed cutout: this measures the
    // distance to the far side of a medium, so a pane is a real boundary here. averRtProceedSolid
    // commits any non-alpha-masked candidate, which preserves exactly what FORCE_OPAQUE did.
    q.TraceRayInline(gScene, RAY_FLAG_NONE,
                     AVER_RT_MASK_OPAQUE_ALL | AVER_RT_MASK_TRANSLUCENT, r);
    averRtProceedSolid(q);
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) return 0.0;
    return q.CommittedRayT() + bias;
}

// Reprojects wpos through LAST frame's camera to sample the reflection history. False when unusable:
// off-screen, behind last frame's near plane, a recorded miss (stored.a <= 0), or a disocclusion --
// same test as rtReprojectHistory, against the reflection's own depth channel.
// A MISS IS NEVER REPROJECTED, the one real difference from the shadow case: sky-by-direction is
// cheap and highly VIEW dependent, so a stale miss would show the wrong patch of sky through a still
// surface. A real hit depends on the surface and a mostly-static light, not viewing angle.
bool rtReprojectReflection(float3 wpos, float2 pixel, out float3 hist, out float2 velocityPx) {
    hist = 0.0;
    velocityPx = 0.0;
    float4 clip = mul(float4(wpos, 1.0), gPrevViewProj);
    if (clip.w <= 1e-4) return false;
    float3 ndc = clip.xyz / clip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0) return false;
    float texW, texH;
    gRtReflHist.GetDimensions(texW, texH);
    float2 px = gSceneViewport.xy +
                float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewport.zw;
    int2 texel = int2(floor(px));   // see rtReprojectHistory for why floor, not round
    if (any(texel < 0) || texel.x >= (int)texW || texel.y >= (int)texH) return false;

    const float4 stored = gRtReflHist.Load(int3(texel, 0));
    if (stored.a <= 0.0) return false;   // a miss was recorded there -- nothing to reuse
    const float tol = max(clip.w, stored.a) * 0.03 + 1.0;
    if (abs(clip.w - stored.a) > tol) return false;

    hist = stored.rgb;
    velocityPx = px - pixel;
    return true;
}

// The SPATIAL denoiser for REFLECTIONS -- there was none before: reprojected/blended in time but
// never filtered in space, harmless while the ray was a deterministic mirror but not once
// rtReflection widens into a lobe (shipping the lobe alone trades wrong-but-clean for right-but-noisy).
//
// STRUCTURE IS rtShadowSpatial's (reprojected gather centre, plane-distance rejection -- read that
// first). Differs in three ways:
//  1. RADIUS FROM ROUGHNESS, not a host constant: a fixed width either blurs a mirror or
//     under-filters a rough surface. Roughness 0 returns centre untouched, no texel loaded.
//  2. A MISSED NEIGHBOUR IS SKIPPED, not counted black: gRtReflHist's miss sentinel is a NEGATIVE
//     alpha, and averaging it in would drag every reflected edge toward black.
//  3. NO LUMINANCE WEIGHT: SVGF's colour-similarity term needs a per-pixel VARIANCE estimate this
//     engine doesn't track (history is (rgb, depth), no second moment); without variance a luminance
//     weight preserves exactly the noise it's meant to remove. Geometry weights only, for now.
// The AVER_GBUFFER_HISTORY crease term in the tap loop is copied from rtShadowSpatial verbatim.
// `N` is a new parameter for that crease term only -- rtReflectionTemporal's own normal, unchanged.
float3 rtReflectionSpatial(float3 centre, float3 wpos, float3 N, float2 pixel, float curDepth,
                           float rough, float dzdx, float dzdy) {
    // Radius tracks the lobe (tan(cone)=rough^2 grows quadratically, this grows linearly --
    // deliberately conservative: too wide smears detail, too narrow just leaves noise for the
    // temporal history). Capped at 3 (7x7 gather): cost is quadratic in radius.
    const int radius = (int)clamp(floor(rough * 6.0), 0.0, 3.0);
    if (radius <= 0 || gRtHistParams.y < 0.5) return centre;

    float texW, texH;
    gRtReflHist.GetDimensions(texW, texH);

    // Gather around where this pixel WAS last frame (gRtReflHist is last frame's). Same arithmetic
    // as rtReprojectHistory/rtShadowSpatial, including their two landmines: last frame's VIEWPORT
    // rect not [0,1], and floor not round.
    float2 centrePx = pixel;
    const float4 pclip = mul(float4(wpos, 1.0), gPrevViewProj);
    if (pclip.w > 1e-4) {
        const float3 pndc = pclip.xyz / pclip.w;
        if (pndc.z >= 0.0 && pndc.z <= 1.0)
            centrePx = gSceneViewport.xy +
                       float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5) * gSceneViewport.zw;
    }
    const int2 base = int2(floor(centrePx));

    // A GAUSSIAN falloff, where rtShadowSpatial uses a flat box: box filters ring in frequency
    // response, visible as square-edged plateaus around a highlight. sigma = radius/2 keeps the
    // kernel's support near the requested radius, widening the blur smoothly.
    const float sigma2 = max((float)radius * 0.5, 0.5);
    const float inv2s2 = 1.0 / (2.0 * sigma2 * sigma2);

    float3 acc  = centre;
    float  wsum = 1.0;
    [loop] for (int oy = -radius; oy <= radius; ++oy) {
        [loop] for (int ox = -radius; ox <= radius; ++ox) {
            if (ox == 0 && oy == 0) continue;
            const int2 t = base + int2(ox, oy);
            if (any(t < 0) || t.x >= (int)texW || t.y >= (int)texH) continue;
            const float4 st = gRtReflHist.Load(int3(t, 0));
            if (st.a <= 0.0) continue;   // that neighbour's ray missed; see (2) above
            const float predicted = curDepth + dzdx * (float)ox + dzdy * (float)oy;
            const float tol = max(abs(predicted), 1.0) * 0.02 + 1.0;
            if (abs(st.a - predicted) > tol) continue;
#if AVER_GBUFFER_HISTORY
            // THE CREASE TERM -- copied from rtShadowSpatial's identical block (see there for the
            // reasoning and cos(60 deg)). ASSUMES gGBufNormalHist matches gRtReflHist's resolution,
            // same assumption made at gRtShadowHist -- both histories share the scene render target.
            const float3 nb = gGBufNormalHist.Load(int3(t, 0)).xyz * 2.0 - 1.0;
            if (dot(N, nb) < 0.5) continue;
#endif
            const float w = exp(-(float)(ox * ox + oy * oy) * inv2s2);
            acc  += st.rgb * w;
            wsum += w;
        }
    }
    return acc / wsum;
}

// Tiled/temporal wrapper around rtReflection(), mirroring rtShadowTemporal's structure/tile schedule
// so shadow and reflection rays amortise on the same cadence. Called ONLY after the caller gates on
// roughness (s.rough <= 0.5 in PSMainVoxi) -- not re-checked here.
// `hit`: true for a real reflection colour (fresh or reused from history; haveHist is already
// conditioned on a real hit, never a miss), false when the caller should fall back to sky.
float3 rtReflectionTemporal(float3 wpos, float3 N, float3 R, float3 L, float2 pixel, float rough,
                            float dzdx, float dzdy, out bool hit) {
    // ---- THE MIRROR CUTOFF: ONE predicate, three consumers ----
    // Below AVER_REFL_MIRROR_ROUGH a surface is a mirror throughout: no jitter, no temporal history,
    // no spatial filter. tan(cone)=rough^2, so at 0.1 the ray is displaced one part in a hundred of
    // its own length -- under a pixel, with no variance for a filter to remove.
    //
    // A CORRECTNESS FIX, NOT A TUNING KNOB: the first version gated the temporal blend on
    // `rough > 0.0` while claiming a smooth surface "still takes the fresh value outright" -- those
    // disagree for every near-mirror (glass at 0.05), both getting an 85%-history blend that can't
    // reduce a variance already at zero and can only add lag, smearing glass behind a moving camera.
    //
    // Deriving all three behaviours from ONE value is the point: a jittered-but-unfiltered lobe is
    // noise, a filtered-but-unjittered ray is blur -- they must agree, which only works reading the
    // same number.
    const float lobeRough = rough < AVER_REFL_MIRROR_ROUGH ? 0.0 : rough;

    // NO HISTORY TEXTURE: the lobe stays closed too -- widening it with nothing to converge into
    // would trade a biased-but-stable reflection for one that flickers every frame. rough=0 reduces
    // rtReflection to the exact mirror ray it traced before this change.
    if (gRtHistParams.x < 0.5) return rtReflection(wpos, N, R, L, pixel, 0.0, 0.0, hit);

    const float4 curClip = mul(float4(wpos, 1.0), gViewProj);
    const uint frameIdx  = (uint)gRtHistParams.z;
    // Per-frame rotation turning one ray per frame into a converging lobe estimate. A pure per-frame
    // count, never wall-clock, so a capture at frame N is reproducible; the golden-angle multiplier
    // keeps successive rotations from landing near each other the way a fixed increment would.
    const float frameJitter = (float)frameIdx * 2.39996323;
    const uint tileBits = (uint)gRtHistParams.w;

    if (tileBits == 0u) {
        // THE SHIPPED PATH: rtPixelsPerRayTileForQuality returns 1 at every tier, so this always
        // runs. Used to trace/write/return with NO temporal blend -- correct for a deterministic
        // mirror, wrong once the lobe opened. Blend added HERE, only where variance was introduced:
        // rough=0 still takes the fresh value outright, unchanged for glass, chrome and water.
        bool curHit;
        float3 fresh = rtReflection(wpos, N, R, L, pixel, lobeRough, frameJitter, curHit);
        float3 col = fresh;

        if (lobeRough > 0.0 && curHit) {
            float3 hist = 0.0;
            float2 velocityPx = 0.0;
            if (gRtHistParams.y > 0.5 && rtReprojectReflection(wpos, pixel, hist, velocityPx)) {
                // Same velocity-discounted shape as rtShadowTemporal: a far-slid sample is the same
                // surface but not the same point, and full trust smears a comet tail behind motion.
                // Still camera: full weight. Fast pan: falls back to this frame's spatial filter.
                const float t = saturate(length(velocityPx) / 6.0);
                col = lerp(hist, fresh, lerp(0.15, 1.0, t));
            }
        }

        // WRITE THE RAW TEMPORAL VALUE, NEVER FILTERED -- same rule as rtShadowTemporal's history
        // write. Feeding a filtered value back makes the spatial pass a compounding IIR filter,
        // the reflection slowly dissolving.
        gRtReflHistOut[uint2(pixel)] = curHit ? float4(col, curClip.w) : float4(0.0, 0.0, 0.0, -1.0);
        hit = curHit;
        return curHit ? rtReflectionSpatial(col, wpos, N, pixel, curClip.w, lobeRough, dzdx, dzdy) : col;
    }

    const uint tileMask = (1u << tileBits) - 1u;
    const uint turnMask  = (1u << (2u * tileBits)) - 1u;
    const uint localIdx  = (((uint)pixel.y & tileMask) << tileBits) | ((uint)pixel.x & tileMask);
    const bool myTurn    = localIdx == (frameIdx & turnMask);

    float3 hist = 0.0;
    float2 velocityPx = 0.0;
    const bool haveHist = gRtHistParams.y > 0.5 && rtReprojectReflection(wpos, pixel, hist, velocityPx);

    float3 col;
    bool curHit;
    if (myTurn || !haveHist) {
        col = rtReflection(wpos, N, R, L, pixel, lobeRough, frameJitter, curHit);
        // Same adaptive weight as rtShadowTemporal. Only blends a HIT with history: a fresh miss
        // stays a miss (caller's sky fallback handles it) rather than dragged toward a stale colour.
        if (curHit && haveHist) {
            const float budget = max(6.0 - 1.5 * (float)tileBits, 1.0);
            const float t = saturate(length(velocityPx) / budget);
            const float weight = lerp(0.9, 0.1, t);
            col = lerp(col, hist, weight);
        }
    } else {
        // Not this pixel's turn, reprojection found a real hit: reuse outright, no ray this frame.
        col = hist;
        curHit = true;
    }

    hit = curHit;
    // Raw, not filtered -- see the untiled branch's own note on why feeding the spatial result back
    // into the history makes it a compounding filter.
    gRtReflHistOut[uint2(pixel)] = curHit ? float4(col, curClip.w) : float4(0.0, 0.0, 0.0, -1.0);
    return curHit ? rtReflectionSpatial(col, wpos, N, pixel, curClip.w, lobeRough, dzdx, dzdy) : col;
}
#endif

// 3x3 PCF inside ONE cascade's quadrant of the atlas. Returns 1 = lit, 0 = shadowed, -1 = outside
// this cascade so the caller can try the next one.
float shadowSampleCascade(float3 wpos, uint c) {
    float4 lp = mul(float4(wpos, 1.0), gCascadeViewProj[c]);
    float3 p = lp.xyz / lp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0 || p.z < 0.0) return -1.0;

    // The 2x2 atlas layout: quadrant x = c&1, y = c>>1. Must match the C++ side.
    float2 quad = float2(c & 1u, c >> 1u) * 0.5;
    float inset = gShadowParams.x;
    uv = quad + clamp(uv * 0.5, float2(inset, inset), float2(0.5 - inset, 0.5 - inset));

    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        s += gShadowTex.SampleCmpLevelZero(gShadowSamp, uv + float2(x, y) * gShadowParams.x, p.z);
    return s / 9.0;
}

// Where shadowing starts fading to unshadowed, as a fraction of the last cascade's reach.
#define AVER_SHADOW_FADE_START 0.84

// Picks a cascade by distance and samples it, offsetting along N by that cascade's bias. Returns
// sun visibility, 1 = fully lit.
float shadowFactor(float3 wpos, float3 N, float ndl) {
    if (gShadowParams.y < 0.5) return 1.0;
    uint count = (uint)gShadowParams.w;
    if (count == 0) return 1.0;
    count = min(count, (uint)AVER_SHADOW_CASCADES);

    float slope = saturate(1.0 - ndl);
    float dist  = distance(wpos, gCamPos.xyz);

    float fadeSpan = max(gCascadeSplit[count - 1].x * (1.0 - AVER_SHADOW_FADE_START), 1e-3);
    float fade = saturate((dist - gCascadeSplit[count - 1].x * AVER_SHADOW_FADE_START) / fadeSpan);

    [loop] for (uint c = 0; c < count; ++c) {
        if (dist > gCascadeSplit[c].x) continue;
        float bias = gCascadeSplit[c].y * (1.0 + slope);
        float s = shadowSampleCascade(wpos + N * bias, c);
        if (s >= 0.0) return lerp(s, 1.0, fade);
    }
    return 1.0;
}

// Sun visibility for LIGHT INJECTION, from the GI-only shadow map. A SEPARATE FUNCTION from
// shadowFactor: that asks "is this PIXEL in shadow" by camera distance, correct on-screen but wrong
// for a voxel, which exists wherever the (camera-independent) GI volume is. Feeding voxels through
// the cascades forced fitCascades to union its last cascade with the whole GI volume (~14x the area,
// every draw). One box over the volume answers directly: no cascade, no camera distance, no fade.
float giShadowFactor(float3 wpos, float3 N, float ndl) {
    // Unusable map: fully lit. The volume is still injected, just without sun occlusion -- the same
    // degradation shadowFactor performs when the atlas is missing, and the reason giShadowTex_ is a
    // soft dependency rather than an init() failure.
    if (gGiShadowParams.y < 0.5) return 1.0;

    float slope = saturate(1.0 - ndl);
    float3 p0 = wpos + N * (gGiShadowParams.z * (1.0 + slope));

    float4 lp = mul(float4(p0, 1.0), gGiShadowViewProj);
    float3 p = lp.xyz / lp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    // OUTSIDE THE BOX IS LIT, NOT SHADOWED. The box covers the whole GI volume by construction, so
    // landing outside it means the voxel is outside the volume too and its radiance is never read.
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0 || p.z < 0.0) return 1.0;

    // No atlas quadrant to inset into: this map is one box filling the whole texture.
    float t = gGiShadowParams.x;
    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        s += gGiShadowTex.SampleCmpLevelZero(gShadowSamp, uv + float2(x, y) * t, p.z);
    return s / 9.0;
}

// Mip level being read by CSMip (b3: b0/b1 are taken by the graphics root signature).
cbuffer MipCB : register(b3) { uint gSrcMip; uint3 _mipPad; };

// world -> [0,1] volume coords
float3 voxelUVW(float3 wp) { return (wp - gVoxelOrigin.xyz) * gVoxelOrigin.w; }
bool insideVolume(float3 uvw) { return all(uvw >= 0.0) && all(uvw <= 1.0); }

// ---- cone tracing ----
// Marches a cone through the volume, widening with distance and reading a coarser mip each step.
// Returns front-to-back composited, premultiplied radiance; alpha is coverage.
float4 traceCone(float3 originWS, float3 dir, float aperture) {
    float voxelWorld = 1.0 / (gVoxelOrigin.w * gVoxelParams.x); // one voxel, world units
    float dist = voxelWorld * 2.0;
    float4 acc = 0;
    [loop] for (int step = 0; step < 24; ++step) {
        if (acc.a >= 0.95 || dist > gVoxelParams.z) break;
        float diameter = max(voxelWorld, 2.0 * aperture * dist);
        float mip = log2(diameter / voxelWorld);
        float3 uvw = voxelUVW(originWS + dir * dist);
        if (!insideVolume(uvw)) break;
        float4 s = gVoxelTex.SampleLevel(gVoxelSamp, uvw, mip);
        acc += (1.0 - acc.a) * s;
        dist += diameter;
    }
    return acc;
}

// Cosine-weighted gather of six cones over the hemisphere: one along the normal, five in a ring.
// Returns the indirect diffuse radiance and, through `ao`, the ambient occlusion.
float3 coneTracedIndirect(float3 wpos, float3 N, out float ao) {
    float3 up = abs(N.z) < 0.9 ? float3(0,0,1) : float3(1,0,0);
    float3 T = normalize(cross(up, N)), B = cross(N, T);

    // APERTURE FROM THE CONE COUNT, not a constant -- and this is what made a higher GI tier look
    // BLURRIER rather than sharper.
    //
    // It was a fixed 0.577 (a 60-degree cone) at every tier. Six of those roughly tile a hemisphere,
    // which is the classic VXGI configuration and where the number came from. Thirteen of them --
    // Epic's rung, and what this project runs -- cover it more than twice over, so raising GI
    // quality bought overlap instead of detail. Worse, a 60-degree cone's diameter grows about 2.15x
    // per march step, so it is sampling mip 7-8 within a couple of metres: texels tens of metres
    // across, i.e. a scene-wide average. That is precisely an ambient term, and it is why bright
    // sunlit floor never showed up on the column standing next to it.
    //
    // Tiling the hemisphere ONCE with N cones gives each a solid angle of 2pi/N, so
    // 2pi(1 - cos(theta)) = 2pi/N and cos(theta) = 1 - 1/N. Narrower cones climb the mip chain more
    // slowly and therefore keep near-field detail, which is the whole of what bounced light looks
    // like. N=6 lands at 33.6 degrees, N=13 at 22.6.
    const float cones    = max(gGiParams.x, 1.0);
    const float cosHalf  = saturate(1.0 - 1.0 / cones);
    const float aperture = sqrt(max(1.0 - cosHalf * cosHalf, 1e-6)) / max(cosHalf, 1e-6);   // tan

    float4 sum = traceCone(wpos, N, aperture);
    float occ = sum.a;
    float wsum = 1.0;
    // THE RING COMES FROM THE QUALITY TIER NOW, not a hardcoded 5: the axial cone along N is always
    // traced, so gGiParams.x is the TOTAL and the ring is one fewer.
    // A DYNAMIC [loop], not [unroll]: an unrolled loop with a dynamic bound is predicated, not
    // skipped, so every tier would still pay for six cones and the ladder would be a lie.
    // Angle is 2*pi/ring computed, not the 1.2566 literal (a rounded 2*pi/5) that stood here -- the
    // six-cone case shifts ~3e-5 radians, no longer bit-identical, in the more correct direction.
    // A COSINE-DISTRIBUTED SPIRAL, NOT ONE RING. Every extra cone used to land at the SAME
    // elevation -- `N * 0.5 + tangent * 0.866` is a fixed 60-degree tilt -- so Epic's thirteen cones
    // were one axial plus twelve crammed into a single band. More cones bought more samples of one
    // elevation rather than coverage of the hemisphere, which is the other half of why the gather
    // behaved like an ambient term no matter how high the tier went.
    //
    // t is stratified over [0,1) and cos(theta) = sqrt(1-t) gives the cosine-weighted hemisphere
    // distribution a diffuse gather actually wants; the golden angle in azimuth keeps successive
    // directions from clumping the way a constant step does. The `w = dot(N,d)` cosine weight below
    // is kept -- it is what makes this a weighted average rather than a plain mean, and the axial
    // cone still carries weight 1.
    const uint ring = (uint)cones - 1u;
    [loop] for (uint k = 0; k < ring; ++k) {
        float t    = ((float)k + 0.5) / (float)ring;
        float cosT = sqrt(saturate(1.0 - t));
        float sinT = sqrt(saturate(t));
        float ang  = 2.39996323 * (float)k;
        float3 d = normalize(N * cosT + (T * cos(ang) + B * sin(ang)) * sinT);
        float w = saturate(dot(N, d));
        float4 c = traceCone(wpos, d, aperture);
        sum += c * w; occ += c.a * w; wsum += w;
    }
    sum /= wsum; occ /= wsum;
    ao = saturate(1.0 - occ);
    // THE SAME CEILING THE INJECTION ALREADY HAS, applied AFTER the intensity multiply. Every voxel
    // is clamped to AVER_VOX_MAXRAD on the way in (PSVoxel), but gVoxelParams.y (giIntensity, up to
    // 8 per Voxi.cpp) multiplies that bounded quantity straight back out: 16 x 8 = 128, out of a
    // volume where nothing emits more than 16 -- the runaway Voxi.hpp documents (enough large,
    // saturated geometry floods the frame with its colour). PHYSICAL bound, not tuned: a gather can't
    // hand back more radiance than the brightest thing it gathered emits, so it reuses the
    // injection's own constant rather than a second one to keep in step.
    // A CLAMP IS NOT A LIGHTING MODEL: it stops divergence, not correctness at the ceiling. Fires in
    // nothing shipped -- ElectricDreams, the gate scene, the furnace all measured bit-identical.
    return min(sum.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
}

// ================= additive G-buffer: velocity, view-space depth, normal+roughness =================
// Gated on AVER_GBUFFER, following AVER_RT's convention: a compile-time define (never a runtime
// branch), off by default, so PSMainVoxi's/PSRayDriven's ORIGINAL variants fall through unchanged --
// THE FEATURE IS ADDITIVE: with the define off the frame must be bit-identical, checked by the
// render gate oracle (18 gates x 9 configurations).
//
// WHY THIS EXISTS: nothing produces motion vectors and there's no G-buffer -- PSMainVoxi returns one
// SV_TARGET, normal/roughness/albedo living only in shader registers. That single gap blocks the
// vendored FidelityFX denoiser (third_party/fidelityfx-denoiser/README.md), FSR 2/3, TAA and
// screen-space reflections at once, and is WHY temporal reprojection is wrong for moving geometry:
// rtReprojectHistory and siblings transform THIS frame's wpos through LAST frame's camera, only
// valid for a surface that didn't move.
//
// THIS SLICE ONLY WRITES THE TARGETS -- wiring a consumer (FFX denoiser, FSR3, TAA) is later work,
// out of scope on purpose: unread-but-written is this codebase's recurring shape, deliberately here.
//
// ---- HOW TO DECODE EACH CHANNEL, so encode and decode don't drift apart ----
//   SV_TARGET1 velocity:        RG16F. Texels/frame, DESTINATION (this frame's) minus SOURCE (last
//                                frame's) texel: `prevPixel = thisPixel - velocity`, matching
//                                rhi::UpscalerNeeds::MotionVectors' documented convention.
//   SV_TARGET2 viewZ:           R32F. VIEW-SPACE LINEAR depth (clip.w), NOT the post-projective
//                                [0,1] SV_Position.z/SV_DEPTH a hardware depth buffer stores.
//   SV_TARGET3 normalRoughness: RGB10A2. xyz = world normal*0.5+0.5 (decode N=xyz*2-1); w = roughness.
//                                ALPHA IS TWO BITS (four levels), specified for this feature, not
//                                chosen here; finer roughness reads the material's own SRV instead.
#if AVER_GBUFFER
struct GBufferOut {
    float4 col              : SV_TARGET0;   // exactly PSMainVoxi's own colour -- unchanged by this define
    float2 velocity         : SV_TARGET1;
    float  viewZ            : SV_TARGET2;
    float4 normalRoughness  : SV_TARGET3;
};

// Packs a world-space unit normal and roughness into the RGB10A2 convention above. Shared by
// PSMainVoxi and PSRayDriven so the encode is written once, not risking divergence.
float4 averPackNormalRoughness(float3 N, float roughness) {
    return float4(N * 0.5 + 0.5, saturate(roughness));
}

// Screen-space motion for the velocity channel: `wpos` reprojected through THIS frame's camera minus
// the SAME wpos through LAST frame's, in the SCENE VIEWPORT RECT (same landmine as
// rtReprojectHistory: the editor docks the 3D view in a sub-rect, so plain ndc*0.5+0.5 is wrong).
//
// STATIC-GEOMETRY ONLY, A STATED DELIBERATE GAP: using `wpos` in both projections is correct only if
// the surface didn't move. A moving instance needs its OWN previous-frame transform (previous
// `gWorld` for raster, previous `RtInstance.objectToWorld` for ray-driven), and NEITHER EXISTS YET.
// Consequence: correct motion for a stationary object under a moving camera, ZERO motion for an
// object that is itself animating -- SILENTLY. Fix: thread a previous-transform through RtInstance
// (or the per-draw cbuffer) and reproject through it instead of `wpos` twice.
float2 averGBufferVelocity(float3 wpos) {
    const float4 curClip  = mul(float4(wpos, 1.0), gViewProj);
    const float4 prevClip = mul(float4(wpos, 1.0), gPrevViewProj);
    // Either transform can put this point behind its own near plane -- prevClip routinely does (first
    // frame, or anything that just entered the frustum). Zero is "no motion known", same fallback as
    // rtReprojectHistory's velocityPx -- the least wrong answer when the maths is undefined, rather
    // than an Inf/NaN divide.
    if (curClip.w <= 1e-4 || prevClip.w <= 1e-4) return float2(0.0, 0.0);

    const float2 curNdc  = curClip.xy  / curClip.w;
    const float2 prevNdc = prevClip.xy / prevClip.w;
    const float2 curPx  = gSceneViewport.xy +
                          float2(curNdc.x * 0.5 + 0.5, 0.5 - curNdc.y * 0.5) * gSceneViewport.zw;
    const float2 prevPx = gSceneViewport.xy +
                          float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * gSceneViewport.zw;
    // DESTINATION (curPx) minus SOURCE (prevPx) -- see header and UpscalerNeeds::MotionVectors
    // (RHIResources.hpp) for why that order is the contract a consumer can assume.
    return curPx - prevPx;
}

// One expansion point for PSMainVoxi's several `return` statements, so the three extra channels stay
// identical everywhere instead of by hand per site. gbufVelocity/gbufViewZ/gbufNormalRough are
// computed once, after `s` is built -- this macro only assembles values that already exist.
#define AVER_GBUF_RETURN(colorExpr) \
    { GBufferOut aver_gbuf_o; aver_gbuf_o.col = (colorExpr); aver_gbuf_o.velocity = gbufVelocity; \
      aver_gbuf_o.viewZ = gbufViewZ; aver_gbuf_o.normalRoughness = gbufNormalRough; return aver_gbuf_o; }
#else
// Disabled: PSMainVoxi's own `return` sites expand to a plain return, exactly what stood at each site
// before this feature existed -- see the #if branch above for what they become when it is on.
#define AVER_GBUF_RETURN(colorExpr) return (colorExpr)
#endif

// The Voxi lit pixel shader. Voxi supplies light transport only â€” sun visibility, sky, bounce â€”
// and the material shades it. Returns linear radiance; the post chain tonemaps.
#if AVER_GBUFFER
GBufferOut PSMainVoxi(VSOut i) {
#else
float4 PSMainVoxi(VSOut i) : SV_TARGET {
#endif
    float3 N = normalize(i.nrmWS);
    float3 L = normalize(gLightDir.xyz);
    float ndl = saturate(dot(N, L));
#if AVER_RT
    // View-space depth/gradient taken HERE, at the top where control flow is uniform, and carried
    // down to the roughness-gated reflection block: a derivative inside divergent flow is undefined
    // in HLSL (the same landmine rtShadow's dpx/dpy step around) and would present as
    // adapter-specific corruption. rtShadowSpatial can afford its own ddx since its caller is uniform.
    const float rtViewZ = mul(float4(i.wpos, 1.0), gViewProj).w;
    const float rtDzdx  = ddx(rtViewZ);
    const float rtDzdy  = ddy(rtViewZ);

    // float3 NOW: rtShadowTemporal carries the medium's colour. shadowFactor returns a scalar and
    // promotes, so the non-RT path is unchanged.
    float3 sunVis;
    if (gShadowParams.z > 0.5)
        sunVis = rtShadowTemporal(i.wpos, N, L, i.pos.xy, ddx(i.wpos), ddy(i.wpos),
                                  (uint)max(gRtParams.y, 1.0));
    else                       sunVis = shadowFactor(i.wpos, N, ndl);
#else
    const float3 sunVis = shadowFactor(i.wpos, N, ndl);
#endif
#if AVER_GBUFFER
    // View-space linear depth for the G-buffer. REUSED, not recomputed, when AVER_RT already computed
    // it as rtViewZ for the reflection block -- both agree on mul(wpos,1,gViewProj).w.
#if AVER_RT
    const float gbufViewZ = rtViewZ;
#else
    const float gbufViewZ = mul(float4(i.wpos, 1.0), gViewProj).w;
#endif
#endif
    float ao = 1.0;
    float3 ind = 0;
    if (gVoxelParams.w > 0.5) ind = coneTracedIndirect(i.wpos, N, ao);

    AverVertex vtx = averVertexOf(i);

    // ---- A single-sided BLENDED surface draws ONE layer, not every face it owns ----
    // THE SCENE PIPELINE DOES NOT CULL (`scene.cull = CullMode::None`, VoxiRenderer.cpp:3133), so a
    // closed volume rasterises all faces; opaque hides this behind the depth test, but a blended draw
    // writes no depth and every surviving face's alpha multiplies. A water box came out ~4 coats
    // thick: MEASURED, alpha 0.02 darkened the pit from (50.7,51.4,49.2) to (36.6,41.9,46.1).
    //
    // GATED ON THE AUTHORED `twosided` FLAG (parseable since the format existed, never read until
    // now): M_Glass sets it (glass is routinely CULL none, walked around), so a pane keeps
    // compositing both faces; water sets twosided=0 and gets one layer. Author decides.
    //
    // BLENDED ONLY: opaque already gets correct single-layer results from the depth test, and
    // single-sided opaque content (a floor seen from below) has always relied on CullMode::None.
    //
    // dot(N,V), not SV_IsFrontFace: reuses averVertexOf's own backFace rather than a second notion
    // that could disagree with the normal flip beside it.
    //
    // INVERTED WHEN THE EYE IS INSIDE THE VOLUME, not switched off -- the whole reason this is safe.
    // Inside a closed volume every face is backFace, so the plain discard deleted the surface
    // outright (swimming under the pool showed no water, just concrete). One layer from below still
    // means keeping back faces and dropping front ones -- the same rule read from the other side.
    //
    // gCameraMedium.x is a bounding-SPHERE test, loose (a shallow pool's sphere bulges above its
    // surface). Inverting rather than disabling makes that safe: a false positive on the deck drops
    // the near face and keeps the far one, still ONE layer. Disabling the discard would resurrect
    // the four-coats bug. Do not "simplify" this into an early-out.
    const bool eyeInside = gCameraMedium.x > 0.5;
    if ((gMaterialFlags & AVER_MAT_ALPHA_BLEND) && !(gMaterialFlags & AVER_MAT_TWO_SIDED) &&
        (eyeInside ? !vtx.backFace : vtx.backFace))
        clip(-1);

    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    sun.visibility = sunVis;
    // CAUSTICS go INTO THE SUN TERM: they're concentrated sunlight, not their own glow, so scaling
    // the sun means a caustic can't appear in shadow (sunVis already zero there) -- the obvious tell
    // of a caustic implementation done wrong.
    // OPAQUE ONLY: the water's own top face sits at the box's max.z and would light itself otherwise.
    if (!(gMaterialFlags & AVER_MAT_ALPHA_BLEND))
        sun.visibility *= 1.0 + averCausticFocus(vtx.wpos);

    AverSurface s = averEvalMaterial(vtx, sun);
#if AVER_GBUFFER
    // Velocity and packed normal/roughness computed once here, after `s` exists but before the first
    // return, so every AVER_GBUF_RETURN site shares identical values. See averGBufferVelocity for
    // what this doesn't yet handle (a moving instance's own motion vs. camera motion).
    const float2 gbufVelocity    = averGBufferVelocity(i.wpos);
    const float4 gbufNormalRough = averPackNormalRoughness(averShadingNormal(s), s.rough);
#endif
    float4 display;
    if (averDisplayColour(s, display)) AVER_GBUF_RETURN(display);

    float3 V = normalize(gCamPos.xyz - i.wpos);
    float3 R = reflect(-V, averShadingNormal(s));
    AverIndirect ind4;
    ind4.ambient      = averSkyIrradiance(averShadingNormal(s));
    ind4.ambientScale = gAmbient.r;
    ind4.diffuse      = ind;
    // TRACED SKY VISIBILITY WHEN THE TIER PAYS FOR IT, the cone gather's own estimate otherwise.
    // gAmbientParams.x is already 0 on any frame without an acceleration structure (VoxiRenderer
    // gates it on rtActive_), so this needs no second RUNTIME test -- but it does need a
    // COMPILE-TIME one: rtSkyOcclusion lives inside this file's `#if AVER_RT` region because it
    // names gScene, and this line does not. Without the guard every non-RT entry point in the file
    // (VSShadow, PSVoxel, CSResolve, ...) fails on an undeclared identifier -- which is exactly what
    // happened, and cost a build that reported OK because HLSL compiles at RUNTIME here.
#if AVER_RT
    ind4.occlusion    = gAmbientParams.x > 0.5
                      ? rtSkyOcclusionTemporal(i.wpos, N, i.pos.xy, (uint)gAmbientParams.x)
                      : ao;
#else
    ind4.occlusion    = ao;
#endif
#if AVER_RT
    // Ray traced when the acceleration structure and geometry table both exist; preferred over the
    // cone trace unconditionally since the cone is bounded by the voxel volume and this is not.
    //
    // WHY 0.75, NOT 1.0 (see rtReflection's own comment for the aperture/lobe history): past ~0.75
    // rough the lobe is wide enough that one ray is estimating a near-hemispherical integral neither
    // filter can close (spatial kernel saturates at radius 3; temporal history rejects itself once
    // the camera moves). What that rough a surface reflects is close to its surroundings' average --
    // what the cone trace/sky term below already return. 0.75 is where the ray stops being the
    // better answer, not where it stops being affordable.
    //
    // The fade is only a seam-hider across the last quarter (0 at 0.5 rough, 1 at 0.75): nothing
    // pops crossing the cutoff, everything below 0.5 is the traced answer at full strength.
    if (gShadowParams.z > 0.5 && gRtParams.w > 0.5 && s.rough <= 0.75) {
        bool specHit = false;
        float3 refl = rtReflectionTemporal(i.wpos, N, R, L, i.pos.xy, s.rough,
                                           rtDzdx, rtDzdy, specHit);
        // ONE skyColor(R), NOT TWO, AND NOT ALWAYS ONE: both lerp operands want the same value, and
        // skyColor is a 32-step atmosphere march (too expensive to trust DXC to CSE), so it's named
        // once and guarded below.
        // WHEN THE MARCH IS PURE WASTE: smoothstep(0.5,0.75,rough) is EXACTLY 0 at/below 0.5, so a
        // hit reduces the lerp to `refl` and the marched sky is multiplied by zero -- water, glass,
        // chrome, wet stone. MEASURED on PTTest pool: blended replay 18.9 -> 15.9ms, whole frame
        // 44.86 -> 41.78ms, nothing on screen changing.
        // STILL ONE CALL: an earlier split into two skyColor(R) branches cost 13% (8.15 -> 9.26ms) --
        // a divergent wave executes both sides, so branching around the march ran it twice instead.
        const float skyW = smoothstep(0.5, 0.75, s.rough);
        float3 skyR = float3(0.0, 0.0, 0.0);
        if (!specHit || skyW > 0.0) skyR = skyColor(R);
        ind4.specular = lerp(specHit ? refl : skyR, skyR, skyW);
    } else
#endif
    if (gVoxelParams.w > 0.5) {
        float  specAperture = clamp(s.rough * 0.5 + 0.02, 0.02, 0.4);
        float4 sceneSpec    = traceCone(i.wpos, R, specAperture);
        // Bounded for the reason coneTracedIndirect is; sky is left alone (not a volume gather, no
        // runaway of its own).
        // SKY IS SKIPPED WHERE THE CONE ALREADY SAW A WALL: HLSL doesn't short-circuit a multiply, so
        // skyColor(R)*(1-sceneSpec.a) ran the full 32-step march even fully occluded -- the common
        // rough>0.75 case (concrete, cloth, stone) paying for a value it then multiplied away.
        // SAME SHAPE/FIX AS averFogInscatter'S THRESHOLD ("scene draw 8.9ms -> 1.3ms, 85% of the
        // scene pass"): a [0,1] coverage scaling bounded radiance can be skipped under one 8-bit step
        // with no banding. 0.004, NOT 0.01, because this weight multiplies a sky far brighter than
        // the fog reference -- at 0.004 the dropped term is at most 0.4% of a sky sample.
        const float skyWeight = 1.0 - sceneSpec.a;
        ind4.specular       = min(sceneSpec.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
        if (skyWeight > 0.004) ind4.specular += skyColor(R) * skyWeight;
    } else {
        ind4.specular       = skyColor(R);
    }

    // ---- TRANSLUCENT MATERIALS TAKE A SEPARATE, EARLY-RETURNING PATH ----
    // Gated on the flag, not averOpacity(s) < 1: alpha is an authored number that can legally
    // disagree with which PIPELINE/blend-state the draw runs under (a blended twin at alpha 1, or
    // vice versa). AVER_MAT_ALPHA_BLEND is set exactly when routed to the blended PSOs
    // (scenePipeline(), VoxiRenderer.cpp) -- the actual decider of PremultipliedAlpha vs. straight
    // composite. Reading the wrong signal packs the wrong kind of output silently: every value stays
    // a plausible colour, just wrong by however translucent the surface is that frame.
    if (gMaterialFlags & AVER_MAT_ALPHA_BLEND) {
        // averShadeSplit is averShadeDirect + averShadeIndirect's IDENTICAL arithmetic (PbrShaders.cpp),
        // kept as two registers instead of summed -- a blended draw gets the same energy an opaque
        // one would, apportioned between "coverage-weighted" and "always full strength" before alpha.
        float3 dif, spc;
        averShadeSplit(s, sun, ind4, dif, spc);
        // rgb = specular + diffuse*alpha, a = alpha (averBlendedOutput's contract) -- straight alpha
        // would multiply `spc` too, so a pane at 0.2 opacity showed its reflection at a fifth
        // strength. sceneBlendedPso_'s PremultipliedAlpha blend state expects it packed this way.
        // THE VOLUME, where one is authored. gAttenuationDistance <= 0 is the off state every
        // material had before, so this compiles to a compare nobody takes for ordinary glass --
        // averBlendedOutputVolume reduces to averBlendedOutput exactly at T=1 either way.
        // MEASURED WITH A RAY, not an authored height: see averVolumeThickness for why a fragment
        // can't know its own thickness (the guessed formula made the PTTest pool a bright opaque
        // slab). Behind AVER_RT: without a ray there's no structure to measure against, so the
        // surface keeps today's volumeless composite instead of a fabricated thickness.
        float4 outc;
#if AVER_RT
        // FRONT FACES ONLY -- a correctness gate, not an optimisation. averVolumeThickness traces
        // ALONG THE VIEW DIRECTION and takes the nearest hit (the medium's exit), correct only when
        // the shaded point is where the ray ENTERS. A BACK face starts where the ray LEAVES, so it
        // measures the distance to whatever's behind the glass (the floor, metres away) instead of
        // the pane's few centimetres. M_Glass authors twosided=1 (walked around), so its back face
        // was absorbing over the scene's depth -- why an 8cm pane read like a metre of bottle glass.
        //
        // s.backFace, NOT SV_IsFrontFace -- A BUG I SHIPPED: SV_IsFrontFace is winding-dependent;
        // s.backFace is `dot(N,V)<0` (averVertexOf), the actual question of whether the ray is
        // entering. The fluid box winds opposite the cube, so the winding test called the pool's
        // visible top a BACK face and silently switched water absorption off -- despite this file
        // already stating the rule ("dot(N,V), not SV_IsFrontFace") a few hundred lines up.
        //
        // Absorbing once, on entry, is also physically right: a pane's thickness is crossed once.
        // FROM THE SURFACE, NOT THE CBUFFER: a material GRAPH can drive attenuationColor/Distance
        // per pixel; reading gAttenuationColor would silently discard whatever the graph decided.
        if (s.attenuationDistance > 0.0 && !s.backFace) {
            // Measured ONCE, used twice: absorption needs it for Beer-Lambert, refraction needs it
            // for how far the bent path travels -- a second trace would give the same answer.
            const float volThick = averVolumeThickness(i.wpos, N, -s.V);
            const float3 volT = averVolumeTransmittance(
                s.attenuationColor, s.attenuationDistance, volThick);
            // THE BACKDROP PATH: what lets attenuationColor's HUE reach the picture at all --
            // averBlendedOutputVolume's fallback can only darken absorbed channels, never tint the
            // background (one blend alpha is one number). i.pos.xy is the screen coord the copy uses.
        outc = averBlendedOutputBackdrop(s, dif, spc, volT, i.pos.xy, i.wpos, volThick);
        } else
#endif
        outc = averBlendedOutput(s, dif, spc);

        // ---- THE FOG DECISION ----
        // Fogged HERE, deliberately not following WaterShaders.hpp's PSWater precedent, which skips
        // averApplyFog because water's colour is already a Fresnel-blended palette standing in for
        // the atmosphere itself -- fogging it again would double-apply air to a surface meant to
        // read as air. Glass isn't that: `dif`/`spc` are ordinary unfogged PBR terms.
        //
        // WHAT'S ALREADY FOGGED AND MUST NOT BE TOUCHED: the scene behind the pane. Those pixels went
        // through their own averApplyFog(radiance, theirWpos), and PremultipliedAlpha's blend
        // (dst_new = outc.rgb + dst.rgb*(1-alpha)) carries dst through untouched -- this code never
        // reads dst, so only `outc.rgb` (this pane's own new light) gets fogged below.
        //
        // FOGGING THE PACKED SUM, NOT dif/spc SEPARATELY: the atmosphere in front of the glass is the
        // same distance for both lobes, so both want identical attenuation/inscatter. averApplyFog is
        // affine (color*T + inscatter) but NOT distributive over the alpha-weighted split -- fogging
        // dif and spc separately then combining as spc_fogged + dif_fogged*alpha would add the
        // inscatter term TWICE for one slab of air, the same double-counting the water comment warns
        // about. Fogging `outc.rgb` once is symmetric with the opaque branch's single fog call below.
        // Alpha is coverage, not radiance, and is untouched by fog either way.
        outc.rgb = averApplyFog(outc.rgb, i.wpos);
        AVER_GBUF_RETURN(outc);
    }

    float3 radiance = 0.0;
    radiance = averShadeDirect(radiance, s, sun);
    radiance = averShadeIndirect(radiance, s, ind4);
    radiance = averApplyFog(radiance, i.wpos);
    AVER_GBUF_RETURN(float4(radiance, averOpacity(s)));
}

// ================= ray-driven primary visibility (experimental) =================
// THE ONLY THING THIS REPLACES IS "WHAT DID THIS PIXEL SEE" -- everything after the first hit is
// the same work PSMainVoxi does (sun shadow, sky ambient, fog), since the rasteriser never did that.
//
// WHAT IT GIVES UP: hardware early-Z. A rasterised fragment discovered hidden is discarded before
// its shader runs; a ray pays the whole traversal to learn the same thing. The trade this mode
// exists to measure; the number to beat is in Settings::rtRenderMode.
//
// TEXTURED -- a correction record: this comment claimed the opposite long after it stopped being
// true. Under AVER_RT_BINDLESS a hit samples base colour, metal-rough, normal, occlusion, emissive
// and the slope-blended second layer (RtInstance::materialIndex/gRtMaterials, real gradient
// footprint). The Texture2DArray this used to call "not yet bound" is in register space 1 below.
//
// WHAT STILL DIFFERS, narrower than before: no material GRAPH runs on any ray path (graphId declared,
// never read), and normal-map PERTURBATION is compiled out (AVER_RT_NORMAL_MAPPING 0, built then
// unused). GEOMETRY MUST NOT DIFFER -- what the side-by-side capture checks.
//
// ---- WHAT A TRANSLUCENT SURFACE SHARES WITH THIS PASS, AND WHAT IT STILL DOES NOT ----
// A blended (glass) draw never reaches PSRayDriven, but not because it's missing from the TLAS --
// it's there, routed into the TRANSLUCENT LANE (kRtMaskTranslucent, FORCE_NON_OPAQUE) so a shadow
// ray can attenuate through it. What excludes it from THIS pass is the MASK: the primary ray here
// traces AVER_RT_MASK_OPAQUE only, so widening the mask would start hitting glass with no TLAS change.
// Glass is drawn only where raster mode draws it: D3D12Device::endFrame's blended-mesh flush, through
// VSMain+PSMainVoxi's premultiplied-alpha PSO, after this pass's scenePass() and the deferred sky --
// depth test, blend equation, vertex math and shading are IDENTICAL code paths regardless of which
// pass answered primary visibility for what's behind the pane; none of it reads rtRenderMode.
//
// WHAT DOES DIVERGE: what a translucent surface reveals, the picture already painted by whichever
// pass drew primary visibility. This pass's hit shading is a simpler material response than
// PSMainVoxi's -- maps ARE sampled (base colour, metal-rough, normal, occlusion, emissive, second
// layer), but no material GRAPH runs, normal-map perturbation is compiled out, and a stochastic
// path-traced bounce stands in for the voxel cone trace. These approximations, accepted for an
// opaque surface, stopped being invisible once glass could put that surface behind a window --
// see this pass's environment-specular block below for the one piece of that gap this change closes
// (voxel-cone GI instead of flat sky) and why the rest is untouched.
//
// BEHIND AVER_RT because RayQuery is: this entry point only compiles into the SM 6.5 variant, and
// VoxiRenderer refuses the mode outright when the device has no ray-query support.
#if AVER_RT
struct RayDrivenOut {
    float4 col   : SV_TARGET;
    float  depth : SV_DEPTH;
};

// PSRayDriven's own G-buffer bundle: same contract as GBufferOut (field comments up by PSMainVoxi)
// plus SV_DEPTH, which this pass writes itself since it answers visibility with a ray and has no
// rasteriser depth to inherit -- same as RayDrivenOut already does without this define.
#if AVER_GBUFFER
struct RayDrivenGBufferOut {
    float4 col              : SV_TARGET0;
    float2 velocity         : SV_TARGET1;
    float  viewZ            : SV_TARGET2;
    float4 normalRoughness  : SV_TARGET3;
    float  depth            : SV_DEPTH;
};
#endif

// THIS ENTRY POINT MUST FILL THE G-BUFFER, NOT OPTIONAL: ray-driven primary visibility is now the
// DEFAULT (Settings::rtRenderMode=1), so a rasteriser-only G-buffer would sit empty by default --
// the "built through every layer and nothing fills it" failure this codebase keeps producing. Both
// `return` sites below fill every AVER_GBUFFER channel.
#if AVER_GBUFFER
RayDrivenGBufferOut PSRayDriven(SkyOut i) {
    RayDrivenGBufferOut o;
#else
RayDrivenOut PSRayDriven(SkyOut i) {
    RayDrivenOut o;
#endif

    // The same NDC-to-world-ray reconstruction PSVoxelDebug does, through the same gInvViewProj,
    // so the primary ray and the debug raymarch cannot disagree about where a pixel looks.
    float4 far = mul(float4(i.ndc, 1.0, 1.0), gInvViewProj);
    float3 dir = normalize(far.xyz / far.w - gCamPos.xyz);

    RayDesc r;
    r.Origin    = gCamPos.xyz;
    r.Direction = dir;
    r.TMin      = 0.0;
    r.TMax      = 1.0e7;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    // Opaque lane. THE ONE RAY USING THE NARROW LANE: AVER_RT_MASK_OPAQUE, not _OPAQUE_ALL -- it
    // starts inside the viewer's own head, so it is the one traversal that must not see
    // AVER_RT_MASK_OWNER_HIDDEN. Every other opaque query in this file asks for _ALL.
    //
    // THIS IS PRIMARY VISIBILITY, so it is the ray the cutout matters most on: whatever it commits
    // is literally what you see. FORCE_OPAQUE here made every leaf card a solid rectangle while the
    // raster path clipped the same material correctly -- and since ray-driven is the standing
    // default, the wrong one was the one on screen.
    q.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE, r);
    averRtProceedSolid(q);

    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        // A miss is the sky at the far plane. Depth 1, not 0: this projection isn't reversed, so 0
        // would put the sky in front of everything.
        // skyColorFull, NOT skyColor -- a whole-screen atmosphere march difference. This colour is
        // thrown away whenever a sky dome draws: the deferred dome (D3D12Device.cpp, gated on
        // skyEnabled_ && !frameSuppressed_) is an OPAQUE fullscreen triangle with DepthFunc EQUAL
        // against the 1.0 written below, so it overwrites every one of these pixels. The dome is the
        // richer sky (clouds, atmosphere, sun disc); this write is only the no-dome placeholder.
        // Paying for a per-pixel march to produce a value unconditionally overwritten is the most
        // expensive way to compute nothing -- MEASURED at up to 41% of a frame once skyColor started
        // honouring the physical model.
        // WHAT CHANGES: nothing, with a sky enabled (every normal frame, overwritten either way).
        // With the sky DISABLED, the background is the authored gradient, not a marched atmosphere.
        o.col   = float4(skyColorFull(dir), 1.0);
        o.depth = 1.0;
#if AVER_GBUFFER
        // No real surface for a miss, so no true velocity or normal. Velocity 0 (matches
        // averGBufferVelocity's own near-plane fallback); viewZ 1e7 (a sentinel past real geometry,
        // matching this ray's own TMax); normal -dir so renormalising gives a unit vector, not a NaN
        // from normalize(0,0,0). NOT A SKY MASK SUBSTITUTE: a consumer excluding sky pixels should
        // use viewZ's far-plane sentinel, not claim anything from this normal.
        o.velocity        = float2(0.0, 0.0);
        o.viewZ            = 1.0e7;
        o.normalRoughness  = averPackNormalRoughness(-dir, 1.0);
#endif
        return o;
    }

    // Surface reconstruction: same barycentric interpolation and ROTATION-ONLY normal transform as
    // rtReflection (see there for why no inverse transpose) -- must agree, or a surface would shade
    // differently seen directly vs. in a mirror.
    RtInstance inst = gRtInstances[q.CommittedInstanceID()];
    uint tri = inst.firstIndex + q.CommittedPrimitiveIndex() * 3;
    uint i0  = inst.firstVertex + gRtIndices[tri + 0];
    uint i1  = inst.firstVertex + gRtIndices[tri + 1];
    uint i2  = inst.firstVertex + gRtIndices[tri + 2];

    float2 bary = q.CommittedTriangleBarycentrics();
    float3 w    = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float3 nObj = normalize(gRtVerts[i0].nrm * w.x + gRtVerts[i1].nrm * w.y + gRtVerts[i2].nrm * w.z);
    float3 N    = normalize(mul(float4(nObj, 0.0), inst.objectToWorld).xyz);
    if (dot(N, dir) > 0.0) N = -N;   // face the ray, so a back-facing hit is not lit from behind

    // UV, same barycentric pattern as `nObj` above -- RtVertex has always carried it, unread until
    // now. NOT sampled here (no texture array yet for it). WHOEVER ADDS THAT SAMPLE: this is a RAY
    // HIT, not a rasterised fragment -- ddx/ddy on it is undefined in HLSL (the same landmine
    // rtShadow's dpx/dpy step around), so the sample MUST be SampleLevel or SampleGrad, never plain
    // Sample(). Mip 0 costs minification aliasing at glancing angles/distance; a future SampleGrad
    // would want a footprint like rdRayDx/rdRayDy below, recomputed here since UV precedes wpos/hitT.
    float2 hitUV = gRtVerts[i0].uv * w.x + gRtVerts[i1].uv * w.y + gRtVerts[i2].uv * w.z;

    // The hit's own material, keyed by RtInstance::materialIndex (see that field's own comment for
    // what it used to be and why repurposing it cost nothing). This is what makes the constants
    // below real instead of guessed -- see the comment just above where they are used.
    RtMaterial mat = gRtMaterials[inst.materialIndex];

    const float hitT = q.CommittedRayT();
    float3 wpos = gCamPos.xyz + dir * hitT;
    float3 L    = normalize(gLightDir.xyz);

    // ---- THE SHADOW-RAY FOOTPRINT: A RAY DIFFERENTIAL, NOT A SCREEN-SPACE DERIVATIVE ----
    // What stood here passed float3(0,0,0) for dpx/dpy -- a point sample, citing rtReflection's own
    // inner shadow call as precedent (correct THERE: a reflected hit's neighbours can land on
    // triangles metres apart, so ddx/ddy is undefined and meaningless). MEASURED (speckle audit): a
    // dense fine speckle on PTTest's pit far wall, ray-driven only, tied to nothing but this call --
    // a zero footprint fires every sample from the same origin, aliasing a sub-pixel shadow boundary
    // (a grazing self-shadow) into speckle instead of a true area fraction. PSMainVoxi never shows
    // it because its ddx(i.wpos)/ddy(i.wpos) footprint is real.
    //
    // UNLIKE THE REFLECTED CASE, a primary ray's DIRECTION is a smooth analytic function of its pixel
    // (`dir`, from i.ndc/gInvViewProj), so it can be evaluated for the NEIGHBOUR pixel directly --
    // no ddx/ddy, well-defined in any control flow. This is a RAY DIFFERENTIAL (Igehy 1999, the same
    // idea a ray-cone texture-LOD scheme uses): reconstruct the neighbour's primary-ray direction and
    // see how far it diverged by `hitT` -- a function of the CAMERA and pixel grid, never of what
    // either ray hit, unlike ddx(wpos) across a silhouette which would return a metres-wide gap.
    //
    // NOTE THE PUNCTUATION: this shader lives inside a C++ raw string literal, so close-paren
    // double-quote ENDS IT. An earlier draft closed a quote right after a bracket here and produced
    // forty lines of C++ syntax errors. Keep brackets and quotes apart in this file.
    const float2 ndcPixelStep = float2(2.0 / max(gSceneViewport.z, 1.0),
                                       2.0 / max(gSceneViewport.w, 1.0));
    float4 farDx = mul(float4(i.ndc + float2(ndcPixelStep.x, 0.0), 1.0, 1.0), gInvViewProj);
    float3 dirDx = normalize(farDx.xyz / farDx.w - gCamPos.xyz);
    float4 farDy = mul(float4(i.ndc + float2(0.0, ndcPixelStep.y), 1.0, 1.0), gInvViewProj);
    float3 dirDy = normalize(farDy.xyz / farDy.w - gCamPos.xyz);
    // World-space displacement to the neighbour ray, at the SAME distance this ray travelled --
    // "pixel angular size times hit distance" -- widening with range, shrinking near the camera.
    const float3 rdRayDx = (dirDx - dir) * hitT;
    const float3 rdRayDy = (dirDy - dir) * hitT;
    // Flattened onto the hit's tangent plane before use as dpx/dpy: rtShadow jitters the ray ORIGIN
    // by these then offsets along N, and an un-flattened footprint could push that origin off-surface,
    // reopening the grazing-angle acne N*bias exists to close. PSMainVoxi's ddx/ddy(wpos) are real
    // surface points already and skip this; this reconstruction isn't, so it's projected onto N.
    const float3 dpx = rdRayDx - N * dot(rdRayDx, N);
    const float3 dpy = rdRayDy - N * dot(rdRayDy, N);

    // THE SHADOW RAY IS THE SAME CALL THE RASTER PATH MAKES, temporal wrapper included -- identical
    // shadow cost, the timing gap is only primary visibility plus this footprint's matrix multiplies.
    // Keyed by pixel, same grid as the raster pass, so the history buffer means the same thing here.
#if AVER_RD_ABLATE == AVER_RD_ABL_SHADOW || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    float sunVis = 1.0;   // ablated: fully lit, no ray
#else
    float3 sunVis = rtShadowTemporal(wpos, N, L, i.pos.xy, dpx, dpy, (uint)max(gRtParams.y, 1.0));
#endif

    // Lambertian exitant radiance, /PI on the direct term -- see rtReflection for what omitting it
    // cost last time (every sunlit surface 3.14x too bright, an exposure-looking bug the white
    // furnace can't catch since it turns the sun off).
    // ---- the bounce loop ----
    // PATH TRACING HERE IS EXTRA RAYS ON THE LOOP ABOVE, not a second renderer: the first hit is
    // already shaded like rtReflection shades its own hit, and each further bounce repeats that,
    // carrying a throughput and adding emission toward the previous surface.
    // COSINE-WEIGHTED so the BRDF's 1/PI and the rendering equation's cosine cancel against the pdf,
    // leaving a plain albedo multiply -- the /PI mistake rtReflection already paid for, reversed.
    // SCREEN-PINNED HASH, no per-frame jitter, matching rtShadow's seeding: the gate oracle compares
    // nine configs bit-exactly, so a frame counter would make each one a different image. Noise is a
    // fixed dither, not something that converges -- honest for a first cut; a temporal accumulator
    // would fix it.
    // THE ENGINE'S OWN BRDF, not a second one: averShadeDirect is the same Cook-Torrance GGX
    // PSMainVoxi uses, so a hand-built AverSurface keeps ray and raster images agreeing on material
    // look. The /PI lives inside it (kdAlbedo/PI) -- the divide rtReflection learned the hard way.
    //
    // THREE CONSTANTS USED TO BE DEFAULTED: reflectance/f90/albedo are per-MATERIAL, but a ray hit
    // had only RtInstance -- inst.albedo is the raster path's flat per-DRAW colour (neutralised to
    // white for an AUTHORED material by the raster shader's multiply convention, a separate known
    // bug), and reflectance/f90 sat at textbook dielectric defaults (0.04/1.0) unconditionally.
    // `mat`, via the new materialIndex/gRtMaterials pair, is real per-material data -- the whole
    // reason that pair exists.
    //
    // METALLIC/ROUGHNESS DELIBERATELY LEFT ON inst.metallic/inst.roughness, NOT mat.metallicFactor/
    // roughnessFactor, despite the task brief calling the former "already real, per-instance". NOT
    // ALWAYS TRUE: for an AUTHORED material inst.metallic/roughness carry the same neutralised-to-1.0
    // placeholder inst.albedo used to (buildAccelerationStructures, VoxiRenderer.cpp, not owned here),
    // so an authored metal can still render fully rough/metallic regardless of authoring. C++-side
    // fix (source RtInstance.metallic/roughness from mat.metallicFactor/roughnessFactor at build
    // time), out of this change's scope. STATED KNOWN GAP, not believed fixed.
    //
    // WHAT THIS DOES NOT VERIFY: whether gRtMaterials is actually populated with each instance's
    // real constants for every draw path (authored .ocmat and the built-in SurfaceLook table) rather
    // than a fallback entry -- C++-side (VoxiRenderer.hpp/.cpp), not confirmable from this file.
    AverSurface s = (AverSurface)0;
    s.N        = N;
    s.V        = -dir;
    s.H        = normalize(s.V + L);
    // MULTIPLY THE PER-DRAW VALUE BY THE MATERIAL FACTOR, DO NOT REPLACE IT -- the raster path's
    // model (PbrShaders.cpp: `s.metallic = saturate(gMaterial.x * a.metallic)`); departing from it
    // turned this whole render white.
    // BOTH TERMS ARE LOAD-BEARING: an AUTHORED (.ocmat) draw has its per-draw colour/metal/rough
    // NEUTRALISED to 1.0 by the caller, so the material factor carries the value (per-draw alone =
    // white); an UNAUTHORED draw has no material, so its fallback factors are 1.0 and the per-draw
    // value carries the colour (factor alone = white). Each is the identity where the other carries
    // the data -- the product is right in both cases, why the raster path multiplies.
    // THE BUG THIS REPLACES: reading `mat.baseColorFactor.rgb` alone shaded every unauthored draw
    // (PTTest's floor, walls, crates, targets) as fallback white while the rasteriser drew them
    // correctly -- reported as "everything is white".
#ifdef AVER_RT_BINDLESS
    // THE STOCK MATERIAL AT A RAY HIT: six of eight maps, slope-blended second layer. Composed like
    // the raster path -- every factor MULTIPLIES its texel rather than replacing it, so a textureless
    // material reduces to the untextured branch and the paths agree. Fallbacks are averSampleMaps'
    // own identity values, so an unbound slot costs one compare.
    //
    // "ALL EIGHT MAPS" AND "NORMAL-MAPPED SHADING NORMAL" IS WHAT THIS COMMENT USED TO SAY, and both
    // halves were wrong. Slots 2 and 7 (normal, layer-1 normal) are sampled below and then DISCARDED:
    // the only reader is averRtPerturbNormal, behind `#define AVER_RT_NORMAL_MAPPING 0` further down.
    // So this path has no normal mapping at all, and a reader trusting the sentence above would look
    // for a bug in the tangent frame that never runs.
    //
    // The samples themselves are almost certainly free -- averRtSampleSlot is pure, so DXC dead-code
    // eliminates a result nothing reads -- which is why this is corrected rather than deleted. The
    // define is 0 because turning it on made ElectricDreams terrain WORSE by a measurement recorded
    // at that define; settling that needs a flat surface with a known-good normal map judged against
    // the path tracer (which gained working normal mapping with a derived tangent frame), not a
    // deletion here.
    // THE EFFECTIVE UV, nothing like the mesh's own UV for a world-aligned material.
    const float2 uvS = averRtSurfaceUV(mat, inst, wpos, N, hitUV);
    // ...and the footprint one pixel covers in that same UV space, from the ray differentials this
    // shader already built for the shadow disc.
    float2 uvGx, uvGy;
    averRtUvGrad(mat, inst, N,
                 gRtVerts[i0].pos, gRtVerts[i1].pos, gRtVerts[i2].pos,
                 gRtVerts[i0].uv,  gRtVerts[i1].uv,  gRtVerts[i2].uv,
                 rdRayDx, rdRayDy, uvGx, uvGy);

    float4 mapBase  = averRtSampleSlot(mat, 0, uvS, uvGx, uvGy, float4(1, 1, 1, 1));
    float4 mapMR    = averRtSampleSlot(mat, 1, uvS, uvGx, uvGy, float4(1, 1, 1, 1));
    float3 mapNrm   = averRtSampleSlot(mat, 2, uvS, uvGx, uvGy, float4(0.5, 0.5, 1, 1)).xyz * 2.0 - 1.0;
    float  mapOcc   = averRtSampleSlot(mat, 3, uvS, uvGx, uvGy, float4(1, 1, 1, 1)).r;
    float3 mapEmis  = averRtSampleSlot(mat, 4, uvS, uvGx, uvGy, float4(0, 0, 0, 1)).rgb;
    // glTF packs occlusion in R, roughness in G, metallic in B -- the same unpack averSampleMaps does.
    float2 metalRough = float2(mapMR.g, mapMR.b);
    float3 normalTS   = float3(mapNrm.xy * mat.normalScale, mapNrm.z);

    // THE SECOND LAYER, blended by SLOPE off the GEOMETRIC normal, not the normal-mapped one
    // (averBlendLayers): "is this a cliff" is a surface property, and a normal map would make the
    // layer choice flicker with every bump. N here is BEFORE perturbation, same as the raster path.
    if (mat.flags & AVER_MAT_SLOPE_BLEND) {
        const float flat01 = saturate(abs(N.z));
        const float lw = 1.0 - smoothstep(mat.slopeBlendLo, mat.slopeBlendHi, flat01);
        if (lw > 0.001) {
            const float2 uv1 = uvS * mat.layer1UvScale;
            if (mat.texIndex[5] != AVER_TEX_UNBOUND)
                mapBase = lerp(mapBase, averRtSampleSlot(mat, 5, uv1, uvGx * mat.layer1UvScale, uvGy * mat.layer1UvScale, mapBase), lw);
            if (mat.texIndex[6] != AVER_TEX_UNBOUND) {
                const float4 mr1 = averRtSampleSlot(mat, 6, uv1, uvGx * mat.layer1UvScale, uvGy * mat.layer1UvScale, mapMR);
                metalRough = lerp(metalRough, float2(mr1.g, mr1.b), lw);
            }
            if (mat.texIndex[7] != AVER_TEX_UNBOUND) {
                const float3 n1 = averRtSampleSlot(mat, 7, uv1, uvGx * mat.layer1UvScale, uvGy * mat.layer1UvScale, float4(0.5, 0.5, 1, 1)).xyz * 2.0 - 1.0;
                normalTS = normalize(lerp(normalTS, float3(n1.xy * mat.normalScale, n1.z), lw));
            }
        }
    }

    // NORMAL MAPPING LAST, only when a map is bound: with none, normalTS is identity (0,0,1) and
    // perturbing is a no-op that still costs a tangent solve.
    // NORMAL MAPPING IS OFF -- A MEASURED DECISION, not an omission. Mean absolute difference against
    // the raster path, whole viewport:
    //                              base colour only   + these slots, no normals   + normal mapping
    //   PTTest (authored flats)          32.08                17.23                    18.21
    //   ElectricDreams (terrain)          6.06                 7.48                    17.19
    // Costs a little on authored surfaces, catastrophic on terrain -- worse than no textures at all.
    // RULED OUT: the frame IS orthonormal (nTS=(0,0,1) reproduces N exactly), the map decodes right
    // (Z saturated positive), normalScale is 1.0, and it isn't the layer-1 blend (no change alone).
    // LEADING SUSPECT: the frame is ROTATED WITHIN THE TANGENT PLANE (nTS=(0,0,1)->N only proves T/B
    // perpendicular to N, not T along +U); or raster doesn't perturb terrain either, in which case
    // raster is the wrong reference and this needs a flat surface with a known-good normal map.
    // Flip to 1 to measure; do not ship at 1 until terrain is explained.
#define AVER_RT_NORMAL_MAPPING 0
#if AVER_RT_NORMAL_MAPPING
    if (mat.texIndex[2] != AVER_TEX_UNBOUND || mat.texIndex[7] != AVER_TEX_UNBOUND) {
        s.N = averRtPerturbNormal(mat, inst, N, normalTS,
                                  gRtVerts[i0].pos, gRtVerts[i1].pos, gRtVerts[i2].pos,
                                  gRtVerts[i0].uv,  gRtVerts[i1].uv,  gRtVerts[i2].uv);
        // The perturbed normal must still face the ray (same reason as the geometric flip above): a
        // normal map can tip a grazing normal past the horizon, shading from behind as a false shadow.
        if (dot(s.N, dir) > 0.0) s.N = -s.N;
    }
#endif

    s.albedo    = inst.albedo * mat.baseColorFactor.rgb * mapBase.rgb;
    s.emissive  = mat.emissiveFactor * mapEmis;
    // THROUGH occlusionStrength, as averBuildSurface does: s.occlusion = lerp(1.0, a.occlusion,
    // gOcclusionStrength). Full strength (tried first) darkened ElectricDreams terrain from
    // 111,101,96 to 73,72,76 against a raster reference of 104,102,100 -- moved further from raster.
    s.occlusion = lerp(1.0, mapOcc, mat.occlusionStrength);
#else
    s.albedo   = inst.albedo * mat.baseColorFactor.rgb;
#endif
#ifdef AVER_RT_BINDLESS
    // metalRough is the SAMPLED pair, unpacked glTF-style: .x roughness (green), .y metallic (blue).
    // Multiplied onto the factors as averStockAuthored does, so an unbound map contributes 1.0.
    s.metallic = saturate(inst.metallic  * mat.metallicFactor  * metalRough.y);
    s.rough    = clamp(inst.roughness * mat.roughnessFactor * metalRough.x, 0.045, 1.0);
#else
    s.metallic = saturate(inst.metallic * mat.metallicFactor);
    s.rough    = clamp(inst.roughness * mat.roughnessFactor, 0.045, 1.0);   // averEvalMaterial's own floor
#endif
    s.ndv      = saturate(dot(s.N, s.V));
    s.f90      = mat.f90;
    s.reflectance = mat.reflectance;
    // HAND-SET: this surface is hand-built, with no material cbuffer to read from (same reason
    // reflectance is carried above rather than read off gMatReflectance). HLSL doesn't
    // zero-initialise a struct, so omitting these would feed averDirectTerms garbage off the stack.
    // A PRIMARY RAY LEAVES THE EYE, so its first hit is always a front face: no refracted ray, no
    // exit interface, TIR cannot arise -- stated here rather than an uninitialised bool reading false.
    s.backFace  = false;
    s.sssWeight = (mat.flags & AVER_MAT_SUBSURFACE) ? saturate(mat.subsurfaceWeight) : 0.0;
    s.sssRadius = (mat.flags & AVER_MAT_SUBSURFACE) ? saturate(mat.subsurfaceRadius) : 0.0;
#ifdef AVER_LAYERED_BSDF
    // THE COAT NEEDS EXPLICIT LINES: this pass zero-inits then hand-sets every field, so a new field
    // silently defaults to 0 (the right OFF state) -- which would have looked correct while meaning
    // "no material has a coat in ray-driven mode". Read from the hit's own material, like the lines
    // above, since this pass has no material cbuffer.
    s.coatWeight = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatWeight)    : 0.0;
    s.coatRough  = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatRoughness) : 0.0;
    s.coatF0     = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatF0)        : 0.0;
#endif
    // Identical shape to averBuildSurface's F0 (PbrShaders.cpp: `lerp(gMatReflectance.xxx, s.albedo,
    // s.metallic)`), mat.reflectance standing in for gMatReflectance -- same value, this pass's own
    // material buffer instead of the per-draw cbuffer a ray hit has no binding for.
    s.F0       = lerp(mat.reflectance.xxx, s.albedo, s.metallic);
    s.F        = fresnelSchlick(saturate(dot(s.H, s.V)), s.F0, s.f90);
    // (1 - transmission), as averBuildSurface does (PbrShaders.cpp, kdAlbedo line): light passing
    // THROUGH the substrate can't also scatter back out, or the material invents energy. Kept
    // identical on both paths deliberately -- a rule honoured by raster but not primary rays is the
    // exact defect shape this tree keeps rediscovering.
    // A BLENDED pane never arrives here (glass is drawn by PSMainVoxi in both modes), so this fires
    // only for an OPAQUE material authoring transmission -- why the rule is against the material
    // field, not the blend mode.
    s.kdAlbedo = (1.0 - s.metallic) * s.albedo * (1.0 - saturate(mat.transmission));
    s.model    = AVER_MODEL_STANDARD;
    s.alpha    = 1.0;
#ifndef AVER_RT_BINDLESS
    // The textured variant sampled a real occlusion map above; this default would overwrite it.
    s.occlusion = 1.0;
#endif

    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    sun.visibility = sunVis;
    // The ray-driven twin of the caustic term above. No blended test here: this pass shades opaque
    // primary hits only -- translucency is drawn by the blended replay through PSMainVoxi.
    sun.visibility *= 1.0 + averCausticFocus(wpos);

    const uint bounces = (uint)max(gPtBounceParams.x, 1.0);

    float3 radiance = averShadeDirect(0.0, s, sun);

    // THE ENVIRONMENT THROUGH THE ENGINE'S OWN INDIRECT TERM, not a diffuse-only line. What stood
    // here (`radiance += s.kdAlbedo * averSkyIrradiance(N) * gAmbient.r`) had two faults, found by
    // the white furnace:
    //   - A WHITE METAL RENDERED BLACK (0.003 vs correct 1.000): averShadeIndirect was never called,
    //     so the environment SPECULAR term (FssEss*ind.specular) didn't exist here at all; a metal's
    //     kdAlbedo=0 gets nothing from a diffuse-only line, so roughness couldn't matter either.
    //   - PATH TRACING DOUBLED THE ENERGY (1.000 -> 1.977): this line added the sky once, then the
    //     bounce loop added it again whenever a ray escaped -- every path escapes on bounce one in
    //     an open scene, so a bounce-depth sweep came back flat and hid the double-count.
    //
    // ONE OWNER FOR THE ENVIRONMENT: this call. The bounce loop no longer adds sky on escape, only
    // surface-to-surface light. Zeroing ind.ambient instead (tried first) measured worse: FmsEms
    // multiplies IRRADIANCE, so zeroing it threw the compensation away and a white metal fell back
    // to the single-scatter curve (0.971 at roughness 0.05 to 0.450 at 1.0) -- FmsEms is specular
    // energy a path-traced bounce doesn't carry.
    // Also makes the ray path structurally match raster: full unoccluded sky ambient plus bounced
    // light on top, without subtracting the sky that bounce occludes. Same approximation, same place.
    float3 R = reflect(dir, N);
    AverIndirect ind;
    ind.ambient      = averSkyIrradiance(N);
    ind.ambientScale = gAmbient.r;

    // ---- DIFFUSE INDIRECT: THE CONE TRACE, exactly as PSMainVoxi does it ----
    // Replaces a ONE-SAMPLE STOCHASTIC BOUNCE THAT COULD NOT CONVERGE -- FROZEN noisy, not merely
    // noisy: the bounce loop drew its direction from `rtHash(i.pos.xy + ...)`, a PURE FUNCTION OF THE
    // PIXEL with no frame term (needed for the gate oracle), so every pixel picked one direction and
    // kept it forever -- a fixed wrong answer, not something a denoiser could average over.
    // WHAT IT LOOKED LIKE: dense static salt-and-pepper on enclosed surfaces, none in the open (an
    // escaped ray adds nothing, variance zero; inside a room every ray lands on a different wall,
    // variance enormous) -- the concrete pit reported "still broken" while the sky nearby was clean.
    // coneTracedIndirect is what PSMainVoxi has always used: DETERMINISTIC (prefiltered clipmap
    // march, no variance), and it hands back a real ambient-occlusion factor the bounce loop never
    // provided (`ind.occlusion = 1.0` below was a stated gap). Makes both paths answer this the same
    // way -- the point of a mode meant to differ only in HOW THE FIRST SURFACE IS FOUND.
    //
    // COST, MEASURED RATHER THAN ASSUMED, and it went the other way from the guess: an earlier
    // version of this comment claimed the cone trace was "cheaper as well as cleaner". It is not --
    // on PTTest (voxelResolution 512, GI Epic) "Voxi ray-driven primary" went 1.3-1.4ms -> 3.0-4.0ms,
    // whole frame 4.13ms median -> 8.86ms. A 6-cone gather over a 512^3 clipmap costs more than three
    // ray-query traversals on hardware with idle ray-query units. Stated because a wrong performance
    // claim in a comment outlives the person who wrote it.
    //
    // STILL THE RIGHT TRADE, for parity not speed: this is the cost PSMainVoxi always paid for the
    // same term. Ray-driven wasn't cheaper before, it was doing something worse and charging less --
    // the two paths now cost the same, the only footing on which "is ray-driven faster" is a
    // question worth asking.
    //
    // WHERE THE COST GOES, AND WHERE IT DOESN'T ANY MORE: this used to say giCones was six,
    // hardcoded. Already false when written -- Renderer::giConesForQuality (Voxi.cpp) has covered
    // Off/Medium=6, Low=3, High=9, Epic=13 since commit fbb3aad, wired through gGiParams.x into the
    // dynamic loop. The ladder already exists and belongs to GI settings. PTTest pins RENDER.GI 4
    // (Epic, 13 cones), so any cost number there is this pass's worst rung.
    //
    // BUT THE CONE COUNT IS *NOT* WHERE THE TIME GOES -- MEASURED, not reasoned. Arithmetic predicted
    // dropping Epic to a lower rung would recover 1.5-2ms of ~3.7ms; it does not. Same camera, only
    // tier differing:
    //     Epic, 13 cones -- ray-driven primary 3.7-3.8ms, whole frame 6.498ms
    //     High,  9 cones -- ray-driven primary 3.5-3.8ms, whole frame 6.066ms
    // Four fewer cones bought ~0.4ms of 6.5ms. The gather is real but not dominant, and **the 3x gap
    // against the rasteriser remains unexplained**. Already tested; don't shave cones on this theory.
    //
    // WHAT TO DO INSTEAD -- AND THE MARKER THIS ASKED FOR ALREADY EXISTS. This used to say "there is
    // NO GPU-timed marker for the raster path's own pixel shading" and call for one to be added. It
    // is there and always was: D3D12Device::beginGpuSpan("scene draw") opens at the tail of
    // beginFrame and closes at the top of endFrame, bracketing every drawMesh the raster path issues
    // -- and "Voxi ray-driven primary" is a CHILD of that same span, so the two are already
    // like-for-like. Nothing needed adding.
    //
    // THE 0.1ms WAS NOT A MISSING MARKER, IT WAS AN EMPTY SPAN. "scene draw" measured 0.1ms because
    // no drawMesh ran at all: the path-traced scene view was suppressing the scene and painting the
    // frame itself, so the "rasteriser" being timed had drawn nothing. D3D12Device.cpp's own
    // suppression warning documents exactly that run. The fix was never instrumentation, it was
    // pinning `--pt 0` so the rasteriser is the thing on screen.
    //
    // SO THE COMPARISON IS: --gpu-timing twice on ONE scene at ONE resolution with a MOVING camera,
    // `--rt-render-mode 1 --pt 0` against `--rt-render-mode 0 --pt 0`, reading "scene draw" from
    // each. Both runs must be checked against the "is painting the scene" log line before either
    // number is believed -- that line, not the timing, is what says which renderer ran.
    //
    // AND THE GAP IS SMALLER THAN THE 3x ABOVE SUGGESTS. This file's own AVER_RD_ABLATE header
    // records ray-driven primary at ~6.7ms of 14.55ms against raster's 7.82ms on PTTest with RT on
    // in both -- so what rasterising primary visibility can recover is that GAP, not the whole
    // "Voxi ray-driven primary" span. That span contains the entire deferred shade (shadow, cones,
    // reflection, sky-occlusion, both sky marches, fog), and the shadow ray in particular is THE SAME
    // CALL the raster path makes, as the comment beside it says.
    float rdAo  = 1.0;
    ind.diffuse = 0.0;
#if AVER_RD_ABLATE == AVER_RD_ABL_GI || AVER_RD_ABLATE == AVER_RD_ABL_ALL
    // ablated: no cone gather
#elif AVER_AO_UNIFIED
    // NOT TRACED HERE AT ALL under the unified ambient ray -- the hemisphere gather below supplies
    // this term from its own hits, and running both would double every interior's bounce light. The
    // cones and the ray answer the SAME question (how much light arrives from the surfaces around
    // this point); the difference is that the ray stopped on geometry while the cone averaged across
    // it. See the ambient block further down, which assigns ind.diffuse.
#else
    if (gVoxelParams.w > 0.5) ind.diffuse = coneTracedIndirect(wpos, N, rdAo);
#endif

    // ---- ENVIRONMENT SPECULAR: A REAL MIRROR RAY NOW, GATED THE SAME WAY PSMainVoxi GATES ONE ----
    // Used to always fall through to PSMainVoxi's cone/flat-sky fallback. Found while chasing a
    // translucency bug: a glass pane's blended replay composites over whatever this pass painted, so
    // an opaque hit under glass in ray-driven mode was a flatter answer than PSMainVoxi's own sharp
    // mirror ray for a qualifying surface -- not a compositing defect (blend/depth/Fresnel unchanged),
    // but the "broad, flat sheet" look the glass/reflection audit traced to this branch.
    //
    // THE OLD COST OBJECTION ("a fourth ray on every pixel") doesn't survive gating identically to
    // PSMainVoxi (`s.rough <= 0.75`): this pays the SAME ray, on the SAME subset of surfaces, the
    // rasteriser already prices into the comparison this mode exists to make. Rough surfaces (most of
    // a scene) still take the cheap cone/sky path unmodified.
    //
    // THE GATE IS PSMainVoxi's PREDICATE VERBATIM (`gShadowParams.z > 0.5 && gRtParams.w > 0.5 &&
    // s.rough <= 0.75`), so the two passes agree on which surfaces earn a mirror ray -- both terms
    // are already true whenever this pass runs at all (rayDrivenActive() requires rtActive_, which
    // sets gShadowParams.z; this pass already reads the geometry table unconditionally above). Kept
    // anyway as the one place either pass is told "no" if the geometry table ever legitimately fails
    // while the TLAS still exists.
    //
    // rtReflectionTemporal's dzdx/dzdy reuse the shadow footprint's ray differential rather than
    // rederiving one: they only predict a REPROJECTED NEIGHBOUR's depth for rtReflectionSpatial's
    // plane-rejection (a filter-quality term, not one that can misplace a ray), so they skip the
    // tangent-plane projection dpx/dpy needed. `rdRayDx`/`rdRayDy` (already built for the shadow call)
    // give the exact displacement wanted; `mul(float4(rdRayDx,0.0), gViewProj).w` reads the
    // DIRECTIONAL part of the same clip.w PSMainVoxi's ddx(viewZ) approximates -- exact here, one
    // extra directional matrix multiply, far cheaper than a second ray differential.
    //
    // EXPECTED COST, STATED not measured (no build/launch under this task's rule): rtReflectionTemporal
    // shares its tile schedule with the shadow ray this pass already pays for, so a smooth pixel's
    // reflection amortises the same way, only where `s.rough <= 0.75` (a minority of most scenes).
    // Nearest reference: the wave-bound-shadow finding's ~2.0ms for one tile-amortised full-screen
    // ray-query pass on ElectricDreams at full coverage; gated to a roughness minority, real cost
    // should land well under that.
    if (gShadowParams.z > 0.5 && gRtParams.w > 0.5 && s.rough <= 0.75) {
        const float rdReflDzdx = mul(float4(rdRayDx, 0.0), gViewProj).w;
        const float rdReflDzdy = mul(float4(rdRayDy, 0.0), gViewProj).w;
        bool specHit = false;
#if AVER_RD_ABLATE == AVER_RD_ABL_REFL || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        float3 refl = float3(0.0, 0.0, 0.0);   // ablated: no mirror ray
#else
        float3 refl = rtReflectionTemporal(wpos, N, R, L, i.pos.xy, s.rough,
                                           rdReflDzdx, rdReflDzdy, specHit);
#endif
        // Same guard as PSMainVoxi's twin (see there for the 13% regression an earlier two-call
        // version cost): smoothstep(0.5,0.75,rough) is 0 at/below 0.5, so a hit reduces the lerp to
        // `refl` and the march is multiplied by zero.
        // THIS ONE IS THE WHOLE SCREEN: PSRayDriven answers primary visibility for every pixel by
        // default, so every smooth surface was paying a march it then discarded.
        const float skyW = smoothstep(0.5, 0.75, s.rough);
        float3 skyR = float3(0.0, 0.0, 0.0);
#if AVER_RD_ABLATE == AVER_RD_ABL_SKY || AVER_RD_ABLATE == AVER_RD_ABL_ALL
        // ablated: no atmosphere march
#else
        if (!specHit || skyW > 0.0) skyR = skyColor(R);
#endif
        ind.specular = lerp(specHit ? refl : skyR, skyR, skyW);
    } else if (gVoxelParams.w > 0.5) {
        // PSMainVoxi's OWN voxel-cone fallback, for the surfaces PSMainVoxi itself falls back for
        // (rough > 0.75, or RT unavailable): past that roughness a one-ray estimate can't resolve a
        // near-hemispherical lobe regardless of which pass is asking.
        float  specAperture = clamp(s.rough * 0.5 + 0.02, 0.02, 0.4);
#if AVER_RD_ABLATE == AVER_RD_ABL_SPECCONE
        // ablated: no specular cone. FULLY OPAQUE (alpha 1) rather than empty, so skyWeight below
        // goes to 0 and this mode measures the CONE ALONE -- an alpha of 0 would instead hand the
        // whole branch to skyColor and measure a march this mode is not trying to price.
        float4 sceneSpec    = float4(0.0, 0.0, 0.0, 1.0);
#else
        float4 sceneSpec    = traceCone(wpos, R, specAperture);
#endif
        // Same fix as PSMainVoxi's identical branch above: HLSL doesn't short-circuit the multiply,
        // so skyColor(R)*(1-sceneSpec.a) wastes a 32-step march when occluded. 0.004 threshold, same
        // reasoning as averFogInscatter's (measured there: "8.9ms -> 1.3ms, 85% of the scene pass").
        const float skyWeight = 1.0 - sceneSpec.a;
        ind.specular        = min(sceneSpec.rgb * gVoxelParams.y, AVER_VOX_MAXRAD);
#if AVER_RD_ABLATE == AVER_RD_ABL_ROUGHSKY
        // ablated: the rough branch's atmosphere march. Mode 4 covers only the REFLECTION branch's;
        // this is the call an enclosed scene actually reaches.
#else
        if (skyWeight > 0.004) ind.specular += skyColor(R) * skyWeight;
#endif
    } else {
        ind.specular        = skyColor(R);
    }
    // REAL AMBIENT OCCLUSION NOW, from the same cone march as the diffuse term. Used to be a
    // hardcoded 1.0 ("a traced bounce is its own occlusion" -- true for a converged path tracer, not
    // for one fixed sample per pixel). Cone trace supplies both now, matching the raster path.
    //
    // AT EPIC `rdAo` IS COMPUTED AND THEN DISCARDED, and that is deliberate rather than an oversight
    // left lying around. The line below prefers the traced sky visibility, so the cone gather's own
    // occlusion goes unused on exactly the tier that pays most for it. It is NOT worth restructuring
    // coneTracedIndirect to skip: the cones still have to be traced for `ind.diffuse`, so the only
    // saving available is the per-cone `occ += c.a * w` accumulation -- roughly two FMAs times
    // thirteen cones, against thirteen cone MARCHES of up to 24 volume samples each. Measured
    // context: the whole cone gather is 4.05 ms of a 50.16 ms frame, and this is a rounding error
    // inside that. Threading a `wantAo` flag through a function mirrored in two files (voxi.hlsl and
    // voxi_gi.hlsli, byte-for-byte) to save it would cost more than it returns.
    // Same substitution as PSMainVoxi's, and it has to be the same or the two primary-visibility
    // paths would disagree about how much sky reaches a surface -- they currently agree to 2.23 MAD
    // and that is worth keeping. Guarded for the same compile-time reason, and kept even though this
    // pass only exists under ray tracing: a reader should not have to prove that to know this builds.
#if AVER_RT && AVER_AO_UNIFIED && AVER_RD_ABLATE != AVER_RD_ABL_SKYOCC
    // ONE GATHER, THREE ANSWERS. See AVER_AO_UNIFIED at the top of this file for the argument.
    if (gAmbientParams.x > 0.5) {
        const AverAmbientTraced amb = rtAmbientTraced(wpos, N, i.pos.xy, (uint)gAmbientParams.x);
        ind.occlusion = amb.open;
        // THE BOUNCE REPLACES THE CONE GATHER, and carries its own occlusion already: averIndirectTerms
        // computes diffBounce as kD * ind.diffuse with NO occlusion factor, which is exactly right for
        // a quantity gathered by rays that were themselves occluded.
        ind.diffuse   = amb.bounce;
        // DIVIDED BY `open` ON PURPOSE, and getting this wrong would darken every shadowed pixel by
        // squaring the occlusion. amb.sky is ALREADY the occluded sky -- the mean over all n samples,
        // where a blocked sample contributed zero -- but averIndirectTerms then multiplies ind.ambient
        // by diffOcc = ind.occlusion * s.occlusion. Pre-dividing here means that multiply puts the
        // occlusion back exactly once: (sky/open) * open == sky.
        //
        // WHY NOT SET ind.occlusion = 1 INSTEAD, which would look simpler: ind.occlusion is also read
        // by averSpecularOcclusion for the specular lobe, and by the material's own s.occlusion map.
        // Flattening it would silently unocclude both.
        //
        // The guard is for the fully-enclosed pixel: open == 0 means sky == 0 too, so the quotient is
        // 0/0 and any finite stand-in gives the correct 0 after the multiply back.
        ind.ambient   = amb.sky / max(amb.open, 1e-4);
    } else {
        ind.occlusion = rdAo;
    }
#elif AVER_RT && AVER_RD_ABLATE != AVER_RD_ABL_SKYOCC
    ind.occlusion    = gAmbientParams.x > 0.5
                     ? rtSkyOcclusionTemporal(wpos, N, i.pos.xy, (uint)gAmbientParams.x)
                     : rdAo;
#else
    // ablated (or no ray tracing): the cone gather's own occlusion, which is what every tier below
    // Epic uses anyway -- so this mode measures the RAY, not the presence of ambient occlusion.
    ind.occlusion    = rdAo;
#endif
    radiance = averShadeIndirect(radiance, s, ind);

    // THE BOUNCE CARRIES THE DIFFUSE RESPONSE, not raw albedo: a metal reflects almost nothing
    // diffusely, so throughput*basecolour would light an interior off surfaces that don't bounce it.
    float3 throughput = s.kdAlbedo;
    float3 bp = wpos;
    float3 bn = N;
    // SKIPPED ENTIRELY WHENEVER THE CONE TRACE ALREADY ANSWERED THIS -- a correctness requirement:
    // both compute the SAME surface-to-surface bounce, so running both would double every interior's
    // brightness. Left reachable for GI-off and for measuring the estimator, but it is ONE cosine
    // sample per pixel per bounce from a hash that can't vary by frame, with no history to accumulate
    // into -- correct but far too undersampled to be an image until it gets per-frame decorrelation
    // and a history buffer it doesn't have. Until then the cone trace above is the better answer.
    const bool rdConeSuppliedDiffuse = gVoxelParams.w > 0.5;
    [loop] for (uint b = 1; b < bounces && !rdConeSuppliedDiffuse; ++b) {
        // A cosine-weighted direction about the surface normal, from the same rtHash the shadow
        // disc uses. Two hashes for the two dimensions, decorrelated by offsetting the pixel.
        float u1 = rtHash(i.pos.xy + float2(b * 17.0, 0.0));
        float u2 = rtHash(i.pos.xy + float2(0.0, b * 23.0));
        float r   = sqrt(u1);
        float phi = 2.0 * PI * u2;
        float3 t  = normalize(abs(bn.z) < 0.999 ? cross(float3(0,0,1), bn) : cross(float3(1,0,0), bn));
        float3 bt = cross(bn, t);
        float3 dirB = normalize(t * (r * cos(phi)) + bt * (r * sin(phi)) + bn * sqrt(max(0.0, 1.0 - u1)));

        RayDesc rb;
        const float bbias = max(gRtParams.z, 1e-4) * (1.0 + length(bp - gCamPos.xyz) * 5e-4);
        rb.Origin = bp + bn * bbias;
        rb.Direction = dirB;
        rb.TMin = bbias;
        rb.TMax = 1.0e7;

        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qb;
        // Opaque lane only -- see AVER_RT_MASK_OPAQUE.
        // Same proof as the primary ray above: this mask cannot see a non-opaque candidate.
        qb.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE_ALL, rb);
        averRtProceedSolid(qb);

        if (qb.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            // Escaped: path ends, deliberately without adding sky (averShadeIndirect already gave
            // this surface the full environment; re-adding it made path tracing read 1.977 vs the
            // furnace's 1.000). The loop carries bounced light off SURFACES only.
            break;
        }

        RtInstance bi = gRtInstances[qb.CommittedInstanceID()];
        uint btri = bi.firstIndex + qb.CommittedPrimitiveIndex() * 3;
        uint b0 = bi.firstVertex + gRtIndices[btri + 0];
        uint b1 = bi.firstVertex + gRtIndices[btri + 1];
        uint b2 = bi.firstVertex + gRtIndices[btri + 2];
        float2 bbary = qb.CommittedTriangleBarycentrics();
        float3 bw = float3(1.0 - bbary.x - bbary.y, bbary.x, bbary.y);
        float3 bnObj = normalize(gRtVerts[b0].nrm * bw.x + gRtVerts[b1].nrm * bw.y + gRtVerts[b2].nrm * bw.z);
        float3 bnWS = normalize(mul(float4(bnObj, 0.0), bi.objectToWorld).xyz);
        if (dot(bnWS, dirB) > 0.0) bnWS = -bnWS;

        bp = bp + dirB * qb.CommittedRayT();
        bn = bnWS;
        // Diffuse response again: a cosine-weighted bounce samples the DIFFUSE lobe, so a metal
        // correctly contributes almost nothing.
        throughput *= (1.0 - saturate(bi.metallic)) * bi.albedo;

        // ONE shadow ray per bounce, no disc -- the penumbra of a surface seen only through two
        // diffuse bounces is not resolvable, and this is the single largest cost in the loop.
        float bshadow = rtShadow(bp, bn, L, i.pos.xy, float3(0,0,0), float3(0,0,0), 1u, 0.0);
        float3 bdirect = averSunRadiance() * saturate(dot(bn, L)) * bshadow / PI;
        radiance += throughput * bdirect;
    }

#if AVER_RD_ABLATE == AVER_RD_ABL_FOG
    // ablated: no aerial perspective and no fog inscatter march.
#elif AVER_RD_ABLATE == AVER_RD_ABL_AERIAL
    // ablated: the aerial march only. Height fog still runs, so the delta against mode 0 is
    // this one term and not the pair.
    radiance = averApplyFogEx(radiance, wpos, false);
#else
    radiance = averApplyFog(radiance, wpos);
#endif

    // Depth for everything that draws AFTER the scene -- the deferred sky, transparentPass, the
    // particle pass. Without it they have nothing to test against and sort against a cleared
    // buffer, which puts smoke in front of walls.
    float4 clip = mul(float4(wpos, 1.0), gViewProj);
    o.depth = clip.w > 1e-6 ? saturate(clip.z / clip.w) : 1.0;
    // UNLIT SUBSTITUTES THE COLOUR AND NOTHING ELSE. Handled here rather than through
    // gShadingModel because a ray hit has no per-draw cbuffer -- the raster path carries the mode in
    // the b1 block PSMainVoxi reads, and this pass never binds it; that asymmetry is why the mode
    // reached the rasteriser and not the renderer that draws the scene by default.
    //
    // AN OVERRIDE RATHER THAN AN EARLY RETURN, deliberately: every AVER_GBUFFER channel below still
    // has to be written, and returning above them would leave velocity, viewZ, normal-roughness and
    // SV_DEPTH unwritten -- the exact "fills only some outputs" fault the struct's own comment warns
    // both return sites about. s.albedo is the SAMPLED base colour; shading a base-colour CONSTANT
    // is what made this mode pure white on every textured mesh.
    o.col   = float4(gViewParams.x > 0.5 ? s.albedo : radiance, 1.0);
#if AVER_GBUFFER
    // clip.w IS the view-space linear depth viewZ wants, reused from o.depth's divide above rather
    // than a second mul. Velocity uses the SAME static-geometry function as PSMainVoxi (see
    // averGBufferVelocity for what it doesn't yet handle); normal is this pass's ray-hit N, not an
    // interpolated vertex normal -- what this feature's task asked for.
    o.velocity        = averGBufferVelocity(wpos);
    o.viewZ            = clip.w;
    o.normalRoughness  = averPackNormalRoughness(N, s.rough);
#endif
    return o;
}
#endif  // AVER_RT

// ================= depth prepass =================
// Same-frame depth-only pass paired with VSMain (VoxiRenderer.hpp's depthPrepassPipeline(),
// D3D12Device::drawMesh) -- same compiled vertex shader as PSMainVoxi's pipelines, so it writes
// EXACTLY the depth the colour pass would produce for the identical triangle.
//
// WRITES NO COLOUR (renderTargetCount=0) and reads only enough to answer "does this survive alpha
// test" -- deliberately short of averEvalMaterial(), which also samples metal-rough/normal/
// occlusion/emissive for Fresnel/GGX setup a depth-only fragment has no use for; calling it just for
// s.alpha would cost four texture fetches instead of at most one.
//
// STILL NOT FREE: every covered pixel pays a gMaterialFlags branch, and an alpha-tested material
// pays one Sample() against gBaseColorMap plus a compare. What it buys is skipping PSMainVoxi
// entirely (shadow lookup, up to eight cone traces, history blending, fog) on every hidden fragment.
//
// DOES NOT EVALUATE AVER_MAT_SLOPE_BLEND's second layer: that flag is landscape-only, and the
// landscape draws through LandscapeRenderer::draw(), never IDevice::drawMesh/drawMeshDepthPrepass
// (one of this feature's three excluded paths, SandboxApp.cpp). If a non-landscape material is ever
// authored with both AVER_MAT_SLOPE_BLEND and AVER_MAT_ALPHA_MASK, alpha would be the FIRST layer's
// alone -- that combination doesn't exist in this tree today.
void PSDepthPrepass(VSOut i) {
#ifdef AVER_MATERIAL_SRV
    if (gMaterialFlags & AVER_MAT_ALPHA_MASK) {
        AverVertex v = averVertexOf(i);
        float2 uv = averSurfaceUV(v);
        float alpha = gBaseColor.a * gBaseColorFactor.a * gBaseColorMap.Sample(gMaterialSampler, uv).a;
        clip(alpha - gAlphaCutoff);
    }
#endif
}

// Depth-only vertex shader for one shadow cascade; which one is in gShadowDraw.x.
float4 VSShadow(VSIn i) : SV_POSITION {
    return mul(mul(float4(i.pos, 1.0), gWorld), gCascadeViewProj[(uint)gShadowDraw.x]);
}

#ifdef AVER_INSTANCE_SRV
// AVER_INSTANCE_SRV is the t-register VoxiRenderer.cpp computed for THIS pipeline's layout when it
// compiled this entry point -- see GraphicsPipelineDesc::instanced in RHIResources.hpp. Two macro
// layers so the register NUMBER (not the literal text "AVER_INSTANCE_SRV") gets pasted after "t".
#define AVER_INST_JOIN2(a, b) a##b
#define AVER_INST_JOIN(a, b) AVER_INST_JOIN2(a, b)
StructuredBuffer<float4x4> gInstanceWorlds : register(AVER_INST_JOIN(t, AVER_INSTANCE_SRV));

// VSShadow's instanced twin: one DrawIndexedInstanced submits every surviving instance of one mesh
// in one cascade, instead of shadowPass calling drawMesh() per instance. World transform comes from
// gInstanceWorlds[instanceID] (VoxiRenderer::shadowPass via drawMeshInstanced), not PerObject's
// gWorld. Everything else is identical to VSShadow.
float4 VSShadowInstanced(VSIn i, uint instanceID : SV_InstanceID) : SV_POSITION {
    float4x4 world = gInstanceWorlds[instanceID];
    return mul(mul(float4(i.pos, 1.0), world), gCascadeViewProj[(uint)gShadowDraw.x]);
}
#endif

// The same depth-only pair again, for the GI-ONLY shadow map. Identical to VSShadow/
// VSShadowInstanced except that they transform into gGiShadowViewProj -- one box over the GI
// volume -- rather than into a cascade selected by gShadowDraw.x. Separate entry points rather than
// a branch because the matrix is picked at pipeline level, not per draw, and a depth-only vertex
// shader is far too hot to spend a dynamic index on.
float4 VSGiShadow(VSIn i) : SV_POSITION {
    return mul(mul(float4(i.pos, 1.0), gWorld), gGiShadowViewProj);
}

#ifdef AVER_INSTANCE_SRV
float4 VSGiShadowInstanced(VSIn i, uint instanceID : SV_InstanceID) : SV_POSITION {
    float4x4 world = gInstanceWorlds[instanceID];
    return mul(mul(float4(i.pos, 1.0), world), gGiShadowViewProj);
}
#endif

// ================= Voxi: voxelisation =================
// The scene is rasterised once per frame with no render target; the pixel shader writes lit
// radiance straight into the volume.
struct VoxOut { float4 pos : SV_POSITION; float3 wpos : TEXCOORD0; float3 nrm : NORMAL; float2 uv : TEXCOORD1; };

// Adapts a VoxOut to AverVertex. V is exactly zero: there is no camera in a voxelisation pass.
// A distinct name, NOT an overload of averVertexOf: FXC converts between compatible structs and
// makes every call ambiguous (error X3067).
AverVertex voxelVertexOf(VoxOut i) {
    AverVertex v;
    v.wpos = i.wpos;
    v.N    = normalize(i.nrm);
    v.V    = float3(0, 0, 0);
    // FALSE, not merely unset: no camera in this pass (V=0 above), so "is the eye inside" has no
    // answer -- false makes the TIR test below inert, correct for a bake with no viewpoint. HLSL
    // leaves a struct member uninitialised, so this must be written.
    v.backFace = false;
    v.uv   = i.uv;
    return v;
}

// Voxelisation vertex shader: outputs world space for the geometry shader to project.
VoxOut VSVoxel(VSIn i) {
    VoxOut o;
    float4 wp = mul(float4(i.pos, 1.0), gWorld);
    o.wpos = wp.xyz;
    o.nrm  = averTransformNormal(i.nrm, gWorld);
    o.uv   = i.uv;
    o.pos  = wp;
    return o;
}

// Projects each triangle along its dominant axis so it covers the most voxels.
[maxvertexcount(3)]
void GSVoxel(triangle VoxOut inp[3], inout TriangleStream<VoxOut> os) {
    float3 n = abs(cross(inp[1].wpos - inp[0].wpos, inp[2].wpos - inp[0].wpos));
    int axis = (n.x > n.y && n.x > n.z) ? 0 : ((n.y > n.z) ? 1 : 2);
    [unroll] for (int k = 0; k < 3; ++k) {
        VoxOut o = inp[k];
        float3 v = voxelUVW(o.wpos);
        float2 p = (axis == 0) ? v.yz : ((axis == 1) ? v.xz : v.xy);
        o.pos = float4(p * 2.0 - 1.0, 0.5, 1.0);
        os.Append(o);
    }
}

#if AVER_MS
// Voxelisation without a geometry shader: the same dominant-axis projection, per primitive.
//
// nr[k] uses averTransformNormal(v.nrm, gWorld), not a plain mul(float4(nrm,0),gWorld), to match
// VSVoxel/VSMain/etc -- see 1856da1, which fixed four OTHER call sites of this bug and missed this
// fifth because MSVoxel compiles only behind AVER_MS, unused at the time. A plain mul is only
// correct under rotation/uniform scale; wrong here would tint an entire surface's bounce light for
// as long as the volume holds it, not just flicker one triangle.
[numthreads(AVER_MS_TRIS, 1, 1)]
[outputtopology("triangle")]
void MSVoxel(uint gid : SV_GroupID, uint gtid : SV_GroupThreadID,
             out vertices VoxOut verts[AVER_MS_TRIS * 3],
             out indices uint3 tris[AVER_MS_TRIS]) {
    uint count = msTriCount(gid);
    SetMeshOutputCounts(count * 3, count);
    if (gtid >= count) return;

    uint3 idx = gIndices.Load3((gid * AVER_MS_TRIS + gtid) * 12);
    float3 wp[3], nr[3];
    float2 uv[3];
    [unroll] for (uint k = 0; k < 3; ++k) {
        MeshVtx v = gVerts[idx[k]];
        wp[k] = mul(float4(v.pos, 1.0), gWorld).xyz;
        nr[k] = averTransformNormal(v.nrm, gWorld);
        uv[k] = v.uv;
    }
    float3 n = abs(cross(wp[1] - wp[0], wp[2] - wp[0]));
    int axis = (n.x > n.y && n.x > n.z) ? 0 : ((n.y > n.z) ? 1 : 2);
    uint o = gtid * 3;
    [unroll] for (uint k = 0; k < 3; ++k) {
        float3 v = voxelUVW(wp[k]);
        float2 p = (axis == 0) ? v.yz : ((axis == 1) ? v.xz : v.xy);
        VoxOut ov;
        ov.wpos = wp[k];
        ov.nrm  = nr[k];
        ov.uv   = uv[k];
        ov.pos  = float4(p * 2.0 - 1.0, 0.5, 1.0);
        verts[o + k] = ov;
    }
    tris[gtid] = uint3(o, o + 1, o + 2);
}
#endif // AVER_MS

// Shades the fragment with shadowed sun plus sky and adds its exitant radiance to the accumulator.
void PSVoxel(VoxOut i) {
    float3 uvw = voxelUVW(i.wpos);
    if (!insideVolume(uvw)) return;
    float3 N = normalize(i.nrm);
    float3 L = normalize(gLightDir.xyz);
    float ndl = saturate(dot(N, L));

    AverLight sun;
    sun.direction  = L;
    sun.radiance   = averSunRadiance();
    // THE GI-ONLY MAP, not the cascades -- see giShadowFactor for why a voxel cannot use them.
    sun.visibility = giShadowFactor(i.wpos, N, ndl);
    AverSurface s = averEvalMaterial(voxelVertexOf(i), sun);
    float3 albedo = averDiffuseAlbedo(s);

    // ---- ONE AXIAL CONE INTO THE PREVIOUS BAKE ------------------------------------------------
    //
    // This injection had two defects that between them made the volume store an AMBIENT TERM rather
    // than bounced light, which is what "there's no bounce lighting" actually looks like from the
    // outside. Both are fixed by the same single cone, so it is traced once and used twice.
    //
    // 1. THE SKY WENT IN UNOCCLUDED. sun.visibility gates the sun term; averSkyIrradiance(N) took
    //    no visibility term at all, and it cannot compute one -- it is a function of the NORMAL and
    //    nothing else, with no world position anywhere in it. So a voxel three walls deep inside the
    //    arcade was handed exactly the same open-sky irradiance as one standing in the courtyard
    //    with the same normal. The stored volume was therefore dominated by a flat,
    //    position-independent term, and a gather over it returns albedo x constant. MEASURED before
    //    this change: isolating the GI's contribution on Sponza gave an image that is essentially a
    //    copy of the albedo texture -- bright where the plaster is white, dark where the stone is
    //    dark -- with no pooling near lit surfaces at all.
    //
    // 2. THERE WAS NO SECOND BOUNCE. Nothing in this module ever read the volume back into the
    //    injection; the only readers of gVoxelTex were the mip filter, the forward-pass gather and
    //    the debug raymarch. Light reflected off exactly one surface and stopped. Indoors that is
    //    most of the light missing -- an arcade is lit by its own walls -- which is why the scene is
    //    dark enough that the eye adaptation pegs at its ceiling every frame.
    //
    // WHY THIS IS LEGAL HERE, checked rather than assumed: voxelTex_ is in ShaderResource for the
    // whole of voxelizePass's raster draws (VoxiRenderer.cpp transitions it to UnorderedAccess only
    // just before the resolve), and bindings_ -- the set this pass binds -- already carries the SRV
    // over the full mip chain at t0. So this needs no new binding, no new descriptor and no barrier
    // change. The clear at the top of the pass zeroes mip 0 ONLY, and the cone's first sample lands
    // at mip ~1.2 (dist starts at two voxels, and a 60-degree aperture is already wider than one
    // voxel there), so what it reads is the previous bake, still intact in mips 1..N.
    //
    // WHAT IT COSTS AND WHY IT CONVERGES: one cone per injected fragment, against the thirteen the
    // forward gather already runs per pixel. Each rebuild adds albedo x (light already in the room),
    // and albedo < 1, so the series is geometric and settles -- the same argument radiosity has
    // always rested on. AVER_VOX_MAXRAD still bounds it if a scene ever tries to break that.
    const float4 room = traceCone(i.wpos, N, AVER_VOX_INJECT_APERTURE);
    // A cone that terminated on solid geometry saw no sky; one that ran out of volume saw all of it.
    // traceCone accumulates occlusion in .a and treats leaving the volume as unoccluded, which is
    // the correct reading of "this voxel is open to the sky" for exactly this purpose.
    const float skyVis = saturate(1.0 - room.a);

    // Exitant radiance, not radiosity: the sun term is an irradiance so it takes the 1/PI, the sky
    // term is already a radiance so it does not, and the feedback term is already exitant radiance
    // gathered from surfaces that have themselves been through this same shader.
    float3 radiance = albedo * (sun.radiance * ndl * sun.visibility / PI
                                + averSkyIrradiance(N) * gAmbient.r * skyVis
                                + room.rgb * AVER_VOX_FEEDBACK);
    radiance = clamp(radiance, 0.0, AVER_VOX_MAXRAD);

    // insideVolume() is inclusive of 1.0, and conservative raster does produce uvw == 1.0 exactly.
    uint3 c = min(uint3(uvw * gVoxelParams.x), (uint)gVoxelParams.x - 1);
    uint3 a = uint3(c.x * 4, c.y, c.z);
    uint prev;
    InterlockedAdd(gVoxelAccum[a],                (uint)(radiance.r * AVER_VOX_FIXED), prev);
    InterlockedAdd(gVoxelAccum[a + uint3(1,0,0)], (uint)(radiance.g * AVER_VOX_FIXED), prev);
    InterlockedAdd(gVoxelAccum[a + uint3(2,0,0)], (uint)(radiance.b * AVER_VOX_FIXED), prev);
    InterlockedAdd(gVoxelAccum[a + uint3(3,0,0)], 1u, prev);   // fragments covering this voxel
}

// Zeroes the accumulator before injection.
[numthreads(4,4,4)]
void CSClear(uint3 id : SV_DispatchThreadID) {
    uint3 a = uint3(id.x * 4, id.y, id.z);
    [unroll] for (uint k = 0; k < 4; ++k) gVoxelAccum[a + uint3(k,0,0)] = 0;
}

// Turns the fixed-point sums into mip 0 of the filterable RGBA16F volume: the mean radiance of the
// fragments that covered each voxel.
[numthreads(4,4,4)]
void CSResolve(uint3 id : SV_DispatchThreadID) {
    uint3 a = uint3(id.x * 4, id.y, id.z);
    uint n = gVoxelAccum[a + uint3(3,0,0)];
    if (n == 0) { gVoxelUAV[id] = 0.0; return; }
    float3 s = float3(gVoxelAccum[a], gVoxelAccum[a + uint3(1,0,0)], gVoxelAccum[a + uint3(2,0,0)]);
    gVoxelUAV[id] = float4(s / (AVER_VOX_FIXED * (float)n), 1.0);   // alpha = occupancy
}

// ================= Voxi: mip filtering =================
// Box-filters radiance and occupancy from one mip into the next. The mip binding set puts a
// SINGLE-MIP view of the source at t0 and the destination level at u0.
[numthreads(4,4,4)]
void CSMip(uint3 id : SV_DispatchThreadID) {
    int3 s = int3(id) * 2;
    float4 a = 0;
    [unroll] for (int x=0;x<2;++x)
    [unroll] for (int y=0;y<2;++y)
    [unroll] for (int z=0;z<2;++z)
        a += gVoxelTex.Load(int4(s + int3(x,y,z), gSrcMip));
    gVoxelUAV[id] = a * 0.125;
}

// Debug view: raymarches the volume straight to screen over the sky. Returns linear radiance.
float4 PSVoxelDebug(SkyOut i) : SV_TARGET {
    float4 far = mul(float4(i.ndc, 1.0, 1.0), gInvViewProj);
    float3 ray = normalize(far.xyz / far.w - gCamPos.xyz);
    float voxelWorld = 1.0 / (gVoxelOrigin.w * gVoxelParams.x);
    float4 acc = 0;
    float t = 0;
    [loop] for (int s = 0; s < 256; ++s) {
        if (acc.a >= 0.98) break;
        float3 uvw = voxelUVW(gCamPos.xyz + ray * t);
        t += voxelWorld;
        if (t > gVoxelParams.z * 2.0) break;
        if (!insideVolume(uvw)) continue;
        float4 v = gVoxelTex.SampleLevel(gVoxelSamp, uvw, 0);
        acc += (1.0 - acc.a) * v;
    }
    float3 bg = skyColor(ray);
    float3 col = acc.rgb + bg * (1.0 - acc.a);
    return float4(col, 1.0);
}
