# Aver.Prefab  (`modules/prefab`)

- **Language:** C++
- **Depends on:** Formats, Scene, Core (`Aver.World`'s `LevelTransform.hpp` is included by path, not linked)
- **Built:** STATIC (`Aver.Prefab`), plus a stateless SHARED `Aver.Prefab.Abi` the .NET layer P/Invokes

Prefab instances in a live `scene::World`: spawn, per-instance overrides, revert, apply to the prefab, and
automatic propagation of a prefab edit to every instance with each one's overrides kept. Nested prefabs and
undo state are included. The design, the file formats, the editor and graph-node wiring and what is not done
are in [docs/PREFABS.md](../../docs/PREFABS.md).

| Header | What it owns |
|---|---|
| `PrefabLibrary.hpp` | The loaded assets, and `resolve`: nested prefabs and overrides flattened to a node list |
| `PrefabSystem.hpp` | Instances in a world: `instantiate`, `computeOverrides`, `setOverrides`, revert, apply, `updatePrefab`, `captureAsPrefab`, undo state, level records |
| `prefab_abi.h` | The plain-C seam; the host installs function pointers |
| `PrefabAbiHost.hpp` | `makeAbiHost(PrefabSystem&)` |

A third target beside the world and the file format, for the reason `Aver.Save` is: `Aver.Formats` sits below
`Aver.Scene` and may not reach it, and `Aver.Scene` may not gain a Formats edge. It links neither the
renderer, physics nor the editor; the editor's per-entity bookkeeping follows through `Hooks`.

The one idea: **overrides are never stored on entities.** They are the difference between live fields and
the prefab, recomputed on demand, so an edit through any route is an override and propagation is "diff against
the old asset, swap, rebuild in place with the same overrides."

Tests: `tests/prefab` (`OcPrefabTest`, `PrefabSystemTest`, `PrefabEditorModelTest`).
