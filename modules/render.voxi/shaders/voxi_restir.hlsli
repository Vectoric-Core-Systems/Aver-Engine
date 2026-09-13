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
    return averShadowLum(sampleRadiance) * cosR;
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

// ---- the initial candidate: ONE cosine ray, traced and shaded through machinery this file already has ----
// Traces off (wpos, N) along a cosine-weighted hemisphere direction and shades whatever it hits
// through the SAME RayQuery + flat geometry table + gRtMaterials + averShadeDirect machinery
// rtReflection and PSRayDriven already use for a mirror ray and a primary ray respectively -- this
// is that toolkit pointed along a diffuse-importance direction instead of a specular one.
//
// Returns false on a miss: nothing a reservoir can reproject. The caller then hands RTXDI an EMPTY
// initial reservoir for this frame and lets temporal resampling (RTXDI_IsValidGIReservoir gates on
// M != 0) carry the pixel from history alone -- exactly the situation that function already handles.
bool giTraceInitialCandidate(float3 wpos, float3 N, float2 pixel, float frameJitter,
                             out float3 samplePos, out float3 sampleNormal, out float3 sampleRadiance) {
    samplePos = sampleNormal = sampleRadiance = 0.0;

    // Cosine-weighted hemisphere sample (Malley's method: a uniform point on the unit disc, lifted
    // onto the hemisphere) -- the standard importance sample for a Lambertian receiver, so its own
    // pdf (cosTheta/PI) is what the caller's RTXDI_MakeGIReservoir divides out. rtDiscSample/rtHash
    // are the SAME per-pixel-rotated low-discrepancy sequence rtReflection's own cone sample uses,
    // jittered per frame the same way (frameJitter) rather than frozen per pixel -- see rtReflection
    // for what a frozen-per-pixel sample cost the reflection ray before that was fixed.
    const float2 xi = rtDiscSample(0, rtHash(pixel) * 6.2831853 + frameJitter);
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
    // indirect term. coneTracedIndirect gathers it (the volume is injected with sky), so switching
    // Indirect diffuse from Voxel cones to ReSTIR dropped that entire contribution on the floor --
    // exactly the surfaces the user reports going near-black while sunlit ones stay correct.
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
        sampleRadiance = clamp(averSkyRadianceCheap(dir) * gAmbient.r, 0.0, AVER_VOX_MAXRAD);
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

    // THE SAME UNTEXTURED SURFACE PSRayDriven BUILDS WHEN AVER_RT_BINDLESS ISN'T COMPILED IN --
    // per-instance factor times per-material factor, no maps sampled. A second bounce is exactly
    // where that approximation is cheapest to accept: RTXDI resamples this radiance over many
    // frames (and, once a spatial pass lands, neighbours), so a flat-shaded hit converges toward a
    // textured one's LOW-FREQUENCY answer, which is the only part indirect light hands back to the
    // FIRST surface anyway.
    AverSurface s = (AverSurface)0;
    s.N           = hitN;
    s.V           = -dir;
    s.H           = normalize(s.V + L);
    s.albedo      = inst.albedo * mat.baseColorFactor.rgb;
    s.metallic    = saturate(inst.metallic * mat.metallicFactor);
    s.rough       = clamp(inst.roughness * mat.roughnessFactor, 0.045, 1.0);
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
    // it contributes almost NO bounce light and reads as a black hole for GI. That is a real gap and
    // it is still open. Closing it needs a term weighted by metalness AND scaled/occluded the way the
    // diffuse ambient is -- not an unconditional sky specular on every surface, which is what this
    // reverts. See aver-ambient-overbright-open-vs-enclosed for why unoccluded ambient in an interior
    // is already over-bright here before anything is added to it.
    radiance += s.kdAlbedo * averSkyIrradiance(s.N) * gAmbient.r;

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
    sampleRadiance = clamp(radiance, 0.0, AVER_VOX_MAXRAD);
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
// of whichever diffuse estimator is chosen, which is the tier this gap matters least at.
float3 giRestirIndirect(float3 wpos, float3 N, float curLinearDepth, float2 pixel, uint frameIdx,
                        out float ao) {
    ao = 1.0;
    const uint2 pixelPos = uint2(pixel);
    const RTXDI_ReservoirBufferParameters resParams = giReservoirBufferParams();
    const float frameJitter = (float)frameIdx * 2.39996323;

    float3 samplePos, sampleNormal, sampleRadiance;
    RTXDI_GIReservoir initial = RTXDI_EmptyGIReservoir();
    if (giTraceInitialCandidate(wpos, N, pixel, frameJitter, samplePos, sampleNormal, sampleRadiance)) {
        const float cosTheta = saturate(dot(normalize(samplePos - wpos), N));
        // THE SAME FLOOR, because this is the gate that actually decides what gets stored. The
        // recomputed cosTheta is the sampled one up to precision (dir is built from it and samplePos
        // lies along dir), so a `> 0.0` here was strictly weaker than the reject inside
        // giTraceInitialCandidate and quietly let the 1/cos blow-up through anyway.
        if (cosTheta > AVER_GI_MIN_COS)
            initial = RTXDI_MakeGIReservoir(samplePos, sampleNormal, sampleRadiance, cosTheta / PI);
    }

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
        stparams.depthThreshold        = 0.1;
        stparams.normalThreshold       = 0.5;
        // ---- 8, NOT 20, AND THE REASON IS THE STABILITY MARGIN RATHER THAN LAG ----
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
        // through the Jacobian at all. maxHistoryLength is left at 8 anyway rather than reopened
        // here: revisiting it is a separate change from the two this task scoped (the Jacobian fix
        // and the motion discount below), and 8 costs nothing now that it is no longer load-bearing
        // for stability -- it just means less history than the number could safely support.
        stparams.maxHistoryLength      = 8;
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

    // KNOWN LIMITATION, not fixed here: a pixel covered by TRANSLUCENT geometry is written to THIS
    // reservoir slot TWICE in one frame. PSRayDriven writes every pixel first; the blended glass
    // replay is PSMainVoxi drawn over the same pixels, and the two share one compiled shader binary
    // differing only in blend state and depth-write, so no #define or per-pass constant can tell
    // them apart from inside this function. It degrades gracefully rather than silently: next frame
    // the OPAQUE surface reprojects into a slot that still describes the glass hit,
    // RTXDI_IsValidNeighbor's depth/normal test rejects the mismatch (glass and whatever sits behind
    // it are rarely coplanar), and that pixel falls back to a fresh candidate exactly as if no
    // history existed at all. The cost is LOST TEMPORAL REUSE behind glass -- one extra frame of
    // noise where reuse should have carried over -- not wrong light.
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
    if (result.weightSum <= 0.0) result = RTXDI_EmptyGIReservoir();
    RTXDI_StoreGIReservoir(result, resParams, pixelPos, writeSlice);

    // NEXT FRAME'S "previous surface" -- see gGiSurfPosHist/gGiSurfNrmHist's own declaration for
    // the two-texture format and the 0-packed-normal sentinel RAB_GetGBufferSurface tests for
    // "nothing written here". Nudged off exactly zero so a legitimately-packed normal never
    // collides with that sentinel.
    const uint packedN = RTXDI_EncodeNormalizedVectorToSnorm2x16(N);
    gGiSurfPosHistOut[pixelPos] = wpos.xy;
    gGiSurfNrmHistOut[pixelPos] = float2(wpos.z, asfloat(packedN == 0u ? 1u : packedN));

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
        // so the two estimators stay comparable at the same intensity setting.
        const float3 est = result.radiance * (cosR * result.weightSum / PI) * gVoxelParams.y;

        // HLSL's max is `x > y ? x : y`, and every comparison against NaN is false -- so
        // max(NaN, 0.0) silently returns 0.0, not NaN. An unguarded NaN estimator (a near-zero
        // Jacobian, a reservoir aged past a disocclusion) would have become PURE BLACK with nothing
        // in the log to say why: the mirror image of this codebase's own acesTonemap(-1)==1.0 trap,
        // where the bug hides behind a comparison instead of behind the tonemap. Zero is still the
        // right answer for a broken sample; the isnan/isinf check just makes that a decision instead
        // of an accident of comparison semantics.
        outDiffuse = any(isnan(est)) || any(isinf(est)) ? float3(0.0, 0.0, 0.0)
                                                         : min(max(est, 0.0), AVER_VOX_MAXRAD);
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
    if (gGiRestirParams.x > 0.5) {
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
        gGiRadianceOut[pixelPos] = float4(ycocg, hitT);
    }

    // LAST FRAME'S DENOISED ANSWER REPLACES THIS FRAME'S RAW ONE. One frame of lag, which is what
    // every temporal consumer in this file already carries (the pass runs in beginShadowHistory,
    // before the pixel shader that produces the input has run), and it is the lag NRD is built to
    // be fed. Zero dimensions means the pass did not run this frame -- no NRD in the build, a
    // backend that refuses its register spaces, no G-buffer, or MSAA above 1x -- and then the raw
    // ReSTIR estimate stands, noisy but correct.
    uint gw = 0, gh = 0;
    gNrdGi.GetDimensions(gw, gh);
    if (gw > 0u && gh > 0u) {
        // _NRD_YCoCgToLinear, the matching half of the write above. REBLUR hands back what it
        // filtered, in the basis it filtered it in; NRD's own back-end unpack is this same transform
        // and also ends in a max against zero, because the chroma round trip can put a channel
        // slightly negative and negative radiance reads BRIGHT once it reaches the tonemap.
        const float3 y = gNrdGi.Load(int3(pixelPos, 0)).rgb;
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
        // because its producer is trusted -- acesTonemap(-1) == 1.0 here, so a stray NaN/Inf/huge
        // finite value in this exact spot renders as confident WHITE, not as visible corruption.
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
        outDiffuse = (any(isnan(decoded)) || any(isinf(decoded)))
                   ? float3(0.0, 0.0, 0.0)
                   : min(max(decoded, 0.0), AVER_VOX_MAXRAD);
    }

    return outDiffuse;
}
