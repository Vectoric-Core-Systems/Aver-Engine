#pragma once
// Small ImGui widget helpers shared across the editor's mode panels and asset tabs.
//
// panelFloat/panelInt started as static, private members of SandboxApp -- built for the landscape
// sculpt and foliage mode panels -- and so were reachable only from SandboxApp's own member
// functions. Every NEW editor that needed the same narrow-dock label fix had to reinvent it, or
// reintroduce the exact bug it exists to prevent (see panelFloat's own comment below). Moved here
// with NO BEHAVIOUR CHANGE: identical signatures, identical bodies, identical call sites -- this is
// a home, not a rewrite.
//
// CORRECTNESS HERE IS VISUAL ONLY. Nothing in this header is exercised by a headless test, and none
// should be claimed: these are ImGui layout helpers with nothing to assert against without a window.
// Verify by eye, in a narrow dock, at high DPI -- the exact scenario the header comment below
// describes.
#if AVER_WITH_IMGUI
#include "imgui.h"

#include "aver/core/Types.hpp"

#include <cstdio>

namespace aver::editor {

// Caption ABOVE the control, control full width below.
// ImGui's default puts the label to the RIGHT of a slider and does not clip it -- it just runs out
// of panel and disappears. In a 20%-width dock at 300% DPI that turned "Radius (cm)" into "Radiu".
// Every panel control that can appear in a narrow dock should go through here so none can regress.
inline bool panelFloat(const char* label, f32* v, f32 lo, f32 hi, const char* fmt,
                        ImGuiSliderFlags flags = 0) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    char id[96];
    std::snprintf(id, sizeof id, "##%s", label);
    return ImGui::SliderFloat(id, v, lo, hi, fmt, flags);
}

// Same fix, for an int slider.
inline bool panelInt(const char* label, int* v, int lo, int hi) {
    ImGui::TextUnformatted(label);
    ImGui::SetNextItemWidth(-1);
    char id[96];
    std::snprintf(id, sizeof id, "##%s", label);
    return ImGui::SliderInt(id, v, lo, hi);
}

} // namespace aver::editor
#endif // AVER_WITH_IMGUI
