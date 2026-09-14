
// ================= the camera post chain =================

// Constants for every pass in the chain.
cbuffer AverPost : register(b0) {
    float4 gPostTone;    // x exposure, y bloom intensity, z bloom threshold, w bloom knee
    float4 gPostDst;     // xy destination size in texels, zw its reciprocal
    float4 gPostSrc;     // xy source size in texels,      zw its reciprocal
    float4 gPostAdapt;   // x min log2 luminance, y 1/log2 range, z adaption alpha, w pixels sampled
    float4 gPostLimit;   // x exposure min, y exposure max, z histogram low cut, w high cut
    float4 gPostMisc;    // x middle grey, y auto-exposure on, z bloom filter radius, w tonemap mode
    float4 gPostClamp;   // x pre-tonemap radiance ceiling (0 = no clamp), yzw spare
};

Texture2D<float4>     gPostSceneTex : register(t0);
Texture2D<float4>     gPostBloomTex : register(t1);
ByteAddressBuffer     gPostExpRead  : register(t2);
SamplerState          gPostSamp     : register(s0);

// Buffers, not textures: a typed UAV load of RGBA16F is an optional D3D12 feature.
RWByteAddressBuffer   gPostHist     : register(u0);
RWByteAddressBuffer   gPostExp      : register(u1);

// ---- bloom ---------------------------------------------------------------------------------
// A half-resolution RGBA16F pyramid of graphics passes, each reading one mip and writing the next.

// Karis' firefly weight, applied only on the first downsample.
float3 averBloomKaris(float3 c) { return c / (1.0 + averLuminance(c)); }

// The exposure this frame settled on; the authored value when auto-exposure is off.
float averPostExposure() {
    return gPostMisc.y > 0.5 ? asfloat(gPostExpRead.Load(0)) : gPostTone.x;
}

// Soft-knee threshold: the quadratic ramp that decides how much of a pixel bloom takes.
float3 averBloomPrefilter(float3 c) {
    float br = max(c.r, max(c.g, c.b));
    float knee = max(gPostTone.w, 1e-4);
    float soft = clamp(br - gPostTone.z + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float weight = max(soft, br - gPostTone.z) / max(br, 1e-4);
    return c * weight;
}

// Interpolants every post pixel shader reads.
struct AverPostVSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// Emits one vertex of the fullscreen post triangle from SV_VertexID.
AverPostVSOut PostVS(uint id : SV_VertexID) {
    AverPostVSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// Halves the scene into the pyramid's first level, Karis-averaged and soft-knee thresholded.
// THE CEILING, APPLIED WHERE THE SCENE IS READ -- before exposure, and before the bloom threshold.
//
// PLACEMENT IS THE WHOLE FIX AND THE FIRST ATTEMPT GOT IT WRONG. Clamping in PSComposite after bloom
// had been added does nothing useful: past about 8 the tonemap already returns pure white, so the sun
// disc looks identical clamped or not. What the unclamped value actually does is drive the BLOOM --
// the prefilter reads the same scene texture, thresholds at 1.0, and a disc sitting at ~42 (the sky
// dome draws sunColour * sunIntensity * 14, and sunIntensity defaults to 3) bleeds a halo
// proportional to all 42 of it. That halo is what reads as the void being overwhelmingly bright.
//
// 0 DISABLES IT rather than meaning "clamp to nothing".
float3 averPostClampRadiance(float3 c) {
    return gPostClamp.x > 0.0 ? min(c, gPostClamp.x) : c;
}

float4 PSBloomPrefilter(AverPostVSOut i) : SV_TARGET {
    float2 o = gPostSrc.zw;
    float3 a = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2(-o.x, -o.y), 0).rgb;
    float3 b = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2( o.x, -o.y), 0).rgb;
    float3 c = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2(-o.x,  o.y), 0).rgb;
    float3 d = gPostSceneTex.SampleLevel(gPostSamp, i.uv + float2( o.x,  o.y), 0).rgb;
    a = averPostClampRadiance(a); b = averPostClampRadiance(b);
    c = averPostClampRadiance(c); d = averPostClampRadiance(d);
    float3 sum = averBloomKaris(a) + averBloomKaris(b) + averBloomKaris(c) + averBloomKaris(d);
    return float4(averBloomPrefilter(sum * 0.25 * averPostExposure()), 1.0);
}

// Jimenez' 13-tap downsample.
float4 PSBloomDown(AverPostVSOut i) : SV_TARGET {
    float2 uv = i.uv;
    float2 o = gPostSrc.zw;

    float3 a = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-2*o.x,  2*o.y), 0).rgb;
    float3 b = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(     0,  2*o.y), 0).rgb;
    float3 c = gPostSceneTex.SampleLevel(gPostSamp, uv + float2( 2*o.x,  2*o.y), 0).rgb;
    float3 d = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-2*o.x,      0), 0).rgb;
    float3 e = gPostSceneTex.SampleLevel(gPostSamp, uv,                          0).rgb;
    float3 f = gPostSceneTex.SampleLevel(gPostSamp, uv + float2( 2*o.x,      0), 0).rgb;
    float3 g = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-2*o.x, -2*o.y), 0).rgb;
    float3 h = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(     0, -2*o.y), 0).rgb;
    float3 j = gPostSceneTex.SampleLevel(gPostSamp, uv + float2( 2*o.x, -2*o.y), 0).rgb;
    float3 k = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(  -o.x,    o.y), 0).rgb;
    float3 l = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(   o.x,    o.y), 0).rgb;
    float3 m = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(  -o.x,   -o.y), 0).rgb;
    float3 n = gPostSceneTex.SampleLevel(gPostSamp, uv + float2(   o.x,   -o.y), 0).rgb;

    float3 sum = e * 0.125;
    sum += (a + c + g + j) * 0.03125;
    sum += (b + d + f + h) * 0.0625;
    sum += (k + l + m + n) * 0.125;
    return float4(sum, 1.0);
}

// 9-tap tent upsample, added into the level above by the blender.
float4 PSBloomUp(AverPostVSOut i) : SV_TARGET {
    float2 o = gPostMisc.z * gPostDst.zw;
    float2 uv = i.uv;

    float3 sum = gPostSceneTex.SampleLevel(gPostSamp, uv, 0).rgb * 0.25;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-o.x,  0.0), 0).rgb * 0.125;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2( o.x,  0.0), 0).rgb * 0.125;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2( 0.0, -o.y), 0).rgb * 0.125;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2( 0.0,  o.y), 0).rgb * 0.125;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-o.x, -o.y), 0).rgb * 0.0625;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2( o.x, -o.y), 0).rgb * 0.0625;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2(-o.x,  o.y), 0).rgb * 0.0625;
    sum += gPostSceneTex.SampleLevel(gPostSamp, uv + float2( o.x,  o.y), 0).rgb * 0.0625;
    return float4(sum, 1.0);
}

// ---- eye adaptation ------------------------------------------------------------------------
// A 256-bin histogram of log2 luminance, reduced to one exposure value damped over time.
groupshared uint gHistLocal[256];

// Bins the scene's log2 luminance into the shared 256-bin histogram.
[numthreads(16, 16, 1)]
void CSHistogram(uint3 tid : SV_DispatchThreadID, uint gi : SV_GroupIndex) {
    gHistLocal[gi] = 0;
    GroupMemoryBarrierWithGroupSync();

    if (tid.x < (uint)gPostDst.x && tid.y < (uint)gPostDst.y) {
        float2 uv = (tid.xy + 0.5) * gPostDst.zw;
        float lum = averLuminance(gPostSceneTex.SampleLevel(gPostSamp, uv, 0).rgb);
        uint bin = 0;
        if (lum > 1e-4) {
            float t = saturate((log2(lum) - gPostAdapt.x) * gPostAdapt.y);
            bin = (uint)(t * 254.0 + 1.0);
        }
        InterlockedAdd(gHistLocal[bin], 1);
    }
    GroupMemoryBarrierWithGroupSync();

    uint local = gHistLocal[gi];
    if (local > 0) gPostHist.InterlockedAdd(gi * 4, local);
}

// Reduces the histogram to one exposure value and damps towards it in log space.
[numthreads(1, 1, 1)]
void CSExposure() {
    float target = gPostTone.x;

    if (gPostMisc.y > 0.5) {
        // BIN 0 IS READ AND DELIBERATELY NOT COUNTED, and getting that wrong disabled half of this
        // function. CSHistogram puts every pixel with luminance <= 1e-4 in bin 0 -- "black", not a
        // measured luminance -- and the averaging loop below starts at b = 1 to exclude it. `total`
        // used to sum ALL 256 bins anyway, so the two percentile cuts were fractions of a
        // population the loop never walks.
        //
        // WHAT THAT COST: `seen` only ever accumulates bins 1..255, so it can only reach highCut if
        // the lit pixels alone exceed histogramHighPercent of the WHOLE frame. In an enclosed scene
        // -- the arcade this engine is developed against, where bin 0 routinely holds a third of the
        // frame -- it never does, and `hi` is zero for every bin. The high cut simply never fires.
        // That cut is the only thing standing between a handful of blown-out pixels and the
        // metering, which is exactly the guard a ray-traced GI estimator with fireflies needs most.
        // The low cut misfires the same way in reverse: it discards `lowCut` genuinely-lit pixels
        // as "the darkest", when the real darkest are all sitting in bin 0, unexamined.
        //
        // Counting the lit population only makes both cuts mean what histogramLowPercent and
        // histogramHighPercent say they mean: percentiles of the pixels actually being averaged.
        uint total = 0;
        uint counts[256];
        counts[0] = gPostHist.Load(0);
        for (uint i = 1; i < 256; ++i) { counts[i] = gPostHist.Load(i * 4); total += counts[i]; }

        float lowCut  = total * gPostLimit.z;
        float highCut = total * gPostLimit.w;
        float seen = 0.0, weighted = 0.0, weight = 0.0;
        for (uint b = 1; b < 256; ++b) {
            float c = counts[b];
            float lo = max(lowCut - seen, 0.0);
            float hi = max(seen + c - highCut, 0.0);
            float take = max(c - lo - hi, 0.0);
            weighted += take * (float)b;
            weight += take;
            seen += c;
        }

        if (weight > 0.0) {
            float avgBin = weighted / weight;
            float avgLum = exp2((avgBin - 1.0) / 254.0 / gPostAdapt.y + gPostAdapt.x);
            target = gPostMisc.x / max(avgLum, 1e-4);
        }
        target = clamp(target, gPostLimit.x, gPostLimit.y);

        for (uint z = 0; z < 256; ++z) gPostHist.Store(z * 4, 0);
    }

    float prev = asfloat(gPostExp.Load(0));
    uint  seeded = gPostExp.Load(4);
    float next = (seeded == 0 || prev <= 0.0)
               ? target
               : exp2(lerp(log2(prev), log2(max(target, 1e-4)), saturate(gPostAdapt.z)));
    gPostExp.Store(0, asuint(next));
    gPostExp.Store(4, 1);
}

// ---- composite -----------------------------------------------------------------------------
// Exposure, bloom, tonemap and gamma in one pass; the frame becomes a display image here.
float4 PSComposite(AverPostVSOut i) : SV_TARGET {
    float3 c = averPostClampRadiance(gPostSceneTex.SampleLevel(gPostSamp, i.uv, 0).rgb);
#ifdef AVER_POST_AUTOEXPOSURE
    c *= asfloat(gPostExpRead.Load(0));
#else
    c *= gPostTone.x;
#endif
#ifdef AVER_POST_BLOOM
    c += gPostBloomTex.SampleLevel(gPostSamp, i.uv, 0).rgb * gPostTone.y;
#endif
    return float4(toGamma(averTonemap(c, gPostMisc.w)), 1.0);
}
