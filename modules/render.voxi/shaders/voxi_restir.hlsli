// voxi_restir.hlsli -- the ReSTIR GI block (giMode == 1).
// Holds: reservoir buffer, surface history, surface/target-function layers, candidate trace, spatio-temporal reuse.
// Reservoir maths is voxi_reservoir.hlsli. Precedes giRestirIndirect entry point in voxi.hlsl.
// HLSL dependencies: gScene, materials, RT machinery; cbuffers gGiRestirParams, gRtHistParams, etc.
// Ordering: helpers must precede their first callers (no forward declarations in HLSL).

// Half-rate ReSTIR GI (staged mode 2): checkerboard trace, denoiser reconstructs skipped half (CSRdGi only).
#ifndef AVER_GI_CHECKERBOARD
#define AVER_GI_CHECKERBOARD 0
#endif

// Candidate-trace/resample split (staged mode): CSRdGiTrace traces/shades, CSRdGi resamples from gRdGiCand.
#ifndef AVER_GI_SPLIT
#define AVER_GI_SPLIT 0
#endif

// Radiance cache (RestirVisibility::Cached): scatter/lookup code only in staged-compute pipelines (CSRdGi, CSRdGiTrace).
#ifndef AVER_NEURAC
#define AVER_NEURAC 0
#endif
#if AVER_NEURAC && AVER_RD_SINGLE_PASS
#error AVER_NEURAC is staged-compute only: the single-pass PSRayDriven is at the register limit
#endif

// ================= ReSTIR GI (Settings::giMode == 1) =================
// In-house: reservoir (voxi_reservoir.hlsli) + reuse (giSpatioTemporalReuse) + engine-specific trace/surface.
// Scope: candidate + fused spatio-temporal reuse from PREVIOUS frame (race-free per pixel).

// Cosine floor for both candidate gates and the stored reservoir's 1/pdf bound.
#define AVER_GI_MIN_COS 0.05

// Candidate-hit material-map footprint: tan(cone half-angle) x ray length.
#define AVER_GI_HIT_TEX_CONE 0.1

// Voxel-lookup occupancy floor: avoid dividing by near-zero coverage.
#define AVER_GI_VOX_MIN_OCC 0.05


// ---- the reservoir buffer: both ping-pong slices in one buffer ----
// Row-major per slice, slice-major across the two: element = slice * (w*h) + y * w + x.
// VoxiRenderer.cpp's giReservoirElemCount sizes it from the same width/height as gGiSurfNrmHist.
// Slice gGiRestirParams.z is written this frame; the other holds last frame's reservoirs.
#include "voxi_reservoir.hlsli"
RWStructuredBuffer<GiPackedReservoir> gGiReservoirs : register(u6);

// ---- the previous-frame SURFACE history giLoadPrevSurface reads ----
// Two RG32Float textures: xy world pos (t12) + z pos & packed normal (t13).
// Packed normal is octahedral uint, 0 is the "never written" sentinel.
Texture2D<float2>   gGiSurfPosHist    : register(t12);
RWTexture2D<float2> gGiSurfPosHistOut : register(u7);
Texture2D<float2>   gGiSurfNrmHist    : register(t13);
RWTexture2D<float2> gGiSurfNrmHistOut : register(u8);

// Half-res visibility history: one pixel per 2x2 block traces; others reconstruct.
// RGBA16F: r=F3 reuse-visibility EMA, g=F2 traced-lum EMA, b=F2 sky-lum EMA, a=written sentinel.
Texture2D<float4>   gGiVisHist    : register(t16);
RWTexture2D<float4> gGiVisHistOut : register(u10);

// Candidate hand-off between CSRdGiTrace (traces) and CSRdGi (resamples); same-frame relay.
struct RdGiCand { float3 pos; uint flags; float3 nrm; float f2LumTraced; float3 rad; float f2LumSky; };
RWStructuredBuffer<RdGiCand> gRdGiCand : register(u17);
// Set by CSRdGi per invocation before calling giRestirIndirect: row-pitch pixel index.
// `static`, not a parameter, like gGiPoisonPdfHit -- giRestirIndirect's signature is shared.
static uint gGiCandIdx = 0;

#if AVER_NEURAC
// ---- RADIANCE CACHE RESOURCES (twin pipelines only; see AVER_NEURAC above) ----
// Order: pure maths (structs/constants), then declarations, then io half.
// t22 is CPU-written Upload ring slot (fixed GENERIC_READ); accum and cells are UAV-only.
#include "voxi_neurac.hlsli"
StructuredBuffer<RcInfo>   gRcInfo  : register(t22);
RWStructuredBuffer<int>    gRcAccum : register(u20);
RWStructuredBuffer<RcCell> gRcCells : register(u21);
#include "voxi_neurac_io.hlsli"
#endif

// A receiving surface: this pixel's (giRestirIndirect) or a reprojected previous-frame one.
// `linearDepth` is carried rather than re-derived per read, keyed to its source matrix.
struct GiSurface {
    bool   valid;
    float3 worldPos;
    float3 normal;
    float  linearDepth;
};
GiSurface giEmptySurface() {
    GiSurface s;
    s.valid = false;
    s.worldPos = 0.0;
    s.normal = float3(0, 0, 1);
    s.linearDepth = 0.0;
    return s;
}

// LAST frame's primary surface at `pixelPosition`, or invalid (sky, outside texture, or no previous frame).
GiSurface giLoadPrevSurface(int2 pixelPosition) {
    GiSurface s = giEmptySurface();
    if (gGiRestirParams.y < 0.5) return s;
    uint texW, texH;
    gGiSurfNrmHist.GetDimensions(texW, texH);
    if (pixelPosition.x < 0 || pixelPosition.y < 0 ||
        pixelPosition.x >= (int)texW || pixelPosition.y >= (int)texH) return s;
    // Sentinel decides if position channel is worth reading (both written together).
    const float2 nrmRaw = gGiSurfNrmHist.Load(int3(pixelPosition, 0));
    const uint packedN = asuint(nrmRaw.y);
    if (packedN == 0u) return s;
    const float2 posXY = gGiSurfPosHist.Load(int3(pixelPosition, 0));
    s.valid = true;
    s.worldPos = float3(posXY, nrmRaw.x);
    s.normal = giOctDecode(packedN);
    // Linearize from stored position to avoid requantisation disagreement.
    s.linearDepth = mul(float4(s.worldPos, 1.0), gPrevViewProj).w;
    return s;
}

// ---- POISON-VIEW INSTRUMENTATION: thread-private flag giTargetPdf sets ----
// Per-invocation storage, reachable from helpers without changing signature.
// giRestirIndirect resets this false before resampling, reads it back at the end.
static bool gGiPoisonPdfHit = false;

// Target pdf = luminance × cosine at receiving surface.
float giTargetPdf(float3 samplePosition, float3 sampleRadiance, GiSurface surface) {
    if (!surface.valid) return 0.0;

    const float3 toSample = samplePosition - surface.worldPos;
    const float  dist2    = dot(toSample, toSample);
    const float  rawCos   = dist2 > 1e-8 ? dot(toSample * rsqrt(dist2), surface.normal) : -1.0;
    const float  cosR     = rawCos > 0.0 ? max(rawCos, AVER_GI_MIN_COS) : 0.0;
    const float pdf = averShadowLum(sampleRadiance) * cosR;
    if (isnan(pdf) || isinf(pdf)) { gGiPoisonPdfHit = true; return 0.0; }
    return pdf;
}

// Jacobian [1/4, 4]: decide acceptance for reprojection.
bool giAcceptJacobian(inout float jacobian) {
    if (isnan(jacobian) || isinf(jacobian) || jacobian <= 0.0) return false;
    if (jacobian < 0.25 || jacobian > 4.0) return false;
    jacobian = 1.0;
    return true;
}

// Depth/normal similarity: normals within `normalThreshold` (cosine) and depth within `depthThreshold` fraction.
bool giIsSimilarSurface(GiSurface prev, float3 normal, float expectedPrevDepth,
                        float normalThreshold, float depthThreshold) {
    if (!prev.valid) return false;
    if (dot(prev.normal, normal) < normalThreshold) return false;
    return abs(prev.linearDepth - expectedPrevDepth) <= depthThreshold * max(expectedPrevDepth, 1e-4);
}

// Half-res reconstruction tunables (mirrored in GiVisibility.hpp).
#define AVER_GI_VIS_HIST_WEIGHT 0.8
#define AVER_GI_VIS_RHO_MAX 4.0
#define AVER_GI_VIS_NORMAL_POW 8.0
#define AVER_GI_VIS_PLANE_TOL_REL 0.02
#define AVER_GI_VIS_PLANE_TOL_CM 1.0

// Spatial taps stay on last frame's screen; clamp to gSceneViewport.
int2 giClampToPrevViewport(int2 pixelPosition) {
    const int2 lo = int2(gSceneViewport.xy);
    const int2 hi = lo + int2(gSceneViewport.zw) - 1;
    return clamp(pixelPosition, lo, max(hi, lo));
}

bool giInsidePrevViewport(int2 pixelPosition) {
    const int2 lo = int2(gSceneViewport.xy);
    const int2 hi = lo + int2(gSceneViewport.zw);
    return all(pixelPosition >= lo) && all(pixelPosition < hi);
}

// ---- reservoir addressing, load and store ----
// Pitch derived from gGiSurfNrmHist's actual dimensions, matching VoxiRenderer.cpp's giReservoirElemCount.
uint giReservoirIndex(uint2 pixelPosition, uint slice) {
    uint w, h;
    gGiSurfNrmHist.GetDimensions(w, h);
    return slice * (w * h) + pixelPosition.y * w + pixelPosition.x;
}

GiReservoir giLoadReservoir(uint2 pixelPosition, uint slice) {
    return giUnpackReservoir(gGiReservoirs[giReservoirIndex(pixelPosition, slice)]);
}

void giStoreReservoir(GiReservoir r, uint2 pixelPosition, uint slice) {
    gGiReservoirs[giReservoirIndex(pixelPosition, slice)] = giPackReservoir(r);
}

// ---- U1's HALF-RESOLUTION RECONSTRUCTION ----
// giVisReconstruct calls giLoadPrevSurface (reuses bounds check and t12/t13 reads).
// Called before giTraceInitialCandidate, so must precede that call site.

// kGiVisPhase: which 2x2 block pixel traces on each frame (cycles every 4).
// giVisTracedPixel decides THIS frame's; giVisReconstruct reads PREVIOUS frame's (frameIdx-1).
static const uint2 kGiVisPhase[4] = { uint2(0, 0), uint2(1, 1), uint2(1, 0), uint2(0, 1) };

// Is pixel `p` this block's traced pixel on frame `frame`? (& 3u for power-of-two cycle).
bool giVisTracedPixel(uint2 p, uint frame) {
    const uint2 ph = kGiVisPhase[frame & 3u];
    return (p.x & 1u) == ph.x && (p.y & 1u) == ph.y;
}

// One reconstructed visibility sample: `valid` says whether any tap survived rejection.
// Mirrors gGiVisHist's r/g/b (F3 reuse-vis EMA, F2 traced-lum EMA, F2 sky-lum EMA).
struct GiVisRecon { bool valid; float v3; float g; float b; float motionPx; };

// Reconstructs F2/F3 from non-traced neighbours' half-resolution history, depth/normal-weighted.
// Expected F2/F3 answer is smooth in (position, normal); weighting both preserves that.
GiVisRecon giVisReconstruct(float3 wpos, float3 N, float2 pixel, uint frameIdx) {
    GiVisRecon rec = (GiVisRecon)0;

    // Check whether surface history and visibility history are bound.
    // gGiRestirParams.y: previous-frame surface (t12/t13) exists.
    // gAmbientParams.w bit 8: gGiVisHist holds real previous frame.
    if (((uint)gAmbientParams.w & 8u) == 0u || gGiRestirParams.y < 0.5) return rec;

    // Reprojection: same recipe as giRestirIndirect (gPrevViewProj + gSceneViewport).
    const float4 prevClip = mul(float4(wpos + gAverReprojDelta, 1.0), gPrevViewProj);
    if (prevClip.w <= 1e-4) return rec;
    const float3 prevNdc = prevClip.xyz / prevClip.w;
    const float2 prevPx = gSceneViewport.xy +
        float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * gSceneViewport.zw;
    rec.motionPx = length(prevPx - pixel);

    // Bilinear taps into half-resolution history. Halve prevPx and recenter (-0.5).
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
        if (t.x < 0 || t.y < 0 || t.x >= (int)visW || t.y >= (int)visH) continue;
        if (wgt[k] <= 0.0) continue;

        // Writer's full-res pixel from LAST frame's phase (frameIdx-1, unsigned wrap deliberate).
        const uint2 writer = uint2(t) * 2u + kGiVisPhase[(frameIdx - 1u) & 3u];
        const GiSurface ps = giLoadPrevSurface(int2(writer));
        if (!ps.valid) continue;

        const float4 vh = gGiVisHist.Load(int3(t, 0));
        if (vh.a < 0.5) continue;

        // Plane test: flat floor + fraction of distance.
        const float planeTol = AVER_GI_VIS_PLANE_TOL_CM + AVER_GI_VIS_PLANE_TOL_REL * ps.linearDepth;
        if (abs(dot(ps.worldPos - (wpos + gAverReprojDelta), N)) > planeTol) continue;

        const float nDot = saturate(dot(ps.normal, N));
        const float w = wgt[k] * pow(nDot, AVER_GI_VIS_NORMAL_POW);
        wsum += w; rsum += w * vh.r; gsum += w * vh.g; bsum += w * vh.b;
    }

    // Valid past a noise floor: 1e-3 floors the sum of up to four weights.
    rec.valid = wsum > 1e-3;
    if (rec.valid) {
        rec.v3 = rsum / wsum;
        rec.g  = gsum / wsum;
        rec.b  = bsum / wsum;
    }
    return rec;
}

// ---- T4 (Settings::rtGiHitShadowMap): sun visibility at a GI hit from the GI-only shadow map ----
// Returns -1 where the map can't answer; caller then fires a shadow ray instead.
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

// ---- the initial candidate: ONE cosine ray, traced and shaded ----
// Traces off (wpos, N) along cosine-weighted hemisphere and shades the hit through RT machinery.
// Returns false when no candidate was drawn (grazing-direction reject): caller starts from EMPTY reservoir.
// U1's F2 PATH SELECTOR AND OUTPUTS (2.10 A/B): `f2Path` is the decode block's verdict
// (3 trace / 2 half-res / 1 reconstructed / 0 legacy), `rho2` is the half-res reconstruction's occlusion ratio.
bool giTraceInitialCandidate(float3 wpos, float3 N, float2 pixel, float frameJitter,
                             out float3 samplePos, out float3 sampleNormal, out float3 sampleRadiance,
                             out bool nonFiniteCandidate, uint f2Path, float rho2,
                             out float f2LumTraced, out float f2LumSky, out bool f2Observed) {
    samplePos = sampleNormal = sampleRadiance = 0.0;
    nonFiniteCandidate = false;
    f2LumTraced = 0.0; f2LumSky = 0.0; f2Observed = false;

    // Cosine-weighted hemisphere sample (Malley's method); pdf cancels at the call site.
    // Per-pixel-rotated low-discrepancy sequence, jittered per frame (not frozen per pixel).
    // F1 (R0), gAmbientParams.z bit 1: legacy 45-degree ring vs. cosine hemisphere.
    // rtHemiDiscSample drawn uniform over frames (corrected); streamSalt 0.0 keeps this stream apart.
    const float2 xi = (((uint)gAmbientParams.z & 1u) != 0u) ? rtDiscSample(0, rtHash(pixel) * 6.2831853 + frameJitter) : rtHemiDiscSample(0u, 1u, (uint)gRtHistParams.z, pixel, 0.0);
    const float  cosTheta = sqrt(saturate(1.0 - dot(xi, xi)));
    // Grazing floor: 0.05 bounds W at ~63 (500x worst-case). Rejects ~0.25% of directions.
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
    r.TMax      = max(gVoxelParams.z, 1.0);

    RayQuery<RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> q;
    q.TraceRayInline(gScene, RAY_FLAG_NONE | gAverRtSecondaryRayFlags, AVER_RT_MASK_OPAQUE_ALL, r);
    averRtProceedSolid(q);
    // A miss samples the sky; cosine ray's sky is most of the hemisphere and most indirect bounce.
    // sampleNormal faces back down the ray; samplePos sits at TMax (real, finite direction).
    if (q.CommittedStatus() != COMMITTED_TRIANGLE_HIT) {
        samplePos      = wpos + dir * r.TMax;
        sampleNormal   = -dir;
        // NaN-safe clamp: min(max(x,lo),hi) floors NaN to lo.
        {
            const float3 rawSky = averSkyRadianceCheap(dir) * gAmbient.r;
            const bool   bad    = any(isnan(rawSky)) || any(isinf(rawSky));
            nonFiniteCandidate  = nonFiniteCandidate || bad;
            sampleRadiance      = bad ? float3(0.0, 0.0, 0.0) : min(max(rawSky, 0.0), AVER_VOX_MAXRAD);
        }
        return true;
    }

    // The hit's own surface, built like every ray hit's (voxi_rt.hlsli's rtHitSurface): its maps through
    // the shared composition, a cone footprint for the mips. Lite in the single pass (register limit).
    const RtHit h = rtHitCommitted(q, wpos, dir);
    const float3 hitPos = h.pos;
    const float3 L = normalize(gLightDir.xyz);
    float2 hgx, hgy;
    rtHitConeGrad(h, AVER_GI_HIT_TEX_CONE, hgx, hgy);
    AverSurface s = rtHitSurface(h, -dir, L, hgx, hgy, AVER_RD_SINGLE_PASS ? AVER_RT_HIT_LITE : AVER_RT_HIT_FULL);
#if AVER_RD_LAMPS
    // Lamp glow already lit by direct term (rdLocalLightsShade); don't double-count.
    if ((h.mat.flags & AVER_MAT_LIGHT) != 0u && rdLocalCarriesEmitters())
        s.emissive = float3(0.0, 0.0, 0.0);
#endif
    AverLight sun;
    sun.direction = L;
    sun.radiance  = averSunRadiance();
    // One fresh shadow ray (not temporal gRtShadowHist): keyed by screen pixel, this sample is world-space.
    // T1 (Settings::rtSecondaryShadowOpaque): hitPos is the secondary hit.
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

    // The hit's emission is the seed; averShadeDirect adds the direct sun term.
    float3 radiance = averShadeDirect(s.emissive, s, sun);

#if AVER_PT_PATHS
    // PATH TRACING: the candidate is a whole path. The lamps light this vertex too, and the path
    // continues through its BSDF for Settings::ptBounces - 1 more vertices (voxi_pt.hlsli) in place of
    // the cached/approximate second bounce below. ReSTIR resamples the path's radiance as before.
    {
        uint rng = ptSeed(pixel, 0x2c1bu);
        PtVertex v;
        v.pos   = hitPos;
        v.s     = s;
        v.cover = 1.0;   // the candidate ray sees the opaque lane only
        radiance += ptLamp(s, hitPos, pixel, rng);
        const float3 li = ptContinue(v, pixel, rng, ptBounceCount() - 1u);
        radiance += li;
        f2Observed  = true;
        f2LumTraced = averShadowLum(li);
        f2LumSky    = averShadowLum(averSkyIrradiance(s.N) * gAmbient.r);
        samplePos    = hitPos;
        sampleNormal = s.N;
        const bool bad     = any(isnan(radiance)) || any(isinf(radiance));
        nonFiniteCandidate = nonFiniteCandidate || bad;
        sampleRadiance     = bad ? float3(0.0, 0.0, 0.0) : min(max(radiance, 0.0), AVER_VOX_MAXRAD);
        return true;
    }
#endif

    // Diffuse ambient only: no unoccluded environment-specular term.
    // F2 (R2): diffuse now owns its own visibility via cosine ray (not unoccluded sky).
    // gAmbientParams.z bit 4 TRUE keeps legacy unoccluded read; FALSE traces one more cosine ray.
    // U1 (2.10 B): four paths (legacy / reconstructed / half / traced) instead of two.
    float3 indY = 0.0;
    if (((uint)gAmbientParams.z & 4u) != 0u || f2Path == 0u) {
        indY = averSkyIrradiance(s.N) * gAmbient.r;
    } else if (f2Path == 1u) {
        // Reconstructed: one voxel-cone march stands in for the traced ray.
        // Cone's radiance only (no sky fallback): the volume is one-voxel shells, sparsely sampled.
        if (gVoxelParams.w > 0.5) {
            const float4 cone = traceCone(hitPos, s.N, AVER_VOX_INJECT_APERTURE);
            indY = min(cone.rgb, AVER_VOX_MAXRAD);
        } else indY = averSkyIrradiance(s.N) * gAmbient.r;
#if AVER_NEURAC
    } else if (f2Path == 4u) {
        // Cached: read the radiance cache in place of the second-bounce ray.
        // rcLookup returns cosine-convolved irradiance/PI; remainder filled with path-2 sky ratio.
        float rem;
        const float3 cached = rcLookup(hitPos, s.N, rem);
        indY = cached + rem * (averSkyIrradiance(s.N) * gAmbient.r * clamp(rho2, 0.0, AVER_GI_VIS_RHO_MAX));
        indY = min(max(indY, 0.0), AVER_VOX_MAXRAD);
#endif
    } else if (f2Path == 2u) {
        // Half non-traced: no ray, no cone, one occlusion ratio.
        indY = averSkyIrradiance(s.N) * gAmbient.r * clamp(rho2, 0.0, AVER_GI_VIS_RHO_MAX);
    } else {
        // Full traced: second cosine ray from the hit.
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
            // Voxel shell straddle: offset lookup half a voxel back to recenter on the room side.
            const float voxelWorldF2 = 1.0 / (gVoxelOrigin.w * gVoxelParams.x);
            const float3 uvw = voxelUVW(r2.Origin + dir2 * (q2.CommittedRayT() - voxelWorldF2 * 0.5));
            if (gVoxelParams.w > 0.5 && insideVolume(uvw)) {
                const float4 vox = gVoxelTex.SampleLevel(gVoxelSamp, uvw, 0);
                // Normalise by occupancy (alpha) rather than premultiplied mean.
                indY = vox.a > AVER_GI_VOX_MIN_OCC ? min(vox.rgb / vox.a, AVER_VOX_MAXRAD) : 0.0;
            }
        }
        // Only reachable at f2Path == 3u (traced path).
#if AVER_NEURAC
        if (rcCacheOn()) rcScatter(hitPos, s.N, dir2, indY, c2);
#endif
        f2Observed  = true;
        f2LumTraced = averShadowLum(indY);
        f2LumSky    = averShadowLum(averSkyIrradiance(s.N) * gAmbient.r);
    }
    radiance += s.kdAlbedo * indY;

    samplePos      = hitPos;
    sampleNormal   = s.N;
    // Clamp before reservoir: an unclamped spike doesn't flicker one frame, it spreads 30+ frames through reuse.
    // NaN-safe clamp: min(max(x,lo),hi) floors NaN to lo.
    {
        const bool bad     = any(isnan(radiance)) || any(isinf(radiance));
        nonFiniteCandidate = nonFiniteCandidate || bad;
        sampleRadiance     = bad ? float3(0.0, 0.0, 0.0) : min(max(radiance, 0.0), AVER_VOX_MAXRAD);
    }
    return true;
}

// ---- the call site's one entry point: candidate + spatio-temporal reuse, in and out ----
// Drop-in replacement for coneTracedIndirect's contract (float3 diffuse radiance, `ao` out).
// `ao` stays 1.0 always: ReSTIR GI's one resampled candidate per pixel does not estimate AO coverage.
#if AVER_GI_CHECKERBOARD
// `static`, like gGiPoisonPdfHit above: giRestirIndirect's signature is shared.
// CSRdGi (voxi.hlsl) sets this per invocation before calling giRestirIndirect.
static bool gGiCbSkip = false;
#endif

// Visibility-mode + F2/F3 path decode (shared by trace/resample split at CSRdGiTrace).
// visMode = settings_.giRestirVisibility (0/1/2/3 = No ray/Reconstructed/Half/Full).
// giVisReconstruct called once; both F2/F3 share same reconstruction.
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
    // spatialSamples: nibble at bit 12 of gAmbientParams.w (givis::packAmbientW); 15 = AUTO.
    const uint spatialSamples = ((uint)gAmbientParams.w >> 12) & 15u;
    // maxHistory: bits 18-22 of gAmbientParams.w; caps M from previous-frame reservoirs (default 0 disables reuse).
    const uint maxHistory     = ((uint)gAmbientParams.w >> 18) & 31u;
    // HLSL conditional operator doesn't support struct results; use if instead.
    GiVisRecon rec = (GiVisRecon)0;
    if (halfBound) rec = giVisReconstruct(wpos, N, pixel, frameIdx);
    // Path numbers: 3=trace, 2=half-res ratio, 1=reconstructed, 0=legacy/no-ray.
    uint f2Path = 3u, f3Path = 3u;
    if (visMode == 1u)                       { f2Path = 1u; f3Path = 1u; }
    if (halfBound && !tracedPx && rec.valid) { f2Path = 2u; f3Path = 2u; }   // no valid reconstruction: trace, as Full
#if AVER_NEURAC
    // Cached path 4 for valid reconstructions (gAmbientParams.w bit 128); f3Path stays 2.
    if (rcCacheOn() && halfBound && !tracedPx && rec.valid) f2Path = 4u;
#endif
    if (((uint)gAmbientParams.z & 4u) != 0u || visMode == 0u) f2Path = 0u;   // legacy bit wins (2.8)
    if (((uint)gAmbientParams.z & 8u) != 0u || visMode == 0u) f3Path = 0u;
#if AVER_GI_CHECKERBOARD
    // Skipped pixels trace no F3 ray; only Full-traced path (3). f2Path goes to 0 (yellow, "no ray").
    // Half-res history loses half sub-pixel positions, not half refresh: fixed offset with 4-frame cycle.
    if (gGiCbSkip) {
        f2Path = 0u;
        if (f3Path == 3u) f3Path = 0u;
    }
#endif

    // rho2: F2's reconstructed-non-traced path reads the neighbourhood's occluded/unoccluded sky ratio.
    const float rho2 = (rec.b > 1e-4) ? (rec.g / rec.b) : 1.0;

    GiPathDecode d;
    d.visMode = visMode; d.halfBound = halfBound; d.tracedPx = tracedPx; d.rec = rec;
    d.spatialSamples = spatialSamples; d.maxHistory = maxHistory;
    d.f2Path = f2Path; d.f3Path = f3Path; d.rho2 = rho2;
    return d;
}

// ---- THE REUSE PASS: one fused spatio-temporal resample over last frame's reservoirs ----
// Streams: this frame's fresh candidate, then up to one temporal tap and GI_RESTIR_MAX_SPATIAL spatial taps.
// Temporal: reprojected pixel with jittered retries; Spatial: uniform disc taps around it.
// A tap joins only if surface is similar (giIsSimilarSurface), reservoir is valid/young, and Jacobian accepted.
// Selection is streaming RIS; weights are M * target(here) * W * |J| per stream.
#define GI_RESTIR_MAX_SPATIAL      8u
#define GI_RESTIR_TEMPORAL_RETRIES 4u
#define GI_RESTIR_TEMPORAL_RADIUS  1.5
// Reused samples older than this many frames are dropped.
#define GI_RESTIR_MAX_AGE          30u

struct GiReuseParams {
    uint  maxHistory;        // M cap per reused reservoir (Settings::giRestirMaxHistory)
    uint  maxAge;            // reused samples at or past this age are dropped
    uint  numSamples;        // spatial taps, at most GI_RESTIR_MAX_SPATIAL
    float samplingRadius;    // spatial tap radius, pixels
    float depthThreshold;    // relative linear-depth tolerance for a similar surface
    float normalThreshold;   // minimum normal cosine for a similar surface
};

GiReservoir giSpatioTemporalReuse(float2 pixel, GiSurface surface, float3 screenSpaceMotion,
                                  uint prevSlice, GiReservoir fresh, inout GiRng rng, GiReuseParams rp) {
    const uint kStreams = 1u + GI_RESTIR_MAX_SPATIAL;   // slot 0 temporal, 1.. spatial
    int2 tapPx[kStreams];
    uint tapM[kStreams];
    [unroll] for (uint z = 0u; z < kStreams; ++z) { tapPx[z] = int2(0, 0); tapM[z] = 0u; }

    GiReservoir selected = giEmptyReservoir();
    uint  selStream = 0u;        // 0 none, 1 fresh, 2 + slot for a reused tap
    float weightSum = 0.0;
    uint  mTotal    = 0u;

    if (giIsValidReservoir(fresh)) {
        mTotal += fresh.M;
        const float w = giTargetPdf(fresh.position, fresh.radiance, surface) * fresh.W * (float)fresh.M;
        if (giStreamAccept(weightSum, w, giRandom(rng))) { selected = fresh; selStream = 1u; }
    }

    if (rp.maxHistory > 0u) {
        // Where this surface sat last frame, and the depth it should have had there.
        const float2 prevPx            = pixel + screenSpaceMotion.xy;
        const float  expectedPrevDepth = surface.linearDepth + screenSpaceMotion.z;
        int2 temporalPx = int2(-1, -1);
        const uint numSpatial = min(rp.numSamples, GI_RESTIR_MAX_SPATIAL);

        [unroll] for (uint k = 0u; k < kStreams; ++k) {
            if (k > numSpatial) break;
            int2 px = int2(-1, -1);
            GiSurface ns = giEmptySurface();
            if (k == 0u) {
                // Temporal: reprojected pixel first, then jittered retries.
                [loop] for (uint t = 0u; t <= GI_RESTIR_TEMPORAL_RETRIES; ++t) {
                    const float2 jitter = t == 0u ? float2(0.0, 0.0) : giDiscOffset(rng, GI_RESTIR_TEMPORAL_RADIUS);
                    const int2 cand = int2(floor(prevPx + jitter));
                    if (!giInsidePrevViewport(cand)) continue;
                    const GiSurface cs = giLoadPrevSurface(cand);
                    if (giIsSimilarSurface(cs, surface.normal, expectedPrevDepth,
                                           rp.normalThreshold, rp.depthThreshold)) {
                        px = cand; ns = cs;
                        break;
                    }
                }
                if (px.x < 0) continue;
                temporalPx = px;
            } else {
                px = giClampToPrevViewport(int2(floor(prevPx + giDiscOffset(rng, rp.samplingRadius))));
                if (all(px == temporalPx)) continue;   // already offered as the temporal stream
                ns = giLoadPrevSurface(px);
                if (!giIsSimilarSurface(ns, surface.normal, expectedPrevDepth,
                                        rp.normalThreshold, rp.depthThreshold)) continue;
            }

            GiReservoir nb = giLoadReservoir(uint2(px), prevSlice);
            if (!giIsValidReservoir(nb) || nb.age >= rp.maxAge) continue;
            nb.M = min(nb.M, rp.maxHistory);
            float jacobian = giReconnectionJacobian(surface.worldPos, ns.worldPos, nb.position, nb.normal);
            if (!giAcceptJacobian(jacobian)) continue;

            tapPx[k] = px;
            tapM[k]  = nb.M;
            mTotal  += nb.M;
            const float w = giTargetPdf(nb.position, nb.radiance, surface) * nb.W * jacobian * (float)nb.M;
            if (giStreamAccept(weightSum, w, giRandom(rng))) {
                selected     = nb;
                selected.age = nb.age + 1u;
                selStream    = 2u + k;
            }
        }
    }

    if (selStream == 0u) return giEmptyReservoir();

    // Normalise: every stream's target for the winner, at that stream's own surface.
    const float pHere = giTargetPdf(selected.position, selected.radiance, surface);
    float pSum = giIsValidReservoir(fresh) ? (float)fresh.M * pHere : 0.0;
    float pSel = selStream == 1u ? pHere : 0.0;
    [unroll] for (uint j = 0u; j < kStreams; ++j) {
        if (tapM[j] == 0u) continue;
        const float p = giTargetPdf(selected.position, selected.radiance, giLoadPrevSurface(tapPx[j]));
        pSum += (float)tapM[j] * p;
        if (selStream == 2u + j) pSel = p;
    }
    selected.W = giFinalizeWeight(weightSum, pSel, pHere, pSum);
    selected.M = mTotal;
    return selected;
}

float3 giRestirIndirect(float3 wpos, float3 N, float curLinearDepth, float2 pixel, uint frameIdx,
                        out float ao) {
    ao = 1.0;
    const uint2 pixelPos = uint2(pixel);
    const float frameJitter = (float)frameIdx * 2.39996323;

    // Reset poison-view flag before anything below can set it.
    gGiPoisonPdfHit = false;

    // Decode visibility mode and paths (U1, 2.10 A / B2); see giDecodePaths.
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
    GiReservoir initial = giEmptyReservoir();
    // Skipped pixels trace no fresh candidate; initial stays empty; spatio-temporal reuse still runs.
#if AVER_GI_SPLIT
    // B4: candidate already traced by CSRdGiTrace (voxi.hlsl); read from gRdGiCand[gGiCandIdx].
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
                initial = giMakeReservoir(samplePos, sampleNormal, sampleRadiance, cosTheta / PI);
        }
    }
#else
#if AVER_GI_CHECKERBOARD
    if (!gGiCbSkip)
#endif
    if (giTraceInitialCandidate(wpos, N, pixel, frameJitter, samplePos, sampleNormal, sampleRadiance,
                                 nonFiniteCandidate, f2Path, rho2, f2LumTraced, f2LumSky, f2Observed)) {
        const float cosTheta = saturate(dot(normalize(samplePos - wpos), N));
        // Recomputed here; must match sampled cosTheta to avoid 1/cos blow-up through reservoir.
        if (cosTheta > AVER_GI_MIN_COS)
            initial = giMakeReservoir(samplePos, sampleNormal, sampleRadiance, cosTheta / PI);
    }
#endif

    // ---- F3's OWN COPY OF THIS FRAME'S FRESH CANDIDATE, TAKEN BEFORE REUSE CAN REPLACE IT ----
    // Needed to distinguish new candidates from reused ones when testing visibility.
    const bool freshValid = giIsValidReservoir(initial);
    const float3 freshPos = initial.position;

    const uint writeSlice = (uint)(gGiRestirParams.z + 0.5);
    GiReservoir result = initial;
    if (gGiRestirParams.y > 0.5) {   // a real previous frame exists to resample against
        GiSurface surface   = giEmptySurface();
        surface.valid       = true;
        surface.worldPos    = wpos;
        surface.normal      = N;
        surface.linearDepth = curLinearDepth;

        // screenSpaceMotion: reprojection position (xy) and depth delta (z); from gPrevViewProj.
        float3 screenSpaceMotion = float3(0, 0, 0);
        {
            const float4 prevClip = mul(float4(wpos + gAverReprojDelta, 1.0), gPrevViewProj);
            if (prevClip.w > 1e-4) {
                const float3 prevNdc = prevClip.xyz / prevClip.w;
                const float2 prevPx = gSceneViewport.xy +
                    float2(prevNdc.x * 0.5 + 0.5, 0.5 - prevNdc.y * 0.5) * gSceneViewport.zw;
                screenSpaceMotion = float3(prevPx - pixel, prevClip.w - curLinearDepth);
            }
        }

        GiRng rng = giInitRng(pixelPos, frameIdx, 1u);
        GiReuseParams reuse;
        // Similarity tolerances: normal within cos 0.5, depth within 10%.
        reuse.depthThreshold  = 0.1;
        reuse.normalThreshold = 0.5;
        // maxHistory: 0 by default (disables reuse); see Settings::giRestirMaxHistory.
        reuse.maxHistory = maxHistory;
        reuse.maxAge     = GI_RESTIR_MAX_AGE;
        // Spatial taps: 2 at 32px at rest; depth/normal similarity is the hard constraint, not radius.
        // Under motion, discount spatial reuse: narrower ring and fewer taps reduce never-converging content.
        const float motionPx = length(screenSpaceMotion.xy);
        const float motionT  = saturate(motionPx / 32.0);
        reuse.numSamples = (uint)round(lerp(2.0, 1.0, motionT));
        reuse.samplingRadius = lerp(32.0, 8.0, motionT);
        // Override the motion discount via spatialSamples (see Settings::giRestirSpatialSamples).
        if (spatialSamples != 15u)
            reuse.numSamples = min(spatialSamples, GI_RESTIR_MAX_SPATIAL);
        // Reconstructed forces temporal-only (U1, 2.10 C).
        if (f3Path == 1u) reuse.numSamples = 0u;

        result = giSpatioTemporalReuse(pixel, surface, screenSpaceMotion, 1u - writeSlice, initial, rng, reuse);
    }

    // ---- Poison guards for invalid estimates ----
    // NaN/Inf stops here; otherwise poison spreads through history.
    const bool corpseWeight     = result.W <= 0.0;
    const bool nonFiniteWeight  = isnan(result.W) || isinf(result.W);
    const bool nonFiniteRad     = any(isnan(result.radiance)) || any(isinf(result.radiance));
    const bool giPoisonStoreHit = nonFiniteWeight || nonFiniteRad;
    if (corpseWeight || giPoisonStoreHit) result = giEmptyReservoir();
    // W6/M5: gAverHistoryWrite gates the reservoir store (blended fragments don't write here).
    if (gAverHistoryWrite) giStoreReservoir(result, pixelPos, writeSlice);

    // Store surface for next frame's reprojection (giLoadPrevSurface).
    const uint packedN = giOctEncode(N);
    if (gAverHistoryWrite) {
        gGiSurfPosHistOut[pixelPos] = wpos.xy;
        gGiSurfNrmHistOut[pixelPos] = float2(wpos.z, asfloat(packedN == 0u ? 1u : packedN));
    }

    // Hoisted to function scope for poison-view combination at the end.
    bool giPoisonEstHit = false;
    bool giPoisonDenoisedHit = false;
    // Ceiling clamp engaging on finite values (see POISON DEBUG VIEW below).
    bool giPoisonEstCeilHit = false;
    bool giPoisonDenoisedCeilHit = false;

    float3 outDiffuse = 0.0;
    if (giIsValidReservoir(result)) {
        // Recomputed here against THIS pixel's (wpos, N), since resampling can swap the sample.
        const float3 toSample = result.position - wpos;
        const float  dist2    = dot(toSample, toSample);
        const float  cosR     = dist2 > 1e-8 ? saturate(dot(toSample * rsqrt(dist2), N)) : 0.0;

        // ---- F3 (R3): A REUSED SAMPLE HAS NEVER BEEN CHECKED FOR VISIBILITY FROM HERE ----
        // Fresh candidate is proven visible by giTraceInitialCandidate; reused samples are not.
        // Skips the ray when not needed (fresh or degenerate sample); one ray per reuse.
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
            // Half non-traced pixel: use reconstructed visibility.
            visF3 = rec.v3;
        }

        // ---- U1 (2.10 E): THE HALF-RESOLUTION HISTORY WRITE ----
        // One texel per 2x2 block, per frame; only the traced pixel in the block writes.
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

        // Estimator: radiance * W * cosR/PI * intensity * visibility; W already includes 1/pdf.
        const float3 est = result.radiance * (cosR * result.W / PI) * gVoxelParams.y * visF3;

        // Guard against NaN/Inf in the estimate.
        giPoisonEstHit = any(isnan(est)) || any(isinf(est));
        // Guard against values that saturate acesTonemap (ceiling).
        const float3 estNonNeg = max(est, 0.0);
        giPoisonEstCeilHit = !giPoisonEstHit && any(estNonNeg > AVER_VOX_MAXRAD);
        outDiffuse = giPoisonEstHit ? float3(0.0, 0.0, 0.0)
                                     : min(estNonNeg, AVER_VOX_MAXRAD);
    }

    // Write raw estimate to denoiser; read back last frame's denoised value.
    // Write only the traced half on half-rate frames.
    bool giDenoiseInWrite = gGiRestirParams.x > 0.5;
#if AVER_GI_CHECKERBOARD
    giDenoiseInWrite = giDenoiseInWrite && !gGiCbSkip;
#else
    const uint giHalfRateWord = (uint)gViewParams.w;
    if ((giHalfRateWord & 0x20000u) != 0u)
        giDenoiseInWrite = giDenoiseInWrite && ((pixelPos.x ^ pixelPos.y ^ (giHalfRateWord >> 16)) & 1u) == 0u;
#endif
    // W6/M5: gated on gAverHistoryWrite; blended fragments don't write here.
    if (giDenoiseInWrite) {
        if (gAverHistoryWrite) gGiRadianceOut[pixelPos] = float4(outDiffuse, 0.0);
    }

    // Read last frame's denoised estimate (one frame lag). Zero dimensions means denoiser off.
    uint gw = 0, gh = 0;
    gDenoisedGi.GetDimensions(gw, gh);
    // W6/M5: blended fragments don't read the opaque surface's denoised answer.
    // Read where the surface was last frame, not at this frame's pixel (reprojected, validated).
    const bool denoisedReproject = gGiRestirParams.y > 0.5 &&   // a previous frame exists to reproject into
                                   ((uint)gAmbientParams.z & 64u) == 0u;
    float3 denoisedSum  = 0.0;
    float  denoisedWsum = 0.0;
    // W6/M5: gAverHistoryWrite gates the surface-history lookups.
    if (gAverHistoryWrite && denoisedReproject && gw > 0u && gh > 0u) {
        const float4 dnPrevClip = mul(float4(wpos + gAverReprojDelta, 1.0), gPrevViewProj);
        if (dnPrevClip.w > 1e-4) {
            const float3 dnPrevNdc = dnPrevClip.xyz / dnPrevClip.w;
            const float2 dnPrevPx = gSceneViewport.xy +
                float2(dnPrevNdc.x * 0.5 + 0.5, 0.5 - dnPrevNdc.y * 0.5) * gSceneViewport.zw;
            const float2 dnF    = dnPrevPx - 0.5;   // texel CENTRES sit at +0.5
            const int2   dnBase = int2(floor(dnF));
            const float2 dnFrac = dnF - float2(dnBase);
            const float  dnPlaneTol = max(curLinearDepth, 1.0) * 0.02 + 1.0;   // cm
            [unroll] for (uint k = 0u; k < 4u; ++k) {
                const int2  off = int2(k & 1u, k >> 1u);
                const float w   = (off.x != 0 ? dnFrac.x : 1.0 - dnFrac.x) *
                                  (off.y != 0 ? dnFrac.y : 1.0 - dnFrac.y);
                if (w <= 0.0) continue;
                const int2 tap = dnBase + off;
                const GiSurface prevSurf = giLoadPrevSurface(tap);   // bounds-checked
                if (!prevSurf.valid) continue;
                if (abs(dot(prevSurf.worldPos - (wpos + gAverReprojDelta), N)) > dnPlaneTol) continue;
                if (dot(prevSurf.normal, N) < 0.9) continue;
                denoisedSum  += gDenoisedGi.Load(int3(tap, 0)).rgb * w;
                denoisedWsum += w;
            }
        }
    }
    if (gAverHistoryWrite && gw > 0u && gh > 0u) {
        // Disoccluded: this frame's own estimate, not the old texel (another surface's light).
        float3 denoised;
        if (!denoisedReproject)        denoised = gDenoisedGi.Load(int3(pixelPos, 0)).rgb;   // legacy
        else if (denoisedWsum > 1e-3)  denoised = lerp(outDiffuse, denoisedSum / denoisedWsum, saturate(denoisedWsum * 2.0));
        else                           denoised = outDiffuse;
        // Guard denoiser output: it is not trusted to be finite/bounded.
        giPoisonDenoisedHit = any(isnan(denoised)) || any(isinf(denoised));
        // Below zero: pull toward grey by the minimum amount that brings every channel >= 0.
        const float  luma   = max(averShadowLum(denoised), 0.0);
        const float  lowest = min(denoised.r, min(denoised.g, denoised.b));
        const float3 inGamut = lowest < 0.0
                             ? luma + (denoised - luma) * (luma / max(luma - lowest, 1e-6))
                             : denoised;
        // Ceiling guard: finite readback above AVER_VOX_MAXRAD.
        const float peak = max(inGamut.r, max(inGamut.g, inGamut.b));
        giPoisonDenoisedCeilHit = !giPoisonDenoisedHit && peak > AVER_VOX_MAXRAD;
        outDiffuse = giPoisonDenoisedHit
                   ? float3(0.0, 0.0, 0.0)
                   : inGamut * min(1.0, AVER_VOX_MAXRAD / max(peak, 1e-6));
    }

    // ---- POISON DEBUG VIEW (voxi.giPoisonView / gGiRestirParams.w) ----
    // Replaces indirect diffuse where a guard fired. Eight colours for eight guard types.
    // Non-finite (magenta-blue) catch corruption; ceiling (red, green) catch saturation.
    if (gGiRestirParams.w > 0.5) {
        if (giPoisonStoreHit)     return float3(1.0, 0.0, 1.0);   // MAGENTA: reservoir store guard
        if (nonFiniteCandidate)   return float3(0.0, 1.0, 1.0);   // CYAN: candidate radiance
        if (gGiPoisonPdfHit)      return float3(1.0, 1.0, 0.0);   // YELLOW: target-pdf guard
        if (giPoisonEstHit)       return float3(1.0, 0.5, 0.0);   // ORANGE: final-estimate guard
        if (giPoisonDenoisedHit)     return float3(0.0, 0.0, 1.0);   // BLUE: denoiser-readback guard
        if (giPoisonEstCeilHit)   return float3(1.0, 0.0, 0.0);   // RED: raw estimate ceiling
        if (giPoisonDenoisedCeilHit) return float3(0.0, 1.0, 0.0);   // GREEN: denoised ceiling
    }

#if AVER_NEURAC
    // ---- NeuRaC VISUALISER (gAmbientParams.w bits 8-11; voxi_neurac_io.hlsli's rcDebugColour) ----
    if (rcCacheOn() && rcViewMode() != 0u) return rcDebugColour(wpos, N);
#endif

    // ---- U1's PATH DEBUG VIEW (voxi.giVisPathView, gAmbientParams.w bit 64; 2.10 I) ----
    // Paints F2's path; F3 follows the same except under legacy bit 8.
    if (gGiRestirParams.w <= 0.5 && ((uint)gAmbientParams.w & 64u) != 0u) {
        if (f2Path == 0u) return float3(1.0, 1.0, 0.0);                 // YELLOW: no ray (mode 0 or legacy bit 4)
        if (f2Path == 1u) return float3(0.0, 1.0, 0.0);                 // GREEN: reconstructed (voxel cone)
#if AVER_NEURAC
        if (f2Path == 4u) return float3(1.0, 0.0, 1.0) * (0.25 + 0.75 * gRcLastConf);   // MAGENTA: radiance-cache
#endif
        if (f2Path == 2u) return float3(0.0, 0.0, 1.0);                 // BLUE: half-res reconstruction
        return (halfBound && !tracedPx) ? float3(1.0, 0.0, 0.0)        // RED: half-res fallback, traced
                                        : float3(1.0, 1.0, 1.0);        // WHITE: traced (Full, or Half's phase pixel)
    }

    return outDiffuse;
}
