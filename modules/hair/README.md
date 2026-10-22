# Aver.Hair

Strand hair authoring and the bridge to Jolt 5.6's Hair. Design: `docs/plans/wave11/SOFTBODY_HAIR.md` part B; user guide: `docs/HAIR.md`.

Pure, like `Aver.SoftBody`: no scene, no GPU, no Jolt header. Depends on Core, Formats and the plain-C ABI of Aver.Physics.

| File | What it does |
|---|---|
| `HairGenerate` | The procedural groom: roots by dart rejection on a masked scalp, shape from the normal towards a style direction, clump guides and followers, curl, part. Deterministic from the seed (PCG32). |
| `HairCurves` | glTF `LINES` / `LINE_STRIP` curves (`fmt::ImportedHairCurves`) to explicit strands: at most 255 points, no coincident neighbours, a width per strand. |
| `HairProxy` | Convex hull points for sphere, capsule and box proxies, and the default humanoid proxy set from joint names. |
| `HairSettingsBuild` | An `.ochair` into `aver_phys_hair_settings_*`: the parameter blocks, the scalp (embedded or from an `.ocmesh`), strands (stored or generated), and the root-to-scalp check. |

The simulation is Jolt's, inside Aver.Physics (`physics_hair_abi.h`). The scene component and system are `Aver.Hair.Scene`; the drawing is
`Aver.Render.Hair` (later phases).

Tests: `tests/hair` (`HairGenerateTest`, `HairSettingsBuildTest`), `tests/formats` (`OcHairTest`), `tests/physics` (`HairTest`).
