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
    uint  gPad;
};

SamplerState gLinear : register(s0);

// The two passes reuse t0/u0, so each is compiled with only its own section: FG_GATHER or FG_FILL
// (ProceduralFrameGenerator.cpp passes the define with the entry point).

// ---------------------------------------------------------------- gather
#if defined(FG_GATHER)

Texture2D<float4> gColN : register(t0);
Texture2D<float2> gVelN : register(t1);
Texture2D<float>  gZN   : register(t2);
Texture2D<float4> gColP : register(t3);
Texture2D<float2> gVelP : register(t4);
Texture2D<float>  gZP   : register(t5);
RWTexture2D<float4> gOut : register(u0);

static const int   kSearchSteps   = 4;
static const float kHoleWeight    = 0.08;   // below this summed confidence the pixel is a hole
static const float kDepthTolerate = 0.02;   // relative view-depth difference accepted outright
static const float kDepthFalloff  = 0.08;   // ...and the extra difference over which agreement fades to 0
static const float kResidualScale = 2.0;    // exp(-k r^2), r in pixels: half a pixel keeps ~0.6

bool inside(float2 p) { return all(p >= 0.0) && all(p < float2(gSize)); }
int3 texel(float2 p)  { return int3(clamp(int2(floor(p)), int2(0, 0), int2(gSize) - 1), 0); }

float2 velN(float2 p) { return gVelN.Load(texel(p)); }
float2 velP(float2 p) { return gVelP.Load(texel(p)); }

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

// Relative depth agreement between a surface at depth zA and what the other real frame stored where
// that surface should be: 1 inside the tolerance, fading to 0. Real depths only -- never a depth
// interpolated to the in-between time.
float depthAgree(float zA, float zB)
{
    const float rel = abs(zA - zB) / max(min(zA, zB), 1e-3);
    return saturate(1.0 - max(rel - kDepthTolerate, 0.0) / kDepthFalloff);
}

struct Candidate { float3 color; float conf; };

// From N: solve p - 0.5 v_N(p) = x.  Checked against P: the surface was at p - v_N(p) in P.
Candidate fromN(float2 x, float2 start)
{
    float2 p = start;
    [unroll] for (int k = 0; k < kSearchSteps; ++k) p = x + 0.5 * velN(p);
    const float2 v = velN(p);
    const float  r = length(p - 0.5 * v - x);

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

// From P: solve r + 0.5 v_P(r) = x (constant velocity).  Checked against N: the surface should be at
// r + v_P(r) in N.
Candidate fromP(float2 x, float2 start)
{
    float2 q = start;
    [unroll] for (int k = 0; k < kSearchSteps; ++k) q = x - 0.5 * velP(q);
    const float2 v = velP(q);
    const float  r = length(q + 0.5 * v - x);

    Candidate c;
    c.color = gColP.SampleLevel(gLinear, q / float2(gSize), 0).rgb;
    c.conf  = 0.0;
    if (!inside(q)) return c;
    const float2 next = q + v;
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
