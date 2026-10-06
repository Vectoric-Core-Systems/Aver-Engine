#pragma once
// The animation state-machine editor tab: an .ocasm opened as an asset, drawn as a node graph of
// states and transitions, with a details panel and a simulator that runs the machine against sliders.
//
// THE HOOK INTO SandboxApp.cpp IS TWO LINES, matching BtEditor.hpp's note:
//   #include "AnimStateMachineEditor.hpp"
//   assetEditors_.registerFactory(&editor::makeAnimStateMachineEditor);   // APPENDED
// plus the file in sandbox/CMakeLists.txt.
#include "AssetEditor.hpp"
#include "SnapshotUndo.hpp"

#include "aver/anim/AnimStateMachine.hpp"

#include <memory>
#include <string>
#include <vector>

namespace aver::editor {

// ---- edits as FREE FUNCTIONS, so a headless test reaches them (see BtEditor.hpp for the argument) ----
//
// Every index in an .ocasm is positional (transitions name states by index, states name sub-machines
// by index), so each removal here renumbers what points past it. Doing that in one place is the point.

// A valid starter: Idle <-> Walk on a `speed` parameter.
anim::AnimStateMachineAsset asmStarterMachine();

// A name not already used by a state of `m`: `base`, then `base 2`, `base 3`...
std::string asmUniqueStateName(const anim::AsmMachine& m, const std::string& base);

// Appends a Clip or BlendSpace state. A position of exactly (0, 0) means "lay it out for me" on a
// four-wide grid; anything else is used as given. Returns its index.
i32 asmAddState(anim::AsmMachine& m, anim::AsmStateKind kind, const std::string& name, f32 x, f32 y);

// Appends a SubMachine state AND the machine it names (one default state inside). Returns the new
// state's index in machine `machine`, or -1 when `machine` is out of range.
i32 asmAddSubMachine(anim::AnimStateMachineAsset& a, i32 machine, const std::string& name, f32 x, f32 y);

// Removes a state, the transitions touching it, and for a SubMachine state the machines it owned.
// `machine` is updated if removing owned machines shifted its own index. False when it would leave the
// machine empty or an index is out of range.
bool asmRemoveState(anim::AnimStateMachineAsset& a, i32& machine, i32 state);

// The machine that holds the state naming `machine` as its sub-machine, or -1 (the root, or orphan).
i32 asmParentMachine(const anim::AnimStateMachineAsset& a, i32 machine);

// Adds a transition. A transition from a state starts with an exit time (so it cannot fire the
// instant it exists); one from Any starts with a condition on the first parameter, if there is one.
// Returns its index, or -1 for an out-of-range source or target.
i32 asmAddTransition(const anim::AnimStateMachineAsset& a, anim::AsmMachine& m, i32 from, i32 to, bool isRoot);
bool asmRemoveTransition(anim::AsmMachine& m, i32 index);
// Moves a transition within the list; the order is the priority. Returns the new index or -1.
i32 asmMoveTransition(anim::AsmMachine& m, i32 index, i32 delta);

// Parameters. Names are unique and non-empty; refusals return -1 / false.
i32 asmAddParam(anim::AnimStateMachineAsset& a, const std::string& name, anim::AsmParamType type, f32 def);
bool asmRemoveParam(anim::AnimStateMachineAsset& a, i32 index);
bool asmRenameParam(anim::AnimStateMachineAsset& a, i32 index, const std::string& name);
// Changes a parameter's type and repairs every condition on it so the op still makes sense.
void asmSetParamType(anim::AnimStateMachineAsset& a, i32 index, anim::AsmParamType type);

anim::AsmOp asmDefaultOp(anim::AsmParamType type);
// The ops offered for a parameter of that type.
std::vector<anim::AsmOp> asmOpsFor(anim::AsmParamType type);
const char* asmOpLabel(anim::AsmOp op);
bool asmOpTakesValue(anim::AsmOp op);
// "speed > 0.1", "grounded is false", "jump (trigger)".
std::string asmConditionText(const anim::AsmCondition& c);

// Canvas geometry, ImGui-free. The point on the border of the box centred (cx, cy) with half-extents
// (hw, hh) that faces (tx, ty).
struct AsmPt {
    f32 x = 0, y = 0;
};
AsmPt asmRectAnchor(f32 cx, f32 cy, f32 hw, f32 hh, f32 tx, f32 ty);
f32 asmDistToSegment(f32 px, f32 py, f32 ax, f32 ay, f32 bx, f32 by);

class AnimStateMachineEditor final : public AssetEditor {
public:
    explicit AnimStateMachineEditor(std::string path);

    const std::string& path() const override { return path_; }
    std::string title() const override;
    bool dirty() const override { return dirty_; }
    void draw(Engine& e) override;
    bool save(std::string* why) override;
    void onFileChanged() override;

    bool loaded() const { return loaded_; }
    const std::string& loadError() const { return loadError_; }
    const anim::AnimStateMachineAsset& asset() const { return asset_; }
    i32 currentMachine() const { return machine_; }
    i32 selectedState() const { return selState_; }
    i32 selectedTransition() const { return selTrans_; }
    void selectState(i32 s) { selState_ = s; selTrans_ = -1; }
    void selectTransition(i32 t) { selTrans_ = t; selState_ = -1; }
    void openMachine(i32 m);

    void pushUndo() { history_.push(asset_); }
    void undo();
    void redo();
    bool canUndo() const { return history_.canUndo(); }
    bool canRedo() const { return history_.canRedo(); }

    // Each pushes one undo entry; the ImGui layer calls these, and so do tests.
    i32 addState(anim::AsmStateKind kind, f32 x, f32 y);
    i32 addSubMachine(f32 x, f32 y);
    void deleteSelectedState();
    i32 addTransition(i32 from, i32 to);
    void deleteSelectedTransition();
    void setEntry(i32 state);
    void markDirty() { dirty_ = true; simStale_ = true; }

private:
    void loadFromDisk();

    std::string path_;
    anim::AnimStateMachineAsset asset_;
    bool loaded_ = false;
    std::string loadError_;
    bool dirty_ = false;
    SnapshotUndo<anim::AnimStateMachineAsset> history_;

    i32 machine_ = 0;          // the machine the canvas is showing
    i32 selState_ = -1;
    i32 selTrans_ = -1;

    // Simulator.
    anim::AnimStateMachine sim_;
    bool simOn_ = false;
    bool simStale_ = true;
    std::vector<anim::AsmEvent> simLog_;

#if AVER_WITH_IMGUI
    f32 panX_ = 0, panY_ = 0;
    i32 dragState_ = -1;
    i32 linkFrom_ = -3;        // -3 = not linking; kAsmAny (-1) is a valid source
    f32 popupX_ = 0, popupY_ = 0;
    bool gesture_ = false;
    void drawCanvas();
    void drawDetails();
    void drawSimulator();
#endif
};

std::unique_ptr<AssetEditor> makeAnimStateMachineEditor(const std::string& path);

} // namespace aver::editor
