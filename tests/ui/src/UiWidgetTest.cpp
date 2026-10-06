// Headless test for the retained widget system: layout maths, DPI scaling, hit testing, focus
// order, input routing, the value widgets, and the ready-made screens. No GPU, no window.
#include "aver/core/Log.hpp"
#include "aver/ui/UiFocus.hpp"
#include "aver/ui/UiHostInput.hpp"
#include "aver/ui/UiLayout.hpp"
#include "aver/ui/UiScreens.hpp"
#include "aver/ui/UiTree.hpp"

#include <cmath>
#include <string>
#include <utility>
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

static UiEstimatedMetrics g_metrics;

static UiWidget& mk(UiTree& t, UiWidgetKind k, UiWidgetId parent, const char* name) {
    return *t.get(t.create(k, parent, name));
}

// A container with no look, so it never takes the pointer.
static UiWidget& box(UiTree& t, UiWidgetId parent, const char* name, UiLayoutMode layout) {
    UiWidget& w = mk(t, UiWidgetKind::Panel, parent, name);
    w.style = "clear";
    w.layout = layout;
    return w;
}

static UiWidget& fixed(UiTree& t, UiWidgetKind k, UiWidgetId parent, const char* name, f32 x, f32 y, f32 w, f32 h) {
    UiWidget& c = mk(t, k, parent, name);
    c.offsets.left = x;
    c.offsets.top = y;
    c.width = w;
    c.height = h;
    return c;
}

static std::vector<UiEvent> drain(UiTree& t) {
    std::vector<UiEvent> out;
    UiEvent e;
    while (t.pollEvent(e)) out.push_back(e);
    return out;
}

static const UiEvent* firstOf(const std::vector<UiEvent>& v, UiEventType type) {
    for (const UiEvent& e : v) if (e.type == type) return &e;
    return nullptr;
}

static const UiEvent* command(const std::vector<UiEvent>& v, const std::string& text) {
    for (const UiEvent& e : v) if (e.type == UiEventType::Command && e.text == text) return &e;
    return nullptr;
}

static UiInputFrame at(f32 x, f32 y, u32 buttons = 0) {
    UiInputFrame f;
    f.pointerX = x;
    f.pointerY = y;
    f.buttons = buttons;
    return f;
}

static UiInputFrame navFrame(UiNav n) {
    UiInputFrame f;
    f.pointerValid = false;
    f.nav = uiNavBit(n);
    return f;
}

static void step(UiTree& t, const UiInputFrame& in) { t.update(0.016f, in, g_metrics); }

int main() {
    AVER_INFO("=== DPI scaling ===");
    {
        UiDpi d;   // height mode against 1920x1080
        check(approx(uiComputeScale(d, 1920, 1080), 1.0f), "the reference resolution is scale 1");
        check(approx(uiComputeScale(d, 3840, 2160), 2.0f), "4K is scale 2");
        d.userScale = 1.5f;
        check(approx(uiComputeScale(d, 1920, 1080), 1.5f), "the user's UI scale multiplies");
        d.userScale = 1.0f;
        d.mode = UiScaleMode::Width;
        check(approx(uiComputeScale(d, 960, 1080), 0.5f), "width mode follows the width");
        d.mode = UiScaleMode::ShortestSide;
        check(approx(uiComputeScale(d, 3840, 1080), 1.0f), "shortest side ignores an ultrawide");
        d.mode = UiScaleMode::Blend;
        check(approx(uiComputeScale(d, 3840, 1080), std::sqrt(2.0f)), "blend is the geometric mean of the ratios");
        d.mode = UiScaleMode::Constant;
        d.platformScale = 1.25f;
        check(approx(uiComputeScale(d, 100, 100), 1.25f), "constant uses the platform scale only");
        d.mode = UiScaleMode::Height;
        check(approx(uiComputeScale(d, 100, 100), 0.25f), "the scale is clamped at the low end");
        check(approx(uiComputeScale(d, 100000, 100000), 8.0f), "and at the high end");
    }

    AVER_INFO("=== anchors ===");
    {
        const UiRect parent{100, 50, 800, 600};
        UiRect r = uiResolveAnchored(parent, UiAnchors{0, 0, 1, 1}, UiInsets{10, 20, 30, 40}, {0, 0}, 0, 0, 1.0f);
        check(approx(r.x, 110) && approx(r.y, 70) && approx(r.w, 760) && approx(r.h, 540), "stretched anchors inset by the offsets");
        r = uiResolveAnchored(parent, UiAnchors{0.5f, 0.5f, 0.5f, 0.5f}, UiInsets{}, {0.5f, 0.5f}, 200, 100, 1.0f);
        check(approx(r.x, 400) && approx(r.y, 300) && approx(r.w, 200) && approx(r.h, 100), "a centred point anchor with a centre pivot");
        r = uiResolveAnchored(parent, UiAnchors{1, 1, 1, 1}, UiInsets{-10, -10, 0, 0}, {1, 1}, 50, 50, 2.0f);
        check(approx(r.x, 830) && approx(r.y, 580), "bottom-right anchor, scaled offset, bottom-right pivot");
        r = uiResolveAnchored(parent, UiAnchors{0, 0, 1, 0}, UiInsets{5, 10, 5, 0}, {0, 0}, 0, 40, 1.0f);
        check(approx(r.w, 790) && approx(r.y, 60) && approx(r.h, 40), "stretch on one axis, point on the other");
    }

    AVER_INFO("=== axis solving ===");
    {
        std::vector<UiAxisItem> items(3);
        items[0].desired = 50; items[1].desired = 30; items[2].desired = 20;
        std::vector<UiSpan> s = uiSolveAxis(items, 300, 10, UiJustify::Start);
        check(approx(s[0].pos, 0) && approx(s[1].pos, 60) && approx(s[2].pos, 100), "fixed items stack with spacing");
        check(approx(uiAxisContent(items, 10), 120), "content is sizes plus spacing");

        items[0].fill = 1; items[2].fill = 3;
        s = uiSolveAxis(items, 330, 0, UiJustify::Start);   // 330 - 30 fixed = 300, shared 1:3
        check(approx(s[0].size, 75) && approx(s[2].size, 225) && approx(s[1].size, 30), "fill items share the remainder by weight");
        check(approx(s[1].pos, 75) && approx(s[2].pos, 105), "and the positions follow");

        items[0].maxSize = 50;
        s = uiSolveAxis(items, 330, 0, UiJustify::Start);
        check(approx(s[0].size, 50) && approx(s[2].size, 250), "a max clamp frees the rest for the others");

        items[0].maxSize = 0;
        items[2].minSize = 280;
        s = uiSolveAxis(items, 330, 0, UiJustify::Start);
        check(approx(s[2].size, 280) && approx(s[0].size, 20), "a min clamp takes its share first");

        std::vector<UiAxisItem> two(2);
        two[0].desired = 40; two[1].desired = 40;
        s = uiSolveAxis(two, 200, 0, UiJustify::Center);
        check(approx(s[0].pos, 60) && approx(s[1].pos, 100), "centre justify splits the leftover");
        s = uiSolveAxis(two, 200, 0, UiJustify::End);
        check(approx(s[0].pos, 120), "end justify pushes to the end");
        s = uiSolveAxis(two, 200, 0, UiJustify::SpaceBetween);
        check(approx(s[0].pos, 0) && approx(s[1].pos, 160), "space-between puts one at each end");

        two[0].before = 5; two[0].after = 7; two[1].before = 3;
        s = uiSolveAxis(two, 200, 10, UiJustify::Start);
        check(approx(s[0].pos, 5) && approx(s[1].pos, 5 + 40 + 7 + 10 + 3), "margins sit outside the slot");

        check(uiSolveAxis({}, 100, 0, UiJustify::Start).empty(), "no items, no spans");

        UiSpan a = uiAlignInSlot(10, 100, 40, UiAlign::Center, 0, 0);
        check(approx(a.pos, 40) && approx(a.size, 40), "centre in a slot");
        a = uiAlignInSlot(10, 100, 40, UiAlign::End, 0, 0);
        check(approx(a.pos, 70), "end in a slot");
        a = uiAlignInSlot(10, 100, 40, UiAlign::Stretch, 0, 60);
        check(approx(a.size, 60), "stretch honours the max");

        check(approx(uiClampScroll(500, 300, 100), 200) && approx(uiClampScroll(-5, 300, 100), 0), "scroll clamps to the content");
        check(approx(uiClampScroll(50, 80, 100), 0), "content shorter than the viewport never scrolls");
        check(approx(uiScrollIntoView(0, 100, 150, 20), 70), "scroll down just enough");
        check(approx(uiScrollIntoView(100, 100, 40, 20), 40), "scroll up just enough");
        check(approx(uiScrollIntoView(10, 100, 40, 20), 10), "already visible stays put");
    }

    AVER_INFO("=== text wrapping ===");
    {
        UiEstimatedMetrics m;
        std::vector<std::string> l = uiWrapText(m, "aaa bbb ccc", 10.0f, 20.0f);
        check(l.size() == 3 && l[0] == "aaa" && l[1] == "bbb" && l[2] == "ccc", "words wrap onto lines");
        l = uiWrapText(m, "aaa bbb", 10.0f, 100.0f);
        check(l.size() == 1 && l[0] == "aaa bbb", "text that fits stays on one line");
        l = uiWrapText(m, "abcdefghij", 10.0f, 23.0f);
        check(l.size() == 3 && l[0] == "abcd" && l[1] == "efgh" && l[2] == "ij", "a long word breaks between characters");
        l = uiWrapText(m, "one\ntwo", 10.0f, 0.0f);
        check(l.size() == 2, "newlines always break");
        check(uiUtf8Length("a\xC3\xA9z") == 3 && uiUtf8Offset("a\xC3\xA9z", 2) == 3, "utf-8 length and offsets count code points");
    }

    AVER_INFO("=== stack layout ===");
    {
        UiTree t;
        UiWidget& root = mk(t, UiWidgetKind::Panel, 0, "Root");
        root.layout = UiLayoutMode::VStack;
        root.spacing = 10;
        const UiWidgetId rid = root.id;
        UiWidget& a = mk(t, UiWidgetKind::Button, rid, "A"); a.text = "A";
        UiWidget& b = mk(t, UiWidgetKind::Button, rid, "B"); b.text = "B";
        UiWidget& c = mk(t, UiWidgetKind::Button, rid, "C"); c.text = "C"; c.height = 60;
        t.layout(g_metrics);
        check(approx(t.scale(), 1.0f), "1920x1080 is scale 1");
        check(approx(root.rect.w, 1920) && approx(root.rect.h, 1080), "a root stretches over the viewport");
        check(approx(a.rect.x, 0) && approx(a.rect.w, 1920), "children stretch across a vertical stack");
        check(approx(a.rect.h, 38.5f), "a button is its line plus padding tall");
        check(approx(b.rect.y, a.rect.bottom() + 10), "children follow with the spacing");
        check(approx(c.rect.y, b.rect.bottom() + 10) && approx(c.rect.h, 60), "a fixed height is honoured");

        t.setViewport({0, 0, 3840, 2160});
        t.layout(g_metrics);
        check(approx(t.scale(), 2.0f), "a bigger viewport doubles the scale");
        check(approx(b.rect.y, a.rect.bottom() + 20) && approx(c.rect.h, 120), "and every size and spacing with it");
    }
    {
        UiTree t;
        UiWidget& root = mk(t, UiWidgetKind::Panel, 0, "Root");
        root.layout = UiLayoutMode::HStack;
        const UiWidgetId rid = root.id;
        UiWidget& a = box(t, rid, "A", UiLayoutMode::None); a.fillW = 1; a.height = 50;
        UiWidget& b = box(t, rid, "B", UiLayoutMode::None); b.fillW = 3; b.height = 50;
        t.layout(g_metrics);
        check(approx(a.rect.w, 480) && approx(b.rect.w, 1440) && approx(b.rect.x, 480), "horizontal fill weights share the width");
        check(approx(a.rect.h, 50), "a fixed cross size beats stretch");
    }
    {
        UiTree t;
        UiWidget& root = mk(t, UiWidgetKind::Panel, 0, "Root");
        root.layout = UiLayoutMode::Grid;
        root.columns = 2;
        root.cellWidth = 100;
        root.cellHeight = 40;
        root.spacing = 10;
        const UiWidgetId rid = root.id;
        UiWidget* cells[4];
        for (int i = 0; i < 4; ++i) cells[i] = &box(t, rid, ("C" + std::to_string(i)).c_str(), UiLayoutMode::None);
        t.layout(g_metrics);
        check(approx(cells[0]->rect.x, 0) && approx(cells[1]->rect.x, 110) && approx(cells[2]->rect.x, 0), "grid columns");
        check(approx(cells[0]->rect.y, 0) && approx(cells[2]->rect.y, 50) && approx(cells[3]->rect.y, 50), "grid rows");
        check(approx(cells[3]->rect.w, 100) && approx(cells[3]->rect.h, 40), "grid cells are the fixed size");
    }
    {
        UiTree t;
        UiWidget& root = mk(t, UiWidgetKind::Panel, 0, "Root");
        root.layout = UiLayoutMode::None;
        const UiWidgetId rid = root.id;
        UiWidget& c = fixed(t, UiWidgetKind::Panel, rid, "Centre", 10, 20, 200, 100);
        c.anchors = UiAnchors{0.5f, 0.5f, 0.5f, 0.5f};
        c.pivot = {0.5f, 0.5f};
        t.layout(g_metrics);
        check(approx(c.rect.x, 960 - 100 + 10) && approx(c.rect.y, 540 - 50 + 20), "an anchored child centres with its offset");
        t.setViewport({0, 0, 3840, 2160});
        t.layout(g_metrics);
        check(approx(c.rect.x, 1920 - 200 + 20) && approx(c.rect.w, 400), "anchors and sizes scale with the DPI");
    }
    {
        UiTree t;
        UiWidget& root = mk(t, UiWidgetKind::Panel, 0, "Root");
        root.layout = UiLayoutMode::VStack;
        root.padding = UiInsets{10, 20, 30, 40};
        const UiWidgetId rid = root.id;
        UiWidget& a = box(t, rid, "A", UiLayoutMode::None);
        a.height = 100;
        t.layout(g_metrics);
        check(approx(a.rect.x, 10) && approx(a.rect.y, 20) && approx(a.rect.w, 1920 - 40), "padding insets the content area");
    }

    AVER_INFO("=== hit testing ===");
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Root", UiLayoutMode::None);
        root.anchors = UiAnchors{0, 0, 1, 1};
        root.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid = root.id;
        const UiWidgetId a = fixed(t, UiWidgetKind::Button, rid, "A", 0, 0, 100, 100).id;
        const UiWidgetId b = fixed(t, UiWidgetKind::Button, rid, "B", 50, 50, 100, 100).id;
        t.layout(g_metrics);
        check(t.hitTest(25, 25) == a, "a point in one button hits it");
        check(t.hitTest(75, 75) == b, "overlap goes to the later sibling");
        check(t.hitTest(300, 300) == 0, "empty space hits nothing, even over a container");
        t.get(b)->visible = false;
        check(t.hitTest(75, 75) == a, "a hidden widget is not hit");
        t.get(b)->visible = true;
        t.get(b)->hit = UiHitMode::Never;
        check(t.hitTest(120, 120) == 0, "hit = never lets the pointer through");
        t.get(rid)->inputMode = UiInputMode::Passive;
        check(t.hitTest(25, 25) == 0, "a passive root is never hit");
    }
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Root", UiLayoutMode::None);
        root.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid = root.id;
        UiWidget& sc = fixed(t, UiWidgetKind::Scroll, rid, "Scroll", 0, 0, 200, 100);
        sc.layout = UiLayoutMode::VStack;
        const UiWidgetId sid = sc.id;
        std::vector<UiWidgetId> btns;
        for (int i = 0; i < 6; ++i) {
            UiWidget& b = mk(t, UiWidgetKind::Button, sid, ("B" + std::to_string(i)).c_str());
            b.height = 40;
            btns.push_back(b.id);
        }
        t.layout(g_metrics);
        check(t.hitTest(10, 10) == btns[0], "the first row is hit");
        check(t.hitTest(10, 150) == 0, "a row below the scroll viewport is clipped out of hit testing");
        check(t.get(btns[5])->rect.y > 100, "though its rectangle exists below");
    }
    {
        UiTree t;
        UiWidget& low = box(t, 0, "Low", UiLayoutMode::None);
        low.inputMode = UiInputMode::Blocking;
        const UiWidgetId lowId = low.id;
        const UiWidgetId lowBtn = fixed(t, UiWidgetKind::Button, lowId, "LowBtn", 0, 0, 100, 100).id;
        UiWidget& top = box(t, 0, "Top", UiLayoutMode::None);
        top.inputMode = UiInputMode::Menu;
        top.modal = true;
        const UiWidgetId topId = top.id;
        const UiWidgetId topBtn = fixed(t, UiWidgetKind::Button, topId, "TopBtn", 500, 500, 100, 100).id;
        t.layout(g_metrics);
        check(t.hitTest(10, 10) == 0, "a modal root blocks the root beneath it");
        check(t.hitTest(510, 510) == topBtn, "while its own widgets are hit");
        t.get(topId)->visible = false;
        check(t.hitTest(10, 10) == lowBtn, "closing the modal frees the root beneath");
    }

    AVER_INFO("=== focus order ===");
    {
        std::vector<UiFocusItem> items;
        items.push_back({1, {0, 0, 10, 10}, 0});
        items.push_back({2, {20, 0, 10, 10}, 2});
        items.push_back({3, {40, 0, 10, 10}, 1});
        items.push_back({4, {60, 0, 10, 10}, 0});
        items.push_back({5, {80, 0, 10, 10}, -1});
        const std::vector<UiWidgetId> order = uiTabOrder(items);
        check(order.size() == 4 && order[0] == 3 && order[1] == 2 && order[2] == 1 && order[3] == 4,
              "positive tab indices first, ascending, then document order; negatives skipped");
        check(uiNextTab(items, 3, false) == 2 && uiNextTab(items, 4, false) == 3, "Tab steps forward and wraps");
        check(uiNextTab(items, 3, true) == 4 && uiNextTab(items, 1, true) == 2, "Shift+Tab steps back and wraps");
        check(uiNextTab(items, 4, false, false) == 4, "without wrap it stays on the end");
        check(uiNextTab(items, 99, false) == 3 && uiNextTab(items, 99, true) == 4, "an unknown start enters at either end");
        check(uiNextTab({}, 1, false) == 0, "nothing focusable gives 0");
    }
    {
        // A 2x2 grid of buttons with one tall button to the right.
        std::vector<UiFocusItem> g;
        g.push_back({1, {0, 0, 100, 40}, 0});
        g.push_back({2, {120, 0, 100, 40}, 0});
        g.push_back({3, {0, 60, 100, 40}, 0});
        g.push_back({4, {120, 60, 100, 40}, 0});
        check(uiNavigate(g, 1, UiNavDir::Right) == 2 && uiNavigate(g, 1, UiNavDir::Down) == 3, "right and down from the corner");
        check(uiNavigate(g, 4, UiNavDir::Left) == 3 && uiNavigate(g, 4, UiNavDir::Up) == 2, "left and up from the far corner");
        check(uiNavigate(g, 2, UiNavDir::Right, false) == 0, "nothing beyond the edge without wrap");
        check(uiNavigate(g, 2, UiNavDir::Right, true) == 1, "wraps to the far side, on the same row");
        check(uiNavigate(g, 3, UiNavDir::Down, true) == 1, "vertical wrap keeps the column");
        check(uiNavigate(g, 99, UiNavDir::Down) == 1, "an unknown start enters at the first");
    }

    AVER_INFO("=== pointer: click, toggle, choice, slider ===");
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Root", UiLayoutMode::None);
        root.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid = root.id;
        UiWidget& btn = fixed(t, UiWidgetKind::Button, rid, "Go", 100, 100, 200, 50);
        btn.text = "Go";
        btn.command = "go";
        const UiWidgetId btnId = btn.id;
        step(t, at(150, 120));
        check(t.get(btnId)->hovered, "hovering marks the widget");
        check(t.inputState().pointerOverUi && t.inputState().wantsPointer(), "the host is told the pointer is on UI");
        check(!t.inputState().pausesGame(), "a blocking HUD does not pause the game");
        step(t, at(150, 120, 1));
        check(t.get(btnId)->pressed && t.focused() == btnId, "pressing presses and focuses");
        check(firstOf(drain(t), UiEventType::Clicked) == nullptr, "no click until release");
        step(t, at(150, 120, 0));
        const std::vector<UiEvent> ev = drain(t);
        check(firstOf(ev, UiEventType::Clicked) && firstOf(ev, UiEventType::Clicked)->widget == btnId, "release over it clicks");
        check(command(ev, "go") != nullptr, "and raises the button's command");

        step(t, at(150, 120, 1));
        step(t, at(900, 900, 0));
        check(firstOf(drain(t), UiEventType::Clicked) == nullptr, "releasing elsewhere cancels the click");
        step(t, at(900, 900));
        check(!t.inputState().pointerOverUi, "the pointer is off the UI again");
    }
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Root", UiLayoutMode::None);
        root.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid = root.id;
        const UiWidgetId tg = fixed(t, UiWidgetKind::Toggle, rid, "T", 0, 0, 200, 40).id;
        UiWidget& ch = fixed(t, UiWidgetKind::Choice, rid, "C", 0, 100, 200, 40);
        ch.items = {"Low", "High", "Ultra"};
        const UiWidgetId chId = ch.id;
        step(t, at(20, 20, 1));
        step(t, at(20, 20, 0));
        std::vector<UiEvent> ev = drain(t);
        check(t.get(tg)->checked && firstOf(ev, UiEventType::Toggled) && approx(firstOf(ev, UiEventType::Toggled)->value, 1), "a click toggles");
        step(t, at(20, 20, 1));
        step(t, at(20, 20, 0));
        check(!t.get(tg)->checked, "and again");
        drain(t);
        step(t, at(180, 120, 1));
        step(t, at(180, 120, 0));
        ev = drain(t);
        check(t.get(chId)->selected == 1 && firstOf(ev, UiEventType::ValueChanged), "the right half of a choice goes forward");
        step(t, at(10, 120, 1));
        step(t, at(10, 120, 0));
        check(t.get(chId)->selected == 0, "the left half goes back");
        step(t, at(10, 120, 1));
        step(t, at(10, 120, 0));
        check(t.get(chId)->selected == 2, "and wraps");
    }
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Root", UiLayoutMode::None);
        root.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid = root.id;
        const UiWidgetId sl = fixed(t, UiWidgetKind::Slider, rid, "S", 100, 100, 200, 30).id;
        step(t, at(200, 110, 1));
        check(approx(t.get(sl)->value, 0.5f, 0.02f), "pressing a slider's middle sets it to half");
        step(t, at(300, 110, 1));
        check(approx(t.get(sl)->value, 1.0f), "dragging past the end clamps to the max");
        step(t, at(0, 500, 1));
        check(approx(t.get(sl)->value, 0.0f), "and keeps tracking when the pointer leaves");
        step(t, at(0, 500, 0));
        const std::vector<UiEvent> ev = drain(t);
        check(firstOf(ev, UiEventType::ValueChanged) != nullptr, "value changes raise events");
        t.get(sl)->step = 0.25f;
        step(t, at(200, 110, 1));
        step(t, at(200, 110, 0));
        check(approx(t.get(sl)->value, 0.5f), "a step snaps the value");
        t.get(sl)->value = 0.0f;
        step(t, at(215, 110, 1));
        step(t, at(215, 110, 0));
        check(approx(std::fmod(t.get(sl)->value, 0.25f), 0.0f, 0.001f), "every value lands on a step");
    }

    AVER_INFO("=== text input and lists ===");
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Root", UiLayoutMode::None);
        root.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid = root.id;
        UiWidget& in = fixed(t, UiWidgetKind::TextInput, rid, "Name", 0, 0, 300, 40);
        in.maxLength = 3;
        in.command = "submitted";
        const UiWidgetId id = in.id;
        step(t, at(10, 10, 1));
        step(t, at(10, 10, 0));
        check(t.focused() == id && t.inputState().textEditing && t.inputState().wantsKeyboard(), "clicking a text input focuses it and claims the keyboard");
        UiInputFrame f = at(10, 10);
        f.chars = {'a', 'b', 'c', 'd'};
        step(t, f);
        check(t.get(id)->text == "abc", "typing appends, up to the max length");
        f = at(10, 10);
        f.editKeys = {UiEditKey::Backspace};
        step(t, f);
        check(t.get(id)->text == "ab" && t.get(id)->caret == 2, "backspace removes the previous character");
        f = at(10, 10);
        f.editKeys = {UiEditKey::Left, UiEditKey::Left, UiEditKey::Delete};
        step(t, f);
        check(t.get(id)->text == "b" && t.get(id)->caret == 0, "arrows move the caret and delete removes ahead");
        f = at(10, 10);
        f.chars = {'x'};
        f.editKeys = {UiEditKey::End};
        step(t, f);
        f = at(10, 10);
        f.chars = {'z'};
        step(t, f);
        check(t.get(id)->text == "xbz", "chars insert at the caret");
        f = at(10, 10);
        f.editKeys = {UiEditKey::Enter};
        step(t, f);
        const std::vector<UiEvent> ev = drain(t);
        check(firstOf(ev, UiEventType::TextChanged) != nullptr, "edits raise TextChanged");
        check(firstOf(ev, UiEventType::TextSubmitted) && firstOf(ev, UiEventType::TextSubmitted)->text == "xbz", "Enter submits");
        check(command(ev, "submitted") != nullptr, "and raises the command");
        step(t, at(700, 700, 1));
        step(t, at(700, 700, 0));
        check(t.focused() == 0 && !t.inputState().textEditing, "clicking away releases the keyboard");
    }
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Root", UiLayoutMode::None);
        root.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid = root.id;
        UiWidget& list = fixed(t, UiWidgetKind::List, rid, "L", 0, 0, 200, 100);
        list.items = {"a", "b", "c", "d", "e", "f", "g", "h"};
        const UiWidgetId id = list.id;
        step(t, at(20, 70, 1));   // row 2: (70 - 2) / 30
        check(t.get(id)->selected == 2, "pressing a row selects it");
        step(t, at(20, 70, 0));
        const std::vector<UiEvent> ev = drain(t);
        check(firstOf(ev, UiEventType::SelectionChanged) && firstOf(ev, UiEventType::SelectionChanged)->index == 2, "a selection event");
        check(firstOf(ev, UiEventType::Clicked) && firstOf(ev, UiEventType::Clicked)->text == "c", "releasing on it activates the row");
        UiInputFrame f = at(20, 70);
        f.wheel = -1.0f;
        step(t, f);
        check(approx(t.get(id)->scrollY, 90, 0.5f), "the wheel scrolls three rows");
        check(t.get(id)->contentSize.y > 200, "the list knows its content height");
    }
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Root", UiLayoutMode::None);
        root.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid = root.id;
        UiWidget& sc = fixed(t, UiWidgetKind::Scroll, rid, "S", 0, 0, 200, 100);
        const UiWidgetId sid = sc.id;
        std::vector<UiWidgetId> kids;
        for (int i = 0; i < 10; ++i) {
            UiWidget& b = mk(t, UiWidgetKind::Button, sid, ("B" + std::to_string(i)).c_str());
            b.height = 40;
            kids.push_back(b.id);
        }
        step(t, at(20, 20));
        const f32 y0 = t.get(kids[0])->rect.y;
        UiInputFrame f = at(20, 20);
        f.wheel = -1.0f;
        step(t, f);
        check(approx(t.get(sid)->scrollY, 60, 0.5f) && approx(t.get(kids[0])->rect.y, y0 - 60, 0.5f), "the wheel moves the content under the viewport");
        f.wheel = -100.0f;
        step(t, f);
        check(approx(t.get(sid)->scrollY, t.get(sid)->contentSize.y - 100, 0.5f), "scrolling stops at the end of the content");
        f.wheel = 100.0f;
        step(t, f);
        check(approx(t.get(sid)->scrollY, 0), "and at the start");
    }

    AVER_INFO("=== keyboard and gamepad navigation ===");
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Menu", UiLayoutMode::VStack);
        root.inputMode = UiInputMode::Menu;
        root.cancelCommand = "menu.back";
        root.spacing = 10;
        const UiWidgetId rid = root.id;
        std::vector<UiWidgetId> b;
        for (int i = 0; i < 3; ++i) {
            UiWidget& x = mk(t, UiWidgetKind::Button, rid, ("B" + std::to_string(i)).c_str());
            x.text = "B";
            x.height = 40;
            x.alignV = UiAlign::Start;
            x.command = "pick" + std::to_string(i);
            b.push_back(x.id);
        }
        step(t, at(0, 0));
        check(t.focused() == b[0], "opening a menu focuses its first control");
        check(t.inputState().menuOpen && t.inputState().pausesGame() && t.inputState().wantsKeyboard(), "an open menu pauses the game and claims the keyboard");
        step(t, navFrame(UiNav::Down));
        check(t.focused() == b[1], "Down moves to the next row");
        step(t, navFrame(UiNav::Down));
        step(t, navFrame(UiNav::Down));
        check(t.focused() == b[0], "and wraps past the last");
        step(t, navFrame(UiNav::Up));
        check(t.focused() == b[2], "Up wraps the other way");
        step(t, navFrame(UiNav::NextTab));
        check(t.focused() == b[0], "Tab follows the tab order");
        step(t, navFrame(UiNav::PrevTab));
        check(t.focused() == b[2], "Shift+Tab goes back");
        drain(t);
        step(t, navFrame(UiNav::Accept));
        std::vector<UiEvent> ev = drain(t);
        check(command(ev, "pick2") != nullptr && firstOf(ev, UiEventType::Clicked), "Accept activates the focused button");
        step(t, navFrame(UiNav::Cancel));
        ev = drain(t);
        check(firstOf(ev, UiEventType::Cancel) && command(ev, "menu.back"), "Cancel raises the menu's cancel command");
        t.get(b[1])->navDown = "B0";
        step(t, navFrame(UiNav::Up));   // from B2 to B1
        step(t, navFrame(UiNav::Down));
        check(t.focused() == b[0], "an explicit navDown name overrides spatial navigation");
        t.get(b[0])->enabled = false;
        step(t, at(0, 0));
        check(t.focused() != b[0], "a disabled widget loses the focus");
        t.get(rid)->visible = false;
        step(t, at(0, 0));
        check(!t.inputState().menuOpen && t.focused() == 0, "closing the menu returns the game its input");
    }
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Menu", UiLayoutMode::VStack);
        root.inputMode = UiInputMode::Menu;
        const UiWidgetId rid = root.id;
        UiWidget& sl = mk(t, UiWidgetKind::Slider, rid, "S");
        sl.height = 30;
        sl.alignV = UiAlign::Start;
        const UiWidgetId slId = sl.id;
        UiWidget& tg = mk(t, UiWidgetKind::Toggle, rid, "T");
        tg.height = 30;
        tg.alignV = UiAlign::Start;
        step(t, at(0, 0));
        check(t.focused() == slId, "the slider is focused first");
        step(t, navFrame(UiNav::Right));
        check(approx(t.get(slId)->value, 0.05f), "Right nudges a slider by a twentieth of its range");
        step(t, navFrame(UiNav::Left));
        step(t, navFrame(UiNav::Left));
        check(approx(t.get(slId)->value, 0.0f), "Left clamps at the minimum");
        check(t.focused() == slId, "left and right stay on the slider");
        step(t, navFrame(UiNav::Down));
        step(t, navFrame(UiNav::Accept));
        check(t.get(t.find("T"))->checked, "Accept flips a focused toggle");
    }
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Menu", UiLayoutMode::VStack);
        root.inputMode = UiInputMode::Menu;
        const UiWidgetId rid = root.id;
        UiWidget& kb = mk(t, UiWidgetKind::KeyBind, rid, "Jump");
        kb.height = 40;
        kb.alignV = UiAlign::Start;
        kb.text = "Space";
        const UiWidgetId id = kb.id;
        step(t, at(0, 0));
        check(t.focused() == id, "the key-bind is focused");
        UiInputFrame f = navFrame(UiNav::Accept);
        f.rawKey = 5;
        step(t, f);
        check(t.get(id)->listening, "Accept starts listening");
        check(firstOf(drain(t), UiEventType::KeyCaptured) == nullptr, "the key that started it is not captured");
        f = at(0, 0);
        f.pointerValid = false;
        f.nav = uiNavBit(UiNav::Down);
        step(t, f);
        check(t.get(id)->listening, "navigation does not leave a listening key-bind");
        f = at(0, 0);
        f.pointerValid = false;
        f.rawKey = 7;
        step(t, f);
        const std::vector<UiEvent> ev = drain(t);
        check(!t.get(id)->listening && firstOf(ev, UiEventType::KeyCaptured) && firstOf(ev, UiEventType::KeyCaptured)->index == 7,
              "the next key is captured and listening ends");
        step(t, navFrame(UiNav::Accept));
        step(t, navFrame(UiNav::Cancel));
        check(!t.get(id)->listening, "Cancel abandons a capture");
    }

    AVER_INFO("=== ready-made screens ===");
    {
        UiTree t;
        UiSettingsModel model = uiDefaultSettingsModel();
        int changes = 0;
        std::string lastKey;
        model.onChange = [&](const UiSettingItem& i) { ++changes; lastKey = i.key; };
        UiRebindModel rebind;
        rebind.rows.push_back({"Jump", 0, "Jump", "Space", true});
        rebind.rows.push_back({"Fire", 0, "Fire", "MouseLeft", true});
        UiSettingsScreen screen(t, model, &rebind);
        const UiWidgetId root = screen.build();
        check(root != 0 && t.get(root)->inputMode == UiInputMode::Menu && t.get(root)->modal, "the screen is a modal menu root");
        check(t.find("tab.0", root) && t.find("tab.1", root) && t.find("tab.2", root), "Graphics, Audio and Controls tabs");
        check(t.get(t.find("page.0", root))->visible && !t.get(t.find("page.1", root))->visible, "only the first page shows");
        screen.showTab(2);
        check(!t.get(t.find("page.0", root))->visible && t.get(t.find("page.2", root))->visible, "tabs switch pages");
        screen.showTab(0);
        step(t, at(0, 0));
        check(t.focused() != 0 && t.rootOf(t.focused()) == root, "focus lands inside the screen");

        const UiSettingItem* master = model.find("audio.master");
        check(master && approx(master->value, 1.0f), "the model starts at its defaults");
        UiEvent e;
        e.type = UiEventType::ValueChanged;
        e.widget = t.find("setting.audio.master", root);
        e.value = 0.3f;
        t.emit(e);
        step(t, at(0, 0));
        const std::vector<UiEvent> ev = drain(t);
        check(approx(model.find("audio.master")->value, 0.3f) && changes == 1 && lastKey == "audio.master", "a slider event updates the model and calls onChange");
        const UiEvent* cmd = command(ev, "setting:audio.master");
        check(cmd && approx(cmd->value, 0.3f), "and raises a setting command for the host");

        e = UiEvent{};
        e.type = UiEventType::ValueChanged;
        e.widget = t.find("setting.graphics.uiScale", root);
        e.value = 1.25f;
        t.emit(e);
        step(t, at(0, 0));
        check(approx(t.dpi().userScale, 1.25f), "the UI scale setting drives the tree's DPI");

        e = UiEvent{};
        e.type = UiEventType::Toggled;
        e.widget = t.find("setting.graphics.taa", root);
        e.value = 0.0f;
        t.emit(e);
        step(t, at(0, 0));
        check(approx(model.find("graphics.taa")->value, 0.0f), "a toggle event updates the model");

        e = UiEvent{};
        e.type = UiEventType::KeyCaptured;
        e.widget = t.find("bind.1", root);
        e.index = 12;
        t.emit(e);
        step(t, at(0, 0));
        const UiEvent* rb = command(drain(t), "rebind:Fire:0");
        check(rb && rb->index == 12, "a captured key raises a rebind command naming the action and slot");
        screen.setBinding(1, "K");
        check(rebind.rows[1].binding == "K" && t.get(t.find("bind.1", root))->text == "K", "the host reports the new binding back");
        model.find("audio.music")->value = 0.1f;
        screen.refresh();
        check(approx(t.get(t.find("setting.audio.music", root))->value, 0.1f), "refresh copies the model into the widgets");
    }
    {
        UiTree t;
        const UiWidgetId root = uiBuildMenu(t, "Pause", "Paused", {{"Resume", "resume", false}, {"Quit", "quit", true}},
                                            UiMenuOptions{true, "resume", 420});
        step(t, at(0, 0));
        check(t.inputState().pausesGame(), "an open pause menu pauses the game");
        const UiWidgetId first = t.focused();
        check(first != 0 && t.get(first)->text == "Resume", "the first entry is focused");
        step(t, navFrame(UiNav::Accept));
        check(command(drain(t), "resume") != nullptr, "choosing an entry raises its command");
        step(t, navFrame(UiNav::Cancel));
        check(command(drain(t), "resume") != nullptr, "Cancel raises the menu's cancel command");
        step(t, navFrame(UiNav::Down));
        step(t, navFrame(UiNav::Accept));
        check(command(drain(t), "quit") != nullptr, "the second entry raises its own");
        t.runBuiltinCommand("ui.close", first);
        check(!t.get(root)->visible, "ui.close hides the widget's root");
        t.runBuiltinCommand("ui.open:Pause", 0);
        check(t.get(root)->visible, "ui.open:Name shows a root by name");
        step(t, at(0, 0));
    }

    AVER_INFO("=== drawing ===");
    {
        UiTree t;
        UiWidget& root = box(t, 0, "Root", UiLayoutMode::VStack);
        root.inputMode = UiInputMode::Blocking;
        const UiWidgetId rid = root.id;
        UiWidget& b = mk(t, UiWidgetKind::Button, rid, "B");
        b.text = "Hello";
        UiWidget& s = mk(t, UiWidgetKind::Slider, rid, "S");
        s.height = 24;
        mk(t, UiWidgetKind::Toggle, rid, "T").text = "Option";
        step(t, at(0, 0));
        UiDrawList list;
        UiDrawListPainter painter(list, nullptr);
        t.draw(painter);
        check(!list.empty() && list.totalCommands() >= 1, "a tree draws geometry into a draw list with no font");
        t.get(rid)->visible = false;
        UiDrawList empty;
        UiDrawListPainter p2(empty, nullptr);
        t.draw(p2);
        check(empty.empty(), "a hidden root draws nothing");
    }

    AVER_INFO("=== host input mapping ===");
    {
        UiHostInputMapper m;
        UiHostSnapshot s;
        const auto frameOf = [&](f32 dt) { UiInputFrame f; const i32 vk = m.map(s, dt, f); return std::make_pair(f, vk); };

        s.keyPressed[0x28] = s.keyHeld[0x28] = true;   // Down
        auto r = frameOf(0.125f);
        check((r.first.nav & uiNavBit(UiNav::Down)) != 0 && r.second == 0x28, "a key press navigates and is reported for rebinding");
        s.keyPressed[0x28] = false;
        int fires = 0;
        for (int i = 0; i < 3; ++i) fires += (frameOf(0.125f).first.nav & uiNavBit(UiNav::Down)) ? 1 : 0;
        check(fires == 0, "a held key waits before it repeats");
        check((frameOf(0.125f).first.nav & uiNavBit(UiNav::Down)) != 0, "then repeats");
        s.keyHeld[0x28] = false;
        check((frameOf(0.125f).first.nav & uiNavBit(UiNav::Down)) == 0, "and stops on release");

        s.keyPressed[0x09] = true;
        r = frameOf(0.016f);
        check((r.first.nav & uiNavBit(UiNav::NextTab)) != 0, "Tab goes forward");
        s.keyHeld[0x10] = true;
        r = frameOf(0.016f);
        check((r.first.nav & uiNavBit(UiNav::PrevTab)) != 0, "Shift+Tab goes back");
        s.keyPressed[0x09] = false;

        s.keyPressed[0x0D] = true;
        s.keyPressed[0x1B] = true;
        r = frameOf(0.016f);
        check((r.first.nav & uiNavBit(UiNav::Accept)) && (r.first.nav & uiNavBit(UiNav::Cancel)), "Enter accepts and Escape cancels");
        check(!r.first.editKeys.empty() && r.first.editKeys[0] == UiEditKey::Enter, "Enter is also an edit key");
        s.keyPressed[0x0D] = s.keyPressed[0x1B] = false;

        s = UiHostSnapshot{};
        s.keyPressed['A'] = s.keyHeld['A'] = true;
        r = frameOf(0.016f);
        check(r.first.chars.size() == 1 && r.first.chars[0] == 'a', "a letter types lower case");
        s.keyHeld[0x10] = true;
        s.keyPressed['A'] = false;
        s.keyPressed['B'] = s.keyHeld['B'] = true;
        r = frameOf(0.016f);
        check(r.first.chars.size() == 1 && r.first.chars[0] == 'B', "Shift types upper case");
        s.keyHeld[0x11] = true;
        s.keyPressed['B'] = false;
        s.keyPressed['C'] = s.keyHeld['C'] = true;
        r = frameOf(0.016f);
        check(r.first.chars.empty(), "Ctrl suppresses typing");
        check(uiVkToChar('1', true) == '!' && uiVkToChar(0xBD, false) == '-' && uiVkToChar(0xBF, true) == '?' && uiVkToChar(0x70, false) == 0,
              "digits, punctuation and unmapped keys");

        s = UiHostSnapshot{};
        s.padConnected = true;
        s.padButton[10] = true;
        r = frameOf(0.016f);
        check((r.first.nav & uiNavBit(UiNav::Accept)) != 0, "the pad's A accepts");
        r = frameOf(0.016f);
        check((r.first.nav & uiNavBit(UiNav::Accept)) == 0, "once per press");
        s.padButton[10] = false;
        s.padAxis[0] = -0.9f;
        r = frameOf(0.016f);
        check((r.first.nav & uiNavBit(UiNav::Left)) != 0, "a stick pushed left navigates left");
        s.padAxis[0] = 0.2f;
        r = frameOf(0.016f);
        check((r.first.nav & uiNavBit(UiNav::Left)) == 0, "a small deflection does not");
        s.padConnected = false;
        s.padButton[11] = true;
        r = frameOf(0.016f);
        check((r.first.nav & uiNavBit(UiNav::Cancel)) == 0, "a disconnected pad is ignored");
    }

    if (g_failures) {
        AVER_ERROR("{} check(s) failed", g_failures);
        return 1;
    }
    AVER_INFO("all widget checks passed");
    return 0;
}
