# Aver.Fluids  (`modules/fluids`)

- **Language:** C++
- **Depends on:** Core, RHI
- **Option:** `AVER_MODULE_FLUIDS` (ON)

Water in three parts. This module implements two; the solver-driven buoyancy is in `modules/physics`. The module was `modules/water` until `b887a78` ("Water and render.fluid become one module called Fluids") merged it with `render.fluid`. The tables and sections below describe only the water half (`GerstnerWave`/`WaterRenderer`/`Underwater`); the solver-driven half that motivated the rename also lives here and is not documented below: `FluidVolume.hpp`/`.cpp` (the authored description), `PbfFluid` (the CPU position-based particle solver), `FluidSurface` (the meshed surface) and `FluidScene` (spawn, step, render) -- see `docs/FLUIDS.md`.

| Part | Where | Why there |
| --- | --- | --- |
| **Surface** — Gerstner waves, drawn in the transparent pass | `include/aver/fluids/GerstnerWave.hpp`, `WaterRenderer.hpp` | An `rhi::IRenderFeature`, the same shape `ParticleRenderer` uses: depth-tested, depth-write off, blended over the opaque scene. |
| **Underwater** — the fog override when the camera is below the surface | `include/aver/fluids/Underwater.hpp` | A pure function over `rhi::SkyAtmosphere`. It touches no device, so it is checkable with no GPU. |
| **Buoyancy** — things float | **NOT HERE.** `modules/physics/src/Buoyancy.*` | It is Jolt's `ApplyBuoyancyImpulse`, and this module must never learn what Jolt is. `modules/physics` keeps `JPH::` private to its own target, which is what makes the physics backend swappable at all. The two halves meet in the composition root and nowhere else. |

## The wave math is separable, on purpose

`GerstnerWave.hpp` takes no `rhi::` and no device: a world position and a time go in, a displaced
position and a normal come out. That is what lets `tests/fluids/src/GerstnerWaveTest.cpp` check the
surface against numbers it computes for itself, on a machine with no GPU — the same argument
`Aver.Sound` and `Aver.Synapse` make for being pure.

**Buoyancy now reads it** (wave 3). `gerstnerSurfaceCm(x, worldY, t)` solves the surface at a WORLD point (the wave displaces horizontally, so the rest point is found by fixed-point iteration) and returns the height and unit normal there. `game::GameWater` installs a physics water sampler (`aver_phys_set_water_sampler`, `physics_water_abi.h`) and the framework's `aver_fw_water_sample` over `sampleSurface`, which takes the highest of the ocean (this math, at the live surface's own wave clock `elapsedSeconds()`, so CPU and GPU agree), the lakes and the rivers. `GerstnerWaveTest` pins the solve. The shader still has its own hand-ported copy of the formula; the CPU and GPU agree by test and by sharing the clock, not by sharing source. The compute pass fades waves near the camera-following grid's rim and the CPU does not, an accepted difference far from the player.

## Rivers and lakes (wave 3)

`WaterBody.hpp` is the CPU half of spline water bodies: a `Spline` (centripetal Catmull-Rom in XY, monotone PCHIP for height, width and flow, so none overshoots), `buildRiverMesh` (three vertices per row, flow in `uv0` for the material graph's `VertexFlow` / `FlowNormal` nodes), `buildLakeMesh` (ear-clipped outline subdivided until every edge is short, watertight) and `WaterBodySet`, the point query (surface height, current, depth, kind; the highest surface wins). It takes no device: the meshes are plain `rhi::MeshVertex` arrays `game::GameWater` uploads and draws through the ordinary material path, which is why rays see them. Records: `RIVER` / `RIVERPT` / `LAKE` / `LAKEPT` (`docs/formats/FORMAT_SPECS.md` section 11.x). Tests: `WaterBodyTest`, `WaterBodyRecordTest`. Not done: vertex waves on rivers and lakes (their ripple is the material's), tributaries, swimming locomotion. See `docs/LANDSCAPE_LAYERS_WATER.md`.

The real, current cost of the split is that the wave math exists in two copies. `WaterShaders.hpp`
does not call `gerstnerDisplaceCm` — it hand-ports the same formula into HLSL, by its own admission
("PORTED BY HAND ... there is NO shared-source mechanism between C++ and HLSL anywhere in this
engine"). Changing the wave formula, the steepness clamp, or the normal derivation means changing
both copies by hand and keeping them in step; nothing in the build checks that they agree.

## What this slice does not do

- **Levels can author water.** `WATER` (name, level height, `bounds`/`infinite`, an optional `simulate` flag) and `WAVE` (cross-referenced to a `WATER` by name, empty name meaning the first declared one) are real `.ocworld` records (`modules/formats/src/OcWorld.cpp`'s `WATER` parse, `OcWorld.hpp`'s `OcWaterPlacement`/`OcGerstnerWave`; `9afd98b`), modelled on `OcPcgVolume` rather than the `LANDSCAPE` precedent, since a `WATER` record has no external asset and nothing about it needs positioning the way a landscape tile does. (This bullet once said the water level and wave set were supplied only by the host at registration; that is no longer true.)

- **No soft shoreline.** The surface is depth-*tested* but does not sample scene depth, so it meets sloped terrain in a hard line rather than a fade. Scene depth is now available on all backends (`VulkanDevice::sceneDepthTexture()` mirrors the D3D12 implementation structurally, both backends support it; D3D11 still returns `IDevice`'s own default of 0, no override in `modules/rhi.d3d11`). The blend is still deferred, but the reason has narrowed to "not built yet".

- **Vulkan: the post chain is not a blocker, but water there is unchecked.** The post chain the surface composites through was once believed refused on Vulkan; `VulkanDevice::runPostChain` is called unconditionally every frame (present since the backend's first checkpoint commit `895ba68`), and `modules/rhi.vulkan/README.md`'s "What is left" lists the post chain's bindings as verified. This module has no Vulkan-specific path or gate (`WaterRenderer.cpp` declines only when the backend exposes no resource factory at all, a generic check, `"[Water] init declined: backend exposes no resource factory"`), so the surface should composite the same way on both backends; that has not been independently checked.

- **The fog defaults are placeholders.** They are unvalidated guesses at centimetre scale, and the existing fog was measured only at kilometre scale — a genuinely different regime.

- **Invisible to every other render feature.** `WaterRenderer::transparentPass` issues its own `ctx.drawIndexed` directly and never goes through `IDevice::drawMesh`, so the surface never enters the shadow cascades, the ray-tracing TLAS, or GI voxelisation. Its lighting is entirely self-contained for the same reason — it was never wired into the scene's light in the first place.

- **No simulation.** There is no wave field being stepped or evolved anywhere. The surface is analytic: a static 129x129 grid whose vertices never change, recentred under the camera each frame by moving a world-space origin uniform, with the Gerstner displacement applied entirely in the vertex shader.
