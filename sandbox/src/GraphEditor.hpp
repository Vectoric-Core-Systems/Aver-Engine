#pragma once
// The .ocgraph node editor: an ImGui canvas over GraphEditorGeometry's layout/hit-test/link-rule/
// auto-layout core and GraphNodeDefs's node descriptor table (the one place a node type is registered).
//
// EXACT HOOK for SandboxApp.cpp -- NOT edited here (owned by another workflow). Three additive lines:
//   1. SandboxApp.cpp:42-44, with the other editor includes: #include "GraphEditor.hpp"
//   2. onInit(Engine&), appended AFTER the existing three registerFactory calls (SandboxApp.cpp:
//      514-516; append not insert -- AssetEditorHost::open() is first-match-wins and none of the
//      three claim ".ocgraph"): assetEditors_.registerFactory(&editor::makeGraphEditor);
//   3. applyDpi(f32 dpi) (SandboxApp.cpp:384), right after `dpi_ = dpi;`: editor::setGraphEditorDpi(dpi_);
//      Needed because AssetEditor::draw(Engine&) takes no dpi (AssetEditorHost::draw takes one itself,
//      for the fallback window, but never threads it into ed.draw()). Same push-style-setter precedent
//      as ActorEditor's setActorEditorContentRoot/setActorEditorHooks, not a widened interface.
//
// Nothing else needed: no Window-menu entry, no dock-layout slot, no bool visibility flag -- AssetEditorHost
// already gives docking, a dirty marker and close-with-unsaved-warning; opening a .ocgraph (Content
// Browser or --open-asset) routes to makeGraphEditor once step 2 lands.
#include "AssetEditor.hpp"
#include "EditorWidgets.hpp"
#include "GraphEditorGeometry.hpp"
#include "SnapshotUndo.hpp"
#include "aver/formats/OcGraph.hpp"

#include <cstdint>
#include <filesystem>   // schemeFileWriteTime_ -- see its own comment below
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Forward-declared, not included: only one method below needs PreviewDraw (by reference), and
// including ActorPreview.hpp would drag the renderer's preview tier into every TU that edits a graph.
namespace aver::render::preview { struct PreviewDraw; }

namespace aver::editor {

// Forward-declared (legal since the enum fixes its underlying type) rather than including
// GraphNodeDefs.hpp, which would drag the ~250-entry catalog (a function-local static vector) into
// every including TU.
enum GraphNodeDomain : std::uint32_t;

// Pushes app DPI into the editor (see EXACT HOOK above). Read fresh every frame by every open tab,
// so a live DPI change (e.g. moving to another monitor) takes effect without notifying tabs individually.
void setGraphEditorDpi(float dpi);

// The shape the material graph's Viewport tab previews it on. Sphere shows every normal-to-view
// angle at once; Cube/Plane/Cylinder matter for a graph whose look depends on flat faces or a seam.
enum class MaterialPreviewShape : u8 { Sphere = 0, Cube = 1, Plane = 2, Cylinder = 3 };

// One open .ocgraph tab: canvas, selection, undo, and load/save. See GraphEditor.cpp for the load/
// save contract (byte-identical no-op round trip, unknown-record preservation, display-only
// auto-layout) -- all three are explained where they are implemented, not just here.
class GraphEditor final : public AssetEditor {
public:
    explicit GraphEditor(std::string path);

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    bool usesSharedPreview() const override { return true; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    // Restores the canvas/details ("Gap B") split to its default width and persists it -- a WIDTH
    // (like ActorEditor's own columns), not the fraction most editors' splits use (details column is
    // a near-constant width; see EditorWidgets.hpp). No-op when AVER_WITH_IMGUI is off, matching
    // draw()'s headless branch.
    void resetLayout() override;

    // ---- read access for the details panel and for headless tests ----------------------------------
    // Read-only view of graph_, exposed so GraphEditorLoadSaveTest can inspect edits through the same
    // surface draw()'s details panel uses, with no ImGui context.
    const fmt::OcGraphData& graph() const { return graph_; }
    const std::vector<std::string>& selectedNodes() const { return selectedNodes_; }

    // Forces the Viewport tab to front on the next draw. Public so a capture run can prove the
    // component tree draws (an inner tab is unreachable from the command line otherwise). Display
    // state only (no pushUndo/dirty_); one-shot so it doesn't fight a human who then clicks Event Graph.
    void showViewportTab() { forceViewportTab_ = true; }

    // Selects exactly `nodeId` (clearing link selection); no-op if it names no node in the graph.
    // Display state only (no pushUndo/dirty_). Public so --graph-select can drive selection headlessly.
    // Returns whether it selected anything -- not decoration: this was void until --graph-select logged
    // "selected node 'write'" for a graph with no such node, so a capture run "verified" nothing and
    // said it had. A hook that cannot fail cannot verify.
    bool selectNode(const std::string& nodeId);

    // Selects several nodes at once (selectNode only ever replaces with one) -- needed to test
    // copy/paste's link remapping. Unknown ids are dropped, not refused: a caller wants the rest of
    // its selection, not nothing.
    void selectNodes(const std::vector<std::string>& nodeIds);

    // Exposes undo/redo so a headless test can check paste is one undo step, not one per node.
    void undoForTest() { undo(); }
    void redoForTest() { redo(); }

    // ---- attribute editing (Gap B) -------------------------------------------------------------------
    // A selected node's NODE-line key=value attributes (param=/field=/class=/...). See
    // GraphEditorGeometry.hpp's getNodeAttribute/setNodeAttribute/removeNodeAttribute for the order-
    // preserving, unknown-survives contract; these add the usual pushUndo()/dirty_ bookkeeping, and are
    // PUBLIC so GraphEditorLoadSaveTest can drive the save() round trip with no ImGui context -- the
    // exact two methods draw()'s details panel itself calls. Both
    // return false (no-op) if `nodeId` is unknown. setAttribute also refuses (false, no edit) a `value`
    // containing whitespace: the NODE line is whitespace-tokenised with no quoting on either side
    // (OcGraph.cpp writeOcgraph / OcGraphParser.cs), so a space would silently truncate on next load.
    bool setAttribute(const std::string& nodeId, const std::string& key, const std::string& value);
    // Deletes the `key=...` token outright (see removeNodeAttribute in GraphEditorGeometry.hpp).
    // Returns false (no-op) if the node doesn't exist or the attribute wasn't set.
    bool clearAttribute(const std::string& nodeId, const std::string& key);

    // ---- variable editing (graph-level VAR declarations) -------------------------------------------
    // Closes a real bug: dragging SetVar from the palette produced a `var=` naming an undeclared
    // variable, which Graph.Validate() refuses at compile (SetVar or GetVar) with no editor-visible
    // sign. These five methods (plus draw()'s Variables panel and var= picker) let the author declare
    // on the spot instead.
    // Same public-surface shape as setAttribute/clearAttribute, for the same reason: GraphEditorLoadSaveTest
    // drives the load/save round trip through these exact calls with no ImGui context.
    const std::vector<fmt::OcGraphVariable>& variables() const { return graph_.variables; }
    const std::vector<fmt::OcGraphComponent>& components() const { return graph_.components; }
    const std::string& selectedComponent() const { return selectedComponent_; }

    // Adds one node of `typeId` at `canvasPos` with its default pins, plus an ENTRY record if it's
    // an Event-category node. Returns the new id, or empty if the type is unknown. Lifted out of the
    // (ImGui-only) palette popup so a headless test can ask "does adding an On Tick actually run" --
    // same ImGui-glue/model-in-class split as the Variables panel and component tree.
    std::string addNodeFromCatalog(const std::string& typeId, Vec2 canvasPos);

    // ---- drag-a-wire-into-empty-space -------------------------------------------------------
    // Releasing a link drag on empty canvas opens the palette filtered to accepting node types and
    // connects whichever is picked (a Blueprint-style gesture; the wire used to just vanish). Modelled
    // as three ImGui-free public methods so a test can arm it, query candidates, pick one, check the link.

    // Arms the gesture. `fromIsOutput` says which end of the wire `fromPin` is, deciding whether
    // candidates are searched for a matching INPUT or OUTPUT.
    void beginLinkDrop(const std::string& fromNode, const std::string& fromPin, bool fromIsOutput);
    void cancelLinkDrop() { linkDropPending_ = false; }
    bool linkDropPending() const { return linkDropPending_; }

    // Would this node type accept the pending wire? False for every type when nothing is pending.
    // Uses the same compatibility predicate commitLink does (exact match for gameplay graphs,
    // widening for material graphs), so the
    // palette can never offer a node whose link would then be refused.
    bool linkDropAccepts(const struct GraphNodeDesc& desc) const;   // GraphNodeDefs.hpp

    // ---- validation, through the managed validator ------------------------------------------
    // Graph.Validate()/OcGraphParser carry ~30 named errors the editor never called (C# vs. C++, no
    // channel between them; an author's first sight was the engine log, if they looked). Supplied as
    // a hook, not a direct ScriptHost call, so this file keeps its existing dependencies (Core +
    // Formats + ImGui) and stays testable with no .NET runtime. Unset
    // means validateNow() reports unavailable rather than claiming valid.
    using ValidateFn = std::function<bool(const std::string& ocgraphText, std::string& err)>;
    void setValidator(ValidateFn fn) { validate_ = std::move(fn); }
    bool validatorInstalled() const { return static_cast<bool>(validate_); }

    // ---- execution highlighting --------------------------------------------------------------
    // Which nodes of THIS graph ran recently, and how long ago. Hook, same reason as the validator.
    // Called at most once/frame by a drawing tab. Armed ONCE at editor start (SandboxApp), not per
    // tab: the recording is keyed by graph NAME, and a static bool test when off is cheaper than
    // tracking tab lifetimes. A packaged game never arms it.
    using NodeHitsFn = std::function<void(const std::string& graphName, f32 maxAgeSeconds,
                                          std::vector<std::pair<std::string, f32>>& out)>;
    void setNodeHitSource(NodeHitsFn fn) { nodeHits_ = std::move(fn); }
    bool nodeHitSourceInstalled() const { return static_cast<bool>(nodeHits_); }

    // How long a hit keeps a node lit. Long enough to see a once-per-second event, short enough that
    // a node which stopped running goes dark while you are still looking at it.
    static constexpr f32 kNodeHitFadeSec = 1.5f;

    // Validates what's ON THE CANVAS (serialises the live graph, not the file) so the answer matches
    // what the author sees. False: `err` is the validator's message, `offendingNode` its named node if resolved.
    bool validateNow(std::string& err, std::string& offendingNode);

    // The node id a validator message names, or empty. Public/static because it's a heuristic over
    // prose worth testing alone: reads the first single-quoted token and returns it only if it names
    // a real node in `g` -- a pin/event/parameter quote yields nothing, so a wrong guess can't highlight an innocent node.
    static std::string errorNodeId(const std::string& message, const fmt::OcGraphData& g);

    // Exactly what save() would write. Public so a test can compare the two.
    std::string serializeForSave() const;

    // Spawns `typeId` at `canvasPos`, wires the pending drop to its first accepting pin, and disarms
    // the gesture either way. Returns the new id, or empty if unknown. With nothing pending, exactly addNodeFromCatalog.
    std::string spawnAndConnectLinkDrop(const std::string& typeId, Vec2 canvasPos);

    // Which node vocabulary the graph belongs to, read from its own DOMAIN record -- the same answer
    // the compiler gives. Deliberately the only notion of domain in the editor; a disagreeing one
    // would offer nodes that then fail to compile.
    GraphNodeDomain openGraphDomain() const;

    // The Viewport tab's content for a MATERIAL graph: materialPreviewShape_ shaded by this graph.
    // False when there's nothing to draw (no path yet, or the graph doesn't currently compile).
    bool buildMaterialPreview(Engine& e, render::preview::PreviewDraw& out);

    // The whole Viewport tab for a material graph -- the preview shape and its compile state, with
    // none of the component furniture a material graph cannot use.
    void drawMaterialViewport(Engine& e, float dpi);

    // Which material-graph id the preview shape is shading with, and the edit count it was compiled
    // at. Kept apart so a graph that stops compiling mid-edit keeps showing the last one that did.
    u32 materialPreviewGraphId_ = 0;
    i64 materialPreviewDirtyMark_ = -1;
    // Which primitive the Viewport tab's shape dropdown currently shows the graph on.
    MaterialPreviewShape materialPreviewShape_ = MaterialPreviewShape::Sphere;

    // ---- functions ----------------------------------------------------------------------------
    // A .ocgraph holds ONE event graph plus named FUNCTIONS, sharing a file, node-id namespace and
    // coordinate space but not a canvas: one subgraph shown at a time, no wire crosses between them
    // (Validate refuses it -- they compile to separate methods).

    // Which subgraph the canvas shows. Empty means the event graph, the same convention `func=`
    // uses (absent = not in a function) -- one convention across model and editor, not a "" / "<none>" / null triple.
    const std::string& currentSubgraph() const { return currentSubgraph_; }
    void setCurrentSubgraph(const std::string& funcName);

    const std::vector<fmt::OcGraphFunction>& functions() const { return graph_.functions; }

    // Declares a function and lays down the nodes it can't exist without: FuncEntry always, FuncReturn
    // once it has an output. Returns the uniquified name, or empty if unusable. Creating FuncEntry is
    // not a convenience -- a function with none fails Validate by name, so a "New Function" button
    // that wrote only the FUNC record would hand back a broken graph with no clue which of the three
    // node types fixes it (the same On Tick/ENTRY lesson the palette already learned).
    std::string addFunction(const std::string& name);

    bool renameFunction(const std::string& oldName, const std::string& newName);

    // Deletes the function, its nodes and their links. Refuses (false) while any CallFunc still
    // names it -- same rule/reason as deleteVariable: an orphaned call fails to compile invisibly.
    bool deleteFunction(const std::string& name, std::vector<std::string>* outBlockedBy = nullptr);

    // Adds or removes one argument / one return. Both rebuild the affected nodes' pins, because a
    // FuncEntry's outputs ARE the function's inputs -- see the .cpp.
    bool addFunctionPin(const std::string& funcName, bool isInput, const std::string& pinName, const std::string& type);
    bool removeFunctionPin(const std::string& funcName, bool isInput, const std::string& pinName);
    bool setFunctionPure(const std::string& funcName, bool pure);

    // Drops a CallFunc node calling `funcName`. Separate from addNodeFromCatalog because a call node
    // has no fixed pin shape -- it takes the callee's, which the catalog cannot know.
    std::string addCallNode(const std::string& funcName, Vec2 canvasPos);

    // ---- comment boxes ------------------------------------------------------------------------
    // Groups nodes visually and says why they're wired that way, with no pins/links/graph effect
    // (fmt::OcGraphComment). Same public surface as addNodeFromCatalog/deleteSelection, for the same
    // reason: the gestures live behind an ImGui popup, so nothing else could test add/move/save/reload.

    // Adds a box spanning `a`..`b` (either corner order) titled `text`; returns its id. A degenerate
    // rectangle is grown to a usable minimum rather than refused (invisible+unclickable is a dead end).
    std::string addComment(Vec2 a, Vec2 b, const std::string& text);

    // ---- framing and layout -------------------------------------------------------------------
    // Closes: graphs used to open showing whatever sat at canvas (0,0) -- for every graph in this
    // repo, a column of Const nodes and nothing else -- with content off-screen at an unchosen zoom
    // and no gesture to reach it.

    // The canvas-space bounding box of every node and comment box. False when the graph is empty
    // (caller must not treat {0,0}..{0,0} as content). Takes a dpi and recomputes layouts (same
    // reason as nodesInsideComment: a node box has no size without one), so this is callable outside a frame.
    bool contentBounds(float dpi, Vec2* outMin, Vec2* outMax);

    // Same, restricted to the current selection -- selected nodes, or the selected comment box.
    // False when nothing is selected.
    bool selectionBounds(float dpi, Vec2* outMin, Vec2* outMax);

    // Points the view at the whole graph (frameAll) or the selection, falling back to the whole graph
    // when nothing is selected (frameSelection). `viewportPx` is the canvas child's real-pixel size.
    // No-op on an empty graph. View-only: touches neither graph_ nor displayPos_, so never dirties.
    void frameAll(Vec2 viewportPx, float dpi);
    void frameSelection(Vec2 viewportPx, float dpi);

    // Re-runs the layered auto-layout over the whole graph and COMMITS it into graph_ as one undoable, dirtying
    // edit. The difference from runAutoLayoutIfUnpositioned(), which is display-only (it fires on
    // load, and load-then-save must stay byte-identical): this is a button the author pressed, so
    // writing the positions is the point -- it's what keeps the graph tidy next time it opens.
    // Returns false, no change, on an empty graph.
    bool applyAutoLayout(float dpi);

    // Adds a box enclosing every selected node plus a margin (the C-key gesture). Empty/no-op when
    // selection is empty. Takes a dpi and recomputes layouts itself rather than reading draw()'s
    // layouts_ -- a node box has no size without one, and depending on the last frame would make this
    // callable only inside a frame. Recompute is idempotent, one pass over the nodes.
    std::string addCommentAroundSelection(float dpi);

    bool deleteComment(const std::string& id);
    bool setCommentText(const std::string& id, const std::string& text);
    bool setCommentColor(const std::string& id, int r, int g, int b);
    const std::vector<fmt::OcGraphComment>& comments() const { return graph_.comments; }
    const std::string& selectedComment() const { return selectedComment_; }

    // Every node whose layout box sits ENTIRELY inside the comment -- decided by geometry each call,
    // not stored: a remembered membership list would disagree with the screen after any drag.
    std::vector<std::string> nodesInsideComment(const std::string& id, float dpi);

    // Deletes every selected node/link, plus the ENTRY/OUT records naming them -- otherwise dangling
    // references the parser refuses on next load. Public for the same reason as addNodeFromCatalog.
    void deleteSelection();

    // Removes every link touching a selected node, keeping the nodes (previously required clicking
    // each wire by hand). A method, not popup-inline code, so a headless test can drive it. One undo
    // step for however many wires -- pushes its own undo rather than leaving that to a caller.
    void breakLinksOnSelection();

    // True when at least one link touches a selected node -- what greys out the Break Links item so
    // it cannot be a no-op that still costs an undo step.
    bool selectionHasLinks() const;

    // ---- copy / paste / duplicate ---------------------------------------------------------------
    // A paste is exactly a palette drop of the same nodes plus their internal links -- deliberate,
    // so it invents no new semantics: a pasted Event node gets an ENTRY record the same way
    // addNodeFromCatalog does, a CustomEvent gets a fresh unique name the same way, and two OnTicks
    // are already legal so paste creates no state the palette couldn't. Only links with BOTH ends in
    // the copied set come along -- re-pointing a dangling link at the original would wire the copy
    // into what it was copied from.
    void copySelection();
    void pasteClipboard(Vec2 canvasPos);
    void duplicateSelection();
    bool clipboardEmpty() const { return clipNodes_.empty(); }

    // ---- component tree edits ---------------------------------------------------------------------
    // Each pushes undo/sets dirty_ and is PUBLIC, like the variable edits below: each could produce
    // a file the parser refuses (duplicate id, orphaned child, parent cycle), and the only honest
    // check is driving them headlessly through GraphEditorLoadSaveTest and reloading the result.

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

    // Declares a new variable. No-op (false) if `name` is empty, has whitespace (VAR is a bare token,
    // same no-quoting constraint as setAttribute's value guard), or already exists (Validate's own
    // "Check VAR declarations are unique" block would reject it anyway; refusing here catches it
    // before a compile attempt). `type` falls back to "float" (the palette's own GetVar/SetVar
    // default) if not float/int/bool -- see isValidVarType in the .cpp.
    bool addVariable(const std::string& name, const std::string& type, const std::string& defaultValue);

    // Renames `oldName` to `newName` AND rewrites every node's `var=` reference, in the SAME undo
    // step (one Ctrl+Z undoes both -- see .cpp for why a rename leaving stale references is refused
    // outright). No-op (false) if oldName isn't declared, newName is empty/whitespace, or newName
    // already names a DIFFERENT variable. True, no-op, when newName == oldName.
    bool renameVariable(const std::string& oldName, const std::string& newName);

    // Changes the declared type of `name` (falls back to "float" like addVariable). Deliberately
    // does NOT touch node pins -- a resulting mismatch stays visible via the Variables panel's own
    // note (computed fresh every frame from the live pins) rather than being silently patched (see
    // .cpp). False if `name` isn't declared; true no-op if newType matches.
    bool retypeVariable(const std::string& name, const std::string& newType);

    // Changes the declared default (the text after type in `VAR name type default`). No format
    // validation -- an unparseable default falls back to the type's zero value on the C# side (see
    // OcGraphParser.cs). No-op if `name` isn't declared or `defaultValue` has whitespace.
    bool setVariableDefault(const std::string& name, const std::string& defaultValue);

    // Deletes variable `name`. Refuses (false) if any node's `var=` still names it -- silently
    // orphaning that reference fails to compile with no editor-visible symptom otherwise. `outBlockedBy`,
    // if non-null, receives the referencing node ids on refusal (untouched on success). Refusal also
    // surfaces through the same rejection banner a rejected link uses (showRejectionBanner below).
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

    // Display-only node positions, seeded from graph_'s node.x/y at load and overwritten by
    // auto-layout when everything loaded at (0,0) -- see runAutoLayoutIfUnpositioned(). Kept apart
    // from graph_ so a load-then-save stays byte-identical; committed into graph_.nodes[i].x/y only
    // when a drag finishes (see the .cpp).
    std::unordered_map<std::string, Vec2> displayPos_;
    // Whether displayPos_ came from auto-layout, and at what DPI. Needed because node boxes are
    // sized at the current DPI while layout spacing is baked in once -- a DPI mismatch overlapped
    // columns on a 300%-DPI display; moving to a differently-scaled monitor re-triggers the same, hence the re-run.
    bool autoLaidOut_ = false;
    float autoLayoutDpi_ = 0.0f;

    // Pins invented at load because the file didn't spell them out, keyed nodeId\x1fpinName\x1fisOutput.
    // Stripped before every save. The C# runtime derives pin shape from node TYPE (OcGraphParser.cs's
    // AddDefaultPins), so files only record PIN lines with extra data (e.g. Drone.ocgraph: 19 nodes,
    // 7 PIN records, 19 LINKs referencing pins that appear nowhere) -- the C++ reader models only
    // what's written, so links had nothing to draw. Display-only: writing these back would add
    // forty-odd PIN records the author never wrote and balloon the file.
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
    // Fills `out` with a Fluid component's seed shell, or false if nothing to draw. See the
    // definition for why the preview shows the seed shell (not the simulated surface) at the
    // component matrix, not identity.
    bool buildFluidPreviewMesh(Engine& e, const fmt::OcGraphComponent& c,
                               render::preview::PreviewDraw& out);


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

    // The canvas/details ("Gap B") divider width, in DPI-independent pixels (ActorEditor's convention,
    // not SplitPane's fraction; see resetLayout() above). <= 0 means "not yet seeded", matching
    // ActorEditor's g_leftColW/g_rightColW. Not gated on AVER_WITH_IMGUI so the class stays headless-
    // compilable (GraphEditorLoadSaveTest), even though only drawEventGraph() and resetLayout() touch it.
    f32 detailsColW_ = 0.0f;

    // ---- selection ------------------------------------------------------------------------------
    std::vector<std::string> selectedNodes_;
    int selectedLink_ = -1;

    // ---- undo (declared ahead of the interaction state below, because MoveNodes drag needs
    // UndoState for its lazily-pushed pending snapshot) ------------------------------------------
    // Via the shared SnapshotUndo<State> template (SnapshotUndo.hpp); Sound/BtEditor migrated to it too.
    struct UndoState { fmt::OcGraphData graph; std::unordered_map<std::string, Vec2> displayPos; };
    SnapshotUndo<UndoState> history_;
    void pushUndo();
    void undo();
    void redo();

    // ---- interaction state machine -----------------------------------------------------------------
    // (NOT "left mouse button" despite the field names below being button-agnostic: PanCanvas has
    // always also fired on Middle-drag, and now on Right-drag too -- see rightButtonPan_.)
    enum class DragMode { None, PanCanvas, MoveNodes, BoxSelect, DrawLink, MoveComment, ResizeComment };
    DragMode dragMode_ = DragMode::None;
    Vec2 dragStartScreen_{};       // canvas-local screen space (relative to the canvas child's origin)
    Vec2 dragStartCanvas_{};
    Vec2 panAnchorPx_{};           // view_.panPx at drag start, for PanCanvas
    // Right-button DRAG pans, but a right-button CLICK must still open Add Node (existing behaviour,
    // kept deliberately). Indistinguishable at mouse-DOWN, so PanCanvas starts immediately (zero-
    // latency, matching Middle-drag) and this flag marks that release needs a click-vs-drag verdict --
    // Space+Left and Middle never set it; neither has a competing click meaning.
    bool rightButtonPan_ = false;
    std::unordered_map<std::string, Vec2> moveStart_;      // per-node displayPos_ at drag start
    bool moveUndoPushed_ = false;   // undo for a move is pushed lazily (only once movement crosses a
                                     // threshold), so a click-to-select doesn't pollute the undo stack
    UndoState pendingMoveSnapshot_; // pre-move state, captured at drag start, pushed into history_
                                     // only if moveUndoPushed_ becomes true
    Vec2 boxSelectCurrentCanvas_{};
    std::string linkDragFromNode_, linkDragFromPin_;
    bool linkDragFromIsOutput_ = false;

    // Feedback for a refused connection attempt, shown for a few seconds rather than nothing --
    // silently ignoring a rejected connection is the most common complaint about these tools.
    std::string lastRejectMsg_;
    double lastRejectAtSec_ = -1000.0;

    // Right-click "add node" palette.
    Vec2 pendingSpawnCanvasPos_{};

    // ---- details panel (Gap B: attribute editing) -------------------------------------------------
    // Which attribute InputText, if any, is mid-edit -- "" \x1f key when none active. One slot is
    // enough since only one field holds ImGui focus at a time: setAttribute()/clearAttribute() (and
    // their pushUndo()) fire once per SESSION (IsItemDeactivatedAfterEdit), not per keystroke -- same
    // shape as SandboxApp's DragFloat3 undo boundary. Keystrokes live in attrEditBuf_ until deactivation.
    std::string attrEditRowKey_;
    char attrEditBuf_[512] = {};

    // ---- Variables panel edit state -- same activate/apply-live/deactivate shape as attrEditRowKey_/
    // attrEditBuf_, for the panel's name/default fields (type is a Combo, no keystroke state needed).
    // Keyed "field\x1fvariableName", not by row index, so a delete/reorder can't hand the buffer to another row.
    std::string varEditRowKey_;
    char varEditBuf_[256] = {};

    // ---- input-scheme action cache -- backs the action= picker on InputAction/InputActionPressed/
    // InputActionReleased/RebindAction/GetActionKey. Resolved from path_ by walking up to the owning
    // .ocproject, reading INPUT.SCHEME, parsing that .ocinput (fmt::loadOcinput) -- see
    // refreshSchemeActionsIfNeeded in the .cpp for the full contract and why it bypasses the framework
    // ABI (no live C# runtime here). Throttled to ~1/sec, and only re-parses when
    // schemeFileWriteTime_ moves, so editing a dozen action= rows in one session costs one stat()
    // call/sec, not one parse.
    std::vector<std::string> schemeActionNames_;
    bool schemeFileFound_ = false;      // a scheme was resolved AND parsed at the last refresh
    std::string schemeResolvedPath_;    // its absolute path, for the picker's tooltip / warning text
    std::filesystem::file_time_type schemeFileWriteTime_{};
    double schemeCacheAtSec_ = -1000.0; // ImGui::GetTime() at the last refresh
    bool schemeCacheInited_ = false;    // false until the first refresh, so that one is never throttled
    void refreshSchemeActionsIfNeeded();

    // One-shot: frame the whole graph on the first draw that knows the canvas size. Set at load, not
    // in loadFromDisk itself -- the viewport size is an ImGui fact that doesn't exist there yet, and
    // framing to a guessed size is the same bug as not framing.
    bool pendingFrame_ = true;
    // Raised by the Frame All / Auto-Layout toolbar buttons, honoured by the canvas the SAME frame.
    // Separate from pendingFrame_ only to keep the two reasons legible (toolbar draws before the
    // canvas child exists, so it doesn't know the viewport size yet either).
    bool framePendingFromToolbar_ = false;
    // The canvas size the PREVIOUS frame saw, so a resize can be noticed. See the frame block in
    // drawEventGraph.
    Vec2 lastCanvasSizePx_{};
    // Whether the AUTHOR has chosen this view (set only by pan/zoom). False keeps the graph framed;
    // true means the view moves only via explicit Frame All / F / Home. Dragging a node does not set
    // it -- moving a thing is not moving the camera, and a graph being tidied should stay framed.
    bool viewTouched_ = false;

    // ---- function state ---------------------------------------------------------------------------
    std::string currentSubgraph_;          // empty = the event graph
    std::string funcEditRowKey_;           // in-flight text edit, keyed like varEditRowKey_
    char funcEditBuf_[128] = {};

    // The add-node popup's search box. Cleared and focused every time the popup opens, so a stale
    // filter from last time is never still applied.
    char addSearch_[128] = {};

    // The copy buffer. Nodes verbatim (ids and all -- they are remapped at paste, not at copy, so the
    // same buffer can be pasted repeatedly) and only the links whose two ends are both in it.
    std::vector<fmt::OcGraphNode> clipNodes_;
    std::vector<fmt::OcGraphLink> clipLinks_;
    char newFuncPinBuf_[64] = {};
    int newFuncPinType_ = 0;               // index into the same float/int/bool list the Variables panel uses
    fmt::OcGraphFunction* findFunction(const std::string& name);
    // Rebuilds the pins of every FuncEntry/FuncReturn/CallFunc depending on `funcName`, from that
    // function's current declaration. Called after any signature edit -- one function rather than
    // three call sites that could drift (see .cpp).
    void resyncFunctionNodePins(const std::string& funcName);
    void drawFunctionsPanel(float dpi);

    // ---- comment box state ------------------------------------------------------------------------
    // Selected/drag targets are held BY ID, never index, same reason as varEditRowKey_: a mid-session
    // delete must not hand selection to whatever shuffled into that slot.
    std::string selectedComment_;

    // What the right button went down on, recorded at press so release can pick the right popup.
    // Right button is ambiguous: a drag pans, a click opens a menu, and which menu depends on what
    // was under the cursor at press -- by release the view may have panned away from it. Empty/-1
    // means empty canvas -> Add Node.
    std::string rightClickNode_;
    int         rightClickLink_ = -1;

    // The two hooks the editor is given from outside: what can check a graph, and what can say which
    // of its nodes just ran. Both may be unset -- a build with no .NET runtime installs neither.
    ValidateFn  validate_;
    NodeHitsFn  nodeHits_;
    // nodeId -> seconds since it last ran, refreshed once a frame. Cleared when the source is absent
    // so a stale set cannot keep glowing after Play stops.
    std::unordered_map<std::string, f32> nodeHitAges_;
    // The last validation result, shown as a badge on the named node until the graph changes.
    std::string validateErr_;
    std::string validateNode_;

    // Armed by beginLinkDrop, cleared by cancelLinkDrop/spawnAndConnectLinkDrop. The from-pin is kept
    // here rather than read from linkDragFromNode_ at use time, because drag state resets the instant
    // the button comes up while the palette is submitted later in the same frame.
    bool        linkDropPending_ = false;
    std::string linkDropFromNode_;
    std::string linkDropFromPin_;
    bool        linkDropFromIsOutput_ = false;
    std::string linkDropFromType_;
    std::string activeComment_;            // the box being moved or resized right now
    Vec2 commentDragStartPos_{}, commentDragStartSize_{};
    // Nodes captured when a MOVE began, and their start positions. A comment box drags what it
    // encloses -- most of why an author draws one -- with membership frozen AT DRAG START, not
    // recomputed per frame -- otherwise a node the box slides over mid-drag would join the convoy
    // it was never inside when the drag began.
    std::unordered_map<std::string, Vec2> commentCapturedStart_;
    bool commentUndoPushed_ = false;       // lazy, exactly like moveUndoPushed_ above
    UndoState pendingCommentSnapshot_;
    // Which box the properties popup is editing, and its in-flight title text. Same activate/
    // apply-live/deactivate shape as attrEditBuf_.
    std::string commentEditId_;
    char commentEditBuf_[256] = {};
    bool commentPopupQueued_ = false;      // one-shot: OpenPopup on the frame after the double-click
    std::string makeUniqueCommentId() const;
    fmt::OcGraphComment* findComment(const std::string& id);
    // Which box, if any, is under `canvasPt`, and whether it's on the RESIZE GRIP vs. title bar --
    // only those two strips are hit-testable; the body stays click-through or it'd swallow clicks
    // meant for enclosed nodes.
    std::string commentAtCanvas(Vec2 canvasPt, float dpi, bool* outOnGrip) const;

    // ---- helpers (implemented in the .cpp, next to the input handling that uses them) -------------
    void loadFromDisk();
    void runAutoLayoutIfUnpositioned();
    void recomputeLayouts(float dpi);
    std::string makeUniqueNodeId(const std::string& typeId) const;
    std::string makeUniqueVariableName(const std::string& base) const;
    // An event name no ENTRY record in this graph already uses.
    std::string makeUniqueEventName(const std::string& base) const;
    // Points the ENTRY record for `nodeId` at `eventName` (inserting or removing as needed). The
    // one place the NODE `name=` attribute and the top-level ENTRY record are reconciled -- see its
    // definition for why they can disagree at all, and what it costs when they do.
    void syncEventEntry(const std::string& nodeId, const std::string& eventName);
    // Sets lastRejectMsg_/lastRejectAtSec_ and logs -- shared plumbing behind the rejection banner.
    // reportLinkRejection and deleteVariable's refusal are its two callers, deleteVariable added
    // alongside it rather than growing a second, near-duplicate banner mechanism.
    void showRejectionBanner(const std::string& msg);
    void reportLinkRejection(const GraphLinkCheck& check);
    void commitLink(const std::string& srcNode, const std::string& srcPin,
                     const std::string& dstNode, const std::string& dstPin);
};

// The text the Content Browser's "New Aver Node Graph" writes for a file whose stem is `stem`.
// Written as TEXT, and that is forced: OcGraphData doesn't model the CLASS record (grep
// modules/formats/src/OcGraph.cpp for "CLASS" -- nothing), which survives only because save() passes
// unrecognised lines through (writeOcgraph(graph_, originalText_)). fmt::saveOcgraph() writes fresh
// with none, so a starter built as an OcGraphData would come out with NO CLASS LINE -- unplaceable
// in a level, because a placement names a class rather than a file. Declared here so a test can
// parse it, same reason SoundEditor.hpp declares snStarterGraph.
std::string graphStarterText(const std::string& stem);

// Installs the validator every GraphEditor opened from here on will use, process-wide (same shape/
// reason as setActorEditorContentRoot: makeGraphEditor takes only a path, nowhere to thread a
// per-instance dependency). SandboxApp calls this once with a lambda over ScriptHost. Call before
// opening files -- an editor already open keeps what it has.
void setGraphValidator(GraphEditor::ValidateFn fn);

// The node-hit source every GraphEditor opened from here on will poll -- same process-wide shape
// and reason as setGraphValidator above.
void setGraphNodeHitSource(GraphEditor::NodeHitsFn fn);

// Creates the .ocgraph editor tab, or nullptr for any other extension. Registered via the EXACT HOOK above.
std::unique_ptr<AssetEditor> makeGraphEditor(const std::string& path);

} // namespace aver::editor
