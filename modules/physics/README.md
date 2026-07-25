# Aver.Physics  (`modules/physics`)

- **Language:** C++
- **Depends on:** Core
- **Planned phase:** 4

Rigid-body dynamics + collision (Jolt Physics, MIT) behind an AvPhysics facade: bodies, shapes, broadphase, raycasts (aero ride-height probe), contact callbacks.

**Implemented and wired.** Built as `Aver.Physics` (SHARED, so the C# layer P/Invokes one binary and
therefore one world), with Jolt linked `PRIVATE` — nothing above this module can include a `JPH::`
header, so replacing the backend stays a decision about this module rather than about the tree.

- **Frame loop:** `SandboxApp` starts the simulation in `onInit`, steps it in `onUpdate` between the
  `PRE_PHYSICS` and `PHYSICS` tick groups, and shuts it down first in `onShutdown` (Jolt owns worker
  threads, which must be joined while what their jobs touch is still alive).
- **Gameplay:** `AverCharacter` is a Jolt `CharacterVirtual` capsule — it falls, collides, reports
  grounded, and jumps. `Physics` and `Body` expose bodies and raycasts to C#.
- **Units and axes:** everything crossing this boundary is the ENGINE's contract (centimetres, +X
  forward, +Y right, +Z up, left-handed). See `src/Convert.hpp`; all four conventions differ from
  Jolt's, so the translation is real arithmetic, not a relabelling.
- **Evidence:** `tests/physics` (25 assertions) checks the change of basis by the property that
  defines it — `convert(rotate(q, v)) == rotate(convert(q), convert(v))` — which cannot hold by
  accident for a mirrored axis map. End to end, `--play-test` drops a character from 3 m and logs it
  accelerating to about 1 g, landing at exactly z=0, and reporting grounded.

See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.
