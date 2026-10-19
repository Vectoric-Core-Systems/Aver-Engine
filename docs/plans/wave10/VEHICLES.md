# Vehicles: Chaos-Vehicles parity for cars, trucks and tanks (wave 10)

Design contract for the implementing agent. Owner approved the Unreal feature port 2026-10-18 (wave 10). Nothing here is built or run. Rules in force: upgrade the existing vehicle ABI, component pattern and node family (no parallel system), no new third-party code (Jolt is already vendored and its tracked-vehicle controller is already compiled), lean comments, never delete files, D3D12 and Vulkan parity (this feature adds no GPU work: vehicles are ordinary mesh entities and the one overlay is `rhi::LineVertex` lines both backends draw), headless ctest where possible, headed manual runs by the owner, never rebuild over the owner's running editor. Format and depth follow `docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md` and `docs/plans/wave9/SOFTBODY_CLOTH_DESTRUCTION.md`.

Every "exists today" claim below was read in the tree on 2026-10-19. Line numbers are last-known-good pointers; grep the symbol when one is off. **UNVERIFIED** marks anything reasoned from reading but not confirmed by running it; every tuning number in this document is an UNVERIFIED starting value until the headed pass in section 6.

**Scope.** Cars and trucks with any wheel and axle count and FWD, RWD or AWD, plus tracked vehicles (tanks) through Jolt's `TrackedVehicleController`. No motorcycles (Jolt's `MotorcycleController` stays unwrapped). No aircraft, boats, trailers or articulated bodies.

---

## 0. Decisions in one screen

1. **Build out `modules/vehicle`, do not recycle it.** Today it is a README and a `module.json` with `"skeleton": true` (`modules/vehicle/module.json`), not in the root `CMakeLists.txt`. It becomes two modules in the `softbody` / `softbody.scene` split: `Aver.Vehicle` (pure: the `.ocvehicle` asset, the spec, built-in presets, the builder that drives the physics C ABI; Core + Formats + Physics) and `Aver.Vehicle.Scene` (`modules/vehicle.scene`: the `CVehicle` component and `VehiclePhysicsSystem`; adds Scene). `modules/netvehicle` stays a skeleton (it already depends on `Aver.Vehicle`).
2. **Physics ABI v2 is a new header, `physics_vehicle_ext_abi.h`, with its own `AVER_PHYS_VEHICLE_ABI_VERSION 2`.** The 14 v1 functions are untouched. A vehicle that never calls a v2 function behaves as it does today (Jolt's own engine curve, gearbox, tyre curves, open differentials, raw pedals). All v2 data lives in `VehicleEntry` and is consumed by `aver_phys_vehicle_finish`.
3. **Both Jolt controllers, no Jolt edits.** Wheeled gains the torque curve, inertia, gear ratios, reverse and final drive, auto or manual, clutch and shift time, differential mode, split and limited slip per axle, centre differential, anti-roll stiffness per axle, per-wheel inertia, damping and friction curves. Tracked (`TrackedVehicleController`, `WheelTV`) is wrapped by the same builder with `set_kind` and `set_track`.
4. **Driver aids run in the fixed step, not in the frame.** The physics step loop gets one hook, `vehicleStepBegin(dt)`, right beside `softbody::onStepBegin` (`PhysicsWorld.cpp:485`). It turns the raw pedals into what Jolt sees: steering rate and speed-sensitive steering, brake-to-reverse, hold at standstill, ABS (per-wheel brake torque scaled from last step's slip), traction control (throttle cut), aerodynamic drag and downforce. Frame-rate independent, and off for v1 vehicles.
5. **`.ocvehicle`** is a new OC-text asset (header `OCVEHICLE 1`, one record per line): body, engine, torque curve, gearbox, axles and differentials, wheel classes and tyre curves, wheels (with visual node or bone names), tracks, driver aids, aero, audio, effects, damage, seats, camera, HUD. Five built-in presets (`car`, `van`, `truck`, `bus`, `sports`) keep the traffic system's numbers byte for byte; `tank` is new.
6. **`CVehicle`** is a runtime-registered component (the `CMotionMatcher` / `CCloth` pattern: no `Components.hpp` growth, no scene-ABI change), 96 bytes, zero-filled storage meaning the useful state. `VehiclePhysicsSystem` owns the Jolt vehicles that belong to entities, builds them lazily, and **writes the entity pose every frame in C++**. The 60 Hz C# timer that copies poses today (`Vehicles.cs:288-320`) is retired for cars made through the new path.
7. **Wheel visuals** are child entities named in the spec (default, any wheel count) or skeletal bones driven through `ControlRigSystem::setInput` (the existing `SetBone` op reading named inputs). The rig-input cap of 8 names per entity (`ControlRig.cpp:301`) is raised to 32, and the one test that pins it is updated.
8. **Telemetry is first-class**: RPM, gear, clutch, applied pedals, assist flags, and per wheel slip, load, compression, angular velocity, contact point, surface friction and surface entity. It reaches C++ (`aver_phys_vehicle_state` / `_wheel_state`), `CVehicle` OUT fields, C# structs, Aver Node pure nodes, the HUD and the debug overlay from one source.
9. **Possession and input.** A vehicle gets a driver (`Enter` / `Exit`). While it has one, the system reads the named input actions `Drive.Throttle`, `Drive.Steer`, `Drive.Brake`, `Drive.Handbrake`, `Drive.ShiftUp`, `Drive.ShiftDown`, `Drive.Reset`, `Drive.Exit`, `Drive.CameraNext` through the existing named-action ABI. The template ships them in its single `.ocinput`; C# projects can push the public `DrivingContext` instead. Camera work is **not** written here: entering creates child rig entities through GAMEPLAY_CAMERAS' `AddCameraRig` and switches the view with `CameraRig.SetViewTarget`; a camera key does the same to the next rig.
10. **Sound, effects and crash damage leave through sinks** the host wires: engine audio by RPM and load, skid, impact and shift sounds through the existing C audio ABI; skid smoke and sparks through GPU_PARTICLES' `aver::spawnEffectAt` seam in `Aver.Core`; crash damage through SOFTBODY_DEPTH's field system. `Aver.Vehicle.Scene` links none of those modules (section 2.8).
11. **The traffic `world::VehicleSystem` reads `.ocvehicle`.** Its hard-coded `kPresets` table (`VehicleSystem.cpp:312-318`) moves into `Aver.Vehicle` and `GameLevel::beginVehicles` accepts `vehicle <path>.ocvehicle` as well as the five names. AI driving is unchanged: it still only sets pedals.
12. **C# and Aver Node.** `Entity` vehicle members, `Vehicles.Spawn`, `VehicleTelemetry`, `WheelTelemetry`, `DrivingContext`, `VehicleHud`. `Vehicles.CreateCar(pos, yaw, CarSpec)` and the 6 existing rows keep working; `CarSpec` is serialised to `.ocvehicle` text so there is **one** parser and one builder. Family `Vehicle` v2: the 6 rows plus 14 (20 total), written to the wave 9 C contract.
13. **Editor:** Add Component `Vehicle`, a Details section with live telemetry, an `.ocvehicle` editor tab (torque curve, gear table, wheel placement over the entity), `Show > Vehicles` overlay (suspension rays, forces, slip, gear and RPM label), a Content Browser entry.
14. **Template and lab content:** `templates/Vehicle` (a sedan, a tank, a drivable on-foot pawn that enters them, a speed / gear / RPM HUD, no C#), and `test-content/VehicleLab` (6x4 truck, ice and ramp strips, a crash wall).
15. **Not in v1, said plainly:** motorcycles, trailers and articulated bodies, aircraft and boats, stability control, Ackermann geometry (every steered wheel turns by the same angle), camber and toe, tyre temperature and wear, a clutch pedal axis (the manual box shifts through a timed clutch), chassis deformation beyond the crash-damage hook, vehicle networking (`modules/netvehicle` stays a skeleton), replay capture, and a hard-locked differential (Jolt has none; section 2.2).
16. **Size XL, seven phases** (section 4). A and B carry the risk (the physics ABI); D (the system) is the largest.

---

## 1. What exists

### 1.1 Physics: the v1 vehicle ABI

| Fact | Where |
|---|---|
| 14 exports, builder style: `create`, `add_wheel` per wheel, `set_engine`, `finish`, `destroy`, `body`, `set_input`, `pose`, `velocity`, `forward_speed`, `wheel_count`, `wheel_pose`, `wheel_contact`, `set_pose`. No version define, no function that reports one | `physics_vehicle_abi.h:78,112,127,143,146,149,162,169,171,173,176,183,186,194` |
| Units and axes are the engine's; the vehicle origin is the bottom centre of the car and `pose` reports the origin, not the centre of mass | `physics_vehicle_abi.h:23-27`, `PhysicsVehicle.cpp:395-405` |
| The handle comes from the one body counter; the chassis is an ordinary body; `vehicleOfBody` maps back so removing the chassis destroys the vehicle first | `PhysicsVehicle.cpp:198-203`, `PhysicsInternal.hpp:202-205`, `PhysicsWorld.cpp:535` |
| The chassis is one box lifted by ground clearance with the centre of mass moved (`RotatedTranslatedShape` inside `OffsetCenterOfMassShape`) | `PhysicsVehicle.cpp:181-196` |
| Engine = three numbers (`maxTorque`, `minRpm`, `maxRpm`); inertia and clutch strength scale with torque against a 500 N*m reference; the automatic box shifts at 0.6 and 0.2 of the rev range; gear ratios are Jolt's defaults (`2.66 1.78 1.3 1.0 0.74`, reverse `-2.90`, switch 0.5 s, clutch release 0.3 s, latency 0.5 s) | `PhysicsVehicle.cpp:79-83,240-251,301-310`; Jolt `VehicleTransmission.h:34-41` |
| The torque curve is Jolt's default `(0, 0.8) (0.66, 1.0) (1.0, 0.8)`, and its X axis is `rpm / maxRpm` (not `(rpm - min) / range`) | Jolt `VehicleEngine.cpp:25-29`, `VehicleEngine.h:63` |
| Axles are found by geometry (same X within 5 cm, opposite sides of Y = 0); each axle with a driven wheel gets a differential with an equal torque share and Jolt's ratio 3.42, split 0.5, slip 1.4; each complete axle gets an anti-roll bar at Jolt's default 1000 N/m | `PhysicsVehicle.cpp:62-65,117-148,312-329`; Jolt `VehicleDifferential.h:31-36`, `VehicleAntiRollBar.h:26-28` |
| Tyre friction curves are Jolt's defaults and are never set: longitudinal `(0,0) (0.06,1.2) (0.2,1.0)`, lateral `(0,0) (3,1.2) (20,1.0)` degrees; wheel inertia 0.9 and damping 0.2 | Jolt `WheeledVehicleController.cpp:44-55`, `.h:36-44` |
| Surface grip is measured against Jolt's default 0.2 through `SetCombineFriction` | `PhysicsVehicle.cpp:70-73,349-354` |
| The controller is cast to `WheeledVehicleController` everywhere; `TrackedVehicleController` is compiled and registered and **no engine code wraps it** | `PhysicsVehicle.cpp:113-115`; `modules/physics.jolt/Jolt/Jolt.cmake:412`, `Jolt/RegisterTypes.cpp:49,169`; grep of `modules/physics/src` finds no reference |
| Input is four floats, held between calls; a non-zero throttle wakes a sleeping vehicle; steering and brakes are applied by Jolt as given (no rate limit, no speed sensitivity) | `PhysicsVehicle.cpp:378-391`; Jolt `WheeledVehicleController.cpp:262` |
| Brake torque is read from each wheel's own settings every step (`mBrakeInput * mMaxBrakeTorque + mHandBrakeInput * mMaxHandBrakeTorque`), and the wheel settings are objects `finish` creates and can keep | `WheeledVehicleController.cpp:537,629`; `PhysicsVehicle.cpp:281-295` |
| What Jolt knows but the ABI hides: engine RPM and torque, current gear, clutch friction, wheel angular velocity, suspension length and impulse, contact body, point, normal, per-wheel slip | Jolt `VehicleEngine.h:53`, `VehicleTransmission.h:80-81`, `Wheel.h:58,76-100,106-112`, `WheeledVehicleController.h:61-62` |
| `wheel_pose` returns the steer-and-spin rotation for a wheel mesh whose axle is engine Y and forward is engine X, origin-relative | `PhysicsVehicle.cpp:431-449` |
| The physics step has a per-fixed-step slot before `system.Update` where buoyancy and (wave 9) soft-body aero already run; vehicles are torn down before the world and before their chassis | `PhysicsWorld.cpp:468-486,324-327` |
| Contact events are polled and cleared each `aver_phys_step`; they report bodies, point and normal, not closing speed | `physics_abi.h:425-440` |
| There is no physical-surface table; a surface is a body's friction (`aver_phys_body_set_friction`) | `physics_abi.h:216-220` |
| Tests: `VehicleTest` (533 lines): settles on four wheels, drives +X, steering sign, brakes, surface friction, teleport, builder refusals, dead handles, teardown | `tests/physics/src/VehicleTest.cpp:125-493`, `tests/physics/CMakeLists.txt:48-55` |
| ABI parity test pairs every physics header with `Native.cs` by name; the vehicle header is in its list | `tests/abi/src/AbiEnumTest.cpp:560-573` |

### 1.2 C# and Aver Node

| Fact | Where |
|---|---|
| `Aver.Physics.Vehicle` is a 152-line `readonly struct` (not `partial`) mirroring the 14 functions; no telemetry, no gearbox, no tracked | `scripting/csharp/Aver.Physics/Vehicle.cs`, `Native.cs:215-228` |
| `CarSpec` (record struct, 18 fields, frozen `Default`), `WheelPlan`, `Car`, and `Vehicles` (`SpringSagCm`, `PlanWheels` four wheels, `AxleOf`, `CountAxles`, `CountDifferentials`, `CreateCar`, `TryGet`, `SyncEntities`) | `Vehicles.cs:20-79,98,110,182-320` |
| Entity pose follows the vehicle through a repeating 60 Hz `Timers.Set` (`IgnoreDilation`), because a body stamped with `SetEntity` does not move its entity | `Vehicles.cs:288-320` |
| Six graph rows over `VehicleGraph`: `an_vehiclecreatecar`, `setinput`, `getstate`, `setpose`, `destroy`, `wheelcontact`; vehicles cross the graph as the vehicle handle | `GraphVehicleNodeTable.cs:14-21`, `VehicleGraph.cs`, `sandbox/src/GraphNodeDefsVehicle.hpp:10-27`, `docs/nodes/Vehicle.md` |
| `VehicleNodeTests` is `[AutoRun]`, bare-process scope (row wiring and the default car's plan, not driving) | `scripting/csharp/Aver.Graph.Tests/VehicleNodeTests.cs:14-27` |

### 1.3 AI traffic

| Fact | Where |
|---|---|
| `world::VehicleSystem` builds one Jolt vehicle per `PLACE ... vehicle <preset>` from the mesh bounds: five hard-coded presets (`car van truck bus sports`: mass, wheel radius range, engine torque, steer, spring Hz, front or rear drive) | `modules/world/src/VehicleSystem.cpp:302-327,404-453,759` |
| It calls `set_engine` with hard-coded 1000 and 6000 rpm and sets pedals only; poses are written by C++ after the step | `VehicleSystem.cpp:446,725,852-890` |
| Hosts call `prePhysics` before and `postPhysics` after the tick groups, inside the PLAYING gate | `Runtime/src/GameApp.cpp:579-584`, `sandbox/src/SandboxUpdate.cpp:766-778` |
| The level token is `vehicle <preset>` (percent-encoded string, unknown preset reads back unchanged); `beginVehicles` builds an unknown preset as `car`, skips scaled, animated or boundless placements | `OcWorld.cpp:834-837,1350`, `OcWorld.hpp:98-108`, `Runtime/src/GameLevel.cpp:651-700`, `docs/formats/FORMAT_SPECS.md:908-933` |
| A save made mid-Play must not write a driven car's live pose; the editor keeps the set of moving entities from `vehicles_.entities()` | `sandbox/src/SandboxLevelEdit.cpp:620-640` |
| Entities moved by a system other than an object clip are declared to `PlayMobility::seedMovable` so their first move does not cost a GI rebuild | `Runtime/include/aver/game/PlayMobility.hpp:45-60`, `sandbox/src/SandboxPlay.cpp:157`, `GameApp.cpp:2026` |

### 1.4 The two skeleton modules

`modules/vehicle` (`Aver.Vehicle`, "Planned: authoritative vehicle simulation", `dependsOn Aver.Core, Aver.Physics`, `default false`, `option null`) and `modules/netvehicle` (`Aver.NetVehicle`, depends on `Aver.Net`, `Aver.Vehicle`, `Aver.SoftBody`) are READMEs and manifests only; the root `CMakeLists.txt` adds neither (`modules/vehicle/module.json`, `modules/netvehicle/module.json`; `CMakeLists.txt:214,234-236,303-312` show where `softbody`, `physics` and `softbody.scene` are added). Decision 1 turns the first into real modules.

### 1.5 Systems this feature reuses

| System | State | Where |
|---|---|---|
| Runtime-registered components, zero-filled storage, `attach*` writes defaults | pattern in use: `CCloth` (40 bytes), `CDestructible`, `CMotionMatcher`, `CMorphTargets` | `modules/softbody.scene/include/aver/softbody/SoftBodySystem.hpp:50-72`, `modules/anim.scene/include/aver/anim/MotionMatchSystem.hpp:34-50` |
| Host wiring in one shared header: register, install, provider, tick, reset | `registerGameSystems` / `tickSoftBodies` / `resetGameSystems`, called from both hosts | `Runtime/include/aver/game/GameSystemsWiring.hpp:184-196,248,263`, `GameApp.cpp:1929`, `sandbox/src/SandboxInit.cpp:330`, `SandboxPlay.cpp:666` |
| Provider-relay ABI, own version, one provider function, 0 with no provider | `framework_softbody_abi.h:22-23,432-446`, `framework_anim_abi.h` | |
| Level records: type list, apply, capture, canonical form omits defaults | `kPlacedComponentTypes` has `Cloth Destructible MorphTargets`; `applyCloth`; FORMAT_SPECS rows | `Runtime/include/aver/game/LevelComponents.hpp:28`, `LevelComponents.cpp:31,189-192,397-420`, `OcWorld.cpp:173-174`, `docs/formats/FORMAT_SPECS.md:977-978` |
| Add Component rows, `canAttach`, `makeNewRecord`; the legacy `.ocmap` cannot store a COMPONENT record | `sandbox/src/PlacedComponentModel.hpp:41-76,115,199` | |
| Details sections live in a model header plus a widget half | `SoftBodyDetailsModel.hpp`, `SandboxComponentDetails.cpp:362-479` | |
| Control rig: `SetBone` sets a bone's position and rotation from named inputs (Float, Vec3, Quat; model or world space); `setInput` allows 8 distinct names per entity and a test pins "the ninth is refused" | `modules/formats/include/aver/formats/OcRig.hpp:46-75`, `ControlRig.hpp:101-104`, `ControlRig.cpp:293-309`, `tests/anim.scene/src/IkRigSystemTest.cpp:443-452` | |
| Named actions from C++: `aver_fw_action_find`, `_value2`, `_held`, `_pressed`; `.ocinput` ACTION / BIND grammar; a project has **one** scheme context; `EnhancedInput.AddContext(ctx, priority)` is the C# stack | `framework_abi.h:450-478`, `modules/formats/include/aver/formats/OcInput.hpp:60-100`, `InputScheme.cs:32`, `EnhancedInput.cs:340,530` | |
| Possession: `aver_fw_possess`, `_unpossess`, `_controlled_pawn`, `_controller_of`; the game camera follows the possessed pawn | `framework_abi.h:180-186`; `templates/FirstPerson/Content/README.md` ("Who spawns the player") | |
| Audio from C++: `aver_audio_play(sound, volume, pitch, looping, bus)`, `_set_voice_volume`, `_set_voice_pitch`; no audio-source component exists (`audio.scene` has `CReverbZone` and `CAudioOcclusion` only) | `modules/audio.abi/include/aver/audio/audio_abi.h:66,82,84`, `modules/audio.scene/include/aver/audio/AudioScene.hpp:15,29` | |
| Game UI: `.ocui` layout with `text` and `progress` widgets, `UiExt` property nodes (wave 9) | `docs/GAME_UI.md` (Widget kinds), `docs/nodes/UiExt.md` | |
| Editor-only line overlays: `collect` into `rhi::LineVertex`, drawn under `!noEditorChrome_`; a `Show` submenu per overlay | `sandbox/src/SoftBodyDebugOverlay.hpp`, `SandboxRender.cpp:1995-2021`, `SandboxShell.cpp:2180-2186` | |
| New Project templates: `<id>/<id>.octemplate` (`NAME`, `DESCRIPTION`, `PREVIEW`, `STARTMAP`, `GAMEMODE`) plus a copied `Content/`; tracked as `templates/` | `sandbox/src/ProjectScaffold.cpp:66-118,918-956`, `tests/editor/src/TemplateScaffoldTest.cpp:210-225` | |
| Test-content precedent: a generator script, checked-in OBJ sources, imported by `AverAssetC` | `test-content/SoftBodyLab/{make_soft_lab.py,README.md}` | |
| Nested placements (`BEGIN` / `CHILD` / `END`) with parent-relative transforms | `OcWorld.hpp:115-129`, `OcWorld.cpp:775-779` | |

**Sibling wave 10 features (written in parallel, contracts fixed by the orchestrator).** `docs/plans/wave10/GAMEPLAY_CAMERAS.md` section 2.12 and `GPU_PARTICLES.md` section 2.10 were read for the consumed signatures (2.8). `SOFTBODY_DEPTH.md` did not exist yet when this was written, so its field-system call is referenced by name only and its shape in 2.8 is an assumption; if its final signature differs, only the host wiring in `GameSystemsWiring.hpp` changes, never `Aver.Vehicle.Scene`.

---

## 2. Design

### 2.1 Architecture and the Chaos mapping

```
 .ocvehicle / builtin preset      Aver.Vehicle (pure: Core + Formats + the physics C ABI)
 CarSpec text (C#)            ->  VehicleSpec, parse/write/validate, specFromBounds, buildVehicle(spec, pose)
                                        |  aver_phys_vehicle_* (v1 builder + v2 ext)
 Aver.Physics (DLL)   physics_vehicle_ext_abi.h
   wheeled + tracked controller, torque curve, gearbox, diffs, tyre curves,
   fixed-step driver aids (vehicleStepBegin), aero, telemetry, polled events
                                        |  state / events
 Aver.Vehicle.Scene   VehiclePhysicsSystem + CVehicle     <- named actions Drive.* , graph / C# / relay ops
   prePhysics: build new vehicles, apply driver or script input, tune, flags
   postPhysics: pose -> entity, wheel visuals, telemetry, events, audio, effects, crash sinks
        |-- sinks (host wires): audio port, effect sink, crash sink, camera sink, notify sink
```

| Unreal (Chaos Vehicles) | Here |
|---|---|
| `ChaosWheeledVehicleMovementComponent` | `CVehicle` + `VehiclePhysicsSystem` (wheeled and tracked) |
| Vehicle template (Advanced Vehicle) | `templates/Vehicle` (sedan, tank, HUD, driving input), `test-content/VehicleLab` |
| Engine setup: torque curve, max RPM, idle, MOI, damping | `ENGINE`, `CURVE` records; `aver_phys_vehicle_set_engine_ex` / `_set_torque_curve` |
| Transmission: auto or manual, gear ratios, final ratio, change up and down RPM, change time, clutch | `GEARBOX` record; `_set_gearbox`, `_set_gearbox_mode`, `_set_gear`, `_shift` |
| Differential: FWD, RWD, AWD, front-rear split, limited slip | `AXLE`, `CENTER` records; `_set_axle`, `_set_center_diff` (which wheels are driven is the drive layout) |
| Steering: speed-sensitive steering curve, steering rate | `STEERCURVE`, `ASSIST steerin steerout` |
| Wheel class: radius, width, friction scale, brake, handbrake, steer angle, suspension, spring, damping, anti-roll | `WHEELCLASS` + `TYRE` + `WHEEL` records; `AXLE antiroll` |
| ABS, traction control | `ASSIST abs tcs`; per-wheel brake scaling, throttle cut |
| Aerodynamics (drag, downforce) | `AERO` record; `_set_aero` |
| Vehicle input (throttle, brake, steer, handbrake, gear up and down, reset) | `Drive.*` named actions, `Entity.SetVehicleInput`, `AN_VehicleDrive` |
| Wheel meshes and animation Blueprint wheel nodes | child entities or `SetBone` rig inputs |
| `p.Vehicle.*` debug draw, `ShowDebug Vehicle` | `Show > Vehicles` overlay and the Details telemetry table |
| Chaos has no tracked vehicle; the owner asked for tanks | Jolt `TrackedVehicleController` through the same builder (section 2.3) |

### 2.2 Physics ABI v2: `physics_vehicle_ext_abi.h`

New header (own file for the reason `physics_vehicle_abi.h` is one: the C# parity test pairs headers with the binding files), new sources `src/PhysicsVehicleExt.cpp` (v2 entry points, step hooks, telemetry, events) and `src/PhysicsVehicleTracked.cpp` (the tracked finish). `PhysicsVehicle.cpp` keeps v1 and its `finish` is extended to consume `VehicleEntry`'s new fields. Types are `int32_t`, `float`, `const float*`, `const int32_t*` only; units are the engine's (cm, kg, N*m, degrees for steering and slip angle, rad/s for wheel speed). Every function returns 1 or 0 (`aver_phys_last_error` as elsewhere) except where stated. **A parameter block may be shorter than its defined length (the rest keep their defaults) and is refused if longer.** Builder calls are refused after `_finish`; live calls are refused before it.

```c
#define AVER_PHYS_VEHICLE_ABI_VERSION 2
AVER_PHYS_API int32_t aver_phys_vehicle_abi_version(void);                         /* 2 */

/* ---- before _finish ---- */
#define AVER_PHYS_VEH_KIND_WHEELED 0
#define AVER_PHYS_VEH_KIND_TRACKED 1
AVER_PHYS_API int32_t aver_phys_vehicle_set_kind(int32_t v, int32_t kind);          /* before add_wheel */
AVER_PHYS_API int32_t aver_phys_vehicle_set_chassis_boxes(int32_t v, const float* boxes6, int32_t count);
                                                      /* 1..8 boxes, cx cy cz hx hy hz, origin-relative cm; replaces create()'s box, mass unchanged */
AVER_PHYS_API int32_t aver_phys_vehicle_set_engine_ex(int32_t v, const float* p, int32_t n);       /* AVER_PHYS_VE_* */
AVER_PHYS_API int32_t aver_phys_vehicle_set_torque_curve(int32_t v, const float* rpmFraction,
                                                         const float* torqueFraction, int32_t n);  /* 2..32 points */
AVER_PHYS_API int32_t aver_phys_vehicle_set_gearbox(int32_t v, int32_t manual, const float* fwd, int32_t nFwd,
                                                    const float* rev, int32_t nRev, const float* p, int32_t n); /* AVER_PHYS_VG_* */
AVER_PHYS_API int32_t aver_phys_vehicle_axle_count(int32_t v);                      /* axles by the v1 geometry rule, front (largest x) first */
AVER_PHYS_API int32_t aver_phys_vehicle_wheel_axle(int32_t v, int32_t wheel);       /* -1 for a bad index */
AVER_PHYS_API int32_t aver_phys_vehicle_set_axle(int32_t v, int32_t axle, const float* p, int32_t n);   /* AVER_PHYS_VA_* */
AVER_PHYS_API int32_t aver_phys_vehicle_set_center_diff(int32_t v, int32_t mode, float slipRatio);       /* between driven axles */
AVER_PHYS_API int32_t aver_phys_vehicle_set_wheel_ex(int32_t v, int32_t wheel, const float* p, int32_t n); /* AVER_PHYS_VW_* */
AVER_PHYS_API int32_t aver_phys_vehicle_set_wheel_curves(int32_t v, int32_t wheel /* -1 = all */,
                      const float* longSlip, const float* longMu, int32_t nLong,
                      const float* latDeg,   const float* latMu,  int32_t nLat);                       /* 2..16 points each */
AVER_PHYS_API int32_t aver_phys_vehicle_set_track(int32_t v, int32_t side /* 0 left, 1 right */,
                      const int32_t* wheels, int32_t n, int32_t drivenWheel, const float* p, int32_t np); /* AVER_PHYS_VT_* */

/* ---- before or after _finish (live) ---- */
AVER_PHYS_API int32_t aver_phys_vehicle_set_assists(int32_t v, const float* p, int32_t n);        /* AVER_PHYS_VD_* */
AVER_PHYS_API int32_t aver_phys_vehicle_set_aero(int32_t v, float dragNs2Pm2, float downforceNs2Pm2);
AVER_PHYS_API int32_t aver_phys_vehicle_set_collision_period(int32_t v, int32_t everyNSteps);     /* 1 = every step (default); far vehicles 2..4 */
AVER_PHYS_API int32_t aver_phys_vehicle_tune(int32_t v, int32_t key, float value);                /* AVER_PHYS_VU_* */

/* ---- after _finish ---- */
AVER_PHYS_API int32_t aver_phys_vehicle_set_gearbox_mode(int32_t v, int32_t manual);
AVER_PHYS_API int32_t aver_phys_vehicle_set_gear(int32_t v, int32_t gear);   /* manual only: -1 reverse, 0 neutral, 1..N; 0 result if out of range */
AVER_PHYS_API int32_t aver_phys_vehicle_shift(int32_t v, int32_t delta);     /* manual only: +1 or -1 from the current gear, clamped */
AVER_PHYS_API int32_t aver_phys_vehicle_flip(int32_t v, float liftCm);       /* upright about the current yaw, stop dead, set down liftCm higher */
AVER_PHYS_API int32_t aver_phys_vehicle_state(int32_t v, float* outF, int32_t nF, int32_t* outI, int32_t nI);        /* AVER_PHYS_VS_* / _VSI_* */
AVER_PHYS_API int32_t aver_phys_vehicle_wheel_state(int32_t v, int32_t wheel, float* outF, int32_t nF,
                                                    int32_t* outI, int32_t nI);                                     /* AVER_PHYS_VWS_* / _VWSI_* */
AVER_PHYS_API int32_t aver_phys_vehicle_event_count(void);                   /* polled, cleared at the top of aver_phys_step, like contacts */
AVER_PHYS_API int32_t aver_phys_vehicle_event_get(int32_t index, int32_t* outI4, float* outF8);
```

`_state` and `_wheel_state` write `min(n, defined)` entries into each array (either may be null) and return the number of floats written, 0 for a dead or unfinished vehicle.

**Parameter blocks** (`index name (unit) = default`). Defaults are Jolt's own where Jolt has one, so an untouched block reproduces v1.

| Block | Indices |
|---|---|
| `VE` engine | 0 `MAX_TORQUE` (N*m) 500; 1 `MIN_RPM` 1000 (idle); 2 `MAX_RPM` 6000; 3 `INERTIA` (kg m^2) 0.5 x torque scale as v1; 4 `ANGULAR_DAMPING` 0.2 (this is engine braking) |
| `VG` gearbox | 0 `SWITCH_TIME` 0.5; 1 `CLUTCH_RELEASE_TIME` 0.3; 2 `SWITCH_LATENCY` 0.5; 3 `SHIFT_UP_FRACTION` 0.6 and 4 `SHIFT_DOWN_FRACTION` 0.2 (of `[minRpm, maxRpm]`, as v1); 5 `CLUTCH_STRENGTH` 10 x torque scale as v1. Forward ratios positive and strictly decreasing is advised not required; reverse ratios are given as **positive magnitudes** and negated inside |
| `VA` axle | 0 `FINAL_DRIVE` 3.42 (Jolt `mDifferentialRatio`); 1 `DIFF_MODE` 0 open, 1 limited slip, 2 locked; 2 `LSD_RATIO` 1.4; 3 `LEFT_RIGHT_SPLIT` 0.5; 4 `TORQUE_SHARE` 0 (0 = equal among driven axles; shares are normalised at `_finish`); 5 `ANTIROLL_STIFFNESS` (N/m) 1000, 0 removes the bar |
| `VW` wheel | 0 `INERTIA` 0.9; 1 `ANGULAR_DAMPING` 0.2; 2 `TV_LONG_FRICTION` 4 and 3 `TV_LAT_FRICTION` 2 (tracked wheels only, Jolt `WheelSettingsTV`); 4 `BRAKE_BIAS` 1 (multiplies this wheel's brake torque) |
| `VT` track | 0 `INERTIA` 10; 1 `ANGULAR_DAMPING` 0.5; 2 `BRAKE_TORQUE` (N*m) 15000; 3 `FINAL_DRIVE` 6.0; 4 `STEER_GAIN` 1.6 (section 2.3); 5 `PIVOT_POWER` 0.6 |
| `VD` driver aids | 0 `ENABLED` 0 (**off means v1 pass-through**); 1 `ABS` 0; 2 `ABS_SLIP` 0.25; 3 `ABS_RELEASE` (per second) 8; 4 `TCS` 0; 5 `TCS_SLIP` 0.35; 6 `STEER_IN` (full locks per second, moving away from centre) 3; 7 `STEER_OUT` (moving back to centre) 5; 8 reserved; 9 `THROTTLE_IN` 6; 10 `THROTTLE_OUT` 8; 11 `BRAKE_IN` 8; 12 `REVERSE_ON_BRAKE` 1; 13 `REVERSE_SPEED` (cm/s) 50; 14 `HOLD_BRAKE` (0..1 at standstill with no pedal) 0.3; 15 `HOLD_SPEED` (cm/s) 30; 16 `SKID_SLIP` 0.45; 17 `SKID_ANGLE_DEG` 12 |
| `VU` live tune | 0 `ENGINE_TORQUE_SCALE` 1; 1 `BRAKE_SCALE` 1; 2 `GRIP_SCALE` 1 (multiplies both tyre curves through the combine-friction lambda, no curve rebuild); 3 `SUSPENSION_STIFFNESS_SCALE` 1 (multiplies every wheel's spring frequency); 4 `STEER_SCALE` 1; 5 `DRAG_SCALE` 1; 6 `DOWNFORCE_SCALE` 1; 7..14 `STEER_CURVE_0..7` (see next paragraph) |
| `VS` vehicle floats | 0 `RPM`; 1 `ENGINE_TORQUE_NM`; 2 `SPEED_CMS`; 3 `FORWARD_SPEED_CMS`; 4 `LATERAL_SPEED_CMS`; 5 `THROTTLE_APPLIED`; 6 `BRAKE_APPLIED`; 7 `STEER_APPLIED` (-1..1); 8 `HANDBRAKE_APPLIED`; 9 `CLUTCH`; 10 `WHEEL_RPM_AT_CLUTCH`; 11 `YAW_RATE_DEG_S`; 12 `LONG_ACCEL_G`; 13 `LAT_ACCEL_G`; 14 `UP_Z`; 15 `TRACK_LEFT_CMS`; 16 `TRACK_RIGHT_CMS`; 17 `AIRBORNE_SECONDS` |
| `VSI` vehicle ints | 0 `GEAR` (-1 reverse); 1 `GEAR_COUNT`; 2 `MANUAL`; 3 `SHIFTING`; 4 `WHEELS_ON_GROUND`; 5 `ABS_ACTIVE`; 6 `TCS_ACTIVE`; 7 `KIND`; 8 `AXLE_COUNT`; 9 `WHEEL_COUNT`; 10 `REV_LIMITED` |
| `VWS` wheel floats | 0 `SLIP_LONG` (signed ratio, positive = spinning faster than the ground, negative = locking); 1 `SLIP_LAT_DEG`; 2 `LOAD_N`; 3 `COMPRESSION_CM`; 4 `SUSP_LENGTH_CM`; 5 `ANG_VEL_RAD_S`; 6 `STEER_DEG`; 7 `SPIN_RAD`; 8..10 `CONTACT_POINT` (world cm); 11..13 `CONTACT_NORMAL`; 14 `FORCE_LONG_N`; 15 `FORCE_LAT_N`; 16 `SURFACE_FRICTION`; 17 `BRAKE_TORQUE_NM` (as applied, after ABS) |
| `VWSI` wheel ints | 0 `CONTACT`; 1 `CONTACT_BODY`; 2 `CONTACT_ENTITY` (what `aver_phys_set_entity` stamped, 0 if none); 3 `SKIDDING` |

The steering-speed curve is up to four `(speedCmS, scale)` points; `_tune` keys `STEER_CURVE_0..7` are the eight floats in order, and a point with speed 0 and scale 0 ends the list. Scale 1 below the first point's speed, linear between points, the last point's scale beyond it. Default curve: one point `(0, 1)`.

**Events** (`event_get`: `outI4` = kind, vehicle, `a`, `b`; `outF8` = point xyz, normal xyz, `value`, `value2`):

| Kind | `a`, `b` | `value`, `value2` |
|---|---|---|
| 1 `SKID_BEGIN` / 2 `SKID_END` | wheel index, 0 | peak slip (ratio or degrees), slide speed cm/s |
| 3 `LAND` | wheel index, 0 | downward speed cm/s, airborne seconds |
| 4 `IMPACT` | other body, 0 | closing speed cm/s along the normal, other mass kg (0 static) |
| 5 `SHIFT` | from gear, to gear | RPM at the shift, 0 |
| 6 `FLIPPED` | 0, 0 | up.z, seconds on its side |
| 7 `ASSIST` | 0 ABS / 1 TCS, 1 on / 0 off | wheel index, 0 |

A wheel is skidding when it has contact, the vehicle is faster than 150 cm/s, and `|SLIP_LONG|` exceeds 0.45 or `SLIP_LAT_DEG` exceeds 12 (UNVERIFIED thresholds; both are `VD` entries (16, 17) a spec sets with `ASSIST skidslip skidangle`). An impact is a chassis contact from the polled contact queue (`physics_abi.h:425-440`) whose closing speed, `max(0, -(v_chassis_before_step - v_other) . n)`, exceeds 100 cm/s, with `v_chassis_before_step` cached by `vehicleStepBegin` (the soft-body velocity-cache idiom). Contacts report only *new* contact pairs, so sustained scraping is not an event in v1. `LAND` fires when a wheel regains contact after at least 0.2 s airborne.

**What `finish` does with v2 data (the contract the tests pin).**
- The engine curve, inertia and damping, gearbox, per-axle differentials and anti-roll bars, per-wheel inertia, damping, friction curves and brake bias all come from the stored description; anything never set is Jolt's default, and for a vehicle that set nothing the Jolt objects are field-for-field what v1 builds today.
- **Limited slip.** `DIFF_MODE 1` writes `LSD_RATIO` into `mLimitedSlipRatio`; `0` writes `FLT_MAX` (open); **`2` writes 1.01 and the doc says so: Jolt has no hard lock, and a ratio of 1.01 sends all torque to the slower wheel at the first measurable speed difference**. The centre differential is the controller's `mDifferentialLimitedSlipRatio` with the same three modes (it only acts between differentials, so it needs at least two driven axles). **Its default is Jolt's own 1.4, not open**, so a v1 vehicle with two driven axles is unchanged; `open` writes `FLT_MAX`.
- **Final drive** is `FINAL_DRIVE` of the axle. Overall ratio at a wheel is gear x final drive, as in Jolt; the spec's per-axle value lets a truck's rear axles differ.
- The wheel settings objects `finish` builds are kept in `VehicleEntry` as non-const `Ref<WheelSettingsWV>` (or `WheelSettingsTV`) so the step hook and `_tune` can change `mMaxBrakeTorque` and `mSuspensionSpring` live. They are never shared between vehicles.
- `_set_chassis_boxes` builds a `StaticCompoundShape` of the boxes inside the same `OffsetCenterOfMassShape` v1 uses, swaps it onto the body with `SetShape(..., updateMassProperties)` and then restores the requested mass and centre of mass, so a truck can have a cab and a bed of different heights. Refused after `_finish`.
- `_flip` sets the rotation to yaw-only, zeroes both velocities and every wheel and engine speed (the `_set_pose` teardown of v1), moves up by `liftCm`, wakes the body.
- `_set_collision_period` calls `SetNumStepsBetweenCollisionTestActive` (`VehicleConstraint.h:174`): the wheel shape casts run every N steps. The system raises it for vehicles far from the camera (section 2.6.6).
- The step hooks are two functions in `PhysicsInternal.hpp` called from `PhysicsWorld.cpp`: `vehicleStepBegin(dt)` after `softbody::onStepBegin` (`:485`), and `vehicleStepEnd(dt)` after `system.Update` (reads Jolt's state, evaluates slip and events, caches velocities). Both walk `g_world->vehicles` in handle order (the determinism contract), and both are no-ops with no finished vehicle.

**Aids, in order, per fixed step** (only when `VD.ENABLED`; with it off `set_input` forwards to Jolt immediately exactly as v1 does):

1. Pedal intent: throttle `T`, steer `S`, brake `B`, handbrake `H` as set. If `REVERSE_ON_BRAKE`: `T < 0` while the forward speed exceeds `REVERSE_SPEED` becomes brake `max(B, -T)` with no throttle; `T > 0` while reversing faster than `REVERSE_SPEED` likewise; otherwise `T` stands (negative selects reverse in the automatic box). With no pedals and `|speed| < HOLD_SPEED`, brake rises to `HOLD_BRAKE` so an idling car does not creep.
2. Steering target = `S * curve(|speed|) * STEER_SCALE`; the applied value moves towards it at `STEER_IN` (away from centre) or `STEER_OUT` (towards it) full locks per second. Throttle and brake move at their `_IN` / `_OUT` rates.
3. Traction control: if any driven wheel's `SLIP_LONG` exceeds `TCS_SLIP` with throttle on, throttle is multiplied by a factor that falls at 4 per second to a floor of 0.2 and recovers at 2 per second; `ASSIST` event on change.
4. ABS: per wheel with contact and brake above 0.1, while `-SLIP_LONG > ABS_SLIP` that wheel's brake factor falls at `ABS_RELEASE` per second to 0 (Jolt keeps a locked wheel locked while any brake torque is on, so a floor above 0 never releases it), otherwise recovers at a quarter of that rate (at the same rate a wheel just unlocked is braked into the lock again); ABS stands down below 200 cm/s; `mMaxBrakeTorque = base * BRAKE_BIAS * tune.brake * factor`.
5. Aero: force at the centre of mass, `-drag * |v| * v` and `-downforce * |v|^2 * up` with `v` in m/s (`BodyInterface::AddForce`, applied before `Update`, so it is part of this step).
6. `SetDriverInput` on the controller with the resulting values; in a manual box the throttle is `max(0, T)` and reverse is a gear.

All of this is arithmetic on values Jolt produced on the previous step plus the pedals; none of it reads a clock. Brake and throttle rates are in units per second, so the result does not depend on the frame rate.

### 2.3 Tracked vehicles

`_set_kind(v, TRACKED)` before the first `_add_wheel` makes `finish` build `WheelSettingsTV` wheels (the add-wheel arguments mean the same; `maxSteerDeg`, the handbrake torque and the `driven` flag are ignored, a log line says so once) and a `TrackedVehicleController`. `_set_track` names the road wheels of each side and the driven (sprocket) wheel among them; `finish` is refused unless both sides have at least one wheel and the driven wheel belongs to its list. There are no differentials and no anti-roll bars (the axle calls are refused for a tracked vehicle). Engine and gearbox are the wheeled ones (`TrackedVehicleControllerSettings` carries both); the track's `FINAL_DRIVE` replaces the differential ratio.

Steering. Jolt's controller takes `(forward, leftRatio, rightRatio, brake)` and asserts both ratios are non-zero (`TrackedVehicleController.h`, `SetDriverInput`). With throttle `F`, steer `S` (+1 = right):

- Moving (`|F| > 0.05`): the inner track's ratio is `1 - |S| * STEER_GAIN`, never under the 0.05 floor (Jolt keeps the tracks in their ratio by exchanging speed between them, and a negative ratio makes that exchange stop a moving tank; counter-rotation is the pivot), the outer stays 1.
- Standing or nearly (`|F| <= 0.05` and `|S| > 0.05`): a **pivot**: forward demand `|S| * PIVOT_POWER`, left ratio `+sign(S)`, right ratio `-sign(S)`.
- Any ratio with magnitude under 0.05 is lifted to 0.05 with its sign. Brake is the brake pedal; the handbrake holds both tracks at the brake torque. The aids block applies except ABS and TCS, which are wheeled-only.

`VS` gains `TRACK_LEFT_CMS` / `TRACK_RIGHT_CMS` (driven wheel angular velocity x radius), which the system publishes as rig inputs `track_left` and `track_right` (floats) for a track-scroll material or rig, and as `CVehicle` telemetry. The turret is not part of the vehicle: the template's tank rotates a turret child entity from a graph.

### 2.4 `.ocvehicle` (OC text; UTF-8; header `OCVEHICLE 1`)

Records are one per line, `KEY` then `name value...` pairs, `#` comments, blank lines ignored. Unknown keys inside a record are ignored with one warning, an unknown record is ignored, a newer version is refused. Written by `writeOcVehicle` (round-trips byte-identically for canonical input). Spec text for `FORMAT_SPECS.md` is added by the integrator from this section. Lengths cm, mass kg, torque N*m, angles degrees.

```
OCVEHICLE 1
NAME Sedan
KIND wheeled                                  # wheeled | tracked
BODY half 220 90 35 clearance 25 com 0 0 40 mass 1300 pitchroll 60
BOX 0 0 35 220 90 35                          # optional, up to 8: cx cy cz hx hy hz; if present they replace BODY's box
ENGINE torque 500 minrpm 1000 maxrpm 6000 inertia 0.5 damping 0.2
CURVE 0 0.80  0.66 1.00  1.00 0.80            # rpm/maxrpm, fraction of peak torque; 2..32 points, x ascending in [0,1]
GEARBOX auto forward 2.66 1.78 1.30 1.00 0.74 reverse 2.90 switch 0.5 clutchrelease 0.3 latency 0.5 up 0.6 down 0.2 clutch 10
CENTER open                                   # open | lsd <ratio> | locked, between driven axles (AWD)
AXLE 0 final 3.42 diff open split 0.5 lsd 1.4 share 0 antiroll 1000     # axle 0 = front (largest x)
AXLE 1 final 3.42 diff lsd lsd 1.4 antiroll 800
TYRE street long 0 0  0.06 1.2  0.2 1.0  lat 0 0  3 1.2  20 1.0
WHEELCLASS street radius 33 width 22 susp 5 30 hz 2.0 damping 0.5 brake 1500 handbrake 2500 inertia 0.9 spindamping 0.2 tyre street
WHEEL FL class street hub 180 -77 33 steer 35 driven 0 node WheelFL
WHEEL FR class street hub 180  77 33 steer 35 driven 0 node WheelFR
WHEEL RL class street hub -180 -77 33 steer 0 driven 1 handbrake 1 node WheelRL
WHEEL RR class street hub -180  77 33 steer 0 driven 1 handbrake 1 node WheelRR
TRACK left wheels 0 1 2 3 4 driven 4 inertia 10 damping 0.5 brake 15000 final 6 steergain 1.6 pivot 0.6    # KIND tracked
ASSIST abs 1 absslip 0.25 tcs 0 tcsslip 0.35 steerin 3 steerout 5 throttlein 6 throttleout 8 brakein 8 reversebrake 1 reversespeed 50 holdbrake 0.3 holdspeed 30
STEERCURVE 0 1  1500 0.6  3500 0.35           # cm/s, fraction of full lock; up to 4 points
AERO drag 0.35 downforce 0.8                  # N per (m/s)^2
SEAT driver 20 -35 55 exit -20 -140 0         # driver seat and exit point, origin-relative
AUDIO engine Sounds/Engine_Idle.ocaudio idle 1000 Sounds/Engine_High.ocaudio high 6000 pitchlow 0.8 pitchhigh 2.2 skid Sounds/Skid.ocaudio impact Sounds/Crash.ocaudio shift Sounds/Shift.ocaudio
EFFECT skid Particles/TyreSmoke.ocparticle rate 40 impact Particles/Sparks.ocparticle count 12
DAMAGE health 1000 crash 600 energy 0.5 radius 80 shell CarShell        # crash = closing cm/s that starts to hurt; shell = child entity that carries a CDestructible
CAMERA modes chase hood cockpit arm 600 eye 25 -35 110 hood 150 0 95    # chase = VehicleChase rig, hood and cockpit = Seat rigs at those points
HUD UI/VehicleHud.ocui
```

- **Wheel position is the settled hub centre** (what a modeller sees with the car at rest, tyre bottom on the origin plane when `hub z` equals the radius). The builder converts it to the ABI's attachment point the way `Vehicles.PlanWheels` and `world::buildVehicle` already do: `attach z = hub z + susp max - springSag(hz)` with `springSag(hz) = 981 / (2 pi hz)^2` (`Vehicles.cs:192`, `VehicleSystem.cpp:350,432`).
- A `WHEEL` names its `class` (a `WHEELCLASS`, Unreal's wheel class: radius, width, brakes, suspension, tyre) and may override `steer`, `driven`, `handbrake` (1 = this wheel carries the class's handbrake torque, 0 = none), `brake`, and per-wheel `hub`. `node` is the visual child entity name, `bone` the skeleton bone name for the rig route (section 2.6.4).
- **Validity** (`validate`, run by parse and by the editor): at least one wheel; every `class` and `tyre` resolves; wheeled needs a driven wheel, tracked needs both `TRACK` records with their wheels in range; `CURVE` ascending; every number finite and in the ABI's ranges (the same ranges `aver_phys_vehicle_add_wheel` already refuses, `PhysicsVehicle.cpp:215-222`); at most 16 wheels (`PhysicsVehicle.cpp:62`); an axle index in `AXLE` that the geometry does not produce is a warning, not an error.
- Defaults for a missing record are the table column in 2.2, so the smallest legal file is `OCVEHICLE 1`, a `BODY`, one `WHEELCLASS` and four `WHEEL` lines.
- **Built-in presets** are not files: `car`, `van`, `truck`, `bus`, `sports` reproduce the five rows of `VehicleSystem.cpp:312-318` and derive their wheel layout from the mesh bounds with the formulas at `:327-337,404-453` (`specFromBounds`); `tank` is new (tracked, 5 road wheels a side, 38 t, 800 N*m engine; all UNVERIFIED). A `CVehicle` or a traffic `vehicle` token that names one gets the traffic behaviour as it is today, with the driver aids off.

### 2.5 `Aver.Vehicle` (modules/vehicle, pure)

`module.json`: `"name":"Aver.Vehicle"`, `"option":"AVER_MODULE_VEHICLE"`, default on, `"skeleton"` removed, `dependsOn: Aver.Core, Aver.Formats, Aver.Physics`, consequence "no `.ocvehicle` assets, no Vehicle component and no AI traffic cars" (the traffic system's preset table *moves* here, so with the module off `world::VehicleSystem` builds nothing and logs that once). Added in the root `CMakeLists.txt` inside `if(AVER_MODULE_PHYSICS)` after `modules/physics` (`:234-236`). Headers (names are the contract; bodies are the implementer's):

```cpp
namespace aver::vehicle {
enum class Kind : u8 { Wheeled, Tracked };
enum class DiffMode : u8 { Open, LimitedSlip, Locked };
struct TyreCurve { std::string name; std::vector<Vec2> longitudinal, lateral; };          // (slip ratio, mu), (slip degrees, mu)
struct WheelClass { std::string name, tyre; f32 radius, width, suspMin, suspMax, suspHz, suspDamping,
                    brakeNm, handbrakeNm, inertia, spinDamping; };
struct WheelDef { std::string name, wheelClass, node, bone; Vec3 hub; f32 steerDeg; bool driven, handbrake;
                  f32 brakeScale = 1; /* optional per-wheel overrides carried as optionals */ };
struct AxleDef { f32 finalDrive = 3.42f; DiffMode diff = DiffMode::Open; f32 split = 0.5f, lsd = 1.4f, share = 0, antiroll = 1000; };
struct TrackDef { std::vector<u32> wheels; u32 driven; f32 inertia, damping, brakeNm, finalDrive, steerGain, pivot; };
struct EngineDef { f32 torque, minRpm, maxRpm, inertia, damping; std::vector<Vec2> curve; };
struct GearboxDef { bool manual; std::vector<f32> forward, reverse; f32 switchTime, clutchRelease, latency, up, down, clutch; };
struct AssistDef { /* the VD block, defaults of 2.2 except enabled = true for a spec that has an ASSIST record */ };
struct VehicleSpec {
  std::string name; Kind kind; BodyDef body; std::vector<BoxDef> boxes; EngineDef engine; GearboxDef gearbox;
  DiffMode center; f32 centerLsd; std::vector<AxleDef> axles; std::vector<TyreCurve> tyres; std::vector<WheelClass> classes;
  std::vector<WheelDef> wheels; TrackDef tracks[2]; AssistDef assist; SteerCurve steer; f32 drag, downforce;
  SeatDef seat; AudioDef audio; EffectDef effects; DamageDef damage; CameraDef camera; std::string hud;
};
bool parseOcVehicle(std::string_view text, VehicleSpec& out, std::string& err);
std::string writeOcVehicle(const VehicleSpec&);
bool validate(const VehicleSpec&, std::string& why);
const VehicleSpec* builtinSpec(std::string_view name);                     // fixed-geometry specs the editor offers (sedan, tank), not the traffic five
bool specFromBounds(std::string_view preset, const Vec3& sizeCm, VehicleSpec& out);   // car van truck bus sports tank: the traffic derivation, moved here
struct BuiltVehicle { i32 vehicle = 0, body = 0; u32 wheelCount = 0; Kind kind; };
bool buildVehicle(const VehicleSpec&, const Transform& origin, BuiltVehicle& out, std::string& why);   // create, wheels, engine, gearbox, axles, tracks, assists, aero, finish
}
```

`buildVehicle` is the only code that turns a spec into ABI calls; the traffic system, `VehiclePhysicsSystem` and the tests all call it. It calls v1 functions for what v1 can express and v2 for the rest, so a spec equal to the traffic preset's numbers issues the same v1 calls with the same arguments (the regression test compares them).

### 2.6 `Aver.Vehicle.Scene` (modules/vehicle.scene)

`module.json`: `"name":"Aver.Vehicle.Scene"`, `"dir":"vehicle.scene"`, `"option":"AVER_MODULE_VEHICLE_SCENE"`, default on, `dependsOn: Aver.Core, Aver.Formats, Aver.Physics, Aver.Scene, Aver.Vehicle`, `optionalDependsOn: Aver.Anim.Scene` (rig inputs). Added in the root `CMakeLists.txt` in the scene block beside `softbody.scene` (`:309-313`). Public macro `AVER_MODULE_VEHICLE_SCENE=1`. Regenerate `cmake/modules.catalog.json` with `scripts/module-catalog.py`; run `scripts/module-guard-audit.py`.

#### 2.6.1 `CVehicle` (registered at runtime; 96 bytes)

```cpp
inline constexpr u32 kVehicleNoActions = 0x1;       // negative sense: zero-filled = reads Drive.* actions while it has a driver
inline constexpr u32 kVehicleNoAudio = 0x2, kVehicleNoEffects = 0x4, kVehicleNoWheelVisuals = 0x8, kVehicleIndestructible = 0x10;
inline constexpr u32 kVehicleFlipRequest = 0x20, kVehicleRepairRequest = 0x40;     // one-shot: a script sets, the system clears
struct CVehicle {                                   // "CVehicle"
  u64 asset = 0;                                    //  0  .ocvehicle ObjectId; 0 = preset
  u32 preset = 0;                                   //  8  0 car 1 van 2 truck 3 bus 4 sports 5 tank (when asset == 0)
  u32 flags = 0;                                    // 12
  f32 inThrottle = 0, inSteer = 0, inBrake = 0, inHandbrake = 0;   // 16..31 hold until set again
  f32 powerScale = 0, gripScale = 0, brakeScale = 0, damageScale = 0;   // 32..47 0 reads 1
  u32 driver = 0;                                   // 48 OUT: the driving entity
  i32 vehicle = 0;                                  // 52 OUT: the physics vehicle handle
  u32 outState = 0;                                 // 56 OUT: bit0 awake, 1 grounded, 2 reversing, 3 ABS, 4 TCS, 5 flipped, 6 shifting, 7 destroyed, 8 manual, 9 skidding
  i32 outGear = 0;                                  // 60 OUT: -1 reverse, 0 neutral, 1..
  f32 outSpeedCmS = 0, outForwardCmS = 0, outRpm = 0, outHealth01 = 0;   // 64..79 OUT (health reads 1 until the first damage)
  f32 outThrottle = 0, outSteer = 0, outBrake = 0;  // 80..91 OUT: as applied after the aids
  u32 outWheelsOnGround = 0;                        // 92 OUT
};
static_assert(sizeof(CVehicle) == 96, "CVehicle must be padding-free");
```

`VehiclePhysicsSystem::registerComponents(world)` registers it with `registerComponent<T>("CVehicle").field(...).verify(sizeof)`; OUT fields are read-only fields (the `CSoftBody::body` precedent). `attach(world, e, asset, preset)` writes defaults over the zero-filled bytes. **Scripts and the relay set inputs through these fields** (the generic scene field ABI), so no relay call is needed for the common loop; the relay carries the operations that need arguments (section 2.9). `outHealth01 == 0` before the first tick reads as "not yet built", so a freshly attached component reports `Health01 = 1` only once built.

#### 2.6.2 System

```cpp
class VehiclePhysicsSystem {
 public:
  static u32 registerComponents(scene::World&);
  static CVehicle* attach(scene::World&, scene::Entity, u64 asset, u32 preset);
  void install(scene::World&);  void uninstall();
  void setSpecSource(SpecSourceFn);        // u64 id -> const VehicleSpec* (host content cache, as SoftBodySystem::setAssetSource)
  void setBoundsSource(BoundsFn);          // entity -> mesh bounds, for preset-only vehicles (traffic derivation)
  void setActionSource(ActionFn);          // name -> {value, held, pressed}; default: aver_fw_action_*; tests inject
  void setAudioPort(AudioPort*);           // load / play / set / stop; default: the C audio ABI when AVER_WITH_AUDIO_ABI
  void setSoundSource(SoundSourceFn);      // u64 asset id -> audio sound handle (host resolver)
  void setEffectSink(EffectFn, void*);     // (effectId, pos, dir, count); default aver::spawnEffectAt (Aver.Core seam), tests inject
  void setCrashSink(CrashFn, void*);       // (entity, point, dir, radiusCm, energyJ, kind) -> SOFTBODY_DEPTH fields
  void setCameraSink(CameraFn, void*);     // (vehicle, driver, CameraEvent{Enter, Exit, Next, Impact}, VehicleCameraInfo) -> GAMEPLAY_CAMERAS rigs
  void setNotifySink(NotifyFn, void*);     // (entity, "VehicleCrash", user): the named-event seam graphs already receive
  void setViewPos(const Vec3&);            // camera, for the per-vehicle LOD
  void prePhysics(scene::World&, f32 dt);  // before aver_phys_step
  void postPhysics(scene::World&, f32 dt); // after it, before the world's flush
  void clear();                            // destroys vehicles (not entities), voices, queued events
  bool enter(scene::Entity vehicle, scene::Entity driver);   bool exit(scene::Entity vehicle);
  bool shift(scene::Entity, i32 delta);  bool setGear(scene::Entity, i32 gear);  bool setManual(scene::Entity, bool);
  bool flip(scene::Entity, f32 liftCm);  bool repair(scene::Entity);  bool damage(scene::Entity, f32 amount01);
  bool tune(scene::Entity, std::string_view key, f32 value);                 // keys of 2.12.3
  bool telemetry(scene::Entity, Telemetry&) const;   bool wheel(scene::Entity, u32 i, WheelTelemetry&) const;
  u32  drainEvents(VehicleEvent* out, u32 max);  Stats stats() const;
  std::vector<scene::Entity> entities() const;                                // vehicles and their wheel children, for PlayMobility::seedMovable
  DebugView debug(scene::Entity) const;                                       // spec, hub points, contact points: what the overlay draws
};
VehiclePhysicsSystem& vehiclePhysicsSystem();   // process singleton, like anim::animSystem()
```

Every loop is ascending by entity id then wheel index (the determinism contract).

#### 2.6.3 Tick

`prePhysics` (once per frame, before `aver_phys_step`, inside the PLAYING gate):

1. **Resolve** new, changed and removed entries. An entity with a `CVehicle` and a `CMeshRenderer` whose spec resolves (asset, or preset plus bounds) gets `vehicle::buildVehicle` at its current world pose (scale must be 1, else one warning and no vehicle, the traffic rule); the chassis body is stamped with `aver_phys_set_entity`; `CVehicle.vehicle` is written. Removal of the component or the entity destroys the vehicle. At most 2 builds per frame (the rest wait), so a level placing 40 vehicles does not hitch. An entity that also has a `vehicle` placement token, a `CRigidBody` or a `CAnimator` is refused with one warning (the same fights `GameLevel::beginVehicles` already refuses, `GameLevel.cpp:696-700`).
2. **Input.** A vehicle with a driver and without `kVehicleNoActions` reads the actions through `ActionFn`: `Drive.Throttle` (axis, -1..1), `Drive.Brake` (axis, 0..1), `Drive.Steer` (axis), `Drive.Handbrake` (held), `Drive.ShiftUp` / `Drive.ShiftDown` (pressed; in a manual box they call `shift`, in an automatic one they are ignored), `Drive.Reset` (pressed: flip), `Drive.Exit` (pressed: exit), `Drive.CameraNext` (pressed: the camera sink with mode "next"). The result is written into `CVehicle.in*`. A vehicle *without* a driver keeps whatever the fields hold (an AI graph, a script or the Details sliders). Then `inThrottle` ... are sent with `aver_phys_vehicle_set_input`, the scales through `_tune`, and a set `kVehicleFlipRequest` / `kVehicleRepairRequest` runs and clears.
3. **LOD.** Beyond 6000 cm from `setViewPos` a driverless vehicle asks for collision period 2; beyond 15000 cm, 4. A driven vehicle is always period 1. (UNVERIFIED; the numbers are the traffic LOD's order of magnitude.)

`postPhysics`:

4. **Pose.** `aver_phys_vehicle_pose` is written to `CLocal` (position and rotation, scale untouched), only when it changed since the last write (a parked fleet dirties nothing, the traffic rule `VehicleSystem.hpp:239-245`). A vehicle whose entity was destroyed is destroyed with it.
5. **Wheel visuals** (section 2.6.4).
6. **Telemetry.** `_state` and `_wheel_state` once per vehicle per frame (the OUT fields and the cached `Telemetry` / `WheelTelemetry` the C# calls read), `outState` bits, and the hysteresis-free `grounded` and `flipped` flags (`UP_Z` below `cos(pitchroll) + 0.05` for 3 s, the traffic rule `VehicleSystem.cpp:377`).
7. **Events.** `aver_phys_vehicle_event_get` is drained once per frame, mapped to `VehicleEvent`s, the notify sink (`VehicleCrash`, `VehicleSkid`, `VehicleLanded`, `VehicleFlipped`, `VehicleShift`, `VehicleEntered`, `VehicleExited`, each at the vehicle entity) and the audio and effect sinks (2.6.5, 2.8). A crash event also feeds damage: `health -= specHealth * damageScale * clamp((closing - crash) / (3 * crash), 0, 1)` against the spec's `DAMAGE health` (UNVERIFIED shape; documented in the Details tooltip), `outHealth01` follows, and at 0 the `destroyed` state bit is set and the engine is cut (`_tune ENGINE_TORQUE_SCALE 0`).
8. **Driver snap.** A driver entity is moved to the seat point every frame (`CLocal` written from the vehicle pose x `SEAT driver`), its mesh hidden (`kMeshRendererVisible` cleared, restored on exit), and, if it has a physics character, `aver_phys_character_set_gravity_factor(0)` and `aver_phys_character_set_max_strength(0)` are set so it neither falls nor shoves the car, both restored on exit (UNVERIFIED that a CharacterVirtual left inside the chassis does not jitter; fallback in open question 5).

#### 2.6.4 Wheel visuals

- **Child entities (default, any wheel count).** For each `WHEEL ... node X` the system finds the child named `X` under the vehicle entity once, at build, and each frame writes `CLocal.position = wheel_pose.position`, `rotation = wheel_pose.rotation` (steer x spin, origin-relative: exactly what `aver_phys_vehicle_wheel_pose` promises, `physics_vehicle_abi.h:177-183`), so the wheel mesh must have its axle on engine Y, forward on X, origin at the hub. Left and right wheels need their own meshes if the tread is asymmetric (negative scale is not used). A missing child is one warning per name, not an error. Children are listed in `entities()`.
- **Skeletal bones.** For each `WHEEL ... bone B` the system sets rig inputs on the entity through `ControlRigSystem::setInput`: `wheel_<B>_rot` (Quat, model space) and `wheel_<B>_pos` (Vec3, model space); the `.ocrig` carries one `SetBone` op per wheel reading them (`OcRig.hpp:46-60`). The entity's skeleton model space must equal the vehicle origin space (bind pose at the origin, +X forward); the template's tank and sedan are authored that way. **The rig-input cap moves from 8 to 32 names per entity** (`ControlRig.hpp:101-104`, `ControlRig.cpp:301`, warning text updated) because a six-wheel truck needs 12 and a tank 12 plus two track floats; `IkRigSystemTest.cpp:443-452` is changed to "32 fit, the 33rd is refused". The per-entity record grows from 8 to 32 slots (about 770 bytes) and only for entities that have inputs.
- Suspension travel needs no code: it is the wheel's hub moving in the pose.

#### 2.6.5 Audio and effects

Audio goes through an `AudioPort` (defaults to the C audio ABI, `audio_abi.h:66,82,84`; a test injects a fake). Per driven or audible vehicle (audible = within 5000 cm of the view, at most 8 at a time, nearest first):

- **Engine**: two looping voices from the spec's `AUDIO engine` (idle loop and high loop) cross-faded by `rpm01 = (rpm - idle) / (high - idle)`, pitch `lerp(pitchlow, pitchhigh, rpm01)`, volume `0.5 + 0.5 * load` where load is throttle (a lift-off is audibly quieter). Placed at the vehicle with `aver_audio_play_at`-style position updates each frame through the existing voice setters. A single-loop spec (no `high`) pitches one voice. No synthesis: this is the classic loop-crossfade, which carries no patent exposure we know of; the technique to avoid is granular engine synthesis from a recorded sweep (several patents in the audio-game field), which this does not do.
- **Skid**: one looping voice per vehicle whose volume follows the largest skidding wheel's slide speed.
- **One-shots**: impact (volume by closing speed), gear shift, landing.
- `kVehicleNoAudio` and a spec without `AUDIO` silence it. Voices are stopped on `clear()`, on exit from range and when the vehicle dies. The mixer's own voice-steal rule (`Audio.StolenVoices`) is the global cap.

Effects: skid smoke is `rate` particles per second per skidding wheel at the contact point, direction opposite the slide, converted to a count per frame with an accumulator (so a 30 Hz frame emits twice the count, not twice the rate of bursts); sparks on `IMPACT`. Both call the effect sink, whose default is `aver::spawnEffectAt(effectId, position, direction, count)` (`modules/core/include/aver/core/EffectSpawn.hpp`, GPU_PARTICLES section 2.10; it returns 0 and does nothing when no spawner is installed, so the module needs no particle dependency). The effect id is `fnv1a64` of the content-relative `.ocparticle` path, computed once at spec load.

#### 2.6.6 Budgets

`Options` (project-overridable): `maxVehicles` 64 built, `buildsPerFrame` 2, `audibleVehicles` 8, `audioRangeCm` 5000, `lodMidCm` 6000, `lodFarCm` 15000, `skidSampleHz` = every frame. Cost, stated as what it is: each vehicle is one constraint that casts one cylinder per wheel per fixed step (`physics_vehicle_abi.h:45-47`); the collision period is the only lever that reduces it, and **no timing of any of this has been measured**: the headed pass (section 6, item 14) records it before anyone tunes.

### 2.7 Host wiring: editor Play and the packaged game

Both hosts get the same three additions through `GameSystemsWiring.hpp` (shared, additive), so they cannot drift:

```cpp
// registerGameSystems(): VehiclePhysicsSystem::registerComponents(world); install(world); aver_fw_set_vehicle_provider(&vehicleProvider, &vehiclePhysicsSystem());
inline void tickVehiclesPre(f32 dt, const Vec3& viewPos);    // sets view pos, prePhysics
inline void tickVehiclesPost(f32 dt);                        // postPhysics
// resetGameSystems(): vehiclePhysicsSystem().clear();
```

- **Editor Play** (`sandbox/src/SandboxUpdate.cpp:766-778`): `tickVehiclesPre` next to `vehicles_.prePhysics` and `tickVehiclesPost` next to `vehicles_.postPhysics`, inside the same PLAYING gate and the same `PlayPhase::Vehicles` profiler span (no new phase). `SandboxInit.cpp:330` registers (through `registerGameSystems`), `SandboxPlay.cpp:157` adds `playMobility_.seedMovable(vehiclePhysicsSystem().entities())` after the traffic seed, `SandboxPlay.cpp:666` resets (through `resetGameSystems`). The save-mid-Play set at `SandboxLevelEdit.cpp:632` gains the same entities so a save never writes a driven car's live pose. Stop restores transforms through the existing Play snapshot (vehicle and wheel entities are level entities, so the snapshot has them; the traffic cars rely on the same).
- **Packaged game** (`Runtime/src/GameApp.cpp:579-584`): the same two calls beside the traffic ones; `:1929` registers; `:2026` seeds; `:2471` clears. The host installs the spec source (a `.ocvehicle` parse cache over the content resolver, the soft-body asset cache pattern, `SandboxInit.cpp:332`), the bounds source (`content_.boundsFor`), the sound source and the sinks of 2.8.
- **GPU parity.** No shader, no pass, no RHI call, no binding slot (so no table-0 / table-1 register base to assert), nothing backend-specific: a vehicle is a `CMeshRenderer` entity plus children, drawn by whichever backend is active. The one overlay uses `createLineMesh` / `drawLines`, which both backends implement (`SandboxRender.cpp:2018-2021`). Checked headed on `--backend vulkan` (section 6, item 12).
- **Traffic.** `GameLevel::beginVehicles` (`:651`) passes the preset string through; `world::VehicleSystem` resolves it with `vehicle::specFromBounds` for the five names and with the host's spec source for `*.ocvehicle`, builds with `vehicle::buildVehicle`, and loses its `kPresets` table and `buildVehicle` (the code moves, the behaviour does not). A tracked or non-wheeled spec is refused for a lane-following car with one log line. `beginVehicles` skips an entity that carries a `CVehicle`. `Aver.World` gains `Aver.Vehicle` in `optionalDependsOn`.

### 2.8 Cross-feature contracts (consumed; signatures live in the owning docs)

| Owner doc | What this feature calls | How |
|---|---|---|
| GAMEPLAY_CAMERAS section 2.12 (`Entity.AddCameraRig(CameraRigPreset, Entity target)`, `SetCameraRigMode`, `SetCameraRigTarget`, `SetCameraRigIgnoreBody(int bodyHandle)`, `CameraRig.SetViewTarget(Entity rig, float blendSeconds, curve, bool cut)`, `CameraRig.ShakeAt(Vec3, radiusCm, intensity, falloffPower)`; C++ `camera::CameraRigSystem::attach(world, rig, preset, target)`, `setViewTarget`; C `aver_fw_camera_attach`) | On `enter`, the camera sink creates the rigs the spec's `CAMERA modes` name as **children of the vehicle**: `chase` is a `VehicleChase` rig targeting the vehicle with `SetCameraRigIgnoreBody(chassis body)` (the system passes `aver_phys_vehicle_body`) and the spec's `arm` as the rig's arm length; `hood` and `cockpit` are `Seat` rigs placed at the spec's `hood` and `eye` points. It then calls `SetViewTarget(firstRig, 0.6)`. `Drive.CameraNext` calls `SetViewTarget` on the next rig in the list (0.4 s blend). On `exit` it restores the previous view target (the driver's own camera) and destroys the rigs it made. An `IMPACT` above 300 cm/s may call `CameraRig.ShakeAt(point, 600, clamp(closing / 2000, 0, 1))` (optional in the camera doc). **No camera math, spring arm, collision probe or blend is written here.** | `CameraFn` sink wired in `GameSystemsWiring.hpp`; in C# `Entity.EnterVehicle` calls the same `CameraRig` members |
| GPU_PARTICLES section 2.10 (`aver::spawnEffectAt(u64 effectId, const Vec3& position, const Vec3& direction, u32 count)` in `Aver.Core`; `particles::spawnAt`; `Particles.SpawnAt(effect, position, direction, count[, speedScale, sizeScale, lifetimeScale, inheritedVelocity])`; `aver_fw_particles_spawn_at`) | Skid smoke and impact sparks, `count` per frame from the accumulator. Effects are authored pointing +Z and the call passes the world direction (opposite the slide for smoke, the contact normal for sparks) | the default `EffectFn` is `aver::spawnEffectAt`; the C# `Vehicles` code never calls particles, the system does |
| SOFTBODY_DEPTH (field system: radial and directional force, strain, anchor release, kill volume; **signature not yet written, assumed**) | On a crash above the spec's `crash` threshold: a radial strain / force field at the contact point on the vehicle's `shell` child (an entity carrying a `CDestructible`, so panels dent). The other party's damage already comes from the rigid chassis hitting its soft body, which the soft-body system's impact polling handles on its own. Anchor release is requested on `destroyed` (hood and doors pinned in a shell come off) | `CrashFn` sink: `energy = 0.5 * m_reduced * closing^2`, `m_reduced` from the event's other-mass, passed with point, normal, `radius` and `kind = Impact` |

Vehicles owns no particle, camera or field code. If a sibling's signature changes, only the three host-wiring lambdas change.

### 2.9 Framework relay: `framework_vehicle_abi.h` (new, `aver_fw_vehicle_*`)

Provider-relay ABI (the `framework_softbody_abi.h` pattern), own `AVER_FW_VEHICLE_ABI_VERSION 1`, `int32_t / float / const char*` only, 1 on success, no provider returns 0. New files `modules/framework/include/aver/framework/framework_vehicle_abi.h`, `modules/framework/src/VehicleAbi.cpp`; the provider is `vehicleProvider` in `Aver.Vehicle.Scene`.

```c
#define AVER_FW_VEHICLE_ABI_VERSION 1
typedef int32_t (AVER_FW_CALL* aver_fw_vehicle_fn)(int32_t op, int32_t entity, const float* in, int32_t inCount,
                                                   const char* text, float* outF, int32_t* outI, int32_t cap, void* user);
AVER_FW_ABI int32_t aver_fw_set_vehicle_provider(aver_fw_vehicle_fn fn, void* user);
AVER_FW_ABI int32_t aver_fw_vehicle_abi_version(void);
AVER_FW_ABI int32_t aver_fw_vehicle_spawn(const char* assetOrPresetOrInlineText, float x, float y, float z, float yawDeg,
                                          int32_t* outEntity, int32_t* outVehicle);   /* text beginning "OCVEHICLE" is parsed inline; creates the entity, mesh-less, with CVehicle */
AVER_FW_ABI int32_t aver_fw_vehicle_enter(int32_t vehicle, int32_t driver);
AVER_FW_ABI int32_t aver_fw_vehicle_exit(int32_t vehicle, int32_t* outDriver);
AVER_FW_ABI int32_t aver_fw_vehicle_shift(int32_t vehicle, int32_t delta);
AVER_FW_ABI int32_t aver_fw_vehicle_set_gear(int32_t vehicle, int32_t gear);
AVER_FW_ABI int32_t aver_fw_vehicle_set_manual(int32_t vehicle, int32_t manual);
AVER_FW_ABI int32_t aver_fw_vehicle_flip(int32_t vehicle, float liftCm);
AVER_FW_ABI int32_t aver_fw_vehicle_repair(int32_t vehicle);
AVER_FW_ABI int32_t aver_fw_vehicle_damage(int32_t vehicle, float amount01);
AVER_FW_ABI int32_t aver_fw_vehicle_tune(int32_t vehicle, const char* key, float value);
AVER_FW_ABI int32_t aver_fw_vehicle_telemetry(int32_t vehicle, float* outF /*VS block*/, int32_t nF, int32_t* outI, int32_t nI);
AVER_FW_ABI int32_t aver_fw_vehicle_wheel(int32_t vehicle, int32_t wheel, float* outF, int32_t nF, int32_t* outI, int32_t nI);
AVER_FW_ABI int32_t aver_fw_vehicle_drain_events(int32_t* outI /*6 per event*/, float* outF /*8 per event*/, int32_t max);
```

`vehicle` in this ABI is the **entity** (the system maps entity to the physics handle); `outVehicle` of `spawn` is the physics handle for the legacy nodes. Every call is applied at the next tick except `spawn` (immediate, so the caller can keep the entity) and the telemetry reads (last tick's cache). Component add, remove and the input fields use the generic scene ABI, as soft bodies and motion matching do.

### 2.10 C# API

Every public member gets an XML `<summary>`; the node methods of 2.11 wrap these, never the reverse (wave 9 decision 5). `Aver.Framework` is additive; the integrator bumps its version once for the wave.

**`Aver.Physics`** (`Vehicle.cs` becomes `public readonly partial struct Vehicle`; the new members live in `VehicleExt.cs`, the P/Invokes in `Native.VehicleExt.cs`, every name copied character for character from `physics_vehicle_ext_abi.h`, the `Native.Ragdoll.cs` rule so `AbiEnumTest` pairs them):

```csharp
public enum VehicleKind { Wheeled = 0, Tracked = 1 }
public enum DifferentialMode { Open = 0, LimitedSlip = 1, Locked = 2 }
public enum VehicleTune { EngineTorqueScale, BrakeScale, GripScale, SuspensionStiffnessScale, SteerScale, DragScale, DownforceScale }
public readonly partial struct Vehicle
{
    /// <summary>The ABI version the loaded physics library reports (2 with the extended vehicle ABI, 1 without).</summary>
    public static int AbiVersion { get; }
    public bool SetKind(VehicleKind kind);                                   // before AddWheel
    public bool SetChassisBoxes(ReadOnlySpan<float> boxes);                  // cx cy cz hx hy hz per box, 1..8
    public bool SetEngine(float maxTorque, float minRpm, float maxRpm, float inertia = 0.5f, float damping = 0.2f);
    public bool SetTorqueCurve(ReadOnlySpan<float> rpmFraction, ReadOnlySpan<float> torqueFraction);
    public bool SetGearbox(bool manual, ReadOnlySpan<float> forward, ReadOnlySpan<float> reverse, GearboxTiming timing = default);
    public int  AxleCount { get; }       public int AxleOf(int wheel);
    public bool SetAxle(int axle, float finalDrive, DifferentialMode mode, float lsdRatio = 1.4f, float split = 0.5f, float torqueShare = 0, float antiRollNmPerM = 1000);
    public bool SetCenterDifferential(DifferentialMode mode, float lsdRatio = 1.4f);
    public bool SetWheel(int wheel, float inertia = 0.9f, float angularDamping = 0.2f, float brakeBias = 1f);
    public bool SetWheelCurves(int wheel, ReadOnlySpan<float> longSlip, ReadOnlySpan<float> longMu, ReadOnlySpan<float> latDeg, ReadOnlySpan<float> latMu);
    public bool SetTrack(int side, ReadOnlySpan<int> wheels, int drivenWheel, float inertia = 10, float damping = 0.5f, float brakeNm = 15000, float finalDrive = 6, float steerGain = 1.6f, float pivotPower = 0.6f);
    public bool SetAssists(in DriverAids aids);   public bool SetAero(float drag, float downforce);   public bool SetCollisionPeriod(int everyNSteps);
    public bool Tune(VehicleTune key, float value);
    public bool SetManualGearbox(bool manual);    public bool SetGear(int gear);   public bool Shift(int delta);   public bool Flip(float liftCm = 50f);
    public bool TryGetState(out VehicleState state);   public bool TryGetWheelState(int wheel, out WheelState state);
    public static int DrainEvents(Span<VehiclePhysicsEvent> buffer);
}
```

`VehicleState`, `WheelState` and `VehiclePhysicsEvent` are plain structs holding the `VS`, `VSI`, `VWS`, `VWSI` and event fields of 2.2 by name.

**`Aver.Framework`** (`EntityVehicle.cs`, `VehicleTelemetry.cs`, `VehicleNative.cs` with `FwVehicle`, `Lib = "Aver.Framework"`):

```csharp
public enum VehiclePreset { Car, Van, Truck, Bus, Sports, Tank }
public enum VehicleEventKind { SkidBegin = 1, SkidEnd, Land, Impact, Shift, Flipped, Assist }
public readonly record struct VehicleTelemetry(float SpeedCmS, float ForwardSpeedCmS, float Rpm, int Gear, int GearCount, bool Manual, bool Shifting,
    float Throttle, float Brake, float Steer, float Handbrake, int WheelsOnGround, bool AbsActive, bool TcsActive, bool Flipped, bool Destroyed,
    float Health01, float AirborneSeconds, float LongAccelG, float LatAccelG, float TrackLeftCmS, float TrackRightCmS);
public readonly record struct WheelTelemetry(bool Contact, float SlipLongitudinal, float SlipLateralDeg, float LoadN, float CompressionCm,
    float AngularVelocityRadS, float SteerDeg, float SpinRad, Vec3 ContactPoint, Vec3 ContactNormal, float ForceLongitudinalN, float ForceLateralN,
    float SurfaceFriction, Entity SurfaceEntity, bool Skidding);
public readonly record struct VehicleEvent(VehicleEventKind Kind, Entity Vehicle, int A, int B, Vec3 Point, Vec3 Normal, float Value, float Value2, int Seq);

public readonly partial struct Entity
{
    /// <summary>Adds a Vehicle component: from an .ocvehicle path, or from a built-in preset sized by the entity's mesh. Needs a mesh.</summary>
    public bool AddVehicle(string asset = "", VehiclePreset preset = VehiclePreset.Car);
    public bool IsVehicle { get; }
    /// <summary>The driver's pedals: throttle -1..1 (negative reverses or brakes, see the aids), steer -1..1 (+1 right), brake and handbrake 0..1. Holds until set again.</summary>
    public bool SetVehicleInput(float throttle, float steer, float brake = 0f, float handbrake = 0f);
    public bool TryGetVehicleTelemetry(out VehicleTelemetry telemetry);
    public bool TryGetWheelTelemetry(int wheel, out WheelTelemetry telemetry);
    public int  VehicleWheelCount { get; }
    public bool SetVehicleManualGearbox(bool manual);  public bool SetVehicleGear(int gear);  public bool ShiftVehicle(int delta);
    /// <summary>Puts <paramref name="driver"/> in the seat: reads the Drive.* actions, attaches the camera rig, hides and parks the driver. False if taken or not a vehicle.</summary>
    public bool EnterVehicle(Entity driver);
    public bool ExitVehicle();                         public Entity VehicleDriver { get; }
    public bool FlipVehicle(float liftCm = 50f);       public bool RepairVehicle();       public bool DamageVehicle(float amount01);
    /// <summary>Live tuning by key: power, brake, grip, suspension, steer, drag, downforce, abs, tcs, manual (section 2.12.3).</summary>
    public bool TuneVehicle(string key, float value);
}

public static partial class Vehicles   // the existing class, additions
{
    /// <summary>Makes a vehicle entity from an .ocvehicle path or a built-in preset name ("car", "van", "truck", "bus", "sports", "tank") at a pose. Needs the vehicle system in the host.</summary>
    public static Entity Spawn(string assetOrPreset, Vec3 position, float yawDegrees = 0f);
    public static Entity SpawnFromText(string ocvehicleText, Vec3 position, float yawDegrees = 0f);
    public static int DrainEvents(Span<VehicleEvent> buffer);
}
public readonly record struct CarSpec { /* unchanged fields */ public string ToOcVehicle(); }
public sealed class DrivingContext : InputMappingContext
{
    /// <summary>The default driving bindings: W / Up / right trigger throttle, S / Down / left trigger brake, A D / left stick steer, Space / A handbrake, E and Q shift, R reset, F exit, C camera. Rebindable through the existing SaveBindings / RebindAction.</summary>
    public DrivingContext();
    public const int Priority = 100;                   public static void Push();   public static void Pop();
    public static InputAction Throttle { get; }  /* "Drive.Throttle" */  /* Brake, Steer, Handbrake, ShiftUp, ShiftDown, Reset, Exit, CameraNext likewise */
}
public sealed class VehicleHud
{
    /// <summary>Binds a layout by widget names Speed, Gear, Rpm, RpmBar, Health, AbsLamp, TcsLamp (any may be absent) to a vehicle.</summary>
    public VehicleHud(Widget root, Entity vehicle, bool miles = false);
    public void Update();                              public static string FormatSpeed(float cmPerSecond, bool miles = false);
}
```

- **`Vehicles.CreateCar(position, yaw, CarSpec)` keeps its signature and result.** It now writes `spec.ToOcVehicle()` (the four-wheel plan `Vehicles.PlanWheels` already computes, front pair steered, rear pair driven with the handbrake) and calls `SpawnFromText`; `Car.Vehicle` is the physics `Vehicle` of the built vehicle (from `CVehicle.vehicle`) so the six old rows and every caller of `Car.Vehicle.*` still work. `CarSpec.Default` maps to `OCVEHICLE` records with the v1 defaults for every new field, so `CarSpec.Default` drives as it does today. `PlanWheels`, `AxleOf`, `CountAxles`, `CountDifferentials` and `SpringSagCm` stay (the tests use them). `SyncEntities()` stays as a no-op for cars the system owns. The `EnsureSync` timer code is removed from `Vehicles.cs`; a host without the vehicle provider answers `Car.None`, which `VehicleNodeTests` already treats as the "library absent" case.
- `Car` gains `TryGetTelemetry`, `Shift`, `Flip`, `Enter` as one-line forwards to the `Entity` members.

### 2.11 Aver Node: family `Vehicle` v2

Wave 9 C contract, exactly. `GraphVehicleNodeTable.cs` keeps `VehicleTable()` with the six rows **unchanged in key, pins, defaults and behaviour** (pins in 1.2) and appends fourteen; `GraphNodeDefsVehicle.hpp` mirrors them pin for pin; `VehicleNodeTests` (`[AutoRun]`, built from `NodeRowKit`) gains the rows; owner methods are `internal static` thin wrappers in `VehicleGraph.cs` over the public C#; failures return false/0 and log once per node id, never throw. Nothing is edited in `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs` or `Program.cs`. Entities are ints, strings are attributes, enums are ints documented in `docs/nodes/Vehicle.md` (generated by `scripts/node-catalog.py --write`). Category `Vehicle`.

| Key | Kind | Pins (besides `exec` / `then`) | Attr | Wraps |
|---|---|---|---|---|
| `an_vehiclespawn` | Exec | in `x y z yaw`; out `vehicle:i` (physics handle, for the old rows), `entity:i`, `success:b` | `asset` (path or preset name) | `Vehicles.Spawn` |
| `an_vehicledrive` | Exec | in `entity:i`, `throttle`, `steer`, `brake`, `handbrake`; out `success:b` | | `Entity.SetVehicleInput` |
| `an_vehicleshift` | Exec | in `entity:i`, `delta:i`; out `success:b` | | `ShiftVehicle` |
| `an_vehiclesetgear` | Exec | in `entity:i`, `gear:i` (-1 reverse, 0 neutral); out `success:b` | | `SetVehicleGear` |
| `an_vehiclegearmode` | Exec | in `entity:i`, `manual:b`; out `success:b` | | `SetVehicleManualGearbox` |
| `an_vehicleenter` | Exec | in `entity:i`, `driver:i`; out `success:b` | | `EnterVehicle` |
| `an_vehicleexit` | Exec | in `entity:i`; out `driver:i`, `success:b` | | `ExitVehicle` |
| `an_vehicleflip` | Exec | in `entity:i`, `liftCm=50`; out `success:b` | | `FlipVehicle` |
| `an_vehicletune` | Exec | in `entity:i`, `value`; out `success:b` | `key` | `TuneVehicle` |
| `an_vehiclerepair` | Exec | in `entity:i`, `amount=1` (health restored, 1 = full); out `success:b` | | `RepairVehicle` / `DamageVehicle` |
| `an_vehicletelemetry` | Pure | in `entity:i`; out `found:b`, `speedCmS`, `rpm`, `gear:i`, `throttle`, `steer`, `brake`, `wheelsOnGround:i`, `health`, `abs:b`, `tcs:b` | | `TryGetVehicleTelemetry` |
| `an_vehiclewheelstate` | Pure | in `entity:i`, `index:i`; out `found:b`, `contact:b`, `slipLong`, `slipLatDeg`, `loadN`, `compressionCm`, `angVelRadS`, `surfaceFriction`, `skidding:b` | | `TryGetWheelTelemetry` |
| `an_vehicledriver` | Pure | in `entity:i`; out `driver:i` | | `VehicleDriver` |
| `an_vehiclefromentity` | Pure | in `entity:i`; out `vehicle:i` (0 when none), `found:b` | | maps an entity to the handle the old rows take |

Events reach graphs through the existing bus entry: a graph handles `AN_OnEvent` with the names `VehicleCrash`, `VehicleSkid`, `VehicleLanded`, `VehicleFlipped`, `VehicleShift`, `VehicleEntered`, `VehicleExited` (raised at the vehicle entity by the notify sink); no new entry node. The numbers of the last crash are read by `an_vehicletelemetry` neighbours or `Vehicles.DrainEvents`.

### 2.12 Level records, tuning keys

**2.12.1 `COMPONENT Vehicle`** (`docs/formats/FORMAT_SPECS.md:977` table): `COMPONENT Vehicle asset=Vehicles/Sedan.ocvehicle preset=car power=1 grip=1 brake=1 damage=1 noactions=0 noaudio=0 noeffects=0 nowheels=0 indestructible=0`. Canonical form omits default keys (`PlacedComponentModel.hpp:195-199`). Runtime state (driver, health, gear, damage) is never saved; a loaded level starts with an intact, unoccupied vehicle. Type name `"Vehicle"` is appended to `kPlacedComponentTypes` (`LevelComponents.hpp:28`) and to the list at `OcWorld.cpp:173-174`, with an enum value and `applyVehicle` / `capture` cases beside `applyCloth` (`LevelComponents.cpp:397`). Apply refuses (one warning) an entity with a `vehicle` token, a `CRigidBody` or an object-clip `CAnimator`.

**2.12.2 The `vehicle` placement token** now also takes a content path: `vehicle Vehicles/Bus.ocvehicle`. The parser already stores any percent-encoded string (`OcWorld.cpp:837`), so no reader change; `FORMAT_SPECS.md:916` gains the sentence. Unknown names still build as `car`.

**2.12.3 Tuning keys** (case-insensitive; used by `TuneVehicle`, `an_vehicletune`, the Details sliders, `CVehicle.*Scale`): `power` (engine torque scale), `brake`, `grip`, `suspension`, `steer`, `drag`, `downforce`, `abs` (0 or 1), `tcs` (0 or 1), `manual` (0 or 1), `collisionperiod` (1..8). An unknown key returns false and logs once.

### 2.13 Editor

- **Add Component.** A `PlacedKind` row after `MorphTargets` (`PlacedComponentModel.hpp:41-76`): `{"Vehicle","Vehicle","Makes this mesh a drivable vehicle (.ocvehicle): engine, gearbox, differentials, wheels. Not for traffic cars (those use the vehicle placement token).","Vehicle",0,"CVehicle",{".ocvehicle"}}`. `AttachContext` gains `hasVehicle`, `hasRigidBody`, `hasVehicleToken`; `canAttach` needs a mesh and refuses the three conflicts with a one-line reason. `makeNewRecord` seeds `preset=car`.
- **Details section** (`VehicleDetailsModel.hpp`, ImGui-free and tested; `VehicleDetailsSection.cpp` the widget half, called from `SandboxComponentDetails.cpp` by one dispatch block so the large file does not grow): asset picker and preset combo (Car, Van, Truck, Bus, Sports, Tank), **Create .ocvehicle** (writes `<mesh dir>/<mesh>.ocvehicle` from the preset's derived spec and assigns it, the `Create .ocsoft` pattern), sliders for the scales and flags; in Play a live block: RPM bar, gear (with shift buttons when manual), speed in km/h, applied pedals, health, ABS / TCS lamps and a per-wheel table (contact, slip long and lateral, load, compression, surface friction, skidding); buttons **Drive** (enters with the editor camera's pawn or a ghost driver), **Flip**, **Repair**, **Stop**.
- **`.ocvehicle` editor tab** (`VehicleEditorModel.hpp` pure, `VehicleEditor.{hpp,cpp}` widgets; opened from the Content Browser like the other asset editors): sections Body, Engine (an editable torque-curve plot, a computed power curve beside it, points draggable), Gearbox (ratio table plus the computed top speed per gear at the rev limit, from wheel radius and final drive), Axles and Drive (derived layout label FWD, RWD, AWD or 4x4 from the `driven` flags, differential modes), Wheels (table plus a viewport preview that draws every hub, suspension travel and tyre cylinder over the selected entity using the overlay's line path), Tyres (the two friction-curve plots), Assists, Aero, Audio, Effects, Damage, Camera, a validation list that is `vehicle::validate`'s output. Undo through the snapshot undo the other asset editors use. Edits apply to vehicles built *after* the save (the next Play start or spawn); the live-tune sliders on the Details section are the in-Play loop.
- **`Show > Vehicles` overlay** (`VehicleDebugOverlay.{hpp,cpp}`, the `SoftBodyDebugOverlay` pattern: `collect` into `rhi::LineVertex` from `VehiclePhysicsSystem::debug`, drawn under `!noEditorChrome_`): *suspension* (attachment point to hub to contact point, green to red by compression), *forces* (longitudinal red and lateral blue arrows from `FORCE_*_N`, 1 cm per 100 N, scale slider), *slip* (a ring at each contact tinted by slip, hot when skidding), *chassis* (the collision boxes and the centre of mass), *velocity* (arrow), *labels* (speed, gear, RPM, throttle and brake bars, ABS and TCS) at the vehicle. A runtime text overlay is the HUD itself: the packaged game has no line-overlay path today and none is added.
- Content Browser: `cbCreateVehicleAsset()`, type label `.ocvehicle` = "Vehicle Asset" with an icon row beside `.ocsoft` (`SandboxContentBrowser.cpp:1894`).
- COFF discipline: the new editor code lives in its own translation units; `SandboxComponentDetails.cpp`, `SandboxApp.cpp` and `SandboxRender.cpp` each get a few lines. `GraphNodeDefsVehicle.hpp` adds 14 `push_back`s to the translation unit that holds the whole node catalog; the implementing agent records the Debug object's section count before and after (the cap note in the memory index) and, if it is within 5% of the cap, moves family headers into a second catalog translation unit before merging.

### 2.14 Template and lab content

`templates/Vehicle/` (found by `listTemplates`, `ProjectScaffold.cpp:918`; only `Content/` is copied into a new project). **No C# anywhere**, the FirstPerson standard. The map is an **`.ocworld`**, not an `.ocmap`, because the legacy `.ocmap` cannot store a `COMPONENT` record (`PlacedComponentModel.hpp:97,119`, the `legacyOcmap` reason); the manifest says `STARTMAP Maps/Default.ocworld`.

| File | What it is |
|---|---|
| `Vehicle.octemplate` | `NAME Vehicle`, `DESCRIPTION`, `STARTMAP Maps/Default.ocworld`, `GAMEMODE AN_VehicleRules` |
| `Content/Maps/Default.ocworld` | a flat arena with a road loop, a ramp, an ice strip (body friction 0.05), a gravel strip, a wall; a Sedan and a Tank placed `COMPONENT Vehicle` with wheel children nested `BEGIN/CHILD/END` |
| `Content/Vehicles/Sedan.ocvehicle` | RWD saloon: 5-speed automatic, torque curve, open rear differential, ABS on, `HUD UI/VehicleHud.ocui` |
| `Content/Vehicles/Tank.ocvehicle` | tracked, 5 road wheels a side, 38 t, manual-capable box, pivot steering |
| `Content/Input/Vehicle.ocinput` | the project's one scheme: `Move`, `Look`, `Interact` (E) for the on-foot pawn **and** `Drive.Throttle`, `Drive.Brake`, `Drive.Steer`, `Drive.Handbrake`, `Drive.ShiftUp`, `Drive.ShiftDown`, `Drive.Reset`, `Drive.Exit`, `Drive.CameraNext` (keyboard and gamepad). The on-foot graph ignores `Drive.*`; the vehicle system ignores the rest |
| `Content/UI/VehicleHud.ocui` | `Speed` (text), `Gear` (text), `Rpm` (text), `RpmBar` (progress), `Health` (progress), `AbsLamp`, `TcsLamp` (text) |
| `Content/Scripts/AN_VehicleRules.ocgraph` | GameMode, `pawn=AN_VehicleDriver controller=AN_VehicleController` |
| `Content/Scripts/AN_VehicleController.ocgraph` | the concrete controller (the FirstPerson reason) |
| `Content/Scripts/AN_VehicleDriver.ocgraph` | on foot: walk and look; on `Interact` finds the nearest vehicle within 400 cm and runs `an_vehicleenter`; on `VehicleEntered` / `VehicleExited` events toggles its `driving` VAR so its walk logic stops |
| `Content/Scripts/AN_VehicleHud.ocgraph` | opens `UI/VehicleHud.ocui` on enter, each tick sets the widgets from `an_vehicletelemetry` through the UiExt nodes, closes on exit |
| `Content/Scripts/AN_TankTurret.ocgraph` | rotates the turret child by `Look` while driving the tank |
| `Content/Meshes/*.ocmesh`, `Content/Sounds/*.ocaudio`, `Content/Particles/*.ocparticle` | procedural geometry and tones written by the generator below and imported with `AverAssetC`, committed (a template copies only `Content/`); `SOURCES.md` says so, no third-party assets |
| `Content/README.md` | controls, the spec records, what the template deliberately does not do |

`templates/Vehicle/make_vehicle_template.py` (stdlib Python, not copied into projects) writes the OBJ sources and WAV loops; the committed `.ocmesh` / `.ocaudio` are its imported output (the `test-content/SoftBodyLab/make_soft_lab.py` precedent). The engine loops are an idle tone and a high-rev tone built from a fundamental and 6 harmonics with a slow amplitude wobble; real recordings can replace them by editing two paths in the spec.

`test-content/VehicleLab/` (engine test content, not a demo): a 6x4 truck (`Truck6x4.ocvehicle`: six wheels, two driven axles, a centre differential, `BOX` cab and bed), an AWD hatchback with a switchable limited-slip rear, the tank, an ice-split lane (left wheels on ice, right on tarmac, for the differential check), a brake-test strip, a drop for the landing event, a slalom, a crash wall with a `Destructible` panel, and a rigged wheel car `Source/RiggedCar.gltf` (box body, four wheel bones; written by `make_vehicle_lab.py`) for the skeletal route. If the glTF skin importer does not accept the generated file, the README says the skeletal route is covered by the headless test only.

### 2.15 Determinism and what is claimed

Inputs to the simulation are the pedals, the gear requests and the ordered `Tune`/`Flip`/`Repair` calls; everything else is derived. `vehicleStepBegin` / `vehicleStepEnd` walk vehicles in handle order; the system walks entities in id order; events carry a per-system sequence number; no wall clock, no `rand`. On one binary and one instruction set the same input stream reproduces the same poses and events (a replay test runs it twice and compares bits). Cross-platform lockstep is not claimed (Jolt is built without `CROSS_PLATFORM_DETERMINISTIC`, as the soft-body doc records). The audio and effect sinks are presentation and are excluded from the replay comparison.

---

## 3. FILE OWNERSHIP

### 3.1 Created (only this feature touches them)

Physics
- modules/physics/include/aver/physics/physics_vehicle_ext_abi.h
- modules/physics/src/PhysicsVehicleExt.cpp
- modules/physics/src/PhysicsVehicleTracked.cpp

Aver.Vehicle (pure)
- modules/vehicle/CMakeLists.txt
- modules/vehicle/include/aver/vehicle/VehicleSpec.hpp
- modules/vehicle/include/aver/vehicle/OcVehicle.hpp
- modules/vehicle/include/aver/vehicle/VehiclePresets.hpp
- modules/vehicle/include/aver/vehicle/VehicleBuild.hpp
- modules/vehicle/src/OcVehicle.cpp
- modules/vehicle/src/VehiclePresets.cpp
- modules/vehicle/src/VehicleBuild.cpp

Aver.Vehicle.Scene
- modules/vehicle.scene/CMakeLists.txt
- modules/vehicle.scene/module.json
- modules/vehicle.scene/README.md
- modules/vehicle.scene/include/aver/vehicle/VehiclePhysicsSystem.hpp
- modules/vehicle.scene/src/VehiclePhysicsSystem.cpp
- modules/vehicle.scene/src/VehicleDriver.cpp
- modules/vehicle.scene/src/VehicleVisuals.cpp
- modules/vehicle.scene/src/VehicleAudio.cpp
- modules/vehicle.scene/src/VehicleProvider.cpp

Framework relay
- modules/framework/include/aver/framework/framework_vehicle_abi.h
- modules/framework/src/VehicleAbi.cpp

C#
- scripting/csharp/Aver.Physics/Native.VehicleExt.cs
- scripting/csharp/Aver.Physics/VehicleExt.cs
- scripting/csharp/Aver.Framework/EntityVehicle.cs
- scripting/csharp/Aver.Framework/VehicleTelemetry.cs
- scripting/csharp/Aver.Framework/VehicleNative.cs
- scripting/csharp/Aver.Framework/DrivingContext.cs
- scripting/csharp/Aver.Framework/VehicleHud.cs
- scripting/csharp/Aver.Graph.Tests/VehicleApiTests.cs
- scripting/csharp/Aver.Graph.Tests/DrivingContextTests.cs

Editor
- sandbox/src/VehicleDetailsModel.hpp
- sandbox/src/VehicleDetailsSection.cpp
- sandbox/src/VehicleEditorModel.hpp
- sandbox/src/VehicleEditor.hpp
- sandbox/src/VehicleEditor.cpp
- sandbox/src/VehicleDebugOverlay.hpp
- sandbox/src/VehicleDebugOverlay.cpp

Tests
- tests/physics/src/VehicleExtTest.cpp
- tests/physics/src/TrackedVehicleTest.cpp
- tests/vehicle/CMakeLists.txt
- tests/vehicle/src/OcVehicleTest.cpp
- tests/vehicle/src/VehicleBuildTest.cpp
- tests/vehicle.scene/CMakeLists.txt
- tests/vehicle.scene/src/VehicleSystemTest.cpp
- tests/vehicle.scene/src/VehicleFixtures.hpp
- tests/framework_vehicle/CMakeLists.txt
- tests/framework_vehicle/src/FrameworkVehicleAbiTest.cpp
- tests/editor/src/VehicleDetailsModelTest.cpp
- tests/editor/src/VehicleEditorModelTest.cpp

Template and lab content
- templates/Vehicle/Vehicle.octemplate
- templates/Vehicle/make_vehicle_template.py
- templates/Vehicle/Content/README.md
- templates/Vehicle/Content/Maps/Default.ocworld
- templates/Vehicle/Content/Vehicles/Sedan.ocvehicle
- templates/Vehicle/Content/Vehicles/Tank.ocvehicle
- templates/Vehicle/Content/Input/Vehicle.ocinput
- templates/Vehicle/Content/UI/VehicleHud.ocui
- templates/Vehicle/Content/Scripts/AN_VehicleRules.ocgraph
- templates/Vehicle/Content/Scripts/AN_VehicleController.ocgraph
- templates/Vehicle/Content/Scripts/AN_VehicleDriver.ocgraph
- templates/Vehicle/Content/Scripts/AN_VehicleHud.ocgraph
- templates/Vehicle/Content/Scripts/AN_TankTurret.ocgraph
- templates/Vehicle/Content/Meshes/SOURCES.md
- templates/Vehicle/Content/Meshes/Sedan.ocmesh
- templates/Vehicle/Content/Meshes/SedanWheel.ocmesh
- templates/Vehicle/Content/Meshes/TankHull.ocmesh
- templates/Vehicle/Content/Meshes/TankTurret.ocmesh
- templates/Vehicle/Content/Meshes/TankWheel.ocmesh
- templates/Vehicle/Content/Sounds/Engine_Idle.ocaudio
- templates/Vehicle/Content/Sounds/Engine_High.ocaudio
- templates/Vehicle/Content/Sounds/Skid.ocaudio
- templates/Vehicle/Content/Sounds/Crash.ocaudio
- templates/Vehicle/Content/Particles/TyreSmoke.ocparticle
- templates/Vehicle/Content/Particles/Sparks.ocparticle
- test-content/VehicleLab/Game.ocproject
- test-content/VehicleLab/README.md
- test-content/VehicleLab/make_vehicle_lab.py
- test-content/VehicleLab/Content/Maps/VehicleLab.ocworld
- test-content/VehicleLab/Content/Vehicles/Truck6x4.ocvehicle
- test-content/VehicleLab/Content/Vehicles/Hatchback.ocvehicle
- test-content/VehicleLab/Content/Vehicles/Tank.ocvehicle
- test-content/VehicleLab/Content/Vehicles/RiggedCar.ocvehicle
- test-content/VehicleLab/Content/Input/VehicleLab.ocinput
- test-content/VehicleLab/Content/Rigs/RiggedCar.ocrig
- test-content/VehicleLab/Content/Source/RiggedCar.gltf

Docs
- docs/VEHICLES.md

### 3.2 Modified, owned by this feature

- modules/physics/src/PhysicsVehicle.cpp
- modules/vehicle/module.json
- modules/vehicle/README.md
- modules/world/src/VehicleSystem.cpp
- modules/world/include/aver/world/VehicleSystem.hpp
- modules/world/module.json
- modules/world/CMakeLists.txt
- Runtime/src/GameLevel.cpp
- Runtime/include/aver/game/GameLevel.hpp
- scripting/csharp/Aver.Physics/Vehicle.cs
- scripting/csharp/Aver.Framework/Vehicles.cs
- scripting/csharp/Aver.Framework/VehicleGraph.cs
- scripting/csharp/Aver.Graph/GraphVehicleNodeTable.cs
- sandbox/src/GraphNodeDefsVehicle.hpp
- scripting/csharp/Aver.Graph.Tests/VehicleNodeTests.cs
- modules/anim.scene/include/aver/anim/ControlRig.hpp
- modules/anim.scene/src/ControlRig.cpp
- tests/anim.scene/src/IkRigSystemTest.cpp
- tests/physics/CMakeLists.txt
- docs/nodes/Vehicle.md

(Recycled, not deleted: the `kPresets` table and `buildVehicle` in `VehicleSystem.cpp` move to `Aver.Vehicle`; `EnsureSync` and the timer in `Vehicles.cs` are removed. No file is deleted.)

### 3.3 SHARED (another wave 10 feature or the integrator also touches these; every edit is small and additive)

- CMakeLists.txt
- cmake/modules.catalog.json
- modules/physics/CMakeLists.txt
- modules/physics/src/PhysicsInternal.hpp
- modules/physics/src/PhysicsWorld.cpp
- modules/framework/CMakeLists.txt
- modules/formats/src/OcWorld.cpp
- Runtime/CMakeLists.txt
- Runtime/include/aver/game/GameSystemsWiring.hpp
- Runtime/include/aver/game/LevelComponents.hpp
- Runtime/src/LevelComponents.cpp
- Runtime/src/GameApp.cpp
- sandbox/CMakeLists.txt
- sandbox/src/SandboxApp.hpp
- sandbox/src/SandboxApp.cpp
- sandbox/src/SandboxInit.cpp
- sandbox/src/SandboxUpdate.cpp
- sandbox/src/SandboxPlay.cpp
- sandbox/src/SandboxRender.cpp
- sandbox/src/SandboxShell.cpp
- sandbox/src/SandboxLevelEdit.cpp
- sandbox/src/SandboxComponentDetails.cpp
- sandbox/src/SandboxContentBrowser.cpp
- sandbox/src/PlacedComponentModel.hpp
- tests/abi/src/AbiEnumTest.cpp
- tests/editor/src/TemplateScaffoldTest.cpp
- tests/editor/CMakeLists.txt
- docs/ABI.md
- docs/MODULES.md
- docs/ARCHITECTURE.md
- docs/SCRIPTING_API.md
- docs/formats/FORMAT_SPECS.md
- CHANGELOG.md

| Shared file | Edit made by this feature | Who else |
|---|---|---|
| `CMakeLists.txt` | `add_subdirectory(modules/vehicle)` inside `if(AVER_MODULE_PHYSICS)` after `modules/physics` (`:234-236`); `add_subdirectory(modules/vehicle.scene)` in the scene block beside `softbody.scene` (`:309-313`); `tests/vehicle`, `tests/vehicle.scene`, `tests/framework_vehicle` | every wave 10 feature adds its own lines |
| `modules/physics/CMakeLists.txt` | `src/PhysicsVehicleExt.cpp`, `src/PhysicsVehicleTracked.cpp` | SOFTBODY_DEPTH adds sources |
| `modules/physics/src/PhysicsInternal.hpp` | `VehicleEntry` and `VehicleWheelDesc` gain the v2 fields; `vehicleStepBegin` / `vehicleStepEnd` declarations (`:160-205,275-286`) | SOFTBODY_DEPTH may add its hooks |
| `modules/physics/src/PhysicsWorld.cpp` | two calls around `system.Update` beside `softbody::onStepBegin` (`:485`) | SOFTBODY_DEPTH edits the same neighbourhood |
| `modules/framework/CMakeLists.txt` | `src/VehicleAbi.cpp` | others add their relay sources |
| `modules/formats/src/OcWorld.cpp` | `"Vehicle"` appended to the COMPONENT type list (`:173-174`) | GPU_PARTICLES, GAMEPLAY_CAMERAS append their own names |
| `Runtime/include/aver/game/GameSystemsWiring.hpp` | register, `tickVehiclesPre`, `tickVehiclesPost`, reset, the sink wiring of 2.8 | all four features add blocks |
| `Runtime/include/aver/game/LevelComponents.hpp`, `Runtime/src/LevelComponents.cpp` | enum value, `applyVehicle`, capture | same |
| `Runtime/src/GameApp.cpp`, `Runtime/CMakeLists.txt` | the two tick calls beside the traffic ones (`:579-584`), seed (`:2026`), link `Aver.Vehicle.Scene` | cameras, particles, soft body |
| `sandbox/src/SandboxUpdate.cpp`, `SandboxPlay.cpp`, `SandboxInit.cpp`, `SandboxLevelEdit.cpp` | the Play ticks (`:766-778`), seed (`:157`), source and sink installation (`:330`), the mid-Play save set (`:632`) | same |
| `sandbox/src/SandboxApp.hpp`, `SandboxApp.cpp`, `SandboxRender.cpp`, `SandboxShell.cpp`, `sandbox/CMakeLists.txt` | overlay options, frame and line-mesh members; overlay collect and draw; `Show > Vehicles`; new sources and link | soft body, cameras |
| `sandbox/src/SandboxComponentDetails.cpp`, `SandboxContentBrowser.cpp`, `PlacedComponentModel.hpp` | one dispatch block; one create-asset entry and type row; one `PlacedKind` and the `AttachContext` fields | soft body, particles, cameras |
| `tests/abi/src/AbiEnumTest.cpp` | `physics_vehicle_ext_abi.h` added to the physics `kAbis` row (`:560-573`) with the `AVER_PHYS_V*` index constants | soft body |
| `docs/ABI.md`, `docs/MODULES.md`, `docs/ARCHITECTURE.md`, `docs/SCRIPTING_API.md`, `docs/formats/FORMAT_SPECS.md`, `CHANGELOG.md` | rows and sections named in 2.x, written by the integrator | every feature |

Not touched: `scene_abi.h`, `Components.hpp`, `framework_abi.h`, `modules/rhi*`, every shader, `modules/render*`, `modules/ragdoll`, `modules/netvehicle`, `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs`, `Program.cs`.

---

## 4. Phased implementation

Each phase ends with its tests green and the tree buildable. Sizes: A L, B M, C M, D XL, E M, F L, G M. Overall **XL**. Never rebuild over the owner's running editor; headed runs are the owner's. The implementing agents use Sonnet workflows with the fewest stages.

**Phase A: physics ABI v2, wheeled (L).** `physics_vehicle_ext_abi.h`, `VehicleEntry`/`VehicleWheelDesc` fields, `finish` consuming them (engine curve and block, gearbox, axles, differentials, centre differential, wheel inertia, damping, curves, kept wheel-settings objects), `_set_chassis_boxes`, the two step hooks (aids, aero, velocity cache, slip, events), `_state`, `_wheel_state`, events, `_tune`, `_set_gear`, `_shift`, `_flip`, `_set_collision_period`. C# `Native.VehicleExt.cs`, `VehicleExt.cs`, `Vehicle.cs` made partial. `AbiEnumTest` row. `VehicleExtTest`. Exit: `VehicleTest` unchanged and green; `VehicleExtTest` green including "a vehicle that sets nothing is field-for-field v1"; `AbiEnumTest` green.

**Phase B: tracked and compound chassis (M).** `PhysicsVehicleTracked.cpp`, `_set_kind`, `_set_track`, the track-ratio steering, track telemetry. `TrackedVehicleTest`. Exit: green.

**Phase C: `Aver.Vehicle` (M).** Module, manifest, `VehicleSpec`, `parse/write/validate`, `specFromBounds` (the traffic numbers moved), `buildVehicle`. `world::VehicleSystem` switches to it and accepts `*.ocvehicle`; `GameLevel::beginVehicles` passes the spec source. `tests/vehicle`. Exit: `OcVehicleTest`, `VehicleBuildTest`, the existing `VehicleLaneTest` and the whole `tests/world` green; a recorded 600-step traffic run before and after the move gives identical poses (the comparison is a test).

**Phase D: `Aver.Vehicle.Scene`, relay, hosts (XL).** Component and system, resident build, input and action reading, pose write, wheel visuals (children, then bones with the rig-cap change), telemetry, events, driver snap, audio port, effect, crash, camera and notify sinks, relay ABI and provider, level record, both hosts, `seedMovable`, mid-Play save set. `VehicleSystemTest`, `FrameworkVehicleAbiTest`. Exit: both green; `IkRigSystemTest` at 32; a headed drive in the editor and in the packaged runtime (section 6).

**Phase E: C#, nodes, HUD helper (M).** `EntityVehicle.cs`, `VehicleTelemetry.cs`, `VehicleNative.cs`, `DrivingContext`, `VehicleHud`, `Vehicles.Spawn`, `CarSpec.ToOcVehicle`, `CreateCar` re-route, timer removal, 14 rows plus palette plus tests, `--write` for `docs/nodes/Vehicle.md`. Exit: `VehicleNodeTests`, `VehicleApiTests`, `DrivingContextTests`, `NodeCatalogCheck`, `CsharpApiIndexCheck` green.

**Phase F: editor (L).** Add Component, Details model and section, `.ocvehicle` tab, overlay, Content Browser entry. Exit: `VehicleDetailsModelTest`, `VehicleEditorModelTest` green; the Debug sandbox object section count recorded.

**Phase G: template, lab, docs (M).** Generators, committed content, `docs/VEHICLES.md`, the doc edits of 3.3, `module-catalog.py --write`, `module-guard-audit.py`, `docs-check.py`. Exit: `TemplateScaffoldTest` finds and scaffolds `Vehicle` and its start map loads headless; full `ctest` green.

---

## 5. Tests

Everything below runs without a GPU or a window. `*Test` executables are picked up by the root walk; each phase adds only an `add_executable`.

**`tests/physics` (link `Aver.Physics Aver.Core`)**
- `VehicleExtTest`: (1) *v1 identity*: the `VehicleTest` car built through v1 and the same car built through v2 with every block at its default drive 300 steps to poses within 1e-4 cm. (2) *Torque curve*: a flat low curve accelerates slower than the default over 3 s; a curve peaking at 0.3 reaches its first shift earlier. (3) *Gearbox*: custom ratios change the speed at the first upshift; reverse magnitudes are negated; manual mode holds a gear through the rev limit and `_set_gear`/`_shift` change it with the clutch timing. (4) *Differentials*: a rear-driven car with one rear wheel on a 0.05-friction strip: open diff covers less distance in 4 s than limited slip, which covers less than locked (1.01); an AWD car with `share` on the front axle moves with its rear wheels lifted off (airborne test body). (5) *Anti-roll stiffness* 0 versus 8000: lower body roll in a steady turn. (6) *Tyre curves and `GRIP_SCALE`*: a flatter curve skids earlier; scale 0.5 lowers peak lateral acceleration to within 15% of half. (7) *Aids*: `ABS` lowers the peak locking slip in a full brake on a 0.3-friction surface; `TCS` lowers the peak driven-wheel spin from a standing start on ice; steering applied rate matches `STEER_IN` within one step; at 3000 cm/s the turn rate is the speed curve's fraction of the 300 cm/s one; reverse-on-brake brakes then reverses; hold brake prevents creep. (8) *Aero*: drag lowers 8 s top speed; downforce raises wheel load (`LOAD_N` sum) at speed. (9) *Telemetry*: `RPM` stays in `[min, max]`; the four `LOAD_N` sum within 15% of `m g` at rest; `COMPRESSION_CM` positive; `CONTACT_ENTITY` equals the stamped entity; `SPEED_CMS` equals `_velocity`'s length. (10) *Events*: hard braking on ice gives `SKID_BEGIN`; a 2 m drop gives `LAND` with downward speed within 20% of `sqrt(2 g h)`; a wall hit gives one `IMPACT` with closing speed within 20% of the approach speed; gear changes give `SHIFT`. (11) *Chassis boxes*: a two-box truck's rest height and centre of mass match the request. (12) *Lifetime*: v2 calls on dead handles and before/after `_finish` refuse as documented; `_flip` after a roll leaves the car upright and still; the collision period halves the cast count (observed through `_wheel_contact` still working at 2). (13) Same-input-twice bit equality.
- `TrackedVehicleTest`: a tracked car drives forward and holds a straight line within 5 degrees over 5 s; a pivot turns in place (yaw rate above 30 degrees/s while the speed stays under 60 cm/s); steering while moving curves toward the right sign; `finish` is refused without both tracks, with a driven wheel not in its list, and axle calls are refused; track speeds in `VS` match the driven wheel's rim speed.

**`tests/vehicle` (link `Aver.Vehicle Aver.Physics Aver.Formats Aver.Core`)**
- `OcVehicleTest`: parse the sedan and tank files of the template; write then parse is byte-identical for canonical input; every `validate` refusal (no wheels, undefined class, descending curve, tracked missing a track, more than 16 wheels, non-finite numbers); unknown records and keys ignored, newer version refused; `specFromBounds` for the five names equals the old `kPresets` row values for mass, radius clamp, torque, steer and spring Hz for three mesh sizes (golden numbers copied from `VehicleSystem.cpp:312-318,404-453` into the test); `tank` derivation is tracked with 5 wheels a side.
- `VehicleBuildTest`: every built-in preset and the template specs build, settle on their wheels, and drive; a `specFromBounds("car", size)` vehicle matches a v1-built car with the old `buildVehicle` arguments (poses within 1e-4 cm after 300 steps); a 6x4 spec has 6 wheels, 3 axles, and 2 differentials.

**`tests/vehicle.scene` (link `Aver.Vehicle.Scene Aver.Vehicle Aver.Physics Aver.Scene Aver.Core`; meshes, actions, audio and specs injected)**
- Component: registered size 96, zero-filled defaults valid, `attach` writes defaults, OUT fields read-only through the generic field ABI.
- Build and pose: an entity with `CVehicle` and a mesh gets a vehicle at its pose; the entity pose follows the chassis after 120 steps; a destroyed entity destroys its vehicle; scale not 1 is refused with one warning; a parked fleet writes no `CLocal`; at most 2 builds per frame.
- Input: fields drive the car; with a driver and an injected action source `Drive.Throttle` accelerates, `Drive.Brake` stops, `Drive.ShiftUp` shifts in manual only, `Drive.Reset` flips, `Drive.Exit` exits; `kVehicleNoActions` ignores the actions.
- Visuals: child entities named in the spec receive the wheel poses each frame (steer sign +Y for right input, spin sign forward); a missing child warns once; the rig route sets 12 inputs on one entity (the raised cap) and the 33rd name is refused; `entities()` lists vehicle and wheel children.
- Driving: `enter`/`exit` set `driver`, hide and restore the driver mesh, set and restore character gravity factor and strength (with a physics character); a second `enter` is refused; the camera sink gets `Enter` then `Exit` with the vehicle, driver, chassis body handle and the spec's rig points, and `Next` on `Drive.CameraNext`.
- Telemetry and events: OUT fields match `Vehicle::TryGetState`; events map to sinks (impact gives notify `VehicleCrash`, crash sink with energy `0.5 m v^2` within 5%, effect sink on skid with the accumulator's count independent of frame rate); health falls with crashes and `destroyed` cuts the engine; repair restores.
- Audio (fake port): voices start for the nearest 8 audible vehicles, pitch rises with RPM, stop on `clear()` and out of range.
- LOD: far driverless vehicles get collision period 2 then 4; a driven one stays 1.
- **Replay equality**: two fresh systems run the same ordered script for 600 ticks; poses, telemetry floats, events and `seq` are bit identical.
- Traffic coexistence: an entity with `CVehicle` is skipped by `beginVehicles`; a traffic car and a driven car share the world.

**`tests/framework_vehicle`**: version; every function returns 0 and writes nothing with no provider; with a fake provider each op marshals its arguments and `text` correctly; inline `OCVEHICLE` text is passed through.

**`tests/editor`**: `VehicleDetailsModelTest` (preset rows, record keys and canonical omission, `canAttach` rules for the three conflicts, Create-asset path, live-table formatting, km/h conversion); `VehicleEditorModelTest` (curve point edit keeps x ascending and clamps, gear table top speeds, drive-layout label for FWD / RWD / AWD / 6x4, validation list equals `vehicle::validate`, undo bookkeeping). `TemplateScaffoldTest` gains: `Vehicle` is found, scaffolds, and its start map parses with two `COMPONENT Vehicle` records.

**`tests/anim.scene`**: `IkRigSystemTest` updated to "32 distinct inputs fit, the 33rd is refused".

**`tests/abi`**: `AbiEnumTest` pairs `physics_vehicle_ext_abi.h` with `Native.VehicleExt.cs` (names and parameter names) and the `AVER_PHYS_V*` index constants with their C# mirrors.

**C# (`Aver.Graph.Tests`, Windows only, ctest `GraphManagedTests`)**: `VehicleNodeTests` (the 20 rows through `NodeRowKit.CheckRows`, `CheckDefaultPins`, `CheckIlValid`; the six old rows still exactly as listed; exec rows refused by the pure compiler; `an_vehicletelemetry` and `an_vehiclewheelstate` work as pure reads; the methods return false rather than throw with no provider); `VehicleApiTests` (`CarSpec.Default.ToOcVehicle()` parses back to the four-wheel plan `PlanWheels` computes and has the v1 numbers; `TuneVehicle` unknown key false; `VehicleHud.FormatSpeed` golden values); `DrivingContextTests` (action names are exactly the `Drive.*` set, default bindings present, rebind round-trips through `SaveBindings`). Python: `NodeCatalogCheck` (table to palette parity for the 14 new rows) and `CsharpApiIndexCheck`.

---

## 6. Headed manual checks for the owner

Use `test-content/VehicleLab` and a project from the `Vehicle` template. Editor first, then the packaged runtime, then `--backend vulkan`. The GPU must be idle (the owner plays other games), screenshots at 1440p with a ping for remote runs.

1. **New Project > Vehicle** opens the arena; Play: a walking pawn; walk to the sedan, press E: the HUD appears, the camera is behind the car, the driver is gone from view.
2. **Drive the sedan.** W / S / A / D: accelerates, shifts up through the gears audibly (RPM falls on each shift), steers smoothly (no snap), S while moving brakes and at standstill reverses, Space slides the rear. Release everything: the car stops creeping.
3. **Engine sound.** Idle tone at standstill; pitch and crossfade rise with RPM; lifting the throttle drops the volume; gear shift click.
4. **Surfaces.** Drive onto the ice strip: wheels slip, skid sound and smoke (once GPU_PARTICLES lands), ABS lamp flickers under braking, TCS lamp (turn `tcs` on with the Details toggle) holds the wheelspin down. Gravel: lower grip. Wheel meshes follow the suspension over the ramp and spin the right way; steered wheels turn the right way.
5. **Tuning live.** Details > sliders: power x2, grip x0.5, `manual` on and E / Q shifting; the telemetry table (slip, load, compression) changes plausibly; the editor `.ocvehicle` tab edits the torque curve and the next spawn feels it.
6. **Differentials (VehicleLab ice-split lane).** Open diff: the car barely moves; limited slip: it crawls out; locked: it drives out. The 6x4 truck pulls away with both rear axles driven and the centre differential in each mode.
7. **Tank.** Enter the tank: throttle drives both tracks, A / D curves, at standstill A / D pivots in place, brake stops; track wheels turn; the turret follows the mouse; belt scroll values change in the Details table.
8. **Crash.** Hit the wall hard: impact sound, sparks (with GPU_PARTICLES), the `VehicleCrash` event fires a graph print, health falls, the shell panel dents and a loose panel comes off once SOFTBODY_DEPTH lands; health 0 cuts the engine; Repair restores it.
9. **Flip.** Roll the car (ramp at speed): the `FLIPPED` event fires after 3 s and `Drive.Reset` puts it upright; it never lands on its roof past the 60 degree limit.
10. **Skeletal wheels (if the lab glTF imported).** The rigged car's wheel bones follow the suspension; a six-wheel truck proves the 12-input cap.
11. **Debug overlay.** `Show > Vehicles`: suspension lines go green to red as the car loads a corner, force arrows grow under braking, slip rings go hot when skidding, the label matches the HUD.
12. **Backends and hosts.** Repeat 1, 2, 4, 8 on `--backend vulkan` and in the packaged `AverEngineRuntime.exe` (Content cooked: the `.ocvehicle`, `.ocinput`, `.ocui` and sounds are present; the vehicle draws, drives, sounds and the HUD works).
13. **Traffic.** A level with `vehicle car` tokens and an `.oclanes` still drives its cars as before; change one token to `vehicle Vehicles/Bus.ocvehicle` and it drives its lane as that bus.
14. **Cost.** With GPU idle, 40 parked vehicles and 8 driven AI cars on the profiler's Vehicles row: record milliseconds per frame with collision period 1 and with the distance LOD on; report the numbers (none exist today).
15. **Save mid-Play.** Autosave while driving: reload the level, the vehicle is where it was placed, not where it drove to.
16. Full `ctest`, the graph suite via `dotnet run`, and open `docs/nodes/Vehicle.md` to confirm 20 rows documented without hand edits.

---

## 7. Open questions (each with the default chosen; proceed unless told otherwise)

1. **Hard-locked differential.** Jolt has none; `Locked` is a limited-slip ratio of 1.01. Default: ship as stated.
2. **Ackermann steering.** Every steered wheel turns by the same angle. Default: not in v1; a per-wheel toe-out factor is the follow-up.
3. **Parking the driver.** The driver is hidden and snapped to the seat every frame with character gravity factor 0 and strength 0. Default: that. If a `CharacterVirtual` inside the chassis jitters in the headed pass, the fallback is parking the character below the world at gravity 0 and teleporting it out on exit (no `physics_character_abi.h` change).
4. **One `.ocinput` context.** A project has one scheme; the template binds `Drive.*` always and the vehicle system reads them only while a driver exists. Default: that plus the public `DrivingContext` for C# projects. A multi-context `.ocinput` (push on enter, pop on exit) changes the format and `InputScheme.cs`; ask the owner before doing it.
5. **`CreateCar` needs the vehicle provider.** A physics-only host with no scene answers `Car.None`. Default: accept (no second code path); the headless C# tests already treat that as "library absent".
6. **Rig-input cap 8 to 32.** Needed for six-wheel and tracked skeletal vehicles. Default: raise it and update the one test. The alternative, a dedicated pose modifier for wheels, would be a second modifier chain.
7. **Surface types.** Surface is the contact body's friction and entity; no physical-material table. Default: none. A material table is cross-cutting (landscape layers, audio footsteps) and is not VEHICLES' to add.
8. **Crash damage curve and thresholds** (`crash`, `energy`, the health formula). Default: the UNVERIFIED shape in 2.6.3, tuned in the headed pass.
9. **Tank scope.** Tracks, pivot, speed telemetry; no belt mesh, no turret physics, no recoil. Default: that.
10. **Engine audio technique.** Loop cross-fade and pitch by RPM only. Granular synthesis from recorded sweeps is deliberately not used (patent-heavy area); a clean-room synthesiser would be a separate feature.
11. **Stability control** is not implemented (live automotive patents on specific control laws). If wanted later it is designed clean-room from published vehicle dynamics (single-track model), not from a vendor description.
12. **Template map format.** `.ocworld`, because components cannot live in `.ocmap`. Default: that; `TemplateScaffoldTest` proves it loads.
13. **Packaging.** No file-type list of cooked assets was found (`.ocsoft` has none either); the headed packaged-runtime check (item 12) confirms `.ocvehicle` and the rest ship. If not, the allowlist line is one added pattern.
14. **`Aver.NetVehicle` stays a skeleton.** Its replication needs the fixed-step input stream and the `seq` events of 2.15, both provided; nothing more is promised.
15. **Patent notes, collected.** ABS, traction control and rate-limited steering here are the generic, long-public techniques (slip-threshold release/recover; throttle cut), implemented from first principles with Jolt's own slip numbers; no vendor control law is copied. Tyre curves are piecewise-linear like Jolt's, not a licensed tyre model. Engine sound: see 10. Stability control: see 11. Nothing in this feature needs a third-party library.


---

## 8. As built: deviations and notes (written by the implementing agent; nothing was compiled or run)

The implementation follows this plan except where listed. Each line says what the plan said, what the code does, and why.

**Physics (phases A and B)**

1. **Differential default.** The plan's table gave `DIFF_MODE` 0 (open) as the default. The code defaults to **1 (limited slip at 1.4)**, which is Jolt's own and what v1 builds, so a vehicle that never calls `set_axle` is identical to a v1 one. A spec spells `diff open` when it means open (the built-in sedan does). `AxleDef` in `Aver.Vehicle` defaults the same way.
2. **Impact detection.** The plan cached velocities in `vehicleStepBegin`. The code notes impacts in the contact listener (`vehicleOnContact`, under `eventMutex`) from the contact points' velocities, which is more accurate, and merges and sorts them in `vehicleStepEnd`.
3. **Chassis boxes.** `set_chassis_boxes` rebuilds the body under the same handle instead of calling `SetShape`.
4. **`SetEngine` inertia** defaults to 0 (auto, scaled to the torque as Jolt's default engine is), not 0.5.
5. **A tracked vehicle needs every wheel on exactly one track.** `validate` and the builder refuse otherwise.

**`Aver.Vehicle.Scene` (phase D)**

6. **The relay applies calls at once**, not "at the next tick": the physics world is idle between steps, and a graph that spawns and then reads telemetry in one chain gets an answer.
7. **Camera rig entities are made by the vehicle system**, as children of the vehicle; the camera sink only attaches rigs to them and points the view. No camera maths lives in the module.
8. **No built-in action source or audio port.** The module links neither the framework nor audio; the host installs the action source (by name, over `aver_fw_action_*`) and the audio port (which also loads sounds by path).
9. **The bone route is a `RigInputFn` sink**, not a link to `Anim.Scene`; `optionalDependsOn` Anim.Scene is therefore dropped from `module.json`.
10. **`damage.energy`** in the spec scales the energy handed to the crash sink.
11. **Traffic.** `GameLevel::beginVehicles` skips entities that carry a `CVehicle`; `world::VehicleSystem` refuses a tracked spec (one log line).
12. **`VehiclePhysicsSystem::Impl` is defined in the public header** because the own-file list had no internal header for the module's four translation units to share. It is not an API.

**C#, Aver Node, editor (phases E and F)**

13. **`Car` goes through the entity.** The 60 Hz timer in `Vehicles.cs` is gone (the system writes poses in C++); `EnsureSync`/`SyncEntities` remain as no-ops for source compatibility. `Car.Destroy` destroys the entity and then the vehicle.
14. **`Entity.HasComponent(string)` P/Invokes native**, so the vehicle members catch `DllNotFoundException` and `EntryPointNotFoundException` and answer false: the bare-process graph tests exercise them.
15. **Editor preview.** The `.ocvehicle` tab publishes a copy of the spec being edited to `vehicleSpecPreview()`; the host passes the selected entity's pose to `vehicleDebugCollect` to draw it. The plan said "over the selected entity using the overlay's line path"; this is that.
16. **Preset combo.** Choosing a preset in the Details section drops an assigned asset (the preset is the source); the combo does not sit disabled beside an asset.
17. **A Destructible cannot be added to a vehicle** (and the reverse): one body per entity root. The plan listed three conflicts; this is a fourth.

**Content (phase G)**

18. **Engine loops are WAV, not `.ocaudio`.** `aver_audio_load` imports `.wav`, and a WAV needs no cooking step; the specs, the template and the lab name `Sounds/*.wav`.
19. **The graph HUD is bars.** Graphs have no number-to-text node, so `UI/VehicleHud.ocui` shows speed, revs, gear and health as progress bars and ABS/TCS as lamps that appear while the aid works. `VehicleHud.FormatSpeed` / `FormatGear` are for C# projects.
20. **No ice or gravel strip in the template or by file in the lab.** The level format has no per-placement friction and no `COMPONENT RigidBody`. The lab places the `IceLeft` and `TarmacRight` strips and its README shows the manual step (Add Component > Rigid Body, friction 0.05).
21. **Graph structure.** `AN_VehicleHud` and `AN_TankTurret` are placed in the map as `class` placements and find the vehicles by name; the pawn tests "somebody is driving" with `driver != 0` (an entity handle does not survive a float, so `driver == me` cannot be built from the float `Equal`). The pawn is a first-person capsule with no mesh.
22. **`INPUTSCHEME` in `.octemplate`.** `Vehicle.octemplate` carries an `INPUTSCHEME` key so a new project gets `INPUT.SCHEME Input/Vehicle.ocinput`. The scaffolder needs a few lines to read it (the integrator's patch); an editor that ignores the key leaves Project Settings > Input Scheme to be set by hand, which the template's README says.
23. **Lab meshes are cooked and committed** (`Content/Meshes`), unlike SoftBodyLab, which asks the owner to import: the lab works on open. `make_vehicle_lab.py` rewrites both the sources and the cooked output. The importer accepted the generated skinned glTF (it re-derives the inverse bind matrices from the rest pose and says so), and the cooked skeleton's bones are at the hubs in centimetres.
24. **Two helper modules** (`templates/Vehicle/vehicle_geo.py`, `graphkit.py`) are shared by both generators; they are not copied into projects (only `Content/` is).

**Not verified.** Nothing was built, run, or looked at: the C++ and C# compile risk, every numeric margin in the new tests, whether the graphs compile in the editor (the generator checks every link against the node reference), and how the meshes look. The headed checks of section 6 are still the owner's.
