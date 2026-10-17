# FBX import (ufbx), at parity with the glTF importer

Design contract for the implementing agent. Owner approved 2026-10-17, including vendoring ufbx (MIT). Nothing here is built. Rules in force: upgrade the existing importer/writer paths (no parallel import system), `Aver.Formats` base stays dependency-light, no GPU work in this feature (D3D12/Vulkan parity is n/a: it ends in `.ocmesh`/`.ocskel`/`.ocanim`/`.ocmat`, which both backends already consume), no new third-party code except ufbx, lean comments, every runtime surface gets C# API and Aver Node nodes.

## 0. Decisions in one screen

1. **ufbx is vendored at `third_party/ufbx/{ufbx.h,ufbx.c,LICENSE.txt,README.aver.md,CMakeLists.txt}`**, one C TU compiled as C into a static lib `ufbx` (warnings off, like meshoptimizer). Pin a release TAG, not a branch; `master`'s header reads `UFBX_HEADER_VERSION = ufbx_pack_version(0, 23, 1)`, so pin the newest tag >= 0.23.1 and `static_assert(UFBX_HEADER_VERSION >= ufbx_pack_version(0, 23, 0))`. Every ufbx name below was checked against that header; anything older is not guaranteed.
2. **A new CMake target `Aver.Formats.Fbx`** (`modules/formats`, the Material/Audio pattern) holds `FbxImport*.cpp` and links `ufbx`. The shipped game runtime (`Aver.Runtime`) does not link it, so ufbx never reaches a packaged game. Linked by `Sandbox`, `AverAssetC` and the tests. If `third_party/ufbx/ufbx.c` is absent the target still builds with `AVER_HAVE_UFBX=0` and `importFbx` fails with "built without ufbx", so the tree configures before the vendoring lands.
3. **One conversion frame, shared with glTF.** ufbx is asked for `target_axes = ufbx_axes_right_handed_y_up`, `target_unit_meters = 1`, `space_conversion = ADJUST_TRANSFORMS`; the result is then in glTF's frame (Y up, metres), so FBX goes through the SAME map as glTF (`x = -z, y = x, z = y`, times `scale`=100, winding reversed once, plus once more per mirrored node). A model exported to FBX and glTF lands in the same place. We deliberately do NOT ask ufbx for the engine's left-handed Z-up axes.
4. **`FbxImportResult` extends `GltfImportResult`** (it IS the same shape: meshes, placements, skeletons, clips, object clips, materials, images). The two writers that consume it (`importGltfToDir` in the editor, the glTF branch of `AverAssetC convert`) are refactored into model-agnostic functions that glTF and FBX both call. Two additive fields go on the shared base: `animationSkeleton` and `cameras`.
5. **Skeleton rest pose comes from the BIND pose** (skin cluster matrices), not the scene's frame-0 node transforms; inverse binds are re-derived from it so a bind pose skins to identity by construction (the glTF invariant, `GltfImport.cpp:799-821`). Skinned meshes are baked into bind-world space, including translation (no placement); static meshes keep glTF's rule (rotation+scale baked, translation becomes a placement).
6. **Animation = `ufbx_bake_anim`** per anim stack, one `OcTrack` per (bone, channel) like glTF, then converted. Object clips (`kOcAnimObject`) are supported by the glTF rule `A(t) = W(t) * Q0^-1`, sampled with `ufbx_evaluate_transform`. **Root motion**: the glTF importer sets no root-motion flag (grep `kOcAnimRootMotion` finds no importer), so FBX gets an option `rootMotion = Off | Auto | All`, default **Auto**: flag a clip when its root bone travels more than 10 cm in the plane. The flag only has effect when an entity has `CRootMotion`, so Auto changes nothing for existing content.
7. **Influences**: top 4 by weight, renormalised, `u16` bone index; truncation is counted and reported (never silent). More than 256 bones is reported (`kSkinMaxBones`).
8. **Blend shapes**: imported as sparse engine-space deltas and handed to the morph-target feature (`docs/plans/wave9/MORPH_TARGETS.md`) through ONE adapter behind a macro (section 2.7). Until that feature defines `AVER_OCMESH_HAS_MORPH`, shapes are reported under `unsupported`, not dropped silently. Weight animation goes out as `OcCurve`s named `morph.<shape>`.
9. **Materials** flow through the existing `ImportedMaterial` -> `cookImportedMaterials` path. The one real gap is that FBX names roughness and metalness as separate maps while `.ocmat` has one packed slot: add `ImportedMaterial::metallicTex` and a pack step in the cook policy layer (`ImportCook.cpp`, beside `mergeOpacityMaps`), instead of the OBJ importer's "roughness wins, metal dropped" (`ObjImport.cpp:644-652`).
10. **Editor**: `.fbx` joins the Import dialog with an options panel, drag-and-drop, `--import-fbx` CLI, and a **`.ocimport` record** written beside the outputs so the Content Browser can **Reimport** with the stored options. Cameras: the first FBX camera becomes the level's start camera (as USD does); lights are reported, not imported (parity with glTF).
11. **Script surface** is deliberately small because import is an editor-time act: C ABI `framework_import_abi.h` (provider relay, only the editor installs one), C# `ModelImport`, and two Aver Nodes (`ImportModel`, `IsModelImportAvailable`). A packaged game has no provider, so every call returns "unavailable".

## 1. What exists today

| Piece | Where | State |
|---|---|---|
| glTF options/result | `modules/formats/include/aver/formats/GltfImport.hpp:114-118` (options), `:47-111` (result), `:121` (`importGltf`) | Result already carries everything FBX needs except per-clip skeleton and cameras. |
| Shared material shape | `ImportedMaterial.hpp:59` (`ImportedMaterial`), `:30` (`ImportedImage`: encoded bytes verbatim), `:41` (`ImportedTexture` with `channel`), `:82` (`metalRoughTex`), `:95` (`opacityTex`) | No separate-metalness slot. |
| Basis change | `GltfImport.cpp:277-293` (`toEngine`, `toEngineQuat` = `{z,-x,-y,w}`, `toEngineScale` = `{z,x,y}`); doc `ASSET_IMPORT.md:22-34` | Private members of class `Gltf`; not reusable as is. |
| Normals/winding | `GltfImport.cpp:345-369` (cofactor normal matrix, `reverseWinding = convertAxes != (det<0)`) | Rule to copy. |
| Node walk, instancing | `GltfImport.cpp:1716-1838`: per-mesh `Built{basis,skin,index}` dedupe, translation split into `GltfPlacement` (`:1815-1830`) | Rule to copy. |
| Skeleton | `GltfImport.cpp:656-943`: parent = nearest ancestor in skin (`:697-701`); topmost joints fold the whole ancestor chain (`:732-758`); topological sort (`:764-797`); inverse bind re-derived (`:799-821`); sockets rule (`:823-903`) | Rule to copy; FBX adds bind-pose rest. |
| Clips | `GltfImport.cpp:957-1085`: one `OcTrack` per channel, `skeletonRef` (`:992`), morph weights noted-and-dropped (`:1000`); object clips `:1181-1480` | Skeleton 0 only. |
| Morph | `GltfImport.cpp:320` `note("morph targets")`; `OcMeshData` (`OcMesh.hpp:204-287`) has no morph field | Gap; owned by MORPH_TARGETS. |
| Mesh limits | `kOcMeshInfluences = 4` (`OcMesh.hpp:33`), `joints` is `u16` (`:217`), one UV stream, no colour data (glTF notes `COLOR_0` at `GltfImport.cpp:331`) | Contract for FBX. |
| Skeleton/clip format | `OcBone :17`, `OcSocket :43`, `OcSkeleton :52`, flags `OcAnim.hpp:95-102` (`kOcAnimRootMotion`, `kOcAnimObject`), `OcTrack :105`, `OcCurve :171`, `OcAnimation :199` (`curves :234`) | Curves are named scalar tracks: the morph weight carrier. |
| Root motion consumer | `modules/anim/include/aver/anim/RootMotion.hpp:34-39` (`rootBoneOf`, `clipHasRootMotion`: flag + a track on the root bone); editor checkbox `sandbox/src/AnimEditor.cpp:2501` | Flag is authored by hand today. |
| Material cook | `ImportCook.hpp:54` (`cookImportedMaterials`), `ImportCook.cpp:157` (`mergeOpacityMaps`), `:237` (`collapseIdenticalImages`), `:277` (`capImageSize`), `:351` (entry) | Policy layer where image ops live. |
| Separate-map precedent | `ObjImport.cpp:626` (Phong exponent -> roughness `sqrt(2/(Ns+2))`, metallic 0), `:644-652`; USD opacity channel logic `UsdImport.cpp:1303-1313` | Copy the Phong rule; fix the metal drop. |
| Asset compiler | `tools/AverAssetC.cpp`: `MeshItem :753`, `ScenePlacement :847`, `SceneCamera :863`, `writeSceneLevel :900`, `writeMeshItems :1067`, `cookAndRewriteSlots :1128`, `main :1358`, glTF branch `:1539-1662`, OBJ `:1664`, USD `:1706`; CMake `tests/formats/CMakeLists.txt:343-360` | Dispatch is by extension. JSON line schema is pinned by `AverAssetCTest`; this feature adds no keys. |
| Editor import | `sandbox/src/SandboxContentBrowser.cpp`: `importGltfToDir :21-269` (calls `importGltf(src,res,{},&why)` with default options, no UI), `drawImportModal :2685` (filter `:2701-2705`, collision hint `:2738`), `importAsset :2802` (dispatch `:2837`), `importModel :2920`; summary struct `SandboxApp.hpp:1035`; CLI `--import-gltf` `SandboxMain.cpp:465` | No options UI, no OBJ/USD in the editor, no reimport beyond the Overwrite checkbox. |
| FBX status | `docs/ASSET_IMPORT.md:398-410` ("Not implemented"), `SCRIPTING_API.md:885-889`, `CHANGELOG.md:965`, `templates/FirstPerson/Content/Meshes/SOURCES.md:30` (FBX hand-converted with assimp) | Docs to update. |
| Build | root `CMakeLists.txt:8` `project(... LANGUAGES C CXX)` (C is on), `aver_add_module` (`cmake/AvModule.cmake`), meshoptimizer vendoring pattern `third_party/meshoptimizer/CMakeLists.txt` + `README.aver.md`; licences listed for the editor payload in `scripts/stage-payload.ps1:375-383` | ufbx follows this pattern. |
| Script/graph pattern | provider relay `framework_anim_abi.h` + `AnimAbi.cpp`; C# `FwAnim` in `AnimNative.cs`; graph rows `GraphGameSystemNodeTable.cs:118` (`Reg(...)`), palette mirror `sandbox/src/GraphNodeDefsGameSystems.hpp:208`; provider installs `sandbox/src/SandboxApp.cpp:637`, `Runtime/src/GameApp.cpp:1868` | Pattern for section 6. |

Missing: any FBX reader, a separate-metalness slot, a per-clip skeleton index, importer-set root-motion flags, cameras from model files in the editor, a reimport record, an importer options UI.

## 2. Design

### 2.1 Public API (`modules/formats/include/aver/formats/FbxImport.hpp`)

```cpp
struct FbxImportOptions {
    f32  scale = 100.0f;              // metres -> cm, after ufbx has converted the file to metres
    f32  unitMetersOverride = 0.0f;   // >0: ignore the file's unit, treat 1 file unit as this many metres
    bool convertAxes = true;          // false: skip the axis map (still metres*scale), as GltfImportOptions
    bool generateMissingNormals = true;   // ufbx smooth normals first, flat fallback after
    bool importMeshes = true, importSkeletons = true, importAnimations = true, importMaterials = true;
    bool importBlendShapes = true;
    bool fileColoursAreLinear = false;    // ImportedMaterial::baseColorFactorLinear; OBJ precedent is false
    u32  bakeFps = 30;                    // resample_rate for non-linear curves
    bool reduceKeys = true;               // ufbx key reduction
    enum class RootMotion : u8 { Off, Auto, All } rootMotion = RootMotion::Auto;
    f32  rootMotionMinTravelCm = 10.0f;   // Auto: planar travel of the root bone over the clip
    f32  maxMemoryMiB = 2048.0f;          // passed to ufbx's allocator limits
};
struct FbxCamera { std::string name; Vec3 position{}; f32 yawDeg = 0, pitchDeg = 0, fovDeg = 60; };   // engine space
struct FbxBlendShape {                // one per blend channel, per mesh
    std::string name;                 // channel name
    std::vector<u32> vertices;        // LOD-0 vertex indices of the owning OcMeshData
    std::vector<f32> positionDeltas;  // 3 per entry, engine space, cm, SAME transform as that mesh's positions
    std::vector<f32> normalDeltas;    // 3 per entry or empty
    f32 defaultWeight = 0.0f;         // 0..1
};
struct FbxImportResult : GltfImportResult {           // GltfImport.hpp
    std::vector<std::vector<FbxBlendShape>> blendShapes;   // parallel to meshes
    struct Stats { u32 nodes, meshes, bones, clips, truncatedInfluenceVertices; f32 maxDroppedWeight; u32 sockets; } stats{};
    std::string report;               // one line per decision worth reading (axis/unit source, bind-pose use, dropped data)
};
bool importFbx(const std::string& path, FbxImportResult& out, const FbxImportOptions& = {}, std::string* why = nullptr);
bool importFbxFromMemory(const u8* bytes, usize size, const std::string& baseDir, FbxImportResult& out,
                         const FbxImportOptions& = {}, std::string* why = nullptr);
bool looksLikeFbx(const u8* bytes, usize size);   // "Kaydara FBX Binary  \0" or ASCII FBXHeaderExtension in the first 4 KiB
std::string fbxOptionsToText(const FbxImportOptions&);                     // "scale=100;rootMotion=auto;..."
bool fbxOptionsFromText(std::string_view, FbxImportOptions&, std::string* why);   // unknown key = error naming it
```
Additive edits to the shared base (`GltfImport.hpp`, empty for glTF, so existing behaviour is byte-identical):
```cpp
std::vector<i32> animationSkeleton;   // parallel to `animations`: index into `skeletons`; empty = all 0 (glTF)
std::vector<ImportedCamera> cameras;  // engine space; ImportedCamera == FbxCamera's fields, defined in a new header ImportedCamera.hpp
```
(`ImportedCamera` lives in a new tiny header `aver/formats/ImportedCamera.hpp`; `FbxCamera` is an alias, so USD can adopt it later.)

`importFbx` refuses non-FBX bytes (`looksLikeFbx`) BEFORE calling ufbx, because ufbx also reads OBJ and `ObjImport` owns that. Failures carry ufbx's own text: `ufbx_format_error(buf, n, &err)` goes into `why`.

### 2.2 Loading (`FbxImport.cpp`)

```cpp
ufbx_load_opts lo = {};                                   // zero init; designated init is not portable across versions
lo.target_axes        = o.convertAxes ? ufbx_axes_right_handed_y_up : ufbx_coordinate_axes{};   // UNKNOWN members = leave as authored
lo.target_unit_meters = o.unitMetersOverride > 0 ? 0 : 1; // 0 = no unit conversion; we multiply by the override ourselves
lo.space_conversion          = UFBX_SPACE_CONVERSION_ADJUST_TRANSFORMS;
lo.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_MODIFY_GEOMETRY;     // falls back to helper nodes when instanced
lo.inherit_mode_handling     = UFBX_INHERIT_MODE_HANDLING_COMPENSATE;                  // helper nodes only where it cannot
lo.pivot_handling            = UFBX_PIVOT_HANDLING_ADJUST_TO_ROTATION_PIVOT;
lo.generate_missing_normals  = o.generateMissingNormals;
lo.normalize_normals         = true;
lo.load_external_files       = false;                    // WE read textures (path policy below); ufbx must not open arbitrary paths
lo.ignore_embedded           = false;
lo.evaluate_skinning         = false;                    // we skin nothing on the CPU
lo.temp_allocator.memory_limit = lo.result_allocator.memory_limit = size_t(o.maxMemoryMiB) << 20;   // verify field names in the pinned header
lo.node_depth_limit          = 512;
lo.filename = ufbx_string{path, len};
```
Why each (these are the "game-engine" settings ufbx documents):
- `ADJUST_TRANSFORMS` puts the axis/unit change into node transforms and animation, with no extra root node; world matrices and baked keys then come out already in the target frame, which is what makes the armature-root-carries-a-100x-rotation failure that `GltfImport.cpp:722-731` documents impossible here.
- `MODIFY_GEOMETRY` bakes FBX geometric transforms (pivot-offset geometry) into vertices, blend offsets and skin matrices; nodes where that is impossible get a helper node (`is_geometry_transform_helper`). `COMPENSATE` does the same for non-default inherit modes (Maya segment-scale-compensate; helper = `is_scale_helper`). Helper nodes carry generated names and sit between real nodes: the skeleton builder (2.4) treats them as ordinary chain members.
- `ufbx_real` stays double (do not define `UFBX_REAL_IS_FLOAT`).
- Memory is bounded and errors are reported, never crashed on: a corrupt file yields `ufbx_error`, a hostile one hits the memory/depth limits.

Matrix convention: `ufbx_matrix` is 4x3 column-major (`cols[0..3]`, `cols[3]` = translation). The engine is row-vector row-major with translation in row 3, so rows 0-2 are `cols[0..2]` and row 3 is `cols[3]`: a straight copy, no transpose. Use `ufbx_matrix_mul`, `ufbx_matrix_invert`, `ufbx_matrix_to_transform` (quaternion `x,y,z,w`) instead of writing a decomposition (GltfImport's `decomposeTrs` stays private to it).

Colour/scale helpers (one place, `FbxBasis`): `toEngine(Vec3)`, `toEngineQuat`, `toEngineScale` are bit-for-bit the glTF formulas (`GltfImport.cpp:277-293`) applied to the ufbx frame, times `scale * unitMetersOverride-or-1`. A test pins FBX == glTF output for the same cube (section 5). Do NOT edit `GltfImport.cpp` to share them in this feature (it is a stable, heavily tested file); a follow-up can move both onto `FbxBasis`.

### 2.3 Meshes, instancing, materials slots

Walk `scene->nodes` from the root (iterative, depth already limited by ufbx). For each node with `node->mesh` and `node->visible`:
- `W = node->geometry_to_world` (includes the geometric transform when `PRESERVE`d; baked otherwise). `basis` = rows 0-2 of `W` with translation zeroed; `det` = its determinant. `reverseWinding = convertAxes != (det < 0)` exactly as `GltfImport.cpp:369`; normals use the cofactor matrix scaled by `sign(det)/max` exactly as `:345-365`. UV: `v -> 1 - v` (FBX origin is bottom-left, like OBJ/USD).
- **Skinned mesh** (`mesh->skin_deformers` non-empty and `importSkeletons`): do not use `W`. Every vertex goes through the `geometry_to_world` of the cluster that weights it most (`skin->clusters[k].geometry_to_world`, the bind-pose matrix; identical for all clusters of a valid file, and a mismatch beyond 1e-3 is reported). Position and normal (inverse-transpose) are transformed by it, translation INCLUDED, no placement is emitted (`position = 0`). Rationale in decision 5: the skeleton's model space is the converted FBX world and the root bone's keys are folded into it (2.5), so nothing is added twice.
- **Static mesh**: rotation+scale baked, translation -> `GltfPlacement.position`; `pl.name` = node name. One `meshes[]` entry per distinct key `{mesh element, basis (relative 1e-5, as `sameBasis` `GltfImport.cpp:1720`), node->materials set}`; the first keeps the element's slot, others are `<name>_2`... copies with names made unique (`GltfImport.cpp:1730-1740`). FBX instancing (`ufbx_mesh::instances`) therefore yields N placements of one mesh when the basis matches. A node-level material override (`node->materials` differs from `mesh->materials`) makes a separate copy.
- A mesh node parented under a bone, with no skin, is a static placement at its rest world position and is reported ("rigidly attached to bone X: imported as a static placement"). Same limitation as glTF.
- Hidden nodes (`visible == false`) and `is_*_helper` nodes without geometry are skipped.

Per mesh, build vertices once:
1. For each face, `ufbx_triangulate_face(tri, mesh->max_face_triangles*3, mesh, face)` (ear clipping; concave n-gons are fine, unlike `ObjImport`). Faces are grouped by `mesh->material_parts[p].face_indices` so each material part is one contiguous index range = one `OcMeshSubmesh` (`materialSlot` by deduped material name; a face-less part is skipped).
2. Unique vertices with `ufbx_generate_indices` over four streams: position (`vertex_position`), normal (`vertex_normal`), UV 0 (`vertex_uv`), and the control-point index (`mesh->vertex_indices`, so two control points at the same position but with different skin weights or blend offsets never merge). A hard edge therefore stays split, a smooth surface welds. Output vertex -> control point is kept for skin and blend-shape lookup.
3. Only UV set 0 is read (a second set is reported); vertex colours are reported ("vertex colour", there is no colour stream, `OcMesh.hpp:204-218`); missing UVs are `0,0`; missing normals follow `generateMissingNormals`.
4. `m.valid()` must hold; a mesh with no triangles is skipped and reported.

Material slot name = `ImportedMaterial::name` = the ufbx material's name, else `Material_<element_id>`; duplicates among distinct elements get `_2`, `_3` (the slot string and the material name must be the same function of the element, `GltfImport.cpp:172-180`). A face with no material uses slot `M_Default` like `:487`.

Cameras: each `scene->cameras` node becomes an `ImportedCamera` (position, yaw/pitch from the node's world forward through the engine map, `field_of_view_deg`). Lights: counted and reported (`"lights"`), not imported.

### 2.4 Materials and textures (`FbxMaterials.cpp`)

From `ufbx_material::pbr` (ufbx synthesises it from Lambert/Phong, Arnold, 3ds Max physical, glTF-material and OSL shaders, so one reader covers them all):

| `ImportedMaterial` | source | notes |
|---|---|---|
| `baseColorFactor` | `pbr.base_color.value_vec4` x `pbr.base_factor`, alpha from `pbr.opacity` | `baseColorFactorLinear = options.fileColoursAreLinear` |
| `baseColorTex` | `pbr.base_color.texture` | |
| `roughnessFactor`, `metalRoughTex` | `pbr.roughness` | with a texture the factor is 1 (`UsdImport.cpp:1276`); without `has_value` on a Lambert/Phong shader: `sqrt(2/(Ns+2))` from `fbx.specular_exponent` (the OBJ rule, `ObjImport.cpp:626`) |
| `metallicFactor`, new `metallicTex` | `pbr.metalness` | **without a value the factor is 0, not glTF's 1** (same reason as `ObjImport.cpp:619-625`) |
| `normalTex`, `normalScale` | `pbr.normal_map` (`fbx.bump` height maps are NOT converted; reported) | |
| `emissiveFactor`, `emissiveTex` | `pbr.emission_color` x `pbr.emission_factor` | |
| `occlusionTex` | `pbr.ambient_occlusion` | |
| `opacityTex`, `alphaMode` | `pbr.opacity`: texture -> `opacityTex`, `alphaMode = MASK` (the USD rule `UsdImport.cpp:1313`); value < 1 -> `BLEND`; same image as base colour read through alpha -> clear `opacityTex` | |
| `doubleSided` | not in FBX; false | |
| `subsurfaceWeight` etc. | `pbr.subsurface_*` (`factor`, `color`, `radius`) when `has_value` | |

Anything else (sheen, coat, transmission, thin film, anisotropy, displacement, UV transforms `texture->has_uv_transform`, layered/procedural textures) is named once in `unsupported`. A `ufbx_texture` that is not `FILE` resolves through its first `file_textures` entry.

**Image bytes** (`ImportedImage`, encoded verbatim, `ImportedImage.hpp:30`):
1. Embedded: `texture->content` or `texture->video->content` (blob) -> `bytes`; `ext` from the filename; `suggestedName` = sanitised leaf stem (never a path).
2. External, searched in order: `absolute_filename`, `relative_filename` against the FBX folder, the leaf name in the FBX folder, `<fbxdir>/textures/<leaf>`, `<fbxdir>/<stem>.fbm/<leaf>` (the folder Autodesk tools extract embedded media to). Paths containing `..` that resolve outside the FBX folder's parent are refused and reported (the file is untrusted input). A missing file leaves `ok = false` and the slot UNSET, never a dangling reference (`ImportedMaterial.hpp:38-40`).
3. Formats `stb_image` cannot decode (`.tif`, `.exr`, some `.psd`) fail in the cook with a warning naming them; nothing to do here.
Dedup of identical bytes is already done by `collapseIdenticalImages` (`ImportCook.cpp:237`).

**Pack step (shared file, small).** `ImportedMaterial` gains `ImportedTexture metallicTex;` (default empty, never set by any other importer). In `ImportCook.cpp`, `packMetalRough(images, materials)` runs right after `mergeOpacityMaps` (`:363`): when `metallicTex` is set and is not the same image+channel the pair `(metalRoughTex, 'g')/(metallicTex, 'b')` already means, decode both (`aver/formats/Texture.hpp`), write a new PNG whose G = roughness channel and B = metalness channel (R = 255, A = 255; a missing side is a constant plane from the factor), append it to `images`, point `metalRoughTex` at it, set `roughnessFactor = metallicFactor = 1` when their sources were textures, clear `metallicTex`. Sizes differ -> resample the smaller by nearest to the larger. Result is a `Data` role image (`imageRoles`, `ImportCook.cpp:34`). Unit-tested without a GPU (section 5).

### 2.5 Skeleton, skin, sockets (`FbxSkeleton.cpp`)

**Bones.** `B` = every `bone_node` of every `skin_cluster` used by an imported mesh, plus every node on the parent path between two members of `B`, plus descendants of members that are `ufbx_bone`-typed (`node->bone != null`, i.e. end bones/helper limbs). The path rule is what lets scale/geometry helper nodes join the chain. Bones group into **skeletons by their topmost member** (one `OcSkeleton` per distinct topmost node); a skin whose clusters span two groups uses the first and is reported. Order: parents before children (stable topological sort, as `GltfImport.cpp:764-797`); `rootBone = 0`; `skeletonNames[i]` = topmost bone name; name collisions inside a skeleton are made unique (`_2`) because tracks bind by index but sockets/retargeting bind by name.

**Rest pose = bind pose.** World rest of a bone: the owning cluster's `bind_to_world` when it has one, else `node->node_to_world` (a non-weighted chain node or an end bone). Local rest = `rest_world[bone] * inverse(rest_world[parent])` through ufbx matrix math, `ufbx_matrix_to_transform`, then the shared `toEngine*` map. A topmost bone has no bone parent, so its "local" IS its model-space (= converted FBX world) transform, the same fold `GltfImport.cpp:732-758` does. Shear/non-uniform-scale residue that a TRS cannot hold is reported with the recovered TRS kept (`:748-753`). `inverseBind = inverse(model rest)` re-derived exactly as `:809-821`; ufbx's own `geometry_to_bone` is used only for the mismatch report.

**Skin weights.** Per output vertex, from `skin->vertices[cp]` and `skin->weights[...]`, mapping `cluster_index -> skin->clusters[i].bone_node -> bone index`. Keep the 4 largest, renormalise to 1, `u16` indices. Count vertices that had more than 4 and the largest dropped weight (`Stats`); a vertex with zero weights binds to bone 0 with weight 1 (the back-fill rule, `GltfImport.cpp:374-379`, `:411-417`). `meshSkinIndex[i]` = skeleton index; all meshes of one skeleton share one index space, so `mergeAll`'s same-skin merge (`AverAssetC.cpp:766`) stays valid.

**Sockets** = the glTF four-rule port: a non-bone node whose parent is a bone, with no mesh/camera/light/skin, no bone descendant, name made unique with a numeric suffix and a note. Offset = the node's local TRS through the same map as bones. Light/camera children of bones are NOT sockets (rule 3). `ufbx_bone`-typed leaf end sites are bones (they are in `B`), not sockets.

**Limits reported, not enforced**: more than 256 bones (`kSkinMaxBones`, `render.skin/include/aver/render/SkinningPass.hpp:21`); a skin with more than 65535 bones; dual-quaternion skinning (`skin->skinning_method`): imported as linear and named in `unsupported`.

### 2.6 Animation (`FbxAnimation.cpp`)

For each `scene->anim_stacks[s]` (skip when `importAnimations` is off or the stack has zero duration):
```cpp
ufbx_bake_opts bo = {};
bo.trim_start_time  = true;                         // clip starts at 0
bo.resample_rate    = o.bakeFps;   bo.maximum_sample_rate = o.bakeFps * 4.0;
bo.key_reduction_enabled = o.reduceKeys;
bo.bake_transform_props  = false;                   // node keys come as TRS keys, not as property curves
ufbx_baked_anim* ba = ufbx_bake_anim(scene, stack->anim, &bo, &err);   // freed with ufbx_free_baked_anim
```
`ba->playback_duration` is the clip duration. Step (constant-interpolated) curves are handled by ufbx's default step handling (a near-duplicate key), so every produced track is `OcInterp::Linear`; no key flags need reading. Rotation keys are quaternions (`ufbx_baked_quat`), already unrolled (no Euler); successive quaternions are sign-flipped to the shortest arc before writing (the sampler slerps).

Per `ufbx_baked_node`: find the engine bone for `element_id`/`typed_id`; for each channel (translation, rotation, scale) with **more than one key, or a constant that differs from the bone's rest value by > 1e-4 (position cm, rotation dot, scale)**, emit one `OcTrack` on that bone/channel. A constant equal to rest, or a bone the stack does not touch, emits nothing: the pose sampler leaves it at rest. Values go through the same `toEngine*` map. **Topmost bones**: each key is premultiplied by the static world matrix `A` of the bone's non-bone parent (row-vector `L(t) * A`, through ufbx matrices), then decomposed, so the keys live in the same model space as the folded rest (an animated ancestor is sampled at t=0 and reported). Clip `skeletonRef` = that skeleton's stem (set by the writers from `animationSkeleton`); `duration` = `ba->playback_duration`; `storage = Keyframed`. Clips are produced for every skeleton the stack animates; name = stack name (`"Take 001"`, `"mixamo.com"` kept, sanitised and de-duplicated case-insensitively as `AverAssetC.cpp:1585`); a stack animating two skeletons yields `<stack>` and `<stack>_<skeleton>`.

**Root motion flag** (`o.rootMotion`): `Off` never sets; `All` sets `kOcAnimRootMotion` on every skeletal clip that has a track on the skeleton's root bone (`rootBoneOf`, `RootMotion.hpp:34`); `Auto` sets it when the root bone's planar (X,Y) travel from first to last key exceeds `rootMotionMinTravelCm`. Import never edits keys (no root locking at import; the runtime's `lockRoot` does that). The importer reports the root bone name and measured travel per clip in `report`, because the extraction assumes the root heading is a rotation about +Z (`RootMotion.hpp:6`).

**Object clips** (parity with `GltfImport.cpp:1181-1480`): a node that is not a bone and has a mesh (or sits under an animated node) and whose world transform changes gets a `kOcAnimObject | kOcAnimLoop` clip, one track on bone 0 holding `A(t) = W(t) * Q0^-1`, where `Q0` is the rotation*scale baked into that placement's mesh copy. `W(t)` is the node's world at each sample time: compose `ufbx_evaluate_transform(stack->anim, ancestor, t)` up the chain (matrix product through ufbx, then the shared map), at the union of the baked key times of the node and its ancestors plus a `bakeFps` grid over the clip, then drop interior keys that linear interpolation reproduces within 1e-4 cm / 1e-6 rad. Zero scale is written as a zero scale key, shear is reported and recovered (`decomposeKey` rules), a skinned mesh node is skipped and reported. Names/attachment follow the glTF rules (`objectAnimationNames`, `objectAnimationPlacement`, first clip wins).

**Blend-shape weights**: for each blend channel, `ufbx_baked_element` props named `DeformPercent` (percent, divide by 100 and clamp to 0..1) become one `OcCurve{name = "morph." + channelName, interp = Linear}` appended to `clip.curves` of the clip(s) whose skeleton owns that mesh (a mesh with no skeleton: its own clip with no tracks, only curves, skeletonRef empty). The prefix is the single constant `kFbxMorphCurvePrefix`. Emitted only when the morph adapter is active (2.7).

### 2.7 Blend shapes -> the morph feature

`mesh->blend_deformers[*]->channels[*]`: each channel's keyframes `{shape, target_weight}` (`target_weight` 0..1; the keyframe with the largest value is the full shape). Per channel take that keyframe's `ufbx_blend_shape`: for each `i < num_offsets`, control point `offset_vertices[i]` with `position_offsets[i]` and `normal_offsets[i]`; every output vertex whose control point matches (kept in 2.3 step 2) gets the delta. Deltas are DIRECTIONS: transformed by the vertex's linear part (the same matrix as the positions without translation, normals by inverse-transpose, then the unit scale), zero deltas (|d| < 1e-6 cm) are not stored. In-between keyframes (target_weight strictly between 0 and the max) are dropped and reported. `defaultWeight` = `channel->weight`.

**Interface I need from MORPH_TARGETS.md** (the morph feature defines it; names here are my assumption, only the capability is required):
1. `OcMeshData` carries sparse targets: `struct OcMorphTarget { std::string name; std::vector<u32> vertices; std::vector<f32> positionDeltas /*3 per*/; std::vector<f32> normalDeltas /*3 per, may be empty*/; f32 defaultWeight; }` as `std::vector<OcMorphTarget> morphTargets`, indices into LOD 0 vertices, engine space, cm; written/read by `writeOcMesh`/`parseOcMesh` as a new optional chunk, absent chunk = no targets, old files byte-identical.
2. `#define AVER_OCMESH_HAS_MORPH 1` in `OcMesh.hpp` when (1) exists. This is the only coupling: `FbxImport.cpp` compiles its adapter `attachBlendShapes(OcMeshData&, const std::vector<FbxBlendShape>&)` under `#if defined(AVER_OCMESH_HAS_MORPH)`; otherwise shapes go to `unsupported`.
3. A float weight channel the runtime reads from clip curves by name. If MORPH_TARGETS picks another curve name, change `kFbxMorphCurvePrefix` only.
4. Writers (`AverAssetC` `mergeAll`, `writeMeshItems` LOD/Trifactor simplification, the editor) must not corrupt target indices: until the morph feature provides a remap, THIS feature's rule is that a mesh with `morphTargets` is never merged by `--merge` and never gets LOD generation (a warning names it).
Optional, not required in v1: in-between shapes (a channel as an ordered list of `(weight, target)`).

### 2.8 Writers (shared refactor)

- **Editor.** `importGltfToDir` (`SandboxContentBrowser.cpp:21-269`): the code after the `importGltf` call (material cook, mesh files, object clips, scene level, skeletons, clips) moves verbatim into `static bool writeImportedModel(fmt::GltfImportResult&, const std::string& src, destDir, contentDir, overwrite, GltfImportSummary&, std::string* why)`. `importGltfToDir` becomes parse + call. A new `importFbxToDir(src, destDir, contentDir, overwrite, const fmt::FbxImportOptions&, summary, why)` is parse + the same call, with three additions inside the shared writer, all inert for glTF: clip `skeletonRef` and skeleton file stems from `animationSkeleton`; `OcWorldData::hasCamera` from `cameras[0]`; the `.ocimport` record (2.9). Blend shapes ride inside the `.ocmesh`.
- **AverAssetC.** `.fbx` is an extension branch next to `.gltf/.glb` (`:1539`). Lines `:1547-1661` move into `static int convertImportedModel(const std::string& input, fmt::GltfImportResult& res, ...)` called by both; `SceneCamera` is filled from `cameras[0]`; the clip's `skeletonRef` uses the skeleton stem `base + (i == 0 ? "" : std::to_string(i))` (`:1634`). New flags, all FBX-only and ignored otherwise: `--scale <f>`, `--unit-meters <f>`, `--root-motion off|auto|all`, `--bake-fps <n>`, `--no-animations`, `--no-blend-shapes`; `--import-report <file>` is reused (writes `FbxImportResult::report` + `unsupported`). The usage text (`:1359`) gains a line. No JSON key changes; `AverAssetCTest` needs no edit.
- Both writers keep `res.unsupported` as warnings (`AVER_WARN("unsupported: ...")`, `:1547`).

### 2.9 Editor UX and reimport

- **Import dialog** (`drawImportModal :2685`): filter strings (`:2701-2705`) and the collision hint (`:2738`) add `.fbx`. After a `.fbx` path is typed/picked, an **FBX options** section appears: Scale (cm per metre, default 100), Unit override (m, 0 = file), checkboxes Meshes / Skeletons / Animations / Materials+textures / Blend shapes, Root motion combo (Off / Auto / All), Bake FPS, Reduce keys, "File colours are linear". A read-only summary line from a cheap pre-parse (`ufbx_load_memory` with `skip_skin_vertices`, runs on a worker thread; shows axes/unit/meshes/bones/stacks/embedded textures) is optional polish; the first phase ships options only. State is a static struct reset on `IsWindowAppearing()`, like `s_overwrite`.
- **Drag-and-drop / `importAsset`** (`:2802`, dispatch `:2837`): `.fbx` -> `importFbxModel()` with defaults (same as glTF: no dialog); the dialog path passes its options.
- **Notifications** reuse `notifyOutcome`; the `unsupported` list and `report` go to the Output Log with `[Import]` prefix, and the count of dropped features appears in the success body ("3 notes, see Output Log").
- **CLI** `--import-fbx <src> <destDir> [options-text]` beside `--import-gltf` (`SandboxMain.cpp:465`); options text = `fbxOptionsToText` format. Used by MCP/CI and the manual checks.
- **`.ocimport` record** (`ImportRecord.hpp/.cpp`, base `Aver.Formats`, line text, unknown lines preserved): `<destDir>/<stem>.ocimport`
  ```
  OCIMPORT 1
  FORMAT fbx
  SOURCE Source/Hero.fbx          # project-relative when inside the project, else absolute
  SOURCESIZE 1843201
  SOURCEMTIME 133901234567890000
  OPTIONS scale=100;rootMotion=auto;...
  OUTPUT mesh Hero.ocmesh
  OUTPUT skeleton Hero.ocskel
  OUTPUT anim Hero_Walk.ocanim
  OUTPUT material Mat_Hero_Body.ocmat
  ```
  Written (replacing) by every successful FBX import; glTF does not write one in v1. The Content Browser shows **Reimport** and **Import options...** on a `.ocimport` entry and on any file named by an `OUTPUT` line of a record in its folder (record list cached per folder in `cbInvalidate`). Reimport = `importFbxToDir(..., overwrite = true)` with the stored options; old files go to the recycle bin through the existing overwrite path (`editor::moveToRecycleBin`). A record whose source is missing shows a disabled action with the path. The record is plain Content (packaging already drops `**/Source/`, `docs/PACKAGING.md`; add `*.ocimport` to the game stager's exclusion list).
- Nothing in this feature adds an asset-editor tab.

### 2.10 Script surface (C# + Aver Node)

Import is an editor-time act, so the runtime surface is a thin relay, not a gameplay system:
- **`modules/framework/include/aver/framework/framework_import_abi.h`** (own `AVER_FW_IMPORT_ABI_VERSION 1`, pattern of `framework_anim_abi.h`; `AVER_FW_ABI_VERSION_MINOR` untouched):
  ```c
  typedef int32_t (AVER_FW_CALL* aver_fw_import_fn)(const char* utf8Source, const char* utf8DestFolder, const char* utf8Options,
                                                     int32_t overwrite, char* outMessage, int32_t messageCap, int32_t* outCounts5, void* user);
  AVER_FW_ABI int32_t aver_fw_import_abi_version(void);
  AVER_FW_ABI int32_t aver_fw_set_import_provider(aver_fw_import_fn fn, void* user);   /* null clears; always 1 */
  AVER_FW_ABI int32_t aver_fw_import_available(void);                                  /* 1 when a provider is installed */
  AVER_FW_ABI int32_t aver_fw_import_model(const char*, const char*, const char*, int32_t overwrite,
                                           char* outMessage, int32_t cap, int32_t* outCounts5);   /* 1 ok, 0 failed or unavailable */
  ```
  `outCounts5` = meshes, skeletons, clips, materials, textures written. No provider -> returns 0, writes "model import is not available in this host", zeroes counts. `src/ImportAbi.cpp` is one more line in the `Aver.Framework` source list.
- **Provider** lives in the editor only: `sandbox/src/ModelImportProvider.cpp` (`modelImportProvider`) resolves `utf8DestFolder` as Content-relative (refuses `..`, absolute paths and engine content), `utf8Source` as absolute or project-relative, parses `utf8Options` with `fbxOptionsFromText`, dispatches by extension to `importFbxToDir` / `importGltfToDir`, writes the record, calls `cbInvalidate` + `wantMeshReload_`, runs synchronously on the calling thread. Installed once in `SandboxApp.cpp` next to the other `aver_fw_set_*_provider` lines (grep `aver_fw_set_anim_provider`, `:637`). `Runtime/src/GameApp.cpp` is NOT touched: a packaged game never has a provider.
- **C#** (`scripting/csharp/Aver.Framework`): `ModelImportNative.cs` (`FwImport` DllImport class, pattern of `AnimNative.cs`) and `ModelImport.cs`:
  ```csharp
  public readonly record struct ModelImportResult(bool Ok, string Message, int Meshes, int Skeletons, int Clips, int Materials, int Textures);
  public sealed class ModelImportOptions { float Scale = 100; float UnitMeters = 0; bool Meshes = true, Skeletons = true, Animations = true, Materials = true, BlendShapes = true;
                                           RootMotionImport RootMotion = RootMotionImport.Auto; int BakeFps = 30; bool ReduceKeys = true; internal string ToText(); }
  public enum RootMotionImport { Off = 0, Auto = 1, All = 2 }
  public static class ModelImport {
      public static bool IsAvailable { get; }
      public static ModelImportResult Import(string source, string destFolder, ModelImportOptions? options = null, bool overwrite = false);
  }
  public static class ModelImportGraph {      // what the graph rows call
      public static bool ImportModelForGraph(string source, string dest, bool overwrite, float scale, out int meshes, out int skeletons, out int clips);
      public static bool IsModelImportAvailableForGraph();
  }
  ```
- **Aver Nodes** (category `Assets`), no `Aver.Scene`/`OcGraphParser.cs` edit (generic table rows only):
  - `GraphGameSystemNodeTable.cs`, appended before the `.Concat(...)` chain:
    `Reg("importmodel", GenericKind.Exec, typeof(ModelImportGraph), "ImportModelForGraph", "overwrite:bool,scale:float=100", "success:bool,meshes:int,skeletons:int,clips:int", "@source,@dest,p:overwrite,p:scale,>meshes,>skeletons,>clips", "success"),`
    `Reg("ismodelimportavailable", GenericKind.Pure, typeof(ModelImportGraph), "IsModelImportAvailableForGraph", "", "available:bool", "", "available"),`
  - `sandbox/src/GraphNodeDefsGameSystems.hpp`, palette mirror pin for pin: `ImportModel` (pins `exec, overwrite, scale=100, then, success, meshes, skeletons, clips`; attrs `source`, `dest`) and `IsModelImportAvailable` (`available`).
  - Docs: `docs/AVER_NODE_NODES.md` gets an "Assets" section (two rows, Path **X** for `ImportModel` since it is a side effect, **D** for the pure read; node counts in the header re-derived by the counting method it states); `docs/SCRIPTING_API.md` section 19 bullet "Import beyond glTF" (`:885-889`) is rewritten (glTF and FBX convert; everything else still copies) and a short "Model import (editor hosts)" subsection added; `docs/VISUAL_SCRIPTING.md` gets one pointer line.

## 3. FILE OWNERSHIP

**Create (only this feature touches them)**
- `third_party/ufbx/ufbx.h`, `third_party/ufbx/ufbx.c`, `third_party/ufbx/LICENSE.txt` (ufbx MIT text), `third_party/ufbx/README.aver.md` (tag, sha256 of both files, what was taken, how to re-vendor), `third_party/ufbx/CMakeLists.txt`
- `modules/formats/include/aver/formats/FbxImport.hpp`, `ImportedCamera.hpp`, `ImportRecord.hpp`
- `modules/formats/src/FbxImport.cpp` (load, scene walk, meshes), `FbxMaterials.cpp`, `FbxSkeleton.cpp`, `FbxAnimation.cpp`, `FbxInternal.hpp` (private shared structs, `FbxBasis`), `ImportRecord.cpp`
- `modules/framework/include/aver/framework/framework_import_abi.h`, `modules/framework/src/ImportAbi.cpp`
- `scripting/csharp/Aver.Framework/ModelImportNative.cs`, `ModelImport.cs`
- `sandbox/src/ModelImportDialog.hpp`, `ModelImportDialog.cpp` (options panel, `importFbxToDir`, reimport actions), `sandbox/src/ModelImportProvider.cpp`
- Tests: `tests/formats/src/FbxImportTest.cpp`, `FbxFixture.hpp`, `FbxMaterialPackTest.cpp`, `ImportRecordTest.cpp`; `tests/framework_import/{CMakeLists.txt,src/FrameworkImportAbiTest.cpp}`; `tests/editor/src/ModelImportDialogTest.cpp`; `scripting/csharp/Aver.Graph.Tests/ModelImportNodeTests.cs`
- `docs/FBX_IMPORT.md` (user-facing: options, what maps to what, what is dropped; short, derived from sections 2.1-2.9)

**Modify, owned by this feature**
- `modules/formats/CMakeLists.txt`: new target `Aver.Formats.Fbx` (below the Material target at `:122`), `ImportRecord.cpp` added to the base source list
- `modules/formats/include/aver/formats/ImportedMaterial.hpp` (`metallicTex`), `modules/formats/src/ImportCook.cpp` (`packMetalRough` + one call at `:363`)
- `tools/AverAssetC.cpp` (the `.fbx` branch, the `convertImportedModel` extraction, flags, usage)
- `sandbox/src/SandboxContentBrowser.cpp` (writer extraction, dialog, dispatch, reimport menu), `sandbox/src/SandboxMain.cpp` (`--import-fbx`), `sandbox/src/SandboxApp.hpp` (declarations only)
- `tests/formats/CMakeLists.txt` (test targets + `AverAssetC` links `Aver.Formats.Fbx` and defines `AVER_HAVE_FBX=1`)
- `docs/ASSET_IMPORT.md` (replace the FBX section `:398-410`; fix the "dependency-free" claim at `:6-7`; add the ufbx note to the "Bringing in third-party assets" steps; ORM packing note under materials), `docs/formats/FORMAT_SPECS.md` section 16 licence table (add ufbx row) if that table exists in the pinned doc

**SHARED files (small, additive; another wave-9 feature may touch the same file; keep both blocks)**
- `modules/formats/include/aver/formats/GltfImport.hpp`: two fields on `GltfImportResult` (`animationSkeleton`, `cameras`). Also touched by: nobody known, but the morph feature may touch `OcMesh.hpp` right next to it.
- `modules/formats/include/aver/formats/OcMesh.hpp`: NOT edited here; the morph feature owns the target struct and the `AVER_OCMESH_HAS_MORPH` macro (section 2.7).
- `CMakeLists.txt` (root): `add_subdirectory(third_party/ufbx)` guarded by `EXISTS`, next to the meshoptimizer block (`:160-165`); `add_subdirectory(tests/framework_import)` beside `tests/framework_anim` (`:577`).
- `modules/framework/CMakeLists.txt`: one source line (`src/ImportAbi.cpp`).
- `sandbox/CMakeLists.txt`: `ModelImportDialog.cpp`, `ModelImportProvider.cpp`, and `target_link_libraries(Sandbox PRIVATE Aver.Formats.Fbx)` under `if(TARGET Aver.Formats.Fbx)` (the Roslyn/Audio pattern, `:165-176`); `tests/editor/CMakeLists.txt`: the new test.
- `sandbox/src/SandboxApp.cpp`: one `aver_fw_set_import_provider(...)` line + include (next to `:637`).
- `scripting/csharp/Aver.Graph/GraphGameSystemNodeTable.cs` and `sandbox/src/GraphNodeDefsGameSystems.hpp`: two rows each, appended (other features append here too; whichever lands second keeps both).
- `docs/AVER_NODE_NODES.md`, `docs/SCRIPTING_API.md`, `docs/VISUAL_SCRIPTING.md`, `README.md` third-party list, `scripts/stage-payload.ps1` (one `@{ Name = 'ufbx'; Licence = 'MIT'; File = 'third_party\ufbx\LICENSE.txt' }` row in the licence list near `:381`; NOT `scripts/stage-game.ps1`, the runtime does not contain ufbx), `scripts/game.allowlist` (exclude `*.ocimport` if the stager does not already drop it), `CHANGELOG.md`.
- The launcher (separate repository) must add `.fbx` to its Aver Exchange file filter; record in the CHANGELOG entry, not editable here.

**Do not edit**: `GltfImport.cpp` (behaviour-neutral, see 2.2), `ObjImport*`, `UsdImport*`, `framework_abi.h`, `OcAnim*` formats, `modules/anim` (root motion consumption is unchanged), any `.ocmat` writer (the pack step produces an ordinary image).

## 4. Phased implementation steps

Each phase ends green (`ctest -R "Fbx|GltfTest|ImportTest|AverAssetC"`), with the glTF/OBJ/USD suites unchanged.

0. **Vendor ufbx.** The owner/main loop downloads the pinned release tag (subagents cannot vendor; record tag and sha256 in `README.aver.md`), commits `third_party/ufbx/*`, CMake `ufbx` static lib (`add_library(ufbx STATIC ufbx.c)`, C language, `/w` on MSVC, `FOLDER third_party`, include dir PUBLIC). Add the `Aver.Formats.Fbx` target with a stub `importFbx` that always fails ("built without ufbx" when `AVER_HAVE_UFBX=0`). Licence rows. Check `scripts/module-guard-audit.py` and `scripts/module-catalog.py --check`.
1. **Static meshes.** Loader options, `looksLikeFbx`, `FbxBasis`, scene walk, triangulation + `ufbx_generate_indices`, submeshes/slot names, placements, instancing dedupe, normals/winding/UV. Fixture builder (`FbxFixture.hpp`). Tests: axes/units/winding/mirror/instancing/hard edges/concave n-gon. AverAssetC `.fbx` branch + the `convertImportedModel` extraction (glTF output must be byte-identical before/after; compare `.ocmesh`/`.ocskel`/`.ocanim` of a fixture glb).
2. **Materials and textures.** `FbxMaterials.cpp`, image discovery, embedded bytes, `metallicTex` + `packMetalRough`. Tests incl. the pack unit test.
3. **Skeleton and skin.** Bones set/skeleton grouping, bind-pose rest, inverse bind, weights/limit/normalise, sockets, helper nodes, reports. Tests.
4. **Animation.** Bake, per-channel tracks, topmost-bone fold, clips per skeleton, `animationSkeleton`, root-motion flag, object clips. Tests.
5. **Blend shapes.** Extraction into `FbxBlendShape` + the `AVER_OCMESH_HAS_MORPH`-gated adapter + `morph.*` curves; without the macro, only the `unsupported` entry. This phase may land before or after MORPH_TARGETS; the gated code is tested through a fake `attach` seam (the test calls `extractBlendShapes` directly).
6. **Editor writer refactor + FBX in the editor.** Extract `writeImportedModel` (verify glTF re-import is byte-identical via `--import-gltf` before/after on a fixture), `importFbxToDir`, dialog options, dispatch, CLI, `ImportRecord`, Reimport menu, cameras into `OcWorldData`.
7. **Script surface.** ABI header + relay + provider + C# + graph rows + palette + docs. Tests.
8. **Docs, CHANGELOG, payload licence row, final sweep** (`ASSET_IMPORT.md`, `SCRIPTING_API.md` stale bullet, `FORMAT_SPECS.md` licences, `scripts/docs-check.py`).

## 5. Tests

All no-GPU, `*Test` executables registered automatically by `aver_register_ctest_in` (add only `add_executable`; do NOT add `add_test`, `FAIL_REGULAR_EXPRESSION "FAIL  |FAILED ==="` is applied centrally). Fixtures are generated in code as **ASCII FBX 7.4** text by `FbxFixture.hpp` (a builder: `axes(up,front,coord)`, `unit(scaleFactor)`, `mesh(name, verts, polys with ~last-index negative marker, normals, uvs, materialIds)`, `material(name, props)`, `texture(name, file | embedded bytes as base64 Content)`, `bone(name, parent, T, R, S)`, `skin(mesh, clusters{bone, indexes, weights, Transform, TransformLink})`, `blendShape(mesh, channel, shape{indexes, vertices, normals})`, `animStack(name, curves{node, "Lcl Translation"|"Lcl Rotation"|"Lcl Scaling"|"DeformPercent", keys at KTime ticks 46186158000/s})`, `toString()`), loaded through `importFbxFromMemory`. A binary-FBX smoke fixture (a few hundred bytes, deflated array) is generated once by the builder's binary writer or checked in as a hex literal, to cover ufbx's inflate path. Where a fixture construct is rejected by the pinned ufbx, the test says which line, never skips.

`FbxImportTest` (link `Aver.Formats.Fbx Aver.Formats Aver.Core Aver.Platform`):
- **Frames**: Y-up metres point (1,2,3) -> engine (-300,100,200); Z-up centimetres point (10,20,30) -> (20,10,30); `UnitScaleFactor` 2.54 (inch) honoured; `unitMetersOverride`; `convertAxes=false`; every case also built as GLB through `importGltfFromMemory` and compared bit-exact on positions, normals, winding and bounds for the same cube.
- **Winding/mirror**: node scale (-1,1,1) reverses triangles once more and normals still face out; a plain node does not.
- **Mesh**: quad and concave n-gon triangulate to the right area; hard edge splits, smooth welds (vertex counts); two control points at one position keep separate skin weights; V flip; missing normals/UVs per option; two materials -> two submeshes with `submeshesPartitionIndices`; duplicate material names deduped and equal to `ImportedMaterial::name`; empty mesh skipped with a report; non-FBX bytes and truncated files refused with a message (no crash); hidden node skipped.
- **Instancing**: two nodes sharing one geometry: same basis -> one mesh, two placements; rotated -> `_2` copy; node-level material override -> separate copy.
- **Skin**: 5 influences -> top 4 renormalised and `Stats` counts the vertex and max dropped weight; zero-weight vertex binds bone 0; bind pose differing from frame-0 node pose: skeleton follows bind (skinning matrix identity at bind: `inverseBind * model == I` for every bone to 1e-4); topmost bone under a non-bone armature node with rotation+scale folds into rest; helper node between bones becomes a chain bone; socket rule (locator child of bone yes; mesh child no; locator with a bone descendant no; duplicate name suffixed); two disjoint rigs -> two skeletons and `meshSkinIndex` differ; >256 bones reported.
- **Animation**: translation/rotation/scale curves -> tracks with expected values at expected times in engine space; constant-equal-to-rest channel emits nothing; stepped curve stays a hold; Euler 350 -> 10 degrees takes the short arc; topmost-bone key equals the hand-computed fold; stack with no duration skipped; two stacks -> two clips; stack animating two skeletons -> two clips with distinct `animationSkeleton`; root motion Off/Auto/All (Auto flags a clip travelling 200 cm, not one travelling 2 cm; flagged clip passes `anim::clipHasRootMotion`); object clip for a rotating mesh node reproduces `W(t)` after the playback formula `F(t) = B * A(t0)^-1 * A(t)` (the check `GltfTest` uses); skinned mesh node skipped and reported; every produced `OcAnimation::valid()`.
- **Materials**: Phong -> roughness `sqrt(2/(Ns+2))`, metallic 0; PBR (glTF-material shader) values pass through; opacity < 1 -> BLEND; opacity texture -> MASK + `opacityTex`; embedded texture bytes verbatim and `suggestedName` sanitised; missing external texture leaves the slot unset with a report; `..` escaping path refused; texture search order (`.fbm` folder); unsupported extras named once each.
- **Cameras/lights**: camera yaw/pitch/FOV in engine space; lights reported only.
- **Blend shapes** (fake attach seam): channel with two keyframes keeps the full one and reports the in-between; deltas transformed by the same basis as positions (a +X delta in a Y-up file lands on engine +Y, scaled by 100); normal deltas by inverse-transpose; a delta applies to every split vertex of its control point; `DeformPercent` curve 0..100 -> `morph.<name>` 0..1.
- **Options text**: `fbxOptionsToText`/`FromText` round trip, unknown key rejected by name.
- **Determinism**: importing the same memory twice gives identical `.ocmesh`/`.ocanim` bytes.

`FbxMaterialPackTest` (links `Aver.Formats.Material` when `TARGET Aver.Formats.Material`, like `MaterialTest`): roughness PNG + metalness PNG of different sizes -> one packed image with G/B from the right sources, factors reset, `metallicTex` cleared, an already-packed material untouched, an old importer's material (no `metallicTex`) byte-identical output.
`ImportRecordTest`: parse/write round trip, unknown lines preserved, relative/absolute source, CRLF, malformed rejected with a reason.
`ModelImportDialogTest` (compiles `sandbox/src/ModelImportDialog.cpp` with `AVER_WITH_IMGUI` undefined, the `AnimGraphEditorTest` pattern): options struct <-> text, reimport selection (record found from an `OUTPUT` file), record write/refresh, extension routing table.
`FrameworkImportAbiTest` (`tests/framework_import`, links `Aver.Framework Aver.Core`): no provider -> `available 0`, `import_model` returns 0 with the message and zero counts; a fake provider receives exact strings/flags and its counts/message are relayed; clearing restores zeros; `aver_fw_import_abi_version() == 1`.
`AverAssetCTest`: unchanged (no key changes). Add one run of `AverAssetC convert fixture.fbx` in `FbxImportTest` (the target `add_dependencies(... AverAssetC)` like `AverAssetCTest`) asserting a mesh, skeleton and clip artifact line with `verified:true`.
C#: `ModelImportNodeTests.cs` in the `MontageNodeTests` mould (`NodeRowKit`): both rows name real `ModelImportGraph` methods matching their pins and defaults, IL valid, exec node refused by the pure compiler, pure node works, missing `source` fails by name; registered in `Program.cs`. Palette mirror parity is checked by the existing `GameSystemNodes.Validate()` call.

## 6. Headed manual verification (main loop, not the implementer's tests)

Use files that may be local only (never commit): any rigged, animated CC0 FBX (Quaternius or Kenney packs; the tree already uses Kenney content), plus a static multi-material prop with textures, plus an FBX with embedded textures (re-save a prop from Blender with "Embed Textures" on, Path Mode: Copy).
1. **Static prop**: Content Browser -> Import Asset -> pick the `.fbx`; the options panel shows. Import with defaults. Expect: `.ocmesh` per mesh, materials and textures under `Content/Materials|Textures`, a scene `.ocworld` if the file has several nodes. Drop it in a level: correct size (a 1 m cube in Blender = 100 cm), upright, not mirrored, textures right way up (V flip), lit like the same asset imported as glTF.
2. **Parity**: export the same Blender scene as FBX and GLB, import both into two folders, run `OcmeshDiff` on the pairs: bounds, vertex and triangle counts match (within welding differences) and the two placed copies overlap exactly in the viewport.
3. **Skinned character**: import; open the `.ocskel` in the Anim editor: bone count and hierarchy as in Blender; play each clip: no explosion, feet where the rig has them, same pose as the DCC at keyframes. Hierarchy with a scaled/rotated armature node (Blender's default FBX export has one): character stands upright at the right size and clips play.
4. **Root motion**: a walk clip with forward travel imports with Root motion checked in the Anim editor (Auto); an in-place clip does not. On a PIE character with `SetRootMotion Apply`, the walk travels; with Off it slides in place.
5. **Embedded textures + missing textures**: the embedded file imports with textures; delete one referenced external texture and re-import: warning names it, the material still imports.
6. **Packed ORM**: a material with separate roughness and metalness maps renders with both effects (a metal sphere with a rough map shows both), where the OBJ path would have dropped the metal.
7. **Reimport**: edit the model in Blender, re-export over the same file, right-click the `.ocimport` (or the mesh) -> Reimport: files replaced (old ones in the recycle bin), options reused; change one option via Import options...; hand-edited level placements keep working.
8. **Blend shapes** (after MORPH_TARGETS lands): a face FBX imports shapes, a `morph.*` curve on the clip animates them.
9. **Graph/C#**: a graph with `IsModelImportAvailable` -> `ImportModel` runs in an editor PIE session and the asset appears; the same graph in a packaged game reports unavailable and does not crash.
10. **Regression**: `--import-gltf` of the Sponza or Kenney glb gives byte-identical outputs before and after the writer refactor (hash the output folder); CLI `AverAssetC convert` on an OBJ and a USD still produces the same JSON lines.
11. **Corrupt input**: truncate an FBX, zero a chunk of a binary FBX, rename an OBJ to `.fbx`: each fails with a message in the notification and Output Log, the editor stays up.

## 7. Open questions (defaults chosen)

1. **ufbx version**: default pin the newest release tag >= 0.23.1 at vendor time (header read: 0.23.1). Everything here uses names from that header; if the owner prefers an older tag, re-check `ufbx_bake_opts`, `UFBX_PIVOT_HANDLING_ADJUST_TO_ROTATION_PIVOT`, `ufbx_skin_cluster::geometry_to_world`, `ufbx_blend_shape::offset_weights`.
2. **Lights**: not imported in v1 (glTF imports none; FBX light units are not portable). Default: reported only. Cameras: first one becomes the level start camera.
3. **Script surface**: ship the ABI + C# + two nodes as specified (the rule requires a surface); default is editor-hosts-only, synchronous. If the owner would rather have none, delete section 2.10 and the corresponding files; nothing else depends on it.
4. **Colour space of FBX factors**: default "sRGB, crosses unconverted" (the OBJ precedent), switchable per import by `fileColoursAreLinear`; Blender's exporter writes linear and Maya display values, and the file does not say.
5. **Root motion default Auto** (flag only, inert without `CRootMotion`); `Off` restores glTF behaviour exactly.

Not in v1 and said so in `unsupported`/docs: lights, vertex colours, second UV set, normal-map green-channel convention detection, bump/height maps, layered/procedural textures, UV transforms, dual-quaternion skinning, in-between blend shapes, animated ancestors above a skeleton root, rigid attachment of unskinned meshes to bones, FBX geometry caches, NURBS/patches (ufbx tessellates them; imported as meshes if present), subdivision surfaces (control cage imports unsubdivided and says so, like USD, `ASSET_IMPORT.md:389-392`).
