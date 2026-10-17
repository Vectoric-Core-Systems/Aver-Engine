// Headless test for line-break opportunities: the implemented UAX #14 rules, numbers, URLs, no-break
// characters, hard breaks, CJK break-anywhere with kinsoku, and invariants over random strings.
#include "aver/core/Log.hpp"
#include "aver/ui/UiLineBreak.hpp"

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

static std::vector<u32> cps(const std::u32string& s) {
    std::vector<u32> v;
    for (const char32_t c : s) v.push_back(static_cast<u32>(c));
    return v;
}

static std::vector<UiBreak> breaksOf(const std::u32string& s) {
    std::vector<UiBreak> out;
    const std::vector<u32> c = cps(s);
    uiLineBreaks(c.data(), c.size(), out);
    return out;
}

static bool no(const std::u32string& s, usize i) { return breaksOf(s)[i] == UiBreak::No; }
static bool allowed(const std::u32string& s, usize i) { return breaksOf(s)[i] == UiBreak::Allowed; }
static bool mandatory(const std::u32string& s, usize i) { return breaksOf(s)[i] == UiBreak::Mandatory; }

int main() {
    AVER_INFO("=== boundary semantics ===");
    {
        const std::vector<UiBreak> b = breaksOf(U"abc");
        check(b.size() == 4, "n + 1 entries");
        check(b[0] == UiBreak::No, "no break at the start (LB2)");
        check(b[3] == UiBreak::Mandatory, "a break at the end (LB3)");
        check(b[1] == UiBreak::No && b[2] == UiBreak::No, "letters do not break (LB28)");
        const std::vector<UiBreak> e = breaksOf(U"");
        check(e.size() == 1, "empty text has one entry");
    }

    AVER_INFO("=== spaces ===");
    {
        check(no(U"ab cd", 2), "no break before a space (LB7)");
        check(allowed(U"ab cd", 3), "a break after a space (LB18)");
        check(no(U"ab  cd", 3) && allowed(U"ab  cd", 4), "after the last of several spaces");
        check(allowed(U" a", 1), "a leading space then a word");
    }

    AVER_INFO("=== hard breaks ===");
    {
        check(no(U"a\nb", 1) && mandatory(U"a\nb", 2), "LF: no break before, mandatory after (LB4, LB6)");
        check(no(U"a\r\nb", 2) && mandatory(U"a\r\nb", 3) && no(U"a\r\nb", 1), "CR LF is one break (LB5)");
        check(mandatory(U"a\rb", 2), "a lone CR breaks after");
        check(mandatory(U"a\u0085b", 2), "NEL breaks after");
        check(mandatory(U"a\U00002028b", 2), "LS breaks after");
        check(mandatory(U"a\u000Bb", 2) && mandatory(U"a\u000Cb", 2), "VT and FF break after");
        check(no(U"a\n", 1) && mandatory(U"a\n", 2), "a trailing newline");
    }

    AVER_INFO("=== numbers and punctuation ===");
    {
        const std::u32string money = U"$10.00";
        bool none = true;
        const std::vector<UiBreak> mb = breaksOf(money);
        for (usize i = 1; i < money.size(); ++i) if (mb[i] != UiBreak::No) none = false;
        check(none, "$10.00 never breaks inside (LB25)");
        const std::u32string thousand = U"1,000";
        const std::vector<UiBreak> tb = breaksOf(thousand);
        bool none2 = true;
        for (usize i = 1; i < thousand.size(); ++i) if (tb[i] != UiBreak::No) none2 = false;
        check(none2, "1,000 never breaks inside");
        const std::u32string pct = U"3.14%";
        const std::vector<UiBreak> pb = breaksOf(pct);
        bool none3 = true;
        for (usize i = 1; i < pct.size(); ++i) if (pb[i] != UiBreak::No) none3 = false;
        check(none3, "3.14% never breaks inside");
        check(no(U"10%", 2), "a percent sign stays with its number (LB25)");
        check(no(U"a!", 1) && no(U"a?", 1) && no(U"a,", 1) && no(U"a.", 1), "no break before ! ? , . (LB13)");
        check(allowed(U"a. b", 3), "but after the space that follows a full stop");
        check(no(U"(ab)", 1) && no(U"(ab)", 3), "no break after ( or before ) (LB13, LB14)");
        check(no(U"( a", 2), "OP SP* x: no break after an opening bracket and spaces (LB14)");
        check(no(U"a\"b", 1) && no(U"a\"b", 2), "straight quotes bind both sides (LB19)");
        check(no(U"a\U00002026b", 1), "no break before an ellipsis (LB22)");
        check(allowed(U"a \"b", 2) && no(U"a \"b", 3), "a space then a quote may break before the quote");
    }

    AVER_INFO("=== URLs, hyphens, no-break characters ===");
    {
        check(no(U"a/b", 1) && allowed(U"a/b", 2), "a slash allows a break after it, not before");
        check(no(U"a-b", 1) && allowed(U"a-b", 2), "a hyphen allows a break after it, not before");
        check(no(U"a\U000000ADb", 1) && allowed(U"a\U000000ADb", 2), "a soft hyphen is a break opportunity after it");
        check(no(U"a\U000000A0b", 1) && no(U"a\U000000A0b", 2), "no-break space (GL) binds both sides (LB12, LB12a)");
        check(no(U"a\U00002060b", 1) && no(U"a\U00002060b", 2), "word joiner binds both sides (LB11)");
        check(no(U"a\U0000200Bb", 1) && allowed(U"a\U0000200Bb", 2), "zero-width space breaks after it (LB8)");
        check(allowed(U"a\U0000200B b", 3), "ZW SP* breaks (LB8)");
        check(no(U"e\U00000301x", 1), "a combining mark stays with its base (LB9)");
        check(no(U"\U00000301a", 1), "a leading mark acts as a letter (LB10)");
        check(no(U"a1", 1) && no(U"1a", 1), "letters and digits stick (LB23)");
        check(no(U"a\U0000200D\U00004E2D", 2), "nothing breaks after a ZWJ (LB8a)");
    }

    AVER_INFO("=== CJK ===");
    {
        check(allowed(U"\U00004E2D\U00006587\U00005B57", 1) && allowed(U"\U00004E2D\U00006587\U00005B57", 2), "ideographs break anywhere");
        check(allowed(U"ab\U00004E2D\U00006587cd", 2) && allowed(U"ab\U00004E2D\U00006587cd", 4) && allowed(U"ab\U00004E2D\U00006587cd", 3), "Latin to CJK and back");
        check(no(U"ab\U00004E2D\U00006587cd", 1) && no(U"ab\U00004E2D\U00006587cd", 5), "but not inside the Latin runs");
        check(allowed(U"\U00003042\U00003044\U00003046", 1) && allowed(U"\U000030A2\U000030A4", 1), "kana breaks anywhere too");
        check(no(U"\U00004E2D%", 1), "a percent after an ideograph stays (LB23a)");

        // Kinsoku: nothing in the cannot-start list may start a line, nothing in the cannot-end list end one.
        const std::u32string cannotStart =
            U")]}\U00003001\U00003002\U0000FF0C\U0000FF0E\U0000FF1A\U0000FF1B\U0000FF01\U0000FF1F\U0000300D\U0000300F\U0000FF09\U00003011\U00003015\U00003009\U0000300B\U0000201D\U00002019"
            U"\U000030FC\U00003041\U00003043\U00003045\U00003047\U00003049\U00003063\U00003083\U00003085\U00003087\U0000308E\U000030A1\U000030A3\U000030A5\U000030A7\U000030A9\U000030C3\U000030E3"
            U"\U000030E5\U000030E7\U000030EE\U000030F5\U000030F6\U00003005\U0000303B\U00002026\U00002025";
        bool startOk = true;
        for (const char32_t c : cannotStart) {
            if (!uiCannotStartLine(static_cast<u32>(c))) { startOk = false; AVER_ERROR("    uiCannotStartLine false for U+{:04X}", static_cast<u32>(c)); }
            std::u32string s = U"\U00004E2D";
            s.push_back(c);
            if (!no(s, 1)) { startOk = false; AVER_ERROR("    a line may start with U+{:04X}", static_cast<u32>(c)); }
        }
        check(startOk, "every cannot-start character stays on the line before it");
        check(!uiCannotStartLine(0x4E2D) && !uiCannotStartLine('a') && !uiCannotStartLine('(') , "ordinary characters may start a line");

        const std::u32string cannotEnd = U"([{\U0000300C\U0000300E\U0000FF08\U00003010\U00003014\U00003008\U0000300A\U0000201C\U00002018";
        bool endOk = true;
        for (const char32_t c : cannotEnd) {
            if (!uiCannotEndLine(static_cast<u32>(c))) { endOk = false; AVER_ERROR("    uiCannotEndLine false for U+{:04X}", static_cast<u32>(c)); }
            std::u32string s;
            s.push_back(c);
            s.push_back(U'\U00004E2D');
            if (!no(s, 1)) { endOk = false; AVER_ERROR("    a line may end with U+{:04X}", static_cast<u32>(c)); }
        }
        check(endOk, "every cannot-end character stays with what follows");
        check(!uiCannotEndLine(0x4E2D) && !uiCannotEndLine(')'), "closing brackets may end a line");
    }

    AVER_INFO("=== random strings ===");
    {
        const std::u32string alphabet =
            U"abAB19 \n\r\t-/.,;:!?()[]{}\"'$%\U000000A0\U0000200B\U00002060\U00000301\U00004E2D\U00006587\U00003002\U0000300C\U0000300D\U000030FC\U00003042\U00003041"
            U"\U000005D0\U00000627\U00002026\u0085\U00002028\U000000AD";
        u32 seed = 777u;
        const auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        bool shape = true, hardOk = true, spaceOk = true, crlfOk = true;
        for (int n = 0; n < 20000; ++n) {
            std::u32string s;
            const u32 len = rnd() % 14;
            for (u32 k = 0; k < len; ++k) s.push_back(alphabet[rnd() % alphabet.size()]);
            const std::vector<UiBreak> b = breaksOf(s);
            if (b.size() != s.size() + 1) { shape = false; continue; }
            if (!s.empty() && (b[0] != UiBreak::No || b[s.size()] != UiBreak::Mandatory)) shape = false;
            for (usize i = 1; i < s.size(); ++i) {
                const UiLb prev = uiLineBreakClassOf(static_cast<u32>(s[i - 1]));
                const UiLb cur = uiLineBreakClassOf(static_cast<u32>(s[i]));
                if (b[i] == UiBreak::Mandatory && prev != UiLb::BK && prev != UiLb::CR && prev != UiLb::LF && prev != UiLb::NL) hardOk = false;
                const bool prevHard = prev == UiLb::BK || prev == UiLb::CR || prev == UiLb::LF || prev == UiLb::NL;   // LB4/5 outrank LB7
                if (cur == UiLb::SP && !prevHard && b[i] != UiBreak::No) spaceOk = false;
                if (prev == UiLb::CR && cur == UiLb::LF && b[i] != UiBreak::No) crlfOk = false;
            }
        }
        check(shape, "always n + 1 entries, [0] is No and [n] is Mandatory");
        check(hardOk, "Mandatory only follows a hard-break character");
        check(spaceOk, "never a break before a space (a hard break still wins)");
        check(crlfOk, "never a break inside CR LF");
    }

    if (g_failures == 0) AVER_INFO("=== all line-break tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
