#pragma once
// Shared value types of the retained widget system: geometry, colour helpers and the enums a
// widget's authored properties use. No behaviour here; see docs/GAME_UI.md.
#include "aver/core/Types.hpp"

#include <algorithm>
#include <cmath>

namespace aver::ui {

struct UiVec2 {
    f32 x = 0, y = 0;
};

// Per-edge distances: padding, margin and stretched-anchor offsets all use this.
struct UiInsets {
    f32 left = 0, top = 0, right = 0, bottom = 0;
    f32 horizontal() const { return left + right; }
    f32 vertical() const { return top + bottom; }
    bool operator==(const UiInsets& o) const {
        return left == o.left && top == o.top && right == o.right && bottom == o.bottom;
    }
};

// Anchor rectangle in the parent's normalised space. min == max on an axis is a point anchor.
struct UiAnchors {
    f32 minX = 0, minY = 0, maxX = 0, maxY = 0;
    bool operator==(const UiAnchors& o) const {
        return minX == o.minX && minY == o.minY && maxX == o.maxX && maxY == o.maxY;
    }
};

// Texture sub-rectangle for images.
struct UiUvRect {
    f32 u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    bool operator==(const UiUvRect& o) const {
        return u0 == o.u0 && v0 == o.v0 && u1 == o.u1 && v1 == o.v1;
    }
};

// Screen-space rectangle, pixels, top-left origin.
struct UiRect {
    f32 x = 0, y = 0, w = 0, h = 0;

    f32 right() const { return x + w; }
    f32 bottom() const { return y + h; }
    bool empty() const { return w <= 0.0f || h <= 0.0f; }
    bool contains(f32 px, f32 py) const { return px >= x && py >= y && px < x + w && py < y + h; }
    UiVec2 center() const { return {x + w * 0.5f, y + h * 0.5f}; }

    UiRect intersect(const UiRect& o) const {
        const f32 l = std::max(x, o.x), t = std::max(y, o.y);
        const f32 r = std::min(right(), o.right()), b = std::min(bottom(), o.bottom());
        return {l, t, std::max(0.0f, r - l), std::max(0.0f, b - t)};
    }
    // Shrinks by per-edge insets; never negative in size.
    UiRect deflate(const UiInsets& i) const {
        return {x + i.left, y + i.top, std::max(0.0f, w - i.horizontal()), std::max(0.0f, h - i.vertical())};
    }
    bool operator==(const UiRect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
};

// ---- colour: straight RGBA packed 0xAABBGGRR, the packing UiDrawList takes ------------------------

constexpr u32 uiRgba(u32 r, u32 g, u32 b, u32 a = 255) { return r | (g << 8) | (b << 16) | (a << 24); }

inline u32 uiAlphaOf(u32 rgba) { return rgba >> 24; }

// Multiplies the alpha channel by `k` (clamped 0..1).
inline u32 uiWithOpacity(u32 rgba, f32 k) {
    const f32 c = k < 0.0f ? 0.0f : (k > 1.0f ? 1.0f : k);
    const u32 a = static_cast<u32>(static_cast<f32>(rgba >> 24) * c + 0.5f);
    return (rgba & 0x00FFFFFFu) | (a << 24);
}

// Moves RGB towards white (k > 0) or black (k < 0); alpha untouched.
inline u32 uiShade(u32 rgba, f32 k) {
    const f32 t = k < 0.0f ? -k : k;
    const f32 target = k < 0.0f ? 0.0f : 255.0f;
    u32 out = rgba & 0xFF000000u;
    for (int s = 0; s <= 16; s += 8) {
        const f32 c = static_cast<f32>((rgba >> s) & 0xFF);
        const u32 v = static_cast<u32>(c + (target - c) * t + 0.5f);
        out |= (v > 255 ? 255u : v) << s;
    }
    return out;
}

// ---- enums an authored widget uses ---------------------------------------------------------------

enum class UiWidgetKind : u8 {
    Panel, Text, Image, Button, Toggle, Slider, Choice, List, Scroll, TextInput, ProgressBar, KeyBind,
    Count
};

// How a widget arranges its CHILDREN.
enum class UiLayoutMode : u8 { None, VStack, HStack, Grid };

// Cross-axis (and grid cell) alignment of a child inside its slot.
enum class UiAlign : u8 { Start, Center, End, Stretch };

// Main-axis distribution of leftover space when no child fills.
enum class UiJustify : u8 { Start, Center, End, SpaceBetween };

enum class UiTextAlign : u8 { Left, Center, Right };
enum class UiImageMode : u8 { Stretch, Fit };

// What a ROOT widget does with input. Menu pauses game input and owns keyboard/gamepad navigation.
enum class UiInputMode : u8 { Passive, Blocking, Menu };

// Whether a widget takes part in pointer hit testing. Auto = interactive kinds and filled panels.
enum class UiHitMode : u8 { Auto, Always, Never };

// How the UI scale follows the viewport.
enum class UiScaleMode : u8 { Constant, Width, Height, ShortestSide, Blend };

} // namespace aver::ui
