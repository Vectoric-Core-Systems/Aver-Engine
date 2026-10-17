// Headless test for the text engine: widths from advances and kerning, line metrics, alignment, wrapping
// (spaces, long words, CJK, kinsoku), ellipsis and maxLines, bidi and Arabic output, marks, rich text,
// caret stops, the layout cache, the glyph budget, raster sizes, the missing-glyph box and glyph quads.
// Fonts are synthetic rectangles (TestFont.hpp) so every number is arithmetic; Roboto is a smoke test.
#include "TestFont.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/ui/UiTextEngine.hpp"
#include "aver/ui/UiUnicode.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <memory>
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

static bool approx(f32 a, f32 b, f32 eps = 0.01f) { return std::fabs(a - b) <= eps; }

// ---- fixtures ------------------------------------------------------------------------------------

static std::vector<u8> latin(int boldExtra = 0) {
    testfont::Spec s = testfont::simpleSpec();
    for (auto& kv : s.chars) if (kv.second.advance > 0) kv.second.advance += boldExtra;
    s.chars['V'] = {620, 40, 0, 580, 700};
    s.chars[0x2026] = {1000, 100, 0, 900, 200};   // the ellipsis: one em
    s.chars['.'] = {250, 50, 0, 150, 100};
    s.chars['('] = {500, 50, 0, 450, 700};
    s.chars[')'] = {500, 50, 0, 450, 700};
    s.chars[0x3002] = {1000, 100, 0, 300, 200};   // ideographic full stop
    return testfont::buildFont(s);
}

static std::vector<u8> hebrewArabic() {
    testfont::Spec s;
    s.chars[0x05D0] = {400, 50, 0, 350, 700};
    s.chars[0x05D1] = {420, 50, 0, 370, 700};
    s.chars[0x05D2] = {440, 50, 0, 390, 700};
    s.chars[0xFE8D] = {300, 50, 0, 250, 700};     // alef isolated
    s.chars[0xFE8F] = {500, 50, 0, 450, 700};     // beh isolated
    s.chars[0xFE90] = {520, 50, 0, 470, 700};     // beh final
    s.chars[0xFE91] = {540, 50, 0, 490, 700};     // beh initial
    s.chars[0xFE92] = {560, 50, 0, 510, 700};     // beh medial
    s.chars[0xFEDF] = {400, 50, 0, 350, 700};     // lam initial
    s.chars[0xFEFB] = {440, 50, 0, 390, 700};     // lam-alef isolated
    s.chars[0xFEFC] = {450, 50, 0, 400, 700};     // lam-alef final
    s.chars[0x064E] = {0, 100, 800, 300, 900};    // fatha: a mark above, box bottom at 800
    s.chars[0x0651] = {0, 100, 600, 300, 800};    // shadda: a mark above that overlaps the base
    s.chars[0x0650] = {0, 100, -300, 300, -150};  // kasra: a mark below
    s.chars[0x0627] = {310, 50, 0, 260, 700};     // base letters, for the unjoined fallback
    s.chars[0x0628] = {510, 50, 0, 460, 700};
    s.chars[0x0644] = {410, 50, 0, 360, 700};
    return testfont::buildFont(s);
}

static std::vector<u8> cjk(int advance = 1000) {
    testfont::Spec s;
    for (const u32 c : {0x4E2Du, 0x6587u, 0x5B57u, 0x7B26u, 0x4E32u}) s.chars[c] = {advance, 100, 0, advance - 100, 800};
    s.chars[0x3002] = {advance, 100, 0, 300, 200};
    return testfont::buildFont(s);
}

static std::vector<u8> kerned() {
    testfont::Spec s = testfont::simpleSpec();
    s.kern = {{'A', 'V', -80}};
    s.chars['V'] = {620, 40, 0, 580, 700};
    return testfont::buildFont(s);
}

struct Env {
    UiTextEngine eng;
    std::map<std::string, std::vector<u8>> files;
    UiFontLoader loader;

    Env() {
        loader.open = [this](UiFontKind, const std::string& name, UiFontBlob& out) {
            const auto it = files.find(name);
            if (it == files.end()) return false;
            auto keep = std::make_shared<std::vector<u8>>(it->second);
            out.data = keep->data();
            out.size = keep->size();
            out.owner = keep;
            return true;
        };
    }
    bool load(const std::string& text) {
        std::string err;
        const bool ok = eng.loadFontSet(text, loader, &err);
        if (!ok) AVER_ERROR("    font set: {}", err);
        return ok;
    }
};

static const char* kBody =
    "OCFONTS 1\n"
    "DEFAULT Body\n"
    "FAMILY Body\n"
    "FACE latin.ttf\n"
    "FACE latinbold.ttf weight=bold\n"
    "FACE tall.ttf\n"
    "FACE hebar.ttf\n"
    "FACE cjk.ttf\n"
    "FAMILY Heading\n"
    "FACE kerned.ttf\n";

static void standardFiles(Env& env) {
    env.files["latin.ttf"] = latin();
    env.files["latinbold.ttf"] = latin(100);
    testfont::Spec t;
    t.ascent = 1000;
    t.descent = -300;
    t.chars[0x00C7] = {700, 50, 0, 650, 800};
    env.files["tall.ttf"] = testfont::buildFont(t);
    env.files["hebar.ttf"] = hebrewArabic();
    env.files["cjk.ttf"] = cjk();
    env.files["kerned.ttf"] = kerned();
}

static UiTextStyle style(f32 px = 20.0f) {
    UiTextStyle st;
    st.px = px;
    return st;
}

static std::string utf8(const std::u32string& s) {
    std::string out;
    for (const char32_t c : s) uiEncodeUtf8Append(out, static_cast<u32>(c));
    return out;
}

static std::shared_ptr<const UiTextLayout> lay(Env& e, const std::u32string& text, const UiTextStyle& st = style(),
                                               const UiTextBox& box = UiTextBox{}) {
    return e.eng.layout(utf8(text), st, box);
}

static std::shared_ptr<const UiTextLayout> layS(Env& e, const std::string& text, const UiTextStyle& st = style(),
                                                const UiTextBox& box = UiTextBox{}) {
    return e.eng.layout(text, st, box);
}

static std::vector<UiGlyphRef> lineGlyphs(const UiTextLayout& l, usize line) {
    std::vector<UiGlyphRef> out;
    if (line >= l.lines.size()) return out;
    for (u32 i = l.lines[line].glyphBegin; i < l.lines[line].glyphEnd; ++i) out.push_back(l.glyphs[i]);
    return out;
}

// The line's glyph ids, left to right.
static std::vector<u32> byX(const UiTextLayout& l, usize line = 0) {
    std::vector<UiGlyphRef> g = lineGlyphs(l, line);
    std::stable_sort(g.begin(), g.end(), [](const UiGlyphRef& a, const UiGlyphRef& b) { return a.x < b.x; });
    std::vector<u32> out;
    for (const UiGlyphRef& r : g) out.push_back(r.glyph);
    return out;
}

static u32 gid(Env& e, u32 cp, bool bold = false) {
    return e.eng.fontSet().resolve(e.eng.fontSet().familyIndex(""), bold, false, cp).glyph;
}

static u16 faceOf(Env& e, u32 cp, bool bold = false) {
    return e.eng.fontSet().resolve(e.eng.fontSet().familyIndex(""), bold, false, cp).face;
}

// Records the strings the default drawText hands to text().
class RecPainter final : public UiPainter {
public:
    explicit RecPainter(UiTextEngine& e) : m_(e) {}
    struct Call { f32 x, y; std::string s; f32 px; u32 rgba; };
    std::vector<Call> calls;
    void pushClip(const UiRect&) override {}
    void popClip() override {}
    void fillRect(const UiRect&, u32) override {}
    void image(const UiRect&, u64, const UiUvRect&, u32) override {}
    void text(f32 x, f32 y, std::string_view s, f32 px, u32 rgba) override { calls.push_back({x, y, std::string(s), px, rgba}); }
    f32 textWidth(std::string_view t, f32 px) const override { return m_.textWidth(t, px); }
    f32 lineHeight(f32 px) const override { return m_.lineHeight(px); }
    f32 ascent(f32 px) const override { return m_.ascent(px); }
    std::shared_ptr<const UiTextLayout> layoutText(std::string_view t, const UiTextStyle& s, const UiTextBox& b) const override {
        return m_.layoutText(t, s, b);
    }
private:
    UiEngineMetrics m_;
};

int main() {
    Env env;
    standardFiles(env);
    check(env.load(kBody), "the test font set loads");
    check(env.eng.usable(), "the engine is usable with a TrueType face");
    {
        // One frame runs the whole suite below: no budget, so no layout is left half rasterised.
        UiTextConfig cfg = env.eng.config();
        cfg.glyphBudget = 0;
        env.eng.configure(cfg);
    }
    env.eng.beginFrame();

    AVER_INFO("=== widths, kerning, letter spacing ===");
    {
        const auto l = lay(env, U"AB");
        check(l->lines.size() == 1 && approx(l->width, 12.0f + 12.2f), "width of AB at 20 px is the sum of advances (600 + 610) * 20 / 1000");
        check(l->glyphs.size() == 2 && l->glyphs[0].x == 0.0f && approx(l->glyphs[1].x, 12.0f), "glyph pen positions");
        check(l->glyphs[0].rasterPx == 20 && l->glyphs[0].face == faceOf(env, 'A'), "raster size 20, the Latin face");
        check(approx(lay(env, U"AB", style(10.0f))->width, 6.0f + 6.1f), "and at 10 px");
        check(approx(lay(env, U"A B")->width, 12.0f + 6.0f + 12.2f), "a space advances too");
        check(lay(env, U"A B")->glyphs.size() == 2, "but draws no glyph");

        UiTextStyle ls = style();
        ls.letterSpacing = 2.0f;
        check(approx(lay(env, U"AB", ls)->width, 24.2f + 4.0f), "letterSpacing is added after every character");

        UiTextStyle heading = style();
        heading.family = "Heading";
        const auto kv = lay(env, U"AV", heading);
        check(approx(kv->width, 12.0f + 12.4f - 1.6f), "AV in a kerning font: advances minus the pair (-80 units at 20 px)");
        check(approx(lay(env, U"AV")->width, 12.0f + 12.4f), "the same text in a font without kerning has no pair adjustment");
        check(kv->width < lay(env, U"A", heading)->width + lay(env, U"V", heading)->width, "kerned AV is narrower than A plus V");
        check(approx(lay(env, U"VA", heading)->width, 12.4f + 12.0f), "VA has no pair");
        check(approx(lay(env, U"A\tB")->width, 12.0f + 4 * 6.0f + 12.2f), "a tab is four spaces");
        check(lay(env, U"")->width == 0.0f && lay(env, U"")->lines.size() == 1, "empty text is one empty line");
        check(approx(lay(env, U"AB", style(20.0f))->width * 2, lay(env, U"AB", style(40.0f))->width, 0.1f), "width scales with the size");
    }

    AVER_INFO("=== line metrics ===");
    {
        const auto l = lay(env, U"AB");
        check(approx(l->lines[0].ascent, 16.0f) && approx(l->lines[0].descent, -4.0f) && approx(l->lines[0].height, 20.0f), "ascent 16, descent -4, line height 20 at 20 px");
        check(approx(l->lines[0].baseline, 16.0f) && l->lines[0].top == 0.0f, "baseline 16 below the line top");
        check(approx(l->height, 20.0f), "block height is the line height");
        UiTextStyle sp = style();
        sp.lineSpacing = 1.5f;
        const auto s2 = lay(env, U"A\nB", sp);
        check(approx(s2->lines[0].height, 30.0f) && approx(s2->lines[1].top, 30.0f) && approx(s2->height, 60.0f), "lineSpacing 1.5 makes 30 px lines");
        check(approx(s2->lines[0].baseline, 21.0f), "with half the extra above the baseline");

        const auto tall = lay(env, U"A\U000000C7");
        check(approx(tall->lines[0].ascent, 20.0f) && approx(tall->lines[0].descent, -6.0f) && approx(tall->lines[0].height, 26.0f),
              "a line that uses a taller face takes its ascent and descent (max of the faces used)");
        check(approx(lay(env, U"A")->lines[0].height, 20.0f), "a line of the regular face only does not");

        const auto two = lay(env, U"AB\nCD");
        check(two->lines.size() == 2 && two->lines[1].srcBegin == 3 && two->lines[0].srcEnd == 2, "a newline starts a new line; source indices skip it");
        check(approx(two->height, 40.0f) && approx(two->lines[1].top, 20.0f), "two lines are 40 px tall");
        check(lay(env, U"A\r\nB")->lines.size() == 2 && lay(env, U"A\rB")->lines.size() == 2, "CR LF and CR are one break each");
        const auto trailing = lay(env, U"A\n");
        check(trailing->lines.size() == 2 && trailing->lines[1].srcBegin == trailing->lines[1].srcEnd, "a trailing newline leaves an empty last line");
        check(lay(env, U"A\U00002028" U"B")->lines.size() == 2, "LS breaks a line inside a paragraph");
        check(lay(env, U"A\n\nB")->lines.size() == 3, "an empty paragraph is an empty line");
        check(layS(env, "A\xC2\x85" "B")->lines.size() == 2, "NEL is a paragraph break");
    }

    AVER_INFO("=== alignment through a painter ===");
    {
        RecPainter rp(env.eng);
        const auto l = lay(env, U"AB");
        const UiRect area{0, 0, 100, 50};
        const f32 w = l->width;
        rp.drawText(*l, area, UiTextAlign::Left, false, 0xFFFFFFFF);
        rp.drawText(*l, area, UiTextAlign::Center, false, 0xFFFFFFFF);
        rp.drawText(*l, area, UiTextAlign::Right, false, 0xFFFFFFFF);
        rp.drawText(*l, area, UiTextAlign::Start, false, 0xFFFFFFFF);
        rp.drawText(*l, area, UiTextAlign::End, false, 0xFFFFFFFF);
        check(rp.calls.size() == 5, "one text() call per line per draw");
        check(approx(rp.calls[0].x, 0.0f) && approx(rp.calls[1].x, (100.0f - w) * 0.5f) && approx(rp.calls[2].x, 100.0f - w), "left, centre, right");
        check(approx(rp.calls[3].x, 0.0f) && approx(rp.calls[4].x, 100.0f - w), "start is left and end is right in LTR text");
        check(approx(rp.calls[0].y, 16.0f) && rp.calls[0].s == "AB" && approx(rp.calls[0].px, 20.0f), "the baseline and the line text");

        rp.calls.clear();
        rp.drawText(*l, area, UiTextAlign::Left, true, 0xFFFFFFFF);
        check(approx(rp.calls[0].y, (50.0f - 20.0f) * 0.5f + 16.0f), "centred vertically in the area");

        UiTextStyle rtl = style();
        rtl.dir = UiTextDir::RTL;
        const auto r = lay(env, U"\U000005D0\U000005D1", rtl);
        check(r->lines[0].rtl, "an RTL paragraph reports rtl lines");
        rp.calls.clear();
        rp.drawText(*r, area, UiTextAlign::Start, false, 0xFFFFFFFF);
        rp.drawText(*r, area, UiTextAlign::End, false, 0xFFFFFFFF);
        rp.drawText(*r, area, UiTextAlign::Left, false, 0xFFFFFFFF);
        check(approx(rp.calls[0].x, 100.0f - r->width) && approx(rp.calls[1].x, 0.0f) && approx(rp.calls[2].x, 0.0f),
              "in RTL text Start is the right edge, End the left, Left stays physical");
    }

    AVER_INFO("=== wrapping ===");
    {
        UiTextBox wrap;
        wrap.wrap = true;
        wrap.maxWidth = 80.0f;
        const auto l = lay(env, U"AAA BBB CCC", style(), wrap);
        check(l->lines.size() == 2, "AAA BBB CCC wraps into two lines at 80 px");
        check(approx(l->lines[0].width, 36.0f + 6.0f + 36.6f) && approx(l->lines[1].width, 37.2f), "line widths exclude the hanging space");
        check(l->lineText(0) == "AAA BBB " && l->lineText(1) == "CCC", "the space stays with the line it ended");
        check(l->lines[0].srcBegin == 0 && l->lines[0].srcEnd == 8 && l->lines[1].srcBegin == 8 && l->lines[1].srcEnd == 11, "source ranges tile the text");
        check(approx(l->width, 78.6f) && approx(l->height, 40.0f), "block width is the widest line, height two lines");

        wrap.maxWidth = 50.0f;
        const auto w2 = lay(env, U"AAAAAAAAAA", style(), wrap);
        check(w2->lines.size() == 3, "a word wider than the box breaks between characters");
        check(w2->lines[0].srcEnd == 4 && w2->lines[1].srcEnd == 8 && w2->lines[2].srcEnd == 10, "4, 4 and 2 characters");
        wrap.maxWidth = 5.0f;
        check(lay(env, U"AAA", style(), wrap)->lines.size() == 3, "at least one character per line");

        wrap.maxWidth = 70.0f;
        const auto cj = lay(env, U"\U00004E2D\U00006587\U00005B57\U00007B26\U00004E32", style(), wrap);
        check(cj->lines.size() == 2 && cj->lines[0].srcEnd == 3 && cj->lines[1].srcEnd == 5, "CJK breaks between any two characters: 3 + 2 at 20 px each");

        const auto kin = lay(env, U"\U00004E2D\U00006587\U00005B57\U00003002\U00007B26", style(), wrap);
        check(kin->lines.size() == 2 && kin->lines[1].srcBegin == 2, "kinsoku (line breaking): the full stop travels with the character before it");

        wrap.maxWidth = 35.0f;
        const auto pd = lay(env, U"abc\U00003002", style(), wrap);
        check(pd->lines.size() == 2 && pd->lines[0].srcEnd == 2 && pd->lines[1].srcBegin == 2, "kinsoku push-down: abc followed by a full stop at 35 px puts c with the stop on line 2");
        const auto pe = lay(env, U"ab(c", style(), wrap);
        check(pe->lines.size() == 2 && pe->lines[0].srcEnd == 2, "kinsoku push-down: an opening bracket is never last on a line");

        UiTextBox noWrap;
        noWrap.maxWidth = 20.0f;
        check(lay(env, U"AAA BBB CCC", style(), noWrap)->lines.size() == 1, "wrap off keeps one line per paragraph");
        check(lay(env, U"AAA\nBBB", style(), noWrap)->lines.size() == 2, "but hard newlines still break");
    }

    AVER_INFO("=== ellipsis and maxLines ===");
    {
        UiTextBox box;
        box.wrap = true;
        box.ellipsis = true;
        box.maxWidth = 100.0f;
        box.maxLines = 2;
        const auto l = lay(env, U"AAAA BBBB CCCC DDDD", style(), box);
        check(l->lines.size() == 2, "maxLines 2 keeps two lines");
        check(approx(l->lines[1].width, 48.8f + 20.0f), "the last line is BBBB plus an ellipsis one em wide");
        check(!l->glyphs.empty() && l->glyphs.back().glyph == gid(env, 0x2026), "and ends with the ellipsis glyph");
        check(l->lines[1].srcEnd == 9 && l->lineText(1) == "BBBB", "its source range stops after BBBB (the trailing space is dropped)");
        UiTextBox plain = box;
        plain.ellipsis = false;
        const auto c = lay(env, U"AAAA BBBB CCCC DDDD", style(), plain);
        check(c->lines.size() == 2 && approx(c->lines[1].width, 48.8f), "without ellipsis maxLines just cuts");

        UiTextBox one;
        one.ellipsis = true;
        one.maxWidth = 60.0f;
        const auto e1 = lay(env, U"AAAAAAAA", style(), one);
        check(e1->lines.size() == 1 && approx(e1->lines[0].width, 3 * 12.0f + 20.0f), "a single wide line is cut to 3 letters + the ellipsis (56 px of 60)");
        check(e1->lines[0].srcEnd == 3, "the kept source range is 3 characters");
        check(lay(env, U"AA", style(), one)->glyphs.size() == 2, "a line that fits gets no ellipsis");

        // right to left: the mark lands at the visual end, which is the left
        UiTextBox rtlBox = one;
        rtlBox.maxWidth = 30.0f;
        UiTextStyle rtl = style();
        rtl.dir = UiTextDir::RTL;
        const auto e3 = lay(env, U"\U000005D0\U000005D1\U000005D2\U000005D0\U000005D1\U000005D2\U000005D0\U000005D1", rtl, rtlBox);
        check(e3->lines[0].rtl && !byX(*e3).empty() && byX(*e3).front() == gid(env, 0x2026), "RTL: the ellipsis is the leftmost glyph");
        check(e3->lines[0].width <= 30.01f, "and the line fits");

        // a font without U+2026 uses three full stops
        Env noEll;
        testfont::Spec s = testfont::simpleSpec();
        s.chars['.'] = {250, 50, 0, 150, 100};
        noEll.files["latin.ttf"] = testfont::buildFont(s);
        noEll.load("OCFONTS 1\nFAMILY F\nFACE latin.ttf\n");
        UiTextBox b2;
        b2.ellipsis = true;
        b2.maxWidth = 60.0f;
        const auto e2 = layS(noEll, "AAAAAAAA", style(), b2);
        check(approx(e2->lines[0].width, 3 * 12.0f + 3 * 5.0f), "without U+2026 the mark is three full stops (36 + 15)");
        const u32 dot = noEll.eng.fontSet().resolve(0, false, false, '.').glyph;
        check(e2->glyphs.size() == 6 && e2->glyphs[3].glyph == dot && e2->glyphs[5].glyph == dot, "three '.' glyphs end the line");
    }

    AVER_INFO("=== bidi and Arabic ===");
    {
        const auto mixed = lay(env, U"AB \U000005D0\U000005D1");
        check(!mixed->lines[0].rtl, "Latin first makes an LTR paragraph");
        const std::vector<u32> order = byX(*mixed);
        check(order == std::vector<u32>({gid(env, 'A'), gid(env, 'B'), gid(env, 0x05D1), gid(env, 0x05D0)}),
              "Latin left to right, then the Hebrew run reversed in place (bet left of alef)");
        const std::vector<UiGlyphRef> g = lineGlyphs(*mixed, 0);
        check(approx(g[2].x, 30.2f) && approx(g[3].x, 30.2f + 8.4f), "positions: after AB and the space, bet then alef");

        const auto heFirst = lay(env, U"\U000005D0\U000005D1 AB");
        check(heFirst->lines[0].rtl, "Hebrew first makes an RTL paragraph");
        check(byX(*heFirst) == std::vector<u32>({gid(env, 'A'), gid(env, 'B'), gid(env, 0x05D1), gid(env, 0x05D0)}),
              "and the Latin run sits on the left, reading left to right");

        const auto behs = lay(env, U"\U00000628\U00000628");
        check(behs->lines[0].rtl, "Arabic text is right to left");
        check(byX(*behs) == std::vector<u32>({gid(env, 0xFE90), gid(env, 0xFE91)}), "beh beh: the final form on the left, the initial form on the right");
        check(approx(behs->width, 10.8f + 10.4f), "its width is the two presentation-form advances");
        check(byX(*lay(env, U"\U00000628\U00000644\U00000627")) == std::vector<u32>({gid(env, 0xFEFC), gid(env, 0xFE91)}),
              "beh + lam-alef: the final ligature on the left of the initial beh");
        check(byX(*lay(env, U"\U00000628")) == std::vector<u32>({gid(env, 0xFE8F)}), "a lone beh is the isolated form");
        check(byX(*lay(env, U"\U00000628\U00000628\U00000628")) == std::vector<u32>({gid(env, 0xFE90), gid(env, 0xFE92), gid(env, 0xFE91)}),
              "three behs: final, medial, initial from the left");

        const auto lam = lay(env, U"\U00000644");
        check(lam->glyphs.size() == 1 && lam->glyphs[0].glyph == gid(env, 0x0644) && (lam->glyphs[0].flags & kGlyphUnjoined) != 0,
              "a letter whose presentation form the face lacks falls back to the base letter, flagged unjoined");

        UiTextStyle spaced = style();
        spaced.letterSpacing = 3.0f;
        check(approx(lay(env, U"\U00000628\U00000628", spaced)->width, 10.8f + 10.4f), "letterSpacing is ignored inside Arabic");
        check(approx(lay(env, U"AB", spaced)->width, 24.2f + 6.0f), "and applied to Latin");
    }

    AVER_INFO("=== marks ===");
    {
        const auto m = lay(env, U"\U00000628\U0000064E");
        check(m->glyphs.size() == 2 && approx(m->width, 10.0f), "a mark has no advance: beh + fatha is as wide as beh");
        const UiGlyphRef* mark = nullptr;
        const UiGlyphRef* base = nullptr;
        for (const UiGlyphRef& g : m->glyphs) ((g.flags & kGlyphMark) ? mark : base) = &g;
        check(mark && base, "one base glyph and one mark glyph");
        if (mark && base) {
            // base box 1..9 (centre 5), mark box 2..6 (centre 4): the mark moves right by 1
            check(approx(mark->x - base->x, 1.0f), "the mark's box centre is moved onto the base's box centre");
            check(mark->x + 2.0f >= base->x + 1.0f && mark->x + 6.0f <= base->x + 9.0f, "so it lies within the base's box horizontally");
            check(approx(mark->y, 0.0f), "a mark already clear of the base is not lifted");
        }
        const auto sh = lay(env, U"\U00000628\U00000651");
        for (const UiGlyphRef& g : sh->glyphs) if (g.flags & kGlyphMark) check(approx(g.y, -3.2f), "a mark that overlaps the base is lifted to clear it by 0.06 em (3.2 px)");
        const auto st = lay(env, U"\U00000628\U0000064E\U00000651");
        f32 lifts[2] = {0, 0};
        int n = 0;
        for (const UiGlyphRef& g : st->glyphs) if ((g.flags & kGlyphMark) && n < 2) lifts[n++] = g.y;
        check(n == 2 && approx(lifts[0], 0.0f) && approx(lifts[1], -7.2f), "a second mark above stacks on the top of the first");
        const auto below = lay(env, U"\U00000628\U00000650");
        for (const UiGlyphRef& g : below->glyphs) if (g.flags & kGlyphMark) check(approx(g.y, 0.0f), "a mark below that is already clear stays put");
        check(lay(env, U"\U00000628\U0000064E")->lines[0].stopEnd - lay(env, U"\U00000628\U0000064E")->lines[0].stopBegin == 2,
              "a base and its mark are one cluster: two caret stops (start and end)");
    }

    AVER_INFO("=== rich text ===");
    {
        UiTextBox rich;
        rich.rich = true;
        const auto b = lay(env, U"A<b>A</b>", style(), rich);
        check(b->source == "AA" && b->glyphs.size() == 2, "tags are stripped from the shown text");
        check(b->glyphs[0].face == faceOf(env, 'A') && b->glyphs[1].face == faceOf(env, 'A', true) && b->glyphs[0].face != b->glyphs[1].face,
              "<b> selects the bold face");
        check(approx(b->width, 12.0f + 14.0f), "the bold A is 700 units wide");

        const auto col = lay(env, U"<color=#FF0000>A</color>B", style(), rich);
        check(col->palette.size() == 2 && col->palette[1] == 0xFF0000FFu && col->glyphs[0].color == 1 && col->glyphs[1].color == 0,
              "<color> adds a palette entry used by the glyphs inside it only");
        UiTextBox plainColors = rich;
        plainColors.richColors = false;
        const auto noCol = lay(env, U"<color=#FF0000>A</color>B", style(), plainColors);
        check(noCol->palette.size() == 1 && noCol->glyphs[0].color == 0, "richColors off ignores colour spans");

        const auto sz = lay(env, U"<size=40>A</size>", style(), rich);
        check(sz->glyphs[0].rasterPx == 40 && approx(sz->width, 24.0f), "<size=40> rasterises at 40 px");
        UiTextStyle scaled = style();
        scaled.scale = 2.0f;
        check(lay(env, U"<size=10>A</size>", scaled, rich)->glyphs[0].rasterPx == 20, "<size> is design pixels, multiplied by the UI scale");
        check(lay(env, U"A<br>B", style(), rich)->lines.size() == 2, "<br> is a line break");
        check(lay(env, U"<<b>", style(), rich)->source == "<b>", "<< is a literal <");
        check(lay(env, U"a < b", style(), rich)->source == "a < b" && lay(env, U"<3", style(), rich)->source == "<3", "text that is not a tag stays");
        check(lay(env, U"<b>A", style(), rich)->glyphs[0].face == faceOf(env, 'A', true), "an unclosed tag closes at the end");
        const auto nest = lay(env, U"<b><i>A</i></b>B", style(), rich);
        check(nest->glyphs[0].face == faceOf(env, 'A', true) && (nest->glyphs[0].flags & kGlyphSkew) != 0 && nest->glyphs[1].face == faceOf(env, 'B'),
              "tags nest; italic with no italic face shears the quad; both end with their tags");
        const auto fam = lay(env, U"<font=Heading>AV</font>", style(), rich);
        check(approx(fam->width, 12.0f + 12.4f - 1.6f), "<font=Heading> switches family (and so applies that font's kerning)");
        check(layS(env, "<b>A", style(), UiTextBox{})->source == "<b>A", "without rich the tags are ordinary text");
        check(lay(env, U"<b>A</b>", style(), rich)->cpCount() == 1, "the layout's source has one code point per shown character");
    }

    AVER_INFO("=== caret stops ===");
    {
        const auto l = lay(env, U"ABC");
        bool round = true;
        for (usize i = 0; i <= 3; ++i) if (l->indexAt(l->caretX(i), 0.0f) != i) round = false;
        check(round, "indexAt(caretX(i)) == i for every index in LTR text");
        check(approx(l->caretX(0), 0.0f) && approx(l->caretX(3), l->width), "the first stop is 0, the last is the line width");
        check(approx(l->caretX(1), 12.0f) && approx(l->caretX(2), 24.2f), "stops sit at the advances");
        check(l->indexAt(11.0f, 0.0f) == 1 && l->indexAt(-5.0f, 0.0f) == 0 && l->indexAt(500.0f, 0.0f) == 3, "indexAt picks the nearest stop and clamps");

        const auto he = lay(env, U"\U000005D0\U000005D1\U000005D2");
        check(he->caretX(0) > he->caretX(1) && he->caretX(1) > he->caretX(2) && he->caretX(2) > he->caretX(3), "RTL text: stops run right to left in logical order");
        check(approx(he->caretX(0), he->width) && approx(he->caretX(3), 0.0f), "from the right edge to the left");
        bool rtlRound = true;
        for (usize i = 0; i <= 3; ++i) if (he->indexAt(he->caretX(i), 0.0f) != i) rtlRound = false;
        check(rtlRound, "and indexAt inverts them");

        const auto cl = lay(env, U"\U00000628\U0000064E");
        check(approx(cl->caretX(1), cl->caretX(0)), "a position inside a cluster snaps to the cluster start");

        UiTextBox wrap;
        wrap.wrap = true;
        wrap.maxWidth = 80.0f;
        const auto w = lay(env, U"AAA BBB CCC", style(), wrap);
        check(w->lineOfIndex(0) == 0 && w->lineOfIndex(7) == 0 && w->lineOfIndex(8) == 1 && w->lineOfIndex(11) == 1, "lineOfIndex follows the wrapped lines");
        check(approx(w->caretX(8), 0.0f) && approx(w->indexAt(1.0f, 25.0f) == 8 ? 0.0f : 1.0f, 0.0f), "the start of the second line has x 0 and a click on it finds it");
    }

    AVER_INFO("=== the layout cache ===");
    {
        UiTextEngine& e = env.eng;
        const u64 hitsBefore = e.stats().layoutHits;
        const auto a = lay(env, U"cache me");
        const auto b = lay(env, U"cache me");
        check(a.get() == b.get(), "the same text, style and box returns the cached layout");
        check(e.stats().layoutHits > hitsBefore, "and counts as a hit");
        check(lay(env, U"cache me", style(21.0f)).get() != a.get(), "a different size is a different layout");
        UiTextStyle bold = style();
        bold.bold = true;
        check(lay(env, U"cache me", bold).get() != a.get(), "so is bold");
        UiTextBox wrap;
        wrap.wrap = true;
        wrap.maxWidth = 40.0f;
        check(lay(env, U"cache me", style(), wrap).get() != a.get(), "and a wrap width");
        UiTextBox noWrapWide;
        noWrapWide.maxWidth = 40.0f;
        check(lay(env, U"cache me", style(), noWrapWide).get() == a.get(), "but a width that nothing uses (no wrap, no ellipsis) is not part of the key");

        e.setLanguage("de");
        check(lay(env, U"cache me").get() != a.get(), "a language change drops the cache (the font epoch moved)");
        e.setLanguage("");

        // raster size quantisation
        const auto p154 = lay(env, U"A", style(15.4f));
        const auto p152 = lay(env, U"A", style(15.2f));
        const auto p156 = lay(env, U"A", style(15.6f));
        check(p154.get() == p152.get() && p154.get() != p156.get(), "15.4 and 15.2 px share a key; 15.6 does not");
        check(p154->glyphs[0].rasterPx == 15 && p156->glyphs[0].rasterPx == 16, "rasterised at 15 and 16 px");
        check(approx(p154->width, 9.0f), "and laid out at the raster size (600 * 15 / 1000)");

        const auto big = lay(env, U"A", style(200.0f));
        check(big->glyphs[0].rasterPx == 160 && approx(big->glyphs[0].quad, 1.25f) && approx(big->width, 600 * 160 / 1000.0f * 1.25f, 0.05f),
              "above 160 px: rasterised at 160 and scaled by the sampler");
        const auto tiny = lay(env, U"A", style(2.0f));
        check(tiny->glyphs[0].rasterPx == 4 && approx(tiny->glyphs[0].quad, 0.5f), "below 4 px: rasterised at 4");

        // a reload clears the atlas and the cache
        const auto before = lay(env, U"reload");
        check(e.atlas().glyphsResident() > 0, "glyphs are resident");
        check(env.load(kBody), "the set reloads");
        check(e.atlas().glyphsResident() == 0 && e.stats().cacheEntries == 0, "a reload empties the atlas contents and the layout cache");
        check(lay(env, U"reload").get() != before.get(), "and layouts are rebuilt");
        bool allDirty = e.atlas().pageCount() > 0;
        for (u32 i = 0; i < e.atlas().pageCount(); ++i) if (!e.atlas().pageDirty(i)) allDirty = false;
        check(allDirty, "pages are kept and marked dirty");
    }

    AVER_INFO("=== the per-frame glyph budget ===");
    {
        Env b;
        standardFiles(b);
        b.load(kBody);
        UiTextConfig cfg = b.eng.config();
        cfg.glyphBudget = 3;
        b.eng.configure(cfg);
        const std::string text = "ABCDEFGHIJKLMNOPQRST";
        int frames = 0;
        bool firstIncomplete = false, cappedPerFrame = true, notStoredWhileIncomplete = true;
        std::shared_ptr<const UiTextLayout> last;
        for (; frames < 20; ) {
            b.eng.beginFrame();
            ++frames;
            last = layS(b, text);
            if (frames == 1) firstIncomplete = last->incomplete;
            if (b.eng.stats().rasterThisFrame > 3) cappedPerFrame = false;
            if (last->incomplete && b.eng.stats().cacheEntries != 0) notStoredWhileIncomplete = false;
            if (!last->incomplete) break;
        }
        check(firstIncomplete, "the first frame cannot rasterise 20 glyphs with a budget of 3");
        check(cappedPerFrame, "no frame rasterises more than 3");
        check(notStoredWhileIncomplete, "an incomplete layout is not cached");
        check(frames == 7, "20 distinct glyphs at 3 per frame complete on the 7th frame (took " + std::to_string(frames) + ")");
        check(b.eng.atlas().glyphsResident() == 20, "all 20 are resident");
        check(layS(b, text).get() == last.get(), "now the layout is cached");

        // prewarm ignores the budget
        b.eng.beginFrame();
        b.eng.prewarm("abcdefghijklmnopqrstuvwxyz", style());
        check(b.eng.atlas().glyphsResident() >= 46, "prewarm rasterises all 26 more at once");
        check(!layS(b, "abcdefghijklmnopqrstuvwxyz")->incomplete, "and the layout is then complete in the same frame");
    }

    AVER_INFO("=== atlas overflow ===");
    {
        Env b;
        standardFiles(b);
        b.load(kBody);
        UiTextConfig cfg = b.eng.config();
        cfg.pageSize = 256;
        cfg.maxPages = 1;
        b.eng.configure(cfg);
        b.eng.beginFrame();
        const auto big = layS(b, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", style(100.0f));
        check(big->incomplete, "26 glyphs of 100 px do not fit one 256 px page in one frame");
        check(b.eng.stats().overflow > 0, "the overflow is counted");
        check(b.eng.stats().cacheEntries == 0, "and the layout is not cached");
        b.eng.beginFrame();
        const auto again = layS(b, "ABCDEFGHIJKLMNOPQRSTUVWXYZ", style(100.0f));
        check(b.eng.stats().evictedPages > 0 || again->incomplete, "next frame the page is recycled (or the layout stays incomplete)");
    }

    AVER_INFO("=== missing glyphs and invisible characters ===");
    {
        const auto m = lay(env, U"A\U0001F600" U"B");
        check(m->glyphs.size() == 3 && (m->glyphs[1].flags & kGlyphBox) != 0 && m->glyphs[1].face == kUiFaceNone, "a character no face carries draws the missing-glyph box");
        check(approx(m->width, 12.0f + 0.6f * 20.0f + 12.2f), "the box advances 0.6 em");
        const auto z = lay(env, U"A\U0000200C" U"B");
        check(z->glyphs.size() == 2 && approx(z->width, 24.2f), "ZWNJ has no glyph and no width, and gets no box");
        check(lay(env, U"A\U0000200D" U"B")->glyphs.size() == 2 && lay(env, U"A\U00000001" U"B")->glyphs.size() == 2, "nor do ZWJ and control characters");
        const auto nb = lay(env, U"A\U000000A0" U"B");
        check(nb->glyphs.size() == 2 && approx(nb->width, 12.0f + 5.0f + 12.2f), "a no-break space no face carries advances a quarter em and draws nothing");
        check(lay(env, U"\U00000301")->glyphs.size() == 1 && (lay(env, U"\U00000301")->glyphs[0].flags & kGlyphBox) != 0, "a lone combining mark with no glyph is a box");
    }

    AVER_INFO("=== language-dependent faces ===");
    {
        Env h;
        h.files["ja.ttf"] = cjk(1000);
        h.files["zh.ttf"] = cjk(900);
        h.load("OCFONTS 1\nFAMILY Han\nFACE ja.ttf lang=ja\nFACE zh.ttf lang=zh-Hans\n");
        h.eng.setLanguage("ja");
        const auto ja = layS(h, "\xE4\xB8\xAD");
        h.eng.setLanguage("zh-CN");
        const auto zh = layS(h, "\xE4\xB8\xAD");
        check(approx(ja->width, 20.0f) && approx(zh->width, 18.0f), "a Han character takes the face of the UI language (ja 20 px, zh-Hans 18 px)");
        check(ja->glyphs[0].face != zh->glyphs[0].face, "from different faces");
    }

    AVER_INFO("=== glyph quads ===");
    {
        env.eng.beginFrame();
        const auto l = lay(env, U"AB");
        UiDrawList list;
        env.eng.emitLine(list, *l, 0, 10.4f, 50.4f, 0xFFFFFFFF);
        check(list.vertices().size() == 8 && list.indices().size() == 12, "two glyphs, two quads");
        check(list.totalCommands() == 1 && list.commands(UiLayer::Content)[0].texture == (kUiAtlasTokenBit | 0), "one command on atlas page 0's token");
        bool snapped = true;
        for (const UiVertex& v : list.vertices()) if (v.x != std::floor(v.x) || v.y != std::floor(v.y)) snapped = false;
        check(snapped, "quads sit on whole pixels");
        const UiAtlasGlyph* a = env.eng.atlas().lookup(UiGlyphKey{l->glyphs[0].face, l->glyphs[0].rasterPx, l->glyphs[0].glyph}, env.eng.frame());
        check(a != nullptr && a->w > 0, "the glyph is resident");
        if (a) {
            check(list.vertices()[0].x == 10.0f + a->offX && list.vertices()[0].y == 50.0f + a->offY, "the first quad is at round(origin) + the bitmap offset");
            check(list.vertices()[2].x - list.vertices()[0].x == a->w && list.vertices()[2].y - list.vertices()[0].y == a->h, "and is the bitmap's size, 1:1");
            const f32 ps = static_cast<f32>(env.eng.atlas().pageSize());
            check(approx(list.vertices()[0].u, a->x / ps, 1e-6f) && approx(list.vertices()[2].v, (a->y + a->h) / ps, 1e-6f), "UVs address the atlas rectangle");
        }
        UiDrawList red;
        env.eng.emitLine(red, *l, 0, 0, 20, 0xFF0000FF);
        check(red.vertices()[0].rgba == uiPremultiply(0xFF0000FF), "the caller's colour is applied");

        UiTextBox rich;
        rich.rich = true;
        const auto rc = lay(env, U"<color=#00FF00>A</color>", style(), rich);
        UiDrawList green;
        env.eng.emitLine(green, *rc, 0, 0, 20, 0x80FFFFFF);
        check(green.vertices()[0].rgba == uiPremultiply(0x8000FF00), "a rich colour keeps its RGB and takes the caller's opacity");

        const auto box = lay(env, U"\U0001F600");
        UiDrawList boxes;
        env.eng.emitLine(boxes, *box, 0, 0, 20, 0xFFFFFFFF);
        check(boxes.vertices().size() == 16 && boxes.commands(UiLayer::Content)[0].texture == 0, "a missing glyph is four white-texel rectangles");

        UiTextStyle it = style();
        it.italic = true;
        const auto sk = lay(env, U"A", it);
        UiDrawList skew;
        env.eng.emitLine(skew, *sk, 0, 0, 20, 0xFFFFFFFF);
        check(skew.vertices()[0].x != skew.vertices()[3].x, "a synthetic italic shears the quad");

        UiDrawList nothing;
        env.eng.emitLine(nothing, *l, 5, 0, 0, 0xFFFFFFFF);
        check(nothing.vertices().empty(), "a line index out of range emits nothing");
    }

    AVER_INFO("=== the baked bitmap font as the last face ===");
    {
        Env bm;
        bm.files["latin.ttf"] = latin();
        bm.load("OCFONTS 1\nFAMILY F\nFACE latin.ttf\nBITMAP legacy\n");
        UiFont font;
        font.pixelSize = 16;
        font.ascent = 12;
        font.descent = -4;
        font.lineHeight = 16;
        font.atlasTexture = 77;
        UiGlyph g;
        g.u0 = 0.1f; g.v0 = 0.2f; g.u1 = 0.3f; g.v1 = 0.4f;
        g.w = 8; g.h = 10; g.offX = 1; g.offY = -10; g.advance = 9;
        font.glyphs[0xE9] = g;
        bm.eng.setBitmapFont(&font);
        const auto l = layS(bm, "A\xC3\xA9");
        check(l->glyphs.size() == 2 && l->glyphs[1].face == kUiFaceBitmap && l->glyphs[1].glyph == 0xE9, "a character only the baked font has is drawn from it");
        check(approx(l->width, 12.0f + 9.0f * 20.0f / 16.0f), "scaled from its bake size to the requested one");
        UiDrawList list;
        bm.eng.emitLine(list, *l, 0, 0, 20, 0xFFFFFFFF);
        bool usesBitmap = false;
        for (const UiDrawCmd& c : list.commands(UiLayer::Content)) if (c.texture == 77) usesBitmap = true;
        check(usesBitmap, "and drawn with the bitmap font's own texture");
        bm.eng.setBitmapFont(nullptr);
        check(layS(bm, "A\xC3\xA9")->glyphs[1].face == kUiFaceNone, "withdrawn, it becomes a missing-glyph box again");
    }

    AVER_INFO("=== the engine behind the metrics interface ===");
    {
        UiEngineMetrics m(env.eng);
        check(approx(m.textWidth("AB", 20.0f), 24.2f) && approx(m.lineHeight(20.0f), 20.0f) && approx(m.ascent(20.0f), 16.0f), "UiEngineMetrics answers the three legacy virtuals");
        check(m.layoutText("AB", style(), UiTextBox{})->glyphs.size() == 2 && !m.layoutText("AB", style(), UiTextBox{})->legacy, "and layoutText with an engine layout");
        UiTextEngine empty;
        UiEngineMetrics em(empty);
        check(em.layoutText("AB", style(), UiTextBox{})->legacy, "an engine with no face answers with a legacy layout");
        check(!empty.usable() && !empty.hasGlyph("", 'A'), "and is not usable");
        check(env.eng.hasGlyph("", 'A') && !env.eng.hasGlyph("", 0x1F600) && env.eng.hasGlyph("Heading", 'V'), "hasGlyph asks the chain");

        // painter with the engine
        UiDrawList list;
        UiDrawListPainter painter(list, &env.eng, nullptr);
        painter.text(5.0f, 30.0f, "AB", 20.0f, 0xFFFFFFFF);
        check(list.vertices().size() == 8, "UiDrawListPainter::text draws through the engine when it has a TrueType face");
        UiDrawListPainter plain(list, nullptr);
        check(approx(plain.textWidth("AB", 20.0f), 20.0f * 2 * 0.55f), "and without one it measures with the estimate, as before");
    }

    AVER_INFO("=== Roboto smoke test ===");
    {
#ifdef AVER_REPO_ROOT
        std::vector<u8> bytes;
        if (readFileBytes(std::string(AVER_REPO_ROOT) + "/third_party/fonts/Roboto-Regular.ttf", bytes)) {
            Env r;
            r.files["Roboto-Regular.ttf"] = bytes;
            check(r.load("OCFONTS 1\nFAMILY R\nFACE Roboto-Regular.ttf\n"), "Roboto loads as a font set");
            const auto latinText = layS(r, "Hello", style(32.0f));
            const auto greek = layS(r, "\xCE\x93\xCE\xB5\xCE\xB9\xCE\xAC", style(32.0f));
            const auto cyrillic = layS(r, "\xD0\x9F\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82", style(32.0f));
            check(latinText->width > 0 && greek->width > 0 && cyrillic->width > 0, "Latin, Greek and Cyrillic all have positive widths");
            bool noBoxes = true;
            for (const UiTextLayout* l : {latinText.get(), greek.get(), cyrillic.get()})
                for (const UiGlyphRef& g : l->glyphs) if (g.flags & kGlyphBox) noBoxes = false;
            check(noBoxes, "and none needs the missing-glyph box");
            check(layS(r, "AV", style(32.0f))->width < layS(r, "A", style(32.0f))->width + layS(r, "V", style(32.0f))->width, "kerning: AV is narrower than A + V");
        } else {
            AVER_INFO("  (Roboto not found under AVER_REPO_ROOT: skipped)");
        }
#else
        AVER_INFO("  (AVER_REPO_ROOT is not defined: skipped)");
#endif
    }

    if (g_failures == 0) AVER_INFO("=== all text layout tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
