#pragma once
// The .ocshape editor tab (SHAPES_MATERIALS, docs/PHYSICS_SHAPES.md "Editor"): a collision shape tree opened as an asset. A node
// outline (leaves, decorators, compounds with their children in order), the selected node's numbers and options, the file's header (name,
// mass, default surface), an issues list that is fmt::validateOcShape's output, and a preview: the shape built into a throw-away static
// body, its triangles read back and drawn by Show > Collision Shapes' overlay over the selected entity (or the world origin).
//
// THE HOOK INTO SandboxApp IS THREE LINES, like VehicleEditor:
//   #include "OcShapeEditor.hpp"
//   assetEditors_.registerFactory(&editor::makeOcShapeEditor);   // APPENDED -- order is precedence
// and the Content Browser opens a double-clicked .ocshape through AssetEditorHost::open as for every asset editor. "New Collision Shape"
// writes editor::writeNewShapeStarter's one-box file.
//
// Edits apply to bodies built AFTER the save (the next Play). Undo is the shared SnapshotUndo<State> with the whole shape as the state
// (a shape is a few kilobytes), the same as BtEditor and VehicleEditor. All structural rules are in OcShapeEditorModel.hpp (tested).
#include "AssetEditor.hpp"
#include "OcShapeEditorModel.hpp"
#include "SnapshotUndo.hpp"

#include "aver/formats/OcShape.hpp"

#include <functional>
#include <memory>
#include <string>

namespace aver::editor {

// What the Content Browser's "New Collision Shape" writes (a 25 cm half-extent box, 10 kg), refusing to touch an existing file.
bool writeNewShapeStarter(const std::string& path, std::string* why);

class OcShapeEditor final : public AssetEditor {
public:
    explicit OcShapeEditor(std::string path);
    ~OcShapeEditor() override;

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    // Reachable for a headless test.
    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const fmt::OcShapeData& shape() const { return shape_; }
    const std::string& selectedId() const { return selectedId_; }
    void select(const std::string& id) { selectedId_ = id; }
    void markDirty() { dirty_ = true; ++revision_; }

    // Whole-shape edit: snapshot for undo, run `change` on the shape, mark dirty. The widgets and the tests both go through it.
    template <typename Fn>
    void edit(Fn&& change) {
        if (!loaded_) return;
        history_.push(shape_);
        change(shape_);
        markDirty();
    }

    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

private:
    void loadFromDisk();
    void publishPreview();

    std::string path_;
    fmt::OcShapeData shape_;
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;
    SnapshotUndo<fmt::OcShapeData> history_;
    std::string selectedId_;
    u64 revision_ = 1;            // bumped by every change to shape_; the preview is rebuilt when it moves
    u64 previewRevision_ = 0;

    // The undo bracket of a drag / slider / text gesture: the shape as it was when the gesture began.
    fmt::OcShapeData gestureBefore_;
    bool gestureOpen_ = false;

#if AVER_WITH_IMGUI
    // ---- widget helpers: call straight after the ImGui widget with its return value ----
    void gesture(bool changed, const std::function<void()>& write);   // drag / text: one undo step per gesture
    void instant(bool changed, const std::function<void()>& write);   // checkbox / combo / button: one undo step per change
    bool dragF(const char* label, f32& field, f32 speed, f32 lo, f32 hi, const char* format = "%.2f");
    bool text(const char* label, std::string& field, const char* hint = nullptr);

    void drawToolbar();
    void drawHeader();
    void drawIssues();
    void drawOutline();
    void drawNode(fmt::ShapeNode& n);
    void drawChildRecords(const fmt::ShapeNode& compound);
    void drawPreviewNote();
#endif
};

// Creates a shape editor for a .ocshape, else nullptr.
std::unique_ptr<AssetEditor> makeOcShapeEditor(const std::string& path);

} // namespace aver::editor
