#pragma once
// The animation curve editor's headless core: screen<->curve mapping, key/tangent-handle layout, and
// hit-testing for the 2D curve widget AnimEditor.cpp's drawCurves() draws. Deliberately ImGui-free and
// Engine-free, the same reason GraphEditorGeometry.hpp is (see its own header comment) -- so it
// compiles and runs with no window and no GPU: see tests/editor/src/AnimCurveGeometryTest.cpp, which
// links exactly this file plus Aver.Formats.
//
// THE DRAG ITSELF IS VISUAL-ONLY -- moving the mouse, updating ImGui's per-frame state, drawing the
// line under the cursor -- and untestable outside a real window; see AnimEditor::drawCurves()'s own
// comment on that half. What lives here is everything that decides WHERE a key or a tangent handle
// sits on screen and WHICH one a given click landed on, which is arithmetic and needs neither.
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcAnim.hpp"

#include <vector>

namespace aver::editor {

// The screen-space rectangle a curve widget draws into, and the curve-space WINDOW mapped onto it:
// time in [tMin, tMax] maps to x in [rectMin.x, rectMax.x]; value in [vMin, vMax] maps to y in
// [rectMax.y, rectMin.y] -- Y INVERTED, because screen space grows down and a curve's value should
// not. This gives a name and a pair of pure, invertible functions to the same lo/hi/top/bot
// arithmetic AnimEditor::drawTimeline's existing curve-shape overlay already computes inline for its
// one-way sampling loop.
struct CurveView {
    Vec2 rectMin{0.0f, 0.0f};
    Vec2 rectMax{1.0f, 1.0f};
    f32  tMin = 0.0f, tMax = 1.0f;
    f32  vMin = 0.0f, vMax = 1.0f;
};

// (time, value) -> screen. A degenerate window (tMax<=tMin, or vMax<=vMin) maps to u=0/w=0 rather
// than dividing by zero -- the same defensive shape GraphEditorGeometry.hpp's frameTransform uses for
// a degenerate content rectangle.
Vec2 curveToScreen(const CurveView& v, f32 t, f32 value);

// screen -> (time, value), the exact inverse of curveToScreen for a non-degenerate window. Used by
// drag handling (a screen point the mouse is at, translated back into curve space) and by this file's
// own round-trip test.
void screenToCurve(const CurveView& v, Vec2 screenPt, f32& outTime, f32& outValue);

// One key's screen geometry: its own point, and -- only when the curve carries a matching tangent
// pair (see OcCurve::inTangents/outTangents) -- its two handle points. Handles are placed at a FIXED
// TIME OFFSET from the key, not a fixed pixel distance, so a handle's on-screen length shrinks and
// grows with the view exactly like the curve it belongs to, rather than staying a constant number of
// pixels while the timeline underneath it stretches.
struct CurveKeyLayout {
    usize index = 0;           // into OcCurve::times/values
    Vec2  keyScreen{};
    bool  hasTangents = false; // true only when inTangents/outTangents both match c.times.size()
    Vec2  inHandleScreen{};    // valid only when hasTangents; the ARRIVING tangent, left of the key
    Vec2  outHandleScreen{};   // valid only when hasTangents; the LEAVING tangent, right of the key
};

// Lays out every key (and, where present, its two handles) of `c` against `view`. Pure and
// deterministic: the same curve, view and handleSeconds always produce the same layout, which is what
// makes this reachable from a test with no ImGui and no mouse. Returns empty for a malformed curve
// (values.size() != times.size()) rather than laying out a mismatched pair.
std::vector<CurveKeyLayout> computeCurveLayout(const fmt::OcCurve& c, const CurveView& view,
                                                 f32 handleSeconds);

// What a click landed on.
enum class CurveHitKind { None, Key, InHandle, OutHandle };
struct CurveHitResult {
    CurveHitKind kind = CurveHitKind::None;
    usize        keyIndex = 0; // valid unless kind == None
};

// Tests `screenPt` against every key's point, then (if it carries handles) its two handle points,
// each within `hitRadiusPx` -- keys and handles are the smallest targets on the widget, so they are
// checked before anything else would be. Keys are tested in full before any handle: a curve's keys
// never overlap on the time axis the way two node bodies can in the graph editor, so "first match"
// needs no z-order tie-break the way GraphEditorGeometry::hitTest's node/pin pass does.
CurveHitResult curveHitTest(const std::vector<CurveKeyLayout>& layout, Vec2 screenPt, f32 hitRadiusPx);

// The IN or OUT TANGENT SLOPE (value units per second -- exactly what OcCurve::inTangents/outTangents
// stores, and what AnimSampler.cpp's hermite() multiplies by a key span to get the tangent TERM) that
// dragging a handle to `handleScreen` implies, given the key's own screen position. This is
// computeCurveLayout's inverse for one handle: lay a key and a slope out to get a handle position;
// drag a handle to a new position to get a slope back.
//
// `minSeconds` floors the effective time offset away from zero, IN THE DIRECTION the handle is
// supposed to be on (positive for an out-handle, negative for an in-handle) -- so a handle dragged
// onto or past its own key cannot divide by zero or flip which side of the key it represents. Same
// defensive floor AnimSampler.cpp's sampleCurve applies to a zero-length key span.
f32 tangentSlopeFromHandle(const CurveView& view, Vec2 keyScreen, Vec2 handleScreen,
                             bool isOutHandle, f32 minSeconds = 1.0f / 256.0f);

} // namespace aver::editor
