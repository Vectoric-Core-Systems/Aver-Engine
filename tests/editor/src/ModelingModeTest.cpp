// ModelingModeTest -- the Modeling mode's glue without a window (sandbox/src/ModelingMode.cpp compiled without ImGui): a click and a marquee through
// handleMouse reach the model's selection, Ctrl adds, the mouse events are reported as used only while a gesture runs, the select-mode keys, and the
// wireframe lines (edges, the selection in orange, vertex crosses only in vertex mode).
#include "ModelingMode.hpp"

#include "aver/core/ErrorCodes.hpp"
#include "aver/core/Log.hpp"

#include <string>

using namespace aver;
namespace md = aver::editor::modeling;

static int g_checks = 0, g_failures = 0;

static void check(bool cond, const std::string& what) {
    ++g_checks;
    if (cond) { AVER_INFO("   ok    {}", what); return; }
    AVER_ERROR("   FAIL  {}", what);
    ++g_failures;
}

// Looking along +Y from far away, orthographic: x -> 3x + 400, z -> 300 - 3z.
static bool front(const Vec3& w, f32& sx, f32& sy) {
    sx = 3.0f * w.x + 400.0f;
    sy = 300.0f - 3.0f * w.z;
    return true;
}

static editor::ModelingMouse press(f32 x, f32 y, bool ctrl = false) {
    editor::ModelingMouse m;
    m.x = x; m.y = y; m.overScene = true; m.down = true; m.clicked = true; m.ctrl = ctrl;
    return m;
}
static editor::ModelingMouse hold(f32 x, f32 y) {
    editor::ModelingMouse m;
    m.x = x; m.y = y; m.overScene = true; m.down = true;
    return m;
}
static editor::ModelingMouse release(f32 x, f32 y, bool ctrl = false) {
    editor::ModelingMouse m;
    m.x = x; m.y = y; m.overScene = true; m.released = true; m.ctrl = ctrl;
    return m;
}

int main() {
    AVER_INFO("==================================================");
    AVER_INFO("Modeling mode");
    editor::ModelingMode mode;
    const Vec3 eye{0, -2000, 0};
    const editor::ModelingMode::WorldProjector proj = front;

    check(!mode.handleMouse(hold(10, 10), proj, eye), "with no gesture a move over the scene is not used");
    mode.model.params.primitive.pivotAtBase = false;
    mode.model.newPrimitive(md::Primitive::Box, {100, 100, 100});
    std::vector<editor::ModelingLine> lines;
    mode.model.setMode(md::SelectMode::Face);
    mode.collectLines(lines);
    check(lines.size() == 12, "a box in face mode draws its twelve edges");

    // click on the middle of the front face
    check(mode.handleMouse(press(400, 300), proj, eye), "a press starts a gesture and is used");
    check(mode.model.selectionCount() == 0, "the selection waits for the release");
    check(mode.handleMouse(release(400, 300), proj, eye) && mode.model.selectionCount() == 1, "the release picks one face");
    check(!mode.marqueeActive(), "a click is not a marquee");
    lines.clear();
    mode.collectLines(lines);
    int hot = 0;
    for (const auto& l : lines) hot += (l.r > 0.9f && l.g < 0.7f) ? 1 : 0;
    check(hot == 4, "the selected face's four edges draw in orange");

    // Ctrl-click the empty background: adds nothing, keeps the selection
    mode.handleMouse(press(50, 50, true), proj, eye);
    mode.handleMouse(release(50, 50, true), proj, eye);
    check(mode.model.selectionCount() == 1, "a Ctrl-click on nothing keeps the selection");
    mode.handleMouse(press(50, 50), proj, eye);
    mode.handleMouse(release(50, 50), proj, eye);
    check(mode.model.selectionCount() == 0, "a plain click on nothing clears it");

    // marquee in vertex mode round the whole box
    mode.setSelectMode(0);
    check(mode.model.mode() == md::SelectMode::Vertex, "key 1 is vertex mode");
    mode.handleMouse(press(100, 100), proj, eye);
    check(mode.handleMouse(hold(700, 500), proj, eye) && mode.marqueeActive(), "dragging past the threshold arms the marquee");
    mode.handleMouse(release(700, 500), proj, eye);
    check(mode.model.selectionCount() == 8 && !mode.marqueeActive(), "the marquee takes all eight vertices");
    lines.clear();
    mode.collectLines(lines);
    check(lines.size() == 12 + 8 * 3, "vertex mode adds a cross per vertex");

    mode.setSelectMode(1);
    check(mode.model.mode() == md::SelectMode::Edge, "key 2 is edge mode");
    mode.setSelectMode(2);
    check(mode.model.mode() == md::SelectMode::Face, "key 3 is face mode");

    // undo goes to the model
    mode.model.setMode(md::SelectMode::Face);
    mode.model.clearSelection();
    mode.model.pickFace(0, false);
    check(mode.model.extrude(10.0) && mode.model.canUndo() && mode.undo() && mode.redo(), "undo and redo reach the model");
    mode.origin = Vec3{1000, 0, 0};
    lines.clear();
    mode.collectLines(lines);
    bool shifted = !lines.empty();
    for (const auto& l : lines) shifted = shifted && l.a.x > 900.0f;
    check(shifted, "the document draws at its placement in the world");

    AVER_INFO("==================================================");
    if (g_failures == 0) AVER_INFO("=== {} assertions, 0 failed ===", g_checks);
    else                 AVER_ERROR("=== {} assertions, {} FAILED ===", g_checks, g_failures);
    return exitCode(g_failures ? ExitCode::Failed : ExitCode::Ok);
}
