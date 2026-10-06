#pragma once
// Styles and themes. A theme holds one default style per widget kind plus named variants a widget
// picks with its `style` property ("title", "danger", ...). A widget's own colour overrides win over
// both. Sizes are design pixels.
#include "aver/ui/UiTypes.hpp"
#include "aver/ui/UiWidget.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aver::ui {

enum class UiVisualState : u8 { Normal, Hover, Pressed, Disabled, Count };

// In a NAMED style, 0 in a colour / font size / border width field means "keep the kind's value".
struct UiStyle {
    u32 background[4] = {0, 0, 0, 0};   // indexed by UiVisualState
    u32 border = 0;
    f32 borderWidth = 0;
    u32 text = 0;
    u32 textDisabled = 0;
    u32 accent = 0;                     // slider fill, checkmark, progress fill, caret
    u32 track = 0;                      // slider / progress track
    u32 placeholder = 0;
    f32 fontSize = 0;                   // 0 = theme base
    UiInsets padding{};                 // added to the widget's own padding
};

struct UiTheme {
    std::string name = "dark";
    f32 baseFontSize = 18.0f;
    u32 focusColor = uiRgba(255, 200, 80);
    f32 focusWidth = 2.0f;
    f32 rowHeight = 30.0f;              // List rows
    f32 scrollbarWidth = 8.0f;
    u32 dim = uiRgba(0, 0, 0, 140);     // the veil drawn behind a modal root
    UiStyle kinds[static_cast<usize>(UiWidgetKind::Count)];
    std::vector<std::pair<std::string, UiStyle>> named;

    // The kind's style with the named variant merged over it. Unknown names fall back to the kind.
    UiStyle resolve(UiWidgetKind kind, std::string_view namedStyle) const;
};

// "dark", "light" or "contrast". Anything else returns "dark".
UiTheme uiBuiltinTheme(std::string_view name);

// Names of the built-in themes, null-terminated.
const char* const* uiBuiltinThemeNames();

} // namespace aver::ui
