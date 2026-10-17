// Headless test for the bidi subset: paragraph level, explicit embeddings / overrides / isolates, weak and
// neutral resolution, implicit levels, L1, L2 reordering and mirroring, with hand-computed cases and
// invariants over random strings.
#include "aver/core/Log.hpp"
#include "aver/ui/UiBidi.hpp"

#include <algorithm>
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

static UiBidiResult resolve(const std::u32string& s, UiTextDir d = UiTextDir::Auto) {
    const std::vector<u32> c = cps(s);
    UiBidiResult r;
    uiBidiResolve(c.data(), c.size(), d, r);
    return r;
}

// "0011": the resolved level of each code point.
static std::string levelsOf(const std::u32string& s, UiTextDir d = UiTextDir::Auto) {
    const UiBidiResult r = resolve(s, d);
    std::string out;
    for (const u8 l : r.levels) out += static_cast<char>('0' + l);
    return out;
}

// The logical index shown at each visual position, after L1, as digits.
static std::string visualOf(const std::u32string& s, UiTextDir d = UiTextDir::Auto) {
    UiBidiResult r = resolve(s, d);
    uiBidiResetTrailing(r.classes.data(), r.levels.data(), r.levels.size(), r.paragraphLevel);
    std::vector<u32> order;
    uiBidiReorderLine(r.levels.data(), r.levels.size(), order);
    std::string out;
    for (const u32 i : order) out += static_cast<char>('0' + i);
    return out;
}

// The text as displayed: visual order, with mirrored characters swapped at odd levels (L4).
static std::u32string displayed(const std::u32string& s, UiTextDir d = UiTextDir::Auto) {
    UiBidiResult r = resolve(s, d);
    uiBidiResetTrailing(r.classes.data(), r.levels.data(), r.levels.size(), r.paragraphLevel);
    std::vector<u32> order;
    uiBidiReorderLine(r.levels.data(), r.levels.size(), order);
    std::u32string out;
    for (const u32 i : order) {
        u32 c = static_cast<u32>(s[i]);
        if (r.levels[i] & 1) c = uiMirror(c);
        out.push_back(static_cast<char32_t>(c));
    }
    return out;
}

int main() {
    AVER_INFO("=== paragraph level ===");
    {
        check(resolve(U"abc").paragraphLevel == 0, "first strong L: left to right");
        check(resolve(U"\U000005D0\U000005D1").paragraphLevel == 1, "first strong R: right to left");
        check(resolve(U"\U00000627\U00000628").paragraphLevel == 1, "first strong AL: right to left");
        check(resolve(U"123 !").paragraphLevel == 0, "no strong letter: left to right");
        check(resolve(U"  \U000005D0").paragraphLevel == 1, "neutrals before the first strong are skipped");
        check(resolve(U"abc", UiTextDir::RTL).paragraphLevel == 1 && resolve(U"\U000005D0", UiTextDir::LTR).paragraphLevel == 0, "an explicit direction wins");
        check(resolve(U"\U00002067\U000005D0\U00002069" U"a").paragraphLevel == 0, "a letter inside an isolate does not set the level (P2)");
        check(resolve(U"\U00002067" U"a\U00002069\U000005D0").paragraphLevel == 1, "and the first one outside does");
        check(resolve(U"").paragraphLevel == 0 && resolve(U"").levels.empty(), "empty text");
        check(uiBidiParagraphLevel(cps(U"\U000005D0").data(), 1, UiTextDir::Auto) == 1, "uiBidiParagraphLevel");
    }

    AVER_INFO("=== basic runs ===");
    {
        check(levelsOf(U"abc") == "000", "Latin is level 0");
        check(levelsOf(U"\U000005D0\U000005D1\U000005D2") == "111", "Hebrew in an RTL paragraph is level 1");
        check(visualOf(U"\U000005D0\U000005D1\U000005D2") == "210", "and is shown reversed");
        check(levelsOf(U"abc", UiTextDir::RTL) == "222", "Latin in a forced RTL paragraph is level 2");
        check(visualOf(U"abc", UiTextDir::RTL) == "012", "and keeps its order");
        check(levelsOf(U"ab \U000005D0\U000005D1\U000005D2") == "000111", "LTR text then Hebrew: the space between L and R takes the embedding level (N2)");
        check(visualOf(U"ab \U000005D0\U000005D1\U000005D2") == "012543", "the Hebrew run is reversed in place");
        check(levelsOf(U"\U000005D0\U000005D1 cd") == "11122", "Hebrew then Latin: the space is R (embedding), the Latin is level 2");
        check(visualOf(U"\U000005D0\U000005D1 cd") == "34210", "Latin on the left, Hebrew reading right to left");
        check(levelsOf(U"a \U000005D0") == "001" && levelsOf(U"\U000005D0 a", UiTextDir::RTL) == "112", "mixed pairs");
    }

    AVER_INFO("=== weak types ===");
    {
        check(levelsOf(U"\U000005D0\U000005D1 123") == "111222", "W: European digits after Hebrew read left to right at level 2");
        check(visualOf(U"\U000005D0\U000005D1 123") == "345210", "digits keep their order, left of the Hebrew");
        check(levelsOf(U"\U00000627 12") == "1122", "W2: digits after an Arabic letter become Arabic numbers (level 2)");
        check(visualOf(U"\U00000627 12") == "2310", "and read left to right");
        check(levelsOf(U"\U00000627" U"1,2") == "1222", "W4: a comma between two Arabic numbers joins them");
        check(levelsOf(U"1+2") == "000", "W4/W7: 1+2 in LTR text is all level 0");
        check(levelsOf(U"1+2", UiTextDir::RTL) == "222", "in RTL text the whole sum is one level-2 run");
        check(visualOf(U"1+2", UiTextDir::RTL) == "012", "kept in order");
        check(levelsOf(U"$5") == "00" && levelsOf(U"$5", UiTextDir::RTL) == "22", "W5: a currency sign joins the number after it");
        check(levelsOf(U"5%", UiTextDir::RTL) == "22", "and a percent sign the number before it");
        check(levelsOf(U"1,2", UiTextDir::RTL) == "222", "W4: a comma between European numbers");
        check(levelsOf(U"1,,2", UiTextDir::RTL) != "2222", "W4 needs a SINGLE separator");
        check(levelsOf(U"\U000005D0\U000005B6") == "11", "W1: a nonspacing mark takes the class of its base");
        check(levelsOf(U"a\U0000200Fb") == "010", "an RLM is strong and zero width");
        check(levelsOf(U"\U00000661\U00000662", UiTextDir::LTR) == "22", "Arabic-Indic digits are level 2 in an LTR paragraph (I1)");
    }

    AVER_INFO("=== neutrals ===");
    {
        check(levelsOf(U"a ! b") == "00000", "N1: neutrals between two L take L");
        check(levelsOf(U"\U000005D0 ! \U000005D1") == "11111", "N1: neutrals between two R take R");
        check(levelsOf(U"a \U000005D0") == "001", "N2: neutrals between different directions take the embedding direction");
        check(levelsOf(U"1 \U000005D0", UiTextDir::LTR) == "001", "W7: a digit at the start of LTR text is a left-to-right letter");
        check(levelsOf(U"\U000005D0 1 \U000005D1") == "11211", "N1: a space between R and EN (R-like) is R");
        check(levelsOf(U"\t") == "0", "tab (S) alone");
    }

    AVER_INFO("=== explicit embeddings, overrides, isolates ===");
    {
        check(levelsOf(U"a\U0000202B\U000005D0\U000005D1\U0000202C" U"c") == "001110", "RLE ... PDF puts the Hebrew at level 1 (controls sit with what precedes them)");
        check(visualOf(U"a\U0000202B\U000005D0\U000005D1\U0000202C" U"c") == "014325", "reordered");
        check(levelsOf(U"\U0000202Eab\U0000202C") == "0111", "RLO forces R: Latin letters become level 1");
        check(visualOf(U"\U0000202Eab\U0000202C") == "0213", "and read right to left (the trailing PDF returns to the paragraph level, L1)");
        check(levelsOf(U"\U0000202D\U000005D0\U000005D1\U0000202C", UiTextDir::RTL) == "1222", "LRO in an RTL paragraph forces L at level 2");
        check(levelsOf(U"a\U0000202A" U"b\U0000202C") == "0022" && visualOf(U"a\U0000202A" U"b\U0000202C") == "0123", "LRE in LTR text raises the level but changes no order");
        check(levelsOf(U"a\U00002067\U000005D0\U000005D1\U00002069" U"c") == "001110", "isolates are treated as plain embeddings");
        check(levelsOf(U"\U00002068\U000005D0\U000005D1\U00002069") == "0111" && resolve(U"\U00002068\U000005D0\U000005D1\U00002069").levels[1] == 1,
              "FSI takes the direction of the first strong letter inside");
        check(levelsOf(U"\U0000202C\U0000202C" U"a") == "000", "unmatched PDF is ignored");
        check(levelsOf(U"\U00002069" U"a") == "00", "and so is an unmatched PDI");

        std::u32string deep;
        for (int i = 0; i < 130; ++i) deep.push_back(i % 2 ? U'\U0000202A' : U'\U0000202B');
        deep += U"x";
        const UiBidiResult r = resolve(deep);
        u8 mx = 0;
        for (const u8 l : r.levels) mx = std::max(mx, l);
        check(mx <= 126, "130 nested embeddings stay within the depth cap (max level " + std::to_string(mx) + ")");
        std::u32string unwound = deep;
        for (int i = 0; i < 130; ++i) unwound.push_back(U'\U0000202C');
        unwound += U"y";
        check(resolve(unwound).levels.back() == 0, "and unwinding returns to the paragraph level");
    }

    AVER_INFO("=== L1: trailing whitespace ===");
    {
        UiBidiClass cls[] = {UiBidiClass::L, UiBidiClass::WS, UiBidiClass::WS};
        u8 lv[] = {2, 2, 2};
        uiBidiResetTrailing(cls, lv, 3, 0);
        check(lv[0] == 2 && lv[1] == 0 && lv[2] == 0, "trailing spaces return to the paragraph level");

        UiBidiClass cls2[] = {UiBidiClass::R, UiBidiClass::WS, UiBidiClass::S, UiBidiClass::R, UiBidiClass::WS};
        u8 lv2[] = {1, 1, 1, 1, 1};
        uiBidiResetTrailing(cls2, lv2, 5, 0);
        check(lv2[0] == 1 && lv2[1] == 0 && lv2[2] == 0 && lv2[3] == 1 && lv2[4] == 0, "spaces before a separator, the separator, and trailing spaces");

        UiBidiClass cls3[] = {UiBidiClass::R, UiBidiClass::BN, UiBidiClass::WS};
        u8 lv3[] = {1, 1, 1};
        uiBidiResetTrailing(cls3, lv3, 3, 0);
        check(lv3[0] == 1 && lv3[1] == 0 && lv3[2] == 0, "formatting characters in the trailing run reset too");
    }

    AVER_INFO("=== L2: reordering ===");
    {
        std::vector<u32> order;
        const u8 even[] = {0, 0, 0};
        uiBidiReorderLine(even, 3, order);
        check(order == std::vector<u32>({0, 1, 2}), "all level 0 is the identity");
        const u8 mix[] = {0, 1, 1, 2, 2, 1, 0};
        uiBidiReorderLine(mix, 7, order);
        check(order == std::vector<u32>({0, 5, 3, 4, 2, 1, 6}), "levels 0 1 1 2 2 1 0 reorder to 0 5 3 4 2 1 6");
        const u8 odd[] = {1, 1, 1};
        uiBidiReorderLine(odd, 3, order);
        check(order == std::vector<u32>({2, 1, 0}), "an all-RTL line is reversed");
        const u8 nested[] = {1, 2, 2, 1};
        uiBidiReorderLine(nested, 4, order);
        check(order == std::vector<u32>({3, 1, 2, 0}), "a level-2 run inside level 1 keeps its own order");
        uiBidiReorderLine(even, 0, order);
        check(order.empty(), "an empty line");
    }

    AVER_INFO("=== mirroring (L4) ===");
    {
        check(displayed(U"(\U000005D0)") == U"(\U000005D0)", "parentheses around Hebrew look the same after reversal and mirroring");
        check(displayed(U"[\U000005D0\U000005D1]") == U"[\U000005D1\U000005D0]", "square brackets too");
        check(displayed(U"(a)") == U"(a)", "and Latin text is untouched");
        check(displayed(U"<\U000005D0>") == U"<\U000005D0>", "angle brackets");
        check(displayed(U"\U000005D0 (b)") == U"(b) \U000005D0", "an LTR group inside RTL text");
    }

    AVER_INFO("=== invariants over random strings ===");
    {
        const std::u32string alphabet =
            U"ab AB19 .,+-$%!()[]\U000005D0\U000005D1\U00000627\U00000628\U00000661\U00000662\U00000301\U0000200C\U0000200E\U0000200F\U0000202A\U0000202B\U0000202C\U0000202D\U0000202E\U00002066\U00002067\U00002068\U00002069\t\u0085";
        u32 seed = 424242u;
        const auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        bool sized = true, bounded = true, perm = true, para = true;
        for (int n = 0; n < 50000; ++n) {
            std::u32string s;
            const u32 len = rnd() % 16;
            for (u32 k = 0; k < len; ++k) s.push_back(alphabet[rnd() % alphabet.size()]);
            UiTextDir d = static_cast<UiTextDir>(rnd() % 3);
            UiBidiResult r = resolve(s, d);
            if (r.levels.size() != s.size() || r.classes.size() != s.size()) { sized = false; continue; }
            if (r.paragraphLevel > 1) para = false;
            for (const u8 l : r.levels) if (l > 126) bounded = false;
            uiBidiResetTrailing(r.classes.data(), r.levels.data(), r.levels.size(), r.paragraphLevel);
            std::vector<u32> order;
            uiBidiReorderLine(r.levels.data(), r.levels.size(), order);
            std::vector<u32> sorted = order;
            std::sort(sorted.begin(), sorted.end());
            for (usize i = 0; i < sorted.size(); ++i) if (sorted[i] != i) perm = false;
        }
        check(sized, "levels and classes have one entry per code point");
        check(bounded, "no level exceeds 126");
        check(para, "the paragraph level is 0 or 1");
        check(perm, "the visual order is a permutation of the line");
    }

    if (g_failures == 0) AVER_INFO("=== all bidi tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
