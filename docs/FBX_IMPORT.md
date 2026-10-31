# FBX import

How to bring a `.fbx` into a project, what the options do, what maps to what, and what is dropped. The
contract it was built to is `docs/plans/wave9/FBX_IMPORT.md`; the shared importer conventions are in
[ASSET_IMPORT.md](ASSET_IMPORT.md).

FBX is read by [ufbx](https://github.com/ufbx/ufbx) (MIT, `third_party/ufbx`). It runs in the editor and in
`AverAssetC`; a packaged game never contains it.

## Ways in

- **Content Browser -> Import Asset** with an `.fbx`: an **FBX options** panel appears under the path.
- **Drag a file onto the Content Browser:** imports with the defaults, like glTF.
- **`Sandbox --import-fbx <src> <destDir> [options-text]`:** import and exit. The options text is the same
  `key=value;key=value` form the record stores (below); leave it out for the defaults.
- **`AverAssetC convert model.fbx --out-dir <dir> --content-dir <dir>`:** the launcher's route. FBX-only flags:
  `--scale <f>`, `--unit-meters <f>`, `--root-motion off|auto|all`, `--bake-fps <n>`, `--no-animations`,
  `--no-blend-shapes`. `--import-report <file>` writes the importer's report and its list of unsupported features.
- **Script / Aver Node:** `ModelImport.Import(source, destFolder, options, overwrite)` in C#, and the **Import
  Model** / **Is Model Import Available** nodes (category Assets). Import is an editor-time act, so in a packaged
  game these answer "unavailable".

## Options

| Option | Default | Text key | |
|---|---|---|---|
| Scale | 100 | `scale` | Centimetres per metre, applied after ufbx has converted the file to metres. |
| Unit override | 0 | `unitMeters` | Greater than 0 ignores the file's unit and treats one file unit as this many metres. |
| Convert axes | on | `convertAxes` | Off leaves the authored axes (still metres times scale). |
| Generate missing normals | on | `generateNormals` | ufbx smooth normals first, flat after. |
| Meshes / Skeletons / Animations / Materials / Blend shapes | on | `meshes` `skeletons` `animations` `materials` `blendShapes` | |
| File colours are linear | off | `linearColours` | FBX states no colour space; the default treats factors as sRGB, like OBJ. |
| Bake FPS | 30 | `bakeFps` | Resample rate for non-linear curves. |
| Reduce keys | on | `reduceKeys` | ufbx key reduction. |
| Root motion | Auto | `rootMotion` | Off, Auto or All, below. |
| Auto travel threshold | 10 | `rootMotionMinCm` | Planar travel of the root bone over the clip. |
| Memory limit | 2048 | `maxMemoryMiB` | Handed to ufbx's allocator limits. |

An unknown key is an error that names it.

## What maps to what

**Frame.** Same map as glTF: `x = -z, y = x, z = y`, times `scale`, triangle winding reversed once (and once
more under a node that mirrors its mesh), `V` flipped. Z-up, Y-up, any unit.

**Meshes.** Faces are triangulated by ear clipping (concave n-gons are fine). One mesh per distinct
(geometry, node basis, material set); nodes sharing a geometry with the same basis become placements of one
mesh, a rotated one gets a `_2` copy. A hard edge stays split, a smooth surface welds. Hidden nodes are skipped.
Two UV sets, vertex colour and the file's own tangents (tangent + bitangent) are kept in the `.ocmesh` VXEX chunk (R1 of [G4_RENDER.md](https://github.com/Vectoric-Core-Systems/Fabric-Connector/blob/main/docs/plans/G4_RENDER.md)); a third UV set is reported.

**Skeletons.** Bones are the skin clusters' bones, the chain nodes between them, and bone-typed descendants (end
bones). One skeleton per topmost bone. The rest pose is the bind pose. Influences are the top 4 by weight,
renormalised; the import report says how many vertices had more and the largest weight dropped. A non-bone
child of a bone (a locator) becomes a **socket**; a mesh child does not. An animation-only FBX (bones, no skin)
still yields its rig.

**Animation.** One clip per animation stack (and per skeleton the stack animates), named after the stack; keys
are baked to linear. A channel equal to its rest value emits nothing. Moving non-bone nodes become object clips
named after the node.
**Root motion:** *Off* never flags a clip; *All* flags every clip with a track on the root bone; *Auto* flags a
clip whose root bone travels more than the threshold in the plane. The flag only acts on an entity with
`CRootMotion`, so Auto changes nothing for existing content. Keys are never edited.

**Materials.** Read through ufbx's PBR view, so Lambert, Phong, Arnold, 3ds Max, glTF-material and OSL shaders
share one reader. Base colour, roughness (a Phong exponent gives `sqrt(2/(Ns+2))`), metalness (0 when absent,
not glTF's 1), normal, emission, occlusion, opacity (a map folds into base colour alpha, `MASK`; a value below 1
is `BLEND`), subsurface. Roughness and metalness maps are packed into the one slot `.ocmat` has.

**Textures.** Embedded data is taken verbatim. External files are searched next to the model, in `textures/`,
and in `<name>.fbm/`; a path that climbs out of the model folder's parent is refused. A texture that cannot be
found leaves the slot unset and is named in the report.

**Cameras.** The first camera becomes the level's start camera. Lights are reported, not imported.

## What is dropped (and said so)

Lights; a third UV set; bump/height maps (they are not converted to normal maps); layered,
procedural and shader-graph textures (the first file texture is used); texture UV transforms; sheen, clear coat,
thin film, anisotropy, transmission, displacement; dual-quaternion skinning (imported as linear); blend shapes
(reported until the morph-target feature lands, then carried as sparse deltas with `morph.<channel>` curves);
in-between blend shapes; animated nodes above a skeleton root (sampled at rest); rigid attachment of an unskinned
mesh to a bone (a static placement); geometry caches; NURBS. More than 256 bones in a skeleton is reported.

## The import record and Reimport

Every FBX import writes `<stem>.ocimport` beside its outputs: source path (project-relative when inside the
project), source size and time, the options text, and one `OUTPUT` line per file written. Right-click the
`.ocimport`, or any file it names, in the Content Browser:

- **Reimport** re-runs the import with the stored options; the files it replaces go to the recycle bin.
- **Import options...** reopens the options, then reimports.

A record whose source file is missing disables the action and shows the path. Record lines this version does
not know are kept when it rewrites the file. `*.ocimport` is plain Content and is not packaged into a game.

## Refusals

A file that is not an FBX is refused before ufbx sees it. A corrupt file fails with ufbx's own message in the
notification and the Output Log; the editor stays up. A hostile file hits ufbx's memory and node-depth limits
instead of running away.
