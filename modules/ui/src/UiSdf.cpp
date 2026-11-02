// UiSdf: coverage bitmap to signed-distance bitmap with an exact Euclidean distance transform. See UiSdf.hpp.
#include "aver/ui/UiSdf.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace aver::ui {
namespace {

constexpr f32 kInf = 1.0e20f;

// Felzenszwalb & Huttenlocher's 1D squared-distance transform of f over n samples, in place via scratch buffers.
void edt1d(const f32* f, f32* d, i32* v, f32* z, i32 n) {
    i32 k = 0;
    v[0] = 0;
    z[0] = -kInf;
    z[1] = kInf;
    for (i32 q = 1; q < n; ++q) {
        f32 s;
        for (;;) {
            const i32 p = v[k];
            s = ((f[q] + static_cast<f32>(q) * static_cast<f32>(q)) - (f[p] + static_cast<f32>(p) * static_cast<f32>(p))) /
                (2.0f * static_cast<f32>(q) - 2.0f * static_cast<f32>(p));
            if (s <= z[k] && k > 0) { --k; continue; }
            break;
        }
        ++k;
        v[k] = q;
        z[k] = s;
        z[k + 1] = kInf;
    }
    k = 0;
    for (i32 q = 0; q < n; ++q) {
        while (z[k + 1] < static_cast<f32>(q)) ++k;
        const f32 dx = static_cast<f32>(q - v[k]);
        d[q] = dx * dx + f[v[k]];
    }
}

// Squared distance from every cell to the nearest cell where `seed` is true (0 on a seed).
void edt2d(const std::vector<u8>& seed, i32 w, i32 h, std::vector<f32>& out) {
    out.assign(static_cast<usize>(w) * h, 0.0f);
    const i32 m = std::max(w, h);
    std::vector<f32> f(static_cast<usize>(m)), d(static_cast<usize>(m)), z(static_cast<usize>(m) + 1);
    std::vector<i32> v(static_cast<usize>(m));
    for (usize i = 0; i < out.size(); ++i) out[i] = seed[i] ? 0.0f : kInf;
    for (i32 x = 0; x < w; ++x) {   // columns
        for (i32 y = 0; y < h; ++y) f[static_cast<usize>(y)] = out[static_cast<usize>(y) * w + x];
        edt1d(f.data(), d.data(), v.data(), z.data(), h);
        for (i32 y = 0; y < h; ++y) out[static_cast<usize>(y) * w + x] = d[static_cast<usize>(y)];
    }
    for (i32 y = 0; y < h; ++y) {   // rows
        for (i32 x = 0; x < w; ++x) f[static_cast<usize>(x)] = out[static_cast<usize>(y) * w + x];
        edt1d(f.data(), d.data(), v.data(), z.data(), w);
        for (i32 x = 0; x < w; ++x) out[static_cast<usize>(y) * w + x] = d[static_cast<usize>(x)];
    }
}

} // namespace

bool uiBitmapToSdf(const UiGlyphBitmap& in, u32 spread, UiGlyphBitmap& out) {
    if (spread == 0 || spread > kUiSdfMaxSpread) return false;
    if (in.w > kUiSdfMaxEdge || in.h > kUiSdfMaxEdge) return false;
    if (in.w == 0 || in.h == 0) {
        if (in.pixels.size() != 0 && in.w != 0 && in.h != 0) return false;
        out = UiGlyphBitmap{};
        out.offX = in.offX;
        out.offY = in.offY;
        return true;
    }
    if (in.pixels.size() < static_cast<usize>(in.w) * in.h) return false;

    const i32 s = static_cast<i32>(spread);
    const i32 w = static_cast<i32>(in.w) + 2 * s, h = static_cast<i32>(in.h) + 2 * s;
    std::vector<u8> cov(static_cast<usize>(w) * h, 0);
    for (i32 y = 0; y < in.h; ++y)
        for (i32 x = 0; x < in.w; ++x) cov[static_cast<usize>(y + s) * w + (x + s)] = in.pixels[static_cast<usize>(y) * in.w + x];

    std::vector<u8> inside(cov.size()), outside(cov.size());
    for (usize i = 0; i < cov.size(); ++i) {
        inside[i] = cov[i] >= 128;
        outside[i] = cov[i] < 128;
    }
    std::vector<f32> dToInside, dToOutside;   // squared distances to the nearest inside / outside texel centre
    edt2d(inside, w, h, dToInside);
    edt2d(outside, w, h, dToOutside);

    out = UiGlyphBitmap{};
    out.w = static_cast<u16>(w);
    out.h = static_cast<u16>(h);
    out.offX = static_cast<i16>(in.offX - s);
    out.offY = static_cast<i16>(in.offY - s);
    out.pixels.assign(cov.size(), 0);
    const f32 inv = 127.0f / static_cast<f32>(spread);
    for (usize i = 0; i < cov.size(); ++i) {
        // Signed distance to the contour, positive outside, in pixels: centre distance less half a texel, then the
        // antialiased edge texels placed by their own coverage.
        f32 d;
        if (cov[i] >= 128) d = -(std::sqrt(std::min(dToOutside[i], kInf)) - 0.5f);
        else               d = std::sqrt(std::min(dToInside[i], kInf)) - 0.5f;
        if (cov[i] > 0 && cov[i] < 255) d = 0.5f - static_cast<f32>(cov[i]) / 255.0f;
        const f32 v = 128.0f - d * inv;
        out.pixels[i] = static_cast<u8>(std::clamp(std::lround(v), 0l, 255l));
    }
    return true;
}

f32 uiSdfCoverage(u8 texel, f32 softness) {
    const f32 e0 = 0.5f - softness, e1 = 0.5f + softness;
    const f32 t = std::clamp((static_cast<f32>(texel) / 255.0f - e0) / std::max(e1 - e0, 1.0e-6f), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

} // namespace aver::ui
