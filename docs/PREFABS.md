# Prefabs

A prefab is a saved entity hierarchy. A level holds **instances** of it, each a link plus a list of
per-instance **overrides**. Editing the prefab reaches every instance automatically and leaves each
instance's overrides alone. Nested prefabs, apply-to-prefab, revert and undo are supported.

| Layer | File |
|---|---|
| Asset and level records (text) | `modules/formats/include/aver/formats/OcPrefab.hpp`, `src/OcPrefab.cpp` |
| `.ocworld` hook (additive) | `OcWorldData::prefabInstances`; three small hunks in `OcWorld.cpp` |
| Instance link component | `modules/scene/include/aver/scene/PrefabLink.hpp` (header-only, registered at runtime) |
| Library, flatten, instances, propagation, undo state | `modules/prefab` (`Aver.Prefab`) |
| C ABI | `modules/prefab/include/aver/prefab/prefab_abi.h`, `abi/PrefabAbi.cpp` (`Aver.Prefab.Abi`) |
| C# and the graph seam | `scripting/csharp/Aver.Prefab` (`Prefabs`, `GraphPrefabs`) |
| Editor model (no UI) and UI | `sandbox/src/PrefabEditorModel.*`, `PrefabEditorUi.*` |
| Tests | `tests/prefab/` (`OcPrefabTest`, `PrefabSystemTest`, `PrefabEditorModelTest`) |

## The model

**A node is addressed by a path, not an index.** Every node of a prefab has a `uid`, unique inside it
and never reused (`OcPrefabData::nextUid`). The prefab's root is the empty path `""`; any other node is
its uid (`"5"`); a node inside a nested prefab is `"<nested node uid>/<uid inside it>"` (`"3/5"`). A live
entity's link and an override both name a node this way, so they survive the prefab being edited,
reordered or having nodes added. A nested prefab's root *is* the node that nests it, so its path is just
that node's (`"3"`), and its children are `"3/..."`.

**An override is a delta.** Three kinds, each on one node:

| Override | Meaning |
|---|---|
| `Set path component field value` | this field has this value |
| `AddComponent path component` | the node has a component the prefab does not |
| `RemoveComponent path component` | the node lacks a component the prefab has |

A node's own name is the pseudo-component `@Node`, field `name`. An entity-reference field is stored as
the target's node path, never a handle, so it stays right when the structure changes.

**An instance is the prefab plus overrides, and the overrides are not stored on the entities.** They
are the difference between the live fields and what the prefab says (`PrefabSystem::computeOverrides`),
recomputed when needed. So an edit made through *any* route (Details, a gizmo, a script, an undo) is an
override without that route knowing about prefabs. The entity carries only `CPrefabLink`
(`prefabId`, `pathHash`, `root`, `instanceId`).

**A nested prefab is a node with `prefab` set.** It has no components of its own; everything that
differs from the nested asset (its position included) is an override on that node, with paths relative
to the nested root. Nesting is flattened when an instance is built (`PrefabLibrary::resolve`): depth is
capped at 8, a prefab that contains itself is refused, and a nested asset that cannot be loaded becomes a
placeholder node plus a warning rather than taking every instance of the outer prefab down.

**The instance root's own transform and name belong to the instance**, not to the prefab: the prefab
root's transform is never used, and a captured prefab's root is normalised to the origin.

## Propagation

`PrefabSystem::updatePrefab(path, data)` is the one entry point, and it is what apply, an asset edit,
an external reload and undo all go through:

1. find every instance that is, or contains, `path`;
2. for each, compute its overrides **against the old asset**;
3. swap the asset;
4. rebuild each instance from the **new** asset with those same overrides on top.

Rebuilding is in place (`realize`). Entities are matched by node path, so a handle, a selection or a
physics body survives; a new node gets a new entity; a removed node loses its entity; anything a user
parented under an instance that the prefab does not know about is kept (moved to the instance root if its
parent goes away). Components the prefab no longer has are removed; fields the prefab changed arrive;
fields an instance overrode do not. Writes happen only where the value differs, and the host's
`Hooks::changed` fires only for entities that actually changed.

## Apply, revert, unpack

* **Revert** an override, a node, or the whole instance: rebuild with the matching overrides dropped.
* **Apply** folds overrides into the instance's own prefab asset, then propagates. Overrides inside a
  nested prefab are folded into the nested **node's** override list in the outer asset, not into the nested
  asset itself. An entity reference that leaves the nested prefab cannot be folded and is skipped with a
  warning.
* **Unpack** removes the links; the entities stay as ordinary ones.
* **Create prefab**: `captureAsPrefab` saves a subtree; a child that is itself an instance root becomes a
  nested node carrying its overrides. `convertToInstance` swaps the selection for an instance.

## Undo

`PrefabState` is a value: prefab data, instance snapshots (by a stable `instanceId`, not a handle,
because delete + undo recreates entities) and plain-subtree snapshots. `captureState` before and after an
operation and `restoreState` puts the world into either. Instances the state does not name but whose
prefab it changes keep their overrides (read against the assets as they are before anything moves).
The editor stores a `PrefabEdit {before, after}` as one undo entry.

## Files

`.ocprefab` is text; grammar and an example are in the `OcPrefab.hpp` header. Components and fields are
written **by name** with their scene kind, so a field added to a component later loads from an older
prefab (it reads as zero) and a component registered at runtime saves without the format knowing it.
`CMeshRenderer.material` is the one special case: the scene holds a per-process intern token, so a prefab
stores the material **name** and interns it on load. Floats are written with `%.9g` and round-trip bit
for bit.

In a level, an instance is `PREFABINST asset pos x y z rot yaw pitch roll scale sx sy sz [name n]` with
`POVERRIDE` lines and `ENDPREFABINST`, written before the placements. An older build skips the unknown
records (it loses the prefab instances, never misplaces anything), and a level with no prefabs writes no
new bytes. Instances are **not** also written as placements.

## C ABI, C# and graph nodes

`prefab_abi.h` (`Aver.Prefab.Abi`, stateless): `aver_prefab_spawn / destroy / root_of / is_instance /
find / revert / override_count`. The host installs function pointers with `aver_prefab_set_host`
(`makeAbiHost(PrefabSystem&)` builds the table). C#: `Aver.Prefab.Prefabs`.

Graph nodes (`GraphPrefabs` in `Aver.Prefab` is the seam; the asset path and node path are node
**attributes** because a pin is float, int or bool):

| Node | Pins | Attributes | Does |
|---|---|---|---|
| `AN_SpawnPrefab` | in `exec`, `parent` int, `x` `y` `z` float, `yaw` float, `scale` float; out `then` exec, `entity` int | `prefab=Prefabs/Crate.ocprefab` | `GraphPrefabs.SpawnPrefabForGraph`; `entity` is the instance root, 0 on failure |
| `AN_DestroyPrefab` | in `exec`, `root` int; out `then` exec | none | `DestroyPrefabForGraph` |
| `AN_GetPrefabRoot` | in `exec`, `entity` int; out `then` exec, `root` int | none | `PrefabRootForGraph`; 0 if not part of an instance |
| `AN_FindPrefabNode` | in `exec`, `root` int; out `then` exec, `entity` int | `node=3/5` (empty = root) | `FindPrefabNodeForGraph` |
| `AN_RevertPrefab` | in `exec`, `root` int; out `then` exec | none | `RevertPrefabForGraph` |

## What is not done (v1)

* Reparenting or deleting a **linked** entity inside an instance, and adding entities to one, are not
  overrides: the next sync restores the prefab's structure (extra *unlinked* children are kept).
* Apply folds into the instance's own (outermost) asset only; applying into a nested asset directly is not offered.
* A node's `className` is stored but a class is bound only if the host installs `Hooks::classBound`; the
  editor wiring below does not.
* Capturing a subtree under a nested instance keeps only the overrides, not extra children a user added under
  the instance's own entities.
* Undoing a conversion or an unpack recreates ordinary entities from a flat capture, so an instance nested
  inside the converted subtree comes back flattened.
* No prefab asset editor tab (the Details section opens the asset's text only through `openAsset`); edit
  through the instance and Apply, or edit the file and call `updateAsset`.
* Packaging: `.ocprefab` is not yet in the cook list, and the runtime loader must be pointed at the packed
  content. Neither is wired here.

## Wiring the integration step must add

None of these touch a file another feature agent owns by default; each is a one-place change.

**CMake**

1. Root `CMakeLists.txt`, inside `if(AVER_MODULE_SCENE)` after `add_subdirectory(modules/save)`:
   `add_subdirectory(modules/prefab)`. And in the tests block, after `add_subdirectory(tests/save)`:
   `add_subdirectory(tests/prefab)`.
2. `sandbox/CMakeLists.txt`: add `src/PrefabEditorModel.cpp` and `src/PrefabEditorUi.cpp` to `Sandbox`, and
   beside the `Aver.Save` link: `if(TARGET Aver.Prefab) target_link_libraries(Sandbox PRIVATE Aver.Prefab Aver.Prefab.Abi) endif()`.
   `Runtime/CMakeLists.txt` the same two libraries for the game host.
3. `modules/scripting/CMakeLists.txt`: add `${AVER_CSHARP_DIR}/Aver.Prefab/*.cs` and `*.csproj` to the bridge
   source glob and `${AVER_BRIDGE_DIR}/Aver.Prefab.dll` to `AVER_BRIDGE_OUTPUTS`; add
   `<ProjectReference Include="..\Aver.Prefab\Aver.Prefab.csproj" />` to `Aver.Graph.csproj` and the bridge csproj.

**Graph nodes** (shared files): add the five rows above to `graphNodeCatalog()` in
`sandbox/src/GraphNodeDefs.hpp`, the default pins in `OcGraphParser.AddDefaultPins`, and the emit in
`GraphCompiler.cs` (a call to the matching `GraphPrefabs.*ForGraph`, reading `prefab=` / `node=` as the
timer nodes read `event=`); add the table above to `docs/AVER_NODE_NODES.md`.

**Docs**: a row for `Aver.Prefab` and `Aver.Prefab.Abi` in `docs/ARCHITECTURE.md` (module DAG: above
`Aver.Scene` and `Aver.Formats`), and replace the sketch in `docs/formats/FORMAT_SPECS.md` section 10 with a
pointer to this page (the text grammar here supersedes it).

**Editor** (`SandboxApp.hpp`, `SandboxLevelLoad.cpp`, `SandboxLevelEdit.cpp`, panels):

* Members: `prefab::PrefabLibrary prefabLib_; prefab::PrefabSystem prefabSys_{scene::World::instance(), prefabLib_};
  editor::PrefabEditorModel prefabModel_{scene::World::instance(), prefabLib_, prefabSys_}; editor::PrefabCreateDialog prefabDlg_;`
* After a project opens: `prefabModel_.setContentDir(project_.contentDir());`
* Hooks, once: `prefabSys_.setHooks({created, changed, destroying})` where
  `created` does `levelEntities_.push_back(e); entityLabels_[u32(e)] = world.name(e);` and, for an entity with a
  `CMeshRenderer`, `rebuildEntityBody(e)`; `changed` rebuilds the body for a mesh entity; `destroying`
  removes `e` from `levelEntities_` and `entityLabels_` and drops its body.
* `saveLevel`: in both loops over `levelEntities_` (the `slotOf` pass and the placement pass) add
  `if (prefabSys_.isLinked(e)) continue;`; before `saveOcworld`:
  `w.prefabInstances = prefabSys_.captureLevelInstances(); prefabModel_.saveDirtyAssets();`
* `loadLevel` after the placements are instantiated (`onLevelInstantiated`):
  `prefabSys_.instantiateLevelInstances(w.prefabInstances);`; in `unloadLevel` no extra step (the entities
  are destroyed with the rest; the instance ids are recomputed from the world).
* Undo: add `Kind::Prefab` and a `std::shared_ptr<editor::PrefabEdit> prefab;` to `SandboxApp::EditCmd`;
  `pushEdit` of one is the `pushUndo` callback; the undo switch calls `prefabModel_.undo(*c.prefab)` and the
  redo switch `prefabModel_.redo(*c.prefab)`, then re-selects nothing (handles may have changed).
* Details panel: `editor::prefabDetailsDraw(prefabModel_, selEntity_, cb)` near the top of the entity section.
* Content browser: `.ocprefab` in the file-type icon table; "Create Prefab from Selection..." in the Add menu and
  the outliner menu opens `prefabCreateDialogOpen(prefabDlg_, selEntity_, name, relDir)` (`relDir` from
  `PrefabEditorModel::assetRefFor`); `prefabCreateDialogDraw(prefabDlg_, prefabModel_, cb)` once per frame; a
  drop of an `.ocprefab` on the viewport calls `prefabModel_.place(ref, xf, kInvalidEntity, root, edit)` and pushes `edit`.
* `AssetRefScan.hpp`: count `PREFABINST` and nested `prefab` nodes as references so Find References and delete see them.

**Runtime** (`Runtime/src/GameApp.cpp`, `GameLevel.cpp`): a `PrefabLibrary` whose loader reads `.ocprefab`
through the same content provider as other text assets; after the level's placements are instantiated,
`instantiateLevelInstances(levelData.prefabInstances)`; `aver_prefab_set_host(&makeAbiHost(sys))` once.

## Tests

`OcPrefabTest`: asset round trip (bit-exact floats, 64-bit ids, escapes), validation, unknown records, level
round trip, **a level with no prefabs writes no prefab bytes**. `PrefabSystemTest`: spawn, override
detection, revert (field / node / all), **an override survives a prefab edit**, node add and remove,
**nested propagation** (an inner edit reaches the outer instance, overrides two levels down kept), apply
(all and one, including into a nested node), added and removed components, missing nested asset, cycle,
undo state (apply, delete, conversion), level round trip, unlink. `PrefabEditorModelTest`: create from a
selection with undo and redo, apply and save to disk, place, unpack and undo.
