# Aver.Particles

Data-driven particles on two tiers: fire, smoke, steam, dust, rain, snow, sparks, embers, leaves, bubbles, spray, magic, debris, fog wisps, ribbons and mesh debris -- general-purpose engine machinery in the same way the mesh renderer is, not a weapon-effects system. Nothing in this module names what an effect IS; every effect is DATA (`aver::particles::ParticleSystemDef`), never a code path. Usage, grammar and limits: `docs/PARTICLES.md`; the design: `docs/plans/wave10/GPU_PARTICLES.md`.

## Pieces

* `scene::CParticleEmitter` (`modules/scene/.../Components.hpp`) -- the ECS-visible half: an opaque effect id plus this emitter's own playback clock and flags (`kParticleEmitterStopped`, `AutoDestroy`, `ForceCpu`, `ForceGpu`). A scene built-in, so the inspector and the C# surface come for free.
* `ParticleTypes.hpp` -- `ParticleEffect` (one emitter definition), `ParticleSystemDef` (emitters + user parameters), `Module`, `Curve4`, `EventSpawn`, and the 64-byte `Particle` that is identical in C++ and `particles_common.hlsli`.
* `ParticleModules.hpp` / `.cpp` -- the closed table of 13 modules (key, stage, parameter names and defaults) and the CPU evaluation of each.
* `ParticleRandom.hpp` -- counter-based RNG (`hash32`, `rand01`) and lattice gradient / curl noise, shared by both tiers.
* `EmitterSim` -- the CPU executor: spawn, step, compaction in stable order, trail points, events. RHI-free; the editor preview owns its own.
* `ParticleEffectLibrary` -- effect id (`fnv1a64` of the content-relative path) to system: `set`, `setSystem`, `find`, `findSystem`, `clear`, `revision`. This module never opens a file.
* `ParticleSystem` -- ticks every `CParticleEmitter`, owns the per-instance sims and the event instances `spawnAt` uses, chooses the tier, and emits GPU jobs. No RHI: the GPU tier sits behind `IParticleGpuTier`.
* `ParticleSpawn.hpp` -- `spawnAt`, `installEffectSpawner` (the `aver::spawnEffectAt` seam in `Aver.Core`).
* `ParticleProvider.hpp` -- the one function the framework C ABI (`framework_particles_abi.h`) relays to.
* `ParticleGpu` / `ParticleGpuPool` -- pooled buffers per capacity class, the kernels' recording, indirect arguments, sort, stats readback. `ParticleGpuTypes.hpp` mirrors the constant-buffer structs with `static_assert`s.
* `ParticleRenderer` -- an `rhi::IRenderFeature`. `prePass` records the GPU jobs, `transparentPass` draws sprites, ribbons and mesh particles through one vertex shader. Resolvers for textures and meshes are installed by the host; the GI seam is below.
* `ParticleLighting` -- `selectParticleLights`, the pure choice of the four lights an emitter uses.
* `ParticleSortNetwork.hpp` -- the bitonic network's index math, run by the C++ test and read by the shader.
* `shaders/` -- `particles_common.hlsli`, `particles_sim.hlsl`, `particles_sort.hlsl`, `particles.hlsl`.

## DECIDED 3 -- `.ocparticle`, and who loads it

The reader/writer lives in `modules/formats.particles` (`Aver.Formats.Particles`), a separate target for `Aver.Formats.Material`'s exact reason: it parses into a type outside `Aver.Formats`. This module never includes that header and never opens a file. Both composition roots walk their project's content root and register each file with `particleEffects().setSystem(fnv1a64(relative path), def)`: `sandbox/src/SandboxApp.cpp`'s `loadProjectParticleEffects()` and `Runtime/src/GameContent.cpp`'s `GameContent::loadProjectParticleEffects()`. The renderer's texture and mesh resolvers (`GameContent::resolveParticleTexture`, `resolveParticleMesh`) are installed the same way.

## DECIDED 4 -- the GI seam

`ParticleRenderer::GiSeam` -- a `prepare` function pointer (pipeline-build time), a `bind` function pointer (per frame) and an opaque `user` pointer, the same shape `VoxiRenderer::setDepthProxy` uses. Installed by whoever owns both sides; this module links no Voxi library. Without a seam an `ambient` or `lit` particle draws as if the GI term were 1. `receivesGI false` (or `LIGHTING unlit`) opts an effect out -- an ember is its own light source.

## Two tiers

`SIM auto` puts an emitter on the GPU when `maxParticles >= 2048` and the device has compute and indirect draw; `cpu` and `gpu` force it. The tier is fixed for an instance's life. `RENDER.PARTICLEGPU` and `--particles-cpu` / `--particles-gpu` restrict it per run. The CPU tier is the reference: `ParticleGpuParityTest` compares the kernels against `EmitterSim`, and anything the GPU cannot do falls back to it with one warning.

## RHI additions this module relies on

`drawIndexedInstanced`, `drawIndexedIndirect`, `dispatchIndirect`, `ResourceState::IndirectArgument`, `DeviceCaps::indirect` on both backends (`IndirectRhiTest`). A device without them runs every emitter on the CPU.

## Not in this module (by design)

Simulation stages with neighbour grids, local-space simulation, depth-buffer collision, soft particles, ray-traced shadows on particles, froxel light shafts, per-particle material graphs, scripted modules, GPU sort above 65,536 particles per emitter. See `docs/PARTICLES.md`.
