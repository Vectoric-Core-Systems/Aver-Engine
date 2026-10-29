// FabricUiTest -- G5 U1 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/UiGame (the PNG and the font file are written here: the fixture holds text only), then
// the files it wrote read back with the engine's own loaders. The Control family must be placed as real classes with Godot's property names, a style box held by a node must become an
// Object(StyleBox...) value, a Theme resource (the project's, a node's, an inline one) must become an .octheme the engine's parser reads, the project's stretch / theme / tooltip settings
// must become the UI.* keys, and every property must be rated in the report with its reason.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/ui/UiThemeSheet.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>
#if !defined(_WIN32)
#include <sys/wait.h>
#endif

using namespace aver;

static int g_checks = 0;
static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool near(double a, double b, double eps = 1e-4) { return std::fabs(a - b) <= eps; }

static int runCapture(const std::string& cmd, std::string& out) {
    out.clear();
#if defined(_WIN32)
    const std::string wrapped = "\"" + cmd + " 2>&1\"";
    FILE* pipe = _popen(wrapped.c_str(), "r");
#else
    const std::string wrapped = cmd + " 2>&1";
    FILE* pipe = popen(wrapped.c_str(), "r");
#endif
    if (!pipe) return -1;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, pipe)) out += buf;
#if defined(_WIN32)
    return _pclose(pipe);
#else
    const int status = pclose(pipe);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

static std::string slurp(const std::string& path) {
    std::string s;
    readFileText(path, s);
    return s;
}

static bool put(const std::string& path, const std::vector<u8>& bytes) {
    std::filesystem::create_directories(std::filesystem::path(path).parent_path());
    return writeFileBytes(path, bytes.data(), bytes.size());
}

// A PNG signature and IHDR for a 12 x 10 image: the converter reads a texture's size from the header.
static std::vector<u8> png12x10() {
    return {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 13, 'I', 'H', 'D', 'R', 0, 0, 0, 12, 0, 0, 0, 10, 8, 6, 0, 0, 0, 0, 0, 0, 0};
}

static int find(const fmt::OcWorldData& w, std::initializer_list<const char*> path) {
    int parent = -1;
    for (const char* name : path) {
        int found = -1;
        for (usize i = 0; i < w.placements.size(); ++i)
            if (w.placements[i].parent == parent && w.placements[i].name == name) { found = static_cast<int>(i); break; }
        if (found < 0) return -1;
        parent = found;
    }
    return parent;
}

static const std::string* propOf(const fmt::OcWorldData& w, int idx, const char* key) {
    if (idx < 0) return nullptr;
    for (const fmt::OcPlacedComponent& c : w.placements[static_cast<usize>(idx)].components)
        if (c.type == "Props") return c.find(key);
    return nullptr;
}

static std::string classOf(const fmt::OcWorldData& w, int idx) {
    return idx < 0 ? std::string() : w.placements[static_cast<usize>(idx)].className;
}

// The decoded text of a Props value (the level percent-encodes spaces and quotes).
static std::string decoded(const std::string* v) {
    if (!v) return "(none)";
    std::string o;
    for (usize i = 0; i < v->size(); ++i) {
        if ((*v)[i] == '%' && i + 2 < v->size() + 0 && std::isxdigit(static_cast<unsigned char>((*v)[i + 1])) && std::isxdigit(static_cast<unsigned char>((*v)[i + 2]))) {
            o += static_cast<char>(std::stoi(v->substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            o += (*v)[i];
        }
    }
    return o;
}

int main() {
    AVER_INFO("FabricUiTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string fixture = std::string(AVER_FABRIC_DATA) + "/UiGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(fixture + "/project.godot", ec), "the Godot fixture exists: " + fixture);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-ui-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    const std::string src = (outRoot / "UiGame").string();
    std::filesystem::copy(fixture, src, std::filesystem::copy_options::recursive, ec);
    check(!ec, "the fixture was copied");
    check(put(src + "/ui/panel9.png", png12x10()) && put(src + "/ui/fonts/ui.ttf", std::vector<u8>{0, 1, 0, 0, 7, 7}), "the PNG and the font file were written");
    const std::string out1 = (outRoot / "out").string();

    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + src + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string project = out1 + "/UiGame";
    const std::string content = project + "/Content";

    // ---- the project ----------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- project --");
    fmt::ProjectDesc proj;
    std::string err;
    check(fmt::loadOcproject(project + "/UiGame.ocproject", proj, &err), "the project loads: " + err);
    check(proj.uiStretch == "canvas_items" && proj.uiAspect == "expand" && proj.uiStretchScaleMode == "integer", "UI.STRETCH canvas_items, UI.ASPECT expand, UI.STRETCHSCALEMODE integer");
    check(near(proj.uiStretchScale, 1.5) && proj.uiBaseW == 1280 && proj.uiBaseH == 720, "UI.STRETCHSCALE 1.5 and UI.BASESIZE 1280 720 (the viewport size)");
    check(near(proj.uiTooltipDelay, 0.8), "UI.TOOLTIPDELAY 0.8");
    check(proj.uiTheme == "Themes/theme.octheme", "UI.THEME names the project theme: " + proj.uiTheme);
    const std::string projText = slurp(project + "/UiGame.ocproject");
    fmt::ProjectDesc again;
    fmt::parseOcproject(fmt::writeOcproject(proj, projText), again, &err);
    check(again.uiTheme == proj.uiTheme && again.uiStretch == proj.uiStretch && again.uiBaseW == 1280 && near(again.uiTooltipDelay, 0.8), "the keys survive a save of the manifest");

    // ---- the theme files ------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- themes --");
    ui::UiThemeSheet th;
    const std::string themeText = slurp(content + "/Themes/theme.octheme");
    check(ui::uiThemeParse(themeText, th, &err), "theme.octheme parses with the engine's parser: " + err);
    check(th.defaultFont == "Fonts/ui/fonts/ui.ttf" && th.defaultFontSize == 20, "the default font (copied under Content/Fonts/) and its size");
    check(std::filesystem::exists(content + "/Fonts/ui/fonts/ui.ttf", ec), "the font file was copied");
    check(th.variations.count("HeaderPanel") && th.variations["HeaderPanel"] == "PanelContainer", "the type variation names its base");
    const auto colour = th.types.find("Control");
    check(colour != th.types.end() && colour->second.colors.count("font_color") && colour->second.colors["font_color"] == 0xFFB3E6E6u, "Control/colors/font_color is packed straight 0xAABBGGRR");
    check(colour != th.types.end() && colour->second.constants["tint"] == 3 && colour->second.fontSizes["font_size"] == 24, "constants and font sizes");
    check(th.types.count("BoxContainer") && th.types["BoxContainer"].constants["separation"] == 7, "a constant of a container type");
    const auto panel = th.types.find("Panel");
    bool flatOk = false;
    if (panel != th.types.end() && panel->second.styleBoxes.count("panel")) {
        const ui::UiStyleBox& b = panel->second.styleBoxes["panel"].box;
        flatOk = b.kind == static_cast<i32>(ui::UiStyleBoxKind::Flat) && near(b.borderL, 2) && near(b.borderB, 2) && near(b.radiusTL, 8) && near(b.radiusBR, 4) && b.cornerDetail == 6 &&
                 near(b.contentL, 10) && near(b.contentT, 6) && near(b.shadowSize, 6) && near(b.shadowOffX, 2) && near(b.shadowOffY, 3) && near(b.aaSize, 0.75) && b.bgColor == 0xFF331F1Au;
    }
    check(flatOk, "a StyleBoxFlat in the theme keeps its borders, radii, detail, content margins, shadow and bg colour");
    const auto frame = th.types.find("PanelContainer");
    bool texOk = false;
    if (frame != th.types.end() && frame->second.styleBoxes.count("panel")) {
        const ui::UiThemeStyleBox& s = frame->second.styleBoxes["panel"];
        texOk = s.box.kind == static_cast<i32>(ui::UiStyleBoxKind::Texture) && s.texturePath == "Textures/ui/panel9.png" && near(s.box.texW, 12) && near(s.box.texH, 10) && near(s.box.patchL, 4) &&
                s.box.axisH == 1 && s.box.modulate == 0xFFCCE6FFu;
    }
    check(texOk, "a StyleBoxTexture keeps the texture (copied under Textures/), its size read from the PNG, the 9-patch margins, the tile axis and the modulate");
    check(std::filesystem::exists(content + "/Textures/ui/panel9.png", ec), "the 9-patch texture was copied");
    const auto focus = th.types["Control"].styleBoxes.find("focus");
    check(focus != th.types["Control"].styleBoxes.end() && focus->second.box.kind == static_cast<i32>(ui::UiStyleBoxKind::Empty), "a StyleBoxEmpty");
    check(th.types.count("HeaderPanel") && th.types["HeaderPanel"].styleBoxes.count("panel") && th.types["HeaderPanel"].colors.count("font_color"), "items of the variation type");

    ui::UiThemeSheet nodeTheme;
    check(ui::uiThemeParse(slurp(content + "/Themes/node_theme.octheme"), nodeTheme, &err) && nodeTheme.types["Panel"].styleBoxes.count("panel"), "a theme a node names is written as its own file");
    ui::UiThemeSheet inlineTheme;
    check(ui::uiThemeParse(slurp(content + "/Themes/main_Theme_inline.octheme"), inlineTheme, &err) && inlineTheme.types["Label"].constants["line_spacing"] == 9 &&
              inlineTheme.types["Label"].colors["font_color"] == 0xFF00FF00u,
          "a Theme sub-resource of a scene is written under the scene's name");

    // ---- the level --------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- level --");
    fmt::OcWorldData w;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &err), "the level loads: " + err);
    const int root = find(w, {"Root"}), hud = find(w, {"Root", "Hud"}), row = find(w, {"Root", "Hud", "Row"}), grid = find(w, {"Root", "Hud", "Row", "Grid"}),
              cell = find(w, {"Root", "Hud", "Row", "Grid", "Cell"}), rule = find(w, {"Root", "Hud", "Row", "Rule"}), margin = find(w, {"Root", "Margin"}),
              scroll = find(w, {"Root", "Margin", "Scroll"}), center = find(w, {"Root", "Margin", "Scroll", "Center"}), cap = find(w, {"Root", "Margin", "Scroll", "Center", "Cap"}),
              go = find(w, {"Root", "Go"});
    check(root >= 0 && hud >= 0 && row >= 0 && grid >= 0 && cell >= 0 && rule >= 0 && margin >= 0 && scroll >= 0 && center >= 0 && cap >= 0 && go >= 0, "every node is placed");
    check(classOf(w, root) == "Control" && classOf(w, hud) == "PanelContainer" && classOf(w, row) == "HBoxContainer" && classOf(w, grid) == "GridContainer" && classOf(w, rule) == "Panel" &&
              classOf(w, margin) == "MarginContainer" && classOf(w, scroll) == "ScrollContainer" && classOf(w, center) == "CenterContainer",
          "the Control family is placed as its own classes");
    check(classOf(w, cap) == "Label" && classOf(w, go) == "Button", "a Label and a Button are placed as their own classes (G5 U2)");
    check(!propOf(w, cap, "_class") && !propOf(w, go, "_class"), "and need no carrier metadata");

    const std::string* anchorR = propOf(w, root, "anchor_right");
    const std::string* preset = propOf(w, root, "anchors_preset");
    check(anchorR && near(std::atof(anchorR->c_str()), 1.0) && preset && std::atoi(preset->c_str()) == 15, "the full-rect anchors are kept");
    const std::string* theme = propOf(w, root, "theme");
    check(theme && decoded(theme).find("Resource(\"Themes/node_theme.octheme\")") != std::string::npos, "a node's theme is a Resource() of the .octheme: " + decoded(theme));
    const std::string* tip = propOf(w, root, "tooltip_text");
    const std::string* filter = propOf(w, root, "mouse_filter");
    check(tip && decoded(tip) == "\"the whole screen\"" && filter && std::atoi(filter->c_str()) == 1, "tooltip_text and mouse_filter Pass");
    const std::string* variation = propOf(w, hud, "theme_type_variation");
    check(variation && decoded(variation).find("HeaderPanel") != std::string::npos, "theme_type_variation");
    const std::string* box = propOf(w, hud, "theme_override_styles/panel");
    const std::string boxText = decoded(box);
    check(box && boxText.rfind("Object(StyleBoxFlat", 0) == 0 && boxText.find("\"bg_color\": Color(0.9, 0.2, 0.1, 0.8)") != std::string::npos && boxText.find("\"border_width_bottom\": 3") != std::string::npos &&
              boxText.find("\"corner_radius_top_left\": 12") != std::string::npos && boxText.find("\"skew\": Vector2(4, 0)") != std::string::npos && boxText.find("\"draw_center\": false") != std::string::npos,
          "a node's style box override is Object(StyleBoxFlat, ...) with Godot's names: " + boxText);
    check(box && boxText.find("bogus_stylebox_option") == std::string::npos, "an unknown style box property is dropped");
    const std::string* lineBox = propOf(w, rule, "theme_override_styles/panel");
    check(lineBox && decoded(lineBox).rfind("Object(StyleBoxLine", 0) == 0 && decoded(lineBox).find("\"vertical\": true") != std::string::npos, "StyleBoxLine");
    check(propOf(w, hud, "theme_override_colors/font_color") && propOf(w, hud, "theme_override_constants/margin") && propOf(w, row, "theme_override_constants/separation"), "colour and constant overrides are kept under their scene names");
    check(propOf(w, cell, "focus_mode") && std::atoi(propOf(w, cell, "focus_mode")->c_str()) == 2 && propOf(w, cell, "focus_neighbor_right") && propOf(w, cell, "focus_next"), "focus mode and neighbours");
    check(propOf(w, cell, "size_flags_stretch_ratio") && near(std::atof(propOf(w, cell, "size_flags_stretch_ratio")->c_str()), 2.0) && propOf(w, cell, "custom_minimum_size"), "size flags, stretch ratio and the minimum size");
    check(propOf(w, grid, "columns") && std::atoi(propOf(w, grid, "columns")->c_str()) == 3 && propOf(w, row, "alignment") && std::atoi(propOf(w, row, "alignment")->c_str()) == 1, "container properties");
    check(propOf(w, scroll, "horizontal_scroll_mode") && propOf(w, scroll, "scroll_vertical") && propOf(w, center, "use_top_left"), "scroll and centre properties");
    check(propOf(w, rule, "mouse_filter") && std::atoi(propOf(w, rule, "mouse_filter")->c_str()) == 2 && propOf(w, rule, "clip_contents"), "mouse_filter Ignore and clip_contents");
    check(propOf(w, cap, "text") && propOf(w, go, "text"), "the carriers keep their own properties (text)");

    // ---- the report -------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- the report --");
    const std::string jsonl = slurp(out1 + "/FabricReport.jsonl");
    const auto has = [&](const std::string& a, const std::string& b, const std::string& c) {
        std::istringstream is(jsonl);
        std::string line;
        while (std::getline(is, line))
            if (line.find(a) != std::string::npos && line.find(b) != std::string::npos && line.find(c) != std::string::npos) return true;
        return false;
    };
    check(has("display/window/stretch/mode", "\"rating\":\"converted\"", "UI.STRETCH canvas_items"), "the stretch mode is rated converted");
    check(has("display/window/stretch/scale", "\"rating\":\"converted\"", "UI.STRETCHSCALE"), "the stretch scale is rated converted");
    check(has("gui/theme/custom", "\"rating\":\"converted\"", "UI.THEME Themes/theme.octheme"), "the project theme is rated converted");
    check(has("gui/timers/tooltip_delay_sec", "\"rating\":\"converted\"", "UI.TOOLTIPDELAY"), "the tooltip delay is rated converted");
    check(has("\"type\":\"Theme\"", "\"rating\":\"converted\"", "Themes/theme.octheme"), "the Theme resource is rated converted");
    check(has("\"type\":\"StyleBoxFlat\"", "\"property\":\"skew\"", "\"rating\":\"approximated\""), "skew is approximated, with its reason");
    check(has("\"type\":\"StyleBoxFlat\"", "\"property\":\"shadow_size\"", "\"rating\":\"approximated\""), "the shadow is approximated, with its reason");
    check(has("\"type\":\"StyleBoxFlat\"", "\"property\":\"bogus_stylebox_option\"", "\"rating\":\"unsupported\""), "an unknown style box property is unsupported");
    check(has("\"type\":\"FontFile\"", "\"rating\":\"converted\"", "font set"), "the font file is rated converted: it becomes a family of the engine's font set");
    check(slurp(content + "/UI/Fonts.ocfonts").find("FAMILY ui\nFACE Fonts/ui/fonts/ui.ttf\n") != std::string::npos, "UI/Fonts.ocfonts names the font a family after its file");
    check(has("\"node\":\".\"", "\"property\":\"anchor_right\"", "\"rating\":\"converted\""), "anchors are rated converted");
    check(has("\"node\":\".\"", "\"property\":\"tooltip_text\"", "\"rating\":\"converted\""), "tooltip_text is rated converted");
    check(has("\"node\":\"Hud\"", "\"property\":\"theme_override_styles/panel\"", "\"rating\":\"converted\""), "a theme override is rated converted");
    check(has("\"node\":\"Go\"", "\"property\":\"rotation\"", "\"rating\":\"approximated\""), "rotation is approximated: styles are drawn upright");
    check(has("Cap", "\"property\":\"texture_filter\"", "\"rating\":\"unsupported\""), "texture_filter is unsupported");
    check(has("Cap", "\"property\":\"text\"", "Aver.Fabric Label reads it"), "a Label's text is read by Aver.Fabric's Label and rated converted");
    check(has("Cap", "\"property\":\"horizontal_alignment\"", "\"rating\":\"converted\""), "and so is its alignment");
    check(has("Margin/Scroll", "\"type\":\"ScrollContainer\"", "\"rating\":\"approximated\""), "the ScrollContainer is approximated: no scroll bars yet");

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    if (!g_failures) std::filesystem::remove_all(outRoot, ec);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
