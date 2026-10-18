// UiCaptionView (docs/ACCESSIBILITY.md section 2.2): the passive caption overlay. Row recycling, bottom and top
// anchoring, background opacity and its high-contrast floor, direction glyphs, speaker lines, colours and fade, the
// size preset, never taking the pointer or focus, localized text that follows a culture change, and an empty list
// hiding the root. No GPU, no window.
#include "aver/core/Log.hpp"
#include "aver/ui/UiAccess.hpp"
#include "aver/ui/UiText.hpp"
#include "aver/ui/UiTree.hpp"

#include <cmath>
#include <map>
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

static bool approx(f32 a, f32 b, f32 eps = 1.5f) { return std::fabs(a - b) <= eps; }

static UiEstimatedMetrics g_metrics;

static void step(UiTree& t, const UiInputFrame& in = UiInputFrame{}) { t.update(0.016f, in, g_metrics); }

static UiCaptionLine line(const char* speaker, const char* text, i32 dir = 0, f32 alpha = 1.0f, u32 rgba = uiRgba(255, 255, 255)) {
    UiCaptionLine l;
    l.speaker = speaker;
    l.text = text;
    l.dir = dir;
    l.alpha = alpha;
    l.rgba = rgba;
    return l;
}

static UiWidgetId child(UiTree& t, UiWidgetId parent, usize i) {
    const UiWidget* w = t.get(parent);
    return w && i < w->children.size() ? w->children[i] : 0;
}

static std::string textOf(UiTree& t, UiWidgetId id) {
    const UiWidget* w = t.get(id);
    return w ? w->text : std::string("(none)");
}

// Row panels of the caption root, in order.
static std::vector<UiWidgetId> rowsOf(UiTree& t) {
    std::vector<UiWidgetId> out;
    const UiWidget* r = t.get(t.find(UiCaptionView::kRootName));
    if (r) out = r->children;
    return out;
}

static void testStructure() {
    AVER_INFO("=== structure, recycling, visibility ===");
    UiTree t;
    t.setViewport(UiRect{0, 0, 1920, 1080});
    UiCaptionView v;
    UiCaptionStyle style;
    check(v.root() == 0, "no root before the first line");

    v.update(t, {}, style);
    check(v.root() == 0 && t.find(UiCaptionView::kRootName) == 0, "an empty list creates nothing");

    std::vector<UiCaptionLine> two = {line("Guard", "Halt!"), line("", "Stop right there.")};
    v.update(t, two, style);
    step(t);
    const UiWidgetId root = t.find(UiCaptionView::kRootName);
    check(root != 0 && root == v.root(), "two lines create the root named Aver.Captions");
    const UiWidget* r = t.get(root);
    check(r && r->inputMode == UiInputMode::Passive && r->layer == UiLayer::Overlay && r->visible, "a passive Overlay root, visible");
    check(v.rowCount() == 2 && rowsOf(t).size() == 2, "one row per line");

    const std::vector<UiWidgetId> first = rowsOf(t);
    v.update(t, two, style);
    check(rowsOf(t) == first, "an unchanged set of lines touches the same widgets");

    v.update(t, {line("", "just one")}, style);
    step(t);
    check(v.rowCount() == 2 && !t.get(first[1])->visible && t.get(first[0])->visible, "fewer lines hide the spare rows instead of destroying them");
    v.update(t, {line("", "a"), line("", "b"), line("", "c")}, style);
    check(v.rowCount() == 3 && rowsOf(t).size() == 3 && rowsOf(t)[0] == first[0] && rowsOf(t)[1] == first[1], "more lines add rows and keep the old ones");

    v.update(t, {}, style);
    check(!t.get(root)->visible, "an empty list hides the root");
    v.update(t, two, style);
    check(t.get(root)->visible, "and the next line shows it again");

    // The tree being cleared under the view is survivable.
    t.clear();
    v.update(t, two, style);
    check(t.find(UiCaptionView::kRootName) != 0, "after the tree was cleared the view makes a new root");
}

static void testAnchor() {
    AVER_INFO("=== bottom and top anchor ===");
    UiTree t;
    t.setViewport(UiRect{0, 0, 1920, 1080});
    UiCaptionView v;
    UiCaptionStyle bottom;
    v.update(t, {line("", "hello")}, bottom);
    step(t);
    UiRect r = t.get(v.root())->rect;
    check(r.w > 0 && r.h > 0 && approx(r.bottom(), 1080 * 0.94f), "bottom: the box ends 6 percent above the bottom edge");
    check(approx(r.x, 1920 * 0.15f, 2.0f) && approx(r.w, 1920 * 0.70f, 2.0f), "centred, 70 percent of the width");

    UiCaptionStyle top;
    top.top = true;
    v.update(t, {line("", "hello")}, top);
    step(t);
    r = t.get(v.root())->rect;
    check(approx(r.y, 1080 * 0.06f), "top: the box starts 6 percent below the top edge");
}

static void testLook() {
    AVER_INFO("=== background, high contrast, size, colour, fade ===");
    UiTree t;
    t.setViewport(UiRect{0, 0, 1920, 1080});
    UiCaptionView v;
    UiCaptionStyle style;
    style.bgOpacity = 0.6f;
    v.update(t, {line("", "hello", 0, 0.5f, uiRgba(125, 211, 252))}, style);
    step(t);
    std::vector<UiWidgetId> rows = rowsOf(t);
    const UiWidget* row = t.get(rows[0]);
    check(row && uiAlphaOf(row->bgColor) == 153 && (row->bgColor & 0xFFFFFF) == 0, "background is black at the opacity (0.6 -> alpha 153)");
    check(row && approx(row->opacity, 0.5f, 0.001f), "the line's alpha is the row opacity (the fade)");
    const UiWidget* body = t.get(child(t, rows[0], 1));
    check(body && body->wrap && body->textColor == uiRgba(125, 211, 252) && approx(body->fontSize, 24.0f, 0.001f), "the body wraps, takes the line colour and the 24 dp base size");

    style.bgOpacity = 0.0f;
    v.update(t, {line("", "hello")}, style);
    check(uiAlphaOf(t.get(rowsOf(t)[0])->bgColor) == 1, "opacity 0 is alpha 1, not 0 (0 would mean 'unset' and bring back the theme panel)");

    style.sizeScale = 1.9f;
    style.bgOpacity = 0.3f;
    style.highContrast = true;
    v.update(t, {line("Guard", "hello", 0, 1.0f, uiRgba(125, 211, 252))}, style);
    rows = rowsOf(t);
    row = t.get(rows[0]);
    body = t.get(child(t, rows[0], 1));
    check(row && uiAlphaOf(row->bgColor) == 217, "high contrast floors the background at 0.85");
    check(body && body->textColor == uiRgba(255, 255, 255), "and draws white text");
    check(body && approx(body->fontSize, 24.0f * 1.9f, 0.01f), "the size preset multiplies the base size");
}

static void testSpeakerAndDirection() {
    AVER_INFO("=== speaker and direction hints ===");
    UiTree t;
    t.setViewport(UiRect{0, 0, 1920, 1080});
    UiCaptionView v;
    UiCaptionStyle style;
    v.update(t, {line("Guard", "left", -1), line("", "right", 1), line("", "behind", 2), line("", "none", 0)}, style);
    step(t);
    const std::vector<UiWidgetId> rows = rowsOf(t);
    check(rows.size() == 4, "four rows");
    const auto head = [&](usize i) { return child(t, rows[i], 0); };
    const auto part = [&](usize i, usize k) { return child(t, head(i), k); };   // 0 pre, 1 speaker, 2 post
    check(t.get(head(0))->visible && textOf(t, part(0, 0)) == "<" && textOf(t, part(0, 1)) == "Guard" && textOf(t, part(0, 2)).empty(), "left: '<' before the speaker");
    check(t.get(head(1))->visible && textOf(t, part(1, 0)).empty() && textOf(t, part(1, 2)) == ">", "right: '>' after");
    check(t.get(head(2))->visible && textOf(t, part(2, 0)) == "v", "behind: 'v'");
    check(!t.get(head(3))->visible, "no speaker and no hint: the head line is hidden");
    check(textOf(t, child(t, rows[0], 1)) == "left" && textOf(t, child(t, rows[3], 1)) == "none", "the body carries the text");
}

static void testPassive() {
    AVER_INFO("=== passive ===");
    UiTree t;
    t.setViewport(UiRect{0, 0, 1920, 1080});
    UiCaptionView v;
    UiCaptionStyle style;
    v.update(t, {line("Guard", "hello"), line("", "world")}, style);
    step(t);
    const UiRect r = t.get(v.root())->rect;
    const UiVec2 c = r.center();
    check(t.hitTest(c.x, c.y) == 0, "the pointer over the captions hits nothing");
    UiInputFrame in;
    in.pointerX = c.x; in.pointerY = c.y;
    step(t, in);
    check(!t.inputState().pointerOverUi && !t.inputState().menuOpen && t.focused() == 0, "no pointer capture, no menu, no focus");
    bool anyFocusable = false;
    t.visit(v.root(), [&](UiWidget& w) { if (w.focusable || w.hit != UiHitMode::Never) anyFocusable = true; });
    check(!anyFocusable, "no widget of the overlay is focusable or hittable");
}

struct FakeLoc {
    std::map<std::string, std::string> en, de;
    bool german = false;
    u32 epoch = 1;
    bool resolve(std::string_view raw, std::string& out) const {
        UiTextRef r;
        if (!uiParseTextRef(raw, r)) return false;
        const auto& m = german ? de : en;
        const auto it = m.find(r.table + ":" + r.key);
        out = it != m.end() ? it->second : (r.hasDefault ? r.defaultText : "[" + r.table + ":" + r.key + "]");
        return true;
    }
};

static void testLocalized() {
    AVER_INFO("=== localized text ===");
    FakeLoc loc;
    loc.en["Cap:hello"] = "Hello";
    loc.de["Cap:hello"] = "Hallo";
    loc.en["Cap:spk.guard"] = "Guard";
    loc.de["Cap:spk.guard"] = "Wache";
    UiTree t;
    t.setViewport(UiRect{0, 0, 1920, 1080});
    t.setTextResolver([&loc](std::string_view raw, std::string& out) { return loc.resolve(raw, out); }, [&loc] { return loc.epoch; });
    UiCaptionView v;
    UiCaptionStyle style;
    v.update(t, {line("@Cap:spk.guard=Guard", "@Cap:hello=Hello")}, style);
    step(t);
    const UiWidgetId row = rowsOf(t)[0];
    const UiWidgetId body = child(t, row, 1), spk = child(t, child(t, row, 0), 1);
    check(textOf(t, body) == "@Cap:hello=Hello", "the widget holds the raw reference");
    check(t.shown(t.get(body)->text) == "Hello" && t.shown(t.get(spk)->text) == "Guard", "and the tree shows it localized");
    loc.german = true;
    ++loc.epoch;
    step(t);
    check(t.shown(t.get(body)->text) == "Hallo" && t.shown(t.get(spk)->text) == "Wache", "a culture switch re-reads it with no update to the view");
}

int main() {
    AVER_INFO("UiCaptionViewTest");
    testStructure();
    testAnchor();
    testLook();
    testSpeakerAndDirection();
    testPassive();
    testLocalized();
    AVER_INFO("UiCaptionViewTest done: {} failure(s)", g_failures);
    return g_failures ? 1 : 0;
}
