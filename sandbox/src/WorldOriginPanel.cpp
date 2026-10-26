// Show > World Origin: region, origin and camera in km, the next shift's distance, the counters, and the last shift's cost by participant.
#include "WorldOriginPanel.hpp"

#if AVER_MODULE_SCENE && AVER_WITH_IMGUI

#include "imgui.h"

namespace aver::editor {

void drawWorldOriginPanel(bool& open, const game::OriginParticipants& origin, const Vec3& camLocalCm, float dpi) {
    if (!open) return;
    ImGui::SetNextWindowSize(ImVec2(380.0f * dpi, 0.0f), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("World Origin", &open, ImGuiWindowFlags_AlwaysAutoResize)) { ImGui::End(); return; }
    const WorldOriginReadout r = makeWorldOriginReadout(origin, camLocalCm);

    if (!r.enabled) ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.0f), "Floating origin is OFF (world.enable 1, or WORLD.FLOATINGORIGIN 1)");
    else ImGui::Text("Floating origin on, policy %s, margin %d cm", r.policy, r.marginCm);
    ImGui::Separator();
    ImGui::Text("Region (%lld, %lld)", static_cast<long long>(r.regionX), static_cast<long long>(r.regionY));
    ImGui::Text("Origin %.3f, %.3f km", r.originKmX, r.originKmY);
    ImGui::Text("Camera %.4f, %.4f, %.4f km (absolute)", r.camAbsKm[0], r.camAbsKm[1], r.camAbsKm[2]);
    ImGui::TextDisabled("Local %.0f, %.0f, %.0f cm", r.camLocalCm[0], r.camLocalCm[1], r.camLocalCm[2]);
    if (r.enabled) {
        const float limit = static_cast<float>(world::kRegionHalfCm + r.marginCm);
        const float used = limit > 0.0f ? static_cast<float>((limit - r.toTriggerCm) / limit) : 0.0f;
        char label[48];
        std::snprintf(label, sizeof label, "%.2f km to the next shift", r.toTriggerCm / 100000.0);
        ImGui::ProgressBar(used < 0.0f ? 0.0f : used > 1.0f ? 1.0f : used, ImVec2(-1.0f, 0.0f), label);
        if (r.nearTrigger) ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.2f, 1.0f), "Within 3 km of a shift.");
    }
    ImGui::Separator();
    ImGui::Text("Generation %u   shifts %u   deferrals %u", r.generation, r.shifts, r.deferrals);
    if (r.lastDeferReason) ImGui::TextDisabled("Last deferral: %s", r.lastDeferReason);
    ImGui::Text("Last shift %.3f ms", r.lastMs);
    if (r.shifts > 0 && ImGui::TreeNode("By participant")) {
        for (const WorldOriginReadout::Part& p : r.parts) ImGui::Text("%-10s %.3f ms", p.name, p.ms);
        ImGui::TreePop();
    }
    ImGui::End();
}

} // namespace aver::editor

#endif
