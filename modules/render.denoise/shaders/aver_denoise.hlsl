// aver_denoise.hlsl -- the engine's spatio-temporal denoiser: AMD FidelityFX Denoiser's reflection
// pipeline (third_party/fidelityfx-denoiser, MIT), driven as a DIFFUSE denoiser for two Voxi
// signals -- the ReSTIR GI radiance and the sky-occlusion hit distance.
//
// FIVE PASSES, ONE COMPILE EACH (AVER_DNSR_PASS), because the three FidelityFX headers each declare
// their own groupshared arrays under the same names and cannot share a translation unit:
//   3 CSDenoiseScale     -- colour only, first: the frame's pre-exposure scale (see dnsrScale).
//   0 CSDenoiseReproject -- reproject last frame's denoised result, accumulate a per-pixel sample
//                           count and temporal variance, and reduce the noisy input to an 8x8
//                           average (the outlier anchor the next two passes clip against).
//   1 CSDenoisePrefilter -- a variance-guided, depth/normal edge-stopped spatial filter of the noisy
//                           input.
//   2 CSDenoiseResolve   -- blend the prefiltered signal into the clipped reprojected history; the
//                           result is both this frame's output and next frame's history.
//   4 CSDenoiseNrdResolve -- Neural Denoise's resolve (docs/rendering/NEURAA_NRD.md section 4), in
//                           place of pass 2: FidelityFX's resolve, or with flag bit 3 the spatial-first
//                           resolve (pyramid candidates, then a short min/max-clamped stabiliser).
//   5 CSDenoiseNrdPyramid -- the current frame's 1/2, 1/4, 1/8 levels for pass 4's spatial path.
//   6 CSDenoiseCapture   -- NRD training capture (colour only): running mean of fresh raw pixels.
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

// OUR OWN COPY OF FIDELITYFX'S CONFIG (its header is guarded by FFX_DNSR_REFLECTIONS_CONFIG), the
// vendored values except three: the 8x8 average is a plain mean (luminance weight 0), and the
// radiance-difference weight is 0.2 (vendored 0.6) with no variance term. Both pull a skewed 1-spp
// diffuse signal toward its dark values (NeonDistrict Day: denoised GI 0.66 of the raw mean as
// vendored, 0.79 with both off). Fully off, a night path-traced signal (rare bright samples) turned
// into blotches in motion; 0.2 keeps ~70% of that light (2x the vendored) with the blotches mostly gone.
#define FFX_DNSR_REFLECTIONS_CONFIG
#define FFX_DNSR_REFLECTIONS_GAUSSIAN_K 3.0
#define FFX_DNSR_REFLECTIONS_RADIANCE_WEIGHT_BIAS 0.2
#define FFX_DNSR_REFLECTIONS_RADIANCE_WEIGHT_VARIANCE_K 0.0
#define FFX_DNSR_REFLECTIONS_AVG_RADIANCE_LUMINANCE_WEIGHT 0.0
#define FFX_DNSR_REFLECTIONS_PREFILTER_VARIANCE_WEIGHT 4.4
#define FFX_DNSR_REFLECTIONS_REPROJECT_SURFACE_DISCARD_VARIANCE_WEIGHT 1.5
#define FFX_DNSR_REFLECTIONS_PREFILTER_VARIANCE_BIAS 0.1
#define FFX_DNSR_REFLECTIONS_PREFILTER_NORMAL_SIGMA 512.0
#define FFX_DNSR_REFLECTIONS_PREFILTER_DEPTH_SIGMA 4.0
#define FFX_DNSR_REFLECTIONS_DISOCCLUSION_NORMAL_WEIGHT 1.4
#define FFX_DNSR_REFLECTIONS_DISOCCLUSION_DEPTH_WEIGHT 1.0
#define FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD 0.9
#define FFX_DNSR_REFLECTIONS_REPROJECTION_NORMAL_SIMILARITY_THRESHOLD 0.9999
#define FFX_DNSR_REFLECTIONS_SAMPLES_FOR_ROUGHNESS(r) (1.0 - exp(-r * 100.0))

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
    uint   gDnsrFlags;              // bit 0: history invalid (reset); bit 1: half-rate input; bit 2: its parity;
                                    // bit 3: NRD's spatial-first resolve (pass 4); bit 4: restart the
                                    // capture mean (pass 6); bit 5: NRD's network (pass 4, t13)
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
#elif AVER_DNSR_PASS == 2 || AVER_DNSR_PASS == 4   // ---- temporal resolve (FidelityFX / NRD) ----

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

#if AVER_DNSR_PASS == 4
// ---- NRD's spatial-first resolve (NEURAA_NRD.md section 4), fixed parameters (phase 4) ----
// Every candidate is a normalised mix of this frame's values, so the local mean is kept: an outlier
// spreads over the coarser levels instead of being rejected. History only steadies what is left.
Texture2D<float4> gNrdLevel1 : register(t10);   // rgb + view Z * 0.01 (0 = no valid sample)
Texture2D<float4> gNrdLevel2 : register(t11);
Texture2D<float4> gNrdLevel3 : register(t12);
StructuredBuffer<float> gNrdNet : register(t13);   // the trained weights (flag bit 5 uses them)

static const float kNrdDepthSigma = 16.0;   // relative view-Z difference that halves a tap ~ 1/23
// Measured 2026-10-05, NeonDistrict Night (NEURAA_NRD.md section 4): coarse-leaning beat fine-leaning on
// brightness, spots and grain, still and moving.
static const float kNrdLogit[4]   = {-2.0, -1.0, 0.0, 0.0};   // this pixel, 1/2, 1/4, 1/8
static const uint  kNrdFramesMoving = 4u, kNrdFramesStill = 16u;

bool nrdSpatial() { return (gDnsrFlags & 8u) != 0u; }
bool nrdNetwork() { return (gDnsrFlags & 32u) != 0u; }

float nrdLum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// A level's value at full-resolution pixel p: bilinear taps kept only on this pixel's surface.
// `conf` is the share of the bilinear weight that survived (0 = nothing usable).
float3 nrdUpsample(Texture2D<float4> lvl, uint shift, int2 p, float z, out float conf) {
    uint lw, lh;
    lvl.GetDimensions(lw, lh);
    const float2 pos  = (float2(p) + 0.5) / float(1u << shift) - 0.5;
    const int2   base = int2(floor(pos));
    const float2 f    = pos - float2(base);
    float3 sum = 0.0;
    float  wsum = 0.0;
    [unroll] for (uint i = 0; i < 4u; ++i) {
        const int2   o = int2(i & 1u, i >> 1);
        const float4 t = lvl.Load(int3(clamp(base + o, int2(0, 0), int2(lw, lh) - 1), 0));
        const float  b = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
        const float  tz = t.w * 100.0;
        const float  w = t.w > 0.0 ? b * exp(-abs(tz - z) / max(z, 1.0e-3) * kNrdDepthSigma) : 0.0;
        sum += w * t.rgb;
        wsum += w;
    }
    conf = wsum;
    return wsum > 1.0e-6 ? sum / wsum : 0.0;
}

struct NrdPixel {
    float  lum[4];     // luminance of the four candidates
    float  conf[3];    // the levels' surviving bilinear weight
    float  hz;         // hit distance / view Z
    float  contact;
    float  push;       // 1 when the outlier rule fired
    float  z;
    float3 n;
    bool   surf;
};

// The spatial estimate S at p, in the scaled space the FidelityFX passes work in. `delta` is the
// network's per-tile correction to the four logits (zero without it); `px` reports the pixel's values.
float3 nrdSpatialEstimate(int2 p, float4 delta, out NrdPixel px) {
    p = dnsrClampPixel(p);
    const float z = dnsrLoadViewZ(p);
    // The raw signal, not FidelityFX's prefiltered value: its outlier rejection darkens.
    const float3 c0 = dnsrLoadInput(p);
    float conf[3];
    float3 c[3];
    c[0] = nrdUpsample(gNrdLevel1, 1u, p, z, conf[0]) * dnsrScale();
    c[1] = nrdUpsample(gNrdLevel2, 2u, p, z, conf[1]) * dnsrScale();
    c[2] = nrdUpsample(gNrdLevel3, 3u, p, z, conf[2]) * dnsrScale();

    float logit[4] = {kNrdLogit[0] + delta.x, kNrdLogit[1] + delta.y, kNrdLogit[2] + delta.z, kNrdLogit[3] + delta.w};
    // An outlier against the 1/4 level leans on the coarse levels (spread, not rejected).
    const float ratio = nrdLum(c0) / max(nrdLum(c[1]), 1.0e-4);
    px.push = 0.0;
    if (conf[1] > 0.25 && ratio > 4.0) {
        const float push = log2(ratio / 4.0);
        logit[0] -= push; logit[2] += 0.5 * push; logit[3] += 0.5 * push;
        px.push = 1.0;
    }
    px.contact = 0.0;
    px.hz = 0.0;
#if !AVER_DNSR_SCALAR
    // Light from nearby geometry changes quickly: a short hit distance leans on the fine levels.
    const float hit = gDnsrInput.Load(int3(p, 0)).a;
    const float contact = 1.0 - saturate(hit / max(z * 0.3, 1.0e-3));
    logit[0] += 2.0 * contact; logit[1] += contact;
    px.contact = contact;
    px.hz = hit / max(z, 1.0e-3);
#endif
    px.lum[0] = nrdLum(c0); px.lum[1] = nrdLum(c[0]); px.lum[2] = nrdLum(c[1]); px.lum[3] = nrdLum(c[2]);
    px.conf[0] = saturate(conf[0]); px.conf[1] = saturate(conf[1]); px.conf[2] = saturate(conf[2]);
    px.z = z;
    px.n = dnsrLoadNormal(p);
    px.surf = z > 0.0 && z < 1.0e6;
    float w[4];
    w[0] = exp(logit[0]);
    float wsum = w[0];
    float3 s = w[0] * c0;
    [unroll] for (uint i = 0; i < 3u; ++i) {
        w[i + 1] = exp(logit[i + 1]) * saturate(conf[i]);
        wsum += w[i + 1];
        s += w[i + 1] * c[i];
    }
    return s / wsum;
}

groupshared float3 gsNrdS[10 * 10];

// ---- the network: per 8x8 tile, 16 statistics -> 16 -> 16 -> 4 logit corrections ----
// gNrdNet holds the weights file body: input mean[16], std[16], W1[16x16], b1, W2[16x16], b2, W3[4x16], b3.
// The statistics must match tools/nrd/nrd_train.py's tile_features order exactly.
#define NRD_IN 16
#define NRD_H 16
static const float kNrdDeltaMax = 6.0;
groupshared float gsNrdA[64][11];   // per-pixel terms for the reductions
groupshared float gsNrdS1[11];      // pass one totals: count, L0..L3, log(hz), conf1..3, contact, push
groupshared float gsNrdS2[9];       // pass two totals: var, tail4, tail16, var(log hz), zmin, zmax, normal
groupshared float4 gsNrdDelta;

float4 nrdTileDelta(uint gi, NrdPixel px) {
    const float m = px.surf ? 1.0 : 0.0;
    const float lhz = log(max(px.hz, 0.0) + 1.0e-3);
    gsNrdA[gi][0] = m;
    [unroll] for (uint k = 0; k < 4u; ++k) gsNrdA[gi][1 + k] = px.lum[k] * m;
    gsNrdA[gi][5] = lhz * m;
    gsNrdA[gi][6] = px.conf[0] * m; gsNrdA[gi][7] = px.conf[1] * m; gsNrdA[gi][8] = px.conf[2] * m;
    gsNrdA[gi][9] = px.contact * m; gsNrdA[gi][10] = px.push * m;
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u) {
        [unroll] for (uint q = 0; q < 11u; ++q) gsNrdS1[q] = 0.0;
        for (uint t = 0; t < 64u; ++t) [unroll] for (uint q2 = 0; q2 < 11u; ++q2) gsNrdS1[q2] += gsNrdA[t][q2];
    }
    GroupMemoryBarrierWithGroupSync();
    const float cnt = max(gsNrdS1[0], 1.0);
    const float m0 = gsNrdS1[1] / cnt, mh = gsNrdS1[5] / cnt;
    const float L0 = px.lum[0];
    // Pass two: what needs the tile means first.
    gsNrdA[gi][0] = (L0 - m0) * (L0 - m0) * m;
    gsNrdA[gi][1] = (L0 > 4.0 * m0) ? L0 * m : 0.0;
    gsNrdA[gi][2] = (L0 > 16.0 * m0) ? L0 * m : 0.0;
    gsNrdA[gi][3] = (lhz - mh) * (lhz - mh) * m;
    gsNrdA[gi][4] = px.surf ? px.z : 1.0e30;
    gsNrdA[gi][5] = px.surf ? px.z : 0.0;
    gsNrdA[gi][6] = px.n.x * m; gsNrdA[gi][7] = px.n.y * m; gsNrdA[gi][8] = px.n.z * m;
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u) {
        [unroll] for (uint q = 0; q < 9u; ++q) gsNrdS2[q] = 0.0;
        gsNrdS2[4] = 1.0e30;
        for (uint t = 0; t < 64u; ++t) {
            [unroll] for (uint q2 = 0; q2 < 4u; ++q2) gsNrdS2[q2] += gsNrdA[t][q2];
            gsNrdS2[4] = min(gsNrdS2[4], gsNrdA[t][4]);
            gsNrdS2[5] = max(gsNrdS2[5], gsNrdA[t][5]);
            [unroll] for (uint q3 = 6; q3 < 9u; ++q3) gsNrdS2[q3] += gsNrdA[t][q3];
        }
        const float tot = gsNrdS1[1] + 1.0e-12;
        const float mean1 = gsNrdS1[2] / cnt, mean2 = gsNrdS1[3] / cnt, mean3 = gsNrdS1[4] / cnt;
        const float eps = 1.0e-4 * (m0 + mean3) + 1.0e-12;
        float f[NRD_IN];
        f[0] = log((mean1 + eps) / (m0 + eps));
        f[1] = log((mean2 + eps) / (m0 + eps));
        f[2] = log((mean3 + eps) / (m0 + eps));
        f[3] = log(gsNrdS2[0] / cnt / (m0 * m0 + eps * eps) + 1.0e-4);
        f[4] = gsNrdS2[1] / tot;
        f[5] = gsNrdS2[2] / tot;
        f[6] = mh;
        f[7] = sqrt(gsNrdS2[3] / cnt);
        const float zlo = gsNrdS2[4], zhi = gsNrdS2[5];
        f[8] = (zlo < 1.0e29 && zhi > 0.0) ? log(max(zhi, 1.0e-3) / max(zlo, 1.0e-3)) : 0.0;
        f[9] = 1.0 - length(float3(gsNrdS2[6], gsNrdS2[7], gsNrdS2[8]) / cnt);
        f[10] = gsNrdS1[0] / 64.0;
        f[11] = gsNrdS1[6] / cnt; f[12] = gsNrdS1[7] / cnt; f[13] = gsNrdS1[8] / cnt;
        f[14] = gsNrdS1[9] / cnt; f[15] = gsNrdS1[10] / cnt;
        [unroll] for (uint n = 0; n < NRD_IN; ++n) f[n] = (f[n] - gNrdNet[n]) / gNrdNet[NRD_IN + n];
        float h1[NRD_H], h2[NRD_H];
        const uint w1 = 2 * NRD_IN, b1 = w1 + NRD_H * NRD_IN, w2 = b1 + NRD_H, b2 = w2 + NRD_H * NRD_H;
        const uint w3 = b2 + NRD_H, b3 = w3 + 4 * NRD_H;
        for (uint a = 0; a < NRD_H; ++a) {
            float v = gNrdNet[b1 + a];
            for (uint n2 = 0; n2 < NRD_IN; ++n2) v += gNrdNet[w1 + a * NRD_IN + n2] * f[n2];
            h1[a] = max(v, 0.0);
        }
        for (uint a2 = 0; a2 < NRD_H; ++a2) {
            float v = gNrdNet[b2 + a2];
            for (uint n3 = 0; n3 < NRD_H; ++n3) v += gNrdNet[w2 + a2 * NRD_H + n3] * h1[n3];
            h2[a2] = max(v, 0.0);
        }
        float4 outD = 0.0;
        [unroll] for (uint o = 0; o < 4u; ++o) {
            float v = gNrdNet[b3 + o];
            for (uint n4 = 0; n4 < NRD_H; ++n4) v += gNrdNet[w3 + o * NRD_H + n4] * h2[n4];
            outD[o] = kNrdDeltaMax * tanh(v);
        }
        gsNrdDelta = gsNrdS1[0] < 1.0 ? 0.0 : outD;   // no surface in this tile: nothing to correct
    }
    GroupMemoryBarrierWithGroupSync();
    return gsNrdDelta;
}

void nrdResolve(int2 dtid, int2 gtid) {
    // S for this 8x8 tile and a one-pixel apron, for the history clamp's 3x3 min/max. The network's
    // correction is this tile's; the apron borrows it.
    const int2 origin = dtid - gtid - 1;
    const uint gi = uint(gtid.y * 8 + gtid.x);
    float4 delta = 0.0;
    NrdPixel px;
    if (nrdNetwork()) {
        nrdSpatialEstimate(dtid, 0.0, px);
        delta = nrdTileDelta(gi, px);
    }
    for (uint i = gi; i < 100u; i += 64u)
        gsNrdS[i] = nrdSpatialEstimate(origin + int2(i % 10u, i / 10u), delta, px);
    GroupMemoryBarrierWithGroupSync();
    if (any(dtid >= int2(gDnsrSize))) return;

    const uint c = uint(gtid.y + 1) * 10u + uint(gtid.x + 1);
    const float3 sNow = gsNrdS[c];
    float3 lo = sNow, hi = sNow;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x) {
            const float3 v = gsNrdS[int(c) + y * 10 + x];
            lo = min(lo, v); hi = max(hi, v);
        }
    const float3 hist = clamp(DNSR_LOAD3(gDnsrReprojected.Load(int3(dtid, 0))), lo, hi);
    const float  n = gDnsrSampleCount.Load(int3(dtid, 0));
    const bool   moving = dot(gDnsrMotion.Load(int3(dtid, 0)), gDnsrMotion.Load(int3(dtid, 0))) > 0.0025;
    const float  frames = min(n, float(moving ? kNrdFramesMoving : kNrdFramesStill));
    const float  a = (dnsrHistoryReset() || frames <= 1.0) ? 0.0 : 1.0 - 1.0 / frames;
    float3 outS = lerp(sNow, hist, a);
    float  var = lerp(FFX_DNSR_Reflections_ComputeTemporalVariance(outS, hist),
                      gDnsrPrefilteredVariance.Load(int3(dtid, 0)), a);
    if (any(isnan(outS)) || any(isinf(outS)) || isnan(var) || isinf(var)) { outS = 0.0; var = 0.0; }
    gDnsrOut[dtid]         = DNSR_STORE(outS / dnsrScale());
    gDnsrVarianceOut[dtid] = var;
}

// Without bit 3 (no weights, or the developer flag off) it is FidelityFX's resolve exactly.
[numthreads(8, 8, 1)]
void CSDenoiseNrdResolve(int2 dtid : SV_DispatchThreadID, int2 gtid : SV_GroupThreadID) {
    if (nrdSpatial()) nrdResolve(dtid, gtid);
    else FFX_DNSR_Reflections_ResolveTemporal(dtid, gtid, gDnsrSize, gDnsrInvSize, gDnsrHistoryClipWeight);
}
#endif

// =================================================================================================
#elif AVER_DNSR_PASS == 5   // ---- NRD pyramid (current frame only) ----

RWTexture2D<float4> gNrdLevel1Out : register(u0);
RWTexture2D<float4> gNrdLevel2Out : register(u1);
RWTexture2D<float4> gNrdLevel3Out : register(u2);

// One 8x8 group: its 4x4 texels of level 1, 2x2 of level 2, one of level 3. Each level keeps the
// nearest surface in its block (a thin object in front keeps its own texel), weighting the others
// by how far they sit behind it. View Z is stored * 0.01 to stay inside fp16.
groupshared float4 gsNrdIn[64];
groupshared float4 gsNrdL1[16];
groupshared float4 gsNrdL2[4];

float4 nrdReduce(float4 a, float4 b, float4 c, float4 d) {
    float zmin = 1.0e30;
    const float4 v[4] = {a, b, c, d};
    [unroll] for (uint i = 0; i < 4u; ++i) if (v[i].w > 0.0) zmin = min(zmin, v[i].w);
    if (zmin >= 1.0e30) return 0.0;
    float3 sum = 0.0;
    float  wsum = 0.0, zsum = 0.0;
    [unroll] for (uint j = 0; j < 4u; ++j) {
        if (v[j].w <= 0.0) continue;
        const float w = exp(-(v[j].w - zmin) / zmin * 16.0);
        sum += w * v[j].rgb; zsum += w * v[j].w; wsum += w;
    }
    return float4(sum / wsum, zsum / wsum);
}

[numthreads(8, 8, 1)]
void CSDenoiseNrdPyramid(int2 dtid : SV_DispatchThreadID, int2 gtid : SV_GroupThreadID) {
    const uint gi = uint(gtid.y * 8 + gtid.x);
    const int2 p = dnsrClampPixel(dtid);
    const float z = dnsrLoadViewZ(p);
    const float3 v = DNSR_LOAD3(gDnsrInput.Load(int3(p, 0)));
    bool valid = all(dtid < int2(gDnsrSize)) && z > 0.0 && z < 1.0e6 && !any(isnan(v)) && !any(isinf(v));
    if (dnsrHalfRateInput() && ((uint(p.x) ^ uint(p.y) ^ dnsrHalfRateParity()) & 1u) != 0u) valid = false;
    gsNrdIn[gi] = valid ? float4(v, z * 0.01) : 0.0;
    GroupMemoryBarrierWithGroupSync();

    if (((gtid.x | gtid.y) & 1) == 0) {
        const uint b = gi;
        const float4 r = nrdReduce(gsNrdIn[b], gsNrdIn[b + 1], gsNrdIn[b + 8], gsNrdIn[b + 9]);
        gsNrdL1[(gtid.y / 2) * 4 + gtid.x / 2] = r;
        gNrdLevel1Out[dtid / 2] = r;
    }
    GroupMemoryBarrierWithGroupSync();
    if (((gtid.x | gtid.y) & 3) == 0) {
        const uint b = uint(gtid.y / 2) * 4u + uint(gtid.x / 2);
        const float4 r = nrdReduce(gsNrdL1[b], gsNrdL1[b + 1], gsNrdL1[b + 4], gsNrdL1[b + 5]);
        gsNrdL2[(gtid.y / 4) * 2 + gtid.x / 4] = r;
        gNrdLevel2Out[dtid / 4] = r;
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0u) gNrdLevel3Out[dtid / 8] = nrdReduce(gsNrdL2[0], gsNrdL2[1], gsNrdL2[2], gsNrdL2[3]);
}

// =================================================================================================
#elif AVER_DNSR_PASS == 6   // ---- NRD training capture: the converged input, from fresh pixels only ----

RWTexture2D<float4> gNrdCapAccum : register(u0);   // rgb running mean, a = fresh samples so far

[numthreads(8, 8, 1)]
void CSDenoiseCapture(int2 dtid : SV_DispatchThreadID) {
    if (any(dtid >= int2(gDnsrSize))) return;
    float4 acc = (gDnsrFlags & 16u) != 0u ? 0.0 : gNrdCapAccum[dtid];
    const bool fresh = !dnsrHalfRateInput() || ((uint(dtid.x) ^ uint(dtid.y) ^ dnsrHalfRateParity()) & 1u) == 0u;
    const float3 v = DNSR_LOAD3(gDnsrInput.Load(int3(dtid, 0)));
    if (fresh && all(isfinite(v))) { acc.a += 1.0; acc.rgb += (v - acc.rgb) / acc.a; }
    gNrdCapAccum[dtid] = acc;
}

#endif
