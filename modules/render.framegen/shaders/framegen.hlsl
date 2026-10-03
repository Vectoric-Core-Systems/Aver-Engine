// framegen.hlsl -- procedural frame interpolation, milestone 1 (docs/rendering/FRAME_INTERPOLATION.md §3).
//
// One frame half-way between the previous real frame (P = N-1) and the one just rendered (N), at scene
// resolution, in HDR. Three passes, all compute, all full resolution:
//
//   CSFgGather  G1 gather + G2 confidence + G3 blend. Per output pixel x, a fixed-point SEARCH into each
//               real frame along that frame's OWN motion (Yang et al. 2011, bidirectional scene
//               reprojection): no motion field is built for the in-between time and nothing is
//               scattered. Each frame's candidate gets ONE continuous confidence from real-frame
//               consistency; the two are blended by those confidences. A pixel neither frame can vouch
//               for is a HOLE: it is written with frame N's own colour at x and confidence 0.
//   CSFgFill    G4, run twice: holes take the confidence-weighted average of their 3x3 neighbours,
//               seeded by that frame-N colour. Full resolution -- no pyramid, no mip chain, no
//               reduce-then-expand (Georgia Tech US 9,094,660 claims that structure), and neighbours
//               are weighted by CONFIDENCE ONLY, never chosen by depth similarity to the hole (the
//               amended claim of NVIDIA US 2025/0106355). Colour space only; motion is never propagated.
//
// Coordinates: continuous pixel positions with texel CENTRES at .5 (pixel q's centre is q + 0.5).
// Motion follows IDevice::gBufferVelocityTexture: texels per frame, destination minus source, so the
// surface at x in frame F was at x - v_F(x) in frame F-1. Over two frames the velocity is assumed
// constant: P's own motion (from N-2 to N-1) stands in for its motion on to N.
//
// Alpha of every intermediate image carries the blend confidence; the last fill pass writes 1.

#define FG_GROUP 8

cbuffer FgConstants : register(b1)
{
    uint2 gSize;     // scene width, height
    uint  gLast;     // CSFgFill: 1 on the final pass (writes alpha 1)
    uint  gTraj;     // CSFgGather: 1 = follow the acceleration image (t6), 0 = straight lines
#if defined(FG_TRAJ)
    uint  gMode;     // CSFgAccel: 0 = analytic acceleration, 1 = the network's (u5)
    uint  gFrame;    // CSFgTrainRecords: varies the sampled pixels frame to frame
    uint  gSamples;  // CSFgTrainRecords: records to write at most
    uint  gPad2;
#endif
};

uint2 quarterSize() { return (gSize + 1u) / 2u; }

SamplerState gLinear : register(s0);

// The passes reuse registers, so each is compiled with only its own section: FG_GATHER, FG_FILL or
// FG_TRAJ (ProceduralFrameGenerator.cpp passes the define with the entry point).
//
// TRAJECTORY (FRAME_INTERPOLATION.md §3.5). The gather can follow a QUADRATIC path instead of a straight
// line. Through three positions of a surface (frames N-2, N-1, N), the in-between point is
//     q = p - 0.5 v - 0.125 a,   a = v - v'
// where v is its motion into N (frame N's own vector at p) and v' its motion into N-1 (frame N-1's
// vector, fetched BACKWARD at p - v with a depth check; a = 0 where that check fails -- never inferred
// from neighbours). `a` is one per pixel of a quarter-resolution acceleration image, either computed
// that way (analytic) or predicted by a small network from the same real-frame motion (neural); the
// network never outputs colour, a weight, a mask or a confidence, and nothing it outputs reaches the
// blend except through where the candidates are gathered. One trajectory per pixel: there is never a
// linear and a quadratic candidate to choose between.

// ---------------------------------------------------------------- shared

static const float kDepthTolerate = 0.02;   // relative view-depth difference accepted outright
static const float kDepthFalloff  = 0.08;   // ...and the extra difference over which agreement fades to 0

bool inside(float2 p) { return all(p >= 0.0) && all(p < float2(gSize)); }
int3 texel(float2 p)  { return int3(clamp(int2(floor(p)), int2(0, 0), int2(gSize) - 1), 0); }

// Relative depth agreement between a surface at depth zA and what the other real frame stored where
// that surface should be: 1 inside the tolerance, fading to 0. Real depths only -- never a depth
// interpolated to the in-between time.
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
Texture2D<float2> gAccel : register(t6);   // quarter resolution, on frame N's grid; read when gTraj != 0
RWTexture2D<float4> gOut : register(u0);

static const int   kSearchSteps   = 4;
static const float kHoleWeight    = 0.08;   // below this summed confidence the pixel is a hole
static const float kResidualScale = 2.0;    // exp(-k r^2), r in pixels: half a pixel keeps ~0.6

float2 velN(float2 p) { return gVelN.Load(texel(p)); }
float2 velP(float2 p) { return gVelP.Load(texel(p)); }
// Frame N's acceleration at p (0 on straight-line gathering).
float2 accel(float2 p)
{
    if (gTraj == 0u) return 0.0;
    const int2 q = clamp(int2(floor(p * 0.5)), int2(0, 0), int2(quarterSize()) - 1);
    return gAccel.Load(int3(q, 0));
}

// The velocity of largest magnitude in the 3x3 around x: a second search start, so a thin fast object
// whose own pixels are not under x at the in-between time can still be found.
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

// From N: solve p - 0.5 v_N(p) - 0.125 a(p) = x.  Checked against P: the surface was at p - v_N(p) in P.
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

// From P: solve r + 0.5 v_P(r) + 0.375 a = x, a fetched at the surface's place in N (r + v_P(r): the
// acceleration lives on N's grid). With a = 0 that is constant velocity.  Checked against N: the
// surface should be at r + v_P(r) + a in N.
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

[numthreads(FG_GROUP, FG_GROUP, 1)]
void CSFgGather(uint3 id : SV_DispatchThreadID)
{
    if (any(id.xy >= gSize)) return;
    const float2 x = float2(id.xy) + 0.5;

    // Two starts per frame; each frame keeps its more confident search result. This picks among the
    // search results within ONE real frame -- it never selects among motion vectors landing at an
    // in-between location (there are none: nothing is splatted).
    Candidate n0 = fromN(x, x);
    Candidate n1 = fromN(x, x + 0.5 * dominantVel(gVelN, x));
    Candidate n  = n0;
    if (n1.conf > n0.conf) n = n1;   // (HLSL's ?: does not take structs)
    Candidate p0 = fromP(x, x);
    Candidate p1 = fromP(x, x - 0.5 * dominantVel(gVelP, x));
    Candidate p  = p0;
    if (p1.conf > p0.conf) p = p1;

    // Colour agreement, low weight: a disagreement is as often a lighting change as an error, so it
    // only scales the TOTAL confidence (how sure the pixel is), never the split between the frames.
    const float ln = luma(n.color), lp = luma(p.color);
    const float colourAgree = lerp(1.0, saturate(1.0 - abs(ln - lp) / (max(ln, lp) + 0.05)), 0.25);

    const float w = n.conf + p.conf;
    if (w < kHoleWeight) {
        // Hole: frame N's own colour at x seeds the fill; confidence 0 marks it for CSFgFill.
        gOut[id.xy] = float4(gColN.Load(int3(id.xy, 0)).rgb, 0.0);
        return;
    }
    const float3 blended = (n.color * n.conf + p.color * p.conf) / w;
    gOut[id.xy] = float4(blended, saturate(w * 0.5 * colourAgree));
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
    // Normalised convolution over the 3x3: each neighbour weighted by its confidence alone.
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
    // A filled pixel passes half its best neighbour's confidence on, so a second pass reaches one
    // pixel further into a wider hole.
    gFillDst[id.xy] = float4(acc / wsum, gLast != 0 ? 1.0 : 0.5 * aMax);
}

#endif  // FG_FILL

// ---------------------------------------------------------------- trajectory
// Four passes on one binding layout:
//   CSFgFeatures      quarter res: the network's input record for each pixel of the acceleration image
//   CSFgAccel         quarter res: the acceleration image, analytic or from the network's output
//   CSFgClearCount    one thread: zeroes the training record count
//   CSFgTrainRecords  gSamples threads: self-supervised training records (below)
//
// THE RECORD (14 floats), identical in meaning for inference and training so one network serves both:
//   [0,1]  v / s       motion over the span ending at the pixel's frame
//   [2,3]  v' / s      the same surface's motion over the span before (backward fetch)
//   [4]    1 if the backward fetch passed its depth check, else 0 (and v' = v)
//   [5]    log2(s) / 8
//   [6-13] v / s at the 4 neighbours 2 pixels away (+x, -x, +y, -y)
// with s = max(|v|, |v'|, 0.5) pixels: scale-free, so a network trained on two-frame spans applies to
// one-frame spans. The output is a / s (2 floats).
//
// SELF-SUPERVISED TRAINING from three ordinary real frames: interpolate the TWO-frame span N-2 -> N and
// take frame N-1 as the answer. For a pixel p of N: v = (N-2 -> N) = v_N(p) + v_{N-1}(p - v_N(p)),
// v' = (N-4 -> N-2) at the surface's N-2 position, and the truth is where it was in N-1, p - v_N(p):
//     p - 0.5 v - 0.125 a = p - v_N   =>   a = 8 v_N - 4 v
// Purely geometric (engine motion vectors): no colour reaches the loss, so the network learns motion,
// never shading. A record is written only where every backward fetch passes its depth check.
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
RWTexture2D<float2>       gAccelOut   : register(u1);   // quarter-resolution acceleration image
RWStructuredBuffer<float> gTrainRec   : register(u2);
RWStructuredBuffer<float> gTrainTgt   : register(u3);
RWStructuredBuffer<uint>  gTrainCount : register(u4);
RWStructuredBuffer<float> gNetOut     : register(u5);   // the network's outputs for gRecords

#define FG_RECORD 14u
#define FG_OUTPUT 2u

float2 tvel(Texture2D<float2> t, float2 p) { return t.Load(texel(p)); }
float  tz(Texture2D<float> t, float2 p)    { return t.Load(texel(p)); }

// Inference: frame N's motion at p, and N-1's for the same surface fetched backward with a depth check.
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

// The centre, in scene pixels, of quarter-resolution pixel q.
float2 quarterCentre(uint2 q) { return float2(q * 2u) + 1.0; }

[numthreads(FG_GROUP, FG_GROUP, 1)]
void CSFgFeatures(uint3 id : SV_DispatchThreadID)
{
    const uint2 qs = quarterSize();
    if (any(id.xy >= qs)) return;
    const float2 p = quarterCentre(id.xy);
    float2 v, vPrev; bool valid;
    inferMotion(p, v, vPrev, valid);
    float2 nb[4];
    [unroll] for (uint k = 0; k < 4u; ++k) nb[k] = tvel(gTVelN, p + kNbOffset[k]);
    writeRecord(gRecords, (id.y * qs.x + id.x) * FG_RECORD, v, vPrev, valid, scaleOf(v, vPrev), nb);
}

[numthreads(FG_GROUP, FG_GROUP, 1)]
void CSFgAccel(uint3 id : SV_DispatchThreadID)
{
    const uint2 qs = quarterSize();
    if (any(id.xy >= qs)) return;
    const float2 p = quarterCentre(id.xy);
    float2 v, vPrev; bool valid;
    inferMotion(p, v, vPrev, valid);
    float2 a = 0.0;   // where the backward fetch failed: straight line, never inferred from neighbours
    if (valid) {
        const float s = scaleOf(v, vPrev);
        const uint o = (id.y * qs.x + id.x) * FG_OUTPUT;
        a = gMode == 1u ? float2(gNetOut[o], gNetOut[o + 1]) * s : v - vPrev;
        // A bound on what one frame can bend a path by: a wild prediction must not throw the search
        // across the screen.
        const float len = length(a);
        if (!(len <= 2.0 * s)) a = len > 0.0 && len == len ? a * (2.0 * s / len) : 0.0;
    }
    gAccelOut[id.xy] = a;
}

[numthreads(1, 1, 1)]
void CSFgClearCount(uint3 id : SV_DispatchThreadID) { gTrainCount[0] = 0u; }

uint hashU(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }

// One surface followed back three frames; false where any step leaves the screen or fails its depth
// check. Gives frame N's own motion, the two-frame span N-2 -> N, and the span N-4 -> N-2 before it.
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
    uint slot;
    InterlockedAdd(gTrainCount[0], 1u, slot);
    if (slot >= gSamples) return;
    const float s = scaleOf(span, spanPrev);
    writeRecord(gTrainRec, slot * FG_RECORD, span, spanPrev, true, s, nb);
    const float2 target = (8.0 * vN - 4.0 * span) / s;
    gTrainTgt[slot * FG_OUTPUT + 0] = target.x;
    gTrainTgt[slot * FG_OUTPUT + 1] = target.y;
}

#endif  // FG_TRAJ
