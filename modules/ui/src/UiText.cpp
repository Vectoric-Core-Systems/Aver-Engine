// Text measurement, wrapping and the draw-list painter.
#include "aver/ui/UiText.hpp"

#include <algorithm>
#include <cmath>

namespace aver::ui {

namespace {

bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

// Byte length of the code point starting at s[i].
usize cpLen(std::string_view s, usize i) {
    usize n = 1;
    while (i + n < s.size() && isContinuation(s[i + n])) ++n;
    return n;
}

void wrapParagraph(const UiTextMetrics& m, std::string_view para, f32 px, f32 maxWidth,
                   std::vector<std::string>& out) {
    if (maxWidth <= 0.0f || para.empty() || m.textWidth(para, px) <= maxWidth) {
        out.emplace_back(para);
        return;
    }
    std::string cur;
    usize i = 0;
    while (i < para.size()) {
        usize sp = 0;
        while (i < para.size() && para[i] == ' ') { ++sp; ++i; }
        const usize wordStart = i;
        while (i < para.size() && para[i] != ' ') ++i;
        const std::string_view word = para.substr(wordStart, i - wordStart);
        if (word.empty()) continue;

        const std::string candidate = cur.empty() ? std::string(word) : cur + std::string(sp ? sp : 1, ' ') + std::string(word);
        if (m.textWidth(candidate, px) <= maxWidth) { cur = candidate; continue; }

        if (!cur.empty()) { out.push_back(cur); cur.clear(); }
        if (m.textWidth(word, px) <= maxWidth) { cur = std::string(word); continue; }

        // A word wider than the line: break between code points, at least one per line.
        usize k = 0;
        std::string chunk;
        while (k < word.size()) {
            const usize n = cpLen(word, k);
            const std::string next = chunk + std::string(word.substr(k, n));
            if (!chunk.empty() && m.textWidth(next, px) > maxWidth) { out.push_back(chunk); chunk.clear(); }
            chunk += std::string(word.substr(k, n));
            k += n;
        }
        cur = chunk;
    }
    if (!cur.empty()) out.push_back(cur);
}

} // namespace

usize uiUtf8Length(std::string_view s) {
    usize n = 0;
    for (const char c : s) if (!isContinuation(c)) ++n;
    return n;
}

usize uiUtf8Offset(std::string_view s, usize index) {
    usize i = 0, cp = 0;
    while (i < s.size() && cp < index) { i += cpLen(s, i); ++cp; }
    return i;
}

void uiAppendUtf8(std::string& out, u32 cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::vector<std::string> uiWrapText(const UiTextMetrics& m, std::string_view text, f32 px, f32 maxWidth) {
    std::vector<std::string> lines;
    usize start = 0;
    while (true) {
        const usize nl = text.find('\n', start);
        const std::string_view para =
            nl == std::string_view::npos ? text.substr(start) : text.substr(start, nl - start);
        wrapParagraph(m, para, px, maxWidth, lines);
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    return lines;
}

// ---- metrics ----------------------------------------------------------------------------------

f32 UiEstimatedMetrics::textWidth(std::string_view text, f32 px) const {
    usize widest = 0, line = 0;
    for (const char c : text) {
        if (c == '\n') { widest = std::max(widest, line); line = 0; continue; }
        if (!isContinuation(c)) ++line;
    }
    widest = std::max(widest, line);
    return static_cast<f32>(widest) * px * 0.55f;
}

f32 UiFontMetrics::scaleFor(f32 px) const {
    return (font_ && font_->pixelSize > 0.0f) ? px / font_->pixelSize : 1.0f;
}

f32 UiFontMetrics::textWidth(std::string_view text, f32 px) const {
    if (!font_ || !font_->valid()) return 0.0f;
    return uiTextWidth(*font_, text) * scaleFor(px);
}

f32 UiFontMetrics::lineHeight(f32 px) const {
    return (font_ && font_->valid()) ? font_->lineHeight * scaleFor(px) : px * 1.25f;
}

f32 UiFontMetrics::ascent(f32 px) const {
    return (font_ && font_->valid()) ? font_->ascent * scaleFor(px) : px * 0.95f;
}

// ---- painter ----------------------------------------------------------------------------------

void UiPainter::strokeRect(const UiRect& r, f32 t, u32 rgba) {
    if (r.empty() || t <= 0.0f) return;
    t = std::min(t, std::min(r.w, r.h) * 0.5f);
    fillRect({r.x, r.y, r.w, t}, rgba);
    fillRect({r.x, r.bottom() - t, r.w, t}, rgba);
    fillRect({r.x, r.y + t, t, r.h - 2 * t}, rgba);
    fillRect({r.right() - t, r.y + t, t, r.h - 2 * t}, rgba);
}

UiDrawListPainter::UiDrawListPainter(UiDrawList& list, const UiFont* font)
    : list_(list), font_(font), fontMetrics_(font) {}

void UiDrawListPainter::pushClip(const UiRect& r) {
    list_.pushClip(UiClip{static_cast<i32>(std::floor(r.x)), static_cast<i32>(std::floor(r.y)),
                          static_cast<i32>(std::ceil(r.right())), static_cast<i32>(std::ceil(r.bottom()))});
}

void UiDrawListPainter::image(const UiRect& r, u64 texture, const UiUvRect& uv, u32 tint) {
    list_.addTexturedRect(r.x, r.y, r.w, r.h, texture, uv.u0, uv.v0, uv.u1, uv.v1, tint);
}

void UiDrawListPainter::text(f32 x, f32 baselineY, std::string_view s, f32 px, u32 rgba) {
    if (!font_ || !font_->valid()) return;
    const f32 scale = font_->pixelSize > 0.0f ? px / font_->pixelSize : 1.0f;
    list_.addTextScaled(x, baselineY, s, *font_, rgba, scale);
}

f32 UiDrawListPainter::textWidth(std::string_view t, f32 px) const {
    return (font_ && font_->valid()) ? fontMetrics_.textWidth(t, px) : estimate_.textWidth(t, px);
}

f32 UiDrawListPainter::lineHeight(f32 px) const {
    return (font_ && font_->valid()) ? fontMetrics_.lineHeight(px) : estimate_.lineHeight(px);
}

f32 UiDrawListPainter::ascent(f32 px) const {
    return (font_ && font_->valid()) ? fontMetrics_.ascent(px) : estimate_.ascent(px);
}

} // namespace aver::ui
