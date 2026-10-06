#pragma once
// The visual behaviour-tree editor tab: a .ocbt opened as a node canvas with blackboard decorators,
// a blackboard schema panel and live debugging against a running entity. It reads and writes the
// .ocbt asset (synapse::BtAsset: the plain tree plus schema, decorators and comments), so files the
// older list editor (BtEditor) wrote open here unchanged.
//
// WIRING, in SandboxApp.cpp where BtEditor is registered: register this factory BEFORE
// editor::makeBtEditor (first match wins), so .ocbt opens here:
//   #include "BtGraphEditor.hpp"
//   assetEditors_.registerFactory(&editor::makeBtGraphEditor);
// and have "New Behaviour Tree" call editor::btGraphWriteStarter(path, &why).
//
// Everything that is not drawing is plain C++ (layout, edit bookkeeping, undo, save/load), so the
// headless test compiles this file with AVER_WITH_IMGUI undefined, like BtEditorTest.
#include "AssetEditor.hpp"
#include "EditorWidgets.hpp"
#include "SnapshotUndo.hpp"

#include "aver/synapse/BtAsset.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace aver::editor {

// ---- layout (pure) ------------------------------------------------------------------------------

struct BtNodeBox {
    f32 x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;   // top-left, graph units
};

struct BtLayoutParams {
    f32 nodeW = 180.0f;
    f32 gapX = 28.0f;
    f32 gapY = 60.0f;
    f32 headerH = 24.0f;
    f32 lineH = 16.0f;
    f32 padY = 8.0f;
};

// Top-down tree layout: one column slot per leaf, parents centred over their children, rows as tall
// as their tallest node (decorator lines and a comment line add height). Index-parallel to nodes.
std::vector<BtNodeBox> btLayout(const synapse::BtAsset& asset, const BtLayoutParams& p = {});
// Smallest box containing every node; zero-sized for an empty tree.
BtNodeBox btLayoutBounds(const std::vector<BtNodeBox>& boxes);
// Topmost node containing the point, or -1.
i32 btHitTest(const std::vector<BtNodeBox>& boxes, f32 x, f32 y);

// One-line texts the canvas and the list use.
std::string btNodeTitle(const synapse::BtAssetNode& n);       // "Action  Wait (1s)"
std::string btDecoratorText(const synapse::BtDecorator& d);   // "Alert is set  [Both]"

// Writes the starter tree to `path` (Content Browser "New Behaviour Tree"). The result is a valid
// tree the tab can open at once.
bool btGraphWriteStarter(const std::string& path, std::string* why = nullptr);

// ---- the tab ------------------------------------------------------------------------------------

class BtGraphEditor final : public AssetEditor {
public:
    explicit BtGraphEditor(std::string path);
    ~BtGraphEditor() override;

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;
    void resetLayout() override;

    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const synapse::BtAsset& asset() const { return asset_; }
    i32 selected() const { return selected_; }
    void select(i32 index) { selected_ = index; }

    void pushUndo() { history_.push(asset_); }
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // Edits as the tab performs them: push undo, apply, remap the selection, mark dirty. Each
    // returns false (leaving the asset untouched) when it refused.
    bool addChild(fmt::OcBtNodeKind kind, const std::string& leafName = {});
    bool deleteSelected();
    bool reparentSelected(i32 newParent);
    bool moveSelected(i32 delta);
    bool setSelectedKind(fmt::OcBtNodeKind kind);
    bool setNodeName(const std::string& name);
    bool setNodeParam(int slot, f32 value);
    bool setNodeString(const std::string& text);
    bool setNodeComment(const std::string& text);

    bool addDecorator();   // a default IsSet decorator on the first key (or an empty key)
    bool removeDecorator(usize index);
    bool setDecorator(usize index, const synapse::BtDecorator& d);

    bool addKey(const synapse::BbKeyDef& def);
    bool removeKey(const std::string& name);
    bool renameKey(const std::string& from, const std::string& to);
    bool setKey(const std::string& name, const synapse::BbKeyDef& def);   // type/scope/default/description

    void markDirty() { dirty_ = true; }

private:
    void loadFromDisk();

    std::string path_;
    synapse::BtAsset asset_;
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;
    i32 selected_ = 0;
    SnapshotUndo<synapse::BtAsset> history_;
    SplitPane split_;

    // canvas view
    f32 panX_ = 24.0f, panY_ = 24.0f, zoom_ = 1.0f;
    bool panning_ = false;
    std::string newKeyName_;
    int newKeyType_ = 0;
    // live debugging
    u32 debugEntity_ = 0;
    std::unordered_map<std::string, u64> lastStamp_;   // key -> stamp seen last frame
    std::unordered_map<std::string, f32> flash_;       // key -> seconds left to highlight

#if AVER_WITH_IMGUI
    void drawToolbar();
    void drawCanvas(f32 width);
    void drawNodePanel();
    void drawBlackboardPanel();
    void drawDebugPanel();
    bool drawDecoratorEditor(usize index);
#endif
};

std::unique_ptr<AssetEditor> makeBtGraphEditor(const std::string& path);

} // namespace aver::editor
