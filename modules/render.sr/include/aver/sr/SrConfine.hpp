// Confining AverSR's passes to the pixels the editor shows. Internal to render.sr.
//
// The docked editor shows only the scene viewport; the upscale of everything else is never read (the
// composite samples the upscaled image at the displayed pixels, one bilinear tap). A pass may skip the
// rest only if every pixel it still writes sees the taps it saw before, so each stage here is the
// next stage's reach, grown by that stage's kernel.
//
// The rect comes from the primary-visibility record (the Voxi staged viewport, which is the device's
// scene viewport), so it exists only while NeuRAA asks for visibility. A frame without it, a generated
// frame, a viewport that fills the image, or any inconsistency returns false: the whole image, as before.
#pragma once

#include "aver/rhi/RHIResources.hpp"

#include <algorithm>

namespace aver::sr {

struct PxRect { u32 x0 = 0, y0 = 0, x1 = 0, y1 = 0; };   // half-open

// Destination pixels around the displayed rect: the composite rounds its rect to whole pixels and
// samples bilinearly.
constexpr u32 kShowPad = 4;
// Source texels past a pixel's own that EASU's four gathers can touch (rows fp-2..fp+3, columns fp-1..fp+2).
constexpr u32 kEasuReach = 4;

inline PxRect growRect(PxRect r, u32 n, u32 w, u32 h) {
    return {r.x0 > n ? r.x0 - n : 0u, r.y0 > n ? r.y0 - n : 0u, std::min(r.x1 + n, w), std::min(r.y1 + n, h)};
}

// The destination pixels the post chain shows, padded. False: process the whole image.
inline bool displayedDst(const rhi::UpscalerInput& in, PxRect& out) {
    const rhi::PrimaryVisibility& v = in.visibility;
    if (!v.buffer || !v.viewport[2] || !v.viewport[3] || !in.srcWidth || !in.srcHeight || !in.dstWidth || !in.dstHeight)
        return false;
    if (v.viewport[0] + v.viewport[2] > in.srcWidth || v.viewport[1] + v.viewport[3] > in.srcHeight) return false;
    const u64 sw = in.srcWidth, sh = in.srcHeight, dw = in.dstWidth, dh = in.dstHeight;
    PxRect r;
    r.x0 = static_cast<u32>(v.viewport[0] * dw / sw);
    r.y0 = static_cast<u32>(v.viewport[1] * dh / sh);
    r.x1 = static_cast<u32>(((v.viewport[0] + v.viewport[2]) * dw + sw - 1) / sw);
    r.y1 = static_cast<u32>(((v.viewport[1] + v.viewport[3]) * dh + sh - 1) / sh);
    out = growRect(r, kShowPad, in.dstWidth, in.dstHeight);
    return out.x0 > 0 || out.y0 > 0 || out.x1 < in.dstWidth || out.y1 < in.dstHeight;
}

// The source texels a destination rect can read, plus `pad`, clamped to the source.
inline PxRect srcReadOf(PxRect d, const rhi::UpscalerInput& in, u32 pad) {
    const u64 sw = in.srcWidth, sh = in.srcHeight, dw = in.dstWidth, dh = in.dstHeight;
    PxRect s;
    s.x0 = static_cast<u32>(d.x0 * sw / dw);
    s.y0 = static_cast<u32>(d.y0 * sh / dh);
    s.x1 = static_cast<u32>((d.x1 * sw + dw - 1) / dw);
    s.y1 = static_cast<u32>((d.y1 * sh + dh - 1) / dh);
    return growRect(s, pad, in.srcWidth, in.srcHeight);
}

} // namespace aver::sr
