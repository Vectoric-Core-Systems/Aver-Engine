// Headless test for the Unicode basics under the text engine: strict UTF-8 and the hand-written range
// tables (bidi class, mirror, script, line-break class, combining, format). No GPU, no font.
#include "aver/core/Log.hpp"
#include "aver/ui/UiUnicode.hpp"

#include <string>
#include <vector>

using namespace aver;
using namespace aver::ui;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// A tiny deterministic generator so the random cases repeat run to run.
struct Lcg {
    u32 s = 12345u;
    u32 next() { s = s * 1664525u + 1013904223u; return s >> 8; }
};

static std::vector<u32> decode(const std::string& s) {
    std::vector<u32> out;
    uiDecodeUtf8(s, out);
    return out;
}

static usize countBad(const std::vector<u32>& cps) {
    usize n = 0;
    for (const u32 c : cps) if (c == kUiReplacementChar) ++n;
    return n;
}

int main() {
    AVER_INFO("=== UTF-8 round trip ===");
    {
        bool all = true;
        usize checked = 0;
        for (u32 cp = 0; cp <= 0x10FFFF; ++cp) {
            if (cp >= 0xD800 && cp <= 0xDFFF) continue;
            std::string e;
            uiEncodeUtf8Append(e, cp);
            usize i = 0;
            const u32 back = uiDecodeOne(e, i);
            if (back != cp || i != e.size()) { all = false; break; }
            ++checked;
        }
        check(all && checked == 0x110000 - 0x800, "every scalar value encodes and decodes to itself, consuming all its bytes");

        std::string e;
        uiEncodeUtf8Append(e, 0xD800);
        check(decode(e) == std::vector<u32>{kUiReplacementChar}, "encoding a surrogate writes U+FFFD");
        e.clear();
        uiEncodeUtf8Append(e, 0x110000);
        check(decode(e) == std::vector<u32>{kUiReplacementChar}, "and so does a value over U+10FFFF");
    }

    AVER_INFO("=== invalid UTF-8: one U+FFFD per bad byte ===");
    {
        check(decode(std::string("\xC0\x80")).size() == 2 && countBad(decode(std::string("\xC0\x80"))) == 2,
              "an overlong two-byte NUL is two bad bytes");
        check(countBad(decode(std::string("\xE0\x80\x80"))) == 3, "an overlong three-byte form is three");
        check(countBad(decode(std::string("\xF0\x80\x80\x80"))) == 4, "an overlong four-byte form is four");
        check(countBad(decode(std::string("\xED\xA0\x80"))) == 3, "an encoded surrogate is three");
        check(countBad(decode(std::string("\xF4\x90\x80\x80"))) == 4, "a value over U+10FFFF is four");
        check(countBad(decode(std::string("\xF5\x80\x80\x80"))) == 4, "F5 is never a lead byte");
        check(countBad(decode(std::string("\x80"))) == 1, "a stray continuation byte is one");
        check(countBad(decode(std::string("\xE2\x82"))) == 2, "a truncated sequence at the end is two");

        const std::vector<u32> mixed = decode(std::string("a\xE2\x82" "b"));
        check(mixed.size() == 4 && mixed[0] == 'a' && mixed[1] == kUiReplacementChar && mixed[2] == kUiReplacementChar &&
                  mixed[3] == 'b',
              "a sequence cut short by an ASCII byte keeps the ASCII byte (never loses sync)");

        const std::vector<u32> euro = decode(std::string("\xE2\x82\xAC"));
        check(euro.size() == 1 && euro[0] == 0x20AC, "a good three-byte sequence decodes");
        const std::vector<u32> face = decode(std::string("\xF0\x9F\x98\x80"));
        check(face.size() == 1 && face[0] == 0x1F600, "and a four-byte one");
    }

    AVER_INFO("=== cpToByte and caps ===");
    {
        std::vector<u32> cps, map;
        const std::string s = "a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80z";
        uiDecodeUtf8(s, cps, &map);
        check(cps.size() == 5 && map.size() == 6, "one offset per code point plus the end");
        check(map[0] == 0 && map[1] == 1 && map[2] == 3 && map[3] == 6 && map[4] == 10 && map[5] == 11,
              "offsets are the byte starts");
        bool mono = true;
        for (usize i = 1; i < map.size(); ++i) if (map[i] <= map[i - 1]) mono = false;
        check(mono, "and strictly increasing");

        uiDecodeUtf8(s, cps, &map, 3);
        check(cps.size() == 3 && map.size() == 4 && map[3] == 6, "a cap stops decoding and the last entry is where it stopped");
    }

    AVER_INFO("=== bidi classes ===");
    {
        check(uiBidiClassOf('a') == UiBidiClass::L && uiBidiClassOf('Z') == UiBidiClass::L, "Latin letters are L");
        check(uiBidiClassOf('5') == UiBidiClass::EN, "digits are EN");
        check(uiBidiClassOf(0x05D0) == UiBidiClass::R && uiBidiClassOf(0x05EA) == UiBidiClass::R, "Hebrew letters are R");
        check(uiBidiClassOf(0x05FF) == UiBidiClass::R && uiBidiClassOf(0x0600) == UiBidiClass::AN,
              "the Hebrew block ends at U+05FF and Arabic number signs start at U+0600");
        check(uiBidiClassOf(0x0627) == UiBidiClass::AL && uiBidiClassOf(0x064A) == UiBidiClass::AL, "Arabic letters are AL");
        check(uiBidiClassOf(0x0660) == UiBidiClass::AN && uiBidiClassOf(0x0669) == UiBidiClass::AN, "Arabic-Indic digits are AN");
        check(uiBidiClassOf(0x06F0) == UiBidiClass::EN && uiBidiClassOf(0x06F9) == UiBidiClass::EN, "extended Arabic-Indic digits are EN");
        check(uiBidiClassOf(0x06FA) == UiBidiClass::AL, "and the next letter is AL again");
        check(uiBidiClassOf(' ') == UiBidiClass::WS && uiBidiClassOf(0x3000) == UiBidiClass::WS, "spaces are WS");
        check(uiBidiClassOf('+') == UiBidiClass::ES && uiBidiClassOf('-') == UiBidiClass::ES, "plus and minus are ES");
        check(uiBidiClassOf('$') == UiBidiClass::ET && uiBidiClassOf('%') == UiBidiClass::ET, "currency and percent are ET");
        check(uiBidiClassOf(',') == UiBidiClass::CS && uiBidiClassOf('.') == UiBidiClass::CS && uiBidiClassOf(':') == UiBidiClass::CS,
              "comma, full stop and colon are CS");
        check(uiBidiClassOf('(') == UiBidiClass::ON && uiBidiClassOf('!') == UiBidiClass::ON, "brackets and marks are ON");
        check(uiBidiClassOf(0x0301) == UiBidiClass::NSM && uiBidiClassOf(0x064E) == UiBidiClass::NSM, "combining marks are NSM");
        check(uiBidiClassOf(0x200B) == UiBidiClass::BN && uiBidiClassOf(0x00AD) == UiBidiClass::BN, "zero-width and soft hyphen are BN");
        check(uiBidiClassOf(0x202A) == UiBidiClass::LRE && uiBidiClassOf(0x202B) == UiBidiClass::RLE &&
                  uiBidiClassOf(0x202C) == UiBidiClass::PDF && uiBidiClassOf(0x202D) == UiBidiClass::LRO &&
                  uiBidiClassOf(0x202E) == UiBidiClass::RLO,
              "the five embedding controls");
        check(uiBidiClassOf(0x2066) == UiBidiClass::LRI && uiBidiClassOf(0x2067) == UiBidiClass::RLI &&
                  uiBidiClassOf(0x2068) == UiBidiClass::FSI && uiBidiClassOf(0x2069) == UiBidiClass::PDI,
              "and the four isolates");
        check(uiBidiClassOf(0x200E) == UiBidiClass::L && uiBidiClassOf(0x200F) == UiBidiClass::R, "LRM is L and RLM is R");
        check(uiBidiClassOf('\n') == UiBidiClass::B && uiBidiClassOf(0x2029) == UiBidiClass::B, "paragraph separators are B");
        check(uiBidiClassOf('\t') == UiBidiClass::S, "tab is S");
        check(uiBidiClassOf(0xFB50) == UiBidiClass::AL && uiBidiClassOf(0xFEFC) == UiBidiClass::AL, "presentation forms are AL");
        check(uiBidiClassOf(0x4E2D) == UiBidiClass::L, "Han is L");
    }

    AVER_INFO("=== mirroring ===");
    {
        check(uiMirror('(') == ')' && uiMirror(')') == '(', "parentheses");
        check(uiMirror('[') == ']' && uiMirror('{') == '}' && uiMirror('<') == '>', "brackets, braces, angle");
        check(uiMirror(0x00AB) == 0x00BB && uiMirror(0x00BB) == 0x00AB, "guillemets");
        check(uiMirror(0x300C) == 0x300D && uiMirror(0xFF08) == 0xFF09, "CJK and fullwidth brackets");
        check(uiMirror('a') == 'a' && uiMirror(0x05D0) == 0x05D0, "letters are unchanged");
        bool involution = true;
        for (u32 cp = 0; cp < 0x10000; ++cp) if (uiMirror(uiMirror(cp)) != cp) involution = false;
        check(involution, "mirroring twice gives the original for every BMP code point");
    }

    AVER_INFO("=== scripts ===");
    {
        check(uiScriptOf('a') == UiScript::Latin && uiScriptOf(0x00E9) == UiScript::Latin, "Latin");
        check(uiScriptOf(0x03B1) == UiScript::Greek, "Greek");
        check(uiScriptOf(0x0416) == UiScript::Cyrillic, "Cyrillic");
        check(uiScriptOf(0x0627) == UiScript::Arabic && uiScriptOf(0xFE8D) == UiScript::Arabic, "Arabic, including presentation forms");
        check(uiScriptOf(0x05D0) == UiScript::Hebrew, "Hebrew");
        check(uiScriptOf(0x4E2D) == UiScript::Han && uiScriptOf(0x20000) == UiScript::Han, "Han");
        check(uiScriptOf(0x3042) == UiScript::Hiragana && uiScriptOf(0x30A2) == UiScript::Katakana, "kana");
        check(uiScriptOf(0xAC00) == UiScript::Hangul, "Hangul");
        check(uiScriptOf(0x0E01) == UiScript::Thai && uiScriptOf(0x0915) == UiScript::Devanagari, "Thai and Devanagari");
        check(uiScriptOf('1') == UiScript::Common && uiScriptOf(' ') == UiScript::Common, "digits and spaces are Common");
        check(std::string(uiScriptName(UiScript::Arabic)) == "Arabic", "script names");
    }

    AVER_INFO("=== line-break classes ===");
    {
        check(uiLineBreakClassOf(' ') == UiLb::SP && uiLineBreakClassOf('\n') == UiLb::LF && uiLineBreakClassOf('\r') == UiLb::CR,
              "space and the hard breaks");
        check(uiLineBreakClassOf('(') == UiLb::OP && uiLineBreakClassOf(')') == UiLb::CP && uiLineBreakClassOf('-') == UiLb::HY,
              "brackets and hyphen");
        check(uiLineBreakClassOf('0') == UiLb::NU && uiLineBreakClassOf('a') == UiLb::AL, "digits and letters");
        check(uiLineBreakClassOf(0x4E2D) == UiLb::ID && uiLineBreakClassOf(0x3002) == UiLb::CL, "ideographs and the full stop");
        check(uiLineBreakClassOf(0x00A0) == UiLb::GL && uiLineBreakClassOf(0x200B) == UiLb::ZW && uiLineBreakClassOf(0x2060) == UiLb::WJ,
              "no-break space, zero-width space, word joiner");
        check(uiLineBreakClassOf(0x3041) == UiLb::CJ && uiLineBreakClassOf(0x3042) == UiLb::ID && uiLineBreakClassOf(0x30FC) == UiLb::CJ,
              "small kana are CJ, full kana ID, the long vowel mark CJ");
        check(uiLineBreakClassOf(0x05D0) == UiLb::HL, "Hebrew letters are HL");
        check(uiLineBreakClassOf(0x0301) == UiLb::CM, "combining marks");
        check(uiLineBreakClassOf('"') == UiLb::QU && uiLineBreakClassOf('!') == UiLb::EX && uiLineBreakClassOf('/') == UiLb::SY,
              "quote, exclamation, solidus");
        check(uiLineBreakClassOf('$') == UiLb::PR && uiLineBreakClassOf('%') == UiLb::PO && uiLineBreakClassOf(',') == UiLb::IS,
              "prefix, postfix, infix separators");
    }

    AVER_INFO("=== predicates ===");
    {
        check(uiIsCombining(0x0301) && uiIsCombining(0x064E) && uiIsCombining(0xFE0F) && !uiIsCombining('a'), "combining marks");
        check(uiIsFormatChar(0x200D) && uiIsFormatChar(0x200C) && uiIsFormatChar(0x00AD) && uiIsFormatChar(0x202E) && !uiIsFormatChar('a'),
              "format characters");
        check(uiIsControlChar(0x01) && uiIsControlChar(0x7F) && !uiIsControlChar('\t') && !uiIsControlChar('a'), "controls other than tab");
        check(uiIsHangingSpace(' ') && uiIsHangingSpace(0x3000) && !uiIsHangingSpace(0x00A0), "hanging spaces exclude no-break space");
        check(uiIsParagraphBreak('\n') && uiIsParagraphBreak('\r') && uiIsParagraphBreak(0x85) && uiIsParagraphBreak(0x2029) &&
                  !uiIsParagraphBreak(0x2028),
              "paragraph breaks (LS is not one)");
        check(uiUnicodeTablesSound(), "every range table is sorted and disjoint");
    }

    AVER_INFO("=== random bytes ===");
    {
        Lcg rng;
        bool stable = true, sized = true;
        for (int n = 0; n < 20000 && stable; ++n) {
            std::string s;
            const u32 len = rng.next() % 24;
            for (u32 k = 0; k < len; ++k) {
                // Bias towards bytes that matter: leads and continuations.
                const u32 r = rng.next();
                s.push_back(static_cast<char>((r % 3 == 0) ? (0x80 + (r >> 4) % 0x80) : ((r >> 4) % 256)));
            }
            std::vector<u32> a, map;
            uiDecodeUtf8(s, a, &map);
            if (map.size() != a.size() + 1) sized = false;
            const std::string back = uiEncodeUtf8(a);
            std::vector<u32> b;
            uiDecodeUtf8(back, b);
            if (a != b) stable = false;
        }
        check(sized, "cpToByte always has one more entry than code points");
        check(stable, "decode, encode, decode is stable for 20,000 random byte strings");
    }

    if (g_failures == 0) AVER_INFO("=== all Unicode tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
