// voxi_rt.hlsli -- the ray-traced lighting estimators voxi.hlsl's entry points shade with, split out
// of voxi.hlsl's #if AVER_RT region. This is everything that region held BEFORE the ReSTIR GI block
// (voxi_restir.hlsli, #include'd immediately after this file, still inside the same guard) -- the
// scene/material resource layer a ray hit reads, the RayQuery helpers built on it, the sampling
// primitives, and the estimators themselves. None of it is ReSTIR; ReSTIR calls INTO it.
//
// WHAT THIS FILE HOLDS, IN DECLARATION ORDER:
//   - the RT scene/geometry/material resources: gScene (t2), RtVertex/RtInstance/RtMaterial,
//     gRtVerts/gRtIndices/gRtInstances (t3/t4/t5), gRtMaterials (t9); under AVER_RT_BINDLESS, the
//     bindless texture table gRtTextures (t0, space1) and the AVER_RD_ABLATE measurement-mode enum;
//     the material-graph adapter built on top of it (averRtSampleSlot, averRtSampleSlotGraph,
//     averRtUvGrad, averRtSurfaceUV, averRtPerturbNormal).
//   - the cutout-aware RayQuery helpers every ray in this file, and in voxi_restir.hlsli, calls
//     through: averRtCandidateOpaque, averRtProceedSolid.
//   - the ray-traced history textures: gRtShadowHist/gRtShadowHistOut (t6/u2), gAoHist/gAoHistOut
//     (t11/u4), gAoHitDistOut (u5), gNrdAo (t14), gGiRadianceOut/gNrdGi (u9/t15),
//     gRtReflHist/gRtReflHistOut (t7/u3), gGBufNormalHist (t10, under AVER_GBUFFER_HISTORY).
//   - the low-discrepancy sampling primitives: rtHash, rtRadicalInverse2, rtDiscSample.
//   - the estimators: rtShadow; AverAmbientTraced/rtAmbientTraced/rtSkyOcclusion;
//     rtReprojectHistory/rtReprojectAo; rtAoSpatial/rtShadowSpatial; rtSkyOcclusionTemporal;
//     averShadowLum/averShadowTint/rtShadowTemporal; rtReflection.
//
// WHAT MUST PRECEDE THIS FILE'S #include LINE IN voxi.hlsl:
//   - cbuffer VoxiFrame and the volume/shadow/backdrop resources and defines that sit above the
//     #if AVER_RT guard in voxi.hlsl -- gVoxelParams, gRtParams, gRtHistParams, gAmbientParams,
//     gRtDenoiseParams, gGiRestirParams and the rest of the cbuffer; AVER_VOX_*, AVER_AO_*,
//     AVER_REFL_MIRROR_ROUGH, AVER_RT_MASK_* defines; gShadowTex/gShadowSamp (t1/s1); gGiShadowTex
//     (t8); gBlendBackdrop (t10); averCausticFocus. All unconditional, all declared before AVER_RT
//     opens.
//   - the #if AVER_RT guard itself. This file carries no #if AVER_RT of its own -- like
//     voxi_restir.hlsli, it is plain, guard-free text spliced into an already-open conditional,
//     exactly as if it had never left voxi.hlsl.
//   - the shared and material preludes voxi.hlsl is textually the tail of: gCamPos, gViewProj,
//     gInvViewProj and the rest of the camera block; gMaterialSampler; the AVER_MAT_* flag bits;
//     averVolumeTransmittance, averSunRadiance, averSkyIrradiance, averSkyRadianceCheap, gAmbient,
//     PI. See voxi.hlsl's own file-level comment for what those preludes are and why a bad
//     declaration anywhere in this chain fails every entry point at once, not just the ones that
//     call this file's functions.
//
// WHAT DEPENDS ON THIS FILE:
//   - voxi_restir.hlsli, included immediately after this one (still inside AVER_RT):
//     giTraceInitialCandidate traces gScene directly through averRtProceedSolid and shades hits
//     through the same material adapter this file builds -- its own header comment says so.
//   - the backdrop/refraction glue and the reflection temporal/spatial wrappers further down
//     voxi.hlsl, still inside AVER_RT, call rtReflection and rtShadow.
//   - PSMainVoxi and PSRayDriven, after AVER_RT closes, call rtShadowTemporal and
//     rtSkyOcclusionTemporal directly.
//
// THE ONE FORWARD DECLARATION IN THIS FILE, CARRIED OVER FROM voxi.hlsl UNCHANGED AND FOR THE SAME
// REASON: `voxelUVW` and `insideVolume` are declared but not defined here, just above
// rtAmbientTraced, because their real definitions sit with the cone tracer in voxi.hlsl's untouched
// tail, well over a thousand lines below where AVER_RT closes. Do not "fix" this by moving the
// definitions up, and do not delete the forward declaration as redundant -- both halves are
// load-bearing exactly as split, and the declaration site still carries voxi.hlsl's own comment
// explaining why.
//
// PRE-EXISTING, NOT INTRODUCED BY THIS SPLIT: gGBufNormalHist below is declared at register t10,
// the same slot gBlendBackdrop uses above the AVER_RT guard in voxi.hlsl. Both sites call their own
// slot a guess pending confirmation against the C++ side (same reasoning as gRtMaterials' t9).
// Reported here rather than fixed -- moved verbatim, unchanged.

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
// THE SAME RAY'S HIT DISTANCE, WRITTEN AND NEVER READ BACK by anything in this file.
//
// [0,1] as a fraction of the ray's own TMax (gVoxelParams.z, Settings::giMaxDistance): 1 means
// every sample escaped to the sky, 0 means every sample hit something at the shading point. It is
// deliberately NOT normalised the way any particular denoiser wants it -- an external filter's
// normalisation curve is that filter's business, and baking one in here would make this texture
// mean whatever the current consumer happens to be. A consumer that wants world units multiplies
// by giMaxDistance, which it has.
//
// WHY THE OCCLUSION ALONE IS NOT ENOUGH, since that is the obvious question. A filter that only
// knows "this pixel is 30% occluded" cannot tell a wide, distant opening from a tight crevice, so
// it cannot choose how far to spread a sample without crossing an edge that is really there.
// Distance is what sets that radius. NVIDIA NRD calls this IN_DIFF_HITDIST and REBLUR_DIFFUSE_
// OCCLUSION does not run without it -- see modules/render.nrd/README.md.
//
// GUARDED BY gRtDenoiseParams.w, the same flag as the u4/t11 pair above, because it is allocated
// and bound under exactly the same condition (VoxiRenderer::aoHistoryWanted). Low and Medium do
// not trace this ray at all, so at those tiers the slot is genuinely absent and must not be
// touched -- writing a null UAV is undefined, not merely wasted.
RWTexture2D<float>  gAoHitDistOut : register(u5);

// t14: the same signal, one frame later, after NVIDIA NRD has filtered it.
//
// DECLARED HERE RATHER THAN BESIDE ITS REGISTER NEIGHBOURS t12/t13, which sit ~700 lines further
// down with the ReSTIR block: HLSL has no forward declarations, and rtSkyOcclusionTemporal -- the
// only reader -- comes BEFORE them. Beside the u5 it filters is also where it explains itself, since
// the two textures hold the same quantity in the same encoding (see AverAmbientTraced::hitDist).
//
// ROUTINELY ABSENT, AND THE READER MUST TEST FOR IT. The pass needs NRD in the build, a D3D12
// device (NRD wants its constant buffer and samplers in register space 1, which Vulkan refuses on
// purpose) and the G-buffer, which is off by default. VoxiRenderer clears this slot on any frame it
// did not denoise, so GetDimensions() returning 0 means "not denoised THIS frame" rather than
// "never" -- a frozen last-good image would be the worse failure.
Texture2D<float>    gNrdAo        : register(t14);

// u9/t15: the ReSTIR GI radiance on its way to NRD's REBLUR_DIFFUSE, and on its way back.
//
// rgb = the indirect diffuse radiance giRestirIndirect produced for this pixel, a = the NORMALISED
// distance the candidate ray travelled to find it. NRD packs and unpacks that pair itself, so both
// channels are written raw -- and the normalisation is by the SAME giMaxDistance the sky-occlusion
// hit distance uses (gVoxelParams.z), which is what lets one hitDistParams describe both signals.
//
// WHY THE RADIANCE NEEDS ITS OWN TEXTURE rather than riding the occlusion one: they are different
// quantities with different denoisers. Occlusion is a scalar NRD filters with
// REBLUR_DIFFUSE_OCCLUSION; this is colour, filtered by REBLUR_DIFFUSE, which keeps its own separate
// history. Sharing a texture would mean sharing a history, and the two signals decorrelate.
//
// BOTH ARE ABSENT UNLESS ReSTIR GI IS ON *AND* NRD IS RUNNING, and the readers test for that the
// same way t14's does -- a null-filled Texture2D reports zero dimensions. Writing is guarded on
// gGiRestirParams.x, which already says whether this frame bound the ReSTIR slots at all.
RWTexture2D<float4> gGiRadianceOut : register(u9);
Texture2D<float4>   gNrdGi         : register(t15);

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
    // MEAN DISTANCE TRAVELLED, as a fraction of TMax, over ALL n samples -- a sample that escaped
    // contributes a full 1.0, not zero and not nothing. That is what makes it a distance rather
    // than a distance-among-the-hits: "nothing in the way for the whole length of the ray" is the
    // largest distance this ray can report, and averaging only over the hits would say the opposite
    // in exactly the open sky where the answer matters least and the error shows most.
    //
    // Computed on EVERY path, unlike `sky`/`bounce` above, and it costs one mad per sample: the
    // consumer is an external denoiser that is either on or off for the whole frame, so making it
    // a second compile-time variant of this function would double the permutations to save an
    // instruction the ray's own BVH traversal dwarfs by four orders of magnitude.
    float  hitDist;
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
    res.open = 0.0; res.sky = float3(0, 0, 0); res.bounce = float3(0, 0, 0); res.hitDist = 0.0;
    // Hoisted out of the loop because the accumulation below divides by it, and because a TMax of
    // zero would otherwise be a divide by zero on a scene whose giMaxDistance was authored to
    // nothing -- max(...,1.0) is the same floor the RayDesc uses a few lines down.
    const float aoTMax = max(gVoxelParams.z, 1.0);
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

        // ONE `if`, BOTH ANSWERS. CommittedRayT() is meaningful only on a hit; on a miss the ray
        // ran its whole length, which IS the distance and is why the miss branch adds aoTMax rather
        // than skipping the term.
        //
        // ON THE SHIPPING PATH THIS IS A FIRST HIT, NOT THE NEAREST ONE, and that is worth knowing
        // rather than discovering. AVER_AO_UNIFIED is 0, so the query above carries
        // ACCEPT_FIRST_HIT_AND_END_SEARCH: traversal stops at whatever triangle it reaches first
        // within TMax, which need not be the closest. The distance is therefore an upper-bounded
        // estimate, never longer than the true one and usually equal to it in the enclosed
        // geometry this term exists for. Making it exact means dropping the flag and paying full
        // traversal on the most incoherent ray in the frame -- 5.37 ms for one ray, per this
        // function's own measurement -- to sharpen a filter radius. Not worth it; recorded so the
        // next reader does not assume precision that is not here.
        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            res.open    += 1.0;
            res.hitDist += aoTMax;
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
        // OUTSIDE THE #if, so the hit distance is accumulated on both paths. Under AVER_AO_UNIFIED
        // the `else` above has already read CommittedRayT() for the bounce lookup; here it is read
        // again rather than threaded through, because a hit's T is a register the query already
        // holds and the alternative is a variable that exists only under one define.
        if (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
            res.hitDist += min(q.CommittedRayT(), aoTMax);
    }

    // ALL THREE DIVIDE BY n, NOT BY THEIR OWN HIT COUNTS. These are Monte Carlo estimates of
    // hemisphere integrals, so a sample that missed contributes zero to `bounce` and a sample that
    // hit contributes zero to `sky` -- that is the estimate, not a gap in it. Dividing `sky` by the
    // miss count instead would return the mean brightness of the visible sky and silently drop the
    // occlusion, which is the very thing being measured.
    const float inv = 1.0 / (float)n;
    res.open    *= inv;
    res.sky     *= inv;
    res.bounce  *= inv;
    // Divided by TMax as well, which is what makes it the [0,1] fraction the declaration promises
    // rather than a world distance. saturate() because CommittedRayT can land a hair past TMax on a
    // ray that hit almost exactly at its own limit, and a consumer told to expect [0,1] should get
    // [0,1] rather than 1.0000001.
    res.hitDist = saturate(res.hitDist * inv / aoTMax);
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

// `coneAo` is the cone gather's own occlusion -- smooth, deterministic, and ALREADY COMPUTED at
// every tier. At Epic it was computed and then thrown away (coneTracedIndirect says exactly that of
// its own `rdAo`). Passing it in as the PRIOR therefore costs nothing.
float rtSkyOcclusionTemporal(float3 wpos, float3 N, float2 pixel, uint rays, float coneAo) {
    // rtAmbientTraced RATHER THAN THE rtSkyOcclusion WRAPPER, and the difference is the hit
    // distance: the wrapper exists to throw away everything but `.open`, and this function is now
    // one of the callers its own comment describes as "meaning to use them". Identical cost -- the
    // wrapper was a field select, not a second trace.
    const AverAmbientTraced amb = rtAmbientTraced(wpos, N, pixel, rays);
    const float fresh = amb.open;
    // gRtDenoiseParams.w, NOT gRtHistParams.x, and the difference matters: the shadow and
    // reflection pairs exist at every ray-tracing tier, but this one is allocated only where the
    // ray is actually traced -- High and Epic. At Low and Medium the slots are genuinely absent,
    // and touching a null UAV is undefined rather than merely wasteful. Returning the fresh trace
    // is also the correct answer there: with no history there is nothing to accumulate against.
    // No AO history allocated at all (Low/Medium): the cone's answer, not a lone binary ray.
    if (gRtDenoiseParams.w < 0.5) return coneAo;

    const float curDepth = mul(float4(wpos, 1.0), gViewProj).w;
    // SEEDED FROM THE CONE, NOT FROM THE RAW RAY. At one ray `fresh` is BINARY -- 0 or 1 -- so a
    // pixel with no usable history put a coin flip on screen and then wrote that coin flip into the
    // history for its neighbours to read next frame. That is the measured artefact: with a
    // scale-free metric (each image normalised to its OWN mean, because both an absolute-code metric
    // and a divide-by-local-median one are confounded by the brightness an ablation changes -- two
    // rankings had to be discarded before this was measured honestly), ablating this ray takes
    // bright speckle from 0.071% of pixels to 0.012%. That is 83% of it, and below the 0.038%
    // measured with ray tracing off altogether.
    //
    // The cone gather answers the SAME question -- how much of the hemisphere is open -- smoothly,
    // deterministically, and it is what every tier below Epic already ships. So it is the right
    // thing to stand on when the traced estimate has nothing to average against: the pixel starts
    // from a plausible smooth value and the traced samples refine it over the following frames,
    // instead of starting from noise and passing noise on.
    float vis = coneAo;
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
        // History exists, so the traced sample is the UPDATE and the prior above goes unused.
        vis = lerp(fresh, histV, weight);
    }
    // ---- NRD's ANSWER WINS WHEN THERE IS ONE, and it replaces the blend above rather than
    // filtering it ----
    //
    // gNrdAo holds LAST frame's hit distance run through NVIDIA's REBLUR_DIFFUSE_OCCLUSION. Both
    // quantities are the same thing in the same units: AverAmbientTraced::hitDist is the mean
    // distance travelled as a FRACTION OF TMax with an escaping sample contributing a full 1.0, so
    // 1 is "nothing in the way" and 0 is "blocked at once" -- exactly the polarity and range of the
    // openness `vis` carries. No remap, and none should be invented here.
    //
    // IT REPLACES THE EMA RATHER THAN FEEDING IT. Handing a denoised value back into the 0.97
    // history blend would be feeding a filter its own output -- the IIR trap the history write
    // below spends a paragraph avoiding -- and it would also double-count NRD's own temporal
    // accumulation, which is the whole thing NRD's permanent pool exists to do.
    //
    // THE ZERO-DIMENSIONS TEST IS THE BOUND-OR-NOT SIGNAL, the same one averBlendBackdropValid uses
    // for the backdrop: the pass is absent on Vulkan, absent without NRD in the build, and absent
    // without the G-buffer, so the shader cannot assume t14 exists. A null-filled Texture2D reports
    // zero dimensions, which the descriptor already tells us -- no fourth mirror of the constant
    // block, which is a trap this file has been caught by before.
    uint nrdW = 0, nrdH = 0;
    gNrdAo.GetDimensions(nrdW, nrdH);
    if (nrdW > 0u && nrdH > 0u) {
        // STILL WRITTEN TO THE HISTORY BELOW, because that history is what the NEXT frame's
        // reprojection reads and what the pass falls back to the moment NRD stops running -- a
        // frame where the G-buffer is switched off would otherwise resume from a stale average.
        vis = saturate(gNrdAo.Load(int3(pixel, 0)).r);
    }

    // The ACCUMULATED value, not the fresh one -- writing `fresh` here would restart the average
    // every frame and buy nothing, the same trap the shadow path documents.
    //
    // AND RAW, NEVER FILTERED, which is the same single most important line the shadow path has:
    // feeding the spatially filtered result back into the history turns this into an IIR filter
    // whose artefacts compound every frame. The filter applies on the way OUT, below.
    gAoHistOut[uint2(pixel)] = float2(vis, curDepth);
    // THE RAW MEASUREMENT, NOT THE ACCUMULATED ONE -- the exact opposite of the line above it, and
    // deliberately so. The history write is accumulated because THIS shader is the thing that
    // consumes it next frame and wants the average. This one goes to an external denoiser that
    // keeps its own history and does its own accumulation; handing it a value already blended
    // against ten previous frames would be feeding a filter its own output, which is the IIR trap
    // the line above spends a paragraph avoiding.
    //
    // AFTER the gRtDenoiseParams.w early-out above, which is what makes the slot safe to touch.
    //
    // IT INHERITS THE BLENDED-REPLAY DOUBLE WRITE, and that is worth stating rather than leaving to
    // be found. PSMainVoxi runs for the translucent replay too (its own PSO, same pixel shader), so
    // a glass pane in front of an opaque surface writes this texel a second time with the PANE's
    // measurement. The two history writes above have exactly the same problem and have had it since
    // they were written -- it is the "non-atomic double write" already on the denoising task list,
    // where the fix is to suppress the history writes on the blended pass, for all of them at once.
    //
    // IT IS WORSE HERE THAN THERE, THOUGH, WHICH IS WHY THIS NOTE EXISTS. A wrong value in the
    // history gets blended away over the following frames. This one goes straight out as THIS
    // FRAME'S measurement of this pixel, so an external denoiser is handed the pane's distance for
    // the surface behind it with nothing to average it against. Guarding only this write would make
    // the three inconsistent for no gain; fix them together or not at all.
    gAoHitDistOut[uint2(pixel)] = amb.hitDist;
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
    // Neighbourhood statistics for the clamp below, EXCLUDING the centre -- the question is whether
    // the centre disagrees with its neighbours, and a statistic containing it cannot answer that.
    // Free: these taps are already loaded, depth-tested and crease-tested.
    float nSum = 0.0, nSum2 = 0.0, nCount = 0.0;
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
            nSum += st.x; nSum2 += st.x * st.x; nCount += 1.0;
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
    // THE OUTLIER CLAMP THIS FILTER NEVER HAD, and its AO twin already argues the case: a Gaussian
    // is LINEAR, so an isolated extreme is SPREAD across the kernel rather than removed. That is
    // why raising the shadow ray count from 1 to 16 moved the impulse metric not at all (0.080% ->
    // 0.080% of pixels past 64 codes) -- more angular samples cannot fix an unclamped impulse, and
    // measuring ray count was what ruled the sampler out and pointed at the filter.
    //
    // Same shape as rtAoSpatial: mean +/- 2 sigma over the accepted neighbours, a floor under sigma
    // so a perfectly flat neighbourhood does not erase the centre's own detail along with its noise,
    // and a three-tap minimum so a silhouette pixel -- where the depth and crease rejects have
    // thrown most of the kernel away -- is not clamped against a statistic built out of nothing.
    float clamped = centre;
    if (nCount >= 3.0) {
        const float mean  = nSum / nCount;
        const float nsig  = sqrt(max(nSum2 / nCount - mean * mean, 0.0));
        const float k     = 2.0 * max(nsig, 0.02);
        clamped = clamp(centre, mean - k, mean + k);
        acc += clamped - centre;   // the blurred average carried the UNCLAMPED centre at weight 1
    }

    const float velPx = reproj ? length(centrePx - pixel) : 0.0;
    const float trust = gRtDenoiseParams.z > 0.0 ? saturate((3.0 - velPx) * gRtDenoiseParams.z) : 1.0;
    // Clamped first, then blended, so the impulse is gone before the linear filter ever sees it --
    // and so a run with the blur turned down still gets the clamp.
    return lerp(clamped, acc / wsum, saturate(gRtDenoiseParams.y) * trust);
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
        // SATURATED. `tint` is this pixel's RAW ratio v/lum, deliberately unfiltered (see
        // averShadowTint), while `vis` has been through a 0.97 temporal blend and a spatial filter.
        // The identity tint * lum == v holds only while vis == lum, and the two are designed to
        // disagree -- so wherever the filters move vis away from the fresh trace, the product is
        // free to exceed the transmittance it came from. averShadowTint bounds its DENOMINATOR at
        // 1e-4 and not its quotient: the per-channel maximum is 1/0.0722 = 13.85 on blue. A deeply
        // shadowed pixel with vis 0.05 and a saturated tint therefore reports 0.69 -- two thirds
        // lit -- and that lands in AverLight::visibility, which material_prelude documents as
        // "(1,1,1) = fully lit" and multiplies straight into the sun's radiance.
        //
        // A visibility cannot exceed one. Saturating says so, and costs nothing.
        return saturate(rtShadowSpatial(vis, wpos, N, pixel, curDepth) * tint);
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
    // The same saturate, and for the same reason -- see the tiled branch above.
    return saturate(rtShadowSpatial(vis, wpos, N, pixel, curDepth) * tint);
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
    // frameJitter, NOT 0.0, AND THE ZERO WAS A FROZEN SAMPLE. rtShadow builds its disc angle as
    // rtHash(pixel) * 2pi + frameJitter, so passing 0 here made this ray a pure function of the
    // PIXEL: one direction on the sun's disc, identical on every frame for the life of the process.
    // At one ray that is a binary value, and temporal accumulation downstream converges TO it rather
    // than averaging it away -- an estimator cannot be denoised into correctness when every sample
    // it will ever take is the same sample.
    //
    // This is the identical defect fixed for the PRIMARY shadow ray in 421a01e3, which left this
    // inner one behind. MEASURED consequence: of the isolated bright outliers left in a shadowed
    // frame, 60.7% sit on the SAME pixels two frames running, and 98.0% of the dark ones do -- the
    // signature of a deterministic per-pixel error rather than of sampling noise.
    float3 shadow = rtShadow(hitPos, nWS, L, pixel, float3(0,0,0), float3(0,0,0), 1u, frameJitter);

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
