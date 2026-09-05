# Aver.GpuDeform  (`modules/gpudeform`)

- **Language:** C++
- **Depends on:** RHI, Render, SoftBody — **`Render` no longer names a real module: `modules/render/`
  was deleted (it was undocumented vaporware, per `docs/STALE_CODE.md`). The CPU half this module is
  meant to reproduce on the GPU now exists as its own built module, `Aver.Deform`
  (`modules/deform/`, CPU-only today), not as part of any render module — see its row in
  `docs/ARCHITECTURE.md` before wiring this one up.** `SoftBody` is still accurate as a *planned*
  dependency: `modules/softbody/` is itself still a skeleton (soft-body simulation proper landed on
  `Aver.Physics`'s C ABI instead — see `docs/ABI.md` §6 — leaving only plastic deformation for that
  module).
- **Planned phase:** 4

[opt] Compute cage-skin: K-nearest node blend -> mirrorY -> gain -> clamp -> rest+disp, UAV aliased as the position vertex stream. VehicleDeform.usf ported to HLSL/DXC. See docs/recon/gpu-deform.md.

> Skeleton only — not yet wired into the top-level `CMakeLists.txt`. It will be
> added (`add_subdirectory(modules/gpudeform)`) when Phase 4 implements it.
> See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.
