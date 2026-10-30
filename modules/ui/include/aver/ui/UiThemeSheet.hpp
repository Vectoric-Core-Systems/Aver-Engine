#pragma once
// The theme resource (.octheme): a Theme as data. Items per type (colours, constants, fonts, font sizes, icons,
// style boxes), type variations, and the lookup order of Control.get_theme_*: the nearest theme in the node chain wins,
// then the project theme, then the default theme, and inside one theme the type list runs variation -> its base ->
// the control's class chain. Text, the same OC dialect as the other formats; parse from memory and write to a string
// (this module does no file I/O, like UiLayoutAsset). docs/formats/FORMAT_SPECS.md, ".octheme".
//
//   OCTHEME 1
//   NAME Main
//   DEFAULTFONT "Fonts/ui.ttf"
//   DEFAULTFONTSIZE 18
//   VARIATION HeaderLabel Label
//   COLOR Label font_color #FFFFFFFF
//   CONSTANT Button h_separation 6
//   FONT HeaderLabel font "Fonts/bold.ttf"
//   FONTSIZE HeaderLabel font_size 28
//   ICON CheckBox checked "Textures/checked.png"
//   STYLEBOX Button normal flat bg=#2E2E2EFF border=2,2,2,2 border_color=#80C0FFFF radius=4,4,4,4 content=8,4,8,4
#include "aver/ui/UiStyleBox.hpp"

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace aver::ui {

inline constexpr i32 kUiThemeVersion = 1;

struct UiThemeStyleBox {
    UiStyleBox box;
    std::string texturePath;   // Texture style boxes: the content path of the image (the engine handle is resolved by the host)
};

enum class UiThemeItemKind : i32 { Color = 0, Constant = 1, Font = 2, FontSize = 3, Icon = 4, StyleBox = 5 };

struct UiThemeType {
    std::map<std::string, u32> colors;            // straight 0xAABBGGRR
    std::map<std::string, i32> constants;
    std::map<std::string, std::string> fonts;     // content path ("" = the default font)
    std::map<std::string, i32> fontSizes;
    std::map<std::string, std::string> icons;     // content path
    std::map<std::string, UiThemeStyleBox> styleBoxes;
};

struct UiThemeSheet {
    std::string name;
    std::string defaultFont;     // "" = unset
    i32 defaultFontSize = -1;    // < 0 = unset
    f32 defaultBaseScale = 0;    // 0 = unset
    std::map<std::string, UiThemeType> types;
    std::map<std::string, std::string> variations;   // variation type -> base type

    bool hasItem(UiThemeItemKind kind, std::string_view type, std::string_view item) const;
    usize itemCount() const;
};

bool uiThemeParse(std::string_view text, UiThemeSheet& out, std::string* err = nullptr);
std::string uiThemeWrite(const UiThemeSheet& sheet);

// The control's type list, most specific first: [variation, its bases..., the class chain] (duplicates dropped).
// `themes` is nearest first; a variation's base is taken from the nearest theme that declares it. `classChain` is the
// control's class and its ancestors ("Button", "BaseButton", "Control").
std::vector<std::string> uiThemeTypeList(const std::vector<const UiThemeSheet*>& themes, const std::vector<std::string>& classChain,
                                         std::string_view variation);

// Lookup: the first theme (nearest first) holding the item under any type of the list, trying the types in order within a
// theme. Returns the theme that answered, or null.
const UiThemeSheet* uiThemeFindColor(const std::vector<const UiThemeSheet*>& themes, const std::vector<std::string>& types, std::string_view name, u32& out);
const UiThemeSheet* uiThemeFindConstant(const std::vector<const UiThemeSheet*>& themes, const std::vector<std::string>& types, std::string_view name, i32& out);
const UiThemeSheet* uiThemeFindFont(const std::vector<const UiThemeSheet*>& themes, const std::vector<std::string>& types, std::string_view name, std::string& out);
const UiThemeSheet* uiThemeFindFontSize(const std::vector<const UiThemeSheet*>& themes, const std::vector<std::string>& types, std::string_view name, i32& out);
const UiThemeSheet* uiThemeFindIcon(const std::vector<const UiThemeSheet*>& themes, const std::vector<std::string>& types, std::string_view name, std::string& out);
const UiThemeSheet* uiThemeFindStyleBox(const std::vector<const UiThemeSheet*>& themes, const std::vector<std::string>& types, std::string_view name, UiThemeStyleBox& out);

// The default font and size when no font item answered: the first theme in the chain that set them.
bool uiThemeDefaultFont(const std::vector<const UiThemeSheet*>& themes, std::string& out);
i32 uiThemeDefaultFontSize(const std::vector<const UiThemeSheet*>& themes, i32 fallback);

// The metrics of the built-in theme, for projects that name no theme: the fallback at the
// end of every lookup. A dark default look; no third-party theme assets are used.
const UiThemeSheet& uiBuiltinDefaultTheme();
const std::string& uiBuiltinDefaultThemeText();

} // namespace aver::ui
