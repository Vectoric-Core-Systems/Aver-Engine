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
#include <string_view>
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

// Can a value read from a pin typed `srcType` flow into a pin typed `dstType`? canConnectPins takes
// one of these as its final argument instead of hard-coding a rule, so this domain-neutral geometry
// file never has to learn the word "material" to let the editor be as permissive as one particular
// compiler. A plain function pointer, not std::function: every predicate below is a stateless free
// function, so there is nothing to capture and no reason to pay a std::function's indirection and
// possible heap allocation for it -- a caller that ever needs to close over state can still supply
// a capture-less lambda, which converts to a function pointer for free.
using PinTypeCompat = bool (*)(std::string_view srcType, std::string_view dstType);

// TODAY'S rule, and canConnectPins' default: the two pin-type strings must match exactly (case-
// insensitively). This is ALSO what enforces "an exec pin only connects to another exec pin, never
// to data" -- see canConnectPins' own comment above its type check for the long form of why.
bool exactPinTypeMatch(std::string_view srcType, std::string_view dstType);

// The promotion rules a `DOMAIN material` .ocgraph compiles under -- mirrored from widen() in
// modules/render.pbr/src/MaterialGraphHlsl.cpp, not reimplemented independently of it, because the
// whole point of this predicate is that the editor refuses exactly what the compiler would refuse
// and nothing more. In order: exact match; else a scalar (`float`) splats into a wider float2/
// float3/float4, matching an HLSL scalar-to-vector cast; else a wider vector truncates into a
// narrower one (float4->float3->float2->float), matching a swizzle. A float2 into a float3 is
// REFUSED, deliberately, same as widen(): inventing the third component is the compiler guessing,
// and it would guess zero. Any pin-type string outside float/float2/float3/float4 -- an exec pin,
// for instance, though a material graph's own vocabulary never has one -- is not recognised as a
// vector width by this predicate and so can only match by the exact-equality branch above, which
// keeps exec-to-data refused under this predicate too, with no exec-specific code here either.
bool materialPinTypeMatch(std::string_view srcType, std::string_view dstType);

// Can `sourceNode.sourcePin` (must resolve to an OUTPUT pin) connect to `destNode.destPin` (must
// resolve to an INPUT pin)? Checks, in order: both pins exist, source is an output, dest is an input,
// not a self-connection, dest input not already occupied, pin types are compatible under
// `typesCompatible`, and finally that the link would not close a cycle. Reads pin types from the
// nodes' OWN recorded pins in `graph`, never from the GraphNodeDefs catalog (see that header's note
// on why).
//
// `typesCompatible` defaults to exactPinTypeMatch, so a caller that passes nothing (every call site
// as of this writing) sees exactly today's exact-match behaviour, unchanged. A caller that knows its
// graph is a DOMAIN material graph -- and only such a caller -- may pass materialPinTypeMatch instead
// to let the editor accept the same scalar-splat and vector-truncate connections the material
// compiler would.
GraphLinkCheck canConnectPins(const fmt::OcGraphData& graph,
                               const std::string& sourceNode, const std::string& sourcePin,
                               const std::string& destNode, const std::string& destPin,
                               PinTypeCompat typesCompatible = exactPinTypeMatch);

// Convenience wrapper: canConnectPins(), and if it passes, appends the link to graph.links. Returns
// the same GraphLinkCheck either way so a caller can report the rejection reason without a second
// call. Does not touch anything else (no undo recording -- that is the ImGui layer's job). Forwards
// `typesCompatible` straight through to canConnectPins, same default and same meaning.
GraphLinkCheck tryAddLink(fmt::OcGraphData& graph,
                           const std::string& sourceNode, const std::string& sourcePin,
                           const std::string& destNode, const std::string& destPin,
                           PinTypeCompat typesCompatible = exactPinTypeMatch);

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

// The pan/zoom that puts the canvas-space rectangle `contentMin`..`contentMax` fully inside a
// viewport of `viewportPx` pixels, centred, with `paddingPx` of screen-space margin on every side.
// Zoom is clamped to [minZoom, maxZoom] -- and the CENTRING STILL HOLDS when the clamp bites, which
// is the whole reason this is not two lines at the call site. A graph of three nodes wants a zoom
// far above 1.0 to fill the screen; clamping that to maxZoom and keeping the naive pan would leave
// the content jammed in a corner, which reads as a broken button rather than a zoom limit.
//
// A DEGENERATE RECTANGLE IS NOT AN ERROR. A single node has zero height between its own edges only
// if something upstream lost it, but one comment box collapsed to a line, or a content box exactly
// as wide as the viewport, both produce a zero or infinite ratio on one axis. Each axis falls back
// to maxZoom independently rather than the pair producing a NaN that silently blanks the canvas.
//
// Free function, not a method, for the reason every other piece of geometry here is: it is pure
// arithmetic over numbers the editor happens to hold, so it can be checked without an ImGui context
// or a loaded file. See GraphEditorGeometryTest.
CanvasTransform frameTransform(Vec2 contentMin, Vec2 contentMax, Vec2 viewportPx,
                                f32 paddingPx, f32 minZoom, f32 maxZoom);

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

// ---------------------------------------------------------------------------------------------------
// Node attributes (extraTokens) -- Gap B: the editor's authoring surface for a node's key=value
// attributes (param=/field=/class=, and anything future).
// ---------------------------------------------------------------------------------------------------
//
// aver::fmt::OcGraphNode::extraTokens is a flat, order-preserving list of trailing NODE-line tokens
// this C++ reader does not interpret (see OcGraph.hpp's own comment on why it exists at all). Most of
// them are `key=value` attributes the C# side's OcGraphParser.cs reads -- param= naming a declared
// PARAM, field= naming a scene field, class= naming a spawnable class -- but the vector itself is
// untyped text, and nothing before this section ever read or wrote the key=value convention from the
// C++ side. These three functions are the ONE place that does, so the editor (and its tests) do not
// each reimplement the split and risk disagreeing with the C# reader's own contract
// (scripting/csharp/Aver.Graph/OcGraphParser.cs: `token.Split('=', 2)`) -- only the FIRST '=' in a
// token delimits key from value, so a value that itself contains '=' is preserved whole, not re-split.
// A token with no '=' at all is never touched by any of the three (there is no key to match), which is
// exactly what makes it survive an edit to a DIFFERENT attribute on the same node -- the entire point
// of extraTokens (see OcGraph.hpp) applied to this narrower, still-load-bearing case.
//
// FIRST MATCH WINS on read; a well-formed file carries at most one token per key (nothing in this
// format enforces that, so a hand-edited file COULD carry two, in which case getNodeAttribute reads
// the first and setNodeAttribute overwrites the first, leaving a stray second one alone -- a corrupt
// input's fault, not silently "fixed" by guessing which the author meant).

// One key=value attribute read from a node's extraTokens, or absent (found == false) if the node
// carries no token for that key.
struct GraphNodeAttribute {
    std::string key;
    std::string value;
    bool found = false;
};

// Reads the value of `key` from `node.extraTokens`. found == false, value == "" if absent.
GraphNodeAttribute getNodeAttribute(const fmt::OcGraphNode& node, const std::string& key);

// Sets `key=value` on `node.extraTokens`, IN PLACE where a `key=...` token already sits (so every
// OTHER token keeps its exact original order and content), or appended at the end if the key was not
// present before. This is the whole contract extraTokens exists for, applied to an edit: a token this
// editor build does not recognise -- matched key or not, `=`-shaped or not -- must survive an edit to
// a different attribute on the same node.
void setNodeAttribute(fmt::OcGraphNode& node, const std::string& key, const std::string& value);

// Removes the `key=...` token from `node.extraTokens`, if present; a no-op otherwise. Used when an
// author clears an attribute's field back to empty -- deleting the token outright (rather than writing
// `key=`) is the more truthful of the two ways to say "this was never set," and both round-trip
// safely through writeOcgraph regardless.
void removeNodeAttribute(fmt::OcGraphNode& node, const std::string& key);

// One row a details/inspector panel should draw for a selected node: either one of the attributes its
// TYPE declares (see GraphNodeDefs.hpp's GraphNodeDesc::attributes) -- shown even when `present` is
// false, so an author sees an attribute like `class` is expected before typing anything -- or a
// leftover `key=value`-shaped extraToken this build's catalog does not declare for that type, shown
// generically so it stays visible AND editable rather than opaque.
struct GraphAttributeRow {
    std::string key;
    std::string label;    // the catalog's friendly label for a declared row; equals `key` for a leftover row
    std::string value;    // current value; "" if `present` is false
    bool present = false; // whether node.extraTokens actually carries a `key=...` token right now
    bool declared = false; // true: one of `declaredKeyLabels` below; false: a leftover extraToken
};

// Builds the rows a details panel should show for `node`. `declaredKeyLabels` is a list of (key,
// label) pairs in display order -- deliberately NOT the GraphNodeDefs.hpp GraphNodeDesc type itself,
// the same reason computeNodeLayout above takes `title` as a plain string rather than looking it up:
// this function's whole point is not needing to know the catalog exists, so a headless test (or a
// future caller with a different node-type table) can call it with any list of keys it likes.
//
// Order: one row per entry in `declaredKeyLabels`, in that order, ALWAYS present (whether or not the
// node currently carries a token for it) -- then one row per remaining extraToken that IS `key=value`
// shaped but whose key is not one of `declaredKeyLabels`, in the node's own file order. A token with
// no '=' at all produces no row (there is nothing to label it with), and is therefore never touched by
// an edit made through these rows -- it survives precisely because nothing here ever looks at it.
std::vector<GraphAttributeRow> computeAttributeRows(
    const fmt::OcGraphNode& node, const std::vector<std::pair<std::string, std::string>>& declaredKeyLabels);

} // namespace aver::editor
