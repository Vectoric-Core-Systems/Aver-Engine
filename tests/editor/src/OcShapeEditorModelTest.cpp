// OcShapeEditorModelTest -- the .ocshape editor tab's structural core without a window (sandbox/src/OcShapeEditorModel.hpp, header-only):
// add a leaf / a compound, wrap in a decorator, add to a compound, reorder, rename, delete with cascade, the outline the tree draws, the
// issues list being fmt::validateOcShape's output, and the snapshot undo bookkeeping the tab uses (SnapshotUndo over the whole shape).
// The widgets themselves are ImGui and covered by the owner's headed run.
#include "OcShapeEditorModel.hpp"
#include "SnapshotUndo.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
namespace sm = aver::editor::shapemodel;
using fmt::OcShapeData;
using fmt::ShapeNodeKind;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

static bool valid(const OcShapeData& d) { return fmt::validateOcShape(d).empty(); }

static usize childrenOf(const OcShapeData& d, const std::string& compound) {
    usize n = 0;
    for (const auto& c : d.children) n += c.compound == compound ? 1u : 0u;
    return n;
}

static void testAdd() {
    AVER_INFO("=== add ===");
    OcShapeData d;
    const std::string a = sm::addLeaf(d, ShapeNodeKind::Box);
    check(a == "box" && d.root == "box" && d.nodes.size() == 1, "the first leaf is the root, named for its kind");
    check(sm::addLeaf(d, ShapeNodeKind::Box) == "box2" && d.root == "box", "a second box is box2 and the root stays");
    check(sm::addLeaf(d, ShapeNodeKind::HullPoints) == "hull", "a hull of points is 'hull'");
    check(sm::addLeaf(d, ShapeNodeKind::Mesh, "Meshes/floor.ocmesh") == "mesh" && d.find("mesh")->path == "Meshes/floor.ocmesh", "a mesh leaf takes its path");
    check(sm::addLeaf(d, ShapeNodeKind::Mesh) != "" && d.find("mesh2")->path == "Meshes/cube.ocmesh", "and has a placeholder when none is given");
    check(sm::addLeaf(d, ShapeNodeKind::Compound).empty() && sm::addLeaf(d, ShapeNodeKind::Scaled).empty(), "compound and decorators are not leaves");
    const std::string c = sm::addCompound(d, false);
    check(c == "compound" && !d.find("compound")->mutableCompound && sm::addCompound(d, true) == "compound2" && d.find("compound2")->mutableCompound, "compounds: static and mutable");

    // Every kind's default numbers validate on their own.
    bool allValid = true;
    for (const ShapeNodeKind k : {ShapeNodeKind::Box, ShapeNodeKind::Sphere, ShapeNodeKind::Capsule, ShapeNodeKind::Cylinder, ShapeNodeKind::TaperedCapsule,
                                  ShapeNodeKind::TaperedCylinder, ShapeNodeKind::Triangle, ShapeNodeKind::Plane, ShapeNodeKind::Empty, ShapeNodeKind::HullPoints}) {
        OcShapeData one;
        sm::addLeaf(one, k);
        allValid = allValid && valid(one);
        if (!valid(one)) AVER_ERROR("   (default {} is not valid: {})", fmt::shapeKindWord(k), fmt::validateOcShape(one).front());
    }
    check(allValid, "a freshly added leaf of every kind is valid as it stands");
    check(sm::numberLabel(ShapeNodeKind::Capsule, 1) == "height (total)" && sm::numberLabel(ShapeNodeKind::HullPoints, 4) == "point 2 y" && sm::numberLabel(ShapeNodeKind::Box, 9) == "value",
          "number labels follow the grammar");
    check(valid(sm::starterShape()) && sm::starterText().find("NODE box box 25 25 25") != std::string::npos, "the starter shape is valid and one box");
}

static void testWrapAndChildren() {
    AVER_INFO("=== wrap, children, reorder ===");
    OcShapeData d;
    const std::string leg = sm::addLeaf(d, ShapeNodeKind::Cylinder);
    const std::string seat = sm::addLeaf(d, ShapeNodeKind::Box);
    const std::string top = sm::addCompound(d, false);
    check(sm::addChild(d, top, seat) && sm::addChild(d, top, leg) && sm::addChild(d, top, leg) && childrenOf(d, top) == 3, "children append in order, a leaf may be added twice");
    check(sm::setRoot(d, top) && d.root == top && !sm::setRoot(d, "nope"), "setRoot");
    check(valid(d), "the chair-shaped tree is valid");

    // Wrapping a child replaces its CHILD records' target.
    const std::string rt = sm::wrap(d, leg, ShapeNodeKind::RotatedTranslated);
    check(rt == "rt" && d.find(rt)->child == leg, "wrap makes a decorator over the node");
    usize pointingAtRt = 0, pointingAtLeg = 0;
    for (const auto& c : d.children) { pointingAtRt += c.node == rt; pointingAtLeg += c.node == leg; }
    check(pointingAtRt == 2 && pointingAtLeg == 0, "both CHILD records of the leg now point at the decorator");
    check(valid(d), "and the tree is still valid");

    // Wrapping the root moves the root.
    const std::string scale = sm::wrap(d, top, ShapeNodeKind::Scaled);
    check(d.root == scale && d.find(scale)->child == top, "wrapping the root makes the decorator the root");
    // Wrapping an already-wrapped node re-points the decorator above it.
    const std::string com = sm::wrap(d, top, ShapeNodeKind::OffsetCom);
    check(d.find(scale)->child == com && d.find(com)->child == top, "wrapping again slides in beneath the first decorator");
    check(sm::wrap(d, "ghost", ShapeNodeKind::Scaled).empty() && sm::wrap(d, top, ShapeNodeKind::Box).empty(), "wrap refuses an unknown node and a non-decorator kind");
    check(valid(d), "valid after the wraps");

    // addChild refusals.
    check(!sm::addChild(d, leg, seat), "a leaf cannot take children");
    check(!sm::addChild(d, top, top), "a compound cannot contain itself");
    check(!sm::addChild(d, top, scale), "nor something that contains it (the root decorator wraps the compound)");
    check(!sm::addChild(d, "ghost", seat) && !sm::addChild(d, top, "ghost"), "unknown ids are refused");
    const std::string mut = sm::addCompound(d, true);
    check(sm::addChild(d, mut, seat), "a mutable compound takes a child");
    check(sm::addChild(d, top, mut) && !sm::addChild(d, top, mut), "and may be placed once, not twice (it belongs to one body)");

    // Reorder within one compound, skipping the other compound's records.
    OcShapeData e;
    const std::string a = sm::addLeaf(e, ShapeNodeKind::Box), b = sm::addLeaf(e, ShapeNodeKind::Sphere), c1 = sm::addCompound(e, false), c2 = sm::addCompound(e, false);
    sm::addChild(e, c1, a); sm::addChild(e, c2, a); sm::addChild(e, c1, b);   // records: c1/a, c2/a, c1/b
    check(sm::moveChild(e, 0, 1) && e.children[0].node == b && e.children[2].node == a, "moving the first c1 record later swaps with the next c1 record, over c2's");
    check(!sm::moveChild(e, 0, -1) && !sm::moveChild(e, 1, 1) && !sm::moveChild(e, 0, 2) && !sm::moveChild(e, 9, 1), "moveChild refuses the ends, a bad delta and a bad index");
    check(sm::removeChild(e, 1) && e.children.size() == 2 && !sm::removeChild(e, 9), "removeChild drops one record");
}

static void testRemoveRename() {
    AVER_INFO("=== delete, rename ===");
    OcShapeData d;
    const std::string leaf = sm::addLeaf(d, ShapeNodeKind::Box);
    const std::string other = sm::addLeaf(d, ShapeNodeKind::Sphere);
    const std::string rt = sm::wrap(d, leaf, ShapeNodeKind::RotatedTranslated);
    const std::string sc = sm::wrap(d, rt, ShapeNodeKind::Scaled);
    const std::string comp = sm::addCompound(d, false);
    sm::addChild(d, comp, sc);
    sm::addChild(d, comp, other);
    sm::setRoot(d, comp);
    check(valid(d) && d.nodes.size() == 5, "five nodes, valid");

    // Deleting the leaf takes both decorators over it and the CHILD record of the outermost.
    check(sm::removeNode(d, leaf) == 3 && d.nodes.size() == 2 && d.children.size() == 1 && d.children[0].node == other, "deleting a leaf cascades through the decorators over it and their CHILD records");
    check(valid(d), "the rest is still valid");
    check(sm::removeNode(d, "ghost") == 0, "deleting an unknown id does nothing");
    check(sm::removeNode(d, comp) == 1 && d.children.empty() && d.root == other, "deleting the root moves the root to the first remaining node");
    check(sm::removeNode(d, other) == 1 && d.nodes.empty() && d.root.empty(), "deleting the last node empties the root");

    OcShapeData r;
    const std::string a = sm::addLeaf(r, ShapeNodeKind::Box);
    const std::string w = sm::wrap(r, a, ShapeNodeKind::Scaled);
    const std::string c = sm::addCompound(r, false);
    sm::addChild(r, c, w);
    sm::setRoot(r, c);
    check(sm::renameNode(r, a, "seat") && r.find("seat") && r.find(w)->child == "seat", "rename updates the decorator that wraps it");
    check(sm::renameNode(r, w, "big") && r.children[0].node == "big", "rename updates CHILD records");
    check(sm::renameNode(r, c, "chair") && r.root == "chair" && r.children[0].compound == "chair", "rename updates the root and a compound's records");
    check(valid(r), "still valid after the renames");
    check(!sm::renameNode(r, "seat", "big") && !sm::renameNode(r, "seat", "bad id") && !sm::renameNode(r, "seat", "") && !sm::renameNode(r, "ghost", "x"), "rename refuses a taken, invalid or empty id and an unknown node");
    check(sm::renameNode(r, "seat", "seat"), "renaming to the same id is a no-op that succeeds");

    // Hull points.
    OcShapeData h;
    const std::string hp = sm::addLeaf(h, ShapeNodeKind::HullPoints);
    fmt::ShapeNode& hull = *h.find(hp);
    check(sm::addHullPoint(hull) && hull.nums.size() == 15 && valid(h), "a hull point is added");
    check(sm::removeHullPoint(hull, 4) && hull.nums.size() == 12 && !sm::removeHullPoint(hull, 0), "a point is removed, but a hull keeps four");
}

static void testOutline() {
    AVER_INFO("=== outline ===");
    OcShapeData d;
    const std::string leg = sm::addLeaf(d, ShapeNodeKind::Cylinder);
    const std::string seat = sm::addLeaf(d, ShapeNodeKind::Box);
    const std::string spare = sm::addLeaf(d, ShapeNodeKind::Sphere);
    const std::string top = sm::addCompound(d, false);
    sm::addChild(d, top, seat);
    sm::addChild(d, top, leg);
    sm::addChild(d, top, leg);
    sm::setRoot(d, top);
    const std::string rt = sm::wrap(d, seat, ShapeNodeKind::RotatedTranslated);

    const auto rows = sm::outline(d);
    // top, rt, seat (under rt), leg, leg (repeated), then the orphan sphere.
    check(rows.size() == 6, "six rows: root, decorator, its leaf, two leg children, one orphan");
    if (rows.size() != 6) return;
    check(d.nodes[rows[0].node].id == top && rows[0].depth == 0, "the root first");
    check(d.nodes[rows[1].node].id == rt && rows[1].depth == 1 && rows[1].childRec == 0, "the decorator is the compound's first child record");
    check(d.nodes[rows[2].node].id == seat && rows[2].depth == 2 && rows[2].childRec == -1, "its leaf is one level down");
    check(d.nodes[rows[3].node].id == leg && !rows[3].repeated && rows[3].childRec == 1, "the leg the first time");
    check(d.nodes[rows[4].node].id == leg && rows[4].repeated && rows[4].childRec == 2, "and marked repeated the second");
    check(rows[5].orphan && d.nodes[rows[5].node].id == spare, "a node the root does not reach comes last, marked orphan");
    check(sm::orphans(d).size() == 1, "orphans lists it");
    check(sm::describe(d, d.nodes[rows[0].node]) == "compound static (3)" && sm::describe(d, d.nodes[rows[1].node]) == "rt " + seat &&
          sm::describe(d, *d.find(seat)) == "box 25 25 25", "describe reads like the file");

    // A hand-made cycle must not hang the outline.
    OcShapeData cyc;
    fmt::ShapeNode a, b;
    a.id = "a"; a.kind = ShapeNodeKind::Scaled; a.child = "b";
    b.id = "b"; b.kind = ShapeNodeKind::Scaled; b.child = "a";
    cyc.nodes = {a, b};
    cyc.root = "a";
    check(sm::outline(cyc).size() == 3, "a cycle is cut at the repeat (three rows) rather than looping");
}

static void testIssuesAndUndo() {
    AVER_INFO("=== issues and undo ===");
    OcShapeData d = sm::starterShape();
    check(fmt::validateOcShape(d).empty(), "the starter has no issues");
    const std::string comp = sm::addCompound(d, false);
    check(!fmt::validateOcShape(d).empty() && fmt::validateOcShape(d).front().find("children") != std::string::npos, "an empty compound is listed as an issue until it has children");
    sm::addChild(d, comp, "box");
    sm::setRoot(d, comp);
    check(fmt::validateOcShape(d).empty(), "and gone once it has one");

    // The tab's undo: the whole shape is the state.
    editor::SnapshotUndo<OcShapeData> history;
    OcShapeData live = d;
    history.push(live);
    sm::addLeaf(live, ShapeNodeKind::Sphere);
    check(live.nodes.size() == 3, "an edit adds a node");
    check(history.canUndo() && history.undo(live) && live.nodes.size() == 2, "undo restores the shape before the edit");
    check(history.canRedo() && history.redo(live) && live.nodes.size() == 3, "redo brings it back");
    check(!history.redo(live), "and there is nothing more to redo");
}

int main() {
    AVER_INFO("==================================================");
    AVER_INFO(".ocshape editor model");
    testAdd();
    testWrapAndChildren();
    testRemoveRename();
    testOutline();
    testIssuesAndUndo();
    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
