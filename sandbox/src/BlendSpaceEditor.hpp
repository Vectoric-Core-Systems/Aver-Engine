#pragma once
// The blend-space editor tab: a .ocblend opened as an asset, shown as a grid of draggable sample
// points over the triangulation, with a probe that shows the live weights.
//
// THE HOOK INTO SandboxApp.cpp IS TWO LINES, matching BtEditor.hpp's note:
//   #include "BlendSpaceEditor.hpp"
//   assetEditors_.registerFactory(&editor::makeBlendSpaceEditor);   // APPENDED -- order is precedence
// plus the file in sandbox/CMakeLists.txt. draw() takes no dpi; the tab scales by font size.
#include "AssetEditor.hpp"
#include "SnapshotUndo.hpp"

#include "aver/anim/BlendSpace.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aver::editor {

// ---- edits as FREE FUNCTIONS, so a headless test reaches them (see BtEditor.hpp for the argument) ----

// A starter space the Content Browser writes: three samples along speed. Valid, so it is saveable the
// instant it exists.
anim::BlendSpaceAsset bsStarterSpace();

// Appends a sample at (x, y) clamped into the axes. Returns its index.
i32 bsAddSample(anim::BlendSpaceAsset& a, const std::string& clip, f32 x, f32 y);

// Removes a sample. False when it is the last one (a space needs at least one) or out of range.
bool bsRemoveSample(anim::BlendSpaceAsset& a, i32 index);

// Moves a sample, clamped into the axes and snapped to `snap` (0 = free). Dimension 1 ignores y.
void bsMoveSample(anim::BlendSpaceAsset& a, i32 index, f32 x, f32 y, f32 snap);

// Switches between 1D and 2D. Going to 1D keeps x and zeroes y so the samples stay valid.
void bsSetDims(anim::BlendSpaceAsset& a, u8 dims);

// Adds a marker to a sample at `time`, keeping the list sorted by time. Returns its index.
i32 bsAddMarker(anim::BlendSpaceAsset& a, i32 sample, const std::string& name, f32 time);
bool bsRemoveMarker(anim::BlendSpaceAsset& a, i32 sample, i32 marker);

// Screen <-> axis mapping for the canvas, ImGui-free so the drag arithmetic is testable. `origin` and
// `size` are the canvas rectangle; y runs up in axis space and down on screen.
struct BsView {
    f32 ox = 0, oy = 0, w = 1, h = 1;
};
void bsToScreen(const anim::BlendSpaceAsset& a, const BsView& v, f32 x, f32 y, f32& sx, f32& sy);
void bsFromScreen(const anim::BlendSpaceAsset& a, const BsView& v, f32 sx, f32 sy, f32& x, f32& y);
// The sample whose point is within `radius` pixels of (sx, sy), nearest first, else -1.
i32 bsPick(const anim::BlendSpaceAsset& a, const BsView& v, f32 sx, f32 sy, f32 radius);

class BlendSpaceEditor final : public AssetEditor {
public:
    explicit BlendSpaceEditor(std::string path);

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const anim::BlendSpaceAsset& asset() const { return asset_; }
    i32 selected() const { return selected_; }
    void select(i32 index) { selected_ = index; }

    void pushUndo() { history_.push(asset_); }
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // Each pushes one undo entry; the ImGui layer calls these, and so do tests.
    i32 addSample(const std::string& clip, f32 x, f32 y);
    void deleteSelected();
    void moveSample(i32 index, f32 x, f32 y);          // no undo entry: a drag pushes once on press
    void setDims(u8 dims);
    void markDirty() { dirty_ = true; }

    // The probe: the input the weights below are computed for.
    void setProbe(f32 x, f32 y);
    const std::vector<anim::SampleWeight>& probeWeights() const { return weights_; }

private:
    void loadFromDisk();
    void refreshTopology();

    std::string path_;
    anim::BlendSpaceAsset asset_;
    anim::BlendTopology topo_;
    std::vector<anim::SampleWeight> weights_;
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;
    i32 selected_ = -1;
    f32 probeX_ = 0, probeY_ = 0;
    f32 snap_ = 0.0f;
    bool dragging_ = false;
    bool draggingProbe_ = false;
    bool gesture_ = false;      // one undo entry per drag or typing gesture
    SnapshotUndo<anim::BlendSpaceAsset> history_;

#if AVER_WITH_IMGUI
    void drawCanvas();
    void drawDetails();
#endif
};

std::unique_ptr<AssetEditor> makeBlendSpaceEditor(const std::string& path);

} // namespace aver::editor
