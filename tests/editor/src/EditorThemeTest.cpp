// The editor theme as data: defaults, a file override, a malformed file, and the contrast of the
// default palette. ImGui-free -- EditorTheme.hpp is plain data on purpose.
#include "../../../sandbox/src/EditorTheme.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(f32 a, f32 b, f32 eps = 0.003f) { return std::fabs(a - b) <= eps; }
static bool same(const Rgba& a, const Rgba& b, f32 eps = 0.003f) {
    return approx(a.r, b.r, eps) && approx(a.g, b.g, eps) && approx(a.b, b.b, eps) && approx(a.a, b.a, eps);
}

static void writeFile(const std::filesystem::path& p, const std::string& text) {
    std::ofstream(p, std::ios::binary) << text;
}

int main() {
    const EditorTheme def;

    // ---- defaults ---------------------------------------------------------------------------------
    {
        check(approx(def.accent.r, 242.0f / 255) && approx(def.accent.g, 101.0f / 255) && approx(def.accent.b, 34.0f / 255),
              "the default accent is Aver orange (242,101,34)");
        const Rgba* ladder[] = {&def.sunken, &def.panel, &def.raised, &def.item, &def.line, &def.text, &def.textDim};
        bool neutral = true;
        for (const Rgba* c : ladder) neutral = neutral && c->r == c->g && c->g == c->b;
        check(neutral, "the neutral ladder has equal R, G and B -- no tint");
        check(def.sunken.r < def.panel.r && def.panel.r < def.raised.r && def.raised.r < def.item.r,
              "and it ascends: sunken < panel < raised < item");
        check(approx(itemHot(def).r, def.item.r + (1 - def.item.r) * 0.07f) && itemHot(def).r == itemHot(def).b,
              "hover is a neutral 7% lift towards white, not a colour");
        check(approx(selection(def).a, 0.25f) && selection(def).r == def.accent.r,
              "selection is the accent at 25% alpha");
        check(def.fontHeader == 18.0f && def.fontBody == 16.0f && def.fontSmall == 13.5f,
              "the type scale is 18 / 16 / 13.5");
    }

    // ---- the shipped file equals the defaults -----------------------------------------------------
    {
        EditorTheme t;
        std::string why;
        const bool ok = loadThemeFile(AVER_THEME_FILE, t, &why);
        check(ok && why.empty(), "sandbox/editor-theme.json loads with no complaint");
        check(same(t.panel, def.panel) && same(t.raised, def.raised) && same(t.item, def.item) &&
                  same(t.sunken, def.sunken) && same(t.line, def.line) && same(t.text, def.text) &&
                  same(t.textDim, def.textDim) && same(t.accent, def.accent) && same(t.popup, def.popup) &&
                  same(t.overlayBorder, def.overlayBorder),
              "the shipped colours equal the built-in defaults");
        check(approx(t.hoverLift, def.hoverLift) && approx(t.selectionAlpha, def.selectionAlpha) &&
                  approx(t.overlayAlpha, def.overlayAlpha) && approx(t.rounding, def.rounding) &&
                  approx(t.fontHeader, def.fontHeader) && approx(t.fontSmall, def.fontSmall) &&
                  approx(t.scrollbarSize, def.scrollbarSize),
              "and so do the shipped metrics and font sizes");
    }

    // ---- a file override --------------------------------------------------------------------------
    {
        EditorTheme t;
        std::string why;
        const bool ok = applyThemeJson(
            R"({"colors":{"panel":"#102030","accent":"#00FF0080"},"metrics":{"rounding":9},"fonts":{"body":20}})", t, &why);
        check(ok && why.empty(), "an override document applies");
        check(same(t.panel, Rgba{16 / 255.0f, 32 / 255.0f, 48 / 255.0f, 1}), "the overridden colour changes");
        check(approx(t.accent.a, 128 / 255.0f) && approx(t.accent.g, 1.0f), "#rrggbbaa carries an alpha");
        check(t.rounding == 9.0f && t.fontBody == 20.0f, "metrics and fonts override");
        check(same(t.text, def.text) && t.fontHeader == def.fontHeader && t.scrollbarSize == def.scrollbarSize,
              "everything the file does not mention keeps its default");

        // The same through a real file on disk.
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver_theme_test";
        std::filesystem::create_directories(dir);
        writeFile(dir / "editor-theme.json", R"({"colors":{"text":"#FFFFFF"}})");
        EditorTheme f;
        std::string fwhy;
        check(loadThemeFile((dir / "editor-theme.json").string(), f, &fwhy) && same(f.text, Rgba{1, 1, 1, 1}),
              "a theme file on disk is read");
        reloadEditorTheme(dir.string());
        check(same(editorTheme().text, Rgba{1, 1, 1, 1}), "reloadEditorTheme makes it the current theme");

        // ---- malformed: fall back to the defaults ----
        writeFile(dir / "editor-theme.json", R"({"colors": {"panel": "#102030",)");
        EditorTheme m;
        m.panel = Rgba{1, 0, 0, 1};
        std::string mwhy;
        check(!loadThemeFile((dir / "editor-theme.json").string(), m, &mwhy) && !mwhy.empty(),
              "a malformed file is rejected, with a reason");
        check(same(m.panel, def.panel), "and the result is the defaults");
        reloadEditorTheme(dir.string());
        check(same(editorTheme().panel, def.panel) && same(editorTheme().text, def.text),
              "reloadEditorTheme on a malformed file leaves the built-in theme in force");

        writeFile(dir / "editor-theme.json", "[1,2,3]");
        check(!loadThemeFile((dir / "editor-theme.json").string(), m, nullptr), "JSON that is not an object is rejected");

        // ---- absent ----
        std::filesystem::remove(dir / "editor-theme.json");
        reloadEditorTheme(dir.string());
        check(same(editorTheme().panel, def.panel), "a missing file gives the defaults");
        std::filesystem::remove_all(dir);

        // ---- a bad entry costs only itself ----
        EditorTheme b;
        std::string bwhy;
        const bool bok = applyThemeJson(R"({"colors":{"panel":"red","text":"#FFFFFF"},"metrics":{"rounding":-3,"scrollbarSize":20}})", b, &bwhy);
        check(bok && !bwhy.empty(), "bad entries are reported");
        check(same(b.panel, def.panel) && b.rounding == def.rounding, "a bad colour and an out-of-range number keep their defaults");
        check(same(b.text, Rgba{1, 1, 1, 1}) && b.scrollbarSize == 20.0f, "while the good entries beside them apply");
    }

    // ---- contrast of the default table (WCAG 2.x) -------------------------------------------------
    {
        const f64 dimPanel = contrastRatio(def.textDim, def.panel);
        const f64 textPanel = contrastRatio(def.text, def.panel);
        const f64 dimRaised = contrastRatio(def.textDim, def.raised);
        const f64 dimItem = contrastRatio(def.textDim, def.item);
        AVER_INFO("  contrast: textDim/panel {:.2f}, text/panel {:.2f}, textDim/raised {:.2f}, textDim/item {:.2f}",
                  dimPanel, textPanel, dimRaised, dimItem);
        check(dimPanel >= 4.5, "textDim on panel is at least 4.5:1");
        check(textPanel >= 4.5, "text on panel is at least 4.5:1");
        check(dimRaised >= 4.5 && dimItem >= 4.5, "textDim also holds 4.5:1 on raised and item surfaces");
        check(contrastRatio(def.accent, def.panel) >= 3.0, "the accent holds 3:1 (non-text) on panel");
        check(std::fabs(contrastRatio(Rgba{1, 1, 1, 1}, Rgba{0, 0, 0, 1}) - 21.0) < 1e-6,
              "the contrast formula gives 21:1 for white on black");
    }

    if (g_failures == 0) AVER_INFO("=== all {} editor theme checks passed ===", g_checks);
    else                 AVER_ERROR("=== {} of {} editor theme check(s) FAILED ===", g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
