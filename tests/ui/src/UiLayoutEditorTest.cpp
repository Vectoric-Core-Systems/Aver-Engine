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

    if (g_failures) {
        AVER_ERROR("{} check(s) failed", g_failures);
        return 1;
    }
    AVER_INFO("all layout editor checks passed");
    return 0;
}
