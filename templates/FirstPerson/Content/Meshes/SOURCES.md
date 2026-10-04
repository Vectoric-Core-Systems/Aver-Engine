# Where this art came from

## Blaster.ocmesh

| | |
|---|---|
| **Source** | Kenney *Blaster Kit* 2.1 — <https://kenney.nl/assets/blaster-kit> |
| **Original file** | `Models/GLB format/blaster-a.glb` |
| **Licence** | Creative Commons Zero (CC0 1.0) — <https://creativecommons.org/publicdomain/zero/1.0/> |
| **Imported** | 2026-08-18, via `Sandbox.exe --import`, 724 verts / 410 tris |

The full licence as shipped by Kenney is in `Blaster.LICENSE.txt` beside this file.

**Why the licence text is committed and not just linked.** This template ships inside the engine, so
the mesh is redistributed to everyone who downloads it. CC0 asks for nothing in return, but a
redistributed asset with no provenance in the tree is one nobody can later verify — and "we think it
was CC0" is not a thing you want to discover you cannot prove. The file costs a couple of kilobytes.

**What was NOT imported.** The GLB carries material definitions and textures; the importer keeps
material names as slots and imports neither. The blaster therefore renders with the template's own
`M_Gun` surface rather than Kenney's colours, which is also why this template still ships no textures
at all. The kit's `magazine` mesh imported cleanly too and was deleted — the body alone is the
viewmodel, and a second mesh nothing parents to would just be a file to explain.

## Character.ocmesh / ../Skeletons/Character.ocskel

| | |
|---|---|
| **Source** | Kenney *Animated Characters Survivors* 1.1 — <https://kenney.nl/assets/animated-characters-survivors> |
| **Original file** | the pack's rigged FBX character, converted to glTF2/GLB with assimpjs (this importer reads `.gltf`/`.glb` only, not FBX) before import |
| **Licence** | Creative Commons Zero (CC0 1.0) — <https://creativecommons.org/publicdomain/zero/1.0/> |
| **Imported** | 2026-08-23, via `Sandbox.exe --import-gltf`, 4812 verts / 1604 tris / 45 bones |

The full licence as shipped by Kenney is in `Character_LICENSE.txt` beside this file, for the same
reason `Blaster.LICENSE.txt` is above: this template redistributes the asset to everyone who downloads
the engine, and CC0 or not, provenance that lives only in a chat transcript is provenance nobody who
did not have that transcript can verify.

**What was NOT imported.** The pack ships its idle/run/jump clips as SEPARATE `.glb` files
(`Character_Idle/Run/Jump.glb`) that carry zero meshes — animation-only exports of the same rig. This
importer's `Gltf::run` refuses any file with no meshes before it ever reads a skin or an animation
(`GltfImport.cpp`: `"glTF: the file contains no meshes"`), so none of the three could be imported at
all; this is a hard gate in the importer, not a judgement call made in content. `AN_FPCharacter.ocgraph`
therefore renders the character in its imported bind pose — see that file's own COMP-record comment for
the scale measurement.

**The rest pose was wrong, and is not any more.** This rig stores its joints as baked `matrix` nodes
and hangs them under an armature node carrying the FBX-to-glTF axis and unit conversion. The importer
read neither, so every bone arrived at identity — 45 of 45 at translation (0,0,0) — and the skeleton
sat in a different space and orientation from the mesh it skins. Fixed in 9a25807; `Character.ocskel`
here was regenerated with it. Worth knowing if you compare this file against an older checkout: the
rendered frame is byte-identical either way (the re-derived inverse bind cancels whatever the rest
pose is), which is exactly why it went unnoticed and why nothing visual changed when it was corrected.

**The skin texture** (`Character_Skin.png`, in `../Textures/`) IS wired up, through
`../Materials/M_Character.ocmat`'s `TEX baseColor {path:Textures/Character_Skin.png}` — unlike the
gun, this character does not render in flat grey.
