#pragma once
// The project's stretch settings (mode, aspect, scale mode): how a base size the UI is authored at lands in a
// window of any shape. Pure arithmetic, no tree and no device.
//
//   disabled      the content is the window's size (times the scale factor): nothing stretches
//   canvas_items  the content keeps its base size and is drawn scaled to the window (crisp text and edges)
//   viewport      the content is rendered at the base size and the picture is scaled up
//
// The aspect decides what happens when the window's shape differs from the base size's: ignore stretches each axis on its
// own, keep adds bars, keep_width / keep_height keep one axis and grow the other, expand grows whichever axis is needed.
#include "aver/ui/UiTypes.hpp"

namespace aver::ui {

enum class UiStretchMode : i32 { Disabled = 0, CanvasItems = 1, Viewport = 2 };
enum class UiStretchAspect : i32 { Ignore = 0, Keep = 1, KeepWidth = 2, KeepHeight = 3, Expand = 4 };
enum class UiStretchScaleMode : i32 { Fractional = 0, Integer = 1 };

struct UiStretchSettings {
    UiStretchMode mode = UiStretchMode::Disabled;
    UiStretchAspect aspect = UiStretchAspect::Keep;
    UiStretchScaleMode scaleMode = UiStretchScaleMode::Fractional;
    f32 baseW = 0, baseH = 0;   // the size the UI is authored at; 0 in either = no base size (behaves as disabled)
    f32 scale = 1.0f;           // display/window/stretch/scale
};

struct UiStretchResult {
    UiRect screen;              // where the content lands in the window, in window pixels; the rest is letterbox
    UiVec2 contentSize;         // the size of the visible rectangle Controls are laid out in (content units)
    UiVec2 renderSize;          // the pixels the content is rendered at (equals `screen` unless the mode is viewport)
    f32 scaleX = 1, scaleY = 1; // content unit -> window pixel
    bool letterboxed = false;   // bars appear on at least one side
};

// The content-scale rules. A window of zero size gives a zero result.
UiStretchResult uiComputeStretch(const UiStretchSettings& s, f32 windowW, f32 windowH);

// A point in window pixels -> content units (the pointer that Controls hit-test against), and back.
UiVec2 uiStretchWindowToContent(const UiStretchResult& r, f32 x, f32 y);
UiVec2 uiStretchContentToWindow(const UiStretchResult& r, f32 x, f32 y);

} // namespace aver::ui
