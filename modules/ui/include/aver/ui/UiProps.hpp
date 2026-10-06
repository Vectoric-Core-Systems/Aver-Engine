#pragma once
// The property table: every authored widget property by name, with typed get/set through text.
// One table feeds the layout asset reader/writer, the editor's inspector and the C ABI's
// set-by-name, so a property added here appears in all three.
#include "aver/ui/UiWidget.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace aver::ui {

enum class UiPropType : u8 { Float, Int, Bool, String, Color, Enum, Insets, Anchors, Vec2, Uv, StringList };

struct UiPropDesc {
    const char* key = "";
    const char* group = "";
    UiPropType type = UiPropType::Float;
    std::vector<const char*> enumNames;   // type Enum: lower-case names in enum order
    f32 minV = 0, maxV = 0;               // inspector range hint; maxV <= minV = unbounded
    u32 kindMask = ~0u;                   // bit per UiWidgetKind the inspector shows it for
    bool rootOnly = false;                // only meaningful on a root widget

    // A property is a list of tokens: one for scalars, four for insets/anchors/uv, two for Vec2.
    std::function<void(const UiWidgetProps&, std::vector<std::string>&)> get;
    std::function<bool(UiWidgetProps&, const std::vector<std::string>&)> set;
};

// All properties, in the order the writer emits them.
const std::vector<UiPropDesc>& uiProperties();
const UiPropDesc* uiFindProperty(std::string_view key);   // case-insensitive

constexpr u32 uiKindBit(UiWidgetKind k) { return 1u << static_cast<u32>(k); }

// The property as text (tokens joined by spaces, quoted where needed). False for an unknown key.
bool uiGetProp(const UiWidgetProps& w, std::string_view key, std::string& out);
// Sets from text, tokenised like a file record. False for an unknown key or unparseable value,
// leaving the widget untouched.
bool uiSetProp(UiWidgetProps& w, std::string_view key, std::string_view text);

// ---- text helpers shared with the layout asset ----------------------------------------------------

// Splits a record into whitespace-separated tokens; "double quoted" tokens keep spaces and honour
// \" \\ \n \t. False on an unterminated quote.
bool uiTokenize(std::string_view line, std::vector<std::string>& out);
// A token ready to write: quoted when empty or containing spaces, quotes, backslashes or newlines.
std::string uiQuote(std::string_view s);
// Shortest decimal text that reads back to exactly `v`.
std::string uiFormatFloat(f32 v);
// Parses a whole token as a float; false when any part is left over.
bool uiParseFloat(std::string_view s, f32& out);
bool uiParseInt(std::string_view s, i32& out);

} // namespace aver::ui
