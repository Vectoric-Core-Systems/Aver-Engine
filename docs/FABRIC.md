# Fabric Connector: bring a Godot 4 project into Aver

The Fabric Connector converts a Godot 4.1+ project into a standalone Aver project, one way (Godot to Aver). It reads your scenes, assets and scripts, writes Aver levels, meshes, materials, input maps and C# scripts, copies your GDExtension plugins, and writes a report that says, item by item, what came across exactly, what came across approximately, and what did not come across at all.

It does not change your Godot project, and the converted project does not need Godot. Scripts run on `Aver.Fabric`, a runtime that has Godot's node API, signals, `Variant`, `SceneTree`, tick order and physics rules on top of Aver's own engine.

Scope: **3D and UI first.** 2D is out of scope for now; 2D nodes are reported as unsupported. Godot 4.1 and later only (a Godot 3 project is refused).

## Convert a project

### From the command line

```
AverAssetC fabric <godot-project-dir> <out-dir> [--dry-run] [--scenes-only] [--scripts-only] [--overwrite]
```

`AverAssetC` is in the folder of the editor (`bin/`). `fabric` is not one of its own commands: it belongs to the Fabric Connector, an engine extension installed in `Extensions/FabricConnector/` beside it (`AverAssetC` hands the command to the extension loader, [EXTENSIONS.md](EXTENSIONS.md); `AverAssetC --help` lists it). Without that folder `fabric` is an unknown command. The extension's sources are in `extensions/FabricConnector/` and the CMake option `AVER_WITH_FABRIC` (default ON) builds it. It writes:

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

Scripts are converted by `Aver.Fabric.Convert`, a .NET tool in the extension's `tools` folder (`bin/Extensions/FabricConnector/tools`). It needs the **.NET SDK** (`dotnet` on `PATH`, or `DOTNET_ROOT`), the extension's managed runtime (`Extensions/FabricConnector/managed/Aver.Fabric.dll`) and the engine's assemblies in `bin/Scripting`. A project with no scripts needs none of them. The converted project's `Scripts.csproj` references `Aver.Fabric.dll` by a `HintPath` to the installed extension, and its `.ocproject` gains `EXTENSION FabricConnector <version>`: opening the project on an engine without the extension logs that name.

### From the editor

**File > Import Godot Project...** is a menu item the Fabric Connector adds through its `extension.json` (`importers`): the editor's import dialog is generic and draws whatever the installed extensions list, so the item exists only while the extension is installed ([EXTENSIONS.md](EXTENSIONS.md), "The editor"). The dialog:

1. **Godot project**: browse to the project's `project.godot` (or type the folder).
2. **Output folder**: filled in as `<project>_Aver` beside the Godot folder; change it if you like.
3. Optional: *Replace a project already converted there*, and *Report only*.
4. **Import**. The converter runs as a separate process, so the editor stays usable. The log scrolls in the dialog and goes to the Output Log; a notification tracks progress and ends with the counts. Closing the dialog does not stop the import.
5. When it ends, **Open converted project** switches the editor to it (save your open level first). The report path is shown and can be copied.

The menu item is greyed out when `AverAssetC` is not beside the editor (build the `AverAssetC` target). The dialog runs `AverAssetC fabric <godot-dir> <out-dir> [--overwrite] [--dry-run]` and reads the summary line the command ends with; no extension code runs inside the editor. The dialog itself has not been run (engine-run ban); its logic is covered headless by `ExtensionImportTest` (the generic dialog logic against fake manifests and a fake tool) and `FabricImportTest` (the Fabric Connector's manifest through the loader and the real `AverAssetC`).

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
| name, main scene, window size, resizable, fullscreen, physics tick rate, interpolation, gravity | converted into the `.ocproject` (including `PHYSICS.COMBINE material`, which turns on Godot's friction and bounce combine rules) |
| `[autoload]` with `*` | converted: persistent placements, first in the start level, in `project.godot` order. An autoload without `*` is not added to the tree, as in Godot, and is reported |
| input map: keys A-Z, 0-9, Space, Shift, Ctrl, Alt, Enter, Escape, Tab, arrows; mouse buttons; wheel; gamepad buttons and axes | converted into an `.ocinput`; modifiers and chords, the wheel and the sign of axes are approximated. An old project's Godot 3 key codes and joypad numbers are read as the Godot 4 inputs they meant (approximated, reported); an empty key event binds nothing |
| other keys (F-keys, Backspace, PageUp ...), Godot's Guide and extra gamepad buttons, touch, mouse motion, shortcuts | unsupported, listed by action with the key or button named |
| editor and import-time settings (`application/config/tags`, script warnings, texture compression formats) | approximated: they change nothing a running game does, so nothing is written |
| `.po` translations | converted (copied under their culture); `.translation` and `.csv` are unsupported |
| every other project setting | unsupported, listed by key |

### Scenes and nodes

Binary Godot files (`.scn` scenes and `.res` resources, including the compressed form Godot writes when "compress" is on) are read like their text twins: a binary `ArrayMesh`, shape or `MeshLibrary` converts as its `.tres` would, and a `.scn` is a level of its own and can be instanced like a `.tscn`. Each one is a "file" row in the report. Godot 4 binaries only (engine version 4.x); a Godot 3 binary, or a damaged one, is reported unsupported and the rest of the project still converts.

Every `.tscn` (and every `.scn`) becomes a level (`Maps/<path>.ocworld`) with one class placement per node: the node's Godot class, or the class of its script, plus its properties, groups, signal connections and owner. Instanced scenes are flattened into the level with their overrides applied, and each is also written as a prefab.

| Converted | Notes |
|---|---|
| `Node`, `Node3D`, `Marker3D`, `MeshInstance3D`, `Camera3D`, `DirectionalLight3D`, `OmniLight3D`, `SpotLight3D`, `Timer` | with Godot's defaults and signals |
| `CollisionShape3D` and the bodies `StaticBody3D`, `AnimatableBody3D`, `RigidBody3D`, `CharacterBody3D`, `Area3D` | Godot's layer, mask, mass, damping, gravity scale and contact monitor are written out explicitly; shapes: box, sphere, capsule, cylinder, convex, concave, height map (to 64 x 64), world boundary, and `CollisionPolygon3D` extruded (approximated) |
| `RayCast3D`, `ShapeCast3D`, `VehicleBody3D`, `VehicleWheel3D` | placed as their own classes; the runtime casts on every physics step and spreads the vehicle's engine force, brake and steering over its wheels as Godot does |
| primitive meshes (`BoxMesh`, `SphereMesh`, `CapsuleMesh`, `CylinderMesh`, `PlaneMesh`, `QuadMesh`, `PrismMesh`, `TorusMesh`) | generated as `.ocmesh` with Godot's defaults |
| `.glb`, `.gltf`, `.fbx`, `.obj` models | imported with Aver's importers; a `mesh` property that names a model takes its first mesh. An instanced glTF gets the node tree Godot's importer makes (the nodes of the file, a `Skeleton3D` with its bones, `BoneAttachment3D`s, an `AnimationPlayer`), so children added to nodes inside the model, and overrides of them, land where the scene says (the `Skeleton3D` is a pose model that skins nothing, approximated) |
| `AnimationPlayer` `libraries/<name>` properties; a `Curve`'s `_limits` | merged into the one `libraries` dictionary; written as the curve's value range and domain |
| `StandardMaterial3D`, `ORMMaterial3D` | cooked to `.ocmat`: albedo, metallic, roughness, normal, AO, emission, transparency, culling |
| `WorldEnvironment` | placed as a node holding its `Environment` / `CameraAttributes`, and becomes the level's `SKY` and `FOG` records, a level-wide post volume (Godot's tonemapper, exposure, white point, glow, display adjustments, depth of field) and the project's `RENDER.TONEMAP`, exposure, bloom and eye-adaptation keys; an HDRI panorama sky is fitted onto Aver's sky dome (approximated) |
| `Control`, `Container`, `BoxContainer`, `HBoxContainer`, `VBoxContainer`, `GridContainer`, `MarginContainer`, `CenterContainer`, `PanelContainer`, `Panel`, `ScrollContainer`, `Theme`, `StyleBoxFlat` / `Texture` / `Line` / `Empty` | placed as their own classes (G5 U1): anchors, offsets, size flags, containers, theme lookup with variations and overrides, style boxes, `_gui_input`, `mouse_filter`, focus, tooltips, drag and drop; the project's `display/window/stretch/*`, `gui/theme/custom` and `gui/timers/tooltip_delay_sec` become the `UI.*` keys; a `Theme` becomes `Content/Themes/<name>.octheme`. A 9-patch is not drawn yet; scroll bars are drawn and draggable (`plans/fabric/G5_PARITY.md` U1, U2) |
| `Label`, `RichTextLabel` (BBCode), `Button`, `CheckBox`, `CheckButton`, `LinkButton`, `TextureButton`, `OptionButton`, `MenuButton`, `ColorPickerButton`, `HSlider`, `VSlider`, `HScrollBar`, `VScrollBar`, `ProgressBar`, `TextureProgressBar`, `SpinBox`, `LineEdit`, `TextEdit`, `ItemList`, `Tree`, `TabBar`, `TabContainer`, `HSplitContainer`, `VSplitContainer`, `HFlowContainer`, `VFlowContainer`, `AspectRatioContainer`, `NinePatchRect`, `TextureRect`, `ColorRect`, `HSeparator`, `VSeparator`, `ColorPicker`, `Popup`, `PopupPanel`, `PopupMenu`, `AcceptDialog`, `ConfirmationDialog`; `ButtonGroup`, `Shortcut`, `LabelSettings`, `FontFile`, `SystemFont`, `FontVariation` | placed as their own classes with Godot's members, signals and signal order (G5 U2): every property is rated; fonts become families of `Content/UI/Fonts.ocfonts`; outline, shadow, `RichTextEffect`s and BBCode are supported, MSDF, OpenType substitutions and variable-font axes are not; popups are top-level Controls, not Windows (`plans/fabric/G5_PARITY.md` U2) |
| `CanvasLayer` | placed as its own class (approximated): `visible` hides everything drawn on it (nested layers too); `layer`, `offset`, `rotation`, `scale` and `follow_viewport_*` are kept but not applied, as the GUI draws every Control in tree order in viewport space. A touch screen UI hung from one converts and compiles; Aver's input chain has no touch source, so `InputEventScreenTouch` / `InputEventScreenDrag` arrive only from `Input.parse_input_event` and `DisplayServer.is_touchscreen_available()` is false |
| every other `Control` class (`CodeEdit`, `MenuBar`, `FileDialog`, `GraphEdit`, `VideoStreamPlayer`...) | a `Control` carrying the class name and the node's properties (approximated) |

Not yet working (placed as a plain `Node` or `Node3D` that keeps the node's properties as metadata, so the tree and any script survive, and reported unsupported): joints, `SoftBody3D`, `VideoStreamPlayer` (the engine has no video decoder), `GPUParticles3D` with a `ParticleProcessMaterial` (one with a particles `ShaderMaterial` works, see Shaders below), and the skeleton modifiers other than `SkeletonIK3D` (`LookAtModifier3D`, `SpringBoneSimulator3D` and the rest). Animation (G5, `plans/fabric/G5_PARITY.md`): `AnimationPlayer` plays property, transform, method, audio and sub-player tracks on any node; `AnimationTree` runs state machines (travel, switch modes, conditions and expressions, cross-fades, nested machines), blend trees (Blend2/3, Add2/3, Sub2, OneShot, TimeScale, TimeSeek, Transition), 1D and 2D blend spaces and root motion; `Skeleton3D` / `BoneAttachment3D` / `SkeletonIK3D` are a script-level pose model (bone tracks write it; it is not connected to a skinned mesh, so posing it moves no vertices); `Curve`, `Curve3D`, `Gradient`, `Path3D`, `PathFollow3D`, `SpringArm3D`, `RemoteTransform3D` and the `VisibleOnScreen` notifier and enabler work. Environment volumes (G4 R8, `plans/fabric/G4_RENDER.md`): `FogVolume` with `FogMaterial` draws inside Voxi's volumetric fog (the converter turns it on, `RENDER.VOLFOG 1`, when the start scene's environment enables volumetric fog; 16 volumes per frame, nearest first; no density texture; a fog `ShaderMaterial` works, see Shaders below); `ReflectionProbe` is placed and queryable from scripts but the renderer samples no captured environment, so surfaces keep Aver's own reflections; `LightmapGI`, `LightmapProbe` and `VoxelGI` are kept and change nothing (Aver has no lightmap baker and its global illumination is real time; a `.lmbake` file is not read); the Environment's SSAO, SSR, SSIL and SDFGI flags are rated approximated by Aver's GI. Geometry nodes (G4 R7, `plans/fabric/G4_RENDER.md`): a stored `ArrayMesh` is decoded from Godot's packed format, `MultiMeshInstance3D` draws its instance table as one baked mesh, CSG trees and `GridMap` cells are baked at convert time (into a mesh and into instanced item nodes plus a static body), `Sprite3D` becomes a baked quad with a material; `Label3D` is placed with its properties and draws nothing, `TextMesh` and `CSGPolygon3D` path extrusion are not converted. Scripts can build meshes at run time with `ArrayMesh`, `SurfaceTool`, `ImmediateMesh` and `MeshDataTool`. Audio (G5 S1, `plans/fabric/G5_PARITY.md`, `AUDIO.md` section 10): `AudioStreamPlayer`, `AudioStreamPlayer3D` (Godot's four attenuation models, `unit_size`, `max_db`, `max_distance`, panning strength, emission cone, attenuation filter, Doppler) and `AudioListener3D` play through a bus graph with Godot's `AudioServer` API; a `default_bus_layout.tres` is converted (buses, sends, volume, mute, solo, bypass, effects and their parameters, a compressor's sidechain) and the project starts with it (`AUDIO.BUSLAYOUT`); the effects are Amplify, Delay, Chorus, Compressor, Limiter, HardLimiter, EQ6/10/21, the six filters, Distortion, Phaser, PitchShift, Reverb, Panner, StereoEnhance, Capture and SpectrumAnalyzer (several are approximations of Godot's algorithm; the report says which); `AudioStreamWAV` (with loop points), `AudioStreamRandomizer`, `Polyphonic`, `Playlist` (no cross-fade), `Synchronized`, `Interactive` (clips switch at once) and `Generator` work. Sound files are copied under `Content/Audio/`. **`.ogg` and `.mp3` files play on every platform** (the engine decodes Ogg Vorbis, MP3 and FLAC itself, `AUDIO.md` section 12; an Ogg's `LOOPSTART` / `LOOPLENGTH` comments are the loop of a looping stream; Ogg Opus is not decoded). Not converted: `AudioEffectRecord`, `AudioEffectBandLimitFilter`, area reverb buses, surround output. Navigation (G5 N1, `plans/fabric/G5_PARITY.md`, `SYNAPSE.md` section 8): `NavigationRegion3D` with its `NavigationMesh` (a mesh baked in the editor arrives with its polygons; one that was never baked is baked by the region when it is ready, from the box / concave collision shapes and `BoxMesh` / `ArrayMesh` meshes under it), `NavigationLink3D`, `NavigationObstacle3D` (avoidance, and carving a hole when a mesh is baked at run time) and `NavigationAgent3D` (path following with Godot's signals, avoidance with `velocity_computed`) work on a navigation map made of polygons on any number of floors, with 32-bit layers and enter / travel costs; scripts can use `NavigationServer3D`, `AStar3D`, `AStar2D` and `AStarGrid2D`. The agent moves nothing: your script reads `get_next_path_position()` as in Godot. Avoidance works in the ground plane. Baking is Aver's heightfield baker, not Recast, so polygons differ from Godot's; `sample_partition_type`, `region_merge_size`, `edge_max_error` and `detail_sample_distance` are stored and reported approximated. Binary `.scn` / `.res` files are not read ("resave as text"): in Godot, convert them to `.tscn` / `.tres`.

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
| `[Rpc]` / `@rpc` | converted (mode, call-local, transfer mode and channel kept; dispatched by `SceneMultiplayer`, G6 NET2) |
| NuGet packages of the Godot project | unsupported, listed per package |

A GDScript is typed from the scenes it is attached to where it can be: `$Path`, `get_node("path")`, `%Unique` and `instantiate()` of a scene it knows become typed members (`GetNode<AnimationPlayer>("Anim").Play("run")`) instead of calls by name, from the node classes the scene converter placed (`Fabric/nodes.json`). A node the scene converter dropped, or one inside an imported model, is not typed that way (a `$Path as Node3D` in the script still types it). An untyped parameter that the project always calls with one math type (a `Vector3`) is that type. A member that GDScript finds at run time on a variable declared as a plain node class (`var town: Node3D` ... `town.mood`, `town.setup(...)`) is reached through the one script of the project that has it (`((Town)town).mood`); where two unrelated scripts have the name it is not guessed and the member is reported missing. See `plans/fabric/G3_SCRIPTS.md` "Typed node access".

Every script is compiled together against `Aver.Fabric`. A script that compiles with nothing dropped is **converted**; one that compiles but used a fallback (reading a member by name at run time, for example) is **approximated**; each compile error becomes an **unsupported** entry that names the exact missing member, and a `// FABRIC:` comment on that line. Semantic differences that still compile are not detected; read the generated C# of anything important.

### GDExtension plugins

Each `.gdextension` file and the libraries it names are copied into `Content/` at the same `res://` path, and declared in `Fabric/project.json`. Nodes of a class the plugin registers (named in the file's `[icons]` section) are placed as `GDExtensionNode` (`GDExtensionNode3D` when the scene gives them a 3D transform) with the property `extension_class`; their properties are handed to the native class. The plugins are loaded from `Content/` the first time such a node needs its class.

The host (`extensions/FabricConnector/native/gdext`, exported by `Aver.Fabric.Native`) serves the interface subset godot-cpp uses and a small engine class registry (`Object`, `RefCounted`, `Resource`, `Node`, `Node3D`). It was written against Godot's published interface and tested with a plugin written in C; **it has not been run against a real godot-cpp plugin**, so the file is rated approximated. A plugin whose classes need `Control`, 2D, rendering, physics or `SceneTree` classes will report what it asked for that the host cannot provide. A library for the operating system you run on must be in the project (Godot projects often ship only some platforms); a missing desktop library is reported.

## Reading FabricReport

`FabricReport.md` opens with the result line (converted / approximated / unsupported), a table with one row per kind of item, and **Top unsupported features**: the ten things that cost you most, by how many items each affects. Below that, Unsupported and Approximated are listed by file, each with its reason and the line in the Godot file; Converted items are counted per file. Kinds: `project`, `setting`, `file`, `scene`, `node`, `resource`, `property`, `input`, `script`.

A good order: read the result line, then Top unsupported features, then the unsupported items of your main scene and your scripts. `FabricReport.jsonl` holds the same data for tools: each line has `kind`, `file` (the `res://` path), `node`, `type`, `property`, `rating`, `reason` and `line`. For scripts, the line number is a line of the converted C# file.

## Known limits

- **2D is not supported.** 2D nodes are kept as plain nodes and reported.
- **SubViewports are cheap views.** `SubViewport`, `ViewportTexture` and `SubViewportContainer` convert: a SubViewport draws the world from its camera into a texture that a material (the converter bakes a `ViewportTexture` into the `.ocmat`), a container on screen, or a script shows. A view is drawn by a separate, cheaper renderer than the main picture (one sun, flat material colours, no shadows, global illumination or post effects), and the project budgets them with `RENDER.SUBVIEWPORTS` (default 4), `RENDER.SUBVIEWRENDERS` (2 a frame) and `RENDER.SUBVIEWPIXELS` (8 MP). See `docs/plans/fabric/G4_RENDER.md` R5.
- **Material features are variants.** `StandardMaterial3D` / `ORMMaterial3D` convert with their UV transforms, triplanar mapping, cull / blend / depth modes, render priority, alpha hash, billboard / fixed size / grow, distance fade, rim, backlight, parallax, detail, unshaded and the rest; the converter writes them into the `.ocmat` `FX` record (clearcoat, subsurface, refraction and `metallic_specular` use the existing material fields) and rates every property. A material with features draws through its own pipeline variant (built the first frame it is used; `RENDER.MATERIALFX 0` turns them off); **every converted material now culls back faces**, Godot's default. Ray-driven mode and ray-traced hits do not see them. See `docs/plans/fabric/G4_RENDER.md` R6.
- **Rendering is mapped, not identical.** Light energies, sky, fog, tonemapper and exposure are carried across as parameters, but Aver renders with its own physically based lighting and global illumination, so the picture differs from Godot's renderer. Light calibration is a placeholder until the rendering work.
- **Shaders (G6, `plans/fabric/G6_SHADERS.md`).** `.gdshader` files, `#include`s and `VisualShader` resources run: a spatial `ShaderMaterial` becomes a material graph (SH2; `light()` is not run, screen / depth textures read a default colour, ray-driven mode runs `fragment()` only), a **sky** shader replaces the sky (the background, reflections and ambient light; ambient is five samples per pixel, so keep the sky cheap), a **fog** shader on a `FogVolume` supplies its density, albedo and emission, and a **particles** shader on a `GPUParticles3D` is the GPU simulation's `start()` / `process()` (`emit_subparticle`, collision, attractors and the texture uniforms of these three shader types are not supported). A `VisualShader` is converted to shading-language text for the common node set; an unknown node is reported per node and reads as unconnected. Nothing has been seen on a GPU: the first run needs the headed checks in `G6_SHADERS.md`.
- **Runtime calls that adapt a game to its renderer answer for Aver and change nothing.** `RenderingServer.get_current_rendering_method()` is `"forward_plus"` (so a `gl_compatibility` workaround is skipped), `directional_soft_shadow_filter_set_quality` / `positional_soft_shadow_filter_set_quality` and `DirectionalLight3D.sky_mode` are kept as set and not applied, `reset_physics_interpolation()` does nothing. `Input.vibrate_handheld()` does nothing (the first call says so in the log) and `Light3D.shadow_reverse_cull_face` is kept and not applied (Aver's shadows have no reverse-cull mode). `RenderingServer.frame_post_draw` comes at the head of the process frame after the one that was drawn (a connection made since the last frame ended waits one frame more), so `await RenderingServer.frame_post_draw` after showing a loading screen lets that screen be drawn first. `PhysicsDirectBodyState3D`'s per-contact reads (`get_contact_collider_object`, `_id`, `_rid`, `_local_normal`, `_local_position`, `_collider_position`, `_local_velocity_at_position`, `_collider_velocity_at_position`) come from the engine's tracked contacts (the partner, where the touch began, the normal): both positions are that one point, and the shape indices and `get_contact_impulse` are not offered. They need `contact_monitor` and `max_contacts_reported`, as in Godot. The contact reads were not run against the physics engine (the headless tests cannot load it beside the managed binding).
- **Particles (other than a particles-shader emitter), joints and most UI controls** are not in the runtime yet (see above); their nodes are kept as carriers.
- **GDExtension coverage** is limited to what the host implements (above) and is untested against real plugins.
- **Networking (G6, `plans/fabric/G6_NETWORK.md`).** `ENetMultiplayerPeer`, `MultiplayerAPI` (`SceneMultiplayer`), `@rpc` / `[Rpc]`, `MultiplayerSpawner`, `MultiplayerSynchronizer` and the UDP / TCP peers run over Aver's own reliable-UDP protocol: a converted game talks to another Aver process, not to a stock Godot server. Authentication, server relay, authority, late joiners and visibility are implemented; `HTTPRequest`, `WebSocketPeer` and WebRTC are not. Nothing has been played over a real network yet.
- The converter never ran inside the engine's own test environment; the converted levels are checked by loading them with Aver's loaders and the scripts by compiling them, not by playing the game. Expect to find gaps when you first play a converted project, and report them with the project's `FabricReport.md`.
- Godot's own tick-for-tick physics (GodotPhysics3D) is not reproduced.

## Where the details live

- Converter contracts: `docs/plans/fabric/G3_FORMATS.md` (text-format reader), `G3_SCENES.md` (scenes, project, report), `G3_RUNTIME.md` (the node contract of `Aver.Fabric`), `G3_SCRIPTS.md` (GodotSharp and GDScript), `G3_GDEXT.md` (GDExtension host).
- Plan and status tables: `docs/plans/fabric/FABRIC_CONNECTOR.md`.
- Godot physics semantics on Aver: `docs/physics/LAYER_FILTERING.md`, `SPACE_QUERIES.md`, `KINEMATIC.md`, `AREAS.md`, `RAYCAST_VEHICLE.md` (renamed from `GODOT_*.md` when the general features lost the Godot name).
- Runtime API list: `docs/api/Aver.Fabric.md`.
- The axis map: `docs/ASSET_IMPORT.md`.
