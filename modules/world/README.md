# Aver.World  (`modules/world`)

- **Language:** C++
- **Depends on:** Scene, Render, Physics, Assets, Formats
- **Planned phase:** 3

The level/world runtime: .ocworld/.ocmap/.scene loading, object-reference placements, surface/ground/killz env, spawn, streaming; wires Scene<->Render<->Physics.

> Skeleton only — not yet wired into the top-level `CMakeLists.txt`. It will be
> added (`add_subdirectory(modules/world)`) when Phase 3 implements it.
> See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.
