# Aver.SoftBody  (`modules/softbody`)

- **Language:** C++
- **Depends on:** Core
- **Planned phase:** 4

[opt] The OpenConstructor cage solver, UE-free: Verlet → damage/plasticity → Gauss-Seidel PBD, break/tear, crush-curve impacts, async fixed-Hz worker. See docs/recon/softbody-solver.md + docs/physics-net/.

> **Still a skeleton, and for a narrower reason than before.**
>
> **Soft body for skeletal meshes does NOT live here.** It is Jolt's, exposed through `Aver.Physics`'s plain-C ABI as `aver_phys_softbody_*` (see `modules/physics/include/aver/physics/physics_abi.h`). Jolt 5.6 is vendored and already compiling in this tree, and its soft body brings `Skinned` constraints — a particle tethered to where ordinary bone skinning would have put it, free to move up to a max distance — which is exactly the feature, plus **collision against the real world**, which a cage solver of our own would not have. `tests/physics/src/SoftBodyTest.cpp` is the evidence.
>
> **What Jolt cannot do, and is therefore all this module would still be for:** Jolt soft bodies are purely **elastic**. No plastic deformation, no permanent set, no material yield curve, no break/tear, no `.ocbeam`. A dent that *stays* is a genuinely different solver, and that — not soft body in general — is what `docs/recon/softbody-solver.md` specifies.
>
> This stays unwired (`add_subdirectory(modules/softbody)` is still absent from the top-level `CMakeLists.txt`) until vehicle damage is actually wanted. Anyone reaching for "soft body" before then wants the Jolt path above, and building a second elastic solver here would be duplicating something that already works.
>
> See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.
