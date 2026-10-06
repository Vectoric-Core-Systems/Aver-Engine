// NRD2 resolve maths (docs/rendering/NRD2.md, phase 1). Shared by nrd2.hlsl's resolve and, later, the
// oracle fit: one copy of how per-tile parameters turn into a pixel's estimate.
//
// Portable: fp32, no wave intrinsics, no atomics. exp/exp2/log2 only with clamped arguments.
// Patent rules (NEURAA_NRD.md section 7): the per-tile parameters are the only learned input (rule 6);
// every per-pixel weight below is fixed maths; candidates come from this frame's pyramid (rule 5);
// fixed bilinear taps (rule 8).
#ifndef NRD2_RESOLVE_HLSLI
#define NRD2_RESOLVE_HLSLI

// Per 8x8 tile, per signal (D = 0, S = 1): three level logits and three log2 edge sensitivities.
// Buffer layout: plane-major, value = buf[plane * tileCount + tile], plane = signal * 6 + field.
#define NRD2_PLANES          12
#define NRD2_FIELD_LOGIT1    0   // 1/2 level; the own pixel's logit is pinned at 0
#define NRD2_FIELD_LOGIT2    1   // 1/4
#define NRD2_FIELD_LOGIT3    2   // 1/8
#define NRD2_FIELD_DEPTH     3   // log2 relative-depth sensitivity
#define NRD2_FIELD_NORMAL    4   // log2 normal-cosine power
#define NRD2_FIELD_LUM       5   // log2 luminance (log-ratio) sensitivity
#define NRD2_FEATURES        12  // CSNrd2Features channels (network input)

struct Nrd2TileParams {
    float logit[3];
    float log2Depth, log2Normal, log2Lum;
};

// Clamped, and a non-finite value falls back to the default (a bad network tile cannot blow up a pixel).
float nrd2SaneParam(float v, float def, float lo, float hi) { return (v == v && abs(v) < 1.0e6) ? clamp(v, lo, hi) : def; }

Nrd2TileParams nrd2SanitiseParams(float v[6], float d[6]) {
    Nrd2TileParams p;
    [unroll] for (uint k = 0u; k < 3u; ++k) p.logit[k] = nrd2SaneParam(v[k], d[k], -16.0, 16.0);
    p.log2Depth  = nrd2SaneParam(v[3], d[3], -8.0, 8.0);
    p.log2Normal = nrd2SaneParam(v[4], d[4], -8.0, 8.0);
    p.log2Lum    = nrd2SaneParam(v[5], d[5], -8.0, 8.0);
    return p;
}

float nrd2Lum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// The decode half of voxi.hlsl's averPackNormalRoughness (also aver_denoise.hlsl's dnsrDecodeNormal).
float3 nrd2DecodeNormal(float4 e) {
    const float2 f = e.xy * 2.0 - 1.0;
    float3 n = float3(f, 1.0 - abs(f.x) - abs(f.y));
    if (n.z < 0.0) n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    return normalize(n);
}

// One pyramid level's four fixed bilinear taps at local pixel q, loaded once (the oracle evaluates many
// parameter sets on the same taps). guide: xyz averaged normal, w view Z in metres (0 = empty).
// value: rgb, a validity fraction.
// off: the depth (m) this pixel's surface plane predicts at the tap's centre, relative to the pixel, from the
// local depth slope zGrad (m per pixel). A tilted plane then keeps its coarse neighbours instead of
// rejecting them as edges, which would leave flat 8x8 blocks.
struct Nrd2Taps {
    float4 g[4];
    float4 x[4];
    float  b[4];
    float  off[4];
};

// Local depth slope (m per pixel) from the 4-neighbour view depths (m, <= 0 = none): per axis the smaller
// one-sided difference, so a silhouette does not tilt the plane.
float2 nrd2DepthSlope(float zm, float zl, float zr, float zd, float zu) {
    const float gl = zl > 0.0 ? zm - zl : 1.0e30, gr = zr > 0.0 ? zr - zm : 1.0e30;
    const float gd = zd > 0.0 ? zm - zd : 1.0e30, gu = zu > 0.0 ? zu - zm : 1.0e30;
    const float gx = abs(gl) < abs(gr) ? gl : gr, gy = abs(gd) < abs(gu) ? gd : gu;
    return float2(abs(gx) < 1.0e29 ? gx : 0.0, abs(gy) < 1.0e29 ? gy : 0.0);
}

Nrd2Taps nrd2LoadTaps(Texture2D<float4> guide, Texture2D<float4> value, uint shift, uint2 q, uint2 lvlSize,
                      float2 zGrad) {
    Nrd2Taps t;
    const float2 pos  = (float2(q) + 0.5) / float(1u << shift) - 0.5;
    const int2   base = int2(floor(pos));
    const float2 f    = pos - float2(base);
    [unroll] for (uint i = 0u; i < 4u; ++i) {
        const int2 o = int2(i & 1u, i >> 1);
        const int2 c = clamp(base + o, int2(0, 0), int2(lvlSize) - 1);
        t.g[i] = guide.Load(int3(c, 0));
        t.x[i] = value.Load(int3(c, 0));
        t.b[i] = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y);
        t.off[i] = dot(zGrad, (float2(base + o) + 0.5) * float(1u << shift) - (float2(q) + 0.5));
    }
    return t;
}

// One tap's weight: kept by depth and normal agreement with this pixel and by the texel's validity.
// dz and cosN come back for the backward pass (0 weight: both unused).
float nrd2TapWeight(float4 g, float4 x, float b, float zm, float off, float3 n, float depthSens, float normalPow,
                    out float dz, out float cosN) {
    dz = 0.0; cosN = 0.0;
    const float nl = length(g.xyz);
    if (g.w <= 0.0 || x.a <= 0.0 || nl < 1.0e-3) return 0.0;
    dz = min(abs(g.w - (zm + clamp(off, -0.5 * zm, 0.5 * zm))) / max(zm, 1.0e-4), 64.0);
    cosN = saturate(dot(n, g.xyz / nl));
    const float wd = exp2(-depthSens * dz);
    const float wn = pow(cosN, normalPow);
    return b * wd * wn * saturate(x.a);
}

// The level's estimate at this pixel; `conf` is the share of the bilinear weight that survived
// (0 = nothing usable).
float3 nrd2UpsampleTaps(Nrd2Taps t, float zm, float3 n, float depthSens, float normalPow, out float conf) {
    float3 sum  = 0.0;
    float  wsum = 0.0;
    [unroll] for (uint i = 0u; i < 4u; ++i) {
        float dz, cosN;
        const float w = nrd2TapWeight(t.g[i], t.x[i], t.b[i], zm, t.off[i], n, depthSens, normalPow, dz, cosN);
        if (w <= 0.0) continue;
        sum  += w * t.x[i].rgb;
        wsum += w;
    }
    conf = wsum;
    return wsum > 1.0e-6 ? sum / wsum : 0.0;
}

float3 nrd2Upsample(Texture2D<float4> guide, Texture2D<float4> value, uint shift, uint2 q, uint2 lvlSize,
                    float2 zGrad, float zm, float3 n, float depthSens, float normalPow, out float conf) {
    return nrd2UpsampleTaps(nrd2LoadTaps(guide, value, shift, q, lvlSize, zGrad), zm, n, depthSens, normalPow, conf);
}

// The pixel's estimate from its own value and the three upsampled levels. Weight of candidate k:
// exp(logit_k + extra_k) * conf_k * luminance term; the own pixel's logit is 0. The luminance term
// compares each candidate's log luminance with the coarsest usable level's, so an outlier gives way to
// the levels around it (spread, not rejected: the levels kept it in their means).
float3 nrd2Combine(float3 c0, bool ownValid, float3 c1, float3 c2, float3 c3, float conf1, float conf2,
                   float conf3, Nrd2TileParams p, float3 extraLogit) {
    const float l0 = nrd2Lum(c0), l1 = nrd2Lum(c1), l2 = nrd2Lum(c2), l3 = nrd2Lum(c3);
    const float ref = conf3 > 0.05 ? l3 : conf2 > 0.05 ? l2 : conf1 > 0.05 ? l1 : l0;
    const float eps = max(ref, 0.0) * 1.0e-3 + 1.0e-7;
    const float sL  = exp2(p.log2Lum);
    const float lr  = log2(max(ref, 0.0) + eps);
    // exp2(-sL * |log2 ratio|), the ratio clamped to 2^16 either way.
    #define NRD2_LUMW(l) exp2(-sL * min(abs(log2(max(l, 0.0) + eps) - lr), 16.0))
    float  w0 = ownValid ? NRD2_LUMW(l0) : 0.0;
    float  w1 = exp(clamp(p.logit[0] + extraLogit.x, -16.0, 16.0)) * saturate(conf1) * NRD2_LUMW(l1);
    float  w2 = exp(clamp(p.logit[1] + extraLogit.y, -16.0, 16.0)) * saturate(conf2) * NRD2_LUMW(l2);
    float  w3 = exp(clamp(p.logit[2] + extraLogit.z, -16.0, 16.0)) * saturate(conf3) * NRD2_LUMW(l3);
    #undef NRD2_LUMW
    const float wsum = w0 + w1 + w2 + w3;
    if (!(wsum > 1.0e-8)) return ownValid ? c0 : 0.0;
    return (w0 * c0 + w1 * c1 + w2 * c2 + w3 * c3) / wsum;
}

// Fixed per-pixel specular terms (not parameters): a smooth lobe keeps its own pixel (a mirror's
// neighbours reflect other things), and so does a short hit distance (contact detail).
static const float kNrd2SpecRoughFall = 3.0;   // per level, at roughness 0
static const float kNrd2SpecContact   = 2.0;
float3 nrd2SpecularExtraLogit(float rough, float hitT, float viewZ) {
    const float g = saturate((rough - 0.05) / 0.3);
    const float contact = hitT > 0.0 ? 1.0 - saturate(hitT / max(0.3 * viewZ, 1.0e-3)) : 0.0;
    return -kNrd2SpecRoughFall * (1.0 - g) * float3(1.0, 2.0, 3.0) - kNrd2SpecContact * contact;
}

// ---- temporal stage (docs/rendering/NRD2.md "Temporal stabiliser"): maths shared with the CPU twin --------
// Runs on jitter-free frames only, on the resolve's D' and S' (irradiance), after the spatial filter. The design
// is AMD FidelityFX's reflections denoiser (reproject, prefilter, temporal resolve; MIT), rewritten: min/max
// order statistics instead of mean +- sigma, luminance relative to the tile, surface-motion reprojection only.
// History length (the sample count n) comes from motion and disocclusion; it weights blends and the prefilter's
// noise estimate, never a radius or a level (NEURAA_NRD.md section 7, rules 1, 5, 7).
static const float kNrd2StabSpeedStill = 0.25;   // px per frame: full history length at or below
static const float kNrd2StabSpeedFast  = 8.0;    // log-space ramp down to nFast here
static const float kNrd2StabSpeedCut   = 128.0;  // no history from here (a whip)
static const float kNrd2StabSpecMin    = 8.0;    // glossy specular still keeps this many frames
static const float kNrd2StabSpecRough  = 100.0;  // n_S = max(8, n * (1 - exp(-100 roughness)))
static const float kNrd2StabDepthRel   = 0.02;   // reprojected-depth acceptance: relative + 1 cm + local plane slope
static const float kNrd2StabDepthAbs   = 0.01;
static const float kNrd2StabNormalCos  = 0.9;    // a history texel's normal must agree this much
static const float kNrd2StabNormalK    = 1.4;    // search score: exp(-K (1 - cos)) * exp(-Z |dz| / z)
static const float kNrd2StabDepthK     = 30.0;
static const float kNrd2StabMinWeight  = 0.5;    // valid bilinear weight needed for any history
static const float kNrd2StabAgeWeight  = 0.1;    // taps lighter than this do not set the history's count
static const float kNrd2AnchorK        = 0.3;    // anchor weight exp(-K lum / tile scale)
static const float kNrd2AnchorMin      = 0.01;
static const float kNrd2AnchorMix      = 0.3;    // share of the reprojected history in the value the anchor sees
static const float kNrd2PfNormalPow    = 512.0;  // prefilter: cos^512
static const float kNrd2PfDepthK       = 30.0;   // exp(-K |dz| / z), plane-predicted
static const float kNrd2PfRadBias      = 0.6;    // exp(-(bias + var * VarK) * |anchor - value| / scale)
static const float kNrd2PfRadVarK      = 0.1;
static const float kNrd2PfRadMin       = 0.01;
static const float kNrd2PfVarK         = 4.4;    // neighbour gain max(VarMin, 1 - exp(-K var))
static const float kNrd2PfVarMin       = 0.1;
static const float kNrd2PfMirror       = 0.1;    // smoother specular is not prefiltered

float3 nrd2StabSane(float3 v) { return all(v == v) ? clamp(v, -6.0e4, 6.0e4) : 0.0; }

// Frames of history allowed at this screen speed (px per frame): nStill at rest, nFast from 8 px, none from 128 px.
float nrd2StabMaxFrames(float speed, float nStill, float nFast) {
    if (!(speed < kNrd2StabSpeedCut)) return 0.0;
    const float t = saturate(log2(max(speed, kNrd2StabSpeedStill) / kNrd2StabSpeedStill) /
                             log2(kNrd2StabSpeedFast / kNrd2StabSpeedStill));
    const float hi = max(nStill, 1.0);
    return exp2(lerp(log2(hi), log2(clamp(nFast, 1.0, hi)), t));
}

// Specular's cap: smooth lobes keep the floor, rough ones the full count (never above the diffuse cap).
float nrd2StabSpecMaxFrames(float nMax, float roughness) {
    if (!(nMax > 0.0)) return 0.0;
    return min(nMax, max(kNrd2StabSpecMin, nMax * (1.0 - exp(-kNrd2StabSpecRough * max(roughness, 0.0)))));
}

// History count after this frame: the accepted history's count + 1, capped; 1 (no history) otherwise.
float nrd2StabSamples(float prevCount, float nMax) {
    if (!(prevCount > 0.0) || !(nMax > 0.0)) return 1.0;
    return max(min(nMax, prevCount + 1.0), 1.0);
}

// Weight of the history after n samples: 0 at n = 1, 1/2, 2/3 ... 1 - 1/n.
float nrd2StabBlend(float n) { return 1.0 - 1.0 / max(n, 1.0); }

// Firefly clamp: a pixel brighter than kNrd2StabFirefly times the brightest of its 8 neighbours is scaled
// down to that (an order statistic, never a mean). Isolated glints go; highlights wider than a pixel stay.
static const float kNrd2StabFirefly = 2.0;
float3 nrd2StabFirefly(float3 c, float neighbourMaxLum) {
    const float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    const float cap = kNrd2StabFirefly * neighbourMaxLum;
    return (l > cap && l > 0.0) ? c * (cap / l) : c;
}

// Input despeckle (CSNrd2Despeckle, before the pyramid): a pixel brighter than `cap` (gNrd2Stab.z, Settings::nrd2DespeckleCap)
// times the 5th brightest of its 24 neighbours (5x5) is scaled down to that. An order statistic, never a
// mean. Rank 5 looks past the 4-pixel cross the half-rate fill makes of one traced sample, so that cross goes
// too; a bright area wider than a few pixels has more bright neighbours and stays. Single-frame, energy lost.
// Keeps the 5 largest of the values offered so far, t[0] largest (a sorting network step; -1 = empty).
void nrd2Top5(inout float t[5], float v) {
    [unroll] for (uint k = 0u; k < 5u; ++k) {
        const float hi = max(t[k], v);
        v = min(t[k], v);
        t[k] = hi;
    }
}
float3 nrd2Despeckle(float3 c, float rankLum, float capScale) {
    const float l = nrd2Lum(c);
    const float cap = capScale * max(rankLum, 0.0);
    return (rankLum >= 0.0 && l > cap && l > 0.0) ? c * (cap / l) : c;
}

// Speckle blur (CSNrd2Blur, Settings::nrd2Speckle 1): a fixed 3-ring, 24-tap Gaussian (sigma = radius / 2)
// on this frame's D' and S', stopped at depth (against the plane-predicted depth) and normal edges. No
// luminance stop, so a blotch spreads out instead of being kept. Specular blurs over a roughness-scaled
// radius, none below 0.1 (mirrors).
static const float kNrd2BlurDepthK  = 30.0;   // exp(-k |dz| / z)
static const float kNrd2BlurNormalP = 8.0;    // cos^p, loose: the G-buffer normals carry the normal maps
float nrd2BlurWeight(float dist, float radius, float dzRel, float cosN) {
    const float s = 0.5 * radius;
    return exp(-dist * dist / (2.0 * s * s)) * exp(-kNrd2BlurDepthK * min(dzRel, 64.0)) *
           pow(saturate(cosN), kNrd2BlurNormalP);
}
float nrd2BlurSpecScale(float rough) { return saturate((rough - 0.1) / 0.3); }

// Ranking of a history texel in the 3x3 search: normal agreement and relative depth error.
float nrd2StabScore(float cosN, float dzAbs, float zExp) {
    return exp(-kNrd2StabNormalK * (1.0 - max(cosN, 0.0))) * exp(-kNrd2StabDepthK * min(dzAbs / max(zExp, 1.0e-4), 64.0));
}

// Squared relative luminance change, the noise estimate: ((a - b) / max(a, b, scale / 2))^2, in [0, 1].
float nrd2StabRelVar(float lumA, float lumB, float scale) {
    const float m = max(max(lumA, lumB), 0.5 * scale);
    const float d = abs(lumA - lumB) / max(m, 1.0e-6);
    return d * d;
}

// Weight of a pixel in its 8x8 tile's anchor: bright outliers fall away. lum / tileScale is unit-free.
float nrd2AnchorWeight(float lum, float tileScale) {
    return max(exp(-kNrd2AnchorK * min(max(lum, 0.0) / max(tileScale, 1.0e-6), 1.0e4)), kNrd2AnchorMin);
}

// The 1/8 anchor texture bilinearly at viewport pixel q (validity-weighted Loads); a = 1 when any tap was valid.
float4 nrd2SampleAnchor(Texture2D<float4> t, uint2 q, uint2 size) {
    const float2 pos  = (float2(q) + 0.5) / 8.0 - 0.5;
    const int2   base = int2(floor(pos));
    const float2 f    = pos - float2(base);
    float4 acc = 0.0;
    [unroll] for (uint i = 0u; i < 4u; ++i) {
        const int2 o = int2(i & 1u, i >> 1);
        const float4 v = t.Load(int3(clamp(base + o, int2(0, 0), int2(size) - 1), 0));
        if (v.a > 0.0) { const float b = (o.x ? f.x : 1.0 - f.x) * (o.y ? f.y : 1.0 - f.y); acc += float4(b * v.rgb, b); }
    }
    return acc.a > 1.0e-4 ? float4(acc.rgb / acc.a, 1.0) : 0.0;
}

// Prefilter weight of one neighbour: normal, depth (relative error vs the plane prediction) and radiance
// (distance to the tile anchor in units of the pixel's luminance scale) stops, times the variance gain.
float nrd2PfWeight(float cosN, float dzRel, float radDiffRel, float variance) {
    const float wn = pow(max(cosN, 0.0), kNrd2PfNormalPow);
    const float wd = exp(-kNrd2PfDepthK * min(dzRel, 64.0));
    const float wr = max(exp(-(kNrd2PfRadBias + variance * kNrd2PfRadVarK) * min(radDiffRel, 1.0e4)), kNrd2PfRadMin);
    return wn * wd * wr * max(kNrd2PfVarMin, 1.0 - exp(-variance * kNrd2PfVarK));
}

// The centre's own weight: the radiance stop alone (it keeps firefly energy out of the sum).
float nrd2PfCentreWeight(float radDiffRel, float variance) {
    return max(exp(-(kNrd2PfRadBias + variance * kNrd2PfRadVarK) * min(radDiffRel, 1.0e4)), kNrd2PfRadMin);
}

// The temporal resolve of one signal: cur (prefiltered, firefly-clamped) pulled toward the anchor by
// 1/(n + 1), clipped to this frame's min/max box united with the anchor; the history clipped to the same box
// (order statistics only, never a mean or variance) and blended in with weight 1 - 1/n.
float3 nrd2TemporalBlend(float3 cur, float3 anchor, float3 lo, float3 hi, float3 hist, float n, out float3 outCur,
                         out float3 outHist) {
    lo = min(lo, anchor);
    hi = max(hi, anchor);
    outCur  = clamp(lerp(cur, anchor, 1.0 / max(n + 1.0, 1.0)), lo, hi);
    outHist = clamp(hist, lo, hi);
    return lerp(outCur, outHist, nrd2StabBlend(n));
}

// ---- backward (phase 3 oracle): d(output)/d(theta) for one pixel and one signal --------------------
// theta = the six tile parameters in field order (logits l1..l3, log2 depth / normal / luminance
// sensitivity); the own logit stays pinned at 0. Levels, guides and the pixel's own value are fixed
// data. The reference level's switch (conf > 0.05) is treated as constant. Assumes theta inside the
// clamp ranges of nrd2SanitiseParams (the fit keeps it there). CPU twin: Nrd2ResolveReference.cpp.
static const float kNrd2Ln2 = 0.69314718;

// A level's estimate with its derivatives by log2 depth (0) and log2 normal (1) sensitivity.
struct Nrd2Level {
    float3 c;
    float  conf;
    float3 dc[2];
    float  dconf[2];
};

Nrd2Level nrd2UpsampleTapsGrad(Nrd2Taps t, float zm, float3 n, float log2Depth, float log2Normal) {
    const float dS = exp2(log2Depth), nP = exp2(log2Normal);
    float3 sum = 0.0, dsum0 = 0.0, dsum1 = 0.0;
    float  wsum = 0.0, dw0 = 0.0, dw1 = 0.0;
    [unroll] for (uint i = 0u; i < 4u; ++i) {
        float dz, cosN;
        const float w = nrd2TapWeight(t.g[i], t.x[i], t.b[i], zm, t.off[i], n, dS, nP, dz, cosN);
        if (w <= 0.0) continue;
        const float gd = -w * kNrd2Ln2 * kNrd2Ln2 * dS * dz;              // d w / d log2Depth
        const float gn = w * log(max(cosN, 1.0e-30)) * kNrd2Ln2 * nP;     // d w / d log2Normal
        sum += w * t.x[i].rgb;   wsum += w;
        dsum0 += gd * t.x[i].rgb; dw0 += gd;
        dsum1 += gn * t.x[i].rgb; dw1 += gn;
    }
    Nrd2Level L;
    L.conf = wsum;
    L.dconf[0] = dw0;
    L.dconf[1] = dw1;
    if (wsum > 1.0e-6) {
        L.c = sum / wsum;
        L.dc[0] = (dsum0 - L.c * dw0) / wsum;
        L.dc[1] = (dsum1 - L.c * dw1) / wsum;
    } else {
        L.c = 0.0; L.dc[0] = 0.0; L.dc[1] = 0.0;
    }
    return L;
}

// Same value as nrd2Combine; dOut[k] = d(output)/d(theta_k).
float3 nrd2ResolveBackward(float3 c0, bool ownValid, Nrd2Level lv[3], Nrd2TileParams p, float3 extraLogit,
                           out float3 dOut[6]) {
    [unroll] for (uint z = 0u; z < 6u; ++z) dOut[z] = 0.0;
    const float conf[3] = {lv[0].conf, lv[1].conf, lv[2].conf};
    float l[4];
    l[0] = nrd2Lum(c0);
    [unroll] for (uint a = 0u; a < 3u; ++a) l[a + 1u] = nrd2Lum(lv[a].c);
    const uint r = conf[2] > 0.05 ? 3u : conf[1] > 0.05 ? 2u : conf[0] > 0.05 ? 1u : 0u;
    const float ref = l[r];
    const float eps = max(ref, 0.0) * 1.0e-3 + 1.0e-7;
    const float sL  = exp2(p.log2Lum);
    const float lr  = log2(max(ref, 0.0) + eps);
    const float logitIn[3] = {p.logit[0] + extraLogit.x, p.logit[1] + extraLogit.y, p.logit[2] + extraLogit.z};

    float  w[4], m[4], e[3], s[3];
    float3 c[4];
    c[0] = c0;
    [unroll] for (uint k = 0u; k < 4u; ++k) {
        if (k > 0u) c[k] = lv[k - 1u].c;
        m[k] = min(abs(log2(max(l[k], 0.0) + eps) - lr), 16.0);
        const float lw = exp2(-sL * m[k]);
        if (k == 0u) { w[0] = ownValid ? lw : 0.0; continue; }
        e[k - 1u] = exp(clamp(logitIn[k - 1u], -16.0, 16.0));
        s[k - 1u] = saturate(conf[k - 1u]);
        w[k] = e[k - 1u] * s[k - 1u] * lw;
    }
    const float wsum = w[0] + w[1] + w[2] + w[3];
    if (!(wsum > 1.0e-8)) return ownValid ? c0 : 0.0;
    const float3 res = (w[0] * c0 + w[1] * c[1] + w[2] * c[2] + w[3] * c[3]) / wsum;

    // One tangent direction per parameter (forward mode).
    [unroll] for (uint j = 0u; j < 6u; ++j) {
        float  dl[4], dconf[3];
        float3 dc[4];
        dl[0] = 0.0; dc[0] = 0.0;
        [unroll] for (uint b = 0u; b < 3u; ++b) {
            const uint g = j == 3u ? 0u : 1u;
            const bool geo = j == 3u || j == 4u;
            dc[b + 1u] = geo ? lv[b].dc[g] : 0.0;
            dconf[b]   = geo ? lv[b].dconf[g] : 0.0;
            dl[b + 1u] = nrd2Lum(dc[b + 1u]);
        }
        const float dref = dl[r];
        const float deps = ref > 0.0 ? 1.0e-3 * dref : 0.0;
        const float dlr  = ((ref > 0.0 ? dref : 0.0) + deps) / ((max(ref, 0.0) + eps) * kNrd2Ln2);
        float  dW = 0.0;
        float3 dNum = 0.0;
        [unroll] for (uint k2 = 0u; k2 < 4u; ++k2) {
            const float a  = max(l[k2], 0.0) + eps;
            const float u  = log2(a) - lr;
            const float du = ((l[k2] > 0.0 ? dl[k2] : 0.0) + deps) / (a * kNrd2Ln2) - dlr;
            const float dm = abs(u) < 16.0 ? (u > 0.0 ? du : u < 0.0 ? -du : 0.0) : 0.0;
            const float lw = exp2(-sL * m[k2]);
            const float dlw = -kNrd2Ln2 * lw * (sL * dm + (j == 5u ? kNrd2Ln2 * sL * m[k2] : 0.0));
            float dw;
            if (k2 == 0u) {
                dw = ownValid ? dlw : 0.0;
            } else {
                const uint i = k2 - 1u;
                const float de = (j == i && abs(logitIn[i]) < 16.0) ? e[i] : 0.0;
                const float ds = (conf[i] > 0.0 && conf[i] < 1.0) ? dconf[i] : 0.0;
                dw = de * s[i] * lw + e[i] * ds * lw + e[i] * s[i] * dlw;
            }
            dW   += dw;
            dNum += dw * c[k2] + w[k2] * dc[k2];
        }
        dOut[j] = (dNum - res * dW) / wsum;
    }
    return res;
}

#endif  // NRD2_RESOLVE_HLSLI
