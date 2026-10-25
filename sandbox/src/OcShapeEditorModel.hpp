#pragma once
// The pure half of the .ocshape editor tab (SHAPES_MATERIALS): the structural edits of a shape tree (add a leaf, wrap in a
// decorator, add to a compound, reorder, rename, delete with cascade), the outline the tree view draws, and the labels of each leaf's
// numbers. Header-only and ImGui-free over fmt::OcShapeData so tests/editor can drive every edit; OcShapeEditor.cpp is the widget half.
//
// EVERY EDIT KEEPS THE FILE A VALID TREE OR REFUSES (returns false / empty) and leaves the data as it was: ids stay unique and valid,
// no reference is left dangling, a compound never ends up inside itself. A tree with an empty compound is allowed while it is being
// built; fmt::validateOcShape (the editor's issues list) says so until it has children.
#include "aver/core/Types.hpp"
#include "aver/formats/OcShape.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aver::editor::shapemodel {

using fmt::OcShapeData;
using fmt::ShapeChildRec;
using fmt::ShapeNode;
using fmt::ShapeNodeKind;

// What a newly added leaf starts as: sizes that validate and read sensibly in a viewport (centimetres).
inline std::vector<f32> defaultNumbers(ShapeNodeKind k) {
    switch (k) {
        case ShapeNodeKind::Box: return {25, 25, 25};
        case ShapeNodeKind::Sphere: return {25};
        case ShapeNodeKind::Capsule: return {20, 100};
        case ShapeNodeKind::Cylinder: return {20, 60};
        case ShapeNodeKind::TaperedCapsule: return {20, 10, 100};
        case ShapeNodeKind::TaperedCylinder: return {20, 10, 60};
        case ShapeNodeKind::Triangle: return {0, 0, 0, 50, 0, 0, 0, 50, 0};
        case ShapeNodeKind::Plane: return {0, 0, 1, 0, 1000};
        case ShapeNodeKind::Empty: return {0, 0, 0};
        case ShapeNodeKind::HullPoints: return {0, 0, 0, 50, 0, 0, 0, 50, 0, 0, 0, 50};
        default: return {};
    }
}

// The label of the i-th number of a leaf kind (hull points: "point N x|y|z").
inline std::string numberLabel(ShapeNodeKind k, usize i) {
    static const char* const kBox[] = {"half X", "half Y", "half Z"};
    static const char* const kRadius[] = {"radius"};
    static const char* const kCapsule[] = {"radius", "height (total)"};
    static const char* const kCylinder[] = {"radius", "height"};
    static const char* const kTCapsule[] = {"top radius", "bottom radius", "height (total)"};
    static const char* const kTCylinder[] = {"top radius", "bottom radius", "height"};
    static const char* const kTriangle[] = {"A x", "A y", "A z", "B x", "B y", "B z", "C x", "C y", "C z"};
    static const char* const kPlane[] = {"normal x", "normal y", "normal z", "offset", "half extent"};
    static const char* const kEmpty[] = {"centre x", "centre y", "centre z"};
    const auto pick = [&](const char* const* table, usize n) { return i < n ? std::string(table[i]) : std::string("value"); };
    switch (k) {
        case ShapeNodeKind::Box: return pick(kBox, 3);
        case ShapeNodeKind::Sphere: return pick(kRadius, 1);
        case ShapeNodeKind::Capsule: return pick(kCapsule, 2);
        case ShapeNodeKind::Cylinder: return pick(kCylinder, 2);
        case ShapeNodeKind::TaperedCapsule: return pick(kTCapsule, 3);
        case ShapeNodeKind::TaperedCylinder: return pick(kTCylinder, 3);
        case ShapeNodeKind::Triangle: return pick(kTriangle, 9);
        case ShapeNodeKind::Plane: return pick(kPlane, 5);
        case ShapeNodeKind::Empty: return pick(kEmpty, 3);
        case ShapeNodeKind::HullPoints: {
            static const char* const kAxis[] = {"x", "y", "z"};
            return "point " + std::to_string(i / 3 + 1) + " " + kAxis[i % 3];
        }
        default: return "value";
    }
}

// The menu label of a kind.
inline const char* kindLabel(ShapeNodeKind k) {
    switch (k) {
        case ShapeNodeKind::Box: return "Box";
        case ShapeNodeKind::Sphere: return "Sphere";
        case ShapeNodeKind::Capsule: return "Capsule";
        case ShapeNodeKind::Cylinder: return "Cylinder";
        case ShapeNodeKind::TaperedCapsule: return "Tapered capsule";
        case ShapeNodeKind::TaperedCylinder: return "Tapered cylinder";
        case ShapeNodeKind::Triangle: return "Triangle";
        case ShapeNodeKind::Plane: return "Plane";
        case ShapeNodeKind::Empty: return "Empty";
        case ShapeNodeKind::HullPoints: return "Convex hull (points)";
        case ShapeNodeKind::HullMesh: return "Convex hull (from mesh)";
        case ShapeNodeKind::Mesh: return "Triangle mesh";
        case ShapeNodeKind::RotatedTranslated: return "Rotate / translate";
        case ShapeNodeKind::OffsetCom: return "Offset centre of mass";
        case ShapeNodeKind::Scaled: return "Scale";
        case ShapeNodeKind::Compound: return "Compound";
    }
    return "Node";
}

// "box", "box2", "box3", ...: the first id of that base nothing uses.
inline std::string uniqueId(const OcShapeData& d, const std::string& base) {
    const auto taken = [&](const std::string& id) { return d.find(id) != nullptr; };
    if (!taken(base)) return base;
    for (int i = 2;; ++i) {
        const std::string id = base + std::to_string(i);
        if (!taken(id)) return id;
    }
}

inline std::string baseIdFor(ShapeNodeKind k) {
    switch (k) {
        case ShapeNodeKind::HullPoints: return "hull";
        case ShapeNodeKind::HullMesh: return "hullmesh";
        case ShapeNodeKind::RotatedTranslated: return "rt";
        case ShapeNodeKind::OffsetCom: return "com";
        case ShapeNodeKind::Scaled: return "scale";
        default: return fmt::shapeKindWord(k);
    }
}

// Appends a leaf with default numbers (the file's default for a path kind is `path`) and returns its id. The first node of an empty tree
// becomes the ROOT.
inline std::string addLeaf(OcShapeData& d, ShapeNodeKind kind, const std::string& path = {}) {
    if (!fmt::shapeKindIsLeaf(kind) || d.nodes.size() >= fmt::kOcShapeMaxNodes) return {};
    ShapeNode n;
    n.id = uniqueId(d, baseIdFor(kind));
    n.kind = kind;
    n.nums = defaultNumbers(kind);
    if (kind == ShapeNodeKind::HullMesh || kind == ShapeNodeKind::Mesh) n.path = path.empty() ? std::string("Meshes/cube.ocmesh") : path;
    d.nodes.push_back(std::move(n));
    if (d.root.empty()) d.root = d.nodes.back().id;
    return d.nodes.back().id;
}

inline std::string addCompound(OcShapeData& d, bool mutableKind) {
    if (d.nodes.size() >= fmt::kOcShapeMaxNodes) return {};
    ShapeNode n;
    n.id = uniqueId(d, "compound");
    n.kind = ShapeNodeKind::Compound;
    n.mutableCompound = mutableKind;
    d.nodes.push_back(std::move(n));
    if (d.root.empty()) d.root = d.nodes.back().id;
    return d.nodes.back().id;
}

// True when `target` is reachable from `from` (or is `from`) through decorator and CHILD edges.
inline bool reaches(const OcShapeData& d, const std::string& from, const std::string& target) {
    std::vector<std::string> stack{from};
    std::unordered_set<std::string> seen;
    while (!stack.empty()) {
        const std::string cur = stack.back();
        stack.pop_back();
        if (cur == target) return true;
        if (!seen.insert(cur).second) continue;
        const ShapeNode* n = d.find(cur);
        if (!n) continue;
        if (fmt::shapeKindIsDecorator(n->kind)) stack.push_back(n->child);
        else if (n->kind == ShapeNodeKind::Compound)
            for (const ShapeChildRec& c : d.children)
                if (c.compound == cur) stack.push_back(c.node);
    }
    return false;
}

// Wraps node `id` in a new decorator and puts the decorator where `id` was (the ROOT, a decorator's child, a compound's CHILD record).
// Returns the decorator's id, or "" when `id` is unknown, `kind` is not a decorator or the tree is full.
inline std::string wrap(OcShapeData& d, const std::string& id, ShapeNodeKind kind) {
    if (!fmt::shapeKindIsDecorator(kind) || !d.find(id) || d.nodes.size() >= fmt::kOcShapeMaxNodes) return {};
    ShapeNode w;
    w.id = uniqueId(d, baseIdFor(kind));
    w.kind = kind;
    w.child = id;
    for (ShapeNode& n : d.nodes)
        if (fmt::shapeKindIsDecorator(n.kind) && n.child == id) n.child = w.id;
    for (ShapeChildRec& c : d.children)
        if (c.node == id) c.node = w.id;
    if (d.root == id) d.root = w.id;
    d.nodes.push_back(std::move(w));
    return d.nodes.back().id;
}

// Appends `node` as a child of `compound`. Refused for an unknown id, a compound that is not one, a node that is the compound or would
// put it inside itself, a mutable compound that already has a parent, and a full compound.
inline bool addChild(OcShapeData& d, const std::string& compound, const std::string& node) {
    const ShapeNode* comp = d.find(compound);
    const ShapeNode* child = d.find(node);
    if (!comp || !child || comp->kind != ShapeNodeKind::Compound) return false;
    if (reaches(d, node, compound)) return false;
    usize count = 0;
    for (const ShapeChildRec& c : d.children) count += c.compound == compound ? 1u : 0u;
    if (count >= fmt::kOcShapeMaxChildren || d.children.size() >= fmt::kOcShapeMaxChildren) return false;
    if (child->kind == ShapeNodeKind::Compound && child->mutableCompound) {
        // A mutable compound belongs to one body: one reference only.
        if (d.root == node) return false;
        for (const ShapeChildRec& c : d.children) if (c.node == node) return false;
        for (const ShapeNode& n : d.nodes) if (fmt::shapeKindIsDecorator(n.kind) && n.child == node) return false;
    }
    ShapeChildRec rec;
    rec.compound = compound;
    rec.node = node;
    d.children.push_back(std::move(rec));
    return true;
}

// Moves CHILD record `index` one place earlier (`delta` -1) or later (+1) among the records of its own compound.
inline bool moveChild(OcShapeData& d, usize index, int delta) {
    if (index >= d.children.size() || (delta != -1 && delta != 1)) return false;
    const std::string comp = d.children[index].compound;
    long j = static_cast<long>(index) + delta;
    while (j >= 0 && j < static_cast<long>(d.children.size()) && d.children[static_cast<usize>(j)].compound != comp) j += delta;
    if (j < 0 || j >= static_cast<long>(d.children.size())) return false;
    std::swap(d.children[index], d.children[static_cast<usize>(j)]);
    return true;
}

inline bool removeChild(OcShapeData& d, usize index) {
    if (index >= d.children.size()) return false;
    d.children.erase(d.children.begin() + static_cast<std::ptrdiff_t>(index));
    return true;
}

// Deletes node `id`, its CHILD records (as a compound and as a child), and every decorator that wrapped it (recursively): a decorator
// with no child is meaningless. The ROOT, if removed, moves to the first remaining node (or empties). Returns how many nodes went.
inline usize removeNode(OcShapeData& d, const std::string& id) {
    if (!d.find(id)) return 0;
    std::unordered_set<std::string> doomed{id};
    for (bool grew = true; grew;) {
        grew = false;
        for (const ShapeNode& n : d.nodes)
            if (fmt::shapeKindIsDecorator(n.kind) && doomed.count(n.child) && doomed.insert(n.id).second) grew = true;
    }
    const usize before = d.nodes.size();
    d.nodes.erase(std::remove_if(d.nodes.begin(), d.nodes.end(), [&](const ShapeNode& n) { return doomed.count(n.id) != 0; }), d.nodes.end());
    d.children.erase(std::remove_if(d.children.begin(), d.children.end(),
                                    [&](const ShapeChildRec& c) { return doomed.count(c.compound) != 0 || doomed.count(c.node) != 0; }),
                     d.children.end());
    if (doomed.count(d.root)) d.root = d.nodes.empty() ? std::string() : d.nodes.front().id;
    return before - d.nodes.size();
}

inline bool setRoot(OcShapeData& d, const std::string& id) {
    if (!d.find(id)) return false;
    d.root = id;
    return true;
}

// Renames a node and every reference to it. Refused for an invalid or taken id.
inline bool renameNode(OcShapeData& d, const std::string& id, const std::string& to) {
    if (id == to) return d.find(id) != nullptr;
    if (!d.find(id) || !fmt::ocShapeIdValid(to) || d.find(to)) return false;
    d.find(id)->id = to;
    for (ShapeNode& n : d.nodes) if (fmt::shapeKindIsDecorator(n.kind) && n.child == id) n.child = to;
    for (ShapeChildRec& c : d.children) {
        if (c.compound == id) c.compound = to;
        if (c.node == id) c.node = to;
    }
    if (d.root == id) d.root = to;
    return true;
}

// Hull points: add one at (0, 0, 25) cm, or remove one (a hull keeps at least four).
inline bool addHullPoint(ShapeNode& n) {
    if (n.kind != ShapeNodeKind::HullPoints) return false;
    n.nums.insert(n.nums.end(), {0.0f, 0.0f, 25.0f});
    return true;
}
inline bool removeHullPoint(ShapeNode& n, usize point) {
    if (n.kind != ShapeNodeKind::HullPoints || n.nums.size() <= 12 || (point + 1) * 3 > n.nums.size()) return false;
    n.nums.erase(n.nums.begin() + static_cast<std::ptrdiff_t>(point * 3), n.nums.begin() + static_cast<std::ptrdiff_t>(point * 3 + 3));
    return true;
}

// ---- the outline the tree draws -----------------------------------------------------------------------------------------------

struct OutlineRow {
    usize node = 0;          // index into OcShapeData::nodes
    int depth = 0;
    int childRec = -1;       // the CHILD record this row is, -1 for the root, a decorator's child and orphans
    bool repeated = false;   // already drawn above (a shared leaf or decorator): not expanded again
    bool orphan = false;     // not reachable from ROOT
};

// Pre-order from ROOT (a compound's children in file order), then the nodes ROOT does not reach. Cycle-safe.
inline std::vector<OutlineRow> outline(const OcShapeData& d) {
    std::vector<OutlineRow> rows;
    std::unordered_set<std::string> shown;
    const auto indexOf = [&](const std::string& id) -> long {
        for (usize i = 0; i < d.nodes.size(); ++i) if (d.nodes[i].id == id) return static_cast<long>(i);
        return -1;
    };
    // An explicit work list keeps the walk flat and bounded (a cycle is cut by `shown`).
    struct Item { std::string id; int depth; int childRec; };
    std::vector<Item> work;
    if (!d.root.empty()) work.push_back({d.root, 0, -1});
    while (!work.empty()) {
        const Item it = work.back();
        work.pop_back();
        const long at = indexOf(it.id);
        if (at < 0) continue;
        OutlineRow row;
        row.node = static_cast<usize>(at);
        row.depth = it.depth;
        row.childRec = it.childRec;
        row.repeated = !shown.insert(it.id).second;
        rows.push_back(row);
        if (row.repeated) continue;
        const ShapeNode& n = d.nodes[static_cast<usize>(at)];
        if (fmt::shapeKindIsDecorator(n.kind)) {
            work.push_back({n.child, it.depth + 1, -1});
        } else if (n.kind == ShapeNodeKind::Compound) {
            for (long r = static_cast<long>(d.children.size()) - 1; r >= 0; --r)   // reversed: the stack pops file order
                if (d.children[static_cast<usize>(r)].compound == n.id) work.push_back({d.children[static_cast<usize>(r)].node, it.depth + 1, static_cast<int>(r)});
        }
    }
    for (usize i = 0; i < d.nodes.size(); ++i) {
        if (shown.count(d.nodes[i].id)) continue;
        OutlineRow row;
        row.node = i;
        row.orphan = true;
        rows.push_back(row);
    }
    return rows;
}

// One line describing a node: "box 40 40 4", "compound static (6)", "rt leg", "mesh Meshes/floor.ocmesh".
inline std::string describe(const OcShapeData& d, const ShapeNode& n) {
    std::string s = fmt::shapeKindWord(n.kind);
    if (n.kind == ShapeNodeKind::HullMesh) s += " mesh " + n.path;
    else if (n.kind == ShapeNodeKind::Mesh) s += " " + n.path;
    else if (n.kind == ShapeNodeKind::HullPoints) s += " (" + std::to_string(n.nums.size() / 3) + " points)";
    else if (fmt::shapeKindIsLeaf(n.kind)) {
        for (const f32 v : n.nums) {
            char b[32];
            std::snprintf(b, sizeof b, " %g", static_cast<double>(v));
            s += b;
        }
    } else if (n.kind == ShapeNodeKind::Compound) {
        usize count = 0;
        for (const ShapeChildRec& c : d.children) count += c.compound == n.id ? 1u : 0u;
        s += n.mutableCompound ? " mutable (" : " static (";
        s += std::to_string(count) + ")";
    } else {
        s += " " + n.child;
    }
    return s;
}

// What a new, empty shape file contains: a 25 cm half-extent box at the root. Valid as it stands.
inline OcShapeData starterShape() {
    OcShapeData d;
    d.massKg = 10.0f;
    addLeaf(d, ShapeNodeKind::Box);
    return d;
}

inline std::string starterText() { return fmt::writeOcShape(starterShape()); }

// The nodes ROOT does not reach (an advisory: they are written but never built).
inline std::vector<usize> orphans(const OcShapeData& d) {
    std::vector<usize> out;
    for (const OutlineRow& r : outline(d)) if (r.orphan) out.push_back(r.node);
    return out;
}

} // namespace aver::editor::shapemodel
