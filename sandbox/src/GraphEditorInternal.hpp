// GraphEditor's file-local helpers, shared by its translation units (GraphEditor.cpp, GraphEditorCanvas.cpp,
// GraphEditorComponents.cpp). Internal: include it after those files' own includes, nowhere else. The split exists
// because one TU crossed the COFF section cap in Debug (sandbox/CMakeLists.txt says why that means a split).
#pragma once

#include "GraphClipboard.hpp"
#include "GraphEditor.hpp"
#include "GraphNodeDefs.hpp"

#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor::gedetail {

inline float g_graphEditorDpi = 1.0f;

inline f32 vecLen(Vec2 v) { return std::sqrt(v.x * v.x + v.y * v.y); }

// The three VAR types the C# PinType enum accepts (Enum.TryParse<PinType>), case-insensitive. Exec is
// excluded: it's computed-with data, not remembered-between-ticks data (OcGraphParser.cs rejects
// "VAR ... declared exec"). Graph.Validate() is the real compile-time gate; this only keeps
// addVariable/retypeVariable's fallback and the Variables panel's Combo honest about what it produces.
inline bool isValidVarType(const std::string& type) {
    std::string t = type;
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t == "float" || t == "int" || t == "bool";
}

// Shared with setAttribute's no-quoting guard (OcGraph.cpp splitWhitespace / OcGraphParser.cs
// SplitWhitespace both tokenise on bare whitespace), so VAR fields get the same protection as NODE attrs.
inline bool containsWhitespace(const std::string& s) {
    return s.find_first_of(" \t\r\n\v\f") != std::string::npos;
}

// Seconds on a clock every build has, for the Problems debounce: pushUndo runs headless too, where there
// is no ImGui::GetTime to ask.
inline double clockSec() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

// Every open tab, so SandboxApp can reach one by path or graph name (a breakpoint that trips in a graph
// whose tab is open elsewhere, Find References across the project) and run diagnostics before Play.
inline std::vector<GraphEditor*> g_openGraphEditors;
inline std::function<bool(const std::string&)> g_graphOpener;
// ImGui frame count of the last frame a graph canvas had focus; -1 until one has.
inline int g_graphCanvasFocusFrame = -1000;

// The .ocproject owning `graphPath`, found by walking up -- duplicates aver::ownerProjectOf
// (sandbox/src/SandboxMain.cpp) in miniature rather than calling it directly: that lives in the
// Sandbox exe's entry-point TU, not a link target of GraphEditorLoadSaveTest (tests/editor/CMakeLists.txt
// compiles GraphEditor.cpp + GraphEditorGeometry.cpp only), so a direct call would link in the real
// Sandbox app and fail to link in that test. Bounded to 8 levels, the same bound ownerProjectOf's own
// comment gives -- well past any real Content/Maps/... nesting, so a graph with no project can't walk
// to the drive root looking for one.
inline std::string findOwnerProjectForGraph(const std::string& graphPath) {
    std::error_code ec;
    std::filesystem::path dir = std::filesystem::path(graphPath).parent_path();
    for (int up = 0; up < 8 && !dir.empty(); ++up) {
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->is_directory(ec)) continue;
            std::string ext = it->path().extension().string();
            for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (ext == ".ocproject") return it->path().string();
        }
        const std::filesystem::path parent = dir.parent_path();
        if (parent == dir) break;  // reached the root; parent_path() stops changing
        dir = parent;
    }
    return {};
}

// The node header label: catalog displayName, except GetVar/SetVar show the bound variable's own name
// (as Unreal does) rather than "Get Var" regardless of which one -- unreadable in a wall of them.
//
// Headless and above the ImGui guard: the layout pass sizes a node from this title with no ImGui in
// the TU, so a box sized for "Get Var" would clip "PlayerSpeed".
inline std::string nodeTitle(const fmt::OcGraphNode& n, const GraphNodeDesc* desc,
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

// Everything below is ImGui-typed (ImVec2/ImU32), used only from draw() (itself behind
// `#if AVER_WITH_IMGUI`) -- guarding these too is what lets GraphEditorLoadSaveTest compile this TU
// with no ImGui headers or context (see that test's own file comment).
#if AVER_WITH_IMGUI
inline Vec2 fromIm(ImVec2 v) { return Vec2(v.x, v.y); }

// Pin colour is by declared type (kept from Unreal's palette: green=float, white=exec); header colour
// is by node CATEGORY in Aver's own orange/steel (matching editor chrome), since the node body isn't a
// borrowed surface. Both fall back to neutral for anything unrecognised.
//
// RULE: warm (orange) = reaches outside the graph (event/world change); cool (steel) = pure
// computation; grey = flow control only. Function calls get orange, not Blueprint's blue -- a user
// function and an engine call look the same from the caller's side. Events are red, not orange, so an
// entry point and a call stay visually distinct (also matches Blueprint's convention).
inline ImU32 headerColorForCategory(const std::string& category) {
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

// One highlight colour for selected node/link/comment borders and a link's end-pin ring (was
// duplicated per site). Amber, not Unreal's white, so it isn't confused with an exec wire; dimmer than
// the (255,220,90) it replaces, for the same glare reason the pin palette below was toned down.
constexpr ImU32 kSelectionCol = IM_COL32(226, 188, 96, 255);

inline ImU32 colorForType(const std::string& type) {
    std::string t = type;
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    // Unreal's pin colours, kept deliberately (see headerColorForCategory above): a Blueprint user
    // already knows green=float, red=bool, so re-hueing would be a worse graph than either palette.
    // Toned down from Unreal's near-fullbright originals (float was (91,255,15), string (255,0,168)) for
    // glare at real graph size, then toned down AGAIN 2026-09-22, because the owner reported the
    // connectors still reading too bright: each value
    // below is the previous one with hue held exactly, saturation x0.72, value x0.86 in HSV, so the
    // five stay in the same relationship rather than drifting apart one eyedropper at a time.
    // Measured minimum pairwise separation (luma-weighted opponent space) went 0.220 -> 0.162 -- still
    // distinguishable at pin size, which is the binding constraint. Another pass this size lands near
    // 0.12; re-measure before doing it.
    if (t == "float")  return IM_COL32(126, 171,  95, 255);  // yellow-green
    if (t == "int")    return IM_COL32( 88, 156, 138, 255);  // turquoise
    if (t == "bool")   return IM_COL32(151,  87,  87, 255);  // red
    if (t == "string") return IM_COL32(160, 102, 139, 255);  // magenta
    if (t == "exec")   return IM_COL32(180, 182, 186, 255);  // white -- see above
    return IM_COL32(131, 134, 138, 255);
}

// A node's header takes its category colour, except GetVar/SetVar take their VARIABLE's type colour
// (see nodeTitle() above). Read from the declaration, not the node's own pin: retypeVariable() doesn't
// rewrite already-placed pins, so a value pin can outlive a retype and still say "float" when the
// variable is now an int.
inline ImU32 nodeHeaderColor(const fmt::OcGraphNode& n, const GraphNodeDesc* desc,
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
inline bool isExecPinType(const std::string& type) {
    std::string t = type;
    for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return t == "exec";
}
#endif

constexpr float kCommentBarPx  = 24.0f;
constexpr float kCommentGripPx = 16.0f;
constexpr float kCommentMinPx  = 60.0f;

// A node's owning subgraph, read straight from its func= attribute. Empty means the event graph --
// see GraphEditor::currentSubgraph() for why one convention is used everywhere rather than three.
inline std::string subgraphOf(const fmt::OcGraphNode& n) {
    const GraphNodeAttribute a = getNodeAttribute(n, "func");
    return a.found ? a.value : std::string();
}
inline bool isFunctionNodeType(const std::string& t) {
    return t == "FuncEntry" || t == "FuncReturn" || t == "CallFunc";
}

constexpr float kFramePaddingPx = 40.0f;

// Zoom floor is DPI-relative (screen px per logical px), not an absolute zoom value: the floor is
// really about zoom * dpi ("don't shrink past a smear"), and a flat floor bites too early on
// high-DPI. Measured: AN_FPCharacter.ocgraph (63 nodes) auto-lays out to 11907x5400 canvas units at
// dpi 3, needing zoom 0.135 to fit an 1853px canvas -- the old flat 0.15 floor clamped that short, so
// Frame All did not frame all. Only the floor scales, not the ceiling: zoom 4 at dpi 3 (12x) is unused
// headroom, and halving it would remove working range for no benefit.
constexpr float kMinScreenPxPerLogicalPx = 0.15f;
constexpr float kMaxZoom = 4.0f;
inline float minZoomForDpi(float dpi) { return kMinScreenPxPerLogicalPx / std::max(dpi, 0.25f); }

// Whether clipboard text is one of ours, without parsing the lot: asked every time the node menu draws.
inline bool looksLikeClip(const char* text) {
    if (!text) return false;
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') ++text;
    return std::strncmp(text, kClipMagic, std::strlen(kClipMagic)) == 0;
}

// The details ("Gap B") column's default width and persisted pref key -- a DPI-INDEPENDENT PIXEL
// WIDTH, ActorEditor's own convention (ActorEditor.cpp), not the fraction most other editors' splits
// use (see EditorWidgets.hpp's top comment). 260.0f matches this tab's old un-persisted default
// (`std::clamp(260.0f * dpi, 180.0f * dpi, std::max(avail.x * 0.45f, 120.0f * dpi))`), which recomputed
// fresh every frame with no persistence at all -- kept for the ordinary wide-enough case; see
// drawEventGraph() for the narrow-window behaviour that formula also covered.
constexpr float kDefaultDetailsColumn = 260.0f;
constexpr const char* kPrefDetailsColumn = "graphEditor.detailsColumn";

} // namespace aver::editor::gedetail
