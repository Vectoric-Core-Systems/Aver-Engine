// voxi_restir.hlsli -- the RTXDI ReSTIR GI block, extracted whole out of voxi.hlsl (giMode == 1).
//
// WHAT THIS FILE HOLDS: the vendored-SDK includes (Rtxdi/GI/ReSTIRGIParameters.h,
// Rtxdi/GI/Reservoir.hlsli, Rtxdi/Utils/RandomSamplerState.hlsli,
// Rtxdi/GI/SpatioTemporalResampling.hlsli), the reservoir buffer (gGiReservoirs, u6) and the
// RTXDI_GI_RESERVOIR_BUFFER/RTXDI_GI_ALLOWED_BIAS_CORRECTION macros that must be defined before
// Reservoir.hlsli is included, the surface-history textures (gGiSurfPosHist/Out t12/u7,
// gGiSurfNrmHist/Out t13/u8), the entire RAB_* contract RTXDI's SDK calls back into (RAB_Surface,
// RAB_EmptySurface, RAB_GetGBufferSurface, RAB_GetMaterial, RAB_AreMaterialsSimilar,
// RAB_GetGISampleTargetPdfForSurface, RAB_ValidateGISampleWithJacobian,
// RAB_GetTemporalConservativeVisibility, kGiNeighborOffsets/RAB_ClampSamplePositionIntoView), and
// this file's own two supporting functions, giReservoirBufferParams and giTraceInitialCandidate,
// ending in giRestirIndirect -- the entry point everything above exists to support.
//
// WHAT MUST PRECEDE THIS FILE'S #include LINE IN voxi.hlsl:
//   - the RT scene/geometry/material declarations: gScene (t2), RtInstance/RtMaterial,
//     gRtInstances/gRtVerts/gRtIndices/gRtMaterials (t3/t4/t5/t9), and the bindless material
//     adapter (averRtProceedSolid, averRtSampleSlot, averRtSurfaceUV, averRtUvGrad) --
//     giTraceInitialCandidate traces gScene directly and shades what it hits through that adapter.
//   - rtHash and rtDiscSample, the low-discrepancy sampling primitives giTraceInitialCandidate
//     draws its candidate direction from.
//   - the VoxiFrame cbuffer fields this file reads (gGiRestirParams, gRtHistParams, gVoxelParams,
//     gViewProj, gPrevViewProj) and gNrdGi (t15).
//   - the #if AVER_RT guard that opens earlier in voxi.hlsl must still be open at the #include
//     point. This file carries no #if AVER_RT of its own -- it is plain, guard-free text spliced
//     into an already-open conditional, exactly as if it had never left voxi.hlsl.
//
// WHAT DEPENDS ON THIS FILE: giRestirIndirect is called from PSMainVoxi and PSRayDriven, both
// defined further down voxi.hlsl, after the #if AVER_RT region closes. Both call sites must stay
// textually after this file's #include point.
//
// THE ORDERING CONTRACT INSIDE THIS FILE IS THE WHOLE REASON IT WAS SPLIT OUT ON ITS OWN --
// GET IT WRONG AND IT FAILS AT RUNTIME, NOT AT BUILD TIME:
//   - RTXDI_GI_RESERVOIR_BUFFER (a macro naming gGiReservoirs) and
//     RTXDI_GI_ALLOWED_BIAS_CORRECTION must both be #defined before "Rtxdi/GI/Reservoir.hlsli" is
//     included -- the vendored header expects them already defined as plain macro substitutions,
//     not template arguments, so gGiReservoirs itself has to be declared first too.
//   - Every RAB_* symbol (RAB_Surface, the type, most of all) must be fully declared before
//     "Rtxdi/GI/SpatioTemporalResampling.hlsli" is included. This is the exact mechanism of the
//     incident that motivated splitting this file up in the first place: an
//     `unknown type name 'RAB_Surface'` from getting this order wrong failed PSMainVoxi,
//     PSRayDriven, VSky and VSMain alike -- everything in the translation unit, including entry
//     points that never call into ReSTIR at all -- and it failed them with zero build errors, only
//     a launch that never draws. If this file is ever subdivided further, this is the ordering
//     that breaks first, and it breaks silently.
//
// Moved verbatim out of voxi.hlsl: nothing below this header comment was rewritten, reformatted,
// or otherwise changed in the move.

// Staged ray-driven milestone 4 (Settings::rayDrivenStages == 2): half-rate ReSTIR GI, traced in NRD's
// own checkerboard so REBLUR (GI denoiser index 1) reconstructs the half this file skips. Guarded by
// its own macro, defaulted to 0 here, rather than an ambient `#ifdef` test at each use site: every
// compile that never sets it (PSMainVoxi, PSRayDriven both variants, every other CS stage) sees a
// plain 0 and takes the untouched branch at each of this file's three checkerboard sites below --
// see CSRdGi (voxi.hlsl) for the one compile that defines this to 1.
#ifndef AVER_GI_CHECKERBOARD
#define AVER_GI_CHECKERBOARD 0
#endif

// GI candidate-trace/resample split (Settings::rayDrivenGiSplit): CSRdGiTrace (voxi.hlsl) traces and
// shades giTraceInitialCandidate's ray in its own dispatch and stores every out param in gRdGiCand
// (below); CSRdGi's own AVER_GI_SPLIT=1 compile then reads that record back inside giRestirIndirect
// instead of tracing again -- same guard convention as AVER_GI_CHECKERBOARD just above, so every compile
// that never sets it (PSMainVoxi, PSRayDriven, CSRdGi's own default compile) sees a plain 0 and takes
// the untouched, byte-for-byte original branch at this file's one AVER_GI_SPLIT site, in giRestirIndirect
// below. See CSRdGi (voxi.hlsl) for the compile that defines this to 1.
#ifndef AVER_GI_SPLIT
#define AVER_GI_SPLIT 0
#endif

// ================= RTXDI ReSTIR GI (Settings::giMode == 1) =================
//
// *** USES THE VENDORED SDK -- third_party/rtxdi -- RATHER THAN HAND-ROLLING A RESERVOIR UPDATE
// RULE. *** Everything below that combines candidates into one (RTXDI_MakeGIReservoir,
// RTXDI_GISpatioTemporalResampling) is a call into Rtxdi/GI/Reservoir.hlsli and
// Rtxdi/GI/SpatioTemporalResampling.hlsli; this file supplies only the two things RTXDI cannot know
// on its own -- how to TRACE a candidate and shade what it hits (giTraceInitialCandidate), and how
// to answer the RAB_* questions the SDK asks about a surface (RAB_GetGBufferSurface and its
// neighbours, just below). The Jacobian, the MIS weight and the M-cap are RTXDI's, not reimplemented
// here.
//
// SCOPE: candidate generation + RTXDI SPATIO-TEMPORAL resampling -- temporal reuse AND a spatial
// pass, not just the former. NOT the plain RTXDI_GISpatialResampling (vendored, unused): it calls
// RAB_GetGBufferSurface(idx, FALSE) for a neighbour -- THIS frame's surface at a pixel that may not
// have run yet, which a pixel shader cannot answer, and which this file's RAB_GetGBufferSurface
// honestly refuses by returning "no surface" whenever previousFrame is false. Spatio-temporal reads
// every neighbour, spatial taps included, from the PREVIOUS frame instead
// (RAB_GetGBufferSurface(idx, true), and an RTXDI_LoadGIReservoir sourceBufferIndex that is always
// the slice NOT being written this frame) -- no read of data this frame is still producing, no race,
// so it drops into this single-pass pixel-shader design where the plain spatial variant cannot.

// The cosine floor both of this file's candidate gates use, and the only bound that exists on a
// stored reservoir's 1/pdf -- see giTraceInitialCandidate, where the cost of this particular number
// is worked out. Named once so the two gates cannot drift apart again; they already had.
#define AVER_GI_MIN_COS 0.05

// The texture footprint of a candidate hit's material maps, as the tangent of a cone half-angle
// around the candidate ray: the hit samples a footprint of this times the ray length. A diffuse
// bounce needs the hit's local AVERAGE colour and metalness, not its texel detail, so this is wide
// on purpose -- a coarser mip is cheaper and no less right on average. See giTraceInitialCandidate.
#define AVER_GI_HIT_TEX_CONE 0.1

// Occupancy (CSResolve's alpha, voxi.hlsl ~3417) below which the F2 traced-hit voxel lookup treats a
// trilinear tap as having no usable radiance to divide by, rather than dividing by a near-zero
// footprint and amplifying quantisation noise into a spike -- see that branch's own comment.
#define AVER_GI_VOX_MIN_OCC 0.05

// REBLUR's hit-distance normalisation constants, MIRRORING aver::render::nrd::Denoiser::ReblurTuning
// (modules/render.nrd/include/aver/render/nrd/NrdDenoiser.hpp), which is what actually configures the
// denoiser. NRD normalises a hit distance by (A + |viewZ|*B) * lerp(C, 1, smc) and REQUIRES the
// producing shader to have divided by the same thing -- NRD.hlsli's _REBLUR_GetHitDistanceNormalization
// and REBLUR_FrontEnd_GetNormHitDist. A is a LENGTH in engine units (300cm = NRD's default 3m); B and
// C are unit-free. Restated here rather than included because NRD's headers are confined to
// modules/render.nrd by its vendoring terms, so these two declarations are commented at each other
// and have to be changed together.
#define AVER_NRD_HITDIST_A 300.0
#define AVER_NRD_HITDIST_B 0.1

// ---- the reservoir buffer RTXDI's own Reservoir.hlsli requires be defined before it is included ----
// RTXDI_GI_RESERVOIR_BUFFER must already point at a declared RWStructuredBuffer<RTXDI_PackedGIReservoir>
// by the time Reservoir.hlsli's own function bodies are parsed (it is a plain macro substitution,
// not a template argument), so the TYPE has to exist first -- pulling just the parameters header
// gets it without duplicating anything, since Reservoir.hlsli's own later #include of the same file
// is a no-op behind its include guard.
#include "Rtxdi/GI/ReSTIRGIParameters.h"
RWStructuredBuffer<RTXDI_PackedGIReservoir> gGiReservoirs : register(u6);
#define RTXDI_GI_RESERVOIR_BUFFER gGiReservoirs
// BASIC, not RAY_TRACED: the ray-traced bias-correction mode costs one MORE shadow-style visibility
// ray per pixel per temporal resample (RAB_GetTemporalConservativeVisibility, implemented below for
// completeness and because a future change to this line should not have to come back and write it),
// on top of the one this file's candidate generation already traces. Basic is RTXDI's own
// documented cheap-but-good option and is what this file selects; the #if this feeds into (RTXDI/
// GI/TemporalResampling.hlsli) compiles the ray-traced branch out entirely below this setting.
#define RTXDI_GI_ALLOWED_BIAS_CORRECTION RTXDI_BIAS_CORRECTION_BASIC
#include "Rtxdi/GI/Reservoir.hlsli"
// TemporalResampling.hlsli calls RTXDI_GetNextRandom/RTXDI_CalculateJacobian (and takes an
// RTXDI_RandomSamplerState parameter) WITHOUT including the headers that declare them -- Utils/
// RandomSamplerState.hlsli (which itself pulls in Utils/Math.hlsli, where RTXDI_CalculateJacobian
// actually lives). The caller is expected to have included it first; explicit here rather than
// relying on Reservoir.hlsli's own include chain to have happened to pull it in already.
#include "Rtxdi/Utils/RandomSamplerState.hlsli"
// TemporalResampling.hlsli is NOT included here -- it is included at the END of the RAB_ block
// below, and the order is load-bearing. See that include's own note.

// ---- the previous-frame SURFACE history RAB_GetGBufferSurface(idx, true) reads ----
// THE ENGINE GAP THE TASK BRIEF NAMED: RTXDI's temporal resampling needs a previous frame's primary
// surface (world position + normal) at an arbitrary reprojected pixel, and nothing in Voxi carried
// one before this pair -- the deferred G-buffer, even when on, is current-frame-only.
// AVER_GBUFFER_HISTORY's own gGBufNormalHist a few hundred lines up is that exact gap's own
// unfinished half-attempt (declared, referenced in three denoiser tap loops, gated behind a define
// nothing ever sets to 1) and is left exactly as found: it is a crease term for the SPATIAL shadow/
// reflection filters, tied to the G-buffer being on at all, and reusing its admittedly-guessed slot
// for an unrelated feature that must work whether or not the G-buffer is enabled risks the very
// drift its own comment warns about. This is a NEW pair, added the same way ensureShadowHistory
// already adds one (VoxiRenderer.cpp/.hpp) -- see giSurfPosHist_/giSurfNrmHist_'s own declaration
// there for the full reasoning.
//
// TWO RG32Float TEXTURES, NOT ONE FOUR-CHANNEL FLOAT TEXTURE: rhi::Format has no such format (only
// RGBA16F, R32Float, RG32Float are float formats it declares) -- gGiSurfPosHist carries xy of the
// world position; gGiSurfNrmHist carries z of that SAME position plus the packed normal (an
// octahedral-packed uint from RTXDI_EncodeNormalizedVectorToSnorm2x16, bit-reinterpreted with
// asfloat so one float channel can hold it whole). 0 in the packed-normal channel is the "nothing
// written here" sentinel RAB_GetGBufferSurface tests for.
Texture2D<float2>   gGiSurfPosHist    : register(t12);
RWTexture2D<float2> gGiSurfPosHistOut : register(u7);
Texture2D<float2>   gGiSurfNrmHist    : register(t13);
RWTexture2D<float2> gGiSurfNrmHistOut : register(u8);

// ---- U1's HALF-RESOLUTION VISIBILITY HISTORY (giRestirVisibility == HalfResolution) ----
// A SECOND, HALF-EXTENT pair, at (w+1)/2 x (h+1)/2, next to the full-resolution surface-history pair
// above: one full-resolution pixel of every 2x2 block traces F2/F3 for real each frame
// (giVisTracedPixel, below); the other three reconstruct from this history instead of tracing --
// see 2.10 D of the optimisation-wave-2 plan for the reconstruction and 2.10 E for the write. Packed
// as one RGBA16F pair rather than three separate textures: F2 and F3 are read and written together at
// exactly one pixel of every four, so a single bound pair costs one fewer SRV/UAV slot than the
// alternative for the same information.
//   r = F3 reuse-visibility EMA (this pixel's own reused-sample visibility, exponentially averaged).
//   g = F2 traced-luminance EMA (the second-bounce candidate hit's own indirect estimate, luminance).
//   b = F2 unoccluded-sky-luminance EMA (the same hit's sky term with no visibility test at all --
//       g/b is therefore an occlusion RATIO, not an absolute value, so it rescales correctly against
//       a non-traced pixel's own unoccluded sky rather than replaying another pixel's absolute
//       brightness).
//   a = 1.0 where this frame's phase pixel actually wrote a value, 0.0 everywhere else (a texture
//       freshly created or resized starts at all zeros, which is exactly "never written" -- the same
//       sentinel convention gGiSurfNrmHist's packed-normal channel already uses).
// UNMEASURED size: 2 x ceil(W/2) x ceil(H/2) x 8 B -- see 2.10 F's own memory table for native and
// AverSR-Quality figures.
Texture2D<float4>   gGiVisHist    : register(t16);  // r = F3 reuse visibility EMA, g = F2 traced-lum EMA,
RWTexture2D<float4> gGiVisHistOut : register(u10);  // b = F2 unoccluded-sky-lum EMA, a = 1 written / 0 never

// ---- CSRdGiTrace/CSRdGi CANDIDATE HAND-OFF (Settings::rayDrivenGiSplit) -- ONE FRAME, ONE PIXEL EACH ----
// gRdGiCand ferries giTraceInitialCandidate's full output from CSRdGiTrace's own dispatch (voxi.hlsl,
// which traces the ray) to CSRdGi's AVER_GI_SPLIT=1 compile (which resamples it) -- not a history
// buffer, like gRdGiTex/gRdSunVisTex and the rest of the staged hand-off textures further up voxi.hlsl,
// just a same-frame relay between two passes. Declared here, in voxi_restir.hlsli rather than voxi.hlsl
// alongside its siblings, because this file is #included (voxi.hlsl L419) BEFORE the staged declarations
// (~L450) -- so CSRdGiTrace and CSRdGi, both further down voxi.hlsl, already have it in scope; putting it
// there instead would need a forward declaration this file's own ordering contract (this file's header
// comment, above) already warns against inventing.
// 48 BYTES, STRIDE 48: two float3+scalar pairs pack cleanly into 16-byte lanes each without a manual pad
// field. flags bit 0 = giTraceInitialCandidate's own return value (a candidate exists at all), bit 1 =
// nonFiniteCandidate, bit 2 = f2Observed -- the three bools giTraceInitialCandidate reports alongside its
// six float outputs, packed into the one field with room to spare.
struct RdGiCand { float3 pos; uint flags; float3 nrm; float f2LumTraced; float3 rad; float f2LumSky; };
RWStructuredBuffer<RdGiCand> gRdGiCand : register(u17);
// Set by CSRdGi, per invocation, before its own call to giRestirIndirect -- the row-pitch pixel index
// (idx = pixel.y * pitch + pixel.x) CSRdGiTrace wrote this same candidate under. A `static`, not a
// parameter, for the identical reason gGiPoisonPdfHit and gGiCbSkip already are: giRestirIndirect's
// signature is shared with PSMainVoxi/PSRayDriven's non-split call sites, which must not change.
static uint gGiCandIdx = 0;

// RTXDI's own RAB_Surface contract. `linearDepth` is carried on the struct rather than re-derived by
// RAB_GetSurfaceLinearDepth from a hardcoded view-projection, because the SAME accessor is called on
// BOTH the current pixel's surface (built in giRestirIndirect, current gViewProj) and a previous
// frame's reprojected one (built in RAB_GetGBufferSurface, gPrevViewProj) -- one function cannot
// pick the right matrix for both without being told, and computing it once at construction is
// cheaper than an extra branch at every read.
struct RAB_Surface {
    bool   valid;
    float3 worldPos;
    float3 normal;
    float  linearDepth;
};
RAB_Surface RAB_EmptySurface() {
    RAB_Surface s;
    s.valid = false;
    s.worldPos = 0.0;
    s.normal = float3(0, 0, 1);
    s.linearDepth = 0.0;
    return s;
}
bool   RAB_IsSurfaceValid(RAB_Surface s)   { return s.valid; }
float3 RAB_GetSurfaceWorldPos(RAB_Surface s) { return s.worldPos; }
float3 RAB_GetSurfaceNormal(RAB_Surface s)   { return s.normal; }
float  RAB_GetSurfaceLinearDepth(RAB_Surface s) { return s.linearDepth; }

// RTXDI_GITemporalResampling only ever calls this with previousFrame=true (Rtxdi/GI/
// TemporalResampling.hlsli) -- a SPATIAL pass (out of scope; see this block's own header comment)
// would be the first real caller of the false branch, so it returns "no surface" honestly here
// rather than fabricating one for an ARBITRARY neighbour pixel this file has no cheap way to build.
RAB_Surface RAB_GetGBufferSurface(int2 pixelPosition, bool previousFrame) {
    RAB_Surface s = RAB_EmptySurface();
    if (!previousFrame) return s;
    if (gGiRestirParams.y < 0.5) return s;   // no real previous frame bound at all this session-frame
    uint texW, texH;
    gGiSurfNrmHist.GetDimensions(texW, texH);   // same resolution as gGiSurfPosHist; either would do
    if (pixelPosition.x < 0 || pixelPosition.y < 0 ||
        pixelPosition.x >= (int)texW || pixelPosition.y >= (int)texH) return s;
    // THE NORMAL TEXTURE FIRST, because its sentinel is what decides whether the position texture is
    // even worth reading -- both were written together (giRestirIndirect), so a zero here means the
    // position channel is equally meaningless (a sky miss, or a pixel from before this pair ever
    // held a real frame).
    const float2 nrmRaw = gGiSurfNrmHist.Load(int3(pixelPosition, 0));
    const uint packedN = asuint(nrmRaw.y);
    if (packedN == 0u) return s;
    const float2 posXY = gGiSurfPosHist.Load(int3(pixelPosition, 0));
    s.valid = true;
    s.worldPos = float3(posXY, nrmRaw.x);
    s.normal = RTXDI_DecodeNormalizedVectorFromSnorm2x16(packedN);
    // Re-derived, not stored a third time: gPrevViewProj already exists for the shadow/reflection
    // histories, so this is one free matrix multiply against an exact stored position rather than a
    // requantised depth that could disagree with it.
    s.linearDepth = mul(float4(s.worldPos, 1.0), gPrevViewProj).w;
    return s;
}

// ---- material similarity: A STATED SIMPLIFICATION, not spatial reuse's "is this the same surface" test ----
// That job belongs to RTXDI_IsValidNeighbor's normal/depth comparison, already run before this
// predicate is even asked (Rtxdi/GI/TemporalResampling.hlsli). What THIS adds on top is telling
// apart two geometrically similar but differently-SHADING surfaces -- glass beside concrete at the
// same depth and normal, say -- which matters most for SPATIAL reuse (walking onto a neighbour
// pixel, where crossing a material seam is the common case) and is explicitly out of this slice.
// Temporal reuse only ever compares one screen location against its own reprojection, already
// gated tightly by the depth/normal test above it. A real per-material handle would need
// RtInstance::materialIndex carried through the surface-history pair, which their formats (see
// those textures' own declaration) have no free channel for.
// #define, not typedef -- matching RtxdiTypes.h's own idiom (`#define uint32_t uint`) rather than
// introducing a language feature nothing else in this file's HLSL uses.
#define RAB_MaterialData uint
RAB_MaterialData RAB_GetMaterial(RAB_Surface s) { return 0; }
bool RAB_AreMaterialsSimilar(RAB_MaterialData a, RAB_MaterialData b) { return true; }

// ---- POISON-VIEW INSTRUMENTATION: a thread-private flag RAB_GetGISampleTargetPdfForSurface sets ----
//
// `static`, NOT groupshared or a resource: in HLSL this is per-invocation storage, fresh for every
// pixel-shader thread (identical in effect to a local variable, just reachable from a helper function
// without changing that helper's signature) -- it does NOT persist across separate invocations of the
// entry point the way a C++ function-local static would. Needed here specifically because
// RAB_GetGISampleTargetPdfForSurface's signature is RTXDI's own RAB_ contract (this vendored SDK calls
// it directly with exactly these three arguments), so it cannot grow an `out` parameter the way
// giTraceInitialCandidate above could. giRestirIndirect resets this to false at the top of its own
// invocation, before any call into RTXDI's resampling reaches this function, then reads it back once
// at the end to paint the poison-view colour -- see giRestirIndirect's own comment on that.
static bool gGiPoisonPdfHit = false;

// Luminance of the candidate's own radiance -- the standard resampling target-PDF proxy (RTXDI
// resamples by comparing this scalar across candidates) -- times the cosine at the RECEIVING
// surface. averShadowLum already exists in this file for exactly the luminance reduction; reused
// rather than a second formula. RIS is unbiased for ANY positive target pdf p^: the resampling math
// (RTXDI_FinalizeGIResampling) divides the same p^ back out when it turns weightSum into a
// normalised weight, so multiplying by cosR here changes no expected value. What it changes is
// VARIANCE, which is the entire reason resampling exists -- the closer p^ tracks the true integrand,
// the more often the one surviving reservoir slot holds the candidate that actually contributes.
// Luminance alone rated a bright, near-grazing candidate (one giRestirIndirect's own cosine term at
// the FINAL estimator will go on to multiply toward zero anyway) as highly as the same brightness
// arriving head-on, so a grazing outlier could win the single slot over the sample that mattered.
// The BRDF's albedo/PI is a constant across every candidate ON ONE SURFACE (same pixel, same
// material) so it cannot change which candidate wins and is correctly left out of a target pdf; the
// cosine is NOT constant across candidates -- each arrives from wherever its own traced ray happened
// to land -- so it belongs here, in the resampling weight, and not only in the final estimator.
float RAB_GetGISampleTargetPdfForSurface(float3 samplePosition, float3 sampleRadiance, RAB_Surface surface) {
    // ---- NO SURFACE, NO TARGET PDF, and this guard is what makes the cosine above SAFE ----
    //
    // THE BUG IT CLOSES, because it is not obvious and it cost a day. Until the cosine was added
    // this function IGNORED `surface` entirely -- it was luminance alone -- so it did not matter in
    // the slightest what was passed in. Making it surface-dependent silently made it sensitive to
    // an argument that is ROUTINELY INVALID, and nothing warned about that.
    //
    // Where the invalid surfaces come from: RTXDI's bias-correction pass evaluates the SELECTED
    // sample's pdf at each NEIGHBOUR's surface (Rtxdi/GI/SpatioTemporalResampling.hlsli, the second
    // loop's `ps`), and those neighbours come from RAB_GetGBufferSurface(idx, true). That legitimately
    // answers "no surface" -- a pixel that was sky last frame, one outside the previous viewport, or
    // any pixel at all on the first frame after the history pair is (re)created. Worse,
    // enableFallbackSampling is 1, and the fallback tap deliberately SKIPS RTXDI_IsValidNeighbor, so
    // an invalid neighbour reaches this function by design rather than by accident.
    //
    // What that produced: RAB_EmptySurface() reports worldPos (0,0,0) and normal (0,0,1), so the
    // cosine was computed from the sample's ABSOLUTE world position against the Z axis -- for Sponza
    // geometry metres above the origin that is close to 1, where a real receiving surface would have
    // given perhaps 0.3. An inflated `ps` inflates piSum, and piSum is the DENOMINATOR of
    // RTXDI_FinalizeGIResampling, so every reused sample came back systematically UNDER-weighted.
    // At rest that compounds through the stored weight frame after frame and the indirect term sinks
    // toward black; move the camera and the history is rejected, the estimate falls back to the
    // single fresh candidate, and it snaps bright again. "Visible when I move, otherwise black" is
    // that bias seen from both ends -- and it is ReSTIR-only, because nothing else consults a
    // previous-frame surface this way.
    //
    // Zero is the honest answer and the one RIS wants: a stream whose surface does not exist
    // contributes no candidate and must contribute no normalisation weight either.
    if (!RAB_IsSurfaceValid(surface)) return 0.0;

    // Recomputed per candidate, not per pixel: samplePosition is wherever THIS candidate's ray
    // landed, independent of every other candidate sharing this surface, so it cannot be hoisted out
    // as a per-surface constant the way the BRDF's albedo/PI term above can.
    //
    // ---- THE SAME COSINE FLOOR AS AVER_GI_MIN_COS, AND FOR THE SAME REASON, APPLIED HERE TOO ----
    //
    // MEASURED to be A REAL SOURCE of the --firefly-metric spike this file's other comments describe
    // (giTraceInitialCandidate's, and giRestirIndirect's own -- see that function for the full
    // measurement history, including three backstops tried AFTER this floor and none of them keeping):
    // adding this floor alone, before any backstop, measurably reduced how often the spike fired on
    // the --cam-wobble repro -- several previously-spiking sampled frames read clean afterward -- but
    // did not eliminate a residual. giRestirIndirect's own comment lays out why that residual survives
    // three different backstops and what it is instead (irreducible per-pixel Monte Carlo variance,
    // not a further instance of this same bug); this floor is kept because it IS a measured
    // improvement on its own terms, stated honestly as partial rather than complete.
    //
    // THE MECHANISM: this return value is `p^`, the target pdf RIS resamples by, and per this
    // function's own header comment RIS is unbiased for ANY positive p^ -- but `weightSum` after
    // RTXDI_FinalizeGIResampling divides by it (SpatioTemporalResampling.hlsli:
    // `weightSum * normalizationNumerator / (selectedTargetPdf * piSum)`), so a p^ allowed to approach
    // zero lets weightSum approach infinity for a SAMPLE THAT IS NOT ITSELF ANY BRIGHTER -- the same
    // 1/cosTheta blow-up AVER_GI_MIN_COS already bounds at the sample's OWN creation, unbounded again
    // here at every surface that later RESAMPLES it, temporal self and spatial neighbours alike. A
    // near-grazing angle between a stored sample and whichever surface is now evaluating it is common
    // under camera rotation (the geometry keeps shifting relative to a position that does not), so this
    // is not a rare edge case -- it is what --cam-wobble exercises every pass through its cycle.
    //
    // NOT BEHIND THE SURFACE, THOUGH: the floor only replaces a SMALL POSITIVE cosine (a grazing but
    // genuine view of the sample), never a negative one (the sample is on the wrong side of the
    // surface's hemisphere and has no business contributing at all) -- computed on the UNCLAMPED dot
    // product, not the old saturate()'d one, so that distinction survives.
    //
    // WHY A FLOOR AND NOT A REJECT, UNLIKE AVER_GI_MIN_COS'S OWN CHOICE: that site rejects rather than
    // clamps because clamping there would understate ONE CANDIDATE's own weight and bias the whole
    // estimator dark (its own comment explains why). This function computes an IMPORTANCE PROXY, not
    // the physical integrand -- giRestirIndirect's own final estimate recomputes cosR separately and
    // unfloored for the actual shading math -- so raising the floor here only changes which candidate
    // RIS is more likely to keep, not what any kept candidate is worth. RIS stays unbiased under any
    // positive, self-consistent p^ function (again, this function's own header comment), so this is a
    // variance fix, not a second correctness trade to weigh against the first.
    const float3 toSample = samplePosition - surface.worldPos;
    const float  dist2    = dot(toSample, toSample);
    const float  rawCos   = dist2 > 1e-8 ? dot(toSample * rsqrt(dist2), surface.normal) : -1.0;
    const float  cosR     = rawCos > 0.0 ? max(rawCos, AVER_GI_MIN_COS) : 0.0;
    // ---- DEFENSE IN DEPTH: mirrors RAB_ValidateGISampleWithJacobian's own idiom two functions below
    // it (isnan/isinf/non-positive -> reject) ----
    // Given Fix #1 above (giTraceInitialCandidate's NaN-safe clamps), sampleRadiance arriving here
    // should already be finite, so this should never fire -- included because every other
    // radiance/weight sink in this file already has exactly this shape, and it is a one-line, zero-
    // cost addition against a case not 100% ruled out (a NaN surviving `cosR`, or a future caller of
    // this function that does not route through giTraceInitialCandidate's own guards).
    const float pdf = averShadowLum(sampleRadiance) * cosR;
    if (isnan(pdf) || isinf(pdf)) { gGiPoisonPdfHit = true; return 0.0; }
    return pdf;
}

// ---- CLOSE THE JACOBIAN ASYMMETRY: keep rejecting on the real value, but hand back 1.0 ----
//
// THE BUG THIS CLOSES. RTXDI's BASIC bias correction (SpatioTemporalResampling.hlsli) combines each
// stream's contribution into the numerator as `targetPdf * jacobian * ...` (around :183) but seeds
// the MIS denominator from `ps * neighborReservoir.M`, with NO jacobian term at all (around :267).
// That is one-sided: any tap whose reprojection geometry differs from the current pixel's inflates
// the numerator with nothing in the denominator to cancel it. It is exactly the taps whose geometry
// SHOULD differ under camera rotation that supply the inflated jacobians -- the +-1-2px jittered tap
// and the +-32px spatial taps (SpatioTemporalResampling.hlsli :114-122, :222-230), all six-of-seven
// candidates that are not the i==0 temporal anchor. That anchor's own Jacobian stays close to 1
// under a pure rotation (its receiver does not move), so it was never the source of this; the other
// six are, every one of them re-tapping a screen location that never repeats frame to frame while
// the camera turns.
//
// THE FIX IS THE RESTIR-DI FORM, not a smaller clamp. This function's one documented job stays --
// reject on the REAL jacobian, since RTXDI's own comment calls the value "valuable information to
// determine if the GI sample should be combined", and a NaN/Inf/non-positive value (a grazing
// reprojection whose partial terms divide by near-zero) must still be refused outright rather than
// multiply a reused sample's weight into a bright outlier this codebase already has a name for (see
// aver-negative-radiance-reads-bright's own history elsewhere in this tree). What changes is what an
// ACCEPTED tap reports: 1.0, not the measured ratio. A jacobian of 1.0 multiplies the numerator by
// 1.0 -- the same effect the denominator's missing term already has by omission -- so once a tap has
// passed the reject test, neither sum carries a jacobian and the two sides agree again.
//
// [1/4, 4], NOT [1/25, 25] -- REASONED HERE, NOT MEASURED (no ground-truth sweep was run to fit
// this number; it is a judgement call, stated as one, and a narrower or wider window was not tried
// against the luma-sweep measurement below). With acceptance now geometry-blind, the window's only
// remaining job is deciding "is this still plausibly the same surface patch", not "how far may I
// trust the weight to scale", so it can be tighter than a bound picked for the old numerator-only
// use. A jacobian is a ratio of solid angles/areas between the current and the reused receiver;
// +-2 stops (4x either way) is generous enough to admit the near-1 anchor tap and a genuinely
// similar spatial neighbour at rest, while rejecting the kind of extreme foreshortening a 32px tap
// can produce across a real depth discontinuity -- RTXDI_IsValidNeighbor's own depth/normal test is
// already the first line of defence against that case; this is the second, narrower one.
bool RAB_ValidateGISampleWithJacobian(inout float jacobian) {
    if (isnan(jacobian) || isinf(jacobian) || jacobian <= 0.0) return false;
    if (jacobian < 0.25 || jacobian > 4.0) return false;
    // MEASURED, NOT ARGUED, 2026-09-19: handing back the real ratio instead was captured against
    // this same scene and changed the camera-motion overshoot not at all (+8.0% against +8.2%), so
    // the reasoning above stands and the dial that tested it is gone again.
    jacobian = 1.0;
    return true;
}

// Used only when the temporal resampling's own bias-correction mode is RAY_TRACED -- this file
// selects BASIC above, so this is not on today's hot path, but RAB_ requires the symbol and a
// future change to that one #define should not have to come back and write this too. One shadow-
// style ray between the CURRENT surface and the REUSED sample's position: RTXDI's own docs call
// this the approximate check bias correction needs, not a converged one.
bool RAB_GetTemporalConservativeVisibility(RAB_Surface currentSurface, RAB_Surface temporalSurface,
                                           float3 samplePosition) {
    const float3 toSample = samplePosition - currentSurface.worldPos;
    const float dist = length(toSample);
    if (dist < 1e-4) return true;
    const float3 dir = toSample / dist;
    RayDesc r;
    const float bias = max(gRtParams.z, 1e-4);
    r.Origin    = currentSurface.worldPos + currentSurface.normal * bias;
    r.Direction = dir;
    r.TMin      = bias;
    r.TMax      = max(dist - bias, bias);
    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE_ALL, r);
    averRtProceedSolid(q);
    return q.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

// ---- the neighbour-offset table RTXDI_CalculateSpatialResamplingOffset indexes ----
// NVIDIA's own samples bind this as an 8192-entry R8G8_SNORM Buffer<float2> that
// rtxdi::FillNeighborOffsetBuffer (third_party/rtxdi/Source/RtxdiUtils.cpp) fills once at startup.
// Voxi's descriptor table is a FIXED kVoxiSrvCount/kVoxiUavCount pair guarded by its own
// static_assert -- adding an 8192-entry bound buffer for one feature is not how this file's
// descriptor budget works, and RTXDI_CalculateSpatialResamplingOffset masks its index by
// rParams.neighborOffsetMask before ever touching the table anyway, so a small constant array with
// a matching small mask is sufficient: 32 taps, mask 31, comfortably covers the 8 spatial samples
// plus temporal-jitter offsets actually drawn per pixel below.
//
// THESE ARE NVIDIA'S OWN VALUES, not a made-up substitute for them: generated by running
// FillNeighborOffsetBuffer's exact algorithm (an R=250-scaled low-discrepancy spiral, rejected to
// the unit disc) for the first 32 entries and decoding the resulting int8 pair the same way an
// R8G8_SNORM buffer VIEW would at sample time -- v / 127 -- rather than the raw disc coordinates.
static const float2 kGiNeighborOffsets[32] = {
    float2(-0.48031,-0.84252), float2(+0.51969,-0.56693), float2(+0.03150,+0.54331), float2(-0.44094,-0.29134),
    float2(+0.55906,-0.01575), float2(+0.07087,-0.86614), float2(-0.40157,+0.25197), float2(+0.59055,+0.52756),
    float2(+0.11024,-0.31496), float2(-0.36220,+0.79528), float2(-0.84252,-0.03937), float2(+0.14961,+0.22835),
    float2(-0.32283,-0.61417), float2(-0.81102,+0.50394), float2(+0.66929,-0.33858), float2(+0.18898,+0.77953),
    float2(-0.28346,-0.06299), float2(+0.70866,+0.20472), float2(+0.22835,-0.62992), float2(-0.25197,+0.48031),
    float2(-0.73228,-0.36220), float2(+0.26772,-0.08661), float2(-0.21260,-0.92913), float2(-0.69291,+0.18110),
    float2(+0.30709,+0.45669), float2(-0.17323,-0.37795), float2(+0.82677,-0.10236), float2(-0.13386,+0.16535),
    float2(-0.61417,-0.67717), float2(+0.86614,+0.43307), float2(+0.37795,-0.40157), float2(-0.09449,+0.70866),
};
#define RTXDI_NEIGHBOR_OFFSETS_BUFFER kGiNeighborOffsets
static const uint kGiNeighborOffsetMask = 31u;

// ---- U1's HALF-RESOLUTION RECONSTRUCTION TUNABLES (2.10 D) ----
// ALL FIVE ARE UNMEASURED: no sweep was run to fit any of them against a ground truth, the same
// honesty this file's other reasoned-not-measured constants (the motion-discount 32px knee, the
// [1/4,4] Jacobian window) already carry. Mirrored byte-for-byte in
// aver/voxi/GiVisibility.hpp's own kHistWeight/kRhoMax/kNormalPow/kPlaneTolRel/kPlaneTolCm --
// GiVisibilityTest checks the two never drift apart.
//
// HIST_WEIGHT: the EMA weight the half-res write (2.10 E) blends toward at rest, lerped down to 0.5
// under motion the same way rtShadowTemporal's own 0.9/0.5 pair is -- see that function's history
// write for the shape this borrows. 0.8 means ~5 frames of effective averaging at rest (1/(1-0.8)).
#define AVER_GI_VIS_HIST_WEIGHT 0.8
// RHO_MAX: the ceiling on F2's reconstructed sky ratio (rho2 = g/b, an occlusion ratio that is in
// [0,1] in expectation but not bounded pixel-to-pixel under EMA lag) -- keeps a stale, still-warming
// history from scaling a non-traced pixel's unoccluded sky term up rather than down.
#define AVER_GI_VIS_RHO_MAX 4.0
// NORMAL_POW: the exponent on saturate(dot(neighbourNormal, N)) in the reconstruction weight --
// higher than a plain cosine so a neighbour whose previous-frame normal only loosely agrees with
// this receiver's contributes little, without the hard cutoff a clamp would impose.
#define AVER_GI_VIS_NORMAL_POW 8.0
// PLANE_TOL_REL / PLANE_TOL_CM: the two-term (relative-to-depth plus a flat floor) tolerance on
// |dot(neighbourWorldPos - wpos, N)| a reconstruction tap must fall inside -- the same shape a
// depth-based plane test usually takes (a fixed floor for near geometry, a fraction of distance for
// far geometry, so neither term alone has to cover both regimes).
#define AVER_GI_VIS_PLANE_TOL_REL 0.02
#define AVER_GI_VIS_PLANE_TOL_CM 1.0

// ---- the one RAB_ callback TemporalResampling.hlsli never needed but the spatial half does ----
// SpatioTemporalResampling.hlsli's spatial samples (via Rtxdi/GI/SpatialResampling.hlsli) walk off
// the reprojected pixel by up to samplingRadius and have to be pulled back on-screen before anything
// reads them; RTXDI names that operation RAB_ClampSamplePositionIntoView and calls it itself rather
// than clamping inline, exactly the way it calls RAB_GetGBufferSurface instead of reading a texture
// directly. gSceneViewport, NOT gSceneViewportCur: every call on this whole path passes
// previousFrame = true (see the include swap's own comment below for why), and this file's standing
// convention wherever gPrevViewProj is used -- rtReprojectHistory, giRestirIndirect's own
// screenSpaceMotion further down -- is that the PREVIOUS frame's rect is gSceneViewport. Clamping to
// the VIEWPORT rather than the history TEXTURE matters at the frame edge: the texture is the whole
// backbuffer, and the pixels outside the 3D view are editor chrome with no surface behind them for a
// clamped sample to land on.
int2 RAB_ClampSamplePositionIntoView(int2 pixelPosition, bool previousFrame) {
    const int2 lo = int2(gSceneViewport.xy);
    const int2 hi = lo + int2(gSceneViewport.zw) - 1;
    return clamp(pixelPosition, lo, max(hi, lo));
}

// ---- AND ONLY NOW THE RESAMPLER ITSELF ----
// HLSL HAS NO FORWARD DECLARATIONS THE WAY C++ DOES, so every RAB_ symbol named anywhere in
// SpatioTemporalResampling.hlsli's own include chain (Reservoir, SpatialResampling and
// TemporalResampling -- that header's first three includes, so nothing from the temporal-only
// version above is lost) -- the RAB_Surface TYPE most of all, plus RAB_ClampSamplePositionIntoView
// just above, which only the spatial half calls -- has to be a complete declaration by the time this
// line is parsed. It was above the whole block before, and the cost of that was not "ReSTIR does not
// work": voxi.hlsl is ONE translation unit holding every entry point, so `unknown type name
// 'RAB_Surface'` failed PSMainVoxi, PSRayDriven, VSky and VSMain alike and took the entire
// ray-tracing feature set down with it, at giMode 0 as much as 1. A runtime HLSL error is not a
// build error here -- nothing catches this but a launch.
#include "Rtxdi/GI/SpatioTemporalResampling.hlsli"

// The reservoir buffer's own addressing parameters, derived from gGiSurfNrmHist's ACTUAL
// dimensions rather than a cbuffer field -- MIRRORS rtxdi::CalculateReservoirBufferParameters
// (third_party/rtxdi/Source/RtxdiUtils.cpp) exactly (RTXDI_RESERVOIR_BLOCK_SIZE-pixel blocks), and
// the C++ side (VoxiRenderer.cpp's giReservoirElemCount) computes the identical formula from the
// SAME width/height it created these textures at -- one source of truth for the resolution both
// sides derive pitch from, rather than a second cbuffer value that could drift from the texture it
// describes.
RTXDI_ReservoirBufferParameters giReservoirBufferParams() {
    uint w, h;
    gGiSurfNrmHist.GetDimensions(w, h);   // same resolution as gGiSurfPosHist; either would do
    const uint blocksX = (w + RTXDI_RESERVOIR_BLOCK_SIZE - 1) / RTXDI_RESERVOIR_BLOCK_SIZE;
    const uint blocksY = (h + RTXDI_RESERVOIR_BLOCK_SIZE - 1) / RTXDI_RESERVOIR_BLOCK_SIZE;
    RTXDI_ReservoirBufferParameters p;
    p.reservoirBlockRowPitch = blocksX * (RTXDI_RESERVOIR_BLOCK_SIZE * RTXDI_RESERVOIR_BLOCK_SIZE);
    p.reservoirArrayPitch = p.reservoirBlockRowPitch * blocksY;
    p.pad1 = p.pad2 = 0;
    return p;
}

// ---- U1's HALF-RESOLUTION RECONSTRUCTION (giRestirVisibility == HalfResolution, 2.10 D) ----
// PLACED HERE, AFTER RAB_GetGBufferSurface (above) AND BEFORE giTraceInitialCandidate (below):
// giVisReconstruct calls RAB_GetGBufferSurface itself (reusing its bounds check and its t12/t13
// read rather than a third copy of that lookup), so it must be declared after it; it is called from
// giRestirIndirect's own decode block, before giTraceInitialCandidate is ever invoked, so it also
// has to exist before that call site.
//
// kGiVisPhase: which of a 2x2 block's four full-resolution pixels traces F2/F3 for real on a given
// frame, cycling every 4 frames so every pixel gets its own turn -- the SAME table read from both
// ends: giVisTracedPixel below decides THIS frame's traced pixel from it directly, and
// giVisReconstruct (further below) reads the PREVIOUS frame's entry (frameIdx - 1) to know which
// full-resolution pixel actually wrote the half-res texel it is about to sample.
static const uint2 kGiVisPhase[4] = { uint2(0, 0), uint2(1, 1), uint2(1, 0), uint2(0, 1) };

// Is pixel `p` this 2x2 block's traced pixel on frame `frame`? `& 3u`, not `% 4u`, matching this
// file's own bitmask convention (kGiNeighborOffsetMask, tileMask/turnMask in voxi_rt.hlsli) -- 4 is
// a compile-time power of two, so the two are identical, and the mask form is what the rest of this
// codebase already writes at every other cyclic-schedule site.
bool giVisTracedPixel(uint2 p, uint frame) {
    const uint2 ph = kGiVisPhase[frame & 3u];
    return (p.x & 1u) == ph.x && (p.y & 1u) == ph.y;
}

// One reconstructed visibility sample: `valid` says whether ANY tap below survived every rejection
// test, `v3`/`g`/`b` mirror gGiVisHist's own r/g/b channels (F3 reuse-visibility EMA, F2
// traced-luminance EMA, F2 unoccluded-sky-luminance EMA), and `motionPx` is this pixel's own
// reprojection distance -- computed once here and reused by the half-res history write's own motion
// knee (2.10 E) so that write does not repeat this function's reprojection arithmetic a second time.
struct GiVisRecon { bool valid; float v3; float g; float b; float motionPx; };

// Reconstructs this pixel's F2/F3 answer from its non-traced neighbours' own half-resolution
// history, depth/normal-weighted the same way a spatial denoiser upsamples -- see 2.10 D's own "why
// this upsampling is sound" for the argument (a previous-frame surface that matches this receiver's
// plane and normal draws candidates from the same hemisphere distribution, so the expected F2/F3
// answer is smooth in (position, normal) and weighting by both preserves that).
GiVisRecon giVisReconstruct(float3 wpos, float3 N, float2 pixel, uint frameIdx) {
    GiVisRecon rec = (GiVisRecon)0;   // valid = false, everything else 0 -- the honest "no reconstruction" answer

    // ---- 1. THE VALIDITY CHECK COMES FIRST, BEFORE ANY t12/t13/t16 READ (checklist item 10) ----
    // gGiRestirParams.y is "does a real previous frame's surface-history pair (t12/t13) exist yet"
    // -- the identical field RAB_GetGBufferSurface itself tests first, so this can never disagree
    // with what that function is about to do. gAmbientParams.w bit 8 is the sibling test for t16:
    // "does gGiVisHist hold a real previous frame", set only once beginShadowHistory has actually
    // written it once (VoxiRenderer::endShadowHistory). Either false means every tap below would
    // read stale or newly-allocated storage with nothing meaningful in it, so this returns invalid
    // before touching any of the three textures a tap would otherwise sample.
    if (((uint)gAmbientParams.w & 8u) == 0u || gGiRestirParams.y < 0.5) return rec;

    // ---- 2. THE SAME REPROJECTION RECIPE giRestirIndirect's OWN screenSpaceMotion USES, further
    // down this file -- gPrevViewProj paired with gSceneViewport (never gSceneViewportCur), this
    // file's own standing convention wherever a PREVIOUS frame's rect is needed (see
    // RAB_ClampSamplePositionIntoView's own comment on that convention). Reused rather than
    // re-derived a second way, so the two reprojections can never disagree about where this pixel
    // was last frame.
    const float4 prevClip = mul(float4(wpos, 1.0), gPrevViewProj);
    if (prevClip.w <= 1e-4) return rec;
    const float3 prevNdc = prevClip.xyz / prevClip.w;
    const float2 prevPx = gSceneViewport.xy +
        float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * gSceneViewport.zw;
    rec.motionPx = length(prevPx - pixel);

    // ---- 3. BILINEAR TAPS INTO THE HALF-RESOLUTION HISTORY ----
    // prevPx is a FULL-resolution coordinate; halving it lands in gGiVisHist's own texel space, and
    // the -0.5 recentres onto the half-res grid the way an ordinary bilinear filter's texel-corner
    // convention does.
    const float2 hp   = prevPx * 0.5 - 0.5;
    const float2 base = floor(hp);
    const float2 f    = hp - base;
    const float  wgt[4] = { (1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y, f.x * f.y };
    const int2   taps[4] = { int2(base), int2(base) + int2(1, 0), int2(base) + int2(0, 1),
                             int2(base) + int2(1, 1) };

    uint visW, visH;
    gGiVisHist.GetDimensions(visW, visH);

    float wsum = 0.0, rsum = 0.0, gsum = 0.0, bsum = 0.0;
    [unroll] for (uint k = 0; k < 4; ++k) {
        const int2 t = taps[k];
        // 4a. BOUNDS-CHECK AGAINST gGiVisHist's OWN DIMENSIONS, not gGiSurfNrmHist's -- this pair is
        // sized (w+1)/2 x (h+1)/2, half the full-resolution surface history, so this tap's own
        // texture is the edge that matters here.
        if (t.x < 0 || t.y < 0 || t.x >= (int)visW || t.y >= (int)visH) continue;
        if (wgt[k] <= 0.0) continue;   // a bilinear corner exactly on a texel needs no neighbour tap

        // 4b. THE WRITER'S FULL-RESOLUTION PIXEL, from LAST frame's phase table (frameIdx - 1u, not
        // frameIdx): this reconstruction runs THIS frame against a history the PREVIOUS frame wrote,
        // and giVisTracedPixel's own phase table is what decided which of the four full-resolution
        // pixels inside t's 2x2 block actually wrote it. Unsigned wrap at frameIdx 0 is deliberate
        // and matches HLSL's own uint arithmetic (checked by GiVisibilityTest).
        const uint2 writer = uint2(t) * 2u + kGiVisPhase[(frameIdx - 1u) & 3u];
        // RAB_GetGBufferSurface ALREADY BOUNDS-CHECKS AND READS t13 THEN t12 -- reused rather than a
        // third copy of that lookup; its own "no real previous frame" test reads the same
        // gGiRestirParams.y field already checked in step 1, so the two can never disagree.
        const RAB_Surface ps = RAB_GetGBufferSurface(int2(writer), true);
        if (!RAB_IsSurfaceValid(ps)) continue;

        const float4 vh = gGiVisHist.Load(int3(t, 0));
        if (vh.a < 0.5) continue;   // never written this texel -- the sentinel the half-res write (2.10 E) sets

        // 4c. THE PLANE TEST: a two-term tolerance (a flat floor plus a fraction of distance) against
        // the SAME worldPos/linearDepth RAB_GetGBufferSurface just reconstructed -- reused rather
        // than a fourth copy of "is this plausibly the same surface".
        const float planeTol = AVER_GI_VIS_PLANE_TOL_CM + AVER_GI_VIS_PLANE_TOL_REL * ps.linearDepth;
        if (abs(dot(ps.worldPos - wpos, N)) > planeTol) continue;

        const float nDot = saturate(dot(ps.normal, N));
        const float w = wgt[k] * pow(nDot, AVER_GI_VIS_NORMAL_POW);
        wsum += w; rsum += w * vh.r; gsum += w * vh.g; bsum += w * vh.b;
    }

    // ---- 5. VALID ONLY PAST A NOISE FLOOR ----
    // Not merely "at least one tap survived": a single near-grazing bilinear corner (a tiny wgt[k])
    // would otherwise hand back a near-zero-confidence reconstruction as if it were as trustworthy as
    // four agreeing taps. 1e-3 is a floor on the SUM of up to four weights each in [0,1] -- generous
    // enough to admit one dim but real tap, tight enough to reject pure numerical noise.
    rec.valid = wsum > 1e-3;
    if (rec.valid) {
        rec.v3 = rsum / wsum;
        rec.g  = gsum / wsum;
        rec.b  = bsum / wsum;
    }
    return rec;
}

// ---- the initial candidate: ONE cosine ray, traced and shaded through machinery this file already has ----
// Traces off (wpos, N) along a cosine-weighted hemisphere direction and shades whatever it hits
// through the SAME RayQuery + flat geometry table + gRtMaterials + averShadeDirect machinery
// rtReflection and PSRayDriven already use for a mirror ray and a primary ray respectively -- this
// is that toolkit pointed along a diffuse-importance direction instead of a specular one.
//
// Returns false on a miss: nothing a reservoir can reproject. The caller then hands RTXDI an EMPTY
// initial reservoir for this frame and lets temporal resampling (RTXDI_IsValidGIReservoir gates on
// M != 0) carry the pixel from history alone -- exactly the situation that function already handles.
// U1's F2 PATH SELECTOR AND ITS THREE OUTPUTS (2.10 A/B): `f2Path` is the decode block's own verdict
// (giRestirIndirect, 3 trace / 2 half-res ratio / 1 reconstructed / 0 legacy-no-ray), `rho2` is the
// half-res reconstruction's own occlusion ratio (only meaningful when f2Path == 2u), and the three
// `out` parameters carry back what F2's TRACED path (f2Path == 3u) observed so the half-res history
// write further down this file (2.10 E) has something to store -- set only on that one path, and
// default-initialised to "nothing observed" everywhere else, the same convention nonFiniteCandidate
// already uses.
bool giTraceInitialCandidate(float3 wpos, float3 N, float2 pixel, float frameJitter,
                             out float3 samplePos, out float3 sampleNormal, out float3 sampleRadiance,
                             out bool nonFiniteCandidate, uint f2Path, float rho2,
                             out float f2LumTraced, out float f2LumSky, out bool f2Observed) {
    samplePos = sampleNormal = sampleRadiance = 0.0;
    // Set on every path, including the early `return false` below: this is an `out` parameter and
    // HLSL, like C++, requires it be written on every exit -- and false is the correct default,
    // since neither early-out here has computed a radiance yet for a guard to have caught anything.
    nonFiniteCandidate = false;
    // Same convention, same reason, for U1's three new outputs: false/0/0 is "F2's traced path did
    // not run this invocation", true whenever a caller sees it, and it is written FOR REAL only where
    // f2Path == 3u actually traces (below) -- every other exit, early or not, leaves this default
    // standing, which is the honest answer since nothing was observed on those paths.
    f2LumTraced = 0.0; f2LumSky = 0.0; f2Observed = false;

    // Cosine-weighted hemisphere sample (Malley's method: a uniform point on the unit disc, lifted
    // onto the hemisphere) -- the standard importance sample for a Lambertian receiver, so its own
    // pdf (cosTheta/PI) is what the caller's RTXDI_MakeGIReservoir divides out. rtDiscSample/rtHash
    // are the SAME per-pixel-rotated low-discrepancy sequence rtReflection's own cone sample uses,
    // jittered per frame the same way (frameJitter) rather than frozen per pixel -- see rtReflection
    // for what a frozen-per-pixel sample cost the reflection ray before that was fixed.
    // ---- F1 (R0), gAmbientParams.z bit 1: legacy 45-degree ring vs. the cosine hemisphere ----
    // true (bit set) keeps rtDiscSample's fixed ring -- the HEAD expression, unchanged -- for
    // comparison; false (the corrected default) draws rtHemiDiscSample (voxi_rt.hlsli, just after
    // rtDiscSample) instead, which is uniform over frames rather than pinned to cosTheta = 1/sqrt(2).
    // Frame index from gRtHistParams.z, not this function's own frameJitter parameter: frameJitter is
    // already frameIdx*2.39996323 (an angle), and rtHemiDiscSample wants the raw index to build its
    // own nested (frame, k) sequence. streamSalt 0.0 keeps this stream's u apart from F2's second-
    // bounce sample and the sky-occlusion ray's, both of which draw at the same (pixel, frame, k=0).
    const float2 xi = (((uint)gAmbientParams.z & 1u) != 0u) ? rtDiscSample(0, rtHash(pixel) * 6.2831853 + frameJitter) : rtHemiDiscSample(0u, 1u, (uint)gRtHistParams.z, pixel, 0.0);
    const float  cosTheta = sqrt(saturate(1.0 - dot(xi, xi)));
    // ---- THE FLOOR IS A FIREFLY BOUND, AND 1e-4 WAS NOT ONE ----
    //
    // This cosTheta IS the sample pdf (cosTheta/PI), so the reservoir the caller builds from it
    // carries weightSum = PI/cosTheta. At the old 1e-4 that is ~31,400, and the reason that was
    // invisible for so long is that it CANCELS in the single-candidate case: the estimator
    // multiplies by the receiver cosine at the same surface, so est reduces algebraically to plain
    // `radiance` however small cosTheta got.
    //
    // IT STOPS CANCELLING THE MOMENT THE RESERVOIR IS REUSED. Stored unclamped, the same weightSum
    // re-enters RIS next frame against a DIFFERENT receiver -- RTXDI_CombineGIReservoirs weighs it by
    // a target pdf evaluated at that neighbour's surface, whose cosine has nothing to do with the
    // grazing one that produced the weight. Nothing downstream bounds it either: the AVER_VOX_MAXRAD
    // clamps cap sampleRadiance and the final on-screen estimate, and both run AFTER
    // RTXDI_StoreGIReservoir, so the unbounded weight is what neighbours inherit. One pixel that
    // happened to sample near its own tangent plane becomes a bright speck that spreads.
    //
    // 0.05 bounds weightSum at ~63 instead, a 500x cut in the worst case. What it costs is exactly
    // measurable rather than guessed: for a cosine-weighted disc sample P(cosTheta < c) = c^2, so
    // this discards 0.25% of directions, all of them within 3 degrees of the tangent plane where the
    // Lambertian lobe carries least weight. Rejected rather than clamped -- clamping the pdf while
    // keeping the sample would understate its weight and bias the whole estimator dark, which is the
    // mistake this file has already made once.
    if (cosTheta <= AVER_GI_MIN_COS) return false;
    float3 up = abs(N.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 T  = normalize(cross(up, N));
    float3 B  = cross(N, T);
    const float3 dir = normalize(T * xi.x + B * xi.y + N * cosTheta);

    RayDesc r;
    const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
    r.Origin    = wpos + N * bias;
    r.Direction = dir;
    r.TMin      = bias;
    r.TMax      = max(gVoxelParams.z, 1.0);   // giMaxDistance -- the reach every other GI ray honours

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE_ALL, r);
    averRtProceedSolid(q);
    // ---- A MISS IS A SAMPLE OF THE SKY, NOT A FAILED SAMPLE ----
    //
    // This used to `return false`, and that is the single clearest reason the reported darkness is
    // ReSTIR-ONLY: a cosine-weighted ray that escapes carries the sky's radiance, and for a shadowed
    // surface under an open arcade the sky is most of the hemisphere and therefore most of the true
    // indirect term. Switching Indirect diffuse from Voxel cones to ReSTIR dropped that entire
    // contribution on the floor -- exactly the surfaces the user reports going near-black while
    // sunlit ones stay correct.
    //
    // OWNERSHIP, CORRECTED (R1/F4, voxi.hlsl): an earlier version of this comment justified the gap
    // by claiming "coneTracedIndirect gathers it (the volume is injected with sky)" -- read elsewhere
    // as license to add the sky back a second time at the receiver (voxi.hlsl's ind4.ambient/
    // ind.ambient), which is what made giMode 1 count it twice. That claim was false:
    // coneTracedIndirect's own `bounce` (voxi_cone.hlsli) is the volume's RE-EMITTED radiance, a
    // LATER bounce, not this ray's own visible sky. THIS branch, with a traced ray behind it, is now
    // the RECEIVER'S ONE AND ONLY copy of the sky term for giMode 1 -- voxi.hlsl's F4 (gAmbientParams.z
    // bit 2) subtracts its own ambient copy back out precisely so this stays true.
    //
    // IT ALSO COST THE ESTIMATOR ITS ANCHOR. Returning false leaves `initial` empty with M = 0, and
    // the MIS denominator is seeded `selectedTargetPdf * inputReservoir.M` -- so a miss was not
    // counted even as an observation. The fresh candidate is the only term in the whole recursion
    // pinned to a physically-derived 1/pdf; losing it on miss-heavy pixels leaves the estimate
    // free-running on reused weights alone.
    //
    // averSkyRadianceCheap IS THE SIBLING'S OWN FUNCTION, not a new sky model -- rtAmbientTraced
    // shades its own escaping rays with it, so the two ray paths now agree about what the sky is
    // worth. sampleNormal faces back down the ray, and samplePos sits at TMax so the Jacobian sees a
    // real, finite direction rather than a degenerate one.
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        samplePos      = wpos + dir * r.TMax;
        sampleNormal   = -dir;
        // ---- NaN-SAFE, REPLACING A NATIVE clamp() -- see this function's own header note on why ----
        // HLSL's three-argument clamp() has compiler/driver-defined behaviour on a NaN input (unlike
        // this file's own hand-rolled min(max(x,lo),hi) idiom, which floors a NaN to lo via the same
        // "comparison against NaN is false" semantics documented in giRestirIndirect). A temporary is
        // needed here because this branch clamps an EXPRESSION, not a variable already named
        // `radiance` the way the hit branch below does.
        {
            const float3 rawSky = averSkyRadianceCheap(dir) * gAmbient.r;
            const bool   bad    = any(isnan(rawSky)) || any(isinf(rawSky));
            nonFiniteCandidate  = nonFiniteCandidate || bad;
            sampleRadiance      = bad ? float3(0.0, 0.0, 0.0) : min(max(rawSky, 0.0), AVER_VOX_MAXRAD);
        }
        return true;
    }

    RtInstance inst = gRtInstances[q.CommittedInstanceID()];
    uint tri = inst.firstIndex + q.CommittedPrimitiveIndex() * 3;
    uint i0 = inst.firstVertex + gRtIndices[tri + 0];
    uint i1 = inst.firstVertex + gRtIndices[tri + 1];
    uint i2 = inst.firstVertex + gRtIndices[tri + 2];
    float2 bary = q.CommittedTriangleBarycentrics();
    float3 w = float3(1.0 - bary.x - bary.y, bary.x, bary.y);
    float3 nObj = normalize(gRtVerts[i0].nrm * w.x + gRtVerts[i1].nrm * w.y + gRtVerts[i2].nrm * w.z);
    float3 hitN = normalize(mul(float4(nObj, 0.0), inst.objectToWorld).xyz);
    if (dot(hitN, dir) > 0.0) hitN = -hitN;   // face the ray, matching rtReflection/PSRayDriven

    const float3 hitPos = wpos + dir * q.CommittedRayT();
    const RtMaterial mat = gRtMaterials[inst.materialIndex];
    const float3 L = normalize(gLightDir.xyz);

    // ---- THE HIT'S OWN MAPS, NOT ITS FACTORS ALONE ----
    //
    // This used to build the surface from factors only, on the reasoning that a flat-shaded hit
    // converges toward a textured one's low-frequency answer. It does not: a glTF factor is a
    // MULTIPLIER on its map, not an average of it. Sponza authors metallicFactor 1.0 and keeps the
    // real (near-zero) metalness in the metal-rough map's blue channel, so every textured hit read as
    // a pure white METAL -- kdAlbedo 0, so no diffuse bounce and no multi-bounce at all (the
    // `kdAlbedo * indY` below multiplied every indirect term by zero), and a single bounce that was
    // nothing but a white specular sun glint.
    //
    // MEASURED, PTTest NewSponza, sun 85.6 deg, gallery pose, linear HDR means against the converged
    // path tracer at the same bounce depth (--tonemap 0, fog off):
    //   single bounce, factor-only        0.0168 whole   0.00338 inner wall  (PT 1 bounce 0.0095 / 0.0012)
    //   single bounce, base colour only   0.0099         0.00053
    //   single bounce, base + metal-rough 0.0112         0.00096
    // and with the factor-only surface, multi-bounce added 0.0004 to a wall the path tracer lights
    // 85% by multi-bounce (4 bounces 0.0080 against 1 bounce 0.0012).
    //
    // The same maps PSRayDriven's own surface and rtReflection's hit sample, at a footprint rather
    // than mip 0: a diffuse bounce only needs the local average colour, and mip 0 is the throughput
    // trap rtReflection's own comment measures. AVER_GI_HIT_TEX_CONE sets that footprint.
    //
    // WITHOUT AVER_RT_BINDLESS (raster PSMainVoxi's RT variant) no map can be read here, so that
    // variant keeps the factor-only surface and its metal-for-glTF error. Ray-driven is the default
    // and is always bindless.
    AverSurface s = (AverSurface)0;
    s.N           = hitN;
    s.V           = -dir;
    // ---- GUARDED: a genuine 0/0 if the traced ray ever lands exactly antiparallel to the sun ----
    // Cheap risk reduction, not a confirmed trigger (see this file's own header note) -- closes it
    // outright since it feeds directly into the same clamp below. s.N is always finite and a
    // reasonable fallback at this measure-zero case.
    {
        const float3 VL  = s.V + L;
        const float  vl2 = dot(VL, VL);
        s.H = vl2 > 1e-12 ? VL * rsqrt(vl2) : s.N;
    }
    float4 hitMapBase = float4(1, 1, 1, 1);
    float4 hitMapMR   = float4(1, 1, 1, 1);
#ifdef AVER_RT_BINDLESS
    {
        const float2 meshUV = gRtVerts[i0].uv * w.x + gRtVerts[i1].uv * w.y + gRtVerts[i2].uv * w.z;
        const float2 huv    = averRtSurfaceUV(mat, inst, hitPos, hitN, meshUV);
        // The footprint basis is rtReflection's: two in-plane directions at the hit, scaled.
        const float  rad = AVER_GI_HIT_TEX_CONE * q.CommittedRayT();
        const float3 hup = abs(hitN.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
        const float3 ht  = normalize(cross(hup, hitN));
        const float3 hb  = cross(hitN, ht);
        float2 hgx, hgy;
        averRtUvGrad(mat, inst, hitN,
                     gRtVerts[i0].pos, gRtVerts[i1].pos, gRtVerts[i2].pos,
                     gRtVerts[i0].uv,  gRtVerts[i1].uv,  gRtVerts[i2].uv,
                     ht * rad, hb * rad, hgx, hgy);
        hitMapBase = averRtSampleSlot(mat, 0, huv, hgx, hgy, float4(1, 1, 1, 1));
        hitMapMR   = averRtSampleSlot(mat, 1, huv, hgx, hgy, float4(1, 1, 1, 1));
    }
#endif
    // glTF's metal-rough channels, as PSRayDriven reads them: G roughness, B metalness.
    s.albedo      = inst.albedo * mat.baseColorFactor.rgb * hitMapBase.rgb;
    s.metallic    = saturate(inst.metallic * mat.metallicFactor * hitMapMR.b);
    s.rough       = clamp(inst.roughness * mat.roughnessFactor * hitMapMR.g, 0.045, 1.0);
    s.ndv         = saturate(dot(s.N, s.V));
    s.f90         = mat.f90;
    s.reflectance = mat.reflectance;
    s.backFace    = false;
    s.sssWeight   = 0.0;
    s.sssRadius   = 0.0;
#ifdef AVER_LAYERED_BSDF
    // Mirrors PSRayDriven's own hand-built surface (voxi.hlsl:1469-1477): read the hit's own
    // material instead of hardcoding the coat off, which silently meant "no material has a coat in
    // ReSTIR GI's candidate hit" no matter what mat.coat* actually said -- the identical trap that
    // comment names for ray-driven mode's own history, now closed here too.
    // INERT TODAY, and kept anyway: averCoatTerms (material_prelude.hlsl) only ever acts through
    // ind.specular, and the environment-specular term this was paired with has been reverted (see
    // below). Reading the real coat is still strictly better than hardcoding it off -- it means the
    // data is correct on the day a properly scaled specular term lands, instead of a second trap.
    s.coatWeight = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatWeight)    : 0.0;
    s.coatRough  = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatRoughness) : 0.0;
    s.coatF0     = (mat.flags & AVER_MAT_COAT) ? saturate(mat.coatF0)        : 0.0;
#endif
    s.F0        = lerp(mat.reflectance.xxx, s.albedo, s.metallic);
    s.F         = fresnelSchlick(saturate(dot(s.H, s.V)), s.F0, s.f90);
    s.kdAlbedo  = (1.0 - s.metallic) * s.albedo * (1.0 - saturate(mat.transmission));
    s.model     = AVER_MODEL_STANDARD;
    s.alpha     = 1.0;
    s.emissive  = mat.emissiveFactor;
    s.occlusion = 1.0;

    AverLight sun;
    sun.direction = L;
    sun.radiance  = averSunRadiance();
    // ONE FRESH SHADOW RAY, NOT gRtShadowHist's TEMPORAL AMORTISATION: that history is keyed by
    // SCREEN PIXEL, and this sample's origin is a WORLD point with no pixel of its own -- the whole
    // reason a reservoir stores a POSITION -- so reusing it would read another pixel's history
    // under a different surface. RTXDI's own temporal resampling is what amortises THIS ray across
    // frames instead, the same way it amortises the rest of the candidate.
    sun.visibility = rtShadow(hitPos, s.N, L, pixel, float3(0, 0, 0), float3(0, 0, 0), 1u, frameJitter);
    sun.visibility *= 1.0 + averCausticFocus(hitPos);

    // 0.0, NOT s.emissive: averShadeIndirect below adds s.emissive itself (its own header comment,
    // "adds ambient, bounce, environment specular and self-emission, in that order"), matching the
    // averShadeDirect(radiance=0)/averShadeIndirect(radiance) pairing PSRayDriven uses for the same
    // AverSurface (voxi.hlsl:1094-1096, :1775). Passing s.emissive here too would count it twice.
    float3 radiance = averShadeDirect(s.emissive, s, sun);

    // ---- DIFFUSE AMBIENT ONLY. AN ENVIRONMENT-SPECULAR TERM HERE FLATTENS THE WHOLE IMAGE ----
    //
    // MEASURED, by shipping it and looking: adding `ind.specular = averSkyRadianceCheap(reflect(...))`
    // and shading through averShadeIndirect turned Sponza's interior into a uniform grey wash with no
    // bounce gradient left in it. The reason is visible in averIndirectTerms
    // (material_prelude.hlsl): `specEnv = FssEss * ind.specular * specOcc` is NOT multiplied by
    // ind.ambientScale, where `diffAmbient` IS. So a secondary hit received the sky at FULL radiance,
    // unscaled by gAmbient.r and unoccluded, while the diffuse half of the same hit was correctly
    // scaled down. Inside an enclosed space every candidate hit is a wall that cannot see the sky at
    // all, so that term is not a small correction -- it dominates the real bounce and erases the
    // variation the estimator exists to capture.
    //
    // The original code omitted this deliberately and its comment said so; that judgement was right
    // and this comment replaces the one that overrode it.
    //
    // WHAT IT COSTS, STATED RATHER THAN LEFT TO BE REDISCOVERED: a metallic hit has kdAlbedo = 0, so
    // it contributes almost NO bounce light and reads as a black hole for GI. Closing that needs a
    // term weighted by metalness AND scaled/occluded the way the diffuse ambient is -- not an
    // unconditional sky specular on every surface, which is what this reverts. (Until the hit read its
    // metal-rough map, above, EVERY textured glTF hit was such a black hole; genuine metals are now
    // the only ones left.)
    //
    // ---- F2 (R2): THE DIFFUSE HALF NOW OWNS ITS OWN VISIBILITY, WHERE IT USED TO HAVE NONE ----
    //
    // THE GAP THIS CLOSES. averSkyIrradiance(s.N) is a function of the normal ALONE (voxi.hlsl's own
    // comment on it) -- it has no notion of what actually stands in front of this candidate hit, so a
    // second-bounce point under an overhang or inside a corner received the FULL open-sky irradiance
    // exactly as if it stood in the open. See aver-ambient-overbright-open-vs-enclosed for why
    // unoccluded ambient in an interior is already over-bright before anything is added to it -- this
    // was one more unoccluded read of the same sky, one bounce deeper.
    //
    // gAmbientParams.z bit 4 TRUE keeps that old unoccluded read, byte-identical to HEAD, for
    // comparison only. FALSE (the corrected default -- ON, per the user's own call, because this
    // extra ray sits behind the legacy bit rather than a second switch) traces one more cosine-
    // weighted ray from this hit instead: a miss reads the same sky function rtAmbientTraced's own
    // miss branch uses (averSkyRadianceCheap), now genuinely visibility-tested; a hit inside the GI
    // volume reads that voxel's stored exitant radiance directly -- the THIRD bounce, missing
    // altogether before this -- and a hit outside the volume contributes nothing, the same rule
    // rtAmbientTraced's own AVER_AO_UNIFIED branch already applies to the identical case (voxi_rt.hlsli,
    // a few hundred lines up).
    //
    // THE ARITHMETIC: L_o,ind(y) = (kd_y/PI) * Integral(L cos dw). Drawing the second direction from a
    // cosine-weighted hemisphere (rtHemiDiscSample, the same sampler F1 above uses -- streamSalt 0.71
    // keeps this stream's u apart from F1's own 0.0 at the same pixel/frame/k) makes PI and the
    // cosine cancel, so one sample gives kd_y * L(w2) directly -- the same convention PSVoxel's own
    // injection pass writes the volume under. In the open, V_y = 1 and this reduces exactly to the
    // legacy value above. gVoxelParams.y (giIntensity) is deliberately NOT applied on this branch: it
    // is applied exactly once, to the WHOLE estimate, at giRestirIndirect's own `est` -- applying it
    // here too would double it on this one bounce alone. Approximation carried over unchanged from the
    // volume's own injection: the value already contains AVER_VOX_FEEDBACK 3.0. It contains NO SKY
    // while ReSTIR GI runs (PSVoxel's own comment says why): the sky reaches this bounce only through
    // this ray's miss, which is the one place its visibility is actually traced.
    //
    // THE NaN-SAFE CLAMP AT THIS FUNCTION'S OWN END STILL RUNS LAST, after `radiance` (built from indY
    // below, same as before) leaves this block -- F2 changes what feeds that clamp, not the clamp
    // itself.
    // ---- U1 (2.10 B): FOUR PATHS NOW, NOT TWO -- the legacy/corrected split above is joined by
    // Reconstructed (a voxel-cone march standing in for the traced ray) and Half's own non-traced
    // reconstruction (a plain sky-luminance ratio, no ray, no cone). THE LEGACY BRANCH'S CONDITION
    // GAINS ONLY `|| f2Path == 0u` -- its body is UNCHANGED text, because f2Path == 0u already means
    // "legacy bit 4 is set, or giRestirVisibility is No ray" (the decode block folds both into the
    // one path number, 2.10 A), so this branch is correct for either reason without needing to know
    // which. THE TRACE BRANCH'S BODY (the final `else`) IS BYTE-IDENTICAL to what stood here before
    // this task -- only reached at f2Path == 3u now, and the three new `out` params are written at
    // its own end, after `indY` is final, exactly mirroring what a caller who traced this ray would
    // have observed.
    float3 indY = 0.0;
    if (((uint)gAmbientParams.z & 4u) != 0u || f2Path == 0u) {
        indY = averSkyIrradiance(s.N) * gAmbient.r;
    } else if (f2Path == 1u) {
        // ---- RECONSTRUCTED: ONE VOXEL-CONE MARCH STANDS IN FOR THE TRACED RAY ----
        // traceCone's own contract (voxi_cone.hlsli:62; forward-declared in voxi_rt.hlsli for this
        // exact reason -- see that prototype's own comment) returns premultiplied radiance plus
        // coverage. gVoxelParams.w guards on the volume being enabled at all, matching every other
        // conditional voxel read in this file; with it off the legacy unoccluded sky is the only
        // thing left to fall back to.
        //
        // THE CONE'S RADIANCE ONLY -- NO `sky * (1 - cone.a)` TERM. This used to read 1 - a as "this
        // direction is open to the sky", as PSVoxel does, and it is not: the volume holds one-voxel
        // SHELLS of surfaces, and a 60-degree cone a few metres out samples mip 5+, where a shell
        // covers ~2% of a cell. The cone sees through a roof. MEASURED at PTTest's gallery (sun 85.6
        // deg, linear means, once the hit's maps were read so the term was no longer multiplied by
        // zero): keeping it put the whole frame at 0.098 against the path tracer's 0.0149 -- 6.6x.
        // So Reconstructed has no sky at a candidate hit, and interiors lit through openings read
        // darker than the path tracer (inner wall 0.0015 against 0.0080); Half and Full trace that
        // sky and do not have this gap.
        if (gVoxelParams.w > 0.5) {
            const float4 cone = traceCone(hitPos, s.N, AVER_VOX_INJECT_APERTURE);
            indY = min(cone.rgb, AVER_VOX_MAXRAD);
        } else indY = averSkyIrradiance(s.N) * gAmbient.r;
    } else if (f2Path == 2u) {
        // ---- HALF, NON-TRACED PIXEL WITH A VALID RECONSTRUCTION: NO RAY, NO CONE, ONE RATIO ----
        // rho2 (computed at the call site, giRestirIndirect) is the neighbourhood's own occluded-
        // over-unoccluded sky luminance ratio -- clamped at AVER_GI_VIS_RHO_MAX (2.10's own comment
        // on that constant) since it is an EMA-lagged ratio, not a value bounded to [0,1] on any
        // single frame the way a fresh trace's V would be.
        indY = averSkyIrradiance(s.N) * gAmbient.r * clamp(rho2, 0.0, AVER_GI_VIS_RHO_MAX);
    } else {
        const float2 xi2 = rtHemiDiscSample(0u, 1u, (uint)gRtHistParams.z, pixel, 0.71);
        const float  c2  = sqrt(saturate(1.0 - dot(xi2, xi2)));
        const float3 up2 = abs(s.N.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
        const float3 T2 = normalize(cross(up2, s.N)); const float3 B2 = cross(s.N, T2);
        const float3 dir2 = normalize(T2 * xi2.x + B2 * xi2.y + s.N * c2);
        RayDesc r2; const float bias2 = max(gRtParams.z, 1e-4) * (1.0 + length(hitPos - gCamPos.xyz) * 5e-4);
        r2.Origin = hitPos + s.N * bias2; r2.Direction = dir2; r2.TMin = bias2; r2.TMax = max(gVoxelParams.z, 1.0);
        RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q2;
        q2.TraceRayInline(gScene, RAY_FLAG_NONE, AVER_RT_MASK_OPAQUE_ALL, r2); averRtProceedSolid(q2);
        if (q2.CommittedStatus() != COMMITTED_TRIANGLE_HIT) indY = averSkyRadianceCheap(dir2) * gAmbient.r;
        else {
            // THE VOXEL SHELL STRADDLE: CSResolve (voxi.hlsl ~3417) stores an occupied voxel as
            // float4(meanRadiance, 1) and an empty one as float4(0,0,0,0) -- the volume is a
            // one-voxel-thick SHELL of surfaces, not a solid fill. Sampling exactly ON the hit (the old
            // `r2.Origin + dir2 * q2.CommittedRayT()`) puts the tap astride that shell, so trilinear
            // filtering blends the lit texel with whichever unlit neighbours it straddles -- the empty
            // far side of the shell, or the hollow interior of a thick wall -- and hands back exitant
            // radiance already scaled down by roughly 0.5-0.75 before anything below even runs.
            // Pulling the LOOKUP POINT half a voxel back along -dir2 (the direction this ray arrived
            // FROM -- there is no fetched geometric normal at a RayQuery hit here, only the ray
            // direction, so this stands in for "the hit's surface normal toward the room") re-centres
            // the tap on the room side of the shell instead of on its seam. voxelWorldF2 is the same
            // "one voxel, world units" quantity PSVoxelDebug/CSResolve's neighbours already compute
            // (voxi.hlsl:3441, voxi_gi.hlsli:210) from the same gVoxelOrigin.w/gVoxelParams.x terms
            // voxelUVW itself uses -- not a new cbuffer field.
            const float voxelWorldF2 = 1.0 / (gVoxelOrigin.w * gVoxelParams.x);
            const float3 uvw = voxelUVW(r2.Origin + dir2 * (q2.CommittedRayT() - voxelWorldF2 * 0.5));
            if (gVoxelParams.w > 0.5 && insideVolume(uvw)) {
                const float4 vox = gVoxelTex.SampleLevel(gVoxelSamp, uvw, 0);
                // Read the full float4 and normalise by occupancy (alpha) rather than the premultiplied
                // mean CSResolve stores -- undoing exactly the darkening the shell-straddle comment
                // above describes. Below AVER_GI_VOX_MIN_OCC there is effectively no occupied voxel left
                // in the trilinear footprint to recover a radiance from, so this falls back to 0 (no
                // information): the SAME value indY already carries into this branch whenever
                // insideVolume(uvw) is false or the volume is disabled (indY's own declaration above
                // defaults it to 0.0) -- not a new fallback, only that existing one made explicit for
                // the low-occupancy case too.
                indY = vox.a > AVER_GI_VOX_MIN_OCC ? min(vox.rgb / vox.a, AVER_VOX_MAXRAD) : 0.0;
            }
        }
        // U1's HALF-RES HISTORY (2.10 E) NEEDS THIS PATH'S OWN LUMINANCE, SEPARATELY FROM THE SKY IT
        // WAS COMPARED AGAINST -- only reachable here, at f2Path == 3u, the one path that actually
        // traced. averShadowLum is this file's own standing luminance reduction (Rec.709 weights,
        // voxi_rt.hlsli:1654), reused rather than a second formula.
        f2Observed  = true;
        f2LumTraced = averShadowLum(indY);
        f2LumSky    = averShadowLum(averSkyIrradiance(s.N) * gAmbient.r);
    }
    radiance += s.kdAlbedo * indY;

    samplePos      = hitPos;
    sampleNormal   = s.N;
    // Clamped here, not just left non-negative: averShadeDirect runs the FULL Cook-Torrance path at
    // this second-bounce hit, and its visibility term (visSmithCorrelated, `0.5 / max(lv + ll,
    // 1e-7)`) blows up exactly at a grazing hit, where both lv and ll go to zero -- the specular lobe
    // can return an enormous value for one cosine-weighted sample in a hundred (cos < 0.1 happens
    // 1% of the time over a uniform disc). The cone gather is structurally immune to this same
    // failure -- it averages over a mip footprint, and PSVoxel clamps to AVER_VOX_MAXRAD before
    // injection -- but this value is about to become the reservoir's STORED radiance, and a
    // reservoir KEEPS its sample (maxReservoirAge is 30 below): an unclamped spike here does not
    // flicker for one frame, it sits on this pixel for thirty and spreads to neighbours the moment a
    // spatial pass exists. So the clamp has to land BEFORE the value reaches the target pdf or the
    // reservoir, not merely at final output where giRestirIndirect already clamps its own estimate.
    //
    // NaN-SAFE, REPLACING A NATIVE clamp() -- see this function's own header note and the sky-miss
    // branch above for why a native three-argument clamp() is not trusted with a possibly-NaN input
    // here: min(max(x,lo),hi) floors a NaN to lo by this codebase's own documented comparison
    // semantics, where a native clamp()'s NaN behaviour is compiler/driver codegen order, not a
    // language guarantee.
    {
        const bool bad     = any(isnan(radiance)) || any(isinf(radiance));
        nonFiniteCandidate = nonFiniteCandidate || bad;
        sampleRadiance     = bad ? float3(0.0, 0.0, 0.0) : min(max(radiance, 0.0), AVER_VOX_MAXRAD);
    }
    return true;
}

// ---- the call site's one entry point: candidate + RTXDI temporal resampling, in and out ----
// A DROP-IN REPLACEMENT FOR coneTracedIndirect's OWN CONTRACT (float3 diffuse radiance, `ao` out) --
// see the two call sites in PSMainVoxi/PSRayDriven, which choose between the two by Settings::giMode
// and nothing else. `ao` is left at 1.0 always: ReSTIR GI's one resampled candidate per pixel does
// not estimate a hemisphere-coverage fraction the way the cone gather's own occlusion accumulator
// does, and inventing one with no real derivation behind it would be exactly the kind of fudge this
// codebase's own house style warns against elsewhere. NOT MODELLED, stated rather than silently
// wrong -- the unified ambient ray (gAmbientParams.x, High/Epic) already supplies AO independently
// of whichever diffuse estimator is chosen, which is the tier this gap matters least at. STILL TRUE
// AFTER F1-F3 (R0/R2/R3): the new hemisphere sampler, the second-bounce ray F2 adds at the candidate
// hit, and F3's own reuse-visibility ray all correct RADIANCE this function already carried -- none
// of them touch this `ao` output, so R4 (ReSTIR supplying no ambient occlusion at Low/Medium, where
// this is the ONLY occlusion signal) remains open and deliberately deferred, not fixed in passing.
#if AVER_GI_CHECKERBOARD
// A `static`, NOT a parameter, for the SAME reason gGiPoisonPdfHit above is one: giRestirIndirect's
// signature is shared with PSMainVoxi and PSRayDriven's own (non-checkerboard) call sites, and those
// compiles must not change. CSRdGi (voxi.hlsl), the one caller that defines AVER_GI_CHECKERBOARD, sets
// this per invocation -- before calling giRestirIndirect -- to say whether THIS pixel is the half NRD
// expects fresh data from this frame, or the half REBLUR is about to reconstruct instead.
static bool gGiCbSkip = false;
#endif

// ---- U1 (2.10 A) / B2: THE VISIBILITY-MODE + F2/F3 PATH DECODE, FACTORED OUT OF giRestirIndirect -----
// Moved out whole, unchanged, so CSRdGiTrace (voxi.hlsl, Settings::rayDrivenGiSplit) can compute the
// exact same f2Path/rho2 giRestirIndirect would for the SAME pixel before giRestirIndirect itself ever
// runs -- the split's whole premise is that the trace pass and the resample pass agree on which path a
// pixel takes without either one re-deriving it differently. giRestirIndirect (below) calls this once
// and unpacks the result into locals of the same names this block used to declare directly, so nothing
// past the decode has to change.
//
// visMode is settings_.giRestirVisibility as the C++ side clamped and packed it
// (VoxiRenderer::beginShadowHistory, givis::packAmbientW) -- 0 No ray, 1 Reconstructed,
// 2 HalfResolution, 3 Full. halfBound additionally requires the half-res pair actually be bound
// THIS frame (bit 4): an allocation failure (2.11) leaves visMode == 2 but the pair unbound, and
// that must behave as Full, not silently read a null descriptor. tracedPx is THIS pixel's own
// verdict from the phase table (giVisTracedPixel) when Half is active; it is trivially true
// (every pixel "traces") for every other mode, so the f2Path/f3Path arithmetic below needs no
// separate branch per mode.
//
// giVisReconstruct is called HERE, once, rather than separately for F2 and F3: both rays share
// one reconstruction (2.10 D's own header note), and calling it twice would trace the same four
// taps twice for no new information.
struct GiPathDecode {
    uint       visMode;
    bool       halfBound;
    bool       tracedPx;
    GiVisRecon rec;
    uint       spatialSamples;
    uint       maxHistory;
    uint       f2Path;
    uint       f3Path;
    float      rho2;
};
GiPathDecode giDecodePaths(float3 wpos, float3 N, float2 pixel, uint frameIdx) {
    const uint2 pixelPos = uint2(pixel);
    const uint visMode   = (uint)gAmbientParams.w & 3u;
    const bool halfBound = visMode == 2u && ((uint)gAmbientParams.w & 4u) != 0u;
    const bool tracedPx  = !halfBound || giVisTracedPixel(pixelPos, frameIdx);
    // Settings::giRestirSpatialSamples (0..15, splits ReSTIR GI's spatial reuse from its temporal
    // reuse for the still-open moving-camera fade bisection -- see that field's own comment, Voxi.hpp,
    // for the state of the investigation this is one half of), packed the same way visMode just
    // above is (givis::packAmbientW), one nibble higher -- decoded here, alongside visMode and
    // movingAge, for the identical "one decode block" reason. 15 is the AUTO sentinel: the override
    // this enables is applied only when the decode below reads something else, after the motion
    // discount computes its own numSamples (see that override's own comment for why it has to sit
    // there rather than beside this decode).
    const uint spatialSamples = ((uint)gAmbientParams.w >> 12) & 15u;
    // Settings::giRestirBiasCorrection (bits 16-17) and giRestirMaxHistory (bits 18-23): the two
    // dials over how a YOUNG reservoir is weighted, which is what the headless captures narrowed
    // this fade to -- a partially-converged reservoir reads brighter than both the no-reuse
    // estimate and the converged one. Decoded here with the rest of gAmbientParams.w rather than
    // beside the stparams fields they override further down, same convention as visMode.
    const uint maxHistory     = ((uint)gAmbientParams.w >> 18) & 31u;
    // NOT a ternary: HLSL's conditional operator only supports numeric scalar/vector/matrix results,
    // never a struct (DXC: "conditional operator only supports results with numeric scalar, vector,
    // or matrix types") -- GiVisRecon is a struct, so `halfBound ? giVisReconstruct(...) : (GiVisRecon)0`
    // is a hard compile error, not merely a style choice. `rec` starts at the same all-zero/invalid
    // default the ternary's false-branch spelled out, and is only overwritten when halfBound.
    GiVisRecon rec = (GiVisRecon)0;
    if (halfBound) rec = giVisReconstruct(wpos, N, pixel, frameIdx);
    // Path numbers, shared by F2 and F3 except where a legacy bit singles one of them out below:
    // 3 trace, 2 half-res ratio, 1 reconstructed (voxel cone / temporal-only), 0 legacy/no-ray.
    uint f2Path = 3u, f3Path = 3u;
    if (visMode == 1u)                       { f2Path = 1u; f3Path = 1u; }
    if (halfBound && !tracedPx && rec.valid) { f2Path = 2u; f3Path = 2u; }   // no valid reconstruction: trace, as Full
    if (((uint)gAmbientParams.z & 4u) != 0u || visMode == 0u) f2Path = 0u;   // legacy bit wins (2.8)
    if (((uint)gAmbientParams.z & 8u) != 0u || visMode == 0u) f3Path = 0u;
#if AVER_GI_CHECKERBOARD
    // SKIPPED PIXELS TRACE NO F3 VISIBILITY RAY EITHER -- only the Full-traced path (3); Path 2, Half's
    // own cheap reconstructed visibility (rec.v3, below), is deliberately left alone. f2Path goes to 0
    // because no candidate (and so no F2 ray) is traced below, which is also what the path debug view
    // then paints: yellow, "no ray".
    //
    // THE HALF-RESOLUTION HISTORY LOSES HALF ITS SUB-PIXEL POSITIONS, NOT HALF ITS REFRESH RATE. A
    // skipped Half-phase pixel observes nothing and the history write further down carries its
    // reconstruction forward (f2Observed/f3Observed false). kGiVisPhase cycles the traced pixel through
    // a 2x2 block over four frames, but the checkerboard flips every frame, so while the two frame
    // counters keep a fixed offset the SAME two phases always land on skipped pixels: each block is
    // refreshed from one fixed diagonal pair instead of all four positions. Which pair shifts whenever
    // NRD skips a frame. Exempting the phase pixel from the skip would restore all four at +1/8 of the
    // candidate rays; not done, the diagonal pair still covers both rows and both columns.
    if (gGiCbSkip) {
        f2Path = 0u;
        if (f3Path == 3u) f3Path = 0u;
    }
#endif

    // rho2: F2's RECONSTRUCTED-NON-TRACED path (f2Path == 2u) reads the neighbourhood's own occluded/
    // unoccluded sky ratio rather than an absolute luminance -- a RATIO OF EXPECTATIONS
    // (sum(w*g) / sum(w*b), giVisReconstruct's own wsum/gsum/bsum), not an average of per-tap ratios
    // (2.10 D.6's own distinction; GiVisibilityTest checks the two estimators disagree on a skewed
    // field so this is not an interchangeable simplification).
    // Computed here, once, rather than inside giTraceInitialCandidate, since F2 is the only reader.
    const float rho2 = (rec.b > 1e-4) ? (rec.g / rec.b) : 1.0;

    GiPathDecode d;
    d.visMode = visMode; d.halfBound = halfBound; d.tracedPx = tracedPx; d.rec = rec;
    d.spatialSamples = spatialSamples; d.maxHistory = maxHistory;
    d.f2Path = f2Path; d.f3Path = f3Path; d.rho2 = rho2;
    return d;
}

float3 giRestirIndirect(float3 wpos, float3 N, float curLinearDepth, float2 pixel, uint frameIdx,
                        out float ao) {
    ao = 1.0;
    const uint2 pixelPos = uint2(pixel);
    const RTXDI_ReservoirBufferParameters resParams = giReservoirBufferParams();
    const float frameJitter = (float)frameIdx * 2.39996323;

    // Reset THIS invocation's poison-view flag before anything below can set it -- see its own
    // declaration for why it is a `static` rather than a parameter.
    gGiPoisonPdfHit = false;

    // ---- U1 (2.10 A) / B2: DECODE THE VISIBILITY MODE AND THE TWO RAYS' PATHS, ONCE, UP FRONT ----
    // See giDecodePaths, just above, for the decode itself and its own header comment -- factored out
    // so CSRdGiTrace's own dispatch (voxi.hlsl) can compute an identical f2Path/rho2 for the candidate
    // it traces on this function's behalf when Settings::rayDrivenGiSplit is on (AVER_GI_SPLIT, below).
    const GiPathDecode gd     = giDecodePaths(wpos, N, pixel, frameIdx);
    const uint         visMode        = gd.visMode;
    const bool         halfBound      = gd.halfBound;
    const bool         tracedPx       = gd.tracedPx;
    const GiVisRecon   rec            = gd.rec;
    const uint         spatialSamples = gd.spatialSamples;
    const uint         maxHistory     = gd.maxHistory;
    const uint         f2Path         = gd.f2Path;
    const uint         f3Path         = gd.f3Path;
    const float        rho2           = gd.rho2;

    float3 samplePos, sampleNormal, sampleRadiance;
    bool nonFiniteCandidate = false;
    float f2LumTraced = 0.0, f2LumSky = 0.0;
    bool  f2Observed  = false;
    RTXDI_GIReservoir initial = RTXDI_EmptyGIReservoir();
    // SKIPPED PIXELS TRACE NO FRESH CANDIDATE: `initial` stays RTXDI_EmptyGIReservoir() (freshValid
    // false, below), which is the correct half-rate behaviour -- the spatio-temporal resampling just
    // past this block still runs for EVERY pixel regardless, so a skipped pixel's reservoir is still
    // re-projected and re-stored from last frame's temporal/spatial reservoirs, keeping the reuse
    // chain valid under motion even on the frame it traces nothing new.
#if AVER_GI_SPLIT
    // B4: THE CANDIDATE IS ALREADY TRACED -- CSRdGiTrace (voxi.hlsl) ran giTraceInitialCandidate for
    // THIS pixel in its own dispatch, with the identical f2Path/rho2 giDecodePaths just computed above
    // (both call sites decode from the same gAmbientParams/gGiCbSkip inputs), and stored every out
    // param in gRdGiCand[gGiCandIdx] -- gGiCandIdx set by CSRdGi, just before this call, to the same
    // row-pitch index CSRdGiTrace wrote under. Reading it back and taking the identical cosTheta/
    // RTXDI_MakeGIReservoir branch below reproduces the non-split branch's `initial` bit-for-bit (full
    // float precision throughout, no quantisation in the hand-off) -- only which dispatch pays for the
    // ray moves. Skipped exactly as the trace itself is skipped today: gGiCbSkip pixels never had a
    // candidate traced in EITHER compile, so there is nothing in gRdGiCand[gGiCandIdx] for them to read.
#if AVER_GI_CHECKERBOARD
    if (!gGiCbSkip)
#endif
    {
        const RdGiCand cand = gRdGiCand[gGiCandIdx];
        samplePos          = cand.pos;
        sampleNormal       = cand.nrm;
        sampleRadiance     = cand.rad;
        nonFiniteCandidate = (cand.flags & 2u) != 0u;
        f2LumTraced        = cand.f2LumTraced;
        f2LumSky           = cand.f2LumSky;
        f2Observed         = (cand.flags & 4u) != 0u;
        if ((cand.flags & 1u) != 0u) {
            const float cosTheta = saturate(dot(normalize(samplePos - wpos), N));
            if (cosTheta > AVER_GI_MIN_COS)
                initial = RTXDI_MakeGIReservoir(samplePos, sampleNormal, sampleRadiance, cosTheta / PI);
        }
    }
#else
#if AVER_GI_CHECKERBOARD
    if (!gGiCbSkip)
#endif
    if (giTraceInitialCandidate(wpos, N, pixel, frameJitter, samplePos, sampleNormal, sampleRadiance,
                                 nonFiniteCandidate, f2Path, rho2, f2LumTraced, f2LumSky, f2Observed)) {
        const float cosTheta = saturate(dot(normalize(samplePos - wpos), N));
        // THE SAME FLOOR, because this is the gate that actually decides what gets stored. The
        // recomputed cosTheta is the sampled one up to precision (dir is built from it and samplePos
        // lies along dir), so a `> 0.0` here was strictly weaker than the reject inside
        // giTraceInitialCandidate and quietly let the 1/cos blow-up through anyway.
        if (cosTheta > AVER_GI_MIN_COS)
            initial = RTXDI_MakeGIReservoir(samplePos, sampleNormal, sampleRadiance, cosTheta / PI);
    }
#endif

    // ---- F3's OWN COPY OF THIS FRAME'S FRESH CANDIDATE, TAKEN BEFORE RESAMPLING CAN REPLACE IT ----
    // `initial` is about to be handed to RTXDI_GISpatioTemporalResampling below, which is free to
    // hand back a completely different reservoir in `result` -- a temporal or spatial tap, not this
    // frame's own traced sample. The visibility gate this function adds further down needs to tell
    // those two cases apart: THIS frame's own candidate already had its visibility proven by the very
    // ray giTraceInitialCandidate just traced, so re-tracing it would be pure waste; anything else
    // reaching `result` has never been visibility-tested at all (F3/R3, below). Captured here, before
    // the resampling call, because `initial` does not survive it unexamined -- RTXDI_
    // GISpatioTemporalResampling takes it by value, and `result` may or may not still equal it once
    // that call returns.
    const bool freshValid = RTXDI_IsValidGIReservoir(initial);
    const float3 freshPos = initial.position;

    const uint writeSlice = (uint)(gGiRestirParams.z + 0.5);
    RTXDI_GIReservoir result = initial;
    if (gGiRestirParams.y > 0.5) {   // a real previous frame exists to resample against
        RAB_Surface surface = RAB_EmptySurface();
        surface.valid       = true;
        surface.worldPos    = wpos;
        surface.normal      = N;
        surface.linearDepth = curLinearDepth;

        // screenSpaceMotion.xy: REUSES rtReprojectHistory's own reprojection recipe (same
        // gPrevViewProj, same gSceneViewport convention) rather than averGBufferVelocity's -- that
        // one returns DESTINATION MINUS SOURCE (UpscalerNeeds::MotionVectors' contract), the
        // OPPOSITE sign RTXDI_GITemporalResampling wants (`prevPos = pixelPosition +
        // screenSpaceMotion.xy`, i.e. source relative to destination). .z is the depth delta a
        // static point's reprojected depth would show, for RTXDI's own disocclusion test.
        float3 screenSpaceMotion = float3(0, 0, 0);
        {
            const float4 prevClip = mul(float4(wpos, 1.0), gPrevViewProj);
            if (prevClip.w > 1e-4) {
                const float3 prevNdc = prevClip.xyz / prevClip.w;
                const float2 prevPx = gSceneViewport.xy +
                    float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * gSceneViewport.zw;
                screenSpaceMotion = float3(prevPx - pixel, prevClip.w - curLinearDepth);
            }
        }

        RTXDI_RandomSamplerState rng = RTXDI_InitRandomSampler(pixelPos, frameIdx, 1u);
        RTXDI_GISpatioTemporalResamplingParameters stparams = (RTXDI_GISpatioTemporalResamplingParameters)0;
        // Settings::giRestirDepthThreshold / giRestirNormalThreshold (Voxi.hpp): RTXDI's own reuse-
        // similarity tolerances, lifted here from a shader literal so the moving-camera fade bisection
        // can sweep them without a rebuild -- the other half of the split spatialSamples (decoded
        // above, applied below) is for. 0.1 / 0.5 were this file's own hard-coded literals before
        // either field existed and are now that field's compiled-in DEFAULT (Voxi.hpp), not a changed
        // number -- the engine clamps whatever a caller sets to [0.001, 1.0] / [0.0, 0.999]
        // (Voxi.cpp) before either ever reaches here, so this line cannot read an out-of-range value.
        stparams.depthThreshold        = 0.1;
        stparams.normalThreshold       = 0.5;
        // ---- 1, NOT 8: A YOUNG HISTORY OVERSHOOTS, AND ITS DEPTH BOUGHT NOTHING NRD DID NOT ----
        //
        // RTXDI_CombineGIReservoirs sums M across the fresh, temporal and spatial streams with only a
        // per-tap clamp to this number, so a lineage a few frames old claims far more independent
        // samples than it holds, and the BASIC normalisation over-weights it. The estimate starts high
        // and decays: measured on PTTest's NewSponza, fixed camera and exposure, the image fell from
        // 82 to 68.5 (tonemapped mean) over ~300 frames after level load, and resolving each pixel
        // from its fresh candidate alone removed the fall (71.9 -> 71.3). The same overshoot follows
        // every GI history reset -- including the one a sun change now triggers (see
        // VoxiRenderer::beginShadowHistory) -- as a short bright bump before settling. At 1 the
        // overshoot is gone with the same settled value (arcade wall, linear x1000: 1.7 at frame 45 and
        // at frame 900, where 8 read 2.5 then 1.7) and no measurable noise cost after NRD: still-frame
        // grain 0.161 vs 0.160, frame-to-frame flicker 0.0161 vs 0.0160, and under a 6-degree camera
        // wobble 1988 vs 1976 firefly outliers per frame with identical flicker.
        //
        // WHAT FOLLOWS IS THE EARLIER 20 -> 8 ANALYSIS, kept because the spatial tap count below still
        // leans on it:
        //
        // Write the weight recursion with K taps each capped at M, plus the fresh candidate's M = 1:
        //     W_next = [ (PI/cos)*1 + K*M*J*W ] / (1 + K*M)
        // The fresh candidate is the ONLY term pinned to a physically-derived 1/pdf; every other
        // term is W multiplied by that stream's Jacobian. So W is a multiplicative accumulator whose
        // contraction factor is K*M*E[J]/(1 + K*M), and it is stable in the mean only while
        // E[J] < (1 + K*M)/(K*M).
        //
        // At the old 20 with numSamples 8 that is 181/180 -- a margin of 0.56%. The Jacobian of a
        // spatial tap 32 pixels away is a ratio of squared distances between two different receivers
        // and a shared sample; under camera motion its mean is nowhere near within half a percent of
        // one, and RAB_ValidateGISampleWithJacobian USED TO only clamp each sample to [1/25, 25],
        // which bounded a single step and did nothing about the compounding. That was the remaining
        // reported symptom: at rest J was close to 1 and the estimate crept toward correct, and the
        // moment the camera moved J scattered and W ran away -- bright, with flat wrong-bright
        // regions where a whole neighbourhood shared one runaway sample.
        //
        // 8 taps of 8 gives K*M = 24 and a margin of 25/24, about 4.2% -- roughly eight times the
        // headroom for a history still deep enough to be worth having. The cost is a shorter
        // effective sample count, which is noise, and noise is the thing a denoiser can actually
        // fix; a runaway multiplicative weight is not.
        //
        // UPDATE, now that RAB_ValidateGISampleWithJacobian (above) reports 1.0 for every ACCEPTED
        // tap instead of the measured ratio: E[J] over accepted taps is now exactly 1.0 by
        // construction, not merely close to it, so the contraction factor K*M/(1+K*M) is stable for
        // ANY K*M -- the runaway this margin analysis was defending against can no longer arise
        // through the Jacobian at all. That is what made dropping to 1 a free choice rather than a
        // stability trade -- see the measurement at the top of this block.
        // voxi.giRestirMaxHistory, default 1 -- the value 602d1b06 measured and shipped. See
        // Settings::giRestirMaxHistory for what raising it is meant to prove.
        stparams.maxHistoryLength      = maxHistory;
        // ---- 0, AND THE 1 IT REPLACES WAS DISARMING EVERY SPATIAL TAP ----
        //
        // `usingFallback` inside RTXDI_GISpatioTemporalResampling is a LATCH, not a per-tap flag: it
        // is set when the temporal taps fail, never cleared, and the loop then runs the fallback tap
        // AND all 8 spatial taps with `!usingFallback && !RTXDI_IsValidNeighbor(...)` short-circuited.
        // Camera motion is precisely what makes the temporal taps fail, so in motion every spatial
        // tap at radius 32 was landing on arbitrary pixels -- sky, disoccluded, anything -- with no
        // depth or normal test between them and the combine. RAB_AreMaterialsSimilar returns true
        // unconditionally here, and the only remaining gate, RTXDI_IsValidGIReservoir, tests M != 0,
        // which is a property of the reservoir SLOT and not of the surface.
        //
        // WHAT THAT COST, and it is the motion half of the reported bug: an invalid neighbour enters
        // the MIS NUMERATOR (targetPdf * jacobian * W * M) but contributes nothing to the DENOMINATOR,
        // because piSum is built from the selected sample's pdf evaluated AT THAT NEIGHBOUR'S surface
        // -- which is invalid, so RAB_GetGISampleTargetPdfForSurface correctly returns 0. Streams in
        // the numerator and absent from the denominator inflate W by up to 180x (9 streams x M=20
        // against a seed of 1). That is the frame washing out the instant the camera moves.
        //
        // Turning it off costs the fallback's intended job -- finding SOME temporal history after a
        // disocclusion -- and that is the correct trade: with the similarity test disarmed it was not
        // finding history, it was reusing 32-pixel-distant strangers indiscriminately.
        stparams.enableFallbackSampling = 0;
        // BASIC. Switching it OFF was captured (2026-09-19) and moved the camera-motion overshoot
        // by nothing at all, so the mode is not a lever on it either way.
        stparams.biasCorrectionMode    = RTXDI_BIAS_CORRECTION_BASIC;
        stparams.maxReservoirAge       = 30;
        stparams.enablePermutationSampling = 0;
        stparams.uniformRandomNumber   = frameIdx * 2654435761u;
        // NVIDIA's own ReSTIR GI sample defaults for the spatial half this variant adds on top of
        // temporal reuse: 8 taps at a 32-pixel radius. That radius is wide enough to bleed indirect
        // light across a corner if nothing were watching it, but RTXDI_IsValidNeighbor's depth/
        // normal similarity test -- already run for every neighbour, temporal jitter and spatial tap
        // alike, inside SpatioTemporalResampling.hlsli -- rejects a tap landing on a dissimilar
        // surface before its reservoir is ever combined in. The rejection is what keeps reuse local
        // AT REST, not the radius -- see the motion discount just below for why "at rest" matters.
        // 2, NOT NVIDIA'S 8, AT REST, and for the stability reason maxHistoryLength above spells out
        // rather than to save the taps. Those 8 are the default for a STANDALONE SPATIAL PASS, which
        // runs once over an already-normalised frame; here they are fused into the temporal pass and
        // so they multiply the history length in K*M. Taking K from 9 to 3 is most of the margin.
        //
        // ---- DISCOUNT SPATIAL REUSE UNDER MOTION -- the change most directly supported by the
        // ghosting measurement (this file's own header comment) ----
        //
        // WHY THE COMBINE NEEDS ONE AND NEVER HAD ONE: under rotation the reprojected screen
        // position moves every frame, so the SAME 32px neighbourhood the depth/normal test above
        // approves this frame is a DIFFERENT 32px neighbourhood next frame -- the taps are locally
        // valid at every instant and STILL never converge, because "locally valid" was never the
        // same test as "the same sample twice". RTXDI_IsValidNeighbor cannot see that; it has no
        // notion of history at all. Fewer taps, drawn from a narrower ring, is what actually answers
        // it: it does not fix any one tap's Jacobian (RAB_ValidateGISampleWithJacobian above already
        // does that), it reduces how much never-converging spatial content is combined in per frame
        // while the camera moves, at the honest cost of more noise for the denoiser to absorb.
        //
        // THE SAME SHAPE AS rtShadowTemporal's OWN VELOCITY DISCOUNT (voxi_rt.hlsli, around
        // :1695-1712: `t = saturate(length(velocityPx) / 32.0); weight = lerp(0.9, 0.5, t)`), not a
        // new mechanism invented for this file -- that function already answers "how much should a
        // reused estimate be trusted as reprojection distance grows" for a different signal (traced
        // shadow visibility), and this is the identical question asked of ReSTIR's spatial reuse.
        // 32.0, the SAME divisor, for the same reason: motionPx is the same "pixels of reprojection
        // this frame" quantity rtShadowTemporal's velocityPx measures, so reusing its calibration is
        // more defensible than inventing a second number with no measurement behind it either.
        // REASONED, NOT MEASURED: no sweep was run here to confirm 32px is also the right knee for
        // THIS discount, only that it is this engine's own existing answer to the same question
        // asked of a different signal, restated rather than a fresh guess.
        //
        // FLOORS, NOT ZERO AT FULL MOTION. samplingRadius 0 would not mean "no spatial reuse", it
        // would mean "every one of numSamples taps lands on (0,0)" -- RTXDI_CalculateSpatialResamplingOffset
        // (Rtxdi/GI/SpatialResampling.hlsli) scales kGiNeighborOffsets by samplingRadius, so a radius
        // of zero collapses every offset to the CENTRE pixel, re-tapping prevPos's own temporal
        // result under a different name rather than reusing nothing. 1 tap at an 8px radius is the
        // smallest neighbourhood that still counts as spatial reuse rather than a disguised no-op.
        const float motionPx = length(screenSpaceMotion.xy);
        const float motionT  = saturate(motionPx / 32.0);
        stparams.numSamples     = (uint)round(lerp(2.0, 1.0, motionT));
        stparams.samplingRadius = lerp(32.0, 8.0, motionT);
        // ---- voxi.giRestirSpatialSamples: OVERRIDE THE MOTION DISCOUNT'S OWN COUNT, FOR THE SAME
        // FADE BISECTION giRestirDepthThreshold/giRestirNormalThreshold ABOVE ARE FOR ----
        //
        // MUST SIT HERE, AFTER THE LERP JUST ABOVE, NOT BESIDE spatialSamples' OWN DECODE (top of this
        // function): it overrides the exact field that lerp just computed, so it needs the discount's
        // verdict to already exist before it can replace it -- the same ordering reason movingAge's
        // own override, a few lines above, sits after motionT rather than beside visMode's decode.
        //
        // 15 (AUTO) skips this entirely, leaving the discount's own numSamples untouched -- byte-
        // identical to today's image. Any other decoded value REPLACES it outright, discount and all:
        // 0 means no spatial reuse at all (temporal only), isolating whether the fade survives with
        // the spatial half of this pass disabled completely; 1..8 pins the tap count regardless of
        // camera motion. min(decoded, 8u) is a second, defensive ceiling on top of Voxi.cpp's own
        // [0,15] clamp -- 9..14 are reachable bit patterns that are not the 15 sentinel, and 8 is the
        // same ceiling the K*M margin analysis above already justifies for numSamples itself.
        if (spatialSamples != 15u)
            stparams.numSamples = min(spatialSamples, 8u);
        // ---- U1 (2.10 C): RECONSTRUCTED FORCES TEMPORAL-ONLY, AFTER THE MOTION DISCOUNT ABOVE, NOT
        // BEFORE ---- f3Path == 1u means giRestirVisibility selected Reconstructed (or the pixel is
        // in Half's non-traced/no-valid-reconstruction limbo that already collapsed to Reconstructed
        // above -- it has not, f2Path/f3Path == 1u only ever comes from visMode == 1u itself, see the
        // decode block), and the spatial half of this pass is exactly the extra reuse Reconstructed's
        // own cost model (2.10 F) accounts for as zero. Setting numSamples AFTER the motion-discount
        // lerp above means this always wins regardless of motionT -- checked by
        // GiVisibilityTest's own source assertion that this line follows the lerp textually.
        if (f3Path == 1u) stparams.numSamples = 0u;
        RTXDI_RuntimeParameters rParams = (RTXDI_RuntimeParameters)0;   // no checkerboard
        // neighborOffsetMask is the field the spatial half actually reads: RTXDI_
        // CalculateSpatialResamplingOffset masks its index by this before it ever touches
        // kGiNeighborOffsets above, so it has to match that table's own size minus one, or the mask
        // opens the index onto whatever garbage sits past the array's 32 entries. frameIndex, by
        // contrast, is NOT read by anything under third_party/rtxdi/Include -- checked the whole
        // tree, not assumed -- it only feeds ReSTIRDIContext's own uniformRandomNumber/checkerboard
        // bookkeeping on the CPU side of the DI half of this SDK, which this file does not use (it
        // hashes its own uniformRandomNumber above instead). Set anyway, because leaving a struct
        // field the SDK's own convention expects populated silently zeroed is the kind of thing that
        // stops mattering right up until a future header reads it.
        rParams.neighborOffsetMask = kGiNeighborOffsetMask;
        rParams.frameIndex         = frameIdx;

        // ARGUMENT ORDER DIFFERS FROM RTXDI_GITemporalResampling ABOVE: Rtxdi/GI/
        // SpatioTemporalResampling.hlsli's own signature takes sourceBufferIndex BEFORE
        // screenSpaceMotion, the opposite of Rtxdi/GI/TemporalResampling.hlsli's. Swapping them back
        // to the old order would still COMPILE -- HLSL implicitly truncates a float3 argument passed
        // where a scalar uint is expected (taking .x) and broadcasts a scalar into a float3 position
        // -- so this order is only trustworthy because it was read off the vendored signature, not
        // because a compile with the wrong order would have said so.
        result = RTXDI_GISpatioTemporalResampling(pixelPos, surface, 1u - writeSlice, screenSpaceMotion,
                                                   initial, rng, rParams, resParams, stparams);
    }

    // ---- THE RESIDUAL AFTER THE cosR FLOOR ABOVE -- THREE BACKSTOPS TRIED, ALL MEASURED, NONE KEPT ----
    //
    // --firefly-metric (SandboxApp.cpp), added for this task, is the first thing that could count a
    // ReSTIR GI firefly rather than guess at one. On the user's own repro (PTTest Sponza, giMode 1,
    // --cam-wobble) it found a real, motion-triggered spike: outlier pixel count sat in a ~150-350
    // band and jumped past 800-1200 for single sampled frames with the denoiser off, while the SAME
    // run with --denoiser 1 never left the low band across 200 frames. A screenshot at one spiking
    // frame confirms it by eye -- a field of small bright dots across an otherwise dark wall, gone
    // completely with the denoiser on. This is the measurement the four prior rounds of fixes to this
    // bug shipped without. Flooring cosR in RAB_GetGISampleTargetPdfForSurface (above) measurably
    // reduced how often this fires -- several previously-spiking sampled frames read clean afterward
    // -- but did not eliminate it; a residual, still motion-triggered, remained.
    //
    // THREE BACKSTOPS WERE TRIED AGAINST THAT RESIDUAL, IN THIS ORDER, EACH MEASURED AND EACH LEFT NO
    // MARK -- kept here as recorded negative results rather than deleted, per this task's own
    // instruction not to reinvent a hypothesis already tried:
    //
    // 1. A WAVE-LEVEL AVERAGE (WaveActiveSum/WaveActiveCountBits over the pixel's own wave) -- the
    //    closest a pixel shader can get to RTXDI_GIBoilingFilter's own groupshared reduction (Rtxdi/
    //    GI/BoilingFilter.hlsli), which needs a COMPUTE thread group and is illegal here
    //    (giRestirIndirect runs from PSMainVoxi/PSRayDriven, pixel shaders both -- moving this pass
    //    onto compute is a real restructuring, out of this task's file ownership). Re-measuring showed
    //    no improvement: frame 70 still spiked to 1016 (was 980), and a NEW spike appeared at frame
    //    110 that had not been one before.
    // 2. EACH PIXEL AGAINST ITS OWN RESERVOIR FROM LAST FRAME (`1u - writeSlice` at the raw,
    //    un-reprojected pixelPos). Reasoned to dodge the spatial contamination above, since it depends
    //    on no neighbour at all -- measured to not matter either: frames 110/150/190 still spiked
    //    (1039/1082/903).
    // 3. AN ABSOLUTE CEILING on the finalised weightSum, at a multiple of PI/AVER_GI_MIN_COS (~62.8,
    //    the most a single well-formed candidate's own 1/pdf can be -- see that constant's own
    //    comment). Independent of any neighbour or history, so immune to the contamination that
    //    plausibly explains why (1) and (2) did nothing. Swept from a generous 8x down to 1x -- the
    //    SAME ceiling a single candidate itself is already held to -- and at EVERY multiple tried, the
    //    spike frames were UNCHANGED (1x: frame 110 = 1042, 115 = 1070, 150 = 843; the un-clamped
    //    baseline was 1024/1066/1037 at the same three frames). A ceiling this tight, doing nothing,
    //    is decisive: no reservoir feeding these frames' pixels is exceeding even a single candidate's
    //    own honest weight.
    //
    // WHAT THAT RULES IN, SINCE IT RULES OUT EVERY weightSum-SIDE EXPLANATION: the widespread, fully-
    // correlated brightening is not any one reservoir's weight running away. The remaining candidate is
    // GENUINE, CORRECT MONTE CARLO VARIANCE -- many nearby pixels, each drawing its OWN independent
    // cosine-weighted candidate direction (rtDiscSample/rtHash, giTraceInitialCandidate above), having
    // a small but real chance of independently landing on the SAME narrow, high-contrast light path
    // (glimpsing the sun or a small sunlit patch through a gap) at THIS specific camera angle. Nothing
    // about that is a bug to clamp away -- clamping it would mean discarding real light transport that
    // a single candidate per pixel is not enough to smoothly resolve on its own, which is exactly the
    // job RTXDI's own spatiotemporal reuse and this engine's REBLUR integration exist to do, and
    // measurably already do (the denoiser-on band above). Shipping a FOURTH unmeasured backstop on top
    // of three that measured as no-ops would be exactly the pattern this task was written to stop.

    // ---- W6/M5: NO LONGER A KNOWN LIMITATION -- GATED ON gAverHistoryWrite, ON BY DEFAULT ----
    // Until this task, a pixel covered by TRANSLUCENT geometry was written to THIS reservoir slot
    // TWICE in one frame: PSRayDriven writes every pixel first; the blended glass/water replay is
    // PSMainVoxi drawn over the same pixels, and the two share one compiled shader binary differing
    // only in blend state and depth-write, so no #define or per-pass constant could tell them apart
    // from inside this function -- only a PER-DRAW signal (the material's own transmission/blend
    // flags, D3D12's own "this draw replayed blended" bit) can, and gAverHistoryWrite is exactly that
    // signal, set once at the top of PSMainVoxi (voxi.hlsl) and read here as a plain gate rather than
    // threaded through as a new parameter. The OLD degrade-gracefully path -- the opaque surface
    // reprojecting into a slot that still described the glass hit, RTXDI_IsValidNeighbor's depth/
    // normal test rejecting the mismatch, and one extra frame of noise while reuse rebuilt from
    // scratch -- is what voxi.legacyBlendedHistoryWrite (gAmbientParams.z bit 32) still reproduces,
    // byte-identical, for A/B: it forces gAverHistoryWrite back to true on every fragment regardless
    // of blend state (voxi.hlsl's own PSMainVoxi comment on the flag). PSRayDriven's own writes are
    // never suppressed by this gate at all -- a blended pane never reaches that entry point (see its
    // own header comment), so it sets gAverHistoryWrite = true unconditionally rather than computing
    // a blendedFragment test it can never need.
    // ---- NEVER STORE A CORPSE: weightSum 0 with M != 0 POISONS EVERY PIXEL THAT TAPS IT ----
    //
    // RTXDI_FinalizeGIResampling writes ONLY weightSum -- it never touches M or radiance. So a
    // reservoir whose MIS numerator finalised to zero is stored carrying weightSum = 0, a fully
    // bright radiance, and an M of up to 181. RTXDI_IsValidGIReservoir tests `M != 0` and nothing
    // else, so next frame that corpse is a perfectly valid neighbour: it adds `ps * M` to piSum and
    // `targetPdf * jacobian * 0 * M` -- exactly zero -- to the numerator. It is pure denominator.
    //
    // THAT MAKES BLACKNESS CONTAGIOUS, and it is the still-camera half of the bug. One dead tap among
    // nine costs the surviving pixels about 21x; those pixels then finalise darker, some of them to
    // zero, and spread it to the nine that tap THEM next frame. It converges to black over a second
    // or two of holding still, and one camera movement rejects the history and hides it again.
    //
    // AND d4e56396 IS WHAT MANUFACTURES THE CORPSES, so this is the other half of that fix rather
    // than a separate concern: making RAB_GetGISampleTargetPdfForSurface return 0 for an invalid
    // surface was right -- an absent surface must not contribute a weight -- but a zero arriving as
    // `pi` finalises the whole reservoir to zero, and nothing was stopping that being written back.
    //
    // An M = 0 reservoir is skipped by RTXDI_IsValidGIReservoir at the top of the neighbour loop, so
    // emptying it here removes it from both sums instead of leaving it in one.
    //
    // ---- EXTENDED: NaN/Inf stops here too, not only an exact-zero weight -- THE FIX THAT MATTERS
    // MOST IN THIS FILE ----
    //
    // A comparison against NaN is false, so the corpse guard above (`<= 0.0`) silently let a NaN
    // weightSum straight through to RTXDI_StoreGIReservoir. RTXDI_CombineGIReservoirs's own
    // `weightSum += risWeight` (third_party/rtxdi/Include/Rtxdi/GI/Reservoir.hlsli) propagates a NaN
    // unconditionally -- unlike this codebase's own comparison-based idioms, `+=` does not self-heal
    // -- and RTXDI_FinalizeGIResampling only ever guards an EXACT-zero denominator. Neither vendored
    // function has ever protected against this. Once stored, RTXDI_IsValidGIReservoir tests only
    // `M != 0`, so a poisoned reservoir reads as a perfectly "valid" neighbour forever after: every
    // later comparison against a NaN weightSum is false, so it can never again be selected OUT, and
    // the 8-32px spatial/temporal taps spread it one hop per frame, forever.
    //
    // THIS GUARD DOES NOT NEED TO KNOW WHERE THE FIRST NON-FINITE VALUE CAME FROM. Whatever
    // manufactures it -- the candidate-radiance clamps below, the target-pdf guard, or a cause nobody
    // has found yet -- a reservoir that is never STORED non-finite can never spread or persist. This is
    // the circuit breaker; the guards elsewhere in this file (giTraceInitialCandidate's clamps,
    // RAB_GetGISampleTargetPdfForSurface's own return) are risk reduction on top of it, not a
    // substitute for it.
    const bool corpseWeight     = result.weightSum <= 0.0;
    const bool nonFiniteWeight  = isnan(result.weightSum) || isinf(result.weightSum);
    const bool nonFiniteRad     = any(isnan(result.radiance)) || any(isinf(result.radiance));
    const bool giPoisonStoreHit = nonFiniteWeight || nonFiniteRad;
    if (corpseWeight || giPoisonStoreHit) result = RTXDI_EmptyGIReservoir();
    // W6/M5: GATED ON gAverHistoryWrite, ON BY DEFAULT (D3 decision) -- this is the reservoir store
    // this function's own KNOWN LIMITATION comment, just above, used to describe as an accepted,
    // unconditional double write: a blended-replay fragment (PSMainVoxi's glass/water pane) used to
    // overwrite THIS pixel's reservoir a second time with the pane's own candidate, so next frame the
    // OPAQUE surface behind it reprojected into a slot describing the glass hit instead. See voxi.hlsl's
    // own gAverHistoryWrite/averDrawIsTranslucent for the flag, and voxi.hlsl's PSMainVoxi for where a
    // blended fragment clears it. voxi.legacyBlendedHistoryWrite (gAmbientParams.z bit 32) restores the
    // old unconditional store, byte-identical, for A/B; PSRayDriven never sees this pixel's write
    // suppressed at all, since it sets gAverHistoryWrite = true unconditionally (a blended pane never
    // reaches that entry point -- see its own header comment).
    if (gAverHistoryWrite) RTXDI_StoreGIReservoir(result, resParams, pixelPos, writeSlice);

    // NEXT FRAME'S "previous surface" -- see gGiSurfPosHist/gGiSurfNrmHist's own declaration for
    // the two-texture format and the 0-packed-normal sentinel RAB_GetGBufferSurface tests for
    // "nothing written here". Nudged off exactly zero so a legitimately-packed normal never
    // collides with that sentinel.
    const uint packedN = RTXDI_EncodeNormalizedVectorToSnorm2x16(N);
    // W6/M5: GATED THE SAME WAY THE RESERVOIR STORE ABOVE IS, FOR THE SAME REASON -- a blended-replay
    // fragment writing its own (glass/water) surface into this pixel's history is the identical
    // double-write, one level up: next frame's giVisReconstruct/RAB_GetGBufferSurface taps would
    // silently read the pane's position/normal instead of the opaque surface's. Both channels share
    // one gate rather than two, since they are always written together (this file's own header
    // comment on the pair: "TWO RG32Float TEXTURES... gGiSurfPosHist carries xy... gGiSurfNrmHist
    // carries z... plus the packed normal").
    if (gAverHistoryWrite) {
        gGiSurfPosHistOut[pixelPos] = wpos.xy;
        gGiSurfNrmHistOut[pixelPos] = float2(wpos.z, asfloat(packedN == 0u ? 1u : packedN));
    }

    // Hoisted to function scope (not declared inside the block below) so the poison-view combination
    // at the end of this function -- after the NRD block further down -- can still read them.
    bool giPoisonEstHit = false;
    bool giPoisonNrdHit = false;
    // THE TWO NEW BITS: not a non-finite guard catching corruption, but AVER_VOX_MAXRAD's ceiling
    // clamp actually engaging on a FINITE value -- see this function's own POISON DEBUG VIEW comment
    // below for why that distinction, and Settings::giRadianceCeiling (Voxi.hpp) for what the ceiling
    // is and why hitting it paints solid white downstream. Hoisted for the same reason as the two
    // above: giPoisonNrdCeilHit is set inside the NRD block further down, read at the same
    // combination site.
    bool giPoisonEstCeilHit = false;
    bool giPoisonNrdCeilHit = false;

    float3 outDiffuse = 0.0;
    if (RTXDI_IsValidGIReservoir(result)) {
        // Recomputed HERE against THIS pixel's (wpos, N), not reused from the initial candidate's
        // cosTheta up at giTraceInitialCandidate's call site: temporal resampling can hand `result`
        // back holding a completely different sample than the one this pixel traced this frame --
        // that is the whole point of a reservoir, and RTXDI_GITemporalResampling is free to replace
        // it wholesale -- so the receiving-surface cosine has to be derived from wherever
        // result.position actually landed, not cached from a direction that may no longer be the one
        // that won. A reservoir stores a world POSITION rather than a cached angle for exactly this
        // reason.
        const float3 toSample = result.position - wpos;
        const float  dist2    = dot(toSample, toSample);
        const float  cosR     = dist2 > 1e-8 ? saturate(dot(toSample * rsqrt(dist2), N)) : 0.0;

        // ---- F3 (R3): A REUSED SAMPLE HAS NEVER BEEN CHECKED FOR VISIBILITY FROM HERE ----
        //
        // giTraceInitialCandidate's own ray already proved THIS frame's fresh candidate visible from
        // wpos -- tracing a ray to it is what that function did. Temporal and spatial resampling above
        // can swap `result` for a DIFFERENT reservoir -- another frame's position, or a neighbour
        // pixel's -- and nothing in RTXDI's own SpatioTemporalResampling.hlsli traces a ray between
        // THIS receiver and THAT stored position: RAB_GetTemporalConservativeVisibility exists for
        // exactly that and sits unused above, because this file selects BASIC bias correction. On a
        // coplanar, similar-depth pixel just the far side of an occlusion edge -- the arcade floor
        // inside vs. outside the roof line, a wall beside a window slot -- a neighbour's sunlit or
        // sky-facing sample was being credited to a receiver that provably cannot see it.
        //
        // ONE RAY, SKIPPED WHEN IT CANNOT POSSIBLY BE NEEDED: gAmbientParams.z bit 8 TRUE restores
        // HEAD exactly -- no ray, `result` shaded exactly as resampled, for comparison only. Bit 8
        // FALSE (the corrected default, ON by the user's own call) still skips the ray for THIS
        // frame's own fresh candidate (freshValid && result.position == freshPos -- already proven
        // visible above) and for a degenerate same-point sample (dist2 <= 1e-8, the same guard cosR
        // already uses); every other case -- any temporal or spatial reuse -- gets exactly one ray.
        //
        // SHADING ONLY, NEVER THE RESERVOIR. `result` itself is never written here, and the corpse-
        // guard store above (this function's own NEVER STORE A CORPSE block) has already happened --
        // a visibility miss darkens only what THIS pixel sees this frame, the same way a shadow ray
        // would, without erasing the sample for whichever neighbour resamples it next with a different
        // view of the same point. Unbiased for the reason this file's own RAB_GetGISampleTargetPdfFor
        // Surface states at its own header: RIS with a target pdf that ignores visibility, multiplied
        // here by the true contribution f*V, stays an unbiased estimator of the VISIBLE integral,
        // because the pdf's support is a superset of the visible one.
        // ---- U1 (2.10 C): f3Path == 3u GUARDS THE RAY NOW, AND THE ORIGINAL CONDITION TEXT IS
        // OTHERWISE UNCHANGED ---- `reused` restates the same "is this actually a resampled sample,
        // not this frame's own already-proven-visible candidate" test the ray's own condition already
        // computes, named once so the new Half branch below does not repeat it a third time; the ray
        // branch keeps the original inline expression rather than reading `reused` itself, so its
        // condition text stays the literal string this file (and GiVisibilityTest) has always had,
        // with only `f3Path == 3u &&` inserted.
        float visF3 = 1.0; bool f3Observed = false;
        const bool reused = !(freshValid && all(result.position == freshPos)) && dist2 > 1e-8;
        if (((uint)gAmbientParams.z & 8u) == 0u && f3Path == 3u &&
            !(freshValid && all(result.position == freshPos)) && dist2 > 1e-8) {
            const float dist = sqrt(dist2);
            const float bias = max(gRtParams.z, 1e-4) * (1.0 + length(wpos - gCamPos.xyz) * 5e-4);
            RayDesc rv;
            rv.Origin    = wpos + N * bias;
            rv.Direction = toSample / dist;
            rv.TMin      = bias;
            rv.TMax      = max(dist - 2.0 * bias, bias);
            RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> qv;
            qv.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, AVER_RT_MASK_OPAQUE_ALL, rv);
            averRtProceedSolid(qv);
            if (qv.CommittedStatus() == COMMITTED_TRIANGLE_HIT) visF3 = 0.0;
            f3Observed = true;
        } else if (f3Path == 2u && reused) {
            // HALF, non-traced pixel with a valid reconstruction: the neighbourhood's own reused-
            // sample visibility stands in for a ray. reused still gates this -- THIS frame's own
            // fresh, already-visibility-proven candidate needs no substitute either way.
            visF3 = rec.v3;
        }

        // ---- U1 (2.10 E): THE HALF-RESOLUTION HISTORY WRITE -- ONE TEXEL PER 2x2 BLOCK, PER FRAME ----
        // Gated on THREE things: gAverHistoryWrite (W6/M5 -- a blended-replay fragment must not
        // overwrite the opaque surface's own history, same reasoning as every other gated write this
        // task adds), halfBound (Half must actually be running with its pair bound this frame), and
        // tracedPx (only the 2x2 block's own traced pixel has a real F2/F3 observation to store this
        // frame -- see giVisTracedPixel's own comment: exactly one full-resolution pixel per block
        // writes, so a fallback-traced pixel elsewhere in the block does NOT also write here and race
        // the phase pixel's own write).
        //
        // h: the SAME motion-discounted EMA shape rtShadowTemporal's own history write uses (voxi_rt.
        // hlsli, 0.9/0.5 pair) -- AVER_GI_VIS_HIST_WEIGHT at rest, falling to 0.5 as rec.motionPx
        // (computed once, inside giVisReconstruct, and reused here rather than re-derived) approaches
        // the same 32px knee this file's spatial-reuse motion discount already uses.
        //
        // r/g/b: each channel is "this frame's fresh observation, blended toward the reconstructed
        // history when one exists" for an OBSERVED ray (f2Observed/f3Observed true -- this frame
        // actually traced F2 or F3), or "carry the reconstructed history forward unchanged" when this
        // frame did not observe that ray but a reconstruction was available (rec.valid), or the
        // honest prior (visF3's own default 1.0, "nothing occluding yet"; 0.0 for the two luminance
        // channels, "nothing traced yet") when NEITHER a fresh observation NOR history exists -- the
        // first frame this texel is ever written. f3Observed/f2Observed and rec.valid are never both
        // false while f2Path/f3Path == 3u traced this frame (a trace always sets its own Observed
        // flag), so this only ever falls to the "neither" case on a genuinely first write.
        if (gAverHistoryWrite && halfBound && tracedPx) {
            const float h = lerp(AVER_GI_VIS_HIST_WEIGHT, 0.5, saturate(rec.motionPx / 32.0));
            const float r = f3Observed ? (rec.valid ? lerp(visF3, rec.v3, h) : visF3)
                                        : (rec.valid ? rec.v3 : 1.0);
            const float g = f2Observed ? (rec.valid ? lerp(f2LumTraced, rec.g, h) : f2LumTraced)
                                        : (rec.valid ? rec.g : 0.0);
            const float b = f2Observed ? (rec.valid ? lerp(f2LumSky, rec.b, h) : f2LumSky)
                                        : (rec.valid ? rec.b : 0.0);
            gGiVisHistOut[pixelPos >> 1] = float4(r, g, b, 1.0);
        }

        // result.radiance * result.weightSum is RTXDI's own finalised estimator (weightSum already
        // folds in 1/pdf and the resampling normalisation -- RTXDI_FinalizeGIResampling is what
        // produces it). But RTXDI's contract leaves the REST of the integrand to the caller: for a
        // Lambertian receiver that is cosR/PI, and cosR is exactly the factor that cancels the
        // 1/cosTheta baked into weightSum by the cosine-weighted sample giTraceInitialCandidate drew
        // (see its own comment on where that magnitude comes from) -- dropping it is what let the
        // estimator diverge toward the tangent plane. Also folded in here: gVoxelParams.y
        // (giIntensity), which coneTracedIndirect already applies at its own `sum.rgb *
        // gVoxelParams.y` and this path never did, so Settings > Rendering > GI intensity (and
        // RENDER.GIINTENSITY) silently did nothing whenever giMode selected ReSTIR over the cone
        // gather. Intensity is applied BEFORE the clamp below, matching coneTracedIndirect's order,
        // so the two estimators stay comparable at the same intensity setting. visF3 (F3/R3, just
        // above) is the one factor here that is not part of RTXDI's own finalised estimator: 1.0
        // unless this frame's visibility ray found an occluder, in which case this pixel's own shaded
        // value is zeroed without touching the stored reservoir.
        const float3 est = result.radiance * (cosR * result.weightSum / PI) * gVoxelParams.y * visF3;

        // HLSL's max is `x > y ? x : y`, and every comparison against NaN is false -- so
        // max(NaN, 0.0) silently returns 0.0, not NaN. An unguarded NaN estimator (a near-zero
        // Jacobian, a reservoir aged past a disocclusion) would have become PURE BLACK with nothing
        // in the log to say why: the SAME SHAPE as this codebase's own acesTonemap negative-radiance
        // guard (color.hlsli's `x = max(x, 0.0)`, added by ded8784a) -- both hide a bad value behind
        // a comparison instead of an obvious break, and BOTH now resolve to confident BLACK, not the
        // WHITE an older comment here once claimed (acesTonemap floored negative/NaN input to 0.0,
        // not 1.0, by the time this guard was written -- see color.hlsli's own history). Zero is
        // still the right answer for a broken sample; the isnan/isinf check just makes that a
        // decision instead of an accident of comparison semantics.
        giPoisonEstHit = any(isnan(est)) || any(isinf(est));
        // THE NEW GUARD: not "is this broken", "is this about to be PAINTED SOLID WHITE by a ceiling
        // that exists to hide exactly this". A value can be perfectly finite and still saturate
        // acesTonemap (flat white by roughly x = 4-5, per its own comment) once clamped to
        // AVER_VOX_MAXRAD -- see Settings::giRadianceCeiling's own comment for the full diagnosis.
        // Computed against the SAME non-negative value the clamp below actually clamps (estNonNeg),
        // not the raw `est`, so a legitimately negative-then-floored component cannot register as a
        // false ceiling hit; skipped when giPoisonEstHit already fired above -- a non-finite value is
        // never "above the ceiling", it is a different failure the guard above already owns.
        const float3 estNonNeg = max(est, 0.0);
        giPoisonEstCeilHit = !giPoisonEstHit && any(estNonNeg > AVER_VOX_MAXRAD);
        outDiffuse = giPoisonEstHit ? float3(0.0, 0.0, 0.0)
                                     : min(estNonNeg, AVER_VOX_MAXRAD);
    }

    // ---- hand this frame's estimate to NRD, and take back last frame's ----
    //
    // THE WRITE IS THE RAW PER-PIXEL ESTIMATE, never the denoised value read below. Feeding a
    // filter its own output is the IIR trap the sky-occlusion history write documents at length,
    // and here it would also fight REBLUR's own temporal accumulation, which is the entire job of
    // the permanent pool NRD keeps for this signal.
    //
    // BOTH CHANNELS ARE ENCODED TO REBLUR'S CONTRACT HERE, and neither half of that contract fails
    // loudly if it is skipped -- which is why this was wrong in two independent ways until NRD's own
    // REBLUR_FrontEnd_PackRadianceAndNormHitDist was read rather than assumed about. NRD's pack and
    // unpack helpers are things the PRODUCER and the CONSUMER call; no NRD pass applies them for you.
    //
    // ALPHA: NORMALISED BY REBLUR'S OWN DIVISOR, NOT BY giMaxDistance. This used to divide by
    // giMaxDistance (4000cm by default) on the reasoning that one hitDistParams could then describe
    // both this signal and the sky-occlusion one. It cannot: REBLUR reconstructs a real distance from
    // this channel by multiplying back by (A + |viewZ|*B) * lerp(C, 1, smc), so a value normalised by
    // anything else is a distance REBLUR reads as some other number -- and it sizes its blur kernel,
    // its disocclusion test and its variance estimate from exactly that. A 40m divisor against a true
    // divisor of ~4m made every bounce read as point-blank, which is a denoiser that cannot converge
    // no matter what its anti-firefly setting says. roughness is 1 for a purely diffuse signal, and
    // NRD's smc curve is 1.0 there, so lerp(C, 1, smc) collapses to 1 and C drops out of this end of
    // the mirror -- it still has to be set on the C++ side, where NRD applies it itself.
    //
    // RGB: YCoCg, NOT LINEAR RGB. REBLUR_Config.hlsli sets REBLUR_USE_YCOCG 1 and the front-end pack
    // converts unconditionally, so REBLUR filters chroma in that basis; handing it linear RGB is not a
    // format error, it is a confidently wrong colour. The inverse is applied where gNrdGi is read,
    // just below -- the two are a matched pair and neither is correct alone.
    // ACTIVE PIXELS WRITE NRD'S INPUT PACKED; SKIPPED PIXELS DO NOT WRITE IT AT ALL -- half-rate GI's
    // whole contract with REBLUR (see this file's AVER_GI_CHECKERBOARD header comment) is that the
    // checkerboard's OTHER half is never touched here, only reconstructed by the denoiser itself.
    // giNrdInPos, not pixelPos, is what actually changes: NRD's BLACK/WHITE checkerboard packs
    // IN_DIFF_RADIANCE_HITDIST into the LEFT HALF of the input texture, one texel per horizontal pixel
    // PAIR (NRDSettings.h, CheckerboardMode), so the traced half of every (2k, 2k+1) pair writes the SAME texel -- the
    // `!gGiCbSkip` gate above is exactly what keeps that from racing (only one of the pair is ever
    // active). With the macro at 0 and bit 17 clear (every frame but a checkerboarded one), this is the
    // plain `if (gGiRestirParams.x > 0.5)` it replaced, writing at pixelPos.
    bool  giNrdInWrite = gGiRestirParams.x > 0.5;
    uint2 giNrdInPos   = pixelPos;
#if AVER_GI_CHECKERBOARD
    giNrdInWrite = giNrdInWrite && !gGiCbSkip;   // NRD reads only the traced half
    giNrdInPos.x >>= 1;                          // NRD's packed left-half layout
#else
    // EVERY OTHER WRITER FOLLOWS THE PACKING ON A FRAME CSRdGi PACKED. After a checkerboard dispatch,
    // VoxiRenderer::recordStagedRayDriven leaves bit 17 of gViewParams.w set for the rest of the frame,
    // parity in bit 16. The writer that matters is PSMainVoxi's blended replay of an opaque-material
    // draw (gAverHistoryWrite stays true for it): unpacked, its write would land on the packed texel of
    // an unrelated pixel at twice its x. Packed, it replaces its own pair's input exactly as it
    // replaces its own pixel's at full rate. Zero on every other frame (prePass resets the field), and
    // the staged stages' row pitch never reaches bit 16.
    const uint giNrdPackWord = (uint)gViewParams.w;
    if ((giNrdPackWord & 0x20000u) != 0u) {
        giNrdInWrite = giNrdInWrite && ((pixelPos.x ^ pixelPos.y ^ (giNrdPackWord >> 16)) & 1u) == 0u;
        giNrdInPos.x >>= 1;
    }
#endif
    if (giNrdInWrite) {
        const float hitDistNorm = AVER_NRD_HITDIST_A + abs(curLinearDepth) * AVER_NRD_HITDIST_B;
        const float hitT = RTXDI_IsValidGIReservoir(result)
                         ? saturate(length(result.position - wpos) / max(hitDistNorm, 1e-4))
                         : 1.0;   // nothing found: "the ray went the whole way", as the sky ray encodes it
        // _NRD_LinearToYCoCg, transcribed (see this block's comment for why it is transcribed and
        // not included). Y is the luminance REBLUR accumulates; Co/Cg are signed, which is why the
        // target is RGBA16F and not a UNORM format.
        const float3 ycocg = float3(dot(outDiffuse, float3( 0.25, 0.5,  0.25)),
                                    dot(outDiffuse, float3( 0.5,  0.0, -0.5 )),
                                    dot(outDiffuse, float3(-0.25, 0.5, -0.25)));
        // W6/M5: GATED ON gAverHistoryWrite, ON BY DEFAULT -- the NRD INPUT, not merely a history
        // texture, and the most visible half of C9's finding: an unguarded write here handed NRD's
        // permanent accumulation pool the PANE's own GI estimate for a pixel the opaque surface
        // behind it also claims, and REBLUR then denoises across both without any way to tell them
        // apart. voxi.legacyBlendedHistoryWrite (gAmbientParams.z bit 32) restores the old
        // unconditional write, byte-identical, for A/B.
        if (gAverHistoryWrite) gGiRadianceOut[giNrdInPos] = float4(ycocg, hitT);
    }

    // LAST FRAME'S DENOISED ANSWER REPLACES THIS FRAME'S RAW ONE. One frame of lag, which is what
    // every temporal consumer in this file already carries (the pass runs in beginShadowHistory,
    // before the pixel shader that produces the input has run), and it is the lag NRD is built to
    // be fed. Zero dimensions means the pass did not run this frame -- no NRD in the build, a
    // backend that refuses its register spaces, no G-buffer, or MSAA above 1x -- and then the raw
    // ReSTIR estimate stands, noisy but correct.
    uint gw = 0, gh = 0;
    gNrdGi.GetDimensions(gw, gh);
    // W6/M5: `gAverHistoryWrite &&` LEADS THIS TEST -- the sibling half of C9's finding (this file's
    // own gGiRadianceOut write above is the other half): a blended-replay fragment must not read back
    // the OPAQUE surface's denoised answer as if it were its own, the exact "a pane is handed the
    // denoised GI of the surface behind it" mis-attribution. Skipping the readback leaves `outDiffuse`
    // at the raw estimate computed just above -- this fragment's own, noisy but correct -- and its own
    // write above is skipped by the same gate, so a blended fragment neither reads nor writes the
    // opaque surface's NRD state.
    // ---- READ WHERE THIS SURFACE WAS LAST FRAME, NOT AT THIS FRAME'S PIXEL ----
    //
    // gNrdGi is laid out on LAST frame's pixel grid: NRD filtered what last frame's pass wrote at
    // last frame's pixel positions. Loading it at this frame's pixelPos is right only while the camera
    // is still. In motion it handed each pixel the GI of whatever last frame drew there -- a whole
    // frame of camera movement away. Every other temporal consumer in this file reprojects
    // (rtReprojectHistory, rtReprojectAo); this one did not.
    //
    // MEASURED, flying 30 cm/frame down PTTest's gallery against a settled frame at the same pose:
    // the old read's error is STRUCTURED -- a bright GI leak down a door frame, bands along a beam --
    // and this read's is not. Its fine-scale difference is higher (0.84 against 0.28 codes, below
    // one display level either way, cause not isolated); its mean is 0.77 codes darker where the old
    // one was 0.53 brighter. A still frame is identical (MAD 0.01).
    //
    // THE SAME RECIPE AS THE TEMPORAL RESAMPLING ABOVE (gPrevViewProj, gSceneViewport), and validated
    // against the surface ReSTIR itself kept for last frame (RAB_GetGBufferSurface): a texel counts
    // only if last frame's surface there lies on this pixel's tangent plane and faces the same way.
    //
    // NO TAP MATCHES = a genuine disocclusion (the strip a turn reveals, the wall behind a column
    // being passed): NRD never saw this surface, so no filtered answer for it exists. Such a pixel
    // keeps the OLD unreprojected read -- smooth, but some neighbouring surface's -- so it is never
    // worse than before this existed. Its own raw estimate was tried and is far worse: at one sample
    // it paints the revealed strip black with sparse bright dots.
    //
    // Legacy bit 64 (voxi.legacyNrdReadback) skips the reprojection entirely, the pre-fix read, A/B.
    //
    // BILINEAR: the four taps around the exact reprojected point, each kept only if last frame's
    // surface there is this one, renormalised -- the shape a TAA history fetch takes. (A nearest-
    // texel 3x3 search measured the same, 0.845 against 0.841; bilinear is kept as the principled
    // resample.) At rest the point is this pixel's own centre and the weights collapse onto it:
    // measured identical still frames, MAD 0.01 ray-driven and 0.06 raster (run-to-run noise).
    const bool nrdReproject = gGiRestirParams.y > 0.5 &&   // a previous frame exists to reproject into
                              ((uint)gAmbientParams.z & 64u) == 0u;
    float3 nrdYcocg = 0.0;
    float  nrdWsum  = 0.0;
    // gAverHistoryWrite: a blended fragment never reads NRD back (the gate below), so it skips the
    // four surface-history lookups too.
    if (gAverHistoryWrite && nrdReproject && gw > 0u && gh > 0u) {
        const float4 nrdPrevClip = mul(float4(wpos, 1.0), gPrevViewProj);
        if (nrdPrevClip.w > 1e-4) {
            const float3 nrdPrevNdc = nrdPrevClip.xyz / nrdPrevClip.w;
            const float2 nrdPrevPx = gSceneViewport.xy +
                float2(nrdPrevNdc.x * 0.5 + 0.5, 0.5 - nrdPrevNdc.y * 0.5) * gSceneViewport.zw;
            const float2 nrdF    = nrdPrevPx - 0.5;   // texel CENTRES sit at +0.5
            const int2   nrdBase = int2(floor(nrdF));
            const float2 nrdFrac = nrdF - float2(nrdBase);
            const float  nrdPlaneTol = max(curLinearDepth, 1.0) * 0.02 + 1.0;   // cm
            [unroll] for (uint k = 0u; k < 4u; ++k) {
                const int2  off = int2(k & 1u, k >> 1u);
                const float w   = (off.x != 0 ? nrdFrac.x : 1.0 - nrdFrac.x) *
                                  (off.y != 0 ? nrdFrac.y : 1.0 - nrdFrac.y);
                if (w <= 0.0) continue;
                const int2 tap = nrdBase + off;
                const RAB_Surface prevSurf = RAB_GetGBufferSurface(tap, true);   // bounds-checked
                if (!prevSurf.valid) continue;
                if (abs(dot(prevSurf.worldPos - wpos, N)) > nrdPlaneTol) continue;
                if (dot(prevSurf.normal, N) < 0.9) continue;
                nrdYcocg += gNrdGi.Load(int3(tap, 0)).rgb * w;
                nrdWsum  += w;
            }
        }
    }
    if (gAverHistoryWrite && gw > 0u && gh > 0u) {
        // _NRD_YCoCgToLinear, the matching half of the write above. REBLUR hands back what it
        // filtered, in the basis it filtered it in; NRD's own back-end unpack is this same transform
        // and also ends in a max against zero, because the chroma round trip can put a channel
        // slightly negative and negative radiance reads BRIGHT once it reaches the tonemap.
        // LINEAR, so blending the four taps in YCoCg and decoding once equals decoding each.
        const float3 y = nrdWsum > 1e-3 ? nrdYcocg / nrdWsum
                                        : gNrdGi.Load(int3(pixelPos, 0)).rgb;   // disocclusion / legacy
        const float  t = y.x - y.z;
        const float3 decoded = float3(t + y.y, y.x + y.z, t - y.y);
        // ---- THE SAME GUARD THE RAW ESTIMATOR ABOVE ALREADY HAS, NOW APPLIED HERE TOO ----
        //
        // Before this, decoding NRD's own output only ever floored the negative-chroma-round-trip
        // case (max(..., 0.0), comment above) -- it trusted a THIRD-PARTY filter's output to be
        // finite and bounded, which is exactly the trust `outDiffuse`'s own raw estimator (see its
        // isnan/isinf check ~20 lines up, and the sibling comment on HLSL's NaN-is-always-false
        // comparison semantics) refuses to extend to ITS OWN inputs. A denoiser is not exempt from
        // that: REBLUR's temporal accumulation and variance-driven history clamp are third-party
        // maths this file does not control, and this file's own house rule (this task's brief, and
        // aver-negative-radiance-reads-bright's history elsewhere in this tree) is that a value which
        // can go non-finite or unbounded must be caught at the point it is CONSUMED, not assumed safe
        // because its producer is trusted -- acesTonemap floors negative/NaN input at zero as of
        // ded8784a (2026-08-31, predating this guard), so a stray NaN/Inf/huge finite value in this
        // exact spot renders as confident BLACK, not as visible corruption. (An older version of this
        // comment said WHITE, describing acesTonemap's behaviour before that fix -- corrected here.)
        //
        // THIS CODEPATH ONLY WENT LIVE RECENTLY: NRD was never created at all for a project whose
        // ray-tracing tier had not just changed (98b2b9a9 "NRD was never created unless SKY
        // OCCLUSION was on"), so for exactly this project's configuration `gw/gh` used to always read
        // 0x0 and this branch was dead code -- every prior round of fixes to THIS function (the
        // cosR floor, the Jacobian symmetry fix, the corpse-reservoir guard, all documented above)
        // was necessarily exercised and measured against the RAW path alone, never this one. Closing
        // the asymmetry is cheap insurance now that the branch is reachable for real: zero cost when
        // `decoded` is what it always is (finite, small), and it turns a would-be white-out into the
        // same "zero is the honest answer for a broken sample" the raw path already chose, rather
        // than a screen-filling flash with nothing in the log to explain it.
        giPoisonNrdHit = any(isnan(decoded)) || any(isinf(decoded));
        // THE NRD-SIDE TWIN OF giPoisonEstCeilHit above, same reasoning: a finite REBLUR readback
        // that is nonetheless above the ceiling paints solid white once tonemapped, invisible to the
        // isnan/isinf guard directly above. Same shape -- checked against the clamp's own
        // already-floored input, skipped when giPoisonNrdHit already fired.
        const float3 decodedNonNeg = max(decoded, 0.0);
        giPoisonNrdCeilHit = !giPoisonNrdHit && any(decodedNonNeg > AVER_VOX_MAXRAD);
        outDiffuse = giPoisonNrdHit
                   ? float3(0.0, 0.0, 0.0)
                   : min(decodedNonNeg, AVER_VOX_MAXRAD);
    }

    // ---- POISON DEBUG VIEW (voxi.giPoisonView / gGiRestirParams.w) -- OVERRIDE 2/3's by-hand tool ----
    //
    // Default off, and cheap when off: one dynamic branch on a cbuffer float already being read this
    // frame, no new binding, no buffer, no readback. While on, this REPLACES the final indirect-
    // diffuse colour at any pixel where one of this file's non-finite guards -- OR, now, one of its
    // two CEILING guards -- fired THIS frame, in an unmistakable, scene-lighting-cannot-produce-this
    // colour PER GUARD -- so the user can see both WHERE a guard is catching something and WHICH one.
    //
    // TWO KINDS OF GUARD, AND THE PRECEDENCE ORDER SAYS SO: the five NON-FINITE guards (magenta
    // through blue below) catch actual corruption -- a NaN/Inf that should never have existed. The
    // two CEILING guards (red, green) catch something else entirely: a perfectly FINITE value that is
    // merely large enough to saturate AVER_VOX_MAXRAD and, downstream, acesTonemap -- which is the
    // very white-patch symptom this whole task diagnoses, not a bug in this file. Deliberately
    // NEITHER WHITE (that is the symptom, not a diagnosis) and both checked LAST: a non-finite guard
    // always outranks a mere ceiling hit at the same pixel, because corruption is the more urgent
    // thing to see and because giPoisonEstCeilHit/giPoisonNrdCeilHit are already false whenever their
    // non-finite sibling fired (see each guard's own comment) -- so in practice the two families never
    // actually compete for the same pixel; the ordering below is what happens if that ever changes.
    // Priority order below (checked highest first) matters only for the rare pixel where more than one
    // fires in the same frame; each is otherwise independent.
    //
    // LEGEND (also in resetgihistory's neighbourhood in buildConsoleCatalog, and in `help`):
    //   MAGENTA (1,0,1)   -- the store-time reservoir guard (giPoisonStoreHit, Part 2.4): a poisoned
    //                        reservoir was about to be written into CROSS-FRAME history and was
    //                        emptied instead. THE ONE THAT MATTERS MOST -- this is the circuit
    //                        breaker.
    //   CYAN    (0,1,1)   -- the candidate-radiance clamp guard (nonFiniteCandidate, Part 2.1): a
    //                        fresh candidate's own shaded/sky radiance was non-finite before it ever
    //                        reached a reservoir.
    //   YELLOW  (1,1,0)   -- the target-pdf guard (gGiPoisonPdfHit, Part 2.3):
    //                        RAB_GetGISampleTargetPdfForSurface computed a non-finite importance
    //                        weight for some candidate/neighbour this pixel combined this frame.
    //   ORANGE  (1,0.5,0) -- the pre-existing final-estimate guard (giPoisonEstHit, ~50 lines up):
    //                        this pixel's own finalised weightSum*radiance was non-finite.
    //   BLUE    (0,0,1)   -- 301418ec's NRD-readback guard (giPoisonNrdHit, just above): REBLUR
    //                        handed back a non-finite value on the denoised read-back path.
    //   RED     (1,0,0)   -- NEW: the raw estimate hit the radiance CEILING (giPoisonEstCeilHit,
    //                        ~50 lines up) -- finite, but AVER_VOX_MAXRAD clamped it, which is what
    //                        paints solid white once tonemapped. THIS is where a white patch's own
    //                        cause is diagnosed, not merely its symptom hidden.
    //   GREEN   (0,1,0)   -- NEW: the NRD-denoised readback hit the same ceiling (giPoisonNrdCeilHit,
    //                        just above) -- REBLUR's own accumulation can push an already-hot value
    //                        higher still before this pixel ever sees it.
    //
    //   AN EIGHTH COLOUR LIVES OUTSIDE THIS FUNCTION (B1/F5): voxi.hlsl's PSMainVoxi/PSRayDriven paint
    //   VIOLET (0.55,0,1) over the ray-traced SPECULAR indirect term's own ceiling hit, gated by this
    //   same gGiRestirParams.w flag but NOT by giMode -- it fires under either diffuse estimator,
    //   since the specular ray this checks has nothing to do with which of giRestirIndirect/
    //   coneTracedIndirect answered the diffuse bounce. It is applied at each pixel shader's own final
    //   output, AFTER this function has already returned, so it cannot interfere with the seven-way
    //   precedence immediately below -- see aver_IsGiRestirPoisonColour's own comment (voxi.hlsl, just
    //   above PSMainVoxi) for how it stays subordinate to whichever of these seven colours this
    //   function itself returns.
    if (gGiRestirParams.w > 0.5) {
        if (giPoisonStoreHit)     return float3(1.0, 0.0, 1.0);
        if (nonFiniteCandidate)   return float3(0.0, 1.0, 1.0);
        if (gGiPoisonPdfHit)      return float3(1.0, 1.0, 0.0);
        if (giPoisonEstHit)       return float3(1.0, 0.5, 0.0);
        if (giPoisonNrdHit)       return float3(0.0, 0.0, 1.0);
        if (giPoisonEstCeilHit)   return float3(1.0, 0.0, 0.0);
        if (giPoisonNrdCeilHit)   return float3(0.0, 1.0, 0.0);
    }

    // ---- U1's PATH DEBUG VIEW (voxi.giVisPathView, gAmbientParams.w bit 64; 2.10 I) ----
    //
    // A SEPARATE VIEW FROM THE POISON ONE ABOVE, gated `gGiRestirParams.w <= 0.5` so the two can
    // never fight over the same return -- the poison view's own block above returns first whenever
    // both are on, so this is simply unreachable then; deliberate, not a bug, since the poison view
    // answers "is anything broken" and this one answers "which path did F2 take", and the two
    // questions are more useful asked one at a time on the same pixel than interleaved.
    //
    // PAINTS F2's PATH. F3 follows the same path except under legacy bit 8 (gAmbientParams.z & 8u),
    // which this view does not separately colour -- F2 and F3 share one path number except where a
    // legacy bit singles one of them out (the decode block, giRestirIndirect's own top), and by far
    // the more expensive of the two rays is the one this view exists to make visible.
    //
    // LIKE THE POISON VIEW, this replaces indirect diffuse AFTER the NRD write above, so it never
    // enters history and never feeds back into next frame's reprojection -- a debug paint that itself
    // corrupted the signal it exists to diagnose would defeat the point.
    if (gGiRestirParams.w <= 0.5 && ((uint)gAmbientParams.w & 64u) != 0u) {
        if (f2Path == 0u) return float3(1.0, 1.0, 0.0);                 // YELLOW: no ray (mode 0 or legacy bit 4)
        if (f2Path == 1u) return float3(0.0, 1.0, 0.0);                 // GREEN: reconstructed (voxel cone)
        if (f2Path == 2u) return float3(0.0, 0.0, 1.0);                 // BLUE: half-res reconstruction
        return (halfBound && !tracedPx) ? float3(1.0, 0.0, 0.0)        // RED: half-res fallback, traced
                                        : float3(1.0, 1.0, 1.0);        // WHITE: traced (Full, or Half's phase pixel)
    }

    return outDiffuse;
}
