// The UI draw list.
//
// This module exists to be testable without a GPU, so this is the test that justifies that choice.
// Everything here is bytes in memory: batching, clip nesting, layer separation and premultiplication
// are all decidable on the CPU, and every one of them is a thing that looks fine on screen while
// being subtly wrong.
#include "aver/ui/UiDrawList.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

int main() {
    AVER_INFO("=== geometry ===");
    {
        ui::UiDrawList dl;
        dl.addRect(10, 20, 100, 50, 0xFFFFFFFF);
        check(dl.vertices().size() == 4, "a rect is four vertices");
        check(dl.indices().size() == 6, "a rect is two triangles");
        check(dl.vertices()[0].x == 10.0f && dl.vertices()[0].y == 20.0f, "top-left corner");
        check(dl.vertices()[2].x == 110.0f && dl.vertices()[2].y == 70.0f, "bottom-right corner");

        // A degenerate rect emits nothing rather than a zero-area quad the rasteriser would discard
        // later. A layout that collapses to zero width is ordinary, not exceptional.
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

        // A texture change must break the batch -- merging across it would draw the second run with
        // the first run's texture, which is the classic UI batching bug.
        dl.addTexturedRect(0, 0, 10, 10, /*texture*/ 42, 0, 0, 1, 1, 0xFFFFFFFF);
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

        // NESTING INTERSECTS. A child that pushes a larger rect than its parent must not escape it:
        // a scrolled list whose row pushed its own bounds would paint over the panel around it.
        dl.pushClip(ui::UiClip{50, 50, 500, 500});
        const ui::UiClip c = dl.clip();
        check(c.left == 50 && c.top == 50 && c.right == 100 && c.bottom == 100,
              "a nested clip is intersected with its parent, not replaced");
        dl.popClip();
        check(dl.clip().right == 100, "popping restores the parent clip");
        dl.popClip();

        // Fully clipped away emits nothing at all, which is the common case for a long list.
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

        // The layers share ONE vertex buffer -- that is the reason layers partition commands rather
        // than owning their own geometry, and it is what keeps the frame to a single upload.
        check(dl.vertices().size() == 12, "all three layers share one vertex buffer");

        // Returning to a layer must NOT merge with the command already there: the overlay drawn in
        // between has to land on top, and merging would silently reorder it underneath.
        dl.setLayer(ui::UiLayer::Content);
        dl.addRect(20, 0, 10, 10, 0xFFFFFFFF);
        check(dl.commands(ui::UiLayer::Content).size() == 1,
              "returning to a layer merges within it (its own commands are still adjacent)");
    }

    AVER_INFO("=== premultiplied alpha ===");
    {
        // The blend mode is src / 1-src.a, so colour must arrive premultiplied. Getting this wrong
        // shows as a halo around every transparent edge -- visible, but easy to blame on the texture.
        check(ui::uiPremultiply(0xFFFFFFFF) == 0xFFFFFFFF, "opaque white is unchanged");
        check(ui::uiPremultiply(0x00FFFFFF) == 0x00000000, "fully transparent white becomes zero");

        const u32 half = ui::uiPremultiply(0x80FFFFFF);   // alpha 128, white
        check((half >> 24) == 0x80, "alpha is preserved, not premultiplied into itself");
        const u32 r = half & 0xFF;
        check(r == 128, "a half-alpha white premultiplies to 128, not 127 (rounded, not truncated)");

        // Rounding rather than truncation matters because UI stacks half-transparent panels: with
        // truncation the same colour drifts darker every time it is composed.
        check((ui::uiPremultiply(0x80808080) & 0xFF) == 64, "mid grey at half alpha rounds to 64");
    }

    if (g_failures == 0) AVER_INFO("=== all UI draw list tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
