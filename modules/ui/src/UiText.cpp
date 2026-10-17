// Text measurement, wrapping and the draw-list painter.
#include "aver/ui/UiText.hpp"
#include "aver/ui/UiTextEngine.hpp"
#include "aver/ui/UiUnicode.hpp"

#include <algorithm>
#include <cmath>

namespace aver::ui {

namespace {

bool isContinuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

bool isRefChar(char c, bool key) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
           (key && (c == '.' || c == '-'));
}

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

bool uiParseTextRef(std::string_view s, UiTextRef& out) {
    if (s.size() < 4 || s[0] != '@' || s[1] == '@') return false;
    usize i = 1;
    while (i < s.size() && isRefChar(s[i], false)) ++i;
    if (i == 1 || i >= s.size() || s[i] != ':') return false;
    const usize tableEnd = i++;
    const usize keyStart = i;
    while (i < s.size() && isRefChar(s[i], true)) ++i;
    if (i == keyStart || (i < s.size() && s[i] != '=')) return false;
    out.table = std::string(s.substr(1, tableEnd - 1));
    out.key = std::string(s.substr(keyStart, i - keyStart));
    out.hasDefault = i < s.size();
    out.defaultText = out.hasDefault ? std::string(s.substr(i + 1)) : std::string();
    return true;
}

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

// ---- the legacy layout ------------------------------------------------------------------------

// Lines from uiWrapText (or the newlines) and the three metrics virtuals: today's measure and draw maths
// expressed as a layout, so the tree has one path for the baked font, the estimate and the engine.
std::shared_ptr<const UiTextLayout> UiTextMetrics::layoutText(std::string_view text, const UiTextStyle& style,
                                                              const UiTextBox& box) const {
    auto out = std::make_shared<UiTextLayout>();
    UiTextLayout& L = *out;
    const f32 px = style.px;
    L.legacy = true;
    L.px = px;
    L.source.assign(text);
    {
        std::vector<u32> unused;
        uiDecodeUtf8(L.source, unused, &L.cpToByte);
    }

    if (box.wrap && box.maxWidth > 0.0f) {
        L.legacyLines = uiWrapText(*this, text, px, box.maxWidth);
    } else {
        usize start = 0;
        while (true) {
            const usize nl = text.find('\n', start);
            if (nl == std::string_view::npos) { L.legacyLines.emplace_back(text.substr(start)); break; }
            L.legacyLines.emplace_back(text.substr(start, nl - start));
            start = nl + 1;
        }
    }

    const f32 lineH = lineHeight(px), asc = ascent(px);
    u32 cpStart = 0;
    for (usize i = 0; i < L.legacyLines.size(); ++i) {
        const std::string& line = L.legacyLines[i];
        UiTextLine tl;
        tl.width = textWidth(line, px);
        tl.top = lineH * static_cast<f32>(i);
        tl.height = lineH;
        tl.ascent = asc;
        tl.descent = asc - lineH;
        tl.baseline = asc;
        tl.srcBegin = cpStart;
        const u32 n = static_cast<u32>(uiUtf8Length(line));
        tl.srcEnd = cpStart + n;
        if (box.carets) {
            tl.stopBegin = static_cast<u32>(L.stops.size());
            for (u32 k = 0; k <= n; ++k) {
                const usize off = uiUtf8Offset(line, k);
                L.stops.push_back(UiCaretStop{cpStart + k, textWidth(std::string_view(line).substr(0, off), px)});
            }
            tl.stopEnd = static_cast<u32>(L.stops.size());
        }
        L.width = std::max(L.width, tl.width);
        L.lines.push_back(tl);
        cpStart = tl.srcEnd + 1;
    }
    L.height = lineH * static_cast<f32>(L.legacyLines.size());
    return out;
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

void UiPainter::drawText(const UiTextLayout& L, const UiRect& area, UiTextAlign align, bool centreVertically,
                         u32 rgba) {
    const f32 px = L.px;
    if (L.legacy) {
        // The old drawBlock maths, line for line.
        const f32 lineH = lineHeight(px), asc = ascent(px);
        const f32 block = lineH * static_cast<f32>(L.lines.size());
        f32 y = area.y + (centreVertically ? (area.h - block) * 0.5f : 0.0f);
        for (usize i = 0; i < L.lines.size(); ++i) {
            const std::string_view line = L.lineText(i);
            const f32 tw = textWidth(line, px);
            text(uiAlignedX(align, area, tw, false), y + asc, line, px, rgba);
            y += lineH;
        }
        return;
    }
    // A layout from the engine on a painter that cannot place glyphs: paint each line as a string.
    const f32 y0 = area.y + (centreVertically ? (area.h - L.height) * 0.5f : 0.0f);
    for (usize i = 0; i < L.lines.size(); ++i) {
        const UiTextLine& line = L.lines[i];
        text(uiAlignedX(align, area, line.width, line.rtl), y0 + line.top + line.baseline, L.lineText(i), px, rgba);
    }
}

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

UiDrawListPainter::UiDrawListPainter(UiDrawList& list, UiTextEngine* engine, const UiFont* bitmap)
    : list_(list), font_(bitmap), engine_(engine), fontMetrics_(bitmap) {}

bool UiDrawListPainter::engineOn() const { return engine_ && engine_->usable(); }

void UiDrawListPainter::pushClip(const UiRect& r) {
    list_.pushClip(UiClip{static_cast<i32>(std::floor(r.x)), static_cast<i32>(std::floor(r.y)),
                          static_cast<i32>(std::ceil(r.right())), static_cast<i32>(std::ceil(r.bottom()))});
}

void UiDrawListPainter::image(const UiRect& r, u64 texture, const UiUvRect& uv, u32 tint) {
    list_.addTexturedRect(r.x, r.y, r.w, r.h, texture, uv.u0, uv.v0, uv.u1, uv.v1, tint);
}

void UiDrawListPainter::text(f32 x, f32 baselineY, std::string_view s, f32 px, u32 rgba) {
    if (engineOn()) {
        UiTextStyle st;
        st.px = px;
        const std::shared_ptr<const UiTextLayout> L = engine_->layout(s, st, UiTextBox{});
        if (L->lines.empty()) return;
        const f32 base0 = L->lines[0].top + L->lines[0].baseline;
        for (usize i = 0; i < L->lines.size(); ++i)
            engine_->emitLine(list_, *L, i, x, baselineY + (L->lines[i].top + L->lines[i].baseline - base0), rgba);
        return;
    }
    if (!font_ || !font_->valid()) return;
    const f32 scale = font_->pixelSize > 0.0f ? px / font_->pixelSize : 1.0f;
    list_.addTextScaled(x, baselineY, s, *font_, rgba, scale);
}

void UiDrawListPainter::drawText(const UiTextLayout& L, const UiRect& area, UiTextAlign align,
                                 bool centreVertically, u32 rgba) {
    if (!engineOn() || L.legacy) { UiPainter::drawText(L, area, align, centreVertically, rgba); return; }
    const f32 y0 = area.y + (centreVertically ? (area.h - L.height) * 0.5f : 0.0f);
    for (usize i = 0; i < L.lines.size(); ++i) {
        const UiTextLine& line = L.lines[i];
        engine_->emitLine(list_, L, i, uiAlignedX(align, area, line.width, line.rtl), y0 + line.top + line.baseline, rgba);
    }
}

std::shared_ptr<const UiTextLayout> UiDrawListPainter::layoutText(std::string_view text, const UiTextStyle& style,
                                                                  const UiTextBox& box) const {
    if (engineOn()) return engine_->layout(text, style, box);
    return UiTextMetrics::layoutText(text, style, box);
}

f32 UiDrawListPainter::textWidth(std::string_view t, f32 px) const {
    if (engineOn()) return engine_->textWidth(t, px);
    return (font_ && font_->valid()) ? fontMetrics_.textWidth(t, px) : estimate_.textWidth(t, px);
}

f32 UiDrawListPainter::lineHeight(f32 px) const {
    if (engineOn()) return engine_->lineHeight(px);
    return (font_ && font_->valid()) ? fontMetrics_.lineHeight(px) : estimate_.lineHeight(px);
}

f32 UiDrawListPainter::ascent(f32 px) const {
    if (engineOn()) return engine_->ascent(px);
    return (font_ && font_->valid()) ? fontMetrics_.ascent(px) : estimate_.ascent(px);
}

} // namespace aver::ui
