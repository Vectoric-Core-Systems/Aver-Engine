// Widget animation through the widget C ABI: the version, listing, play / reverse / loop / pause / seek / stop, the finish
// event and the render-transform properties. Links the DLL the C# side loads. No GPU, no window.
#include "aver/core/Log.hpp"
#include "aver/ui/ui_widget_abi.h"

#include <cmath>
#include <cstring>
#include <string>

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(float a, float b, float eps = 0.02f) { return std::fabs(a - b) <= eps; }

static void frame(float dt) {
    aver_ui_begin_frame(0, 0, 1280, 720);
    aver_ui_widgets_frame(dt);
}

// Counts the animation-finished events waiting, dropping every event.
static int finished(std::string* name = nullptr, float* value = nullptr) {
    int n = 0, type = 0, index = 0;
    uint32_t widget = 0;
    float v = 0;
    char text[64];
    while (aver_ui_event_poll(&type, &widget, &v, &index, text, sizeof text)) {
        if (type == AVER_UI_EVENT_ANIM_FINISHED) {
            ++n;
            if (name) *name = text;
            if (value) *value = v;
        }
    }
    return n;
}

int main() {
    check(aver_ui_widget_abi_version() == AVER_UI_WIDGET_ABI_VERSION && AVER_UI_WIDGET_ABI_VERSION >= 2, "the widget ABI has animation (version 2 and up)");

    const char* doc =
        "OCUI 2\nWIDGET Root panel -\nWIDGET Box panel Root\n"
        "ANIM Fade Box opacity 0 0 1 1\n"
        "ANIM Move Box renderTranslate 0 0,0 2 40,0\n";
    const uint32_t root = aver_ui_layout_open(doc, 0);
    check(root != 0, "the layout opens");
    check(aver_ui_anim_count(root) == 2 && std::string(aver_ui_anim_name(root, 0)) == "Fade" && aver_ui_anim_name(root, 5) == nullptr, "animations are listed");
    check(approx(aver_ui_anim_length(root, "Move"), 2.0f) && aver_ui_anim_length(root, "Nope") < 0.0f, "lengths are in seconds");
    check(aver_ui_anim_play(root, "Nope", 1, 1, 0, 0) == 0 && aver_ui_anim_play(99999, "Fade", 1, 1, 0, 0) == 0, "an unknown animation or root does not play");

    const uint32_t box = aver_ui_widget_find("Box", 0);
    check(aver_ui_anim_play(root, "Fade", 1.0f, 1, 0, 0) == 1 && aver_ui_anim_is_playing(root, "Fade") == 1, "play");
    frame(0.5f);
    check(approx(aver_ui_anim_time(root, "Fade"), 0.5f), "time advances with the frame");
    check(std::string(aver_ui_widget_get_prop(box, "opacity")) == "1", "the widget's own opacity is unchanged");
    check(aver_ui_anim_pause(root, "Fade", 1) == 1 && aver_ui_anim_is_playing(root, "Fade") == 0, "pause");
    frame(1.0f);
    check(approx(aver_ui_anim_time(root, "Fade"), 0.5f), "a paused animation holds");
    aver_ui_anim_pause(root, "Fade", 0);
    frame(0.75f);
    check(aver_ui_anim_is_playing(root, "Fade") == 0 && aver_ui_anim_time(root, "Fade") < 0.0f, "it ends");
    std::string name;
    float value = 0;
    check(finished(&name, &value) == 1 && name == "Fade" && approx(value, 1.0f), "and raises one finished event named for it");

    check(aver_ui_anim_seek(root, "Move", 1.0f) == 1 && approx(aver_ui_anim_time(root, "Move"), 1.0f) && aver_ui_anim_is_playing(root, "Move") == 0, "seek");
    check(aver_ui_anim_play(root, "Move", 2.0f, 0, 1, 1) == 1 && approx(aver_ui_anim_time(root, "Move"), 2.0f), "reverse ping-pong loop starts at the end");
    frame(0.5f);
    check(approx(aver_ui_anim_time(root, "Move"), 1.0f), "speed 2 runs it back one second in half a second");
    check(aver_ui_anim_reverse(root, "Move") == 1, "reverse flips it");
    check(aver_ui_anim_stop(root, "Move", 1) == 1 && aver_ui_anim_time(root, "Move") < 0.0f, "stop");
    check(finished() == 0, "a stopped animation raises no finished event");

    check(aver_ui_widget_set_prop(box, "renderRotation", "45") == 1 && std::string(aver_ui_widget_get_prop(box, "renderRotation")) == "45" &&
              aver_ui_widget_set_prop(box, "renderScale", "2 3") == 1 && std::string(aver_ui_widget_get_prop(box, "renderScale")) == "2 3",
          "the render transform is a property");
    check(aver_ui_widget_set_prop(box, "renderScale", "2") == 0, "and a half-written vector is refused");

    aver_ui_widget_destroy(root);
    check(aver_ui_anim_count(root) == 0 && aver_ui_anim_play(root, "Fade", 1, 1, 0, 0) == 0, "destroying the root drops its animations");

    if (g_failures) {
        AVER_ERROR("{} check(s) failed", g_failures);
        return 1;
    }
    AVER_INFO("all animation ABI checks passed");
    return 0;
}
