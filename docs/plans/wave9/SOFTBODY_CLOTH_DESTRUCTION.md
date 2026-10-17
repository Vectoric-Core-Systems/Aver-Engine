# Feature: Cloth and destruction on soft-body physics (dents, tearing, detachment)

Design contract for the implementing agent. Owner approved 2026-10-17 with the instruction "for destruction and cloth use soft body physics". This **supersedes the Voronoi pre-fracture reading of parity row #28** (`docs/plans/UE_FEATURE_PARITY_PLAN.md:46`) for destruction: pieces here are soft bodies, not rigid fragments. Nothing here is built or run. Format and depth follow `docs/plans/wave5/MOTION_MATCHING.md`.

Rules in force: upgrade the existing systems (no parallel one), module DAG holds (`docs/MODULES.md`), every runtime capability gets Aver Node nodes and C# bindings, no third-party code, lean comments, D3D12 and Vulkan parity, no-GPU ctest where possible.

`file:line` cites were read in this pass against the tree at `fa7f3489`. **UNVERIFIED** marks anything reasoned from reading but not confirmed by reading the exact code path or running it.

## 0. Decisions in one screen

1. **Jolt owns the physics for both features.** Cloth and destructibles are Jolt soft bodies (`SoftBodySharedSettings` with edge, dihedral-bend and `Skinned` constraints, Jolt 5.6 at `modules/physics.jolt`). The Verlet/PBD solver in `Aver.SoftBody` (`Solver.cpp`, `AsyncSolver`, `aver_sb_*`, the Window > Soft Body panel) is **retired in the last phase**; two soft-body solvers would be a parallel system. The rest of `Aver.SoftBody` (cage topology, material yield model, tear resolution into duplicate particles, render binding that follows tears) is kept and becomes the pure **topology + damage + asset library that drives Jolt**.
2. **One runtime owner.** A new module `Aver.SoftBody.Scene` (`modules/softbody.scene`, class `SoftBodySystem`) owns every Jolt soft body that belongs to an entity. `SoftBodyScene` (`modules/render.softbody`) stops creating bodies and only draws what the system publishes. `CSoftBody` (id 13, 16 bytes) is unchanged and stays the marker and the legacy jiggle/elastic path; cloth and destructibles are two **runtime-registered** components, `CCloth` and `CDestructible` (the `CMotionMatcher` pattern: no `Components.hpp` growth, no scene-ABI change).
3. **Dent = in-place write, tear = rebuild.** Jolt reads edge `mRestLength` and dihedral `mInitialAngle` from the shared settings every iteration (`SoftBodyMotionProperties.cpp:594-597` edges, `:417` dihedral), so plastic deformation is a between-steps write on settings the physics module owns. Removing edges, duplicating vertices and splitting islands changes the vertex and constraint arrays, which Jolt cannot do on a live body, so tears **rebuild the body with the same handle and carry positions and velocities across**; island splits create extra bodies the same way. The handle-stable rebuild has precedent in `aver_phys_body_set_motion_type` (`PhysicsBody.cpp:165-205`).
4. **Render follows topology by rebuilding the draw mesh at a topology epoch.** Sim particles are the **weld by position** of the render vertices; a `RenderBinding` maps many render vertices to one particle (`RenderBinding.hpp:15-45`, many-to-one already supported). A tear appends duplicate render vertices (`applySplits`) and drops dead triangles (`visibleIndices`); `SoftBodyScene` then makes a fresh source mesh with `createMesh` plus `createSkinTargetMesh` and retires the old one after the frames-in-flight delay. Between epochs only vertex positions change (today's ring and copy). Detached islands become **new scene entities** so they get shadows, GI, ray tracing and culling for free.
5. **Collision.** World and rigid bodies: Jolt soft-vs-rigid. Characters: Jolt's `CharacterVirtual` never reaches a soft body's collision query (`physics_abi.h:526-536`), so a skeleton's collision proxy is its **Kinematic ragdoll** (`RagdollMode::Kinematic`, `docs/physics/RAGDOLL.md`), put on a dedicated layer pair. **Self-collision and soft-vs-soft collision do not exist in Jolt 5.6** (checked: no such setting anywhere under `Jolt/Physics/SoftBody`); v1 ships without them and says so.
6. **Authoring** is a `.ocsoft` JSON asset (kind cloth or destructible) plus five presets (Cloth light, Cloth heavy, Metal, Wood, Glass-like brittle). Pins and reach come from a painted per-particle array, from rules (height, bone weight, sphere), or from a mask texture sampled at the vertex UV. **Vertex colour is not available**: `.ocmesh` has no colour stream and the glTF importer drops `COLOR_0` (`GltfImport.cpp:331`). Components work from a preset alone with no asset.
7. **New ABIs are additive and in new files.** `physics_softbody_abi.h` (`aver_phys_sb_*`, builder style, in the shared `Aver.Physics` DLL) and `framework_softbody_abi.h` (`aver_fw_softbody_*`, one provider function, the `framework_anim_abi.h` pattern). No edit to `scene_abi.h`, `Components.hpp` ids, `framework_abi.h`.
8. **Events and determinism.** Inputs are queued and applied at the top of one tick in a fixed order; outputs are a polled event list (torn edge, detached piece, dust, repaired, impact) plus a notify sink that fires named graph events. State is a pure function of (assets, seed, ordered event list, tick count) on one binary; cross-platform determinism is not claimed (Jolt is built with `CROSS_PLATFORM_DETERMINISTIC` off).
9. **`modules/fracture` is deleted** (skeleton only, never wired). Its README promised "farthest-point cluster split" rigid debris; that is replaced by this feature. `modules/gpudeform` and `modules/deform` are untouched.
10. **Budgets are explicit and default-on** (section 2.9): live pieces, active particles, rebuilds per frame, debris lifetime, distance freeze.

## 1. What exists today

| Fact | Where |
|---|---|
| `modules/fracture` is a README plus `module.json` with `"skeleton": true`; not in the root `CMakeLists.txt`. Also named in `docs/ARCHITECTURE.md:85,651` and `docs/physics-net/PHYSICS_NET.md:3` | `modules/fracture/README.md:1-12`, `modules/fracture/module.json` |
| Jolt soft bodies are exposed as `aver_phys_softbody_create` (fixed triangle shell, edge constraints from faces, `mBendCompliance = FLT_MAX` so no bend), `_create_skinned`, `_skin`, `_vertex_count`, `_vertices`, `_apply_impulse`. Header says elastic only, no plastic or tear | `physics_abi.h:452-557`, `PhysicsWorld.cpp:1135-1190` (`buildSoftShared`), `:1199-1217` (`addSoftBody`: layer `MOVING`, `mUpdatePosition=false`), `:1222`, `:1245`, `:1300`, `:1331`, `:1356` |
| **Nothing calls `aver_phys_softbody_skin` at runtime.** `SoftBodyScene` creates a skinned body for a skinned mesh and never skins it; callers are the C# wrapper and `tests/physics/src/SoftBodyTest.cpp` only | `SoftBodyScene.cpp:262-286`; tree grep |
| The creation path takes mesh-local vertices plus a centre: entity **rotation and scale never reach the rest shape** ("Known limit") | `modules/render.softbody/README.md` last section, `PhysicsWorld.cpp:1222` |
| `CharacterVirtual`'s companion body is never a soft-body collision hit; `apply_impulse` is the existing velocity-only fallback | `physics_abi.h:526-557` |
| Soft body handles are ordinary body handles (`bodies`/`byId`, `aver_phys_set_entity`, `aver_phys_body_set_layer` which reads the moving half back) | `PhysicsWorld.cpp:1209-1216`, `PhysicsLayers.cpp:114-127` |
| Contact events are polled and cleared at the top of each `aver_phys_step`; soft-body contacts are not reported | `PhysicsWorld.cpp:80-118`, `:381-392`, `physics_abi.h:421-440` |
| Physics step: fixed loop, buoyancy evaluated before `system.Update`, `afterStepJoints`; shutdown order destroys joints, ragdolls, vehicles, then bodies | `PhysicsWorld.cpp:468-480`, `:315-345` |
| Jolt facts used: `Initialize` copies settings positions and velocities into vertices (`:70-82`); edge solve reads `e->mRestLength` live (`:594-597`) and a `FLT_MAX` compliance gives `0*inf = NaN` at zero length, so "disable an edge by compliance" is unsafe; dihedral reads `b->mInitialAngle` live (`:417`); `SkinVertices` uses `mSettings->mVertices` as the bind pose (`:1139-1230`); sleep from `max_v_sq` (`:903-914`); `OnSoftBodyContactValidate` and `OnSoftBodyContactAdded` per soft body per step (`SoftBodyContactListener.h`, `SoftBodyMotionProperties.cpp:150-165`); `PhysicsSystem::SetSoftBodyContactListener` (`PhysicsSystem.h:71`); `Body::SetAllowSleeping` (`Body.h:136`) | `modules/physics.jolt/Jolt/Physics/SoftBody/*`, `Body/Body.h` |
| Jolt has no self-collision or soft-vs-soft collision | grep over `Jolt/Physics/SoftBody/*.h`, `PhysicsSettings.h`: no match |
| `Aver.SoftBody` (Core only): `Particle`, `Beam` (rest, rest0, plastic, broken), `Triangle` (3 edge ids), `SplitEvent`, `Cage` | `softbody/Cage.hpp:17-74` |
| Material yield model (force in N from `axialStiffness * stretch` cm; yield, creep, hardening, `maxBend`, `breakStrain`, brittle behaviours), crush curve, tear radius | `Material.hpp:17-64` |
| The damage half of `step()` (yield creep, snap, recoil) is phase 2 of the Verlet step; Gauss-Seidel is phase 3 | `Solver.cpp:42-87`, `:88-120` |
| `resolveTears` duplicates a particle per disconnected fan, marks 2-broken-edge triangles dead, logs `SplitEvent`s; `countPieces` is connected components | `Cage.cpp:162-285`, `:286` |
| `RenderBinding`: render vertex to particle, render triangle to sim triangle, `applySplits`, `visibleIndices`, `gatherPositions` | `RenderBinding.hpp`, `RenderBinding.cpp:1-81` |
| `Aver.SoftBody` is used only by `tests/softbody` and the editor's `SoftBodyPanel` (Window > Soft Body (plastic)) | `sandbox/src/SoftBodyPanel.cpp`, `SandboxShell.cpp:2050`, `SandboxPanels.cpp:939`, `sandbox/CMakeLists.txt:162` |
| `SoftBodyScene`: 64 resident cap, one body per entity created in `resident()`, `createSkinTargetMesh(source)` with fixed vertex count and the **source's index buffer**, Upload staging ring of 3, `prePass` copy | `SoftBodyScene.cpp:102`, `:166-300`, `:211`, `:307-393`, `:395-420`; `RHI.hpp:489,524,529` |
| `SoftBodyScene` does not write posed bounds back to `CMeshRenderer`; `SkinnedScene` does every frame | `SkinnedScene.cpp:182-202`; `Components.hpp:64-70` |
| The editor asks `SoftBodyScene` through `SandboxApp::posedHandle`; the **packaged game has no soft-body scene** (`GameApp` holds only `skinnedScene_`) and `DrawWorldOptions` has no seam for it | `SandboxRender.cpp:2394-2402`, `SandboxApp.cpp:821-830,2293-2296`, `GameRender.cpp:663`, `GameRender.hpp:217-222,325`, `GameApp.cpp:677-686,2139-2140,2282` |
| The scene walk needs a non-zero `CMeshRenderer::mesh` and a resolvable base handle, then substitutes `posedMesh`; bounds of a "skinned" entity are trusted from the writer | `GameRender.cpp:640-700` |
| `CSoftBody { maxDistanceCm, compliance, body (read-only), flags }`, `kSoftBodyDisabled = 0x1`, registered with a size assert | `Components.hpp:235-262,470`, `Builtins.cpp:134-143` |
| `CSoftBody` is the only soft-body record in `.ocworld` (`COMPONENT SoftBody simulated=`) | `LevelComponents.cpp:28,155,361-392,508`, `OcWorld.cpp:173`, `LevelComponents.hpp:28` |
| Add Component for runtime-registered components: `kPlacedKinds[]`, `canAttach`, `makeNewRecord`; Details sections in `SandboxComponentDetails.cpp` | `PlacedComponentModel.hpp:46-71,100-125,190`, `SandboxComponentDetails.cpp:140,279,412,465-471` |
| Runtime-registered component precedent (zero-filled storage, `attach` writes defaults, fields over the generic scene ABI, no C ABI) | `anim.scene/include/aver/anim/MotionMatchSystem.hpp:1-57` |
| `AnimSystem::skinning(entity, count)` returns the per-frame palette (`model x inverse-bind`) | `AnimSystem.hpp:201` |
| Kinematic ragdoll parts: capsule bodies per bone, parts of one ragdoll never collide with each other, world collision per `userLayer`, `RagdollSystem::start(e, Kinematic, blend)` | `docs/physics/RAGDOLL.md`, `RagdollSystem.hpp:35-80,105-110` |
| Layers: 16 user layers, subtractive matrix `aver_phys_set_layer_collision`, `aver_phys_body_set_layer` | `physics_layers_abi.h:41-65` |
| Provider-relay ABI pattern (host installs one function, `aver_fw_*` forwards, no provider returns 0) | `framework_anim_abi.h:1-60`, `framework_abi.h:57,268-294`, `modules/framework/src/AnimAbi.cpp` |
| Frame order in both hosts: gameplay (physics step) -> `tickAnimGraphs` -> ragdoll tick -> `animSystem().tick` -> particles -> `skinnedScene->update` -> `softBodyScene->update` -> draw | `SandboxApp.cpp:2236-2296`, `GameApp.cpp:2096-2140` |
| Host wiring is one shared header: `registerGameSystems`, `tickAnimGraphs`, `resetGameSystems` | `GameSystemsWiring.hpp:121,166,177` |
| Named-event sink seam for graphs (animation notifies, AI events) | `SandboxApp.cpp:1026-1032`, `GameSystemsWiring.hpp:96-110` |
| Graph nodes: table file + palette file + interop class + tests; aggregated in one `.Concat` chain and one `append...NodeDefs(t)` call | `GraphMotionMatchNodeTable.cs`, `GraphNodeDefsMotionMatch.hpp`, `GraphGameSystemNodeTable.cs:120-121`, `GraphNodeDefs.hpp:115,1107-1108`, `Aver.Graph.Tests/Program.cs:280`, `MotionMatchNodeTests.cs` |
| C# soft-body wrappers on `Body` only (`SoftBodyVertices`, `SoftBodySkin`, `SoftBodyApplyImpulse`); no entity-level cloth or damage API | `Aver.Physics/Body.cs:225-245`, `Physics.cs:162-177` |
| ABI header/binding parity is tested by name | `tests/abi/src/AbiEnumTest.cpp:562-572` (header list `kAbis`) |
| Module manifests generate `cmake/modules.catalog.json`; new options come from `module.json` | `scripts/module-catalog.py`, `cmake/modules.catalog.json:937` |
| Wind exists only as sky cloud drift (`cloudWind`, cm/s) | `OcWorld.cpp:445,1032`, `SandboxPanels.cpp:2729` |

## 2. Design

### 2.1 Architecture

```
 .ocsoft + preset          Aver.SoftBody (pure, Core+Formats)
 + CCloth/CDestructible -> SoftAsset, SimMesh (weld -> Cage + RenderBinding), Damage, Islands, Material
                                   |  arrays only
 Aver.SoftBody.Scene  SoftBodySystem  (scene + physics + anim.scene)
   inbox (damage, tear, wind, ...) -> tick: impacts -> readback -> damage/plastic -> tear/rebuild/split
   -> skin+wind+proxies+LOD -> debris lifetime -> publish PieceViews + events
                                   |  aver_phys_sb_*                |  PieceViews
 Aver.Physics (DLL)  physics_softbody_abi.h        Aver.Render.SoftBody  SoftBodyScene (draw only)
   builder, rebuild, in-place rest, aero, events      epoch mesh rebuild, ring copy, bounds write
```

Units and axes are the engine's (cm, +X forward, +Y right, +Z up, left-handed) at every boundary. Conversion to Jolt stays inside `modules/physics` (`Convert.hpp`).

### 2.2 `.ocsoft` (JSON, UTF-8; `"format":"ocsoft"`, `"version":1`)

Read with `fmt::JsonValue` (`Json.hpp`); written by a small emitter in `SoftAsset.cpp` (the reader has no writer). Unknown keys ignored, a newer version refused. Spec text for `FORMAT_SPECS.md` is added by the main loop.

```json
{ "format":"ocsoft", "version":1, "kind":"cloth",
  "preset":"cloth_light",
  "material": { "areaDensityKgM2":0.25, "compliance":0, "bendCompliance":0.05,
                "yieldStrain":0, "hardening":0, "maxSetCm":0, "breakStrain":0.4,
                "toughnessJ":0, "behavior":"deform", "friction":0.4, "restitution":0 },
  "sim":      { "iterations":6, "damping":0.3, "vertexRadiusCm":0.4, "maxSpeedCmS":2500,
                "gravityScale":1, "weldCm":0.01, "maxParticles":4096, "allowSleep":true },
  "cloth":    { "dragCoeff":1.0, "liftCoeff":0.3, "tearable":false,
                "skin": { "defaultReachCm":6, "backstopCm":-1 } },
  "destruct": { "minImpactCmS":150, "impulseEfficiency":0.6, "maxKickCmS":1200,
                "minPieceTriangles":4, "debrisLifetimeS":12, "maxPieces":16 },
  "paint":    { "meshKey":"<hex fnv1a64 of vertex count + welded positions>",
                "pin":"<hex RLE, one u8 per sim particle, 255 = pinned>",
                "reach":"<hex RLE, u8 per particle, 0 = hard skin, 255 = free>",
                "rules":[ {"type":"above","axis":"z","valueCm":170},
                          {"type":"bone","bone":"spine_03","minWeight":0.5},
                          {"type":"sphere","centreCm":[0,0,120],"radiusCm":8},
                          {"type":"mask","texture":"Textures/cape_mask.png","pinBelow":0.5} ] } }
```

- `kind` selects which of `cloth` and `destruct` is read; `preset` names a row of section 2.2.1 and every key under `material` and `sim` overrides it. Missing keys fall back to the preset, so a hand-written minimal asset is `{"format":"ocsoft","version":1,"kind":"cloth","preset":"cloth_light"}`.
- **Paint** is stored per **welded sim particle** and keyed by `meshKey`. A stale key makes the editor warn ("mesh changed since painting") and fall back to `rules`; the runtime applies `rules` over `paint` only when the key is stale or `pin` is absent. `reach` maps linearly to `[0, defaultReachCm * 2]`.
- **Mask rule**: R channel sampled at the vertex UV with `fmt::loadTexture` (`Texture.hpp:30`), below `pinBelow` pins, G scales reach. This is the vertex-colour substitute.
- Pinned means invMass 0 and a hard-skinned constraint (`maxDistance 0`), exactly the "kinematic vertex" Jolt hard-skins at `SoftBodyMotionProperties.cpp:1215-1228`.

#### 2.2.1 Presets (starting values; tuned headed, section 6)

Compliance is Jolt's (m/N; 0 = inextensible). Strains are dimensionless. All values are **UNVERIFIED starting points**.

| `preset` key (enum) | areaDensity kg/m2 | edge compliance | bend compliance | iterations | damping | behaviour | yieldStrain | maxSetCm | breakStrain | toughnessJ/edge | friction |
|---|---|---|---|---|---|---|---|---|---|---|---|
| `cloth_light` (0) | 0.25 | 0 | 0.05 | 6 | 0.30 | deform, no yield | 0 | 0 | 0.40 (only if `tearable`) | 0 | 0.4 |
| `cloth_heavy` (1) | 0.8 | 0 | 0.01 | 6 | 0.50 | deform, no yield | 0 | 0 | 0.35 (only if `tearable`) | 0 | 0.5 |
| `metal` (2) | 7.8 | 2e-7 | 1e-6 | 8 | 0.50 | deform | 0.010 | 25% of rest | 0.30 | 40 | 0.3 |
| `wood` (3) | 6.0 | 1e-6 | 5e-6 | 8 | 0.50 | fracture | (= break) | 0 | 0.020 | 3 | 0.5 |
| `glass` (4) | 10 | 2e-7 | 1e-6 | 8 | 0.20 | shatter | (= break) | 0 | 0.006 | 0.4 | 0.3 |

### 2.3 Pure library: `Aver.SoftBody` (modules/softbody)

`module.json` gains `Aver.Formats` (for `Json.hpp`, `OcMesh.hpp`, `Texture.hpp`); still no GPU, scene or Jolt. Names below are the contract; bodies are the implementer's.

**`Material.hpp` (edit, additive)**: `f32 yieldStrain = 0` (0 = derive `bendForceN / (axialStiffness * rest)`, the old force model, so `.ocbeam` imports keep meaning), `f32 toughnessJ = 0`, `f32 compliance = 0`, `f32 bendCompliance = 0`, `f32 areaDensityKgM2 = 1`, `f32 friction = 0.3f`, `f32 restitution = 0`. Add `const Material& presetMaterial(SoftPreset)`; `enum class SoftPreset : u8 { ClothLight, ClothHeavy, Metal, Wood, Glass }` (values mirror C#). `crushCurve`, `impactTearRadius`, `depthFromImpulse` stay (used by the damage deposit).

**`Cage.hpp` (edit, additive)**: `Particle` gains `f32 mass` (kg), `f32 reachCm` (< 0 free, 0 hard skin), `u32 simIndex` (index in the current Jolt body, written by the builder); `Beam` gains `u32 bend` (index into `Cage::bends` or `kNone`) and `f32 damage` (J accumulated). New `struct Bend { u32 v[4]; u32 beam; f32 compliance; }` and `Cage::bends`. The Verlet fields (`prev`, `scratchStart`) are removed in phase 7.

**`SimMesh.hpp/.cpp` (new)**

```cpp
struct SimMeshInput {                 // views over fmt::OcMeshData, which this header does not include
  const f32* positions; const f32* uvs; const u32* indices; u32 vertexCount, indexCount;
  const u16* joints; const f32* weights;     // optional, 4 per vertex
};
struct SimBuildSpec { Material material; f32 weldCm = 0.01f; u32 maxParticles = 4096;
                      Mat4 entityWorld; f32 uniformScale = 1; PaintData paint; bool destructible; };
struct SimMesh {                      // result
  Cage cage;                          // particles in WORLD space, beams, triangles, bends
  RenderBinding binding;              // render vertex -> particle, render tri -> sim tri
  std::vector<Vec3> bindLocal;        // per particle, mesh-local (the Jolt "bind pose" for skinning)
  std::vector<u16>  joint4; std::vector<f32> weight4;   // per particle, from the first render vertex
  std::string error;                  // empty on success
};
bool buildSimMesh(const SimMeshInput&, const SimBuildSpec&, SimMesh& out);
u64  simMeshKey(const SimMeshInput&, f32 weldCm);
void applyPaint(Cage&, const PaintData&, const SimMeshInput&, const MaskSampler&, const BoneWeightQuery&);
```

Rules: weld render vertices within `weldCm` (spatial hash, deterministic order: lowest render index is the representative); degenerate and duplicate triangles dropped; one `Beam` per undirected sim edge; `Bend` per interior edge with two live triangles (4 distinct vertices, `v[0..1]` the shared edge, the Jolt `DihedralBend` layout); particle mass = areaDensity x one third of the area of each adjacent triangle (x `uniformScale^2`); `maxParticles` exceeded returns an error (the entity then keeps its authored mesh and logs once; decimation is out of scope). A non-skinned mesh bakes `entityWorld` into particle positions, which lifts the rotation and scale limit for the new path.

**`Damage.hpp/.cpp` (new; the old Phase 2 re-expressed on positions Jolt produced)**

```cpp
struct DamageConfig { f32 dt; f32 creepRate = 8; f32 breakKickCmS = 0; bool tearable = true; };
struct DamageResult { u32 yielded, broken; f32 maxStrain; std::vector<u32> dirtyBeams; /* rest changed */ };
// Reads particle positions (already set from Jolt), updates rest/plastic/broken, no integration.
DamageResult evaluateStrain(Cage&, const DamageConfig&);
struct DamageDeposit { Vec3 point, dir; f32 radiusCm, energyJ; u8 kind; };   // Impact, Explosion, Gameplay
struct DepositResult { f32 kickSpeedCmS; f32 massKg; u32 beamsDamaged, broken; };
// Energy path: spreads energyJ over beams in radius by (1 - d/r)^2, breaks a beam when damage >= toughnessJ
// (brittle or toughnessJ > 0 only), and returns the velocity kick to apply with aver_phys_sb_impulse.
DepositResult depositEnergy(Cage&, const DamageDeposit&, f32 impulseEfficiency, f32 maxKickCmS);
u32  tearWithin(Cage&, Vec3 point, f32 radiusCm);          // scripted cut: breaks beams whose midpoint is in radius
void repairAll(Cage&);                                      // existing repair() + damage, plastic, dead tris
```

`evaluateStrain` per live beam: `strain = (len - rest) / rest0`; `yield = Material::yieldStrain` (or the force-derived value) `+ hardening * plastic`; if `|strain| > yield` and the material deforms, `rest += sign * (|strain| - yield) * rest0 * min(1, creepRate * dt)`, `plastic += ...`, record in `dirtyBeams`; snap when `strain > breakStrain`, `plastic > maxSetCm`, or brittle and `|strain| > yield`; a snap marks `broken` and sets `topologyDirty`. Brittle particles do not creep. `tearable == false` (cloth) disables the snap tests. The kick from `depositEnergy` is `min(maxKick, sqrt(2 * efficiency * E / M_radius) * 100)` cm/s, with `M_radius` the mass of particles inside the radius, so a bullet and an explosion use one code path.

**`Islands.hpp/.cpp` (new)**

```cpp
struct IslandPlan {
  struct Island { std::vector<u32> particles, beams, triangles; bool anchored; f32 massKg; };
  std::vector<Island> islands;        // ordered: anchored first, then by descending mass, ties by lowest particle id
  std::vector<u32> dust;              // particles in pieces smaller than minPieceTriangles
};
IslandPlan planIslands(const Cage&, u32 minPieceTriangles);
```

Call `resolveTears` first (duplicates particles per torn fan), then `planIslands`. An island is **anchored** if it contains a pinned particle. The first island keeps the entity and body handle; the others become debris pieces.

Existing `Cage.cpp` functions (`addBeam`, `addTriangle`, `build`, `repair`, `resolveTears`, `countPieces`, `maxStrain`) and `RenderBinding` are reused unchanged except where noted; `repair` also clears `damage` and restores `Bend` links.

### 2.4 Physics ABI: `physics_softbody_abi.h` (new, `aver_phys_sb_*`)

New header and `src/PhysicsSoftBody.cpp`, `src/SoftBodyAero.cpp/.hpp` in `modules/physics`. Types: `int32_t`, `float`, `const float*`, `const int32_t*` only; units cm and cm/s; every function returns 1/0 (`aver_phys_last_error` as elsewhere) unless it returns a handle. The existing `aver_phys_softbody_*` functions are **untouched** (Fluids uses them: `FluidScene.cpp:235`).

**Builder** (stages arrays; nothing is a body until `finish`):

```
int32_t aver_phys_sb_builder_begin(int32_t vertexCount);                       // builder id, 0 = refused
int32_t aver_phys_sb_builder_vertices(b, const float* restXyz /*world cm, edge/bind basis*/,
                                      const float* invMass /*opt*/,
                                      const float* statePosXyz /*opt, else rest*/,
                                      const float* stateVelXyz /*opt, else 0*/);
int32_t aver_phys_sb_builder_faces(b, const int32_t* tri3, int32_t triCount);
int32_t aver_phys_sb_builder_edges(b, const int32_t* ab, const float* restCm, const float* compliance, int32_t edgeCount);
int32_t aver_phys_sb_builder_bends(b, const int32_t* quad4, const float* compliance, int32_t bendCount);
int32_t aver_phys_sb_builder_skin(b, const int32_t* joints4, const float* weights4,
                                  const float* reachCm /*per vertex, <0 = no skinned constraint*/,
                                  float backstopCm /*<0 off*/, int32_t jointCount);
int32_t aver_phys_sb_builder_params(b, const float* p, int32_t n);
int32_t aver_phys_sb_builder_finish(b, float cx, float cy, float cz,
                                    int32_t replaceBody /*0 = new body*/,
                                    int32_t stateSrcBody /*0 = none*/, const int32_t* stateMap /*new vertex -> old vertex, -1 = rest*/,
                                    int32_t* outEdgeSlot /*edgeCount, final Jolt edge index per input edge, opt*/,
                                    int32_t* outBendSlot /*bendCount, opt*/);
int32_t aver_phys_sb_builder_cancel(b);
```

`params[]` indices (a `#define AVER_PHYS_SB_P_*` block, mirrored in C# and checked by `AbiEnumTest`): `ITERATIONS, DAMPING, FRICTION, RESTITUTION, GRAVITY_SCALE, VERTEX_RADIUS_CM, MAX_SPEED_CMS, ALLOW_SLEEP, DOUBLE_SIDED, LAYER, ENTITY`.

`finish` rules (the contract the tests pin):
- Settings are built **by hand**, not with `CreateConstraints`: one `Edge` per input edge (`mRestLength` = `restCm / 100`, `mCompliance` as given), one `DihedralBend` per bend with `CalculateBendConstraintConstants()` run on the rest positions, faces wound straight through exactly as `buildSoftShared` does (`PhysicsWorld.cpp:1157-1180`, the comment there explains the double negative), no shear edges (a shear edge across a tear would keep it shut), `Skinned` entries only for vertices with `reachCm >= 0` (bind pose = `restXyz` converted, one identity `InvBind` per joint, the trick at `PhysicsWorld.cpp:1259-1269`), then `CalculateSkinnedConstraintNormals()` and `Optimize(results)`. `results.mEdgeRemap` and `mDihedralBendRemap` fill `outEdgeSlot` / `outBendSlot`; the body also keeps them in its registry record.
- The registry record (`SoftRecord`, file-local to `PhysicsSoftBody.cpp`) owns a **non-const** `JPH::Ref<SoftBodySharedSettings>` per body (never shared between bodies), the slot tables, the last palette, the params and the aero state. In-place writes below mutate that object only between `aver_phys_step` calls.
- Centre of mass: `cx,cy,cz` becomes the body position, vertices are stored relative to it (`mUpdatePosition = false`, as `addSoftBody`).
- Vertex state: `Initialize` copies settings positions and velocities. `finish` therefore builds the settings with **rest** positions (edge rest comes from `restCm`, bind pose from rest), creates the body, then overwrites `GetVertices()[i].mPosition/mPreviousPosition/mVelocity` from `statePos/statePos` (or from `stateSrcBody` through `stateMap`, reading the old vertices COM-relative, which is valid because both bodies get the same COM transform). Writing positions before the first step is safe (Jolt's warning is about missed collisions during simulation, `SoftBodyVertex.h:12-15`).
- `replaceBody != 0`: the **same handle** is kept: create the new body, add it, swap `bodies[h]` and `byId`, remove and destroy the old one, re-apply params, layer, `aver_phys_set_entity` user data and the last palette (call `SkinVertices` with it so skinned constraints are valid on the next step). `replaceBody == 0` returns a new handle. `stateSrcBody` and `replaceBody` may name the same body.
- Refusals (return 0 with `aver_phys_last_error`): no world, bad builder, face index out of range, `vertexCount > 65536`, body limit (`PhysicsWorld.cpp:1210`). A refused `finish` leaves the old body intact.
- **Never call from inside a step listener or a contact callback.** The host is single threaded here (the system ticks after `aver_phys_step` returned).

**State and in-place edits**

```
int32_t aver_phys_sb_state(int32_t body, float* outPosXyz, float* outVelXyz, int32_t maxVertices);   // world cm, cm/s; either out may be null
int32_t aver_phys_sb_set_edge_rest(int32_t body, const int32_t* edgeSlot, const float* restCm, int32_t n);
int32_t aver_phys_sb_rest_bends_from_pose(int32_t body, const int32_t* bendSlot, int32_t n);          // dihedral mInitialAngle := current angle
int32_t aver_phys_sb_impulse(int32_t body, const float* centreCm, float radiusCm, const float* dirUnit,
                             float speedCmS, float radialBlend /*0 directional .. 1 radial*/, float falloffPower);
int32_t aver_phys_sb_skin_world(int32_t body, const float* jointMatrices, int32_t jointCount,
                                const float* entityWorld16 /*null = identity*/, int32_t hardSkin);
int32_t aver_phys_sb_set_active(int32_t body, int32_t active);   // wake or deactivate; deactivated keeps its pose
```

- `set_edge_rest` is the **dent**: writes `mEdgeConstraints[slot].mRestLength`. `rest_bends_from_pose` recomputes `mInitialAngle` for the listed bends using the same signed-angle expression as `ApplyDihedralBendConstraints` (`SoftBodyMotionProperties.cpp:395-445`), so the constraint residual is zero in the current pose; without it a dented sheet springs flat again. Both wake the body.
- `skin_world` composes the palette in the engine's row-vector order, `M' = skin[j] * entityWorld * translate(-COM)`, so the cloth simulates in a world-aligned frame and the entity's rotation and scale reach the skinned constraints (the limit in section 1). The record keeps `M'` for `finish`.
- `impulse` adds `dir`/radial velocity x `(1 - d/r)^falloffPower` x `speed` to the velocity of vertices in radius and activates the body (the no-lock activation trick in the comment at `PhysicsWorld.cpp:1396-1425` applies: this function holds the body write lock).

**Aero (wind and drag)** evaluated **inside the fixed loop before `system.Update`**, next to buoyancy (`PhysicsWorld.cpp:468-475`), so it is frame-rate independent:

```
int32_t aver_phys_sb_set_aero(int32_t body, float drag, float lift, int32_t flags);   // drag 0 = off, 0..2; flags bit0 keep-awake
void    aver_phys_sb_set_wind(float vx, float vy, float vz, float gustSpeed, float gustHz, float turbulence);
```

Per triangle: `v_rel = wind(p, step) - mean(v)`, `v_n = v_rel . n`, force `F = 0.5 * 1.2 * A * (drag * |v_n| v_n * n + lift * ( v_rel - v_n n ) |v_rel|)`, applied as `dv = F / m_vertex * dt / 3` per corner (kinematic vertices skipped). `wind(p, step)` = base + `gustSpeed * sin(2 pi gustHz * step * dt + phase(p))` + turbulence from an integer-hash noise of `(floor(p / 100 cm), step / 8)`; **no wall clock and no `rand`**, so it is reproducible. A body with aero and non-zero wind has sleeping disabled through `Body::SetAllowSleeping(false)` (UNVERIFIED on a soft body; fallback is `ActivateBody` each step on the no-lock interface).

**Events** (polled, cleared at the top of `aver_phys_step`, like contacts):

```
int32_t aver_phys_sb_impact_count(void);
int32_t aver_phys_sb_impact_get(int32_t index, int32_t* outSoftBody, int32_t* outOtherBody,
                                float* outPoint3, float* outNormal3, float* outClosingCmS,
                                float* outOtherMassKg, int32_t* outContactVertices);
```

Backed by a `JPH::SoftBodyContactListener` installed at init beside `SetContactListener` (`PhysicsWorld.cpp:305`) and cleared at shutdown (`:334`). `OnSoftBodyContactValidate(soft, other, settings)` stores the other body's linear velocity and inverse mass in a per-pair slot (read-only access of a body that the soft-body update phase does not modify; the race note in Jolt's header is about other threads mutating it, which the step order excludes: UNVERIFIED, see tests). `OnSoftBodyContactAdded(soft, manifold)` then folds contact vertices into one record per `(soft, other)`: mean point and normal, count, and `closing = max(0, (v_other - v_vertex_before) . -n)` where `v_vertex_before` comes from the velocities cached at the start of the step for bodies registered with `AVER_PHYS_SB_P_IMPACTS` (default on for destructibles only, so cloth pays nothing). Events are appended under `eventMutex`. A record below 1 cm/s closing is dropped in the listener.

**Edits to shared physics files (all small)**: `PhysicsWorld.cpp` (call `softbody::installListener()` after `:305` and `removeListener()` at `:334`; `softbody::onStepBegin(dt)` before `Update` at `:478`; `softbody::onBodyRemoved(handle)` in `aver_phys_remove_body`; `softbody::destroyAll()` in shutdown before the body loop at `:327`), `modules/physics/CMakeLists.txt` (two sources), `tests/abi/src/AbiEnumTest.cpp:562` (add the header to `kAbis`).

### 2.5 Runtime: `Aver.SoftBody.Scene` (modules/softbody.scene)

`module.json`: `"name":"Aver.SoftBody.Scene"`, `"dir":"softbody.scene"`, `"option":"AVER_MODULE_SOFTBODY_SCENE"`, default on, `dependsOn`: `Aver.Core, Aver.Formats, Aver.Physics, Aver.Scene, Aver.SoftBody, Aver.Anim, Aver.Anim.Scene`. Added in the root `CMakeLists.txt` inside `if(AVER_MODULE_SCENE)` after the ragdoll block (`:297-299`) with `if(AVER_MODULE_PHYSICS)`. `Aver.Render.SoftBody`'s manifest gains `Aver.SoftBody.Scene`. Regenerate `cmake/modules.catalog.json` with `scripts/module-catalog.py`; run `scripts/module-guard-audit.py`.

#### 2.5.1 Components (runtime-registered, zero-filled storage must mean the useful state)

```cpp
struct CCloth {                 // "CCloth", 40 bytes
  u64 asset = 0;                // .ocsoft ObjectId; 0 = preset only
  u32 preset = 0;               // SoftPreset
  u32 flags = 0;                // negative sense: kClothNoWorld, kClothNoCharacterProxy, kClothNoWind; kClothTearable; kClothRelease (script sets, system clears)
  f32 windScale = 0;            // 0 reads 1
  f32 stiffnessScale = 0;       // 0 reads 1 (scales compliance by 1/s)
  f32 reachScale = 0;           // 0 reads 1 (scales skinned reach)
  f32 freezeDistanceCm = 0;     // 0 = system default
  f32 outMaxStrain = 0;         // OUT
  u32 outState = 0;             // OUT: bit0 awake, bit1 frozen, bit2 released, bit3 asset stale, bits 8+ piece count
};
struct CDestructible {          // "CDestructible", 48 bytes
  u64 asset = 0; u32 preset = 0; u32 flags = 0;   // kDestructNoDebris, kDestructIndestructible (damage ignored), kDestructRepairOnReset
  f32 toughnessScale = 0;       // 0 reads 1 (multiplies toughnessJ and break strains)
  f32 impactScale = 0;          // 0 reads 1 (multiplies deposited energy)
  f32 debrisLifetimeS = 0;      // 0 = asset default
  u32 maxPieces = 0;            // 0 = asset default
  f32 outDamage = 0;            // OUT: broken beams / total, 0..1
  u32 outBroken = 0, outPieces = 0, outState = 0;
};
static_assert(sizeof(CCloth) == 40 && sizeof(CDestructible) == 48);
```

`SoftBodySystem::registerComponents(World&)` registers both with `registerComponent<T>(name).field(...).verify(sizeof)`; OUT fields are read-only fields (the `CSoftBody::body` precedent, `Builtins.cpp:141`). `SoftBodySystem::attachCloth(world, e, preset, asset)` / `attachDestructible(...)` write defaults over the zero-filled bytes and add `CSoftBody` if absent (`CSoftBody` is the marker the draw seam keys on). Cloth and destructible on one entity are refused (one body per entity root); a destructible cloth is a cloth with `kClothTearable`.

`CSoftBody` gains one flag in `Components.hpp`: `kSoftBodyPiece = 0x2` ("runtime piece or debris, owned by the system, never serialised"). The struct size and ids do not change.

#### 2.5.2 System lifecycle and tick

```cpp
class SoftBodySystem {
 public:
  static u32 registerComponents(scene::World&);
  void install(scene::World&);   void uninstall();            // physics wiring and notify sink
  void setMeshSource(MeshSourceFn);                            // u64 meshId -> const fmt::OcMeshData* (host content cache)
  void setAssetSource(AssetSourceFn);                          // u64 id -> const SoftAsset* (parsed .ocsoft, cached)
  void setPaletteSource(PaletteFn);                            // default: anim::animSystem().skinning; tests inject
  void setCharacterProxyFn(ProxyFn);                           // host wires RagdollSystem::start/stop(Kinematic)
  void setNotifySink(NotifyFn, void*);                         // (entity, "SoftBodyTorn", user)
  void setViewPos(const Vec3&);                                // camera, for LOD
  void tick(scene::World&, f32 dt);                            // once per frame, section 2.5.3
  void clear();                                                // destroys bodies and debris entities
  // inbox (the framework provider and graph nodes call these; applied in order at the next tick)
  bool damage(Entity, Vec3 point, f32 radiusCm, f32 energyJ, Vec3 dir, u8 kind);
  u32  explode(Vec3 centre, f32 radiusCm, f32 impulseCmS, f32 energyJ);   // returns entities touched
  u32  tearAt(Entity, Vec3 point, f32 radiusCm);   bool repair(Entity);   bool release(Entity);
  void setWind(Vec3 v, f32 gust, f32 gustHz, f32 turbulence);   void setBudget(const Budget&);
  // outputs
  const PieceView* pieces(u32& count) const;       // for SoftBodyScene
  u32  drainEvents(SoftEvent* out, u32 max);
  Stats stats() const;
};
SoftBodySystem& softBodySystem();   // process singleton, like anim::animSystem()
```

Per entity entry: resolved spec (asset + preset + component scales), `SimMesh`, a `std::vector<Piece>`, per-piece cached world positions/velocities, the topology epoch. Entities enter when they hold `CSoftBody` (not disabled) and a `CMeshRenderer` whose mesh decodes; they leave on component removal, disable or destroy.

**`tick` order** (all loops ascending by entity id, then piece id; this order is the determinism contract):

1. Resolve new and changed entries (build `SimMesh`, create bodies, set layers, ask for proxies). Budget: at most 2 builds per frame; the rest wait.
2. Apply the inbox (damage, explode, tear, repair, release, wind, budget) in arrival order.
3. Poll physics impacts: `aver_phys_sb_impact_get`, sorted by `(soft handle, other handle, index)`, converted to `DamageDeposit{kind=Impact}` when `closing >= minImpactCmS`; energy = `0.5 * m_reduced * closing^2`, `m_reduced = m_other * M_local / (m_other + M_local)` (static others use `M_local`).
4. For each awake piece of each destructible or tearable cloth: read back state once (`aver_phys_sb_state`), mirror into `Cage.particles[].pos`.
5. Destructibles: apply deposits (`depositEnergy` then `aver_phys_sb_impulse`), then `evaluateStrain` (at most every second fixed step, and only for "hot" pieces: impact within 2 s or strain above a watch level), then **plastic apply**: one `aver_phys_sb_set_edge_rest` for `dirtyBeams` and one `aver_phys_sb_rest_bends_from_pose` for the bends adjacent to them.
6. If any beam broke: **tear pipeline** (2.5.4), within the per-frame rebuild budget; leftovers carry to next frame.
7. Cloth: refresh the palette (`PaletteFn`), call `aver_phys_sb_skin_world` (hard skin on the first frame and after a teleport, i.e. when the entity moved more than `teleportCm` in one frame), set aero and wind, update character proxies, LOD (2.5.5).
8. Debris lifecycle (2.5.6), `CCloth`/`CDestructible` OUT fields, publish `PieceView`s, fire events.

It runs once per frame right after `animSystem().tick` and before `skinnedScene->update` in both hosts (physics has already stepped earlier in the frame, `SandboxApp.cpp:2236`, `GameApp.cpp:2096-2125`); the palette it pushes drives the **next** step, a one-frame skin lag that is the same lag ragdoll motors have. Outside Play nothing steps, so soft entities show their bind pose.

#### 2.5.3 Cloth

- **Body**: all cloth is Jolt-`Skinned`. A character cape uses the skeleton palette (`joint4/weight4` from the mesh skin, `CSkeletalMesh` present). Cloth on a prop (flag on a moving cart) uses a **single joint** whose matrix is the entity world, so pinned vertices ride the entity without a second code path. Pinned particles: invMass 0 and `reach 0`; reach-limited particles: `reachCm` from paint or `defaultReachCm`; free particles have no `Skinned` entry. A cape mesh that carries skin weights to the spine and shoulders and an author-painted reach gradient is the typical case.
- **World collision**: the body lives on user layer `clothLayer` (default 14, system option). `installLayers()` once: cloth collides with layer 0 (world, rigid props) and with `proxyLayer` (default 15), nothing else; the proxy layer collides with `clothLayer` only (15 subtractive calls to `aver_phys_set_layer_collision`). Both numbers are `SoftBodySystem::Options`.
- **Character capsules**: for cloth whose entity or an ancestor carries a skeleton, unless `kClothNoCharacterProxy`, the system calls `ProxyFn(entity, true)`; the host implements it as `ragdollSystem().start(e, Kinematic, 0)` with `setUserLayer(proxyLayer)` and stops it when the last cloth user leaves. Per-bone capsules come from the ragdoll builder's existing rules (`RagdollBuilder.hpp`). If no `ProxyFn` is set, cloth ignores characters (documented, not an error).
- **Wind and drag**: global wind (`setWind`, also the `Set Soft Body Wind` node and `SoftBodies.SetWind`), scaled by `CCloth::windScale` and the asset's `dragCoeff`/`liftCoeff`; wind is **independent** of the sky's `cloudWind` (an editor button copies it). `kClothNoWind` skips `set_aero`.
- **Self-collision**: not in v1 (Jolt has none). Mitigations that ship: `vertexRadiusCm`, dihedral bend, the backstop sphere on skinned vertices (`backstopCm`), and a documented authoring rule (layered garments should be separate cloths with the inner one stiffer). Open question 1.
- **Release / tear-off**: `kClothRelease` (C# `ReleaseCloth`, node `Release Soft Body`) drops all pins: the system rebuilds the piece without `Skinned` entries and without invMass 0, so a cape falls off. Same rebuild path as tearing.
- **Tearable cloth** (`kClothTearable`): `evaluateStrain` snaps beams past `breakStrain`; the tear pipeline applies; islands without a pin fall as debris pieces.

#### 2.5.4 Destruction

**Inputs** (all become `DamageDeposit`s in the inbox order): rigid-body impacts (step 3), `damage()`, `explode()` (queries `aver_phys_overlap_sphere` for soft-body handles, resolves entities through `aver_phys_set_entity` user data, one deposit per entity with distance falloff and a radial `aver_phys_sb_impulse`), `tearAt()`. Thresholds per material: `minImpactCmS` (asset `destruct`), `toughnessJ` per edge, `yieldStrain`, `breakStrain`, `maxSetCm`; scaled by `CDestructible::toughnessScale` and `impactScale`.

**Plastic deformation (dent)**: step 5. The body is not rebuilt; its settings are edited in place. Creep is rate limited (`creepRate` 1/s) so a hit spreads over frames.

**Tear pipeline** (step 6), for a piece with newly broken beams:

1. `resolveTears(cage)`: duplicates particles on fans a break disconnected, marks dead triangles, appends `SplitEvent`s (`Cage.cpp:162`).
2. `planIslands(cage, minPieceTriangles)`.
3. For the render side, `binding.applySplits(cage.splitLog)` records which render vertices are duplicated and from which source vertex (UV and normal come from it).
4. For each island, build its arrays: local particle order, `restCm` = current `Beam::rest` x scale, faces from live sim triangles, bends whose four particles and both triangles are in the island, skin entries for particles with `reachCm >= 0`, and `stateMap[i]` = the **old Jolt vertex** of the particle's origin (duplicates copy the origin's position and velocity, which is exactly the "do not lose velocities" rule).
5. Build non-first islands first with `stateSrcBody = old` (reading the old body), then `aver_phys_sb_builder_finish(replaceBody = old, stateMap)` for island 0, which keeps the handle and the entity. Write `Particle::simIndex`, `beamSlot`, `bendSlot` back from `outEdgeSlot/outBendSlot`.
6. Each non-first island becomes a **debris piece**: a new scene entity (`world.create("SoftPiece")`, `CLocal` identity, `CMeshRenderer` copied from the source entity so the scene walk resolves the same asset, material and flags, `CSoftBody{flags = kSoftBodyPiece}`, no `CCloth`/`CDestructible`), a new `Piece` bound to the new body, layer `clothLayer`, a copy of the parent's params. Particles in `dust` are dropped (and never get a body); a `Dust` event carries their centroid so gameplay can spawn particles.
7. Events: one `Torn` per broken beam batch (beam ids, ascending), one `Detached` per new piece, `Dust` per dust cluster.

Pieces are single-use topologies: a debris piece is itself destructible (it keeps the cage subset, materials and damage state) until `maxPieces` or `minPieceTriangles` stops it, so a glass pane shatters recursively under further hits but never below 4 triangles.

**Debris policy / caps** (section 2.9): per-piece age, sleep timer, despawn shrink, global cap with oldest-farthest eviction. A piece asleep for `debrisSleepS` (2 s) and older than `debrisLifetimeS` is shrunk toward its centroid over `despawnShrinkS` (0.6 s, a CPU vertex scale at pack time, no alpha needed), then its entity is destroyed and its body removed.

#### 2.5.5 LOD, sleep, freeze

- **Sleep**: Jolt's own sleep (`SoftBodyMotionProperties.cpp:903-914`) stays on for cloth without wind and for all debris; a sleeping piece costs nothing and is not read back.
- **Distance tiers** (from `setViewPos`): `<1500 cm` full iterations; `<3500 cm` x0.5; beyond x0.25 (min 2), applied with `AVER_PHYS_SB_P_ITERATIONS` through `builder_params`-style `aver_phys_sb_set_params(body, p, n)` (the same array, applied live; add this one entry point). Beyond `freezeDistanceCm` (cloth 6000, debris 4000) the piece is `aver_phys_sb_set_active(false)` and keeps its last pose; a 10% hysteresis band reactivates it. A frozen piece's draw reuses the last packed vertices (no read, no copy).
- **Active-particle budget**: the sum of particles in non-frozen pieces may not exceed `maxActiveParticles` (20000); the farthest pieces freeze first.
- **Render LOD**: none in v1 (one draw mesh per piece). The sim mesh is the welded asset mesh capped at `maxParticles` 4096 per body; larger meshes are refused with a log line, not decimated (open question 4).

#### 2.5.6 Budgets (defaults; `SoftBodySystem::Options`, project-overridable, editor panel)

| Option | Default |
|---|---|
| `maxLivePieces` (debris pieces beyond roots) | 48 |
| `maxActiveParticles` | 20000 |
| `maxRebuildsPerFrame` (tear pipelines + resident builds + draw-mesh epochs) | 2 |
| `maxDepositsPerFrame` | 64 (excess merged by point) |
| `minPieceTriangles` | 4 |
| `debrisLifetimeS` / `debrisSleepS` / `despawnShrinkS` | 12 / 2 / 0.6 |
| `freezeDistance` cloth / debris | 6000 / 4000 cm |
| `clothLayer` / `proxyLayer` | 14 / 15 |
| `damageEvalEveryNSteps` | 2 |

#### 2.5.7 Events

```cpp
enum class SoftEventKind : u8 { Impact, Torn, Detached, Dust, Repaired, Released };
struct SoftEvent { SoftEventKind kind; u32 entity; u32 piece; u32 beam; Vec3 point; f32 value; u32 seq; };
```

`seq` is a per-system counter (the replication key). The notify sink fires the names `SoftBodyImpact`, `SoftBodyTorn`, `SoftBodyDetached`, `SoftBodyDust`, `SoftBodyRepaired` at the owning entity (for `Detached`, at the new piece entity and at the parent), using the `(Entity, const char*, void*)` seam hosts already install for animation and AI (`SandboxApp.cpp:1026-1032`). C# also polls `SoftBodies.DrainEvents`.

### 2.6 Render: `SoftBodyScene` becomes draw-only (modules/render.softbody)

- `resident()` stops creating bodies. It asks `SoftBodySystem::pieces()` for `PieceView { entity, piece, epoch, vertexCount, const Vec3* world, const u32* indices, indexCount, const u8* sourceVertex /*new render vertex -> source*/, bounds, scale01 /*despawn shrink*/, frozen }` and keeps one `Resident` per `(entity, piece)`.
- **Epoch change** (a tear, a release, an island): build a source with `createMesh(restVerts, vertexCount, indices, indexCount)` (rest vertices copied from the base mesh by `sourceVertex`, so UVs and normals are right), `createSkinTargetMesh(source, &vb)`, a fresh staging ring; swap into the `Resident`; put the old `drawMesh`, its source and its ring on a retire list destroyed `kSoftBodyFramesInFlight + 1` frames later (the "retire during recording" rule). The draw-mesh rebuilds count against `maxRebuildsPerFrame`. RT: the new mesh gets a BLAS like any new mesh; it is a compute-written mesh afterwards exactly as skinned ones are.
- **Between epochs**: unchanged ring copy (`SoftBodyScene.cpp:395-420`), now fed by `PieceView::world`. `softBodyPackVertices` (`:39`) is kept for tests and extended by `softBodyPackPieces` (gathers render vertices through the binding, applies `scale01` toward the piece centroid, recomputes normals over the visible indices).
- **Bounds**: `update()` writes the piece's vertex AABB (entity-local) into `CMeshRenderer::aabbMin/Max` every frame it packs, the SkinnedScene rule (`SkinnedScene.cpp:182-202`); without it a flung piece is culled by the asset box at its old place. Debris entities' `CLocal` is set to the piece centroid each frame and vertices are packed relative to it, so `worldToLocal` stays well conditioned.
- `kMaxResident` becomes the system's `maxLivePieces + roots`; the legacy plain `CSoftBody` (no `CCloth`/`CDestructible`) is handled by the system as a cloth piece with the old behaviour (skinned if the mesh has skin, else free, `maxDistanceCm`/`compliance` from the component), so existing content keeps working and finally gets `skin` called.
- **Vulkan/D3D12 parity**: no shader, no new RHI call; `createMesh`, `createSkinTargetMesh`, `destroyMesh`, `bufferBarrier`, `copyBuffer` exist on both backends (`RHI.hpp:489,524,529`). Verified in section 6 with `--backend vulkan`.

### 2.7 Hosts, level records, packaged game

- `GameSystemsWiring.hpp` (shared, additive): `registerGameSystems` registers the components and installs the system (`#if AVER_MODULE_SOFTBODY_SCENE`); new `tickSoftBodies(f32 dt, const Vec3& viewPos)`; `resetGameSystems` calls `softBodySystem().clear()`.
- **Editor**: `SandboxApp.cpp` calls `game::tickSoftBodies` right after `animSystem().tick` (`:2265`), sets the notify sink (`:1026`), the proxy function (ragdoll), mesh/asset sources (the resolvers `softBodyScene_->setResolvers` already uses, `:825`), and `softBodyScene_->update` keeps its place (`:2295`).
- **Packaged game**: `GameApp` gains `softBodyScene_` (created beside `skinnedScene_`, `GameApp.cpp:677`, added as a render feature, updated after `skinnedScene_->update` at `:2140`), calls `tickSoftBodies`, installs the same proxy, sink and sources, and `DrawWorldOptions` gets one field `const render::SoftBodyScene* softBodies = nullptr`; `GameRender.cpp:663` becomes `posedMesh = skinning ? skinning->drawHandle(ent) : 0; if (!posedMesh && opts.softBodies) posedMesh = opts.softBodies->drawHandle(ent);`. This is required, not optional: today a packaged game draws soft entities at their authored mesh (`GameRender.hpp:217-222`).
- **Level records**: `COMPONENT Cloth asset=<path> preset=cloth_light wind=1 stiffness=1 reach=1 freeze=0 tearable=0` and `COMPONENT Destructible asset=<path> preset=metal toughness=1 impact=1 lifetime=0 maxpieces=0 indestructible=0 nodebris=0`; canonical form omits default keys (`PlacedComponentModel.hpp:184`). Append `"Cloth","Destructible"` to the type list (`OcWorld.cpp:173`, `LevelComponents.hpp:28`, enum at `LevelComponents.cpp:28`), add `applyCloth/applyDestructible/capture` cases beside `applySoftBody` (`LevelComponents.cpp:361`, `:508`). Cloth needs a mesh placement; with a `SkeletalMesh` record it skins to it, else it rides the entity. Runtime state (damage, pieces) is **not saved**; a loaded level starts intact (open question 5). Debris entities are flagged `kSoftBodyPiece` and skipped by placement capture (the editor save walks `levelEntities_`; confirm the Play snapshot does not keep runtime entities, UNVERIFIED).

### 2.8 Framework relay ABI: `framework_softbody_abi.h` (new, `aver_fw_softbody_*`)

New header plus `modules/framework/src/SoftBodyAbi.cpp`, own `AVER_FW_SOFTBODY_ABI_VERSION 1`, the `framework_anim_abi.h` conventions: `int32_t/float` only, 1 on success, no provider returns 0.

```c
#define AVER_FW_SB_OP_DAMAGE   1   /* in: x y z dirx diry dirz radius energy kind  (entity or 0) */
#define AVER_FW_SB_OP_EXPLODE  2   /* in: x y z radius impulse energy; out i[0] = entities touched */
#define AVER_FW_SB_OP_TEAR     3   /* in: x y z radius; out i[0] = beams broken */
#define AVER_FW_SB_OP_REPAIR   4
#define AVER_FW_SB_OP_RELEASE  5
#define AVER_FW_SB_OP_WIND     6   /* in: vx vy vz gust gustHz turbulence */
#define AVER_FW_SB_OP_BUDGET   7   /* in: maxPieces maxParticles maxRebuilds lifetime */
#define AVER_FW_SB_OP_PRESET   8   /* in: preset (changes the preset of the entity's component) */
#define AVER_FW_SB_OP_STATE    9   /* out f: damage01 maxStrain; out i: pieces brokenEdges state */
#define AVER_FW_SB_OP_EVENTS  10   /* drains up to n events: i = kind entity piece beam seq; f = x y z value */
typedef int32_t (AVER_FW_CALL* aver_fw_softbody_fn)(int32_t op, int32_t entity, const float* in, int32_t inCount,
                                                    float* outF, int32_t* outI, int32_t cap, void* user);
AVER_FW_ABI int32_t aver_fw_set_softbody_provider(aver_fw_softbody_fn fn, void* user);
AVER_FW_ABI int32_t aver_fw_softbody_abi_version(void);
AVER_FW_ABI int32_t aver_fw_softbody_damage(int32_t entity, float x, float y, float z, float dx, float dy, float dz,
                                            float radiusCm, float energyJ, int32_t kind);
AVER_FW_ABI int32_t aver_fw_softbody_explode(float x, float y, float z, float radiusCm, float impulseCmS, float energyJ);
AVER_FW_ABI int32_t aver_fw_softbody_tear(int32_t entity, float x, float y, float z, float radiusCm);
AVER_FW_ABI int32_t aver_fw_softbody_repair(int32_t entity);
AVER_FW_ABI int32_t aver_fw_softbody_release(int32_t entity);
AVER_FW_ABI int32_t aver_fw_softbody_set_wind(float vx, float vy, float vz, float gustSpeed, float gustHz, float turbulence);
AVER_FW_ABI int32_t aver_fw_softbody_set_budget(int32_t maxPieces, int32_t maxParticles, int32_t maxRebuilds, float debrisLifetimeS);
AVER_FW_ABI int32_t aver_fw_softbody_set_preset(int32_t entity, int32_t preset);
AVER_FW_ABI int32_t aver_fw_softbody_state(int32_t entity, float* outDamageMaxStrain2, int32_t* outPiecesBrokenState3);
AVER_FW_ABI int32_t aver_fw_softbody_drain_events(int32_t* outI /*5 per event*/, float* outF /*4 per event*/, int32_t max);
```

The provider (`softBodyProvider`, a free function in `softbody.scene` with `user = &softBodySystem()`) is installed in `registerGameSystems`; each op forwards to the inbox, so every C# or graph call is deferred to the next tick (deterministic order). Component add/remove and fields use the generic scene ABI (`aver_scene_add_component` by name, `aver_scene_field`), as motion matching does; no scene-ABI change.

### 2.9 C# API

`scripting/csharp/Aver.Framework/SoftBodies.cs` (new) plus `SoftBodyNative.cs` (new, P/Invoke for the relay, `AnimNative.cs` pattern):

```csharp
public enum SoftPreset { ClothLight = 0, ClothHeavy = 1, Metal = 2, Wood = 3, Glass = 4 }
public enum SoftDamageKind { Impact = 0, Explosion = 1, Gameplay = 2 }
public enum SoftEventKind { Impact, Torn, Detached, Dust, Repaired, Released }
public readonly record struct SoftEvent(SoftEventKind Kind, int Entity, int Piece, int Beam, Vec3 Point, float Value, int Seq);
public readonly record struct SoftBodyState(float Damage01, float MaxStrain, int Pieces, int BrokenEdges, bool Awake, bool Frozen);

public readonly partial struct Entity
{
    public bool AddCloth(SoftPreset preset = SoftPreset.ClothLight, string asset = "");      // adds CSoftBody + CCloth (needs a mesh)
    public bool AddDestructible(SoftPreset preset = SoftPreset.Metal, string asset = "");    // adds CSoftBody + CDestructible
    public bool SetSoftPreset(SoftPreset preset);
    public bool ApplySoftDamage(Vec3 point, float radiusCm, float energyJoules, Vec3 direction = default, SoftDamageKind kind = SoftDamageKind.Gameplay);
    public bool TearSoftBody(Vec3 point, float radiusCm);          // scripted cut
    public bool RepairSoftBody();
    public bool ReleaseCloth();                                    // unpin: a cape falls off
    public bool TryGetSoftBodyState(out SoftBodyState state);
}
public static class SoftBodies
{
    public static bool SetWind(Vec3 velocityCmPerS, float gustCmPerS = 0, float gustHz = 0.3f, float turbulence = 0);
    public static int  Explode(Vec3 centre, float radiusCm, float impulseCmPerS, float energyJoules);   // returns entities touched
    public static bool SetBudget(int maxPieces = 48, int maxParticles = 20000, int maxRebuildsPerFrame = 2, float debrisLifetimeSeconds = 12);
    public static int  DrainEvents(Span<SoftEvent> buffer);
}
internal static class SoftBodyGraphInterop     // int-entity / primitive signatures for GraphCompiler
{
    internal static bool SoftBodyDamageForGraph(int entity, float x, float y, float z, float radius, float energy, float dirX, float dirY, float dirZ);
    internal static bool SoftBodyExplodeForGraph(float x, float y, float z, float radius, float impulse, float energy, out int touched);
    internal static bool SoftBodyTearForGraph(int entity, float x, float y, float z, float radius);
    internal static bool SoftBodyRepairForGraph(int entity);
    internal static bool SoftBodyReleaseForGraph(int entity);
    internal static bool SetSoftBodyWindForGraph(float vx, float vy, float vz, float gust, float gustHz, float turbulence);
    internal static bool SetSoftBodyPresetForGraph(int entity, int preset);
    internal static bool GetSoftBodyStateForGraph(int entity, out float damage, out float maxStrain, out int pieces, out int brokenEdges);
}
```

`Aver.Physics` (assembly) gets the low-level surface for tools and tests: `Native.SoftBody.cs` (every name copied from `physics_softbody_abi.h` character for character, the `Native.Ragdoll.cs` rule) and `SoftBody.cs` with a `SoftBodyBuilder`/`SoftBodyHandle` wrapper (`State`, `Impulse`, `SetEdgeRest`, `Skin`, `SetAero`, `SetWind`, `DrainImpacts`). Keep it thin; scripts use `Entity` and `SoftBodies`.

### 2.10 Aver Node nodes

Category `"Physics"` (a soft-body body is a physics object), in `scripting/csharp/Aver.Graph/GraphSoftBodyNodeTable.cs` (new, a `SoftBodyTable()` added to the `.Concat` chain at `GraphGameSystemNodeTable.cs:120-121`) and the palette mirror `sandbox/src/GraphNodeDefsSoftBody.hpp` (new, `appendSoftBodyNodeDefs(t)` called after `appendMotionMatchNodeDefs(t)` at `GraphNodeDefs.hpp:1108`, included at `:115`). Exec nodes have pins `exec`, `entity:int`, outputs `then`, `success:bool`. Pin names are the parser's; defaults in the third column are the unwired values.

| Node | Pins (besides exec/then/success) | Wraps |
|---|---|---|
| `SoftBodyDamage` | in `entity:int`, `x,y,z:float`, `radius:float=30`, `energy:float=100` (J), `dirX,dirY,dirZ:float` (0 = radial) | `SoftBodyDamageForGraph` |
| `SoftBodyExplode` | in `x,y,z`, `radius:float=300`, `impulse:float=600` (cm/s), `energy:float=500`; out `touched:int` | `SoftBodyExplodeForGraph` |
| `SoftBodyTear` | in `entity`, `x,y,z`, `radius:float=20`; success only | `SoftBodyTearForGraph` |
| `SoftBodyRepair` | in `entity` | `SoftBodyRepairForGraph` |
| `ReleaseSoftBody` | in `entity` | `SoftBodyReleaseForGraph` |
| `SetSoftBodyWind` | in `vx,vy,vz`, `gust:float=0`, `gustHz:float=0.3`, `turbulence:float=0` | `SetSoftBodyWindForGraph` |
| `SetSoftBodyPreset` | in `entity`, `preset:int=0` | `SetSoftBodyPresetForGraph` |
| `GetSoftBodyState` (pure) | in `entity`; out `found:bool`, `damage:float`, `maxStrain:float`, `pieces:int`, `brokenEdges:int` | `GetSoftBodyStateForGraph` |

Events reach graphs through the existing named-event seam: a graph handles `OnEvent` with names `SoftBodyTorn`, `SoftBodyDetached`, `SoftBodyDust`, `SoftBodyImpact`, `SoftBodyRepaired` (no new node type). `docs/AVER_NODE_NODES.md` rows and `docs/SCRIPTING_API.md` section are added by the main loop at integration (the 8 rows above and the C# block of 2.9).

### 2.11 Editor

- **Add Component**: two `PlacedKind` rows after `SmartObject` in `kPlacedKinds` (`PlacedComponentModel.hpp:46-71`): `{"Cloth","Cloth","Simulates this mesh as cloth (pinned, painted or ruled). Skins to a Skeletal Mesh if there is one.","Cloth",0,"CCloth",{".ocsoft"}}` and `{"Destructible","Destructible",...,"CDestructible",{".ocsoft"}}`; `canAttach` needs a mesh and refuses an entity that has the other one; `makeNewRecord` seeds `preset`.
- **Details sections** in `SandboxComponentDetails.cpp` (widget half) over a pure `sandbox/src/SoftBodyDetailsModel.hpp` (ImGui-free, testable): preset combo (Cloth light/heavy, Metal, Wood, Glass), material sliders (compliance, bend, yield strain, break strain, toughness, density), sim sliders (iterations, damping, vertex radius), cloth wind/drag/reach scales and flags, destructible thresholds, debris lifetime and caps, read-only OUT readouts (damage, pieces, max strain, awake/frozen), buttons **Hit**, **Explode**, **Tear**, **Repair**, **Release** (enabled in Play, call the same inbox), **Create .ocsoft** (writes `<mesh dir>/<mesh>.ocsoft` and assigns it), **Copy sky wind**.
- **Paint** (`SoftBodyPaintTool.hpp/.cpp`, model + viewport brush; precedent `LandscapePaintTool`): modes Pin, Reach, Erase; radius and strength sliders; picks the nearest sim particle with the existing `ViewportPick` ray test on the entity's mesh and selects particles inside the brush radius in **sim space** (so welded seams paint together); rule rows (above-height, near-bone with weight, sphere, mask texture) with a live preview count; writes `paint.pin/reach` and `meshKey` into the `.ocsoft` through the same undo snapshot used by other asset editors. Pinned particles draw yellow, reach as a green-to-red gradient.
- **Debug view of strain** (`SoftBodyDebugOverlay.hpp/.cpp`, `MotionDebugOverlay` pattern: `collect` emits `rhi::LineVertex`, hooked under `!noEditorChrome_`): beams as lines coloured blue (compressed) to green (rest) to red (at `breakStrain`), plastic set as white tint, **broken beams magenta**, pinned particles yellow points, wind arrow, piece bounds, `Show > Soft Bodies` toggles (`strain`, `pins`, `pieces`, `wind`, `budget text`). The strain it shows is the same `Beam` data the damage step uses.
- **Content Browser**: `cbCreateSoftAsset()` (copy of `cbCreateMotionDb`), type label `.ocsoft` = "Soft Body Asset".
- **Removed**: the Window > Soft Body (plastic) panel (`SoftBodyPanel.*`, `SandboxShell.cpp:2050`, `SandboxPanels.cpp:939`, the `SandboxApp.hpp` members, `sandbox/CMakeLists.txt:26,162`).
- **Profiler**: a `PlayPhase::SoftBody` row in `PlayProfile.hpp` around `tickSoftBodies`.

### 2.12 Determinism and network-agnostic notes

- **Inputs** are only: asset bytes + preset, the ordered inbox (damage, explode, tear, repair, release, wind, budget), the entity transforms and palettes per tick, and the fixed step count. Everything else (strain, tears, islands, piece ids) is derived. Piece and event ids come from counters advanced in the fixed tick order above, never from hashing pointers or container iteration order (`std::unordered_map` is never iterated to produce output; vectors sorted by id).
- **Wind** is a pure function of (position, step index): no clock, no `rand`.
- **Tear decisions** compare floats produced by Jolt; on one binary and one instruction set with the same call order they reproduce (Jolt's own guarantee, `modules/physics.jolt/CMakeLists.txt` determinism note). Cross-platform lockstep is **not** promised; a networked game should treat the server's `SoftEvent` stream (`Torn` beam ids, `Detached` piece ids and seq) as authoritative and replay it on clients: `tearAt`/`repair` re-create the same topology from ids without needing the same floats. Impact events are inputs only on the authority.
- **Replay equality tests** (section 5) run each scenario twice and compare positions, events and piece ids bit for bit.
- Real time never reaches the sim: `dt` is the fixed step.

## 3. FILE OWNERSHIP

### 3.1 Created (only this feature touches them)

- `modules/softbody/include/aver/softbody/{SoftAsset,SimMesh,Damage,Islands}.hpp`, `modules/softbody/src/{SoftAsset,SimMesh,Damage,Islands}.cpp`
- `modules/softbody.scene/{CMakeLists.txt,module.json,README.md}`, `include/aver/softbody/SoftBodySystem.hpp`, `src/{SoftBodySystem,SoftBodyBuild,SoftBodyDamage,SoftBodyCloth,SoftBodyProvider}.cpp`
- `modules/physics/include/aver/physics/physics_softbody_abi.h`, `modules/physics/src/{PhysicsSoftBody,SoftBodyAero}.cpp`, `modules/physics/src/SoftBodyAero.hpp`
- `modules/framework/include/aver/framework/framework_softbody_abi.h`, `modules/framework/src/SoftBodyAbi.cpp`
- `scripting/csharp/Aver.Framework/{SoftBodies,SoftBodyNative}.cs`, `scripting/csharp/Aver.Physics/{Native.SoftBody,SoftBody}.cs`, `scripting/csharp/Aver.Graph/GraphSoftBodyNodeTable.cs`, `scripting/csharp/Aver.Graph.Tests/SoftBodyNodeTests.cs`
- `sandbox/src/{SoftBodyDetailsModel.hpp,SoftBodyPaintTool.hpp,SoftBodyPaintTool.cpp,SoftBodyDebugOverlay.hpp,SoftBodyDebugOverlay.cpp,GraphNodeDefsSoftBody.hpp}`
- `tests/softbody.scene/{CMakeLists.txt,src/SoftBodySystemTest.cpp,src/SoftBodyFixtures.hpp}`, `tests/physics/src/SoftBodyBuilderTest.cpp`, `tests/softbody/src/{SimMeshTest,DamageTest,IslandsTest,SoftAssetTest}.cpp`, `tests/editor/src/SoftBodyDetailsModelTest.cpp`
- `test-content/SoftBodyLab/` (level, cape character content, flag, curtain, glass pane, steel door, plank crate; created in phase 6 from CC0 or procedural geometry, no third-party assets)
- `docs/SOFTBODY.md` is **rewritten** (owned by this feature) to describe the Jolt-driven design; `docs/physics/SOFTBODY_DESTRUCTION.md` optional user-facing summary of 2.2-2.5.

### 3.2 Modified, owned by this feature

- `modules/softbody/`: `CMakeLists.txt` (sources, link `Aver.Formats`, drop retired sources in phase 7), `module.json`, `README.md`, `include/.../{Material,Cage,RenderBinding}.hpp`, `src/{Cage,RenderBinding}.cpp` (phase 7 deletes `Solver.cpp`, `AsyncSolver.cpp`, `softbody_abi.cpp`, `AsyncSolver.hpp`, `softbody_abi.h`)
- `modules/render.softbody/`: `CMakeLists.txt`, `module.json`, `README.md`, `include/aver/render/SoftBodyScene.hpp`, `src/SoftBodyScene.cpp`
- `tests/softbody/CMakeLists.txt` and its two existing tests (ported in phase 7), `tests/render.softbody/src/SoftBodySceneTest.cpp` (+ `CMakeLists.txt`), `tests/physics/CMakeLists.txt` (one target), `tests/physics/src/SoftBodyTest.cpp` (only if a test needs a helper; the existing tests must not change)
- `sandbox/src/SoftBodyPanel.{hpp,cpp}` (deleted phase 7)
- `docs/ARCHITECTURE.md` rows for `Aver.SoftBody` (`:290`) and `Aver.Fracture` (`:85,651`), `docs/physics-net/PHYSICS_NET.md:3`, `docs/STALE_CODE.md` (note the retired solver), `modules/fracture/` (deleted)

### 3.3 SHARED files (other features may also touch them; keep edits small, additive, append-only)

| File | Edit |
|---|---|
| `CMakeLists.txt` (root) | `add_subdirectory(modules/softbody.scene)` inside the scene block (`:297-299` neighbourhood); `add_subdirectory(tests/softbody.scene)` in the tests section; the `Aver.SoftBody` test guard (`:495`) unchanged |
| `cmake/modules.catalog.json` | regenerated by `scripts/module-catalog.py`: new entry, `Aver.Fracture` removed |
| `modules/physics/CMakeLists.txt` | add `src/PhysicsSoftBody.cpp`, `src/SoftBodyAero.cpp` |
| `modules/physics/src/PhysicsWorld.cpp` | five one-line hook calls (listener install/remove, step begin, body removed, shutdown) |
| `modules/physics/src/PhysicsInternal.hpp` | none planned (registry is file-local); a one-line `extern` only if a hook needs it |
| `modules/framework/CMakeLists.txt` | add `src/SoftBodyAbi.cpp` |
| `modules/scene/include/aver/scene/Components.hpp` | one constant, `kSoftBodyPiece = 0x2` |
| `modules/formats/src/OcWorld.cpp` | two names appended to the COMPONENT type list (`:173`) |
| `Runtime/include/aver/game/LevelComponents.hpp`, `Runtime/src/LevelComponents.cpp` | two enum values, apply/capture cases |
| `Runtime/include/aver/game/GameSystemsWiring.hpp` | register, `tickSoftBodies`, reset (additive) |
| `Runtime/include/aver/game/GameRender.hpp`, `Runtime/src/GameRender.cpp` | `DrawWorldOptions::softBodies` and the one-line `posedMesh` fallback (`:663`) |
| `Runtime/include/aver/game/GameApp.hpp`, `Runtime/src/GameApp.cpp`, `Runtime/CMakeLists.txt` | `softBodyScene_`, install, tick, update, link `Aver.SoftBody.Scene` and `Aver.Render.SoftBody` |
| `sandbox/src/SandboxApp.cpp`, `SandboxApp.hpp` | install, tick call, overlay member, remove old panel members |
| `sandbox/src/SandboxPanels.cpp`, `SandboxShell.cpp`, `SandboxRender.cpp`, `SandboxComponentDetails.cpp`, `PlacedComponentModel.hpp`, `PlayProfile.hpp`, `SandboxContentBrowser.cpp` | one block each (menu entry, overlay collect, details sections, two kinds, profiler phase, create-asset entry); remove the old panel hooks (`SandboxPanels.cpp:939`, `SandboxShell.cpp:2050`) |
| `sandbox/CMakeLists.txt` | add new sources, link `Aver.SoftBody.Scene`; remove `SoftBodyPanel.cpp` (`:26`) |
| `sandbox/src/GraphNodeDefs.hpp` | `#include "GraphNodeDefsSoftBody.hpp"` (`:115`) and `appendSoftBodyNodeDefs(t)` (`:1108`) |
| `scripting/csharp/Aver.Graph/GraphGameSystemNodeTable.cs` | `.Concat(SoftBodyTable())` at `:120-121` |
| `scripting/csharp/Aver.Graph.Tests/Program.cs` | `failures += SoftBodyNodeTests.RunAll();` beside `:280` |
| `tests/abi/src/AbiEnumTest.cpp` | add `physics_softbody_abi.h` to the physics `kAbis` row (`:562`) and the `AVER_PHYS_SB_P_*` constants to the enum table |
| `docs/ABI.md` (§1 routing table, §16 note), `docs/MODULES.md`, `docs/SCRIPTING_API.md`, `docs/AVER_NODE_NODES.md`, `docs/formats/FORMAT_SPECS.md` | main loop at integration, from the sections named above |

Not touched: `scene_abi.h`, `framework_abi.h`, `Native.cs`, `Entity.cs`, `modules/rhi*`, `modules/ragdoll` (the proxy is injected), `modules/anim*`, `modules/render.skin`, `modules/formats` beyond `OcWorld.cpp`.

## 4. Phased implementation steps

Each phase ends green (`ctest`, the `Aver.Graph.Tests` runner) before the next starts. Sizes: A M, B L, C L, D L, E M, F M, G S. Overall **XL**.

**Phase A, pure library (no Jolt, no scene).** `Material`/`Cage` additions, `SoftAsset` (parse, write, presets, paint codec, validation), `SimMesh` (weld, beams, bends, masses, paint/rules/mask), `Damage` (`evaluateStrain`, `depositEnergy`, `tearWithin`, `repairAll`), `Islands`. Port the old tear-and-binding tests to the new entry points while the Verlet code still exists (nothing deleted yet). Exit: `tests/softbody` green with the new suites.

**Phase B, physics ABI.** `physics_softbody_abi.h`, builder/finish/replace/state, in-place rest and bend writes, `skin_world`, `impulse`, aero + wind in the step loop, impact listener and queue, params, active, registry cleanup, hooks in `PhysicsWorld.cpp`. C# `Native.SoftBody.cs`/`SoftBody.cs`, `AbiEnumTest` row. Exit: `SoftBodyBuilderTest` green; the existing `SoftBodyTest` (1088 lines) and `FluidDampingCalibrationTest` unchanged and green.

**Phase C, cloth runtime and render.** `Aver.SoftBody.Scene` module, components, resident building, skin push (palette and entity-world), proxies, layers, wind, LOD/freeze, budgets; `SoftBodyScene` refactor to `PieceView`, bounds write, legacy `CSoftBody` routed through the system (it finally calls `skin`); both hosts wired (`GameApp` gains the scene); `COMPONENT Cloth` records; headless `SoftBodySystemTest` cloth cases. Exit: cape and flag verified headed (section 6, steps 1-4) in the editor and the packaged runtime.

**Phase D, destruction.** Damage inbox, impact polling, plastic apply, tear pipeline, rebuild, island split, debris entities, caps, events + notify, `COMPONENT Destructible`. Exit: `SoftBodySystemTest` destruction cases and replay-equality green; headed steps 5-9.

**Phase E, framework relay, C#, graph nodes.** `framework_softbody_abi.h`, `SoftBodyAbi.cpp`, provider, `SoftBodies.cs`, interop, node table + palette + tests + `Program.cs` line. Exit: `SoftBodyNodeTests` green, a sample graph drives damage and wind.

**Phase F, editor.** Add Component kinds, Details model + sections, paint tool, debug overlay, Content Browser entry, profiler row, `Create .ocsoft`, SoftBodyLab content. Exit: `SoftBodyDetailsModelTest` green, headed steps 10-12.

**Phase G, retire and document.** Delete the Verlet solver (`Solver.cpp`, async worker, `aver_sb_*`, `softbody_abi.*`), `SoftBodyPanel.*` and its hooks, `modules/fracture`, stale `Particle::prev`/`StepConfig`/`StepResult`; port or drop `PlasticSoftBodyTest`/`PlasticSoftBodyAsyncTest` (their tear-in-two and slit-duplicates properties already live in `IslandsTest`; the determinism property is covered by replay equality); rewrite `docs/SOFTBODY.md`, fix the ARCHITECTURE and PHYSICS_NET lines, regenerate the catalog, run `module-guard-audit.py` and `docs-check.py`. Exit: full `ctest` green, no reference to `aver_sb_` or `Aver.Fracture` left.

## 5. Tests

All ctest executables are picked up by the `*Test` walk in the root `CMakeLists.txt`; each phase adds only an `add_executable`.

**`tests/softbody` (link `Aver.SoftBody Aver.Formats Aver.Core Aver.Platform`; no GPU)**
- `SoftAssetTest`: write -> parse -> write byte-identical; every `valid()` refusal; a newer version refused; unknown keys ignored; preset table values; minimal asset resolves to the preset; paint RLE round-trip; stale `meshKey` detected.
- `SimMeshTest`: weld of a UV-seamed quad pair gives one particle across the seam with two render vertices (binding many-to-one); degenerate and duplicate triangles dropped; one beam per edge; bend quad layout (shared edge first); masses sum to `areaDensity x area`; entity yaw 90 degrees bakes into positions; `maxParticles` overflow errors; mask rule against an in-memory texture; `simMeshKey` changes with a moved vertex.
- `DamageTest`: a beam held at 1.5x yield strain creeps `rest` toward the stretched length and keeps it after release; below yield returns unchanged; hardening raises the yield; `breakStrain`, `maxSetCm` and brittle-at-yield each snap; `tearable=false` never snaps; `depositEnergy` kick matches `sqrt(2 e E / M)`; toughness breaks exactly the beams whose accumulated damage passes it; falloff is monotone; same input twice gives identical bits.
- `IslandsTest` (ported from the old suite): a strip cut across tears into exactly 2 islands, 5 duplicated particles on the cut row, every render triangle wholly on one side; a single broken edge in a sheet splits nothing; anchored island ordering; pieces under `minPieceTriangles` go to `dust`; render binding: new vertices copy their source, dead triangles omitted.

**`tests/physics` (`SoftBodyBuilderTest`, link `Aver.Physics Aver.Core`; no GPU)**
- Builder: a 10x10 pinned-edge sheet hangs and settles; a refused `finish` leaves the old body; wrong index refused with `aver_phys_last_error`; shutdown and re-init leave no registry entries.
- **Rebuild keeps state**: step 1 s, `rebuild` with identity map, then state positions and velocities equal the pre-rebuild ones within 1e-4 cm and the handle is unchanged; with a duplicated vertex the duplicate starts with the origin's velocity.
- **Dent**: `set_edge_rest` + `rest_bends_from_pose` on a pressed region; after 2 s the region keeps the dent (displacement > 80% of the pressed depth); without the bend call it springs back (guards the bend rule).
- **Tear**: remove a column of edges via `rebuild`, the two halves fall apart (distance grows), each keeps its own momentum.
- **Skin world**: pinned edge follows an entity matrix translated and yawed 90 degrees (positions within 0.1 cm of `entityWorld x bind`), hard skin snaps on the first call.
- **Aero**: a sheet in a 600 cm/s wind deflects downwind more with `drag 1` than `0.2`, none with `0`; the same run twice is bit identical; wind with a changed fixed step gives the same displacement within 2% (frame-rate independence).
- **Impacts**: a 5 kg sphere dropped on a sheet yields one `impact_get` record with closing speed within 15% of `sqrt(2 g h)`; resting contact yields none; cleared at the next `aver_phys_step`.
- Layer: cloth on layer 14 passes through a layer-0 kinematic ragdoll-less box only when the matrix says so; proxy layer collides with cloth only.
- **UNVERIFIED items this suite settles**: `Body::SetAllowSleeping` on a soft body, `SetObjectLayer` on a soft body, reading the other body's velocity in `OnSoftBodyContactValidate`.

**`tests/softbody.scene` (`SoftBodySystemTest`; links `Aver.SoftBody.Scene Aver.Physics Aver.Scene Aver.SoftBody Aver.Core`; palettes and meshes are injected, no GPU, no device)**
- Components: registration, sizes 40 and 48, zero-filled defaults valid, `attach*` adds `CSoftBody`, OUT fields read-only.
- Cloth: a flag mesh pinned along one edge hangs; wind swings it and `windScale 0` does not; a skinned cape follows an injected palette moving 100 cm; entity yaw reaches the body; `kClothRelease` drops it; freeze beyond `freezeDistanceCm` stops stepping and a return reactivates; `maxActiveParticles` freezes the farthest.
- Destruction: a `metal` panel hit with `damage()` keeps a dent (`outMaxStrain`, `set_edge_rest` called) while a `cloth` does not; a `glass` pane hit with enough energy tears, `Torn` then `Detached` events in order, pieces <= `maxPieces`, small shards become `Dust`; a `wood` plank breaks at `breakStrain`; `explode()` touches every destructible in radius and none outside; `tearAt` + `repair` restore topology and body count; debris is despawned after `debrisLifetimeS` and its entities and bodies are gone; `maxLivePieces` evicts oldest-farthest; `maxRebuildsPerFrame` defers extra tears to later frames without losing them.
- **Replay equality**: two fresh systems run the same ordered script (hits, wind, tear) for 600 ticks; vertex positions, event lists (`seq`, ids) and piece counts are bit identical.
- Legacy: a plain `CSoftBody` entity (old content) still simulates and now follows `skin` when the mesh has skin weights.

**`tests/render.softbody`**: `softBodyPackPieces` with a welded binding (seam vertices share a particle but keep UVs), duplicate vertices after a split, dead triangles omitted, `scale01` shrinks toward the centroid, normals recomputed on visible triangles; existing `softBodyPackVertices` cases unchanged.

**`tests/editor`**: `SoftBodyDetailsModelTest` (ImGui-free, like `MontageEditorTest`): preset application writes the right fields, canonical record round-trip, `canAttach` rules, rule-based paint counts, brush selection in sim space, `.ocsoft` create/assign/undo bookkeeping.

**C# (`Aver.Graph.Tests/SoftBodyNodeTests.cs`, `MotionMatchNodeTests` mould)**: every table row names a real `SoftBodyGraphInterop` method matching its pins and defaults; emitted IL valid; exec nodes refused by the pure compiler; `GetSoftBodyState` works as a pure read; `NodeRowKit.CheckRows`, `CheckDefaultPins`, `CheckIlValid`, `CheckCompilerSplit`.

**`AbiEnumTest`**: `physics_softbody_abi.h` against `Native.SoftBody.cs` (names, parameter names) and the `AVER_PHYS_SB_P_*` indices against the C# constants.

## 6. Headed manual verification (the main loop runs these; the GPU must be idle, the owner plays other games)

Use `test-content/SoftBodyLab`. Editor first, then the packaged runtime, then `--backend vulkan`. Screenshots at 1440p with a ping for remote runs.

1. **Flag**: Cloth light pinned on one edge, wind off -> hangs still and sleeps (check `Awake` readout false after a few seconds); wind (300, 0, 0) with gust -> ripples, no jitter, no stretch beyond a few percent; wind off -> settles and sleeps.
2. **Cape on a character**: Cloth on an animated character (locomotion clip). Walk, run, turn: cape trails, never passes through the torso or legs (kinematic ragdoll proxy), follows the root when the character is teleported (hard skin, no streak); `kClothNoCharacterProxy` shows the cape sinking into the body (proves the proxy is doing the work).
3. **Curtain / tarp on a prop**: moves with its entity (cart on a kinematic path); entity rotated 45 degrees keeps its rest shape (the old scale/rotation limit is gone).
4. **Distance**: walk away until `freezeDistance`; the cape holds its last pose, no CPU cost in the profiler row; return -> resumes without a pop (hysteresis).
5. **Metal door dent**: Destructible metal, `Hit` button then a thrown crate: a permanent dent that stays after the crate is gone, edge-strain overlay shows red then white set; a second hit deepens it; **Repair** restores.
6. **Glass pane**: shoot/`Explode`: shatters into several shards that fall and bounce on the floor, tiny ones vanish as dust (particle burst via the `SoftBodyDust` event graph), pieces despawn by shrinking after ~12 s; count never exceeds `maxLivePieces`.
7. **Wood plank crate**: a heavy hit snaps planks along the grain; broken edges magenta in the overlay; the render mesh opens along the break with no stretched triangles (dead triangles hidden) and UV seams intact.
8. **Cloth tear**: tearable tarp pulled by a graph `SoftBodyTear` + gravity: tears, the unpinned half falls, velocity continuous (no stall at the rebuild frame; check with 1 frame timing capture).
9. **Cape release**: `ReleaseSoftBody` node on a cape: it falls off the shoulders and lands as debris, collides with the floor and sleeps.
10. **Editor authoring**: Add Component > Cloth and > Destructible on a placed mesh; presets change behaviour live in Play; paint pins on a banner with the brush and with a rule; save, reload: pins and thresholds persist, runtime damage does not.
11. **Strain overlay** toggles and the budget text (`pieces/active particles/rebuilds`) match the profiler.
12. **Backends and hosts**: repeat 1, 2, 5, 6 on `--backend vulkan` and in the packaged `AverEngineRuntime.exe` (soft entities draw simulated, not authored; bounds not culled when a shard flies off-screen edge and back).
13. **Budgets**: 30 cloth flags + 10 glass panes on screen, GPU idle: frame time cost of the `SoftBody` profiler row stays under 2 ms CPU at default budgets; shatter 10 panes in one frame -> rebuilds spread over frames (no hitch over 8 ms).

## 7. Open questions (defaults chosen; proceed unless told otherwise)

1. **Self-collision** is not in v1 because Jolt 5.6 has none. Default: ship without; layered garments are separate cloths. A later CPU vertex-vertex spacing pass (velocity-only, spatial hash) would be a separate feature.
2. **Retire the Verlet solver and the Window > Soft Body panel** in phase G. Default: yes (no parallel solver). Say no to keep the plastic panel as a lab.
3. **Vertex colour** import is not added; paint, rules and mask textures cover it. Default: no importer change.
4. **No decimation / sim proxy**: a mesh with more than 4096 welded particles is refused (author a lower-poly cloth mesh or a proxy mesh asset). Default: refuse with a clear log and Details warning. A Deform-cage proxy would be a follow-up.
5. **Damage state is not saved**: a loaded or restarted level starts intact. Default: transient (no `.ocsave` schema change).
6. **Layers 14 and 15** are reserved by default for cloth/debris and character proxies and are `SoftBodySystem::Options`. Default: as stated; a project that uses them already changes the two numbers.
