// voxi_rt.hlsli -- RT lighting estimators split out of voxi.hlsl's #if AVER_RT region: everything that
// region held BEFORE the ReSTIR GI block (voxi_restir.hlsli, #include'd right after, same guard). The
// scene/material resource layer a ray hit reads, the RayQuery helpers built on it, the sampling
// primitives, and the estimators. None of it is ReSTIR; ReSTIR calls INTO it.
// HOLDS, in order: RT scene/geometry/material resources (gScene t2, RtVertex/RtInstance/RtMaterial,
// gRtVerts/gRtIndices/gRtInstances t3/t4/t5, gRtMaterials t9; under AVER_RT_BINDLESS the bindless
// texture table gRtTextures t0/space1 and the AVER_RD_ABLATE measurement enum; the material-graph
// adapter averRtSampleSlot(Graph)/averRtUvGrad/averRtSurfaceUV/averRtPerturbNormal); the cutout-aware
// RayQuery helpers every ray here (and in voxi_restir.hlsli) calls through: averRtCandidateOpaque,
// averRtProceedSolid; the RT history textures (gRtShadowHist/Out t6/u2, gAoHist/Out t11/u4,
// gAoHitDistOut u5, gNrdAo t14, gGiRadianceOut/gNrdGi u9/t15, gRtReflHist/Out t7/u3, gGBufNormalHist
// t10 under AVER_GBUFFER_HISTORY); the LD samplers rtHash/rtRadicalInverse2/rtDiscSample; the
// estimators rtShadow, lamp struct RdLocalLight + rdLocalIrradiance/rdLocalShadow,
// AverAmbientTraced/rtAmbientTraced/rtSkyOcclusion, rtReprojectTexel/History/Ao,
// rtAoSpatial/rtShadowSpatial, rtSkyOcclusionTemporal, averShadowLum/Tint/rtShadowTemporal, rtReflection.
// MUST PRECEDE THIS #include IN voxi.hlsl: cbuffer VoxiFrame and the volume/shadow/backdrop
// resources/defines above the AVER_RT guard (gVoxelParams, gRtParams, gRtHistParams, gAmbientParams,
// gRtDenoiseParams, gGiRestirParams, AVER_VOX_*/AVER_AO_*/AVER_REFL_MIRROR_ROUGH/AVER_RT_MASK_* defines,
// gShadowTex/gShadowSamp t1/s1, gGiShadowTex t8, gBlendBackdrop t10, averCausticFocus); the #if AVER_RT
// guard itself (this file carries none of its own -- plain text spliced into an already-open
// conditional); the shared/material preludes (gCamPos, gViewProj, gInvViewProjRel, gMaterialSampler,
// AVER_MAT_* flags, averVolumeTransmittance, averSunRadiance, averSkyIrradiance, averSkyRadianceCheap,
// gAmbient, PI -- see voxi.hlsl's file comment for why a bad declaration here fails every entry point).
// DEPENDS ON THIS FILE: voxi_restir.hlsli (giTraceInitialCandidate traces gScene through
// averRtProceedSolid, shades through the same material adapter); the backdrop/refraction glue and
// reflection wrappers further down voxi.hlsl call rtReflection/rtShadow; PSMainVoxi/PSRayDriven call
// rtShadowTemporal/rtSkyOcclusionTemporal directly.
// FORWARD DECLARATION: voxelUVW/insideVolume are declared, not defined, just above rtAmbientTraced --
// real definitions sit with the cone tracer over a thousand lines below where AVER_RT closes. Both
// halves are load-bearing as split; don't move the definitions up or delete the declaration.
// gGBufNormalHist below is guessed at t10, the slot gBlendBackdrop uses above the AVER_RT guard --
// unconfirmed against the C++ side, same as gRtMaterials' t9 guess. Moved verbatim, unchanged.

// DXR 1.1 inline ray tracing: traced from the pixel shader, no state objects or binding tables.
RaytracingAccelerationStructure gScene : register(t2);

// The flat geometry a ray reads after hitting something. Four descriptors for the whole scene, not
// per-mesh/material: this RHI uses explicit descriptor tables, not bindless -- indexing a bound
// StructuredBuffer at a runtime index is Tier 1 everywhere; indexing a descriptor HEAP is the only
// thing "bindless" means here, and none of these are that.
struct RtVertex   { float3 pos; float3 nrm; float2 uv; };
struct RtInstance { float4x4 objectToWorld; uint firstIndex; uint firstVertex; float3 albedo;
                    float metallic; float roughness; uint materialIndex; };
StructuredBuffer<RtVertex>   gRtVerts     : register(t3);
StructuredBuffer<uint>       gRtIndices   : register(t4);
StructuredBuffer<RtInstance> gRtInstances : register(t5);

// ---- per-material data for a ray hit, keyed by RtInstance::materialIndex ----
// materialIndex repurposes `pad`, a u32 nothing read (grep confirmed zero refs) -- zero extra bytes,
// RtInstance stays 96 bytes (VoxiRenderer.hpp static_assert unchanged).
// RtMaterial mirrors pbr::MaterialConstants (MaterialGpu.hpp) field-for-field, same order as
// `cbuffer AverMaterial` (PbrShaders.cpp) -- a mismatched order reads a neighbour's bytes with no
// compile error. Unread fields stay declared in order for the same reason: no partial
// StructuredBuffer element in HLSL.
#ifdef AVER_RT_BINDLESS
// Ray path's texture array, space1 to avoid colliding with space0's descriptor tables/SRVs/matrices.
// AVER_RT_TEX_CAPACITY must equal PipelineLayout::bindlessTextureCount exactly (more reads past the
// root signature's range); fixed-size because this backend serialises root signature 1.0.
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
    // Mirrors MaterialConstants::subsurfaceWeight/subsurfaceRadius, replacing the _pad0/_pad1 this used
    // to declare; same order/offsets (struct totals 176 bytes, MaterialGpu.hpp's static_assert).
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

    // Lamp brightness at 1m in the sun's units, mirrors MaterialConstants::lightIntensity (offset 160,
    // taking the struct to 176 bytes). >0 sets AVER_MAT_LIGHT, turning the draw into a sphere light
    // (gRdLocalLights, voxi.hlsl). Table uploads MaterialConstants bytes verbatim, so the pad must stay
    // declared for the stride.
    float  lightIntensity;
    float3 _lightPad;
};

#ifdef AVER_RT_BINDLESS
// One material map at a ray hit, or `fallback` where nothing bound. SampleLevel, never Sample: implicit
// derivatives are garbage at a fullscreen ray pass's silhouettes (neighbour pixels may hit unrelated
// triangles) -- mip 0 aliases in the distance, but predictably. NonUniformResourceIndex because
// neighbouring pixels genuinely hit different materials; without it the hardware may broadcast one
// lane's index across the wave.
// ---- AVER_RD_ABLATE: a measurement switch, not a feature ----
// Ray-driven primary costs ~6.7ms of 14.55ms on PTTest vs raster's 7.82ms (RT on in both), strongly
// pixel-bound; no RT quality dial moves it (--rt-rays 4/2/1: 14.49/14.46/14.44). GPU timing spans
// bracket draws, not terms, so terms are isolated by neutralising one at a time and diffing.
// EVERY NON-ZERO VALUE RENDERS A DELIBERATELY WRONG FRAME -- never wire to a quality tier; 0 is the
// only correct/default value. Terms should roughly sum to the raster gap (7.82ms); if they don't,
// the ablation isn't measuring what it claims.
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
// 7: restores ACCEPT_FIRST_HIT_AND_END_SEARCH on the sun-shadow ray (dropped so glass could attenuate
// rather than stop a shadow), pricing that decision. Knowingly breaks tinted shadows through glass;
// never wire it to a quality tier either.
#define AVER_RD_ABL_SHADOW_FIRSTHIT 7
// 8..11 close blind spots modes 1-6 leave in four larger terms; without them a 1-6 sweep looks
// complete and blames the residual on whatever mode was biggest.
#define AVER_RD_ABL_SKYOCC   8  // the sky-visibility ray (rtSkyOcclusion) -- ambient occlusion
// The SPECULAR cone, traced in the rough-surface branch. NOT covered by AVER_RD_ABL_GI, which only
// skips coneTracedIndirect -- so mode 2 has always left a 14th cone running and called it "GI off".
#define AVER_RD_ABL_SPECCONE 9
// skyColor in the ROUGH branch. Mode 4 ablates only the reflection branch's march; this is the other
// call site, and it is the one an ENCLOSED scene actually takes.
#define AVER_RD_ABL_ROUGHSKY 10
// averApplyFog: 4-step aerial march + possible 32-step atmosphere march, run unconditionally per
// pixel. Once 41% of a frame (elsewhere in the tree); MEASURED on Sponza now 0.22ms/16.83ms (1.3%) --
// history to keep it attributable, not a reason to assume it's still expensive.
#define AVER_RD_ABL_FOG      11
// The AERIAL HALF alone (the 4-step march), height fog left running since mode 11 removes both and
// can't separate them. Exists because a gate was proposed for this march (it's the only term in
// averApplyFog with no magnitude threshold, so it looks like the eager-lerp bug fixed twice before):
// measured 0.19ms/1.1% of the pass while changing 28.6% of the frame by up to 37 codes -- used, not
// discarded, so no gate was added. Kept so the number can be re-run rather than re-derived.
#define AVER_RD_ABL_AERIAL   12

// READ BEFORE SUBTRACTING TWO ABLATION NUMBERS: several modes REROUTE work rather than removing it,
// so deltas are not additive and a term can measure NEGATIVE.
//   - Mode 3 (REFL): specHit false makes `!specHit || skyW > 0.0` always true, forcing a full 32-step
//     skyColor(R) march a hit would've skipped, so its delta is (reflection cost) MINUS (added sky march).
//   - Mode 5 (TEX): also commits every alpha-masked cutout candidate, ending traversal EARLY.
//   - Mode 1 (SHADOW): replaces only the primary rtShadowTemporal call; the reflection's and bounce
//     loop's shadow rays keep running, so it measures the primary estimator only.

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

// ---- the material-graph adapter for this pass ----
// A generated graph emits `averSampleSlot(slot, uv)`, reading the eight BOUND texture registers through
// the material cbuffer's sampler; a ray hit has neither (its maps live in bindless gRtTextures, indexed
// via RtMaterial), so the generated body can't be used verbatim -- not a missing feature, why no
// material GRAPH ran on any ray path for the DEFAULT renderer. MaterialGraphHlsl.cpp emits a second
// copy with `averSampleSlot(` rewritten to `averRtSampleSlotGraph(`, differing by one token.
// MATERIAL TRAVELS IN A STATIC, NOT A PARAMETER: the generated body's module can't see RtMaterial
// (Aver.Render.PBR.Materials doesn't know this backend exists), so threading `mat` through would mean
// naming a type it must not know. A per-thread static costs nothing (HLSL statics are per-invocation,
// not shared) and keeps the emitter ignorant. Set immediately before calling the graph; nothing else
// reads them.
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
// NOT ddx(uv)/ddy(uv): implicit derivatives describe the screen coordinate, meaningless in a
// fullscreen ray pass where the neighbour lane may have hit an unrelated triangle. Uses
// rdRayDx/rdRayDy instead -- neighbouring pixels' primary rays, reconstructed via averViewRayDir
// (gInvViewProjRel) and scaled by this ray's hitT, already built here for the shadow disc -- a real
// world-space footprint that this turns into a UV-space one.
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
// Sampling every material at mesh UV read as noise on a mesh whose UVs don't match its projection
// (PTTest's floor/concrete, worlduv=1) -- missed initially since ElectricDreams terrain, where this
// was first measured, doesn't set the flag. Mirrors averSurfaceUV (material_prelude.hlsl),
// inst.objectToWorld standing in for gWorld.
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
// 10 for table 1 (material textures) to auto-rebase t9->t10 (see giLayout()) -- one more setSrvBuffer
// call, built like gRtInstances. C++ side (kGiSrvCount, giTableKinds, the per-frame buffer) owned by a
// concurrent agent on VoxiRenderer.hpp/.cpp -- if their actual slot differs from t9, move THIS line.
StructuredBuffer<RtMaterial> gRtMaterials : register(t9);

// ---- ALPHA-TESTED GEOMETRY, SEEN BY A RAY ----
// createBlas marks every geometry OPAQUE; only BLENDED materials un-opaqued their instance, so an
// alpha-MASKED one (foliage, chain-link, grates) traced as a solid sheet -- every leaf rendered as its
// bounding rectangle -- disagreeing with the raster depth prepass's correct
// `clip(s.alpha - a.alphaCutoff)` (material_prelude.hlsl). FIX COST: an alpha-masked instance is now
// FORCE_NON_OPAQUE (gives up the hardware's any-hit skip); each candidate pays an index fetch, three
// vertex reads, one texture sample. Opaque geometry is untouched.
// BY REFERENCE, against the ONE RayQuery template argument every trace in this file uses (HLSL can't
// be generic over the flags, why the loops below call this rather than inlining it six times). Guarded
// on AVER_RT_BINDLESS, which gates the material/texture tables this reads; without it no cutout can be
// expressed, so every candidate is as solid as its geometry.
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
// Replaces `RAY_FLAG_FORCE_OPAQUE` + a single `Proceed()`, correct only while non-opaque instances
// were all translucent (excluded by mask, once called "provably a no-op here") -- once an
// alpha-masked instance could appear in the opaque lane, that flag made the hardware commit the leaf
// card without asking.
// Still bounded in practice: only alpha-masked instances produce candidates here, so a scene with no
// cutout materials loops exactly as before.
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
// The same ray's hit distance, written and never read back by anything in this file. [0,1] as a
// fraction of TMax (gVoxelParams.z, giMaxDistance): 1 = escaped to sky, 0 = hit at the shading point.
// Deliberately NOT normalised to any denoiser's curve -- a consumer wanting world units multiplies by
// giMaxDistance. DISTANCE MATTERS BECAUSE OCCLUSION ALONE CAN'T: "30% occluded" can't distinguish a
// wide distant opening from a tight crevice, so a filter can't size its spread radius without it.
// NVIDIA NRD calls this IN_DIFF_HITDIST; REBLUR_DIFFUSE_OCCLUSION won't run without it (see
// modules/render.nrd/README.md).
// Guarded by gRtDenoiseParams.w (same flag/condition as u4/t11, VoxiRenderer::aoHistoryWanted).
// Low/Medium never trace this ray, so the slot is genuinely absent there and must not be touched --
// writing a null UAV is undefined, not merely wasted.
RWTexture2D<float>  gAoHitDistOut : register(u5);

// t14: the same signal, one frame later, after NVIDIA NRD has filtered it. Declared here rather than
// beside its t12/t13 register neighbours (~700 lines down with ReSTIR): HLSL has no forward
// declarations and rtSkyOcclusionTemporal, the only reader, comes before them -- and here is beside
// the u5 it filters, same quantity/encoding (see AverAmbientTraced::hitDist).
// ROUTINELY ABSENT -- readers must test for it. Needs NRD in the build, D3D12 (Vulkan refuses NRD's
// space-1 layout) and the G-buffer (off by default). VoxiRenderer clears this slot on any undenoised
// frame, so GetDimensions()==0 means "not denoised this frame", not "never" -- a frozen last-good
// image would be the worse failure.
Texture2D<float>    gNrdAo        : register(t14);

// u9/t15: the ReSTIR GI radiance on its way to NRD's REBLUR_DIFFUSE, and on its way back.
// rgb = giRestirIndirect's indirect diffuse radiance for this pixel; a = the candidate ray's distance,
// normalised by the SAME giMaxDistance the sky-occlusion hit distance uses (one hitDistParams
// describes both signals). NRD packs/unpacks the pair itself.
// OWN TEXTURE, NOT SHARED WITH OCCLUSION: different denoisers with separate histories (scalar
// REBLUR_DIFFUSE_OCCLUSION vs colour REBLUR_DIFFUSE) -- sharing a texture would share a history, and
// the two signals decorrelate.
// BOTH ABSENT UNLESS ReSTIR GI IS ON *AND* NRD IS RUNNING; readers test via zero dimensions like
// t14's. Writing is guarded on gGiRestirParams.x (says whether ReSTIR slots are bound this frame).
RWTexture2D<float4> gGiRadianceOut : register(u9);
Texture2D<float4>   gNrdGi         : register(t15);

// Ray-traced reflection history: same ping-pong as the shadow history above, its own pair of
// textures. rgb = shaded colour, a = linear hit depth, OR NEGATIVE meaning the ray missed.
Texture2D<float4>   gRtReflHist    : register(t7);
RWTexture2D<float4> gRtReflHistOut : register(u3);

// ---- Last frame's normal-roughness G-buffer, read-only, for the spatial denoisers' crease term ----
// NOT the same resource as SV_TARGET3 below (this frame's, bound as an RTV by the same draw -- no
// backend allows reading that as an SRV, and a pixel shader couldn't see unwritten neighbour output
// anyway). Same ping-pong fix as rtShadowHist_/rtReflHist_ applied to this channel.
// Gated on its OWN define, not AVER_GBUFFER (which only proves the non-ping-ponged target exists):
// reading an unbound register is a silent null-descriptor read / Vulkan validation failure, so this
// stays off until the ping-ponged pair (VoxiRenderer.hpp/.cpp, D3D12Device.cpp, VulkanDevice.cpp)
// exists -- AVER_GBUFFER_HISTORY=1 on top of AVER_GBUFFER=1, enforced host-side, not here.
// SLOT t10 IS A GUESS (same reasoning as gRtMaterials' t9). Assumed same resolution as
// gRtShadowHist -- tap loops reuse ITS GetDimensions() rather than querying this separately;
// unverified since allocation is C++-side.
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


// Radical inverse of `i` in base 2 (bits reflected about the binary point) in [0,1). Sample sequence
// is NESTED (unlike the sqrt((k+0.5)/n) it replaced, which put sample k's radius at a radius
// depending on total ray count n -- so n=2 and n=4 were unrelated estimators, no shared samples):
// phi(k) depends on k alone, so raising ray count fills in between existing samples rather than
// replacing them, letting a ray-count sweep read as convergence.
// EXACT on every adapter (unlike a hash): reversebits + IEEE round-to-nearest + 2^-32 being a power
// of two make the multiply exact, which the gate oracle's bit-exact 9-configuration comparisons need.
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

// ---- F1 (R0): A COSINE-WEIGHTED HEMISPHERE SAMPLE, NOT A FIXED 45-DEGREE RING ----
// BUG THIS REPLACES: rtDiscSample(0, ang0) always returns radius sqrt(0.5) (k=0 makes
// rtRadicalInverse2(k+1) exactly 0.5); lifted via cosTheta = sqrt(1-dot(xi,xi)), that's cosTheta ==
// 1/sqrt(2) for every pixel on every frame -- a deterministic 45-degree ring, not a cosine sample.
// The ReSTIR candidate (giTraceInitialCandidate, voxi_restir.hlsli) and the sky-occlusion ray both
// drew from it; an occluder boundary off 45 degrees reads too open/closed by a fixed step that no
// temporal accumulation can fix (same disease as rtAmbientTraced's per-pixel-only hash below: "a
// deterministic wrong answer is exactly what temporal accumulation cannot fix").
// NEW FUNCTION, NOT A FIX TO rtDiscSample: rtShadow and rtReflection deliberately reuse rtDiscSample
// for a sun disc / fixed specular ring, neither wanting a cosine-hemisphere distribution --
// retargeting rtDiscSample itself would silently break two features this task doesn't own. This is a
// sibling sampler using Malley's method: a uniform disc point (u in [0,1)) lifted via sqrt(u), giving
// the correct cosine law P(cosTheta < c) = c^2.
// idx = frameIdx*n+k keeps the sequence NESTED across ray count, same reasoning as
// rtRadicalInverse2. streamSalt keeps two callers at the same (pixel, frame, k) (ReSTIR at 0.0,
// sky-occlusion at 0.37) from drawing the same u, so successive frames sweep u across [0,1) instead
// of repeating one radius.
// Deterministic in (pixel, frame index) only, no true per-frame RNG -- same rule and reasoning as
// rtShadowTemporalEx's own FRAME INDEX IS SAFE note on its frameIdx use: a --frames N run always ends
// on the same frameIdx, so results reproduce.
float2 rtHemiDiscSample(uint k, uint n, uint frameIdx, float2 pixelKey, float streamSalt) {
    const uint  idx = frameIdx * max(n, 1u) + k;
    const float u   = frac(rtRadicalInverse2(idx + 1u) + rtHash(pixelKey + float2(streamSalt, 17.0 + streamSalt)));
    const float a   = rtHash(pixelKey) * 6.2831853 + (float)idx * 2.39996323 + streamSalt;
    return float2(cos(a), sin(a)) * sqrt(u);
}

// Traces occlusion rays toward the sun's DISC: 0 fully shadowed, 1 fully lit, between is a real
// penumbra (the single ray this replaced gave a hard 0/1 edge). gRtParams.x carries the sun's angular
// tangent (~half a degree), so softening is the sun's own property, not a tuned constant.
// Bias scales with camera distance (0.02cm is ~300 ulp at 1000cm, <3 at 100000cm -- else distant
// geometry self-intersects and flickers).
// `dpx`/`dpy`: receiver's screen-space footprint, PASSED IN rather than ddx/ddy(wpos) -- only valid
// for a primary surface, wrong for a reflected hit. A reflected caller passes zero.
// `rays`: explicit, not from the cbuffer, so a secondary ray can ask for fewer than a primary one.
// `frameJitter`: added to the rotation, NOT folded into rtHash (pure-per-pixel for the gate oracle).
// Only rtShadowTemporal passes nonzero, only with tiling on.
// `kFirst` (rtShadowEx only): starts at sample kFirst instead of 0, for CSRdShadowProbe (picks one of
// this pixel's real samples). Other callers use rtShadow, which passes 0.
float3 rtShadowEx(float3 wpos, float3 N, float3 L, float2 pixel, float3 dpx, float3 dpy, uint rays,
                  float frameJitter, uint kFirst) {
    const uint  n    = max(rays, 1u);
    const float tanR = max(gRtParams.x, 0.0);
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);

    // A frame around the light direction, to spread samples across the disc.
    float3 up = abs(L.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T  = normalize(cross(up, L));
    float3 B  = cross(L, T);

    // THE PIXEL'S OWN FOOTPRINT (screen-space derivatives of world position) is what fixes the
    // jagged edge, not the sun's disc: at contact distances the true penumbra is far narrower than a
    // pixel, so the disc alone gives the same binary answer except at one edge pixel, and the
    // once-per-pixel shadow term stair-steps against geometry resolved at 8x MSAA. Jittering the ray
    // ORIGIN across the footprint turns the per-pixel test into an area estimate.
    const float ang0 = rtHash(pixel) * 6.2831853 + frameJitter;
    float3 vis = float3(0.0, 0.0, 0.0);

    [loop] for (uint k = 0; k < n; ++k) {
        // Sample k, independent of n: it sits in the same place whatever the ray count, so raising
        // the count refines the estimate rather than replacing it. The same rotated pattern serves
        // both the sun disc and the pixel footprint.
        float2 disc = rtDiscSample(k + kFirst, ang0);

        float3 dir = normalize(L + (T * disc.x + B * disc.y) * tanR);
        // Half the footprint, so samples stay inside the pixel they are estimating.
        float3 org = wpos + (dpx * disc.x + dpy * disc.y) * 0.5;

        RayDesc r;
        // Offset along the NORMAL and along the ray -- the normal alone leaves acne at grazing angles.
        r.Origin    = org + N * bias + dir * bias;
        r.Direction = dir;
        r.TMin      = bias;
        r.TMax      = 100000.0;
        // NO ACCEPT_FIRST_HIT: right for a binary shadow, wrong for a transmissive one (glass would
        // stop the ray like a wall). Instead runs its own traversal, multiplying transmittance per
        // translucent surface crossed. COST IS REAL: every shadow ray now walks to an opaque hit or
        // the structure's end, even rays that never meet a pane -- measure before assuming it's small.
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
        // travelled inside it; otherwise the per-crossing surface rule applies (a thin sheet). Gathers
        // spans rather than multiplying as it goes, since a path length needs both ends.
        // PAIRED BY MIN/MAX t, never arrival order or facing: DXR doesn't guarantee non-opaque
        // candidates arrive nearest-first, and winding (CandidateTriangleFrontFace()) is worse -- it's
        // what broke volume absorption on the pool (fluid box winds opposite the cube).
        //   one hit  -> ray started inside the medium, t is the distance out (pool floor under water)
        //   two hits -> entered and exited; the span between is the thickness (a pane)
        //   more     -> concave/overlapping geometry; the outer span is the honest estimate
        // TWO SLOTS AS SCALARS, NOT AN ARRAY: loop-indexed local arrays spilled out of registers on
        // the wave-bound sun shadow, MEASURED 7.96ms -> 9.95ms (25% regression). Two is what the
        // scene needs (glass over water); a third medium falls through to the surface rule.
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

            // ---- A CUTOUT IS NOT A MEDIUM ----
            // Alpha-masked instances are non-opaque now, so they arrive here alongside glass, but
            // must NOT enter the transmittance walk: a leaf has no thickness/attenuation colour.
            // Binary rule: above cutoff, commit (fully blocked); below it, the ray passes through
            // the hole -- continue without touching transmittance.
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

float3 rtShadow(float3 wpos, float3 N, float3 L, float2 pixel, float3 dpx, float3 dpy, uint rays,
               float frameJitter) {
    return rtShadowEx(wpos, N, L, pixel, dpx, dpy, rays, frameJitter, 0u);
}

// Decodes gGiShadowParams.w's runtime bit-field (voxi.hlsl has the per-bit meaning). One accessor so
// T1 below, T2 (rtSkyOcclusionTemporal) and T3 (rtReflectionTemporalEx, voxi.hlsl) can't drift.
// THE SINGLE-PASS PSRayDriven COMPILE (AVER_RD_SINGLE_PASS) SEES CONSTANTS INSTEAD: T1 on, T2/T3/T4
// off. Compiling every runtime bit's both paths made that megakernel (shadow+GI+reflection+sky in one
// PS) exceed the AMD driver's limit: MEASURED 2026-09-26 RX 7800 XT, device lost on frame 1 at any
// resolution; removing any one stage, or folding bits to constants with all stages kept, fixed it.
// Staged passes keep all four toggles live. Re-run (--rd-stages 0 --dred) after touching this path.
#ifndef AVER_RD_SINGLE_PASS
#define AVER_RD_SINGLE_PASS 0
#endif
#if AVER_RD_SINGLE_PASS
uint rtGiShadowBits() { return 1u; }
#else
uint rtGiShadowBits() { return (uint)gGiShadowParams.w; }
#endif

// Where a shadow ray along `dir` leaves the surface at wpos: rtShadowEx's distance-scaled bias, offset
// along the normal AND the ray (see rtShadowEx for why both). Shared with rdLocalShadow so a lamp's
// shadow ray leaves the surface exactly as the sun's does. rtShadowEx keeps its own inline copy
// instead of calling this, to avoid touching its measured hot loop.
void rtShadowRayStart(float3 wpos, float3 N, float3 dir, out float3 origin, out float bias) {
    bias   = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
    origin = wpos + N * bias + dir * bias;
}

// The ray a single rtShadowEx sample would build at rays=1, kFirst=0, zero footprint -- what every
// caller of rtShadowOpaque below passes. Factored out so rtShadowOpaque can't drift from rtShadowEx's
// formula. rtShadowEx does not call this itself, to avoid touching its measured hot loop for a
// caller that only wants n=1.
void rtShadowRay0(float3 wpos, float3 N, float3 L, float2 pixel, float frameJitter,
                  out float3 dir, out float3 origin, out float bias) {
    float3 up = abs(L.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T  = normalize(cross(up, L));
    float3 B  = cross(L, T);
    const float tanR  = max(gRtParams.x, 0.0);
    const float ang0  = rtHash(pixel) * 6.2831853 + frameJitter;
    const float2 disc = rtDiscSample(0, ang0);
    dir    = normalize(L + (T * disc.x + B * disc.y) * tanR);
    rtShadowRayStart(wpos, N, dir, origin, bias);
}

// T1 (Settings::rtSecondaryShadowOpaque): the cheap sun-shadow ray for a SECONDARY hit --
// rtReflection's hit and the ReSTIR GI candidate's hit (giTraceInitialCandidate, voxi_restir.hlsli) --
// where rtShadow's transmittance walk buys detail neither caller can resolve. ONE ray,
// ACCEPT_FIRST_HIT_AND_END_SEARCH, against AVER_RT_MASK_OPAQUE_ALL (not the bare _OPAQUE CSRdVisibility's
// primary ray uses -- that's the owner-hidden exclusion, a property of the primary ray only), resolved
// through averRtProceedSolid so a leaf still casts its alpha-tested SHAPE's shadow, not its bounding rectangle.
// THE TRADE: a translucent instance (glass, water) is excluded by mask, not walked/attenuated, so it
// casts NO shadow for these two callers -- losing the coloured tint a pane would cast. The PRIMARY sun
// shadow (rtShadowTemporalEx / CSRdShadow / CSRdShadowProbe) keeps the tint. Saving: up to 8 transmittance
// steps (RAY_FLAG_NONE, AVER_RT_MASK_ALL) traded for one BVH traversal to the first opaque hit.
// Builds the identical ray rtShadowEx(rays=1, kFirst=0, dpx=dpy=0) would, via rtShadowRay0, so
// flipping this bit changes cost, not the ray itself. Returns float3 (0 or 1 per channel) to drop
// into either call site unchanged.
float3 rtShadowOpaque(float3 wpos, float3 N, float3 L, float2 pixel, float frameJitter) {
    float3 dir, origin;
    float  bias;
    rtShadowRay0(wpos, N, L, pixel, frameJitter, dir, origin, bias);

    RayDesc r;
    r.Origin    = origin;
    r.Direction = dir;
    r.TMin      = bias;
    r.TMax      = 100000.0;

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, AVER_RT_MASK_OPAQUE_ALL, r);
    averRtProceedSolid(q);

    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? float3(0, 0, 0) : float3(1, 1, 1);
}

// ---- LOCAL LIGHTS: lamps lit the way the sun is ----
// A material with lightIntensity > 0 (AVER_MAT_LIGHT) turns its draw into a small SPHERE light (world
// bounding sphere, coloured by emissiveFactor). VoxiRenderer gathers up to 32/frame into
// gRdLocalLights (t18, voxi.hlsl). Each lit pixel gets ONE stochastic shadow ray toward one of them,
// accumulated via the sun history's own reprojection (rdLocalLightsVisibility, voxi.hlsl), and every
// light in range shaded through the sun's BRDF times that visibility (rdLocalLightsShade, voxi.hlsl) --
// in the staged passes (CSRdLocalLights + Stage B), single-pass PSRayDriven, and PSMainVoxi alike.
// Declared here (not beside gRdLocalLights) because voxi_restir.hlsli, #included right after this
// file and before voxi.hlsl's staged declarations, needs rdLocalCarriesEmitters() too (an emitter's
// own emission leaves ReSTIR GI's candidate hits while these lights carry it).
// posRadius     = world centre (cm), sphere radius (cm, >= 1).
// radianceRange = rgb: colour * (the sphere's own 1-metre irradiance, from its emissive peak and
//                 posRadius.w, times lightIntensity -- VoxiRenderer::buildLocalLights derives this,
//                 not the shader), in the sun's units (averSunRadiance()); w: range in cm, past which
//                 the light contributes nothing.
// MIRRORS the C++ RdLocalLight (32 bytes) field for field -- a StructuredBuffer stride mismatch reads
// the neighbour's bytes with no compile error.
struct RdLocalLight { float4 posRadius; float4 radianceRange; };

// AVER_RD_SINGLE_PASS_LAMPS (default 1) keeps lamps in the single-pass megakernel, already at the AMD
// driver's register limit (rtGiShadowBits() above). ";AVER_RD_SINGLE_PASS_LAMPS=0" strips them: every
// lamp function/call sits under AVER_RD_LAMPS or !AVER_RD_SINGLE_PASS (only the struct/resource
// declarations do not, and an unread declaration drops for free), so that compile holds no lamp code.
// Every other compile always carries them.
#ifndef AVER_RD_SINGLE_PASS_LAMPS
#define AVER_RD_SINGLE_PASS_LAMPS 1
#endif
#define AVER_RD_LAMPS (!AVER_RD_SINGLE_PASS || AVER_RD_SINGLE_PASS_LAMPS)

#if AVER_RD_LAMPS
// gCameraMedium.z/.w (voxi.hlsl has the field comment). Count 0 means lamps off/unavailable; every
// reader treats it as "no lamps", never "read the buffer".
uint rdLocalLightCount() { return (uint)(gCameraMedium.z + 0.5); }
// gRdLocalHist (t19) holds a usable previous frame for the SAME light set (bit 1). False restarts
// accumulation -- a visibility accumulated against a different light set answers another question.
bool rdLocalHistValid()  { return ((uint)(gCameraMedium.w + 0.5) & 1u) != 0u; }
// Every lamp-flagged draw is a live light this frame (bit 2), so a GI estimator hitting one may skip
// its emission (direct term already carries it). False when a flagged lamp missed the 32-cap list --
// it keeps its GI glow instead of going dark.
bool rdLocalCarriesEmitters() { return ((uint)(gCameraMedium.w + 0.5) & 2u) != 0u; }

// One sphere light's DIFFUSE IRRADIANCE at wpos, before visibility: inverse square from the centre,
// normalised so d=100cm gives radianceRange.rgb exactly (already lightIntensity-scaled, C++ side),
// clamped at the sphere's radius so a receiver touching the bulb doesn't blow up, faded to zero at
// range by (1-(d/range)^4)^2 (a hard cut would ring).
// rdLocalLightsVisibility (voxi.hlsl) picks its shadowed light by this weight; rdLocalLightAt shades
// with the SAME falloff minus N.L (the BRDF applies that itself). Change one, change both.
float3 rdLocalIrradiance(RdLocalLight l, float3 wpos, float3 N) {
    const float3 toC   = l.posRadius.xyz - wpos;
    const float  d2    = dot(toC, toC);
    const float  range = l.radianceRange.w;
    // Also the zero-range guard: a range of 0 always takes this return, so the divide below never sees it.
    if (d2 >= range * range) return float3(0.0, 0.0, 0.0);
    const float r   = l.posRadius.w;
    const float x2  = d2 / (range * range);             // (d/range)^2
    const float win = saturate(1.0 - x2 * x2);          // 1 - (d/range)^4
    const float ndl = saturate(dot(N, toC) * rsqrt(max(d2, 1e-8)));
    return l.radianceRange.rgb * (1e4 / max(d2, r * r)) * (win * win) * ndl;
}

// ONE opaque shadow ray from wpos toward a point on the light's sphere: 1 unoccluded, 0 blocked. The
// sun's own recipe (T/B frame, per-pixel rotated disc sample via rtHash + frameJitter, rtDiscSample(0, .),
// rtShadowRayStart) with the sphere's radius standing in for the sun's tangent; same first-hit
// opaque-including-cutouts lane as rtShadowOpaque, so glass casts no lamp shadow either.
// TMax stops short of the sphere (distance to centre - 1.25 radius) so the bulb's own surface never
// shadows the light it is; a receiver that close has no room for an occluder and returns 1 without
// tracing (dist > 1.25r >= 1.25cm, never the zero vector).
float rdLocalShadow(float3 wpos, float3 N, RdLocalLight l, float2 pixel, float frameJitter) {
    const float3 toC  = l.posRadius.xyz - wpos;
    const float  dist = length(toC);
    const float  tMax = dist - l.posRadius.w * 1.25;
    if (tMax <= 0.0) return 1.0;

    const float3 Lc = toC / dist;
    float3 up = abs(Lc.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T  = normalize(cross(up, Lc));
    float3 B  = cross(Lc, T);
    const float  ang0   = rtHash(pixel) * 6.2831853 + frameJitter;
    const float2 disc   = rtDiscSample(0, ang0);
    const float3 target = l.posRadius.xyz + (T * disc.x + B * disc.y) * l.posRadius.w;
    const float3 dir    = normalize(target - wpos);

    float3 origin;
    float  bias;
    rtShadowRayStart(wpos, N, dir, origin, bias);

    RayDesc r;
    r.Origin    = origin;
    r.Direction = dir;
    r.TMin      = bias;
    r.TMax      = max(tMax, bias);

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, AVER_RT_MASK_OPAQUE_ALL, r);
    averRtProceedSolid(q);

    return (q.CommittedStatus() == COMMITTED_TRIANGLE_HIT) ? 0.0 : 1.0;
}
#endif

// The fraction of the hemisphere above `N` from which the SKY is actually reachable: 1 fully open, 0
// fully enclosed. The scalar `diffAmbient` multiplies the sky irradiance by, traced not estimated.
//
// WHY: coneTracedIndirect's `ao` (six 60-degree cones marching the voxel volume) averages a thin
// wall with empty space beside it once the cone widens into a coarse mip, and passes through. Roughly
// right in an OPEN scene (so Epic-only there); optimistic in an enclosed one -- MEASURED on Sponza
// against a converged path-traced reference: shadowed pixels read [25,26,30] vs reference [7,7,7],
// blue-biased because what leaks in is sky.
// COSINE-WEIGHTED BY CONSTRUCTION via Malley's method: a uniform disc point lifted onto the
// hemisphere IS a cosine-weighted direction, so averaging a binary visibility test over them already
// gives the cosine-weighted integral. rtDiscSample supplies the point, sharing the shadow ray's
// NESTED sequence.
// NO FRAME TERM: every RT sampler here is a pure function of pixel position so the gate oracle's
// exact-pixel comparison stays reproducible (a frame-varying set would denoise better but break 181
// gates). Noise here is FIXED per pixel -- stable, structured, doesn't average away over time. To
// smooth it, add rays or a spatial filter, not a frame counter.
// A MISS IS SKY: a plain occlusion query, ACCEPT_FIRST_HIT_AND_END_SEARCH, against the opaque lane --
// unlike the sun ray above, it only asks whether anything is in the way.
// FORWARD-DECLARED: the voxel helpers are defined with the cone tracer ~900 lines below, and HLSL
// needs the declaration before the call; moving the definitions up would drag the whole clipmap block
// above the RT section.
float3 voxelUVW(float3 wp);
bool   insideVolume(float3 uvw);

// F2's RECONSTRUCTED path (voxi_restir.hlsli) needs one voxel-cone march before voxi_cone.hlsli's
// real definition is reachable (#include'd only after voxi_restir.hlsli) -- same forward-declare
// split as the two lines above, for the same reason. Defined at voxi_cone.hlsli; signature copied
// verbatim, so a changed definition that forgets to update this fails loudly (compile error, not
// silent drift).
float4 traceCone(float3 originWS, float3 dir, float aperture);

// What one hemisphere gather learned. (A closest-hit variant also returning visible sky + bounce at
// the hit, AVER_AO_UNIFIED, measured 2026-09-27 at +1.3ms/frame and was removed.)
struct AverAmbientTraced {
    float  open;    // fraction of samples that reached the sky
    // Mean distance travelled as a fraction of TMax over ALL n samples (an escaped sample contributes
    // 1.0 -- averaging only hits would invert the truth in open sky). Always computed (one MAD/sample),
    // for the external denoiser's hit-distance input.
    float  hitDist;
};

AverAmbientTraced rtAmbientTraced(float3 wpos, float3 N, float2 pixel, uint rays) {
    const uint n = clamp(rays, 1u, 32u);
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
    // ROTATION SHARED ACROSS A TILE, NOT PER PIXEL: the only lever measurements support here. Cost is
    // not arithmetic (this already runs 32-64 lanes in lockstep under SIMT) -- 3 more SUN rays cost
    // 0.05ms (removing the shadow ray entirely would save 11.95ms) since they walk the same BVH nodes,
    // ray 1 pays traversal, the rest ride cache; while this cosine-distributed ray costs 5.37ms for ONE,
    // because every lane descends a different part of the tree and the wave runs at its unluckiest
    // lane's speed. Sharing azimuth across an NxN tile makes neighbours trace near-parallel rays
    // touching the same cache lines, trading independent noise for correlated noise within a tile --
    // fine for a low-frequency term like AO, wrong for sharp detail.
    // Hardware-agnostic (plain HLSL, no wave intrinsics/vendor extension) rather than Shader Execution
    // Reordering, which is vendor-specific or needs a tier this engine doesn't require.
    // Tile size 1 reproduces the old per-pixel rotation exactly (floor(p/1)==p), so this is a runtime
    // dial (gAmbientParams.y, compile-time define as floor) with a true no-op setting, not a rewrite --
    // was a #define alone, set nowhere, unmeasurable like the ray-count gap beside it.
    const float aoTileEdge = max(gAmbientParams.y, AVER_AO_COHERENCE_TILE);
    const float2 aoTile = floor(pixel / aoTileEdge);
    // A DIFFERENT DIRECTION EVERY FRAME, or the history pair below buys nothing. rtHash(aoTile) alone
    // is a pure function of pixel, so every pixel traced the same hemisphere direction forever --
    // deterministic, and a deterministic wrong answer can't be fixed by temporal accumulation
    // (averaging ten identical samples returns the sample). That produced fixed per-pixel salt-and-
    // pepper on shadowed surfaces, and explains why raising the accumulation weight 0.9->0.95 measured
    // no change: nothing different to average. THE GOLDEN ANGLE (2.39996323, same as the tiled shadow
    // path) spreads successive frames far apart on the disc, so ~10 frames give ~10 spread samples
    // instead of one counted ten times.
    const float ang0 = rtHash(aoTile) * 6.2831853 + gRtHistParams.z * 2.39996323;
    float3 T, B;
    // Matches ptBasis/averBasis convention: any orthonormal pair about N will do, since the disc
    // sample is rotated by ang0 anyway.
    const float3 up = abs(N.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
    T = normalize(cross(up, N));
    B = cross(N, T);

    AverAmbientTraced res;
    res.open = 0.0; res.hitDist = 0.0;
    // Hoisted out of the loop because the accumulation below divides by it, and because a TMax of
    // zero would otherwise be a divide by zero on a scene whose giMaxDistance was authored to
    // nothing -- max(...,1.0) is the same floor the RayDesc uses a few lines down.
    const float aoTMax = max(gVoxelParams.z, 1.0);
    [loop] for (uint k = 0; k < n; ++k) {
        // ---- F1 (R0), gAmbientParams.z bit 1: legacy 45-degree ring vs. the cosine hemisphere ----
        // Bit set keeps rtDiscSample's fixed ring for comparison; default draws rtHemiDiscSample (see
        // its own comment for why the ring was wrong). aoTile, not pixel, since azimuth is already
        // tile-shared (see ang0); streamSalt 0.37 separates this stream's u from ReSTIR's own 0.0 at
        // the same (pixel, frame, k).
        const float2 d = (((uint)gAmbientParams.z & 1u) != 0u) ? rtDiscSample(k, ang0) : rtHemiDiscSample(k, n, (uint)gRtHistParams.z, aoTile, 0.37);
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
        // BOUNDED, NOT 1km -- MEASURED TO CHANGE NOTHING (written down because the obvious assumption
        // is that it should). The sun ray runs to 100000 because a missed shadow caster is a visibly
        // wrong hard edge; ambient is smooth, so bounding it LOOKS like an optimisation but on Sponza
        // is worth nothing (65.716ms bounded vs 65.715ms unbounded, byte-identical viewport mean) --
        // every ray in an enclosed scene hits something well under 40m. THE COST IS INCOHERENCE, NOT
        // LENGTH: cosine-distributed directions send neighbouring lanes into unrelated BVH parts
        // (unlike parallel sun rays), so the lever is ray count/amortisation, not distance. Kept
        // unbounded anyway: gVoxelParams.z (giMaxDistance) is the authored "how far light travels
        // here" and where traceCone stops too, so both estimates measure the same extent of world.
        r.TMax      = max(gVoxelParams.z, 1.0);

        // THE TEMPLATE ARGUMENT LOST ACCEPT_FIRST_HIT_AND_END_SEARCH; the flag moved to the
        // TraceRayInline call:
        //
        //   averRtProceedSolid needs one RayQuery template argument, so this was the one ray still
        //   calling a bare Proceed() with no candidate test. The resulting bug is the OPPOSITE of the
        //   obvious guess: alpha-masked instances stay in the OPAQUE lane but carry FORCE_NON_OPAQUE
        //   (VoxiRenderer.cpp: "NOT THE MASK, only the flags"), so an uncommitted RayQuery meeting one
        //   stops traversal AT the pane. CommittedStatus then reads
        //   COMMITTED_NOTHING and the sample was counted as HAVING REACHED THE SKY.
        //
        //   So a leaf made ambient light LEAK, not over-occlude: any direction surfacing a cutout
        //   candidate first scored fully open regardless of the solid wall behind it, brightening
        //   exactly the enclosed, foliage-heavy interiors this ray was added to darken. The shared loop
        //   now resolves each candidate against its alpha and keeps traversing, like every other ray here.
        //
        // Runtime flags OR with template flags: the same query as before, just spelled at the call.
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
        q.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, AVER_RT_MASK_OPAQUE_ALL, r);
        averRtProceedSolid(q);

        // ONE `if`, BOTH ANSWERS: CommittedRayT() is meaningful only on a hit; on a miss the ray ran
        // its whole length, which IS the distance, hence the miss branch adds aoTMax.
        //
        // FIRST HIT, NOT NEAREST: ACCEPT_FIRST_HIT_AND_END_SEARCH stops at whatever triangle is
        // reached first within TMax, not necessarily the closest -- an upper-bounded estimate, usually
        // exact in the enclosed geometry this term targets. Exact would mean dropping the flag and
        // paying full traversal on the frame's most incoherent ray (5.37ms for one, measured above) to
        // sharpen a filter radius; not worth it.
        if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
            res.open    += 1.0;
            res.hitDist += aoTMax;
        } else {
            res.hitDist += min(q.CommittedRayT(), aoTMax);
        }
    }

    // DIVIDED BY n, NOT BY A HIT COUNT: these are Monte Carlo estimates of hemisphere integrals.
    const float inv = 1.0 / (float)n;
    res.open    *= inv;
    // Divided by TMax too, making it the [0,1] fraction promised rather than a world distance.
    // saturate() since CommittedRayT can land a hair past TMax on a near-limit hit.
    res.hitDist = saturate(res.hitDist * inv / aoTMax);
    return res;
}

// The open fraction alone, for callers that do not need the hit distance.
float rtSkyOcclusion(float3 wpos, float3 N, float2 pixel, uint rays) {
    return rtAmbientTraced(wpos, N, pixel, rays).open;
}

// Reprojects wpos through LAST frame's camera to sample the ray-traced shadow history. False when
// unusable: off-screen, behind last frame's near plane, or a DISOCCLUSION (stored depth disagrees
// with the reprojected texel) -- `hist`/`velocityPx` untouched on false. `velocityPx` is the
// reprojection's screen-space displacement from `pixel`, letting the caller discount a moved sample
// (depth alone can't tell "same surface, slid since last frame").
// NDC -> LAST frame's VIEWPORT rect (gSceneViewport), not [0,1] of the whole texture: the editor
// docks the 3D view in a sub-rect, and plain ndc*0.5+0.5 lands on the wrong texel otherwise. Caught
// by the shadow-rt/penumbra-rt gates, not shipped.
// NEAREST, not bilinear -- REVERSED from an earlier version that was read every frame in sync (a
// bilinear tap blended in something nearly identical there): this tiled path reads out of sync between
// neighbours, and bilinear mixed a stale neighbour across a penumbra -- MEASURED to converge to a
// stable but WRONG value (a 4x4 tile settled at 46,46,47 vs a true ~23,27,32, unmoved 300-1500 frames).
// Nearest guarantees this pixel's own last write.
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
    // FLOOR, not round: `px` for a static pixel lands on index+0.5, a tie round() breaks
    // inconsistently (round-half-to-even) by index parity, sending ~half of pixels to the wrong
    // neighbour. floor() matches
    // how the WRITE side indexes (gRtShadowHistOut[uint2(pixel)] truncates SV_Position) -- MEASURED:
    // round() converged to a stable but wrong value (penumbra-rt settled at 78,75,70 vs true ~23,27,32).
    int2 texel = int2(floor(px));
    if (any(texel < 0) || texel.x >= (int)texW || texel.y >= (int)texH) return false;

    const float2 stored = gRtShadowHist.Load(int3(texel, 0));   // x = visibility, y = linear depth
    // clip.w is the expected depth (VSMain's o.pos.w for wpos, through LAST frame's camera); comparing
    // to stored depth catches a disocclusion a screen-position check alone can't (a silhouette edge
    // can reproject onto an already-populated texel at a different depth).
    //
    // TOLERANCE FOLLOWS THE DEPTH GRADIENT: a flat 3% made grazing surfaces speckle. `stored.y` is up
    // to half a texel from where this pixel reprojected; face-on that's millimetres and 3% covers it,
    // but edge-on (a floor stretching away) half a texel of screen space is metres of depth, so the
    // test rejected good history and fell back to the raw ONE-RAY (BINARY) estimate -- salt-and-pepper
    // exactly where geometry grazes, which is where the user reported it. (|ddz/dx|+|ddz/dy|)*2 adds
    // the depth change this surface's own slope makes unavoidable across a pixel; face-on geometry
    // (gradient ~0) keeps the old tolerance.
    //
    // ddx/ddy ARE SAFE HERE (unlike rtShadowSpatial's warning about derivatives in divergent flow
    // differing against a lane that never ran): the branch above this call is on gAmbientParams.x, a
    // uniform cbuffer value, and the gradient is taken before any early-out.
    const float tol = max(clip.w, stored.y) * 0.03 + 1.0 + (abs(dzdx) + abs(dzdy)) * 2.0;   // 3% relative, +1cm floor at grazing distances
    if (abs(clip.w - stored.y) > tol) return false;

    hist = stored.x;
    velocityPx = px - pixel;
    return true;
}

// THE LOCAL-LIGHT TWIN of rtReprojectHistory, for rdLocalLightsVisibility (voxi.hlsl): same
// reprojection and depth test against the SUN history's depth (validity is a property of the
// surface, not the light, so gRdLocalHist needs no depth channel), returning the texel instead of
// visibility. Near-copy on purpose (a shared helper would add phis to the register-bound CSRdShadow);
// arithmetic IDENTICAL to rtReprojectHistory -- see there for why. Change one, change both.
bool rtReprojectTexel(float3 wpos, float2 pixel, out int2 texel, out float2 velocityPx) {
    texel = int2(0, 0);
    velocityPx = 0.0;
    float4 clip = mul(float4(wpos, 1.0), gPrevViewProj);
    const float dzdx = ddx(clip.w);   // before every early-out, as in rtReprojectHistory
    const float dzdy = ddy(clip.w);
    if (clip.w <= 1e-4) return false;
    float3 ndc = clip.xyz / clip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0) return false;
    float texW, texH;
    gRtShadowHist.GetDimensions(texW, texH);
    float2 px = gSceneViewport.xy +
                float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5) * gSceneViewport.zw;
    const int2 t = int2(floor(px));
    if (any(t < 0) || t.x >= (int)texW || t.y >= (int)texH) return false;
    const float storedDepth = gRtShadowHist.Load(int3(t, 0)).y;
    const float tol = max(clip.w, storedDepth) * 0.03 + 1.0 + (abs(dzdx) + abs(dzdy)) * 2.0;
    if (abs(clip.w - storedDepth) > tol) return false;
    texel = t;
    velocityPx = px - pixel;
    return true;
}

// The AMBIENT twin of rtReprojectHistory, against gAoHist. Near-copy on purpose: HLSL below SM 6.6
// can't take a Texture2D parameter, and folding both behind a flag would branch in every pixel's hot
// path to save nine lines. Arithmetic IDENTICAL (floor over round, the 3% depth tolerance) -- see
// rtReprojectHistory for why. Change one, change both.
bool rtReprojectAo(float3 wpos, float2 pixel, out float hist, out float2 velocityPx) {
    hist = 0.0;
    velocityPx = 0.0;
    float4 clip = mul(float4(wpos, 1.0), gPrevViewProj);
    const float dzdx = ddx(clip.w);   // before every early-out, so never evaluated in divergent flow
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
    const float tol = max(clip.w, stored.y) * 0.03 + 1.0 + (abs(dzdx) + abs(dzdy)) * 2.0;
    if (abs(clip.w - stored.y) > tol) return false;
    hist = stored.x;
    velocityPx = px - pixel;
    return true;
}

// AMBIENT OCCLUSION, ACCUMULATED OVER TIME instead of over rays.
//
// At one ray/pixel this estimator is `open = hit ? 0 : 1`, a binary mask whose MEAN is already
// correct (a probe read the same value at 1 ray and at 4 -- a probe can't measure variance, which is
// why one never caught this). The old fix -- four
// rays sharing one azimuth across a 4x4 tile -- bought five quantisation levels over two but
// correlated them into visible BLOCKS. Accumulating instead gets sample count from FRAMES: at weight
// 0.9 the history averages ~10 of them, so one ray behaves like ten and the tile can shrink to 1 --
// strictly cheaper, which is what lets sky occlusion drop off the Epic-only rung.
//
// VELOCITY TERM AND 32-PIXEL BUDGET ARE THE SHADOW PATH'S, DELIBERATELY: this history is also exactly
// one frame old with its own depth test, so velocity only pays for sub-texel alignment (bounded at
// half a texel at any speed by the nearest-neighbour lookup). AO tolerates a stale sample better than
// a shadow edge does, not worse.
//
// THE SPATIAL HALF THE AMBIENT TERM NEVER HAD: the sun shadow is denoised twice (rtShadowTemporal
// accumulates, rtShadowSpatial filters on read); sky occlusion got only the first, so its residual
// variance is ~6/7 of this renderer's measured frame flicker (still camera, one frame apart: 0.158%
// of channels past 8 codes with it on, 0.026% off, 0.000% with RT off entirely).
//
// NEAR-COPY OF rtShadowSpatial (same reason as rtReprojectAo: no Texture2D parameters below SM 6.6,
// and a flag would branch a hot path). IDENTICAL arithmetic (plane-distance rejection in last frame's
// depth, Gaussian sigma=radius/2, normal crease test, velocity taper) except radius is at least 2
// (5x5): one ray/pixel at half rate has more variance than the sun's penumbra -- at Epic's shadow
// radius 1 the AO view read high-pass RMS 12.45, at 2 it's 6.44 for +0.09ms, no measurable image
// change still or moving (textures mask it; untextured surfaces show it). 0 (Undenoised) still turns it off.
//
// Reads gAoHist -- LAST frame's openness -- exactly as the shadow filter does, making the reprojected
// centre the right place to gather around.
float rtAoSpatial(float centre, float3 wpos, float3 N, float2 pixel, float curDepth) {
    const int shadowRadius = (int)gRtDenoiseParams.x;
    if (shadowRadius <= 0 || gRtHistParams.y < 0.25 || gRtDenoiseParams.w < 0.5) return centre;
    const int radius = max(shadowRadius, 2);

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

    // ---- THE CLAMP, WHICH IS WHAT ACTUALLY REMOVES SALT AND PEPPER ----
    //
    // The Gaussian above is the wrong tool for an isolated extreme: a LINEAR filter spreads an
    // impulse over its kernel instead of removing it. MEASURED on a still frame as deviation from the
    // 3x3 MEDIAN: 0.248% of pixels past 64 codes with RT on vs 0.019% off; raising shadow rays to 4
    // barely moved it (0.242%, not the shadow), disabling traced sky occlusion did (0.082%, mostly
    // this term).
    //
    // Clamping to the neighbourhood's mean +/- k*sigma (the firefly fix TAA has used for years) works
    // because an impulse is BY DEFINITION a value its neighbours don't share; a real feature survives
    // because it's supported on one side. 2 SIGMA: tighter eats real gradients (this renderer has
    // already paid for that once), looser misses outliers that are many sigma out. Minimum tap count
    // 3 stops a silhouette pixel (most of its kernel rejected) from being clamped to nothing.
    float clamped = centre;
    if (nCount >= 3.0) {
        const float mean  = nSum / nCount;
        const float sigma = sqrt(max(nSum2 / nCount - mean * mean, 0.0));
        // Floor under sigma: a perfectly flat neighbourhood has zero variance, and clamping to
        // [mean, mean] there would erase the centre's own legitimate detail along with its noise.
        const float k = 2.0 * max(sigma, 0.02);
        clamped = clamp(centre, mean - k, mean + k);
        // The blurred average carried the UNCLAMPED centre at weight 1; correct in place rather than
        // re-running the loop.
        acc += clamped - centre;
    }

    const float velPx = reproj ? length(centrePx - pixel) : 0.0;
    const float trust = gRtDenoiseParams.z > 0.0 ? saturate((3.0 - velPx) * gRtDenoiseParams.z) : 1.0;
    // Clamped first, then blended, so the impulse is gone before the linear filter sees it, and a
    // run with the blur turned down still gets the clamp.
    return saturate(lerp(clamped, acc / wsum, saturate(gRtDenoiseParams.y) * trust));
}

// `coneAo`: the cone gather's own occlusion -- smooth, deterministic, ALREADY COMPUTED at every tier
// (at Epic it was computed then thrown away -- coneTracedIndirect's own `rdAo` comment says so). Free
// as a prior.
// `nrdAoUsable`: whether this pass's OWN inputs produced gNrdAo -- the zero-dimensions test below only
// tells "allocated" vs "absent", not "produced from this frame's inputs".
//
// MEASURED 2026-09-22, PTTest NewSponza, deep shadow (reference <8 luminance, 71% of viewport):
// ray-driven primary visibility read 12.68 vs raster's 7.87 vs path-traced truth 1.58 -- washed-out
// darks, reported by the owner as "raster looks more realistic, the darkness is truly dark".
// Bisecting every term in both paths found the whole of it here: the FRESH trace agreed between
// paths (median 0.00, correctly occluded); only after NRD's override did ray-driven diverge to
// ~0.83 open while raster stayed ~0.
//
// WHY: gNrdAo (REBLUR_DIFFUSE_OCCLUSION) reprojects using motion vectors/depth/normals the RASTER
// path writes. Ray-driven primary visibility runs with the G-buffer off (VoxiRenderer selects
// rayDrivenTexPso_ precisely then), so those inputs aren't this frame's, and the denoised answer
// isn't about this frame's geometry -- but the texture stays bound and non-zero-dimensioned, so the
// test can't see the mismatch.
//
// A PARAMETER, NOT A REMOVAL: disabling it regressed raster (7.87->25.30, MAD 10.63->22.75; raster
// DEPENDS on this denoised answer) while helping ray-driven (12.68->8.98, MAD 12.59->9.83, closer to
// truth than raster; high-freq energy 1.486->1.172 vs raster's 1.035).
//
// BETTER FIX, NOT TAKEN: give ray-driven correct NRD inputs (motion vectors/depth for a ray-traced
// primary hit) so the denoiser earns its place there too -- VoxiRenderer/NRD wiring work, not a
// shader change. When it lands, flip this parameter back to true.
//
// `coneAoIsGather`: whether `coneAo` was actually measured (coneTracedIndirect ran); under ReSTIR GI
// or with GI off the caller's `ao` is still its 1.0 init.
float rtSkyOcclusionTemporal(float3 wpos, float3 N, float2 pixel, uint rays, float coneAo,
                             bool coneAoIsGather, bool nrdAoUsable) {
    // gRtDenoiseParams.w, NOT gRtHistParams.x: the shadow/reflection pairs exist at every RT tier, but
    // this AO history pair is allocated only while a sky occlusion ray is wanted (VoxiRenderer::
    // aoHistoryWanted; touching a null UAV is undefined). rays>0 here means a TRANSITIONAL frame (just
    // after a resize/render-scale change/AO switched on, before the pair exists) or a failed allocation
    // -- T2 below never applies since there's no history pair.
    //
    // THE CONE'S ANSWER ONLY WHEN THE CONE ANSWERED: under ReSTIR GI `coneAo` is the constant 1.0, and
    // returning it painted full sky ambient on every enclosed surface (same constant that caused the
    // ray-driven motion wash as a reprojection prior below) -- this pixel's own trace is right in
    // expectation instead.
    if (gRtDenoiseParams.w < 0.5) {
        // rtAmbientTraced, not the rtSkyOcclusion wrapper: this function needs the hit distance too --
        // one of the callers the wrapper's own comment calls "meaning to use them" -- and the wrapper
        // is just a field select at identical cost.
        const AverAmbientTraced amb = rtAmbientTraced(wpos, N, pixel, rays);
        return coneAoIsGather ? coneAo : amb.open;
    }

    const float curDepth = mul(float4(wpos, 1.0), gViewProj).w;

    // ---- T2 (Settings::rtSkyOcclusionHalfRate, console voxi.rtSkyOcclusionHalfRate) -- DECIDED
    // BEFORE THE TRACE so a skip avoids the ray rather than discarding it ----
    //
    // Reprojection is hoisted up here (a texture lookup, not a ray, so free to evaluate early) so the
    // skip decision can see it before tracing. With the bit clear, skipTrace is always false and every
    // line from here to the trace computes exactly what it did before this hoist.
    //
    // A WHOLE 8x8 TILE (one CSRdSkyOcc thread group) shares the skip decision, viewport-relative
    // (gSceneViewportCur.xy) so it lines up with the dispatched group. Parity alternates by frame
    // index, so a skipping tile traces next frame -- half the tiles trace every frame, not the same
    // half forever.
    //
    // SKIPPING ONLY WHEN THIS PIXEL'S OWN HISTORY REPROJECTS VALIDLY is the whole safety condition: a
    // disoccluded pixel always traces regardless of tile parity -- the same ray-driven motion-wash
    // lesson below (SEEDED FROM THIS PIXEL'S OWN TRACE): standing on a prior for newly revealed
    // surface reads as a grey wash taking ~33 frames to clear.
    float histV = 0.0;
    float2 velocityPx = 0.0;
    const bool haveHist = gRtHistParams.y > 0.25 && rtReprojectAo(wpos, pixel, histV, velocityPx);

    bool skipTrace = false;
    if ((rtGiShadowBits() & 2u) != 0u && haveHist) {
        const uint2 tile = (uint2(pixel) - (uint2)gSceneViewportCur.xy) / 8u;
        skipTrace = ((tile.x ^ tile.y ^ (uint)gRtHistParams.z) & 1u) != 0u;
    }

    // `tracedNow` GATES THE HIT-DISTANCE WRITE below. gAoHitDistOut (u5) is not ping-ponged -- only
    // NRD's external accumulation reads it back -- so a skipped pixel leaves it UNWRITTEN, at most one
    // frame stale under this checkerboard (a skipping tile traced last frame, will trace next); gAoHist
    // carries (openness, depth), not hit distance, so there's no reprojected value to write instead.
    float fresh;
    float hitDist = 0.0;
    bool  tracedNow;
    if (skipTrace) {
        // The reprojected history IS this frame's fresh estimate: `vis = lerp(fresh, histV, weight)`
        // with fresh=histV reduces to histV exactly (lerp(a,a,t)==a), so the temporal blend "keeps it"
        // with no special case, and the spatial filter below still runs.
        fresh = histV;
        tracedNow = false;
    } else {
        const AverAmbientTraced amb = rtAmbientTraced(wpos, N, pixel, rays);
        fresh = amb.open;
        hitDist = amb.hitDist;
        tracedNow = true;
    }
    // ---- SEEDED FROM THIS PIXEL'S OWN TRACE, not `coneAo` as before ----
    //
    // Old reasoning (still correct on its own terms): at one ray `fresh` is BINARY, so an unseeded
    // disocclusion writes a coin flip into history. Measured scale-free (each image normalised to its
    // own mean; two other metrics were tried and discarded as confounded by the exposure an ablation
    // changes), seeding from the cone took bright speckle from 0.071% of pixels to 0.012%.
    //
    // BUT under giMode=1, `coneAo` is a hardcoded 1.0 (giRestirIndirect in voxi_restir.hlsli sets it
    // once and never reassigns it -- the identifier appears exactly twice in an 800-line body, and
    // voxi.hlsl skips coneTracedIndirect entirely in that mode), and PTTest.ocproject ships giMode 1
    // -- the scene every measurement here uses.
    // MEASURED: `ao`/`rdAo` render flat 1.0 in both paths, deep shadow included (230.71 vs white
    // 230.81), while the cone gather itself is healthy (alpha 0.90, would give ao=0.10); `fresh` is
    // nearly exact instead (1.50 vs truth 1.35). So the real choice is "constant FULLY OPEN" vs "this
    // pixel's own noisy-but-correct-in-expectation measurement", and a prior is only consulted on
    // disocclusion -- exactly where a full-open constant paints sky and takes ~33 frames to clear.
    //
    // THE FIX FOR RAY-DRIVEN'S GREY WASH WHILE THE CAMERA MOVES (an earlier version of this comment
    // called it invisible; that measurement's motion was too gentle to fail reprojection anywhere that
    // mattered): every shadowed wall going mottled grey, clearing ~1s after stopping -- a revealed
    // strip with no history read coneAo=1.0. The owner's shots fit an ADDITIVE lift, not exposure (no
    // single gain matches every band: darkest needs x22, brightest x1.08). MEASURED, PTTest gallery
    // (--cam -577 85 746 0 90), matched pose (--cam-wobble 30 16 --cam-wobble-stop 60, 63 vs 180
    // frames), moving vs settled:
    //   coneAo prior, auto-exposure on:  +10.14 mean, 12.1% of pixels past 32 codes -- the report
    //   fresh prior,  auto-exposure on:   -0.15 mean,  0.4%
    //   coneAo prior, fixed exposure 4:   +6.78        fresh: -0.09
    //   flying 30/frame down the gallery: +2.93        fresh: +0.63
    // voxi.debugResetHistoryEveryFrame 2 (every pixel on the prior, still camera): coneAo +31.2 (deep
    // shadow 11%->0.07%); fresh +0.02. Still-frame numbers unchanged (3.61 raster / 3.71 ray-driven vs
    // reference, parity 0.98). A gentle rig (--cam-translate 3 --cam-wobble 8 40) barely moved it
    // (4.10->4.09): a matched-pose rig only tests a prior if the motion reveals real screen area.
    //
    // THE SIBLING CASE (early-out above, no AO pair bound) makes the same choice -- see coneAoIsGather.
    float vis = fresh;
    // haveHist/histV/velocityPx: the SAME rtReprojectAo call this function always made here, just
    // hoisted above (see the T2 comment) so the skip decision could see it before tracing.
    if (haveHist) {
        const float t      = saturate(length(velocityPx) / 32.0);
        // THE SHADOW PATH'S OWN WEIGHTS, and 0.95 was tried rather than assumed. Doubling the
        // effective sample count to ~20 moved dark-region local roughness from 0.4509 to 0.4486 --
        // nothing, because what is left in a still frame is Sponza's stone TEXTURE, not sampling
        // noise, and that floor is the same in every configuration measured (the old four-ray tile
        // scored 0.3929 on it). Deeper history is not free -- it is lag on a disocclusion the depth
        // test does not catch -- so the value that buys nothing is not the one to ship.
        // 0.97, not 0.9 (the paragraph above is stale, not wrong): 0.95 once measured no change when a
        // still frame's residual was Sponza's stone TEXTURE, not sampling noise. The sky irradiance
        // calibration made the ambient this multiplies 8x larger, so the same relative variance is now
        // 8x more visible, and this term (not the texture) is the floor. ~33 frames of history vs ~10.
        const float weight = lerp(0.97, 0.5, t);
        vis = lerp(fresh, histV, weight);   // history exists, so the traced sample is the UPDATE
    }
    // ---- NRD's ANSWER WINS WHEN THERE IS ONE, REPLACING the blend above rather than filtering it ----
    //
    // gNrdAo (REBLUR_DIFFUSE_OCCLUSION on last frame's hit distance) shares AverAmbientTraced::hitDist's
    // units exactly (fraction of TMax, 1="nothing in the way"), so no remap.
    //
    // REPLACES THE EMA, DOESN'T FEED IT: handing a denoised value into the 0.97 blend would feed a
    // filter its own output and double-count NRD's own temporal accumulation.
    //
    // ZERO-DIMENSIONS TEST is the bound-or-not signal (same one averBlendBackdropValid uses): absent
    // on Vulkan, without NRD, or without the G-buffer -- t14 can't be assumed present.
    uint nrdW = 0, nrdH = 0;
    gNrdAo.GetDimensions(nrdW, nrdH);
    // W6/M5: `gAverHistoryWrite &&` leads this test so a blended-replay fragment (glass/water pane,
    // PSMainVoxi) doesn't read back the OPAQUE surface's own denoised answer -- the mis-attribution
    // this task's C9 finding named ("a pane handed the denoised GI/AO of the surface behind it").
    // Skipping leaves `vis` at this fragment's own fresh/history blend; the pane's write below is
    // gated the same way. See voxi.hlsl's gAverHistoryWrite/averDrawIsTranslucent, and
    // voxi_restir.hlsli's identical gate on gGiRadianceOut's NRD GI readback.
    if (nrdAoUsable && gAverHistoryWrite && nrdW > 0u && nrdH > 0u) {
        // Still written to history below: next frame's reprojection reads it, and the pass falls
        // back to it the moment NRD stops running (e.g. G-buffer switched off).
        vis = saturate(gNrdAo.Load(int3(pixel, 0)).r);
    }

    // ACCUMULATED, not fresh (writing fresh would restart the average every frame, same trap the
    // shadow path documents), and RAW, never filtered (feeding the spatially filtered result back
    // would make this an IIR filter whose artefacts compound; the filter applies on the way OUT,
    // below).
    //
    // W6/M5: gated on gAverHistoryWrite (on by default, D3 decision) -- see voxi.hlsl's
    // gAverHistoryWrite/averDrawIsTranslucent. Before this gate, a blended (glass/water) replay pass
    // wrote this texel a second time with the PANE's own measurement, overwriting the opaque
    // surface's (the "non-atomic double write" this file's C9 comment used to accept unfixed).
    // voxi.legacyBlendedHistoryWrite (gAmbientParams.z bit 32) restores the old write, byte-identical,
    // for A/B.
    if (gAverHistoryWrite) gAoHistOut[uint2(pixel)] = float2(vis, curDepth);
    // THE RAW MEASUREMENT, deliberately the opposite of the accumulated write above (safe to touch
    // only after the gRtDenoiseParams.w early-out above): this goes to an external denoiser with its
    // own history, so handing it an already-blended value would be the same IIR trap -- worse here
    // than the write above, since a glass pane's replay used to overwrite it with the PANE's hit
    // distance going straight out as THIS FRAME'S measurement, unaveraged. Gated the same way, for the
    // same reason -- this write, the one above, and gRtShadowHistOut further down are all now
    // consistent about which fragment last wrote them.
    //
    // tracedNow &&: T2's own gate (see its header above) -- a skipped pixel has no fresh hitDist and
    // leaves this texel as its last real trace left it.
    if (tracedNow && gAverHistoryWrite) gAoHitDistOut[uint2(pixel)] = hitDist;
    return rtAoSpatial(vis, wpos, N, pixel, curDepth);
}

// The SPATIAL denoiser: average this pixel's shadow with its neighbours' from the history texture,
// weighted by how well each neighbour's surface agrees with this one's.
// READS LAST FRAME'S TEXTURE SAFELY: t6/u2 are different ping-ponged textures (t6 rests in
// ShaderResource for the whole colour pass), a plain load needing no barrier. Unlike the TEMPORAL
// path's reuse of a value up to 2^(2*tileBits) frames old, the centre here always contributes its
// own fresh trace and a failing neighbour is DROPPED, never substituted.
// AVERAGING IS AN ESTIMATE, NOT A BLUR: rtShadow jitters the ray origin across the pixel's own
// footprint, so neighbours on one flat receiver already sample different points of the same surface.
float rtShadowSpatial(float centre, float3 wpos, float3 N, float2 pixel, float curDepth) {
    const int radius = (int)gRtDenoiseParams.x;
    if (radius <= 0 || gRtHistParams.y < 0.75) return centre;

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
    if (gRtHistParams.y > 0.75 && pdepth > 1e-4) {
        const float3 pndc = pclip.xyz / pdepth;
        if (pndc.z >= 0.0 && pndc.z <= 1.0) {
            centrePx = gSceneViewport.xy +
                       float2(pndc.x * 0.5 + 0.5, 0.5 - pndc.y * 0.5) * gSceneViewport.zw;
            reproj = true;
        }
    }
    const int2 base = int2(floor(centrePx));

    // Plane-distance rejection, not a raw depth delta: centre depth + screen-space gradient defines
    // the receiver's plane, so a neighbour ON it is kept regardless of depth. A plain |dz| test fails
    // on a grazing floor.
    //
    // EXPRESSED IN LAST FRAME'S DEPTH WHENEVER THE GATHER REPROJECTED -- the motion fix. Taps load
    // gRtShadowHist's LAST-frame depth, but a predictor built from curDepth (THIS frame's) diverges
    // under motion with the yaw rate, spending the 2% tolerance asymmetrically (accept receding side,
    // reject approaching) -- a biased average that reads as a region shifting brightness, worsening
    // with radius since farther taps disagree more.
    const float planeDepth = reproj ? pdepth : curDepth;
    const float dzdx       = reproj ? dpdx   : cdx;
    const float dzdy       = reproj ? dpdy   : cdy;

    // Gaussian falloff (used to give every accepted neighbour weight 1.0 -- a flat kernel rings in
    // frequency response, a visible square halo around a bright feature). sigma = radius/2, matched to
    // rtReflectionSpatial's so the two filters don't disagree in shape.
    //
    // LIVE AT EVERY REAL TIER (a comment here used to claim the opposite, that rtShadowDenoiseForQuality
    // returns 0 at every tier -- FALSE: Voxi.cpp returns 2 for Low/Medium/High, 1 for Epic, only
    // Off/unknown return 0; Medium is default). CAUGHT BY THE ORACLE: re-recording gates moved 15
    // values, three in every RT-capable configuration, including WARP:
    //   penumbra-rt   33,39,48 -> 62,64,66      partially occluded, much brighter
    //   rt-penumbra   91,88,85 -> 96,93,89      partially occluded, brighter
    //   ms-rt-gi      52,19,13 -> 38,15,11      same sunVis feeding the GI-composited path
    // shadow-rt/shadow-ms-rt didn't move at all -- full umbra pins every tap at 0, while a penumbra's
    // old flat kernel dragged the estimate toward far neighbours the Gaussian now discounts (0.135 at
    // distance 2, 0.018 at the corner) -- bit-identical across hardware/WARP, confirmed by an
    // independent verify pass. Lesson: an "inert" change is one nobody reviews visually.
    const float sigma  = max((float)radius * 0.5, 0.5);
    const float inv2s2 = 1.0 / (2.0 * sigma * sigma);

    float acc = centre;
    float wsum = 1.0;
    // Neighbourhood statistics for the clamp below, EXCLUDING the centre (a statistic containing it
    // can't say whether the centre disagrees). Free: taps already loaded/depth/crease-tested.
    float nSum = 0.0, nSum2 = 0.0, nCount = 0.0;
    [loop] for (int oy = -radius; oy <= radius; ++oy) {
        [loop] for (int ox = -radius; ox <= radius; ++ox) {
            if (ox == 0 && oy == 0) continue;
            const int2 t = base + int2(ox, oy);
            if (any(t < 0) || t.x >= (int)texW || t.y >= (int)texH) continue;
            const float2 st = gRtShadowHist.Load(int3(t, 0));
            // What this neighbour's depth WOULD be on the centre's plane, in the same frame st.y was
            // recorded (see planeDepth).
            const float predicted = planeDepth + dzdx * (float)ox + dzdy * (float)oy;
            const float tol = max(abs(predicted), 1.0) * 0.02 + 1.0;
            if (abs(st.y - predicted) > tol) continue;
#if AVER_GBUFFER_HISTORY
            // CREASE TERM: a depth-plane test can't see a normal DISCONTINUITY (wall meeting floor).
            // Sampled at LAST frame's normal (same tap `t`) to match gRtShadowHist's own frame.
            const float3 nb = gGBufNormalHist.Load(int3(t, 0)).xyz * 2.0 - 1.0;
            // HARD REJECT: a disagreeing normal is the wrong surface, not noise. cos(60deg), not
            // tighter -- averPackNormalRoughness quantises to RGB10A2, so tighter rejects a flat
            // surface's own quantisation noise.
            if (dot(N, nb) < 0.5) continue;
#endif
            const float w = exp(-(float)(ox * ox + oy * oy) * inv2s2);
            acc  += st.x * w;
            wsum += w;
            nSum += st.x; nSum2 += st.x * st.x; nCount += 1.0;
        }
    }

    // gRtDenoiseParams.y: blend weight of the filtered value. At 0, taps still run (radius is a
    // runtime constant) but this returns `centre` exactly -- the cost-measurement configuration.
    //
    // TAPERED BY THIS FILTER'S OWN REPROJECTION VELOCITY, to stop shadows flickering under camera
    // motion. MEASURED on Sponza, still camera vs six-degree wobble at a matched pose (both at
    // sin(phase)=0, isolating motion history as the only difference), population of differing pixels
    // (a mean is 0.01 of a code -- the affected pixels are few but change a lot):
    //   radius 0 (filter off)   9-32 codes: 0.000%   33+: 0.000%   max delta   4
    //   radius 1                9-32 codes: 0.187%   33+: 0.002%   max delta  60
    //   radius 2 (the default)  9-32 codes: 2.445%   33+: 0.177%   max delta  95
    //   radius 2, 8 rays/pixel  9-32 codes: 0.205%   33+: 0.003%   max delta  64
    // Mechanism is THE KERNEL'S SAMPLE SET SLIDING, not staleness or the plane test: rtShadow's
    // per-pixel jitter (rtHash(pixel)) makes the raw estimate at a given pose the SAME every frame
    // (filter off: moving and still agree to 4 codes), but once the gather centre reprojects each
    // frame averages a DIFFERENT 25 taps of one static noise field -- spatially noisy but temporally
    // stable becomes temporally unstable. Scales with radius; 8 rays largely fixes it (not affordable,
    // this taper is).
    //
    // NOT A DISOCCLUSION TEST (rtReprojectHistory already rejects depth-disagreeing taps) -- this is
    // about taps that are all individually valid and still average to a different number. Full
    // strength below half a pixel of drift (stationary camera bit-for-bit unchanged), gone by three.
    // gRtDenoiseParams.z is the falloff rate; 0 means NO TAPER, not "taper to nothing instantly" -- a
    // knob whose off position silently disabled the whole filter would be measured by accident.
    //
    // THE OUTLIER CLAMP THIS FILTER NEVER HAD (its AO twin already argues the case: a Gaussian is
    // LINEAR, spreading an isolated extreme rather than removing it -- raising shadow rays 1->16
    // moved the impulse metric not at all, 0.080%->0.080% past 64 codes, ruling out the sampler).
    // Same shape as rtAoSpatial: mean +/- 2 sigma, sigma floor, three-tap minimum.
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
// frame). At 0 it now ACCUMULATES (blends fresh against reprojected history) instead of the old plain
// write, which discarded every previous measurement at every shipped tier (tile edge 1 everywhere).
// Tiling cuts ray COUNT; blending is what makes one ray behave like ten.
// Splits a tinted visibility into the scalar the denoiser filters and the colour it does not.
//
// WHY THE HISTORY DIDN'T GROW: occlusion is binary/noisy (what filters smooth); a medium's tint is
// smooth and near-noise-free, so filtering it buys nothing. Scalar keeps the existing RG32Float
// history bit-for-bit; RGBA32Float would double two ~56MB buffers, RGBA16Float would drop the depth
// channel to 8cm precision where the disocclusion test reads it.
float averShadowLum(float3 v) { return dot(v, float3(0.2126, 0.7152, 0.0722)); }

// The normalised colour of a tinted visibility. White when there is effectively nothing to tint --
// a fully occluded pixel has no medium colour to speak of, and dividing by its luminance would be a
// 0/0 that spreads NaN through the filter.
float3 averShadowTint(float3 v, float lum) {
    return (lum > 1e-4) ? (v / lum) : float3(1.0, 1.0, 1.0);
}

// A: SUN SHADOW SPLIT (Settings::rayDrivenShadowTiles) -- every caller's shape, plus one escape hatch.
// `haveFresh`/`freshIn` let a caller that has ALREADY CLASSIFIED this pixel's fresh trace (CSRdShadow's
// AVER_RD_SHADOW_TILES compile: a 3x3 probe-tile neighbourhood agreeing "fully lit"/"fully blocked")
// hand that in directly, skipping the ray loop while still running every temporal/spatial step. THE
// SKIP IS EXACT ONLY WHERE THE NEIGHBOURHOOD TRULY IS UNIFORM (a sampling claim, not an identity --
// "every probe in 576 pixels agreed" stands in for "every sample would agree"; at 4/8 rays the probe
// sees one rotated sample per pixel; CSRdShadowProbe rotates which sample each pixel traces to
// represent every disc radius in every tile -- see its own comment for the fixed-radius version that
// failed this). `false`/anything is the ordinary path: every `if (!haveFresh)` below reduces to the
// plain rtShadow(...) it replaced, exactly what rtShadowTemporal's own wrapper asks for, so nothing
// already calling it changes behaviour.
float3 rtShadowTemporalEx(float3 wpos, float3 N, float3 L, float2 pixel, float3 dpx, float3 dpy, uint rays,
                          bool haveFresh, float3 freshIn) {
    // gRtHistParams.x is 0 when t6/u2 aren't bound this frame (VoxiRenderer::beginShadowHistory) --
    // an unbound slot is Tier 1 null-filled, so touching either would hit a null descriptor.
    // IF/ELSE, NOT ?:, at all three sites: the whole point of haveFresh is that the ray loop does not
    // run, so the skip is spelled as control flow rather than left to the language's evaluation rules.
    if (gRtHistParams.x < 0.5) {
        if (haveFresh) return freshIn;
        return rtShadow(wpos, N, L, pixel, dpx, dpy, rays, 0.0);
    }

    // THIS frame's linear depth at wpos (mul(wp, gViewProj).w, as VSMain computes it) since the
    // shadow pass has no depth buffer to read back. Written alongside visibility below so next
    // frame's rtReprojectHistory always sees a fresh depth, not one stale since this pixel's last turn.
    const float curDepth = mul(float4(wpos, 1.0), gViewProj).w;

    const uint tileBits = (uint)gRtHistParams.w;
    if (tileBits == 0u) {
        // SAME GOLDEN-ANGLE FRAME JITTER AS THE TILED PATH BELOW -- passing 0.0 here instead is what
        // made this branch's accumulation buy nothing: rtShadow's ang0 becomes a pure function of the
        // PIXEL with jitter=0, so it traces one fixed direction forever, and the exponential average
        // below then averages ten copies of one sample (a comment here once said this "converges
        // toward the many-ray answer" -- it can't). At rays=1 (every shipped tier below Epic) a
        // partially occluded pixel reported one hard 0/1-ish sample forever.
        //
        // MEASURED, rt-penumbra probe (gates.ps1:296, --sun-angle 8.0, 1204 frames) vs a converged
        // 16-ray/tile-1 ground truth (motion-invariant, so a real ground truth; 139,136,133 still,
        // 137,134,133 moving):
        //   tile 1, 1 ray (EVERY SHIPPED TIER)   88,92,99    error -51
        //   tile 2, 1 ray                        132,129,128 error  -7
        //   tile 4, 1 ray                        137,134,132 error  -2
        // The amortised tiles differ only by passing this jitter, and are an order of magnitude more
        // accurate. Tell: tile 1 read 126 moving vs 88 still -- motion was accidentally decorrelating
        // it via reprojection across different rtHash values (an estimator that improves when shaken
        // is not sampling).
        //
        // FRAME INDEX IS SAFE HERE despite the standing pure-function-of-pixel rule for the gate
        // oracle: frameIdx is deterministic (a --frames N run always ends on the same index). It's
        // only a hazard when indexed INTO the tiled path's radical-inverse sequence (a power-of-two
        // stride pins low bits -- measured, and recorded as a disproven fix); this is an angular
        // offset with stride 1. Recorded RT gate values move; they are re-recorded, not suppressed.
        const float frameJitter = (float)((uint)gRtHistParams.z) * 2.39996323;
        float3 fresh3 = freshIn;
        if (!haveFresh) fresh3 = rtShadow(wpos, N, L, pixel, dpx, dpy, rays, frameJitter);
        const float  fresh   = averShadowLum(fresh3);
        const float3 tint    = averShadowTint(fresh3, fresh);

        // TEMPORAL ACCUMULATION belongs on THIS branch specifically: tiling is RAY AMORTISATION
        // (trace one pixel in N, reuse the rest), and switching it off means "trace every pixel every
        // frame", not "discard every previous measurement" -- but this branch used to write the raw
        // trace to history and never read it back, so at rtPixelsPerRayTile 1 (every shipped tier)
        // reprojection was dead code.
        //
        // MEASURED: one ray/pixel's noise is what shadows were flickering with. The spatial filter
        // gathers 25 taps around a REPROJECTING centre, so a moving camera averages a different
        // subset of one static noise field each frame. Sponza, still vs six-degree wobble at matched
        // pose, counting pixels: filter off agrees to 4 codes; radius 1, 60; radius 2 (default), 95
        // with 2.4% of the frame past 9 codes. Raw trace is temporally stable/spatially noisy; the
        // filter converts one into the other. 8 rays/pixel cuts it ~12x (not affordable).
        //
        // Accumulating buys the same variance reduction for one texture read: at rest the 0.9 weight
        // averages ~10 frames, converging toward the many-ray answer. rtReprojectHistory's depth test
        // keeps it honest across a disocclusion; the velocity term discounts history as reprojection
        // gets less trustworthy -- both already written, unreachable until now.
        //
        // THE ACCUMULATED VALUE IS STORED, not the fresh trace -- writing fresh would restart the
        // average every frame.
        float vis = fresh;
        float histV = 0.0;
        float2 velocityPx = 0.0;
        // FAR LOOSER VELOCITY BUDGET THAN THE TILED PATH: there, history can be 2^(2*tileBits) frames
        // old so staleness compounds with motion (6px is fair). Here history is always EXACTLY one
        // frame old; landing on a different surface is caught by the depth test, not velocity, and
        // sub-texel error is bounded to half a texel at any speed (nearest-neighbour). Reusing 6
        // throttled weight to 0.1 at the six-degree wobble's 24.7 px/frame -- switching accumulation
        // off in exactly the case it was added for.
        if (gRtHistParams.y > 0.75 && rtReprojectHistory(wpos, pixel, histV, velocityPx)) {
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
        // W6/M5: gated on gAverHistoryWrite (on by default, D3 decision) -- see voxi.hlsl's
        // gAverHistoryWrite/averDrawIsTranslucent. Before this gate, a blended (glass/water) replay
        // fragment overwrote this texel with the PANE's own measurement (same non-atomic double
        // write as the AO history above). voxi.legacyBlendedHistoryWrite (gAmbientParams.z bit 32)
        // restores the old unconditional write, byte-identical, for A/B.
        if (gAverHistoryWrite) gRtShadowHistOut[uint2(pixel)] = float2(vis, curDepth);
        // FILTERED HERE TOO: this branch is what Medium (default tier) runs, and used to return
        // `fresh` unfiltered, leaving the default's hard 0/1 shadow untouched -- the penumbra probe
        // read an unchanged 61,59,59 at every radius, which caught it. Two returns, two call sites --
        // easy for a later edit to drop one again.
        // SATURATED: `tint` is the RAW ratio v/lum (unfiltered), while `vis` went through a 0.97
        // temporal blend and spatial filter -- the two are designed to disagree, so tint*lum can
        // exceed the transmittance it came from -- averShadowTint's denominator floor (1e-4) bounds
        // the quotient to 1/0.0722=13.85 on blue, so vis=0.05 with a saturated tint reports 0.69
        // (two-thirds lit) into AverLight::visibility, which material_prelude documents as "(1,1,1)
        // = fully lit".
        // A visibility can't exceed one; saturating costs nothing.
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
    const bool haveHist = gRtHistParams.y > 0.75 && rtReprojectHistory(wpos, pixel, hist, velocityPx);

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
        float3 vis3 = freshIn;
        if (!haveFresh) vis3 = rtShadow(wpos, N, L, pixel, dpx, dpy, rays, frameJitter);
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
    // a filtered value back would make this an IIR filter whose artefacts compound every frame; the
    // filter applies on READ, below -- writing it here to "save work" is the bug, not the
    // optimisation. Same gate/reason as the branch above.
    if (gAverHistoryWrite) gRtShadowHistOut[uint2(pixel)] = float2(vis, curDepth);
    return saturate(rtShadowSpatial(vis, wpos, N, pixel, curDepth) * tint);
}

// The ordinary entry point most callers need (every caller before this task used it): haveFresh=false
// makes every `if (!haveFresh)` above take its rtShadow(...) branch, so this is rtShadowTemporalEx
// exactly as before A split the trace out from under it.
float3 rtShadowTemporal(float3 wpos, float3 N, float3 L, float2 pixel, float3 dpx, float3 dpy, uint rays) {
    return rtShadowTemporalEx(wpos, N, L, pixel, dpx, dpy, rays, false, float3(0.0, 0.0, 0.0));
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
    // Opaque lane only (AVER_RT_MASK_OPAQUE_ALL): wants solid surfaces, excludes translucent by mask.
    // FORCE_OPAQUE used to be a no-op here (createBlas marks every geometry OPAQUE, so only the
    // excluded translucent lane was ever un-opaqued) and is now provably WRONG: alpha-masked instances
    // stay in the OPAQUE lane (they occlude, they cast shadow) but are non-opaque so a ray can see
    // their holes. With the flag on, the hardware would commit the leaf card unasked.
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
    // Whether the SUN reaches the reflected surface (without this every reflection glows unshadowed).
    // Seeded from the PIXEL with NO footprint (seeding from hitPos.xy would make it depend on ray
    // distance, likely to differ between adapter and WARP). ONE ray, not the full disc -- the single
    // largest saving in the ray path (was 5 rays, now 2; a reflected penumbra isn't resolvable in a
    // one-bounce mirror image anyway). float3: a tinted medium tints this too.
    // frameJitter, NOT 0.0: rtShadow's disc angle is rtHash(pixel)*2pi + frameJitter, so zero froze
    // this ray to one pixel-pure direction forever, a binary value that temporal accumulation
    // converges TO rather than averages away -- the identical defect fixed for
    // the PRIMARY shadow ray (421a01e3), left behind here. MEASURED: of isolated bright outliers in a
    // shadowed frame, 60.7% sit on the same pixels two frames running (98.0% of dark ones) -- a
    // deterministic per-pixel error, not sampling noise.
    //
    // T1 (Settings::rtSecondaryShadowOpaque): hitPos is a SECONDARY hit, what rtShadowOpaque exists
    // for (see its header for the ray/trade). if/else, not ?:, so the bit alone selects the ray.
    float3 shadow;
    if ((rtGiShadowBits() & 1u) != 0u) {
        shadow = rtShadowOpaque(hitPos, nWS, L, pixel, frameJitter);
    } else {
        shadow = rtShadow(hitPos, nWS, L, pixel, float3(0,0,0), float3(0,0,0), 1u, frameJitter);
    }

    // LAMBERTIAN EXITANT RADIANCE; the /PI is the whole point (averGroundRadiance's reference:
    // E = sunIrradiance*ndl + PI*skyRadiance*ambient; return albedo*E/PI -- sky's PI cancels, sun's
    // doesn't). Omitting it made SUNLIT reflections 3.14x too bright (shaded ones stayed correct,
    // read as an exposure bug) -- moved lit ray-traced gates from 94,27,14 to 131,58,40. The white
    // furnace (sun off) doesn't catch this, since it tests only the ambient half; a furnace with a
    // sun would be worth having.
    float3 direct = averSunRadiance() * saturate(dot(nWS, L)) * shadow / PI;
    float3 ambient = averSkyIrradiance(nWS) * gAmbient.r;
    hit = true;

    // `inst.albedo` is the per-draw FLAT colour, fine when everything was flat but reading as "the
    // reflection is fake" once directly-viewed surfaces carry real base-colour maps. Everything
    // needed is already here, so a reflected surface resolves its UV the same way a direct one does.
    float3 reflAlbedo = inst.albedo;
    // THE REFLECTED SURFACE'S OWN EMISSION, added to the result and not scaled by its albedo or by
    // the sun reaching it, so a lamp bulb glows in a mirror as it does seen directly.
    float3 emission = 0.0;
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

#if !AVER_RD_SINGLE_PASS
        // The emissive map on the base colour's footprint, white (the identity) when unbound.
        emission = rmat.emissiveFactor
                 * averRtSampleSlot(rmat, 4, ruv, rgx, rgy, float4(1, 1, 1, 1)).rgb;
#else
        // The factor alone: the single-pass compile is at the AMD driver's register limit
        // (rtGiShadowBits() above), so no new texture sample on this secondary-hit path.
        emission = rmat.emissiveFactor;
#endif
    }
#else
    // No texture table on this compile: the factor alone.
    emission = gRtMaterials[inst.materialIndex].emissiveFactor;
#endif
    return reflAlbedo * (direct + ambient) + emission;
}
