// The text engine through the C ABI: the host's font callbacks, the font set, measuring and drawing, the
// atlas and its counters, the language hook, the widget text size, and the legacy aver_ui_text path (which
// now decodes UTF-8). Links the DLL the C# side loads. No GPU, no window.
#include "TestFont.hpp"

#include "aver/core/Log.hpp"
#include "aver/ui/UiDrawList.hpp"
#include "aver/ui/UiFont.hpp"
#include "aver/ui/UiGlyphAtlas.hpp"
#include "aver/ui/ui_abi.h"
#include "aver/ui/ui_widget_abi.h"

#include <cmath>
#include <cstring>
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

// ---- the host's callbacks ----------------------------------------------------------------------

static std::map<std::string, std::vector<u8>> g_files;
static int g_opened = 0, g_closed = 0, g_lastKind = -1;
static const char* g_lang = "ja";

static int32_t openCb(int32_t kind, const char* name, const uint8_t** data, uint64_t* size, void** token) {
    g_lastKind = kind;
    const auto it = g_files.find(name);
    if (it == g_files.end()) return 0;
    *data = it->second.data();
    *size = it->second.size();
    *token = new int(1);
    ++g_opened;
    return 1;
}

static void closeCb(void* token) {
    delete static_cast<int*>(token);
    ++g_closed;
}

static const char* langCb() { return g_lang; }

static int32_t fakeResolve(const char* raw, char* out, int32_t cap) {
    const std::string ref(raw, std::strcspn(raw, "="));
    if (ref != "@UI:short") return -1;
    const std::string text = "Hi";
    if (out && cap > 0) std::memcpy(out, text.c_str(), text.size() + 1);
    (void)cap;
    return static_cast<int32_t>(text.size());
}

static std::vector<u8> cjk(int advance) {
    testfont::Spec s;
    s.chars[0x4E2D] = {advance, 100, 0, advance - 100, 800};
    return testfont::buildFont(s);
}

static ui::UiFont g_font;   // lent to the ABI for the whole run, so it must outlive every block

int main() {
    AVER_INFO("=== the baked-font path decodes UTF-8 ===");
    {
        ui::UiFont& font = g_font;
        std::string err;
        const char* doc =
            "OCFONT 1\nNAME Test\nSIZE 16\nATLAS none.png\nMETRICS 12 -4 16\n"
            "GLYPH 65 0 0 0.5 0.5 8 8 0 0 10\nGLYPH 233 0 0 0.5 0.5 8 8 0 0 12\n";
        check(ui::parseOcfont(doc, font, &err), "the test font parses: " + err);
        aver_ui_begin_frame(0, 0, 800, 600);
        check(aver_ui_has_font() == 0, "no font yet");
        aver_ui_set_font(&font);
        check(aver_ui_has_font() == 1, "a lent bitmap font counts");
        check(aver_ui_text_width("\xC3\xA9") == 12.0f && aver_ui_text_width("A\xC3\xA9") == 22.0f,
              "an e-acute (two UTF-8 bytes) is ONE glyph of advance 12, not two lookups of the bytes");
        const float x = aver_ui_text(5.0f, 20.0f, "\xC3\xA9", 0xFFFFFFFFu);
        check(x == 17.0f && aver_ui_vertex_count() == 4, "and draws one quad and advances 12");
        float whl[3] = {};
        check(aver_ui_text_measure("A\xC3\xA9", nullptr, 16.0f, 0, 0.0f, whl) == 1 && approx(whl[0], 22.0f) && approx(whl[1], 16.0f) && approx(whl[2], 1.0f),
              "aver_ui_text_measure falls back to the baked font (22 x 16, one line)");

        // the engine
        testfont::Spec latin = testfont::simpleSpec();
        latin.chars['V'] = {620, 40, 0, 580, 700};
        g_files["latin.ttf"] = testfont::buildFont(latin);
        g_files["ja.ttf"] = cjk(1000);
        g_files["zh.ttf"] = cjk(900);

        aver_ui_text_set_loader(&openCb, &closeCb);
        const char* set =
            "OCFONTS 1\nDEFAULT Body\nFAMILY Body\nFACE latin.ttf\nFACE ja.ttf lang=ja\nFACE zh.ttf lang=zh-Hans\nBITMAP legacy\n";
        check(aver_ui_text_load_fontset(set) == 1 && std::string(aver_ui_text_error()).empty(), "the font set loads through the host's callbacks");
        check(g_opened == 3 && g_lastKind == AVER_UI_FONT_CONTENT, "three project faces were opened, as Content");
        check(aver_ui_text_load_fontset("not a font set") == 0 && !std::string(aver_ui_text_error()).empty(), "a bad document fails with an error");
        check(aver_ui_text_load_fontset(nullptr) == 0, "and so does NULL");
        check(aver_ui_text_load_fontset(set) == 1, "reloading the good one works");

        check(aver_ui_has_font() == 1, "the engine counts as a font");
        check(approx(aver_ui_text_width("AB"), (600 + 610) * 16.0f / 1000.0f), "aver_ui_text_width now measures the default family at 16 px");
        float m[3] = {};
        aver_ui_font_metrics(m);
        check(approx(m[0], 13.0f) && approx(m[1], -3.0f) && approx(m[2], 16.0f), "aver_ui_font_metrics: ascent 13, descent -3, line 16 at 16 px");
        aver_ui_begin_frame(0, 0, 800, 600);
        const float end = aver_ui_text(10.0f, 40.0f, "AB", 0xFFFFFFFFu);
        check(approx(end, 10.0f + 19.36f), "aver_ui_text returns the pen after the text");
        check(aver_ui_vertex_count() == 8, "and drew two glyph quads");
        check(aver_ui_text_width("A\xC3\xA9") > 9.0f, "the lent bitmap font is the last link of the chain: e-acute still measures");
    }

    AVER_INFO("=== measure, block, prewarm, glyph queries ===");
    {
        float whl[3] = {};
        check(aver_ui_text_measure("AB", nullptr, 20.0f, 0, 0.0f, whl) == 1 && approx(whl[0], 24.2f) && approx(whl[1], 20.0f) && approx(whl[2], 1.0f),
              "measure AB at 20 px: 24.2 x 20, one line");
        check(aver_ui_text_measure("AAA BBB CCC", "", 20.0f, AVER_UI_TEXT_FLAG_WRAP, 80.0f, whl) == 1 && approx(whl[2], 2.0f) && approx(whl[1], 40.0f),
              "with FLAG_WRAP and a width it wraps to two lines");
        check(aver_ui_text_measure("AAA BBB CCC", "", 20.0f, 0, 80.0f, whl) == 1 && approx(whl[2], 1.0f), "without the flag a width does not wrap");
        check(aver_ui_text_measure("AAAAAAAA", nullptr, 20.0f, AVER_UI_TEXT_FLAG_ELLIPSIS, 60.0f, whl) == 1 && whl[0] <= 60.01f, "FLAG_ELLIPSIS cuts a wide line to fit");
        check(aver_ui_text_measure("A<b>A</b>", nullptr, 20.0f, AVER_UI_TEXT_FLAG_RICH, 0.0f, whl) == 1 && approx(whl[0], 24.0f), "FLAG_RICH strips tags (bold falls back to the regular face here)");
        check(aver_ui_text_measure(nullptr, nullptr, 20.0f, 0, 0.0f, whl) == 0, "a NULL string measures nothing");

        aver_ui_set_text_resolver(&fakeResolve, nullptr);
        check(aver_ui_text_measure("@UI:short", nullptr, 16.0f, 0, 0.0f, whl) == 1 && approx(whl[0], (670 + 580) * 16.0f / 1000.0f),
              "localization references resolve before measuring (Hi)");
        aver_ui_set_text_resolver(nullptr, nullptr);

        check(aver_ui_text_has_glyph(nullptr, 'A') == 1 && aver_ui_text_has_glyph("Body", 'A') == 1 && aver_ui_text_has_glyph("NoSuch", 'A') == 1,
              "has_glyph: the default family for NULL or an unknown name");
        check(aver_ui_text_has_glyph(nullptr, 0x1F600) == 0 && aver_ui_text_has_glyph(nullptr, 0x4E2D) == 1, "and false for a character no face carries");

        aver_ui_begin_frame(0, 0, 800, 600);
        const float h = aver_ui_text_block(10.0f, 20.0f, "AAA BBB CCC", 0xFFFFFFFFu, nullptr, 20.0f, AVER_UI_TEXT_FLAG_WRAP, 80.0f,
                                           AVER_UI_TEXT_ALIGN_LEFT, whl);
        check(approx(h, 40.0f) && approx(whl[0], 78.6f) && approx(whl[1], 40.0f) && approx(whl[2], 2.0f), "aver_ui_text_block returns the height and reports the size");
        const auto* list = static_cast<const ui::UiDrawList*>(aver_ui_draw_list());
        check(list != nullptr && aver_ui_command_count() >= 1, "it drew into the draw list");
        bool token = false;
        if (list)
            for (const ui::UiDrawCmd& c : list->commands(ui::UiLayer::Content))
                if (c.texture == (ui::kUiAtlasTokenBit | 0)) token = true;
        check(token, "whose commands reference atlas page 0 by its token");
        const float leftX = list ? list->vertices()[0].x : 0.0f;

        aver_ui_begin_frame(0, 0, 800, 600);
        aver_ui_text_block(10.0f, 20.0f, "AB", 0xFFFFFFFFu, nullptr, 20.0f, 0, 100.0f, AVER_UI_TEXT_ALIGN_RIGHT, nullptr);
        list = static_cast<const ui::UiDrawList*>(aver_ui_draw_list());
        check(list && list->vertices()[0].x > leftX + 50.0f, "AVER_UI_TEXT_ALIGN_RIGHT aligns the block to the right of the width");

        aver_ui_begin_frame(0, 0, 800, 600);
        aver_ui_text_prewarm("prewarm me please", nullptr, 18.0f, 0);
        float st[12] = {};
        aver_ui_text_stats(st);
        check(st[1] > 0 && st[3] > 0 && st[7] > 0 && st[10] >= 3, "stats: glyphs resident, rasterised, layout misses, three faces open");
        check(st[0] >= 1, "and at least one atlas page");
        aver_ui_text_stats(nullptr);
    }

    AVER_INFO("=== the atlas and its configuration ===");
    {
        const auto* atlas = static_cast<const ui::UiGlyphAtlas*>(aver_ui_text_atlas());
        check(atlas != nullptr && atlas->pageSize() == 512 && atlas->config().maxPages == 16, "the atlas is lent by an opaque pointer: 512 pixel pages, at most 16");
        aver_ui_text_configure(256, 4, 100);
        check(atlas->pageSize() == 256 && atlas->config().maxPages == 4, "aver_ui_text_configure changes the geometry");
        aver_ui_text_configure(0, -1, 0);
        check(atlas->pageSize() == 256 && atlas->config().maxPages == 4, "values <= 0 keep the current setting");
        aver_ui_text_configure(512, 16, 256);
        check(atlas->pageSize() == 512 && atlas->config().maxPages == 16, "and back");
    }

    AVER_INFO("=== the language follows the host ===");
    {
        float ja[3] = {}, zh[3] = {};
        aver_ui_text_set_language_fn(&langCb);
        g_lang = "ja";
        aver_ui_begin_frame(0, 0, 800, 600);
        aver_ui_text_measure("\xE4\xB8\xAD", nullptr, 20.0f, 0, 0.0f, ja);
        g_lang = "zh-CN";
        aver_ui_begin_frame(0, 0, 800, 600);
        aver_ui_text_measure("\xE4\xB8\xAD", nullptr, 20.0f, 0, 0.0f, zh);
        check(approx(ja[0], 20.0f) && approx(zh[0], 18.0f), "a Han character takes the face of the language the host reports each frame (ja 20, zh-CN 18)");
        aver_ui_text_set_language("ja");
        aver_ui_text_measure("\xE4\xB8\xAD", nullptr, 20.0f, 0, 0.0f, ja);
        check(approx(ja[0], 20.0f), "aver_ui_text_set_language sets it directly");
        aver_ui_text_set_language_fn(nullptr);
    }

    AVER_INFO("=== widgets: styling by property, text size ===");
    {
        const uint32_t root = aver_ui_widget_create(AVER_UI_KIND_PANEL, 0, "Root");
        const uint32_t label = aver_ui_widget_create(AVER_UI_KIND_TEXT, root, "Label");
        aver_ui_widget_set_text(label, "AB");
        check(aver_ui_widget_set_prop(label, "fontSize", "20") == 1, "fontSize");
        check(aver_ui_widget_set_prop(label, "fontFamily", "Body") == 1 && aver_ui_widget_set_prop(label, "bold", "true") == 1 &&
                  aver_ui_widget_set_prop(label, "italic", "false") == 1 && aver_ui_widget_set_prop(label, "lineSpacing", "1.2") == 1 &&
                  aver_ui_widget_set_prop(label, "maxLines", "0") == 1 && aver_ui_widget_set_prop(label, "ellipsis", "false") == 1 &&
                  aver_ui_widget_set_prop(label, "rich", "false") == 1 && aver_ui_widget_set_prop(label, "textAlign", "start") == 1 &&
                  aver_ui_widget_set_prop(label, "textDir", "auto") == 1,
              "the text properties go through the property table");
        check(aver_ui_widget_set_prop(label, "textDir", "sideways") == 0 && aver_ui_widget_set_prop(label, "noSuchProp", "1") == 0, "bad values are refused");
        const char* got = aver_ui_widget_get_prop(label, "textAlign");
        check(got && std::string(got) == "start", "and read back");

        aver_ui_begin_frame(0, 0, 1920, 1080);
        aver_ui_widgets_frame(0.016f);
        float whl[3] = {9, 9, 9};
        check(aver_ui_widget_text_size(label, whl) == 1 && approx(whl[0], 24.2f) && approx(whl[1], 24.0f) && approx(whl[2], 1.0f),
              "aver_ui_widget_text_size: the laid-out text (lineSpacing 1.2 makes the 20 px line 24 tall)");
        check(aver_ui_widget_text_size(root, whl) == 0 && whl[0] == 0.0f, "a panel has no text size");
        check(aver_ui_widget_text_size(label, nullptr) == 1 && aver_ui_widget_text_size(99999, whl) == 0, "NULL output and unknown ids are safe");

        aver_ui_widget_set_prop(label, "lineSpacing", "1");
        aver_ui_widget_set_prop(label, "letterSpacing", "2");
        aver_ui_begin_frame(0, 0, 1920, 1080);
        aver_ui_widgets_frame(0.016f);
        aver_ui_widget_text_size(label, whl);
        check(approx(whl[0], 28.2f) && approx(whl[1], 20.0f), "letterSpacing adds 2 px per character");
        check(aver_ui_vertex_count() > 0, "the frame drew the text");
        aver_ui_widget_destroy(root);
    }

    AVER_INFO("=== clearing the loader closes every font ===");
    {
        const int opened = g_opened;
        aver_ui_text_set_loader(nullptr, nullptr);
        check(g_closed == opened && g_closed > 0, "every font the host opened was closed (" + std::to_string(g_closed) + " of " + std::to_string(opened) + ")");
        float st[12] = {};
        aver_ui_text_stats(st);
        check(st[10] == 0.0f && st[1] == 0.0f, "no faces, no resident glyphs");
        check(aver_ui_has_font() == 1 && aver_ui_text_width("A\xC3\xA9") == 22.0f, "the lent bitmap font is all that is left, and still works");
        aver_ui_set_font(nullptr);
        check(aver_ui_has_font() == 0, "and withdrawing it leaves nothing");
    }

    AVER_INFO("UiTextAbi tests done: {} failure(s)", g_failures);
    return g_failures ? 1 : 0;
}
