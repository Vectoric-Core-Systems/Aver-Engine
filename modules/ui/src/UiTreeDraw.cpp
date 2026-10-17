// UiTree drawing: every widget kind, through a UiPainter.
#include "aver/ui/UiTree.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace aver::ui {

namespace {

constexpr f32 kThumbDp = 14.0f;
constexpr f32 kBarDp = 6.0f;

std::string formatValue(const UiWidget& w) {
    char buf[32];
    const f32 range = w.maxValue - w.minValue;
    if (w.minValue == 0.0f && w.maxValue <= 1.0001f) std::snprintf(buf, sizeof buf, "%d%%", static_cast<int>(std::lround(w.value * 100.0f)));
    else if (w.step >= 1.0f || range >= 20.0f) std::snprintf(buf, sizeof buf, "%.0f", static_cast<double>(w.value));
    else std::snprintf(buf, sizeof buf, "%.2f", static_cast<double>(w.value));
    return buf;
}

} // namespace

void UiTree::draw(UiPainter& p) {
    syncText();
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

    // Lays `txt` out with this widget's text props and paints it inside `area`. The layout comes from the
    // painter, so a widget is drawn with the same metrics that measured it.
    const auto drawStr = [&](const std::string& txt, const UiTextBox& box, const UiRect& area, UiTextAlign align,
                             bool centreV, u32 colour) {
        const std::shared_ptr<const UiTextLayout> l = p.layoutText(txt, textStyleOf(w, st), box);
        p.drawText(*l, area, align, centreV, colour);
    };
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
            const std::string& txt = shown(w.text);
            if (txt.empty()) break;
            drawStr(txt, textBoxOf(w, inner.w), inner, w.textAlign, !w.wrap, C(textCol));
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
            std::string label = shown(w.text);
            if (w.kind == UiWidgetKind::KeyBind && w.listening) {
                static const std::string kPrompt = "@UI:controls.pressKey=Press a key...";
                label = shown(w.placeholder.empty() ? kPrompt : w.placeholder);
            }
            drawStr(label, textBoxOf(w, inner.w), inner, w.textAlign, true, C(w.listening ? accent : textCol));
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
            const std::string& txt = shown(w.text);
            if (!txt.empty()) {
                const f32 tx = b.right() + 8.0f * s;
                drawStr(txt, textBoxOf(w, 0.0f), UiRect{tx, inner.y, std::max(0.0f, inner.right() - tx), inner.h},
                        UiTextAlign::Left, true, C(textCol));
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
                drawStr(formatValue(w), UiTextBox{}, inner, UiTextAlign::Right, true, C(textCol));
            }
            break;
        }

        case UiWidgetKind::Choice: {
            fillBg(r);
            border(r);
            const std::string sel = (w.selected >= 0 && static_cast<usize>(w.selected) < w.items.size())
                                        ? shown(w.items[static_cast<usize>(w.selected)]) : std::string();
            drawStr(sel, textBoxOf(w, inner.w), inner, UiTextAlign::Center, true, C(textCol));
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
                    const f32 inset = 8.0f * s;
                    drawStr(shown(w.items[static_cast<usize>(i)]), textBoxOf(w, std::max(0.0f, row.w - inset)),
                            UiRect{row.x + inset, row.y, std::max(0.0f, row.w - inset), rh}, UiTextAlign::Left, true,
                            C(textCol));
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
            const std::string typed = displayText(w);
            if (typed.empty() && !w.placeholder.empty() && !w.focused) {
                drawStr(shown(w.placeholder), UiTextBox{}, area, UiTextAlign::Left, true,
                        C(st.placeholder ? st.placeholder : textCol));
            } else {
                const std::shared_ptr<const UiTextLayout> l = inputLayout(w, p);
                const f32 shift = textShift(w, *l);
                p.drawText(*l, UiRect{area.x - shift, area.y, area.w + shift, area.h}, UiTextAlign::Left, true,
                           C(textCol));
                if (w.focused && std::fmod(time_, 1.0f) < 0.55f) {
                    const f32 cx = area.x - shift + l->caretX(static_cast<usize>(std::max(0, w.caret)));
                    const f32 caretH = l->lines.empty() ? lineH : l->lines[0].height;
                    p.fillRect({cx, area.y + (area.h - l->height) * 0.5f, std::max(1.0f, s), caretH}, C(textCol));
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
            const std::string label = w.showValue ? formatValue(w) : shown(w.text);
            if (!label.empty()) drawStr(label, UiTextBox{}, inner, UiTextAlign::Center, true, C(textCol));
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
