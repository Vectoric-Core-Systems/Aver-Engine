#ifndef AVER_COLOUR_VISION_HLSLI
#define AVER_COLOUR_VISION_HLSLI
// Colour-vision filter (docs/ACCESSIBILITY.md section 2.3), shared by the scene composite (post.hlsl PSComposite) and
// the HUD pixel shader (render.ui ui.hlsl, which draws after the tonemap). CPU twin: rhi/ColourVision.hpp.
//
// Simulation: Machado, Oliveira and Fernandes, IEEE TVCG 15(6), 2009, severity 1.0, linear RGB. Correction: the error
// shift of Fidaner, Lin and Ashburner (2005) on top of it. Rows of the matrices sum to 1, so grey stays grey.
//
// Self-contained: it carries its own 2.2 curve so it needs neither color.hlsli nor the shared prelude (two differently
// composed shaders include it). Mode 0 never reaches it: callers branch on packed > 0.5.

float3x3 averCvMatrix(int mode) {
    if (mode == 1)   // protanopia
        return float3x3(0.152286, 1.052583, -0.204868,
                        0.114503, 0.786281, 0.099216,
                        -0.003882, -0.048116, 1.051998);
    if (mode == 2)   // deuteranopia
        return float3x3(0.367322, 0.860646, -0.227968,
                        0.280085, 0.672501, 0.047413,
                        -0.011820, 0.042940, 0.968881);
    return float3x3(1.255528, -0.076749, -0.178779,      // tritanopia
                    -0.078411, 0.930809, 0.147602,
                    0.004733, 0.691367, 0.303900);
}

float3x3 averCvShift(int mode) {
    if (mode == 3)
        return float3x3(1.0, 0.0, 0.7,
                        0.0, 1.0, 0.7,
                        0.0, 0.0, 0.0);
    return float3x3(0.0, 0.0, 0.0,
                    0.7, 1.0, 0.0,
                    0.7, 0.0, 1.0);
}

// disp: display-space [0,1]. packed = mode + 4 * simulate + 8 * round(strength * 100).
float3 averColourVision(float3 disp, float packed) {
    int p = (int)(packed + 0.5);
    int mode = p & 3;
    bool sim = (p & 4) != 0;
    float k = (float)(p >> 3) * 0.01;
    if (mode == 0)
        return disp;
    float3 lin = pow(saturate(disp), 2.2);
    float3 s = mul(averCvMatrix(mode), lin);
    float3 o = sim ? s : lin + mul(averCvShift(mode), lin - s);
    return pow(saturate(lerp(lin, o, k)), 1.0 / 2.2);
}
#endif
