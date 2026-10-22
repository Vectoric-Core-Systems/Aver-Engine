// The "Physics (Jolt)" section of the profiler panel (Window > GPU Profiler): the stats line, Enable and Reset, and the scope tree
// from aver_phys_profile_* -- Jolt's own stages and the engine's fixed-step stages as one tree. Physics stays one row, "Gameplay
// tick + physics step", in the CPU phases table; this is the breakdown of that row. See PhysicsDebugOverlay.hpp.
#include "PhysicsDebugOverlay.hpp"

#if AVER_WITH_IMGUI && AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/physics/physics_world_abi.h"
#  include "imgui.h"

#  include <cstdint>
#endif

namespace aver::editor {

#if AVER_WITH_IMGUI && AVER_MODULE_PHYSICS

void drawPhysicsProfileSection(float dpi) {
    if (!ImGui::CollapsingHeader("Physics (Jolt)")) return;
    ImGui::PushID("physprof");
    if (!aver_phys_ready()) {
        ImGui::TextDisabled("No physics world.");
        ImGui::PopID();
        return;
    }

    double st[AVER_PHYS_ST_COUNT] = {};
    aver_phys_stats(st, AVER_PHYS_ST_COUNT);
    ImGui::Text("%d bodies (%d active dynamic), %d fixed step(s) last call, step %.3f ms avg / %.3f ms max",
                static_cast<int>(st[AVER_PHYS_ST_BODIES]), static_cast<int>(st[AVER_PHYS_ST_ACTIVE_DYNAMIC]),
                static_cast<int>(st[AVER_PHYS_ST_STEPS_LAST_CALL]), st[AVER_PHYS_ST_STEP_MS_AVG], st[AVER_PHYS_ST_STEP_MS_MAX]);
    if (st[AVER_PHYS_ST_UPDATE_ERRORS] != 0.0)
        ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.25f, 1.0f),
                           "The solver dropped contacts on %d step(s): raise PHYSICS.MAXCONTACTS / MAXBODYPAIRS.",
                           static_cast<int>(st[AVER_PHYS_ST_UPDATE_ERROR_STEPS]));

    if (!(aver_phys_build_flags() & AVER_PHYS_BUILD_PROFILER)) {
        ImGui::TextDisabled("This build has no profiler hook (AVER_PHYSICS_PROFILER is off).");
        ImGui::PopID();
        return;
    }

    const bool on = aver_phys_profile_is_enabled() != 0;
    if (ImGui::Button(on ? "Disable" : "Enable")) aver_phys_profile_enable(on ? 0 : 1);
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset")) aver_phys_profile_reset();
    ImGui::SameLine();
    ImGui::TextDisabled(on ? "collecting" : "off: costs one branch per scope");

    const int n = aver_phys_profile_count();
    if (n == 0) {
        ImGui::TextDisabled(on ? "Nothing yet: the table appears after a few fixed steps." : "Enable the profiler to fill the table.");
        ImGui::PopID();
        return;
    }
    if (ImGui::BeginTable("##physscopes", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Scope", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("incl ms", ImGuiTableColumnFlags_WidthFixed, 64.0f * dpi);
        ImGui::TableSetupColumn("excl ms", ImGuiTableColumnFlags_WidthFixed, 64.0f * dpi);
        ImGui::TableSetupColumn("max ms", ImGuiTableColumnFlags_WidthFixed, 64.0f * dpi);
        ImGui::TableSetupColumn("calls", ImGuiTableColumnFlags_WidthFixed, 56.0f * dpi);
        ImGui::TableHeadersRow();
        for (int i = 0; i < n; ++i) {
            uint8_t name[96] = {};
            double v[4] = {};
            int pd[2] = {};
            if (!aver_phys_profile_get(i, name, static_cast<int>(sizeof name), v, pd)) break;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (pd[1] > 0) ImGui::Indent(12.0f * dpi * static_cast<float>(pd[1]));
            ImGui::TextUnformatted(reinterpret_cast<const char*>(name));
            if (pd[1] > 0) ImGui::Unindent(12.0f * dpi * static_cast<float>(pd[1]));
            ImGui::TableSetColumnIndex(1); ImGui::Text("%.3f", v[0]);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.3f", v[1]);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%.3f", v[2]);
            ImGui::TableSetColumnIndex(4); ImGui::Text("%.1f", v[3]);
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Per fixed step, averaged over the last 60. \"Worker jobs\" is CPU time summed over the solver's threads.");
    ImGui::PopID();
}

#endif

} // namespace aver::editor
