// The .ocgraph node editor tab: ImGui canvas on top of GraphEditorGeometry's headless core.
//
// LOAD/SAVE CONTRACT:
//   1. Load -> save with no edits is byte-identical: graph_ is the exact parseOcgraph() output,
//      mutated ONLY in the edit paths below (each preceded by pushUndo()); auto-layout/display never
//      touch it (see displayPos_ in GraphEditor.hpp). save() always writes
//      fmt::writeOcgraph(graph_, originalText_) -- matches testRoundTrip (tests/formats/src/OcGraphTest.cpp:149).
//   2. Unknown records survive a save: NEVER call fmt::saveOcgraph() (OcGraph.cpp:376-386), which
//      writes fresh with no `existing` text and drops anything unmodeled (the .ocworld trap in
//      docs/VISUAL_SCRIPTING.md). Always go through save() below, which threads originalText_ through
//      writeOcgraph so unrecognised lines survive untouched (testUnknownRecords, OcGraphTest.cpp:183).
#include "EditorTransform.hpp"
#include "aver/core/Types.hpp"   // AVER_PATH_SEP
#include "EditorKeybinds.hpp"
#include "EditorPrefs.hpp"
#include "GraphEditor.hpp"
#include "GraphNodeDefs.hpp"
#if AVER_MODULE_FLUIDS
#  include "aver/fluids/FluidVolume.hpp"   // the seed shell a Fluid component previews
#endif
#if AVER_WITH_IMGUI
// Viewport tab only; guarded because tests/editor compiles this file with no ImGui/preview module and a three-library link (tests/editor/CMakeLists.txt).
#  include "ActorEditor.hpp"   // sharedPreview / sharedPreviewMeshes / actorEditorContentRoot
#endif

#include "aver/core/Log.hpp"
#include "aver/formats/OcInput.hpp"       // loadOcinput -- refreshSchemeActionsIfNeeded's action= picker
#include "aver/formats/OcProject.hpp"     // loadOcproject -- resolves INPUT.SCHEME for the same picker
#include "aver/platform/FileSystem.hpp"   // writeFileTextAtomic -- see save() below
#if AVER_WITH_IMGUI
#  include "aver/runtime/Engine.hpp"
#  include "aver/render/preview/ActorPreview.hpp"
#  include "aver/render/preview/PreviewMeshCache.hpp"
#  include "PreviewChrome.hpp"   // drawPreviewToolbar/drawPreviewStats/drawPreviewAxes/formatCount
#  if AVER_MODULE_PBR
// Material-graph registry: a material graph's Viewport preview shades via the same compiled graph the renderer uses, so they can't drift.
// Nested inside the ImGui guard too, since tests/editor compiles this file with neither.
#    include "aver/pbr/MaterialGraphRegistry.hpp"
#  endif
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <utility>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#  include "imgui_internal.h"   // GImGui/ImGuiLastItemData -- see drawMaterialShapeSelector below
#endif

#include "GraphEditorInternal.hpp"

namespace aver::editor {

using namespace gedetail;

void setGraphEditorDpi(float dpi) { g_graphEditorDpi = dpi; }

GraphEditor::GraphEditor(std::string path) : path_(std::move(path)) {
    loadFromDisk();
    g_openGraphEditors.push_back(this);
}

GraphEditor::~GraphEditor() {
    g_openGraphEditors.erase(std::remove(g_openGraphEditors.begin(), g_openGraphEditors.end(), this),
                             g_openGraphEditors.end());
}

void GraphEditor::loadFromDisk() {
    std::ifstream in(path_, std::ios::binary);
    if (!in) {
        loaded_ = false;
        loadError_ = "could not open " + path_;
        return;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    originalText_ = ss.str();

    fmt::OcGraphData g;
    std::string err;
    if (!fmt::parseOcgraph(originalText_, g, &err)) {
        loaded_ = false;
        loadError_ = err.empty() ? "failed to parse .ocgraph" : err;
        return;
    }

    graph_ = std::move(g);
    loaded_ = true;
    loadError_.clear();
    dirty_ = false;

    // Before layout: node box height comes from pin count, so filling pins in afterwards would
    // space nodes smaller than what's actually drawn.
    synthesizeMissingPins();
    // A function node (FuncEntry/FuncReturn/CallFunc) has no catalog pins -- its shape comes from a
    // FUNC declaration, not a node type. Without this, a call node loaded from disk draws no pins and
    // every wire into it silently disappears.
    for (const auto& f : graph_.functions) resyncFunctionNodePins(f.name);

    displayPos_.clear();
    for (const auto& n : graph_.nodes)
        displayPos_[n.id] = Vec2(static_cast<f32>(n.x), static_cast<f32>(n.y));
    runAutoLayoutIfUnpositioned();

    selectedNodes_.clear();
    selectedLink_ = -1;
    history_.clear();
    dragMode_ = DragMode::None;
    view_ = CanvasTransform{};

    // A fresh read is a fresh graph for the Problems panel: nothing it listed applies any more.
    problems_.clear();
    problemsCut_ = 0;
    badges_.clear();
    diagRunSerial_ = kDiagnosticsNeverRan;
    find_ = FindCursor{};
    refHits_.clear();
    refProjectHits_.clear();

#if AVER_WITH_IMGUI
    bottomOpen_ = prefBool("graphEditor.problemsOpen", false);
    // This user's breakpoints for this graph, read once per session: the session set is the live one
    // after that (a second tab on the same graph must not clobber edits made since).
    {
        static std::set<std::string> loaded;
        if (loaded.insert(debugGraphName()).second)
            debugSession().breakpoints.loadGraph(debugGraphName(), prefString("graphEditor.breakpoints." + debugGraphName(), ""));
    }
#endif
}

// Gives every node the pins its TYPE implies but the file did not write down (see the
// synthesizedPins_ comment in the header for why a valid .ocgraph can be missing them). Matched on
// name AND direction, since a node may legitimately have an input and output sharing a name.
void GraphEditor::synthesizeMissingPins() {
    synthesizedPins_.clear();
    for (auto& n : graph_.nodes) {
        const GraphNodeDesc* desc = findGraphNodeDesc(n.type);
        if (!desc) continue;   // a type this build does not know: leave it exactly as written
        for (const GraphPinSpec& spec : desc->pins) {
            bool present = false;
            for (const auto& p : n.pins)
                if (p.isOutput == spec.isOutput && p.name == spec.name) { present = true; break; }
            if (present) continue;
            fmt::OcGraphPin p;
            p.name = spec.name;
            p.type = spec.type;
            p.isOutput = spec.isOutput;
            p.defaultValue = spec.defaultValue;
            n.pins.push_back(std::move(p));
            synthesizedPins_.insert(pinKey(n.id, spec.name, spec.isOutput));
        }
    }
}

// Auto-layout for a graph with no meaningful stored positions: triggers only when EVERY node sits at
// exactly (0,0) -- the shape a hand-written file has when nobody set NODE's x/y, since the parser
// requires x on every NODE line (OcGraph.cpp:78-81) but a human just types "0 0" or omits y. A file
// with one deliberate node at the origin and others placed elsewhere is left alone. Writes ONLY the
// display overlay (see GraphEditor.hpp's displayPos_ comment).
void GraphEditor::runAutoLayoutIfUnpositioned() {
    if (graph_.nodes.empty()) return;
    bool allOrigin = true;
    for (const auto& n : graph_.nodes) {
        if (n.x != 0.0 || n.y != 0.0) { allOrigin = false; break; }
    }
    if (!allOrigin) return;
    // Same scale recomputeLayouts() draws at, not 1.0: node boxes are sized at DPI, so spacing them as
    // if DPI were 1 packs columns three times too tight on a 300% display and titles clip. Layout and
    // drawing must agree on how big a node is.
    autoLayoutDpi_ = g_graphEditorDpi;
    autoLaidOut_ = true;
    const auto pos = autoLayoutPositions(graph_, style_, autoLayoutDpi_);
    for (const auto& kv : pos) displayPos_[kv.first] = kv.second;
}

std::string GraphEditor::title() const {
    return std::filesystem::path(path_).filename().string() + "  [Graph]";
}

bool GraphEditor::save(std::string* why) {
    if (!loaded_) {
        if (why) *why = "cannot save: file failed to load (" + loadError_ + ")";
        return false;
    }
    // Strip the pins synthesizeMissingPins() invented, so a save writes the author's own PIN records,
    // not forty the editor made up to have something to draw wires to.
    fmt::OcGraphData toWrite = graph_;
    if (!synthesizedPins_.empty()) {
        for (auto& n : toWrite.nodes) {
            n.pins.erase(std::remove_if(n.pins.begin(), n.pins.end(),
                                        [&](const fmt::OcGraphPin& p) {
                                            return synthesizedPins_.count(pinKey(n.id, p.name, p.isOutput)) != 0;
                                        }),
                         n.pins.end());
        }
    }
    // See the file-header comment: writeOcgraph(graph_, originalText_), never saveOcgraph(), is what
    // preserves unrecognised records and keeps a no-op round trip byte-identical.
    const std::string text = fmt::writeOcgraph(toWrite, originalText_);

    std::error_code ec;   // (validateNow() below serialises through serializeForSave(), the same path)
    const std::filesystem::path p(path_);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);

    // Write-to-temp-and-swap via writeFileTextAtomic (aver/platform/FileSystem.hpp), lifted to the
    // shared platform layer -- the same pattern aver::fmt::saveOcSave and aver_settings_flush use. This
    // used to open `path_` directly with ios::trunc -- the same bug saveOcgraph (OcGraph.cpp) has --
    // which zeroes the file before `text` lands a byte, so a crash or full disk between open and write
    // destroyed the graph outright.
    if (!writeFileTextAtomic(path_, text)) {
        if (why) *why = "could not write " + path_;
        return false;
    }
    originalText_ = text; // the next save merges against what is now actually on disk
    dirty_ = false;
    return true;
}

// Exactly what save() would write, factored out so validateNow() can't check a different document
// than the one that lands on disk -- including the synthesized-pin strip, or the validator's answers
// would name pins the author never wrote.
std::string GraphEditor::serializeForSave() const {
    fmt::OcGraphData toWrite = graph_;
    if (!synthesizedPins_.empty()) {
        for (auto& n : toWrite.nodes) {
            n.pins.erase(std::remove_if(n.pins.begin(), n.pins.end(),
                                        [&](const fmt::OcGraphPin& p) {
                                            return synthesizedPins_.count(pinKey(n.id, p.name, p.isOutput)) != 0;
                                        }),
                         n.pins.end());
        }
    }
    return fmt::writeOcgraph(toWrite, originalText_);
}

std::string GraphEditor::errorNodeId(const std::string& message, const fmt::OcGraphData& g) {
    // A deliberately timid heuristic: the validator's messages are hand-written sentences (e.g. "Node
    // 'muzzle' is a Param node but has no param= attribute...") with no structured field to read, so
    // the node id is just the first single-quoted token -- returned ONLY if it's a real node id in
    // this graph, so a message quoting a pin/event/param/function name yields nothing rather than
    // badging the wrong node.
    // Quote character as a constant, not a literal: writing it inline is what this file's escapes
    // keep getting mangled on (see the escape-eating-tooling note).
    const char kQuote = 0x27;   // '
    const usize a = message.find(kQuote);
    if (a == std::string::npos) return {};
    const usize b = message.find(kQuote, a + 1);
    if (b == std::string::npos || b <= a + 1) return {};
    const std::string token = message.substr(a + 1, b - a - 1);
    for (const auto& n : g.nodes) if (n.id == token) return token;
    return {};
}

bool GraphEditor::validateNow(std::string& err, std::string& offendingNode) {
    err.clear();
    offendingNode.clear();
    if (!validate_) {
        // NOT "valid". An absent validator is an absence of evidence, and saying "valid" here would
        // be the exact unbacked claim this codebase has been bitten by before.
        err = "no validator is available (the .NET bridge exports no GraphValidate)";
        return false;
    }
    std::string message;
    if (validate_(serializeForSave(), message)) return true;
    err = message.empty() ? std::string("the graph is not valid") : message;
    offendingNode = errorNodeId(err, graph_);
    return false;
}

void GraphEditor::onFileChanged() {
    if (dirty_) return; // never clobber unsaved edits behind the user's back
    loadFromDisk();
    // A reload is a new set of positions, so the view re-frames onto them -- an externally edited
    // graph that moved every node would otherwise reopen looking at an empty grid. AFTER the dirty
    // guard: a notification that reloads nothing must move nothing either.
    pendingFrame_ = true;
    viewTouched_ = false;
}

void GraphEditor::pushUndo() {
    // A standing validation result describes the graph it was run on, and every pushUndo caller is
    // about to change that graph -- left up, the message would go on naming a node just fixed or
    // deleted, reading as "still broken" when it isn't.
    validateErr_.clear();
    validateNode_.clear();

    ++editSerial_;
    lastEditAtSec_ = clockSec();

    UndoState s;
    s.graph = graph_;
    s.displayPos = displayPos_;
    history_.push(std::move(s));
}

void GraphEditor::undo() {
    UndoState s{graph_, displayPos_};
    if (!history_.undo(s)) return;
    graph_ = std::move(s.graph);
    displayPos_ = std::move(s.displayPos);
    ++editSerial_;
    lastEditAtSec_ = clockSec();
    dirty_ = true;
    selectedNodes_.clear();
    selectedLink_ = -1;
}

void GraphEditor::redo() {
    UndoState s{graph_, displayPos_};
    if (!history_.redo(s)) return;
    graph_ = std::move(s.graph);
    displayPos_ = std::move(s.displayPos);
    ++editSerial_;
    lastEditAtSec_ = clockSec();
    dirty_ = true;
    selectedNodes_.clear();
    selectedLink_ = -1;
}

std::string GraphEditor::makeUniqueNodeId(const std::string& typeId) const {
    std::string base = typeId;
    for (char& c : base) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (int i = 1; i < 1000000; ++i) {
        const std::string candidate = base + std::to_string(i);
        bool taken = false;
        for (const auto& n : graph_.nodes) {
            if (n.id == candidate) { taken = true; break; }
        }
        if (!taken) return candidate;
    }
    return base + "_x"; // unreachable in practice
}

std::string GraphEditor::makeUniqueEventName(const std::string& base) const {
    std::string name = base;
    for (int n = 1; n < 10000; ++n) {
        bool taken = false;
        for (const auto& e : graph_.entryPoints) if (e.second == name) { taken = true; break; }
        if (!taken) return name;
        name = base + std::to_string(n);
    }
    return base;
}

// The node's name= and the ENTRY record say the same thing twice -- that's the format, not a choice
// made here: `ENTRY <nodeId> <eventName>` is the only thing that makes an event fire (CompileEntryPoint
// matches on it alone). One function writes the ENTRY side and every path touching the name goes
// through it, or a rename would save/load/display fine while the event silently stopped firing.
void GraphEditor::syncEventEntry(const std::string& nodeId, const std::string& eventName) {
    for (usize i = 0; i < graph_.entryPoints.size(); ++i) {
        if (graph_.entryPoints[i].first != nodeId) continue;
        if (eventName.empty()) graph_.entryPoints.erase(graph_.entryPoints.begin() + static_cast<isize>(i));
        else graph_.entryPoints[i].second = eventName;
        return;
    }
    if (!eventName.empty()) graph_.entryPoints.emplace_back(nodeId, eventName);
}

// OnInputAction's ENTRY name is composed from its action= and event= attributes, the bus event
// EnhancedInput.PumpEvents raises (docs/plans/wave2/ENHANCED_INPUT.md section 6); empty until both are set.
static std::string inputActionEntryName(const fmt::OcGraphNode& n) {
    const std::string action = getNodeAttribute(n, "action").value;
    const std::string event = getNodeAttribute(n, "event").value;
    return action.empty() || event.empty() ? std::string() : "Input." + action + "." + event;
}

// Which node vocabulary the open graph belongs to, read from the file's own DOMAIN record -- the same
// answer the compiler gives, so the palette never offers a node the compiler would then refuse.
GraphNodeDomain GraphEditor::openGraphDomain() const {
    return fmt::ocGraphDomainOf(graph_) == fmt::OcGraphDomain::Material ? kDomainMaterial
                                                                        : kDomainGameplay;
}

// The pin a link drag started from, looked up in the graph rather than in the layout cache: the
// layout carries geometry, and what the filter needs is the pin's TYPE.
static const fmt::OcGraphPin* findGraphPin(const fmt::OcGraphData& g, const std::string& nodeId,
                                           const std::string& pinName) {
    for (const auto& n : g.nodes) {
        if (n.id != nodeId) continue;
        for (const auto& p : n.pins) if (p.name == pinName) return &p;
        return nullptr;
    }
    return nullptr;
}

void GraphEditor::beginLinkDrop(const std::string& fromNode, const std::string& fromPin, bool fromIsOutput) {
    const fmt::OcGraphPin* p = findGraphPin(graph_, fromNode, fromPin);
    if (!p) { linkDropPending_ = false; return; }   // a pin that is not there cannot be wired to
    linkDropPending_      = true;
    linkDropFromNode_     = fromNode;
    linkDropFromPin_      = fromPin;
    linkDropFromIsOutput_ = fromIsOutput;
    linkDropFromType_     = p->type;
}

bool GraphEditor::linkDropAccepts(const GraphNodeDesc& desc) const {
    if (!linkDropPending_) return false;
    if ((desc.domain & openGraphDomain()) == 0u) return false;

    // The SAME predicate commitLink uses, not a second rule that happens to agree today -- so the
    // palette never offers a node whose link then gets refused: the author has already committed to
    // the gesture by the time they find out.
    const PinTypeCompat compat = openGraphDomain() == kDomainMaterial ? &materialPinTypeMatch
                                                                     : &exactPinTypeMatch;
    for (const auto& ps : desc.pins) {
        // Dragging FROM an output looks for an INPUT to land on, and vice versa; getting this
        // backwards would offer exactly the wrong half of the palette.
        if (ps.isOutput == linkDropFromIsOutput_) continue;
        const bool ok = linkDropFromIsOutput_ ? compat(linkDropFromType_, ps.type)
                                              : compat(ps.type, linkDropFromType_);
        if (ok) return true;
    }
    return false;
}

std::string GraphEditor::spawnAndConnectLinkDrop(const std::string& typeId, Vec2 canvasPos) {
    if (!linkDropPending_) return addNodeFromCatalog(typeId, canvasPos);

    const GraphNodeDesc* desc = findGraphNodeDescIn(typeId, openGraphDomain());
    if (!desc) { linkDropPending_ = false; return {}; }

    // Resolve the target pin BEFORE spawning, so a type with no compatible pin costs no node and no
    // undo step (reached when a caller spawns something the filter wouldn't have offered).
    const PinTypeCompat compat = openGraphDomain() == kDomainMaterial ? &materialPinTypeMatch
                                                                     : &exactPinTypeMatch;
    std::string targetPin;
    for (const auto& ps : desc->pins) {
        if (ps.isOutput == linkDropFromIsOutput_) continue;
        const bool ok = linkDropFromIsOutput_ ? compat(linkDropFromType_, ps.type)
                                              : compat(ps.type, linkDropFromType_);
        // First accepting pin in catalog declaration order (not nearest geometrically -- the node
        // doesn't exist yet to measure against), so a wire dropped near Add lands on `a` consistently.
        if (ok) { targetPin = ps.name; break; }
    }
    if (targetPin.empty()) { linkDropPending_ = false; return {}; }

    const std::string newId = addNodeFromCatalog(typeId, canvasPos);
    if (newId.empty()) { linkDropPending_ = false; return {}; }

    // commitLink pushes its own undo, so this gesture costs two steps (node, then wire) -- deliberate:
    // undoing once leaves the node disconnected rather than back at nothing.
    if (linkDropFromIsOutput_) commitLink(linkDropFromNode_, linkDropFromPin_, newId, targetPin);
    else                       commitLink(newId, targetPin, linkDropFromNode_, linkDropFromPin_);

    linkDropPending_ = false;
    return newId;
}

std::string GraphEditor::addNodeFromCatalog(const std::string& typeId, Vec2 canvasPos) {
    // Domain-aware: sixteen type names exist in both vocabularies with DIFFERENT PINS (a gameplay Add
    // takes two scalars, a material Add takes two float3s). Resolving by name alone handed a material
    // graph the scalar shape.
    const GraphNodeDesc* desc = findGraphNodeDescIn(typeId, openGraphDomain());
    if (!desc) return {};

    pushUndo();
    fmt::OcGraphNode node;
    node.id = makeUniqueNodeId(desc->typeId);
    node.type = desc->typeId;
    node.x = canvasPos.x;
    node.y = canvasPos.y;
    for (const auto& ps : desc->pins)
        node.pins.push_back(fmt::OcGraphPin{ps.name, ps.type, ps.isOutput, ps.defaultValue});
    graph_.nodes.push_back(node);

    // An event node with no `ENTRY <nodeId> <eventName>` record never runs -- the type is only a
    // label (docs/AVER_NODE_NODES.md's Flow section). Dropping an On Tick used to create the node and
    // nothing else, saving and running while silently doing nothing. Event name defaults to the node
    // TYPE (`ENTRY tick OnTick`), driven off the catalog's own "Event" category rather than a second
    // hand-maintained list (see GraphNodeDefs.hpp for why there's only one registration point).
    if (desc->category == "Event") {
        // CustomEvent is the one event whose name is NOT its type; a fresh one gets a unique generated
        // name since an ENTRY with no name is unrepresentable.
        std::string eventName = desc->typeId;
        if (desc->typeId == "CustomEvent") {
            eventName = makeUniqueEventName("MyEvent");
            setNodeAttribute(graph_.nodes.back(), "name", eventName);
        } else if (desc->typeId == "OnInputAction") {
            setNodeAttribute(graph_.nodes.back(), "event", "Triggered");
            eventName = inputActionEntryName(graph_.nodes.back());
        }
        syncEventEntry(node.id, eventName);
    }

    // A node dropped onto a function's canvas belongs to that function -- otherwise it lands in the
    // event graph and vanishes the instant it's drawn: present in the file, invisible on the canvas
    // that made it.
    if (!currentSubgraph_.empty()) setNodeAttribute(graph_.nodes.back(), "func", currentSubgraph_);

    displayPos_[node.id] = canvasPos;
    selectedNodes_ = {node.id};
    selectedLink_ = -1;
    dirty_ = true;
    return node.id;
}

// Bar height and grip size, in CANVAS units (logical px at zoom 1) -- scale by dpi only, not
// view_.zoom, since the drawing code already multiplies by zoom on its way to the screen; folding zoom
// in here too would make the bar twice as tall.

std::string GraphEditor::makeUniqueCommentId() const {
    for (int i = 1; ; ++i) {
        const std::string candidate = "cmt" + std::to_string(i);
        bool taken = false;
        for (const auto& c : graph_.comments) if (c.id == candidate) { taken = true; break; }
        if (!taken) return candidate;
    }
}

fmt::OcGraphComment* GraphEditor::findComment(const std::string& id) {
    for (auto& c : graph_.comments) if (c.id == id) return &c;
    return nullptr;
}

std::string GraphEditor::addComment(Vec2 a, Vec2 b, const std::string& text) {
    pushUndo();
    fmt::OcGraphComment c;
    c.id = makeUniqueCommentId();
    c.x = std::min(a.x, b.x);
    c.y = std::min(a.y, b.y);
    c.w = std::max(static_cast<f64>(std::abs(b.x - a.x)), static_cast<f64>(kCommentMinPx));
    c.h = std::max(static_cast<f64>(std::abs(b.y - a.y)), static_cast<f64>(kCommentMinPx));
    c.text = text;
    graph_.comments.push_back(c);
    selectedComment_ = c.id;
    dirty_ = true;
    return c.id;
}

std::string GraphEditor::addCommentAroundSelection(float dpi) {
    if (selectedNodes_.empty()) return {};
    recomputeLayouts(dpi);
    // Measured from the LAYOUTS, not displayPos_: a node position is its top-left, and a box drawn to
    // those would clip the right-hand pins off every node on its edge.
    bool any = false;
    Vec2 lo{}, hi{};
    for (const auto& nl : layouts_) {
        if (std::find(selectedNodes_.begin(), selectedNodes_.end(), nl.nodeId) == selectedNodes_.end()) continue;
        if (!any) { lo = nl.min; hi = nl.max; any = true; continue; }
        lo.x = std::min(lo.x, nl.min.x); lo.y = std::min(lo.y, nl.min.y);
        hi.x = std::max(hi.x, nl.max.x); hi.y = std::max(hi.y, nl.max.y);
    }
    if (!any) return {};
    // Margin on all sides, extra on top for the title bar (or it would hide the first row of node
    // headers). Scaled by DPI, since computeNodeLayout sizes nodes at DPI while position is unscaled --
    // an unscaled margin would run a third too narrow at 300% DPI. Runs loose at lower DPI later;
    // accepted over a box too small to enclose its own nodes on the machine that drew it.
    const float kMargin = 24.0f * dpi;
    lo.x -= kMargin; hi.x += kMargin;
    lo.y -= kMargin + kCommentBarPx * dpi; hi.y += kMargin;
    return addComment(lo, hi, "Comment");
}

bool GraphEditor::deleteComment(const std::string& id) {
    for (usize i = 0; i < graph_.comments.size(); ++i) {
        if (graph_.comments[i].id != id) continue;
        pushUndo();
        graph_.comments.erase(graph_.comments.begin() + static_cast<isize>(i));
        if (selectedComment_ == id) selectedComment_.clear();
        dirty_ = true;
        return true;
    }
    return false;
}

bool GraphEditor::setCommentText(const std::string& id, const std::string& text) {
    fmt::OcGraphComment* c = findComment(id);
    if (!c) return false;
    // A newline would end the record early, turning the title's tail into a separate record -- folded
    // to a space rather than refused, since a pasted title is common and losing it is worse than
    // flattening it.
    std::string flat = text;
    for (char& ch : flat) if (ch == 0x0a || ch == 0x0d) ch = 0x20;
    if (c->text == flat) return true;
    pushUndo();
    findComment(id)->text = flat;
    dirty_ = true;
    return true;
}

bool GraphEditor::setCommentColor(const std::string& id, int r, int g, int b) {
    fmt::OcGraphComment* c = findComment(id);
    if (!c) return false;
    const int cr = std::clamp(r, 0, 255), cg = std::clamp(g, 0, 255), cb = std::clamp(b, 0, 255);
    if (c->r == cr && c->g == cg && c->b == cb) return true;
    pushUndo();
    fmt::OcGraphComment* live = findComment(id);
    live->r = cr; live->g = cg; live->b = cb;
    dirty_ = true;
    return true;
}

std::vector<std::string> GraphEditor::nodesInsideComment(const std::string& id, float dpi) {
    recomputeLayouts(dpi);
    std::vector<std::string> out;
    const fmt::OcGraphComment* c = nullptr;
    for (const auto& k : graph_.comments) if (k.id == id) { c = &k; break; }
    if (!c) return out;
    // The title bar is excluded from the interior: a node overlapping the bar is a node the author
    // can still see and click, so it is not "in" the box for dragging purposes either.
    const f64 top = c->y + kCommentBarPx * dpi;
    for (const auto& nl : layouts_) {
        if (nl.min.x >= c->x && nl.max.x <= c->x + c->w && nl.min.y >= top && nl.max.y <= c->y + c->h)
            out.push_back(nl.nodeId);
    }
    return out;
}

std::string GraphEditor::commentAtCanvas(Vec2 canvasPt, float dpi, bool* outOnGrip) const {
    if (outOnGrip) *outOnGrip = false;
    // Back to front: boxes draw in file order, so the last one drawn is on top and owns the click.
    // Scanning forwards would hand an overlapping click to the box underneath.
    for (usize i = graph_.comments.size(); i-- > 0; ) {
        const auto& c = graph_.comments[i];
        const f64 barH  = kCommentBarPx * dpi;
        const f64 grip  = kCommentGripPx * dpi;
        const bool inX  = canvasPt.x >= c.x && canvasPt.x <= c.x + c.w;
        if (!inX) continue;
        if (canvasPt.x >= c.x + c.w - grip && canvasPt.y >= c.y + c.h - grip && canvasPt.y <= c.y + c.h) {
            if (outOnGrip) *outOnGrip = true;
            return c.id;
        }
        if (canvasPt.y >= c.y && canvasPt.y <= c.y + barH) return c.id;
    }
    return {};
}


fmt::OcGraphFunction* GraphEditor::findFunction(const std::string& name) {
    for (auto& f : graph_.functions) if (f.name == name) return &f;
    return nullptr;
}

void GraphEditor::setCurrentSubgraph(const std::string& funcName) {
    if (currentSubgraph_ == funcName) return;
    currentSubgraph_ = funcName;
    // A selection in the subgraph just left is invisible but Delete would still act on it, so it's
    // cleared; the view re-frames onto the new subgraph's content ("the view is the editor's until
    // you touch it", per the framing code).
    selectedNodes_.clear();
    selectedLink_ = -1;
    selectedComment_.clear();
    pendingFrame_ = true;
    viewTouched_ = false;
}

std::string GraphEditor::addFunction(const std::string& name) {
    if (name.empty() || name.find_first_of(" \t") != std::string::npos) return {};
    std::string unique = name;
    for (int i = 2; findFunction(unique) != nullptr; ++i) unique = name + std::to_string(i);

    pushUndo();
    fmt::OcGraphFunction fn;
    fn.name = unique;
    fn.pure = true;   // a fresh function has no body, so it has no control flow to be impure about
    graph_.functions.push_back(fn);

    // The entry node, always. See the header for why this is not a convenience.
    fmt::OcGraphNode entry;
    entry.id = makeUniqueNodeId("FuncEntry");
    entry.type = "FuncEntry";
    entry.x = 0.0; entry.y = 0.0;
    setNodeAttribute(entry, "func", unique);
    graph_.nodes.push_back(entry);
    displayPos_[entry.id] = Vec2{0.0f, 0.0f};

    dirty_ = true;
    return unique;
}

bool GraphEditor::renameFunction(const std::string& oldName, const std::string& newName) {
    if (newName.empty() || newName.find_first_of(" \t") != std::string::npos) return false;
    if (oldName == newName) return true;
    if (findFunction(oldName) == nullptr) return false;
    if (findFunction(newName) != nullptr) {
        showRejectionBanner("cannot rename function '" + oldName + "': '" + newName + "' already exists");
        return false;
    }
    pushUndo();
    findFunction(oldName)->name = newName;
    // EVERY REFERENCE MOVES IN THE SAME UNDO STEP -- both func= (where a node lives) and call= (what
    // a call node calls). Left behind, either produces a file that parses and refuses to compile --
    // the same failure renameVariable avoids for var=.
    for (auto& n : graph_.nodes) {
        const GraphNodeAttribute owner = getNodeAttribute(n, "func");
        if (owner.found && owner.value == oldName) setNodeAttribute(n, "func", newName);
        const GraphNodeAttribute target = getNodeAttribute(n, "call");
        if (target.found && target.value == oldName) setNodeAttribute(n, "call", newName);
    }
    if (currentSubgraph_ == oldName) currentSubgraph_ = newName;
    dirty_ = true;
    return true;
}

bool GraphEditor::deleteFunction(const std::string& name, std::vector<std::string>* outBlockedBy) {
    if (findFunction(name) == nullptr) return false;
    std::vector<std::string> callers;
    for (const auto& n : graph_.nodes) {
        const GraphNodeAttribute target = getNodeAttribute(n, "call");
        if (!target.found || target.value != name) continue;
        // A call INSIDE the function being deleted goes with it, so it is not a blocker.
        if (subgraphOf(n) == name) continue;
        callers.push_back(n.id);
    }
    if (!callers.empty()) {
        if (outBlockedBy) *outBlockedBy = callers;
        std::string list;
        for (const auto& c : callers) list += (list.empty() ? "" : ", ") + c;
        showRejectionBanner("cannot delete function '" + name + "': still called by " + list);
        return false;
    }

    pushUndo();
    std::vector<std::string> doomed;
    for (const auto& n : graph_.nodes) if (subgraphOf(n) == name) doomed.push_back(n.id);
    const auto isDoomed = [&](const std::string& id) {
        return std::find(doomed.begin(), doomed.end(), id) != doomed.end();
    };
    std::vector<fmt::OcGraphLink> keptLinks;
    for (const auto& l : graph_.links)
        if (!isDoomed(l.sourceNode) && !isDoomed(l.destNode)) keptLinks.push_back(l);
    graph_.links = std::move(keptLinks);
    std::vector<fmt::OcGraphNode> keptNodes;
    for (auto& n : graph_.nodes) {
        if (isDoomed(n.id)) { displayPos_.erase(n.id); continue; }
        keptNodes.push_back(std::move(n));
    }
    graph_.nodes = std::move(keptNodes);
    for (usize i = 0; i < graph_.functions.size(); ++i) {
        if (graph_.functions[i].name != name) continue;
        graph_.functions.erase(graph_.functions.begin() + static_cast<isize>(i));
        break;
    }
    if (currentSubgraph_ == name) setCurrentSubgraph({});
    selectedNodes_.clear();
    dirty_ = true;
    return true;
}

bool GraphEditor::addFunctionPin(const std::string& funcName, bool isInput, const std::string& pinName,
                                  const std::string& type) {
    fmt::OcGraphFunction* fn = findFunction(funcName);
    if (!fn || pinName.empty() || pinName.find_first_of(" \t") != std::string::npos) return false;
    auto& list = isInput ? fn->inputs : fn->outputs;
    for (const auto& p : list) if (p.name == pinName) return false;
    const std::string t = (type == "int" || type == "bool") ? type : std::string("float");

    pushUndo();
    fmt::OcGraphFunction* live = findFunction(funcName);
    (isInput ? live->inputs : live->outputs).push_back(fmt::OcGraphFunctionPin{pinName, t});

    // An OUTPUT needs somewhere to come from. The first one brings the FuncReturn node with it, for
    // the same reason addFunction brings the FuncEntry: a function that declares an output and has no
    // FuncReturn does not compile, and the error names a node type the author has not met yet.
    if (!isInput && live->outputs.size() == 1) {
        bool haveReturn = false;
        for (const auto& n : graph_.nodes)
            if (n.type == "FuncReturn" && subgraphOf(n) == funcName) { haveReturn = true; break; }
        if (!haveReturn) {
            fmt::OcGraphNode ret;
            ret.id = makeUniqueNodeId("FuncReturn");
            ret.type = "FuncReturn";
            ret.x = 420.0; ret.y = 0.0;
            setNodeAttribute(ret, "func", funcName);
            graph_.nodes.push_back(ret);
            displayPos_[ret.id] = Vec2{420.0f, 0.0f};
        }
    }
    resyncFunctionNodePins(funcName);
    dirty_ = true;
    return true;
}

bool GraphEditor::removeFunctionPin(const std::string& funcName, bool isInput, const std::string& pinName) {
    fmt::OcGraphFunction* fn = findFunction(funcName);
    if (!fn) return false;
    auto& list = isInput ? fn->inputs : fn->outputs;
    usize idx = list.size();
    for (usize i = 0; i < list.size(); ++i) if (list[i].name == pinName) { idx = i; break; }
    if (idx == list.size()) return false;

    pushUndo();
    fmt::OcGraphFunction* live = findFunction(funcName);
    auto& liveList = isInput ? live->inputs : live->outputs;
    liveList.erase(liveList.begin() + static_cast<isize>(idx));
    // EVERY WIRE INTO THE PIN THAT NO LONGER EXISTS GOES TOO, in the same undo step. Leaving them
    // would ship a LINK naming a pin nothing declares, which the parser refuses on the next load --
    // the editor writing a file it cannot reopen, which is the worst failure an authoring tool has.
    std::vector<fmt::OcGraphLink> kept;
    for (const auto& l : graph_.links) {
        bool drop = false;
        for (const auto& n : graph_.nodes) {
            if (!isFunctionNodeType(n.type)) continue;
            const bool aboutThis = (n.type == "CallFunc")
                ? (getNodeAttribute(n, "call").found && getNodeAttribute(n, "call").value == funcName)
                : (subgraphOf(n) == funcName);
            if (!aboutThis) continue;
            if (l.sourceNode == n.id && l.sourcePin == pinName) drop = true;
            if (l.destNode == n.id && l.destPin == pinName) drop = true;
        }
        if (!drop) kept.push_back(l);
    }
    graph_.links = std::move(kept);
    resyncFunctionNodePins(funcName);
    dirty_ = true;
    return true;
}

bool GraphEditor::setFunctionPure(const std::string& funcName, bool pure) {
    fmt::OcGraphFunction* fn = findFunction(funcName);
    if (!fn || fn->pure == pure) return fn != nullptr;
    pushUndo();
    findFunction(funcName)->pure = pure;
    // Purity decides whether every one of this function's nodes has exec pins at all, so the pins
    // have to be rebuilt -- and any exec wire that just stopped having a pin to land on has to go
    // with them, or the file will not reopen.
    resyncFunctionNodePins(funcName);
    dirty_ = true;
    return true;
}

void GraphEditor::resyncFunctionNodePins(const std::string& funcName) {
    const fmt::OcGraphFunction* fn = findFunction(funcName);
    if (!fn) return;

    // ONE PLACE THAT KNOWS THE PIN SHAPE OF ALL THREE NODE TYPES, rather than three call sites that
    // could drift apart -- and it has to agree exactly with OcGraphParser.AddDefaultPins on the C#
    // side, which is the reader that actually runs the graph. The rules, in both: a pure function has
    // no exec pins anywhere; an impure one has them on all three; a FuncEntry's OUTPUTS are the
    // function's inputs; a FuncReturn's INPUTS are its outputs; a call node has both.
    const auto pins = [&](const fmt::OcGraphNode& n) {
        std::vector<fmt::OcGraphPin> out;
        if (n.type == "FuncEntry") {
            if (!fn->pure) out.push_back({"then", "exec", true, ""});
            for (const auto& p : fn->inputs) out.push_back({p.name, p.type, true, ""});
        } else if (n.type == "FuncReturn") {
            if (!fn->pure) out.push_back({"exec", "exec", false, ""});
            for (const auto& p : fn->outputs) out.push_back({p.name, p.type, false, ""});
        } else {   // CallFunc
            if (!fn->pure) {
                out.push_back({"exec", "exec", false, ""});
                out.push_back({"then", "exec", true, ""});
            }
            for (const auto& p : fn->inputs) out.push_back({p.name, p.type, false, ""});
            for (const auto& p : fn->outputs) out.push_back({p.name, p.type, true, ""});
        }
        return out;
    };

    std::vector<std::string> touched;
    for (auto& n : graph_.nodes) {
        const bool mine = (n.type == "CallFunc")
            ? (getNodeAttribute(n, "call").found && getNodeAttribute(n, "call").value == funcName)
            : (isFunctionNodeType(n.type) && subgraphOf(n) == funcName);
        if (!mine) continue;
        n.pins = pins(n);
        touched.push_back(n.id);
        // Marked synthesised, so save() strips them again: they just restate the FUNC/FUNCIN/FUNCOUT
        // records (both this and OcGraphParser.AddDefaultPins derive them identically), and writing
        // them would duplicate the signature in the file, free to drift after any edit.
        for (const auto& p : n.pins) synthesizedPins_.insert(pinKey(n.id, p.name, p.isOutput));
    }

    // Any wire whose end no longer exists on a rebuilt node is dropped -- see removeFunctionPin for
    // why a dangling LINK is not survivable.
    std::vector<fmt::OcGraphLink> kept;
    for (const auto& l : graph_.links) {
        const auto stillThere = [&](const std::string& nodeId, const std::string& pin, bool isOutput) {
            if (std::find(touched.begin(), touched.end(), nodeId) == touched.end()) return true;
            for (const auto& n : graph_.nodes) {
                if (n.id != nodeId) continue;
                for (const auto& p : n.pins) if (p.name == pin && p.isOutput == isOutput) return true;
                return false;
            }
            return true;
        };
        if (stillThere(l.sourceNode, l.sourcePin, true) && stillThere(l.destNode, l.destPin, false))
            kept.push_back(l);
    }
    graph_.links = std::move(kept);
}

std::string GraphEditor::addCallNode(const std::string& funcName, Vec2 canvasPos) {
    if (findFunction(funcName) == nullptr) return {};
    pushUndo();
    fmt::OcGraphNode node;
    node.id = makeUniqueNodeId("CallFunc");
    node.type = "CallFunc";
    node.x = canvasPos.x;
    node.y = canvasPos.y;
    setNodeAttribute(node, "call", funcName);
    // A call node dropped while a function's canvas is open belongs to THAT function -- including
    // when it calls the same one, which is how a recursive call is authored.
    if (!currentSubgraph_.empty()) setNodeAttribute(node, "func", currentSubgraph_);
    graph_.nodes.push_back(node);
    displayPos_[node.id] = canvasPos;
    resyncFunctionNodePins(funcName);
    selectedNodes_ = {node.id};
    selectedLink_ = -1;
    dirty_ = true;
    return node.id;
}

// Thin ImGui glue over addFunction/renameFunction/deleteFunction/addFunctionPin/removeFunctionPin/
// setFunctionPure -- the same split the Variables panel above it draws, and for the same reason: the
// model half is exercised headlessly by GraphEditorLoadSaveTest, against the exact calls this panel
// makes rather than a hand-simulated approximation of them.
void GraphEditor::drawFunctionsPanel(float dpi) {
#if AVER_WITH_IMGUI
    if (!ImGui::CollapsingHeader("Functions", ImGuiTreeNodeFlags_DefaultOpen)) return;

    if (graph_.functions.empty()) ImGui::TextDisabled("No functions declared.");

    static const char* kTypes[] = {"float", "int", "bool"};

    for (usize fi = 0; fi < graph_.functions.size(); ++fi) {
        const std::string fname = graph_.functions[fi].name;   // by VALUE: the loop body can delete it
        ImGui::PushID(static_cast<int>(fi));

        const bool open = ImGui::TreeNodeEx("##fn", ImGuiTreeNodeFlags_DefaultOpen, "%s", fname.c_str());
        if (ImGui::BeginPopupContextItem("##fnctx")) {
            if (ImGui::MenuItem("Find References")) showReferences(RefKind::Function, fname, false);
            if (ImGui::MenuItem("Find References (whole project)")) showReferences(RefKind::Function, fname, true);
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Open")) setCurrentSubgraph(fname);
        ImGui::SameLine();
        if (ImGui::SmallButton("Call")) {
            // Dropped at the middle of wherever the canvas is looking, which is the only position
            // available here -- the panel has no cursor of its own.
            addCallNode(fname, screenToCanvas(view_, Vec2{lastCanvasSizePx_.x * 0.5f, lastCanvasSizePx_.y * 0.5f}));
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(currentSubgraph_.empty()
                               ? "Add a node that calls this function, in the event graph"
                               : "Add a node that calls this function, in the subgraph you are editing");

        if (open) {
            const std::string nameKey = "fnname\x1f" + fname;
            if (funcEditRowKey_ != nameKey) std::snprintf(funcEditBuf_, sizeof funcEditBuf_, "%s", fname.c_str());
            ImGui::SetNextItemWidth(150.0f * dpi);
            if (ImGui::InputText("Name", funcEditBuf_, sizeof funcEditBuf_)) funcEditRowKey_ = nameKey;
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                renameFunction(fname, funcEditBuf_);
                funcEditRowKey_.clear();
            }

            bool pure = graph_.functions[fi].pure;
            if (ImGui::Checkbox("Pure", &pure)) setFunctionPure(fname, pure);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pure: no exec pins, callable from a data wire.\n"
                                   "An impure function is the only kind that can RECURSE -- a pure one\n"
                                   "has no Branch to stop with, and Select evaluates both of its sides.");

            for (int side = 0; side < 2; ++side) {
                const bool isInput = (side == 0);
                ImGui::TextDisabled("%s", isInput ? "Inputs" : "Outputs");
                const auto& list = isInput ? graph_.functions[fi].inputs : graph_.functions[fi].outputs;
                for (usize pi = 0; pi < list.size(); ++pi) {
                    ImGui::PushID(static_cast<int>(side * 1000 + pi));
                    ImGui::TextUnformatted(list[pi].name.c_str());
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", list[pi].type.c_str());
                    ImGui::SameLine();
                    if (ImGui::SmallButton("X")) removeFunctionPin(fname, isInput, list[pi].name);
                    ImGui::PopID();
                }
                ImGui::PushID(side);
                ImGui::SetNextItemWidth(90.0f * dpi);
                ImGui::InputTextWithHint("##newpin", isInput ? "new input" : "new output",
                                          newFuncPinBuf_, sizeof newFuncPinBuf_);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(70.0f * dpi);
                ImGui::Combo("##newpintype", &newFuncPinType_, kTypes, IM_ARRAYSIZE(kTypes));
                ImGui::SameLine();
                if (ImGui::SmallButton("+") && newFuncPinBuf_[0] != 0) {
                    addFunctionPin(fname, isInput, newFuncPinBuf_, kTypes[newFuncPinType_]);
                    newFuncPinBuf_[0] = 0;
                }
                ImGui::PopID();
            }

            // ---- delete. deleteFunction raises its own rejection banner naming every caller, the
            // same way deleteVariable does -- so this is one line and no error handling.
            if (ImGui::SmallButton("Delete function")) deleteFunction(fname);
            ImGui::TreePop();
        }
        ImGui::PopID();
        ImGui::Separator();
    }

    if (ImGui::Button("+ New Function")) {
        const std::string made = addFunction("NewFunction");
        if (!made.empty()) setCurrentSubgraph(made);   // a new function you cannot see is not made
    }
#else
    (void)dpi;
#endif
}

// Screen-space margin left around the content when framing. Enough that a node on the edge of the
// graph does not sit flush against the canvas border, where its pins would be unclickable.

bool GraphEditor::contentBounds(float dpi, Vec2* outMin, Vec2* outMax) {
    recomputeLayouts(dpi);
    bool any = false;
    Vec2 lo{}, hi{};
    const auto grow = [&](Vec2 a, Vec2 b) {
        if (!any) { lo = a; hi = b; any = true; return; }
        lo.x = std::min(lo.x, a.x); lo.y = std::min(lo.y, a.y);
        hi.x = std::max(hi.x, b.x); hi.y = std::max(hi.y, b.y);
    };
    for (const auto& nl : layouts_) grow(nl.min, nl.max);
    // Comment boxes count as content too -- often the largest thing on the canvas, and cropping one
    // would look like the box was resized rather than the view moved.
    for (const auto& c : graph_.comments) {
        grow(Vec2{static_cast<f32>(c.x), static_cast<f32>(c.y)},
             Vec2{static_cast<f32>(c.x + c.w), static_cast<f32>(c.y + c.h)});
    }
    if (!any) return false;
    if (outMin) *outMin = lo;
    if (outMax) *outMax = hi;
    return true;
}

bool GraphEditor::selectionBounds(float dpi, Vec2* outMin, Vec2* outMax) {
    recomputeLayouts(dpi);
    bool any = false;
    Vec2 lo{}, hi{};
    const auto grow = [&](Vec2 a, Vec2 b) {
        if (!any) { lo = a; hi = b; any = true; return; }
        lo.x = std::min(lo.x, a.x); lo.y = std::min(lo.y, a.y);
        hi.x = std::max(hi.x, b.x); hi.y = std::max(hi.y, b.y);
    };
    for (const auto& nl : layouts_) {
        if (std::find(selectedNodes_.begin(), selectedNodes_.end(), nl.nodeId) == selectedNodes_.end()) continue;
        grow(nl.min, nl.max);
    }
    for (const auto& c : graph_.comments) {
        if (c.id != selectedComment_) continue;
        grow(Vec2{static_cast<f32>(c.x), static_cast<f32>(c.y)},
             Vec2{static_cast<f32>(c.x + c.w), static_cast<f32>(c.y + c.h)});
    }
    if (!any) return false;
    if (outMin) *outMin = lo;
    if (outMax) *outMax = hi;
    return true;
}

void GraphEditor::frameAll(Vec2 viewportPx, float dpi) {
    Vec2 lo, hi;
    if (!contentBounds(dpi, &lo, &hi)) return;
    // Same zoom limits the wheel enforces, passed in rather than re-declared, so a framed view is
    // always one the wheel could also reach (otherwise it would snap on the next scroll notch).
    view_ = frameTransform(lo, hi, viewportPx, kFramePaddingPx * dpi, minZoomForDpi(dpi), kMaxZoom);
}

void GraphEditor::frameSelection(Vec2 viewportPx, float dpi) {
    Vec2 lo, hi;
    if (!selectionBounds(dpi, &lo, &hi)) { frameAll(viewportPx, dpi); return; }
    // A single selected node would otherwise frame to its own size and zoom to the 4x ceiling, losing
    // every neighbour and the wire being followed -- padded out to a minimum for context.
    const f32 minSpan = 420.0f * dpi;
    const f32 padX = std::max(0.0f, (minSpan - (hi.x - lo.x)) * 0.5f);
    const f32 padY = std::max(0.0f, (minSpan * 0.6f - (hi.y - lo.y)) * 0.5f);
    lo.x -= padX; hi.x += padX;
    lo.y -= padY; hi.y += padY;
    view_ = frameTransform(lo, hi, viewportPx, kFramePaddingPx * dpi, minZoomForDpi(dpi), kMaxZoom);
}

bool GraphEditor::applyAutoLayout(float dpi) {
    if (graph_.nodes.empty()) return false;
    pushUndo();
    const auto pos = autoLayoutPositions(graph_, style_, dpi);
    for (const auto& kv : pos) {
        displayPos_[kv.first] = kv.second;
        for (auto& n : graph_.nodes) {
            if (n.id != kv.first) continue;
            n.x = kv.second.x;
            n.y = kv.second.y;
            // A node with no position now has one -- the intended effect of pressing the button, and
            // the one thing runAutoLayoutIfUnpositioned refuses to do behind the user's back.
            n.hasPosition = true;
            break;
        }
    }
    // Positions are the file's now, not an overlay, so draw()'s DPI-change re-layout must stop
    // rewriting them or it would fight every position the author just asked to keep.
    autoLaidOut_ = false;
    dirty_ = true;
    return true;
}

void GraphEditor::copySelection() {
    if (selectedNodes_.empty()) return;
    const ClipPayload clip = collectClip(graph_, selectedNodes_);
    if (clip.empty()) return;
    const std::string text = serializeClip(clip);
    clipStore().text = text;
#if AVER_WITH_IMGUI
    ImGui::SetClipboardText(text.c_str());
#endif
}


bool GraphEditor::clipboardEmpty() const {
#if AVER_WITH_IMGUI
    if (looksLikeClip(ImGui::GetClipboardText())) return false;
#endif
    return !looksLikeClip(clipStore().text.c_str());
}

void GraphEditor::pasteClipboard(Vec2 canvasPos) {
    // The OS clipboard first -- another tab, another editor process or a previous run may have put a clip
    // there -- then the process store, which is all a headless run has. Foreign text on the OS clipboard
    // falls through to the store rather than pasting nothing: the last COPY here still means something.
    std::optional<ClipPayload> clip;
#if AVER_WITH_IMGUI
    if (const char* os = ImGui::GetClipboardText()) clip = parseClip(os);
#endif
    if (!clip) clip = parseClip(clipStore().text);
    if (!clip) return;
    pastePayload(*clip, canvasPos);
}

void GraphEditor::pastePayload(const ClipPayload& clip, Vec2 canvasPos) {
    // Decided before anything is touched, so a refusal costs no undo step.
    const PastePlan plan = planPaste(clip, graph_);
    if (!plan.ok) {
        showRejectionBanner("Paste refused: " + plan.refusal);
        return;
    }
    pushUndo();

    // A paste is exactly a palette drop of the same nodes plus their internal links -- deliberate, so it
    // invents no new semantics: the model remaps ids and links and declares missing variables; what is
    // left here is what addNodeFromCatalog also does after pushing (ENTRY for events, the positions
    // overlay), for the same reasons.
    PasteResult pasted = applyPaste(graph_, clip, canvasPos.x, canvasPos.y, currentSubgraph_,
                                    [this](const std::string& type) { return makeUniqueNodeId(type); });
    for (const std::string& id : pasted.newIds) {
        fmt::OcGraphNode* n = nullptr;
        for (auto& cand : graph_.nodes) if (cand.id == id) { n = &cand; break; }
        if (!n) continue;
        const GraphNodeDesc* desc = findGraphNodeDescIn(n->type, openGraphDomain());
        if (desc && desc->category == "Event") {
            std::string eventName = desc->typeId;
            if (desc->typeId == "CustomEvent") {
                eventName = makeUniqueEventName("MyEvent");
                setNodeAttribute(*n, "name", eventName);
            } else if (desc->typeId == "OnInputAction") {
                eventName = inputActionEntryName(*n);
            }
            syncEventEntry(n->id, eventName);
        }
        displayPos_[n->id] = Vec2{static_cast<f32>(n->x), static_cast<f32>(n->y)};
    }

    // The paste becomes the selection, so it can be dragged into place immediately, and a second
    // Ctrl+V doesn't silently stack a third copy on a selection the user thinks is the second.
    selectedNodes_ = pasted.newIds;
    selectedLink_ = -1;
    dirty_ = true;

    const std::string note = describePaste(pasted.plan);
    if (!note.empty()) showRejectionBanner(note);
}

void GraphEditor::duplicateSelection() {
    // Duplicate is copy+paste that does NOT disturb the clipboard: a user who copied one thing, then
    // duplicated another, still has the first on the clipboard. The offset is a nudge rather than a
    // position, so the copy is visibly on top of but not exactly over its original.
    if (selectedNodes_.empty()) return;
    const ClipPayload clip = collectClip(graph_, selectedNodes_);
    if (clip.empty()) return;
    Vec2 at{static_cast<f32>(clip.nodes[0].x), static_cast<f32>(clip.nodes[0].y)};
    for (const fmt::OcGraphNode& n : clip.nodes) {
        at.x = n.x < at.x ? static_cast<f32>(n.x) : at.x;
        at.y = n.y < at.y ? static_cast<f32>(n.y) : at.y;
    }
    at.x += 40.0f;
    at.y += 40.0f;
    pastePayload(clip, at);
}

bool GraphEditor::selectionHasLinks() const {
    return std::any_of(graph_.links.begin(), graph_.links.end(), [&](const fmt::OcGraphLink& l) {
        return std::find(selectedNodes_.begin(), selectedNodes_.end(), l.sourceNode) != selectedNodes_.end()
            || std::find(selectedNodes_.begin(), selectedNodes_.end(), l.destNode) != selectedNodes_.end();
    });
}

void GraphEditor::breakLinksOnSelection() {
    if (selectedNodes_.empty() || !selectionHasLinks()) return;   // never an undo step that changes nothing
    pushUndo();
    graph_.links.erase(
        std::remove_if(graph_.links.begin(), graph_.links.end(), [&](const fmt::OcGraphLink& l) {
            return std::find(selectedNodes_.begin(), selectedNodes_.end(), l.sourceNode) != selectedNodes_.end()
                || std::find(selectedNodes_.begin(), selectedNodes_.end(), l.destNode) != selectedNodes_.end();
        }),
        graph_.links.end());
    // The link selection is an INDEX into graph_.links, so it means something different now that the
    // vector has shrunk -- and would point at an unrelated wire, or past the end. Cleared, the same
    // thing deleteSelection does after it prunes.
    selectedLink_ = -1;
    dirty_ = true;
}

void GraphEditor::deleteSelection() {
    // A selected comment box is deleted on its own, BEFORE the node/link work, then returns -- not
    // folded into the same undo step, since a box and a node selection are never both live at once
    // (clicking either clears the other) and deleteComment already pushes its own undo.
    if (!selectedComment_.empty()) { deleteComment(selectedComment_); return; }
    if (selectedNodes_.empty() && selectedLink_ < 0) return;
    pushUndo();

    // Links touching a deleted node must go too, or the file would ship a dangling reference the
    // parser's own LINK validation (modules/formats/src/OcGraph.cpp:166-178) would refuse to reload.
    auto touchesSelection = [&](const fmt::OcGraphLink& l) {
        for (const auto& id : selectedNodes_)
            if (l.sourceNode == id || l.destNode == id) return true;
        return false;
    };
    std::vector<fmt::OcGraphLink> keptLinks;
    keptLinks.reserve(graph_.links.size());
    for (usize i = 0; i < graph_.links.size(); ++i) {
        if (static_cast<int>(i) == selectedLink_) continue;
        if (touchesSelection(graph_.links[i])) continue;
        keptLinks.push_back(graph_.links[i]);
    }
    graph_.links = std::move(keptLinks);

    if (!selectedNodes_.empty()) {
        std::vector<fmt::OcGraphNode> keptNodes;
        keptNodes.reserve(graph_.nodes.size());
        for (auto& n : graph_.nodes) {
            const bool isSelected = std::find(selectedNodes_.begin(), selectedNodes_.end(), n.id) != selectedNodes_.end();
            if (isSelected) { displayPos_.erase(n.id); continue; }
            keptNodes.push_back(std::move(n));
        }
        graph_.nodes = std::move(keptNodes);
        for (const auto& id : selectedNodes_) debugSession().breakpoints.remove(debugGraphName(), id);
        persistBreakpoints();

        // ENTRY AND OUT RECORDS NAMING A DELETED NODE GO TOO, for the same reason as links above --
        // this half was missing. Deleting the node an `ENTRY tick OnTick` pointed at used to save
        // cleanly and then refuse to reopen with "ENTRY references non-existent node: tick".
        const auto wasDeleted = [&](const std::string& id) {
            return std::find(selectedNodes_.begin(), selectedNodes_.end(), id) != selectedNodes_.end();
        };
        std::vector<std::pair<std::string, std::string>> keptEntries;
        for (const auto& e : graph_.entryPoints) if (!wasDeleted(e.first)) keptEntries.push_back(e);
        graph_.entryPoints = std::move(keptEntries);
        std::vector<std::pair<std::string, std::string>> keptOutputs;
        for (const auto& o : graph_.outputs) if (!wasDeleted(o.first)) keptOutputs.push_back(o);
        graph_.outputs = std::move(keptOutputs);
    }

    selectedNodes_.clear();
    selectedLink_ = -1;
    dirty_ = true;
}

void GraphEditor::commitLink(const std::string& srcNode, const std::string& srcPin,
                              const std::string& dstNode, const std::string& dstPin) {
    // THE EDITOR REFUSES EXACTLY WHAT THE COMPILER REFUSES, and no more. In a material graph a scalar
    // splats into a float3 and a float4 truncates into one -- both compile silently (see widen() in
    // MaterialGraphHlsl.cpp), so refusing them here would forbid wiring that works, reading as a
    // broken palette. float2 into float3 stays refused on both sides, since inventing the third
    // component is the compiler guessing. Gameplay graphs keep exact-match, unchanged.
    const GraphLinkCheck check = canConnectPins(graph_, srcNode, srcPin, dstNode, dstPin,
                                                openGraphDomain() == kDomainMaterial
                                                    ? &materialPinTypeMatch
                                                    : &exactPinTypeMatch);
    if (!check.ok) {
        reportLinkRejection(check);
        return;
    }
    pushUndo();
    fmt::OcGraphLink link;
    link.sourceNode = srcNode; link.sourcePin = srcPin;
    link.destNode = dstNode; link.destPin = dstPin;
    graph_.links.push_back(link);
    dirty_ = true;
}

void GraphEditor::selectNodes(const std::vector<std::string>& nodeIds) {
    selectedNodes_.clear();
    for (const std::string& id : nodeIds)
        for (const auto& n : graph_.nodes)
            if (n.id == id) { selectedNodes_.push_back(id); break; }
    selectedLink_ = -1;
}

bool GraphEditor::selectNode(const std::string& nodeId) {
    for (const auto& n : graph_.nodes) {
        if (n.id != nodeId) continue;
        selectedNodes_ = {nodeId};
        selectedLink_ = -1;
        return true;
    }
    // FALLS BACK TO A COMPONENT: a graph holds two kinds of selectable thing and the caller naming one
    // by id shouldn't have to know which. Node and component ids live in separate namespaces --
    // nothing stops a NODE `gun` and a COMP `gun` -- so nodes are tried FIRST.
    for (const auto& c : graph_.components) {
        if (c.id != nodeId) continue;
        selectedComponent_ = nodeId;
        return true;
    }
    return false;
}

bool GraphEditor::setAttribute(const std::string& nodeId, const std::string& key, const std::string& value) {
    // No quoting in this format (OcGraph.cpp's splitWhitespace / OcGraphParser.cs's SplitWhitespace)
    // means a whitespace-containing value would truncate silently on write, becoming invisible junk
    // tokens no reader surfaces again -- refused here instead, the narrowest fix that doesn't touch
    // the shared reader/writer or add escaping to the format.
    //
    // containsWhitespace matches isSpace() (TextScan.hpp) exactly, including '\v'/'\f' an earlier
    // version missed. KNOWN GAP: OcGraphParser.cs's SplitWhitespace also treats Unicode whitespace
    // (e.g. NBSP) as a delimiter, which isSpace() does not -- such a value round-trips here but still
    // corrupts on the C# side. Needs a shared whitespace definition or real quoting; out of scope here.
    if (containsWhitespace(value)) return false;
    for (auto& n : graph_.nodes) {
        if (n.id != nodeId) continue;
        pushUndo();
        setNodeAttribute(n, key, value); // GraphEditorGeometry.hpp -- the order-preserving read/write
        // A CustomEvent's `name=` is half of a pair -- see syncEventEntry, which owns the other
        // half. Renaming the attribute without the record leaves a graph that looks renamed and
        // has silently stopped firing.
        if (key == "name" && n.type == "CustomEvent") syncEventEntry(n.id, value);
        // OnInputAction's ENTRY is Input.<action>.<event>: either half changing recomposes it.
        if ((key == "action" || key == "event") && n.type == "OnInputAction") syncEventEntry(n.id, inputActionEntryName(n));
        // A Const node writes its literal twice: the NODE line's `value=` (this row) and the output
        // PIN's default (written at spawn). OcGraphParser.cs turns both into a ConstantOutput for the
        // same node id and the compiler takes FirstOrDefault, so the attribute wins today only because
        // NODE precedes its own PIN lines -- correct by emission order alone, and fragile to it: a
        // writer that ever grouped PIN records differently would silently flip every constant in every
        // graph back to its spawn default. Writing both here keeps the file self-consistent regardless
        // of order.
        if (key == "value") {
            for (fmt::OcGraphPin& p : n.pins)
                if (p.isOutput && p.name == "value") p.defaultValue = value;
        }
        dirty_ = true;
        return true;
    }
    return false;
}

bool GraphEditor::clearAttribute(const std::string& nodeId, const std::string& key) {
    for (auto& n : graph_.nodes) {
        if (n.id != nodeId) continue;
        if (!getNodeAttribute(n, key).found) return false; // nothing to clear is not an edit
        pushUndo();
        removeNodeAttribute(n, key);
        // The ENTRY record goes with it (setAttribute's reason in reverse): an ENTRY with no event
        // name can't be expressed, so clearing the name means the node stops being an entry point.
        if (key == "name" && n.type == "CustomEvent") syncEventEntry(n.id, "");
        if ((key == "action" || key == "event") && n.type == "OnInputAction") syncEventEntry(n.id, "");
        dirty_ = true;
        return true;
    }
    return false;
}

void GraphEditor::showRejectionBanner(const std::string& msg) {
    lastRejectMsg_ = msg;
#if AVER_WITH_IMGUI
    lastRejectAtSec_ = ImGui::GetTime();
#endif
    AVER_WARN("[GraphEditor] {}", msg);
}

void GraphEditor::reportLinkRejection(const GraphLinkCheck& check) {
    // "Connection refused: " lives here, not in draw()'s overlay code, so a banner reused for an
    // unrelated refusal (e.g. deleteVariable) isn't mislabelled -- draw() shows lastRejectMsg_ verbatim.
    showRejectionBanner("Connection refused: " + (check.message.empty() ? std::string("link refused") : check.message));
}

// See GraphEditor.hpp's own header comment on this block for the bug these five methods close.

std::string GraphEditor::makeUniqueVariableName(const std::string& base) const {
    for (int i = 1; i < 1000000; ++i) {
        const std::string candidate = base + std::to_string(i);
        bool taken = false;
        for (const auto& v : graph_.variables) if (v.name == candidate) { taken = true; break; }
        if (!taken) return candidate;
    }
    return base + "_x"; // unreachable in practice, mirrors makeUniqueNodeId's own fallback
}

bool GraphEditor::addVariable(const std::string& name, const std::string& type, const std::string& defaultValue) {
    if (name.empty() || containsWhitespace(name)) return false;
    if (containsWhitespace(defaultValue)) return false;
    for (const auto& v : graph_.variables) if (v.name == name) return false; // Graph.Validate's own uniqueness rule
    pushUndo();
    fmt::OcGraphVariable var;
    var.name = name;
    var.type = isValidVarType(type) ? type : "float"; // "float": the palette's own GetVar/SetVar default
    var.defaultValue = defaultValue;
    graph_.variables.push_back(std::move(var));
    dirty_ = true;
    return true;
}

bool GraphEditor::renameVariable(const std::string& oldName, const std::string& newName) {
    if (newName.empty() || containsWhitespace(newName)) return false;
    fmt::OcGraphVariable* var = nullptr;
    for (auto& v : graph_.variables) if (v.name == oldName) { var = &v; break; }
    if (!var) return false;
    if (newName == oldName) return true; // a no-op "rename" is not a failure, just nothing to do
    for (const auto& v : graph_.variables) {
        if (&v != var && v.name == newName) return false; // would collide with a DIFFERENT variable
    }
    pushUndo();
    var->name = newName;
    // A bare `var->name = newName` would leave every GetVar/SetVar naming `oldName` still naming it,
    // so Graph.Validate() would refuse the next C# compile of a graph the panel just showed renamed.
    // Every reference is rewritten here, in the SAME pushUndo() step.
    for (auto& n : graph_.nodes) {
        const GraphNodeAttribute a = getNodeAttribute(n, "var");
        if (a.found && a.value == oldName) setNodeAttribute(n, "var", newName);
    }
    dirty_ = true;
    return true;
}

bool GraphEditor::retypeVariable(const std::string& name, const std::string& newType) {
    fmt::OcGraphVariable* var = nullptr;
    for (auto& v : graph_.variables) if (v.name == name) { var = &v; break; }
    if (!var) return false;
    const std::string t = isValidVarType(newType) ? newType : "float";
    if (var->type == t) return true; // no-op, not a failure
    pushUndo();
    var->type = t;
    // Deliberately does not touch any node's pins: a palette-spawned GetVar/SetVar's own 'value' pin
    // is typed at spawn, so a later retype can leave it disagreeing. Silently rewriting it was
    // rejected -- a wired link could break with no visible cause -- so this surfaces as a live
    // mismatch note in the Variables panel instead (computed fresh every frame).
    dirty_ = true;
    return true;
}

bool GraphEditor::setVariableDefault(const std::string& name, const std::string& defaultValue) {
    if (containsWhitespace(defaultValue)) return false;
    fmt::OcGraphVariable* var = nullptr;
    for (auto& v : graph_.variables) if (v.name == name) { var = &v; break; }
    if (!var) return false;
    if (var->defaultValue == defaultValue) return true; // no-op, not a failure
    pushUndo();
    var->defaultValue = defaultValue;
    dirty_ = true;
    return true;
}

bool GraphEditor::deleteVariable(const std::string& name, std::vector<std::string>* outBlockedBy) {
    bool exists = false;
    for (const auto& v : graph_.variables) if (v.name == name) { exists = true; break; }
    if (!exists) return false;

    // Refuse rather than silently orphan: see this method's own header comment (GraphEditor.hpp) for
    // why a delete that leaves nodes pointing at a variable that no longer exists is worse than no
    // delete at all.
    std::vector<std::string> referencing;
    for (const auto& n : graph_.nodes) {
        const GraphNodeAttribute a = getNodeAttribute(n, "var");
        if (a.found && a.value == name) referencing.push_back(n.id);
    }
    if (!referencing.empty()) {
        if (outBlockedBy) *outBlockedBy = referencing;
        std::string msg = "cannot delete variable '" + name + "': still referenced by ";
        for (usize i = 0; i < referencing.size(); ++i) { if (i) msg += ", "; msg += referencing[i]; }
        showRejectionBanner(msg);
        return false;
    }

    pushUndo();
    graph_.variables.erase(std::remove_if(graph_.variables.begin(), graph_.variables.end(),
                                           [&](const fmt::OcGraphVariable& v) { return v.name == name; }),
                            graph_.variables.end());
    dirty_ = true;
    return true;
}

// ---- Problems ---------------------------------------------------------------------------------------------------

bool GraphEditor::runDiagnosticsNow() {
    // A material graph is another language; the managed checks mean nothing to it.
    if (!diagnose_ || openGraphDomain() == kDomainMaterial) return false;
    // Marked as run even on failure: a bridge that cannot answer should be asked again after the NEXT
    // edit, not every frame until it does.
    diagRunSerial_ = editSerial_;
    std::string wire;
    if (!diagnose_(serializeForSave(), wire)) return false;
    ProblemList list = parseProblemWire(wire);
    sortProblems(list.items);
    problemsCut_ = list.more;
    problems_ = std::move(list.items);
    badges_ = buildBadgeMap(problems_);
    return true;
}

void GraphEditor::tickDiagnostics() {
    if (!diagnose_ || !loaded_) return;
    if (diagnosticsDue(editSerial_, diagRunSerial_, lastEditAtSec_, clockSec())) runDiagnosticsNow();
}

bool GraphEditor::selectProblem(usize index) {
    if (index >= problems_.size() || problems_[index].nodeId.empty()) return false;
    return revealNode(problems_[index].nodeId);
}

// ---- Find and reveal --------------------------------------------------------------------------------------------

bool GraphEditor::revealNode(const std::string& nodeId) {
    for (const auto& n : graph_.nodes) {
        if (n.id != nodeId) continue;
        setCurrentSubgraph(subgraphOf(n));   // clears the selection and re-frames; both overridden below
        selectedNodes_ = {nodeId};
        selectedLink_ = -1;
        selectedComment_.clear();
        revealPending_ = true;
        return true;
    }
    return false;
}

void GraphEditor::revealFindHit(const FindHit& hit) {
    switch (hit.target) {
    case FindTarget::Node:
        revealNode(hit.id);
        break;
    case FindTarget::Function:
        setCurrentSubgraph(hit.id);
        break;
    case FindTarget::Comment:
        for (const auto& c : graph_.comments) {
            if (c.id != hit.id) continue;
            setCurrentSubgraph({});
            selectedNodes_.clear();
            selectedComment_ = c.id;
            revealPending_ = true;
            break;
        }
        break;
    case FindTarget::Variable: {
        // A variable has no place on the canvas; its first use is the nearest thing to show.
        const std::vector<RefHit> uses = findReferences(graph_, RefKind::Variable, hit.id);
        if (!uses.empty()) revealNode(uses.front().nodeId);
        break;
    }
    }
}

void GraphEditor::setFindQuery(const std::string& query) {
    findLastQuery_ = query;
    const GraphNodeDomain domain = openGraphDomain();
    find_.reset(findInGraph(graph_, query, FindScope::All, [domain](const std::string& type) {
        const GraphNodeDesc* d = findGraphNodeDescIn(type, domain);
        return d ? d->displayName : std::string();
    }));
    if (const FindHit* h = find_.current()) revealFindHit(*h);
}

bool GraphEditor::findNext() {
    const FindHit* h = find_.next();
    if (!h) return false;
    revealFindHit(*h);
    return true;
}

bool GraphEditor::findPrevious() {
    const FindHit* h = find_.previous();
    if (!h) return false;
    revealFindHit(*h);
    return true;
}

void GraphEditor::showReferences(RefKind kind, const std::string& name, bool wholeProject) {
    refHits_ = findReferences(graph_, kind, name);
    refProjectHits_.clear();
    if (wholeProject) {
        const std::string owner = findOwnerProjectForGraph(path_);
        fmt::ProjectDesc proj;
        if (!owner.empty() && fmt::loadOcproject(owner, proj, nullptr)) {
            for (ProjectRefHit& h : findInProject(proj.contentDir(), kind, name)) {
                std::error_code ec;
                if (std::filesystem::equivalent(h.file, path_, ec)) continue;   // already listed above
                refProjectHits_.push_back(std::move(h));
            }
        }
    }
    static const char* const kKinds[] = {"variable", "function", "event", "class"};
    refTitle_ = std::string("References to ") + kKinds[static_cast<int>(kind)] + " '" + name + "'" +
                (wholeProject ? " (whole project)" : "");
    bottomOpen_ = true;
    bottomTabRequest_ = 1;
}

// ---- Debugger ---------------------------------------------------------------------------------------------------

void GraphEditor::persistBreakpoints() {
#if AVER_WITH_IMGUI
    setPrefString("graphEditor.breakpoints." + debugGraphName(),
                  debugSession().breakpoints.serializeGraph(debugGraphName()));
    flushEditorPrefs();
#endif
}

bool GraphEditor::toggleBreakpointOnSelection() {
    if (selectedNodes_.size() != 1) return false;
    debugSession().breakpoints.toggle(debugGraphName(), selectedNodes_.front());
    persistBreakpoints();
    return true;
}

bool GraphEditor::hasBreakpoint(const std::string& nodeId) const {
    return debugSession().breakpoints.has(debugGraphName(), nodeId);
}

bool GraphEditor::breakpointEnabled(const std::string& nodeId) const {
    return debugSession().breakpoints.enabled(debugGraphName(), nodeId);
}

bool GraphEditor::setBreakpointEnabled(const std::string& nodeId, bool on) {
    if (!debugSession().breakpoints.setEnabled(debugGraphName(), nodeId, on)) return false;
    persistBreakpoints();
    return true;
}

// ---- Collapse to Function ---------------------------------------------------------------------------------------

CollapseResult GraphEditor::collapseSelectionToFunction(const std::string& name) {
    CollapseOptions options;
    const GraphNodeDomain domain = openGraphDomain();
    options.isEventType = [domain](const std::string& type) {
        const GraphNodeDesc* d = findGraphNodeDescIn(type, domain);
        return d && d->category == "Event";
    };

    // Done on a copy and committed after the undo snapshot: a refusal must leave no undo step behind.
    fmt::OcGraphData next = graph_;
    CollapseResult r = collapseToFunction(next, selectedNodes_, name, options);
    if (!r.ok) {
        showRejectionBanner("Cannot collapse: " + r.reason);
        return r;
    }

    pushUndo();
    graph_ = std::move(next);
    displayPos_[r.entryNodeId] = Vec2{static_cast<f32>(r.entryX), static_cast<f32>(r.entryY)};
    if (!r.returnNodeId.empty()) displayPos_[r.returnNodeId] = Vec2{static_cast<f32>(r.returnX), static_cast<f32>(r.returnY)};
    displayPos_[r.callNodeId] = Vec2{static_cast<f32>(r.callX), static_cast<f32>(r.callY)};
    // The three new nodes' pins restate the FUNC record, so they are marked synthesised (and stripped on
    // save) like any function node's -- the same call every signature edit makes.
    resyncFunctionNodePins(r.functionName);
    selectedNodes_ = {r.callNodeId};
    selectedLink_ = -1;
    selectedComment_.clear();
    dirty_ = true;
    return r;
}

// ---- the bottom panel: Problems and References -----------------------------------------------------------

void GraphEditor::drawBottomPanel(float width, float height, float dpi) {
#if AVER_WITH_IMGUI
    ImGui::BeginChild("##graphBottom", ImVec2(width, height), true);
    if (ImGui::BeginTabBar("##graphBottomTabs")) {
        const ProblemCounts counts = countProblems(problems_);
        const std::string problemsLabel = "Problems (" + problemSummary(counts) + ")###problems";
        if (ImGui::BeginTabItem(problemsLabel.c_str(), nullptr, bottomTabRequest_ == 0 ? ImGuiTabItemFlags_SetSelected : 0)) {
            if (!diagnose_) {
                ImGui::TextDisabled("The .NET bridge exports no GraphDiagnose, so only Validate can check this graph.");
            } else if (problems_.empty()) {
                ImGui::TextDisabled("Nothing wrong with this graph.");
            }
            for (usize i = 0; i < problems_.size(); ++i) {
                const GraphProblem& p = problems_[i];
                const bool isError = p.severity == ProblemSeverity::Error;
                ImGui::PushID(static_cast<int>(i));
                ImGui::TextColored(isError ? ImVec4(0.95f, 0.45f, 0.40f, 1.0f) : ImVec4(0.95f, 0.78f, 0.35f, 1.0f),
                                   "%s", isError ? "error" : "warning");
                ImGui::SameLine(72.0f * dpi);
                const std::string row = (p.nodeId.empty() ? std::string("(graph)") : p.nodeId) + "   " + p.message;
                if (ImGui::Selectable(row.c_str(), false) && !p.nodeId.empty()) selectProblem(i);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s", p.code.c_str(), p.nodeId.empty() ? "" : "  -  click to select the node");
                ImGui::PopID();
            }
            if (problemsCut_ > 0) ImGui::TextDisabled("...and %zu more not shown", problemsCut_);
            ImGui::EndTabItem();
        }
        if (!refTitle_.empty()) {
            if (ImGui::BeginTabItem("References###refs", nullptr, bottomTabRequest_ == 1 ? ImGuiTabItemFlags_SetSelected : 0)) {
                ImGui::TextDisabled("%s", refTitle_.c_str());
                if (refHits_.empty() && refProjectHits_.empty()) ImGui::TextDisabled("No uses found.");
                for (usize i = 0; i < refHits_.size(); ++i) {
                    ImGui::PushID(static_cast<int>(i));
                    const std::string row = refHits_[i].nodeId + "   " + refHits_[i].what;
                    if (ImGui::Selectable(row.c_str(), false)) revealNode(refHits_[i].nodeId);
                    ImGui::PopID();
                }
                for (usize i = 0; i < refProjectHits_.size(); ++i) {
                    ImGui::PushID(static_cast<int>(1000000 + i));
                    const ProjectRefHit& h = refProjectHits_[i];
                    const std::string row = std::filesystem::path(h.file).filename().string() + "   " + h.nodeId + "   " + h.what;
                    if (ImGui::Selectable(row.c_str(), false)) revealGraphNode(h.file, h.nodeId);
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", h.file.c_str());
                    ImGui::PopID();
                }
                ImGui::EndTabItem();
            }
        }
        ImGui::EndTabBar();
    }
    bottomTabRequest_ = -1;
    ImGui::EndChild();
#else
    (void)width; (void)height; (void)dpi;
#endif
}

void GraphEditor::recomputeLayouts(float dpi) {
    layouts_.clear();
    layouts_.reserve(graph_.nodes.size());
    for (const auto& n : graph_.nodes) {
        // Only the subgraph on screen: layouts_ is what drawing, hit-testing, box-select and framing
        // all read, so filtering here scopes all four at once with no second place to disagree.
        if (subgraphOf(n) != currentSubgraph_) continue;
        fmt::OcGraphNode display = n; // DISPLAY position (a live drag only touches displayPos_), so
                                       // drawing follows a drag without touching graph_ until commit.
        auto it = displayPos_.find(n.id);
        if (it != displayPos_.end()) { display.x = it->second.x; display.y = it->second.y; }
        const GraphNodeDesc* desc = findGraphNodeDesc(n.type);
        // The SAME string the header will draw -- computing width from "Get Var" while drawing
        // "PlayerSpeed" would clip it.
        const std::string title = nodeTitle(n, desc, graph_.variables);
        // `dpi` only, NOT dpi*zoom: canvas-space geometry is fixed-logical-scale, and
        // CanvasTransform::zoom applies uniformly afterward (canvasToScreen/screenToCanvas) --
        // folding zoom in here too would double-apply it.
        layouts_.push_back(computeNodeLayout(display, title, style_, dpi));
    }
}


void GraphEditor::draw(Engine& e) {
#if AVER_WITH_IMGUI
    const float dpi = g_graphEditorDpi;
    // An auto-laid-out graph is spaced for one DPI; if that changed (opened before applyDpi ran, or
    // moved monitors), redo it or boxes collide. Only auto-layout is redone -- user-dragged or
    // file positions stay untouched.
    if (autoLaidOut_ && dpi != autoLayoutDpi_) {
        autoLayoutDpi_ = dpi;
        const auto pos = autoLayoutPositions(graph_, style_, dpi);
        for (const auto& kv : pos) displayPos_[kv.first] = kv.second;
    }

    if (!loaded_) {
        ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.45f, 1.0f), "This file could not be read.");
        ImGui::Separator();
        ImGui::TextWrapped("%s", loadError_.c_str());
        return;
    }

    if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                   keybinds().pressed(CommandId::AssetSave, ImGui::GetIO()))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[GraphEditor] save failed for '{}': {}", path_, why);
        // Checked on save, but the save still happens: a half-built graph is normal mid-build, and
        // refusing to write it would be unusable. Result is reported, not enforced.
        else if (validate_) { validateNow(validateErr_, validateNode_); }
    }
    ImGui::SameLine();
    // Also its own button, not just on save: "is this finished" is a question asked mid-build too.
    ImGui::BeginDisabled(!validate_ && !diagnose_);
    if (ImGui::Button("Validate")) {
        // The full list first (every error and warning, each with its node), then the one-line result.
        if (runDiagnosticsNow()) { bottomOpen_ = true; bottomTabRequest_ = 0; }
        if (!validate_) {
            // Diagnostics alone: the Problems tab is the answer.
        } else if (validateNow(validateErr_, validateNode_)) {
            validateErr_.clear();
            validateNode_.clear();
            showRejectionBanner("graph is valid");
        } else {
            showRejectionBanner(validateErr_);
        }
    }
    ImGui::EndDisabled();
    if (!validate_ && !diagnose_ && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The .NET bridge exports no GraphValidate, so nothing can check this graph.");
    ImGui::SameLine();
    // Standing result, so re-pressing Validate isn't needed to remember what was wrong; cleared by the
    // next Validate or by any edit (see pushUndo).
    if (!validateErr_.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "%s%s",
                           validateNode_.empty() ? "" : (validateNode_ + ": ").c_str(),
                           validateErr_.c_str());
        ImGui::SameLine();
    }
    if (diagnose_) {
        // The standing answer to "is this graph finished"; click it to open the list.
        const ProblemCounts counts = countProblems(problems_);
        ImGui::PushStyleColor(ImGuiCol_Text, counts.errors ? ImVec4(0.95f, 0.45f, 0.40f, 1.0f)
                                           : counts.warnings ? ImVec4(0.95f, 0.78f, 0.35f, 1.0f)
                                                             : ImVec4(0.60f, 0.80f, 0.60f, 1.0f));
        const std::string summary = problemSummary(counts) + "##problemsSummary";
        if (ImGui::SmallButton(summary.c_str())) {
            bottomOpen_ = !bottomOpen_;
            bottomTabRequest_ = 0;
            setPrefBool("graphEditor.problemsOpen", bottomOpen_);
            flushEditorPrefs();
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click to show or hide the Problems list");
        ImGui::SameLine();
    }
    if (debugValuesSrc_) {
        bool armed = debugSession().armed;
        if (ImGui::Checkbox("Debug", &armed)) {
            debugSession().setArmed(armed);
            setPrefBool("graphEditor.debugArmed", armed);
            flushEditorPrefs();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Arm the graph debugger: F9 puts a breakpoint on the selected node, and wires show their values.\n"
                              "Graphs compile when Play starts, so this takes effect on the NEXT Play.\n"
                              "A breakpoint pauses the session at the end of the frame its node ran in; Frame Skip steps.");
        ImGui::SameLine();
    }
    ImGui::BeginDisabled(!history_.canUndo());
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!history_.canRedo());
    if (ImGui::Button("Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    // Frame All and Auto-Layout are buttons, not just key bindings, so a first-time author can find a
    // graph that opens on an empty grid without already knowing a shortcut.
    // A combo, not a second tab bar, for which subgraph the canvas shows: function count is unbounded
    // and the tab bar above already means something else (DOES vs IS). Absent until a graph declares
    // its first function.
    if (!graph_.functions.empty()) {
        const std::string label = currentSubgraph_.empty() ? std::string("Event Graph") : currentSubgraph_;
        ImGui::SetNextItemWidth(180.0f * dpi);
        if (ImGui::BeginCombo("##graphSubgraph", label.c_str())) {
            if (ImGui::Selectable("Event Graph", currentSubgraph_.empty())) setCurrentSubgraph({});
            for (const auto& f : graph_.functions) {
                const bool sel = (currentSubgraph_ == f.name);
                std::string row = f.name + (f.pure ? "  (pure)" : "");
                if (ImGui::Selectable(row.c_str(), sel)) setCurrentSubgraph(f.name);
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
    }
    if (ImGui::Button("Frame All")) framePendingFromToolbar_ = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fit the whole graph in view  (Home)\n"
                                                   "F frames the selection instead");
    ImGui::SameLine();
    ImGui::BeginDisabled(graph_.nodes.empty());
    if (ImGui::Button("Auto-Layout")) {
        applyAutoLayout(dpi);
        framePendingFromToolbar_ = true;   // a tidy graph you cannot see is not tidy
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Re-place every node in dependency columns, and SAVE those positions.\n"
                           "One undo step. Undo restores exactly where everything was.");
    ImGui::SameLine();
    // Pan hint on the status line, not a tooltip: a tooltip only reaches someone already hovering the
    // thing, backwards for a gesture nobody knew to reach for. Every gesture listed is wired below.
    ImGui::TextDisabled("%s  |  %zu nodes, %zu links, %zu comments  |  zoom %.0f%%  |  pan: right- or middle-drag, or Space+drag  |  C: comment box",
                         path_.c_str(), graph_.nodes.size(), graph_.links.size(),
                         graph_.comments.size(), view_.zoom * 100.0f);

    // The two tabs an actor gets: what it DOES (event graph) and what it IS (component tree,
    // fmt::OcGraphComponent) -- Blueprint's own split, not merely organisational: the two share no
    // canvas, selection, or unit. A non-class graph still gets both tabs: hiding Viewport until a CLASS
    // record exists would mean discovering a graph CAN be an actor requires already knowing it.
    if (ImGui::BeginTabBar("##graphInnerTabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("Event Graph")) {
            drawEventGraph(dpi);
            ImGui::EndTabItem();
        }
        const ImGuiTabItemFlags viewportFlags =
            forceViewportTab_ ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
        forceViewportTab_ = false;
        if (ImGui::BeginTabItem("Viewport", nullptr, viewportFlags)) {
            drawViewport(e, dpi);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
#else
    (void)e;
#endif
}

// Restores the canvas/details ("Gap B") split to its default width and persists it immediately (see
// AssetEditor.hpp's resetLayout() for why "Reset Tab Layout" needs every tab to implement this, not
// just ActorEditor). No-op with ImGui off: a headless build never lays the panels out, so there's
// nothing to restore.
void GraphEditor::resetLayout() {
#if AVER_WITH_IMGUI
    detailsColW_ = 0.0f;
    setPrefFloat(kPrefDetailsColumn, kDefaultDetailsColumn);
    flushEditorPrefs();
#endif
}

// Refreshes schemeActionNames_ from the owning project's INPUT.SCHEME, for the action= attribute
// picker on InputAction/InputActionPressed/InputActionReleased/RebindAction/GetActionKey. Throttled to
// ~once/sec (schemeCacheAtSec_): called every frame the details panel is open, and this editor has no
// file-watch, so it polls -- a stat() a second isn't worth noticing next to a redraw.
//
// Re-parses from disk rather than asking the running game: there is no running game here. The
// framework ABI (aver_fw_input_scheme_load) InputScheme.cs actually loads through is a main-thread
// singleton slot ("Returned strings stay valid until the next aver_fw_input_scheme_load"), shared with
// a live Play session, and this editor's script host may not even be running (e.g. --open-asset with a
// bare path). Calling through it here would race a live session's load or require one to exist;
// reading .ocinput directly via fmt::loadOcinput is the same data with no such dependency.
//
// A scheme file, not the C# binding table: shows what a rebind session would START from (declared
// actions), not live rebind state, which only exists once EnhancedInput has pushed a context -- the
// action= combo is a name picker, not a bound-state readout (GetActionKey's own key pin answers "is it
// bound right now").
void GraphEditor::refreshSchemeActionsIfNeeded() {
#if AVER_WITH_IMGUI
    const double now = ImGui::GetTime();
    if (schemeCacheInited_ && (now - schemeCacheAtSec_) < 1.0) return;
    schemeCacheAtSec_ = now;
    schemeCacheInited_ = true;

    const std::string ownerProject = findOwnerProjectForGraph(path_);
    fmt::ProjectDesc proj;
    if (ownerProject.empty() || !fmt::loadOcproject(ownerProject, proj, nullptr) || proj.inputScheme.empty()) {
        schemeFileFound_ = false;
        schemeActionNames_.clear();
        schemeResolvedPath_.clear();
        return;
    }
    const std::string schemePath = proj.contentDir() + AVER_PATH_SEP + proj.inputScheme;

    std::error_code ec;
    const auto writeTime = std::filesystem::last_write_time(schemePath, ec);
    if (ec) {
        // Named in the manifest but missing/unreadable now -- e.g. mid-edit in another tool.
        schemeFileFound_ = false;
        schemeActionNames_.clear();
        schemeResolvedPath_.clear();
        return;
    }
    if (schemeFileFound_ && schemeResolvedPath_ == schemePath && writeTime == schemeFileWriteTime_)
        return; // unchanged since the last successful parse -- nothing to redo

    fmt::OcInputData scheme;
    if (!fmt::loadOcinput(schemePath, scheme, nullptr)) {
        schemeFileFound_ = false;
        schemeActionNames_.clear();
        schemeResolvedPath_ = schemePath;
        schemeFileWriteTime_ = writeTime;
        return;
    }
    schemeActionNames_.clear();
    schemeActionNames_.reserve(scheme.actions.size());
    for (const fmt::OcInputAction& a : scheme.actions) schemeActionNames_.push_back(a.name);
    schemeFileFound_ = true;
    schemeResolvedPath_ = schemePath;
    schemeFileWriteTime_ = writeTime;
#endif
}




std::string graphStarterText(const std::string& stem) {
    // A raw string literal, so the file reads as the lines it becomes, sidestepping a trap this repo has
    // hit twice: a backslash-n eaten by tooling becomes a real newline in a narrow literal, which MSVC
    // answers with a wall of C2001 "newline in string literal".
    //
    // The class name derives from the stem (NewGraph.ocgraph -> AN_NewGraph), since it must be a legal
    // identifier and the file name need not be; renaming the file does NOT rename the class -- a level
    // placement names the CLASS, changed instead from the Details panel.
    return std::string(R"(OCGRAPH 1
DOMAIN gameplay
# A new Aver Node graph. It declares a spawnable class and does nothing yet.
#
# CLASS is what makes this placeable: a level PLACE line names a class, not a file, so a graph with
# no CLASS record can be opened and edited but never put in a world. `Actor` is the bootstrap base
# every graph class may name for free; `Character` is the other concrete one, and gives you
# CharacterMove with no C# involved.
#
# Right-click the canvas to add an OnStart or OnTick event and give it behaviour.
NAME )") + stem + R"(
DESCRIPTION A new graph.
CLASS AN_)" + stem + R"( Actor
)";
}

namespace {
// The validator handed to every GraphEditor built after setGraphValidator runs. Empty by default,
// which is the honest state in a build with no .NET runtime: validateNow() then reports that nothing
// could check the graph rather than claiming it is fine.
GraphEditor::ValidateFn g_graphValidator;
GraphEditor::NodeHitsFn g_graphNodeHits;
GraphEditor::DiagnoseFn g_graphDiagnoser;
GraphEditor::DebugValuesFn g_graphDebugValues;
} // namespace

void setGraphValidator(GraphEditor::ValidateFn fn) { g_graphValidator = std::move(fn); }
void setGraphNodeHitSource(GraphEditor::NodeHitsFn fn) { g_graphNodeHits = std::move(fn); }
void setGraphDiagnoser(GraphEditor::DiagnoseFn fn) { g_graphDiagnoser = std::move(fn); }
void setGraphDebugValueSource(GraphEditor::DebugValuesFn fn) { g_graphDebugValues = std::move(fn); }
void setGraphOpener(std::function<bool(const std::string& path)> fn) { g_graphOpener = std::move(fn); }

namespace {
GraphEditor* openGraphEditorAt(const std::string& path) {
    for (GraphEditor* g : g_openGraphEditors) {
        std::error_code ec;
        if (g->path() == path || std::filesystem::equivalent(g->path(), path, ec)) return g;
    }
    return nullptr;
}
} // namespace

bool revealGraphNode(const std::string& path, const std::string& nodeId) {
    GraphEditor* g = openGraphEditorAt(path);
    if (!g) {
        if (!g_graphOpener || !g_graphOpener(path)) return false;
        g = openGraphEditorAt(path);   // the opener builds the tab, which registers itself
    }
    return g && g->revealNode(nodeId);
}

std::string graphPathForName(const std::string& graphName) {
    for (const GraphEditor* g : g_openGraphEditors)
        if (g->debugGraphName() == graphName) return g->path();
    return {};
}

usize runGraphDiagnosticsForOpenTabs(usize* warnings) {
    usize errors = 0, warns = 0;
    for (GraphEditor* g : g_openGraphEditors) {
        if (!g->diagnoserInstalled() || g->openGraphDomain() == kDomainMaterial) continue;
        if (!g->runDiagnosticsNow()) continue;
        const ProblemCounts c = countProblems(g->diagnostics());
        errors += c.errors;
        warns += c.warnings;
    }
    if (warnings) *warnings = warns;
    return errors;
}

bool graphCanvasHasFocus() {
#if AVER_WITH_IMGUI
    return ImGui::GetCurrentContext() != nullptr && ImGui::GetFrameCount() - g_graphCanvasFocusFrame <= 1;
#else
    return false;
#endif
}

std::unique_ptr<AssetEditor> makeGraphEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (ext != ".ocgraph") return nullptr;
    auto ed = std::make_unique<GraphEditor>(path);
    if (g_graphValidator) ed->setValidator(g_graphValidator);
    if (g_graphNodeHits)  ed->setNodeHitSource(g_graphNodeHits);
    if (g_graphDiagnoser) ed->setDiagnoser(g_graphDiagnoser);
    if (g_graphDebugValues) ed->setDebugValueSource(g_graphDebugValues);
    return ed;
}


} // namespace aver::editor
