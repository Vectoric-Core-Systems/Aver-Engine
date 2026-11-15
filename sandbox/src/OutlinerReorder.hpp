// The pure half of dropping Outliner rows beside / onto another row: which third of the row the cursor is
// in, and where a set of rows lands in a sibling list. No ImGui, no scene::, so it is unit-tested on the CPU.
#pragma once

#include <algorithm>
#include <vector>

namespace aver::editor {

enum class OutlinerDropZone { Before, Into, After };

// Top quarter of the row = insert before it, bottom quarter = insert after it, the middle = reparent onto it.
inline OutlinerDropZone outlinerDropZone(float mouseY, float rowY0, float rowY1) {
    const float h = rowY1 - rowY0;
    if (h <= 0.0f) return OutlinerDropZone::Into;
    const float t = (mouseY - rowY0) / h;
    if (t < 0.25f) return OutlinerDropZone::Before;
    if (t > 0.75f) return OutlinerDropZone::After;
    return OutlinerDropZone::Into;
}

// Removes every element of `moving` from `order` and re-inserts them, in `moving`'s sequence, directly before (or after) `anchor`.
// `moving` elements missing from `order` are still inserted (they have just become siblings). False, `order` untouched, when the
// anchor is not in `order` or is itself one of `moving`.
template <class T>
bool placeBeside(std::vector<T>& order, const std::vector<T>& moving, const T& anchor, bool after) {
    if (std::find(moving.begin(), moving.end(), anchor) != moving.end()) return false;
    if (std::find(order.begin(), order.end(), anchor) == order.end()) return false;
    std::vector<T> rest;
    rest.reserve(order.size());
    for (const T& e : order)
        if (std::find(moving.begin(), moving.end(), e) == moving.end()) rest.push_back(e);
    auto at = std::find(rest.begin(), rest.end(), anchor);
    if (after) ++at;
    rest.insert(at, moving.begin(), moving.end());
    order.swap(rest);
    return true;
}

}  // namespace aver::editor
