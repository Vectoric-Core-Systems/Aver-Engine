# Aver.Render.Hair  (`modules/render.hair`)

- **Language:** C++ and HLSL
- **Depends on:** Core, RHI (generic, never a backend), Scene, Formats, Hair, Hair.Scene, Physics (headers only), Render.PBR
- **Option:** `AVER_MODULE_RENDER_HAIR` (ON)

Draws strand hair and runs Jolt's hair simulation on the GPU. The simulation, the components and the per-instance rules are `Aver.Hair.Scene`'s; this
module is the GPU half. Design: `docs/plans/wave11/SOFTBODY_HAIR.md` 2.10, 2.11, 2.16, 2.17; operation and checks: `docs/HAIR.md`.

## RhiComputeHost -- Jolt's compute, on Aver's RHI

Jolt 5.6's `Hair::Update` only RECORDS compute commands into a queue it is given. `Aver.Physics` forwards them, one to one, to a plain-C table
(`AverPhysComputeHostV1`, `physics_hair_abi.h`); `RhiComputeHost` is the other end. Its callbacks touch CPU memory only (they may run on a worker); a
frame's `replay(ctx)` turns the sealed batches into RHI commands on the render thread. Jolt's own DX12 and Vulkan compute backends stay OFF: they want
a private queue and a private resource-state tracker, and the RHI cannot hand out its native device (the design lists the reasons). One code path,
so D3D12 and Vulkan agree by construction. Details of buffers, barriers and binding sets are in the header.

## The shaders

Jolt's fifteen kernels are compiled UNCHANGED by Aver's DXC pipeline. `shaders/AverJolt<Name>.hlsl` (generated) defines `JPH_SHADER_OVERRIDE_MACROS`,
includes `AverHairMacros.hlsli` (the macro block of Jolt's `ShaderCore.h` with `: register(AVER_REG_<name>)` added) and the generated
`AverJolt<Name>.regs.hlsli`, then the vendored kernel, which the module's CMake copies flat into `<exe>/shaders`. `scripts/gen-hair-shader-regs.py`
writes the wrappers, the register headers and `src/HairRegs.inc`; run it when Jolt is updated. `tests/render.hair/HairRegsTest` re-derives all of it from
Jolt's `*Bindings.h` and fails on any difference. The wind kernel is `modules/physics/shaders/hair/AverHairWind.hlsl` (one source, also compiled as C++
for the CPU backend).

## RibbonPack, AverHairRibbons.hlsl, HairScene

Strands draw as camera-facing ribbons written by one compute pass into a `rhi::MeshVertex` skin-target mesh and drawn by the ordinary mesh draw. The
layout puts the strands in a seeded random rank order so any prefix is a uniform thinning: the four levels of detail are index prefixes (posed-part
meshes) of the same vertices. A strand thinner than a pixel is drawn one pixel wide and keeps its width as coverage, spent stochastically per strand per
frame (temporal anti-aliasing integrates it). `RibbonPack.hpp` holds the layout builder and the C++ twin of the shader; `HairScene` is the render feature.

## Shading

`AVER_MODEL_HAIR` is in the shared evaluator (`modules/render.pbr/shaders/material_prelude.hlsl`), selected by `MaterialFlag_Hair` on the material, not by
the per-draw shading-model constant, so neither backend changes. See `docs/HAIR.md`.

## Ray tracing, shadows and timing

Hair is out of the acceleration structure unless its `CHair` has `kHairRayTraced`, and out of the shadow cascades with `kHairNoShadow`. `HairScene`
answers `inRayTracing(drawEntity)` and `castsShadow(drawEntity)`; the scene walk (`Runtime/src/GameRender.cpp`) passes them to Voxi as the sticky submit
flags `setSubmitNoRay` and `setSubmitNoShadow` around the hair draw. A compute-written mesh is already out of voxelisation and GI (Voxi skips
`meshVertexBuffer(mesh) != 0` there), so hair adds no indirect light. A ray hit on ray-traced hair is shaded with the standard model: `averAuthoredFrom`
zeroes the metallic slot (where hair keeps its colour variation) for a hair-flagged material. `prePass` opens a `Hair` GPU timer region for
`--gpu-timing`. Details and the reasons: `docs/HAIR.md` 4g.

## Vulkan

The module is backend neutral. The things the Vulkan backend must get right (root CBVs at b3 and b4, tightly packed structured buffers through
`-fvk-use-dx-layout`, the buffer states) are listed in `docs/HAIR.md` 4h, and `HairSelfTest vulkan` is the headed gate.
