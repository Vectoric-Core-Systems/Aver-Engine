// AverSR: temporal anti-aliasing with upscale (TAAU). One pass, at OUTPUT resolution:
//   reconstruct this frame from the jittered scene samples around the output pixel, reproject last
//   frame's result with the G-buffer velocity, clip it to this frame's neighbourhood, blend.
// Works in the squashed space c / (1 + max(c)) (see sr_fsr1.hlsl) so bright pixels cannot dominate;
// the history is stored squashed and FSR's RCAS pass un-squashes after sharpening.

cbuffer AverSrTaaCB : register(b3) {
    float4 gTaaSrc;   // xy scene size, zw reciprocal
    float4 gTaaDst;   // xy output size, zw reciprocal
    float4 gTaaJit;   // xy this frame's jitter in scene pixels (+y down), z 1 = no usable history, w 1 = velocity bound
};

Texture2D<float4> gTaaColor   : register(t0);   // scene colour, HDR, jittered
Texture2D<float2> gTaaVel     : register(t1);   // scene pixels, destination minus source, carries +jitter
Texture2D<float>  gTaaViewZ   : register(t2);   // 0 where nothing wrote the G-buffer
Texture2D<float4> gTaaHistory : register(t3);   // last result, output size, squashed
SamplerState      gTaaLinear  : register(s0);   // linear, clamp

struct AverSrTaaVSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

AverSrTaaVSOut AverSrTaaVS(uint id : SV_VertexID) {
    AverSrTaaVSOut o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

float3 averTaaSquash(float3 c) { c = max(c, 0.0); return c / (1.0 + max(c.r, max(c.g, c.b))); }
float3 averTaaToYCoCg(float3 c) { return float3(dot(c, float3(0.25, 0.5, 0.25)), dot(c, float3(0.5, 0.0, -0.5)), dot(c, float3(-0.25, 0.5, -0.25))); }
float3 averTaaFromYCoCg(float3 c) { return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z); }

// Catmull-Rom history fetch in 9 bilinear taps (the 4x4 kernel's inner taps merged), so the history
// does not soften each time a moving camera resamples it between pixels.
float3 averTaaHistoryCatmullRom(float2 uv) {
    const float2 pos = uv * gTaaDst.xy;
    const float2 c = floor(pos - 0.5) + 0.5;
    const float2 f = pos - c;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);
    const float2 w12 = w1 + w2;
    const float2 t0 = (c - 1.0) * gTaaDst.zw, t3 = (c + 2.0) * gTaaDst.zw;
    const float2 t12 = (c + w2 / w12) * gTaaDst.zw;
    float3 r = 0.0;
    r += gTaaHistory.SampleLevel(gTaaLinear, float2(t0.x,  t0.y),  0).rgb * w0.x  * w0.y;
    r += gTaaHistory.SampleLevel(gTaaLinear, float2(t12.x, t0.y),  0).rgb * w12.x * w0.y;
    r += gTaaHistory.SampleLevel(gTaaLinear, float2(t3.x,  t0.y),  0).rgb * w3.x  * w0.y;
    r += gTaaHistory.SampleLevel(gTaaLinear, float2(t0.x,  t12.y), 0).rgb * w0.x  * w12.y;
    r += gTaaHistory.SampleLevel(gTaaLinear, float2(t12.x, t12.y), 0).rgb * w12.x * w12.y;
    r += gTaaHistory.SampleLevel(gTaaLinear, float2(t3.x,  t12.y), 0).rgb * w3.x  * w12.y;
    r += gTaaHistory.SampleLevel(gTaaLinear, float2(t0.x,  t3.y),  0).rgb * w0.x  * w3.y;
    r += gTaaHistory.SampleLevel(gTaaLinear, float2(t12.x, t3.y),  0).rgb * w12.x * w3.y;
    r += gTaaHistory.SampleLevel(gTaaLinear, float2(t3.x,  t3.y),  0).rgb * w3.x  * w3.y;
    return max(r, 0.0);
}

float4 AverSrTaaResolvePS(AverSrTaaVSOut i) : SV_TARGET {
    const float2 uv     = i.pos.xy * gTaaDst.zw;
    const float2 srcPos = uv * gTaaSrc.xy;                    // continuous scene-pixel position
    const float2 jit    = gTaaJit.xy;
    // The camera is offset by +jit, so scene pixel n shows the point at n + 0.5 - jit. Nearest one:
    const int2   centre = int2(floor(srcPos + jit));
    const int2   maxPx  = int2(gTaaSrc.xy) - 1;

    // Two reconstructions of this frame: WIDE (kernel in scene pixels) for the clip box and for
    // pixels without history; NARROW (kernel in output pixels) for the blend, weighted by how close
    // the nearest sample landed -- over the jitter cycle every output pixel gets one close to it,
    // which is what makes the accumulated image as sharp as the output grid.
    const float  scale = gTaaDst.x * gTaaSrc.z;                // output pixels per scene pixel
    float3 sum = 0.0, m1 = 0.0, m2 = 0.0, nsum = 0.0;
    float  wsum = 0.0, nwsum = 0.0, nmax = 0.0;
    float  bestZ = 1.0e30;
    int2   bestPx = clamp(centre, int2(0, 0), maxPx);
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int2  p = clamp(centre + int2(x, y), int2(0, 0), maxPx);
            const float3 c = averTaaSquash(gTaaColor.Load(int3(p, 0)).rgb);
            const float2 d = (float2(p) + 0.5 - jit) - srcPos;
            const float  w = exp(-2.29 * dot(d, d));           // Blackman-Harris-like, in scene pixels
            sum += c * w; wsum += w;
            const float2 dn = d * scale;
            const float  wn = exp(-2.29 * dot(dn, dn));        // the same, in output pixels
            nsum += c * wn; nwsum += wn; nmax = max(nmax, wn);
            const float3 yc = averTaaToYCoCg(c);
            m1 += yc; m2 += yc * yc;
            const float z = gTaaViewZ.Load(int3(p, 0));
            if (z > 0.0 && z < bestZ) { bestZ = z; bestPx = p; }   // nearest surface leads the motion
        }
    }
    const float3 wide    = sum / max(wsum, 1e-6);
    const float3 narrow  = nwsum > 1e-4 ? nsum / nwsum : wide;
    const float3 mean  = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));

    // Pure motion: velocity minus this frame's jitter (it is written against an unjittered
    // previous camera). Where nothing wrote the G-buffer, the surface is treated as static.
    float2 motion = 0.0;
    if (gTaaJit.w > 0.5 && bestZ < 1.0e30) motion = gTaaVel.Load(int3(bestPx, 0)) - jit;
    const float2 histUv = uv - motion * gTaaSrc.zw;

    const bool  histOk = gTaaJit.z < 0.5 && all(histUv > 0.0) && all(histUv < 1.0);
    float3 history = averTaaHistoryCatmullRom(histUv);

    // Variance clip toward the mean: history that this frame's neighbourhood cannot explain (a
    // disocclusion, a moving shadow) is pulled to its edge instead of ghosting.
    const float3 hYc = averTaaToYCoCg(history);
    const float3 boxMin = mean - sigma * 1.25, boxMax = mean + sigma * 1.25;
    const float3 toH = hYc - mean;
    const float3 ext = max((boxMax - boxMin) * 0.5, 1e-5);
    const float  t = max(abs(toH.x) / ext.x, max(abs(toH.y) / ext.y, abs(toH.z) / ext.z));
    history = averTaaFromYCoCg(t > 1.0 ? mean + toH / t : hYc);

    // Up to 10% new per frame when still, more while moving, scaled by how close this frame's
    // nearest sample is to the pixel. Without history, the wide estimate stands alone.
    const float3 current = histOk ? narrow : wide;
    float alpha = histOk ? max(lerp(0.1, 0.25, saturate(length(motion) / 16.0)) * nmax, 0.03) : 1.0;
    // Luma weighting keeps one flickering sample from swinging the result.
    const float wc = alpha / (1.0 + current.g), wh = (1.0 - alpha) / (1.0 + history.g);
    const float3 result = (current * wc + history * wh) / max(wc + wh, 1e-6);
    return float4(result, 1.0);
}
