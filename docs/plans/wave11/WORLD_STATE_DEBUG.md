# World, state and debug: the rest of Jolt's world surface, and large-world coordinates (wave 11)

Design contract for the implementing agent. Owner decision (wave 11): integrate the rest of Jolt Physics. This area is the **world and solver configuration, the job system, state recording (rollback, replay, quicksave), the shape binary cache, the Jolt debug renderer, the profiler bridge, stats and determinism tools, and large-world coordinates**. Nothing here is built or run. Rules in force: upgrade the existing physics ABI and the existing editor overlays (no parallel systems), no new third-party code (every Jolt source named below is already vendored under `modules/physics.jolt/Jolt` and already listed in `Jolt.cmake`), lean comments, never delete files, D3D12 and Vulkan parity (this area adds **no GPU work**: the only GPU-visible output is `rhi::LineVertex` lines that both backends already draw), headless ctest where possible, headed manual runs by the owner, never rebuild over the owner's running editor. Excluded by owner decision: Jolt's ObjectStream and PhysicsScene files. Included by owner decision: `JPH_DOUBLE_PRECISION` and Jolt 5.6 Hair (Hair is another wave 11 area; section 7 says where the two meet).

Format and depth follow `docs/plans/wave10/VEHICLES.md`. Every "exists today" claim below was read in the tree on 2026-10-19. Line numbers are last-known-good pointers; grep the symbol when one is off. **UNVERIFIED** marks anything reasoned from reading but not confirmed by running it; every number that is a starting value is UNVERIFIED until the headed pass in section 6.

**Landing order, stated once and repeated where it matters: sections 2.1 to 2.9 (phases A to G) are independent of the other wave 11 areas and can land any time. Large-world coordinates (section 2.10, phase H) is cross-cutting and lands LAST, after all five other wave 11 areas have merged,** because it changes the type of every position that crosses the physics C ABI's internals, touches `Convert.hpp` and every physics `.cpp`, and would otherwise collide with all of them.

---

## 0. Decisions in one screen

1. **Three new ABI headers, all additive; no existing signature or version changes.** `physics_world_abi.h` (settings, init config, stats, profiler table, build facts, large-world origin), `physics_state_abi.h` (snapshots, exact stepping, state hash, shape binary cache), `physics_debug_abi.h` (the Jolt debug renderer as a line feed). Each carries its own `AVER_PHYS_*_ABI_VERSION 1`. `aver_phys_init(void)` and the roughly 300 other existing exports are untouched, so the 30-plus test call sites stay as they are. A world that never touches a new function behaves exactly as today (a test pins this).
2. **One settings table drives everything.** 27 live solver settings plus 5 init-time config keys live in one table in `PhysicsConfig.cpp` (name, unit, range, default = Jolt's own). That table produces the C ABI (`aver_phys_setting_*`), the `PHYSICS.*` project keys, the `phys.*` console variables, the C# enum, the Aver Node attribute list and the editor page. Defaults are Jolt's, converted to the engine's centimetres, so an untouched project is identical.
3. **The hard-coded limits become real, applied settings.** `PHYSICS.MAXBODIES` / `MAXBODYPAIRS` / `MAXCONTACTS` / `TEMPALLOCMB` are parsed today and read by nothing (`GameTick.hpp` says so in its own comment). They become *pending config* applied by `aver_phys_rebuild`, which runs only on an empty world, called by both hosts after the manifest is parsed and before the first level load. A world that is not empty keeps its limits and says so ("applies when the world is next created"). `PHYSICS.THREADS` joins them.
4. **Job system: no engine scheduler exists** (section 1.2 shows the search), so the bridge is a thread count, a single-threaded mode (Jolt's own `JobSystemSingleThreaded`), and a one-line type change that lets a future scheduler slot in. `World::jobs` becomes a `JPH::JobSystem` base pointer.
5. **Profiler: `JPH_EXTERNAL_PROFILE` with a hook compiled into the Jolt target.** Off at run time by default (a null-sink early-out), on from the editor's profiler panel. Aver's own fixed-step stages (buoyancy, soft-body aero, vehicle aids, characters, joint break checks) are bracketed by the same macro so the table is one tree. Jolt's built-in profiler (`JPH_PROFILE_ENABLED`, HTML dumps) is the diagnostic preset's job, not the default (the two are mutually exclusive in `Jolt.cmake`).
6. **Debug renderer: `JPH_DEBUG_RENDERER` on in Debug and Release; output is a flat line feed.** A `DebugRendererSimple` subclass collects lines into a buffer in the engine's axes and centimetres, laid out as `rhi::LineVertex` pairs; the editor draws them through the line-mesh path every other overlay uses, so D3D12 and Vulkan are identical by construction. Distance-culled and line-budgeted; static triangle meshes and heightfields are drawn only near the camera through `Shape::GetTrianglesStart`. A `Show > Physics` submenu beside the existing `Show Colliders`.
7. **State recording: a byte-vector `StateRecorder`, a snapshot handle space, all-or-nothing restore.** Snapshots cover Jolt's bodies, contacts, constraints (joints, ragdolls, vehicle wheels/engine/gearbox), characters, soft bodies, **plus** the engine-side state Jolt cannot see (fixed-step accumulator, vehicle driver-aid state, character ground carry). Statics are excluded by default. A topology epoch refuses a restore into a world whose bodies or constraints changed in a way Jolt would mis-restore silently (Jolt restores constraints by array index; see 2.6). `aver_phys_step_exact(n)` and `aver_phys_state_hash` are the deterministic-replay primitives. The rollback *netcode* is wave 10 B's and not built here.
8. **Shape binary cache** for mesh and heightfield shapes (`Shape::SaveBinaryState` / `sRestoreFromBinaryState`), stored by the host as an AVR1 `PSHP` file under `<project>/DerivedDataCache/Physics/`. **Gated by a measurement**: phase E starts by timing BVH build against restore on the real levels and wires the cache only if restore is under a fifth of build (the repo has shipped two "informative negatives" from optimising unmeasured costs).
9. **Determinism tooling is cheap by default and heavy only in a diagnostic preset.** Always available: the state hash and a per-step hash log. Diagnostic preset (`AVER_PHYSICS_DIAGNOSTICS=ON`): `JPH_ENABLE_DETERMINISM_LOG`, the stats-tracking defines and Jolt's internal profiler. `CROSS_PLATFORM_DETERMINISTIC` becomes a CMake option, default OFF, with **no cross-platform claim** until a Windows plus macOS state-hash test is green in CI.
10. **Large world: both mechanisms, split by what each is good at.** The engine gets a **floating origin** (region-anchored, X and Y only, per `docs/CHUNKS.md` slice 10) because the scene graph and the renderer are f32 and will stay so. **Jolt is built `JPH_DOUBLE_PRECISION`** and the physics ABI gains a **world origin offset**: every existing float position function means "relative to the current origin", Jolt stores absolute doubles, so **a rebase costs physics O(1) (set the offset), moves no body, drops no warm start, and snapshots are origin-independent.** A short list of `_d` double entry points covers the few callers that hold absolute coordinates (teleport, query origins, net, saves). A float build is kept alive behind `AVER_PHYSICS_PRECISION=SINGLE`: origin fixed at zero, rebasing disabled.
11. **Not in this area, said plainly:** ObjectStream / PhysicsScene files (owner), Hair and Jolt's compute backends (another area), the rollback netcode itself and delta compression of snapshots (wave 10 B), saving physics state across a level reload (entity handles change; the `Aver.Save` path owns that), cross-platform determinism as a *promise*, a physics-material table, and any packaged-game line overlay (the packaged game still has no line-overlay path).
12. **Size.** Phases A to G are **L** together; phase H (large world) is **XL** by itself. Overall **XL**, with H last.

---

## 1. What exists

### 1.1 Survey claims, verified

| Survey claim | Verdict | Evidence |
|---|---|---|
| `SetPhysicsSettings` is never called | **True** | no reference outside `modules/physics.jolt`; `PhysicsSystem.h:111-112` declares it; `World` has no `JPH::PhysicsSettings` member (`PhysicsInternal.hpp:289-336`) |
| Hard-coded init limits at `PhysicsWorld.cpp:291-306,490` | **True, and the numbers moved** | temp allocator 10 MB `:291`; workers `hardware_concurrency() - 2`, floor 1 `:292-294`; `kMaxBodies = 65536`, `kMaxBodyPairs = 65536`, `kMaxContactConstraints = 16384` `:302-304`; `system.Init(..., 0 body mutexes, ...)` `:305`; `Update(fixedStep, 1, ...)` with the **collision step count a literal 1** `:490`. The 4096/8192/2048 in `docs/code-notes/formats.md:42` and `docs/RUNTIME-DEDUP.md:19` is stale |
| `PHYSICS.MAXBODIES` etc. exist as keys | **True, parsed and dead** | `OcProject.hpp:195-198` (fields), `OcProject.cpp:212-219` (parse), `:325` (owned-key list), `:432-435` (write); the only reader is a comment at `PhysicsWorld.cpp:265` and `GameTick.hpp:86-97` explaining that nothing applies them |
| Physics starts before the project is known | **True** | `SandboxInit.cpp:279` (`startPhysics("Sandbox")` before any project can open), `GameApp.cpp:580` (`initPhysics`); `applyProjectPhysics` runs after the level load (`GameApp.cpp:1284`, `SandboxProject.cpp:240`). So a limit cannot be applied by a setter after the fact: this is why phase A needs `aver_phys_rebuild` |
| Job system to bridge | **No engine scheduler exists** | grep for `Scheduler`, `WorkerPool`, `ThreadPool`, `TaskGraph`, `JobSystem` over `modules/ Runtime/ sandbox/` finds only Jolt's. Threads are ad hoc: `Bc7.cpp:235`, `GameContent.cpp`, `VoxiRenderer.cpp`, `MotionMatchSystem.cpp`, `D3D12Device.cpp`. Only physics sizes a pool, and it uses `std::thread::hardware_concurrency()` at `PhysicsWorld.cpp:292` (the other hit is `Bc7.cpp:235`) |
| StateRecorder SaveState/RestoreState unused | **True** | no `StateRecorder`, `SaveState` or `RestoreState` call in `modules/physics/src`, `tests/physics`, `sandbox`, `Runtime`. Jolt compiles `StateRecorder.h` and `StateRecorderImpl.cpp` (`Jolt.cmake:407-409`) |
| Shape binary cache missing | **True** | no `SaveBinaryState` anywhere. Mesh shapes are built once per mesh id and shared (`aver_phys_create_mesh_shape` `PhysicsWorld.cpp:898`, table `g_meshShapes` `:174-176`, callers `world/src/LevelInstance.cpp:197` and `Runtime/src/GameContent.cpp:1338`); a heightfield is built straight into a body (`aver_phys_add_heightfield` `PhysicsWorld.cpp:989`, caller `Runtime/src/GameLandscape.cpp:82`). A disk cache of the *triangles* exists (`writeCollisionCacheFile`, `GameContent.cpp:1328,1553`); the **BVH built from them is not cached** |
| Debug renderer not enabled | **True** | `JPH_DEBUG_RENDERER` is defined only by `Jolt.cmake:914-918` options we never set (`modules/physics.jolt/CMakeLists.txt:55-63`); `Jolt/Renderer/DebugRenderer*.cpp` are in the source list (`Jolt.cmake:436-443`) and compile to nothing |
| Existing AABB overlay and line-mesh path | **Found** | `Show Colliders` toggles `showColliders_` (`SandboxShell.cpp:2218`); `SandboxApp::rebuildColliderOverlay` (`SandboxViewport.cpp:75`) draws world AABBs from `aver_phys_body_aabb` into a cached line mesh; called at `SandboxRender.cpp:2095-2097`. Line API: `rhi::LineVertex {px,py,pz,r,g,b}` (`RHI.hpp:60`), `createLineMesh` / `drawLines` / `destroyLineMesh` (`RHI.hpp:678-688`), implemented by D3D12 (`D3D12Device.cpp:4656,4666`) and Vulkan (`VulkanDevice.cpp:2459,2467`). Eight overlays already use the pattern (AI, motion, camera rigs, soft bodies, vehicles ...; `SandboxRender.cpp:1960-2060`) |
| Profiler | **Found, three disconnected things** | `PlayProfile` (`sandbox/src/PlayProfile.hpp`): nine Play phases, physics lives inside the single row "Gameplay tick + physics step"; `CpuLap`/`CpuNest` scene-walk spans (`core/CpuTiming.hpp`, `cpuReadTicks`, `cpuNsPerTick`, `cpuEnsureCalibrated`); `HitchMarks` (`core/HitchMarks.hpp`, `AVER_HITCH_MS`). Jolt's `JPH_PROFILE_*` macros compile to nothing today. `PhysicsInternal.hpp:100` already guards `GetBroadPhaseLayerName` for the day a profiler define is set |
| Stats tracking, determinism log, cross-platform determinism | **All off** | `JPH_TRACK_BROADPHASE_STATS` / `_NARROWPHASE_` / `JPH_TRACK_SIMULATION_STATS` need CMake variables (`Jolt.cmake:874-886`); `DeterminismLog.h:7` has `//#define JPH_ENABLE_DETERMINISM_LOG` (a source comment, so it must come from a compile definition); `CROSS_PLATFORM_DETERMINISTIC OFF` with a recorded reason (`physics.jolt/CMakeLists.txt:47-56`) |
| Large-world coordinates | **Nothing exists** | Jolt `DOUBLE_PRECISION OFF` (`physics.jolt/CMakeLists.txt:55`); engine is absolute f32 end to end: `Vec3`/`Transform` (`core/Math.hpp:32,268`), `CLocal.xf` (`scene/Components.hpp:29-32`), `float*` across the physics, scene and framework ABIs; no origin shift code anywhere (`docs/CHUNKS.md` section 3.4, `modules/world/README.md`); `grep worldOrigin\|originOffset` over `modules Runtime sandbox` is empty |

### 1.2 Facts the design leans on

| Fact | Where |
|---|---|
| The fixed-step loop, in order: characters, buoyancy, `softbody::onStepBegin`, `vehicleStepBegin`, `system.Update`, `vehicleStepEnd`, `afterStepJoints` | `PhysicsWorld.cpp:387-496` |
| `aver_phys_step(dt)` accumulates into `World::accumulator`, clamps catch-up to 8 steps, runs whole steps, returns the count | `PhysicsWorld.cpp:387-499`, `PhysicsInternal.hpp:334-335` |
| `Update`'s `EPhysicsUpdateError` return (`ManifoldCacheFull`, `BodyPairCacheFull`, `ContactConstraintsFull`) is thrown away, so a too-small limit fails silently | `PhysicsWorld.cpp:490`, `Jolt/Physics/EPhysicsUpdateError.h` |
| `World` holds `std::map<int32_t, VehicleEntry> vehicles` with all the vehicle step state in the header (pedals, applied pedals, TCS/ABS factors, gear timers, last velocity, per-wheel airborne/skid/ABS), so a snapshot can serialise it without touching `PhysicsVehicle.cpp` | `PhysicsInternal.hpp:215-276,303-310` |
| `characterCarry`, `characterGravity`, `characters`, `accumulator`, `fixedStep` are `World` members | `PhysicsInternal.hpp:311-336` |
| Joint, ragdoll, soft-body and stair-setting tables are file-static in their `.cpp` files (not reachable from a new file) | `PhysicsJoints.cpp:61-68`, `PhysicsRagdoll.cpp:71-72`, `PhysicsSoftBody.cpp:106-121`, `PhysicsCharacter.cpp:59-83` |
| A breakable joint's "broken" state is Jolt's `Constraint::mEnabled`, which `Constraint::SaveState` writes | `PhysicsJoints.cpp:951-966`, `Jolt/Physics/Constraints/Constraint.cpp:51-59` |
| `PhysicsSystem::SaveState` writes global (previous dt, gravity), every body in the broad phase (position, rotation, motion properties; soft bodies add their vertices), the contact cache, and constraints **by array index**; it does **not** save `CharacterVirtual`s | `Jolt/Physics/PhysicsSystem.cpp:2899-2919`, `Body/BodyManager.cpp:704-728`, `Constraints/ConstraintManager.cpp:205-239`, `Character/CharacterVirtual.h:267` |
| `RestoreState` (non-validating) looks each stored body up by `BodyID` and returns false mid-way if one is gone; constraints are restored through `mConstraints[constraint_index]`, which silently mis-restores when an earlier constraint was removed | `Body/BodyManager.cpp:775-798`, `Constraints/ConstraintManager.cpp:276-292` |
| `StateRecorderImpl` is a `std::stringstream`: correct but allocating and slow | `Jolt/Physics/StateRecorderImpl.h:12-50` |
| With `JPH_DOUBLE_PRECISION`, body positions, `RVec3`/`RMat44`, ray and shape cast origins, `ContactManifold::mBaseOffset`, character and vehicle world positions, and constraint attachment points are doubles; shapes, velocities and local-space data stay float; the **broad-phase bounds stay float** (`Body::mBounds` is an `AABox`) | `Jolt/Math/Real.h`, `Body.h:273-291,446-448`, `ContactListener.h:26-35`, `MotionProperties.cpp:60-90` |
| Physics code that converts positions: about 100 sites use `toJolt(` / `fromJolt(` (68 and 30 in `modules/physics/src`), ~27 use `RMat44`/world transforms, ~20 `CollideShape`, ~18 cast calls | grep, 2026-10-19 |
| `Convert.hpp` is the one place the axis map and the cm to m scale live | `modules/physics/src/Convert.hpp:19-37` |
| The editor console is a voxi-shaped variable table (`ConsoleVar` with a `stage` lambda and a `ConsoleBatch`), already extended by non-voxi families (`registerPostVars`, `registerRhiVars`, `registerOcclusionVars`), with a command catalog beside it | `sandbox/src/EditorConsole.hpp:150-200,1370-1395,1430-1440,1612-1640` |
| Project Settings has a Physics page (gravity, tick rate) that writes `PHYSICS.*` | `SandboxSettings.cpp:747,1174` |
| The profiler panel is a stack of collapsing headers (CPU phases, scene walk, GPU) | `SandboxShell.cpp:150-260` |
| ABI parity test: one `Abi` row lists up to 12 headers for the physics ABI and **has 11 + the terminator, so it is full**; `Native.<feature>.cs` partials beside `Native.cs` are scanned automatically | `tests/abi/src/AbiEnumTest.cpp:552-573,905-920` |
| Wave 9 Aver Node contract: `Graph<Family>NodeTable.cs` + `GraphNodeDefs<Family>.hpp` + `[AutoRun]` tests, auto-discovered; owner methods in `Aver.Framework` (`VehicleGraph.cs`) | `scripting/csharp/Aver.Graph/GraphVehicleNodeTable.cs`, `sandbox/src/GraphNodeDefsVehicle.hpp`, `docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md` |
| Derived-data cache precedent: `<project>/DerivedDataCache/...` with an AVR1 container (`GIVL`), keys from stat + versions, a miss is never an error | memory note `aver-gi-derived-data-cache`, `modules/assets/include/aver/assets/TextureCache.hpp`, `formats/include/aver/formats/Avr1.hpp:20-30,54-95` |
| Far-origin vibration was fixed for the view rays (camera-relative inverse, `invViewProjRel`, `averViewRayDir`); **still absolute**: vertex transforms, `gPrevViewProj` reprojection, RT origins, CPU picking | memory `aver-far-origin-vibration`, `RHI.hpp:571-575` |
| Streaming already splits an absolute f64 position into `(region, chunk, local f32)`; region 0 is centred on the origin; the planned float origin snaps to region origins, X and Y only (`gCamPos.z` feeds the atmosphere) | `docs/CHUNKS.md` sections 3.1, 3.4, slice 10; `world/include/aver/world/ChunkCoord.hpp:80-140` |
| The level writer still prints numbers with `%.6g` (a 12.3 km coordinate moves 3 cm per save) | `formats/src/OcWorld.cpp:27`, `docs/CHUNKS.md` B8 |

### 1.3 Wave 10 landed and is the base

`physics_vehicle_ext_abi.h`, `PhysicsVehicleExt.cpp`, `PhysicsVehicleTracked.cpp`, the `Show Vehicles` overlay, `vehicleStepBegin/End` and `vehicleBeginCall` are in the tree (`PhysicsWorld.cpp:484-494`). This area builds on that loop and adds its own hooks beside them; SOFTBODY_DEPTH and the other wave 11 areas edit the same neighbourhood (section 3.3).

---

## 2. Design

### 2.1 Architecture, build matrix, file map

```
 project .ocproject PHYSICS.*  --- OcProject (names mirror) ---+
 console phys.*  --- PhysicsConsoleVars.hpp --------------------+--> aver_phys_setting_* / aver_phys_config_*
 C# PhysicsSettings, node rows ---------------------------------+           (physics_world_abi.h, PhysicsConfig.cpp)
                                                                                    |
 Aver.Physics (DLL)                                                                 v
   PhysicsConfig   settings table, pending config, rebuild, job system choice, stats, update-error log
   PhysicsProfile  hook sink + per-thread collector + table       <-- AverProfileHook.cpp (in the Jolt target)
   PhysicsState    ByteStateRecorder, snapshots, engine sections, step_exact, hash, shape save/restore
   PhysicsDebug    LineSinkRenderer, collect(flags,camera,radius,budget), labels
        |  plain C ABI: lines / blobs / numbers
 Editor (sandbox): Settings page, console vars + commands, Show > Physics overlay, profiler panel
 Hosts (Runtime + sandbox): configureProjectPhysics (before first level), applyProjectPhysics (live keys)
 World (modules/world): shape-cache read/write around create_mesh_shape            [phase E]
```

**CMake options** (all in `modules/physics.jolt/CMakeLists.txt`, which is ours; the vendored `Jolt.cmake` is not edited). Each maps to the variable `Jolt.cmake` already reads:

| Option | Default | Sets | Notes |
|---|---|---|---|
| `AVER_PHYSICS_PRECISION` | `SINGLE` until phase H lands, then `DOUBLE` | `DOUBLE_PRECISION` | ABI identical either way; `aver_phys_precision()` reports it |
| `AVER_PHYSICS_DEBUG_RENDERER` | `ON` | `DEBUG_RENDERER_IN_DEBUG_AND_RELEASE` | defines `JPH_DEBUG_RENDERER` for Debug and Release. Binary size only, no run cost until a draw call |
| `AVER_PHYSICS_PROFILER` | `ON` | `JPH_USE_EXTERNAL_PROFILE`, `PROFILER_IN_DEBUG_AND_RELEASE`; adds `AverProfileHook.cpp` to the Jolt target | the hook must live in the Jolt target because `PhysicsTest` links Jolt directly (`tests/physics/CMakeLists.txt:8-12`); defined only here it would be an unresolved symbol in that exe |
| `AVER_PHYSICS_CROSS_PLATFORM` | `OFF` | `CROSS_PLATFORM_DETERMINISTIC` | see 2.9; keeps `USE_FMADD OFF` (already) |
| `AVER_PHYSICS_DIAGNOSTICS` | `OFF` | `TRACK_BROADPHASE_STATS`, `TRACK_NARROWPHASE_STATS`, `JPH_TRACK_SIMULATION_STATS`, `JPH_ENABLE_DETERMINISM_LOG` (compile definition, PUBLIC), and **swaps the external profiler for Jolt's internal one** (`JPH_USE_EXTERNAL_PROFILE OFF`) | heavy; developer preset, never the default or CI |

All of these are PUBLIC compile definitions on the `Jolt` target, so `Aver.Physics` and the one test that links Jolt see the same layout (Jolt checks this at start-up in `RegisterTypes.cpp:83`: a mismatch is a loud failure, not a silent one). `aver_phys_build_flags()` reports which are on; tests skip what a build lacks.

**Units and conventions** in every new function: engine centimetres, +X forward, +Y right, +Z up, left-handed; durations in seconds; angles in degrees unless a name says `Rad`; all ABI calls are main-thread and between steps unless a comment says otherwise (as the rest of the ABI is). Returns are 1/0 with `aver_phys_last_error()` for the reason, the existing channel (`ErrorCodes.hpp`); the one family that needs a richer result (restore) has its own `..._last_result()` and does not return negative codes (the inversion trap the error header documents).

### 2.2 `physics_world_abi.h`: settings, config, stats, build facts, profile

```c
#define AVER_PHYS_WORLD_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_world_abi_version(void);                               /* 1 */

/* ---- build facts: callable with no world ---- */
#define AVER_PHYS_PRECISION_SINGLE 1
#define AVER_PHYS_PRECISION_DOUBLE 2
AVER_PHYS_API int32_t  aver_phys_precision(void);
#define AVER_PHYS_BUILD_DEBUG_RENDERER   0x01u
#define AVER_PHYS_BUILD_PROFILER         0x02u   /* external profiler hook compiled in */
#define AVER_PHYS_BUILD_CROSS_PLATFORM   0x04u
#define AVER_PHYS_BUILD_DETERMINISM_LOG  0x08u
#define AVER_PHYS_BUILD_STATS_TRACKING   0x10u
#define AVER_PHYS_BUILD_ASSERTS          0x20u
AVER_PHYS_API uint32_t aver_phys_build_flags(void);
AVER_PHYS_API int32_t  aver_phys_build_string(char* out, int32_t cap);   /* Jolt's configuration string + " aver-phys-state N"; returns length needed incl. NUL */

/* ---- live solver settings (JPH::PhysicsSettings and the Update call), stored even before aver_phys_init ---- */
AVER_PHYS_API int32_t     aver_phys_setting_count(void);                              /* AVER_PHYS_WS_COUNT */
AVER_PHYS_API const char* aver_phys_setting_name(int32_t key);                        /* "velocitySteps"; NULL if out of range */
AVER_PHYS_API int32_t     aver_phys_setting_find(const char* name);                   /* case-insensitive; -1 if unknown */
AVER_PHYS_API int32_t     aver_phys_setting_range(int32_t key, float* outMin, float* outMax, float* outDefault);
AVER_PHYS_API int32_t     aver_phys_setting_set(int32_t key, float value);            /* clamps into range; applies to the live world at once; 0 for a bad key */
AVER_PHYS_API int32_t     aver_phys_setting_get(int32_t key, float* outValue);
AVER_PHYS_API int32_t     aver_phys_setting_reset(int32_t key);                       /* -1 = every setting */
AVER_PHYS_API int32_t     aver_phys_setting_get_all(float* out, int32_t n);           /* min(n, COUNT) written; returns the count written */

/* ---- init-time config (fixed for a world's lifetime) ---- */
#define AVER_PHYS_CF_MAX_BODIES      0     /* 65536    [1024, 8388607]  Jolt's BodyID limit is 0x7fffff */
#define AVER_PHYS_CF_MAX_BODY_PAIRS  1     /* 65536    [1024, 4194304] */
#define AVER_PHYS_CF_MAX_CONTACTS    2     /* 16384    [1024, 1048576] */
#define AVER_PHYS_CF_TEMP_MB         3     /* 10       [1, 1024]   contact scratch */
#define AVER_PHYS_CF_THREADS         4     /* -1 auto (today's hardware_concurrency - 2, floor 1); 0 = single-threaded; N = N workers  [-1, 64] */
#define AVER_PHYS_CF_COUNT           5
AVER_PHYS_API int32_t aver_phys_config_set(int32_t key, float value);      /* stores the PENDING config (works with or without a world); 1 on success */
AVER_PHYS_API int32_t aver_phys_config_get(int32_t key, float* out);       /* the pending value, i.e. what the NEXT world will use */
AVER_PHYS_API int32_t aver_phys_config_active(int32_t key, float* out);    /* what the LIVE world was built with; 0 with no world */
AVER_PHYS_API int32_t aver_phys_config_differs(void);                      /* 1 when a world exists and pending != active (the editor's "restart" badge) */
AVER_PHYS_API int32_t aver_phys_rebuild(void);                              /* shutdown + init with the pending config. With no world = plain init.
                                                                              Refused (0, reason logged once) unless the world is EMPTY: no bodies, characters,
                                                                              joints, ragdolls, vehicles, soft-body builders */

/* ---- stats ---- */
AVER_PHYS_API int32_t aver_phys_stats(double* out, int32_t n);             /* AVER_PHYS_ST_*; min(n, COUNT) written; returns the count; zeros with no world */
AVER_PHYS_API int32_t aver_phys_stats_reset_window(void);                   /* clears the max, peak and update-error fields */
AVER_PHYS_API int32_t aver_phys_diag_report(int32_t kind);                  /* 1 = broad-phase stats, 2 = per-body simulation stats, written to the engine log through JPH::Trace;
                                                                              0 (and a log line) in any build without AVER_PHYSICS_DIAGNOSTICS. Always exported so the C# parity test sees no conditional ABI */

/* ---- profiler table (needs AVER_PHYS_BUILD_PROFILER; off at run time until enabled) ---- */
AVER_PHYS_API int32_t aver_phys_profile_enable(int32_t on);                 /* 0 if the build has no hook */
AVER_PHYS_API int32_t aver_phys_profile_count(void);                        /* rows in the last published window */
AVER_PHYS_API int32_t aver_phys_profile_get(int32_t index, char* nameOut, int32_t nameCap, double* outF4, int32_t* outI2);
                                                                           /* outF4: inclusive ms/step (avg), exclusive ms/step (avg), max inclusive ms (window), calls/step;
                                                                              outI2: parent row (-1 = root), depth */
AVER_PHYS_API int32_t aver_phys_profile_reset(void);
```

**Live settings (`AVER_PHYS_WS_*`)**: defaults are Jolt's `PhysicsSettings` (`Jolt/Physics/PhysicsSettings.h`), lengths converted from metres to the engine's centimetres, speeds likewise. The name column is the camelCase string, the console variable is `phys.<name>`, the project key is `PHYSICS.<NAME UPPERCASED>`.

| Key | Name | Unit | Default | Range | Jolt field |
|---|---|---|---|---|---|
| 0 | `velocitySteps` | count | 10 | 2..64 (below 2 friction stops working: Jolt says so) | `mNumVelocitySteps` |
| 1 | `positionSteps` | count | 2 | 1..32 | `mNumPositionSteps` |
| 2 | `collisionSteps` | count per fixed step | 1 | 1..8 | the `Update` argument (`PhysicsWorld.cpp:490`, literal 1 today) |
| 3 | `baumgarte` | fraction | 0.2 | 0..1 | `mBaumgarte` |
| 4 | `speculativeDistance` | cm | 2 | 0.1..20 | `mSpeculativeContactDistance` |
| 5 | `penetrationSlop` | cm | 2 | 0.01..10 | `mPenetrationSlop` |
| 6 | `maxPenetration` | cm | 20 | 1..200 | `mMaxPenetrationDistance` |
| 7 | `manifoldTolerance` | cm | 0.1 | 0.001..2 | `mManifoldTolerance` |
| 8 | `linearCastThreshold` | fraction of inner radius | 0.75 | 0.05..1 | `mLinearCastThreshold` |
| 9 | `linearCastMaxPenetration` | fraction | 0.25 | 0..1 | `mLinearCastMaxPenetration` |
| 10 | `minRestitutionSpeed` | cm/s | 100 | 0..1000 | `mMinVelocityForRestitution` |
| 11 | `sleepTime` | s | 0.5 | 0.05..60 | `mTimeBeforeSleep` |
| 12 | `sleepSpeed` | cm/s | 3 | 0.1..100 | `mPointVelocitySleepThreshold` |
| 13 | `allowSleeping` | bool | 1 | 0/1 | `mAllowSleeping` |
| 14 | `deterministic` | bool | 1 | 0/1 | `mDeterministicSimulation` (off is faster and **breaks snapshots and replay**; the page says so) |
| 15 | `warmStart` | bool | 1 | 0/1 | `mConstraintWarmStart` |
| 16 | `bodyPairCache` | bool | 1 | 0/1 | `mUseBodyPairContactCache` |
| 17 | `manifoldReduction` | bool | 1 | 0/1 | `mUseManifoldReduction` |
| 18 | `largeIslandSplitter` | bool | 1 | 0/1 | `mUseLargeIslandSplitter` |
| 19 | `checkActiveEdges` | bool | 1 | 0/1 | `mCheckActiveEdges` |
| 20 | `maxPairsInFlight` | count | 16384 | 256..262144 | `mMaxInFlightBodyPairs` |
| 21 | `stepListenerBatch` | count | 8 | 1..64 | `mStepListenersBatchSize` |
| 22 | `stepListenerBatchesPerJob` | count | 1 | 1..1000000 | `mStepListenerBatchesPerJob` |
| 23 | `pairCacheDeltaPos` | cm | 0.1 | 0.001..10 | `mBodyPairCacheMaxDeltaPositionSq` (stored squared inside) |
| 24 | `pairCacheDeltaRot` | degrees | 2 | 0.01..30 | `mBodyPairCacheCosMaxDeltaRotationDiv2` |
| 25 | `contactNormalDelta` | degrees | 5 | 0.1..45 | `mContactNormalCosMaxDeltaRotation` |
| 26 | `lambdaPreserveDist` | cm | 1 | 0.01..50 | `mContactPointPreserveLambdaMaxDistSq` |
| 27 | `AVER_PHYS_WS_COUNT` | | | | |

Not exposed on purpose: `mCheckActiveEdges` is exposed but `mInternalEdgeRemovalVertexToleranceSq` is not (mesh-baking tolerance, not a runtime knob), and the collision-tolerance constants of `PhysicsSettings.h` (`cDefaultCollisionTolerance` etc.) are compile-time in Jolt.

**How a setting applies.** `PhysicsConfig.cpp` owns one process-wide `float settings[COUNT]` (so it can be set before `aver_phys_init`), converts it to a `JPH::PhysicsSettings` and calls `system.SetPhysicsSettings(ps)` at world creation and on every set. `collisionSteps` is read by the step loop instead of the literal 1. Settings survive `aver_phys_shutdown` (they are process configuration, unlike the layer matrix, which is per world and dies with it). Between steps only, as everything else.

**Config apply, `aver_phys_rebuild`.** `aver_phys_init` is rewritten to read the pending config (defaults reproduce today's literals), so a caller that never configures gets the same world. `aver_phys_rebuild` is `aver_phys_shutdown` + `aver_phys_init` guarded by an emptiness check built from the tables already in `World`, plus `g_meshShapes` is intentionally kept (a shape handle is world-independent, `PhysicsWorld.cpp:168-173`). The hosts' call order is in 2.13.

**Stats (`AVER_PHYS_ST_*`, doubles):** 0 `BODIES`, 1 `MAX_BODIES`, 2 `STATIC`, 3 `DYNAMIC`, 4 `ACTIVE_DYNAMIC`, 5 `KINEMATIC`, 6 `ACTIVE_KINEMATIC`, 7 `SOFT`, 8 `ACTIVE_SOFT` (all from `PhysicsSystem::GetBodyStats()`, `PhysicsSystem.h:231`), 9 `CONSTRAINTS` (`GetConstraints().size()`), 10 `CHARACTERS`, 11 `VEHICLES`, 12 `STEPS_LAST_CALL`, 13 `STEPS_TOTAL`, 14 `STEP_MS_LAST` (wall time of the last `system.Update`), 15 `STEP_MS_AVG` (EMA, 0.5 s time constant like `PlayProfile`), 16 `STEP_MS_MAX` (window since reset), 17 `CALL_MS_LAST` (whole `aver_phys_step`), 18 `CONTACTS_LAST_CALL`, 19 `UPDATE_ERRORS` (bit set of `EPhysicsUpdateError`, cumulative since reset), 20 `UPDATE_ERROR_STEPS`, 21 `TEMP_PEAK_BYTES`, 22 `TEMP_SIZE_BYTES`, 23 `WORKER_THREADS`, 24 `ACCUMULATOR_S`, 25 `TOPOLOGY_EPOCH` (2.6). `TEMP_PEAK_BYTES` needs a tracking wrapper: `TempAllocatorImpl` is `final` (`TempAllocator.h:34`), so the world owns a small `TrackingTempAllocator : JPH::TempAllocator` that forwards to an inner `TempAllocatorImpl` and records the high-water mark.

**Update errors are no longer silent.** The `EPhysicsUpdateError` returned by `Update` is OR-ed into stat 19 and logged **once per world** (the `warnBodyLimit` pattern, `PhysicsWorld.cpp:203-208`) with the cure named: "contact constraints full: raise PHYSICS.MAXCONTACTS (now 16384) and rebuild the world".

### 2.3 Jobs: threads, single-threaded mode, the seam

- `CF_THREADS`: -1 keeps `max(1, hardware_concurrency() - 2)` (`PhysicsWorld.cpp:292-294`), N is N worker threads, **0 builds `JPH::JobSystemSingleThreaded`** (`Jolt/Core/JobSystemSingleThreaded.h`, runs each job as it is added; `GetMaxConcurrency() == 1`). Single-threaded is for deterministic debugging, tests that compare thread counts, and a future dedicated server on a small box.
- `World::jobs` changes from `unique_ptr<JPH::JobSystemThreadPool>` to `unique_ptr<JPH::JobSystem>` (`PhysicsInternal.hpp:297`; the only consumer is `Update`, `PhysicsWorld.cpp:490`). That is the whole bridge for now. **If the engine ever grows a shared task scheduler**, a `JobSystemWithBarrier` subclass over it replaces the construction in `PhysicsConfig.cpp::makeJobSystem` and nothing else changes; nobody should design that scheduler around physics.
- `WORKER_THREADS` is reported in stats. The physics default already leaves two cores for the main and render threads; the page documents that a high `THREADS` on a machine that also runs the denoiser trainer or BC7 encoder will oversubscribe.
- **Determinism does not depend on thread count** (Jolt's own claim, UNVERIFIED here): test 5.2 runs the same scene at 0, 1 and 4 threads and compares the state hash every step.

### 2.4 Project keys, console, C#, editor page (settings)

**Project keys** (`OcProject.hpp/.cpp`, shared): the four existing keys become live; the rest follow one rule, `PHYSICS.` + the setting name upper-cased (`PHYSICS.VELOCITYSTEPS`, `PHYSICS.SPECULATIVEDISTANCE`, `PHYSICS.SLEEPTIME` ...) plus `PHYSICS.THREADS`. `ProjectDesc` gains `std::vector<std::pair<std::string, f32>> physicsSettings` (set keys only, file order) and `int physThreads = -2` (-2 unset, since -1 is a legal value). Formats must not depend on Physics, so `OcProject.cpp` carries a **name list mirror** (`kPhysicsSettingNames[]`, one row per setting) used by parse, by the owned-key list at `OcProject.cpp:325` (so a re-save does not duplicate lines) and by the writer; `tests/game/ProjectPhysicsApplyTest` (links both) fails if the mirror and `aver_phys_setting_name` ever disagree. Canonical form omits keys at their default. Example:

```
PHYSICS.VELOCITYSTEPS 14
PHYSICS.SLEEPTIME 1
PHYSICS.MAXBODIES 200000
PHYSICS.THREADS 6
```

`FORMAT_SPECS.md` and `PROJECTS.md:143` (the "6 keys" row) get the table; the integrator writes them from this section.

**Console** (`sandbox/src/PhysicsConsoleVars.hpp`, new, header-only like `registerOcclusionVars`): `registerPhysicsVars(t)` loops `aver_phys_setting_count()` and adds `phys.<name>` (type U32/F32/Bool by the table, `help` from the unit and range strings, `validate` from `aver_phys_setting_range`, `stage` pushing a `deviceSetters` lambda that ignores the device and calls `aver_phys_setting_set`), plus the read-only `phys.stat.*` family from `aver_phys_stats`. `buildVarTable()` gets one line (`EditorConsole.hpp:1370`). `--set phys.velocitySteps 14` then works from the CLI exactly like `--set voxi.x` (`SandboxMain.cpp:242`, per `docs/plans/wave4/MANY_LIGHTS.md` section 3.6). New commands in the catalog (`EditorConsole.hpp:1612`): `phys.snapshot [slot]`, `phys.restore [slot]`, `phys.hash`, `phys.hashlog on <path>|off`, `phys.stats`, `phys.rebuild`, `phys.profile on|off`. The packaged runtime has no console; its path is the `PHYSICS.*` keys.

**Editor page.** `buildPhysicsSettings()` (`SandboxSettings.cpp:1174`) keeps gravity and tick rate and gains, through one call into `PhysicsSettingsPage.cpp`, five groups: Solver (steps, Baumgarte, slop, distances, restitution speed), Sleeping, Continuous collision, Determinism (the `deterministic` toggle with its warning, the state-hash readout), Limits and threads (the five config keys, with a red "restart world" badge from `aver_phys_config_differs()` and a **Rebuild world** button enabled only when the level is closed or empty). Every row shows the unit, the Jolt default, a per-row reset, and writes `PHYSICS.*` through `project_` + `projectDirty_` like the gravity row. The model (`PhysicsSettingsModel.hpp`: rows from the ABI table, validation, canonical omission, "changed" detection) is ImGui-free and tested.

**C#** (`Aver.Physics`, new files): 

```csharp
public enum PhysicsSetting { VelocitySteps = 0, PositionSteps, CollisionSteps, Baumgarte, SpeculativeDistance, ... }   // the 27, in table order
public enum PhysicsConfigKey { MaxBodies, MaxBodyPairs, MaxContacts, TempAllocatorMb, Threads }
public static partial class Physics {
    public static float GetSetting(PhysicsSetting key);  public static bool SetSetting(PhysicsSetting key, float value);
    public static bool ResetSetting(PhysicsSetting key);  public static (float Min, float Max, float Default) SettingRange(PhysicsSetting key);
    public static bool SetConfig(PhysicsConfigKey key, float value);  public static float GetConfig(PhysicsConfigKey key);
    public static bool ConfigNeedsRebuild { get; }  public static bool RebuildWorld();
    public static PhysicsPrecision Precision { get; }  public static PhysicsBuildFlags BuildFlags { get; }
    public static PhysicsStats GetStats();                 // readonly struct: Bodies, ActiveDynamic, StepMsLast, StepMsAvg, StepMsMax, UpdateErrors, ...
    public static void ProfileEnable(bool on);  public static IReadOnlyList<ProfileRow> ProfileRows();
}
```

The enum is checked against the ABI's name table at test time (`PhysicsSettingsTests`), the same way vehicle index constants are mirrored.

### 2.5 Profiler bridge

- **Hook.** `modules/physics.jolt/AverProfileHook.cpp` (ours, added to the `Jolt` target by `target_sources` when `AVER_PHYSICS_PROFILER`) defines `JPH::ExternalProfileMeasurement`'s constructor and destructor (`Jolt/Core/Profiler.h:38-48`: statically linked builds must supply them; 64 bytes of user data). Both read one atomic sink pointer pair `{begin(name, colour, userData64), end(userData64)}`; null sink = the constructor writes a zero marker and returns (a relaxed load and a branch). `Aver.Physics` installs the sink when `aver_phys_profile_enable(1)` is called and clears it on 0 and on shutdown.
- **Collector** (`PhysicsProfile.cpp`): each thread has a small scope stack (depth cap 64, overflow ignored) and a flat accumulator keyed by `(name pointer, parent name pointer)`; time is `cpuReadTicks()` converted with `cpuNsPerTick` after one `cpuEnsureCalibrated()` at enable (the same tick source the scene-walk instrument calibrates, `CpuTiming.hpp:269-303`, no second clock). At the end of each `aver_phys_step` call the main thread merges the thread accumulators **by name text** (string literals in different translation units can have different addresses), computes inclusive and exclusive per-fixed-step averages over a 60-step window and the window maximum, and publishes the table read by `aver_phys_profile_get`. Workers touch only their own accumulator; the merge happens when no job is running (after `Update` returns).
- **Aver scopes.** A macro `AVER_PHYS_SCOPE(name)` in `PhysicsInternal.hpp` expands to `JPH::ExternalProfileMeasurement` when the hook is built and to nothing otherwise. Six call sites in `PhysicsWorld.cpp`: characters, buoyancy, `softbody::onStepBegin`, `vehicleStepBegin`, `vehicleStepEnd`, `afterStepJoints`. Jolt's own `JPH_PROFILE` scopes (broad phase, narrow phase, solver, CCD, soft bodies, character) nest under the `Update` scope for free.
- **Editor.** A "Physics (Jolt)" collapsing header in the profiler panel (`SandboxShell.cpp:150-260`, one call into `PhysicsProfilePanel.cpp`): the stats line (bodies, active, steps/frame, ms avg/max, update errors in red), the scope tree (indent by depth, ms inclusive/exclusive/max, calls), Enable and Reset buttons. `PlayPhase` gets no new row (physics stays inside "Gameplay tick + physics step"); the new table is the breakdown of that row.
- **Hitches.** When a `HitchMarks` scope around the gameplay tick fires (`AVER_HITCH_MS`), physics appends its top three scopes to the log line if the profiler is on. Cheap, optional (phase B item 6).
- **Cost.** With the sink null the cost is the constructor branch per Jolt scope. The count of `JPH_PROFILE` scopes per step is in the thousands for a busy scene (UNVERIFIED), so phase B measures the overhead with the sink null, and if it exceeds 0.3 ms per frame on NeonDistrict in Release the option defaults to Debug-only. The headed check records the number.

### 2.6 State recording: `physics_state_abi.h`

```c
#define AVER_PHYS_STATE_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_state_abi_version(void);

/* what a snapshot holds */
#define AVER_PHYS_SNAP_BODIES       0x01u   /* dynamic and kinematic bodies, soft bodies' vertices included */
#define AVER_PHYS_SNAP_CONTACTS     0x02u   /* the contact cache: without it the next step re-detects contacts and loses warm start */
#define AVER_PHYS_SNAP_CONSTRAINTS  0x04u   /* joints, ragdolls, vehicle wheels / engine / gearbox */
#define AVER_PHYS_SNAP_CHARACTERS   0x08u   /* CharacterVirtual state, which Jolt's SaveState does not include */
#define AVER_PHYS_SNAP_ENGINE       0x10u   /* accumulator, vehicle driver-aid state, character ground carry */
#define AVER_PHYS_SNAP_STATICS      0x20u   /* also static bodies (off by default: ~48 bytes each, 65k statics = 3 MB per snapshot) */
#define AVER_PHYS_SNAP_ALL          0x1Fu
/* restore options */
#define AVER_PHYS_RESTORE_STRICT    0x01u   /* also refuse when bodies were ADDED since the capture (default: allowed, they keep their current state) */
/* restore results (aver_phys_snapshot_last_result) */
#define AVER_PHYS_RESTORE_OK              0
#define AVER_PHYS_RESTORE_BAD_HANDLE     -1
#define AVER_PHYS_RESTORE_BAD_HEADER     -2   /* magic / ABI version / truncated */
#define AVER_PHYS_RESTORE_BUILD_MISMATCH -3   /* precision, Jolt version, state layout version, determinism build differ */
#define AVER_PHYS_RESTORE_TOPOLOGY       -4   /* a captured body, character, vehicle or constraint is gone, or the constraint set changed */
#define AVER_PHYS_RESTORE_CORRUPT        -5   /* Jolt refused the stream (nothing was applied: restore is validated first) */

AVER_PHYS_API int32_t  aver_phys_snapshot_capture(int32_t reuse, uint32_t flags);   /* new handle, or `reuse` overwritten in place (no allocation once warm); 0 on failure */
AVER_PHYS_API int32_t  aver_phys_snapshot_restore(int32_t snapshot, uint32_t options);   /* 1 / 0; the reason is aver_phys_snapshot_last_result() */
AVER_PHYS_API int32_t  aver_phys_snapshot_last_result(void);
AVER_PHYS_API int32_t  aver_phys_snapshot_release(int32_t snapshot);
AVER_PHYS_API int32_t  aver_phys_snapshot_bytes(int32_t snapshot);                   /* 0 for a dead handle */
AVER_PHYS_API int32_t  aver_phys_snapshot_read(int32_t snapshot, uint8_t* out, int32_t cap);   /* bytes copied (for disk / network); call with out == NULL for the size */
AVER_PHYS_API int32_t  aver_phys_snapshot_import(const uint8_t* data, int32_t size); /* validates the header, returns a handle usable with restore; 0 on failure */
AVER_PHYS_API int32_t  aver_phys_snapshot_info(int32_t snapshot, int32_t* outI, int32_t n);   /* AVER_PHYS_SI_*: flags, bodies saved, characters, vehicles, topology epoch, precision, steps-total at capture */
AVER_PHYS_API uint64_t aver_phys_snapshot_hash(int32_t snapshot);                    /* FNV-1a 64 of the payload bytes; 0 for a dead handle */
AVER_PHYS_API uint64_t aver_phys_state_hash(uint32_t flags);                         /* captures into a scratch buffer and hashes it; no handle */
AVER_PHYS_API int32_t  aver_phys_hash_log(const char* pathOrNull);                   /* non-NULL: append "step hash" per fixed step to the file; NULL: stop. 1 on success */

/* exact stepping, the replay primitives */
AVER_PHYS_API int32_t  aver_phys_step_exact(int32_t steps);                          /* run exactly `steps` fixed steps (1..600), ignoring and leaving the accumulator alone; returns steps run */
AVER_PHYS_API int64_t  aver_phys_step_total(void);                                   /* fixed steps since this world was created */
AVER_PHYS_API int32_t  aver_phys_accumulator_set(float seconds);                     /* for replays that must restore a mid-frame position */
AVER_PHYS_API float    aver_phys_accumulator_get(void);

/* shape binary cache, 2.7 */
AVER_PHYS_API uint32_t aver_phys_shape_cache_version(void);
AVER_PHYS_API int32_t  aver_phys_create_heightfield_shape(const float* samples, int32_t sampleCount, float spacingCm);   /* same handle space as aver_phys_create_mesh_shape */
AVER_PHYS_API int32_t  aver_phys_shape_save(int32_t shape, uint64_t key, uint8_t* out, int32_t cap);   /* bytes written (or needed when out == NULL); 0 if the handle is not a mesh or heightfield */
AVER_PHYS_API int32_t  aver_phys_shape_restore(const uint8_t* data, int32_t size, uint64_t key);       /* shape handle, or 0 for a wrong key / version / build / corrupt blob (always a MISS, never fatal) */
```

**Layout of the blob** (all little-endian): `magic "APST"`, state layout version (bumped by any change below), `sizeof(JPH::Real)`, Jolt version triple, the determinism-build flags, snapshot flags, `topologyEpoch`, `constraintEpoch`, steps-total at capture, a **manifest** (the sorted handles of every body and character and vehicle and joint captured, with the raw `BodyID` of each body), then the sections in this order: (1) Jolt `PhysicsSystem::SaveState(recorder, state, filter)` with the filter below, (2) per-character `CharacterVirtual::SaveState` in handle order, (3) engine section. Because the layout depends on `JPH_DOUBLE_PRECISION` (`Body::SaveState` writes `mPosition`, `MotionProperties` writes `mSleepTestOffset` only in double builds: `MotionProperties.cpp:70-72`), **a snapshot from a float build is refused by a double build** (`BUILD_MISMATCH`), and `aver_phys_build_string` is what two peers compare before exchanging state.

**The recorder.** `ByteStateRecorder : JPH::StateRecorder` (`PhysicsStateRecorder.hpp`, new): a `std::vector<uint8_t>` with a write cursor and a read cursor, `WriteBytes/ReadBytes/IsEOF/IsFailed`. No `stringstream`, no per-field allocation, and `capture(reuse)` clears and refills the same vector so a ring of N snapshots allocates N buffers once. `SetValidating` is used only by the determinism test (5.2) to find the first diverging byte.

**The filter.** `StateRecorderFilter::ShouldSaveBody` returns `!body.IsStatic() || (flags & STATICS)`; `ShouldSaveContact` returns true when at least one body is non-static (a dynamic-on-ground contact must survive; static-on-static never exists). Constraints are saved unfiltered (the vehicle, ragdoll and joint constraints are few).

**Restore is all-or-nothing.** Jolt's restore can fail halfway (a missing body) and leaves the world half-written. So `aver_phys_snapshot_restore` **validates first** against the manifest: every captured body handle still maps to the same `BodyID` in `World::bodies`; every captured character, vehicle and joint still exists; `constraintEpoch` equals the current one. Only then does it call Jolt. Failures return `TOPOLOGY` with nothing applied. Rules this follows from the Jolt facts in 1.2:

- **Bodies removed since the capture** refuse the restore. **Bodies added since** are allowed by default (Jolt's non-validating restore iterates only the stored ids, `BodyManager.cpp:775-798`; the extra bodies keep their current state, which is what a rollback layer that despawns predicted spawns itself wants) and refused under `STRICT`.
- **Constraints are restored by array index** (`ConstraintManager.cpp:276-292`), so *any* constraint add or remove (joint create/destroy, vehicle `finish`/destroy, ragdoll create/destroy) changes indices and would restore the wrong constraint without error. `constraintEpoch` counts those events in `PhysicsJoints.cpp`/`PhysicsVehicle.cpp`/`PhysicsRagdoll.cpp` through one inline `noteConstraintTopology()` in `PhysicsInternal.hpp` (a one-line call at each add/remove site: the **only** edit those three files get from this area). A mismatch refuses. A game that creates constraints during rollback windows must rebuild them from its own tables; the netcode doc owns that policy.
- After a successful Jolt restore the system calls `NotifyBodiesAABBChanged` itself (`PhysicsSystem.cpp:2939-2948`). The engine then clears the **event queues** (contacts, overlaps, vehicle events, break events, soft-body impacts), because the resimulated steps will produce them again; a rollback layer drains events per resimulated step.

**Engine section** (what Jolt cannot see, serialised in handle order from members reachable without touching other areas' `.cpp` files): `accumulator`; per vehicle the dynamic fields of `VehicleEntry` (`reqT/S/B/H`, `appT/S/B/H`, `tcsFactor`, `absActive`, `tcsActive`, `gear`, `shiftLeft`, `clutchRelease`, `lastGear`, `lastRpm`, `lastVel[3]`, `longAccelG`, `latAccelG`, `airborneAll`, `sideSeconds`, `flippedSent`, and each wheel's `absFactor`, `airborne`, `hadContact`, `skidding`, `skidPeak`; the derived `vs/vsi/f/i` arrays are recomputed by `vehicleStepEnd` and are **not** saved); `characterCarry` per character; `characterGravity` is configuration and is not saved. **Inputs are not state** and are never in a snapshot: pedals set this frame, forces, impulses, velocities written by gameplay, kinematic targets, tune values, ragdoll mode and strength, wind. A rollback layer re-issues them for each resimulated tick; the section list above is what is *derived by the simulation* between inputs.

**Known gaps, named:** the ragdoll `Entry` pose targets and mode, `g_wind`, the soft-body registry's impact pair slots (`PhysicsSoftBody.cpp:121`) and buoyancy overrides are Aver-side configuration or inputs and are not captured; the breakable-joint "broken" flag **is** (Jolt `mEnabled`). Soft-body *vertices* are (they are `Body` motion-property state). If the soft-body impact events must replay identically, SOFTBODY_DEPTH adds a `snapshotSave/Restore` pair beside its registry; this area leaves the hook point (`registerStateParticipant`) and does not edit that file.

**Memory.** Per dynamic body roughly position + rotation (double build 24+16 bytes, float 12+16), linear and angular velocity, force and torque (4 x 12), three sleep-test spheres (3 x 16), timer and flag: about 160 bytes (UNVERIFIED, measured in test 5.2). Contacts add per touching pair. A 2,000-dynamic-body scene is therefore about 320 KB plus contacts; a 128-tick ring is about 40 MB. Delta compression and ring policy belong to the netcode; the byte layout is stable per body so a later diff is straightforward.

**Quicksave.** In-session quicksave/quickload (checkpoint within one loaded level) uses a snapshot plus the scene state the `Aver.Save` module already captures (`modules/save/include/aver/save/SaveWorld.hpp`). **Cross-session** physics state does not use this: a reloaded level creates bodies with new handles and new `BodyID`s, so the manifest cannot match. `aver_phys_snapshot_read`/`_import` exist for transport and for tests, not as a save format. Open question 5.

**Hash and replay.** `aver_phys_state_hash(flags)` is FNV-1a 64 (`core/Hash.hpp:14-15`) over a scratch snapshot, so it equals `snapshot_hash` of a capture made at the same instant. `aver_phys_hash_log(path)` writes `step hash` per fixed step (`vehicleStepEnd`'s neighbourhood), which two runs, two machines or two thread counts diff directly. `aver_phys_step_exact(n)` is the replay primitive: `aver_phys_step` is split into a prologue (clear events, `beginCall`s), `runOneFixedStep()` (the loop body, unchanged) and an epilogue, and `step_exact` calls the middle part n times without touching the accumulator. That split is the one refactor of `PhysicsWorld.cpp` in this area (a pure move; the existing three physics suites are its regression check).

### 2.7 Shape binary cache

- **ABI** above. `aver_phys_create_heightfield_shape` is `aver_phys_add_heightfield`'s construction (`PhysicsWorld.cpp:989-1011`, same axis handling) minus the body, returning a shape handle in the existing shape table, so `aver_phys_add_mesh_shape_body` (which scales any shape, `:962`) places it and `aver_phys_add_heightfield` stays as is.
- **Blob.** `"APSH"`, cache version (`aver_phys_shape_cache_version()`, bumped on any change to unit conversion, winding fix or this layout), Jolt version, `sizeof(JPH::Real)`, the caller's `key`, then `Shape::SaveBinaryState`. Mesh and heightfield shapes created by our functions have no materials, so `SaveMaterialState` is empty and restore calls `RestoreMaterialState(nullptr, 0)`. `sRestoreFromBinaryState` returns a `ShapeResult`; any error is a miss. Safe from worker threads (the shape table is already mutex-guarded, `PhysicsWorld.cpp:174-183`).
- **Host side: `modules/world/include/aver/world/PhysicsShapeCache.hpp` (new).** `loadShape(dir, key, out)` / `storeShape(dir, key, blob)` wrap the blob in an AVR1 container, subtype `PSHP` (new constant beside `kAvrSubtypeWrld`, `Avr1.hpp:21-28`; FORMAT_SPECS gets a row), one chunk `DATA`, file `<project>/DerivedDataCache/Physics/<key as 16 hex>.pshp`. The key is `fnv1a64(mesh path, source size, source mtime, kCollisionCacheVersion, shapeCacheVersion)` computed by the **same code that already keys the collision-triangle cache** (`GameContent.cpp:1328,1553`), so a stale source invalidates both.
- **Wiring.** `world::InstantiateOptions` gains `std::function<u64(u32 meshId)> shapeKeyFor` and a cache directory; `LevelInstance.cpp:189-202` tries the cache before `aver_phys_create_mesh_shape` and stores after. `GameContent.cpp:1338` (the Runtime prefetch job) does the same. The landscape heightfield (`GameLandscape.cpp:82`) moves to `create_heightfield_shape` + cache + `add_mesh_shape_body` **only if the measurement says its build is worth caching**.
- **The gate (phase E, step 1).** `PhysicsBenchTool shapes` (a tool exe, not a `*Test`, so ctest does not pick it up) times, for the largest real meshes (NewSponza, NeonDistrict buildings, CyberCity2099) and one 4k heightfield: build, `SaveBinaryState`, restore, blob size. Wire the cache only if restore is under a fifth of build and the blob under three times the triangle cache. Otherwise phase E records the numbers in section 8 and stops; the ABI stays (it is the first half of an offline cooking step).
- **Offline cooking** (the survey's "cooking BVHs offline"): the same two ABI calls plus `AverAssetC` get a `cook-collision` verb (phase E, optional) that writes the `.pshp` files into a project's `DerivedDataCache` during a package, so the packaged game never builds a BVH at load. Not a new tool: a verb on the existing one.

### 2.8 Debug renderer: `physics_debug_abi.h`

```c
#define AVER_PHYS_DEBUG_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_debug_abi_version(void);
AVER_PHYS_API int32_t aver_phys_debug_available(void);                          /* 1 when built with JPH_DEBUG_RENDERER */

#define AVER_PHYS_DBG_BODIES        0x00000001u   /* wireframe shapes of bodies within the radius (meshes and heightfields excluded: see STATIC_TRIANGLES) */
#define AVER_PHYS_DBG_BOUNDS        0x00000002u   /* world-space bounding boxes */
#define AVER_PHYS_DBG_COM           0x00000004u   /* centre-of-mass axes */
#define AVER_PHYS_DBG_VELOCITY      0x00000008u
#define AVER_PHYS_DBG_SLEEP         0x00000010u   /* sleep-test spheres and timers; sleeping bodies grey */
#define AVER_PHYS_DBG_MASS          0x00000020u   /* mass and inertia boxes */
#define AVER_PHYS_DBG_CONSTRAINTS   0x00000040u   /* joint frames, ragdoll joints, vehicle wheels and suspension */
#define AVER_PHYS_DBG_LIMITS        0x00000080u   /* joint limit arcs */
#define AVER_PHYS_DBG_CONTACTS      0x00000100u   /* contact points and normals of the LAST fixed step (arms per-step capture) */
#define AVER_PHYS_DBG_SUPPORT_FACES 0x00000200u   /* supporting faces found by the narrow phase (arms per-step capture) */
#define AVER_PHYS_DBG_SOFT_VERTICES 0x00000400u
#define AVER_PHYS_DBG_SOFT_EDGES    0x00000800u   /* edge, bend, volume, skin and LRA constraints */
#define AVER_PHYS_DBG_CHARACTERS    0x00001000u   /* character capsules, ground normal, supporting contact (CharacterVirtual is not a body) */
#define AVER_PHYS_DBG_STATIC_TRIANGLES 0x00002000u /* static mesh / heightfield triangles inside the radius, via Shape::GetTrianglesStart */
#define AVER_PHYS_DBG_LINEAR_CAST   0x00004000u   /* where continuous collision ran (arms per-step capture) */
#define AVER_PHYS_DBG_LABELS        0x00008000u   /* handle, mass and speed text per body */
#define AVER_PHYS_DBG_ISLANDS       0x00010000u   /* colour by island (needs a body-colour scheme change only) */

AVER_PHYS_API int32_t aver_phys_debug_set_color_scheme(int32_t scheme);          /* 0 motion type (default), 1 sleep, 2 layer, 3 shape type, 4 island, 5 instance */
AVER_PHYS_API int32_t aver_phys_debug_collect(uint32_t flags, const float* cameraXyz, float radiusCm, int32_t maxLines);
                                                                                /* clears the buffers, runs Jolt's draw calls into the sink, returns the line count (clamped to maxLines);
                                                                                   0 with no world or flags == 0. Main thread, between steps */
AVER_PHYS_API int32_t aver_phys_debug_truncated(void);                           /* 1 if the last collect hit maxLines */
AVER_PHYS_API int32_t aver_phys_debug_lines(float* out, int32_t maxLines);       /* copies up to maxLines lines, 12 floats each: (x y z r g b) x 2, engine cm and axes, colours 0..1 -- exactly two rhi::LineVertex */
AVER_PHYS_API int32_t aver_phys_debug_label_count(void);
AVER_PHYS_API int32_t aver_phys_debug_label_get(int32_t index, float* outXyz, float* outRgb, char* text, int32_t textCap);
AVER_PHYS_API int32_t aver_phys_debug_arm(uint32_t flags);                       /* capture contact / support-face / linear-cast drawing during steps even before collect (the editor calls it every frame the option is on) */
```

- **Sink** (`PhysicsDebugRenderer.hpp`, `PhysicsDebug.cpp`): `LineSinkRenderer : JPH::DebugRendererSimple` (`Jolt/Renderer/DebugRendererSimple.h`) overriding `DrawLine` and `DrawText3D`; its `DrawTriangle` default draws three lines, which is what wireframe wants, and `DrawGeometry` in the Simple base already tessellates `EDrawMode::Wireframe` into `DrawLine`s (`DebugRendererSimple.cpp:50-80`). Positions go through `fromJoltPos` (axis map, m to cm, and in phase H the origin subtraction) so the buffer is already in the engine's frame. `SetCameraPos` is fed from `cameraXyz` for Jolt's LOD choice. The line vector is mutex-guarded because `ContactConstraintManager::sDraw*` calls come from worker threads during `Update`.
- **What `collect` runs**, in this order, each gated by its flag: `system.DrawBodies(settings, sink, &filter)` with a `BodyManager::DrawSettings` built from the flags (`mDrawShape` + `mDrawShapeWireframe` for BODIES, `mDrawBoundingBox`, `mDrawCenterOfMassTransform`, `mDrawVelocity`, `mDrawSleepStats`, `mDrawMassAndInertia`, `mDrawSoftBody*`, `mDrawShapeColor` from the scheme: `Body.h`/`BodyManager.h:243-269`); `DrawConstraints` / `DrawConstraintLimits` / `DrawConstraintReferenceFrame` (`PhysicsSystem.h:181-190`); characters via `CharacterVirtual::GetShape()->Draw(...)` plus the `CharacterVirtual::sDraw*` flags (`CharacterVirtual.h:500-503`); static triangles (below).
- **Armed capture.** Contact points, supporting faces and linear-cast markers are drawn by Jolt *inside* `Update` through the static flags `ContactConstraintManager::sDrawContactPoint` / `sDrawSupportingFaces` / `sDrawContactManifolds` and `PhysicsSystem::sDrawMotionQualityLinearCast` (`ContactConstraintManager.h:250-254`, `PhysicsSystem.h:178`). `aver_phys_debug_arm(flags)` sets those and the step loop clears the contact-class lines at the start of each fixed step, so after a frame's steps the buffer holds the **last** step's contacts (not a smear of every substep). Disarmed, none of it runs; the flags default false, which is the only cost in a normal frame.
- **Filter.** A `BodyDrawFilter` (`BodyFilter.h:116-123`) returns false for a body whose world bounds are farther than `radiusCm` from the camera, and for a body whose shape (unwrapped through scaled, offset-COM and compound wrappers) is a mesh or heightfield. Those are drawn by the **static triangle** pass: for each such body whose bounds intersect the camera sphere, `Shape::GetTrianglesStart(ctx, box, positionCOM, rotation, scale)` and `GetTrianglesNext` (`Shape.h:346-365`) produce only the triangles inside the box, emitted as lines (a city mesh or an 8k heightfield is never drawn whole). Budget: every emit checks `maxLines`; hitting it stops the pass and sets `truncated`.
- **Defaults** the editor uses: radius 4000 cm, `maxLines` 60,000 (a slider to 400,000: at 48 bytes a line and a `createLineMesh` upload per frame that is the soft-body overlay's cost class, UNVERIFIED), colour scheme motion type.
- **Editor** (`sandbox/src/PhysicsDebugOverlay.{hpp,cpp}`, the `SoftBodyDebugOverlay` pattern): `PhysicsDebugOptions` (one bool per flag, radius, max lines, scheme), `physicsDebugCollect(opts, camera, frame)` calls `aver_phys_debug_collect` + `_lines` into `std::vector<rhi::LineVertex>` (a straight copy: the layout is the same) and labels; `SandboxRender.cpp` gets a block beside the soft-body one (`:1995-2021`): destroy last frame's line mesh, collect, `createLineMesh`, `drawLines` with the identity matrix under `!noEditorChrome_`. Labels draw inside the viewport window like the vehicle labels. `Show Colliders` is unchanged; `Show > Physics` is a submenu with: Bodies, Bounds, Centre of mass, Velocity, Sleep, Mass, Constraints, Limits, Contacts, Support faces, Soft body vertices, Soft body constraints, Characters, Static triangles, Continuous collision, Labels, colour-by combo, radius and line-budget sliders, and a "truncated" warning. `uiReg_.track("view.showPhysics")` like its siblings.
- **Backends.** The feed is `rhi::LineVertex`; `drawLines` is implemented on D3D12 (`D3D12Device.cpp:4666`) and Vulkan (`VulkanDevice.cpp:2467`) with the same contract, so parity is by construction and the headed check (6.8) confirms it on `--backend vulkan`. The packaged game has no line overlay path and none is added here; the C ABI and C# `PhysicsDebugDraw` exist for tools and tests.

### 2.9 Determinism, stats tracking, cross-platform

- **Always on, cheap:** the state hash, `aver_phys_hash_log`, thread-count independence test, replay-twice test (section 5.2). These answer "did these two runs diverge, and at which fixed step" without a special build.
- **Diagnostic preset** `AVER_PHYSICS_DIAGNOSTICS=ON` (a developer's CMake preset, documented in `docs/MODULES.md`; never CI): `JPH_ENABLE_DETERMINISM_LOG` (Jolt writes `detlog.txt` in the working directory or `DeterminismLog::sBasePath`, a function-local static the physics module sets from the project's log directory; diff two runs to find the first diverging *value*), the three stats-tracking defines (their CSV goes through `JPH::Trace`, which `traceImpl` already sends to the engine log, `PhysicsWorld.cpp:63-70`; the console command `phys.report broadphase|simulation` calls the always-exported `aver_phys_diag_report(kind)` of 2.2, which in this preset calls `ReportBroadphaseStats()` / `ReportSimulationStats()` (`PhysicsSystem.h:259,264`) and elsewhere returns 0), and the internal profiler. The preset exists because those three need either a different profiler or a slow log and cannot live in a default build.
- **`CROSS_PLATFORM_DETERMINISTIC`** (`AVER_PHYS_CROSS_PLATFORM`, default OFF): Jolt's mode for bit-identical results across compilers and CPUs. We already keep `USE_FMADD OFF` (`physics.jolt/CMakeLists.txt:44`) and SSE4.2 baseline. Turning it on costs speed and constrains the compiler flags for the whole engine's float semantics; **it is not a promise**. Its acceptance test is `tests/physics/PhysicsDeterminismTest` hashing a fixed 600-step scene (boxes, a vehicle, a ragdoll, a character, a cloth) into a golden file, run in the Windows and macOS CI jobs with the option on; the doc claims cross-platform lockstep only after both are green. Engine-side float code inside the fixed step (the vehicle aids, written with plain `float` arithmetic) must also be bit-stable across the two compilers: the test is what finds out, and a failure there is a finding to fix in `PhysicsVehicleExt.cpp`, not a reason to weaken the claim. (Wave 10 VEHICLES 2.15 deliberately made no such claim.)
- **Soft bodies and cross-platform determinism are not assumed.** Jolt's documentation scopes its determinism guarantee; this repo has not read that scoping for soft bodies in 5.6 (not vendored: only `Jolt/` is, `modules/physics.jolt/README.md`), so the golden scene includes a cloth and the result decides the claim.

### 2.10 Large-world coordinates (phase H; **lands LAST, after all five other wave 11 areas**)

This is the one cross-cutting item. It is separated from sections 2.2 to 2.9 so none of them depends on it and it can be done in a quiet tree.

**Problem statement.** f32 gives 0.625 mm out to 8.19 km and 1.25 mm to 16 km, 10 mm at 100 km (`CHUNKS.md` 3.3). The engine is absolute f32 end to end; Jolt is single precision; there is no rebasing code. The owner wants worlds larger than ~10 km with physics that works there. Two layers have to be solved and they have different best answers:

| Layer | Needs | Best mechanism |
|---|---|---|
| Scene graph, renderer, audio, particles, UI | f32 local coordinates near the camera | **Floating origin** (rebase): they stay f32, the origin moves |
| Physics simulation | Stable contacts and no teleport glitch at any distance from the world origin; a rebase must not disturb warm starts | **Double positions inside Jolt** with an offset applied only at the ABI |
| Authoring, saves, net, streaming addresses | Absolute coordinates, exact | f64 / `(region, chunk, local)` (exists: `ChunkCoord`) |

**Decision: both, composed.** The engine owns an **absolute origin** `O` (a `double[3]`, centimetres). Every `float` position that crosses a physics, scene or render ABI means **local = absolute - O**. Physics stores absolute doubles. Rebasing means changing `O` (and shifting the f32 scene), which for physics is a single call that moves nothing.

**2.10.1 Physics ABI (additive, in `physics_world_abi.h`)**

```c
AVER_PHYS_API int32_t aver_phys_origin_set(double x, double y, double z);     /* the world origin O, engine cm. Double build: O(1), valid between steps.
                                                                                Float build: only (0,0,0) is accepted, anything else is refused (Unsupported) */
AVER_PHYS_API int32_t aver_phys_origin_get(double* outXyz);
AVER_PHYS_API int32_t aver_phys_origin_shift(double dx, double dy, double dz);   /* O += delta, then every cached local-frame value inside the module is re-expressed (2.10.3) */

/* absolute (double) positions, engine cm. In a float build these narrow and say so in aver_phys_last_error (Ok but lossy is not reported; callers check aver_phys_precision) */
AVER_PHYS_API int32_t aver_phys_body_position_d(int32_t body, double* outXyz);
AVER_PHYS_API int32_t aver_phys_body_set_position_d(int32_t body, double x, double y, double z);
AVER_PHYS_API int32_t aver_phys_character_position_d(int32_t ch, double* outXyz);
AVER_PHYS_API int32_t aver_phys_character_set_position_d(int32_t ch, double x, double y, double z);
AVER_PHYS_API int32_t aver_phys_vehicle_pose_d(int32_t v, double* outPosXyz, float* outQuat);
AVER_PHYS_API int32_t aver_phys_vehicle_set_pose_d(int32_t v, double x, double y, double z, float yawDeg);
AVER_PHYS_API int32_t aver_phys_raycast_d(double ox, double oy, double oz, float dx, float dy, float dz, float maxDistCm,
                                          int32_t* outBody, double* outPointXyz, float* outNormalXyz);
```

Everything else stays float and means origin-relative: creation (`aver_phys_add_*`), `body_position`, `body_set_position`, queries, contact points, soft-body vertex output, joint anchors, character and vehicle poses. That is deliberate: it keeps the roughly 300 existing exports and every caller unchanged, and a body anywhere within a few kilometres of the *origin* is exact in float. A caller that must place something 50 km from `O` uses the `_d` setters after creation, or (the streaming case) the origin is near it anyway because streaming only loads chunks near the camera and the origin follows the camera's region.

**2.10.2 Inside the module**

- `Convert.hpp` (SHARED with every physics area) gains `toJoltPos(const Vec3& local) -> JPH::RVec3` (`(double)local + O`, then the axis map and cm to m) and `fromJoltPos(JPH::RVec3Arg) -> Vec3` (subtract `O` in double, then cast), plus `toJoltPosD/fromJoltPosD` for the `_d` calls. The direction/scale/unit helpers (`toJoltDir`, `toJoltUnit`, `toJoltAngular`, `toJoltTorque`) are untouched: they are not positions. `O` is stored in Jolt axes and metres in `World` so the conversion is one add.
- **The mechanical migration**: every position-bearing site of the ~100 `toJolt(`/`fromJolt(` calls, ~18 casts, ~20 `CollideShape` calls and ~27 transform sites in `modules/physics/src` moves to `toJoltPos`/`fromJoltPos` and Jolt's `RVec3`/`RMat44` types. In a **double** build the compiler finds the misses (Jolt will not implicitly narrow `DVec3` to `Vec3`), which is why this phase compiles under `AVER_PHYSICS_PRECISION=DOUBLE` first and the float build second. Hit points: `RRayCast` hit = `origin + direction * fraction` in `RVec3`; contact points = `manifold.GetWorldSpaceContactPointOn1(i)` (`ContactListener.h:26`) through `fromJoltPos`; shape-cast and collide queries pass `RVec3` base offsets.
- **Soft bodies.** `SoftBodyCreationSettings::mPosition` is `RVec3`; vertex data stays float relative to the body, so `aver_phys_softbody_vertices` returns `body position - O + local vertex`. A soft body 500 km out is exact because the large number is only ever subtracted from another large number in double before it becomes a small float.
- **Vehicles.** `VehicleConstraint` world-space wheel transforms are `RMat44`; `finish`, `pose`, `wheel_pose`, `set_pose` use `fromJoltPos`. The vehicle step hooks use velocities and local-frame quantities (float, unaffected). The wheel `CONTACT_POINT` telemetry (`VWS 8..10`) goes through `fromJoltPos`.
- **Characters.** `CharacterVirtual` position is `RVec3`; `ExtendedUpdate` and the ground-velocity code work in floats relative to the character. The `characterCarry` vector is a velocity (unaffected).
- **Broad phase limit.** Body bounds stay float (`Body.h:448`). A body at 1000 km has float ULP ~6 cm in its AABB, so its broad-phase box is inflated by that much: conservative, slightly more narrow-phase work, not a correctness problem (UNVERIFIED). The supported simulation radius is therefore stated as **plus or minus 1000 km** from the *world* origin (not from `O`), with a log warning past 500 km at body creation. Worlds beyond that need a second absolute frame (a "world cell" layer), which is out of scope.
- **Precision switch.** `using PReal = JPH::Real;` in `Convert.hpp`; the new helpers compile to float math when `JPH_DOUBLE_PRECISION` is off. `aver_phys_origin_set` in a float build accepts only zero and returns 0 with `AbiError::Unsupported`; `aver_phys_origin_shift` likewise. The engine's rebaser checks `aver_phys_precision() == AVER_PHYS_PRECISION_DOUBLE` (or the module being compiled out) before it enables.

**2.10.3 Rebase mechanics in the engine (`Aver.World`, always built; no Scene dependency for the service itself)**

`world::WorldOrigin` (new, `modules/world/include/aver/world/WorldOrigin.hpp`): the authoritative `double origin[3]`, a **region-anchored X/Y-only policy** (the decision recorded in `CHUNKS.md` 3.4: origin snaps to region origins, 16,384 cm apart at the default chunk size; Z is never shifted because `averAtmoCamAlt()` reads `gCamPos.z`), hysteresis (rebase when the camera leaves the origin's region), and a **participant list**: `registerParticipant(name, fn(const Vec3d& delta))` called in a fixed order at a fixed point of the frame (the start of `onUpdate`, before input and before the physics step, so no system sees half a shift). Participants and what each does:

| System | On a shift of `delta` |
|---|---|
| Physics | `aver_phys_origin_shift(delta)`: O(1); no body moves, no contact cache invalidates, `aver_phys_state_hash` is unchanged |
| Scene | every **root** entity's `CLocal.xf.position -= delta` (children are parent-relative and do not change); `CWorld` recomposition runs in the normal flush; entities with a physics body (`aver_phys_set_entity`) are *not* repositioned from the body this frame (their pose is recomputed from the unchanged absolute body and the new `O` by the usual post-physics write, and equals the shifted root) |
| Camera | position `-= delta` (the camera is a root entity or a rig output) |
| Renderer (Voxi, both backends) | `PerFrameCB` gains a world-origin field (hi/lo float3 so the renderer can reconstruct an absolute position or a stable hash of it); `prevViewProj_` (`VoxiRenderer.cpp`) is composed with the shift translation (exactly as `CHUNKS.md` 3.4 says); per-instance previous-model matrices and the TLAS instance transforms are rebuilt that frame; the shader terms that use an **absolute** world position (cloud noise lattice, `D3D12Device.cpp`'s `wpos * (1/featureSize)`) use `(wpos + O_wrapped)` where `O_wrapped` is `O` modulo the noise period, so the sky is bit-identical across a rebase (the slice 10 acceptance); world-keyed caches (the GI radiance volume, light grids) invalidate and rebuild on the rebase frame through their existing rebuild gates (the gate logs it); denoiser and TAA history survive through the composed `prevViewProj_`. `PerFrameCB` is hand-mirrored across `D3D12Device.cpp`, `RHIShaders.cpp` and every consumer's copy (`CHUNKS.md` 3.4): the field is added to all of them in one commit |
| Particles (CPU and GPU), decals, trails, lights, probes | positions are entity-relative or root entities: shifted with the scene; free-flying CPU particles `-= delta`; GPU particle buffers hold world positions and take a one-pass `-= delta` compute dispatch (GPU_PARTICLES area contract; on both backends) |
| Audio | the listener and 3D voices follow entity positions: shifted with the scene (no audio state is absolute) |
| Navigation (Synapse) | nav grids carry their own origin: `gridOrigin -= delta`; crowd agents are entities |
| Streaming (`world::ChunkStreamer`) | already keys by `ChunkCoord` (absolute i32): unchanged; the rebase only changes how a chunk's local offset is turned into scene positions |
| Landscape | section origins `-= delta`; the physics heightfield body is absolute in double and is **not** touched |
| Editor | selection gizmos, picking rays, the collider/other overlays recompute from entities each frame; cached overlay meshes that hold world-space vertices (the collider overlay's cached line meshes, `colliderOverlaySig_`) are invalidated through their existing signature (add the origin generation to it) |
| Gameplay scripts | C# `Entity.Position` etc. stay local f32; **new** absolute accessors `Entity.WorldPosition` (`Double3`) = local + `O`; a `WorldOriginChanged` event reaches graphs through the named-event seam (`AN_OnEvent`) for scripts that cache positions |

**2.10.4 Networking and saves**

Replicated positions are sent as `(region i32 x3, local f32 x3)` or f64 and re-expressed against each peer's own `O`. Physics snapshots are **origin-independent** (absolute doubles inside Jolt), so rollback and state hashes are identical on peers whose floating origins differ. The `.ocworld` writer's `%.6g` (`OcWorld.cpp:27`, B8) must become `%.17g` (round-trip safe) in this phase or authored positions past ~10 km keep corrupting on every save; `FormatTest` round-trips a 123,456,789.123 cm placement. Saves (`Aver.Save`) store entity transforms as f64 absolute.

**2.10.5 Cost**

- **Memory (Jolt, double build):** `Body` stores an `RVec3` (a `DVec3` is two SSE registers, 32 bytes, against 16 for `Vec3`), `MotionProperties` stores the sleep-test offset as a double triple (+12 bytes), and a contact manifold carries a double base offset. Estimate +32 to +48 bytes per body: at the 65,536-body ceiling about +2 to +3 MB (UNVERIFIED; the headed pass records `sizeof` through a debug print and the body-count stat). Snapshots grow by the same per-body amount.
- **Time:** every position read and write converts through a subtraction/addition in double; Jolt's solver math on velocities, impulses and local-space data is float and unchanged. Expected cost: low single-digit percent on the physics step (UNVERIFIED, no number exists). Phase H step 1 builds `PhysicsBenchTool step` (a 2,000-box pile, a 40-vehicle convoy, a cloth, 20 characters; step ms averaged over 600 fixed steps) and records float versus double before anything else is migrated; if double costs more than 10% of the step it is reported and the owner decides whether `DOUBLE` becomes the default or stays an option.
- **Engine:** the origin service is O(root entities) per rebase and rebases happen once per ~16 km of travel; the renderer cost is one rebuilt TLAS and one dispatch of particle shifting on the rebase frame.

**2.10.6 Keeping the float build working**

`AVER_PHYS_PRECISION=SINGLE` stays a supported configuration: CI keeps one job that configures it, builds `Aver.Physics` and runs `PhysicsTest`, `PhysicsSettingsTest`, `PhysicsStateTest`, `LargeWorldTest` (which detects the precision and asserts the float-build contract: origin refused, `_d` narrowing, ABI otherwise identical). Rebasing is disabled with a log line, so a float build is exactly today's engine. No ABI function is conditional on precision; only `aver_phys_origin_*` behaves differently.

**2.10.7 Phases of H, in order** (each leaves the tree buildable and green)

- **H0 measure.** `PhysicsBenchTool step`, float result recorded in section 8. Audit float position caches inside `modules/physics/src` (anything that stores a world position across steps: soft-body registry impact points, vehicle `lastVel` is a velocity and fine, joint anchors are body-local) and list them.
- **H1 double physics, origin 0.** `Convert.hpp` helpers, the mechanical migration, `AVER_PHYSICS_PRECISION=DOUBLE`, all existing physics suites green in both precisions (tolerances where a float-exact assertion existed; a double build is not bit-identical to a float build and the gates that record physics-driven motion are re-recorded once, owner-approved, UNVERIFIED which ones).
- **H2 origin offset and `_d` ABI**, `LargeWorldTest`: a 100 km offset with sub-millimetre contact stability, rebase mid-simulation with an identical state hash, the float-build contract.
- **H3 engine origin service** (`WorldOrigin`, scene roots, camera, streaming, landscape, nav, audio, particles, editor caches), headless `WorldOriginTest`: a simulated 100 km flight rebases ~6 times; every entity's absolute position (local + `O`) is unchanged within 1 mm; the physics state hash is unchanged.
- **H4 renderer** (the `PerFrameCB` field in all mirrors, `prevViewProj_` composition, TLAS/previous-model refresh, the noise term, GI/light-grid invalidation) on D3D12 and Vulkan. Headed: slice 10's done-when.
- **H5 net and saves f64**, `%.17g` writer, `Entity.WorldPosition`.

### 2.11 C#: state, debug, large world

```csharp
public sealed class PhysicsSnapshot : IDisposable {
    public static PhysicsSnapshot? Capture(SnapshotParts parts = SnapshotParts.All, PhysicsSnapshot? reuse = null);
    public bool Restore(RestoreOptions options = RestoreOptions.None);      // false: see LastResult
    public static RestoreResult LastResult { get; }
    public ulong Hash { get; }  public int ByteCount { get; }  public byte[] ToBytes();  public static PhysicsSnapshot? FromBytes(ReadOnlySpan<byte> data);
    public SnapshotInfo Info { get; }  public void Dispose();
}
public sealed class PhysicsStateRing {                                     // rollback helper: fixed capacity, reuses buffers, keyed by tick
    public PhysicsStateRing(int capacity, SnapshotParts parts = SnapshotParts.All);
    public void Push(long tick);  public bool Rewind(long tick, RestoreOptions options = RestoreOptions.None);  public long OldestTick { get; }
}
public static partial class Physics {
    public static int StepExact(int steps);  public static long StepTotal { get; }
    public static ulong StateHash(SnapshotParts parts = SnapshotParts.All);  public static bool HashLog(string? path);
    public static float Accumulator { get; set; }
}
public static class PhysicsDebugDraw {                                      // for tools and tests; the packaged game has no line path
    public static bool Available { get; }  public static int Collect(PhysicsDebugFlags flags, Float3 camera, float radiusCm = 4000, int maxLines = 60000);
    public static ReadOnlySpan<float> Lines { get; }  public static bool Truncated { get; }  public static IReadOnlyList<DebugLabel> Labels { get; }
}
// phase H
public readonly struct Double3 { public readonly double X, Y, Z; ... }
public static class PhysicsOrigin { public static Double3 Origin { get; } public static bool Set(Double3 o); public static bool Shift(Double3 d); }
public partial struct Body { public Double3 PositionD { get; set; } }          // plus CharacterBody.PositionD, Vehicle.PoseD
```

New files, all in `scripting/csharp/Aver.Physics`: `Native.World.cs`, `Native.State.cs`, `Native.Debug.cs` (partials of `Native`, auto-scanned by the parity test), `PhysicsSettings.cs`, `PhysicsStats.cs`, `PhysicsSnapshot.cs`, `PhysicsDebugDraw.cs`, and in phase H `LargeWorld.cs`. `Body`, `CharacterBody` and `Vehicle` are `readonly struct`s (`Vehicle.cs`); phase H adds the `PositionD` members through `partial` declarations only if they are already partial, otherwise as extension members in `LargeWorld.cs` (checked when the phase starts; the vehicle area made `Vehicle` partial for the same reason).

### 2.12 Aver Node: family `PhysicsWorld` (version 1)

Wave 9 C contract exactly: `GraphPhysicsWorldNodeTable.cs` (`scripting/csharp/Aver.Graph`), `GraphNodeDefsPhysicsWorld.hpp` (`sandbox/src/`, mirrors pin for pin), owner methods as `internal static` wrappers in `scripting/csharp/Aver.Framework/PhysicsWorldGraph.cs`, `[AutoRun]` `PhysicsWorldNodeTests` built from `NodeRowKit`, failures return false/0 and log once per node id, never throw, `docs/nodes/PhysicsWorld.md` generated by `scripts/node-catalog.py --write`. Nothing is edited in `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs` or `Program.cs`. Category `Physics`.

| Key | Kind | Pins (besides `exec` / `then`) | Attr | Wraps |
|---|---|---|---|---|
| `an_physicssetsetting` | Exec | in `value`; out `success:b` | `setting` (a name from the table) | `Physics.SetSetting` |
| `an_physicsgetsetting` | Pure | out `found:b`, `value` | `setting` | `Physics.GetSetting` |
| `an_physicssnapshot` | Exec | out `snapshot:i` (0 = failed), `success:b` | `parts` (all, bodies, characters ...) | `PhysicsSnapshot.Capture` into a graph-owned table |
| `an_physicsrestore` | Exec | in `snapshot:i`; out `success:b` | | `PhysicsSnapshot.Restore` |
| `an_physicsreleasesnapshot` | Exec | in `snapshot:i`; out `success:b` | | `Dispose` |
| `an_physicsstatehash` | Pure | out `low:i`, `high:i` (the two halves of the u64) | | `Physics.StateHash` |
| `an_physicsstats` | Pure | out `bodies:i`, `activeDynamic:i`, `steps:i` (last call), `stepMs`, `updateErrors:i` | | `Physics.GetStats` |

A graph-owned snapshot table caps at 16 live snapshots per world and releases them on level change; the in-session rewind and checkpoint gameplay (a "rewind time" mechanic, an arena reset) is what these rows are for. `StepExact`, the profiler, the debug draw and the large-world origin are **not** nodes: stepping from a graph breaks the tick contract, and the others are tools or engine plumbing (a script reads `Entity.WorldPosition` through C#, phase H, and reacts to `WorldOriginChanged`).

### 2.13 Host wiring

- **Before the first level** (the fix for "physics starts before the project is known"): new `Runtime/include/aver/game/PhysicsProjectApply.hpp` (header-only, like `GameTick.hpp`) with `configureProjectPhysics(project)` (writes the five config keys and the live settings into the pending tables) and `applyProjectPhysics` moved to call it plus the live settings. `GameTick.hpp` keeps `startPhysics` and `applyProjectPhysics` as thin forwards (three lines of edit) so call sites do not change. Order: the host parses the manifest, calls `configureProjectPhysics`, and **if the world is empty and `aver_phys_config_differs()`** calls `aver_phys_rebuild()` (both hosts have an empty world at that moment: Runtime's `initPhysics` precedes `openProject`; the editor's project open follows closing the previous level); otherwise it logs "[Project] PHYSICS.MAXBODIES changes apply when the world is next created". `applyProjectPhysics`'s existing `else if (project.hasGravity ...)` message (`GameTick.hpp:92-95`) becomes true for the first time for every key.
- **The tick:** nothing changes in `tickGameplayGroups` (`GameTick.hpp:146-156`). Netcode will call `aver_phys_step_exact` itself.
- **Editor Play:** Play start and stop do not touch snapshots; `GameSystemsWiring.hpp` is not touched. A level change calls `aver_phys_snapshot_release` for every live snapshot (`PhysicsState.cpp` registers an at-shutdown clear; a handle that outlives its world answers `BAD_HANDLE`).
- **Overlay and panel:** `SandboxApp.hpp` members (`physicsDebug_`, `physicsDebugFrame_`, `physicsDebugMesh_`), the `Show > Physics` menu (`SandboxShell.cpp` beside `Show Colliders`, `:2218`), the render block (`SandboxRender.cpp`), the profiler header (`SandboxShell.cpp:150-260`); each a few lines because the logic is in new files.

### 2.14 What the hooks cost when unused

World without any of this used: one relaxed atomic load per Jolt profile scope (sink null), the debug-renderer flag checks Jolt already does, the `EPhysicsUpdateError` OR per step, a `steady_clock` pair around `Update` for stats (about 40 ns per fixed step). No allocation per step. A test (5.1) asserts the per-step allocation count is unchanged.

---

## 3. FILE OWNERSHIP

### 3.1 Created (only this area touches them)

Physics ABI and sources
- modules/physics/include/aver/physics/physics_world_abi.h
- modules/physics/include/aver/physics/physics_state_abi.h
- modules/physics/include/aver/physics/physics_debug_abi.h
- modules/physics/src/PhysicsConfig.hpp
- modules/physics/src/PhysicsConfig.cpp
- modules/physics/src/PhysicsStats.cpp
- modules/physics/src/PhysicsProfile.hpp
- modules/physics/src/PhysicsProfile.cpp
- modules/physics/src/PhysicsState.cpp
- modules/physics/src/PhysicsStateEngine.cpp
- modules/physics/src/PhysicsStateRecorder.hpp
- modules/physics/src/PhysicsShapeCache.cpp
- modules/physics/src/PhysicsDebug.cpp
- modules/physics/src/PhysicsDebugRenderer.hpp
- modules/physics/src/PhysicsDeterminism.cpp
- modules/physics/src/PhysicsLargeWorld.cpp   (phase H: origin, `_d` functions)
- modules/physics.jolt/AverProfileHook.cpp

Host and world
- Runtime/include/aver/game/PhysicsProjectApply.hpp
- modules/world/include/aver/world/PhysicsShapeCache.hpp
- modules/world/src/PhysicsShapeCache.cpp
- modules/world/include/aver/world/WorldOrigin.hpp   (phase H)
- modules/world/src/WorldOrigin.cpp   (phase H)

Editor (sandbox)
- sandbox/src/PhysicsConsoleVars.hpp
- sandbox/src/PhysicsSettingsModel.hpp
- sandbox/src/PhysicsSettingsPage.cpp
- sandbox/src/PhysicsDebugOverlay.hpp
- sandbox/src/PhysicsDebugOverlay.cpp
- sandbox/src/PhysicsProfilePanel.cpp
- sandbox/src/GraphNodeDefsPhysicsWorld.hpp

Scripting
- scripting/csharp/Aver.Physics/Native.World.cs
- scripting/csharp/Aver.Physics/Native.State.cs
- scripting/csharp/Aver.Physics/Native.Debug.cs
- scripting/csharp/Aver.Physics/PhysicsSettings.cs
- scripting/csharp/Aver.Physics/PhysicsStats.cs
- scripting/csharp/Aver.Physics/PhysicsSnapshot.cs
- scripting/csharp/Aver.Physics/PhysicsDebugDraw.cs
- scripting/csharp/Aver.Physics/LargeWorld.cs   (phase H)
- scripting/csharp/Aver.Framework/PhysicsWorldGraph.cs
- scripting/csharp/Aver.Graph/GraphPhysicsWorldNodeTable.cs
- scripting/csharp/Aver.Graph.Tests/PhysicsWorldNodeTests.cs
- scripting/csharp/Aver.Framework.Tests/PhysicsStateApiTests.cs
- docs/nodes/PhysicsWorld.md   (generated)

Tests
- tests/physics/src/PhysicsSettingsTest.cpp
- tests/physics/src/PhysicsConfigTest.cpp
- tests/physics/src/PhysicsStateTest.cpp
- tests/physics/src/PhysicsDeterminismTest.cpp
- tests/physics/src/PhysicsShapeCacheTest.cpp
- tests/physics/src/PhysicsDebugTest.cpp
- tests/physics/src/PhysicsProfileTest.cpp
- tests/physics/src/LargeWorldTest.cpp   (phase H)
- tests/physics/src/PhysicsBenchTool.cpp   (a tool, not a ctest)
- tests/world/src/PhysicsShapeCacheFileTest.cpp
- tests/world/src/WorldOriginTest.cpp   (phase H)
- tests/editor/src/PhysicsSettingsModelTest.cpp
- tests/editor/src/PhysicsDebugOverlayTest.cpp
- tests/game/src/ProjectPhysicsApplyTest.cpp

### 3.2 Modified, owned by this area

- modules/physics.jolt/CMakeLists.txt   (the five options of 2.1; this file is ours, not vendored)
- tests/physics/CMakeLists.txt   (the new `add_executable`s; append-only block)
- docs/plans/wave11/WORLD_STATE_DEBUG.md   (section 8 as built)

### 3.3 SHARED (another wave 11 area or the integrator also touches these; every edit is small, additive and described)

- modules/physics/src/PhysicsInternal.hpp
- modules/physics/src/PhysicsWorld.cpp
- modules/physics/src/Convert.hpp   (phase H only)
- modules/physics/src/PhysicsJoints.cpp   (one `noteConstraintTopology()` call per add/remove site; phase H position migration)
- modules/physics/src/PhysicsVehicle.cpp   (same one-liner; phase H migration)
- modules/physics/src/PhysicsRagdoll.cpp   (same one-liner; phase H migration)
- modules/physics/src/PhysicsBody.cpp, PhysicsBodyExt.cpp, PhysicsCharacter.cpp, PhysicsLayers.cpp, PhysicsQueries.cpp, PhysicsShapes.cpp, PhysicsSoftBody.cpp, PhysicsVehicleExt.cpp, PhysicsVehicleTracked.cpp, Buoyancy.cpp, BuoyancyAbi.cpp, SoftBodyAero.cpp   (phase H mechanical position migration **only**)
- modules/physics/include/aver/physics/physics_abi.h   (not edited; named because its doc comments on `aver_phys_init` / `aver_phys_max_bodies` are corrected by the integrator to point at the config keys)
- modules/physics/CMakeLists.txt
- scripting/csharp/Aver.Physics/Native.cs   (not edited: new natives live in `Native.*.cs` partials; named because phase H moves nothing in it)
- tests/abi/src/AbiEnumTest.cpp
- CMakeLists.txt   (only if `tests/world` or `tests/game` registration is not already a root walk; verify)
- modules/formats/include/aver/formats/OcProject.hpp
- modules/formats/src/OcProject.cpp
- modules/formats/include/aver/formats/Avr1.hpp   (the `PSHP` subtype constant)
- modules/formats/src/OcWorld.cpp   (phase H: `%.17g`)
- tests/formats/src/FormatTest.cpp
- modules/world/CMakeLists.txt
- modules/world/include/aver/world/LevelInstance.hpp, modules/world/src/LevelInstance.cpp   (shape-cache options and the cache call)
- Runtime/include/aver/game/GameTick.hpp
- Runtime/src/GameApp.cpp, Runtime/src/GameContent.cpp, Runtime/src/GameLandscape.cpp
- sandbox/src/SandboxApp.hpp, SandboxShell.cpp, SandboxRender.cpp, SandboxSettings.cpp, SandboxProject.cpp, SandboxInit.cpp, EditorConsole.hpp, SandboxViewport.cpp (phase H overlay signature)
- sandbox/CMakeLists.txt
- scripting/csharp/Aver.Framework/*.cs   (phase H: `Entity.WorldPosition`)
- modules/rhi/src/RHIShaders.cpp, modules/rhi.d3d12/src/D3D12Device.cpp, modules/rhi.vulkan/src/VulkanDevice.cpp, modules/render.voxi/src/VoxiRenderer.cpp and the shaders that read absolute positions   (phase H4 only)
- docs/ABI.md, docs/MODULES.md, docs/ARCHITECTURE.md, docs/SCRIPTING_API.md, docs/PROJECTS.md, docs/CHUNKS.md, docs/formats/FORMAT_SPECS.md, modules/physics/README.md, modules/physics.jolt/README.md, CHANGELOG.md

| Shared file | Edit made by this area | Who else |
|---|---|---|
| `PhysicsInternal.hpp` | `World::jobs` becomes `unique_ptr<JPH::JobSystem>`; `World::temp` becomes the tracking allocator type; new members `settingsRev`, `topologyEpoch`, `constraintEpoch`, `stepsTotal`, `lastUpdateError`; declarations `noteConstraintTopology()`, `runOneFixedStep()`, `registerStateParticipant`, the `AVER_PHYS_SCOPE` macro | SOFTBODY_DEPTH, hair, any area adding `World` members |
| `PhysicsWorld.cpp` | `aver_phys_init` reads the pending config (defaults equal today's literals) and applies settings; `aver_phys_step` split into prologue / `runOneFixedStep` / epilogue (a pure move); `Update` gets `collisionSteps` and its returned error is kept; six `AVER_PHYS_SCOPE` lines; stats clocks; debug-clear and hash-log calls in the loop; `aver_phys_shutdown` clears snapshots | every area that edits the step loop |
| `PhysicsJoints/Vehicle/Ragdoll.cpp` | one `noteConstraintTopology()` call at each constraint add/remove (3 to 6 lines total) | their owners |
| `Convert.hpp` + all physics `.cpp` | phase H only: `toJoltPos`/`fromJoltPos` and the position-site migration | all physics areas: **this is why H lands last** |
| `AbiEnumTest.cpp` | the `headers[12]` array is full (11 + terminator): **widen it to 24**, then add the three new headers to the physics row and `AVER_PHYS_WS_*` / `_CF_*` / `_ST_*` / `_SNAP_*` / `_DBG_*` constant mirrors to the index-constant checks | every area adding a physics header |
| `OcProject.hpp/.cpp`, `FormatTest.cpp` | `physicsSettings`, `physThreads`, the name mirror, parse/write/owned-key list | none expected |
| `GameTick.hpp`, `GameApp.cpp`, `SandboxProject.cpp`, `SandboxInit.cpp` | forward to `PhysicsProjectApply.hpp`; the configure-before-level call | cameras, particles |
| `EditorConsole.hpp` | one `registerPhysicsVars(t)` line and the new commands | other families append their own |
| `SandboxApp.hpp/Render/Shell/Settings` | members, menu, render block, panel header, page call | soft body, vehicles, cameras, particles |
| `Avr1.hpp`, `LevelInstance.*`, `GameContent.cpp`, `GameLandscape.cpp` | `PSHP` subtype; shape-cache call sites | none expected |

Not touched: `scene_abi.h`, `Components.hpp` (the origin shift only edits `CLocal` values), `framework_abi.h`, `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs`, `Program.cs`, every shader before phase H4, `modules/render*` before H4, `modules/physics.jolt/Jolt/**` (vendored; the hook TU and options live outside it).

---

## 4. Phased implementation

Each phase ends with its tests green and the tree buildable. Sizes: A M, B M, C L, D L, E M, F M, G M, H XL. **A to G (overall L) have no dependency on the other wave 11 areas and can run in parallel with them; H starts only after all five other wave 11 areas are merged.** Never rebuild over the owner's running editor; headed runs are the owner's. The implementing agents use Sonnet workflows with the fewest stages.

**A: settings, config, stats, jobs (M).** `physics_world_abi.h`, `PhysicsConfig.*`, `PhysicsStats.cpp`, `World` member changes, `aver_phys_init` reading the pending config, `aver_phys_rebuild`, `collisionSteps` in the loop, the tracking temp allocator, update-error log, job system choice (single-threaded mode). `OcProject` keys, `PhysicsProjectApply.hpp`, host call order, C# `Native.World.cs` + `PhysicsSettings.cs` + `PhysicsStats.cs`, `AbiEnumTest` widened and extended, console vars, Project Settings page and model. Exit: `PhysicsSettingsTest`, `PhysicsConfigTest`, `ProjectPhysicsApplyTest`, `FormatTest`, `PhysicsSettingsModelTest`, `AbiEnumTest` green; the three existing physics suites unchanged and green (the identity test).

**B: profiler bridge (M).** The `AVER_PHYSICS_PROFILER` option, `AverProfileHook.cpp` in the Jolt target, `PhysicsProfile.*`, the six scopes, the ABI table, the editor panel header and the hitch append. Exit: `PhysicsProfileTest` green; measured overhead with the sink null recorded in section 8 (decision rule in 2.5).

**C: debug renderer and overlay (L).** `AVER_PHYSICS_DEBUG_RENDERER`, `physics_debug_abi.h`, `LineSinkRenderer`, the collect pipeline, filter, static triangles, armed capture, `PhysicsDebugOverlay`, `Show > Physics`, C# `PhysicsDebugDraw`. Exit: `PhysicsDebugTest`, `PhysicsDebugOverlayTest` green; build compiles with the option off (the ABI then answers `available = 0`).

**D: state recording (L).** `ByteStateRecorder`, snapshots, the manifest and validation, the engine section, `noteConstraintTopology()` in the three files, `step_exact` and the step split, hash, hash log, `PhysicsSnapshot`/`PhysicsStateRing`, console commands, node family `PhysicsWorld`. Exit: `PhysicsStateTest`, `PhysicsDeterminismTest` (single-precision, single-platform parts), `PhysicsWorldNodeTests`, `PhysicsStateApiTests` green.

**E: shape cache (M).** Step 1 measure (`PhysicsBenchTool shapes`), record, decide. If go: `physics_state_abi.h` shape functions, `create_heightfield_shape`, `PSHP` subtype, `world::PhysicsShapeCache`, the two call sites, optionally the landscape and the `AverAssetC cook-collision` verb. Exit: `PhysicsShapeCacheTest`, `PhysicsShapeCacheFileTest` green (the ABI half ships regardless of the gate).

**F: determinism tools (M).** `AVER_PHYSICS_CROSS_PLATFORM` and `AVER_PHYSICS_DIAGNOSTICS` options, the diagnostic report call, the golden-hash scene, the CI jobs (a float-precision job now; the cross-platform job runs the golden test on Windows and macOS with the option on). Exit: the golden test green on both CI OSes, or a recorded finding (then the claim stays unmade).

**G: docs, content hooks, polish (M).** `docs/nodes/PhysicsWorld.md` generated, `ABI.md`, `MODULES.md`, `PROJECTS.md`, `FORMAT_SPECS.md`, `SCRIPTING_API.md`, READMEs, `module-catalog.py --write`, `module-guard-audit.py`, `docs-check.py`, `CsharpApiIndexCheck`, `NodeCatalogCheck`. Exit: those checks and the full `ctest` green.

**H: large world, LAST (XL).** Sub-phases H0 to H5 in 2.10.7. Prerequisite: A to G merged **and** every other wave 11 area merged (they all add or change position-bearing physics calls). Exit per sub-phase as listed; overall exit is the headed section 6.12 to 6.15.

---

## 5. Tests

Everything below runs without a GPU or a window except where marked. `*Test` executables are picked up by the root walk; each phase adds only an `add_executable`.

### 5.1 `tests/physics` (link `Aver.Physics Aver.Core`)

- `PhysicsSettingsTest`: (1) table: every key has a name, a range containing its default, `setting_find(name(k)) == k`, case-insensitive; (2) **identity**: a world that sets nothing steps a 300-step scene (box pile, sphere, character, vehicle) to bit-identical state hash as the same scene on the pre-change binary's golden hash (recorded once in phase A); (3) `velocitySteps` 10 versus 2 changes a stacking test's settle jitter in the expected direction; `velocitySteps` below 2 is clamped to 2; (4) `collisionSteps` 2 doubles Jolt's collision rate (a fast sphere through a thin wall tunnels at 1, holds at 4; UNVERIFIED thresholds, tuned in the test); (5) `allowSleeping 0` keeps a resting box active for 10 s, 1 sleeps it after `sleepTime`; (6) `deterministic 0` is accepted and logs; (7) setting before `aver_phys_init` is stored and applied; survives shutdown; `reset(-1)` restores every default; (8) out-of-range clamps, NaN refused.
- `PhysicsConfigTest`: (1) a tiny `MAX_CONTACTS` plus a pile triggers `UPDATE_ERRORS` bit 0 or 2 and exactly one log line per world; (2) `config_set` on a live world sets `config_differs`; `rebuild` refused with one body present (world unchanged), accepted when empty and then `config_active` equals the pending value and `aver_phys_max_bodies()` follows; (3) `THREADS` 0, 1, 4: `WORKER_THREADS` reports each; (4) `MAX_BODIES` 1024 refuses the 1025th body with the existing one-time warning; (5) shape handles survive a rebuild; (6) per-step allocation count with nothing enabled equals the pre-change count (a counting `operator new` hook around 100 steps).
- `PhysicsStateTest`: (1) *round trip*: capture at step 120 of a scene with 50 boxes, a ragdoll, a vehicle, a cloth, a character, a breakable joint; run 120 more steps recording the hash every step; restore; run 120 again; **every per-step hash equal**; (2) *validation first*: remove a captured body, restore returns 0 with `TOPOLOGY` and the world's hash is unchanged; add a body, restore succeeds and the new body is untouched; add under `STRICT`, refused; (3) create a joint after the capture, restore refused (`constraintEpoch`); (4) vehicle aid state: a car mid-ABS-brake restored from a snapshot reproduces the same brake factors (ABS and TCS flags compared through `_state`); (5) character ground carry: a character on a moving platform restores and keeps riding; (6) events cleared on restore; (7) `reuse` handle overwritten without growth (the byte vector capacity is stable after warm-up: observed through `aver_phys_snapshot_bytes` and an allocation counter); (8) export/import: bytes written, imported into a fresh world of the same build, restored: same hash; a header with a flipped precision byte is `BUILD_MISMATCH`; truncated is `BAD_HEADER`; garbage payload is `CORRUPT` with nothing applied; (9) statics excluded by default: snapshot bytes of a 20,000-static scene are within 5% of the same scene with 0 statics; (10) `step_exact(7)` runs exactly 7 steps and leaves `accumulator_get` unchanged; (11) breakable-joint broken state restored (a broken joint un-breaks on restoring an earlier snapshot); (12) soft-body vertices restore (cloth hash equal).
- `PhysicsDeterminismTest`: (1) replay twice from the same start with the same inputs: per-step hash equal across 600 steps; (2) **thread-count independence**: 0, 1, 4 threads, equal hash every step; (3) validating recorder: snapshot, step, restore with `SetValidating` finds no mismatch for a deterministic scene; (4) the hash log file: `step hash` lines, two runs diff-equal; (5) the cross-platform golden: compares against `tests/physics/data/golden_hash_<precision>.txt` only when `AVER_PHYSICS_CROSS_PLATFORM` is on (skipped, loudly, otherwise).
- `PhysicsShapeCacheTest`: (1) mesh shape saved and restored behaves identically (raycast, drop test, `aabb`) to the original; (2) heightfield likewise; (3) wrong `key` is a miss (0 handle, no log spam); a wrong version, wrong precision byte, truncated and corrupt blob are misses; (4) restore from two threads at once; (5) blob size is reported and bounded (a regression guard against accidentally saving the triangle list twice).
- `PhysicsDebugTest` (skips with a message if `available == 0`): (1) a box and a sphere with `BODIES` produce lines whose bounds equal the shapes' AABBs within 1 cm, in engine axes (+Z up, +X forward: a box taller than wide proves the axis map); (2) radius culls: a body 100 m away is absent at radius 40 m, present at 150 m; (3) `maxLines` truncates and sets the flag; (4) static triangles: a 50,000-triangle mesh 10 m away yields only triangles inside the radius, count far below the mesh's; a heightfield likewise; (5) `CONTACTS` armed: a resting box produces contact lines from the last fixed step only (count equal across 1 and 4 steps per call); disarmed, none; (6) constraints draw a hinge's frame; (7) labels carry the body handle; (8) `flags == 0` and no-world return 0; (9) line buffer layout is 12 floats per line and colours lie in 0..1.
- `PhysicsProfileTest` (skips if no hook): enable, step a loaded scene, rows include `Update`, `PhysicsStep` children and Aver's six scopes; every row's inclusive >= exclusive; parent indices valid and acyclic; `reset` clears; disabled leaves the table frozen; the scope names merge across threads (no duplicate names at one depth).
- `LargeWorldTest` (phase H; parameterised on `aver_phys_precision()`): double build: (1) a box dropped at `O + (0, 0, 500)` with `O` = (1e8, 0, 0) cm lands and rests with sub-millimetre jitter; the same scene at `O = 0` produces local positions within 1e-3 cm; (2) **rebase mid-simulation**: run 100 steps, `origin_shift`, run 100 more: local positions shift by exactly the delta and `aver_phys_state_hash` equals the no-rebase run's at every step; (3) raycast/overlap/contact points round-trip through the origin; (4) vehicle and character 200 km out drive/walk with the same telemetry as at the origin (within the float-local tolerance); (5) soft body 500 km out has finite, stable vertices; (6) `_d` setters/getters round-trip 1e9 cm to 1e-3 cm; (7) snapshot taken before a rebase restores after it. Float build: origin non-zero refused with `Unsupported`; `_d` narrow; everything else identical.

### 5.2 Other C++

- `tests/world/PhysicsShapeCacheFileTest`: AVR1 `PSHP` round trip; key from (path, size, mtime) changes with each; a stale file is a miss; a half-written file is a miss (the write is temp-then-rename); the cache directory missing is a miss.
- `tests/world/WorldOriginTest` (H3): participants run in registration order exactly once per shift; Z is never shifted; hysteresis (a camera jittering across a region edge rebases once); a simulated 100 km flight at 200 m/s rebases ~6 times; every entity's absolute position (local + origin) preserved within 1 mm; physics hash unchanged; a participant that throws (returns false) aborts with a logged error and the origin is left unchanged (the shift is applied only when all participants accepted, with the physics call last).
- `tests/editor/PhysicsSettingsModelTest`: rows from the ABI table, validation, "changed" detection, canonical omission of defaults, restart badge logic, tooltip text carries unit and default. `PhysicsDebugOverlayTest`: options to flags mapping, `physicsDebugCollect` against a fake feed (line count, vertex layout, label pass-through), the cached mesh is dropped when the origin generation changes (H).
- `tests/game/ProjectPhysicsApplyTest` (links Formats + Physics + Runtime headers): a manifest with the keys above parses; `configureProjectPhysics` + `rebuild` on an empty world leaves `config_active == key`; non-empty world keeps limits and logs; the **name mirror equals `aver_phys_setting_name`** for every setting; round trip through `writeOcproject` preserves order and omits defaults; unknown `PHYSICS.` keys survive a re-save (owned-key list).
- `tests/formats FormatTest`: the new keys; a `%.17g` placement (H5).
- `tests/abi AbiEnumTest`: the three new headers against `Native.World.cs`, `Native.State.cs`, `Native.Debug.cs` (names, parameter names, types incl. `uint64_t`/`ulong`, `uint8_t*`/`byte[]`); the `AVER_PHYS_WS_*`, `_CF_*`, `_ST_*`, `_SNAP_*`, `_RESTORE_*`, `_DBG_*` constants against C# enums.

### 5.3 C# (Windows only, ctest `GraphManagedTests`)

`PhysicsWorldNodeTests` (the 7 rows through `NodeRowKit.CheckRows`, `CheckDefaultPins`, `CheckIlValid`; exec rows refused by the pure compiler; failures return false rather than throw with no world; `an_physicsstatehash` pure); `PhysicsStateApiTests` (`PhysicsSnapshot.Capture/Restore/Hash`, `PhysicsStateRing` push 200 ticks into a ring of 64 and `Rewind(OldestTick)` works, `Rewind(OldestTick - 1)` is false; `Physics.SetSetting` unknown key false; settings enum equals the native name table). Python: `NodeCatalogCheck` (table to palette parity for the 7 rows), `CsharpApiIndexCheck`.

---

## 6. Headed manual checks for the owner

Use NeonDistrict and the vehicle lab (`test-content/VehicleLab`) in the editor, then the packaged runtime, then `--backend vulkan`. The GPU must be idle (the owner plays other games), screenshots at 1440p with a ping for remote runs.

1. **Settings page.** Project Settings > Physics: change velocity steps and sleep time, Play; stacked crates settle differently; "restart world" badge appears on Max bodies; reopen the project: the limits are in effect (the stat line shows the new `MAX_BODIES`).
2. **Console.** `vars phys`, `set phys.velocitySteps 4`, `get phys.stat.stepMsAvg`; `--set phys.velocitySteps 4` on the command line.
3. **Update errors.** Set `PHYSICS.MAXCONTACTS 256`, drop 500 boxes: one red line in the log naming the key, the profiler stats line goes red, nothing else breaks. Restore the key.
4. **Profiler.** Enable the Physics (Jolt) header in Play: tree rows for broad phase, narrow phase, solver, CCD and the Aver stages; numbers move when a crate pile is spawned; record the overhead with the hook idle (item 14).
5. **Debug overlay.** `Show > Physics`: Bodies, Constraints (a ragdoll and a vehicle), Contacts (a crate resting: points and normals; they do not smear when the frame runs several fixed steps), Soft body vertices, Static triangles near a building (only nearby triangles appear; walk away and they follow), Characters, Labels. Truncation warning appears with the line budget at 5,000 on NeonDistrict.
6. **Snapshot rewind.** In Play, `phys.snapshot 1`, knock over a crate stack, drive the lab car, `phys.restore 1`: the world returns exactly (crates, car pose, gear, cloth); `phys.hash` printed before and after restore is equal; spawn a crate after the snapshot and restore: the crate stays, the rest rewinds; remove a captured crate and restore: refused with the topology message, world untouched.
7. **Determinism.** `phys.hashlog on run1.txt`, a scripted 30 s scene, again to `run2.txt` after relaunch: the files are identical; repeat with `PHYSICS.THREADS 0`: identical to `run1`.
8. **Backends and hosts.** Repeat 5 on `--backend vulkan` (the lines look the same). In the packaged `AverEngineRuntime.exe` the `PHYSICS.*` keys apply (limits, steps) and the game plays; there is no overlay there by design.
9. **Shape cache (if phase E went).** First launch of NeonDistrict logs the cache fills; second launch is faster to the first frame by the amount `PhysicsBenchTool` predicted; touch a mesh source and only that mesh rebuilds.
10. **Threads.** `PHYSICS.THREADS 2` versus auto on NeonDistrict: record step ms for each; single-threaded mode runs correctly and slower.
11. **Cross-platform claim.** Read the CI result of the golden hash test on Windows and macOS (section 2.9); no claim unless both are green.
12. **Large world (H).** A test level whose origin region is 80 km from the world origin: the ground, a car and a crate pile behave as at the origin; no vibration (the far-origin symptom, `aver-far-origin-vibration`); the Stats line shows the origin.
13. **Rebase (H).** Fly the editor camera along +X at speed for 100 km: the sky is unchanged across each rebase (screenshots before and after a rebase frame, 1440p), shadows and reflections do not glitch, a car left running keeps driving across a rebase, and `phys.hash` before and after a rebase is equal.
14. **Cost.** With the GPU idle, NeonDistrict and the 2,000-box pile: step ms float versus double (item H0/H2), profiler hook null versus enabled, debug overlay on versus off. Record the numbers in section 8; they are the first of their kind.
15. **Float build.** Configure with `-DAVER_PHYSICS_PRECISION=SINGLE` in a separate build tree (never over the owner's running editor): the engine runs, rebasing is disabled with a log line, behaviour equals today's.
16. Full `ctest`, the graph suite via `dotnet run`, open `docs/nodes/PhysicsWorld.md` and confirm 7 rows documented without hand edits.

---

## 7. Open questions (each with the default chosen; proceed unless told otherwise)

1. **Double precision becomes the default build** once H passes. Default: yes, with the float CI job kept. If the benchmark in H0/H2 shows more than 10% step cost, report to the owner before flipping.
2. **Profiler default in Release.** Default: compiled in for Debug and Release, off at run time. If the idle-hook overhead is over 0.3 ms per frame in Release on NeonDistrict, switch to Debug only.
3. **Debug renderer in Release.** Default: compiled in (the editor ships as Release). Costs binary size only.
4. **`PHYSICS.THREADS` default.** Default: -1, today's formula. A lower default is a performance decision for the owner after item 10.
5. **Cross-session physics state.** Not supported through snapshots (handles change on reload). Default: in-session only; `Aver.Save` keeps owning cross-session state (transforms, velocities if it chooses). If the owner wants exact physics resume across launches, the design is a stable per-body identity (entity id or placement index) in the manifest instead of handles; deferred.
6. **Statics in snapshots.** Default: excluded. A game that moves statics (streaming) and wants them in a rollback window passes `SNAP_STATICS`.
7. **Hash function.** Default: FNV-1a 64 (already in Core, no dependency, ~1 GB/s: a 320 KB state hashes in ~0.3 ms). xxHash64 exists in `Aver.Formats` but physics depends on Core only.
8. **Rebuild at project open.** Default: when the world is empty at manifest time (both hosts, section 2.13). If the editor ever opens a project with a level already loaded, the limits apply on the next world creation and the page says so.
9. **Z is never rebased.** Default: per `CHUNKS.md` 3.4 (atmosphere reads `gCamPos.z`). Physics supports any offset; the policy lives in the engine's service.
10. **Supported simulation radius** plus or minus 1000 km. Default: that, with a warning at 500 km. Beyond needs a second absolute frame; out of scope.
11. **Hair and Jolt compute backends.** `JPH_USE_DX12` / `JPH_USE_VK` stay OFF in this area (`physics.jolt/CMakeLists.txt:63-65`); the Hair area owns turning them on or porting the shaders. The one overlap: Hair's collider feed and `Convert.hpp`; if Hair lands first, its position-bearing calls are migrated in H like everything else.
12. **Ragdoll mode and soft-body impact slots in snapshots.** Default: not captured (inputs/configuration); SOFTBODY_DEPTH may register a state participant if it wants exact replay of impact events.
13. **Rollback netcode ring policy and delta compression** are wave 10 B's; this area ships `PhysicsStateRing` only as a convenience with a fixed capacity.
14. **Patent notes, collected.** Everything here is long-public, generic technique: solver settings (Jolt, MIT), state save/restore for replay and rollback (Jolt's own `StateRecorder`; the technique is decades old and generic), floating-origin rebasing (used by many engines for over twenty years), FNV-1a (public domain), line-feed debug drawing, scope profilers. **No patent search was run** (consistent with the parity plan's rule for anything vendor-shaped, none of it is Epic- or NVIDIA-shaped here beyond the long-public floating-origin idea). No vendor code or control law is copied; no third-party library is added; Jolt is MIT and its notice already ships (`scripts/stage-payload.ps1`). Hair-specific patent notes belong to the Hair area.

---

## 8. As built: deviations and notes

Written by the implementing agent after each phase; nothing here has been compiled or run yet. Each line says what the plan said, what the code does, and why. Also record here: the phase E measurement table (build / save / restore ms and blob sizes), the profiler idle-hook overhead, the float-versus-double step benchmark, the Debug object section counts for any `sandbox` translation unit that grew (the COFF cap note in the memory index), and the audit list from H0.
