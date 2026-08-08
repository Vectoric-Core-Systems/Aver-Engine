#pragma once
// The .ocgraph node editor's headless core: layout, hit-testing, link rules, cycle detection, and the
// canvas pan/zoom transform. Deliberately ImGui-free and Engine-free (see GraphNodeDefs.hpp for the
// node descriptor table this reads) so it compiles and runs with no window and no GPU -- see
// tests/editor/src/GraphEditorGeometryTest.cpp, which links exactly this file plus Aver.Formats.
//
// Everything here is either a pure function or a plain struct with value semantics. DPI/zoom scale is
// always an explicit parameter, never read from global UI state -- that is what makes it testable
// outside ImGui, and it is the same reason SandboxApp.cpp's own dpi_ is threaded through by parameter
// rather than read from a global (see AssetEditorHost::draw's `dpi` parameter).
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcGraph.hpp"

#include <string>
#include <unordered_map>
#include <vector>

namespace aver::editor {

// ---------------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------------

// Unscaled ("design pixel", scale == 1.0) layout constants. Every call below multiplies these by an
// explicit `scale` parameter -- follow sandbox/src/SandboxApp.cpp's own convention of `<value>f *
// dpi_` at the call site once this is wired into ImGui drawing.
struct GraphLayoutStyle {
    // Text-width estimate. There is no ImGui here to call CalcTextSize against a real font, so node
    // width is sized from a fixed average-glyph-width heuristic instead. This is intentionally an
    // approximation, not a font metric -- it keeps layout deterministic and headless-testable. A
    // drawing layer that wants pixel-perfect title fit can still round-trip: nothing here prevents
    // ImGui::CalcTextSize from being used as an *additional* check before submitting geometry to the
    // draw list, this just guarantees a reasonable, stable box exists without one.
    f32 charWidthPx = 7.0f;
    f32 headerHeightPx = 24.0f;
    f32 pinRowHeightPx = 20.0f;
    f32 pinRadiusPx = 5.0f;
    // Pin hit radius is deliberately larger than the visual dot radius (5px). A pin is the smallest,
    // most failure-prone target in a node editor -- missing it by two pixels while dragging a link is
    // the single most common node-editor usability complaint -- so this gives roughly a Fitts's-law-
    // friendly ~18px-diameter target (matching the touch-target-adjacent sizing used for the drawer
    // resize grip elsewhere in this codebase) rather than the visual dot's ~10px diameter.
    f32 pinHitRadiusPx = 9.0f;
    f32 paddingXPx = 10.0f;
    f32 paddingBottomPx = 8.0f;
    f32 minWidthPx = 90.0f;
    f32 pinLabelReservePx = 46.0f; // per-side reserve for a pin's label, beyond its dot+gap
    f32 linkHitDistancePx = 6.0f;  // max perpendicular distance from a bezier sample to count as a hit
};

// One pin's computed position and identity, in CANVAS space (not screen space -- see CanvasTransform
// below for the canvas->screen step).
struct GraphPinLayout {
    std::string name;
    std::string type;
    bool isOutput = false;
    Vec2 pos; // canvas-space position of the pin's connection point
};

// One node's computed box and pin positions, in canvas space.
struct GraphNodeLayout {
    std::string nodeId;
    Vec2 min; // top-left, canvas space
    Vec2 max; // bottom-right, canvas space
    std::vector<GraphPinLayout> pins; // order matches node.pins
};

// Computes a node's box size and every pin's position from its title, its OWN recorded pins (NOT the
// catalog -- see GraphNodeDefs.hpp's header comment on why), and the style/scale. Deterministic: the
// same node, style and scale always produce the same layout.
//
// `title` is passed separately from the node rather than derived from node.type, so a caller that
// wants to show a display name (from GraphNodeDefs) or a raw type string (for an unrecognised type)
// can do either without this function knowing about the catalog.
GraphNodeLayout computeNodeLayout(const fmt::OcGraphNode& node, const std::string& title,
                                   const GraphLayoutStyle& style, f32 scale);

// ---------------------------------------------------------------------------------------------------
// Hit-testing
// ---------------------------------------------------------------------------------------------------

enum class GraphHitKind { None, Node, Pin, Link };

struct GraphHitResult {
    GraphHitKind kind = GraphHitKind::None;
    std::string nodeId;   // valid for Node and Pin
    std::string pinName;  // valid for Pin
    bool pinIsOutput = false; // valid for Pin
    isize linkIndex = -1; // valid for Link -- index into OcGraphData::links
};

// Finds a pin's canvas-space position within a set of already-computed layouts. Returns nullptr if
// the node or the named pin isn't present in `layouts`. Shared by hitTest (for link endpoints) and by
// any future drawing code that needs a pin's screen anchor for a bezier curve.
const GraphPinLayout* findPinLayout(const std::vector<GraphNodeLayout>& layouts,
                                     const std::string& nodeId, const std::string& pinName);

// Tests a canvas-space point against pins first (generous radius, see GraphLayoutStyle::pinHitRadiusPx
// -- pins are small and must win over the node body beneath them), then node bodies, then links
// (sampled against their bezier curve). `layouts` is iterated back-to-front so the most recently drawn
// (last in the vector / topmost) node wins on overlap, matching normal editor z-order expectations.
GraphHitResult hitTest(const fmt::OcGraphData& graph, const std::vector<GraphNodeLayout>& layouts,
                        Vec2 canvasPoint, const GraphLayoutStyle& style, f32 scale);

// The four cubic-bezier control points for the curve drawn between a source (output) pin and a dest
// (input) pin, given their canvas positions. Horizontal-tangent convention: the same shape
// ImGui::AddBezierCubic precedent in this codebase expects (third_party/imgui/imgui.h:3521).
struct GraphBezier { Vec2 p1, p2, p3, p4; };
GraphBezier linkBezier(Vec2 sourcePos, Vec2 destPos);

// Samples a cubic bezier at `segments` even steps (inclusive of both ends) into `outPoints` (cleared
// first). Exposed as its own function so hit-testing and a future draw call sample identically.
void sampleCubicBezier(const GraphBezier& b, int segments, std::vector<Vec2>& outPoints);

// ---------------------------------------------------------------------------------------------------
// Link rules
// ---------------------------------------------------------------------------------------------------

enum class GraphLinkReject {
    None,             // allowed
    UnknownPin,       // named node/pin does not exist on that node
    SourceNotOutput,  // the "source" end is not an output pin
    DestNotInput,     // the "dest" end is not an input pin
    SelfConnection,   // source and dest are the same node
    DestOccupied,     // the dest input already has an incoming link
    TypeMismatch,     // pin.type differs between the two ends
    WouldCreateCycle, // adding this link would create a cycle in the dataflow graph
};

struct GraphLinkCheck {
    bool ok = false;
    GraphLinkReject reason = GraphLinkReject::None;
    std::string message; // human-readable, names the offending node/pin -- same spirit as the format
                          // parser's own error strings (modules/formats/src/OcGraph.cpp)
};

// Can `sourceNode.sourcePin` (must resolve to an OUTPUT pin) connect to `destNode.destPin` (must
// resolve to an INPUT pin)? Checks, in order: both pins exist, source is an output, dest is an input,
// not a self-connection, dest input not already occupied, pin types match, and finally that the link
// would not close a cycle. Reads pin types from the nodes' OWN recorded pins in `graph`, never from
// the GraphNodeDefs catalog (see that header's note on why).
GraphLinkCheck canConnectPins(const fmt::OcGraphData& graph,
                               const std::string& sourceNode, const std::string& sourcePin,
                               const std::string& destNode, const std::string& destPin);

// Convenience wrapper: canConnectPins(), and if it passes, appends the link to graph.links. Returns
// the same GraphLinkCheck either way so a caller can report the rejection reason without a second
// call. Does not touch anything else (no undo recording -- that is the ImGui layer's job).
GraphLinkCheck tryAddLink(fmt::OcGraphData& graph,
                           const std::string& sourceNode, const std::string& sourcePin,
                           const std::string& destNode, const std::string& destPin);

// ---------------------------------------------------------------------------------------------------
// Cycle detection
// ---------------------------------------------------------------------------------------------------

// True if `destNode` can already reach `sourceNode` by following existing links forward (a link is a
// directed edge sourceNode -> destNode, output feeds input). If it can, adding one more link
// sourceNode -> destNode would close a loop through the path that already exists, so the caller must
// refuse it. Runs a plain BFS over `graph.links`; O(nodes + links) per call, which is fine at
// editor-interaction scale (one call per attempted connection, not per frame).
bool wouldCreateCycle(const fmt::OcGraphData& graph, const std::string& sourceNode, const std::string& destNode);

// ---------------------------------------------------------------------------------------------------
// Canvas transform (pan/zoom)
// ---------------------------------------------------------------------------------------------------

// screen = canvas * zoom + panPx. An explicit struct rather than a matrix: only uniform zoom and 2D
// translation are needed, and keeping the two components named/separate is what makes
// zoomAroundScreenPoint's math legible.
struct CanvasTransform {
    Vec2 panPx{0.0f, 0.0f};
    f32 zoom = 1.0f;
};

Vec2 canvasToScreen(const CanvasTransform& t, Vec2 canvasPt);
Vec2 screenToCanvas(const CanvasTransform& t, Vec2 screenPt);

// Changes zoom to `newZoom` while keeping the canvas point currently under `screenPivot` fixed on
// screen at that same pivot -- the "zoom towards the cursor" behaviour, and the classic place a node
// editor's zoom silently drifts if the pan isn't recomputed alongside the zoom factor. Returns the
// adjusted transform; does not mutate `t`.
CanvasTransform zoomAroundScreenPoint(const CanvasTransform& t, f32 newZoom, Vec2 screenPivot);

// ---------------------------------------------------------------------------------------------------
// Auto-layout
// ---------------------------------------------------------------------------------------------------

// A simple layered/topological placement for a graph whose nodes carry no meaningful position (a
// hand-written .ocgraph, or one written by a tool that never set x/y): nodes with no incoming links
// form column 0, their downstream nodes form column 1, and so on, computed with a Kahn's-algorithm
// layering so it terminates even if the file contains a cycle the editor's own connect rules would
// normally refuse (a hand-edited file can still contain one -- the layering dumps any cycle remnant
// into one final column rather than looping forever).
//
// Returns a plain node-id -> canvas-position map. Deliberately does NOT touch `graph` or return an
// OcGraphData: GraphEditor.cpp keeps this as a DISPLAY-only overlay so a load with no further edits and
// an immediate save stays byte-identical -- auto-layout must never quietly write positions into data
// the user didn't touch. See GraphEditor.cpp's displayPos_ for how the two are kept apart.
std::unordered_map<std::string, Vec2> autoLayoutPositions(const fmt::OcGraphData& graph,
                                                            const GraphLayoutStyle& style, f32 scale);

} // namespace aver::editor
