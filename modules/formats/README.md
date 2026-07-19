# Aver.Formats  (`modules/formats`)

- **Language:** C++
- **Depends on:** Assets, Core, Platform
- **Status:** implemented (Phase 2) — `.ocbeam` + `.ocmap` loaders

Faithful, tolerant text loaders for the carried-over OpenConstructor formats:

- **`OcBeam.hpp` / `.cpp`** — `.ocbeam` (breakable deformable cage): materials (10–13
  fields), nodes, beams, panels, parts; skips GLB/rig/collision blocks; applies
  SCALE/NORMALIZE; adds the new `OBJECTID`. Verified against Ferrari499P (10-field) and
  Puegot9x8EVOChassis (13-field).
- **`OcMap.hpp` / `.cpp`** — `.ocmap` (world container): identity/surface/env +
  PLACE/DEFORM placements, positions as **f64**, per-placement Object IDs. Faithful to
  the authoritative `OcMap.cs`. Verified against demoworld/openworld.

Golden test: `tests/formats` → `FormatTest.exe`. `.ocaero`, `.scene`, and the new binary
formats (`.ocmesh`, …) follow in later phases. See [docs/formats/DECISIONS.md](../../docs/formats/DECISIONS.md).
