# GPU particles: Niagara-class effects on the existing particle module (wave 10)

Design contract for the implementing agent. Parity target: Unreal's Niagara (GPU compute simulation, ribbons, mesh particles, a module stack, events, spawn-at-event). Nothing here is built or run. Rules in force: upgrade the existing systems (no second particle system, no second format, no second editor), no new third-party code, lean comments, D3D12 and Vulkan parity, headless ctest where possible, headed manual checks by the owner, never delete a file, never rebuild over the owner's running editor, Debug object files near the COFF section cap must not grow unchecked.

Every "exists today" claim in section 1 was read in the tree on 2026-10-19. Line numbers are last-known-good pointers; grep the symbol when one is off. **UNVERIFIED** marks anything reasoned from reading but not confirmed by reading the exact code path or running it.

---

## 0. Decisions in one screen

1. **One particle system, upgraded.** `modules/particles` (`Aver.Particles`), `.ocparticle`, `scene::CParticleEmitter` and `sandbox/src/ParticleEditor.cpp` stay the only particle code. `ParticleEffect` becomes one *emitter definition*; a file may hold several (`.ocparticle` version 2, a system of emitters). Version-1 files load unchanged and are written back as version 1 until something v2-only is authored.
2. **One simulation definition, two executors.** A fixed table of 13 data-driven modules (spawn and update stages) is evaluated by a CPU executor (`EmitterSim`) and a GPU executor (compute kernels) from the same parameters, with the same counter-based RNG, over one 64-byte `Particle` struct that is identical in C++ and HLSL. The CPU path is the reference for tests, the fallback when compute or indirect draw is missing, and the home of small emitters.
3. **One draw path.** Sprites, ribbons and mesh particles are drawn by one vertex shader that reads `StructuredBuffer<Particle>` and an index list by `SV_InstanceID`. The CPU path uploads its array to the same layout. The CPU-baked-vertex path in `ParticleRenderer::transparentPass` is removed.
4. **The RHI lacks indirect draw, indirect dispatch and instance counts** (verified, section 1.2). They are added to both backends: `drawIndexedInstanced`, `drawIndexedIndirect`, `dispatchIndirect`, `ResourceState::IndirectArgument`, `DeviceCaps::indirect`.
5. **GPU tier:** pooled buffers per emitter instance, spawn kernel, update kernel, free list ("dead list") with atomic pop and push, ping-pong alive lists (compaction by append), indirect draw arguments written on the GPU, optional bitonic sort for translucency. All compute runs in `IRenderFeature::prePass` (before the scene render scope, legal on Vulkan); the draw stays in `transparentPass`. 32-bit atomics and groupshared only: no wave intrinsics, no 64-bit atomics, one HLSL file for both backends (the `skin.hlsl` rule).
6. **Selection is automatic and safe.** `SIM auto` (default) puts an emitter on the GPU when `maxParticles >= 2048` and the device reports `caps.computeShaders && caps.indirect`; otherwise CPU. A tier is chosen when an emitter instance is created and never changes mid-life.
7. **Ribbons and mesh particles on both paths.** Ribbon modes: `trail` (a ring of recent points per particle) and `linked` (consecutive spawns joined, ring-allocated). Mesh particles instance a small `.ocmesh` per particle. Both run the same simulation as sprites; only the render kind differs.
8. **Lighting and fog.** Three modes: `unlit`, `ambient` (today's GI term, unchanged default), `lit` (sun with Voxi's cascade shadow when the GI seam is present, sky irradiance, up to 4 nearest scene lights unshadowed). **Particles are not fogged today** (section 1.1); all kinds gain analytic height fog and aerial perspective from the shared prelude. Volumetric froxel shafts are not applied (section 7, Q7).
9. **Events and spawn-at-event (cross-feature contract, section 2.10).** One API: `particles::spawnAt` (C++), `aver::spawnEffectAt` (a dependency-free seam in `Aver.Core` so `softbody.scene` and vehicle code need no new module dependency), `Particles.SpawnAt` (C#), `aver_fw_particles_spawn_at` (C ABI), `AN_SpawnParticlesAt` (node). Internal events (`ONEVENT death|collide SPAWN`) spawn from one emitter into another, consumed one tick later on both paths.
10. **Aver Node family `Particles` v2:** 9 new nodes in `GraphParticlesNodeTable.cs` (spawn at, three typed user-parameter setters, burst, stop, modifiers, emitter state, global stats). The four existing emitter nodes stay in `RenderProps` and keep their keys; this family adds no duplicate.
11. **Editor.** The same tab gains an emitter list, a module stack (add, remove, reorder, enable, bind a parameter to a user parameter), render and lighting sections, user parameters, events, and a "Compute bounds" button. The preview stops reimplementing the simulation and runs `EmitterSim`. Emitters become persistable in levels through a `COMPONENT ParticleEmitter` record (today a level cannot save one).
12. **Budgets are explicit:** 2,000,000 GPU particles, 200,000 CPU particles, 2,048 spawn-at requests per frame, defaults; project key `RENDER.PARTICLEGPU` (0 off, 1 auto, 2 on) and flags `--particles-cpu`, `--particles-gpu`.
13. **Out of v1, said plainly:** simulation stages and neighbour grids, local-space simulation, depth-buffer collision and soft particles, ray-traced particle shadows, froxel light shafts on particles, per-particle PBR material graphs, a scripted (HLSL/graph) module authoring path, GPU sort above 65,536 particles per emitter. Each is in section 7 with its reason.
14. **Patent stance (section 2.12):** bitonic sort (1968) and atomic free-list compaction only; decoupled look-back scans and Onesweep radix sort are not used; noise is a from-scratch lattice gradient noise, not simplex.
15. **Size XL, nine phases** (section 4). Phase 0 (RHI) and phase 5 (GPU) carry the risk; phases 1, 2 and 3 make the CPU path complete and deliver the cross-feature contract early.

---

## 1. What exists

### 1.1 Particles, format, editor, scripting

| Fact | Where |
|---|---|
| The module is CPU-simulated billboards; GPU simulation and a cross-emitter sort are listed as "not in this module yet" | `modules/particles/README.md:1-3`, `:53-58` |
| `ParticleEffect`: shape Point/Sphere/Box, rate, burst, `maxParticles`, lifetime range, cone direction and spread, speed range, constant gravity, linear damping, size and colour start/end, `blend`, `textureId`, `receivesGI` | `ParticleTypes.hpp:16-20` (shapes), `:31-79` |
| `textureId` is declared and never read: every effect draws a procedural soft dot | `ParticleTypes.hpp:61-67`, `shaders/particles.hlsl:40-47` |
| `Particle` is 48 bytes: position, age, velocity, lifetime, `seed`, padding. `seed` is "forward-looking only", the CPU never reads it; there is no rotation, id, per-particle size or tint | `ParticleTypes.hpp:97-111` |
| Spawn draws shape offset, cone direction, speed and lifetime from one `std::mt19937` per emitter, seeded from the entity hash. A compute thread cannot reproduce that stream | `ParticleSystem.cpp:57-60`, `:108-127`, `:32-35` |
| Update is semi-implicit Euler with constant gravity and linear damping only; nothing else can move a particle | `ParticleSystem.cpp:129-148` |
| Over budget the spawn is dropped, never an old particle evicted | `ParticleSystem.cpp:213-224` |
| The tick runs unconditionally (editor outside Play too, packaged game every frame) | `sandbox/src/SandboxUpdate.cpp:826-833`, `Runtime/src/GameApp.cpp:2204-2205` |
| Emitters are first-seen ordered; there is no sort across emitters | `ParticleSystem.hpp:42-58`, `modules/particles/README.md:56-58` |
| The renderer bakes camera-facing quads on the CPU every frame (`cameraBasis` from `invViewProjRel`), sorts each emitter back to front with `std::sort`, uploads into a ring of 3 vertex/index buffers, and issues one `drawIndexed` per emitter | `ParticleRenderer.cpp:47-56`, `:331-440` (sort `:356`, upload `:411-415`, draw `:439`) |
| It overrides only `transparentPass`, not `prePass` | `ParticleRenderer.hpp:108` |
| Four pipelines: PremultipliedAlpha and Additive, each with a GI twin built only while a seam is installed; GI uses table 0 `t0` (Texture3D) and `t1` (Texture2D) | `ParticleRenderer.cpp:171-300`, `ParticleRenderer.hpp:63-102`, `:130` |
| The GI term is `o.rgb *= coneTracedIndirect(wpos, N, ao) * ao` with `N` toward the camera; no sun, no sky ambient, no local light | `shaders/particles.hlsl:48-70` |
| **No fog anywhere in the particle module** (`grep -i fog modules/particles` returns nothing). The shared prelude has `averFogTermsAirVis` (extinction and in-scatter as two terms), already prepended to the particle shader | `shared_prelude.hlsl:641-654`, `ParticleRenderer.cpp:80-83` |
| Voxi's borrowable slice for foreign pipelines is exactly the cascade lookup `shadowFactor(wpos, N, ndl)` and `coneTracedIndirect`; `averSunRadiance()` and `averSkyIrradiance(N)` are in the shared prelude | `VoxiGiShaders.hpp:12-71`, `voxi_gi.hlsli:194`, `:258`, `shared_prelude.hlsl:166-170`, `:750` |
| Scene lights are gatherable by any module that links `Aver.Scene`: `scene::gatherLights` fills `WorldLight` (position, colour, candela, range, cone) | `modules/scene/include/aver/scene/LightGather.hpp:17-40`, `:88` |
| `.ocparticle` reads `OCPARTICLE 1` only (any other version is refused); records NAME SHAPE BLEND EMISSION LIFETIME DIRECTION SPEED GRAVITY DAMPING SIZE COLOR TEX GI; numbers parse strictly; unknown records survive a load/save cycle | `OcParticle.cpp:139-152`, `:204-208`; `OcParticle.hpp:20-30`; `FORMAT_SPECS.md:1224-1257` |
| The editor tab reimplements spawn and integrate for its preview (a second simulation), draws each particle as a lit cube through `ActorPreview`, and labels the texture field "Unused this slice" | `ParticleEditor.hpp:131-141`, `ParticleEditor.cpp:~475`, `:~578-600` |
| Effects load from `*.ocparticle` under the content root with id `fnv1a64(relative path)`, in the editor and the packaged game | `Runtime/src/GameContent.cpp:1874-1912`, `sandbox/src/SandboxProject.cpp:203`, `GameApp.cpp:1217` |
| `CParticleEmitter` is 24 bytes (`effect`, `age`, `emitAccum`, `seed`, `flags`); flag bit 0 is the negative-sense `kParticleEmitterStopped`; fields are registered, so `GetField/SetField` reach them | `Components.hpp:180`, `:195-201`, `:477`; `Builtins.cpp:120-126` |
| **A level cannot store an emitter**: `ParticleEmitter` is in neither `kPlacedComponentTypes` nor `kPlacedKinds`; the only authoring UI is the Details panel block for an already-attached component | `LevelComponents.hpp:29-31`, `PlacedComponentModel.hpp:46-71`, `SandboxPanels.cpp:2307-2320` |
| Scripting reaches emitters only through entity fields: `AN_SetEmitterEnabled`, `AN_SetEmitterEffect`, `AN_RestartEmitter`, `AN_SpawnEmitterAt` (creates an entity and destroys it on a timer) | `GraphRenderPropsNodeTable.cs:26-29`, `EntityRenderProps.cs:102-150`, `RenderPropsGraph.cs:120-155`, `GraphNodeDefsRenderProps.hpp:39-50` |
| There is no particle C ABI and no user-parameter concept; `modules/framework/include/aver/framework/` has no `framework_particles_abi.h` | directory listing |
| The provider-relay ABI pattern to copy (host installs one function, with none every call returns 0) | `framework_anim_abi.h:1-40`, `AnimAbi.cpp:1-30` |
| Host wiring for game systems lives in one shared header both hosts call | `Runtime/include/aver/game/GameSystemsWiring.hpp:184` (`registerGameSystems`), `:263` (`resetGameSystems`) |

### 1.2 RHI and backends

| Fact | Where |
|---|---|
| Compute pipelines, structured-buffer SRV/UAV slots (with `firstElement`), `dispatch`, buffer barriers, `copyBuffer`, Readback buffers and `readBuffer` all exist | `RHIResources.hpp:140-146` (`BufferDesc`), `:326`, `:544-553`, `:618`, `:624`, `:671-675` |
| **No indirect draw or dispatch, and no instance count on a draw**: `drawIndexed(indexCount, firstIndex, baseVertex)` calls `DrawIndexedInstanced(..., 1, ...)`; `ExecuteIndirect` appears only as a breadcrumb name; no `vkCmdDrawIndexedIndirect` or `vkCmdDispatchIndirect` anywhere | `RHIResources.hpp:645`, `D3D12Device.cpp:9600-9606`, `:714`; grep of `modules/rhi.vulkan` |
| `ResourceState` has no indirect-argument state; the D3D12 and Vulkan maps and the Vulkan buffer usage mask would need one | `RHIResources.hpp:81-96`, `D3D12Device.cpp:616-632`, `VulkanCommon.hpp:517-545`, `:1336-1355` |
| A buffer rests in Common at frame top; barriers name `from` and `to` explicitly | `RHIResources.hpp:139` |
| `prePass` runs before the scene targets are bound (compute is legal, including on Vulkan); `transparentPass` runs inside the scene pass. A Vulkan `bufferBarrier` suspends the render scope, so barriers belong in `prePass` | `RHIResources.hpp:727-729`, `D3D12Device.cpp:4265`, `:5997-6005`, `VulkanDevice.cpp:3142`, `:3357-3363`, `VulkanRenderContext.cpp:1368-1374` |
| `GeometryRead` already covers vertex-shader and fragment-shader reads of a buffer on both backends | `D3D12Device.cpp:627-630`, `VulkanCommon.hpp:535-545` |
| A no-vertex-buffer draw is not expressible: an empty `VertexLayout` means "the engine's MeshVertex" | `RHIResources.hpp:291-296` |
| A second binding table starts its registers where table 0 ends; slot counts are capped at 28 | `RHIResources.hpp:215-224`, `VoxiFroxelFog.hpp:39-42` |
| `runStandaloneCompute` records, submits and waits outside a frame: the GPU test seam (D3D12, WARP or hardware; Vulkan) | `RHI.hpp:433`, `tests/rhi/src/ColourVisionShaderTest.cpp:1-12` |
| `DeviceCaps::computeShaders` is true on D3D12 and Vulkan; the D3D11 backend has no `resources()` override, so `ParticleRenderer::init` already declines there | `RHI.hpp:81-86`, `D3D12Device.cpp:2692`, `VulkanDevice.cpp:1177`, `RHI.hpp:381`, `ParticleRenderer.cpp:74-79` |
| Precedents for the pieces reused: a UAV/StructuredBuffer compute pass with one shared file for both backends and a dummy buffer in unused slots | `modules/render.skin/shaders/skin.hlsl:1-30`, `SkinningPass.cpp:60-80` |

### 1.3 Gaps this design closes

No GPU simulation; no indirect RHI; no module stack; no texture, sprite sheet, rotation or per-particle variation; no ribbons or mesh particles; no fog, sun or local light on particles; no cross-emitter ordering; no spawn-at-position API for other systems (cloth dust and vehicle smoke have nothing to call); no persistence of an emitter in a level; no user parameters; a duplicate simulation in the editor.

---

## 2. Design

### 2.1 Architecture

```
 .ocparticle v2 --> ParticleSystemDef { emitters[] : ParticleEffect }         (Aver.Formats.Particles parses)
                              |
 CParticleEmitter / spawnAt --> ParticleSystem::tick (no RHI)  --------------------------------+
        |   picks tier per emitter instance                                                    |
        |-- CPU tier: EmitterSim (spawn + modules + integrate)  --> upload ring --+            | GpuJob list
        '-- GPU tier: jobs {params, spawn requests, event counts}                 |            | (plain data)
                                                                                  v            v
 ParticleRenderer : IRenderFeature                                      ParticleGpu (RHI)
   prePass     : ParticleGpu::record()  -- Begin, Spawn, Args, Update, Finish, [Sort]  (compute)
   transparentPass : sort emitters by depth; per draw: sprite | ribbon | mesh VS/PS, one instanced
                     draw (CPU: drawIndexedInstanced, GPU: drawIndexedIndirect)
```

`ParticleSystem` stays RHI-free (the split `README.md:22-27` describes). It produces a `std::vector<GpuJob>` the renderer drains in `prePass`; the renderer tells it whether the GPU tier is usable (`setGpuAvailable`) and returns an opaque `gpuHandle` per emitter instance. Units and axes are the engine's (cm, +X forward, +Y right, +Z up, left-handed).

### 2.2 Data model and `.ocparticle` version 2

```cpp
// ParticleTypes.hpp (additions; existing fields keep their names, defaults and meaning)
enum class EmitterShape : u32 { Point, Sphere, Box, Cylinder, Disc };       // appended
enum class SimTier     : u8  { Auto, Cpu, Gpu };
enum class RenderKind  : u8  { Sprite, Ribbon, Mesh };
enum class SpriteAlign : u8  { Camera, Velocity, Axis };
enum class Lighting    : u8  { Unlit, Ambient, Lit };                         // receivesGI true == Ambient
enum class SortMode    : u8  { Auto, None, Depth };                           // Auto: Depth for AlphaBlend/Premultiplied
enum class ModuleKind  : u8  { /* 13 entries, section 2.3 */ };

struct ModuleParam { f32 value = 0; i8 userSlot = -1; u8 comp = 0; };        // bound to user param slot/component or literal
struct Module      { ModuleKind kind; bool enabled = true; ModuleParam p[8]; };
struct Curve4      { u8 n = 0; f32 t[4]; f32 v[4][4]; };                      // n == 0: use the start/end pair
struct UserParam   { std::string name; u8 kind; f32 v[4]; };                  // 0 float, 1 vec3, 2 colour; slot = index (max 16)
struct EventSpawn  { u8 on /*0 death, 1 collide*/; u32 emitter; u32 count; f32 inheritVelocity; };

struct ParticleEffect {                       // ONE EMITTER. Existing members unchanged.
    // new:
    SimTier simTier = SimTier::Auto;
    RenderKind render = RenderKind::Sprite;
    SpriteAlign align = SpriteAlign::Camera; Vec3 alignAxis{0,0,1}; f32 stretch = 0;     // Velocity: length *= 1 + stretch * speed / 100
    u16 subuvCols = 1, subuvRows = 1, subuvFrames = 1; f32 subuvFps = 0; bool subuvBlend = false;
    u64 meshId = 0;  u8 meshOrient = 0;       // fnv1a64(path) of an .ocmesh; 0 velocity, 1 spin, 2 fixed
    u8 trailPoints = 12; f32 trailSegmentCm = 4; f32 trailFade = 1;  bool linked = false; u16 linkedCapacity = 1024;
    Lighting lighting = Lighting::Ambient;    // receivesGI kept as a view of this: false == Unlit
    SortMode sort = SortMode::Auto;  bool fog = true;
    f32 boundsRadiusCm = 0;                   // 0: computed by the editor, stored in the file
    f32 cullDistanceCm = 0;                   // 0: never
    bool spawnAtTarget = true;                // takes part in spawnAt requests (section 2.10)
    Curve4 sizeCurve, colorCurve;             // override sizeStart/End and colorStart/End when n >= 2
    std::vector<Module> modules;              // authored order is execution order within a phase
    std::vector<EventSpawn> events;
    std::string name;                         // emitter name (events and the editor refer to it)
};
struct ParticleSystemDef { std::string name; std::vector<ParticleEffect> emitters; std::vector<UserParam> params; };
```

`ParticleEffectLibrary` stores `ParticleSystemDef` per id (`setSystem/findSystem`); `set/find` keep their signatures and mean "emitter 0" so every existing caller and test is untouched.

**`Particle` (64 bytes, 4 x float4), identical in `ParticleTypes.hpp` and `shaders/particles_common.hlsli`:**

```cpp
struct Particle {
    Vec3 position; f32 age;                 //  0
    Vec3 velocity; f32 lifetime;            // 16
    u32  seed; u32 id; f32 rotation; f32 angularVelocity;   // 32  id: per-instance spawn counter, unique, never reused
    f32  sizeScale; u32 tint /*RGBA8*/; u32 spawnInfo /*u16 start frame | u16 flags*/; u32 trail /*u8 head | u8 count*/;  // 48
};
static_assert(sizeof(Particle) == 64);
```

**Randomness.** `ParticleRandom.hpp` replaces `std::mt19937`: `rand01(seed, stream)` = `(hash32(seed + 0x9E3779B9u * (stream + 1)) >> 8) * (1/16777216)` using the existing splitmix32 `hash32` constants (`ParticleSystem.cpp:21`). Each use of randomness has a fixed named stream id (shape x, shape y, ..., cone phi, speed, lifetime, each spawn module), so results do not depend on call order. A particle's `seed = hash32(emitterSeed ^ (id * 0x85EBCA6Bu))`. The HLSL twin is the same five lines. A golden table of 32 `(seed, stream, value)` triples is asserted in C++ (`ParticleModulesTest`) and read back from the GPU in the parity test.

**Deliberate break:** per-seed particle values differ from 0.7.x runs (distribution and counts do not). Nothing persists particle state, so no data migrates.

**File grammar, `OCPARTICLE 2`** (the writer emits the lowest version that can express the data, so a v1-only effect stays `OCPARTICLE 1` and old tools keep reading it; a newer file in an old tool is refused with the existing version message, `OcParticle.cpp:206-207`). All v1 records and their strict range rules are unchanged. Records before the first `EMITTER` belong to emitter 0.

```
OCPARTICLE 2
NAME Sparks                                 # system name (v1 meaning)
PARAM Intensity float 1.0                   # user parameters: float | vec3 x y z | color r g b a   (max 16)
EMITTER Sparks                              # starts an emitter block; v1 records below belong to it
SIM auto|cpu|gpu
SHAPE cylinder r h [shell]                  # point | box x y z | sphere r | cylinder r h | disc r
RENDER sprite | ribbon trail 12 4.0 fade 1 | ribbon linked 1024 | mesh Meshes/shard.ocmesh velocity|spin|fixed
SPRITE camera | velocity stretch 0.5 | axis x y z
SUBUV 4 4 16 life|fps 24 [blend]
SIZECURVE 0 1  0.3 2  1 0.5                 # 2..4 (t value) pairs
COLORCURVE 0 1 .8 .3 1  0.5 1 .3 .1 .8  1 .1 .1 .1 0      # 2..4 (t r g b a) stops
LIGHTING unlit|ambient|lit                  # GI on == ambient, GI off == unlit (v1 record still accepted)
SORT auto|none|depth
FOG on|off
BOUNDS 600                                  # cm
CULL 8000                                   # cm
SPAWNAT on|off
MODULE spawn.sizescale min=0.6 max=1.4
MODULE update.curlnoise freq=0.01 strength=@Intensity.x scroll=0.3 octaves=2   # @Name[.x|.y|.z|.w] binds a user parameter
ONEVENT death SPAWN Smoke 3 inherit 0.5     # emitter name, count, optional velocity inheritance
```

Parse errors are line-specific and strict like v1 (unknown module key, unknown parameter name, `@` to an undeclared parameter, more than 16 modules, more than 8 emitters, event naming a missing emitter, curve times not increasing). A failed parse leaves the caller's effect at the format's defaults. Spec text for `FORMAT_SPECS.md` §11a is written by the integrator from this section.

### 2.3 The module stack

Closed set of 13, in `ParticleModules.hpp` (`ModuleKind`, a constexpr table of `{key, stage, phase, paramNames[8], defaults[8]}`). Parameter names are the file keys and the editor labels. Defaults make an unconfigured module a no-op. **Execution order per tick, identical on both executors** (this preserves today's order, so a v1 effect simulates as before apart from the RNG): `age += dt; die if age >= lifetime; v += gravity*dt; v *= max(0, 1 - damping*dt); velocity-phase modules in authored order; p += v*dt; position-phase modules in authored order; rotation += angularVelocity*dt; trail push; events`.

| Key | Stage | Phase | Parameters (default) | Rule |
|---|---|---|---|---|
| `spawn.sizescale` | spawn | | min 1, max 1 | `sizeScale = lerp(min, max, rand)`; multiplies the size curve |
| `spawn.colortint` | spawn | | r0 g0 b0 r1 g1 b1 (1) | tint = lerp(c0, c1, rand) |
| `spawn.rotation` | spawn | | minDeg 0, maxDeg 0, spinMin 0, spinMax 0 | initial angle and angular velocity (deg/s) |
| `spawn.inheritvelocity` | spawn | | scale 0 | adds `emitterVelocity * scale` (emitter velocity from the world-matrix delta; spawn-at supplies its own) |
| `update.force` | update | velocity | x y z (0) | constant acceleration, cm/s^2 world (wind) |
| `update.drag` | update | velocity | k 0 | `v -= v * |v| * k * dt`, clamped so speed never reverses |
| `update.attractor` | update | velocity | x y z, strength 0, radius 0, falloff 1, local 0 | pull (+) or push (-) toward a point; `local` makes the point emitter-relative; falloff exponent over `radius` |
| `update.vortex` | update | velocity | ax ay az (0 0 1), strength 0, radius 0, lift 0 | tangential acceleration around an axis through the emitter origin |
| `update.curlnoise` | update | velocity | freq 0.01, strength 100, scroll 0.2, octaves 1, seed 0 | curl of a 3D lattice gradient noise (section 2.12); acceleration = `strength * curl`; the field scrolls with emitter age |
| `update.speedlimit` | update | velocity | max 0 (off), min 0 | clamps speed |
| `update.collideplane` | update | position | nx ny nz (0 0 1), d 0, restitution 0.4, friction 0.2, kill 0 | world plane `n.p = d`; reflect, damp tangential, optionally kill; raises the `collide` event once per particle |
| `update.collidesphere` | update | position | x y z, r, restitution, friction, kill | solid sphere; pushes out and reflects |
| `update.killvolume` | update | position | shape 0 box/1 sphere, cx cy cz, hx hy hz, invert 0 | kills inside (or outside with `invert`) |

Up to 16 modules per emitter. Each parameter is a literal or a user-parameter reference (`userSlot`, `comp` 0-3), resolved from `user[16]` at evaluation on both executors, so a script can drive wind strength or an attractor position live. `EmitterSim` evaluates the table in C++; `particles_common.hlsli` evaluates the same table; `ParticleModulesTest` and `ParticleGpuParityTest` pin them together.

**Simulation stages (neighbour grids, particle-to-particle reads) are not in v1**: they need a spatial structure and a second kernel family; the kernel layout below leaves room for them (section 7, Q3).

### 2.4 The CPU executor, `EmitterSim`

`EmitterSim.hpp/.cpp` (new, scene-free, RHI-free). It is the old `spawnParticle`/`updateParticles` rebuilt on the module table, as an object any caller can own:

```cpp
struct SpawnRequest { Mat4 xf; Vec3 velocity; u32 count; f32 speedScale = 1, sizeScale = 1, lifeScale = 1; u32 tint = 0xFFFFFFFF; u32 seed = 0; };
class EmitterSim {
public:
    void init(const ParticleEffect&, const std::vector<UserParam>&, u32 emitterSeed);
    void setUser(u32 slot, const f32 v[4]);
    u32  spawn(const SpawnRequest&);                         // applies shape, cone, spawn modules; returns accepted (cap = maxParticles)
    void step(f32 dt, f32 time, std::vector<EventRecord>* eventsOut);   // age, integrate, modules, compaction (stable order)
    const std::vector<Particle>& particles() const;
    u32  nextId() const;
};
```

`ParticleSystem::tick` owns one `EmitterSim` per CPU emitter instance; the rate accumulator, the stopped-to-playing burst edge and over-budget-drops-new behaviour (`ParticleSystem.cpp:194-225`) are preserved and run for both tiers (the GPU tier receives the integer spawn count the CPU computed, so rate behaviour is identical). The editor preview owns its own `EmitterSim` (replacing `ParticleEditor::tickPreview`'s copy). Ribbon and mesh emitters are the same `EmitterSim`; `trail` points are stored in a side array (`kPointsPerTrail <= 32` float4 per particle slot) that only trail ribbons allocate.

### 2.5 RHI additions (both backends)

```cpp
// RHIResources.hpp
struct DrawIndexedIndirectArgs { u32 indexCount, instanceCount, firstIndex; i32 baseVertex; u32 firstInstance; };  // 20 B
struct DispatchIndirectArgs    { u32 x, y, z; };                                                                    // 12 B
enum class ResourceState : u8 { /* existing ... */ AccelerationStructure, IndirectArgument };   // appended: no value shifts

// IRenderContext (defaulted: the base class logs once and does nothing, so D3D11/Null still compile)
virtual void drawIndexedInstanced(u32 indexCount, u32 instanceCount, u32 firstIndex = 0, i32 baseVertex = 0, u32 firstInstance = 0);
virtual void drawIndexedIndirect(BufferHandle args, u64 argsOffset = 0);   // args in IndirectArgument state, DrawIndexedIndirectArgs
virtual void dispatchIndirect(BufferHandle args, u64 argsOffset = 0);      // args in IndirectArgument state, DispatchIndirectArgs

// RHI.hpp DeviceCaps
bool indirect = false;    // drawIndexedInstanced + drawIndexedIndirect + dispatchIndirect
```

- **D3D12:** two `ID3D12CommandSignature` (draw-indexed stride 20, dispatch stride 12, no root arguments) created at device init beside the root signature; `drawIndexedIndirect` calls `applyDrawBinding()` then `ExecuteIndirect(sig, 1, argsRes, offset, nullptr, 0)`; `drawIndexedInstanced` is the existing `DrawIndexedInstanced` with the count passed through (`D3D12Device.cpp:9600`); `toResourceStates` maps `IndirectArgument` to `D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT` (`:616`); `caps_.indirect = true` (`:2692`).
- **Vulkan:** `CmdDrawIndexedIndirect` and `CmdDispatchIndirect` added to the function table (core 1.0); `toVkBufferUsage` gains `VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT` ("everything is everything", `VulkanCommon.hpp:1336`); `toVkBufferBarrierInfo` maps `IndirectArgument` to `{VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT, VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT}` (`:517`); `stateName` gets the new case (`VulkanRenderContext.cpp:132`); `caps_.indirect = true` (`VulkanDevice.cpp:1177`).
- Every particle draw uses `firstInstance = 0`, so the DXC SPIR-V mapping of `SV_InstanceID` (instance index minus base instance) never matters.
- D3D11 and Null report `indirect = false`; the particle module then runs the CPU tier only.
- Bounds: args buffers are small `BufferKind::Default` with `allowUnorderedAccess` (written by compute, read by the command processor); a buffer's state sequence per frame is `Common -> UnorderedAccess -> IndirectArgument -> Common (decay)`.

### 2.6 The GPU executor

**Pool.** `ParticleGpuPool` suballocates per-instance ranges from large buffers per size class (capacity 256, 1,024, 4,096, 16,384, 65,536, 262,144, 1,048,576 particles; an emitter takes the smallest class that holds `maxParticles` and the logical cap stays `maxParticles`). Each class owns one buffer per resource kind, bound per instance through `setUavBuffer(..., firstElement)` (`RHIResources.hpp:546`): `particles` (64 B), `deadList` (4 B), `aliveA`/`aliveB` (4 B each), `sortKey` (8 B, only sorted emitters), `trail` (16 B x `trailPoints`, only trail ribbons). About 84 bytes per particle; one million = 84 MB. Allocation is by `acquire(effect)` when an emitter instance is created, `release(handle)` when it is retired. Small per-instance resources: `counters` (16 uint), `args` (draw 20 B + dispatch args 12 B x 3, IndirectArgument), `requests` (Upload ring, 64 B x 64, one per frame in flight), `events` (32 B x 4,096, only emitters that raise or consume events), a 64 B readback slot.

**Per-frame job** (`GpuJob`, plain data from `ParticleSystem::tick`): `SimParams` (about 480 bytes: emitter world matrix, dt, time, spawn count, capacity, first spawn id, emitter seed, shape, cone, speed and lifetime ranges, gravity, damping, inherited velocity, scale modifiers, `float4 user[16]`) written with `setConstantBuffer(3, ...)`, the module table (`StructuredBuffer<float4>`, 3 float4 per module, static per effect), and up to 64 spawn requests (`SpawnRequest`, 64 B). C++ and HLSL structs are mirrored in `ParticleGpuTypes.hpp` and `particles_common.hlsli` with `static_assert`s on sizes and offsets, and `CSProbe` writes sentinel field values the parity test reads back (the three-mirrors lesson).

**Kernels** (`shaders/particles_sim.hlsl`, one compute layout shared by all: 4 SRV, 10 UAV, `b3` root CBV; unused slots bind a dummy buffer because D3D12 tier 1 needs a valid descriptor in every declared slot, as `SkinningPass.cpp:60-80` does):

| Kernel | Threads | What it does |
|---|---|---|
| `CSBegin` | 1 | swaps alive lists; zeroes `aliveNext`; computes spawn dispatch args = ceil((cpuSpawn + eventSpawn) / 64) |
| `CSSpawn` | 64 x args | for thread `i`: find its request by the cumulative counts; `InterlockedAdd(deadCount, -1)` pops a slot (underflow restores and drops the spawn, which is the CPU's "over budget drops the new spawn"); initialise the particle through the spawn path (shape, cone, speed, lifetime, spawn modules); `InterlockedAdd` appends to the current alive list |
| `CSArgsUpdate` | 1 | update dispatch args from the alive count |
| `CSUpdate` | 64 x args | reads `alive[i]`; runs the update order of section 2.3; survivors append to `aliveNext`; the dead push their slot to `deadList`; collide and death events append to `events` |
| `CSFinish` | 1 | draw args: `instanceCount = aliveNext` (ribbon trail: `x (trailPoints - 1)`); sort args; counter copy for the stats readback |
| `CSInit` | 64 x ceil(cap/64) | once at acquire: dead list = `0..cap-1`, counters zero |

Atomics are `InterlockedAdd` on `RWStructuredBuffer<uint>`. Barriers (`uavBarrierBuffer`) separate each kernel's writes from the next kernel's reads. After the last kernel `particles`, `aliveNext` and `sortKey` go `UnorderedAccess -> GeometryRead` and `args` goes `UnorderedAccess -> IndirectArgument`; the next frame's `prePass` transitions back from Common. Slot order in the alive list is not deterministic (atomic order), which is why every particle carries a unique `id` and every comparison keys on it.

**Linked ribbons** use no dead list: slot = `id mod capacity`, a segment joins slot `k` and `k-1` when both are alive. The draw is `capacity - 1` instances with dead segments collapsed in the vertex shader (deterministic, no compaction).

**Events.** `ONEVENT death|collide SPAWN child count` appends records in `CSUpdate`; the child's `CSSpawn` of the next tick consumes them (one tick of latency on both tiers; the CPU tier queues the same records). A child may not itself be a parent of the same system in a cycle (the parser refuses cycles).

**Stats.** `CSFinish` copies `aliveNext` into a 3-deep Readback ring; `ParticleGpu::aliveApprox(handle)` reads the slot written three frames ago, never a stall. Script reads (`AN_GetEmitterState`, `Particles.GetStats`) are therefore up to three frames old on the GPU tier and exact on the CPU tier; the node page says so.

**Selection and fallback.** `ParticleSystem` resolves the tier on first sight of an instance: `Cpu` if `simTier == Cpu`, if the renderer reports `!gpuAvailable()` (no resource factory, `!caps.computeShaders`, `!caps.indirect`, shader compile failure, `--particles-cpu`, `RENDER.PARTICLEGPU 0`), or if `Auto` and `maxParticles < 2048`; `Gpu` otherwise; `Gpu` on a device without the capability falls back to CPU with one warning per effect. A device loss tears the pool down and every GPU instance restarts (particles are transient). Budgets: `maxGpuParticles` counts allocated capacity; a refused `acquire` falls back to CPU for that instance.

### 2.7 Sorting

`SortMode::Auto` sorts `AlphaBlend` and `PremultipliedAlpha` emitters; `Additive` and `None` never sort. Sorting is back to front by view depth of the particle centre.
- **CPU tier:** the index list is sorted as today (`std::sort`, `ParticleRenderer.cpp:356`) and uploaded as the `order` buffer.
- **GPU tier:** `CSSortKeys` writes `{ ~asuint(depth), slot }` (depth is positive in front of the camera, so the float bit pattern orders as an unsigned integer) for each alive entry, padded to the next power of two with `0xFFFFFFFF` keys; then a bitonic network (Batcher 1968): `CSSortLocal` does every step whose stride fits a 2,048-element groupshared tile; `CSSortGlobal` does the larger strides one dispatch each; `CSSortLocalMerge` finishes each merge level inside tiles. Dispatches are indirect (sized by `CSFinish` from the alive count). For 65,536 entries that is about 21 dispatches. The pure index math lives in `ParticleSortNetwork.hpp` (strides, partner, direction per step) so the C++ test runs the same network on the CPU and the HLSL reads the same formulas.
- **Above 65,536 alive particles in one emitter:** no sort, one warning per effect, `Additive` recommended (section 7, Q5).
- **Across emitters:** the renderer orders draws by emitter-bounds-centre distance (back to front), ties by authored emitter order, so a smoke emitter in front of a fire draws in front of it; particles of different emitters are never interleaved. This closes the README's "no cross-emitter sort" for the common case and says exactly what remains.

### 2.8 Render kinds

One graphics shader file, `shaders/particles.hlsl` (modified), with three vertex entry points and one pixel entry point selected by defines (`AVER_PARTICLES_KIND`, `AVER_PARTICLES_LIGHT`, `AVER_PARTICLES_GI`):

| Kind | Template geometry | Instances | Vertex shader |
|---|---|---|---|
| Sprite | static quad: 4 vertices (corner in -1..1), 6 indices | alive count | reads `particles[order[SV_InstanceID]]`; size and colour from the curves at `age/lifetime`; billboard from the camera basis passed in the draw constants; `Velocity` alignment stretches along the projected velocity; `Axis` fixes the facing; rotation applied in the quad plane; SubUV frame from `age` or `fps` (with `blend` the pixel shader samples two frames) |
| Ribbon (trail) | static quad strip template: `(t along segment 0..1, side -1..1)` | alive x (points - 1) | decodes particle and segment from the instance id, reads the trail ring, builds the tangent from neighbouring points, widens perpendicular to tangent and view direction; width from the size curve, alpha fade along the trail by `trailFade`, UV `u` along length |
| Ribbon (linked) | same | `linkedCapacity - 1` | segment between slot `k` and `k-1`; collapsed when either is dead |
| Mesh | the mesh's own vertex/index buffers (layout position, normal, uv in 32 bytes) | alive count | per-instance basis from velocity, spin or fixed; uniform scale = size curve / mesh bounding diameter |

Pipelines are keyed `(kind, blend, lightingVariant)` and built lazily in a map (`std::unordered_map<u32, PipelineHandle>`); sprite unlit/ambient are built eagerly at `onRenderTargetsChanged` as today, the rest on first use with one `AVER_INFO`. Table 0 SRVs: `t0` particles, `t1` order, `t2` trail points (dummy when unused), `t3` texture (Texture2D, `SlotKind::Texture2D`), then the GI pair at `kGiSrvBase = 4`; `static_assert(kGiSrvBase == kParticleSrvCount)` and `static_assert(kGiSrvBase + 2 <= rhi::kMaxBindingSlots)`. The per-draw block (`b3`, `setConstantBuffer`): camera right/up, alignment, SubUV, curves (`Curve4` x 2), tint, lights, fog flag, fade distances, trail parameters, `particleCount`. The vertex layout is two attributes for the quad template (`RG32Float`), or the mesh layout split across `RG32Float` + `R32Float` as `ParticleVertex` does today (`ParticleRenderer.cpp:181-186`), since the RHI has no three-component float vertex format. **UNVERIFIED on both backends: a vertex shader reading a table-0 `StructuredBuffer` SRV.** Phase 0's `IndirectRhiTest` is built to settle exactly this before any GPU simulation work depends on it.

**Textures.** `ParticleRenderer::setTextureResolver(TextureFn, void* user)` with `rhi::TextureHandle (*)(u64 textureId, void* user)`; the host maps the id through the texture cache it already keeps for materials (`GameContent.hpp:68` is the precedent) and owns the handle. A missing texture draws the procedural dot. `Aver.Particles` still links no material or format module.

**Mesh data.** `ParticleRenderer::setMeshResolver(MeshFn, void* user)` returns plain arrays (`positions`, `normals`, `uvs`, `indices`); the renderer builds Upload vertex/index buffers once per mesh id and caches them (limit 2,048 triangles; larger is refused with a log naming the mesh).

**Culling.** `boundsRadiusCm` and `cullDistanceCm` cull whole emitters on the CPU against the camera frustum (from `viewProj`) and distance; a culled GPU emitter is neither dispatched nor drawn (its particles freeze and resume, documented).

### 2.9 Lighting and fog

| `LIGHTING` | Pixel shader adds | Needs |
|---|---|---|
| `unlit` | nothing (`receivesGI false`) | |
| `ambient` (default, today's behaviour) | `rgb *= coneTracedIndirect(wpos, N, ao) * ao`, `N` toward the camera | GI seam; without it the particle draws unlit exactly as today |
| `lit` | `rgb *= (sun + sky + locals) * ao`: `sun = averSunRadiance() * shadowFactor(wpos, N, ndl) * wrap(ndl)`; `sky = coneTracedIndirect` with the seam, `averSkyIrradiance(N)` without; `locals` = up to 4 scene lights, unshadowed, `radiance * atten(d, range, sourceRadius) * wrap(ndl)` with the cone for spots | seam for shadowed sun and GI; without it the sun is unshadowed |

`wrap(x) = saturate(x * 0.5 + 0.5)` (half-Lambert; a billboard has no stable normal, so the light is wrapped rather than cut). Sprite `N` is toward the camera, as the GI term already assumes (`particles.hlsl:58-63`); mesh particles use the mesh normal. Lights are chosen per emitter on the CPU by the pure function `selectParticleLights(const std::vector<scene::WorldLight>&, Vec3 centre, f32 radius, ParticleLight out[4])`, ranked by irradiance at the emitter bounds centre with `E1m = candela / rhi::kLuminanceToCdm2` (the unit `SceneLight.hpp:45` uses, equal to `rhi::kLuminanceToCdm2`, `RHI.hpp:149`); `Aver.Particles` already links `Aver.Scene`, so it calls `scene::gatherLights` once per frame, no new seam. Additive and self-lit effects keep `unlit`.

**Fog.** The vertex shader calls `averFogTermsAirVis(worldPos, true, 1.0, extinction, inscatter)` per vertex (a ribbon needs it per vertex; for sprites all four corners share the particle centre) and passes both as interpolants. The pixel shader applies the affine decomposition the blended panes use: `PremultipliedAlpha/AlphaBlend: rgb = rgb * extinction + inscatter * a`; `Additive: rgb *= extinction`. `FOG off` skips the call. The aerial-perspective march costs 0.19 ms on a full-screen primary pass (`shared_prelude.hlsl:~614`); per vertex on quads it is far less, but it is **measured in the headed check** (section 6) before the default is accepted. Froxel light shafts live in Voxi's table 0 `u24` and the shade sites (`VoxiRenderer.hpp:190`); they are not reachable from this pipeline and are not applied (Q7).

### 2.10 Events and spawn-at-event (the cross-feature contract)

GPU_PARTICLES owns every particle signature below. SOFTBODY_DEPTH (dust, debris) and VEHICLES (skid smoke, sparks) call them and write no particle code.

**Identity.** An effect is addressed by its content-relative path hashed with `fnv1a64`, the id `loadProjectParticleEffects` already registers (`GameContent.cpp:1906`) and `Assets.ObjectIdOf(path)` computes in C#. Helper (in `ParticleEffectLibrary.hpp`): `inline u64 effectId(std::string_view contentRelativePath)`.

**Semantics.** `spawnAt` creates `count` particles in every emitter of the system whose `SPAWNAT` is on, at `position`, with the effect's authored `direction` rotated so effect +Z maps onto `direction` (author effects pointing +Z; a zero direction means no rotation). It is fire-and-forget: no entity is created. Requests are queued and applied at the start of the next `ParticleSystem::tick` (so a call made in this frame's gameplay step shows this frame); the tick order in both hosts already runs physics before particles (`GameApp.cpp:2162-2205`). Particles live in shared per-`(effect, emitter)` "event instances" that retire after 5 idle seconds, so a thousand skid puffs are one simulation. Not thread-safe: call from the game thread. Returns the number of particles accepted after clamps (`maxParticles`, `maxSpawnAtPerFrame`), 0 for an unknown effect (logged once per id) or when particles are not installed.

```cpp
// modules/core/include/aver/core/EffectSpawn.hpp  (new, header-only, depends on Types.hpp and Math.hpp only)
namespace aver {
using EffectSpawnFn = u32 (*)(u64 effectId, const Vec3& position, const Vec3& direction, u32 count, void* user);
void setEffectSpawner(EffectSpawnFn fn, void* user);                        // Aver.Particles installs; null clears
u32  spawnEffectAt(u64 effectId, const Vec3& position, const Vec3& direction, u32 count = 1);   // 0 when none installed
}

// modules/particles/include/aver/particles/ParticleSpawn.hpp  (new)
namespace aver::particles {
struct SpawnAtParams {
    u64  effect = 0;                   // effectId(path)
    Vec3 position{0, 0, 0};            // world, cm
    Vec3 direction{0, 0, 1};           // world; need not be unit; zero => +Z
    u32  count = 1;                    // particles per participating emitter
    f32  speedScale = 1, sizeScale = 1, lifetimeScale = 1;
    Vec3 inheritedVelocity{0, 0, 0};   // cm/s, scaled by the effect's spawn.inheritvelocity
    f32  tint[4] = {1, 1, 1, 1};
    u32  seed = 0;                     // 0: engine counter (deterministic per run)
};
u32 spawnAt(const SpawnAtParams&);                                                          // process-global ParticleSystem
u32 spawnAt(u64 effect, const Vec3& position, const Vec3& direction, u32 count = 1);        // the one-line form
}
```

Consumers that already depend on nothing new call `aver::spawnEffectAt(id, pos, dir, count)`; consumers that link `Aver.Particles` may call `particles::spawnAt`. With `AVER_MODULE_PARTICLES=OFF` the seam returns 0 and everything builds. A consumer stores effect *paths* in its data (for example an `.ocsoft` `"dustEffect"` key or a vehicle spec) and converts once with `effectId`.

**C ABI and C#:** section 2.11. **Node:** `AN_SpawnParticlesAt` (section 2.13). **Bus events:** `Particles.EffectFinished` (entity) when an emitter flagged `AutoDestroy` has no live particles and has stopped, raised through `Events.Dispatch` (no new entry node).

### 2.11 C ABI and C# API

**`framework_particles_abi.h`** (own `AVER_FW_PARTICLES_ABI_VERSION 1`, provider-relay as `framework_anim_abi.h`; `Aver.Framework` links no particle module, the composition root installs the provider; with none every call returns 0). All names and parameters are hashed with `fnv1a64`.

```c
#define AVER_FW_PARTICLES_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_particles_abi_version(void);
typedef int32_t (AVER_FW_CALL* aver_fw_particles_fn)(int32_t op, int32_t entity, int64_t h0, int64_t h1,
                                                     const float* in, float* outF, int32_t* outI, void* user);
AVER_FW_ABI int32_t aver_fw_set_particles_provider(aver_fw_particles_fn fn, void* user);   /* null clears; returns 1 */

#define AVER_FW_PARTICLES_OP_SPAWN_AT   1   /* h0 effect; in[16]: pos3 dir3 vel3 speedScale sizeScale lifeScale tint4; in[16] count as float; returns accepted */
#define AVER_FW_PARTICLES_OP_SET_PARAM  2   /* entity, h0 name hash, h1 kind (0 float 1 vec3 2 colour), in[4]; returns 1 */
#define AVER_FW_PARTICLES_OP_BURST      3   /* entity, in[0] count */
#define AVER_FW_PARTICLES_OP_STOP       4   /* entity, in[0] killParticles (0/1) */
#define AVER_FW_PARTICLES_OP_MODIFIERS  5   /* entity, in[0..2] rateScale sizeScale speedScale */
#define AVER_FW_PARTICLES_OP_STATE      6   /* entity; outI[0] alive (approximate on the GPU tier), outI[1] tier (0 cpu 1 gpu) */
#define AVER_FW_PARTICLES_OP_STATS      7   /* outI[0..5] gpuEmitters cpuEmitters gpuParticles cpuParticles spawnAtDropped gpuAllocatedKB */
#define AVER_FW_PARTICLES_OP_BUDGET     8   /* in[0..2] maxGpu maxCpu maxSpawnAtPerFrame (<0 keeps) */
#define AVER_FW_PARTICLES_OP_GPU_MODE   9   /* in[0] 0 off 1 auto 2 on */

AVER_FW_ABI int32_t aver_fw_particles_spawn_at(int64_t effect, float px, float py, float pz,
                                               float dx, float dy, float dz, int32_t count);
AVER_FW_ABI int32_t aver_fw_particles_spawn_at_ex(int64_t effect, const float* args16, int32_t count);
AVER_FW_ABI int32_t aver_fw_particles_set_param(int32_t entity, int64_t nameHash, int32_t kind, float v0, float v1, float v2, float v3);
AVER_FW_ABI int32_t aver_fw_particles_burst(int32_t entity, int32_t count);
AVER_FW_ABI int32_t aver_fw_particles_stop(int32_t entity, int32_t killParticles);
AVER_FW_ABI int32_t aver_fw_particles_set_modifiers(int32_t entity, float rateScale, float sizeScale, float speedScale);
AVER_FW_ABI int32_t aver_fw_particles_emitter_state(int32_t entity, int32_t* outAlive, int32_t* outTier);
AVER_FW_ABI int32_t aver_fw_particles_stats(int32_t* outSix);
AVER_FW_ABI int32_t aver_fw_particles_set_budget(int32_t maxGpu, int32_t maxCpu, int32_t maxSpawnAtPerFrame);
AVER_FW_ABI int32_t aver_fw_particles_set_gpu_mode(int32_t mode);
```

The provider (`particles::particleProvider`, `ParticleProvider.hpp/.cpp`, `user = &particleSystem()`) is installed with `aver::setEffectSpawner` in `registerGameSystems`. Calls that mutate an instance (parameters, burst, stop, modifiers) are queued and applied at the top of the next tick in call order. `AbiEnumTest` gets a row for `AVER_FW_PARTICLES_OP_*`.

**C#** (`Aver.Framework`, XML `<summary>` on every public member; `Particles.cs`, `ParticlesNative.cs`, `EntityParticles.cs`):

```csharp
public readonly record struct ParticleStats(int GpuEmitters, int CpuEmitters, int GpuParticles, int CpuParticles, int SpawnAtDropped, int GpuAllocatedKB);
public enum ParticleParamKind { Float = 0, Vector = 1, Color = 2 }

public static class Particles
{
    /// <summary>Spawns count particles of the effect (a content path ending .ocparticle) at a world point, fired along direction (zero: the effect's authored direction). Returns how many were accepted.</summary>
    public static int SpawnAt(string effect, Vec3 position, Vec3 direction, int count = 1);
    /// <summary>As above with scales and an inherited velocity (a vehicle's own velocity, for sparks and smoke).</summary>
    public static int SpawnAt(string effect, Vec3 position, Vec3 direction, int count, float speedScale, float sizeScale = 1f, float lifetimeScale = 1f, Vec3 inheritedVelocity = default);
    /// <summary>The same by effect id (Assets.ObjectIdOf(path)), for callers that hash once.</summary>
    public static int SpawnAt(long effectId, Vec3 position, Vec3 direction, int count = 1);
    public static bool SetBudget(int maxGpuParticles = 2_000_000, int maxCpuParticles = 200_000, int maxSpawnAtPerFrame = 2048);
    public static bool SetGpuMode(int mode);                       // 0 off, 1 auto, 2 on
    public static bool TryGetStats(out ParticleStats stats);
}
public readonly partial struct Entity
{
    public bool SetParticleFloat(string name, float value);
    public bool SetParticleVector(string name, Vec3 value);
    public bool SetParticleColor(string name, float r, float g, float b, float a = 1f);
    public bool BurstEmitter(int count);
    public bool StopEmitter(bool killParticles = false);
    public bool SetEmitterModifiers(float rateScale = 1f, float sizeScale = 1f, float speedScale = 1f);
    public bool TryGetEmitterState(out int aliveParticles, out bool onGpu);
}
```

`Entity.SpawnEmitterAt` (entity based, timed) stays for callers that want a handle. `Aver.Framework` is additive; the integrator bumps its contract version once for the wave.

### 2.12 Patent and clean-room notes

| Technique | Status | Decision |
|---|---|---|
| Bitonic sorting network | Batcher 1968, long expired | used; implemented from the network definition, no vendor code |
| Atomic free-list pop/push and append-compaction of alive lists | the standard GPU particle scheme since 2004 (Kipfer et al., Latta); no known patent | used; written from scratch |
| Decoupled look-back prefix scan, Onesweep radix sort (Merrill and others, NVIDIA); CUB code | patent status **UNVERIFIED**, and the code is a new third-party dependency | **not used**. If a sort above 65,536 is ever wanted, build a plain LSD radix from per-tile histograms and a Blelloch scan (1990) and get counsel to review first |
| Curl noise (Bridson, Hourihan, Nordenstam 2007) | published method, no known patent | used; the potential is a 3D lattice gradient noise written from the definition with the existing `hash32`; derivatives by central difference |
| Simplex noise | US 6,867,776, expired January 2022 | **not used**, nor any noise library |
| Flipbook blending, velocity-aligned stretch, half-Lambert wrap | long public | used |
| Niagara's data model and file format | Epic's | not copied: module names, params and the `.ocparticle` grammar are this engine's own |

Nothing here is legal advice; the owner may want a counsel pass on GPU particle sorting and compaction before a commercial release.

### 2.13 Aver Node family `Particles` v2

Contract of `VISUAL_SCRIPTING_CSHARP_SYNC.md` §2.5, followed exactly: `scripting/csharp/Aver.Graph/GraphParticlesNodeTable.cs` (`internal static partial class GameSystemNodes { private static GenericNodeSpec[] ParticlesTable() => ... }`), palette mirror `sandbox/src/GraphNodeDefsParticles.hpp` (`inline void appendParticlesNodeDefs(std::vector<GraphNodeDesc>& t)`), owner `internal static class ParticlesGraph` in `Aver.Framework/ParticlesGraph.cs` with thin `...ForGraph` wrappers over the public `Particles` and `Entity` members, tests `Aver.Graph.Tests/ParticlesNodeTests.cs` (`[AutoRun] static class ParticlesNodeTests { public static int RunAll() }`, built from `NodeRowKit`). No edit to `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs` or `Program.cs`. Failure rule: false/0, log once per node id, never throw. Palette category `Particles`.

Existing and kept where they are (family `RenderProps`, `GraphRenderPropsNodeTable.cs:26-29`): `an_setemitterenabled`, `an_setemittereffect`, `an_restartemitter`, `an_spawnemitterat`. New keys (the generated page lists all 13 under Particles):

| Key | Kind | Pins (defaults) | Attr | Wraps |
|---|---|---|---|---|
| `an_spawnparticlesat` | Exec | in `x y z`, `dirX=0 dirY=0 dirZ=1`, `count:i=1`; out `spawned:i` | `effect` | `Particles.SpawnAt` |
| `an_setparticlefloat` | Exec | in `entity:i`, `value`; out `success:b` | `param` | `Entity.SetParticleFloat` |
| `an_setparticlevector` | Exec | in `entity:i`, `x y z`; out `success:b` | `param` | `Entity.SetParticleVector` |
| `an_setparticlecolor` | Exec | in `entity:i`, `r g b`, `a=1`; out `success:b` | `param` | `Entity.SetParticleColor` |
| `an_burstemitter` | Exec | in `entity:i`, `count:i=10`; out `success:b` | | `Entity.BurstEmitter` |
| `an_stopemitter` | Exec | in `entity:i`, `killParticles:b=false`; out `success:b` | | `Entity.StopEmitter` |
| `an_setemittermodifiers` | Exec | in `entity:i`, `rateScale=1 sizeScale=1 speedScale=1`; out `success:b` | | `Entity.SetEmitterModifiers` |
| `an_getemitterstate` | Pure | in `entity:i`; out `alive:i`, `onGpu:b`, `found:b` | | `Entity.TryGetEmitterState` |
| `an_getparticlestats` | Pure | out `gpuEmitters:i`, `cpuEmitters:i`, `gpuParticles:i`, `cpuParticles:i`, `ok:b` | | `Particles.TryGetStats` |

Row example: `Reg("an_spawnparticlesat", GenericKind.Exec, typeof(ParticlesGraph), "SpawnParticlesAtForGraph", "x:float,y:float,z:float,dirX:float=0,dirY:float=0,dirZ:float=1,count:int=1", "spawned:int", "p:x,p:y,p:z,p:dirX,p:dirY,p:dirZ,p:count,@effect", "spawned")`. Graph reaction to `Particles.EffectFinished` is `AN_OnEvent`. `scripts/node-catalog.py --write` regenerates `docs/nodes/Particles.md`; no hand edit. The 9 `push_back` rows add the same cost to `GraphNodeDefs.cpp` as any other family file.

### 2.14 Editor

`ParticleEditor` keeps its class, factory, undo (`SnapshotUndo`), dirty handling and the Content Browser entry; the state it snapshots becomes `ParticleSystemDef`. To keep Debug object files small, the ImGui drawing is split: `ParticleEditor.cpp` (state, load/save, undo, preview) and new `ParticleEditorUi.cpp` (all `drawParams` code). The edit logic is an ImGui-free header, `sandbox/src/ParticleStackModel.hpp` (add/remove/duplicate emitter, add/remove/reorder/enable module, bind or unbind a parameter, add/rename/remove user parameter, add/remove event, curve point edits, each clamped to what the parser accepts so a typed value can never be one the save path rejects), so `tests/editor` covers it without a window.

Layout: a header row (Save, Undo, Redo, Play/Pause preview, Restart, **Compute bounds**); an emitter list (add, duplicate, delete, reorder, per-emitter solo/mute for preview); then sections for the selected emitter: *Simulation* (tier, max particles, shape, emission, lifetime, velocity cone, gravity, damping), *Spawn modules*, *Update modules* (a searchable "Add module" popup listing the 13; each row has an enable checkbox, a drag handle, the parameter widgets, and a per-parameter "bind" button that offers the declared user parameters), *Render* (kind, sprite alignment and stretch, SubUV, ribbon, mesh picker, size and colour curves as 2-4 point editors, blend, texture picker that now works), *Lighting and fog* (the three modes, with the Voxi-seam note), *Sorting*, *Events*, *Culling and bounds*, and *User parameters*. The "Unused this slice" label on the texture field and the GI tooltip's stale text are replaced. **Compute bounds** runs `EmitterSim` for 3 s of simulated time at 60 Hz, takes the position bounding sphere from the emitter origin, adds the largest size, scales by 1.2 and writes `BOUNDS`.

**Preview.** `tickPreview` is rewritten over `EmitterSim` (the duplicated spawn/integrate code is deleted, same public methods, same fixed seed). Drawing: `render.actorpreview` gains `ActorPreview::setTransparentHook(std::function<void(rhi::IRenderContext&)>)`, called after the preview's own opaque draws, and the tab owns a private `ParticleRenderer` instance (CPU tier, pipelines baked for the preview target through `onRenderTargetsChanged`) fed from its own `EmitterSim`s. **UNVERIFIED:** how `ActorPreview` records its offscreen pass; if the hook cannot be added without rework, the cube preview stays for v1 and Phase 7 reports it (Q6). The global singletons are never touched by the preview (the constraint at `ParticleEditor.hpp:131-141`).

**Persistence in levels.** New placed component `ParticleEmitter`, appended last to `kPlacedComponentTypes` (`LevelComponents.hpp:29-31`) and `kPlacedKinds` (`PlacedComponentModel.hpp:46-71`): `{"ParticleEmitter", "Particle Emitter", "Plays a particle effect (.ocparticle) from this placement.", "Particle Emitter", scene::kComponentParticleEmitter, "", {".ocparticle"}}`; record `COMPONENT ParticleEmitter effect=Particles/Sparks.ocparticle sim=auto playing=true seed=0` (`effect` is a content path, resolved to the id by the existing `PathOfIdFn`); apply writes every field explicitly (zero-filled storage); capture reads only authored fields (`effect`, flags, seed), never `age`/`emitAccum`. `OcWorld.cpp`'s COMPONENT type list gets the name. The existing Details block (`SandboxPanels.cpp:2307`) moves into `SandboxComponentDetails.cpp` beside the others and gains the tier readout, an "Open effect" button and Burst/Stop test buttons in Play.

**Component flags (append-only, zero-default semantics, no struct growth).** `kParticleEmitterAutoDestroy = 0x2` (destroy the entity when stopped and drained; raises `Particles.EffectFinished`), `kParticleEmitterForceCpu = 0x4`, `kParticleEmitterForceGpu = 0x8`.

**Project and runtime settings.** `RENDER.PARTICLEGPU 0|1|2` in `.ocproject` (`OcProject.hpp/.cpp`, owned-key rule as `RENDER.MANYLIGHTS`, `FORMAT_SPECS.md:1648`), a "Particles on GPU" combo on the Rendering settings page, flags `--particles-cpu` and `--particles-gpu` (beside `--no-particle-gi`, `GameApp.cpp:463`, `SandboxInit.cpp`). Console variables are not added.

### 2.15 Hosts: packaged game and editor Play

Both hosts already attach the renderer after Voxi and tick the system unconditionally (`SandboxInit.cpp:913-931`, `GameApp.cpp:847-870`, `SandboxUpdate.cpp:826`, `GameApp.cpp:2204`). Changes, identical in both:

1. `registerGameSystems()` (`GameSystemsWiring.hpp`) installs the provider, the Core spawner and the `COMPONENT` appliers' dependencies; `resetGameSystems()` (`:263`) calls `ParticleSystem::reset()` (drops event instances, queued jobs and queued spawn-at requests) on Play stop, level change and shutdown.
2. After `init`, each host calls `particleRenderer_.setTextureResolver/setMeshResolver` (new `GameContent::resolveParticleTexture/Mesh`, used by the editor through its own content object) and `setGpuMode` from the project key and flags, and reports `gpuAvailable()` to `particleSystem()`.
3. The sort and light-gather camera data come from `IDevice::camera` inside the renderer; no new per-frame host call. The tick call is unchanged. The editor wraps it in the profiler row `PlayPhase::Particles` (`PlayProfile.hpp`).
4. Edit mode: effects animate outside Play as today; a placed emitter that is `kParticleEmitterForceGpu` runs the GPU tier in the viewport too.
5. Shutdown: `ParticleRenderer::shutdown` (`SandboxApp.cpp:660-661`, `GameApp.cpp:2414-2418`) destroys the pool before the device; unchanged order.

### 2.16 D3D12 and Vulkan parity (checklist)

One HLSL source per stage; no wave ops, no int64 atomics, no vendor extension; `InterlockedAdd` on 32-bit structured buffers, groupshared bitonic tiles, `[numthreads(64,1,1)]` (a multiple of both vendors' wave sizes, and what `skin.hlsl` uses). Compute only in `prePass` (Vulkan forbids it inside dynamic rendering; `bufferBarrier` suspends the scope). Draws only in `transparentPass`. Indirect support on both (section 2.5). `static_assert`s on every constant-block mirror. The same `ParticleGpuParityTest` runs `d3d12` (default, WARP or `hw`) and `vulkan`. Slot numbers asserted: `kGiSrvBase == kParticleSrvCount`, table 0 total <= `kMaxBindingSlots`, compute layout 4 SRV + 10 UAV <= 28, and there is no second table. Any new register in `voxi.hlsl` is not touched; the froxel volume at `u24`/table 1 base is never read.

---

## 3. FILE OWNERSHIP

### 3.1 Created (only this feature touches them)

- `modules/core/include/aver/core/EffectSpawn.hpp`
- `modules/particles/include/aver/particles/ParticleModules.hpp`
- `modules/particles/include/aver/particles/ParticleRandom.hpp`
- `modules/particles/include/aver/particles/EmitterSim.hpp`
- `modules/particles/include/aver/particles/ParticleSortNetwork.hpp`
- `modules/particles/include/aver/particles/ParticleSpawn.hpp`
- `modules/particles/include/aver/particles/ParticleLighting.hpp`
- `modules/particles/include/aver/particles/ParticleGpu.hpp`
- `modules/particles/include/aver/particles/ParticleGpuTypes.hpp`
- `modules/particles/include/aver/particles/ParticleProvider.hpp`
- `modules/particles/src/ParticleModules.cpp`
- `modules/particles/src/EmitterSim.cpp`
- `modules/particles/src/ParticleSpawn.cpp`
- `modules/particles/src/ParticleSort.cpp`
- `modules/particles/src/ParticleLighting.cpp`
- `modules/particles/src/ParticleGpu.cpp`
- `modules/particles/src/ParticleGpuPool.cpp`
- `modules/particles/src/ParticleMeshCache.cpp`
- `modules/particles/src/ParticleProvider.cpp`
- `modules/particles/shaders/particles_common.hlsli`
- `modules/particles/shaders/particles_sim.hlsl`
- `modules/particles/shaders/particles_sort.hlsl`
- `modules/framework/include/aver/framework/framework_particles_abi.h`
- `modules/framework/src/ParticlesAbi.cpp`
- `scripting/csharp/Aver.Framework/Particles.cs`
- `scripting/csharp/Aver.Framework/ParticlesNative.cs`
- `scripting/csharp/Aver.Framework/EntityParticles.cs`
- `scripting/csharp/Aver.Framework/ParticlesGraph.cs`
- `scripting/csharp/Aver.Graph/GraphParticlesNodeTable.cs`
- `scripting/csharp/Aver.Graph.Tests/ParticlesNodeTests.cs`
- `sandbox/src/ParticleStackModel.hpp`
- `sandbox/src/ParticleEditorUi.cpp`
- `sandbox/src/GraphNodeDefsParticles.hpp`
- `tests/particles/src/ParticleModulesTest.cpp`
- `tests/particles/src/EmitterSimTest.cpp`
- `tests/particles/src/ParticleSpawnAtTest.cpp`
- `tests/particles/src/ParticleSortNetworkTest.cpp`
- `tests/particles/src/ParticleLightingTest.cpp`
- `tests/particles/src/ParticleGpuParityTest.cpp`
- `tests/rhi/src/IndirectRhiTest.cpp`
- `tests/framework_particles/CMakeLists.txt`
- `tests/framework_particles/src/FrameworkParticlesAbiTest.cpp`
- `tests/editor/src/ParticleStackModelTest.cpp`
- `test-content/ParticleDemo/Content/Maps/ParticleDemo.ocworld`
- `test-content/ParticleDemo/Content/Particles/Embers.ocparticle`
- `test-content/ParticleDemo/Content/Particles/Rain100k.ocparticle`
- `test-content/ParticleDemo/Content/Particles/FireSmoke.ocparticle`
- `test-content/ParticleDemo/Content/Particles/SparkTrails.ocparticle`
- `test-content/ParticleDemo/Content/Particles/BeamRibbon.ocparticle`
- `test-content/ParticleDemo/Content/Particles/Debris.ocparticle`
- `docs/PARTICLES.md`

Generated, not hand-written: `docs/nodes/Particles.md` (by `scripts/node-catalog.py --write`). The `ParticleDemo` project already holds `Mist` and `Snowfall` and no level (`test-content/ParticleDemo/Content/Particles/`); the new effects and level are procedural, no third-party assets. To recycle (never deleted): none. The CPU vertex-baking code, `ParticleVertex` and the editor's duplicated integrator are removed from inside files that stay.

### 3.2 Modified, owned by this feature

- `modules/particles/CMakeLists.txt`
- `modules/particles/module.json`
- `modules/particles/README.md`
- `modules/particles/include/aver/particles/ParticleTypes.hpp`
- `modules/particles/include/aver/particles/ParticleSystem.hpp`
- `modules/particles/include/aver/particles/ParticleEffectLibrary.hpp`
- `modules/particles/include/aver/particles/ParticleRenderer.hpp`
- `modules/particles/src/ParticleSystem.cpp`
- `modules/particles/src/ParticleEffectLibrary.cpp`
- `modules/particles/src/ParticleRenderer.cpp`
- `modules/particles/src/ParticleShaders.hpp`
- `modules/particles/shaders/particles.hlsl`
- `modules/formats.particles/include/aver/formats/OcParticle.hpp`
- `modules/formats.particles/src/OcParticle.cpp`
- `modules/rhi/include/aver/rhi/RHIResources.hpp`
- `modules/rhi/include/aver/rhi/RHI.hpp`
- `modules/rhi.d3d12/src/D3D12Device.cpp`
- `modules/rhi.vulkan/src/VulkanCommon.hpp`
- `modules/rhi.vulkan/src/VulkanRenderContext.cpp`
- `modules/rhi.vulkan/src/VulkanDevice.cpp`
- `modules/render.actorpreview/include/aver/render/preview/ActorPreview.hpp`
- `modules/render.actorpreview/src/ActorPreview.cpp`
- `sandbox/src/ParticleEditor.hpp`
- `sandbox/src/ParticleEditor.cpp`
- `Runtime/include/aver/game/GameContent.hpp`
- `Runtime/src/GameContent.cpp`
- `tests/particles/CMakeLists.txt`
- `tests/particles/src/ParticleSystemTest.cpp`
- `tests/formats/src/OcParticleTest.cpp`
- `tests/editor/src/ParticleEditorLoadSaveTest.cpp`
- `test-content/ParticleDemo/Game.ocproject`

### 3.3 SHARED (another wave-10 feature or the integrator also touches these; every edit is small and additive)

- `CMakeLists.txt`
- `modules/framework/CMakeLists.txt`
- `cmake/modules.catalog.json`
- `modules/scene/include/aver/scene/Components.hpp`
- `modules/formats/src/OcWorld.cpp`
- `modules/formats/include/aver/formats/OcProject.hpp`
- `modules/formats/src/OcProject.cpp`
- `Runtime/include/aver/game/LevelComponents.hpp`
- `Runtime/src/LevelComponents.cpp`
- `Runtime/include/aver/game/GameSystemsWiring.hpp`
- `Runtime/include/aver/game/GameApp.hpp`
- `Runtime/src/GameApp.cpp`
- `sandbox/src/SandboxApp.hpp`
- `sandbox/src/SandboxApp.cpp`
- `sandbox/src/SandboxInit.cpp`
- `sandbox/src/SandboxUpdate.cpp`
- `sandbox/src/SandboxPanels.cpp`
- `sandbox/src/SandboxComponentDetails.cpp`
- `sandbox/src/PlacedComponentModel.hpp`
- `sandbox/src/SandboxSettings.cpp`
- `sandbox/src/PlayProfile.hpp`
- `sandbox/CMakeLists.txt`
- `tests/abi/src/AbiEnumTest.cpp`
- `tests/rhi/CMakeLists.txt`
- `tests/editor/CMakeLists.txt`
- `CHANGELOG.md`
- `docs/ABI.md`
- `docs/formats/FORMAT_SPECS.md`
- `docs/SCRIPTING_API.md`

What each shared edit is: root `CMakeLists.txt` gets `add_subdirectory(tests/framework_particles)` in the framework test block; `modules/framework/CMakeLists.txt` gets `src/ParticlesAbi.cpp`; `cmake/modules.catalog.json` is regenerated by `scripts/module-catalog.py` after the `module.json` edit; `Components.hpp` gets the three `kParticleEmitter*` flag constants; `OcWorld.cpp` gets `ParticleEmitter` in the COMPONENT type list; `OcProject.hpp/.cpp` get `RENDER.PARTICLEGPU`; `LevelComponents.*` and `PlacedComponentModel.hpp` get the `ParticleEmitter` kind; `GameSystemsWiring.hpp` installs the provider and spawner and resets on `resetGameSystems`; `GameApp.*` and the `Sandbox*` files get resolver and mode wiring, the tier readout in the Details block (moved from `SandboxPanels.cpp:2307` into `SandboxComponentDetails.cpp`), the settings combo, flags and the profiler row; `sandbox/CMakeLists.txt` gets `ParticleEditorUi.cpp`; the `tests/*/CMakeLists.txt` files and `AbiEnumTest.cpp` get one target or one table row each; docs are append-only at integration.

---

## 4. Phased implementation

Each phase leaves the tree buildable and `ctest` green before the next starts. Phases 1 to 4 deliver the whole feature on the CPU tier, so the cross-feature contract is usable from Phase 2.

**Phase 0: RHI indirect (M).** `RHIResources.hpp` structs, `ResourceState::IndirectArgument`, the three `IRenderContext` entry points (defaulted), `DeviceCaps::indirect`; D3D12 command signatures and mapping; Vulkan function table, usage bit, barrier info, state name; `IndirectRhiTest` (compute writes args, `dispatchIndirect` runs, a table-0 `StructuredBuffer` read in a vertex shader of an instanced draw, `drawIndexedIndirect` into a small render target read back). Exit: test green on D3D12 and `vulkan`, skipped (77) where neither exists; this settles the UNVERIFIED vertex-shader SRV question or sends the design to a fallback (vertex-buffer-per-instance) before Phase 5.

**Phase 1: pure core and format (L).** `Particle` to 64 bytes, `ParticleRandom`, `ParticleModules`, `EmitterSim`, `ParticleSystemDef` and library storage, `ParticleSystem::tick` rebuilt on `EmitterSim` with the old rate/burst/budget behaviour, `OcParticle` v2 (parse, write lowest version, round trip), editor preview moved onto `EmitterSim`. The renderer is adapted only enough to read the 64-byte struct (bake path still in place). Exit: `ParticleModulesTest`, `EmitterSimTest`, extended `ParticleSystemTest`, `OcParticleTest` (all v1 files bit-identical on rewrite), `ParticleEditorLoadSaveTest` green.

**Phase 2: spawn-at contract and scripting (M).** `EffectSpawn.hpp`, `ParticleSpawn`, event instances, `framework_particles_abi.h` + `ParticlesAbi.cpp` + provider, C# `Particles`/`Entity` members, `ParticlesGraph`, node table, palette, `ParticlesNodeTests`, `FrameworkParticlesAbiTest`, `AbiEnumTest` row. Exit: tests green; SOFTBODY_DEPTH and VEHICLES can call the contract (CPU tier, sprites).

**Phase 3: unified renderer, sprites (L).** `particles.hlsl` instanced VS/PS, quad template, structured-buffer binding, the CPU upload of `particles` + `order`, curves, SubUV, texture resolver, alignment modes, lighting modes with `selectParticleLights`, fog, lazy pipeline map, cross-emitter sort, culling; bake path removed. Exit: `ParticleLightingTest` green; headed checks 1-5 (section 6) on D3D12 and Vulkan.

**Phase 4: ribbons and mesh particles on the CPU tier (M).** Trail ring, linked ring, ribbon VS, mesh resolver and cache, mesh VS. Exit: headed checks 6-7.

**Phase 5: GPU tier (XL).** `ParticleGpuTypes`, pool, `particles_common.hlsli`, `particles_sim.hlsl`, `ParticleGpu::record`, tier selection and fallback, indirect draw wiring, stats readback, sort (`particles_sort.hlsl`, `ParticleSortNetwork`, `ParticleSort.cpp`). Exit: `ParticleSortNetworkTest`, `ParticleGpuParityTest` (spawn, update with every module, sort, layout probe) green on both backends; headed check 8.

**Phase 6: GPU events, ribbons, mesh, spawn-at (M).** Event buffers and child consumption, trail and linked ribbons and mesh instancing on the GPU tier, spawn-at requests on the GPU tier. Exit: parity test extended for each; headed checks 9-10.

**Phase 7: editor and persistence (L).** `ParticleStackModel`, the tab UI split, Compute bounds, the preview hook (or the recorded fallback), `COMPONENT ParticleEmitter`, Details section, settings page combo, `RENDER.PARTICLEGPU`, flags. Exit: `ParticleStackModelTest`, `ParticleEditorLoadSaveTest`, an `.ocworld` round-trip test for the new component green; headed checks 11-13.

**Phase 8: hosts, content, docs (M).** Wiring in both hosts (section 2.15), the `ParticleDemo` content, `docs/PARTICLES.md`, README rewrite, FORMAT_SPECS §11a, ABI.md, SCRIPTING_API.md, CHANGELOG, generated node page, catalog regeneration. Exit: full `ctest`; `scripts/node-catalog.py --check`; every headed check once on D3D12 and `--backend vulkan`.

---

## 5. Tests

Everything below runs without a GPU unless it says so. `*Test` executables are picked up by the root walk (`aver_register_ctest_in`); GPU tests follow the `ColourVisionShaderTest` pattern (`runStandaloneCompute`, optional `vulkan`/`hw` arguments, exit 77 = skipped when a backend or capability is absent).

**Pure CPU (`tests/particles`, link `Aver.Particles Aver.Scene Aver.Core Aver.Platform`)**
- `ParticleModulesTest`: golden `(seed, stream)` values of the RNG; every module kind against hand-computed numbers (force, drag never reverses a velocity, attractor falloff, vortex tangent perpendicular to the radius, speedlimit, plane reflection with restitution and friction, sphere push-out, killvolume inside/outside, spawn modules' ranges over 10,000 draws); user-parameter binding (a bound parameter change alters the next step); table order = execution order; module with default parameters is a no-op.
- `EmitterSimTest`: the same seed gives byte-identical particle arrays; `maxParticles` cap drops new spawns; ids are unique and increasing; a v1 effect (no modules) conserves the old closed-form (gravity and damping) trajectory within 1e-4; linked-ribbon ring allocation and segment liveness; trail ring contents; death and collide events recorded once per particle; curves evaluate (2, 3 and 4 points).
- `ParticleSpawnAtTest`: `count` accepted and clamped by the per-frame budget; direction maps effect +Z; unknown effect returns 0 once-logged; a request made before `tick` appears in that tick; event instance retires after 5 idle seconds; two systems batch into one instance; `aver::spawnEffectAt` returns 0 without an installed spawner and the particle count with one; `Particles.EffectFinished` for an `AutoDestroy` emitter.
- `ParticleSortNetworkTest`: the network from `ParticleSortNetwork.hpp` sorts 1,000 random arrays of sizes 2 to 65,536 (padded) identically to `std::sort`; step count formula; direction and partner functions for the first and last stage.
- `ParticleLightingTest`: `selectParticleLights` ranking (nearest bright wins, directional excluded, fewer than 4 lights fills the rest with zero radiance), unit conversion matches `rhi::kLuminanceToCdm2`.
- `OcParticleTest` (`tests/formats`): v2 round trip byte-identical on a second write; every v1 fixture writes back as v1; unknown records survive; every refusal in section 2.2 reports a line; `@Param` binding and undeclared-parameter refusal; cycle refusal for events.

**GPU, headless-capable (WARP for D3D12 when asked, `vulkan` argument for Vulkan)**
- `IndirectRhiTest` (`tests/rhi`): see Phase 0.
- `ParticleGpuParityTest` (`tests/particles`): builds the real kernels, runs the layout probe (every `SimParams` field and `GpuParticle` offset), the RNG golden table, then steps 60 fixed frames of a test effect that uses every module on both `EmitterSim` and the GPU executor, reads the particle buffer back through a Readback buffer, matches particles by `id`, and requires equal alive counts and positions within `max(1e-3 * |v|, 0.01)` cm; separate cases for death and collide events, linked ribbons, trail rings, spawn-at requests and the dead-list overflow (spawn drops, no corruption), plus bitonic sort of 5,000 random keys equal to `std::sort`. If the backend lacks `caps.indirect` or compute, the test exits 77.
- Not claimed: the rendered image. `drawIndexedIndirect` pixels are covered by `IndirectRhiTest` if backends allow render targets inside `runStandaloneCompute` (**UNVERIFIED**), otherwise by the headed screenshot comparison (section 6, step 8).

**ABI and C#**
- `FrameworkParticlesAbiTest` (links the `Aver.Framework` DLL): no provider returns 0 for every call and writes nothing; a mock provider receives each op with the documented arguments; version; bad arguments (null outputs, negative counts) return 0.
- `AbiEnumTest`: `AVER_FW_PARTICLES_OP_*` against the C# constants.
- `ParticlesNodeTests` (`[AutoRun]`): `NodeRowKit.CheckRows`, `CheckDefaultPins`, `CheckIlValid`, `CheckCompilerSplit` for the nine rows; a missing `effect` or `param` attribute fails by name; unavailable native provider is not a failure; the four existing emitter keys are still present exactly once (no duplicate across families).

**Editor and level (`tests/editor`)**
- `ParticleStackModelTest`: every edit operation, undo/redo, binding and unbinding, clamping, parameter removal that unbinds its references, canonical record round trip.
- `ParticleEditorLoadSaveTest` (extended): v1 file opens, edits, saves as v1; adding a module saves as v2; preview ticks through `EmitterSim`.
- A `COMPONENT ParticleEmitter` apply/capture round trip in the existing placed-component test (`LevelComponents` pattern), including the zero-filled storage case.

---

## 6. Headed manual checks for the owner

Run headed (project rule). Use `test-content/ParticleDemo`. Do not rebuild over a running editor; the build deploys shaders into it. D3D12 first, then `--backend vulkan`, then the packaged runtime. Remote runs: 1440p shots with a ping.

1. **Fog.** An embers or smoke effect behind a fog-heavy level fades into the fog and into aerial haze at distance (before: it stayed full colour). `FOG off` restores the old look.
2. **Texture and SubUV.** A flipbook smoke sheet plays over life and with `fps`; `blend` removes the stepping; the texture field in the editor no longer says unused.
3. **Alignment.** Sparks stretch along velocity; a ground ring uses `axis`; rotation and spin visible.
4. **Lighting.** Same smoke in `unlit`, `ambient`, `lit`: `lit` darkens in shadow of a wall and picks up a nearby coloured point light; `--no-particle-gi` makes `lit` fall back to unshadowed sun with a log line.
5. **Order.** Fire with a smoke child in front of it and behind it from a moving camera: no popping; additive and alpha emitters overlap correctly.
6. **Ribbons.** Spark trails curve smoothly through a turn with no flipping; a linked-ribbon beam follows a moving emitter; width and fade along length.
7. **Mesh particles.** Debris chunks tumble, collide with the ground plane and are lit.
8. **GPU tier.** The 100k rain effect runs on `SIM gpu`; the profiler row and `Particles.GetStats` show it on the GPU; the same effect on `SIM cpu` looks equivalent (screenshot comparison per backend); the GPU frame cost of 1M particles is measured and recorded (**UNVERIFIED target: under 0.5 ms update on the owner's GPU**); the GPU idle rule applies.
9. **Sort and events.** A dense alpha-blended cloud on the GPU tier does not show back-to-front artefacts; a death event spawns smoke from spark deaths one tick later; a collide event raises splashes from the plane.
10. **Spawn-at.** `AN_SpawnParticlesAt` on a key press fires bursts along the chosen direction; 500 calls in one frame do not hitch and respect the budget counter; a soft-body dust hook or a vehicle skid (once those features land) shows particles with no particle code of their own.
11. **Editor.** Add and reorder modules, bind `update.force` strength to a user parameter and watch it change live from `AN_SetParticleFloat`; Compute bounds fills `BOUNDS`; undo/redo across module edits; the preview shows real blending (or the recorded cube fallback).
12. **Levels.** Add Component > Particle Emitter on a placement, save, reopen, play: the emitter persists; Stop/Burst test buttons work in Play.
13. **Fallbacks and budgets.** `--particles-cpu` runs every effect on the CPU with the same look; `RENDER.PARTICLEGPU 0` too; with 2M GPU particles requested, the allocation refuses cleanly to CPU and logs once; a device-lost simulation restarts effects without a crash.

---

## 7. Open questions (defaults chosen; proceed unless told otherwise)

1. **Local-space simulation.** Not in v1; world space only. Default: a vehicle exhaust uses `spawn.inheritvelocity`. A `SIMSPACE local` flag is a later addition (one extra matrix multiply in the VS).
2. **Depth-buffer collision and soft particles.** Not in v1: the scene depth and the G-buffer view-Z are render targets or depth attachments during `transparentPass`, so a pixel-shader read is a hazard, and a prePass read of last frame's G-buffer needs a state audit (**UNVERIFIED**). Default: planes and spheres only (`update.collideplane/sphere`).
3. **Simulation stages and neighbour grids.** Out of v1. The kernel layout (separate kernels, one shared compute layout) leaves room; a grid needs its own buffers and a hash pass.
4. **Linked-ribbon capacity.** Ring of at most 4,096 points per emitter, default 1,024.
5. **Sort above 65,536 alive particles per emitter.** Unsorted with a warning; use Additive. A radix path needs the patent review of section 2.12.
6. **Editor preview drawing.** Default: add `ActorPreview::setTransparentHook` and a private CPU-tier `ParticleRenderer`; if the preview pass cannot take it without rework, keep the cube preview and report it.
7. **Froxel light shafts and per-particle PBR.** Not applied to particles; analytic height fog and aerial perspective only. Mesh particles use the particle lighting model, not material graphs. Default: as stated.
8. **GPU counts are approximate.** `AN_GetEmitterState.alive` lags up to three frames on the GPU tier (no stalls). Default: accepted and documented.
9. **Project key name.** `RENDER.PARTICLEGPU` (0/1/2). Default: as stated; it needs the `OcProject` owned-key treatment.
10. **GPU threshold.** `Auto` picks the GPU at `maxParticles >= 2048` (dispatch overhead dominates below). Default: 2048; tunable per effect with `SIM`.
11. **Per-seed reproduction.** The RNG change (mt19937 to counter hash) alters particle values for a given seed versus 0.7.x. Default: accepted; counts and distributions are unchanged and nothing persists particle state.
12. **Tolerances.** CPU/GPU parity is within `max(1e-3 * |v|, 0.01)` cm after 60 steps, not bit-exact (fused multiply-add and transcendental differences). Default: as stated; the test prints the worst delta.
13. **Event cycles.** Parent/child event cycles are refused at parse time. Default: refused (no runtime recursion guard needed).
14. **D3D11 and Null backends.** CPU tier only (no resource factory or `caps.indirect == false`). Default: as stated.
