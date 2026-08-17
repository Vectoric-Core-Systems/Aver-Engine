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
