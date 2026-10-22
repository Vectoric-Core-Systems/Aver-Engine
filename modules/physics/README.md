# Aver.Physics  (`modules/physics`)

- **Language:** C++
- **Depends on:** Core, and `Aver.Physics.Jolt` **privately**
- **Planned phase:** 4

Rigid-body dynamics + collision behind an AvPhysics facade: bodies, shapes, broadphase, raycasts (aero ride-height probe), contact callbacks.

Implemented and wired. Built as `Aver.Physics` (SHARED, so the C# layer P/Invokes one binary and
therefore one world), with the backend linked `PRIVATE` — nothing above this module can include a
`JPH::` header, so replacing the backend stays a decision about this module rather than about the
tree.

## The backend is a sibling module: [`physics.jolt`](../physics.jolt/README.md)

This pair is `rhi` + `rhi.d3d12` again, and the naming is the same claim: the module here states a
contract, and the dotted one satisfies it. Jolt is to physics what WASAPI is to the mixer and D3D12
is to the RHI — the engine's backend, not a dependency the build happens to pull in.

Two properties follow from that split, and both are enforced by a link line rather than a convention:

- **The ABI above is expressible without the backend.** `physics_abi.h` names no `JPH::` type, and
  could not, because nothing that includes it links anything that defines one.
- **The exception is visible.** `tests/physics` links `Aver.Physics.Jolt` directly and is the only
  thing in the tree that does, because checking that the change of basis commutes with rotation
  needs to speak both sides. A second consumer appearing there would mean the rule had stopped
  holding.

Jolt's sources are **vendored** — MIT, 5.6.0, not edited here. The provenance, the conventions table
(all four of Jolt's differ from the engine's) and the update procedure live in that module's README.

- **Soft bodies:** `physics_softbody_abi.h` (`aver_phys_sb_*`) is the builder-style surface for cloth and
  destruction (in-place dents, rebuild under the same handle, aero, impact events), driven by
  `Aver.SoftBody.Scene`. `aver_phys_softbody_*` is unchanged. See `docs/physics/SOFTBODY_DESTRUCTION.md`.
- **Frame loop:** `SandboxApp` starts the simulation in `onInit`, steps it in `onUpdate` between the
  `PRE_PHYSICS` and `PHYSICS` tick groups, and shuts it down first in `onShutdown` (Jolt owns worker
  threads, which must be joined while what their jobs touch is still alive).
- **Gameplay:** `AverCharacter` is a Jolt `CharacterVirtual` capsule — it falls, collides, reports
  grounded, and jumps. `Physics` and `Body` expose bodies and raycasts to C#.
- **Units and axes:** everything crossing this boundary is the ENGINE's contract (centimetres, +X
  forward, +Y right, +Z up, left-handed). See `src/Convert.hpp`; all four conventions differ from
  Jolt's, so the translation is real arithmetic, not a relabelling.
- **Tests:** `tests/physics` has 413+ assertions across 9 suites. `PhysicsTest.cpp` checks the change
  of basis by the property that defines it — `convert(rotate(q, v)) == rotate(convert(q), convert(v))` —
  which cannot hold by accident for a mirrored axis map. `CharacterTest` verifies character-on-ground
  behaviour (`aver_phys_character_grounded`/`ground_state`, kerb step-up, max slope angle). The
  `--play-test` harness in `SandboxApp` begins Play, holds synthetic `W` for 150 frames and logs where
  the pawn walked to, verifying that movement works end-to-end.

See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the full module DAG.

## World configuration, stats and rebuild: `physics_world_abi.h`

The solver settings Jolt exposes (27: velocity and position steps, collision steps per fixed step, slop, sleeping, restitution speed, the caches ...) are one table in `src/PhysicsConfig.cpp`; `aver_phys_setting_*` reads and writes it, a set applies to the live world at once, and the values live in the process so they can be set before `aver_phys_init` and survive `aver_phys_shutdown`. Defaults are Jolt's, converted to centimetres, and a setting still at its default keeps Jolt's literal, so a world that never sets one steps bit for bit as before.

The limits a world is built with (`MAX_BODIES`, `MAX_BODY_PAIRS`, `MAX_CONTACTS`, `TEMP_MB`, `THREADS`) are PENDING config: `aver_phys_config_set` stores them, the next `aver_phys_init` reads them, and `aver_phys_rebuild` is shutdown plus init, refused unless the world is empty (`aver_phys_world_empty`). `THREADS 0` builds Jolt's single-threaded job system; the seam for a future engine scheduler is `makeJobSystem`. The temp allocator is wrapped so `aver_phys_stats` can report its peak, and the `EPhysicsUpdateError` that `Update` returns is kept (stats slot `UPDATE_ERRORS`) and logged once per world with the key to raise.

The project keys, the `phys.*` console variables, the C# `PhysicsSetting` enum and the editor page all derive from the same table; `modules/formats/src/OcProject.cpp` mirrors the name list because Formats may not depend on Physics, and `tests/game/ProjectPhysicsApplyTest` fails if the two disagree. See `docs/plans/wave11/WORLD_STATE_DEBUG.md`.

## Profiler: `aver_phys_profile_*` (same header)

With the profiler hook built (`AVER_PHYSICS_PROFILER`, `aver_phys_build_flags() & AVER_PHYS_BUILD_PROFILER`) every Jolt scope and the engine's fixed-step stages (`FixedStep` > `Characters`, `Buoyancy`, `SoftBodyBegin`, `VehicleStepBegin`, `PhysicsStep` > Jolt's `Update`, `VehicleStepEnd`, `JointBreakChecks`) are one tree. `aver_phys_profile_enable(1)` installs the collector; each thread keeps its own tree and the main thread merges them by scope NAME (string literals from different translation units have different addresses) at the end of every `aver_phys_step`, publishing every 60 fixed steps (the first table after 5) as inclusive, exclusive and window-maximum milliseconds per fixed step, depth first. Jolt's job scopes run on the worker threads and appear under one `Worker jobs` row whose times are CPU time summed over the workers. The ticks are the same `cpuReadTicks` clock the scene-walk instrument uses. A step call longer than `AVER_HITCH_MS` logs the three largest scopes of the last window while the profiler is on. The overhead with the hook idle is a headed measurement, not a ctest.

## Debug renderer: `physics_debug_abi.h`

Jolt's debug drawing as a flat line feed. `aver_phys_debug_collect(flags, camera, radius, maxLines)` runs the draw calls the flags ask for into a sink and returns the lines, 12 floats each: `(x y z r g b)` twice in engine centimetres and axes, which is two `rhi::LineVertex`, so a host uploads them as they are. Bodies are culled by the radius and drawn as wireframe feature edges (a shape's tessellation diagonals are dropped); meshes and heightfields are never drawn whole but as the triangles inside the radius (`STATIC_TRIANGLES`), through `Shape::GetTrianglesStart`. Contacts, supporting faces and continuous-collision markers are drawn by Jolt inside `Update`, so `aver_phys_debug_arm` switches that capture on beforehand; each fixed step clears it, which makes the buffer the LAST step's contacts. The packaged game has no line-overlay path and none is added; the editor's `Show > Physics` and C# `PhysicsDebugDraw` are the consumers. A build without the renderer answers `aver_phys_debug_available() == 0`.

## State recording: `physics_state_abi.h`

`aver_phys_snapshot_capture` writes the whole simulation into a byte blob and `aver_phys_snapshot_restore` puts it back: Jolt's bodies (a soft body's vertices included), contact cache and constraints (joints, ragdolls, vehicle wheels, engine and gearbox), every `CharacterVirtual` (which Jolt's own save leaves out), and the engine's own derived state (the fixed-step accumulator, a vehicle's driver-aid state including the last step's wheel telemetry, which the next step's ABS and traction control read, a character's ground carry). Inputs are not state: pedals set this frame, forces, kinematic targets, tune values and wind are re-issued by whatever resimulates. Statics are left out unless `AVER_PHYS_SNAP_STATICS` asks for them.

Restore is all-or-nothing. The payload starts with a manifest (the Jolt id of every saved body, the characters, vehicles, joints and ragdolls, the constraint count) and a restore checks it against the live world before Jolt is asked to do anything, because Jolt's non-validating restore stops halfway at a missing body and restores constraints by array index. A body, character, vehicle or joint that is gone, or ANY constraint added or removed since (the `constraintEpoch` that `noteConstraintTopology()` bumps in the joint, vehicle and ragdoll files), answers `TOPOLOGY` and changes nothing; bodies added since are left alone unless `AVER_PHYS_RESTORE_STRICT`. The blob header carries the state layout version, `sizeof(JPH::Real)`, the Jolt version and the cross-platform flag, so a snapshot from another build is `BUILD_MISMATCH`, and a checksum of the payload makes a damaged import `CORRUPT` before it reaches Jolt. A snapshot is not a save file: handles change when a level reloads.

`aver_phys_state_hash` is FNV-1a 64 over the payload (not the header), so two runs, two machines and two thread counts compare by it; `aver_phys_hash_log` writes one `step hash` line per fixed step; `aver_phys_snapshot_diff` names the first byte at which two snapshots differ. `aver_phys_step_exact(n)` runs exactly `n` fixed steps without touching the accumulator, which makes it the replay primitive. It is one `aver_phys_step` call for any `n` up to 600 (the eight-step clamp bounds only what a call adds to the accumulator), so the event queues are cleared once and describe all `n` steps. Other code can add its own state to a snapshot with `registerStateParticipant` (`PhysicsInternal.hpp`); nothing does yet. Known gaps: the ragdoll pose targets and mode, the wind field and its step index, the soft-body impact pair slots and buoyancy overrides are configuration or inputs and are not captured.


## Shape binary cache: `physics_state_abi.h` (same header)

A mesh or heightfield shape is mostly its BVH, which costs far more to build than to read back. `aver_phys_shape_save(shape, key, out, cap)` writes a shape handle (the table `aver_phys_create_mesh_shape` fills) as a blob and `aver_phys_shape_restore(data, size, key)` rebuilds it; `aver_phys_create_heightfield_shape` is `aver_phys_add_heightfield`'s construction without the body. The blob is a 48-byte header (`APSH`, the cache version, `sizeof(JPH::Real)`, the Jolt version, the caller's key, the kind, the payload size and its FNV-1a 64) around Jolt's `Shape::SaveBinaryState`. Every field is checked before Jolt reads a byte, because a restored `MeshShape` takes its tree as raw bytes and does not validate them: a wrong key, version, build or precision, a short blob or one damaged byte is a MISS (handle 0, no log line), never an error. A shape that carries physical-surface materials or per-triangle user data is not saved, because neither survives a file; a restored heightfield has its user data cleared, so it is a plain collision shape and `aver_phys_heightfield_set_heights` refuses it.

The disk half is `modules/world` `PhysicsShapeCache.hpp`: one AVR1 `PSHP` file per mesh under `<project>/Saved/DerivedDataCache/Physics`, keyed by the mesh path, size, mtime, the collision cache version, the shape cache version and the triangle counts. `createMeshShapeCached` is `aver_phys_create_mesh_shape` with the cache in front, and `createMeshShapeFromSource` is the call the level loaders make: it builds the key from the loader's source key (`GameContent::CollisionMesh::shapeSource`: mesh path, size, mtime, collision cache version and what each material slot said), the shape cache version and the triangle counts, and skips the cache when the source key is 0 or the directory is empty. `world::InstantiateOptions` carries `shapeCacheDir` and `shapeKeyFor`, the Runtime prefetch worker passes the same key, and the editor's own level load is not wired (it builds every launch, as before). **It is off** (`aver_phys_shape_cache_enabled()`, set by `PHYSICS.SHAPECACHE`, the `phys.shapecache` command or `AVER_PHYS_SHAPE_CACHE=1`): the design gates it on `PhysicsBenchTool shapes`, which times build, save and restore on the real levels and says GO only when restore is under a fifth of build and the blob under three times the triangles. That measurement has not been taken. The landscape heightfield is not routed through the cache for the same reason.

## Determinism tools (phase F)

Always available, in every build: `aver_phys_state_hash`, `aver_phys_hash_log`, `aver_phys_step_exact` and `phys.build` (precision, Jolt's configuration string, the optional features compiled in). `tests/physics` `PhysicsDeterminismTest` replays a scene twice, compares worker counts 0, 1 and 4 at every step, and hashes a fixed 600-step scene at six checkpoints (the golden hash).

Two CMake options in `modules/physics.jolt/CMakeLists.txt`, both OFF: `AVER_PHYSICS_CROSS_PLATFORM` (Jolt's `CROSS_PLATFORM_DETERMINISTIC`, plus `-ffp-contract=off` outside MSVC) and `AVER_PHYSICS_DIAGNOSTICS` (Jolt's determinism log, the three stats-tracking defines and its internal profiler, which replaces the external hook; `phys.report broadphase|simulation|narrowphase|profile` prints them; `aver_phys_diag_log_dir` says where `detlog.txt` goes). **No cross-platform claim is made.** The CI job `physics-determinism` builds with the first option on Windows and macOS, runs the golden scene and diffs the two checkpoint files; the claim is made only when they are equal and the recorded checkpoints in the test are filled in. The engine's own float code in the fixed step (the vehicle driver aids) is part of what that test finds out.
