// FabricWidgetTest -- G5 U2 through the Godot converter: `AverAssetC fabric` on tests/fabric/data/WidgetGame (the fonts and the PNG are written here: the fixture holds text only).
// The widgets must be placed as their own classes, their properties rated (converted, approximated, unsupported) with a reason, the resources they hold (LabelSettings, FontVariation,
// SystemFont, ButtonGroup, Shortcut) must become Object(...) values or one shared Resource(), the dynamic keys (popup/item_0/text) must survive, and every converted font must be a
// family of Content/UI/Fonts.ocfonts, which the engine's font set parser reads.
#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"
#include "aver/formats/OcProject.hpp"
#include "aver/formats/OcWorld.hpp"
#include "aver/platform/FileSystem.hpp"
#include "aver/ui/UiFontSet.hpp"

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

static std::string classOf(const fmt::OcWorldData& w, int idx) { return idx < 0 ? std::string() : w.placements[static_cast<usize>(idx)].className; }

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

static bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

int main() {
    AVER_INFO("FabricWidgetTest");
    const std::filesystem::path exe = AVER_ASSETC_EXE;
    const std::string fixture = std::string(AVER_FABRIC_DATA) + "/WidgetGame";
    std::error_code ec;
    check(std::filesystem::exists(exe, ec), "AverAssetC was built: " + exe.string());
    check(std::filesystem::exists(fixture + "/project.godot", ec), "the Godot fixture exists: " + fixture);
    if (g_failures) return exitCode(ExitCode::Failed);

    const std::filesystem::path outRoot = std::filesystem::temp_directory_path() / "aver-fabric-widget-test";
    std::filesystem::remove_all(outRoot, ec);
    std::filesystem::create_directories(outRoot, ec);
    const std::string src = (outRoot / "WidgetGame").string();
    std::filesystem::copy(fixture, src, std::filesystem::copy_options::recursive, ec);
    check(!ec, "the fixture was copied");
    const std::vector<u8> fakeFont{0, 1, 0, 0, 7, 7};
    check(put(src + "/ui/icon.png", png12x10()) && put(src + "/ui/fonts/Body-Regular.ttf", fakeFont) && put(src + "/ui/fonts/Body-Bold.ttf", fakeFont), "the PNG and the two font files were written");
    const std::string out1 = (outRoot / "out").string();
    std::string log;
    const int code = runCapture("\"" + exe.string() + "\" fabric \"" + src + "\" \"" + out1 + "\" --scenes-only", log);
    check(code == 0, "the converter exits 0 (got " + std::to_string(code) + ")");
    if (code != 0) AVER_INFO("{}", log);

    const std::string project = out1 + "/WidgetGame";
    const std::string content = project + "/Content";

    // ---- the font set -----------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- fonts --");
    const std::string fonts = slurp(content + "/UI/Fonts.ocfonts");
    check(!fonts.empty(), "UI/Fonts.ocfonts was written");
    check(contains(fonts, "DEFAULT Body-Regular\n"), "gui/theme/custom_font is the default family");
    check(contains(fonts, "FAMILY Body-Regular\nFACE Fonts/ui/fonts/Body-Regular.ttf\n"), "a family per font file, named after the file");
    check(contains(fonts, "FAMILY Body-Bold\nFACE Fonts/ui/fonts/Body-Bold.ttf weight=bold\n"), "a bold file is marked as the bold face");
    check(contains(fonts, "SYSTEM auto\nBITMAP legacy\n"), "each family falls back to the operating system's fonts and the baked font");
    ui::UiFontSetDoc doc;
    std::string err;
    check(ui::UiFontSet::parse(fonts, doc, &err) && doc.families.size() == 2 && doc.defaultFamily == "Body-Regular", "the engine's font set parser reads it: " + err);
    check(std::filesystem::exists(content + "/Fonts/ui/fonts/Body-Bold.ttf", ec), "the font files were copied under Content/Fonts/");

    // ---- the level --------------------------------------------------------------------------------------------------------------------
    AVER_INFO("-- level --");
    fmt::OcWorldData w;
    check(fmt::loadOcworld(content + "/Maps/main.ocworld", w, &err), "the level loads: " + err);
    const char* names[] = {"Title", "Msg", "Start", "Check", "Opt", "Vol", "Name", "Notes", "Tabs", "Tree", "Split", "Flow", "Icon", "Radial", "Prog", "Code", "Menu", "Ask", "Sys"};
    bool all = true;
    for (const char* n : names) all = all && find(w, {"Root", n}) >= 0;
    check(all, "every node is placed");
    const auto cls = [&](const char* n) { return classOf(w, find(w, {"Root", n})); };
    check(cls("Title") == "RichTextLabel" && cls("Msg") == "Label" && cls("Start") == "Button" && cls("Check") == "CheckBox" && cls("Opt") == "OptionButton" && cls("Vol") == "HSlider" &&
              cls("Name") == "LineEdit" && cls("Notes") == "TextEdit" && cls("Tabs") == "TabContainer" && cls("Tree") == "Tree" && cls("Split") == "HSplitContainer" && cls("Flow") == "HFlowContainer" &&
              cls("Icon") == "TextureRect" && cls("Radial") == "TextureProgressBar" && cls("Prog") == "ProgressBar" && cls("Menu") == "PopupMenu" && cls("Ask") == "ConfirmationDialog",
          "the widgets are placed as their own classes");
    check(cls("Code") == "Control" && propOf(w, find(w, {"Root", "Code"}), "_class") && decoded(propOf(w, find(w, {"Root", "Code"}), "_class")).find("CodeEdit") != std::string::npos,
          "a CodeEdit has no widget: a Control carrier naming its class");
    const int title = find(w, {"Root", "Title"}), msg = find(w, {"Root", "Msg"}), start = find(w, {"Root", "Start"}), check1 = find(w, {"Root", "Check"}), opt = find(w, {"Root", "Opt"}),
              menu = find(w, {"Root", "Menu"}), notes = find(w, {"Root", "Notes"}), radial = find(w, {"Root", "Radial"}), sys = find(w, {"Root", "Sys"});
    check(decoded(propOf(w, title, "text")).find("[b]Hello[/b]") != std::string::npos && decoded(propOf(w, title, "bbcode_enabled")) == "true", "BBCode text and bbcode_enabled are kept as written");

    const std::string settings = decoded(propOf(w, msg, "label_settings"));
    check(settings.rfind("Object(LabelSettings", 0) == 0 && contains(settings, "\"font_size\": 24") && contains(settings, "\"outline_size\": 3") && contains(settings, "\"shadow_offset\": Vector2(2, 3)") &&
              contains(settings, "\"line_spacing\": 5"),
          "LabelSettings becomes Object(LabelSettings) with Godot's names: " + settings);
    check(propOf(w, msg, "autowrap_mode") && decoded(propOf(w, msg, "autowrap_mode")) == "3" && propOf(w, msg, "horizontal_alignment") && propOf(w, msg, "max_lines_visible"), "Label enums and counts");

    const std::string group1 = decoded(propOf(w, start, "button_group")), group2 = decoded(propOf(w, check1, "button_group"));
    check(group1 == group2 && contains(group1, "Resource(\"ButtonGroups/") && contains(group1, "_unpress.ocbuttongroup"), "a ButtonGroup is one shared Resource() (allow_unpress in the name): " + group1);
    const std::string shortcut = decoded(propOf(w, start, "shortcut"));
    check(shortcut.rfind("Object(Shortcut", 0) == 0 && contains(shortcut, "Object(InputEventKey") && contains(shortcut, "\"keycode\": 4194336") && contains(shortcut, "\"ctrl_pressed\": true"), "a Shortcut keeps its input events: " + shortcut);
    const std::string variation = decoded(propOf(w, start, "theme_override_fonts/font"));
    check(variation.rfind("Object(FontVariation", 0) == 0 && contains(variation, "\"base_font\": Resource(\"Fonts/ui/fonts/Body-Bold.ttf\")") && contains(variation, "\"variation_embolden\": 0.8"),
          "a FontVariation keeps its base font and embolden: " + variation);
    const std::string system = decoded(propOf(w, sys, "theme_override_fonts/font"));
    check(system.rfind("Object(SystemFont", 0) == 0 && contains(system, "\"font_weight\": 700"), "a SystemFont keeps its names and weight: " + system);

    check(propOf(w, opt, "popup/item_0/text") && decoded(propOf(w, opt, "popup/item_0/text")) == "\"Easy\"" && propOf(w, opt, "popup/item_1/id") && propOf(w, opt, "selected") && propOf(w, opt, "item_count"),
          "an OptionButton's popup/item_N/... keys are kept");
    check(propOf(w, menu, "item_0/text") && propOf(w, menu, "item_1/checkable") && propOf(w, menu, "item_2/separator") && propOf(w, menu, "hide_on_item_selection"), "a PopupMenu's item_N/... keys are kept");
    check(propOf(w, notes, "wrap_mode") && propOf(w, radial, "radial_initial_angle") && propOf(w, radial, "fill_mode"), "the other widgets' properties are kept");

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
    check(has("\"node\":\"Title\"", "\"property\":\"text\"", "\"rating\":\"converted\""), "RichTextLabel.text is converted");
    check(has("\"node\":\"Title\"", "\"type\":\"RichTextLabel\"", "\"rating\":\"approximated\""), "the RichTextLabel class is approximated, with the reason (effects, bold through faces)");
    check(has("\"node\":\"Title\"", "\"property\":\"threaded\"", "\"rating\":\"approximated\""), "threaded is approximated: stored");
    check(has("\"node\":\"Msg\"", "\"property\":\"bogus_label_property\"", "\"rating\":\"unsupported\""), "an unknown Label property is unsupported");
    check(has("\"node\":\"Msg\"", "\"property\":\"label_settings\"", "\"rating\":\"converted\""), "label_settings is converted");
    check(has("\"type\":\"LabelSettings\"", "\"rating\":\"converted\"", "Aver.Fabric builds it"), "the LabelSettings resource is converted");
    check(has("\"type\":\"ButtonGroup\"", "\"rating\":\"converted\"", "shared by name"), "the ButtonGroup resource is converted as a shared name");
    check(has("\"type\":\"FontVariation\"", "\"property\":\"spacing_glyph\"", "\"rating\":\"approximated\""), "FontVariation spacing is approximated: the engine draws glyphs at their advance");
    check(has("\"type\":\"FontVariation\"", "\"property\":\"variation_embolden\"", "bold face"), "embolden is approximated: the family's bold face");
    check(has("\"type\":\"SystemFont\"", "\"property\":\"font_names\"", "\"rating\":\"approximated\""), "SystemFont names are approximated");
    check(has("\"node\":\"Opt\"", "\"property\":\"popup/item_0/text\"", "\"rating\":\"converted\""), "OptionButton items are converted");
    check(has("\"node\":\"Name\"", "\"property\":\"caret_blink\"", "\"rating\":\"approximated\""), "caret_blink is approximated: the caret does not blink");
    check(has("\"node\":\"Name\"", "\"property\":\"virtual_keyboard_type\"", "\"rating\":\"approximated\""), "the virtual keyboard type is approximated");
    check(has("\"node\":\"Notes\"", "\"property\":\"minimap_draw\"", "\"rating\":\"approximated\""), "the minimap is approximated: none");
    check(has("\"node\":\"Notes\"", "\"type\":\"TextEdit\"", "no syntax highlighting"), "the TextEdit class names what is missing");
    check(has("\"node\":\"Radial\"", "\"property\":\"radial_initial_angle\"", "\"rating\":\"approximated\""), "a radial fill's angles are approximated");
    check(has("\"node\":\"Radial\"", "\"property\":\"fill_mode\"", "\"rating\":\"converted\""), "fill_mode is converted");
    check(has("\"node\":\"Menu\"", "\"property\":\"system_menu_id\"", "\"rating\":\"approximated\""), "a native menu id is approximated");
    check(has("\"node\":\"Menu\"", "\"type\":\"PopupMenu\"", "top-level Control"), "a popup is a top-level Control, not a Window: rated approximated with the reason");
    check(has("\"node\":\"Ask\"", "\"property\":\"initial_position\"", "\"rating\":\"approximated\""), "a dialog's initial_position is approximated");
    check(has("\"node\":\"Ask\"", "\"property\":\"dialog_text\"", "\"rating\":\"converted\""), "dialog_text is converted");
    check(has("\"node\":\"Code\"", "\"type\":\"CodeEdit\"", "no CodeEdit widget"), "a CodeEdit is a carrier with the reason");
    check(has("gui/theme/custom_font", "\"rating\":\"converted\"", "Body-Regular"), "gui/theme/custom_font is converted");
    check(has("\"type\":\"FontFile\"", "\"rating\":\"converted\"", "font set"), "a font file is converted as a family");
    check(has("\"node\":\"Tabs\"", "\"property\":\"current_tab\"", "\"rating\":\"converted\""), "TabContainer.current_tab is converted");
    check(has("\"node\":\"Tree\"", "\"property\":\"select_mode\"", "\"rating\":\"converted\""), "Tree.select_mode is converted");
    check(has("\"node\":\"Split\"", "\"property\":\"dragger_visibility\"", "\"rating\":\"converted\""), "SplitContainer.dragger_visibility is converted");

    AVER_INFO("=== {} checks, {} failed ===", g_checks, g_failures);
    if (!g_failures) std::filesystem::remove_all(outRoot, ec);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
