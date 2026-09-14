#pragma once
// The foliage type editor tab: a .ocfoliage opened as an asset. There is no list to select among --
// a type IS the whole record, matching ParticleEditor.hpp's own reason for the identical shape -- so
// this tab is one parameter panel covering every field OcFoliage.hpp declares. No live preview,
// unlike ParticleEditor: a mesh scaled and randomised is not worth a bespoke simulation the way an
// emitter's motion is, so this follows BtEditor.hpp/SoundEditor.hpp's simpler shape instead.
//
// THE HOOK INTO SandboxApp.cpp IS THREE LINES, matching BtEditor.hpp/SoundEditor.hpp before it:
//   #include "FoliageTypeEditor.hpp"
//   assetEditors_.registerFactory(&editor::makeFoliageTypeEditor);   // APPENDED -- order is precedence
// This tab registers no render feature and needs no shutdown hook -- see ParticleEditor.hpp's own
// comment on why THAT tab needs one (a shared preview device resource) for the contrast: this tab
// owns nothing device-side at all.
//
// UNDO THROUGH THE SHARED SnapshotUndo<State> TEMPLATE (SnapshotUndo.hpp), from the start -- unlike
// GraphEditor/SoundEditor/BtEditor, which each hand-rolled their own triad before the template
// existed, this tab has never had a bespoke one to migrate away from.
#include "AssetEditor.hpp"
#include "SnapshotUndo.hpp"

#include "aver/formats/OcFoliage.hpp"

#include <memory>
#include <string>

namespace aver::editor {

// The starter type the Content Browser's "New Foliage Type" writes: OcFoliageData's own defaults,
// which already mirror aver::world::ScatterSpecies' defaults one for one (including its placeholder
// meshPath) -- so, like btStarterTree()/pxStarterEffect() before it, the file this writes is already
// valid() and immediately openable, not a stub the editor would refuse. Declared here so a test can
// check exactly that, matching those two functions' own precedent.
inline fmt::OcFoliageData foliageStarterType() { return fmt::OcFoliageData{}; }

// ---- Foliage mode's entry gate -------------------------------------------------------------------
//
// Whether SandboxApp's Foliage editor mode can be ENTERED right now, and -- when it can, but there is
// nothing in the palette -- what its own panel should say about that. Pulled out of
// SandboxApp::editorModeAvailable's Foliage case as two bools in, one small struct out, so a headless
// test can walk every combination without an App, an Engine, or a live scene::World.
//
// THE REGRESSION THIS UNDOES: when foliagePalette_ moved from "one entry per already-loaded mesh" to
// "one entry per .ocfoliage asset" (see foliageStarterType()'s own comment above), the Foliage case
// started refusing entry outright whenever that palette was empty -- so a project with no .ocfoliage
// authored yet lost foliage painting entirely, the mode's dropdown entry greyed out with no way to
// even reach the panel, which already had text for exactly this case (SandboxApp::
// buildFoliageModePanel's empty-palette branch) that nothing could ever open to show. This module's
// own commit history had already named that failure shape -- "a mode that cannot be entered is a dead
// dropdown entry" -- as the reason the palette was made a real asset in the first place; refusing
// again on empty would reintroduce the exact complaint that change was meant to answer.
//
// So the mode now OPENS on an empty palette (showEmptyState tells the panel to draw its
// create-a-type action instead of the palette list) and refuses ONLY for the other, still-genuine
// reason: nothing to paint ONTO. That second reason is unchanged -- it is not the bug being fixed.
struct FoliageModeGate {
    bool available;       // false only when there is no landscape section to paint onto
    const char* whyNot;   // set when !available -- the dropdown's disabled-entry tooltip; empty otherwise
    bool showEmptyState;  // true when available but the palette has no .ocfoliage types yet
};

inline FoliageModeGate foliageModeGate(bool landscapeLoaded, bool paletteEmpty) {
    if (!landscapeLoaded) {
        return FoliageModeGate{
            false, "Foliage paints onto terrain, and this level has no landscape section.", false};
    }
    return FoliageModeGate{true, "", paletteEmpty};
}

// ---- the tab ------------------------------------------------------------------------------------
//
// Declared in the header rather than hidden behind the factory, for BtEditor.hpp's own reason: a
// headless test constructs this tab directly to exercise load/save/dirty/undo/every field setter
// with no ImGui and no window.
class FoliageTypeEditor final : public AssetEditor {
public:
    explicit FoliageTypeEditor(std::string path);

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    // Reachable for a headless test, for BtEditor/ParticleEditor's own reason.
    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const fmt::OcFoliageData& type() const { return type_; }
    void markDirty() { dirty_ = true; }

    // Snapshot undo, through the shared SnapshotUndo<State> template -- see the header comment above.
    void pushUndo();
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // ---- field edits, as MEMBERS, matching ParticleEditor's own reason: this format has no
    // structure to edit -- one record, no nodes, no links -- so every edit here already IS the whole
    // operation. Each clamps to whatever range OcFoliageData::valid() enforces at save time, so a
    // value typed in the UI can never be one the save path would go on to reject. ----
    void setMeshPath(std::string meshPath);
    void setMaterial(std::string material);
    void setScaleRange(f32 scaleMin, f32 scaleMax);
    void setWeight(f32 weight);
    void setRandomizeYaw(bool on);
    void setCollisionRadiusCm(f32 radiusCm);
    void setAlignToNormal(bool on);

private:
    void loadFromDisk();
    // Resyncs meshBuf_/materialBuf_ from type_ -- loadFromDisk()'s own two snprintf calls, factored
    // out so undo()/redo() can share them, matching ParticleEditor::syncEditBuffers()'s own reason:
    // an undo that changes a string field without this would leave the visible text box showing the
    // PRE-undo value until the author happened to touch it.
    void syncEditBuffers();

    std::string path_;
    fmt::OcFoliageData type_;
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;

    // Through the shared SnapshotUndo<State> template (SnapshotUndo.hpp). State IS the whole record,
    // matching BtEditor/SoundEditor's own shape (their State is fmt::OcBtData/fmt::OcSoundData
    // directly) rather than ParticleEditor's {effect, extras} pair -- OcFoliageData already carries
    // every field this tab edits, with nothing held alongside it the way OcParticleExtras is.
    SnapshotUndo<fmt::OcFoliageData> history_;

    // Editable-text scratch buffers, synced from type_ on every load/reload/undo/redo -- plain char
    // arrays so they need no #if, even though only draw() (ImGui) reads them.
    char meshBuf_[256] = {};
    char materialBuf_[128] = {};

#if AVER_WITH_IMGUI
    void drawParams();
#endif
};

// Creates a foliage type editor for a .ocfoliage, else nullptr.
std::unique_ptr<AssetEditor> makeFoliageTypeEditor(const std::string& path);

} // namespace aver::editor
