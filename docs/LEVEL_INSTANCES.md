# Level Instances and Packed Level Actors

A **level instance** places one `.ocworld` inside another: a transform, a list of overrides, and a live link to the child level. Edit the child once and every instance of it updates. Instances nest to a depth of eight, stream as units, and carry their lights, decals, prefabs, classes and static colliders with them. An instance can optionally be **packed**: its static meshes become ray-traced instance rows instead of entities.

The design contract is [plans/wave3/LEVEL_INSTANCES.md](plans/wave3/LEVEL_INSTANCES.md); this page is the guide to what shipped. The record is in [formats/FORMAT_SPECS.md](formats/FORMAT_SPECS.md) section 11 (`LEVELINST`, `EXTENT`) and 11f (`.ocpack`).

Naming trap: `world::LevelInstance` is the *result of `world::instantiate()`* and has nothing to do with this feature. The user-facing term is "Level Instance".

## The record

```
LEVELINST Levels/Room.ocworld pos 1200 0 0 rot 90 0 0 scale 1 1 1 name Room%20A id=00000000000000a1 packed stream 1
  LIOVERRIDE set 00000000000000c3 @Placement material string ~M_Red
  LIOVERRIDE set 00000000000000c4 @Placement removed bool 1
  LIOVERRIDE set 00000000000000b2/00000000000000c9 @Placement position vec3 0 0 40
ENDLEVELINST
```

It is written before the placements, additive: an older build skips the records, so the instance vanishes visibly and nothing is misplaced. There is no "instanceable" flag; any level can be instanced.

- **Overrides** are keyed by the child placement's stable `id=` (`<nested instance id>/<placement id>` through a nested instance). Fields: `position`, `rotation`, `scale` (replace, in the child's frame), `material`, `visible`, `collide`, `removed` (drops the placement and its children), `name`.
- **Ids.** Placing a level as an instance, or authoring an override, gives the child's placements and nested instances ids once (the child file is rewritten if any were missing; refused while that level is the open one with unsaved edits: "save it first"). The editor keeps ids across saves; once a level has any, every placement gets one.
- **`EXTENT`** (the level's own frame) lets another level bound an instance of it without loading meshes. The editor writes it on every save. A child with no `EXTENT` and no nested instances has unknown bounds, and its instance is always loaded.
- A child's `SUN SKY FOG CLOUDS GAMEMODE DEFAULTPAWN SPAWN CAMERA STREAM`, `LANDSCAPE WATER WAVE PCGVOLUME SCATTER POSTVOLUME SEQUENCE` records are the host level's own and are ignored (one log line per instance names what). `vehicle <preset>` is realised as a plain mesh and `snap` is ignored, each with one warning.

## What an instance contains

Placements (with their static bodies, object animation and kinematic bodies), class placements (spawned in the late class pass once scripting is up), lights, decals, prefab instances (through the prefab system), the child's `FOLIAGE` tables (through the instance matrix), and nested instances. Every entity carries a `CLevelInstanceLink` (asset id, placement id, root, instance id, depth, flags); the level save skips linked entities and writes the `LEVELINST` record.

## In the editor

- **Place.** Drag a `.ocworld` from the Content Browser onto the viewport. A level that would contain itself (directly, or through another level) is refused with a status line, as is one that does not parse.
- **Locked by default.** A click selects the instance root; **Alt+click** picks a member (override mode). Moves, material changes, hides and deletes on a member are overrides, computed as the difference from the child at save, undo and Details time. Creating or reparenting structure under a locked instance is refused.
- **Edit Instance** shows the raw child level (your overrides are set aside); members are ordinary entities. **Commit** writes the child `.ocworld`, rebuilds every other instance of it in place keeping their overrides, and ends the edit. **Discard** rebuilds from the asset. Only one instance is edited at a time. The outliner marks it `[editing]`; saving or leaving the level while an edit is open asks you to commit or discard first.
- **Open Level** opens the child on its own (the unsaved-changes prompt applies); **< Back to ...** in the viewport bar returns. Saving the child standalone keeps its ids and writes `EXTENT`.
- **Create Level Instance...** (outliner context menu) writes the selection and what is under it as a new level, re-origined to the first entity (yaw only), and replaces the originals with an instance at the same world poses.
- **Break** turns the instance into ordinary placements in the host level; undo restores the instance.
- **Reload** rebuilds from the child file (after an outside edit); **Streaming** is Follow the level / Always loaded / Streamed; **Packed** and **Bake Pack** are below.

## Streaming

The unit is the instance. Instances stream when `stream 1`, or when they have no explicit setting and the host level has `STREAM` enabled with a device; `stream 0` is always resident. Residency uses `world::PlacementStreamer` over the instances' world boxes: an instance with unknown bounds is never given to it, and stays loaded. A unit loads once every mesh it needs has been read (requested ahead within 1.3x the load distance, with its materials' textures), within 12 ms near / 3 ms far per tick, over as many ticks as it needs: meshes one at a time, each mesh's materials built one at a time (`ContentPort::buildMaterial`), then entities a slice of whole subtrees at a time once their materials exist. A unit the viewer leaves past the unload distance mid-load is dropped, a pinned one finishes at once, and any change to its record (overrides, packed, edit) restarts it. Evictions 2 ms. An instance that is selected, being edited, has a dirty override or was loaded by hand is pinned. Before a unit is evicted its overrides are stored on the record, so it reloads with them. Nested instances load and unload with their top-level unit.

A unit realises over ticks, so its size bounds how long it takes to appear, not a hitch; still keep a streamed child to a few thousand placements (the Details panel warns above 5000).

## Packed mode

`packed` (Details checkbox, `SetPacked`) renders an instance's statically packable mesh placements as rows of an instance table through the foliage path, with static bodies kept for rows that collide. **Bake Pack** (Details, or `Sandbox.exe --bake-level-pack <content-relative level>`) writes `Binaries/LevelPacks/<flat>.ocinst` and `.ocpack`. The instance falls back to ordinary entities, silently and with the reason shown in Details, when the renderer is not in ray-driven primary visibility, there is no pack, the pack is stale (its `SOURCE` no longer matches the level and everything it nests), the instance has any override, or it is being edited. A placement packs only if it is a plain visible mesh placement (not a class, vehicle, animated, hidden or `snap` placement, not under one, and a mesh that loads with at most 16 parts and nothing translucent); everything else is `KEEP`ed as an entity.

Limits: packed rows are ray-traced geometry only. They cast and receive RT shadows and appear in ray-driven primary visibility and reflections; they are not rasterised, not injected into the voxel GI volume and not in the reference path tracer. They have collision but no entity: no scripts, no per-instance hide, no sequence tracks, no editor outline. Use it for repeated static dressing (props, bricks, rubble); do not pack hero geometry.

## The packaged game

Child levels ship in `Content/` and packs in `Binaries/LevelPacks/`; the existing staging copies both. The runtime uses the same `game::LevelInstances` as the editor. Lights: before this feature the packaged game spawned no `LIGHT` record at all (only the editor did); `game::spawnLevelLights` and a shared `game::SceneLightFeed` now light packaged levels too, instanced or not.

## Scripting

C# (`Aver.LevelInstance`, namespace `Aver.LevelInstance`):

```csharp
int root = LevelInstances.Spawn("Levels/Room.ocworld", x, y, z, yaw: 90, scale: 1, packed: false); // runtime-only, 0 on failure
LevelInstances.Destroy(root);
int r = LevelInstances.RootOf(entity);              // 0 when not a member
int e = LevelInstances.Find(root, "b2/c9");         // hex placement ids, leading zeros optional; "" = the root
LevelInstances.SetLoaded(root, false);  LevelInstances.IsLoaded(root);  LevelInstances.Reload(root);
LevelInstances.SetPacked(root, true);               // true = the effective state matches
LevelInstances.TryGetBounds(root, out var min, out var max);
LevelInstances.Count;  LevelInstances.At(i);  LevelInstances.AssetId(root);
```

Every call returns 0/false until a host is installed; `LevelInstanceAbi.LastError` says why a call failed. The C ABI is `modules/levelinstance.abi/include/aver/levelinstance/level_instance_abi.h` (`Aver.LevelInstance.Abi`).

Graph nodes (palette category *Level Instance*): `AN_SpawnLevelInstance` (`level=`), `AN_DestroyLevelInstance`, `AN_GetLevelInstanceRoot`, `AN_FindLevelInstanceEntity` (`path=`), `AN_SetLevelInstanceLoaded`, `AN_IsLevelInstanceLoaded`, `AN_ReloadLevelInstance`, `AN_SetLevelInstancePacked`, `AN_GetLevelInstanceBounds`, `AN_GetLevelInstanceCount`, `AN_GetLevelInstanceAt`.

Events on the existing bus: `LevelInstance.Loaded` / `LevelInstance.Unloaded`, sender = target = the instance root, payload index 0 = the entity count (hook with `AN_OnEvent` and the root as owner).

## Save games

`aver::save` captures `CLevelInstanceLink` with the other components, so a restored world still knows its instances; `LevelInstances::rescan()` rebuilds the table from the links. Static bodies are not part of a save (as for ordinary placements).

## Not in v1

Vehicles, landscape, water, post volumes, sequences and PCG inside an instance (the host level's own); per-component overrides (`addcomp` / `rmcomp` are reserved); per-placement streaming inside an instance; merged-mesh packing; Content Browser thumbnails for levels; hot reload from disk (use Reload). Non-uniform instance scale with rotated children makes body composition approximate, as for nested non-uniform placements.

## Where the code is

| Piece | Where |
|---|---|
| `LEVELINST`, `EXTENT`, `.ocpack`, row transform | `modules/formats` (`OcLevelInst.*`, `OcPack.*`) |
| Parse cache, closure, cycle and depth checks, ids, overrides, bounds; the bake | `modules/world` (`LevelAssetLibrary.*`, `LevelPack.*`); `LevelInstance.hpp` gains `Attach` and `addStaticPlacementBody` |
| The link component | `modules/scene/include/aver/scene/LevelInstanceLink.hpp` |
| Realise, stream, edit, overrides, packed feed | `Runtime` (`game::LevelInstances`, `LevelPackBaker`, `LevelLights`, `SceneLightFeed`); foliage extras in `GameFoliage` |
| C ABI and its host | `modules/levelinstance.abi`; `Runtime/include/aver/game/LevelInstancesAbiHost.hpp` |
| C# | `scripting/csharp/Aver.LevelInstance` |
| Editor | `sandbox/src/LevelInstanceEditorModel.*`, `LevelInstanceEditorUi.*`, `SandboxLevelInstances.cpp` |
| Tests | `tests/levelinstance`, `tests/game/src/LevelLightsTest.cpp`, `tests/world/src/LevelInstanceTest.cpp` |
