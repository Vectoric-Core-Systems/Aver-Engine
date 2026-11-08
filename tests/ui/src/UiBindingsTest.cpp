// Headless test for bindings and event routes: the path and format helpers, BIND / EVENT records reaching a UiTree, a
// resolver feeding frame-mode and change-mode bindings, and widget events routed to commands, graph events and methods.
#include "aver/core/Log.hpp"
#include "aver/ui/UiBindings.hpp"
#include "aver/ui/UiLayoutAsset.hpp"
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

static bool approx(f32 a, f32 b, f32 eps = 0.01f) { return std::fabs(a - b) <= eps; }

static UiEstimatedMetrics g_metrics;

static UiBindValue num(double v) { UiBindValue b; b.kind = UiBindValue::Kind::Number; b.number = v; return b; }
static UiBindValue boolean(bool v) { UiBindValue b; b.kind = UiBindValue::Kind::Bool; b.number = v ? 1 : 0; return b; }
static UiBindValue text(const char* s) { UiBindValue b; b.kind = UiBindValue::Kind::Text; b.text = s; return b; }

static void frame(UiTree& t, f32 dt = 0.016f, const UiInputFrame& in = {}) {
    t.setViewport(UiRect{0, 0, 800, 600});
    t.update(dt, in, g_metrics);
}

static std::vector<UiEvent> drain(UiTree& t) {
    std::vector<UiEvent> out;
    UiEvent e;
    while (t.pollEvent(e)) out.push_back(e);
    return out;
}

int main() {
    AVER_INFO("=== paths ===");
    {
        UiBindSource s;
        std::string p;
        check(uiParseBindPath("vm:Player.Score", s, p) && s == UiBindSource::ViewModel && p == "Player.Score", "vm: names a view-model path");
        check(uiParseBindPath("VAR:Health", s, p) && s == UiBindSource::Variable && p == "Health", "var: names a graph variable, case-insensitively");
        check(uiParseBindPath("Player.Score", s, p) && s == UiBindSource::ViewModel && p == "Player.Score", "no prefix is a view-model path");
        check(!uiParseBindPath("", s, p), "an empty path is refused");
        check(uiBindPathText(UiBindSource::Variable, "A") == "var:A" && uiBindPathText(UiBindSource::ViewModel, "A") == "vm:A", "and prints back with its prefix");
    }

    AVER_INFO("=== formatting ===");
    {
        check(uiFormatBound(num(5), "") == "5" && uiFormatBound(num(2.5), "") == "2.5" && uiFormatBound(boolean(true), "") == "true" && uiFormatBound(text("hi"), "") == "hi",
              "no format: the value as it reads");
        check(uiFormatBound(num(7), "Score: {0:D6}") == "Score: 000007", "D pads integers");
        check(uiFormatBound(num(3.14159), "{0:F2}") == "3.14" && uiFormatBound(num(3), "{0:F1}x") == "3.0x", "F fixes decimals");
        check(uiFormatBound(num(0.256), "{0:P0}") == "26%" && uiFormatBound(num(0.256), "{0:P1}") == "25.6%", "P is a percentage of a 0..1 value");
        check(uiFormatBound(num(1234567), "{0:N0}") == "1,234,567" && uiFormatBound(num(-1234.5), "{0:N1}") == "-1,234.5", "N groups thousands");
        check(uiFormatBound(num(3.5), "{0:0.00}") == "3.50" && uiFormatBound(num(3.5), "{0:0.##}") == "3.5" && uiFormatBound(num(3), "{0:0.##}") == "3" && uiFormatBound(num(4), "{0:000}") == "004",
              "custom patterns: zeros fix or pad, hashes are optional");
        check(uiFormatBound(boolean(true), "{0:On|Off}") == "On" && uiFormatBound(boolean(false), "{0:On|Off}") == "Off", "a bool picks a|b");
        check(uiFormatBound(text("Ab"), "{0:U}") == "AB" && uiFormatBound(text("Ab"), "{0:L}") == "ab", "text can be cased");
        check(uiFormatBound(num(9), "HP") == "HP9", "a format with no placeholder is a prefix");
        check(uiFormatBound(num(9), "{{{0}}}") == "{9}", "doubled braces are literal");
        check(uiFormatBound(UiBindValue{}, "{0}").empty(), "no value formats to nothing");
    }

    AVER_INFO("=== BIND records in a tree ===");
    {
        UiLayoutDoc doc;
        std::string err;
        const bool ok = uiParseLayout(
            "OCUI 2\nWIDGET Root panel -\nWIDGET Score text Root\nWIDGET Bar progress Root\nWIDGET Warn panel Root\nWIDGET Name text Root\n"
            "BIND Score text vm:Player.Score format \"Score: {0:D4}\"\n"
            "BIND Bar value var:Health mode change\n"
            "BIND Warn visible vm:Player.Low\n"
            "BIND Name text vm:Player.Name format {0:U}\n"
            "BIND Ghost text vm:X\n"
            "BIND Score nonsense vm:X\n", doc, &err);
        check(ok, "the layout parses: " + err);
        UiTree t;
        const UiInstantiateResult built = uiInstantiateLayoutEx(doc, t, 0, {});
        check(built.root != 0 && t.bindingCount(built.root) == 4 && built.errors.size() == 2, "four bindings attach; a missing widget and an unknown property are reported");

        std::map<std::string, UiBindValue> vm, vars;
        int reads = 0;
        t.setBindingResolver([&](UiBindSource s, const std::string& path, UiBindValue& out) {
            ++reads;
            auto& m = s == UiBindSource::Variable ? vars : vm;
            const auto it = m.find(path);
            if (it == m.end()) return false;
            out = it->second;
            return true;
        });
        const UiWidgetId score = t.find("Score"), bar = t.find("Bar"), warn = t.find("Warn"), name = t.find("Name");

        vm["Player.Score"] = num(42);
        vm["Player.Low"] = boolean(false);
        vm["Player.Name"] = text("neo");
        vars["Health"] = num(0.75);
        frame(t);
        check(t.get(score)->text == "Score: 0042", "a number is formatted into the text");
        check(approx(t.get(bar)->value, 0.75f), "a graph variable sets a value");
        check(!t.get(warn)->visible && t.get(name)->text == "NEO", "a bool sets a bool property; text is formatted");

        vm["Player.Score"] = num(43);
        frame(t);
        check(t.get(score)->text == "Score: 0043", "frame mode follows the source every frame");
        vars["Health"] = num(0.5);
        frame(t);
        check(approx(t.get(bar)->value, 0.75f), "change mode does not read until told");
        t.bindingsChanged("Health");
        frame(t);
        check(approx(t.get(bar)->value, 0.5f), "bindingsChanged(path) makes it read again");
        vars["Health"] = num(0.25);
        t.bindingsChanged("Other");
        frame(t);
        check(approx(t.get(bar)->value, 0.5f), "an unrelated path leaves it alone");
        t.bindingsChanged();
        frame(t);
        check(approx(t.get(bar)->value, 0.25f), "an empty path marks everything");

        t.get(score)->text = "edited";
        frame(t);
        check(t.get(score)->text == "edited", "an unchanged source is not written again");
        vm["Player.Score"] = num(44);
        frame(t);
        check(t.get(score)->text == "Score: 0044", "but a changed one is");

        vm.erase("Player.Name");
        frame(t);
        check(t.get(name)->text == "NEO", "a source with no value leaves the widget as it was");
        vm["Player.Low"] = boolean(true);
        frame(t);
        check(t.get(warn)->visible, "visible follows a bool");

        check(t.bind(score, "textColor", UiBindSource::ViewModel, "Player.Tint") && !t.bind(score, "nonsense", UiBindSource::ViewModel, "A") && !t.bind(9999, "text", UiBindSource::ViewModel, "A"),
              "bind() adds one in code and refuses an unknown property or widget");
        vm["Player.Tint"] = text("#FF0000");
        frame(t);
        check(t.get(score)->textColor == uiRgba(255, 0, 0), "a colour property takes text");

        t.setBindingResolver({});
        vm["Player.Score"] = num(1);
        frame(t);
        check(t.get(score)->text == "Score: 0044", "with no resolver the bindings hold");
        const int before = reads;
        frame(t);
        check(reads == before, "and cost nothing");
    }

    AVER_INFO("=== EVENT records ===");
    {
        UiLayoutDoc doc;
        std::string err;
        const bool ok = uiParseLayout(
            "OCUI 2\nWIDGET Root panel -\nSET Root inputMode blocking\n"
            "WIDGET Play button Root\nSET Play anchors 0 0\nSET Play width 200\nSET Play height 60\n"
            "WIDGET Vol slider Root\nSET Vol anchors 0 0\nSET Vol offsets 0 100 0 0\n"
            "EVENT Play click command game.start\n"
            "EVENT Play click graph OnPlay a b\n"
            "EVENT Play hover method Hud.Hover\n"
            "EVENT Play unhover command hud.unhover\n"
            "EVENT Vol valuechanged method Settings.SetVolume\n"
            "EVENT Play focus graph OnFocus\n"
            "EVENT Play nonsense command x\n"
            "EVENT Play click teleport x\n"
            "EVENT Ghost click command x\n", doc, &err);
        check(ok, "the layout parses: " + err);
        UiTree t;
        const UiInstantiateResult built = uiInstantiateLayoutEx(doc, t, 0, {});
        check(built.root != 0 && built.errors.size() == 3, "a bad event name, a bad action and a missing widget are reported");
        const UiWidgetId play = t.find("Play"), vol = t.find("Vol");
        UiInputFrame away;
        away.pointerX = 700;
        away.pointerY = 500;
        const auto idle = [&] { frame(t, 0.016f, away); };
        idle();
        drain(t);
        UiRoutedEvent r;
        while (t.pollRouted(r)) {}

        UiEvent ev;
        ev.type = UiEventType::Clicked;
        ev.widget = play;
        t.emit(ev);
        idle();
        const std::vector<UiEvent> got = drain(t);
        usize clicks = 0, commands = 0;
        std::string cmd;
        for (const UiEvent& e : got) {
            if (e.type == UiEventType::Clicked) ++clicks;
            if (e.type == UiEventType::Command) { ++commands; cmd = e.text; }
        }
        check(clicks == 1 && commands == 1 && cmd == "game.start", "a click route raises a Command event beside the click");
        check(t.pollRouted(r) && r.action == UiRouteAction::Graph && r.target == "OnPlay" && r.args == std::vector<std::string>({"a", "b"}) &&
                  r.widget == play && r.eventType == static_cast<u8>(UiEventType::Clicked),
              "a graph route is queued with its target and arguments");
        check(!t.pollRouted(r), "and only that one");

        UiEvent vc;
        vc.type = UiEventType::ValueChanged;
        vc.widget = vol;
        vc.value = 0.4f;
        t.emit(vc);
        idle();
        check(t.pollRouted(r) && r.action == UiRouteAction::Method && r.target == "Settings.SetVolume" && approx(r.value, 0.4f), "a method route carries the event's value");

        UiEvent unrouted;
        unrouted.type = UiEventType::Clicked;
        unrouted.widget = vol;
        t.emit(unrouted);
        idle();
        check(!t.pollRouted(r), "an event with no route goes nowhere");
        drain(t);

        // Hover through the real pointer: enter, then leave.
        UiInputFrame over;
        over.pointerX = 50;
        over.pointerY = 20;
        frame(t, 0.016f, over);
        check(t.pollRouted(r) && r.action == UiRouteAction::Method && r.target == "Hud.Hover" && r.eventType == static_cast<u8>(UiEventType::Hovered) && r.value == 1.0f,
              "the pointer entering a widget with a hover route fires it");
        drain(t);
        check(!t.pollRouted(r), "once");
        idle();
        bool unhover = false;
        for (const UiEvent& e : drain(t)) if (e.type == UiEventType::Command && e.text == "hud.unhover") unhover = true;
        check(unhover, "leaving fires the unhover route");

        // A real click: press and release on the button.
        UiInputFrame press = over;
        press.buttons = 1;
        frame(t, 0.016f, press);
        UiInputFrame release = over;
        frame(t, 0.016f, release);
        bool started = false;
        for (const UiEvent& e : drain(t)) if (e.type == UiEventType::Command && e.text == "game.start") started = true;
        check(started, "a pointer click reaches the route");
        while (t.pollRouted(r)) {}

        t.destroy(built.root);
        frame(t);
        check(t.runtimeOf(built.root) == nullptr, "destroying the root drops the routes");
    }

    AVER_INFO("=== finished route and a tree without records ===");
    {
        UiLayoutDoc doc;
        uiParseLayout("OCUI 2\nWIDGET Root panel -\nWIDGET Box panel Root\nANIM Fade Box opacity 0 0 1 1\nEVENT Root finished command fade.done\n", doc);
        UiTree t;
        const UiInstantiateResult built = uiInstantiateLayoutEx(doc, t, 0, {});
        t.animPlay(built.root, "Fade");
        frame(t, 1.5f);
        bool done = false;
        for (const UiEvent& e : drain(t)) if (e.type == UiEventType::Command && e.text == "fade.done") done = true;
        check(done, "an animation's finish can be routed to a command");

        UiTree plain;
        const UiWidgetId r = plain.create(UiWidgetKind::Panel, 0, "Root");
        frame(plain);
        check(plain.runtimeOf(r) == nullptr && plain.bindingCount(r) == 0, "a tree built without records has no runtime");
    }

    if (g_failures) {
        AVER_ERROR("{} check(s) failed", g_failures);
        return 1;
    }
    AVER_INFO("all binding checks passed");
    return 0;
}
