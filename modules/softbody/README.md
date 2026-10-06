# Aver.SoftBody  (`modules/softbody`)

- **Language:** C++
- **Depends on:** Core (only)
- **Status:** implemented, not yet wired into the root build (see docs/SOFTBODY.md "Wiring")

The plastic half of soft bodies: a UE-free cage solver — Verlet, then damage/plasticity, then Gauss-Seidel
PBD — with a material yield curve (elastic to yield, then permanent set), break and tear that duplicates
particles along the cut, a render-mesh mapping that follows tears, crush-curve impacts, and an async
fixed-Hz worker. Plus a C ABI (`softbody_abi.h`). Spec: docs/recon/softbody-solver.md. Design and usage:
**docs/SOFTBODY.md**.

**Soft body for skeletal meshes and cloth that springs back is still Jolt's**, through `Aver.Physics`'s
plain-C ABI as `aver_phys_softbody_*`: it brings `Skinned` constraints and collision against the real
world, which this module does not. What Jolt cannot do — dents that stay, yield curves, break/tear — is
all this module is for. The two never share state and this module does not link `Aver.Physics`.

| File | What |
|---|---|
| `Material.hpp` | `Material`, `Behavior`, the crush curve, tear radius |
| `Cage.hpp` / `Cage.cpp` | particles, beams, triangles; build/repair; tear resolution; queries |
| `Solver.cpp` | `step`, `applyImpact` |
| `RenderBinding.hpp` | render vertices/triangles following tears |
| `AsyncSolver.hpp` | the fixed-rate worker and its snapshots |
| `softbody_abi.h` | the C ABI (`aver_sb_*`) |

Tests: `tests/softbody` (`PlasticSoftBodyTest`, `PlasticSoftBodyAsyncTest`).

See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.
