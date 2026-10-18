// Accessibility in the retained UI (docs/ACCESSIBILITY.md): text scale (fonts only, not layout), high contrast
// (the contrast theme, authored colours ignored, a thicker focus ring, restored when it turns off), the spoken form of
// every widget kind (uiAccessibleText), the settings screen's new tabs and a 2.0 UI scale that keeps Back on screen, and
// the announcer on the widget C ABI (one announcement per focus change, value changes spoken, nothing for a same-widget
// setFocus). No GPU, no window.
#include "aver/core/Log.hpp"
#include "aver/ui/UiAccess.hpp"
#include "aver/ui/UiScreens.hpp"
#include "aver/ui/UiText.hpp"
#include "aver/ui/UiTree.hpp"
#include "aver/ui/ui_abi.h"
#include "aver/ui/ui_widget_abi.h"

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

static bool approx(f32 a, f32 b, f32 eps = 0.01f) { return std::fabs(a - b) <= eps; }

static UiEstimatedMetrics g_metrics;

static UiWidget& mk(UiTree& t, UiWidgetKind k, UiWidgetId parent, const char* name) {
    return *t.get(t.create(k, parent, name));
}

static UiInputFrame navFrame(UiNav n) {
    UiInputFrame f;
    f.pointerValid = false;
    f.nav = uiNavBit(n);
    return f;
}

static void step(UiTree& t, const UiInputFrame& in = UiInputFrame{}) { t.update(0.016f, in, g_metrics); }

// Records what the tree draws: filled rects with colours, and texts with their size and colour.
class RecordingPainter final : public UiPainter {
public:
    struct Rect { UiRect r; u32 rgba; };
    struct Text { std::string s; f32 px; u32 rgba; };
    std::vector<Rect> rects;
    std::vector<Text> texts;
    void pushClip(const UiRect&) override {}
    void popClip() override {}
    void fillRect(const UiRect& r, u32 rgba) override { rects.push_back({r, rgba}); }
    void image(const UiRect&, u64, const UiUvRect&, u32) override {}
    void text(f32, f32, std::string_view s, f32 px, u32 rgba) override { texts.push_back({std::string(s), px, rgba}); }
    f32 textWidth(std::string_view s, f32 px) const override { return g_metrics.textWidth(s, px); }
    f32 lineHeight(f32 px) const override { return g_metrics.lineHeight(px); }
    f32 ascent(f32 px) const override { return g_metrics.ascent(px); }
    const Text* find(const std::string& s) const {
        for (const Text& t : texts) if (t.s == s) return &t;
        return nullptr;
    }
    bool hasRectColour(u32 c) const {
        for (const Rect& r : rects) if (r.rgba == c) return true;
        return false;
    }
    // A menu focuses its first button, which then draws in its hover shade.
    bool hasButtonColour(u32 c) const { return hasRectColour(c) || hasRectColour(uiShade(c, 0.15f)); }
};

static std::string drawn(UiTree& t, RecordingPainter& p) {
    p = RecordingPainter{};
    step(t);
    t.draw(p);
    std::string sig;
    for (const auto& r : p.rects) sig += "R" + std::to_string(r.r.x) + "," + std::to_string(r.r.y) + "," + std::to_string(r.r.w) + "," + std::to_string(r.r.h) + "," + std::to_string(r.rgba) + ";";
    for (const auto& x : p.texts) sig += "T" + x.s + "," + std::to_string(x.px) + "," + std::to_string(x.rgba) + ";";
    return sig;
}

// A small menu: a Button with authored colours and a fixed-size Panel.
static UiWidgetId buildScene(UiTree& t) {
    UiWidget& root = mk(t, UiWidgetKind::Panel, 0, "Root");
    root.inputMode = UiInputMode::Menu;
    root.style = "clear";
    UiWidget& b = mk(t, UiWidgetKind::Button, root.id, "Go");
    b.text = "Go";
    b.offsets.left = 40; b.offsets.top = 60;
    b.width = 200; b.height = 60;
    b.bgColor = uiRgba(255, 0, 0);
    b.textColor = uiRgba(0, 255, 0);
    b.accentColor = uiRgba(0, 0, 255);
    UiWidget& label = mk(t, UiWidgetKind::Text, root.id, "Label");
    label.text = "Hello";
    label.offsets.left = 40; label.offsets.top = 200;
    label.width = 300; label.height = 40;
    return b.id;
}

static void testTextScale() {
    AVER_INFO("=== text scale ===");
    UiTree a, b;
    buildScene(a);
    buildScene(b);
    RecordingPainter pa, pb;
    const std::string sigA = drawn(a, pa);
    b.setAccess(UiAccess{});
    check(drawn(b, pb) == sigA, "the default UiAccess draws byte for byte what an untouched tree draws");

    const RecordingPainter::Text* base = pa.find("Hello");
    UiRect labelRect = a.get(a.find("Label"))->rect;
    check(base != nullptr, "the label draws");
    UiAccess big;
    big.textScale = 2.0f;
    a.setAccess(big);
    RecordingPainter big1;
    drawn(a, big1);
    const RecordingPainter::Text* scaled = big1.find("Hello");
    check(scaled && base && approx(scaled->px, base->px * 2.0f, 0.05f), "text scale 2.0 doubles the font size");
    check(a.get(a.find("Label"))->rect == labelRect, "and moves no rectangle: it is text only, not layout");

    // Wrapping: a wrapped text with a fixed width takes more lines at a larger size.
    UiTree w;
    UiWidget& root = mk(w, UiWidgetKind::Panel, 0, "Root");
    root.style = "clear";
    UiWidget& wrapped = mk(w, UiWidgetKind::Text, root.id, "Wrapped");
    wrapped.text = "alpha beta gamma delta epsilon zeta eta theta iota kappa";
    wrapped.wrap = true;
    wrapped.width = 240;
    wrapped.offsets.left = 10; wrapped.offsets.top = 10;
    step(w);
    UiVec2 sz1{}; i32 lines1 = 0;
    w.lastTextSize(w.find("Wrapped"), sz1, lines1);
    w.setAccess(big);
    step(w);
    UiVec2 sz2{}; i32 lines2 = 0;
    w.lastTextSize(w.find("Wrapped"), sz2, lines2);
    check(lines1 >= 1 && lines2 > lines1, "a larger text size wraps onto more lines");
    check(w.get(w.find("Wrapped"))->width == 240.0f, "(the authored width is untouched)");

    // A text scale below 1 shrinks it.
    UiAccess small;
    small.textScale = 0.75f;
    a.setAccess(small);
    RecordingPainter sm;
    drawn(a, sm);
    check(sm.find("Hello") && base && approx(sm.find("Hello")->px, base->px * 0.75f, 0.05f), "0.75 shrinks it");
}

static void testHighContrast() {
    AVER_INFO("=== high contrast ===");
    UiTree t;
    t.setTheme(uiBuiltinTheme("light"));   // the game's own theme: it must come back
    const UiWidgetId btn = buildScene(t);
    RecordingPainter off;
    drawn(t, off);
    const UiTheme light = uiBuiltinTheme("light");
    const UiTheme contrast = uiBuiltinTheme("contrast");
    check(off.hasButtonColour(uiRgba(255, 0, 0)) && off.find("Go") && off.find("Go")->rgba == uiRgba(0, 255, 0), "off: the authored background and text colours are used");

    UiAccess hc;
    hc.highContrast = true;
    t.setAccess(hc);
    RecordingPainter on;
    drawn(t, on);
    const UiStyle cb = contrast.resolve(UiWidgetKind::Button, "");
    check(!on.hasButtonColour(uiRgba(255, 0, 0)) &&
              (on.hasRectColour(cb.background[0]) || on.hasRectColour(cb.background[static_cast<usize>(UiVisualState::Hover)])), "on: the authored background is ignored and the contrast theme's button background is used");
    check(on.find("Go") && on.find("Go")->rgba == cb.text, "and so is its text colour");

    // The focus ring: thicker, in the contrast theme's colour.
    t.setFocus(btn);
    RecordingPainter ring;
    drawn(t, ring);
    f32 ringThickness = 0.0f;
    for (const auto& r : ring.rects) if (r.rgba == contrast.focusColor) { ringThickness = r.r.h; break; }
    const f32 expected = contrast.focusWidth * 2.0f * t.scale();
    check(ringThickness > 0.0f && approx(ringThickness, std::fmin(expected, t.get(btn)->rect.h * 0.5f), 0.05f), "the focus ring is the contrast colour at twice the width");

    // Off again: the game's own theme and the authored colours come back.
    t.setAccess(UiAccess{});
    RecordingPainter back;
    drawn(t, back);
    check(back.hasButtonColour(uiRgba(255, 0, 0)) && !back.hasRectColour(cb.background[0]) && t.theme().name == light.name, "turning it off restores the game's theme and the authored colours");
    RecordingPainter ring2;
    drawn(t, ring2);
    f32 normalRing = 0.0f;
    for (const auto& r : ring2.rects) if (r.rgba == light.focusColor) { normalRing = r.r.h; break; }
    check(normalRing > 0.0f && approx(normalRing, light.focusWidth * t.scale(), 0.05f), "and the ring is the game's again");

    // Layout is unchanged by the theme swap (same metrics).
    UiTree u;
    buildScene(u);
    RecordingPainter pu;
    drawn(u, pu);
    const UiRect before = u.get(u.find("Go"))->rect;
    u.setAccess(hc);
    step(u);
    check(u.get(u.find("Go"))->rect == before, "high contrast moves no widget");
}

// Pieces of a settings-style row for the spoken-form tests.
static UiWidgetId row(UiTree& t, UiWidgetId parent, const char* name, const char* label, UiWidgetKind kind, const char* ctlName) {
    UiWidget& r = mk(t, UiWidgetKind::Panel, parent, name);
    r.style = "clear";
    r.layout = UiLayoutMode::HStack;
    UiWidget& l = mk(t, UiWidgetKind::Text, r.id, "label");
    l.text = label;
    return mk(t, kind, r.id, ctlName).id;
}

struct FakeLoc {
    std::map<std::string, std::string> text;
    bool resolve(std::string_view raw, std::string& out) const {
        UiTextRef r;
        if (!uiParseTextRef(raw, r)) return false;
        const auto it = text.find(r.table + ":" + r.key);
        out = it != text.end() ? it->second : (r.hasDefault ? r.defaultText : "[" + r.table + ":" + r.key + "]");
        return true;
    }
};

static void testSpokenForm() {
    AVER_INFO("=== uiAccessibleText ===");
    UiTree t;
    UiWidget& root = mk(t, UiWidgetKind::Panel, 0, "Root");
    root.style = "clear";
    root.layout = UiLayoutMode::VStack;
    const UiWidgetId rid = root.id;

    UiWidget& play = mk(t, UiWidgetKind::Button, rid, "Play");
    play.text = "Play";
    check(uiAccessibleText(t, play.id) == "Play, button", "button: name and role");

    const UiWidgetId tog = row(t, rid, "r1", "Fullscreen", UiWidgetKind::Toggle, "tog");
    check(uiAccessibleText(t, tog) == "Fullscreen, checkbox, unchecked", "toggle: the label sibling names it, state follows");
    t.get(tog)->checked = true;
    check(uiAccessibleText(t, tog) == "Fullscreen, checkbox, checked", "checked");

    const UiWidgetId sld = row(t, rid, "r2", "UI scale", UiWidgetKind::Slider, "sld");
    UiWidget& s = *t.get(sld);
    s.minValue = 0.75f; s.maxValue = 2.0f; s.step = 0.05f; s.value = 1.0f; s.showValue = true;
    check(uiAccessibleText(t, sld) == "UI scale, slider, 1.00", "slider with a shown value reads the number");
    s.showValue = false;
    check(uiAccessibleText(t, sld) == "UI scale, slider, 20%", "and the percent of its range without");
    s.minValue = 0; s.maxValue = 1; s.value = 0.5f; s.showValue = true;
    check(uiAccessibleText(t, sld) == "UI scale, slider, 50%", "a 0..1 slider shows a percent");

    const UiWidgetId ch = row(t, rid, "r3", "Quality", UiWidgetKind::Choice, "ch");
    t.get(ch)->items = {"Low", "Medium", "High"};
    t.get(ch)->selected = 1;
    check(uiAccessibleText(t, ch) == "Quality, choice, Medium, 2 of 3", "choice: the item and its place");

    const UiWidgetId kb = row(t, rid, "r4", "Jump", UiWidgetKind::KeyBind, "kb");
    t.get(kb)->text = "Space";
    check(uiAccessibleText(t, kb) == "Jump, key binding, Space", "key bind: the label names it, the bound key is the value");
    t.get(kb)->listening = true;
    check(uiAccessibleText(t, kb) == "Jump, key binding, press a key", "and it says so while listening");

    const UiWidgetId ed = row(t, rid, "r5", "Name", UiWidgetKind::TextInput, "ed");
    t.get(ed)->text = "Ada";
    check(uiAccessibleText(t, ed) == "Name, edit field, Ada", "text input: its content");
    const UiWidgetId pw = row(t, rid, "r6", "Password", UiWidgetKind::TextInput, "pw");
    t.get(pw)->text = "hunter2";
    t.get(pw)->masked = true;
    const std::string said = uiAccessibleText(t, pw);
    check(said == "Password, edit field, hidden" && said.find("hunter2") == std::string::npos, "a masked field's content is never read");

    const UiWidgetId ls = row(t, rid, "r7", "Levels", UiWidgetKind::List, "ls");
    t.get(ls)->items = {"First", "Second"};
    t.get(ls)->selected = 1;
    check(uiAccessibleText(t, ls) == "Levels, list, Second", "list: the selected row");

    UiWidget& over = mk(t, UiWidgetKind::Button, rid, "Quit");
    over.text = "X";
    over.accessName = "Exit game";
    check(uiAccessibleText(t, over.id) == "Exit game, button", "accessName overrides the text");
    over.enabled = false;
    check(uiAccessibleText(t, over.id) == "Exit game, button, unavailable", "a disabled widget says so");

    // Labels with no sibling fall back to the placeholder.
    UiWidget& lone = mk(t, UiWidgetKind::TextInput, rid, "Lone");
    lone.placeholder = "Search";
    check(uiAccessibleText(t, lone.id) == "Search, edit field", "no label: the placeholder names it");
    check(uiAccessibleText(t, 9999).empty(), "an unknown widget says nothing");

    // References localize: names, roles and states all go through the tree's resolver.
    FakeLoc loc;
    loc.text["Access:role.button"] = "Taste";
    loc.text["UI:play"] = "Spielen";
    t.setTextResolver([&loc](std::string_view raw, std::string& out) { return loc.resolve(raw, out); }, [] { return 1u; });
    play.text = "@UI:play=Play";
    check(uiAccessibleText(t, play.id) == "Spielen, Taste", "a culture switch re-reads the name and the role");
}

static void testSettingsScreen() {
    AVER_INFO("=== the settings screen ===");
    UiSettingsModel m = uiDefaultSettingsModel();
    const std::vector<std::string> tabs = m.tabs();
    auto has = [&](const char* tab) { for (const std::string& s : tabs) if (s == tab) return true; return false; };
    check(has("@UI:settings.tab.access=Accessibility") && has("@UI:settings.tab.captions=Captions and Speech") && has("@UI:settings.tab.input=Input Assist"),
          "the default model has the Accessibility, Captions and Speech, and Input Assist tabs");
    const UiSettingItem* scale = m.find("graphics.uiScale");
    check(scale && scale->tab == "@UI:settings.tab.access=Accessibility" && approx(scale->maxValue, 2.0f), "graphics.uiScale moved to the Accessibility tab and now goes to 2.0");
    check(m.find("access.ui.textScale") && m.find("access.colour.mode") && m.find("access.captions.size") && m.find("access.speech.rate") && m.find("access.input.holdScale"), "the new keys are in the model");
    check(!m.find("access.colour.simulate"), "the simulation preview is not a player option");

    UiTree t;
    t.setViewport(UiRect{0, 0, 1280, 720});
    UiDpi dpi = t.dpi();
    dpi.userScale = 2.0f;
    t.setDpi(dpi);
    UiSettingsScreen screen(t, m);
    const UiWidgetId root = screen.build();
    step(t);
    const UiWidget* back = t.get(t.find("Back", root));
    check(back != nullptr, "the screen has a Back button");
    if (back) {
        const UiRect r = back->rect;
        check(r.x >= 0 && r.y >= 0 && r.right() <= 1280.5f && r.bottom() <= 720.5f && r.w > 0 && r.h > 0,
              "at UI scale 2.0 on 1280x720 Back is still inside the viewport");
    }
    const UiWidget* win = t.get(t.find("Window", root));
    check(win && win->rect.x >= 0 && win->rect.right() <= 1280.5f && win->rect.y >= 0 && win->rect.bottom() <= 720.5f, "and so is the whole window");

    // A change on the Accessibility page raises the new key's Command event.
    const usize accessTab = 2;
    screen.showTab(accessTab);
    step(t);
    const UiWidgetId slider = t.find("setting.access.ui.textScale", root);
    check(slider != 0 && t.setFocus(slider), "the text size slider is focusable on its page");
    UiEvent e;
    while (t.pollEvent(e)) {}
    step(t, navFrame(UiNav::Right));
    bool saw = false;
    while (t.pollEvent(e)) if (e.type == UiEventType::Command && e.text == "setting:access.ui.textScale") saw = true;
    check(saw, "adjusting it raises setting:access.ui.textScale");
    check(m.find("access.ui.textScale")->value > 1.0f, "and the model took the value");
}

// ---- the announcer, through the widget C ABI --------------------------------------------------------------

struct Said { std::string text; int interrupt; };
static std::vector<Said> g_said;
static int32_t announce(const char* text, int32_t interrupt) {
    g_said.push_back({text, interrupt});
    return 1;
}

static void frame() {
    aver_ui_begin_frame(0, 0, 1280, 720);
    aver_ui_widgets_frame(0.016f);
}

static void testAnnouncer() {
    AVER_INFO("=== the announcer ===");
    const uint32_t root = aver_ui_widget_create(AVER_UI_KIND_PANEL, 0, "AnnRoot");
    aver_ui_widget_set_prop(root, "inputMode", "menu");
    aver_ui_widget_set_prop(root, "style", "clear");
    aver_ui_widget_set_prop(root, "layout", "vstack");
    const uint32_t a = aver_ui_widget_create(AVER_UI_KIND_BUTTON, root, "A");
    aver_ui_widget_set_text(a, "Alpha");
    const uint32_t b = aver_ui_widget_create(AVER_UI_KIND_BUTTON, root, "B");
    aver_ui_widget_set_text(b, "Beta");
    const uint32_t s = aver_ui_widget_create(AVER_UI_KIND_SLIDER, root, "S");
    aver_ui_widget_set_prop(s, "accessName", "Volume");
    aver_ui_widget_set_prop(s, "minValue", "0");
    aver_ui_widget_set_prop(s, "maxValue", "1");
    aver_ui_widget_set_prop(s, "step", "0.1");
    aver_ui_widget_set_prop(s, "showValue", "true");
    aver_ui_widget_set_value(s, 0.5f);

    check(std::string(aver_ui_widget_accessible_text(a)) == "Alpha, button", "aver_ui_widget_accessible_text speaks a widget");
    check(aver_ui_widget_accessible_text(99999) == nullptr, "and answers NULL for an unknown id");

    // The announcer goes in BEFORE the first frame: opening a menu focuses its first control, and that is a focus change.
    g_said.clear();
    aver_ui_set_announcer(&announce);
    frame();
    frame();
    check(g_said.size() == 1 && g_said[0].text == "Alpha, button" && g_said[0].interrupt == 1, "the menu's first control takes focus on open: spoken once, interrupting");
    aver_ui_set_focus(a);
    frame();
    check(g_said.size() == 1, "setFocus on the widget that already has focus says nothing");
    aver_ui_set_focus(b);
    frame();
    check(g_said.size() == 2 && g_said[1].text == "Beta, button", "the next focus change speaks the new widget");

    aver_ui_set_focus(s);
    frame();
    check(g_said.size() == 3 && g_said[2].text == "Volume, slider, 50%", "a slider is spoken with its value");
    aver_ui_input_nav(AVER_UI_NAV_RIGHT);
    frame();
    check(g_said.size() == 4 && g_said[3].text == "Volume, slider, 60%", "adjusting it speaks the new value");

    aver_ui_set_announcer(nullptr);
    aver_ui_set_focus(a);
    frame();
    check(g_said.size() == 4, "NULL clears the announcer");

    aver_ui_widget_destroy(root);
}

int main() {
    AVER_INFO("UiAccessTest");
    testTextScale();
    testHighContrast();
    testSpokenForm();
    testSettingsScreen();
    testAnnouncer();
    AVER_INFO("UiAccessTest done: {} failure(s)", g_failures);
    return g_failures ? 1 : 0;
}
