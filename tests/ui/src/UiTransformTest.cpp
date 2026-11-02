// Parity P40 / P41 / P38 on the UI draw list and its C ABI: the 2D affine transform stack (rects, glyph quads, meshes,
// clips, hit rects), per-draw sampler and blend state, the mesh export, and the text spacing block. No GPU, no window.
#include "TestFont.hpp"

#include "aver/core/Log.hpp"
#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/ui_abi.h"

#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(float a, float b, float eps = 0.02f) { return std::fabs(a - b) <= eps; }

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
    AVER_INFO("=== P40: the transform stack ===");
    {
        ui::UiDrawList dl;
        dl.addRect(10, 20, 100, 50, 0xFFFFFFFF);
        const ui::UiVertex plain = dl.vertices()[0];
        dl.clear();
        dl.pushTransform(ui::UiAffine{});   // identity
        dl.addRect(10, 20, 100, 50, 0xFFFFFFFF);
        check(!dl.hasTransform(), "an identity push is no transform");
        check(dl.vertices()[0].x == plain.x && dl.vertices()[0].y == plain.y && dl.vertices()[2].x == 110.0f, "and the vertices are bit for bit the untransformed ones");
        dl.popTransform();

        // A quarter turn about the origin: x' = -y, y' = x (a = 0, b = 1, c = -1, d = 0).
        dl.clear();
        dl.pushTransform(ui::UiAffine{0, 1, -1, 0, 0, 0});
        dl.addRect(10, 20, 100, 50, 0xFFFFFFFF);
        check(approx(dl.vertices()[0].x, -20) && approx(dl.vertices()[0].y, 10), "a quarter turn moves the top-left corner (10,20) to (-20,10)");
        check(approx(dl.vertices()[2].x, -70) && approx(dl.vertices()[2].y, 110), "and the bottom-right (110,70) to (-70,110)");
        check(dl.vertices()[1].u == 1.0f && dl.vertices()[1].v == 0.0f, "texture coordinates are not touched");
        dl.popTransform();
        dl.addRect(0, 0, 4, 4, 0xFFFFFFFF);
        check(dl.vertices()[dl.vertices().size() - 2].x == 4.0f, "after the pop, draws are untransformed again");

        // Composition: scale 2 then translate: the pushed matrix is applied FIRST.
        dl.clear();
        dl.pushTransform(ui::UiAffine{1, 0, 0, 1, 100, 0});   // outer: translate
        dl.pushTransform(ui::UiAffine{2, 0, 0, 2, 0, 0});     // inner: scale
        dl.addRect(1, 1, 2, 2, 0xFFFFFFFF);
        check(approx(dl.vertices()[0].x, 102) && approx(dl.vertices()[0].y, 2) && approx(dl.vertices()[2].x, 106), "nested pushes compose: scale, then translate");
        dl.popTransform();
        dl.popTransform();
        dl.popTransform();   // one too many
        dl.addRect(1, 1, 2, 2, 0xFFFFFFFF);
        check(dl.vertices()[dl.vertices().size() - 2].x == 3.0f, "an extra pop is harmless");

        // Non-finite: ignored, and the pop still pairs.
        dl.clear();
        dl.pushTransform(ui::UiAffine{1, 0, 0, 1, 5, 0});
        dl.pushTransform(ui::UiAffine{std::numeric_limits<f32>::quiet_NaN(), 0, 0, 1, 0, 0});
        dl.addRect(0, 0, 2, 2, 0xFFFFFFFF);
        check(approx(dl.vertices()[0].x, 5) && std::isfinite(dl.vertices()[0].y), "a NaN matrix changes nothing");
        dl.popTransform();
        dl.addRect(0, 0, 2, 2, 0xFFFFFFFF);
        check(approx(dl.vertices()[dl.vertices().size() - 2].x, 7), "and its pop leaves the earlier transform in force (the quad spans x 5..7)");
        dl.clear();
        check(!dl.hasTransform(), "clear() empties the stack");

        // Meshes and glyph quads.
        dl.pushTransform(ui::UiAffine{0, 1, -1, 0, 0, 0});
        const ui::UiVertex tri[3] = {{0, 0, 0, 0, 0xFFFFFFFF}, {10, 0, 1, 0, 0xFFFFFFFF}, {0, 10, 0, 1, 0xFFFFFFFF}};
        const u32 idx[3] = {0, 1, 2};
        dl.addMesh(tri, 3, idx, 3, 7);
        check(approx(dl.vertices()[1].x, 0) && approx(dl.vertices()[1].y, 10) && approx(dl.vertices()[2].x, -10), "a mesh is transformed vertex by vertex");
        const usize n = dl.vertices().size();
        dl.addGlyphQuad(0, 0, 10, 10, 7, 0, 0, 1, 1, 0xFFFFFFFF, 0.25f, 10.0f);
        check(dl.vertices().size() == n + 4, "a skewed glyph quad is one quad");
        check(approx(dl.vertices()[n].x, -0) && approx(dl.vertices()[n].y, 2.5f), "and is sheared first, then transformed (top-left (2.5,0) -> (-0,2.5))");

        // Clips and hit rects become the box of their corners.
        dl.clear();
        dl.pushTransform(ui::UiAffine{0, 1, -1, 0, 0, 0});
        dl.pushClip(ui::UiClip{10, 20, 110, 70});
        check(dl.clip().left == -70 && dl.clip().top == 10 && dl.clip().right == -20 && dl.clip().bottom == 110, "a clip rect becomes the box of its rotated corners");
        dl.popClip();
        dl.addHitRect(5, 10, 20, 100, 50);
        check(dl.hitRects().size() == 1 && approx(dl.hitRects()[0].x, -70) && approx(dl.hitRects()[0].w, 50) && approx(dl.hitRects()[0].h, 100), "a hit rect likewise");
        dl.clear();
        const f32 s = std::sin(0.5f), c = std::cos(0.5f);
        dl.pushTransform(ui::UiAffine{c, s, -s, c, 0, 0});
        dl.pushClip(ui::UiClip{0, 0, 100, 100});
        check(dl.clip().left == -48 && dl.clip().top == 0 && dl.clip().right == 88 && dl.clip().bottom == 136,
              "any other angle gives the box of the rotated corners (here -48..88 by 0..136 for 0.5 rad)");
        dl.clear();
        dl.pushTransform(ui::UiAffine{1e30f, 0, 0, 1e30f, 0, 0});
        dl.pushClip(ui::UiClip{0, 0, 100, 100});
        check(dl.clip().right <= (1 << 24) && dl.clip().left >= -(1 << 24), "a huge scale clamps the clip instead of overflowing");
    }

    AVER_INFO("=== P41: sampler and blend state ===");
    {
        ui::UiDrawList dl;
        dl.addTexturedRect(0, 0, 4, 4, 9, 0, 0, 1, 1, 0xFFFFFFFF);
        dl.addTexturedRect(8, 0, 4, 4, 9, 0, 0, 1, 1, 0xFFFFFFFF);
        check(dl.totalCommands() == 1, "same texture, same state: one command");
        dl.setSampler(ui::UiFilter::Nearest, ui::UiAddress::Repeat);
        dl.addTexturedRect(16, 0, 4, 4, 9, 0, 0, 1, 1, 0xFFFFFFFF);
        check(dl.totalCommands() == 2, "a new sampler splits the batch");
        const ui::UiDrawCmd& b = dl.commands(ui::UiLayer::Content)[1];
        check(b.filter == ui::UiFilter::Nearest && b.address == ui::UiAddress::Repeat && b.blend == ui::UiBlend::Mix, "and the command carries it");
        dl.addTexturedRect(24, 0, 4, 4, 9, 0, 0, 1, 1, 0xFFFFFFFF);
        check(dl.totalCommands() == 2, "the same sampler merges again");
        dl.setBlend(ui::UiBlend::Add);
        dl.addTexturedRect(32, 0, 4, 4, 9, 0, 0, 1, 1, 0xFFFFFFFF);
        check(dl.totalCommands() == 3 && dl.commands(ui::UiLayer::Content)[2].blend == ui::UiBlend::Add, "a new blend splits it too");
        dl.clear();
        check(dl.filter() == ui::UiFilter::Linear && dl.address() == ui::UiAddress::Clamp && dl.blend() == ui::UiBlend::Mix, "clear() restores the defaults");
        dl.addRect(0, 0, 4, 4, 0xFFFFFFFF);
        const ui::UiDrawCmd& d = dl.commands(ui::UiLayer::Content)[0];
        check(d.filter == ui::UiFilter::Linear && d.address == ui::UiAddress::Clamp && d.blend == ui::UiBlend::Mix, "a default draw is linear, clamped, mixed (what every draw was before)");

        aver_ui_begin_frame(0, 0, 100, 100);
        aver_ui_set_blend(3);
        aver_ui_set_sampler(1, 2);
        aver_ui_rect(0, 0, 4, 4, 0xFFFFFFFF);
        aver_ui_set_blend(99);
        aver_ui_set_sampler(-1, 0);
        aver_ui_rect(8, 0, 4, 4, 0xFFFFFFFF);
        const auto* list = static_cast<const ui::UiDrawList*>(aver_ui_draw_list());
        check(list && list->totalCommands() == 1, "an out-of-range blend or sampler is ignored (the state stays)");
        check(list && list->commands(ui::UiLayer::Content)[0].blend == ui::UiBlend::Mul && list->commands(ui::UiLayer::Content)[0].address == ui::UiAddress::Mirror,
              "through the ABI: multiply, nearest, mirror");
        aver_ui_begin_frame(0, 0, 100, 100);
        aver_ui_rect(0, 0, 4, 4, 0xFFFFFFFF);
        list = static_cast<const ui::UiDrawList*>(aver_ui_draw_list());
        check(list->commands(ui::UiLayer::Content)[0].blend == ui::UiBlend::Mix, "aver_ui_begin_frame resets the state");
    }

    AVER_INFO("=== the mesh export and the transform through the ABI ===");
    {
        aver_ui_begin_frame(0, 0, 100, 100);
        const float xyuv[12] = {0, 0, 0, 0,  10, 0, 1, 0,  0, 10, 0, 1};
        const uint32_t col[3] = {0x80FFFFFF, 0x80FFFFFF, 0x80FFFFFF};
        const uint32_t idx[3] = {0, 1, 2};
        aver_ui_mesh(xyuv, col, 3, idx, 3, 0);
        check(aver_ui_vertex_count() == 3 && aver_ui_command_count() == 1, "aver_ui_mesh adds the triangle");
        const auto* list = static_cast<const ui::UiDrawList*>(aver_ui_draw_list());
        check(list->vertices()[0].rgba == ui::uiPremultiply(0x80FFFFFF), "colours are premultiplied on the way in");
        const uint32_t bad[3] = {0, 1, 3};
        aver_ui_mesh(xyuv, col, 3, bad, 3, 0);
        aver_ui_mesh(xyuv, col, 0, idx, 3, 0);
        aver_ui_mesh(xyuv, col, 3, idx, 2, 0);
        aver_ui_mesh(nullptr, col, 3, idx, 3, 0);
        aver_ui_mesh(xyuv, col, 100000, idx, 3, 0);
        check(aver_ui_vertex_count() == 3, "an index past the vertices, a zero or a too-large count and NULLs draw nothing");
        aver_ui_push_transform(2, 0, 0, 2, 1, 1);
        aver_ui_mesh(xyuv, col, 3, idx, 3, 0);
        aver_ui_pop_transform();
        check(approx(list->vertices()[4].x, 21) && approx(list->vertices()[4].y, 1), "aver_ui_push_transform applies to the mesh (10,0) -> (21,1)");
        aver_ui_push_clip(0, 0, 10, 10);
        aver_ui_push_transform(1, 0, 0, 1, 50, 0);
        aver_ui_push_clip(0, 0, 10, 10);
        check(list->clip().empty(), "a transformed clip is intersected with the clip in force (here, disjoint)");
        aver_ui_pop_clip();
        aver_ui_pop_transform();
        aver_ui_pop_clip();
    }

    AVER_INFO("=== P38: text spacing ===");
    {
        testfont::Spec latin = testfont::simpleSpec();
        g_files["latin.ttf"] = testfont::buildFont(latin);
        aver_ui_text_set_loader(&openCb, &closeCb);
        check(aver_ui_text_load_fontset("OCFONTS 1\nDEFAULT Body\nFAMILY Body\nFACE latin.ttf\n") == 1, "the test font set loads");
        aver_ui_begin_frame(0, 0, 800, 600);
        float base[3] = {}, w[3] = {};
        check(aver_ui_text_measure("AB A", nullptr, 20.0f, 0, 0.0f, base) == 1 && base[0] > 0, "baseline measure");

        const float none[4] = {0, 0, 0, 0};
        check(aver_ui_text_measure_ex("AB A", nullptr, 20.0f, 0, 0.0f, none, w) == 1 && approx(w[0], base[0]) && approx(w[1], base[1]), "an all-zero spacing block changes nothing");
        check(aver_ui_text_measure_ex("AB A", nullptr, 20.0f, 0, 0.0f, nullptr, w) == 1 && approx(w[0], base[0]), "and neither does NULL");

        const float letter[4] = {0, 2, 0, 0};
        aver_ui_text_measure_ex("AB A", nullptr, 20.0f, 0, 0.0f, letter, w);
        check(approx(w[0], base[0] + 4 * 2), "glyph spacing adds 2 px after each of the four characters (including the space)");

        const float space[4] = {0, 0, 7, 0};
        aver_ui_text_measure_ex("AB A", nullptr, 20.0f, 0, 0.0f, space, w);
        check(approx(w[0], base[0] + 7), "space spacing adds only after the one space");

        float two[3] = {};
        aver_ui_text_measure("AAA BBB CCC", nullptr, 20.0f, AVER_UI_TEXT_FLAG_WRAP, 80.0f, two);
        check(approx(two[2], 2.0f), "the wrapped text is two lines");
        const float lineMul[4] = {1.5f, 0, 0, 0};
        float lm[3] = {};
        aver_ui_text_measure_ex("AAA BBB CCC", nullptr, 20.0f, AVER_UI_TEXT_FLAG_WRAP, 80.0f, lineMul, lm);
        check(approx(lm[1], two[1] * 1.5f, 1.5f), "the line multiplier scales the height");
        const float extra[4] = {0, 0, 0, 5};
        float le[3] = {};
        aver_ui_text_measure_ex("AAA BBB CCC", nullptr, 20.0f, AVER_UI_TEXT_FLAG_WRAP, 80.0f, extra, le);
        check(approx(le[1], two[1] + 2 * 5), "extra line pixels add per line (two lines, +10)");

        const float nan[4] = {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(), -1e9f};
        aver_ui_text_measure_ex("AB A", nullptr, 20.0f, 0, 0.0f, nan, w);
        check(std::isfinite(w[0]) && std::isfinite(w[1]) && w[0] > 0, "NaN, infinity and huge values are clamped, not propagated");

        // The same through block and fx drawing: the block reports the measured size.
        float bl[3] = {};
        aver_ui_text_block_ex(0, 0, "AB A", 0xFFFFFFFF, nullptr, 20.0f, 0, 0.0f, AVER_UI_TEXT_ALIGN_LEFT, letter, bl);
        check(approx(bl[0], base[0] + 8), "aver_ui_text_block_ex lays out with the spacing");
        aver_ui_text_block_fx_ex(0, 0, "AB A", 0xFFFFFFFF, nullptr, 20.0f, 0, 0.0f, AVER_UI_TEXT_ALIGN_LEFT, 2.0f, 0xFF000000, 0, 0, 0, 0, letter, bl);
        check(approx(bl[0], base[0] + 8), "aver_ui_text_block_fx_ex too (the outline is not counted)");
        float nb[3] = {};
        aver_ui_text_block(0, 0, "AB A", 0xFFFFFFFF, nullptr, 20.0f, 0, 0.0f, AVER_UI_TEXT_ALIGN_LEFT, nb);
        check(approx(nb[0], base[0]), "the old aver_ui_text_block is the default spacing");
    }

    if (g_failures == 0) AVER_INFO("=== all UI transform / state / spacing tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
