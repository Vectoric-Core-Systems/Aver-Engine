# Format decisions (authoritative)

Project-owner directives that pin/override the broader design in
[FORMAT_SPECS.md](FORMAT_SPECS.md). These are what the loaders actually implement.

## Naming

Every format is `.oc<suffix>`:

| Ext | Asset | Status |
|---|---|---|
| `.ocbeam` | breakable/deformable skeletal (soft-body cage) mesh | carry-over + Object ID |
| `.ocmap` | world container (references + positions) | carry-over, doubles |
| `.ocmesh` | static mesh | new (Phase 3+) |
| `.octex` `.ocmat` `.ocskel` `.ocanim` `.ocprefab` | texture / material / skeletal / animation / prefab | new (later) |
| `.ocaero` | baked aero table | carry-over |

## `.ocbeam`

- **Unchanged** from the existing format (materials, nodes, beams, panels, parts, plus
  the skipped GLB/rig/collision blocks) — existing files load as-is.
- **One added parameter: `OBJECTID`** — a unique 64-bit id for the object. Optional
  header directive `OBJECTID 0x<hex>` (decimal also accepted). When absent (all existing
  files), it is derived stably from the filename via FNV-1a-64, so old content still gets
  a stable id.
- **MATERIAL rows: accept 10–13 fields.** Fields 0–9 (`Name, Stiffness, AxialStiffness,
  BendForceN, BreakForceN, PlasticStiffness, MaxBend, BendAbsorb, BreakAbsorb, Behavior`)
  are identical in every file (Behavior always last of the canonical 10). Newer files
  (e.g. `Puegot9x8EVOChassis.ocbeam`) append `tearStrainTension, tearStrainCompression,
  density`; these are preserved. Verified against real content: Ferrari = 10, Puegot = 13.
  (The OC runtime required exactly 10 and dropped the Puegot's materials — Aver does not.)

## `.ocmap`

- **A container of references + positions** — identity block (`ID`/`NAME`/`BUILD`/`ALGO`/
  `ROOT`/`CLIENT`), a `SURFACE` grip table, `GROUND`/`KILLZ`/`SPAWN`, and `PLACE`/`DEFORM`
  placement rows (asset reference + transform). No baked geometry.
- **Positions/rotations stored as doubles (f64)** for large-world precision — the
  authoritative C# reader used `float`; Aver improves this. `ID` is FNV-1a-64 of `NAME`
  (verified `demoworld → 0x376B85BC4D1A03BA`).
- Each placement resolves an **Object ID** (explicit if present, else `fnv1a64(asset)`),
  so map references and object ids line up with `.ocbeam`'s Object ID.
- Reader is faithful to `OcMap.cs`: `PLACE` has an optional 9th `surfaceId`; `DEFORM`
  material defaults to `default`; `#` truncates to end-of-line; one trailing `;` tolerated;
  mixed LF/CRLF accepted. Server-load invariants (NAME, ID≠0, ROOT≠0, GROUND-or-a-placement)
  are checked and reported by `ocmapIsServerValid()`.

## `.ocmesh` and beyond

Static meshes and the other new types follow the container design in FORMAT_SPECS.md,
implemented alongside the renderer/import pipeline in later phases.
