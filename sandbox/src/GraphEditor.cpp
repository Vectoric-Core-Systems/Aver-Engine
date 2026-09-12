// The .ocgraph node editor tab: ImGui canvas on top of GraphEditorGeometry's headless core.
//
// LOAD/SAVE CONTRACT (the two hard requirements from the task brief):
//   1. A load -> save with no edits is byte-identical. Guaranteed structurally, not by luck: graph_
//      is the exact OcGraphData parseOcgraph() produced, and it is mutated ONLY inside the explicit
//      edit paths below (each preceded by pushUndo()). Auto-layout and canvas display never touch it
//      -- see displayPos_'s doc comment on GraphEditor.hpp. save() always calls
//      fmt::writeOcgraph(graph_, originalText_), matching testRoundTrip's own assertion
//      (tests/formats/src/OcGraphTest.cpp:149) that a write of an untouched parse reproduces its
//      input byte for byte.
//   2. Unknown records survive a save. This editor NEVER calls fmt::saveOcgraph() -- that function
//      (modules/formats/src/OcGraph.cpp:376-386) always writes fresh with no `existing` text and
//      would silently drop anything this editor doesn't model, exactly the docs/VISUAL_SCRIPTING.md
//      trap already documented for .ocworld. Every write here goes through save() below, which reads
//      writeOcgraph(graph_, originalText_) and writes THAT string, so any unrecognised line the
//      parser skipped (a comment, a future record type) rides through untouched, the same guarantee
//      testUnknownRecords exercises (tests/formats/src/OcGraphTest.cpp:183).
#include "EditorTransform.hpp"
#include "EditorKeybinds.hpp"
#include "EditorPrefs.hpp"
#include "GraphEditor.hpp"
#include "GraphNodeDefs.hpp"
#if AVER_MODULE_FLUIDS
#  include "aver/fluids/FluidVolume.hpp"   // the seed shell a Fluid component previews
#endif
#if AVER_WITH_IMGUI
// The Viewport tab only. Behind the guard because tests/editor compiles THIS FILE with no ImGui,
// no preview module and a three-library link -- see tests/editor/CMakeLists.txt, which says so.
#  include "ActorEditor.hpp"   // sharedPreview / sharedPreviewMeshes / actorEditorContentRoot
#endif

#include "aver/core/Log.hpp"
#include "aver/platform/FileSystem.hpp"   // writeFileTextAtomic -- see save() below
#if AVER_WITH_IMGUI
#  include "aver/runtime/Engine.hpp"
#  include "aver/render/preview/ActorPreview.hpp"
#  include "aver/render/preview/PreviewMeshCache.hpp"
#  if AVER_MODULE_PBR
// The material-graph registry, for the Viewport tab of a DOMAIN material graph: the preview sphere
// is shaded by the same compiled graph the renderer uses, so the two cannot drift. Nested inside the
// ImGui guard because tests/editor compiles this file with neither.
#    include "aver/pbr/MaterialGraphRegistry.hpp"
#  endif
#endif

#include <algorithm>
#include <cctype>
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
#endif

namespace aver::editor {

namespace {
float g_graphEditorDpi = 1.0f;

f32 vecLen(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// The three VAR types the C# PinType enum accepts (Enum.TryParse<PinType>), case-insensitive. Exec is
// excluded on purpose: it's data the graph COMPUTES WITH, not data it REMEMBERS between ticks
// (OcGraphParser.cs rejects "VAR ... declared exec"). Graph.Validate() is the real compile-time gate
// for this; this check only keeps addVariable/retypeVariable's fallback and the Variables panel's
// type Combo honest about what the editor itself will produce.
bool isValidVarType(const std::string& type) {
    std::string t = type;
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t == "float" || t == "int" || t == "bool";
}

// Same no-quoting guard setAttribute uses (OcGraph.cpp's splitWhitespace / OcGraphParser.cs's
// SplitWhitespace both tokenise on bare whitespace) -- shared so VAR fields get identical protection
// to NODE attributes, rather than a second copy that can drift.
bool containsWhitespace(const std::string& s) {
    return s.find_first_of(" \t\r\n\v\f") != std::string::npos;
}

// The node header label. For all types but GetVar/SetVar this is just the catalog displayName --
// those two used to draw "Get Var" regardless of which variable, unreadable in a wall of them; this
// shows the variable's own name instead (as Unreal does).
//
// Headless and above the ImGui guard on purpose: the layout pass sizes a node from this title with no
// ImGui in the translation unit, so a box sized for "Get Var" would clip "PlayerSpeed".
std::string nodeTitle(const fmt::OcGraphNode& n, const GraphNodeDesc* desc,
                      const std::vector<fmt::OcGraphVariable>& vars) {
    const std::string fallback = desc ? desc->displayName : n.type;
    if (n.type != "GetVar" && n.type != "SetVar") return fallback;

    const GraphNodeAttribute a = getNodeAttribute(n, "var");
    // Bound to nothing yet: a freshly dropped node carries no var= token, and "Get Var" is exactly
    // what it is until someone picks one.
    if (!a.found || a.value.empty()) return fallback;

    // SET keeps its prefix. Unreal titles its setter "SET", and a read and a write of the same
    // variable drawn identically would be a genuinely dangerous thing to misread.
    const std::string shown = (n.type == "SetVar") ? ("SET " + a.value) : a.value;
    for (const fmt::OcGraphVariable& v : vars)
        if (v.name == a.value) return shown;
    // Names a variable this graph does not declare -- say so here rather than only in the panel.
    return shown + "  [?]";
}

// Everything below is ImGui-typed (ImVec2/ImU32) and used only from draw(), which is itself entirely
// behind `#if AVER_WITH_IMGUI`. Guarding these too is what lets GraphEditorLoadSaveTest compile this
// translation unit with no ImGui headers and no ImGui context -- see that test's own file comment.
#if AVER_WITH_IMGUI
inline ImVec2 toIm(Vec2 v) { return ImVec2(v.x, v.y); }
inline Vec2 fromIm(ImVec2 v) { return Vec2(v.x, v.y); }

// Pin colour is by declared type (Unreal's palette, kept -- already learned: green=float, white=exec);
// header colour is by node CATEGORY, in Aver's own orange/steel (matching editor chrome), because the
// node body is not a borrowed surface. Both fall back to a neutral default for anything unrecognised.
//
// THE RULE IS WARM VERSUS COOL, not one hue per category: warm (orange) means the node reaches
// outside the graph (an event, or the world changing); cool (steel) is pure computation; grey is flow
// control (order only, touches nothing). A function call gets Aver orange rather than Blueprint's
// blue -- from the caller's side a user function and an engine call are the same thing. Events used
// to share that orange; split to red because an entry point and a call are the two things a reader
// most needs to tell apart at a glance (and it matches Blueprint's convention).
ImU32 headerColorForCategory(const std::string& category) {
    if (category == "Function") return IM_COL32(242, 101, 34, 255);
    std::string c = category;
    for (char& ch : c) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    if (c == "event")    return IM_COL32(155,  36,  36, 255);  // deep red, as Blueprint marks an event

    if (c == "scene" || c == "actor" || c == "material" || c == "mesh" || c == "name" ||
        c == "character" || c == "game" || c == "physics" || c == "transform" || c == "debug")
                         return IM_COL32(242, 101,  34, 255);  // Aver orange -- the "f" nodes
    if (c == "input")    return IM_COL32(190,  90,  45, 255);  // device in, between call and compute

    if (c == "flow")     return IM_COL32( 74,  82,  96, 255);  // slate

    // Vector and Convert sit here with Var rather than with the calls above because they are
    // arithmetic -- the same place Blueprint puts a pure function, just not the same hue.
    if (c == "var")      return IM_COL32( 78, 104, 168, 255);  // steel, darkened -- storage
    return IM_COL32( 91, 141, 239, 255);                        // Aver steel -- math, vector, convert, logic, const
}

// The one highlight colour for selected node/link/comment borders and a link's end-pin ring --
// previously duplicated at each site. Amber, not Unreal's white, so it can't be confused with an exec
// wire; dimmer than the (255,220,90) it replaces, for the same glare reason the pin palette came down.
constexpr ImU32 kSelectionCol = IM_COL32(226, 188, 96, 255);

ImU32 colorForType(const std::string& type) {
    std::string t = type;
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // Unreal's pin colours, kept deliberately (see headerColorForCategory above). Toned down from
    // Unreal's near-fullbright originals (float was (91,255,15), string (255,0,168)) -- fine on
    // Blueprint's own background but glare at real graph size; these sit at ~75-80% value and lower
    // saturation while keeping each type's HUE, so the learned association still reads at a glance.
    if (t == "float")  return IM_COL32(126, 199,  76, 255);  // yellow-green
    if (t == "int")    return IM_COL32( 72, 181, 152, 255);  // turquoise
    if (t == "bool")   return IM_COL32(176,  72,  72, 255);  // red
    if (t == "string") return IM_COL32(186,  92, 152, 255);  // magenta
    if (t == "exec")   return IM_COL32(206, 210, 216, 255);  // white -- see above
    return IM_COL32(150, 154, 160, 255);
}

// A node's header takes its category's colour, except GetVar/SetVar take their VARIABLE'S TYPE
// colour instead -- the pins' own language. See nodeTitle() above for why these two are split out.
//
// The type comes from the VARIABLE, not the node's own pin: retypeVariable() does not rewrite pins
// already placed, so a value pin can outlive a retype and still say "float" when the variable is now
// an int. Reading the declaration keeps the header honest regardless.
ImU32 nodeHeaderColor(const fmt::OcGraphNode& n, const GraphNodeDesc* desc,
                      const std::vector<fmt::OcGraphVariable>& vars) {
    const ImU32 categoryCol = headerColorForCategory(desc ? desc->category : std::string());
    if (n.type != "GetVar" && n.type != "SetVar") return categoryCol;
    const GraphNodeAttribute a = getNodeAttribute(n, "var");
    if (!a.found || a.value.empty()) return categoryCol;   // unbound: still a generic Var node
    for (const fmt::OcGraphVariable& v : vars)
        if (v.name == a.value) return colorForType(v.type);
    return categoryCol;                                     // undeclared: do not fake a type colour
}

// Whether a pin's declared type is "exec" -- the ONE place this comparison lives, so drawing code
// shares a single definition of "is this pin control flow" rather than re-deriving it. Case-
// insensitive for the same reason colorForType is: the format doesn't fix a case for pin type strings.
bool isExecPinType(const std::string& type) {
    std::string t = type;
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t == "exec";
}
#endif

} // namespace

void setGraphEditorDpi(float dpi) { g_graphEditorDpi = dpi; }

GraphEditor::GraphEditor(std::string path) : path_(std::move(path)) {
    loadFromDisk();
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
    // A function node (FuncEntry/FuncReturn/CallFunc) has no catalog pins for the loop above --
    // its shape comes from a FUNC declaration, not a node type. Without this, a call node loaded
    // from disk draws no pins and every wire into it silently disappears.
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

// Auto-layout for a graph with no meaningful stored positions: triggers only when EVERY node sits
// at exactly (0,0) -- the shape a hand-written file has when nobody set NODE's x/y, since the parser
// requires x on every NODE line (OcGraph.cpp:78-81) but a human just types "0 0" or omits y. A file
// with one deliberate node at the origin and others placed elsewhere is left alone. Writes ONLY to
// the display overlay (see GraphEditor.hpp's displayPos_ comment).
void GraphEditor::runAutoLayoutIfUnpositioned() {
    if (graph_.nodes.empty()) return;
    bool allOrigin = true;
    for (const auto& n : graph_.nodes) {
        if (n.x != 0.0 || n.y != 0.0) { allOrigin = false; break; }
    }
    if (!allOrigin) return;
    // THE SAME SCALE recomputeLayouts() draws at, not 1.0. Node boxes are sized at DPI; spacing them
    // as if DPI were 1 packs columns three times too tightly on a 300% display, so nodes land on top
    // of each other and titles clip. Layout and drawing must agree on how big a node is.
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
    // Strip the pins synthesizeMissingPins() invented, so a save writes what the author wrote plus
    // their edits -- not forty PIN records the editor made up to have something to draw wires to.
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

    // Write to a temporary and swap, via writeFileTextAtomic (aver/platform/FileSystem.hpp) -- the
    // same pattern aver::fmt::saveOcSave and aver_settings_flush already use, lifted to the shared
    // platform layer. This used to open `path_` directly with ios::trunc, the same bug saveOcgraph
    // (OcGraph.cpp) has: that zeroes the file the instant it opens, before `text` lands a single
    // byte, so a crash or full disk between open and write destroyed the graph rather than merely
    // failing to update it.
    if (!writeFileTextAtomic(path_, text)) {
        if (why) *why = "could not write " + path_;
        return false;
    }
    originalText_ = text; // the next save merges against what is now actually on disk
    dirty_ = false;
    return true;
}

// EXACTLY WHAT save() WOULD WRITE, factored out so validateNow() cannot check a different document
// from the one that lands on disk. The synthesized-pin strip matters here as much as it does there:
// validating with forty invented PIN records would ask the managed validator about pins the author
// never wrote, and its answers would name them.
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
    // A HEURISTIC OVER PROSE, and deliberately a timid one. The validator's messages are sentences
    // written by hand -- "Node 'muzzle' is a Param node but has no param= attribute..." -- so there
    // is no structured field to read; the node id is simply the first single-quoted token. What
    // makes guessing safe is the second half: the token is returned ONLY if it is a real node id in
    // this graph. A message quoting a pin name, an event name, a parameter or a function yields
    // nothing at all, so a wrong guess cannot badge an innocent node -- it just badges none.
    // The quote character named through a constant rather than a character literal, because writing
    // it inline is what this file's history keeps getting mangled on -- see the escape-eating note.
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
    // A reload is a new set of positions, so the view is pointed at them again -- an externally
    // edited graph that moved every node would otherwise reopen looking at empty grid. AFTER the
    // dirty guard, deliberately: a notification that reloads nothing must move nothing either.
    pendingFrame_ = true;
    viewTouched_ = false;
}

void GraphEditor::pushUndo() {
    // A STANDING VALIDATION RESULT DESCRIBES THE GRAPH IT WAS RUN ON, and every caller of pushUndo is
    // about to change that graph. Left up, the message would go on naming a node the author has just
    // fixed (or deleted), which is worse than showing nothing: it reads as "still broken".
    validateErr_.clear();
    validateNode_.clear();

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
    dirty_ = true;
    selectedNodes_.clear();
    selectedLink_ = -1;
}

void GraphEditor::redo() {
    UndoState s{graph_, displayPos_};
    if (!history_.redo(s)) return;
    graph_ = std::move(s.graph);
    displayPos_ = std::move(s.displayPos);
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

// THE NODE'S name= AND THE ENTRY RECORD SAY THE SAME THING TWICE, and that's the format, not a
// choice made here: `ENTRY <nodeId> <eventName>` is the only thing that makes an event fire
// (CompileEntryPoint matches on it alone). Exactly one function writes the ENTRY side and every path
// that touches the name goes through it -- otherwise a rename would save, load and display fine while
// the event silently stopped firing.
void GraphEditor::syncEventEntry(const std::string& nodeId, const std::string& eventName) {
    for (usize i = 0; i < graph_.entryPoints.size(); ++i) {
        if (graph_.entryPoints[i].first != nodeId) continue;
        if (eventName.empty()) graph_.entryPoints.erase(graph_.entryPoints.begin() + static_cast<isize>(i));
        else graph_.entryPoints[i].second = eventName;
        return;
    }
    if (!eventName.empty()) graph_.entryPoints.emplace_back(nodeId, eventName);
}

// Which node vocabulary the open graph belongs to, read from the file's own DOMAIN record -- the
// same answer the COMPILER gives, since an editor offering a palette its compiler then refused would
// be worse than offering nothing.
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

    // THE SAME PREDICATE commitLink USES, not a second rule that happens to agree today. A palette
    // that offers a node whose link is then refused is worse than one that offers nothing: the
    // author has already committed to the gesture by the time they find out.
    const PinTypeCompat compat = openGraphDomain() == kDomainMaterial ? &materialPinTypeMatch
                                                                     : &exactPinTypeMatch;
    for (const auto& ps : desc.pins) {
        // Dragging FROM an output looks for an INPUT to land on, and vice versa. The direction is
        // half the filter, and getting it backwards would offer exactly the wrong half of the palette.
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
    // undo step. Reached when a caller spawns something the filter would not have offered.
    const PinTypeCompat compat = openGraphDomain() == kDomainMaterial ? &materialPinTypeMatch
                                                                     : &exactPinTypeMatch;
    std::string targetPin;
    for (const auto& ps : desc->pins) {
        if (ps.isOutput == linkDropFromIsOutput_) continue;
        const bool ok = linkDropFromIsOutput_ ? compat(linkDropFromType_, ps.type)
                                              : compat(ps.type, linkDropFromType_);
        // FIRST accepting pin in declaration order, which is the catalog's own reading order -- so
        // a wire dropped near an Add lands on `a`, not on whichever pin a set happened to iterate
        // first. Not the nearest pin geometrically: the node does not exist yet to measure against.
        if (ok) { targetPin = ps.name; break; }
    }
    if (targetPin.empty()) { linkDropPending_ = false; return {}; }

    const std::string newId = addNodeFromCatalog(typeId, canvasPos);
    if (newId.empty()) { linkDropPending_ = false; return {}; }

    // commitLink pushes its own undo, so this gesture costs two steps -- one for the node, one for
    // the wire. Deliberate: undoing once leaves the node you asked for, disconnected, which is a
    // more useful place to land than back at nothing.
    if (linkDropFromIsOutput_) commitLink(linkDropFromNode_, linkDropFromPin_, newId, targetPin);
    else                       commitLink(newId, targetPin, linkDropFromNode_, linkDropFromPin_);

    linkDropPending_ = false;
    return newId;
}

std::string GraphEditor::addNodeFromCatalog(const std::string& typeId, Vec2 canvasPos) {
    // DOMAIN-AWARE, because sixteen type names exist in both vocabularies with DIFFERENT PINS -- a
    // gameplay Add takes two scalars, a material Add takes two float3s. Resolving by name alone
    // handed a material graph the scalar shape and an author a node that fits nothing around it.
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
    // nothing else, saving and running while silently doing nothing.
    //
    // Event name defaults to the node TYPE (every ENTRY here reads `ENTRY tick OnTick`), driven off
    // the catalog's own "Event" category rather than a second hand-maintained list --
    // GraphNodeDefs.hpp's header explains why there is only one place a node type gets registered.
    if (desc->category == "Event") {
        // CustomEvent is the one event whose name is NOT its type. A fresh one gets a unique
        // generated name rather than empty, since an ENTRY with no name is unrepresentable and a
        // silently-non-firing node is exactly the On Tick failure this just got fixed for.
        std::string eventName = desc->typeId;
        if (desc->typeId == "CustomEvent") {
            eventName = makeUniqueEventName("MyEvent");
            setNodeAttribute(graph_.nodes.back(), "name", eventName);
        }
        syncEventEntry(node.id, eventName);
    }

    // A NODE DROPPED ONTO A FUNCTION'S CANVAS BELONGS TO THAT FUNCTION. Without this it would be
    // created in the event graph and vanish the instant it was drawn -- present in the file,
    // invisible on the canvas that made it.
    if (!currentSubgraph_.empty()) setNodeAttribute(graph_.nodes.back(), "func", currentSubgraph_);

    displayPos_[node.id] = canvasPos;
    selectedNodes_ = {node.id};
    selectedLink_ = -1;
    dirty_ = true;
    return node.id;
}

// Bar height and grip size, in CANVAS units. Canvas units are logical pixels at zoom 1, which is why
// both scale by dpi and NOT by view_.zoom -- the drawing code multiplies by zoom on its way to the
// screen, so folding zoom in here as well would make the bar twice as tall each time it is asked.
namespace {
constexpr float kCommentBarPx  = 24.0f;
constexpr float kCommentGripPx = 16.0f;
constexpr float kCommentMinPx  = 60.0f;
} // namespace

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
    // Measured from the LAYOUTS, not from displayPos_, because a node position is its top-left and a
    // box drawn to those would clip the right-hand pins off every node on its edge.
    bool any = false;
    Vec2 lo{}, hi{};
    for (const auto& nl : layouts_) {
        if (std::find(selectedNodes_.begin(), selectedNodes_.end(), nl.nodeId) == selectedNodes_.end()) continue;
        if (!any) { lo = nl.min; hi = nl.max; any = true; continue; }
        lo.x = std::min(lo.x, nl.min.x); lo.y = std::min(lo.y, nl.min.y);
        hi.x = std::max(hi.x, nl.max.x); hi.y = std::max(hi.y, nl.max.y);
    }
    if (!any) return {};
    // Margin on all sides, extra on top for the title bar (drawn inside the box, or it would hide the
    // first row of node headers). Scaled by DPI: computeNodeLayout sizes nodes at DPI while position
    // is unscaled, so an unscaled margin would run a third too narrow at 300% DPI. The box ends up
    // loose if later viewed at lower DPI -- accepted, since the alternative is a box too small to
    // enclose its own nodes on the machine that drew it.
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
    // A newline would end the record early, turning the tail of the title into a new record the
    // parser reads separately -- folded to a space rather than refused, since the text arrives from a
    // paste as often as a keystroke and losing a pasted title is worse than flattening it.
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
    // BACK TO FRONT. Boxes draw in file order, so the LAST one drawn is the one on top, and the one
    // on top is the one a click belongs to. Scanning forwards would hand every click on an
    // overlapping pair to the box underneath.
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

namespace {
// A node's owning subgraph, read straight from its func= attribute. Empty means the event graph --
// see GraphEditor::currentSubgraph() for why one convention is used everywhere rather than three.
std::string subgraphOf(const fmt::OcGraphNode& n) {
    const GraphNodeAttribute a = getNodeAttribute(n, "func");
    return a.found ? a.value : std::string();
}
bool isFunctionNodeType(const std::string& t) {
    return t == "FuncEntry" || t == "FuncReturn" || t == "CallFunc";
}
} // namespace

fmt::OcGraphFunction* GraphEditor::findFunction(const std::string& name) {
    for (auto& f : graph_.functions) if (f.name == name) return &f;
    return nullptr;
}

void GraphEditor::setCurrentSubgraph(const std::string& funcName) {
    if (currentSubgraph_ == funcName) return;
    currentSubgraph_ = funcName;
    // A selection in the subgraph just left is invisible but Delete would still act on it, so it's
    // cleared; the view re-frames onto the new subgraph's content, the same "the view is the editor's
    // until you touch it" rule the framing code follows.
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
namespace {
constexpr float kFramePaddingPx = 40.0f;

// The zoom floor is DPI-relative (screen px per logical px), not an absolute zoom value -- a flat
// floor bites too early on a high-DPI display, since what it's for ("don't shrink past a smear") is
// really about zoom * dpi. Measured: AN_FPCharacter.ocgraph (63 nodes) auto-lays out to 11907x5400
// canvas units at dpi 3, needing zoom 0.135 to fit an 1853px canvas -- the old flat 0.15 floor
// clamped that short, so Frame All did not frame all. Only the floor scales, not the ceiling: zoom 4
// at dpi 3 (12x) is unused headroom, and halving it would remove working range for no benefit.
constexpr float kMinScreenPxPerLogicalPx = 0.15f;
constexpr float kMaxZoom = 4.0f;
inline float minZoomForDpi(float dpi) { return kMinScreenPxPerLogicalPx / std::max(dpi, 0.25f); }
} // namespace

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
    // COMMENT BOXES COUNT. A box is content -- often the largest thing on the canvas, and a frame
    // that cropped one would look like the box had been resized rather than the view moved.
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
    // The same zoom limits the wheel enforces, passed in rather than re-declared, so a framed view is
    // always a view the wheel could also have reached -- a frame that landed outside the wheel range
    // would snap on the next scroll notch.
    view_ = frameTransform(lo, hi, viewportPx, kFramePaddingPx * dpi, minZoomForDpi(dpi), kMaxZoom);
}

void GraphEditor::frameSelection(Vec2 viewportPx, float dpi) {
    Vec2 lo, hi;
    if (!selectionBounds(dpi, &lo, &hi)) { frameAll(viewportPx, dpi); return; }
    // A SINGLE selected node would otherwise frame to a rectangle the size of one node and zoom to
    // the 4x ceiling, which is disorienting rather than helpful -- you lose every neighbour and the
    // wire you were following. Padding the box out to a minimum keeps some context in shot.
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
            // A node that carried NO position now carries one. That is the intended effect of
            // pressing the button -- see the header -- and it is also the one thing this does that
            // runAutoLayoutIfUnpositioned refuses to do behind the user's back.
            n.hasPosition = true;
            break;
        }
    }
    // The positions are the file's now, not an overlay, so the DPI-change re-layout in draw() must
    // stop rewriting them -- it would fight every position the author just asked to keep.
    autoLaidOut_ = false;
    dirty_ = true;
    return true;
}

void GraphEditor::copySelection() {
    clipNodes_.clear();
    clipLinks_.clear();
    if (selectedNodes_.empty()) return;

    const auto selected = [&](const std::string& id) {
        return std::find(selectedNodes_.begin(), selectedNodes_.end(), id) != selectedNodes_.end();
    };

    for (const fmt::OcGraphNode& n : graph_.nodes)
        if (selected(n.id)) clipNodes_.push_back(n);

    // BOTH ENDS, OR NOT AT ALL -- see the header. A half-copied link would either dangle or, worse,
    // point the copy back at the original.
    for (const fmt::OcGraphLink& l : graph_.links)
        if (selected(l.sourceNode) && selected(l.destNode)) clipLinks_.push_back(l);
}

void GraphEditor::pasteClipboard(Vec2 canvasPos) {
    if (clipNodes_.empty()) return;
    pushUndo();

    // Pasted where asked, keeping the copied nodes' RELATIVE layout: the top-left of the copied set
    // lands on canvasPos and everything keeps its offset from it. Pasting a shape and getting a pile
    // would make the feature useless for the case it exists for.
    Vec2 origin{static_cast<f32>(clipNodes_[0].x), static_cast<f32>(clipNodes_[0].y)};
    for (const fmt::OcGraphNode& n : clipNodes_) {
        origin.x = n.x < origin.x ? static_cast<f32>(n.x) : origin.x;
        origin.y = n.y < origin.y ? static_cast<f32>(n.y) : origin.y;
    }

    std::unordered_map<std::string, std::string> remap;
    std::vector<std::string> pasted;

    for (const fmt::OcGraphNode& src : clipNodes_) {
        fmt::OcGraphNode n = src;
        n.id = makeUniqueNodeId(src.type);
        remap[src.id] = n.id;
        n.x = canvasPos.x + (src.x - origin.x);
        n.y = canvasPos.y + (src.y - origin.y);
        graph_.nodes.push_back(n);

        // The same two things addNodeFromCatalog does after pushing, for the same reasons: an Event
        // node with no ENTRY never runs, and a node pasted onto a function's canvas belongs to that
        // function rather than to the event graph it would otherwise vanish into.
        const GraphNodeDesc* desc = findGraphNodeDescIn(n.type, openGraphDomain());
        if (desc && desc->category == "Event") {
            std::string eventName = desc->typeId;
            if (desc->typeId == "CustomEvent") {
                eventName = makeUniqueEventName("MyEvent");
                setNodeAttribute(graph_.nodes.back(), "name", eventName);
            }
            syncEventEntry(n.id, eventName);
        }
        if (!currentSubgraph_.empty()) setNodeAttribute(graph_.nodes.back(), "func", currentSubgraph_);
        else                           removeNodeAttribute(graph_.nodes.back(), "func");

        displayPos_[n.id] = Vec2{static_cast<f32>(n.x), static_cast<f32>(n.y)};
        pasted.push_back(n.id);
    }

    for (const fmt::OcGraphLink& src : clipLinks_) {
        const auto a = remap.find(src.sourceNode);
        const auto b = remap.find(src.destNode);
        if (a == remap.end() || b == remap.end()) continue;   // unreachable: copy filtered these out
        fmt::OcGraphLink l = src;
        l.sourceNode = a->second;
        l.destNode   = b->second;
        graph_.links.push_back(l);
    }

    // The paste becomes the selection, so it can be dragged into place immediately -- and so a second
    // Ctrl+V does not silently stack a third copy on a selection the user thinks is the second.
    selectedNodes_ = pasted;
    selectedLink_ = -1;
    dirty_ = true;
}

void GraphEditor::duplicateSelection() {
    // Duplicate is copy+paste that does NOT disturb the clipboard: a user who copied one thing, then
    // duplicated another, still has the first on the clipboard. The offset is a nudge rather than a
    // position, so the copy is visibly on top of but not exactly over its original.
    if (selectedNodes_.empty()) return;
    std::vector<fmt::OcGraphNode> keepNodes = clipNodes_;
    std::vector<fmt::OcGraphLink> keepLinks = clipLinks_;

    copySelection();
    Vec2 at{0.0f, 0.0f};
    if (!clipNodes_.empty()) {
        at = Vec2{static_cast<f32>(clipNodes_[0].x), static_cast<f32>(clipNodes_[0].y)};
        for (const fmt::OcGraphNode& n : clipNodes_) {
            at.x = n.x < at.x ? static_cast<f32>(n.x) : at.x;
            at.y = n.y < at.y ? static_cast<f32>(n.y) : at.y;
        }
        at.x += 40.0f;
        at.y += 40.0f;
    }
    pasteClipboard(at);

    clipNodes_ = std::move(keepNodes);
    clipLinks_ = std::move(keepLinks);
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
        // A CONSTANT IS WRITTEN TWICE INTO THE FILE, so both copies have to agree.
        //
        // A Const node carries its literal in two places: the NODE line's `value=` (which this row
        // edits) and the output PIN's default, written at spawn. OcGraphParser.cs turns BOTH into a
        // ConstantOutput for the same node id, and the compiler takes FirstOrDefault -- so today the
        // attribute happens to win, purely because a NODE line precedes its own PIN lines in the
        // file. That is a correct outcome resting on nothing but emission order, and a writer that
        // ever grouped PIN records differently would silently flip every constant in every graph
        // back to its spawn default.
        //
        // Writing both makes the file self-consistent, so the order stops mattering and a human
        // reading the text is not shown two different numbers for one value.
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
        // The ENTRY record goes with it, for setAttribute's reason in reverse: an ENTRY with no
        // event name is a record this format cannot express, so clearing the name has to mean the
        // node stops being an entry point rather than becoming a malformed one.
        if (key == "name" && n.type == "CustomEvent") syncEventEntry(n.id, "");
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
    // The "Connection refused: " prefix used to live in draw()'s overlay code, baked onto every
    // lastRejectMsg_ regardless of source. Once deleteVariable (below) reused the same banner for an
    // unrelated refusal, that prefix would have mislabelled it -- moved here, to the one call site
    // that actually means it, so the banner draw can show lastRejectMsg_ verbatim.
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
    // Deliberately does not touch any node's pins: a palette-spawned GetVar/SetVar has its own
    // 'value' pin typed to the catalog default at spawn, so a later retype can leave it disagreeing.
    // Rewriting it silently was rejected -- the user may already have wired a link against that
    // visible type, and changing it invisibly could break the link with no visible cause. Left as a
    // live mismatch note in the Variables panel instead (computed fresh every frame).
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

void GraphEditor::recomputeLayouts(float dpi) {
    layouts_.clear();
    layouts_.reserve(graph_.nodes.size());
    for (const auto& n : graph_.nodes) {
        // ONLY THE SUBGRAPH ON SCREEN. layouts_ is what the canvas draws, what hit-testing tests,
        // what box-select selects and what framing measures -- so filtering here scopes all four
        // at once, and there is no second place that could disagree about which nodes are visible.
        if (subgraphOf(n) != currentSubgraph_) continue;
        fmt::OcGraphNode display = n; // substitute the DISPLAY position (a live drag only touches
                                       // displayPos_) so drawing follows a drag without touching
                                       // graph_ until it commits.
        auto it = displayPos_.find(n.id);
        if (it != displayPos_.end()) { display.x = it->second.x; display.y = it->second.y; }
        const GraphNodeDesc* desc = findGraphNodeDesc(n.type);
        // The SAME string the header will draw -- a variable node is as wide as its variable's name,
        // and computing the width from "Get Var" while drawing "PlayerSpeed" would clip it.
        const std::string title = nodeTitle(n, desc, graph_.variables);
        // `dpi` only, NOT dpi*zoom: canvas-space geometry is computed once at a fixed logical scale,
        // and CanvasTransform::zoom applies uniformly afterward when converting to/from screen space
        // (canvasToScreen/screenToCanvas) -- folding zoom in here too would double-apply it.
        layouts_.push_back(computeNodeLayout(display, title, style_, dpi));
    }
}

// The details ("Gap B") column's default width and its persisted preference key -- a
// DPI-INDEPENDENT PIXEL WIDTH, ActorEditor's own convention (ActorEditor.cpp), not the FRACTION
// most other editors' splits use: see EditorWidgets.hpp's own top comment for why. 260.0f is this
// tab's own PRE-EXISTING default -- it used to be `std::clamp(260.0f * dpi, 180.0f * dpi,
// std::max(avail.x * 0.45f, 120.0f * dpi))`, recomputed fresh every frame with no persistence at
// all -- kept exactly for the ordinary (wide-enough) case; see drawEventGraph() below for what
// changed in the narrow-window squeeze that formula's middle branch also covered.
constexpr float kDefaultDetailsColumn = 260.0f;
constexpr const char* kPrefDetailsColumn = "graphEditor.detailsColumn";

void GraphEditor::draw(Engine& e) {
#if AVER_WITH_IMGUI
    const float dpi = g_graphEditorDpi;
    // An auto-laid-out graph is spaced for one DPI. If that changed since (opened before applyDpi
    // ran, or moved to a differently-scaled monitor), redo it or the boxes collide. Only auto-layout
    // is redone -- positions the user dragged, or from the file, are theirs and stay untouched.
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
        // CHECKED ON SAVE, BUT THE SAVE STILL HAPPENS. A half-built graph is the normal state of one
        // being built, and an editor that refuses to write it would be unusable. The result is
        // reported, not enforced.
        else if (validate_) { validateNow(validateErr_, validateNode_); }
    }
    ImGui::SameLine();
    // VALIDATE, as its own button as well as on save, because the question "is this finished" is one
    // an author asks mid-build, not only when writing to disk.
    ImGui::BeginDisabled(!validate_);
    if (ImGui::Button("Validate")) {
        if (validateNow(validateErr_, validateNode_)) {
            validateErr_.clear();
            validateNode_.clear();
            showRejectionBanner("graph is valid");
        } else {
            showRejectionBanner(validateErr_);
        }
    }
    ImGui::EndDisabled();
    if (!validate_ && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("The .NET bridge exports no GraphValidate, so nothing can check this graph.");
    ImGui::SameLine();
    // The standing result, so an author is not made to re-press Validate to remember what was wrong.
    // Cleared by the next Validate, and by any edit that could have fixed it (see pushUndo).
    if (!validateErr_.empty()) {
        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "%s%s",
                           validateNode_.empty() ? "" : (validateNode_ + ": ").c_str(),
                           validateErr_.c_str());
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
    // FRAME ALL AND AUTO-LAYOUT ARE BUTTONS, not just key bindings: someone who doesn't know this
    // editor yet can find a graph that opens on an empty grid, but not a binding they'd need telling
    // about -- the shortcut is in the label.
    // A combo, not a second tab bar, for which subgraph the canvas shows: the number of functions is
    // unbounded and the tab bar above already means something else (what the actor DOES vs IS).
    // Absent until a graph declares its first function, so nothing changes for graphs that have none.
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
    // The pan hint lives on the status line, not a tooltip -- a tooltip only reaches someone already
    // hovering the thing it explains, backwards for a gesture whose problem is nobody knew to reach
    // for it. Not aspirational: every gesture listed is wired in the block below.
    ImGui::TextDisabled("%s  |  %zu nodes, %zu links, %zu comments  |  zoom %.0f%%  |  pan: right- or middle-drag, or Space+drag  |  C: comment box",
                         path_.c_str(), graph_.nodes.size(), graph_.links.size(),
                         graph_.comments.size(), view_.zoom * 100.0f);

    // The two tabs an actor gets: what it DOES (event graph), and what it IS (component tree).
    // Blueprint's own split, not merely organisational -- a class graph carries both an exec graph
    // and a COMPONENT TREE (fmt::OcGraphComponent) answering different questions, with no shared
    // canvas, selection, or unit between them. A non-class graph still gets both tabs: hiding
    // Viewport until a CLASS record exists would mean discovering a graph CAN be an actor requires
    // already knowing it.
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

// Restores the canvas/details ("Gap B") split to its default width and persists that immediately --
// see AssetEditor.hpp's own resetLayout() comment for why "Reset Tab Layout" needs every tab to
// implement this rather than just ActorEditor. A no-op `#if AVER_WITH_IMGUI` is off: a headless
// build never lays the panels out at all, so there is nothing for a reset to restore.
void GraphEditor::resetLayout() {
#if AVER_WITH_IMGUI
    detailsColW_ = 0.0f;
    setPrefFloat(kPrefDetailsColumn, kDefaultDetailsColumn);
    flushEditorPrefs();
#endif
}

// The Event Graph tab: the node canvas and its details column. Lifted out of draw() unchanged when
// the Viewport tab arrived -- draw() is now the tab bar and nothing else, which is the only way
// either tab's body stays readable.
void GraphEditor::drawEventGraph(float dpi) {
    // WHICH NODES ARE RUNNING, refreshed once a frame. Cheap by construction: the managed side keys
    // its table by graph NAME, so this asks only about the graph on this canvas, and the recording
    // that fills it costs a static bool test when nothing is recording.
    //
    // (An earlier draft said "armed only while a tab is open". It is not: SandboxApp arms it ONCE
    // when the editor starts, deliberately, because the managed table is keyed by graph NAME and
    // costs a static bool test when off -- which is cheaper than tracking tab lifetimes. A packaged
    // game, having no editor, never arms it at all.)
    //
    // CLEARED WHEN THERE IS NO SOURCE, so a set captured during Play cannot go on glowing after Play
    // stops -- a highlight that outlives the execution it describes is a lie with a half-life.
    if (nodeHits_) {
        std::vector<std::pair<std::string, f32>> hits;
        nodeHits_(graph_.name, kNodeHitFadeSec, hits);
        nodeHitAges_.clear();
        for (auto& h : hits) nodeHitAges_.emplace(std::move(h.first), h.second);
    } else if (!nodeHitAges_.empty()) {
        nodeHitAges_.clear();
    }

#if AVER_WITH_IMGUI
    recomputeLayouts(dpi);

    // The canvas gives up a strip on the right for the details panel (Gap B), draggable and
    // persisted through the shared splitterHandle/clampSplitWidth primitives (EditorWidgets.hpp) --
    // see kDefaultDetailsColumn's own comment above for the DPI-independent-pixel-width convention
    // this keeps from ActorEditor. Every downstream canvas calculation (originIm, canvasSize,
    // mouse-to-canvas conversion) derives from GetContentRegionAvail() called AFTER ##graphCanvas's
    // BeginChild below, so it automatically sees the narrowed region -- nothing past this point
    // needed to change for that to hold.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float minDetails = 180.0f * dpi, minCanvas = 40.0f * dpi;
    if (detailsColW_ <= 0.0f) detailsColW_ = prefFloat(kPrefDetailsColumn, kDefaultDetailsColumn) * dpi;
    detailsColW_ = clampSplitWidth(detailsColW_, avail.x, minDetails, minCanvas);
    const float detailsW = detailsColW_;
    const float canvasW = std::max(avail.x - detailsW - ImGui::GetStyle().ItemSpacing.x, minCanvas);
    ImGui::BeginChild("##graphCanvas", ImVec2(canvasW, std::max(avail.y, 80.0f * dpi)), true,
                       ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const ImVec2 originIm = ImGui::GetCursorScreenPos();
    const Vec2 origin = fromIm(originIm);
    const ImVec2 canvasSize = ImGui::GetContentRegionAvail();
    ImGui::InvisibleButton("##graphCanvasHit", ImVec2(std::max(canvasSize.x, 1.0f), std::max(canvasSize.y, 1.0f)),
                            ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                            ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool canvasFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows) || hovered;

    ImGuiIO& io = ImGui::GetIO();
    const Vec2 mouseScreen = fromIm(io.MousePos) - origin; // canvas-local screen space
    const Vec2 mouseCanvas = screenToCanvas(view_, mouseScreen);

    auto toScreenAbs = [&](Vec2 canvasPt) -> ImVec2 {
        const Vec2 s = canvasToScreen(view_, canvasPt);
        return ImVec2(originIm.x + s.x, originIm.y + s.y);
    };

    // Reframes whenever the graph has unframed content (pendingFrame_) OR the canvas resizes, but
    // ONLY until the author pans or zooms (viewTouched_) -- "the view is mine until you touch it",
    // not "frame once and hope". A plain one-shot-on-open was tried first and measured a canvas
    // 1853px wide when the window settled at 2670px, framing a third of a screen off; "wait for two
    // consecutive frames to agree" was tried next and also measured 1853 twice, since the window
    // keeps growing past that. Tracking "has the author chosen a view yet" instead sidesteps needing
    // to guess when layout is final, and fixes window-resize and the details-panel split for free.
    const bool canvasResized = canvasSize.x != lastCanvasSizePx_.x || canvasSize.y != lastCanvasSizePx_.y;
    if (framePendingFromToolbar_ || (!viewTouched_ && (pendingFrame_ || canvasResized))) {
        frameAll(Vec2{canvasSize.x, canvasSize.y}, dpi);
        pendingFrame_ = false;
        framePendingFromToolbar_ = false;
    }
    lastCanvasSizePx_ = Vec2{canvasSize.x, canvasSize.y};

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(originIm, ImVec2(originIm.x + canvasSize.x, originIm.y + canvasSize.y), IM_COL32(30, 30, 34, 255));
    {
        const f32 gridStep = 64.0f * dpi * view_.zoom;
        if (gridStep > 6.0f) { // don't draw a fog of lines when zoomed far out
            const f32 startX = std::fmod(view_.panPx.x, gridStep);
            const f32 startY = std::fmod(view_.panPx.y, gridStep);
            for (f32 x = startX; x < canvasSize.x; x += gridStep)
                dl->AddLine(ImVec2(originIm.x + x, originIm.y), ImVec2(originIm.x + x, originIm.y + canvasSize.y), IM_COL32(255, 255, 255, 12));
            for (f32 y = startY; y < canvasSize.y; y += gridStep)
                dl->AddLine(ImVec2(originIm.x, originIm.y + y), ImVec2(originIm.x + canvasSize.x, originIm.y + y), IM_COL32(255, 255, 255, 12));
        }
    }

    if (hovered && dragMode_ == DragMode::None) {
        const bool spacePan = io.KeyShift == false && ImGui::IsKeyDown(ImGuiKey_Space);
        if ((spacePan && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            dragMode_ = DragMode::PanCanvas;
            rightButtonPan_ = false;
            dragStartScreen_ = mouseScreen;
            panAnchorPx_ = view_.panPx;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            // Right button is ambiguous at mouse-DOWN: a plain click must still open Add Node
            // (existing behaviour), but a right-drag is the direct pan gesture this task asked for.
            // Start panning right away (same zero-latency feel as Middle-drag) and remember the press
            // position (to measure travel) and press CANVAS point (pendingSpawnCanvasPos_, what the
            // popup would open at) so release can retroactively decide which gesture this was. See
            // the PanCanvas case below for the other half.
            dragMode_ = DragMode::PanCanvas;
            rightButtonPan_ = true;
            dragStartScreen_ = mouseScreen;
            panAnchorPx_ = view_.panPx;
            pendingSpawnCanvasPos_ = mouseCanvas;

            // WHICH POPUP THE RELEASE WILL OPEN, decided here at PRESS because the hit test has to
            // run against the canvas position under the cursor when the button went down -- by
            // release the view may have panned. Right-clicking a node used to open Add Node, the
            // same as right-clicking empty space: there was no context menu anywhere in this editor,
            // so copy, cut, paste, duplicate, delete and break-links were keyboard-only and
            // undiscoverable. Right-clicking the thing you want to act on is the first gesture
            // anyone tries.
            const GraphHitResult rhit = hitTest(graph_, layouts_, mouseCanvas, style_, dpi);
            rightClickNode_ = rhit.kind == GraphHitKind::Node ? rhit.nodeId : std::string{};
            rightClickLink_ = rhit.kind == GraphHitKind::Link ? static_cast<int>(rhit.linkIndex) : -1;
            // SELECT WHAT WAS RIGHT-CLICKED, unless it is already part of the selection -- so
            // right-clicking one of five selected nodes acts on all five (what every editor does),
            // while right-clicking outside the selection retargets to just that node rather than
            // silently acting on something off-screen.
            if (!rightClickNode_.empty()) {
                const bool already = std::find(selectedNodes_.begin(), selectedNodes_.end(),
                                               rightClickNode_) != selectedNodes_.end();
                if (!already) { selectedNodes_ = {rightClickNode_}; selectedLink_ = -1; }
                selectedComment_.clear();
            } else if (rightClickLink_ >= 0) {
                selectedNodes_.clear();
                selectedLink_ = rightClickLink_;
                selectedComment_.clear();
            }
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const GraphHitResult hit = hitTest(graph_, layouts_, mouseCanvas, style_, dpi);
            const bool ctrl = io.KeyCtrl;
            if (hit.kind != GraphHitKind::None) selectedComment_.clear();
            if (hit.kind == GraphHitKind::Pin) {
                dragMode_ = DragMode::DrawLink;
                linkDragFromNode_ = hit.nodeId;
                linkDragFromPin_ = hit.pinName;
                linkDragFromIsOutput_ = hit.pinIsOutput;
            } else if (hit.kind == GraphHitKind::Node) {
                const bool already = std::find(selectedNodes_.begin(), selectedNodes_.end(), hit.nodeId) != selectedNodes_.end();
                if (ctrl) {
                    if (already) selectedNodes_.erase(std::remove(selectedNodes_.begin(), selectedNodes_.end(), hit.nodeId), selectedNodes_.end());
                    else selectedNodes_.push_back(hit.nodeId);
                } else if (!already) {
                    selectedNodes_ = {hit.nodeId};
                }
                selectedLink_ = -1;
                dragMode_ = DragMode::MoveNodes;
                dragStartCanvas_ = mouseCanvas;
                moveStart_.clear();
                for (const auto& id : selectedNodes_) {
                    auto it = displayPos_.find(id);
                    moveStart_[id] = it != displayPos_.end() ? it->second : Vec2{};
                }
                pendingMoveSnapshot_ = UndoState{graph_, displayPos_};
                moveUndoPushed_ = false;
            } else if (hit.kind == GraphHitKind::Link) {
                if (!ctrl) selectedNodes_.clear();
                selectedLink_ = static_cast<int>(hit.linkIndex);
            } else {
                // COMMENT BOXES ARE TESTED LAST: a box draws behind everything, so anything drawn on
                // top of it owns the click -- including a node over its title bar. Testing the box
                // first would make those nodes unclickable for no visible reason.
                bool onGrip = false;
                const std::string cid = commentAtCanvas(mouseCanvas, dpi, &onGrip);
                if (!cid.empty()) {
                    selectedNodes_.clear();
                    selectedLink_ = -1;
                    selectedComment_ = cid;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !onGrip) {
                        // Double-click the bar opens the properties popup (title, colour,
                        // delete). Queued rather than opened here because BeginPopup for it is
                        // submitted further down this same frame, after the drawing.
                        commentEditId_ = cid;
                        const fmt::OcGraphComment* c = nullptr;
                        for (const auto& k : graph_.comments) if (k.id == cid) { c = &k; break; }
                        std::snprintf(commentEditBuf_, sizeof commentEditBuf_, "%s", c ? c->text.c_str() : "");
                        commentPopupQueued_ = true;
                    } else {
                        const fmt::OcGraphComment* c = nullptr;
                        for (const auto& k : graph_.comments) if (k.id == cid) { c = &k; break; }
                        if (c) {
                            dragMode_ = onGrip ? DragMode::ResizeComment : DragMode::MoveComment;
                            activeComment_ = cid;
                            dragStartCanvas_ = mouseCanvas;
                            commentDragStartPos_ = Vec2{static_cast<f32>(c->x), static_cast<f32>(c->y)};
                            commentDragStartSize_ = Vec2{static_cast<f32>(c->w), static_cast<f32>(c->h)};
                            commentCapturedStart_.clear();
                            if (dragMode_ == DragMode::MoveComment) {
                                for (const auto& nid : nodesInsideComment(cid, dpi)) {
                                    auto it = displayPos_.find(nid);
                                    commentCapturedStart_[nid] = it != displayPos_.end() ? it->second : Vec2{};
                                }
                            }
                            pendingCommentSnapshot_ = UndoState{graph_, displayPos_};
                            commentUndoPushed_ = false;
                        }
                    }
                } else {
                    if (!ctrl) { selectedNodes_.clear(); selectedLink_ = -1; }
                    selectedComment_.clear();
                    dragMode_ = DragMode::BoxSelect;
                    dragStartCanvas_ = mouseCanvas;
                    boxSelectCurrentCanvas_ = mouseCanvas;
                }
            }
        }
    }

    switch (dragMode_) {
    case DragMode::PanCanvas: {
        const bool stillDown = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                                ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (stillDown) {
            const Vec2 moved = mouseScreen - dragStartScreen_;
            if (vecLen(moved) > 0.0f) viewTouched_ = true;
            view_.panPx = panAnchorPx_ + moved;
        } else {
            // Right button only: resolve click-vs-drag now that the button is up. A few px of "click
            // threshold" absorbs the jitter a real mouse always has between press and release --
            // without it every right click would measure nonzero travel and the popup would never
            // open. dpi-scaled (unlike MoveNodes's bare 3.0f below) because mouseScreen is raw device
            // pixels: at 200% DPI the same hand-jitter covers twice as many, and an unscaled threshold
            // would make the popup progressively harder to summon on high-DPI.
            constexpr f32 kRightClickDragThresholdPx = 4.0f;
            if (rightButtonPan_) {
                const f32 travelled = vecLen(mouseScreen - dragStartScreen_);
                if (travelled <= kRightClickDragThresholdPx * dpi) {
                    view_.panPx = panAnchorPx_; // a click must pan by exactly zero, not by a few stray px
                    // On a node or a link: the context menu. On empty canvas: Add Node, as before.
                    if (!rightClickNode_.empty() || rightClickLink_ >= 0)
                        ImGui::OpenPopup("##graphNodeMenu");
                    else
                        ImGui::OpenPopup("##graphAddNode"); // pendingSpawnCanvasPos_ was set at press time
                }
                // else: a real drag happened. The pan already applied above stays; no popup.
            }
            dragMode_ = DragMode::None;
        }
        break;
    }
    case DragMode::MoveNodes: {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const Vec2 delta = mouseCanvas - dragStartCanvas_;
            if (!moveUndoPushed_ && vecLen(delta) * view_.zoom > 3.0f) {
                history_.push(pendingMoveSnapshot_);
                moveUndoPushed_ = true;
            }
            for (const auto& id : selectedNodes_) displayPos_[id] = moveStart_[id] + delta;
        } else {
            if (moveUndoPushed_) {
                for (const auto& id : selectedNodes_) {
                    for (auto& n : graph_.nodes) {
                        if (n.id == id) {
                            const Vec2 p = displayPos_[id];
                            n.x = p.x; n.y = p.y;
                            break;
                        }
                    }
                }
                dirty_ = true;
            }
            dragMode_ = DragMode::None;
        }
        break;
    }
    case DragMode::BoxSelect: {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            boxSelectCurrentCanvas_ = mouseCanvas;
        } else {
            const Vec2 lo(std::min(dragStartCanvas_.x, boxSelectCurrentCanvas_.x), std::min(dragStartCanvas_.y, boxSelectCurrentCanvas_.y));
            const Vec2 hi(std::max(dragStartCanvas_.x, boxSelectCurrentCanvas_.x), std::max(dragStartCanvas_.y, boxSelectCurrentCanvas_.y));
            for (const auto& nl : layouts_) {
                const bool intersects = nl.min.x <= hi.x && nl.max.x >= lo.x && nl.min.y <= hi.y && nl.max.y >= lo.y;
                if (intersects && std::find(selectedNodes_.begin(), selectedNodes_.end(), nl.nodeId) == selectedNodes_.end())
                    selectedNodes_.push_back(nl.nodeId);
            }
            dragMode_ = DragMode::None;
        }
        break;
    }
    case DragMode::DrawLink: {
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const GraphHitResult hit = hitTest(graph_, layouts_, mouseCanvas, style_, dpi);
            if (hit.kind == GraphHitKind::Pin && !(hit.nodeId == linkDragFromNode_ && hit.pinName == linkDragFromPin_)) {
                if (linkDragFromIsOutput_) commitLink(linkDragFromNode_, linkDragFromPin_, hit.nodeId, hit.pinName);
                else                       commitLink(hit.nodeId, hit.pinName, linkDragFromNode_, linkDragFromPin_);
            } else if (hit.kind == GraphHitKind::None) {
                // DRAGGING A WIRE INTO EMPTY SPACE OPENS THE PALETTE, filtered to node types that
                // could actually accept this wire, and connects the one you pick. Releasing here
                // used to do nothing at all -- the wire simply vanished.
                //
                // This is the gesture a Blueprint author reaches for to create most nodes, and its
                // absence is a large part of why every graph in this repo was typed by hand rather
                // than drawn: without it, building a chain means opening the palette, finding the
                // node, placing it somewhere, then dragging a wire to it, for every single node.
                //
                // Only on EMPTY canvas (hit.kind == None). Dropping on a node's body but missing its
                // pin still does nothing, deliberately -- the author was aiming at that node, and
                // spawning a second one on top of it would be a worse guess than doing nothing.
                beginLinkDrop(linkDragFromNode_, linkDragFromPin_, linkDragFromIsOutput_);
                if (linkDropPending_) {
                    pendingSpawnCanvasPos_ = mouseCanvas;
                    ImGui::OpenPopup("##graphAddNode");
                }
            }
            dragMode_ = DragMode::None;
        }
        break;
    }
    case DragMode::MoveComment:
    case DragMode::ResizeComment: {
        fmt::OcGraphComment* c = findComment(activeComment_);
        if (!c) { dragMode_ = DragMode::None; break; }
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            const Vec2 delta = mouseCanvas - dragStartCanvas_;
            // Same lazy undo boundary MoveNodes uses, and the same 3px-of-travel threshold, so a
            // plain click to select a box does not push an identical state onto the stack.
            if (!commentUndoPushed_ && vecLen(delta) * view_.zoom > 3.0f) {
                history_.push(pendingCommentSnapshot_);
                commentUndoPushed_ = true;
            }
            if (dragMode_ == DragMode::MoveComment) {
                c->x = commentDragStartPos_.x + delta.x;
                c->y = commentDragStartPos_.y + delta.y;
                for (const auto& kv : commentCapturedStart_) displayPos_[kv.first] = kv.second + delta;
            } else {
                c->w = std::max(static_cast<f64>(commentDragStartSize_.x + delta.x), static_cast<f64>(kCommentMinPx));
                c->h = std::max(static_cast<f64>(commentDragStartSize_.y + delta.y), static_cast<f64>(kCommentMinPx));
            }
        } else {
            if (commentUndoPushed_) {
                // The nodes that rode along commit their new positions into graph_, exactly as the
                // end of a MoveNodes drag does -- displayPos_ alone is not saved.
                for (const auto& kv : commentCapturedStart_) {
                    for (auto& n : graph_.nodes) {
                        if (n.id != kv.first) continue;
                        const Vec2 p = displayPos_[kv.first];
                        n.x = p.x; n.y = p.y;
                        break;
                    }
                }
                dirty_ = true;
            }
            commentCapturedStart_.clear();
            dragMode_ = DragMode::None;
        }
        break;
    }
    case DragMode::None: default: break;
    }

    // Wheel zooms by default; Shift+wheel or MouseWheelH pans instead. MouseWheelH was unread before
    // this and is newly bound; Shift+wheel is REPURPOSED, not new -- the old handler checked bare
    // io.MouseWheel with no Shift exclusion, so Shift-held already zoomed identically and gained
    // nothing from the modifier. A distinct pan meaning loses nothing and gives a mouse-only user a
    // second direct-pan gesture. The bare-wheel zoom path below is unchanged.
    if (hovered && io.KeyShift && io.MouseWheel != 0.0f) {
        // Sign matches "content scrolls like a document": wheel-up moves the CONTENT down
        // (panPx.y grows), same as scrolling up in a text editor revealing earlier content --
        // not the "camera pans up" reading, which would be the opposite sign.
        constexpr f32 kWheelPanPxPerNotch = 60.0f;
        viewTouched_ = true;
        view_.panPx.y += io.MouseWheel * kWheelPanPxPerNotch * dpi;
    } else if (hovered && io.MouseWheel != 0.0f) {
        const f32 newZoom = std::clamp(view_.zoom * std::pow(1.1f, io.MouseWheel), minZoomForDpi(dpi), kMaxZoom);
        viewTouched_ = true;
        view_ = zoomAroundScreenPoint(view_, newZoom, mouseScreen);
    }
    if (hovered && io.MouseWheelH != 0.0f) {
        // Always horizontal pan, no modifier needed: this axis has no existing zoom meaning to collide
        // with (the code above only ever reads io.MouseWheel), so it is free in every state.
        constexpr f32 kWheelPanPxPerNotch = 60.0f;
        viewTouched_ = true;
        view_.panPx.x -= io.MouseWheelH * kWheelPanPxPerNotch * dpi;
    }

    if (canvasFocused) {
        // THROUGH THE REGISTRY, so a rebind made on the Preferences page applies in here too. These
        // were nine hardcoded keys duplicating commands that already existed, which meant rebinding
        // Copy changed it everywhere except the canvas a node author works in all day.
        //
        // Delete/Undo/Redo/Copy/Paste/Duplicate and Frame Selected are the SAME commands as the
        // level viewport's, widened to this scope rather than cloned -- there is one "Copy" and the
        // user rebinds it once. Comment Box and Frame Everything are genuinely graph-only and have
        // their own rows.
        auto& kb = editor::keybinds();
        using editor::CommandId;

        if (kb.pressed(CommandId::EditDelete, io)) deleteSelection();
        // C wraps the selection in a comment box -- the same key Blueprint binds it to, and
        // the reason the gesture is worth having at all: drawing a box by hand around six
        // nodes and then nudging its edges is enough work that nobody does it.
        if (kb.pressed(CommandId::GraphCommentBox, io) && !selectedNodes_.empty())
            addCommentAroundSelection(dpi);

        // CTRL+SHIFT+Z STAYS HARDCODED, and dropping it here is the trap this promotion sets. It was
        // an alternate spelling of Redo folded into the undo branch (`if (io.KeyShift) redo()`), and
        // EditUndo checks Shift -- so routing undo through the registry makes Ctrl+Shift+Z match
        // nothing at all, and it would have gone quietly dead. Kept exactly as SandboxApp keeps its
        // own copy, and deliberately NOT a second rebindable command: one Redo that can drift into
        // two spellings is worse than one spelling that cannot be rebound.
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false)) redo();
        if (kb.pressed(CommandId::EditUndo, io)) undo();
        if (kb.pressed(CommandId::EditRedo, io)) redo();
        // PASTE LANDS UNDER THE CURSOR -- expected, and it's what makes pasting the same clipboard
        // twice land the copies somewhere different; a fixed offset would stack them.
        if (kb.pressed(CommandId::EditCopy, io)) copySelection();
        if (kb.pressed(CommandId::EditPaste, io)) pasteClipboard(mouseCanvas);
        if (kb.pressed(CommandId::EditDuplicate, io)) duplicateSelection();
        // F frames the selection (falling back to everything), Home always frames everything --
        // the same two bindings Blueprint uses, and the reason for having both is that "show me
        // what I just clicked" and "show me where I am" are different questions.
        if (kb.pressed(CommandId::ViewFrameSelected, io))
            frameSelection(Vec2{canvasSize.x, canvasSize.y}, dpi);
        if (kb.pressed(CommandId::GraphFrameAll, io)) frameAll(Vec2{canvasSize.x, canvasSize.y}, dpi);
    }

    // Comment boxes draw at the back, then links, then nodes -- via ImDrawListSplitter, not
    // submission order (which would put a link over whichever node happened to draw after it; the
    // splitter is what imgui.h itself recommends). Three channels, not two: a box must sit behind the
    // wires too, or a wire crossing it would read as a pane of glass rather than scenery.
    dl->ChannelsSplit(3);

    // Which pins are wired, built once per frame rather than searched per pin (both link ends count).
    // Used below to fill a connected pin and leave an unconnected one hollow -- previously an unwired
    // exec input (which never runs) looked identical to a wired one.
    //
    // Text scales with zoom and stops drawing once it stops being readable. Labels used to be
    // submitted at UI font size regardless of zoom, so a 63-node graph zoomed out to fit (zoom ~0.2 at
    // 300% DPI) drew sixty full-size pin labels stacked on each other -- a type bug that looked like a
    // layout bug. Two thresholds, not one: pin labels (5x as many, and the ones that collide)
    // disappear first; node titles survive further out, since at overview zoom the only remaining
    // question is what the shapes ARE.
    const float uiFontPx     = ImGui::GetFontSize();
    const float scaledFontPx = uiFontPx * view_.zoom;
    const bool  showPinText  = scaledFontPx >= 7.0f;
    const bool  showTitles   = scaledFontPx >= 4.5f;
    ImFont* const font       = ImGui::GetFont();

    std::set<std::pair<std::string, std::string>> connectedPins;
    for (const auto& link : graph_.links) {
        connectedPins.emplace(link.sourceNode, link.sourcePin);
        connectedPins.emplace(link.destNode, link.destPin);
    }

    dl->ChannelsSetCurrent(0); // comment boxes, behind even the wires
    for (const auto& c : graph_.comments) {
        const ImVec2 pMin = toScreenAbs(Vec2{static_cast<f32>(c.x), static_cast<f32>(c.y)});
        const ImVec2 pMax = toScreenAbs(Vec2{static_cast<f32>(c.x + c.w), static_cast<f32>(c.y + c.h)});
        const f32 barH = kCommentBarPx * dpi * view_.zoom;
        const bool sel = (c.id == selectedComment_);
        const int r = std::clamp(c.r, 0, 255), g = std::clamp(c.g, 0, 255), b = std::clamp(c.b, 0, 255);
        // BODY AT ALPHA 40. It has to be low enough that the grid and any wire crossing the box are
        // still readable through it -- a box that obscures what it contains is worse than no box --
        // and high enough that the region reads as one thing from across a zoomed-out canvas.
        dl->AddRectFilled(pMin, pMax, IM_COL32(r, g, b, 40), 4.0f * dpi);
        dl->AddRectFilled(pMin, ImVec2(pMax.x, pMin.y + barH), IM_COL32(r, g, b, 200), 4.0f * dpi,
                          ImDrawFlags_RoundCornersTop);
        dl->AddRect(pMin, pMax, sel ? kSelectionCol : IM_COL32(r, g, b, 220), 4.0f * dpi, 0,
                    (sel ? 2.5f : 1.5f) * dpi);
        // The resize grip, drawn as two short strokes in the bottom-right corner. It is only ever
        // drawn -- the hit test that matches it lives in commentAtCanvas, in canvas units, so the two
        // agree at every zoom without either measuring the other.
        const f32 grip = kCommentGripPx * dpi * view_.zoom;
        if (grip > 4.0f) {
            const ImU32 gc = IM_COL32(255, 255, 255, sel ? 200 : 110);
            dl->AddLine(ImVec2(pMax.x - grip, pMax.y - 2.0f * dpi), ImVec2(pMax.x - 2.0f * dpi, pMax.y - grip), gc, 1.5f * dpi);
            dl->AddLine(ImVec2(pMax.x - grip * 0.5f, pMax.y - 2.0f * dpi), ImVec2(pMax.x - 2.0f * dpi, pMax.y - grip * 0.5f), gc, 1.5f * dpi);
        }
        // A comment title is a landmark to navigate a zoomed-out graph by, so it gets a floor the
        // node labels don't: it shrinks with the box but never below readable size. The existing
        // clip rect keeps an oversized title inside its own bar.
        const float cmtFontPx = std::max(uiFontPx * view_.zoom, 11.0f);
        if (!c.text.empty() && barH > 3.0f) {
            dl->PushClipRect(pMin, ImVec2(pMax.x, pMin.y + barH), true);
            dl->AddText(font, cmtFontPx, ImVec2(pMin.x + 6.0f * dpi * view_.zoom, pMin.y + 2.0f * dpi * view_.zoom),
                         IM_COL32(255, 255, 255, 255), c.text.c_str());
            dl->PopClipRect();
        }
    }

    dl->ChannelsSetCurrent(1); // links
    for (usize i = 0; i < graph_.links.size(); ++i) {
        const auto& link = graph_.links[i];
        const GraphPinLayout* sp = findPinLayout(layouts_, link.sourceNode, link.sourcePin);
        const GraphPinLayout* dp = findPinLayout(layouts_, link.destNode, link.destPin);
        if (!sp || !dp) continue; // a link to a pin this file's PIN records never declared; draw nothing rather than guess
        const GraphBezier b = linkBezier(sp->pos, dp->pos);
        const bool selected = (static_cast<int>(i) == selectedLink_);
        const ImU32 col = selected ? kSelectionCol : colorForType(sp->type);
        // Exec wires draw HEAVIER than data wires: the exec chain is the spine (the order things
        // happen in), everything else is an argument being fetched. Weight tells them apart from
        // across the canvas, at a distance where colour is still legible but shape is not.
        const f32 baseWidth = isExecPinType(sp->type) ? 3.0f : 2.0f;
        dl->AddBezierCubic(toScreenAbs(b.p1), toScreenAbs(b.p2), toScreenAbs(b.p3), toScreenAbs(b.p4),
                            col, (selected ? baseWidth + 1.0f : baseWidth) * dpi, 24);
    }

    dl->ChannelsSetCurrent(2); // nodes
    for (const auto& nl : layouts_) {
        const bool selected = std::find(selectedNodes_.begin(), selectedNodes_.end(), nl.nodeId) != selectedNodes_.end();
        const ImVec2 pMin = toScreenAbs(nl.min);
        const ImVec2 pMax = toScreenAbs(nl.max);
        const f32 headerH = style_.headerHeightPx * dpi * view_.zoom;

        // The descriptor is resolved BEFORE the header is drawn, because the header now takes
        // its colour from the node's category. It used to be looked up afterwards, purely for
        // the label, and every node got the same slate-blue bar.
        const fmt::OcGraphNode* srcNode = nullptr;
        for (const auto& n : graph_.nodes) if (n.id == nl.nodeId) { srcNode = &n; break; }
        const GraphNodeDesc* desc = srcNode ? findGraphNodeDesc(srcNode->type) : nullptr;
        const ImU32 headerCol = srcNode ? nodeHeaderColor(*srcNode, desc, graph_.variables)
                                        : headerColorForCategory(std::string());

        // A soft drop shadow, offset down-right, drawn before anything else in the node -- makes it
        // read as sitting ABOVE the wire layer rather than punched out of it. Matters more here than
        // in most UIs: the canvas behind a node is a mesh of bright wires, not empty background.
        const f32 shadowOff = 3.0f * dpi;
        dl->AddRectFilled(ImVec2(pMin.x + shadowOff, pMin.y + shadowOff),
                          ImVec2(pMax.x + shadowOff, pMax.y + shadowOff),
                          IM_COL32(0, 0, 0, 90), 6.0f * dpi);

        // Body in the engine's own card colour, not generic dark grey, so a node reads as part of
        // this editor. Fully opaque, deliberately -- at the old alpha 240 (94%) a wire behind a node
        // showed through as a faint smear, exactly what the channel split above exists to prevent.
        dl->AddRectFilled(pMin, pMax, IM_COL32(21, 25, 32, 255), 5.0f * dpi);
        dl->AddRectFilled(pMin, ImVec2(pMax.x, pMin.y + headerH), headerCol, 5.0f * dpi, ImDrawFlags_RoundCornersTop);
        // A hairline under the header. Cheap, and it stops a dark body and a dark header from
        // reading as one block on the categories whose colour is already close to the body.
        dl->AddLine(ImVec2(pMin.x, pMin.y + headerH), ImVec2(pMax.x, pMin.y + headerH),
                    IM_COL32(0, 0, 0, 90), 1.0f * dpi);
        dl->AddRect(pMin, pMax, selected ? kSelectionCol : IM_COL32(12, 14, 18, 255), 5.0f * dpi, 0, selected ? 2.5f * dpi : 1.0f * dpi);
        // THE NODE THE VALIDATOR NAMED, ringed in amber. Drawn OVER the selection ring rather than
        // instead of it, so a node that is both selected and broken still reads as both -- and
        // outside the node's own rect, so it cannot be mistaken for the node's border colour.
        // Only ever one node: the validator reports the first problem it finds, not a list.
        if (!validateNode_.empty() && nl.nodeId == validateNode_) {
            const f32 pad = 3.0f * dpi;
            dl->AddRect(ImVec2(pMin.x - pad, pMin.y - pad), ImVec2(pMax.x + pad, pMax.y + pad),
                        IM_COL32(240, 150, 60, 255), 7.0f * dpi, 0, 2.5f * dpi);
        }
        // AND A GREEN RING FOR A NODE THAT JUST RAN, fading with age. Drawn OUTSIDE the validator's
        // so a node that is both broken and executing shows both, and drawn from real recorded hits
        // rather than an animation -- a plausible-looking glow that did not correspond to execution
        // would be worse than nothing, because it would be trusted.
        if (const auto hit = nodeHitAges_.find(nl.nodeId); hit != nodeHitAges_.end()) {
            const f32 t = 1.0f - (hit->second / kNodeHitFadeSec);
            if (t > 0.0f) {
                const f32 pad = 6.0f * dpi;
                dl->AddRect(ImVec2(pMin.x - pad, pMin.y - pad), ImVec2(pMax.x + pad, pMax.y + pad),
                            IM_COL32(90, 230, 130, static_cast<int>(255.0f * (t < 1.0f ? t : 1.0f))),
                            9.0f * dpi, 0, 2.0f * dpi);
            }
        }
        if (showTitles) {
            const std::string label = srcNode ? nodeTitle(*srcNode, desc, graph_.variables) : nl.nodeId;
            dl->PushClipRect(pMin, ImVec2(pMax.x, pMin.y + headerH), true);
            dl->AddText(font, scaledFontPx, ImVec2(pMin.x + 6.0f * dpi * view_.zoom, pMin.y + 3.0f * dpi * view_.zoom),
                         IM_COL32(255, 255, 255, 255), label.c_str());
            dl->PopClipRect();
        }

        for (const auto& pl : nl.pins) {
            const ImVec2 dot = toScreenAbs(pl.pos);
            const f32 r = style_.pinRadiusPx * dpi * view_.zoom;
            const bool isLinkEnd = (dragMode_ == DragMode::DrawLink && nl.nodeId == linkDragFromNode_ && pl.name == linkDragFromPin_);
            const bool wired = connectedPins.count({nl.nodeId, pl.name}) != 0;
            const ImU32 pinCol = colorForType(pl.type);
            // Exec pins draw as an arrow, data pins as a circle -- shape, not just colour (colour
            // alone fails a colour-blind reader or a greyscale screenshot). The arrow also POINTS
            // (it used to draw a diamond, which doesn't), showing direction on both sides of the node
            // so the chain reads left-to-right without following a wire -- Blueprint's shape, for the
            // same learned-language reason as the pin colours. Hollow means unconnected for both
            // shapes: an unwired exec input never runs, an unwired data input falls back to its
            // default -- both invisible before this.
            if (isExecPinType(pl.type)) {
                // Slightly narrower than tall, so it reads as an arrowhead rather than a wedge.
                const ImVec2 arrow[3] = {
                    ImVec2(dot.x - r * 0.85f, dot.y - r),
                    ImVec2(dot.x + r * 0.95f, dot.y),
                    ImVec2(dot.x - r * 0.85f, dot.y + r),
                };
                if (wired) dl->AddConvexPolyFilled(arrow, 3, pinCol);
                // (points, num_points, col, thickness, flags) -- current AddPolyline signature
                // (imgui.h:3527); pre-1.92.7 took (col, flags, thickness). imconfig.h's compat shim
                // means either order links, but this uses the current order deliberately. Outline
                // draws in both states: over the fill it separates a white arrow from a white-ish
                // header; without a fill, it IS the pin.
                dl->AddPolyline(arrow, 3, wired ? IM_COL32(40, 40, 40, 255) : pinCol,
                                (wired ? 1.0f : 2.0f) * dpi, ImDrawFlags_Closed);
            } else if (wired) {
                dl->AddCircleFilled(dot, r, pinCol);
            } else {
                // Ring, not disc. Drawn a hair inside r so the stroke's outer edge lands where the
                // filled version's edge does and a pin does not appear to grow when it is wired.
                dl->AddCircle(dot, r - 1.0f * dpi, pinCol, 0, 2.0f * dpi);
            }
            if (isLinkEnd) dl->AddCircle(dot, r + 2.0f * dpi, kSelectionCol, 0, 2.0f * dpi);
            if (!showPinText) continue;
            // Measured at the SCALED size, not the UI size, or an output label would be positioned
            // from a width it no longer has and would drift off its own node as you zoom out.
            const ImVec2 textSize = font->CalcTextSizeA(scaledFontPx, FLT_MAX, 0.0f, pl.name.c_str());
            const f32 tx = pl.isOutput ? dot.x - textSize.x - r - 3.0f * dpi : dot.x + r + 3.0f * dpi;
            dl->AddText(font, scaledFontPx, ImVec2(tx, dot.y - textSize.y * 0.5f),
                         IM_COL32(220, 220, 220, 255), pl.name.c_str());
        }
    }
    dl->ChannelsMerge();

    // ---- overlays: in-progress link drag, box-select rect, rejection banner (drawn AFTER merge, so
    // always on top regardless of channel) ----------------------------------------------------------
    if (dragMode_ == DragMode::DrawLink) {
        const GraphPinLayout* from = findPinLayout(layouts_, linkDragFromNode_, linkDragFromPin_);
        if (from) {
            const Vec2 a = linkDragFromIsOutput_ ? from->pos : mouseCanvas;
            const Vec2 b = linkDragFromIsOutput_ ? mouseCanvas : from->pos;
            const GraphBezier bez = linkBezier(a, b);
            dl->AddBezierCubic(toScreenAbs(bez.p1), toScreenAbs(bez.p2), toScreenAbs(bez.p3), toScreenAbs(bez.p4),
                                IM_COL32(255, 255, 255, 180), 2.0f * dpi, 24);
        }
    }
    if (dragMode_ == DragMode::BoxSelect) {
        const ImVec2 a = toScreenAbs(dragStartCanvas_);
        const ImVec2 b = toScreenAbs(boxSelectCurrentCanvas_);
        dl->AddRectFilled(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), IM_COL32(120, 160, 255, 40));
        dl->AddRect(ImVec2(std::min(a.x, b.x), std::min(a.y, b.y)), ImVec2(std::max(a.x, b.x), std::max(a.y, b.y)), IM_COL32(120, 160, 255, 200));
    }
    if (!lastRejectMsg_.empty() && (ImGui::GetTime() - lastRejectAtSec_) < 4.0) {
        // lastRejectMsg_ already carries its own full message (a link rejection embeds "Connection
        // refused: " itself; a blocked variable delete embeds its own text) -- see
        // reportLinkRejection's comment for why the prefix moved to the caller.
        const std::string& msg = lastRejectMsg_;
        dl->AddRectFilled(originIm, ImVec2(originIm.x + ImGui::CalcTextSize(msg.c_str()).x + 16.0f * dpi, originIm.y + 22.0f * dpi), IM_COL32(90, 25, 25, 220));
        dl->AddText(ImVec2(originIm.x + 8.0f * dpi, originIm.y + 4.0f * dpi), IM_COL32(255, 210, 210, 255), msg.c_str());
    }

    // ---- comment box properties: title, colour, delete. Opened by double-clicking a box's title bar
    // (see the hit-test above), which is where an author reaches for it first.
    if (commentPopupQueued_) { ImGui::OpenPopup("##graphCommentProps"); commentPopupQueued_ = false; }
    if (ImGui::BeginPopup("##graphCommentProps")) {
        fmt::OcGraphComment* c = findComment(commentEditId_);
        if (!c) {
            ImGui::CloseCurrentPopup();
        } else {
            ImGui::TextUnformatted("Comment box");
            ImGui::Separator();
            ImGui::SetNextItemWidth(220.0f * dpi);
            // Same activate/apply-live/deactivate boundary the attribute and variable fields use:
            // graph_ is untouched while keys are landing, and setCommentText -- with its pushUndo --
            // fires once, when the field is left. Otherwise one typed title would be thirty undo steps.
            ImGui::InputText("Title", commentEditBuf_, sizeof commentEditBuf_);
            if (ImGui::IsItemDeactivatedAfterEdit()) setCommentText(commentEditId_, commentEditBuf_);
            float col[3] = {c->r / 255.0f, c->g / 255.0f, c->b / 255.0f};
            if (ImGui::ColorEdit3("Colour", col, ImGuiColorEditFlags_NoInputs)) {
                setCommentColor(commentEditId_, static_cast<int>(col[0] * 255.0f + 0.5f),
                                static_cast<int>(col[1] * 255.0f + 0.5f),
                                static_cast<int>(col[2] * 255.0f + 0.5f));
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Delete box")) {
                deleteComment(commentEditId_);
                commentEditId_.clear();
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndPopup();
    }

    // ---- right-click "add node" palette, built entirely from graphNodeCatalog() -- see
    // GraphNodeDefs.hpp's own header comment for why this is the one place a node type is registered.
    // THE NODE CONTEXT MENU. Every item here already existed as a keyboard shortcut and nowhere
    // else -- an author who did not already know Ctrl+D duplicates could not find out from the
    // editor. Shortcut labels are shown beside each item precisely so this menu teaches them.
    //
    // Acts on the SELECTION, not on rightClickNode_: the press handler has already made the two
    // agree (it selects what was right-clicked unless that node is already part of a larger
    // selection), so "Delete" on one of five selected nodes deletes five, as it does from the
    // keyboard. Routing through the same copySelection/pasteClipboard/duplicateSelection/
    // deleteSelection calls the shortcuts use means undo, id remapping and link pruning behave
    // identically whichever way the command was issued -- there is no second implementation here to
    // drift.
    if (ImGui::BeginPopup("##graphNodeMenu")) {
        const bool onNode = !rightClickNode_.empty();
        const int  count  = static_cast<int>(selectedNodes_.size());
        if (onNode) {
            if (count > 1) ImGui::TextDisabled("%d nodes selected", count);
            else            ImGui::TextDisabled("%s", rightClickNode_.c_str());
            ImGui::Separator();
            if (ImGui::MenuItem("Cut", "Ctrl+X"))       { copySelection(); deleteSelection(); }
            if (ImGui::MenuItem("Copy", "Ctrl+C"))      copySelection();
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, !clipboardEmpty()))
                pasteClipboard(pendingSpawnCanvasPos_);
            if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicateSelection();
            ImGui::Separator();
            // BREAK LINKS is the one command with no keyboard shortcut at all -- before this menu
            // the only way to disconnect a node was to click each wire and press Delete.
            if (ImGui::MenuItem("Break Links", nullptr, false, selectionHasLinks()))
                breakLinksOnSelection();
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del")) deleteSelection();
        } else {
            // A link was right-clicked. Only two things can sensibly be done to a wire, and both
            // are the same thing -- so this arm is short on purpose rather than padded to match.
            ImGui::TextDisabled("Link");
            ImGui::Separator();
            if (ImGui::MenuItem("Delete", "Del")) deleteSelection();
        }
        ImGui::EndPopup();
    }

    if (ImGui::BeginPopup("##graphAddNode")) {
        // WHEN THE PALETTE WAS OPENED BY DROPPING A WIRE, say so and say what is being hidden. A
        // silently shortened list reads as a missing node rather than as a filter, which is the
        // same failure the search box's "...and N more" line exists to avoid.
        if (linkDropPending_) {
            ImGui::TextDisabled("connecting %s.%s (%s) -- showing types that accept it",
                                linkDropFromNode_.c_str(), linkDropFromPin_.c_str(),
                                linkDropFromType_.c_str());
            ImGui::Separator();
        }
        // Above the categories, not inside one: a comment box is not a node, has no pins, and
        // filing it under a node family would be the first place an author looked and the last
        // place they found it.
        // Every declared function gets a real "Call <name>" entry, regenerated from graph_.functions
        // every frame, so renaming one renames its palette row with no second list to keep in step. A
        // call node has no fixed pin shape (it takes the callee's), so it can't live in the static catalog.
        if (!graph_.functions.empty() && ImGui::BeginMenu("Call Function")) {
            for (const auto& f : graph_.functions) {
                std::string row = f.name + (f.pure ? "  (pure)" : "");
                if (ImGui::MenuItem(row.c_str())) addCallNode(f.name, pendingSpawnCanvasPos_);
            }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Comment Box", "C")) {
            const Vec2 a = pendingSpawnCanvasPos_;
            addComment(a, Vec2{a.x + 320.0f * dpi, a.y + 180.0f * dpi}, "Comment");
        }
        ImGui::Separator();

        // The search box, focused first when the popup opens. 240 node types across 23 categories
        // used to be reachable only by knowing which submenu a node was filed under (VecAdd is
        // Vector, not Math) -- typing is how anyone who hasn't memorised the catalog finds one.
        // Matching and ranking live in GraphNodeDefs.hpp so a test can drive them.
        if (ImGui::IsWindowAppearing()) {
            addSearch_[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(260.0f * dpi);
        ImGui::InputTextWithHint("##addsearch", "Search nodes...", addSearch_, sizeof addSearch_);

        if (addSearch_[0] != '\0') {
            const GraphNodeDomain searchDomain = openGraphDomain();
            const usize kShown = 40;
            const std::vector<const GraphNodeDesc*> hits =
                graphPaletteSearch(addSearch_, searchDomain, kShown);
            if (hits.empty()) {
                ImGui::TextDisabled("no node matches");
            } else {
                for (const GraphNodeDesc* d : hits) {
                    if (linkDropPending_ && !linkDropAccepts(*d)) continue;
                    // The category rides on the row rather than being a header: a ranked list is not
                    // grouped, and a reader still needs to know that Add is Math and VecAdd is Vector.
                    const std::string row = d->displayName + "##s" + d->typeId;
                    if (ImGui::MenuItem(row.c_str())) {
                        spawnAndConnectLinkDrop(d->typeId, pendingSpawnCanvasPos_);
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", d->category.c_str());
                }
                // NEVER A SILENT TRUNCATION. A capped list that just stops looks like the whole
                // answer, and "there is no such node" is the wrong thing to learn from a full box.
                const usize total = graphPaletteSearchCount(addSearch_, searchDomain);
                if (total > hits.size())
                    ImGui::TextDisabled("...and %zu more; type more to narrow", total - hits.size());
            }
        }
        // The category menus are the EMPTY-BOX view, so an `else` and not an early return: EndChild()
        // and the entire details panel are drawn after this popup, and returning from here would take
        // the right-hand side of the editor with it.
        else {
        ImGui::Separator();
        // ONLY THIS GRAPH'S OWN VOCABULARY. A material graph has no Branch, Spawn or CharacterMove --
        // those compile to IL and call the framework, while a material is arithmetic evaluated per
        // pixel with nothing to call. Offering them would suggest nodes whose only outcome is a
        // compile error. Filtered here, the ONE place the palette is built, so an empty category
        // doesn't appear at all.
        const GraphNodeDomain domain = openGraphDomain();
        std::vector<std::string> categories;
        for (const auto& d : graphNodeCatalog()) {
            if ((d.domain & domain) == 0u) continue;
            // A category whose every member refuses the pending wire is not shown at all, for the
            // reason the domain filter above gives: an empty submenu is a dead end that looks like
            // a place the node might be.
            if (linkDropPending_ && !linkDropAccepts(d)) continue;
            if (std::find(categories.begin(), categories.end(), d.category) == categories.end())
                categories.push_back(d.category);
        }
        for (const auto& cat : categories) {
            // See GraphNodeDefs.hpp's Function block: these three exist in the catalog for their
            // name and colour, and are created by the Functions panel, which knows which function
            // they belong to. Dropping a bare one produces a node with no pins and no owner.
            if (cat == "Function") continue;
            if (ImGui::BeginMenu(cat.c_str())) {
                for (const auto& d : graphNodeCatalog()) {
                    if (d.category != cat || (d.domain & domain) == 0u) continue;
                    if (linkDropPending_ && !linkDropAccepts(d)) continue;
                    // Thin glue: everything the drop actually DOES is spawnAndConnectLinkDrop (which
                    // is addNodeFromCatalog when no wire is pending), so it can be driven by a test
                    // with no ImGui context.
                    if (ImGui::MenuItem(d.displayName.c_str()))
                        spawnAndConnectLinkDrop(d.typeId, pendingSpawnCanvasPos_);
                }
                ImGui::EndMenu();
            }
        }
        }   // else: the category menus
        ImGui::EndPopup();
    } else if (linkDropPending_ && !ImGui::IsPopupOpen("##graphAddNode")) {
        // DISARMED WHEN THE POPUP GOES AWAY WITHOUT A PICK (Escape, or a click outside). Without
        // this the gesture stays armed, and the NEXT ordinary right-click Add Node would silently
        // wire the node it spawns to a pin the author dragged from minutes ago.
        cancelLinkDrop();
    }

    ImGui::EndChild();

    // Details panel: Variables (graph-level) above a selected node's attributes (Gap B). Thin ImGui
    // glue only -- the model lives in GraphEditor's own addVariable/renameVariable/retypeVariable/
    // setVariableDefault/deleteVariable (this file, above) for the Variables panel, and in
    // GraphEditorGeometry.hpp's getNodeAttribute/setNodeAttribute/removeNodeAttribute/
    // computeAttributeRows for the per-node rows below -- both exercised headlessly by
    // GraphEditorGeometryTest and GraphEditorLoadSaveTest.
    //
    // The handle mutates detailsColW_ for the NEXT frame (`detailsW` above is this frame's already-
    // captured value), and persists it -- as a DPI-independent pixel width, ActorEditor's own
    // convention -- the instant the drag ends. Matches ActorEditor's own right-column splitter site
    // (ActorEditor.cpp) exactly: capture-then-size, drag-after, persist-on-release.
    bool detailsReleased = false;
    splitterHandle("##graphSplit", 6.0f * dpi, &detailsColW_, avail.x, minDetails, minCanvas,
                    &detailsReleased);
    if (detailsReleased) {
        setPrefFloat(kPrefDetailsColumn, detailsColW_ / dpi);
        flushEditorPrefs();
    }
    ImGui::BeginChild("##graphDetails", ImVec2(detailsW, std::max(avail.y, 80.0f * dpi)), true);

    drawFunctionsPanel(dpi);

    // Variables panel: declare / rename / retype / delete. Lives ABOVE the per-node section, drawn
    // regardless of selection -- a variable belongs to the GRAPH, not whichever node is selected, and
    // right after opening a graph (nothing selected) is exactly when an author needs to declare one.
    // This is also THE fix for the bug the task brief leads with: a freshly palette-spawned SetVar has
    // no variable to name yet, and this panel -- not a free-text field -- is where one gets created.
    if (ImGui::CollapsingHeader("Variables", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (graph_.variables.empty()) {
            ImGui::TextDisabled("No variables declared.");
        }
        // Snapshot names before iterating: a Delete pressed on row i mutates graph_.variables mid-loop
        // (pushUndo() + erase inside deleteVariable), invalidating any iterator/index into the vector
        // for later rows. Looking each one up FRESH by name every iteration tolerates that -- a name
        // deleted by an earlier row this frame is simply skipped, not read through a dangling ref.
        std::vector<std::string> varNames;
        varNames.reserve(graph_.variables.size());
        for (const auto& v : graph_.variables) varNames.push_back(v.name);

        for (const std::string& vname : varNames) {
            const fmt::OcGraphVariable* vptr = nullptr;
            for (const auto& v : graph_.variables) if (v.name == vname) { vptr = &v; break; }
            if (!vptr) continue; // deleted by an earlier row's Delete button this same frame
            const fmt::OcGraphVariable& v = *vptr;

            ImGui::PushID(("##var_" + vname).c_str());
            ImGui::PushItemWidth(70.0f * dpi);

            // ---- name (rename), same activate/apply-live/deactivate shape as attrEditRowKey_/
            // attrEditBuf_ above, keyed by field+name so a delete elsewhere never bleeds into this
            // row's in-flight buffer.
            const std::string nameRowKey = "varname\x1f" + vname;
            char nameBuf[256];
            std::snprintf(nameBuf, sizeof nameBuf, "%s", (varEditRowKey_ == nameRowKey) ? varEditBuf_ : v.name.c_str());
            const bool nameChanged = ImGui::InputText("##name", nameBuf, sizeof nameBuf);
            if (ImGui::IsItemActivated()) varEditRowKey_ = nameRowKey;
            if (nameChanged && varEditRowKey_ == nameRowKey) std::snprintf(varEditBuf_, sizeof varEditBuf_, "%s", nameBuf);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                const std::string newName = varEditBuf_;
                if (newName != vname && !renameVariable(vname, newName)) {
                    showRejectionBanner("cannot rename '" + vname + "' to '" + newName +
                                         "': empty, contains whitespace, or already used by another variable");
                }
                varEditRowKey_.clear();
            } else if (ImGui::IsItemDeactivated()) {
                varEditRowKey_.clear();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rename -- updates every GetVar/SetVar that uses this variable");
            ImGui::SameLine();

            // ---- type (retype): a Combo, not free text -- VAR only ever declares float/int/bool
            // (see isValidVarType's own comment), so there is nothing a text field would offer that a
            // fixed 3-item list doesn't already cover, and a Combo can't typo its way into a 4th.
            static const char* kVarTypes[] = {"float", "int", "bool"};
            int typeIdx = 0;
            for (int i = 0; i < 3; ++i) if (v.type == kVarTypes[i]) { typeIdx = i; break; }
            if (ImGui::Combo("##type", &typeIdx, kVarTypes, 3)) retypeVariable(vname, kVarTypes[typeIdx]);
            ImGui::SameLine();

            const std::string defRowKey = "vardefault\x1f" + vname;
            char defBuf[256];
            std::snprintf(defBuf, sizeof defBuf, "%s", (varEditRowKey_ == defRowKey) ? varEditBuf_ : v.defaultValue.c_str());
            const bool defChanged = ImGui::InputText("##default", defBuf, sizeof defBuf);
            if (ImGui::IsItemActivated()) varEditRowKey_ = defRowKey;
            if (defChanged && varEditRowKey_ == defRowKey) std::snprintf(varEditBuf_, sizeof varEditBuf_, "%s", defBuf);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                if (!setVariableDefault(vname, varEditBuf_))
                    showRejectionBanner("cannot set default for '" + vname + "': value contains whitespace, which this format cannot represent");
                varEditRowKey_.clear();
            } else if (ImGui::IsItemDeactivated()) {
                varEditRowKey_.clear();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Default value (optional) -- what a fresh GraphHost seeds this variable with");
            ImGui::PopItemWidth();
            ImGui::SameLine();

            // ---- delete: deleteVariable itself raises the rejection banner (naming every blocking
            // node) when refused, so there is nothing further to do with its return value here.
            if (ImGui::Button("X")) deleteVariable(vname);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Delete -- refused while any GetVar/SetVar still uses this variable");

            // Mismatch note: nodes whose OWN 'value' pin type disagrees with the variable's CURRENT
            // type. Computed fresh every frame from the live pins -- see retypeVariable's comment for
            // why a retype never rewrites those pins, and why this is what surfaces the mismatch.
            {
                std::vector<std::string> mismatched;
                for (const auto& n : graph_.nodes) {
                    const GraphNodeAttribute a = getNodeAttribute(n, "var");
                    if (!a.found || a.value != vname) continue;
                    for (const auto& p : n.pins) {
                        if (p.name == "value" && p.type != v.type) { mismatched.push_back(n.id); break; }
                    }
                }
                if (!mismatched.empty()) {
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "  used as %s by a node whose pin type disagrees:", v.type.c_str());
                    std::string names;
                    for (usize i = 0; i < mismatched.size(); ++i) { if (i) names += ", "; names += mismatched[i]; }
                    ImGui::TextColored(ImVec4(0.95f, 0.75f, 0.35f, 1.0f), "  %s", names.c_str());
                }
            }

            ImGui::PopID();
        }

        if (ImGui::Button("+ New Variable")) {
            addVariable(makeUniqueVariableName("Var"), "float", "");
        }
    }
    ImGui::Separator();

    if (selectedNodes_.size() != 1) {
        ImGui::TextDisabled(selectedNodes_.empty() ? "Select a node to edit its attributes."
                                                     : "Select a single node to edit its attributes.");
    } else {
        const std::string& nodeId = selectedNodes_.front();
        fmt::OcGraphNode* node = nullptr;
        for (auto& n : graph_.nodes) if (n.id == nodeId) { node = &n; break; }
        if (!node) {
            ImGui::TextDisabled("Selected node no longer exists.");
        } else {
            const GraphNodeDesc* desc = findGraphNodeDesc(node->type);
            ImGui::TextUnformatted(node->id.c_str());
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", node->type.c_str());
            ImGui::Separator();

            std::vector<std::pair<std::string, std::string>> declared;
            if (desc) {
                declared.reserve(desc->attributes.size());
                for (const GraphAttributeSpec& a : desc->attributes) declared.emplace_back(a.key, a.label);
            }
            const std::vector<GraphAttributeRow> rows = computeAttributeRows(*node, declared);

            if (rows.empty()) {
                ImGui::TextDisabled("This node type has no attributes.");
            }
            bool drewLeftoverHeader = false;
            for (const GraphAttributeRow& row : rows) {
                if (!row.declared && !drewLeftoverHeader) {
                    if (!declared.empty()) ImGui::Separator();
                    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
                    ImGui::TextDisabled("Other attributes (not recognised by this editor build, preserved on save):");
                    ImGui::PopTextWrapPos();
                    drewLeftoverHeader = true;
                }

                const std::string rowKey = node->id + "\x1f" + row.key;
                ImGui::PushID(rowKey.c_str());

                // THE PICKER: GetVar/SetVar's var= row gets a combo instead of the generic free-text
                // field below (field=/class=/param=/etc. keep that path unchanged -- deliberately
                // narrow, not a generic key=value framework). `row.key == "var"` is reachable only
                // for GetVar/SetVar (GraphNodeDefs.hpp), so no separate node-type check is needed.
                if (row.declared && row.key == "var") {
                    const bool known = !row.value.empty() &&
                        std::any_of(graph_.variables.begin(), graph_.variables.end(),
                                    [&](const fmt::OcGraphVariable& v) { return v.name == row.value; });
                    std::string preview = row.value.empty() ? "(none)" : row.value;
                    if (!row.value.empty() && !known) preview += "  [undeclared!]";
                    if (ImGui::BeginCombo(row.label.c_str(), preview.c_str())) {
                        if (ImGui::Selectable("(none)", row.value.empty())) clearAttribute(node->id, "var");
                        for (const fmt::OcGraphVariable& v : graph_.variables) {
                            const bool selected = (v.name == row.value);
                            const std::string itemLabel = v.name + "  (" + v.type + ")";
                            if (ImGui::Selectable(itemLabel.c_str(), selected)) setAttribute(node->id, "var", v.name);
                        }
                        ImGui::EndCombo();
                    }
                    if (!row.value.empty() && !known) {
                        // A GetVar/SetVar naming an undeclared variable doesn't compile
                        // (Graph.Validate() refuses it) and used to give no sign anything was wrong.
                        // Names the problem AND offers a one-click fix.
                        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "'%s' is not a declared variable.", row.value.c_str());
                        const std::string declareLabel = "Declare '" + row.value + "'";
                        if (ImGui::Button(declareLabel.c_str())) {
                            // Guess the type from the node's own 'value' pin (a freshly spawned
                            // GetVar/SetVar always has one) so the declared variable matches what
                            // the node is already wired to expect.
                            std::string guessType = "float";
                            for (const auto& p : node->pins) if (p.name == "value") { guessType = p.type; break; }
                            addVariable(row.value, guessType, "");
                        }
                    } else if (row.value.empty() && graph_.variables.empty()) {
                        // A freshly spawned SetVar with no variables declared yet has nothing for
                        // the combo to offer but "(none)" -- point at the panel that fixes that.
                        ImGui::TextDisabled("No variables declared -- add one in the Variables panel above.");
                    } else if (row.value.empty()) {
                        // var="" with a variable already declared: neither branch above fires, so
                        // without this the combo would show "(none)" as if that compiled. It doesn't
                        // -- OcGraphParser.cs refuses a missing var= exactly as an undeclared one.
                        // Reachable: testDeleteVariableRefusesWhileReferencedThenSucceeds produces
                        // this by clearing 'gv's var= to unblock a delete.
                        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "No variable selected -- pick one above; this node will not compile without one.");
                    }
                    ImGui::PopID();
                    continue; // this row is fully drawn; skip the generic InputText path below
                }

                char buf[512];
                if (attrEditRowKey_ == rowKey) {
                    std::snprintf(buf, sizeof buf, "%s", attrEditBuf_);
                } else {
                    std::snprintf(buf, sizeof buf, "%s", row.value.c_str());
                }

                const bool changed = ImGui::InputText(row.label.c_str(), buf, sizeof buf);
                if (ImGui::IsItemActivated()) attrEditRowKey_ = rowKey;
                if (changed && attrEditRowKey_ == rowKey) std::snprintf(attrEditBuf_, sizeof attrEditBuf_, "%s", buf);
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    const std::string newValue = attrEditBuf_;
                    if (newValue.empty()) clearAttribute(node->id, row.key);
                    else setAttribute(node->id, row.key, newValue);
                    attrEditRowKey_.clear();
                } else if (ImGui::IsItemDeactivated()) {
                    attrEditRowKey_.clear();
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
#else
    (void)dpi;
#endif
}


// The Viewport tab: a class graph's COMPONENT TREE, and a preview of the actor it assembles. Lives
// beside the node canvas rather than in its own editor because a .ocgraph with a CLASS record is one
// asset answering two questions (what the actor DOES, what it IS) edited into one file -- two tabs
// share a save path, undo stack and dirty flag; two editors would need all three duplicated.
//
// The preview is the one the Actor Editor already owns: sharedPreview() exists so every asset tab
// draws through one render feature (ActorEditor.hpp explains why a second would draw into this one's
// target), and previewComposeTransform is exported from there so a COMP-assembled actor and a
// designer-placed one cannot disagree about what "yaw 90" means.

namespace {

// The kinds this tab can create, in the order the Add menu lists them. `blurb` is the tooltip: what
// the kind is FOR, not what it is called, since the name is already on the row.
struct GraphComponentKind {
    const char* name;
    const char* blurb;
};
constexpr GraphComponentKind kGraphComponentKinds[] = {
    {"Scene",        "A named transform and nothing else -- a muzzle, a socket, an attach point."},
    {"Mesh",         "A drawn mesh. `mesh=` is content-relative, e.g. Meshes/cube.ocmesh."},
    {"SkeletalMesh", "A mesh plus the skeleton that poses it."},
    // NOT "on whatever this is parented to", which is what this said and which is false.
    // GraphComponentTree.Build creates ONE ENTITY PER COMP RECORD, and AnimSystem::tick poses an
    // entity only when CAnimator and CSkeletalMesh sit on the SAME one -- so a parented Animator is
    // a sibling of the mesh it means to drive, its clock ticks, and nothing moves. The old blurb
    // described the arrangement an author would reach for and get nothing from.
    {"Animator",     "A clip and the clock running it. Each COMP is its own entity, so this cannot "
                     "pose a SkeletalMesh beside it -- use the PlayAnimation node for that."},
    {"Particles",    "One emitter instance, playing a .ocparticle effect."},
    {"Camera",       "Camera parameters. Stored correctly; no renderer reads CCamera yet."},
    {"Light",        "A light. Stored correctly; no renderer reads CLight yet."},
    {"Fluid",        "A simulated fluid volume. Size comes from scale=; 1,1,1 is a 2x2x1 m pool."},
};

// The kind-specific attribute rows the details panel shows, per kind. Everything not listed here is
// still preserved in the file (extraTokens keeps it verbatim) -- this table only decides what gets a
// labelled row, so an attribute this build has never heard of survives being edited around.
struct GraphComponentAttrRow {
    const char* key;
    const char* label;
    const char* hint;
};
struct GraphComponentAttrSet {
    const char* kind;
    const GraphComponentAttrRow* rows;
    int count;
};
// Engine built-ins, not anything a template ships. These two rows used to read
// "Meshes/Blaster.ocmesh" and "M_Gun" -- the FirstPerson template's own gun and material -- so the
// palette suggested an asset only that one template has. Every other row here is an invented
// illustrative name belonging to no shipped content; only Mesh had drifted into naming real template
// files, since the editor must not know a template exists. cube.ocmesh and M_Crate ARE true engine
// built-ins (PreviewMeshCache's primitive; a material SandboxApp registers), so unlike the invented
// names these actually resolve in a blank project, which is what a hint should do.
constexpr GraphComponentAttrRow kMeshRows[] = {
    {"mesh", "Mesh", "Meshes/cube.ocmesh"},
    {"material", "Material", "M_Crate"},
};
constexpr GraphComponentAttrRow kSkelRows[] = {
    {"mesh", "Mesh", "Meshes/Hero.ocmesh"},
    {"material", "Material", "M_Hero"},
    {"skeleton", "Skeleton", "Skeletons/Hero.ocskel"},
};
constexpr GraphComponentAttrRow kAnimRows[] = {
    {"clip", "Clip", "Anims/Idle.ocanim"},
    {"speed", "Speed", "1"},
    {"weight", "Blend weight", "1"},
};
constexpr GraphComponentAttrRow kParticleRows[] = {
    {"effect", "Effect", "Effects/Muzzle.ocparticle"},
    {"seed", "Seed", "0"},
};
constexpr GraphComponentAttrRow kCameraRows[] = {
    {"fov", "FoV (deg)", "60"},
    {"near", "Near (cm)", "10"},
    {"far", "Far (cm)", "100000"},
    {"priority", "Priority", "0"},
};
constexpr GraphComponentAttrRow kLightRows[] = {
    {"kind", "Kind", "point | spot | directional"},
    {"r", "Red", "1"}, {"g", "Green", "1"}, {"b", "Blue", "1"},
    {"intensity", "Intensity (lux)", "100000"},
    {"range", "Range (cm)", "800"},
    {"inner", "Inner cone (deg)", "0"},
    {"outer", "Outer cone (deg)", "45"},
};
// A fluid's SIZE is deliberately not here: it comes from the generic `scale=` row every component
// already has (GraphComponentTree.ApplyFluid reads 100/100/50 cm per unit).
//
// Ordered preset, then real-world values, then raw solver knobs -- the order an author reaches for
// them. `preset` resolves to exactly the density/viscosity pair below it, shown deliberately (seeing
// what `water` means is the point). The raw four are the escape hatch, listed last: setting `damping`
// while a preset or density/viscosity is also set is REFUSED at spawn (fluids::FluidScene::spawn),
// not silently overridden.
constexpr GraphComponentAttrRow kFluidRows[] = {
    {"preset",     "Preset (water/oil/honey/lava)", ""},
    {"density",    "Density (kg/m^3)",              "998"},
    {"viscosity",  "Viscosity (Pa*s)",              "0.001"},
    {"compliance", "Compliance (raw)",              "1e-4"},
    {"damping",    "Damping (raw)",                 "0.1"},
    {"iterations", "Solver iterations (raw)",       "5"},
    {"pressure",   "Pressure (raw, -1 = derived)",  "-1"},
};
constexpr GraphComponentAttrSet kGraphComponentAttrs[] = {
    {"Mesh", kMeshRows, 2},
    {"SkeletalMesh", kSkelRows, 3},
    {"Animator", kAnimRows, 3},
    {"Particles", kParticleRows, 2},
    {"Camera", kCameraRows, 4},
    {"Light", kLightRows, 8},
    {"Fluid", kFluidRows, 7},
};

// The rows for a kind, or an empty set. Case-insensitive, because the parser does not care and an
// author typing `mesh` rather than `Mesh` should still get a details panel.
const GraphComponentAttrSet* attrSetFor(const std::string& kind) {
    for (const auto& set : kGraphComponentAttrs) {
        if (kind.size() != std::strlen(set.kind)) continue;
        bool same = true;
        for (usize i = 0; i < kind.size(); ++i) {
            const char a = static_cast<char>(std::tolower(static_cast<unsigned char>(kind[i])));
            const char b = static_cast<char>(std::tolower(static_cast<unsigned char>(set.kind[i])));
            if (a != b) { same = false; break; }
        }
        if (same) return &set;
    }
    return nullptr;
}

// SHORTEST ROUND-TRIPPING TEXT for a number, not "%f". A component transform is written straight back
// into the file, so `pos=12,0,-8` has to stay `pos=12,0,-8` and not become `pos=12.000000,0.000000,
// -8.000000` the first time someone opens the Viewport tab -- that is a diff on every component of
// every graph anyone looks at.
std::string compNum(f32 v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%g", static_cast<double>(v));
    return buf;
}

std::string compVec3(const f32 v[3]) {
    return compNum(v[0]) + "," + compNum(v[1]) + "," + compNum(v[2]);
}

// Reads `x,y,z` into `out`, leaving any component it cannot read alone. Same fallback direction as
// the C# parser's own ParseVec3, and for the same reason: `out` arrives holding the identity value,
// so a half-typed scale gives 2,1,2 rather than an actor flattened on one axis.
void compParseVec3(std::string_view text, f32 out[3]) {
    int axis = 0;
    usize at = 0;
    while (axis < 3 && at <= text.size()) {
        const usize comma = text.find(',', at);
        const std::string_view part = text.substr(at, comma == std::string_view::npos ? std::string_view::npos
                                                                                       : comma - at);
        if (!part.empty()) {
            char buf[64];
            const usize n = part.size() < sizeof buf - 1 ? part.size() : sizeof buf - 1;
            std::memcpy(buf, part.data(), n);
            buf[n] = '\0';
            char* endp = nullptr;
            const double parsed = std::strtod(buf, &endp);
            if (endp != buf) out[axis] = static_cast<f32>(parsed);
        }
        ++axis;
        if (comma == std::string_view::npos) break;
        at = comma + 1;
    }
}


// The three transform attributes, read off a component into arrays already holding the identity.
void compReadTransform(const fmt::OcGraphComponent& c, f32 pos[3], f32 rot[3], f32 scale[3]) {
    pos[0] = pos[1] = pos[2] = 0.0f;
    rot[0] = rot[1] = rot[2] = 0.0f;
    scale[0] = scale[1] = scale[2] = 1.0f;
    compParseVec3(fmt::componentAttr(c, "pos"), pos);
    compParseVec3(fmt::componentAttr(c, "rot"), rot);
    compParseVec3(fmt::componentAttr(c, "scale"), scale);
}

} // namespace

bool GraphEditor::componentIsAncestorOf(const std::string& maybeAncestor, const std::string& id) const {
    if (maybeAncestor == id) return true;
    std::string at = id;
    // Bounded by the component count, the same guard the parser's own cycle check uses: this runs on
    // a tree that is ALREADY valid, but it also runs mid-edit, in the frame where a reparent combo is
    // being evaluated -- which is exactly when a cycle would exist if this were the thing allowing it.
    for (usize hops = 0; hops <= graph_.components.size(); ++hops) {
        const fmt::OcGraphComponent* c = nullptr;
        for (const auto& o : graph_.components) if (o.id == at) { c = &o; break; }
        if (!c) return false;
        const std::string_view parent = fmt::componentAttr(*c, "parent");
        if (parent.empty()) return false;
        if (parent == maybeAncestor) return true;
        at = std::string(parent);
    }
    return false;
}

void GraphEditor::componentWorldMatrix(const std::string& id, float out[16]) const {
    constexpr f32 kIdentity[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
    std::memcpy(out, kIdentity, sizeof kIdentity);

    const fmt::OcGraphComponent* c = nullptr;
    for (const auto& o : graph_.components) if (o.id == id) { c = &o; break; }
    if (!c) return;

    f32 pos[3], rot[3], scale[3];
    compReadTransform(*c, pos, rot, scale);
    composeEditorTransform(pos, rot, scale, out);

    const std::string_view parent = fmt::componentAttr(*c, "parent");
    if (parent.empty()) return;
    // Recursive rather than iterative because the depth is a handful and the recursive form is the
    // one that reads as the definition: a component's world matrix is its local matrix times its
    // parent's world matrix. The parser has already proved the chain terminates.
    f32 parentWorld[16];
    componentWorldMatrix(std::string(parent), parentWorld);
    f32 combined[16];
    multiplyEditorTransform(out, parentWorld, combined);
    std::memcpy(out, combined, sizeof combined);
}

void GraphEditor::addComponent(const std::string& kind) {
    // A UNIQUE ID WITHOUT ASKING. The parser refuses duplicates, so an editor that offered `Mesh`
    // twice and produced two `mesh` records would author a file it cannot then open -- the worst
    // possible failure for a create button.
    std::string base;
    for (char ch : kind) base += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::string id = base;
    for (int n = 1; n < 10000; ++n) {
        bool taken = false;
        for (const auto& c : graph_.components) if (c.id == id) { taken = true; break; }
        if (!taken) break;
        id = base + std::to_string(n);
    }

    pushUndo();
    fmt::OcGraphComponent c;
    c.id = id;
    c.kind = kind;
    // Parented to whatever is selected, which is what a component tree editor is expected to do:
    // building a hierarchy means adding under the thing just clicked, not at the root every time.
    if (!selectedComponent_.empty()) fmt::setComponentAttr(c, "parent", selectedComponent_);
    graph_.components.push_back(std::move(c));
    selectedComponent_ = id;
    dirty_ = true;
    previewFramed_ = false;
}

void GraphEditor::deleteComponentSubtree(const std::string& id) {
    if (id.empty()) return;
    bool found = false;
    for (const auto& c : graph_.components) if (c.id == id) { found = true; break; }
    if (!found) return;

    pushUndo();
    // THE WHOLE SUBTREE, not just the one record. A child left behind would name a parent that no
    // longer exists, which the parser refuses on the next load -- deleting one component would make
    // the file unopenable. Blueprint deletes the subtree too, matching what an author expects.
    std::vector<fmt::OcGraphComponent> kept;
    kept.reserve(graph_.components.size());
    for (const auto& c : graph_.components)
        if (!componentIsAncestorOf(id, c.id)) kept.push_back(c);
    graph_.components = std::move(kept);
    if (selectedComponent_ == id || !std::any_of(graph_.components.begin(), graph_.components.end(),
                                                  [&](const fmt::OcGraphComponent& c) { return c.id == selectedComponent_; }))
        selectedComponent_.clear();
    dirty_ = true;
    previewFramed_ = false;
}

void GraphEditor::setComponentParent(const std::string& id, const std::string& parentId) {
    // Refusing rather than silently correcting: the combo below never OFFERS a descendant, so
    // reaching this with one means a caller went around the UI, and the honest answer is nothing.
    if (!parentId.empty() && componentIsAncestorOf(id, parentId)) return;
    for (auto& c : graph_.components) {
        if (c.id != id) continue;
        pushUndo();
        fmt::setComponentAttr(c, "parent", parentId);
        dirty_ = true;
        previewFramed_ = false;
        return;
    }
}

void GraphEditor::setComponentAttribute(const std::string& id, const std::string& key,
                                         const std::string& value) {
    // Same no-quoting guard the NODE attribute editor uses, and for the identical reason -- see
    // setAttribute's comment. A COMP line splits on whitespace exactly as a NODE line does, so a
    // value with a space in it truncates silently and leaves junk tokens behind.
    if (containsWhitespace(value)) return;
    for (auto& c : graph_.components) {
        if (c.id != id) continue;
        pushUndo();
        fmt::setComponentAttr(c, key, value);
        dirty_ = true;
        if (key == "pos" || key == "rot" || key == "scale") previewFramed_ = false;
        return;
    }
}

void GraphEditor::renameComponent(const std::string& id, const std::string& newId) {
    if (newId.empty() || newId == id || containsWhitespace(newId)) return;
    for (const auto& c : graph_.components) if (c.id == newId) return;   // taken

    bool found = false;
    for (const auto& c : graph_.components) if (c.id == id) { found = true; break; }
    if (!found) return;

    pushUndo();
    for (auto& c : graph_.components) {
        if (c.id == id) c.id = newId;
        // EVERY CHILD FOLLOWS. A rename that left children pointing at the old id would produce a
        // file the parser refuses -- the same unopenable-file failure deleting a subtree avoids.
        if (fmt::componentAttr(c, "parent") == id) fmt::setComponentAttr(c, "parent", newId);
    }
    if (selectedComponent_ == id) selectedComponent_ = newId;
    dirty_ = true;
}

#if AVER_WITH_IMGUI

// The Viewport tab for a material graph: the sphere, and what the graph currently compiles to.
// DELIBERATELY NOT A SECOND EDITING SURFACE: everything an author changes about a material graph is
// changed on the Event Graph tab, in the nodes; this tab exists to answer one question -- what does
// it look like -- and the only other thing it says is why, when the answer is "nothing".
void GraphEditor::drawMaterialViewport(Engine& e, float dpi) {
    buildComponentPreview(e);

    render::preview::ActorPreview* preview = sharedPreview(e);
#if AVER_MODULE_PBR
    // The compile error, if there is one, ABOVE the picture rather than instead of it: a graph that
    // stopped compiling mid-edit still shows the last surface that worked (the id is kept), and an
    // author needs to see both -- the message says what to fix, the sphere says what they had.
    if (materialPreviewGraphId_ == 0) {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f),
                            "This graph does not compile yet -- see the log for which node.");
    } else {
        ImGui::TextDisabled("Material graph %u -- the same compiled surface a placed .ocmat gets.",
                            materialPreviewGraphId_);
    }
#else
    ImGui::TextDisabled("Built without the PBR module, so there is no material to preview.");
#endif

    if (!preview || !preview->uiTextureId()) {
        ImGui::TextDisabled(preview ? "The preview has not rendered a frame yet."
                                     : "This backend has no GPU preview.");
        return;
    }
    const ImVec2 region = ImGui::GetContentRegionAvail();
    const f32 texW = static_cast<f32>(preview->width());
    const f32 texH = static_cast<f32>(preview->height());
    const f32 fit = std::min(region.x / std::max(texW, 1.0f), region.y / std::max(texH, 1.0f));
    const f32 w = std::max(texW * fit, 16.0f * dpi);
    const f32 h = std::max(texH * fit, 16.0f * dpi);
    ImGui::Image(static_cast<ImTextureID>(preview->uiTextureId()), ImVec2(w, h));

    // THE SPHERE TURNS NOW. This viewport drew the image and stopped, so the preview sat at whatever
    // frameAll() picked once and never moved again -- and a material is exactly the thing you judge
    // by moving it, because roughness, anisotropy and a clear coat only declare themselves as the
    // highlight travels. Every sibling preview in this file and in AssetEditor already wires this
    // same block; this was the one place it was missing.
    //
    // SAFE FROM THE onUpdate STALENESS TRAP: an asset editor's draw runs inside the UI pass, which
    // is after ImGui::NewFrame, so io.MouseWheel here holds this frame's real value -- unlike the
    // same read from onUpdate, which is always zero. See the fly-camera's own note on that.
    if (ImGui::IsItemHovered()) {
        ImGuiIO& io = ImGui::GetIO();
        if (io.MouseWheel != 0.0f)
            preview->camera().addZoom(io.MouseWheel > 0.0f ? 0.88f : 1.0f / 0.88f);
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            preview->camera().addOrbit(-d.x * 0.4f, d.y * 0.4f);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
            const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
            preview->camera().panPixels(d.x, d.y, static_cast<f32>(preview->height()));
        }
    }
}

void GraphEditor::drawViewport(Engine& e, float dpi) {
    // A MATERIAL GRAPH HAS NO COMPONENTS, so it gets neither the component toolbar nor the tree --
    // both would be furniture for something this file cannot contain, and the toolbar's warning ("no
    // CLASS record, so nothing spawns them") is actively misleading here: a material graph isn't
    // supposed to have a CLASS record. The preview fills the tab instead.
    const bool material = openGraphDomain() == kDomainMaterial;
    if (material) {
        drawMaterialViewport(e, dpi);
        return;
    }
    drawComponentToolbar(dpi);

    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float panelW = std::clamp(300.0f * dpi, 200.0f * dpi, std::max(avail.x * 0.5f, 140.0f * dpi));
    const float previewW = std::max(avail.x - panelW - ImGui::GetStyle().ItemSpacing.x, 40.0f * dpi);
    const float rowH = std::max(avail.y, 120.0f * dpi);

    ImGui::BeginChild("##compPanel", ImVec2(panelW, rowH), true);
    drawComponentTree(dpi);
    ImGui::Separator();
    drawComponentDetails(dpi);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##compPreview", ImVec2(previewW, rowH), true);
    buildComponentPreview(e);

    render::preview::ActorPreview* preview = sharedPreview(e);
    if (!preview || !preview->uiTextureId()) {
        // NOT AN ERROR, and it says which of the two reasons it is. A headless or software backend
        // has no preview feature at all; a fresh one has no texture until its first pass runs.
        ImGui::TextDisabled(preview ? "The preview has not rendered a frame yet."
                                     : "This backend has no GPU preview -- the tree above still edits the file.");
    } else {
        const ImVec2 region = ImGui::GetContentRegionAvail();
        const f32 texW = static_cast<f32>(preview->width());
        const f32 texH = static_cast<f32>(preview->height());
        // Fit, never stretch: the preview target's aspect is its own, and letting ImGui scale it to
        // the panel would make a wide panel report a shape the actor does not have.
        const f32 fit = std::min(region.x / std::max(texW, 1.0f), region.y / std::max(texH, 1.0f));
        const ImVec2 size(std::max(texW * fit, 16.0f), std::max(texH * fit, 16.0f));
        ImGui::Image(static_cast<ImTextureID>(preview->uiTextureId()), size);

        if (ImGui::IsItemHovered()) {
            ImGuiIO& io = ImGui::GetIO();
            if (io.MouseWheel != 0.0f) preview->camera().addZoom(io.MouseWheel > 0.0f ? 0.88f : 1.0f / 0.88f);
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
                const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                preview->camera().addOrbit(-d.x * 0.4f, d.y * 0.4f);
            }
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle)) {
                const ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
                preview->camera().panPixels(d.x, d.y, static_cast<f32>(preview->height()));
            }
        }
    }
    ImGui::EndChild();
}

void GraphEditor::drawComponentToolbar(float dpi) {
    if (ImGui::Button("Add Component")) ImGui::OpenPopup("##addComponent");
    if (ImGui::BeginPopup("##addComponent")) {
        for (const auto& k : kGraphComponentKinds) {
            if (ImGui::MenuItem(k.name)) addComponent(k.name);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", k.blurb);
        }
        ImGui::EndPopup();
    }

    ImGui::SameLine();
    ImGui::BeginDisabled(selectedComponent_.empty());
    if (ImGui::Button("Delete")) deleteComponentSubtree(selectedComponent_);
    ImGui::EndDisabled();
    if (!selectedComponent_.empty() && ImGui::IsItemHovered())
        ImGui::SetTooltip("Deletes '%s' and everything under it.", selectedComponent_.c_str());

    ImGui::SameLine();
    if (ImGui::Button("Frame All")) previewFramed_ = false;

    ImGui::SameLine();
    // THE CLASS RECORD IS THE POINT OF THIS TAB, stated here rather than left for someone to discover
    // when their carefully built tree spawns nothing. C++ doesn't model CLASS (it rides through as an
    // unknown record), so this asks the raw text -- the same question GameApp's ocgraphDeclaresClass asks.
    const bool declaresClass = originalText_.rfind("CLASS ", 0) == 0 ||
                               originalText_.find("\nCLASS ") != std::string::npos;
    if (declaresClass) {
        ImGui::TextDisabled("%zu component(s)", graph_.components.size());
    } else {
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.35f, 1.0f),
                            "%zu component(s) -- but this graph has no CLASS record, so nothing spawns them",
                            graph_.components.size());
    }
    (void)dpi;
}

void GraphEditor::drawComponentTree(float dpi) {
    ImGui::TextDisabled("Components");
    if (graph_.components.empty()) {
        ImGui::TextWrapped("No components. Add one to give this class a body: a Mesh to draw, a Scene "
                            "node to hang things off, a Camera to look through.");
        return;
    }

    // The actor's own entity is the root every parentless component hangs off. Drawn as a real row
    // rather than implied, because "attached to the actor itself" is a choice an author makes and a
    // tree with no visible root makes that choice look like an accident.
    const bool rootOpen = ImGui::TreeNodeEx("##compRoot", ImGuiTreeNodeFlags_DefaultOpen |
                                             ImGuiTreeNodeFlags_SpanAvailWidth, "Actor (self)");
    if (ImGui::IsItemClicked()) selectedComponent_.clear();
    if (rootOpen) {
        for (const auto& c : graph_.components)
            if (fmt::componentAttr(c, "parent").empty()) drawComponentTreeNode(c.id, dpi);
        ImGui::TreePop();
    }
}

void GraphEditor::drawComponentTreeNode(const std::string& id, float dpi) {
    const fmt::OcGraphComponent* c = nullptr;
    for (const auto& o : graph_.components) if (o.id == id) { c = &o; break; }
    if (!c) return;

    bool hasChild = false;
    for (const auto& o : graph_.components)
        if (fmt::componentAttr(o, "parent") == id) { hasChild = true; break; }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_DefaultOpen |
                                ImGuiTreeNodeFlags_SpanAvailWidth;
    if (!hasChild) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selectedComponent_ == id) flags |= ImGuiTreeNodeFlags_Selected;

    const bool open = ImGui::TreeNodeEx(id.c_str(), flags, "%s", id.c_str());
    if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selectedComponent_ = id;
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", c->kind.c_str());

    if (open && hasChild) {
        for (const auto& o : graph_.components)
            if (fmt::componentAttr(o, "parent") == id) drawComponentTreeNode(o.id, dpi);
        ImGui::TreePop();
    }
}

void GraphEditor::drawComponentDetails(float dpi) {
    if (selectedComponent_.empty()) {
        ImGui::TextDisabled("Select a component to edit it.");
        return;
    }
    const fmt::OcGraphComponent* sel = nullptr;
    for (const auto& o : graph_.components) if (o.id == selectedComponent_) { sel = &o; break; }
    if (!sel) { selectedComponent_.clear(); return; }
    const fmt::OcGraphComponent& c = *sel;

    ImGui::PushItemWidth(150.0f * dpi);

    // Same activate / apply-on-enter / discard-on-deactivate shape the node attribute rows use, keyed
    // by field and id so switching selection never bleeds one row's in-flight text into another.
    {
        const std::string rowKey = "compid\x1f" + c.id;
        char buf[256];
        std::snprintf(buf, sizeof buf, "%s", (compEditRowKey_ == rowKey) ? compEditBuf_ : c.id.c_str());
        if (ImGui::InputText("Name", buf, sizeof buf)) {
            compEditRowKey_ = rowKey;
            std::snprintf(compEditBuf_, sizeof compEditBuf_, "%s", buf);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) {
            renameComponent(c.id, compEditBuf_);
            compEditRowKey_.clear();
        } else if (ImGui::IsItemDeactivated()) {
            compEditRowKey_.clear();
        }
    }

    if (ImGui::BeginCombo("Kind", c.kind.c_str())) {
        for (const auto& k : kGraphComponentKinds) {
            const bool selected = c.kind == k.name;
            if (ImGui::Selectable(k.name, selected) && !selected) {
                // Changing a kind KEEPS the old kind's attributes rather than clearing them. They
                // stay in the file, this panel stops showing them, and switching back brings them
                // straight back -- which beats destroying a mesh path because someone clicked the
                // wrong row of a combo.
                for (auto& target : graph_.components) {
                    if (target.id != c.id) continue;
                    pushUndo();
                    target.kind = k.name;
                    dirty_ = true;
                    break;
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", k.blurb);
        }
        ImGui::EndCombo();
    }

    {
        const std::string_view parent = fmt::componentAttr(c, "parent");
        const std::string label = parent.empty() ? "Actor (self)" : std::string(parent);
        if (ImGui::BeginCombo("Attach to", label.c_str())) {
            if (ImGui::Selectable("Actor (self)", parent.empty())) setComponentParent(c.id, "");
            for (const auto& o : graph_.components) {
                // A DESCENDANT IS NOT OFFERED, which is how a cycle is prevented rather than
                // detected: the parser refuses a cycle on load, so an editor that let one be made
                // would author a file it cannot reopen.
                if (componentIsAncestorOf(c.id, o.id)) continue;
                if (ImGui::Selectable(o.id.c_str(), parent == o.id)) setComponentParent(c.id, o.id);
            }
            ImGui::EndCombo();
        }
    }

    ImGui::Separator();

    // Written back only on release (IsItemDeactivatedAfterEdit), NOT every frame of the drag: each
    // write pushes an undo entry, so a live write would fill the undo stack with one entry per frame
    // and make Ctrl-Z useless for exactly the edit most likely to need it.
    f32 pos[3], rot[3], scale[3];
    compReadTransform(c, pos, rot, scale);
    const struct { const char* label; f32* v; const char* fmtStr; const char* key; } rows[] = {
        {"Position", pos, "%.1f", "pos"},
        {"Rotation", rot, "%.1f", "rot"},
        {"Scale", scale, "%.3f", "scale"},
    };
    ImGui::PushItemWidth(190.0f * dpi);
    for (const auto& row : rows) {
        ImGui::DragFloat3(row.label, row.v, 0.5f, 0.0f, 0.0f, row.fmtStr);
        if (ImGui::IsItemDeactivatedAfterEdit()) setComponentAttribute(c.id, row.key, compVec3(row.v));
    }
    ImGui::PopItemWidth();
    ImGui::TextDisabled("cm  |  degrees, yaw/pitch/roll  |  multiplier");

    if (const GraphComponentAttrSet* set = attrSetFor(c.kind)) {
        ImGui::Separator();
        for (int i = 0; i < set->count; ++i) {
            const GraphComponentAttrRow& row = set->rows[i];
            const std::string rowKey = std::string("compattr\x1f") + c.id + "\x1f" + row.key;
            const std::string_view current = fmt::componentAttr(c, row.key);
            char buf[256];
            std::snprintf(buf, sizeof buf, "%.*s",
                          (compEditRowKey_ == rowKey) ? 0 : static_cast<int>(current.size()),
                          current.data());
            if (compEditRowKey_ == rowKey) std::snprintf(buf, sizeof buf, "%s", compEditBuf_);

            ImGui::PushID(row.key);
            if (ImGui::InputTextWithHint(row.label, row.hint, buf, sizeof buf)) {
                compEditRowKey_ = rowKey;
                std::snprintf(compEditBuf_, sizeof compEditBuf_, "%s", buf);
            }
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                setComponentAttribute(c.id, row.key, compEditBuf_);
                compEditRowKey_.clear();
            } else if (ImGui::IsItemDeactivated()) {
                compEditRowKey_.clear();
            }
            ImGui::PopID();
        }
    }

    ImGui::PopItemWidth();
}

// The seed shell for a Fluid component, uploaded once per distinct size and cached.
//
// Shows the SEED SHELL, not the simulated surface: the solver only advances inside aver_phys_step,
// gated on Play (docs/GAME-LIFT.md's tested invariant: no play session -> step count 0), so it never
// runs in edit mode. generateFluidSeedShell is pure arithmetic over the desc -- no solver, no device,
// no gate -- so it shows exactly what the component is choosing without misrepresenting motion it
// isn't simulating.
//
// Drawn through the component matrix, unlike the runtime path (which draws at IDENTITY): FluidScene's
// buffer holds ABSOLUTE WORLD positions from the solver, while a seed shell spans -halfExtent..
// +halfExtent about the LOCAL origin. The desc below passes centre zero so the component matrix is
// the only place the position gets applied.
bool GraphEditor::buildFluidPreviewMesh(Engine& e, const fmt::OcGraphComponent& c,
                                        render::preview::PreviewDraw& out) {
    fluids::FluidVolumeDesc fd;
    fd.centreCm[0] = fd.centreCm[1] = fd.centreCm[2] = 0.0f;
    // The same cm-per-unit ApplyFluid uses (GraphComponentTree.cs), so what the preview shows and
    // what the game spawns are one number, not two that can drift.
    f32 pos[3], rot[3], scale[3];
    compReadTransform(c, pos, rot, scale);
    fd.halfExtentCm[0] = 100.0f * scale[0];
    fd.halfExtentCm[1] = 100.0f * scale[1];
    fd.halfExtentCm[2] = 50.0f  * scale[2];

    char key[96];
    std::snprintf(key, sizeof key, "$fluid/%.2f/%.2f/%.2f/%d/%d/%d",
                  static_cast<double>(fd.halfExtentCm[0]), static_cast<double>(fd.halfExtentCm[1]),
                  static_cast<double>(fd.halfExtentCm[2]),
                  fd.subdivisions[0], fd.subdivisions[1], fd.subdivisions[2]);
    const std::string k = key;

    render::preview::PreviewMeshCache& meshes = sharedPreviewMeshes();
    // The shell is built INSIDE the cache's callback, so a size already uploaded never generates one
    // again -- which is what keeps this off the per-frame cost of rebuilding the draw list.
    out.mesh = meshes.generated(*e.device(), k,
        [&fd](std::vector<rhi::MeshVertex>& verts, std::vector<u32>& indices) {
            fluids::FluidVolume vol(fd);
            vol.generateSeedShell();
            const std::vector<f32>& vp = vol.positionsCm();
            const std::vector<f32>& nrm = vol.normals();
            const std::vector<i32>& idx = vol.indices();
            verts.resize(vp.size() / 3);
            for (usize i = 0; i < verts.size(); ++i) {
                verts[i].px = vp[i * 3 + 0]; verts[i].py = vp[i * 3 + 1]; verts[i].pz = vp[i * 3 + 2];
                verts[i].nx = nrm[i * 3 + 0]; verts[i].ny = nrm[i * 3 + 1]; verts[i].nz = nrm[i * 3 + 2];
                verts[i].u = 0.0f; verts[i].v = 0.0f;
            }
            indices.assign(idx.begin(), idx.end());
        }, &out.boundsRadius);
    if (out.mesh == 0) return false;
    // The same still-water blue the transparent pass settles on, so the preview and the level read
    // as the same substance even though one of them is a static shell.
    out.baseColor[0] = 0.10f; out.baseColor[1] = 0.30f; out.baseColor[2] = 0.36f; out.baseColor[3] = 1.0f;
    out.roughness = 0.12f;   // the value the fluid's own transparent pass shades with
    return true;
}

// The Viewport tab for a MATERIAL graph: one sphere, shaded by this very graph.
//
// A sphere, not the component tree: a material graph has no entities, only a surface to see, and a
// sphere shows every normal-to-view angle at once (Fresnel, roughness, normal maps), where a cube
// shows exactly six.
//
// Compiled through the SAME registry the renderer uses, keyed on this file's path -- the preview is
// literally the renderer's own generated function, not a separately-compiled approximation that could
// drift. Recompiled only when the dirty flag AND the emitted text actually differ (an idle editor
// asks nothing of the shader compiler), and a graph that fails to compile mid-edit keeps the last good
// id -- the sphere shows the last thing that worked rather than going black between valid states.
bool GraphEditor::buildMaterialPreview(Engine& e, render::preview::PreviewDraw& out) {
#if AVER_MODULE_PBR
    if (path_.empty()) return false;

    // Re-register only when this editor has actually been edited since last time; the registry then
    // decides if anything changed, so a save-less edit that emits identical text costs one graph
    // compile and no shader compile.
    //
    // Undo-stack depth is used as the edit counter because every edit path calls pushUndo() first,
    // so it moves exactly when the graph does. A plain dirty_ flag would not: it latches true on the
    // first edit and never resets, so the preview would recompile once and then never again.
    const i64 mark = static_cast<i64>(history_.undoCount()) - static_cast<i64>(history_.redoCount());
    if (materialPreviewDirtyMark_ != mark) {
        materialPreviewDirtyMark_ = mark;
        const u32 id = pbr::materialGraphs().add(path_, graph_.name, graph_);
        if (id != 0) materialPreviewGraphId_ = id;
    }
    if (materialPreviewGraphId_ == 0) return false;

    render::preview::PreviewMeshCache& meshes = sharedPreviewMeshes();
    out.mesh = meshes.resolve(*e.device(), "Meshes/sphere.ocmesh", &out.boundsRadius);
    if (out.mesh == 0) return false;

    // The unit sphere is radius 1; 60 makes it the size of the cubes the level editor places, which
    // is the scale the orbit camera's own framing was tuned against.
    for (int i = 0; i < 16; ++i) out.world[i] = 0.0f;
    out.world[0] = out.world[5] = out.world[10] = 60.0f;
    out.world[15] = 1.0f;

    // White, because the GRAPH decides the colour. A tint here would multiply into everything the
    // author sees (averBuildSurface applies gBaseColor) and quietly misreport their own values.
    out.baseColor[0] = out.baseColor[1] = out.baseColor[2] = out.baseColor[3] = 1.0f;
    out.materialGraphId = materialPreviewGraphId_;
    return true;
#else
    (void)e; (void)out;
    return false;
#endif
}

void GraphEditor::buildComponentPreview(Engine& e) {
    render::preview::ActorPreview* preview = sharedPreview(e);
    if (!preview || !e.device()) return;

    // A MATERIAL GRAPH TAKES A DIFFERENT PREVIEW ENTIRELY -- see buildMaterialPreview. It shares
    // this feature and this tab, and nothing else with the component tree below.
    if (openGraphDomain() == kDomainMaterial) {
        std::vector<render::preview::PreviewDraw> draws;
        render::preview::PreviewDraw d;
        const bool haveSphere = buildMaterialPreview(e, d);
        if (haveSphere) draws.push_back(d);
        preview->setDrawList(std::move(draws));
        // FRAMED ONLY ONCE THERE IS SOMETHING TO FRAME. frameAll() fits the camera to the CURRENT
        // draw list, empty on the first frames since the tab opens before the graph is registered and
        // its sphere resolved. Latching `previewFramed_` on an empty list burns the one automatic
        // framing on nothing, leaving the camera staring at empty space for the rest of the session --
        // reading as "the preview is broken" rather than "not yet aimed".
        if (!previewFramed_ && haveSphere) {
            preview->frameAll();
            previewFramed_ = true;
        }
        return;
    }

    render::preview::PreviewMeshCache& meshes = sharedPreviewMeshes();
    meshes.setContentRoot(*e.device(), actorEditorContentRoot());

    std::vector<render::preview::PreviewDraw> draws;
    draws.reserve(graph_.components.size());
    for (const auto& c : graph_.components) {
        render::preview::PreviewDraw d;

        // A FLUID HAS NO MESH TO NAME, so it cannot come through the path below: its geometry is
        // generated arithmetic (fluids::generateFluidSeedShell) rather than a file. It is the one
        // component kind whose shape the editor has to build itself.
        if (detail::ciEquals(c.kind, "Fluid")) {
            if (!buildFluidPreviewMesh(e, c, d)) continue;
        } else {
            const std::string_view meshPath = fmt::componentAttr(c, "mesh");
            // ONLY WHAT HAS GEOMETRY. A Scene node, Camera or Animator has nothing to draw, and
            // inventing a placeholder box would make the preview disagree with the game -- the one
            // thing a preview must never do. Their transforms are still real, just invisible here too.
            if (meshPath.empty()) continue;
            d.mesh = meshes.resolve(*e.device(), meshPath, &d.boundsRadius);
            if (d.mesh == 0) continue;   // missing or unloadable; meshes.missing() already records it
        }
        componentWorldMatrix(c.id, d.world);
        d.selected = (c.id == selectedComponent_);
        draws.push_back(d);
    }

    preview->setDrawList(std::move(draws));
    if (!previewFramed_) {
        preview->frameAll();
        previewFramed_ = true;
    }
}

#else   // !AVER_WITH_IMGUI

void GraphEditor::drawViewport(Engine&, float) {}
void GraphEditor::drawMaterialViewport(Engine&, float) {}
void GraphEditor::drawComponentToolbar(float) {}
void GraphEditor::drawComponentTree(float) {}
void GraphEditor::drawComponentTreeNode(const std::string&, float) {}
void GraphEditor::drawComponentDetails(float) {}
void GraphEditor::buildComponentPreview(Engine&) {}
bool GraphEditor::buildMaterialPreview(Engine&, render::preview::PreviewDraw&) { return false; }

#endif  // AVER_WITH_IMGUI

std::string graphStarterText(const std::string& stem) {
    // A raw string literal, so the file is legible as the lines it becomes, and to sidestep a trap
    // this repo has hit twice: a backslash-n eaten by tooling lands as a real newline inside a narrow
    // literal, which MSVC answers with a wall of C2001 "newline in string literal".
    //
    // The class name derives from the stem (NewGraph.ocgraph -> AN_NewGraph) since it must be a legal
    // identifier and the file name need not be. Renaming the file does NOT rename the class -- a level
    // placement names the CLASS, changed from the Details panel instead.
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
} // namespace

void setGraphValidator(GraphEditor::ValidateFn fn) { g_graphValidator = std::move(fn); }
void setGraphNodeHitSource(GraphEditor::NodeHitsFn fn) { g_graphNodeHits = std::move(fn); }

std::unique_ptr<AssetEditor> makeGraphEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (ext != ".ocgraph") return nullptr;
    auto ed = std::make_unique<GraphEditor>(path);
    if (g_graphValidator) ed->setValidator(g_graphValidator);
    if (g_graphNodeHits)  ed->setNodeHitSource(g_graphNodeHits);
    return ed;
}

} // namespace aver::editor
