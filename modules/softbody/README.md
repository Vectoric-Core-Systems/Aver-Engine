# Aver.SoftBody  (`modules/softbody`)

- **Language:** C++
- **Depends on:** Core
- **Planned phase:** 4

[opt] The OpenConstructor cage solver, UE-free: Verlet -> damage/plasticity -> Gauss-Seidel PBD, break/tear, crush-curve impacts, async fixed-Hz worker. See docs/recon/softbody-solver.md + docs/physics-net/.

> Skeleton only — not yet wired into the top-level `CMakeLists.txt`. It will be
> added (`add_subdirectory(modules/softbody)`) when Phase 4 implements it.
> See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.
