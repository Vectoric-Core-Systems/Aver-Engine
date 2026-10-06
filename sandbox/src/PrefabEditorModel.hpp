#pragma once
// The editor's side of prefabs, with no UI in it: creating a prefab from a selection, placing one,
// apply / revert / unpack on an instance, and the override list the Details panel shows. Every
// mutating call fills a PrefabEdit (the prefab state before and after) so the host can put ONE entry
// on its undo stack; undo() and redo() just restore that state.
//
// THE HOOK INTO THE EDITOR (docs/PREFABS.md "Editor wiring" has the full list):
//   * a PrefabLibrary + PrefabSystem + PrefabEditorModel as SandboxApp members;
//   * Hooks on the PrefabSystem that register / unregister / refresh an entity with the editor's own
//     bookkeeping (levelEntities_, labels, bodies);
//   * saveLevel skips linked entities and writes sys.captureLevelInstances() into
//     OcWorldData::prefabInstances; loadLevel calls sys.instantiateLevelInstances().
//
// Plain functions and value types, so a test can drive it (tests/prefab/PrefabEditorModelTest.cpp).
#if AVER_MODULE_SCENE
#include "aver/prefab/PrefabSystem.hpp"

#include <set>
#include <string>
#include <vector>

namespace aver::editor {

// One undoable prefab operation.
struct PrefabEdit {
    std::string label;
    prefab::PrefabState before;
    prefab::PrefabState after;
};

// One override as the Details panel lists it.
struct PrefabOverrideRow {
    fmt::OcPrefabOverride ov;
    std::string nodePath;     // where in the instance
    std::string nodeName;     // that node's entity name, for display
    std::string label;        // "CTags.bits = 7", "+ CTags", "Name = Top"
};

class PrefabEditorModel {
public:
    PrefabEditorModel(scene::World& world, prefab::PrefabLibrary& library, prefab::PrefabSystem& system);

    // The project's Content folder: prefab asset references are relative to it, and the library's
    // loader and saver are pointed at it.
    void setContentDir(const std::string& dir);
    const std::string& contentDir() const { return contentDir_; }
    // "<content>/Prefabs/Crate.ocprefab" -> "Prefabs/Crate.ocprefab" (forward slashes); "" when outside.
    static std::string assetRefFor(const std::string& contentDir, const std::string& fullPath);
    std::string fullPathFor(const std::string& assetRef) const;
    static bool isPrefabPath(const std::string& path);

    // ---- create / place -------------------------------------------------------------------------
    struct CreateResult {
        bool ok = false;
        std::string assetRef;
        scene::Entity instance = scene::kInvalidEntity;   // the instance that replaced the selection, if asked
        std::string error;
        bool hasEdit = false;
        PrefabEdit edit;                                   // only when it replaced the selection
    };
    // Captures `root`'s subtree into a new asset at `assetRef` (refuses to overwrite one), writes it,
    // and with `replaceWithInstance` swaps the selection for an instance of it.
    CreateResult createPrefab(scene::Entity root, const std::string& assetRef, bool replaceWithInstance);
    // Spawns an instance. `edit` is filled so the spawn can be undone.
    bool place(const std::string& assetRef, const Transform& xf, scene::Entity parent,
               scene::Entity& outRoot, PrefabEdit& edit, std::string* why = nullptr);

    // ---- instance operations ------------------------------------------------------------------------
    // `e` is any entity of the instance. False (edit untouched) when it is not part of one.
    bool applyAll(scene::Entity e, PrefabEdit& edit, std::string* why = nullptr);
    bool applyOverride(scene::Entity e, const fmt::OcPrefabOverride& ov, PrefabEdit& edit, std::string* why = nullptr);
    bool revertAll(scene::Entity e, PrefabEdit& edit);
    bool revertOverride(scene::Entity e, const fmt::OcPrefabOverride& ov, PrefabEdit& edit);
    bool revertNode(scene::Entity e, PrefabEdit& edit);
    // Turns the instance into ordinary entities (new ones; the instance and its link are gone).
    bool unpack(scene::Entity e, PrefabEdit& edit, std::string* why = nullptr);
    bool destroyInstance(scene::Entity e, PrefabEdit& edit);
    // A prefab asset was edited elsewhere (a prefab editor tab, a reload): propagate to every instance.
    bool updateAsset(const std::string& assetRef, fmt::OcPrefabData data, PrefabEdit& edit, std::string* why = nullptr);
    bool saveAsset(const std::string& assetRef, std::string* why = nullptr);
    // Assets edited (applied to, or updated) and not yet written. The host calls saveDirtyAssets when
    // the level is saved; undo does not clear the mark, so an undone edit is simply written back as it was.
    const std::set<std::string>& dirtyAssets() const { return dirty_; }
    bool saveDirtyAssets(std::string* why = nullptr);

    // ---- undo ---------------------------------------------------------------------------------------------
    bool undo(const PrefabEdit& e) { return sys_.restoreState(e.before); }
    bool redo(const PrefabEdit& e) { return sys_.restoreState(e.after); }

    // ---- Details ----------------------------------------------------------------------------------------------
    bool isInstance(scene::Entity e) const { return sys_.isLinked(e); }
    scene::Entity rootOf(scene::Entity e) const { return sys_.instanceRoot(e); }
    std::string assetOf(scene::Entity e) const { return sys_.prefabPathOf(e); }
    // The overrides of `e`'s node, or of the whole instance.
    std::vector<PrefabOverrideRow> rows(scene::Entity e, bool wholeInstance);
    int overrideCount(scene::Entity e);
    static std::string describe(const fmt::OcPrefabOverride& ov);

private:
    template <class F>
    bool runOnInstance(const char* label, scene::Entity e, bool touchesAsset, PrefabEdit& edit, F&& op);

    scene::World& world_;
    prefab::PrefabLibrary& lib_;
    prefab::PrefabSystem& sys_;
    std::string contentDir_;
    std::set<std::string> dirty_;
};

} // namespace aver::editor
#endif // AVER_MODULE_SCENE
