#pragma once
// Line-break opportunities: a subset of UAX #14 plus CJK break-anywhere and kinsoku. Pure and table
// driven (classes come from UiUnicodeData.cpp), so the whole thing is a headless test.
//
// Implemented: LB4-LB8, LB9-LB10 (combining marks), LB11-LB13, LB14-LB17, LB18, LB19, LB21-LB25, LB28-LB31.
// Not implemented: LB20 (contingent breaks), LB26-LB27 (Hangul jamo: Hangul syllables are AL, so Korean
// wraps at spaces only), LB30 beyond the OP/CP East Asian carve-out, tailoring. Thai, Lao, Khmer and
// Myanmar are AL (break at spaces only).
#include "aver/ui/UiUnicode.hpp"

#include <vector>

namespace aver::ui {

enum class UiBreak : u8 { No, Allowed, Mandatory };

// out has n + 1 entries: out[i] is the boundary BEFORE code point i. out[0] is No; out[n] is Mandatory
// (end of text). Hard breaks (LF, CR LF, NEL, VT, FF, LS, PS) give Mandatory after them.
void uiLineBreaks(const u32* cps, usize n, std::vector<UiBreak>& out);

// Kinsoku: characters that must not start a line (closing brackets and quotes, the long vowel mark,
// small kana, iteration marks, ellipses, fullwidth ! ?) or end one (opening brackets and quotes).
bool uiCannotStartLine(u32 cp);
bool uiCannotEndLine(u32 cp);

} // namespace aver::ui
