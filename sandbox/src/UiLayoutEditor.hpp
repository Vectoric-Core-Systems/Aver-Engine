#pragma once
// The UI layout editor tab: a .ocui opened as an asset. A palette of widget kinds to drag onto a
// canvas that previews the layout live, a hierarchy, and a property inspector built from the same
// property table the file format and the C ABI use (modules/ui UiProps.hpp).
//
// THE HOOK INTO THE SANDBOX IS TWO LINES in SandboxApp.cpp, like BtEditor's:
//   #include "UiLayoutEditor.hpp"
//   assetEditors_.registerFactory(&editor::makeUiLayoutEditor);   // APPENDED -- order is precedence
// plus "New UI Layout" in the Content Browser, which writes editor::uiStarterLayoutText() (see
// docs/GAME_UI.md for the exact registration list).
//
// Everything that is not drawing is a free function or a member that needs no ImGui, so
// tests/ui/UiLayoutEditorTest runs it headless: the structural edits are modules/ui's own
// (UiLayoutAsset.hpp), and the canvas maths -- moving, resizing, anchor presets, picking -- live here.
#include "AssetEditor.hpp"
#include "EditorWidgets.hpp"
#include "SnapshotUndo.hpp"

#include "aver/ui/UiLayoutAsset.hpp"
#include "aver/ui/UiProps.hpp"
#include "aver/ui/UiTree.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aver::editor {

// Localization as the preview sees it. The host installs these (SandboxApp.cpp, over aver_fw_loc_*) so this file links
// no framework and the headless test can pass fakes. Unset: references show their inline defaults.
struct UiLayoutEditorLoc {
    std::function<bool(std::string_view raw, std::string& out)> resolve;   // true = `raw` was a reference
    std::function<u32()> epoch;                                            // changes when any text may have
    std::function<std::vector<std::string>()> cultures;                    // the shipped cultures
    std::function<std::string()> culture;                                  // the preview culture
    std::function<void(const std::string&)> setCulture;                    // switches (and remembers) it
};
void setUiLayoutEditorLoc(UiLayoutEditorLoc loc);

// ---- canvas maths, as FREE FUNCTIONS --------------------------------------------------------------
//
// All distances are DESIGN pixels (what the file stores), never preview or screen pixels; the tab
// divides by the preview scale and zoom before calling.

enum class UiEdge : u8 { Left, Right, Top, Bottom };

// Moves a widget. A point-anchored axis shifts its position; a stretched axis shifts both insets so
// the size is kept.
void uiEditorMoveBy(ui::UiWidgetProps& p, f32 dx, f32 dy);

// Drags one edge by `delta`, keeping the opposite edge where it was. `widthDesign`/`heightDesign` are
// the widget's current size, used to give an auto-sized axis an explicit size before resizing it.
void uiEditorMoveEdge(ui::UiWidgetProps& p, UiEdge edge, f32 delta, f32 widthDesign, f32 heightDesign);

enum class UiAnchorPreset : u8 {
    TopLeft, TopCenter, TopRight, MiddleLeft, Center, MiddleRight, BottomLeft, BottomCenter, BottomRight,
    StretchTop, StretchBottom, StretchLeft, StretchRight, StretchHorizontal, StretchVertical, StretchAll,
    Count
};
const char* uiAnchorPresetName(UiAnchorPreset p);

// Re-anchors a widget WITHOUT moving it: sets the anchors (and pivot for a point preset) and
// recomputes the offsets and size so the widget keeps `rectDesign` inside `parentDesign`.
void uiEditorApplyAnchorPreset(ui::UiWidgetProps& p, UiAnchorPreset preset, const ui::UiRect& parentDesign,
                               const ui::UiRect& rectDesign);

// The deepest, topmost visible widget whose rectangle contains the point, ignoring input modes (an
// editor selects what a player could not click). Clipping containers hide what they clip. 0 for none.
ui::UiWidgetId uiEditorPick(const ui::UiTree& tree, f32 x, f32 y);

// The text the Content Browser's "New UI Layout" writes. A valid, renderable layout (a test loads it).
std::string uiStarterLayoutText();

// ---- the tab ----------------------------------------------------------------------------------------

class UiLayoutEditor final : public AssetEditor {
public:
    explicit UiLayoutEditor(std::string path);

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;
    void resetLayout() override;

    // ---- the headless surface ------------------------------------------------------------------
    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const ui::UiLayoutDoc& doc() const { return doc_; }
    i32 selected() const { return selected_; }
    void select(i32 index);
    void markDirty() { dirty_ = true; }

    void pushUndo();
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // The edits as the TAB performs them: push undo, apply, keep the selection on what was acted on,
    // mark dirty, refresh the preview. -1 / false when the edit refused (and nothing was recorded).
    i32 addWidget(ui::UiWidgetKind kind);                    // under the selection
    i32 addPreset(usize preset);                             // a palette panel (ui::uiWidgetPresets()), under the selection
    void deleteSelected();
    void duplicateSelected();
    void reparentSelected(i32 newParent);
    void moveSelected(i32 delta);
    bool renameSelected(const std::string& name);
    bool setProperty(i32 index, std::string_view key, std::string_view text);
    void setTheme(const std::string& name);
    void setScaling(const ui::UiDpi& dpi);
    void applyAnchorPreset(UiAnchorPreset preset);           // keeps the widget where it is
    // Drops a new widget of `kind` at a point in the preview (preview pixels): under the container
    // there, positioned there when its parent places by anchors. Returns the new index or -1.
    i32 placeWidgetAt(ui::UiWidgetKind kind, f32 previewX, f32 previewY);
    i32 placePresetAt(usize preset, f32 previewX, f32 previewY);
    i32 placeUseAt(const std::string& usePath, f32 previewX, f32 previewY);

    // ---- nested layouts (USE) ----------------------------------------------------------------------
    // Paths are relative to the project's content root (the nearest "Content" folder above this file, else this
    // file's folder). A USE that would make a cycle is refused and the reason is in statusMessage().
    i32 addUse(const std::string& usePath);                  // under the selection
    bool setUsePath(i32 index, const std::string& usePath);
    // Sets / clears an OVERRIDE of a USE node; one undo entry each. `valueText` is tokenised like a file record.
    bool setOverride(i32 index, const std::string& name, std::string_view valueText);
    bool clearOverride(i32 index, const std::string& name);
    // The exposed properties of the layout a USE node names, with this instance's values.
    std::vector<ui::UiExposedProp> exposedOf(i32 index, std::string* err = nullptr) const;
    // The other .ocui files of the project, as USE paths.
    std::vector<std::string> projectLayouts() const;
    // What went wrong building the preview (a missing nested layout, a cycle) and the last refusal; empty when fine.
    const std::vector<std::string>& previewErrors() const { return previewErrors_; }
    const std::string& statusMessage() const { return status_; }
    // Moves the selection by a canvas drag of (dx, dy) PREVIEW pixels. Inside a stack or grid it
    // reorders among its siblings instead. `begin` records one undo entry for a whole drag.
    void dragSelected(f32 dx, f32 dy, bool begin);
    void dragSelectedEdge(UiEdge edge, f32 delta, bool begin);

    // ---- the preview -----------------------------------------------------------------------------
    // The preview tree is rebuilt from the document after every edit. Layout needs text metrics, so
    // the caller supplies them (the tab uses ImGui's; a test uses the estimate).
    void setPreviewSize(f32 w, f32 h);
    void layoutPreview(const ui::UiTextMetrics& m);
    ui::UiTree& preview() { return preview_; }
    const ui::UiTree& preview() const { return preview_; }
    // The preview widget for a document node, or 0.
    ui::UiWidgetId previewWidget(i32 index) const;
    // A node's rectangle in preview pixels / in design pixels (preview divided by the scale).
    ui::UiRect previewRect(i32 index) const;
    ui::UiRect designRect(i32 index) const;
    // The document node a preview widget came from, or -1.
    i32 nodeOf(ui::UiWidgetId id) const;

private:
    void loadFromDisk();
    void rebuildPreview();
    void applyPreviewLoc();
    ui::UiRect designRectOfParent(i32 index) const;
    // Shared by the two drops: kind (or, when preset >= 0, that palette panel) at a preview point.
    i32 placeAt(ui::UiWidgetKind kind, i64 preset, const std::string* usePath, f32 previewX, f32 previewY);
    ui::UiLayoutLoader loader() const;
    std::string selfUsePath() const;          // this file as a USE path, "" when it is outside the content root
    bool useAllowed(const std::string& usePath, i32 replacing);

    std::string path_;
    ui::UiLayoutDoc doc_;
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;
    i32 selected_ = 0;
    SnapshotUndo<ui::UiLayoutDoc> history_;

    ui::UiTree preview_;
    std::vector<ui::UiWidgetId> previewIds_;
    std::vector<std::string> previewErrors_;
    std::string status_;
    f32 previewW_ = 1920.0f, previewH_ = 1080.0f;
    const ui::UiTextMetrics* metrics_ = nullptr;   // last metrics given to layoutPreview
    ui::UiEstimatedMetrics estimate_;

    bool dragUndoPushed_ = false;
    u32 structureVersion_ = 0;   // bumped whenever the document is rebuilt; a draw loop compares it
    f32 dragAccumX_ = 0, dragAccumY_ = 0;   // reordering inside a stack or grid

    SplitPane splitLeft_;
    SplitPane splitInspector_;

#if AVER_WITH_IMGUI
    void drawToolbar();
    void drawPalette();
    void drawHierarchy();
    void drawHierarchyRow(i32 index);
    void drawCanvas();
    void drawInspector();
    // `target` edits a scratch copy instead of the selected widget (an exposed property of a USE); `applied` then
    // stores the result. `label` replaces the property's key as the caption.
    bool drawProperty(const ui::UiPropDesc& d, ui::UiWidgetProps* target = nullptr,
                      const std::function<void(const ui::UiWidgetProps&)>& applied = {}, const char* label = nullptr);
    void drawUseInspector();
    // Under a text / placeholder / items field: the resolved "Shown:" line for a reference, else a Localize button.
    void drawLocalizeRow(const ui::UiPropDesc& d);
    void propEditBegin();
    void propEditEnd();

    std::vector<std::string> layouts_;   // the palette's project layouts
    int layoutsScanFrame_ = 0;
    bool interact_ = false;          // run the preview as a player would instead of editing it
    i32 zoomPercent_ = 0;            // 0 = fit
    i32 canvasDrag_ = 0;             // bits: 1 left, 2 right, 4 top, 8 bottom edge; 16 move; 0 none
    bool canvasDragStarted_ = false;
    std::string lastEvent_;          // the last event the interactive preview raised
    bool propEditing_ = false;
    bool propEdited_ = false;
#endif
};

// Creates a layout editor for a .ocui, else nullptr.
std::unique_ptr<AssetEditor> makeUiLayoutEditor(const std::string& path);

} // namespace aver::editor
