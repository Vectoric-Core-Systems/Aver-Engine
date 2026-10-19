# Particles

One particle system: `modules/particles` (`Aver.Particles`), the `.ocparticle` format (`modules/formats.particles`), `scene::CParticleEmitter`, the editor tab (`sandbox/src/ParticleEditor*.cpp`) and the scripting surface. Wave 10 made it a two-tier system: small emitters simulate on the CPU, large ones on the GPU, from the same data and the same module table. The frozen design is `docs/plans/wave10/GPU_PARTICLES.md`; this page is what was built and how to use it.

## The data

A **system** is a list of up to 8 **emitters** plus up to 16 named **user parameters**. `particles::ParticleSystemDef` is the in-memory form, `ParticleEffect` is one emitter (the name stayed so v1 code keeps compiling). A file with one emitter and no parameters is the same as before: a **version 1** `.ocparticle` loads unchanged and is written back as version 1 until something v2-only is authored (a module, a parameter, a second emitter, a render kind, a lighting mode and so on), at which point the writer emits `OCPARTICLE 2`. A reader built before wave 10 refuses a v2 file by version, so nothing mis-parses.

The registered effect id is `fnv1a64` of the content-relative path (`particles::effectId(path)`, `Assets.ObjectIdOf(path)` in C#). `ParticleEffectLibrary::setSystem(id, def)` registers a whole system; `find(id)` still answers emitter 0 for old callers.

### Grammar additions (version 2)

| Record | Meaning |
|---|---|
| `PARAM <name> float <v>` / `vec3 <x y z>` / `color <r g b a>` | a user parameter, settable from a script and referenced by a module as `@Name` or `@Name.x` |
| `EMITTER <name>` | starts the next emitter block; records before the first one belong to emitter 0 |
| `SIM auto\|cpu\|gpu` | tier request (see Selection) |
| `SHAPE sphere <r>`, `cylinder <r> <h>`, `disc <r>`, optional `shell` | short forms; the v1 `SHAPE kind x y z` spelling still works |
| `SIZECURVE`, `COLORCURVE` | 2 to 4 stops, override `SIZE` and `COLOR start/end` |
| `RENDER sprite`, `ribbon trail <points> <segmentCm> fade <f>`, `ribbon linked <capacity>`, `mesh <path> velocity\|spin\|fixed` | what a particle draws as |
| `SPRITE camera`, `velocity stretch <k>`, `axis <x y z>` | sprite alignment |
| `SUBUV <cols> <rows> <frames> life\|fps <n> [blend]` | flipbook |
| `LIGHTING unlit\|ambient\|lit`, `SORT auto\|none\|depth`, `FOG on\|off` | shading |
| `BOUNDS <cm>`, `CULL <cm>`, `SPAWNAT on\|off` | culling radius, cull distance, whether `spawnAt` uses this emitter |
| `MODULE <key> [off] <param>=<number\|@Name[.x]> ...` | one entry of the module stack, authored order |
| `ONEVENT death\|collide SPAWN <emitter> <count> [inherit <k>]` | spawn into another emitter of the same system, consumed one tick later (cycles are refused) |

Parse errors carry `line N:`. The format spec is `docs/formats/FORMAT_SPECS.md` section 11a.

## The module stack

13 modules, the same on both tiers (`ParticleModules.hpp`, `EmitterSim.cpp`, `particles_common.hlsli`). Order per tick: age, die, gravity, damping, velocity modules in authored order, move, position modules in authored order, rotate, trail, events.

`spawn.sizescale`, `spawn.colortint`, `spawn.rotation`, `spawn.inheritvelocity`, `update.force`, `update.drag`, `update.attractor`, `update.vortex`, `update.curlnoise` (octaves 1 to 4), `update.speedlimit`, `update.collideplane`, `update.collidesphere`, `update.killvolume`. Every parameter is a literal or a user-parameter reference. Collision planes and spheres are in world space.

Randomness is counter-based (`hash32(seed, particleId, stream)`), so a spawn does not depend on thread order. Noise is a from-scratch lattice gradient noise; curl noise is its analytic curl.

## Two tiers

* **CPU tier**: `EmitterSim` in `ParticleSystem::tick`. The reference implementation, used for small emitters, for tests and when the device cannot run the GPU one.
* **GPU tier**: compute kernels (`particles_sim.hlsl`) over pooled buffers: spawn, update, dead-list free/alloc by atomics, ping-pong alive lists, indirect draw arguments written on the GPU, optional bitonic sort (`particles_sort.hlsl`). All of it runs in the renderer's `prePass`.

**Selection.** `SIM auto` (the default) puts an emitter on the GPU when `maxParticles >= 2048` and the device reports `caps.computeShaders && caps.indirect`. `SIM cpu` / `SIM gpu` force it (`gpu` falls back with one warning if the device cannot). The choice is made when an emitter instance starts and never changes. `RENDER.PARTICLEGPU` in the project (0 off, 1 auto, 2 on), the **Particles on GPU** combo in Settings > Render > Performance and the `--particles-cpu` / `--particles-gpu` flags restrict it for the whole run. Placed emitters can also be flagged (`kParticleEmitterForceCpu`, `kParticleEmitterForceGpu`).

**Budgets** (defaults, settable with `Particles.SetBudget`): 2,000,000 GPU particles (allocated capacity), 200,000 CPU particles, 2,048 spawn-at requests per frame. An emitter that does not fit the GPU budget runs on the CPU.

**Statistics.** `Particles.TryGetStats` and `AN_GetParticleStats` report emitter and particle counts. GPU counts come from a 3-deep readback ring, so they are up to three frames old and never stall the frame; CPU counts are exact.

## Drawing

One vertex shader reads `StructuredBuffer<Particle>` plus an order list by `SV_InstanceID`; the CPU tier uploads its array into the same layout. Kinds:

* **Sprite**: camera, velocity (stretched) or fixed-axis aligned, optional flipbook.
* **Ribbon trail**: each particle drags a ring of its recent positions; segment length and fade are authored.
* **Ribbon linked**: consecutive particles are joined into one strip, ring-allocated (`capacity` links); the strip is as long as the emission rate times the lifetime, up to the capacity.
* **Mesh**: a small mesh per particle (at most 2,048 triangles). `Meshes/cube.ocmesh` and `Meshes/sphere.ocmesh` work without a file.

**Sorting.** `SORT auto` sorts alpha-blended and premultiplied emitters back to front (CPU `std::sort`; GPU bitonic network up to 65,536 particles per emitter, above which it stops sorting with one warning) and never sorts additive ones. Draw order across emitters is by distance of the emitter bounds centre, so a smoke in front of a fire draws in front of it.

**Lighting.** `unlit`; `ambient` (the existing GI term, the default for `GI on`); `lit` (sun, sky and up to four nearby scene lights, half-Lambert wrapped because a billboard has no stable normal). **Fog:** every kind takes the analytic height fog and aerial perspective term unless `FOG off`. Light shafts and ray-traced shadows on particles are not applied.

## Spawning an effect at a point (the contract other features call)

```cpp
aver::spawnEffectAt(effectId, position, direction, count);          // Aver.Core, no module dependency
aver::particles::spawnAt(aver::particles::SpawnAtParams{...});     // when you link Aver.Particles
```

```csharp
Particles.SpawnAt("Particles/Debris.ocparticle", pos, dir, count);
Particles.SpawnAt(path, pos, dir, count, speedScale, sizeScale, lifetimeScale, tint, inheritedVelocity);
```

C ABI: `aver_fw_particles_spawn_at` and `_ex` in `framework_particles_abi.h`. Node: `AN_SpawnParticlesAt`.

Fire and forget: no entity is made. Every emitter of the system with `SPAWNAT on` receives `count` particles at `position`, the authored direction rotated so +Z maps onto `direction` (zero direction: no rotation). Requests are applied at the start of the next tick. Particles live in shared "event instances" keyed by effect id and emitter, retired after 5 idle seconds, so a thousand skid puffs are one simulation. Not thread-safe. Returns the particles accepted; 0 for an unknown effect (logged once) or with no particles installed.

## Scripting

* **C#** (`Aver.Framework`): `Particles.SpawnAt`, `SetBudget`, `SetGpuMode`, `TryGetStats`; on `Entity`: `SetParticleFloat/Vector/Color(name, ...)`, `BurstEmitter`, `StopEmitter`, `SetEmitterModifiers`, `TryGetEmitterState`. Parameter sets are queued and applied at the top of the next tick.
* **Bus event**: `Particles.EffectFinished` (entity) when an emitter flagged `kParticleEmitterAutoDestroy` has stopped and drained.
* **Aver Node family `Particles` v2** (9 nodes, keys `an_spawnparticlesat`, `an_setparticlefloat`, `an_setparticlevector`, `an_setparticlecolor`, `an_burstemitter`, `an_stopemitter`, `an_setemittermodifiers`, `an_getemitterstate`, `an_getparticlestats`). The four older emitter nodes in `RenderProps` keep their keys.

## The editor

Open a `.ocparticle`. The tab has an emitter list (add, duplicate, remove, reorder, rename, solo, mute), a module stack (add, remove, reorder, enable, bind a parameter to a user parameter), render, lighting, sorting, events and culling sections, the user parameters, and **Compute bounds** (runs the emitter for 3 s of simulated time and writes `BOUNDS`). The preview runs `EmitterSim` into a private CPU-tier renderer drawn into the shared preview surface; if the surface does not expose its transparent hook it falls back to coloured cubes. Edits are undoable; a drag is one undo step. The edit logic is `ParticleStackModel.hpp` (ImGui-free, tested by `ParticleStackModelTest`).

Emitters are placed in levels with `COMPONENT ParticleEmitter effect=Particles/Embers.ocparticle sim=auto playing=true seed=0`.

## Test content

`test-content/ParticleDemo` is a floor with six placed emitters: `Embers` (GPU, curl noise, curves), `FireSmoke` (three emitters and a death event), `SparkTrails` (trail ribbons, bounce), `BeamRibbon` (linked ribbon), `Debris` (mesh particles, lit) and `Rain100k` (100,000 live particles, user-parameter wind). `Snowfall` and `Mist` are the v1 files. Everything is procedural; there is no third-party asset.

## Tests

`ParticleModulesTest`, `EmitterSimTest`, `ParticleSpawnAtTest`, `ParticleSortNetworkTest`, `ParticleLightingTest`, `ParticleSystemTest` (tiers, ops, events, budgets), `ParticleGpuParityTest` (the GPU kernels against `EmitterSim`, on every available backend, within stated tolerances because atomic ordering differs), `IndirectRhiTest` (indirect draw and dispatch, and a vertex shader reading a table-0 `StructuredBuffer`), `OcParticleTest` (v1 byte identity, v2 round trip, every error), `FrameworkParticlesAbiTest`, `ParticleStackModelTest`, `ParticleEditorLoadSaveTest`, `ParticlesNodeTests` (C#).

## Limits, said plainly

No simulation stages (neighbour grids, particle-to-particle reads), no local-space simulation, no depth-buffer collision or soft particles, no ray-traced particle shadows, no froxel light shafts on particles, no per-particle material graphs, no scripted modules, no GPU sort above 65,536 particles per emitter, a mesh particle is at most 2,048 triangles, a trail ribbon has at most 32 points and a linked ribbon at most 4,096 links.

## Patent and clean-room notes

Bitonic sort (Batcher, 1968) and atomic free-list compaction only. Decoupled look-back scans and Onesweep radix sort are not used. Noise is lattice gradient noise written from scratch, not simplex.
