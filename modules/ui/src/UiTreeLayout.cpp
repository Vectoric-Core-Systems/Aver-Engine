// UiTree layout: measure bottom-up (content size), arrange top-down (final rectangles).
#include "aver/ui/UiTree.hpp"

#include <algorithm>
#include <cmath>
#include <string_view>

namespace aver::ui {

namespace {

struct GridTracks {
    std::vector<UiAxisItem> cols, rows;
};

// Column and row items for a grid, shared by measure and arrange so they cannot disagree.
GridTracks gridTracks(const UiWidget& w, const std::vector<UiWidget*>& kids, f32 s) {
    const usize cols = static_cast<usize>(std::max(1, w.columns));
    const usize rows = (kids.size() + cols - 1) / cols;
    GridTracks t;
    t.cols.assign(cols, UiAxisItem{});
    t.rows.assign(rows, UiAxisItem{});
    for (usize i = 0; i < kids.size(); ++i) {
        const UiWidget& k = *kids[i];
        UiAxisItem& c = t.cols[i % cols];
        UiAxisItem& r = t.rows[i / cols];
        c.desired = std::max(c.desired, k.desired.x + k.margin.horizontal() * s);
        c.fill = std::max(c.fill, k.fillW);
        r.desired = std::max(r.desired, k.desired.y + k.margin.vertical() * s);
        r.fill = std::max(r.fill, k.fillH);
    }
    if (w.uniform) {
        f32 mc = 0.0f, mr = 0.0f;
        for (const UiAxisItem& c : t.cols) mc = std::max(mc, c.desired);
        for (const UiAxisItem& r : t.rows) mr = std::max(mr, r.desired);
        for (UiAxisItem& c : t.cols) { c.desired = mc; c.fill = std::max(c.fill, 1.0f); }
        for (UiAxisItem& r : t.rows) { r.desired = mr; r.fill = std::max(r.fill, 1.0f); }
    }
    if (w.cellWidth >= 0.0f) for (UiAxisItem& c : t.cols) c.desired = w.cellWidth * s;
    if (w.cellHeight >= 0.0f) for (UiAxisItem& r : t.rows) r.desired = w.cellHeight * s;
    return t;
}

struct WrapLine {
    usize first = 0, count = 0;
    f32 width = 0, height = 0;
};

// Greedy line breaking for a Wrap: a child goes on the current line while it fits `availW` (<= 0 = unbounded).
std::vector<WrapLine> wrapLines(const std::vector<UiWidget*>& kids, f32 availW, f32 spX, f32 s) {
    std::vector<WrapLine> lines;
    WrapLine cur;
    for (usize i = 0; i < kids.size(); ++i) {
        const UiWidget& k = *kids[i];
        const f32 iw = k.desired.x + k.margin.horizontal() * s, ih = k.desired.y + k.margin.vertical() * s;
        if (cur.count > 0 && availW > 0.0f && cur.width + spX + iw > availW + 0.01f) {
            lines.push_back(cur);
            cur = WrapLine{};
        }
        if (cur.count == 0) cur.first = i;
        cur.width += (cur.count > 0 ? spX : 0.0f) + iw;
        cur.height = std::max(cur.height, ih);
        ++cur.count;
    }
    if (cur.count > 0) lines.push_back(cur);
    return lines;
}

// The height of a layout's first line: what one-line widgets size to. A legacy layout reports the
// metrics' own line height, so this is today's lineH for the baked font and the estimate.
f32 firstLineH(const UiTextLayout& l, f32 fallback) {
    return l.lines.empty() ? fallback : std::max(fallback, l.lines[0].height);
}

} // namespace

const UiTheme& UiTree::activeTheme() const {
    if (!access_.highContrast) return theme_;
    if (!hcValid_) {
        hcTheme_ = uiBuiltinTheme("contrast");
        hcTheme_.baseFontSize = theme_.baseFontSize;     // the same metrics as the game's theme, only the look changes
        hcTheme_.rowHeight = theme_.rowHeight;
        hcTheme_.scrollbarWidth = theme_.scrollbarWidth;
        hcValid_ = true;
    }
    return hcTheme_;
}

UiStyle UiTree::styleOf(const UiWidget& w) const { return activeTheme().resolve(w.kind, w.style); }

// Text scale is text only: row heights, padding and widths stay on the UI scale.
f32 UiTree::fontPx(const UiWidget& w, const UiStyle& st) const {
    const f32 dp = w.fontSize > 0.0f ? w.fontSize : (st.fontSize > 0.0f ? st.fontSize : theme_.baseFontSize);
    return dp * scaleOf(w) * access_.textScale;
}

UiTextStyle UiTree::textStyleOf(const UiWidget& w, const UiStyle& st) const {
    UiTextStyle s;
    s.family = !w.fontFamily.empty() ? std::string_view(w.fontFamily) : std::string_view(st.fontFamily);
    s.px = fontPx(w, st);
    s.scale = scaleOf(w);
    s.bold = w.bold || st.bold > 0;
    s.italic = w.italic;
    s.dir = w.textDir;
    s.lineSpacing = w.lineSpacing > 0.0f ? w.lineSpacing : 1.0f;
    s.letterSpacing = w.letterSpacing * scaleOf(w);
    return s;
}

UiTextBox UiTree::textBoxOf(const UiWidget& w, f32 maxWidth) const {
    UiTextBox b;
    b.maxWidth = maxWidth;
    b.wrap = w.wrap && w.kind == UiWidgetKind::Text;
    b.ellipsis = w.ellipsis;
    b.maxLines = w.maxLines > 0 ? static_cast<u32>(w.maxLines) : 0u;
    b.rich = w.rich && w.kind != UiWidgetKind::TextInput;
    b.richColors = !access_.highContrast;   // authored colours are ignored under high contrast
    return b;
}

bool UiTree::lastTextSize(UiWidgetId id, UiVec2& size, i32& lines) const {
    const auto it = textLayouts_.find(id);
    if (it == textLayouts_.end() || !it->second || !get(id)) return false;
    size = {it->second->width, it->second->height};
    lines = static_cast<i32>(it->second->lines.size());
    return true;
}

UiInsets UiTree::paddingOf(const UiWidget& w, const UiStyle& st) const {
    const f32 s = scaleOf(w);
    return {(w.padding.left + st.padding.left) * s, (w.padding.top + st.padding.top) * s,
            (w.padding.right + st.padding.right) * s, (w.padding.bottom + st.padding.bottom) * s};
}

f32 UiTree::rowHeightOf(const UiWidget& w) const {
    return (w.rowHeight > 0.0f ? w.rowHeight : theme_.rowHeight) * scaleOf(w);
}

UiRect UiTree::contentRect(UiWidgetId id) const {
    const UiWidget* w = get(id);
    return w ? w->rect.deflate(paddingOf(*w, styleOf(*w))) : UiRect{};
}

UiRect UiTree::sliderTrack(const UiWidget& w) const {
    const UiStyle st = styleOf(w);
    UiRect inner = w.rect.deflate(paddingOf(w, st));
    if (w.showValue) inner.w = std::max(0.0f, inner.w - fontPx(w, st) * 3.4f);
    return inner;
}

std::vector<UiWidget*> UiTree::visibleChildren(UiWidget& w) {
    std::vector<UiWidget*> out;
    out.reserve(w.children.size());
    const bool switcher = w.layout == UiLayoutMode::Switcher;
    i32 index = 0;
    for (const UiWidgetId c : w.children) {
        UiWidget* k = get(c);
        const i32 at = index++;
        if (!k) continue;
        k->inactive = switcher && at != w.activeIndex;
        if (k->visible && !k->inactive) out.push_back(k);
    }
    return out;
}

UiVec2 UiTree::measure(UiWidget& w) {
    w.scale = scale_;
    const UiStyle st = styleOf(w);
    const f32 s = scale_;
    const f32 px = fontPx(w, st);
    const UiInsets pad = paddingOf(w, st);
    const UiTextMetrics& m = *metrics_;
    const f32 lineH = m.lineHeight(px);

    std::vector<UiWidget*> kids = visibleChildren(w);
    for (UiWidget* k : kids) measure(*k);

    // One layout per string, shared by what this measures and what draw paints. A fixed width is the wrap
    // (Text) or ellipsis (any textual widget) width.
    f32 fixedW = w.width >= 0.0f ? std::max(0.0f, w.width * s - pad.horizontal()) : 0.0f;
    // A wrapped Text that takes its width from a fill slot wraps at the width its last arrangement gave it.
    if (w.width < 0.0f && w.kind == UiWidgetKind::Text && w.wrap && w.fillW > 0.0f) fixedW = w.wrapHint;
    const UiTextStyle tstyle = textStyleOf(w, st);
    const auto lay = [&](const std::string& txt, f32 maxW) { return m.layoutText(txt, tstyle, textBoxOf(w, maxW)); };

    f32 cw = 0.0f, ch = 0.0f;
    switch (w.kind) {
        case UiWidgetKind::Text:
        case UiWidgetKind::Button:
        case UiWidgetKind::KeyBind: {
            const std::shared_ptr<const UiTextLayout> l = lay(shown(w.text), fixedW);
            cw = l->width;
            ch = l->height;
            textLayouts_[w.id] = l;
            break;
        }
        case UiWidgetKind::Toggle: {
            const f32 box = px * 1.1f;
            const std::string& txt = shown(w.text);
            const std::shared_ptr<const UiTextLayout> l = lay(txt, 0.0f);
            cw = box + (txt.empty() ? 0.0f : 8.0f * s + l->width);
            ch = std::max(box, firstLineH(*l, lineH));
            textLayouts_[w.id] = l;
            break;
        }
        case UiWidgetKind::Slider:
            cw = 180.0f * s;
            ch = std::max(lineH, 22.0f * s);
            break;
        case UiWidgetKind::Choice: {
            f32 line = lineH;
            for (const std::string& it : w.items) {
                const std::shared_ptr<const UiTextLayout> l = lay(shown(it), 0.0f);
                cw = std::max(cw, l->width);
                line = firstLineH(*l, line);
            }
            ch = line;
            break;
        }
        case UiWidgetKind::List: {
            for (const std::string& it : w.items) cw = std::max(cw, lay(shown(it), 0.0f)->width);
            cw = std::max(cw, 160.0f * s);
            const f32 rows = static_cast<f32>(std::clamp<usize>(w.items.size(), 1, 6));
            ch = rowHeightOf(w) * rows;
            break;
        }
        case UiWidgetKind::TextInput:
            cw = 200.0f * s;
            ch = firstLineH(*lay(std::string(), 0.0f), lineH);
            break;
        case UiWidgetKind::ProgressBar:
            cw = 200.0f * s;
            ch = std::max(lineH, 16.0f * s);
            break;
        case UiWidgetKind::Image:
        case UiWidgetKind::Panel:
        case UiWidgetKind::Scroll:
        case UiWidgetKind::Count:
            break;
    }

    if (w.layout == UiLayoutMode::VStack || w.layout == UiLayoutMode::HStack) {
        const bool vertical = w.layout == UiLayoutMode::VStack;
        f32 main = 0.0f, cross = 0.0f;
        for (UiWidget* k : kids) {
            const f32 km = vertical ? k->desired.y + k->margin.vertical() * s : k->desired.x + k->margin.horizontal() * s;
            const f32 kc = vertical ? k->desired.x + k->margin.horizontal() * s : k->desired.y + k->margin.vertical() * s;
            main += km;
            cross = std::max(cross, kc);
        }
        if (!kids.empty()) main += w.spacing * s * static_cast<f32>(kids.size() - 1);
        cw = std::max(cw, vertical ? cross : main);
        ch = std::max(ch, vertical ? main : cross);
    } else if (w.layout == UiLayoutMode::Grid && !kids.empty()) {
        const GridTracks t = gridTracks(w, kids, s);
        const f32 spY = (w.spacingY >= 0.0f ? w.spacingY : w.spacing) * s;
        cw = std::max(cw, uiAxisContent(t.cols, w.spacing * s));
        ch = std::max(ch, uiAxisContent(t.rows, spY));
    } else if (w.layout == UiLayoutMode::Overlay || w.layout == UiLayoutMode::Switcher) {
        for (UiWidget* k : kids) {
            cw = std::max(cw, k->desired.x + k->margin.horizontal() * s);
            ch = std::max(ch, k->desired.y + k->margin.vertical() * s);
        }
    } else if (w.layout == UiLayoutMode::Wrap && !kids.empty()) {
        // Rows break at the fixed width, else at the width the last arrangement gave (second layout pass).
        f32 availW = 0.0f;
        if (w.width >= 0.0f) availW = std::max(0.0f, w.width * s - pad.horizontal());
        else if (w.wrapHint > 0.0f) availW = w.wrapHint;
        if (w.maxWidth > 0.0f) {
            const f32 cap = std::max(0.0f, w.maxWidth * s - pad.horizontal());
            availW = availW > 0.0f ? std::min(availW, cap) : cap;
        }
        const std::vector<WrapLine> lines = wrapLines(kids, availW, w.spacing * s, s);
        const f32 spY = (w.spacingY >= 0.0f ? w.spacingY : w.spacing) * s;
        f32 maxLine = 0.0f, total = 0.0f;
        for (const WrapLine& l : lines) { maxLine = std::max(maxLine, l.width); total += l.height; }
        if (!lines.empty()) total += spY * static_cast<f32>(lines.size() - 1);
        cw = std::max(cw, maxLine);
        ch = std::max(ch, total);
    }
    w.contentDesired = {cw, ch};
    if (!kids.empty() && w.scaleFit == UiScaleFit::None && w.contentScale > 0.0f && w.contentScale != 1.0f) {
        cw *= w.contentScale;
        ch *= w.contentScale;
    }

    f32 dw = w.width >= 0.0f ? w.width * s : cw + pad.horizontal();
    f32 dh = w.height >= 0.0f ? w.height * s : ch + pad.vertical();
    dw = uiClampSize(dw, w.minWidth * s, w.maxWidth * s);
    dh = uiClampSize(dh, w.minHeight * s, w.maxHeight * s);
    w.desired = {dw, dh};
    return w.desired;
}

UiRect UiTree::placeAnchored(const UiWidget& c, const UiRect& parent) const {
    const f32 s = scale_;
    UiRect r = uiResolveAnchored(parent, c.anchors, c.offsets, c.pivot, c.desired.x, c.desired.y, s);
    r.w = uiClampSize(r.w, c.minWidth * s, c.maxWidth * s);
    r.h = uiClampSize(r.h, c.minHeight * s, c.maxHeight * s);
    return r;
}

void UiTree::translate(UiWidget& w, f32 dx, f32 dy) {
    w.rect.x += dx;
    w.rect.y += dy;
    for (const UiWidgetId c : w.children) if (UiWidget* k = get(c)) translate(*k, dx, dy);
}

void UiTree::arrangeStack(UiWidget& w, const UiRect& inner, bool vertical, const std::vector<UiWidget*>& kids) {
    const f32 s = scale_;
    std::vector<UiAxisItem> items;
    items.reserve(kids.size());
    for (const UiWidget* k : kids) {
        UiAxisItem it;
        it.desired = vertical ? k->desired.y : k->desired.x;
        it.fill = vertical ? k->fillH : k->fillW;
        it.minSize = (vertical ? k->minHeight : k->minWidth) * s;
        it.maxSize = (vertical ? k->maxHeight : k->maxWidth) * s;
        it.before = (vertical ? k->margin.top : k->margin.left) * s;
        it.after = (vertical ? k->margin.bottom : k->margin.right) * s;
        items.push_back(it);
    }
    const f32 spacing = w.spacing * s;
    f32 availMain = vertical ? inner.h : inner.w;
    // A scroll container lays out in the room its content needs; the viewport then shows part of it.
    if (w.kind == UiWidgetKind::Scroll) availMain = std::max(availMain, uiAxisContent(items, spacing));
    const std::vector<UiSpan> spans = uiSolveAxis(items, availMain, spacing, w.justify);

    const f32 crossStart = vertical ? inner.x : inner.y;
    const f32 crossAvail = vertical ? inner.w : inner.h;
    const f32 mainStart = vertical ? inner.y : inner.x;
    for (usize i = 0; i < kids.size(); ++i) {
        UiWidget& k = *kids[i];
        const UiAlign align = vertical ? k.alignH : k.alignV;
        const bool fixedCross = vertical ? k.width >= 0.0f : k.height >= 0.0f;
        const f32 crossDesired = vertical ? k.desired.x : k.desired.y;
        const f32 mBefore = (vertical ? k.margin.left : k.margin.top) * s;
        const f32 mAfter = (vertical ? k.margin.right : k.margin.bottom) * s;
        const f32 minC = (vertical ? k.minWidth : k.minHeight) * s;
        const f32 maxC = (vertical ? k.maxWidth : k.maxHeight) * s;
        const UiSpan cs = uiAlignInSlot(crossStart + mBefore, std::max(0.0f, crossAvail - mBefore - mAfter),
                                        crossDesired, (align == UiAlign::Stretch && fixedCross) ? UiAlign::Start : align,
                                        minC, maxC);
        const UiRect r = vertical ? UiRect{cs.pos, mainStart + spans[i].pos, cs.size, spans[i].size}
                                  : UiRect{mainStart + spans[i].pos, cs.pos, spans[i].size, cs.size};
        arrange(k, r);
    }
}

void UiTree::arrangeGrid(UiWidget& w, const UiRect& inner, const std::vector<UiWidget*>& kids) {
    if (kids.empty()) return;
    const f32 s = scale_;
    const usize cols = static_cast<usize>(std::max(1, w.columns));
    const GridTracks t = gridTracks(w, kids, s);
    const f32 spX = w.spacing * s;
    const f32 spY = (w.spacingY >= 0.0f ? w.spacingY : w.spacing) * s;
    f32 availH = inner.h;
    if (w.kind == UiWidgetKind::Scroll) availH = std::max(availH, uiAxisContent(t.rows, spY));
    const std::vector<UiSpan> cs = uiSolveAxis(t.cols, inner.w, spX, w.justify);
    const std::vector<UiSpan> rs = uiSolveAxis(t.rows, availH, spY, UiJustify::Start);

    for (usize i = 0; i < kids.size(); ++i) {
        UiWidget& k = *kids[i];
        const UiSpan& c = cs[i % cols];
        const UiSpan& r = rs[i / cols];
        const f32 mL = k.margin.left * s, mR = k.margin.right * s, mT = k.margin.top * s, mB = k.margin.bottom * s;
        const UiAlign ah = (k.alignH == UiAlign::Stretch && k.width >= 0.0f) ? UiAlign::Start : k.alignH;
        const UiAlign av = (k.alignV == UiAlign::Stretch && k.height >= 0.0f) ? UiAlign::Start : k.alignV;
        const UiSpan x = uiAlignInSlot(inner.x + c.pos + mL, std::max(0.0f, c.size - mL - mR), k.desired.x, ah,
                                       k.minWidth * s, k.maxWidth * s);
        const UiSpan y = uiAlignInSlot(inner.y + r.pos + mT, std::max(0.0f, r.size - mT - mB), k.desired.y, av,
                                       k.minHeight * s, k.maxHeight * s);
        arrange(k, UiRect{x.pos, y.pos, x.size, y.size});
    }
}

void UiTree::finishScroll(UiWidget& w, const UiRect& inner, const std::vector<UiWidget*>& kids) {
    const f32 s = scale_;
    if (w.kind == UiWidgetKind::List) {
        w.contentSize = {inner.w, rowHeightOf(w) * static_cast<f32>(w.items.size())};
        w.scrollX = 0.0f;
        w.scrollY = uiClampScroll(w.scrollY, w.contentSize.y, inner.h);
        return;
    }
    f32 maxR = inner.x, maxB = inner.y;
    for (const UiWidget* k : kids) {
        maxR = std::max(maxR, k->rect.right() + k->margin.right * s);
        maxB = std::max(maxB, k->rect.bottom() + k->margin.bottom * s);
    }
    w.contentSize = {std::max(0.0f, maxR - inner.x), std::max(0.0f, maxB - inner.y)};
    w.scrollX = uiClampScroll(w.scrollX, w.contentSize.x, inner.w);
    w.scrollY = uiClampScroll(w.scrollY, w.contentSize.y, inner.h);
    if (w.scrollX != 0.0f || w.scrollY != 0.0f)
        for (UiWidget* k : kids) translate(*k, -w.scrollX, -w.scrollY);
}

void UiTree::arrangeOverlay(const UiRect& inner, const std::vector<UiWidget*>& kids) {
    const f32 s = scale_;
    for (UiWidget* kp : kids) {
        UiWidget& k = *kp;
        const f32 mL = k.margin.left * s, mR = k.margin.right * s, mT = k.margin.top * s, mB = k.margin.bottom * s;
        const UiAlign ah = (k.alignH == UiAlign::Stretch && k.width >= 0.0f) ? UiAlign::Start : k.alignH;
        const UiAlign av = (k.alignV == UiAlign::Stretch && k.height >= 0.0f) ? UiAlign::Start : k.alignV;
        const UiSpan x = uiAlignInSlot(inner.x + mL, std::max(0.0f, inner.w - mL - mR), k.desired.x, ah, k.minWidth * s, k.maxWidth * s);
        const UiSpan y = uiAlignInSlot(inner.y + mT, std::max(0.0f, inner.h - mT - mB), k.desired.y, av, k.minHeight * s, k.maxHeight * s);
        arrange(k, UiRect{x.pos, y.pos, x.size, y.size});
    }
}

void UiTree::arrangeWrap(UiWidget& w, const UiRect& inner, const std::vector<UiWidget*>& kids) {
    const f32 s = scale_;
    const f32 spX = w.spacing * s, spY = (w.spacingY >= 0.0f ? w.spacingY : w.spacing) * s;
    const std::vector<WrapLine> lines = wrapLines(kids, inner.w, spX, s);
    f32 y = inner.y;
    for (const WrapLine& line : lines) {
        const f32 leftover = std::max(0.0f, inner.w - line.width);
        f32 cursor = inner.x, gap = spX;
        if (w.justify == UiJustify::Center) cursor += leftover * 0.5f;
        else if (w.justify == UiJustify::End) cursor += leftover;
        else if (w.justify == UiJustify::SpaceBetween && line.count > 1) gap += leftover / static_cast<f32>(line.count - 1);
        for (usize j = 0; j < line.count; ++j) {
            UiWidget& k = *kids[line.first + j];
            const f32 mL = k.margin.left * s, mR = k.margin.right * s, mT = k.margin.top * s, mB = k.margin.bottom * s;
            const UiAlign av = (k.alignV == UiAlign::Stretch && k.height >= 0.0f) ? UiAlign::Start : k.alignV;
            const UiSpan sy = uiAlignInSlot(y + mT, std::max(0.0f, line.height - mT - mB), k.desired.y, av, k.minHeight * s, k.maxHeight * s);
            arrange(k, UiRect{cursor + mL, sy.pos, k.desired.x, sy.size});
            cursor += mL + k.desired.x + mR + gap;
        }
        y += line.height + spY;
    }
}

f32 UiTree::contentFactor(const UiWidget& w, const UiRect& inner) const {
    const f32 user = w.contentScale > 0.0f ? w.contentScale : 1.0f;
    if (w.scaleFit == UiScaleFit::None) return user;
    const f32 cw = w.contentDesired.x, ch = w.contentDesired.y;
    if (cw <= 0.0f || ch <= 0.0f) return 1.0f;
    const f32 fx = inner.w / cw, fy = inner.h / ch;
    f32 f = 1.0f;
    switch (w.scaleFit) {
        case UiScaleFit::Contain: f = std::min(fx, fy); break;
        case UiScaleFit::Cover: f = std::max(fx, fy); break;
        case UiScaleFit::Down: f = std::min(1.0f, std::min(fx, fy)); break;
        case UiScaleFit::FitWidth: f = fx; break;
        case UiScaleFit::FitHeight: f = fy; break;
        case UiScaleFit::None: break;
    }
    return std::clamp(f, 0.01f, 100.0f);
}

void UiTree::arrange(UiWidget& w, const UiRect& rect) {
    w.rect = rect;
    w.scale = scale_;
    const UiRect inner = rect.deflate(paddingOf(w, styleOf(w)));
    std::vector<UiWidget*> kids = visibleChildren(w);

    // A scaled subtree is re-measured and laid out at the new design-pixel scale, so text, padding and borders scale too.
    const f32 outerScale = scale_;
    UiRect area = inner;
    const f32 f = kids.empty() ? 1.0f : contentFactor(w, inner);
    if (std::fabs(f - 1.0f) > 1e-4f) {
        scale_ = outerScale * f;
        for (UiWidget* k : kids) measure(*k);
    }
    if (!kids.empty() && w.scaleFit != UiScaleFit::None && w.contentDesired.x > 0.0f && w.contentDesired.y > 0.0f) {
        const f32 aw = w.contentDesired.x * f, ah = w.contentDesired.y * f;
        area = UiRect{inner.x + (inner.w - aw) * 0.5f, inner.y + (inner.h - ah) * 0.5f, aw, ah};
    }
    switch (w.layout) {
        case UiLayoutMode::VStack: arrangeStack(w, area, true, kids); break;
        case UiLayoutMode::HStack: arrangeStack(w, area, false, kids); break;
        case UiLayoutMode::Grid: arrangeGrid(w, area, kids); break;
        case UiLayoutMode::Overlay:
        case UiLayoutMode::Switcher: arrangeOverlay(area, kids); break;
        case UiLayoutMode::Wrap: arrangeWrap(w, area, kids); break;
        case UiLayoutMode::None:
            for (UiWidget* k : kids) arrange(*k, placeAnchored(*k, area));
            break;
    }
    if (w.kind == UiWidgetKind::Scroll || w.kind == UiWidgetKind::List) finishScroll(w, inner, kids);
    scale_ = outerScale;
}

void UiTree::layout(const UiTextMetrics& m) {
    syncText();
    textLayouts_.clear();
    const UiTextMetrics* previous = metrics_;
    metrics_ = &m;
    scale_ = uiComputeScale(dpi_, viewport_.w, viewport_.h);
    for (int pass = 0; pass < 2; ++pass) {
        for (const UiWidgetId id : roots_) {
            UiWidget* r = get(id);
            if (!r || !r->visible) continue;
            measure(*r);
            arrange(*r, placeAnchored(*r, viewport_));
        }
        // Second pass only when a wrapped fill-width label learned a new width (first frame, resize, scale change).
        bool moved = false;
        for (auto& kv : widgets_) {
            UiWidget& w = *kv.second;
            if (!w.visible || w.inactive || w.width >= 0.0f) continue;
            const bool wrappedText = w.kind == UiWidgetKind::Text && w.wrap && w.fillW > 0.0f;
            if (!wrappedText && w.layout != UiLayoutMode::Wrap) continue;
            const f32 inner = std::max(0.0f, w.rect.w - paddingOf(w, styleOf(w)).horizontal());
            if (std::fabs(inner - w.wrapHint) > 0.5f) { w.wrapHint = inner; moved = true; }
        }
        if (!moved) break;
    }
    metrics_ = previous;
}

} // namespace aver::ui
