// Text outline and shadow passes (G5 U2): the CPU geometry of the passes, and the ABI call that walks them through the ordinary text
// path (links the DLL the C# side loads). No GPU, no window.
#include "TestFont.hpp"

#include "aver/core/Log.hpp"
#include "aver/ui/UiTextEffect.hpp"
#include "aver/ui/ui_abi.h"

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

static bool near(f32 a, f32 b, f32 eps = 1e-3f) { return std::fabs(a - b) <= eps; }

static std::map<std::string, std::vector<u8>> g_files;

static int32_t openCb(int32_t, const char* name, const uint8_t** data, uint64_t* size, void** token) {
    const auto it = g_files.find(name);
    if (it == g_files.end()) return 0;
    *data = it->second.data();
    *size = it->second.size();
    *token = nullptr;
    return 1;
}

static void closeCb(void*) {}

int main() {
    AVER_INFO("=== ring geometry ===");
    {
        std::vector<UiTextFxPass> none;
        uiOutlineOffsets(0.0f, none);
        uiOutlineOffsets(0.4f, none);
        check(none.empty(), "an outline under half a pixel has no samples");

        std::vector<UiTextFxPass> r1;
        uiOutlineOffsets(1.0f, r1);
        check(r1.size() == 8, "radius 1: eight samples");
        bool onRing = true;
        for (const UiTextFxPass& p : r1) onRing = onRing && near(std::sqrt(p.dx * p.dx + p.dy * p.dy), 1.0f);
        check(onRing, "every sample lies on the ring");
        check(near(r1[0].dx, 1.0f) && near(r1[0].dy, 0.0f), "the first sample is on +X");
        check(near(r1[2].dx, 0.0f) && near(r1[2].dy, 1.0f), "a quarter turn is on +Y (screen down)");

        std::vector<UiTextFxPass> r2;
        uiOutlineOffsets(2.0f, r2);
        check(r2.size() == 13, "radius 2: 13 samples (ceil of the circumference)");
        std::vector<UiTextFxPass> r4;
        uiOutlineOffsets(4.0f, r4);
        check(r4.size() == 26 + 13, "radius 4: the outer ring of 26 and an inner ring of 13 at radius 2");
        bool inner = false;
        for (const UiTextFxPass& p : r4) inner = inner || near(std::sqrt(p.dx * p.dx + p.dy * p.dy), 2.0f);
        check(inner, "the inner ring sits at half the radius");
        std::vector<UiTextFxPass> big;
        uiOutlineOffsets(40.0f, big);
        check(big.size() == 64, "the count is capped at 32 per ring");
    }

    AVER_INFO("=== passes ===");
    {
        const auto plain = uiTextFxPasses(UiTextFxStyle{}, 0xFFFFFFFFu);
        check(plain.size() == 1 && plain[0].fill && plain[0].dx == 0 && plain[0].dy == 0 && plain[0].rgba == 0xFFFFFFFFu, "nothing set: the single fill pass");

        UiTextFxStyle s;
        s.outlineSize = 1.0f;
        s.outlineRgba = 0xFF0000FFu;
        auto o = uiTextFxPasses(s, 0xFFFFFFFFu);
        check(o.size() == 9 && o.back().fill && o.front().rgba == 0xFF0000FFu && !o.front().fill, "an outline: eight ring copies in its colour, then the fill");

        s.outlineRgba = 0x00000000u;
        check(uiTextFxPasses(s, 1u).size() == 1, "a fully transparent outline draws nothing");

        UiTextFxStyle sh;
        sh.shadowRgba = 0x80000000u;
        sh.shadowOffX = 2.0f;
        sh.shadowOffY = 3.0f;
        auto p = uiTextFxPasses(sh, 7u);
        check(p.size() == 2 && near(p[0].dx, 2.0f) && near(p[0].dy, 3.0f) && p[0].rgba == 0x80000000u && p[1].fill, "a shadow: one copy at the offset, then the fill");

        sh.shadowOutlineSize = 1.0f;
        sh.outlineSize = 1.0f;
        p = uiTextFxPasses(sh, 7u);
        check(p.size() == 8 + 1 + 8 + 1, "shadow outline, shadow, outline, fill in that order");
        check(near(p[0].dx, 3.0f) && near(p[0].dy, 3.0f) && near(p[8].dx, 2.0f) && near(p[8].dy, 3.0f), "the shadow outline ring is centred on the shadow offset");
        check(p[9].rgba == 0xFF000000u && !p[9].fill && p.back().fill, "the outline follows the shadow and the fill is last");

        UiTextFxStyle off;
        off.shadowRgba = 0x00FFFFFFu;
        off.shadowOffX = 5.0f;
        check(uiTextFxPasses(off, 1u).size() == 1, "a shadow with alpha 0 is off");
    }

    AVER_INFO("=== the bounds a styled run grows to ===");
    {
        UiTextFxStyle s;
        UiTextFxGrow g = uiTextFxGrow(s);
        check(g.left == 0 && g.top == 0 && g.right == 0 && g.bottom == 0, "no effect, no growth");
        s.outlineSize = 3.0f;
        g = uiTextFxGrow(s);
        check(g.left == 3 && g.right == 3 && g.top == 3 && g.bottom == 3, "an outline grows every side by its size");
        s.shadowRgba = 0xFF000000u;
        s.shadowOffX = 5.0f;
        s.shadowOffY = -1.0f;
        g = uiTextFxGrow(s);
        check(g.right == 5 && g.left == 3 && g.bottom == 3 && g.top == 3, "a shadow offset to the right grows the right edge past the outline");
        s.shadowOutlineSize = 2.0f;
        g = uiTextFxGrow(s);
        check(g.right == 7 && g.left == 3 && g.top == 3 && g.bottom == 3, "its own outline adds to the shifted side");
    }

    AVER_INFO("=== the ABI call ===");
    {
        testfont::Spec latin = testfont::simpleSpec();
        g_files["latin.ttf"] = testfont::buildFont(latin);
        aver_ui_text_set_loader(&openCb, &closeCb);
        check(aver_ui_text_load_fontset("OCFONTS 1\nDEFAULT Body\nFAMILY Body\nFACE latin.ttf\n") == 1, "a font set loads");

        aver_ui_begin_frame(0, 0, 800, 600);
        float whl[3] = {};
        const float plain = aver_ui_text_block(10, 10, "AB", 0xFFFFFFFFu, nullptr, 20.0f, 0, 0.0f, 0, whl);
        const int plainVerts = aver_ui_vertex_count();
        check(plain > 0.0f && plainVerts == 8, "a plain block: two glyph quads");

        aver_ui_begin_frame(0, 0, 800, 600);
        float fx[3] = {};
        const float same = aver_ui_text_block_fx(10, 10, "AB", 0xFFFFFFFFu, nullptr, 20.0f, 0, 0.0f, 0, 0.0f, 0xFF000000u, 0u, 0.0f, 0.0f, 0.0f, fx);
        check(aver_ui_vertex_count() == plainVerts && near(same, plain) && near(fx[0], whl[0]) && near(fx[1], whl[1]),
              "no outline and no shadow draw exactly the plain block and report its size");

        aver_ui_begin_frame(0, 0, 800, 600);
        const float outlined = aver_ui_text_block_fx(10, 10, "AB", 0xFFFFFFFFu, nullptr, 20.0f, 0, 0.0f, 0, 1.0f, 0xFF000000u, 0u, 0.0f, 0.0f, 0.0f, fx);
        check(aver_ui_vertex_count() == plainVerts * 9 && near(outlined, plain) && near(fx[0], whl[0]),
              "an outline of 1 is eight copies and the fill, and the block size is the text's");

        aver_ui_begin_frame(0, 0, 800, 600);
        aver_ui_text_block_fx(10, 10, "AB", 0xFFFFFFFFu, nullptr, 20.0f, 0, 0.0f, 0, 0.0f, 0xFF000000u, 0x80000000u, 1.0f, 1.0f, 0.0f, nullptr);
        check(aver_ui_vertex_count() == plainVerts * 2, "a shadow is one more copy");

        aver_ui_text_set_loader(nullptr, nullptr);
        aver_ui_begin_frame(0, 0, 800, 600);
        check(aver_ui_text_block_fx(10, 10, "AB", 0xFFFFFFFFu, nullptr, 20.0f, 0, 0.0f, 0, 2.0f, 0xFF000000u, 0x80000000u, 1.0f, 1.0f, 1.0f, fx) == 0.0f &&
                  aver_ui_vertex_count() == 0,
              "with no font nothing is drawn and the height is 0");
    }

    if (g_failures == 0) AVER_INFO("UiTextEffectTest: all checks passed");
    return g_failures == 0 ? 0 : 1;
}
