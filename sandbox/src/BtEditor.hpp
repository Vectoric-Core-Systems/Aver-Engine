#pragma once
// The behaviour-tree editor tab: a .ocbt opened as an asset, shown as a tree beside a parameter
// panel for whichever node is selected.
//
// THE HOOK INTO SandboxApp.cpp IS THREE LINES, matching GraphEditor.hpp's own header note:
//   #include "BtEditor.hpp"
//   assetEditors_.registerFactory(&editor::makeBtEditor);   // APPENDED -- order is precedence
// and nothing else. draw(Engine&) carries no dpi, but this tab needs none (it has no canvas to
// scale, unlike GraphEditor's node graph), so there is no push-setter counterpart here.
#include "AssetEditor.hpp"

#include "aver/formats/OcBt.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aver::editor {

// ---- structural edits, as FREE FUNCTIONS -------------------------------------------------------
//
// NOT BtEditor members, and that is the whole reason this slice is testable at all: draw() is
// behind `#if AVER_WITH_IMGUI` and cannot run headless, but these can -- the same "keep the logic
// where a test can reach it" split GraphEditorGeometry.hpp already makes against GraphEditor's own
// draw(), and that tests/editor/CMakeLists.txt's own comments argue for at length.
//
// Every one REBUILDS the array in pre-order DFS, so .ocbt's "parents before children" invariant
// (OcBtData::valid(), modules/formats/src/OcBt.cpp) holds BY CONSTRUCTION rather than by each edit
// separately remembering to preserve it. Indices therefore MOVE across any of these calls; each
// returns the new index of whatever it acted on, so a caller can keep a selection on the same node.

// The children of `parent`, in array order -- which IS their execution order (a Selector runs them
// until one succeeds, a Sequence until one fails).
std::vector<i32> btChildrenOf(const std::vector<fmt::OcBtNode>& nodes, i32 parent);

// Appends a child of `kind` under `parent`. A Condition/Action is given a real registered built-in
// name ("HasTarget"/"Wait") rather than an empty one, so the tree stays valid() -- and therefore
// SAVEABLE -- the instant it is created. -1 when `parent` is out of range.
i32 btAddChild(std::vector<fmt::OcBtNode>& nodes, i32 parent, fmt::OcBtNodeKind kind);

// Removes `index` AND its whole subtree -- never a lone node, because an orphaned child would leave
// a second parentless node and OcBtData::valid() refuses to write that. Returns the new index of
// the deleted node's PARENT so a caller can select something sensible; -1 when `index` is the root
// (a tree must keep one) or is out of range.
i32 btDeleteSubtree(std::vector<fmt::OcBtNode>& nodes, i32 index);

// Moves `index` and its subtree under `newParent`. Returns its new index; -1 when that would make a
// cycle (`newParent` is `index` itself or one of its descendants), when `index` is the root, or
// when either is out of range.
i32 btReparent(std::vector<fmt::OcBtNode>& nodes, i32 index, i32 newParent);

// The tree the Content Browser's "New Behaviour Tree" writes: a Selector root over one `Wait`.
//
// DECLARED HERE SO A TEST CAN CHECK IT, exactly as snStarterGraph is in SoundEditor.hpp. This one has
// a sharper reason than convention: BtEditor's constructor refuses a file it cannot load (it sets
// loaded_ = false and shows an error instead of an editable tree), so a starter that fails
// OcBtData::valid() would be written successfully and then rejected by the very editor the create
// path opens for it -- a failure that looks like success at the moment it happens and only appears
// one step later. A test that saves this and loads it back is what makes that impossible.
//
// `Wait` rather than a bare structural root because it is one of the registered built-ins (see
// btAddChild above, which picks the same name for the same reason): the tree resolves and runs,
// instead of being a shape that validates and does nothing.
fmt::OcBtData btStarterTree();

// Moves `index` one place earlier (delta < 0) or later (delta > 0) among its siblings. ORDER IS
// SEMANTIC HERE, not cosmetic -- see btChildrenOf -- so this is a real behaviour edit, not a tidy-up.
// Returns its new index; -1 when it is already at that end, is the root, or is out of range.
i32 btMoveSibling(std::vector<fmt::OcBtNode>& nodes, i32 index, i32 delta);

// Changes `index`'s kind, keeping the tree valid(): a node becoming a Condition/Action gains a
// default built-in name if it had none, and one leaving those kinds has its name cleared, because
// valid() requires a node be named if and only if it is one of those two. Without this, flipping a
// Sequence to a Condition in the UI would produce a tree that silently refused to save.
void btSetKind(std::vector<fmt::OcBtNode>& nodes, i32 index, fmt::OcBtNodeKind kind);

// The display name of a node kind, for a combo box or a tree row.
const char* btKindName(fmt::OcBtNodeKind kind);

// ---- the tab ------------------------------------------------------------------------------------

// Declared in the header (rather than hidden in the .cpp behind the factory) for GraphEditor's own
// reason: tests/editor's GraphEditorLoadSaveTest constructs that editor directly to exercise
// load/save/dirty/undo with no ImGui and no window, and this tab is meant to be tested the same way.
class BtEditor final : public AssetEditor {
public:
    explicit BtEditor(std::string path);

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    // Reachable for a headless test, and for the same reason the edits above are free functions.
    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const fmt::OcBtData& tree() const { return tree_; }
    i32 selected() const { return selected_; }
    void select(i32 index) { selected_ = index; }

    // Snapshot undo, copying GraphEditor::pushUndo's own whole-state approach rather than command
    // objects -- a behaviour tree is a short vector, so a full copy per edit is cheaper than the
    // bookkeeping an undoable-command layer would need. Every structural edit calls pushUndo first.
    void pushUndo();
    void undo();
    void redo();
    bool canUndo() const { return !undoStack_.empty(); }
    bool canRedo() const { return !redoStack_.empty(); }

    // The structural edits as the TAB performs them: push undo, apply, remap the selection, mark
    // dirty. A test drives these to check the tab's own bookkeeping, not just the free functions'.
    void addChild(fmt::OcBtNodeKind kind);
    void deleteSelected();
    void reparentSelected(i32 newParent);
    void moveSelected(i32 delta);
    void setSelectedKind(fmt::OcBtNodeKind kind);
    void markDirty() { dirty_ = true; }

private:
    void loadFromDisk();

    std::string path_;
    fmt::OcBtData tree_;
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;
    i32 selected_ = 0;

    std::vector<fmt::OcBtData> undoStack_;
    std::vector<fmt::OcBtData> redoStack_;

#if AVER_WITH_IMGUI
    void drawTreeRow(i32 index);
    void drawDetails();
#endif
};

// Creates a behaviour-tree editor for a .ocbt, else nullptr.
std::unique_ptr<AssetEditor> makeBtEditor(const std::string& path);

} // namespace aver::editor
