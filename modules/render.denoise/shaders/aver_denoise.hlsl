// aver_denoise.hlsl -- the engine's spatio-temporal denoiser: AMD FidelityFX Denoiser's reflection
// pipeline (third_party/fidelityfx-denoiser, MIT), driven as a DIFFUSE denoiser for two Voxi
// signals -- the ReSTIR GI radiance and the sky-occlusion hit distance.
//
// FOUR PASSES, ONE COMPILE EACH (AVER_DNSR_PASS), because the three FidelityFX headers each declare
// their own groupshared arrays under the same names and cannot share a translation unit:
//   3 CSDenoiseScale     -- colour only, first: the frame's pre-exposure scale (see dnsrScale).
//   0 CSDenoiseReproject -- reproject last frame's denoised result, accumulate a per-pixel sample
//                           count and temporal variance, and reduce the noisy input to an 8x8
//                           average (the outlier anchor the next two passes clip against).
//   1 CSDenoisePrefilter -- a variance-guided, depth/normal edge-stopped spatial filter of the noisy
//                           input.
//   2 CSDenoiseResolve   -- blend the prefiltered signal into the clipped reprojected history; the
//                           result is both this frame's output and next frame's history.
//
// WHY THE REFLECTION PIPELINE FOR A DIFFUSE SIGNAL. FidelityFX Denoiser ships two denoisers: shadows
// (a 1-bit-per-pixel hit mask) and reflections. Neither of Voxi's signals is a hit mask; both are a
// noisy per-pixel value over opaque surfaces, which is what the reflection pipeline filters. A
// diffuse lobe is the roughness-1 end of that pipeline, so the host callbacks below report every
// pixel as glossy and never as a mirror (roughness 1 everywhere), and the hit distance as 0. With a
// zero hit distance the pipeline's mirror "parallax" reprojection collapses onto the surface
// reprojection: there is no reflected image to follow, only the surface's own motion.
//
// AVER_DNSR_SCALAR 1 compiles the one-channel variant for sky occlusion: the value travels in .x of
// every FidelityFX min16float3 and is stored in single-channel targets.
//
// NO PRELUDE: this touches no engine concept beyond its own bindings.
//
// THE G-BUFFER IT READS is LAST frame's (the denoiser runs before this frame's scene pass), and so
// is the noisy signal; the history textures hold the frame before that. Motion vectors are the
// G-buffer's: PIXELS, destination minus source (voxi.hlsl's GBufferOut header).

#ifndef AVER_DNSR_PASS
#define AVER_DNSR_PASS 0
#endif
#ifndef AVER_DNSR_SCALAR
#define AVER_DNSR_SCALAR 0
#endif

// The 1x1 frame-scale texture's SRV slot: one past each pass's own inputs.
#if AVER_DNSR_PASS == 1
#define DNSR_SCALE_SLOT t6
#else
#define DNSR_SCALE_SLOT t9
#endif

// FP32 THROUGHOUT. FidelityFX writes its maths in min16float and packs it into groupshared memory
// with f32tof16 (FFX_DNSR_Reflections_PackFloat16). Some DXC versions fold that call's float2()
// widening away and emit `bitcast half to i16` on the min-precision value, which the DXIL
// validator rejects ("Bitcast on minprecison types is not allowed"), and the pipeline never
// builds. Without -enable-16bit-types min16float is only a hint that desktop drivers mostly run
// at fp32 anyway, so spelling it float here costs nothing, removes the min-precision values the
// validator objects to on every DXC version, and leaves the vendored headers untouched.
#define min16float  float
#define min16float2 float2
#define min16float3 float3
#define min16float4 float4

#if AVER_DNSR_SCALAR
#define DNSR_TEX float
#define DNSR_LOAD3(v) ((min16float3)((v).xxx))
#define DNSR_STORE(v) ((float)(v).x)
#else
#define DNSR_TEX float4
#define DNSR_LOAD3(v) ((min16float3)((v).xyz))
#define DNSR_STORE(v) (float4((v), 0.0))
#endif

cbuffer AverDenoiseCB : register(b1) {
    uint2  gDnsrSize;               // render resolution: every input, history and output
    float2 gDnsrInvSize;
    uint   gDnsrFlags;              // bit 0: history invalid (reset); bit 1: half-rate input; bit 2: its parity
    uint   gDnsrMaxSamples;         // reproject: cap on the accumulated sample count
    float  gDnsrHistoryClipWeight;  // resolve: neighbourhood-clip width for the history
    float  gDnsrTemporalStability;  // reproject: passed through to FidelityFX (unused by this version)
};

SamplerState gDnsrLinear : register(s0);   // linear, clamp

// ---- inputs shared by every pass (t0..t3) ----
Texture2D<float>  gDnsrViewZ      : register(t0);   // view-space linear depth (G-buffer)
Texture2D<float4> gDnsrNormal     : register(t1);   // averPackNormalRoughness (G-buffer)
Texture2D<float2> gDnsrMotion     : register(t2);   // pixels, destination minus source (G-buffer)
#if AVER_DNSR_SCALAR
Texture2D<float>  gDnsrInput      : register(t3);   // the noisy signal
#else
Texture2D<float4> gDnsrInput      : register(t3);
#endif

bool dnsrHistoryReset()   { return (gDnsrFlags & 1u) != 0u; }
bool dnsrHalfRateInput()  { return (gDnsrFlags & 2u) != 0u; }
uint dnsrHalfRateParity() { return (gDnsrFlags >> 2) & 1u; }

int2 dnsrClampPixel(int2 p) { return clamp(p, int2(0, 0), int2(gDnsrSize) - 1); }

// The decode half of voxi.hlsl's averPackNormalRoughness: an octahedral normal in xy (Cigolle et al.
// 2014, mapped to [0,1]), roughness in z. Change the two together (and sandbox/shaders/
// gbuffer_debug.hlsl, the third reader).
float3 dnsrDecodeNormal(float4 e) {
    const float2 f = e.xy * 2.0 - 1.0;
    float3 n = float3(f, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

float  dnsrLoadViewZ(int2 p)  { return gDnsrViewZ.Load(int3(dnsrClampPixel(p), 0)); }
float3 dnsrLoadNormal(int2 p) { return dnsrDecodeNormal(gDnsrNormal.Load(int3(dnsrClampPixel(p), 0))); }
// In UV units, previous-to-current: FidelityFX reprojects by history_uv = uv - motion_vector.
float2 dnsrLoadMotion(int2 p) { return gDnsrMotion.Load(int3(dnsrClampPixel(p), 0)) * gDnsrInvSize; }

// ---- pre-exposure (colour only; docs/rendering/DENOISING.md) ----
// FidelityFX's constants assume radiance near 1; ReSTIR GI is often near 0.01. Every radiance
// entering the passes is multiplied by the frame scale and every value leaving for history divided
// by it, so stored values stay in scene units.
#if AVER_DNSR_SCALAR || AVER_DNSR_PASS == 3
float dnsrScale() { return 1.0; }
#else
Texture2D<float> gDnsrScale : register(DNSR_SCALE_SLOT);
float dnsrScale() { return gDnsrScale.Load(int3(0, 0, 0)); }
#endif

min16float3 dnsrLoadInputTexel(int2 p) { return DNSR_LOAD3(gDnsrInput.Load(int3(dnsrClampPixel(p), 0))) * dnsrScale(); }

// ---- the noisy input, with half-rate reconstruction ----
// Under half-rate ReSTIR GI only pixels with ((x ^ y ^ parity) & 1) == 0 traced this frame. A
// skipped pixel takes its four traced edge neighbours, weighted by depth and normal agreement so a
// silhouette does not bleed across; with no agreeing neighbour it falls back to the plain mean.
min16float3 dnsrLoadInput(int2 p) {
    const int2 c = dnsrClampPixel(p);
    if (dnsrHalfRateInput() && (((uint(c.x) ^ uint(c.y) ^ dnsrHalfRateParity()) & 1u) != 0u)) {
        const int2 offs[4] = {int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)};
        const float  zc = dnsrLoadViewZ(c);
        const float3 nc = dnsrLoadNormal(c);
        min16float3 sum = 0.0, plain = 0.0;
        float wsum = 0.0;
        [unroll] for (int i = 0; i < 4; ++i) {
            const int2 q = c + offs[i];
            const min16float3 v = dnsrLoadInputTexel(q);
            const float w = exp(-abs(dnsrLoadViewZ(q) - zc) / max(zc, 1e-3) * 32.0) *
                            pow(saturate(dot(dnsrLoadNormal(q), nc)), 8.0);
            sum += v * w;
            plain += v;
            wsum += w;
        }
        return wsum > 1e-3 ? sum / wsum : plain * 0.25;
    }
    return dnsrLoadInputTexel(c);
}

// The prefilter's edge-stopping weight is exp(-|zc - zn| * zc * 4): absolute, sized for depth in
// [0,1], and zero for any neighbour off a plane at Voxi's centimetre view Z. Feeding it
// f(z) = sqrt(K ln z) turns that into exp(-2K |dz| / z), a relative test; K = 10 gives e^-1 at 5%.
float dnsrPrefilterDepth(float viewZ) { return sqrt(10.0 * log(max(viewZ, 1.0) + 1.0)); }

// ---- the roughness-1 contract (see the header) ----
min16float FFX_DNSR_Reflections_LoadRoughness(int2 p) { return 1.0; }
bool FFX_DNSR_Reflections_IsGlossyReflection(float roughness) { return true; }
bool FFX_DNSR_Reflections_IsMirrorReflection(float roughness) { return false; }

// =================================================================================================
#if AVER_DNSR_PASS == 0   // ---- reproject ----

Texture2D<float>    gDnsrDepthHistory      : register(t4);
Texture2D<float4>   gDnsrNormalHistory     : register(t5);
Texture2D<DNSR_TEX> gDnsrRadianceHistory   : register(t6);
Texture2D<float>    gDnsrVarianceHistory   : register(t7);
Texture2D<float>    gDnsrSampleCountHistory: register(t8);

RWTexture2D<DNSR_TEX> gDnsrReprojectedOut  : register(u0);
RWTexture2D<DNSR_TEX> gDnsrAverageOut      : register(u1);   // 1/8 resolution
RWTexture2D<float>    gDnsrVarianceOut     : register(u2);
RWTexture2D<float>    gDnsrSampleCountOut  : register(u3);

int2 dnsrUvToPixel(float2 uv) { return dnsrClampPixel(int2(floor(uv * float2(gDnsrSize)))); }

min16float3 FFX_DNSR_Reflections_LoadRadiance(int2 p)          { return dnsrLoadInput(p); }
min16float3 FFX_DNSR_Reflections_LoadWorldSpaceNormal(int2 p)  { return (min16float3)dnsrLoadNormal(p); }
float       FFX_DNSR_Reflections_LoadDepth(int2 p)             { return dnsrLoadViewZ(p); }
float2      FFX_DNSR_Reflections_LoadMotionVector(int2 p)      { return dnsrLoadMotion(p); }
min16float  FFX_DNSR_Reflections_LoadRayLength(int2 p)         { return 0.0; }
// The depth this pipeline passes around is already view-space linear depth.
float FFX_DNSR_Reflections_GetLinearDepth(float2 uv, float depth) { return depth; }

// The "parallax" reprojection with a zero hit distance (FFX_DNSR_Reflections_GetHitPositionReprojection):
// these three are written so the ray it shoots "through the surface" has length zero beyond it and
// lands back on the surface's own screen position, reprojected by the surface's own motion vector --
// the same answer as the surface reprojection, which is the correct one for a diffuse signal. The
// "view space" here is just (uv, 1): any non-zero vector would do, since only its normalised
// direction and unchanged length survive the round trip.
float3 FFX_DNSR_Reflections_ScreenSpaceToViewSpace(float3 screen) { return float3(screen.xy, 1.0); }
float3 FFX_DNSR_Reflections_ViewSpaceToWorldSpace(float4 view)    { return view.xyz; }
float3 FFX_DNSR_Reflections_WorldSpaceToScreenSpacePrevious(float3 world) {
    return float3(world.xy - dnsrLoadMotion(dnsrUvToPixel(world.xy)), 0.0);
}

// History reads. After a reset the history textures may hold anything (a fresh allocation, another
// size, another scene): a zero sample count makes FidelityFX weigh the history at exactly zero, and
// every other read answers with this frame's own values or a neutral constant so that a stale NaN
// can never reach arithmetic it would poison (0 * NaN is NaN). The radiance answers with this
// frame's input rather than zero because the reproject pass also mixes 30% of the reprojected value
// into its 8x8 average, which a zero would darken for the frame after every reset.
min16float3 FFX_DNSR_Reflections_SampleRadianceHistory(float2 uv) {
    if (dnsrHistoryReset()) return dnsrLoadInput(dnsrUvToPixel(uv));
    return DNSR_LOAD3(gDnsrRadianceHistory.SampleLevel(gDnsrLinear, uv, 0)) * dnsrScale();
}
min16float3 FFX_DNSR_Reflections_LoadRadianceHistory(int2 p) {
    if (dnsrHistoryReset()) return dnsrLoadInput(p);
    return DNSR_LOAD3(gDnsrRadianceHistory.Load(int3(dnsrClampPixel(p), 0))) * dnsrScale();
}
min16float FFX_DNSR_Reflections_SampleVarianceHistory(float2 uv) {
    if (dnsrHistoryReset()) return 1.0;
    return (min16float)gDnsrVarianceHistory.SampleLevel(gDnsrLinear, uv, 0);
}
min16float FFX_DNSR_Reflections_SampleNumSamplesHistory(float2 uv) {
    if (dnsrHistoryReset()) return 0.0;
    return (min16float)gDnsrSampleCountHistory.SampleLevel(gDnsrLinear, uv, 0);
}
min16float FFX_DNSR_Reflections_SampleRoughnessHistory(float2 uv) { return 1.0; }
float FFX_DNSR_Reflections_LoadDepthHistory(int2 p) {
    if (dnsrHistoryReset()) return dnsrLoadViewZ(p);
    return gDnsrDepthHistory.Load(int3(dnsrClampPixel(p), 0));
}
float FFX_DNSR_Reflections_SampleDepthHistory(float2 uv) { return FFX_DNSR_Reflections_LoadDepthHistory(dnsrUvToPixel(uv)); }
min16float3 FFX_DNSR_Reflections_LoadWorldSpaceNormalHistory(int2 p) {
    if (dnsrHistoryReset()) return (min16float3)dnsrLoadNormal(p);
    return (min16float3)dnsrDecodeNormal(gDnsrNormalHistory.Load(int3(dnsrClampPixel(p), 0)));
}
min16float3 FFX_DNSR_Reflections_SampleWorldSpaceNormalHistory(float2 uv) {
    return FFX_DNSR_Reflections_LoadWorldSpaceNormalHistory(dnsrUvToPixel(uv));
}

void FFX_DNSR_Reflections_StoreRadianceReprojected(int2 p, min16float3 v) { gDnsrReprojectedOut[p] = DNSR_STORE(v); }
// Stored in scene units: the exposure pass reads it next frame, after the scale has moved.
void FFX_DNSR_Reflections_StoreAverageRadiance(int2 p, min16float3 v)     { gDnsrAverageOut[p] = DNSR_STORE(v / dnsrScale()); }
void FFX_DNSR_Reflections_StoreVariance(int2 p, min16float v)             { gDnsrVarianceOut[p] = v; }
void FFX_DNSR_Reflections_StoreNumSamples(int2 p, min16float v)           { gDnsrSampleCountOut[p] = v; }

// The vendored headers trip two harmless DXC warnings (an unsuffixed literal shift, an implicit
// vector truncation) on every compile; muted around each include only, never for our own code.
#pragma dxc diagnostic push
#pragma dxc diagnostic ignored "-Wambig-lit-shift"
#pragma dxc diagnostic ignored "-Wconversion"
#include "FidelityFX/ffx_denoiser_reflections_reproject.h"
#pragma dxc diagnostic pop

[numthreads(8, 8, 1)]
void CSDenoiseReproject(int2 dtid : SV_DispatchThreadID, int2 gtid : SV_GroupThreadID) {
    FFX_DNSR_Reflections_Reproject(dtid, gtid, gDnsrSize, gDnsrTemporalStability, (int)gDnsrMaxSamples);
}

// =================================================================================================
#elif AVER_DNSR_PASS == 1   // ---- prefilter ----

Texture2D<float>    gDnsrVariance : register(t4);   // the reproject pass's
Texture2D<DNSR_TEX> gDnsrAverage  : register(t5);   // 1/8 resolution

RWTexture2D<DNSR_TEX> gDnsrPrefilteredOut         : register(u0);
RWTexture2D<float>    gDnsrPrefilteredVarianceOut : register(u1);

void FFX_DNSR_Reflections_LoadNeighborhood(int2 p, out min16float3 radiance, out min16float variance,
                                           out min16float3 normal, out float depth, int2 screenSize) {
    radiance = dnsrLoadInput(p);
    variance = (min16float)gDnsrVariance.Load(int3(dnsrClampPixel(p), 0));
    normal   = (min16float3)dnsrLoadNormal(p);
    depth    = dnsrPrefilterDepth(dnsrLoadViewZ(p));
}
min16float3 FFX_DNSR_Reflections_SampleAverageRadiance(float2 uv) { return DNSR_LOAD3(gDnsrAverage.SampleLevel(gDnsrLinear, uv, 0)) * dnsrScale(); }
void FFX_DNSR_Reflections_StorePrefilteredReflections(int2 p, min16float3 radiance, min16float variance) {
    gDnsrPrefilteredOut[p]         = DNSR_STORE(radiance);
    gDnsrPrefilteredVarianceOut[p] = variance;
}

#pragma dxc diagnostic push
#pragma dxc diagnostic ignored "-Wambig-lit-shift"
#pragma dxc diagnostic ignored "-Wconversion"
#include "FidelityFX/ffx_denoiser_reflections_prefilter.h"
#pragma dxc diagnostic pop

[numthreads(8, 8, 1)]
void CSDenoisePrefilter(int2 dtid : SV_DispatchThreadID, int2 gtid : SV_GroupThreadID) {
    FFX_DNSR_Reflections_Prefilter(dtid, gtid, gDnsrSize);
}

// =================================================================================================
#elif AVER_DNSR_PASS == 3   // ---- frame scale (colour only; runs first) ----

Texture2D<DNSR_TEX> gDnsrAverageLast : register(t4);   // last frame's 8x8 averages, scene units
RWTexture2D<float>  gDnsrScaleOut    : register(u0);   // 1x1, read-modify-write

static const uint kDnsrScaleThreads = 256;
groupshared float gsDnsrLumSum[kDnsrScaleThreads];
groupshared float gsDnsrCount[kDnsrScaleThreads];

// Scale brings the mean luminance to 0.25, eased in log space so a flash does not snap the filter
// weights. At 1.0 FidelityFX's firefly weight ate real light (NeonDistrict 8% darker); 0.25 kept
// most of the noise gain at <1% bias (docs/rendering/DENOISING.md).
static const float kDnsrScaleTargetLum = 0.25;
[numthreads(16, 16, 1)]
void CSDenoiseScale(uint gi : SV_GroupIndex) {
    const uint2 dim = (gDnsrSize + 7u) / 8u;
    float sum = 0.0, count = 0.0;
    for (uint i = gi; i < dim.x * dim.y; i += kDnsrScaleThreads) {
        const float3 v = DNSR_LOAD3(gDnsrAverageLast.Load(int3(i % dim.x, i / dim.x, 0)));
        const float l = dot(v, float3(0.299, 0.587, 0.114));
        if (l >= 0.0 && l < 65504.0) { sum += l; count += 1.0; }   // false for NaN too
    }
    gsDnsrLumSum[gi] = sum;
    gsDnsrCount[gi] = count;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = kDnsrScaleThreads / 2; s > 0; s >>= 1) {
        if (gi < s) { gsDnsrLumSum[gi] += gsDnsrLumSum[gi + s]; gsDnsrCount[gi] += gsDnsrCount[gi + s]; }
        GroupMemoryBarrierWithGroupSync();
    }
    if (gi != 0) return;
    const float old = gDnsrScaleOut[uint2(0, 0)];
    const bool oldOk = old >= 0.01 && old <= 1.0e4;
    float k = oldOk ? old : 1.0;
    if (gsDnsrCount[0] > 0.0 && gsDnsrLumSum[0] > 0.0) {
        const float target = clamp(kDnsrScaleTargetLum * gsDnsrCount[0] / gsDnsrLumSum[0], 0.01, 1.0e4);
        k = (dnsrHistoryReset() || !oldOk) ? target : exp(lerp(log(old), log(target), 0.2));
    }
    gDnsrScaleOut[uint2(0, 0)] = k;
}

// =================================================================================================
#else   // AVER_DNSR_PASS == 2 ---- temporal resolve ----

Texture2D<DNSR_TEX> gDnsrPrefiltered         : register(t4);
Texture2D<DNSR_TEX> gDnsrReprojected         : register(t5);
Texture2D<float>    gDnsrPrefilteredVariance : register(t6);
Texture2D<float>    gDnsrSampleCount         : register(t7);
Texture2D<DNSR_TEX> gDnsrAverage             : register(t8);   // 1/8 resolution

RWTexture2D<DNSR_TEX> gDnsrOut         : register(u0);   // the denoised result = next frame's history
RWTexture2D<float>    gDnsrVarianceOut : register(u1);   // next frame's variance history

min16float3 FFX_DNSR_Reflections_LoadRadiance(int2 p)            { return DNSR_LOAD3(gDnsrPrefiltered.Load(int3(dnsrClampPixel(p), 0))); }
min16float3 FFX_DNSR_Reflections_LoadRadianceReprojected(int2 p) { return DNSR_LOAD3(gDnsrReprojected.Load(int3(dnsrClampPixel(p), 0))); }
min16float  FFX_DNSR_Reflections_LoadVariance(int2 p)            { return (min16float)gDnsrPrefilteredVariance.Load(int3(dnsrClampPixel(p), 0)); }
min16float  FFX_DNSR_Reflections_LoadNumSamples(int2 p)          { return (min16float)gDnsrSampleCount.Load(int3(dnsrClampPixel(p), 0)); }
min16float3 FFX_DNSR_Reflections_SampleAverageRadiance(float2 uv) { return DNSR_LOAD3(gDnsrAverage.SampleLevel(gDnsrLinear, uv, 0)) * dnsrScale(); }
void FFX_DNSR_Reflections_StoreTemporalAccumulation(int2 p, min16float3 radiance, min16float variance) {
    gDnsrOut[p]         = DNSR_STORE(radiance / dnsrScale());
    gDnsrVarianceOut[p] = variance;
}

#pragma dxc diagnostic push
#pragma dxc diagnostic ignored "-Wambig-lit-shift"
#pragma dxc diagnostic ignored "-Wconversion"
#include "FidelityFX/ffx_denoiser_reflections_resolve_temporal.h"
#pragma dxc diagnostic pop

[numthreads(8, 8, 1)]
void CSDenoiseResolve(int2 dtid : SV_DispatchThreadID, int2 gtid : SV_GroupThreadID) {
    FFX_DNSR_Reflections_ResolveTemporal(dtid, gtid, gDnsrSize, gDnsrInvSize, gDnsrHistoryClipWeight);
}

#endif
