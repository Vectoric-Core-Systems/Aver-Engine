#pragma once
// Show > World Origin: the floating origin's readout window (LARGE_WORLD 2.7). The rows come from WorldOriginReadout.hpp.
#include "WorldOriginReadout.hpp"

#if AVER_MODULE_SCENE && AVER_WITH_IMGUI

namespace aver::editor {

// Draws the window. `open` is the Show menu's checkbox (the close button clears it). `camLocalCm` is the camera the readout is about.
void drawWorldOriginPanel(bool& open, const game::OriginParticipants& origin, const Vec3& camLocalCm, float dpi);

} // namespace aver::editor

#endif
