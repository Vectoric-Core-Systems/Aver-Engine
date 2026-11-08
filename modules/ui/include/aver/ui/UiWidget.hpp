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
    std::string slot;           // a child of a USE: the used layout's SLOT it fills (empty = the instance root)

    // ---- layout of the children ---------------------------------------------------------------
    UiLayoutMode layout = UiLayoutMode::None;
    f32 spacing = 0;
    f32 spacingY = -1;          // grid row spacing; <0 = same as spacing
    i32 columns = 2;
    f32 cellWidth = -1, cellHeight = -1;   // grid; <0 = fit the content
    UiJustify justify = UiJustify::Start;
    bool uniform = false;       // Grid: every cell the size of the largest, sharing the space equally
    i32  activeIndex = 0;       // Switcher: the child shown
    UiScaleFit scaleFit = UiScaleFit::None;   // scale the children's subtree to fit the content box
    f32  contentScale = 1.0f;   // extra multiplier on the children's scale (design px, text and all)

    // ---- visuals ------------------------------------------------------------------------------
    bool visible = true;
    bool enabled = true;
    f32  opacity = 1.0f;
    bool clip = false;
    UiHitMode hit = UiHitMode::Auto;
    std::string style;                       // named style in the theme; empty = the kind's default
    u32 bgColor = 0, textColor = 0, accentColor = 0;   // 0 = use the theme (alpha-zero is "unset")
    f32 borderWidth = -1.0f;                 // design px; <0 = the style's
    u32 borderColor = 0;                     // 0 = the style's

    // ---- render transform: drawn only (layout, hit testing and focus ignore it) ----------------
    UiVec2 renderTranslate{0, 0};            // design px
    f32    renderRotation = 0;               // degrees, clockwise on screen
    UiVec2 renderScale{1, 1};
    UiVec2 renderPivot{0.5f, 0.5f};          // fraction of the widget's rectangle

    // ---- content ------------------------------------------------------------------------------
    std::string text;
    UiTextAlign textAlign = UiTextAlign::Left;
    f32  fontSize = 0;                       // design px; 0 = theme
    bool wrap = false;
    // Text styling (docs/GAME_UI_TEXT.md). Empty family / false flags inherit the theme style.
    std::string fontFamily;                  // a family of the loaded font set; empty = the style's, then the default
    bool bold = false, italic = false;
    UiTextDir textDir = UiTextDir::Auto;
    f32  lineSpacing = 1.0f, letterSpacing = 0.0f;   // multiplier; design px
    bool ellipsis = false;                   // end a clipped line with "..." (needs a width)
    i32  maxLines = 0;                       // 0 = unlimited
    bool rich = false;                       // <b> <i> <color=#RRGGBB> <size=N> <font=Name> <br> tags in text
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
    std::string accessName;                  // the spoken label (text or reference); empty = derived from the widget and its label

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

// Values an animation drives over the authored ones (UiAnimation.hpp). Runtime only: never saved, never in a property.
enum UiFxBit : u32 { kFxOpacity = 1, kFxBg = 2, kFxText = 4, kFxAccent = 8, kFxTint = 16, kFxTranslate = 32, kFxRotation = 64, kFxScale = 128 };
struct UiWidgetFx {
    u32 mask = 0;                // UiFxBit: which fields hold a value
    f32 opacity = 1;
    u32 bg = 0, text = 0, accent = 0, tint = 0xFFFFFFFFu;
    UiVec2 translate{0, 0};
    f32 rotation = 0;
    UiVec2 scale{1, 1};
};

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
    UiVec2 contentDesired{};     // the children's natural extent, before padding and content scaling
    f32 scale = 0;               // design-pixel scale this widget was laid out at (0 = the tree's); differs inside a scale box
    bool inactive = false;       // hidden by a Switcher parent
    f32 scrollX = 0, scrollY = 0;
    UiVec2 contentSize{};        // Scroll: extent of the children

    // ---- interaction state --------------------------------------------------------------------
    bool hovered = false;
    bool pressed = false;
    bool focused = false;
    bool listening = false;      // KeyBind waiting for a key
    i32  caret = 0;              // TextInput
    i32  hotItem = -1;           // List: row under the pointer
    UiWidgetFx fx{};             // animated overrides
    f32  wrapHint = 0;           // wrapped Text with a fill weight: the content width its last arrangement gave it (second layout pass)
};

} // namespace aver::ui
