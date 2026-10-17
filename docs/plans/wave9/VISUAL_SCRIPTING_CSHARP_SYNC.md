# Aver Node and C# API sync, and the graph editor (wave 9)

Design contract for the implementing agent. Owner approved 2026-10-17. Nothing here is built. Rules in force: upgrade the existing node machinery (no parallel system), table-driven nodes only, no new third-party code, lean comments, D3D12 and Vulkan parity (this feature adds no GPU work; the one renderer-facing piece feeds CPU settings both backends already consume), headless ctest where possible, a Windows-only C# suite (`tests/managed/CMakeLists.txt:14-16`), headed manual runs, never rebuild over the owner's running editor.

Every "exists today" claim below was read in the tree on 2026-10-17. Line numbers are last-known-good pointers; grep the symbol when one is off.

---

## 0. Decisions in one screen

1. **Table rows are the only way a new node is made.** No new `OcGraphParser.AddDefaultPins` case, no new `IsExecCapable*` predicate, no new emitter. The hand-written parser cases (about 200; the node reference counted 202 at its last pass and `--counts` prints the current number) stay frozen. A new node is one `Reg(...)` row, one owner method, one palette entry (`GraphNodeDefsXxx.hpp`), one test row.
2. **Registration needs zero shared-file edits (Phase 0).** C# tables are found by reflection (any static zero-argument method of `GameSystemNodes` returning `GenericNodeSpec[]`); palette families are found by a CMake-generated include (`sandbox/src/GraphNodeFamilies.gen.inc`); C# test classes are found by an `[AutoRun]` attribute. Both existing manual conventions (the `.Concat(...)` chain, the include plus `append...NodeDefs(t)` lines, an explicit `Program.cs` line) keep working and are idempotent with discovery, so the three wave-9 docs already written against the old convention need no rework.
3. **Parity becomes enforced.** `scripts/node-catalog.py --check` (ctest `NodeCatalogCheck`) compares every table row with its palette entry (pin name, type, direction, order, defaults, attribute keys), rejects duplicate keys, and verifies the generated reference `docs/nodes/*.md`. Prototype run today: 189 table rows, 189 palette matches, zero pin disagreements, and 5 attribute-key differences (four UI rows whose `@path|name|text` alternatives the palette names by one of them, and the `OnInputAction` trigger), all legitimate. Before this, the only guard was a name-set check against the parser (`tests/editor/src/GraphEditorGeometryTest.cpp:1010`).
4. **139 new nodes in 9 families**, all pins `float/int/bool/exec`, strings as NODE attributes, entities as ints, vectors as loose x/y/z: Math (55), GameFlow incl. timers and project settings (20), SceneQuery (12), RenderProps lights/camera/particles (14), AudioControl (11), UiExt (6), World (3), Vehicle (6), WorldFx sun/fog/post overrides (12). Gameplay palette goes from 396 rows to 535.
5. **One implementation for graph and C#.** Every new node's owner method is a thin `...ForGraph` wrapper over a public C# member (`AverMath`, `AverRandom`, `Vehicles`, `PostFx`, `Sky` additions, `Entity` light/camera/emitter properties, `Game.SetPaused`). A node never reaches an ABI the C# API cannot also reach.
6. **One small native ABI, `framework_worldfx_abi.h`**: script overrides for sun/fog/sky light and named post-process layers, using the "script publishes, host reads each frame" shape of `aver_fw_set_sky_clouds` (`framework_abi.h:614-631`). Hosts apply it in `GameApp.cpp` and the editor Play path. Everything else is managed-only.
7. **Editor v1 (five items):** a Problems panel (all errors and warnings, linked to nodes, live), Find and Find References (graph and project), a shared clipboard (between graph tabs and the OS clipboard), a graph debugger (breakpoints, pause at end of frame, values on wires, frame step), and Collapse to Function. All model code is header-only and ImGui-free, so ctest covers it.
8. **Debugger semantics are honest:** a breakpoint pauses the Play session at the end of the frame in which the node ran (`aver_fw_set_paused`, `framework_abi.h:208`). It cannot stop mid-tick because the engine loop is single-threaded and the editor draws on the same thread. Frame Skip (`SandboxPlay.cpp:1413`) is the existing step.
9. **Not in v1, said plainly:** String/Vec3/array pin types, latent Delay, macros, interfaces, event dispatchers with typed payloads, graph diff, minimap, reroute dots, node-help tooltips, `OpenLevel`, level-streaming queries, save slots, procedural audio, runtime landscape edit. Each is named in section 2.2 tier C with the reason.
10. **Wave-9 contract (section 2.5):** each feature owns one table file `Graph<Family>NodeTable.cs`, one palette file `GraphNodeDefs<Family>.hpp`, one owner class, one test file, and appends nothing to a central list. Events reach graphs through the existing `AN_OnEvent` bus entry, never a new entry node. Failures return false/0 and log once, never throw. FBX is editor-only (two `Assets` nodes), many lights adds no nodes, and its settings are reached by the project key.
11. **Docs stop rotting by construction:** `docs/nodes/*.md` is generated (one page per family, summaries lifted from the owner methods' XML docs), `docs/api/*.md` is a generated public C# surface index, and the hand-written pages lose every count and every "not yet available" claim that is now false.
12. **Size XL, seven phases** (section 4). Phases 0, 1, 6 are mostly mechanical; 2, 3, 4, 5 carry the risk.

---

## 1. What exists

### 1.1 Machinery

| Fact | Where |
|---|---|
| Table node = `GenericNodeSpec` (`Type`, `Kind` Exec/Pure/Trigger, `Owner`, `MethodName`, `Inputs`, `Outputs`, `Args`, `Ret`); default pins come from the row: Exec adds `exec` in and `then` out around the declared pins, Trigger is a single `exec` out | `scripting/csharp/Aver.Graph/GraphGameSystemNodes.cs:16-61`, `:87-103` |
| `Reg(type, kind, owner, method, ins, outs, args, ret)` and the arg-token grammar: `p:pin`, `@attr` (trailing `?` optional, `a|b` alternatives), `=text`, `i:`/`f:`/`b:` constants, `>pin` out parameter | `GraphGameSystemNodes.cs:123-147`, `:34-42` |
| `GameSystemNodes.Validate()` checks every row against the real method (count, types, out parameters, return pin, every input pin feeds a parameter) | `GraphGameSystemNodes.cs:150-200` |
| The dictionary is built last-wins, so a duplicate key across tables is silently shadowed | `GraphGameSystemNodes.cs:67-73` (`d[s.Type] = s` at `:71`) |
| One chain concatenates the sub-tables; the main `Table()` is the first array | `GraphGameSystemNodeTable.cs:17`, `:120-121` (`GasTable`, `AiQueryTable`, `LevelInstanceTable`, `TerrainTable`, `IkTable`, `MotionMatchTable`, `SequenceTable`) |
| Parser falls through to the table for pins, the compiler for both paths | `OcGraphParser.cs:2582`; `GraphCompiler.cs:705-711` (pull), `:2854` (exec), `:4144`, `:4159` (predicates), `:4199` (pure pull); one generic emitter `GraphCompilerGameSystems.cs:25-58` |
| `AddDefaultPins` returns at the first explicit `PIN` record, so a hand-written PIN set suppresses all defaults | `OcGraphParser.cs:811-814` |
| Pin types are exactly `Float`, `Int`, `Bool`, `Exec` | `Graph.cs:15` |
| Hand-written parser vocabulary: about 200 distinct `case` labels in `AddDefaultPins` (202 at the node reference's last count, aliases included) | `OcGraphParser.cs:811-2590`; `AVER_NODE_NODES.md:11` |
| Table rows today: 189 (97 main + Gas 28 + AiQuery 21 + LevelInstance 11 + Terrain 7 + Ik 11 + MotionMatch 6 + Sequence 8) | `GraphGameSystemNodeTable.cs`, `Graph*NodeTable.cs` |
| The palette is a hand-kept C++ mirror, one `append...NodeDefs(t)` per family; 456 `t.push_back` rows, 60 material, so **396 gameplay rows** (docs claim 252 and 254) | `sandbox/src/GraphNodeDefs.hpp:108-116` (includes), `:1101-1109` (calls); `AVER_NODE_NODES.md:11`; `VISUAL_SCRIPTING.md:448-451` |
| The editor draws pins it did not find in the file from the palette, so a node with no palette entry shows no pins | `sandbox/src/GraphEditor.cpp:269-290` (`synthesizeMissingPins`) |
| Parity today is by discipline: the doc itself says so, and the one test compares type-name sets by scraping parser `case` labels | `AVER_NODE_NODES.md:1022`; `tests/editor/src/GraphEditorGeometryTest.cpp:1010-1043` |
| Failure rule for table nodes: false/0, log once per node id, never throw; strings are attributes | `docs/plans/wave2/GAS.md:442-444` |
| Events reach graphs through the bus entry `AN_OnEvent`; timers fire named graph events | `GraphGameSystemNodeTable.cs:18-19`; `AVER_NODE_NODES.md:678-686` |
| A hit recorder already runs on every exec node (static bool test when off); editor arms it once at startup | `GraphCompiler.cs:2765-2785`, `:5268-5273`; `Aver.Framework/GraphInterop.cs:381-411`; `SandboxApp.cpp:445-450` |
| Optional bridge export groups (validate, hits, classes) bind separately so an old bridge still boots | `modules/scripting/src/ScriptHost.cpp:293-310`; `HostBridge.cs:553` (`GraphValidate`), `:596`, `:619` |
| Play can pause, and a paused session can step one frame | `framework_abi.h:190-208`; `SandboxApp.cpp:2130`, `:2215`; `SandboxPlay.cpp:1413-1417` |
| Component fields are registered append-only, which is what makes `GetField/SetField` reach any component | `modules/scene/src/Builtins.cpp:77-91` (`CLight`), `:93` (`CCamera`), `:100` (the append-only comment), `:120` (`CParticleEmitter`), `:134` (`CSoftBody`) |

### 1.2 Editor

| Capability | State | Where |
|---|---|---|
| Palette search, ranked in three tiers, domain-filtered | exists | `GraphNodeDefs.hpp:1197`; `GraphEditor.cpp:2570-2610` |
| Drag a wire into empty space opens a palette filtered to accepting nodes (context-sensitive) | exists | `GraphEditor.hpp:138-146` |
| Comment boxes (add, colour, around selection) | exists | `GraphEditor.hpp:250`, `:283` |
| Reroute nodes `RerouteFloat/Int/Bool/Exec` (ordinary full-size nodes, not dots) | exists | `GraphNodeDefs.hpp:388-391` |
| Functions: declare, pins, pure flag, rename, delete, call node, one subgraph shown at a time | exists | `GraphEditor.hpp:216-241`; `GraphEditor.cpp:791-884`, `:1027` |
| Macros, collapse to function | **missing** (`VISUAL_SCRIPTING.md:194`, `:197-199`) | |
| Variables panel, attribute details panel, component tree viewport | exists | `GraphEditor.cpp:2677` |
| Validate: one button, first error only, node guessed from quoted text in the message | exists, weak | `GraphEditor.cpp:1709`; `GraphEditor.hpp:154-174`; `HostBridge.cs:553-585`; `Graph.cs:473` (`Validate` returns at the first problem) |
| Live execution highlight (fading node glow) | exists | `GraphEditor.hpp:163-165`; `SandboxApp.cpp:445-450` |
| Breakpoints, step, watch values on wires | **missing** | no hit in `GraphEditor.*` |
| Find in graph, find references | **missing** | no hit |
| Copy, cut, paste, duplicate inside one tab; clipboard is a member, so **no paste between tabs or restarts** | partial | `GraphEditor.hpp:315-318`, `:547` |
| Auto layout, frame all / selection | exists | `GraphEditor.hpp:269-277` |
| Undo for every model edit | exists | `GraphEditor.hpp` (`pushUndo`); `SnapshotUndo.hpp` |
| Graph diff, minimap, bookmarks, node-help tooltips | missing | |

### 1.3 Engine feature coverage matrix (verified)

"Nodes" counts the palette rows that reach the feature; "C#" is the public surface. Tier letters refer to section 2.2.

| Feature | Runtime | Aver Node today | C# today | Gap and action |
|---|---|---|---|---|
| Level instances | `modules/levelinstance.abi`, `Runtime/include/aver/game/LevelInstances.hpp` | 11 (`GraphLevelInstanceNodeTable.cs:14`) | `LevelInstances` (`Aver.LevelInstance/LevelInstances.cs:16`) | none |
| Authored-level streaming | `Runtime/include/aver/game/LevelStreaming.hpp`, `docs/LEVEL_STREAMING.md` | none | none | tier C (no script need yet) |
| Open/travel to a level | only `AVER_FW_END_TRAVEL` (`framework_hooks.h:31`) | none | none (`Game.cs` has no OpenLevel) | tier C |
| Gameplay abilities | `Aver.Framework/Gas/` | 28 (`GraphGasNodeTable.cs:15`) | `GameplayAbilities` (`Gas/GameplayAbilities.cs:107`), `AbilitySystem` (`Gas/AbilitySystem.cs:69`) | none |
| Enhanced Input | `EnhancedInput.cs` | 4 table rows + `OnInputAction` (`GraphGameSystemNodeTable.cs:88-92`) + 14 legacy input nodes | `InputAction`, `InputMappingContext`, `EnhancedInput` (`EnhancedInput.cs:68`, `:340`, `:516`) | context stack is C#-only; `.ocinput` has one CONTEXT. tier C |
| Data tables | `DataTables.cs:208` | 8 (`:94-101`) | `DataTable`, `DataTable<T>`, `DataTableBinder` | none |
| Localization | `Localization.cs:15` | 6 (`:102-107`) | `Loc`, `LocText`, `LocFormat` | dynamic text needs a String pin. tier C |
| EQS, smart objects, behaviour/state trees | `Aver.Synapse` | 21 (`GraphAiQueryNodeTable.cs:16`) | `Eqs` (`Eqs.cs:89`), `SmartObjects` (`:33`), `ManagedAi`, `Entity` brain (`Ai/Brain.cs`) | none |
| Landscape layers and surface | `Landscape.cs:29`, `:132` | 5 reads (`GraphTerrainNodeTable.cs:14`) | reads | runtime sculpt/paint/carve is editor-side only (`modules/landscape/include/aver/landscape/{Sculpt,Paint,Carve}.hpp`). tier C |
| Water | `Water.cs:35` | `AN_WaterSample`, `AN_EntityWater` | `Water`, `Game.SpawnFluidVolume` (`Game.cs:204`) | **add `AN_SpawnFluidVolume`** (World) |
| Montages, root motion | `Montage.cs`, `RootMotion.cs` | 9 + 2 | `Entity.PlayMontage...` (`Montage.cs:28-72`) | none |
| IK and rig inputs | `Ik.cs` | 11 | `Entity.SetIkRig...` | none |
| Motion matching | `MotionMatching.cs` | 6 | `Entity.SetMotionMatching...` | none |
| Ragdolls | `Ragdoll.cs:15-70` | 4 legacy (`RagdollStart/Stop/Strength/Impulse`) | `Entity.StartRagdoll`, `RagdollState`, `HasRagdoll` | **add `AN_GetRagdollState`** (SceneQuery) |
| Physics queries, joints, bodies | `Aver.Physics` | 54 legacy | `Physics`, `Body`, `Joint`, queries | none |
| Vehicles | `physics_vehicle_abi.h`, `Aver.Physics/Vehicle.cs` | none | low-level `Vehicle` only (create, wheels, engine, finish, input) | **add Vehicle family** |
| Soft body | `Aver.Physics/Body.cs:225-245`, `CSoftBody` (`Components.hpp:235`) | none | `Physics.AddSoftBody` (`Aver.Physics/Physics.cs:162`) | owned by `SOFTBODY_CLOTH_DESTRUCTION.md` |
| Audio, streamed audio, reverb zones | `Audio.cs:169`, `AudioStreaming.cs:122` | 16 (6 legacy, 10 `AN_`) | `Audio`, `Music`, `Voice` | **add AudioControl** (master/bus/voice control) |
| Procedural audio (Aver Sound) | `modules/sound/include/aver/sound/Synth.hpp`, `docs/SOUND.md` | none | none | tier C |
| Game UI | `Aver.UI/Widgets.cs:231` | 14 | `Widget`, `UiSystem`, `SettingsScreen` | **add UiExt** (generic property, items, selection, theme, input state) |
| Save | `modules/save`, `Game.cs` | `SaveGame`, `LoadGame` | `Game.SaveGame/LoadGame` | slots (exists/delete/list). tier C |
| Sequencer | `LevelSequence.cs:18` | 8 | `LevelSequences` | none |
| Render queue | `modules/formats/include/aver/formats/RenderJob.hpp`, `docs/RENDER_QUEUE.md` | n/a | n/a | by design: an offline editor/CLI job runner, not gameplay |
| Post-process volumes | `OcPostVolume` (`OcWorld.hpp:611`), `Runtime/include/aver/game/GamePostVolumes.hpp` | none | none | **WorldFx** |
| Sun, fog, sky light | `OcWorldEnv` (`OcWorld.hpp:450`), `assets/LevelSky.hpp` | none | `Sky.SetClouds` only (`Sky.cs:25`) | **WorldFx** + clouds nodes (World) |
| PCG | `Aver.Pcg.Apply/PcgApply.cs:49`, `GameStreaming.hpp` | none | `PcgApply` | tier C |
| Particles | `CParticleEmitter` (`Components.hpp:195`, `Builtins.cpp:120`) | none (GetField only) | none | **RenderProps** |
| Lights | `CLight` (`Components.hpp:124`) | none (GetField only) | none | **RenderProps** |
| Camera | `CCamera` (`Components.hpp:140`) | `SetViewEntity` | `AverCharacter.CameraViewMode` | **RenderProps** (fov, clip, priority); shake is `ACCESSIBILITY.md` |
| Crowds, hearing, cover, squads | `Aver.Synapse/Crowd.cs:40` | 14 | `Crowd`, `Hearing`, `Cover`, `Squad` | none |
| Decals, prefabs | `Aver.Scene/Decals.cs:65`, `Aver.Prefab/Prefabs.cs:13` | 3 + 5 | public | none |
| Timers and events | `Timers.cs:51`, `Events.cs:108` | 5 | public | pause/reset/remaining/dilation/time. **GameFlow** |
| Project settings store | `Settings.cs:20`, opened by `HostBridge.cs:420` | none | public | **GameFlow** |
| Scene iteration | `Game.AllActors/WithTag/ActorsOf` (`Game.cs`), `EntityScene.cs` | `FindEntity`, tag nodes | public | counted access by index. **SceneQuery** |
| Math, random | `System.Math` only | 19 scalar + 9 vector, no integer math, no random, no trig beyond sin/cos | none of its own | **Math** |
| Materials at runtime | `Aver.Scripting/Pbr.cs`; `docs/plans/wave4/MATERIAL_INSTANCES_FUNCTIONS.md` (not landed) | `SetMaterial` | `PbrMaterial` | owned by that plan |
| Networking | `modules/net` is a README | none | none | no runtime exists |

### 1.4 Docs and generators

No script generates, checks or counts `AVER_NODE_NODES.md`: `grep AVER_NODE_NODES scripts` returns nothing. The nearest precedents are `scripts/module-catalog.py --check/--write` registered as ctest `ModuleCatalogCheck` (`CMakeLists.txt:719-731`) and `scripts/docs-check.py` (path existence only).

Stale claims to fix: node counts (`AVER_NODE_NODES.md:3`, `:11`; `VISUAL_SCRIPTING.md:448-451`), "No `HUD` / macros" (`VISUAL_SCRIPTING.md:194`, `:197-199`), the "Game systems" section header list (`AVER_NODE_NODES.md:676`), `SCRIPTING_API.md:869-891` ("Not yet available": audio, timers and widgets all exist now), `SCRIPTING.md:156-182` (claims `Aver.Framework` is not reachable and that input, physics and audio are absent).

---

## 2. Design

### 2.1 Registration infrastructure (Phase 0)

**C# discovery.** In `GraphGameSystemNodes.cs`, replace `ByType()`'s `Table()` call with `AllRows()`:

```csharp
// every static zero-parameter method of GameSystemNodes returning GenericNodeSpec[], by name
private static IEnumerable<GenericNodeSpec> AllRows() => typeof(GameSystemNodes)
    .GetMethods(BindingFlags.Static | BindingFlags.Public | BindingFlags.NonPublic)
    .Where(m => m.GetParameters().Length == 0 && m.ReturnType == typeof(GenericNodeSpec[]))
    .OrderBy(m => m.Name, StringComparer.Ordinal)
    .SelectMany(m => ((GenericNodeSpec[])m.Invoke(null, null)!).Select(s => { s.Family = Family(m.Name); return s; }));
```

- `GenericNodeSpec` gains `internal string Family { get; set; }` (method name without the `Table` suffix; `Table()` is `GameSystem`).
- Duplicate handling: a second row with the same `Type` is **benign** when `Owner`, `MethodName`, `Kind`, `Args` and pins are identical (it is the old `.Concat` line also being discovered), otherwise it is recorded; `Validate()` reports it as `"{type}: defined by {FamilyA} and {FamilyB}"`. `TryAdd`, not `d[s.Type] = s`.
- `GraphGameSystemNodeTable.cs:120-121`: delete the `.Concat(...)` chain, keep `.ToArray()`. This is the one edit to that file in this feature and the last one anybody needs.
- No trimming or AOT is used by `Aver.Graph.csproj`, so reflection is safe.

**Palette discovery.** New `cmake/GraphNodeFamilies.cmake`, included once from the root `CMakeLists.txt` after `project()`:

1. `file(GLOB hdrs CONFIGURE_DEPENDS ".../sandbox/src/GraphNodeDefs*.hpp")`, drop `GraphNodeDefs.hpp` itself.
2. Order: the nine legacy headers in today's order (`SynapseAi, GameSystems, Gas, AiQuery, LevelInstances, Terrain, Ik, MotionMatch, Sequencer`, i.e. the order of `GraphNodeDefs.hpp:1101-1109`) so the category menu order does not change, then every other header alphabetically.
3. For each header read its text and take the function name from `inline void (append\w*NodeDefs)\(` (this tolerates the two legacy singular names, `appendGameSystemNodeDefs` and `appendLevelInstanceNodeDefs`); a header with none is a configure error naming the file.
4. Write `sandbox/src/GraphNodeFamilies.gen.inc` only when the content changed (no rebuild churn):
   ```cpp
   // generated by cmake/GraphNodeFamilies.cmake; do not edit
   #include "GraphNodeDefsSynapseAi.hpp"  ...
   inline void appendGeneratedFamilies(std::vector<GraphNodeDesc>& t) { appendSynapseAiNodeDefs(t); ... }
   ```
5. Add the file to `.gitignore`. It lives in the source tree so `tests/editor` and every other target that already has `sandbox/src` on its include path needs no CMake edit (`tests/editor/CMakeLists.txt:139-146`).

`GraphNodeDefs.hpp` edits: replace the nine includes (`:108-116`) with `#include "GraphNodeFamilies.gen.inc"` (guarded by `__has_include`, else `#error "re-run cmake"`), replace the nine calls (`:1101-1109`) with `appendGeneratedFamilies(t);`, and finish `buildCatalog()` with a stable de-duplication on `(lowercase typeId, domain)` keeping the first, so an explicit `appendXxxNodeDefs(t)` line added by an agent following the old convention does not double the rows.

**Test discovery.** `Aver.Graph.Tests/AutoRun.cs`: `[AutoRun]` attribute plus `AutoRun.RunDiscovered()` which runs `public static int RunAll()` on every attributed class in name order and returns the failure sum. `Program.cs` gets one call after the last explicit `RunAll()` (`:300`) and before the summary. Rule: a class is either listed explicitly in `Program.cs` or marked `[AutoRun]`, never both.

**Catalog script, `scripts/node-catalog.py` (stdlib Python 3).**

| Command | Effect |
|---|---|
| `--check` | exit 1 on any problem below (ctest `NodeCatalogCheck`, registered beside `ModuleCatalogCheck`) |
| `--write` | regenerate `docs/nodes/<Family>.md` and `docs/nodes/Core.md` |
| `--counts` | print table/palette/parser counts, for the sentence that replaces the stale numbers |
| `--scaffold-palette <Family> --category <Cat>` | print a `GraphNodeDefs<Family>.hpp` body from the C# rows (display name from the PascalCase key), to be hand-tuned |

Checks: (C1) every `Reg(` line parses (count of `Reg(` equals count parsed); (C2) every table row has a palette entry whose typeId equals the key case-insensitively; (C3) pins derived from the row (`exec`, inputs, `then`, outputs for Exec; inputs then outputs for Pure; one `exec` out for Trigger) equal the palette's in name, type, direction and order, defaults equal as numbers; (C4) palette attribute keys equal the row's `@` tokens, where a palette key may be any of an `a|b|c` alternative and Trigger rows are skipped; (C5) a palette entry in a family header with no row is reported unless the family is hand-parsed (`Core`); (C6) keys unique across tables, palette headers and the parser's case labels and aliases; (C7) naming for families **not** in the legacy set: key equals lower-case palette typeId, typeId is `[A-Za-z0-9_]` with no space, category non-empty; (C8) generated pages current; (C9) a non-legacy family has `Aver.Graph.Tests/<Family>NodeTests.cs`. The palette scan must be a brace-matching scanner, not one regex: the prototype's regex missed 5 of 267 rows in `GraphNodeDefs.hpp`.

**Generated pages.** One page per family: type, display name, category, Kind mapped to the existing Path codes (Pure D, Exec P, Trigger X), pins, attributes (required or optional), and the C# method with its one-line summary taken from the `/// <summary>` above the method in the file declaring the owner class. An optional hand-written `docs/nodes/<Family>.notes.md` is included verbatim above the table. `docs/nodes/Core.md` lists the hand-parsed nodes from the palette (type, display, category, pins, attributes), which closes the "palette entries with no row" gap `AVER_NODE_NODES.md:3` admits (`SaveGame`, `LoadGame`, `SetSkeleton`, the Synapse nodes).

**C# API index, `scripts/csharp-api-index.py` (`--check/--write`, ctest `CsharpApiIndexCheck`).** A source scanner (no build) that writes `docs/api/<Assembly>.md` for `Aver.Framework`, `Aver.Scene`, `Aver.Physics`, `Aver.UI`, `Aver.Synapse`, `Aver.LevelInstance`, `Aver.Prefab`, `Aver.Scripting`, `Aver.Pcg.Apply`: every `public` type and member as a single-line signature with its first summary sentence. It reads source rather than XML because the XML only exists after a build and `Aver.Framework` is the only project that emits it (`Aver.Framework.csproj`, `GenerateDocumentationFile`). Known limit: it is a scanner; nested types are listed flat with the parent prefix.

### 2.2 Additions, prioritised

**Tier A: managed only, ships in v1 (127 nodes).** Row grammar for the tables below: parameters of the owner method map in order, a parameter named like an input pin is `p:`, a `string` parameter is `@attr`, an `out` parameter is `>pin`, the return value is the pin in the Ret column. Keys are written without the `an_` prefix; the table key is `an_<key>`, the palette id `AN_<Pascal>`. Pin types are `float` unless marked `:i` (int) or `:b` (bool).

*Math* (`GraphMathNodeTable.cs`, `MathTable()`, owner `Aver.Framework.AverMath`, public static; category as listed; all Pure unless noted; `Random` is Exec because a Pure node is recomputed per reader and two readers would get two values).

| Key | Signature (pins) | Rule |
|---|---|---|
| `tan` `asin` `acos` `atan` | `a` to `result` | `asin`/`acos` clamp the input to [-1, 1] |
| `atan2` | `y`, `x` to `result` | radians |
| `sign` `frac` `truncate` | `a` to `result` | `frac` = `a - floor(a)` |
| `degtorad` `radtodeg` `exp` | `a` to `result` | |
| `log` | `a` to `result` | `a <= 0` gives 0 (the `Divide` convention, not `Sqrt`'s NaN) |
| `smoothstep` | `edge0`, `edge1`, `x` | equal edges give a step at the edge |
| `inverselerp` | `a`, `b`, `value` | `a == b` gives 0, not clamped |
| `maprange` | `value`, `inMin=0`, `inMax=1`, `outMin=0`, `outMax=1`, `clamp:b=true` | `inMin == inMax` gives `outMin` |
| `wrap` | `value`, `min`, `max` | `max <= min` gives `min` |
| `interpto` | `current`, `target`, `deltaTime`, `speed` | `speed <= 0` gives `target`; step never overshoots |
| `deltaangle` `lerpangle` | `a`, `b` (`t`) | degrees, shortest way |
| `nearlyequal` | `a`, `b`, `tolerance=0.0001` to `result:b` | |
| `isfinite` | `a` to `result:b` | |
| `intadd` `intsubtract` `intmultiply` `intmin` `intmax` | `a:i`, `b:i` to `result:i` | unchecked wrap |
| `intdivide` `intmodulo` | `a:i`, `b:i` | `b == 0` gives 0; modulo sign follows the dividend (as `Mod`) |
| `intclamp` | `a:i`, `min:i`, `max:i` | |
| `intabs` `intsign` | `a:i` | |
| `intcompare` | `a:i`, `b:i` to `less:b`, `equal:b`, `greater:b` | exact; floats lose integers above 2^24 |
| `bitand` `bitor` `bitxor` | `a:i`, `b:i` | |
| `bitshiftleft` `bitshiftright` | `a:i`, `count:i` | count masked to 0..31 |
| `bittest` | `value:i`, `bit:i` to `result:b` | |
| `randomfloat` (Exec) | `min=0`, `max=1` to `value` | `AverRandom.NextFloat` |
| `randomint` (Exec) | `min:i=0`, `max:i=1` to `value:i` | inclusive both ends |
| `randombool` (Exec) | `chance=0.5` to `value:b` | |
| `randomunitvector` (Exec) | to `x`, `y`, `z` | uniform on the sphere |
| `setrandomseed` (Exec, no Ret) | `seed:i` | reseeds the shared generator |
| `vecnegate` `vecmultiply` | `ax ay az` (`bx by bz`) to `x y z` | component-wise |
| `veclength2d` `vecdistance2d` | `ax ay` (`bx by`) to `result` | ignores Z |
| `vecreflect` | `vx vy vz`, `nx ny nz` to `x y z` | normal need not be unit |
| `vecproject` | `ax ay az`, `bx by bz` to `x y z` | zero `b` gives zero |
| `vecclamplength` | `vx vy vz`, `maxLength` to `x y z` | |
| `vecangle` | `ax ay az`, `bx by bz` to `result` | degrees, zero vector gives 0 |
| `rotatortoforward` | `yaw`, `pitch` to `x y z` | degrees, `Rot`'s convention (`Aver.Scene/Math.cs:129-146`: yaw about +Z, pitch about +Y) |
| `forwardtorotator` | `x y z` to `yaw`, `pitch` | inverse of the above |
| `rotatevector` | `vx vy vz`, `yaw`, `pitch`, `roll` to `x y z` | via `Rot.ToQuat()` |
| `lookatrotation` | `fromX fromY fromZ`, `toX toY toZ` to `yaw`, `pitch` | |

Palette categories: `Math`, `Integer`, `Random`, `Vector`, `Rotator`. `AverMath`/`AverRandom` also carry `Vec3`/`Rot` overloads for C# callers (`Reflect`, `Project`, `InterpTo`, `LookAt`, `ForwardOf`).

*GameFlow* (`GraphGameFlowNodeTable.cs`, owner `Aver.Framework.GraphGameFlow`, internal, over `Game`, `Timers`, `Settings`). Categories `Game`, `Events`, `Settings`.

| Key | Kind | Pins | Attr | Wraps |
|---|---|---|---|---|
| `gettime` `getrealtime` | Pure | out `seconds` | | `Timers.Time`, `Timers.RealTime` (`Timers.cs`) |
| `gettimedilation` | Pure | out `value` | | `Timers.TimeDilation` |
| `settimedilation` | Exec | in `value=1`; out `success:b` | | |
| `isgamepaused` | Pure | out `paused:b` | | `Game.IsPaused` (`IsPlaying` is false while paused; this is the missing read) |
| `setgamepaused` | Exec | in `paused:b`; out `success:b` | | **new** `Game.SetPaused(bool)` over `aver_fw_set_paused` |
| `quitgame` | Exec | none, no Ret | | `Game.Quit` |
| `timerremaining` | Pure | in `handle:i`; out `seconds` | | `Timers.Remaining` |
| `istimeractive` | Pure | in `handle:i`; out `active:b` | | `Timers.IsActive` |
| `pausetimer` | Exec | in `handle:i`, `paused:b`; out `success:b` | | `Timers.SetPaused` |
| `resettimer` | Exec | in `handle:i`, `delay=-1`; out `success:b` | | `Timers.Reset` |
| `gettimerfirecount` | Pure | in `handle:i`; out `count:i` | | `Timers.FireCount` |
| `getsettingfloat` `getsettingint` `getsettingbool` | Pure | in `fallback`; out `value` | `key` | `Settings.Get*`; returns `fallback` when unset or the store is closed |
| `hassetting` | Pure | out `has:b` | `key` | `Settings.Has` |
| `setsettingfloat` `setsettingint` `setsettingbool` | Exec | in `value`; out `success:b` | `key` | `Settings.Set*` |
| `flushsettings` | Exec | out `success:b` | | `Settings.Flush` |

(`fallback`, not `default`, because the method parameter is C#-legal.)

*SceneQuery* (`GraphSceneQueryNodeTable.cs`, owner `GraphSceneQueries`, internal; category `Scene`). Counted access instead of arrays: the existing counted `ForEach` plus `...At(index)` iterates a set.

| Key | Kind | Pins | Attr |
|---|---|---|---|
| `countactorswithtag` | Pure | in `mask:i`; out `count:i` | |
| `getactorwithtagat` | Pure | in `mask:i`, `index:i`; out `entity:i` (0 out of range) | |
| `countactorsofclass` | Pure | out `count:i` | `class` |
| `getactorofclassat` | Pure | in `index:i`; out `entity:i` | `class` |
| `getchildcount` | Pure | in `entity:i`; out `count:i` | |
| `getchildat` | Pure | in `entity:i`, `index:i`; out `child:i` | |
| `getparent` | Pure | in `entity:i`; out `parent:i` | |
| `hascomponent` | Pure | in `entity:i`; out `has:b` | `component` |
| `addcomponent` | Exec | in `entity:i`; out `success:b` | `component` |
| `getdistance` | Pure | in `a:i`, `b:i`; out `distance`, `success:b` | |
| `getragdollstate` | Pure | in `entity:i`; out `mode:i` (-1 none; 0 Kinematic, 1 Limp, 2 Powered), `has:b` | |
| `getactorcount` | Pure | out `count:i` (`Game.AllActors`) | |

*RenderProps* (`GraphRenderPropsNodeTable.cs`, owner `RenderPropsGraph`, internal; public side is a new partial `Entity` file `EntityRenderProps.cs`). All write through the existing scene fields, so they work on any entity carrying the component and need no ABI. Categories `Light`, `Camera`, `Particles`.

| Key | Kind | Pins | Attr | Field(s) |
|---|---|---|---|---|
| `setlightintensity` | Exec | `entity:i`, `lux`; `success:b` | | `CLight.intensityLux` (off is 0; a light has no enable bit, `Components.hpp:124-138`) |
| `getlightintensity` | Pure | `entity:i`; `lux`, `found:b` | | |
| `setlightcolor` | Exec | `entity:i`, `r`, `g`, `b`; `success:b` | | `CLight.colour` |
| `setlightrange` | Exec | `entity:i`, `rangeCm`; `success:b` | | `CLight.rangeCm` |
| `setlightcone` | Exec | `entity:i`, `innerDeg=20`, `outerDeg=35`; `success:b` | | `innerCos`, `outerCos` (degrees converted; inner clamped to <= outer) |
| `setlightshadows` | Exec | `entity:i`, `enabled:b=true`; `success:b` | | `CLight.flags` bit `kLightNoShadows` (`Components.hpp:116`) |
| `setcamerafov` | Exec | `entity:i`, `fovDeg=60`; `success:b` | | `CCamera.fovYRad` (degrees converted) |
| `getcamerafov` | Pure | `entity:i`; `fovDeg`, `found:b` | | |
| `setcameraclip` | Exec | `entity:i`, `nearCm=5`, `farCm=500000` | | |
| `setcamerapriority` | Exec | `entity:i`, `priority:i` | | |
| `setemitterenabled` | Exec | `entity:i`, `enabled:b=true` | | `CParticleEmitter.flags` bit 0 (`kParticleEmitterStopped`, negative sense) |
| `setemittereffect` | Exec | `entity:i` | `effect` | `CParticleEmitter.effect` = `Assets.ObjectIdOf(path)` |
| `restartemitter` | Exec | `entity:i` | | `age = 0`, clear stopped bit |
| `spawnemitterat` | Exec | `x`, `y`, `z`, `lifetime=3`; `entity:i` | `effect` | creates the entity, adds `CParticleEmitter`, destroys it after `lifetime` through `Timers.Set` (0 or negative: never) |

*AudioControl* (`GraphAudioControlNodeTable.cs`, owner `AudioControlGraph`, internal, over `Audio`, `Voice`, `Music`; category `Audio`): `setmastervolume` (`volume=1`), `getmastervolume` (out `volume`), `getbusvolume` (`bus:i`; `volume`), `stopallsounds`, `setvoicevolume` (`voice:i`, `volume`), `setvoicepitch` (`voice:i`, `pitch=1`), `setvoiceposition` (`voice:i`, `x y z`), `isvoiceplaying` (`voice:i`; `playing:b`), `stopvoice` (`voice:i`), `setocclusioncurve` (`minCutoffHz=800`, `minVolume=0.25`), `getaudiostats` (Pure; `activeVoices:i`, `activeStreams:i`, `underruns:i`). Voice handles come from the existing `AN_PlayStream` / `AN_PlayStreamAt` / `AN_PlayMusic` `voice` pin.

*UiExt* (`GraphUiExtNodeTable.cs`, owner `UiExtGraph`, internal; category `UI`): `setuiproperty` (`widget:i`, `value`; attr `property`; `Widget.Set(string, float)`), `setuipropertytext` (`widget:i`; attrs `property`, `text`), `setuiitems` (`widget:i`; attr `items`, `|`-separated; `Widget.SetItems`), `setuiselected` (`widget:i`, `index:i`), `setuitheme` (attr `theme`; `UiSystem.SetTheme`), `getuiinputstate` (Pure; `pointer:b`, `keyboard:b`, `pausesGame:b`, `showCursor:b`; `UiSystem.Wants*`). **UI scale is deliberately not here**: `ACCESSIBILITY.md` owns it as the persisted option `graphics.uiScale`, set through `SetAccessibilityOption`.

*World* (`GraphWorldNodeTable.cs`, owner `WorldGraph`, internal; category `World`): `setskyclouds` (`seed:i`, `coverage=0.45`, `density=1`, `bottomCm=150000`, `topCm=280000`, `featureScale=0.00002`, `windX=900`, `windY=260`; `Sky.SetClouds`, defaults copied from `Sky.cs:36-40`), `clearskyclouds` (`Sky.Clear`), `spawnfluidvolume` (`cx cy cz`, `hx hy hz`; attr `name?`; `Game.SpawnFluidVolume`, `Game.cs:204`; queued, so `success` means a provider accepted it).

*Vehicle* (`GraphVehicleNodeTable.cs`, owner `VehicleGraph`, internal; new public `Aver.Framework.Vehicles` wrapping `Aver.Physics.Vehicle`, `Vehicle.cs`; category `Vehicle`). `Vehicles.CreateCar(Vec3 position, float yawDegrees, CarSpec spec)` builds the chassis, four wheels (front steered, rear driven, an axle each so the differential and anti-roll bar are created, `Vehicle.cs:50-58`), engine and `Finish`, then creates a scene entity and calls `Body.SetEntity` so the physics step moves it. `CarSpec.Default` (frozen constants): half extents 230 x 95 x 45 cm, ground clearance 30, centre of mass (0, 0, -10), mass 1500 kg, wheelbase 280, track 160, wheel radius 36 and width 25, suspension 30 to 50 cm at 1.6 Hz and damping 0.5, front steer 30 degrees, brake 1500 N*m, handbrake 4000 N*m on the rear, engine 500 N*m at 1000 to 6000 rpm, `Finish(60)`.

| Key | Kind | Pins |
|---|---|---|
| `vehiclecreatecar` | Exec | in `x y z`, `yaw`, `massKg=1500`; out `vehicle:i` (0 on failure), `entity:i` |
| `vehiclesetinput` | Exec | in `vehicle:i`, `throttle`, `steer`, `brake`, `handbrake`; out `success:b` |
| `vehiclegetstate` | Pure | in `vehicle:i`; out `found:b`, `speedCmS`, `x y z`, `yaw` |
| `vehiclesetpose` | Exec | in `vehicle:i`, `x y z`, `yaw`; out `success:b` |
| `vehicledestroy` | Exec | in `vehicle:i`; out `success:b` |
| `vehiclewheelcontact` | Pure | in `vehicle:i`, `index:i`; out `contact:b` |

**Tier B: small native ABI, ships in v1 (12 nodes): WorldFx** (section 2.4).

**Tier C: named and deferred.** `OpenLevel`/travel (needs a host hand-over in `GameApp.cpp`, no hook exists); level-streaming residency queries; save slots (list, exists, delete); procedural audio (Aver Sound); runtime landscape sculpt/paint/carve; `AN_PcgRegenerate`; material parameter nodes (`MATERIAL_INSTANCES_FUNCTIONS.md` owns them and has not landed); navigation-query nodes beyond the Synapse set; a mapping-context stack; and the node-system limits: String, Vec3 and array pin types (changes the `.ocgraph` grammar in both readers, `OcGraph.cpp` and `OcGraphParser.cs`; needs the owner's decision), latent `Delay`, macros, interfaces, event dispatchers with typed payloads. `AN_DispatchEvent` already carries one int and one float (`GraphGameSystemNodeTable.cs:21`).

**C# public API added** (parity rule, decision 5): `AverMath`, `AverRandom`, `Game.SetPaused/Pause/Resume`, `Game.ActorsOfClass(string)`, `Vehicles` + `CarSpec`, `Sky` additions (`SetSunDirection`, `SetSunIntensity`, `SetSunColor`, `SetFog...`, `SetSkyLight`, `ClearOverrides`), `PostFx` (static class), `Entity` light/camera/emitter members (`EntityRenderProps.cs`). `Aver.Framework` is a versioned contract assembly (`Aver.Framework.csproj`); the surface is additive, so the integrator bumps it once for the wave.

### 2.3 Editor (Phases 3 to 5)

All model logic is header-only and ImGui-free so it links into the existing test pattern that compiles `GraphEditor.cpp` into a test binary (`tests/editor/CMakeLists.txt:152-185`). `GraphEditor` gains thin public methods and ImGui glue.

**E1 Problems panel.**
- C#: `GraphDiagnostics.Run(string ocgraphText)` returns `GraphDiagnostic { Severity (Error|Warning), NodeId?, Pin?, Code, Message }`. It parses with a new `OcGraphParser.Parse(text, out g, out err, validate: false)` overload (the final `graph.Validate` call at `OcGraphParser.cs:800` is skipped), then runs `Graph.Diagnose(list)`: `Validate()`'s checks refactored to append instead of return, **same messages in the same order**, `Validate()` returning the first. It then tries `CompileEntryPoint` for every ENTRY and every function body and maps a thrown `InvalidOperationException` to a diagnostic (node id taken from the first quoted token, as `validateNow` does today, `GraphEditor.hpp:174`). Added checks: table node missing a required `@attr` (code `attr.missing`, from `GenericNodeSpec.Args`), exec node unreachable from any entry or function entry (warning `exec.unreachable`), VAR never read or never written (warnings `var.unused`, `var.writeonly`). A hard parse failure is one error with code `parse`.
- Bridge: optional export `GraphDiagnose(utf8Text, byte* buffer, capacity)` returning lines `E|W<TAB>nodeId<TAB>pin<TAB>code<TAB>message`, 64 KB buffer, truncated at a line with a final `...N more` line; `ScriptHost::graphDiagnose/graphDiagnoseAvailable` bound as another optional group (`ScriptHost.cpp:293-310` pattern). `GraphValidate` stays.
- Editor: `GraphEditor::setDiagnoser(DiagnoseFn)`; `diagnostics()`; `runDiagnosticsNow()`. Runs on a debounce (0.5 s after the last edit counted by `pushUndo`), on open, and before Play. A Problems tab under the canvas lists severity, node, message; click selects and frames the node (`selectNode`, `frameSelection`). Error and warning badges at the node corner; toolbar text `2 errors, 1 warning` beside the Validate button (`GraphEditor.cpp:1709`). The existing Validate button remains and now shows the full list. Model in `sandbox/src/GraphProblems.hpp` (wire-line parser, sort, per-node badge map, debounce decision as a pure function).

**E2 Find and Find References.** `sandbox/src/GraphFind.hpp`:
- `findInGraph(const fmt::OcGraphData&, query, scope)` matches node id, type, palette display name, attribute values, comment text, variable and function names; ranked exact id, then name prefix, then substring.
- `findReferences(graph, RefKind, name)` for `Variable` (`var=` on `GetVar`/`SetVar`), `Function` (`call=`, `func=`), `Event` (`CustomEvent` name, `FireEvent`/`AN_OnEvent`/`AN_DispatchEvent` `event=`, `ENTRY` names), `Class` (`Spawn class=`).
- `findInProject(contentDir, kind, name)` scans `*.ocgraph` under Content through `fmt::loadOcgraph` (`OcGraph.hpp:351`) and returns `{file, nodeId}`; a click opens the tab (`AssetEditorHost`) and calls `revealNode`.
- UI: Ctrl+F opens a find bar on the canvas (query, `3/12`, Enter next, Shift+Enter previous); right-click on a variable row, function row or a `GetVar/SetVar/CallFunc/CustomEvent` node offers Find References, with a "whole project" toggle.

**E3 Shared clipboard.** `sandbox/src/GraphClipboard.hpp`: `serializeClip(nodes, links, vars, functions)` writes `AVER_NODE_CLIP 1` followed by the same records `writeOcgraph` (`OcGraph.hpp:355`) emits for just those nodes, links, and the VAR/FUNC records they reference; `parseClip(text)` returns nothing for text without the magic. The clip lives in a process-wide store **and** `ImGui::SetClipboardText`, so paste works between graph tabs, after the source tab closes, after restart, and pasting into a text editor shows readable `.ocgraph`. On paste: refuse across domains (gameplay vs material) with the rejection banner (`showRejectionBanner`, `GraphEditor.hpp:625`); missing VARs are declared with the carried type and default in the same undo step and reported; a `CallFunc` whose function is absent is dropped and listed. `clipNodes_/clipLinks_` (`GraphEditor.hpp:547-548`) become that store.

**E4 Debugger.** C# `GraphDebug` (new, `Aver.Graph/GraphDebug.cs`):
- `GraphDebug.Armed` (set by the editor via bridge export `GraphDebugSetEnabled(int)`) is read **at compile time**: with it off the emitted IL is byte-identical to today, so a packaged game and an undebugged editor pay nothing. Arming applies to graphs compiled afterwards (take effect on the next Play start; the toolbar says so).
- `EmitExecNode` (`GraphCompiler.cs:2765`) emits, right after the existing `RecordNodeHit` call (`:2785`), `GraphDebug.NodeReached(graph, node)` which checks the breakpoint set (`HashSet<string>` of `graph\x1fnode`, replaced wholesale by `GraphDebugSetBreakpoints(utf8)`) and latches a pending break.
- `EmitPullInput` (`GraphCompiler.cs:4070`) is renamed `EmitPullInputCore` and a new `EmitPullInput` calls it, then (armed only) stores the value in a temp, loads `graph, node, pin, (double)value` and calls `GraphDebug.RecordPin`, then reloads the temp. The last value per `node.pin` is kept (floats, ints and bools widened to double). Only values on data inputs are captured; exec-node result pins are not.
- Exports: `GraphDebugSetEnabled`, `GraphDebugSetBreakpoints`, `GraphDebugTakeBreak(byte*, cap)` (returns `graph\x1fnode` once, then empty), `GraphDebugGetValues(utf8Graph, byte*, cap)` (`node.pin=value;`). `ScriptHost` gets matching optional-group methods.
- Editor: F9 toggles a breakpoint on the selected node (red dot at the node corner); the set persists per user through `setPrefString` (`EditorPrefs.hpp:29`), keyed by graph name, not in the `.ocgraph`. `SandboxApp` polls `GraphDebugTakeBreak` each frame; on a hit it calls `aver_fw_set_paused(1)`, opens the tab and calls `revealNode`, shows a notification, and the node pulses red. While paused, wires show their last value as a small label (hover for all), fed by `GraphDebugGetValues`. Resume is the existing Pause chord (`SandboxApp.cpp:2130`); stepping is Frame Skip (`requestPlayFrameStep`, `SandboxPlay.cpp:1413`). A node on an `OnTick` chain breaks again every frame after resume; the node's context menu has "Disable breakpoint". Model in `sandbox/src/GraphDebugModel.hpp` (key encode/decode, value formatting, stale expiry).

**E5 Collapse to Function.** `sandbox/src/GraphCollapse.hpp`: `collapse(OcGraphData&, selectedIds, name) -> {ok, reason}`, called by `GraphEditor::collapseSelectionToFunction` inside one `pushUndo`. Built on `addFunction`, `addFunctionPin`, `addCallNode` (`GraphEditor.cpp:791`, `:884`, `:1027`):
- Data inputs: one function input per distinct crossing source pin (named `<node>_<pin>`, deduplicated), type from the pin. Data outputs likewise from crossing sink pins.
- Exec: at most one crossing exec input link and one crossing exec output target; none of either makes the function `pure`. More than one of either is refused with `"selection has N exec exits; v1 supports one"` (and the entry equivalent).
- Refused with a named reason: `ENTRY` nodes, `OnStart/OnTick/OnHit/CustomEvent/AN_OnEvent`, `FuncEntry/FuncReturn`, `Param` nodes (the graph PARAM list does not exist inside a function), a selection that splits a `While/ForEach` from its loop body.
- Result: `FUNC`, `FUNCIN`/`FUNCOUT`, moved nodes get `func=`, crossing links re-pointed through the new `FuncEntry`/`FuncReturn`, a `CallFunc` node placed at the selection centroid. Node ids stay unique (file-wide namespace). Behaviour is unchanged, which `CollapseFunctionTests` proves by running both graphs.
- Expand (the inverse) and true macros are tier C.

**Smaller items folded into the phases:** `GraphEditor::revealNode(id)` (select, frame, switch subgraph), the Problems/Find/Debug state saved in editor prefs, and the "pasted nodes need variable X" banner.

### 2.4 WorldFx native ABI (Phase 2)

New `modules/framework/include/aver/framework/framework_worldfx_abi.h` (own `AVER_FW_WORLDFX_ABI_VERSION 1`, `framework_abi.h`'s `AVER_FW_ABI_VERSION_MINOR` untouched, pattern of `framework_ik_abi.h`), `modules/framework/src/WorldFxAbi.cpp`, C# `WorldFxNative.cs` (`FwWorldFx`, `Lib = "Aver.Framework"` as `IkNative.cs:13`). All state lives in the framework DLL (no provider relay), `int32_t/int64_t/float` only, 1 on success and 0 on refusal.

```c
#define AVER_FW_ENV_SUN_DIR      1  /* v0..v2, world direction TOWARD the light (OcWorldEnv::sunDir) */
#define AVER_FW_ENV_SUN_LUX      2  /* v0 */
#define AVER_FW_ENV_SUN_COLOR    3  /* v0..v2 linear rgb */
#define AVER_FW_ENV_SUN_TEMP     4  /* v0 kelvin, 0 = use colour */
#define AVER_FW_ENV_FOG_DENSITY  5  /* v0 per cm */
#define AVER_FW_ENV_FOG_COLOR    6
#define AVER_FW_ENV_FOG_FALLOFF  7
#define AVER_FW_ENV_FOG_HEIGHT   8  /* world Z at which density is exactly fogDensity */
#define AVER_FW_ENV_FOG_START    9  /* cm */
#define AVER_FW_ENV_FOG_MAXOPAC 10
#define AVER_FW_ENV_SKY_LIGHT   11  /* multiplier on the sky-hemisphere irradiance */
AVER_FW_ABI int32_t aver_fw_worldfx_abi_version(void);
AVER_FW_ABI int32_t aver_fw_env_set(int32_t key, float v0, float v1, float v2);   /* 0: unknown key */
AVER_FW_ABI int32_t aver_fw_env_clear(int32_t key);                                /* 0 clears all */
AVER_FW_ABI int32_t aver_fw_env_get(int32_t key, float* outV3);                    /* 1 when overridden */
AVER_FW_ABI int32_t aver_fw_post_set(int64_t layer, int64_t keyHash, float v0, float v1, float v2);
AVER_FW_ABI int32_t aver_fw_post_set_weight(int64_t layer, float weight, float blendSeconds);
AVER_FW_ABI int32_t aver_fw_post_clear(int64_t layer);                             /* 0 clears all */
AVER_FW_ABI int32_t aver_fw_post_volume_weight(int64_t volumeNameHash, float weight, float blendSeconds); /* weight < 0 removes the scale */
/* host side, once per frame, before reading */
AVER_FW_ABI void    aver_fw_worldfx_tick(float dt);
AVER_FW_ABI int32_t aver_fw_post_layer_count(void);
AVER_FW_ABI int32_t aver_fw_post_layer_info(int32_t index, int64_t* outLayer, float* outWeight, int32_t* outValueCount);
AVER_FW_ABI int32_t aver_fw_post_layer_value(int32_t index, int32_t valueIndex, int64_t* outKeyHash, float* outV3);
AVER_FW_ABI int32_t aver_fw_post_volume_scale(int64_t volumeNameHash, float* outWeight);
```

Names and keys travel as `fnv1a64` hashes (`Assets.ObjectIdOf` in C#), the IK ABI's rule, so no string marshalling. A post key hash is `fnv1a64` of the lower-case key from `kOcPostKeys` (`OcWorld.hpp:636-666`); the host resolves hashes by hashing that table once. An unknown post key is refused (returns 0) and logged once.

Host application (identical in both hosts, so both backends see the same CPU settings):
- **Environment:** a new inline `assets::applyScriptEnv(rhi::SkyAtmosphere&, ...)` appended to `modules/assets/include/aver/assets/LevelSky.hpp` (the shared mapping both hosts already use) and called each frame next to the cloud override (`Runtime/src/GameApp.cpp:1696-1721`); the editor calls it only during a Play session, so authoring is untouched. Releasing a key restores the level's authored value (the same rule as `scriptSkyReported_`).
- **Post layers:** immediately before `blendPostVolumes` (`GameApp.cpp:1780`; the editor path `sandbox/src/PostVolumeTools.cpp:75`), build the volume vector as authored volumes (their `weight` multiplied by `aver_fw_post_volume_scale`) followed by one transient infinite `OcPostVolume` per layer, priority `10000 + index`, weight from `aver_fw_post_layer_info`. The order in a frame is therefore layers, then `blendPostVolumes`, then the player accessibility override (`ACCESSIBILITY.md` section 2, `applyAccessibilityToPost`), which wins by design.
- C#: `Sky` gains `SetSunDirection`, `SetSunIntensity`, `SetSunColor`, `SetFogDensity`, `SetFogColor`, `SetFogShape`, `SetSkyLight`, `ClearOverrides`; new `PostFx` static class: `SetValue(layer, key, v...)`, `SetLayerWeight`, `ClearLayer`, `SetVolumeWeight`.

Nodes (`GraphWorldFxNodeTable.cs`, owner `WorldFxGraph`, category `World`, all Exec): `setsundirection` (`x y z`), `setsunintensity` (`lux`), `setsuncolor` (`r g b`, `temperatureK=0`), `setfogdensity` (`density`), `setfogcolor` (`r g b`), `setfogshape` (`falloff`, `height`, `start`, `maxOpacity=1`), `setskylight` (`multiplier=1`), `clearenvironment` (attr `key?` one of `sun.dir`, `fog.density`, ... or empty for all), `setpostvalue` (`v0 v1 v2`; attrs `layer?`, `key`), `setpostlayerweight` (`weight=1`, `blendSeconds=0`; attr `layer?`), `clearpostlayer` (attr `layer?`), `setpostvolumeweight` (`weight`, `blendSeconds`; attr `volume`). An absent `layer` means the layer named `script`. All return `success:b`.

### 2.5 Wave-9 integration contract

**The rules every wave-9 feature follows** (they restate decisions 1, 2, 10; the "old way" column is still valid):

| Item | Rule | Old way, still valid |
|---|---|---|
| Table | `scripting/csharp/Aver.Graph/Graph<Family>NodeTable.cs` holding `private static GenericNodeSpec[] <Family>Table() => new[] {...}` inside `partial class GameSystemNodes` | `.Concat(<Family>Table())` on `GraphGameSystemNodeTable.cs:120-121`; or rows appended to the main `Table()` (FBX does this) |
| Palette | `sandbox/src/GraphNodeDefs<Family>.hpp` with `inline void append<Family>NodeDefs(std::vector<GraphNodeDesc>& t)` mirroring the rows pin for pin; run `scripts/node-catalog.py --scaffold-palette` first | include and call lines in `GraphNodeDefs.hpp` (de-duplicated) |
| Owner | `internal static class <Family>Graph` in `Aver.Framework` (visible to `Aver.Graph` through `InternalsVisibleTo`, `Aver.Framework.csproj`) or a public static class in the feature's own assembly already referenced by `Aver.Graph.csproj`; methods are `...ForGraph`, primitives only | |
| Tests | `Aver.Graph.Tests/<Family>NodeTests.cs`, `[AutoRun]`, built from `NodeRowKit` (`CheckRows`, `CheckDefaultPins`, `CheckIlValid`) | an explicit `failures += ...RunAll();` in `Program.cs` (not both) |
| Names | table key = lower-case palette typeId. New families: prefix `AN_` for any word a user might also type as a plain verb or noun (`AN_Announce`, not `Announce`); keys never collide with the parser case labels or any other key (checked) | bare names accepted for families already specified that way |
| Pins | `float/int/bool/exec`; strings are attributes; entities and handles are ints; vectors are loose `x y z`; enums are ints documented in the family page | |
| Failure | return false/0/default and log once per node id; never throw (`GAS.md:442`) | |
| Events | raise through `Events.Dispatch("Feature.Event", ...)` (or the sequencer Event track) so a graph reacts with `AN_OnEvent`; no new entry node kind | `AccessibilityGraphInterop`/`SoftBody*` event names already chosen in their docs stand |
| Components | register fields in `Builtins.cpp` append-only (`:100`), negative-sense flags (zero-filled storage); that alone gives `GetField/SetField` and `COMP` records | |
| ABI | new header `framework_<feature>_abi.h` (or the feature's own `*.abi` module), `aver_fw_<feature>_*`, own `AVER_FW_<FEATURE>_ABI_VERSION`, C# `<Feature>Native.cs` with `FwXxx`, row in `tests/abi/src/AbiEnumTest.cpp` for any enum | |
| C# | public static class or partial `Entity` file in `Aver.Framework`; XML `<summary>` on every public member; the node method wraps it | |
| Docs | feature doc keeps a "Graph nodes" table; `scripts/node-catalog.py --write` produces the reference; no hand edits to `docs/nodes/` | the feature doc lists rows for `AVER_NODE_NODES.md` (an integrator step) |

**Per feature** (what exists in `docs/plans/wave9/` today is quoted, the rest is the default the author should adopt):

| Feature | Nodes | Family and files | C# | ABI | Notes |
|---|---|---|---|---|---|
| UI text: font sizes, kerning, CJK and Arabic shaping | yes (author decides rows; likely font size, weight, alignment, wrap per widget) | `UiText`: `GraphUiTextNodeTable.cs`, `GraphNodeDefsUiText.hpp`, owner `UiTextGraph` | `Widget` properties in `Aver.UI/Widgets.cs` (that feature's edit) | own `ui_widget_abi.h` additions | do **not** extend the `UiGraphInterop.UiNodeForGraph` switch (`UiGraphInterop.cs:26`); use typed rows. `docs/plans/wave9` has no UI-text doc yet when this was written |
| Accessibility | 11 nodes, category `Accessibility` (`ACCESSIBILITY.md:351-367`) | `Accessibility`; bare names as specified there | `Accessibility`, `Captions`, `Speech`, `CameraFx` | `framework_access_abi.h` | owns UI scale and text scale (this feature adds none); events `Access.Changed`, `Caption.Show`; applies after my post layers |
| FBX import | 2 nodes, category `Assets` (`FBX_IMPORT.md:239-270`) | rows appended to the main `Table()` and `GraphNodeDefsGameSystems.hpp`; editor-only (a provider exists only in the editor) | `ModelImport` | `framework_import_abi.h` | nothing else; imported assets are referenced by `SetMesh`, `PlayAnimation`, `PlayMontage` |
| Morph targets and facial animation | yes | `Morph`: prefix `AN_Morph*`, category `Morph`, channel names as attributes hashed like IK names | partial `Entity` file `EntityMorph.cs` (`SetMorphWeight`, `GetMorphWeight`, `ClearMorphWeights`) | `framework_morph_abi.h` | animation curves driving morphs should also raise `Morph.*` bus events if a graph needs them; component fields append-only |
| Cloth and destruction (soft body) | 8 nodes, category `Physics` (`SOFTBODY_CLOTH_DESTRUCTION.md:495-510`) | `SoftBody`; bare names as specified | `SoftBodies`, `Aver.Physics.SoftBody` | `physics_softbody_abi.h`, `framework_softbody_abi.h` | events `SoftBodyTorn` etc. via `AN_OnEvent`; wind is independent of `Sky` clouds |
| Many lights | **none** (`MANY_LIGHTS.md:193-195`) | no files | none | none | settings only: project key `RENDER.MANYLIGHTS`, console `voxi.manyLights*`. Scripts that change lights use RenderProps nodes (this feature) or `GetField/SetField` on `CLight`. A runtime toggle, if ever wanted, goes on the C# `Voxi` class (`Aver.Scripting/Voxi.cs`), not a node |

**Overlaps resolved here:** UI scale (Accessibility owns it), camera shake and bob (Accessibility `CameraFx`), sun and fog scripting (WorldFx, nobody else), light and emitter field writes (RenderProps, nobody else), `AN_Announce`-style speech (Accessibility). Post-process order in a frame: authored volumes, then script layers (WorldFx), then `blendPostVolumes`, then the accessibility override.

### 2.6 Keeping docs and references in sync (Phase 6)

- `docs/nodes/README.md` (static, says the pages are generated and how); `docs/nodes/*.md` generated; `docs/api/*.md` generated.
- `AVER_NODE_NODES.md`: replace the counts (`:3`, `:11`) by "run `scripts/node-catalog.py --counts`"; delete the "several node families have no row" paragraph (the generated `Core.md` is complete); add a banner above "Game systems" (`:674`) that pins are authoritative in `docs/nodes/`; rewrite "Parser vs. editor catalog" (`:1011-1022`) to say parity is enforced and by what.
- `VISUAL_SCRIPTING.md`: replace the 254/24 claim (`:448-456`), update the macro row (`:194`) and the "no collapsed sub-graphs" paragraph (`:197-205`) for Collapse to Function, add the section "Editor tools" (Problems, Find, clipboard, debugger, collapse) and "Adding a node family" (the contract in 2.5).
- `SCRIPTING_API.md`: rewrite section 19 (`:869-891`), add numbered sections for the public statics added here and an index pointer to `docs/api/`.
- `SCRIPTING.md`: rewrite section 5 (`:156-182`).
- `CHANGELOG.md`: one entry. `docs/ABI.md`: append the WorldFx entry points.

---

## 3. FILE OWNERSHIP

### 3.1 Created (only this feature touches them)

C# runtime and tables
- `scripting/csharp/Aver.Framework/AverMath.cs`, `AverRandom.cs`, `GraphGameFlow.cs`, `GraphSceneQueries.cs`, `RenderPropsGraph.cs`, `EntityRenderProps.cs`, `AudioControlGraph.cs`, `UiExtGraph.cs`, `WorldGraph.cs`, `Vehicles.cs`, `VehicleGraph.cs`, `WorldFxNative.cs`, `PostFx.cs`, `WorldFxGraph.cs`
- `scripting/csharp/Aver.Graph/GraphMathNodeTable.cs`, `GraphGameFlowNodeTable.cs`, `GraphSceneQueryNodeTable.cs`, `GraphRenderPropsNodeTable.cs`, `GraphAudioControlNodeTable.cs`, `GraphUiExtNodeTable.cs`, `GraphWorldNodeTable.cs`, `GraphVehicleNodeTable.cs`, `GraphWorldFxNodeTable.cs`, `GraphDiagnostics.cs`, `GraphDebug.cs`

C# tests (`scripting/csharp/Aver.Graph.Tests/`)
- `AutoRun.cs`, `NodeTableDiscoveryTests.cs`, `MathNodeTests.cs`, `GameFlowNodeTests.cs`, `SceneQueryNodeTests.cs`, `RenderPropsNodeTests.cs`, `AudioControlNodeTests.cs`, `UiExtNodeTests.cs`, `WorldNodeTests.cs`, `VehicleNodeTests.cs`, `WorldFxNodeTests.cs`, `DiagnosticsTests.cs`, `DebugHookTests.cs`, `CollapseFunctionTests.cs`, fixtures `collapse_test.ocgraph` and `collapse_test.output.ocgraph` (written by the C++ test, committed, as `attribute_edit_test.output.ocgraph` is)

Native
- `modules/framework/include/aver/framework/framework_worldfx_abi.h`, `modules/framework/src/WorldFxAbi.cpp`
- `tests/framework_worldfx/CMakeLists.txt`, `tests/framework_worldfx/src/FrameworkWorldFxAbiTest.cpp`
- `tests/graphtools/CMakeLists.txt`, `tests/graphtools/src/{GraphFindTest,GraphProblemsTest,GraphCollapseTest,GraphClipboardTest,GraphDebugModelTest}.cpp`

Editor (`sandbox/src/`)
- `GraphNodeDefsMath.hpp`, `GraphNodeDefsGameFlow.hpp`, `GraphNodeDefsSceneQuery.hpp`, `GraphNodeDefsRenderProps.hpp`, `GraphNodeDefsAudioControl.hpp`, `GraphNodeDefsUiExt.hpp`, `GraphNodeDefsWorld.hpp`, `GraphNodeDefsVehicle.hpp`, `GraphNodeDefsWorldFx.hpp`
- `GraphFind.hpp`, `GraphProblems.hpp`, `GraphClipboard.hpp`, `GraphCollapse.hpp`, `GraphDebugModel.hpp` (all header-only, so `sandbox/CMakeLists.txt` needs no source line)
- `GraphNodeFamilies.gen.inc` (generated, gitignored)

Build and tooling
- `cmake/GraphNodeFamilies.cmake`, `scripts/node-catalog.py`, `scripts/csharp-api-index.py`

Docs
- `docs/nodes/README.md`, generated `docs/nodes/*.md`, generated `docs/api/*.md`

### 3.2 Modified, owned by this feature

- `scripting/csharp/Aver.Graph/GraphGameSystemNodes.cs` (discovery, `Family`, duplicate report), `GraphGameSystemNodeTable.cs` (delete `.Concat` chain, `:120-121`), `Graph.cs` (`Diagnose`), `OcGraphParser.cs` (`Parse(..., validate: false)` overload, `:22`, `:800`), `GraphCompiler.cs` (`EmitExecNode` `:2765`, `EmitPullInput` split `:4070`)
- `scripting/csharp/Aver.Graph.Tests/Program.cs` (one `AutoRun.RunDiscovered()` call, before `:300`'s summary)
- `scripting/csharp/Aver.Framework/Sky.cs` (sun/fog/sky light setters), `Game.cs` (`SetPaused`, `Pause`, `Resume`, `ActorsOfClass`)
- `sandbox/src/GraphEditor.hpp`, `GraphEditor.cpp` (the five editor items), `GraphNodeDefs.hpp` (include swap, de-duplication)
- `modules/assets/include/aver/assets/LevelSky.hpp` (`applyScriptEnv`, additive)
- `tests/abi/src/AbiEnumTest.cpp` (one row for `AVER_FW_ENV_*`; additive)
- `docs/AVER_NODE_NODES.md`, `docs/VISUAL_SCRIPTING.md`, `docs/SCRIPTING_API.md`, `docs/SCRIPTING.md`, `docs/ABI.md` (append), `CHANGELOG.md`

### 3.3 SHARED (other wave-9 features also touch these; every edit here is small and additive)

| File | Edit made by this feature | Who else |
|---|---|---|
| root `CMakeLists.txt` | `include(cmake/GraphNodeFamilies.cmake)`; `add_subdirectory(tests/graphtools)` and, inside the `if(AVER_MODULE_FRAMEWORK)` test block (`:574`) beside `tests/framework_ik` (`:578`), `tests/framework_worldfx`; the `NodeCatalogCheck` and `CsharpApiIndexCheck` blocks beside `ModuleCatalogCheck` (`:719-731`) | accessibility, soft body, FBX add `add_subdirectory` lines |
| `modules/framework/CMakeLists.txt` | `src/WorldFxAbi.cpp` in the source list | soft body, FBX, accessibility add their own `.cpp` |
| `scripting/csharp/Aver.Scripting.Bridge/HostBridge.cs` | exports `GraphDiagnose`, `GraphDebugSetEnabled`, `GraphDebugSetBreakpoints`, `GraphDebugTakeBreak`, `GraphDebugGetValues` in the graph section (`:553-640`) | accessibility edits `DispTickAll` (`:1498`) |
| `modules/scripting/include/aver/scripting/ScriptHost.hpp`, `src/ScriptHost.cpp` | one optional group each for diagnose and debug (`:106-130`, `:293-310`) | none expected |
| `sandbox/src/SandboxApp.cpp`, `SandboxApp.hpp` | install `setDiagnoser`, the debug break poll, breakpoint push (next to `:439-450`) | accessibility, soft body (frame/init insertions) |
| `Runtime/src/GameApp.cpp`, `GameApp.hpp` | call `aver_fw_worldfx_tick`, `applyScriptEnv` (after `:1721`), build the layered volume vector before `:1780` | accessibility wraps the post result at `:1778` and edits the same region; soft body, FBX edit elsewhere |
| `sandbox/src/PostVolumeTools.cpp` | the same layered vector at `:75` (editor Play) | none expected |
| `scripting/csharp/Aver.Framework/Game.cs` | four members (above) | soft body and accessibility add Framework files, not this one |
| `.gitignore` | one line for `GraphNodeFamilies.gen.inc` | |
| `sandbox/src/GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs`, `Program.cs` | Phase 0 edits above; **after Phase 0 nobody else needs to edit these**, but the three wave-9 docs written before it list edits to them (`ACCESSIBILITY.md:445-449`, `SOFTBODY_CLOTH_DESTRUCTION.md:571-573`). Merge rule: keep both; discovery and the de-duplication make the explicit lines harmless; Phase 6 removes the redundant lines | accessibility, soft body, FBX |
| `scripting/csharp/Aver.Graph/Aver.Graph.csproj` | none planned; a feature whose owner class is in an unreferenced assembly adds one `ProjectReference` | possible: morph, UI text |
| `docs/ABI.md`, `docs/AVER_NODE_NODES.md`, `docs/SCRIPTING_API.md`, `docs/VISUAL_SCRIPTING.md`, `CHANGELOG.md` | append-only edits at integration | every feature |

---

## 4. Phased implementation

Each phase ends with its tests green; a phase never leaves the tree unable to build. Phase 0 lands first and alone (its own commit) because every later phase and the other wave-9 features benefit from it; if a wave-9 agent lands first with the old manual lines, Phase 0 absorbs them (idempotent). Standing project rules: lean comments, headed manual runs, do not rebuild over the owner's running editor.

**Phase 0: registration and drift guard (M).**
1. `GraphGameSystemNodes.cs` discovery, `Family`, duplicate report; delete the `.Concat` chain; `NodeTableDiscoveryTests` (row set equals the 189 of today, by key; a synthetic duplicate is reported; an identical repeat is not).
2. `AutoRun.cs` plus the `Program.cs` call.
3. `cmake/GraphNodeFamilies.cmake`, `GraphNodeDefs.hpp` swap and de-duplication, `.gitignore`. Existing editor tests unchanged and green.
4. `scripts/node-catalog.py` `--check/--write/--counts/--scaffold-palette` and `NodeCatalogCheck`; run `--write` to commit `docs/nodes/*.md`.
Exit: `ctest` green, `GraphEditorGeometryTest` and `GraphManagedTests` green, `--check` green on the unmodified tree.

**Phase 1: tier A families (L).** Order: Math (`AverMath`, `AverRandom`), GameFlow (with `Game.SetPaused`), SceneQuery, RenderProps, AudioControl, UiExt, World, Vehicle. Per family: owner class, table, palette, test, `--write`. Exit: each family's `NodeRowKit` rows green; Math numeric tests green; `--check` green.

**Phase 2: WorldFx ABI and nodes (M).** Header, `WorldFxAbi.cpp`, `FwWorldFx`, `Sky`/`PostFx`, `applyScriptEnv`, host wiring in `GameApp.cpp` and the editor Play path, nodes, `FrameworkWorldFxAbiTest`, `AbiEnumTest` row. Exit: native test green; a headed Play changes sun, fog and saturation on D3D12 and `--backend vulkan`.

**Phase 3: Problems panel, Find, clipboard (M).** `GraphDiagnostics.cs` + `Graph.Diagnose` + bridge export + `ScriptHost`, `GraphProblems.hpp`, `GraphFind.hpp`, `GraphClipboard.hpp`, editor glue, tests. Exit: `DiagnosticsTests`, `GraphProblemsTest`, `GraphFindTest`, `GraphClipboardTest` green; the first diagnostic of every existing negative fixture equals the old `Validate` message.

**Phase 4: debugger (M).** `GraphDebug.cs`, compiler hooks, exports, `ScriptHost`, `GraphDebugModel.hpp`, editor glue, pause/notification wiring. Exit: `DebugHookTests` green; armed compile of every fixture graph yields valid IL; unarmed IL unchanged; a headed breakpoint pauses Play.

**Phase 5: Collapse to Function (M).** `GraphCollapse.hpp`, editor command and menu, `GraphCollapseTest` writing `collapse_test.output.ocgraph`, `CollapseFunctionTests`. Exit: both green.

**Phase 6: docs and cleanup (M).** The edits of 2.6, `scripts/csharp-api-index.py` and `CsharpApiIndexCheck`, remove the redundant explicit include/call/`.Concat`/`Program.cs` lines left by the other wave-9 features, `docs-check.py` clean. Exit: `ctest` green; `--check` green; every count in the hand-written docs removed or generated.

---

## 5. Tests

Everything runs without a GPU. `*Test` executables are picked up by the root walk (`aver_register_ctest_in`, `CMakeLists.txt:650-707`); the Python checks and the managed suite need explicit registration.

**C# (`Aver.Graph.Tests`, ctest `GraphManagedTests`, Windows only)**
- `NodeTableDiscoveryTests`: described in Phase 0.
- Every family: `NodeRowKit.CheckRows` (a real method behind every row, types match pins), `CheckDefaultPins` (pin order and default values), `CheckIlValid` (parse, compile, invoke; native failure is not a failure), exec rows refused by the pull compiler with the standard message, pure rows compile under both, a missing required attribute fails by name.
- `MathNodeTests`: golden values against `System.Math` for every float row; `Divide`-style edge cases (`log(0)`, `maprange` with equal input bounds, `wrap` with `max <= min`, `interpto` never overshooting and `speed <= 0`, `deltaangle` of 350 and 10 is 20); integer rows including `intdivide(x, 0)`, `intmodulo` sign, shift masking, `intcompare` for values above 2^24; random rows stay in range, `setrandomseed` makes a sequence repeat, inclusive `randomint` reaches both ends over 10000 draws; `forwardtorotator(rotatortoforward(y, p))` returns `(y, p)` within 1e-3 and `rotatortoforward` agrees with `Quat` rotation of `Vec3.Forward` by `Rot.ToQuat()`.
- `GameFlowNodeTests`, `SceneQueryNodeTests`, `RenderPropsNodeTests` (the field writes need the scene DLL: row and IL checks always, behaviour checks "unavailable is not a failure" as `GasPersistTest` does), `AudioControlNodeTests`, `UiExtNodeTests`, `WorldNodeTests`, `VehicleNodeTests` (`CarSpec.Default` builds a valid `Vehicle` description: wheel count 4, one differential per axle by the geometry rule), `WorldFxNodeTests`.
- `DiagnosticsTests`: a graph with three independent faults returns three diagnostics with node ids; the first message equals `Graph.Validate`'s for each existing negative fixture (`MissingPinTests`, `FunctionTests`, `GraphVarTests`); `attr.missing` for a table node without its required attribute; `exec.unreachable`, `var.unused`, `var.writeonly`; a compile-time exception maps to its node; a parse failure is one `parse` error.
- `DebugHookTests`: unarmed compile of every fixture is unchanged (a counting hook proves `RecordPin` and `NodeReached` are never emitted); armed compile of every fixture is valid IL; a set breakpoint latches once and `TakeBreak` returns it once; float, int and bool values on links are captured and read back.
- `CollapseFunctionTests`: compiles `collapse_test.output.ocgraph`, runs `OnTick` and compares its VAR results with `collapse_test.ocgraph` run the same way.

**C++ (no ImGui, no GPU)**
- `tests/graphtools`: `GraphFindTest` (ranking, scope, every reference kind, project scan over a temp content tree), `GraphProblemsTest` (wire lines, sort, badge map, the debounce decision), `GraphCollapseTest` (pure selection, one-in one-out exec chain, every refusal with its reason, id uniqueness, one undo step through `GraphEditor`, parse round trip, writes the C# fixture), `GraphClipboardTest` (round trip, missing VAR auto-declared, missing function dropped, domain refusal, foreign text ignored), `GraphDebugModelTest` (breakpoint key encode/decode, value formatting, expiry).
- `tests/framework_worldfx`: set/get/clear for every key, unknown key refused, post layer lifecycle, blend advances with `aver_fw_worldfx_tick`, volume scale, version, zero results for bad arguments.
- `GraphEditorGeometryTest` keeps passing unchanged and gains one check: the catalog has no duplicate `(typeId, domain)` after de-duplication.

**Python (ctest)**: `NodeCatalogCheck`, `CsharpApiIndexCheck` (skipped with a status message when no Python 3, as `ModuleCatalogCheck`).

---

## 6. Headed manual verification

Run the editor and the game headed (project rule). Use `test-content/GraphDemo/Game.ocproject` or any project with a graph class.

1. **Registration.** Add a throwaway family file pair (table + palette) with one node, reconfigure, build: the node appears in the palette search with no edit to any shared file; `NodeCatalogCheck` fails when its pin order differs from the table, passes when fixed. Delete the throwaway.
2. **Palette.** Search `atan2`, `randomint`, `spawnemitter`, `vehicle`: rows appear in `Math`, `Random`, `Particles`, `Vehicle`. Drag a wire from an int output into empty space: only int-accepting nodes are offered.
3. **Math probe.** A graph on `OnTick` that prints `maprange`, `interpto`, `intdivide`, `forwardtorotator`: log values match hand calculation.
4. **Flicker.** A point light, `randomfloat` into `setlightintensity` each tick: it flickers; `setlightcone` on a spot light changes the cone.
5. **Particles.** `spawnemitterat` on a key press with a project `.ocparticle`: the emitter appears and removes itself after `lifetime`.
6. **Time of day and fog.** `setsundirection` driven by `gettime`, `setfogdensity`: sun sweeps, fog thickens; `clearenvironment` returns the level's authored sky. Repeat with `--backend vulkan`: same result.
7. **Post layers.** `setpostvalue key=saturation` 0 with `setpostlayerweight` 1 over 2 s: the view desaturates smoothly; `clearpostlayer` restores it; with the Accessibility reduce-motion option on (once landed) its override still wins.
8. **Vehicle.** `vehiclecreatecar` on the floor, `MoveAxis` into `vehiclesetinput`: the car drives and its entity follows; `vehicledestroy` removes it.
9. **Settings and pause.** `setsettingint` then restart: `getsettingint` returns it; `setgamepaused` pauses and the editor shows Paused.
10. **Problems.** Open a graph and make three faults (delete a required attribute, leave a node unlinked, declare an unused VAR): the panel lists all three within a second, badges show on the nodes, a click selects and frames each, fixing one updates the list live.
11. **Find.** Ctrl+F for a node type, step through hits; Find References on a VAR with the project toggle on lists the other graphs using it and opens them.
12. **Clipboard.** Copy nodes in graph A, paste into tab B (missing VAR is declared and reported), close A and paste again, paste into Notepad (readable `.ocgraph`), paste a material graph's nodes into a gameplay graph (refused with a banner).
13. **Debugger.** Arm the debugger, press Play, F9 on a node under `OnTick`: Play pauses at the end of that frame with the node pulsing and wires labelled; Frame Skip advances one frame and re-breaks; "Disable breakpoint" then Pause chord resumes. With the debugger unarmed, frame cost matches before.
14. **Collapse.** Select a chain with one in and one out, Collapse to Function, Play: identical behaviour; a selection containing `OnTick` is refused with the reason; Ctrl+Z restores the original in one step.
15. `ctest` full run and `dotnet run` the graph suite; open `docs/nodes/` and confirm a new row is documented without hand edits.

---

## 7. Open questions (defaults chosen)

1. **Breakpoint semantics.** Pause at the end of the frame, not mid-tick (section 2.3, E4). Default: accepted; a true mid-tick stop needs a second thread or a re-entrant UI loop, both out of scope.
2. **Palette include in the source tree.** `GraphNodeFamilies.gen.inc` is generated into `sandbox/src` and gitignored, so no CMake include path changes. Default: yes; the alternative (build tree) touches every target that includes `GraphNodeDefs.hpp`.
3. **`log(a <= 0)`.** Returns 0, matching `Divide`'s no-NaN rule (`AVER_NODE_NODES.md:142-150`). Default: 0; `Sqrt` keeps its NaN.
4. **String, Vec3 and array pin types.** Not in v1 because they change the `.ocgraph` grammar and both readers. Default: ask the owner before any work; every row above was designed so a later pin type can be adopted without renaming a node.
5. **`Aver.Framework` version.** Additive surface, so one minor bump by the integrator after the wave (as the GAS plan did). Default: bump once at the end of Phase 6, not per feature.
