# Aver.SoftBody.Scene  (`modules/softbody.scene`)

- **Language:** C++
- **Depends on:** Core, Formats, Physics, Scene, SoftBody, Anim, Anim.Scene
- **Option:** `AVER_MODULE_SOFTBODY_SCENE` (ON)
- **Status:** wave 9 built and tested; the wave 10 depth files (fields, caps, effects, save, proxy) are written but not yet compiled or run

The join between the pure cloth/destruction library (`Aver.SoftBody`), the scene and the Jolt soft-body
ABI (`aver_phys_sb_*`). It owns the `CCloth`, `CDestructible` and `CSoftField` components, the `SoftBodySystem` tick,
the damage inbox, debris pieces, events, and the `aver_fw_softbody_*` and `aver_fw_softfield_*` framework providers.
Design and schema: **docs/SOFTBODY.md**.

| File | What |
|---|---|
| `SoftBodySystem.hpp` | The public API: components, `Options`, inbox, `PieceView`, events, `softBodySystem()` |
| `SoftBodySystem.cpp` | Lifecycle, entries, the tick order, mesh and asset caches, outputs |
| `SoftBodyBuild.cpp` | Cage to builder arrays, body creation, rebuild with a state map |
| `SoftBodyDamage.cpp` | Impacts, deposits, strain, in-place dents, plasticity, pieces, debris lifecycle and baking |
| `SoftBodyTear.cpp` | The tear pipeline: `planTear` (topology and piece table only) and `commitTear` (bodies, entities, events, fx) |
| `SoftBodyFields.cpp` | Pulses, component and transient fields, hold and release, spawned-field lifetimes |
| `SoftBodyCaps.cpp` | Cap geometry per piece, the cap child entity, cap LOD |
| `SoftBodyFx.cpp` | Break effects: particle requests through `EffectSpawn.hpp`, the sound sink, cooldowns |
| `SoftBodySave.cpp` | `saveState` / `restoreState`, tear-log replay |
| `SoftBodyProxy.cpp` | Proxy cage simulation and the full mesh following it |
| `SoftBodyCloth.cpp` | Palette refresh, skin pushes, wind, character proxies, LOD and freezing |
| `SoftBodyProvider.cpp` | The framework relay providers (`softBodyProvider`, `softFieldProvider`) |

Tick order is the determinism contract: entities ascending by id, then pieces; fields run pulses, component fields, then transient fields. Nothing here draws;
`Aver.Render.SoftBody` reads `pieces()`. Outside Play nothing steps.

Tests: `tests/softbody.scene` (`SoftBodySystemTest`, `SoftBodyDepthTest`, `SoftBodyCalibrationTest`, fixtures in `SoftBodyFixtures.hpp`).
