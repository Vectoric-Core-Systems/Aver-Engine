#include "DataLayersPanel.hpp"

#if AVER_WITH_IMGUI

#include "EditorIcons.hpp"

#include "imgui.h"

#include <cfloat>
#include <cstring>

namespace aver::editor {
namespace {

const char* stateName(fmt::OcDataLayerState s) {
    switch (s) {
        case fmt::OcDataLayerState::Unloaded: return "Unloaded";
        case fmt::OcDataLayerState::Loaded: return "Loaded";
        case fmt::OcDataLayerState::Activated: return "Activated";
    }
    return "Activated";
}

void startRename(DataLayersUi& ui, const std::string& name) {
    ui.renaming = name;
    std::strncpy(ui.nameBuf, name.c_str(), sizeof(ui.nameBuf) - 1);
    ui.nameBuf[sizeof(ui.nameBuf) - 1] = 0;
    ui.focusName = true;
}

}  // namespace

void drawDataLayersPanel(bool* open, dl::Model& model, DataLayersUi& ui, const DataLayersHost& host, f32 dpi) {
    if (!open || !*open) return;
    ImGui::SetNextWindowSize(ImVec2(560.0f * dpi, 360.0f * dpi), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Data Layers", open)) { ImGui::End(); return; }

    const auto push = [&](dl::Edit e) {
        if (e.empty()) return;
        if (host.push) host.push(std::move(e));
    };
    if (!model.hasLayer(ui.selected)) ui.selected.clear();

    if (ImGui::Button(ICON_ADD " New")) {
        const std::string name = model.freshName();
        push(model.create(name));
        ui.selected = name;
        startRename(ui, name);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(ui.selected.empty());
    if (ImGui::Button(ICON_ADD " New Child")) {
        const std::string name = model.freshName();
        push(model.create(name, ui.selected));
        ui.selected = name;
        startRename(ui, name);
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_DELETE " Delete")) {
        push(model.remove(ui.selected));
        ui.selected.clear();
        if (host.refreshVisibility) host.refreshVisibility();
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Deletes the layer. Its children move up; its objects stay in the level.");

    const std::vector<dl::Key> sel = host.selection ? host.selection() : std::vector<dl::Key>{};
    ImGui::BeginDisabled(ui.selected.empty() || sel.empty());
    if (ImGui::Button("Assign Selection")) push(model.assign(sel, ui.selected));
    ImGui::SameLine();
    if (ImGui::Button("Remove Selection")) push(model.unassign(sel, ui.selected));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(ui.selected.empty());
    if (ImGui::Button("Select Members") && host.selectKeys) host.selectKeys(model.membersOf(ui.selected));
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::TextDisabled("%zu selected", sel.size());

    const std::vector<dl::Model::Row> rows = model.rows();
    if (rows.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("No data layers. A layer groups objects so the streamer can load, hide or activate them together; the level saves them as DATALAYER records.");
    } else if (ImGui::BeginTable("##layers", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Show", ImGuiTableColumnFlags_WidthFixed, 40.0f * dpi);
        ImGui::TableSetupColumn("Loaded", ImGuiTableColumnFlags_WidthFixed, 54.0f * dpi);
        ImGui::TableSetupColumn("Layer", ImGuiTableColumnFlags_None, 3.0f);
        ImGui::TableSetupColumn("Objects", ImGuiTableColumnFlags_WidthFixed, 56.0f * dpi);
        ImGui::TableSetupColumn("Initial state", ImGuiTableColumnFlags_WidthFixed, 110.0f * dpi);
        ImGui::TableSetupColumn("Runtime", ImGuiTableColumnFlags_WidthFixed, 56.0f * dpi);
        ImGui::TableHeadersRow();

        for (const dl::Model::Row& row : rows) {
            const fmt::OcDataLayer* layer = model.find(row.name);
            if (!layer) continue;
            ImGui::PushID(row.name.c_str());
            ImGui::TableNextRow();

            const dl::EditorFlags flags = model.flagsOf(row.name);
            ImGui::TableSetColumnIndex(0);
            bool vis = flags.visible;
            if (ImGui::Checkbox("##vis", &vis)) { model.setEditorVisible(row.name, vis); if (host.refreshVisibility) host.refreshVisibility(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Editor only: draw this layer's objects in the viewport. Not saved.");
            ImGui::TableSetColumnIndex(1);
            bool loaded = flags.loaded;
            if (ImGui::Checkbox("##loaded", &loaded)) { model.setEditorLoaded(row.name, loaded); if (host.refreshVisibility) host.refreshVisibility(); }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Editor only: an unloaded layer's objects are hidden and cannot be picked,\nso a big level opens light. They stay in the level and save as before.");

            ImGui::TableSetColumnIndex(2);
            if (static_cast<f32>(row.depth) * 14.0f * dpi > 0.0f) ImGui::Indent(static_cast<f32>(row.depth) * 14.0f * dpi);   // Indent(0) would apply the default indent
            if (ui.renaming == row.name) {
                if (ui.focusName) { ImGui::SetKeyboardFocusHere(); ui.focusName = false; }
                ImGui::SetNextItemWidth(-FLT_MIN);
                const bool enter = ImGui::InputText("##rename", ui.nameBuf, sizeof(ui.nameBuf), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
                if (enter) {
                    const std::string to = ui.nameBuf;
                    if (to != row.name) {
                        dl::Edit e = model.rename(row.name, to);
                        if (!e.empty()) ui.selected = to;
                        push(std::move(e));
                    }
                    ui.renaming.clear();
                } else if (ImGui::IsItemDeactivated()) {
                    ui.renaming.clear();
                }
            } else {
                if (ImGui::Selectable(row.name.c_str(), ui.selected == row.name, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                    ui.selected = row.name;
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) startRename(ui, row.name);
            }
            if (static_cast<f32>(row.depth) * 14.0f * dpi > 0.0f) ImGui::Unindent(static_cast<f32>(row.depth) * 14.0f * dpi);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%zu", model.memberCount(row.name));

            ImGui::TableSetColumnIndex(4);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::BeginCombo("##initial", stateName(layer->initial))) {
                for (const fmt::OcDataLayerState s : {fmt::OcDataLayerState::Unloaded, fmt::OcDataLayerState::Loaded, fmt::OcDataLayerState::Activated})
                    if (ImGui::Selectable(stateName(s), s == layer->initial)) push(model.setInitial(row.name, s));
                ImGui::EndCombo();
            }
            ImGui::TableSetColumnIndex(5);
            bool rt = layer->runtime;
            if (ImGui::Checkbox("##rt", &rt)) push(model.setRuntime(row.name, rt));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("The running game may change this layer's state. Off: it keeps its initial state.");
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::End();
}

}  // namespace aver::editor

#else

namespace aver::editor {
void drawDataLayersPanel(bool*, dl::Model&, DataLayersUi&, const DataLayersHost&, f32) {}
}  // namespace aver::editor

#endif
