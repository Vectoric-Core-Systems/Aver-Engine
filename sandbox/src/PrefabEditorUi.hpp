#pragma once
// The ImGui half of the prefab editor: the "Prefab" section of the Details panel (what this is an
// instance of, its overrides, Apply / Revert / Unpack) and the "Create Prefab" dialog the content
// browser and the outliner open. All behaviour is in PrefabEditorModel; these only draw and forward.
//
// THE HOOKS (docs/PREFABS.md "Editor wiring"):
//   Details panel, for the selected entity:      editor::prefabDetailsDraw(prefabModel_, sel, cb);
//   Content browser "Add" menu and outliner menu: if (ImGui::MenuItem("Create Prefab from Selection..."))
//                                                     editor::prefabCreateDialogOpen(dlg_, sel, name, dir);
//   Once per frame, beside the other modals:      editor::prefabCreateDialogDraw(dlg_, prefabModel_, cb);
// Everything is a no-op in a build without ImGui.
#if AVER_MODULE_SCENE
#include "PrefabEditorModel.hpp"

#include <functional>
#include <string>

namespace aver::editor {

// What the host does with the results. All optional.
struct PrefabUiCallbacks {
    std::function<void(const PrefabEdit&)> pushUndo;            // one undo entry per successful operation
    std::function<void(scene::Entity)> select;
    std::function<void(const std::string& assetRef)> openAsset; // open the prefab asset's own editor tab
    std::function<void()> markDirty;                            // the level has unsaved edits
    std::function<void(const std::string&)> status;             // a line for the status bar
};

// Draws the Prefab section for `e`. Does nothing for an entity that is not part of an instance.
// True when it changed the scene (the caller refreshes whatever it caches).
bool prefabDetailsDraw(PrefabEditorModel& m, scene::Entity e, const PrefabUiCallbacks& cb);

struct PrefabCreateDialog {
    bool wantOpen = false;
    scene::Entity source = scene::kInvalidEntity;
    char name[128] = {};
    std::string dir;           // content-relative folder; "" is Content itself
    bool replace = true;       // swap the selection for an instance of the new prefab
    std::string error;
};

void prefabCreateDialogOpen(PrefabCreateDialog& d, scene::Entity source, const std::string& suggestedName,
                            const std::string& contentRelativeDir);
// Call every frame. True on the frame a prefab was created.
bool prefabCreateDialogDraw(PrefabCreateDialog& d, PrefabEditorModel& m, const PrefabUiCallbacks& cb);

} // namespace aver::editor
#endif // AVER_MODULE_SCENE
