// neurafi.hlsl -- procedural frame interpolation, milestone 1 (docs/rendering/NEURAFI.md Â§3).
// Three compute passes, full resolution: gather candidates, fill holes, compute motion for training.

#define FG_GROUP 8

cbuffer FgConstants : register(b1)
{
    uint2 gSize;     // scene width, height
    uint  gLast;     // CSFgFill: 1 on the final pass (writes alpha 1)
    uint  gTraj;     // CSFgGather: 1 = follow the acceleration image (t6), 0 = straight lines
    uint  gMode;     // CSFgAccel: 0 = analytic acceleration, 1 = the network's (u5)
    uint  gFrame;    // CSFgTrainRecords: varies the sampled pixels frame to frame
    uint  gSamples;  // CSFgTrainRecords: records to write at most
    uint  gBlock;    // the acceleration image holds one texel per gBlock x gBlock scene pixels
    uint  gViz;      // CSFgGather: the visualisation written to gVizOut (NeuraFI::Visualisation; 0 = none)
    float gVizScale; // CSFgGather: pixels of path bend shown at full heat (visualisations 3 and 4)
    uint  gPad0, gPad1;
};

// The acceleration image's size. gBlock grows with the scene (2, 4, 8...) so the network's record count stays bounded.
uint2 accelSize() { return (gSize + gBlock - 1u) / gBlock; }

SamplerState gLinear : register(s0);

// Each pass compiled with only its own section: FG_GATHER, FG_FILL or FG_TRAJ.
//
// TRAJECTORY (NEURAFI.md Â§3.5). The gather follows a QUADRATIC path: q = p - 0.5 v - 0.125 a,
// where v is motion into N and v' its motion into N-1 (fetched backward with a depth check; a = 0 if check fails).
// `a` is analytic v - v' plus (neural) a small network's CORRECTION, one per gBlock x gBlock pixels.
// One trajectory per pixel: never a choice between linear and quadratic.

// ---------------------------------------------------------------- shared

static const float kDepthTolerate = 0.02;   // relative view-depth difference accepted outright
static const float kDepthFalloff  = 0.08;   // ...and the extra difference over which agreement fades to 0

bool inside(float2 p) { return all(p >= 0.0) && all(p < float2(gSize)); }
int3 texel(float2 p)  { return int3(clamp(int2(floor(p)), int2(0, 0), int2(gSize) - 1), 0); }

// Relative depth agreement between a surface at depth zA and what the other real frame stored where that surface should be: 1 inside the tolerance, fading to 0.
float depthAgree(float zA, float zB)
{
    const float rel = abs(zA - zB) / max(min(zA, zB), 1e-3);
    return saturate(1.0 - max(rel - kDepthTolerate, 0.0) / kDepthFalloff);
}

// ---------------------------------------------------------------- gather
#if defined(FG_GATHER)

Texture2D<float4> gColN : register(t0);
Texture2D<float2> gVelN : register(t1);
Texture2D<float>  gZN   : register(t2);
Texture2D<float4> gColP : register(t3);
Texture2D<float2> gVelP : register(t4);
Texture2D<float>  gZP   : register(t5);
Texture2D<float2> gAccel : register(t6);   // one texel per gBlock x gBlock pixels, on frame N's grid
Texture2D<float2> gAccelNet : register(t7);   // the network's share of gAccel
RWTexture2D<float4> gOut : register(u0);
RWTexture2D<float4> gVizOut : register(u1);   // display-ready visualisation colour

static const int   kSearchSteps   = 4;
static const float kHoleWeight    = 0.08;   // below this summed confidence the pixel is a hole
static const float kResidualScale = 2.0;    // exp(-k r^2), r in pixels

float2 velN(float2 p) { return gVelN.Load(texel(p)); }
float2 velP(float2 p) { return gVelP.Load(texel(p)); }
// Frame N's acceleration at p (0 on straight-line gathering).
float2 accel(float2 p)
{
    if (gTraj == 0u) return 0.0;
    const int2 q = clamp(int2(floor(p / float(gBlock))), int2(0, 0), int2(accelSize()) - 1);
    return gAccel.Load(int3(q, 0));
}

// The velocity of largest magnitude in the 3x3 around x: a second search start for fast-moving thin objects.
float2 dominantVel(Texture2D<float2> vel, float2 x)
{
    float2 best = 0.0;
    float  bestLen = -1.0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx) {
        const float2 v = vel.Load(texel(x + float2(dx, dy)));
        const float  l = dot(v, v);
        if (l > bestLen) { bestLen = l; best = v; }
    }
    return best;
}

struct Candidate { float3 color; float conf; };

// From N: solve p - 0.5 v_N(p) - 0.125 a(p) = x. Checked against P: the surface was at p - v_N(p) in P.
Candidate fromN(float2 x, float2 start)
{
    float2 p = start;
    [unroll] for (int k = 0; k < kSearchSteps; ++k) p = x + 0.5 * velN(p) + 0.125 * accel(p);
    const float2 v = velN(p);
    const float  r = length(p - 0.5 * v - 0.125 * accel(p) - x);

    Candidate c;
    c.color = gColN.SampleLevel(gLinear, p / float2(gSize), 0).rgb;
    c.conf  = 0.0;
    if (!inside(p)) return c;
    const float2 prev = p - v;
    // Not on screen in P: nothing to check against, so half trust rather than none.
    const float agree = inside(prev) ? depthAgree(gZN.Load(texel(p)), gZP.Load(texel(prev))) : 0.5;
    c.conf = exp(-kResidualScale * r * r) * agree;
    return c;
}

// From P: solve r + 0.5 v_P(r) + 0.375 a = x, where a is fetched on N's grid. Checked against N.
Candidate fromP(float2 x, float2 start)
{
    float2 q = start;
    [unroll] for (int k = 0; k < kSearchSteps; ++k) {
        const float2 vk = velP(q);
        q = x - 0.5 * vk - 0.375 * accel(q + vk);
    }
    const float2 v = velP(q);
    const float2 a = accel(q + v);
    const float  r = length(q + 0.5 * v + 0.375 * a - x);

    Candidate c;
    c.color = gColP.SampleLevel(gLinear, q / float2(gSize), 0).rgb;
    c.conf  = 0.0;
    if (!inside(q)) return c;
    const float2 next = q + v + a;
    const float agree = inside(next) ? depthAgree(gZP.Load(texel(q)), gZN.Load(texel(next))) : 0.5;
    c.conf = exp(-kResidualScale * r * r) * agree;
    return c;
}

float luma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// ---- VISUALISATION ----
// Display-ready colours: blended over the finished frame after tonemap by the editor.
static const float3 kVizHole = float3(1.0, 0.0, 1.0);   // magenta

// Blue (0) -> cyan -> green -> yellow -> red (1).
float3 vizHeat(float t)
{
    t = saturate(t);
    return saturate(float3(1.5 - abs(4.0 * t - 3.0), 1.5 - abs(4.0 * t - 2.0), 1.5 - abs(4.0 * t - 1.0)));
}

float2 accelNet(float2 p)
{
    const int2 q = clamp(int2(floor(p / float(gBlock))), int2(0, 0), int2(accelSize()) - 1);
    return gAccelNet.Load(int3(q, 0));
}

// Visualisation modes: 1=source blend (orange/blue), 2=confidence, 3=path bend (quadratic+learned), 4=network share.
float4 vizColour(float2 x, float wN, float wP, float conf, bool hole)
{
    if (gViz == 1u) {
        if (hole) return float4(kVizHole, 1.0);
        const float share = wP / max(wN + wP, 1e-6);
        const float3 c = lerp(float3(1.0, 0.55, 0.12), float3(0.15, 0.55, 1.0), share);
        return float4(c * (0.35 + 0.65 * saturate((wN + wP) * 0.5)), 1.0);
    }
    if (gViz == 2u) return float4(hole ? kVizHole : vizHeat(conf), 1.0);
    const float scale = max(gVizScale, 1e-3);
    if (gViz == 3u) return float4(vizHeat(0.125 * length(accel(x)) / scale), 1.0);
    if (gViz == 4u) return float4(vizHeat(gMode == 1u ? 0.125 * length(accelNet(x)) / scale : 0.0), 1.0);
    return float4(0.0, 0.0, 0.0, 0.0);
}

[numthreads(FG_GROUP, FG_GROUP, 1)]
void CSFgGather(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gSize)) return;
    const float2 x = float2(id.xy) + 0.5;

    // Two starts per frame; keep the more confident result within each frame.
    Candidate n0 = fromN(x, x);
    Candidate n1 = fromN(x, x + 0.5 * dominantVel(gVelN, x));
    Candidate n  = n0;
    if (n1.conf > n0.conf) n = n1;
    Candidate p0 = fromP(x, x);
    Candidate p1 = fromP(x, x - 0.5 * dominantVel(gVelP, x));
    Candidate p  = p0;
    if (p1.conf > p0.conf) p = p1;

    // Colour agreement scales total confidence only, never the frame split: lighting changes are common.
    const float ln = luma(n.color), lp = luma(p.color);
    const float colourAgree = lerp(1.0, saturate(1.0 - abs(ln - lp) / (max(ln, lp) + 0.05)), 0.25);

    const float w = n.conf + p.conf;
    const bool hole = w < kHoleWeight;
    const float conf = hole ? 0.0 : saturate(w * 0.5 * colourAgree);
    if (gViz != 0u) gVizOut[id.xy] = vizColour(x, n.conf, p.conf, conf, hole);
    if (hole) {
        // Hole: frame N's own colour at x seeds the fill; confidence 0 marks it for CSFgFill.
        gOut[id.xy] = float4(gColN.Load(int3(id.xy, 0)).rgb, 0.0);
        return;
    }
    const float3 blended = (n.color * n.conf + p.color * p.conf) / w;
    gOut[id.xy] = float4(blended, conf);
}

#endif  // FG_GATHER

// ---------------------------------------------------------------- fill
#if defined(FG_FILL)

Texture2D<float4>   gFillSrc : register(t0);
RWTexture2D<float4> gFillDst : register(u0);

static const float kFilled     = 0.05;   // confidence above which a pixel is left alone
static const float kSeedWeight = 0.05;   // the hole's own frame-N colour, against neighbours' confidence

[numthreads(FG_GROUP, FG_GROUP, 1)]
void CSFgFill(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gSize)) return;
    const float4 c = gFillSrc.Load(int3(id.xy, 0));
    if (c.a >= kFilled) {
        gFillDst[id.xy] = float4(c.rgb, gLast != 0 ? 1.0 : c.a);
        return;
    }
    // Normalised convolution over the 3x3: each neighbour weighted by its confidence.
    float3 acc = c.rgb * kSeedWeight;
    float  wsum = kSeedWeight;
    float  aMax = 0.0;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx) {
        if (dx == 0 && dy == 0) continue;
        const int2 t = clamp(int2(id.xy) + int2(dx, dy), int2(0, 0), int2(gSize) - 1);
        const float4 nb = gFillSrc.Load(int3(t, 0));
        acc  += nb.rgb * nb.a;
        wsum += nb.a;
        aMax  = max(aMax, nb.a);
    }
    // A filled pixel passes half its best neighbour's confidence on, so a second pass reaches one pixel further.
    gFillDst[id.xy] = float4(acc / wsum, gLast != 0 ? 1.0 : 0.5 * aMax);
}

#endif  // FG_FILL

// ---------------------------------------------------------------- trajectory
// Four passes: CSFgFeatures (per accel texel: network input), CSFgAccel (acceleration image),
// CSFgClearCount (zero training record count), CSFgTrainRecords (self-supervised training).
//
// THE RECORD (14 floats): layout identical for inference and training so one network serves both:
//   [0,1]  v / s       motion over the span ending at the pixel's frame
//   [2,3]  v' / s      the same surface's motion over the span before (backward fetch)
//   [4]    1 if the backward fetch passed its depth check, else 0 (and v' = v)
//   [5]    log2(s) / 8
//   [6-13] v / s at the 4 neighbours 2 pixels away (+x, -x, +y, -y)
// with s = max(|v|, |v'|, 0.5) pixels: scale-free, so a network trained on two-frame spans applies to one-frame.
// The output is the correction to the analytic acceleration, (a - (v - v')) / s.
//
// SELF-SUPERVISED TRAINING from three ordinary real frames: interpolate the span N-2 -> N using frame N-1 as ground truth.
// For a pixel p of N: v = (N-2 -> N), v' = (N-4 -> N-2) at the surface's N-2 position, target = a / s where a = 8 v_N - 4 v.
// Purely geometric (engine motion vectors): no colour reaches the loss, so the network learns motion only.
// Records written only where every backward fetch passes its depth check.

#if defined(FG_TRAJ)

Texture2D<float2> gTVelN  : register(t0);   // frame N (the one just rendered)
Texture2D<float>  gTZN    : register(t1);
Texture2D<float2> gTVel1  : register(t2);   // N-1
Texture2D<float>  gTZ1    : register(t3);
Texture2D<float2> gTVel2  : register(t4);   // N-2 (training only)
Texture2D<float>  gTZ2    : register(t5);
Texture2D<float2> gTVel3  : register(t6);   // N-3 (training only)
Texture2D<float>  gTZ3    : register(t7);
RWStructuredBuffer<float> gRecords    : register(u0);   // inference records
RWTexture2D<float2>       gAccelOut   : register(u1);   // the acceleration image
RWStructuredBuffer<float> gTrainRec   : register(u2);
RWStructuredBuffer<float> gTrainTgt   : register(u3);
RWStructuredBuffer<uint>  gTrainCount : register(u4);   // [0] records written, [1] outliers rejected
RWStructuredBuffer<float> gNetOut     : register(u5);   // the network's outputs for gRecords
RWTexture2D<float2>       gAccelNetOut : register(u6);  // the network's share of gAccelOut (visualisation only)

#define FG_RECORD 14u
#define FG_OUTPUT 2u

float2 tvel(Texture2D<float2> t, float2 p) { return t.Load(texel(p)); }
float  tz(Texture2D<float> t, float2 p)    { return t.Load(texel(p)); }

// Frame N's motion at p, and N-1's for the same surface fetched backward with a depth check.
void inferMotion(float2 p, out float2 v, out float2 vPrev, out bool valid)
{
    v = tvel(gTVelN, p);
    const float2 y = p - v;
    valid = inside(y) && depthAgree(tz(gTZN, p), tz(gTZ1, y)) > 0.5;
    vPrev = valid ? tvel(gTVel1, y) : v;
}

float scaleOf(float2 v, float2 vPrev) { return max(max(length(v), length(vPrev)), 0.5); }

void writeRecord(RWStructuredBuffer<float> buf, uint base, float2 v, float2 vPrev, bool valid, float s,
                 float2 nb[4])
{
    const float is = 1.0 / s;
    buf[base + 0] = v.x * is;      buf[base + 1] = v.y * is;
    buf[base + 2] = vPrev.x * is;  buf[base + 3] = vPrev.y * is;
    buf[base + 4] = valid ? 1.0 : 0.0;
    buf[base + 5] = log2(s) / 8.0;
    [unroll] for (uint k = 0; k < 4u; ++k) { buf[base + 6 + 2 * k] = nb[k].x * is; buf[base + 7 + 2 * k] = nb[k].y * is; }
}

static const float2 kNbOffset[4] = { float2(2, 0), float2(-2, 0), float2(0, 2), float2(0, -2) };

// The centre, in scene pixels, of acceleration-image texel q.
float2 blockCentre(uint2 q) { return float2(q * gBlock) + 0.5 * float(gBlock); }

[numthreads(FG_GROUP, FG_GROUP, 1)]
void CSFgFeatures(uint3 id : SV_DispatchThreadID)
{
    const uint2 qs = accelSize();
    if (any(id.xy >= qs)) return;
    const float2 p = blockCentre(id.xy);
    float2 v, vPrev; bool valid;
    inferMotion(p, v, vPrev, valid);
    float2 nb[4];
    [unroll] for (uint k = 0; k < 4u; ++k) nb[k] = tvel(gTVelN, p + kNbOffset[k]);
    writeRecord(gRecords, (id.y * qs.x + id.x) * FG_RECORD, v, vPrev, valid, scaleOf(v, vPrev), nb);
}

[numthreads(FG_GROUP, FG_GROUP, 1)]
void CSFgAccel(uint3 id : SV_DispatchThreadID)
{
    const uint2 qs = accelSize();
    if (any(id.xy >= qs)) return;
    const float2 p = blockCentre(id.xy);
    float2 v, vPrev; bool valid;
    inferMotion(p, v, vPrev, valid);
    float2 a = 0.0;   // where the backward fetch failed: straight line, never inferred from neighbours
    float2 corr = 0.0;
    if (valid) {
        const float s = scaleOf(v, vPrev);
        const uint o = (id.y * qs.x + id.x) * FG_OUTPUT;
        a = v - vPrev;
        if (gMode == 1u) {
            corr = float2(gNetOut[o], gNetOut[o + 1]) * s;   // the network's correction
            a += corr;
        }
        // A bound on what one frame can bend a path by: a wild prediction must not throw the search across the screen.
        const float len = length(a);
        if (!(len <= 2.0 * s)) a = len > 0.0 && len == len ? a * (2.0 * s / len) : 0.0;
    }
    gAccelOut[id.xy] = a;
    if (gViz == 4u) gAccelNetOut[id.xy] = all(corr == corr) ? corr : float2(0.0, 0.0);
}

[numthreads(1, 1, 1)]
void CSFgClearCount(uint3 id : SV_DispatchThreadID) { gTrainCount[0] = 0u; gTrainCount[1] = 0u; }

static const float kMaxCorrection = 0.5;   // |correction| / s above this is a motion-vector seam

uint hashU(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }

// One surface followed back three frames; false where any step leaves the screen or fails its depth check.
bool twoFrameSpans(float2 p, out float2 vN, out float2 span, out float2 spanPrev)
{
    vN = tvel(gTVelN, p);
    const float2 y1 = p - vN;
    if (!inside(y1) || depthAgree(tz(gTZN, p), tz(gTZ1, y1)) <= 0.5) return false;
    const float2 v1 = tvel(gTVel1, y1);
    const float2 y2 = y1 - v1;
    if (!inside(y2) || depthAgree(tz(gTZ1, y1), tz(gTZ2, y2)) <= 0.5) return false;
    const float2 v2 = tvel(gTVel2, y2);
    const float2 y3 = y2 - v2;
    if (!inside(y3) || depthAgree(tz(gTZ2, y2), tz(gTZ3, y3)) <= 0.5) return false;
    span = vN + v1;
    spanPrev = v2 + tvel(gTVel3, y3);
    return true;
}

[numthreads(64, 1, 1)]
void CSFgTrainRecords(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gSamples) return;
    const uint h = hashU(id.x * 0x9E3779B9u ^ hashU(gFrame + 0x632BE5ABu));
    const float2 p = float2(h % gSize.x, (h / gSize.x) % gSize.y) + 0.5;
    float2 vN, span, spanPrev;
    if (!twoFrameSpans(p, vN, span, spanPrev)) return;
    // The neighbours' two-frame spans (no depth checks: context, not the quantity being learned).
    float2 nb[4];
    [unroll] for (uint k = 0; k < 4u; ++k) {
        const float2 pn = p + kNbOffset[k];
        const float2 a = tvel(gTVelN, pn);
        nb[k] = a + tvel(gTVel1, pn - a);
    }
    const float s = scaleOf(span, spanPrev);
    const float2 target = ((8.0 * vN - 4.0 * span) - (span - spanPrev)) / s;
    // Corrections above half the span's motion are motion-vector seams, not camera/object motion, and are rejected.
    if (!(length(target) <= kMaxCorrection)) { InterlockedAdd(gTrainCount[1], 1u); return; }
    uint slot;
    InterlockedAdd(gTrainCount[0], 1u, slot);
    if (slot >= gSamples) return;
    writeRecord(gTrainRec, slot * FG_RECORD, span, spanPrev, true, s, nb);
    gTrainTgt[slot * FG_OUTPUT + 0] = target.x;
    gTrainTgt[slot * FG_OUTPUT + 1] = target.y;
}

#endif  // FG_TRAJ
