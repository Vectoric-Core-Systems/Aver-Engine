#include "LevelInstanceEditorUi.hpp"
#include "EditorChrome.hpp"

#if AVER_MODULE_SCENE
#include <cstdio>
#include <cstring>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

void levelInstanceCreateDialogOpen(LevelInstanceCreateDialog& d, const std::vector<scene::Entity>& sources,
                                   const std::string& suggestedName) {
    d.sources = sources;
    std::snprintf(d.name, sizeof d.name, "%s", suggestedName.c_str());
    d.error.clear();
    d.wantOpen = true;
}

#if AVER_WITH_IMGUI

namespace {

// The override list is a diff of every member, so it is recomputed a few times a second and after anything this
// panel does, not on every frame.
struct RowCache {
    scene::Entity root = scene::kInvalidEntity;
    int age = 1000;
    std::vector<LevelInstanceOverrideRow> rows;
};

RowCache& cache() { static RowCache c; return c; }

void status(const LevelInstanceUiCallbacks& cb, const std::string& s) { if (cb.status) cb.status(s); }

} // namespace

bool levelInstanceDetailsDraw(LevelInstanceEditorModel& m, scene::Entity e, const LevelInstanceUiCallbacks& cb) {
    const scene::Entity root = m.topRootOf(e);
    if (root == scene::kInvalidEntity) return false;
    if (!::aver::editor::sectionHeader("Level Instance", ImGuiTreeNodeFlags_DefaultOpen)) return false;
    ImGui::PushID(static_cast<int>(root));

    bool changed = false;
    RowCache& c = cache();
    const bool editing = m.editing(root);
    if (c.root != root || c.age >= 15) {
        c.root = root;
        c.age = 0;
        c.rows = editing ? std::vector<LevelInstanceOverrideRow>{} : m.rows(root);
    }
    ++c.age;

    const std::string asset = m.assetOf(root);
    ImGui::TextDisabled("Instance of");
    ImGui::SameLine();
    ImGui::TextUnformatted(asset.empty() ? "(unknown level)" : asset.c_str());
    if (editing) ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "[editing] Members are ordinary entities; Commit writes the level.");
    else         ImGui::TextDisabled("%s", e == root ? "This is the instance root. Alt+click picks a member (override mode)."
                                                     : "Part of a level instance: changes here are overrides.");
    if (m.isNested(root)) ImGui::TextDisabled("Nested inside another instance.");

    const auto finish = [&](bool ok, LevelInstanceEdit& edit, const std::string& why) {
        if (ok) {
            if (cb.pushUndo) cb.pushUndo(edit);
            if (cb.markDirty) cb.markDirty();
            c.age = 1000;
            changed = true;
        } else if (!why.empty()) {
            status(cb, why);
        }
        return ok;
    };

    if (e != root && ImGui::Button("Select Root") && cb.select) cb.select(root);
    if (e != root) ImGui::SameLine();
    if (ImGui::Button("Open Level") && cb.openLevel) cb.openLevel(asset);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open this level on its own. Back (in the level bar) returns here.");
    ImGui::SameLine();
    if (ImGui::Button("Reload")) { if (m.reload(root)) { c.age = 1000; changed = true; } }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rebuild the instance from its level file (after an outside edit).");

    if (!editing) {
        if (ImGui::Button("Edit Instance")) {
            std::string why;
            if (m.beginEdit(root, &why)) { c.age = 1000; changed = true; }
            else status(cb, why);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Change the level itself, in place. Every other instance of it updates when you Commit.");
    } else {
        if (ImGui::Button("Commit")) {
            std::string why;
            if (m.commit(root, &why)) { if (cb.markDirty) cb.markDirty(); c.age = 1000; changed = true; }
            else status(cb, "Commit failed: " + why);
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard")) { if (m.discard(root)) { c.age = 1000; changed = true; } }
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(editing || m.isNested(root));
    if (ImGui::Button("Break")) {
        LevelInstanceEdit ed;
        if (finish(m.breakInstance(root, ed), ed, "That instance cannot be broken here.")) { ImGui::EndDisabled(); ImGui::PopID(); return true; }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Turn the instance into ordinary placements in this level. The level file is untouched.");

    ImGui::Separator();
    const fmt::OcLevelInstance* rec = m.recordOf(root);
    if (rec && !m.isNested(root)) {
        bool packed = rec->packed;
        if (ImGui::Checkbox("Packed", &packed)) {
            LevelInstanceEdit ed;
            finish(m.setPacked(root, packed, ed), ed, std::string());
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%s", m.packState(root).c_str());
        ImGui::TextDisabled("Static meshes become ray-traced instance rows: no entities, no raster, no voxel GI.");
        if (ImGui::Button("Bake Pack") && cb.bakePack) {
            std::string why;
            if (cb.bakePack(root, &why)) { status(cb, "Baked the pack of " + asset); c.age = 1000; changed = true; }
            else status(cb, "Bake failed: " + why);
        }

        int stream = rec->stream < 0 ? 0 : (rec->stream == 0 ? 1 : 2);
        const char* streamNames[] = {"Follow the level", "Always loaded", "Streamed"};
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Streaming", &stream, streamNames, 3)) {
            LevelInstanceEdit ed;
            finish(m.setStream(root, stream == 0 ? -1 : (stream == 1 ? 0 : 1), ed), ed, std::string());
        }
        const u32 placements = m.placementCount(root);
        if (placements > LevelInstanceEditorModel::kBigUnitPlacements)
            ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
                               "%u placements: a unit this size loads in one go. Split it into smaller levels to stream smoothly.", placements);
        else
            ImGui::TextDisabled("%u placement(s) in the level", placements);
    }

    if (!editing) {
        ImGui::Separator();
        if (c.rows.empty()) {
            ImGui::TextDisabled("No overrides: this instance matches its level.");
        } else {
            ImGui::Text("Overrides: %d", static_cast<int>(c.rows.size()));
            ImGui::SameLine();
            if (ImGui::SmallButton("Revert All")) {
                LevelInstanceEdit ed;
                finish(m.revertAll(root, ed), ed, std::string());
            }
            int i = 0;
            for (const LevelInstanceOverrideRow& row : c.rows) {
                if (changed) break;   // the rows are stale after an edit; the next frame rebuilds them
                ImGui::PushID(i++);
                ImGui::TextUnformatted(row.label.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Revert")) { LevelInstanceEdit ed; finish(m.revertOverride(root, row.ov, ed), ed, std::string()); }
                ImGui::PopID();
            }
        }
    }
    ImGui::PopID();
    return changed;
}

bool levelInstanceCreateDialogDraw(LevelInstanceCreateDialog& d, LevelInstanceEditorModel& m,
                                   const LevelInstanceEditorModel::CaptureFn& capture, const LevelInstanceUiCallbacks& cb) {
    if (d.wantOpen) { ImGui::OpenPopup("Create Level Instance"); d.wantOpen = false; }
    bool created = false;
    if (!ImGui::BeginPopupModal("Create Level Instance", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return false;

    ImGui::TextUnformatted("Saves the selected entities (and what is under them) as a level, re-origined to the first one.");
    ImGui::InputText("Name", d.name, sizeof d.name);
    ImGui::TextDisabled("Folder: Content/%s", d.dir.c_str());
    ImGui::Checkbox("Replace the selection with an instance", &d.replace);
    if (!d.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", d.error.c_str());

    ImGui::BeginDisabled(d.name[0] == '\0');
    if (ImGui::Button("Create")) {
        std::string file = d.name;
        const std::string ext = ".ocworld";
        if (file.size() < ext.size() || file.compare(file.size() - ext.size(), ext.size(), ext) != 0) file += ext;
        const std::string ref = d.dir.empty() ? file : d.dir + "/" + file;
        const LevelInstanceEditorModel::CreateResult r = m.createFromSelection(d.sources, ref, capture);
        d.error = r.error;
        if (r.ok) {
            if (d.replace && cb.removeReplaced) cb.removeReplaced(d.sources);
            if (cb.pushUndo) cb.pushUndo(r.edit);
            if (r.instance != scene::kInvalidEntity && cb.select) cb.select(r.instance);
            if (cb.markDirty) cb.markDirty();
            status(cb, "Created " + r.assetRef);
            created = true;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return created;
}

#else // !AVER_WITH_IMGUI

bool levelInstanceDetailsDraw(LevelInstanceEditorModel&, scene::Entity, const LevelInstanceUiCallbacks&) { return false; }
bool levelInstanceCreateDialogDraw(LevelInstanceCreateDialog&, LevelInstanceEditorModel&,
                                   const LevelInstanceEditorModel::CaptureFn&, const LevelInstanceUiCallbacks&) { return false; }

#endif

} // namespace aver::editor
#endif // AVER_MODULE_SCENE
