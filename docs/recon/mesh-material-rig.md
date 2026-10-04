I have everything I need. Note: `PartMesh` and `RMat` do not exist anywhere in the source file — I confirmed this with case-insensitive search. I'll flag that explicitly. Here is the extracted spec.

---

# OCCompiler Internal Data Model — Faithful Spec for Native Format Design

Source of truth: `C:/Users/User/Documents/OpenConstructorSupportAssets/OCCompiler/Main.java` (4117 lines). All line citations are to this file.

## 0. Critical findings up front (read first)

1. **`PartMesh` and `RMat` do not exist in the source.** A case-insensitive search of the whole file returns zero hits for either name. They are named in the task but are not implemented classes. Do not invent them. The closest real analogues are `MeshImporter.MeshData` (geometry) and `MaterialAsset` (physics material) plus `PartRow` (per-part geometry + mesh binding). This spec documents what actually exists.
2. **There is no render/PBR material model in the compiler.** `MaterialAsset` (line 14) is a *structural/physics* material (stiffness, break force, density), not base-color/metallic/roughness. PBR materials, textures, UVs, normals, tangents, and vertex colors are **never parsed or stored by the compiler** — they ride along as the raw `.glb` bytes embedded verbatim in the `.ocbeam` and are parsed downstream by Unreal at import time (lines 2682–2684, 1281–1292). This is the single biggest gap for a native engine (see §8).
3. **The compiler extracts only what it needs to (a) decimate geometry into a physics cage and (b) fully resolve a skeletal rig.** Rendering data is deliberately not modeled.

---

## 1. Global conventions (coordinate / units / endianness / delimiters / versioning)

| Concern | Value | Evidence |
|---|---|---|
| Binary buffer endianness (glTF, .uasset) | **Little-endian** throughout | `le32`/`leF` (2184–2185, 2622–2623); `UAssetReader.i32` (2105) |
| glTF matrices | **Column-major** 4×4, 16 floats | comment 2183; `trsMatrix`/`matMul` (2243–2254); BONE header comment 2496 |
| Quaternion layout | `(x, y, z, w)`; identity `{0,0,0,1}` | `Bone.r` default (2173); `decompose` writes `r[0..3]=qx,qy,qz,qw` (2294) |
| Rig space stored | **RAW glTF space** — no reorientation baked in | comment 2171; positions/TRS/IBM copied verbatim |
| Cage/mesh space | Reoriented via `transformVert`: **Y-up→Z-up** `(x,y,z)→(x,-z,y)`, forward-axis rotation about Z, optional Y mirror, uniform scale | `transformVert` (1790–1801) |
| Forward modes | `0`=+X (no change), `1`=+Y, `2`=−X, `3`=−Y | switch (1793–1798) |
| Cage node length unit | **Centimetres** (Unreal convention); AeroSim converts cm→m by ×0.01 | 2832–2853 |
| **Units ambiguity** | glTF is nominally metres, but `transformVert` applies only `importScale` (no explicit m→cm). Whether source→cm conversion is expected to come from `importScale` or an implicit assumption is **not determinable from this file** | 1790–1801 vs 2853 — flag for porter |
| Text file (`.ocbeam`) numeric locale | `Locale.US` (`.` decimal), `%.7g` for rig floats, `%.2f`–`%.4f` for cage | 2353, 1250–1276 |
| `.ocbeam` delimiters | Sections `NAME{ ... }`; rows comma-separated, `;`-terminated for cage rows; rig rows comma-separated, no trailing `;` | 1245–1298, 2493–2537 |
| Version header | First line `OCBEAM 1` (format version 1) | 1247 |

---

## 2. Source formats imported

| Format | Extensions | Reader | What is extracted |
|---|---|---|---|
| Wavefront OBJ | `.obj` | `MeshImporter.loadObj` (2563) | Positions + triangulated faces + o/g/usemtl groups. **No** vt/vn/colors. |
| glTF 2.0 text | `.gltf` (+ external/`data:` buffers) | `MeshImporter.loadGltf` (2628) | POSITION + indices per triangle primitive; group per mesh. **No** normals/UV/color/material. |
| glTF binary | `.glb` (magic `glTF`) | `MeshImporter.loadGltf` (2634–2643) | Same as `.gltf`. Whole file also stored verbatim as `sourceGlbBytes` (1741) for embedding + rig extraction. |
| Unreal package | `.uasset` (UE 5.7, pkg ver ~1018) | `UAssetReader` (2102) | Component list only (names + FRACTURE/DEFORM kind). No geometry. |

Rig extraction (`GltfRig.extract`, 2357) accepts **either** `.glb` (chunked) **or** raw `.gltf` JSON bytes (2361–2368), and also decodes `data:` and `uri`-less (bin-chunk) buffers.

---

## 3. Static mesh model — `MeshImporter.MeshData` (2541) + `MeshImporter.Json` (2768)

### 3.1 `MeshData` fields (2542–2547)

| Field | Type | Units/space | Meaning |
|---|---|---|---|
| `verts` | `float[][]` (N×3) | source units, un-transformed | Vertex positions `(x,y,z)` only |
| `tris` | `int[][]` (M×3) | — | Triangle vertex indices, **0-based**, into `verts` |
| `triGroup` | `int[]` (length M) | — | Group index per triangle, parallel to `tris` |
| `groupNames` | `String[]` | — | Group/object/mesh names |

### 3.2 Vertex attributes actually captured

| Attribute | Captured? | Notes |
|---|---|---|
| Position | **Yes** | OBJ `v` (2598–2599); glTF `POSITION` accessor as `VEC3<float>` (2704, `readVec3` 2744) |
| Normal | No | OBJ `vn` explicitly skipped (2596); glTF NORMAL never read |
| Tangent | No | never read |
| UV / texcoord | No | never read (comment 2683–2684) |
| Vertex color | No | never read |
| Joints / weights | No (in `MeshData`) | handled separately by `GltfRig`, §6 |

### 3.3 Index format

- Internal: triangles as `int[3]`, 0-based, into `verts`.
- glTF index accessor component types decoded (`readIndices` 2752–2755): `5121`=u8, `5123`=u16, else u32 (`5125`). Non-indexed primitives synthesize `0..N` (2708).
- Only `mode == 4` (triangles) primitives are used; others skipped (2700). Polygons in OBJ are fan-triangulated (2610); negative OBJ indices are relative-to-end (2608).

### 3.4 Submesh / material assignment

- No material assignment at all in `MeshData`. Grouping is geometric only:
  - **OBJ**: each `o`/`g` block → a group; `usemtl` is a *fallback* grouping only when no `o`/`g` seen (2585–2593). Faces before any group default to group "Body" (2601).
  - **glTF**: one group per mesh that contributes triangles; name taken from the first instancing **node's** name, else mesh name, else `mesh_<i>` (2657–2718). All primitives of a mesh collapse into that one group (per-primitive material is discarded).
- Groups later map to `PartRow`s and panel grouping downstream (2871–2877).

### 3.5 `MeshImporter.Json` (2768)

Minimal recursive-descent JSON parser: objects (`LinkedHashMap`, order-preserving), arrays, strings (with `\uXXXX`), numbers (all as `Double`), `true`/`false`/`null`. No error recovery. Reused by both the mesh importer and `GltfRig` (2370). Numbers are always `Double` — integer accessor fields are read via `asInt` = `round(doubleValue)` (2624, 2186).

### 3.6 Related: `PartRow` reference geometry (80–93) — adjacent, not in task list

`PartRow` carries editor-only reference geometry: `geomVerts` (raw model-space x,y,z), `geomFaces` (0-based tri indices), `geomWorld` (cached transformed). Comment 86–90: used only for editor visualization/auto-bind, **not written to `.ocbeam`**. `PartRow.meshName` (81) is the component tag used to match UE mesh parts.

---

## 4. Material model — `MaterialAsset` (14) — STRUCTURAL, not render

Mirrors the UE `.ocbeam` MATERIAL schema: 10 required + 3 optional fields (15–17).

| # | Field | Type | Units | Meaning | Line |
|---|---|---|---|---|---|
| 1 | `name` | String | — | Material key | 18 |
| 2 | `stiffness` | float | 0..1 | Relaxation feel | 19 |
| 3 | `axialStiffness` | float | N per cm stretch | Elastic axial stiffness | 20 |
| 4 | `bendForceN` | float | N | Yield: permanent bend begins | 21 |
| 5 | `breakForceN` | float | N | Instant snap force | 22 |
| 6 | `plasticStiffness` | float | N per cm | Post-yield extra-bend stiffness | 23 |
| 7 | `maxBend` | float | cm | Accumulated bend before tear | 24 |
| 8 | `bendAbsorb` | float | 0..1 | Energy soaked per cm bend | 25 |
| 9 | `breakAbsorb` | float | 0..1 | Energy soaked on snap | 26 |
| 10 | `behavior` | String | enum | `DEFORM \| FRACTURE \| SHATTER` | 27 |
| 11 | `tearStrainTension` | float | strain fraction | `-1` = unset | 29 |
| 12 | `tearStrainCompression` | float | strain fraction | `-1` = unset | 30 |
| 13 | `density` | float | relative | node-mass density, default `1.0` | 31 |

Serialized (line 1256): `Name,%.2f,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.2f,Behavior,%.2f,%.2f,%.2f;`.

**Render material params requested by the task — base color, metallic, roughness, normal map, emissive, texture refs — are ENTIRELY ABSENT.** None are read, stored, or emitted. They exist only inside the embedded raw `.glb`.

---

## 5. Skeleton — `GltfRig.Bone` (2173), assembled in `GltfRig.extract` (2357)

### 5.1 `Bone` fields

| Field | Type | Units/space | Meaning |
|---|---|---|---|
| `name` | String | — | Joint node name, sanitized (`,`, spaces, CR/LF → `_`, 2352); fallback `bone_<i>` |
| `parent` | int | — | Parent **index within the joint set** (not glTF node index); `-1` = root (2407) |
| `t` | float[3] | raw glTF | Local bind translation |
| `r` | float[4] | raw glTF, quat xyzw | Local bind rotation |
| `s` | float[3] | raw glTF | Local bind scale |
| `inv` | float[16] | raw glTF, column-major | glTF `inverseBindMatrix`; if skin has none, derived as inverse of accumulated world bind (`deriveInvBind` 2305) |

### 5.2 Skeleton build rules

- Uses **skin[0] only**; multiple skins warn and are ignored (2391–2392) — a v1 limitation.
- Joint order = glTF `skin.joints` order (2393–2398); `nodeToBone` maps node→bone index.
- Local TRS from node `matrix` (decomposed, 2299) or explicit `translation`/`rotation`/`scale` (2300–2302). Defaults: T `{0,0,0}`, R `{0,0,0,1}`, S `{1,1,1}` (2173).
- `present = !bones.isEmpty()` (2488); no skins/joints → empty rig returned, cage still valid (2372, 2394).

### 5.3 `.ocbeam` BONE emission (2495–2507)

Row: `id,name,parent, tx,ty,tz, qx,qy,qz,qw, sx,sy,sz, inv[0..15]` (header comment 2496).

---

## 6. Skin — `GltfRig.PartSkin` (2174), built at 2413–2447

| Field | Type | Meaning |
|---|---|---|
| `part` | String | Part name — **must match UE `OcGltf` part naming + vertex order exactly** ("canonical-order contract", 2168, 2413). First primitive = mesh/node name; subsequent = `name_<n>` (2430) |
| `bind` | List<float[3]> | Per-vertex **raw glTF bind POSITION** (2438) |
| `ji` | List<int[4]> | 4 joint indices per vertex (into bone set) |
| `jw` | List<float[4]> | 4 weights per vertex |

Rules:
- Vertex attributes read: `POSITION` (VEC3 f), `JOINTS_0` (VEC4 uint via `readVec4u`, comp 5123=u16/5125=u32/else u8, 2220–2229), `WEIGHTS_0` (VEC4 via `readVecNf`, normalized int decode, 2206–2219).
- Joint index clamped to `[0,NB)` else 0; weights clamped `≥0`, **normalized to sum 1**; degenerate (sum≈0) → `(joint0,w=1)` fallback (2439–2442).
- Zero-triangle primitives are skipped **but still advance the part index** to stay aligned with UE (2429). Unskinned primitives (no JOINTS/WEIGHTS) count as a part but emit no SKIN (2432).
- Exactly **4 influences per vertex** — fixed, no variable-count support.

`.ocbeam` SKIN emission (2508–2518): `PART <name>` then per-vertex `bx,by,bz,j0,w0,j1,w1,j2,w2,j3,w3`.

---

## 7. Animation — `GltfRig.Anim`/`Track`/`Sampler` (2175–2176, 2327)

### 7.1 `Anim` (2176)

| Field | Type | Units | Meaning |
|---|---|---|---|
| `name` | String | — | Clip name, sanitized; fallback `anim_<i>` |
| `frames` | int | frames | `ceil(maxT × fps) + 1`, min 1 (2470) |
| `fps` | float | fps | **Fixed 30** (2176, 2469) |
| `tracks` | List<Track> | — | One per animated bone |

### 7.2 `Track` (2175)

| Field | Type | Meaning |
|---|---|---|
| `bone` | int | Bone index |
| `hasT/hasR/hasS` | boolean | Channel-present mask → emitted bitmask `T=1,R=2,S=4` (2524) |
| `t` | float[frames][3] | Resampled translation |
| `r` | float[frames][4] | Resampled rotation (quat, re-normalized 2480) |
| `s` | float[frames][3] | Resampled scale |

All clips **resampled to a uniform 30 fps grid** (2478); sample time `frame/fps`.

### 7.3 `Sampler` (2327) — interpolation

| Field | Type | Meaning |
|---|---|---|
| `in` | float[] | Keyframe input times (seconds) |
| `out` | float[][] | Output values (VEC3 or VEC4 by `comp`) |
| `interp` | String | `LINEAR` (default) / `STEP` / `CUBICSPLINE` |
| `comp` | int | 3 (T/S) or 4 (R) |

Interpolation behavior (`sample`, 2340–2349): clamp before first / after last key; `STEP` = hold; quaternions **slerp** (`slerp` 2317, sign-corrected, lerp+normalize fallback when nearly parallel); vectors **lerp**. **CUBICSPLINE is reduced to its value samples and treated as LINEAR** — in/out tangents are discarded (2336–2338). Channel→TRS mapping via `target.path` = `translation`/`rotation`/`scale` (2464).

`.ocbeam` ANIM emission (2519–2535): `CLIP <name> <frames> <fps>`, then per bone `BONE <bone> <mask>` and per-frame rows (T then R then S, comma-joined per mask).

---

## 8. Unreal `.uasset` reader — `UAssetReader` (2102) + `Comp` (2103)

Purpose: extract *part components* from a UE 5.7 Blueprint (no geometry). Targets package version ~1018, `PACKAGE_SAVED_HASH` (2098–2101, 2126).

### 8.1 `Comp` (2103)

| Field | Type | Meaning |
|---|---|---|
| `name` | String | Component name, with `_GEN_VARIABLE` suffix stripped (2156–2157) |
| `kind` | String | `FRACTURE` (`GeometryCollectionComponent`) or `DEFORM` (`SkeletalMeshComponent`) (2153–2154) |

### 8.2 Header/table layout parsed (2119–2160)

| Item | Value | Line |
|---|---|---|
| Package magic tag | `0x9E2A83C1` | 2120 |
| Legacy version gate | `legacy`; extra i32 unless `-4`; UE5 ver if `legacy≤-8` | 2121–2124 |
| SavedHash (FIoHash) | 20 bytes skipped if `ue5≥1016` | 2126 |
| Custom versions | count × 20 bytes (FGuid16+int32) | 2128 |
| Conditional blocks | SoftObjectPaths if `ue5≥1008`; LocalizationId if `ue4≥516`; GatherableText if `ue4≥459` | 2132–2134 |
| Name table entry | FString + 2×uint16 hash (`pos+=4`) | 2141 |
| FString encoding | len>0 = UTF-8 (len includes NUL); len<0 = UTF-16LE (`-len` chars) | 2107–2113 |
| FObjectImport | 40 bytes; ObjectName FName @ +20 | 2143–2144 |
| FObjectExport | 112 bytes; ClassIndex @ 0, ObjectName @ 16 | 2146–2152 |
| ClassIndex resolution | negative → import table (`-idx-1`) | 2152 |

Throws if offsets are out of range (targets UE 5.7 layout only, 2137–2138). Only two component classes are recognized; everything else ignored. When imported, FRACTURE→role "Fracture"/material "Carbon", DEFORM→"Deform"/"Steel" (2086–2087).

---

## 9. Data that currently has NO storage format (needs new `.oc*` formats)

The compiler's own persisted format (`.ocbeam`, `writeOcbeamDataFile` 1245) stores the **physics cage** (MATERIAL/NODE/BEAM/PANEL/PART), the **rig** (BONE/SKIN/ANIM), and the **entire source `.glb` base64-embedded** (GLB{} + GLBXFORM orientation flags, 1284–1292). For a native permissively-licensed engine you must design real formats for the following, because they either have no first-class format or are only smuggled through the opaque embedded glb:

| New format (suggested) | Data that needs it | Why it's unformatted today | Evidence |
|---|---|---|---|
| **Static mesh** (`.ocmesh`) | Positions, normals, tangents, UVs, vertex colors, index buffer, per-submesh material binding | Compiler keeps only position+index+group (`MeshData`); all other attributes discarded; render geometry currently survives *only* as embedded glb bytes | 2542, 2683–2684, 1281–1292 |
| **Material** (`.ocmat`) | PBR: base color, metallic, roughness, normal, emissive, texture refs, per-primitive material assignment | **No render-material model exists at all**; `MaterialAsset` is physics-only; PBR lives only inside the embedded glb | 14–60, 2683–2684 |
| **Texture** (`.octex`) | Image pixel data + sampler/wrap/filter + sRGB flag | Never parsed; only present as bytes inside the embedded glb | 2683–2684, 1281–1292 |
| **Skeletal mesh** (`.ocskm`) | Skinned render mesh binding bind-pose positions to 4 joints/weights + the mesh's normals/UVs; the "canonical vertex-order contract" | `PartSkin` stores bind position + joints/weights but **no render attributes**, and relies on an implicit external vertex-order contract with UE `OcGltf` | 2174, 2168, 2413 |
| **Skeleton** (`.ocskel`) | Bone hierarchy, local bind TRS, inverse-bind matrices | Currently only serialized as text BONE{} rows in `.ocbeam`; no standalone binary/versioned format; single-skin limit | 2173, 2495–2507, 2391–2392 |
| **Animation** (`.ocanim`) | TRS tracks, per-channel mask, fps/frame count | Only text ANIM{} rows; **fixed 30 fps**, CUBICSPLINE tangents lost, STEP flattened at resample time — original fidelity is not preserved | 2176, 2469–2483, 2336–2338 |

Fidelity caveats to preserve or fix in the port: (1) 30 fps hard-resample loses original keyframe timing; (2) CUBICSPLINE and STEP curve shapes are lost; (3) only 4 influences/vertex; (4) only the first glTF skin; (5) no morph targets, no cameras, no lights, no LODs read anywhere.

---

## 10. UE dependencies to strip/replace in the port

The classes in scope are **pure Java** (only `java.*`) — no Unreal types are referenced inside `MeshImporter`, `MaterialAsset`, `GltfRig`, or `UAssetReader`. The UE coupling is *semantic/contractual*, not type-level:

| Dependency | Where | Port action |
|---|---|---|
| `.uasset` UE5.7 package binary layout (tags, 40/112-byte records, version gates, FIoHash) | `UAssetReader` (2098–2160) | Replace with your engine's Blueprint/scene importer or drop; this is Epic package-format reverse-engineering that breaks when Epic changes the format (2100–2101) |
| Component class names `GeometryCollectionComponent`, `SkeletalMeshComponent` | 2153–2154 | Map to native fracture/deform component types |
| `_GEN_VARIABLE` UE Blueprint naming convention | 2156 | Drop or replace with your naming scheme |
| "Mirror UE `OcGltf::Parse` part naming + vertex order EXACTLY" | 2168, 2413, 2430 | Replace the implicit contract with an explicit, versioned skeletal-mesh format that stores its own vertex order |
| Orientation deferred to UE `FCageRig.GltfToCage` (rig kept in raw glTF space) | 2171 | Decide whether the native format bakes orientation or stores a transform; today rig is un-oriented and relies on the UE side |
| `.ocbeam` GLB{}/GLBXFORM handoff (UE re-parses embedded glb for geometry+PBR at import) | 1281–1298 | Eliminate by giving geometry/material/texture real native formats (§9) instead of an embedded glb |
| Unreal cm length unit assumption | 2832, 2853 | Fix a canonical unit in the native formats and convert explicitly at import |

Swing/AWT imports (lines 1–9) belong to the surrounding editor UI, not to these data classes, and are irrelevant to the format design.