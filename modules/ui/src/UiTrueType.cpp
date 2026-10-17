// The only translation unit that includes stb_truetype (Dear ImGui's vendored imstb_truetype.h 1.26,
// public domain). STBTT_STATIC keeps every stb symbol private to this file, so it cannot collide with
// ImGui's own copy in the Sandbox. UiFontFace's methods live here for the same reason: stb's types and
// functions are not visible anywhere else.
#include "aver/ui/UiFontFace.hpp"

#include <cmath>
#include <cstdlib>
#include <unordered_map>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4100 4244 4267 4456 4457 4505 4701 4702 4706)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#endif
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "imstb_truetype.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace aver::ui {

struct UiFontFace::Impl {
    UiFontBlob blob;
    stbtt_fontinfo info{};
    f32 invUpem = 0.0f;                 // 1 / unitsPerEm
    u32 upem = 0;
    int ascent = 0, descent = 0, gap = 0;   // font units
    UiKernSource kernSrc = UiKernSource::None;
    mutable std::unordered_map<u64, int> kernMemo;
};

UiFontFace::UiFontFace() = default;
UiFontFace::~UiFontFace() = default;
UiFontFace::UiFontFace(UiFontFace&&) noexcept = default;
UiFontFace& UiFontFace::operator=(UiFontFace&&) noexcept = default;

bool UiFontFace::open(const UiFontBlob& blob, i32 index, std::string* err) {
    impl_.reset();
    if (!uiValidateSfnt(blob.data, blob.size, index, err)) return false;

    auto m = std::make_unique<Impl>();
    m->blob = blob;
    const int off = stbtt_GetFontOffsetForIndex(blob.data, index);
    if (off < 0) {
        if (err) *err = "font collection index out of range";
        return false;
    }
    if (!stbtt_InitFont(&m->info, blob.data, off)) {
        if (err) *err = "stb_truetype could not read the font (no Unicode cmap or unsupported outlines)";
        return false;
    }

    const f32 unit = stbtt_ScaleForMappingEmToPixels(&m->info, 1.0f);
    if (!(unit > 0.0f)) {
        if (err) *err = "bad unitsPerEm";
        return false;
    }
    m->invUpem = unit;
    m->upem = static_cast<u32>(std::lround(1.0f / unit));

    stbtt_GetFontVMetrics(&m->info, &m->ascent, &m->descent, &m->gap);
    if (m->ascent == 0 && m->descent == 0) {
        int a = 0, d = 0, g = 0;
        if (stbtt_GetFontVMetricsOS2(&m->info, &a, &d, &g)) { m->ascent = a; m->descent = d; m->gap = g; }
    }
    m->kernSrc = m->info.gpos ? UiKernSource::Gpos : (m->info.kern ? UiKernSource::Kern : UiKernSource::None);
    impl_ = std::move(m);
    return true;
}

bool UiFontFace::valid() const { return impl_ != nullptr; }
u32 UiFontFace::glyphCount() const { return impl_ ? static_cast<u32>(impl_->info.numGlyphs) : 0; }
u32 UiFontFace::unitsPerEm() const { return impl_ ? impl_->upem : 0; }

u32 UiFontFace::glyphFor(u32 cp) const {
    if (!impl_ || cp > 0x10FFFF) return 0;
    const int g = stbtt_FindGlyphIndex(&impl_->info, static_cast<int>(cp));
    return g > 0 ? static_cast<u32>(g) : 0;
}

bool UiFontFace::isBlank(u32 glyph) const {
    return !impl_ || stbtt_IsGlyphEmpty(&impl_->info, static_cast<int>(glyph)) != 0;
}

f32 UiFontFace::advance(u32 glyph, f32 px) const {
    if (!impl_) return 0.0f;
    int adv = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&impl_->info, static_cast<int>(glyph), &adv, &lsb);
    return static_cast<f32>(adv) * px * impl_->invUpem;
}

f32 UiFontFace::kern(u32 g1, u32 g2, f32 px) const {
    if (!impl_ || impl_->kernSrc == UiKernSource::None || g1 == 0 || g2 == 0) return 0.0f;
    const u64 key = (static_cast<u64>(g1) << 32) | g2;
    int units;
    const auto it = impl_->kernMemo.find(key);
    if (it != impl_->kernMemo.end()) {
        units = it->second;
    } else {
        units = stbtt_GetGlyphKernAdvance(&impl_->info, static_cast<int>(g1), static_cast<int>(g2));
        impl_->kernMemo.emplace(key, units);
    }
    return static_cast<f32>(units) * px * impl_->invUpem;
}

UiKernSource UiFontFace::kernSource() const { return impl_ ? impl_->kernSrc : UiKernSource::None; }

bool UiFontFace::glyphBox(u32 glyph, i32& x0, i32& y0, i32& x1, i32& y1) const {
    if (!impl_) return false;
    int a = 0, b = 0, c = 0, d = 0;
    if (!stbtt_GetGlyphBox(&impl_->info, static_cast<int>(glyph), &a, &b, &c, &d)) return false;
    x0 = a; y0 = b; x1 = c; y1 = d;
    return true;
}

f32 UiFontFace::ascent(f32 px) const { return impl_ ? static_cast<f32>(impl_->ascent) * px * impl_->invUpem : 0.0f; }
f32 UiFontFace::descent(f32 px) const { return impl_ ? static_cast<f32>(impl_->descent) * px * impl_->invUpem : 0.0f; }
f32 UiFontFace::lineGap(f32 px) const { return impl_ ? static_cast<f32>(impl_->gap) * px * impl_->invUpem : 0.0f; }

bool UiFontFace::rasterize(u32 glyph, u32 rasterPx, UiGlyphBitmap& out) const {
    out = UiGlyphBitmap{};
    if (!impl_ || rasterPx == 0) return false;
    const f32 scale = stbtt_ScaleForMappingEmToPixels(&impl_->info, static_cast<f32>(rasterPx));
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    stbtt_GetGlyphBitmapBoxSubpixel(&impl_->info, static_cast<int>(glyph), scale, scale, 0.0f, 0.0f, &x0, &y0, &x1, &y1);
    const int w = x1 - x0, h = y1 - y0;
    if (w <= 0 || h <= 0) return true;                    // blank glyph
    if (w > 1024 || h > 1024) return false;               // a hostile or broken outline
    const int gw = w + 2, gh = h + 2;
    out.pixels.assign(static_cast<usize>(gw) * static_cast<usize>(gh), 0);
    stbtt_MakeGlyphBitmapSubpixel(&impl_->info, out.pixels.data() + gw + 1, w, h, gw, scale, scale, 0.0f, 0.0f,
                                  static_cast<int>(glyph));
    out.w = static_cast<u16>(gw);
    out.h = static_cast<u16>(gh);
    out.offX = static_cast<i16>(x0 - 1);
    out.offY = static_cast<i16>(y0 - 1);
    return true;
}

} // namespace aver::ui
