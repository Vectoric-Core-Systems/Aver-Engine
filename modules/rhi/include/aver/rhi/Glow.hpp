#pragma once
// How the bloom pyramid's result joins the frame (add, screen, soft light, replace, mix) and the debanding noise, as the CPU twin of the
// AVER_POST_TONEMAP_EXT block in color.hlsli / post.hlsl. The GPU runs the HLSL; this header is what the headless tests check against
// reference values. Only the EXT composite carries any of it: PostSettings' glow and deband fields at their defaults leave the ordinary
// composite untouched (postNeedsExtComposite in Tonemap.hpp).
#include "aver/core/Types.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/Tonemap.hpp"

#include <algorithm>
#include <cmath>

namespace aver::rhi {

enum GlowBlendMode : u32 { kGlowAdd = 0, kGlowScreen, kGlowSoftLight, kGlowReplace, kGlowMix, kGlowBlendCount };

inline u32 clampGlowBlend(i64 mode) { return static_cast<u32>(std::clamp<i64>(mode, 0, kGlowBlendCount - 1)); }

// True when the glow or the debanding needs the EXT composite: a blend other than add, a strength other than 1, a luminance cap or debanding.
inline bool postUsesGlowExt(const PostSettings& p) {
    return p.glowBlend != 0 || p.glowStrength != 1.0f || p.glowLumCap > 0.0f || p.deband != 0;
}

namespace glow {

using tmap::Vec3f;

// The soft-light curve (W3C compositing, the Photoshop form) for backdrop cb and source cs, one channel.
inline f32 softLightChannel(f32 cb, f32 cs) {
    if (cs <= 0.5f) return cb - (1.0f - 2.0f * cs) * cb * (1.0f - cb);
    const f32 d = cb <= 0.25f ? ((16.0f * cb - 12.0f) * cb + 4.0f) * cb : std::sqrt(cb);
    return cb + (2.0f * cs - 1.0f) * (d - cb);
}

// The frame colour `c` (scene-linear, exposed) with the glow sample `g` (the pyramid's rgb) blended in. `intensity` is PostSettings::bloomIntensity,
// `strength` and `lumCap` the glow fields. Mode 0 is c + g * intensity, which is what the ordinary composite does with strength 1 and no cap.
inline Vec3f blend(u32 mode, Vec3f c, Vec3f g, f32 intensity, f32 strength, f32 lumCap) {
    strength = std::max(strength, 0.0f);
    for (int i = 0; i < 3; ++i) g.v[i] *= strength;
    if (lumCap > 0.0f) {
        const f32 l = 0.2126f * g.v[0] + 0.7152f * g.v[1] + 0.0722f * g.v[2];
        if (l > lumCap) for (int i = 0; i < 3; ++i) g.v[i] *= lumCap / l;
    }
    Vec3f o;
    if (mode == kGlowMix) {
        const f32 t = std::clamp(intensity, 0.0f, 1.0f);
        for (int i = 0; i < 3; ++i) o.v[i] = c.v[i] + (g.v[i] - c.v[i]) * t;
        return o;
    }
    for (int i = 0; i < 3; ++i) g.v[i] *= intensity;
    for (int i = 0; i < 3; ++i) {
        const f32 cb = std::max(c.v[i], 0.0f);
        switch (mode) {
            case kGlowScreen:    o.v[i] = std::max((cb + g.v[i]) - cb * g.v[i], 0.0f); break;
            case kGlowSoftLight: o.v[i] = softLightChannel(cb, g.v[i] * 0.5f + 0.5f); break;
            case kGlowReplace:   o.v[i] = g.v[i]; break;
            default:             o.v[i] = c.v[i] + g.v[i]; break;
        }
    }
    return o;
}

// The debanding offset added to a display-space channel: interleaved gradient noise `n` in [0, 1) mapped to +-half an 8-bit step.
inline f32 debandOffset(f32 n) { return (n - 0.5f) / 255.0f; }

} // namespace glow
} // namespace aver::rhi
