#pragma once
// Modeling mode viewport picking, as pure math (WAVE2_UE W5 step 6): the edit mesh projected to the screen once, then a click picks the nearest
// vertex, the nearest edge or the front-most face under the cursor, and a marquee collects the vertices, edges or faces inside a rectangle. No ImGui,
// no scene, no window: the caller supplies a projector (world point to pixels plus a depth to order by), so a test feeds a plain camera matrix.
//
// Selection by click or marquee is not occlusion-tested: a back vertex of a box is picked as readily as a front one (a click prefers the nearer one
// when two sit under the cursor; a marquee takes everything inside the rectangle, the "select through" behaviour). The marquee rectangle is
// normalised by ViewportMarquee.hpp's helper, the same one the object marquee uses.
#include "ModelingModel.hpp"
#include "ViewportMarquee.hpp"

#include <cmath>
#include <functional>
#include <set>
#include <vector>

namespace aver::editor::modeling {

struct ScreenVert {
    f32 x = 0.0f, y = 0.0f;
    f32 depth = 0.0f;       // larger = farther
    bool ok = false;        // false: behind the camera
};

// Mesh-local point -> pixels and depth. False when the point cannot be shown (behind the camera).
using Projector = std::function<bool(const em::V3& p, f32& sx, f32& sy, f32& depth)>;

struct Rect {
    f32 loX = 0, loY = 0, hiX = 0, hiY = 0;
    bool contains(f32 x, f32 y) const { return x >= loX && x <= hiX && y >= loY && y <= hiY; }
};
inline Rect rectOf(f32 x0, f32 y0, f32 x1, f32 y1) {
    Rect r;
    normalizeMarqueeRect(x0, y0, x1, y1, r.loX, r.loY, r.hiX, r.hiY);
    return r;
}

inline std::vector<ScreenVert> projectMesh(const em::EditMesh& m, const Projector& proj) {
    std::vector<ScreenVert> out(m.pos.size());
    for (usize i = 0; i < m.pos.size(); ++i) {
        ScreenVert s;
        s.ok = proj(m.pos[i], s.x, s.y, s.depth);
        out[i] = s;
    }
    return out;
}

// Vertices that a face uses (the rest are orphans the editor does not show), ascending.
inline std::vector<u32> usedVertexList(const em::EditMesh& m) {
    std::set<u32> s;
    for (const em::Face& f : m.faces) for (const u32 v : f.v) if (v < m.pos.size()) s.insert(v);
    return {s.begin(), s.end()};
}

// Every distinct edge of every face, normalised, ascending.
inline std::vector<em::Edge> edgeList(const em::EditMesh& m) {
    std::set<em::Edge> s;
    for (const em::Face& f : m.faces)
        for (usize c = 0; c < f.v.size(); ++c) {
            const u32 a = f.v[c], b = f.v[(c + 1) % f.v.size()];
            if (a != b && a < m.pos.size() && b < m.pos.size()) s.insert(em::makeEdge(a, b));
        }
    return {s.begin(), s.end()};
}

inline f32 distToSegment(f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by) {
    const f32 vx = bx - ax, vy = by - ay, wx = px - ax, wy = py - ay;
    const f32 len2 = vx * vx + vy * vy;
    f32 t = len2 > 1e-9f ? (wx * vx + wy * vy) / len2 : 0.0f;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const f32 cx = ax + vx * t, cy = ay + vy * t;
    return std::sqrt((px - cx) * (px - cx) + (py - cy) * (py - cy));
}

// The used vertex nearest the cursor within `radiusPx`; a tie within half a pixel goes to the nearer to the camera. -1 for none.
inline int nearestVertex(const em::EditMesh& m, const std::vector<ScreenVert>& sv, f32 mx, f32 my, f32 radiusPx) {
    int best = -1;
    f32 bestD = radiusPx, bestDepth = 0.0f;
    for (const u32 v : usedVertexList(m)) {
        if (v >= sv.size() || !sv[v].ok) continue;
        const f32 d = std::sqrt((sv[v].x - mx) * (sv[v].x - mx) + (sv[v].y - my) * (sv[v].y - my));
        if (d > radiusPx) continue;
        if (best < 0 || d < bestD - 0.5f || (std::fabs(d - bestD) <= 0.5f && sv[v].depth < bestDepth)) { best = static_cast<int>(v); bestD = d; bestDepth = sv[v].depth; }
    }
    return best;
}

// The edge nearest the cursor within `radiusPx` (both ends must be on screen).
inline bool nearestEdge(const em::EditMesh& m, const std::vector<ScreenVert>& sv, f32 mx, f32 my, f32 radiusPx, em::Edge& out) {
    bool found = false;
    f32 bestD = radiusPx, bestDepth = 0.0f;
    for (const em::Edge& e : edgeList(m)) {
        if (e.first >= sv.size() || e.second >= sv.size() || !sv[e.first].ok || !sv[e.second].ok) continue;
        const f32 d = distToSegment(mx, my, sv[e.first].x, sv[e.first].y, sv[e.second].x, sv[e.second].y);
        const f32 depth = 0.5f * (sv[e.first].depth + sv[e.second].depth);
        if (d > radiusPx) continue;
        if (!found || d < bestD - 0.5f || (std::fabs(d - bestD) <= 0.5f && depth < bestDepth)) { found = true; bestD = d; bestDepth = depth; out = e; }
    }
    return found;
}

inline bool pointInTriangle(f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by, f32 cx, f32 cy) {
    const f32 d1 = (px - bx) * (ay - by) - (ax - bx) * (py - by);
    const f32 d2 = (px - cx) * (by - cy) - (bx - cx) * (py - cy);
    const f32 d3 = (px - ax) * (cy - ay) - (cx - ax) * (py - ay);
    const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
    return !(neg && pos);
}

// The face under the cursor, nearest to the camera by the mean depth of its corners (the polygon is tested as a fan from its first corner). -1 for none.
inline int faceAt(const em::EditMesh& m, const std::vector<ScreenVert>& sv, f32 mx, f32 my) {
    int best = -1;
    f32 bestDepth = 0.0f;
    for (u32 fi = 0; fi < m.faces.size(); ++fi) {
        const em::Face& f = m.faces[fi];
        if (f.v.size() < 3) continue;
        bool ok = true;
        f32 depth = 0.0f;
        for (const u32 v : f.v) { ok = ok && v < sv.size() && sv[v].ok; if (ok) depth += sv[v].depth; }
        if (!ok) continue;
        depth /= static_cast<f32>(f.v.size());
        bool inside = false;
        for (usize c = 1; c + 1 < f.v.size() && !inside; ++c) {
            const ScreenVert &a = sv[f.v[0]], &b = sv[f.v[c]], &d = sv[f.v[c + 1]];
            inside = pointInTriangle(mx, my, a.x, a.y, b.x, b.y, d.x, d.y);
        }
        if (inside && (best < 0 || depth < bestDepth)) { best = static_cast<int>(fi); bestDepth = depth; }
    }
    return best;
}

inline std::vector<u32> verticesIn(const em::EditMesh& m, const std::vector<ScreenVert>& sv, const Rect& r) {
    std::vector<u32> out;
    for (const u32 v : usedVertexList(m)) if (v < sv.size() && sv[v].ok && r.contains(sv[v].x, sv[v].y)) out.push_back(v);
    return out;
}
// Edges with both ends inside.
inline std::vector<em::Edge> edgesIn(const em::EditMesh& m, const std::vector<ScreenVert>& sv, const Rect& r) {
    std::vector<em::Edge> out;
    for (const em::Edge& e : edgeList(m))
        if (e.first < sv.size() && e.second < sv.size() && sv[e.first].ok && sv[e.second].ok && r.contains(sv[e.first].x, sv[e.first].y) && r.contains(sv[e.second].x, sv[e.second].y))
            out.push_back(e);
    return out;
}
// Faces whose every corner is inside.
inline std::vector<u32> facesIn(const em::EditMesh& m, const std::vector<ScreenVert>& sv, const Rect& r) {
    std::vector<u32> out;
    for (u32 fi = 0; fi < m.faces.size(); ++fi) {
        const em::Face& f = m.faces[fi];
        bool all = !f.v.empty();
        for (usize c = 0; c < f.v.size() && all; ++c) all = f.v[c] < sv.size() && sv[f.v[c]].ok && r.contains(sv[f.v[c]].x, sv[f.v[c]].y);
        if (all) out.push_back(fi);
    }
    return out;
}

// ---- onto the model ----
// A click in the model's current select mode. `additive` toggles the element in the selection; otherwise it replaces it (a click on nothing clears).
// True when the selection changed.
inline bool applyClick(ModelingModel& model, const std::vector<ScreenVert>& sv, f32 mx, f32 my, f32 radiusPx, bool additive) {
    const Selection before = model.doc.sel;
    const em::EditMesh& m = model.doc.mesh;
    bool hit = false;
    switch (model.mode()) {
        case SelectMode::Vertex: { const int v = nearestVertex(m, sv, mx, my, radiusPx); if (v >= 0) { model.pickVertex(static_cast<u32>(v), additive); hit = true; } break; }
        case SelectMode::Edge: { em::Edge e; if (nearestEdge(m, sv, mx, my, radiusPx, e)) { model.pickEdge(e.first, e.second, additive); hit = true; } break; }
        case SelectMode::Face: { const int f = faceAt(m, sv, mx, my); if (f >= 0) { model.pickFace(static_cast<u32>(f), additive); hit = true; } break; }
    }
    if (!hit && !additive) model.clearSelection();
    return !(before.verts == model.doc.sel.verts && before.edges == model.doc.sel.edges && before.faces == model.doc.sel.faces);
}

// A marquee in the current select mode: replaces the selection, or adds to it when `additive`. True when it changed.
inline bool applyMarquee(ModelingModel& model, const std::vector<ScreenVert>& sv, const Rect& r, bool additive) {
    const Selection before = model.doc.sel;
    const em::EditMesh& m = model.doc.mesh;
    if (!additive) model.clearSelection();
    switch (model.mode()) {
        case SelectMode::Vertex: for (const u32 v : verticesIn(m, sv, r)) model.doc.sel.verts.insert(v); break;
        case SelectMode::Edge: for (const em::Edge& e : edgesIn(m, sv, r)) model.doc.sel.edges.insert(e); break;
        case SelectMode::Face: for (const u32 f : facesIn(m, sv, r)) model.doc.sel.faces.insert(f); break;
    }
    return !(before.verts == model.doc.sel.verts && before.edges == model.doc.sel.edges && before.faces == model.doc.sel.faces);
}

}  // namespace aver::editor::modeling
