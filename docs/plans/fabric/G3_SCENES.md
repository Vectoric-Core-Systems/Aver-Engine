# G3 F1b: the scene and asset converter (`AverAssetC fabric`)

`AverAssetC fabric <godot-project-dir> <out-dir>` turns a Godot 4.1+ project into a standalone Aver project. The code is `tools/fabric/*.cpp`, linked into `AverAssetC` (`tests/formats/CMakeLists.txt`); `AverAssetC.cpp` has one dispatch line and the model-import callback. It reads Godot's text formats through `Aver.Formats.Godot` (G3_FORMATS.md) and writes nodes exactly as G3_RUNTIME.md section 1 says. Test: `FabricConvertTest` (144 checks) over `tests/fabric/data/ConvertGame/`, run through ctest like any `*Test`.

3D and UI first, one way only (Godot to Aver). 2D nodes are reported unsupported. This package records which script goes with which class (`Fabric/scripts.json`); the script converter (G3_SCRIPTS.md) and the GDExtension host (G3_GDEXT.md) are driven from the same command ("The whole conversion", below).

Exit code 0 when the project converted (unsupported items are not a failure; they are in the report), 1 when the project could not be read or a scene or file could not be written, 2 on bad usage. stdout carries AverAssetC's JSON-lines artifacts for the imported models and a final summary line:

```
{"schemaVersion":1,"summary":true,"kind":"fabric","input":"...","output":"<out>/<Name>","project":"<Name>","levels":2,"converted":71,"approximated":21,"unsupported":13,"scripts":3,"scriptsConverted":true,"extensions":1,"report":"<out>/FabricReport.md","dryRun":false,"exitCode":0}
```

## The whole conversion (F6)

`AverAssetC fabric <godot-dir> <out-dir> [--dry-run] [--scenes-only] [--scripts-only] [--overwrite]` (`tools/fabric/FabricConvert.cpp` `run`) is the one command. The order, with no option:

1. Read `project.godot`, scan the project, work out `<Name>`. Refuse (exit 1) when `<out>/<Name>/<Name>.ocproject` exists and `--overwrite` is not given.
2. `Converter::convertExtensions` (`FabricExtensions.cpp`): every `.gdextension` and the libraries it names for windows, linux and macos (and `[dependencies]`) are copied into `Content/` at their `res://` path; `[icons]` keys are the plugin's classes (`setExtensionClasses`); a class is spatial when a scene gives a node of it a Vector3 position, rotation or scale or a Transform3D. A node of such a class becomes the class placement `GDExtensionNode3D` / `GDExtensionNode` with `_class` and the Prop `extension_class="TheClass"` (G3_GDEXT section 7). Reported: the file (approximated; unsupported when `compatibility_minimum` is below 4.1 or no library for the running OS exists), each class (approximated), each desktop library the project lacks (unsupported). `Fabric/project.json` gets `"extensions": [{path, content, entrySymbol, compatibilityMinimum, compatibilityMaximum, hostLibrary, classes, files, missing}]`. At run time `GDExtensionManager.LoadProjectExtensions` loads every `.gdextension` under `Paths.Content` the first time a plugin class is asked for.
3. Scenes, standalone resources, project files, input, translations, the project (everything above in this file); `Fabric/scripts.json` and `Fabric/project.json`; `FabricReport.md` / `.jsonl` (without script entries).
4. `Converter::convertScripts` (`FabricScripts.cpp`): `dotnet <extension dir>/tools/Aver.Fabric.Convert.dll scripts --project <godot> --out <out>/<Name> --manifest Fabric/scripts.json --refs <extension dir>/managed;<exe dir>/Scripting --report <out>/FabricReport.jsonl [--engine <checkout above the exe>]` through `FabricProcess.cpp` (`CreateProcessW` / `posix_spawnp`, no shell). The tool's exit code is read with its summary line: 0, 3, or 1 with a summary are the scripts' own result (compile errors are report entries, not a failure of the run); anything else, or dotnet / the tool / `Aver.Fabric.dll` missing, writes an unsupported entry per script and fails the run (exit 1). The merged jsonl is read back (`Report::readJsonl`) and **`FabricReport.md` is rewritten from it**, so the final report is one document: result line, per-kind table, top unsupported features, unsupported and approximated items by file.
5. The summary line.

| Option | Effect |
|---|---|
| `--dry-run` | Converts into `<tmp>/aver-fabric-dry-<hash>`, copies the two report files to `<out-dir>`, deletes the scratch folder; the summary has `"dryRun":true` and `output` is `<out-dir>`. |
| `--scenes-only` | Skips step 4. The three scripts' entries are `unsupported` with the reason "script conversion was skipped (--scenes-only)". |
| `--scripts-only` | Needs the project from an earlier run (exit 1 without it). Reads the earlier jsonl, writes `scripts.json` only when absent, runs step 4. `levels` in the summary is 0. Cannot be combined with `--scenes-only` (exit 2). |
| `--overwrite` | Replaces the project in place; files the converter did not write are left alone. |

The editor's File > Import Godot Project... (the generic import dialog `sandbox/src/ExtensionImport.cpp`, driven by this extension's `extension.json`) launches this command as a child process and reads the summary line; its headless half is tested by `FabricImportTest` and the engine's `ExtensionImportTest`. The end-to-end test is `FabricE2ETest` over `tests/fabric/data/E2EGame/`.

Output is deterministic: the same input gives the same files byte for byte (tested).

## Output layout

```
<out-dir>/FabricReport.md            the report for people
<out-dir>/FabricReport.jsonl         the same, one JSON object per line (schema below)
<out-dir>/<Name>/<Name>.ocproject    manifest; <Name> is application/config/name with everything but [A-Za-z0-9_-] removed
<out-dir>/<Name>/Fabric/scripts.json     script path -> generated class name (below)
<out-dir>/<Name>/Fabric/nodes.json       each scene's nodes as placed (path, class, script, owner, %Unique, assigned .tscn files), for typed $Path in scripts (G3_SCRIPTS.md "Typed node access")
<out-dir>/<Name>/Fabric/project.json     autoloads, layer names, tonemap, start level, level and prefab lists
<out-dir>/<Name>/Content/
    Maps/<scene path>.ocworld        one level per .tscn: res://props/crate.tscn -> Maps/props/crate.ocworld
    Prefabs/<scene path>.ocprefab    one per instanced .tscn (structure only, see Instanced scenes)
    Meshes/Generated/<Type>_<hash>.ocmesh    PrimitiveMesh sub-resources
    Meshes/<model path>/<name>.ocmesh        imported models (.glb .gltf .fbx .obj), with skeletons and clips beside them
    Materials/<Name>_<material>.ocmat        StandardMaterial3D / ORMMaterial3D
    Textures/<Name>/<texture>        textures a material uses (copied by the material cook)
    Textures/<res:// path>           every other texture
    Input/<Name>.ocinput             the [input] map
    Localization/<culture>/<name>.po translations
    Scripts/<Class>.cs               the converted scripts, Scripts.csproj (script step, G3_SCRIPTS.md)
    <res:// path>.gdextension        every GDExtension file and its libraries, at their res:// paths (F6)
```

`<Name>` is also the material-name prefix, so materials of two projects never collide in one Content folder. Paths in Props values are content-relative asset paths with no `res://`.

## Project

| Godot | Aver |
|---|---|
| `application/config/name` | `NAME`, `WINDOW.TITLE` |
| `application/run/main_scene` | `STARTMAP Maps/<scene>.ocworld` |
| `display/window/size/viewport_width` `viewport_height` `resizable` `mode` | `WINDOW.SIZE` `WINDOW.RESIZABLE` `WINDOW.FULLSCREEN` (mode 3 or 4) |
| `physics/3d/default_gravity` times `default_gravity_vector` | `PHYSICS.GRAVITY`, through the axis map and times 100 (9.8 down is `0 0 -980`) |
| `physics/common/physics_ticks_per_second` | `PHYSICS.FIXEDSTEP` = 1 / ticks |
| `physics/common/physics_interpolation` | `PHYSICS.INTERPOLATION` 0 or 1 |
| (always) | `PHYSICS.COMBINE material` (written `godot` before the general-feature rename; both read) |
| `physics/3d/default_linear_damp` `default_angular_damp` | no project key; added into every RigidBody3D's damping (below) |
| `internationalization/locale/fallback` | `LOCALE.DEFAULT` |
| `internationalization/locale/translations` | `.po` files copied to `Localization/<culture>/`; the culture is the file's `Language:` header, else its name. `.translation` and `.csv` are reported unsupported |
| `layer_names/3d_physics/layer_N` | `# FABRIC.LAYER 3d_physics N <name>` comment lines in the .ocproject (it has no key) and `layerNames` in `Fabric/project.json` |
| `[autoload]` with `*` | persistent top-level placements, first in the **start level** (below). An entry without `*` is reported and not placed, as Godot does not add it to the tree either |
| first scene's `WorldEnvironment` | (G4 R3) `RENDER.TONEMAP` (3 + Godot's `tonemap_mode`), `RENDER.TONEMAPWHITE`, `RENDER.EXPOSURE` (`tonemap_exposure` x the camera attributes' `exposure_multiplier`, written only off 1), `RENDER.AUTOEXPOSURE` (0 unless CameraAttributesPractical enables it), `RENDER.BLOOM` (glow intensity, 0 without glow), `RENDER.ADJUST*` (display adjustments, written only when `adjustment_enabled`); the tonemap mode is also in `Fabric/project.json` |
| `application/config/tags`, `debug/gdscript/warnings/*`, `debug/shader_language/warnings/*`, `rendering/textures/vram_compression/*` | editor and import-time settings: nothing a running game does changes; reported approximated, not written |
| every other setting | reported unsupported, by key |

### Input map

`[input]` becomes `Input/<Name>.ocinput` and `INPUT.SCHEME`. Action names are kept. `CONTEXT <Name>Input priority 0`.

| Godot event | Aver |
|---|---|
| `InputEventKey` (`keycode`, else `physical_keycode`, else `key_label`) | `BIND <a> key <Name>` for A-Z, 0-9, Space, Shift, Ctrl, Alt, Enter (also KP Enter), Escape, Tab, arrows. Anything else (F-keys, Backspace, Home, End, PageUp, PageDown, Ctrl+X as a chord ...) is reported unsupported and not bound, the key named in the reason. A key with Shift, Ctrl, Alt or Meta required is bound bare and reported approximated. A key event with no keycode, physical keycode or key label binds nothing in Godot either: reported approximated, no binding (an action holding only that is declared with none). Two events that name one Aver key (Enter and KP Enter) are one binding. **Old projects**: a special key written with Godot 3's flag (`1<<24 | n`, Godot 4 writes `1<<22 | n`) is read as the Godot 4 key of the same low bits (Enter 16777221, Escape 16777217, Tab 16777218, the arrows 16777231 to 16777234) and reported approximated |
| `InputEventMouseButton` 1, 2, 3 | `key MouseLeft` / `MouseRight` / `MouseMiddle` |
| wheel up, down (4, 5) | `mousewheel` with scale +1 / -1, reported approximated; side buttons and horizontal wheel unsupported |
| `InputEventJoypadButton` | `gamepadbutton` A B X Y Back Start LeftThumb RightThumb LeftShoulder RightShoulder DPad* (Godot 4's numbering: d-pad 11 to 14); Guide and the extra buttons (Misc 1, paddles, touchpad) are unsupported, named. An action that also holds a Godot 3 key code holds Godot 3 joypad numbers (d-pad 12 to 15, shoulders 4 and 5, sticks 8 and 9, Select 10, Start 11): they are read that way and reported approximated, and Godot 3's L2 / R2 buttons (6, 7) are bound as the `LeftTrigger` / `RightTrigger` axes |
| `InputEventJoypadMotion` | `gamepadaxis` LeftX LeftY RightX RightY LeftTrigger RightTrigger, scale = sign of `axis_value`, flipped for the Y axes (Godot's stick Y runs down, XInput's up); the action becomes `axis1` with a `deadzone:<d>:1` modifier. Reported approximated: Godot's strength is never negative, an Aver axis binding is signed |
| mouse motion, touch, shortcuts, anything else | reported unsupported |

Only the actions the project file overrides are written; Godot's built-in `ui_*` actions are not.

## Scenes

Every `.tscn` of the project is converted; binary `.scn`/`.res` files are reported "resave it as text". The scene tree is built first (instances expanded, overrides applied), then written as one **class placement per node** (G3_RUNTIME 1.1 A):

```
PLACE none <x y z yaw pitch roll scale> class <Class> name <Name>       (PLACEG with three scales when non-uniform; CHILD / CHILDG nested under BEGIN/END)
  COMPONENT Props _name="<Name>" _owner=".." ... <property>=<Variant text>
  COMPONENT Groups names=a,b
```

- **Transform.** Godot local transform to the placement line through `aver/formats/AxisMap.hpp`, the one map `GltfImport.cpp`, `ObjImport.cpp` and the Y-up USD path use: position `(x,y,z) -> (-z,x,y) * 100`, rotation quaternion `(x,y,z,w) -> (z,-x,-y,w)`, scale `(x,y,z) -> (z,x,y)`. A basis is decomposed Godot's way (scale = column lengths, negated when the determinant is; rotation = columns divided by them), so `transform`, `position` + `rotation` (order YXZ unless `rotation_order`) + `scale`, `rotation_degrees`, `quaternion` and `basis` all come out the same. Float noise in a scale (0.99999994) is snapped to 1. A sheared basis also writes the exact `transform` Prop and is reported approximated. Transforms are parent-relative.
- **Class.** The node's Godot class when it is in G3_RUNTIME's class table (Node, Node3D, Marker3D, VisualInstance3D, GeometryInstance3D, MeshInstance3D, Camera3D, the three lights, CollisionShape3D, StaticBody3D, AnimatableBody3D, RigidBody3D, CharacterBody3D, Area3D, Timer); the **script's generated class** when a script is attached. Everything else is a *carrier*: the placement is the nearest class that exists and `_class="<GodotClass>"` names the real one, with the node's properties kept as Props (metadata at runtime). Control family -> `Control`, SubViewport -> `Viewport`, WorldEnvironment -> its own class since G4 R3, other 3D classes (joints, NavigationAgent3D, XROrigin3D ...) -> `Node3D`, 2D classes and everything else -> `Node`.
- **Props.** `_name` always, then `_owner` (relative path to the owner, omitted on the scene root), `_unique`, `_class`, `_persistent`, then every property line in file order as Godot's Variant text. `ExtResource`/`SubResource` never reach the file: a mesh is `Resource("Meshes/...ocmesh")`, a material `Resource("Materials/...ocmat")`, a texture `Resource("Textures/...")`, a PackedScene value `Resource("Maps/...ocworld")`, a collision shape `Object(BoxShape3D, "size": Vector3(...))` in Godot metres (the runtime converts). Properties whose value could not be converted are left out and reported. A path-style property (`metadata/x`) is written and reported approximated.
- **Groups**: header `groups=[...]` plus overrides, as `COMPONENT Groups names=a,b`.
- **Connections**: every `[connection]` rides on its source node as `_connections` (array of `{signal, to, method, flags, binds, unbinds}`), `to` relative to the source, the method name as Godot wrote it. A connection naming a missing node is reported.
- **Owners**: nodes written in a scene belong to that scene's root; an instance root belongs to the scene that instanced it.

### Godot defaults made explicit

For StaticBody3D, AnimatableBody3D, RigidBody3D, CharacterBody3D and Area3D: `collision_layer` and `collision_mask` (32-bit, default 1) are always written. RigidBody3D also gets `mass` (1) and `gravity_scale` (1), and **damping is written as the effective value**: `linear_damp_mode` Combine (the default) adds the project default (`default_linear_damp`, 0.1 in Godot) to the body's own, Replace keeps the body's own; same for angular; the `*_damp_mode` properties are dropped. Friction 1.0 and bounce 0 are Godot's `PhysicsMaterial` defaults and are carried by `PHYSICS.COMBINE godot`, not written per body. A `physics_material_override` is written as a dictionary `{"__class": "PhysicsMaterial", "friction": ..., "bounce": ..., "rough": ..., "absorbent": ...}` and reported approximated, because the runtime body node does not read it yet. `contact_monitor`, `max_contacts_reported`, area overrides (`gravity_space_override`, `gravity`, `linear_damp_space_override` ...) and axis locks are written as properties; the area overrides and axis locks are reported approximated (the node classes do not apply them yet).

### Collision shapes

| Godot | Written |
|---|---|
| BoxShape3D, SphereShape3D, CapsuleShape3D, CylinderShape3D, WorldBoundaryShape3D | `Object(<Class>, ...)` with the properties as written |
| ConvexPolygonShape3D, ConcavePolygonShape3D | the same, with `points` / `data` |
| HeightMapShape3D (up to 64 x 64) | a ConcavePolygonShape3D of the grid's triangles, centred on the origin as Godot does; reported approximated. Larger maps are kept as a dictionary and reported |
| `CollisionPolygon3D` | becomes a CollisionShape3D: the polygon extruded to `depth`, centred on Z; a convex polygon gives a ConvexPolygonShape3D, a concave one an ear-clipped triangle shape; reported approximated |
| SeparationRayShape3D and others | kept as a dictionary with `__class`, reported |
| `.tres` shapes | resolved like sub-resources |

Joints (`PinJoint3D`, `HingeJoint3D`, ...) are carriers whose parameters and `node_a`/`node_b` paths stay as Props for the joint package (the G2 joint mapper is C#-side).

### Meshes and materials

- **PrimitiveMesh** sub-resources (BoxMesh, SphereMesh, CapsuleMesh, CylinderMesh, PlaneMesh, QuadMesh, PrismMesh, TorusMesh) are generated with Godot's defaults and dimensions, in Godot space, then through the axis map with the winding reversed (`FabricConvertTest` checks that they face their normals the way the glTF importer's meshes do). Equal meshes share one file. The mesh's own `material` becomes its slot name. UVs are per-surface 0..1, not Godot's atlas layout.
- **Model files** (`.glb .gltf .fbx .obj`) go through AverAssetC's own importers (`convertImportedModel`, the material cook, the skeleton and clip writers); an instanced **glTF** model becomes a `Node3D` named after the instance with the node tree Godot's importer makes (next paragraph); any other model format becomes that `Node3D` with a `MeshInstance3D` child per mesh at the model's own placement; a `mesh` property that points at a model takes its first mesh (reported when there are several). The import's `nodes/root_scale` is reported, not applied.
- **ArrayMesh** (sub-resource or `.tres`/`.res`): not decoded. Godot stores the surface in its packed vertex format; the mesh is reported unsupported ("re-export as .glb").
- **StandardMaterial3D / ORMMaterial3D** go through `fmt::cookMaterials`: `albedo_color` (sRGB, as the .ocmat holds it), `albedo_texture`, `metallic`, `roughness`, the roughness / metallic / ORM textures (packed by `packSeparateMetalRough`), `normal_*`, `ao_*`, `emission*` (linear colour times energy, no unit calibration, reported), `transparency` (alpha -> blend, scissor and hash -> masked with `alpha_scissor_threshold`, depth pre-pass -> blend), `cull_mode` disabled -> two-sided, subsurface scattering -> weight. Reported unsupported by property: `uv1_scale`/`uv1_offset` (the .ocmat has no UV transform), front-face culling, rim, clearcoat, anisotropy, parallax, billboard, triplanar, distance fade and every other setting with no .ocmat field. A texture the image decoder cannot read (.webp, .svg, .exr, .hdr, .dds ...) is reported and not bound.
- **ShaderMaterial**, `.gdshader`: reported unsupported (G6).
- `surface_material_override/0` of a MeshInstance3D is written as `material_override` (approximated); other surfaces are reported.

### Lights, camera, environment

Lights and cameras are class placements with Godot's own properties (`light_energy`, `omni_range`, `spot_angle`, `fov`, `near`, `far`, `current` ...); the runtime does the unit mapping (G3_RUNTIME 1.4, `Light3D.LuxPerEnergy` / `CandelaPerEnergy` 104719.755, G4_RENDER.md R2). The level also gets records, so it is right before any node is ready:

| Level record | From |
|---|---|
| `SUN` | the first DirectionalLight3D: direction = the light's -Z reversed through the axis map, colour linear, `lux = light_energy * 104719.755` (pi engine units, G4 R2), `angular = light_angular_distance`. The node is still placed |
| `CAMERA` | the `current` Camera3D (else the first): world position in cm, yaw and pitch of its -Z |
| `SKY` | WorldEnvironment's Environment: background colour / ProceduralSkyMaterial -> the authored dome (colours scaled by the energy multipliers in linear light); PhysicalSkyMaterial -> Aver's physical sky (haze scales the Mie terms, `ground_color` the ground albedo, energy the sky light); PanoramaSkyMaterial -> the panorama fitted onto the authored dome (`PanoramaSky.hpp`, G4 R3: approximated, the renderers do not sample it); `ambient_light_energy` -> `skylight` |
| `FOG` | `fog_enabled`: density per metre / 100 per centimetre, light colour; depth fog (`fog_mode` 1) -> a start distance and a density reaching ~95% at the end; `fog_height` / `fog_height_density` -> Aver's height falloff; `volumetric_fog_enabled` -> distance fog at `volumetric_fog_density` (all approximated) |
| `POSTVOLUME` | (G4 R3) one infinite volume named `WorldEnvironment` per level that has the node: `tonemap`, `tonemapwhite`, `exposure`, `autoexposure`, `bloom` (+ `bloomthreshold` when stated), `adjbrightness`, `adjcontrast`, `adjsaturation`, `dof` / `doffocus` / `dofstop` / `dofmm` from the camera attributes |
| project `RENDER.*` | the start level's environment, see the table above |

The `WorldEnvironment` node itself is placed as the `WorldEnvironment` class with its `environment` and `camera_attributes` as inline `Object(Environment, ...)` / `Object(CameraAttributesPractical, ...)` values (and `Object(Sky, ...)`, the sky materials inside), so scripts read Godot's properties; the panorama texture is kept under `Textures/`. Every Environment setting without a record (SSAO, SSR, SSIL, SDFGI ...) is reported unsupported by name, with the note that Aver's ray-traced GI, AO and reflections cover it when ray tracing is on.

### Instanced scenes

Per G3_RUNTIME 1.7 the level holds the instance **flattened**: the instance node is a placement of the sub-scene's root class, its `scene_file_path` Prop is the Godot path, and every node below it is a nested placement owned by the instance root. Overrides in the instancing scene are applied to the copy: property lines on an instanced node (`[node name="Lid" parent="Crate" index="1"]`), nodes added inside the instance (`parent="Crate/Body"`), `index` sibling order, groups, scripts, transform. `[editable]` is implied. Inherited scenes (a root with `instance=`) work the same way. Instance placeholders are instanced at once (reported).

**Instanced glTF models** (`tools/fabric/FabricModelTree.cpp`, from the file's JSON only) are given the node tree Godot's importer makes, so a scene can add children to, and override, nodes inside the model (`Player/Skeleton/Skeleton3D/Robot`, `Enemy/AnimationPlayer`). The rules, as Godot 4 shows them in scenes: the instance is the root; each scene root node of the file is its child (with `GODOT_single_root` and an untransformed single root, that node is the root itself); names are the glTF names with `. : @ / " %` made `_`, a repeat getting a number after it; a joint is a bone, not a node; skins that share a joint are one skeleton, written as a `Skeleton3D` named `Skeleton3D` (then `Skeleton3D2`) under the parent of its top joint(s), where its first bone is met, with `bones/N/name`, `parent`, `rest`, `enabled`, `position`, `rotation`, `scale` (bones in depth-first order, so `bones/1/position` overrides in a scene land on the bone Godot means); a node hung on a bone is held by a `BoneAttachment3D` below the skeleton (named `BoneAttachment3D`, `BoneAttachment3D2`...) with `bone_name`; a skinned mesh node moves below its `Skeleton3D` after any attachments with its own transform dropped; a file with animations gets an `AnimationPlayer` as the root's last child. A camera or light node stays a plain `Node3D` (reported). The Aver importer stores a mesh with its world rotation and scale baked into the vertices and its position kept apart, so a mesh node's local transform is the one that puts it at that position under the real transforms of its parents (a `Skeleton` scaled 0.3 gives its mesh a scale of 3.33); children of the model's other nodes keep their authored transforms. The `Skeleton3D` is the script-level pose model (it is not connected to the skinned mesh, rated approximated), the `AnimationPlayer` holds no library until the scene's overrides give it one, and a model whose file cannot be read, or an FBX / OBJ, keeps the flat layout. Not guessed beyond the corpus: Godot's names for an unnamed glTF node and the suffix for a repeated name (both follow what the importer's source does as remembered, not a checked run).

An `AnimationPlayer`'s `libraries/<name>` properties (Godot 4's form for each library; `libraries/` is the default one) are merged into the one `libraries` dictionary the runtime reads. A `Curve`'s `_limits = [min_value, max_value, min_domain, max_domain]` is written as those four properties. A property on a node whose script declares it (an exported variable the node class has no such property for) is carried by name and reported approximated, not lost.

Each instanced `.tscn` is also written once as `Prefabs/<path>.ocprefab` (nodes with uid, parent, name, class, `CLocal` transform and `CMeshRenderer.mesh`), and as its own level. The prefab is structure for the editor; the properties live in the levels' Props, which prefab nodes cannot hold, so no level uses `PREFABINST` yet. Reported approximated once per scene.

### Autoloads

In the start level, before the scene root, in `project.godot` order: a script autoload is a placement of the script's class (base class from its `extends`), a scene autoload the flattened scene; both with `_persistent=1` and `_name=<AutoloadName>` (G3_RUNTIME 1.7). Other levels do not repeat them.

## `Fabric/scripts.json`

One entry per `.gd` and `.cs` file in the project, sorted by path. The script packages read it; the scenes name the same classes.

```json
{"schema": 1, "project": "ConvertGame", "scripts": [
  {"path": "res://scripts/player.gd", "kind": "gdscript", "className": "Player", "declaredClassName": true,
   "extends": "CharacterBody3D", "extendsPath": "", "exists": true, "autoload": false, "uid": "",
   "output": "Content/Scripts/Player.cs", "uses": ["res://main.tscn:Player"]}]}
```

- `className`: the C# class the converted script must be. `class_name` for GDScript, the class in the file for C#, else the file stem in PascalCase. A name that would collide with an Aver.Fabric or Godot class gets `Script` appended (`node.gd` -> `NodeScript`); a duplicate gets a number. The scenes' placements already use it.
- `extends`: the Godot class at the bottom of the chain (`extends "res://base.gd"` and `extends BaseClassName` are followed through other scripts); `extendsPath` the written path when it was a path.
- `output`: where the script packages should write the converted file, content-relative.
- `uses`: `"<scene>:<node path>"` for every node that attaches the script (the autoload's is `res://project.godot:<Name>`).
- `uid`: from the `.uid` sidecar when there is one.

## `Fabric/project.json`

`name`, `godotName`, `godotVersion`, `usesCSharp`, `renderingMethod`, `startScene`, `startLevel`, `levels[]`, `prefabs[]`, `autoloads[{name,path,singleton}]`, `layerNames{}`, `enabledPlugins[]` and `environment{found, tonemapMode, tonemapExposure, glow, source}`.

## The report

`FabricReport.jsonl`, one object per line, in the order things were found; identical entries are kept once.

```json
{"schema":1,"kind":"node","file":"res://main.tscn","node":"Floor/Mesh","type":"MeshInstance3D","property":"","rating":"converted","reason":"","line":42}
```

| Field | Meaning |
|---|---|
| `kind` | `project`, `setting`, `file`, `scene`, `node`, `resource`, `property`, `input`, `script` |
| `file` | the `res://` file the item is written in ("" for project-wide items). Nodes of an instanced scene are reported against the sub-scene's file and path, once |
| `node` | scene-relative node path ("" when not about a node); an action or autoload name for `input` / `setting` |
| `type` | the Godot class or resource type |
| `property` | the property or setting key ("" for the whole item) |
| `rating` | `converted` (behaves as in Godot), `approximated` (present but differs; `reason` says how), `unsupported` (not functional in Aver yet; the data is kept where possible) |
| `reason` | required for approximated and unsupported |
| `line` | line in `file` (0 when not known) |

`FabricReport.md` has the per-kind table, then the unsupported and approximated items by file, then a per-file count of converted ones.

What the report calls unsupported today: 2D nodes; every 3D class outside the runtime table (joints, soft bodies, navigation, audio and particle nodes (AnimationPlayer is in the table since G5 A1), Path3D, Skeleton3D, CSG, GridMap ...); shaders and shader materials; ArrayMesh data; audio, font, video and theme files; script compile errors (and scripts under `--scenes-only`); desktop GDExtension libraries the project does not hold; material features without an .ocmat field; Environment settings without a level record; keys and buttons outside Aver's input set. (`RayCast3D`, `ShapeCast3D`, `VehicleBody3D` and `VehicleWheel3D` are placed as their own classes since F6; the runtime has had them since F2b.)

## Not verified, and limits

- Nothing here has run in the engine (engine-run ban). The levels load with `loadOcworld`, the meshes with `loadOcMesh`, the materials with `loadOcmat`, the prefabs and input with their loaders; what the runtime then does with the Props is G3_RUNTIME's contract and F2a's tests, not this package's.
- The level `SUN` record is the sun (G4 R2): the engine never drew a directional light component, and the DirectionalLight3D node now adds none; the first directional light forwards later changes to the sky's sun overrides and any further one is inert and reported.
- A `.gltf`/`.glb` mesh property takes the first mesh; model `root_scale` and import-time options (LOD, lightmap UVs, animation splitting) are not applied. Skeletons and clips are written next to the meshes but no node uses them (no AnimationPlayer or skeleton nodes yet).
- Textures are copied verbatim; Godot's `.import` texture settings (compression, mipmaps, normal-map flags) are ignored.
- The test fixture's "PNG" is a stand-in text file, as in `MiniGame`: the cook copies bytes and the image decoder is not exercised, except by the packed-roughness path, which no fixture reaches (a material with both a roughness and a metallic texture).
- FBX models import through ufbx with its own axis handling, not `AxisMap.hpp`.
