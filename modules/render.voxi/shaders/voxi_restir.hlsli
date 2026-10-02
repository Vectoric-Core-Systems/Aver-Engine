// voxi_restir.hlsli -- the RTXDI ReSTIR GI block, extracted whole out of voxi.hlsl (giMode == 1).
//
// HOLDS: the vendored RTXDI includes (GI/ReSTIRGIParameters.h, GI/Reservoir.hlsli,
// Utils/RandomSamplerState.hlsli, GI/SpatioTemporalResampling.hlsli), the reservoir buffer
// (gGiReservoirs u6) with the RTXDI_GI_RESERVOIR_BUFFER/RTXDI_GI_ALLOWED_BIAS_CORRECTION macros
// Reservoir.hlsli needs defined before it, the surface-history textures (gGiSurfPosHist/Out
// t12/u7, gGiSurfNrmHist/Out t13/u8), the RAB_* contract RTXDI's SDK calls back into, and this
// file's own giReservoirBufferParams/giTraceInitialCandidate, ending in giRestirIndirect (the
// entry point everything above supports).
//
// MUST PRECEDE THIS FILE'S #include IN voxi.hlsl: the RT scene/geometry/material decls (gScene
// t2, RtInstance/RtMaterial, gRtInstances/gRtVerts/gRtIndices/gRtMaterials t3/t4/t5/t9 and the
// instance lookup rtLoadInstance/rtPackCommitted over them and the foliage tables t20/t21, bindless
// adapter averRtProceedSolid/averRtSampleSlot/averRtSurfaceUV/averRtUvGrad -- giTraceInitialCandidate
// traces gScene and shades through it); rtHash/rtDiscSample (candidate-direction sampling);
// rdLocalCarriesEmitters (voxi_rt.hlsli, AVER_RD_LAMPS); the VoxiFrame cbuffer fields this file
// reads (gGiRestirParams, gRtHistParams, gVoxelParams, gViewProj, gPrevViewProj) and gNrdGi (t15);
// and the #if AVER_RT guard opened earlier in voxi.hlsl must still be open here -- this file has
// no #if AVER_RT of its own, it's plain text spliced into an already-open conditional.
//
// DEPENDS ON THIS FILE: giRestirIndirect, called from PSMainVoxi/PSRayDriven further down
// voxi.hlsl (after #if AVER_RT closes) -- both call sites must stay textually after the #include.
//
// ORDERING CONTRACT (why this was split out -- get it wrong and it fails at RUNTIME, not build):
// RTXDI_GI_RESERVOIR_BUFFER/RTXDI_GI_ALLOWED_BIAS_CORRECTION must be #defined, and gGiReservoirs
// declared, before "Rtxdi/GI/Reservoir.hlsli" is included (plain macro substitution, not a
// template arg). Every RAB_* symbol (RAB_Surface, the type, most of all) must be fully declared
// before "Rtxdi/GI/SpatioTemporalResampling.hlsli" is included -- getting this wrong once gave
// `unknown type name 'RAB_Surface'`, a RUNTIME failure (zero build errors) that killed
// PSMainVoxi, PSRayDriven, VSky and VSMain alike, since all entry points share one translation
// unit. If this file is subdivided further, this is the ordering that breaks first, and silently.
//
// Moved verbatim out of voxi.hlsl -- nothing below was rewritten in the move.

// Staged ray-driven milestone 4 (Settings::rayDrivenStages == 2): half-rate ReSTIR GI, traced in
// NRD's own checkerboard so REBLUR (GI denoiser index 1) reconstructs the skipped half. Own macro
// rather than an ambient `#ifdef` per use site, so every compile that never sets it (PSMainVoxi,
// PSRayDriven, every other CS stage) takes the untouched branch at each checkerboard site below.
// See CSRdGi (voxi.hlsl) for the one compile that defines this to 1.
#ifndef AVER_GI_CHECKERBOARD
#define AVER_GI_CHECKERBOARD 0
#endif

// GI candidate-trace/resample split (Settings::rayDrivenGiSplit): CSRdGiTrace (voxi.hlsl) traces
// and shades giTraceInitialCandidate's ray in its own dispatch and stores every out param in
// gRdGiCand (below); CSRdGi's AVER_GI_SPLIT=1 compile reads that record back inside
// giRestirIndirect instead of tracing again. Same guard convention as AVER_GI_CHECKERBOARD above:
// every other compile sees 0 and takes the original, byte-identical branch at this file's one
// AVER_GI_SPLIT site (in giRestirIndirect). See CSRdGi (voxi.hlsl) for the compile that sets it 1.
#ifndef AVER_GI_SPLIT
#define AVER_GI_SPLIT 0
#endif

// ================= RTXDI ReSTIR GI (Settings::giMode == 1) =================
//
// USES THE VENDORED SDK (third_party/rtxdi), not a hand-rolled reservoir update: everything that
// combines candidates (RTXDI_MakeGIReservoir, RTXDI_GISpatioTemporalResampling) calls into
// Rtxdi/GI/Reservoir.hlsli and SpatioTemporalResampling.hlsli. This file supplies only what RTXDI
// cannot know on its own -- how to TRACE a candidate and shade what it hits
// (giTraceInitialCandidate), and the RAB_* answers about a surface (RAB_GetGBufferSurface and
// neighbours, below). Jacobian, MIS weight, M-cap are RTXDI's, not reimplemented here.
//
// SCOPE: candidate generation + RTXDI SPATIO-TEMPORAL resampling (temporal reuse AND a spatial
// pass) -- NOT the plain RTXDI_GISpatialResampling (vendored, unused): that calls
// RAB_GetGBufferSurface(idx, FALSE) for a neighbour, i.e. THIS frame's surface at a pixel that may
// not have run yet, which a pixel shader cannot answer (this file's RAB_GetGBufferSurface refuses
// by returning "no surface" whenever previousFrame is false). Spatio-temporal instead reads every
// neighbour, spatial taps included, from the PREVIOUS frame (RAB_GetGBufferSurface(idx, true), and
// a sourceBufferIndex that is always the slice NOT being written this frame) -- no read of
// same-frame in-progress data, no race, so it works from this single-pass pixel shader where the
// plain spatial variant cannot.

// Cosine floor for both of this file's candidate gates, and the only bound on a stored
// reservoir's 1/pdf (cost worked out at giTraceInitialCandidate). Named once so the two gates
// cannot drift apart again -- they already had.
#define AVER_GI_MIN_COS 0.05

// Candidate-hit material-map footprint, as tan(cone half-angle) x ray length. Wide on purpose: a
// diffuse bounce only needs the hit's local average colour/metalness, and a coarser mip is cheaper
// and no less right on average. See giTraceInitialCandidate.
#define AVER_GI_HIT_TEX_CONE 0.1

// Occupancy floor (CSResolve's alpha, voxi.hlsl ~3417) below which F2's traced-hit voxel lookup
// treats a trilinear tap as having no usable radiance -- avoids dividing by a near-zero footprint
// and amplifying quantisation noise into a spike.
#define AVER_GI_VOX_MIN_OCC 0.05

// REBLUR hit-distance normalisation constants, MIRRORING aver::render::nrd::Denoiser::ReblurTuning
// (modules/render.nrd/include/aver/render/nrd/NrdDenoiser.hpp), which actually configures the
// denoiser -- REBLUR normalises a hit distance by (A + |viewZ|*B) * lerp(C, 1, smc)
// (NRD.hlsli's _REBLUR_GetHitDistanceNormalization/REBLUR_FrontEnd_GetNormHitDist) and requires the
// producer to divide by the same thing. A is a length in engine units (300cm = NRD's default 3m);
// B/C are unit-free. Restated rather than included (NRD headers are confined to modules/render.nrd
// by vendoring terms) -- change both declarations together.
#define AVER_NRD_HITDIST_A 300.0
#define AVER_NRD_HITDIST_B 0.1
// REBLUR's denoisingRange in cm, MIRRORING kNrdDenoisingRangeCm (VoxiRenderer.cpp, where the NRD
// FrameSettings are filled): REBLUR writes no output texel whose viewZ is past it, so a surface that
// far keeps its raw estimate instead of reading back a texel NRD never wrote. Change both together.
#define AVER_NRD_DENOISING_RANGE 500000.0

// ---- the reservoir buffer Reservoir.hlsli requires defined before it is included ----
// RTXDI_GI_RESERVOIR_BUFFER must already name a declared RWStructuredBuffer<RTXDI_PackedGIReservoir>
// by the time Reservoir.hlsli's bodies are parsed (plain macro substitution, not a template arg),
// so the type must exist first -- pulling just the parameters header gets it without duplicating
// anything (Reservoir.hlsli's own later #include of the same file is a no-op behind its guard).
#include "Rtxdi/GI/ReSTIRGIParameters.h"
RWStructuredBuffer<RTXDI_PackedGIReservoir> gGiReservoirs : register(u6);
#define RTXDI_GI_RESERVOIR_BUFFER gGiReservoirs
// BASIC, not RAY_TRACED: RAY_TRACED costs one more shadow-style visibility ray per pixel per
// temporal resample (RAB_GetTemporalConservativeVisibility, implemented below for completeness)
// on top of the one candidate generation already traces. BASIC is RTXDI's documented
// cheap-but-good default; the #if this feeds (RTXDI/GI/TemporalResampling.hlsli) compiles the
// ray-traced branch out entirely below this setting.
#define RTXDI_GI_ALLOWED_BIAS_CORRECTION RTXDI_BIAS_CORRECTION_BASIC
#include "Rtxdi/GI/Reservoir.hlsli"
// TemporalResampling.hlsli calls RTXDI_GetNextRandom/RTXDI_CalculateJacobian (and takes an
// RTXDI_RandomSamplerState parameter) without including the headers that declare them -- pulled in
// explicitly here rather than relying on Reservoir.hlsli's own chain to have happened to include it.
#include "Rtxdi/Utils/RandomSamplerState.hlsli"
// TemporalResampling.hlsli itself is included at the END of the RAB_ block below -- load-bearing
// order, see that include's own note.

// ---- the previous-frame SURFACE history RAB_GetGBufferSurface(idx, true) reads ----
// ENGINE GAP: RTXDI's temporal resampling needs a previous frame's primary surface (world pos +
// normal) at an arbitrary reprojected pixel; nothing in Voxi carried one before this pair (the
// deferred G-buffer is current-frame-only). AVER_GBUFFER_HISTORY's gGBufNormalHist is a different,
// unfinished attempt at the same gap (declared, used in 3 denoiser tap loops, gated behind a
// define nothing sets to 1): a crease term for spatial shadow/reflection filters, tied to
// the G-buffer being on -- left as-is rather than reused, since this must work whether or not the
// G-buffer is enabled, which would risk the drift its own comment warns about. This is a NEW
// pair, added the way ensureShadowHistory already adds one
// (VoxiRenderer.cpp/.hpp) -- see giSurfPosHist_/giSurfNrmHist_ there for the full reasoning.
//
// TWO RG32Float TEXTURES, NOT ONE FOUR-CHANNEL FLOAT TEXTURE: rhi::Format has no such format
// (only RGBA16F/R32Float/RG32Float are float). gGiSurfPosHist carries xy of world position;
// gGiSurfNrmHist carries z of that position plus the packed normal (octahedral uint from
// RTXDI_EncodeNormalizedVectorToSnorm2x16, bit-reinterpreted via asfloat). 0 in the packed-normal
// channel is the "nothing written here" sentinel RAB_GetGBufferSurface tests for.
Texture2D<float2>   gGiSurfPosHist    : register(t12);
RWTexture2D<float2> gGiSurfPosHistOut : register(u7);
Texture2D<float2>   gGiSurfNrmHist    : register(t13);
RWTexture2D<float2> gGiSurfNrmHistOut : register(u8);

// ---- U1's HALF-RESOLUTION VISIBILITY HISTORY (giRestirVisibility == HalfResolution) ----
// A second, half-extent pair at (w+1)/2 x (h+1)/2: one full-res pixel of every 2x2 block traces
// F2/F3 for real each frame (giVisTracedPixel below), the other three reconstruct from this
// history (2.10 D/E of the optimisation-wave-2 plan). One RGBA16F pair rather than three separate
// textures, since F2/F3 read+write together at one pixel in four.
//   r = F3 reuse-visibility EMA. g = F2 traced-luminance EMA (the second-bounce candidate hit's
//       own indirect estimate). b = F2 unoccluded-sky-luminance EMA
//       (g/b is an occlusion RATIO, not absolute, so it rescales correctly against a non-traced
//       pixel's own unoccluded sky). a = 1.0 where this frame's phase pixel wrote, else 0 (same
//       "never written" sentinel convention as gGiSurfNrmHist).
// UNMEASURED size: 2 x ceil(W/2) x ceil(H/2) x 8B -- see 2.10 F's memory table for native/AverSR-Q.
Texture2D<float4>   gGiVisHist    : register(t16);  // r = F3 reuse visibility EMA, g = F2 traced-lum EMA,
RWTexture2D<float4> gGiVisHistOut : register(u10);  // b = F2 unoccluded-sky-lum EMA, a = 1 written / 0 never

// ---- CSRdGiTrace/CSRdGi CANDIDATE HAND-OFF (Settings::rayDrivenGiSplit) -- ONE FRAME, ONE PIXEL EACH ----
// gRdGiCand ferries giTraceInitialCandidate's full output from CSRdGiTrace's dispatch (voxi.hlsl,
// traces the ray) to CSRdGi's AVER_GI_SPLIT=1 compile (resamples it) -- a same-frame relay, not a
// history buffer like gRdGiTex/gRdSunVisTex. Declared here rather than in voxi.hlsl because this
// file is #included (voxi.hlsl L419) BEFORE the staged declarations (~L450), so CSRdGiTrace/CSRdGi
// already have it in scope without a forward declaration (which this file's ordering contract,
// header comment above, warns against).
// 48 bytes, stride 48: two float3+scalar pairs pack into 16-byte lanes without a manual pad field.
// flags bit 0 = giTraceInitialCandidate's return value (candidate exists), bit 1 = nonFiniteCandidate,
// bit 2 = f2Observed -- 3 bools alongside its 6 float outputs, packed with room to spare.
struct RdGiCand { float3 pos; uint flags; float3 nrm; float f2LumTraced; float3 rad; float f2LumSky; };
RWStructuredBuffer<RdGiCand> gRdGiCand : register(u17);
// Set by CSRdGi, per invocation, before calling giRestirIndirect: the row-pitch pixel index
// (idx = pixel.y * pitch + pixel.x) CSRdGiTrace wrote this candidate under. `static`, not a
// parameter, like gGiPoisonPdfHit/gGiCbSkip -- giRestirIndirect's signature is shared with
// PSMainVoxi/PSRayDriven's non-split call sites and must not change.
static uint gGiCandIdx = 0;

// RTXDI's RAB_Surface contract. `linearDepth` is carried on the struct rather than re-derived by
// RAB_GetSurfaceLinearDepth, because the same accessor is called on both the current pixel's
// surface (giRestirIndirect, current gViewProj) and a reprojected previous-frame one
// (RAB_GetGBufferSurface, gPrevViewProj) -- computing it once at construction is cheaper than a
// branch at every read.
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
// TemporalResampling.hlsli); a spatial pass (out of scope, see this block's header) would be the
// first real caller of the false branch, so this returns "no surface" honestly rather than
// fabricating one for an arbitrary neighbour pixel it has no cheap way to build.
RAB_Surface RAB_GetGBufferSurface(int2 pixelPosition, bool previousFrame) {
    RAB_Surface s = RAB_EmptySurface();
    if (!previousFrame) return s;
    if (gGiRestirParams.y < 0.5) return s;   // no real previous frame bound at all this session-frame
    uint texW, texH;
    gGiSurfNrmHist.GetDimensions(texW, texH);   // same resolution as gGiSurfPosHist; either would do
    if (pixelPosition.x < 0 || pixelPosition.y < 0 ||
        pixelPosition.x >= (int)texW || pixelPosition.y >= (int)texH) return s;
    // Normal texture first: its sentinel decides whether the position channel is worth reading
    // (both written together in giRestirIndirect, so zero here means position is equally meaningless).
    const float2 nrmRaw = gGiSurfNrmHist.Load(int3(pixelPosition, 0));
    const uint packedN = asuint(nrmRaw.y);
    if (packedN == 0u) return s;
    const float2 posXY = gGiSurfPosHist.Load(int3(pixelPosition, 0));
    s.valid = true;
    s.worldPos = float3(posXY, nrmRaw.x);
    s.normal = RTXDI_DecodeNormalizedVectorFromSnorm2x16(packedN);
    // Re-derived rather than stored a third time: one free matrix multiply against the exact stored
    // position, avoiding a requantised depth that could disagree with it.
    s.linearDepth = mul(float4(s.worldPos, 1.0), gPrevViewProj).w;
    return s;
}

// ---- material similarity: a STATED SIMPLIFICATION, not spatial reuse's "same surface" test ----
// "Same surface" is RTXDI_IsValidNeighbor's normal/depth comparison, already run first
// (Rtxdi/GI/TemporalResampling.hlsli). This would
// add telling apart two geometrically similar but differently-shading surfaces (glass beside
// concrete), which matters most for SPATIAL reuse (crossing a material seam) and is out of scope
// here -- temporal reuse only compares one screen location against its own already depth/normal-
// gated reprojection. A real handle needs RtInstance::materialIndex carried through the
// surface-history pair, and its formats have no free channel for that.
// #define, not typedef -- matches RtxdiTypes.h's own idiom (`#define uint32_t uint`).
#define RAB_MaterialData uint
RAB_MaterialData RAB_GetMaterial(RAB_Surface s) { return 0; }
bool RAB_AreMaterialsSimilar(RAB_MaterialData a, RAB_MaterialData b) { return true; }

// ---- POISON-VIEW INSTRUMENTATION: thread-private flag RAB_GetGISampleTargetPdfForSurface sets ----
// `static`, not groupshared/a resource: per-invocation storage (like a local, but reachable from a
// helper without changing its signature) -- does not persist across entry-point invocations.
// Needed because RAB_GetGISampleTargetPdfForSurface's signature is RTXDI's fixed RAB_ contract, so
// it cannot grow an `out` param. giRestirIndirect resets this false at the top of its invocation,
// before RTXDI's resampling can reach this function, then reads it back at the end to paint the
// poison-view colour.
static bool gGiPoisonPdfHit = false;

// Target pdf = luminance (averShadowLum, this file's standing reduction) times cosine at the
// RECEIVING surface. RIS is unbiased for any positive p^ (RTXDI_FinalizeGIResampling divides it
// back out), so the cosine only changes VARIANCE: luminance alone rated a bright near-grazing
// candidate (whose cosine will multiply toward zero at the final estimator anyway) as highly as
// one arriving head-on, letting a grazing outlier win the slot. Albedo/PI is constant per surface
// and correctly left out; cosine varies per candidate and belongs in the resampling weight.
float RAB_GetGISampleTargetPdfForSurface(float3 samplePosition, float3 sampleRadiance, RAB_Surface surface) {
    // ---- NO SURFACE, NO TARGET PDF -- what makes the cosine above SAFE ----
    // THE BUG THIS CLOSES -- not obvious, cost a day to find. Before the cosine was added this
    // ignored `surface`, so a routinely-invalid one went unnoticed: RTXDI's bias correction evaluates
    // the selected sample's pdf at each NEIGHBOUR's surface via RAB_GetGBufferSurface(idx,true),
    // which legitimately answers "no surface" (sky last frame, outside the previous viewport, or the
    // first frame after the history pair is recreated), and enableFallbackSampling skips
    // RTXDI_IsValidNeighbor so an invalid neighbour reaches here by design. RAB_EmptySurface() gives
    // worldPos (0,0,0), normal (0,0,1): for Sponza geometry above the origin that cosine reads ~1
    // where a real surface gives ~0.3, inflating piSum (RTXDI_FinalizeGIResampling's denominator) and
    // systematically UNDER-weighting every reused sample -- compounding toward black at rest, snapping
    // bright again once motion rejects the history ("visible when I move, otherwise black" --
    // ReSTIR-only, since nothing else consults a previous-frame surface this way).
    // Zero is the honest answer RIS wants: a surface that doesn't exist contributes no weight either.
    if (!RAB_IsSurfaceValid(surface)) return 0.0;

    // Recomputed per candidate (not hoistable like the per-surface albedo/PI term above): wherever
    // THIS candidate's ray landed.
    //
    // ---- SAME COSINE FLOOR AS AVER_GI_MIN_COS, same reason, applied here too ----
    // MEASURED (--firefly-metric, --cam-wobble repro): this floor alone measurably reduced how often
    // the reported spike fired, but left a residual -- three later backstops were tried against that
    // residual and none of them kept; see giRestirIndirect's own comment for why it is irreducible
    // Monte Carlo variance, not more of this bug. Kept as a measured improvement, stated as partial,
    // not complete.
    // MECHANISM: this return value is p^; weightSum divides by it after finalizing
    // (`weightSum * normalizationNumerator / (selectedTargetPdf * piSum)`), so p^ -> 0 lets weightSum
    // -> infinity for a sample that is no brighter -- the same 1/cosTheta blow-up AVER_GI_MIN_COS
    // bounds at creation, unbounded again here at every surface that later resamples it. Common
    // under camera rotation, since geometry keeps shifting relative to a stored position.
    // Floors only a small POSITIVE cosine (grazing but genuine), never negative (wrong hemisphere) --
    // computed on the unclamped dot product so that distinction survives.
    // FLOOR, NOT REJECT, unlike AVER_GI_MIN_COS: this is an importance PROXY, not the physical
    // integrand (giRestirIndirect recomputes cosR separately, unfloored, for shading), so raising
    // the floor only changes which candidate RIS keeps, not what a kept candidate is worth -- a
    // variance fix, not a second correctness trade.
    const float3 toSample = samplePosition - surface.worldPos;
    const float  dist2    = dot(toSample, toSample);
    const float  rawCos   = dist2 > 1e-8 ? dot(toSample * rsqrt(dist2), surface.normal) : -1.0;
    const float  cosR     = rawCos > 0.0 ? max(rawCos, AVER_GI_MIN_COS) : 0.0;
    // Defense in depth (mirrors RAB_ValidateGISampleWithJacobian's isnan/isinf/non-positive->reject
    // idiom below): sampleRadiance should already be finite given giTraceInitialCandidate's NaN-safe
    // clamps, so this should never fire; included since every other radiance/weight sink here has
    // this same shape.
    const float pdf = averShadowLum(sampleRadiance) * cosR;
    if (isnan(pdf) || isinf(pdf)) { gGiPoisonPdfHit = true; return 0.0; }
    return pdf;
}

// ---- CLOSE THE JACOBIAN ASYMMETRY: reject on the real value, but hand back 1.0 ----
// BUG: RTXDI's BASIC bias correction (SpatioTemporalResampling.hlsli) combines each stream into the
// numerator as `targetPdf * jacobian * ...` (~:183) but seeds the MIS denominator from
// `ps * neighborReservoir.M` with NO jacobian term (~:267) -- one-sided, inflating the numerator for
// any tap whose reprojection geometry differs from the current pixel's. That's exactly the taps that
// SHOULD differ under camera rotation: the +-1-2px jittered temporal tap and the +-32px spatial taps
// (SpatioTemporalResampling.hlsli :114-122, :222-230; all six-of-seven non-anchor candidates), never
// the i==0 temporal anchor (whose Jacobian stays ~1 since its receiver doesn't move) -- the other six
// each re-tap a screen location that never repeats frame to frame while the camera turns.
// FIX IS THE RESTIR-DI FORM: still reject on the real jacobian (NaN/Inf/non-positive, or a grazing
// reprojection whose terms divide by near-zero -- must still be refused outright rather than feed a
// reused sample's weight into the bright-outlier bug named in aver-negative-radiance-reads-bright)
// since RTXDI calls it "valuable information to determine if the GI sample should be combined" --
// but an ACCEPTED tap now reports 1.0, not the measured ratio, so once accepted neither sum carries a
// jacobian and the two sides agree again.
// [1/4, 4], NOT [1/25, 25] -- REASONED, NOT MEASURED: with acceptance now geometry-blind, the window
// only decides "plausibly the same surface patch", so it can be tighter than the old numerator-only
// bound. +-2 stops admits the near-1 anchor and a similar spatial neighbour at rest while rejecting
// extreme foreshortening across a depth discontinuity (RTXDI_IsValidNeighbor's depth/normal test is
// the first line of defence against that; this is the second, narrower one).
bool RAB_ValidateGISampleWithJacobian(inout float jacobian) {
    if (isnan(jacobian) || isinf(jacobian) || jacobian <= 0.0) return false;
    if (jacobian < 0.25 || jacobian > 4.0) return false;
    // MEASURED 2026-09-19: handing back the real ratio instead changed the camera-motion overshoot
    // not at all (+8.0% vs +8.2%), so the reasoning above stands.
    jacobian = 1.0;
    return true;
}

// Only used when temporal resampling's bias-correction mode is RAY_TRACED; this file selects BASIC
// above so this isn't hot, but RAB_ requires the symbol. One shadow-style ray between the CURRENT
// surface and the REUSED sample's position -- RTXDI's own approximate check, not a converged one.
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
    q.TraceRayInline(gScene, RAY_FLAG_NONE | gAverRtSecondaryRayFlags, AVER_RT_MASK_OPAQUE_ALL, r);
    averRtProceedSolid(q);
    return q.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

// ---- the neighbour-offset table RTXDI_CalculateSpatialResamplingOffset indexes ----
// NVIDIA's samples bind this as an 8192-entry R8G8_SNORM buffer that FillNeighborOffsetBuffer
// (third_party/rtxdi/Source/RtxdiUtils.cpp) fills at startup; Voxi's descriptor table is a FIXED
// kVoxiSrvCount/kVoxiUavCount pair, so an 8192-entry buffer for one feature doesn't fit the budget.
// RTXDI_CalculateSpatialResamplingOffset masks its index by neighborOffsetMask anyway, so a small
// array + matching mask suffices: 32 taps/mask 31 covers the 8 spatial samples plus temporal-jitter
// offsets drawn per pixel below.
// NVIDIA'S OWN VALUES: generated by running FillNeighborOffsetBuffer's exact algorithm (R=250-scaled
// low-discrepancy spiral, rejected to the unit disc) for the first 32 entries and decoding the
// int8 pair the way an R8G8_SNORM view would (v/127), not the raw disc coordinates.
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
// ALL FIVE UNMEASURED: no sweep fit any against ground truth (same honesty as the motion-discount
// 32px knee / [1/4,4] Jacobian window elsewhere in this file). Mirrored byte-for-byte in
// aver/voxi/GiVisibility.hpp's kHistWeight/kRhoMax/kNormalPow/kPlaneTolRel/kPlaneTolCm --
// GiVisibilityTest checks the two never drift apart.
// HIST_WEIGHT: EMA weight the half-res write (2.10 E) blends toward at rest, lerped to 0.5 under
// motion (same shape as rtShadowTemporal's 0.9/0.5 pair). 0.8 = ~5 frames effective averaging.
#define AVER_GI_VIS_HIST_WEIGHT 0.8
// RHO_MAX: ceiling on F2's reconstructed sky ratio (rho2 = g/b, in [0,1] in expectation but not
// bounded pixel-to-pixel under EMA lag) -- keeps a stale history from scaling a non-traced pixel's
// unoccluded sky term up rather than down.
#define AVER_GI_VIS_RHO_MAX 4.0
// NORMAL_POW: exponent on saturate(dot(neighbourNormal, N)) in the reconstruction weight -- higher
// than a plain cosine so a loosely-agreeing neighbour normal contributes little, without a hard cutoff.
#define AVER_GI_VIS_NORMAL_POW 8.0
// PLANE_TOL_REL / PLANE_TOL_CM: two-term (relative-to-depth + flat floor) tolerance on
// |dot(neighbourWorldPos - wpos, N)| a reconstruction tap must fall inside -- covers both near and
// far geometry regimes.
#define AVER_GI_VIS_PLANE_TOL_REL 0.02
#define AVER_GI_VIS_PLANE_TOL_CM 1.0

// ---- the one RAB_ callback TemporalResampling.hlsli never needed but the spatial half does ----
// SpatioTemporalResampling.hlsli's spatial samples (via Rtxdi/GI/SpatialResampling.hlsli) walk off
// the reprojected pixel by up to samplingRadius and must be pulled back on-screen; RTXDI calls this
// itself (RAB_ClampSamplePositionIntoView) rather than clamping inline, the same way it calls
// RAB_GetGBufferSurface instead of reading a texture directly. gSceneViewport, NOT gSceneViewportCur:
// every call here passes
// previousFrame=true, and this file's standing convention (rtReprojectHistory, giRestirIndirect's
// screenSpaceMotion) is that the PREVIOUS frame's rect is gSceneViewport. Clamping to the viewport
// rather than the texture matters at the frame edge -- pixels outside the 3D view are editor
// chrome with no surface for a clamped sample to land on.
int2 RAB_ClampSamplePositionIntoView(int2 pixelPosition, bool previousFrame) {
    const int2 lo = int2(gSceneViewport.xy);
    const int2 hi = lo + int2(gSceneViewport.zw) - 1;
    return clamp(pixelPosition, lo, max(hi, lo));
}

// ---- AND ONLY NOW THE RESAMPLER ITSELF ----
// HLSL has no forward declarations: every RAB_ symbol named anywhere in
// SpatioTemporalResampling.hlsli's own include chain (Reservoir/SpatialResampling/
// TemporalResampling, its first three includes) -- RAB_Surface most of all, plus
// RAB_ClampSamplePositionIntoView above (spatial-only) -- must be fully declared by the time this
// line is parsed. Getting this wrong once failed PSMainVoxi, PSRayDriven, VSky and VSMain alike
// (`unknown type name 'RAB_Surface'`, a runtime failure with zero build errors, since all entry
// points share one translation unit) -- took the whole RT feature set down, giMode 0 same as 1.
#include "Rtxdi/GI/SpatioTemporalResampling.hlsli"

// Reservoir buffer addressing params, derived from gGiSurfNrmHist's ACTUAL dimensions rather than a
// cbuffer field -- mirrors rtxdi::CalculateReservoirBufferParameters
// (third_party/rtxdi/Source/RtxdiUtils.cpp) exactly (RTXDI_RESERVOIR_BLOCK_SIZE-pixel blocks).
// VoxiRenderer.cpp's giReservoirElemCount computes the identical formula from the same
// width/height, so both sides derive pitch from one source of truth rather than a cbuffer value
// that could drift from the texture it describes.
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
// PLACED HERE: giVisReconstruct calls RAB_GetGBufferSurface itself (reusing its bounds check and
// t12/t13 read), so must follow it; called from giRestirIndirect's decode block before
// giTraceInitialCandidate runs, so must precede that call site too.
//
// kGiVisPhase: which of a 2x2 block's four full-res pixels traces F2/F3 for real on a given frame,
// cycling every 4 frames so every pixel gets its own turn -- read from both ends: giVisTracedPixel decides THIS frame's traced pixel
// directly; giVisReconstruct reads the PREVIOUS frame's entry (frameIdx - 1) to know which pixel
// wrote the half-res texel it samples.
static const uint2 kGiVisPhase[4] = { uint2(0, 0), uint2(1, 1), uint2(1, 0), uint2(0, 1) };

// Is pixel `p` this 2x2 block's traced pixel on frame `frame`? `& 3u`, not `% 4u`, matching this
// file's own bitmask convention (kGiNeighborOffsetMask, tileMask/turnMask in voxi_rt.hlsli) for a
// cyclic schedule over a compile-time power of two.
bool giVisTracedPixel(uint2 p, uint frame) {
    const uint2 ph = kGiVisPhase[frame & 3u];
    return (p.x & 1u) == ph.x && (p.y & 1u) == ph.y;
}

// One reconstructed visibility sample: `valid` says whether any tap survived rejection, `v3`/`g`/`b`
// mirror gGiVisHist's r/g/b channels (F3 reuse-visibility EMA, F2 traced-luminance EMA, F2
// unoccluded-sky-luminance EMA), `motionPx` is this pixel's reprojection distance -- computed once
// here and reused by the half-res history write's motion knee (2.10 E).
struct GiVisRecon { bool valid; float v3; float g; float b; float motionPx; };

// Reconstructs this pixel's F2/F3 answer from non-traced neighbours' half-resolution history,
// depth/normal-weighted like a spatial denoiser upsample: a previous-frame surface matching this
// receiver's plane and normal draws candidates from the same hemisphere distribution, so the
// expected F2/F3 answer is smooth in (position, normal) and weighting by both preserves that.
GiVisRecon giVisReconstruct(float3 wpos, float3 N, float2 pixel, uint frameIdx) {
    GiVisRecon rec = (GiVisRecon)0;   // valid = false, everything else 0 -- the honest "no reconstruction" answer

    // 1. Validity first, before any t12/t13/t16 read. gGiRestirParams.y = does a real previous-frame
    // surface-history pair (t12/t13) exist (same field RAB_GetGBufferSurface tests). gAmbientParams.w
    // bit 8 = does gGiVisHist (t16) hold a real previous frame (set once beginShadowHistory has
    // written it, VoxiRenderer::endShadowHistory). Either false means every tap below would read
    // stale/newly-allocated storage, so bail before touching any of the three textures.
    if (((uint)gAmbientParams.w & 8u) == 0u || gGiRestirParams.y < 0.5) return rec;

    // 2. Same reprojection recipe as giRestirIndirect's own screenSpaceMotion further down --
    // gPrevViewProj + gSceneViewport (never gSceneViewportCur, this file's standing convention for
    // a previous frame's rect; see RAB_ClampSamplePositionIntoView's comment on that convention) --
    // reused rather than re-derived, so the two can never disagree.
    const float4 prevClip = mul(float4(wpos, 1.0), gPrevViewProj);
    if (prevClip.w <= 1e-4) return rec;
    const float3 prevNdc = prevClip.xyz / prevClip.w;
    const float2 prevPx = gSceneViewport.xy +
        float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * gSceneViewport.zw;
    rec.motionPx = length(prevPx - pixel);

    // 3. Bilinear taps into the half-resolution history. prevPx is full-resolution; halving lands
    // in gGiVisHist's texel space, and -0.5 recentres onto the half-res grid (texel-corner convention).
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
        // 4a. Bounds-check against gGiVisHist's OWN dimensions ((w+1)/2 x (h+1)/2), not gGiSurfNrmHist's.
        if (t.x < 0 || t.y < 0 || t.x >= (int)visW || t.y >= (int)visH) continue;
        if (wgt[k] <= 0.0) continue;   // a bilinear corner exactly on a texel needs no neighbour tap

        // 4b. The writer's full-resolution pixel, from LAST frame's phase table (frameIdx - 1u, not
        // frameIdx): this reconstruction runs against a history the PREVIOUS frame wrote, and
        // giVisTracedPixel's phase table decided which of the block's four pixels wrote it. Unsigned
        // wrap at frameIdx 0 is deliberate, matching HLSL uint arithmetic (checked by GiVisibilityTest).
        const uint2 writer = uint2(t) * 2u + kGiVisPhase[(frameIdx - 1u) & 3u];
        // RAB_GetGBufferSurface already bounds-checks and reads t13 then t12; its own "no real
        // previous frame" test reads the same gGiRestirParams.y field already checked in step 1.
        const RAB_Surface ps = RAB_GetGBufferSurface(int2(writer), true);
        if (!RAB_IsSurfaceValid(ps)) continue;

        const float4 vh = gGiVisHist.Load(int3(t, 0));
        if (vh.a < 0.5) continue;   // never written this texel -- the sentinel the half-res write (2.10 E) sets

        // 4c. Plane test: two-term tolerance (flat floor + fraction of distance) against the same
        // worldPos/linearDepth RAB_GetGBufferSurface just reconstructed.
        const float planeTol = AVER_GI_VIS_PLANE_TOL_CM + AVER_GI_VIS_PLANE_TOL_REL * ps.linearDepth;
        if (abs(dot(ps.worldPos - wpos, N)) > planeTol) continue;

        const float nDot = saturate(dot(ps.normal, N));
        const float w = wgt[k] * pow(nDot, AVER_GI_VIS_NORMAL_POW);
        wsum += w; rsum += w * vh.r; gsum += w * vh.g; bsum += w * vh.b;
    }

    // 5. Valid only past a noise floor, not merely "one tap survived": a single near-grazing corner
    // (tiny wgt[k]) would otherwise be trusted as much as four agreeing taps. 1e-3 floors the SUM of
    // up to four weights in [0,1] -- admits one dim real tap, rejects pure numerical noise.
    rec.valid = wsum > 1e-3;
    if (rec.valid) {
        rec.v3 = rsum / wsum;
        rec.g  = gsum / wsum;
        rec.b  = bsum / wsum;
    }
    return rec;
}

// ---- T4 (Settings::rtGiHitShadowMap): sun visibility at a GI hit from the GI-only shadow map ----
// Repeats giShadowFactor's lookup (voxi.hlsl, defined after this file, so can't be called) but
// returns -1 where the map can't answer, instead of "fully lit" -- a GI ray can leave the map's box,
// or the map can be unusable for a frame; the caller then fires the shadow ray it would have fired
// anyway. MEASURED (NewSponza, staged mode 1): GI trace 3.38 -> 2.68ms; the prototype's ~1% brightening
// under column capitals (19cm texels) no longer reproduces (MAD 0.18). ON by default; see Settings::rtGiHitShadowMap.
// gGiShadowTex/gShadowSamp/gGiShadowViewProj/gGiShadowParams declared at the top of voxi.hlsl.
float giHitShadowMapVisibility(float3 wpos, float3 N, float3 L) {
    if (gGiShadowParams.y < 0.5) return -1.0;
    const float  slope = saturate(1.0 - saturate(dot(N, L)));
    const float3 p0    = wpos + N * (gGiShadowParams.z * (1.0 + slope));
    const float4 lp    = mul(float4(p0, 1.0), gGiShadowViewProj);
    const float3 p     = lp.xyz / lp.w;
    const float2 uv    = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (any(uv < 0.0) || any(uv > 1.0) || p.z > 1.0 || p.z < 0.0) return -1.0;
    const float t = gGiShadowParams.x;
    float s = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x)
        s += gGiShadowTex.SampleCmpLevelZero(gShadowSamp, uv + float2(x, y) * t, p.z);
    return s / 9.0;
}

// ---- the initial candidate: ONE cosine ray, traced and shaded through machinery this file already has ----
// Traces off (wpos, N) along a cosine-weighted hemisphere direction and shades the hit through the
// same RayQuery + flat geometry table + gRtMaterials + averShadeDirect machinery rtReflection/
// PSRayDriven use for a mirror/primary ray, pointed along a diffuse-importance direction instead.
//
// Returns false on a miss: caller hands RTXDI an EMPTY initial reservoir and lets temporal
// resampling (RTXDI_IsValidGIReservoir gates on M != 0) carry the pixel from history alone.
// U1's F2 PATH SELECTOR AND OUTPUTS (2.10 A/B): `f2Path` is the decode block's verdict
// (giRestirIndirect: 3 trace / 2 half-res ratio / 1 reconstructed / 0 legacy-no-ray), `rho2` is the
// half-res reconstruction's occlusion ratio (meaningful only at f2Path == 2u), and the three `out`
// params carry back what F2's TRACED path (f2Path == 3u) observed for the half-res history write
// (2.10 E) -- set only on that path, default "nothing observed" elsewhere (same convention as
// nonFiniteCandidate).
bool giTraceInitialCandidate(float3 wpos, float3 N, float2 pixel, float frameJitter,
                             out float3 samplePos, out float3 sampleNormal, out float3 sampleRadiance,
                             out bool nonFiniteCandidate, uint f2Path, float rho2,
                             out float f2LumTraced, out float f2LumSky, out bool f2Observed) {
    samplePos = sampleNormal = sampleRadiance = 0.0;
    // `out` params must be written on every exit (HLSL, like C++); false/0 is the correct default
    // on every early-out below, since nothing has been observed yet to have caught anything.
    nonFiniteCandidate = false;
    f2LumTraced = 0.0; f2LumSky = 0.0; f2Observed = false;

    // Cosine-weighted hemisphere sample (Malley's method), the standard Lambertian importance
    // sample -- its pdf (cosTheta/PI) is what RTXDI_MakeGIReservoir divides out at the call site.
    // rtDiscSample/rtHash: same per-pixel-rotated low-discrepancy sequence rtReflection's cone
    // sample uses, jittered per frame (frameJitter) rather than frozen per pixel -- see rtReflection
    // for what a frozen-per-pixel sample cost the reflection ray before that was fixed.
    // ---- F1 (R0), gAmbientParams.z bit 1: legacy 45-degree ring vs. cosine hemisphere ----
    // true (bit set) keeps rtDiscSample's fixed ring (HEAD, unchanged) for comparison; false
    // (corrected default) draws rtHemiDiscSample (voxi_rt.hlsli) instead, uniform over frames
    // rather than pinned to cosTheta = 1/sqrt(2). Frame index from gRtHistParams.z (raw index for
    // rtHemiDiscSample's nested sequence), not frameJitter (already frameIdx*2.39996323, an angle).
    // streamSalt 0.0 keeps this stream apart from F2's second-bounce and the sky-occlusion ray,
    // both at (pixel,frame,k=0).
    const float2 xi = (((uint)gAmbientParams.z & 1u) != 0u) ? rtDiscSample(0, rtHash(pixel) * 6.2831853 + frameJitter) : rtHemiDiscSample(0u, 1u, (uint)gRtHistParams.z, pixel, 0.0);
    const float  cosTheta = sqrt(saturate(1.0 - dot(xi, xi)));
    // ---- THE FLOOR IS A FIREFLY BOUND, AND 1e-4 WAS NOT ONE ----
    // cosTheta IS the sample pdf (cosTheta/PI), so weightSum = PI/cosTheta -- at the old 1e-4 that's
    // ~31,400, invisible because it CANCELS in the single-candidate case (the estimator's receiver
    // cosine at the same surface reduces est to plain `radiance`).
    // IT STOPS CANCELLING ONCE THE RESERVOIR IS REUSED: RTXDI_CombineGIReservoirs weighs the stored,
    // unclamped weightSum by a target pdf at a DIFFERENT neighbour's surface, unrelated to the
    // grazing angle that produced it. AVER_VOX_MAXRAD clamps run AFTER RTXDI_StoreGIReservoir, so the
    // unbounded weight is what neighbours inherit -- one grazing sample becomes a bright speck that
    // spreads.
    // 0.05 bounds weightSum at ~63 (500x cut worst case). Cost is measurable: for a cosine-weighted
    // disc sample P(cosTheta < c) = c^2, so this discards 0.25% of directions, all within 3 degrees
    // of the tangent plane where the Lambertian lobe carries least weight. Rejected, not clamped --
    // clamping the pdf while keeping the sample would understate its weight and bias the estimator
    // dark, the mistake this file already made once.
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
    q.TraceRayInline(gScene, RAY_FLAG_NONE | gAverRtSecondaryRayFlags, AVER_RT_MASK_OPAQUE_ALL, r);
    averRtProceedSolid(q);
    // ---- A MISS IS A SAMPLE OF THE SKY, NOT A FAILED SAMPLE ----
    // Used to `return false` -- the clearest reason reported darkness was ReSTIR-ONLY: a cosine ray
    // that escapes carries the sky's radiance, and under an open arcade the sky is most of the
    // hemisphere and most of the true indirect term. Dropping it made exactly those surfaces go
    // near-black while sunlit ones stayed correct.
    // OWNERSHIP, CORRECTED (R1/F4, voxi.hlsl): an earlier comment claimed coneTracedIndirect already
    // gathers the sky via the volume, licensing a second sky add at the receiver (voxi.hlsl's
    // ind4.ambient/ind.ambient) -- false: coneTracedIndirect's `bounce` (voxi_cone.hlsli) is the
    // volume's RE-EMITTED radiance, a later bounce, not this ray's own visible sky. This branch is
    // now the RECEIVER'S ONE AND ONLY copy of the sky term for giMode 1; voxi.hlsl's F4
    // (gAmbientParams.z bit 2) subtracts its own ambient copy back out to keep that true.
    // ALSO COST THE ESTIMATOR ITS ANCHOR: returning false left `initial` empty (M=0), so a miss
    // wasn't even counted as an observation, leaving miss-heavy pixels free-running on reused
    // weights with no physically-derived 1/pdf anchor.
    // averSkyRadianceCheap is rtAmbientTraced's own function (not a new sky model), so both ray
    // paths agree on the sky's worth. sampleNormal faces back down the ray; samplePos sits at TMax
    // so the Jacobian sees a real, finite direction.
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        samplePos      = wpos + dir * r.TMax;
        sampleNormal   = -dir;
        // NaN-safe, replacing a native clamp(): HLSL's clamp() has compiler/driver-defined NaN
        // behaviour, unlike this file's hand-rolled min(max(x,lo),hi) (floors NaN to lo). A
        // temporary is needed since this clamps an expression, not a named `radiance` variable.
        {
            const float3 rawSky = averSkyRadianceCheap(dir) * gAmbient.r;
            const bool   bad    = any(isnan(rawSky)) || any(isinf(rawSky));
            nonFiniteCandidate  = nonFiniteCandidate || bad;
            sampleRadiance      = bad ? float3(0.0, 0.0, 0.0) : min(max(rawSky, 0.0), AVER_VOX_MAXRAD);
        }
        return true;
    }

    RtInstance inst = rtLoadInstance(rtPackCommitted(q));
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
    // Used to build the surface from factors only (flat-shaded assumed to converge toward a
    // textured low-frequency answer) -- wrong, since a glTF factor MULTIPLIES its map rather than
    // averaging it. Sponza sets metallicFactor 1.0 and keeps the real near-zero metalness in the
    // metal-rough map's blue channel, so every textured hit read as pure white METAL (kdAlbedo 0,
    // no diffuse/multi-bounce, `kdAlbedo * indY` below zeroed) -- a single bounce that was nothing
    // but a white specular sun glint.
    // MEASURED, PTTest NewSponza, sun 85.6deg, gallery pose, linear HDR means vs. converged path
    // tracer at the same bounce depth (--tonemap 0, fog off):
    //   single bounce, factor-only        0.0168 whole   0.00338 inner wall  (PT 1 bounce 0.0095 / 0.0012)
    //   single bounce, base colour only   0.0099         0.00053
    //   single bounce, base + metal-rough 0.0112         0.00096
    // factor-only multi-bounce added only 0.0004 to a wall the path tracer lights 85% by multi-bounce
    // (4 bounces 0.0080 vs 1 bounce 0.0012).
    // Same maps as PSRayDriven's surface / rtReflection's hit sample, at a footprint (AVER_GI_HIT_TEX_CONE)
    // rather than mip 0 -- mip 0 is the throughput trap rtReflection's own comment measures; a
    // diffuse bounce only needs the local average colour.
    // WITHOUT AVER_RT_BINDLESS (raster PSMainVoxi's RT variant) no map can be read, so that variant
    // keeps the factor-only surface and its metal-for-glTF error. Ray-driven (default) is always bindless.
    AverSurface s = (AverSurface)0;
    s.N           = hitN;
    s.V           = -dir;
    // Guarded against a genuine 0/0 if the traced ray lands exactly antiparallel to the sun (cheap
    // risk reduction, not a confirmed trigger) -- s.N is always finite and a reasonable fallback.
    {
        const float3 VL  = s.V + L;
        const float  vl2 = dot(VL, VL);
        s.H = vl2 > 1e-12 ? VL * rsqrt(vl2) : s.N;
    }
    float4 hitMapBase = float4(1, 1, 1, 1);
    float4 hitMapMR   = float4(1, 1, 1, 1);
    // White: an unbound emissive map is the identity, so mat.emissiveFactor alone carries a lamp's glow.
    float4 hitMapEmis = float4(1, 1, 1, 1);
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
#if !AVER_RD_SINGLE_PASS
        // The emissive map, on the footprint just computed. Not in the single-pass compile, which is
        // at the AMD driver's register limit (rtGiShadowBits() in voxi_rt.hlsli): the factor alone
        // there.
        hitMapEmis = averRtSampleSlot(mat, 4, huv, hgx, hgy, float4(1, 1, 1, 1));
#endif
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
    s.sssColor    = float3(1.0, 1.0, 1.0);   // set, not left undefined; inert at weight 0
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
    s.emissive  = mat.emissiveFactor * hitMapEmis.rgb;
#if AVER_RD_LAMPS
    // A promoted lamp's glow is already light from the direct lamp term (rdLocalLightsShade,
    // voxi.hlsl) while local lights are active: its lightIntensity lights every surface in range,
    // shadowed. Carrying its emission here too would light twice.
    // Only when EVERY lamp-flagged draw made this frame's light list (rdLocalCarriesEmitters), or a
    // lamp cut by the 32-light cap would lose both. Voxel GI injection and reflections keep it (a
    // mirrored bulb must still glow, and the volume serves passes with no lamp term). Compiled
    // wherever the lamp term is (AVER_RD_LAMPS), so the two always come and go together.
    if ((mat.flags & AVER_MAT_LIGHT) != 0u && rdLocalCarriesEmitters())
        s.emissive = float3(0.0, 0.0, 0.0);
#endif
    s.occlusion = 1.0;

    AverLight sun;
    sun.direction = L;
    sun.radiance  = averSunRadiance();
    // One fresh shadow ray, not gRtShadowHist's temporal amortisation: that history is keyed by
    // SCREEN PIXEL, and this sample's origin is a world point with no pixel of its own -- the reason
    // a reservoir stores a POSITION. RTXDI's own temporal resampling amortises this ray across
    // frames instead.
    // T1 (Settings::rtSecondaryShadowOpaque): hitPos is this candidate's SECONDARY hit, what
    // rtShadowOpaque (voxi_rt.hlsli) exists for -- see its own header for the translucent-tint trade.
    // if/else, not ?:, so the bit is the only thing selecting which ray runs.
    // T4 (Settings::rtGiHitShadowMap): the GI-only shadow map answers instead wherever it can (see
    // giHitShadowMapVisibility above); -1 (no answer) falls through to the rays below.
    float mapVis = -1.0;
    if ((rtGiShadowBits() & 8u) != 0u) mapVis = giHitShadowMapVisibility(hitPos, s.N, L);
    if (mapVis >= 0.0) {
        sun.visibility = mapVis;
    } else if ((rtGiShadowBits() & 1u) != 0u) {
        sun.visibility = rtShadowOpaque(hitPos, s.N, L, pixel, frameJitter);
    } else {
        sun.visibility = rtShadow(hitPos, s.N, L, pixel, float3(0, 0, 0), float3(0, 0, 0), 1u, frameJitter);
    }
    sun.visibility *= 1.0 + averCausticFocus(hitPos);

    // The hit's emission is the seed: averShadeDirect returns its first argument plus the direct sun
    // term (material_prelude.hlsl), and nothing else here adds s.emissive again -- counted exactly
    // once. (averShadeIndirect, which adds s.emissive itself, is PSRayDriven's pairing, not used here.)
    float3 radiance = averShadeDirect(s.emissive, s, sun);

    // ---- DIFFUSE AMBIENT ONLY. AN ENVIRONMENT-SPECULAR TERM HERE FLATTENS THE WHOLE IMAGE ----
    // MEASURED: adding `ind.specular = averSkyRadianceCheap(reflect(...))` through averShadeIndirect
    // turned Sponza's interior into a uniform grey wash. Cause, visible in averIndirectTerms
    // (material_prelude.hlsl): `specEnv = FssEss * ind.specular * specOcc` is NOT scaled by
    // ind.ambientScale the way `diffAmbient` is, so a
    // secondary hit got the sky at FULL, unoccluded radiance while its diffuse half was correctly
    // scaled down -- inside an enclosed space that dominates the real bounce. The original omission
    // was right; this replaces the comment that overrode it. COST: a metallic hit has kdAlbedo=0 and
    // reads as a GI black hole -- fixing that needs a term weighted by metalness AND scaled/occluded
    // like diffuse ambient, not an unconditional sky specular. (Before the hit's maps were read,
    // above, every textured glTF hit was such a black hole; only genuine metals are now.)
    //
    // ---- F2 (R2): THE DIFFUSE HALF NOW OWNS ITS OWN VISIBILITY, WHERE IT USED TO HAVE NONE ----
    // GAP: averSkyIrradiance(s.N) depends on the normal alone, with no notion of occlusion, so a
    // second-bounce point under an overhang got full open-sky irradiance as if it stood outside (see
    // aver-ambient-overbright-open-vs-enclosed for why unoccluded ambient in an interior is already
    // over-bright before this). gAmbientParams.z bit 4 TRUE keeps that old unoccluded read (HEAD,
    // A/B only); FALSE (corrected default, ON per the user's own call -- this extra ray sits behind
    // the legacy bit rather than a second switch) traces one more cosine ray from this hit instead:
    // a miss reads the genuinely visibility-tested SH sky; a hit inside the GI volume reads that
    // voxel's exitant radiance (the THIRD bounce, missing before this); a hit outside the volume
    // contributes nothing (real geometry outside the voxelised region is occluded, not open sky).
    // ARITHMETIC: L_o,ind(y) = (kd_y/PI) * Integral(L cos dw). A cosine-weighted second direction
    // (rtHemiDiscSample, streamSalt 0.71 to stay apart from F1's 0.0 at the same pixel/frame/k) makes
    // PI and cosine cancel, so one sample gives kd_y * L(w2) directly (PSVoxel's own injection
    // convention). In the open, V_y=1 and this reduces to the legacy value. gVoxelParams.y
    // (giIntensity) is deliberately NOT applied here -- applied once, to the whole estimate, at
    // giRestirIndirect's `est`; applying it here too would double it. Approximation carried over
    // unchanged from the volume's own injection: the value already carries PSVoxel's capped feedback
    // gain. It carries no sky while ReSTIR GI runs (PSVoxel's own comment) -- sky reaches this bounce
    // only via this ray's own miss.
    // The NaN-safe clamp at this function's end still runs last; F2 changes what feeds it, not the
    // clamp itself.
    // ---- U1 (2.10 B): FOUR PATHS, NOT TWO -- adds Reconstructed (voxel-cone march standing in for
    // the traced ray) and Half's non-traced reconstruction (a plain sky-luminance ratio). The legacy
    // branch's condition only gains `|| f2Path == 0u` (its body is unchanged text: f2Path==0u already
    // means "legacy bit 4 set, or giRestirVisibility is No ray", folded by the decode block). The
    // trace branch (final `else`) is byte-identical to before this task, only reached at f2Path==3u
    // now, with the three new `out` params written at its end.
    float3 indY = 0.0;
    if (((uint)gAmbientParams.z & 4u) != 0u || f2Path == 0u) {
        indY = averSkyIrradiance(s.N) * gAmbient.r;
    } else if (f2Path == 1u) {
        // ---- RECONSTRUCTED: ONE VOXEL-CONE MARCH STANDS IN FOR THE TRACED RAY ----
        // traceCone's contract (voxi_cone.hlsli:62; forward-declared in voxi_rt.hlsli) returns
        // premultiplied radiance + coverage. gVoxelParams.w guards on the volume being enabled, same
        // as every other conditional voxel read here; off falls back to the legacy unoccluded sky.
        // CONE'S RADIANCE ONLY -- no `sky * (1 - cone.a)` term. 1-a used to read as "open to sky" (as
        // PSVoxel does), which is wrong here: the volume is one-voxel SHELLS, and a 60-degree cone a
        // few metres out samples mip 5+ where a shell covers ~2% of a cell -- the cone sees through a
        // roof. MEASURED at PTTest's gallery (sun 85.6deg, linear means, once the hit's maps were read
        // so the term was no longer multiplied by zero): keeping it put the whole frame at 0.098
        // vs the path tracer's 0.0149 (6.6x). So Reconstructed has no sky at a hit, and interiors lit
        // through openings read darker than the path tracer (inner wall 0.0015 vs 0.0080); Half/Full
        // trace that sky and don't have this gap.
        if (gVoxelParams.w > 0.5) {
            const float4 cone = traceCone(hitPos, s.N, AVER_VOX_INJECT_APERTURE);
            indY = min(cone.rgb, AVER_VOX_MAXRAD);
        } else indY = averSkyIrradiance(s.N) * gAmbient.r;
    } else if (f2Path == 2u) {
        // HALF, non-traced pixel with a valid reconstruction: no ray, no cone, one ratio. rho2
        // (computed at the call site, giRestirIndirect) is the neighbourhood's occluded/unoccluded
        // sky luminance ratio, clamped at AVER_GI_VIS_RHO_MAX since it's an EMA-lagged ratio, not
        // frame-bounded [0,1].
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
        q2.TraceRayInline(gScene, RAY_FLAG_NONE | gAverRtSecondaryRayFlags, AVER_RT_MASK_OPAQUE_ALL, r2); averRtProceedSolid(q2);
        if (q2.CommittedStatus() != COMMITTED_TRIANGLE_HIT) indY = averSkyRadianceCheap(dir2) * gAmbient.r;
        else {
            // THE VOXEL SHELL STRADDLE: CSResolve (voxi.hlsl ~3417) stores an occupied voxel as
            // float4(meanRadiance,1), empty as float4(0,0,0,0) -- a one-voxel-thick SHELL, not a
            // solid fill. Sampling exactly ON the hit puts the tap astride that shell, so trilinear
            // filtering blends the lit texel with unlit neighbours (the shell's far side, or a thick
            // wall's hollow interior), darkening the result by roughly 0.5-0.75 before anything below
            // runs. Pulling the lookup point half a voxel back along -dir2 (the incoming direction --
            // no geometric normal exists at a RayQuery hit, so this stands in for the hit's surface
            // normal toward the room) re-centres the tap on the room side of the shell. voxelWorldF2
            // is the same "one voxel, world units" quantity PSVoxelDebug/CSResolve's neighbours
            // compute (voxi.hlsl:3441, voxi_gi.hlsli:210) from the same gVoxelOrigin.w/gVoxelParams.x
            // terms voxelUVW uses -- not a new cbuffer field.
            const float voxelWorldF2 = 1.0 / (gVoxelOrigin.w * gVoxelParams.x);
            const float3 uvw = voxelUVW(r2.Origin + dir2 * (q2.CommittedRayT() - voxelWorldF2 * 0.5));
            if (gVoxelParams.w > 0.5 && insideVolume(uvw)) {
                const float4 vox = gVoxelTex.SampleLevel(gVoxelSamp, uvw, 0);
                // Normalise by occupancy (alpha) rather than reading the premultiplied mean CSResolve
                // stores -- undoes the shell-straddle darkening above. Below AVER_GI_VOX_MIN_OCC
                // there's no occupied voxel left to recover a radiance from, so this falls back to 0
                // -- the same default indY already carries when insideVolume is false or off.
                indY = vox.a > AVER_GI_VOX_MIN_OCC ? min(vox.rgb / vox.a, AVER_VOX_MAXRAD) : 0.0;
            }
        }
        // U1's half-res history (2.10 E) needs this path's own luminance, separately from the sky it
        // was compared against -- only reachable at f2Path == 3u, the one path that actually traced.
        // averShadowLum: this file's standing luminance reduction (Rec.709, voxi_rt.hlsli:1654).
        f2Observed  = true;
        f2LumTraced = averShadowLum(indY);
        f2LumSky    = averShadowLum(averSkyIrradiance(s.N) * gAmbient.r);
    }
    radiance += s.kdAlbedo * indY;

    samplePos      = hitPos;
    sampleNormal   = s.N;
    // Clamped, not just left non-negative: averShadeDirect's visibility term (visSmithCorrelated,
    // `0.5 / max(lv + ll, 1e-7)`) blows up at a grazing hit (cos < 0.1 happens 1% of the time over a
    // uniform disc). The cone gather is structurally immune to this: it averages over a mip footprint
    // and PSVoxel clamps to AVER_VOX_MAXRAD before injection. This value becomes the reservoir's
    // STORED radiance, and a reservoir keeps its sample (maxReservoirAge 30 below) -- an unclamped
    // spike doesn't flicker one frame, it sits for thirty and spreads once a spatial pass exists. So
    // the clamp must land before the target pdf or reservoir, not only at final output.
    // NaN-safe, replacing a native clamp(): min(max(x,lo),hi) floors NaN to lo (this codebase's
    // documented comparison semantics); native clamp()'s NaN behaviour is compiler/driver codegen
    // order, not a language guarantee.
    {
        const bool bad     = any(isnan(radiance)) || any(isinf(radiance));
        nonFiniteCandidate = nonFiniteCandidate || bad;
        sampleRadiance     = bad ? float3(0.0, 0.0, 0.0) : min(max(radiance, 0.0), AVER_VOX_MAXRAD);
    }
    return true;
}

// ---- the call site's one entry point: candidate + RTXDI temporal resampling, in and out ----
// Drop-in replacement for coneTracedIndirect's contract (float3 diffuse radiance, `ao` out) --
// PSMainVoxi/PSRayDriven choose between the two by Settings::giMode only. `ao` stays 1.0 always:
// ReSTIR GI's one resampled candidate per pixel does not estimate a hemisphere-coverage fraction
// the way the cone gather's occlusion accumulator does, and a fudged one is worse than stating it
// NOT MODELLED -- the unified ambient ray (gAmbientParams.x, High/Epic) already supplies AO
// independently, at the tier this gap matters least. STILL TRUE after F1-F3 (R0/R2/R3): those
// correct RADIANCE this function already carried, none touch `ao` -- R4 (no AO from ReSTIR at
// Low/Medium, where this is the ONLY occlusion signal) stays open, deliberately deferred.
#if AVER_GI_CHECKERBOARD
// `static`, not a parameter, like gGiPoisonPdfHit above: giRestirIndirect's signature is shared
// with PSMainVoxi/PSRayDriven's non-checkerboard call sites and must not change. CSRdGi (voxi.hlsl,
// the only definer of AVER_GI_CHECKERBOARD) sets this per invocation before calling
// giRestirIndirect: is this pixel the half NRD expects fresh data for, or the half REBLUR reconstructs?
static bool gGiCbSkip = false;
#endif

// ---- U1 (2.10 A) / B2: VISIBILITY-MODE + F2/F3 PATH DECODE, factored out of giRestirIndirect ----
// Moved out whole, unchanged, so CSRdGiTrace (voxi.hlsl, Settings::rayDrivenGiSplit) can compute the
// same f2Path/rho2 for the same pixel before giRestirIndirect runs -- the split's premise is that
// trace and resample passes agree on the path without re-deriving it differently. giRestirIndirect
// (below) calls this once and unpacks the result into locals of the same names this block used to
// declare directly, so nothing past the decode has to change.
//
// visMode is settings_.giRestirVisibility as C++ clamped/packed it (VoxiRenderer::beginShadowHistory,
// givis::packAmbientW) -- 0 No ray, 1 Reconstructed, 2 HalfResolution, 3 Full. halfBound also
// requires the half-res pair bound THIS frame (bit 4): an allocation failure (2.11) leaves visMode==2
// with the pair unbound, which must behave as Full, not read a null descriptor. tracedPx is this
// pixel's phase-table verdict (giVisTracedPixel) when Half is active, trivially true otherwise.
//
// giVisReconstruct is called HERE once, not separately for F2/F3: both rays share one
// reconstruction, and calling it twice would retrace the same four taps for no new information.
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
    // Settings::giRestirSpatialSamples (0..15; splits spatial from temporal reuse for the still-open
    // moving-camera fade bisection, Voxi.hpp), packed one nibble above visMode (givis::packAmbientW).
    // 15 is the AUTO sentinel: the override it enables applies only after the motion discount
    // computes its own numSamples (see that override's own comment for why it must sit there).
    const uint spatialSamples = ((uint)gAmbientParams.w >> 12) & 15u;
    // Settings::giRestirBiasCorrection (bits 16-17) / giRestirMaxHistory (bits 18-23): dials over how
    // a YOUNG reservoir is weighted -- headless captures show a partially-converged reservoir reads
    // brighter than both the no-reuse and converged estimates. Decoded here with the rest of
    // gAmbientParams.w, same convention as visMode.
    const uint maxHistory     = ((uint)gAmbientParams.w >> 18) & 31u;
    // Not a ternary: HLSL's conditional operator only supports numeric scalar/vector/matrix results,
    // never a struct (DXC: "conditional operator only supports results with numeric scalar, vector,
    // or matrix types"), so `halfBound ? giVisReconstruct(...) : (GiVisRecon)0` is a compile error.
    // `rec` starts at that same default, overwritten only when halfBound.
    GiVisRecon rec = (GiVisRecon)0;
    if (halfBound) rec = giVisReconstruct(wpos, N, pixel, frameIdx);
    // Path numbers, shared by F2/F3 except where a legacy bit singles one out below: 3 trace,
    // 2 half-res ratio, 1 reconstructed (voxel cone / temporal-only), 0 legacy/no-ray.
    uint f2Path = 3u, f3Path = 3u;
    if (visMode == 1u)                       { f2Path = 1u; f3Path = 1u; }
    if (halfBound && !tracedPx && rec.valid) { f2Path = 2u; f3Path = 2u; }   // no valid reconstruction: trace, as Full
    if (((uint)gAmbientParams.z & 4u) != 0u || visMode == 0u) f2Path = 0u;   // legacy bit wins (2.8)
    if (((uint)gAmbientParams.z & 8u) != 0u || visMode == 0u) f3Path = 0u;
#if AVER_GI_CHECKERBOARD
    // Skipped pixels trace no F3 visibility ray either, only the Full-traced path (3) -- Half's own
    // cheap reconstructed visibility (rec.v3) is deliberately left alone. f2Path goes to 0 since no
    // candidate is traced (path debug view paints yellow, "no ray"); a skipped pixel observes
    // nothing this frame (f2Observed/f3Observed stay false) and the history write below carries the
    // reconstruction forward instead.
    // THE HALF-RES HISTORY LOSES HALF ITS SUB-PIXEL POSITIONS, NOT HALF ITS REFRESH RATE: with a
    // fixed offset between the checkerboard and kGiVisPhase's own 4-frame cycle, the same two phases
    // always land on skipped pixels, so each block refreshes from one fixed diagonal pair (which
    // shifts whenever NRD skips a frame) instead of all four positions -- still covers both rows and
    // both columns, so left as is; exempting the phase pixel from the skip would restore all four at
    // +1/8 more candidate rays.
    if (gGiCbSkip) {
        f2Path = 0u;
        if (f3Path == 3u) f3Path = 0u;
    }
#endif

    // rho2: F2's reconstructed-non-traced path (f2Path==2u) reads the neighbourhood's occluded/
    // unoccluded sky RATIO OF EXPECTATIONS (sum(w*g)/sum(w*b), giVisReconstruct's own wsum/gsum/
    // bsum), not an average of per-tap ratios (2.10 D.6; GiVisibilityTest checks the two estimators
    // disagree on a skewed field). Computed once here rather than inside giTraceInitialCandidate,
    // since F2 is the only reader.
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

    // U1 (2.10 A) / B2: decode the visibility mode and both rays' paths, once, up front -- see
    // giDecodePaths above. Factored out so CSRdGiTrace's dispatch (voxi.hlsl) can compute an
    // identical f2Path/rho2 for the candidate it traces on this function's behalf when
    // Settings::rayDrivenGiSplit is on (AVER_GI_SPLIT).
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
    // Skipped pixels trace no fresh candidate: `initial` stays empty (freshValid false below), the
    // correct half-rate behaviour -- spatio-temporal resampling below still runs for every pixel, so
    // a skipped pixel's reservoir is still reprojected/restored from last frame's, keeping the reuse
    // chain valid under motion even on a frame it traces nothing new.
#if AVER_GI_SPLIT
    // B4: the candidate is already traced -- CSRdGiTrace (voxi.hlsl) ran giTraceInitialCandidate for
    // this pixel with the identical f2Path/rho2 giDecodePaths just computed (both call sites decode
    // from the same gAmbientParams/gGiCbSkip inputs), stored in gRdGiCand[gGiCandIdx] (gGiCandIdx set
    // by CSRdGi to the same row-pitch index CSRdGiTrace wrote under). Reading it back and taking the
    // identical branch below reproduces the non-split branch's `initial` bit-for-bit (full float
    // precision, no quantisation in the hand-off) -- only which dispatch pays for the ray moves.
    // Skipped the same way the trace is: gGiCbSkip pixels never had a candidate traced in either
    // compile, so there is nothing in gRdGiCand[gGiCandIdx] to read.
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
        // Same floor: this is the gate that actually decides what gets stored. The recomputed
        // cosTheta matches the sampled one up to precision (dir is built from it and samplePos lies
        // along dir), so a `> 0.0` here was strictly weaker than giTraceInitialCandidate's own reject
        // and quietly let the 1/cos blow-up through anyway.
        if (cosTheta > AVER_GI_MIN_COS)
            initial = RTXDI_MakeGIReservoir(samplePos, sampleNormal, sampleRadiance, cosTheta / PI);
    }
#endif

    // ---- F3's OWN COPY OF THIS FRAME'S FRESH CANDIDATE, TAKEN BEFORE RESAMPLING CAN REPLACE IT ----
    // `initial` goes into RTXDI_GISpatioTemporalResampling below, which is free to hand back a
    // completely different reservoir in `result` (a temporal/spatial tap). The visibility gate
    // further down (F3/R3) needs to tell the two apart: this frame's own candidate already had its
    // visibility proven by giTraceInitialCandidate's ray, so re-tracing would be pure waste; anything
    // else reaching `result` has never been visibility-tested. Captured here since `result` may not
    // still equal `initial` once the resampling call (which takes it by value) returns.
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

        // screenSpaceMotion.xy reuses rtReprojectHistory's reprojection recipe (gPrevViewProj +
        // gSceneViewport), not averGBufferVelocity's -- that returns destination-minus-source
        // (UpscalerNeeds::MotionVectors), the opposite sign RTXDI_GITemporalResampling wants
        // (`prevPos = pixelPosition + screenSpaceMotion.xy`). .z is the depth delta a static point's
        // reprojected depth would show, for RTXDI's disocclusion test.
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
        // Settings::giRestirDepthThreshold / giRestirNormalThreshold (Voxi.hpp): RTXDI's reuse-
        // similarity tolerances, lifted from a shader literal so the moving-camera fade bisection can
        // sweep them without a rebuild (paired with spatialSamples for that same purpose). 0.1/0.5
        // are this field's compiled-in DEFAULT (Voxi.hpp), unchanged from the old literal; the engine
        // clamps any caller value to [0.001,1.0]/[0.0,0.999] (Voxi.cpp) before it reaches here.
        stparams.depthThreshold        = 0.1;
        stparams.normalThreshold       = 0.5;
        // ---- 1, NOT 8: A YOUNG HISTORY OVERSHOOTS, AND ITS DEPTH BOUGHT NOTHING NRD DID NOT ----
        // RTXDI_CombineGIReservoirs sums M across fresh/temporal/spatial streams with only a per-tap
        // clamp to this number, so a lineage a few frames old claims more independent samples than it
        // holds and BASIC normalisation over-weights it. MEASURED on PTTest's NewSponza (fixed camera/
        // exposure): image fell from 82 to 68.5 (tonemapped mean) over ~300 frames after level load;
        // resolving each pixel from its fresh candidate alone removed the fall (71.9 -> 71.3). Same
        // overshoot follows every GI history reset (including a sun change, see
        // VoxiRenderer::beginShadowHistory) as a short bright bump before settling. At 1 the overshoot
        // is gone with the same settled value (arcade wall, linear x1000: 1.7 at frame 45 and at frame
        // 900, where 8 read 2.5 then 1.7) and no measurable noise cost after NRD: still-frame grain
        // 0.161 vs 0.160, flicker 0.0161 vs 0.0160, 6-degree wobble 1988 vs 1976 firefly outliers/frame
        // with identical flicker.
        //
        // EARLIER 20 -> 8 ANALYSIS, kept because the spatial tap count below still leans on it: the
        // weight recursion with K taps capped at M plus the fresh candidate's M=1,
        //     W_next = [ (PI/cos)*1 + K*M*J*W ] / (1 + K*M),
        // is stable in the mean only while E[J] < (1+K*M)/(K*M). At the old 20/numSamples 8 that
        // margin was 0.56% -- too tight given a 32px spatial tap's Jacobian mean is nowhere near 1
        // under camera motion (RAB_ValidateGISampleWithJacobian used to only clamp each tap to
        // [1/25,25], bounding one step but not the compounding): at rest J~1 and the estimate crept
        // correct, in motion J scattered and W ran away bright, whole neighbourhoods sharing one
        // runaway sample. 8 taps of 8 (K*M=24) gave a 4.2% margin instead -- roughly eight times the
        // old headroom -- trading a shorter effective sample count (noise, which a denoiser can fix)
        // for stability (which it can't).
        // UPDATE: now that RAB_ValidateGISampleWithJacobian reports 1.0 for every accepted tap, E[J]=1
        // by construction, so K*M/(1+K*M) is stable for ANY K*M -- the runaway this margin analysis
        // defended against can no longer arise, making the drop to 1 a free choice, not a trade.
        // voxi.giRestirMaxHistory, default 0 (the camera-motion fade fix); 602d1b06's value of 1 is
        // what raising it restores. See Settings::giRestirMaxHistory (Voxi.hpp).
        stparams.maxHistoryLength      = maxHistory;
        // ---- 0, AND THE 1 IT REPLACES WAS DISARMING EVERY SPATIAL TAP ----
        // `usingFallback` inside RTXDI_GISpatioTemporalResampling is a LATCH: set once temporal taps
        // fail, never cleared, after which the fallback tap AND all 8 spatial taps run with
        // RTXDI_IsValidNeighbor short-circuited. Camera motion is precisely what makes temporal taps
        // fail, so in motion every 32px spatial tap landed on arbitrary pixels with no depth/normal
        // test (RAB_AreMaterialsSimilar returns true unconditionally here), and the only remaining
        // gate (RTXDI_IsValidGIReservoir, M != 0) tests the reservoir slot, not the surface. COST (the
        // motion half of the reported bug): an invalid neighbour enters the MIS numerator (targetPdf *
        // jacobian * W * M) but contributes nothing to piSum (its pdf there is correctly 0),
        // inflating W by up to 180x (9 streams x M=20 vs a seed of 1) -- the frame washing out the
        // instant the camera moves. Turning this off costs the fallback's job of finding SOME history
        // after a disocclusion, the correct trade since it was reusing 32px-distant strangers instead.
        stparams.enableFallbackSampling = 0;
        // BASIC. Switching it off was measured 2026-09-19 to move the camera-motion overshoot by
        // nothing at all, so the mode is not a lever on it either way.
        stparams.biasCorrectionMode    = RTXDI_BIAS_CORRECTION_BASIC;
        stparams.maxReservoirAge       = 30;
        stparams.enablePermutationSampling = 0;
        stparams.uniformRandomNumber   = frameIdx * 2654435761u;
        // NVIDIA's own ReSTIR GI sample defaults for the spatial half added on top of temporal reuse:
        // 8 taps at 32px -- wide enough to bleed indirect light across a corner if nothing were
        // watching it, but RTXDI_IsValidNeighbor's depth/normal test (run for every neighbour, temporal
        // jitter and spatial tap alike, inside SpatioTemporalResampling.hlsli) is what keeps reuse local
        // AT REST, not the radius -- see the motion discount below for why "at rest" matters. 2, NOT
        // NVIDIA'S 8, AT REST, for the stability reason maxHistoryLength above spells out rather than
        // to save the taps: those 8 are the default for a STANDALONE spatial pass running once over an
        // already-normalised frame; fused into the temporal pass here they multiply the history length
        // in K*M, and taking K from 9 to 3 is most of the stability margin.
        //
        // ---- DISCOUNT SPATIAL REUSE UNDER MOTION (the change most supported by the ghosting
        // measurement in this file's header) ----
        // WHY: under rotation the reprojected position moves every frame, so the same 32px
        // neighbourhood approved this frame is a different one next frame -- "locally valid" (all
        // RTXDI_IsValidNeighbor checks) never meant "the same sample twice", and that function has no
        // notion of history. Fewer taps from a narrower ring does not fix any one tap's Jacobian
        // (RAB_ValidateGISampleWithJacobian above already does that); it reduces how much
        // never-converging spatial content combines per frame in motion, at the cost of more noise for
        // the denoiser. SAME SHAPE AS rtShadowTemporal's velocity discount, which already answers "how
        // much should a reused estimate be trusted as reprojection distance grows" for a different
        // signal, traced shadow visibility (voxi_rt.hlsli ~:1695-1712:
        // `t = saturate(length(velocityPx)/32.0); weight = lerp(0.9,0.5,t)`), reused rather than
        // invented -- same 32.0 divisor since motionPx is the same "pixels of reprojection" quantity.
        // REASONED, NOT MEASURED for THIS discount specifically; restated from that existing answer.
        // FLOORS, NOT ZERO AT FULL MOTION: samplingRadius 0 would collapse every offset to the centre
        // pixel (RTXDI_CalculateSpatialResamplingOffset, Rtxdi/GI/SpatialResampling.hlsli, scales
        // kGiNeighborOffsets by the radius), re-tapping the temporal result under a different name
        // rather than reusing nothing. 1 tap at 8px is the smallest neighbourhood that still counts as
        // spatial reuse.
        const float motionPx = length(screenSpaceMotion.xy);
        const float motionT  = saturate(motionPx / 32.0);
        stparams.numSamples     = (uint)round(lerp(2.0, 1.0, motionT));
        stparams.samplingRadius = lerp(32.0, 8.0, motionT);
        // voxi.giRestirSpatialSamples: overrides the motion discount's own count, for the same fade
        // bisection giRestirDepthThreshold/giRestirNormalThreshold above are for. Must sit here,
        // after the lerp, since it overrides the field the lerp just computed. 15 (AUTO) skips this
        // entirely (byte-identical to today's image); any other value replaces the discount outright:
        // 0 = no spatial reuse (isolates whether the fade survives with it disabled), 1..8 pins the
        // tap count regardless of motion. min(decoded, 8u) is a defensive ceiling on top of Voxi.cpp's
        // [0,15] clamp (9..14 are reachable non-sentinel patterns), matching numSamples' own K*M ceiling.
        if (spatialSamples != 15u)
            stparams.numSamples = min(spatialSamples, 8u);
        // U1 (2.10 C): Reconstructed forces temporal-only, after the motion discount, not before.
        // f3Path==1u only ever comes from visMode==1u itself, never from Half's non-traced/
        // no-valid-reconstruction limbo collapsing to Reconstructed (see the decode block above), and
        // means giRestirVisibility selected Reconstructed, whose cost model (2.10 F) accounts for the
        // spatial half as zero. Set after the lerp so it always wins regardless of motionT
        // (GiVisibilityTest asserts this line follows the lerp textually).
        if (f3Path == 1u) stparams.numSamples = 0u;
        RTXDI_RuntimeParameters rParams = (RTXDI_RuntimeParameters)0;   // no checkerboard
        // neighborOffsetMask: RTXDI_CalculateSpatialResamplingOffset masks its index by this before
        // touching kGiNeighborOffsets, so it must match that table's size minus one, or the mask opens
        // the index onto whatever garbage sits past the array's 32 entries. frameIndex is NOT read
        // anywhere under third_party/rtxdi/Include (checked) -- it only feeds ReSTIRDIContext's own
        // uniformRandomNumber/checkerboard bookkeeping on the DI half's CPU side, unused here
        // (uniformRandomNumber is hashed above instead) -- set anyway so this SDK-expected field isn't
        // silently left zeroed.
        rParams.neighborOffsetMask = kGiNeighborOffsetMask;
        rParams.frameIndex         = frameIdx;

        // Argument order differs from RTXDI_GITemporalResampling above: SpatioTemporalResampling.hlsli
        // takes sourceBufferIndex before screenSpaceMotion (opposite of TemporalResampling.hlsli).
        // The wrong order would still COMPILE -- HLSL implicitly truncates a float3 argument passed
        // where a scalar uint is expected (taking .x) and broadcasts a scalar into a float3 position --
        // so this order is trustworthy only because it was read off the vendored signature.
        result = RTXDI_GISpatioTemporalResampling(pixelPos, surface, 1u - writeSlice, screenSpaceMotion,
                                                   initial, rng, rParams, resParams, stparams);
    }

    // ---- THE RESIDUAL AFTER THE cosR FLOOR ABOVE -- THREE BACKSTOPS TRIED, ALL MEASURED, NONE KEPT ----
    // --firefly-metric (SandboxApp.cpp) is the first thing that could count a ReSTIR GI firefly rather
    // than guess at one -- the measurement the four prior rounds of fixes to this bug shipped without.
    // It found a real, motion-triggered spike on PTTest Sponza giMode 1 --cam-wobble: outlier count
    // sits in a ~150-350 band, jumps past 800-1200 for single sampled frames with the denoiser off,
    // stays low across 200 frames with --denoiser 1 (a screenshot at one spiking frame confirms it by
    // eye -- small bright dots across an otherwise dark wall, gone with the denoiser on). Flooring
    // cosR in RAB_GetGISampleTargetPdfForSurface measurably reduced how often this fires but left a
    // residual, still motion-triggered.
    // THREE BACKSTOPS TRIED AGAINST THAT RESIDUAL, each measured, each a no-op -- kept as recorded
    // negative results so the hypothesis isn't retried:
    // 1. Wave-level average (WaveActiveSum/CountBits) -- the closest a pixel shader gets to
    //    RTXDI_GIBoilingFilter's groupshared reduction (needs a compute thread group, illegal here;
    //    moving this pass onto compute is a real restructuring, out of this task's file ownership).
    //    No improvement: frame 70 still spiked to 1016 (was 980), plus a NEW spike at frame 110.
    // 2. Each pixel against its own last-frame reservoir (un-reprojected). No effect either: frames
    //    110/150/190 still spiked (1039/1082/903).
    // 3. Absolute ceiling on finalised weightSum, multiples of PI/AVER_GI_MIN_COS (~62.8, the most a
    //    single candidate's own 1/pdf can be). Independent of neighbour/history. Swept 8x down to 1x,
    //    the same ceiling a single candidate itself is already held to: spike frames UNCHANGED at
    //    every multiple (1x: 110=1042, 115=1070, 150=843; unclamped baseline 1024/1066/1037). A
    //    ceiling this tight doing nothing is decisive: no reservoir here exceeds even a single
    //    candidate's own honest weight.
    //
    // RULES OUT every weightSum-side explanation: the widespread, correlated brightening is not any
    // reservoir's weight running away. It's GENUINE MONTE CARLO VARIANCE -- many nearby pixels, each
    // drawing its own independent cosine-weighted candidate direction (rtDiscSample/rtHash,
    // giTraceInitialCandidate above), having a small but real chance of landing on the same narrow,
    // high-contrast light path
    // (glimpsing the sun through a gap) at this camera angle. Not a bug to clamp away -- that would
    // discard real light transport a single candidate can't smoothly resolve alone, which is exactly
    // what RTXDI's spatiotemporal reuse and this engine's REBLUR integration already do (the
    // denoiser-on band above). A fourth unmeasured backstop would repeat the pattern this task stops.

    // ---- W6/M5: NO LONGER A KNOWN LIMITATION -- GATED ON gAverHistoryWrite, ON BY DEFAULT ----
    // Until this task, a pixel covered by TRANSLUCENT geometry was written to this reservoir slot
    // TWICE per frame: PSRayDriven writes every pixel first, then the blended glass/water replay
    // (PSMainVoxi, same compiled shader binary, differing only in blend/depth state) overwrites it.
    // No #define or per-pass constant could tell them apart from inside this function -- only a
    // PER-DRAW signal can, and gAverHistoryWrite is that signal (set at the top of PSMainVoxi,
    // voxi.hlsl). The OLD degrade-gracefully path -- the opaque surface reprojecting into a slot
    // that still described the glass hit, RTXDI_IsValidNeighbor's depth/normal test rejecting the
    // mismatch, and one extra frame of noise while reuse rebuilt from scratch -- is what
    // voxi.legacyBlendedHistoryWrite (gAmbientParams.z bit 32) still reproduces byte-identical for
    // A/B (forces the flag true unconditionally). PSRayDriven's writes are never suppressed -- a
    // blended pane never reaches that entry point, so it sets the flag true unconditionally.
    // ---- NEVER STORE A CORPSE: weightSum 0 with M != 0 POISONS EVERY PIXEL THAT TAPS IT ----
    // RTXDI_FinalizeGIResampling writes only weightSum, never M or radiance -- so a reservoir whose
    // MIS numerator finalised to zero is stored with weightSum=0, a fully bright radiance, and M up
    // to 181. RTXDI_IsValidGIReservoir tests only `M != 0`, so next frame that corpse is a "valid"
    // neighbour: it adds `ps * M` to piSum (denominator) but `targetPdf * jacobian * 0 * M` --
    // exactly zero -- to the numerator. It is pure denominator. THAT MAKES BLACKNESS CONTAGIOUS (the
    // still-camera half of the bug): one dead tap among nine costs survivors ~21x, they finalise
    // darker (some to zero), and spread it onward -- converging to black over a second or two at
    // rest, undone the moment the camera moves and rejects the history.
    // d4e56396 is what manufactures the corpses (the other half of that fix): making
    // RAB_GetGISampleTargetPdfForSurface return 0 for an invalid surface was right -- an absent
    // surface must not contribute a weight -- but a zero arriving as `pi` finalises the whole
    // reservoir to zero with nothing stopping it being stored. An M=0 reservoir is skipped by
    // RTXDI_IsValidGIReservoir at the top of the neighbour loop, so emptying it here removes it
    // from both sums instead of leaving it in one.
    //
    // ---- EXTENDED: NaN/Inf stops here too, not only exact-zero -- THE FIX THAT MATTERS MOST HERE ----
    // A comparison against NaN is false, so the corpse guard's `<= 0.0` silently let a NaN weightSum
    // through to RTXDI_StoreGIReservoir (RTXDI_CombineGIReservoirs' `weightSum += risWeight`,
    // third_party/rtxdi/Include/Rtxdi/GI/Reservoir.hlsli, propagates NaN unconditionally -- unlike
    // this codebase's own comparison-based idioms, `+=` does not self-heal; RTXDI_FinalizeGIResampling
    // only guards an exact-zero denominator -- neither vendored function protects against this). Once
    // stored, RTXDI_IsValidGIReservoir's `M != 0` test means a poisoned reservoir reads as valid
    // forever: every later comparison against NaN is false, so it can never be selected out, and the
    // 8-32px spatial/temporal taps spread it one hop per frame, forever.
    // This guard doesn't need to know where the first non-finite value came from -- whatever
    // manufactures it (the candidate-radiance clamps below, the target-pdf guard, or a cause nobody
    // has found yet), a reservoir never STORED non-finite can never spread or persist. It's the
    // circuit breaker; the other guards in this file (giTraceInitialCandidate's clamps,
    // RAB_GetGISampleTargetPdfForSurface's return) are risk reduction on top of it, not a substitute.
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

    // NEXT FRAME'S "previous surface" -- see gGiSurfPosHist/gGiSurfNrmHist's declaration for the
    // two-texture format and the 0-packed-normal sentinel RAB_GetGBufferSurface tests for "nothing
    // written here". Nudged off exactly zero so a real packed normal never collides with the sentinel.
    const uint packedN = RTXDI_EncodeNormalizedVectorToSnorm2x16(N);
    // W6/M5: gated the same way as the reservoir store above, same reason -- a blended-replay
    // fragment writing its own surface here would make next frame's giVisReconstruct/
    // RAB_GetGBufferSurface taps silently read the pane's position/normal instead of the opaque
    // surface's. Both channels share one gate since they're always written together.
    if (gAverHistoryWrite) {
        gGiSurfPosHistOut[pixelPos] = wpos.xy;
        gGiSurfNrmHistOut[pixelPos] = float2(wpos.z, asfloat(packedN == 0u ? 1u : packedN));
    }

    // Hoisted to function scope so the poison-view combination at the end (after the NRD block) can
    // still read them.
    bool giPoisonEstHit = false;
    bool giPoisonNrdHit = false;
    // Not a non-finite guard catching corruption, but AVER_VOX_MAXRAD's ceiling clamp engaging on a
    // FINITE value (see the POISON DEBUG VIEW comment below, and Settings::giRadianceCeiling for why
    // hitting it paints solid white downstream). giPoisonNrdCeilHit is set in the NRD block further
    // down, read at the same combination site.
    bool giPoisonEstCeilHit = false;
    bool giPoisonNrdCeilHit = false;

    float3 outDiffuse = 0.0;
    if (RTXDI_IsValidGIReservoir(result)) {
        // Recomputed here against THIS pixel's (wpos, N), not reused from giTraceInitialCandidate's
        // cosTheta: temporal resampling can hand `result` back holding a completely different sample
        // (the whole point of a reservoir), so the receiving cosine must derive from wherever
        // result.position actually landed. A reservoir stores a world POSITION, not a cached angle,
        // for exactly this reason.
        const float3 toSample = result.position - wpos;
        const float  dist2    = dot(toSample, toSample);
        const float  cosR     = dist2 > 1e-8 ? saturate(dot(toSample * rsqrt(dist2), N)) : 0.0;

        // ---- F3 (R3): A REUSED SAMPLE HAS NEVER BEEN CHECKED FOR VISIBILITY FROM HERE ----
        // giTraceInitialCandidate's ray already proved THIS frame's fresh candidate visible from
        // wpos. Temporal/spatial resampling can swap `result` for a different reservoir (another
        // frame's or a neighbour pixel's position), and nothing in RTXDI's own resampling traces a
        // ray between this receiver and that stored position (RAB_GetTemporalConservativeVisibility
        // exists for that but sits unused, since this file selects BASIC). On a coplanar pixel just
        // the far side of an occlusion edge, a neighbour's sunlit sample was credited to a receiver
        // that provably can't see it.
        // ONE RAY, SKIPPED WHEN NOT NEEDED: gAmbientParams.z bit 8 TRUE restores HEAD exactly (no
        // ray, A/B only). FALSE (corrected default) still skips the ray for this frame's own fresh
        // candidate (already proven visible) and a degenerate same-point sample (dist2 <= 1e-8);
        // every other case (any reuse) gets exactly one ray.
        // SHADING ONLY, NEVER THE RESERVOIR: a visibility miss darkens only what this pixel sees this
        // frame, without erasing the sample for a neighbour resampling it with a different view.
        // Unbiased per RAB_GetGISampleTargetPdfForSurface's own header: RIS with a visibility-blind
        // target pdf, multiplied here by the true f*V, stays unbiased since the pdf's support is a
        // superset of the visible one.
        // U1 (2.10 C): f3Path == 3u now guards the ray; the rest of the condition is unchanged text.
        // `reused` restates the same test the ray's condition computes, named once so the Half
        // branch below doesn't repeat it a third time (the ray branch keeps its own inline
        // expression so its condition text, checked by GiVisibilityTest, stays literal).
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
            qv.TraceRayInline(gScene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | gAverRtSecondaryRayFlags, AVER_RT_MASK_OPAQUE_ALL, rv);
            averRtProceedSolid(qv);
            if (qv.CommittedStatus() == COMMITTED_TRIANGLE_HIT) visF3 = 0.0;
            f3Observed = true;
        } else if (f3Path == 2u && reused) {
            // HALF, non-traced pixel with a valid reconstruction: the neighbourhood's reused-sample
            // visibility stands in for a ray. `reused` still gates this -- this frame's own fresh,
            // already-proven candidate needs no substitute.
            visF3 = rec.v3;
        }

        // ---- U1 (2.10 E): THE HALF-RESOLUTION HISTORY WRITE -- ONE TEXEL PER 2x2 BLOCK, PER FRAME ----
        // Gated on gAverHistoryWrite (W6/M5, same reasoning as every other gated write), halfBound
        // (Half running with its pair bound this frame), and tracedPx (only the block's traced pixel
        // has a real observation to store -- see giVisTracedPixel: exactly one pixel per block writes,
        // so a fallback-traced pixel elsewhere in the block does not also write here and race the
        // phase pixel's own write).
        // h: same motion-discounted EMA shape as rtShadowTemporal's history write (0.9/0.5 pair) --
        // AVER_GI_VIS_HIST_WEIGHT at rest, falling to 0.5 as rec.motionPx (computed once inside
        // giVisReconstruct, reused here) approaches the same 32px knee.
        // r/g/b: each channel is this frame's fresh observation blended toward the reconstructed
        // history (OBSERVED ray true), or the reconstructed history carried forward unchanged (no
        // fresh observation but rec.valid), or the honest prior (visF3 default 1.0 "nothing
        // occluding yet", 0.0 for the luminance channels) only on a genuinely first write (Observed
        // and rec.valid are never both false while f2Path/f3Path==3u traced this frame, since a trace
        // always sets its own Observed flag).
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
        // folds in 1/pdf and the resampling normalisation -- RTXDI_FinalizeGIResampling produces it).
        // RTXDI's contract leaves the rest of the integrand to the caller: for a Lambertian receiver
        // that's cosR/PI, and cosR cancels the 1/cosTheta baked into weightSum by the cosine-weighted
        // sample (giTraceInitialCandidate) -- dropping it let the estimator diverge toward the
        // tangent plane. Also folded in: gVoxelParams.y (giIntensity), which coneTracedIndirect
        // already applies (`sum.rgb * gVoxelParams.y`) but this path never did, so Settings >
        // Rendering > GI intensity (and RENDER.GIINTENSITY) silently did nothing when giMode
        // selected ReSTIR over the cone gather. Applied before the clamp below, matching
        // coneTracedIndirect's order, so the two estimators stay comparable at the same intensity.
        // visF3 (F3/R3) is the one factor not part of RTXDI's finalised estimator: 1.0 unless this
        // frame's visibility ray found an occluder, zeroing this pixel's shaded value without
        // touching the stored reservoir.
        const float3 est = result.radiance * (cosR * result.weightSum / PI) * gVoxelParams.y * visF3;

        // max(NaN, 0.0) silently returns 0.0 (every NaN comparison is false), so an unguarded NaN
        // estimator (a near-zero Jacobian, a reservoir aged past a disocclusion) would become pure
        // black with nothing in the log to say why -- same shape as acesTonemap's negative-radiance
        // guard (color.hlsli, ded8784a), both now resolving to confident BLACK (not the WHITE an
        // older comment here claimed, before that fix). Zero is still right for a broken sample;
        // the isnan/isinf check just makes that a decision.
        giPoisonEstHit = any(isnan(est)) || any(isinf(est));
        // THE NEW GUARD: not "is this broken" but "is this about to be painted solid white by the
        // ceiling that exists to hide exactly this" -- a finite value can still saturate acesTonemap
        // (flat white ~x=4-5) once clamped to AVER_VOX_MAXRAD (see Settings::giRadianceCeiling).
        // Checked against estNonNeg (the value the clamp below actually clamps, so a legitimately
        // negative value floored to zero can't register as a false ceiling hit), skipped when
        // giPoisonEstHit already fired -- a non-finite value is a different failure, not "above ceiling".
        const float3 estNonNeg = max(est, 0.0);
        giPoisonEstCeilHit = !giPoisonEstHit && any(estNonNeg > AVER_VOX_MAXRAD);
        outDiffuse = giPoisonEstHit ? float3(0.0, 0.0, 0.0)
                                     : min(estNonNeg, AVER_VOX_MAXRAD);
    }

    // ---- hand this frame's estimate to NRD, and take back last frame's ----
    // The write is the RAW per-pixel estimate, never the denoised value read below -- feeding a
    // filter its own output is the IIR trap the sky-occlusion history write documents; it would
    // also fight REBLUR's own temporal accumulation, the entire job of the permanent pool NRD keeps
    // for this signal.
    // Both channels are encoded to REBLUR's contract here, and neither half fails loudly if skipped
    // (NRD's pack/unpack helpers are things the producer/consumer call; no NRD pass applies them for
    // you) -- which is why this was wrong in two independent ways until
    // REBLUR_FrontEnd_PackRadianceAndNormHitDist was actually read:
    // ALPHA: normalised by REBLUR's own divisor, NOT by giMaxDistance. Used to divide by giMaxDistance
    // (4000cm), assuming one hitDistParams could describe both this signal and sky-occlusion -- it
    // can't: REBLUR reconstructs a real distance by multiplying back (A + |viewZ|*B) * lerp(C,1,smc),
    // and sizes its blur kernel, disocclusion test and variance estimate off that distance, so a
    // 40m divisor against a true ~4m made every bounce read as point-blank, undenoisable regardless
    // of anti-firefly settings. roughness=1 (purely diffuse) makes NRD's smc curve 1.0, so
    // lerp(C,1,smc) collapses to 1 and C drops out here (still set on the C++ side).
    // RGB: YCoCg, not linear RGB -- REBLUR_Config.hlsli's REBLUR_USE_YCOCG 1 means the front-end pack
    // converts unconditionally, so linear RGB here is a confidently wrong colour, not a format error.
    // The inverse is applied where gNrdGi is read below; the two are a matched pair.
    // ACTIVE PIXELS WRITE NRD'S INPUT PACKED; SKIPPED PIXELS DON'T WRITE AT ALL -- half-rate GI's
    // contract with REBLUR (this file's AVER_GI_CHECKERBOARD header comment) is that the
    // checkerboard's other half is only reconstructed by the denoiser. giNrdInPos changes because
    // NRD's checkerboard packs IN_DIFF_RADIANCE_HITDIST into the LEFT HALF of the input, one texel
    // per horizontal pixel PAIR (NRDSettings.h, CheckerboardMode), so the traced half of every
    // (2k, 2k+1) pair writes the SAME texel -- the `!gGiCbSkip` gate above is what keeps that write
    // from racing (only one active). With the macro at 0 and bit 17 clear (every frame but a
    // checkerboarded one), this is the plain `if (gGiRestirParams.x > 0.5)` it replaced, writing at
    // pixelPos.
    bool  giNrdInWrite = gGiRestirParams.x > 0.5;
    uint2 giNrdInPos   = pixelPos;
#if AVER_GI_CHECKERBOARD
    giNrdInWrite = giNrdInWrite && !gGiCbSkip;   // NRD reads only the traced half
    giNrdInPos.x >>= 1;                          // NRD's packed left-half layout
#else
    // Every other writer follows the packing on a frame CSRdGi packed: VoxiRenderer::
    // recordStagedRayDriven leaves bit 17 of gViewParams.w set for the rest of the frame (parity in
    // bit 16) after a checkerboard dispatch. PSMainVoxi's blended replay of an opaque draw matters
    // here (gAverHistoryWrite stays true for it): unpacked, its write would land on an unrelated
    // pixel's packed texel at twice its x; packed, it replaces its own pair's input exactly as its
    // own pixel's at full rate. Zero on every other frame (prePass resets the field).
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
        // _NRD_LinearToYCoCg, transcribed (NRD headers are confined to modules/render.nrd). Y is the
        // luminance REBLUR accumulates; Co/Cg are signed, hence RGBA16F rather than a UNORM format.
        const float3 ycocg = float3(dot(outDiffuse, float3( 0.25, 0.5,  0.25)),
                                    dot(outDiffuse, float3( 0.5,  0.0, -0.5 )),
                                    dot(outDiffuse, float3(-0.25, 0.5, -0.25)));
        // W6/M5: gated on gAverHistoryWrite, ON BY DEFAULT -- this is the NRD INPUT, not merely a
        // history texture, and the most visible half of C9's finding: an unguarded write here handed
        // NRD's permanent accumulation pool the PANE's GI estimate for a pixel the opaque surface
        // behind it also claims, with REBLUR then denoising across both indistinguishably.
        // legacyBlendedHistoryWrite (gAmbientParams.z bit 32) restores the old unconditional write,
        // byte-identical, for A/B.
        if (gAverHistoryWrite) gGiRadianceOut[giNrdInPos] = float4(ycocg, hitT);
    }

    // Last frame's denoised answer replaces this frame's raw one -- one frame of lag, which every
    // temporal consumer here already carries (the pass runs in beginShadowHistory, before the pixel
    // shader that produces the input has run) and NRD is built to be fed. Zero dimensions means the
    // pass didn't run this frame (no NRD in the build, backend refused register spaces, no G-buffer,
    // MSAA above 1x), and the raw ReSTIR estimate stands, noisy but correct.
    uint gw = 0, gh = 0;
    gNrdGi.GetDimensions(gw, gh);
    // W6/M5: `gAverHistoryWrite &&` leads this test, sibling to the write's own gate above -- a
    // blended-replay fragment must not read back the opaque surface's denoised answer as its own.
    // Skipping leaves `outDiffuse` at the raw estimate above, this fragment's own -- and since the
    // write above is gated the same way, a blended fragment neither reads nor writes the opaque
    // surface's NRD state.
    // ---- READ WHERE THIS SURFACE WAS LAST FRAME, NOT AT THIS FRAME'S PIXEL ----
    // gNrdGi is laid out on LAST frame's pixel grid. Loading at this frame's pixelPos is right only
    // while the camera is still; in motion it handed each pixel whatever GI last frame drew there.
    // Every other temporal consumer in this file reprojects
    // (rtReprojectHistory, rtReprojectAo); this one did not.
    // MEASURED, flying 30cm/frame down PTTest's gallery against a settled frame at the same pose:
    // the old read's error is STRUCTURED (a bright GI leak down a door frame, bands along a beam),
    // this read's is not (fine-scale diff 0.84 vs 0.28 codes, below one display level either way,
    // cause not isolated; mean 0.77 codes darker vs 0.53 brighter; still frame identical, MAD 0.01).
    // Same recipe as the temporal resampling above (gPrevViewProj, gSceneViewport), validated
    // against the surface ReSTIR kept for last frame (RAB_GetGBufferSurface): a texel counts only if
    // last frame's surface there lies on this pixel's tangent plane and faces the same way.
    // NO TAP MATCHES = a genuine disocclusion (the strip a turn reveals, the wall behind a passed
    // column): NRD never saw this surface. Such a pixel keeps the OLD unreprojected read (smooth,
    // but a neighbour's) -- never worse than before; the raw estimate was tried and is far worse
    // (paints the revealed strip black with sparse bright dots at one sample).
    // Legacy bit 64 (voxi.legacyNrdReadback) skips the reprojection entirely, the pre-fix read, A/B.
    // BILINEAR: four taps around the reprojected point, kept only if their surface matches,
    // renormalised (TAA-style). A nearest-texel 3x3 search measured the same (0.845 vs 0.841);
    // bilinear kept as the principled resample. At rest the taps collapse onto this pixel's centre:
    // measured identical still frames, MAD 0.01 ray-driven and 0.06 raster (run-to-run noise).
    const bool nrdReproject = gGiRestirParams.y > 0.5 &&   // a previous frame exists to reproject into
                              ((uint)gAmbientParams.z & 64u) == 0u;
    // Only a surface inside REBLUR's range has an output texel NRD wrote -- see AVER_NRD_DENOISING_RANGE.
    const bool nrdInRange = curLinearDepth < AVER_NRD_DENOISING_RANGE;
    float3 nrdYcocg = 0.0;
    float  nrdWsum  = 0.0;
    // gAverHistoryWrite: a blended fragment never reads NRD back (the gate below), so it skips the
    // four surface-history lookups too.
    if (gAverHistoryWrite && nrdInRange && nrdReproject && gw > 0u && gh > 0u) {
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
    if (gAverHistoryWrite && nrdInRange && gw > 0u && gh > 0u) {
        // _NRD_YCoCgToLinear, the matching half of the write above. REBLUR hands back what it
        // filtered, in the basis it filtered it in; NRD's own back-end unpack is this same transform
        // followed by a per-channel max against zero -- which this does NOT copy, see the gamut step
        // below. LINEAR, so blending the four taps in YCoCg and decoding once equals decoding each.
        const float3 y = nrdWsum > 1e-3 ? nrdYcocg / nrdWsum
                                        : gNrdGi.Load(int3(pixelPos, 0)).rgb;   // disocclusion / legacy
        const float  t = y.x - y.z;
        const float3 decoded = float3(t + y.y, y.x + y.z, t - y.y);
        // ---- THE SAME GUARD THE RAW ESTIMATOR ABOVE ALREADY HAS, NOW APPLIED HERE TOO ----
        // Before this, decoding NRD's output only floored the negative-chroma round-trip case,
        // trusting a THIRD-PARTY filter's output to be finite/bounded -- the exact trust outDiffuse's
        // own raw estimator refuses to extend to its own inputs. REBLUR's temporal accumulation and
        // variance-driven history clamp are third-party maths this file doesn't control, so a stray
        // NaN/Inf/huge value here must be caught at the point it's consumed (this file's own house
        // rule; see aver-negative-radiance-reads-bright). acesTonemap floors negative/NaN input at
        // zero (ded8784a, 2026-08-31, predating this guard), so this renders as confident BLACK now,
        // not the WHITE an older version of this comment claimed (describing pre-fix behaviour).
        //
        // THIS CODEPATH ONLY WENT LIVE RECENTLY: NRD was never created for a project whose ray-
        // tracing tier hadn't just changed (98b2b9a9 "NRD was never created unless SKY OCCLUSION was
        // on"), so `gw/gh` used to always read 0x0 here and this branch was dead code -- every prior
        // fix to this function (cosR floor, Jacobian symmetry, corpse-reservoir guard) was measured
        // against the RAW path alone, never this. Closing
        // the asymmetry is cheap insurance now that the branch is reachable for real: zero cost when
        // `decoded` is what it always is (finite, small), and it turns a would-be white-out into the
        // same "zero is the honest answer for a broken sample" the raw path already chose, rather
        // than a screen-filling flash with nothing in the log to explain it.
        giPoisonNrdHit = any(isnan(decoded)) || any(isinf(decoded));
        // ---- OUT OF GAMUT GOES TOWARD GREY AT THE SAME LUMINANCE, NOT CHANNEL BY CHANNEL ----
        // REBLUR filters Y and chroma separately, so a hard-clamped pixel (firefly, thin lamp frame
        // with no similar neighbours) can come back with luma pulled down and chroma kept -- a YCoCg
        // triple no RGB colour has. With Cg dominant that decodes to R and B below zero, G above, and
        // flooring each channel at zero (as this used to) turned it into PURE GREEN, capped by the
        // ceiling below at (0,16,0): the bright green dots with a bloom halo the owner saw scattered
        // around lamps/arches. Pulling toward grey by the smallest amount that brings every channel
        // >= 0 keeps the filtered luminance and invents no hue; Y <= 0 reads black.
        const float  lumaY  = max(y.x, 0.0);
        const float  lowest = min(decoded.r, min(decoded.g, decoded.b));
        const float3 inGamut = lowest < 0.0
                             ? lumaY + (decoded - lumaY) * (lumaY / max(lumaY - lowest, 1e-6))
                             : decoded;
        // NRD-side twin of giPoisonEstCeilHit: a finite REBLUR readback above the ceiling paints
        // solid white once tonemapped, invisible to the isnan/isinf guard above. Skipped when
        // giPoisonNrdHit fired. Scaled down as a whole, not per channel -- same reason as the gamut
        // step: a per-channel min would shift hue toward whichever channels were under the ceiling.
        const float peak = max(inGamut.r, max(inGamut.g, inGamut.b));
        giPoisonNrdCeilHit = !giPoisonNrdHit && peak > AVER_VOX_MAXRAD;
        outDiffuse = giPoisonNrdHit
                   ? float3(0.0, 0.0, 0.0)
                   : inGamut * min(1.0, AVER_VOX_MAXRAD / max(peak, 1e-6));
    }

    // ---- POISON DEBUG VIEW (voxi.giPoisonView / gGiRestirParams.w) -- OVERRIDE 2/3's by-hand tool ----
    // Default off, cheap when off (one dynamic branch on a cbuffer float already read this frame,
    // no new binding/buffer/readback). While on, replaces the final indirect-diffuse colour at any
    // pixel where a guard fired this frame, in an unmistakable per-guard colour.
    // TWO KINDS OF GUARD: the five NON-FINITE guards (magenta-blue) catch actual corruption (a NaN/
    // Inf that should never exist). The two CEILING guards (red, green) catch something else: a
    // perfectly FINITE value merely large enough to saturate AVER_VOX_MAXRAD and acesTonemap -- the
    // white-patch symptom this task diagnoses, not a bug here. Deliberately never white (that's the
    // symptom, not the diagnosis) and checked last: a non-finite guard always outranks a mere ceiling
    // hit, because corruption is more urgent to see (and giPoisonEstCeilHit/giPoisonNrdCeilHit are
    // already false whenever their non-finite sibling fired, so the two families never actually
    // compete). Order below matters only for the rare pixel where more than one fires; each is
    // otherwise independent.
    //
    // LEGEND (also in resetgihistory's neighbourhood in buildConsoleCatalog, and in `help`):
    //   MAGENTA (1,0,1)   -- store-time reservoir guard (giPoisonStoreHit): a poisoned reservoir was
    //                        about to enter CROSS-FRAME history and was emptied instead. THE CIRCUIT
    //                        BREAKER -- the one that matters most.
    //   CYAN    (0,1,1)   -- candidate-radiance clamp guard (nonFiniteCandidate): a fresh candidate's
    //                        shaded/sky radiance was non-finite before reaching a reservoir.
    //   YELLOW  (1,1,0)   -- target-pdf guard (gGiPoisonPdfHit): RAB_GetGISampleTargetPdfForSurface
    //                        computed a non-finite importance weight this frame.
    //   ORANGE  (1,0.5,0) -- final-estimate guard (giPoisonEstHit): this pixel's finalised
    //                        weightSum*radiance was non-finite.
    //   BLUE    (0,0,1)   -- NRD-readback guard (giPoisonNrdHit): REBLUR handed back a non-finite
    //                        value on the denoised read-back path.
    //   RED     (1,0,0)   -- raw estimate hit the radiance CEILING (giPoisonEstCeilHit): finite, but
    //                        AVER_VOX_MAXRAD clamped it, which paints solid white once tonemapped --
    //                        diagnoses a white patch's cause, not just its symptom.
    //   GREEN   (0,1,0)   -- NRD-denoised readback hit the same ceiling (giPoisonNrdCeilHit): REBLUR's
    //                        accumulation can push an already-hot value higher before this pixel sees it.
    //
    //   AN EIGHTH COLOUR LIVES OUTSIDE THIS FUNCTION (B1/F5): voxi.hlsl's PSMainVoxi/PSRayDriven paint
    //   VIOLET (0.55,0,1) over the ray-traced SPECULAR indirect term's ceiling hit, gated by the same
    //   flag but not by giMode -- it fires under either diffuse estimator since the specular ray it
    //   checks is independent of which of giRestirIndirect/coneTracedIndirect answered the diffuse
    //   bounce. Applied at each pixel shader's own final output, after this function returns, so it
    //   stays subordinate to whichever of these seven colours this function returns (see
    //   aver_IsGiRestirPoisonColour, voxi.hlsl, above PSMainVoxi).
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
    // Separate from the poison view above, gated `gGiRestirParams.w <= 0.5` so the two never fight
    // over the same return (poison view returns first when both are on -- deliberate: it answers
    // "is anything broken", this answers "which path did F2 take").
    // Paints F2's path; F3 follows the same path except under legacy bit 8 (gAmbientParams.z & 8u),
    // which this view does not separately colour -- F2/F3 share one path number except where the
    // decode block at this function's top singles one out, and F2 is by far the more expensive ray.
    // Like the poison view, replaces indirect diffuse AFTER the NRD write above, so it never enters
    // history or feeds back into next frame's reprojection -- a debug paint that corrupted the signal
    // it exists to diagnose would defeat the point.
    if (gGiRestirParams.w <= 0.5 && ((uint)gAmbientParams.w & 64u) != 0u) {
        if (f2Path == 0u) return float3(1.0, 1.0, 0.0);                 // YELLOW: no ray (mode 0 or legacy bit 4)
        if (f2Path == 1u) return float3(0.0, 1.0, 0.0);                 // GREEN: reconstructed (voxel cone)
        if (f2Path == 2u) return float3(0.0, 0.0, 1.0);                 // BLUE: half-res reconstruction
        return (halfBound && !tracedPx) ? float3(1.0, 0.0, 0.0)        // RED: half-res fallback, traced
                                        : float3(1.0, 1.0, 1.0);        // WHITE: traced (Full, or Half's phase pixel)
    }

    return outDiffuse;
}
