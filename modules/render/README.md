# Aver.Render  (`modules/render`)

- **Language:** C++
- **Depends on:** RHI, Assets, Core
- **Planned phase:** 3

Render graph, render-scene, clustered deferred + forward+ hybrid, PBR metallic-roughness materials, virtualized shadows, TAA/FSR, HDR/tonemap/post. Hosts CPU deform parity.

> Skeleton only — not yet wired into the top-level `CMakeLists.txt`. It will be
> added (`add_subdirectory(modules/render)`) when Phase 3 implements it.
> See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.
