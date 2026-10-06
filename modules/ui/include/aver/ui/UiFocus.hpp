#pragma once
// Keyboard and gamepad focus movement as pure functions over a list of focusable rectangles, so the
// ordering rules are testable without a tree.
#include "aver/ui/UiTypes.hpp"
#include "aver/ui/UiWidget.hpp"

#include <vector>

namespace aver::ui {

// One focusable widget, supplied in document (depth-first) order.
struct UiFocusItem {
    UiWidgetId id = 0;
    UiRect rect{};
    i32 tabIndex = 0;   // >0 first (ascending), then 0 in document order; <0 is skipped by Tab
};

enum class UiNavDir : u8 { Up, Down, Left, Right };

// The Tab order: positive tab indices ascending (document order breaks ties), then zeros.
std::vector<UiWidgetId> uiTabOrder(const std::vector<UiFocusItem>& items);

// The widget after/before `current` in tab order. An unknown `current` yields the first (or last
// going backwards). 0 when there is nothing focusable. Without `wrap` it stays on the end.
UiWidgetId uiNextTab(const std::vector<UiFocusItem>& items, UiWidgetId current, bool backwards,
                     bool wrap = true);

// The widget to move to from `current` in a direction: the nearest by distance along the direction
// plus twice the sideways offset, where rectangles overlapping on the cross axis count as aligned.
// With `wrap` and nothing ahead, jumps to the far side. 0 when nothing qualifies; an unknown
// `current` yields the first item in tab order.
UiWidgetId uiNavigate(const std::vector<UiFocusItem>& items, UiWidgetId current, UiNavDir dir,
                      bool wrap = true);

} // namespace aver::ui
