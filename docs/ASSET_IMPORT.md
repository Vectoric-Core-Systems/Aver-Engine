# Asset import

How third-party geometry and textures get into this engine, what each importer supports, and — the
part most import documentation leaves out — what each one **cannot** do and how it tells you.

Everything here is dependency-free except glTF, which was already. No importer in this tree links a
third-party library, and none of the supported formats requires one.

---

## The engine contract every importer converts into

| | Source formats | This engine |
|---|---|---|
| Handedness | right-handed | **left-handed** |
| Up axis | +Y (glTF, OBJ, USD default) or +Z (USD, if declared) | **+Z** |
| Forward | −Z | **+X** |
| Units | metres, or whatever the author chose | **centimetres** |
| Texture origin | bottom-left | **top-left** |
| Matrices | — | row-major, **row-vector** (`v * M`) |

The position transform is the same one in all three importers, deliberately:

```
engine.x = -source.z
engine.y =  source.x
engine.z =  source.y
```

then scaled to centimetres. **Triangle winding reverses** with it, because the basis change has
determinant −1. `V` flips (`v -> 1 - v`) because the texture origin moves.

Sharing one convention means a model exported to two formats lands in the same place, and means
there is one piece of arithmetic in this codebase to get wrong instead of three.
`ObjImport.cpp` and `UsdImport.cpp` both point at `GltfImport.cpp:173` and `:297` rather than
restating it.

**The USD case is different and is handled separately** — see below.

---

## glTF 2.0 / GLB — `GltfImport.hpp`

The oldest importer and the most complete: meshes, skeletons, skins and animation clips. `.glb` and
`.gltf` are told apart by the GLB magic, not the extension. Buffers may be external files or base64
data URIs.

The editor exposes it at `SandboxApp.cpp:4036`, writing `Content/Meshes/<base>.ocmesh`.

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

## USD — `UsdImport.hpp`

**The ASCII (USDA) encoding only, parsed natively.**

Pixar's OpenUSD is Apache-2.0, so the licence is not the objection; the size is. It pulls TBB and a
hundred-megabyte build for what this engine wants from a `.usd` file, which is triangles, normals,
UVs and a transform.

### Encoding is sniffed from content, never the extension

`.usd` is legally either ASCII or binary, so trusting the name gets it wrong roughly half the time.

| Magic | Encoding | Result |
|---|---|---|
| `#usda` | USDA | **imported** |
| `PXR-USDC` | USDC binary crate | refused, naming it, suggesting `usdcat -o out.usda in.usdc` |
| `PK\x03\x04` | USDZ zip | refused, naming it, explaining it is a zip to extract |

**The refusals are the point of the design.** A silent empty import looks identical to a model that
legitimately has no geometry, and it sends the user to look at the renderer. Every failure path here
returns `false` with a message that says what the file actually is.

### Stage metadata is read and honoured

USD records `upAxis` and `metersPerUnit` explicitly, so unlike OBJ there is no guessing:

- **Y-up stage** → the same `(-z, x, y)` as glTF and OBJ
- **Z-up stage** → up already agrees with the engine, so only the handedness flips: `(x, -y, z)`.
  Treating a Z-up stage as Y-up is the most common USD import bug and it lays the model on its side.
- `metersPerUnit` folds with the caller's `scale`. A stage in metres scales by 100; one already in
  centimetres (`metersPerUnit = 0.01`) scales by exactly 1.

### Supported

`UsdGeomMesh` `points`, `faceVertexIndices`, `faceVertexCounts`, `normals`, `primvars:st`;
`xformOp:translate` / `scale` / `rotateX|Y|Z|XYZ` / `transform`, composed down the prim tree;
`orientation` (`leftHanded` cancels the winding flip rather than doubling it); face-varying vs
per-point normals and UVs, distinguished by array length.

USD's matrices are row-major and pre-multiply row vectors — **the same convention as this engine** —
so a `matrix4d` copies across without a transpose.

### Not supported, and reported

references and payloads · variant sets · `PointInstancer` · `Material` / `Shader` prims ·
time-sampled attributes · subdivision (the control cage imports **unsubdivided**, and says so, because
a faceted result otherwise reads as a broken model) · non-uniform scale on normals (applied without
an inverse-transpose, so shading on that prim is approximate)

A stage that parses but composes no `UsdGeomMesh` **fails** with exactly that message.

---

## FBX

**Not implemented.** The Autodesk FBX SDK is proprietary and is therefore forbidden by this
project's permissive-dependency rule.

The viable route is [ufbx](https://github.com/ufbx/ufbx) (MIT, a single `.c`/`.h` pair, no
dependencies — it bundles its own inflate, which matters because FBX 7.x deflate-compresses its large
arrays and this tree vendors no zlib or miniz). Vendoring it means downloading source, so it is a
decision to take deliberately rather than a thing to slip in.

A from-scratch FBX reader is possible but would cover only ASCII FBX and *uncompressed* binary FBX —
which excludes most real-world files, including anything exported from Maya or Max with default
settings. That is why ufbx is the recommendation rather than another native parser.

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
than a decode bug, which is why this is decided by slot in both resolvers
(`SandboxApp.cpp:1167`, `GameContent.cpp:229`) and not by guessing from the name.

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
CC-BY with attribution. That rules out:

- **Fab / Quixel / Megascans** under Epic's Content License. Usable in *your own* game under Epic's
  terms if you accept them; **not** committable to this repository, and not something the engine can
  ship as sample content.
- Anything requiring the Autodesk FBX SDK to read.
- Asset-store content with "no redistribution" terms — which is most of it.

To bring in a model you have the rights to:

1. Export or convert to **glTF/GLB**, **OBJ**, or **USDA**. (Blender exports all three; `usdcat`
   converts USDC to USDA.)
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
`RHI.hpp:348` is `drawMesh(mesh, world, baseColor, metallic, roughness)` and nothing in the draw path
iterates submeshes. So bark and leaves cannot be two material slots on one entity. Both surfaces live
in one texture and the UVs are **banded**: trunks map into the left half, foliage into the right. One
draw call, correct colours. Splitting into trunk and canopy meshes would also work and would double
the draw calls, which matters because **there is no instancing in the RHI either**.

**The `.obj` output is not a convenience.** `emit()` re-imports every `.obj` it writes and compares
triangle count, slot count and bounds against the source. `writeObj` undoes the importer's basis
change by hand, so if either side of that pair is wrong the geometry comes back moved, mirrored or
inside out. It is how the OBJ importer gets checked against real geometry — hundreds of triangles
with UVs and normals — rather than only against hand-written triangles.

---

## Testing

`tests/formats/src/ImportTest.cpp` — 74 assertions over OBJ and USDA: the basis change in both
directions, winding reversal, the V flip, hard-edge splitting, n-gon triangulation, negative indices,
`usemtl` slot reuse, `o` splitting, `.mtl` option skipping and PBR extensions, Y-up vs Z-up stages,
`metersPerUnit` folding, nested `xformOp` inheritance, and every refusal path.

`tools/MakeFoliage.cpp` — round-trips three real meshes through `.obj` on every run.

**Not covered by an automated test:** the texture cache's colour-class split. `MaterialSystem`
requires a GPU device (`init` returns false without one), so it cannot be exercised headless. It is
verified by construction and by a real run — loading SkyForge's forest shows `T_Tree_MR.png` uploaded
**once** for the two linear slots that share it, which is the behaviour the key change was for.
