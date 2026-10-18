#include "AccessibilityPreview.hpp"

#include "EditorPrefs.hpp"
#include "aver/rhi/ColourVision.hpp"

#include <algorithm>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

f32 AccessibilityPreview::packed() const {
    return active() ? rhi::packColourVision(static_cast<u32>(mode), simulate, strength) : 0.0f;
}

void AccessibilityPreview::load() {
    mode = std::clamp(prefInt("access.preview.mode", 0), 0, 3);
    simulate = prefBool("access.preview.simulate", false);
    strength = std::clamp(prefFloat("access.preview.strength", 1.0f), 0.0f, 1.0f);
}

void AccessibilityPreview::save() const {
    setPrefInt("access.preview.mode", mode);
    setPrefBool("access.preview.simulate", simulate);
    setPrefFloat("access.preview.strength", strength);
}

void applyPreviewToPost(const AccessibilityPreview& p, rhi::PostSettings& post) {
    if (!p.active() || post.colourVision != 0) return;
    post.colourVision = static_cast<u32>(p.mode) | (p.simulate ? 4u : 0u);
    post.colourVisionStrength = p.strength;
}

#if AVER_WITH_IMGUI
bool drawColourVisionPreviewMenu(AccessibilityPreview& p) {
    bool changed = false;
    if (!ImGui::BeginMenu("Colour Vision Preview")) return false;
    static const char* const kNames[] = {"Off", "Protanopia", "Deuteranopia", "Tritanopia"};
    for (int m = 0; m < 4; ++m)
        if (ImGui::MenuItem(kNames[m], nullptr, p.mode == m)) { p.mode = m; changed = true; }
    ImGui::Separator();
    if (ImGui::MenuItem("Correction", nullptr, !p.simulate)) { p.simulate = false; changed = true; }
    if (ImGui::MenuItem("Simulation", nullptr, p.simulate)) { p.simulate = true; changed = true; }
    ImGui::Separator();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::SliderFloat("Strength", &p.strength, 0.0f, 1.0f, "%.2f")) changed = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Filters the scene only, not the editor panels.\nSimulation shows what the viewer sees; Correction\nshifts colours so they can be told apart.");
    ImGui::EndMenu();
    if (changed) p.save();
    return changed;
}
#else
bool drawColourVisionPreviewMenu(AccessibilityPreview&) { return false; }
#endif

} // namespace aver::editor
