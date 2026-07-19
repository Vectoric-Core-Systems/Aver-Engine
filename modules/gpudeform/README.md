# Aver.GpuDeform  (`modules/gpudeform`)

- **Language:** C++
- **Depends on:** RHI, Render, SoftBody
- **Planned phase:** 4

[opt] Compute cage-skin: K-nearest node blend -> mirrorY -> gain -> clamp -> rest+disp, UAV aliased as the position vertex stream. VehicleDeform.usf ported to HLSL/DXC. See docs/recon/gpu-deform.md.

> Skeleton only — not yet wired into the top-level `CMakeLists.txt`. It will be
> added (`add_subdirectory(modules/gpudeform)`) when Phase 4 implements it.
> See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.
