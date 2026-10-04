I have now traced the full pipeline from writer to runtime. Here is the implementation-ready specification.

---

# `.ocbeam` Format & Runtime Consumption — Faithful Spec

## 0. What it is / pipeline

`.ocbeam` is a **line-based UTF-8 text** file describing a soft-body "cage" (mass-spring lattice) for a vehicle, plus an optional embedded glTF body, articulated rig, baked collision hulls, and header directives. It is **source**, compiled once at editor-import into a `UVehicleCageAsset` (`.uasset`, the "bytecode"), which the runtime consumes with no text parsing (`VehicleCageAsset.h:143-158`).

| Stage | Code | Role |
|---|---|---|
| Write | `Main.java:writeOcbeamDataFile` (1245-1300) | OpenConstructor Studio emits the text |
| Import | `VehicleCageFactory.cpp:CompileOcbeamIntoAsset` (860-980) | parse + resolve + bounds-fit glTF + fold rig/collision/aero → `.uasset` |
| Row parse | `VehicleDamage.cpp:ParseOcbeam` (199-524) | shared cage-row parser (used by both import and the loose-file fallback) |
| Sim | `VehicleDamage` solver | PBD mass-spring cage → per-node displacements |
| Render | `VehicleCageComponent.cpp` | ProceduralMesh sections skinned to the cage nodes |

Import registration: text factory, single extension **`ocbeam`**, target class `UVehicleCageAsset` (`VehicleCageFactory.cpp:985-991`).

---

## 1. Lexical / global rules

Confirmed in `ParseOcbeam` (`VehicleDamage.cpp:221-253`) and the factory sub-parsers:

- Line-oriented. Each line is `TrimStartAndEnd`'d, then **one** trailing `;` is stripped (`224`). (Rig/collision sub-parsers strip their own trailing `;` — `708`, `828`.)
- `#` at line start = comment; blank lines ignored (`226`).
- **Section keyword lines** are detected by prefix `StartsWith(...)` and switch a mode; a lone `}` resets mode to None (`244-253`). Because detection is a prefix match, `MATERIAL{`, `NODE{` etc. all trigger (the `{` is not required by the parser, but the writer always emits it).
- Row **field delimiter is `,`**; header directives (`REBOUND`, `SCALE`, `GLBXFORM`) are **space**-delimited.
- Numbers use `FCString::Atof/Atoi` (UE) ⇄ `Locale.US` on write (`Main.java:1256`), i.e. `.` decimal, no thousands sep.
- Embedded binary (glTF, buffers) is **base64** text; glTF-internal buffers are **little-endian** (`Main.java:2184-2185`).

### Versioning / magic
- Optional **first line `OCBEAM 1`** (writer always emits — `Main.java:1247`). It is a *tolerated/ignored* version header: the UE `ParseOcbeam` treats it as an unknown line inside no section (no-op); the Java re-loader explicitly skips `OCBEAM…` (`Main.java:1153`). There is **no binary magic**.
- The compiled asset carries its own `FormatVersion`, set to **2** at import (`VehicleCageFactory.cpp:969`; "2 = added articulated rig"). The `OCBEAM 1` text header and the asset `FormatVersion` are unrelated counters.

### Coordinate system & units
| Property | Value | Source |
|---|---|---|
| Space | Vehicle-local | `FVehicleNode.Position` comment `VehicleDamage.h:79` |
| Axes | **X = forward/length, Y = right/width, Z = up** | `ParseImportScale` NORMALIZE: WIDTH=min(X,Y), HEIGHT=Z, LENGTH=max(X,Y) (`VehicleCageFactory.cpp:847-849`); CLAUDE.md:271 |
| Handedness | Cage space is **right-handed** (glTF-derived); UE actor space is left-handed → **Y-mirror at render** (`Y→−Y`) | `VehicleCageComponent.cpp:252-276`, `646-647` |
| Length unit | **centimetres (cm)** | `VehicleDamage.h:55`, CLAUDE.md:271 |
| Force unit | **Newtons (N)** | `VehicleDamage.h:55,63-66` |
| glTF→cage | `(x,y,z)→(x,−z,y)` (Y-up→Z-up) + optional forward-rotate + Y-mirror, **no scale** | `OcGltf::Orient` (`VehicleCageFactory.cpp:489-502`) |

---

## 2. Top-level grammar

```
[OCBEAM <n>]                         # optional version header, ignored
# comments...
[REBOUND <0..1>]                     # optional; whole-car restitution
[SCALE <factor>] | [NORMALIZE <LENGTH|WIDTH|HEIGHT> <cm>]   # optional; import-time uniform scale
MATERIAL{ ... }
NODE{ ... }
BEAM{ ... }
PANEL{ ... }
PART{ ... }
[GLBXFORM yup=<0|1> forward=<0..3> mirror=<0|1>]
[GLB{ ENC 1 ; B <base64> ... }]      # embedded whole .glb body
[BONE{ ... }] [SKIN{ ... }] [ANIM{ ... }]   # articulated rig
[COLLISION{ HULL{ v x y z; ... } ... }]      # baked convex hulls
```
Section **order in the writer** is exactly the above (`Main.java:1247-1298`), but the parser is order-independent (mode is set per keyword; references are resolved in a second pass). Only `NODE{}` is effectively required — `ParseOcbeam` returns success iff `Nodes.Num() > 0` (`VehicleDamage.cpp:523`).

### Header directives
| Directive | Grammar | Meaning | Parse |
|---|---|---|---|
| `REBOUND` | `REBOUND <f>` | Whole-car coefficient of restitution, clamped 0..1. **Absent ⇒ −1 sentinel** → runtime keeps the Blueprint's `ReboundCoefficient` (no override). | `VehicleDamage.cpp:252`; asset `Rebound` `VehicleCageAsset.h:200-205` |
| `SCALE` | `SCALE <f>` | Uniform multiplier applied to **all node positions at import** (raw). Body glTF, rig `GltfToCage`, beam rest-lengths auto-follow (they derive from scaled nodes); collision hull verts, aero CoP, and aero ride-axis scaled by the same factor afterward. Aero force (N) left unscaled. | `ParseImportScale:833`; applied `860-884`, `962-967` |
| `NORMALIZE` | `NORMALIZE LENGTH\|WIDTH\|HEIGHT <cm>` | Alternative to SCALE: factory measures the node-AABB extent on that axis and solves the factor to hit `<cm>`. | `ParseImportScale:838-851` |

`SCALE`/`NORMALIZE` are read only from **header lines before the first section**; the scan stops at the first `MATERIAL/NODE/BEAM/PANEL/PART/GLB` keyword (`VehicleCageFactory.cpp:852-855`). The Java writer emits `SCALE` only when `≠1` (`Main.java:1251`).

---

## 3. `MATERIAL{}`

A material is defined once, referenced by **name**. Row = comma-separated fields.

⚠️ **Field-count discrepancy — must resolve in the port** (see §11): the **runtime parser requires EXACTLY 10 fields and *skips* any other count** (`VehicleDamage.cpp:261-265`), but the **current Java writer emits 13** (`Main.java:1254-1258`). The sample `Ferrari499P.ocbeam` has 10-field rows (an older writer). Documented as **the 10 the runtime actually reads**, with the 3 extra writer fields noted.

Runtime schema (`VehicleDamage.cpp:266-283`, defaults `VehicleDamage.h:57-71`):

| # | Field | Type | Units | Meaning | Default |
|---|---|---|---|---|---|
| 0 | Name | string | — | reference key | — |
| 1 | Stiffness | float | 0..1 | PBD relaxation feel | 0.7 |
| 2 | AxialStiffness | float | N/cm | elastic stretch resistance | 3500 |
| 3 | BendForceN | float | N | yield onset (permanent bend begins) | 3000 |
| 4 | BreakForceN | float | N | instant catastrophic snap | 12000 |
| 5 | PlasticStiffness | float | N/cm | extra bend resistance once yielded | 800 |
| 6 | MaxBend | float | cm | accumulated bend before tear | 8 |
| 7 | BendAbsorb | float | 0..1 | energy soaked per cm of bend | 0.3 |
| 8 | BreakAbsorb | float | 0..1 | energy soaked on snap | 0.6 |
| 9 | Behavior | enum | — | `DEFORM`\|`FRACTURE`\|`SHATTER` | Deform |

Extra writer-only fields 10-12 (`Main.java:29-31`, **not read by the shipped parser**): `TearStrainTension` (−1=unset), `TearStrainCompression` (−1=unset), `Density` (relative node mass, 1.0=default).

**Behavior mapping** (`VehicleDamage.cpp:278-281`): `SHATTER` or `GLASS` → `Shatter`; `FRACTURE` → `Fracture`; anything else → `Deform`. (Case-insensitive, uppercased.)

**Behavior semantics** (`EDamageBehavior`, `VehicleDamage.h:47-52`; render side `VehicleCageComponent.cpp:317-360`):
| Behavior | Physics | Render (crumple stiffness / tear) | FX family |
|---|---|---|---|
| **DEFORM** (steel) | Ductile: bends, detaches whole, never shatters | full dent (`DeformStiffness=1`), ragged high-stretch tear (`DuctileTearScale`) | metal spark |
| **FRACTURE** (carbon) | Detaches WHOLE at detach impulse; shatters into shards only above the higher shatter impulse | renders only `CarbonDeformStiffness` of crumple → cracks; mid tear | carbon dust |
| **SHATTER** (glass) | ALWAYS shatters into shards (brittle), even at detach impulse | `GlassDeformStiffness`; crisp low-stretch tear (`BrittleTearScale`) | glass dust |

Sample (`Ferrari499P.ocbeam:6-8`): `Steel,0.70,3500.0,3000.0,12000.0,800.0,8.00,0.30,0.60,DEFORM` / `Carbon,…,FRACTURE` / `Glass,…,SHATTER`.

---

## 4. `NODE{}`

Row: `ID,X,Y,Z` — **exactly 4 fields**, else skipped (`VehicleDamage.cpp:292-304`). Writer `%d,%.2f,%.2f,%.2f` (`Main.java:1260`).

| Field | Type | Units | Meaning |
|---|---|---|---|
| ID | int32 | — | **node identity** (referenced by beams; also `node_<ID>` bone match, CLAUDE.md:272). Not necessarily contiguous/array-index. |
| X,Y,Z | float | cm | position, vehicle-local (X fwd, Y right, Z up) |

A node is one point mass. Runtime state (velocity via Verlet `PrevPosition`, pin flag, mass, freed/torn flags) is all transient/computed, never authored (`VehicleDamage.h:81-93`).

---

## 5. `BEAM{}`

Row: `ID(NodeA,NodeB)` — split on `(`, strip `)`, inner needs **≥2** comma fields (extra ignored) (`VehicleDamage.cpp:308-333`). Writer `%d(%d,%d)` (`Main.java:1262`).

| Field | Type | Meaning |
|---|---|---|
| ID | int32 | beam identity (referenced by panels) |
| NodeA, NodeB | int32 | **NODE IDs** (not indices) this beam connects |

**Cross-reference resolution:** beams reference nodes **by NodeID**. The physics properties (Stiffness…Behavior) are **not in the beam row** — they are *pushed down from the owning panel's material* in Phase 2 (`VehicleDamage.cpp:496-516`; fields `VehicleDamage.h:105-115`). NodeID→index resolution (`NodeAIdx/NodeBIdx`) and rest-length capture happen later in the solver's `InitSolver`, not in the parser (`VehicleDamage.h:117-119`).

---

## 6. `PANEL{}`

Row: `ID(BeamA,BeamB,BeamC[,MaterialOverride])` — split on `(`, inner needs **≥3** fields; optional **4th** = material name override (`VehicleDamage.cpp:336-366`). Writer emits 3 (`Main.java:1264`); the 4th is parser-supported but not written by the current writer.

| Field | Type | Meaning |
|---|---|---|
| ID | int32 | panel identity (referenced by parts) |
| BeamA/B/C | int32 | **BEAM IDs** forming a triangle |
| MaterialOverride | string (opt) | material name overriding the part's material for this panel's 3 beams |

**Semantics:** a panel is a **triangle of 3 beams**. It is the *material-carrier* and the *part-membership unit*. Panels are **not rendered directly** — rendering comes from the embedded glTF PartMeshes (§9). A panel's material (override, else its part's) is written onto its 3 beams (`VehicleDamage.cpp:481-516`).

---

## 7. `PART{}`

Row: `PartID(Name,Role,Material[,MeshName][,DETACH=<f>],[p0,p1,...])`

Parse (`VehicleDamage.cpp:369-435`):
1. Split on `(`. **The leading `PartID` is parsed off but discarded** — `FVehiclePart` has no ID field (the writer still emits it, `Main.java:1271-1273`).
2. Split remainder on `[` … `]` to separate header from the panel-ID list.
3. In the header, **`DETACH=<f>` is pulled out first (position-independent, scanned from the end)** so it never shifts positional fields (`405-413`).
4. Header needs **≥3** positional fields (Name,Role,Material) else the line is skipped (`394-398`).

| Field | Type | Meaning | Read by solver? |
|---|---|---|---|
| Name | string | part identity; matches `FVehicleCagePartMesh.Part` (glTF mesh name) and SKIN `PART` blocks; UE component match falls back to this | yes |
| Role | string | **free label, stored but unused** (`VehicleDamage.h:144`; `PartRow.role` comment `Main.java:82`) | no |
| Material | string | default material for this part's panels/beams; **must name a defined MATERIAL** or the part's resolution is skipped with a warning (`461-467`) | yes |
| MeshName | string (opt 4th) | component name/tag matched in UE, falls back to Name (CLAUDE.md:301-306) | at bind time |
| `DETACH=<f>` | float 0..1 | fraction of this part's beams that must break before the whole panel sheds off; **default 0.3** (`VehicleDamage.h:149-154`) | yes |
| `[panel list]` | int32 CSV | **PANEL IDs** belonging to this part | yes |

Writer role default `"Structural"`; always writes `DETACH=%.3f` and the panel list (`Main.java:1266-1277`). Runtime `Role` values seen in Studio UI: `Fracture`/`Deform`/`Structural` (`Main.java:1440`) — but again, Role is not read by the solver; the **material's** Behavior drives everything.

---

## 8. Reference graph (Phase 2 resolution) — `VehicleDamage.cpp:439-518`

```
PART ──(Material name)──▶ MATERIAL         (MaterialIndexByName)
PART ──(PanelID list)───▶ PANEL            (PanelIndexByID)
PANEL ─(BeamA/B/C ID)───▶ BEAM             (BeamIndexByID)
PANEL ─(MaterialOverride name, opt)─▶ MATERIAL
BEAM ──(NodeA/NodeB ID)─▶ NODE             (resolved in InitSolver, not parser)
```
Material push-down: for each PART → each of its PANELs → panel's 3 BEAMs get `{Stiffness, AxialStiffness, BendForceN, BreakForceN, PlasticStiffness, MaxBend, BendAbsorb, BreakAbsorb, Behavior}` copied from the panel's effective material (override else part material). Unresolved refs are logged and skipped (never fatal). **A beam not owned by any part/panel keeps its struct defaults** (`VehicleDamage.h:106-114`) — it is never assigned a material. All values are **strength-neutral**; the per-vehicle `StructuralStrength` multiplier is applied later in `InitSolver` (`VehicleDamage.cpp:508` comment).

---

## 9. Embedded glTF body — `GLBXFORM` + `GLB{}`

Import-factory-only; the runtime `ParseOcbeam` skips any line starting `GLB` (`VehicleDamage.cpp:249`).

**`GLBXFORM yup=<0|1> forward=<0..3> mirror=<0|1>`** (`VehicleCageFactory.cpp:601-618`; writer `Main.java:1285`) — the Generate-from-Mesh orientation used to build the cage. Defaults `yup=1, forward=0, mirror=0`.

**`GLB{`** then **`ENC 1`** (encoding tag, ignored) then repeated **`B <base64>`** lines (writer chunks base64 at 120 chars, `Main.java:1289-1290`), closed by `}`. Extraction concatenates all `B` payloads and base64-decodes to the raw `.glb` bytes (`ParseGlbSection:585-599`).

Import processing (`BuildPartMeshesFromGltf:507-582`):
1. Parse the `.glb` (geometry + glTF metallic-roughness PBR materials + PNG/JPEG images) via `OcGltf::Parse`.
2. **Orient** every vert/normal to cage space (`OcGltf::Orient`, §1); if `mirror`, reverse triangle winding (`513-520`).
3. **Bounds-fit**: per-axis affine mapping oriented-mesh AABB → cage-node AABB: `fit(v) = CageBox.Min + (v − MeshBox.Min) * (CageSize/MeshSize)` (`522-530`). This is why alignment survives arbitrary scale/offset but **cannot recover axis swaps** (per-axis min→min; CLAUDE.md:289).
4. Emit **one `FVehicleCagePartMesh` per glTF part**, name = glTF mesh/object name (`551-566`), building `UTexture2D` + `UMaterial` subobjects. Sets `bPartMeshesInCageSpace = true` (`568`).
5. Bake `Rig.GltfToCage` affine (linear rows = Orient∘Scale of basis vectors; W row = fit translation) so a skin *delta* maps into cage space with the exact transform the verts got (`570-580`).

`FVehicleCagePartMesh` (`VehicleCageAsset.h:17-39`): `Part` (FName, matches PART), `Verts/Normals/UVs/Tris/Tangents` (cage space), `Material`, and optional skin arrays `BindPosGltf[verts]`, `BoneIdx[verts*4]`, `BoneWt[verts*4]`.

> Legacy `MESH{}`/`TEXTURE{}`/`RENDERMAT{}` sections were removed and are neither written nor read (CLAUDE.md:292-293).

---

## 10. Articulated rig — `BONE{}` / `SKIN{}` / `ANIM{}`

Import-factory-only; `ParseOcbeam` **blanket-skips** these (skip-until-`}`) because their rows can mimic cage keywords, e.g. SKIN's `PART` line (`VehicleDamage.cpp:234-242`). Emitted fully-resolved by `Main.java:GltfRig.write` (2493-2537), loaded by `VehicleCageFactory.cpp:ParseRigSections` (685-812). Empty ⇒ rig no-ops.

### `BONE{}` — one armature joint per row
Row (comma-separated, **≥29 tokens** else skipped; `716-731`):
`id, name, parent, px,py,pz, qx,qy,qz,qw, sx,sy,sz, m0..m15`

| Tokens | Type | Meaning |
|---|---|---|
| 0 `id` | int | index (implied by order; token 0 discarded) |
| 1 `name` | string | bone name (`node_<ID>` convention links to cage node, CLAUDE.md:272) |
| 2 `parent` | int | parent bone index, −1 = root |
| 3-5 | float×3 | bind-pose **local** translation (parent-relative), glTF space |
| 6-9 | float×4 | rotation quaternion `x,y,z,w` |
| 10-12 | float×3 | scale |
| 13-28 | float×16 | glTF **inverseBindMatrix**, column-major (read sequentially into `M[r][c]` = transpose = UE row-vector form; `726-729`) |

→ `FCageBone` (`VehicleCageAsset.h:89-98`).

### `SKIN{}` — per-part linear-blend skin (`733-754`)
- `PART <name>` selects the matching `FVehicleCagePartMesh` (else logged, block ignored).
- Then one row per vertex (**canonical order = the rendered vertex order**), **≥11 tokens**:
`bx,by,bz, b0,w0, b1,w1, b2,w2, b3,w3` — bind position (glTF space) + 4×(bone index, weight). Weights re-normalized defensively (`748-752`).
- Validated after load: skin-vert count must equal mesh-vert count (else skin dropped), and `GltfToCage·BindPos` must land on the rendered verts within 1 cm (else drift warning) (`793-811`).

### `ANIM{}` — pre-resampled clips (`755-787`)
- `CLIP <name> <frames> <fps>` → `FCageAnim` with `Duration=(frames−1)/fps` (`757-765`).
- `BONE <boneIdx> <mask>` → track; **mask bits: 1=Pos(3), 2=Rot(4), 4=Scale(3)** (`768-778`).
- Then exactly `<frames>` rows; each row concatenates only the present channels in **T,R,S order** (`781-786`). Rotations normalized. Empty axis array ⇒ constant at RestLocal.

→ `FCageAnimTrack`/`FCageAnim`/`FCageRig` (`VehicleCageAsset.h:102-141`). Java writer mirror: `Main.java:2519-2536`.

---

## 11. `COLLISION{}` (baked hulls)

Import-factory-only (`ParseCollisionSection:643-683`); `ParseOcbeam` skips `COLLISION`/`HULL` lines (`VehicleDamage.cpp:250`). **Not written by the Java writer** — produced by `Scripts/bake_cage_collision.py` (`VehicleCageAsset.h:53-57`).

```
COLLISION{
  HULL{ v x y z; v x y z; ... }   # OBJ-style 'v ' rows, cage space cm, ≥4 verts per hull
  ...
}
```
Each `HULL{}` with ≥4 verts → one `FVehicleCollisionHull{ TArray<FVector> Verts }` (cage space, cm). Absent ⇒ the pawn convex-hulls the raw node cloud at spawn (`661-676`).

---

## 12. How the runtime turns this into a simulated cage + render mesh

### 12a. Simulated cage (physics) — `UVehicleDamage`
- `LoadCageFromAsset` copies the asset's `Materials/Nodes/Beams/Panels/Parts` verbatim (`VehicleDamage.cpp:526+`).
- `InitSolver`: resolve `NodeAIdx/NodeBIdx` from NodeIDs; capture per-beam `RestLength`; compute `InvMass`; pin base nodes; apply `StructuralStrength` to stiffness/force thresholds.
- Per tick: Verlet integrate + **PBD relaxation** over beam constraints. A beam past `BreakForceN` snaps instantly; past `BendForceN` it accumulates plastic bend (`PlasticAccum`) at `PlasticStiffness` until `MaxBend` → tear (`VehicleDamage.h:108-123`). Broken/torn state frees nodes (debris). Output: per-node **displacement-from-rest** field (`GetCageDisplacements`) + rest positions (`GetCageRestPositions`).

### 12b. Render mesh — `UVehicleCageComponent`
1. **BuildSections** (`236-385`): one `ProceduralMeshComponent` section **per `FVehicleCagePartMesh`** (not per panel). Cage→actor conversion = **Y-mirror of verts+normals** for `bPartMeshesInCageSpace` bodies, *without* re-reversing winding (the reflection already flips front-face) (`252-279`). Each section gets a `MaterialInstanceDynamic` (livery params). Per-section `Behavior`/`BreakForceN` looked up by matching `PartMesh.Part → FVehiclePart.Name → FVehicleMaterial` (`320-326`). Skin arrays copied for skinned parts (`329-335`).
2. **BindToCage** (`406-474`): once the solver's rest pose exists, bind each render vertex to its **K = `SkinK` (clamped 1..8) nearest cage nodes** via a uniform spatial grid; **inverse-distance weights** `w = 1/(dist²+1)`, normalized (`458-471`). Lookup is done in cage space (un-mirror the actor-space vert first).
3. **UpdateDeform / DeformedVert** (`506-632`, `1215-1261`), per tick, per dirty section:
   `vert = rigidPose(rest) + clamp( mirrorY( Σ_k weight_k · nodeDisp[node_k] ) · DeformGain · DeformStiffness , MaxBoneDisplacement ) + boneSkinDelta`
   - node-displacement blend = the cage crumple; brittle materials use fractional `DeformStiffness` (crack vs. flex).
   - bone skin: `Σ w·(BonePalette[bone]·BindPos)`, delta mapped by `CachedGltfToCage.TransformVector` then Y-mirrored, composed **on top of** crumple (`1241-1259`).
   - **Strain-limit tear** opens holes when edges over-stretch past the per-material `TearRatioSq` (`592-603`); impact-driven `FractureSection`/`FractureNearestPanel` shed or shatter whole panels per Behavior (`670-825`).
   - Per-panel dirty check + normal recompute only when moved (perf) (`560-619`).
- `DrawCage` (638-665) draws rest nodes/beams (debug) using the same cage→actor Y-mirror, connecting by **NodeID** — confirming beams reference nodes by ID at every stage.

---

## 13. UE dependencies to strip/replace in the port

| UE type / API | Where | Replace with |
|---|---|---|
| `FString`, `FName`, `TArray`, `TMap`, `TSet`, `FCString::Atof/Atoi`, `ParseIntoArray` | throughout parser | `std::string/std::vector/...`, custom tokenizer |
| `FVector`, `FVector2D`, `FVector3f`, `FQuat/FQuat4f`, `FMatrix`, `FPlane`, `FTransform`, `FBox` | structs + math | your math lib (mind **column-major glTF vs. row-vector UE** convention at `VehicleCageFactory.cpp:726-729`) |
| `USTRUCT/UCLASS/UPROPERTY/UDataAsset`, `GENERATED_BODY` | `VehicleDamage.h`, `VehicleCageAsset.h` | POD structs + your own asset/serialization |
| `UProceduralMeshComponent`, `CreateMeshSection_LinearColor`, `UpdateMeshSection_LinearColor`, `FProcMeshTangent` | component | dynamic vertex-buffer mesh |
| `UMaterialInterface/UMaterial/UMaterialInstanceDynamic`, `UTexture2D`, `IImageWrapper`, material-expression graph builders | factory glTF PBR import | your material/texture pipeline (PNG/JPEG decode) |
| `FBase64`, `Dom/JsonObject` + `JsonReader/Serializer` | GLB decode/parse | base64 + glTF/JSON reader (Java side already has a self-contained one, `Main.java:2768+`) |
| `UFactory` / `FactoryCreateText`, `UAssetImportData`, editor import subsystem | `VehicleCageFactory.cpp:985+` | your importer/CLI |
| `UNiagaraSystem`, `DrawDebugLine`, GPU proxy/compute path (`bGpuProxyActive`) | component FX/debug | engine-specific, optional |
| `UVehicleAerodynamics::ParseOcaero` (sibling `.ocaero`) | `VehicleCageFactory.cpp:928-958` | separate format, out of scope here |

---

## 14. Known discrepancies / ambiguities (flag for the port)

1. **MATERIAL field count (load-bearing):** runtime `ParseOcbeam` accepts **exactly 10** and skips otherwise (`VehicleDamage.cpp:261`), but the current Java writer emits **13** (`Main.java:1256`). A file from the current writer would have **every material dropped** by the shipped UE parser (→ parts then fail "unknown material"). The sample `Ferrari499P.ocbeam` is 10-field (older writer). The Java comment "UE parser accepts 10..13" (`Main.java:15,28`) does **not** match the shipped C++ code. The port should decide the canonical count (recommend: accept 10-13, defaulting fields 10-12).
2. **`DETACH=` in PART:** written by current Java writer (`Main.java:1269`) and read by current parser (`VehicleDamage.cpp:405-413`), but **absent from the sample** and from CLAUDE.md's grammar (267-306). Parser default 0.3 covers old files.
3. **Behavior set:** code supports **three** (`DEFORM/FRACTURE/SHATTER`, +`GLASS` alias) (`VehicleDamage.cpp:278-281`); CLAUDE.md:297 says only two — CLAUDE.md is stale, trust the code.
4. **`PANEL` material override (4th field):** parser-supported (`VehicleDamage.cpp:361-364`) but the current writer never emits it.
5. **`PartID` leading integer:** written but **discarded** by the runtime (no ID field on `FVehiclePart`). Do not rely on it.
6. **Sample vs. writer version skew generally:** `Ferrari499P.ocbeam` predates the 13-field material + `DETACH=` + `SCALE`/`GLBXFORM`/rig additions; use the code (writer + parser), not the sample, as the authority for the current grammar.

Key files: writer `C:/Users/User/Documents/OpenConstructorSupportAssets/OCCompiler/Main.java`; runtime row parser `C:/Users/User/Documents/Unreal Projects/OpenConstructor27/Source/OpenConstructor27/Private/VehicleDamage.cpp`; import factory `.../Private/VehicleCageFactory.cpp`; render/deform `.../Private/VehicleCageComponent.cpp`; compiled asset `.../Public/VehicleCageAsset.h`; runtime structs/enum `.../Public/VehicleDamage.h`.