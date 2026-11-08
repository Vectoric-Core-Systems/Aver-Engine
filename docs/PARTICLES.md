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
| `SHAPE ring <r> [<h> [<inner>]] [axis <x y z>] [shell]` | an annulus: outer radius, full height along the axis (centred), inner radius, the axis it turns about (default +Z); `shell` = the outer rim only |
| `SIZECURVE`, `COLORCURVE` | 2 to 4 stops, override `SIZE` and `COLOR start/end` |
| `RENDER sprite`, `ribbon trail <points> <segmentCm> fade <f>`, `ribbon linked <capacity>`, `mesh <path> velocity\|spin\|fixed` | what a particle draws as |
| `SPRITE camera`, `velocity stretch <k>`, `axis <x y z>` | sprite alignment |
| `SUBUV <cols> <rows> <frames> life\|fps <n> [blend]` | flipbook |
| `LIGHTING unlit\|ambient\|lit`, `SORT auto\|none\|depth`, `FOG on\|off` | shading |
| `BOUNDS <cm>`, `CULL <cm>`, `SPAWNAT on\|off` | culling radius, cull distance, whether `spawnAt` uses this emitter |
| `MODULE <key> [off] <param>=<number\|@Name[.x]> ...` | one entry of the module stack, authored order |
| `ONEVENT death\|collide SPAWN <emitter> <count> [inherit <k>]` | spawn into another emitter of the same system, consumed one tick later (cycles are refused) |
| `TRIGGER death\|collide\|spawn\|lifetime <fraction>\|timer <seconds> SPAWN <emitter> <count> [inherit <k>] [color] [size] [age] [user <v>]` | the same rule with more triggers and a payload (below). A plain death or collide rule is still written as `ONEVENT`; an older reader skips `TRIGGER` as an unknown record |

**Event triggers and payload.** Besides death and collide a rule can fire at a particle's **birth** (`spawn`, reported by the next tick), when it passes a **life fraction** (`lifetime 0.5`, once, strictly between 0 and 1) or **every N seconds of its age while it is alive** (`timer 0.5`, 0.01 to 3600 s; several marks inside one step each fire, up to four). The child spawns at the parent's position. Payload, per rule: `inherit k` is the fraction of the parent's velocity; `color` gives the child the parent's colour at the event (its colour curve times its tint) as its tint; `size` scales the child by the parent's size at the event relative to its size at birth (times the parent's own scale); `age` starts the child as far through its life as the parent was through its own; `user v` multiplies the child's speed by `v` (0 is off). **Both tiers.** On the GPU each emitter's events buffer holds one list per channel: 0 death, 1 collide, then one per trigger rule in rule order (`gpuEventChannel`). A record carries position, id, velocity and the payload (the particle's colour at the event, its size relative to its birth size, its life fraction), computed from the emitter's colour and size curves in the constant block. Births are raised by the spawn kernel, life-fraction and timer marks by the update kernel (a timer faster than the step fires up to four times, as on the CPU); the child's spawn dispatch reads its parent's list one frame later, so the latency is one tick on both tiers. Each channel holds 2048 records a frame; the rest are dropped, and a full pool drops the spawns. **What stays on the CPU:** an emitter with more than four firing trigger rules, or a system in which a child is fed by more than one rule (a GPU child reads one channel), runs the whole system on the CPU tier (event-linked emitters share a tier), even for `SIM gpu`; `ParticleSystem::wantGpu`, `buildInstance` and `systemEventsNeedCpu` enforce it and the editor's Tier column reads `CPU (events)`. A rule that never fires (a life fraction outside (0,1), a period under a millisecond) takes no channel on either tier. Event children do not reproduce the CPU's random draws bit for bit, as before. The `payload` colour and size are applied to children whose `spawn.colortint` / size modules do not replace them (`spawn.colortint` replaces the tint).

Parse errors carry `line N:`. The format spec is `docs/formats/FORMAT_SPECS.md` section 11a.

## The module stack

21 modules, the same on both tiers (`ParticleModules.hpp`, `EmitterSim.cpp`, `particles_common.hlsli`). Order per tick: age, die, gravity, damping, velocity modules in authored order, move, position modules in authored order, rotate, trail, events.

`spawn.sizescale`, `spawn.colortint`, `spawn.rotation`, `spawn.inheritvelocity`, `update.force`, `update.drag`, `update.attractor`, `update.vortex`, `update.curlnoise` (octaves 1 to 4), `update.speedlimit`, `update.collideplane`, `update.collidesphere`, `update.killvolume`. Every parameter is a literal or a user-parameter reference. Collision planes and spheres are in world space.

Wave 6 appended eight (ids 13 to 20, nothing before them moved). The `min`/`max` pairs are drawn once per particle from a fixed random stream, so a particle keeps its own value for its whole life:

| Key | Stage | Parameters | What it does |
|---|---|---|---|
| `update.linearaccel` | velocity | `min`, `max` | accelerates along the particle's own velocity (cm/s^2) |
| `update.radialaccel` | velocity | `min`, `max` | accelerates away from the emitter's origin (negative pulls in) |
| `update.tangentialaccel` | velocity | `min`, `max`, `ax ay az` | accelerates along `cross(direction from the origin, axis)`; nothing on the axis |
| `update.damping` | velocity | `min`, `max` | a constant deceleration (cm/s^2) that stops at zero speed and never reverses; negative values are clamped to 0 |
| `update.orbit` | position | `min`, `max` (revolutions/s), `ax ay az` | turns the position about an axis through the emitter's origin; the velocity is not turned |
| `spawn.huevariation` | spawn | `min`, `max` (turns) | a per-particle hue turn, kept in `spawnInfo` bits 17 to 31 and applied to the drawn colour by the draw shader (a rotation about the grey axis; grey stays grey) |
| `spawn.animstart` | spawn | `min`, `max`, `frames` | the sprite sheet's first frame, drawn from `[min, max]` of the sheet (the default emitter behaviour, a random frame over the whole sheet, applies only without this module) |
| `update.collidebox` | position | `cx cy cz`, `hx hy hz`, `restitution`, `friction` | an axis-aligned solid box in world space: a particle inside is pushed out through the nearest face and reflected like the plane and sphere; the collide event fires once |

Not built: height-field, signed-distance-field and vector-field modules. They need a texture or buffer on the simulation's compute layout (9 UAVs and one constant buffer today); that layout is shared by every kernel, so it is a separate, GPU-tested change.

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

**Track C additions** (docs/plans/EDITOR_UE_PARITY.md):

* **Statistics** header: alive against the cap per emitter, a CPU/GPU badge (the tier the emitter takes in a game, by the `SIM` request or the 2048 rule; the preview itself is always CPU) and an estimate of the pooled GPU buffers (80 bytes a particle, 88 when it sorts, plus 16 per trail point).
* **Playback**: play/pause, frame step (1/60 s), restart, speed 0.05x to 4x, loop with a length, a time scrubber and a time/frame readout. Seeking resimulates from 0 in fixed 1/60 s steps with the fixed per-emitter seed, so a time always shows the same particles.
* **Curves**: the size and colour curves have a graph (wheel zoom, middle/right drag pan, drag a key, double-click to add, right-click or Delete to remove; the colour graph plots R G B A) and the colour curve a gradient bar with draggable stops (double-click a stop for a colour picker). The runtime curve is piecewise-linear with 2 to 4 stops, so there are no tangents.
* **Isolate**: the `iso` button on a module runs the emitter with that module alone; preview only, the saved enable flags do not change.
* **Preview**: Ground (floor grid), Bounds (each emitter's `BOUNDS` sphere) and a moving test emitter (circle or back-and-forth line) for trails and `spawn.inheritvelocity`.
* **Templates**: Content Browser > Add > New Particle Effect from Template (smoke, sparks, rain, snow, embers, dust, mist, trail, debris, flipbook burst; all from the engine's own modules and built-ins), and a **Presets** button next to Add module (turbulence, wind, swirl, ground bounce and so on). The data is `sandbox/src/ParticleTemplates.hpp`.
* **Live parameters**: sliders for the user parameters that drive the preview only (no undo entry, no dirty mark); Keep writes one into the asset.
* **Scalability**: per-tier (Low, Medium, High, Epic) spawn-rate and particle-cap multipliers and an off switch, as an effect default with per-emitter overrides (`SCALEALL` / `SCALE`, see FORMAT_SPECS 11a.1). "Preview as" plays any tier. In a game the tier is the project key `RENDER.PARTICLESCALE` (0 Low .. 3 Epic; when absent the tier follows the Overall Quality rung the project's render settings read as, Low..Epic, and stays Epic, everything as written, for a Custom mix; picking an Overall preset in the editor or `voxi.scalability` moves it too, unless the project pins the key), applied by `ParticleEffectLibrary::setScalabilityTier`; effects without scalability data are untouched.

Emitters are placed in levels with `COMPONENT ParticleEmitter effect=Particles/Embers.ocparticle sim=auto playing=true seed=0`.

## Test content

`test-content/ParticleDemo` is a floor with six placed emitters: `Embers` (GPU, curl noise, curves), `FireSmoke` (three emitters and a death event), `SparkTrails` (trail ribbons, bounce), `BeamRibbon` (linked ribbon), `Debris` (mesh particles, lit) and `Rain100k` (100,000 live particles, user-parameter wind). `Snowfall` and `Mist` are the v1 files. Everything is procedural; there is no third-party asset.

## Tests

`ParticleModulesTest`, `EmitterSimTest`, `ParticleSpawnAtTest`, `ParticleSortNetworkTest`, `ParticleLightingTest`, `ParticleSystemTest` (tiers, ops, events, budgets), `ParticleGpuParityTest` (the GPU kernels against `EmitterSim`, on every available backend, within stated tolerances because atomic ordering differs), `IndirectRhiTest` (indirect draw and dispatch, and a vertex shader reading a table-0 `StructuredBuffer`), `OcParticleTest` (v1 byte identity, v2 round trip, every error), `FrameworkParticlesAbiTest`, `ParticleStackModelTest`, `ParticleEditorLoadSaveTest`, `ParticlesNodeTests` (C#). Wave 6 added `ParticleRegressionTest` (the shipped `.ocparticle` files simulate on the CPU executor as they did before the new shape and modules: sums against values captured on the previous build), `ParticleSpirvCompileTest` (every kernel and draw shader compiles to SPIR-V and DXIL with no device) and the wave 6 cases in `ParticleModulesTest` (each new module, the hue rotation, the shared HLSL's ids), `EmitterSimTest` (ring, orbit, box) and `OcParticleTest` (ring and module round trip).

## Limits, said plainly

No simulation stages (neighbour grids, particle-to-particle reads), no local-space simulation, no depth-buffer collision or soft particles, no ray-traced particle shadows, no froxel light shafts on particles, no per-particle material graphs, no scripted modules, no GPU sort above 65,536 particles per emitter, a mesh particle is at most 2,048 triangles, a trail ribbon has at most 32 points and a linked ribbon at most 4,096 links.

## Patent and clean-room notes

Bitonic sort (Batcher, 1968) and atomic free-list compaction only. Decoupled look-back scans and Onesweep radix sort are not used. Noise is lattice gradient noise written from scratch, not simplex.
