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
struct Nrd2Taps {
    float4 g[4];
    float4 x[4];
    float  b[4];
};

Nrd2Taps nrd2LoadTaps(Texture2D<float4> guide, Texture2D<float4> value, uint shift, uint2 q, uint2 lvlSize) {
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
    }
    return t;
}

// One tap's weight: kept by depth and normal agreement with this pixel and by the texel's validity.
// dz and cosN come back for the backward pass (0 weight: both unused).
float nrd2TapWeight(float4 g, float4 x, float b, float zm, float3 n, float depthSens, float normalPow,
                    out float dz, out float cosN) {
    dz = 0.0; cosN = 0.0;
    const float nl = length(g.xyz);
    if (g.w <= 0.0 || x.a <= 0.0 || nl < 1.0e-3) return 0.0;
    dz = min(abs(g.w - zm) / max(zm, 1.0e-4), 64.0);
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
        const float w = nrd2TapWeight(t.g[i], t.x[i], t.b[i], zm, n, depthSens, normalPow, dz, cosN);
        if (w <= 0.0) continue;
        sum  += w * t.x[i].rgb;
        wsum += w;
    }
    conf = wsum;
    return wsum > 1.0e-6 ? sum / wsum : 0.0;
}

float3 nrd2Upsample(Texture2D<float4> guide, Texture2D<float4> value, uint shift, uint2 q, uint2 lvlSize,
                    float zm, float3 n, float depthSens, float normalPow, out float conf) {
    return nrd2UpsampleTaps(nrd2LoadTaps(guide, value, shift, q, lvlSize), zm, n, depthSens, normalPow, conf);
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

// ---- temporal stabiliser (docs/rendering/NRD2.md): maths shared with the CPU twin ------------------------
// Runs on jitter-free frames only, on the resolve's D' and S' (irradiance), after the spatial filter. History
// length comes from motion and disocclusion only and feeds the blend weight alone (NEURAA_NRD.md rule 5).
static const float kNrd2StabSpeedStill = 0.25;   // px per frame: full history length at or below
static const float kNrd2StabSpeedFast  = 8.0;    // log-space ramp down to Nfast here
static const float kNrd2StabSpeedCut   = 128.0;  // no history from here (a whip)
static const float kNrd2StabRoughLo    = 0.35;   // S takes history only for rough lobes (the resolve's smooth threshold)
static const float kNrd2StabRoughSpan  = 0.3;
static const float kNrd2StabDepthRel   = 0.02;   // reprojected-depth acceptance: relative + 1 cm + local plane slope
static const float kNrd2StabDepthAbs   = 0.01;
static const float kNrd2StabMinWeight  = 0.5;    // valid bilinear weight needed for any history
static const float kNrd2StabAgeWeight  = 0.1;
static const float kNrd2StabAgeCap     = 255.0;

struct Nrd2StabTap {
    float  b;      // bilinear weight (0 outside the viewport)
    float  age;    // frames of history behind this texel (0 = none)
    float  zm;     // its view Z, metres
    float3 d, s;   // stabilised D'' and S''
};

// Frames of history allowed at this screen speed (px per frame): nStill at rest, nFast from 8 px, none from 128 px.
float nrd2StabMaxFrames(float speed, float nStill, float nFast) {
    if (!(speed < kNrd2StabSpeedCut)) return 0.0;
    const float t = saturate(log2(max(speed, kNrd2StabSpeedStill) / kNrd2StabSpeedStill) /
                             log2(kNrd2StabSpeedFast / kNrd2StabSpeedStill));
    const float hi = max(nStill, 1.0);
    return exp2(lerp(log2(hi), log2(clamp(nFast, 1.0, hi)), t));
}

// Weight of the history: 0, 1/2, 2/3, ... up to 1 - 1/nMax.
float nrd2StabAlpha(float age, float nMax) {
    return saturate(1.0 - 1.0 / max(min(age + 1.0, nMax), 1.0));
}

// cur, the clamp box (min/max of this frame's values only), the four reprojected history taps and the depth
// they must match (zExp +- zTol). Out: the blended D'' / S'' and the history age to store.
void nrd2StabCombine(float3 curD, float3 curS, float3 loD, float3 hiD, float3 loS, float3 hiS, Nrd2StabTap t[4],
                     float zExp, float zTol, float nMax, float rough, bool historyValid, out float3 outD,
                     out float3 outS, out float outAge) {
    float  wsum = 0.0, nAge = 1.0e9;
    float3 hd = 0.0, hs = 0.0;
    [unroll] for (uint i = 0u; i < 4u; ++i) {
        if (!(historyValid && t[i].b > 0.0 && t[i].age > 0.0 && abs(t[i].zm - zExp) <= zTol)) continue;
        wsum += t[i].b;
        hd += t[i].b * t[i].d;
        hs += t[i].b * t[i].s;
        if (t[i].b > kNrd2StabAgeWeight) nAge = min(nAge, t[i].age);
    }
    float aD = 0.0, aS = 0.0;
    outAge = 1.0;
    if (wsum >= kNrd2StabMinWeight && nMax > 0.0) {
        aD = nrd2StabAlpha(nAge, nMax);
        aS = aD * saturate((rough - kNrd2StabRoughLo) / kNrd2StabRoughSpan);
        hd = clamp(hd / wsum, loD, hiD);
        hs = clamp(hs / wsum, loS, hiS);
        outAge = min(nAge + 1.0, kNrd2StabAgeCap);
    }
    outD = lerp(curD, hd, aD);
    outS = lerp(curS, hs, aS);
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
        const float w = nrd2TapWeight(t.g[i], t.x[i], t.b[i], zm, n, dS, nP, dz, cosN);
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
