# Aver.SoftBody.Scene  (`modules/softbody.scene`)

- **Language:** C++
- **Depends on:** Core, Formats, Physics, Scene, SoftBody, Anim, Anim.Scene
- **Option:** `AVER_MODULE_SOFTBODY_SCENE` (ON)
- **Status:** written against the frozen design; not yet compiled or run

The join between the pure cloth/destruction library (`Aver.SoftBody`), the scene and the Jolt soft-body
ABI (`aver_phys_sb_*`). It owns the `CCloth` and `CDestructible` components, the `SoftBodySystem` tick,
the damage inbox, debris pieces, events, and the `aver_fw_softbody_*` framework provider.
Design and schema: **docs/SOFTBODY.md**.

| File | What |
|---|---|
| `SoftBodySystem.hpp` | The public API: components, `Options`, inbox, `PieceView`, events, `softBodySystem()` |
| `SoftBodySystem.cpp` | Lifecycle, entries, the tick order, mesh and asset caches, outputs |
| `SoftBodyBuild.cpp` | Cage to builder arrays, body creation, rebuild with a state map |
| `SoftBodyDamage.cpp` | Impacts, deposits, strain, in-place dents, the tear pipeline, pieces and debris |
| `SoftBodyCloth.cpp` | Palette refresh, skin pushes, wind, character proxies, LOD and freezing |
| `SoftBodyProvider.cpp` | The framework relay provider (`softBodyProvider`) |

Tick order is the determinism contract: entities ascending by id, then pieces. Nothing here draws;
`Aver.Render.SoftBody` reads `pieces()`. Outside Play nothing steps.

Tests: `tests/softbody.scene` (`SoftBodySystemTest`, with fixtures in `SoftBodyFixtures.hpp`).
