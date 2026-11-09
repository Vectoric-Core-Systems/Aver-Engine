// ModelingModelTest -- sandbox/src/ModelingModel.hpp: document, selection modes, commands with undo / redo, refusals, UV commands, save / reopen.
// Header-only model over fmt::em::EditMesh; no ImGui, no device. Exits 0 or 1.
#include "ModelingModel.hpp"

#include "aver/core/Log.hpp"

#include <cmath>
#include <filesystem>

using namespace aver::editor::modeling;
namespace em = aver::fmt::em;

static int g_checks = 0, g_failures = 0;
static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static aver::u32 topFace(const em::EditMesh& m) {
    aver::u32 best = 0; double bz = -1e300;
    for (aver::u32 i = 0; i < m.faces.size(); ++i) if (em::faceNormal(m, i).z > 0.99 && em::faceCentroid(m, i).z > bz) { bz = em::faceCentroid(m, i).z; best = i; }
    return best;
}

static void testSelectionModes() {
    AVER_INFO("=== selection modes ===");
    ModelingModel mm;
    mm.newPrimitive(Primitive::Box, {100, 100, 100});
    check(mm.faceCount() == 6 && mm.vertexCount() == 8 && mm.triangleCount() == 12 && mm.dirty(), "a new box document");
    check(mm.mode() == SelectMode::Face, "face mode is the default");
    const aver::u32 top = topFace(mm.doc.mesh);
    mm.pickFace(top, false);
    check(mm.selectionCount() == 1, "pick a face");
    mm.pickFace(top, true);
    check(mm.selectionCount() == 0, "an additive second pick removes it");
    mm.pickFace(top, false);
    mm.pickFace(top, false);
    check(mm.selectionCount() == 0, "a plain re-pick of the only selected face clears it");
    mm.pickFace(top, false);
    mm.setMode(SelectMode::Vertex);
    check(mm.selectionCount() == 4 && mm.mode() == SelectMode::Vertex, "face -> vertex: its four corners");
    mm.setMode(SelectMode::Edge);
    check(mm.selectionCount() == 4, "vertex -> edge: the four edges between them");
    mm.setMode(SelectMode::Face);
    check(mm.selectionCount() == 1 && mm.doc.sel.faces.count(top) == 1, "edge -> face: the face whose every edge is selected");
    mm.setMode(SelectMode::Vertex);
    mm.pickVertex(0, false);
    mm.setMode(SelectMode::Face);
    check(mm.selectionCount() == 0, "one vertex selects no face");
    mm.selectAll();
    check(mm.selectionCount() == 6, "select all faces");
    mm.setMode(SelectMode::Edge);
    check(mm.selectionCount() == 12, "face -> edge for everything: the cube's 12 edges");
    mm.clearSelection();
    check(mm.selectionCount() == 0, "clear");
    mm.setMode(SelectMode::Face);
    mm.pickFace(99, false);
    check(mm.selectionCount() == 0, "an out-of-range pick is ignored");
}

static void testCommands() {
    AVER_INFO("=== commands, undo and refusals ===");
    ModelingModel mm;
    mm.newPrimitive(Primitive::Box, {100, 100, 100});
    const aver::u32 top = topFace(mm.doc.mesh);
    check(!mm.extrude(10) && mm.status().find("Extrude") != std::string::npos && !mm.canUndo(), "extrude with nothing selected is refused, with a message, and records no undo");
    mm.pickFace(top, false);
    mm.params.extrudeDistance = 50;
    check(mm.extrude() && mm.canUndo() && near(mm.volume(), 1.5e6, 1e-3) && mm.validation().closed(), "extrude by the tool parameter");
    check(mm.doc.sel.faces.size() == 1 && *mm.doc.sel.faces.begin() == top, "the cap stays selected so the next command acts on it");
    check(mm.inset(10) && near(mm.volume(), 1.5e6, 1e-3), "inset it");
    check(mm.extrude(-20) && near(mm.volume(), 1.5e6 - 80.0 * 80 * 20, 1e-3), "and push it in");
    const double v3 = mm.volume();
    check(mm.undo() && mm.undo() && near(mm.volume(), 1.5e6, 1e-3), "two undos");
    check(mm.canRedo() && mm.redo() && mm.redo() && near(mm.volume(), v3, 1e-3), "two redos");
    check(!mm.redo(), "nothing left to redo");
    check(mm.undo() && mm.undo() && mm.undo() && near(mm.volume(), 1e6, 1e-3) && !mm.undo(), "undo all the way back; then nothing");

    const size_t faces = mm.faceCount();
    mm.setMode(SelectMode::Face);
    mm.clearSelection();
    mm.pickFace(top, false);
    check(!mm.inset(80) && mm.faceCount() == faces && mm.doc.sel.faces.size() == 1 && mm.status().find("wider") != std::string::npos, "an oversized inset says so and changes nothing");
    check(!mm.bevel() && mm.status().find("edges") != std::string::npos, "bevel in face mode is refused, with the reason");
    mm.setMode(SelectMode::Vertex);
    check(!mm.extrude(5) && mm.status().find("faces or boundary edges") != std::string::npos, "extrude in vertex mode is refused");

    mm.setMode(SelectMode::Face);
    mm.clearSelection();
    check(!mm.move({1, 0, 0}) && mm.status().find("nothing is selected") != std::string::npos, "move with nothing selected is refused");
    mm.pickFace(top, false);
    check(mm.move({0, 0, 25}) && near(em::bounds(mm.doc.mesh).hi.z, 75, 1e-9), "move the top face up 25");
    mm.undo();
    mm.selectAll();
    check(mm.scale({2, 2, 2}) && near(mm.volume(), 8e6, 1e-1), "scale everything about its centre by 2: volume x8");
    mm.undo();
    check(mm.rotate({0, 0, 1}, 3.14159265358979323846 / 4) && near(em::bounds(mm.doc.mesh).hi.x, 50 * std::sqrt(2.0), 1e-9), "rotate 45 degrees");
    mm.undo();
    mm.params.pivot = PivotMode::WorldOrigin;
    mm.setMode(SelectMode::Vertex);
    mm.clearSelection();
    mm.pickVertex(0, false);
    const em::V3 p0 = mm.doc.mesh.pos[0];
    check(mm.scale({0.5, 0.5, 0.5}) && em::length(mm.doc.mesh.pos[0] - p0 * 0.5) < 1e-9, "scale about the world origin when asked");
    mm.params.pivot = PivotMode::SelectionCentre;
}

static void testEdgeModeAndMore() {
    AVER_INFO("=== bevel, delete, weld, subdivide, mirror, flip ===");
    ModelingModel mm;
    mm.newPrimitive(Primitive::Box, {100, 100, 100});
    mm.setMode(SelectMode::Edge);
    mm.selectAll();
    mm.params.bevelWidth = 10;
    check(mm.bevel() && mm.validation().closed() && mm.faceCount() == 26 && mm.doc.sel.empty(), "bevel every edge with the tool parameters; the selection resets");
    check(near(mm.volume(), 80.0 * 80 * 80 + 6 * 80.0 * 80 * 10 + 12 * 80.0 * 50 + 8 * 1000.0 / 6.0, 1e-3), "and the volume is the chamfered cube's");
    mm.undo();
    check(mm.faceCount() == 6 && mm.canRedo(), "undo restores the box");

    mm.setMode(SelectMode::Face);
    mm.clearSelection();
    mm.pickFace(topFace(mm.doc.mesh), false);
    check(mm.deleteSelection() && mm.faceCount() == 5 && mm.vertexCount() == 8 && mm.doc.sel.empty(), "delete the top face");
    check(!mm.validation().closed() && mm.validation().openEdges == 4, "which opens the box");
    mm.undo();

    mm.selectAll();
    check(mm.flip() && near(mm.volume(), -1e6, 1e-3), "flip all faces");
    mm.flip();
    check(near(mm.volume(), 1e6, 1e-3), "and back");
    mm.clearSelection();
    check(mm.flip() && near(mm.volume(), -1e6, 1e-3), "flip with nothing selected acts on the whole mesh");
    mm.undo();

    check(mm.subdivide() && mm.faceCount() == 24 && mm.validation().closed() && mm.doc.sel.faces.size() == 24, "subdivide the whole box: 24 faces, all selected after");
    mm.setMode(SelectMode::Vertex);
    mm.clearSelection();
    mm.pickVertex(0, false);
    mm.pickVertex(1, true);
    const size_t vBefore = mm.vertexCount();
    check(mm.weld() && mm.vertexCount() == vBefore - 1, "weld two selected vertices into one");
    mm.undo();
    mm.clearSelection();
    check(mm.weld() && mm.status().find("0 vertices welded") != std::string::npos && mm.vertexCount() == vBefore, "weld with no selection reports how many were within the tolerance (none)");
    mm.undo();

    ModelingModel half;
    half.newPrimitive(Primitive::Box, {100, 100, 100});
    for (em::V3& p : half.doc.mesh.pos) p.x += 50;
    half.setMode(SelectMode::Face);
    for (aver::u32 i = 0; i < half.doc.mesh.faces.size(); ++i) if (em::faceNormal(half.doc.mesh, i).x < -0.99) half.pickFace(i, false);
    half.deleteSelection();
    half.params.mirrorAxis = 0;
    half.params.mirrorAt = 0;
    check(half.mirror() && half.validation().closed() && near(half.volume(), 2e6, 1e-3) && half.vertexCount() == 12, "mirror the half box across its open side");
}

static void testBooleanAndUv() {
    AVER_INFO("=== boolean and UV ===");
    ModelingModel mm;
    mm.newPrimitive(Primitive::Box, {2, 2, 2});
    ModelingModel other;
    other.newPrimitive(Primitive::Box, {2, 2, 2});
    for (em::V3& p : other.doc.mesh.pos) p += em::V3{1, 1, 1};
    check(mm.booleanWith(other.doc.mesh, aver::fmt::csg::Op::Union) && near(mm.volume(), 15.0, 1e-6) && mm.validation().closed(), "union with another document's mesh");
    check(mm.undo() && near(mm.volume(), 8.0, 1e-6), "undo the boolean");
    check(mm.redo() && near(mm.volume(), 15.0, 1e-6), "redo it");
    mm.undo();
    em::EditMesh open = other.doc.mesh;
    open.faces.pop_back();
    check(!mm.booleanWith(open, aver::fmt::csg::Op::Union) && mm.status().find("closed") != std::string::npos && near(mm.volume(), 8.0, 1e-6), "an open operand is refused with the reason; the document is intact");
    check(mm.canRedo(), "a refusal does not disturb the redo stack");
    em::EditMesh far = other.doc.mesh;
    for (em::V3& p : far.pos) p += em::V3{100, 0, 0};
    check(!mm.booleanWith(far, aver::fmt::csg::Op::Intersection) && mm.status().find("empty") != std::string::npos && near(mm.volume(), 8.0, 1e-6), "an empty result is refused rather than leaving an empty document");

    ModelingModel uv;
    uv.newPrimitive(Primitive::Box, {100, 100, 100});
    uv.params.uvTile = 50;
    check(uv.projectUv(em::UvProjection::Box) && near(uv.texelDensity(), 512.0 / 50.0, 1e-9), "box projection at tile 50 over the whole mesh: 512/50 texels per cm");
    uv.pickFace(topFace(uv.doc.mesh), false);
    check(uv.transformUv({0, 0}, 0, {0.5, 0.5}) && near(uv.texelDensity(), 512.0 / 50.0 / 2, 1e-9), "the density readout follows the selected face");
    uv.undo();
    check(near(uv.texelDensity(), 512.0 / 50.0, 1e-9), "undo restores the UVs");
}

static void testFiles() {
    AVER_INFO("=== files ===");
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "aver_modeling_model_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const std::string path = (dir / "pillar.ocmesh").string();
    ModelingModel mm;
    check(!mm.saveAgain(), "save-again with no path does nothing");
    mm.newPrimitive(Primitive::Box, {40, 40, 200});
    mm.pickFace(topFace(mm.doc.mesh), false);
    mm.extrude(30);
    check(mm.dirty() && mm.save(path) && !mm.dirty() && mm.path() == path, "save writes the file and clears the dirty flag");
    mm.inset(5);
    check(mm.dirty() && mm.saveAgain() && !mm.dirty(), "an edit dirties it again; save-again reuses the path");
    check(mm.undo() && mm.dirty(), "undo after a save is a change from the file");
    ModelingModel re;
    check(re.load(path) && !re.reopenedAsTriangles() && re.faceCount() == mm.faceCount() + 4 && !re.dirty() && !re.canUndo(), "reopen: quads from the edit data, clean, with a fresh history");
    check(re.status().find("edit data") != std::string::npos, "the status says where the mesh came from");
    fs::remove(path + ".edit", ec);
    ModelingModel tri;
    check(tri.load(path) && tri.reopenedAsTriangles() && tri.status().find("triangles") != std::string::npos && tri.validation().closed(), "without the edit data it opens as triangles, closed, and says so");
    check(!tri.load((dir / "missing.ocmesh").string()) && tri.status().find("Open failed") != std::string::npos, "a missing file reports a failure");
    ModelingModel empty;
    check(!empty.save(path) && empty.status().find("Save failed") != std::string::npos, "an empty document does not save");
    fs::remove_all(dir, ec);
}

static void testPrimitiveCommand() {
    AVER_INFO("=== primitives ===");
    const Primitive all[] = {Primitive::Box, Primitive::Sphere, Primitive::Cylinder, Primitive::Cone, Primitive::Torus, Primitive::Plane, Primitive::Stairs, Primitive::Arch};
    for (const Primitive p : all) {
        ModelingModel mm;
        mm.newPrimitive(p, {100, 100, 100}, 16);
        const bool sheet = p == Primitive::Plane;
        check(mm.faceCount() > 0 && (sheet || (mm.validation().closed() && mm.volume() > 0)), "primitive " + std::to_string(int(p)) + " builds " + (sheet ? "an open sheet" : "a closed solid"));
    }
    ModelingModel mm;
    mm.params.primitive.pivotAtBase = true;
    mm.newPrimitive(Primitive::Cylinder, {40, 40, 100});
    check(near(em::bounds(mm.doc.mesh).lo.z, 0, 1e-9), "pivot-at-base is a tool parameter");
}

int main() {
    testSelectionModes();
    testCommands();
    testEdgeModeAndMore();
    testBooleanAndUv();
    testFiles();
    testPrimitiveCommand();
    AVER_INFO("{} checks, {} failures", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
