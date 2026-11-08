// Bindings and event routes through the widget C ABI: the resolver a host installs, BIND records from a layout, aver_ui_bind,
// change notifications, and aver_ui_event_route / aver_ui_route_poll. Links the DLL the C# side loads. No GPU, no window.
#include "aver/core/Log.hpp"
#include "aver/ui/ui_widget_abi.h"

#include <cmath>
#include <cstring>
#include <map>
#include <string>

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static std::map<std::string, double> g_numbers;
static std::map<std::string, std::string> g_texts;
static std::map<std::string, int> g_bools;
static int g_resolveCalls = 0;

static int32_t resolve(int32_t source, const char* path, double* number, char* text, int32_t cap) {
    ++g_resolveCalls;
    const std::string key = std::string(source == AVER_UI_BIND_VARIABLE ? "var:" : "vm:") + path;
    if (g_numbers.count(key)) { *number = g_numbers[key]; return AVER_UI_BIND_NUMBER; }
    if (g_bools.count(key)) { *number = g_bools[key]; return AVER_UI_BIND_BOOL; }
    if (g_texts.count(key)) { std::strncpy(text, g_texts[key].c_str(), static_cast<size_t>(cap) - 1); text[cap - 1] = 0; return AVER_UI_BIND_TEXT; }
    return AVER_UI_BIND_NONE;
}

static void frame(float dt = 0.016f) {
    aver_ui_begin_frame(0, 0, 1280, 720);
    aver_ui_widgets_frame(dt);
}

static std::string prop(uint32_t id, const char* key) {
    const char* v = aver_ui_widget_get_prop(id, key);
    return v ? v : "";
}

int main() {
    check(aver_ui_widget_abi_version() == AVER_UI_WIDGET_ABI_VERSION && AVER_UI_WIDGET_ABI_VERSION == 3, "the widget ABI is version 3");

    const char* doc =
        "OCUI 2\nWIDGET Root panel -\nWIDGET Score text Root\nWIDGET Bar progress Root\nWIDGET Go button Root\n"
        "BIND Score text vm:Player.Score format \"Score {0:D3}\"\n"
        "BIND Bar value var:Health mode change\n"
        "EVENT Go click graph OnGo x\n"
        "EVENT Go focus command go.focus\n";
    const uint32_t root = aver_ui_layout_open(doc, 0);
    check(root != 0 && aver_ui_binding_count(root) == 2, "the layout opens with two bindings");

    g_numbers["vm:Player.Score"] = 7;
    g_numbers["var:Health"] = 0.5;
    frame();
    const uint32_t score = aver_ui_widget_find("Score", 0), bar = aver_ui_widget_find("Bar", 0);
    check(std::string(aver_ui_widget_get_text(score)).empty(), "with no resolver installed nothing is bound");
    aver_ui_set_binding_resolver(resolve);
    frame();
    check(std::string(aver_ui_widget_get_text(score)) == "Score 007" && prop(bar, "value") == "0.5", "the resolver's numbers reach the widgets, formatted");
    g_numbers["var:Health"] = 0.9;
    frame();
    check(prop(bar, "value") == "0.5", "a change-mode binding waits");
    aver_ui_bindings_changed("Health");
    frame();
    check(prop(bar, "value") == "0.9", "until it is told");

    const uint32_t go = aver_ui_widget_find("Go", 0);
    g_texts["vm:Player.Name"] = "Ada";
    g_bools["vm:Player.Dead"] = 1;
    check(aver_ui_bind(go, "text", AVER_UI_BIND_VIEWMODEL, "Player.Name", "Hi {0}", 0) == 1 && aver_ui_bind(go, "visible", AVER_UI_BIND_VIEWMODEL, "Player.Dead", "", 0) == 1 &&
              aver_ui_bind(go, "nonsense", AVER_UI_BIND_VIEWMODEL, "x", "", 0) == 0 && aver_ui_bind(0, "text", AVER_UI_BIND_VIEWMODEL, "x", "", 0) == 0,
          "aver_ui_bind adds a binding in code");
    frame();
    check(std::string(aver_ui_widget_get_text(go)) == "Hi Ada" && prop(go, "visible") == "true", "text and bool sources are set");

    aver_ui_set_binding_resolver(nullptr);
    const int calls = g_resolveCalls;
    frame();
    check(g_resolveCalls == calls, "clearing the resolver stops the calls");

    check(aver_ui_event_route(go, "click", AVER_UI_ROUTE_METHOD, "Menu.Play") == 1 && aver_ui_event_route(go, "bogus", AVER_UI_ROUTE_METHOD, "x") == 0 &&
              aver_ui_event_route(go, "click", 9, "x") == 0 && aver_ui_event_route(0, "click", AVER_UI_ROUTE_GRAPH, "x") == 0,
          "aver_ui_event_route adds a route and refuses bad names, actions and widgets");
    int32_t action = -1, type = -1, index = 0;
    uint32_t widget = 0;
    float value = 0;
    char target[64], args[64], text[64];
    check(aver_ui_route_poll(&action, &widget, &type, &value, &index, target, sizeof target, args, sizeof args, text, sizeof text) == 0, "nothing has fired yet");

    // Focus is a routed event the button raises when it takes the focus: its route raises a command.
    aver_ui_set_focus(go);
    frame();
    int commands = 0;
    int32_t et = 0, ei = 0;
    uint32_t ew = 0;
    float ev = 0;
    char buf[64];
    while (aver_ui_event_poll(&et, &ew, &ev, &ei, buf, sizeof buf))
        if (et == AVER_UI_EVENT_COMMAND && std::string(buf) == "go.focus") ++commands;
    check(commands == 1, "a focus route raised its command");

    aver_ui_widget_destroy(root);
    check(aver_ui_binding_count(root) == 0, "destroying the root drops them");

    if (g_failures) {
        AVER_ERROR("{} check(s) failed", g_failures);
        return 1;
    }
    AVER_INFO("all binding ABI checks passed");
    return 0;
}
