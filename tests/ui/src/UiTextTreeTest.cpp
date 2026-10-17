// Headless test for the widget tree's text: with the estimated metrics (the legacy path) layout and draw
// are exactly today's, and with the text engine widgets measure and draw through one layout. Also the new
// text properties: the property table, the .ocui round trip, themes and rich text on widgets.
#include "TestFont.hpp"

#include "aver/core/Log.hpp"
#include "aver/ui/UiLayoutAsset.hpp"
#include "aver/ui/UiProps.hpp"
#include "aver/ui/UiTextEngine.hpp"
#include "aver/ui/UiTree.hpp"
#include "aver/ui/UiUnicode.hpp"

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

static bool approx(f32 a, f32 b, f32 eps = 0.02f) { return std::fabs(a - b) <= eps; }

static UiEstimatedMetrics g_metrics;

static UiWidget& mk(UiTree& t, UiWidgetKind k, UiWidgetId parent, const char* name) {
    return *t.get(t.create(k, parent, name));
}

static UiWidget& box(UiTree& t, UiWidgetId parent, const char* name) {
    UiWidget& w = mk(t, UiWidgetKind::Panel, parent, name);
    w.style = "clear";
    return w;
}

// A child placed at (x, y) in its parent; w / h < 0 leave that axis automatic.
static UiWidget& at(UiTree& t, UiWidgetKind k, UiWidgetId parent, const char* name, f32 x, f32 y, f32 w = -1, f32 h = -1) {
    UiWidget& c = mk(t, k, parent, name);
    c.offsets.left = x;
    c.offsets.top = y;
    c.width = w;
    c.height = h;
    c.fontSize = 20.0f;
    return c;
}

static UiInputFrame frame(f32 x = -100, f32 y = -100, u32 buttons = 0) {
    UiInputFrame f;
    f.pointerX = x;
    f.pointerY = y;
    f.buttons = buttons;
    return f;
}

// Records the painter calls the tree makes. With an engine it measures and lays out through it.
class Rec final : public UiPainter {
public:
    explicit Rec(UiTextEngine* e = nullptr) { if (e) em_ = std::make_unique<UiEngineMetrics>(*e); }
    struct Text { f32 x, y; std::string s; f32 px; u32 rgba; };
    struct Fill { UiRect r; u32 rgba; };
    std::vector<Text> texts;
    std::vector<Fill> fills;
    void clear() { texts.clear(); fills.clear(); }

    void pushClip(const UiRect&) override {}
    void popClip() override {}
    void fillRect(const UiRect& r, u32 rgba) override { fills.push_back({r, rgba}); }
    void image(const UiRect&, u64, const UiUvRect&, u32) override {}
    void text(f32 x, f32 y, std::string_view s, f32 px, u32 rgba) override { texts.push_back({x, y, std::string(s), px, rgba}); }
    f32 textWidth(std::string_view s, f32 px) const override { return em_ ? em_->textWidth(s, px) : g_metrics.textWidth(s, px); }
    f32 lineHeight(f32 px) const override { return em_ ? em_->lineHeight(px) : g_metrics.lineHeight(px); }
    f32 ascent(f32 px) const override { return em_ ? em_->ascent(px) : g_metrics.ascent(px); }
    std::shared_ptr<const UiTextLayout> layoutText(std::string_view s, const UiTextStyle& st, const UiTextBox& b) const override {
        return em_ ? em_->layoutText(s, st, b) : UiTextMetrics::layoutText(s, st, b);
    }
    const Text* find(const std::string& s) const {
        for (const Text& t : texts) if (t.s == s) return &t;
        return nullptr;
    }
private:
    std::unique_ptr<UiEngineMetrics> em_;
};

static void run(UiTree& t, Rec& rec, const UiInputFrame& in = frame()) {
    rec.clear();
    t.update(0.016f, in, rec);
    t.draw(rec);
}

// ---- engine fixtures -----------------------------------------------------------------------------

static std::vector<u8> latin() {
    testfont::Spec s = testfont::simpleSpec();
    s.chars['V'] = {620, 40, 0, 580, 700};
    return testfont::buildFont(s);
}

static std::vector<u8> latinBold() {
    testfont::Spec s = testfont::simpleSpec();
    for (auto& kv : s.chars) if (kv.second.advance > 0) kv.second.advance += 100;
    return testfont::buildFont(s);
}

static std::vector<u8> kerned() {
    testfont::Spec s = testfont::simpleSpec();
    s.kern = {{'A', 'V', -80}};
    s.chars['V'] = {620, 40, 0, 580, 700};
    return testfont::buildFont(s);
}

static std::vector<u8> hebrew() {
    testfont::Spec s;
    s.chars[0x05D0] = {400, 50, 0, 350, 700};
    s.chars[0x05D1] = {420, 50, 0, 370, 700};
    s.chars[0x05D2] = {440, 50, 0, 390, 700};
    return testfont::buildFont(s);
}

struct EngineEnv {
    UiTextEngine eng;
    std::map<std::string, std::vector<u8>> files;
    UiFontLoader loader;
    EngineEnv() {
        files["latin.ttf"] = latin();
        files["bold.ttf"] = latinBold();
        files["kerned.ttf"] = kerned();
        files["hebrew.ttf"] = hebrew();
        loader.open = [this](UiFontKind, const std::string& name, UiFontBlob& out) {
            const auto it = files.find(name);
            if (it == files.end()) return false;
            auto keep = std::make_shared<std::vector<u8>>(it->second);
            out.data = keep->data();
            out.size = keep->size();
            out.owner = keep;
            return true;
        };
        std::string err;
        const bool ok = eng.loadFontSet(
            "OCFONTS 1\nDEFAULT Body\nFAMILY Body\nFACE latin.ttf\nFACE bold.ttf weight=bold\nFACE hebrew.ttf\n"
            "FAMILY Heading\nFACE kerned.ttf\n",
            loader, &err);
        if (!ok) AVER_ERROR("    font set: {}", err);
        UiTextConfig cfg = eng.config();
        cfg.glyphBudget = 0;
        eng.configure(cfg);
    }
};

int main() {
    AVER_INFO("=== legacy path: a Text widget ===");
    {
        UiTree t;
        const UiWidgetId rid = box(t, 0, "Root").id;
        UiWidget& hello = at(t, UiWidgetKind::Text, rid, "T", 10, 20);
        hello.text = "Hello";
        const UiWidgetId id = hello.id;
        Rec rec;
        run(t, rec);
        check(approx(t.get(id)->rect.w, 55.0f) && approx(t.get(id)->rect.h, 25.0f), "Hello at 20 px measures 5 * 11 by 25 with the estimate");
        check(rec.texts.size() == 1 && rec.texts[0].s == "Hello", "it draws one string");
        check(approx(rec.texts[0].x, 10.0f) && approx(rec.texts[0].y, 20.0f + 19.0f) && approx(rec.texts[0].px, 20.0f), "at its left edge, baseline = top + ascent");
        UiVec2 size;
        i32 lines = 0;
        check(t.lastTextSize(id, size, lines) && approx(size.x, 55.0f) && approx(size.y, 25.0f) && lines == 1, "lastTextSize reports what the last layout measured");
        check(!t.lastTextSize(rid, size, lines) && !t.lastTextSize(9999, size, lines), "and nothing for a panel or an unknown widget");

        t.get(id)->text = "";
        run(t, rec);
        check(rec.texts.empty(), "empty text draws nothing");
        check(approx(t.get(id)->rect.h, 25.0f) && approx(t.get(id)->rect.w, 0.0f), "but still has a line's height");
    }

    AVER_INFO("=== legacy path: wrapping and alignment ===");
    {
        UiTree t;
        const UiWidgetId rid = box(t, 0, "Root").id;
        UiWidget& para = at(t, UiWidgetKind::Text, rid, "P", 0, 60, 100);
        para.text = "aa bb cc dd";
        para.wrap = true;
        const UiWidgetId pid = para.id;
        Rec rec;
        run(t, rec);
        check(approx(t.get(pid)->rect.h, 50.0f), "a wrapped paragraph is two lines tall");
        check(rec.texts.size() == 2 && rec.texts[0].s == "aa bb cc" && rec.texts[1].s == "dd", "uiWrapText's lines are drawn");
        check(approx(rec.texts[0].y, 60.0f + 19.0f) && approx(rec.texts[1].y, 60.0f + 25.0f + 19.0f), "a line height apart, top aligned");

        UiWidget& c = at(t, UiWidgetKind::Text, rid, "C", 0, 200, 200, 30);
        c.text = "Hi";
        c.textAlign = UiTextAlign::Center;
        UiWidget& r = at(t, UiWidgetKind::Text, rid, "R", 0, 240, 200, 30);
        r.text = "Ok";
        r.textAlign = UiTextAlign::Right;
        UiWidget& st = at(t, UiWidgetKind::Text, rid, "S", 0, 280, 200, 30);
        st.text = "St";
        st.textAlign = UiTextAlign::Start;
        UiWidget& en = at(t, UiWidgetKind::Text, rid, "E", 0, 320, 200, 30);
        en.text = "En";
        en.textAlign = UiTextAlign::End;
        run(t, rec);
        check(approx(rec.find("Hi")->x, (200.0f - 22.0f) * 0.5f), "centre");
        check(approx(rec.find("Ok")->x, 200.0f - 22.0f), "right");
        check(approx(rec.find("St")->x, 0.0f) && approx(rec.find("En")->x, 200.0f - 22.0f), "start is left and end is right when the text is left to right");
        check(approx(rec.find("Hi")->y, 200.0f + (30.0f - 25.0f) * 0.5f + 19.0f), "single lines are centred vertically in their box");
    }

    AVER_INFO("=== legacy path: buttons, toggles, sliders, choices ===");
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Root");
        root.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid = root.id;
        UiWidget& two = at(t, UiWidgetKind::Button, rid, "B2", 0, 0);
        two.text = "a\nb";
        UiWidget& none = at(t, UiWidgetKind::Button, rid, "B0", 0, 100);
        none.text = "";
        UiWidget& tog = at(t, UiWidgetKind::Toggle, rid, "Tg", 0, 200);
        tog.text = "Opt";
        UiWidget& sl = at(t, UiWidgetKind::Slider, rid, "Sl", 0, 300, 300, 30);
        sl.showValue = true;
        sl.value = 0.5f;
        UiWidget& ch = at(t, UiWidgetKind::Choice, rid, "Ch", 0, 400, 300, 40);
        ch.items = {"Low", "High"};
        Rec rec;
        run(t, rec);
        check(approx(t.get(two.id)->rect.h, 50.0f + 16.0f), "a two-line button is two line heights plus padding");
        std::vector<const Rec::Text*> bt;
        for (const Rec::Text& x : rec.texts) if (x.s == "a" || x.s == "b") bt.push_back(&x);
        check(bt.size() == 2 && approx(bt[1]->y - bt[0]->y, 25.0f), "both lines are drawn a line height apart");
        bool emptyDrawn = false;
        for (const Rec::Text& x : rec.texts) if (x.s.empty()) emptyDrawn = true;
        check(emptyDrawn, "an empty label is still handed to text() as one empty line, as before");
        const Rec::Text* opt = rec.find("Opt");
        check(opt && approx(opt->x, 22.0f + 8.0f) && approx(opt->px, 20.0f), "a toggle's label sits after its box (1.1 em) and 8 px");
        const Rec::Text* pct = rec.find("50%");
        check(pct && approx(pct->x, t.get(sl.id)->rect.right() - 33.0f), "a slider's value is right aligned (3 characters of 11)");
        const Rec::Text* sel = rec.find("Low");
        check(sel && approx(sel->x, t.get(ch.id)->rect.center().x - 16.5f), "a choice centres the selected item");
        check(rec.find("<") && rec.find(">"), "and draws its arrows as plain text");
    }

    AVER_INFO("=== legacy path: a text input ===");
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Root");
        root.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid = root.id;
        UiWidget& in = at(t, UiWidgetKind::TextInput, rid, "In", 0, 100, 300, 40);
        in.text = "abc";
        in.placeholder = "type";
        const UiWidgetId id = in.id;
        Rec rec;
        run(t, rec);
        check(rec.find("abc") && approx(rec.find("abc")->x, 10.0f), "typed text starts at the field's padding");
        check(rec.find("type") == nullptr, "the placeholder is hidden while there is text");

        run(t, rec, frame(25, 120, 1));
        check(t.focused() == id && t.get(id)->caret == 1, "a click 15 px into the text puts the caret after the first character (midpoint 16.5)");
        run(t, rec, frame(25, 120, 0));
        run(t, rec, frame(28, 120, 1));
        check(t.get(id)->caret == 2, "18 px in is past the second midpoint: after two characters");
        run(t, rec, frame(28, 120, 0));
        run(t, rec, frame(290, 120, 1));
        check(t.get(id)->caret == 3, "a click beyond the text goes to the end");
        run(t, rec, frame(290, 120, 0));
        run(t, rec, frame(12, 120, 1));
        check(t.get(id)->caret == 0, "and one on the left edge to the start");
        run(t, rec, frame(12, 120, 0));

        t.get(id)->caret = 2;
        run(t, rec, frame(-100, -100, 0));
        const Rec::Fill* caret = nullptr;
        for (const Rec::Fill& f : rec.fills) if (approx(f.r.w, 1.0f) && approx(f.r.h, 25.0f)) caret = &f;
        check(caret && approx(caret->r.x, 10.0f + 22.0f), "the caret is drawn after the width of the text before it");

        t.get(id)->text = "";
        t.get(id)->caret = 0;
        t.clearFocus();
        run(t, rec);
        check(rec.find("type") != nullptr, "an empty, unfocused field shows its placeholder");

        t.get(id)->text = "secret";
        t.get(id)->masked = true;
        run(t, rec);
        check(rec.find("******") != nullptr && rec.find("secret") == nullptr, "a masked field draws one * per character");
    }

    AVER_INFO("=== text properties ===");
    {
        UiWidgetProps p;
        check(uiSetProp(p, "fontFamily", "Heading") && p.fontFamily == "Heading", "fontFamily");
        check(uiSetProp(p, "bold", "true") && p.bold && uiSetProp(p, "italic", "1") && p.italic, "bold and italic");
        check(uiSetProp(p, "textDir", "rtl") && p.textDir == UiTextDir::RTL && uiSetProp(p, "textDir", "LTR") && p.textDir == UiTextDir::LTR, "textDir by name, any case");
        check(!uiSetProp(p, "textDir", "diagonal") && p.textDir == UiTextDir::LTR, "an unknown direction is refused and leaves the value");
        check(uiSetProp(p, "lineSpacing", "1.5") && approx(p.lineSpacing, 1.5f) && uiSetProp(p, "letterSpacing", "-2") && approx(p.letterSpacing, -2.0f), "lineSpacing and letterSpacing");
        check(uiSetProp(p, "ellipsis", "true") && p.ellipsis && uiSetProp(p, "maxLines", "3") && p.maxLines == 3 && uiSetProp(p, "rich", "yes") && p.rich, "ellipsis, maxLines, rich");
        check(uiSetProp(p, "textAlign", "start") && p.textAlign == UiTextAlign::Start && uiSetProp(p, "textAlign", "end") && p.textAlign == UiTextAlign::End, "textAlign start and end");
        std::string s;
        check(uiGetProp(p, "textAlign", s) && s == "end" && uiGetProp(p, "textDir", s) && s == "ltr" && uiGetProp(p, "fontFamily", s) && s == "Heading", "and they read back");
        check(static_cast<int>(UiTextAlign::Start) == 3 && static_cast<int>(UiTextAlign::End) == 4 && static_cast<int>(UiTextAlign::Right) == 2,
              "UiTextAlign keeps its old numbers and appends Start = 3, End = 4");

        const UiPropDesc* rich = uiFindProperty("rich");
        const UiPropDesc* fam = uiFindProperty("fontFamily");
        check(rich && !(rich->kindMask & uiKindBit(UiWidgetKind::TextInput)) && (rich->kindMask & uiKindBit(UiWidgetKind::Text)), "rich is offered for Text but not TextInput");
        check(fam && (fam->kindMask & uiKindBit(UiWidgetKind::TextInput)) && !(fam->kindMask & uiKindBit(UiWidgetKind::Image)), "fontFamily for text-bearing widgets only");

        UiLayoutDoc d = uiStarterLayout();
        const i32 n = uiLayoutAdd(d, 0, UiWidgetKind::Text, "Styled");
        UiWidgetProps& sp = d.nodes[static_cast<usize>(n)].props;
        sp.fontFamily = "Heading";
        sp.bold = true;
        sp.italic = true;
        sp.textDir = UiTextDir::RTL;
        sp.textAlign = UiTextAlign::Start;
        sp.lineSpacing = 1.25f;
        sp.letterSpacing = 2.5f;
        sp.ellipsis = true;
        sp.maxLines = 2;
        sp.rich = true;
        const std::string w1 = uiWriteLayout(d);
        UiLayoutDoc back;
        std::string err;
        check(uiParseLayout(w1, back, &err), "a layout with every text property parses: " + err);
        const i32 bn = uiLayoutFind(back, "Styled");
        const UiWidgetProps& bp = back.nodes[static_cast<usize>(bn)].props;
        check(bp.fontFamily == "Heading" && bp.bold && bp.italic && bp.textDir == UiTextDir::RTL && bp.textAlign == UiTextAlign::Start &&
                  approx(bp.lineSpacing, 1.25f) && approx(bp.letterSpacing, 2.5f) && bp.ellipsis && bp.maxLines == 2 && bp.rich,
              "every property survives .ocui");
        check(uiWriteLayout(back) == w1, "write(parse(write(x))) == write(x)");
        check(w1.find("SET Styled fontFamily Heading") != std::string::npos, "the writer emits what differs from the defaults");
        UiLayoutDoc plain = uiStarterLayout();
        check(uiWriteLayout(plain).find("fontFamily") == std::string::npos && uiWriteLayout(plain).find("lineSpacing") == std::string::npos,
              "and nothing for a layout that does not use them (no file changes for existing content)");

        UiLayoutDoc skip;
        check(uiParseLayout("OCUI 1\nWIDGET Root panel -\nSET Root futureThing 1 2 3\nSET Root textAlign end\n", skip, &err) &&
                  skip.nodes[0].props.textAlign == UiTextAlign::End,
              "a SET key from a newer build is skipped, the others still apply");
    }

    EngineEnv ee;
    AVER_INFO("=== engine path: measure and draw agree ===");
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Root");
        root.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid = root.id;
        UiWidget& ab = at(t, UiWidgetKind::Text, rid, "AB", 10, 10);
        ab.text = "AB";
        UiWidget& para = at(t, UiWidgetKind::Text, rid, "Para", 10, 100, 80);
        para.text = "AAA BBB CCC";
        para.wrap = true;
        UiWidget& btn = at(t, UiWidgetKind::Button, rid, "Btn", 10, 200);
        btn.text = "AB";
        UiWidget& choice = at(t, UiWidgetKind::Choice, rid, "Ch", 10, 300);
        choice.items = {"AB", "ABC"};
        Rec rec(&ee.eng);
        ee.eng.beginFrame();
        run(t, rec);
        check(approx(t.get(ab.id)->rect.w, 24.2f) && approx(t.get(ab.id)->rect.h, 20.0f), "Text AB measures 24.2 x 20 from the font's advances");
        UiVec2 size;
        i32 lines = 0;
        check(t.lastTextSize(para.id, size, lines) && lines == 2 && approx(size.x, 78.6f) && approx(size.y, 40.0f), "a wrapped paragraph reports two lines, 78.6 x 40");
        check(approx(t.get(para.id)->rect.h, 40.0f), "and is two lines tall");
        check(approx(t.get(btn.id)->rect.w, 24.2f + 28.0f) && approx(t.get(btn.id)->rect.h, 20.0f + 16.0f), "a button is the measured text plus its padding");
        check(approx(t.get(choice.id)->rect.w, 36.6f + 56.0f), "a choice is as wide as its widest item plus padding");
        const Rec::Text* first = rec.find("AB");
        check(first && approx(first->x, 10.0f) && approx(first->y, 10.0f + 16.0f), "drawn at the widget's left edge with the font's ascent (16 at 20 px)");
        check(rec.find("AAA BBB ") && rec.find("CCC"), "the wrapped paragraph's lines come from the same layout the measure used");
    }

    AVER_INFO("=== engine path: alignment, direction, styling ===");
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Root");
        root.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid = root.id;
        UiWidget& st = at(t, UiWidgetKind::Text, rid, "He", 0, 0, 200, 30);
        st.text = "\xD7\x90\xD7\x91";     // alef bet: right to left
        st.textAlign = UiTextAlign::Start;
        UiWidget& en = at(t, UiWidgetKind::Text, rid, "He2", 0, 40, 200, 30);
        en.text = "\xD7\x90\xD7\x91";
        en.textAlign = UiTextAlign::End;
        UiWidget& lt = at(t, UiWidgetKind::Text, rid, "Lt", 0, 80, 200, 30);
        lt.text = "AB";
        lt.textAlign = UiTextAlign::Start;
        UiWidget& forced = at(t, UiWidgetKind::Text, rid, "Forced", 0, 120, 200, 30);
        forced.text = "AB";
        forced.textAlign = UiTextAlign::Start;
        forced.textDir = UiTextDir::RTL;
        Rec rec(&ee.eng);
        ee.eng.beginFrame();
        run(t, rec);
        const f32 heW = (400 + 420) * 20.0f / 1000.0f;
        check(approx(rec.texts[0].x, 200.0f - heW), "Start in right-to-left text is the right edge");
        check(approx(rec.texts[1].x, 0.0f), "End in right-to-left text is the left edge");
        check(approx(rec.texts[2].x, 0.0f), "Start in left-to-right text is the left edge");
        check(approx(rec.texts[3].x, 200.0f - 24.2f), "textDir rtl forces Start to the right for Latin text too");

        // family / bold from the widget and from the theme
        UiTree t2;
        UiWidget& root2 = box(t2, 0, "Root");
        root2.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid2 = root2.id;
        UiWidget& plain = at(t2, UiWidgetKind::Text, rid2, "Plain", 0, 0);
        plain.text = "AV";
        UiWidget& fam = at(t2, UiWidgetKind::Text, rid2, "Fam", 0, 40);
        fam.text = "AV";
        fam.fontFamily = "Heading";
        UiWidget& bold = at(t2, UiWidgetKind::Text, rid2, "Bold", 0, 80);
        bold.text = "A";
        bold.bold = true;
        UiWidget& spaced = at(t2, UiWidgetKind::Text, rid2, "Spaced", 0, 120);
        spaced.text = "AB";
        spaced.letterSpacing = 2.0f;
        UiWidget& themed = at(t2, UiWidgetKind::Text, rid2, "Themed", 0, 160);
        themed.text = "AV";
        themed.style = "serif";
        UiTheme th = uiBuiltinTheme("dark");
        UiStyle serif;
        serif.fontFamily = "Heading";
        serif.bold = 0;
        th.named.emplace_back("serif", serif);
        t2.setTheme(th);
        Rec rec2(&ee.eng);
        ee.eng.beginFrame();
        run(t2, rec2);
        check(approx(t2.get(plain.id)->rect.w, 12.0f + 12.4f), "AV in the default family: no kerning");
        check(approx(t2.get(fam.id)->rect.w, 12.0f + 12.4f - 1.6f), "fontFamily on the widget selects the kerning family");
        check(approx(t2.get(bold.id)->rect.w, 14.0f), "bold selects the bold face (A is 700 units there)");
        check(approx(t2.get(spaced.id)->rect.w, 24.2f + 4.0f), "letterSpacing is design pixels times the scale");
        check(approx(t2.get(themed.id)->rect.w, 12.0f + 12.4f - 1.6f), "a named theme style can carry the family");

        UiStyle merged = th.resolve(UiWidgetKind::Text, "serif");
        check(merged.fontFamily == "Heading" && merged.bold == 0 && th.resolve(UiWidgetKind::Text, "").bold == -1 && th.resolve(UiWidgetKind::Text, "").fontFamily.empty(),
              "UiStyle merges family and bold, and defaults to inherit");
    }

    AVER_INFO("=== engine path: ellipsis, rich text, scale ===");
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Root");
        root.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid = root.id;
        UiWidget& cut = at(t, UiWidgetKind::Text, rid, "Cut", 0, 0, 100);
        cut.text = "AAAA BBBB CCCC DDDD";
        cut.wrap = true;
        cut.maxLines = 2;
        UiWidget& rich = at(t, UiWidgetKind::Text, rid, "Rich", 0, 200);
        rich.text = "A<b>A</b>";
        rich.rich = true;
        UiWidget& literal = at(t, UiWidgetKind::Text, rid, "Lit", 0, 240);
        literal.text = "A<b>A</b>";
        Rec rec(&ee.eng);
        ee.eng.beginFrame();
        run(t, rec);
        UiVec2 size;
        i32 lines = 0;
        check(t.lastTextSize(cut.id, size, lines) && lines == 2, "maxLines 2 limits a wrapped paragraph to two lines");
        check(approx(t.get(rich.id)->rect.w, 12.0f + 14.0f), "rich: the bold A is wider and the tags take no room");
        check(t.get(literal.id)->rect.w > 12.0f + 14.0f, "without rich the same string shows its tags");

        // the UI scale reaches the text size
        t.setDpi(UiDpi{UiScaleMode::Constant, 1920, 1080, 2.0f, 1.0f});
        run(t, rec);
        check(approx(t.scale(), 2.0f) && approx(t.get(rich.id)->rect.w, 2.0f * (12.0f + 14.0f), 0.2f), "at scale 2 the text is laid out at 40 px");
    }

    AVER_INFO("=== engine path: the text input ===");
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Root");
        root.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid = root.id;
        UiWidget& in = at(t, UiWidgetKind::TextInput, rid, "In", 0, 100, 300, 40);
        in.text = "ABC";
        const UiWidgetId id = in.id;
        Rec rec(&ee.eng);
        ee.eng.beginFrame();
        run(t, rec);
        run(t, rec, frame(10.0f + 23.0f, 120, 1));
        check(t.get(id)->caret == 2, "a click 23 px in lands on the stop at 24.2: after two letters");
        run(t, rec, frame(10.0f + 23.0f, 120, 0));
        t.get(id)->caret = 2;
        run(t, rec);
        const Rec::Fill* caret = nullptr;
        for (const Rec::Fill& f : rec.fills) if (approx(f.r.w, 1.0f) && approx(f.r.h, 20.0f)) caret = &f;
        check(caret && approx(caret->r.x, 10.0f + 24.2f), "the caret is drawn at the stop's x");

        UiWidget& he = at(t, UiWidgetKind::TextInput, rid, "He", 0, 200, 300, 40);
        he.text = "\xD7\x90\xD7\x91\xD7\x92";   // alef bet gimel
        const UiWidgetId hid = he.id;
        run(t, rec);
        // left to right on screen: gimel (8.8), bet (8.4), alef (8.0); logical stops run right to left
        run(t, rec, frame(10.0f + 1.0f, 220, 1));
        check(t.get(hid)->caret == 3, "right-to-left text: a click at the left edge is the logical end");
        run(t, rec, frame(10.0f + 1.0f, 220, 0));
        run(t, rec, frame(10.0f + 25.0f, 220, 1));
        check(t.get(hid)->caret == 0, "and one at the right edge of the text the logical start");
        run(t, rec, frame(10.0f + 25.0f, 220, 0));
    }

    if (g_failures == 0) AVER_INFO("=== all text tree tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
