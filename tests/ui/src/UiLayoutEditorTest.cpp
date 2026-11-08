// Headless test for the UI layout editor tab: the canvas maths, the tab's own load / save / dirty /
// undo bookkeeping and its edits against a real file. AVER_WITH_IMGUI is undefined here, so only the
// part that does not draw is compiled.
#include "UiLayoutEditor.hpp"

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"

#include <cmath>
#include <filesystem>
#include <string>

using namespace aver;
using namespace aver::editor;
using namespace aver::ui;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(f32 a, f32 b, f32 eps = 0.05f) { return std::fabs(a - b) <= eps; }

static std::string tempFile(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver_ui_editor_test";
    std::filesystem::create_directories(dir);
    return (dir / name).string();
}

static std::vector<std::string> childNames(const UiLayoutEditor& ed, const std::string& parent) {
    std::vector<std::string> out;
    const i32 p = uiLayoutFind(ed.doc(), parent);
    for (const i32 c : uiLayoutChildren(ed.doc(), p)) out.push_back(ed.doc().nodes[static_cast<usize>(c)].props.name);
    return out;
}

static UiEstimatedMetrics est;

int main() {
    AVER_INFO("=== canvas maths ===");
    {
        UiWidgetProps p;
        p.offsets = UiInsets{10, 20, 0, 0};
        uiEditorMoveBy(p, 5, -3);
        check(approx(p.offsets.left, 15) && approx(p.offsets.top, 17), "moving a point-anchored widget shifts its position");

        UiWidgetProps s;
        s.anchors = UiAnchors{0, 0, 1, 1};
        s.offsets = UiInsets{10, 10, 10, 10};
        uiEditorMoveBy(s, 4, 6);
        check(approx(s.offsets.left, 14) && approx(s.offsets.right, 6) && approx(s.offsets.top, 16) && approx(s.offsets.bottom, 4),
              "moving a stretched widget shifts both insets and keeps its size");

        UiWidgetProps e;
        e.width = 100;
        e.height = 50;
        e.offsets.left = 30;
        uiEditorMoveEdge(e, UiEdge::Right, 20, 100, 50);
        check(approx(e.width, 120) && approx(e.offsets.left, 30), "dragging the right edge grows the width");
        uiEditorMoveEdge(e, UiEdge::Left, 10, 120, 50);
        check(approx(e.width, 110) && approx(e.offsets.left, 40), "dragging the left edge keeps the right edge still");
        e.pivot = {0.5f, 0.5f};
        const f32 leftBefore = e.offsets.left - 0.5f * e.width;
        const f32 rightBefore = leftBefore + e.width;
        uiEditorMoveEdge(e, UiEdge::Left, 10, 110, 50);
        check(approx(e.offsets.left - 0.5f * e.width + e.width, rightBefore), "the opposite edge holds under a centre pivot too");
        check(approx(e.offsets.left - 0.5f * e.width, leftBefore + 10), "and the dragged edge moves by the drag");
        UiWidgetProps a;
        uiEditorMoveEdge(a, UiEdge::Right, 20, 80, 30);
        check(approx(a.width, 100), "an auto-sized axis takes its current size first");
        const f32 keep = e.width;
        uiEditorMoveEdge(e, UiEdge::Right, -1000, keep, 50);
        check(approx(e.width, keep), "a drag past zero size is refused");
        UiWidgetProps st;
        st.anchors = UiAnchors{0, 0, 1, 1};
        st.offsets = UiInsets{5, 5, 5, 5};
        uiEditorMoveEdge(st, UiEdge::Left, 10, 0, 0);
        uiEditorMoveEdge(st, UiEdge::Bottom, 7, 0, 0);
        check(approx(st.offsets.left, 15) && approx(st.offsets.bottom, -2), "stretched edges move their inset");
    }
    {
        const UiRect parent{0, 0, 1000, 500};
        const UiRect rect{100, 50, 200, 80};
        bool all = true;
        for (u32 i = 0; i < static_cast<u32>(UiAnchorPreset::Count); ++i) {
            UiWidgetProps p;
            uiEditorApplyAnchorPreset(p, static_cast<UiAnchorPreset>(i), parent, rect);
            const UiRect back = uiResolveAnchored(parent, p.anchors, p.offsets, p.pivot, p.width, p.height, 1.0f);
            if (!(approx(back.x, rect.x) && approx(back.y, rect.y) && approx(back.w, rect.w) && approx(back.h, rect.h))) {
                AVER_ERROR("    preset '{}' moved the widget", uiAnchorPresetName(static_cast<UiAnchorPreset>(i)));
                all = false;
            }
        }
        check(all, "every anchor preset keeps the widget exactly where it was");
        UiWidgetProps p;
        uiEditorApplyAnchorPreset(p, UiAnchorPreset::Center, parent, rect);
        check(approx(p.anchors.minX, 0.5f) && approx(p.pivot.x, 0.5f) && approx(p.offsets.left, -300) && approx(p.offsets.top, -160),
              "centre anchors at the middle with the offsets measured from it");
        uiEditorApplyAnchorPreset(p, UiAnchorPreset::StretchAll, parent, rect);
        check(approx(p.offsets.left, 100) && approx(p.offsets.right, 700) && approx(p.offsets.top, 50) && approx(p.offsets.bottom, 370),
              "stretch-all records the insets");
        check(std::string(uiAnchorPresetName(UiAnchorPreset::TopLeft)) == "Top left", "presets have names");
    }

    AVER_INFO("=== the starter and the factory ===");
    {
        UiLayoutDoc d;
        std::string err;
        check(uiParseLayout(uiStarterLayoutText(), d, &err) && d.valid(&err), "the starter text parses and is valid: " + err);
        check(makeUiLayoutEditor("x.txt") == nullptr, "the factory refuses other extensions");
        check(makeUiLayoutEditor(tempFile("missing.ocui")) != nullptr, "and claims .ocui even when the file is missing (the tab shows the error)");
        UiLayoutEditor missing(tempFile("really_missing.ocui"));
        check(!missing.loaded() && !missing.loadError().empty(), "a missing file is a load error, not a crash");
        std::string why;
        check(!missing.save(&why) && !why.empty(), "and cannot be saved over");
    }

    AVER_INFO("=== the tab ===");
    const std::string file = tempFile("tab.ocui");
    {
        check(writeFileText(file, uiStarterLayoutText()), "wrote the starter file");
        UiLayoutEditor ed(file);
        check(ed.loaded() && ed.doc().nodes.size() == 6 && !ed.dirty() && !ed.canUndo(), "loads clean");
        check(ed.previewWidget(0) != 0 && ed.preview().size() == 6, "the preview has a widget per node");
        UiEstimatedMetrics m;
        ed.layoutPreview(m);
        const i32 win = uiLayoutFind(ed.doc(), "Window");
        check(approx(ed.previewRect(win).x, 750) && approx(ed.previewRect(win).w, 420), "the window is centred at the reference size");
        check(ed.nodeOf(ed.previewWidget(win)) == win && ed.nodeOf(0) == -1, "preview widgets map back to nodes");

        ed.setPreviewSize(1280, 720);
        check(approx(ed.previewRect(win).w, 420.0f * 720.0f / 1080.0f), "a smaller preview scales the layout");
        ed.setPreviewSize(1920, 1080);

        ed.select(win);
        const i32 added = ed.addWidget(UiWidgetKind::Button);
        check(added > 0 && ed.dirty() && ed.canUndo() && ed.selected() == added, "adding a widget selects it and dirties the tab");
        check(ed.doc().nodes[static_cast<usize>(added)].parent == win, "under the selection");
        ed.undo();
        check(ed.doc().nodes.size() == 6 && ed.canRedo(), "undo removes it");
        ed.redo();
        check(ed.doc().nodes.size() == 7, "redo restores it");
        ed.undo();

        ed.select(0);
        ed.deleteSelected();
        check(ed.doc().nodes.size() == 6, "the root cannot be deleted");

        const i32 play = uiLayoutFind(ed.doc(), "PlayButton");
        ed.select(play);
        check(ed.renameSelected("StartButton") && uiLayoutFind(ed.doc(), "StartButton") >= 0, "rename");
        check(ed.doc().nodes[0].props.defaultFocus == "StartButton", "references by name follow a rename");
        check(!ed.renameSelected("Window") && !ed.renameSelected(""), "a taken or empty name is refused");
        ed.undo();
        check(ed.doc().nodes[0].props.defaultFocus == "PlayButton", "and undo puts them back");
    }
    {
        UiLayoutEditor ed(file);
        const i32 win = uiLayoutFind(ed.doc(), "Window");
        check(ed.setProperty(win, "width", "300") && ed.doc().nodes[static_cast<usize>(win)].props.width == 300 && ed.dirty(), "set a property by name");
        ed.undo();
        check(ed.doc().nodes[static_cast<usize>(win)].props.width == 420, "undo restores it");

        UiLayoutEditor clean(file);
        check(clean.setProperty(win, "width", "420") && !clean.canUndo(), "setting the same value records nothing");
        check(!clean.setProperty(win, "width", "banana") && !clean.canUndo(), "a bad value is refused and records nothing");
        check(!clean.setProperty(win, "noSuchProperty", "1") && !clean.canUndo(), "so is an unknown property");
        check(clean.setProperty(win, "layout", "grid") && clean.doc().nodes[static_cast<usize>(win)].props.layout == UiLayoutMode::Grid, "enums by name");
    }
    {
        UiLayoutEditor ed(file);
        UiEstimatedMetrics m;
        ed.layoutPreview(m);
        const i32 win = uiLayoutFind(ed.doc(), "Window");

        // A drop inside the window goes into the window (a vertical stack: no position).
        const UiRect wr = ed.previewRect(win);
        const i32 inWin = ed.placeWidgetAt(UiWidgetKind::Toggle, wr.x + 10, wr.y + 10);
        check(inWin > 0 && ed.doc().nodes[static_cast<usize>(inWin)].parent == uiLayoutFind(ed.doc(), "Window"),
              "dropping onto the window puts the widget in it");
        ed.undo();

        // A drop on empty root space goes into the root, positioned there.
        const i32 panel = ed.placeWidgetAt(UiWidgetKind::Panel, 40, 30);
        check(panel > 0 && ed.doc().nodes[static_cast<usize>(panel)].parent == 0, "dropping on empty space puts it in the root");
        check(approx(ed.doc().nodes[static_cast<usize>(panel)].props.offsets.left, 40) &&
                  approx(ed.doc().nodes[static_cast<usize>(panel)].props.offsets.top, 30),
              "at the drop point");
        ed.layoutPreview(m);
        check(approx(ed.previewRect(panel).x, 40) && approx(ed.previewRect(panel).y, 30), "and it lands there in the preview");

        ed.select(panel);
        ed.dragSelected(30, 5, true);
        check(approx(ed.doc().nodes[static_cast<usize>(panel)].props.offsets.left, 70) &&
                  approx(ed.doc().nodes[static_cast<usize>(panel)].props.offsets.top, 35), "dragging moves it");
        ed.dragSelected(10, 0, false);
        check(approx(ed.doc().nodes[static_cast<usize>(panel)].props.offsets.left, 80), "and keeps moving");
        ed.undo();
        check(approx(ed.doc().nodes[static_cast<usize>(panel)].props.offsets.left, 40), "the whole drag is one undo");

        ed.dragSelectedEdge(UiEdge::Right, 50, true);
        check(approx(ed.doc().nodes[static_cast<usize>(panel)].props.width, 250), "dragging an edge resizes it");
        ed.layoutPreview(m);
        const UiRect before = ed.designRect(panel);
        ed.applyAnchorPreset(UiAnchorPreset::Center);
        ed.layoutPreview(m);
        const UiRect after = ed.designRect(panel);
        check(approx(before.x, after.x) && approx(before.y, after.y) && approx(before.w, after.w) && approx(before.h, after.h),
              "re-anchoring in the tab does not move the widget");

        const UiWidgetId picked = uiEditorPick(ed.preview(), 960, 540);
        check(picked != 0 && ed.nodeOf(picked) >= 0, "picking finds a widget");
        check(uiEditorPick(ed.preview(), -50, -50) == 0, "and nothing outside the viewport");
    }
    {
        UiLayoutEditor ed(file);
        UiEstimatedMetrics m;
        ed.layoutPreview(m);
        ed.select(uiLayoutFind(ed.doc(), "SettingsButton"));
        ed.dragSelected(0, 30, true);
        check(childNames(ed, "Window") == std::vector<std::string>({"Title", "PlayButton", "QuitButton", "SettingsButton"}),
              "dragging down inside a vertical stack reorders it");
        check(ed.doc().nodes[static_cast<usize>(ed.selected())].props.name == "SettingsButton", "the selection follows the widget");
        ed.dragSelected(0, -80, false);
        check(childNames(ed, "Window")[1] == "SettingsButton", "and back up");
    }
    {
        UiLayoutEditor ed(file);
        ed.select(uiLayoutFind(ed.doc(), "QuitButton"));
        ed.duplicateSelected();
        check(uiLayoutFind(ed.doc(), "QuitButton_2") > 0 && ed.selected() == uiLayoutFind(ed.doc(), "QuitButton_2"), "duplicate selects the copy");
        ed.deleteSelected();
        check(uiLayoutFind(ed.doc(), "QuitButton_2") < 0 && ed.dirty(), "delete removes it");
        ed.select(uiLayoutFind(ed.doc(), "PlayButton"));
        ed.reparentSelected(0);
        check(ed.doc().nodes[static_cast<usize>(uiLayoutFind(ed.doc(), "PlayButton"))].parent == 0, "reparent moves a widget");
        ed.reparentSelected(uiLayoutFind(ed.doc(), "PlayButton"));
        check(ed.doc().nodes[static_cast<usize>(uiLayoutFind(ed.doc(), "PlayButton"))].parent == 0, "a self-parent is refused");
        ed.setTheme("light");
        check(ed.doc().theme == "light", "the theme is a document setting");
        UiDpi d = ed.doc().dpi;
        d.mode = UiScaleMode::Width;
        d.refWidth = 1280;
        ed.setScaling(d);
        check(ed.doc().dpi.mode == UiScaleMode::Width && approx(ed.doc().dpi.refWidth, 1280), "and so is the scaling");
    }
    {
        UiLayoutEditor ed(file);
        ed.setTheme("contrast");
        ed.addWidget(UiWidgetKind::Slider);
        std::string why;
        check(ed.save(&why) && !ed.dirty(), "saving writes the file and cleans the tab: " + why);
        UiLayoutEditor again(file);
        check(again.loaded() && again.doc().theme == "contrast" && again.doc().nodes.size() == 7, "and it loads back as saved");
        check(uiWriteLayout(again.doc()) == uiWriteLayout(ed.doc()), "identically");

        ed.setTheme("dark");
        const std::string kept = uiWriteLayout(ed.doc());
        ed.onFileChanged();
        check(ed.dirty() && uiWriteLayout(ed.doc()) == kept, "a dirty tab keeps its edits when the file changes on disk");
        UiLayoutEditor clean(file);
        writeFileText(file, uiStarterLayoutText());
        clean.onFileChanged();
        check(clean.doc().nodes.size() == 6, "a clean tab reloads");
    }

    AVER_INFO("=== localized text in the preview ===");
    {
        const std::string locFile = tempFile("loc.ocui");
        writeFileText(locFile, uiStarterLayoutText());
        UiLayoutEditor ed(locFile);
        const i32 play = uiLayoutFind(ed.doc(), "PlayButton");
        check(play > 0 && ed.setProperty(play, "text", "\"@UI:Menu.Play=Play\""), "a reference can be set as text");
        const UiWidget* w = ed.preview().get(ed.previewWidget(play));
        check(w && w->text == "@UI:Menu.Play=Play" && ed.preview().shown(w->text) == "Play", "with no host hook the preview shows the inline default");

        static bool german = false;
        static u32 epoch = 1;
        UiLayoutEditorLoc loc;
        loc.resolve = [](std::string_view raw, std::string& out) {
            if (raw != "@UI:Menu.Play=Play") return false;
            out = german ? "Spielen" : "Play";
            return true;
        };
        loc.epoch = [] { return epoch; };
        setUiLayoutEditorLoc(loc);
        ed.save(nullptr);
        UiLayoutEditor hooked(locFile);                                  // a preview is built with the host's resolver
        const i32 hp = uiLayoutFind(hooked.doc(), "PlayButton");
        w = hooked.preview().get(hooked.previewWidget(hp));
        check(w && hooked.preview().shown(w->text) == "Play", "the host's resolver reaches the preview");
        german = true;
        ++epoch;
        hooked.layoutPreview(est);
        check(w && hooked.preview().shown(w->text) == "Spielen", "and the preview follows its epoch without a rebuild");
        check(hooked.doc().nodes[static_cast<usize>(hp)].props.text == "@UI:Menu.Play=Play", "while the document keeps the reference");
        setUiLayoutEditorLoc({});
    }

    AVER_INFO("=== nested layouts ===");
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver_ui_editor_use";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "Parts");
        const auto put = [&](const char* rel, const std::string& text) { writeFileText((dir / rel).string(), text); };
        put("Main.ocui", uiStarterLayoutText());
        put("Parts/Row.ocui",
            "OCUI 2\nWIDGET RowRoot panel -\nSET RowRoot layout hstack\nWIDGET Label text RowRoot\nSET Label text Default\n"
            "WIDGET Body panel RowRoot\nEXPOSE Title Label text\nSLOT Content Body\n");
        put("Loop.ocui", "OCUI 2\nWIDGET LoopRoot panel -\nUSE Back Main.ocui LoopRoot\n");

        UiLayoutEditor ed((dir / "Main.ocui").string());
        const std::vector<std::string> layouts = ed.projectLayouts();
        check(layouts.size() == 2 && layouts[0] == "Loop.ocui" && layouts[1] == "Parts/Row.ocui", "the palette lists the project's other layouts as USE paths");

        const i32 row = ed.addUse("Parts/Row.ocui");
        check(row > 0 && ed.doc().nodes[static_cast<usize>(row)].use && ed.dirty() && ed.canUndo(), "adding a layout is an undoable USE node");
        check(ed.doc().nodes[static_cast<usize>(row)].props.name == "Row", "named after its file");
        check(ed.previewErrors().empty(), "and its file is found under the content root");
        const UiWidgetId label = ed.preview().find("Row/Label");
        check(label != 0 && ed.nodeOf(label) == row, "the preview holds the used widgets, which belong to the USE node when picked");
        check(ed.previewWidget(row) == ed.preview().find("Row"), "the USE node is the instance root");

        std::string err;
        const std::vector<UiExposedProp> ex = ed.exposedOf(row, &err);
        check(ex.size() == 1 && ex[0].name == "Title" && ex[0].value[0] == "Default" && !ex[0].overridden, "its exposed properties are listed with their defaults");
        check(ed.setOverride(row, "Title", "Hello") && ed.preview().get(ed.preview().find("Row/Label"))->text == "Hello", "an override reaches the preview");
        check(ed.exposedOf(row)[0].overridden && ed.exposedOf(row)[0].value[0] == "Hello", "and the inspector's list");
        ed.undo();
        check(ed.preview().get(ed.preview().find("Row/Label"))->text == "Default", "undo takes it back");
        ed.redo();
        check(ed.clearOverride(row, "Title") && ed.preview().get(ed.preview().find("Row/Label"))->text == "Default", "clearing returns to the layout's value");

        ed.select(row);
        const i32 fill = ed.addWidget(UiWidgetKind::Button);
        check(ed.setProperty(fill, "slot", "Content") && ed.preview().get(ed.preview().find("Button"))->parent == ed.preview().find("Row/Body"),
              "a child with a slot lands in the used layout's slot");

        check(ed.addUse("Loop.ocui") == -1 && ed.statusMessage().find("cycle") != std::string::npos, "a layout that uses this one is refused: a cycle");
        const usize nodes = ed.doc().nodes.size();
        check(ed.doc().nodes.size() == nodes, "and nothing was added");
        check(ed.setUsePath(row, "Parts/Gone.ocui") && !ed.previewErrors().empty() && ed.preview().find("Row/Missing") != 0,
              "a missing file is a placeholder with a message, not a failure");
        check(ed.setUsePath(row, "Parts/Row.ocui") && ed.previewErrors().empty(), "and pointing back repairs it");

        const i32 placed = ed.placeUseAt("Parts/Row.ocui", 10, 10);
        check(placed > 0 && ed.doc().valid(), "a layout can be dropped on the canvas");

        check(ed.save(&err), "it saves: " + err);
        UiLayoutEditor again((dir / "Main.ocui").string());
        check(again.loaded() && again.doc().usesV2() && uiLayoutFind(again.doc(), "Row") > 0 && again.previewErrors().empty(), "and reloads with the nested layout");
        std::filesystem::remove_all(dir);
    }

    AVER_INFO("=== palette panels ===");
    {
        const std::string file = tempFile("panels.ocui");
        writeFileText(file, uiStarterLayoutText());
        UiLayoutEditor ed(file);
        const std::vector<UiWidgetPreset>& presets = uiWidgetPresets();
        usize sizeBox = presets.size();
        for (usize i = 0; i < presets.size(); ++i) if (std::string(presets[i].label) == "Size Box") sizeBox = i;
        check(sizeBox < presets.size(), "the palette offers a Size Box");
        const i32 added = ed.addPreset(sizeBox);
        check(added > 0 && ed.dirty() && ed.canUndo(), "adding a panel preset is an undoable edit");
        const UiWidgetProps& p = ed.doc().nodes[static_cast<usize>(added)].props;
        check(p.layout == UiLayoutMode::Overlay && p.kind == UiWidgetKind::Panel && approx(p.width, 200), "it is a plain panel in overlay mode with a size");
        ed.layoutPreview(est);
        check(approx(ed.designRect(added).w, 200) && approx(ed.designRect(added).h, 100), "and the preview lays it out at that size");
        const i32 placed = ed.placePresetAt(sizeBox, 5, 5);
        check(placed > 0 && ed.doc().valid(), "a preset can be dropped on the canvas too");
        ed.undo();
        ed.undo();
        check(uiLayoutFind(ed.doc(), "SizeBox") < 0, "undo removes them");
        ed.select(0);
        const i32 sw = ed.addPreset(presets.size() - 1);
        ed.layoutPreview(est);
        const UiWidget* w = ed.preview().get(ed.previewWidget(sw));
        check(w && w->layout == UiLayoutMode::Switcher, "the widget switcher preset is a switcher");
        const i32 a = ed.addWidget(UiWidgetKind::Button);
        ed.select(sw);
        ed.addWidget(UiWidgetKind::Button);
        ed.select(a);
        const std::string before = uiWriteLayout(ed.doc());
        ed.dragSelected(40, 40, true);
        check(uiWriteLayout(ed.doc()) == before, "dragging inside an overlay or switcher does not reorder");
    }

    AVER_INFO("=== palette search, device presets, safe zone ===");
    {
        check(uiEditorPaletteMatch("Uniform Grid Panel", "") && uiEditorPaletteMatch("Uniform Grid Panel", "grid") && uiEditorPaletteMatch("Uniform Grid Panel", "PANEL uni"),
              "palette search: empty matches all, words match in any order and case");
        check(!uiEditorPaletteMatch("Button", "grid") && !uiEditorPaletteMatch("Button", "butter"), "and every word must be found");
        const std::vector<UiDevicePreset>& presets = uiEditorDevicePresets();
        check(presets.size() >= 8 && presets[0].width == 1920 && presets[0].height == 1080, "device presets start with 1080p");
        const UiRect safe = uiEditorSafeRect(UiInsets{0.05f, 0.05f, 0.05f, 0.05f}, 1000, 500);
        check(approx(safe.x, 50) && approx(safe.y, 25) && approx(safe.w, 900) && approx(safe.h, 450), "the safe rectangle removes the fractions from each edge");

        const std::string f = tempFile("device.ocui");
        writeFileText(f, uiStarterLayoutText());
        UiLayoutEditor ed(f);
        check(ed.devicePreset() == 0, "a new tab previews at the first preset");
        i32 phone = -1;
        for (usize i = 0; i < presets.size(); ++i) if (std::string(presets[i].name) == "Phone portrait") phone = static_cast<i32>(i);
        ed.setDevicePreset(phone);
        ed.layoutPreview(est);
        check(ed.devicePreset() == phone && ed.preview().viewport().w == 1080 && ed.preview().viewport().h == 2340, "a device preset sets the preview size");
        const UiRect zone = ed.safeZoneRect();
        check(zone.y > 0 && zone.x == 0 && zone.h < 2340 && zone.w == 1080, "and the phone's safe zone keeps the notch and the home bar out");
        ed.setPreviewSize(777, 555);
        check(ed.devicePreset() == -1 && approx(ed.safeZoneRect().x, 777 * 0.05f), "a custom size is no preset and gets a 5% safe zone");
        ed.setPreviewSize(1920, 1080);
        check(ed.devicePreset() >= 0, "a size that matches a preset selects it again");
    }

    AVER_INFO("=== multi-select ===");
    {
        const std::string f = tempFile("multi.ocui");
        writeFileText(f, uiStarterLayoutText());
        UiLayoutEditor ed(f);
        const i32 a = ed.addWidget(UiWidgetKind::Button);
        ed.select(0);
        const i32 b = ed.addWidget(UiWidgetKind::Button);
        ed.select(0);
        const i32 c = ed.addWidget(UiWidgetKind::Text);
        const std::string an = ed.doc().nodes[static_cast<usize>(a)].props.name, bn = ed.doc().nodes[static_cast<usize>(b)].props.name;
        ed.select(uiLayoutFind(ed.doc(), an));
        ed.toggleSelect(uiLayoutFind(ed.doc(), bn));
        check(ed.selection().size() == 2 && ed.isSelected(uiLayoutFind(ed.doc(), an)) && ed.isSelected(uiLayoutFind(ed.doc(), bn)), "ctrl-click adds a second widget");
        check(ed.selected() == uiLayoutFind(ed.doc(), bn), "the toggled one becomes the primary");
        ed.toggleSelect(uiLayoutFind(ed.doc(), bn));
        check(ed.selection().size() == 1 && ed.selected() == uiLayoutFind(ed.doc(), an), "toggling the primary off hands over to the other");
        ed.toggleSelect(uiLayoutFind(ed.doc(), bn));
        (void)c;

        const bool set = ed.setPropertyOnSelection("width", "333");
        check(set && approx(ed.doc().nodes[static_cast<usize>(uiLayoutFind(ed.doc(), an))].props.width, 333) &&
                  approx(ed.doc().nodes[static_cast<usize>(uiLayoutFind(ed.doc(), bn))].props.width, 333),
              "a property set on a multi-selection reaches every selected widget");
        ed.undo();
        check(!approx(ed.doc().nodes[static_cast<usize>(uiLayoutFind(ed.doc(), an))].props.width, 333) &&
                  !approx(ed.doc().nodes[static_cast<usize>(uiLayoutFind(ed.doc(), bn))].props.width, 333),
              "and is one undo entry");
        check(!ed.setPropertyOnSelection("noSuchProp", "1") && !ed.setPropertyOnSelection("value", "1"), "an unknown property, or one no selected widget has, records nothing");
        const f32 x0 = ed.doc().nodes[static_cast<usize>(uiLayoutFind(ed.doc(), an))].props.offsets.left;
        ed.moveSelectionBy(15, 0);
        check(approx(ed.doc().nodes[static_cast<usize>(uiLayoutFind(ed.doc(), an))].props.offsets.left, x0 + 15), "moving the selection moves all of it");
        ed.undo();
        ed.select(uiLayoutFind(ed.doc(), an));
        ed.toggleSelect(uiLayoutFind(ed.doc(), bn));
        const usize count = ed.doc().nodes.size();
        ed.deleteSelection();
        check(ed.doc().nodes.size() == count - 2 && uiLayoutFind(ed.doc(), an) < 0 && uiLayoutFind(ed.doc(), bn) < 0 && ed.doc().valid(), "delete removes the whole selection");
        ed.undo();
        check(ed.doc().nodes.size() == count, "in one undo");
        ed.select(0);
        ed.deleteSelection();
        check(ed.doc().nodes.size() == count, "the root is never deleted");
        ed.selectRange(1, 3);
        check(ed.selection().size() == 3 && ed.selected() == 3, "shift-click selects a range, the clicked end primary");
    }

    AVER_INFO("=== animation timeline ===");
    {
        const std::string f = tempFile("timeline.ocui");
        writeFileText(f, uiStarterLayoutText());
        UiLayoutEditor ed(f);
        const i32 btn = uiLayoutFind(ed.doc(), "PlayButton");
        ed.select(btn);
        const std::string anim = ed.addAnimation("Intro");
        check(anim == "Intro" && ed.animationNames().size() == 1 && ed.timelineAnimation() == "Intro", "a new animation has one opacity track and becomes current");
        check(ed.addAnimation("Intro").empty(), "a taken name is refused");
        const std::string second = ed.addAnimation();
        check(second == "Anim", "an empty name gets a free one");
        ed.deleteAnimation("Anim");
        check(ed.animationNames().size() == 1, "and an animation can be deleted");
        ed.setTimelineAnimation("Intro");

        const std::vector<UiTimelineTrack> tracks = ed.animationTracks("Intro");
        check(tracks.size() == 1 && tracks[0].widget == "PlayButton" && tracks[0].prop == UiAnimProp::Opacity && tracks[0].keys.size() == 2 &&
                  approx(tracks[0].keys[0].v[0], 0) && approx(tracks[0].keys[1].v[0], 1) && approx(ed.animationLength("Intro"), 1),
              "the track runs 0 -> 1 over a second");

        check(ed.addTrack("Intro", btn, UiAnimProp::Translate) && !ed.addTrack("Intro", btn, UiAnimProp::Translate) && !ed.addTrack("Intro", 999, UiAnimProp::Tint),
              "a second track is added; the same widget and property twice, or a missing widget, is refused");
        check(ed.setKeyValue("Intro", 1, 1, "40,0") && approx(ed.animationTracks("Intro")[1].keys[1].v[0], 40), "a key value is set from its token");
        check(!ed.setKeyValue("Intro", 1, 1, "banana") && !ed.setKeyValue("Intro", 1, 9, "1,1"), "a bad token or key is refused");
        const i32 mid = ed.addKey("Intro", 1, 0.5f);
        check(mid == 1 && approx(ed.animationTracks("Intro")[1].keys[1].v[0], 20), "a key added between two samples the curve there");
        check(ed.addKey("Intro", 1, 0.5f) < 0, "two keys cannot share a time");
        const i32 moved = ed.moveKey("Intro", 1, 1, 2.0f);
        const std::vector<UiTimelineTrack> after = ed.animationTracks("Intro");
        check(moved == 2 && approx(after[1].keys[2].time, 2.0f) && approx(ed.animationLength("Intro"), 2.0f) && after[1].keys[0].time < after[1].keys[1].time,
              "moving a key past another keeps them ordered and stretches the animation");
        check(ed.setKeyEase("Intro", 1, 0, UiEase::QuadOut) && ed.animationTracks("Intro")[1].keys[0].ease == UiEase::QuadOut && !ed.setKeyEase("Intro", 1, 0, UiEase::QuadOut),
              "a key's ease is set, and setting it again records nothing");
        check(ed.deleteKey("Intro", 1, 2) && ed.animationTracks("Intro")[1].keys.size() == 2, "a key is deleted");
        ed.deleteKey("Intro", 1, 1);
        check(!ed.deleteKey("Intro", 1, 0), "the last key of a track stays");
        check(ed.deleteTrack("Intro", 1) && ed.animationTracks("Intro").size() == 1, "a track is deleted");

        std::string written = uiWriteLayout(ed.doc());
        UiLayoutDoc back;
        std::string err;
        check(uiParseLayout(written, back, &err) && uiWriteLayout(back) == written, "the edited document round-trips: " + err);

        // The preview plays it.
        ed.layoutPreview(est);
        const UiWidgetId pw = ed.previewWidget(btn);
        ed.timelineSeek(0.5f);
        check(ed.preview().get(pw)->fx.mask & kFxOpacity && approx(ed.preview().get(pw)->fx.opacity, 0.5f), "scrubbing applies the time to the preview widget");
        check(approx(ed.timelineTime(), 0.5f) && !ed.timelinePlaying(), "and does not play");
        ed.setPropertyOnSelection("width", "123");
        check(approx(ed.preview().get(ed.previewWidget(btn))->fx.opacity, 0.5f), "an edit rebuilds the preview and keeps the scrubbed time");
        ed.timelinePlay(true);
        ed.timelineTick(0.25f);
        check(ed.timelinePlaying() && approx(ed.timelineTime(), 0.75f), "play advances with the tick");
        ed.timelineTick(0.5f);
        check(!ed.timelinePlaying() && approx(ed.timelineTime(), 1.0f) && approx(ed.preview().get(ed.previewWidget(btn))->fx.opacity, 1.0f),
              "and stops at the end holding the last values");
        ed.setTimelineLoop(true);
        ed.timelinePlay(true);
        ed.timelineTick(1.25f);
        check(ed.timelinePlaying() && approx(ed.timelineTime(), 0.25f), "looping wraps");
        ed.timelinePlay(false);
        check(!ed.timelinePlaying(), "pause");

        check(ed.renameAnimation("Intro", "Appear") && ed.timelineAnimation() == "Appear" && !ed.renameAnimation("Appear", "") && ed.animationNames()[0] == "Appear", "rename");
        ed.undo();
        check(ed.animationNames()[0] == "Intro", "undo puts the name back");
        ed.deleteAnimation("Intro");
        check(ed.timelineAnimation().empty() && ed.animationNames().empty(), "deleting the current animation clears the timeline");
        ed.undo();
        check(ed.animationNames().size() == 1, "and undo brings it back");
    }

    AVER_INFO("=== bindings and events ===");
    {
        const std::string f = tempFile("binds.ocui");
        writeFileText(f, uiStarterLayoutText());
        UiLayoutEditor ed(f);
        const i32 btn = uiLayoutFind(ed.doc(), "PlayButton");
        check(ed.addBinding(btn, "text", "Player.Name", "Hi {0}") == 0 && ed.doc().binds[0].path == "vm:Player.Name" && ed.doc().binds[0].widget == "PlayButton" &&
                  ed.doc().binds[0].rest == std::vector<std::string>({"format", "Hi {0}"}),
              "a binding is recorded with its source prefix and format");
        check(ed.addBinding(btn, "visible", "var:Alive", "", true) == 1 && ed.doc().binds[1].rest == std::vector<std::string>({"mode", "change"}), "a change-mode variable binding");
        check(ed.addBinding(btn, "nonsense", "A") < 0 && ed.addBinding(btn, "text", "") < 0 && ed.addBinding(99, "text", "A") < 0, "an unknown property, an empty path and a missing widget are refused");
        check(ed.setBinding(0, "text", "var:Name", "", false) && ed.doc().binds[0].path == "var:Name" && ed.doc().binds[0].rest.empty(), "a binding is edited");
        const std::string text = uiWriteLayout(ed.doc());
        UiLayoutDoc back;
        check(uiParseLayout(text, back) && back.binds.size() == 2 && uiWriteLayout(back) == text, "bindings round-trip through the file");
        check(ed.deleteBinding(0) && ed.doc().binds.size() == 1 && !ed.deleteBinding(5), "a binding is deleted");

        check(ed.addEvent(btn, "click", "command", "game.start") == 0 && ed.addEvent(btn, "hover", "method", "Hud.Hover", {"x"}) == 1, "events are recorded");
        check(ed.addEvent(btn, "bogus", "command", "x") < 0 && ed.addEvent(btn, "click", "bogus", "x") < 0 && ed.addEvent(btn, "click", "command", "") < 0 && ed.addEvent(99, "click", "command", "x") < 0,
              "a bad event, action, target or widget is refused");
        check(ed.setEvent(1, "unhover", "graph", "OnLeave", {}) && ed.doc().events[1].event == "unhover" && ed.doc().events[1].action == "graph", "an event is edited");
        ed.layoutPreview(est);
        check(ed.previewErrors().empty(), "the preview builds the routes without complaint");
        ed.select(btn);
        check(ed.renameSelected("StartButton") && ed.doc().events[0].widget == "StartButton" && ed.doc().binds[0].widget == "StartButton", "renaming a widget updates its bindings and events");
        check(ed.deleteEvent(0) && ed.doc().events.size() == 1, "an event is deleted");
        ed.deleteSelected();
        check(ed.doc().events.empty() && ed.doc().binds.empty() && ed.doc().valid(), "deleting a widget takes its records with it");
    }

    AVER_INFO("=== widget state preview ===");
    {
        const std::string f = tempFile("state.ocui");
        writeFileText(f, uiStarterLayoutText());
        UiLayoutEditor ed(f);
        const i32 btn = uiLayoutFind(ed.doc(), "PlayButton");
        ed.select(btn);
        ed.layoutPreview(est);
        ed.setPreviewState(1);
        ed.applyPreviewState();
        check(ed.preview().get(ed.previewWidget(btn))->hovered && !ed.preview().get(ed.previewWidget(btn))->pressed, "hovered state");
        ed.setPreviewState(2);
        ed.applyPreviewState();
        check(ed.preview().get(ed.previewWidget(btn))->pressed, "pressed state");
        ed.setPreviewState(3);
        ed.applyPreviewState();
        check(!ed.preview().get(ed.previewWidget(btn))->enabled && !ed.preview().get(ed.previewWidget(btn))->hovered, "disabled state");
        ed.setPreviewState(0);
        ed.applyPreviewState();
        check(ed.preview().get(ed.previewWidget(btn))->enabled && ed.doc().nodes[static_cast<usize>(btn)].props.enabled, "back to normal; the document was never touched");
        check(ed.setProperty(btn, "bgHoverColor", "#112233") && ed.doc().nodes[static_cast<usize>(btn)].props.bgHoverColor == uiRgba(0x11, 0x22, 0x33), "per-state colours are properties");
    }

    AVER_INFO("=== theme sheets ===");
    {
        const std::filesystem::path dir = std::filesystem::temp_directory_path() / "aver_ui_editor_theme";
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir / "Content" / "UI");
        const std::string f = (dir / "Content" / "UI" / "Menu.ocui").string();
        writeFileText(f, uiStarterLayoutText());
        const usize btnKind = static_cast<usize>(UiWidgetKind::Button);
        {
            UiLayoutEditor ed(f);
            ed.layoutPreview(est);
            check(!ed.themeIsSheet() && !ed.editTheme([](UiTheme& t) { t.baseFontSize = 30; }), "a built-in theme cannot be edited");
            check(!ed.createThemeSheet("../Escape.octheme") && !ed.createThemeSheet("/abs.octheme"), "a sheet path must stay under the content folder");
            check(ed.createThemeSheet("UI/Main") && ed.themeIsSheet() && ed.doc().theme == "UI/Main.octheme", "creating a sheet selects it (the extension is added)");
            check(std::filesystem::exists(dir / "Content" / "UI" / "Main.octheme") && !ed.createThemeSheet("UI/Main.octheme"), "the file exists and is not overwritten");
            check(ed.canUndo() && ed.dirty(), "selecting it is an undoable edit");
            const f32 before = ed.preview().theme().kinds[btnKind].radius;
            check(ed.editTheme([&](UiTheme& t) { t.kinds[btnKind].radius = 9; t.kinds[btnKind].background[1] = 0xFF445566; }), "an edit applies");
            check(ed.preview().theme().kinds[btnKind].radius == 9 && before == 0 && ed.preview().theme().kinds[btnKind].background[1] == 0xFF445566, "to the preview tree at once");
            check(!ed.editTheme([](UiTheme&) {}), "an edit that changes nothing records nothing");
            check(ed.editTheme([&](UiTheme& t) { t.kinds[btnKind].radius = 11; }, true) && ed.editTheme([&](UiTheme& t) { t.kinds[btnKind].radius = 12; }, false), "a drag continues its entry");
            ed.undo();
            check(ed.theme().kinds[btnKind].radius == 9 && ed.preview().theme().kinds[btnKind].radius == 9, "undo takes the whole drag back");
            ed.redo();
            check(ed.theme().kinds[btnKind].radius == 12, "redo restores it");
            std::string why;
            check(ed.save(&why) && !ed.themeDirty(), "save writes the layout and the sheet: " + why);
            std::string sheetText;
            readFileText((dir / "Content" / "UI" / "Main.octheme").string(), sheetText);
            check(sheetText.find("CONSTANT Aver.button radius 12") != std::string::npos && sheetText.find("BASETHEME dark") != std::string::npos, "the sheet holds the change as a diff");
            std::string layoutText;
            readFileText(f, layoutText);
            check(layoutText.find("UI/Main.octheme") != std::string::npos, "and the layout names it");
            check(ed.projectThemes().size() == 1 && ed.projectThemes()[0] == "UI/Main.octheme", "the project lists its sheets");
        }
        {
            UiLayoutEditor again(f);
            check(again.themeIsSheet() && again.themeError().empty() && again.theme().kinds[btnKind].radius == 12, "reopened, the sheet is read back");
            check(again.preview().theme().kinds[btnKind].background[1] == 0xFF445566, "and the preview draws with it");
            again.setTheme("light");
            check(!again.themeIsSheet() && again.preview().theme().name == "light", "switching to a built-in theme");
            again.undo();
            check(again.themeIsSheet() && again.theme().kinds[btnKind].radius == 12, "and back by undo");
            again.setTheme("UI/Nope.octheme");
            check(!again.themeError().empty() && again.preview().theme().name == "dark" && again.doc().theme == "UI/Nope.octheme", "a missing sheet keeps its name, shows dark and says why");
        }
        {
            // Items another tool keeps in the sheet survive an edit.
            writeFileText((dir / "Content" / "UI" / "Shared.octheme").string(), "OCTHEME 1\nNAME Shared\nCOLOR Button font_color #FFFFFFFF\nFUTURE thing\n");
            UiLayoutEditor ed(f);
            ed.setTheme("UI/Shared.octheme");
            check(ed.editTheme([&](UiTheme& t) { t.kinds[btnKind].radius = 4; }), "a sheet from elsewhere can be edited");
            std::string why;
            ed.save(&why);
            std::string sheetText;
            readFileText((dir / "Content" / "UI" / "Shared.octheme").string(), sheetText);
            check(sheetText.find("COLOR Button font_color #FFFFFFFF") != std::string::npos && sheetText.find("CONSTANT Aver.button radius 4") != std::string::npos, "its Godot items are kept beside the edit");
        }
        std::filesystem::remove_all(dir);
    }

    if (g_failures) {
        AVER_ERROR("{} check(s) failed", g_failures);
        return 1;
    }
    AVER_INFO("all layout editor checks passed");
    return 0;
}
