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
#include "GraphEditor.hpp"
#include "GraphNodeDefs.hpp"
#if AVER_WITH_IMGUI
// The Viewport tab only. Behind the guard because tests/editor compiles THIS FILE with no ImGui,
// no preview module and a three-library link -- see tests/editor/CMakeLists.txt, which says so.
#  include "ActorEditor.hpp"   // sharedPreview / sharedPreviewMeshes / actorEditorContentRoot
#endif

#include "aver/core/Log.hpp"
#if AVER_WITH_IMGUI
#  include "aver/runtime/Engine.hpp"
#  include "aver/render/preview/ActorPreview.hpp"
#  include "aver/render/preview/PreviewMeshCache.hpp"
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

// Whether `type` is one of the three concrete variable types the C# side's PinType enum accepts for a
// VAR record (Enum.TryParse<PinType> is case-insensitive, so this is too) -- Exec is data a graph
// COMPUTES WITH, never data it REMEMBERS between ticks (see OcGraphParser.cs's own "VAR ... cannot be
// declared exec" rejection), so it is never offered by addVariable/retypeVariable's fallback, or by
// the Variables panel's type Combo (draw(), below), even though nothing at the OcGraphVariable/
// OcGraphData layer would stop a hand-edited file from carrying one -- Graph.Validate() is what
// actually enforces this at C# compile time; this is only the editor keeping its own authoring
// surface honest about what it will produce.
bool isValidVarType(const std::string& type) {
    std::string t = type;
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t == "float" || t == "int" || t == "bool";
}

// Same "this format has no quoting" guard setAttribute's own value check uses (OcGraph.cpp's
// splitWhitespace / OcGraphParser.cs's own SplitWhitespace both tokenise on bare whitespace with no
// escaping) -- shared here so a VAR name/type/default gets the identical protection a NODE-line
// attribute value already has, rather than a second, easier-to-drift-from copy of the same six
// characters.
bool containsWhitespace(const std::string& s) {
    return s.find_first_of(" \t\r\n\v\f") != std::string::npos;
}

// The name a node shows on its header.
//
// For all 150 catalog types but two this is the catalog's own displayName. The exception is the
// pair that READ AND WRITE A VARIABLE. A GetVar node used to draw the words "Get Var", so a wall of
// them was unreadable -- which variable each one touched was discoverable ONLY by selecting it and
// reading the properties panel. Unreal puts the variable's NAME on the node instead, and that is
// what this returns.
//
// HEADLESS, and above the ImGui guard on purpose: the LAYOUT pass sizes a node from its title with
// no ImGui headers in the translation unit, and a node as wide as "Get Var" would clip a variable
// called "PlayerSpeed". nodeHeaderColor() below is the ImGui-typed other half.
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

// Pin/link colour by declared type. Falls back to a neutral grey for anything unrecognised -- a type
// this editor has never seen must still draw, not vanish or assert.
//
// "exec" is WHITE, not merely another entry in this list. Blueprint-style editors converged on this
// convention independently for a reason: it is the one colour no data type here (or plausibly ever
// added later) also uses, so a white wire reads as "control flow" at a glance without having to
// remember a legend. Colour ALONE would still leave exec and data pins the same DOT shape, though,
// which is why the actual pin-drawing loop below also changes the pin's geometry for exec (a diamond,
// not a circle) -- see that code's own comment for why shape, not just colour, is the point: a
// colour-blind reader (or a screenshot inspected in greyscale) loses colour information entirely,
// but a diamond next to a circle is still visibly two different things.
// The node HEADER colour, by palette category. Unlike the pin colours above -- which follow
// Blueprint so the pin language is portable -- these are the engine's own tokens, the same
// orange/steel pair the editor chrome and the website use.
//
// THE RULE IS WARM VERSUS COOL, not one hue per category, because a reader should be able to
// tell what a node DOES from across the canvas without learning seven colours. Warm (orange)
// means it reaches outside the graph: an event arriving, or the world being changed. Cool
// (steel) means pure computation that could be deleted without the world noticing. Grey is
// control flow, which is neither -- it decides ORDER and touches nothing.
//
// Unrecognised categories fall back to the cool default rather than asserting, for the same
// reason colorForType has a grey fallback: a category this editor has not seen must still draw.
// THE NODE CHROME IS WHERE THIS ENGINE LOOKS LIKE ITSELF, which is the counterpart to the note on
// colorForType below: pin colours are Unreal's because a pin colour is a learned language, and the
// body of the node is not, so this is the surface that carries Aver's palette.
//
// A FUNCTION CALL IS AVER ORANGE, not the blue Blueprint gives it. Blueprint marks a call with a
// blue header and an "f", and that blue is the single most common colour on a real graph -- so it is
// the one worth spending on identity. White title text over it, which every header already used.
//
// EVENTS MOVE TO RED, and they had to. They were Aver orange, and two node classes sharing one
// colour is worse than either choice on its own: an entry point and a call are the two things a
// reader most needs to tell apart at a glance. Red is also what Blueprint uses for an event, so
// this ends up MORE readable to someone arriving from there, not less.
//
// The split is by what the node DOES, not by which family it is filed under: calling into the
// engine is orange, computing a value is cool, ordering other nodes is neutral.
ImU32 headerColorForCategory(const std::string& category) {
    // A graph's OWN functions get the Aver orange the engine-call families use, because that is
    // exactly what a Call Function node is from the caller's side: a call. Blueprint makes the
    // same choice -- a user function call and an engine function call are drawn identically,
    // because the distinction does not matter at the call site.
    if (category == "Function") return IM_COL32(242, 101, 34, 255);
    std::string c = category;
    for (char& ch : c) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

    // Entry points: something outside the graph called US.
    if (c == "event")    return IM_COL32(155,  36,  36, 255);  // deep red, as Blueprint marks an event

    // Function calls: WE call something outside the graph. Aver orange.
    if (c == "scene" || c == "actor" || c == "material" || c == "mesh" || c == "name" ||
        c == "character" || c == "game" || c == "physics" || c == "transform" || c == "debug")
                         return IM_COL32(242, 101,  34, 255);  // Aver orange -- the "f" nodes
    if (c == "input")    return IM_COL32(190,  90,  45, 255);  // device in, between call and compute

    // Neutral: ordering only, nothing computed and nothing called.
    if (c == "flow")     return IM_COL32( 74,  82,  96, 255);  // slate

    // Cool: pure computation. Vector and Convert sit here rather than with the calls above because
    // they are arithmetic -- the same place Blueprint puts a pure function, just not the same hue.
    if (c == "var")      return IM_COL32( 78, 104, 168, 255);  // steel, darkened -- storage
    return IM_COL32( 91, 141, 239, 255);                        // Aver steel -- math, vector, convert, logic, const
}

// The one highlight colour: selected node border, selected link, selected comment border, and the
// ring on a pin that a link ends at. It was this literal written out at each of those four sites.
// Amber rather than Unreal's white so it cannot be confused with an exec wire, and dimmer than the
// (255,220,90) it replaces for the same reason the pin palette came down.
constexpr ImU32 kSelectionCol = IM_COL32(226, 188, 96, 255);

ImU32 colorForType(const std::string& type) {
    std::string t = type;
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // THESE ARE UNREAL'S PIN COLOURS ON PURPOSE, and it is the one place in this editor that
    // deliberately does not use the engine's own palette. Pin colour is a LEARNED LANGUAGE, not
    // a branding surface: someone who has used Blueprints reads green-as-float and red-as-bool
    // without looking at a legend, and spending that recognition to be visually distinctive
    // would cost every one of those readers something and buy nothing back. The node chrome
    // around them (headerColorForCategory below) is where this engine looks like itself.
    //
    // TONED DOWN FROM UNREAL'S LITERAL VALUES, which are near-fullbright: float was (91,255,15) and
    // string (255,0,168). At Blueprint's wire thickness on Blueprint's background that reads fine;
    // here, a graph of any size turned into a field of glare that pulled the eye away from the
    // nodes -- the thing you are actually reading. These sit at roughly 75-80% value and noticeably
    // lower saturation while keeping the HUE each type is recognised by, which is the part that
    // carries the meaning. Someone arriving from Blueprint still reads green-as-float at a glance.
    if (t == "float")  return IM_COL32(126, 199,  76, 255);  // yellow-green
    if (t == "int")    return IM_COL32( 72, 181, 152, 255);  // turquoise
    if (t == "bool")   return IM_COL32(176,  72,  72, 255);  // red
    if (t == "string") return IM_COL32(186,  92, 152, 255);  // magenta
    if (t == "exec")   return IM_COL32(206, 210, 216, 255);  // white -- see above
    return IM_COL32(150, 154, 160, 255);
}

// The colour a node's header is drawn in: its category's, except for the two variable nodes, which
// take the colour of their VARIABLE'S TYPE -- the same colour language the pins already speak. See
// nodeTitle() above for the other half and for why they are split.
//
// THE TYPE COMES FROM THE VARIABLE, NOT FROM THE NODE'S OWN PIN, and that is load-bearing.
// retypeVariable() deliberately does not rewrite the pins of nodes already placed ("DELIBERATELY
// DOES NOT TOUCH ANY NODE'S PINS"), so a node's value pin can outlive a retype and still say
// "float" when the variable is now an int. Reading the declaration keeps the header honest.
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

// Whether a pin's declared type is "exec" -- the ONE place this string comparison lives, so the
// diamond-vs-circle drawing choice below and any future exec-specific drawing logic share a single
// definition of "is this pin control flow" rather than each re-deriving it. Case-insensitive for the
// same reason colorForType is: nothing in the format requires a specific case for a pin type string.
bool isExecPinType(const std::string& type) {
    std::string t = type;
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t == "exec";
}
#endif

} // namespace

void setGraphEditorDpi(float dpi) { g_graphEditorDpi = dpi; }

// ================================================================================== construction ===

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

    // Before layout: a node's box height comes from its pin count, so filling the pins in afterwards
    // would space the graph for nodes smaller than the ones actually drawn.
    synthesizeMissingPins();
    // A FUNCTION NODE HAS NO CATALOG PINS TO SYNTHESISE, so the loop above cannot help it: a
    // FuncEntry/FuncReturn/CallFunc takes its shape from a FUNC declaration, which is data, not a
    // node type. Derived here for the same reason synthesizeMissingPins exists at all -- the file
    // records PIN lines only where a pin carries something extra, so without this a call node
    // loaded from disk has no pins, draws none, and every wire into it silently disappears. Which
    // is exactly what the first screenshot of this feature showed.
    for (const auto& f : graph_.functions) resyncFunctionNodePins(f.name);

    displayPos_.clear();
    for (const auto& n : graph_.nodes)
        displayPos_[n.id] = Vec2(static_cast<f32>(n.x), static_cast<f32>(n.y));
    runAutoLayoutIfUnpositioned();

    selectedNodes_.clear();
    selectedLink_ = -1;
    undoStack_.clear();
    redoStack_.clear();
    dragMode_ = DragMode::None;
    view_ = CanvasTransform{};
}

// Auto-layout for a graph with no meaningful stored positions. Triggers only when EVERY node sits at
// exactly (0,0) -- the shape a hand-written file has when nobody bothered with NODE's x/y, since the
// parser requires x on every NODE line (modules/formats/src/OcGraph.cpp:78-81) but a human just types
// "0 0" or omits y. A file with one deliberate node at the origin and others placed elsewhere is left
// alone. See GraphEditor.hpp's displayPos_ comment: this writes ONLY to the display overlay.
// Gives every node the pins its TYPE implies but the file did not write down. See the
// synthesizedPins_ comment in the header for why a valid .ocgraph can be missing them.
//
// Matched on name AND direction: a node may legitimately carry an input and an output sharing a
// name, and treating those as one would leave a real pin unsynthesised while thinking it was there.
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

void GraphEditor::runAutoLayoutIfUnpositioned() {
    if (graph_.nodes.empty()) return;
    bool allOrigin = true;
    for (const auto& n : graph_.nodes) {
        if (n.x != 0.0 || n.y != 0.0) { allOrigin = false; break; }
    }
    if (!allOrigin) return;
    // THE SAME SCALE recomputeLayouts() draws at, not 1.0. Node boxes are sized at DPI; spacing them
    // as if DPI were 1 packs the columns three times too tightly on a 300% display, so every node
    // lands on top of its neighbour and the titles clip. Layout and drawing have to agree on how big
    // a node is, and the only way to guarantee that is to hand both the same number.
    autoLayoutDpi_ = g_graphEditorDpi;
    autoLaidOut_ = true;
    const auto pos = autoLayoutPositions(graph_, style_, autoLayoutDpi_);
    for (const auto& kv : pos) displayPos_[kv.first] = kv.second;
}

std::string GraphEditor::title() const {
    return std::filesystem::path(path_).filename().string() + "  [Graph]";
}

// ===================================================================================== load/save ===

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

    std::error_code ec;
    const std::filesystem::path p(path_);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream f(path_, std::ios::binary | std::ios::trunc);
    if (!f) {
        if (why) *why = "could not open " + path_ + " for writing";
        return false;
    }
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!f) {
        if (why) *why = "write failed for " + path_;
        return false;
    }
    originalText_ = text; // the next save merges against what is now actually on disk
    dirty_ = false;
    return true;
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

// ======================================================================================= undo/redo =

void GraphEditor::pushUndo() {
    UndoState s;
    s.graph = graph_;
    s.displayPos = displayPos_;
    undoStack_.push_back(std::move(s));
    constexpr usize kUndoCap = 200;
    if (undoStack_.size() > kUndoCap) undoStack_.erase(undoStack_.begin());
    redoStack_.clear();
}

void GraphEditor::undo() {
    if (undoStack_.empty()) return;
    UndoState redoEntry{graph_, displayPos_};
    redoStack_.push_back(std::move(redoEntry));
    UndoState s = std::move(undoStack_.back());
    undoStack_.pop_back();
    graph_ = std::move(s.graph);
    displayPos_ = std::move(s.displayPos);
    dirty_ = true;
    selectedNodes_.clear();
    selectedLink_ = -1;
}

void GraphEditor::redo() {
    if (redoStack_.empty()) return;
    UndoState undoEntry{graph_, displayPos_};
    undoStack_.push_back(std::move(undoEntry));
    UndoState s = std::move(redoStack_.back());
    redoStack_.pop_back();
    graph_ = std::move(s.graph);
    displayPos_ = std::move(s.displayPos);
    dirty_ = true;
    selectedNodes_.clear();
    selectedLink_ = -1;
}

// ========================================================================================= edits ===

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

// THE NODE LINE AND THE ENTRY RECORD ARE TWO PLACES THAT SAY THE SAME THING, and that is not a
// design this editor chose -- it is what the format is. `ENTRY <nodeId> <eventName>` is the only
// thing that makes an event fire (CompileEntryPoint matches on it and nothing else), while the
// canvas needs something to draw and edit on the node itself, which is the `name=` attribute.
//
// Two places that must agree is a bug waiting to happen, so there is exactly ONE function that
// writes the ENTRY side and every path that touches the name goes through it. The failure it
// prevents is silent in the worst way: rename the attribute alone and the canvas shows the new
// name, the file saves, the graph loads, and the event that used to fire simply stops.
void GraphEditor::syncEventEntry(const std::string& nodeId, const std::string& eventName) {
    for (usize i = 0; i < graph_.entryPoints.size(); ++i) {
        if (graph_.entryPoints[i].first != nodeId) continue;
        if (eventName.empty()) graph_.entryPoints.erase(graph_.entryPoints.begin() + static_cast<isize>(i));
        else graph_.entryPoints[i].second = eventName;
        return;
    }
    if (!eventName.empty()) graph_.entryPoints.emplace_back(nodeId, eventName);
}

std::string GraphEditor::addNodeFromCatalog(const std::string& typeId, Vec2 canvasPos) {
    const GraphNodeDesc* desc = findGraphNodeDesc(typeId);
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

    // AN EVENT NODE WITHOUT AN `ENTRY` RECORD NEVER RUNS. The node type is only a label -- what
    // actually starts an exec chain is a top-level `ENTRY <nodeId> <eventName>` line, which
    // docs/AVER_NODE_NODES.md's Flow section says in as many words. Dropping an On Tick from the
    // palette used to produce the node and nothing else, so the graph looked complete, saved, ran,
    // and did nothing, with no error at any layer.
    //
    // The event name is the node TYPE, which is what every ENTRY record in this repo already says
    // -- `ENTRY tick OnTick`. Driven off the catalog's own "Event" category rather than a second
    // hand-maintained list of type names, for the reason GraphNodeDefs.hpp's header gives: there is
    // one place a node type is registered.
    if (desc->category == "Event") {
        // CustomEvent is the one event whose name is NOT its type -- that is the whole point of
        // it. A fresh one gets a unique generated name rather than an empty one, because an ENTRY
        // with no event name is a record the format cannot express, and a node that silently
        // fires nothing until someone finds the attribute row is the failure this palette just
        // stopped shipping for On Tick.
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

// ---------------------------------------------------------------- comment boxes

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
    // Margin on all four sides, and EXTRA on top for the title bar, which is drawn INSIDE the box.
    // Without it the bar would sit over the first row of nodes and hide their headers.
    //
    // BOTH SCALED BY DPI, because the node boxes they are measured against are. computeNodeLayout
    // sizes a node at the current DPI while its POSITION comes out of the file unscaled, so canvas
    // space is only half DPI-independent -- a margin left in raw units would be a third of its
    // intended width beside a 300%-DPI node. That asymmetry belongs to the layout system, not to
    // this gesture (autoLayoutDpi_ exists to cope with the same thing), and its consequence is
    // worth stating plainly: a box drawn snugly around six nodes at 300% is a loose box at 100%.
    // Still the right trade -- the alternative is a box that fails to enclose its own nodes on the
    // machine that drew it.
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
    // A newline would end the record early and turn the tail of the title into a line the parser
    // reads as a whole new record -- so it is folded to a space rather than refused. Refusing would
    // be the stricter choice and the wrong one here: the text arrives from a paste as often as from
    // a keystroke, and losing a pasted title is a worse outcome than flattening it.
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

// ---------------------------------------------------------------- functions

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
    // A selection in the subgraph you just left is a selection you can no longer see, and Delete
    // would still act on it. Cleared, and the view re-framed onto whatever the new subgraph holds --
    // which is the same "the view is the editor's until you touch it" rule the framing code follows,
    // applied to a change of what there is to look at.
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
    // EVERY REFERENCE MOVES IN THE SAME UNDO STEP -- both the func= that says where a node lives and
    // the call= that says what a call node calls. A rename that left either behind would produce a
    // file that parses and refuses to compile, which is the exact failure renameVariable was written
    // to avoid for var=.
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
        // MARKED SYNTHESISED, so save() strips them again. These pins are not information -- they
        // are a restatement of the FUNC/FUNCIN/FUNCOUT records three lines up, and BOTH readers
        // derive them the same way (this function and OcGraphParser.AddDefaultPins). Writing them
        // would put a second copy of the signature in the file, free to disagree with the first
        // after any edit, and would balloon a graph with one call node into a dozen PIN records the
        // author never wrote -- the same damage synthesizedPins_ was created to prevent.
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

// ---------------------------------------------------------------- the Functions panel

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
            // ---- name
            const std::string nameKey = "fnname\x1f" + fname;
            if (funcEditRowKey_ != nameKey) std::snprintf(funcEditBuf_, sizeof funcEditBuf_, "%s", fname.c_str());
            ImGui::SetNextItemWidth(150.0f * dpi);
            if (ImGui::InputText("Name", funcEditBuf_, sizeof funcEditBuf_)) funcEditRowKey_ = nameKey;
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                renameFunction(fname, funcEditBuf_);
                funcEditRowKey_.clear();
            }

            // ---- purity
            bool pure = graph_.functions[fi].pure;
            if (ImGui::Checkbox("Pure", &pure)) setFunctionPure(fname, pure);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pure: no exec pins, callable from a data wire.\n"
                                   "An impure function is the only kind that can RECURSE -- a pure one\n"
                                   "has no Branch to stop with, and Select evaluates both of its sides.");

            // ---- inputs and outputs
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

// ---------------------------------------------------------------- framing and layout

// Screen-space margin left around the content when framing. Enough that a node on the edge of the
// graph does not sit flush against the canvas border, where its pins would be unclickable.
namespace {
constexpr float kFramePaddingPx = 40.0f;

// THE ZOOM FLOOR IS A DPI-RELATIVE QUANTITY, and it was written as an absolute one. What a
// minimum zoom is FOR is "do not let the graph shrink past the point where it is a smear" --
// which is a statement about SCREEN pixels per logical pixel, i.e. about zoom * dpi, not about
// zoom. A flat floor of 0.15 therefore bites three times too early on a 300% display.
//
// MEASURED, not reasoned: a real 63-node graph (AN_FPCharacter.ocgraph) auto-lays out to a
// content span of 11907 x 5400 canvas units at dpi 3. Fitting that in an 1853 px canvas needs
// zoom 0.135. The old floor clamped it to 0.15, so the content overflowed the viewport and the
// button labelled "Frame All" did not frame all -- which is worse than having no button.
//
// ONLY THE FLOOR SCALES. The ceiling would be equally defensible in theory (zoom 4 at dpi 3 is
// a 12x magnification nobody needs), but nothing is broken up there, and dividing it would
// TAKE AWAY zoom range that works today on the display this engine is developed on. A fix that
// removes a working capability to satisfy a symmetry is not a fix.
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

void GraphEditor::deleteSelection() {
    // A selected comment box is deleted on its own, BEFORE the node/link work, and then this
    // returns. Not folded into the same undo step: a box and a node selection are never both
    // live at once (clicking either clears the other), so there is nothing to combine, and
    // deleteComment already pushes its own undo.
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

        // ENTRY AND OUT RECORDS NAMING A DELETED NODE GO TOO, for the identical reason the links
        // above do -- and this half was missing. Both are validated on load, so deleting the node
        // an `ENTRY tick OnTick` points at left a file that SAVED cleanly and then refused to open
        // with "ENTRY references non-existent node: tick". The editor produced a graph it could not
        // then read back, which is the worst failure an authoring surface has.
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
    const GraphLinkCheck check = canConnectPins(graph_, srcNode, srcPin, dstNode, dstPin);
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

bool GraphEditor::selectNode(const std::string& nodeId) {
    for (const auto& n : graph_.nodes) {
        if (n.id != nodeId) continue;
        selectedNodes_ = {nodeId};
        selectedLink_ = -1;
        return true;
    }
    // FALLS BACK TO A COMPONENT, because a graph now holds two kinds of selectable thing and the
    // caller naming one by id should not have to know which kind it is. Node ids and component ids
    // live in separate namespaces -- nothing stops a graph having a NODE `gun` and a COMP `gun` --
    // so nodes are tried FIRST and this only runs when no node answered.
    for (const auto& c : graph_.components) {
        if (c.id != nodeId) continue;
        selectedComponent_ = nodeId;
        return true;
    }
    return false;
}

bool GraphEditor::setAttribute(const std::string& nodeId, const std::string& key, const std::string& value) {
    // ADVERSARIAL-VERIFICATION FIX: the .ocgraph NODE line has no quoting on either side (OcGraph.cpp's
    // splitWhitespace, matched token-for-token by OcGraphParser.cs's own SplitWhitespace, whose doc
    // comment says "respecting no quoting"). Writing a value containing whitespace here would produce a
    // NODE line that reads back as MULTIPLE raw tokens: the value silently truncates at the first space
    // (both getNodeAttribute here and OcGraphParser.cs's NODE case take only the text up to the next
    // whitespace), and the remainder becomes permanently-invisible junk tokens -- no '=', so
    // computeAttributeRows never surfaces them again, and OcGraphParser.cs's own NODE loop just skips
    // any token without '=' outright. Net effect, reproduced empirically: a compiled graph would run
    // silently on a truncated field=/class=/param= value with no error at any layer. Refusing here (a
    // no-op, same contract as the nonexistent-node case below) stops the editor's own authoring surface
    // from ever writing something this format cannot represent -- it is the narrowest fix that does not
    // touch the shared reader/writer or add quoting/escaping to the file format itself.
    //
    // ADVERSARIAL-VERIFICATION FOLLOW-UP: the check above named OcGraph.cpp's splitWhitespace as its
    // authority but only tested 4 of the 6 characters that function's own isSpace() (TextScan.hpp)
    // treats as delimiters -- '\v' and '\f' were missing, so a value containing either would have
    // passed this guard and then hit the exact same silent-truncation bug the guard exists to prevent.
    // Widened to match isSpace()'s full set exactly, character for character. NOT covered here (a
    // genuine remaining gap, not chased further): OcGraphParser.cs's own SplitWhitespace splits on
    // char.IsWhiteSpace, which recognises Unicode whitespace (e.g. U+00A0 NBSP) that C++'s isSpace()
    // does not -- such a value would still round-trip correctly through THIS editor (C++ writer/reader
    // agree) but corrupt on the C# side. Closing that would mean picking one language's whitespace
    // definition as authoritative for both, or adding real quoting to the format -- a design decision,
    // not a small fix.
    if (containsWhitespace(value)) return false;
    for (auto& n : graph_.nodes) {
        if (n.id != nodeId) continue;
        pushUndo();
        setNodeAttribute(n, key, value); // GraphEditorGeometry.hpp -- the order-preserving read/write
        // A CustomEvent's `name=` is half of a pair -- see syncEventEntry, which owns the other
        // half. Renaming the attribute without the record leaves a graph that looks renamed and
        // has silently stopped firing.
        if (key == "name" && n.type == "CustomEvent") syncEventEntry(n.id, value);
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
    // The "Connection refused: " prefix used to live in draw()'s overlay-drawing code, baked onto
    // EVERY lastRejectMsg_ regardless of what produced it. Once deleteVariable (below) started using
    // the same banner for an unrelated refusal, that hard-coded prefix would have mislabelled it
    // ("Connection refused: cannot delete variable ...") -- moved here, onto the one call site that
    // actually means it, so the banner-drawing code can show lastRejectMsg_ verbatim.
    showRejectionBanner("Connection refused: " + (check.message.empty() ? std::string("link refused") : check.message));
}

// ==================================================================================== variables ===
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
    // THE PART A BARE `var->name = newName` WOULD BE MISSING, AND WHY THAT'S NOT AN ACCEPTABLE
    // "RENAME": every GetVar/SetVar (or any future var=-carrying node type) that names `oldName`
    // still names `oldName` after that one-line change -- Graph.Validate()'s "references undeclared
    // variable" check would then refuse the very next C# compile of a graph that, from the editor's
    // own Variables panel, LOOKS like it just successfully renamed a variable. Rewriting every
    // reference here, inside the SAME pushUndo() step as the rename itself, is what makes "rename"
    // actually mean rename: one edit, one undo entry, no node silently left pointing at a name that
    // no longer exists.
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
    // DELIBERATELY DOES NOT TOUCH ANY NODE'S PINS. A GetVar/SetVar spawned from the palette carries
    // its own explicit 'value' PIN record (the Add-Node handler below copies GraphNodeDesc::pins
    // verbatim onto the new node), typed to whatever the catalog's default was (float) at spawn time
    // -- retyping the VARIABLE after that does not retroactively retype an already-placed pin, so a
    // node spawned before this retype can end up with a 'value' pin type that disagrees with the
    // variable's new declared type. Silently rewriting that pin here was considered and rejected: a
    // pin's type is data the user can see and has possibly already wired a LINK against, and changing
    // it out from under them could turn one visible edit into a second, invisible mismatch on
    // whatever was connected to it. Left visible instead -- see the Variables panel's own mismatch
    // note in draw(), computed fresh every frame from the LIVE pins, so it never goes stale the way a
    // one-shot warning captured at retype time would.
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

// ======================================================================================== layout ===

void GraphEditor::recomputeLayouts(float dpi) {
    layouts_.clear();
    layouts_.reserve(graph_.nodes.size());
    for (const auto& n : graph_.nodes) {
        // ONLY THE SUBGRAPH ON SCREEN. layouts_ is what the canvas draws, what hit-testing tests,
        // what box-select selects and what framing measures -- so filtering here scopes all four
        // at once, and there is no second place that could disagree about which nodes are visible.
        if (subgraphOf(n) != currentSubgraph_) continue;
        fmt::OcGraphNode display = n; // layout reads position from the node it's given; substitute
                                       // the DISPLAY position so a live drag (which only touches
                                       // displayPos_, see below) affects drawing without touching
                                       // graph_ until the drag commits.
        auto it = displayPos_.find(n.id);
        if (it != displayPos_.end()) { display.x = it->second.x; display.y = it->second.y; }
        const GraphNodeDesc* desc = findGraphNodeDesc(n.type);
        // The SAME string the header will draw -- a variable node is as wide as its variable's name,
        // and computing the width from "Get Var" while drawing "PlayerSpeed" would clip it.
        const std::string title = nodeTitle(n, desc, graph_.variables);
        // `dpi` only -- NOT dpi*zoom. Canvas-space geometry is computed once at a fixed logical
        // scale (DPI only); CanvasTransform::zoom is applied uniformly afterwards, at the point
        // canvas coordinates are converted to screen coordinates (canvasToScreen) and back
        // (screenToCanvas, used to convert the mouse position before hit-testing). Folding zoom into
        // `scale` here AS WELL would double-apply it -- node boxes would grow twice as fast as the
        // pins drawn at their corners. See draw()'s canvasToScreen calls for the other half of this.
        layouts_.push_back(computeNodeLayout(display, title, style_, dpi));
    }
}

// ========================================================================================== draw ===

void GraphEditor::draw(Engine& e) {
#if AVER_WITH_IMGUI
    const float dpi = g_graphEditorDpi;
    // An auto-laid-out graph is spaced for one DPI. If that changed since -- the asset opened before
    // applyDpi ran, or the window moved to a differently-scaled monitor -- redo it, or the spacing
    // and the boxes disagree and the columns collide. Only auto-layout is redone: positions the user
    // dragged, or that came from the file, are theirs and are left exactly where they are.
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

    // ---- toolbar --------------------------------------------------------------------------------
    if (ImGui::Button("Save") || (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                                   ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))) {
        std::string why;
        if (!save(&why)) AVER_ERROR("[GraphEditor] save failed for '{}': {}", path_, why);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(undoStack_.empty());
    if (ImGui::Button("Undo")) undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(redoStack_.empty());
    if (ImGui::Button("Redo")) redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    // FRAME ALL AND AUTO-LAYOUT ARE BUTTONS, not only key bindings, and that is the point of them.
    // The graph that opens on empty grid is found by someone who does not yet know this editor;
    // a binding they have to be told about does not reach them. The shortcut is in the label.
    // ---- WHICH SUBGRAPH THE CANVAS IS SHOWING. A combo rather than a second tab bar, because the
    // number of functions is unbounded and the tab bar above already means something else (what the
    // actor DOES versus what it IS). Absent entirely until a graph declares its first function, so
    // nothing changes for the graphs that have none -- which is every graph written before now.
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
    // The pan hint lives HERE, on the status line the user is already reading for the zoom percentage,
    // deliberately instead of a tooltip: a tooltip only reaches someone already hovering the thing it
    // explains, which is exactly backwards for a gesture whose entire problem is that nobody knew to
    // reach for it. Lists every gesture that actually works today (this comment is not aspirational --
    // Right/Middle-drag and Space+drag are both wired in the block below).
    ImGui::TextDisabled("%s  |  %zu nodes, %zu links, %zu comments  |  zoom %.0f%%  |  pan: right- or middle-drag, or Space+drag  |  C: comment box",
                         path_.c_str(), graph_.nodes.size(), graph_.links.size(),
                         graph_.comments.size(), view_.zoom * 100.0f);

    // ---- the two tabs an actor gets: what it DOES, and what it IS ------------------------------
    // Blueprint's own split, and the reason for it is not organisational. A class graph now carries
    // both an exec graph and a COMPONENT TREE (see fmt::OcGraphComponent), and those are answers to
    // different questions: one is behaviour over time, the other is a thing assembled in space. They
    // do not share a canvas, a selection, or a unit -- so they do not share a view.
    //
    // A NON-CLASS GRAPH STILL GETS BOTH TABS, deliberately. Hiding Viewport until a CLASS record
    // exists would mean the way to discover that a graph can BE an actor is to already know. The tab
    // says so instead, in the one place someone would look for it.
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

// The Event Graph tab: the node canvas and its details column. Lifted out of draw() unchanged when
// the Viewport tab arrived -- draw() is now the tab bar and nothing else, which is the only way
// either tab's body stays readable.
void GraphEditor::drawEventGraph(float dpi) {
#if AVER_WITH_IMGUI
    recomputeLayouts(dpi);

    // ---- canvas + details columns -------------------------------------------------------------------
    // The canvas gives up a fixed-width strip on the right for the details panel (Gap B). Every
    // downstream canvas calculation (originIm, canvasSize, mouse-to-canvas conversion) is derived from
    // GetContentRegionAvail() called AFTER ##graphCanvas's BeginChild below, so it automatically sees
    // the narrowed region rather than the full window -- nothing past this point needed to change for
    // that to hold.
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    const float detailsW = std::clamp(260.0f * dpi, 180.0f * dpi, std::max(avail.x * 0.45f, 120.0f * dpi));
    const float canvasW = std::max(avail.x - detailsW - ImGui::GetStyle().ItemSpacing.x, 40.0f * dpi);
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

    // ---- frame on first open, now that the canvas size is a real number -----------------------
    // A graph whose nodes sit at large coordinates -- or one auto-laid-out at a DPI this display
    // does not use -- opened showing empty grid, and the only way to find the content was to drag
    // until it appeared. One shot, cleared whether or not it found anything, so an empty graph
    // does not retry every frame and a graph the user has since panned is never yanked back.
    // THE RULE IS "THE VIEW IS MINE UNTIL YOU TOUCH IT", not "frame once and hope".
    //
    // The first thing tried here was a one-shot on open, and it framed against a canvas 1853 px
    // wide when the window settled at 2670 -- measured, not assumed -- leaving a correctly
    // computed frame a third of a screen off. The second thing tried was "keep framing until two
    // consecutive frames agree on the size", and it ALSO measured 1853 twice, because the window
    // grows later than that and there is no frame count at which it is safe to stop looking.
    //
    // So stop guessing when the layout is final and track the thing that actually matters: has
    // the AUTHOR chosen a view? Until they pan or zoom, the view is not theirs, it is this
    // editor's best answer to "show me the graph" -- and the best answer to that changes when the
    // canvas changes size. The moment they scroll or drag, it is theirs and nothing moves it
    // again. This also fixes window resize and the details-panel split for free, which the
    // one-shot never would have.
    const bool canvasResized = canvasSize.x != lastCanvasSizePx_.x || canvasSize.y != lastCanvasSizePx_.y;
    if (framePendingFromToolbar_ || (!viewTouched_ && (pendingFrame_ || canvasResized))) {
        frameAll(Vec2{canvasSize.x, canvasSize.y}, dpi);
        pendingFrame_ = false;
        framePendingFromToolbar_ = false;
    }
    lastCanvasSizePx_ = Vec2{canvasSize.x, canvasSize.y};

    // ---- background grid ----------------------------------------------------------------------
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

    // ---- input: start a new interaction ---------------------------------------------------------
    if (hovered && dragMode_ == DragMode::None) {
        const bool spacePan = io.KeyShift == false && ImGui::IsKeyDown(ImGuiKey_Space);
        if ((spacePan && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) || ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
            dragMode_ = DragMode::PanCanvas;
            rightButtonPan_ = false;
            dragStartScreen_ = mouseScreen;
            panAnchorPx_ = view_.panPx;
        } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            // Right button is ambiguous at mouse-DOWN: a plain click must still open Add Node (existing,
            // relied-on behaviour), but a right-button DRAG is the direct pan gesture this task asked
            // for -- the button is otherwise idle for the whole rest of the drag, since only the CLICK
            // was ever taken. Start panning right away (same zero-latency feel as Middle-drag, below)
            // and remember both the press position (to measure travel) and the press CANVAS point
            // (pendingSpawnCanvasPos_ -- what the popup would open at) so release can retroactively
            // decide which gesture this was. See the PanCanvas case below for the other half.
            dragMode_ = DragMode::PanCanvas;
            rightButtonPan_ = true;
            dragStartScreen_ = mouseScreen;
            panAnchorPx_ = view_.panPx;
            pendingSpawnCanvasPos_ = mouseCanvas;
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
                // COMMENT BOXES ARE TESTED LAST, after nodes, pins and links have all missed.
                // A box is drawn behind everything, so anything drawn on top of it owns the
                // click -- including a node sitting over its title bar. Testing the box first
                // would make those nodes unclickable for no visible reason.
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

    // ---- input: continue / end the active interaction --------------------------------------------
    switch (dragMode_) {
    case DragMode::PanCanvas: {
        const bool stillDown = ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
                                ImGui::IsMouseDown(ImGuiMouseButton_Right);
        if (stillDown) {
            const Vec2 moved = mouseScreen - dragStartScreen_;
            if (vecLen(moved) > 0.0f) viewTouched_ = true;
            view_.panPx = panAnchorPx_ + moved;
        } else {
            // Right button only: resolve the click-vs-drag ambiguity now that the button is up. A
            // couple of screen pixels of "click threshold" absorbs the involuntary jitter a real mouse
            // click always has between press and release -- without it, EVERY right click would measure
            // a nonzero travel and the popup would never open. dpi-scaled (not the bare "3.0f" MoveNodes
            // uses just below) because mouseScreen is raw device pixels: at 200% DPI the same physical
            // hand-jitter covers twice as many of them, and an unscaled threshold would make the popup
            // progressively harder to summon on a high-DPI display.
            constexpr f32 kRightClickDragThresholdPx = 4.0f;
            if (rightButtonPan_) {
                const f32 travelled = vecLen(mouseScreen - dragStartScreen_);
                if (travelled <= kRightClickDragThresholdPx * dpi) {
                    view_.panPx = panAnchorPx_; // a click must pan by exactly zero, not by a few stray px
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
                undoStack_.push_back(pendingMoveSnapshot_);
                constexpr usize kUndoCap = 200;
                if (undoStack_.size() > kUndoCap) undoStack_.erase(undoStack_.begin());
                redoStack_.clear();
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
                undoStack_.push_back(pendingCommentSnapshot_);
                constexpr usize kUndoCap = 200;
                if (undoStack_.size() > kUndoCap) undoStack_.erase(undoStack_.begin());
                redoStack_.clear();
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

    // ---- wheel: zoom by default; Shift+wheel (and MouseWheelH, a genuine horizontal-scroll axis on
    // trackpads/tilt-wheel mice) pan instead. NOT both newly-bound: MouseWheelH was genuinely never
    // read anywhere in this file before, but Shift+wheel was not unbound -- the pre-change handler
    // checked only bare io.MouseWheel with no KeyShift exclusion, so a Shift-held scroll already fell
    // into that branch and zoomed, identically to a bare scroll (io.MouseWheel carries the same value
    // regardless of which modifiers are down). Shift+wheel is REPURPOSED here from a redundant alias of
    // zoom to a distinct pan gesture, not bound from nothing -- and loses nothing by it, since it never
    // produced an effect a bare wheel didn't already produce. Either way, this is additive to a bare,
    // unmodified wheel: that path (the `else if` below) is untouched and still zooms exactly as before.
    if (hovered && io.KeyShift && io.MouseWheel != 0.0f) {
        // Pan, not zoom, while Shift is held -- a second direct-pan gesture for a mouse-only user (no
        // middle button, no reach for Space) who is already resting a hand on the wheel. Sign matches
        // the "content scrolls like a document" convention every other app on the user's desktop
        // already trained them on: wheel-up (io.MouseWheel > 0) moves the CONTENT down (panPx.y grows),
        // the same direction scrolling up in a text editor reveals earlier/upper content by pushing the
        // current view down -- NOT the "camera pans up" reading, which would be the opposite sign.
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

    // ---- keyboard: delete selection, undo/redo -----------------------------------------------------
    if (canvasFocused) {
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) deleteSelection();
        // C wraps the selection in a comment box -- the same key Blueprint binds it to, and
        // the reason the gesture is worth having at all: drawing a box by hand around six
        // nodes and then nudging its edges is enough work that nobody does it.
        if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_C, false) && !selectedNodes_.empty())
            addCommentAroundSelection(dpi);
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) { if (io.KeyShift) redo(); else undo(); }
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) redo();
        // F frames the selection (falling back to everything), Home always frames everything --
        // the same two bindings Blueprint uses, and the reason for having both is that "show me
        // what I just clicked" and "show me where I am" are different questions.
        if (!io.KeyCtrl && !io.KeyAlt && ImGui::IsKeyPressed(ImGuiKey_F, false))
            frameSelection(Vec2{canvasSize.x, canvasSize.y}, dpi);
        if (ImGui::IsKeyPressed(ImGuiKey_Home, false)) frameAll(Vec2{canvasSize.x, canvasSize.y}, dpi);
    }

    // ---- draw: comment boxes at the back, then links, then nodes -- ImDrawListSplitter, not
    // submission order (submission order would put every link over whichever node happened to
    // draw after it; the splitter is exactly what it exists for, per third_party/imgui/imgui.h's
    // own recommendation).
    //
    // THREE channels now, not two. A comment box has to be behind the WIRES as well as behind the
    // nodes: a box tinted over a wire reads as a pane of glass in front of it, and the whole
    // point of the box is to be scenery.
    dl->ChannelsSplit(3);

    // WHICH PINS ARE WIRED UP, built once for the whole frame rather than searched per pin. Both
    // ends of every link count, because "is this pin connected" is a question about the pin, not
    // about which direction the wire leaves it. The drawing below uses it to fill a connected pin
    // and leave an unconnected one hollow -- which is the one piece of information a node editor
    // can show for free and which this one was throwing away: an unwired `exec` input means the
    // node never runs, and that was previously indistinguishable from a wired one.
    // ---- TEXT SCALES WITH ZOOM, AND STOPS BEING DRAWN WHEN IT STOPS BEING READABLE.
    //
    // Every label here used to be submitted at the UI font size regardless of zoom. Node BOXES
    // shrink with zoom and text did not, so a graph zoomed out far enough to see whole -- which,
    // for a real 63-node graph at 300% DPI, is about zoom 0.2 -- drew sixty full-size pin labels
    // on top of each other and read as noise. The nodes were laid out correctly the whole time;
    // it was the type that was wrong, which is why it looked like a layout bug and was not.
    //
    // Two thresholds, not one. Pin labels go first and go earlier: there are five times as many of
    // them, they are the ones that collide, and "which pin is this" is a question you ask close up.
    // A node TITLE survives further out, because at overview zoom the only question left is what
    // the shapes ARE, and a graph of unlabelled boxes answers nothing.
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
        // A comment title is a LANDMARK -- the thing you navigate a zoomed-out graph by -- so it
        // gets a floor the node labels do not: it shrinks with the box, but never below the size
        // at which it would stop doing its job. The clip rect it already had keeps an oversized
        // title inside its own bar rather than letting it run across the canvas.
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
        // Exec wires are drawn HEAVIER than data wires. In a graph with both, the exec chain is
        // the spine -- the order things happen in -- and everything else is an argument being
        // fetched. Weight says which is which from across the canvas, at a distance where the
        // white-versus-coloured difference is still legible but the shapes are not.
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

        // A soft drop shadow, offset down-right, before anything else in the node. It is what makes
        // a node read as sitting ABOVE the wire layer instead of being punched out of it -- worth
        // more here than in most UIs, because the canvas behind a node is not empty background but
        // a mesh of bright wires, and without it the eye has no cue for which is in front.
        const f32 shadowOff = 3.0f * dpi;
        dl->AddRectFilled(ImVec2(pMin.x + shadowOff, pMin.y + shadowOff),
                          ImVec2(pMax.x + shadowOff, pMax.y + shadowOff),
                          IM_COL32(0, 0, 0, 90), 6.0f * dpi);

        // Body in the engine's own card colour rather than a generic dark grey, so a node reads
        // as part of this editor and not as a floating rectangle.
        //
        // FULLY OPAQUE, deliberately. It used to be alpha 240, and at 94% opacity a bright green
        // data wire passing behind a node showed through its body as a faint diagonal smear --
        // which reads as a wire crossing IN FRONT, the exact thing the channel split above exists
        // to prevent. Six percent of translucency bought nothing and undid that.
        dl->AddRectFilled(pMin, pMax, IM_COL32(21, 25, 32, 255), 5.0f * dpi);
        dl->AddRectFilled(pMin, ImVec2(pMax.x, pMin.y + headerH), headerCol, 5.0f * dpi, ImDrawFlags_RoundCornersTop);
        // A hairline under the header. Cheap, and it stops a dark body and a dark header from
        // reading as one block on the categories whose colour is already close to the body.
        dl->AddLine(ImVec2(pMin.x, pMin.y + headerH), ImVec2(pMax.x, pMin.y + headerH),
                    IM_COL32(0, 0, 0, 90), 1.0f * dpi);
        dl->AddRect(pMin, pMax, selected ? kSelectionCol : IM_COL32(12, 14, 18, 255), 5.0f * dpi, 0, selected ? 2.5f * dpi : 1.0f * dpi);
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
            // EXEC PINS DRAW AS A RIGHT-POINTING ARROW, DATA PINS AS A CIRCLE -- shape, not just
            // colour (see colorForType's own comment on why colour alone is not enough). This is
            // the single change that makes a graph with both dataflow and control flow on the same
            // node body actually readable at a glance: a wire is either a value or "what runs
            // next", and mixing the two up is the exact failure the task called out as the biggest
            // usability risk in a node editor.
            //
            // The arrow POINTS, which a diamond -- what this used to draw -- does not. Execution
            // has a direction and the shape can say so for free, on both sides of the node: an
            // input arrow points into the body, an output arrow points out of it, and the chain
            // reads left-to-right without following a single wire. It is also what Blueprints
            // draw, and the same argument as the pin colours applies -- this is a learned
            // language, not a place to be distinctive.
            //
            // HOLLOW MEANS UNCONNECTED, for both shapes. An exec input with no wire is a node
            // that never runs; a data input with no wire falls back to its default. Both are
            // things an author needs to see without clicking, and both were invisible before.
            if (isExecPinType(pl.type)) {
                // Slightly narrower than tall, so it reads as an arrowhead rather than a wedge.
                const ImVec2 arrow[3] = {
                    ImVec2(dot.x - r * 0.85f, dot.y - r),
                    ImVec2(dot.x + r * 0.95f, dot.y),
                    ImVec2(dot.x - r * 0.85f, dot.y + r),
                };
                if (wired) dl->AddConvexPolyFilled(arrow, 3, pinCol);
                // (points, num_points, col, thickness, flags) -- the CURRENT AddPolyline signature
                // (imgui.h:3527). An older 1.92.7-and-earlier signature took (col, flags, thickness) in
                // the other order; this codebase's imconfig.h leaves the compatibility shim enabled
                // (IMGUI_DISABLE_OBSOLETE_FUNCTIONS is commented out) so either order would technically
                // link, but writing the current order directly avoids depending on that shim.
                //
                // The outline is drawn in BOTH states: over the fill it is the dark separation that
                // keeps a white arrow off a white-ish header, and without a fill it IS the pin.
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
        // refused: " itself; a blocked variable delete embeds "cannot delete variable ...") -- see
        // showRejectionBanner's own comment for why the prefix moved to the CALLER rather than living
        // here, where it used to be hard-coded onto every possible rejection reason regardless of
        // what produced it.
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
    if (ImGui::BeginPopup("##graphAddNode")) {
        // Above the categories, not inside one: a comment box is not a node, has no pins, and
        // filing it under a node family would be the first place an author looked and the last
        // place they found it.
        // Every declared function gets a real "Call <name>" entry, regenerated from graph_.functions
        // every frame -- so renaming one renames its palette row, and there is no second list to
        // keep in step. A call node has no fixed pin shape (it takes the callee's), which is why
        // these cannot live in the static catalog with everything else.
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
        std::vector<std::string> categories;
        for (const auto& d : graphNodeCatalog()) {
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
                    if (d.category != cat) continue;
                    // Thin glue: everything the drop actually DOES is addNodeFromCatalog, so it can
                    // be driven by a test with no ImGui context.
                    if (ImGui::MenuItem(d.displayName.c_str()))
                        addNodeFromCatalog(d.typeId, pendingSpawnCanvasPos_);
                }
                ImGui::EndMenu();
            }
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();

    // ---- details panel: Variables (graph-level) above a selected node's attributes (Gap B) -----------
    // Thin ImGui glue only -- the actual model lives in GraphEditor's own addVariable/renameVariable/
    // retypeVariable/setVariableDefault/deleteVariable (this file, above) for the Variables panel, and
    // in GraphEditorGeometry.hpp's getNodeAttribute/setNodeAttribute/removeNodeAttribute/
    // computeAttributeRows for the per-node attribute rows below it -- both exercised headlessly (no
    // ImGui context) by GraphEditorGeometryTest and GraphEditorLoadSaveTest.
    ImGui::SameLine();
    ImGui::BeginChild("##graphDetails", ImVec2(detailsW, std::max(avail.y, 80.0f * dpi)), true);

    drawFunctionsPanel(dpi);

    // ---- Variables panel: declare / rename / retype / delete -----------------------------------------
    // Lives ABOVE the per-node section below, and is drawn regardless of selection (unlike everything
    // below it) -- a variable belongs to the GRAPH, not to whichever node happens to be selected, and
    // the state right after opening a graph (nothing selected) is exactly when an author most needs to
    // declare one. This is also THE fix for the bug the task brief leads with: a freshly palette-
    // spawned SetVar has no variable to name yet, and this panel -- not a free-text field somewhere --
    // is where one gets created.
    if (ImGui::CollapsingHeader("Variables", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (graph_.variables.empty()) {
            ImGui::TextDisabled("No variables declared.");
        }
        // Snapshot names before iterating: a Delete button pressed on row i mutates graph_.variables
        // mid-loop (pushUndo() + erase, inside deleteVariable), which would invalidate an iterator or
        // index into the live vector for every row after it. Looking each one up FRESH by name, every
        // iteration, tolerates that -- a name deleted by an earlier row this same frame is simply
        // skipped (continue), rather than read through a dangling reference.
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

            // ---- default value ------------------------------------------------------------------
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

            // ---- mismatch note: nodes whose OWN 'value' pin type disagrees with this variable's
            // CURRENT declared type. Computed fresh every frame straight from the live pins -- see
            // retypeVariable's own comment for why a retype never rewrites those pins itself, and why
            // this is what makes the resulting mismatch visible instead of silently wrong.
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

                // ---- THE PICKER: GetVar/SetVar's var= row, special-cased over EVERY other declared
                // attribute (field=/class=/param=/etc. all keep the generic free-text InputText path
                // below, unchanged -- see the task brief's own "narrow, not a generic key=value
                // framework" scoping). `row.declared && row.key == "var"` is reachable ONLY for
                // GetVar/SetVar: they are the only catalog entries that declare a "var" attribute at
                // all (GraphNodeDefs.hpp), so this needs no separate node-type check.
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
                        // THE BUG THIS ROW EXISTS FOR: a GetVar/SetVar naming a variable nobody
                        // declared (a freshly typed-then-later-deleted name, a hand-edited file, or --
                        // before this picker existed -- simple free-text fat-fingering) does not
                        // compile (Graph.Validate() refuses it) and used to give no sign anything was
                        // wrong. Naming the problem AND offering a one-click fix, per the task's own
                        // "either the variable can be declared on the spot, or the node says exactly
                        // what is wrong" requirement -- this does both, rather than choosing one.
                        ImGui::TextColored(ImVec4(0.95f, 0.55f, 0.35f, 1.0f), "'%s' is not a declared variable.", row.value.c_str());
                        const std::string declareLabel = "Declare '" + row.value + "'";
                        if (ImGui::Button(declareLabel.c_str())) {
                            // Guess the type from the node's OWN 'value' pin, if it has one -- a
                            // freshly palette-spawned GetVar/SetVar always does (GraphNodeDefs.hpp's
                            // catalog copies its pins onto the node at spawn time), so this declares
                            // the variable at the type the node is ALREADY wired to expect, needing no
                            // further retype to compile.
                            std::string guessType = "float";
                            for (const auto& p : node->pins) if (p.name == "value") { guessType = p.type; break; }
                            addVariable(row.value, guessType, "");
                        }
                    } else if (row.value.empty() && graph_.variables.empty()) {
                        // THE BUG'S OTHER HALF: a freshly palette-spawned SetVar has var="" AND the
                        // graph has no variables declared at all yet, so the combo above has nothing
                        // to offer but "(none)". Point at the Variables panel above rather than
                        // leaving a dead end with no next step visible from here.
                        ImGui::TextDisabled("No variables declared -- add one in the Variables panel above.");
                    } else if (row.value.empty()) {
                        // THE GAP THE OTHER TWO BRANCHES MISS: var="" (a freshly palette-spawned node,
                        // or one just cleared to "(none)" via the Selectable above) in a graph that
                        // ALREADY has at least one variable declared. Neither branch above fires here --
                        // `known` is false (row.value is empty, so the any_of never runs) but so is the
                        // "!row.value.empty()" guard on the undeclared-name branch, and
                        // graph_.variables.empty() is false too -- so without this branch the combo
                        // just shows "(none)" as if that were a complete, compilable choice. It is not:
                        // OcGraphParser.cs refuses a GetVar/SetVar with no var= attribute at all just as
                        // firmly as one naming an undeclared variable (verified empirically -- see the
                        // task's verification notes -- not merely asserted). Proven reachable by the
                        // load/save test suite itself: testDeleteVariableRefusesWhileReferencedThenSucceeds
                        // clears 'gv's var= to unblock a delete, and the resulting saved file is exactly
                        // this state -- a GetVar node with var="" in a graph that still declares 'speed'.
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


// ================================================================================================
// The Viewport tab: a class graph's COMPONENT TREE, and a preview of the actor it assembles.
// ================================================================================================
//
// WHY THIS LIVES BESIDE THE NODE CANVAS AND NOT IN ITS OWN EDITOR. A .ocgraph carrying a CLASS record
// is one asset that answers two questions -- what the actor DOES and what it IS -- and both are
// edited into the same file. Two tabs over one document is the arrangement that keeps a single save
// path, a single undo stack and a single dirty flag; two editors over one file would need all three
// duplicated and reconciled.
//
// THE PREVIEW IS THE ONE THE ACTOR EDITOR ALREADY OWNS. sharedPreview() exists precisely so every
// asset tab draws through one render feature (see ActorEditor.hpp's comment on why a second one would
// draw into this one's target), and previewComposeTransform is exported from that file so an actor
// assembled from COMP records and one assembled from designer placements cannot disagree about what
// "yaw 90" means.

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
    {"Animator",     "A clip and the clock running it, on whatever this is parented to."},
    {"Particles",    "One emitter instance, playing a .ocparticle effect."},
    {"Camera",       "Camera parameters. Stored correctly; no renderer reads CCamera yet."},
    {"Light",        "A light. Stored correctly; no renderer reads CLight yet."},
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
// ENGINE BUILT-INS, NOT ANYTHING A TEMPLATE SHIPS. These two rows used to read
// "Meshes/Blaster.ocmesh" and "M_Gun", which are the FirstPerson template's own gun and its own
// material name -- so the editor's palette suggested, to every project on the machine, an asset
// only one template contains and a material nothing outside it defines. Every other row here
// (Hero.ocmesh, Idle.ocanim, Muzzle.ocparticle) is an INVENTED illustrative name that belongs to
// no shipped content at all; the Mesh row was the one that had drifted into naming real files from
// a template, and the editor must not know a template exists.
//
// cube.ocmesh and M_Crate are engine built-ins -- the primitive PreviewMeshCache synthesises and
// one of the materials SandboxApp registers -- so unlike the invented names these actually resolve
// in a blank project, which is what a hint should do.
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
constexpr GraphComponentAttrSet kGraphComponentAttrs[] = {
    {"Mesh", kMeshRows, 2},
    {"SkeletalMesh", kSkelRows, 3},
    {"Animator", kAnimRows, 3},
    {"Particles", kParticleRows, 2},
    {"Camera", kCameraRows, 4},
    {"Light", kLightRows, 8},
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

// ---------------------------------------------------------------- tree queries

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

// ---------------------------------------------------------------- tree edits

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
    // longer exists, and the parser refuses that on the next load -- deleting one component would
    // make the file unopenable. Blueprint deletes the subtree too, so this is also what an author
    // expects to happen.
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

// ---------------------------------------------------------------- the tab

void GraphEditor::drawViewport(Engine& e, float dpi) {
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
    // THE CLASS RECORD IS THE POINT OF THIS TAB, so its absence is stated here rather than left for
    // someone to discover when their carefully built tree spawns nothing. C++ does not model CLASS
    // (it rides through as an unknown record), so this asks the raw text -- the same question
    // GameApp's own ocgraphDeclaresClass asks, for the same yes/no purpose.
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

    // ---- name ------------------------------------------------------------------------------------
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

    // ---- kind ------------------------------------------------------------------------------------
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

    // ---- parent ----------------------------------------------------------------------------------
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

    // ---- transform -------------------------------------------------------------------------------
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

    // ---- kind-specific ---------------------------------------------------------------------------
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

void GraphEditor::buildComponentPreview(Engine& e) {
    render::preview::ActorPreview* preview = sharedPreview(e);
    if (!preview || !e.device()) return;

    render::preview::PreviewMeshCache& meshes = sharedPreviewMeshes();
    meshes.setContentRoot(*e.device(), actorEditorContentRoot());

    std::vector<render::preview::PreviewDraw> draws;
    draws.reserve(graph_.components.size());
    for (const auto& c : graph_.components) {
        const std::string_view meshPath = fmt::componentAttr(c, "mesh");
        // ONLY WHAT HAS GEOMETRY. A Scene node, a Camera and an Animator have nothing to draw, and
        // inventing a placeholder box for them would make the preview disagree with the game -- the
        // one thing a preview must never do. Their transforms are still real; they are simply
        // invisible here exactly as they are invisible there.
        if (meshPath.empty()) continue;
        render::preview::PreviewDraw d;
        d.mesh = meshes.resolve(*e.device(), meshPath, &d.boundsRadius);
        if (d.mesh == 0) continue;   // missing or unloadable; meshes.missing() already records it
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
void GraphEditor::drawComponentToolbar(float) {}
void GraphEditor::drawComponentTree(float) {}
void GraphEditor::drawComponentTreeNode(const std::string&, float) {}
void GraphEditor::drawComponentDetails(float) {}
void GraphEditor::buildComponentPreview(Engine&) {}

#endif  // AVER_WITH_IMGUI

// ------------------------------------------------------------------------------------------ factory

std::unique_ptr<AssetEditor> makeGraphEditor(const std::string& path) {
    std::string ext = std::filesystem::path(path).extension().string();
    for (char& c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (ext != ".ocgraph") return nullptr;
    return std::make_unique<GraphEditor>(path);
}

} // namespace aver::editor
