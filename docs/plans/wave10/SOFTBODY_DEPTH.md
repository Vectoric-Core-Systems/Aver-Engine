# Feature: Soft-body destruction depth (solid-looking breaks, fields, break effects, saved damage, proxy cages, cheaper debris)

Design contract for the implementing agent. Owner decision: deepen the existing soft-body destruction only. **No rigid fracture, no Voronoi, no geometry collections.** Pieces stay Jolt soft bodies cut from the authored triangle mesh by the wave-9 tear pipeline (`docs/plans/wave9/SOFTBODY_CLOTH_DESTRUCTION.md`, shipped in commit `0a37294a`). Nothing here is built or run. Format and depth follow `docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md` and the wave-9 soft-body doc.

Rules in force: upgrade the existing systems (no parallel solver, no second asset format, no second editor), no new third-party code (none is needed: Jolt and meshoptimizer are already in the tree), lean code comments, never delete a file, D3D12 and Vulkan parity, patent awareness, no growth of the Debug object files that sit near the COFF section cap (new code goes into new translation units; the node catalog grows by 15 rows).

Every claim in section 1 was read in the tree at `295c4b58` on 2026-10-19 with a `file:line`. **UNVERIFIED** marks anything reasoned from reading but not confirmed by reading the exact path or running it. The wave-9 headed tuning pass has not happened (`test-content/SoftBodyLab/README.md` says "Nothing here has been run"; `docs/SOFTBODY.md:9` still says "not yet compiled or run"), so every preset number below is a starting value.

## 0. Decisions in one screen

1. **Thick breaks are derived draw geometry, not simulation.** A pure function (`Caps.hpp`) builds a back skin (the visible triangles reversed and offset inward along the smooth normal) and wall quads along every boundary edge of the visible triangle set, from the draw indices alone. The cage, the Jolt body and the tear pipeline do not change. Walls get generated UVs (centimetres per tile, rest-length based) and can use their own **interior material** through a companion cap entity; with no interior material they merge into the surface mesh. Thickness, back-skin mode, UV scale and cap LOD live in a new optional `"caps"` section of `.ocsoft` and a per-preset default.
2. **One field system, evaluated on the CPU by a pure library function.** Six kinds (radial force, directional force, damage, anchor release, kill, sleep), four shapes (sphere, box, capsule, everywhere), placeable as the runtime-registered component `CSoftField` on any placed mesh (an invisible Cube or Sphere primitive is the volume), startable transiently from C++/C#/nodes, or fired as a one-shot pulse. They run in a new tick step between the inbox and the impact poll, in a fixed order. The only new physics entry point is `aver_phys_sb_add_velocity` (indexed velocity add); no Jolt object exists per field. This is the contract VEHICLES calls for crash damage (section 2.18).
3. **Break events are wired to effects at emit time.** `.ocsoft` gains an optional `"effects"` section with five slots (impact, tear, detach, dust, release). Particles go through GPU_PARTICLES' dependency-free seam `aver::spawnEffectAt` (no new module edge); sounds go through one host seam, implemented by a new header-only adapter over the audio ABI, the `SequenceHost.hpp` pattern. Counts scale with the event's value; cooldowns, per-tick caps and a distance cull bound the cost. Effects are output only and never feed the simulation.
4. **Damage state is saved through the existing `$State` channel of `Aver.Save`.** Channel `softbody`, a text payload holding a *delta* against the pristine cage the loader rebuilds: the ordered tear log, per-beam plastic set and damage, and per-particle position and velocity. Restore replays the tear log through a topology-only path (`planTear`, split out of `tearPipeline` in this feature) and builds bodies once. Debris entities are skipped at capture and recreated from the replay. A changed mesh or asset (key mismatch) drops the state with a warning and the entity loads intact.
5. **Meshes over the particle cap get a cooked proxy cage.** `.ocsoft` names a proxy `.ocmesh` (an existing format) made in the editor by welding and simplifying with the already vendored meshoptimizer through `trifactor::simplifyMesh`. The runtime simulates the proxy and binds every render vertex to a proxy triangle by closest-point barycentric coordinates plus a normal offset, evaluated on demand against whichever triangle a piece owns. No mean-value or harmonic coordinates (patent note, section 2.19). No runtime auto-simplification in this wave (open question 4).
6. **Debris gets cheaper in five ways.** (a) A debris entity starts invisible and becomes visible only when the draw side has a resident mesh for it (today a new piece draws the whole authored mesh until then; section 1.4). (b) Residents have their own per-frame build budget, separate from the tear budget. (c) A piece asleep long enough is **baked**: its Jolt body is removed and it is redrawn as one plain static mesh, with no skin target, no upload ring and no per-frame pack; the despawn shrink becomes an entity scale. (d) Distance tiers cut the pack cadence and drop caps, and sub-pixel pieces are not drawn. (e) Fragments below `minPieceTriangles` already become `Dust`; the dust slot's particle effect may emit mesh particles, which is the engine's debris instancing (GPU_PARTICLES owns it). No rigid chunk bodies are added.
7. **The three Jolt behaviours the wave-9 doc left open are resolved by reading the vendored source** (section 1.3): `Body::SetAllowSleeping` is honoured by soft bodies; `BodyInterface::SetObjectLayer` works on soft bodies and is what gates their collision with rigid bodies; reading the other body's velocity in `OnSoftBodyContactValidate` is sanctioned by Jolt's own access grant and race-free at one collision step. One bias in the impact closing speed is documented (up to `g * dt`).
8. **Presets are tuned headed with a fixed procedure** (section 6.2): a new `SoftBodyTune` map, a Tuning HUD that shows the last deposit, a headless calibration test that prints the same numbers, and a table of symptom, number and direction. Tuned values are folded back into `presetInfo` (`Material.hpp:80`).
9. **Ghost debris, draw budget, save skip and the particle contract are defects or gaps found while reading**, not new scope; they are fixed here because the new features cannot be verified without them.
10. **Aver Node.** Family `SoftBody` v2 adds 7 rows to `GraphSoftBodyNodeTable.cs` (the existing 8 are untouched, so saved graphs keep loading). A new family `Fields` adds 8 rows with the `an_` prefix. Both follow the wave-9 contract exactly: table rows, palette mirror, thin `...ForGraph` wrappers, `[AutoRun]` tests, no edit to `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs` or `Program.cs` (except the already explicit `SoftBodyNodeTests` line, which stays).
11. **Two ABIs, both versioned.** `framework_softfield_abi.h` is new (`AVER_FW_SOFTFIELD_ABI_VERSION 1`). `framework_softbody_abi.h` goes to version 2 with five additive ops. `physics_softbody_abi.h` finally gets a version macro (`AVER_PHYS_SB_ABI_VERSION`, 2) and one new call.
12. **`.ocsoft` stays version 1.** Every addition is an optional section written only when non-default, so the six SoftBodyLab assets round-trip byte-identically and an older reader ignores what it does not know.
13. **Runtime and Play share everything.** All new simulation, fx, save and render code lives in modules both hosts already link. Hosts get one-line installs next to the existing soft-body installs. Editor-only: Details sections, field gizmos, Tuning HUD, the proxy tool.
14. **Size XL, ten phases** (section 4). Phases A to F are independent of the editor; the proxy (phase I) is the riskiest and last.

## 1. What exists today (verified)

### 1.1 The wave-9 soft-body stack

| Fact | Where |
|---|---|
| The cage is particles, beams (edge constraints with rest, plastic set, accumulated damage), dihedral bends, a triangle skin, a dead-triangle mask and a split log | `modules/softbody/include/aver/softbody/Cage.hpp:18` (`Particle`), `:33` (`Beam`), `:49` (`Bend`), `:58` (`Triangle`), `:66` (`SplitEvent`), `:72` (`Cage`), `:80` (`triDead`) |
| `resolveTears` duplicates a particle per disconnected triangle fan, marks triangles with two or more broken edges dead, leaves a broken beam's endpoints alone | `modules/softbody/src/Cage.cpp:223` (function), `:248` (`spans`), `:299` (duplicate), `:318` (broken beams keep `a`/`b`), `:343` (`triDead`) |
| `planIslands` returns anchored-first islands and the dust particles of pieces under `minPieceTriangles` | `modules/softbody/src/Islands.cpp:8`; `modules/softbody/include/aver/softbody/Islands.hpp:13` |
| `depositEnergy` weights beams by `(1 - d/r)^2` on the beam midpoint, breaks a beam when accumulated damage reaches `toughnessJ` (brittle or `toughnessJ > 0`), and returns a velocity kick | `modules/softbody/src/Damage.cpp:59` (function), `:74` (weight), `:84` (break test); `evaluateStrain` at `:15` |
| Material and the preset table (cloth light and heavy, metal, wood, glass); every number is an UNVERIFIED starting value | `modules/softbody/include/aver/softbody/Material.hpp:23` (`Material`), `:48` (`SoftPreset`), `:51` (`PresetInfo`), `:80` (`presetInfo`) |
| `RenderBinding` maps render vertices to particles (many to one), render triangles to sim triangles (CSR in `simStart`/`simList`), replays splits and filters dead triangles | `modules/softbody/include/aver/softbody/RenderBinding.hpp:15`; `modules/softbody/src/RenderBinding.cpp:33` (`applySplits`), `:56` (`visibleIndices`) |
| `buildSimMesh` welds render vertices into particles and refuses a mesh whose weld exceeds `maxParticles` (default 4096) | `modules/softbody/src/SimMesh.cpp:171`, `:26` (weld), `:177-180` (refusal text); `modules/softbody/include/aver/softbody/SoftAsset.hpp:19-28` (`SimParams.maxParticles`) |
| `.ocsoft` is parsed and written by `SoftAsset.cpp`; a file newer than `kSoftAssetVersion` (1) is refused, unknown keys are ignored | `modules/softbody/src/SoftAsset.cpp:196`, `:208`, `:331`; `modules/softbody/include/aver/softbody/SoftAsset.hpp:15` |
| `CCloth` (40 bytes) and `CDestructible` (48 bytes) are runtime-registered with read-only OUT fields; `Options`, `PieceView`, `SoftEvent` and the host seams (`PathFn`, `ProxyFn`, `NotifyFn`) are in the system header | `modules/softbody.scene/include/aver/softbody/SoftBodySystem.hpp:51`, `:63`, `:74`, `:78`, `:106-116`, `:119`, `:160-166` |
| `tick` order: entries, readback, inbox, impacts, plastic and tear per entry, cloth, LOD and freeze, debris, eviction, publish | `modules/softbody.scene/src/SoftBodySystem.cpp:720-766`; `stepsThisTick` from the physics fixed step at `:739` |
| Readback reads positions only (velocity pointer is null) | `modules/softbody.scene/src/SoftBodySystem.cpp:617` |
| `publish` makes one `PieceView` per live piece; `useEntityWorld` is false for debris | `modules/softbody.scene/src/SoftBodySystem.cpp:647`, `:660-677` |
| Impacts: closing speed below the asset's `minImpactCmS` is dropped; the deposit radius is `12 + 6 sqrt(verts)` clamped 12 to 80 cm; energy is `0.5 m_reduced v^2` | `modules/softbody.scene/src/SoftBodyDamage.cpp:154` (`pollImpacts`), `:175`, `:179`, `:185-188` |
| The tear pipeline is one function: rebuild budget gate, `resolveTears`, binding splits, `planIslands`, dust clusters, debris entities and bodies, then the surviving first island is rebuilt under the same handle | `modules/softbody.scene/src/SoftBodyDamage.cpp:275` (function), `:281` (budget), `:285`, `:296`, `:315-327` (dust), `:366` (`toDust`) |
| A debris entity copies the source `CMeshRenderer` and **sets it visible at creation**, plus `CSoftBody{kSoftBodyPiece}` | `modules/softbody.scene/src/SoftBodyDamage.cpp:257-273`, `:264`, `:268` |
| Debris lifecycle: age, asleep timer, shrink then destroy; eviction by age then distance | `modules/softbody.scene/src/SoftBodyDamage.cpp:453`, `:476` |
| `Repair` destroys the entry and rebuilds it from scratch | `modules/softbody.scene/src/SoftBodyDamage.cpp:436` |
| `buildPieceBody` passes null starting state to the builder (`builder_vertices(..., nullptr, nullptr)`), takes position and velocity from an old body through a state map, and re-takes bend rest angles when `carryPose` | `modules/softbody.scene/src/SoftBodyBuild.cpp:18`, `:122`, `:151`; destructibles are single sided and report impacts at `:115-116` |
| `fillPieceDraw` builds a piece's draw vertices from render vertices (`sourceVertex` maps back to the authored vertex for UV and normal) | `modules/softbody.scene/src/SoftBodyBuild.cpp:161-188` |
| Cloth: palette push, release, distance tiers, 10% freeze hysteresis, active-particle budget, character proxy | `modules/softbody.scene/src/SoftBodyCloth.cpp:14`, `:31`, `:90`, `:116`, `:128` |
| The relay provider handles damage (entity 0 means everything in reach), explode, tear, repair, release, wind, budget, preset, state and events | `modules/softbody.scene/src/SoftBodyProvider.cpp:44-117`; `modules/framework/include/aver/framework/framework_softbody_abi.h:22-35` |
| The draw side: one resident per `(entity, piece)`, a source mesh plus a skin target and a ring of 3 Upload buffers per resident, rebuilt on a topology epoch change, **new residents and epoch rebuilds share `maxRebuildsPerFrame` (2)** | `modules/render.softbody/src/SoftBodyScene.cpp:154` (`buildResident`), `:172-174`, `:184`, `:203`, `:211`, `:221`; `modules/render.softbody/include/aver/render/SoftBodyScene.hpp:37` |
| The pack function recomputes normals from the visible indices and writes culling bounds into `CMeshRenderer` each packed frame | `modules/render.softbody/src/SoftBodyScene.cpp:31`, `:71`, `:259-268` |
| The scene walk substitutes the soft-body draw handle after skinning; **a zero handle means "draw the authored mesh"** | `Runtime/src/GameRender.cpp:666-671`; `Runtime/include/aver/game/GameRender.hpp:331` |
| Both hosts tick the system, then update the draw scene; the notify sink reaches graphs | `sandbox/src/SandboxUpdate.cpp:812`, `:843`; `Runtime/src/GameApp.cpp:2195`, `:2211`; `sandbox/src/SandboxInit.cpp:733`; `Runtime/src/GameApp.cpp:1343` |
| Host installs for the soft-body system | `Runtime/include/aver/game/GameSystemsWiring.hpp:167` (`installSoftBodyHost`), `:184` (`registerGameSystems`), `:248` (`tickSoftBodies`), `:263` (`resetGameSystems`); callers `sandbox/src/SandboxInit.cpp:333`, `Runtime/src/GameApp.cpp:1931` |
| Physics: builder, in-place edge and bend writes, `impulse`, skin push, active, params, aero, impacts | `modules/physics/include/aver/physics/physics_softbody_abi.h` (no version macro; `docs/ABI.md:1382` says so too); `modules/physics/src/PhysicsSoftBody.cpp:413` (finish), `:624` (state, which can return velocities), `:648`, `:665`, `:697`, `:731`, `:751`, `:772` |
| C#: `Entity` soft-body members, `SoftBodies`, `SoftBodyGraphInterop`; the low-level `SoftBodyBuilder` | `scripting/csharp/Aver.Framework/SoftBodies.cs`; `scripting/csharp/Aver.Physics/SoftBody.cs:34` |
| Eight nodes with bare keys; the test pins "exactly 8" | `scripting/csharp/Aver.Graph/GraphSoftBodyNodeTable.cs`; `scripting/csharp/Aver.Graph.Tests/SoftBodyNodeTests.cs` (`TestRowSetIsTheDesignSet`), listed explicitly at `scripting/csharp/Aver.Graph.Tests/Program.cs:286` |
| Editor: Cloth and Destructible Details sections, paint tool, strain overlay, `SoftBodyDetailsModel`; Add Component needs a mesh placement | `sandbox/src/SandboxComponentDetails.cpp:362-540`; `sandbox/src/SoftBodyPaintTool.cpp`; `sandbox/src/SoftBodyDebugOverlay.hpp:1-40`; `sandbox/src/SoftBodyDetailsModel.hpp`; `sandbox/src/PlacedComponentModel.hpp:66` (the Cloth row), `:123` (needs a mesh) |
| Level records `COMPONENT Cloth` and `COMPONENT Destructible`; runtime damage is documented as never saved | `Runtime/src/LevelComponents.cpp:31`, `:397`; `docs/formats/FORMAT_SPECS.md:977-978` |
| Test content: the glass pane and steel door are **single sheets**; the crate is a closed box | `test-content/SoftBodyLab/make_soft_lab.py:84-87`; `test-content/SoftBodyLab/Content/Soft/GlassPane.ocsoft` |

### 1.2 Verified absent (grepped before claiming)

| Claim | Search |
|---|---|
| No field, volume or kill-zone concept exists for soft bodies | `grep -rniE "SoftField|FieldSpec|kill volume|force field" modules/softbody modules/softbody.scene modules/render.softbody modules/physics/src/PhysicsSoftBody.cpp scripting/csharp/Aver.Framework/SoftBodies.cs` returns nothing |
| No thickness, back skin or interior face anywhere in the soft-body stack | `grep -rniE "thickness|backskin|interior" modules/softbody modules/softbody.scene modules/render.softbody` returns nothing |
| The soft-body modules call no particle or audio code | `grep -rniE "spawnAt|aver_audio|ParticleEmitter" modules/softbody modules/softbody.scene modules/render.softbody` returns nothing |
| Nothing saves or restores soft-body damage | `grep -rniE "ocsave|saveState|restoreState|aver::save" modules/softbody modules/softbody.scene` returns nothing |
| No proxy, simplification or baking | `grep -rniE "proxy|instanc|bake" modules/softbody/src modules/softbody/include modules/render.softbody` finds only the unrelated character-collision proxy and one comment about baking the entity matrix |
| The visible flag of a debris entity is never withheld | `grep -rn kMeshRendererVisible modules/render.softbody modules/softbody.scene` finds only the set at `SoftBodyDamage.cpp:264` and the hide and restore of a root at `SoftBodyDamage.cpp:355`, `SoftBodySystem.cpp:583` |
| The physics ABI has no indexed velocity add for soft bodies | `grep -n add_velocity modules/physics/include/aver/physics/*.h` finds only the rigid `aver_phys_body_add_velocity` (`physics_abi.h:183`) |

### 1.3 The three Jolt behaviours the wave-9 doc marked unconfirmed (read in `modules/physics.jolt`)

**(a) `Body::SetAllowSleeping` on a soft body: honoured.** `Body::SetAllowSleeping` writes `mMotionProperties->mAllowSleeping` with no rigid-body assertion (`Jolt/Physics/Body/Body.cpp:74-79`). A soft body's motion properties derive from `MotionProperties`, and the soft-body update reads exactly that flag to decide whether it can sleep (`Jolt/Physics/SoftBody/SoftBodyMotionProperties.cpp:903-913`). The engine uses it to keep a winded cloth awake (`modules/physics/src/PhysicsSoftBody.cpp:298`, restored at `:791`) and `tests/physics/src/SoftBodyBuilderTest.cpp:494-495` asserts both outcomes. Nothing to build.

**(b) `SetObjectLayer` on a soft body: works, and it is what routes soft-versus-rigid collision.** `BodyInterface::SetObjectLayer` has no body-type restriction; it updates the layer and notifies the broad phase (`Jolt/Physics/Body/BodyInterface.cpp:357-378`). The soft body then collects colliding bodies with the layer filter of **its own** object layer (`Jolt/Physics/SoftBody/SoftBodyMotionProperties.cpp:252`, `DefaultObjectLayerFilter object_layer_filter = inSystem.GetDefaultLayerFilter(layer)`), so the engine's user-layer matrix gates cloth and debris against the world and the character proxy exactly as the wave-9 design assumed (`modules/physics/src/PhysicsSoftBody.cpp:798`). **Soft-versus-soft stays impossible**: the collector only accepts rigid bodies (`SoftBodyMotionProperties.cpp:147`, "TODO: We should support soft body vs soft body"). Self-collision is likewise absent, so wave-9 open question 1 stands.

**(c) Contact-callback velocity: safe to read.** The validate callback runs inside the **collide** job (`JobSoftBodyCollide`, `Jolt/Physics/PhysicsSystem.cpp:2782-2800`), which Jolt itself brackets with an access grant of read on rigid-body positions and velocities (`:2785-2786`, "Reading rigid body positions and velocities"), and Jolt reads `mp->GetLinearVelocity()` of the other body at the same point (`SoftBodyMotionProperties.cpp:216`). Rigid-body velocities are written back only in `JobSoftBodyFinalize` (`PhysicsSystem.cpp:2842`, calling `UpdateRigidBodyVelocities` at `:2861`; `SoftBodyMotionProperties.cpp:923-933`), and the finalize job depends on every simulate job (`PhysicsSystem.cpp:2748-2767`), which in turn wait for the collide jobs (`:2770-2778`). So `PhysicsSoftBody.cpp:176-190` reads a stable value at one collision step (`g_world->system.Update(dt, 1, ...)`, `modules/physics/src/PhysicsWorld.cpp:478`). The `OnSoftBodyContactAdded` callback runs inside the simulate job (`SoftBodyMotionProperties.cpp:842`) while rigid velocities are still unwritten; the engine already guards its pair table and event list with `g_eventMutex` because several soft bodies update on worker threads. **Documented bias:** the vertex velocity is cached at the start of the step (`PhysicsSoftBody.cpp:292`) but the other body's velocity is read after that step's integration, so a falling other body shows up to one step of gravity extra (at 60 Hz, about 16 cm/s) in the closing speed (`PhysicsSoftBody.cpp:218`). `minImpactCmS` thresholds (120 to 200 cm/s in the lab assets) sit well above it; the tuning table accounts for it.

### 1.4 Defects and limits found while reading (fixed or handled here)

1. **Ghost debris.** A debris entity is created visible (`SoftBodyDamage.cpp:264`) and the scene walk draws the authored mesh whenever the soft-body handle is 0 (`GameRender.cpp:666-671`). `SoftBodyScene::update` builds at most `maxRebuildsPerFrame` (2) new residents per frame (`SoftBodyScene.cpp:211`, `:221`) and new residents and epoch rebuilds share that counter. A glass pane that splits into 16 pieces therefore draws up to 14 whole intact panes at the piece centroids for several frames. Fix: section 2.8.
2. **Single-sided sheets.** The lab's glass and steel door are one-triangle-thick sheets and the destructible body is single sided (`SoftBodyBuild.cpp:115`). A shard has no thickness and, where the active renderer culls back faces, vanishes when seen from behind (the renderer's cull behaviour on the ray-driven path is UNVERIFIED; thickness makes it irrelevant). Fix: section 2.3.
3. **Debris is captured as plain entities.** `save::capture` takes every live entity; the hosts pass no skip predicate (`Runtime/src/GameApp.cpp:201-213`, `sandbox/src/SandboxApp.hpp:431-444`, `modules/save/include/aver/save/SaveWorld.hpp:84`). A save taken after a break would restore each `SoftPiece` as an entity with `CSoftBody{kSoftBodyPiece}` and an authored mesh, which nothing simulates or draws correctly. Fix: section 2.6.
4. **The `$State` channel has one managed provider.** `aver_fw_set_state_provider` takes a single function triple and gameplay abilities installs it (`scripting/csharp/Aver.Framework/Gas/GameplayAbilities.cs:141`); the hosts relay straight to it. A native system must be chained in the host relay, not registered as a second provider. Fix: section 2.6.
5. **Closing speed bias** (section 1.3 c).
6. **Docs say "not compiled or run".** `docs/SOFTBODY.md:9` and the lab README predate the commits; the headed pass is the thing missing, not the build. Fixed by the rewrite of those lines in this feature.

### 1.5 Wave-9 deferred list (`docs/plans/wave9/SOFTBODY_CLOTH_DESTRUCTION.md` section 7), status

| Item | Status after this feature |
|---|---|
| Self-collision | Still out: Jolt has none (1.3 b). |
| Retire the Verlet solver and panel | Done in wave 9. |
| Vertex colour | Still out; paint, rules and masks cover it. |
| Decimation for meshes over 4096 particles | **Done here** as a cooked proxy cage (2.7). |
| Damage state saved | **Done here** (2.6). |
| Layers 14 and 15 | Unchanged. |

## 2. Design

### 2.1 Architecture

```
 .ocsoft v1 (+ caps, effects, proxy)        Aver.SoftBody (pure; Core + Formats)
 + CCloth / CDestructible / CSoftField  ->  Fields, Caps, SoftState, ProxyBind (new)  +  Damage, SimMesh, Islands (extended)
                                            |  arrays only
 Aver.SoftBody.Scene  SoftBodySystem        tick: entries, readback, inbox, FIELDS, impacts, plastic, planTear/commitTear,
   (scene, physics, anim.scene, core)         cloth, debris (bake), FX flush, publish (+ caps, cadence)
        |  aver_phys_sb_*  (+ add_velocity)        |  PieceView (+ uv, layer, bake, reveal)       |  aver::spawnEffectAt (GPU_PARTICLES seam)
 Aver.Physics (DLL)                          Aver.Render.SoftBody  SoftBodyScene              sound seam -> Runtime/.../SoftBodyEffects.hpp
                                              (resident budget, UV override, baked static mesh)  -> audio ABI
 Aver.Save  <--  $State "softbody"  <--  game::softBodyState* chained in the hosts' save relay
```

Units and axes are the engine's (cm, +X forward, +Y right, +Z up, left-handed) at every boundary. The dependency graph does not change: `Aver.SoftBody` stays Core plus Formats (`modules/softbody/module.json`), `Aver.SoftBody.Scene` keeps its dependencies (`modules/softbody.scene/module.json`) and gains only a header-only include of `aver/core/EffectSpawn.hpp` (Aver.Core, from GPU_PARTICLES), so `scripts/module-guard-audit.py` and the catalog need no change.

### 2.2 `.ocsoft` additions (still `"version":1`)

All three sections are optional and written only when they differ from the defaults below. Reading is by `fmt::JsonValue` as today; `valid()` refuses negative thickness or distance, more than 8 sounds per slot, and a proxy path that is not a `.ocmesh`. Spec text goes to `docs/formats/FORMAT_SPECS.md:28` (the `.ocsoft` row) via the integrator.

```json
{ "format":"ocsoft", "version":1, "kind":"destructible", "preset":"glass",
  "destruct": { "minImpactCmS":120, "minPieceTriangles":3, "maxPieces":24,
                "impactRadiusScale":1.0, "bake":true },
  "caps":     { "mode":"auto", "thicknessCm":0.6, "backSkin":"auto", "uvCmPerTile":40, "lodCm":2500,
                "interiorMaterial":"M_GlassEdge" },
  "effects":  { "impact": { "particle":"Particles/GlassSparkle.ocparticle",
                            "sounds":["Audio/GlassTick1.wav","Audio/GlassTick2.wav"],
                            "minSpeedCmS":250, "count":4, "countPerMs":3, "volume":0.8, "pitchJitter":0.08,
                            "cooldownS":0.1, "innerCm":300, "outerCm":4000 },
                "tear":   { "particle":"Particles/GlassShards.ocparticle", "sounds":["Audio/GlassBreak.wav"], "count":24 },
                "detach": { }, "dust": { "particle":"Particles/GlassDust.ocparticle", "countPerParticle":0.5 },
                "release":{ } },
  "proxy":    { "mesh":"Meshes/Door.proxy.ocmesh", "maxParticles":2048 } }
```

- `destruct.impactRadiusScale` multiplies the impact deposit radius that is hard-coded today (`SoftBodyDamage.cpp:179`); `destruct.bake` switches settled-piece baking (2.8) per asset.
- `caps.mode`: `auto` (on for a destructible, off for cloth), `off`, `on`. `caps.thicknessCm`: absent or negative means the preset's value. `caps.backSkin`: `auto` (all pieces when the authored mesh has boundary edges, i.e. is an open sheet; debris only when the mesh is closed), `off`, `debris`, `all`. `caps.uvCmPerTile`: wall and cap UV scale. `caps.lodCm`: beyond this distance from the camera a piece draws without caps. `caps.interiorMaterial`: a material **name**, resolved once per entry with `aver_scene_material(0, name)` (`modules/scene/include/aver/scene/scene_abi.h:183`); empty merges the walls into the surface mesh.
- `effects.<slot>`: `particle` is a content-relative path hashed with `fnv1a64` (GPU_PARTICLES' `effectId`); `sounds` are content-relative paths (up to 8; the variant is `hash(seq) % n`, so it is deterministic); `minSpeedCmS` applies to `impact`; the count is `clamp(count + countPerMs * speedMs, 0, 64)` with `speedMs = value * 0.01`; the dust slot uses `countPerParticle * particles` instead. `innerCm`/`outerCm` are the audio attenuation radii.
- `proxy.mesh`: a content-relative `.ocmesh`; `proxy.maxParticles` overrides `sim.maxParticles` for the proxy. When `proxy.mesh` is set the simulation runs on it (2.7).
- Defaults that depend on the preset: `PresetInfo` (`Material.hpp:51`) gains `capThicknessCm` (default member initialiser 0, so the aggregate rows that omit it still compile): cloth 0, metal 0.5, wood 2.0, glass 0.6.

### 2.3 Capped breaks: back skin, walls, generated UVs, interior material

**Pure library, `modules/softbody/include/aver/softbody/Caps.hpp`.**

```cpp
enum class BackSkin : u8 { Auto = 0, Off = 1, Debris = 2, All = 3 };

struct CapSpec {
    f32 thicknessCm = 0.5f;          // world cm (the caller multiplies by the entity's uniform scale)
    bool backSkin = true;            // resolved from BackSkin and the piece by the caller
    f32 uvCmPerTile = 40.0f;
    u32 vertexLimit = 8192;          // per piece; over it the piece draws without caps (logged once)
};

struct CapVertex { u32 src; f32 depth; f32 u, v; };   // position = P(src) - N(src) * depth * thickness, depth 0 or 1

struct CapGeometry {
    std::vector<CapVertex> backVerts;  std::vector<u32> backIndices;   // reversed copies of the visible triangles
    std::vector<CapVertex> wallVerts;  std::vector<u32> wallIndices;   // one quad per boundary edge
    u32 boundaryEdges = 0;
    bool overLimit = false;
};

// Draw-space inputs: `idx` are the piece's visible triangles over `n` draw vertices; `weldOf[d]` is the identity
// of the surface point (a cage particle, so a UV seam is not an edge); `rest` are the draw positions at the epoch
// (lengths for UVs only); `uv` are 2 floats per draw vertex.
CapGeometry buildCaps(const u32* idx, u32 idxCount, const u32* weldOf, const Vec3* rest, const f32* uv, u32 n,
                      const CapSpec& spec);

// Per frame: the smooth normal of every draw vertex (area weighted over its weld group, from the visible
// triangles) and the cap vertex positions from it.
void smoothNormals(const Vec3* pos, u32 n, const u32* idx, u32 idxCount, const u32* weldOf, Vec3* outNormal);
void capPositions(const std::vector<CapVertex>& verts, const Vec3* pos, const Vec3* normal, f32 thicknessCm, Vec3* out);
```

Algorithm. (1) Count every undirected edge `(weld a, weld b)` over the visible triangles; an edge used exactly once is a **boundary edge** and keeps the direction of its one triangle. Because tears duplicate particles, the two triangles across a crack no longer share a weld id, so each side of a crack gets its own boundary and its own wall; an edge shared with a hidden (dead) triangle is a boundary for the same reason. A seam of two draw vertices on one particle is not a boundary. (2) Each boundary edge `(da, db)` becomes a quad with top vertices `{src da/db, depth 0}` and bottom vertices `{src da/db, depth 1}`; the wall UV is `u = h(a,b) + t * len / uvCmPerTile`, `v = depth * thickness / uvCmPerTile`, where `len` is the rest length and `h` is a per-edge hash fraction that decorrelates neighbouring walls (so a fracture face is not a repeating stripe). Winding is chosen so the wall normal points away from its triangle; `CapsTest` pins it on a flat strip. (3) The back skin has one vertex per used draw vertex (`depth 1`, the same UV as the front, so the material continues on the reverse side) and the triangles reversed. (4) `n` boundary edges cost `4n` vertices and `6n` indices; a piece over `vertexLimit` returns `overLimit` and draws uncapped.

**Where it runs.** `fillPieceDraw` (`SoftBodyBuild.cpp:161`) already builds a piece's draw vertices and indices at each topology epoch. After it, when caps are enabled for the piece it calls `buildCaps` with `weldOf[d] = binding.vertexParticle[renderVerts[d]]` and appends the results. `Piece` gains the UV array for every draw vertex (authored UV through `sourceVertex` for the front, generated for the rest), and the draw arrays are laid out `[front | back skin | walls]`. `sourceVertex` of a generated vertex is the new sentinel `kGeneratedVertex = 0xFFFFFFFF`. `refreshDraw` (`SoftBodySystem.cpp:590`) computes the front positions as today, then `smoothNormals` and `capPositions` for the awake pieces only. Cost is O(draw vertices + cap vertices) per awake piece per tick.

**Interior material.** With `caps.interiorMaterial` set the walls leave the surface mesh: each piece owns a **cap entity** created beside it (`world.create("SoftCap")`, parented to the piece's entity with an identity local so both pack against the same origin, `CMeshRenderer` copied from the source with `material = token`, `CSoftBody{kSoftBodyPiece}` so `syncEntries` ignores it, created without the visible bit). It is published as a second `PieceView` (own entity, the walls as its draw arrays) and drawn by the unchanged scene walk, which substitutes the soft-body handle for it like any other piece. No change to `GameRender.cpp`, the scene walk or either backend. It is destroyed with its piece and hidden with its root. The back skin never uses the interior material (it is the reverse of the surface).

**Cap LOD.** A piece beyond `caps.lodCm` publishes the front arrays only (an epoch bump when it crosses, with a 10% hysteresis band) and a hidden cap entity.

### 2.4 The field system

Pure part, `modules/softbody/include/aver/softbody/Fields.hpp` (Core only):

```cpp
enum class FieldKind  : u8 { RadialForce = 0, DirectionalForce = 1, Damage = 2, ReleaseAnchors = 3, Kill = 4, Sleep = 5 };
enum class FieldShape : u8 { Sphere = 0, Box = 1, Capsule = 2, Everywhere = 3 };
inline constexpr u32 kFieldTargetCloth = 1, kFieldTargetDestructible = 2, kFieldTargetDebris = 4, kFieldTargetAll = 7;

struct FieldSpec {
    FieldKind kind = FieldKind::RadialForce;
    FieldShape shape = FieldShape::Sphere;
    u32  targets = kFieldTargetAll;
    bool perSecond = false;            // strength and energy are rates; a pulse applies them once
    Mat4 world = Mat4::identity();     // shape frame, row-vector convention: translation row = centre
    Vec3 extents{100, 100, 100};       // local cm. Sphere: radius = x. Box: half extents. Capsule: radius = x, half height = z (axis +Z)
    Vec3 dir{0, 0, 1};                 // DirectionalForce: local axis. Damage: direction of travel, zero = radial
    f32  strength = 0.0f;              // RadialForce: velocity change at the centre in cm/s, negative pulls. DirectionalForce: velocity change along dir. Per second when perSecond
    f32  energyJ = 0.0f;               // Damage: joules (total for a pulse, per second when perSecond)
    f32  falloff = 1.0f;               // weight = (1 - n)^falloff, n = 0 at the centre line, 1 at the surface
    f32  turbulence = 0.0f;            // 0..1: deterministic noise on magnitude and direction
    f32  wakeCmS = 5.0f;               // a sleeping piece wakes only if the field would change a vertex velocity by this much
};

f32  fieldWeight(const FieldSpec&, const Vec3& worldPos);                                         // 0 outside the shape
Vec3 fieldVelocityDelta(const FieldSpec&, const Vec3& worldPos, f32 weight, f32 seconds, u64 step);   // RadialForce / DirectionalForce
bool fieldOverlapsBounds(const FieldSpec&, const Vec3& lo, const Vec3& hi);                        // broad phase against a piece AABB
void fieldBeamWeights(const Cage&, const FieldSpec&, std::vector<f32>& out);                       // by beam midpoint, 0 for broken beams
u32  anchorsInField(const Cage&, const FieldSpec&, std::vector<u32>& particles);                  // pinned particles inside
```

`fieldWeight` evaluates in the shape's local frame (`inverse(world)`, so non-uniform scale and rotation come free): sphere `n = |p| / x`, box `n = max(|x|/ex, |y|/ey, |z|/ez)`, capsule `n = distance to the axis segment / radius`, everywhere `n = 0`. Turbulence uses an integer hash of `(floor(p / 100 cm), step / 4)` like the aero (`physics_softbody_abi.h` wind note): no clock, no `rand`. A radial field at the centre uses +Z as its direction.

`Damage.hpp` gains `depositWeighted(Cage&, const std::vector<f32>& beamWeight, f32 energyJ, f32 impulseEfficiency, f32 maxKickCmS)`; `depositEnergy` (`Damage.cpp:59`) becomes the sphere case of it and `DamageTest` pins that the results are bit identical to the old function, so impacts, explosions and fields share one break rule.

**Placeable component** (runtime-registered like `CCloth`, 64 bytes, zero-filled storage means a safe default):

```cpp
struct CSoftField {                  // "CSoftField"
    u32 kind = 0, shape = 0, flags = 0, targets = 0;      // targets 0 reads kFieldTargetAll, except Kill which reads Debris only
    f32 extentX = 0, extentY = 0, extentZ = 0;            // 0 reads 100
    f32 strength = 0, energyJPerS = 0, falloff = 0;       // falloff 0 reads 1
    f32 dirX = 0, dirY = 0, dirZ = 0;                     // all zero reads +Z for DirectionalForce
    f32 turbulence = 0;
    u32 outTouched = 0;               // OUT: pieces in reach on the last tick
    u32 outState = 0;                 // OUT: bit0 active, bit1 fired (a kFieldOnce field after it ran)
};
static_assert(sizeof(CSoftField) == 64);
inline constexpr u32 kFieldDisabled = 0x1;   // negative sense: zero-filled = enabled
inline constexpr u32 kFieldOnce = 0x2;       // apply for one tick, then set kFieldDisabled
```

The entity's world matrix is the shape frame, so moving, rotating and scaling the entity (or parenting it to a vehicle) moves the field. `SoftBodySystem::registerComponents` registers it beside `CCloth` and `CDestructible`; `attachSoftField(world, e)` writes defaults. The component needs no mesh of its own, but Add Component requires a mesh placement (`PlacedComponentModel.hpp:123`), so a field **volume is a placed Cube or Sphere primitive** (`sandbox/src/SandboxShell.cpp:2275`, `:2278`) with `visible=0` and `nocollide`, which the Details section sets with one button. Open question 2.

**System API** (all deferred to the next tick, arrival order):

```cpp
u32  SoftBodySystem::pulseField(const FieldSpec& f);                    // one application; returns the pieces in reach now
u32  SoftBodySystem::startField(const FieldSpec& f, f32 lifetimeS);     // transient continuous field (perSecond forced on); 0 = refused
bool SoftBodySystem::stopField(u32 handle);
u32  SoftBodySystem::impact(Vec3 point, Vec3 dir, f32 radiusCm, f32 energyJ, f32 kickCmS);   // Damage pulse + directional kick; the crash helper
scene::Entity SoftBodySystem::spawnFieldEntity(scene::World&, const FieldSpec&, f32 lifetimeS); // an entity with CSoftField that destroys itself
```

**Tick placement and order.** A new step 2b runs after `applyInbox` and before `pollImpacts` (`SoftBodySystem.cpp:720-766` neighbourhood): first the pulses already in the inbox (arrival order, applied inside `applyInbox` as a new item kind), then component fields in ascending entity id, then transient fields in ascending handle. Time is `stepSeconds = stepsThisTick * fixedStep` (the value `stepsThisTick` already derives at `SoftBodySystem.cpp:739`), never the raw frame time, so a `perSecond` field is frame-rate independent and reproducible by step count.

Per field, per entry, per piece (ascending), skipping pieces that fail the target mask (cloth roots, destructible roots, debris), the broad phase `fieldOverlapsBounds` against `boundsMin/Max`, and baked pieces (no body):

- **RadialForce and DirectionalForce.** For each particle of the piece (the readback positions in `posBuf`; invMass 0 skipped) compute weight and `fieldVelocityDelta`; collect `(vertex, dv)` pairs and make **one** `aver_phys_sb_add_velocity` call per piece (section 2.10). A sleeping piece is touched only if some `|dv| >= wakeCmS`. Cloth reach limits still bound the motion (the skinned constraints are unchanged).
- **Damage.** `fieldBeamWeights` then `depositWeighted` with `energyJ * (perSecond ? stepSeconds : 1)`; the same `noteBroken`, kick and `hotFor` path as `depositOn` (`SoftBodyDamage.cpp:42`), including the `kDestructIndestructible` check (`:45-48`) and cloth only when tearable.
- **ReleaseAnchors.** `anchorsInField` lists pinned particles inside; the system un-pins them (`pinned=false`, `reachCm=-1`, `invMass` restored), clears `anchored` where no pin remains and rebuilds the owning piece through `buildPieceBody` with a state map, exactly the shape of `releaseEntry` (`SoftBodyDamage.cpp:413`) restricted to those particles. A door whose hinge particles are in the field falls off; a cape released near its shoulders only loses those pins.
- **Kill.** Pieces whose centroid is inside start the existing shrink-then-destroy (`shrinking = true`, `SoftBodyDamage.cpp:464`); a **root** is affected only when its target bit is set, in which case the same "everything is dust" branch as `SoftBodyDamage.cpp:343-359` runs (body removed, entity hidden, entry kept so `Repair` restores it).
- **Sleep.** Pieces whose centroid is inside are deactivated and held (`aver_phys_sb_set_active(body, 0)`, the same call the distance freeze uses at `SoftBodyCloth.cpp:121`) and flagged `fieldHeld`; when they leave the volume or the field is disabled they are released. A held piece never counts against `maxActiveParticles`.

`CSoftField::outTouched` and the Tuning HUD read the per-field count. A `kFieldOnce` field sets `kFieldDisabled` after it ran. Two runs of the same ordered script give bit-identical results (the field step is in the replay test of section 5).

### 2.5 Break events wired to effects

**Slots.** `FxSlot { Impact, Tear, Detach, Dust, Release }`. They are driven by the places that already call `emit` (`SoftBodySystem.cpp:228`): the impact poll (`SoftBodyDamage.cpp:194`, with the contact normal as direction and the closing speed as value), `noteBroken` (tear: direction up, value the broken-beam count), the detached piece (direction of its velocity), the dust clusters (`SoftBodyDamage.cpp:315-327` and `toDust`, count by particles), and release or repair.

```cpp
struct FxRequest {
    FxSlot slot; scene::Entity entity;
    Vec3 pos, dir; u32 count;
    u64 particle;                    // fnv1a64 of the effect path, 0 = none
    u64 sound;                       // fnv1a64 of the sound path, 0 = none
    f32 volume, pitch, innerCm, outerCm;
};
using SoundFn = void (*)(const FxRequest& r, void* user);
void SoftBodySystem::setSoundSink(SoundFn fn, void* user);
void SoftBodySystem::setEffectsEnabled(bool on);       // the master switch (SoftBodies.SetEffectsEnabled, node setsoftbodyeffects)
```

`emit` queues an `FxRequest` per binding that exists; `flushFx()` at the end of the tick applies the budgets and then dispatches. **Particles** call `aver::spawnEffectAt(r.particle, r.pos, r.dir, r.count)` from `modules/core/include/aver/core/EffectSpawn.hpp` (GPU_PARTICLES contract, `docs/plans/wave10/GPU_PARTICLES.md` section 2.10; it returns 0 when no particle module is installed, so nothing links). **Sounds** go through `SoundFn`.

**Host adapter** `Runtime/include/aver/game/SoftBodyEffects.hpp` (header-only, `#if AVER_WITH_AUDIO_ABI`, the `SequenceHost.hpp:40-45` arrangement): `installSoftBodyEffects(pathFn, pathUser)` sets the sink; the sink resolves `sound` to a path with the same `PathFn` the system already holds for meshes, loads it with `aver_audio_load` (cached by path in Aver.Audio, `audio_abi.h:50`) and plays it with `aver_audio_play_at(sound, x, y, z, volume, pitch, 0, AVER_AUDIO_BUS_SFX, innerCm, outerCm)` (`audio_abi.h:70`). A build without the audio ABI installs nothing and the sounds are silent.

**Budgets.** `Options::fxMaxPerTick = 16`, `fxSoundsPerTick = 6`, `fxMaxDistanceCm = 6000` from `viewPos_`; each slot has a per-entry `cooldownS`; excess requests are dropped (counted in `Stats::fxDropped`). Order is the emit order, so the same run produces the same requests. Effects never touch the simulation or an RNG.

### 2.6 Saving and loading damage

**Channel.** `Aver.Save` already carries a synthetic `$State` component of named text channels per entity, relayed through `Host.stateCount/stateAt/stateSet` (`modules/save/include/aver/save/SaveWorld.hpp:61-72`, `modules/save/src/SaveWorld.cpp:92-96`; payload cap 1 MiB, 64 channels). The soft-body system adds the channel `softbody` on an entity whose entry has damage. `PlaySnapshots` (`sandbox/src/SandboxPlay.cpp:755`) is the editor's pre-Play level backup and is not a gameplay save; it is unchanged.

**Chaining, not a second provider** (1.4 item 4). A new header-only `Runtime/include/aver/game/SoftBodySave.hpp` offers

```cpp
inline i32  softBodyStateCount(scene::Entity e);                                                   // 0 or 1
inline i32  softBodyStateAt(scene::Entity e, i32 index, char* nameBuf, i32 nameCap, char* dataBuf, i32 dataCap);
inline i32  softBodyStateSet(scene::Entity e, const char* name, const char* data, i32 dataLen);   // 1 if "softbody" and well formed
inline bool softBodySaveSkip(scene::Entity e, void* user);                                         // a kSoftBodyPiece entity
```

and each host's three state relays add the soft-body channel after the framework's: `saveStateCount(e) = aver_fw_state_count(e) + softBodyStateCount(e)`; `saveStateAt` forwards indices at or above the framework count to `softBodyStateAt`; `saveStateSet` tries the framework first and falls back to `softBodyStateSet`. The hosts also set `CaptureOptions::skip = &softBodySaveSkip` (pieces and cap entities are not captured; restore needs no skip, it destroys them with everything else) and call `softBodySystem().clear()` before `restore`. These are edits to `saveHost`/`saveLoadProvider` in `Runtime/src/GameApp.cpp:201-250` and `sandbox/src/SandboxApp.hpp:431-480`.

**Payload.** Text `SB1 <meshKey hex> <assetKey hex> <base64 body>`. `meshKey` is `SimMesh::meshKey` (`SimMesh.hpp`, FNV over the welded positions); `assetKey` is a hash of every resolved value that changes the cage (material, preset, sim and destruct params, proxy path). Body, little-endian, produced by the pure codec `modules/softbody/include/aver/softbody/SoftState.hpp`:

```
u8   flags            bit0 released
u32  particleCount    live particles including duplicates
varint tearGroups; per group: varint count, varint beam-id deltas   // beams that broke between two tear pipeline runs, in order
varint deltaCount; per entry: varint beam-id delta, f32 rest, f32 plastic, f32 damage   // beams whose rest != rest0, plastic > 0 or damage > 0
varint goneCount; varint particle-id deltas                          // particles whose piece was already destroyed
particleCount x { f32 pos[3], f32 vel[3] }                           // world cm, cm/s; fixed-point 16 bit relative to the entry AABB over 512 KB
```

A 4096-particle cage is about 100 KB raw and 135 KB as base64, far below the 1 MiB cap. A pristine entry (nothing broken, no plastic, nothing released) writes no channel.

**Capture.** `SoftBodySystem::saveState(Entity, std::string&) const` serialises the entry; broken beams not yet in the log (a tear still waiting for the rebuild budget) are appended as a final group so the loader reaches the state the next tick would. The tear log is new bookkeeping in `Entry`: every `planTear` appends the beams that broke since the previous call.

**Restore.** `restoreState(Entity, std::string_view)` validates the header and stores the blob under the (new) entity handle; `buildEntry` checks for one right after `buildSimMesh`. A key mismatch logs `[SoftBody] entity N: saved damage is for a different mesh or asset; loading intact` and discards the blob. Otherwise: (1) write the beam deltas into the cage; (2) for each logged group set `broken` and run `planTear` (topology only: `resolveTears`, binding splits, `rebuildBends`, `planIslands`, piece table; no physics, no entities, **no events**: `restoring_` suppresses `emit` and fx); (3) mark the `gone` particles' owners as `kNone` so those pieces are not recreated; (4) `commitTear` once for the final piece table: bodies via `buildPieceBody` with the saved positions and velocities as starting state (new `statePos/stateVel` parameters that reach `aver_phys_sb_builder_vertices`, `SoftBodyBuild.cpp:122`, which today passes null) and `carryPose = true` so bend rests take the dented pose, debris entities via `makeDebrisEntity`. Piece ages restart at zero, so debris lives its full lifetime again after a load. The log is capped at `restoreMaxReplayGroups = 64` groups; beyond it consecutive groups are merged (the result is the same cage, only the duplicate numbering may differ; open question 7).

This needs `tearPipeline` split: **`planTear`** (topology and piece table, no physics) and **`commitTear`** (bodies, entities, events). Live ticks call them back to back, so behaviour is unchanged; the existing `SoftBodySystemTest` cases are the regression guard.

### 2.7 Proxy simulation cage

**Why.** A mesh over `sim.maxParticles` today keeps its authored mesh and is not simulated (`SimMesh.cpp:177-180`, logged by `SoftBodySystem.cpp` as "keeps its authored mesh"). A car body or a scanned door has tens of thousands of vertices; Jolt wants a few thousand particles.

**Cooking (editor).** `sandbox/src/SoftProxyTool.hpp/.cpp`, a button in the Cloth/Destructible Details ("Create proxy cage", enabled when the weld exceeds the cap or on demand): weld the mesh with the sim weld, make a single-submesh copy, call `trifactor::simplifyMesh(mesh, ratio)` (`modules/trifactor/include/aver/trifactor/ClusterBuilder.hpp:467`, which wraps `meshopt_simplify`; the editor links Trifactor when present, `sandbox/CMakeLists.txt:538-540`) with `ratio = 2 * targetParticles / weldedTriangles`, and write `<mesh>.proxy.ocmesh` beside the mesh with `fmt::saveOcMesh`. meshopt collapses edges onto existing vertices, so proxy particles lie **on** the authored surface and the binding offsets are near zero. The tool then writes `proxy.mesh` into the `.ocsoft` through the existing undo snapshot. The runtime never simplifies, so Trifactor and meshoptimizer stay out of the packaged soft-body path (the packaged game already links meshoptimizer for collision meshes, `Runtime/src/GameContent.cpp:919`, but nothing here relies on it). With `AVER_MODULE_TRIFACTOR` off the button is disabled with that reason.

**Binding (pure), `modules/softbody/include/aver/softbody/ProxyBind.hpp`.**

```cpp
struct ProxyBind {
    std::vector<u32> homeTri;       // per render vertex: the proxy (cage) triangle it is closest to
    std::vector<f32> bary;          // 3 per render vertex, relative to homeTri
    std::vector<f32> offset;        // cm along the smooth proxy normal at rest (signed)
    f32 maxDistanceCm = 0;          // the largest |offset| bound, for the refusal
};
bool bindRenderToProxy(const SimMeshInput& render, const Cage& proxyCage, const std::vector<Vec3>& bindLocal,
                       f32 maxDistanceCm, ProxyBind& out, std::string* why);
// Position of a render vertex evaluated against ANY proxy triangle `tri` (barycentric extrapolated if it lies outside).
Vec3 proxyEvaluate(const ProxyBind&, u32 renderVertex, u32 tri, const Cage&, const Vec3* smoothNormal);
```

Closest-point-on-triangle with a uniform grid over the proxy triangles (deterministic: ties go to the lower triangle id). Plain barycentric coordinates plus a smooth-normal offset: no mean-value, harmonic or Green coordinates (patent note, 2.19). A render vertex farther than `maxDistanceCm` (default 5% of the mesh diagonal) from every proxy triangle fails the bind with the vertex id, and the entity keeps its authored mesh (logged once).

**Runtime.** `buildEntry` (`SoftBodySystem.cpp:395`) builds the `SimMesh` from the **proxy** mesh when `proxy.mesh` is set, then calls `bindRenderToProxy` with the authored mesh. `RenderBinding::vertexParticle` is filled with the dominant-weight corner particle so every existing consumer (owners, `applySplits`) keeps working; `triSim[r]` is the proxy triangle nearest the render triangle's centroid. Draw positions come from `proxyEvaluate` in `refreshDraw`. **Tear rule:** a draw vertex is evaluated against its home triangle unless the render triangle that uses it belongs to a proxy triangle that no longer shares the particles the home blend uses at that vertex (checked through `Triangle::v`, which tears rewrite); then the vertex is **split for that render triangle** and evaluated against its own proxy triangle, so a render triangle never bridges a crack and a piece owns the vertices of its own triangles (when the home triangle belongs to another piece the same rule applies). Caps (2.3) work unchanged because they read the draw triangles.

**Limits.** A proxy cage dents and tears at proxy resolution, so a dent has the proxy's width; a crack runs along render triangles that straddle it (one render triangle of slack). Proxy particles are capped like any cage. UVs, normals and materials are always the authored mesh's.

### 2.8 Debris: ghost fix, budgets, baking, LOD, instancing

1. **Reveal handshake.** `makeDebrisEntity` (and the cap entity) create the `CMeshRenderer` **without** `kMeshRendererVisible` (`SoftBodyDamage.cpp:264` changes). `PieceView::revealOnReady` is true for them; `SoftBodyScene::update` sets the visible bit and `dirty` the first frame a resident is packed for that view (next to the bounds write, `SoftBodyScene.cpp:259-268`). A host with no draw scene never shows debris, which is correct. The root entity keeps today's rule (authored mesh until the first resident, hide only when `hidden`).
2. **Separate draw budget.** `Options::maxDrawBuildsPerFrame = 8` (new residents and epoch rebuilds in `SoftBodyScene`); `maxRebuildsPerFrame` stays the tear and entry-build budget. Debris meshes are small; 16 pieces appear within two frames.
3. **Baking settled pieces.** In `debrisLifecycle` (`SoftBodyDamage.cpp:453`), a debris piece with `asleepFor >= bakeAfterS` (3 s, default; `destruct.bake` and `Options::bakeEnabled` gate it) is baked: `aver_phys_remove_body`, `p.body = 0`, `p.baked = true`, `p.origin = p.centroid` and `world.setLocalPosition(entity, centroid)`, epoch bump. The `PieceView` carries `bake = true`; `SoftBodyScene::buildResident` then packs once against the new origin and builds a **plain `createMesh`** (not a skin target; `createMesh` exists on both backends, `modules/rhi/include/aver/rhi/RHI.hpp:496`), creating no source mesh and no staging ring, and drops any it had (retired behind the frames in flight like every other buffer). The piece never packs again; its despawn shrink becomes `world.setLocalScale(entity, scale01)` (zero GPU work). `debrisLifecycle` must treat a baked piece as permanently asleep: today it recomputes `asleep` from `p.body` and resets `asleepFor` to 0 when there is none (`SoftBodyDamage.cpp:462-463`), which would stop a baked piece from ever shrinking. Consequences, accepted and documented: a baked piece is no longer a collider or damageable (damage and fields skip it), it is one-way until `Repair`, and a rigid body that lands on it passes through. Roots and cloth are never baked. This also removes the per-frame BLAS refit a skin-target mesh costs (UNVERIFIED: the RT path's handling of skin-target meshes was not read here; the saving for the pack, the ring and the Jolt body is certain).
4. **Distance tiers for drawing.** The existing tiers (`SoftBodyCloth.cpp:90`, 1500 and 3500 cm) also set a pack cadence: tier 0 every tick, tier 1 every 2nd, tier 2 every 4th (phase `pieceId % period` to spread load), published as `PieceView::frozen` for the skipped ticks (the scene already skips a frozen view that has packed once, `SoftBodyScene.cpp:243`). A piece whose bounding radius over its distance is below `cullPixelRatio` (0.004) has its visible bit cleared (debris and cap entities only). Caps drop beyond `caps.lodCm` (2.3).
5. **Instancing, stated precisely.** Pieces are unique topologies and cannot share a mesh, so there is no mesh instancing of pieces. The instanced thing is what the engine cannot afford as bodies: fragments below `minPieceTriangles` already become `Dust` (`SoftBodyDamage.cpp:315-327`); with an effect bound the dust slot spawns `count` particles of a GPU_PARTICLES effect, which may be a **mesh-particle** chunk emitter (their instanced path). The owner chooses the split with `minPieceTriangles` (lower = more real pieces, higher = more particle chunks). Chunks are cosmetic and not colliders; no rigid bodies are introduced.

### 2.9 Rendering interface changes

`PieceView` (`SoftBodySystem.hpp:119`) gains: `const f32* uv` (2 per draw vertex; null means authored), `u32 layer` (0 surface, 1 cap), `bool revealOnReady`, `bool bake`. `kGeneratedVertex = 0xFFFFFFFF` in `sourceVertex` marks a cap vertex. `SoftBodyScene::buildResident` (`SoftBodyScene.cpp:154`) reads UVs from `v.uv` for generated vertices and from the authored mesh otherwise; normals for generated vertices come from the pack. `softBodyPackPieces` is unchanged (normals are already recomputed over all visible indices, so back skin and walls light correctly). New pure helper `softBodyBakeVertices` (CPU, tested) packs the one-time static mesh. No shader, no RHI call, no backend branch: `createMesh`, `createSkinTargetMesh`, `copyBuffer`, `bufferBarrier` and `destroyMesh` already exist on both backends.

### 2.10 Physics ABI v2 (`physics_softbody_abi.h`)

```c
#define AVER_PHYS_SB_ABI_VERSION 2   /* 1 = the wave-9 surface, which had no macro */
AVER_PHYS_API int32_t aver_phys_sb_abi_version(void);

/* Adds dvXyz[i] (world cm/s, 3 floats each) to vertex[i] of a body this ABI built. Kinematic vertices (inverse mass 0)
 * are skipped, indices out of range refuse the call, and the body is activated only when some component is non-zero. */
AVER_PHYS_API int32_t aver_phys_sb_add_velocity(int32_t body, const int32_t* vertex, const float* dvXyz, int32_t n);
```

Implemented in `PhysicsSoftBody.cpp` next to `aver_phys_sb_impulse` (`:697`) with the same write lock and no-lock activation comment, the world-to-body rotation of a free vector, and clearing the record's `frozen`. `AbiEnumTest` pins the version macro and the C# mirror. C#: `Aver.Physics/Native.SoftBody.cs` (names copied character for character) and `SoftBody.AddVelocity(ReadOnlySpan<int> vertices, ReadOnlySpan<Vec3> dv)`.

### 2.11 Framework ABIs

**`framework_softfield_abi.h`** (new; `AVER_FW_SOFTFIELD_ABI_VERSION 1`; `int32_t/float` only; 1 on success, 0 on refusal or no provider; the `framework_softbody_abi.h` provider pattern; every op deferred to the next tick):

```c
#define AVER_FW_SOFTFIELD_ABI_VERSION 1
AVER_FW_ABI int32_t aver_fw_softfield_abi_version(void);

#define AVER_FW_SF_SPEC_COUNT 24
/* spec[]: 0 kind, 1 shape, 2 targets (mask), 3 perSecond (0/1), 4-6 centre xyz (cm), 7-9 extents xyz (cm),
 * 10-12 yaw pitch roll (degrees), 13-15 dir xyz, 16 strength, 17 energyJ, 18 falloff, 19 turbulence, 20 wakeCmS,
 * 21 lifetimeS (start and spawn only), 22-23 reserved (0) */
#define AVER_FW_SF_OP_PULSE   1   /* in: spec; out i[0] = pieces in reach now */
#define AVER_FW_SF_OP_START   2   /* in: spec; out i[0] = handle */
#define AVER_FW_SF_OP_STOP    3   /* in: f[0] = handle */
#define AVER_FW_SF_OP_IMPACT  4   /* in: x y z dx dy dz radiusCm energyJ kickCmS; out i[0] = pieces in reach */
#define AVER_FW_SF_OP_SPAWN   5   /* in: spec; out i[0] = entity (a CSoftField entity that removes itself after lifetimeS) */
typedef int32_t (AVER_FW_CALL* aver_fw_softfield_fn)(int32_t op, int32_t entity, const float* in, int32_t inCount,
                                                     float* outF, int32_t* outI, int32_t cap, void* user);
AVER_FW_ABI int32_t aver_fw_set_softfield_provider(aver_fw_softfield_fn fn, void* user);
AVER_FW_ABI int32_t aver_fw_softfield_pulse(const float* spec, int32_t n);
AVER_FW_ABI int32_t aver_fw_softfield_start(const float* spec, int32_t n);          /* handle, 0 = refused */
AVER_FW_ABI int32_t aver_fw_softfield_stop(int32_t handle);
AVER_FW_ABI int32_t aver_fw_softfield_impact(float x, float y, float z, float dx, float dy, float dz,
                                             float radiusCm, float energyJ, float kickCmS);   /* pieces in reach */
AVER_FW_ABI int32_t aver_fw_softfield_spawn(const float* spec, int32_t n);          /* entity, 0 = refused */
```

`pulse`/`impact` return the number of pieces in reach at call time (an upper bound of what the next tick touches), 0 with no provider. The provider is `softbody::softFieldProvider(...)` declared in `SoftBodySystem.hpp` and installed in `registerGameSystems` beside `softBodyProvider` (`GameSystemsWiring.hpp:184`). Spec values that are not finite, or a kind or shape out of range, refuse the call.

**`framework_softbody_abi.h`** goes to `AVER_FW_SOFTBODY_ABI_VERSION 2` with five additive ops (the relay and the existing 10 ops are untouched):

```c
#define AVER_FW_SB_OP_ASLEEP        11   /* entity; in f[0] = 1 asleep / 0 awake: holds or releases every piece (the Sleep field's hold) */
#define AVER_FW_SB_OP_CLEAR_DEBRIS  12   /* entity: destroys its debris pieces now (shrink skipped) */
#define AVER_FW_SB_OP_EFFECTS       13   /* in f[0] = 1/0: the master switch for break effects */
#define AVER_FW_SB_OP_DENTS         14   /* entity; out f[0] = maxSetCm; out i[0..1] = dentedEdges debrisPieces */
#define AVER_FW_SB_OP_REPAIR_ALL    15   /* out i[0] = entities repaired */
AVER_FW_ABI int32_t aver_fw_softbody_set_asleep(int32_t entity, int32_t asleep);
AVER_FW_ABI int32_t aver_fw_softbody_clear_debris(int32_t entity);
AVER_FW_ABI int32_t aver_fw_softbody_set_effects(int32_t enabled);
AVER_FW_ABI int32_t aver_fw_softbody_dents(int32_t entity, float* outMaxSetCm, int32_t* outDentedDebris2);
AVER_FW_ABI int32_t aver_fw_softbody_repair_all(void);
```

### 2.12 C# API

`scripting/csharp/Aver.Framework/SoftFields.cs` (new) and additions to `SoftBodies.cs`; every public member has an XML summary. Natives in `SoftFieldNative.cs` (`FwSoftField`, the `AnimNative.cs` pattern).

```csharp
/// <summary>What a soft field does (pinned to softbody::FieldKind).</summary>
public enum SoftFieldKind { RadialForce = 0, DirectionalForce = 1, Damage = 2, ReleaseAnchors = 3, Kill = 4, Sleep = 5 }
/// <summary>The volume of a soft field (pinned to softbody::FieldShape).</summary>
public enum SoftFieldShape { Sphere = 0, Box = 1, Capsule = 2, Everywhere = 3 }
/// <summary>Which soft bodies a field touches.</summary>
[Flags] public enum SoftFieldTargets { Cloth = 1, Destructible = 2, Debris = 4, All = 7 }

/// <summary>One field. Extents are local centimetres: a sphere's radius is X; a box's half extents; a capsule's radius is X and half height Z.</summary>
public readonly record struct SoftFieldSpec(
    SoftFieldKind Kind, SoftFieldShape Shape, Vec3 Centre, Vec3 Extents, Vec3 RotationDegrees = default, Vec3 Direction = default,
    float Strength = 0f, float EnergyJoules = 0f, float Falloff = 1f, float Turbulence = 0f,
    SoftFieldTargets Targets = SoftFieldTargets.All, bool PerSecond = false);

public static class SoftFields
{
    /// <summary>Applies the field once at the next tick. Strength is a velocity change in cm/s; energy is total joules. Returns the pieces in reach now.</summary>
    public static int Pulse(in SoftFieldSpec spec);
    /// <summary>The crash helper: damage energy in a sphere at <paramref name="point"/>, shoved along <paramref name="direction"/>
    /// (zero = radial) with an extra kick of <paramref name="kickCmPerS"/>. Returns the pieces in reach now.</summary>
    public static int Impact(Vec3 point, Vec3 direction, float radiusCm, float energyJoules, float kickCmPerS = 0f);
    /// <summary>Starts a continuous field (strength and energy become per second) that stops after <paramref name="lifetimeSeconds"/>. Returns a handle, 0 if refused.</summary>
    public static int Start(in SoftFieldSpec spec, float lifetimeSeconds);
    /// <summary>Stops a field started with Start.</summary>
    public static bool Stop(int handle);
    /// <summary>Creates an entity with a CSoftField that removes itself after the lifetime; move it to move the field.</summary>
    public static Entity Spawn(in SoftFieldSpec spec, float lifetimeSeconds);
}

public readonly partial struct Entity
{
    /// <summary>Adds a CSoftField to this entity (its transform is the field's frame). Needs nothing else.</summary>
    public bool AddSoftField(SoftFieldKind kind, SoftFieldShape shape, Vec3 extents, float strength = 0f, float energyJoulesPerSecond = 0f);
    /// <summary>Enables or disables this entity's soft field.</summary>
    public bool SetSoftFieldEnabled(bool enabled);
    /// <summary>The pieces this entity's field reached on the last tick.</summary>
    public bool TryGetSoftFieldTouched(out int pieces);
    /// <summary>Destroys this soft body's debris pieces now.</summary>
    public bool ClearSoftDebris();
    /// <summary>Holds (true) or releases (false) this soft body's pieces asleep.</summary>
    public bool SetSoftBodyAsleep(bool asleep);
    /// <summary>Permanent set: dented edges, the largest set in cm, and live debris pieces.</summary>
    public bool TryGetSoftBodyDents(out int dentedEdges, out float maxSetCm, out int debrisPieces);
}

// added to SoftBodies
/// <summary>Switches the break effects (particles and sounds bound in .ocsoft) on or off for the whole world.</summary>
public static bool SetEffectsEnabled(bool enabled);
/// <summary>Repairs every soft body; returns how many.</summary>
public static int RepairAll();
/// <summary>Takes one pending event off the queue (the same queue as DrainEvents).</summary>
public static bool TryPopEvent(out SoftEvent ev);
```

`SoftFieldGraph` (internal, in `SoftFields.cs`) and the new `SoftBodyGraphInterop` methods are the `...ForGraph` wrappers (2.13). A field created by `AddSoftField` is attached with the same generic scene ABI as `AddCloth` (add by name, field writes), so it needs no extra native call.

### 2.13 Aver Node

Both families follow the wave-9 contract in `docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md` section 2.5: table rows in `scripting/csharp/Aver.Graph/Graph<Family>NodeTable.cs` (`internal static partial class GameSystemNodes { private static GenericNodeSpec[] <Family>Table() => ... }`), palette mirror `sandbox/src/GraphNodeDefs<Family>.hpp` (`inline void append<Family>NodeDefs(std::vector<GraphNodeDesc>& t)`), tests `[AutoRun] static class <Family>NodeTests { public static int RunAll() }` built from `NodeRowKit.cs`. All discovered; `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs` and `Program.cs` are not edited. Failure rule: false/0 and log once per node id, never throw. Run `scripts/node-catalog.py --scaffold-palette` first, `--check` after; `docs/nodes/*.md` are regenerated, not hand edited.

**SoftBody v2** (seven rows appended to `SoftBodyTable()` in `GraphSoftBodyNodeTable.cs`, owner `SoftBodyGraphInterop`, category `Physics`, bare lower-case keys like the existing eight; the existing rows and pins do not change, so saved graphs keep loading). Exec nodes carry `exec`/`then`/`success`.

| Key | Kind | Pins (besides exec/then/success) | Wraps |
|---|---|---|---|
| `popsoftbodyevent` | Exec | out `found:bool`, `kind:int`, `entity:int`, `piece:int`, `x,y,z:float`, `value:float` | `PopSoftBodyEventForGraph` (`SoftBodies.TryPopEvent`) |
| `setsoftbodybudget` | Exec | in `maxPieces:int=48`, `maxParticles:int=20000`, `maxRebuilds:int=2`, `lifetime:float=12` | `SetSoftBodyBudgetForGraph` |
| `clearsoftbodydebris` | Exec | in `entity:int` | `ClearSoftBodyDebrisForGraph` |
| `setsoftbodyasleep` | Exec | in `entity:int`, `asleep:bool` | `SetSoftBodyAsleepForGraph` |
| `setsoftbodyeffects` | Exec | in `enabled:bool=true` | `SetSoftBodyEffectsForGraph` |
| `repairallsoftbodies` | Exec | out `repaired:int` | `RepairAllSoftBodiesForGraph` |
| `getsoftbodydents` | Pure | in `entity:int`; out `found:bool`, `dentedEdges:int`, `maxSetCm:float`, `debrisPieces:int` | `GetSoftBodyDentsForGraph` |

`popsoftbodyevent` gives a graph the position and value the named events (`SoftBodyTorn` and the rest) do not carry; it pops from the queue `SoftBodies.DrainEvents` reads, so one consumer should own the queue (the effects bound in `.ocsoft` are dispatched at emit time and are not affected).

**Fields** (new family, new file `GraphFieldsNodeTable.cs`, `FieldsTable()`, owner `SoftFieldGraph`, category `Physics`, keys prefixed `an_` because `pulse`, `impact` and `start` are words a user types as plain verbs):

| Key (palette id) | Kind | Pins (besides exec/then/success) | Wraps |
|---|---|---|---|
| `an_softfieldpulse` (`AN_SoftFieldPulse`) | Exec | in `kind:int`, `shape:int`, `x,y,z`, `ex,ey,ez`, `dirX,dirY,dirZ`, `strength`, `energy`, `falloff=1`, `targets:int=7`; out `reached:int` | `SoftFieldPulseForGraph` |
| `an_softfieldimpact` (`AN_SoftFieldImpact`) | Exec | in `x,y,z`, `dirX,dirY,dirZ`, `radius=40`, `energy=500`, `kick=0`; out `reached:int` | `SoftFieldImpactForGraph` |
| `an_softfieldstart` (`AN_SoftFieldStart`) | Exec | the pulse inputs plus `lifetime=5`; out `handle:int` | `SoftFieldStartForGraph` |
| `an_softfieldstop` (`AN_SoftFieldStop`) | Exec | in `handle:int` | `SoftFieldStopForGraph` |
| `an_spawnsoftfield` (`AN_SpawnSoftField`) | Exec | in `kind`, `shape`, `x,y,z`, `ex,ey,ez`, `strength`, `energy`, `falloff=1`, `lifetime=5`; out `entity:int` | `SpawnSoftFieldForGraph` |
| `an_softfieldsetenabled` (`AN_SoftFieldSetEnabled`) | Exec | in `entity:int`, `enabled:bool=true` | `SoftFieldSetEnabledForGraph` |
| `an_softfieldsetstrength` (`AN_SoftFieldSetStrength`) | Exec | in `entity:int`, `strength`, `energy` | `SoftFieldSetStrengthForGraph` |
| `an_getsoftfieldtouched` (`AN_GetSoftFieldTouched`) | Pure | in `entity:int`; out `found:bool`, `touched:int` | `GetSoftFieldTouchedForGraph` |

The enum values (`kind` 0 to 5, `shape` 0 to 3, `targets` bit mask) are documented in the generated page `docs/nodes/Fields.md`. Node rotation is axis-aligned (a rotated box needs the C# `SoftFieldSpec` or an entity field).

### 2.14 Level records and editor

**Record.** `COMPONENT SoftField kind=radial shape=sphere extent=300,300,300 strength=600 energy=0 falloff=1 dir=0,0,1 turbulence=0 targets=cloth,destructible,debris once=0 off=0`; kind words `radial directional damage release kill sleep`, shape words `sphere box capsule all`; keys at their default are omitted (canonical form, `SoftBodyDetailsModel` convention). Wiring: append `"SoftField"` to `kKnownComponentTypes` (`modules/formats/src/OcWorld.cpp:172`), add `kSoftField` before `kTypeCount` and `applySoftField`/capture cases in `Runtime/src/LevelComponents.cpp` (`:31`, `:397` pattern, shared with the editor), a `kPlacedKinds` row after `Destructible` (`sandbox/src/PlacedComponentModel.hpp:66` pattern; `canAttach` needs a mesh, which the primitive provides) and the `FORMAT_SPECS.md` rows beside `:977`. The runtime damage still never goes in the level; the `.ocsave` channel of 2.6 is the only place it is saved.

**Details.** A `Soft Field` section, built in the new `sandbox/src/SoftFieldSection.cpp` over a pure `sandbox/src/SoftFieldDetailsModel.hpp` (ImGui-free, tested): kind and shape combos, extents (with a "match scale" helper), strength, energy, falloff, direction, turbulence, target checkboxes, Once, Enabled, the OUT readout, and one button **Make invisible volume** (visible off, no collision). `SandboxComponentDetails.cpp` gains one call. The Cloth/Destructible section (`:362-540`) gains subsections **Caps** (mode, thickness, back skin, UV scale, interior material picker, LOD), **Effects** (five slots: particle picker, up to eight sound pickers, thresholds, counts, cooldown), **Proxy** (path, **Create proxy cage**, over-cap warning, `kStateOverCap`/`kStateProxy` readout bits) and **Debris** (bake, bake delay, impact radius scale). All edit the `.ocsoft` through the existing undo snapshot and `Create .ocsoft`.

**Viewport.** `SoftBodyDebugOverlay` (`sandbox/src/SoftBodyDebugOverlay.*`) gains `fields` (the shape wireframe in the kind's colour: radial orange, directional blue with arrows, damage red, release yellow, kill magenta, sleep grey, pieces in reach highlighted) and `tuning` (the **Tuning HUD**, below) toggles under `Show > Soft Bodies`. **Tuning HUD:** for the selected soft body, the last deposit (kind, closing speed, energy J, radius, beams damaged and broken, max strain, max permanent set in cm, tick) from `SoftBodySystem::lastHit(Entity)`, the live piece and baked counts, and the frame's `Stats` (pieces, active particles, rebuilds, draw builds, fx sent and dropped).

### 2.15 Runtime and editor Play wiring

| Piece | Packaged game (`Runtime/`) | Editor Play (`sandbox/`) |
|---|---|---|
| New components and providers | `registerGameSystems` (`GameSystemsWiring.hpp:184`) registers `CSoftField` and installs the softfield provider; called at `GameApp.cpp` startup | the same call (`SandboxInit.cpp`, before `:333`) |
| Effects (sound sink) | `game::installSoftBodyEffects(pathFn, user)` next to `installSoftBodyHost` (`GameApp.cpp:1931`); particles need nothing | next to `SandboxInit.cpp:333` |
| Tick | unchanged (`GameApp.cpp:2195`); fields, caps, fx, bake run inside `tick` | unchanged (`SandboxUpdate.cpp:812`) |
| Draw side | unchanged call (`GameApp.cpp:2211`); the module changed | unchanged (`SandboxUpdate.cpp:843`) |
| Save/load | `GameApp.cpp` `saveHost`/`saveLoadProvider` (chain + skip + clear) | `SandboxApp.hpp` `saveHost`/`saveLoadProvider` |
| Level records | `Runtime/src/LevelComponents.cpp` (shared code, applies in both) | the same code plus the Details model |
| Editor only | | Details sections, field gizmos, Tuning HUD, proxy tool, Add Component row |
| Stop Play | `resetGameSystems()` -> `softBodySystem().clear()` also clears transient fields, queued pulses, the fx queue and any pending restore blobs | same |

### 2.16 D3D12 and Vulkan parity

No shader, no RHI interface change and no backend branch. Every GPU object the feature makes goes through `createMesh`, `createSkinTargetMesh`, `copyBuffer`, `bufferBarrier`, `destroyMesh` and the Upload buffer kind, which exist on both backends (`modules/rhi/include/aver/rhi/RHI.hpp:496-531`). The extra cap entities and baked meshes are ordinary meshes to the scene walk, shadow cascades, voxelisation and the RT geometry table. The Tuning HUD and field gizmos use the existing line renderer. Section 6 repeats the key checks with `--backend vulkan`.

### 2.17 Determinism

Inputs stay: asset bytes, preset, the ordered inbox (now including pulses), entity transforms, palettes, the step count. New derived state follows the same rules: fields run in ascending entity id then handle then arrival order with time from the step count; piece ids, event `seq` and the tear log come from counters advanced in the fixed tick order; the noise is an integer hash; effect requests are queued in emit order and never feed back; sound variants are `hash(seq) % n`; `restoring_` suppresses events and fx during replay. The replay-equality test of section 5 runs the field step, caps and a save/restore round trip.

### 2.18 Cross-feature contracts

**Offered by this feature (exact signatures; VEHICLES references them by name).**

```cpp
// modules/softbody/include/aver/softbody/Fields.hpp and modules/softbody.scene/include/aver/softbody/SoftBodySystem.hpp
u32           softbody::SoftBodySystem::pulseField(const softbody::FieldSpec& f);
u32           softbody::SoftBodySystem::startField(const softbody::FieldSpec& f, f32 lifetimeS);
bool          softbody::SoftBodySystem::stopField(u32 handle);
u32           softbody::SoftBodySystem::impact(Vec3 point, Vec3 dir, f32 radiusCm, f32 energyJ, f32 kickCmS);
scene::Entity softbody::SoftBodySystem::spawnFieldEntity(scene::World& world, const softbody::FieldSpec& f, f32 lifetimeS);
```
```c
// modules/framework/include/aver/framework/framework_softfield_abi.h  (AVER_FW_SOFTFIELD_ABI_VERSION 1)
int32_t aver_fw_softfield_pulse(const float* spec, int32_t n);
int32_t aver_fw_softfield_impact(float x, float y, float z, float dx, float dy, float dz, float radiusCm, float energyJ, float kickCmS);
int32_t aver_fw_softfield_start(const float* spec, int32_t n);
int32_t aver_fw_softfield_stop(int32_t handle);
int32_t aver_fw_softfield_spawn(const float* spec, int32_t n);
```
```csharp
// scripting/csharp/Aver.Framework/SoftFields.cs
public static int SoftFields.Impact(Vec3 point, Vec3 direction, float radiusCm, float energyJoules, float kickCmPerS = 0f);
public static int SoftFields.Pulse(in SoftFieldSpec spec);
```
**The crash-damage recipe for VEHICLES:** per contact with a destructible in range, call `SoftFields.Impact(contactPoint, contactNormalOrRelativeVelocityDirection, radiusCm: 30..80, energyJoules: 0.5 * reducedMassKg * closingMs^2, kickCmPerS: 0)`; the field system owns thresholds (`minImpactCmS` does not apply to a script call; `kDestructIndestructible` does), breaking, the kick and the debris. VEHICLES writes no field, damage or debris code and no particle code.

**Consumed.**

- GPU_PARTICLES: `aver::spawnEffectAt(u64 effectId, const Vec3& position, const Vec3& direction, u32 count = 1)` in `modules/core/include/aver/core/EffectSpawn.hpp` and the id rule `fnv1a64(content-relative path)` (`docs/plans/wave10/GPU_PARTICLES.md` section 2.10). If that document's signature changes, `SoftBodyFx.cpp` has the single call site.
- GAMEPLAY_CAMERAS: nothing (this feature touches no camera code).
- Existing: `aver_audio_load`, `aver_audio_play_at` (`audio_abi.h:50`, `:70`); `aver_scene_material` (`scene_abi.h:183`); `aver::save` `Host.stateCount/stateAt/stateSet` and `CaptureOptions::skip` (`SaveWorld.hpp:61`, `:84`); `trifactor::simplifyMesh` (editor only, `ClusterBuilder.hpp:467`).

### 2.19 Patents and third-party

- **No new third-party code.** Jolt (in tree), meshoptimizer (MIT, vendored at `third_party/meshoptimizer`, reached only through the existing Trifactor wrapper in the editor tool), nothing else.
- **Patent awareness.** (1) Binding a render mesh to a coarse cage can be done with mean-value, harmonic or Green coordinates; the harmonic-coordinates family is patented. This design uses plain closest-point barycentric coordinates and a smooth-normal offset, which is the clean-room alternative and what `ProxyBind` implements; do not substitute a generalised-coordinate scheme without a patent review. (2) Position-based and extended position-based soft-body solving is Jolt's, unchanged. (3) Mesh decimation is quadric-style edge collapse as implemented by meshoptimizer (MIT); no patented decimation scheme is added. (4) The thickness shell (offset duplicate plus wall quads), field volumes, energy-based break rules, tear-log replay and settle-to-static baking are generic techniques with no known patent exposure; fracture-pattern synthesis (Voronoi, "Blast"-style chunk hierarchies) is deliberately not used.

## 3. FILE OWNERSHIP

### 3.1 Created (only this feature touches them)

- `modules/softbody/include/aver/softbody/Fields.hpp`
- `modules/softbody/include/aver/softbody/Caps.hpp`
- `modules/softbody/include/aver/softbody/SoftState.hpp`
- `modules/softbody/include/aver/softbody/ProxyBind.hpp`
- `modules/softbody/src/Fields.cpp`
- `modules/softbody/src/Caps.cpp`
- `modules/softbody/src/SoftState.cpp`
- `modules/softbody/src/ProxyBind.cpp`
- `modules/softbody.scene/src/SoftBodyTear.cpp`
- `modules/softbody.scene/src/SoftBodyFields.cpp`
- `modules/softbody.scene/src/SoftBodyCaps.cpp`
- `modules/softbody.scene/src/SoftBodyFx.cpp`
- `modules/softbody.scene/src/SoftBodySave.cpp`
- `modules/softbody.scene/src/SoftBodyProxy.cpp`
- `modules/framework/include/aver/framework/framework_softfield_abi.h`
- `modules/framework/src/SoftFieldAbi.cpp`
- `scripting/csharp/Aver.Framework/SoftFields.cs`
- `scripting/csharp/Aver.Framework/SoftFieldNative.cs`
- `scripting/csharp/Aver.Graph/GraphFieldsNodeTable.cs`
- `scripting/csharp/Aver.Graph.Tests/FieldsNodeTests.cs`
- `sandbox/src/GraphNodeDefsFields.hpp`
- `sandbox/src/SoftFieldDetailsModel.hpp`
- `sandbox/src/SoftFieldSection.cpp`
- `sandbox/src/SoftProxyTool.hpp`
- `sandbox/src/SoftProxyTool.cpp`
- `Runtime/include/aver/game/SoftBodyEffects.hpp`
- `Runtime/include/aver/game/SoftBodySave.hpp`
- `tests/softbody/src/FieldsTest.cpp`
- `tests/softbody/src/CapsTest.cpp`
- `tests/softbody/src/SoftStateTest.cpp`
- `tests/softbody/src/ProxyBindTest.cpp`
- `tests/softbody.scene/src/SoftBodyDepthTest.cpp`
- `tests/softbody.scene/src/SoftBodyCalibrationTest.cpp`
- `tests/framework_softfield/CMakeLists.txt`
- `tests/framework_softfield/src/FrameworkSoftFieldAbiTest.cpp`
- `tests/editor/src/SoftFieldDetailsModelTest.cpp`
- `test-content/SoftBodyLab/Content/Maps/SoftBodyTune.ocworld`
- `test-content/SoftBodyLab/make_soft_lab_audio.py`
- `test-content/SoftBodyLab/Content/Audio/Thud.wav`
- `test-content/SoftBodyLab/Content/Audio/WoodCrack.wav`
- `test-content/SoftBodyLab/Content/Audio/GlassBreak.wav`
- `test-content/SoftBodyLab/Content/Audio/MetalClang.wav`

Files to recycle (never delete): none. The new files replace nothing; the rewritten sections of the owned docs keep their files.

### 3.2 Modified, owned by this feature

- `modules/softbody/CMakeLists.txt`
- `modules/softbody/README.md`
- `modules/softbody/include/aver/softbody/Material.hpp`
- `modules/softbody/include/aver/softbody/SoftAsset.hpp`
- `modules/softbody/include/aver/softbody/Damage.hpp`
- `modules/softbody/include/aver/softbody/RenderBinding.hpp`
- `modules/softbody/src/SoftAsset.cpp`
- `modules/softbody/src/Damage.cpp`
- `modules/softbody/src/RenderBinding.cpp`
- `modules/softbody.scene/CMakeLists.txt`
- `modules/softbody.scene/README.md`
- `modules/softbody.scene/include/aver/softbody/SoftBodySystem.hpp`
- `modules/softbody.scene/src/SoftBodySystem.cpp`
- `modules/softbody.scene/src/SoftBodyBuild.cpp`
- `modules/softbody.scene/src/SoftBodyDamage.cpp`
- `modules/softbody.scene/src/SoftBodyCloth.cpp`
- `modules/softbody.scene/src/SoftBodyProvider.cpp`
- `modules/render.softbody/include/aver/render/SoftBodyScene.hpp`
- `modules/render.softbody/src/SoftBodyScene.cpp`
- `modules/physics/include/aver/physics/physics_softbody_abi.h`
- `modules/physics/src/PhysicsSoftBody.cpp`
- `modules/framework/include/aver/framework/framework_softbody_abi.h`
- `modules/framework/src/SoftBodyAbi.cpp`
- `scripting/csharp/Aver.Framework/SoftBodies.cs`
- `scripting/csharp/Aver.Framework/SoftBodyNative.cs`
- `scripting/csharp/Aver.Physics/Native.SoftBody.cs`
- `scripting/csharp/Aver.Physics/SoftBody.cs`
- `scripting/csharp/Aver.Graph/GraphSoftBodyNodeTable.cs`
- `scripting/csharp/Aver.Graph.Tests/SoftBodyNodeTests.cs`
- `sandbox/src/GraphNodeDefsSoftBody.hpp`
- `sandbox/src/SoftBodyDetailsModel.hpp`
- `sandbox/src/SoftBodyDebugOverlay.hpp`
- `sandbox/src/SoftBodyDebugOverlay.cpp`
- `tests/softbody/CMakeLists.txt`
- `tests/softbody/src/SoftAssetTest.cpp`
- `tests/softbody/src/DamageTest.cpp`
- `tests/softbody.scene/CMakeLists.txt`
- `tests/softbody.scene/src/SoftBodyFixtures.hpp`
- `tests/softbody.scene/src/SoftBodySystemTest.cpp`
- `tests/render.softbody/src/SoftBodySceneTest.cpp`
- `tests/physics/src/SoftBodyBuilderTest.cpp`
- `test-content/SoftBodyLab/README.md`
- `test-content/SoftBodyLab/make_soft_lab.py`
- `test-content/SoftBodyLab/Content/Soft/GlassPane.ocsoft`
- `test-content/SoftBodyLab/Content/Soft/SteelDoor.ocsoft`
- `test-content/SoftBodyLab/Content/Soft/PlankCrate.ocsoft`
- `test-content/SoftBodyLab/Content/Maps/SoftBodyLab.ocworld`
- `docs/SOFTBODY.md`
- `docs/physics/SOFTBODY_DESTRUCTION.md`

### 3.3 SHARED (another wave-10 feature or the integrator also touches these; every edit is small, additive, append-only)

- `CMakeLists.txt`
- `CHANGELOG.md`
- `docs/ABI.md`
- `docs/formats/FORMAT_SPECS.md`
- `docs/SCRIPTING_API.md`
- `docs/nodes/SoftBody.md`
- `docs/nodes/Fields.md`
- `docs/api/Aver.Framework.md`
- `modules/framework/CMakeLists.txt`
- `modules/formats/src/OcWorld.cpp`
- `Runtime/include/aver/game/GameSystemsWiring.hpp`
- `Runtime/include/aver/game/LevelComponents.hpp`
- `Runtime/src/LevelComponents.cpp`
- `Runtime/src/GameApp.cpp`
- `sandbox/CMakeLists.txt`
- `sandbox/src/SandboxApp.hpp`
- `sandbox/src/SandboxInit.cpp`
- `sandbox/src/SandboxComponentDetails.cpp`
- `sandbox/src/PlacedComponentModel.hpp`
- `sandbox/src/SandboxPanels.cpp`
- `sandbox/src/SandboxShell.cpp`
- `tests/abi/src/AbiEnumTest.cpp`
- `tests/editor/CMakeLists.txt`

What each shared file receives:

| File | Edit |
|---|---|
| `CMakeLists.txt` | `add_subdirectory(tests/framework_softfield)` inside the `AVER_MODULE_FRAMEWORK` test block (beside `tests/framework_morph`); nothing for modules (no new module) |
| `CHANGELOG.md` | one entry under `[Unreleased]`, "Animation" or a new "Physics" heading |
| `docs/ABI.md` | the `framework_softfield_abi.h` paragraph beside `:1384`, the version note for `framework_softbody_abi.h` (2) and `physics_softbody_abi.h` (2, `aver_phys_sb_add_velocity`) at `:1382` |
| `docs/formats/FORMAT_SPECS.md` | `.ocsoft` row (`:28`) gains caps, effects, proxy; `COMPONENT SoftField` rows beside `:977`; the `$State` channel `softbody` payload; the "runtime damage is never saved" sentences at `:977-978` are changed to point at the channel |
| `docs/SCRIPTING_API.md` | the `SoftFields` section and the new `Entity`/`SoftBodies` members |
| `docs/nodes/SoftBody.md`, `docs/nodes/Fields.md`, `docs/api/Aver.Framework.md` | generated by `scripts/node-catalog.py --write` and `scripts/csharp-api-index.py --write` at integration; no hand edits |
| `modules/framework/CMakeLists.txt` | `src/SoftFieldAbi.cpp` in the source list |
| `modules/formats/src/OcWorld.cpp` | `"SoftField"` appended to `kKnownComponentTypes` (`:172`) |
| `Runtime/include/aver/game/GameSystemsWiring.hpp` | softfield provider install in `registerGameSystems` (`:184`); `installSoftBodyEffects` declaration use; include `SoftBodyEffects.hpp` and `SoftBodySave.hpp` |
| `Runtime/include/aver/game/LevelComponents.hpp`, `Runtime/src/LevelComponents.cpp` | `kSoftField` enum value, `applySoftField` and capture case |
| `Runtime/src/GameApp.cpp` | `installSoftBodyEffects` beside `:1931`; `saveHost` chain, skip and `clear()` before restore (`:201-250`) |
| `sandbox/CMakeLists.txt` | `SoftFieldSection.cpp`, `SoftProxyTool.cpp` in the sources (beside `:31-32`) |
| `sandbox/src/SandboxApp.hpp` | the same `saveHost` chain (`:431-480`) |
| `sandbox/src/SandboxInit.cpp` | `installSoftBodyEffects` beside `:333` |
| `sandbox/src/SandboxComponentDetails.cpp` | one call into `SoftFieldSection.cpp`; the Caps, Effects, Proxy and Debris subsections inside the existing soft-body section (`:362-540`) |
| `sandbox/src/PlacedComponentModel.hpp` | `SoftField` row in `kPlacedKinds` |
| `sandbox/src/SandboxPanels.cpp`, `SandboxShell.cpp` | the `Show > Soft Bodies` toggles `fields` and `tuning` |
| `tests/abi/src/AbiEnumTest.cpp` | `framework_softfield_abi.h` in `kAbis`, `AVER_FW_SF_*`, `AVER_FW_SB_OP_*` 11 to 15, `AVER_PHYS_SB_ABI_VERSION`, `AVER_FW_SOFTBODY_ABI_VERSION` |
| `tests/editor/CMakeLists.txt` | `SoftFieldDetailsModelTest` target beside `:230` |

## 4. Phased implementation

Each phase leaves the tree buildable and `ctest` plus the `Aver.Graph.Tests` runner green. Sizes: A M, B S, C M, D M, E M, F S, G M, H L, I L, J M. Overall **XL**. New simulation code goes into new translation units (`SoftBodyTear/Fields/Caps/Fx/Save/Proxy.cpp`); `SoftBodySystem.cpp` (766 lines), `SoftBodyDamage.cpp` (493) and the sandbox files near the COFF cap do not grow beyond small hooks.

**Phase A, pure library (M).** `Fields`, `Caps`, `SoftState`, `ProxyBind` headers and sources; `depositWeighted` (with `depositEnergy` as its sphere case); `.ocsoft` sections `caps`, `effects`, `proxy` and `destruct.impactRadiusScale`/`bake` (parse, write, `valid()`, `makeSoftAsset` defaults, `PresetInfo::capThicknessCm`). Exit: `tests/softbody` green with `FieldsTest`, `CapsTest`, `SoftStateTest`, `ProxyBindTest` and the extended `SoftAssetTest`/`DamageTest`; the six lab assets round-trip byte-identical.

**Phase B, physics ABI v2 (S).** `aver_phys_sb_add_velocity`, version macro and function, C# natives, `AbiEnumTest` rows. Exit: the new `SoftBodyBuilderTest` case and the existing 1088-line `SoftBodyTest` unchanged and green.

**Phase C, system core (M).** Split `tearPipeline` into `planTear`/`commitTear` (`SoftBodyTear.cpp`, behaviour identical; the tear log appended); reveal handshake, `maxDrawBuildsPerFrame`, `Options` additions; `lastHit`/`Stats`; the field step, `CSoftField`, `pulseField/startField/stopField/impact/spawnFieldEntity`, the `ReleaseAnchors/Kill/Sleep` operations; the inbox item `Pulse`. Exit: `SoftBodySystemTest` (all existing cases) and the new `SoftBodyDepthTest` field cases green.

**Phase D, caps (M).** `fillPieceDraw`/`refreshDraw` integration, cap entities, `PieceView` additions, `SoftBodyScene` UV override and reveal; cap LOD. Exit: cap cases in `SoftBodyDepthTest`, `SoftBodySceneTest` pack cases; headed steps 1 to 3.

**Phase E, effects (M).** `SoftBodyFx.cpp`, slots, budgets, `SoftBodyEffects.hpp`, host installs. Exit: fx cases green; headed step 5 once GPU_PARTICLES' CPU tier has landed (until then particles return 0 and sounds still play).

**Phase F, debris cost (S).** Baking, cadence, sub-pixel cull, `SoftBodyScene` static build and `softBodyBakeVertices`. Exit: bake cases green, headed step 8.

**Phase G, save and load (M).** `SoftBodySave.cpp`, `SoftBodySave.hpp`, host chain, skip, `clear()` before restore, `restoring_`. Exit: save cases green including the tear-log replay equality; headed step 6.

**Phase H, framework, C# and nodes (L).** `framework_softfield_abi.h`/`SoftFieldAbi.cpp`/provider, `framework_softbody_abi.h` v2 ops, `SoftFields.cs`, `SoftBodies.cs` additions, interop, both node tables, both palette mirrors, `FieldsNodeTests`, extended `SoftBodyNodeTests`, `FrameworkSoftFieldAbiTest`, `AbiEnumTest`. Exit: `Aver.Graph.Tests` green, `scripts/node-catalog.py --check` green, a sample graph fires an impact field.

**Phase I, proxy cage (L).** `SoftBodyProxy.cpp`, `buildEntry` proxy path, draw-vertex split rule, `SoftProxyTool`, `kStateProxy/kStateOverCap`. Exit: proxy cases green; headed step 7. The riskiest phase: if it slips the feature ships without it and the over-cap refusal stays.

**Phase J, editor, content, tuning, docs (M).** Details sections and model, record `SoftField`, Add Component row, gizmos, Tuning HUD, `SoftBodyTune` map, generated audio, `SoftBodyCalibrationTest`, rewrite `docs/SOFTBODY.md` and `docs/physics/SOFTBODY_DESTRUCTION.md`, regenerate the catalog docs, `module-guard-audit.py` and `docs-check.py` clean. Exit: `SoftFieldDetailsModelTest` green; headed steps 4, 9 to 11 and the tuning pass.

## 5. Tests

Everything runs without a GPU or a device. Executables are picked up by the root `*Test` walk; each phase adds an `add_executable` in its test directory. C# suites are Windows-only (`tests/managed/CMakeLists.txt:14-16`).

**`tests/softbody` (links `Aver.SoftBody Aver.Formats Aver.Core Aver.Platform`)**

- `FieldsTest`: weight is 1 at the centre line and 0 outside for sphere, box, capsule and everywhere; monotone in falloff; a rotated and non-uniformly scaled box (`Mat4`) matches hand-computed points; radial pushes away (negative pulls); directional follows the entity rotation; turbulence is 0 at 0 and identical bits twice at the same step; `perSecond` scales with `seconds`; `fieldBeamWeights` skips broken beams; `anchorsInField` returns exactly the pinned particles inside; `fieldOverlapsBounds` agrees with brute force.
- `DamageTest` (extended): `depositWeighted` over sphere weights is bit identical to `depositEnergy` on the old fixtures; toughness, kick and the break rule unchanged.
- `CapsTest`: a flat 2-triangle strip has 4 boundary edges and 4 wall quads (16 vertices, 24 indices); the wall normal faces away from the triangle; a closed cube has none; breaking one beam of a cube (after `resolveTears`) gives walls on **both** sides of the crack only; a UV seam does not create a wall; wall `u` spans edge length over `uvCmPerTile` plus the hash fraction, `v` spans thickness over `uvCmPerTile`; the back skin reverses winding and keeps the UV; zero thickness or `Off` gives an empty geometry; `vertexLimit` sets `overLimit`; two builds are bit identical; `smoothNormals` is unit length and seam-continuous; `capPositions` offsets along `-normal` by exactly `depth * thickness`.
- `SoftStateTest`: encode, decode, encode is byte identical; truncated, corrupt, wrong-magic and oversize payloads are rejected without reading past the buffer; a stale `meshKey` or `assetKey` is reported; 4096 particles encode under 150 KB of base64; the 16-bit fallback keeps positions within 0.05 cm; **tear-log replay equality**: tearing a grid cage in three incremental `resolveTears` rounds and replaying the logged groups on a fresh cage give identical particles, beam endpoints, `triDead`, split log and `planIslands` output.
- `ProxyBindTest`: a flat 30 x 30 render sheet bound to a 6 x 6 proxy reproduces every rest vertex within 0.01 cm; a bump (curved proxy) reproduces within the simplification error and offsets are signed correctly; moving proxy particles moves vertices linearly; evaluating a vertex against an adjacent triangle extrapolates continuously on the shared edge; a vertex beyond `maxDistanceCm` fails with its id; ties pick the lower triangle id; the bind is bit identical twice.
- `SoftAssetTest` (extended): the new sections round-trip; a minimal v1 asset and the six lab assets write byte-identically to their input; unknown keys inside the new sections are ignored; invalid values (negative thickness, nine sounds, a non-`.ocmesh` proxy) are refused with a reason; defaults resolve per preset.

**`tests/physics` (`SoftBodyBuilderTest`, extended)**: `aver_phys_sb_add_velocity` changes only the listed vertices, skips kinematic ones, refuses an out-of-range index, wakes a sleeping body only for a non-zero delta, and the velocity is read back by `aver_phys_sb_state`; `aver_phys_sb_abi_version()` returns 2. The three Jolt behaviours of 1.3 stay pinned by the existing cases (`testSleepAndFreeze` `:494-495`, `testLayers` `:563`, `testImpacts` `:511`); one new assertion pins the closing-speed bias at or below `g * dt` plus 15% for a sphere dropped on a sheet.

**`tests/softbody.scene` (`SoftBodyDepthTest`, links as `SoftBodySystemTest`; meshes, assets and palettes injected)**

- Fields: a radial pulse moves cloth vertices and a negative strength pulls; a continuous box field accelerates only debris inside it and `targets` excludes the rest; a Damage field breaks a glass pane when its energy passes toughness and respects `kDestructIndestructible`; ReleaseAnchors makes a hinge-pinned door fall while a cloth keeps its other pins; Kill shrinks and destroys debris and leaves roots alone unless targeted; Sleep holds pieces inactive and a disabled field releases them; `kFieldOnce` fires one tick; a transient field expires at its lifetime; a `CSoftField` entity moved by its parent moves the field; `outTouched` is right; **replay equality** with fields, caps and a save round trip: two fresh systems run the same ordered script for 600 ticks and agree bit for bit on positions, events and piece ids.
- Reveal and budgets: debris and cap entities are created without the visible bit and the test host's mimic of the scene sets it; with a test budget of 2 resident builds, 16 pieces are all revealed within 8 builds-per-frame accounting; no debris entity is visible without a resident.
- Caps: a glass pane (a sheet) publishes front, back skin and walls with `uv` non-null for generated vertices, all indices valid, no NaN normals; a crate (closed) publishes back skin for debris only under `auto`; an interior material creates one cap entity per piece with the right `CMeshRenderer::material` token and a second `PieceView` (`layer 1`); beyond `caps.lodCm` the cap arrays are empty and the epoch changes once with hysteresis; over `vertexLimit` the piece draws uncapped.
- Effects: a sink records requests for impact, tear, detach and dust with the expected counts and directions; cooldown, `fxMaxPerTick`, `fxSoundsPerTick`, the distance cull and the master switch each cut requests; no binding means no request; request order is the emit order; `aver::spawnEffectAt` is called with the right id and returns 0 harmlessly with no spawner.
- Save: dent a metal panel and shatter a pane, `saveState`, build a second world and system, `restoreState`, tick: broken flags, plastic set and damage equal, positions within 1e-3 cm, piece count equal, debris entities recreated, despawned pieces not recreated, no events or effects fired during replay, a changed `meshKey` or `assetKey` loads intact with a warning, a pristine entity writes no channel, tear-log merging above 64 groups keeps the cage valid.
- Bake: a debris piece asleep for `bakeAfterS` loses its Jolt body (`aver_phys_sb_count` drops), publishes `bake = true` and an updated origin, still despawns (entity scale shrinks), is skipped by damage and fields, and baking can be switched off per asset and globally.
- Proxy: a 100 x 100 plate (10k welded particles, over the cap) with a proxy runs where it used to fail; a hit dents the proxy and the draw vertices follow; a tear never produces a render triangle whose corners are evaluated against triangles on opposite sides of the crack; the entry reports `kStateProxy`; a plate with no proxy still fails as before with `kStateOverCap`.
- `SoftBodyCalibrationTest`: runs the section-6.2 scenarios headless (a fixed projectile energy ladder against a steel door, a glass pane, a plank crate and a cloth flag) and **prints** the measured table (dent depth, broken beams, piece count, max strain); it asserts only coarse brackets (for example metal: 90 J leaves a dent between 0.5 and 8 cm and no tear; glass: 1 J does nothing and 90 J makes at least 4 pieces) so a later tuning pass cannot silently move a preset out of its intended class.

**`tests/render.softbody`**: `softBodyPackPieces` with caps (generated vertices keep their UV, back-skin normals point the other way, walls get flat normals, no NaN); `softBodyBakeVertices` produces vertices relative to the new origin and is deterministic; existing pack cases unchanged.

**`tests/framework_softfield`**: version 1; every call returns 0 with no provider; a stub provider receives the right op, entity and spec array (all 24 floats); non-finite values and out-of-range kind or shape are refused before the provider; `spawn` and `start` return the provider's value.

**`tests/editor` (`SoftFieldDetailsModelTest`)**: record round trip and canonical form, kind and shape words, `extent`/`dir` parsing, target list, `canAttach` rules, the make-invisible helper's flag changes, Tuning HUD text for a fixed `HitInfo`.

**`tests/abi` (`AbiEnumTest`)**: `framework_softfield_abi.h` against `SoftFieldNative.cs`; `AVER_FW_SF_*`, `AVER_FW_SB_OP_*` 11 to 15, `AVER_PHYS_SB_ABI_VERSION`, `AVER_FW_SOFTBODY_ABI_VERSION`.

**C#**: `FieldsNodeTests` (`[AutoRun]`, `NodeRowKit.CheckRows/CheckDefaultPins/CheckIlValid/CheckCompilerSplit`, exec rows refused by the pure compiler, `an_getsoftfieldtouched` as a pure read, the row set equals the designed 8, a chain Spawn -> SetStrength -> Pulse -> Stop compiles); `SoftBodyNodeTests` extended from 8 to 15 rows (`TestRowSetIsTheDesignSet` updated, one pure read `getsoftbodystate` plus `getsoftbodydents`); `NodeCatalogCheck` and `CsharpApiIndexCheck` stay green.

## 6. Headed manual checks for the owner

Run on a quiet GPU. Editor first, then the packaged runtime, then `--backend vulkan`. Screenshots at 1440p with a ping for remote runs. Use `test-content/SoftBodyLab` (import the meshes as its README says; `make_soft_lab.py` gains a 12k-triangle `HighPolyDoor` for step 7) and the new `SoftBodyTune` map.

### 6.1 Checks

1. **Thickness.** Shatter the GlassPane. Look at a shard edge-on: it has a visible edge of about 0.6 cm; look from behind: it is still there. Look at a break edge close up: the interior material (tinted glass edge) shows on the wall, the UVs are not stretched, neighbouring walls do not repeat as stripes, no z-fighting where the wall meets the face. Toggle `backSkin` off and on in Details to see the difference.
2. **Steel door and crate.** Dent the door, then tear it at a corner: the tear rim has thickness and an edge material. Drop the crate: planks have thickness; set `backSkin` to `all` and the hollow crate with a hole reads as a solid 2 cm wall.
3. **Cap LOD.** Walk away from a shattered pane past 2500 cm: caps disappear without a visible pop; return: they come back.
4. **Fields.** Place an invisible Cube and add `Soft Field`. Radial: debris and a curtain are pushed away, negative strength pulls; directional: a gust across the flag and across settled debris; damage: a damage volume over the glass breaks it at its rate; release: a volume over the door hinge makes the door fall off, over the cape shoulders drops the cape; kill: debris entering the volume shrinks away; sleep: cloth inside stops moving and resumes when the volume is disabled. Gizmo colours and the pieces-in-reach highlight match the kinds. Move the volume with the gizmo during Play and with a parent that moves.
5. **Effects.** With the lab effect assets (GPU_PARTICLES must be in), a thrown crate on the door gives a dust puff and a clang; the pane gives a shard burst and a break sound; the crate gives splinters; tiny fragments become dust bursts. Ten panes at once stay under the per-tick caps (watch the HUD `fx dropped`); far panes (beyond 6000 cm) are silent.
6. **Saved damage.** Dent the door, shatter a pane, let some debris settle and let some despawn, then Save Game (the existing node) and quit. Load: the dent and the tears are there, the settled shards are where they were, the despawned ones are not, nothing plays an effect during the load. Load again after changing the pane mesh: a warning and an intact pane.
7. **Proxy.** Select `HighPolyDoor`: Details warns it is over the particle cap; **Create proxy cage**; press Play. It simulates, dents with the render mesh following at full detail, tears along render-triangle slack, no triangle bridges a crack.
8. **Debris cost.** Shatter ten panes in one frame: no whole-pane ghosts on any frame (frame step with the pause chord), pieces appear within two frames, the HUD shows `draw builds <= 8`. After 3 s asleep, pieces report `baked` and the Soft Body profiler row and the GPU memory number drop; despawn still shrinks them. Repair restores everything.
9. **Vehicles hook.** A graph on a key press calls `AN_SoftFieldImpact` at the aim point: same result as a thrown crate of matching energy.
10. **Backends and hosts.** Repeat 1, 4, 5, 6 and 8 on `--backend vulkan` and in the packaged `AverEngineRuntime.exe`.
11. **Budgets.** 30 cloth flags plus 10 glass panes plus 6 fields active: the `SoftBody` profiler row stays under 2 ms CPU at default budgets; 10 panes shattering in one frame cause no hitch over 8 ms.

### 6.2 Preset tuning procedure

Every preset number is an UNVERIFIED starting value. The owner runs this once per preset in `SoftBodyTune`; the implementer folds the agreed numbers into the `presetInfo` table (`Material.hpp:80`) and tightens the `SoftBodyCalibrationTest` brackets.

**The rig.** `SoftBodyTune` has one row per preset, each with the lab target at the same size (door, pane, crate, flag), a **Hit** graph that fires `SoftBodyDamage` at the aim point at keys 1 to 5 with an energy ladder of 5, 25, 90, 400 and 3000 J, a 5 kg crate on a kinematic lift that drops from 50, 100, 200 and 400 cm (`0.5 m v^2` of about 25, 49, 98 and 196 J: use it to check that real impacts agree with the keys), and the **Tuning HUD** on (`Show > Soft Bodies > Tuning`) to read the last deposit. Change values in the Details sliders (they write the `.ocsoft`); press Repair between hits.

**Order.** (1) how it moves, (2) when it dents or breaks, (3) how it breaks apart, (4) how long it lasts, (5) how it looks and sounds. Do not tune a later step before the earlier one reads right.

| Preset | What to look at | Target (starting intent) | If it is wrong | Number and direction |
|---|---|---|---|---|
| `cloth_light`, `cloth_heavy` | Hang, then wind (300 cm/s with gust 120) | Settles in under 2 s, ripples at 1 to 2 Hz, no jitter, stretch under 3% | Rubbery or stretchy | `compliance` down (0 is inextensible), `iterations` up by 2 |
| | | | Too stiff, board-like | `bendCompliance` up (0.05 light, 0.01 heavy), step x2 |
| | | | Never calms down | `damping` up (0.3 light, 0.5 heavy), step 0.1 |
| | | | Flutters too little or too much in wind | `dragCoeff`, `liftCoeff` (1.0 and 0.3), step 0.2 |
| | | | Sags through its pins | `defaultReachCm` down, or pin more particles (paint tool) |
| `metal` (door) | Keys 1 to 5, then the crate drops | 5 and 25 J: nothing visible; 90 J (crate from 200 cm): a 3 to 6 cm dent that stays; 400 J: 10 to 15 cm; 3000 J: deep dent and a tear at the rim; never falls apart below 1500 J | Dents too little | `yieldStrain` down (0.010), step x0.7 |
| | | | Dents too easily | `yieldStrain` up, step x1.4 |
| | | | Dent springs back | `maxSetFrac` is too low or `hardening` too high; check `maxSetCm` on the HUD |
| | | | Tears too early | `toughnessJ` up (40 per edge), step x1.5, or `maxSetFrac` up (0.25) |
| | | | Shoved around too much by a hit | `destruct.impulseEfficiency` (0.6) and `maxKickCmS` (1200) down |
| | | | Light bumps register | `destruct.minImpactCmS` up (200 = 2 m/s; remember the up-to-16 cm/s bias, 1.3 c) |
| `glass` (pane) | Same ladder | 5 J: nothing; 25 J: a star crack and 4 to 10 pieces; 90 J: 12 to 30 pieces; 400 J: full shatter and fine dust | Shatters from a touch | `yieldStrain` up (0.006), step x1.5; `minImpactCmS` up |
| | | | Too few, too large pieces | `toughnessJ` down (0.4 per edge), step x0.6; `impactRadiusScale` up |
| | | | Too much dust, no shards | `minPieceTriangles` down (3), `maxPieces` up (24) |
| | | | Pieces fly too far | `maxKickCmS` down |
| `wood` (crate) | Drop from 50, 100, 200, 400 cm | 50 cm: untouched; 100 cm: a crack but whole; 200 and 400 cm: breaks into 6 to 12 planks | Breaks from the first drop | `yieldStrain` and `breakStrain` up together (0.02), step x1.4 |
| | | | Never breaks | the same down; or `toughnessJ` down (3) |
| | | | Planks too small | `minPieceTriangles` up |

**Then step 4 and 5.** `debrisLifetimeS` (12), `Options::debrisSleepS` (2) and `bakeAfterS` (3): debris should rest visibly for a while and then leave without a pop. Caps: `thicknessCm` per preset (glass 0.6, metal 0.5, wood 2.0) from the side view; `uvCmPerTile` so a fracture face reads at the material's scale; effect `count` and `countPerMs` so a hard hit looks heavier than a soft one; `cooldownS` so a rolling shard does not machine-gun a sound.

**Rules of thumb for the owner.** Change one number at a time and one step at a time. Keep the HUD's `energy`, `broken` and `max set` readings with the screenshot, so a result can be reproduced headless. When a preset feels right, report the numbers; do not edit `Material.hpp` yourself.

## 7. Open questions (defaults chosen; proceed unless told otherwise)

1. **Back skin on the root of a closed mesh.** Default `auto`: all pieces for an open sheet, debris only for a closed mesh (the crate's hole shows a hollow interior behind the rim). `all` doubles the vertex count of the root; the owner can set it per asset.
2. **Field volumes as invisible Cube/Sphere placements** with a `COMPONENT SoftField`, not a new level record or Outliner entry (the Post Process Volume route, `sandbox/src/SandboxApp.hpp:3171`, costs a header record, parser, Outliner and gizmo plumbing). Default: the component route.
3. **Baked debris is one-way and non-interactive** (no collision, no damage). Default on for debris only; roots and cloth never bake; `destruct.bake=false` or `Options::bakeEnabled=false` turns it off.
4. **No runtime auto-proxy.** A mesh over the cap without a cooked proxy keeps today's refusal and log plus a Details warning with the **Create proxy cage** button. Default: cooked proxies only (deterministic, reviewed, no meshoptimizer in the soft-body runtime).
5. **Particle contract.** This document assumes GPU_PARTICLES' `aver::spawnEffectAt(u64, const Vec3&, const Vec3&, u32)` and `fnv1a64(path)` ids exactly as written in its section 2.10. If it changes, only `SoftBodyFx.cpp` changes.
6. **Sound path resolution** reuses the system's `PathFn` (the content index, `fnv1a64(relative path)`), so sounds must be project assets; no OS paths. Default: as stated.
7. **Tear-log replay cap.** More than 64 logged tear groups are merged on load (same final cage, possibly different duplicate numbering, which is invisible). Default 64.
8. **Closing-speed bias** (1.3 c) is documented and tuned around, not corrected; a correction would need the other body's pre-step velocity cached for every tracked pair. Default: leave.
9. **`.ocsoft` version stays 1** because all additions are optional sections an older reader ignores. If a future change needs a breaking field, bump then.
10. **Kill fields do not remove roots unless the Destructible target bit is set**, and a killed root is hidden, not destroyed, so `Repair` restores it. Default: as stated.
11. **Self-collision** remains out (Jolt has none, 1.3 b). Unchanged from wave 9.
