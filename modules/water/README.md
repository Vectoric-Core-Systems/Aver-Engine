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

It is also load-bearing beyond rendering: buoyancy wants the wave height at a point, and so would a
boat, a buoy, or a shoreline foam mask. A wave formula that only existed inside a shader could serve
none of them.

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
