#pragma once
// The particle editor's curve graph and gradient bar, with no ImGui: the visible window of a graph (zoom, pan, framing), the
// mapping between a graph point and a screen pixel, key picking, and the key edits the mouse makes. The curves are the runtime's
// Curve4 (2 to 4 piecewise-linear stops, so there are no tangents to edit); every edit goes through the clamped functions in
// ParticleStackModel.hpp, so a dragged key can never be one the parser would refuse (strictly increasing times in [0, 1]).
#include "ParticleStackModel.hpp"

#include <algorithm>
#include <cmath>

namespace aver::editor::pxmodel {

// The part of the (time, value) plane the graph shows.
struct CurveView {
    f32 t0 = -0.05f, t1 = 1.05f;
    f32 v0 = -0.1f, v1 = 1.1f;
};
// A screen rectangle in pixels, y growing downward.
struct ScreenRect {
    f32 x0 = 0, y0 = 0, x1 = 1, y1 = 1;
    f32 w() const { return x1 - x0; }
    f32 h() const { return y1 - y0; }
};

inline f32 viewToX(const CurveView& v, const ScreenRect& r, f32 t) { return r.x0 + (t - v.t0) / (v.t1 - v.t0) * r.w(); }
inline f32 viewToY(const CurveView& v, const ScreenRect& r, f32 val) { return r.y1 - (val - v.v0) / (v.v1 - v.v0) * r.h(); }
inline f32 xToView(const CurveView& v, const ScreenRect& r, f32 x) { return v.t0 + (x - r.x0) / r.w() * (v.t1 - v.t0); }
inline f32 yToView(const CurveView& v, const ScreenRect& r, f32 y) { return v.v0 + (r.y1 - y) / r.h() * (v.v1 - v.v0); }

// Fits the view around the stops of the given channels (bit i of `channels` = channel i), with a margin, and keeps time 0..1 in sight.
inline CurveView frameCurve(const Curve4& c, u32 channels) {
    f32 lo = 0.0f, hi = 1.0f;
    for (u32 i = 0; i < c.n; ++i)
        for (u32 k = 0; k < 4; ++k)
            if (channels & (1u << k)) { lo = std::min(lo, c.v[i][k]); hi = std::max(hi, c.v[i][k]); }
    if (hi - lo < 1e-3f) hi = lo + 1.0f;
    const f32 pad = (hi - lo) * 0.1f;
    CurveView v;
    v.v0 = lo - pad;
    v.v1 = hi + pad;
    return v;
}

// Zooms about a graph point: factor < 1 zooms in. Each axis can be zoomed alone. The span never gets smaller than 1e-3 or larger than 1e5.
inline void zoomView(CurveView& v, f32 anchorT, f32 anchorV, f32 factorT, f32 factorV) {
    const auto axis = [](f32& a, f32& b, f32 anchor, f32 f) {
        f = std::clamp(f, 0.05f, 20.0f);
        f32 na = anchor + (a - anchor) * f, nb = anchor + (b - anchor) * f;
        if (nb - na < 1e-3f || nb - na > 1e5f) return;
        a = na; b = nb;
    };
    axis(v.t0, v.t1, anchorT, factorT);
    axis(v.v0, v.v1, anchorV, factorV);
}

// Slides the window by a mouse delta in pixels (the content follows the mouse).
inline void panView(CurveView& v, const ScreenRect& r, f32 dxPx, f32 dyPx) {
    const f32 dt = -dxPx / r.w() * (v.t1 - v.t0);
    const f32 dv = dyPx / r.h() * (v.v1 - v.v0);
    v.t0 += dt; v.t1 += dt;
    v.v0 += dv; v.v1 += dv;
}

// The stop nearest to a pixel within `radiusPx`, or -1. `channel` is the value the graph plots for the stop.
inline int pickKey(const Curve4& c, int channel, const CurveView& v, const ScreenRect& r, f32 mx, f32 my, f32 radiusPx) {
    int best = -1;
    f32 bestD = radiusPx * radiusPx;
    for (u32 i = 0; i < c.n; ++i) {
        const f32 dx = viewToX(v, r, c.t[i]) - mx, dy = viewToY(v, r, c.v[i][channel & 3]) - my;
        const f32 d = dx * dx + dy * dy;
        if (d <= bestD) { bestD = d; best = static_cast<int>(i); }
    }
    return best;
}

// Moves one stop to a graph point, editing only `channel` of its value. Time is clamped between the neighbours; a size is >= 0, an
// alpha is in [0, 1] and a colour channel is >= 0. Returns whether the curve changed.
inline bool moveKey(Curve4& c, int idx, int channel, f32 t, f32 value, bool isColor) {
    if (idx < 0 || static_cast<u32>(idx) >= c.n || !std::isfinite(t) || !std::isfinite(value)) return false;
    channel &= 3;
    f32 v[4] = {c.v[idx][0], c.v[idx][1], c.v[idx][2], c.v[idx][3]};
    v[channel] = isColor ? (channel == 3 ? std::clamp(value, 0.0f, 1.0f) : std::max(0.0f, value)) : std::max(0.0f, value);
    return setCurvePoint(c, idx, t, v);
}

// Adds a stop at time `t` that sits on the curve's current line except for `channel`, which takes `value`. Fails at four stops or on
// top of an existing one.
inline bool insertKey(Curve4& c, int channel, f32 t, f32 value, bool isColor) {
    if (c.n < 2 || c.n >= 4) return false;
    f32 v[4];
    if (!particles::evalCurve(c, std::clamp(t, 0.0f, 1.0f), v)) return false;
    channel &= 3;
    v[channel] = isColor ? (channel == 3 ? std::clamp(value, 0.0f, 1.0f) : std::max(0.0f, value)) : std::max(0.0f, value);
    return addCurvePoint(c, t, v);
}

// ---- the gradient bar: stops along 0..1 in x, the colour fixed ----

// The stop under a pixel x within `radiusPx`, or -1.
inline int gradientPick(const Curve4& c, f32 barX0, f32 barX1, f32 mx, f32 radiusPx) {
    int best = -1;
    f32 bestD = radiusPx;
    for (u32 i = 0; i < c.n; ++i) {
        const f32 d = std::fabs(barX0 + c.t[i] * (barX1 - barX0) - mx);
        if (d <= bestD) { bestD = d; best = static_cast<int>(i); }
    }
    return best;
}
inline f32 gradientTimeAt(f32 barX0, f32 barX1, f32 mx) { return std::clamp((mx - barX0) / std::max(1e-3f, barX1 - barX0), 0.0f, 1.0f); }

// Moves a stop along the bar, keeping its colour.
inline bool gradientMoveStop(Curve4& c, int idx, f32 t) {
    if (idx < 0 || static_cast<u32>(idx) >= c.n) return false;
    f32 v[4] = {c.v[idx][0], c.v[idx][1], c.v[idx][2], c.v[idx][3]};
    return setCurvePoint(c, idx, t, v);
}
// Adds a stop at `t` with the colour the gradient has there.
inline bool gradientInsertStop(Curve4& c, f32 t) {
    if (c.n < 2 || c.n >= 4) return false;
    f32 v[4];
    if (!particles::evalCurve(c, std::clamp(t, 0.0f, 1.0f), v)) return false;
    return addCurvePoint(c, t, v);
}
// Recolours a stop.
inline bool gradientSetStopColor(Curve4& c, int idx, const f32 rgba[4]) {
    if (idx < 0 || static_cast<u32>(idx) >= c.n) return false;
    f32 v[4] = {std::max(0.0f, rgba[0]), std::max(0.0f, rgba[1]), std::max(0.0f, rgba[2]), std::clamp(rgba[3], 0.0f, 1.0f)};
    return setCurvePoint(c, idx, c.t[idx], v);
}

} // namespace aver::editor::pxmodel
