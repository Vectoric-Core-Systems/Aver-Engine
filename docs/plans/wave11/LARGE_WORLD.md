# Large-world coordinates through chunk streaming: one position type, a floating origin, cooked authored chunks (wave 11, LARGE_WORLD)

Design contract for the implementing agents. **Owner decision (2026-10-25):** wave 11's large-world coordinates go THROUGH CHUNK STREAMING, using the three-tier coordinates already designed in `docs/CHUNKS.md` section 3 (region i32 / chunk within region / f32 local offset; 16 m chunks, 1024^3-chunk regions, region 0 centred on the origin) and CHUNKS.md slice 10 (floating origin, X and Y only, region-anchored, `prevViewProj_` compensation, the `PerFrameCB` change). **Jolt stays single precision.** This REPLACES section 2.10 and phase H of `docs/plans/wave11/WORLD_STATE_DEBUG.md` (`JPH_DOUBLE_PRECISION` plus a separate floating origin); section 8 lists exactly what is superseded. Slices 0 to 9 of CHUNKS.md are done. Nothing here is built or run. Rules in force: upgrade the existing systems and never add parallel ones, no new third-party code (this area needs none), lean comments, D3D12 and Vulkan parity, never delete files, ABI changes additive (new functions, new headers), headless ctest where possible, headed manual runs by the owner, never rebuild over the owner's running editor. Format and depth follow `SHAPES_MATERIALS.md` and `WORLD_STATE_DEBUG.md` in this folder.

**How the facts below were established (2026-10-25, branch `08-gdint`, HEAD `bdf8202d`).** Two read-only surveys fed this document (A: streaming and levels, B: consumers of absolute positions). The author re-read the load-bearing claims in the tree: `ChunkCoord.hpp`, `ChunkPayload.hpp/.cpp`, `ChunkStreamer.hpp`, `OcWorld.cpp` `num()`, the `OcWorldPlacement` struct, `LevelInstance.cpp` (the f64 narrowing), `FrameConstants.hpp`, `Convert.hpp`, `PhysicsJoints.cpp` (`createJoint`, `g_joints`), `PlayMobility.hpp`, `VoxiRenderer.cpp::setVolume`, and the vendored Jolt (`BodyInterface.cpp`, `PulleyConstraint`, `CharacterVirtual.h`, `BroadPhase.h`). Section 1 items tagged **(S)** come from the surveys only and carry survey line numbers; line numbers are pointers, grep the symbol when one is off. **UNVERIFIED** marks anything reasoned from reading but not confirmed by running it. Every number that is a tuning value is an UNVERIFIED starting value until the headed pass in section 6. No cost in this document was measured; costs are complexity classes plus numbers read from source.

**Scope.** (1) The world position type. (2) The floating origin and everything that must move or be compensated when it shifts: scene, hosts, gameplay systems, renderer (both backends), physics (single-precision Jolt), audio, particles, editor. (3) Cooking an authored level into region files and streaming it through the chunk streamer, so a level can be larger than one f32 frame. (4) Level files, saves, snapshots (CHUNKS.md B8). (5) The C#, graph and C ABI surface. (6) The editor's part. **Not here:** double precision anywhere in Jolt; networking (`modules/net`, `netvehicle`, `match` are README stubs, so only the rules a future netcode must follow are stated); landscape section tiling, navigation-grid tiling and per-cell foliage origins (named in 2.9 as v1 limits); actor despawn on leaving all loaded range (CHUNKS.md 8.3 and B7 still stand); a second absolute frame beyond roughly 100,000 km.

---

## 0. Decisions in one screen

1. **One position type: `WorldPos` = global chunk index (i32 x3) + f32 offset inside the chunk (`[0,1600)` cm).** It is the existing `SplitPos` of `ChunkCoord.hpp`, kept as the only authority; the region (i32 x3) and the chunk-within-region (`[-512,511]`) are derived views, so the three tiers of CHUNKS.md 3.1 are stored as two numbers and read as three. Precision is 1.2 micrometres everywhere, against 0.625 mm for "region + region-relative f32" (2.1). Authoring text and saves carry f64 absolute centimetres (exact to 2^53 cm); everything per-frame (scene, physics, render, audio, ABI) carries **origin-relative f32 centimetres**. `WorldPos` is header-only and Core-only (`aver/world/WorldPos.hpp`), no I/O, like `Vec3`.
2. **The floating origin is region-anchored, X and Y only, in steps of 1,638,400 cm (16.384 km).** The step is 1024 chunks x 1600 cm; the PLAN TEXT'S "16,384 cm" is a unit slip (section 1.6). Z is never shifted (`averAtmoCamAlt` reads `gCamPos.z`). It rebases when the camera is more than **921,600 cm** from the origin (region half-width 819,200 + a **102,400 cm hysteresis margin**), to the nearest region centre; the dead band against flipping back is 204,800 cm (2.05 km); the sub-millimetre cliff is 1,048,576 cm, so every resident object stays inside it. A 100 km flight rebases six times (9.2, 25.6, 42.0, 58.4, 74.8, 91.1 km).
3. **One service, one slot, two phases.** `world::WorldOrigin` (Aver.World, always built, no Scene dependency) owns the origin, the generation counter and an ordered participant list. A shift runs at the top of `onUpdate` in both hosts, before input, gameplay, the physics step, streaming and the scene flush: every participant first answers `prepare` (it may refuse, the shift is retried next frame), then all run `commit` (cannot fail). Order: physics, scene roots, host members, scene systems, streaming, renderer, audio and particles, editor, script event (2.2).
4. **Physics: single-precision Jolt in the origin-relative frame, shifted in the same slot.** The shift moves every body with `BodyInterface::SetPosition(..., DontActivate)` by an offset that is a multiple of 16,384 m, so it is exact (Sterbenz) for everything within 1 km of the camera and at worst half an ULP (0.625 mm) for far bodies. Characters move through `CharacterVirtual::SetPosition` plus `RefreshContacts`. A joint to the world gets an unadded static **anchor body** instead of `Body::sFixedToWorld`, which makes the constraint itself shift-proof; pulleys (absolute fixed points) are rebuilt from `GetConstraintSettings()`. `Convert.hpp` does not change, there are no `_d` entry points, no precision switch, no `RVec3` migration. One new header, `physics_origin_abi.h`. A benchmark gates the work (2.3.7).
5. **Renderer: a `shiftWorld` entry on the device and on Voxi.** `prevViewProj_` is composed with the shift translation in f64 (device, Voxi's `prevViewProj_` and `prev2ViewProj_`, the froxel fog copy); `PerFrameCB` grows from 752 to **784 bytes** (`worldOrigin`, `wavePhase`); cloud noise is fixed by folding the f64 origin into `cloudMotion.xy` on the CPU (the claim in WORLD_STATE_DEBUG that "`O mod noise period` keeps the sky bit-identical" is wrong, 1.6); wave phase gets a per-wave f64-reduced offset; Voxi's GI keys are re-adopted so there is no 25-frame GI rebuild; NRD2 `prevCamPos` shifts; the TLAS rebuilds once; ReSTIR reservoir history is dropped for one frame (2.2.6).
6. **Authored levels stream through chunks by being cooked.** A `cook` turns an authored level into region files (`.avrgn` v3, packed) plus an index under `<project>/Binaries/Chunks/<level>/`, reading **f64** `OcWorldData` and never the scene. The payload (codec v2) grows typed TLV extras so a chunk carries what a placement carries: placement id, animation, collision kind, COMPONENT records, outliner label, ground snap. The residency of a cooked level uses `ChunkStreamer` with the upgrades authored content needs (milliseconds budget, a prepare gate for mesh readiness, a column-occupancy scan instead of a Z slab, an AABB "wide set" for oversize items). The existing `LevelStreaming` stays the editor's machinery (it hangs off editable records) and is split into policy and mechanism so the cooked path reuses its mechanism; flat is the oracle (CHUNKS.md 2.1) and a test asserts chunked equals flat (2.4).
7. **Level text and saves stop losing precision (B8).** `num()` in `OcWorld.cpp` becomes shortest-round-trip; a placement the editor did not touch is written back from its original f64 record, never re-derived from f32; the loader subtracts the origin in f64 before it narrows (`LevelInstance.cpp` is the single narrowing site). `.ocsave` gains an origin header (additive chunk). Physics snapshots carry the origin generation (2.6).
8. **ABI is additive and origin-relative.** No existing signature changes. New: `physics_origin_abi.h`, `framework_origin_abi.h` (origin, generation, `WorldPos` converters), the event `world.origin_changed` through the existing `aver_fw_event_post`, C# `WorldPos`, `Double3`, `WorldOrigin`, `Entity.AbsolutePosition` (the plan's `Entity.WorldPosition (Double3)` collides with the existing float `Entity.WorldPosition`, 1.6) and a three-row Aver Node family (2.5).
9. **Editor.** Gizmos, picking and the camera stay in the local frame by construction. A rebase is deferred while a drag or paint stroke is active; undo/redo stacks, clipboard, Play-start snapshots and the collider overlay signature are rebased; `Show > World Origin` and the console variables `world.*` give a readout (2.7).
10. **Identity at origin zero.** With O = (0,0) and no content beyond 9.2 km, every changed path computes what it computes today (subtraction of zero) and the new constant-buffer fields are zero. A test pins this per area. Existing gates and levels are unaffected unless the camera leaves the 9.2 km window.
11. **Known v1 limits, said plainly (2.9):** level-global sidecars that are f32-absolute (`.ocland`, `.oclanes`, `.ocnav`, foliage tables, one landscape section) are valid within +-1,048,576 cm of world (0,0); chunk sizes other than 1600 disable the floating origin; Z is not rebased (altitude beyond +-10.48 km loses precision); nothing in the editor's record machinery is replaced.
12. **Four implementation areas with disjoint files:** ORIGIN (coordinates, service, scene, hosts, gameplay systems, files, ABI, C#, editor), PHYSICS, RENDER, COOK (authored cook and chunk streaming). Section 3.
13. **Supersedes** WORLD_STATE_DEBUG.md section 2.10 and phase H in full, and the "H lands LAST" rule: physics changes are now additive and no longer collide with the other wave 11 areas (section 8).
14. **Size.** ORIGIN L, PHYSICS L (benchmark-gated), RENDER XL, COOK XL, integration L. Overall **XL**; the four areas run in parallel after a one-day interface freeze (phase 0).

---

## 1. What exists

Notation: **(S)** = from the surveys only, not re-read; unmarked = read in the tree today.

### 1.1 Coordinates (`modules/world/include/aver/world/ChunkCoord.hpp`, header-only, Core-only)

- `kChunksPerRegionAxis = 1024`, local chunk index `[-512, 511]`, `kDefaultChunkSizeCm = 1600`, `chunkSizeValid` (512 x size < 2^20, so at most 2047 cm), `withinSubMillimetre`, `floorDiv/floorMod`, `ChunkCoord`/`RegionCoord`/`ChunkLocal` (the global i32 chunk index "is the identity everywhere", the region/local pair "is a VIEW of this, not a separate truth"), `regionOfAxis/localOfAxis` (+512 bias, region 0 centred on the origin), `packLocal/groupIndexOf/slotIndexOf` (6+4 bit split).
- `SplitPos { ChunkCoord chunk; Vec3 local; }` with `local` on `[0, chunkSizeCm)`. `splitCm(f64 x,y,z, size)` (the f64 overload "is the one the loader should use": floor in f64, remainder from the same floor, f32 only for the small part), `chunkOriginCmAxis` (i64, exact), `toWorldCm` (lossy beyond 10.5 km, says so), `toRegionRelativeCm(SplitPos, RegionCoord, size)` (every component within 8.192 km of the region centre; "the function slice 10 is built on"). A region spans chunks `[1024r-512, 1024r+511]`, so its centre, the chunk with local index 0, is at `r x 1,638,400` cm: that point is the origin of region `r`.
- There is **no** `WorldOrigin` type, no origin parameter in `capture`, `restore`, `ownerChunkOf`, `partitionWorld`, `StreamSource`, `PlacementStreamer`, and no floating-origin code anywhere (grep for WorldOrigin, rebase, floating origin finds only comments).

### 1.2 Chunk streaming today

- `ChunkPayload.hpp`: `PayloadEntity` = name, objectId, parent index (`-1` root; parents precede children), `Transform local` (root: chunk-local; child: parent-relative), tags, mesh (id, material NAME, flags, aabb), `hasBody` + box half extent. The whole file is inside `#if AVER_MODULE_SCENE` but the structs use Core types only; the gate is for `capture/restore`'s `scene::World`.
- `ChunkPayload.cpp::restore`: roots get `pe.local.position + static_cast<f32>(chunkOriginCmAxis(coord, size))` (lines 113-116) - absolute f32, the line that needs an origin. `RestoreOptions` has two hooks, `bindMaterial` and `createBody`; the return value of `createBody` is discarded (:148) and nothing sets `hasBody`, so streamed chunk entities have no collision today (S).
- `ChunkCodec` v1 (no string table, no palette, no compression: about 127 bytes per mesh entity with a 30-char asset name, S). `RegionFile` v2: 4 KiB sectors, two header copies with serial and CRC, sparse group table (64^3) over dense 16^3 group directories of 12-byte entries (48 KiB per touched group); `RegionWriter` mutates in place; the group-directory cache is an unbounded vector searched linearly (S). `RegionIndex` (`world.ocindex`: levelId, chunkSizeCm, worldSeed, generatorVersion, per-region hash and count). `generatorVersion`, `worldSeed` and `levelId` are written and **never compared** on open (S).
- `ChunkStreamer`: `StreamSettings` (loadRadius 3, evictRadius 5, loadBudget 2 and evictBudget 4 as **counts**, verticalRadius 1, lead 1.5 s); `StreamSource { Vec3 positionCm; Vec3 velocityCmPerSec; }` - **f32 absolute**; the wanted set scans a (2R+1)^2 x (2V+1) cube each time an anchor's chunk changes. `ChunkWorld` always owns a `GeneratedChunkSource` (an authored-only world is not expressible), a `RegionChunkSource` of overrides and a `LayeredChunkSource` in which an override wins the whole chunk. `GameStreaming` makes one `ChunkWorld` per non-Sky PCGVOLUME (radius clamp 24 chunks, S) and differences the camera against `lastPos_` for velocity. `GeneratedChunkSource` feeds f32 absolute world x/y to the noise (S).
- Tests: `ChunkCoordTest`, `ChunkPartitionTest`, `ChunkStreamTest` (chunked == flat over 9 regions), `RegionFileTest`, `RegionWriteTest`, `ChunkGenerateTest`, `ChunkActorStreamTest`, `ChunkWorldTest`, `PlacementStreamerTest`. None exercises an authored `.ocworld` with animation, components, level instances or sidecars.

### 1.3 Authored levels today (S)

Four residency mechanisms run side by side and share no key or grid: (1) `ChunkWorld`/`ChunkStreamer`, PCG scatter only; (2) `game::LevelStreaming` over `world::PlacementStreamer` (`Runtime/src/LevelStreaming.cpp`, 475 lines; `STREAM cell= load= evict= lazy= data=` header; uniform XY grid of `cellCm`, residency by XY distance to each placement's AABB, item budgets plus millisecond budgets, mesh prefetch gate, refcounted meshes, shared Jolt mesh shapes, kinematic bodies for animated placements, COMPONENT apply, pinning); (3) `LevelFoliage` cells; (4) `LevelInstances` (LEVELINST records, a second `PlacementStreamer` of instance extents). `docs/LEVEL_STREAMING.md` chose records over region files because "region files cannot carry what a placement carries (triangle collision, animation, classes, slot materials) and would need rewriting on every edit". The owner decision reverses the first half of that sentence (the payload will carry them) and keeps the second (the cook is derived data, rebuilt from the `.ocworld`, which stays the source of truth).

Real content (measured from the owner's projects, S): **NeonDistrict** top level has 685 LEVELINST records; its `c_*` cell files hold 602,028 PLACE records, no CHILD/BEGIN, no COMPONENT, no LIGHT; the union extent is about 16 km square (X -800,041..800,123 cm, Y -800,000..800,000, Z -600..51,128), i.e. it fits inside region 0 (+-819,200 cm); 281,378 occupied 1600 cm chunks, 2.14 placements per occupied chunk (145,449 chunks hold exactly one, max 42), 2,168 occupied 16^3 groups, exactly one occupied region. **Caldera_Capital**: 33,122 PLACE, 9 FOLIAGE tables, terrain tiles of hundreds of metres. No authored content needs more than region 0, so the 100 km flight can only be exercised by the PCG `ChunkWorld` or synthetic content (`BigWorldLab`, 2.4.9).

The fields a placement carries (`OcWorldPlacement`, read): `asset`, `objectId`, `material`, f64 `x y z`, `yaw pitch roll`, `sx sy sz`, `collide`, `visible`, `snapToGround`, `className` (graph-as-class actor), `name` (outliner label), `animClip/animSpeed/animTime/animOnce`, `vehiclePreset`, `parent` (index; children are parent-relative), `placementId`, `hasBounds/boundsMin/boundsMax` (f32 world AABB), `components` (type + ordered key/value strings, 20 types).

### 1.4 Where an absolute position lives today

| Layer | Site | Form today |
|---|---|---|
| Level text | `modules/formats/src/OcWorld.cpp:24` `num()` | `"%.6g"` for every number: 1 cm at 1 km, 10 cm at 10 km, 3 cm drift per save at 12.3 km (B8) |
| Level load | `modules/world/src/LevelInstance.cpp:383` | `xf.position = Vec3{(f32)p.x,(f32)p.y,(f32)pz}`: the single f64-to-f32 narrowing; child world transforms are composed in f32 (S) |
| Scene | `CLocal.xf` (`Components.hpp`), `CWorld.m` | roots absolute f32, children parent-relative; `World::setLocalPosition` (`World.cpp:789`) is the safe writer (raw pool writes need `touchLocal`) |
| Hosts | `GameApp.hpp:301 camPos_`, `:304 eye_`, `:351 giCenter_`; `SandboxApp.hpp:3390 camPos_`, `:3395 viewPosOv_`, `:3621 occlusionBasisCamPos_`, `:3760 camWanderBasePos_`, `:3771 camPosOverride_`, `:4112 sculptCursor_`, `:4347-4349 dronePos_/droneVel_/droneLastPos_`, `:1960-1982 pieCamRef_/vmRef_`, `:3856 giCenter_` (S) | the camera is a host member, not a root entity |
| Chunk path | `ChunkPayload.cpp:113`, `ChunkPartition.cpp:43,67`, `ChunkStreamer.cpp:80,128,146`, `ChunkGenerator.cpp:162,213` | f32 absolute sums and splits |
| Renderer | `rhi/FrameConstants.hpp` `PerFrameCB` (752 bytes, `static_assert`); `D3D12Device.cpp` `prevViewProj_`; `VoxiRenderer` `prevViewProj_`, `prev2ViewProj_`, `prevCamPos_`, GI centres, RT instance table, foliage rows, froxel `prevViewProj_`; cloud noise (`scene.hlsl`) and Gerstner waves sample absolute `wpos` (S) | absolute f32; `invViewProjRel` and `averViewRayDir` are already camera-relative |
| Physics | `Convert.hpp` `toJolt/fromJolt` (the one position funnel, about 130 call sites in 25 files, S) | cm to m in f32; Jolt built `DOUBLE_PRECISION OFF` |
| Systems that hold world positions across frames | `AnimSystem::ObjectBase.base`, `SeqBaseStore xforms_`, `CameraRigSystem` lag state, `SoftBodySystem::lastWorld`, `LevelInstance` `g_driven`, Synapse paths/hearing/tactics/smart objects/nav origin, `Mixer` voices, particles (CPU and GPU), `PlayMobility` slots | (S) |
| Saves | `.ocsave` (`OcSave.hpp`) | raw f32 `CLocal.position`, no origin field (S) |
| Sidecars | `.ocland` `originCm`, `.oclanes`, `.ocnav` `originXCm/YCm`, foliage `.ocinst` rows | f32 absolute (S) |

### 1.5 Facts the design leans on

- **Jolt single precision**: `physics.jolt/CMakeLists.txt` sets `DOUBLE_PRECISION OFF` (S); `RVec3` is `Vec3`. Vendored 5.6.0 has no shift-origin API (grep finds none). `BodyInterface::SetPositionAndRotation` (`BodyInterface.cpp:389`) does a per-body write lock, `SetPositionAndRotationInternal`, and notifies the broad phase **only if `body.IsInBroadPhase()`** - so it works on a body that was created and never added, and activates only when asked and only for broad-phase bodies. `BroadPhase::NotifyBodiesAABBChanged` takes an array but `BodyInterface` exposes no batch. `PulleyConstraint` stores `mFixedPosition1/2` as absolute `RVec3` with no setter, but the constraint can be re-expressed through `GetConstraintSettings()` (`mFixedPoint1/2`). `CharacterVirtual::SetPosition` also moves the inner body (`UpdateInnerBodyTransform`) and its active contacts hold world positions until the next update or `RefreshContacts`.
- **Aver joints**: `PhysicsJoints.cpp::createJoint` resolves handle 0 to `JPH::Body::sFixedToWorld`, builds the constraint with a world-space settings default, registers `Joint{constraint, bodyA, bodyB}` in `g_joints` and keeps **no creation arguments**. Points are given in the engine's world cm.
- **Layers**: `ObjectLayer = userLayer*2 + moving`; user layers 0..15 (`PhysicsInternal.hpp`).
- **`PlayMobility`**: a Static entity whose `worldRev` changed and whose world-matrix hash differs becomes **Movable, terminal for the session**, and Movable entities are excluded from the GI bake and the static TLAS key (about 33,000 mesh entities in NeonDistrict). A scene shift would reclassify every static entity forever.
- **`VoxiRenderer::setVolume`** treats an authored-centre move of more than 1 cm as a level load and requests a radiance-cache reset (`rc_.requestReset()`, `rcNet_.resetWarmup()`); the host calls it every frame with `giCenter_`.
- **Scene flush**: `World::flush` recomposes `CWorld` from `CLocal`; the shift writes roots through `setLocalPosition`, so children recompose by `composedParentRev` and rev-keyed caches (`WorldBoxEntry`, smart-object cache, light and decal gathers) refresh by themselves, once, at O(entities) (S).
- **`aver_fw_event_post`** posts a named event with one string argument (`framework_loc_abi.h` uses it for `loc.culture_changed`) so `AN_OnEvent` graphs react: the seam for `world.origin_changed`.

### 1.6 Corrections to the brief, to WORLD_STATE_DEBUG.md and to the surveys (verify before relying on them)

1. **STALE: "a flat .ocworld's PLACE lines are always instantiated whole".** Authored distance streaming already exists (`LevelStreaming`, 1.3) and is used by both hosts. The design must say what happens to it (2.4.1).
2. **Unit slip.** WORLD_STATE_DEBUG.md says the region-anchored origin snaps to "region origins, 16,384 cm apart". A region is 1024 x 1600 = **1,638,400 cm = 16.384 km** (its own 2.10.5 says "~16 km").
3. **No hysteresis in the plan.** "Rebase when the camera leaves the origin's region" makes a camera parked on a seam rebase every frame. A margin is specified (2.2.2).
4. **"The camera is a root entity" is false.** It is a set of host members (1.4).
5. **`Entity.WorldPosition` already exists** (`Aver.Framework/Entity.cs`, float, world-matrix translation); the plan's new `Entity.WorldPosition (Double3)` collides. The new member is `AbsolutePosition`.
6. **`PerFrameCB` is no longer hand-mirrored in three places.** One C++ struct (`FrameConstants.hpp`, size asserted) and one HLSL cbuffer (`shared_prelude.hlsl`), patched for Vulkan at `VulkanCommon.hpp` and covered by `CbufferLayoutPatchTest`. `VoxiFrame` (b4) is separate and needs no new field.
7. **The cloud claim is wrong.** `O_wrapped = O mod noise period, sky bit-identical` cannot hold: `averHash13` is `frac(p*0.1031)` (non-periodic) and the three octaves sample at `p`, `p*0.41`, `p*3.17`, so no period exists. The shipped fix is an f64-reduced CPU fold (2.2.6). "Bit-identical" in slice 10's done-when is restated as "identical within f32 rounding of the fold", which the headed check measures.
8. **Physics under single precision is not O(1).** WORLD_STATE_DEBUG 2.10's "a rebase costs physics O(1), moves no body, drops no warm start, snapshots are origin-independent" depended on doubles and is void (section 8).
9. **Networking does not exist** in the tree (stubs only); only the physics state ABI is net-adjacent.
10. **The 100 km acceptance flight cannot use NeonDistrict** (16 km square, one region). It needs the PCG chunk world and `BigWorldLab`.
11. **`D3D12Device.cpp:304` (cited in CHUNKS.md for cloud noise) is gone;** the shader is `scene.hlsl`. The CPU fold sites are `D3D12Device.cpp` (about :4777) and `VulkanDevice.cpp` (about :2584), which must be edited together (S).

---

## 2. Design

### 2.1 (a) The one world position type

**Chosen: `WorldPos` = global chunk index (`ChunkCoord`, i32 x3) + `Vec3 local` in `[0,1600)` cm.** It is `SplitPos` under a stable name. Header `modules/world/include/aver/world/WorldPos.hpp`, header-only, Core-only (it includes `ChunkCoord.hpp`), no I/O. The frozen interface (landed in phase 0 verbatim; other areas code against it):

```cpp
namespace aver::world {
inline constexpr i32 kWorldChunkCm  = kDefaultChunkSizeCm;                       // 1600; the floating origin requires it
inline constexpr i64 kOriginStepCm  = i64(kChunksPerRegionAxis) * kWorldChunkCm; // 1'638'400 = one region = 16.384 km
inline constexpr i64 kRegionHalfCm  = kOriginStepCm / 2;                         //   819'200
inline constexpr i64 kSubMmCliffCm  = i64(1) << 20;                              // 1'048'576: above this an f32 grid is 1.25 mm
inline constexpr i32 kRebaseMarginDefaultCm = 102'400;                           // 64 chunks, 1,024 m
inline constexpr i32 kRebaseMarginMaxCm     = i32(kSubMmCliffCm - kRegionHalfCm);// 229'376

struct Vec3d    { f64 x = 0, y = 0, z = 0; };
struct OriginCm { i64 x = 0, y = 0; };            // Z is never shifted. Both always multiples of kOriginStepCm.
using  WorldPos = SplitPos;                       // chunk + local, kWorldChunkCm

WorldPos    worldPosOfAbs(f64 xCm, f64 yCm, f64 zCm);                  // splitCm(.., kWorldChunkCm)
WorldPos    worldPosOfLocal(const Vec3& localCm, OriginCm o);          // splitCm(f64(local)+o): the add is in f64
Vec3        localOf(const WorldPos& p, OriginCm o);                    // f32(chunkOrigin - o, in i64) + p.local; the integer part is exact below 2^30 cm
Vec3d       absOf(const WorldPos& p);                                  // f64(chunkOrigin) + local; exact to 2^53 cm
f64         distanceCm(const WorldPos& a, const WorldPos& b);          // i64 chunk delta, then the local difference in f64
WorldPos    offsetBy(const WorldPos& p, const Vec3d& deltaCm);
RegionCoord regionOf(const WorldPos& p);                               // the derived view (ChunkCoord.hpp regionOfAxis)
}
```

**Why not the other candidates** (precision numbers are CHUNKS.md 3.3, re-derived):

| Candidate | Resolution | Verdict |
|---|---|---|
| **chunk i32 x3 + f32 local `[0,1600)`** | **1.2 micrometres at any distance** (f32 ULP in `[1024,1600)` is 2^-13 cm) | **Chosen.** It is the identity the streamer, the partitioner, the region files and the cook already use, so no conversion exists between a stored payload position and a `WorldPos`; the floor happens once, in `splitCm`. |
| region i32 x3 + f32 region-relative | 0.625 mm inside +-8.19 km, 1.25 mm to 16 km | Rejected as the stored form: 500 times coarser, and it needs the chunk identity re-derived by a floor at every streamer call. It remains the form `toRegionRelativeCm` already produces for the renderer's convenience. |
| f64 absolute | 2e-9 cm at 10^7 cm, 0.125 cm at 10^15 cm | Kept as the AUTHORING form (text, LEVELINST, sequence keys, `PlacementStreamer` bounds): diffs and tools think in absolute centimetres and an f64 holds them exactly to 2^53. Not the runtime form: chunk identity would need a floor per use, which is exactly the truncate-vs-floor bug class CHUNKS.md 3.1 removed. |

The three tiers of CHUNKS.md 3.1 are therefore stored as two numbers and read as three: `gc` (i32) is the chunk, `regionOf(gc) = (gc+512)>>10` the region, `localOfAxis(gc)` the chunk within the region, `local` the f32 offset. A `WorldPos` is 24 bytes (the same as a `Vec3d`).

**Where each form is used.**

| Form | Where |
|---|---|
| `WorldPos` | stream sources (`StreamSource` gains a `WorldPos` overload), wide-set owners, the `.ocsave` origin header, replication (future), the C# `WorldPos`, `world::WorldOrigin::absolute(Vec3 local)` |
| f64 absolute cm (`Vec3d`) | `.ocworld` records (`OcWorldPlacement`, LEVELINST, sequence camera keys), `PlacementStreamer` item bounds and viewers (new f64 path), `LevelStreaming` records, the cook's input, `Entity.AbsolutePositionCm` |
| **origin-relative f32 cm** | the scene (`CLocal` roots, `CWorld`), physics (through `Convert.hpp`, unchanged), the renderer, audio, particles, every `float*` crossing `scene_abi.h`, `framework_abi.h` and `physics_*_abi.h`, camera and gizmo state, undo transforms |
| chunk-local f32 | `.avrgn` payload root positions (unchanged: the only on-disk position scheme already independent of any origin) |

**The origin is a property of the scene frame, not of a chunk size.** Only `chunkSizeCm == 1600` is supported by the floating origin (`kWorldChunkCm`); a `.ocindex` with another size still streams but in a single frame with the old +-10.4 km limit and the origin disabled (open question 3).

**Conversions that matter** (all in `WorldPos.hpp`): `localOf(p, O)` subtracts in i64 *before* narrowing, so the integer part is exact (a multiple of 1600 cm is a multiple of 64, so 24 bits reach 2^30 cm), and the single f32 rounding happens on the sum with `p.local`, on a number of magnitude at most about 10.5 km for anything resident: 0.625 mm worst case, as `toRegionRelativeCm`. `worldPosOfLocal` adds in f64 and splits, so a camera at local `(1,000,000, 0, 0)` with `O = 3 x 1,638,400` is the exact position `5,915,200` cm.

### 2.2 (b) The floating origin

#### 2.2.1 Constants and invariants

- **I1.** `O.x`, `O.y` are always multiples of `kOriginStepCm`; `O.z` is always 0. (Z is not rebased: `averAtmoCamAlt` reads `gCamPos.z` as altitude over a 6360 km planet, and the CPU fog reference reads Z; both break if Z shifts. Altitude beyond +-10.48 km therefore loses precision; open question 14.)
- **I2.** After a committed shift the camera satisfies `|local| <= kRegionHalfCm + margin` per axis; under deferral it may exceed it for a few frames.
- **I3.** Every resident object is within `kRegionHalfCm + margin + R` of `O`, `R` the largest streaming radius (NeonDistrict evict 64,000 cm). Sub-millimetre is kept while `margin + R <= 229,376`. The service clamps `margin` to `[0, 229,376]` and logs once when a configured streaming radius breaks I3.
- **I4.** Every shift delta is a multiple of 1,638,400 cm = 16,384 m, exactly representable in f32 in both centimetres and metres.
- **I5. Exactness.** For `p` with `|p| >= delta/2` (that is, `|p| >= 819,200 cm` on a shifted axis) `p - delta` is exact in f32 (Sterbenz). The trigger puts the camera at or beyond 921,600 cm, so everything within `margin` = 1,024 m of the camera, ahead of it or behind it, qualifies: **nothing near the player moves by even a ULP.** Anything farther behind rounds by at most half an ULP of the result, 0.625 mm. The same holds in Jolt metres (`16,384 m`, `|p| >= 8,192 m`).

#### 2.2.2 When it shifts (hysteresis)

```
limit = kRegionHalfCm + margin                       // 921'600 cm at the default
per axis a in {x,y}:  if |camLocal[a]| > limit:  k = llround(camLocal[a] / kOriginStepCm);  delta[a] = k * kOriginStepCm
```

`llround` snaps to the nearest region centre, so a teleport of 50 km shifts three steps in one commit. Axes are independent. After a forward trigger at `c = 921,601` the camera sits at `-716,799` cm; the return trigger is `-921,600`, so flipping back needs **204,800 cm (2.05 km)** of travel in the opposite direction: a camera parked on a seam or jittering by a kilometre rebases at most once. A forward flight rebases every 1,638,400 cm: **a 100 km flight at O = 0 rebases at 9.216, 25.6, 41.984, 58.368, 74.752 and 91.136 km (six times)**, the seventh at 107.52 km. The decision uses last frame's camera (one frame of lag is far inside the margin).

#### 2.2.3 The service (`modules/world/include/aver/world/WorldOrigin.hpp`, `src/WorldOrigin.cpp`; Aver.World, no Scene dependency)

```cpp
enum class OriginPolicy : u8 { FollowCamera, Fixed, Manual };   // Fixed: lockstep/replay/net; Manual: tests and the console
struct ShiftEvent {
    OriginCm oldOrigin, newOrigin;
    Vec3d    deltaCm;          // newOrigin - oldOrigin; every local position changes by -deltaCm
    Vec3     deltaF;           // the same, f32 (exact by I4)
    u32      generation;       // the value AFTER the shift; starts at 0, +1 per commit
    bool     urgent;           // |camLocal| > 1'000'000 cm on a shifted axis: participants must not defer
};
struct PrepareResult { bool ok = true; const char* deferReason = nullptr; };
struct Participant {
    const char* name; i32 order;
    std::function<PrepareResult(const ShiftEvent&)> prepare;   // no mutation; may refuse
    std::function<void(const ShiftEvent&)>          commit;    // must not fail
};
class WorldOrigin {
public:
    struct Settings { bool enabled = true; i32 marginCm = kRebaseMarginDefaultCm; OriginPolicy policy = OriginPolicy::FollowCamera; };
    void  configure(const Settings&);                     // clamps the margin, logs I3 violations once
    void  reset(OriginCm o);                              // level load: set the origin when nothing exists yet; generation++ ; no participants run
    OriginCm origin() const;  u32 generation() const;  bool enabled() const;
    void  addParticipant(Participant p);                  // ordered by `order`, then registration
    OriginCm decide(const Vec3& camLocalCm) const;        // the delta 2.2.2 would apply (zero = none); pure
    bool  update(const Vec3& camLocalCm);                 // decide + shiftBy; false = nothing to do or deferred
    bool  shiftBy(OriginCm delta, bool urgent = false);   // prepare all, then commit all; false = a prepare deferred
    WorldPos absolute(const Vec3& localCm) const;         // worldPosOfLocal(local, origin())
    Vec3     local(const WorldPos& p) const;              // localOf(p, origin())
    struct Stats { u32 shifts, deferrals; f64 lastMs; f64 lastParticipantMs[32]; const char* lastDeferReason; };
    const Stats& stats() const;
};
}
```

`shiftBy` is the whole protocol: (1) validate `delta` (multiples of the step, Z zero) and `enabled`; (2) call every `prepare` in order, stop at the first refusal (record the reason, log at most once a second, return false: nothing has changed); (3) set the new origin, bump the generation; (4) call every `commit` in order, timing each into `Stats`; (5) return true. A participant must not read state another participant has already shifted in `prepare` (nothing is shifted yet) and must tolerate being called with the new origin already visible through `origin()` in `commit`. Registration lives in two places: the hosts' wiring file (`OriginParticipants.cpp`, ORIGIN) registers what the hosts own; the physics, renderer and streaming areas expose the functions it calls (3.4).

#### 2.2.4 The slot, and the order of effects

```
onUpdate (GameApp.cpp ~:2103, SandboxUpdate.cpp:37)
  S0  origin.update(camLocal)       prepare all, commit all          <- the only place a shift happens
  S1  input, gameplay groups, aver_phys_step ...                     (see one origin)
  S2  streaming ticks                (ChunkWorld, AuthoredStreaming, LevelStreaming)
  S3  World::flush                   (recomposes CWorld from the shifted roots)
  S4  render                         (device and Voxi already composed their history)
```

Quiesce rules (the `prepare` side): defer while a level is loading or unloading; while an editor drag, paint, sculpt or foliage stroke is active (the editor participant refuses, and when `urgent` it cancels the gesture restoring the pre-gesture absolute transforms and accepts); while Play is starting or stopping; while physics is inside `aver_phys_step` (cannot happen from the frame thread between steps, kept as an assertion). The shift is **never** run from a worker; workers see only origin-free data (a decoded `ChunkPayload` has chunk-local positions; a mesh prefetch has none), so an in-flight prefetch is unaffected by a rebase.

**Commit order** (`order` values; lower first):

| Order | Participant | Why here |
|---|---|---|
| 10 | PHYSICS | the only participant with a real refusal and the only expensive commit |
| 20 | SCENE roots, then PlayMobility | physics and scene must both be shifted before anything runs |
| 30 | HOST members (camera, eye, GI centre, drone, PIE refs, occlusion basis) | next frame's decide and render read them |
| 40 | SCENE SYSTEMS (anim base, sequencer, camera rig, soft body, hair, kinematic driver, Synapse, nav) | cached world-space state |
| 50 | STREAMING (GameStreaming, AuthoredStreaming, LevelStreaming, LevelInstances streamer) | velocity continuity; no re-keying needed |
| 60 | RENDER (device, Voxi, froxel, NRD2, landscape, foliage, occlusion) | after the scene so the GI re-key sees the shifted draws |
| 70 | AUDIO, PARTICLES | independent |
| 80 | EDITOR (undo, clipboard, PIE snapshots, overlay signature, gesture gate) | |
| 90 | SCRIPT EVENT `world.origin_changed` | scripts see a consistent world |

#### 2.2.5 What each system does on a shift (`delta` = new minus old; locals change by `-delta`)

| System | Action | Cost class | Area |
|---|---|---|---|
| Physics | `aver_phys_origin_shift` (2.3) | O(bodies) | PHYSICS |
| Scene roots | new `World::shiftRoots(deltaF)`: every entity with `CHierarchy.parent == kInvalidEntity` gets `setLocalPosition(p - delta)`; children are parent-relative and untouched; `World::originGeneration` bumps. Raw pool writes are forbidden (flush skips without `touchLocal`) | O(entities) scan, O(roots) writes, one O(entities) recompose in the next flush | ORIGIN |
| **PlayMobility** (trap) | `noteOriginShift(generation)`: the next `beginFrame` re-baselines every slot's `worldRev` and `worldHash` without counting a move. Without it every static entity becomes Movable for the session and leaves the GI bake and the static TLAS key | O(slots), 32 B each | ORIGIN |
| Host members | `camPos_`, `eye_`, `giCenter_` (GameApp); `camPos_`, `viewPosOv_`, `occlusionBasisCamPos_`, `camWanderBasePos_`, `camPosOverride_`, `sculptCursor_`, `dronePos_`, `droneLastPos_` (`droneVel_` is a velocity), `pieCamRef_/vmRef_`, `giCenter_` (SandboxApp) `-= delta` | O(1) | ORIGIN |
| AnimSystem | `ObjectBase.base.position -= delta` for roots (`writeObject` overwrites `CLocal` from `base` every tick; startInverse is animation-space and invariant) | O(animated) | ORIGIN |
| Sequencer | `SeqBaseStore xforms_ -= delta`; a sampled Transform/Camera key (authored f64, level-absolute) is converted with `- O` in f64 when written | O(tracks) | ORIGIN |
| CameraRigSystem | `lagPivot`, `prevTarget`, `lastTargetPos`, `lastPov`, LeadState `-= delta` (else the boom lags 16 km and snaps) | O(rigs) | ORIGIN |
| SoftBodySystem | `lastWorld` translation `-= delta` (else one hard re-skin via `teleportCm`); the palette formula `invBuildWorld` is entity-local and stays valid | O(soft bodies) | ORIGIN |
| Hair | previous root pose `-= delta` (else the 200 cm teleport rule fires: harmless, but avoidable) | O(hair) | ORIGIN |
| LevelInstance kinematic driver | `g_driven.pose.position -= delta` (else every animated kinematic body "jumps" and teleports, zeroing velocity and not carrying riders) | O(driven bodies) | ORIGIN |
| Synapse | nav grid `originXCm/YCm -= dx,dy` (O(1)); `AgentSystem` path points, tactics `squadTargets_`, `HeardMemory.pos`, smart-object slot positions `-= delta` | O(points) | ORIGIN |
| Vehicles, crowd, ragdoll, anim attachments | none: poses are re-read from physics or the entity each tick and self-heal in one frame, provided physics and scene shift in the same slot | - | - |
| GameStreaming | `lastPos_ -= delta` (else one huge velocity and a 64-step prefetch corridor); `ChunkStreamer` keys are absolute chunk coordinates and need no change: **zero loads and evictions on a rebase** | O(1) | COOK |
| LevelStreaming, PlacementStreamer, LevelInstances streamer | records and bounds are absolute f64 (new path); viewers are converted `local + O` each tick: no state to shift | O(1) | COOK |
| Renderer | device, Voxi, froxel, NRD2 (2.2.6) | see 2.2.6 | RENDER |
| Landscape, foliage | landscape `originCm` and per-node build origins; foliage resident rows re-pushed | O(nodes), O(resident instances) | RENDER |
| Particles | CPU: `SystemInstance.origin/prevOrigin`, `Particle.position`, `EmitterSim origin_`, queued spawn transforms `-= delta`; GPU: one extra compute dispatch per live instance (`gParticles` alive range, `gTrail` points, event records, `GpuSimHeader.origin`) | O(live particles) | RENDER |
| Audio | the 64-voice pool: `posX/posY -= delta` with relaxed atomic stores from the game thread (the pattern `aver_audio_set_voice_position` uses); entity-bound voices re-set their position every frame | O(64) | ORIGIN |
| Editor | undo and redo stacks, clipboard, `PlaySavedTransform`, `placedBaseline_`, collider and other cached overlay line meshes, gesture gate (2.7) | O(stack) | ORIGIN |
| Script event | post `world.origin_changed` (string `"dxCm,dyCm,generation"`) through `aver_fw_event_post` | O(1) | ORIGIN |
| Nothing to do (checked) | rev-keyed caches (`WorldBoxEntry`, smart-object cache), `drawsPrev_`, per-frame draw/light/decal gathers, `invViewProjRel`, `averViewRayDir`, `ptBias` (length of `p - gCamPos`), decals (world-minus-eye), light-grid hash (rebuilds itself), BLAS (object space), `PhysicsShapeCache` keys (no position), Jolt velocities and impulses, soft-body COM-local vertices, `characterCarry`, `VehicleEntry.lastVel` | - | - |

#### 2.2.6 Renderer, both backends

New in `rhi/WorldShift.hpp` (header-only, Core-only so it is unit-testable): `struct ShiftWorldInfo { Vec3d deltaCm; OriginCm newOrigin; u32 generation; }`, `composePrevViewProj(const f32 m[16], Vec3d delta) -> array<f32,16>` (f64 inside, one rounding out), `wavePhaseOffset(Vec3d dirXY, f64 k, OriginCm O) -> f32` and `cloudMotionFold(Vec3d wind, OriginCm O) -> {f32,f32}`. `IDevice` gains `virtual void shiftWorld(const ShiftWorldInfo&) {}` (non-pure, as `IResourceFactory::destroyBlas` was, so the mocks in `tests/render.ui` and `tests/render.actorpreview` keep compiling). The host calls `device.shiftWorld` and `voxi.shiftWorld` from one participant (order 60).

1. **`prevViewProj_` compensation.** Local positions satisfy `x_old = x_new + delta`, so the previous matrix, which maps old-frame world to previous clip, becomes `prevVP' = T(+delta) * prevVP` in the row-vector convention the shaders use (a new-frame point is moved into the old frame, then projected). Composition is done in f64 and rounded once. Applied to: the device's `prevViewProj_` (D3D12Device snapshots `frameCB_.viewProj` inside `setCamera`) and `taaLastViewProj_`; `VoxiRenderer::prevViewProj_` and `prev2ViewProj_`; `VoxiFroxelFog::prevViewProj_` (a **separate copy**, easy to miss); `fgPrevCamPos_ -= delta` (else `frameInterpCameraJumped` sees a 16 km jump and cuts NeuraFI). NRD2 and the FFX denoiser take `prevViewProjRel` (camera-relative, invariant) and a camera delta `eye - prevCamPos`: **`Nrd2` `prevCamPos` and `VoxiRenderer::prevCamPos_ -= delta`**, else NRD2 is fed a 16 km motion. Vulkan keeps no previous matrix (no TAA jitter or G-buffer velocity yet), so the device-level composition is D3D12-only today; Vulkan needs the cbuffer fields and the folds.
2. **`PerFrameCB` 752 to 784 bytes.** Two `f32[4]` APPENDED after `jitter` (so no existing offset moves): `worldOrigin` at 752 (xyz = O in cm, exact in f32 as multiples of 1,638,400; w = generation as a float, for debugging) and `wavePhase` at 768 (xyz = per-wave phase offset; w unused). Edit in lockstep: `rhi/FrameConstants.hpp` (struct, `static_assert(sizeof == 784)`, `offsetof(worldOrigin)==752`, `offsetof(wavePhase)==768`), `rhi/shaders/shared_prelude.hlsl` cbuffer (same position), `tests/rhi/src/CbufferLayoutPatchTest.cpp` expectations (the Vulkan cbuffer text patch at `VulkanCommon.hpp` is generic and the test covers it), the two device fill sites. `VoxiFrame` (b4) is untouched.
3. **Clouds.** The shader samples `p = (wpos + gCloudMotion.xy) * gCloudMotion.z`. With `wpos` now origin-relative, the CPU writes `cloudMotion.xy = f32(f64(wind.xy) + f64(O.xy))` at the two fold sites (`D3D12Device.cpp` ~:4777 and `VulkanDevice.cpp` ~:2584: both, in one commit). The sum `wpos_local + (wind + O)` equals the old absolute sum up to f32 rounding of the fold (at `O` = 100 km, 1 cm) and of `wpos`. No shader change, no field. The pattern is continuous across the rebase; it is not a bit-exact identity.
4. **Waves.** `averWavePhaseT = dot(w.xy, p)*w.z + t*w.w` and the Gerstner displacement in `water.hlsl` / `water_displace.hlsl` use absolute XY. Add `gWavePhase[i]` to the phase, computed per frame in f64 as `fmod(k_i * dot(dir_i, O.xy), 2*pi)`. The water pass has its own cbuffer (`water.hlsl:33-35`, S) that receives the same three numbers.
5. **Voxi histories.**
   - RT instance table: `carryPrevTransform` matches previous-frame rows by `memcmp` or nearest translation within `kRtMaxCarryCm = 2000`; after a shift nothing matches. Shift `rtCarryEntries_[*].world` translations by `-delta` so movers keep their motion vectors.
   - TLAS: `rtAccelDrawsKey` hashes static world matrices, every one of which changed: **one full TLAS rebuild and one instance/material table upload on the rebase frame** (BLAS are per mesh in object space and unaffected). Accepted; it is a one-frame cost every 16 km.
   - GI: `setVolume` treats a >1 cm centre move as a level load (radiance-cache reset). `VoxiRenderer::shiftWorld` shifts `authoredCenter_` **first**, then `center_`, `followLastEye_`, `giSnapCenter_`, `giBoxCentre_`, `giShadowCentre_`, `cascadeCentre_` and `cb_.voxelOrigin`, then recomputes `giDrawsKey_` and its sub-keys over the shifted draws and adopts them as the snapshot so `giSnapshotUnchanged` accepts. Without the re-key the staged rebuild takes about 25 frames on NeonDistrict (S). **Exit: the full-rebuild counter and the radiance-cache reset counter do not move across a rebase.**
   - NeuRaC: cells are keyed by integer world cell (sizes 25, 100, 400 cm, all dividing 1,638,400), so the texel `cell & 63` is shift-invariant but the 400 cm cascade's tag `(cell>>6)&255` moves by 64. Add per-cascade integer `rcOriginCell` (O / cell size) to `RcInfo` and key texel and tag by `cell_local + rcOriginCell`: the cache is then keyed by absolute cell and survives. UNVERIFIED which HLSL users assume absolute cells (`voxi_neurac_io.hlsli`): RENDER reads them before editing. Fallback if the plumbing is not clean: set `kFlagClearAll` once (full relearn of 56.6 MB accum and 25.2 MB cells, a visible noise burst).
   - ReSTIR reservoirs (`voxi_reservoir.hlsli`) and `giSurfPosHist_/giSurfNrmHist_` store old-frame world positions that `prevViewProj'` cannot fix: drop that history for one frame (`giHistValid_ = false`, flags at `VoxiRenderer.hpp` ~:1485). A shift pass over the buffers is the follow-up (open question 8).
   - RT shadow, reflection, AO and local-light histories store scalars and depth and reproject through `prevViewProj'`: valid. Light grid: keyed by a hash of light bytes, rebuilds itself on the rebase frame. Shadow cascades: texel-snap phase changes, one frame of shimmer, accepted. Path tracer: accumulation restarts (`ptAccumKey_`), accepted. Occlusion: `basisCamPos_` shifted or the basis invalidated (one frame of unculled objects).
6. **Landscape.** `OcLandData.originCm` and the `LandscapeTree` node bounds are rebuilt from the new origin (the code comment says "rebuilt rather than nudged"). Resident node meshes hold vertices in their build frame; each node records its build origin and `LandscapeRenderer::draw` applies `T(-(O_now - O_build))` through the `world` matrix it already takes, so no node rebuilds on the rebase frame and nodes re-mesh as LOD turns over. `terrainHeightAt` and `tileAt` take absolute f64 (or an i64 tile plus f32) so noise stays a function of absolute position; `groundHeightAt` for placements converts `local + O`. The physics heightfield body shifts as any body. Landscape tiling of several sections is not implemented in the tree (`GameLandscape.cpp` ~:258): a v1 limit (2.9).
7. **Foliage.** Resident cells' rows (`world[12]` per instance) are re-pushed from the CPU copy with `-= delta`; the TLAS rebuild is the one already counted. Foliage tables are f32-absolute authored data valid within +-1,048,576 cm of world (0,0) (2.9).
8. **Material graphs.** The `WorldPosition` node returns the origin-relative `v.wpos`; patterns driven by it (`averChecker`, `averValueNoise`) jump once per rebase. Documented, not fixed (open question 10); `averWorldUV` is object-relative and invariant.

All of the above is unmeasured. The rebase frame therefore costs, in order of the commit list: a TLAS rebuild and table upload, a light-grid rebuild, one froxel/denoiser history composition (O(1)), a particle shift dispatch per live instance and the scene's O(entities) recompose; the headed pass records each.

### 2.3 (c) Physics: single-precision Jolt in the origin-relative frame

#### 2.3.1 Frame model

There is **one frame**: the scene's. Jolt's world is the scene's frame in metres through the unchanged `Convert.hpp` (`toJolt(v) = (v.y, v.z, -v.x) / 100`). Physics shifts in the same slot as the scene, so `aver_phys_set_entity` write-back, every `aver_phys_body_position` call and every C# `Body.Position` mean exactly what they mean today. Engine delta `(dx, dy, 0)` cm is Jolt delta `dj = (dy, 0, -dx) / 100` m. There are no `_d` entry points (the host converts a `WorldPos` through `WorldOrigin::local` before it calls physics; a target outside the resident window is not simulated), no `AVER_PHYSICS_PRECISION` option, no `RVec3` migration, and `aver_phys_precision()` keeps answering `AVER_PHYS_PRECISION_SINGLE`.

Option considered and rejected: a physics origin `P` decoupled from the scene origin `O` with `(O - P)` added in `Convert.hpp`. It makes the scene rebase free for physics but runs the simulation at `|local + (O-P)|` up to 25 km (2 mm f32 grid) until the physics shift catches up, which re-creates the far-origin vibration the engine has hit before; and `P` can only be shared between peers if the simulation derives it, which is a netcode rule, not a physics mechanism (2.3.6).

#### 2.3.2 ABI: `physics_origin_abi.h` (new; `AVER_PHYS_ORIGIN_ABI_VERSION 1`)

```c
AVER_PHYS_API int32_t  aver_phys_origin_prepare(double dxCm, double dyCm, double dzCm);   /* -1 refuse (no world, inside a step, delta not a finite multiple of 100 cm), else the number of objects a shift would move; mutates nothing */
AVER_PHYS_API int32_t  aver_phys_origin_shift(double dxCm, double dyCm, double dzCm);     /* 1 = done, 0 = refused (same checks); between steps; every object moves by -delta */
AVER_PHYS_API uint32_t aver_phys_origin_generation(void);                                 /* +1 per shift; 0 at init and after aver_phys_rebuild */
AVER_PHYS_API void     aver_phys_origin_total(double* outXyzCm);                          /* sum of all shifts since init: the physics frame's origin in the engine's absolute frame */
AVER_PHYS_API int32_t  aver_phys_origin_stats(float* outMs, int32_t* outBodies, int32_t* outCharacters, int32_t* outAnchors, int32_t* outPulleysRebuilt, int32_t* outStaticsMoved); /* the last shift */
```

C#: `Aver.Physics/Native.Origin.cs` (partial of `Native`, auto-scanned by `AbiEnumTest`) and `PhysicsOrigin.cs` (`Generation`, `Total`, `LastShiftMs`). Nothing in `physics_abi.h` or the other headers is edited. The module-internal registry for other wave 11 areas: `physicsOriginHook(const char* name, void (*fn)(const Vec3d& deltaCm))` in `PhysicsOrigin.hpp`; any area that keeps a world position in module state registers one at init (force volumes and water volumes of CONTACTS_FILTERS and CHARACTER_RAGDOLL_VEHICLE, soft-body attachments with world targets, hair's previous root pose); the shift calls the hooks after the bodies move.

#### 2.3.3 The shift, in order (one call, between steps, frame thread)

1. **Validate** (`prepare`): world exists; not inside a step; `delta` is finite, a multiple of 100 cm per axis (so `dj` is a whole number of metres) and Z is zero.
2. **Capture pulleys**: for each pulley joint, `GetConstraintSettings()` BEFORE any body moves (its `mWorldSpacePosition` fields are last step's, in the old frame).
3. **Bodies**: for every handle in `World::bodies` (skipping any body that is a character's inner body, because `CharacterVirtual::SetPosition` moves that one): `BodyInterface::SetPosition(id, pos - dj, EActivation::DontActivate)`. Sleeping bodies stay asleep; static bodies move like any other (this is the broad-phase cost, 2.3.7). Soft-body vertices are COM-local and ride the body; sensors, kinematics, heightfield bodies, vehicle chassis, ragdoll parts and hair colliders are bodies.
4. **Anchor bodies** (2.3.4) the same way.
5. **Characters**: `CharacterVirtual::SetPosition(pos - dj)`, then `RefreshContacts` with the character's own filters, because its active contacts store world positions. **Not** `aver_phys_character_set_position` (it calls `RefreshContacts` and erases `characterCarry`). `characterCarry` is a velocity and stays.
6. **Pulleys**: remove the old constraint, add one built from the captured settings with `mBodyPoint1/2` and `mFixedPoint1/2` reduced by `dj`, swap it into the same `g_joints` entry (the handle number is unchanged), call `noteConstraintTopology()`. Lambdas are lost (a pulley is a rope: its rest length is in the settings).
7. **Engine-side state**: pending `ContactEvent.point`, `VehicleImpactRaw.point`, overlap and soft-body impact points are shifted in place (the queues are drained by the host between steps; shifting is simpler than draining); registered hooks run.
8. If the number of static bodies moved exceeds 4,096, schedule `aver_phys_optimize_broadphase` for the start of the next step (moving many static leaves leaves the static tree loose).
9. `originGeneration++`, accumulate `total`, fill `stats`.

**Contact caches and warm starts.** `SetPosition` does not invalidate contact caches (only `SetShape` and friends call `InvalidateContactCacheForBody`), and the cached manifolds store contact points relative to body 1, so resting contacts keep their warm start (UNVERIFIED: `PhysicsOriginTest` stacks a pile, shifts, and asserts no velocity spike and no wake). `SetPositionAndRotationInternal` resets the sleep timer, so a body about to sleep waits another half second; a sleeping body is untouched.

#### 2.3.4 Joints with a world endpoint

Today handle 0 is `JPH::Body::sFixedToWorld`, a dummy at the origin: a hinge to the world stores its anchor in that body's frame, i.e. as an ABSOLUTE point, and Jolt has no setter. After the bodies move the constraint would pull them back 16 km. The mechanism, chosen because it leaves every constraint object, every handle and the constraint topology untouched (so snapshots stay valid):

- In `createJoint`, when one side is the world, create an **anchor body**: a static body with a tiny sphere shape created with `BodyInterface::CreateBody` and **never added** to the broad phase (exactly what `sFixedToWorld` is: an unadded static). It is invisible to queries and collisions; `SetPosition` on it moves it and, because `IsInBroadPhase()` is false, notifies nothing. It is passed to `build` in place of `sFixedToWorld`.
- **Placement.** Every constraint type Aver builds in world space (`mSpace` defaults to `WorldSpace`: hinge, slider, fixed, distance, cone, swing-twist, six-DOF, point) converts the given world points to the anchor's local frame at creation, so the anchor goes at the other body's centre of mass with identity rotation and the stored local anchors stay small (a grep of `PhysicsJoints.cpp` finds no `mSpace` assignment, so each type keeps Jolt's default; `PhysicsOriginTest` item 6 proves it per type rather than trusting the grep). `PathConstraint` has no space option (its path is "in the space of body 1"): its anchor sits at the frame origin `(0,0,0)` with identity rotation so the engine's world points keep their meaning; after k shifts the anchor is at `-k * 16,384 m` and the sum `anchor + local` is exact where it matters (Sterbenz, `p >= 8,192 m`).
- `Joint` gains `anchorBody` (an invalid `BodyID` when none). Removal order: `RemoveConstraint`, then `DestroyBody`. `destroyAllJoints` (already run before the world dies) destroys them. The shift loop visits anchors explicitly, since they are not in `World::bodies`.
- **Pulley** is the one type with absolute state outside any body (2.3.3 step 6). Gear and rack-and-pinion constraints reference other constraints and need nothing.
- A joint between two real bodies is untouched: local frames shift with the bodies.

#### 2.3.5 Per-class table

| Class | On shift | Note |
|---|---|---|
| Dynamic, kinematic, static, sensor bodies | `SetPosition(.., DontActivate)` | exact within 1 km of the camera (I5) |
| Heightfield and mesh statics from streaming | same | O(statics) is the cost driver |
| Characters (`CharacterVirtual`) | `SetPosition` + `RefreshContacts` | skip the inner body in the body loop |
| Vehicles (wheeled, tracked, bike) | chassis shifts as a body | wheel contact points are recomputed each step; `VehicleEntry.lastVel` is a velocity |
| Ragdolls | parts are bodies | constraints are local-anchored |
| Soft bodies | ride the body | vertices COM-local; `SoftAttachment.targetCm` for world targets through a hook |
| Hair (Jolt 5.6) | head-space simulation; the engine feeds the root transform every frame | previous root pose through a hook |
| Joints, world side | anchor body (2.3.4) | |
| Pulleys | rebuilt | |
| Water volumes, force volumes, gravity volumes | hook shifts `surfacePosition`/centre | the global water plane is Z-only: invariant |
| Pending events with world points | shifted in place | |
| Snapshot rings | origin generation in the blob (2.3.6) | |

#### 2.3.6 Snapshots, the state hash, determinism, networking

Under single precision the blob and the state hash are **frame-dependent**. Rules: (1) the blob header (`PhysicsStateRecorder.hpp`) records the physics origin total and generation and the flag set it contains; (2) `aver_phys_snapshot_restore` with a stale generation restores, then applies `shift(current - blob)` to exactly the classes the blob contained (dynamic and kinematic bodies, characters, soft bodies; statics only when `SNAP_STATICS` was set; **anchors never**, because they are not in the blob and already sit in the current frame); (3) a restore crossing a pulley rebuild is refused by the existing topology epoch, which is correct and documented; (4) `aver_phys_state_hash` hashes frame-relative bytes, so the hash log line prints the generation and two logs are comparable only at equal generation. **Cross-peer determinism requires a shared physics origin.** `OriginPolicy::Fixed` (rebasing off; the play window is +-10.4 km around a fixed origin) is the lockstep and replay setting; a netcode that wants a large world must derive the shift from simulation state and record `(tick, delta)` shift events in its input stream so a replay reproduces the frames. Neither exists yet; this document only fixes the rule.

#### 2.3.7 Cost, and the benchmark that gates the area

The per-body loop is lock plus broad-phase notify per call (no batch API exists). Nothing is measured. **Phase P-0 is `PhysicsOriginBenchTool`** (a tool, not a ctest): scenes of 10,000 statics + 1,000 dynamics, 100,000 statics + 2,000 dynamics, and 500 sleeping stacks; report `origin_shift` ms in total and by class, `optimize_broadphase` ms, and a raycast batch before/after (loose-tree penalty). Decision rule: at most 16 ms for the first scene ships as designed; if the second scene exceeds 50 ms, apply the levers in order and re-measure: (1) physics residency radius smaller than the visual one for streamed statics (default off, project key `PHYSICS.STATICRADIUS`); (2) shift statics beyond 300 m from the camera one frame late, nearest first, with the late statics excluded from queries until moved (only if lever 1 is insufficient, because it adds a state); (3) streamed statics are destroyed and re-created in the new frame by the streamer's `BodyRegistry` instead of shifted. The owner is told the numbers before any lever is applied.

#### 2.3.8 What does not change

`Convert.hpp`, every existing `aver_phys_*` signature and meaning, shape handles and the shape cache (its key contains no position), velocities, impulses, forces, joint local anchors, `characterCarry`, `VehicleEntry.lastVel`, the C# `Body`, `CharacterBody`, `Vehicle` APIs. Scripts that cache a position in a field are the one class the engine cannot fix; they get `world.origin_changed` and `AbsolutePosition` (2.5).

### 2.4 (d) Authored levels streamed through chunks: the cook

#### 2.4.1 What happens to `LevelStreaming` (the question the brief leaves open): coexist, split into policy and mechanism

| | Source of truth | Residency | Used by |
|---|---|---|---|
| **Flat / editor** | the `.ocworld` (f64 text, editable) | `LevelStreaming` over `PlacementStreamer`, now with f64 bounds and viewers and origin-aware narrowing | the editor (edit-while-streamed: write-back, pins, undo, whole-level save, sequence binding by placement index, all unchanged) and any level without a valid cooked set |
| **Cooked** | derived `.avrgn` set under `Binaries/Chunks/<level>/` plus a slim `level.ocworld` | `ChunkStreamer` (upgraded, 2.4.6) with an authored host | the shipped game; the editor's optional `Play > Use cooked chunks` (read-only preview: chunk-owned entities are already treated as unselectable, unsaved scenery) |

Reasons. (1) The editor's record machinery (SandboxLevelStream.cpp: `entityLabels_`, `entityCollide_`, `entityAnim_`, `entitySnapZ_`, `entityBodies_`, write-back on evict, pins, `adoptNewLevelEntities`, `buildStreamedPlacements`, `appendStreamedSequenceTracks`) exists *because* the source is editable records; region files cannot be edited in place without a re-cook per edit, which is LEVEL_STREAMING.md's original objection and still true. (2) The shipped game needs random access to a level far larger than one f32 frame, which a flat text file cannot give: a 10^7-placement level cannot be parsed whole, and per-chunk payloads are origin-free. (3) CHUNKS.md 2.1 already settled that flat stays permanently as the oracle: `ChunkedEqualsFlat` for authored content (5) is the test that stops the two paths drifting. (4) World Partition shows the same split: editable source, derived runtime cells.

`LevelStreaming.cpp` (475 lines) is split: the **mechanism** (`loadItems`/`evictItem`: mesh acquire and release with refcounts, the shared Jolt mesh-shape cache, slot-material binding, kinematic recording for animated placements, COMPONENT apply, the after-load hook) moves into `game::ItemLoader` (new, `Runtime/include/aver/game/ItemLoader.hpp`); `LevelStreaming` keeps the **policy** (records, `PlacementStreamer`, pinning, editor write-back hooks) and calls `ItemLoader`. It is a behaviour-preserving move, pinned by the existing tests; the cooked path calls the same `ItemLoader`, so collision (triangle mesh, fitted box, pivot box), animators, kinematic bodies, slot materials and COMPONENT records need no second implementation.

#### 2.4.2 Chunk plumbing with an origin (applies to PCG scatter and to authored chunks)

- `RestoreOptions` gains `OriginCm origin`. `restore` places a root at `f32(chunkOriginCmAxis(coord, size) - origin.x) + local.x` (the subtraction in i64 BEFORE narrowing; Z uses origin 0). `capture`, `ownerChunkOf`, `partitionWorld`, `captureAll` gain an `OriginCm` defaulting to zero and compute `splitCm(f64(local)+origin)`. The baker never goes through these (it reads f64 `OcWorldData`).
- `StreamSource` gains a `WorldPos` form; the f32 form stays and is read as origin-relative. `ChunkStreamer::update(world, sources, OriginCm)` converts once with `worldPosOfLocal`. **Chunk keys are absolute i32 and `lastScanAnchors_` are chunk coordinates, so a rebase changes nothing the streamer remembers: the idle early-out still holds and no chunk loads or evicts because of a shift.** Sources farther than `kSubMmCliffCm` from `O` on X or Y are not streamed (one warning): the resident window is one f32 frame.
- `GeneratedChunkSource` feeds f32 absolute world x/y to `pcg::sampleInfinite` and the height source (`ChunkGenerator.cpp` ~:162, :213, S); past ~10 km the lattice `floor()` input quantises, so the same chunk could generate differently at different distances. The generator takes `(i64 chunk origin, f32 local)` and forms the lattice cell in i64/f64. `generatorVersion` bumps 1 to 2. Persisted overrides stay valid (an override replaces the whole chunk); an unmodified chunk may differ from the v1 generator in rare threshold cases. **`RegionChunkSource::open` and `ChunkWorld::open` finally compare `generatorVersion`, `worldSeed` and `levelId`** (written today, never read, S) and log a named warning.
- `ChunkPayload::restore` keeps the return value of `createBody` and calls `BodyRegistry::attach` (today the value is discarded, so `freedBodies` would be empty and bodies would leak on eviction as soon as `hasBody` is used, S).

#### 2.4.3 Payload v2: what a chunk carries

`kChunkCodecVersion` 1 to 2; the reader accepts both. Each entity record gains a TLV list (`u32 tag, u16 length, bytes`; unknown tags are skipped, so later additions never break old readers). Positions stay as in v1 (root chunk-local, child parent-relative).

| Tag | Content | Replaces |
|---|---|---|
| `PLID` | `u64 placementId` | stable identity for write-back, sequences, LEVELINST overrides (`objectId` is the mesh id, not the instance) |
| `ANIM` | clip path (string), `f32 speed`, `f32 time`, `u8 once` | `CAnimator` + kinematic body creation by the host, as `instantiate` does today |
| `COLL` | `u8 collide` (the authored flag) | the host's `ItemLoader` picks triangle mesh, fitted box or pivot box from the asset exactly as the flat path; v1's `hasBody`/`bodyHalfExtentCm` stay for scatter |
| `CMPN` | type, `u16 n`, n x (key, value) strings, verbatim `OcPlacedComponent` | the host applies them through `applyPlacedComponents` after restore (new `ChunkHost` hook); the canonical apply order is the host's |
| `LABL` | outliner label | `OcWorldPlacement::name` |
| `SNAP` | `u8 1` | `snapToGround`: the stored Z is the authored offset above ground; the host resolves ground at restore (`groundHeightAt`), as the flat path does |

Not chunk-owned and therefore **not in payloads** (the level-global set, instantiated at load from `level.ocworld`): placements with `className` (graph-as-class actors; CHUNKS.md 8.3/B7 forbid churning actors), `vehiclePreset`, targets of level-sequence tracks (`OcSeqTrack::target` is a placement index), anything the cook cannot bound. Also level-global in v1: LIGHT, DECAL, PREFABINST, water, landscape, post volumes, FOLIAGE tables, sidecars (2.9). The material override is the NAME already carried; slot materials of the mesh are bound by `ItemLoader`, not the payload. The cook must build each root's rotation with the same `quatFromEulerDeg` on the same f32 casts `instantiate` uses, or chunked-equals-flat cannot be bit-identical.

#### 2.4.4 Region files v3 (packed), the index, and where they live

NeonDistrict cooked at 1600 cm with the v2 layout would be 281,378 payloads of about 270 bytes each (2.14 placements x ~127 B, S) padded to 4 KiB sectors: roughly **1.15 GiB of sectors for 75 MB of data**, plus 2,168 dense group directories of 48 KiB (about 104 MB), and the group-directory cache is an unbounded vector searched linearly (up to 2,168 entries per `has()`). Chunk size cannot grow to help (`chunkSizeValid` caps at 2047 cm). The fix is a format variant, not a coarser unit:

- `RegionHeader` gains `unitBytes` (4096 for v2 behaviour, 16 for packed) and a `kPacked` flag; the directory entry's `firstSector` is read as `firstUnit`. A packed region is **cooked and immutable**: `RegionWriter` refuses it, so the free list and in-place mutation do not exist for it. Payloads are 16-byte aligned back to back, each with its own CRC32C as today.
- Packed regions use a **sparse group directory**: sorted `{u16 slot; u16 pad; u32 firstUnit; u32 byteLength; u32 crc32c}` (16 B) per occupied chunk, binary-searched; the 64^3 group table is unchanged. 281,378 entries are 4.5 MB instead of 104 MB. Group directories are cached in an LRU capped at 128 groups with a hash index (this also fixes the unbounded cache for the v2 path). Estimate (UNVERIFIED, from layout arithmetic): the NeonDistrict cook is about 90 MB of payload plus 5 MB of directories, in one region file.
- Region file naming (none exists outside tests): `r.<x>.<y>.<z>.avrgn` with signed decimals, in the level's cook directory.
- `RegionIndex` v2 adds `kind` (0 persisted overrides / PCG, 1 cooked authored), `sourceHash` (u64: hash of the flattened placement set, the codec version and the cook version), a `wide` list `{ChunkCoord owner; i64 minCm[3]; i64 maxCm[3]}` (2.4.6) and keeps `levelId`, `chunkSizeCm`, `worldSeed`, `generatorVersion`, per-region hash and count. The reader accepts v1. `levelId` of a cooked set is the FNV of the level stem.
- **Location: `<project>/Binaries/Chunks/<levelStem>/`** (`world.ocindex` keeps its name, the regions, `level.ocworld`). `scripts/stage-game.ps1` already copies `<project>\Binaries` recursively as-is minus `.pdb` (S, :389-400), so a cooked set ships with no packaging change. The existing PCG persisted overrides stay at `<project>\Chunks\<field>` (player and editor data; `LevelList.hpp:53` skips `Chunks`). The cook writes through a temp directory and a rename; the asset directory watcher must ignore `Binaries/Chunks` (it watches LAST_WRITE and would see the engine's own writes).
- Staleness: the level header record `STREAM ... chunks=<project-relative dir>` (`OcStreamSettings::chunksPath`, additive key). At load, if the source `.ocworld` exists and `hashPlacements`-style `sourceHash` differs, the cooked set is stale: warn, fall back to `LevelStreaming` (never serve stale). A packaged game that ships no source trusts the index.

#### 2.4.5 The cook

`world::cookAuthoredLevel(const fmt::OcWorldData&, const CookInput&, const CookOptions&, CookReport&)` in `modules/world/src/ChunkCook.cpp`. To let a Scene-free tool call it, the `#if AVER_MODULE_SCENE` gate of `ChunkPayload.hpp`, `ChunkCodec`, `RegionFile`, `RegionIndex` narrows to `capture`/`restore` and the scene-using classes (the structs use Core types only, 1.2). Driven by: the editor action **World Settings > Level Streaming > Cook Chunks** (beside Regenerate Streaming Data), `AverAssetC cook-chunks <level> --content-dir ... --project-dir ...` (if `AverAssetC` can link `Aver.World` without a GPU module: checked in phase C-0; otherwise a `--cook-chunks` mode of the headless host, whose first use is a phase exit because the headless path is untested), and the packaging step for levels that declare `chunks=` (open question 16).

1. Load the level and **expand LEVELINST in f64** (child level files, instance transform composed with each child placement, LIOVERRIDE applied). One shared function, `world::flattenLevelInstances`, extracted from what the runtime `LevelInstances` and `bakeLevelPackWithContent` already do (UNVERIFIED which of them owns the composition today: the implementing agent greps `LevelPackBaker.cpp` and reuses it, never a third copy).
2. Run the bounds bake (`fmt::bakeOcStream` logic: mesh AABB x transform per placement) so every placement has an f32 world AABB; a placement without bounds goes to the level-global set and is reported.
3. Classify each root: **level-global** (2.4.3), **wide** (AABB extent in X or Y greater than `wideCm`, default 6,400 cm = 4 chunks: terrain tiles, skyline buildings, bridges), else **chunk-owned**. Children follow their root.
4. Owner chunk = `splitCm(f64 x, y, z, 1600)` of the ROOT position. Never f32, never the scene (instantiating into `scene::World` and calling `partitionWorld`, as the tests do, goes through f32 `CLocal` and is exactly the B8 loss past 10 km).
5. Build `PayloadEntity` per placement (2.4.3), group by chunk, sort, write each region packed with `writeRegion` (byte-deterministic: two cooks `memcmp` equal), write the index (with `wide`), write the slim `level.ocworld` (everything except cooked placements and flattened LEVELINST records, `STREAM chunks=` retained).
6. Report: placements by class, chunks, regions, bytes, largest chunk, the wide list, unknown bounds, placements beyond +-1,048,576 cm of world (0,0) (fine for chunks; they warn for sidecars, 2.9).

#### 2.4.6 Streaming a cooked level

- `AuthoredChunkSource : IChunkSource` is a `RegionChunkSource` over the cook directory with no generator. `ChunkWorld::openAuthored(dir)` builds a world with an **optional** generator (today a `ChunkWorld` always owns one).
- `IChunkSource::forEachOccupied(const ChunkBox&, fn)` (default: scan with `has()`; region sources walk the group table and directories) is the **column-occupancy scan**: with `StreamSettings::verticalRadius = kAllLayers` the wanted set is `forEachOccupied` over the XY radius and the index's Z bounds, not a (2R+1)^2 x (2V+1) cube, so a tower whose floors span chunk layers -1..31 is found without a Z slab. PCG keeps `verticalRadius = 1` and its cube scan unchanged.
- **Budgets in milliseconds.** `StreamSettings` gains `loadBudgetMs` and `evictBudgetMs` beside the counts; authored defaults copy `LevelStreaming` (near load 12 ms, far 3 ms, evict 2 ms, item cap 128); PCG defaults are unchanged (counts only), so PCG is bit-identical.
- **Prepare gate.** New `world::IChunkHost { prepare(const ChunkPayload&) -> Ready|Pending|Failed; restore(...); evict(...); }` (`ChunkHost.hpp`). The streamer decodes a chunk, calls `prepare` (the authored host starts mesh prefetch and returns `Pending` until `meshReady`), keeps pending payloads in a bounded map, retries every update, and calls `restore` only on `Ready`. `Failed` records the chunk resident-empty as today. The default `ScatterChunkHost` wraps today's `restore()` and is bit-identical.
- **Oversize is honoured (`Partition::oversize` is filled and read by nothing today).** The index's `wide` list holds each wide item's owner chunk and absolute i64 AABB. The wanted set is the radius set UNION the owner chunks of wide items whose AABB is within the load distance of any source (XY distance to the AABB, the `PlacementStreamer` rule); a wide-owned chunk evicts by AABB distance with the same hysteresis. Chunk-owned items (extent at most `wideCm`) are covered by padding the radii by `ceil(wideCm / 2 / chunkSize)` = 2 chunks. `pin(ChunkCoord)` is added for sequences and tooling.
- **Authored host.** `game::AuthoredStreaming` (new, Runtime) owns the `ChunkWorld` and `AuthoredHost : IChunkHost`: `prepare` decodes the payload into placements (f64: chunk origin i64 + local f32, exact) and gates on `ItemLoader`'s mesh readiness; `restore` calls `ItemLoader::loadItems(batch, origin)`; `evict` calls `ItemLoader::evictItem` and releases the chunk's entity list. `GameLevel::load` chooses: valid cooked set then `AuthoredStreaming::begin`, else `LevelStreaming::begin` (never both for the same placements). `GameApp` ticks it where it ticks `level_.tickStreaming` (~:2297); `owns()` is exposed so the save path can skip streamed scenery.
- Scale check (NeonDistrict, 500 m load radius = 31 chunks, UNVERIFIED arithmetic from the survey totals): the window holds on the order of a thousand to a few thousand occupied chunks and a few thousand placements; a count budget of 2 per frame (today's default) would take 10 to 30 seconds to fill it, which is why the millisecond budget and the item cap are part of the design and not a tuning afterthought. The candidate scan (63 x 63 x 3 = 11,907 cells per rescan) becomes the occupancy query.

#### 2.4.7 What stays outside

Lights, decals, water volumes, post volumes, landscape, foliage tables, `.oclanes`, `.ocnav`, sequences, PREFABINST: level-global, loaded from `level.ocworld`, roots shifted with the scene. Lights and decals beyond +-1,048,576 cm of the load origin are skipped with one warning in v1 (a generality question, not a NeonDistrict blocker: it has none).

#### 2.4.8 Editor Play

`Play` keeps using `LevelStreaming` (edit fidelity, one source of truth). `Play > Use cooked chunks` (off by default) cooks if stale, then plays the shipped path; chunk-owned entities there are unselectable and unsaved scenery, which is already how the editor treats `ChunkWorld::owns()`. This is the editor's smoke test of the shipping path.

#### 2.4.9 `BigWorldLab` (the content the 100 km flight needs)

NeonDistrict is 16 km square and fits in region 0, so no authored content exercises a rebase. `scripts/gen-bigworld.py` generates `test-content/BigWorldLab/`: a level of about 400,000 placements along +-100 km of X (2,000 per km: lamp posts and block buildings of built-in primitives, no new art), a crate pile and a hinged door every 5 km, a spinning animated prop, one COMPONENT-carrying placement, one 500 m "bridge" (wide set), items straddling every region seam and chunk boundary at negative coordinates, a level-global class placement, and an infinite PCG field for scatter. Cooked by the cook; also the cook's scale test (`AuthoredCookTest` uses a generated 20,000-placement slice headless).

### 2.5 (e) C#, Aver Node and C ABI surface (additive only)

Rule: **every position crossing an ABI stays origin-relative f32**, exactly as today. Absolute values appear only through explicit new calls.

`framework_origin_abi.h` (new, `AVER_FW_ORIGIN_ABI_VERSION 1`, beside `framework_loc_abi.h`; backed by function pointers the hosts register so Aver.Framework gains no link dependency on Aver.World):

```c
AVER_FW_API int32_t  aver_fw_origin_get(int64_t* outXyCm);                              /* origin O (x, y) in cm */
AVER_FW_API uint32_t aver_fw_origin_generation(void);
AVER_FW_API int32_t  aver_fw_origin_enabled(void);
AVER_FW_API void     aver_fw_origin_to_local(double ax, double ay, double az, float* outXyz);   /* absolute cm -> origin-relative */
AVER_FW_API void     aver_fw_origin_to_abs(float x, float y, float z, double* outXyz);
AVER_FW_API void     aver_fw_worldpos_from_abs(double ax, double ay, double az, int32_t* outChunk3, float* outLocal3);
AVER_FW_API void     aver_fw_worldpos_to_abs(const int32_t* chunk3, const float* local3, double* outXyz);
AVER_FW_API int32_t  aver_fw_origin_request_shift(int32_t dxRegions, int32_t dyRegions);   /* console/test only; refused unless WORLD.ALLOWSHIFTREQUEST=1 */
```

The event: `aver_fw_event_post("world.origin_changed", "dxCm,dyCm,generation")` at order 90, so `AN_OnEvent` graphs and C# subscribers react without a new dispatch-table entry (the table has exactly ten entries and growing it bumps `AVER_FW_DISPATCH_VERSION`).

C# (`Aver.Framework`, new files `WorldPos.cs`, `WorldOrigin.cs`, `Native.Origin.cs` following the neighbouring `Native*.cs` pattern):

```csharp
public readonly struct Double3 { public readonly double X, Y, Z; /* + - * */ }
public readonly struct WorldPos {                      // chunk + f32 offset: 1.2 micrometres at any distance
    public readonly int ChunkX, ChunkY, ChunkZ; public readonly Float3 Local;
    public static WorldPos FromAbsolute(Double3 cm);  public Double3 ToAbsolute();
    public Float3 ToLocal();  public static WorldPos FromLocal(Float3 local);   // through the current origin
    public double DistanceTo(WorldPos other);          public (int X,int Y,int Z) Region { get; }
}
public static class WorldOrigin {
    public static (long X, long Y) Origin { get; }  public static uint Generation { get; }  public static bool Enabled { get; }
    public static event Action<Double3, uint> Changed;                // delta cm, generation: the cache-invalidation seam
}
partial class Entity { public WorldPos AbsolutePosition { get; }  public Double3 AbsolutePositionCm { get; }  public void SetAbsolutePosition(WorldPos p); }
```

`Entity.WorldPosition` (float, world-matrix translation) keeps its meaning: origin-relative. Documentation rule for scripts: a field that outlives a frame stores `AbsolutePosition`, not `Position`. `Character.cs` `SyncFromSimulation` writes `Self.SetLocalPosition(capsule pos)` assuming a root entity and equal frames; they stay equal because physics and scene shift in the same slot.

Aver Node family `WorldOrigin` v1 (the wave 9 C contract: `GraphWorldOriginNodeTable.cs`, `GraphNodeDefsWorldOrigin.hpp` mirroring pin for pin, owner methods as `internal static` wrappers in `Aver.Framework/WorldOriginGraph.cs`, `[AutoRun]` `WorldOriginNodeTests`, `docs/nodes/WorldOrigin.md` generated; nothing edited in `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs`, `Program.cs`). A graph float is f32, so the rows carry integers plus region-relative floats, never a bare absolute:

| Key | Kind | Pins (besides exec/then) |
|---|---|---|
| `an_getworldorigin` | Pure | out `regionX:i`, `regionY:i`, `generation:i`, `enabled:b` |
| `an_getworldregion` | Pure | in `entity`; out `regionX:i`, `regionY:i`, `regionZ:i`, `x`, `y`, `z` (region-relative cm) |
| `an_setworldregion` | Exec | in `entity`, `regionX`, `regionY`, `regionZ`, `x`, `y`, `z`; out `success:b` |

### 2.6 (f) Level files, saves, snapshots

1. **B8, the writer.** `OcWorld.cpp::num()` (`"%.6g"`) becomes `std::to_chars` shortest-round-trip for every number the writer emits (positions, bounds, angles, scales, distances); the in-tree precedent is `OcPrefab.cpp:28-33` and `OcLevelInst.cpp:15-20` (shortest of `%.9g`/`%.17g` that reads back exactly), so PREFABINST and LEVELINST already round-trip and PLACE did not. `FormatTest` round-trips `123456789.123` and `1e9 + 0.001` for PLACE, LIGHT, DECAL, SPAWN, CAMERA, PCGVOLUME, WATER, LANDSCAPE `at`, RIVERPT, LAKEPT, POSTVOLUME. The first save of an existing level rewrites numbers that were previously damaged by `%.6g` as their (already damaged) f64 values; no diff churn for values that were exact in six digits.
2. **The untouched-record rule.** Narrowing a record to f32 and widening it back is not an identity, so `saveLevel` must not re-derive an unchanged placement from the scene. Each placement keeps its loaded f64 record (the editor already keeps `placedBaseline_` and `LevelStreaming` already keeps `records_`); on save, if the entity's current local f32 equals `f32(record - O_at_load_or_shift)` the original f64 is written; if it changed, the absolute is `f64(O) + f64(local)` quantised to 1e-4 cm (1 micrometre, below the f32 grid at any position that matters) and written shortest. The same rule governs `onStreamedOut` write-back. Result: a level saved at origin `(3, 0)` after flying 50 km differs from the file on disk **only in the records the author edited**.
3. **The loader.** `world::InstantiateOptions` gains `OriginCm origin` (default zero; part of the phase 0 freeze so COOK can use it). `LevelInstance.cpp` (~:383, the single narrowing) computes `f32(p.x - origin.x)`, `f32(p.y - origin.y)`, Z as today, in f64; `groundHeightAt(p.x, p.y)` still receives the absolute f64 pair (the host function converts); child world transforms needed for body placement are composed in f64. `LevelStreaming`'s `localOf` (~:26-33) does the same. A flat (non-streamed) level with a placement beyond +-1,048,576 cm of the initial origin gets one warning: it will be quantised; stream it (CHUNKS.md 9's precision guard). **Initial origin:** `GameLevel::load` and the editor's level open choose the region containing the SPAWN record (game) or CAMERA record (editor); old levels, which sit in region 0, get `O = (0,0)`.
4. **Saves.** `OcSaveData` gets an additive chunk `ORIG` (`i64 x, i64 y`, written at capture; `OcSave.hpp`'s own comment names chunks as the additive mechanism). `SaveWorld` restore re-expresses every root `CLocal.position` as `f32(f64(saved) + (O_saved - O_now))`; a save with no `ORIG` chunk is origin zero, which is what it was. Separately and independently of large worlds, **neither host wires `CaptureOptions::skip` for chunk-streamed scenery** (GameApp sets only `softBodySaveSkip`): streamed scenery is saved as plain entities and becomes permanent on load while `ChunkStreamer::resident_` still holds the old handles (S). Both hosts wire the skip predicate to `ChunkWorld::owns()` and `AuthoredStreaming::owns()` in this wave, because a large world makes the leak large.
5. **Physics snapshots** carry the origin generation (2.3.6). **Replays and net**: record `(tick, delta)` shift events; replicate `WorldPos` or f64, never origin-relative f32.
6. **Sequences.** Camera keys (`OcSeqKey.v[9]`, f64 world x y z) stay absolute and are converted `- O` when sampled; Transform keys are in the placement's local frame and need no change.
7. **Sidecars** (`.ocland` `originCm`, `.oclanes`, `.ocnav`, foliage tables): v1 limit, 2.9. Loaders subtract the origin in f64 where they build runtime structures (nav `originXCm/YCm`, landscape `originCm`); files keep absolute values.

### 2.7 (g) Editor

- **Camera, gizmos, picking.** All live in the local frame by construction: `viewportRay` composes `invViewProjRel` with the translation of the (local) eye (`GameCamera.hpp::pushCamera`), gizmo state is read from entities each frame. The only hazard is state held across frames while a gesture is active.
- **Gesture gate.** `EditorOriginParticipant::prepare` refuses while a gizmo drag, marquee, sculpt or paint stroke or foliage stroke is active (those hold `before = o.pos` and similar local positions). When the event is `urgent` (camera beyond 1,000,000 cm) it cancels the gesture, restoring the pre-gesture transforms from its absolute copy, and accepts.
- **Undo and redo** (`EditCmd`: `EditXform` before/after, `AlsoMoved` locals, `EntitySnapshot` full `CLocal`, landscape and foliage stroke payloads): a commit walks both stacks and subtracts `delta` from root-level positions (children are parent-relative); stroke payloads that carry world positions are rebased by a hook their owner registers, those that carry grid indices are untouched. The clipboard `EntitySnapshot` likewise. Rejected alternatives: storing f64 in every entry (large, touches every edit site), and forbidding rebase in Edit mode (leaves the editor stuck at +-9.2 km while the runtime is not).
- **Play (PIE).** `PlaySavedTransform` and `placedBaseline_` `-= delta` for roots, so Stop restores into the current frame; streamed content reloads from f64 records and narrows fresh.
- **Overlays.** `colliderOverlaySig_` (SandboxViewport.cpp ~:85-121) gains `originGeneration`; every other cached world-space line mesh (nav bake, AI debug, soft-shape gizmos, decal gizmo, collision-shape overlay) is dropped on a generation change. Per-frame overlays read entities and need nothing.
- **Readout and controls.** `Show > World Origin`: region `(rx, ry)`, O in km, camera absolute position in km (computed in f64 from `WorldOrigin::absolute`), generation, shifts, deferrals and the last reason, last shift milliseconds with the per-participant breakdown; optional translucent bands at the region seam and the trigger distance when the camera is within 3 km of them. Console (appended to `EditorConsole.hpp`): `world.origin`, `world.rebase <dxRegions> <dyRegions>`, `world.margin <cm>`, `world.enable 0|1`, `world.policy follow|fixed|manual`, `world.tp <xKm> <yKm> <zKm>` (teleports the camera to an absolute position and shifts to it in one commit; the tool for the 50 and 100 km checks). Project keys `WORLD.FLOATINGORIGIN` (default 1) and `WORLD.REBASEMARGIN` (cm) in `OcProject`.
- **Not changed:** editing a cooked level. The cooked set is read-only derived data; "Cook Chunks" and the stale warning are the only editor surface for it.

### 2.8 Identity, cost, threads

- **Identity.** At `O = (0,0)` every touched path subtracts or adds zero in f64 before narrowing, which returns the same f32 as today for every value representable in f32 (the f64-to-f32 narrowing of `p.x - 0` is `(f32)p.x`); the new `PerFrameCB` fields are zero; the cloud fold adds 0.0; `ScatterChunkHost` is today's path; PCG budgets are counts. Each area ships an identity test (5). A level that never leaves the 9.2 km window never rebases. The one intentional behaviour change at zero is the PCG generator's lattice path (`generatorVersion` 2, 2.4.2).
- **Idle cost.** `decide` is four comparisons per frame.
- **Threads.** The shift is frame-thread only. Workers (mesh prefetch, payload decode, the cook) handle origin-free data. Nothing in this design touches `scene::World` from a worker (CHUNKS.md 11.8).

### 2.9 Known v1 limits (said plainly)

- Level-global sidecars are f32-absolute and valid within +-1,048,576 cm of world (0,0): `.ocland` (one landscape section; multi-section tiling is not implemented in the tree), `.oclanes`, `.ocnav` (one 2.5D grid for the whole level), FOLIAGE `.ocinst` tables (per-cell origin would be a format bump). Beyond that a precision guard warns and the feature works at degraded precision. Tiling each is future work.
- Only `chunkSizeCm == 1600` gets the floating origin (open question 3).
- Z is not rebased: altitude beyond +-10.48 km loses precision.
- A resident window is one f32 frame: stream sources farther than 1,048,576 cm from `O` on X or Y do not stream (a second player 40 km away needs a second frame: not designed here).
- A pulley in the world makes physics snapshots taken before a rebase unrestorable (the topology epoch refuses).
- Procedural materials driven by the `WorldPosition` node jump once per rebase.
- GI is noisier for a few frames after a rebase (ReSTIR history dropped once).
- Actor despawn on leaving all loaded range remains deferred (B7).

---

## 3. FILE OWNERSHIP

Four implementation areas with disjoint files. A file appears under exactly one area in 3.1 and 3.2; files that more than one area must edit are in 3.3 with the hunk each makes. Paths are exact where the file was read; "and its `.cpp`" means the implementer greps the sibling. The working tree already carries uncommitted wave 11 edits (physics, hair, soft body, formats); an edit to a file that `git status` shows modified waits for that area's commit or is rebased onto it.

| Area | Scope | Size | Needs from others |
|---|---|---|---|
| **ORIGIN** | `WorldPos`, `WorldOrigin` service, scene shift, hosts, gameplay-system rebase, level text, saves, loader, framework ABI, C#, graph nodes, editor | L + L (integration) | stubs of the three other areas' entry points (3.4) |
| **PHYSICS** | the shift, anchor bodies, pulleys, characters, hooks, snapshot generation, benchmark, C# | L | `WorldPos.hpp` for the C# docs only |
| **RENDER** | `PerFrameCB`, device and Voxi `shiftWorld`, folds, histories, GI re-key, NeuRaC key, particles, water, landscape, foliage, occlusion | XL | `WorldPos.hpp`, `WorldOrigin.hpp` |
| **COOK** | origin plumbing in chunks, payload v2, packed regions, index v2, the cook, streamer upgrades, `ItemLoader`, `AuthoredStreaming`, `PlacementStreamer` f64, `BigWorldLab` | XL | `WorldPos.hpp`, `InstantiateOptions::origin` |

### 3.1 Created (only the named area touches them)

**ORIGIN**
- modules/world/include/aver/world/WorldPos.hpp   (phase 0)
- modules/world/include/aver/world/WorldOrigin.hpp   (phase 0)
- modules/world/src/WorldOrigin.cpp   (phase 0)
- modules/world/include/aver/world/PlacementWriteBack.hpp   (phase 0; the untouched-record rule, header-only)
- modules/framework/include/aver/framework/framework_origin_abi.h
- modules/framework/src/FrameworkOriginAbi.cpp
- Runtime/include/aver/game/OriginParticipants.hpp
- Runtime/src/OriginParticipants.cpp   (registers every participant; the only file that calls the other areas' entry points)
- sandbox/src/EditorOriginParticipant.hpp, sandbox/src/EditorOriginParticipant.cpp, sandbox/src/WorldOriginPanel.cpp, sandbox/src/GraphNodeDefsWorldOrigin.hpp
- scripting/csharp/Aver.Framework/WorldPos.cs, WorldOrigin.cs, Native.Origin.cs, WorldOriginGraph.cs
- scripting/csharp/Aver.Graph/GraphWorldOriginNodeTable.cs; scripting/csharp/Aver.Graph.Tests/WorldOriginNodeTests.cs; scripting/csharp/Aver.Framework.Tests/WorldPosApiTests.cs; docs/nodes/WorldOrigin.md (generated)
- tests/world/src/WorldPosTest.cpp, tests/world/src/WorldOriginTest.cpp, tests/scene/src/SceneOriginShiftTest.cpp, tests/game/src/OriginParticipantsTest.cpp, tests/game/src/WorldFlightTest.cpp, tests/save/src/SaveOriginTest.cpp, tests/editor/src/EditorOriginTest.cpp

**PHYSICS**
- modules/physics/include/aver/physics/physics_origin_abi.h
- modules/physics/src/PhysicsOrigin.cpp, modules/physics/src/PhysicsOrigin.hpp   (shift driver, hook registry, anchor bodies' shift, pulley rebuild)
- scripting/csharp/Aver.Physics/Native.Origin.cs, scripting/csharp/Aver.Physics/PhysicsOrigin.cs
- tests/physics/src/PhysicsOriginTest.cpp, tests/physics/src/PhysicsOriginBenchTool.cpp   (a tool, not a ctest)

**RENDER**
- modules/rhi/include/aver/rhi/WorldShift.hpp   (header-only CPU math: composition, wave phase, cloud fold)
- modules/particles/shaders/particles_shift.hlsl
- tests/rhi/src/WorldShiftTest.cpp, tests/render.voxi/src/VoxiShiftTest.cpp, tests/particles/src/ParticleShiftTest.cpp, tests/landscape/src/LandscapeShiftTest.cpp

**COOK**
- modules/world/include/aver/world/ChunkHost.hpp   (`IChunkHost`, `ScatterChunkHost`)
- modules/world/include/aver/world/ChunkCook.hpp, modules/world/src/ChunkCook.cpp
- modules/world/include/aver/world/ChunkPayloadExtras.hpp, modules/world/src/ChunkPayloadExtras.cpp   (the TLV tags)
- modules/world/include/aver/world/AuthoredChunkSource.hpp, modules/world/src/AuthoredChunkSource.cpp
- modules/world/include/aver/world/LevelFlatten.hpp, modules/world/src/LevelFlatten.cpp   (`flattenLevelInstances`, shared with the pack baker if it owns the composition today)
- Runtime/include/aver/game/ItemLoader.hpp, Runtime/src/ItemLoader.cpp   (extracted from `LevelStreaming`)
- Runtime/include/aver/game/AuthoredStreaming.hpp, Runtime/src/AuthoredStreaming.cpp
- sandbox/src/SandboxLevelCook.cpp   (the editor action, `Play > Use cooked chunks`)
- scripts/gen-bigworld.py; test-content/BigWorldLab/   (generated level, project manifest, README)
- tests/world/src/ChunkOriginTest.cpp, ChunkPayloadV2Test.cpp, RegionPackedTest.cpp, AuthoredCookTest.cpp, AuthoredStreamTest.cpp; tests/game/src/ItemLoaderTest.cpp

### 3.2 Modified, owned by one area

**ORIGIN**
- modules/scene/include/aver/scene/World.hpp, modules/scene/src/World.cpp   (`shiftRoots`, `originGeneration`)
- Runtime/include/aver/game/PlayMobility.hpp, Runtime/src/PlayMobility.cpp   (`noteOriginShift`)
- modules/anim.scene/include/aver/anim/AnimSystem.hpp, modules/anim.scene/src/AnimSystem.cpp   (`rebase(delta)` on `ObjectBase`)
- Runtime/src/SequenceTracks.cpp and its header   (`SeqBaseStore`, absolute-key conversion)
- modules/camera/include/aver/camera/CameraRigSystem.hpp and its `.cpp`
- modules/softbody.scene/include/aver/softbody/SoftBodySystem.hpp, `SoftBodyCloth.cpp`
- modules/hair.scene (`HairSystem` previous root pose)   (after the SOFTBODY_HAIR commit; it is modified in the tree today)
- modules/world/include/aver/world/LevelInstance.hpp, modules/world/src/LevelInstance.cpp   (`InstantiateOptions::origin`, the f64 narrowing, `g_driven` rebase)
- Runtime/src/LevelInstances.cpp
- Runtime/include/aver/game/GameLevel.hpp, Runtime/src/GameLevel.cpp   (initial origin from SPAWN; `placementBounds`; `SpawnPoint`)
- Runtime/include/aver/game/GameApp.hpp, Runtime/src/GameApp.cpp, Runtime/include/aver/game/GameTick.hpp   (host members, slot S0, save skip predicate)
- modules/synapse.scene/include/aver/synapse/SynapseAgent.hpp, SynapseTactics.hpp, SynapseHearing.hpp, SynapseSmartObject.hpp, SynapseCrowd.hpp and their `.cpp`; the `Nav` runtime that owns `originXCm/YCm`
- modules/audio/include/aver/audio/Mixer.hpp and its `.cpp`; modules/audio.scene/include/aver/audio/AudioScene.hpp
- modules/formats/src/OcWorld.cpp (`num()`, writer, loader origin helper), modules/formats/include/aver/formats/OcSave.hpp, modules/formats/src/OcSave.cpp, modules/save/src/SaveWorld.cpp
- modules/formats/include/aver/formats/OcProject.hpp and `.cpp`   (`WORLD.FLOATINGORIGIN`, `WORLD.REBASEMARGIN`, `WORLD.ALLOWSHIFTREQUEST`)
- scripting/csharp/Aver.Framework/Entity.cs   (`AbsolutePosition` members)
- sandbox/src/SandboxApp.hpp, SandboxUpdate.cpp (slot S0), SandboxViewport.cpp (overlay signature, gesture flags), SandboxPlay.cpp, SandboxLevelEdit.cpp (`saveLevel`'s untouched-record rule), SandboxShell.cpp (`Show > World Origin`), EditorConsole.hpp (`world.*`)
- docs/plans/wave11/LARGE_WORLD.md   (section 9 as built; the only doc an area edits)

**PHYSICS**
- modules/physics/src/PhysicsJoints.cpp   (`Joint::anchorBody`, anchor creation in `createJoint`, destroy in remove and `destroyAllJoints`, pulley record)
- modules/physics/src/PhysicsState.cpp, modules/physics/src/PhysicsStateRecorder.hpp   (origin generation and flag set in the blob header; restore re-shift)
- modules/physics/CMakeLists.txt   (two sources)

**RENDER**
- modules/rhi/include/aver/rhi/FrameConstants.hpp, modules/rhi/shaders/shared_prelude.hlsl, modules/rhi/include/aver/rhi/RHI.hpp
- modules/rhi.d3d12/src/D3D12Device.cpp and its header; modules/rhi.vulkan/src/VulkanDevice.cpp (and `VulkanCommon.hpp` only if the cbuffer patch needs it)
- modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp, modules/render.voxi/src/VoxiRenderer.cpp, VoxiFroxelFog.hpp/.cpp, and the NeuRaC HLSL under modules/render.voxi (grep `rcOriginCell`'s neighbours: `voxi_neurac_io.hlsli`, `NeuRaCLayout.hpp`)
- modules/render.denoise/src/Nrd2.cpp and `Nrd2.hpp`
- modules/occlusion (`OcclusionCuller` basis invalidation)
- modules/particles (`ParticleSystem.hpp`, `EmitterSim.hpp`, `ParticleTypes.hpp`, the GPU sim host and its dispatch on both backends)
- modules/fluids (`water.hlsl`, `water_displace.hlsl`, `FluidVolume.hpp`)
- modules/landscape (`LandscapeRenderer`, `ChunkMesh`, `TerrainTile`, `LandscapeTree`), Runtime/src/GameLandscape.cpp, Runtime/src/GameFoliage.cpp and their headers
- tests/rhi/src/CbufferLayoutPatchTest.cpp

**COOK**
- modules/world/include/aver/world/ChunkPayload.hpp, ChunkCodec.hpp, ChunkPartition.hpp, ChunkSource.hpp, ChunkStreamer.hpp, ChunkWorld.hpp, ChunkGenerator.hpp, RegionFile.hpp, RegionIndex.hpp, PlacementStreamer.hpp, BodyRegistry.hpp and every `.cpp` of those names in modules/world/src
- Runtime/include/aver/game/GameStreaming.hpp, Runtime/src/GameStreaming.cpp, Runtime/include/aver/game/LevelStreaming.hpp, Runtime/src/LevelStreaming.cpp   (policy/mechanism split; `localOf` narrowing)
- sandbox/src/SandboxLevelStream.cpp, sandbox/src/SandboxLevelLoad.cpp   (origin-aware write-back and load; `STREAM chunks=` handling)
- tools/AverAssetC.cpp   (`cook-chunks`); scripts/stage-game.ps1 only if open question 16 is accepted
- tests/world/src/PlacementStreamerTest.cpp, ChunkStreamTest.cpp, ChunkGenerateTest.cpp, RegionFileTest.cpp, RegionWriteTest.cpp   (extended, never loosened)

### 3.3 SHARED (more than one area edits; every edit is small and additive)

| File | Edits | Order |
|---|---|---|
| modules/world/CMakeLists.txt | ORIGIN adds `WorldOrigin.cpp`; COOK adds its sources | append-only, any order |
| modules/formats/include/aver/formats/OcWorld.hpp, modules/formats/src/OcWorld.cpp | ORIGIN: `num()`, writer number paths, loader origin helper. COOK: the `STREAM chunks=` key (`OcStreamSettings::chunksPath`) parse and write | ORIGIN's `num()` lands in its first commit (day 1) so COOK's hunk rebases on it |
| Runtime/src/GameLevel.cpp | ORIGIN owns; COOK adds the 15-line choice "valid cooked set then `AuthoredStreaming::begin` else `LevelStreaming::begin`" at the `streamed` decision (~:158) and `streaming_.begin` (~:289) | COOK after ORIGIN's phase 1 |
| Runtime/src/GameApp.cpp, sandbox/src/SandboxUpdate.cpp | ORIGIN owns (slot S0, wiring). COOK adds the authored tick next to `tickStreaming` (~:2297, `tickLevelStreaming` :936) | |
| sandbox/src/SandboxApp.hpp | ORIGIN owns (participant members). COOK appends the cook-action members | append-only |
| tests/abi/src/AbiEnumTest.cpp | PHYSICS: `physics_origin_abi.h` rows. ORIGIN: `framework_origin_abi.h` rows | the `headers[]` array was widened to 24 by wave 11; widen again only if full |
| modules/physics/src/PhysicsInternal.hpp, PhysicsWorld.cpp | PHYSICS: `originGeneration`, `Joint` forward members, hook declarations; other wave 11 areas edit the same neighbourhoods | small additive hunks |
| modules/physics/src/PhysicsJoints.cpp | PHYSICS (anchor block in `createJoint`, about 40 lines); QUERIES_JOINTS edits the same file heavily | whoever lands second rebases; the anchor block is self-contained |
| docs: CHUNKS.md (slice 10 status, payload v2, packed regions), LEVEL_STREAMING.md (the cooked path), ABI.md, FORMAT_SPECS.md (`ORIG` chunk, index v2, region v3, `STREAM chunks=`), PROJECTS.md (`WORLD.*` keys), MODULES.md, SCRIPTING_API.md, ARCHITECTURE.md, CHANGELOG.md, WORLD_STATE_DEBUG.md (a one-paragraph "superseded by LARGE_WORLD.md" banner on section 2.10 and phase H) | the integrator, phase 4, one pass | |

### 3.4 Interfaces between areas (landed first, as stubs, so every area compiles at any time)

| Provider | Entry point | Landed |
|---|---|---|
| ORIGIN | `WorldPos.hpp`, `WorldOrigin.hpp` (service, `ShiftEvent`, `Participant`), `InstantiateOptions::origin`, `PlacementWriteBack.hpp` | phase 0, frozen |
| PHYSICS | `physics_origin_abi.h` (the five functions; stubs answer 0 until implemented), `physicsOriginHook` | first PHYSICS commit |
| RENDER | `rhi::ShiftWorldInfo`, `IDevice::shiftWorld` (default no-op), `VoxiRenderer::shiftWorld`, `ParticleSystem::shiftWorld(Vec3d)`, `Occlusion::invalidateBasis()`, `GameLandscape::onOriginShift`, `GameFoliage::onOriginShift` | first RENDER commit |
| COOK | `GameStreaming::onOriginShift`, `AuthoredStreaming::onOriginShift` and `owns()`, `ChunkStreamer::update(.., OriginCm)`, `PlacementStreamer` f64 overloads (additive; the f32 API stays until ORIGIN has moved `LevelInstances.cpp` over) | first COOK commit |

`OriginParticipants.cpp` (ORIGIN, phase 2) is the only caller of the PHYSICS, RENDER and COOK entry points; until then each area is tested through its own tests with fake participants.

### 3.5 Not touched

`Convert.hpp` and every existing `physics_*_abi.h`; `Native.cs`; `scene_abi.h`, `Components.hpp` (the shift edits `CLocal` values only), `framework_abi.h`; the vendored `modules/physics.jolt/Jolt/**`; `VoxiFrame` (b4); `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs`, `Program.cs`; `ChunkCoord.hpp` (all of slice 1 is reused as is); any new third-party code.

---

## 4. Phased implementation

Each phase ends with its tests green and the tree buildable. Implementing agents use Sonnet workflows with the fewest stages. Never rebuild over the owner's running editor; headed runs are the owner's. Sizes in parentheses.

**Phase 0: interface freeze (ORIGIN, S, about a day).** `WorldPos.hpp`, `WorldOrigin.hpp/.cpp` (service without participants), `PlacementWriteBack.hpp`, the `InstantiateOptions::origin` field (default zero), `WorldPosTest`, `WorldOriginTest`. Also the **narrowing audit**: grep every f64-to-f32 narrowing of a placement or level position and every consumer of an absolute f32 position (`static_cast<f32>(p.`, `Vec3{(f32)`, `camPos_`, `lastPos_`, `prevViewProj`, `giCenter`), list them in section 9. Exit: both tests and every existing suite green; the headers are frozen (a later change needs the other three areas' agreement).

**Phase 1: four areas in parallel** (each starts by landing its stubs, 3.4).

- **PHYSICS (L).** P-0 `PhysicsOriginBenchTool` and the numbers (2.3.7), recorded before anything else is built. P-1 `aver_phys_origin_prepare/shift/generation/total/stats`, bodies, characters, engine-side hooks, pending events. P-2 anchor bodies and the pulley rebuild. P-3 snapshot generation and restore re-shift. P-4 C#. Exit: `PhysicsOriginTest` green; existing physics suites unchanged (identity); bench table in section 9; the owner is told the numbers before any 2.3.7 lever is applied.
- **RENDER (XL).** R-1 `WorldShift.hpp` math and tests, `PerFrameCB` 784 with the HLSL cbuffer and `CbufferLayoutPatchTest`. R-2 D3D12 device composition, Voxi histories, RT carry, TLAS key, GI re-key, froxel, NRD2, NeuRaC key. R-3 Vulkan folds, waves, clouds. R-4 particles (CPU and GPU kernel, both backends), landscape per-node translation and f64 height, foliage re-push, occlusion basis. Exit: `WorldShiftTest`, `VoxiShiftTest`, `ParticleShiftTest`, `LandscapeShiftTest`, `CbufferLayoutPatchTest` green; identity (zero fields, zero fold) proven in tests; D3D12 and Vulkan build.
- **COOK (XL).** C-0 checks and the behaviour-preserving moves: can `AverAssetC` link `Aver.World`, un-gate the POD half, `ItemLoader` extracted from `LevelStreaming` with `LevelStreaming` tests unchanged. C-1 origin plumbing (`ChunkOriginTest`). C-2 payload v2, packed regions, index v2, LRU group cache, generator version check (`ChunkPayloadV2Test`, `RegionPackedTest`). C-3 `flattenLevelInstances` and the cook (`AuthoredCookTest`). C-4 streamer upgrades: ms budgets, `IChunkHost`, occupancy scan, wide set, pad, pin (`AuthoredStreamTest`). C-5 `AuthoredStreaming`, `GameLevel` choice, editor action, CLI, `Play > Use cooked chunks`. C-6 `gen-bigworld.py` and `BigWorldLab`. Exit: all listed tests green; **ChunkedEqualsFlat for an authored level**; existing world suites green.
- **ORIGIN (L).** O-1 `World::shiftRoots`, `PlayMobility::noteOriginShift`, the `rebase` methods of AnimSystem, sequencer, camera rig, soft body, hair, kinematic driver, Synapse, audio (each unit-tested alone), the loader origin option and the `LevelInstance.cpp` narrowing, `OcWorld.cpp` writer and `PlacementWriteBack`, `OcSave` `ORIG`, project keys. Exit: `SceneOriginShiftTest`, `OriginParticipantsTest` (fake participants), `FormatTest` additions, `SaveOriginTest` green; every existing suite unchanged at `O = 0`.

**Phase 2: integration (ORIGIN, L; starts when PHYSICS, RENDER and COOK exits are met).** `OriginParticipants.cpp` (the order table of 2.2.4), slot S0 in both hosts, deferral rules, the editor participant (gesture gate, undo walk, PIE, overlays), `world.*` console and `Show > World Origin`, save-skip wiring, C# and graph family, `AbiEnumTest` rows, `world.origin_changed`. Exit: **`WorldFlightTest` (100 km, six shifts)** green; full `ctest`; the graph suite via `dotnet run`; `NodeCatalogCheck`, `CsharpApiIndexCheck`.

**Phase 3: headed pass (the owner), then numbers.** Section 6. The implementing agents record the numbers into section 9 as built: the bench table, the rebase-frame milliseconds by participant, the cook size and time, resident counts. A tuning that changes a default (margin, `wideCm`, budgets) is a one-line edit made then.

**Phase 4: docs (integrator).** The shared documents of 3.3, `module-catalog.py --write`, `module-guard-audit.py`, `docs-check.py`.

---

## 5. Tests

Everything below is headless (no GPU, no window) unless marked. Each `*Test.cpp` is picked up by the root walk; a phase adds only an `add_executable` to its `tests/<module>/CMakeLists.txt`. Existing suites are never loosened; a tolerance is only ever added in a new test.

### 5.1 `tests/world` (ORIGIN and COOK)

- **`WorldPosTest`** (links `Aver.Core` only, like `ChunkCoordTest`): (1) `worldPosOfAbs(absOf(p)) == p` for exactly representable positions in regions -3..+3 and at +-10^8 cm; (2) `localOf(p, O)` against an i64 reference: the integer part exact, the sum within 0.625 mm for `|abs - O| <= 1,048,576`; (3) **`decide` table**: inside the limit none; 921,600 none; 921,601 one step; a 5,000,000 teleport three steps; negative symmetric; axes independent; Z never; (4) **dead band**: after a +1 step the camera at -716,799 does not shift, at -921,601 shifts back once; a camera oscillating +-1 km about a seam shifts at most once; (5) **100 km flight**: the shifts happen at 9.216, 25.6, 41.984, 58.368, 74.752, 91.136 km (+-1 cm), six in total; (6) **Sterbenz properties**: for 10^6 random f32 `p` with `|p| >= 819,200` `p - 1,638,400` is exact against double, and in metres for `|p| >= 8,192` and 16,384; for smaller `|p|` the error is at most 0.625 mm after the shift; (7) a root's absolute position `absOf(worldPosOfLocal(l, O))` is preserved across `(l - d, O + d)` exactly within the margin; (8) `PlacementWriteBack`: an unchanged entity writes back the original f64 bit for bit; a changed one is quantised to 1e-4 cm and round-trips through the writer; (9) margin clamp to 229,376 and the I3 warning once.
- **`WorldOriginTest`**: participants run in `order` then registration order, once each per shift; a `prepare` refusal changes nothing (no commit ran, origin and generation unchanged, `deferrals` +1, reason recorded, one log line per second); commit sees the new `origin()`; `Fixed` never shifts and `Manual` shifts only through `shiftBy`; `reset` runs no participant; `urgent` is set beyond 1,000,000 cm and reaches `prepare`; a delta that is not a multiple of the step or has Z is refused; per-participant milliseconds are recorded; `enabled=false` makes `update` a no-op.
- **`ChunkOriginTest`**: (1) `restore` with `O` in {0, +1 step, -2 steps}: every root's `localOf`-reconstructed absolute equals the capture's; (2) `capture` and `partitionWorld` with `OriginCm` zero equal the existing results bit for bit (identity); (3) **zero churn**: stream a flight, shift the origin between two updates (scene roots shifted by a fake participant): `loadedThisUpdate == evictedThisUpdate == 0`, the resident set and `lastScanAnchors_` are identical, the idle early-out still fires; (4) sources farther than the cliff are not streamed and warn once; (5) `GeneratedChunkSource` at 100 km: the same chunk generates the same bytes at `O = 0` and `O = 6 steps` and in a child process (extends `ChunkGenerateTest`); (6) `generatorVersion`/`worldSeed`/`levelId` mismatch on open logs the named warning; (7) `createBody`'s return value is attached and freed on eviction.
- **`ChunkPayloadV2Test`**: every TLV tag round-trips (`PLID`, `ANIM`, `COLL`, `CMPN` with 40 keys, `LABL`, `SNAP`); an unknown tag is skipped; a v1 payload decodes; a forward parent reference and trailing bytes are rejected as in v1; bytes per entity are reported.
- **`RegionPackedTest`**: a packed cook round-trips every chunk; two cooks with chunks fed in opposite orders are byte-identical; a damaged payload is refused naming the chunk; **truncation at every unit boundary never serves a wrong chunk** (the `RegionWriteTest` property); `RegionWriter` refuses a packed region; the group-directory LRU holds its cap of 128 across a sweep of 2,000 groups and `has()` is a binary search (asserted by a counting source); a synthetic 281,378-chunk cook stays under 100 MB of payload and 6 MB of directories (UNVERIFIED bound, the NeonDistrict arithmetic of 2.4.4).
- **`AuthoredCookTest`**: a generated level with placements at 0, +-1, +-3 region steps, at 1599/1600/-1/-1600 cm, on the seams +-819,200, with children several chunks from the root, a wide item (500 m), a `className` placement, a sequence-target placement, a nested LEVELINST with an override, and animation, `nocollide`, a COMPONENT and a label on one placement each. Asserts: (1) the owner chunk of every root equals the f64 floor reference; (2) **`ChunkedEqualsFlat` for authored content**: the flat path (instantiate with `origin` = region of the first placement) and the chunk path (every chunk resident through the authored host with a fake `ItemLoader`) produce the same entity set, names, transforms bit for bit, material names, animation fields, COMPONENT strings and collide flags; (3) cook determinism (`memcmp`); (4) a changed placement changes `sourceHash` and the set is reported stale; (5) `level.ocworld` contains exactly the level-global set; (6) the report counts; (7) a 20,000-placement slice cooks within a stated time bound (recorded, not asserted on a fixed number).
- **`AuthoredStreamTest`** (fake `IChunkHost`, fake clock): `Pending` becomes `Ready` after N updates and `restore` is never called before it; the millisecond budget is respected and loads are nearest first; the column-occupancy scan finds a tower spanning chunk layers -1..31 that a Z slab of 1 misses; **wide set**: an 800 m tile stays resident while a source is inside it although its owner chunk is beyond the evict radius, and evicts by AABB distance with hysteresis; padded radii keep a 60 m chunk-owned building whose origin is at the load edge; `pin` holds a chunk; a `Failed` chunk is recorded resident-empty; PCG defaults are unchanged (`ScatterChunkHost` equals the old path on `ChunkStreamTest`'s fixture).
- Extended, never loosened: `PlacementStreamerTest` (the f64 overloads equal the f32 results in region 0, and at 100 km the f32 path is shown to quantise while f64 does not), `ChunkStreamTest`, `RegionFileTest`, `RegionWriteTest`.

### 5.2 `tests/scene`, `tests/game`, `tests/save`, `tests/editor`, `tests/formats`

- **`SceneOriginShiftTest`**: roots shift, children unchanged (a 5-deep hierarchy), absolute positions preserved, `CWorld` recomposes in the next flush, rev bumps, entity count unchanged, `originGeneration` +1.
- **`OriginParticipantsTest`**: with fake participants and real systems: AnimSystem object base shifted (the animated root does not snap back on the next tick); the sequencer's base store and absolute camera key; camera rig boom does not lag; soft-body `lastWorld` (no hard re-skin); the kinematic driver (`g_driven`) does not teleport; **PlayMobility: 10,000 static entities stay Static after shift and flush**; Synapse nav origin, agent path, hearing memory; audio voice positions; `GameStreaming::lastPos_`; hair previous pose. A deliberately omitted participant makes the matching assertion fail (the negative control).
- **`WorldFlightTest`** (headless, no GPU; the integration test of phase 2): a scene, physics, a PCG `ChunkWorld` generating at 100 km and beyond, tracked entities, a sleeping crate pile, an awake pile, a character on a platform, a car, a hinged door anchored to the world, a pulley, level-global class placement. A camera flies +X 100 km at 200 m/s in fixed steps with `GameStreaming` ticking. Asserts: **six shifts**; tracked entities' absolute positions preserved to 1 mm (exact within the margin); the sleeping pile stays asleep with no velocity above epsilon; the door still holds (its hinge anchor did not pull it back); the character stays grounded; the car's speed is continuous; zero chunk loads and evictions attributable to a shift, and the resident chunk set (coordinates) and entity count identical immediately before and after every shift; `BodyRegistry` counts are consistent; PlayMobility's static count is unchanged; per-shift milliseconds are logged by participant (recorded, not asserted).
- **`SaveOriginTest`**: capture at `O1`, restore at `O2`: absolute positions preserved; no `ORIG` chunk is origin zero; the skip predicate omits streamed scenery (and with it removed the control saves it).
- **`EditorOriginTest`**: undo after a shift lands at the same absolute position; redo likewise; the clipboard; PIE `PlaySavedTransform` rebase; the gesture gate defers, then cancels and accepts when `urgent`; the overlay signature changes with the generation.
- **`FormatTest` additions**: `123456789.123` and `1e9 + 0.001` round-trip for every position-bearing record (2.6); a level whose numbers were exact in six digits writes byte-identical text; `STREAM chunks=` parses and writes; the index v2 reader accepts v1; `OcSave` `ORIG` chunk additive (an old reader skips it).
- **`ItemLoaderTest`** (the refactor's identity): `LevelStreaming` on a fixture produces the same entity set, bodies and mesh refcounts before and after the extraction.
- **`AbiEnumTest`**: `physics_origin_abi.h` and `framework_origin_abi.h` against `Native.Origin.cs` (names, parameter names, types incl. `int64_t`/`long`, `uint32_t`/`uint`).

### 5.3 `tests/physics` (PHYSICS)

`PhysicsOriginTest` (links `Aver.Physics Aver.Core`): (1) **identity**: a world that never calls the new functions is bit-identical in a 300-step scene to the same scene built before (the `PhysicsSettingsTest` identity technique); (2) **exactness**: 1,000 bodies within 1 km of a point at 9,216 m shift to exactly `pos - dj` bitwise, bodies 8 km away within 0.625 mm; (3) a 20-box pile that is **asleep stays asleep** with equal positions, an awake settling pile continues for 200 steps with no velocity spike and settles within 0.5 cm of an unshifted control translated by `dj`; (4) a character standing on a platform stays supported, `characterCarry` survives, no fall; (5) a vehicle driving through the shift keeps speed and wheel contact; (6) **a hinge door, a slider and a path constraint attached to the world hold** across the shift (the anchor body), swing limits unchanged; (7) a **pulley** is rebuilt: same handle, same rope length, `constraintEpoch` bumped, lambdas reset; (8) pending contact and vehicle-impact points are shifted; (9) registered hooks run in order after the bodies; (10) **snapshot restore across a shift**: capture, shift, restore: bodies end at the current-frame positions, anchors are not double-shifted, statics untouched without `SNAP_STATICS`; (11) refused: inside a step, Z non-zero, delta not a multiple of 100 cm; (12) 4,097 statics moved schedules `optimize_broadphase`; (13) `aver_phys_origin_total` accumulates and `generation` counts; (14) body count constant; raycasts and overlaps after the shift hit statics at the shifted coordinates; (15) the shape cache key does not change; (16) a cloth stays finite and stable; sensors still fire; (17) a character's inner body is not shifted twice. `PhysicsOriginBenchTool` is the P-0 benchmark (2.3.7).

### 5.4 Renderer (RENDER)

- `WorldShiftTest` (`tests/rhi`, CPU): `composePrevViewProj` equals the matrix rebuilt at `eye - delta` within 1e-6 relative for random cameras, and projecting a world point before and after agrees (the row-vector convention is pinned by a single hand-computed case); `wavePhaseOffset` continuity: phase at `p_old` equals phase at `p_new` with the offset mod 2*pi within 1e-3 over 10^5 points; the cloud fold: `wpos_new + fold_new` equals `wpos_old + fold_old` within 2 ULP of the sum; zero origin gives zero fields and a zero fold (identity). `CbufferLayoutPatchTest`: size 784, `offsetof(worldOrigin) == 752`, `offsetof(wavePhase) == 768`, Vulkan patched text lays out the same.
- `VoxiShiftTest` (CPU parts): `carryPrevTransform` keeps matches after the carry shift; the GI keys recomputed over shifted draws equal the adopted snapshot and `giSnapshotUnchanged` accepts; `rtAccelDrawsKey` changes exactly once and is stable on the next frame; the NeuRaC absolute-cell mapping (texel and tag) is invariant under a shift for the three cascade sizes; `Nrd2` `prevCamPos` shift gives a zero camera delta for a stationary camera across a rebase.
- `ParticleShiftTest`: CPU particles, origins and queued spawns shift; the GPU kernel's CPU mirror (the `RibbonPackTest` precedent) equals the HLSL arithmetic. `LandscapeShiftTest`: a node's draw translation equals a rebuilt node's vertices; `terrainHeightAt` at 100 km is independent of the current origin.
- **Needs a GPU, so headed (section 6):** the rebase-frame image comparison, the GI rebuild counters, the cloud and wave visual continuity, Vulkan parity.

---

## 6. Headed manual checks for the owner

Use `BigWorldLab` first, then NeonDistrict (it fits in region 0, so it is the identity check), then the vehicle lab. The GPU must be idle; screenshots at 1440p with a ping for remote runs; never over a running editor; test with the owner's real `editor.ini`. `world.tp` and `Show > World Origin` (2.7) are the tools.

1. **Readout and console.** `Show > World Origin` shows region, origin, absolute camera km, generation; `world.origin` prints it; `world.margin` and `world.enable 0|1` work; `WORLD.FLOATINGORIGIN 0` in the project disables rebasing.
2. **The 100 km flight.** In `BigWorldLab`, fly +X at 300 m/s (`--cam-fly`): six rebases at the distances of 2.2.2; no jitter on nearby geometry at any point (the sub-millimetre property); screenshots one frame before and after a rebase differ only by the camera's motion; sky, clouds, fog and god rays continuous; shadows and reflections do not glitch; the GI full-rebuild and radiance-cache reset counters in the log do not move; TAA shows no ghosting on the rebase frame; the frame-time spike at a rebase is below the recorded budget (UNVERIFIED target: under 50 ms; record the real number and the per-participant breakdown).
3. **Seam parking.** Park on the trigger line and wiggle +-1 km: exactly one rebase, none while wiggling; the readout shows 0 deferrals.
4. **Physics across a rebase.** A crate stack asleep stays asleep; an awake pile does not pop; a car left driving continues; a character walking across stays grounded; a hinged door anchored to the world, a slider and a pulley hold; a ragdoll and a cloth survive; a sleeping rigid body does not wake. `phys.hash` differs after a rebase by design: compare positions in absolute terms.
5. **Streaming.** On the cooked `BigWorldLab` and on cooked NeonDistrict: resident chunk and entity counts across a rebase are unchanged; zero loads and evictions on the rebase frame; the load window fills within the millisecond budget; a hitch-free fly-through of NeonDistrict compared with the same fly-through on `LevelStreaming` (the shipping path against the editor path); a mesh not yet ready never appears as a missing building (prepare gate).
6. **Cook.** `Cook Chunks` on NeonDistrict: record time, size (estimate in 2.4.4: about 100 MB) and the report; edit one placement, the set is reported stale and the game falls back to `LevelStreaming` with a warning; re-cook, fresh.
7. **Editor.** Drag a gizmo while flying out (the rebase waits); move an object, fly 20 km, undo: it returns to the right place; save the level at `O != 0` and `diff` the `.ocworld` against the original: only the records you edited differ, and a placement at 123,456,789 cm round-trips; Play (LevelStreaming) and `Play > Use cooked chunks` start and stop cleanly across a rebase.
8. **Saves.** Save at 40 km, quit, load: the player is where they were; a save made before this change loads at the origin it always did; chunk-streamed scenery is not duplicated.
9. **Vulkan parity.** Repeat 2 on `--backend vulkan` (cloud and wave folds, `PerFrameCB`; there is no device velocity or TAA to compare). NeonDistrict on Vulkan is pre-broken: use `BigWorldLab`.
10. **Audio and effects.** A looping 3D sound at a fixed place keeps its level and pan across a rebase; a static fire emitter keeps burning and its GPU particles do not streak; water waves keep their phase; decals and lights stay attached.
11. **Far origin.** `world.tp 500 0 0`, then drive, shoot, walk: no far-origin vibration (the symptom the engine has hit before), no TDR in the first frames at the new place, physics stable.
12. **Identity.** NeonDistrict at the usual cameras: no rebase ever happens, frame time within noise of the previous build, and the existing gate sweep unchanged (the owner's call whether to run it: it fronts an editor window).
13. **Rebase cost.** Record, for NeonDistrict-sized resident content, the rebase frame milliseconds by participant (physics, scene, TLAS rebuild, light grid, particles) in section 9; they are the first numbers of their kind.
14. Full `ctest`, the graph suite via `dotnet run`, `docs/nodes/WorldOrigin.md` generated with the three rows and no hand edits.

---

## 7. Open questions (each with the default chosen; proceed unless told otherwise)

1. **Editor Play: records or cooked chunks?** Default: records (`LevelStreaming`); `Play > Use cooked chunks` is opt-in. The editor's edit-while-streamed machinery is not replaced in this wave.
2. **Retire `LevelStreaming` in favour of chunks everywhere?** Default: no. It is the editable path, and flat is the permanent oracle (CHUNKS.md 2.1). Revisit after `ChunkedEqualsFlat` has been green for a release.
3. **Chunk sizes other than 1600 cm.** Default: floating origin disabled for such a level (one f32 frame, +-10.4 km), with a log line; `chunkSizeValid` still admits them for streaming.
4. **Rebase margin.** Default 102,400 cm (dead band 204,800). Tunable by `WORLD.REBASEMARGIN` within `[0, 229,376]`; the owner may prefer a larger margin if a headed pass shows rebase hitches near seams, at the price of less margin for streaming radius (I3).
5. **Sidecars beyond +-1,048,576 cm of world (0,0)** (landscape, lanes, nav, foliage tables). Default: v1 limit with a precision guard (2.9). Per-sidecar tiling or per-cell origins are follow-ups.
6. **Packed region layout.** Default: sparse per-group directory with 16-byte units (2.4.4). Alternative: one payload per 16^3 group (no format change, 104 MB of dense directories). Choose the alternative only if phase C-2 shows the packed reader costs more than it saves.
7. **Lights, decals, water, post volumes, PREFABINST.** Default: level-global in v1, skipped with one warning beyond +-1,048,576 cm of the load origin. Payload tags for lights and decals are a follow-up if a large authored level needs them.
8. **ReSTIR reservoir history across a rebase.** Default: dropped for one frame (a few frames of noisier GI). A compute pass that subtracts the delta from the stored positions is exact and a few tens of MB of bandwidth at 1440p (UNVERIFIED); do it only if the headed pass finds the burst objectionable.
9. **NeuRaC.** Default: key the cache by absolute cell through `rcOriginCell`. Fallback: `kFlagClearAll` on a rebase. RENDER decides after reading the HLSL users.
10. **Material `WorldPosition` node.** Default: origin-relative, jumps once per rebase, documented. An `AbsoluteWorldPosition` node (offset modulo a pattern period) is a follow-up.
11. **Undo across a rebase.** Default: walk the stacks (2.7). If the walk proves incomplete at some edit site, the fallback is an `originAtRecord` pair on each `EditCmd`.
12. **Networking and replay.** Nothing exists. Rules fixed here: `OriginPolicy::Fixed` for lockstep, shift events recorded in the input stream, replicate `WorldPos` or f64. A large cooperative world needs a deterministic shift policy designed with the netcode.
13. **Physics shift budget.** If the P-0 benchmark exceeds 16 ms at 10,000 statics or 50 ms at 100,000, the levers of 2.3.7 apply in that order and the owner is told first. Default until measured: single-frame shift, no lever.
14. **Altitude.** Z is never rebased; precision degrades beyond +-10.48 km of altitude. Space or high-altitude play needs a separate vertical scheme.
15. **Resident window.** One f32 frame around the camera. Stream sources beyond the cliff do not stream. A second remote player needs a second frame; not designed.
16. **Cooking at packaging.** Default: `stage-game.ps1` runs `AverAssetC cook-chunks` for every level that declares `STREAM chunks=` and fails the package on a stale or missing set. Needs the owner's yes because it changes the package step.
17. **Initial origin.** Default: the region containing the SPAWN record (game) or CAMERA record (editor).
18. **`WORLD.FLOATINGORIGIN` default.** On. A level that never leaves the window never rebases, so it costs nothing.
19. **`generatorVersion` bump to 2** and the open-time comparison (2.4.2). Default: yes; a stored override still wins its chunk.
20. **Patent notes.** Everything here is long-public generic technique: floating-origin rebasing (used by many engines for over twenty years), hierarchical integer-plus-float coordinates, region-file chunk storage, history-matrix reprojection compensation. **No patent search was run** (consistent with the parity plan's rule for anything vendor-shaped; nothing here is Epic- or NVIDIA-shaped beyond the long-public floating-origin idea). No vendor code or control law is copied; no third-party library is added.

### 7.1 Risks that are not questions

1. **PlayMobility** terminal Movable (2.2.5) is the easiest thing to miss; `OriginParticipantsTest` holds the negative control.
2. **`setVolume` reset**: shifting the host's `giCenter_` before the renderer is told resets the radiance cache and net warm-up.
3. **A world-anchored joint** created before the anchor change and loaded from a snapshot taken in the old model: snapshots are in-session only (WORLD_STATE_DEBUG open question 5), so none persists.
4. **The physics shift cost** is unmeasured (P-0 gates it).
5. **A missed narrowing site** keeps a feature working at 9 km and breaking at 20: the phase 0 audit list and the 100 km `WorldFlightTest` are the guards.
6. **Mesh-readiness stalls** in the authored host are visible as missing buildings; the prepare gate and the millisecond budgets exist for exactly this, and `AuthoredStreamTest` pins the gate.
7. **Two residency systems drifting**: `ChunkedEqualsFlat` is the guard; if it goes red the cook, not the test, is wrong.
8. **Uncommitted wave 11 work in the tree** (physics, hair, soft body, formats): the shared-file table orders the edits; each area rebases onto what has landed.

---

## 8. Supersession of `WORLD_STATE_DEBUG.md` phase H

This document replaces, in full, the following of `WORLD_STATE_DEBUG.md`. Everything else in that file (sections 2.1 to 2.9, phases A to G, their tests and headed checks 1 to 11 and 16) stands.

| WORLD_STATE_DEBUG.md | Status | Replaced by |
|---|---|---|
| Section 0 item 10 ("both mechanisms": `JPH_DOUBLE_PRECISION` + floating origin; "a rebase costs physics O(1), moves no body, drops no warm start, snapshots are origin-independent") | **Void.** Jolt stays single precision (owner decision 2026-10-25) | section 0 items 4 and 5, 2.3 |
| Section 2.10 intro and the three-layer table | Replaced: the layer split is now chunk streaming / `WorldPos` (authoring, saves, net, streaming), origin-relative f32 (everything per-frame) | 2.1 |
| 2.10.1 ABI: `aver_phys_origin_set/get/shift`, `_d` position functions (`body_position_d`, `character_*_d`, `vehicle_pose_d`, `raycast_d`) | **Void.** No set, no get-of-O inside physics, no `_d` twins | `physics_origin_abi.h` (2.3.2): prepare, shift, generation, total, stats |
| 2.10.2 `Convert.hpp` `toJoltPos/fromJoltPos`, the `RVec3`/`RMat44` migration of about 100 call sites, the soft-body, vehicle and character double notes, the "+-1000 km / warning at 500 km" radius, the `PReal` precision switch | **Void.** `Convert.hpp` does not change; no migration; the supported world is unbounded through chunks, the simulation window is one f32 frame | 2.3.1, 2.3.8 |
| 2.10.3 rebase mechanics: `WorldOrigin` in `Aver.World` | **Kept in spirit, replaced in detail.** The step is 1,638,400 cm (the "16,384 cm" is a unit slip); hysteresis is added (the plan had none); the service is two-phase with an ordered participant list | 2.2.1 to 2.2.4 |
| 2.10.3 table, Physics row | **Void** (O(1), nothing moves) | 2.3.3 |
| 2.10.3 table, Scene row | Kept, with PlayMobility, AnimSystem, sequencer and the others it omitted | 2.2.5 |
| 2.10.3 table, Camera row ("the camera is a root entity") | **Corrected**: host members | 2.2.5 |
| 2.10.3 table, Renderer row (hi/lo float3 in `PerFrameCB`, `O_wrapped` keeps the sky bit-identical, "three hand mirrors") | **Corrected**: one C++ struct + one cbuffer; two appended `float4`s (784 bytes); the wrap claim is wrong, replaced by the f64 CPU fold; GI re-key, NRD2, froxel copy, NeuRaC, TLAS listed | 2.2.6 |
| 2.10.3 table, Particles / Audio / Navigation rows | Kept, with the real state (GPU kernel, voice pool, nav origin, paths) | 2.2.5 |
| 2.10.3 table, Streaming row | Kept (keys unchanged), plus the origin parameter on restore, capture, partition and sources | 2.4.2 |
| 2.10.3 table, Landscape row ("the physics heightfield body is absolute in double and is not touched") | **Void**: the heightfield body shifts like any body | 2.3.5, 2.2.6 item 6 |
| 2.10.3 table, Editor row | Kept, plus undo walk, PIE, gesture gate | 2.7 |
| 2.10.3 table, Gameplay scripts row (`Entity.WorldPosition (Double3)`, `WorldOriginChanged`) | **Renamed**: `Entity.AbsolutePosition` (the float `WorldPosition` exists); the event is `world.origin_changed` | 2.5 |
| 2.10.4 networking and saves ("physics snapshots are origin-independent", "`%.17g`", "saves store f64 absolute") | **Void for snapshots** (frame-dependent now); `%.17g` replaced by shortest round-trip plus the untouched-record rule; saves get an origin header, not per-entity f64 | 2.3.6, 2.6 |
| 2.10.5 cost (memory +32 to +48 bytes per body, double-vs-float step benchmark) | **Void** | 2.3.7 (shift benchmark) |
| 2.10.6 keeping the float build alive (`AVER_PHYSICS_PRECISION`, `LargeWorldTest` float contract, CI job) | **Void.** Single precision is the only precision; `aver_phys_precision()` keeps answering `SINGLE` | |
| 2.10.7 phases H0 to H5 | **Replaced** | section 4 |
| 2.11 C# `Double3`, `PhysicsOrigin` (`Set`, `Shift`), `Body.PositionD` | **Replaced**: `Double3` and `WorldPos` live in `Aver.Framework`; `PhysicsOrigin` is read-only (`Generation`, `Total`, `LastShiftMs`); no `PositionD` | 2.5 |
| Section 3.1 `PhysicsLargeWorld.cpp`, `WorldOrigin.hpp/.cpp`, `LargeWorld.cs`, `LargeWorldTest.cpp`, `WorldOriginTest.cpp`; section 3.3 entries for `Convert.hpp` "phase H only", the "all physics `.cpp`" position migration, `OcWorld.cpp` "phase H: `%.17g`", `FormatTest` `%.17g`, `Entity.WorldPosition`, `modules/rhi...` "phase H4" | **Void or renamed** to the files of section 3 | section 3 |
| Section 4 landing order and phase H ("lands LAST, after all five other wave 11 areas", XL) | **Void.** The reason was that H changed the type of every position in every physics `.cpp`; the new design is additive and does not collide, so it need not wait | section 4 |
| Section 5.1 `LargeWorldTest`; 5.2 `WorldOriginTest` and the `PhysicsDebugOverlayTest` origin-generation clause | **Replaced** (the overlay-signature clause moves to `EditorOriginTest`) | section 5 |
| Section 6 items 12 to 15 (large world, rebase, cost double-vs-float, float build) | **Replaced** | section 6 |
| Section 7 open questions 1 (double default), 9 (Z never rebased: kept), 10 (+-1000 km radius: void) | 1 and 10 void; 9 kept as I1 | 2.2.1, open question 14 here |

The other wave 11 documents that expected a "large-world sweep" (CHARACTER_RAGDOLL_VEHICLE item 1 of its open questions and its file table; CONTACTS_FILTERS 2.3.7, which expected `fromJoltPos(RVec3Arg)` in `Convert.hpp`; SHAPES_MATERIALS open question 1; QUERIES_JOINTS question 8; SOFTBODY_HAIR 2.21) need **no sweep and no `_d` twins**: under single precision `RVec3` is `Vec3`, their positions are origin-relative f32 like everything else, and the only obligations they have are the engine-side world-position hooks of 2.3.2. A one-line pointer to this section is the integrator's edit.

---

## 9. As built: deviations and notes

Written by the implementing agent after each phase; nothing here has been compiled, run or tested yet. Each line says what the plan said, what the code does and why. Also record here: the phase 0 narrowing audit list; the P-0 physics shift benchmark table (statics, dynamics, total ms, optimize ms, query penalty) and which lever, if any, was applied; the rebase-frame milliseconds by participant on NeonDistrict-sized content; the NeonDistrict and `BigWorldLab` cook sizes, times and chunk counts; the Debug object-section counts for any `sandbox` translation unit that grew (the COFF cap note in the memory index; `SandboxApp.hpp` is the likely one); the answer to C-0 (can `AverAssetC` link `Aver.World`); which function owned the LEVELINST composition (`flattenLevelInstances`); the NeuRaC decision (absolute cell or `ClearAll`).
