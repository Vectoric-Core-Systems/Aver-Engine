#pragma once
// Layout maths with no widgets in it: DPI scale, anchor resolution, one-axis stack solving, slot
// alignment and scroll clamping. UiTree composes these; they are tested on their own.
#include "aver/ui/UiTypes.hpp"

#include <vector>

namespace aver::ui {

// How design pixels become device pixels.
struct UiDpi {
    UiScaleMode mode = UiScaleMode::Height;
    f32 refWidth = 1920.0f, refHeight = 1080.0f;   // the resolution a layout is authored against
    f32 userScale = 1.0f;                          // the player's UI-scale setting
    f32 platformScale = 1.0f;                      // OS display scaling, used by Constant
    bool operator==(const UiDpi& o) const {
        return mode == o.mode && refWidth == o.refWidth && refHeight == o.refHeight &&
               userScale == o.userScale && platformScale == o.platformScale;
    }
};

// The multiplier for design pixels in a viewport of (vw, vh) device pixels. Clamped to [0.25, 8].
// Blend is the geometric mean of the width and height ratios, so a ultrawide or a portrait window
// does not blow the UI up or shrink it to nothing.
f32 uiComputeScale(const UiDpi& dpi, f32 vw, f32 vh);

// Places a child in `parent` by anchors. `width`/`height` are device pixels and used only on a
// point-anchored axis; a stretched axis takes its size from the anchors and `offsets` (insets).
// `scale` multiplies the offsets (they are design pixels).
UiRect uiResolveAnchored(const UiRect& parent, const UiAnchors& anchors, const UiInsets& offsets,
                         UiVec2 pivot, f32 width, f32 height, f32 scale);

// One child along a stack's main axis.
struct UiAxisItem {
    f32 desired = 0;        // content size
    f32 fill = 0;           // flex weight; 0 = takes `desired`
    f32 minSize = 0;
    f32 maxSize = 0;        // 0 = unbounded
    f32 before = 0, after = 0;   // margins along the axis
};

// A solved slot, relative to the start of the axis. pos/size exclude the margins.
struct UiSpan {
    f32 pos = 0, size = 0;
};

// Lays items out along an axis of `available` length. Fill items share what the fixed items leave
// (honouring min/max by re-sharing); leftover with no fill item is placed by `justify`.
std::vector<UiSpan> uiSolveAxis(const std::vector<UiAxisItem>& items, f32 available, f32 spacing,
                                UiJustify justify);

// Length a stack needs at minimum: desired sizes, margins and spacing.
f32 uiAxisContent(const std::vector<UiAxisItem>& items, f32 spacing);

// Places a child of `desired` size in a slot [slotPos, slotPos+slotSize) on the cross axis.
UiSpan uiAlignInSlot(f32 slotPos, f32 slotSize, f32 desired, UiAlign align, f32 minSize, f32 maxSize);

// Clamps a scroll offset to [0, content - viewport].
f32 uiClampScroll(f32 offset, f32 content, f32 viewport);

// The scroll offset that brings [itemPos, itemPos+itemSize) (content space) into view, moving as
// little as possible.
f32 uiScrollIntoView(f32 offset, f32 viewport, f32 itemPos, f32 itemSize);

// Clamp to [lo, hi]; hi <= 0 means unbounded.
f32 uiClampSize(f32 v, f32 lo, f32 hi);

} // namespace aver::ui
