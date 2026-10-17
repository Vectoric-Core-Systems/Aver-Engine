#pragma once
// Unicode basics the text engine needs, with no tables vendored: strict UTF-8, and hand-written range
// tables for bidi class, mirroring, script, line-break class and combining marks (UiUnicodeData.cpp).
// Pure functions, no state, no I/O. See docs/GAME_UI_TEXT.md.
#include "aver/core/Types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace aver::ui {

constexpr u32 kUiReplacementChar = 0xFFFD;

// ---- UTF-8 ----------------------------------------------------------------------------------------

// Decodes the code point at s[i] and advances i. Strict: an overlong form, a surrogate, a value over
// U+10FFFF, a truncated sequence or a stray continuation byte yields U+FFFD and consumes ONE byte, so
// every bad byte becomes one U+FFFD and decoding never loses sync.
u32 uiDecodeOne(std::string_view s, usize& i);

// Decodes all of `in` (at most `maxCps` code points when non-zero). `cpToByte`, if given, receives one
// byte offset per code point plus a final entry: the offset where decoding stopped. Returns the count.
usize uiDecodeUtf8(std::string_view in, std::vector<u32>& out, std::vector<u32>* cpToByte = nullptr,
                   usize maxCps = 0);

// Appends one code point; a surrogate or a value over U+10FFFF is written as U+FFFD.
void uiEncodeUtf8Append(std::string& out, u32 cp);
std::string uiEncodeUtf8(const u32* cps, usize n);
inline std::string uiEncodeUtf8(const std::vector<u32>& cps) { return uiEncodeUtf8(cps.data(), cps.size()); }

// ---- properties -----------------------------------------------------------------------------------

// UAX #9 bidirectional classes.
enum class UiBidiClass : u8 {
    L, R, AL, EN, ES, ET, AN, CS, NSM, BN, B, S, WS, ON, LRE, LRO, RLE, RLO, PDF, LRI, RLI, FSI, PDI
};
UiBidiClass uiBidiClassOf(u32 cp);

// The mirrored counterpart of a paired character (parentheses, brackets, angle quotes ...), else cp.
u32 uiMirror(u32 cp);

enum class UiScript : u8 {
    Common, Latin, Greek, Cyrillic, Arabic, Hebrew, Han, Hiragana, Katakana, Hangul, Thai, Devanagari, Other
};
UiScript uiScriptOf(u32 cp);
const char* uiScriptName(UiScript s);

// UAX #14 line-break classes (the subset the engine implements).
enum class UiLb : u8 {
    AL, BK, CR, LF, NL, CM, WJ, ZW, GL, SP, B2, BA, BB, HY, CL, CP, EX, IN, NS, OP, QU, IS, NU, PO, PR, SY,
    ID, HL, CJ, ZWJ
};
UiLb uiLineBreakClassOf(u32 cp);

// A mark that attaches to the previous base (nonspacing and common spacing combining marks, variation
// selectors). Arabic transparent marks are included.
bool uiIsCombining(u32 cp);
// Zero-width and formatting characters that have no glyph and must not draw a missing-glyph box:
// ZWSP, ZWNJ, ZWJ, bidi marks and embeddings, word joiner, soft hyphen, variation selectors, tags, BOM.
bool uiIsFormatChar(u32 cp);
// C0 and C1 controls other than tab.
bool uiIsControlChar(u32 cp);
// Space-like characters that hang at the end of a wrapped line.
bool uiIsHangingSpace(u32 cp);
// Paragraph separators: LF, CR, NEL, PS. (LS U+2028 is a line break inside a paragraph.)
bool uiIsParagraphBreak(u32 cp);

// Checks the range tables are sorted and disjoint. Used by the unit test; true when sound.
bool uiUnicodeTablesSound();

} // namespace aver::ui
