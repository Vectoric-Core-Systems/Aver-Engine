#pragma once
// Colour-vision filter: the CPU twin of shaders/aver_colour_vision.hlsli. Simulation uses the Machado, Oliveira and
// Fernandes (IEEE TVCG 15(6), 2009) severity-1.0 matrices in linear RGB; correction is the Fidaner, Lin and Ashburner
// (2005) error shift on top. Display-space input and output, [0,1], gamma 2.2 (the engine's own curve, color.hlsli).
// docs/ACCESSIBILITY.md section 2.3.
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

#include <algorithm>
#include <cmath>

namespace aver::rhi {

enum class ColourVisionMode : u32 { Off = 0, Protanopia = 1, Deuteranopia = 2, Tritanopia = 3 };

// Rows of M, out = M * in. Index 0 = Protanopia, 1 = Deuteranopia, 2 = Tritanopia. Every row sums to 1: grey is kept.
inline constexpr f32 kColourVisionMatrix[3][3][3] = {
    {{0.152286f, 1.052583f, -0.204868f}, {0.114503f, 0.786281f, 0.099216f}, {-0.003882f, -0.048116f, 1.051998f}},
    {{0.367322f, 0.860646f, -0.227968f}, {0.280085f, 0.672501f, 0.047413f}, {-0.011820f, 0.042940f, 0.968881f}},
    {{1.255528f, -0.076749f, -0.178779f}, {-0.078411f, 0.930809f, 0.147602f}, {0.004733f, 0.691367f, 0.303900f}},
};

// The error shift: the part of the colour the viewer cannot see, moved into channels they can. Protan and deutan push
// red error into green and blue; tritan pushes blue error into red and green.
inline constexpr f32 kColourVisionShift[2][3][3] = {
    {{0, 0, 0}, {0.7f, 1, 0}, {0.7f, 0, 1}},
    {{1, 0, 0.7f}, {0, 1, 0.7f}, {0, 0, 0}},
};

// mode (0..3) + 4 * simulate + 8 * round(strength * 100); 0 when the mode is Off. The one float that rides PostCB.access.x
// and the UI shader's root constant.
inline f32 packColourVision(u32 mode, bool simulate, f32 strength) {
    if (mode == 0 || mode > 3) return 0.0f;
    const f32 s = std::clamp(strength, 0.0f, 1.0f);
    return static_cast<f32>(mode + (simulate ? 4u : 0u) + 8u * static_cast<u32>(std::floor(s * 100.0f + 0.5f)));
}

// PostSettings::colourVision is mode | (simulate ? 4 : 0).
inline f32 packColourVision(const PostSettings& p) {
    return packColourVision(p.colourVision & 3u, (p.colourVision & 4u) != 0, p.colourVisionStrength);
}

// Decodes the packed float (mirror of the shader).
inline u32 colourVisionMode(f32 packed) { return static_cast<u32>(packed + 0.5f) & 3u; }
inline bool colourVisionSimulate(f32 packed) { return (static_cast<u32>(packed + 0.5f) & 4u) != 0; }
inline f32 colourVisionStrength(f32 packed) { return static_cast<f32>(static_cast<u32>(packed + 0.5f) >> 3) * 0.01f; }

// Display-space colour in, display-space colour out, exactly what averColourVision() does. packed <= 0.5 is the identity.
inline void applyColourVision(f32 packed, const f32 in[3], f32 out[3]) {
    if (!(packed > 0.5f)) { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; return; }
    const u32 mode = colourVisionMode(packed);
    const bool sim = colourVisionSimulate(packed);
    const f32 k = colourVisionStrength(packed);
    if (mode == 0) { out[0] = in[0]; out[1] = in[1]; out[2] = in[2]; return; }
    const f32 (*m)[3] = kColourVisionMatrix[mode - 1];
    const f32 (*sh)[3] = kColourVisionShift[mode == 3 ? 1 : 0];

    f32 lin[3], s[3], o[3];
    for (int i = 0; i < 3; ++i) lin[i] = std::pow(std::clamp(in[i], 0.0f, 1.0f), 2.2f);
    for (int i = 0; i < 3; ++i) s[i] = m[i][0] * lin[0] + m[i][1] * lin[1] + m[i][2] * lin[2];
    if (sim) {
        for (int i = 0; i < 3; ++i) o[i] = s[i];
    } else {
        const f32 err[3] = {lin[0] - s[0], lin[1] - s[1], lin[2] - s[2]};
        for (int i = 0; i < 3; ++i) o[i] = lin[i] + sh[i][0] * err[0] + sh[i][1] * err[1] + sh[i][2] * err[2];
    }
    for (int i = 0; i < 3; ++i) {
        const f32 v = lin[i] + (o[i] - lin[i]) * k;
        out[i] = std::pow(std::clamp(v, 0.0f, 1.0f), 1.0f / 2.2f);
    }
}

} // namespace aver::rhi
