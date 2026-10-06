// UiTree layout: measure bottom-up (content size), arrange top-down (final rectangles).
#include "aver/ui/UiTree.hpp"

#include <algorithm>

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
    if (w.cellWidth >= 0.0f) for (UiAxisItem& c : t.cols) c.desired = w.cellWidth * s;
    if (w.cellHeight >= 0.0f) for (UiAxisItem& r : t.rows) r.desired = w.cellHeight * s;
    return t;
}

usize lineCount(std::string_view text) {
    usize n = 1;
    for (const char c : text) if (c == '\n') ++n;
    return n;
}

} // namespace

UiStyle UiTree::styleOf(const UiWidget& w) const { return theme_.resolve(w.kind, w.style); }

f32 UiTree::fontPx(const UiWidget& w, const UiStyle& st) const {
    const f32 dp = w.fontSize > 0.0f ? w.fontSize : (st.fontSize > 0.0f ? st.fontSize : theme_.baseFontSize);
    return dp * scale_;
}

UiInsets UiTree::paddingOf(const UiWidget& w, const UiStyle& st) const {
    return {(w.padding.left + st.padding.left) * scale_, (w.padding.top + st.padding.top) * scale_,
            (w.padding.right + st.padding.right) * scale_, (w.padding.bottom + st.padding.bottom) * scale_};
}

f32 UiTree::rowHeightOf(const UiWidget& w) const {
    return (w.rowHeight > 0.0f ? w.rowHeight : theme_.rowHeight) * scale_;
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
    for (const UiWidgetId c : w.children) {
        UiWidget* k = get(c);
        if (k && k->visible) out.push_back(k);
    }
    return out;
}

UiVec2 UiTree::measure(UiWidget& w) {
    const UiStyle st = styleOf(w);
    const f32 s = scale_;
    const f32 px = fontPx(w, st);
    const UiInsets pad = paddingOf(w, st);
    const UiTextMetrics& m = *metrics_;
    const f32 lineH = m.lineHeight(px);

    std::vector<UiWidget*> kids = visibleChildren(w);
    for (UiWidget* k : kids) measure(*k);

    f32 cw = 0.0f, ch = 0.0f;
    switch (w.kind) {
        case UiWidgetKind::Text: {
            const f32 wrapW = (w.wrap && w.width >= 0.0f) ? std::max(0.0f, w.width * s - pad.horizontal()) : 0.0f;
            if (wrapW > 0.0f) {
                const std::vector<std::string> lines = uiWrapText(m, w.text, px, wrapW);
                for (const std::string& l : lines) cw = std::max(cw, m.textWidth(l, px));
                ch = lineH * static_cast<f32>(lines.size());
            } else {
                cw = m.textWidth(w.text, px);
                ch = lineH * static_cast<f32>(lineCount(w.text));
            }
            break;
        }
        case UiWidgetKind::Button:
        case UiWidgetKind::KeyBind:
            cw = m.textWidth(w.text, px);
            ch = lineH * static_cast<f32>(lineCount(w.text));
            break;
        case UiWidgetKind::Toggle: {
            const f32 box = px * 1.1f;
            cw = box + (w.text.empty() ? 0.0f : 8.0f * s + m.textWidth(w.text, px));
            ch = std::max(box, lineH);
            break;
        }
        case UiWidgetKind::Slider:
            cw = 180.0f * s;
            ch = std::max(lineH, 22.0f * s);
            break;
        case UiWidgetKind::Choice:
            for (const std::string& it : w.items) cw = std::max(cw, m.textWidth(it, px));
            ch = lineH;
            break;
        case UiWidgetKind::List: {
            for (const std::string& it : w.items) cw = std::max(cw, m.textWidth(it, px));
            cw = std::max(cw, 160.0f * s);
            const f32 rows = static_cast<f32>(std::clamp<usize>(w.items.size(), 1, 6));
            ch = rowHeightOf(w) * rows;
            break;
        }
        case UiWidgetKind::TextInput:
            cw = 200.0f * s;
            ch = lineH;
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

void UiTree::arrange(UiWidget& w, const UiRect& rect) {
    w.rect = rect;
    const UiRect inner = rect.deflate(paddingOf(w, styleOf(w)));
    std::vector<UiWidget*> kids = visibleChildren(w);
    switch (w.layout) {
        case UiLayoutMode::VStack: arrangeStack(w, inner, true, kids); break;
        case UiLayoutMode::HStack: arrangeStack(w, inner, false, kids); break;
        case UiLayoutMode::Grid: arrangeGrid(w, inner, kids); break;
        case UiLayoutMode::None:
            for (UiWidget* k : kids) arrange(*k, placeAnchored(*k, inner));
            break;
    }
    if (w.kind == UiWidgetKind::Scroll || w.kind == UiWidgetKind::List) finishScroll(w, inner, kids);
}

void UiTree::layout(const UiTextMetrics& m) {
    const UiTextMetrics* previous = metrics_;
    metrics_ = &m;
    scale_ = uiComputeScale(dpi_, viewport_.w, viewport_.h);
    for (const UiWidgetId id : roots_) {
        UiWidget* r = get(id);
        if (!r || !r->visible) continue;
        measure(*r);
        arrange(*r, placeAnchored(*r, viewport_));
    }
    metrics_ = previous;
}

} // namespace aver::ui
