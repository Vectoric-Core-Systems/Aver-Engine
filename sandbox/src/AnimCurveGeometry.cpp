// The animation curve editor's headless core -- see AnimCurveGeometry.hpp for what this is and why
// it is ImGui-free.
#include "AnimCurveGeometry.hpp"

#include <cmath>

namespace aver::editor {
namespace {

f32 lerpf(f32 a, f32 b, f32 t) { return a + (b - a) * t; }

// Squared-distance hit test, so the caller never pays for a sqrt it only compares.
bool within(Vec2 a, Vec2 b, f32 r) {
    const f32 dx = a.x - b.x, dy = a.y - b.y;
    return dx * dx + dy * dy <= r * r;
}

} // namespace

Vec2 curveToScreen(const CurveView& v, f32 t, f32 value) {
    const f32 tSpan = v.tMax - v.tMin;
    const f32 vSpan = v.vMax - v.vMin;
    const f32 u = tSpan > 1e-9f ? (t - v.tMin) / tSpan : 0.0f;
    const f32 w = vSpan > 1e-9f ? (value - v.vMin) / vSpan : 0.0f;
    const f32 x = lerpf(v.rectMin.x, v.rectMax.x, u);
    // Y INVERTED: u=0 (vMin) draws at rectMax.y (the BOTTOM of the widget), u=1 (vMax) at rectMin.y.
    const f32 y = lerpf(v.rectMax.y, v.rectMin.y, w);
    return Vec2{x, y};
}

void screenToCurve(const CurveView& v, Vec2 screenPt, f32& outTime, f32& outValue) {
    const f32 xSpan = v.rectMax.x - v.rectMin.x;
    const f32 ySpan = v.rectMax.y - v.rectMin.y;   // POSITIVE: rectMax.y is the bottom of the widget
    const f32 u = std::fabs(xSpan) > 1e-9f ? (screenPt.x - v.rectMin.x) / xSpan : 0.0f;
    // The exact reverse of curveToScreen's w -> y step above.
    const f32 w = std::fabs(ySpan) > 1e-9f ? (v.rectMax.y - screenPt.y) / ySpan : 0.0f;
    outTime  = lerpf(v.tMin, v.tMax, u);
    outValue = lerpf(v.vMin, v.vMax, w);
}

std::vector<CurveKeyLayout> computeCurveLayout(const fmt::OcCurve& c, const CurveView& view, f32 handleSeconds) {
    std::vector<CurveKeyLayout> out;
    const usize n = c.times.size();
    if (c.values.size() != n) return out;   // malformed curve; nothing to lay out safely
    out.reserve(n);

    const bool hasTangents = c.inTangents.size() == n && c.outTangents.size() == n;
    for (usize i = 0; i < n; ++i) {
        CurveKeyLayout k;
        k.index = i;
        k.keyScreen = curveToScreen(view, c.times[i], c.values[i]);
        k.hasTangents = hasTangents;
        if (hasTangents) {
            // OUT: a point handleSeconds AFTER this key, at the value its out-tangent slope predicts
            // there -- the same "value + slope*dt" relationship hermite()'s own b0 term uses.
            k.outHandleScreen = curveToScreen(view, c.times[i] + handleSeconds,
                                               c.values[i] + c.outTangents[i] * handleSeconds);
            // IN: the mirror image, a point handleSeconds BEFORE this key, at the value its in-tangent
            // slope predicts arriving FROM.
            k.inHandleScreen = curveToScreen(view, c.times[i] - handleSeconds,
                                              c.values[i] - c.inTangents[i] * handleSeconds);
        }
        out.push_back(k);
    }
    return out;
}

CurveHitResult curveHitTest(const std::vector<CurveKeyLayout>& layout, Vec2 screenPt, f32 hitRadiusPx) {
    for (const CurveKeyLayout& k : layout)
        if (within(k.keyScreen, screenPt, hitRadiusPx)) return CurveHitResult{CurveHitKind::Key, k.index};
    for (const CurveKeyLayout& k : layout) {
        if (!k.hasTangents) continue;
        if (within(k.inHandleScreen, screenPt, hitRadiusPx))  return CurveHitResult{CurveHitKind::InHandle, k.index};
        if (within(k.outHandleScreen, screenPt, hitRadiusPx)) return CurveHitResult{CurveHitKind::OutHandle, k.index};
    }
    return CurveHitResult{};
}

f32 tangentSlopeFromHandle(const CurveView& view, Vec2 keyScreen, Vec2 handleScreen, bool isOutHandle, f32 minSeconds) {
    f32 keyTime = 0.0f, keyValue = 0.0f, handleTime = 0.0f, handleValue = 0.0f;
    screenToCurve(view, keyScreen, keyTime, keyValue);
    screenToCurve(view, handleScreen, handleTime, handleValue);

    // dt is POSITIVE for an out-handle (which sits to the right of its key) and NEGATIVE for an
    // in-handle (which sits to the left) -- floored away from zero IN THAT SAME DIRECTION, so a
    // handle dragged onto or past its own key still yields a slope rather than a divide-by-zero or a
    // flip to the wrong side of the key.
    f32 dt = handleTime - keyTime;
    const f32 floor = std::fabs(minSeconds);
    if (isOutHandle) { if (dt < floor) dt = floor; }
    else             { if (dt > -floor) dt = -floor; }

    const f32 dv = handleValue - keyValue;
    return dv / dt;
}

} // namespace aver::editor
