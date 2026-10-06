#pragma once
// Window > Soft Body (plastic): a side-on test bar for tuning a plastic material. Edit the yield
// force, break limits and creep, hit the bar, and watch the dent stay (green beams are unstrained,
// red have crept, broken ones disappear).
//
// THE HOOK INTO SandboxShell.cpp IS THREE LINES (see docs/SOFTBODY.md "Wiring"):
//   #include "SoftBodyPanel.hpp"
//   ImGui::MenuItem("Soft Body (plastic)", nullptr, &showSoftBody_);   // in the Window menu
//   editor::softBodyPanelDraw(softBody_, &showSoftBody_);               // beside the other panels
// with `bool showSoftBody_ = false; editor::SoftBodyPanelState softBody_;` as members.
//
// The state and its operations are plain functions with no ImGui in them, so a test can drive them;
// only softBodyPanelDraw needs a UI.
#include "aver/softbody/Cage.hpp"

namespace aver::editor {

struct SoftBodyPanelState {
    softbody::Material material;       // what the bar is made of (edited live)
    softbody::StepConfig cfg;
    softbody::Cage cage;
    u32 tipTop = 0, tipBot = 0;
    f32 barLengthCm = 100.0f;
    f32 hitDepthCm = 25.0f;
    f32 accumulator = 0.0f;
    bool built = false;
    bool running = true;
    bool settled = true;
};

// (Re)builds the bar from `material`. Called on first draw and when the material is changed.
void softBodyPanelRebuild(SoftBodyPanelState& s);
// Hits the bar from above at fraction `along` (0 root, 1 tip) of its length.
void softBodyPanelHit(SoftBodyPanelState& s, f32 along);
// Undoes every dent and tear without rebuilding.
void softBodyPanelRepair(SoftBodyPanelState& s);
// Advances by `realDt` seconds in fixed steps (at most four per call); returns the steps run.
int  softBodyPanelAdvance(SoftBodyPanelState& s, f32 realDt);
// The largest permanent set over the bar's beams, cm.
f32  softBodyPanelMaxSet(const SoftBodyPanelState& s);

// Draws the window. A no-op in a build without ImGui.
void softBodyPanelDraw(SoftBodyPanelState& s, bool* open);

} // namespace aver::editor
