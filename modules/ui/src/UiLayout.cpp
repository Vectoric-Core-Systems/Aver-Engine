// Layout maths. See UiLayout.hpp.
#include "aver/ui/UiLayout.hpp"

#include <algorithm>
#include <cmath>

namespace aver::ui {

f32 uiClampSize(f32 v, f32 lo, f32 hi) {
    if (hi > 0.0f && v > hi) v = hi;
    if (v < lo) v = lo;
    return v;
}

f32 uiComputeScale(const UiDpi& dpi, f32 vw, f32 vh) {
    f32 base = dpi.platformScale;
    if (vw > 0.0f && vh > 0.0f && dpi.refWidth > 0.0f && dpi.refHeight > 0.0f) {
        const f32 rw = vw / dpi.refWidth, rh = vh / dpi.refHeight;
        switch (dpi.mode) {
            case UiScaleMode::Constant:     base = dpi.platformScale; break;
            case UiScaleMode::Width:        base = rw; break;
            case UiScaleMode::Height:       base = rh; break;
            case UiScaleMode::ShortestSide: base = std::min(rw, rh); break;
            case UiScaleMode::Blend:        base = std::exp2(0.5f * std::log2(rw) + 0.5f * std::log2(rh)); break;
        }
    }
    const f32 s = base * dpi.userScale;
    return std::clamp(s, 0.25f, 8.0f);
}

namespace {

UiSpan resolveAxis(f32 pMin, f32 pSize, f32 aMin, f32 aMax, f32 offA, f32 offB, f32 pivot, f32 size,
                   f32 scale) {
    const f32 a0 = pMin + aMin * pSize;
    const f32 a1 = pMin + aMax * pSize;
    if (aMax - aMin > 1e-5f) {
        const f32 pos = a0 + offA * scale;
        const f32 end = a1 - offB * scale;
        return {pos, std::max(0.0f, end - pos)};
    }
    return {a0 + offA * scale - pivot * size, size};
}

} // namespace

UiRect uiResolveAnchored(const UiRect& parent, const UiAnchors& a, const UiInsets& o, UiVec2 pivot,
                         f32 width, f32 height, f32 scale) {
    const UiSpan sx = resolveAxis(parent.x, parent.w, a.minX, a.maxX, o.left, o.right, pivot.x, width, scale);
    const UiSpan sy = resolveAxis(parent.y, parent.h, a.minY, a.maxY, o.top, o.bottom, pivot.y, height, scale);
    return {sx.pos, sy.pos, sx.size, sy.size};
}

f32 uiAxisContent(const std::vector<UiAxisItem>& items, f32 spacing) {
    if (items.empty()) return 0.0f;
    f32 total = spacing * static_cast<f32>(items.size() - 1);
    for (const UiAxisItem& it : items)
        total += uiClampSize(it.desired, it.minSize, it.maxSize) + it.before + it.after;
    return total;
}

std::vector<UiSpan> uiSolveAxis(const std::vector<UiAxisItem>& items, f32 available, f32 spacing,
                                UiJustify justify) {
    const usize n = items.size();
    std::vector<UiSpan> out(n);
    if (n == 0) return out;

    std::vector<f32> size(n, 0.0f);
    std::vector<bool> fill(n, false), frozen(n, false);
    f32 fixedTotal = spacing * static_cast<f32>(n - 1);
    usize fillCount = 0;
    for (usize i = 0; i < n; ++i) {
        const UiAxisItem& it = items[i];
        fixedTotal += it.before + it.after;
        if (it.fill > 0.0f) { fill[i] = true; ++fillCount; }
        else { size[i] = uiClampSize(it.desired, it.minSize, it.maxSize); fixedTotal += size[i]; }
    }

    // Flex distribution: share the remainder by weight; any item pushed outside its min/max is
    // frozen at the bound and the rest re-share what is left.
    f32 remaining = std::max(0.0f, available - fixedTotal);
    for (usize pass = 0; pass <= n && fillCount > 0; ++pass) {
        f32 weight = 0.0f;
        for (usize i = 0; i < n; ++i) if (fill[i] && !frozen[i]) weight += items[i].fill;
        if (weight <= 0.0f) break;

        bool violated = false;
        for (usize i = 0; i < n; ++i) {
            if (!fill[i] || frozen[i]) continue;
            const f32 share = remaining * items[i].fill / weight;
            const f32 clamped = uiClampSize(share, items[i].minSize, items[i].maxSize);
            if (clamped != share) {
                size[i] = clamped;
                frozen[i] = true;
                violated = true;
            }
        }
        if (violated) {
            remaining = std::max(0.0f, available - fixedTotal);
            for (usize i = 0; i < n; ++i) if (fill[i] && frozen[i]) remaining = std::max(0.0f, remaining - size[i]);
            continue;
        }
        for (usize i = 0; i < n; ++i)
            if (fill[i] && !frozen[i]) size[i] = remaining * items[i].fill / weight;
        break;
    }

    f32 used = fixedTotal;
    for (usize i = 0; i < n; ++i) if (fill[i]) used += size[i];
    const f32 leftover = std::max(0.0f, available - used);

    f32 cursor = 0.0f, gap = spacing;
    switch (justify) {
        case UiJustify::Start: break;
        case UiJustify::Center: cursor = leftover * 0.5f; break;
        case UiJustify::End: cursor = leftover; break;
        case UiJustify::SpaceBetween: if (n > 1) gap += leftover / static_cast<f32>(n - 1); break;
    }
    for (usize i = 0; i < n; ++i) {
        cursor += items[i].before;
        out[i] = {cursor, size[i]};
        cursor += size[i] + items[i].after + gap;
    }
    return out;
}

UiSpan uiAlignInSlot(f32 slotPos, f32 slotSize, f32 desired, UiAlign align, f32 minSize, f32 maxSize) {
    if (align == UiAlign::Stretch)
        return {slotPos, uiClampSize(slotSize, minSize, maxSize)};
    const f32 size = uiClampSize(desired, minSize, maxSize);
    if (align == UiAlign::Center) return {slotPos + (slotSize - size) * 0.5f, size};
    if (align == UiAlign::End) return {slotPos + slotSize - size, size};
    return {slotPos, size};
}

f32 uiClampScroll(f32 offset, f32 content, f32 viewport) {
    const f32 maxOff = std::max(0.0f, content - viewport);
    return std::clamp(offset, 0.0f, maxOff);
}

f32 uiScrollIntoView(f32 offset, f32 viewport, f32 itemPos, f32 itemSize) {
    if (itemPos < offset) return itemPos;
    if (itemPos + itemSize > offset + viewport) return itemPos + itemSize - viewport;
    return offset;
}

} // namespace aver::ui
