// The blend-space and state-machine editor tabs' headless core: the structural edits (which renumber
// every index that points past what they remove), the canvas geometry, and each tab's own
// load / save / dirty / undo bookkeeping.
//
// Same BtEditorTest arrangement: Sandbox is add_executable-only, so this compiles the two editor
// sources directly with AVER_WITH_IMGUI left undefined, leaving exactly the part worth testing.
#include "AnimStateMachineEditor.hpp"
#include "BlendSpaceEditor.hpp"

#include "aver/anim/AnimGraphAsset.hpp"
#include "aver/core/Log.hpp"

#include <cmath>
#include <filesystem>
#include <string>

using namespace aver;
using namespace aver::editor;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static bool approx(f32 a, f32 b, f32 eps = 1e-4f) { return std::fabs(a - b) <= eps; }

static void blendSpaceEdits() {
    AVER_INFO("blend space edits");
    anim::BlendSpaceAsset a = bsStarterSpace();
    check(a.valid(), "the starter space is valid");

    const i32 n = bsAddSample(a, "c", 9999.0f, 5.0f);
    check(n == 3 && approx(a.samples[3].x, a.axisX.max) && a.samples[3].y == 0.0f,
          "a new sample is clamped into the axes, and y stays 0 in 1D");
    check(bsRemoveSample(a, 3) && a.samples.size() == 3, "a sample can be removed");

    anim::BlendSpaceAsset one = a;
    one.samples.resize(1);
    check(!bsRemoveSample(one, 0), "the last sample cannot be removed");
    check(!bsRemoveSample(a, 9), "an out-of-range index is refused");

    bsMoveSample(a, 1, 203.0f, 50.0f, 10.0f);
    check(approx(a.samples[1].x, 200.0f) && a.samples[1].y == 0.0f, "a move snaps to the grid and ignores y in 1D");
    bsMoveSample(a, 1, -500.0f, 0.0f, 0.0f);
    check(approx(a.samples[1].x, a.axisX.min), "a move clamps to the axis");

    bsSetDims(a, 2);
    bsMoveSample(a, 1, 100.0f, 90.0f, 0.0f);
    check(approx(a.samples[1].y, 90.0f), "in 2D y is kept");
    bsSetDims(a, 1);
    check(a.samples[1].y == 0.0f && a.dims == 1, "going back to 1D zeroes y");

    bsAddMarker(a, 0, "R", 0.5f);
    bsAddMarker(a, 0, "L", 0.0f);
    const i32 mid = bsAddMarker(a, 0, "M", 0.25f);
    check(mid == 1 && a.samples[0].markers[0].name == "L" && a.samples[0].markers[2].name == "R",
          "markers stay sorted by time");
    check(bsRemoveMarker(a, 0, 1) && a.samples[0].markers.size() == 2, "a marker can be removed");

    anim::BlendSpaceAsset g;
    g.dims = 2;
    g.axisX = {"x", 0, 10, 0};
    g.axisY = {"y", 0, 20, 0};
    g.samples = {};
    bsAddSample(g, "a", 2.0f, 4.0f);
    bsAddSample(g, "b", 8.0f, 16.0f);
    BsView v{100.0f, 50.0f, 400.0f, 200.0f};
    f32 sx, sy, x, y;
    bsToScreen(g, v, 2.0f, 4.0f, sx, sy);
    check(approx(sx, 180.0f) && approx(sy, 210.0f), "axis -> screen: x grows right, y grows UP the screen");
    bsFromScreen(g, v, sx, sy, x, y);
    check(approx(x, 2.0f) && approx(y, 4.0f), "screen -> axis inverts it");
    check(bsPick(g, v, sx + 3.0f, sy - 2.0f, 8.0f) == 0, "a click near a point picks it");
    check(bsPick(g, v, 0.0f, 0.0f, 8.0f) == -1, "a click far from every point picks nothing");
}

static void blendSpaceTab() {
    AVER_INFO("blend space tab");
    const std::string path = (std::filesystem::temp_directory_path() / "aver-editor-test.ocblend").string();
    std::string why;
    check(anim::saveBlendSpace(path, bsStarterSpace(), &why), "the fixture writes " + why);

    BlendSpaceEditor ed(path);
    check(ed.loaded(), "the tab loads the file");
    check(!ed.dirty() && !ed.canUndo(), "a fresh tab is clean");
    check(ed.probeWeights().size() >= 1, "the probe has weights from the start");

    const i32 added = ed.addSample("Content/Anim/Jog.ocanim", 400.0f, 0.0f);
    check(added == 3 && ed.dirty() && ed.canUndo() && ed.asset().samples.size() == 4, "adding a sample is one undo entry");
    ed.undo();
    check(ed.asset().samples.size() == 3 && ed.canRedo(), "undo removes it");
    ed.redo();
    check(ed.asset().samples.size() == 4, "redo puts it back");

    ed.setProbe(100.0f, 0.0f);
    f32 total = 0.0f;
    for (const auto& w : ed.probeWeights()) total += w.weight;
    check(approx(total, 1.0f, 1e-5f), "the probe's weights sum to one");

    check(ed.save(&why), "saves " + why);
    check(!ed.dirty(), "and is clean again");
    anim::BlendSpaceAsset back;
    check(anim::loadBlendSpace(path, back) && back.samples.size() == 4, "the saved file has the new sample");

    ed.deleteSelected();
    ed.onFileChanged();
    check(ed.dirty() && ed.asset().samples.size() == 3, "a dirty tab keeps its edits when the file changes");

    check(makeBlendSpaceEditor(path) != nullptr, "the factory claims .ocblend");
    check(makeBlendSpaceEditor("x.ocanim") == nullptr, "and nothing else");
    BlendSpaceEditor bad("no/such/file.ocblend");
    check(!bad.loaded() && !bad.loadError().empty(), "a missing file loads as an error, not a crash");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

static anim::AnimStateMachineAsset threeStates() {
    anim::AnimStateMachineAsset a = asmStarterMachine();
    anim::AsmMachine& m = a.machines[0];
    asmAddState(m, anim::AsmStateKind::Clip, "Run", 0.0f, 0.0f);
    m.states[2].asset = "Content/Anim/Run.ocanim";
    asmAddTransition(a, m, 1, 2, true);
    asmAddTransition(a, m, 2, 0, true);
    return a;
}

static void stateMachineEdits() {
    AVER_INFO("state machine edits");
    anim::AnimStateMachineAsset a = asmStarterMachine();
    check(a.valid(), "the starter machine is valid");

    anim::AsmMachine& m = a.machines[0];
    const i32 s1 = asmAddState(m, anim::AsmStateKind::Clip, "Idle", 0.0f, 0.0f);
    check(m.states[static_cast<usize>(s1)].name == "Idle 2", "a clashing state name is made unique");
    check(m.states[static_cast<usize>(s1)].posX != 0.0f, "a state added at (0,0) is laid out on the grid");

    // Transitions get safe defaults.
    const i32 t = asmAddTransition(a, m, 0, 1, true);
    check(t >= 0 && m.transitions[static_cast<usize>(t)].hasExitTime, "a transition from a state starts with an exit time");
    const i32 ta = asmAddTransition(a, m, anim::kAsmAny, 0, true);
    check(m.transitions[static_cast<usize>(ta)].conditions.size() == 1 && !m.transitions[static_cast<usize>(ta)].hasExitTime,
          "an Any transition starts with a condition on the first parameter");
    check(asmAddTransition(a, m, 0, anim::kAsmExit, true) == -1, "the root machine has no Exit");
    check(asmAddTransition(a, m, 0, 99, true) == -1, "a missing target is refused");
    check(asmAddTransition(a, m, 99, 0, true) == -1, "a missing source is refused");
    check(asmMoveTransition(m, 0, 1) == 1 && asmMoveTransition(m, 0, -1) == -1, "priority moves within the list and stops at the ends");

    // Removal renumbers.
    anim::AnimStateMachineAsset b = threeStates();
    i32 mi = 0;
    const usize before = b.machines[0].transitions.size();
    check(b.valid(), "three states and four transitions");
    b.machines[0].entry = 2;
    check(asmRemoveState(b, mi, 0), "the first state can be removed");
    check(b.machines[0].states.size() == 2 && b.machines[0].entry == 1, "the entry follows its state");
    check(b.machines[0].transitions.size() == before - 3, "every transition touching it goes");
    check(b.machines[0].transitions.size() == 1 && b.machines[0].transitions[0].from == 0 && b.machines[0].transitions[0].to == 1,
          "and the survivor (Walk -> Run) is renumbered to 0 -> 1");
    check(b.valid(), "the result is still a valid asset");
    asmRemoveState(b, mi, 0);
    check(!asmRemoveState(b, mi, 0), "the last state cannot be removed");

    // Sub-machines own machines; removing the state removes them and fixes every later index.
    anim::AnimStateMachineAsset c = asmStarterMachine();
    const i32 sub1 = asmAddSubMachine(c, 0, "Sub1", 0.0f, 0.0f);
    const i32 inner = asmAddSubMachine(c, 1, "Inner", 0.0f, 0.0f);
    const i32 sub2 = asmAddSubMachine(c, 0, "Sub2", 0.0f, 0.0f);
    check(sub1 >= 0 && inner >= 0 && sub2 >= 0 && c.machines.size() == 4, "three sub-machines make four machines");
    check(asmParentMachine(c, 2) == 1 && asmParentMachine(c, 3) == 0 && asmParentMachine(c, 0) == -1, "parents are found");
    i32 cur = 0;
    check(asmRemoveState(c, cur, sub1), "removing a sub-machine state succeeds");
    check(c.machines.size() == 2, "its machine and the machine nested in it are gone");
    check(c.machines[0].states.back().kind == anim::AsmStateKind::SubMachine && c.machines[0].states.back().subMachine == 1,
          "the surviving sub-machine state points at its renumbered machine");
    for (auto& mm : c.machines)
        for (auto& s : mm.states)
            if (s.kind != anim::AsmStateKind::SubMachine && s.asset.empty()) s.asset = "x";
    check(c.valid(), "the asset is still valid");

    // The caller's machine index survives removing a nested sub-machine state.
    anim::AnimStateMachineAsset d = asmStarterMachine();
    asmAddSubMachine(d, 0, "P", 0.0f, 0.0f);                     // machine 1
    const i32 x = asmAddSubMachine(d, 1, "X", 0.0f, 0.0f);       // machine 2, inside P
    i32 inP = 1;
    check(asmRemoveState(d, inP, x) && d.machines.size() == 2 && inP == 1,
          "removing a nested sub-machine state drops its machine and keeps the caller's index");
}

static void stateMachineParams() {
    AVER_INFO("parameters");
    anim::AnimStateMachineAsset a = asmStarterMachine();
    check(asmAddParam(a, "grounded", anim::AsmParamType::Bool, 1.0f) == 1, "a parameter adds");
    check(asmAddParam(a, "speed", anim::AsmParamType::Float, 0.0f) == -1 && asmAddParam(a, "", anim::AsmParamType::Float, 0.0f) == -1,
          "duplicate and empty names are refused");

    a.machines[0].states[0].speedParam = "speed";
    check(asmRenameParam(a, 0, "velocity"), "a parameter renames");
    check(a.machines[0].transitions[0].conditions[0].param == "velocity" && a.machines[0].states[0].speedParam == "velocity",
          "and every condition and state reference follows");
    check(!asmRenameParam(a, 0, "grounded"), "renaming onto another parameter is refused");
    check(a.valid(), "the asset is still valid after a rename");

    asmSetParamType(a, 0, anim::AsmParamType::Trigger);
    check(a.machines[0].transitions[0].conditions[0].op == anim::AsmOp::Trigger, "changing a parameter to a trigger repairs its conditions");
    asmSetParamType(a, 0, anim::AsmParamType::Bool);
    check(a.machines[0].transitions[0].conditions[0].op == anim::AsmOp::IsTrue, "and back to a bool again");
    asmSetParamType(a, 0, anim::AsmParamType::Float);
    check(a.machines[0].transitions[0].conditions[0].op == anim::AsmOp::Greater, "and to a float");

    check(asmRemoveParam(a, 0), "a parameter removes");
    check(a.machines[0].transitions[0].conditions.empty() && a.machines[0].states[0].speedParam.empty(),
          "taking its conditions and state references with it");
    check(a.valid(), "the asset is still valid after a removal");

    anim::AsmCondition c{"speed", anim::AsmOp::Greater, 0.1f};
    check(asmConditionText(c) == "speed > 0.1", "condition text: a comparison");
    c = {"grounded", anim::AsmOp::IsFalse, 0.0f};
    check(asmConditionText(c) == "grounded is false", "condition text: a bool");
    c = {"jump", anim::AsmOp::Trigger, 0.0f};
    check(asmConditionText(c) == "jump (trigger)", "condition text: a trigger");
    check(asmOpsFor(anim::AsmParamType::Trigger).size() == 1 && asmOpsFor(anim::AsmParamType::Float).size() == 6, "op lists by type");
}

static void geometry() {
    AVER_INFO("canvas geometry");
    AsmPt p = asmRectAnchor(0, 0, 10, 5, 100, 0);
    check(approx(p.x, 10.0f) && approx(p.y, 0.0f), "an anchor on the right edge");
    p = asmRectAnchor(0, 0, 10, 5, 0, -100);
    check(approx(p.x, 0.0f) && approx(p.y, -5.0f), "an anchor on the top edge");
    p = asmRectAnchor(0, 0, 10, 5, 100, 100);
    check(approx(p.x, 5.0f) && approx(p.y, 5.0f), "a diagonal leaves through the nearer edge");
    check(approx(asmDistToSegment(5, 3, 0, 0, 10, 0), 3.0f), "distance to a segment, perpendicular");
    check(approx(asmDistToSegment(-4, 3, 0, 0, 10, 0), 5.0f), "distance to a segment, past the end");
}

static void stateMachineTab() {
    AVER_INFO("state machine tab");
    const std::string path = (std::filesystem::temp_directory_path() / "aver-editor-test.ocasm").string();
    std::string why;
    check(anim::saveStateMachine(path, asmStarterMachine(), &why), "the fixture writes " + why);

    AnimStateMachineEditor ed(path);
    check(ed.loaded() && !ed.dirty() && !ed.canUndo(), "the tab loads clean");

    const i32 s = ed.addState(anim::AsmStateKind::Clip, 300.0f, 200.0f);
    check(s == 2 && ed.dirty() && ed.selectedState() == 2, "adding a state selects it");
    ed.undo();
    check(ed.asset().machines[0].states.size() == 2 && ed.selectedState() == -1, "undo removes it and clears the selection");
    ed.redo();
    check(ed.asset().machines[0].states.size() == 3, "redo restores it");

    const i32 t = ed.addTransition(0, 2);
    check(t >= 0 && ed.selectedTransition() == t, "adding a transition selects it");
    ed.deleteSelectedTransition();
    check(ed.asset().machines[0].transitions.size() == 2, "and it deletes");

    ed.selectState(2);
    ed.deleteSelectedState();
    check(ed.asset().machines[0].states.size() == 2, "a selected state deletes");

    const i32 sub = ed.addSubMachine(0.0f, 0.0f);
    check(sub >= 0 && ed.asset().machines.size() == 2, "a sub-machine adds with its machine");
    ed.openMachine(1);
    check(ed.currentMachine() == 1, "and can be opened");
    const i32 ex = ed.addTransition(0, anim::kAsmExit);
    check(ex >= 0, "inside it a transition to Exit is allowed");
    ed.openMachine(0);
    check(ed.addTransition(0, anim::kAsmExit) == -1, "at the root it is not");

    ed.setEntry(1);
    check(ed.asset().machines[0].entry == 1, "the entry state can change");

    check(ed.save(&why) && !ed.dirty(), "saves " + why);
    anim::AnimStateMachineAsset back;
    check(anim::loadStateMachine(path, back, &why) && back.machines.size() == 2 && back.machines[0].entry == 1, "the file has the edits");

    check(makeAnimStateMachineEditor(path) != nullptr && makeAnimStateMachineEditor("x.ocbt") == nullptr, "the factory claims only .ocasm");
    std::error_code ec;
    std::filesystem::remove(path, ec);
}

int main() {
    AVER_INFO("AnimGraphEditorTest");
    blendSpaceEdits();
    blendSpaceTab();
    stateMachineEdits();
    stateMachineParams();
    geometry();
    stateMachineTab();
    AVER_INFO(g_failures == 0 ? "AnimGraphEditorTest: PASS" : "AnimGraphEditorTest: FAIL");
    return g_failures == 0 ? 0 : 1;
}
