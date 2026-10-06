#pragma once
// One widget: the authored properties (UiWidgetProps, what a layout asset stores) plus the runtime
// state a UiTree keeps (UiWidget). Sizes in props are design pixels; the tree scales them.
#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/UiTypes.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::ui {

using UiWidgetId = u32;   // 0 is "no widget"

struct UiWidgetProps {
    std::string name;
    UiWidgetKind kind = UiWidgetKind::Panel;

    // ---- placement in the parent --------------------------------------------------------------
    // Point anchor on an axis: offsets.left/top is the position, width/height the size (<0 = auto).
    // Stretched anchor: offsets are insets from the anchor edges and width/height are ignored.
    // Inside a stack or grid parent the anchors are ignored and width/height/fill/align apply.
    UiAnchors anchors{};
    UiInsets  offsets{};
    UiVec2    pivot{};
    f32 width = -1, height = -1;
    f32 minWidth = 0, minHeight = 0, maxWidth = 0, maxHeight = 0;   // 0 max = unbounded
    UiInsets margin{};
    UiInsets padding{};
    UiAlign alignH = UiAlign::Stretch, alignV = UiAlign::Stretch;
    f32 fillW = 0, fillH = 0;   // flex weights in a stack parent

    // ---- layout of the children ---------------------------------------------------------------
    UiLayoutMode layout = UiLayoutMode::None;
    f32 spacing = 0;
    f32 spacingY = -1;          // grid row spacing; <0 = same as spacing
    i32 columns = 2;
    f32 cellWidth = -1, cellHeight = -1;   // grid; <0 = fit the content
    UiJustify justify = UiJustify::Start;

    // ---- visuals ------------------------------------------------------------------------------
    bool visible = true;
    bool enabled = true;
    f32  opacity = 1.0f;
    bool clip = false;
    UiHitMode hit = UiHitMode::Auto;
    std::string style;                       // named style in the theme; empty = the kind's default
    u32 bgColor = 0, textColor = 0, accentColor = 0;   // 0 = use the theme (alpha-zero is "unset")

    // ---- content ------------------------------------------------------------------------------
    std::string text;
    UiTextAlign textAlign = UiTextAlign::Left;
    f32  fontSize = 0;                       // design px; 0 = theme
    bool wrap = false;
    std::string image;                       // content path; the host resolves it to UiWidget::texture
    UiUvRect uv{};
    UiImageMode imageMode = UiImageMode::Stretch;
    u32  tint = 0xFFFFFFFFu;

    // ---- values -------------------------------------------------------------------------------
    f32  value = 0, minValue = 0, maxValue = 1, step = 0;
    bool checked = false;
    i32  selected = 0;                       // Choice and List
    std::vector<std::string> items;          // Choice and List
    std::string placeholder;
    i32  maxLength = 0;                      // 0 = unlimited
    bool masked = false;
    bool showValue = false;
    f32  rowHeight = 0;                      // List; 0 = theme

    // ---- navigation and commands --------------------------------------------------------------
    bool focusable = false;
    i32  tabIndex = 0;                       // >0 first, ascending; then 0 in document order
    std::string navUp, navDown, navLeft, navRight;   // widget names overriding spatial navigation
    std::string command;                     // emitted as a Command event when activated

    // ---- root only ----------------------------------------------------------------------------
    UiInputMode inputMode = UiInputMode::Passive;
    bool modal = false;
    i32  zOrder = 0;
    UiLayer layer = UiLayer::Content;
    std::string defaultFocus;                // widget name focused when the root opens
    std::string cancelCommand;               // Command emitted when Cancel reaches the root
};

// The defaults for a freshly created widget of `kind` (focusable buttons, scroll clipping, ...).
// Serialisation writes only what differs from this.
UiWidgetProps uiDefaultProps(UiWidgetKind kind);

const char* uiKindName(UiWidgetKind kind);
bool uiKindFromName(std::string_view name, UiWidgetKind& out);

// Runtime widget owned by a UiTree.
struct UiWidget : UiWidgetProps {
    UiWidgetId id = 0;
    UiWidgetId parent = 0;
    std::vector<UiWidgetId> children;
    u32 seq = 0;                 // creation order, the z tie-break between roots

    u64 texture = 0;             // resolved image, set by the host

    // ---- computed by layout (device pixels) ---------------------------------------------------
    UiRect rect{};
    UiVec2 desired{};            // content-based size, before min/max and stretching
    f32 scrollX = 0, scrollY = 0;
    UiVec2 contentSize{};        // Scroll: extent of the children

    // ---- interaction state --------------------------------------------------------------------
    bool hovered = false;
    bool pressed = false;
    bool focused = false;
    bool listening = false;      // KeyBind waiting for a key
    i32  caret = 0;              // TextInput
    i32  hotItem = -1;           // List: row under the pointer
};

} // namespace aver::ui
