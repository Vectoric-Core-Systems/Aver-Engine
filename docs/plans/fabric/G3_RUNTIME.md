# G3 F2a: the Aver.Fabric runtime and THE NODE CONTRACT

Project `scripting/csharp/Aver.Fabric` (assembly `Aver.Fabric`, namespace `Aver.Fabric`, net10.0, GodotSharp names). Tests: `scripting/csharp/Aver.Fabric.Tests` (console project, run with `dotnet run -c Debug`; ctest `FabricManagedTests`). The generated API list is `docs/api/Aver.Fabric.md`; this file is the part a list cannot say: how a Godot node is represented in an Aver level, and what the scene converter must write.

Wiring:
- Built and staged to `bin/Scripting` with the other assemblies (`modules/scripting/CMakeLists.txt`, recursive `CONFIGURE_DEPENDS` glob; the bridge references it).
- The bridge declares every non-abstract `Node` subclass of the assembly as a framework class (`HostBridge.DeclareActors`), registry name = C# type name (`Node3D`, `MeshInstance3D`, ...). A test (`the scripting bridge can declare and bind every node class`) pins: public parameterless constructor, distinct names, none equal to a framework base class.
- A project's `Scripts.csproj` gets a `ProjectReference` to `Aver.Fabric.csproj` (`sandbox/src/ProjectScaffold.cpp`, new projects and the migration check that repoints it; it is not demanded of existing projects).
- Converted scripts compile against it: `partial class Player : CharacterBody3D`, `using Aver.Fabric;`.

## 1. The node contract

### 1.1 One node = one entity

Every Godot node of the converted scene is an **entity (placement) in the `.ocworld`**, nested under its parent's placement with `BEGIN`/`END` (the file's own nesting, `CHILD` lines), in Godot's child order. The runtime `Node` for it is always an `AverActor` of an `Aver.Fabric` class. There are two kinds of placement:

**A. Class placement (the normal case, and the only one for a node with a script).** `PLACE none <x y z yaw pitch roll scale> class <ClassName>`.
- `<ClassName>` is the **registry name of the C# type** that implements the node: the script class for a node with a script (`Player`, the converter's C# for `player.gd`; it derives from the node's Godot class), otherwise the Godot class itself (`Node3D`, `MeshInstance3D`, `RigidBody3D` ... the table in 1.4). An `[AverClass("Name")]` attribute on a script class overrides the name; without it the C# type name is the name. Two scripts must therefore not share a type name.
- The framework spawns it (spawn batch, Godot's order, 1.3), the bridge binds a new instance of the type, and the node reads its values from the entity's Props (1.2).
- The transform is the placement's transform (1.5). Other mesh/material tokens on the line are inert for a class placement; a mesh is the node's `mesh` property.

**B. Plain placement.** An ordinary placement (a mesh, or a bare `PLACE none ... nocollide` marker) that is not an actor. The converter uses this only for a node with no script and no behaviour of its own that it has already turned into engine content (typically a static mesh with an `.ocmesh`/`.ocmat` and a baked collider). `Node.FromEntity(entity)` wraps it lazily (a `Node3D`, cached, always "in the tree" and "ready", never ticking), so `GetNode("Crate")`, `GetParent()`, `GetChildren()` and `Position` work for it. A `COMPONENT Props` record may carry `_class=<Node3D-derived Godot class>` (e.g. `Marker3D`, `OmniLight3D`) and `_name=<name>`; the wrapper then has that type. A wrapper has no `_Ready`, no signals from the engine and no properties applied: if the converter needs any of that it must write a class placement.

Rule of thumb for the converter: **class placement for everything**, plain placement only as an optimisation for pure static geometry.

### 1.2 Properties: `COMPONENT Props`

Every Godot property of the node that is not the transform is written into **one** `COMPONENT Props key=value ...` record on the node's placement (G1 Editables; `OcPlacedComponent{type="Props", kv}`; hosts load it into the framework props store before the node enters the tree). The writer percent-encodes the values, so the converter passes **raw text**. Keys are lower-cased by the level reader; the runtime matches them against members **ignoring case and underscores** (`move_speed`, `movespeed`, `MoveSpeed` all find `MoveSpeed`; exact spellings are tried first). Order matters: values are applied in file order, before `_EnterTree`.

- **Key**: Godot's property name, snake_case as in the `.tscn` (`collision_layer`, `light_energy`, `process_mode`, `unique_name_in_owner`, `custom_prop`). `metadata/tag` is `SetMeta("tag", ...)` (lower-cased, as every level key is). Other path-style names (`surface_material_override/0`) are not mapped; the converter reports them. A key the node class has no member for is kept as node metadata (`GetMeta("key")`) and logged once as a warning.
- **Value**: Godot's **text form of the Variant**, exactly what the `.tscn` has after `=`, read by `Aver.Fabric.VariantText` and written by `GodotVariant`'s canonical writer in `modules/formats.godot` (G3_FORMATS.md). Accepted: `null true false`, ints (hex too), floats (`inf inf_neg nan`), `"strings"`, `&"StringName"`, `^"NodePath"`, `NodePath("..")`, `Vector2/2i/3/3i/4/4i`, `Color`, `Quaternion`, `Plane`, `Rect2/2i`, `AABB`, `Basis`, `Transform3D`, `Transform2D`, `Projection`, `RID`, `Array`/typed `Array[T](...)` (element type dropped), `Dictionary`, `[ ... ]`, `{ ... }`, all `Packed*Array`, and two resource forms:
  - `Resource("res://x.ocmesh")`: an untyped resource with that path; it becomes the property's own type when assigned (a `Mesh` property gets a `Mesh` whose `ResourcePath` is the path). The converter writes the **Aver asset path** (an `.ocmesh`, `.ocmat`, `.ocshape`, ...), not the Godot one, with or without `res://`.
  - `Object(ClassName, "prop": value, ...)`: an inline resource built by `ClassDB` (`Object(BoxShape3D, "size": Vector3(1, 1, 1))`, `Object(SphereMesh, "radius": 2)`). The class must be an `Aver.Fabric` class (or one a script registered with `ClassDB.Register`); an unknown class makes the value unreadable (warning, property skipped).
  - `ExtResource(...)` / `SubResource(...)` are **never written**: the converter resolves them into the two forms above (the formats reader already checks every reference).
- **Types are converted on assignment**: numbers to the member's numeric type, enums from their integer value, `Array` to typed arrays/`Collections.Array<T>`, a path to a `Node`-typed member (below).
- **Node-typed exports** (`@export var target: Node3D`, written in the scene as `NodePath("../Target")` with `node_paths=`): the converter writes the NodePath **relative to this node**. The runtime looks the node up when the value is applied (or, if it is not linked yet, when the node becomes ready, with a warning if it never appears) and assigns the node when it is of the member's type, otherwise null. An empty path clears the member. Arrays of nodes are not mapped.
- **Reserved keys** (never properties):

| Key | Value | Meaning |
|---|---|---|
| `_name` | text | The node's Godot name. **Always written**: a class placement's `name` token does not rename the spawned actor (the spawner passes the class name). Characters `. : / @ %` are stripped as in Godot. A taken sibling name gets Godot's `@Name@2` form, so the converter keeps names unique among siblings. The converter writes it as a quoted Variant string (`_name="Crate"`); the runtime accepts that and a bare word. |
| `_owner` | NodePath from this node | The node's `owner`. For the scene root: omit. For a node of the scene: the path to the root (`..`, `../..`). For nodes inside an instanced scene: the path to that instance's root. Quoted like `_name`. |
| `_unique` | `1` | `unique_name_in_owner` (`%Name` lookups from inside the owner's scene). Writing the property `unique_name_in_owner = true` does the same. |
| `_class` | Godot class | Plain placements only (1.1 B). |
| `_persistent` | `1` | The node survives `Game.ChangeLevel` (an autoload). |
| `_connections` | Variant text of an array of dictionaries | The scene's `[connection]` lines whose `from` is this node; see 1.6. |

- Other node-level settings are ordinary properties: `process_mode` (int), `process_priority`, `process_physics_priority`, `visible`, `top_level`, `scene_file_path`. Node3D: `position`, `rotation`, `rotation_degrees`, `scale`, `basis`, `transform` (use only when the placement line cannot say it, 1.5).
- **Groups** stay the G1 record: `COMPONENT Groups names=a,b` on the placement (header `groups=["a","b"]` in the scene). The node joins them before `_EnterTree`.
- **Script exports** (`@export` / `[Export]` members of the converted script class) are Props like any other. Their member name is whatever the converter emitted; the fold-case match above finds it. Exports whose value is a resource use the two resource forms; a typed enum is written as its integer.

### 1.3 Lifecycle, in Godot's order

The level load spawns the placements in one **spawn batch** (G1): `_EnterTree` runs for the whole subtree pre-order (parent first), then `_Ready` post-order (children first, then the parent), exactly Godot's. Per node:

1. the node is linked to its parent and the level's values are applied (name, groups, Props, owner) ;
2. `_Notification(NotificationEnterTree)`, `_EnterTree()`, signal `tree_entered`, parent's `child_entered_tree`;
3. later, in the ready pass: scene connections are made, `_Notification(PostEnterTree)`, once only `_Notification(Ready)`, `_Ready()`, signal `ready`.

Leaving the tree (`RemoveChild`, `QueueFree`, a destroyed entity, a level change): `tree_exiting`, `_Notification(ExitTree)`, `_ExitTree()` (children before parents), `tree_exited`. `Free()`/`QueueFree()` go through the framework destroy, which ends every actor below.

The virtuals map onto the framework hooks (`Aver.Framework.AverActor`):

| Godot | Framework hook | Notes |
|---|---|---|
| `_EnterTree` | `OnEnterTree` (dispatch v3) | pre-order in a batch |
| `_Ready` | `OnBeginPlay` | post-order; runs once, re-adding does not repeat it |
| `_ExitTree` | `OnExitTree` | before `OnEndPlay` |
| `_Process(double)` | `OnTick` | once a frame, priority then tree order; scaled by `Game.TimeScale` |
| `_PhysicsProcess(double)` | `OnPhysicsTick` | once per fixed step before the step |
| `_Input`, `_UnhandledInput`, `_UnhandledKeyInput` | `OnInput`, `OnUnhandledInput` | the G1 chain (UI between them) |
| `_Notification(what)` | called by the runtime | `NotificationParented/Unparented/EnterTree/ExitTree/Ready/PostEnterTree/TransformChanged/VisibilityChanged ...` as Godot numbers them |
| `process_mode` | `PauseMode` | `ProcessModeEnum` values map 1:1 |
| `process_priority` / `process_physics_priority` | `TickPriority` / `PhysicsTickPriority` | |
| `SetProcess/SetPhysicsProcess/SetProcessInput...` | `TickEnabled`, `PhysicsTickEnabled`, `InputEnabled` | A class that overrides none of `_Process`, `_PhysicsProcess`, `_Input...` does not tick at all; overriding is what turns processing on (as in Godot), and `SetProcess(false)` turns it off. |
| `CallDeferred`, `SetDeferred`, `QueueFree`, `await` | `Deferred`, `QueueDestroy`, the game-thread context | flushed at each pass's start and end |

### 1.4 Class table (what the converter may place today)

| Godot class | Fabric class | What it does in the engine |
|---|---|---|
| `Node` | `Node` | tree only (an entity with no transform use) |
| `Node3D`, `Marker3D` | `Node3D`, `Marker3D` | transform |
| `VisualInstance3D`, `GeometryInstance3D` | same | values: `layers`, `cast_shadow`, `material_override` ... (held; not all drawn) |
| `MeshInstance3D` | `MeshInstance3D` | `mesh` resource path becomes the entity's mesh renderer; `material_override` path its material; visibility reaches the mesh flag |
| `Camera3D` | `Camera3D` | camera component (`fov` degrees vertical, `near`, `far` metres converted to cm), `current` makes it the view entity when ready |
| `DirectionalLight3D`, `OmniLight3D`, `SpotLight3D` | same | omni and spot: a light component (kind, sRGB colour to linear, `light_energy * Light3D.CandelaPerEnergy`, shadows, `omni_range`/`spot_range` to cm, `spot_angle` as the cone half-angle with an 80 per cent inner cone for the ray-traced path, and the Godot block: attenuation exponents, `light_specular`, `light_negative`, `light_size`, bias and distance fade; G4_RENDER.md R2). Pushed on entering the tree and on every later change. Calibration (G4 R2): energy 1 = pi engine units = 104719.755 lux or candela (`Light3D.LuxPerEnergy`, `CandelaPerEnergy`). A `DirectionalLight3D` adds no component: the first one is the sun node and forwards its later changes to the sky's sun overrides; the level's `SUN` record is the sun. |
| `CollisionShape3D` | `CollisionShape3D` | holds `shape` and `disabled`; its **parent** reads it when the parent becomes ready (direct children only, with their local transform and scale) |
| `StaticBody3D`, `AnimatableBody3D`, `RigidBody3D`, `CharacterBody3D`, `Area3D` | same | an Aver physics body built when the node is ready from its `CollisionShape3D` children (box, sphere, capsule, cylinder, convex polygon, concave polygon and world boundary shapes; the body is made again when a child shape is added, changed or disabled); layer/mask are 32-bit (G2 filters) and can change at runtime; section 5.6 has the rest |
| `RayCast3D`, `ShapeCast3D` | same | cast every physics step on the G2 space-state queries (5.6) |
| `VehicleBody3D`, `VehicleWheel3D` | same | the G2 raycast vehicle: engine force, brake and steering spread over the wheels as Godot does; the wheel nodes follow the suspension (5.6) |
| `Timer` | `Timer` | `wait_time`, `one_shot`, `autostart`, `paused`, `process_callback`, `ignore_time_scale`, signal and event `timeout` |
| `Control`, `CanvasItem`, `Window`, `Viewport` | placeholders | layout values only; the UI package fills them |
| a class a GDExtension plugin registered (F5) | `GDExtensionNode` (extends `Node`), `GDExtensionNode3D` (extends `Node3D`) | the Prop `extension_class` names the native class; its virtuals, methods, properties and signals run from the node (`G3_GDEXT.md`, section 7). The plugin must be loaded with `GDExtensionManager.Load` first |
| any other 3D class | **not yet** | the converter places a `Node3D` with `_class` metadata and reports the node as unsupported or approximated |

Not yet (the converter keeps these as carriers and reports them): joints, `SoftBody3D`, `NavigationAgent3D`, `AnimationPlayer`/`AnimationTree`, `AudioStreamPlayer3D`, `GPUParticles3D`, `Path3D`/`PathFollow3D`, `SpringArm3D`, `Skeleton3D`/`BoneAttachment3D`. (`Sprite3D`, `Label3D`, `CSG*`, `GridMap` and `MultiMeshInstance3D` exist since G4 R7; `Label3D` draws nothing.) All 2D nodes are out of scope. (F2b added `RayCast3D`, `ShapeCast3D`, `VehicleBody3D`, `VehicleWheel3D`, `Tween`, `PackedScene.Instantiate` and the resource loaders; section 5.)

### 1.5 Transforms and units

Scripts see Godot numbers: metres, +Y up, right-handed, `Basis`/`Quaternion` in Godot's conventions. The engine sees Aver numbers. The conversion lives in **one place**, `FabricSpace`, and is the map `modules/formats/src/GltfImport.cpp` applies (`toEngine`, `toEngineQuat`, `toEngineScale`):

| | Godot to Aver |
|---|---|
| position (m) | `(x, y, z) -> (-z, x, y) * 100` (cm) |
| direction, unitless vector | `(x, y, z) -> (-z, x, y)` |
| rotation quaternion | `(x, y, z, w) -> (z, -x, -y, w)` (the axis map has determinant -1: the axis is a pseudovector) |
| scale | `(x, y, z) -> (z, x, y)` |
| length (m) | `* 100` |

The `MathTests` case "FabricSpace: golden values of the glTF map, round trips, rotation consistency" holds the reference values; **the scene converter's C++ side must apply the same map and should be checked against those numbers** (there is no shared C++ helper yet; the first converter package that needs one creates it in `modules/formats` and the glTF/FBX/OBJ importers should call it).

A placement's `PLACE x y z yaw pitch roll scale` is in Aver space (cm, degrees, the placement's local frame, as for any level). `Node3D` reads its local transform from the entity and converts back, so a placement written with the map above shows the node its exact Godot values (to float precision). Two ways to carry what the line cannot:
- non-uniform scale: `PLACEG ... scale3=` where the writer supports it, or the `scale` Prop (`Vector3(sx, sy, sz)`, Godot axes);
- exactness: a `transform` Prop (`Transform3D(...)`) is applied on entering the tree and overrides the line.
The placement's transform is **parent-relative** for a nested placement, as Godot's local transform is.

### 1.6 Signals

- Built-in signal names are Godot's snake_case (`ready`, `tree_entered`, `tree_exiting`, `tree_exited`, `renamed`, `child_entered_tree`, `child_exiting_tree`, `child_order_changed`, `visibility_changed`, `timeout`, `body_entered`, `body_exited`, `area_entered`, `area_exited`, ...), exposed as `Node.SignalName.Ready` etc.
- A script's own signals follow GodotSharp: `[Signal] public delegate void HitEventHandler(int damage);` declares `Hit`, with a nested `SignalName.Hit`. The runtime matches a signal name **ignoring case and underscores**, so a scene (or a GDScript-derived name) saying `hit` or `Hit` reaches the same signal.
- `Connect(signal, Callable, flags)` with `ConnectFlags.Deferred` (runs when the tick pass ends), `Persist` (accepted, no effect at runtime), `OneShot`, `ReferenceCounted`; `Disconnect`, `IsConnected`, `EmitSignal`, `await ToSignal(obj, SignalName.X)`. A connection to a freed object is dropped. A handler that throws is logged and the rest still run.
- **Scene connections** are `_connections` Props on the **source** node: Variant text of an array of dictionaries `{ "signal": "body_entered", "to": "../Door", "method": "_on_body_entered", "flags": 0, "binds": [], "unbinds": 0 }` (`to` is a NodePath from the source node, `flags` Godot's `CONNECT_*` bits, `binds` appended, `unbinds` trailing arguments dropped). They are made when the node becomes ready, after every node of the batch has entered the tree. `method` is looked up with Godot's snake_case turned into PascalCase with leading underscores dropped (`_on_body_entered` -> `OnBodyEntered`), as written, camelCase and `_camelCase`; if the converter names the generated C# method differently it writes that name here. A target that is missing or a method that does not exist is a warning, not a failure.

### 1.7 Instanced scenes, autoloads, the root

- **Instanced scenes** are flattened by the scene converter in v1 into nested placements under the instance node's placement (the instance node itself is a placement of the sub-scene's root class), with `scene_file_path` set as a Prop on the instance root and `_owner` of every node pointing at it. At run time `PackedScene.Instantiate` (5.3) builds any scene's level file afresh, so a script's `preload("res://enemy.tscn").instantiate()` works whether or not the editor flattened it into a level.
- **The root.** `SceneTree.Instance.Root` is the `/root` window; a level's top-level placements are its children (`GetNode("/root/Main")`). Autoloads are top-level class placements with `_persistent=1` and `_name=<AutoloadName>`, first in the file, in `project.godot` order; they stay under `/root` across `ChangeSceneToFile`.
- **`SceneTree`** is complete in F2b (5.1). In the engine two hidden `SceneTreeDriver` nodes (`SceneTreeHead` first, `SceneTreeTail` last in tick order, persistent, process mode Always) are children of the root: they carry the frame signals, timers and tweens. A script that lists `GetTree().Root.GetChildren()` sees them.

## 2. API surface (what scripts get)

- `GodotObject` (an `AverActor`), `RefCounted`, `Resource`; instance ids, `Get/Set/Call/CallDeferred/SetDeferred/HasMethod` by name (Godot's snake_case or C# names), meta, `Free`, `IsQueuedForDeletion`, `GodotObject.InstanceFromId`, `IsInstanceValid`.
- `Node` (tree API with Godot's semantics: names and clash rules, `GetNode<T>` with relative/absolute/`..`/`%Unique` paths, `FindChild(ren)`, `Owner`, groups, `Reparent`, `MoveChild`, `Duplicate`, `ReplaceBy`, `AddSibling`, processing switches), `Node3D` (local/global transform properties, `LookAt`, `Translate`, `Rotate*`, `ToLocal/ToGlobal`, `TopLevel`, visibility with propagation), the node classes of 1.4, `SceneTree`, `Timer`.
- Math in Godot space: `Vector2/2I/3/3I/4/4I`, `Quaternion`, `Basis`, `Transform2D/3D`, `Aabb`, `Plane`, `Rect2/2I`, `Color` (+ `Colors`), `Projection`, `Mathf`; the Aver boundary is `FabricSpace` (public, for the converter's own generated code and tests).
- `Variant` (struct, `VariantType`, implicit conversions, Godot's equality, truthiness and `to_int`/`to_float` rules), `StringName`, `NodePath`, `Callable` (method, delegate/lambda, `Bind`, `Unbind`, `Call`, `CallDeferred`), `Signal`, `Collections.Array`/`Dictionary` and typed views, `Rid`, `VariantText` (parse), `ClassDB`.
- GDScript support (F4b): `GDScript` (Variant operators, `Format`, `Range`, dynamic `GetMember`/`CallMethod` ...), `StringExtensions`/`ArrayExtensions`/`DictionaryExtensions`, `Engine`, `OS`, `ProjectSettings.GetSetting`; see G3_SCRIPTS.md, "What was added to Aver.Fabric for this".
- `GD` (`Print`, `PrintRich` with tags stripped, `PrintErr`, `PushError`, `PushWarning`, `Randf/Randi/RandRange/Randomize/Seed` on the G1 PCG32, `Str`, `Load<T>`, `Preload<T>`, `InstanceFromId`, `IsInstanceValid`), `RandomNumberGenerator`, `ResourceLoader`, `PackedScene`, `Tween`, `Input`, `InputMap`, the `InputEvent` classes, the physics nodes and queries of section 5, attributes `[Export]`, `[Signal]`, `[GlobalClass]`, `[Tool]`, `[Rpc]` (accepted, not networked), `[ExportGroup]` and friends.
- C# events for the signals scripts `+=` most: `Node.Ready/TreeEntered/TreeExiting/TreeExited/Renamed/ChildEnteredTree/ChildExitingTree`, `Timer.Timeout`, `SceneTreeTimer.Timeout`, `SceneTree.ProcessFrame/PhysicsFrame/NodeAdded/NodeRemoved/NodeRenamed/TreeChanged`, `Tween.Finished/StepFinished/LoopFinished`, `Tweener.Finished`, `Resource.Changed`, `RigidBody3D.BodyEntered/BodyExited/SleepingStateChanged`, `Area3D.BodyEntered/BodyExited/AreaEntered/AreaExited`. They are `Connect` with a delegate, so `-=` works.

## 3. Test hosts and seams

`NodeHost.Current` is the engine host unless a test or tool installs `ManagedNodeHost`, which runs the same enter/ready/exit order in-process (`T.Reset()` does this). Everything that touches the engine (spawning, components, physics bodies, `FromEntity`) is behind `Self.IsValid && NodeHost.Current.IsEngine` and is **not** exercised headless; see the status table in `FABRIC_CONNECTOR.md`.

## 4. For the next packages

- **Scene converter**: write levels exactly as in section 1; the checklist is class placement + `_name` + Props (+ `_owner`, `_unique`, `_connections`, Groups) for every node; sub-scenes flattened; resources as `Resource(path)`/`Object(...)`; transforms through the map of 1.5.
- **Later runtime packages**: AnimationPlayer, joints, audio, particles, navigation, UI controls; each new node class is added to the table in 1.4 and needs only a public parameterless constructor and a unique type name.
- **Converted scripts** can rely on: `GetNode<T>("../Foo")` in `_Ready`, `[Export]` members set before `_EnterTree`, `SignalName.X` constants, `EmitSignal(SignalName.X, args)`, `ToSignal`, `Callable.From(...)`, `CallDeferred`, `QueueFree`, and everything in section 5.

## 5. F2b: the services

Tests: `TweenTests`, `ServiceTests`, `InputTests`, `PhysicsWrapperTests` in `Aver.Fabric.Tests` (1132 checks together with F2a's), run by the managed loop `SceneTree.RunManagedFrame` (the tests' stand-in for the engine's tick: `SceneTreeHead` signals, then the nodes by priority and tree order, then timers and tweens, then the deferred queue).

### 5.1 SceneTree

- `Root`, `CurrentScene` (the scene a change set, else the first top-level node that is not an autoload), `Paused` (the G1 pause; a change sends `NotificationPaused` to nodes that stop processing and `NotificationUnpaused` to those that start, so a WhenPaused node hears "unpaused" when the tree pauses), `Quit`, `GetFrame`, `GetNodeCount`.
- Scene changes: `ChangeSceneToFile(path)` (engine: `Game.ChangeLevel` on the level the converter wrote, `ScenePathMapper`; without the engine: load the scene and swap it), `ChangeSceneToPacked`, `ReloadCurrentScene`, `UnloadCurrentScene`. The swap happens at the end of the frame, as Godot's.
- Signals (and C# events): `process_frame` and `physics_frame` before any node's `_Process` / `_PhysicsProcess`; `node_added`, `node_removed`, `node_renamed`, `tree_changed`.
- Groups: `GetNodesInGroup` (tree order), `GetFirstNodeInGroup`, `HasGroup`, `GetNodeCountInGroup`, `CallGroup`, `CallGroupFlags` (`GroupCallFlags.Reverse` / `Deferred` / `Unique`), `SetGroup(Flags)`, `NotifyGroup(Flags)`. A node without the method is skipped without an error.
- `CreateTimer(sec, processAlways = true, processInPhysics = false, ignoreTimeScale = false)` returns a `SceneTreeTimer`: counted after the nodes in the frame (or physics step), fires `timeout` when `TimeLeft <= 0`, in creation order; a paused tree stops it unless `processAlways`; `ignoreTimeScale` counts real seconds. `await ToSignal(GetTree().CreateTimer(1), SceneTreeTimer.SignalName.Timeout)` resumes inside the emit, as GodotSharp's does (the G1 game-thread context only matters for code that awaits a `Task`).
- Autoloads are ordinary top-level nodes with `Persistent`; `GetNode("/root/Global")` finds them. `Node.Persistent` is a managed field until the node has an entity and the engine's flag after.

### 5.2 Tween

Godot 4's API: `Node.CreateTween()` (bound to the node) and `SceneTree.CreateTween()`; `TweenProperty` (property paths with sub-properties, `position:x`, `modulate:a`), `TweenInterval`, `TweenCallback`, `TweenMethod`, `TweenSubtween`; `SetParallel` / `Parallel` / `Chain`, `SetLoops`, `SetTrans` / `SetEase` (tween-wide and per tweener, all 12 transitions times 4 eases), `SetSpeedScale`, `SetProcessMode`, `SetPauseMode` (`Bound` / `Stop` / `Process`), `SetIgnoreTimeScale`, `BindNode`; `PropertyTweener.From / FromCurrent / AsRelative / SetDelay / SetCustomInterpolator`; `Play / Pause / Stop / Kill / CustomStep / IsRunning / IsValid / GetLoopsLeft / GetTotalElapsedTime`; signals `finished`, `step_finished(idx)`, `loop_finished(n)`; `Tween.InterpolateValue`. The step algorithm is Godot's: the time a tweener does not use goes on into the next step, a zero-length step ends in the frame it starts, and an infinite tween whose loop takes no time runs one loop per frame. Interpolation is per component for ints (truncated), floats, Vector2/2I/3/3I/4, Color and Rect2; a Quaternion is a slerp of the eased fraction (approximated); a value with no in-between keeps its start until the end.

`Easing.cs` holds the curves, written from Penner's published equations (sine, quint, quart, quad, expo, elastic, cubic, circ, bounce, back) and Godot's spring; the tests compare 12 transitions at several times in all four eases against values worked out independently. The dead-time rule differs from Godot only where an elapsed time is exactly equal to a duration in floating point.

### 5.3 PackedScene, ResourceLoader, Resource

- A scene is the level file the converter wrote for it (`Maps/<path>.ocworld`). `OcWorldScene` is a small managed reader for the part of the format a converted scene uses (`PLACE`/`PLACEG`/`CHILD`/`CHILDG` with `BEGIN`/`END`, `class`, `name`, `COMPONENT Props`, `COMPONENT Groups`; everything else is skipped). `PackedScene.Instantiate()` builds a fresh detached tree: each node is made by class name (`ClassDB`, then any loaded assembly's `Node` subclass by its registry name), named, placed (the PLACE line is Aver space; `FabricSpace` brings it to Godot's local transform), given its groups and its Props as pending values (applied as it enters the tree, before `_EnterTree`, exactly as a level load does), and the root gets `SceneFilePath`. Placements with `_persistent=1` (autoloads) are not part of the scene. A class that is not known becomes a `Node3D` and a warning. A plain mesh placement becomes a `MeshInstance3D`. `Pack(node)` stores a `Duplicate()` of a tree (limits of `Node.Duplicate`: fields of script classes, `Node3D` transform and groups; Fabric properties other than the transform are not copied).
- In the engine an instance is added with `AddChild`, which spawns each node as an actor in a spawn batch (G1), so `_EnterTree` / `_Ready` order is Godot's. `ChangeSceneToPacked` of a scene that has a converted level travels to that level instead (the engine's level change keeps the autoloads).
- `ResourceLoader.Load<T>(path)`: `res://a/b.png` is looked up in the content folder as written, then under `Textures/` (where the converter puts textures); a scene (`.tscn`) is `Maps/<path>.ocworld`. Two loads of one path are one object (`CacheMode.Ignore` / `Replace` exist). A path with no file behind it still loads: a stand-in of the requested type (or the type its extension suggests: `.ocmesh` Mesh, `.ocmat` StandardMaterial3D, images Texture2D) that knows its path, because a level's `Resource("path")` value is resolved by the engine's own asset loaders; `ResourceLoader.Exists(path)` is the honest question, and `GD.Preload<T>` logs an error for a missing file. `LoadThreadedRequest / GetStatus / Get`: the load runs at the end of the current pass (status `InProgress` until then); `LoadThreadedGet` early loads at once. **GPU prefetch of meshes and textures is not done** (it is host loading code that needs a headed run; G1 moved it here and it moves on to the engine-run work). `Resource.Duplicate(subresources)` copies the resources it holds when asked; `ResourceLocalToScene` resources get their own copy (and `_SetupLocalToScene`) per node they are assigned to; `TakeOverPath`, `EmitChanged`, `Changed`.
- A dictionary with a `"__class"` key converts to an object of that class when a property of that type is assigned (the converter writes `physics_material_override` as `{"__class": "PhysicsMaterial", "friction": ..., ...}`).

### 5.4 Input

- `Input` (static, Godot's names): `IsActionPressed / JustPressed / JustReleased` (with `exactMatch`), `GetActionStrength / RawStrength`, `GetAxis`, `GetVector` (circular dead zone on the raw strengths, the average of the four actions' dead zones when none is given), `ActionPress / ActionRelease`, `IsKeyPressed / IsPhysicalKeyPressed / IsKeyLabelPressed`, `IsMouseButtonPressed`, `IsJoyButtonPressed`, `GetJoyAxis`, `GetMouseButtonMask`, `GetLastMouseVelocity`, `MouseMode` (Visible and Confined ask the engine for a free cursor), `GetConnectedJoypads`, `IsAnythingPressed`, `ParseInputEvent`.
- `InputMap`: `AddAction`, `EraseAction`, `HasAction`, `GetActions`, `ActionAddEvent / EraseEvent / EraseEvents / HasEvent / GetEvents`, `ActionSetDeadzone / GetDeadzone`, `EventIsAction`. In the engine the project's actions are the ones the converter wrote into the `.ocinput` (framework actions); their events come back from the bindings (key, mouse button, wheel, pad button, pad axis; the sign of an axis binding is not recoverable and reads positive). Actions added by script live in a managed layer beside them.
- Event classes with Godot's members: `InputEvent` (`IsPressed`, `IsReleased`, `IsEcho`, `IsAction`, `IsActionPressed(action, allowEcho, exactMatch)`, `IsActionReleased`, `GetActionStrength`, `IsMatch`, `AsText`), `InputEventKey`, `InputEventMouseButton`, `InputEventMouseMotion`, `InputEventJoypadButton`, `InputEventJoypadMotion`, `InputEventAction`; enums `Key` (Godot's codes), `MouseButton`, `JoyButton`, `JoyAxis`, `KeyModifierMask`. Matching follows Godot: a stored event's modifiers must all be held in the incoming one (equal with `exactMatch`); a joypad axis matches by axis and sign, pressed past the dead zone with strength `(|v| - dz) / (1 - dz)`.
- The engine's G1 events (`Aver.Framework.InputEvent`) are bridged by `InputBridge` into these (keys by the table of Aver's key set, the wheel as a wheel button event, a pad stick's Y inverted because XInput's is up and Godot's is down), and the same Godot event object serves every node so `GetViewport().SetInputAsHandled()` marks the engine's event. `Node._Input` / `_UnhandledInput` / `_UnhandledKeyInput` now take the Godot `InputEvent`. A pressed state is held until its release event arrives; "just pressed" lasts the frame it came in (or the physics step after it, inside a physics pass).
- Not done: touch and absolute mouse position (the engine reports only deltas: `GetMousePosition` is where script-fed events last put it), several pads, input while paused beyond the node's process mode, the SDL mapping database. In the engine `ParseInputEvent` reaches `_Input` through the framework for events it has a twin for (keys, mouse buttons, wheel, motion, pad buttons) and through the tree for the rest (actions, pad motion); the engine's polled action state does not change from a fed event, but Fabric's own queries do.

### 5.5 Camera3D, lights, mesh

`Camera3D`: `Current` / `MakeCurrent` / `ClearCurrent` (one current camera; the engine's view entity follows), `ProjectRayOrigin`, `ProjectRayNormal`, `ProjectPosition(screen, zDepth)`, `UnprojectPosition(world)`, `IsPositionBehind`, `IsPositionInFrustum`, `GetCameraTransform`, `GetCameraProjection`, perspective (`KeepAspect` Height or Width) and orthogonal. The viewport size is `Viewport.Size`: the window size of the project (`WINDOW.SIZE`) or 1280 by 720, settable. `MeshInstance3D.MaterialOverride` re-applies when it changes. The light calibration of F2a stays a placeholder.

### 5.6 Physics nodes (G2 underneath; Godot units at the script boundary)

- Conversions live in `FabricPhysics` and `CollisionObject3D`: lengths and forces times 100, directions through the axis map, **angular velocity, torque and angular impulse are pseudovectors** and take the sign the rotation map gives the quaternion's vector part, `(x, y, z) -> (z, -x, -y)` (`FabricSpace.ToAverAngular`); F2a's rigid-body wrappers used the polar map for them, which had the wrong sign, and torques were not scaled by 100 squared. Positions are placed by the shape's origin (a body's `Position` in the ABI is its centre of mass).
- `CollisionObject3D`: layer and mask change at runtime, `GetRid`, collision exceptions, `QueueRebuild` (a child shape added, changed or disabled makes the body again at the end of the pass; velocities carry over).
- `PhysicsBody3D`: `PhysicsMaterialOverride` (`PhysicsMaterial`: friction, bounce, rough, absorbent; Godot's own defaults apply when the world uses `PHYSICS.COMBINE godot`), axis locks (`AxisLockLinear*` / `Angular*`, enforced by zeroing the locked velocity components before each step: an approximation, the ABI has no axis locks).
- `StaticBody3D` constant velocities (conveyors), follows its node when script moves it; `AnimatableBody3D` with `SyncToPhysics` is driven through the step so it carries what rests on it.
- `RigidBody3D`: mass, gravity scale, damping, freeze and freeze mode, contact monitor, `Sleeping`, `LinearVelocity`, `AngularVelocity`, `ApplyCentralImpulse/Impulse/TorqueImpulse/CentralForce/Force/Torque`, `ConstantForce`, `ConstantTorque`, `AddConstantForce/CentralForce/Torque`, `GetContactCount`, `GetCollidingBodies`, `body_entered/exited`, `sleeping_state_changed`, `CustomIntegrator` and a virtual `_IntegrateForces(PhysicsDirectBodyState3D)` (the G2 callback; the state object has `Step`, `InverseMass`, `TotalGravity`, `LinearVelocity`, `AngularVelocity`, `Transform`, `GetContactCount`, `ApplyCentralImpulse/Force`, `ApplyImpulse/Force`, `ApplyTorqueImpulse/Torque`; the pose is read, not written back). A dynamic body that script teleports is not carried to the physics body (the entity follows the body, not the other way).
- `CharacterBody3D`: `Velocity`, `MoveAndSlide` (the node's pose goes to the body first, so script moves count), `MoveAndCollide` (on `Body.TestMotion`, with `testOnly`, margin, recovery as collision), `GetSlideCollision(i)` / `GetLastSlideCollision` as `KinematicCollision3D`, `IsOnFloor/Wall/Ceiling(+Only)`, normals, platform velocity, `GetRealVelocity`, floor options, `MotionMode`. Not done: `ApplyFloorSnap`, `PlatformFloorLayers` beyond the option, pushing dynamic bodies (the G2 controller teleports).
- `Area3D`: monitoring / monitorable at runtime, priority, gravity (direction or point, strength, unit distance) and damping space overrides with Godot's five modes, overlaps and the four signals. Not done: wind (Godot's needs a source node), per-shape signals.
- `RayCast3D`, `ShapeCast3D`: update every physics step on `Aver.Physics.RayCast3D` / `ShapeCast3D` with Godot's defaults (back faces on, from inside off, exclude parent), `ForceRaycastUpdate`, exceptions by node and rid, results as nodes, metres and Godot normals.
- `World3D.DirectSpaceState` (`GetWorld3D()`, `Viewport.World3D`): `IntersectRay(PhysicsRayQueryParameters3D)`, `IntersectShape`, `CastMotion`, `CollideShape`, `GetRestInfo` return Godot's dictionaries (`position`, `normal`, `collider`, `collider_id`, `rid`, `shape`, `face_index` = -1; shape results `collider`, `collider_id`, `rid`, `shape`; rest info `point`, `normal`, `rid`, `collider_id`, `shape`, `linear_velocity`) and are empty when there is no physics world. `PhysicsServer3D` (lite): `BodySetState/GetState` (transform, velocities, sleeping), `BodyApplyCentralImpulse/CentralForce/TorqueImpulse`, collision layer and mask, collision exceptions, `SpaceGetDirectState`; keyed by the Rid of `CollisionObject3D.GetRid()`.
- `VehicleBody3D` (mass 40 by default) and `VehicleWheel3D`: the body's `EngineForce` goes to wheels with `UseAsTraction`, `Steering` to `UseAsSteering` wheels, `Brake` to all; a wheel's own value overrides. Wheel geometry comes from the wheel node (`-Y` suspension direction, `X` axle, local position), the rest from its properties through `RaycastWheelSettings.FromGodot`; damping is taken as Godot's ratio (`DampingFromRatio`; **UNVERIFIED** against the target Godot version, as the G2 doc says). Wheels follow the last step's pose. `IsInContact`, `GetRpm`, `GetSkidinfo`, `GetContactBody`, `GetContactPoint`.

### 5.7 What ran, and what did not

Verified headless (managed loop, no engine): everything in 5.1 to 5.5, the managed halves of 5.6 (defaults, conversions, the direct-state object, vehicle force distribution, query results without a world, `PackedScene` against the converter's real output for `ConvertGame`). **Not verified, because the engine-run ban is in force and the managed `Aver.Physics` binding cannot be loaded beside the native DLL of the same name inside a test project**: the `SceneTreeDriver` path (frame signals, timers and tweens driven by the engine's tick), `MoveAndSlide` / `MoveAndCollide` on a real floor, body building from shapes, area overlap, ray and shape casts, the vehicle, `_IntegrateForces`, the instance-in-engine path of `AddChild` for an instanced scene, engine-backed `Input` and `InputMap` queries, `MouseMode`, and the local physics-material surface defaults.
