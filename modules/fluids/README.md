# Aver.Fluids  (`modules/fluids`)

- **Language:** C++
- **Depends on:** Core, RHI
- **Option:** `AVER_MODULE_FLUIDS` (ON)

Water in three parts. This module implements two; the solver-driven buoyancy is in `modules/physics`. The module was `modules/water` until `b887a78` ("Water and render.fluid become one module called Fluids") merged it with `render.fluid`. The tables and sections below describe only the water half (`GerstnerWave`/`WaterRenderer`/`Underwater`); the solver-driven half that motivated the rename also lives here and is not documented below: `FluidVolume.hpp`/`.cpp` and `FluidScene.hpp`/`.cpp` ("FluidVolume builds a closed shell of arbitrary compliance and pressure and hands it to a solver. That is a fluid, not specifically water", per that commit's message).

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

Nothing else calls it yet. `gerstnerHeightCm` and `gerstnerDisplaceCm` have exactly one real call
site each, in `GerstnerWaveTest.cpp` — every other hit in the repo is a comment. Buoyancy in
particular does not read the wave: `SandboxApp.cpp` hands `aver_phys_set_water_plane` a FLAT
constant height (`waterRenderer_.waterLevelCm()`), and `Buoyancy.cpp` evaluates bodies against that
plane, never against a per-point sample of the surface. A boat, a buoy, and a shoreline foam mask
that read the wave height are all still hypothetical. The separation is worth keeping regardless —
a formula wired into device state could not be unit-tested off-GPU at all — but the case for it is
"this stays testable and reusable if a caller ever wants it", not "callers already depend on it".

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
