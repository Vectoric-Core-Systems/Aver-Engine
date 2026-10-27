# Fabric Connector: Godot 4 -> Aver (plan, approved 2026-10-27)

Status tracker: rows flip to DONE only with test evidence (commit + test name).

## Context
The owner wants Godot projects and plugins converted one way into standalone native Aver projects ("Fabric Connector"), and asked whether Aver can behave **1:1 with base Godot**. Three read-only surveys (10-27) checked Godot 4.x feature by feature against the tree: core runtime; rendering, 2D and UI; physics, animation, audio and navigation. This plan answers that question, lists the gaps, orders the work to close them, and keeps the converter design that was agreed earlier.

Owner decisions so far:
- Convert everything into a standalone Aver project.
- Inputs: scenes and assets, GDScript, GodotSharp C#, and compiled GDExtension plugins.
- Godot 4.1 and later only.
- 3D plus UI first; 2D later.
- Vendoring Godot's MIT `gdextension_interface.h` and `extension_api.json` is approved.
- Test corpus: public samples (godot-demo-projects plus a few addons).
- Branch: `08-gdint`.

## Can Aver be 1:1 with Godot? The answer has three levels
1. **The same API and the same rules: yes.** This means tick order, pause modes, lifecycle order, signals, deferred calls, Variant, RNG sequence, input propagation, `move_and_slide` rules, Tween and AnimationPlayer. It all comes down to Aver engine work plus the `Aver.Fabric` runtime. There's no technical blocker, only the size of the job.
2. **Physics behaves the same within a tolerance: yes, if the target is Godot's Jolt module rather than GodotPhysics3D.**
   - Godot 4.4+ ships Jolt, and Aver already runs Jolt, so the remaining differences are Godot's rules layered on top: combine rules, ray defaults, joint collision exclusion, layer/mask filtering, default damping and friction, and `move_and_slide`.
   - Projects on GodotPhysics3D will drift. Bit-identical results would mean porting GodotPhysics3D (MIT) as a second physics backend. **Not recommended.**
3. **Pixel-identical rendering: no, short of porting Godot's renderer.** Aver is physical units, RT and voxel GI with NRD. Godot Forward+ is clustered forward with shadow maps, SDFGI/VoxelGI, screen-space effects, a different BRDF and tonemappers. Recommendation: map parameters across, close the missing features listed below, and accept that Aver's look differs. A "Godot look" raster mode would be a second renderer, an L+ item that needs the owner's say.

## Tier 0: Aver bugs found along the way (fix first; they hurt every Aver game)
| Bug | Evidence | Fix |
|---|---|---|
| Destroying an actor doesn't end the child actors in its subtree. Children get no `OnEndPlay` or unbind, so their C# keeps ticking on dead handles. | `FrameworkAbi.cpp` `destroyActor`; `World::destroy`/`collectAndRetire` | Walk the subtree and end/unbind each child, deepest first. |
| Runtime prefab actor nodes never bind (no `classBound`/`destroying` hooks), and prefab destroy skips `OnEndPlay`. | `Runtime/src/GameApp.cpp:~2030`, `modules/prefab/src/PrefabSystem.cpp` | Wire the hooks the way the editor does. |
| `[Editable]` is never read, so per-instance C# field values don't exist. | `Aver.Framework/Attributes.cs` (no consumer) | Add a placement `PROP` record in the level, apply it on bind, and add an editor details row. |
| Pause stops the UI, input-action and event pumps, so pause-menu C# handlers never fire. `IGNORE_PAUSE` timers stop too. | `HostBridge.cs:1616-1624`, `GameApp::tickGameplay` | Move the pumps out of tick group 0 and run them while paused. |
| The `Ticks(group, order)` order argument is ignored. | `ClassBuilder.cs`, `HostBridge.DispTickAll` | Sort the buckets by order. |
| Solid contacts never report an exit (only sensors do). | `PhysicsWorld.cpp` `OnContactRemoved` | Report removed contacts per body pair. |
| A stale comment says capsule and cylinder are "authoring-only", but `createShapeBody` builds them. | `Components.hpp` | Fix the comment. |

### G0 status (2026-10-27)
| Bug | Status |
|---|---|
| Subtree destroy skips child actors | DONE: `destroyChildActors` in `FrameworkAbi.cpp`. FrameworkTest `testSubtreeDestroyEndsChildren` fails without the fix (2 checks) and passes with it. |
| `Ticks(group, order)` ignored | DONE: the bridge keeps each bucket sorted by order (`InsertByTickOrder`). Compile-checked only, because the bridge's tick list has no headless test seam. |
| Pause stops the UI/content/accessibility pumps | DONE: `AVER_FW_PUMP_FRAME` (framework ABI minor 12), `game::tickPausedFrame`, wired in GameApp and the editor's Play. Gameplay abilities and input-action events stay gated on PLAYING. A headed check by the owner is pending. |
| Stale capsule/cylinder comment | DONE |
| Prefab actor nodes never bind | MOVED to G1. It needs a new "adopt an existing entity as an actor" framework call; neither the runtime nor the editor sets `classBound` today. |
| `[Editable]` unread | MOVED to G1. It needs a level `PROP` record, apply-on-bind, and an editor row. |
| Solid contacts never report an exit | MOVED to G2. Jolt reports a removed contact whenever a body falls asleep, so this needs the per-body contact tracking G2 designs, and must not disturb the snapshot/restore and determinism hashes. |

### G1 status
| Item | Status |
|---|---|
| Tick model → Godot's order (owner decision 2026-10-28: Aver's only order, no compat switch) | DONE: `OnPhysicsTick` once per fixed step before the step (physics step hook), `OnTick` once a frame after, by priority then tree order (`aver_fw_tree_order`, World order stamps). Runtime `TickEnabled`/`PhysicsTickEnabled` and priorities; a throwing tick no longer disables its actor. Framework ABI 2.0. Tests: GameTickOrderTest, FrameworkTest `testTreeOrder`. Graph-class instances still tick in their own pass, once a frame. |
| Time scale on every delta | DONE: `Game.TimeScale` (= timer dilation) scales both passes, each physics step (real-time cadence, `aver_phys_set_time_scale`), timers, anim, particles, AI, vehicles, sequences, decals; 0 holds the world while ticks run; reset at session end. GameTickOrderTest, FrameworkTest. |
| Physics interpolation | open |
| Per-actor pause modes | DONE: `AverActor.PauseMode` (Inherit/Pausable/WhenPaused/Always/Disabled, Inherit resolved up the actor tree); a paused frame runs both passes on the fixed clock for the actors allowed, without stepping physics. GameTickOrderTest, PauseRulesTests. |
| Ready order | DONE: spawn batches (`aver_fw_spawn_batch_begin/end`, framework ABI 2.1) begin a level's placed actors children first, siblings in order; both hosts' level loads use one. FrameworkTest `testSpawnBatchReadyOrder`. |
| Placed actors keep their placement parent | DONE: `PlacementTree` (both hosts): an actor spawns under its file parent (a placement or an earlier actor), an ordinary placement under an actor joins it; the editor save writes real parent slots for actors and placements under them. PlacementTreeTest, GraphClassPlacementTest. |
| enter_tree / exit_tree hooks; transform, visibility and parented notifications | open |
| Deferred queue, await context | DONE: `Deferred.Call`, `AverActor.QueueDestroy`, a game-thread `SynchronizationContext`, `Frame.Next/NextPhysics/Delay`; flushed at each pass's start and end. GameThreadTests. |
| Signal flags (ONE_SHOT/DEFERRED, auto-disconnect on free) | open (with the signal layer) |
| Prefab actor bind (moved from G0) | DONE: `aver_fw_adopt` / `aver_fw_adopt_by_name` (framework ABI 2.2), both hosts' prefab hooks; a pending class adopts on seal; `aver_fw_destroy` (and `Entity.Destroy`) ends every actor below a plain entity. FrameworkTest `testAdopt`. |
| `[Editable]` per-instance values (moved from G0) | open |
| Variant, PCG32 RNG, groups, level travel, files/resources, input chain, platform services | open |

## Gap list, sized S/M/L, and where each fix goes
**E** = Aver engine change (benefits every Aver game). **F** = `Aver.Fabric` runtime only. **C** = converter only.

### Core runtime
| Gap | Where | Size |
|---|---|---|
| Hierarchy-order ticking plus per-instance priority; `set_process(false)` at runtime | E (bridge buckets) | M |
| Fixed-step script callback (`_physics_process` runs N times per frame at 1/60), plus physics interpolation | E (`GameTick.hpp`, `PhysicsWorld`) | M |
| Per-actor pause modes (Inherit/Pausable/WhenPaused/Always/Disabled); `time_scale` applied to actor dt, physics, animation and particles | E | M |
| Tree lifecycle: enter_tree (parent first), ready (children first), exit_tree; transform, visibility and parented notifications | E (spawn path, `World` callbacks) + F | M |
| End-of-frame deferred queue (`call_deferred`, `set_deferred`, `queue_free` at frame end); signal flags ONE_SHOT/DEFERRED; disconnect automatically when an object is freed | E (queue) + F (signals) | M |
| Main-thread `SynchronizationContext` (`await` on signals and timers, and HTTP callbacks marshalled back) | E (bridge) + F | M |
| Reflection: set, get and call by name on C# actors; `set_meta` | F | S |
| Variant (64-bit int, double float); PCG32 RNG with Godot's seeding; `RandomNumberGenerator` instances | F | S |
| `res://` and `user://` resolution; FileAccess, DirAccess, ConfigFile, JSON wrappers; C# actor state in save games | E (content path ABI) + F | M |
| Resource objects (shared, `duplicate`, `local_to_scene`); async load handles (`load_threaded_*`) | E (async load ABI) + F | M |
| Level travel (`change_scene_to_file`, `END_TRAVEL` is defined but unused); autoloads that persist across scene changes | E | M |
| Named groups (string groups, `call_group`) instead of a 32-bit tag mask | E | S |
| Input event objects and the propagation chain (`_input` → GUI → shortcut → unhandled, `set_input_as_handled`); per-event UI/game consumption; touch; simulated input; absolute mouse position; mouse modes; multiple pads; SDL mapping database | E (input) + F | L |
| Runtime window and vsync API; `ProjectSettings.get_setting`; OS feature tags; FPS and frame count; general debug draw from scripts | E | S–M |
| Localization: PO import, a single message-ID `tr()`, Godot format strings | C + F | S |
| **Networking:** MultiplayerAPI, `@rpc`, Spawner/Synchronizer, ENet. `modules/net` is only a skeleton. | E (planned in wave 10) | L |

### Physics (target: Godot's Jolt module)
| Gap | Where | Size |
|---|---|---|
| Godot combine rules: friction = min (max if rough), bounce = clamped sum (subtracted if absorbent); defaults friction 1.0, damping 0.1 | E (`PhysicsSurface.cpp` combine hook) + C | S |
| Ray defaults: hit back faces on, hit from inside off; exclude arrays | E (`PhysicsQueries.cpp`) | S |
| 32-bit layer plus 32-bit mask per body (an ObjectLayerFilter replacing the 16-layer matrix); per-body-pair exceptions; jointed bodies excluded from colliding with each other | E (`physics_layers_abi.h`) | M |
| CharacterBody3D `move_and_slide` re-implemented on `body_cast` and `test_move`: script-applied gravity, any `up_direction`, is_on_wall and is_on_ceiling, slide collisions, floor_* options, floating mode | E/F | M |
| Area3D: signals; gravity, damping and wind overrides; static-body detection; area-vs-area | E | M |
| Contact monitoring per body: max_contacts, body_entered/exited signals, `_integrate_forces` direct state; constant velocity on static bodies (conveyors) | E | M |
| RayCast3D and ShapeCast3D nodes; `intersect_shape`/`collide_shape`/`get_rest_info` for arbitrary shapes; `move_and_collide` with depenetration | E/F | M |
| SeparationRayShape, CollisionPolygon3D (converter extrudes it), per-shape disable | E/C | S |
| Joint parameters translated from Godot to Jolt springs; VehicleBody3D per-wheel force mode (a second controller next to Jolt's wheeled one); SoftBody3D and PhysicalBone3D parameter mapping | E/C | M |

### Animation, audio and navigation
| Gap | Where | Size |
|---|---|---|
| AnimationPlayer: per-clip property tracks on any node (not only level sequences), method tracks with arguments, ping-pong looping, capture mode, crossfade between clips, finished signal, animation libraries, RESET track | E (`.ocanim` property channels plus a player component) | L |
| AnimationTree: `travel()` pathfinding, expression and OR conditions, TimeSeek, Transition, Blend2/3, Add2/3 nodes | E (`modules/anim`) | M |
| Skeleton bone-pose API from scripts; SpringBoneSimulator3D | E | M |
| Tween library; Path3D, Curve3D and PathFollow3D; unlimited-point Curve and Gradient resources | F (Tween), E (curves) | M |
| Audio: bus graph with sends, mute, solo, bypass and an effect chain (delay, chorus, compressor, limiter, EQ, filters, distortion, phaser, pitch shift, capture, spectrum); Godot's 4 attenuation models; unit_size and max_db; doppler; emission cone; pause; per-player polyphony; Randomizer, Polyphonic and Interactive streams; a generator stream; listener following the camera; Ogg decoding that doesn't depend on Windows Media Foundation | E (`modules/audio`) | L |
| Navigation: polygon navmesh with multiple floors, runtime rebake, links, dynamic obstacles, navigation layers and area costs, NavigationAgent3D options and signals, generic AStar3D, AStar2D and AStarGrid2D | E (`modules/synapse`) | L |
| SpringArm3D as a general node; RemoteTransform3D; VisibleOnScreenNotifier3D and Enabler3D; Timer node; XR | E/F | S each (XR is L, deferred) |

### Rendering (goal: the same features with a mapped look, not identical pixels)
| Gap | Where | Size |
|---|---|---|
| **Vertex colour, tangent and UV2 at runtime** (`MeshVertex` has position, normal and UV0 only) | E (`rhi/RHI.hpp`, mesh path) | M |
| **Local lights without RT, and on Vulkan** (today: no omni or spot light at all without D3D12 RT); shadow maps for local lights | E (render.pbr/voxi) | L |
| Tonemappers Linear, Reinhard, Filmic and AgX plus the white point; grading after the tonemap as an option; brightness | E (`color.hlsli`) | S |
| A Godot shader-language front-end: `.gdshader` → `.ocgraph`, which needs vertex output, uniforms, varyings, screen and depth textures, and sky, fog and particle domains in the graph | E + C | L |
| ReflectionProbe, sampling of baked LightmapGI, an SSAO/SSR fallback for non-RT tiers, FogVolume, HDRI panorama sky, sky shaders | E | L (combined) |
| Material features: billboard, triplanar, detail/UV2, rim, anisotropy, parallax, proximity and distance fade, alpha hash and A2C, subtract and multiply blending, depth-draw modes and render priority, grow, shadows-only; wire up the recorded `unlit` and `cull` values | E | M–L |
| Light cull masks plus VisualInstance layers and camera cull_mask; visibility ranges; MultiMesh with per-instance data and a script API; Label3D and Sprite3D; CSG; GridMap and MeshLibrary; runtime mesh building from scripts (ArrayMesh, SurfaceTool, ImmediateMesh) | E | L (combined) |
| SubViewport and ViewportTexture (render to texture); multiple cameras and split screen; orthographic and frustum projection; keep_aspect and h/v offset | E | L |
| A low-end tier (Compatibility/Mobile): D3D11 is a stub | E | L, deferred |

### UI (Control)
| Gap | Where | Size |
|---|---|---|
| StyleBoxFlat (corner radius, per-side borders, shadow) and StyleBoxTexture 9-patch; Theme as a file resource with inheritance | E (`modules/ui`, `render.ui`) | M |
| Widgets: dropdown OptionButton, MenuButton, PopupMenu and dialogs, TabContainer, Tree, TextEdit, SpinBox, TextureButton, TextureProgressBar, NinePatchRect, ColorPicker, sub-windows, vertical Slider, Split, Flow and AspectRatio containers, button icons | E | L |
| Tooltips; drag-and-drop; raw `_gui_input`; mouse_filter pass-through | E | M |
| Full RichTextLabel BBCode (url/meta, img, tables, effects); MSDF fonts, outlines and shadows; OpenType GSUB; variable fonts | E | M–L |
| A viewport stretch mode and letterboxing | E | S |
| VideoStreamPlayer (no video decoder today) | E | M |

### 2D (owner: after v1)
There is no 2D world at all: Node2D, sprites, TileMap, Camera2D, Light2D, Line2D/Polygon2D, `_draw`, canvas shaders, 2D particles, 2D physics. `render.ui` (textured quads, no transforms) is the only starting point. This becomes a separate **L+ wave**.

## Order of work
1. **G0, Tier 0 bug fixes.** S–M, can start now. Headless tests only.
2. **G1, runtime semantics.** The core-runtime E rows plus the `Aver.Fabric` runtime: tick order, fixed step, pause modes, lifecycle, deferred queue, await, Variant, RNG, groups, travel, input chain. This is what makes converted scripts behave 1:1.
3. **G2, physics semantics.** All the physics rows, aimed at Godot's Jolt module. Godot defaults sit behind a per-project `PHYSICS.COMPAT godot` key, so Aver's current defaults don't change for existing projects.
4. **G3, the converter itself.** The F1–F6 below, built alongside G1 and G2 because the corpus shows which gaps actually bite.
5. **G4, rendering basics.** Vertex colour, tangent and UV2; local lights without RT; tonemappers; ortho camera; SubViewport; cull masks; material features.
6. **G5, animation, audio, UI and navigation parity.**
7. **G6, the shader language front-end, then 2D, then networking** (networking stays in wave 10's plan).

Each wave gets a design doc in `docs/plans/fabric/` laid out like `docs/plans/wave9/*.md`, plus a parity tracker table modelled on `docs/plans/UE_FEATURE_PARITY_PLAN.md`: Godot feature / Aver today / v1 to build / size. G1 and G2 change existing systems rather than adding parallel ones, which is the standing UE-parity rule.

## Converter (Fabric Connector), as agreed earlier
- **`modules/formats.godot/`:** parser for Godot's text formats (`.tscn`, `.tres`, `project.godot`, `.import`). Binary `.scn`/`.res` are reported as "resave as text". Template: `modules/formats/src/UsdStageImport.cpp`.
- **`AverAssetC fabric <godot-dir> <out-dir>`** (`tools/fabric/*.cpp`). It reuses:
  - `writeSceneLevel` and `convertImportedModel` (`tools/AverAssetC.cpp`);
  - the glTF, FBX and OBJ importers and `cookImportedMaterials`;
  - the `OcWorld`, `OcProject`, `OcInput` and `OcPrefab` writers.

  Output: `.ocproject`, `.ocinput`, `.ocworld` (nested PLACE), `.ocprefab` (instanced scenes), `.ocmat`, `.ocshape`, `.ocmesh`, plus `FabricReport.md` and `.jsonl`, which rate every item converted / approximated / unsupported. The basis change follows `docs/ASSET_IMPORT.md` (x=−z, y=x, z=y, ×100).
- **`scripting/csharp/Aver.Fabric/`:** the runtime. Node facade over entities and `CHierarchy`; Godot-space math types converted only at the boundary; signals; await; Variant; Tween; SceneTree; Input. It sits on `AverActor`, `[AverClass]` and the G1 engine hooks.
- **`scripting/csharp/Aver.Fabric.Convert/`:** GodotSharp → Aver C# through a Roslyn rewriter (the already-vendored `formats.roslyn`), and GDScript 2.0 → readable C# (parser, type inference, `Variant` fallback). Editor-only code is marked `// FABRIC:` and listed in the report.
- **`modules/fabric.gdext/`:** GDExtension host. It loads plugins through `aver::loadLibrary`, implements the interface subset godot-cpp uses (Variant, String, ClassDB registration, method binds), checks hashes against the vendored `extension_api.json`, and generates C# wrappers for the classes plugins register. The bridge ABI is `fabric_gdext_abi.h`, built to the `docs/ABI.md` §16 checklist. Windows first.
- **Editor:** `File > Import Godot Project…` in `SandboxContentBrowser.cpp` (`importAsset`).
- **Phases:**
  - F0 docs and corpus list
  - F1 formats and 3D scenes
  - F2 runtime
  - F3 GodotSharp
  - F4 GDScript
  - F5 GDExtension
  - F6 editor, CI and docs

## Verification
The engine-run ban is in force: builds and headless tests only; the owner does the headed checks and confirms fixes.
- **Tier 0 / G1 / G2:** new ctest cases in the existing suites.
  - `tests/framework`: subtree destroy, prefab bind, pause pumps, tick order.
  - `tests/physics`: combine rules, ray flags, layer/mask filtering, `move_and_slide` cases against expected values worked out from Godot's documented algorithm.
  - Managed `dotnet run` suites for `Aver.Fabric`: Variant, the PCG32 sequence against Godot's published PCG32 reference output, the await context, signals.
  - The physics determinism golden hashes must stay unchanged unless `PHYSICS.COMPAT godot` is set.
- **Converter:**
  - `FabricFormatsTest` and `FabricConvertTest` on our own hand-written fixtures, with reload through `parseOcworld`, `parseOcproject` and the other parsers.
  - `Aver.Fabric.Tests` golden GDScript→C# and GodotSharp rewrite pairs; the generated C# must compile.
  - `FabricGdextTest`: a tiny in-tree GDExtension built from our own source.
- **Corpus:** convert the approved godot-demo-projects and addons headless (the download is asked for first). Track unsupported count, script compile and level reload in `docs/fabric/CORPUS.md`. The parity tracker rows flip only with test evidence.
- **CI:** the fixture jobs run on all three platforms.
