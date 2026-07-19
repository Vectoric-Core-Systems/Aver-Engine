# Aver.Render.GI  (`modules/render.gi`)

- **Language:** C++
- **Depends on:** Render
- **Planned phase:** 3

[opt] Global illumination: surfel/probe GI + screen-space GI baseline, optional DXR ray-traced reflections/GI. The UE5-Lumen / Source2 tier, behind a feature gate.

> Skeleton only — not yet wired into the top-level `CMakeLists.txt`. It will be
> added (`add_subdirectory(modules/render.gi)`) when Phase 3 implements it.
> See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.
