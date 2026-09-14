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

    AVER_INFO("=== fonts and text ===");
    {
        // A hand-written .ocfont, so what each case exercises is legible in the case. 'A' has ink,
        // ' ' has an advance and no bitmap, and 'j' descends below the baseline.
        const char* doc =
            "OCFONT 1\n"
            "# a comment, and a trailing one below\n"
            "NAME Test\n"
            "SIZE 16\n"
            "ATLAS Fonts/Test.png\n"
            "METRICS 15 -4 19\n"
            "GLYPH 32 0 0 0 0 0 0 0 0 4\n"          // space: advance only
            "GLYPH 65 0.0 0.0 0.5 0.5 8 11 1 -11 10\n"   // 'A'
            "GLYPH 106 0.5 0.0 0.6 0.6 4 15 0 -11 5\n";  // 'j', descends
        ui::UiFont f;
        std::string err;
        check(ui::parseOcfont(doc, f, &err), "an .ocfont parses");
        check(f.name == "Test" && f.pixelSize == 16.0f, "its name and size survive");
        check(f.lineHeight == 19.0f && f.ascent == 15.0f && f.descent == -4.0f, "and its metrics");
        check(f.glyphs.size() == 3, "three glyphs");
        check(f.glyph('A') != nullptr && f.glyph('Z') == nullptr,
              "a glyph it has is found and one it does not is null -- no invented fallback");

        check(!ui::parseOcfont("NAME NoHeader\nGLYPH 65 0 0 1 1 1 1 0 0 1\n", f, &err),
              "a document with no OCFONT header FAILS rather than parsing empty");
        check(ui::parseOcfont(doc, f, &err), "and the good one still parses after that");
        check(!ui::parseOcfont("OCFONT 1\nNAME Empty\n", f, &err),
              "a font with NO GLYPHS fails too -- it would draw nothing and report success");
        (void)ui::parseOcfont(doc, f, &err);

        // ---- measurement, which alignment needs before anything is drawn ----------------------
        check(ui::uiTextWidth(f, "A") == 10.0f, "one glyph measures its ADVANCE, not its ink width");
        check(ui::uiTextWidth(f, "AA") == 20.0f, "two accumulate");
        check(ui::uiTextWidth(f, "A A") == 24.0f, "and a space contributes its advance");
        check(ui::uiTextWidth(f, "AA\nA") == 20.0f, "a multi-line string measures its WIDEST line");
        check(ui::uiTextLines(f.glyphs.empty() ? "" : "a\nb\nc") == 3, "and its lines are counted");
        check(ui::uiTextWidth(f, "AZA") == 20.0f, "an unknown codepoint contributes nothing at all");

        // ---- geometry ---------------------------------------------------------------------------
        ui::UiDrawList dl;
        f.atlasTexture = 77;
        const f32 endX = dl.addText(100.0f, 200.0f, "A", f, 0xFFFFFFFF);
        check(endX == 110.0f, "addText returns the pen after the run, so runs can be chained");
        check(dl.vertices().size() == 4, "one glyph is one quad");
        // THE PEN IS THE BASELINE. 'A' has offY -11, so its top edge must be 11 ABOVE y, and getting
        // this backwards is the single easiest way to render a whole HUD a line too low.
        check(dl.vertices()[0].y == 189.0f, "and the quad sits ABOVE the baseline, by offY");
        check(dl.vertices()[0].x == 101.0f, "offset right by the glyph's left bearing");

        dl.clear();
        dl.addText(0.0f, 0.0f, "A A", f, 0xFFFFFFFF);
        check(dl.vertices().size() == 8,
              "a SPACE emits no quad -- it has an advance and no bitmap, and two dead triangles per "
              "space would ride in every string");
        check(dl.totalCommands() == 1, "and a whole run merges into ONE draw command");

        dl.clear();
        dl.addText(0.0f, 0.0f, "A\nA", f, 0xFFFFFFFF);
        check(dl.vertices().size() == 8, "a newline emits no quad either");
        check(dl.vertices()[4].y - dl.vertices()[0].y == 19.0f, "and advances the pen by lineHeight");
        check(dl.vertices()[4].x == dl.vertices()[0].x, "returning it to the starting x");
    }

    AVER_INFO("=== hit testing ===");
    {
        ui::UiDrawList dl;
        dl.addHitRect(11, 10.0f, 10.0f, 100.0f, 20.0f);
        check(dl.hitTest(50.0f, 20.0f) == 11, "a point inside a registered rect finds its id");
        check(dl.hitTest(5.0f, 20.0f) == 0, "and outside finds nothing");
        // Half-open, like every other rect convention here: the far edge belongs to the next pixel.
        check(dl.hitTest(10.0f, 10.0f) == 11, "the near edge is inside");
        check(dl.hitTest(110.0f, 20.0f) == 0, "the far edge is NOT");
        check(dl.hitTest(0.0f, 0.0f) == 0, "and a rect with id 0 could never be registered anyway");

        // TOPMOST WINS. Two overlapping rects in the same layer: the later one is drawn on top, so
        // it is the one a person is pointing at.
        dl.clear();
        dl.addHitRect(1, 0.0f, 0.0f, 50.0f, 50.0f);
        dl.addHitRect(2, 0.0f, 0.0f, 50.0f, 50.0f);
        check(dl.hitTest(25.0f, 25.0f) == 2, "the LAST registration in a layer wins");

        // And a higher layer beats a lower one regardless of registration order.
        dl.clear();
        dl.setLayer(ui::UiLayer::Overlay);
        dl.addHitRect(9, 0.0f, 0.0f, 50.0f, 50.0f);
        dl.setLayer(ui::UiLayer::Background);
        dl.addHitRect(3, 0.0f, 0.0f, 50.0f, 50.0f);
        check(dl.hitTest(25.0f, 25.0f) == 9,
              "a HIGHER layer wins even when the lower one registered later");

        // CLIPPED AWAY IS NOT CLICKABLE, which is the whole reason the clip is captured at
        // registration rather than at query time: a widget scrolled out of its panel is not there.
        dl.clear();
        dl.setLayer(ui::UiLayer::Content);
        dl.pushClip(ui::UiClip{0, 0, 20, 20});
        dl.addHitRect(4, 0.0f, 0.0f, 100.0f, 100.0f);
        dl.popClip();
        check(dl.hitTest(5.0f, 5.0f) == 4, "inside both the rect and its clip hits");
        check(dl.hitTest(50.0f, 50.0f) == 0, "inside the rect but OUTSIDE its clip does not");

        dl.clear();
        check(dl.hitRects().empty(), "clear() drops the hit rects with the geometry");
    }

    if (g_failures == 0) AVER_INFO("=== all UI draw list tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
