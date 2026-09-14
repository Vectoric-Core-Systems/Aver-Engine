// Aver.Render.PathTracer: the accumulator denoise (CSPtDenoise).
//
// Moved out of a C++ raw-string literal; composed and -D'd exactly as before.

cbuffer PtDenoiseCB : register(b1) {
    uint4  gDnInfo;   // x width, y height, z mode (0 resolve / 1 filter), w step in source texels
    float4 gDnTune;   // x colour sigma, y max samples, zw unused
};

StructuredBuffer<float4>   gDnSrc : register(t0);
RWStructuredBuffer<float4> gDnDst : register(u0);

float ptLum(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

// One source texel as (mean radiance, sample count), whichever layout the source is in.
float4 ptDnFetch(uint x, uint y, uint W, uint mode) {
    const uint p = y * W + x;
    if (mode == 0u) {
        const float4 rad = gDnSrc[p * 2 + 0];
        const float  n   = max(gDnSrc[p * 2 + 1].z, 1.0);
        return float4(rad.rgb / n, n);
    }
    return gDnSrc[p];
}

[numthreads(8, 8, 1)]
void CSPtDenoise(uint3 tid : SV_DispatchThreadID) {
    const uint W = gDnInfo.x, H = gDnInfo.y;
    if (tid.x >= W || tid.y >= H) return;
    const uint mode = gDnInfo.z;
    const uint step = max(gDnInfo.w, 1u);

    const float4 c = ptDnFetch(tid.x, tid.y, W, mode);

    // RESOLVE ONLY CONVERTS. Filtering on the same pass would make the first iteration a special
    // case with a different support from the other two, for no gain -- the chain is short enough
    // that one extra dispatch over 130k pixels costs nothing worth counting.
    if (mode == 0u) { gDnDst[tid.y * W + tid.x] = c; return; }

    // A CONVERGED PIXEL IS PASSED THROUGH UNTOUCHED. The sigma below already tends to zero as the
    // sample count rises, so this changes no pixel that the filter would have altered meaningfully
    // -- it is here because this view exists to be a REFERENCE, and "the reference is bit-exact once
    // converged" is a property worth being able to state without qualification rather than one that
    // merely holds to several decimal places.
    if (c.a >= gDnTune.y) { gDnDst[tid.y * W + tid.x] = c; return; }

    // THE EDGE STOP. Expected Monte Carlo error falls as 1/sqrt(n), so the luminance difference that
    // counts as "still the same surface" is scaled by exactly that: wide on the frame after a camera
    // move, vanishing once the image has settled.
    //
    // RELATIVE TO LOCAL BRIGHTNESS, which is the half that matters for the problem this was built
    // for. A shadow sits near 0.1 luminance and the sunlit floor near 0.8; an absolute threshold
    // tuned on the floor does nothing in the shadow, and one tuned in the shadow flattens the floor.
    const float lc    = ptLum(c.rgb);
    const float sigma = gDnTune.x * rsqrt(max(c.a, 1.0)) * max(lc, 0.02);

    // B3 spline, the standard a-trous kernel: [1 4 6 4 1]/16 as an outer product.
    const float k[5] = { 0.0625, 0.25, 0.375, 0.25, 0.0625 };

    float3 sum = float3(0, 0, 0);
    float  wsum = 0.0;
    [unroll] for (int dy = -2; dy <= 2; ++dy) {
        [unroll] for (int dx = -2; dx <= 2; ++dx) {
            const int sx = int(tid.x) + dx * int(step);
            const int sy = int(tid.y) + dy * int(step);
            // CLAMPED, not skipped. Skipping would renormalise the kernel differently at the border
            // and leave a one-texel frame of differently-filtered pixels around the image.
            const uint qx = (uint)clamp(sx, 0, int(W) - 1);
            const uint qy = (uint)clamp(sy, 0, int(H) - 1);
            const float4 q = ptDnFetch(qx, qy, W, mode);
            const float wc = exp(-abs(ptLum(q.rgb) - lc) / (sigma + 1e-6));
            const float w  = k[dx + 2] * k[dy + 2] * wc;
            sum  += q.rgb * w;
            wsum += w;
        }
    }

    // wsum can never be zero -- the centre tap weighs k[2]*k[2] with wc == 1 -- but the guard costs
    // nothing and a NaN here would propagate through every later pass.
    gDnDst[tid.y * W + tid.x] = float4(wsum > 1e-6 ? sum / wsum : c.rgb, c.a);
}
