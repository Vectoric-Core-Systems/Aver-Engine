# Aver.Physics  (`modules/physics`)

- **Language:** C++
- **Depends on:** Core, and `Aver.Physics.Jolt` **privately**
- **Planned phase:** 4

Rigid-body dynamics + collision behind an AvPhysics facade: bodies, shapes, broadphase, raycasts (aero ride-height probe), contact callbacks.

Implemented and wired. Built as `Aver.Physics` (SHARED, so the C# layer P/Invokes one binary and
therefore one world), with the backend linked `PRIVATE` — nothing above this module can include a
`JPH::` header, so replacing the backend stays a decision about this module rather than about the
tree.

## The backend is a sibling module: [`physics.jolt`](../physics.jolt/README.md)

This pair is `rhi` + `rhi.d3d12` again, and the naming is the same claim: the module here states a
contract, and the dotted one satisfies it. Jolt is to physics what WASAPI is to the mixer and D3D12
is to the RHI — the engine's backend, not a dependency the build happens to pull in.

Two properties follow from that split, and both are enforced by a link line rather than a convention:

- **The ABI above is expressible without the backend.** `physics_abi.h` names no `JPH::` type, and
  could not, because nothing that includes it links anything that defines one.
- **The exception is visible.** `tests/physics` links `Aver.Physics.Jolt` directly and is the only
  thing in the tree that does, because checking that the change of basis commutes with rotation
  needs to speak both sides. A second consumer appearing there would mean the rule had stopped
  holding.

Jolt's sources are **vendored** — MIT, 5.6.0, not edited here. The provenance, the conventions table
(all four of Jolt's differ from the engine's) and the update procedure live in that module's README.

- **Frame loop:** `SandboxApp` starts the simulation in `onInit`, steps it in `onUpdate` between the
  `PRE_PHYSICS` and `PHYSICS` tick groups, and shuts it down first in `onShutdown` (Jolt owns worker
  threads, which must be joined while what their jobs touch is still alive).
- **Gameplay:** `AverCharacter` is a Jolt `CharacterVirtual` capsule — it falls, collides, reports
  grounded, and jumps. `Physics` and `Body` expose bodies and raycasts to C#.
- **Units and axes:** everything crossing this boundary is the ENGINE's contract (centimetres, +X
  forward, +Y right, +Z up, left-handed). See `src/Convert.hpp`; all four conventions differ from
  Jolt's, so the translation is real arithmetic, not a relabelling.
- **Tests:** `tests/physics` has 413+ assertions across 9 suites. `PhysicsTest.cpp` checks the change
  of basis by the property that defines it — `convert(rotate(q, v)) == rotate(convert(q), convert(v))` —
  which cannot hold by accident for a mirrored axis map. `CharacterTest` verifies character-on-ground
  behaviour (`aver_phys_character_grounded`/`ground_state`, kerb step-up, max slope angle). The
  `--play-test` harness in `SandboxApp` begins Play, holds synthetic `W` for 150 frames and logs where
  the pawn walked to, verifying that movement works end-to-end.

See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.
