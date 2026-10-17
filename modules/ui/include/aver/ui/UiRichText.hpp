#pragma once
// Rich text, deliberately small: <b></b> <i></i> <color=#RRGGBB[AA]></color> <size=N></size> <font=Family></font>
// <br>, and `<<` for a literal `<`. Tags are case-insensitive and nest by a stack; unclosed tags close at
// the end. Anything that is not exactly one of these tags (`a < b`, `<3`, `<b` with no `>`) is literal text.
// Off unless a widget sets `rich`, so existing strings, including localized ones containing `<`, are
// untouched. Pure: no engine, no fonts.
#include "aver/ui/UiUnicode.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::ui {

// A run of the plain text with one resolved style. Runs are in order, contiguous and cover all of it.
struct UiRichSpan {
    u32 begin = 0, end = 0;        // code point range in the plain text
    bool bold = false, italic = false;
    bool hasColor = false;
    u32 color = 0;                 // straight RGBA packed 0xAABBGGRR when hasColor
    f32 size = 0;                  // design pixels from <size=N>; 0 = unchanged
    std::string font;              // family from <font=Name>; empty = unchanged
};

// Strips the tags from `in` into `plain` (decoded code points; <br> becomes U+000A) and the style runs.
// Returns the number of tags recognised.
usize uiParseRich(std::string_view in, std::vector<u32>& plain, std::vector<UiRichSpan>& spans);

// The text with every tag removed, as UTF-8: what a screen reader or a caption should hear.
std::string uiStripRichText(std::string_view in);

} // namespace aver::ui
