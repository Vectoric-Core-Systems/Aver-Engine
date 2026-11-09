// Editor: Modeling mode's viewport input (WAVE2_UE W5 step 6). Click and marquee picking is ModelingMode::handleMouse over ModelingPick.hpp; this is the
// part that knows the editor camera and the keyboard: the projector, the 1 / 2 / 3 select modes, Delete, and the mouse state.
#include "SandboxApp.hpp"

#if AVER_WITH_IMGUI

namespace aver {

void SandboxApp::handleModelingInput(const ImGuiIO& io, bool overScene, f32 mx, f32 my) {
    if (viewState_.levelFocused_ && !io.WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_1, false)) modeling_.setSelectMode(0);
        if (ImGui::IsKeyPressed(ImGuiKey_2, false)) modeling_.setSelectMode(1);
        if (ImGui::IsKeyPressed(ImGuiKey_3, false)) modeling_.setSelectMode(2);
        if (ImGui::IsKeyPressed(ImGuiKey_Delete, false)) modeling_.model.deleteSelection();
    }
    editor::ModelingMouse m;
    m.x = mx; m.y = my;
    m.overScene = overScene;
    m.down = io.MouseDown[0];
    m.clicked = io.MouseClicked[0];
    m.released = io.MouseReleased[0];
    m.ctrl = io.KeyCtrl;
    m.shift = io.KeyShift;
    modeling_.handleMouse(m, [this](const Vec3& w, f32& sx, f32& sy) { return project(w, sx, sy); }, viewState_.eye_);
}

}  // namespace aver

#endif
