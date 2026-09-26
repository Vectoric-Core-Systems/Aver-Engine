
// ================= the camera post chain =================

// Constants for every pass in the chain.
cbuffer AverPost : register(b0) {
    float4 gPostTone;    // x exposure (compensation on the adapted value under auto-exposure),
                         // y bloom intensity, z bloom threshold, w bloom knee
    float4 gPostDst;     // xy destination size in texels, zw its reciprocal
    float4 gPostSrc;     // xy source size in texels,      zw its reciprocal
    float4 gPostAdapt;   // x min log2 luminance, y 1/log2 range, z adaption alpha toward a brighter view, w unused
    float4 gPostLimit;   // x exposure min, y exposure max, z histogram low cut, w high cut
    float4 gPostMisc;    // x middle grey, y auto-exposure on, z bloom filter radius, w tonemap mode
    float4 gPostClamp;   // x pre-tonemap radiance ceiling (0 = no clamp), y local exposure shadows
                         // [0,1], z local exposure highlights [0,1] (see PostSettings), w adaption
                         // alpha toward a darker view (PostSettings::exposureSpeedDark)
    // THE DOCKED-VIEWPORT SUB-RECT: xy is this pass's source uv origin, zw its uv size, both already
    // in the post chain's own normalised source space (see FrameConstants.hpp's PostCB::region for
    // the C++-side derivation). (0,0,1,1) identity when no sub-rect applies -- every read below is
    // then gPostRegion.xy + uv*gPostRegion.zw == uv, so this is a no-op on that path.
    float4 gPostRegion;
    // EYE ADAPTATION REALISM (Krawczyk, Myszkowski & Seidel 2005, "Perceptual effects in real-time
    // tone mapping"): x PostSettings::adaptationRealism [0,1] -- 0 is full adaptation, today's
    // behaviour, exactly; 1 is the perceptual model, used by CSExposure for both the
    // luminance-dependent key and the rod-slowed dark-adaptation speed. y kLuminanceToCdm2, the
    // scene-linear-radiance-unit -> cd/m^2 constant derived from LevelSky.hpp's sunIntensity
    // calibration (see FrameConstants.hpp's PostCB::eye), used wherever this cbuffer needs a real
    // luminance rather than an engine-unit one. z PostSettings::nightVision [0,1], the scotopic
    // desaturation's own strength, read only by PSComposite. w PostSettings::meteringCenterWeight
    // [0,1], read only by CSHistogram. Mirrors PostCB::eye, appended here for the same reason
    // gPostRegion was -- a new post scalar gets its own row rather than hunting for spare
    // components once none are left.
    float4 gPostEye;
};

Texture2D<float4>     gPostSceneTex : register(t0);
Texture2D<float4>     gPostBloomTex : register(t1);
ByteAddressBuffer     gPostExpRead  : register(t2);
SamplerState          gPostSamp     : register(s0);

// Buffers, not textures: a typed UAV load of RGBA16F is an optional D3D12 feature.
RWByteAddressBuffer   gPostHist     : register(u0);
RWByteAddressBuffer   gPostExp      : register(u1);
// The local-exposure bilateral grid: u2 is CSLocalGrid's raw (sum, count) tally, u3 is CSLocalBlur's
// blurred read of it. Declared here, next to the other post UAVs, even though PSComposite is the
// only pixel shader that touches u3 -- see that pass for why (no new SRV slot needed).
RWByteAddressBuffer   gPostLocalGrid     : register(u2);
RWByteAddressBuffer   gPostLocalGridBlur : register(u3);

// ---- bloom ---------------------------------------------------------------------------------
// A half-resolution RGBA16F pyramid of graphics passes, each reading one mip and writing the next.

// Karis' firefly weight, applied only on the first downsample.
float3 averBloomKaris(float3 c) { return c / (1.0 + averLuminance(c)); }

// The exposure this frame displays at. Auto-exposure off: the authored value. On: the adapted value
// times the authored one, which then acts as exposure compensation (1 = as metered, 2 = a stop up).
float averPostExposure() {
    return gPostMisc.y > 0.5 ? asfloat(gPostExpRead.Load(0)) * gPostTone.x : gPostTone.x;
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
        // gPostDst is THIS DISPATCH's own downscaled grid (sized off the sub-rect in scene pixels
        // when docked -- see runPostChain), so uvLocal alone is 0..1 over the whole scene only when
        // undocked; gPostRegion maps it into the sub-rect actually being metered either way.
        float2 uvLocal = (tid.xy + 0.5) * gPostDst.zw;
        float2 uv = gPostRegion.xy + uvLocal * gPostRegion.zw;
        float lum = averLuminance(gPostSceneTex.SampleLevel(gPostSamp, uv, 0).rgb);
        uint bin = 0;
        if (lum > 1e-4) {
            float t = saturate((log2(lum) - gPostAdapt.x) * gPostAdapt.y);
            bin = (uint)(t * 254.0 + 1.0);
        }
        // CENTRE-WEIGHTED METERING (Krawczyk et al. sec. 4; the classical centre-weighted camera
        // meter they cite it against): the region's centre counts for more than its edges. `d` is
        // this sample's distance from the region's centre in units of the region's HALF-HEIGHT on
        // BOTH axes -- aspect-corrected, so a widescreen region's horizontal falloff matches its
        // vertical one instead of stretching into an ellipse; gPostDst is THIS dispatch's own grid
        // (the metered region, downscaled -- see the comment above), so x/y here is exactly that
        // region's aspect ratio. meteringCenterWeight 0 collapses w to 1 always, identical to the
        // unweighted count this replaces. The percentile cuts below (CSExposure) already treat
        // gPostHist as counts of arbitrary mass, not one-per-pixel, so a weighted count needs no
        // change there.
        float aspect = gPostDst.x / max(gPostDst.y, 1e-4);
        float2 centred = float2((uvLocal.x - 0.5) * 2.0 * aspect, (uvLocal.y - 0.5) * 2.0);
        float d2 = dot(centred, centred);
        uint w = 1u + (uint)round(3.0 * gPostEye.w * exp(-d2 / (2.0 * 0.3 * 0.3)));
        InterlockedAdd(gHistLocal[bin], w);
    }
    GroupMemoryBarrierWithGroupSync();

    uint local = gHistLocal[gi];
    if (local > 0) gPostHist.InterlockedAdd(gi * 4, local);
}

// LUMINANCE-DEPENDENT KEY (Krawczyk, Myszkowski & Seidel 2005, "Perceptual effects in real-time
// tone mapping", sec. 4): partial light adaptation. alpha(Y) in (0,1) is the fraction of full
// adaptation the eye reaches at luminance Y cd/m^2 -- lower at low Y, which is what keeps a dim
// scene looking dim after the eye adapts to it instead of every view settling to one identical
// average brightness. No division by zero or negative log: Ycdm2 is clamped >= 0, so the
// denominator is always >= 2. Worked values (PostSettings' own comment repeats them, since they
// pin this exact formula): alpha(0.01) = 0.032, alpha(1) = 0.161, alpha(100) = 0.530,
// alpha(2000) = 0.653.
float averEyeAlpha(float Ycdm2) {
    return 1.03 - 2.0 / (2.0 + log10(max(Ycdm2, 0.0) + 1.0));
}

// Reduces the histogram to one exposure value and damps towards it in log space.
[numthreads(1, 1, 1)]
void CSExposure() {
    // 1, NOT THE AUTHORED EXPOSURE: that is applied where the exposure is read (averPostExposure,
    // PSComposite) as compensation on this value, so storing it here too would square it. A frame
    // with nothing metered (all black) therefore still displays at exactly the authored value.
    float target = 1.0;
    // The metered average luminance in cd/m^2, filled in below only inside the weighted-average
    // branch; 0 otherwise (nothing was metered this frame, e.g. an all-black region), which reads
    // as true darkness to both uses further down -- exactly the direction a rod-dominated view
    // should slow into.
    float avgLumCdm2 = 0.0;

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
            avgLumCdm2 = avgLum * gPostEye.y;   // kLuminanceToCdm2 -- LevelSky's lux/(100000/3) calibration
            // keyEff blends the authored middle grey (exposureKey, gPostMisc.x) between "as
            // authored" (adaptationRealism 0, today's full-adaptation behaviour -- keyEff ==
            // exposureKey exactly, the lerp's base case) and the perceptual curve above, normalised
            // so keyEff == exposureKey at the reference Yref = 100 cd/m^2 too (a dim interior;
            // exposureKey was tuned against NewSponza's noon courtyard, which meters ~82 cd/m^2, so
            // the two nearly coincide at full realism there). averEyeAlpha(100.0) is computed, not
            // the 0.530 hardcoded, so it can never drift from the formula above.
            float keyEff = gPostMisc.x * lerp(1.0, averEyeAlpha(avgLumCdm2) / max(averEyeAlpha(100.0), 1e-4), gPostEye.x);
            target = keyEff / max(avgLum, 1e-4);
        }
        target = clamp(target, gPostLimit.x, gPostLimit.y);

        for (uint z = 0; z < 256; ++z) gPostHist.Store(z * 4, 0);
    }

    float prev = asfloat(gPostExp.Load(0));
    uint  seeded = gPostExp.Load(4);
    // TWO SPEEDS, like an eye: a view getting brighter (target below prev) is met at gPostAdapt.z,
    // one getting darker (target above prev) at the slower gPostClamp.w -- PostSettings'
    // exposureSpeed / exposureSpeedDark.
    //
    // ROD-SLOWED DARK ADAPTATION (Krawczyk et al. sec. 4): rods take over below about 1 cd/m^2 and
    // adapt more slowly than cones, so darkening gets up to 4x slower in true darkness. sigmaAvg is
    // the same rod/cone mix fraction PSComposite's scotopic desaturation computes per pixel, here on
    // the metered average instead of one pixel. darkSlow collapses to 1 (no change from before this
    // feature existed) at adaptationRealism 0, same lerp base as keyEff above.
    float sigmaAvg = saturate(0.04 / (0.04 + max(avgLumCdm2, 0.0)));
    float darkSlow = lerp(1.0, lerp(1.0, 0.25, sigmaAvg), gPostEye.x);
    const float alpha = (target > prev) ? gPostClamp.w * darkSlow : gPostAdapt.z;
    float next = (seeded == 0 || prev <= 0.0)
               ? target
               : exp2(lerp(log2(prev), log2(max(target, 1e-4)), saturate(alpha)));
    gPostExp.Store(0, asuint(next));
    gPostExp.Store(4, 1);
}

// ---- local exposure grid --------------------------------------------------------------------
// A bilateral grid of log2 luminance -- a coarse (tile x tile x bin) histogram of the scene,
// blurred so a lookup near a tile or bin edge doesn't jump -- that PSComposite reads to find each
// pixel's LOCAL average log-luminance rather than the single global one CSExposure produced above.
// Letting a dark region brighten (and a bright one dim) independently of the rest of the frame is
// what keeps a sunlit-courtyard shot from crushing its shaded arcade to near-black; a single global
// exposure can only pick one multiplier for the whole image. Runs only when local exposure is on
// (either gPostClamp.y or .z > 0), after CSExposure and before PSComposite, regardless of whether
// auto-exposure itself is on.
#define AVER_LOCALEXP_TILE     32    // scene pixels per grid cell, one axis
#define AVER_LOCALEXP_BINS     16    // log2-luminance bins per cell
#define AVER_LOCALEXP_MAX_DOWN 2.0   // stops a bright region may be pulled down (highlights)
#define AVER_LOCALEXP_MAX_UP   4.0   // stops a dark region may be pulled up (shadows)

// gridW/gridH cells cover the scene at one AVER_LOCALEXP_TILE-pixel cell per tile, rounded up.
uint averLocalGridDim(uint sceneDim) { return (sceneDim + AVER_LOCALEXP_TILE - 1) / AVER_LOCALEXP_TILE; }

// Byte offset of cell (x, y, bin)'s 8-byte (sum, count) pair -- 16 bins * 8 bytes = 128 bytes/cell,
// row-major in x then y.
uint averLocalGridOffset(uint x, uint y, uint bin, uint gridW) {
    return (((y * gridW) + x) * AVER_LOCALEXP_BINS + bin) * 8;
}

// The SAME log2-luminance -> bin mapping CSHistogram uses (its `t`), just quantised to 16 bins
// instead of 254, so a grid cell and the global histogram agree on what "bin" means.
uint averLocalBin(float lum) {
    float t = saturate((log2(lum) - gPostAdapt.x) * gPostAdapt.y);
    return min((uint)(t * (float)AVER_LOCALEXP_BINS), (uint)AVER_LOCALEXP_BINS - 1);
}

// Fixed-point scale for the groupshared log2-luminance accumulator below: InterlockedAdd only
// takes integers, so each pixel's (log2(L) - gPostAdapt.x) -- non-negative for any L inside the
// metered range, which is exactly what gPostAdapt.x is for -- is scaled up, summed as a uint, and
// unscaled (plus gPostAdapt.x times the count, to undo the per-pixel offset) when the tile is
// written out.
#define AVER_LOCALEXP_FIXED_SCALE 1024.0

groupshared uint gLocalSum[AVER_LOCALEXP_BINS];
groupshared uint gLocalCount[AVER_LOCALEXP_BINS];

// THE GRID'S SCENE SIZE IS gPostSrc.xy IN ALL THREE PASSES, NOT GetDimensions(t0). The backends
// size the buffers from the scene target, and every pass that touches the grid is handed that
// size as src -- including the composite, whose t0 on the AverSR path is the PRESENT-sized upscale
// rather than the scene. Deriving gridW from t0 there would stride the lookup by the wrong row
// length and read other tiles' bins.

// Bins one AVER_LOCALEXP_TILE-pixel tile's log2 luminance into 16 (sum, count) pairs and writes
// them to u2. One group per tile (dispatched (gridW, gridH, 1)); each of the 16x16 threads covers
// a 2x2 block of the tile's pixels, 16*2 = 32 pixels per axis.
[numthreads(16, 16, 1)]
void CSLocalGrid(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    if (gi < (uint)AVER_LOCALEXP_BINS) { gLocalSum[gi] = 0; gLocalCount[gi] = 0; }
    GroupMemoryBarrierWithGroupSync();

    uint sceneW = (uint)gPostSrc.x, sceneH = (uint)gPostSrc.y;
    uint2 origin = gid.xy * AVER_LOCALEXP_TILE + tid.xy * 2;

    [unroll]
    for (uint dy = 0; dy < 2; ++dy) {
        [unroll]
        for (uint dx = 0; dx < 2; ++dx) {
            uint2 px = origin + uint2(dx, dy);
            if (px.x >= sceneW || px.y >= sceneH) continue;
            float2 uv = (px + 0.5) * gPostSrc.zw;
            // Clamped exactly as PSComposite clamps before it looks the pixel up, so a sun glint
            // is binned where the composite will search for it.
            float lum = averLuminance(averPostClampRadiance(gPostSceneTex.SampleLevel(gPostSamp, uv, 0).rgb));
            if (lum <= 1e-4) continue;   // black stays black -- excluded, not binned
            float offsetLog = max(log2(lum) - gPostAdapt.x, 0.0);
            uint bin = averLocalBin(lum);
            InterlockedAdd(gLocalSum[bin], (uint)(offsetLog * AVER_LOCALEXP_FIXED_SCALE));
            InterlockedAdd(gLocalCount[bin], 1);
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (gi < (uint)AVER_LOCALEXP_BINS) {
        uint gridW = averLocalGridDim(sceneW);
        uint count = gLocalCount[gi];
        float sumLog = count > 0
            ? (float)gLocalSum[gi] / AVER_LOCALEXP_FIXED_SCALE + gPostAdapt.x * (float)count
            : 0.0;
        uint off = averLocalGridOffset(gid.x, gid.y, gi, gridW);
        gPostLocalGrid.Store2(off, uint2(asuint(sumLog), asuint((float)count)));
    }
}

// Separable (1,2,1) blur of u2 into u3, over x, y AND the bin axis -- smooths the grid so a lookup
// near a tile boundary or a thinly-populated bin doesn't jump. Weights apply to (sum, count)
// together and neighbours clamp to the grid/bin edges rather than being skipped; normalisation
// happens at the read (PSComposite divides sum by count), so the weighted sum written here is
// deliberately left unnormalised.
[numthreads(8, 8, 1)]
void CSLocalBlur(uint3 tid : SV_DispatchThreadID) {
    uint gridW = averLocalGridDim((uint)gPostSrc.x);
    uint gridH = averLocalGridDim((uint)gPostSrc.y);
    if (tid.x >= gridW || tid.y >= gridH) return;

    const float kW[3] = { 1.0, 2.0, 1.0 };

    [unroll]
    for (uint bi = 0; bi < (uint)AVER_LOCALEXP_BINS; ++bi) {
        float binSum = 0.0, binCount = 0.0;
        [unroll]
        for (int db = -1; db <= 1; ++db) {
            uint nb = (uint)clamp((int)bi + db, 0, AVER_LOCALEXP_BINS - 1);
            float wb = kW[db + 1];
            [unroll]
            for (int dy = -1; dy <= 1; ++dy) {
                uint ny = (uint)clamp((int)tid.y + dy, 0, (int)gridH - 1);
                float wy = kW[dy + 1];
                [unroll]
                for (int dx = -1; dx <= 1; ++dx) {
                    uint nx = (uint)clamp((int)tid.x + dx, 0, (int)gridW - 1);
                    float wx = kW[dx + 1];
                    uint off = averLocalGridOffset(nx, ny, nb, gridW);
                    uint2 v = gPostLocalGrid.Load2(off);
                    float wgt = wx * wy * wb;
                    binSum += asfloat(v.x) * wgt;
                    binCount += asfloat(v.y) * wgt;
                }
            }
        }
        uint outOff = averLocalGridOffset(tid.x, tid.y, bi, gridW);
        gPostLocalGridBlur.Store2(outOff, uint2(asuint(binSum), asuint(binCount)));
    }
}

// Trilinear read of u3 over (x, y, bin), the local-exposure analogue of a texture's bilinear
// filter. Cell (x, y, bin) samples are centred at (x+0.5, y+0.5, bin+0.5) in grid space -- the
// usual texel-centre convention -- and neighbours clamp to the grid/bin edges. `tileXY` and `bin`
// are UNSHIFTED continuous grid coordinates (scene pixel / AVER_LOCALEXP_TILE, and t * BINS); the
// centring subtraction happens in here.
void averLocalGridSample(float2 tileXY, float bin, uint gridW, uint gridH, out float sum, out float count) {
    float3 p = float3(tileXY.x - 0.5, tileXY.y - 0.5, bin - 0.5);
    int3 p0 = (int3)floor(p);
    float3 f = p - p0;
    sum = 0.0; count = 0.0;

    [unroll]
    for (int dz = 0; dz <= 1; ++dz) {
        uint bz = (uint)clamp(p0.z + dz, 0, AVER_LOCALEXP_BINS - 1);
        float wz = dz == 0 ? (1.0 - f.z) : f.z;
        [unroll]
        for (int dy = 0; dy <= 1; ++dy) {
            uint by = (uint)clamp(p0.y + dy, 0, (int)gridH - 1);
            float wy = dy == 0 ? (1.0 - f.y) : f.y;
            [unroll]
            for (int dx = 0; dx <= 1; ++dx) {
                uint bx = (uint)clamp(p0.x + dx, 0, (int)gridW - 1);
                float wx = dx == 0 ? (1.0 - f.x) : f.x;
                uint off = averLocalGridOffset(bx, by, bz, gridW);
                uint2 v = gPostLocalGridBlur.Load2(off);
                float wgt = wx * wy * wz;
                sum += asfloat(v.x) * wgt;
                count += asfloat(v.y) * wgt;
            }
        }
    }
}

// ---- composite -----------------------------------------------------------------------------

// SCOTOPIC NIGHT VISION (Krawczyk, Myszkowski & Seidel 2005, "Perceptual effects in real-time tone
// mapping", sec. 5, after Kim et al.): below about 1 cd/m^2 rods take over from cones -- vision
// loses the photopigments that see colour at all, and blue-shifts (the Purkinje effect) into what
// rods respond to. Runs on SCENE-LINEAR radiance: rods react to the scene's own brightness, not to
// a display value exposure has not produced yet.
float3 averNightVision(float3 rgb) {
    // CIE XYZ of the linear-sRGB pixel, Rec.709 primaries / D65 white -- the same primaries
    // averLuminance's Y row already assumes.
    float X = dot(rgb, float3(0.4124564, 0.3575761, 0.1804375));
    float Y = max(dot(rgb, float3(0.2126729, 0.7151522, 0.0721750)), 0.0);
    float Z = dot(rgb, float3(0.0193339, 0.1191920, 0.9503041));
    // SCALE-FREE GUARD. (Y + Z) / X is a chromaticity ratio, the same for a pixel at any brightness,
    // so the only X to guard is exactly zero -- and with non-negative radiance that is only black,
    // whose V is 0 anyway. An absolute floor (1e-4) would be wrong here: one engine radiance unit is
    // kLuminanceToCdm2 = 33,333 cd/m^2, so every pixel this ever affects (below ~1 cd/m^2) sits near
    // 3e-5 and a floor that size bends the ratio until V goes negative and the pixel goes black.
    //
    // NORMALISED TO WHITE: the model's V is ~2.573 x Y for a D65-neutral surface ((Y+Z)/X = 2.198),
    // and handing that back unscaled would make every dark region 2.6x BRIGHTER than it metered.
    // Divided out, a grey surface keeps its brightness and only the COLOUR changes -- reds (which
    // rods barely see) darken and blues lift, the Purkinje shift. The tint's own luminance is ~1.009.
    const float kScotopicWhite = 2.573;
    float V = (X > 0.0) ? max(Y * (1.33 * (1.0 + (Y + Z) / X) - 1.68), 0.0) / kScotopicWhite : 0.0;
    float Ycd = Y * gPostEye.y;   // kLuminanceToCdm2 -- LevelSky's lux/(100000/3) calibration
    float sigma = saturate(0.04 / (0.04 + Ycd) * gPostEye.z);
    return lerp(rgb, V * float3(1.05, 0.97, 1.27), sigma);
}

// Exposure, bloom, tonemap and gamma in one pass; the frame becomes a display image here.
float4 PSComposite(AverPostVSOut i) : SV_TARGET {
    // i.uv is 0..1 over WHATEVER the raster viewport this pass drew into was set to -- the whole
    // present canvas when undocked, or just the docked sub-rect when runPostChain confined the
    // composite's own viewport/scissor to it (see its own comment). Either way it is NOT yet a
    // valid uv into the source texture: gPostRegion maps it there. Computed once and reused for
    // every source read below -- the scene sample, the bloom sample and the local-exposure lookup --
    // so all three agree on which part of the (possibly larger) source textures this pixel belongs to.
    float2 srcUv = gPostRegion.xy + i.uv * gPostRegion.zw;
    float3 cRaw = averPostClampRadiance(gPostSceneTex.SampleLevel(gPostSamp, srcUv, 0).rgb);
    // NIGHT VISION HERE, NOT AFTER: cRaw is SCENE-LINEAR radiance, still BEFORE exposure and before
    // local exposure (both below) -- exactly the scotopic model's input. It applies unconditionally,
    // auto-exposure on or off (gPostEye.z alone gates it, via averNightVision's own sigma), unlike
    // the key and dark-adaptation-speed changes above, which only take effect where CSExposure runs.
    cRaw = averNightVision(cRaw);
#ifdef AVER_POST_AUTOEXPOSURE
    // The adapted exposure, with the authored one as COMPENSATION applied after local exposure.
    // Kept apart because local exposure decides what is dark against the metered exposure alone:
    // folded in, it would pull shadows back toward middle grey and undo half of every stop the
    // compensation adds (at the default shadow strength 0.5).
    float currentExposure = asfloat(gPostExpRead.Load(0));
    float compensation = gPostTone.x;
#else
    float currentExposure = gPostTone.x;
    float compensation = 1.0;
#endif
    float3 c = cRaw * currentExposure;

    // ---- local exposure ----
    // Brightens this pixel's region if it sits below middle grey, tames it if above, independent
    // of the single global `currentExposure` just applied -- see PostSettings::localExposure-
    // Shadows/Highlights (gPostClamp.y/z) and the grid built by CSLocalGrid/CSLocalBlur above.
    // Uniform branch (gPostClamp is per-frame), so this is free when both are 0.
    if (gPostClamp.y > 0.0 || gPostClamp.z > 0.0) {
        float lPre = averLuminance(cRaw);   // BEFORE exposure -- the grid was built from the same
        if (lPre > 1e-4) {
            uint gridW = averLocalGridDim((uint)gPostSrc.x);
            uint gridH = averLocalGridDim((uint)gPostSrc.y);

            // srcUv, NOT i.uv: the grid was built (CSLocalGrid) over the WHOLE scene, unconfined by
            // any sub-rect -- see runPostChain's comment on why that pass is left alone -- so the
            // lookup into it needs the same source-space uv the scene sample above just used.
            float2 tileXY = srcUv * gPostSrc.xy / (float)AVER_LOCALEXP_TILE;
            float t = saturate((log2(lPre) - gPostAdapt.x) * gPostAdapt.y);
            float binCoord = t * (float)AVER_LOCALEXP_BINS;

            float sum, count;
            averLocalGridSample(tileXY, binCoord, gridW, gridH, sum, count);
            if (count > 1e-4) {
                float localLog = sum / max(count, 1e-4);
                float exposedLocal = localLog + log2(currentExposure);
                float d = log2(gPostMisc.x) - exposedLocal;
                float stops = d > 0.0 ? d * gPostClamp.y : d * gPostClamp.z;
                stops = clamp(stops, -AVER_LOCALEXP_MAX_DOWN, AVER_LOCALEXP_MAX_UP);
                c *= exp2(stops);
            }
        }
    }
    c *= compensation;

#ifdef AVER_POST_BLOOM
    // srcUv again: the bloom pyramid, like the local-exposure grid, is built over the whole scene.
    c += gPostBloomTex.SampleLevel(gPostSamp, srcUv, 0).rgb * gPostTone.y;
#endif
    return float4(toGamma(averTonemap(c, gPostMisc.w)), 1.0);
}
