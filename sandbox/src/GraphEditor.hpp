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

    // Forces the Viewport tab to the front on the next draw, exactly as clicking it would.
    //
    // Public for the same reason selectNode is (see its comment below): a capture run needs to
    // prove the component tree DRAWS, and an inner tab is not reachable from the command line any
    // other way. Display state, so no pushUndo() and no dirty_ -- and one-shot, so a human who
    // then clicks Event Graph is not fought with every frame afterwards.
    void showViewportTab() { forceViewportTab_ = true; }

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

    // ---- variable editing (graph-level VAR declarations) -------------------------------------------
    // THE BUG THIS EXISTS TO CLOSE: dragging SetVar out of the palette produced a `var=` attribute
    // with no picker, no validation, and no way to declare the variable it named -- so the freshly
    // spawned node could not compile (Graph.Validate() refuses a SetVar/GetVar naming an undeclared
    // variable) and the editor gave no sign anything was wrong. These five methods, plus draw()'s
    // Variables panel and its var= picker (both ImGui, both calling straight into these), are what
    // give that path somewhere useful to end: declare on the spot, or say exactly what's wrong.
    //
    // Same public-surface shape as setAttribute/clearAttribute above, and for the identical reason:
    // GraphEditorLoadSaveTest exercises these directly, with no ImGui context, so the load/save
    // contract (byte-identical no-op round trip, an edit landing on disk through the real save() path)
    // is proven against the exact calls the details panel makes, not a hand-simulated approximation of
    // them.
    const std::vector<fmt::OcGraphVariable>& variables() const { return graph_.variables; }
    const std::vector<fmt::OcGraphComponent>& components() const { return graph_.components; }
    const std::string& selectedComponent() const { return selectedComponent_; }

    // ---- component tree edits ---------------------------------------------------------------------
    // Each pushes undo and sets dirty_, exactly like the variable edits below, and each is PUBLIC
    // for the same reason those are: every one of them can produce a file the parser then refuses
    // to open -- a duplicate id, a child orphaned by a delete, a parent cycle made by a reparent --
    // and the only honest way to know they do not is to drive them headlessly and reload the
    // result. That is GraphEditorLoadSaveTest, which has no ImGui context at all.

    // Adds one component of `kind`, id derived from the kind and made unique, parented to whatever
    // is currently selected. Selects it.
    void addComponent(const std::string& kind);
    // Deletes `id` AND EVERYTHING UNDER IT. A child left behind would name a parent that no longer
    // exists, which the parser refuses -- deleting one component would make the file unopenable.
    void deleteComponentSubtree(const std::string& id);
    // Reparents `id` under `parentId` (empty = the actor's own entity). A no-op when `parentId` is
    // `id` or below it, since that would be a cycle.
    void setComponentParent(const std::string& id, const std::string& parentId);
    // Sets one key=value on a component; an empty value removes the key. Refuses a value containing
    // whitespace, for the reason setAttribute's own comment gives -- this format has no quoting.
    void setComponentAttribute(const std::string& id, const std::string& key, const std::string& value);
    // Renames a component, rewriting every child's parent= to match. Refuses an empty name, a name
    // with whitespace, and a name already taken.
    void renameComponent(const std::string& id, const std::string& newId);
    // True when `maybeAncestor` is `id` itself or anywhere above it.
    bool componentIsAncestorOf(const std::string& maybeAncestor, const std::string& id) const;
    // The component's world matrix, walking its parent chain. Identity for an unknown id.
    void componentWorldMatrix(const std::string& id, float out[16]) const;

    // Declares a new variable. No-op (false, no edit) if `name` is empty, contains whitespace (a VAR
    // name is a bare token on the NODE-line-adjacent VAR line -- same "this format has no quoting"
    // constraint setAttribute's own value guard already enforces), or a variable named `name` already
    // exists: Graph.Validate()'s own "Check VAR declarations are unique" block would reject the
    // duplicate at C# compile time regardless, so refusing here catches it immediately rather than
    // only once someone tries to compile the saved file. `type` falls back to "float" (the palette's
    // own GetVar/SetVar default) if it is not one of float/int/bool -- see isValidVarType in the .cpp.
    bool addVariable(const std::string& name, const std::string& type, const std::string& defaultValue);

    // Renames `oldName` to `newName`, AND rewrites every node's `var=` attribute that currently names
    // `oldName` to name `newName` instead, in the SAME undo step (one Ctrl+Z undoes the whole rename,
    // references included) -- see the .cpp for why a rename that leaves stale references is refused
    // outright rather than silently shipped. No-op (false, no edit) if oldName isn't declared, newName
    // is empty or contains whitespace, or newName already names a DIFFERENT declared variable (a
    // same-name "rename" is accepted as a harmless no-op, not a collision). True but a genuine no-op
    // when newName == oldName.
    bool renameVariable(const std::string& oldName, const std::string& newName);

    // Changes the declared type of `name` (falls back to "float" like addVariable if `newType` isn't
    // float/int/bool). Deliberately does NOT touch any node's pins -- see the .cpp for why a resulting
    // variable/pin type mismatch is left visible (the Variables panel's own mismatch note, computed
    // fresh every frame from the live pins) rather than silently patched. No-op (false) if `name`
    // isn't declared. True but a no-op if newType already matches.
    bool retypeVariable(const std::string& name, const std::string& newType);

    // Changes the declared default (the literal text after the type in `VAR name type default`). No
    // format validation here -- same division of labour OcGraphParser.cs's own VAR-parsing comment
    // describes: an unparseable default is not this layer's problem, it falls back to the type's zero
    // value on the C# side. No-op (false) if `name` isn't declared or `defaultValue` contains
    // whitespace (same guard as setAttribute's value, and for the identical reason).
    bool setVariableDefault(const std::string& name, const std::string& defaultValue);

    // Deletes the variable named `name`. REFUSES (false, no edit) if any node's `var=` attribute
    // still names it -- silently deleting it would leave those nodes referencing a variable that no
    // longer exists, an uncompilable graph with no editor-visible symptom until someone runs the C#
    // compiler, which is exactly the "silently orphaned" failure mode the task brief calls worse than
    // refusing. `outBlockedBy`, if non-null, receives every referencing node id on refusal (untouched
    // on success) so a caller can name them without re-deriving the search. The refusal is also
    // surfaced through the same rejection banner mechanism a rejected link connection uses (see
    // showRejectionBanner below).
    bool deleteVariable(const std::string& name, std::vector<std::string>* outBlockedBy = nullptr);

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

    // ---- the two tab bodies ---------------------------------------------------------------------
    // draw() is the tab bar; these are what it switches between. See draw() for why the split.
    void drawEventGraph(float dpi);
    void drawViewport(Engine& e, float dpi);

    // The Viewport tab's pieces, in the order they draw.
    void drawComponentToolbar(float dpi);
    void drawComponentTree(float dpi);
    void drawComponentTreeNode(const std::string& id, float dpi);
    void drawComponentDetails(float dpi);
    void buildComponentPreview(Engine& e);


    // ---- component tree state -------------------------------------------------------------------
    std::string selectedComponent_;   // by id; empty = nothing selected
    bool forceViewportTab_ = false;   // one-shot, cleared the frame it is honoured
    // In-flight text edit for a component field, keyed id \x1f key, mirroring attrEditRowKey_.
    std::string compEditRowKey_;
    char compEditBuf_[256] = {};
    // Whether the shared preview has been pointed at this tree yet. Reset by any edit that
    // changes where things are, so adding a component that lands off-screen still gets framed.
    bool previewFramed_ = false;

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

    // ---- interaction state machine -----------------------------------------------------------------
    // (NOT "left mouse button" despite the field names below being button-agnostic: PanCanvas has
    // always also fired on Middle-drag, and now on Right-drag too -- see rightButtonPan_.)
    enum class DragMode { None, PanCanvas, MoveNodes, BoxSelect, DrawLink };
    DragMode dragMode_ = DragMode::None;
    Vec2 dragStartScreen_{};       // canvas-local screen space (relative to the canvas child's origin)
    Vec2 dragStartCanvas_{};
    Vec2 panAnchorPx_{};           // view_.panPx at drag start, for PanCanvas
    // Right-button DRAG now pans (see draw()'s start-interaction block) but a right-button CLICK must
    // still open the Add Node popup -- that's existing, muscle-memory behaviour this task was told
    // explicitly not to remove. The two are indistinguishable at mouse-DOWN, so PanCanvas starts
    // immediately for a zero-latency drag feel (matching Middle-drag), and this flag marks that THIS
    // particular PanCanvas run needs a click-vs-drag verdict on release -- Space+Left and Middle never
    // set it, because neither of them has a competing "click" meaning to fall back to.
    bool rightButtonPan_ = false;
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

    // ---- Variables panel edit state -- identical activate/apply-live/deactivate shape attrEditRowKey_/
    // attrEditBuf_ use just above, applied to the panel's name/default text fields (its type field is a
    // Combo, which has no comparable "live keystroke" state to buffer -- a selection either fires or it
    // doesn't). Keyed "field\x1fvariableName" (e.g. "varname\x1fscore") rather than by row index: an
    // edit that deletes or reorders a variable mid-session must not have some OTHER row inherit an
    // in-flight edit buffer it never asked for.
    std::string varEditRowKey_;
    char varEditBuf_[256] = {};

    // ---- helpers (implemented in the .cpp, next to the input handling that uses them) -------------
    void loadFromDisk();
    void runAutoLayoutIfUnpositioned();
    void recomputeLayouts(float dpi);
    std::string makeUniqueNodeId(const std::string& typeId) const;
    std::string makeUniqueVariableName(const std::string& base) const;
    void deleteSelection();
    // Sets lastRejectMsg_/lastRejectAtSec_ and logs -- the shared plumbing behind the on-canvas
    // rejection banner. reportLinkRejection (below) is one caller; deleteVariable's own refusal is
    // another, added alongside it rather than growing a second, near-duplicate banner mechanism.
    void showRejectionBanner(const std::string& msg);
    void reportLinkRejection(const GraphLinkCheck& check);
    void commitLink(const std::string& srcNode, const std::string& srcPin,
                     const std::string& dstNode, const std::string& dstPin);
};

// Creates the .ocgraph editor tab, or nullptr for any other extension. Registered via the EXACT
// HOOK above.
std::unique_ptr<AssetEditor> makeGraphEditor(const std::string& path);

} // namespace aver::editor
