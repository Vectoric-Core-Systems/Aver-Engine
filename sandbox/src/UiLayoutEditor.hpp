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

#include "aver/ui/UiAnimation.hpp"
#include "aver/ui/UiLayoutAsset.hpp"
#include "aver/ui/UiProps.hpp"
#include "aver/ui/UiTree.hpp"

#include <algorithm>
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

// Palette search: every word of `query` (case-insensitive) is found in `label`; an empty query matches everything.
bool uiEditorPaletteMatch(std::string_view label, std::string_view query);

// Device presets for the preview size, with the safe zone each device needs (fractions of the screen, per edge).
struct UiDevicePreset {
    const char* name;
    f32 width, height;
    ui::UiInsets safe;
};
const std::vector<UiDevicePreset>& uiEditorDevicePresets();
// The safe rectangle of a screen of `w` x `h` with `safe` fractions removed from each edge.
ui::UiRect uiEditorSafeRect(const ui::UiInsets& safe, f32 w, f32 h);

// One row of the animation timeline: a track as the editor shows it.
struct UiTimelineTrack {
    std::string widget;
    ui::UiAnimProp prop = ui::UiAnimProp::Opacity;
    std::vector<ui::UiAnimKey> keys;
};

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

    // ---- multi-select ------------------------------------------------------------------------------
    // The primary selection (select(), selected()) plus any number of others, kept by name so a renumbering edit does not lose
    // them. select() resets to one widget.
    void toggleSelect(i32 index);                            // Ctrl+click: add or remove, the toggled one becomes primary
    void selectRange(i32 first, i32 last);                   // Shift+click in the hierarchy: the nodes between, in document order
    std::vector<i32> selection() const;                      // every selected node, ascending
    bool isSelected(i32 index) const;
    // Edits applied to every selected widget as ONE undo entry. A widget a property does not apply to is left as it was.
    bool setPropertyOnSelection(std::string_view key, std::string_view text);
    void deleteSelection();                                  // the root is never deleted
    void moveSelectionBy(f32 dxDesign, f32 dyDesign, bool begin = true);   // begin = one undo entry; false continues a drag

    // ---- device preview ------------------------------------------------------------------------------
    i32 devicePreset() const { return device_; }             // index into uiEditorDevicePresets(), -1 for a custom size
    void setDevicePreset(i32 index);                         // sets the preview size and the safe zone
    bool safeZoneShown() const { return safeZone_; }
    void showSafeZone(bool on) { safeZone_ = on; }
    ui::UiRect safeZoneRect() const;                         // preview pixels; the preset's insets, else 5% on every edge

    // ---- per-state preview ---------------------------------------------------------------------------
    // Draws the selected widget as if hovered / pressed / disabled (0 normal, 1 hovered, 2 pressed, 3 disabled) in the
    // non-interactive preview, so the state colours can be judged while editing them.
    i32 previewState() const { return previewState_; }
    void setPreviewState(i32 state) { previewState_ = std::clamp(state, 0, 3); }
    void applyPreviewState();

    // ---- animation timeline --------------------------------------------------------------------------
    // The ANIM records as animations, edited with undo; the preview tree plays them (scrub, play, loop).
    std::vector<std::string> animationNames() const;
    std::vector<UiTimelineTrack> animationTracks(const std::string& anim) const;
    f32 animationLength(const std::string& anim) const;
    // A new animation: one track (opacity of the selected widget, 0 -> its value over one second). Empty name = a free one.
    // Returns the name, empty if refused.
    std::string addAnimation(const std::string& name = {});
    bool deleteAnimation(const std::string& anim);
    bool renameAnimation(const std::string& anim, const std::string& newName);
    // Tracks are numbered in file order within the animation. addTrack starts with the widget's authored value at 0 and at the
    // animation's end. False when the animation has that widget and property already, or the widget is not a node.
    bool addTrack(const std::string& anim, i32 widgetIndex, ui::UiAnimProp prop);
    bool deleteTrack(const std::string& anim, i32 track);
    // Keys: addKey samples the value the track has at `time` (so adding one changes nothing visible); moveKey keeps the keys
    // ordered and returns the key's new number (-1 refused); setKeyValue takes the record's token ("0.5", "10,20", "#FF0000").
    i32 addKey(const std::string& anim, i32 track, f32 time);
    i32 moveKey(const std::string& anim, i32 track, i32 key, f32 time);
    bool setKeyValue(const std::string& anim, i32 track, i32 key, const std::string& token);
    bool setKeyEase(const std::string& anim, i32 track, i32 key, ui::UiEase ease);
    bool deleteKey(const std::string& anim, i32 track, i32 key);
    // Playback in the preview. The current animation and time survive the preview rebuild every edit does.
    const std::string& timelineAnimation() const { return animName_; }
    void setTimelineAnimation(const std::string& anim);
    f32 timelineTime() const { return animTime_; }
    void timelineSeek(f32 seconds);                          // scrub: applies the time, stops playing
    void timelinePlay(bool play);
    bool timelinePlaying() const { return animPlaying_; }
    bool timelineLoop() const { return animLoop_; }
    void setTimelineLoop(bool loop) { animLoop_ = loop; }
    void timelineTick(f32 dt);                               // advances the preview while playing

    // ---- bindings and events ---------------------------------------------------------------------------
    // BIND and EVENT records, by their number in the document (doc().binds / doc().events). Each refusal returns false/-1 and
    // records nothing; each success is one undo entry.
    i32 addBinding(i32 widgetIndex, const std::string& property, const std::string& path, const std::string& format = {}, bool onChange = false);
    bool setBinding(i32 binding, const std::string& property, const std::string& path, const std::string& format, bool onChange);
    bool deleteBinding(i32 binding);
    i32 addEvent(i32 widgetIndex, const std::string& event, const std::string& action, const std::string& target, const std::vector<std::string>& args = {});
    bool setEvent(i32 index, const std::string& event, const std::string& action, const std::string& target, const std::vector<std::string>& args);
    bool deleteEvent(i32 index);

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

    std::vector<std::string> extraSelected_;   // selected besides selected_, by name
    i32 device_ = 0;
    bool safeZone_ = false;
    i32 previewState_ = 0;
    std::string animName_;
    f32 animTime_ = 0;
    bool animPlaying_ = false;
    bool animLoop_ = false;
    void applyTimeline();                       // re-applies the timeline after the preview was rebuilt
    bool editAnimTrack(const std::string& anim, i32 track, const std::function<bool(ui::UiAnimTrack&)>& edit);
    i32 animRecord(const std::string& anim, i32 track) const;   // index into doc_.anims, -1 if none

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

    void drawBottomPanel();
    void drawTimeline();
    void drawBindingsTab();
    void drawEventsTab();
    void drawStates();
    std::vector<std::string> layouts_;   // the palette's project layouts
    std::string paletteQuery_;
    i32 bottomTab_ = 0;              // 0 timeline, 1 bindings, 2 events
    f32 bottomHeight_ = 0;           // 0 = a default share of the window
    bool bottomOpen_ = true;
    i32 timelineTrack_ = -1, timelineKey_ = -1;
    bool timelineDragging_ = false, timelineDragMoved_ = false, scrubbing_ = false;
    f32 timelineDragTime_ = 0;
    f32 timelinePxPerSec_ = 160.0f;
    i32 timelineAddProp_ = 0;
    std::string newBindProp_ = "text", newBindPath_, newBindFormat_;
    bool newBindChange_ = false;
    std::string newEvent_ = "click", newEventAction_ = "command", newEventTarget_;
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
