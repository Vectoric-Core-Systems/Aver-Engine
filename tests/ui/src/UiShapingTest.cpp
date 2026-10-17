// Headless test for Arabic shaping: joining types, positional forms, lam-alef, ZWJ / ZWNJ / tatweel,
// marks, the Persian letters, and the fixed-point / source-accounting invariants. The expected sequences
// are hand-derived from the Presentation Forms-B chart (FE70..FEFC).
#include "aver/core/Log.hpp"
#include "aver/ui/UiShaping.hpp"

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

using Cps = std::vector<u32>;

static std::vector<UiShapedCp> shape(const Cps& in) {
    std::vector<UiShapedCp> out;
    uiShapeText(in.data(), in.size(), out);
    return out;
}

static Cps cpsOf(const std::vector<UiShapedCp>& s) {
    Cps out;
    for (const UiShapedCp& u : s) out.push_back(u.cp);
    return out;
}

static Cps shaped(const Cps& in) { return cpsOf(shape(in)); }

static usize srcSum(const std::vector<UiShapedCp>& s) {
    usize n = 0;
    for (const UiShapedCp& u : s) n += u.srcCount;
    return n;
}

constexpr u32 ALEF = 0x0627, ALEF_MADDA = 0x0622, ALEF_HAMZA = 0x0623, ALEF_HAMZA_BELOW = 0x0625, BEH = 0x0628, TEH_MARBUTA = 0x0629,
              SEEN = 0x0633, AIN = 0x0639, LAM = 0x0644, MEEM = 0x0645, HEH = 0x0647, YEH = 0x064A, REH = 0x0631, DAL = 0x062F,
              HAH = 0x062D, TATWEEL = 0x0640, FATHA = 0x064E, ZWNJ = 0x200C, ZWJ = 0x200D, SPACE = 0x20;

int main() {
    AVER_INFO("=== joining types ===");
    {
        check(uiJoiningType(ALEF) == UiJoin::R && uiJoiningType(DAL) == UiJoin::R && uiJoiningType(REH) == UiJoin::R, "alef, dal, reh join to the right only");
        check(uiJoiningType(BEH) == UiJoin::D && uiJoiningType(LAM) == UiJoin::D && uiJoiningType(YEH) == UiJoin::D, "beh, lam, yeh join both ways");
        check(uiJoiningType(0x0621) == UiJoin::U, "hamza does not join");
        check(uiJoiningType(TEH_MARBUTA) == UiJoin::R && uiJoiningType(0x0648) == UiJoin::R, "teh marbuta and waw join to the right only");
        check(uiJoiningType(FATHA) == UiJoin::T && uiJoiningType(0x0651) == UiJoin::T && uiJoiningType(0x0670) == UiJoin::T, "marks are transparent");
        check(uiJoiningType(TATWEEL) == UiJoin::C && uiJoiningType(ZWJ) == UiJoin::C, "tatweel and ZWJ cause joining");
        check(uiJoiningType(ZWNJ) == UiJoin::U && uiJoiningType('a') == UiJoin::U && uiJoiningType(0x0660) == UiJoin::U, "ZWNJ, Latin and digits do not join");
        check(uiJoiningType(0x067E) == UiJoin::D && uiJoiningType(0x0686) == UiJoin::D && uiJoiningType(0x0698) == UiJoin::R &&
                  uiJoiningType(0x06AF) == UiJoin::D && uiJoiningType(0x06A9) == UiJoin::D && uiJoiningType(0x06CC) == UiJoin::D,
              "the Persian letters");

        // Every letter with a presentation-form row is consistent with its joining type.
        bool consistent = true;
        usize rows = 0;
        for (u32 cp = 0x0600; cp <= 0x06FF; ++cp) {
            if (uiArabicForm(cp, 0) == 0) continue;
            ++rows;
            if (cp == 0x0621) continue;   // hamza: isolated form only, joining type U
            const UiJoin t = uiJoiningType(cp);
            if (t != UiJoin::D && t != UiJoin::R) { consistent = false; AVER_ERROR("    U+{:04X} has forms but type {}", cp, static_cast<int>(t)); }
            if (uiArabicForm(cp, 2) != 0 && t != UiJoin::D) consistent = false;
            if (t == UiJoin::R && (uiArabicForm(cp, 2) != 0 || uiArabicForm(cp, 3) != 0)) consistent = false;
            if (uiArabicForm(cp, 1) == 0) { consistent = false; AVER_ERROR("    U+{:04X} has no final form", cp); }
        }
        check(consistent, "every letter with forms is D or R, and only D letters have initial forms");
        check(rows >= 70, "the form table has at least 70 rows (" + std::to_string(rows) + ")");
        check(uiArabicForm(BEH, 0) == 0xFE8F && uiArabicForm(BEH, 1) == 0xFE90 && uiArabicForm(BEH, 2) == 0xFE91 && uiArabicForm(BEH, 3) == 0xFE92, "beh: FE8F FE90 FE91 FE92");
        check(uiArabicForm('a', 0) == 0 && uiArabicForm(BEH, 4) == 0 && uiArabicForm(BEH, -1) == 0, "no forms for Latin or a bad index");
        check(uiArabicLamAlef(ALEF, false) == 0xFEFB && uiArabicLamAlef(ALEF, true) == 0xFEFC &&
                  uiArabicLamAlef(ALEF_MADDA, false) == 0xFEF5 && uiArabicLamAlef(ALEF_HAMZA, true) == 0xFEF8 &&
                  uiArabicLamAlef(ALEF_HAMZA_BELOW, false) == 0xFEF9 && uiArabicLamAlef('a', false) == 0,
              "lam-alef ligature code points");
        check(uiIsArabicShapable(BEH) && uiIsArabicShapable(ZWJ) && uiIsArabicShapable(FATHA) && !uiIsArabicShapable('a') && !uiIsArabicShapable(0xFE8F),
              "what takes part in shaping (presentation forms do not)");
    }

    AVER_INFO("=== words ===");
    {
        check(shaped({ALEF, LAM, AIN, REH, BEH, YEH, TEH_MARBUTA}) == Cps({0xFE8D, 0xFEDF, 0xFECC, 0xFEAE, 0xFE91, 0xFEF4, 0xFE94}),
              "the Arabic word for Arabic: isolated alef, initial lam, medial ain, final reh, initial beh, medial yeh, final teh marbuta");
        check(shaped({SEEN, LAM, ALEF, MEEM}) == Cps({0xFEB3, 0xFEFC, 0xFEE1}), "salam: initial seen, final lam-alef ligature, isolated meem");
        check(shaped({MEEM, HAH, MEEM, DAL}) == Cps({0xFEE3, 0xFEA4, 0xFEE4, 0xFEAA}), "Muhammad: initial, medial, medial, final dal");
        check(shaped({BEH, SEEN, MEEM, SPACE, ALEF, LAM, LAM, HEH}) == Cps({0xFE91, 0xFEB4, 0xFEE2, SPACE, 0xFE8D, 0xFEDF, 0xFEE0, 0xFEEA}),
              "bismillah: the space ends the joining, Allah is isolated alef, initial lam, medial lam, final heh");
        check(shaped({BEH}) == Cps({0xFE8F}) && shaped({ALEF}) == Cps({0xFE8D}) && shaped({HEH}) == Cps({0xFEE9}), "single letters are isolated");
        check(shaped({0x0621}) == Cps({0xFE80}), "hamza has only an isolated form");
        check(shaped({'a', 'b', SPACE, 'c'}) == Cps({'a', 'b', SPACE, 'c'}), "Latin passes through");
        check(shaped({'a', BEH, 'c'}) == Cps({'a', 0xFE8F, 'c'}) && shaped({0x0660, 0x0661}) == Cps({0x0660, 0x0661}),
              "an Arabic letter among Latin is isolated; digits are untouched");
    }

    AVER_INFO("=== lam-alef ===");
    {
        check(shaped({LAM, ALEF}) == Cps({0xFEFB}), "lam + alef isolated: FEFB");
        check(shaped({LAM, ALEF_HAMZA}) == Cps({0xFEF7}) && shaped({LAM, ALEF_HAMZA_BELOW}) == Cps({0xFEF9}) && shaped({LAM, ALEF_MADDA}) == Cps({0xFEF5}),
              "the other three alefs, isolated");
        check(shaped({BEH, LAM, ALEF}) == Cps({0xFE91, 0xFEFC}), "after a joining letter the ligature is final: FEFC");
        check(shaped({BEH, LAM, ALEF_HAMZA}) == Cps({0xFE91, 0xFEF8}) && shaped({BEH, LAM, ALEF_HAMZA_BELOW}) == Cps({0xFE91, 0xFEFA}) &&
                  shaped({BEH, LAM, ALEF_MADDA}) == Cps({0xFE91, 0xFEF6}),
              "and so are the other three");
        check(shaped({ALEF, LAM, ALEF}) == Cps({0xFE8D, 0xFEFB}), "after alef (which does not join forward) the ligature is isolated");
        check(shaped({DAL, LAM, ALEF}) == Cps({0xFEA9, 0xFEFB}), "after dal too");

        const std::vector<UiShapedCp> s = shape({BEH, LAM, ALEF});
        check(s.size() == 2 && s[1].srcCount == 2 && s[1].src == 1 && s[1].base == LAM && s[1].base2 == ALEF, "the ligature unit spans both source code points");
        check((s[1].flags & kShapeLigature) != 0 && (s[1].flags & kShapeJoined) != 0, "and is flagged");

        const std::vector<UiShapedCp> m = shape({LAM, FATHA, ALEF});
        check(m.size() == 2 && m[0].cp == 0xFEFB && m[0].srcCount == 2 && m[1].cp == FATHA && m[1].src == 1, "a mark between lam and alef still ligates and follows the ligature");
        check(srcSum(m) == 3, "and the source count still sums to 3");
        check(shaped({LAM, LAM, ALEF}) == Cps({0xFEDF, 0xFEFC}), "lam lam alef: initial lam then final ligature");
    }

    AVER_INFO("=== ZWJ, ZWNJ, tatweel, marks ===");
    {
        check(shaped({BEH, ZWNJ, BEH}) == Cps({0xFE8F, ZWNJ, 0xFE8F}), "ZWNJ breaks the join");
        check(shaped({BEH, ZWJ, BEH}) == Cps({0xFE91, ZWJ, 0xFE90}), "ZWJ forces it");
        check(shaped({BEH, TATWEEL, BEH}) == Cps({0xFE91, TATWEEL, 0xFE90}), "tatweel is dual-joining: beh joins through it");
        check(shaped({BEH, FATHA}) == Cps({0xFE8F, FATHA}), "a mark after a lone letter");
        check(shaped({BEH, FATHA, BEH}) == Cps({0xFE91, FATHA, 0xFE90}), "marks are transparent: the letters around them still join");
        check(shaped({ZWJ, BEH}) == Cps({ZWJ, 0xFE90}) && shaped({BEH, ZWJ}) == Cps({0xFE91, ZWJ}), "a ZWJ on either side joins towards it");

        const std::vector<UiShapedCp> t = shape({BEH, FATHA});
        check(t[1].cp == FATHA && t[1].srcCount == 1 && t[1].src == 1 && t[1].flags == 0, "a mark keeps its code point and source");
    }

    AVER_INFO("=== Persian and Urdu letters ===");
    {
        check(shaped({0x067E}) == Cps({0xFB56}) && shaped({0x0686}) == Cps({0xFB7A}) && shaped({0x0698}) == Cps({0xFB8A}) &&
                  shaped({0x06AF}) == Cps({0xFB92}) && shaped({0x06A9}) == Cps({0xFB8E}) && shaped({0x06CC}) == Cps({0xFBFC}),
              "peh, tcheh, jeh, gaf, keheh, farsi yeh: isolated forms");
        check(shaped({0x067E, 0x067E}) == Cps({0xFB58, 0xFB57}), "peh peh: initial, final");
        check(shaped({0x06CC, 0x06CC, 0x06CC}) == Cps({0xFBFE, 0xFBFF, 0xFBFD}), "farsi yeh x3: initial, medial, final");
        check(shaped({0x06AF, 0x06AF}) == Cps({0xFB94, 0xFB93}) && shaped({0x06A9, 0x06A9}) == Cps({0xFB90, 0xFB8F}), "gaf and keheh");
        check(shaped({0x0698, 0x0698}) == Cps({0xFB8A, 0xFB8A}), "jeh joins to the right only: two isolated forms");
        check(shaped({0x0686, BEH}) == Cps({0xFB7C, 0xFE90}), "tcheh then beh: initial tcheh, final beh");
        check(shaped({0x06BA, 0x06BA}) != Cps({0x06BA, 0x06BA}), "a two-form letter used in a joining position still maps (isolated / final)");
        const std::vector<UiShapedCp> nf = shape({0x06BA, 0x06BA});
        check(nf[0].cp == 0x06BA && (nf[0].flags & kShapeNoForm) != 0, "a position the letter has no form for keeps the base letter, flagged");
    }

    AVER_INFO("=== invariants ===");
    {
        const std::vector<Cps> samples = {
            {ALEF, LAM, AIN, REH, BEH, YEH, TEH_MARBUTA}, {SEEN, LAM, ALEF, MEEM}, {BEH, SPACE, 'a', BEH, FATHA, BEH},
            {LAM, FATHA, ALEF, BEH}, {0x067E, 0x0686, 0x0698, 0x06AF, 0x06A9, 0x06CC}, {BEH, ZWNJ, BEH, ZWJ, BEH, TATWEEL, BEH},
            {'x', 'y'}, {}, {FATHA}, {LAM}, {ALEF, ALEF}};
        bool fixed = true, sums = true, ordered = true;
        for (const Cps& s : samples) {
            const std::vector<UiShapedCp> once = shape(s);
            const Cps first = cpsOf(once);
            if (shaped(first) != first) { fixed = false; AVER_ERROR("    not a fixed point"); }
            if (srcSum(once) != s.size()) sums = false;
            u32 expectSrc = 0;
            for (const UiShapedCp& u : once) {
                if (u.src < expectSrc && !(u.src + 1 == expectSrc)) ordered = false;
                expectSrc = std::max(expectSrc, u.src + u.srcCount);
            }
        }
        check(fixed, "shaping an already shaped string changes nothing");
        check(sums, "srcCount sums to the input length");
        check(ordered, "units come out in source order");

        // Random strings from a mixed alphabet keep the accounting.
        const Cps alphabet = {ALEF, BEH, LAM, SEEN, AIN, DAL, REH, HEH, YEH, MEEM, FATHA, 0x0651, ZWNJ, ZWJ, TATWEEL, SPACE, 'a', 0x0660,
                              0x067E, 0x0621, ALEF_HAMZA, TEH_MARBUTA};
        u32 seed = 31337u;
        const auto rnd = [&]() { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
        bool ok = true;
        for (int n = 0; n < 20000; ++n) {
            Cps s;
            const u32 len = rnd() % 12;
            for (u32 k = 0; k < len; ++k) s.push_back(alphabet[rnd() % alphabet.size()]);
            const std::vector<UiShapedCp> once = shape(s);
            if (srcSum(once) != s.size()) ok = false;
            const Cps first = cpsOf(once);
            if (shaped(first) != first) ok = false;
            for (const UiShapedCp& u : once) if (u.src + u.srcCount > s.size()) ok = false;
        }
        check(ok, "20,000 random strings: source accounting and the fixed point hold");
    }

    if (g_failures == 0) AVER_INFO("=== all shaping tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
