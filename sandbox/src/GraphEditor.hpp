#pragma once
// The .ocgraph node editor: an ImGui canvas built on GraphEditorGeometry's pure layout / hit-test /
// link-rule / auto-layout core (see that header for why the split exists) and GraphNodeDefs's node
// descriptor table (the ONE place a node type is registered -- see that header's own comment).
//
// ============================================================================================
// EXACT HOOK for SandboxApp.cpp -- NOT edited here, that file is owned by another workflow.
// Three lines, all additive, none of them touch existing code:
//
//   1. Near the other editor includes (SandboxApp.cpp:42-44, alongside AssetEditor.hpp/
//      ActorEditor.hpp/AnimEditor.hpp):
//          #include "GraphEditor.hpp"
//
//   2. In onInit(Engine& e), appended AFTER the existing three registerFactory calls
//      (SandboxApp.cpp:514-516) -- append, not insert ahead of them: AssetEditorHost::open() is
//      first-match-wins by registration order, and none of the existing three claim ".ocgraph" so
//      order relative to them does not matter, but appending is the smallest, safest diff:
//          assetEditors_.registerFactory(&editor::makeGraphEditor);
//
//   3. In applyDpi(f32 dpi) (SandboxApp.cpp:384), right after the existing `dpi_ = dpi;` line:
//          editor::setGraphEditorDpi(dpi_);
//      This is needed because AssetEditor::draw(Engine&) carries no dpi parameter (confirmed by
//      reading AssetEditor.hpp -- AssetEditorHost::draw takes `dpi` itself to size the fallback
//      window, but never threads it into ed.draw()). ActorEditor.hpp already solves the identical
//      problem for its own app-owned config (content root, toolbar hooks) with a push-style setter
//      -- setActorEditorContentRoot / setActorEditorHooks -- rather than widening the AssetEditor
//      interface for one subclass. setGraphEditorDpi follows that exact precedent instead of
//      inventing a new mechanism.
//
// That's it. No Window-menu entry, no dock-layout slot, no bool visibility flag: AssetEditorHost
// already gives every asset tab docking, a dirty marker (the tab's unsaved-changes dot) and
// close-with-unsaved-warning for free -- see AssetEditor.hpp's own doc comments. Opening a .ocgraph
// file (double-click in the Content Browser, or --open-asset on the command line) routes to
// makeGraphEditor automatically once step 2 lands; nothing else needs to know the extension exists.
// ============================================================================================
#include "AssetEditor.hpp"
#include "GraphEditorGeometry.hpp"
#include "aver/formats/OcGraph.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace aver::editor {

// Pushes the app's current DPI scale into the graph editor. See the EXACT HOOK comment above for
// why this exists instead of a constructor/draw parameter. Read fresh every frame by every open
// GraphEditor tab (not captured once), so a live DPI change (e.g. dragging the window to a different
// monitor) takes effect on the next frame without needing every tab to be told individually.
void setGraphEditorDpi(float dpi);

// One open .ocgraph tab: canvas, selection, undo, and load/save. See GraphEditor.cpp for the load/
// save contract (byte-identical no-op round trip, unknown-record preservation, display-only
// auto-layout) -- all three are explained where they are implemented, not just here.
class GraphEditor final : public AssetEditor {
public:
    explicit GraphEditor(std::string path);

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    // ---- read access for the details panel and for headless tests ----------------------------------
    // The real data model -- read-only. Exposed (unlike graph_ itself) so GraphEditorLoadSaveTest can
    // inspect the result of an attribute edit through the exact same public surface draw()'s details
    // panel reads from, with no ImGui context required.
    const fmt::OcGraphData& graph() const { return graph_; }
    const std::vector<std::string>& selectedNodes() const { return selectedNodes_; }

    // Selects exactly `nodeId` (clearing any link selection), the same end state a canvas click on
    // that node reaches -- a no-op if `nodeId` does not name a node currently in the graph. Not an
    // "edit" (no pushUndo(), no dirty_): selection is display state, not data, exactly like
    // displayPos_. Public so a caller other than the canvas's own click handling can drive selection
    // -- SandboxApp's --open-asset/--graph-select test hook uses this to prove the details panel
    // renders a real, populated node without a human clicking the canvas.
    void selectNode(const std::string& nodeId);

    // ---- attribute editing (Gap B) -------------------------------------------------------------------
    // A selected node's NODE-line key=value attributes -- param=/field=/class= today, anything else
    // tomorrow. See GraphEditorGeometry.hpp's getNodeAttribute/setNodeAttribute/removeNodeAttribute for
    // the underlying order-preserving, unknown-survives contract; these two just add the same
    // pushUndo()/dirty_ bookkeeping every other edit path in this file already has (deleteSelection,
    // commitLink). PUBLIC, unlike those two, specifically so GraphEditorLoadSaveTest can prove the
    // save()/C#-compiles round trip without an ImGui context -- draw()'s details panel below calls
    // these exact same two methods a headless caller would. Both return false (no-op, no undo entry)
    // if `nodeId` does not name a node currently in the graph. setAttribute ALSO returns false, with no
    // edit applied, if `value` contains any whitespace: the NODE line is whitespace-tokenised with no
    // quoting on either the C++ writer or the C# reader side (OcGraph.cpp's writeOcgraph joins
    // extraTokens with a bare space; OcGraphParser.cs re-splits on whitespace), so a value containing a
    // space cannot round-trip -- it would silently re-split into extra bare tokens on the next load and
    // read back truncated to its first word. Refusing here is the same "not an edit" shape
    // clearAttribute already uses for a no-op target, rather than writing something the very next load
    // would read back differently.
    bool setAttribute(const std::string& nodeId, const std::string& key, const std::string& value);
    // Clears (deletes) the `key=...` token outright rather than writing `key=` -- see
    // GraphEditorGeometry.hpp's removeNodeAttribute comment for why. Returns false (no-op, no undo
    // entry) if the node doesn't exist OR the attribute wasn't set to begin with -- clearing something
    // already absent is not an edit.
    bool clearAttribute(const std::string& nodeId, const std::string& key);

private:
    // ---- identity / data model ---------------------------------------------------------------
    std::string path_;
    std::string originalText_; // on-disk text as of the last load/save; writeOcgraph's `existing`
                                // argument, so unrecognised records always survive (see .cpp).
    fmt::OcGraphData graph_;   // the real data model. Mutated ONLY by explicit edit actions, each
                               // preceded by pushUndo() -- never by layout/display code.
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;

    // Display-only node positions. Seeded from graph_'s own node.x/y at load, and overwritten by
    // auto-layout when every node loaded at (0,0) -- see runAutoLayoutIfUnpositioned() in the .cpp.
    // Deliberately kept apart from graph_: a load with no further edits, followed immediately by
    // save(), must stay byte-identical, and auto-layout must not silently invent position data the
    // user never asked to write. A node's entry here is committed into graph_.nodes[i].x/y only when
    // the user actually finishes dragging it (see the drag-commit code in the .cpp).
    std::unordered_map<std::string, Vec2> displayPos_;
    // Whether displayPos_ came from auto-layout rather than the file, and at what DPI it was
    // computed. Both are needed because node BOXES are sized at the current DPI while auto-layout
    // spacing is baked in once: compute the spacing at a different DPI from the drawing and every
    // column overlaps the next, which is what a 300%-DPI display did to the first version of this.
    // Dragging a window to a monitor with different scaling would do it again, hence the re-run.
    bool autoLaidOut_ = false;
    float autoLayoutDpi_ = 0.0f;

    // Pins this editor invented at load because the file did not spell them out, keyed
    // nodeId \x1f pinName \x1f isOutput. Stripped again before every save.
    //
    // WHY THE FILE CAN LACK THEM. The C# runtime derives a node's pin shape from its TYPE
    // (OcGraphParser.cs's AddDefaultPins), so a graph it wrote records PIN lines only where a pin
    // carries something extra -- a constant's value, say. Drone.ocgraph has 19 nodes and 7 PIN
    // records, while its 19 LINKs happily reference pins like orbit_angle.result that appear
    // nowhere in the file. The C++ reader models only what is written, so the editor saw nodes with
    // no pins and drew no wires between them.
    //
    // Synthesised for DISPLAY ONLY, and that half matters as much. Writing them back would add
    // forty-odd PIN records the author never wrote, so opening a graph and saving it would balloon
    // the file -- the same class of silent damage as dropping its comments.
    std::unordered_set<std::string> synthesizedPins_;
    void synthesizeMissingPins();
    static std::string pinKey(const std::string& nodeId, const std::string& pin, bool isOutput) {
        return nodeId + "\x1f" + pin + "\x1f" + (isOutput ? "1" : "0");
    }

    // ---- view state -----------------------------------------------------------------------------
    CanvasTransform view_;
    GraphLayoutStyle style_;
    std::vector<GraphNodeLayout> layouts_; // recomputed once at the top of every draw() call

    // ---- selection ------------------------------------------------------------------------------
    std::vector<std::string> selectedNodes_;
    int selectedLink_ = -1;

    // ---- undo (declared here, ahead of the interaction state below, because MoveNodes drag needs
    // the UndoState type for its lazily-pushed pending snapshot) ------------------------------------
    struct UndoState { fmt::OcGraphData graph; std::unordered_map<std::string, Vec2> displayPos; };
    std::vector<UndoState> undoStack_, redoStack_;
    void pushUndo();
    void undo();
    void redo();

    // ---- interaction state machine (left mouse button) -------------------------------------------
    enum class DragMode { None, PanCanvas, MoveNodes, BoxSelect, DrawLink };
    DragMode dragMode_ = DragMode::None;
    Vec2 dragStartScreen_{};       // canvas-local screen space (relative to the canvas child's origin)
    Vec2 dragStartCanvas_{};
    Vec2 panAnchorPx_{};           // view_.panPx at drag start, for PanCanvas
    std::unordered_map<std::string, Vec2> moveStart_;      // per-node displayPos_ at drag start
    bool moveUndoPushed_ = false;   // see .cpp: undo for a move is pushed lazily, only once real
                                     // movement crosses a small threshold, so a plain click-to-select
                                     // never pollutes the undo stack with a no-op entry
    UndoState pendingMoveSnapshot_; // pre-move state, captured at drag start, pushed onto undoStack_
                                     // only if moveUndoPushed_ becomes true
    Vec2 boxSelectCurrentCanvas_{};
    std::string linkDragFromNode_, linkDragFromPin_;
    bool linkDragFromIsOutput_ = false;

    // Feedback for a refused connection attempt -- shown for a few seconds rather than nothing, per
    // the task brief: a node editor that silently ignores a rejected connection is the single most
    // common complaint about these tools.
    std::string lastRejectMsg_;
    double lastRejectAtSec_ = -1000.0;

    // Right-click "add node" palette.
    Vec2 pendingSpawnCanvasPos_{};

    // ---- details panel (Gap B: attribute editing) -------------------------------------------------
    // Which attribute InputText, if any, is mid-edit right now -- "" \x1f key when nothing is active.
    // Only ONE field can hold ImGui keyboard focus at a time, so a single slot (not a per-row map) is
    // enough: it exists so setAttribute()/clearAttribute() -- and the pushUndo() inside them -- fire
    // ONCE per edit SESSION (on IsItemDeactivatedAfterEdit), not once per keystroke, the same
    // activate/apply-live/deactivate shape SandboxApp.cpp's own transform DragFloat3 fields use for
    // their own undo boundary. Live keystrokes are held in attrEditBuf_ only; graph_ is untouched until
    // the field is deactivated.
    std::string attrEditRowKey_;
    char attrEditBuf_[512] = {};

    // ---- helpers (implemented in the .cpp, next to the input handling that uses them) -------------
    void loadFromDisk();
    void runAutoLayoutIfUnpositioned();
    void recomputeLayouts(float dpi);
    std::string makeUniqueNodeId(const std::string& typeId) const;
    void deleteSelection();
    void reportLinkRejection(const GraphLinkCheck& check);
    void commitLink(const std::string& srcNode, const std::string& srcPin,
                     const std::string& dstNode, const std::string& dstPin);
};

// Creates the .ocgraph editor tab, or nullptr for any other extension. Registered via the EXACT
// HOOK above.
std::unique_ptr<AssetEditor> makeGraphEditor(const std::string& path);

} // namespace aver::editor
