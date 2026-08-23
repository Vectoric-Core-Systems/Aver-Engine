# Aver.Water  (`modules/water`)

- **Language:** C++
- **Depends on:** Core, RHI
- **Option:** `AVER_MODULE_WATER` (ON)

Water in three parts, of which this module is two.

| Part | Where | Why there |
| --- | --- | --- |
| **Surface** — Gerstner waves, drawn in the transparent pass | `include/aver/water/GerstnerWave.hpp`, `WaterRenderer.hpp` | An `rhi::IRenderFeature`, the same shape `ParticleRenderer` uses: depth-tested, depth-write off, blended over the opaque scene. |
| **Underwater** — the fog override when the camera is below the surface | `include/aver/water/Underwater.hpp` | A pure function over `rhi::SkyAtmosphere`. It touches no device, so it is checkable with no GPU. |
| **Buoyancy** — things float | **NOT HERE.** `modules/physics/src/Buoyancy.*` | It is Jolt's `ApplyBuoyancyImpulse`, and this module must never learn what Jolt is. `modules/physics` keeps `JPH::` private to its own target, which is what makes the physics backend swappable at all. The two halves meet in the composition root and nowhere else. |

## The wave math is separable, on purpose

`GerstnerWave.hpp` takes no `rhi::` and no device: a world position and a time go in, a displaced
position and a normal come out. That is what lets `tests/water/src/GerstnerWaveTest.cpp` check the
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

Recorded here rather than left to be discovered:

- **No `.ocworld` water record.** The water level and wave set are supplied by the host at
  registration, not authored per level. Making them level data is a separate slice and follows the
  `LANDSCAPE` record precedent exactly.
- **No soft shoreline.** The surface is depth-*tested* but does not sample scene depth, so it meets
  sloped terrain in a hard line rather than a fade. Scene depth is D3D12-only today
  (`IDevice::sceneDepthTexture()` returns 0 on every other backend), so the blend is deferred rather
  than half-built against a backend gap.
- **Vulkan is untested, not broken.** The code is written against the generic `rhi::` vocabulary and
  should compile, but the post chain it would composite through is refused on Vulkan today, so no
  claim is made about the final image there.
- **The fog defaults are placeholders.** They are unvalidated guesses at centimetre scale, and the
  existing fog was measured only at kilometre scale — a genuinely different regime.
- **Invisible to every other render feature.** `WaterRenderer::transparentPass` issues its own
  `ctx.drawIndexed` directly and never goes through `IDevice::drawMesh`, so the surface never enters
  the shadow cascades, the ray-tracing TLAS, or GI voxelisation. Its lighting is entirely
  self-contained for the same reason — it was never wired into the scene's light in the first place.
- **No simulation.** There is no wave field being stepped or evolved anywhere. The surface is
  analytic: a static 129x129 grid whose vertices never change, recentred under the camera each frame
  by moving a world-space origin uniform, with the Gerstner displacement applied entirely in the
  vertex shader.
