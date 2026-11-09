#pragma once
// The pure half of the Modeling mode (docs/plans/WAVE2_UE.md W5): which mesh is being authored, what is selected (vertices, edges or faces), the tool
// parameters, and every command the panels and hotkeys will issue -- primitives, move / rotate / scale of the selection, extrude, inset, bevel, delete, weld,
// flip, subdivide, mirror, boolean against another mesh, UV projection, save and reopen. Header-only and ImGui-free over fmt::em::EditMesh so tests/editor can
// drive all of it. Undo is SnapshotUndo of the whole document (mesh + selection), fine at the basic scale the mode targets.
// A command that refuses leaves the document and the undo history alone and puts the reason in status().
#include "SnapshotUndo.hpp"

#include "aver/formats/EditMesh.hpp"

#include <algorithm>
#include <set>
#include <string>
#include <vector>

namespace aver::editor::modeling {

namespace em = fmt::em;

enum class SelectMode { Vertex, Edge, Face };
enum class Primitive { Box, Sphere, Cylinder, Cone, Torus, Plane, Stairs, Arch };
enum class PivotMode { SelectionCentre, WorldOrigin };

// Only the set of the active mode is filled; switching modes converts it.
struct Selection {
    std::set<u32> verts;
    std::set<em::Edge> edges;
    std::set<u32> faces;
    bool empty() const { return verts.empty() && edges.empty() && faces.empty(); }
};

struct Doc {
    em::EditMesh mesh;
    Selection sel;
    SelectMode mode = SelectMode::Face;
};

// What the numeric fields of the tool panels hold. Lengths are centimetres.
struct ToolParams {
    double extrudeDistance = 20.0;
    double insetAmount = 5.0;
    double bevelWidth = 5.0;
    int bevelSegments = 1;
    double weldTolerance = 0.01;
    int mirrorAxis = 0;                 // 0 x, 1 y, 2 z
    double mirrorAt = 0.0;
    double uvTile = 100.0;
    double textureSize = 512.0;         // for the texel-density readout
    PivotMode pivot = PivotMode::SelectionCentre;
    em::PrimitiveOptions primitive;     // pivot at base, UV tile, material
};

class ModelingModel {
public:
    Doc doc;
    ToolParams params;

    // ---- the document ----
    const std::string& status() const { return status_; }
    bool dirty() const { return dirty_; }
    const std::string& path() const { return path_; }
    bool reopenedAsTriangles() const { return triangles_; }
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // A new document holding a primitive (sizes in cm). Resets history and the path.
    void newPrimitive(Primitive p, const em::V3& size = {100, 100, 100}, int detail = 16) {
        const em::PrimitiveOptions& o = params.primitive;
        em::EditMesh m;
        switch (p) {
        case Primitive::Box: m = em::makeBox(size, o); break;
        case Primitive::Sphere: m = em::makeSphere(size.x / 2, std::max(detail, 3), std::max(detail / 2, 2), o); break;
        case Primitive::Cylinder: m = em::makeCylinder(size.x / 2, size.z, std::max(detail, 3), o); break;
        case Primitive::Cone: m = em::makeCone(size.x / 2, size.z, std::max(detail, 3), o); break;
        case Primitive::Torus: m = em::makeTorus(size.x / 8, size.x / 2, std::max(detail / 2, 3), std::max(detail, 3), o); break;
        case Primitive::Plane: m = em::makePlane(size.x, size.y, std::max(detail / 8, 1), std::max(detail / 8, 1), o); break;
        case Primitive::Stairs: m = em::makeStairs(size.y, size.x, size.z, std::max(detail / 2, 1), o); break;
        case Primitive::Arch: m = em::makeArch(size.x, size.z / 2, size.x / 6, size.y, std::max(detail, 4), o); break;
        }
        reset(std::move(m));
        dirty_ = true;
        status_ = "New primitive";
    }
    void reset(em::EditMesh m) {
        doc = Doc{};
        doc.mesh = std::move(m);
        history_.clear();
        path_.clear();
        dirty_ = false;
        triangles_ = false;
        status_.clear();
    }

    // ---- selection ----
    SelectMode mode() const { return doc.mode; }
    void setMode(SelectMode m) {
        if (m == doc.mode) return;
        Selection n;
        const em::EditMesh& e = doc.mesh;
        const std::vector<u32> fs(doc.sel.faces.begin(), doc.sel.faces.end());
        std::set<u32> vs = doc.sel.verts;
        if (doc.mode == SelectMode::Face) { const std::vector<u32> v = em::verticesOfFaces(e, fs); vs.insert(v.begin(), v.end()); }
        if (doc.mode == SelectMode::Edge) for (const em::Edge& ed : doc.sel.edges) { vs.insert(ed.first); vs.insert(ed.second); }
        if (m == SelectMode::Vertex) n.verts = vs;
        else if (m == SelectMode::Edge) {
            if (doc.mode == SelectMode::Face) { for (const em::Edge& ed : em::edgesOfFaces(e, fs)) n.edges.insert(ed); }
            else for (const em::Edge& ed : allEdges()) if (vs.count(ed.first) && vs.count(ed.second)) n.edges.insert(ed);
        } else {
            if (doc.mode == SelectMode::Edge) {                      // faces whose every edge is selected
                for (u32 i = 0; i < e.faces.size(); ++i) {
                    const em::Face& f = e.faces[i];
                    bool all = !f.v.empty();
                    for (usize c = 0; c < f.v.size() && all; ++c) all = doc.sel.edges.count(em::makeEdge(f.v[c], f.v[(c + 1) % f.v.size()])) > 0;
                    if (all) n.faces.insert(i);
                }
            } else {                                                  // faces whose every vertex is selected
                for (u32 i = 0; i < e.faces.size(); ++i) {
                    bool all = !e.faces[i].v.empty();
                    for (const u32 v : e.faces[i].v) all = all && vs.count(v) > 0;
                    if (all) n.faces.insert(i);
                }
            }
        }
        doc.sel = std::move(n);
        doc.mode = m;
    }
    void clearSelection() { doc.sel = Selection{}; }
    void selectAll() {
        clearSelection();
        const em::EditMesh& e = doc.mesh;
        if (doc.mode == SelectMode::Face) for (u32 i = 0; i < e.faces.size(); ++i) doc.sel.faces.insert(i);
        else if (doc.mode == SelectMode::Edge) for (const em::Edge& ed : allEdges()) doc.sel.edges.insert(ed);
        else for (const u32 v : usedVertices()) doc.sel.verts.insert(v);
    }
    // index of a vertex / face for the active mode; additive keeps the rest of the selection, a second pick of a selected element removes it.
    void pickVertex(u32 v, bool additive) { pick(doc.sel.verts, v, additive, SelectMode::Vertex, v < doc.mesh.pos.size()); }
    void pickFace(u32 f, bool additive) { pick(doc.sel.faces, f, additive, SelectMode::Face, f < doc.mesh.faces.size()); }
    void pickEdge(u32 a, u32 b, bool additive) {
        if (doc.mode != SelectMode::Edge) return;
        const em::Edge e = em::makeEdge(a, b);
        if (!additive) doc.sel = Selection{};
        if (!doc.sel.edges.erase(e)) doc.sel.edges.insert(e);
    }
    usize selectionCount() const { return doc.mode == SelectMode::Vertex ? doc.sel.verts.size() : doc.mode == SelectMode::Edge ? doc.sel.edges.size() : doc.sel.faces.size(); }

    std::vector<u32> selectedVertices() const {
        std::set<u32> v = doc.sel.verts;
        if (doc.mode == SelectMode::Face) { const std::vector<u32> f(doc.sel.faces.begin(), doc.sel.faces.end()); const auto x = em::verticesOfFaces(doc.mesh, f); v.insert(x.begin(), x.end()); }
        if (doc.mode == SelectMode::Edge) for (const em::Edge& e : doc.sel.edges) { v.insert(e.first); v.insert(e.second); }
        return {v.begin(), v.end()};
    }
    em::V3 selectionCentre() const {
        const std::vector<u32> v = selectedVertices();
        em::V3 c;
        if (v.empty()) return c;
        for (const u32 i : v) c += doc.mesh.pos[i];
        return c * (1.0 / v.size());
    }

    // ---- commands (each one undoable; false = refused, status() says why) ----
    bool move(const em::V3& delta) {
        return edit("Move", [&](Doc& d) { em::moveVertices(d.mesh, selectedOf(d), delta); return ok(); }, true);
    }
    bool rotate(const em::V3& axis, double radians) {
        const em::V3 pivot = params.pivot == PivotMode::SelectionCentre ? selectionCentre() : em::V3{};
        return edit("Rotate", [&](Doc& d) { em::rotateVertices(d.mesh, selectedOf(d), pivot, axis, radians); return ok(); }, true);
    }
    bool scale(const em::V3& s) {
        const em::V3 pivot = params.pivot == PivotMode::SelectionCentre ? selectionCentre() : em::V3{};
        return edit("Scale", [&](Doc& d) { em::scaleVertices(d.mesh, selectedOf(d), pivot, s); return ok(); }, true);
    }
    bool extrude() { return extrude(params.extrudeDistance); }
    bool extrude(double distance) {
        return edit("Extrude", [&](Doc& d) {
            if (d.mode == SelectMode::Face) {
                em::OpResult r = em::extrudeFaces(d.mesh, std::vector<u32>(d.sel.faces.begin(), d.sel.faces.end()), distance, params.uvTile);
                if (r.ok) d.sel.faces = std::set<u32>(r.faces.begin(), r.faces.end());
                return r;
            }
            if (d.mode == SelectMode::Edge) {
                em::V3 dir;                                                     // along the normal of the face each edge borders
                for (const em::Edge& e : d.sel.edges)
                    for (u32 f = 0; f < d.mesh.faces.size(); ++f) {
                        const em::Face& fc = d.mesh.faces[f];
                        for (usize c = 0; c < fc.v.size(); ++c) if (em::makeEdge(fc.v[c], fc.v[(c + 1) % fc.v.size()]) == e) dir += em::faceNormal(d.mesh, f);
                    }
                em::OpResult r = em::extrudeEdges(d.mesh, std::vector<em::Edge>(d.sel.edges.begin(), d.sel.edges.end()), em::normalized(dir) * distance, params.uvTile);
                if (r.ok) { d.sel = Selection{}; }
                return r;
            }
            return refuse("Extrude works on faces or boundary edges");
        });
    }
    bool inset() { return inset(params.insetAmount); }
    bool inset(double amount) {
        return edit("Inset", [&](Doc& d) {
            if (d.mode != SelectMode::Face) return refuse("Inset works on faces");
            return em::insetFaces(d.mesh, std::vector<u32>(d.sel.faces.begin(), d.sel.faces.end()), amount);
        });
    }
    bool bevel() { return bevel(params.bevelWidth, params.bevelSegments); }
    bool bevel(double width, int segments) {
        return edit("Bevel", [&](Doc& d) {
            if (d.mode != SelectMode::Edge) return refuse("Bevel works on edges");
            em::OpResult r = em::bevelEdges(d.mesh, std::vector<em::Edge>(d.sel.edges.begin(), d.sel.edges.end()), width, segments);
            if (r.ok) d.sel = Selection{};
            return r;
        });
    }
    bool deleteSelection() {
        return edit("Delete", [&](Doc& d) {
            em::OpResult r;
            if (d.mode == SelectMode::Face) r = em::deleteFaces(d.mesh, std::vector<u32>(d.sel.faces.begin(), d.sel.faces.end()));
            else if (d.mode == SelectMode::Edge) r = em::deleteEdges(d.mesh, std::vector<em::Edge>(d.sel.edges.begin(), d.sel.edges.end()));
            else r = em::deleteVertices(d.mesh, std::vector<u32>(d.sel.verts.begin(), d.sel.verts.end()));
            if (r.ok) { em::compact(d.mesh); d.sel = Selection{}; }
            return r;
        });
    }
    // Two or more selected vertices merge at their centre; with fewer, every vertex closer than the tolerance is welded.
    bool weld() {
        return edit("Weld", [&](Doc& d) {
            const std::vector<u32> v = selectedOf(d);
            em::OpResult r;
            if (v.size() >= 2) r = em::mergeVertices(d.mesh, v);
            else { r.ok = true; r.message = std::to_string(em::weldVertices(d.mesh, params.weldTolerance)) + " vertices welded"; }
            if (r.ok) { em::compact(d.mesh); d.sel = Selection{}; }
            return r;
        });
    }
    bool flip() {
        return edit("Flip", [&](Doc& d) {
            em::flipFaces(d.mesh, facesOf(d));
            return ok();
        });
    }
    bool subdivide() {
        return edit("Subdivide", [&](Doc& d) {
            em::OpResult r = em::subdivideFlat(d.mesh, d.mode == SelectMode::Face ? std::vector<u32>(d.sel.faces.begin(), d.sel.faces.end()) : std::vector<u32>{});
            if (r.ok) { d.sel = Selection{}; d.mode = SelectMode::Face; d.sel.faces = std::set<u32>(r.faces.begin(), r.faces.end()); }
            return r;
        });
    }
    bool mirror() {
        return edit("Mirror", [&](Doc& d) { em::OpResult r = em::mirror(d.mesh, params.mirrorAxis, params.mirrorAt, params.weldTolerance); if (r.ok) { em::compact(d.mesh); d.sel = Selection{}; } return r; });
    }
    // The document mesh OP `other` (a mesh from the level, a primitive, or another model's document). Both must be closed solids.
    bool booleanWith(const em::EditMesh& other, fmt::csg::Op op) {
        return edit("Boolean", [&](Doc& d) {
            em::EditMesh out;
            std::string why;
            em::OpResult r;
            if (!em::boolean(d.mesh, other, op, out, &why)) { r.message = why; return r; }
            if (out.faces.empty()) { r.message = why.empty() ? "the result is empty" : why; return r; }
            d.mesh = std::move(out);
            d.sel = Selection{};
            r.ok = true;
            return r;
        });
    }
    bool projectUv(em::UvProjection kind) {
        return edit("Project UV", [&](Doc& d) {
            em::UvProjectionParams p;
            p.tile = params.uvTile;
            const std::vector<u32> f = facesOf(d);
            if (kind != em::UvProjection::Box) { p.origin = selectionCentreOf(d); }
            em::projectUv(d.mesh, f, kind, p);
            return ok();
        });
    }
    bool transformUv(const em::V2& offset, double rotationDegrees, const em::V2& scale) {
        return edit("Transform UV", [&](Doc& d) { em::transformUv(d.mesh, facesOf(d), offset, rotationDegrees, scale); return ok(); });
    }
    double texelDensity() const { return em::texelDensity(doc.mesh, doc.mode == SelectMode::Face ? std::vector<u32>(doc.sel.faces.begin(), doc.sel.faces.end()) : std::vector<u32>{}, params.textureSize); }

    bool undo() {
        if (!history_.undo(doc)) return false;
        dirty_ = true; status_ = "Undo";
        return true;
    }
    bool redo() {
        if (!history_.redo(doc)) return false;
        dirty_ = true; status_ = "Redo";
        return true;
    }

    // ---- readouts ----
    em::Validation validation() const { return em::validate(doc.mesh); }
    double volume() const { return em::volume(doc.mesh); }
    usize vertexCount() const { return usedVertices().size(); }
    usize faceCount() const { return doc.mesh.faces.size(); }
    usize triangleCount() const { return em::triangulate(doc.mesh).size(); }

    // ---- files ----
    bool save(const std::string& path, const em::SaveOptions& o = {}) {
        std::string why;
        if (!em::saveAuthoredMesh(path, doc.mesh, o, &why)) { status_ = "Save failed: " + why; return false; }
        path_ = path; dirty_ = false; status_ = "Saved " + path;
        return true;
    }
    bool saveAgain(const em::SaveOptions& o = {}) { return !path_.empty() && save(path_, o); }
    bool load(const std::string& path) {
        em::EditMesh m;
        bool side = false;
        std::string why;
        if (!em::loadAuthoredMesh(path, m, &side, &why)) { status_ = "Open failed: " + why; return false; }
        reset(std::move(m));
        path_ = path;
        triangles_ = !side;
        status_ = side ? "Opened with its edit data" : "Opened as triangles: this mesh has no edit data, so its quads are not recoverable";
        return true;
    }

private:
    SnapshotUndo<Doc> history_;
    std::string status_;
    std::string path_;
    bool dirty_ = false;
    bool triangles_ = false;

    static em::OpResult ok() { em::OpResult r; r.ok = true; return r; }
    static em::OpResult refuse(const std::string& why) { em::OpResult r; r.message = why; return r; }

    // Runs the command on the live document; if it refuses the snapshot goes back and the history is not touched.
    template <class F> bool edit(const char* name, F&& fn, bool needsSelection = false) {
        if (needsSelection && selectionCount() == 0) { status_ = std::string(name) + ": nothing is selected"; return false; }
        Doc before = doc;
        const em::OpResult r = fn(doc);
        if (!r.ok) {
            doc = std::move(before);
            status_ = std::string(name) + ": " + (r.message.empty() ? "refused" : r.message);
            return false;
        }
        history_.push(std::move(before));   // only a command that worked touches the history, so a refusal keeps the redo stack
        dirty_ = true;
        status_ = std::string(name) + (r.message.empty() ? "" : ": " + r.message);
        return true;
    }

    std::vector<u32> selectedOf(const Doc& d) const {
        std::set<u32> v = d.sel.verts;
        if (d.mode == SelectMode::Face) { const std::vector<u32> f(d.sel.faces.begin(), d.sel.faces.end()); const auto x = em::verticesOfFaces(d.mesh, f); v.insert(x.begin(), x.end()); }
        if (d.mode == SelectMode::Edge) for (const em::Edge& e : d.sel.edges) { v.insert(e.first); v.insert(e.second); }
        return {v.begin(), v.end()};
    }
    // faces a face-level command applies to: the selection in face mode, the faces using the selected vertices otherwise, every face when nothing is selected
    std::vector<u32> facesOf(const Doc& d) const {
        if (d.mode == SelectMode::Face) return std::vector<u32>(d.sel.faces.begin(), d.sel.faces.end());
        const std::vector<u32> v = selectedOf(d);
        return v.empty() ? std::vector<u32>{} : em::facesOfVertices(d.mesh, v);
    }
    em::V3 selectionCentreOf(const Doc& d) const {
        const std::vector<u32> v = selectedOf(d);
        em::V3 c;
        if (v.empty()) { const em::Bounds3 b = em::bounds(d.mesh); return b.valid ? (b.lo + b.hi) * 0.5 : c; }
        for (const u32 i : v) c += d.mesh.pos[i];
        return c * (1.0 / v.size());
    }
    void pick(std::set<u32>& s, u32 i, bool additive, SelectMode need, bool inRange) {
        if (doc.mode != need || !inRange) return;
        if (!additive) { const bool had = s.count(i) > 0 && s.size() == 1; s.clear(); if (!had) s.insert(i); return; }
        if (!s.erase(i)) s.insert(i);
    }
    std::vector<u32> usedVertices() const {
        std::set<u32> s;
        for (const em::Face& f : doc.mesh.faces) s.insert(f.v.begin(), f.v.end());
        return {s.begin(), s.end()};
    }
    std::vector<em::Edge> allEdges() const {
        std::vector<u32> all;
        for (u32 i = 0; i < doc.mesh.faces.size(); ++i) all.push_back(i);
        return em::edgesOfFaces(doc.mesh, all);
    }
};

}  // namespace aver::editor::modeling
