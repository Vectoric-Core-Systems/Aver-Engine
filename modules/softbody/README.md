# Aver.SoftBody  (`modules/softbody`)

- **Language:** C++
- **Depends on:** Core, Formats (for `Json.hpp` and the file helpers)
- **Status:** implemented; driven by `Aver.SoftBody.Scene` (modules/softbody.scene)

The pure topology, damage and asset library behind cloth and destruction. **Jolt owns the physics**
(soft bodies through `Aver.Physics`' `aver_phys_sb_*` ABI); this module owns what Jolt has no notion of:
the cage, the material yield model, tear resolution into duplicate particles, island planning, the render
binding that follows tears, and the `.ocsoft` asset. No scene, no GPU, no Jolt, so `tests/softbody`
decides all of it with no device. The Verlet/PBD solver, its async worker and `aver_sb_*` are retired.
Design: **docs/SOFTBODY.md**.

| File | What |
|---|---|
| `Material.hpp` | `Material` (yield, break, toughness, Jolt compliance), `SoftPreset` and the preset table, crush curve |
| `Cage.hpp` / `Cage.cpp` | particles, beams, dihedral bends, triangles; build/repair; tear resolution; masses |
| `SimMesh.hpp` | weld a render mesh into particles, build the cage and its `RenderBinding`, pins and reach from paint, rules or a mask |
| `Damage.hpp` | `evaluateStrain` (creep, snap), `depositEnergy`, `tearWithin`, `repairAll` |
| `Islands.hpp` | connected pieces after a tear, anchored first, dust below the minimum |
| `RenderBinding.hpp` | render vertices and triangles following tears |
| `SoftAsset.hpp` | `.ocsoft` read/write, validation, the paint run-length codec |

Tests: `tests/softbody` (`SoftBodyTest`, `SoftAssetTest`, `SimMeshTest`, `DamageTest`, `IslandsTest`).
