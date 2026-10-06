// UiTree drawing: every widget kind, through a UiPainter.
#include "aver/ui/UiTree.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aver::ui {

namespace {

constexpr f32 kThumbDp = 14.0f;
constexpr f32 kBarDp = 6.0f;

std::vector<std::string_view> splitLines(std::string_view text) {
    std::vector<std::string_view> out;
    usize start = 0;
    while (true) {
        const usize nl = text.find('\n', start);
        if (nl == std::string_view::npos) { out.push_back(text.substr(start)); break; }
        out.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
}

std::string formatValue(const UiWidget& w) {
    char buf[32];
    const f32 range = w.maxValue - w.minValue;
    if (w.minValue == 0.0f && w.maxValue <= 1.0001f) std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(w.value * 100.0f)));
    else if (w.step >= 1.0f || range >= 20.0f) std::snprintf(buf, sizeof buf, "%.0f", static_cast<double>(w.value));
    else std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(w.value));
    return buf;
}

// Draws `lines` as a block inside `r`, aligned horizontally by `align`, centred vertically.
void drawBlock(UiPainter& p, const UiRect& r, const std::vector<std::string_view>& lines, f32 px, UiTextAlign align,
               u32 colour, bool centreVertically) {
    const f32 lineH = p.lineHeight(px), asc = p.ascent(px);
    const f32 block = lineH * static_cast<f32>(lines.size());
    f32 y = r.y + (centreVertically ? (r.h - block) * 0.5f : 0.0f);
    for (const std::string_view line : lines) {
        const f32 tw = p.textWidth(line, px);
        f32 x = r.x;
        if (align == UiTextAlign::Center) x = r.x + (r.w - tw) * 0.5f;
        else if (align == UiTextAlign::Right) x = r.right() - tw;
        p.text(x, y + asc, line, px, colour);
        y += lineH;
    }
}

} // namespace

void UiTree::draw(UiPainter& p) {
    for (const UiWidgetId id : sortedRoots()) {
        const UiWidget* r = get(id);
        if (!r || !r->visible) continue;
        p.setLayer(r->layer);
        if (r->modal) p.fillRect(viewport_, theme_.dim);
        drawWidget(p, *r, 1.0f, true);
    }
}

void UiTree::drawScrollbar(UiPainter& p, const UiWidget& w, const UiStyle& st, f32 opacity) {
    const UiRect inner = w.rect.deflate(paddingOf(w, st));
    if (w.contentSize.y <= inner.h + 0.5f || inner.h <= 0.0f) return;
    const f32 span = w.contentSize.y - inner.h;
    const f32 thumbH = std::max(24.0f * scale_, inner.h * inner.h / w.contentSize.y);
    const f32 t = span > 0.0f ? std::clamp(w.scrollY / span, 0.0f, 1.0f) : 0.0f;
    const f32 bw = theme_.scrollbarWidth * scale_;
    const u32 base = st.text ? st.text : uiRgba(255, 255, 255);
    p.fillRect({w.rect.right() - bw - 2.0f * scale_, inner.y + t * (inner.h - thumbH), bw, thumbH},
               uiWithOpacity(uiWithOpacity(base, 0.35f), opacity));
}

void UiTree::drawWidget(UiPainter& p, const UiWidget& w, f32 parentOpacity, bool parentEnabled) {
    if (!w.visible) return;
    const f32 op = parentOpacity * w.opacity;
    if (op <= 0.003f) return;
    const bool enabled = parentEnabled && w.enabled;

    const UiStyle st = styleOf(w);
    const f32 s = scale_;
    const f32 px = fontPx(w, st);
    const UiRect r = w.rect;
    const UiRect inner = r.deflate(paddingOf(w, st));
    const f32 lineH = p.lineHeight(px), asc = p.ascent(px);

    UiVisualState vs = UiVisualState::Normal;
    if (!enabled) vs = UiVisualState::Disabled;
    else if (w.pressed && w.hovered) vs = UiVisualState::Pressed;
    else if (w.hovered || (w.focused && focusVisible_)) vs = UiVisualState::Hover;

    const auto C = [op](u32 c) { return uiWithOpacity(c, op); };
    u32 bg = st.background[static_cast<usize>(vs)];
    if (w.bgColor) {
        bg = w.bgColor;
        if (vs == UiVisualState::Hover) bg = uiShade(bg, 0.15f);
        else if (vs == UiVisualState::Pressed) bg = uiShade(bg, -0.2f);
        else if (vs == UiVisualState::Disabled) bg = uiWithOpacity(bg, 0.5f);
    }
    u32 textCol = w.textColor ? w.textColor : st.text;
    if (!enabled) textCol = w.textColor ? uiWithOpacity(w.textColor, 0.5f) : (st.textDisabled ? st.textDisabled : textCol);
    const u32 accent = w.accentColor ? w.accentColor : st.accent;

    const auto fillBg = [&](const UiRect& rect) { if (uiAlphaOf(bg) > 0) p.fillRect(rect, C(bg)); };
    const auto border = [&](const UiRect& rect) {
        if (st.borderWidth > 0.0f && uiAlphaOf(st.border) > 0) p.strokeRect(rect, st.borderWidth * s, C(st.border));
    };

    bool clipChildren = w.clip;

    switch (w.kind) {
        case UiWidgetKind::Panel:
            fillBg(r);
            border(r);
            break;

        case UiWidgetKind::Text: {
            if (w.text.empty()) break;
            std::vector<std::string> wrapped;
            std::vector<std::string_view> lines;
            if (w.wrap) {
                wrapped = uiWrapText(p, w.text, px, inner.w);
                for (const std::string& l : wrapped) lines.push_back(l);
            } else {
                lines = splitLines(w.text);
            }
            drawBlock(p, inner, lines, px, w.textAlign, C(textCol), !w.wrap);
            break;
        }

        case UiWidgetKind::Image: {
            UiRect dst = r;
            if (w.imageMode == UiImageMode::Fit && w.width > 0.0f && w.height > 0.0f && !r.empty()) {
                const f32 aspect = w.width / w.height;
                const f32 fitW = std::min(r.w, r.h * aspect);
                const f32 fitH = fitW / aspect;
                dst = {r.x + (r.w - fitW) * 0.5f, r.y + (r.h - fitH) * 0.5f, fitW, fitH};
            }
            if (w.texture != 0) {
                p.image(dst, w.texture, w.uv, C(w.tint));
            } else if (w.image.empty()) {
                p.fillRect(dst, C(w.tint));                       // a plain tinted rectangle
            } else {
                p.fillRect(dst, C(uiRgba(120, 120, 130, 90)));    // a path the host has not resolved
                p.strokeRect(dst, s, C(uiRgba(160, 160, 170, 200)));
            }
            break;
        }

        case UiWidgetKind::Button:
        case UiWidgetKind::KeyBind: {
            fillBg(r);
            border(r);
            std::string shown = w.text;
            if (w.kind == UiWidgetKind::KeyBind && w.listening) shown = w.placeholder.empty() ? "Press a key..." : w.placeholder;
            const std::vector<std::string_view> lines = splitLines(shown);
            drawBlock(p, inner, lines, px, w.textAlign, C(w.listening ? accent : textCol), true);
            break;
        }

        case UiWidgetKind::Toggle: {
            const f32 box = px * 1.1f;
            const UiRect b{inner.x, inner.y + (inner.h - box) * 0.5f, box, box};
            fillBg(b);
            border(b);
            if (w.checked) {
                const f32 pad = std::max(2.0f, box * 0.22f);
                p.fillRect({b.x + pad, b.y + pad, box - 2 * pad, box - 2 * pad}, C(accent));
            }
            if (!w.text.empty()) {
                const f32 tx = b.right() + 8.0f * s;
                p.text(tx, inner.y + (inner.h - lineH) * 0.5f + asc, w.text, px, C(textCol));
            }
            break;
        }

        case UiWidgetKind::Slider: {
            const UiRect track = sliderTrack(w);
            const f32 half = kThumbDp * s * 0.5f;
            const f32 usable = std::max(0.0f, track.w - 2.0f * half);
            const f32 range = w.maxValue - w.minValue;
            const f32 t = range > 0.0f ? std::clamp((w.value - w.minValue) / range, 0.0f, 1.0f) : 0.0f;
            const f32 barH = kBarDp * s;
            const UiRect bar{track.x + half, track.y + (track.h - barH) * 0.5f, usable, barH};
            p.fillRect(bar, C(st.track ? st.track : bg));
            p.fillRect({bar.x, bar.y, bar.w * t, bar.h}, C(accent));
            const UiRect thumb{track.x + t * usable, track.y + track.h * 0.12f, 2.0f * half, track.h * 0.76f};
            u32 thumbCol = enabled ? accent : st.textDisabled;
            if (vs == UiVisualState::Hover) thumbCol = uiShade(thumbCol, 0.25f);
            else if (vs == UiVisualState::Pressed) thumbCol = uiShade(thumbCol, -0.2f);
            p.fillRect(thumb, C(thumbCol));
            if (w.showValue) {
                const std::string v = formatValue(w);
                const f32 tw = p.textWidth(v, px);
                p.text(inner.right() - tw, inner.y + (inner.h - lineH) * 0.5f + asc, v, px, C(textCol));
            }
            break;
        }

        case UiWidgetKind::Choice: {
            fillBg(r);
            border(r);
            const std::string shown = (w.selected >= 0 && static_cast<usize>(w.selected) < w.items.size())
                                          ? w.items[static_cast<usize>(w.selected)] : std::string();
            drawBlock(p, inner, {shown}, px, UiTextAlign::Center, C(textCol), true);
            const f32 base = r.y + (r.h - lineH) * 0.5f + asc;
            p.text(r.x + 10.0f * s, base, "<", px, C(accent));
            p.text(r.right() - 10.0f * s - p.textWidth(">", px), base, ">", px, C(accent));
            break;
        }

        case UiWidgetKind::List: {
            fillBg(r);
            border(r);
            clipChildren = true;
            p.pushClip(inner);
            const f32 rh = rowHeightOf(w);
            const i32 n = static_cast<i32>(w.items.size());
            if (rh > 0.0f) {
                const i32 first = std::max(0, static_cast<i32>(std::floor(w.scrollY / rh)));
                const i32 last = std::min(n, static_cast<i32>(std::ceil((w.scrollY + inner.h) / rh)));
                for (i32 i = first; i < last; ++i) {
                    const UiRect row{inner.x, inner.y + static_cast<f32>(i) * rh - w.scrollY, inner.w, rh};
                    if (i == w.selected) p.fillRect(row, C(uiWithOpacity(accent, 0.35f)));
                    else if (i == w.hotItem) p.fillRect(row, C(uiWithOpacity(accent, 0.15f)));
                    p.text(row.x + 8.0f * s, row.y + (rh - lineH) * 0.5f + asc, w.items[static_cast<usize>(i)], px, C(textCol));
                }
            }
            p.popClip();
            drawScrollbar(p, w, st, op);
            break;
        }

        case UiWidgetKind::Scroll:
            clipChildren = true;
            break;

        case UiWidgetKind::TextInput: {
            fillBg(r);
            if (w.focused) p.strokeRect(r, 2.0f * s, C(accent));
            else border(r);
            const UiRect area = inner;
            p.pushClip(area);
            const std::string shown = displayText(w);
            const f32 base = area.y + (area.h - lineH) * 0.5f + asc;
            if (shown.empty() && !w.placeholder.empty() && !w.focused) {
                p.text(area.x, base, w.placeholder, px, C(st.placeholder ? st.placeholder : textCol));
            } else {
                const f32 shift = textShift(w, p);
                p.text(area.x - shift, base, shown, px, C(textCol));
                if (w.focused && std::fmod(time_, 1.0f) < 0.55f) {
                    const usize off = uiUtf8Offset(shown, static_cast<usize>(std::max(0, w.caret)));
                    const f32 cx = area.x - shift + p.textWidth(std::string_view(shown).substr(0, off), px);
                    p.fillRect({cx, area.y + (area.h - lineH) * 0.5f, std::max(1.0f, s), lineH}, C(textCol));
                }
            }
            p.popClip();
            break;
        }

        case UiWidgetKind::ProgressBar: {
            p.fillRect(r, C(st.track ? st.track : bg));
            const f32 range = w.maxValue - w.minValue;
            const f32 t = range > 0.0f ? std::clamp((w.value - w.minValue) / range, 0.0f, 1.0f) : 0.0f;
            p.fillRect({r.x, r.y, r.w * t, r.h}, C(accent));
            const std::string label = w.showValue ? formatValue(w) : w.text;
            if (!label.empty()) drawBlock(p, inner, {label}, px, UiTextAlign::Center, C(textCol), true);
            break;
        }

        case UiWidgetKind::Count:
            break;
    }

    if (clipChildren) p.pushClip(r);
    for (const UiWidgetId c : w.children)
        if (const UiWidget* k = get(c)) drawWidget(p, *k, op, enabled);
    if (clipChildren) p.popClip();

    if (w.kind == UiWidgetKind::Scroll) drawScrollbar(p, w, st, op);
    if (w.focused && focusVisible_ && enabled && w.focusable && theme_.focusWidth > 0.0f)
        p.strokeRect(r, theme_.focusWidth * s, C(theme_.focusColor));
}

} // namespace aver::ui
