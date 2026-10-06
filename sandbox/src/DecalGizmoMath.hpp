#pragma once
// The decal box gizmo's arithmetic, apart from any device or ImGui so tests can run it: the projector
// box in world space, a ray-vs-box pick (decals draw nothing a mesh pick could hit), the twelve edges
// and projection arrow as line segments, face-handle picking, and the symmetric handle drag that
// resizes CDecal::sizeCm. docs/rendering/DECALS.md.
#include "aver/core/Math.hpp"
#include "aver/scene/Components.hpp"
#include "aver/scene/DecalGather.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

namespace aver::editor {

// A decal's box in world space: centre, unit axes (the projector's X/Y/Z) and half extents in cm.
struct DecalBox {
    Vec3 centre{0, 0, 0};
    Vec3 axis[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    Vec3 half{50, 50, 50};
    f32  scale[3] = {1, 1, 1};   // the matrix's per-axis scale, to turn world sizes back into sizeCm
};

inline DecalBox decalBoxOf(const scene::CDecal& c, const Mat4& world) {
    DecalBox b;
    b.centre = Vec3{world.m[3][0], world.m[3][1], world.m[3][2]};
    f32* h = &b.half.x;
    for (int i = 0; i < 3; ++i) {
        const Vec3 row{world.m[i][0], world.m[i][1], world.m[i][2]};
        const f32 len = row.size();
        b.scale[i] = len > 1e-6f ? len : 1.0f;
        b.axis[i] = len > 1e-6f ? row / len : b.axis[i];
        h[i] = 0.5f * scene::decalSizeCm(c, i) * b.scale[i];
    }
    return b;
}

// Nearest entry of the ray into the box (slab test in box space). False for a miss or a box behind.
inline bool rayHitsDecalBox(const Vec3& o, const Vec3& d, const DecalBox& b, f32& tHit) {
    const Vec3 rel = o - b.centre;
    const f32* h = &b.half.x;
    f32 tMin = 0.0f, tMax = 1e30f;
    for (int i = 0; i < 3; ++i) {
        const f32 po = dot(rel, b.axis[i]);
        const f32 pd = dot(d, b.axis[i]);
        if (std::fabs(pd) < 1e-8f) {
            if (std::fabs(po) > h[i]) return false;
            continue;
        }
        f32 t0 = (-h[i] - po) / pd, t1 = (h[i] - po) / pd;
        if (t0 > t1) std::swap(t0, t1);
        tMin = std::max(tMin, t0);
        tMax = std::min(tMax, t1);
        if (tMin > tMax) return false;
    }
    tHit = tMin;
    return true;
}

inline Vec3 decalCorner(const DecalBox& b, int sx, int sy, int sz) {
    return b.centre + b.axis[0] * (b.half.x * static_cast<f32>(sx)) + b.axis[1] * (b.half.y * static_cast<f32>(sy)) +
           b.axis[2] * (b.half.z * static_cast<f32>(sz));
}

// Line-list vertex pairs: the twelve box edges, an arrow from the centre along the projection (+X)
// with a head, and a cross on the projector (back, -X) face so the direction reads at a glance.
inline void decalBoxSegments(const DecalBox& b, std::vector<Vec3>& out) {
    auto seg = [&](const Vec3& a, const Vec3& c) { out.push_back(a); out.push_back(c); };
    for (int sy = -1; sy <= 1; sy += 2)
        for (int sz = -1; sz <= 1; sz += 2) seg(decalCorner(b, -1, sy, sz), decalCorner(b, 1, sy, sz));
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2) seg(decalCorner(b, sx, -1, sz), decalCorner(b, sx, 1, sz));
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2) seg(decalCorner(b, sx, sy, -1), decalCorner(b, sx, sy, 1));
    const Vec3 tip = b.centre + b.axis[0] * b.half.x;
    const f32 head = 0.18f * std::min(b.half.y, b.half.z);
    seg(b.centre, tip);
    seg(tip, tip - b.axis[0] * head + b.axis[1] * head);
    seg(tip, tip - b.axis[0] * head - b.axis[1] * head);
    seg(tip, tip - b.axis[0] * head + b.axis[2] * head);
    seg(tip, tip - b.axis[0] * head - b.axis[2] * head);
    seg(decalCorner(b, -1, -1, -1), decalCorner(b, -1, 1, 1));
    seg(decalCorner(b, -1, -1, 1), decalCorner(b, -1, 1, -1));
}

// The six face handles: axis 0..2, sign -1/+1.
struct DecalHandle {
    int axis = -1;
    int sign = 0;
    bool valid() const { return axis >= 0; }
};

inline Vec3 decalHandlePos(const DecalBox& b, const DecalHandle& h) {
    const f32* half = &b.half.x;
    return b.centre + b.axis[h.axis] * (half[h.axis] * static_cast<f32>(h.sign));
}

// The handle whose pick sphere (`radius` world cm) the ray passes through, nearest first.
inline DecalHandle pickDecalHandle(const DecalBox& b, const Vec3& o, const Vec3& d, f32 radius) {
    DecalHandle best;
    f32 bestT = 1e30f;
    for (int axis = 0; axis < 3; ++axis) {
        for (int sign = -1; sign <= 1; sign += 2) {
            const DecalHandle h{axis, sign};
            const Vec3 p = decalHandlePos(b, h);
            const f32 t = dot(p - o, d);
            if (t <= 0.0f) continue;
            if ((o + d * t - p).sizeSquared() <= radius * radius && t < bestT) { bestT = t; best = h; }
        }
    }
    return best;
}

// Where along the box axis `axis` (signed cm from the centre) the ray passes closest to that axis'
// line. False when the ray runs parallel to it.
inline bool decalAxisParameter(const DecalBox& b, int axis, const Vec3& o, const Vec3& d, f32& s) {
    const Vec3& a = b.axis[axis];
    const f32 bb = dot(a, d);
    const f32 denom = 1.0f - bb * bb;
    if (denom < 1e-6f) return false;
    const Vec3 w = b.centre - o;
    s = (bb * dot(d, w) - dot(a, w)) / denom;
    return true;
}

// Dragging a face handle to the ray's closest point on its axis sets that axis' full size to twice
// the distance, so the box stays centred. Returns the new CDecal::sizeCm for the axis, clamped to
// [1, 100000] cm, or a negative number when the ray gives no answer.
inline f32 dragDecalHandleSize(const DecalBox& b, const DecalHandle& h, const Vec3& o, const Vec3& d) {
    f32 s = 0.0f;
    if (!h.valid() || !decalAxisParameter(b, h.axis, o, d, s)) return -1.0f;
    const f32 halfWorld = std::fabs(s);
    const f32 sizeCm = 2.0f * halfWorld / b.scale[h.axis];
    return std::min(std::max(sizeCm, 1.0f), 100000.0f);
}

} // namespace aver::editor
