// ModelingPickTest -- the Modeling mode's viewport picking without a window (sandbox/src/ModelingPick.hpp, header-only): a box seen from the front through
// a plain orthographic projector, clicks that pick the nearest vertex / edge / front-most face, additive and replace behaviour, and marquees in each
// select mode. A point behind the camera is never picked.
#include "ModelingPick.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
namespace md = aver::editor::modeling;
namespace em = aver::fmt::em;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// Looking along +Y: screen x = 3 * x + 400, screen y = 300 - 3 * z, depth = y (the y = -50 face is the near one). Nothing is behind unless y < -1000.
static md::Projector frontView() {
    return [](const em::V3& p, f32& sx, f32& sy, f32& depth) {
        if (p.y < -1000.0) return false;
        sx = static_cast<f32>(3.0 * p.x + 400.0);
        sy = static_cast<f32>(300.0 - 3.0 * p.z);
        depth = static_cast<f32>(p.y);
        return true;
    };
}

static md::ModelingModel boxModel() {
    md::ModelingModel m;
    m.params.primitive.pivotAtBase = false;
    m.newPrimitive(md::Primitive::Box, {100, 100, 100});
    return m;
}

// The screen position of the box vertex nearest a corner of the front face (x, z signs +-1; y = -50).
static int cornerVertex(const em::EditMesh& m, double sx, double sz, double y) {
    for (usize i = 0; i < m.pos.size(); ++i)
        if (std::fabs(m.pos[i].x - 50.0 * sx) < 1e-3 && std::fabs(m.pos[i].z - 50.0 * sz) < 1e-3 && std::fabs(m.pos[i].y - y) < 1e-3) return static_cast<int>(i);
    return -1;
}

int main() {
    AVER_INFO("==================================================");
    AVER_INFO("Modeling viewport picking");
    md::ModelingModel model = boxModel();
    const em::EditMesh& mesh = model.doc.mesh;
    check(mesh.faces.size() == 6, "a box has six faces");
    const md::Projector proj = frontView();
    const auto sv = md::projectMesh(mesh, proj);
    check(sv.size() == mesh.pos.size() && sv[0].ok, "every vertex projects");

    const int frontTL = cornerVertex(mesh, -1, 1, -50.0), backTL = cornerVertex(mesh, -1, 1, 50.0);
    check(frontTL >= 0 && backTL >= 0 && frontTL != backTL, "the fixture finds a front and a back corner");
    const f32 tlx = sv[static_cast<usize>(frontTL)].x, tly = sv[static_cast<usize>(frontTL)].y;
    check(md::usedVertexList(mesh).size() == 8 && md::edgeList(mesh).size() == 12, "8 vertices and 12 edges");

    // ---- vertices ----
    model.setMode(md::SelectMode::Vertex);
    check(md::nearestVertex(mesh, sv, tlx + 2, tly + 1, 8.0f) >= 0, "a vertex within the radius is found");
    check(md::nearestVertex(mesh, sv, tlx + 40, tly + 40, 8.0f) == -1, "none outside the radius");
    check(md::nearestVertex(mesh, sv, tlx, tly, 8.0f) == frontTL, "front and back corners coincide on screen: the nearer to the camera wins");
    check(md::applyClick(model, sv, tlx, tly, 8.0f, false) && model.doc.sel.verts.count(static_cast<u32>(frontTL)) == 1 && model.doc.sel.verts.size() == 1, "a click selects it");
    const f32 trx = sv[static_cast<usize>(cornerVertex(mesh, 1, 1, -50.0))].x;
    check(md::applyClick(model, sv, trx, tly, 8.0f, true) && model.doc.sel.verts.size() == 2, "an additive click adds another");
    check(md::applyClick(model, sv, tlx, tly, 8.0f, true) && model.doc.sel.verts.size() == 1, "an additive click on a selected one removes it");
    check(md::applyClick(model, sv, 5000, 5000, 8.0f, false) && model.doc.sel.empty(), "a click on nothing clears");
    check(!md::applyClick(model, sv, 5000, 5000, 8.0f, false), "and reports no change the second time");

    // ---- edges ----
    model.setMode(md::SelectMode::Edge);
    em::Edge e;
    const f32 midTopX = 400.0f, topY = tly;
    check(md::nearestEdge(mesh, sv, midTopX, topY + 2, 6.0f, e), "an edge near its middle is found");
    check(md::applyClick(model, sv, midTopX, topY + 2, 6.0f, false) && model.doc.sel.edges.size() == 1, "a click selects the edge");
    check(!md::nearestEdge(mesh, sv, 400.0f, 300.0f, 6.0f, e), "the middle of a face is no edge");

    // ---- faces ----
    model.setMode(md::SelectMode::Face);
    const int front = md::faceAt(mesh, sv, 400.0f, 300.0f);
    check(front >= 0, "a face is under the cursor");
    bool nearer = true;
    for (const u32 v : mesh.faces[static_cast<usize>(front)].v) nearer = nearer && std::fabs(mesh.pos[v].y - (-50.0)) < 1e-3;
    check(nearer, "of the front and back faces under the cursor, the near one wins");
    check(md::applyClick(model, sv, 400.0f, 300.0f, 6.0f, false) && model.doc.sel.faces.size() == 1, "a click selects it");
    check(md::faceAt(mesh, sv, 50.0f, 50.0f) == -1, "outside the box there is no face");

    // ---- marquee ----
    const md::Rect all = md::rectOf(100, 100, 700, 500);
    model.setMode(md::SelectMode::Vertex);
    check(md::applyMarquee(model, sv, all, false) && model.doc.sel.verts.size() == 8, "a marquee round everything takes all 8 vertices");
    const md::Rect flipped = md::rectOf(700, 500, 0, 0);
    check(flipped.loX == 0 && flipped.hiX == 700 && flipped.loY == 0 && flipped.hiY == 500, "a marquee dragged right to left is normalised");
    const md::Rect top = md::rectOf(200, tly - 5, 600, tly + 5);
    check(md::applyMarquee(model, sv, top, false) && model.doc.sel.verts.size() == 4, "a thin marquee along the top takes the four top vertices (front and back, through)");
    check(!md::applyMarquee(model, sv, md::rectOf(0, 0, 20, 20), true) && model.doc.sel.verts.size() == 4, "an additive marquee that catches nothing keeps the selection and reports no change");
    check(md::applyMarquee(model, sv, md::rectOf(0, 0, 20, 20), false) && model.doc.sel.empty(), "a replacing marquee that catches nothing clears it");
    model.setMode(md::SelectMode::Edge);
    check(md::applyMarquee(model, sv, all, false) && model.doc.sel.edges.size() == 12, "edges: all twelve");
    check(md::applyMarquee(model, sv, top, false) && model.doc.sel.edges.size() == 4, "edges: a thin top marquee takes the four top edges (two across, two running away from the camera)");
    model.setMode(md::SelectMode::Face);
    check(md::applyMarquee(model, sv, all, false) && model.doc.sel.faces.size() == 6, "faces: all six");
    check(md::applyMarquee(model, sv, top, false) && model.doc.sel.faces.size() == 1, "faces: only whole faces inside count (the top face is edge-on, so its corners all sit in the thin rectangle)");

    // ---- behind the camera ----
    const auto behind = md::projectMesh(mesh, [](const em::V3&, f32&, f32&, f32&) { return false; });
    check(md::nearestVertex(mesh, behind, 400, 300, 1000) == -1 && md::faceAt(mesh, behind, 400, 300) == -1 && md::verticesIn(mesh, behind, all).empty(), "nothing behind the camera is picked");

    // the selection feeds the model's own commands
    model.setMode(md::SelectMode::Face);
    md::applyClick(model, sv, 400.0f, 300.0f, 6.0f, false);
    const usize facesBefore = model.faceCount();
    check(model.extrude(20.0) && model.faceCount() > facesBefore, "a picked face extrudes");

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
