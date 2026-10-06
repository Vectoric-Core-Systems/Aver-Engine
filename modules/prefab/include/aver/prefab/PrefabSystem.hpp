#pragma once
// Prefab instances in a live scene::World: spawn, override, revert, apply, and propagate a prefab
// edit to every instance. docs/PREFABS.md is the design; this header is the surface.
//
// THE ONE IDEA: an instance is its prefab plus a list of overrides, and the overrides are NEVER
// stored on the entities. They are the difference between the live fields and what the prefab says
// (computeOverrides), recomputed when something needs them. So an edit made through any route --
// the Details panel, a gizmo, a script, an undo -- is an override with no route having to know about
// prefabs, and "propagate" is: take every affected instance's overrides against the OLD prefab, swap
// the prefab, rebuild each instance from the NEW prefab with those same overrides on top.
//
// Rebuilding is IN PLACE (realize): entities are matched by node path, so a handle, a selection or a
// physics body survives; new nodes get new entities, removed nodes lose theirs, and anything a user
// parented under an instance that the prefab does not know about is kept.
//
// WHAT IS NOT AN OVERRIDE (v1, documented in docs/PREFABS.md): reparenting or deleting a linked entity
// inside an instance, and adding entities to one. They are not recorded, so the next sync restores the
// prefab's structure (extra unlinked children are kept, re-parented to the instance root if their
// parent goes away).
#include "aver/core/Math.hpp"
#include "aver/formats/OcPrefab.hpp"
#include "aver/prefab/PrefabLibrary.hpp"
#include "aver/scene/PrefabLink.hpp"
#include "aver/scene/World.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace aver::prefab {

// ---- undo state -----------------------------------------------------------------------------------
// A PrefabState is a value: capture it before and after an edit and restoreState() puts the world
// (instances, prefab data, plain subtrees) into either one. The editor stores the pair in one undo
// entry; nothing here knows about the editor's undo stack.

struct LibrarySnapshot {
    std::string path;
    bool existed = true;              // false: this state has no such asset in memory
    fmt::OcPrefabData data;
};

struct InstanceSnapshot {
    u32 instanceId = 0;               // the stable id, not a handle
    bool present = true;              // false: this state has no such instance
    std::string prefab;
    std::string name;                 // the root's name
    Transform local;                  // the root's local transform, used only when it must be recreated
    scene::Entity parent = scene::kInvalidEntity;
    std::vector<fmt::OcPrefabOverride> overrides;
};

// A plain (unlinked) subtree, for undoing a conversion into an instance. Recreated from `data`.
struct PlainSnapshot {
    u32 groupId = 0;
    bool present = true;
    fmt::OcPrefabData data;
    Transform local;
    scene::Entity parent = scene::kInvalidEntity;
};

struct PrefabState {
    std::vector<LibrarySnapshot> libs;
    std::vector<InstanceSnapshot> instances;
    std::vector<PlainSnapshot> plains;
};

// Callbacks the host (the editor, the runtime) installs so its own per-entity bookkeeping follows.
struct Hooks {
    std::function<void(scene::Entity)> created;      // a new entity, links and components in place
    std::function<void(scene::Entity)> changed;      // an existing entity's fields or components changed
    std::function<void(scene::Entity)> destroying;   // about to be destroyed; still valid
    // A node names a framework class. The host spawns / binds it; unset, the node is a plain entity.
    std::function<void(scene::Entity, const std::string& className)> classBound;
};

class PrefabSystem {
public:
    PrefabSystem(scene::World& world, PrefabLibrary& library);

    void setHooks(Hooks h) { hooks_ = std::move(h); }
    scene::World& world() { return world_; }
    PrefabLibrary& library() { return lib_; }
    // The registered CPrefabLink type id, 0 if registration failed.
    u32 linkComponent() const { return linkType_; }

    // ---- queries ------------------------------------------------------------------------------
    bool isLinked(scene::Entity e) const;
    // The instance root `e` belongs to, kInvalidEntity when it is not part of an instance.
    scene::Entity instanceRoot(scene::Entity e) const;
    bool isInstanceRoot(scene::Entity e) const;
    u32 instanceIdOf(scene::Entity e) const;
    scene::Entity rootOfInstance(u32 instanceId) const;
    // The prefab asset an instance root was spawned from ("" when unknown).
    std::string prefabPathOf(scene::Entity e) const;
    // The node path of a linked entity inside its instance ("" is the root); false when unlinked.
    bool nodePathOf(scene::Entity e, std::string& path);
    std::vector<scene::Entity> instanceRoots() const;
    // Instances of `prefabPath`; with includeNested also those that contain it as a nested prefab.
    std::vector<scene::Entity> instancesOf(const std::string& prefabPath, bool includeNested = true);
    // Every entity linked to `root`'s instance, parents first.
    std::vector<scene::Entity> instanceEntities(scene::Entity root) const;
    // The live entity of a node in an instance, kInvalidEntity when absent.
    scene::Entity entityAtPath(scene::Entity root, const std::string& path) const;

    // ---- lifecycle ------------------------------------------------------------------------------
    // Spawns an instance. `overrides` are applied on top (a level's saved list). The prefab root's
    // own transform is never used; `local` is the instance's. kInvalidEntity on failure.
    scene::Entity instantiate(const std::string& prefabPath, const Transform& local,
                              scene::Entity parent = scene::kInvalidEntity,
                              const std::vector<fmt::OcPrefabOverride>& overrides = {},
                              const std::string& name = {}, std::string* why = nullptr);
    // Destroys an instance and everything under it (deferred to the world's flush).
    bool destroyInstance(scene::Entity root);
    // "Unpack": drops the links, leaving ordinary entities. The prefab and other instances are untouched.
    bool unlink(scene::Entity root);

    // ---- overrides ------------------------------------------------------------------------------
    // The instance's overrides: live fields against the prefab, in deterministic order. The root's
    // own transform and name are the instance's and never appear.
    bool computeOverrides(scene::Entity root, std::vector<fmt::OcPrefabOverride>& out, std::string* why = nullptr);
    // Makes the instance exactly the prefab plus `overrides`; fields not named go back to the prefab.
    bool setOverrides(scene::Entity root, const std::vector<fmt::OcPrefabOverride>& overrides, std::string* why = nullptr);
    // Reverts everything / one override / every override on one node.
    bool revertAll(scene::Entity root);
    bool revertOverride(scene::Entity root, const fmt::OcPrefabOverride& which);
    bool revertNode(scene::Entity anyEntityOfTheNode);

    // ---- editing the prefab ---------------------------------------------------------------------------
    // Folds one override / every override of the instance into its prefab asset, then propagates.
    // The asset is updated in the library; library().save() writes it. Overrides that lie inside a
    // nested prefab are folded into the nested NODE's override list, not into the nested asset.
    bool applyOverride(scene::Entity root, const fmt::OcPrefabOverride& which, std::string* why = nullptr);
    bool applyAll(scene::Entity root, std::string* why = nullptr);
    // Replaces an asset and propagates: every instance that is, or contains, `prefabPath` keeps its
    // overrides and picks up the change. This is the automatic propagation.
    bool updatePrefab(const std::string& prefabPath, fmt::OcPrefabData data, std::string* why = nullptr);
    // Rebuilds one instance from the current assets, keeping its overrides (after an external reload).
    bool syncInstance(scene::Entity root, std::string* why = nullptr);

    // ---- authoring --------------------------------------------------------------------------------------
    // Captures `root` and its subtree as a prefab (root transform normalised to identity). A child
    // that is itself an instance root becomes a nested prefab node carrying its overrides.
    bool captureAsPrefab(scene::Entity root, const std::string& name, fmt::OcPrefabData& out,
                         std::string* why = nullptr);
    // Replaces the plain subtree at `root` with an instance of `prefabPath` (already in the library,
    // typically from captureAsPrefab) at the same local transform and parent. Returns the new root.
    scene::Entity convertToInstance(scene::Entity root, const std::string& prefabPath, std::string* why = nullptr);

    // ---- undo ---------------------------------------------------------------------------------------------
    // Gives a plain subtree a group id so a PlainSnapshot can name it across delete + undo.
    u32 registerPlain(scene::Entity root);
    PrefabState captureState(const std::vector<std::string>& prefabPaths,
                             const std::vector<u32>& instanceIds,
                             const std::vector<u32>& plainGroups);
    bool restoreState(const PrefabState& state, std::string* why = nullptr);

    // ---- levels -----------------------------------------------------------------------------------------------
    // One record per instance root `include` accepts (all, when unset). Transform is the root's local one.
    std::vector<fmt::OcPrefabInstance> captureLevelInstances(const std::function<bool(scene::Entity)>& include = {});
    // Spawns every record; a record whose asset is missing is skipped with a warning. Returns the roots.
    std::vector<scene::Entity> instantiateLevelInstances(const std::vector<fmt::OcPrefabInstance>& records);

private:
    struct Live;
    Live liveOf(scene::Entity root) const;
    bool diffAgainst(scene::Entity root, const Resolved& base, std::vector<fmt::OcPrefabOverride>& out);
    scene::Entity realize(scene::Entity existingRoot, const Resolved& base,
                          const std::vector<fmt::OcPrefabOverride>& overrides, const Transform* rootLocal,
                          scene::Entity parent, const std::string& rootName, u32 instanceId);
    bool resolveInstance(scene::Entity root, Resolved& out, std::string* why);
    void destroyTree(scene::Entity e);
    scene::Entity instantiateWithId(const std::string& prefabPath, const Transform& local, scene::Entity parent,
                                    const std::vector<fmt::OcPrefabOverride>& overrides,
                                    const std::string& name, u32 instanceId, std::string* why);

    scene::World& world_;
    PrefabLibrary& lib_;
    Hooks hooks_;
    u32 linkType_ = 0;
    u32 nextInstanceId_ = 1;
    u32 nextGroupId_ = 1;
    std::unordered_map<u32, scene::Entity> plainRoots_;
    u32 plainTemp_ = 0;
};

} // namespace aver::prefab
