// Project Settings > Physics > Surfaces and the material inspector's Physical Surface combo (SHAPES_MATERIALS). Every rule is in
// PhysicalSurfaceTableModel.hpp (header-only, tested); this file is the widgets. In its own translation unit because the files that
// call it are at or near the Debug COFF section cap.
#include "PhysicalSurfaceTableModel.hpp"

#if AVER_WITH_IMGUI

#include "aver/core/Log.hpp"
#include "aver/game/PhysicalSurfaces.hpp"
#include "aver/platform/FileSystem.hpp"

#include "imgui.h"

#include <cfloat>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>

namespace aver::editor {
namespace {

namespace sm = surfmodel;

const ImVec4 kRed{0.95f, 0.35f, 0.25f, 1.0f};

struct PanelState {
    std::string loadedFor;     // the content dir the table was read from
    fmt::OcSurfData table;     // being edited
    fmt::OcSurfData saved;     // as on disk
    bool dirty = false;
    std::string status;
};

PanelState& panelState() {
    static PanelState s;
    return s;
}

void loadTable(PanelState& s, const std::string& contentDir) {
    s = PanelState{};
    s.loadedFor = contentDir;
    const std::string path = game::physicalSurfacesPath(contentDir);
    std::error_code ec;
    if (path.empty() || !std::filesystem::exists(path, ec)) {
        s.status = "No Surfaces.ocsurf in this project yet; Save creates it.";
        return;
    }
    std::string err;
    if (!fmt::loadOcSurf(path, s.table, &err)) {
        s.status = "Could not load the table: " + err;
        s.table = fmt::OcSurfData{};
        return;
    }
    s.saved = s.table;
}

void saveTable(PanelState& s, const std::string& contentDir) {
    if (!sm::canSave(s.table)) {
        s.status = "Fix the marked rows first.";
        return;
    }
    const std::string path = game::physicalSurfacesPath(contentDir);
    std::string err;
    if (!fmt::saveOcSurf(path, s.table, &err)) {
        s.status = "Could not save: " + err;
        AVER_ERROR("[Surfaces] {}", err);
        return;
    }
    // Register is an upsert: the numbers are live for the next contact of every body on those surfaces (and the next Play).
    const usize changed = sm::changedRows(s.saved, s.table).size();
    game::registerPhysicalSurfaces(s.table);
    s.saved = s.table;
    s.dirty = false;
    s.status = "Saved " + std::to_string(s.table.surfaces.size()) + " surface(s); " + std::to_string(changed) + " registered with the physics module.";
    AVER_INFO("[Surfaces] saved {}", path);
}

} // namespace

void drawPhysicalSurfaceTable(const std::string& contentDir, float dpi) {
    PanelState& s = panelState();
    ImGui::Separator();
    ImGui::TextUnformatted("Surfaces");
    ImGui::SameLine();
    ImGui::TextDisabled("(Content/Physics/Surfaces.ocsurf)");
    if (contentDir.empty()) {
        ImGui::TextDisabled("Open a project to edit its physical surfaces.");
        return;
    }
    if (s.loadedFor != contentDir) loadTable(s, contentDir);

    ImGui::TextDisabled("A surface is a name. Materials, landscape layers and shapes refer to it; friction and restitution left 'unset' defer to the body.");
    const std::vector<sm::RowProblem> problems = sm::problems(s.table);
    auto problemOf = [&](usize row) -> const sm::RowProblem* {
        for (const sm::RowProblem& p : problems)
            if (p.row == row) return &p;
        return nullptr;
    };

    if (ImGui::BeginTable("##surfaces", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Friction", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Restitution", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Tag", ImGuiTableColumnFlags_WidthFixed, 70.0f * dpi);
        ImGui::TableSetupColumn("##move", ImGuiTableColumnFlags_WidthFixed, 56.0f * dpi);
        ImGui::TableSetupColumn("##del", ImGuiTableColumnFlags_WidthFixed, 28.0f * dpi);
        ImGui::TableHeadersRow();

        int removeAt = -1, moveRow = -1, moveBy = 0;
        for (usize i = 0; i < s.table.surfaces.size(); ++i) {
            fmt::OcSurfRecord& r = s.table.surfaces[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::TableNextRow();
            const sm::RowProblem* bad = problemOf(i);

            ImGui::TableNextColumn();
            char buf[80];
            std::snprintf(buf, sizeof buf, "%s", r.name.c_str());
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (bad) ImGui::PushStyleColor(ImGuiCol_Text, kRed);
            if (ImGui::InputText("##name", buf, sizeof buf)) { r.name = buf; s.dirty = true; }
            if (bad) {
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", bad->text.c_str());
            }

            // Below zero reads "unset": the surface names the thing without changing how anything slides.
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat("##friction", &r.friction, 0.01f, -1.0f, 10.0f, r.friction < 0.0f ? "unset" : "%.2f")) {
                if (r.friction < 0.0f) r.friction = -1.0f;
                s.dirty = true;
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragFloat("##restitution", &r.restitution, 0.01f, -1.0f, 10.0f, r.restitution < 0.0f ? "unset" : "%.2f")) {
                if (r.restitution < 0.0f) r.restitution = -1.0f;
                s.dirty = true;
            }
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::DragInt("##tag", &r.tag, 0.2f)) s.dirty = true;

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("^")) { moveRow = static_cast<int>(i); moveBy = -1; }
            ImGui::SameLine();
            if (ImGui::SmallButton("v")) { moveRow = static_cast<int>(i); moveBy = 1; }
            ImGui::TableNextColumn();
            if (ImGui::SmallButton("x")) removeAt = static_cast<int>(i);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remove this surface. It stays registered until the editor restarts.");
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (moveRow >= 0 && sm::moveRow(s.table, static_cast<usize>(moveRow), moveBy)) s.dirty = true;
        if (removeAt >= 0 && sm::removeRow(s.table, static_cast<usize>(removeAt))) s.dirty = true;
    }

    if (ImGui::Button("Add surface")) { sm::addRow(s.table); s.dirty = true; }
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.dirty);
    if (ImGui::Button("Revert")) loadTable(s, contentDir);
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!s.dirty || !problems.empty());
    if (ImGui::Button("Save")) saveTable(s, contentDir);
    ImGui::EndDisabled();
    if (!problems.empty()) { ImGui::SameLine(); ImGui::TextColored(kRed, "%d row(s) need fixing", static_cast<int>(problems.size())); }
    else if (s.dirty) { ImGui::SameLine(); ImGui::TextDisabled("unsaved changes"); }
    if (!s.status.empty()) ImGui::TextDisabled("%s", s.status.c_str());
}

void drawMaterialPhysSurface(const std::string& binariesDir, const std::string& contentDir, const std::string& materialName) {
    ImGui::Separator();
    ImGui::TextUnformatted("Physics");
    if (materialName.empty() || contentDir.empty()) {
        ImGui::TextDisabled("Physical Surface: open a project and name the material first.");
        return;
    }

    // The file physics reads for a slot of this name: the compiled one under Binaries first, then Content (GameContent.cpp, the same order).
    static std::string s_key, s_path, s_text, s_current;
    static int s_lastCheck = -1000;
    const int frame = ImGui::GetFrameCount();
    if (s_key != materialName || frame - s_lastCheck > 30) {
        s_key = materialName;
        s_lastCheck = frame;
        s_path.clear();
        s_text.clear();
        const std::string candidates[3] = {
            binariesDir + AVER_PATH_SEP "Materials" AVER_PATH_SEP + materialName + ".ocmat",
            contentDir + AVER_PATH_SEP "Materials" AVER_PATH_SEP + materialName + ".ocmat",
            contentDir + AVER_PATH_SEP + materialName,
        };
        for (const std::string& c : candidates) {
            std::error_code ec;
            if (!binariesDir.empty() || c != candidates[0]) {
                if (std::filesystem::exists(c, ec) && readFileText(c, s_text)) { s_path = c; break; }
            }
        }
        s_current = s_path.empty() ? std::string() : sm::readPhysSurface(s_text);
    }
    if (s_path.empty()) {
        ImGui::TextDisabled("Physical Surface: this material has no .ocmat file yet (Compile C# writes one).");
        return;
    }

    PanelState& s = panelState();
    if (s.loadedFor != contentDir) loadTable(s, contentDir);

    const auto apply = [&](const std::string& name) {
        const std::string patched = sm::patchPhysSurface(s_text, name);
        if (patched == s_text) return;
        if (!writeFileTextAtomic(s_path, patched)) {
            AVER_ERROR("[Surfaces] could not write {}", s_path);
            return;
        }
        s_text = patched;
        s_current = name;
        AVER_INFO("[Surfaces] {}: PHYSSURFACE {}", s_path, name.empty() ? std::string("(none)") : name);
    };

    if (ImGui::BeginCombo("Physical Surface", s_current.empty() ? "(none)" : s_current.c_str())) {
        if (ImGui::Selectable("(none)", s_current.empty())) apply({});
        for (const fmt::OcSurfRecord& r : s.saved.surfaces)
            if (ImGui::Selectable(r.name.c_str(), r.name == s_current)) apply(r.name);
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The surface a collision mesh built from this material's slot carries (friction, restitution and tag come from\n"
                          "Project Settings > Physics > Surfaces). Written to:\n%s\n"
                          "Compile C# regenerates the files under Binaries and drops this record: author it in\n"
                          "Content/Materials/<name>.ocmat for a lasting value.", s_path.c_str());
    // A name not in the table is allowed (the surface is then just a name); type it here.
    static char s_free[80] = {};
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::InputTextWithHint("##physFree", "other name + Enter", s_free, sizeof s_free, ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (fmt::ocSurfNameValid(s_free)) { apply(s_free); s_free[0] = 0; }
    }
    if (std::strlen(s_free) > 0 && !fmt::ocSurfNameValid(s_free)) { ImGui::SameLine(); ImGui::TextColored(kRed, "letters, digits, underscore"); }
}

} // namespace aver::editor

#endif // AVER_WITH_IMGUI
