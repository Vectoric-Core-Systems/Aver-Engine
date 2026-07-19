# Aver.Physics  (`modules/physics`)

- **Language:** C++
- **Depends on:** Core
- **Planned phase:** 4

Rigid-body dynamics + collision (Jolt Physics, MIT) behind an AvPhysics facade: bodies, shapes, broadphase, raycasts (aero ride-height probe), contact callbacks.

> Skeleton only — not yet wired into the top-level `CMakeLists.txt`. It will be
> added (`add_subdirectory(modules/physics)`) when Phase 4 implements it.
> See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.
