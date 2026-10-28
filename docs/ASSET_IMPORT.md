# Asset import

How third-party geometry and textures get into this engine, what each importer supports, and — the
part most import documentation leaves out — what each one **cannot** do and how it tells you.

Everything here is dependency-free except FBX. The glTF, OBJ and USD readers are this tree's own code and
link no third-party library. **FBX is read by [ufbx](https://github.com/ufbx/ufbx)** (MIT, vendored at
`third_party/ufbx`), linked only by the editor, `AverAssetC` and the tests (`Aver.Formats.Fbx`), never by the
shipped game runtime.

---

## The engine contract every importer converts into

| | Source formats | This engine |
|---|---|---|
| Handedness | right-handed | **left-handed** |
| Up axis | +Y (glTF, OBJ, USD default) or +Z (USD, if declared) | **+Z** |
| Forward | −Z | **+X** |
| Units | metres, or whatever the author chose | **centimetres** |
| Texture origin | bottom-left (OBJ, USD); top-left (glTF) | **top-left** |
| Matrices | — | row-major, **row-vector** (`v * M`) |

The position transform is the same one in all three importers, deliberately:

```
engine.x = -source.z
engine.y =  source.x
engine.z =  source.y
```

then scaled to centimetres. **Triangle winding reverses** with it, because the basis change has
determinant −1 — and in glTF reverses once more under a node that mirrors its mesh (a negative
determinant, e.g. a scale of −1), whose normals also take the node basis' inverse-transpose rather
than the basis itself. **`V` flips (`v -> 1 - v`) for OBJ and USD only**, whose texture origin is
bottom-left; glTF's UVs are already top-left origin, so they cross unchanged.

Sharing one convention means a model exported to two formats lands in the same place, and means
there is one piece of arithmetic in this codebase to get wrong instead of three. The map itself
(`(x,y,z) -> (-z,x,y)`, rotation `(z,-x,-y,w)`, scale `(z,x,y)`, metres to centimetres) is
`aver/formats/AxisMap.hpp`; the glTF, OBJ and Y-up USD importers and the Godot converter (`AverAssetC fabric`) all call it.
`ObjImport.cpp` and `UsdImport.cpp` both reference `GltfImport.cpp`'s basis change and winding-reversal code
for the shared transformation logic rather than restating it. **`UsdImport.cpp` implements its own `toEngine`**
handling both the Y-up and Z-up cases that OBJ and glTF never need.

**The USD case is different and is handled separately** — see below.

---

## glTF 2.0 / GLB — `GltfImport.hpp`

The oldest importer and the most complete: meshes, skeletons, skins, skeletal animation clips and object
animation clips. `.glb` and
`.gltf` are told apart by the GLB magic, not the extension. Buffers may be external files or base64
data URIs.

The editor exposes it through `SandboxContentBrowser.cpp`'s `importGltfToDir`, writing
`Content/Meshes/<base>.ocmesh`.

### Sockets: an Empty parented to a bone

glTF has no socket concept, so this is a **rule the importer applies**, not a field it reads — and it
is the way an artist already authors one in Blender or Maya. A node becomes a socket on the skeleton
when **all four** of these hold:

1. its parent is a joint of this skin — there is a bone to hang from;
2. it is not itself a joint — a bone is a bone;
3. it carries no `mesh`, `camera` or `skin`. **A skinned mesh is routinely parented under the
   armature**, and a looser rule would make every character export sprout a socket called `body`;
4. no descendant of it is a joint. glTF permits ordinary nodes *between* joints (the importer already
   walks to the "nearest ancestor that is also in this skin"), and such a spacer is part of the
   hierarchy rather than an attachment point. This is the condition a three-rule version misses.

The node's name becomes the socket name and its TRS becomes the offset, **through the same basis
change the bone rest transforms get** — anything else would put attachments in a mirrored place on an
otherwise correct rig. A duplicate name is renamed with a numeric suffix and noted, because
`OcSkeleton::socket()` returns the first match and the loser would otherwise be unreachable.

To add one: in Blender, `Add > Empty`, then parent it to a bone in Pose mode (`Ctrl-P > Bone`), name
it, and export. It arrives as a socket you can see and adjust in the engine's skeleton editor.

### One mesh, several nodes

Blender writes linked duplicates (`Alt+D`) and collection instances as one glTF mesh named by
several nodes. Each node becomes a placement; nodes that differ only by **translation** share one
imported mesh, and a node that **rotates or scales** it differently gets its own copy (`<name>_2`,
`<name>_3`…), because rotation and scale are baked into the vertices. Before 2026-09-29 every node
appended another copy into the same mesh, so each placement drew all of them.

### Object animation: a car on a route, a fan

Animation of a node that is **not a joint** is object motion, imported as an **object clip**: an ordinary
`.ocanim` flagged `kOcAnimObject | kOcAnimLoop`, with an empty `skeletonRef` and one track on bone 0
(translation + rotation, scale only if it moves). Before this, those channels were dropped, and a file
with no skin dropped every animation. Joint channels are unchanged: skeletal clips, `skeletons[0]` only.

One clip per glTF animation and **moving** node, named after the node — or `<animation>_<node>` when that
node moves in more than one glTF animation. A node moves when it has channels of its own, **or** when it
is a plain mesh node with none that sits *under* one that does: a static child rides on its animated
parent (a wheel on a car, a prop on a turntable), so it gets a clip of its own, with every ancestor
composed. A child whose world never changes (the ancestor's channels hold one value) gets none.
`GltfImportResult` carries `objectAnimations`, `objectAnimationNames` and `objectAnimationPlacement`
(the index of the node's mesh placement, or `-1` for a node with no mesh, such as an empty used as a
**route driver**). A node that carries a skinned mesh is skipped and noted, whether it has channels of its
own or only a moving ancestor: glTF ignores its own transform.

**Duration.** Every clip lasts its glTF animation's full length (the latest key of any of its channels),
not the node's own last key. A track that ends early holds its last pose (the sampler clamps past the
final key), so all the clips of one animation wrap together instead of looping out of phase.

**Zero scale.** A scale keyed to zero (or one axis flattened to zero) is written as a zero scale key, so a
node that shrinks to nothing stays shrunk. The rotation of a fully collapsed key cannot be recovered from
the matrix, so it borrows the neighbouring key's. Playback divides by the *start* pose, which a collapsed
one does not have, so a clip whose first key has a zero-scale axis is noted: start it (`animtime`) at a
time it is not collapsed.

**The rule.** The importer bakes a mesh node's rest rotation*scale `Q0` into that node's mesh copy and
exports only the rest translation as the placement. The clip stores `A(t) = W(t) * Q0^-1` in function
order (`Q0^-1` first, then `W`; the row-vector matrix is `Q0^-1 * W`), where `W(t)` is the node's world
transform with every ancestor composed, animated or not, and `Q0` is exactly what was baked into that
copy (identity for an empty). Playback uses only relative motion, `F(t) = B * A(t0)^-1 * A(t)` with `B`
the placement (`docs/formats/FORMAT_SPECS.md` §9.6), so a mesh at its imported placement, played from
`t0 = 0`, reproduces the authored motion — and a library mesh placed at any pose of a route driver's clip
follows that route with the right heading. Values go through the importer's own axis and unit
conversion, the same one node transforms and placements use.

**Sampling.** The node's world transform is sampled at the union of the key times of the node's and its
ancestors' channels, so differing per-channel timelines and animated parents are handled and a LINEAR
curve is exact at its keys. Where the product is not linear between those keys — an ancestor that
rotates or scales, or a CUBICSPLINE curve — each span is also sampled at 30 Hz. A STEP curve holds: a
track made only of STEP channels is a Step track, and a STEP channel mixed with interpolated ones gets a
sample just before each jump. A transform that cannot be a translation/rotation/scale triple (shear from
a non-uniformly scaled parent) is noted and the rest recovered.

`AverAssetC` writes each object clip as `<base>_<name>.ocanim` beside the skeletal clips, and puts the
`anim <clip>` token on the scene level's placement of a moving mesh node (see the `.ocworld` `anim`
tokens, `docs/formats/FORMAT_SPECS.md` §11), so an imported animated scene plays as authored. A
placement plays ONE clip: when a node moves in several glTF animations the FIRST one is attached, the
others are still written, and the tool logs which were not attached. Clip file names are de-duplicated
case-insensitively (Windows treats `Car.ocanim` and `car.ocanim` as one file). The placement is the
node's rest transform (its own TRS in the file), so author that equal to the first key; a clip whose
first key differs still plays, but starts from the rest pose rather than the first key.

### Base colour factor

glTF's `baseColorFactor` is **linear**; an `.ocmat` holds it **sRGB-encoded** (the pack step decodes
it with `pow(x, 2.2)`). The import cook encodes a linear factor on the way in
(`ImportedMaterial::baseColorFactorLinear`, set by glTF and USD), so an untextured 0.5 grey renders
at 0.5, not 0.22. Only the factor is affected — textures were always decoded correctly. OBJ's `Kd`
is **treated as sRGB** and crosses unconverted: `.mtl` names no colour space, and that is an
assumption (Blender's exporter is believed to write its linear base colour there). Materials
imported before this change keep the old, darker encoding until they are re-imported. FBX has the same
ambiguity: it states no colour space, so its factor crosses unconverted unless the import option
`fileColoursAreLinear` says otherwise.

### Separate roughness and metalness maps (FBX)

`.ocmat` has one packed slot (`metalRough`: G = roughness, B = metalness, the glTF layout). Formats that name
the two as separate files fill `ImportedMaterial::metallicTex` (only FBX does today), and the import policy
layer — `packSeparateMetalRough` in `ImportCook.cpp`, right after the opacity fold — decodes both, writes a
new PNG (G from the roughness map, B from the metalness map, R and A 255; a missing roughness map is a
constant plane from the factor; different sizes: the smaller is resampled to the larger by nearest texel),
points `metalRoughTex` at it, resets both factors to 1 and clears `metallicTex`. A pair that already is the
packed layout, and every material with no `metallicTex`, is untouched. The OBJ importer still takes
roughness and drops the metal map; that is its documented behaviour, not the pack step's.

---

## Wavefront OBJ + MTL — `ObjImport.hpp`

Written from scratch. OBJ is a line-oriented text format with about a dozen keywords that matter; a
dependency would cost more in build surface and licence review than the parser costs to own.

**Supported**

- `v` / `vt` / `vn`, and all four face-index spellings: `v`, `v/vt`, `v//vn`, `v/vt/vn`
- **Negative (relative) indices**, resolved against what has been declared *so far* — not against
  the end of the file, which is a common way to get this wrong
- N-gon faces, fan-triangulated
- `o` objects → separate meshes (`splitByObject`, on by default); `g` groups inside an object
- `usemtl` runs → submeshes, with a repeated material reusing its slot
- `mtllib` → `ObjMaterial`, including the PBR extensions `Pr` / `Pm` / `map_Pr` / `map_Pm` that
  Blender and Substance write, and the three spellings of a normal map (`norm`, `bump`, `map_Bump`)
- `map_*` option runs — `map_Kd -bm 0.2 -o 1 1 1 tex.png` yields `tex.png`, because the filename is
  what follows the options, not simply the last token

**One position with two normals becomes two vertices.** That is what a hard edge *is* in OBJ, and
merging by position instead would smooth every hard edge in the file — the classic OBJ import bug,
which looks like broken shading rather than a broken importer.

**Not supported, and reported in `result.unsupported`**

| | Why |
|---|---|
| Concave n-gons | Fan triangulation produces triangles outside a concave outline. Ear clipping in 3D needs a reliable face plane an arbitrary OBJ does not always give, and would fail *differently* rather than never. |
| Smoothing groups (`s`) | Normals come from `vn`, or are generated per split corner. |
| Freeform geometry (`curv`, `surf`, `cstype`, `vp`) | This is a polygon importer. |
| Vertex colours on `v` lines | `rhi::MeshVertex` is 32 bytes: pos3, nrm3, uv2. There is no colour channel. |

**Texture paths are returned exactly as written, unresolved.** Resolution is the caller's job,
because only the caller knows the project's content root — and an importer that silently rewrote a
path would hide the absolute-path bug `docs/PACKAGING.md` §116 exists to catch.

A file with no faces **fails** rather than returning an empty mesh.

---

## USD — `UsdImport.hpp`, `UsdCrate.hpp`

**Both encodings (USDA text and USDC binary), parsed natively.** Two entry points:

- `importUsd` reads **one text layer**, exactly as written.
- `importUsdStage` reads **a whole stage** from its root layer — sublayers, binary layers,
  references, inherits and `PointInstancer`s (below). `AverAssetC convert` uses this one.

Pixar's OpenUSD is Apache-2.0, so the licence is not the objection; the size is. It pulls TBB and a
hundred-megabyte build for what this engine wants from a `.usd` file, which is triangles, normals,
UVs and a transform.

### Encoding is sniffed from content, never the extension

`.usd` is legally either ASCII or binary, so trusting the name gets it wrong roughly half the time.

| Magic | Encoding | `importUsd` | `importUsdStage` |
|---|---|---|---|
| `#usda` | USDA | **imported** | **imported** |
| `PXR-USDC` | USDC binary crate (0.4.0+) | refused, naming it | **imported** (`UsdCrate`) |
| `PK\x03\x04` | USDZ zip | refused, naming it, explaining it is a zip to extract | same |

### Whole stages — `importUsdStage`

- **The layer stack**: the root's `subLayers`, strongest first. Binary layers are merged **by prim
  path** (children are the union, a field's value is the strongest opinion) — how a scene split into
  per-element files comes out whole. Text layers each contribute what they define.
- **References, payloads, inherits** resolve to the geometry they bring, recursively; a reference's
  asset path is relative to the layer that authored it.
- **Every layer in the root's units and up axis**, which is how USD composes a stage.
- **`PointInstancer`s**: each prototype is built **once**, as one mesh (its root transform baked in,
  USD's IncludeProtoXform), and each kept instance becomes a placement with its own rotation and scale.
  A prototype's typical instance scale is moved into the mesh so it is a sensible size on its own.
- **The instance budget** (`UsdImportOptions::maxInstances`, `maxInstanceTriangles`): a scattered stage
  can declare millions of instances. Kept instances are real ones at their real transforms; the budget
  is shared by sqrt(count) × drawn size, the triangle cap halves the costliest prototype and hands its
  share to cheaper ones, and a `focus` (the stage's first camera, or a point) keeps full density within
  `focusRadius` and thins as (radius / distance)³ beyond. Prototypes with at most `keepAllBelow`
  instances are kept whole. `0` means **no limit** on either budget, on the command line and in
  `UsdImportOptions` alike.
- **Thinning by what can be seen** (`UsdImportOptions::focusBySize`, on in `AverAssetC`'s foliage
  mode): each prototype keeps full density out to max(`focusRadius`, K × its drawn radius), thinning as
  (reach / distance)³ beyond, with K bisected so the kept count fills `maxInstances`. A 12 m tree stays
  dense 1.2–1.6 km out while 15 cm moss thins past the focus radius; one falloff for every prototype had
  kept 0.2% of Jungle Ruins' forests and left a quarter of the budget unspent (a prototype's keep
  saturates at 1, the falloff did not). The triangle cap is not applied in this mode.
- **Translucency maps** UsdPreviewSurface cannot name are found on disk beside the base colour
  (`<stem>_Translucency.*` / `_Transmission.*`, case-insensitive; Intel's Blender materials feed them into
  Transmission) and reduced to the material's `subsurfaceWeight` -- their mean over the texels the
  cutout keeps. The map itself is not written.
- **One file, one image**: texture paths are normalised before comparison (layers in different folders
  name a shared atlas by different relative paths), and byte-identical images share one file.
- **Kept instances become instanced FOLIAGE by default, not entities** — see
  [Instanced foliage](#instanced-foliage--foliage--ocinst) below for what that means and why.
- **Cameras** come back as viewpoints (`UsdImportResult::cameras`); `AverAssetC` writes the first as the
  level's `CAMERA` record.
- **The sun** (`UsdImportResult::sun`): a `DistantLight` is one outright; a `DomeLight` whose Radiance
  `.hdr` holds a distinct sun (peak over 20× the sky's mean) gives its brightest texels' direction —
  longitude `(u − 0.5)·360°` from the dome's +Z toward +X, pole turned onto the stage's up axis per
  `poleAxis`, then the light's transform. That orientation was checked against Intel's Jungle Ruins
  Karma renders (the mirrored reading lights the opposite pyramid faces). `AverAssetC` writes it as
  the level's `SUN` direction; colour and brightness stay the physical sky's.
- **`excludePrims`** (`--exclude /root/A,/root/B`) leaves prims and everything under them out — for
  sources that stack two versions of one surface, like Jungle Ruins' cinematic terrain tiles laid
  over four of its backdrop tiles.
- **A connected `roughness`/`metallic` takes the texture whole** (factor 1), not the unconnected
  defaults 0.5/0 — those halved every textured material's roughness before 2026-09-28.
- **Identical corners are welded** (`weldIdenticalVertices`, both entry points): the build makes one
  vertex per face corner, then merges corners whose position, normal and UV are bit-identical, so a
  smooth, continuously mapped surface shares its vertices while creases and UV seams keep theirs.
- **`honourSharpFace`** (default on): a mesh whose authored normals are flat per face while Blender's
  `primvars:sharp_face` calls those faces smooth gets smooth normals rebuilt — angle-weighted, by
  position (so a mesh split into per-quad islands still smooths across them), limited to faces within
  60° so real creases survive. Jungle Ruins' terrain arrives exactly that way and otherwise renders as
  a mosaic of triangles; any mesh without the contradiction keeps its authored normals.
- **`meshGroups`** names the folder of each mesh's source layer; `AverAssetC` writes a multi-folder
  stage into matching subfolders instead of one flat directory.
- **Purpose and visibility**: prims with `purpose` guide or proxy, or `visibility = invisible`, are skipped
  with their subtrees (collision, occlusion and lighting volumes in game data such as Activision's Caldera).
- **`!resetXformStack!`** is honoured everywhere a transform is composed (the stage walk, what a
  reference/payload/inherit brings in, nested instancers, nested arcs, text layers): the prim's world is
  its own local transform and its ancestors are dropped. Inside a prototype "world" is the prototype's
  space, so a reset prim there drops the ancestors up to the prototype root. A reference or payload's
  asset root now also supplies the prim's own xform (it used to be inherits and references only).
- **The text reader obeys `xformOpOrder`** (also `!invert!`, `!resetXformStack!`, suffixed ops such as
  `xformOp:translate:pivot`, `xformOp:orient`): only the listed ops apply, in USD's order. A prim with no
  `xformOpOrder` keeps the old fixed order (scale, rotate, transform, translate).
- **Overlapping `GeomSubset`s**: a face listed by more than one subset is built once, by the first;
  later listings are skipped and counted (`diagnostics.duplicateSubsetFaces`). Before this each overlap
  was a second copy of the triangle: z-fighting and double lighting.
- **Exact-coplanar duplicate triangles** across the slots of one mesh (same three vertex positions
  quantised to 0.01 mm, either winding) are reported per slot pair (`diagnostics.coplanar`).
  `UsdImportOptions::resolveCoplanar = DropLater` also removes the later slot's copy (a slot it empties
  loses its submesh). Meshes over 8M triangles are not scanned.
- **Exact placements**: a placement whose matrix has shear or a negative determinant (a mirror) is no
  position + quaternion + scale, so `UsdPlacement::exact` is set and `UsdPlacement::matrix` holds the
  true engine-space world matrix (row-major, row-vector, translation in `matrix[12..14]`). `rotation`/
  `scale` then hold only the nearest TRS.
- **Diagnostics** (`UsdImportResult::diagnostics`, as text via `usdImportReportText`): distinct prims
  skipped or re-posed by reason (`invisible`, `guide-proxy-purpose`, `inactive`,
  `nested-point-instancer`, `specializes`, `reset-xform-stack`) with up to 8 example paths each;
  duplicate-subset faces; coplanar pairs; meshes carrying `primvars:displayOpacity`/`displayColor`
  (never read). `UsdImportOptions::tracePrims` (path prefixes) logs the composition steps of those
  prims into `diagnostics.trace`.
- **`--material-map <file>`**: a stage with no materials but named `familyName = materialBind` subsets
  (Caldera) keeps each subset as its own submesh; each line `<pattern> <material stem>` (stem `-` drops
  the faces) binds the subsets that arrived with no material, and the mesh is regrouped to one submesh
  per material (`groupSubmeshesByMaterial`) before clustering. Matching is `fmt::MaterialMap`
  (`aver/formats/MaterialMap.hpp`): the pattern is matched against the lowercased leaf name split on `_`,
  as WHOLE tokens (trailing digits allowed, so `stain` hits `wall_stain_01` and not `metal_stainless`);
  `a_b` matches contiguous tokens; `*x*` is a substring, `x*` a prefix, `*x` a suffix, `*` anything. **The
  most specific matching rule wins** (more tokens; at the same count the earlier line), so
  `water_tank Tank` beats `water Water` wherever each sits, and among one-token rules the order still
  decides (list drop rules first). `--material-map-legacy` keeps
  the old first-substring-in-file-order behaviour. Every run writes `<out-dir>/<base>.materialmap.report.txt`:
  each distinct name with the rule line that won and its material, every dropped and every unmatched
  name, and the rules that won nothing; every mesh a drop empties is also logged.

`AverAssetC convert <root.usda> --out-dir … --content-dir …` takes `--instances-as foliage|entities`
(`foliage`), `--max-instances` (4000000 in `foliage` mode, 12000 in `entities` mode), `--max-instance-tris`
(0 — unlimited — in `foliage` mode, 150000000 in `entities` mode), `--focus camera|none|<x>,<y>`
(camera), `--focus-radius <cm>` (10000), `--keep-all-below <n>` (2000) and `--exclude <prim>[,<prim>…]`; `--instances-dir`, `--material-map-legacy` and `--import-report <file>` (the importer's diagnostics as text) are described below, and `--resolve-coplanar report|drop-later` and `--trace-prims <prim>[,<prim>…]` pass straight through to `UsdImportOptions`.

**The refusals are the point of the design.** A silent empty import looks identical to a model that
legitimately has no geometry, and it sends the user to look at the renderer. Every failure path here
returns `false` with a message that says what the file actually is.

### Instanced foliage — `FOLIAGE` / `.ocinst`

**Every kept `PointInstancer` instance is written as instanced foliage by default**, not as a `PLACE`/
`PLACEG` entity. This is what makes the instance budget above affordable at all: a scattered stage can
declare millions of instances (Intel's Jungle Ruins: 8.7 million), and a level built out of one
`scene::Entity` per instance is CPU-bound long before the budget gets anywhere near that count.
Instanced foliage costs none of that — it lives **outside** the entity/draw system entirely.

**What foliage is, precisely**: static, **ray-traced only** (one TLAS instance per row; not drawn in
raster mode yet, and never in `VoxiRenderer::draws_`), **no collision**, and **not individually
selectable** — there is no entity per blade of grass to select, move or delete on its own. A level
names its baked instance table with a `FOLIAGE <content-relative path>` record
(`fmt::OcWorldData::foliageFiles`, `modules/formats/include/aver/formats/OcWorld.hpp`), and the path
names a binary `.ocinst` file — `modules/formats/include/aver/formats/OcInstances.hpp` — an `AVR1`
container (the same one `.ocmesh`/`.ocland`/`.ocfoliage` use) holding a header, a string table of
prototype asset paths, a group table (one `{asset, flags, first, count}` run per prototype mesh) and
one bulk chunk of `12 f32` per instance: the engine's own row-vector world matrix
(`v * M`, translation in the last row) with its trailing `(0, 0, 0, 1)` column dropped —
`t[r*3 + c] = M[r][c]` for `r` in `0..3`, `c` in `0..2`. `AverAssetC` writes `<content>/<instances-dir>/<level>.ocinst`
(`--instances-dir <content-relative folder>`, default `Instances`, created if missing; the level's
`FOLIAGE` record is `<instances-dir>/<level>.ocinst`). The stem is the `.ocworld`'s, so a de-duplicated
`Foo_2.ocworld` gets `Foo_2.ocinst`, never a table silently misnamed after a level import that did
**not** overwrite an existing one; an `.ocinst` already in the folder is never overwritten either
(`_2`, `_3`, ...).

**Rows are built from the instance's quaternion and scale directly** (`Transform::toMatrix()`,
`aver/core/Math.hpp`), with no Euler round trip: `world::eulerDegFromQuat` is only lossy at gimbal lock,
and there it gives a `PLACE` entity the wrong orientation, which a foliage row has no reason to inherit.
A placement with **shear or a mirror** (`UsdPlacement::exact`) cannot be a position, Euler angles and a
scale at all, so its exact matrix is written as the row and it goes to the `.ocinst` even when it has
too few copies to count as scatter, or `--instances-as entities` is set.

**`--instances-as entities`** restores the pre-`.ocinst` behaviour verbatim: every kept instance goes
back to being an ordinary `PLACE`/`PLACEG` line with `nocollide`, and the instance budget defaults
revert to the old entity-sized numbers (12000 / 150000000 triangles) since each one now costs a real
draw. In `foliage` mode the triangle cap is **unlimited by default** (`0`) because instancing shares
one BLAS per prototype — the per-instance triangle cost that motivated capping entities does not apply
— and the instance count defaults to 4,000,000, hard-capped at 8,000,000
(`aver::voxi::VoxiRenderer::kMaxFoliageInstances`) since a table past that many rows only wastes
convert time and disk: the runtime's own `VoxiRenderer::setFoliage` truncates anything beyond it, with
its own warning, the moment the level loads.

Loading a `FOLIAGE` record into `voxi::VoxiRenderer`'s ray-traced instance buffers is
`aver::game::loadLevelFoliage` (`Runtime/include/aver/game/GameFoliage.hpp`), run by both hosts the
same way any other level record is.

### Stage metadata is read and honoured

USD records `upAxis` and `metersPerUnit` explicitly, so unlike OBJ there is no guessing:

- **Y-up stage** → the same `(-z, x, y)` as glTF and OBJ
- **Z-up stage** → up already agrees with the engine, so only the handedness flips: `(x, -y, z)`.
  Treating a Z-up stage as Y-up is the most common USD import bug and it lays the model on its side.
- `metersPerUnit` folds with the caller's `scale`. A stage in metres scales by 100; one already in
  centimetres (`metersPerUnit = 0.01`) scales by exactly 1.

### Supported

`UsdGeomMesh` `points`, `faceVertexIndices`, `faceVertexCounts`, `normals`, `primvars:st`;
`xformOp:translate` / `scale` / `rotateX|Y|Z|XYZ` (and the other axis orders) / `orient` / `transform`,
in `xformOpOrder`, composed down the prim tree;
`orientation` (`leftHanded` cancels the winding flip rather than doubling it); face-varying vs
per-point normals and UVs, distinguished by array length.

USD's matrices are row-major and pre-multiply row vectors — **the same convention as this engine** —
so a `matrix4d` copies across without a transpose.

### Not supported, and reported

`importUsd`: references, payloads, sublayers and `PointInstancer`s (use `importUsdStage`).
Both: variant sets · specializes · relocates · UsdLux lights · time-sampled attributes (the default is
used) · shading models other than `UsdPreviewSurface` · subdivision (the control cage imports
**unsubdivided**, and says so, because a faceted result otherwise reads as a broken model) ·
non-uniform scale on normals (applied without an inverse-transpose, so shading on that prim is
approximate)

A stage that parses but composes no `UsdGeomMesh` **fails** with exactly that message.

---

## FBX

**Implemented** — `FbxImport.hpp`, in `Aver.Formats.Fbx`, at parity with glTF: meshes, skeletons and skin
weights, skeletal and object animation, materials and textures, cameras. Options, what maps to what and
what is dropped: [FBX_IMPORT.md](FBX_IMPORT.md). Design: `docs/plans/wave9/FBX_IMPORT.md`.

The Autodesk FBX SDK is proprietary and forbidden by this project's permissive-dependency rule, so the reader
is [ufbx](https://github.com/ufbx/ufbx) (MIT, one `.c`/`.h` pair, its own inflate — FBX 7.x deflates its large
arrays and this tree vendors no zlib). It is vendored at `third_party/ufbx` (`README.aver.md` has the tag and
hashes) and is the only third-party code the feature adds. `importFbx` refuses a file that is not an FBX
before ufbx sees it, because ufbx also reads OBJ and `ObjImport` owns that.

- **Frame.** ufbx is asked for right-handed, +Y up, metres, which is glTF's frame, so FBX goes through the
  *same* map as glTF (`x = -z, y = x, z = y`, times `scale`, winding reversed once and once more per mirrored
  node, `V` flipped because FBX's texture origin is bottom-left). A model exported to FBX and to glTF lands in
  the same place. Any file axis or unit is converted; `unitMetersOverride` replaces the file's unit.
- **Skeleton.** The rest pose is the **bind pose** (the skin clusters' matrices), not the scene's frame-0
  pose; inverse binds are re-derived from it so a bind-pose skeleton skins to the identity. Skinned meshes are
  baked into bind-world space, translation included; static meshes keep glTF's rule (rotation and scale baked,
  translation becomes a placement). Influences: top 4 by weight, renormalised, **counted and reported**.
- **Animation.** `ufbx_bake_anim` per animation stack, one track per bone channel; stacks become clips named
  after the stack. A clip's root bone travelling more than `rootMotionMinTravelCm` flags it `RootMotion`
  (option **Auto**, inert without a `CRootMotion` component; **Off** restores glTF behaviour).
- **Materials.** FBX names roughness and metalness as two maps and `.ocmat` has one packed slot: the policy layer
  packs the pair (see below) instead of dropping the metalness the way OBJ does.
- **Records.** Every FBX import leaves `<stem>.ocimport` beside its outputs; the Content Browser's
  **Reimport** re-runs it with the stored options.
- **Vertex colour, authored tangent and a second UV set** are carried into the `.ocmesh` VXEX chunk (glTF `COLOR_0` / `TANGENT` /
  `TEXCOORD_1`, FBX vertex colour / tangent+bitangent / the second UV set, OBJ `v x y z r g b`); see `docs/plans/fabric/G4_RENDER.md` R1.
- **Not carried, and reported:** lights, bump/height maps, a third UV set, layered or
  procedural textures, UV transforms, dual-quaternion skinning, blend shapes (until the morph-target feature
  lands), in-between blend shapes, animated ancestors above a skeleton root, geometry caches, NURBS.

---

## PNG and other textures

**Already worked before any of the above.** `stb_image` is vendored at `third_party/stb`, and
`modules/platform/src/Image.cpp` is the single `STB_IMAGE_IMPLEMENTATION` translation unit in the
engine.

### The slot decides the colour space, never the filename

| Slot | Usage | Uploaded as |
|---|---|---|
| `baseColor`, `emissive` | `Colour` | `R8G8B8A8_UNORM_SRGB` |
| `normal` | `NormalMap` | linear, and sRGB is forced **off** regardless of options |
| `metalRough`, `occlusion` | `Data` | linear |

A normal map read as sRGB is a subtly wrong lighting response that looks like a shading bug rather
than a decode bug, which is why this is decided by slot in both resolvers' matching
`pbr::TextureSlot` switch (`SandboxApp.cpp` line 1167 and `GameContent.cpp` line 229, where both
map base colour and emissive to Colour, normal to NormalMap, and everything else to Data) and not by guessing from the name.

Mipmaps are generated on the CPU, filtering **in linear light** — sRGB taps are decoded with the
exact piecewise curve, averaged, and re-encoded. Alpha is always averaged linearly.

### The texture cache is keyed by reference *and colour class*

`MaterialSystem::resolveTexture` keys on `path-or-id | colourClass`. Two slots that read a file the
same way still share one upload; a PNG bound to **both** `baseColor` and `occlusion` gets two, because
one is sRGB-decoded and the other is not. Keying on path alone let whichever slot resolved first
decide the colour space for both — invisible per pixel and wrong everywhere, the worst shape a
rendering bug can have.

### A failed resolve is remembered, but not forever

Caching the `0` stops a material naming a missing texture from re-hitting the filesystem on every
dirty drain. But holding it for the process lifetime meant a texture dropped into the project after
startup never appeared. Now:

- a material that is **marked dirty** retries its failed references (it changed — that is exactly
  when a missing texture may have arrived)
- `MaterialSystem::forgetFailedResolves()` drops all of them, for a content refresh or hot reload

---

## Bringing in third-party assets

The importers accept anything in a supported format. The constraint is **licensing, not technical**.

**This repository accepts permissively-licensed content only** — MIT, BSD, Apache-2.0, zlib, CC0,
CC-BY with attribution.

> **There is no exception.** No proprietary SDK is in the tree.

That rules out:

- **Fab / Quixel / Megascans** under Epic's Content License. Usable in *your own* game under Epic's
  terms if you accept them; **not** committable to this repository, and not something the engine can
  ship as sample content.
- Anything requiring the Autodesk FBX SDK to read (the FBX importer uses ufbx, which does not).
- Asset-store content with "no redistribution" terms — which is most of it.

To bring in a model you have the rights to:

1. Export or convert to **glTF/GLB**, **FBX**, **OBJ**, or **USD** (text or binary). Blender exports all four.
   A third-party library is part of that route for FBX only: **ufbx** (MIT, `third_party/ufbx`); a new
   third-party dependency still needs its licence recorded in `README.md` and the payload's NOTICE.
2. Import it — the editor's importer writes `Content/Meshes/<name>.ocmesh`.
3. Put the source file under a `Source/` directory. `docs/PACKAGING.md` drops every `**/Source/`
   when staging a game, so editable source does not ship.
4. Write an `.ocmat` naming the textures. Keep texture paths **relative to the content root**.

---

## Generated foliage — `tools/MakeFoliage.cpp`

Because the licensing above rules out marketplace foliage, the tree meshes in this tree are
**generated**, which leaves no licence attached to the result and is deterministic — a forest built
from it is byte-identical on every machine.

```bash
MakeFoliage.exe "<project>\Content\Meshes" "<project>\Content\Maps\Forest.ocworld" "<project>\Content\Textures"
```

Writes `Tree_Pine`, `Tree_Oak` and `Bush_Small` as `.ocmesh`, the same geometry as `.obj` under
`Meshes/Source/`, the `T_Tree_BC/_MR/_N` PNG set, and optionally a forest level.

**One mesh per tree, not two.** The renderer draws a whole mesh with a single material —
`RHI.hpp`'s `IRenderContext::drawMesh(mesh, world, baseColor, metallic, roughness)` and nothing in the draw path
iterates submeshes. So bark and leaves cannot be two material slots on one entity. Both surfaces live
in one texture and the UVs are **banded**: trunks map into the left half, foliage into the right. One
draw call, correct colours. Splitting into trunk and canopy meshes would also work and would double
the draw calls — **the RHI does now have instancing** (`IRenderContext::drawMeshInstanced`,
`modules/rhi/include/aver/rhi/RHIResources.hpp`), but only the depth-only shadow and GI-shadow passes group draws by mesh and use it
(`VoxiRenderer::shadowPass`/its GI-shadow twin); the coloured forward draw a tree's foliage actually
shows on screen still issues one `drawMesh` per instance, so splitting one material per tree into two
would still double what a viewer's frame time pays for.

**The `.obj` output is not a convenience.** `emit()` re-imports every `.obj` it writes and compares
triangle count, slot count and bounds against the source. `writeObj` undoes the importer's basis
change by hand, so if either side of that pair is wrong the geometry comes back moved, mirrored or
inside out. It is how the OBJ importer gets checked against real geometry — hundreds of triangles
with UVs and normals — rather than only against hand-written triangles.

---

## Testing

`tests/formats/src/ImportTest.cpp` — 162 assertions over OBJ and USDA, exercising the
basis change in both directions, winding reversal, the V flip, hard-edge splitting, n-gon triangulation, negative indices,
`usemtl` slot reuse, `o` splitting, `.mtl` option skipping and PBR extensions, Y-up vs Z-up stages,
`metersPerUnit` folding, nested `xformOp` inheritance, and every refusal path.

`tests/formats/src/UsdStageImportTest.cpp` — `importUsdStage` over hand-written `.usda` stages: reset stack,
`xformOpOrder` in the text reader, overlapping subsets, coplanar report/`DropLater`, `tracePrims`, and the
exact-matrix flag (through `usd_detail::placementForMatrix`; a crate cannot be authored in a test).

`tests/formats/src/FbxImportTest.cpp` (with `FbxFixture.hpp`, which writes ASCII FBX 7.4 from code) — frames
and units, winding and mirrors, triangulation, hard edges, instancing, skin limits and bind pose, sockets,
animation tracks and root motion, object clips, materials and textures, cameras, the blend-shape extractor,
options text, determinism, and one run of `AverAssetC convert`. `FbxMaterialPackTest.cpp` covers the
roughness/metalness pack, `ImportRecordTest.cpp` the `.ocimport` record, and
`tests/editor/src/ModelImportDialogTest.cpp` the editor side (routing, the shared writer, Reimport).

`tools/MakeFoliage.cpp` — round-trips three real meshes through `.obj` on every run.

**Not covered by an automated test:** the texture cache's colour-class split. `MaterialSystem`
requires a GPU device (`init` returns false without one), so it cannot be exercised headless. It is
verified by construction and by a real run — loading SkyForge's forest shows `T_Tree_MR.png` uploaded
**once** for the two linear slots that share it, which is the behaviour the key change was for.
