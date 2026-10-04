// Aver.Render.PathTracer: presents the accumulator to the scene target (PSPathTracePresent).
//
// Moved out of a C++ raw-string literal; composed and -D'd exactly as before.

struct PtPresentIn { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };

cbuffer PtPresentCB : register(b1) { float4 gPtPresentInfo; };   // x width, y height, z layout, w unused

StructuredBuffer<float4> gPtAccumRead : register(t0);

float4 PSPathTracePresent(PtPresentIn i) : SV_TARGET {
    uint W = (uint)gPtPresentInfo.x;
    uint H = (uint)gPtPresentInfo.y;
    // i.ndc is [-1,1] with +Y toward the top of the viewport (D3D clip-space convention); the
    // accumulator's row 0 is ALSO the top of the traced image (see PtShaders.hpp's CSPathTrace: pixel
    // row 0 maps to ndc.y=-1 in ITS OWN convention, whose -ndc.y term then leans the ray toward +up --
    // i.e. row 0 leans toward whatever "up" the camera was handed). Flipping v here is what keeps the
    // two agreeing without needing a matching flip on the C++ side.
    float2 uv = i.ndc * 0.5 + 0.5;

    // BILINEAR, NOT NEAREST, AND THIS IS THE CHEAPEST REAL IMPROVEMENT ON THIS PATH.
    //
    // The accumulator is 480x270 (kAccumWidth/kAccumHeight) and the viewport it is shown in is
    // whatever the window is -- 3532x1987 on the display this was measured on, a 7.4x blow-up. The
    // old lookup truncated uv to an integer texel, so every accumulator texel became a solid ~7x7
    // block of identical pixels. That does not add noise, but it MAGNIFIES it: per-texel variance
    // that would read as fine film grain reads instead as coarse blocky mottling, which is far more
    // objectionable at the same numerical error. It also stair-stepped every silhouette in the
    // image, visible on the horizon line of any capture taken before this.
    //
    // Manual rather than a SamplerState because the accumulator is a StructuredBuffer, not a
    // texture -- it has to be, since the compute pass writes it as a UAV of float4 PAIRS (radiance,
    // and a sample count in .z). Four fetches instead of one, on a fullscreen pass that was already
    // trivially cheap.
    //
    // THE SAMPLE COUNT IS INTERPOLATED TOO, not taken from one texel. Every texel of a still frame
    // holds the same count so it makes no difference then -- but the frame after a camera move has
    // texels mid-update, and dividing one texel's radiance by another's count is how a blend seam
    // becomes a bright or dark band.
    float fx = clamp(uv.x * float(W) - 0.5, 0.0, float(W) - 1.0);
    float fy = clamp((1.0 - uv.y) * float(H) - 0.5, 0.0, float(H) - 1.0);
    uint x0 = (uint)floor(fx), y0 = (uint)floor(fy);
    uint x1 = min(x0 + 1u, W - 1u), y1 = min(y0 + 1u, H - 1u);
    float tx = fx - float(x0), ty = fy - float(y0);

    // TWO SOURCE LAYOUTS, chosen by gPtPresentInfo.z. The denoiser writes a one-element buffer of
    // means, so there is nothing left to divide; without it this reads the raw two-element
    // accumulator and divides, exactly as it did before the filter existed. Keeping the fallback
    // is what lets a device that cannot compile the filter still show a path-traced image.
    const bool denoised = gPtPresentInfo.z > 0.5;

    float3 c00, c10, c01, c11;
    if (denoised) {
        c00 = gPtAccumRead[y0 * W + x0].rgb;
        c10 = gPtAccumRead[y0 * W + x1].rgb;
        c01 = gPtAccumRead[y1 * W + x0].rgb;
        c11 = gPtAccumRead[y1 * W + x1].rgb;
    } else {
        // The sample count is interpolated with the radiance rather than taken from one texel:
        // the frame after a camera move has texels mid-update, and dividing one texel's radiance
        // by another's count is how a blend seam becomes a band.
        c00 = gPtAccumRead[(y0 * W + x0) * 2 + 0].rgb / max(gPtAccumRead[(y0 * W + x0) * 2 + 1].z, 1.0);
        c10 = gPtAccumRead[(y0 * W + x1) * 2 + 0].rgb / max(gPtAccumRead[(y0 * W + x1) * 2 + 1].z, 1.0);
        c01 = gPtAccumRead[(y1 * W + x0) * 2 + 0].rgb / max(gPtAccumRead[(y1 * W + x0) * 2 + 1].z, 1.0);
        c11 = gPtAccumRead[(y1 * W + x1) * 2 + 0].rgb / max(gPtAccumRead[(y1 * W + x1) * 2 + 1].z, 1.0);
    }

    return float4(lerp(lerp(c00, c10, tx), lerp(c01, c11, tx), ty), 1.0);
}
