// Focus movement: Tab order and directional navigation.
#include "aver/ui/UiFocus.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace aver::ui {

namespace {

const UiFocusItem* findItem(const std::vector<UiFocusItem>& items, UiWidgetId id) {
    for (const UiFocusItem& it : items) if (it.id == id) return &it;
    return nullptr;
}

} // namespace

std::vector<UiWidgetId> uiTabOrder(const std::vector<UiFocusItem>& items) {
    std::vector<const UiFocusItem*> positive, zero;
    for (const UiFocusItem& it : items) {
        if (it.tabIndex > 0) positive.push_back(&it);
        else if (it.tabIndex == 0) zero.push_back(&it);
    }
    std::stable_sort(positive.begin(), positive.end(),
                     [](const UiFocusItem* a, const UiFocusItem* b) { return a->tabIndex < b->tabIndex; });
    std::vector<UiWidgetId> out;
    out.reserve(positive.size() + zero.size());
    for (const UiFocusItem* it : positive) out.push_back(it->id);
    for (const UiFocusItem* it : zero) out.push_back(it->id);
    return out;
}

UiWidgetId uiNextTab(const std::vector<UiFocusItem>& items, UiWidgetId current, bool backwards, bool wrap) {
    const std::vector<UiWidgetId> order = uiTabOrder(items);
    if (order.empty()) return 0;
    const auto it = std::find(order.begin(), order.end(), current);
    if (it == order.end()) return backwards ? order.back() : order.front();
    const usize i = static_cast<usize>(it - order.begin());
    if (backwards) {
        if (i == 0) return wrap ? order.back() : order.front();
        return order[i - 1];
    }
    if (i + 1 >= order.size()) return wrap ? order.front() : order.back();
    return order[i + 1];
}

UiWidgetId uiNavigate(const std::vector<UiFocusItem>& items, UiWidgetId current, UiNavDir dir, bool wrap) {
    if (items.empty()) return 0;
    const UiFocusItem* cur = findItem(items, current);
    if (!cur) {
        const std::vector<UiWidgetId> order = uiTabOrder(items);
        return order.empty() ? items.front().id : order.front();
    }

    const bool vertical = dir == UiNavDir::Up || dir == UiNavDir::Down;
    const f32 sign = (dir == UiNavDir::Down || dir == UiNavDir::Right) ? 1.0f : -1.0f;
    const auto along = [&](const UiRect& r) { return vertical ? r.center().y : r.center().x; };
    const auto across = [&](const UiRect& r) { return vertical ? r.center().x : r.center().y; };
    const auto overlapsAcross = [&](const UiRect& a, const UiRect& b) {
        const f32 lo = vertical ? std::max(a.x, b.x) : std::max(a.y, b.y);
        const f32 hi = vertical ? std::min(a.right(), b.right()) : std::min(a.bottom(), b.bottom());
        return hi - lo > 0.0f;
    };
    const auto sideways = [&](const UiRect& r) {
        return overlapsAcross(cur->rect, r) ? 0.0f : std::fabs(across(r) - across(cur->rect));
    };

    UiWidgetId best = 0;
    f32 bestScore = std::numeric_limits<f32>::max();
    for (const UiFocusItem& it : items) {
        if (it.id == current) continue;
        const f32 d = (along(it.rect) - along(cur->rect)) * sign;
        if (d <= 0.5f) continue;
        const f32 score = d + 2.0f * sideways(it.rect);
        if (score < bestScore) { bestScore = score; best = it.id; }
    }
    if (best || !wrap) return best;

    // Nothing ahead: wrap to the far side, preferring the item best aligned with where we were.
    f32 maxBehind = 0.0f;
    for (const UiFocusItem& it : items) {
        if (it.id == current) continue;
        const f32 behind = -(along(it.rect) - along(cur->rect)) * sign;
        if (behind > 0.5f) maxBehind = std::max(maxBehind, behind);
    }
    bestScore = std::numeric_limits<f32>::max();
    for (const UiFocusItem& it : items) {
        if (it.id == current) continue;
        const f32 behind = -(along(it.rect) - along(cur->rect)) * sign;
        if (behind <= 0.5f) continue;
        const f32 score = 2.0f * sideways(it.rect) + (maxBehind - behind);
        if (score < bestScore) { bestScore = score; best = it.id; }
    }
    return best;
}

} // namespace aver::ui
