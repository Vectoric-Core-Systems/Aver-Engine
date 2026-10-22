// Project Settings > Physics: the solver, sleeping, continuous-collision, determinism and limits groups
// (wave 11, WORLD_STATE_DEBUG). Everything with a decision in it lives in PhysicsSettingsModel.hpp; this file
// is the widgets. The caller (SandboxApp::buildPhysicsSettings) keeps gravity and tick rate and calls
// drawPhysicsSettingsGroups below them.
//
// This TU also holds the phys.* console registration (PhysicsConsoleVars.hpp): it is a new, small file, and the
// console header is included by translation units that must not grow.
#include "PhysicsSettingsModel.hpp"
#include "PhysicsConsoleVars.hpp"

#if AVER_WITH_IMGUI && AVER_MODULE_PHYSICS
#  include "aver/physics/physics_abi.h"
#  include "aver/physics/physics_state_abi.h"
#  include "imgui.h"

#  include <cstring>
#  include <string>
#  include <vector>
#endif

namespace aver::editor {

#if AVER_WITH_IMGUI && AVER_MODULE_PHYSICS

namespace {

// Which group a setting sits in; anything unlisted lands in "More solver settings".
struct Group {
    const char* title;
    std::vector<const char*> names;
};

const std::vector<Group>& groups() {
    static const std::vector<Group> g = {
        {"Solver", {"velocitySteps", "positionSteps", "baumgarte", "speculativeDistance", "penetrationSlop",
                    "maxPenetration", "manifoldTolerance", "minRestitutionSpeed"}},
        {"Sleeping", {"allowSleeping", "sleepTime", "sleepSpeed"}},
        {"Continuous collision", {"collisionSteps", "linearCastThreshold", "linearCastMaxPenetration"}},
        {"Determinism", {"deterministic"}},
    };
    return g;
}

bool listed(const char* name) {
    for (const Group& g : groups())
        for (const char* n : g.names) if (std::strcmp(n, name) == 0) return true;
    return false;
}

// One setting: widget by kind, unit, changed marker, tooltip and a per-row reset.
void settingRowUi(fmt::ProjectDesc& project, bool& dirty, const PhysicsSettingRow& r, float dpi) {
    ImGui::PushID(r.key);
    const float cur = physicsSettingValue(project, r);
    const bool changed = physicsSettingChanged(project, r);
    float next = cur;
    bool edited = false;
    ImGui::SetNextItemWidth(160.0f * dpi);
    if (r.isBool) {
        bool b = cur != 0.0f;
        if (ImGui::Checkbox(r.name.c_str(), &b)) { next = b ? 1.0f : 0.0f; edited = true; }
    } else if (r.isCount) {
        int i = static_cast<int>(cur);
        if (ImGui::DragInt(r.name.c_str(), &i, 1.0f, static_cast<int>(r.min), static_cast<int>(r.max),
                           "%d", ImGuiSliderFlags_AlwaysClamp)) { next = static_cast<float>(i); edited = true; }
    } else {
        float f = cur;
        const float speed = (r.max - r.min) * 0.002f;
        if (ImGui::DragFloat(r.name.c_str(), &f, speed, r.min, r.max, "%.4g", ImGuiSliderFlags_AlwaysClamp)) {
            next = f;
            edited = true;
        }
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", physicsSettingTooltip(r).c_str());
    if (!r.isBool) { ImGui::SameLine(); ImGui::TextDisabled("%s", r.unit.c_str()); }
    if (changed) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1.0f), "(default %s)", physicsNumberText(r.def).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("reset")) {
            if (resetPhysicsSetting(project, r)) dirty = true;
        }
    }
    if (edited && setPhysicsSettingValue(project, r, next)) dirty = true;
    ImGui::PopID();
}

void limitRowUi(fmt::ProjectDesc& project, bool& dirty, const PhysicsLimitRow& r, float dpi) {
    ImGui::PushID(100 + r.key);
    int i = static_cast<int>(physicsLimitValue(project, r));
    ImGui::SetNextItemWidth(160.0f * dpi);
    // Enter commits: a half-typed 6 on the way to 65536 must not become a limit.
    if (ImGui::InputInt(r.name.c_str(), &i, 1, 1024, ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (setPhysicsLimit(project, r, static_cast<float>(i))) dirty = true;
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("%s", physicsLimitTooltip(r).c_str());
    if (physicsLimitChanged(project, r)) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.72f, 0.25f, 1.0f), "(default %s)", physicsNumberText(r.def).c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("reset")) {
            if (resetPhysicsLimit(project, r)) dirty = true;
        }
    }
    ImGui::PopID();
}

} // namespace

void drawPhysicsSettingsGroups(fmt::ProjectDesc& project, bool& dirty, float dpi) {
    static const std::vector<PhysicsSettingRow> rows = physicsSettingRows();
    static const std::vector<PhysicsLimitRow> limits = physicsLimitRows();
    const auto find = [&](const char* name) -> const PhysicsSettingRow* {
        for (const PhysicsSettingRow& r : rows) if (r.name == name) return &r;
        return nullptr;
    };

    ImGui::Spacing();
    const int changed = physicsChangedCount(project, rows);
    if (changed > 0) {
        ImGui::TextDisabled("%d solver setting(s) differ from Jolt's defaults. Defaults are not written to the project.", changed);
        ImGui::SameLine();
        if (ImGui::SmallButton("reset all")) {
            for (const PhysicsSettingRow& r : rows)
                if (resetPhysicsSetting(project, r)) dirty = true;
        }
    }

    for (const Group& g : groups()) {
        if (!ImGui::CollapsingHeader(g.title, ImGuiTreeNodeFlags_DefaultOpen)) continue;
        for (const char* n : g.names)
            if (const PhysicsSettingRow* r = find(n)) settingRowUi(project, dirty, *r, dpi);
        if (std::strcmp(g.title, "Determinism") == 0) {
            const PhysicsSettingRow* det = find("deterministic");
            if (det && physicsSettingValue(project, *det) == 0.0f)
                ImGui::TextColored(ImVec4(0.9f, 0.35f, 0.3f, 1.0f),
                                   "Off: faster, but state snapshots and replays will no longer match.");
            char build[160] = {};
            aver_phys_build_string(reinterpret_cast<uint8_t*>(build), static_cast<int32_t>(sizeof build));
            ImGui::TextDisabled("build: %s", build);
            // The state hash (physics_state_abi.h): equal for two worlds in the same state, so a run, a restore or a thread count can be compared.
            static unsigned long long shownHash = 0;
            static long long shownStep = 0;
            ImGui::BeginDisabled(!aver_phys_ready());
            if (ImGui::SmallButton("Hash state now")) {
                shownHash = static_cast<unsigned long long>(aver_phys_state_hash(0));
                shownStep = static_cast<long long>(aver_phys_step_total());
            }
            ImGui::EndDisabled();
            if (shownHash != 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("%016llx at fixed step %lld", shownHash, shownStep);
            }
        }
    }

    if (ImGui::CollapsingHeader("More solver settings")) {
        for (const PhysicsSettingRow& r : rows)
            if (!listed(r.name.c_str())) settingRowUi(project, dirty, r, dpi);
    }

    if (ImGui::CollapsingHeader("Limits and threads", ImGuiTreeNodeFlags_DefaultOpen)) {
        for (const PhysicsLimitRow& r : limits) limitRowUi(project, dirty, r, dpi);
        if (physicsRestartBadge()) {
            ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.25f, 1.0f), "restart world");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("The limits above differ from the ones the running world was built with.\n"
                                  "They apply when the world is next created: reopen the project, or rebuild it\n"
                                  "here once the level is closed.");
            ImGui::SameLine();
        }
        ImGui::BeginDisabled(!physicsRebuildEnabled());
        if (ImGui::Button("Rebuild world")) aver_phys_rebuild();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Creates the physics world again with the limits above. Only when the world is empty\n"
                              "(no bodies, characters, joints or vehicles): close the level first.");

        if (aver_phys_ready()) {
            double st[AVER_PHYS_ST_COUNT] = {};
            aver_phys_stats(st, AVER_PHYS_ST_COUNT);
            ImGui::TextDisabled("world: %d / %d bodies (%d active dynamic), %d worker thread(s)",
                                static_cast<int>(st[AVER_PHYS_ST_BODIES]), static_cast<int>(st[AVER_PHYS_ST_MAX_BODIES]),
                                static_cast<int>(st[AVER_PHYS_ST_ACTIVE_DYNAMIC]), static_cast<int>(st[AVER_PHYS_ST_WORKER_THREADS]));
            ImGui::TextDisabled("step %.3f ms avg, %.3f ms max; temp memory peak %.1f of %.0f MB",
                                st[AVER_PHYS_ST_STEP_MS_AVG], st[AVER_PHYS_ST_STEP_MS_MAX],
                                st[AVER_PHYS_ST_TEMP_PEAK_BYTES] / 1048576.0, st[AVER_PHYS_ST_TEMP_SIZE_BYTES] / 1048576.0);
            if (st[AVER_PHYS_ST_UPDATE_ERRORS] != 0.0)
                ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.25f, 1.0f),
                                   "The solver dropped contacts (%d step(s)): raise PHYSICS.MAXCONTACTS / MAXBODYPAIRS.",
                                   static_cast<int>(st[AVER_PHYS_ST_UPDATE_ERROR_STEPS]));
        } else {
            ImGui::TextDisabled("No physics world yet -- the limits apply when one is created.");
        }
    }
}

#else

void drawPhysicsSettingsGroups(fmt::ProjectDesc&, bool&, float) {}

#endif

} // namespace aver::editor
