#include "PrefabEditorUi.hpp"

#if AVER_MODULE_SCENE
#include <cstdio>
#include <cstring>

#if AVER_WITH_IMGUI
#  include "imgui.h"
#endif

namespace aver::editor {

void prefabCreateDialogOpen(PrefabCreateDialog& d, scene::Entity source, const std::string& suggestedName,
                            const std::string& contentRelativeDir) {
    d.source = source;
    std::snprintf(d.name, sizeof d.name, "%s", suggestedName.c_str());
    d.dir = contentRelativeDir;
    d.error.clear();
    d.wantOpen = true;
}

#if AVER_WITH_IMGUI

namespace {

// The override list is a diff of every field of every node, so it is recomputed a few times a second
// and after anything this panel does, not on every frame.
struct RowCache {
    scene::Entity entity = scene::kInvalidEntity;
    int age = 1000;
    int total = 0;
    std::vector<PrefabOverrideRow> rows;
};

RowCache& cache() { static RowCache c; return c; }

void status(const PrefabUiCallbacks& cb, const std::string& s) { if (cb.status) cb.status(s); }

} // namespace

bool prefabDetailsDraw(PrefabEditorModel& m, scene::Entity e, const PrefabUiCallbacks& cb) {
    if (!m.isInstance(e)) return false;
    if (!ImGui::CollapsingHeader("Prefab", ImGuiTreeNodeFlags_DefaultOpen)) return false;

    bool changed = false;
    RowCache& c = cache();
    if (c.entity != e || c.age >= 15) {
        c.entity = e;
        c.age = 0;
        c.rows = m.rows(e, false);
        c.total = m.overrideCount(e);
    }
    ++c.age;

    const scene::Entity root = m.rootOf(e);
    const std::string asset = m.assetOf(e);
    ImGui::TextDisabled("Instance of");
    ImGui::SameLine();
    ImGui::TextUnformatted(asset.empty() ? "(unknown asset)" : asset.c_str());
    ImGui::TextDisabled("%s", e == root ? "This is the instance root." : "Part of a prefab instance.");

    const auto finish = [&](bool ok, PrefabEdit& edit, const std::string& why) {
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

    if (e != root) {
        if (ImGui::Button("Select Root") && cb.select) cb.select(root);
        ImGui::SameLine();
    }
    if (ImGui::Button("Open Prefab") && cb.openAsset) cb.openAsset(asset);

    ImGui::BeginDisabled(c.total == 0);
    if (ImGui::Button("Apply All")) { PrefabEdit ed; std::string why; finish(m.applyAll(e, ed, &why), ed, why); }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Write every override of this instance into the prefab asset.\nEvery other instance picks the change up and keeps its own overrides.");
    ImGui::SameLine();
    if (ImGui::Button("Revert All")) { PrefabEdit ed; finish(m.revertAll(e, ed), ed, std::string()); }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Put every override of this instance back to what the prefab says.");
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Unpack")) {
        PrefabEdit ed; std::string why;
        if (finish(m.unpack(e, ed, &why), ed, why)) return true;   // `e` was replaced; draw nothing more
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Turn this instance into ordinary entities. The prefab asset is untouched.");

    if (c.total == 0) {
        ImGui::TextDisabled("No overrides: this instance matches the prefab.");
        return changed;
    }
    ImGui::Separator();
    ImGui::Text("Overrides on this node: %d   (in the whole instance: %d)", static_cast<int>(c.rows.size()), c.total);

    int i = 0;
    for (const PrefabOverrideRow& row : c.rows) {
        ImGui::PushID(i++);
        ImGui::TextUnformatted(row.label.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply")) { PrefabEdit ed; std::string why; finish(m.applyOverride(e, row.ov, ed, &why), ed, why); }
        ImGui::SameLine();
        if (ImGui::SmallButton("Revert")) { PrefabEdit ed; finish(m.revertOverride(e, row.ov, ed), ed, std::string()); }
        ImGui::PopID();
        if (changed) break;   // the rows are stale after an edit; the next frame rebuilds them
    }
    return changed;
}

bool prefabCreateDialogDraw(PrefabCreateDialog& d, PrefabEditorModel& m, const PrefabUiCallbacks& cb) {
    if (d.wantOpen) { ImGui::OpenPopup("Create Prefab"); d.wantOpen = false; }
    bool created = false;
    if (!ImGui::BeginPopupModal("Create Prefab", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return false;

    ImGui::TextUnformatted("Saves the selected entity and everything under it as a prefab asset.");
    ImGui::InputText("Name", d.name, sizeof d.name);
    ImGui::TextDisabled("Folder: Content/%s", d.dir.c_str());
    ImGui::Checkbox("Replace the selection with an instance", &d.replace);
    if (!d.error.empty()) ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", d.error.c_str());

    ImGui::BeginDisabled(d.name[0] == '\0');
    if (ImGui::Button("Create")) {
        std::string file = d.name;
        const std::string ext = ".ocprefab";
        if (file.size() < ext.size() || file.compare(file.size() - ext.size(), ext.size(), ext) != 0) file += ext;
        const std::string ref = d.dir.empty() ? file : d.dir + "/" + file;
        const PrefabEditorModel::CreateResult r = m.createPrefab(d.source, ref, d.replace);
        d.error = r.error;
        if (r.ok) {
            if (r.hasEdit && cb.pushUndo) cb.pushUndo(r.edit);
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

bool prefabDetailsDraw(PrefabEditorModel&, scene::Entity, const PrefabUiCallbacks&) { return false; }
bool prefabCreateDialogDraw(PrefabCreateDialog&, PrefabEditorModel&, const PrefabUiCallbacks&) { return false; }

#endif

} // namespace aver::editor
#endif // AVER_MODULE_SCENE
