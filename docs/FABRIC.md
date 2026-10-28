# Fabric Connector: bring a Godot 4 project into Aver

The Fabric Connector converts a Godot 4.1+ project into a standalone Aver project, one way (Godot to Aver). It reads your scenes, assets and scripts, writes Aver levels, meshes, materials, input maps and C# scripts, copies your GDExtension plugins, and writes a report that says, item by item, what came across exactly, what came across approximately, and what did not come across at all.

It does not change your Godot project, and the converted project does not need Godot. Scripts run on `Aver.Fabric`, a runtime that has Godot's node API, signals, `Variant`, `SceneTree`, tick order and physics rules on top of Aver's own engine.

Scope: **3D and UI first.** 2D is out of scope for now; 2D nodes are reported as unsupported. Godot 4.1 and later only (a Godot 3 project is refused).

## Convert a project

### From the command line

```
AverAssetC fabric <godot-project-dir> <out-dir> [--dry-run] [--scenes-only] [--scripts-only] [--overwrite]
```

`AverAssetC` is in the folder of the editor (`bin/`). It writes:

```
<out-dir>/FabricReport.md          what converted, what did not (read this first)
<out-dir>/FabricReport.jsonl       the same, one JSON object per line
<out-dir>/<Name>/<Name>.ocproject  the Aver project (open this)
<out-dir>/<Name>/Content/...       levels, meshes, materials, textures, input, scripts, GDExtensions
<out-dir>/<Name>/Fabric/           scripts.json and project.json: the converter's own manifests
```

`<Name>` is the project's name (`application/config/name`) with everything but letters, digits, `_` and `-` removed.

| Option | What it does |
|---|---|
| (none) | The whole conversion: scenes and assets, then the scripts, then the GDExtension plugins, then the merged report. |
| `--dry-run` | Does all of that in a scratch folder and keeps only `FabricReport.md` and `.jsonl` in `<out-dir>`. Use it to see how a project fares before committing to the output. |
| `--scenes-only` | Scenes, assets, project settings, input, translations and GDExtension files. Scripts are not converted (the report says so). |
| `--scripts-only` | Converts only the scripts, into a project converted before (needs `<out-dir>/<Name>/<Name>.ocproject`). The scene part of the earlier report is kept. |
| `--overwrite` | Replaces a project already converted into `<out-dir>`. Without it the converter refuses, so a second run cannot silently overwrite hand edits. Files are replaced in place; nothing else in the folder is deleted. |

Exit code: **0** converted (the report may list approximated and unsupported items; that is not a failure), **1** the conversion failed (no `project.godot`, a Godot 3 project, a file could not be written, the project already exists without `--overwrite`, or the script converter could not run), **2** wrong arguments. The last line on stdout is a JSON summary (`"kind":"fabric"`, with the output folder, the counts, how many scripts and GDExtensions were found, and the report path).

Scripts are converted by `Aver.Fabric.Convert`, a .NET tool in `bin/Tools`. It needs the **.NET SDK** (`dotnet` on `PATH`, or `DOTNET_ROOT`) and the managed runtime in `bin/Scripting`. A project with no scripts needs neither.

### From the editor

**File > Import Godot Project...** opens a dialog:

1. **Godot project**: browse to the project's `project.godot` (or type the folder).
2. **Output folder**: filled in as `<project>_Aver` beside the Godot folder; change it if you like.
3. Optional: *Replace a project already converted there*, and *Report only*.
4. **Import**. The converter runs as a separate process, so the editor stays usable. The log scrolls in the dialog and goes to the Output Log; a notification tracks progress and ends with the counts. Closing the dialog does not stop the import.
5. When it ends, **Open converted project** switches the editor to it (save your open level first). The report path is shown and can be copied.

The menu item is greyed out when `AverAssetC` is not beside the editor (build the `AverAssetC` target).

## Run the converted project

1. Open `<out-dir>/<Name>/<Name>.ocproject` in the editor (the dialog's *Open converted project*, or the project browser).
2. **Tools > Compile Scripts** builds `Content/Scripts/Scripts.csproj` (the converter writes it when it finds the engine checkout; otherwise the editor creates one with the first script you make). The scripts compile against `Aver.Fabric`.
3. Open the start level (`Maps/<your main scene>.ocworld`) and press Play, or **File > Launch in Aver Engine Runtime**.

Converted scripts are ordinary C# in `Content/Scripts`. Where a script uses something the runtime does not have yet, the converter leaves a `// FABRIC:` comment on that line and reports it; the script is still written, so you can fix the line by hand and compile.

## Units and axes

Godot works in metres, +Y up, right-handed. Aver works in centimetres, +Z up, left-handed. The converter uses exactly the map Aver's glTF importer uses (`aver/formats/AxisMap.hpp`), so a model exported to glTF and a node converted from Godot land in the same place:

| | Godot to Aver |
|---|---|
| position | `(x, y, z)` metres to `(-z, x, y) * 100` centimetres |
| rotation (quaternion) | `(x, y, z, w)` to `(z, -x, -y, w)` |
| scale | `(x, y, z)` to `(z, x, y)` |
| length (radius, range, size) | times 100 |
| triangle winding | reversed (the axis map mirrors) |

Scripts do not see any of this. `Aver.Fabric` keeps Godot's numbers at the script boundary: `Position` is in metres, `Vector3` is Godot's, `Basis` and `Quaternion` follow Godot's conventions, and the runtime converts when it talks to the engine. A converted `position.y += 1.0` still lifts the node one metre.

Gravity (`physics/3d/default_gravity` times its vector) becomes `PHYSICS.GRAVITY` through the same map, so 9.8 down is `0 0 -980`.

## What converts

The three ratings in the report:

- **converted**: it behaves as it does in Godot.
- **approximated**: it is there and works, but differs; the report says how.
- **unsupported**: it does not work in Aver yet. Where possible the data is kept (as node properties or metadata) so nothing is lost for later.

### Project, input, translations

| Godot | Result |
|---|---|
| name, main scene, window size, resizable, fullscreen, physics tick rate, interpolation, gravity | converted into the `.ocproject` (including `PHYSICS.COMBINE godot`, which turns on Godot's friction and bounce combine rules) |
| `[autoload]` with `*` | converted: persistent placements, first in the start level, in `project.godot` order. An autoload without `*` is not added to the tree, as in Godot, and is reported |
| input map: keys A-Z, 0-9, Space, Shift, Ctrl, Alt, Enter, Escape, Tab, arrows; mouse buttons; wheel; gamepad buttons and axes | converted into an `.ocinput`; modifiers and chords, the wheel and the sign of axes are approximated |
| other keys (F-keys, Backspace ...), touch, mouse motion, shortcuts | unsupported, listed by action |
| `.po` translations | converted (copied under their culture); `.translation` and `.csv` are unsupported |
| every other project setting | unsupported, listed by key |

### Scenes and nodes

Every `.tscn` becomes a level (`Maps/<path>.ocworld`) with one class placement per node: the node's Godot class, or the class of its script, plus its properties, groups, signal connections and owner. Instanced scenes are flattened into the level with their overrides applied, and each is also written as a prefab.

| Converted | Notes |
|---|---|
| `Node`, `Node3D`, `Marker3D`, `MeshInstance3D`, `Camera3D`, `DirectionalLight3D`, `OmniLight3D`, `SpotLight3D`, `Timer` | with Godot's defaults and signals |
| `CollisionShape3D` and the bodies `StaticBody3D`, `AnimatableBody3D`, `RigidBody3D`, `CharacterBody3D`, `Area3D` | Godot's layer, mask, mass, damping, gravity scale and contact monitor are written out explicitly; shapes: box, sphere, capsule, cylinder, convex, concave, height map (to 64 x 64), world boundary, and `CollisionPolygon3D` extruded (approximated) |
| `RayCast3D`, `ShapeCast3D`, `VehicleBody3D`, `VehicleWheel3D` | placed as their own classes; the runtime casts on every physics step and spreads the vehicle's engine force, brake and steering over its wheels as Godot does |
| primitive meshes (`BoxMesh`, `SphereMesh`, `CapsuleMesh`, `CylinderMesh`, `PlaneMesh`, `QuadMesh`, `PrismMesh`, `TorusMesh`) | generated as `.ocmesh` with Godot's defaults |
| `.glb`, `.gltf`, `.fbx`, `.obj` models | imported with Aver's importers; a `mesh` property that names a model takes its first mesh |
| `StandardMaterial3D`, `ORMMaterial3D` | cooked to `.ocmat`: albedo, metallic, roughness, normal, AO, emission, transparency, culling |
| `WorldEnvironment` | becomes the level's `SUN`, `SKY` and `FOG` records and the project's bloom and exposure |
| `Control` family, `Viewport`, `Window` | placeholders that hold their layout values until the UI package fills them (approximated) |

Not yet working (placed as a plain `Node` or `Node3D` that keeps the node's properties as metadata, so the tree and any script survive, and reported unsupported): joints, `SoftBody3D`, `NavigationAgent3D` and navigation regions, `AnimationPlayer` / `AnimationTree`, audio players and buses, `GPUParticles3D`, `Path3D` / `PathFollow3D`, `SpringArm3D`, `Skeleton3D`, `Sprite3D` / `Label3D`, CSG, `GridMap`, `MultiMeshInstance3D`, reflection probes, `LightmapGI` / `VoxelGI`. ArrayMesh data is not decoded ("re-export as .glb"). Binary `.scn` / `.res` files are not read ("resave as text"): in Godot, convert them to `.tscn` / `.tres`.

Several material settings have no Aver equivalent yet and are reported by name: UV scale and offset, front-face culling, rim, clearcoat, anisotropy, parallax, billboard, triplanar, distance fade. Environment settings with no record (SSAO, SSR, SDFGI, DOF ...) are reported by name.

### Behaviour: what the runtime does like Godot

Converted scripts run on `Aver.Fabric`, which follows Godot's rules for: process and physics-process order and priorities, pause modes, `_enter_tree` / `_ready` / `_exit_tree` order, signals and their connect flags, deferred calls and `await`, `Variant` and its operators, `SceneTree` (groups, timers, scene changes), `Tween`, `Input` and `InputMap`, `PackedScene.Instantiate`, `ResourceLoader`, `CharacterBody3D.MoveAndSlide`, area signals and overrides, raycasts, shape casts, `PhysicsDirectSpaceState3D` queries, contact monitoring, `_integrate_forces`, and the raycast vehicle. Physics runs on Aver's Jolt with Godot's combine rules, 32-bit layers and masks, and Godot's defaults, so it matches Godot's own Jolt module within tolerance. It does not match GodotPhysics3D bit for bit.

### Scripts

| Source | Result |
|---|---|
| GodotSharp C# | rewritten (`Godot` namespaces to `Aver.Fabric`, attributes kept, generated `SignalName` / `MethodName` / `PropertyName` tables and signal events) and compiled in memory |
| GDScript 2.0 | parsed, typed, and written as readable C# (typed members, exports, `@onready`, signals, setters, lambdas, `match`, `await`, string formatting, built-in functions) and compiled in memory |
| Godot 3 GDScript | not supported |
| `[Tool]` / `@tool` scripts | kept, reported unsupported (there are no editor scripts) |
| `[Rpc]` / `@rpc` | recorded, approximated (nothing is networked yet) |
| NuGet packages of the Godot project | unsupported, listed per package |

Every script is compiled together against `Aver.Fabric`. A script that compiles with nothing dropped is **converted**; one that compiles but used a fallback (reading a member by name at run time, for example) is **approximated**; each compile error becomes an **unsupported** entry that names the exact missing member, and a `// FABRIC:` comment on that line. Semantic differences that still compile are not detected; read the generated C# of anything important.

### GDExtension plugins

Each `.gdextension` file and the libraries it names are copied into `Content/` at the same `res://` path, and declared in `Fabric/project.json`. Nodes of a class the plugin registers (named in the file's `[icons]` section) are placed as `GDExtensionNode` (`GDExtensionNode3D` when the scene gives them a 3D transform) with the property `extension_class`; their properties are handed to the native class. The plugins are loaded from `Content/` the first time such a node needs its class.

The host (`modules/fabric.gdext`) serves the interface subset godot-cpp uses and a small engine class registry (`Object`, `RefCounted`, `Resource`, `Node`, `Node3D`). It was written against Godot's published interface and tested with a plugin written in C; **it has not been run against a real godot-cpp plugin**, so the file is rated approximated. A plugin whose classes need `Control`, 2D, rendering, physics or `SceneTree` classes will report what it asked for that the host cannot provide. A library for the operating system you run on must be in the project (Godot projects often ship only some platforms); a missing desktop library is reported.

## Reading FabricReport

`FabricReport.md` opens with the result line (converted / approximated / unsupported), a table with one row per kind of item, and **Top unsupported features**: the ten things that cost you most, by how many items each affects. Below that, Unsupported and Approximated are listed by file, each with its reason and the line in the Godot file; Converted items are counted per file. Kinds: `project`, `setting`, `file`, `scene`, `node`, `resource`, `property`, `input`, `script`.

A good order: read the result line, then Top unsupported features, then the unsupported items of your main scene and your scripts. `FabricReport.jsonl` holds the same data for tools: each line has `kind`, `file` (the `res://` path), `node`, `type`, `property`, `rating`, `reason` and `line`. For scripts, the line number is a line of the converted C# file.

## Known limits

- **2D is not supported.** 2D nodes are kept as plain nodes and reported.
- **Rendering is mapped, not identical.** Light energies, sky, fog, tonemapper and exposure are carried across as parameters, but Aver renders with its own physically based lighting and global illumination, so the picture differs from Godot's renderer. Light calibration is a placeholder until the rendering work.
- **Shaders: pending.** `.gdshader` files and `ShaderMaterial` are reported unsupported until the shader-language front end (G6).
- **Audio, animation players, navigation, particles, joints and most UI controls** are not in the runtime yet (see above); their nodes are kept as carriers.
- **GDExtension coverage** is limited to what the host implements (above) and is untested against real plugins.
- **Networking** (`MultiplayerAPI`, `@rpc`, spawners) is not implemented.
- The converter never ran inside the engine's own test environment; the converted levels are checked by loading them with Aver's loaders and the scripts by compiling them, not by playing the game. Expect to find gaps when you first play a converted project, and report them with the project's `FabricReport.md`.
- Godot's own tick-for-tick physics (GodotPhysics3D) is not reproduced.

## Where the details live

- Converter contracts: `docs/plans/fabric/G3_FORMATS.md` (text-format reader), `G3_SCENES.md` (scenes, project, report), `G3_RUNTIME.md` (the node contract of `Aver.Fabric`), `G3_SCRIPTS.md` (GodotSharp and GDScript), `G3_GDEXT.md` (GDExtension host).
- Plan and status tables: `docs/plans/fabric/FABRIC_CONNECTOR.md`.
- Godot physics semantics on Aver: `docs/physics/GODOT_FILTERING.md`, `GODOT_QUERIES.md`, `GODOT_KINEMATIC.md`, `GODOT_AREAS.md`, `GODOT_VEHICLE.md`.
- Runtime API list: `docs/api/Aver.Fabric.md`.
- The axis map: `docs/ASSET_IMPORT.md`.
