# G3 F1a: the Godot text-format reader (`modules/formats.godot`)

Library `Aver.Formats.Godot` (static, always built, depends on Aver.Core and Aver.Platform only). Namespace `aver::fmt::godot`. Headers under `modules/formats.godot/include/aver/formats/godot/`. Test: `FormatsGodotTest` over the hand-written project in `tests/fabric/data/` (`MiniGame/`, `Godot3Scene/`).

The reader turns Godot 4 text files into plain structs. It knows no classes (a `Node3D` is a node with `type == "Node3D"`), converts no units or axes, and loads no images, meshes or scripts. Everything the converter needs about *meaning* is built on top of it.

## Conventions every other package relies on

- **Raw Godot values.** Metres, +Y up, right-handed, exactly as written in the file. The reader performs **no** axis or unit conversion. The conversion the converter must use is the one `GltfImport.cpp` (`toEngine`) already applies: `(x, y, z) -> (-z, x, y)`, positions and sizes times 100 (metres to centimetres), winding reversed; rotations conjugated through the same map. There is no shared helper yet; the placement package that first needs it creates one (a single header in `modules/formats`) and the glTF/FBX/OBJ importers should call it too.
- **Diagnostics, not exceptions.** Every parse function takes a `GodotDiagnostics&` (or pointer) and returns `true` when *this call* added no errors. A failing parse still fills `out` with whatever was readable, and recovers at the next `key = value` line or `[tag]` so one run lists every error. A `GodotDiag` has `severity`, `file`, `line`, a machine `code` and a `message`; `str()` prints `file:line: error: message`.
- **Codes in use:** `godot.parse` (syntax), `godot.format` (header format=), `godot.kind` (a .tres read as a scene or the reverse), `godot.ref` (undeclared `ExtResource`/`SubResource`), `godot.node_parent`, `godot.node_path`, `godot.unknown_section` (warning), `godot.binary_resource`, `godot.binary_script`, `godot.unreadable`, `godot.unresolved`, `godot.uid_moved` (info), `godot.uid_duplicate` (warning), `godot.unsupported_version`, `godot.project`, `godot.import`, `godot.gdextension`, `godot.scan`.
- **File names in diagnostics** are whatever the caller passes as `file`/`displayName`. `GodotProjectFs` always passes the `res://` path.
- **Binary files.** `.scn`/`.res` (magic `RSRC`/`RSCC`) and compiled `.gdc` (`GDSC`) are detected by `detectGodotFileKind` and refused by `readGodotTextFile` with code `godot.binary_resource` and the text "resave as text" (`godot.binary_script` for `.gdc`: the converter needs the `.gd`). `GodotProjectFs::scan` lists every `.scn`/`.res` in `binaryResources()` and warns once for each.
- **Versions.** `format=3` (Godot 4) reads cleanly. `format=2` (Godot 3 spellings: `PoolIntArray`, `Quat`, `Transform`, integer ids) reads with a warning. `format>3` warns. `project.godot` `config_version < 5` is an error (`godot.unsupported_version`); a `config/features` version below 4.1 warns.

## Variant (`GodotVariant.hpp`)

`GodotVariant` is one tagged struct (`type: GodotVType`, `line`, and the payload fields `b i f s s2 nums strs bytes items keys typed`). Which fields a type uses:

| Type | Payload |
|---|---|
| `Nil` / `Bool` / `Int` / `Float` | none / `b` / `i` (64-bit, hex ok) / `f` (`inf`, `inf_neg`, `nan` ok) |
| `String`, `StringName`, `NodePath` | `s` (UTF-8; `\n \t \u \U` and surrogate pairs decoded) |
| `Vector2/2i/3/3i/4/4i`, `Rect2/2i`, `Quaternion`, `Plane`, `AABB`, `Color` | `nums`, in the literal's order (`Quaternion` = x,y,z,w; `Rect2` = x,y,w,h; `AABB` = position then size) |
| `Basis` | 9 numbers **row by row** as written: `nums[r*3 + c]`; the X axis is the first *column* `(nums[0], nums[3], nums[6])` |
| `Transform3D` | the 9 basis numbers (row by row) then the origin (`nums[9..11]`) |
| `Transform2D` | `xx, xy, yx, yy, ox, oy` as written (columns x, y, origin) |
| `Projection` | 16 numbers as written (column by column) |
| `Array` | `items`; typed `Array[T]([...])`: `typed = true`, `s = "T"` (`ExtResource("id")` text for a script class) |
| `Dictionary` | `keys` and `items` (parallel); typed: `typed`, `s` = key type, `s2` = value type |
| `Object(Class, "p": v, ...)` | `s` = class, `keys` (String variants) and `items`; use `find("p")` |
| `PackedByteArray` | `bytes` (the base64 string form and the int-list form both read) |
| `PackedInt32/Int64/Float32/Float64Array`, `PackedVector2/3/4Array`, `PackedColorArray` | `nums` flat (strides 1, 1, 1, 1, 2, 3, 4, 4); `count()` gives the element count |
| `PackedStringArray` | `strs` |
| `ExtResource`, `SubResource` | `s` = id text (`"1_abc"`; `"3"` in format 2) |
| `Resource("path")` | `s` = path |
| `RID`, `Callable`, `Signal` | `i` / `items` (constructor arguments) |

Integers stored in `nums` (`Vector2i`, `Rect2i`, `PackedInt64Array`) are doubles: exact to 2^53. Helpers: `asNumber/asInt/asBool/asString`, `num(i)`, `count()`, `find(key)`, `operator==` (ignores `line`, NaN == NaN). `parseGodotVariant(text, out, diag, file, firstLine)` parses one literal; `godotVariantToText(v)` writes canonical text that parses back to an equal tree (the test checks every literal through this round trip). `GodotVariantReader` is the cursor the file parsers use; the converter can use it for ad-hoc strings.

Not accepted: `Color("#hex")` (Godot's own writer never emits it), unquoted dictionary keys.

## Sections (`GodotSections.hpp`)

`parseGodotSections(text, file, out, diag)` is the shared line grammar: `[tag key=value ...]` headers (`GodotTag{name, attrs, line}`) each followed by `key = value` lines (`GodotProperty{name, value, line}`). Keys may contain `/` and `.` (`surface_material_override/0`, `windows.debug.x86_64`) or be quoted. `;` comments run to the end of the line, also after a value. Lines before the first tag form a section with an empty tag name. `readGodotTextFile(path, text, diag, displayName)` loads text and applies the binary rule.

## Scenes and resources (`GodotScene.hpp`)

`parseGodotScene` / `loadGodotScene` fill a `GodotScene`; `parseGodotResource` / `loadGodotResource` a `GodotResource`. Both derive from `GodotDocBase`:

- `file`, `header` (`kind` "gd_scene"/"gd_resource", `type`, `scriptClass`, `format`, `loadSteps`, `uid`, `line`);
- `extResources`: `GodotExtResource{id, type, path, uid, line}`, `findExt(id)`;
- `subResources`: `GodotSubResource{id, type, props, line}`, `findSub(id)`, `prop(name)`.

`GodotScene` adds:

- `nodes` (file order, parents first). `GodotNode`:
  - `name`, `type` (empty for an instance or an override), `isRoot`, `parentPath` (as written; `.` = root);
  - `path` (scene-relative: `.` for the root, else `Parent/Name`), `parent` (index into `nodes`, or -1 for the root);
  - `instance` (ext resource id of `instance=ExtResource(...)`), `instancePlaceholder`, `owner`, `groups`, `nodePaths` (the `node_paths=` property names), `uniqueId`, `index` (sibling-order attribute, -1 if absent);
  - `attrs` (every header attribute), `props` (property lines), `prop(name)`, `line`.
  - **Parents inside instances.** A node whose `parentPath` names a node *inside an instanced sub-scene* (`parent="Crate/Body"` where `Crate` is an instance) has `parent == -1`, `unresolvedParent == true` and `anchor` = the index of the nearest node of this file on that path (the instance node `Crate`). A node with no `type` and no `instance` is an **override** (`isOverride`): it edits a node that comes from the instanced scene (or inherited base), and its property lines are overrides to apply on top of the instance.
  - **Inherited scenes.** When the root has an `instance`, `inheritsFrom()` returns that ext resource id.
- `connections`: `GodotConnection{signal, from, to, method, flags, unbinds, binds, line}` (paths relative to the scene root, `.` = root; `flags` are Godot's `CONNECT_*` bits).
- `editablePaths` (`[editable path="Crate"]`: the instance's children are opened for editing).
- `findNode(path)`, `childrenOf(index)` (file order; the `index` attribute is not applied).

`GodotResource` adds `props` (the `[resource]` section) and `prop(name)`.

Checks done at parse time: header present and first; `format=`; duplicate ext/sub ids; the first node is the only one without `parent`; every non-root node's parent resolves (locally or through an instance anchor); duplicate node paths; `instance=` points at a declared ext resource; **every `ExtResource("id")` / `SubResource("id")` anywhere in a value points at a declaration**. Unknown sections warn (`godot.unknown_section`) and are skipped.

## Project, import and extension files (`GodotProject.hpp`)

All three are ConfigFile text. `GodotConfig` is the raw view (`section(name)`, `get(section, key)`, `getString`); top-level keys live in section `""`.

- **`GodotProject`** (`parseGodotProject`, `loadGodotProject(folderOrFile)`): `configVersion`, `name`, `description`, `mainScene`, `icon`, `features`, `engineVersion` ("4.3"), `usesCSharp`, `renderingMethod` ("forward_plus" | "mobile" | "gl_compatibility"), `autoloads` (`{name, path, singleton, order, line}`; the `*` prefix is stripped into `singleton`; a path may be `uid://`), `inputActions` (`{name, deadzone, events}`; each event is an `Object` variant such as `InputEventKey` with its properties), `enabledPlugins`, `layerNames` (`"3d_physics/layer_1" -> "player"`), and `setting("section/key/subkey")` for anything else (`setting("display/window/size/viewport_width")`; the first component is the file section).
- **`GodotImport`** (`parseGodotImport`, `loadGodotImport`): `[remap]` `importer`, `type`, `uid`, `valid`, `path`, `pathsByTag` (`path.s3tc` ...), `metadata`; `[deps]` `sourceFile`, `destFiles`; `[params]` as `params` (`param(name)` with names as written, e.g. `compress/mode`).
- **`GodotGdExtension`** (`parseGodotGdExtension`, `loadGodotGdExtension`): `entrySymbol`, `compatibilityMinimum/Maximum`, `reloadable`, `libraries` (`{key, tags, path}` where `tags` is the key split on `.`), `icons`, `dependencies` (`{key, files[(source, destination folder)]}`), and `selectLibrary(features)` using Godot's rule: the first `[libraries]` entry, in file order, all of whose tags are in the feature set.

## res:// and uid:// (`GodotProjectFs.hpp`)

`GodotProjectFs fs; fs.open(projectFolder, diag); fs.scan(diag);`

- `normalizeRes("res://a/./b/../c")` collapses `.`/`..` and backslashes; empty for non-`res://`, or a path that climbs out of the root.
- `toDiskPath(ref)` / `toResPath(disk)` map between a `res://` or `uid://` reference and a disk path (`/` separators). `user://` does not resolve (empty).
- `scan` walks the project (skips dot-folders such as `.godot` and `.git`, and any folder holding `.gdignore`), fills `files()` (sorted `res://` paths) and `binaryResources()`, and builds the uid map from text, since `.godot/uid_cache.bin` is binary and is **not read**:
  - `uid="uid://..."` in the header of every `.tscn` / `.tres`;
  - `<file>.uid` sidecars (Godot 4.4+): `foo.gd.uid` maps its uid to `res://foo.gd`;
  - `[remap] uid=` of every `<file>.import`, mapping to the source file `res://<file>`.
  The first path (sorted) wins when two files claim a uid; a duplicate warns. Uids inside binary `.scn`/`.res` are not found.
- `resolve(uid, path)` is Godot's rule for an `[ext_resource]`: the uid wins when it is in the map, otherwise the normalised path; a uid that now names a different file than `path` adds an info diag (`godot.uid_moved`). `resolve(const GodotExtResource&)` is the same for a loaded document.
- `loadScene(ref)`, `loadResource(ref)`, `loadImport(sourceRef)` (reads `<ref>.import`), `readText(ref)` resolve, read, parse, and name the document by its `res://` path.

## Not in F1a

Interpreting node types, properties or resources (placement, meshes, materials, lights, physics shapes), GDScript, `.gdshader`, `.import` payloads (`.ctex`, `.mesh`) and the uid cache binary; 2D nodes are out of scope for the whole converter.
