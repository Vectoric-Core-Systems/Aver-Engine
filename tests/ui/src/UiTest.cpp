// Headless test for the UI draw list: geometry, batching, clip nesting, layers, premultiplication.
#include "aver/ui/UiDrawList.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;

static int g_failures = 0;

// Logs one assertion and counts the failures.
static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// Runs the suite. Returns 0 when every check passed.
int main() {
    AVER_INFO("=== geometry ===");
    {
        ui::UiDrawList dl;
        dl.addRect(10, 20, 100, 50, 0xFFFFFFFF);
        check(dl.vertices().size() == 4, "a rect is four vertices");
        check(dl.indices().size() == 6, "a rect is two triangles");
        check(dl.vertices()[0].x == 10.0f && dl.vertices()[0].y == 20.0f, "top-left corner");
        check(dl.vertices()[2].x == 110.0f && dl.vertices()[2].y == 70.0f, "bottom-right corner");

        const usize before = dl.vertices().size();
        dl.addRect(0, 0, 0, 50, 0xFFFFFFFF);
        dl.addRect(0, 0, 50, -1, 0xFFFFFFFF);
        check(dl.vertices().size() == before, "zero and negative sizes emit nothing");
    }

    AVER_INFO("=== batching ===");
    {
        ui::UiDrawList dl;
        for (int i = 0; i < 100; ++i) dl.addRect(f32(i) * 4, 0, 3, 3, 0xFFFFFFFF);
        check(dl.totalCommands() == 1, "100 untextured rects merge into ONE command");
        check(dl.commands(ui::UiLayer::Content)[0].indexCount == 600, "and the command covers all 600 indices");

        dl.addTexturedRect(0, 0, 10, 10, 42, 0, 0, 1, 1, 0xFFFFFFFF);
        check(dl.totalCommands() == 2, "a different texture starts a new command");
        dl.addTexturedRect(20, 0, 10, 10, 42, 0, 0, 1, 1, 0xFFFFFFFF);
        check(dl.totalCommands() == 2, "the same texture merges again");
        dl.addRect(40, 0, 10, 10, 0xFFFFFFFF);
        check(dl.totalCommands() == 3, "switching back to untextured starts another");
    }

    AVER_INFO("=== clipping ===");
    {
        ui::UiDrawList dl;
        dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);
        const usize unclipped = dl.totalCommands();
        dl.pushClip(ui::UiClip{0, 0, 100, 100});
        dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);
        check(dl.totalCommands() == unclipped + 1, "a clip change starts a new command");

        dl.pushClip(ui::UiClip{50, 50, 500, 500});
        const ui::UiClip c = dl.clip();
        check(c.left == 50 && c.top == 50 && c.right == 100 && c.bottom == 100,
              "a nested clip is intersected with its parent, not replaced");
        dl.popClip();
        check(dl.clip().right == 100, "popping restores the parent clip");
        dl.popClip();

        ui::UiDrawList dl2;
        dl2.pushClip(ui::UiClip{0, 0, 0, 0});
        dl2.addRect(0, 0, 10, 10, 0xFFFFFFFF);
        check(dl2.empty(), "an empty clip emits no geometry");
    }

    AVER_INFO("=== layers ===");
    {
        ui::UiDrawList dl;
        dl.setLayer(ui::UiLayer::Content);
        dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);
        dl.setLayer(ui::UiLayer::Overlay);
        dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);
        dl.setLayer(ui::UiLayer::Tooltip);
        dl.addRect(0, 0, 10, 10, 0xFFFFFFFF);

        check(dl.commands(ui::UiLayer::Content).size() == 1, "content layer has its own command");
        check(dl.commands(ui::UiLayer::Overlay).size() == 1, "overlay layer likewise");
        check(dl.commands(ui::UiLayer::Tooltip).size() == 1, "tooltip layer likewise");
        check(dl.commands(ui::UiLayer::Background).empty(), "an unused layer is empty");
        check(dl.totalCommands() == 3, "three commands in total");

        check(dl.vertices().size() == 12, "all three layers share one vertex buffer");

        dl.setLayer(ui::UiLayer::Content);
        dl.addRect(20, 0, 10, 10, 0xFFFFFFFF);
        check(dl.commands(ui::UiLayer::Content).size() == 1,
              "returning to a layer merges within it (its own commands are still adjacent)");
    }

    AVER_INFO("=== premultiplied alpha ===");
    {
        check(ui::uiPremultiply(0xFFFFFFFF) == 0xFFFFFFFF, "opaque white is unchanged");
        check(ui::uiPremultiply(0x00FFFFFF) == 0x00000000, "fully transparent white becomes zero");

        const u32 half = ui::uiPremultiply(0x80FFFFFF);   // alpha 128, white
        check((half >> 24) == 0x80, "alpha is preserved, not premultiplied into itself");
        const u32 r = half & 0xFF;
        check(r == 128, "a half-alpha white premultiplies to 128, not 127 (rounded, not truncated)");

        check((ui::uiPremultiply(0x80808080) & 0xFF) == 64, "mid grey at half alpha rounds to 64");
    }

    if (g_failures == 0) AVER_INFO("=== all UI draw list tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
