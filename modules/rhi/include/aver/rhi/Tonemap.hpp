#pragma once
// The extended tonemappers (linear, Reinhard, filmic, ACES, AgX, each with a white point) and the post-tonemap adjustments,
// as the CPU twin of color.hlsli's AVER_POST_TONEMAP_EXT block. The GPU runs the HLSL; this header is what the headless tests
// check against reference values and what the composite-variant selection (postNeedsExtComposite) is decided by.
//
// Modes ride PostSettings::tonemap and PostCB::misc.w. 0..2 are Aver's own curves and are untouched; the extended curve
// with index m (0 linear, 1 Reinhard, 2 filmic, 3 ACES, 4 AgX) is mode 3 + m.
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"

#include <algorithm>
#include <cmath>

namespace aver::rhi {

enum TonemapMode : u32 {
    kTonemapAces = 0, kTonemapAcesFitted = 1, kTonemapAcesLuma = 2,   // Aver's own
    kTonemapExtLinear = 3, kTonemapExtReinhard = 4, kTonemapExtFilmic = 5, kTonemapExtAces = 6, kTonemapExtAgx = 7,
};
inline constexpr u32 kTonemapModeCount = 8;

inline bool tonemapIsExt(u32 mode) { return mode >= kTonemapExtLinear; }
inline u32 clampTonemapMode(i64 mode) { return static_cast<u32>(std::clamp<i64>(mode, 0, kTonemapModeCount - 1)); }
// An extended-curve index (0..4: linear, Reinhard, filmic, ACES, AgX) as an Aver mode.
inline u32 tonemapModeFromExtIndex(i64 extIndex) { return kTonemapExtLinear + static_cast<u32>(std::clamp<i64>(extIndex, 0, 4)); }

// True when the post chain needs the AVER_POST_TONEMAP_EXT composite: an extended tonemapper, or any display-space adjustment off identity.
// False is the default path: the ordinary composite pipelines, compiled with today's defines and nothing else.
inline bool postNeedsExtComposite(const PostSettings& p) {
    return tonemapIsExt(p.tonemap) || p.adjustBrightness != 1.0f || p.adjustContrast != 1.0f || p.adjustSaturation != 1.0f;
}

// The project's RENDER.TONEMAP* keys onto `p` (each < 0 = unstated and left alone). `mode` is applied only when `modeAllowed`
// (the editor leaves a mode the command line set). The one mapping both hosts share. True when anything was stated.
inline bool applyProjectTonemapKeys(PostSettings& p, int mode, f32 white, f32 brightness, f32 contrast, f32 saturation, bool modeAllowed) {
    bool any = false;
    if (mode >= 0) { any = true; if (modeAllowed) p.tonemap = clampTonemapMode(mode); }
    if (white >= 0.0f)      { p.tonemapWhite = white;          any = true; }
    if (brightness >= 0.0f) { p.adjustBrightness = brightness; any = true; }
    if (contrast >= 0.0f)   { p.adjustContrast = contrast;     any = true; }
    if (saturation >= 0.0f) { p.adjustSaturation = saturation; any = true; }
    return any;
}

namespace tmap {

struct Vec3f { f32 v[3]; };

// ---- the extended curves (written from their published definitions) ----

// Extended index 0. The result is clipped by the display encode.
inline Vec3f linear(Vec3f c, f32 /*white*/) { return c; }

// Extended Reinhard: c * (1 + c / white^2) / (1 + c).
inline Vec3f reinhard(Vec3f c, f32 white) {
    const f32 w2 = white * white;
    Vec3f o;
    for (int i = 0; i < 3; ++i) o.v[i] = c.v[i] * (1.0f + c.v[i] / w2) / (1.0f + c.v[i]);
    return o;
}

// Hable's filmic with an exposure bias of 2 baked into A and B, divided by the same curve at the white point.
inline f32 filmicCurve(f32 x) {
    constexpr f32 bias = 2.0f;
    constexpr f32 A = 0.22f * bias * bias, B = 0.30f * bias, C = 0.10f, D = 0.20f, E = 0.01f, F = 0.30f;
    return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}
inline Vec3f filmic(Vec3f c, f32 white) {
    const f32 w = filmicCurve(white);
    Vec3f o;
    for (int i = 0; i < 3; ++i) o.v[i] = filmicCurve(c.v[i]) / w;
    return o;
}

// ACES (Stephen Hill's RRT+ODT fit) with an exposure bias of 1.8 in the input matrix; the white point goes through the
// same curve (scaled by the bias) and divides the result.
inline Vec3f acesExt(Vec3f c, f32 white) {
    constexpr f32 bias = 1.8f;
    constexpr f32 A = 0.0245786f, B = 0.000090537f, C = 0.983729f, D = 0.4329510f, E = 0.238081f;
    static constexpr f32 in[3][3] = {{0.59719f, 0.35458f, 0.04823f}, {0.07600f, 0.90834f, 0.01566f}, {0.02840f, 0.13383f, 0.83777f}};
    static constexpr f32 out[3][3] = {{1.60475f, -0.53108f, -0.07367f}, {-0.10208f, 1.10813f, -0.00605f}, {-0.00327f, -0.07276f, 1.07602f}};
    f32 r[3];
    for (int i = 0; i < 3; ++i) {
        const f32 x = (in[i][0] * c.v[0] + in[i][1] * c.v[1] + in[i][2] * c.v[2]) * bias;
        r[i] = (x * (x + A) - B) / (x * (C * x + D) + E);
    }
    const f32 w = white * bias;
    const f32 wt = (w * (w + A) - B) / (w * (C * w + D) + E);
    Vec3f o;
    for (int i = 0; i < 3; ++i) o.v[i] = (out[i][0] * r[0] + out[i][1] * r[1] + out[i][2] * r[2]) / wt;
    return o;
}

// AgX (Troy Sobotka's), with the 6th-order polynomial sigmoid fit of the default look and no white point.
inline f32 agxContrast(f32 x) {
    const f32 x2 = x * x, x4 = x2 * x2;
    return 0.021f * x + 4.0111f * x2 - 25.682f * x2 * x + 70.359f * x4 - 74.778f * x4 * x + 27.069f * x4 * x2;
}
inline Vec3f agx(Vec3f c, f32 /*white*/) {
    static constexpr f32 toRec2020[3][3] = {{0.6274039f, 0.3292830f, 0.0433131f}, {0.0690973f, 0.9195405f, 0.0113622f}, {0.0163914f, 0.0880133f, 0.8955953f}};
    static constexpr f32 inset[3][3] = {{0.856627153315983f, 0.0951212405381588f, 0.0482516061458583f},
                                        {0.137318972929847f, 0.761241990602591f, 0.101439036467562f},
                                        {0.11189821299995f, 0.0767994186031903f, 0.811302368396859f}};
    static constexpr f32 outset[3][3] = {{1.1271005818144368f, -0.11060664309660323f, -0.016493938717834573f},
                                         {-0.1413297634984383f, 1.157823702216272f, -0.016493938717834257f},
                                         {-0.14132976349843826f, -0.11060664309660294f, 1.2519364065950405f}};
    static constexpr f32 fromRec2020[3][3] = {{1.6604910f, -0.5876411f, -0.0728499f}, {-0.1245505f, 1.1328999f, -0.0083494f}, {-0.0181508f, -0.1005789f, 1.1187297f}};
    constexpr f32 minEv = -12.4739311883324f, maxEv = 4.02606881166759f;
    auto mul = [](const f32 m[3][3], const f32 v[3], f32 o[3]) {
        for (int i = 0; i < 3; ++i) o[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
    };
    f32 a[3], b[3];
    mul(toRec2020, c.v, a);
    mul(inset, a, b);
    for (int i = 0; i < 3; ++i) {
        const f32 x = std::clamp(std::log2(std::max(b[i], 2e-10f)), minEv, maxEv);
        const f32 y = agxContrast((x - minEv) / (maxEv - minEv));
        b[i] = std::pow(std::max(y, 0.0f), 2.4f);
    }
    mul(outset, b, a);
    mul(fromRec2020, a, b);
    return Vec3f{{b[0], b[1], b[2]}};
}

// The tonemapper `mode` (3..7) on linear, exposed radiance. Negative input is floored at zero except by AgX, which floors it itself.
inline Vec3f ext(u32 mode, Vec3f c, f32 white) {
    white = std::max(white, 1e-3f);
    for (int i = 0; i < 3; ++i) c.v[i] = std::max(c.v[i], 0.0f);
    switch (mode) {
        case kTonemapExtReinhard: return reinhard(c, white);
        case kTonemapExtFilmic:   return filmic(c, white);
        case kTonemapExtAces:     return acesExt(c, white);
        case kTonemapExtAgx:      return agx(c, white);
        default:                  return linear(c, white);
    }
}

// Brightness / contrast / saturation, on the display-space colour after the tonemap: brightness scales,
// contrast pivots on 0.5, saturation pivots on the channel mean.
inline Vec3f adjust(Vec3f c, f32 brightness, f32 contrast, f32 saturation) {
    for (int i = 0; i < 3; ++i) c.v[i] *= brightness;
    for (int i = 0; i < 3; ++i) c.v[i] = 0.5f + (c.v[i] - 0.5f) * contrast;
    const f32 mean = (c.v[0] + c.v[1] + c.v[2]) * 0.33333f;
    for (int i = 0; i < 3; ++i) c.v[i] = mean + (c.v[i] - mean) * saturation;
    return c;
}

} // namespace tmap
} // namespace aver::rhi
