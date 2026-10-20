# Character, ragdoll and vehicle depth: the rest of Jolt's actors (wave 11)

Design contract for the implementing agent. Owner decision (wave 11): integrate the rest of Jolt Physics, everything in the survey except Jolt's ObjectStream and PhysicsScene files, with large-world coordinates and Jolt 5.6 Hair in scope for the wave (they belong to other areas; section 7 says how this one stays out of their way). This area is the three **actor** families: the walking character (`CharacterVirtual` and the rigid `Character`), the ragdoll (`Ragdoll`, `SkeletonMapper`) and the vehicle (`MotorcycleController`, the wheel collision testers, the wheel and constraint knobs Aver never reached). Nothing here is built or run. Rules in force: upgrade the existing systems and ABIs, never a parallel one; no new third-party code (everything below is Jolt source that is already vendored and already in `Jolt.cmake`); lean comments; never delete files; additive ABI only; D3D12 and Vulkan parity (this area adds **no GPU work**: the one overlay is `rhi::LineVertex` lines both backends draw); headless ctest where possible; headed manual runs by the owner; never rebuild over the owner's running editor. Format and depth follow `docs/plans/wave10/VEHICLES.md`.

Every "exists today" claim below was read in the tree on 2026-10-19. Line numbers are last-known-good pointers; grep the symbol when one is off. **UNVERIFIED** marks anything reasoned from reading but not confirmed by running it; every tuning number in this document is an UNVERIFIED starting value until the headed pass in section 6.

**Scope.** Character: orientation and gravity, any shape, all of CharacterVirtual's settings, a script-safe contact listener, ground detail, a locomotion motor, an enable switch, the rigid-body NPC. Ragdoll: per-part shapes, mass, friction, joint kinds, motors, selective self-collision, partial (per-group) modes, extra loop constraints, Stabilize, ragdoll-wide velocity, bounds, root and collision group, the SkeletonMapper. Vehicle: the motorcycle, wheel casts, springs, wheel geometry, gravity override, tyre model, more wheels, solver steps. **Not here:** a balance or behaviour-synthesis controller for ragdolls (live patents, section 7), character AI, animation retargeting, a vehicle damage model (VEHICLES wave 10 owns it), Jolt's debug renderer and profiler (another area; the overlays here draw their own lines).

---

## 0. Decisions in one screen

1. **Three new physics headers, all additive, each with its own version define and function.** `physics_character_ext_abi.h` (`AVER_PHYS_CHARACTER_ABI_VERSION 2`), `physics_ragdoll_ext_abi.h` (`AVER_PHYS_RAGDOLL_ABI_VERSION 2`), `physics_vehicle_adv_abi.h` (`AVER_PHYS_VEHICLE_ADV_ABI_VERSION 1`). The v1 character and ragdoll functions and the 14 v1 and 25 v2 vehicle functions are untouched; `aver_phys_vehicle_abi_version()` stays 2 (a test pins it, `VehicleExtTest.cpp:163`). The one edit to an existing header is a define: `AVER_PHYS_VEH_KIND_MOTORCYCLE 2` in `physics_vehicle_ext_abi.h`.
2. **A character that never calls anything new is bit-identical to today** (one character; with several, only the order the step visits them changes, section 2.2.9). The fixed-step character block moves out of `PhysicsWorld.cpp` (`:404-464`) into `PhysicsCharacterStep.cpp` with its arithmetic unchanged; every new behaviour is gated by a setting whose default is the current behaviour. The first test is a bit-equality run against the unmodified path.
3. **Up, rotation and gravity become per-character state.** `set_up`, `set_rotation`, `align_to_up`, and a gravity mode (world, along `-up`, explicit vector). Planet walking and wall walking need only these. The stair vectors, the supporting-volume plane and the carried-velocity projection already read `ch->GetUp()` or are recomputed from it (section 2.2.2).
4. **Any shape, with an offset.** Capsule, sphere, box, cylinder, tapered capsule through `create_ex` and `set_shape_ex`; `set_shape_offset`. The v1 capsule functions keep working and keep refusing a non-capsule (`shape` getter).
5. **CharacterVirtual's settings are one block, some live and some creation-only.** Padding, predictive distance, back-face mode, the two iteration caps, minimum time and collision tolerance have **no setter in Jolt**; changing one rebuilds the character under the same handle (the idiom `aver_phys_character_set_inner_body` already uses, `PhysicsCharacter.cpp:396-412`). Penetration recovery, max hits, hit reduction, the supporting volume and the layers are live.
6. **The contact listener is data, not code.** One native `CharacterContactListener` is installed on every character. It applies **declarative rules** (a per-object character profile: traction, slide, belt velocity, push flags, ghost) and appends to a polled event queue. No managed or script call can run inside the step: the callbacks run on the thread that calls `ExtendedUpdate` (the caller of `aver_phys_step`, **not** a Jolt worker; the survey was wrong about that), and a script re-entering the physics ABI mid-step would deadlock or corrupt, so the rule is the same as for worker threads. Scripts set profiles between steps and drain events after them.
7. **A locomotion motor makes "ice" and "mud" real.** Optional (default off): horizontal velocity chases a wish at ground or air acceleration scaled by the traction of what the character stands on. Jolt gives no ground friction to a virtual character; its own header says to emulate ice by adjusting the velocity per frame, and this does that natively, frame-rate independently.
8. **`set_enabled(ch, 0)` parks a character** (not stepped, out of the character-vs-character set, inner body out of the broad phase). It replaces the "gravity 0 and strength 0" hack `VehicleDriver.cpp:232-240` uses and gives the ragdoll hand-off (a dying character) a clean off switch.
9. **The rigid-body `JPH::Character` is an option, `aver_phys_npc_*`.** For crowds of simple NPCs that must be simulated bodies. The NPC handle **is** its body handle, so every body function works on it.
10. **Ragdolls get a builder, like vehicles.** `aver_phys_ragdoll_builder_begin / _set_bone / _set_options / _add_constraint / _finish`. `aver_phys_ragdoll_create` is re-implemented on the same internal build and its behaviour does not change (a test compares).
11. **Per-bone and ragdoll-wide blocks.** Per bone: shape (capsule, box, cylinder, tapered), mass, friction, restitution, damping, joint kind (cone, pyramid, hinge, fixed, point), hinge axis and range, motor frequency, damping and torque scales, pose group. Ragdoll-wide: Stabilize, self-collision, collision group, the motor base numbers (today's constants become data), CCD, SkeletonMapper.
12. **Stabilize is opt-in** and documented as what it does: it conserves total mass and **flattens parent-child mass ratios to 0.8..1.2** (Jolt, `Ragdoll.cpp:51-131`). Off by default, so the masses a caller asks for are the masses it gets (the reason `RAGDOLL.md` records for not applying it today).
13. **Self-collision is selective.** Today every part pair is off (`PhysicsRagdoll.cpp:386-388`). The new mode uses Jolt's `DisableParentChildCollisions(pose, minSeparation)` (parent-child and pairs that overlap in the creation pose are off, the rest collide) and a runtime per-pair toggle on the shared `GroupFilterTable`.
14. **Partial ragdolls are per-part modes.** Parts carry a pose group (0..31); `set_part_mode(group, mode)` makes the upper body powered and the legs kinematic. Jolt's `DriveToPoseUsingMotors` asserts on any joint that is not swing-twist or hinge (`Ragdoll.cpp:677`), so motor driving is done here per joint, never through that helper.
15. **The SkeletonMapper improves in-between bones.** The pose arrays are already per animation bone, so the mapper is not needed to reach the animation skeleton; it replaces "a bone without a part rides rigidly on its ancestor" with Jolt's chain reorientation and optional translation lock (a spine with three helper bones bends instead of kinking). Off by default.
16. **Vehicles: one more kind, one more header.** `KIND motorcycle` (Jolt `MotorcycleController`) is a third value of `aver_phys_vehicle_set_kind`; its lean block, plus wheel geometry (camber, toe, caster, fork rake), wheel springs (frequency, stiffness, mass-normalised; preload; force point), wheel casts (cylinder, sphere, ray; layer), gravity override, live pitch/roll limit, solver steps, per-wheel grip, a tyre model and a wheel cap up to 64 live in `physics_vehicle_adv_abi.h`.
17. **The tyre-impulse hook is native and data-driven.** Jolt's `TireMaxImpulseCallback` runs on worker threads, so it never calls out. A built-in model (load sensitivity, friction ellipse, per-wheel grip scalars the script writes between steps) is the whole of it.
18. **`.ocvehicle` stays version 1.** New records (`MOTO`, `CAST`, `SOLVER`, `TYREMODEL`) and new keys on `WHEELCLASS` and `WHEEL` are additive; `KIND motorcycle` is rejected by older parsers with their existing "not wheeled or tracked" error, which is the honest outcome. A sample bike ships in the Vehicle template.
19. **Three Aver Node families, written to the wave 9 C contract:** `Character` (10 rows), `Ragdoll` (10 rows), `VehicleAdv` (5 rows). No existing table, `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs` or `Program.cs` is edited.
20. **Script access to the ragdoll goes through a new relay**, `framework_ragdoll_ext_abi.h` (provider + one generic `aver_fw_ragdoll_ext_op`), and to the vehicle through one generic `aver_fw_vehicle_op` in `framework_vehicle_adv_abi.h`. Characters need no relay: `Aver.Framework` already references `Aver.Physics`.
21. **Events leave through the existing named-event seam.** A new `Runtime/include/aver/game/CharacterWiring.hpp`, called from `tickGameplayGroups`, drains the character event queue after the physics step and posts `CharacterLanded`, `CharacterLeftGround`, `CharacterSliding`, `CharacterHeadBump` with the entity as sender. Numbers are read from the ground node, not carried in the event.
22. **Editor:** `Show > Physics Actors` (characters, ragdolls) and lean and caster lines in `Show > Vehicles`; the `.ocvehicle` tab gains the motorcycle, cast, spring and geometry fields; `AverCharacter`'s new fields appear in Details through the existing field reflection. No new component.
23. **Content:** a motorcycle in `templates/Vehicle`, `test-content/CharacterLab` (conveyor, ice, stairs, a small planet, a crouch bar, a ragdoll dummy), a bike in `test-content/VehicleLab`.
24. **Size XL, eight phases** (section 4). A (character core), C (ragdoll ABI) and D (vehicle ABI and the motorcycle) carry the risk; G (C# and nodes) is the widest.

---

## 1. What exists

The survey is right on every point it makes. Corrections and nuances found while reading are in 1.5.

### 1.1 Character

| Fact | Where |
|---|---|
| `aver_phys_character_create(radius, height, x, y, z)` builds a capsule `CharacterVirtualSettings` with: up = engine +Z through `toJoltUnit`, max slope 50 degrees, a supporting-volume plane `Plane(up, -radius)`, inner body (default on) on `Layers::MOVING`, edge removal (default on), **identity rotation**, every other setting at Jolt's default. Position is the capsule **centre** | `PhysicsWorld.cpp:673-710` (`:689` up, `:692` plane, `:702` layer, `:705` rotation) |
| No `SetListener`, no `SetShapeOffset`, no `SetUp` / `SetRotation`, no `GetGroundMaterial` / `GetGroundSubShapeID` / `GetGroundUserData`, no `CancelVelocityTowardsSteepSlopes` / `IsSlopeTooSteep`, no use of the rigid `JPH::Character` anywhere in `modules/physics/src` | grep, all negative |
| The fixed-step block: gravity = `system.GetGravity() * factor`, supported characters have the downward part cancelled and `UpdateGroundVelocity` carried, airborne ones keep the horizontal carry, then `ExtendedUpdate(dt, gravity, us, MOVING filters, characterBodyFilter(), ...)`. Movement always uses the `MOVING` layer filters | `PhysicsWorld.cpp:404-464` |
| `ExtendedUpdateSettings` is filled by `applyCharacterStairSettings`: only `mWalkStairsStepUp` and `mStickToFloorStepDown`, along **engine +Z** (not `GetUp()`). `mWalkStairsMinStepForward`, `mWalkStairsStepForwardTest`, `mWalkStairsCosAngleForwardContact` and `mWalkStairsStepDownExtra` stay at Jolt's defaults | `PhysicsCharacter.cpp:522-531` |
| Shape: `set_shape` swaps a capsule (checks the new shape against the world, then `SetInnerBodyShape`, then recomputes the supporting plane); the `shape` getter and `can_set_shape` refuse anything that is not a capsule | `PhysicsCharacter.cpp:270-325,416-450`; header `:128,:135,:207` |
| Ground: `ground_state` (4-way), `ground_normal`, `ground_position`, `ground_body` (two-table lookup, falls back to the contact list for a character), `ground_velocity`, `inherited_velocity` | `PhysicsCharacter.cpp:193-264`; header `:74-114` |
| Contacts: `contact_count`, `contact_get` (body handle, point, normal) over `touching()` | `PhysicsCharacter.cpp:100-105,454-477` |
| Rebuild idiom: `set_inner_body` copies `GetCharacterVirtualSettings()`, edits one field, builds a new `CharacterVirtual` under the same handle, copies position, rotation, user data and velocity, re-registers, `RefreshContacts` | `PhysicsCharacter.cpp:391-412` |
| Per-character side state is file-local (`g_stairSettings`, the character-vs-character list `g_cvc`, the inner-body id set) and cleared by `clearCharacterStairSettings` from `aver_phys_shutdown`; `World` holds `characterCarry` and `characterGravity` (a factor only) | `PhysicsCharacter.cpp:59-70,533-542`; `PhysicsInternal.hpp:314-316` |
| A parked-driver workaround: `VehicleDriver` sets the driver's gravity factor and max strength to 0 and restores them on exit | `modules/vehicle.scene/src/VehicleDriver.cpp:232-240` |
| C#: `Aver.Physics.CharacterBody` (non-partial `readonly struct`, 263 lines); `Aver.Framework.AverCharacter` (public fields `MoveSpeed`, `Height`, `Radius`, `JumpSpeed`...) creates its capsule through the framework's own `Phys` shim and sets its velocity every tick | `CharacterBody.cs`, `Character.cs:44-82,278-291,478-488`, `Physics.cs:37-50` |
| The editor's walk and fly capsules are made and driven in `SandboxPlay.cpp` (`:516,:585`) | `SandboxPlay.cpp` |
| Tests: `CharacterTest` (stairs, slope, shape swap, ground state), `CharacterExtTest` (character vs character, inner body, can-stand, contacts, edge removal) | `tests/physics/CMakeLists.txt` |

### 1.2 Ragdoll

| Fact | Where |
|---|---|
| `aver_phys_ragdoll_create`: bones in parent-first order, a capsule per bone (`radius <= 0` = no part), swing/swing/twist limits, one total mass; parts are plain body handles; modes kinematic / limp / powered for **the whole ragdoll** | `physics_ragdoll_abi.h:83-133`, `PhysicsRagdoll.cpp:324-464` |
| Shapes: capsule, or sphere when the segment is shorter than 0.1 mm. Mass = total x volume share. Damping 0.3 angular (Jolt's linear default). Friction, restitution untouched | `PhysicsRagdoll.cpp:238-277,364-365,404` |
| Joints: always `SwingTwistConstraintSettings` with `ESwingType::Cone`, the twist axis along the segment, the plane axis the bone's +Z (or +Y), `mMaxFrictionTorque = hold x 0.02` | `PhysicsRagdoll.cpp:408-428` |
| Collision: one `GroupFilterTable` with **every pair off**, sub-group = part index, group id = the ragdoll handle (`g_nextRagdoll++`), so two ragdolls never share a filter | `PhysicsRagdoll.cpp:72,386-388,434-435` |
| Motors: one `strength` for all joints; frequency `2 + 8 s` Hz, damping ratio 1, torque ceiling `hold x 20 x s`. The three numbers are constants (`:46-53`). Joints are cast to `SwingTwistConstraint` everywhere | `PhysicsRagdoll.cpp:44-53,121-158` |
| `RagdollSettings::Stabilize()` is **not** called (documented: it would flatten the requested mass ratios); `DisableParentChildCollisions`, `mAdditionalConstraints`, `SkeletonMapper` are not used; no velocity, bounds, root-transform or group-id function exists | `docs/physics/RAGDOLL.md`, grep |
| `get_pose`: a part bone is its body; a bone without a part is `ancestor part now x (ancestor target)^-1 x bone target` (rigid ride) | `PhysicsRagdoll.cpp:550-587` |
| Kinematic driving is a `PhysicsStepListener` (`Stepper`) that calls `MoveKinematic` on every part of every kinematic ragdoll | `PhysicsRagdoll.cpp:205-226` |
| The system above it: `RagdollSystem` (`modules/ragdoll`) builds a `RagdollDef` from a skeleton (capsules by rule, limits 45/45/30), applies an optional `<skeleton>.ocragdoll` text override (`mass`, `bone ... radius / length / limits / on / off`), and blends the simulated pose over the animation by a weight. Builds with `aver_phys_ragdoll_create`, never with anything richer | `RagdollBuilder.hpp:25-71`, `RagdollSystem.cpp:176-230`, `docs/physics/RAGDOLL_ANIMATION.md:87-107` |
| Relay: `aver_fw_set_ragdoll_provider` takes five typed function pointers (`framework_abi.h`, off limits); both hosts install it | `GameApp.cpp:1949-1957`, `SandboxInit.cpp:304-` |
| C#: `Aver.Physics.Ragdoll` (non-partial struct), `Aver.Framework.Entity.StartRagdoll / StopRagdoll / SetRagdollStrength / AddRagdollImpulse`; four Core graph rows (`RagdollStart`, `RagdollStop`, `SetRagdollStrength`, `RagdollImpulse`) | `Ragdoll.cs`, `Aver.Framework/Ragdoll.cs`, `docs/nodes/Core.md:179-191` |
| Tests: `RagdollTest` (ABI), `RagdollBuilderTest`, `RagdollSystemTest` | `tests/physics`, `tests/ragdoll` |

### 1.3 Vehicles

| Fact | Where |
|---|---|
| `aver_phys_vehicle_set_kind` accepts wheeled and tracked only; `wheeledOf` casts the controller to `WheeledVehicleController` (which `MotorcycleController` derives from, so every wheeled path works for a bike unchanged). `MotorcycleController.cpp` is in `Jolt.cmake:410` and its settings are registered in `RegisterTypes.cpp:171` | `PhysicsVehicleTracked.cpp:177-184`, `PhysicsVehicleExt.cpp:80-82` |
| The wheel cap is a file constant `kMaxWheels = 16` | `PhysicsVehicle.cpp:68,359` |
| The wheel cast is `VehicleCollisionTesterCastCylinder(Layers::MOVING)` for every vehicle: no ray, no sphere, a fixed layer | `PhysicsVehicle.cpp:501` |
| Wheel settings write position, suspension direction (down), steering axis, wheel up, wheel forward (all engine axes), min / max length, **frequency-and-damping** spring, radius, width, steer, brake torques, inertia, damping, curves. Never written: `mSuspensionPreloadLength`, `mSuspensionForcePoint` / `mEnableSuspensionForcePoint`, the other two spring modes, a camber / toe / caster / rake | `PhysicsVehicle.cpp:144-173` |
| `mMaxPitchRollAngle` is set once at `finish`; no `OverrideGravity`; no `SetNumVelocityStepsOverride`; no `SetTireMaxImpulseCallback`; only the combine-friction lambda is installed | `PhysicsVehicle.cpp:478,510-515`, grep |
| The axle rule pairs wheels by x within 5 cm on opposite sides of y = 0; a wheel on y = 0 is its own axle (left), so a two-wheeler is two axles of one wheel, the driven one gets a differential with `mRightWheel = -1`, and no anti-roll bar is built (it needs both sides) | `PhysicsVehicle.cpp:241-267,181-203` |
| `applySuspensionScale` writes `mSuspensionSpring.mFrequency = baseHz x scale` | `PhysicsVehicleExt.cpp:142-146` |
| Telemetry reads `upZ = bv.up.GetY()` (the body's up against Jolt's world Y) and flips with engine +Z: both assume the world up is +Z | `PhysicsVehicleExt.cpp:394,838-843` |
| Spec side: `Kind { Wheeled, Tracked }`, `kMaxWheels = 16` in the spec, `WheelClass` (frequency spring only), `WheelDef` (hub, steer, driven, handbrake), no cast, camber or solver fields; `buildVehicle` makes the v1 calls, then v2 calls only for non-defaults; the attachment height is `hub.z + suspMax - sag(hz)` | `VehicleSpec.hpp:23-95`, `VehicleBuild.cpp:63-90` |
| `.ocvehicle` parser: `KIND` accepts `wheeled` and `tracked` only; unknown keys warn, unknown records are ignored, a newer version is refused | `OcVehicle.cpp:458-465` |
| The system and traffic: `world::VehicleSystem` skips any non-wheeled spec (`VehicleSystem.cpp:702`); `VehicleVisuals` branches on `Kind::Tracked` for the track scroll inputs (`:87`) | grep |
| Tests and content: `VehicleTest`, `VehicleExtTest`, `TrackedVehicleTest`, `tests/vehicle`, `tests/vehicle.scene`, `tests/framework_vehicle`; `templates/Vehicle` (Sedan, Tank), `test-content/VehicleLab` (Hatchback, Truck6x4, Tank, RiggedCar) | trees |

### 1.4 Seams this area uses

| Seam | State | Where |
|---|---|---|
| ABI parity test: C# `Native.<feature>.cs` partials are merged into the "physics ABI" row, so **every new header must be in that row's header list**, and that list is a `headers[12]` array that is **full** (11 headers + terminator) | `AbiEnumTest.cpp:552-574,908-921` |
| Fixed-step hooks: `vehicleBeginCall` before the accumulator loop, `vehicleStepBegin` / `vehicleStepEnd` around `system.Update`, the character block before them | `PhysicsWorld.cpp:395-497` |
| `aver_phys_remove_body` calls `releaseRagdollOfBody` and `releaseVehicleOfBody` before it touches a body; `aver_phys_shutdown` calls `clearCharacterStairSettings` before `characters.clear()` | `PhysicsWorld.cpp:329-332`, `PhysicsInternal.hpp:377-393` |
| Framework relays: provider-pointer pattern with generic `op`, `aver_fw_vehicle_fn` | `framework_vehicle_abi.h:32-44`, `VehicleAbi.cpp:10-20` |
| Named events from C++: `aver_fw_event_post(name, sender, target)` | `framework_timers_abi.h:99-101` |
| Node families: reflection-discovered `Graph<Family>NodeTable.cs` plus a palette mirror `sandbox/src/GraphNodeDefs<Family>.hpp` pulled in by `cmake/GraphNodeFamilies.cmake`; `[AutoRun]` tests on `NodeRowKit` | `GraphGameSystemNodeTable.cs:1-9`, `GraphNodeDefsVehicle.hpp:1-9`, `VehicleNodeTests.cs` |
| Gameplay systems register in `GameSystemsWiring.hpp`; the ragdoll provider is installed twice, once per host | `GameSystemsWiring.hpp`, hosts above |
| Existing test content with a skinned character and skeleton: `test-content/ControlRigDemo/Content/{Meshes/Character.ocmesh, Skeletons/Character.ocskel}` | tree |

### 1.5 Corrections and findings against the survey

1. **Where character callbacks run.** `CharacterContactListener` callbacks are invoked from `CharacterVirtual::Update` / `ExtendedUpdate`, i.e. on the thread that calls them: here the caller of `aver_phys_step`, inside the step. Only the body `ContactListener`, the vehicle tyre callback and the ragdoll `Stepper` run on Jolt workers. The design rule is unchanged (no script mid-step), but the cost model is cheaper: the listener needs no lock and no atomics for its own state.
2. **Half the "settings" are creation-only.** `CharacterVirtual` has setters for max hits, hit-reduction angle, penetration recovery, edge removal, mass, strength, shape offset, up, rotation, slope and supporting volume. Padding, predictive distance, back-face mode, `mMaxCollisionIterations`, `mMaxConstraintIterations`, `mMinTimeRemaining` and `mCollisionTolerance` are constructor-only (`CharacterVirtual.h:342-361,698-709`), so they rebuild.
3. **The stair extras are three more fields**, not two: min forward, forward test, cos contact angle and step-down extra (`CharacterVirtual.h:429-437`), of which the code reaches none.
4. **`SkeletonMapper` is not what makes the animation skeleton reachable.** The existing ABI takes the animation skeleton's pose arrays and reduces to parts internally. The mapper's value is the in-between bones (chains) and the stretch lock (decision 15).
5. **Jolt's `DriveToPoseUsingMotors` supports swing-twist and hinge only** and asserts otherwise (`Ragdoll.cpp:647-680`); the repository builds Jolt with asserts in Debug (`modules/physics.jolt/CMakeLists.txt`). Fixed and point joints in a powered ragdoll would assert unless motors are driven by Aver's own loop (decision 14).
6. **A hinge needs its rest angle inside its limits** (Jolt asserts `min <= 0 <= max` at creation); section 2.4 states the range as relative to the creation pose.
7. **Two ragdolls in one group id never collide** because their filters differ (`GroupFilterTable::CanCollide` returns false for the same group with different filters). That is exactly the "friendly crowd" switch the group id wants.
8. **`MotorcycleController` has no public lean-angle getter** (`mTargetLean` is protected); the lean angle the ABI reports is computed from the body's orientation against the world up.
9. **The vehicle telemetry assumes world up = +Z** (`PhysicsVehicleExt.cpp:394`); a gravity override needs a `worldUp(e)` helper (section 2.6.5).
10. **The suspension force point is the stable choice, and Jolt's motorcycle code reads it.** `Wheel.h` says that with the point disabled (the default) forces act at the contact point, which is more accurate against dynamic objects but less stable, and that enabling it applies every tyre force at a fixed point on the body; `MotorcycleController::GetWheelBase` measures the wheelbase from the force points when they are enabled (`MotorcycleController.cpp:73-76`). The motorcycle preset enables the point at the wheel centre. (That Jolt's own motorcycle sample does the same is recalled, UNVERIFIED; the sample is not vendored.)

---

## 2. Design

### 2.1 Architecture

```
 Aver Node rows (Character, Ragdoll, VehicleAdv)      C# (Aver.Physics wrappers, Aver.Framework Entity / AverCharacter members)
        |                                                    |
        |   Aver.Framework --> Aver.Physics (P/Invoke)       |   ragdoll + vehicle ops --> framework relays (provider pattern)
        v                                                    v
 modules/physics   physics_character_ext_abi.h   physics_ragdoll_ext_abi.h   physics_vehicle_adv_abi.h
   PhysicsCharacterExt.cpp   PhysicsCharacterStep.cpp (fixed-step block, listener, profiles, motor, events)   PhysicsCharacterNpc.cpp
   PhysicsRagdollBuilder.cpp PhysicsRagdollExt.cpp (+ PhysicsRagdoll.cpp refactored)
   PhysicsVehicleAdv.cpp     PhysicsVehicleMoto.cpp (+ PhysicsVehicle*.cpp touched)
        |                                                    |
 modules/ragdoll (RagdollSystem, .ocragdoll)      modules/vehicle (.ocvehicle, buildVehicle)  +  modules/vehicle.scene (VehiclePhysicsSystem)
        |                                                    |
 hosts: Runtime/include/aver/game/CharacterWiring.hpp (event pump, ragdoll ext provider)  |  sandbox: Show > Physics Actors overlay
```

**Threading rule for the whole area, stated once.** Everything callable from the outside is called between `aver_phys_step` calls, from the stepping thread. Inside a step, native code reads the plain-data tables below and writes only its own state and queues; **no function pointer supplied by a caller is ever stored** (the only function pointers anywhere in this design are native lambdas and the existing combine-friction lambda). A test per feature runs the same input twice and compares bits (section 5).

### 2.2 Character physics ABI: `physics_character_ext_abi.h`

New header, new sources `PhysicsCharacterExt.cpp` (settings, orientation, shapes, ground and contact detail, steep helpers, enable), `PhysicsCharacterStep.cpp` (the moved step block, the listener, profiles, the motor, events) and `PhysicsCharacterNpc.cpp` (the rigid character), with a src-local `PhysicsCharacterState.hpp` defining `CharacterLive` (the per-character live state: enable flag, wish, gravity mode and vector, previous ground state, airborne time, last landing speed), held in an **ordered** `World::characterLive` map so the state-recording code of WORLD_STATE_DEBUG can reach it from `PhysicsInternal.hpp` (section 2.12), and the configuration records (settings, stairs, motor values, profiles), which stay file-local. `PhysicsCharacter.cpp` keeps the v1 functions; its `StairSettings` table and `characterCreated / characterDestroyed / clearCharacterStairSettings` hooks are extended to own and clear the new state. Types are `int32_t`, `float`, `const float*`, `int32_t*`. Units and axes are the engine's (cm, +X forward, +Y right, +Z up, left-handed), angles in **degrees** in blocks (v1 character functions use radians; each function says), returns 1 or 0 with `aver_phys_last_error` as everywhere else, `-1` where 0 is a real answer. **A parameter block may be shorter than its defined length (the rest keep their values) and is refused if longer** (the vehicle rule). Every ground, contact and block index below is an `int32_t` constant in the header, mirrored by C# classes pinned in `AbiEnumTest` (section 3.3).

```c
#define AVER_PHYS_CHARACTER_ABI_VERSION 2
AVER_PHYS_API int32_t aver_phys_character_abi_version(void);                       /* 2 */

/* ---- shapes: AVER_PHYS_CHRSHAPE_* ------------------------------------------------------------------------ */
#define AVER_PHYS_CHRSHAPE_CAPSULE  0   /* p: radius, totalHeight                                  */
#define AVER_PHYS_CHRSHAPE_SPHERE   1   /* p: radius                                                */
#define AVER_PHYS_CHRSHAPE_BOX      2   /* p: hx, hy, hz (half extents, engine axes: x fwd, y right, z up) */
#define AVER_PHYS_CHRSHAPE_CYLINDER 3   /* p: radius, totalHeight                                   */
#define AVER_PHYS_CHRSHAPE_TAPERED  4   /* p: topRadius, bottomRadius, totalHeight (a tapered capsule) */

/* Like aver_phys_character_create, any shape, and the settings block applied at birth (no rebuild). The position is
   the SHAPE CENTRE for every kind (as for the v1 capsule). settings may be NULL. Returns the handle or 0. */
AVER_PHYS_API int32_t aver_phys_character_create_ex(int32_t shapeKind, const float* shapeParams, int32_t nParams,
                                                    float x, float y, float z, const float* settings, int32_t nSettings);

AVER_PHYS_API int32_t aver_phys_character_set_settings(int32_t ch, const float* p, int32_t n);   /* AVER_PHYS_CHRSET_* */
AVER_PHYS_API int32_t aver_phys_character_settings(int32_t ch, float* out, int32_t n);           /* floats written, 0 for a dead handle */
AVER_PHYS_API int32_t aver_phys_character_list(int32_t* outHandles, int32_t cap);                 /* live characters, ascending; returns the count (may exceed cap) */
```

#### 2.2.1 The settings block (`AVER_PHYS_CHRSET_*`)

`index NAME (unit) = default, [live | rebuild]`. **Rebuild** means Jolt has no setter: `set_settings` builds a replacement `CharacterVirtual` from `GetCharacterVirtualSettings()` with the edited fields, under the same handle, copying position, rotation, velocity, user data, the inner body (re-created, as in `set_inner_body`), and re-registering it (`characterDestroyed`, `characterCreated`); `RefreshContacts` finds the ground again. A call that changes no rebuild-flagged value never rebuilds (a test pins the object identity). A block equal to the current values is a no-op.

| Index | Name | Default | Notes |
|---|---|---|---|
| 0 | `PADDING_CM` | 2 | how far the sweep stays off geometry (`mCharacterPadding` 0.02 m). rebuild |
| 1 | `PREDICTIVE_CONTACT_CM` | 10 | `mPredictiveContactDistance` 0.1 m; 0 sticks. rebuild |
| 2 | `PENETRATION_RECOVERY` | 1 | 0 = nothing resolved, 1 = all in one update. live |
| 3 | `MAX_HITS` | 256 | contact collection cap. live |
| 4 | `HIT_REDUCTION_COS` | 0.999 | merge hits whose normals differ less; -1 off. live |
| 5 | `BACK_FACE_MODE` | 0 | 0 collide with back faces (Jolt's default), 1 ignore them. rebuild |
| 6 | `MAX_COLLISION_ITERATIONS` | 5 | rebuild |
| 7 | `MAX_CONSTRAINT_ITERATIONS` | 15 | rebuild |
| 8 | `MIN_TIME_REMAINING` | 0.0001 | seconds; rebuild |
| 9 | `COLLISION_TOLERANCE_CM` | 0.1 | rebuild |
| 10 | `SUPPORT_AUTO` | 1 | 1 = the supporting volume is the plane at the shape's horizontal radius above the position (what v1 builds); 0 = use `SUPPORT_HEIGHT_CM`. live |
| 11 | `SUPPORT_HEIGHT_CM` | 0 | contacts below this height above the position (along up) may support the character. live |
| 12 | `AUTO_CANCEL_STEEP` | 0 | 1 = before each update the step runs `CancelVelocityTowardsSteepSlopes` on the velocity (Jolt's own recipe), so a character cannot be driven up an unclimbable slope. live |
| 13 | `MOVE_USER_LAYER` | 0 | the user layer the character **moves as** (its sweeps use that layer's object and broad-phase filters). live |
| 14 | `INNER_USER_LAYER` | -1 | the user layer of the inner body; -1 = same as `MOVE_USER_LAYER`. live (`BodyInterface::SetObjectLayer`) |
| | `COUNT` | 15 | |

The layer pair closes the survey's "inner-body layer" gap and adds the one thing it implies: a character can be told to ignore a team barrier through the layer matrix (`physics_layers_abi.h`). The values are user layers 0..15 and map through `Layers::encode(layer, true)`; both default to 0, which is `Layers::MOVING`, today's behaviour.

#### 2.2.2 Up, rotation, gravity

```c
/* Any length except zero. Turns the supporting-volume normal, the stair vectors (they now use GetUp(), see below) and the
   carry projection (already GetUp()). Does NOT turn the shape; see align_to_up. */
AVER_PHYS_API int32_t aver_phys_character_set_up(int32_t ch, float x, float y, float z);
AVER_PHYS_API int32_t aver_phys_character_up(int32_t ch, float* outXyz);
AVER_PHYS_API int32_t aver_phys_character_set_rotation(int32_t ch, float qx, float qy, float qz, float qw);   /* the shape's orientation; matters for boxes and tapered shapes */
AVER_PHYS_API int32_t aver_phys_character_rotation(int32_t ch, float* outXyzw);
/* set_up, plus the rotation turned the shortest way so the shape's +Z is the new up, the heading (its forward projected on
   the new up plane) kept. The call a planet walker makes every frame. */
AVER_PHYS_API int32_t aver_phys_character_align_to_up(int32_t ch, float x, float y, float z);

#define AVER_PHYS_CHRGRAV_WORLD    0   /* the world's gravity vector x the gravity factor (today)                     */
#define AVER_PHYS_CHRGRAV_ALONG_UP 1   /* the world gravity's MAGNITUDE along -up x the gravity factor                 */
#define AVER_PHYS_CHRGRAV_VECTOR   2   /* x, y, z in cm/s^2 x the gravity factor                                       */
AVER_PHYS_API int32_t aver_phys_character_set_gravity_mode(int32_t ch, int32_t mode, float x, float y, float z);
AVER_PHYS_API int32_t aver_phys_character_gravity_mode(int32_t ch, int32_t* outMode, float* outXyz);
```

- A character at the default (`up` = +Z, mode `WORLD`) takes the same arithmetic path as v1; `gravity = system.GetGravity() * factor` is evaluated in the same order.
- **Mode `WORLD` with a different up is legal** and means "walk on the wall while falling down the world" (a magnet boot). The planet case wants `ALONG_UP`.
- `PhysicsCharacter.cpp:528` builds the stair vectors from `toJoltUnit(0,0,1)`; with `GetUp()` they follow the character (identical at the default). `set_shape` recomputes the supporting plane from `GetUp()` too (`:307`).
- The quaternion functions convert with `toJolt(Quat)` and normalise; zero length is refused. `set_rotation` calls `CharacterVirtual::SetRotation`, which also moves the inner body.
- `can_set_shape` and `set_shape` already read `c->GetUp()` (`PhysicsCharacter.cpp:435`), so nothing there changes.

#### 2.2.3 Shapes and offset

```c
/* Replaces the shape. Like v1 set_shape it is checked against the world first (maxPenetrationCm) and REFUSED (0, Unsupported) if it
   would embed; the inner body shape and the supporting plane follow. FEET STAY: the position moves along up by the change in the
   shape's lowest extent (shape bounds, so it works for every kind). */
AVER_PHYS_API int32_t aver_phys_character_set_shape_ex(int32_t ch, int32_t kind, const float* p, int32_t n, float maxPenetrationCm);
AVER_PHYS_API int32_t aver_phys_character_shape_ex(int32_t ch, int32_t* outKind, float* outParams, int32_t cap);   /* floats written */
AVER_PHYS_API int32_t aver_phys_character_can_set_shape_ex(int32_t ch, int32_t kind, const float* p, int32_t n);   /* the v1 can-stand query for any shape */
/* A local offset of the shape from the character's position (cm, character frame). keepWorldPlace = 1 moves the position by the
   opposite so the shape does not move (Jolt: setting it on the fly can teleport the shape into collision). */
AVER_PHYS_API int32_t aver_phys_character_set_shape_offset(int32_t ch, float x, float y, float z, int32_t keepWorldPlace);
AVER_PHYS_API int32_t aver_phys_character_shape_offset(int32_t ch, float* outXyz);
```

- The shape of every kind is built in Jolt's axes with its long axis on Jolt Y (= engine Z), centred, exactly as the capsule is built now (`halfCylinder = totalHeight / 2 - radius`). A box takes its half extents through the same permutation `aver_phys_add_static_box` uses (`PhysicsWorld.cpp:503-505`). A cylinder takes `totalHeight / 2` as its half height. A tapered capsule is `TaperedCapsuleShapeSettings(halfCylinder, top, bottom)`.
- The feet offset used for "feet stay" and for `can_set_shape_ex` is `shapeOffset.y + shape->GetLocalBounds().mMin.GetY()` in Jolt shape space, rotated by the character's rotation, so a box or a rotated character keeps its feet correctly. For the capsule it equals the v1 formula.
- `aver_phys_character_shape` (v1) still returns 0 / `Unsupported` for a non-capsule; the `_ex` getter is the general one.
- **Supporting volume for non-capsules:** the auto plane height is the shape's largest horizontal half extent.
- Characters of different shapes meet each other through `CharacterVsCharacterCollisionSimple` as before; a box character is a legitimate crate-sized pusher.

#### 2.2.4 Ground and contact detail

```c
/* AVER_PHYS_CHRGND_* ints, AVER_PHYS_CHRGNDF_* floats (either array may be NULL); returns floats written, 0 for a dead handle. */
AVER_PHYS_API int32_t aver_phys_character_ground_info(int32_t ch, int32_t* outI, int32_t nI, float* outF, int32_t nF);
/* index over the same touching() list as contact_get; AVER_PHYS_CHRCON_* ints, AVER_PHYS_CHRCONF_* floats. */
AVER_PHYS_API int32_t aver_phys_character_contact_info(int32_t ch, int32_t index, int32_t* outI, int32_t nI, float* outF, int32_t nF);
```

| Ground ints (`CHRGND_`) | Ground floats (`CHRGNDF_`) |
|---|---|
| 0 `STATE` (the `AVER_PHYS_GROUND_*` value) | 0..2 `NORMAL`, 3..5 `POSITION`, 6..8 `VELOCITY` (as `ground_velocity`) |
| 1 `BODY` handle (body or character, as `ground_body`) | 9 `BODY_FRICTION` (the ground body's, 0 if none) |
| 2 `BODY_ENTITY` (the `aver_phys_set_entity` stamp of what it stands on, 0 if none) | 10 `SLOPE_DEG` (angle of the normal from up) |
| 3 `SUBSHAPE` (raw `SubShapeID` value; a compound's child, a triangle's id) | 11 `LAST_LAND_SPEED_CMS` (downward speed at the last landing) |
| 4 `MATERIAL` (a physical-material id, **0 until SHAPES_MATERIALS lands**, section 7) | 12 `AIRBORNE_SECONDS` (0 while supported) |
| 5 `CHARACTER` (the handle of the character it stands on) | 13 `TRACTION` (the profile value in force, 1 if none) |
| 6 `STEEP` (the contact is beyond the max slope) | `CHRGNDF_COUNT` 14 |
| 7 `MAX_HITS_EXCEEDED` (the last collision check dropped hits: the geometry is too complex for the cap) | |
| `CHRGND_COUNT` 8 | |

| Contact ints (`CHRCON_`) | Contact floats (`CHRCONF_`) |
|---|---|
| 0 `BODY` (or the other character's handle), 1 `ENTITY` (stamp), 2 `SUBSHAPE`, 3 `MATERIAL` (0 until SHAPES_MATERIALS) | 0..2 `POINT`, 3..5 `NORMAL` (toward the character), 6..8 `SURFACE_NORMAL` (the real surface normal, flipped for a back face) |
| 4 `IS_CHARACTER`, 5 `HAD_COLLISION`, 6 `BACK_FACE`, 7 `CAN_PUSH` (the listener's final answer for this contact) | 9..11 `CONTACT_VELOCITY`, 12 `DISTANCE_CM`, `CHRCONF_COUNT` 13 |
| `CHRCON_COUNT` 8 | |

`LAST_LAND_SPEED_CMS` and `AIRBORNE_SECONDS` are kept by the step block (section 2.2.7), not by Jolt. The v1 `ground_*` and `contact_*` functions are unchanged.

#### 2.2.5 Stairs and steep slopes

```c
/* AVER_PHYS_CHRSTAIR_*: 0 STEP_UP_CM 40, 1 STEP_DOWN_CM 50, 2 MIN_FORWARD_CM 2, 3 FORWARD_TEST_CM 15, 4 MAX_CONTACT_ANGLE_DEG 75, 5 STEP_DOWN_EXTRA_CM 0.  COUNT 6. */
AVER_PHYS_API int32_t aver_phys_character_set_stairs(int32_t ch, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_character_stairs(int32_t ch, float* out, int32_t n);
/* CancelVelocityTowardsSteepSlopes: the desired velocity (cm/s, world) with any part that would climb an unclimbable slope removed. */
AVER_PHYS_API int32_t aver_phys_character_cancel_steep_velocity(int32_t ch, float vx, float vy, float vz, float* outXyz);
/* IsSlopeTooSteep for a unit normal against this character's up and max slope: 1, 0, or -1 for a dead handle. */
AVER_PHYS_API int32_t aver_phys_character_slope_too_steep(int32_t ch, float nx, float ny, float nz);
```

`set_stair_stepping` (v1) and `set_stairs` write the same `StairSettings` record (grown from two floats to six), so the two cannot disagree. `applyCharacterStairSettings` fills all six `ExtendedUpdateSettings` fields; the vectors are `GetUp() * distance`, the scalar ones convert cm to metres and degrees to a cosine. Defaults are Jolt's (`mWalkStairsMinStepForward 0.02`, `mWalkStairsStepForwardTest 0.15`, `cos(75 deg)`, extra 0), so an untouched character is unchanged.

#### 2.2.6 Enable, and what "off" means

```c
AVER_PHYS_API int32_t aver_phys_character_set_enabled(int32_t ch, int32_t on);
AVER_PHYS_API int32_t aver_phys_character_enabled(int32_t ch);    /* 1 / 0, 0 for a dead handle */
```

Disabled: the character is skipped by the step block (no gravity, no `ExtendedUpdate`, no events), removed from the `CharacterVsCharacterCollisionSimple` list (`g_cvc->Remove`), and its inner body is removed from the broad phase (`BodyInterface::RemoveBody`; rays, sensors and crates no longer see it). Position, velocity and every setting are kept. Enabling re-adds the body, re-registers, and runs `RefreshContacts`. The handle stays valid; `set_position` works while disabled (the owner moves it with the vehicle seat). `VehicleDriver` switches to this (section 2.3.4) with the old gravity and strength values as the fallback when the call fails.

#### 2.2.7 The listener, profiles, the motor and events

One `final` class in `PhysicsCharacterStep.cpp` derives `JPH::CharacterContactListener` and is set (`SetListener`) on every character by `characterCreated`. It is the **only** character listener (a `CharacterVirtual` holds one pointer); CONTACTS_FILTERS' pair, team and group rules reach it through the weak hooks of 2.12. Its callbacks read the **character profile** of what the character touches (and of the character itself) and write the event queue. They run on the stepping thread inside `ExtendedUpdate`.

```c
/* AVER_PHYS_CHRPROF_*: 0 TRACTION 1, 1 SLIDE 1, 2..4 BELT_X/Y/Z (cm/s, world) 0, 5 FLAGS (bit mask carried in a float) 3.  COUNT 6.
   AVER_PHYS_CHRPROFFLAG_*: 1 CAN_PUSH_CHARACTER, 2 CAN_RECEIVE_IMPULSES (both ON by default), 4 GHOST (characters pass through and
   cannot stand on it), 8 EVENTS (this object reports contact begin and end events). */
AVER_PHYS_API int32_t aver_phys_chrprofile_set(int32_t handle, const float* p, int32_t n);          /* a body OR a character handle */
AVER_PHYS_API int32_t aver_phys_chrprofile_set_entity(int32_t entity, const float* p, int32_t n);   /* every body or character stamped with this entity */
AVER_PHYS_API int32_t aver_phys_chrprofile_clear(int32_t handle);
AVER_PHYS_API int32_t aver_phys_chrprofile_clear_entity(int32_t entity);
AVER_PHYS_API int32_t aver_phys_chrprofile_set_surface(int32_t surfaceId, const float* p, int32_t n);     /* every body whose ground material has this SHAPES_MATERIALS surface id; inert until that area fills MATERIAL */
AVER_PHYS_API int32_t aver_phys_chrprofile_clear_surface(int32_t surfaceId);
AVER_PHYS_API int32_t aver_phys_chrprofile(int32_t handle, float* out, int32_t n);                  /* 0 when none is set */
```

| Callback | What it does with the profile |
|---|---|
| `OnAdjustBodyVelocity` | `ioLinearVelocity += toJoltDir(belt)` for a body with a belt velocity. This is Jolt's own conveyor hook; `UpdateGroundVelocity` and the moving-obstacle logic both go through it (`CharacterVirtual.cpp:178,1155,1505`), so a character standing on a belt is carried and one pushing into its side sees a moving wall |
| `OnContactValidate` / `OnCharacterContactValidate` | `false` when the installed pair rule says no (2.12), then `false` for a `GHOST` profile (the contact is discarded, so it neither blocks nor supports) |
| `OnContactAdded` / `OnContactPersisted` (+ character variants) | `ioSettings.mCanPushCharacter` and `mCanReceiveImpulses` from the flags. A character's own profile is consulted for `OnCharacterContact*` (so a boss that cannot be shoved sets CAN_PUSH_CHARACTER 0 on itself). Appends `CONTACT_BEGIN` for an `EVENTS` profile |
| `OnContactRemoved` | `CONTACT_END` for an `EVENTS` profile |
| `OnContactSolve` | `SLIDE` below 1: `ioNewCharacterVelocity = lerp(contactVelocity, ioNewCharacterVelocity, slide)`: 1 is Jolt's slide, 0 sticks the character to the surface's own motion (a glue floor, a sticky wall). UNVERIFIED feel |

The lookup order is the body or character handle (a `BodyID` resolved through `World::byId`, a character through its id), then the entity stamp (`GetUserData()` truncated to `int32`), then the ground material's surface id (SHAPES_MATERIALS, inert until it exists); the first profile found wins. A hash miss costs one lookup per contact. With no profile set a callback does exactly what Jolt's default does, which is nothing.

```c
/* The locomotion motor, AVER_PHYS_CHRMOTOR_*: 0 ENABLED 0, 1 ACCEL_GROUND 2400 cm/s^2, 2 ACCEL_AIR 600, 3 BRAKE_GROUND 2800, 4 BRAKE_AIR 0, 5 MAX_SPEED 0 (0 = unlimited).  COUNT 6. */
AVER_PHYS_API int32_t aver_phys_character_set_motor(int32_t ch, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_character_motor(int32_t ch, float* out, int32_t n);
AVER_PHYS_API int32_t aver_phys_character_set_wish(int32_t ch, float x, float y, float z);   /* world cm/s; only the part on the up plane is used; held until set again */
```

With `ENABLED`, each fixed step, before `ExtendedUpdate` and after the gravity and carry lines: take the character's own horizontal velocity `h` (the velocity minus its up component, which is already relative to the ground because the carry is held outside it), the target `w` (the wish's horizontal part, clamped to `MAX_SPEED`), and `a = (supported ? ACCEL_GROUND * traction : ACCEL_AIR)` where `traction` comes from the ground body's profile (1 if none). If `|w| > 0.01` move `h` toward `w` by at most `a * dt`; otherwise toward zero by at most `(supported ? BRAKE_GROUND * traction : BRAKE_AIR) * dt`. The vertical part is untouched, so jumps, falls and `set_velocity` keep working; a `set_velocity` while the motor is on is a launch (knockback) that the motor then decays. Ice is traction 0.05; mud 0.4. Acceleration, not speed, carries the surface, so it is independent of the frame rate.

```c
/* Polled and cleared at the top of aver_phys_step, like contacts. AVER_PHYS_CHREV_*: 1 CONTACT_BEGIN, 2 CONTACT_END, 3 LANDED, 4 LEFT_GROUND,
   5 SLIDING_BEGIN, 6 SLIDING_END, 7 HEAD_BUMP.
   outI6 = kind, character, the character's entity stamp, a, b, 0; outF8 = point xyz, normal xyz, value, value2.
   CONTACT_*: a = other body or character handle, b = its entity stamp.
   LANDED: value = downward speed cm/s, value2 = airborne seconds.  LEFT_GROUND: value = upward speed cm/s (a jump), a = 1 if the character was on steep ground.
   SLIDING_*: ground became or stopped being OnSteepGround, normal = the slope.  HEAD_BUMP: a contact whose normal points down while the character moves up; value = speed. */
AVER_PHYS_API int32_t aver_phys_character_event_count(void);
AVER_PHYS_API int32_t aver_phys_character_event_get(int32_t index, int32_t* outI6, float* outF8);
```

State transitions (`LANDED`, `LEFT_GROUND`, `SLIDING_*`) are detected in the step block right after each character's `ExtendedUpdate` by comparing the ground state to the one kept from the previous step; `HEAD_BUMP` from the contact list. The queue holds at most 4096 events per call (the vehicle limit); the first 4096 are kept and the rest dropped with one log line.

#### 2.2.8 The rigid-body NPC: `aver_phys_npc_*`

```c
/* AVER_PHYS_NPC_*: 0 MASS_KG 80, 1 FRICTION 0.2, 2 GRAVITY_FACTOR 1, 3 USER_LAYER 0, 4 MAX_SEPARATION_CM 3, 5 MAX_SLOPE_DEG 50, 6 YAW_FREE 0 (1 = may rotate about up). COUNT 7. */
AVER_PHYS_API int32_t aver_phys_npc_create(int32_t shapeKind, const float* shapeParams, int32_t nParams, float x, float y, float z,
                                           const float* settings, int32_t nSettings);   /* the BODY handle, or 0 */
AVER_PHYS_API int32_t aver_phys_npc_destroy(int32_t npc);                               /* same as aver_phys_remove_body for it */
AVER_PHYS_API int32_t aver_phys_npc_is(int32_t handle);                                 /* 1 if the handle is an NPC body */
AVER_PHYS_API int32_t aver_phys_npc_set_settings(int32_t npc, const float* p, int32_t n);   /* live: mass, friction, gravity factor, max separation, slope */
AVER_PHYS_API int32_t aver_phys_npc_ground_state(int32_t npc);                          /* AVER_PHYS_GROUND_*, -1 dead */
AVER_PHYS_API int32_t aver_phys_npc_ground_info(int32_t npc, int32_t* outI, int32_t nI, float* outF, int32_t nF);   /* the CHRGND blocks */
AVER_PHYS_API int32_t aver_phys_npc_set_shape_ex(int32_t npc, int32_t kind, const float* p, int32_t n, float maxPenetrationCm);
```

`JPH::Character` owns one rigid body that you drive by setting its velocity; it takes part in the solver (pushes and is pushed, is hit by bullets) and costs less than a virtual character per instance, at the price of no stair walking, no padding, no listener rules and softer slope handling. The handle returned is the body handle, registered in `World::bodies` / `byId` like a ragdoll part, so `aver_phys_body_set_velocity`, `_position`, `aver_phys_set_entity`, raycasts and joints work on it; the NPC functions above are only what a body cannot say (the ground). `characterStepEnd` calls `Character::PostSimulation(maxSeparation)` after `system.Update` for each NPC. `aver_phys_remove_body` on the handle releases the NPC first (`releaseNpcOfBody`, beside `releaseRagdollOfBody`), and `clearCharacterStairSettings` (the shutdown hook) destroys them all. Rotation is frozen unless `YAW_FREE` (allowed DOFs are the three translations by default), so an NPC stays upright; it is built for `up` = +Z only, which the header says.

#### 2.2.9 Order of work in a fixed step (`characterStepBegin`)

For each enabled character in handle order (`World::characters` is an unordered map today; the new block iterates the ordered `World::characterLive` map, whose keys are the handles, so events and results are deterministic):

1. gravity vector by mode, times the factor; `up = ch->GetUp()`;
2. supported / airborne bookkeeping and the carry, **verbatim from `PhysicsWorld.cpp:422-447`**;
3. `AUTO_CANCEL_STEEP` (off by default);
4. the motor (off by default);
5. stairs filled from the six-field record;
6. `ExtendedUpdate(dt, gravity, us, filters for MOVE_USER_LAYER, characterBodyFilter(), ...)`;
7. the carry taken back out (`:463`);
8. transition events, `LAST_LAND_SPEED`, `AIRBORNE_SECONDS`.

`characterStepEnd` runs NPC `PostSimulation`. `characterBeginCall` clears the event queue.

**What is and is not identical to v1.** One character, all settings at their defaults: bit-identical (the arithmetic of steps 1, 2, 5 (two fields), 6, 7 is v1's). Several characters: v1 iterated `World::characters`, an `unordered_map`, in hash order; the new block iterates handle order, so a world with characters that push each other can differ from v1 in the last bits. That is the one difference, it is deliberate (determinism is worth more than a hash order nobody chose), and the identity test uses a single character plus a separate bit-equality-of-two-runs test for many.

### 2.3 Characters in C#, the framework and the hosts

#### 2.3.1 `Aver.Physics` (the binding)

`CharacterBody` becomes `public readonly partial struct CharacterBody` (one keyword in `CharacterBody.cs`); the new members live in `CharacterBodyExt.cs`, the P/Invokes in `Native.CharacterExt.cs` (a `Native.<feature>.cs` partial, merged into the "physics ABI" parity row automatically; names copied character for character from the header). The index constants are `static class` mirrors in `CharacterExt.Index.cs` (`ChrSetIndex`, `ChrStairIndex`, `ChrMotorIndex`, `ChrGndIndex`, `ChrGndfIndex`, `ChrConIndex`, `ChrConfIndex`, `ChrProfIndex`, `NpcIndex`) and enums (`CharacterShapeKind`, `CharacterGravityMode`, `CharacterEventKind`, `CharacterProfileFlags`) pinned by new `AbiEnumTest` groups.

```csharp
public enum CharacterShapeKind { Capsule = 0, Sphere = 1, Box = 2, Cylinder = 3, TaperedCapsule = 4 }
public enum CharacterGravityMode { World = 0, AlongUp = 1, Vector = 2 }
[Flags] public enum CharacterProfileFlags { CanPushCharacter = 1, CanReceiveImpulses = 2, Ghost = 4, Events = 8 }

/// <summary>A character shape. Static factories name the parameters; the position of a character is the shape centre.</summary>
public readonly record struct CharacterShape(CharacterShapeKind Kind, float A, float B = 0, float C = 0)
{
    public static CharacterShape Capsule(float radius, float totalHeight);   public static CharacterShape Sphere(float radius);
    public static CharacterShape Box(float hx, float hy, float hz);          public static CharacterShape Cylinder(float radius, float totalHeight);
    public static CharacterShape Tapered(float topRadius, float bottomRadius, float totalHeight);
}
public readonly record struct CharacterSettings(float PaddingCm = 2, float PredictiveContactCm = 10, float PenetrationRecovery = 1, int MaxHits = 256,
    float HitReductionCos = 0.999f, bool IgnoreBackFaces = false, int MaxCollisionIterations = 5, int MaxConstraintIterations = 15,
    float MinTimeRemaining = 1e-4f, float CollisionToleranceCm = 0.1f, bool SupportAuto = true, float SupportHeightCm = 0, bool AutoCancelSteep = false,
    int MoveUserLayer = 0, int InnerUserLayer = -1);                       // the CHRSET block, defaults included
public readonly record struct StairSettings(float StepUpCm = 40, float StepDownCm = 50, float MinForwardCm = 2, float ForwardTestCm = 15,
    float MaxContactAngleDeg = 75, float StepDownExtraCm = 0);
public readonly record struct CharacterMotor(bool Enabled = false, float AccelGround = 2400, float AccelAir = 600, float BrakeGround = 2800,
    float BrakeAir = 0, float MaxSpeed = 0);
public readonly record struct CharacterProfile(float Traction = 1, float Slide = 1, Float3 Belt = default, CharacterProfileFlags Flags = CharacterProfileFlags.CanPushCharacter | CharacterProfileFlags.CanReceiveImpulses);
public readonly record struct CharacterGround(GroundState State, Body Body, int Entity, int SubShape, int Material, CharacterBody Character, bool Steep,
    bool MaxHitsExceeded, Float3 Normal, Float3 Position, Float3 Velocity, float BodyFriction, float SlopeDeg, float LastLandSpeedCmS,
    float AirborneSeconds, float Traction);
public readonly record struct CharacterContactInfo(Body Body, int Entity, int SubShape, int Material, bool IsCharacter, bool HadCollision, bool BackFace,
    bool CanPush, Float3 Point, Float3 Normal, Float3 SurfaceNormal, Float3 ContactVelocity, float DistanceCm);
public readonly record struct CharacterEvent(CharacterEventKind Kind, CharacterBody Character, int Entity, int A, int B, Float3 Point, Float3 Normal, float Value, float Value2);

public readonly partial struct CharacterBody
{
    /// <summary>The ABI version the loaded physics library reports for characters (2 with the extended ABI, 1 without).</summary>
    public static int AbiVersion { get; }
    public static CharacterBody Create(in CharacterShape shape, Float3 position, in CharacterSettings settings);
    public static CharacterBody Create(in CharacterShape shape, Float3 position);                       // all defaults
    public CharacterSettings Settings { get; }        public bool Apply(in CharacterSettings settings);   // rebuilds only if a creation-only value changed
    public Float3 Up { get; }                         public bool SetUp(Float3 up);          public bool AlignToUp(Float3 up);
    public Quaternion Rotation { get; set; }
    public (CharacterGravityMode Mode, Float3 Vector) Gravity { get; }   public bool SetGravity(CharacterGravityMode mode, Float3 vector = default);
    public bool Enabled { get; set; }
    public bool SetShape(in CharacterShape shape, float maxPenetrationCm);   public bool CanSetShape(in CharacterShape shape);   public CharacterShape? Shape { get; }
    public Float3 ShapeOffset { get; }                public bool SetShapeOffset(Float3 offset, bool keepWorldPlace = true);
    public StairSettings Stairs { get; set; }
    public Float3 CancelSteepVelocity(Float3 desired);   public bool IsSlopeTooSteep(Float3 unitNormal);
    public CharacterGround? Ground { get; }
    public CharacterContactInfo? GetContactInfo(int index);
    public CharacterMotor Motor { get; set; }         public bool SetWish(Float3 worldVelocityCmS);
    public static int DrainEvents(Span<CharacterEvent> buffer);        // reads this call's events, does not consume them (the next aver_phys_step clears)
    public static int List(Span<CharacterBody> buffer);
}
public static class CharacterProfiles
{
    public static bool Set(Body body, in CharacterProfile p);   public static bool Set(CharacterBody who, in CharacterProfile p);
    public static bool SetForEntity(int entity, in CharacterProfile p);   public static bool Clear(Body body);   public static bool ClearForEntity(int entity);
    public static CharacterProfile? Get(Body body);
}
/// <summary>A simulated rigid-body character for crowds. Its handle is also a Body.</summary>
public readonly struct NpcBody : IEquatable<NpcBody>
{
    public static NpcBody Create(in CharacterShape shape, Float3 position, in NpcSettings settings = default);   public bool Destroy();
    public Body AsBody { get; }   public GroundState GroundState { get; }   public CharacterGround? Ground { get; }
    public bool SetShape(in CharacterShape shape, float maxPenetrationCm);   public bool Apply(in NpcSettings settings);
}
public readonly record struct NpcSettings(float MassKg = 80, float Friction = 0.2f, float GravityFactor = 1, int UserLayer = 0, float MaxSeparationCm = 3,
    float MaxSlopeDeg = 50, bool YawFree = false);
```

A wrapper over a library without the extended ABI (`EntryPointNotFoundException`) answers false / null and `AbiVersion` 1, the `Vehicle.AbiVersion` pattern (`VehicleExt.cs:152`).

#### 2.3.2 `Aver.Framework`

- **`AverCharacter`** (`Character.cs`) becomes `partial` (one keyword) and gains, in a new `CharacterPhysics.cs`, serialised public fields that Details shows the way `MoveSpeed` and `Height` already are: `MaxSlopeDegrees` (50), `StepUpCm` (40), `StepDownCm` (50), `MassKg` (70), `PushStrength` (100 N, converted to kg*cm/s^2), `Acceleration` (0 = today: the tick sets the velocity directly; above 0 the motor is on with `AccelGround = Acceleration`, `AccelAir = Acceleration / 4`, `BrakeGround = Acceleration * 1.2`) and `UpMode` (`WorldZ` today, `FollowGravityField` for planets: each tick it asks a `GravityField` delegate for the up). `EnsureCapsule` (`Character.cs:481-488`) applies them after creation through `CharacterBody`; `OnTick` writes the wish instead of the horizontal velocity when `Acceleration > 0` (the vertical part still goes through `set_velocity`, `Character.cs:290`). With every field at its default the tick is the same calls in the same order as today (the identity test covers it).
- **`Entity`** (`EntityCharacter.cs`, partial): `CharacterBody Character { get; }` (via `aver_phys_character_of_entity`), `bool SetCharacterUp(Vec3 up, bool align = true)`, `SetCharacterGravity(CharacterGravityMode, Vec3)`, `bool TryGetCharacterGround(out CharacterGround)`, `bool SetCharacterShape(CharacterShape, float maxPenetrationCm = 1)`, `bool CanSetCharacterShape(CharacterShape)`, `bool TuneCharacter(string key, float value)` (keys: `slope` degrees, `stepup`, `stepdown`, `mass`, `strength`, `padding`, `recovery`, `predictive`, `maxhits`, `gravity` factor, `supportheight`, `steepcancel`, `enabled`, `layer`, `innerlayer`; unknown returns false and logs once), `SetCharacterMotor(...)`, `SetCharacterWish(Vec3)`, `SetCharacterProfile(CharacterProfile)`, `ClearCharacterProfile()`, `bool IsTouchingEntity(Entity other)`. Every member is XML-documented; the node methods wrap these, never the reverse.
- **`CharacterEvents`** (`CharacterEvents.cs`): `static int Drain(Span<CharacterEvent>)` for C# projects, same data as `CharacterBody.DrainEvents` with entities instead of handles.

#### 2.3.3 Events and the hosts

`Runtime/include/aver/game/CharacterWiring.hpp` (new, header-only like `GameSystemsWiring.hpp`):

```cpp
inline void tickCharacterEvents();       // after aver_phys_step: drain aver_phys_character_event_*, aver_fw_event_post("CharacterLanded" | "CharacterLeftGround" | "CharacterSliding" | "CharacterHeadBump", entity, other)
inline void installRagdollExtProvider(); // once, from registerGameSystems(): aver_fw_set_ragdoll_ext_provider(&ragdoll::providerExt, &ragdollSystem())
```

The sender is the character's own entity stamp, which every event carries (`outI6[2]`). Neither host is edited: `tickCharacterEvents()` is called from `tickGameplayGroups` right after `aver_phys_step` (one line in the shared `GameTick.hpp`, beside the physics event pump CONTACTS_FILTERS adds there), and `installRagdollExtProvider()` is called from `registerGameSystems()` (one line in the shared `GameSystemsWiring.hpp`, where the vehicle provider is installed). Events are posted with `aver_fw_event_post`, so graphs hear them at the next frame's flush, as for timers. Payload numbers are read from `an_characterground` (`LAST_LAND_SPEED_CMS`), not carried by the event: the named-event seam passes none from C++ today (the vehicle notify sink has the same limit). `CONTACT_BEGIN / END` of an `EVENTS` surface post `CharacterTouched` / `CharacterUntouched` with the profiled object's entity as target.

#### 2.3.4 `VehicleDriver` parks the driver with `set_enabled`

`modules/vehicle.scene/src/VehicleDriver.cpp:232-240` replaces "gravity factor 0 and max strength 0" with `aver_phys_character_set_enabled(driverCharacter, 0)` on enter and `1` on exit, keeping the old values as the path taken when the call fails (an older library). This also answers wave 10's open question 3 (a `CharacterVirtual` left inside the chassis jittering) for free, because a disabled character is not in the broad phase and is not stepped. Verified in headed check 11.

### 2.4 Ragdoll physics ABI: `physics_ragdoll_ext_abi.h`

New header; new sources `PhysicsRagdollBuilder.cpp` (the builder and the one internal build both creation paths use), `PhysicsRagdollExt.cpp` (runtime operations, inspection, the mapper) and a src-local `PhysicsRagdollInternal.hpp` that takes `Entry` and `g_ragdolls` out of `PhysicsRagdoll.cpp` so the three translation units share them (`detail::RagdollEntry`, `findRagdollEntry`). `PhysicsRagdoll.cpp` keeps the v1 ABI and the `Stepper`; `aver_phys_ragdoll_create` becomes "begin, no blocks, finish" and must behave exactly as before (test 1 of section 5). Conventions are 2.2's: engine units, angles in degrees, blocks may be shorter and are refused when longer, 0 / -1 answers, `aver_phys_last_error`.

```c
#define AVER_PHYS_RAGDOLL_ABI_VERSION 2
AVER_PHYS_API int32_t aver_phys_ragdoll_abi_version(void);       /* 2 */

/* ---- the builder: ragdoll_create's arguments, then optional blocks, then finish -------------------------------- */
AVER_PHYS_API int32_t aver_phys_ragdoll_builder_begin(int32_t boneCount, const int32_t* parents, const float* worldPos, const float* worldRot,
                                                      const float* capsules, const float* limitsDeg, float totalMassKg, uint32_t userLayer);
AVER_PHYS_API int32_t aver_phys_ragdoll_builder_set_bone(int32_t b, int32_t bone, const float* p, int32_t n);        /* AVER_PHYS_RAGBONE_* */
AVER_PHYS_API int32_t aver_phys_ragdoll_builder_set_options(int32_t b, const float* p, int32_t n);                    /* AVER_PHYS_RAGOPT_*  */
AVER_PHYS_API int32_t aver_phys_ragdoll_builder_add_constraint(int32_t b, int32_t kind, int32_t boneA, int32_t boneB,
                                                               const float* p, int32_t n);                            /* AVER_PHYS_RAGCON_*  */
AVER_PHYS_API int32_t aver_phys_ragdoll_builder_finish(int32_t b);   /* the ragdoll handle (consumes b), or 0 with aver_phys_last_error */
AVER_PHYS_API int32_t aver_phys_ragdoll_builder_cancel(int32_t b);
```

A builder holds copies of the arrays (so the caller's can go), lives in its own table with its own counter, and is discarded by `aver_phys_shutdown`. Begin validates exactly what `create` validates (`PhysicsRagdoll.cpp:328-343`); `set_bone` refuses a bone out of range; a second `finish` of the same handle is `BadHandle`.

#### 2.4.1 The per-bone block (`AVER_PHYS_RAGBONE_*`)

`index NAME (unit) = default`. A bone without a part (`radius <= 0`) ignores its block. Shape kinds `AVER_PHYS_RAGSHAPE_*`: 0 `CAPSULE` (the v1 shape: capsule, or sphere when the segment is shorter than 0.1 mm), 1 `BOX`, 2 `CYLINDER`, 3 `TAPERED`. Joint kinds `AVER_PHYS_RAGJOINT_*`: 0 `CONE` (v1), 1 `PYRAMID`, 2 `HINGE`, 3 `FIXED`, 4 `POINT`.

| Index | Name | Default | Meaning |
|---|---|---|---|
| 0 | `SHAPE` | 0 | a `RAGSHAPE_*` |
| 1..3 | `P0..P2` | 0 | box: half extents (bone-local engine axes) centred on the segment midpoint, oriented as the bone. Cylinder: `P0` half height (0 = half the segment length), radius from `capsules[]`. Tapered: `P0` = radius at the b end, `capsules[]` radius at the a end |
| 4 | `MASS_KG` | 0 | 0 = share by volume; above 0 = exactly this (section 2.4.5) |
| 5 | `FRICTION` | -1 | negative = Jolt's 0.2 |
| 6 | `RESTITUTION` | -1 | negative = Jolt's 0 |
| 7 | `LINEAR_DAMPING` | 0.05 | Jolt's default today |
| 8 | `ANGULAR_DAMPING` | 0.3 | today's `part.mAngularDamping` |
| 9 | `JOINT` | 0 | a `RAGJOINT_*` |
| 10..12 | `HINGE_AXIS` | 0 | hinge axis, bone-local; (0,0,0) = the swing2 axis of the v1 frame (perpendicular to the segment and to the plane axis) |
| 13, 14 | `HINGE_MIN_DEG`, `HINGE_MAX_DEG` | 0, 0 | relative to the creation pose, so `min <= 0 <= max` (refused otherwise); both 0 = `-swing2 .. +swing1` from `limitsDeg` |
| 15 | `MOTOR_FREQ_SCALE` | 1 | multiplies this joint's spring frequency |
| 16 | `MOTOR_DAMPING` | 1 | damping ratio (today's constant) |
| 17 | `MOTOR_TORQUE_SCALE` | 1 | multiplies this joint's torque ceiling |
| 18 | `FRICTION_TORQUE_SCALE` | 1 | multiplies the joint friction (`hold x 0.02` today) |
| 19 | `POSE_GROUP` | 0 | 0..31; the unit of partial control |
| | `COUNT` | 20 | `RAGBONE 15..18` are also the four fields of `set_motor` |

Hinge sign: angles are positive in the engine's own rotation sense about the given axis (the child's rotation relative to the parent as the engine's `Quat` expresses it). The left-handed to right-handed map flips axial vectors (`Convert.hpp` explains it), so the builder passes Jolt `toJoltAngular(axis)` and the same angle. A knee test pins the sign (section 5, ragdoll 5).

#### 2.4.2 The ragdoll-wide block (`AVER_PHYS_RAGOPT_*`)

| Index | Name | Default | Meaning |
|---|---|---|---|
| 0 | `STABILIZE` | 0 | `RagdollSettings::Stabilize()` before creation. **Conserves total mass, clamps each parent-child mass ratio to 0.8..1.2 of the chain, raises parent inertia to at least the sum of its children's** (`Ragdoll.cpp:51-131`). Explicit `MASS_KG` values are overwritten by it; the builder logs that once |
| 1 | `SELF_COLLISION` | 0 | 0 no part pair collides (v1). 1 selective: `DisableParentChildCollisions(creationPoseMatrices, SELF_MIN_SEPARATION_CM)`, so parent-child pairs and pairs that overlap in the creation pose are off and the rest (a hand and the opposite thigh) collide |
| 2 | `SELF_MIN_SEPARATION_CM` | 1 | pairs closer than this in the creation pose are treated as touching and disabled |
| 3 | `GROUP_ID` | 0 | 0 = the ragdoll's own group (v1: the handle). Above 0 = a shared **user group** 1..65535: ragdolls with the same user group never collide with each other (same group id, different filters: `GroupFilterTable::CanCollide` says no). Mapped to Jolt group `0x10000 + id` so it cannot meet a handle-derived id |
| 4 | `MOTOR_BASE_HZ` | 2 | today's `2` in `2 + 8 s` |
| 5 | `MOTOR_HZ_PER_STRENGTH` | 8 | today's `8` |
| 6 | `MOTOR_TORQUE_AT_FULL` | 20 | today's `kTorqueAtFullStrength` |
| 7 | `CCD` | 0 | 1 = `EMotionQuality::LinearCast` on every part, for thin fast limbs |
| 8 | `MAPPER` | 0 | 0 off (v1: bones without a part ride rigidly), 1 `SkeletonMapper` on, 2 on with every translation below the first mapped joint locked to the neutral pose (no visual stretch; the simulation still stretches) |
| | `COUNT` | 9 | |

#### 2.4.3 Extra (loop) constraints

`add_constraint(b, kind, boneA, boneB, p, n)` joins the parts that own the two bones (a bone's own part, or the ancestor part it rides on; both must exist and differ) with a Jolt constraint built in world space at the **creation pose** and stored in `RagdollSettings::mAdditionalConstraints`, so the `Ragdoll` creates, enables, disables and destroys it with the rest. Kinds `AVER_PHYS_RAGCONKIND_*`: 0 `DISTANCE`, 1 `POINT`, 2 `FIXED`, 3 `HINGE`, 4 `CONE`. Block `AVER_PHYS_RAGCON_*`: 0..2 `POINT_A` (world cm), 3..5 `POINT_B` (DISTANCE only; for every other kind the constraint sits at `POINT_A`), 6..8 `AXIS` (HINGE axis, CONE axis; world), 9 `MIN` (distance cm, or hinge degrees), 10 `MAX`, 11 `HALF_CONE_DEG`; `COUNT` 12. `MIN = MAX = -1` for DISTANCE keeps the creation distance. At most 16 per ragdoll. This is how two hands hold one rifle, a tail closes a loop, or a cape's shoulders hold a spar.

#### 2.4.4 Runtime operations

```c
/* Partial control. poseGroup -1 = every part. mode: -1 follow the ragdoll's mode, else AVER_PHYS_RAGDOLL_KINEMATIC / LIMP / POWERED. */
AVER_PHYS_API int32_t aver_phys_ragdoll_set_part_mode(int32_t ragdoll, int32_t poseGroup, int32_t mode);
AVER_PHYS_API int32_t aver_phys_ragdoll_part_mode(int32_t ragdoll, int32_t bone);          /* effective mode of the part the bone is or rides on; -1 dead / no part */
AVER_PHYS_API int32_t aver_phys_ragdoll_set_group_strength(int32_t ragdoll, int32_t poseGroup, float strength);   /* < 0 follows the ragdoll's strength */
AVER_PHYS_API int32_t aver_phys_ragdoll_set_motor(int32_t ragdoll, int32_t bone, const float* p, int32_t n);       /* RAGBONE 15..18 as a 4-float block; bone -1 = every joint */
/* Self-collision pairs on the shared filter. (-1,-1) every pair; (a,-1) part a against all others. Parent-child pairs can be turned on too. */
AVER_PHYS_API int32_t aver_phys_ragdoll_set_part_collision(int32_t ragdoll, int32_t boneA, int32_t boneB, int32_t on);
AVER_PHYS_API int32_t aver_phys_ragdoll_part_collision(int32_t ragdoll, int32_t boneA, int32_t boneB);               /* 1 / 0, -1 dead */
AVER_PHYS_API int32_t aver_phys_ragdoll_set_group_id(int32_t ragdoll, int32_t group);      /* the user group of RAGOPT 3; 0 back to the ragdoll's own */
AVER_PHYS_API int32_t aver_phys_ragdoll_group_id(int32_t ragdoll);
/* Whole-ragdoll motion. Linear cm/s world; angular rad/s world (engine sense). Dynamic parts only: kinematic parts are driven by the target. */
AVER_PHYS_API int32_t aver_phys_ragdoll_set_velocity(int32_t ragdoll, float vx, float vy, float vz, float ax, float ay, float az);
AVER_PHYS_API int32_t aver_phys_ragdoll_add_velocity(int32_t ragdoll, float vx, float vy, float vz);
AVER_PHYS_API int32_t aver_phys_ragdoll_bounds(int32_t ragdoll, float* outMinMax6);        /* world AABB of every part, cm: min xyz, max xyz */
AVER_PHYS_API int32_t aver_phys_ragdoll_root(int32_t ragdoll, float* outPos3, float* outRot4);   /* the root part's transform (the first part) */
/* Inspection: AVER_PHYS_RAGINFO_* ints, AVER_PHYS_RAGINFOF_* floats; AVER_PHYS_RAGPART_* floats. */
AVER_PHYS_API int32_t aver_phys_ragdoll_info(int32_t ragdoll, int32_t* outI, int32_t nI, float* outF, int32_t nF);
AVER_PHYS_API int32_t aver_phys_ragdoll_part_info(int32_t ragdoll, int32_t bone, float* outF, int32_t nF);
AVER_PHYS_API int32_t aver_phys_ragdoll_list(int32_t* outHandles, int32_t cap);              /* live ragdolls, ascending; returns the count */
```

`RAGINFO` ints: 0 `PART_COUNT`, 1 `BONE_COUNT`, 2 `MODE`, 3 `ACTIVE` (any part awake), 4 `GROUP_ID`, 5 `MAPPER`, 6 `SELF_COLLISION`, 7 `JOLT_GROUP` (the effective Jolt collision group id: the handle in v1, `0x10000 + id` with a user group; wave 11's cloth asks it so a cape keeps ignoring its wearer's ragdoll); `COUNT` 8. `RAGINFOF` floats: 0 `TOTAL_MASS_KG`, 1 `STRENGTH`, 2 `ROOT_SPEED_CMS`; `COUNT` 3. `RAGPART` floats: 0 `SHAPE`, 1..3 `P0..P2` (the resolved shape numbers), 4 `RADIUS_CM`, 5 `MASS_KG`, 6..8 `CENTRE` (world), 9..12 `ROTATION` (the body's, xyzw), 13..15 `SEG_A`, 16..18 `SEG_B` (world segment ends), 19 `JOINT` kind (-1 for a root part), 20..22 `JOINT_POINT`, 23..25 `JOINT_AXIS` (world), 26..28 `LIMITS` (swing1, swing2, twist; or hinge min, max, 0), 29 `MODE` (effective), 30 `MOTOR_ON`; `COUNT` 31. They exist for the overlay and for tests.

#### 2.4.5 Semantics (the contract the implementation and the tests follow)

- **Mass.** Parts with `MASS_KG > 0` get exactly that. The rest share `totalMassKg - sum(explicit)` by volume (the v1 formula for capsules and spheres, `Shape::GetVolume()` for the new kinds). If the explicit masses reach `totalMassKg` and some part has none, the builder refuses (`InvalidArgument`); if every part is explicit the total is their sum. `STABILIZE` runs after, on the resulting masses.
- **Per-part material.** `FRICTION`, `RESTITUTION`, the two dampings are written to the part's `BodyCreationSettings` (`mFriction`, `mRestitution`, `mLinearDamping`, `mAngularDamping`); the v1 build writes only the angular one, so the defaults above reproduce it.
- **Self-collision.** With `SELF_COLLISION = 1` the `GroupFilterTable` is the one Jolt's `DisableParentChildCollisions` creates; its `Ref` is kept in the entry (the object is owned by the settings and shared with every part, so toggling it on a live ragdoll changes the next collision test). Toggling happens between steps only. With `0` the table is the v1 one (every pair off).
- **Partial modes.** Each part has an effective mode (its own `set_part_mode` value, or the ragdoll's). A part is **kinematic** (body motion type Kinematic; `Stepper` moves it by `MoveKinematic` to the target bone) or **dynamic** (limp, powered). A joint is enabled unless both its parts are kinematic; a joint's motor is on only when its part is POWERED, and driven per joint kind by Aver's own loop (swing-twist: `SetSwingMotorState / SetTwistMotorState(Position)` + `SetTargetOrientationBS`; hinge: `SetMotorState(Position)` + `SetTargetOrientationBS`; fixed and point joints have no motor and are skipped, which is why Jolt's `DriveToPoseUsingMotors` is not used). Kinematic to dynamic keeps the velocity `MoveKinematic` gave (momentum into the fall), as the whole-ragdoll switch does today. A **hit reaction** is: upper-body group POWERED, legs KINEMATIC, then a limp pulse on the struck group.
- **Motor numbers per joint:** frequency `MOTOR_BASE_HZ + MOTOR_HZ_PER_STRENGTH x s x MOTOR_FREQ_SCALE`, damping `MOTOR_DAMPING`, torque limit `hold x MOTOR_TORQUE_AT_FULL x s x MOTOR_TORQUE_SCALE`, with `s` the part's group strength if set else the ragdoll's. With the defaults this is `2 + 8 s`, 1 and `hold x 20 x s`: v1.
- **Extra constraints** follow the same enable rule (off while both parts are kinematic).
- **`get_pose` with the mapper** (`MAPPER` 1 or 2): the reduced skeleton (parts) and the animation skeleton (every bone) are two `JPH::Skeleton`s whose joints are named `b<bone>` so Jolt's default name mapper pairs them; neutral poses are the creation pose (world matrices); `SkeletonMapper::Initialize` runs once at finish. Each `get_pose` builds the parts' world matrices from the bodies and the animation skeleton's **local** target pose, calls `Map(parts, targetLocal, out)` and converts `out` to the per-bone arrays. Chains between two mapped joints are reoriented toward the next one (a spine with three helper bones bends); joints beyond the first or last mapped one follow their local animation; `MAPPER = 2` also calls `LockAllTranslations`. A bone with neither part nor ancestor part is untouched, as in v1. UNVERIFIED: how it looks under extreme poses (Jolt says "very extreme animation poses will show artifacts"); headed check 8.
- **Bounds** convert the Jolt AABox corners with `fromJolt` and re-sort per axis (the axis permutation negates one). **Root** is the first part's body transform.
- **Teardown** is unchanged: `destroy`, `remove_body` on a part, shutdown. Extra constraints and the kept filter `Ref` die with the entry.

### 2.5 The ragdoll system, `.ocragdoll`, the relay and C#

#### 2.5.1 `.ocragdoll` directives (additive)

The file stays plain text (`mass`, `bone ...`); a malformed line still discards the file as a whole. New directives, names as everywhere are the `.ocskel` bone names without spaces, unknown names ignored (the file is shared between similar rigs):

```
stabilize on|off                              # RAGOPT 0
selfcollide off|on [minsep <cm>]              # RAGOPT 1, 2
group <id>                                    # RAGOPT 3 (a shared user group)
motor base <hz> perstrength <hz> torque <k>   # RAGOPT 4..6
ccd on|off                                    # RAGOPT 7
mapper off|on|lock                            # RAGOPT 8
posegroup <name> <bone> [<bone> ...]          # up to 31 named groups; every bone listed (not its children) joins the group
bone <name> shape box <hx> <hy> <hz> | cylinder [<halfheight>] | tapered <radiusB> | capsule
bone <name> mass <kg> friction <f> restitution <r> damping <lin> <ang>
bone <name> joint cone|pyramid|fixed|point | hinge [axis <x> <y> <z>] [range <minDeg> <maxDeg>]
bone <name> motor <freqScale> <damping> <torqueScale> [friction <s>]
extra distance <boneA> <boneB> <min> <max>    # anchored at the two bones' origins in the creation pose; min = max = -1 keeps the distance
extra point|fixed <boneA> <boneB>             # at boneB's origin
extra hinge <boneA> <boneB> axis <x> <y> <z> range <minDeg> <maxDeg>
extra cone <boneA> <boneB> axis <x> <y> <z> halfangle <deg>
```

Later lines for the same bone win (as today); the bone-level words may share a line. `RagdollOverrides` and `RagdollDef` grow the matching fields (`std::vector<f32> boneExt` with the RAGBONE stride, `f32 options[9]`, named pose groups, extras); `defFor` caches them with the definition. A definition with no new directive builds through `aver_phys_ragdoll_create` exactly as today (a test compares the two paths).

#### 2.5.2 `RagdollSystem`

```cpp
struct StartOptions {
  RagdollMode mode = RagdollMode::Limp;  f32 blendSeconds = 0.1f;
  bool inheritVelocity = false;     // add the entity's physics character velocity (own + inherited) to every part at the build
  bool disableCharacter = false;    // aver_phys_character_set_enabled(0) on the entity's character; stop() enables it again (it is not moved)
  i32  collisionGroup = 0;          // RAGOPT 3 for this ragdoll, 0 = the definition's
};
bool start(scene::Entity, const StartOptions&);                       // start(e, mode, blend) calls it with the defaults above: behaviour unchanged
bool setGroupMode(scene::Entity, u64 poseGroupHash, i32 mode);         // 0 hash = every part; -1 follows
bool setGroupStrength(scene::Entity, u64 poseGroupHash, f32 strength);
bool setMotor(scene::Entity, u64 boneHash, f32 freqScale, f32 damping, f32 torqueScale);   // 0 hash = every joint
bool setPairCollision(scene::Entity, u64 boneHashA, u64 boneHashB, bool on);               // (0,0) = every pair
bool setVelocity(scene::Entity, const Vec3&, bool additive);
bool bounds(scene::Entity, Vec3& mn, Vec3& mx) const;    bool root(scene::Entity, Vec3& pos, Quat& rot) const;
bool setGroupId(scene::Entity, i32 group);               bool info(scene::Entity, Info&) const;   // Info: mode, weight, parts, active, group, mass
// and, beside providerStart (a free function, not a member), the relay below:
// i32 providerExt(i32 op, i32 entity, const f32* in, i32 nIn, const char* text, f32* outF, i32* outI, i32 cap, void* user);
```

`build` uses the builder when the definition has anything beyond v1 and `aver_phys_ragdoll_create` otherwise. Pose-group names are hashed with `fnv1a64` like bone names. A pending group mode set before the physics ragdoll exists (the build is lazy: the first pose the animation seam sees builds it) is stored in `State` and applied after the build. `tick` applies nothing new.

#### 2.5.3 Relay: `framework_ragdoll_ext_abi.h` (new, `aver_fw_ragdoll_ext_*`)

The existing ragdoll provider is five typed function pointers in `framework_abi.h`, which is off limits, so the new calls use the vehicle relay's pattern: own version, one provider function, no provider answers 0.

```c
#define AVER_FW_RAGDOLL_EXT_ABI_VERSION 1
typedef int32_t (AVER_FW_CALL* aver_fw_ragdoll_ext_fn)(int32_t op, int32_t entity, const float* in, int32_t inCount, const char* text,
                                                       float* outF, int32_t* outI, int32_t cap, void* user);
AVER_FW_ABI int32_t aver_fw_set_ragdoll_ext_provider(aver_fw_ragdoll_ext_fn fn, void* user);
AVER_FW_ABI int32_t aver_fw_ragdoll_ext_abi_version(void);
AVER_FW_ABI int32_t aver_fw_ragdoll_ext_op(int32_t op, int32_t entity, const float* in, int32_t inCount, const char* text,
                                           float* outF, int32_t* outI, int32_t cap);      /* the provider's answer, 0 with none */
/* Ops (integers travel as the bit pattern of an int32 in a float slot, the vehicle relay's rule):
   1 START_EX   in: mode(i), blend, flags(i: bit0 inherit velocity, bit1 disable character), group(i)
   2 GROUP_MODE text = pose group ("" = all); in: mode(i)
   3 GROUP_STRENGTH text = pose group; in: strength
   4 MOTOR      text = bone ("" = all); in: freqScale, damping, torqueScale
   5 PAIR_COLLISION text = "boneA|boneB" ("" = every pair); in: on(i)
   6 VELOCITY   in: x, y, z, additive(i)
   7 BOUNDS     outF[6]
   8 ROOT       outF[7] (pos xyz, rot xyzw)
   9 GROUP_ID   in: group(i)
  10 INFO       outI: mode, parts, active, group; outF: weight, mass, speed */
```

Files: `modules/framework/include/aver/framework/framework_ragdoll_ext_abi.h` and `modules/framework/src/RagdollExtAbi.cpp` (new); `modules/ragdoll/src/RagdollExtProvider.cpp` (new) holds `providerExt`. `CharacterWiring.hpp::installRagdollExtProvider()` installs it, called from `registerGameSystems()`; it needs no `install` of the ragdoll system first, because every op answers 0 until the system exists.

#### 2.5.4 C#

`Ragdoll` (Aver.Physics) becomes `partial`; `RagdollExt.cs` adds `RagdollBuilder` (a class holding the arrays and blocks, `Build()` returns a `Ragdoll`), `RagdollBoneExt` (a record struct with the RAGBONE fields and their defaults), `RagdollOptions`, `RagdollConstraint`, and on `Ragdoll`: `SetPartMode(int poseGroup, RagdollMode? mode)`, `PartMode(int bone)`, `SetGroupStrength`, `SetMotor`, `SetPartCollision(a, b, bool)`, `PartCollision`, `GroupId { get; set; }`, `SetVelocity(Float3 linear, Float3 angular)`, `AddVelocity`, `Bounds`, `Root`, `Info`, `PartInfo(bone)`, `static int List(Span<Ragdoll>)`. `Native.RagdollExt.cs` holds the P/Invokes.

`Aver.Framework` (`RagdollExt.cs`, `RagdollExtNative.cs` with `FwRagdollExt`, `Lib = "Aver.Framework"`):

```csharp
public readonly record struct RagdollStartOptions(RagdollMode Mode = RagdollMode.Limp, float BlendSeconds = 0.1f, bool InheritVelocity = true,
                                                  bool DisableCharacter = true, int CollisionGroup = 0);
public readonly partial struct Entity
{
    /// <summary>As StartRagdoll(mode, blend), plus the character hand-off: the capsule's velocity goes into the parts and the capsule is parked.</summary>
    public bool StartRagdoll(in RagdollStartOptions options);
    public bool SetRagdollGroupMode(string poseGroup, RagdollMode? mode);          // null follows the ragdoll; "" is every part
    public bool SetRagdollGroupStrength(string poseGroup, float strength);
    public bool SetRagdollMotor(string bone, float frequencyScale = 1, float damping = 1, float torqueScale = 1);   // "" is every joint
    public bool SetRagdollSelfCollision(bool on);                                  // every pair
    public bool SetRagdollPairCollision(string boneA, string boneB, bool on);
    public bool SetRagdollVelocity(Vec3 velocity, bool additive = false);
    public bool TryGetRagdollBounds(out Vec3 min, out Vec3 max);   public bool TryGetRagdollRoot(out Vec3 position, out Quat rotation);
    public bool SetRagdollCollisionGroup(int group);
    public bool TryGetRagdollInfo(out RagdollInfo info);          // Mode, BlendWeight, Parts, Active, Group, MassKg, SpeedCmS
}
```

The old four members keep their exact behaviour; `StartRagdoll(mode, blend)` is the defaults-off call, so no existing script changes.

### 2.6 Vehicle physics ABI: `physics_vehicle_adv_abi.h` and the motorcycle

`physics_vehicle_ext_abi.h` gains one define, `AVER_PHYS_VEH_KIND_MOTORCYCLE 2`, and `aver_phys_vehicle_set_kind` (in `PhysicsVehicleTracked.cpp:177`) accepts it. Everything else is the new header `physics_vehicle_adv_abi.h` with sources `PhysicsVehicleAdv.cpp` (the calls and the tyre callback) and `PhysicsVehicleMoto.cpp` (the motorcycle controller, lean state), and a src-local `PhysicsVehicleAdv.hpp` defining `VehicleAdvData` (one member `adv` added to `VehicleEntry`; `PhysicsInternal.hpp` includes the header). Wave 10's rules apply unchanged: engine units, parameter blocks that may be shorter and are refused when longer, builder calls refused after `_finish` and live calls usable on both sides unless stated, defaults are Jolt's so an untouched vehicle builds the same objects it does today.

```c
#define AVER_PHYS_VEHICLE_ADV_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_vehicle_adv_abi_version(void);     /* 1 */

/* ---- before the first add_wheel ---------------------------------------------------------------------------- */
AVER_PHYS_API int32_t aver_phys_vehicle_set_wheel_cap(int32_t v, int32_t maxWheels);                  /* 16..64, default 16 (v1's ceiling) */

/* ---- before _finish ----------------------------------------------------------------------------------------- */
AVER_PHYS_API int32_t aver_phys_vehicle_set_wheel_geometry(int32_t v, int32_t wheel, const float* p, int32_t n);   /* AVER_PHYS_VWG_*, wheel -1 = all */
AVER_PHYS_API int32_t aver_phys_vehicle_set_wheel_spring(int32_t v, int32_t wheel, const float* p, int32_t n);     /* AVER_PHYS_VWP_*, wheel -1 = all */

/* ---- before or after _finish -------------------------------------------------------------------------------- */
AVER_PHYS_API int32_t aver_phys_vehicle_set_wheel_cast(int32_t v, const float* p, int32_t n);                       /* AVER_PHYS_VCT_*: how the wheels find the ground */
AVER_PHYS_API int32_t aver_phys_vehicle_set_motorcycle(int32_t v, const float* p, int32_t n);                       /* AVER_PHYS_VMC_*; kind MOTORCYCLE only */
AVER_PHYS_API int32_t aver_phys_vehicle_set_tyre_model(int32_t v, int32_t wheel, const float* p, int32_t n);        /* AVER_PHYS_VTM_*, wheel -1 = all */
AVER_PHYS_API int32_t aver_phys_vehicle_set_gravity(int32_t v, int32_t enable, float gx, float gy, float gz);       /* cm/s^2; enable 0 resets */
AVER_PHYS_API int32_t aver_phys_vehicle_set_max_pitch_roll(int32_t v, float degrees);                               /* 0 or >= 180 = off */
AVER_PHYS_API int32_t aver_phys_vehicle_set_solver_steps(int32_t v, int32_t velocitySteps, int32_t positionSteps);  /* 0 = the world's; max 255 */
AVER_PHYS_API int32_t aver_phys_vehicle_set_wheel_grip(int32_t v, int32_t wheel, float scale);                      /* wheel -1 = all; 0..4 */

/* ---- after _finish ------------------------------------------------------------------------------------------ */
AVER_PHYS_API int32_t aver_phys_vehicle_set_lean(int32_t v, int32_t key, float value);       /* one AVER_PHYS_VMC_* value, live */
AVER_PHYS_API int32_t aver_phys_vehicle_adv_state(int32_t v, float* outF, int32_t nF);       /* AVER_PHYS_VLS_*; floats written, 0 for a dead or unfinished vehicle */
```

**Blocks** (`index NAME (unit) = default`):

| Block | Indices |
|---|---|
| `VWG` wheel geometry (`AVER_PHYS_VWG_*`) | 0 `CAMBER_DEG` 0 (positive = the wheel's top leans away from the centreline; for a wheel on y = 0, toward +Y); 1 `TOE_DEG` 0 (positive = toe-in: the front of the wheel turns toward the centreline); 2 `CASTER_DEG` 0 (positive = the steering axis' top leans backward, toward -X); 3 `FORK_RAKE_DEG` 0 (tilts the suspension direction and the steering axis together by this angle, the same sense as caster: a motorcycle fork; the steering axis tilts by `rake + caster`). `COUNT` 4 |
| `VWP` wheel spring (`AVER_PHYS_VWP_*`) | 0 `MODE` 0 (0 frequency and damping ratio, v1; 1 stiffness N/m and damping N*s/m; 2 mass-normalised stiffness and damping, Jolt's `MassNormalizedStiffnessAndDamping`); 1 `VALUE` 0 (Hz, N/m or 1/s^2 by mode; 0 keeps the frequency `add_wheel` was given); 2 `DAMPING` -1 (ratio, N*s/m or 1/s by mode; -1 keeps `add_wheel`'s); 3 `PRELOAD_CM` 0 (`mSuspensionPreloadLength`: the spring's natural length is max length + preload, so it is compressed by this much at full droop; a discontinuity at touchdown, Jolt's comment); 4 `FORCE_POINT_ON` 0; 5..7 `FORCE_POINT_CM` 0 (vehicle-origin cm; with the point on and all three zero it is the wheel centre at full droop, `attach + suspensionDir x maxLength`). `COUNT` 8 |
| `VCT` wheel cast (`AVER_PHYS_VCT_*`) | 0 `KIND` 0 (0 cylinder, v1; 1 sphere; 2 ray); 1 `USER_LAYER` 0 (the user layer the cast tests **as**: it hits whatever that layer collides with in the layer matrix; 0 is `Layers::MOVING`, v1); 2 `CONVEX_RADIUS_FRACTION` 0.1 (cylinder); 3 `SPHERE_RADIUS_CM` 0 (0 = half the first wheel's width); 4 `MAX_SLOPE_DEG` 80 (ray and sphere ignore steeper surfaces, so a wall is not ground). `COUNT` 5 |
| `VMC` motorcycle (`AVER_PHYS_VMC_*`) | 0 `MAX_LEAN_DEG` 45; 1 `LEAN_SPRING` 5000 (N*m/rad); 2 `LEAN_DAMPING` 1000 (N*m*s/rad); 3 `LEAN_INTEGRAL` 0 (turns the spring into a PID's I term); 4 `LEAN_INTEGRAL_DECAY` 4 (per second, while the wheels are off the ground); 5 `LEAN_SMOOTHING_60HZ` 0.8 (0 none, 1 frozen; Jolt's smoothing is **per step**, so the stored value is at 60 Hz and the step hook rescales it to `f^(dt x 60)` for the fixed step in force); 6 `LEAN_ENABLED` 1 (0 lets the bike fall over); 7 `STEER_LIMIT_ENABLED` 1 (Jolt's speed-dependent steering limit that stops a steer that would topple it). `COUNT` 8 |
| `VTM` tyre model (`AVER_PHYS_VTM_*`) | 0 `ELLIPSE` 0 (0..1 friction-ellipse coupling of longitudinal and lateral grip); 1 `LOAD_SENSITIVITY` 0 (grip falls as load rises above the reference); 2 `REF_LOAD_N` 0 (0 = the vehicle's weight over its wheel count, a flat approximation); 3 `MIN_GRIP` 0.5; 4 `MAX_GRIP` 1.3. `COUNT` 5 |
| `VLS` lean state (`AVER_PHYS_VLS_*`, floats) | 0 `LEAN_DEG` (positive = leaning right); 1 `LEAN_RATE_DEG_S`; 2 `STEER_DEG` (the steered wheel's applied angle, positive = right); 3 `WHEELBASE_CM`; 4 `LEAN_ENABLED`. `COUNT` 5 |

#### 2.6.1 Wheel geometry

The wheel's neutral frame in engine axes is forward `F = (1,0,0)`, up `U = (0,0,1)`, suspension direction `D = (0,0,-1)`, steering axis `S = (0,0,1)` (v1 writes exactly these, `PhysicsVehicle.cpp:149-152`). With `side = sign(y)` (+1 for y >= 0), `r = rake`, `k = caster`, `c = camber`, `t = toe`, the builder composes, in this order and as a contract: **rake** and **caster** are rotations about the lateral axis Y (so `S = (-sin(r+k), 0, cos(r+k))` and, when `r != 0`, `D = (sin r, 0, -cos r)`); **camber** rotates `U` about `F` by `side x c` (so `U = (0, side sin c, cos c)` before the lateral tilt); **toe** rotates `F` about `U` by `-side x t`. The results go to `mSteeringAxis`, `mSuspensionDirection`, `mWheelUp` and `mWheelForward`. All zero is the v1 vectors, bit for bit. `GetWheelLocalTransform` (and so `aver_phys_vehicle_wheel_pose`) already derives the wheel basis from these settings, so a cambered, toed or raked wheel mesh follows with no visual code. **The signs are derived on paper and UNVERIFIED**; the first geometry test pins each against `wheel_pose` and against the steering sign the v1 comment documents (`PhysicsVehicle.cpp:21-25`).

#### 2.6.2 Springs, preload, force point

`buildWheeled` writes `SpringSettings(mode, value, damping)` from `VWP` (v1's `FrequencyAndDamping` when unset), `mSuspensionPreloadLength = cmToM(preload)`, and `mEnableSuspensionForcePoint` with `mSuspensionForcePoint = toJolt(point)`. `SUSPENSION_STIFFNESS_SCALE` (`applySuspensionScale`) keeps meaning "stiffer by this factor": it multiplies the frequency in mode 0 and the stiffness by `scale^2` in modes 1 and 2 (frequency goes as the square root of stiffness), so one tune key works for every mode. Aver.Vehicle's settled-height formula needs the spring's static sag; in stiffness modes it uses `sag = m g / k` for the wheel's share of the weight (section 2.7.2). The force point stays off for cars (Jolt: less accurate against dynamic objects, more stable) and is on at the wheel centre for the bike.

#### 2.6.3 Wheel casts

`finish` builds the tester from `VCT` (default: today's cylinder on `Layers::MOVING`) and `set_wheel_cast` swaps it live (`SetVehicleCollisionTester` takes a `RefConst`, and the next step uses it). Ray and sphere need the world up: `toJoltUnit(0,0,1)`, or `-normalize(gravity)` after an override. The cylinder cast is the accurate one against kerbs; the sphere is cheaper and smoother over rubble (the wheel does not catch on a plank edge); the ray is the cheapest and sees through anything narrower than its width, which is why a 40-vehicle traffic LOD may switch far vehicles to it. The layer lets debris or a "road paint" decal layer be invisible to the wheels while still hitting the chassis.

#### 2.6.4 The motorcycle

`set_kind(v, MOTORCYCLE)` before the first `add_wheel`; `finish` then builds a `MotorcycleControllerSettings` (derived from `WheeledVehicleControllerSettings`, so the engine, gearbox, differentials and anti-roll code in `buildWheeled` fill the same members) with the `VMC` block. Rules, each refused with a log line: exactly **two** wheels (a trike is a wheeled vehicle); at least one driven; `VMC` calls only on this kind. The usual consequences of the axle rule apply and are fine: two axles of one wheel each, one differential with one wheel, no anti-roll bar. `maxPitchRollDeg` passed to `finish` is raised to `MAX_LEAN + 20` if it is above 0 and below that (a pitch/roll cone tighter than the lean would fight the lean controller); the bike preset passes 0 (off). A bike that is allowed to fall (`LEAN_ENABLED 0`) lies down, raises `FLIPPED` after 3 s on its side like a car, and `aver_phys_vehicle_flip` stands it up; `STEER_LIMIT_ENABLED` stays on by default because without it a hard steer at speed throws the bike.

`vehicleStepBegin` gets one extra line for a bike: `SetLeanSmoothingFactor(pow(f60, dt x 60))`, plus the live `VMC` copies (`SetLeanSpringConstant`, `SetLeanSpringDamping`, `SetLeanSpringIntegrationCoefficient`, `...Decay`, `EnableLeanController`, `EnableLeanSteeringLimit`) so `set_lean` is live without a rebuild. `vehicleStepEnd` fills `VLS`: lean angle from the body's right vector against the world up (`-asin(right . up)`, positive to the right), its rate from the last step's value, and the front wheel's applied steer angle. The driver aids of wave 10 work on a bike unchanged (steering rate, speed-sensitive steering, ABS, hold brake); TCS reads driven-wheel slip as for a car.

The bike's numbers (all **UNVERIFIED**, recalled from Jolt's own motorcycle sample where they come from there, and to be tuned in the headed pass): 240 kg, wheelbase 150 cm, wheel radius 31 cm and width 10 cm, front fork rake 27 degrees with 30 degrees of steer, front suspension 1.5 Hz and rear 2.0 Hz with travel 8 to 26 cm, 140 N*m engine from 1200 to 9500 rpm, six forward gears 2.27 1.63 1.30 1.09 0.96 0.88, final drive 4.8, `MAX_LEAN` 45, lean spring 5000 and damping 1000, smoothing 0.8, force points at the wheel centres. Jolt itself calls the controller "still in development ... may need a lot of tweaks/hacks to work properly" (`MotorcycleController.h`), and the doc says so to the owner (section 7).

#### 2.6.5 Gravity override, up, live limits, solver steps

- `set_gravity(v, 1, gx, gy, gz)` calls `VehicleConstraint::OverrideGravity(toJoltDir(g))`. Jolt then sets the chassis' gravity factor to 0 and applies the override in the constraint's own step listener, so buoyancy, aero and every other force still add on top; `enable 0` calls `ResetGravityOverride()` (gravity factor back to 1; no vehicle sets another).
- A `worldUp(e)` helper (`-normalize(override or system gravity)`, Jolt Y by default) replaces the hard-coded `bv.up.GetY()` in `stepEndOne` (`PhysicsVehicleExt.cpp:394`), the flip limit, the `FLIPPED` test and `aver_phys_vehicle_flip` (which uprights about the **current** world up instead of +Z when an override is set; at the default the arithmetic is unchanged). `VS_UP_Z` keeps its name and now means "dot of the chassis' up with the world up".
- `set_max_pitch_roll` calls `SetMaxPitchRollAngle` live and updates the entry's copy that the flip threshold reads.
- `set_solver_steps` calls `SetNumVelocityStepsOverride / SetNumPositionStepsOverride` on the constraint (Jolt's own remedy for a car dipping over light rubble, `VehicleConstraint.h:44-58`); 0 is "the world's".

#### 2.6.6 The tyre model and per-wheel grip

`SetTireMaxImpulseCallback` runs inside the solver on worker threads, so nothing a caller supplies can be in it. Aver installs a native lambda **only when a `VTM` block or a wheel grip differs from the default** (an untouched vehicle keeps Jolt's own callback and so its bits). With `g_i` = the wheel's grip scalar (`set_wheel_grip`, written between steps, read here as a plain float), `load = suspensionImpulse / dt`:

```
grip   = g_i x clamp(1 - LOAD_SENSITIVITY x (load / refLoad - 1), MIN_GRIP, MAX_GRIP)        (1 when LOAD_SENSITIVITY is 0)
longMax = longitudinalFriction x suspensionImpulse x grip
latMax  = lateralFriction      x suspensionImpulse x grip
if ELLIPSE > 0:  uLong = min(1, |longSlip| / peakLong), uLat = min(1, |latSlip| / peakLat)
                 longMax x= sqrt(1 - ELLIPSE x uLat^2);  latMax x= sqrt(1 - ELLIPSE x uLong^2)
```

`peakLong` and `peakLat` are the slip values at the maximum of the wheel's own friction curves, read once at `finish`. The friction ellipse is the textbook coupling of the two grips (public vehicle-dynamics literature, no vendor model); it makes a car that is spinning its tyres understeer or oversteer less sharply than the uncoupled Jolt model, which gives full lateral grip at any wheelspin. A script writes per-wheel grip for a wet patch or a punctured tyre with `an_vehiclewheelgrip`. The combine-friction lambda (`VU_GRIP_SCALE`) is untouched and multiplies in series.

#### 2.6.7 Wheel cap and cost

`set_wheel_cap` raises the per-vehicle ceiling (`PhysicsVehicle.cpp:68,359`) to at most 64: a 12-axle crane, a tank with 24 road wheels. Each wheel is one shape cast per fixed step (and the `VehicleWheelLive` and telemetry arrays grow with it), so the cap is also the cost knob: `aver_phys_vehicle_set_collision_period` (wave 10) already spreads the casts. The spec's own `kMaxWheels = 16` becomes `body.wheelCap` (default 16, validate to 64). No timing exists; headed check 13 records it.

### 2.7 `Aver.Vehicle`, `Aver.Vehicle.Scene` and the vehicle content

#### 2.7.1 `.ocvehicle` additions (version stays 1)

```
KIND motorcycle                                  # wheeled | tracked | motorcycle
BODY half 100 28 40 clearance 18 com 0 0 52 mass 240 pitchroll 0 wheelcap 16      # wheelcap is new, 16..64
MOTO maxlean 45 spring 5000 damping 1000 integral 0 decay 4 smoothing 0.8 lean 1 steerlimit 1       # only for KIND motorcycle; keys optional
CAST cylinder|sphere|ray layer 0 convex 0.1 radius 0 slope 80
SOLVER velocity 0 position 0
TYREMODEL ellipse 0 loadsens 0 refload 0 mingrip 0.5 maxgrip 1.3
WHEELCLASS front radius 31 width 10 susp 8 26 hz 1.5 damping 0.5 brake 900 handbrake 0 inertia 0.9 spindamping 0.2 tyre street \
           springmode freq|stiffness|massnorm  stiffness 0  sdamping -1  preload 0
WHEEL F class front hub 75 0 31 steer 30 driven 0 camber 0 toe 0 caster 0 rake 27 forcepoint 75 0 31 node WheelF   # the new keys are optional
```

(`\` above only wraps the example; records are one line.) `springmode` with `stiffness` and `sdamping` replaces `hz` and `damping` for that class; `forcepoint` is a hub-like origin-relative point (the settled centre is the usual value; `forcepoint auto` = the hub). `CAST`, `SOLVER`, `TYREMODEL` and `MOTO` are written only when they differ from the defaults, and `WHEEL` keys only when non-zero, so the five existing built-in specs and the template's Sedan and Tank round-trip **byte-identically** (a test). An older engine reads a file with these records by ignoring them (unknown records are ignored), and rejects `KIND motorcycle` with its existing error; the new `wheelcap` key is ignored with the existing one-line warning. `validate` adds: motorcycle needs exactly two wheels and a `driven` one; `wheelcap` 16..64 and at least the wheel count; `MOTO` ranges (`maxlean` 5..80, `smoothing` 0..1); `CAST` kind and layer 0..15; `springmode` consistent with its values; camber, toe, caster and rake within -30..30 (rake -10..50); `SOLVER` 0..255; `TYREMODEL` ranges.

#### 2.7.2 Spec and builder

`Kind::Motorcycle`; `BodyDef::wheelCap`; `MotoDef`, `CastDef`, `SolverDef`, `TyreModelDef` on `VehicleSpec`; `WheelClass` gains `springMode`, `stiffness`, `sdamping`, `preload`; `WheelDef` gains `camber`, `toe`, `caster`, `rake`, `hasForcePoint` and `forcePoint`. `buildVehicle` keeps its order (v1 calls first, v2 only for non-defaults) and adds, in this order: `set_kind(MOTORCYCLE)` (with TRACKED today), `set_wheel_cap` before the first wheel when above 16, per wheel `set_wheel_geometry` and `set_wheel_spring` when any value is non-default, `set_motorcycle`, `set_wheel_cast`, `set_solver_steps`, `set_tyre_model`, then `finish`. The attachment point generalises to `attach = hub - D x (suspMax - sag)` with `D` the (possibly raked) suspension direction; for `D = (0,0,-1)` that is today's `hub.z + suspMax - sag`, so the five traffic presets issue the same v1 arguments (the regression test of wave 10 still compares them). `sag` is `g / (2 pi hz)^2` in frequency mode and `m_wheel g / k` (with `m_wheel = mass / wheelCount`) in stiffness modes. A spec for a bike sets the force point to the hub unless it says otherwise.

#### 2.7.3 Presets and the sample bike

`builtinSpec("bike")` (fixed geometry, the numbers of 2.6.4) beside `sedan` and `tank`; `specFromBounds("bike", size)` derives wheel positions from a mesh's bounds (wheels at the two ends of the long axis, radius from the height) so a placed mesh can be made a bike without a spec; `VehiclePreset.Motorcycle` and `CVehicle.preset == 6`. `Vehicles/Motorbike.ocvehicle` is the template's file and the same vehicle the engine embeds (the tests compare the two, as for the Sedan). Traffic (`world::VehicleSystem`) keeps skipping non-wheeled specs (`VehicleSystem.cpp:702`); lane-following bikes are not in this wave.

#### 2.7.4 `VehiclePhysicsSystem`, the relay, `VehicleDriver`

The system already handles any kind through `buildVehicle`; the changes are small: `VehicleVisuals` needs nothing for a bike (two child wheels; the lean is the body rotation the pose already carries); `Telemetry` gains `LeanDeg`, `LeanRateDegS`, `SteerDeg` read with `aver_phys_vehicle_adv_state` once per frame for a motorcycle; the driver seat of a bike has the `SEAT driver` record as any vehicle. **Relay** `framework_vehicle_adv_abi.h` (new, `AVER_FW_VEHICLE_ADV_ABI_VERSION 1`) adds one generic call over the existing provider, defined in `VehicleAbi.cpp` where the provider pointer lives, and the new ops are numbered above the existing 1..13 so the provider's switch grows without a break:

```c
AVER_FW_ABI int32_t aver_fw_vehicle_op(int32_t op, int32_t entity, const float* in, int32_t inCount, const char* text, float* outF, int32_t* outI, int32_t cap);
/* 20 SET_GRAVITY in: enable(i), gx, gy, gz     21 SET_LEAN text = key (lean, steerlimit, maxlean, spring, damping, smoothing) in: value
   22 LEAN_STATE outF[5] (the VLS block)         23 WHEEL_GRIP in: wheel(i), scale    24 MAX_PITCH_ROLL in: degrees    25 SOLVER in: velocity(i), position(i) */
```

`VehicleDriver` changes as in 2.3.4.

#### 2.7.5 C#

`Aver.Physics.Vehicle` is already `partial`; `VehicleAdv.cs` and `Native.VehicleAdv.cs` add `SetWheelCap`, `SetWheelGeometry(wheel, WheelGeometry)`, `SetWheelSpring(wheel, WheelSpring)`, `SetWheelCast(WheelCast)`, `SetMotorcycle(MotorcycleSettings)`, `SetTyreModel(wheel, TyreModel)`, `OverrideGravity(Float3? gravity)`, `SetMaxPitchRoll(float degrees)`, `SetSolverSteps(int velocity, int position)`, `SetWheelGrip(int wheel, float scale)`, `SetLean(MotorcycleKey, float)`, `TryGetLeanState(out LeanState)`, plus the records `WheelGeometry(Camber, Toe, Caster, ForkRake)`, `WheelSpring(Mode, Value, Damping, PreloadCm, ForcePoint?)`, `WheelCast(Kind, UserLayer, ConvexRadiusFraction, SphereRadiusCm, MaxSlopeDeg)`, `MotorcycleSettings(...)` and `TyreModel(...)` with the block defaults. `VehicleKind` gains `Motorcycle = 2` (and its parity row). `AbiVersion` stays the vehicle v2 reading; a new `AdvAbiVersion` reads the new header. `Aver.Framework`: `VehiclePreset.Motorcycle`; `VehicleNative.cs` (`FwVehicle`) gains `aver_fw_vehicle_op`; `EntityVehicleAdv.cs` adds `TryGetMotorcycleTelemetry(out MotorcycleTelemetry)` (a **new** record, because `VehicleTelemetry` is positional and growing it would break callers), `SetVehicleGravity(Vec3?)`, `SetVehicleMaxPitchRoll`, `SetVehicleWheelGrip(int, float)`, `SetVehicleLean(bool enabled)`; `Vehicles.Spawn("bike")` works through the existing preset name path. `Vehicles.CreateCar` and the six old rows are untouched.

#### 2.7.6 Content

- `templates/Vehicle`: `Vehicles/Motorbike.ocvehicle`, `Meshes/Motorbike.ocmesh` (body) and `Meshes/MotorbikeWheel.ocmesh`, `Source/Motorbike.obj`, `Source/MotorbikeWheel.obj`, a bike placed in `Maps/Default.ocworld` with its two wheel children, the generator (`make_vehicle_template.py`, `vehicle_geo.py`) extended, `Content/README.md` gains a bike paragraph and the honest sentence about the controller's maturity. The Vehicle template needs no new input: `Drive.*` already covers throttle, brake, steer and handbrake.
- `test-content/VehicleLab`: a bike on the ice-split lane and the ramp, a "lean off" switch graph (`AN_BikeCrash`) that calls `an_vehicleleancontrol`, a gravity-override loop-the-loop wall strip for `an_vehiclegravity`; `make_vehicle_lab.py` extended, cooked content committed as before.

### 2.8 Aver Node: three families

Wave 9 C contract, exactly (`docs/plans/wave9/VISUAL_SCRIPTING_CSHARP_SYNC.md` section 2.2): one `Graph<Family>NodeTable.cs` per family holding a `<Family>Table()` of `Reg(...)` rows (found by reflection), one palette mirror `sandbox/src/GraphNodeDefs<Family>.hpp` pin for pin (pulled in by `cmake/GraphNodeFamilies.cmake`), one `[AutoRun]` test class built from `NodeRowKit`. Owner methods are `internal static` thin wrappers (`CharacterGraph`, `RagdollGraph`, `VehicleAdvGraph` in `Aver.Framework`) over the public C#; a failure returns false / 0 and logs once per node id, never throws. Entities are ints, strings are attributes, enums are ints documented in the generated `docs/nodes/<Family>.md` (`scripts/node-catalog.py --write`). Nothing is edited in `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs` or `Program.cs`. Every row below is a **new** key; no existing row changes.

**Family `Character`** (category `Character`; `GraphCharacterNodeTable.cs`, `GraphNodeDefsCharacter.hpp`, `CharacterNodeTests.cs`). Entities are the entity a character's capsule is stamped with (`aver_phys_character_of_entity`).

| Key | Kind | Pins besides `exec` / `then` | Attr | Wraps |
|---|---|---|---|---|
| `an_charactersetting` | Exec | in `entity:i`, `value`; out `success:b` | `key` (slope, stepup, stepdown, mass, strength, padding, recovery, predictive, maxhits, gravity, supportheight, steepcancel, enabled, layer, innerlayer) | `TuneCharacter` |
| `an_characterup` | Exec | in `entity:i`, `x`, `y`, `z`, `align:b=true`; out `success:b` | | `SetCharacterUp` |
| `an_charactergravity` | Exec | in `entity:i`, `mode:i` (0 world, 1 along up, 2 vector), `x`, `y`, `z`; out `success:b` | | `SetCharacterGravity` |
| `an_charactersetshape` | Exec | in `entity:i`, `kind:i` (0 capsule, 1 sphere, 2 box, 3 cylinder, 4 tapered), `a`, `b`, `c`, `maxPenetrationCm=1`; out `success:b` | | `SetCharacterShape` (a, b, c are the kind's parameters in order: capsule radius, height; box hx, hy, hz; ...) |
| `an_charactercanshape` | Pure | in `entity:i`, `kind:i`, `a`, `b`, `c`; out `found:b`, `canSet:b` | | `CanSetCharacterShape` |
| `an_characterground` | Pure | in `entity:i`; out `found:b`, `state:i`, `steep:b`, `normalX`, `normalY`, `normalZ`, `slopeDeg`, `groundEntity:i`, `material:i`, `landSpeedCmS`, `airborneSeconds`, `traction` | | `TryGetCharacterGround` |
| `an_charactermotor` | Exec | in `entity:i`, `wishX`, `wishY`, `wishZ`, `accelGround=2400`, `accelAir=600`, `brake=2800`; out `success:b` | | `SetCharacterMotor` + `SetCharacterWish` (`accelGround <= 0` turns the motor off) |
| `an_characterprofileset` | Exec | in `entity:i`, `traction=1`, `slide=1`, `beltX`, `beltY`, `beltZ`, `canPush:b=true`, `canReceiveImpulses:b=true`, `ghost:b`, `events:b`; out `success:b` | | `SetCharacterProfile` |
| `an_characterprofileclear` | Exec | in `entity:i`; out `success:b` | | `ClearCharacterProfile` |
| `an_charactertouching` | Pure | in `entity:i`, `other:i`; out `found:b`, `touching:b`, `count:i` | | `IsTouchingEntity` |

Events reach graphs through the named-event bus (`AN_OnEvent`) with the character's entity as sender: `CharacterLanded`, `CharacterLeftGround`, `CharacterSliding`, `CharacterHeadBump`, and `CharacterTouched` / `CharacterUntouched` (target = the surface entity) for an object profiled with `events`. The numbers are read with `an_characterground`.

**Family `Ragdoll`** (category `Ragdoll`; `GraphRagdollNodeTable.cs`, `GraphNodeDefsRagdoll.hpp`, `RagdollNodeTests.cs`). The four Core rows (`RagdollStart`, `RagdollStop`, `SetRagdollStrength`, `RagdollImpulse`) stay as they are and keep working beside these.

| Key | Kind | Pins besides `exec` / `then` | Attr | Wraps |
|---|---|---|---|---|
| `an_ragdollstartex` | Exec | in `entity:i`, `mode:i`, `blend=0.1`, `inheritVelocity:b=true`, `disableCharacter:b=true`, `group:i`; out `success:b` | | `StartRagdoll(in RagdollStartOptions)` |
| `an_ragdollpartmode` | Exec | in `entity:i`, `mode:i` (-1 follow, 0 kinematic, 1 limp, 2 powered); out `success:b` | `group` (pose group; empty = all) | `SetRagdollGroupMode` |
| `an_ragdollgroupstrength` | Exec | in `entity:i`, `strength`; out `success:b` | `group` | `SetRagdollGroupStrength` |
| `an_ragdollmotor` | Exec | in `entity:i`, `freqScale=1`, `damping=1`, `torqueScale=1`; out `success:b` | `bone` (empty = every joint) | `SetRagdollMotor` |
| `an_ragdollselfcollision` | Exec | in `entity:i`, `enabled:b`; out `success:b` | `boneA?`, `boneB?` (both empty = every pair) | `SetRagdollSelfCollision` / `SetRagdollPairCollision` |
| `an_ragdollvelocity` | Exec | in `entity:i`, `x`, `y`, `z`, `additive:b`; out `success:b` | | `SetRagdollVelocity` |
| `an_ragdollgroupid` | Exec | in `entity:i`, `group:i`; out `success:b` | | `SetRagdollCollisionGroup` |
| `an_ragdollbounds` | Pure | in `entity:i`; out `found:b`, `minX`, `minY`, `minZ`, `maxX`, `maxY`, `maxZ` | | `TryGetRagdollBounds` |
| `an_ragdollroot` | Pure | in `entity:i`; out `found:b`, `x`, `y`, `z`, `speedCmS` | | `TryGetRagdollRoot` |
| `an_ragdollstate` | Pure | in `entity:i`; out `found:b`, `mode:i`, `weight`, `awake:b`, `parts:i` | | `TryGetRagdollInfo` |

**Family `VehicleAdv`** (category `Vehicle`; `GraphVehicleAdvNodeTable.cs`, `GraphNodeDefsVehicleAdv.hpp`, `VehicleAdvNodeTests.cs`). A new family rather than rows appended to `GraphVehicleNodeTable.cs`, so the wave 10 family and its 20 rows stay exactly as shipped.

| Key | Kind | Pins besides `exec` / `then` | Wraps |
|---|---|---|---|
| `an_vehiclegravity` | Exec | in `entity:i`, `enabled:b`, `x`, `y`, `z` (cm/s^2); out `success:b` | `SetVehicleGravity` |
| `an_vehiclelean` | Pure | in `entity:i`; out `found:b`, `leanDeg`, `leanRateDegS`, `steerDeg`, `leanEnabled:b` | `TryGetMotorcycleTelemetry` |
| `an_vehicleleancontrol` | Exec | in `entity:i`, `enabled:b` (false lets the bike fall); out `success:b` | `SetVehicleLean` |
| `an_vehiclewheelgrip` | Exec | in `entity:i`, `wheel:i`, `scale=1`; out `success:b` | `SetVehicleWheelGrip` |
| `an_vehiclemaxpitchroll` | Exec | in `entity:i`, `degrees`; out `success:b` | `SetVehicleMaxPitchRoll` |

Tests per family (`CharacterNodeTests`, `RagdollNodeTests`, `VehicleAdvNodeTests`, all `[AutoRun]`, bare-process scope like `VehicleNodeTests`): `NodeRowKit.CheckRows` against the table above, `CheckDefaultPins`, `CheckIlValid`, `CheckCompilerSplit`; exec rows refused by the pure compiler; pure rows work as reads; every method returns false rather than throws with no provider and no library; the unknown `key` of `an_charactersetting` answers false and logs once. Python: `NodeCatalogCheck` (table to palette parity for all 25 rows) and `CsharpApiIndexCheck`.

### 2.9 Editor

- **`Show > Physics Actors`** (`PhysicsActorsDebugOverlay.{hpp,cpp}`, the `SoftBodyDebugOverlay` pattern: ImGui-free `collect` into `rhi::LineVertex` from the list and inspection ABI, drawn under `!noEditorChrome_`). Characters: the shape outline from `shape_ex` and `shape_offset`, the up arrow, the ground normal and point, the supporting-volume height, the step-up and step-down ticks, each contact's point and normal (red when `CAN_PUSH` is off), disabled characters dimmed, NPC bodies tagged. Ragdolls: every part's shape from `part_info`, each joint's axis and limit fan (swing cone or hinge arc from `LIMITS`), part colour by effective mode (kinematic blue, limp grey, powered green), the pose group as a tint, enabled self-collision pairs as thin lines, the bounds box and the root frame. Sub-toggles and one scale slider, like the soft-body overlay. Both backends draw `createLineMesh` / `drawLines`, which is why this area needs no GPU parity work beyond the headed check on `--backend vulkan`.
- **Relation to WORLD_STATE_DEBUG's `Show > Physics`.** That area draws Jolt's own debug renderer (shapes, constraints, wheels) under `Show > Physics`. This overlay carries what Jolt's renderer does not know: profile colours, motor and pose-group state, limit fans, the ground and contact detail. It sits as a sub-entry of `Show > Physics` when that menu exists, and skips shape outlines while Jolt's are on.
- **`Show > Vehicles`** (wave 10's overlay) gains: the steering axis and caster line per wheel, the force points, the lean arc and target for a bike, the wheel-cast shape (cylinder, sphere, ray) at each wheel.
- **Vehicle Details section** (`VehicleDetailsModel.hpp`, `VehicleDetailsSection.cpp`, wave 10): a bike shows lean, lean rate and steer angle in its live block, and the per-wheel table gains the cast kind and the wheel's grip scalar.
- **`.ocvehicle` tab** (`VehicleEditor`, `VehicleEditorModel`, wave 10): a Motorcycle section when `KIND motorcycle` (lean, spring, damping, smoothing, flags), `CAST`, `SOLVER`, `TYREMODEL` sections, per-wheel camber / toe / caster / rake / force point columns, per-class spring mode, stiffness and preload, `wheelcap`; the validation list is `vehicle::validate`'s, so the new rules show up without UI code.
- **Details.** `AverCharacter`'s new fields (2.3.2) show through the field reflection `MoveSpeed` and `Height` already use; if that reflection needs an attribute, the implementing agent adds it (a headed check confirms). No Add Component row: characters are classes, ragdolls come from skeletal meshes plus `.ocragdoll`, vehicles already have theirs.
- **No new editor for ragdolls.** `.ocragdoll` stays a text file; the overlay is how an author sees the result. A ragdoll authoring tab (draggable capsules and limit gizmos) is a follow-up and not promised here.
- COFF discipline: new editor code lives in its own translation units; `SandboxApp.cpp`, `SandboxRender.cpp`, `SandboxShell.cpp` each get a few lines, and the three `GraphNodeDefs*.hpp` add `push_back`s to the translation unit that holds the whole catalog. The implementing agent records the Debug sandbox object's section count before and after (the cap note in the memory index) and, if it is within 5% of the cap, moves the family headers into a second catalog translation unit before merging.

### 2.10 Host wiring in one place

- **Physics step** (`PhysicsWorld.cpp`, shared, four small edits): the character block `:404-464` is replaced by `characterStepBegin(g_world->fixedStep)`; `characterStepEnd(dt)` after `vehicleStepEnd`; `characterBeginCall()` beside `vehicleBeginCall()`; `releaseNpcOfBody(handle)` in `aver_phys_remove_body` beside `releaseRagdollOfBody`; nothing is added to `aver_phys_shutdown` (the NPCs, the profiles and the events are cleared by `clearCharacterStairSettings`, which it already calls). `PhysicsInternal.hpp` (shared) gains the five declarations, one `adv` member of `VehicleEntry`, `wheelCap`, and `std::map<int32_t, CharacterLive> characterLive` in `World`.
- **Hosts.** Neither host file is edited. `GameTick.hpp` (`tickGameplayGroups`, shared by the editor Play loop, the packaged runtime and `--play-test`) gets `tickCharacterEvents()` after `aver_phys_step`; `GameSystemsWiring.hpp` (`registerGameSystems`, called by both hosts) gets `installRagdollExtProvider()`. `Aver.Vehicle.Scene` and `Aver.Ragdoll` are already wired by both hosts.
- **GPU parity.** No shader, no pass, no RHI call, no binding slot. The one overlay is lines; checked headed on `--backend vulkan` (section 6, item 14).

### 2.11 Determinism and what is claimed

Inputs are the ordered ABI calls and the step; the new state is derived from them in a fixed order (characters and vehicles by handle, ragdoll parts by index, events in the order the loops produce them, no wall clock, no `rand`). On one binary and one instruction set the same input stream reproduces the same poses and events; each feature has a "run twice, compare bits" test. Cross-platform lockstep is not claimed (Jolt is built without `CROSS_PLATFORM_DETERMINISTIC`). The listener lambda and the tyre lambda read plain data written between steps, which is the same rule `onStepBegin` and the combine-friction lambda already follow. **What is not claimed:** that a ragdoll looks right at a mapper's extreme poses, that the bike is stable at every speed, that the friction ellipse matches any real tyre, that the numbers in this document are good; each is UNVERIFIED until section 6.


### 2.12 Seams with the other wave 11 areas

Read from the sibling documents in `docs/plans/wave11/` on 2026-10-19; they are written in parallel, so where a sibling's final signature differs only the small adapter named here changes. Every seam is a **weak hook with a default that means "this area alone"**: a function pointer or an accessor that is null or returns the neutral value until the other area installs it, so neither area waits for the other.

| Area | What it does that touches this one | The contract here |
|---|---|---|
| CONTACTS_FILTERS | Plans its own `CharacterContactListener` for pair, team and group rules (its 2.4.7). A `CharacterVirtual` holds **one** listener pointer, so two classes cannot coexist | There is one class, `detail::CharacterListener` (`PhysicsCharacterStep.cpp`, this area). CONTACTS_FILTERS' rules enter through `detail::setCharacterPairRule(bool (*)(int32_t character, const JPH::BodyID& other, const JPH::SubShapeID& sub))` and `detail::setCharacterCharacterRule(bool (*)(int32_t character, int32_t other))`, null = allow, consulted first by both validate callbacks. Its "conveyors are rigid-body only, a follow-up uses `OnAdjustBodyVelocity`" is done here; one authored conveyor can move crates and characters because `OnAdjustBodyVelocity` also adds `detail::setBodyConveyorFn(Vec3 (*)(const JPH::Body&))`'s answer when installed (profile belts add to it) |
| CONTACTS_FILTERS | Per-body and volume gravity; `aver_phys_body_set_gravity_vector` accepts character handles; `ALIGN_CHARACTER_UP` | The step block asks `detail::setCharacterGravityFn(bool (*)(int32_t handle, const JPH::RVec3& pos, JPH::Vec3 base, JPH::Vec3& outGravity, bool& outAlignUp))` when a character is in mode `WORLD` (the default) and the function is installed; `ALONG_UP` and `VECTOR` do not ask. `outAlignUp` makes the block call `SetUp(-normalize(gravity))` (no rotation, the same as `set_up`). With no volume the function must return `base * factor` evaluated as v1 does, which keeps the default bit-identical. `aver_phys_body_set_gravity_vector` on a character handle is implemented by calling this area's `characterSetGravityVector(handle, on, g)`: one store, `CharacterLive` |
| CONTACTS_FILTERS | Binds every body to a tag by `BodyID` at creation and at each rebuild site | This area calls `contactsBindBody(handle, id)` / `contactsRebind(handle, oldId, newId)` / `contactsForgetBody(handle)` (when they exist) at: NPC create and destroy, each ragdoll part created by the builder, and the character settings rebuild (a new inner body). `set_enabled` and layer changes keep the `BodyID`, so they need none |
| CONTACTS_FILTERS | Posts physics events from `tickGameplayGroups` | `tickCharacterEvents()` is one more line in the same function; the two event streams stay separate (a rigid body hitting a character is already flagged `CHARACTER` in theirs; this area's `CHREV` events are the character's own ground and surface transitions) |
| SHAPES_MATERIALS | Surface ids (32-bit name hash, never 0, 0 = none); `aver_phys_character_ground_detail`, `aver_phys_vehicle_wheel_surface`; `vehicle_set_chassis_shape`; preserves the upper 32 bits of `UserData` for a body label | `MATERIAL` in the ground and contact blocks is that id, filled by `detail::surfaceIdOfMaterial(const JPH::PhysicsMaterial*)` (theirs; default 0). Their `ground_detail` stays and mine stays: theirs is surface-centred (friction, restitution, tag), mine character-centred (state, steepness, detail of the contact), both read the same Jolt fields. This area reads an entity stamp as `static_cast<int32_t>(GetUserData())`, as every reader does. Their two one-line friction edits and the chassis rebuild touch `PhysicsVehicle*.cpp` (shared, 3.3); the `adv` data lives in the `VehicleEntry`, which a chassis rebuild keeps, so nothing is re-applied. Ragdoll `FRICTION` and NPC `FRICTION` are body frictions; a part on a named surface is a follow-up |
| WORLD_STATE_DEBUG | Snapshots with an engine section: accumulator, `VehicleEntry` dynamic fields, `characterCarry` | The new **live** character state is in `World::characterLive` (reachable from `PhysicsInternal.hpp`, so their code adds it without touching this area's `.cpp`): enable flag, wish, gravity mode and vector, previous ground state, airborne seconds, last landing speed. Configuration (settings, stairs, motor numbers, profiles, ragdoll part modes and group strengths, vehicle `adv` blocks) is not captured and is said so; the bike's lean controller state is inside Jolt's `VehicleConstraint::SaveState`. The lean smoothing rescale reads the fixed step in force, so a restored snapshot at another step is the caller's to avoid |
| WORLD_STATE_DEBUG | Profiler scopes, `Show > Physics`, large-world sweep (lands last), `aver_phys_rebuild` | `characterStepBegin` and `characterStepEnd` are bracketed with their `AVER_PHYS_SCOPE("Characters")`; the overlay of 2.9 relates as stated there; the sweep converts the moved block with the rest; the tables here (NPCs, profiles, builders, events) are cleared by `clearCharacterStairSettings`, which their rebuild path also reaches through shutdown. Their determinism scene (a vehicle, a ragdoll, a character) stays golden because every default here is bit-identical |
| QUERIES_JOINTS | Removing a body now removes the joints attached to it | Ragdoll parts, chassis bodies and NPC bodies run their release hooks first (`releaseRagdollOfBody`, `releaseVehicleOfBody`, `releaseNpcOfBody`), the joint cleanup after; the ragdoll's extra constraints are Jolt-internal and are not in the joints table. Its `Pyramid` joint swing and this area's `RAGJOINT_PYRAMID` are independent builds of the same Jolt option |
| SOFTBODY_HAIR | A cape ignores its wearer's ragdoll by group id = ragdoll handle; the cloth character proxy starts a KINEMATIC ragdoll and moves its parts to a layer (`GameSystemsWiring.hpp:168-195`) | The default (no `GROUP_ID`) leaves the group id as the handle; with a user group `RAGINFO.JOLT_GROUP` gives the effective id. The proxy start sequence is a test in `RagdollExtSystemTest`. This area's step hooks sit beside `softbody::onStepBegin` and do not reorder it |

---

## 3. FILE OWNERSHIP

Paths are repository-relative. "Owned" means no other wave 11 area is expected to touch the file (the vehicle, ragdoll and character files of waves 1 to 10 are this area's now). "Shared" means another wave 11 area or the integrator also edits it and every edit here is small and additive; a shared edit must be the minimum that makes the owned files link.

### 3.1 Created (only this area touches them)

**Physics (`modules/physics`)**
- `include/aver/physics/physics_character_ext_abi.h`, `physics_ragdoll_ext_abi.h`, `physics_vehicle_adv_abi.h`
- `src/PhysicsCharacterExt.cpp`, `PhysicsCharacterStep.cpp`, `PhysicsCharacterNpc.cpp`, `PhysicsCharacterState.hpp`
- `src/PhysicsRagdollBuilder.cpp`, `PhysicsRagdollExt.cpp`, `PhysicsRagdollInternal.hpp`
- `src/PhysicsVehicleAdv.cpp`, `PhysicsVehicleAdv.hpp`, `PhysicsVehicleMoto.cpp`

**Framework relays and the modules above physics**
- `modules/framework/include/aver/framework/framework_ragdoll_ext_abi.h`, `framework_vehicle_adv_abi.h`; `modules/framework/src/RagdollExtAbi.cpp`
- `modules/ragdoll/src/RagdollExtProvider.cpp`
- `modules/vehicle/src/VehicleMoto.cpp` (the bike preset)
- `modules/vehicle.scene/src/VehicleAdvOps.cpp`
- `Runtime/include/aver/game/CharacterWiring.hpp`

**Editor**
- `sandbox/src/PhysicsActorsDebugOverlay.hpp`, `PhysicsActorsDebugOverlay.cpp`
- `sandbox/src/GraphNodeDefsCharacter.hpp`, `GraphNodeDefsRagdoll.hpp`, `GraphNodeDefsVehicleAdv.hpp`

**C#**
- `scripting/csharp/Aver.Physics/`: `Native.CharacterExt.cs`, `CharacterBodyExt.cs`, `CharacterExt.Index.cs`, `NpcBody.cs`, `Native.RagdollExt.cs`, `RagdollExt.cs`, `RagdollExt.Index.cs`, `Native.VehicleAdv.cs`, `VehicleAdv.cs`, `VehicleAdv.Index.cs`
- `scripting/csharp/Aver.Framework/`: `CharacterPhysics.cs`, `EntityCharacter.cs`, `CharacterEvents.cs`, `CharacterGraph.cs`, `RagdollExt.cs`, `RagdollExtNative.cs`, `RagdollGraph.cs`, `EntityVehicleAdv.cs`, `VehicleAdvGraph.cs`
- `scripting/csharp/Aver.Graph/`: `GraphCharacterNodeTable.cs`, `GraphRagdollNodeTable.cs`, `GraphVehicleAdvNodeTable.cs`
- `scripting/csharp/Aver.Graph.Tests/` (where wave 10's `VehicleApiTests` and `DrivingContextTests` live): `CharacterNodeTests.cs`, `RagdollNodeTests.cs`, `VehicleAdvNodeTests.cs`, `CharacterApiTests.cs`, `RagdollApiTests.cs`, `VehicleAdvApiTests.cs`

**Tests**
- `tests/physics/src/CharacterAdvTest.cpp`, `CharacterProfileTest.cpp`, `NpcTest.cpp`, `RagdollAdvTest.cpp`, `VehicleAdvTest.cpp`, `MotorcycleTest.cpp`; `tests/physics/ChrRagVeh.cmake` (the six `add_executable` blocks, so the shared `tests/physics/CMakeLists.txt` gets one `include` line)
- `tests/vehicle/src/OcVehicleAdvTest.cpp`, `VehicleBuildAdvTest.cpp`; `tests/vehicle.scene/src/VehicleAdvSystemTest.cpp`
- `tests/ragdoll/src/RagdollExtBuilderTest.cpp`, `RagdollExtSystemTest.cpp`
- `tests/framework_ragdoll_ext/CMakeLists.txt`, `tests/framework_ragdoll_ext/src/FrameworkRagdollExtAbiTest.cpp`
- `tests/game/src/CharacterWiringTest.cpp`; `tests/editor/src/PhysicsActorsOverlayTest.cpp`

**Content**
- `templates/Vehicle/Content/Vehicles/Motorbike.ocvehicle`, `Content/Meshes/Motorbike.ocmesh`, `Content/Meshes/MotorbikeWheel.ocmesh`, `Source/Motorbike.obj`, `Source/MotorbikeWheel.obj`
- `test-content/CharacterLab/` (`Game.ocproject`, `README.md`, `make_character_lab.py`, `Content/` with the level, the graphs, the dummy's `.ocragdoll`); it reuses the skinned character and skeleton of `test-content/ControlRigDemo` by copy, with that asset's licence file

**Generated** (by `scripts/node-catalog.py --write`, never by hand): `docs/nodes/Character.md`, `docs/nodes/Ragdoll.md`, `docs/nodes/VehicleAdv.md`.

### 3.2 Modified, owned by this area

- `modules/physics/src/PhysicsCharacterInternal.hpp` (new hook declarations), `PhysicsVehicleTracked.cpp` (`set_kind` accepts 2). The four other existing physics sources this area edits heavily (`PhysicsCharacter.cpp`, `PhysicsRagdoll.cpp`, `PhysicsVehicle.cpp`, `PhysicsVehicleExt.cpp`) are in 3.3 because CONTACTS_FILTERS and SHAPES_MATERIALS add one-line hooks to them.
- `modules/physics/include/aver/physics/physics_vehicle_ext_abi.h` (one define and its comment)
- `modules/ragdoll/include/aver/ragdoll/RagdollBuilder.hpp`, `RagdollSystem.hpp`; `src/RagdollBuilder.cpp`, `RagdollSystem.cpp`; `CMakeLists.txt`
- `modules/vehicle/include/aver/vehicle/VehicleSpec.hpp`, `VehiclePresets.hpp`; `src/OcVehicle.cpp`, `VehicleBuild.cpp`, `VehiclePresets.cpp`; `CMakeLists.txt`; `README.md`
- `modules/vehicle.scene/include/aver/vehicle/VehiclePhysicsSystem.hpp`; `src/VehiclePhysicsSystem.cpp`, `VehicleDriver.cpp`, `VehicleProvider.cpp`; `CMakeLists.txt`
- `modules/framework/src/VehicleAbi.cpp` (the generic `aver_fw_vehicle_op`)
- `scripting/csharp/Aver.Physics/CharacterBody.cs` and `Ragdoll.cs` (`partial` only), `VehicleExt.cs` (`VehicleKind.Motorcycle`)
- `scripting/csharp/Aver.Framework/Character.cs` (`partial`, the capsule settings hook, the wish call), `EntityVehicle.cs` (`VehiclePreset.Motorcycle`), `VehicleNative.cs` (`aver_fw_vehicle_op`)
- `sandbox/src/VehicleDebugOverlay.*`, `VehicleEditor.*`, `VehicleEditorModel.hpp`, `VehicleDetailsModel.hpp`, `VehicleDetailsSection.cpp`
- `tests/vehicle/CMakeLists.txt`, `tests/vehicle.scene/CMakeLists.txt`, `tests/ragdoll/CMakeLists.txt`
- `templates/Vehicle/make_vehicle_template.py`, `vehicle_geo.py`, `Content/Maps/Default.ocworld` (regenerated), `Content/README.md`; `test-content/VehicleLab/make_vehicle_lab.py`, `README.md`, `Content/Maps/VehicleLab.ocworld` (regenerated)
- `docs/physics/CHARACTER.md`, `docs/physics/RAGDOLL.md`, `docs/physics/RAGDOLL_ANIMATION.md`, `docs/VEHICLES.md`

### 3.3 SHARED (another wave 11 area or the integrator also touches these; every edit is small and additive)

- `CMakeLists.txt`
- `modules/physics/CMakeLists.txt`
- `modules/physics/src/PhysicsInternal.hpp`
- `modules/physics/src/PhysicsWorld.cpp`
- `modules/physics/src/PhysicsCharacter.cpp`
- `modules/physics/src/PhysicsRagdoll.cpp`
- `modules/physics/src/PhysicsVehicle.cpp`
- `modules/physics/src/PhysicsVehicleExt.cpp`
- `modules/framework/CMakeLists.txt`
- `Runtime/include/aver/game/GameTick.hpp`
- `Runtime/include/aver/game/GameSystemsWiring.hpp`
- `sandbox/CMakeLists.txt`
- `sandbox/src/SandboxApp.hpp`
- `sandbox/src/SandboxApp.cpp`
- `sandbox/src/SandboxRender.cpp`
- `sandbox/src/SandboxShell.cpp`
- `tests/abi/src/AbiEnumTest.cpp`
- `tests/physics/CMakeLists.txt`
- `tests/game/CMakeLists.txt`
- `tests/editor/CMakeLists.txt`
- `docs/ABI.md`
- `docs/ARCHITECTURE.md`
- `docs/SCRIPTING_API.md`
- `docs/formats/FORMAT_SPECS.md`
- `CHANGELOG.md`

| Shared file | Edit made by this area | Who else |
|---|---|---|
| `CMakeLists.txt` | `add_subdirectory(tests/framework_ragdoll_ext)` inside the existing framework-test block | every wave 11 area adds its own test directories |
| `modules/physics/CMakeLists.txt` | seven sources: `PhysicsCharacterExt.cpp`, `PhysicsCharacterStep.cpp`, `PhysicsCharacterNpc.cpp`, `PhysicsRagdollBuilder.cpp`, `PhysicsRagdollExt.cpp`, `PhysicsVehicleAdv.cpp`, `PhysicsVehicleMoto.cpp` | SHAPES_MATERIALS, soft body / hair, the large-world sweep add sources |
| `PhysicsInternal.hpp` | `#include "PhysicsVehicleAdv.hpp"` and `PhysicsCharacterState.hpp`; one `VehicleAdvData adv` member and `int32_t wheelCap = 16` in `VehicleEntry`; one `std::map<int32_t, CharacterLive> characterLive` in `World`; declarations `characterBeginCall`, `characterStepBegin`, `characterStepEnd`, `releaseNpcOfBody`, `RagdollEntry* findRagdollEntry` | every area adding hooks or members; the large-world sweep changes position types in the same header |
| `PhysicsWorld.cpp` | the four edits of 2.10 (one replaces the character block, which is the only multi-line change) | soft body / hair (the neighbourhood of `onStepBegin`), the large-world sweep (`RVec3` sites in the character block it replaces) |
| `modules/framework/CMakeLists.txt` | `src/RagdollExtAbi.cpp` | others add their relay sources |
| `GameTick.hpp`, `GameSystemsWiring.hpp` | one line each: `tickCharacterEvents()` after `aver_phys_step` in `tickGameplayGroups`; `installRagdollExtProvider()` in `registerGameSystems()` (no host file is edited) | CONTACTS_FILTERS (its event pump in the same function), every wiring area |
| `PhysicsCharacter.cpp`, `PhysicsRagdoll.cpp`, `PhysicsVehicle.cpp`, `PhysicsVehicleExt.cpp` | the heavy edits of 2.2, 2.4 and 2.6: stair record, hooks, `Entry` moved out, per-part drive, wheel cap, `buildWheeled`, tyre lambda, `worldUp`, lean hook. Keep each hunk local so the others' one-liners merge | CONTACTS_FILTERS (`contactsRebind` / bind lines at the rebuild and creation sites, `characterCreated`), SHAPES_MATERIALS (the leaf-material friction lines `PhysicsVehicle.cpp:512`, `PhysicsVehicleExt.cpp:347`, `vehicle_set_chassis_shape`), the large-world sweep |
| `sandbox/CMakeLists.txt`, `SandboxApp.hpp`, `SandboxApp.cpp`, `SandboxRender.cpp`, `SandboxShell.cpp` | overlay options and frame members; collect and draw; the `Show > Physics Actors` menu entry; the new sources | every overlay |
| `tests/abi/src/AbiEnumTest.cpp` | `headers[12]` (11 headers + terminator, **full**) grows to 32 (the six wave 11 areas together add 17 headers by their documents' counts, so the row needs 29 slots), the three new headers join the "physics ABI" row, and the constant groups of 3.4 are added; `VEH_KIND_MOTORCYCLE` needs no test edit: the existing "vehicle kinds" group is prefix-based and picks it up once `VehicleKind.Motorcycle` exists | every area that adds a physics header (this is the line that will collide first) |
| `tests/physics/CMakeLists.txt`, `tests/game/CMakeLists.txt`, `tests/editor/CMakeLists.txt` | one `include(ChrRagVeh.cmake)` / one `add_executable` each | the other areas' tests |
| `docs/ABI.md`, `docs/ARCHITECTURE.md`, `docs/SCRIPTING_API.md`, `docs/formats/FORMAT_SPECS.md`, `CHANGELOG.md` | rows and sections named in 2.x; the `.ocvehicle` additions and the new `.ocragdoll` directives in `FORMAT_SPECS.md`; written by the integrator | every area |

The `Aver.Framework` version is bumped once for the wave by the integrator.

**Not touched:** the v1 headers `physics_abi.h`, `physics_character_abi.h`, `physics_ragdoll_abi.h`, `physics_vehicle_abi.h`; `Native.cs`; `PhysicsJoints.cpp`, `PhysicsShapes.cpp`, `Convert.hpp` (the large-world sweep owns the position helpers; this area's new code converts positions only with `toJolt` / `fromJolt` so a sweep catches it); `Components.hpp`, `scene_abi.h`, `framework_abi.h`, `modules/rhi*`, every shader, `modules/render*`; `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs`, `Program.cs`; the vendored Jolt tree.

### 3.4 Constant groups the parity test gains

Each is a header prefix against a C# mirror class, the vehicle-block precedent (`AbiEnumTest.cpp:792-817`): `AVER_PHYS_CHRSHAPE_` / `CharacterShapeKind`; `AVER_PHYS_CHRGRAV_` / `CharacterGravityMode`; `AVER_PHYS_CHRSET_` / `ChrSetIndex`; `AVER_PHYS_CHRSTAIR_` / `ChrStairIndex`; `AVER_PHYS_CHRMOTOR_` / `ChrMotorIndex`; `AVER_PHYS_CHRGND_` / `ChrGndIndex`; `AVER_PHYS_CHRGNDF_` / `ChrGndfIndex`; `AVER_PHYS_CHRCON_` / `ChrConIndex`; `AVER_PHYS_CHRCONF_` / `ChrConfIndex`; `AVER_PHYS_CHREV_` / `CharacterEventKind`; `AVER_PHYS_CHRPROF_` / `ChrProfIndex`; `AVER_PHYS_CHRPROFFLAG_` / `CharacterProfileFlags`; `AVER_PHYS_NPC_` / `NpcIndex`; `AVER_PHYS_RAGSHAPE_`, `RAGJOINT_`, `RAGBONE_`, `RAGOPT_`, `RAGCONKIND_`, `RAGCON_`, `RAGINFO_`, `RAGINFOF_`, `RAGPART_` against `RagdollExt.Index.cs`; `AVER_PHYS_VWG_`, `VWP_`, `VCT_`, `VMC_`, `VTM_`, `VLS_` against `VehicleAdv.Index.cs`; each block has a `_COUNT` mirrored by a `Count` member. The prefixes are longer than the vehicle ones on purpose, so no other area's header can collide with them.

---

## 4. Phased implementation

Each phase ends with its tests green and the tree buildable. Sizes: A L, B M, C L, D L, E M, F M, G L, H M. Overall **XL**. Never rebuild over the owner's running editor; headed runs are the owner's. The implementing agents use Sonnet workflows with the fewest stages.

**Phase A: character core (L).** `physics_character_ext_abi.h`; `PhysicsCharacterStep.cpp` holding the moved step block (verbatim) with gravity modes, `AUTO_CANCEL_STEEP` and the layer filters; `PhysicsCharacterExt.cpp` (settings with the rebuild path, up / rotation / align, shapes and offset, ground and contact detail, stairs, steep helpers, enable, list); the four `PhysicsWorld.cpp` edits; C# `Native.CharacterExt.cs`, `CharacterBodyExt.cs`, index mirrors, `AbiEnumTest` rows. Exit: `CharacterTest` and `CharacterExtTest` untouched and green; `CharacterAdvTest` green including the bit-equality-with-v1 test; `AbiEnumTest` green.

**Phase B: character interaction (M).** The listener, profiles, the motor, events (`PhysicsCharacterStep.cpp`); `PhysicsCharacterNpc.cpp`; `CharacterWiring.hpp` and its two one-line calls (`GameTick.hpp`, `GameSystemsWiring.hpp`); `VehicleDriver` switched to `set_enabled`. Exit: `CharacterProfileTest`, `NpcTest`, `CharacterWiringTest` green; a world with a listener installed and no profile is bit-identical to phase A.

**Phase C: ragdoll ABI (L).** `physics_ragdoll_ext_abi.h`, `PhysicsRagdollInternal.hpp` and the refactor of `PhysicsRagdoll.cpp` first (with `RagdollTest` unchanged and green), then the builder, per-part shapes, mass, material, joints, the per-joint motor loop, selective self-collision, partial modes, extra constraints, velocity, bounds, root, group id, inspection, the mapper; C# `Native.RagdollExt.cs`, `RagdollExt.cs`, index mirrors. Exit: `RagdollTest` unchanged and green; `RagdollAdvTest` green including "builder with nothing set is `create`".

**Phase D: vehicle ABI and the motorcycle (L).** `physics_vehicle_adv_abi.h`, `PhysicsVehicleAdv.cpp`, `PhysicsVehicleMoto.cpp`, the touched vehicle sources (`worldUp`, cap, springs, geometry, cast, tyre lambda, solver, gravity, lean hook), `set_kind` accepting 2; C# `Native.VehicleAdv.cs`, `VehicleAdv.cs`. Exit: `VehicleTest`, `VehicleExtTest`, `TrackedVehicleTest` untouched and green; `VehicleAdvTest` and `MotorcycleTest` green.

**Phase E: vehicles above the ABI (M).** `Aver.Vehicle` (kind, records, validation, builder, `VehicleMoto.cpp`), `Aver.Vehicle.Scene` (telemetry, the generic relay op, `VehicleAdvOps.cpp`), the template and lab content with the regenerated maps. Exit: wave 10's `OcVehicleTest`, `VehicleBuildTest`, `VehicleSystemTest`, `FrameworkVehicleAbiTest` untouched and green; `OcVehicleAdvTest`, `VehicleBuildAdvTest`, `VehicleAdvSystemTest` green; `TemplateScaffoldTest` still scaffolds `Vehicle`.

**Phase F: the ragdoll system (M).** `.ocragdoll` directives, `RagdollDef` blocks, `RagdollSystem` additions (start options, group modes, motors, pairs, velocity, bounds, root, group), `framework_ragdoll_ext_abi.h`, `RagdollExtProvider.cpp`, the host install line. Exit: `RagdollBuilderTest` and `RagdollSystemTest` untouched and green; `RagdollExtBuilderTest`, `RagdollExtSystemTest`, `FrameworkRagdollExtAbiTest` green.

**Phase G: C# and Aver Node (L).** `Aver.Framework` (character fields and members, events, ragdoll members and native, vehicle additions), the three node tables and palette mirrors, the node tests, `--write` for the three generated docs. Exit: the three `*NodeTests`, `*ApiTests`, `NodeCatalogCheck`, `CsharpApiIndexCheck`, `DrivingContextTests` green; the six wave 10 vehicle rows and 20-row `VehicleNodeTests` untouched.

**Phase H: editor, labs, docs (M).** `Show > Physics Actors`, the vehicle overlay additions, the `.ocvehicle` tab fields, `test-content/CharacterLab`, the doc edits of 3.2 and 3.3, `docs-check.py`, the Debug sandbox section count recorded. Exit: `PhysicsActorsOverlayTest` green; full `ctest` green.

Order: A before B; C before F; D before E; A to D before G; everything before H.

---

## 5. Tests

Everything below runs without a GPU or a window. `*Test` executables are picked up by the root walk; each phase adds only an `add_executable`. Numbers in assertions are UNVERIFIED margins to be set from the first green run, never loosened to pass without a note.

**`tests/physics` (link `Aver.Physics Aver.Core`)**

- `CharacterAdvTest`
  1. *v1 identity*: one character on a floor with a kerb and a ramp walks a 600-step scripted route; the same route with every new call made at its default (`set_settings` with the default block, `set_stairs` defaults, `set_motor` off, `set_gravity_mode(WORLD)`, `set_up(+Z)`, `set_enabled(1)`) gives bit-identical positions and velocities, and the character handle is unchanged (no rebuild). Run first.
  2. *Orientation*: `up = +X` with mode `ALONG_UP` on a wall-floor box stays supported for 120 steps with the ground normal within 1 degree of up; mode `WORLD` falls off. `align_to_up` puts the shape's +Z on the new up within 0.5 degree and keeps the heading within 1 degree. A planet (a large static sphere) circumnavigated by re-aligning each step stays grounded.
  3. *Shapes*: a box, a sphere and a cylinder character rest at their half height within 1 cm; `set_shape_ex` capsule to box keeps the feet within 1 cm; `can_set_shape_ex` is false under a low ceiling and true after a crouch-sized shape; `set_shape_ex` there is refused and the old shape kept; `set_shape_offset(keepWorldPlace)` leaves the shape's world position within 0.1 cm; the v1 `shape` getter refuses a box while `shape_ex` answers.
  4. *Settings*: padding 2 versus 6 changes the resting gap by 4 cm +/- 1; applying a rebuild-flagged value rebuilds (position, velocity, entity stamp, stairs, gravity factor, mass and strength preserved, handle unchanged, ground state found); `BACK_FACE_MODE` collide versus ignore against a one-sided triangle seen from behind; `MAX_HITS` 4 against a dense mesh sets `MAX_HITS_EXCEEDED`; `PENETRATION_RECOVERY` 0 leaves an embedded start embedded, 1 resolves it.
  5. *Stairs*: a 45 cm step is not climbed at 40 and is at 50; `MAX_CONTACT_ANGLE_DEG` and `FORWARD_TEST_CM` each change the outcome of a purpose-built step; `set_stair_stepping` (v1) and `set_stairs` read back the same record.
  6. *Steep slopes*: `slope_too_steep` is 1 at 60 degrees and 0 at 30, and 0 at 60 after `set_max_slope_angle(70 deg)`; `cancel_steep_velocity` removes the uphill part (dot <= 1e-3); with `AUTO_CANCEL_STEEP` a character driven at a 60 degree slope for 2 s rises less than 2 cm.
  7. *Ground and contact detail*: standing on a box stamped with entity 77 gives `BODY_ENTITY` 77; on a compound, `SUBSHAPE` is the child's; on another character, `CHARACTER` is its handle; `MATERIAL` is 0; `LAST_LAND_SPEED_CMS` after a 2 m drop is within 15% of `sqrt(2 g h)`; `contact_info` of a wall has the wall's normal, entity and `HAD_COLLISION`.
  8. *Layers*: `MOVE_USER_LAYER` 3 passes through a wall on layer 4 when the matrix says they do not collide and stops at one that does; `INNER_USER_LAYER` changes which rays hit the inner body.
  9. *Enable*: a disabled character does not move or fall, a ray through its place hits nothing, another character walks through it; enabling restores all three; `set_position` works while disabled.
- `CharacterProfileTest` (profiles, motor, events)
  1. *Conveyor*: a static box with a 200 cm/s belt carries a still character at 200 +/- 20 cm/s; `ground_velocity` reads it; `clear_profile` stops it; the same through `set_profile_entity`.
  2. *Traction*: motor on, wish 300 cm/s: traction 1 reaches 90% speed in about `0.9 x 300 / 2400` s (+/- 30%), traction 0.05 takes at least 10 times longer, and the braking distance on traction 0.05 is at least 10 times the normal one.
  3. *Push rules*: a 10 kg crate is not displaced by a character pushing for 2 s when `CAN_RECEIVE_IMPULSES` is off and is when on; a kinematic bar swept into a character displaces it with `CAN_PUSH_CHARACTER` on and differs with it off.
  4. *Ghost* profile: the character walks through a `GHOST` box and does not stand on one it falls onto.
  5. *Slide*: pressing diagonally into a wall with `SLIDE` 0 leaves under 5 cm/s tangential speed; with 1 it slides.
  6. *Events*: `LANDED` once per landing with the fall speed; `LEFT_GROUND` on a jump; `SLIDING_BEGIN / END` on a steep ramp; `HEAD_BUMP` under a ceiling; `CONTACT_BEGIN / END` only for an `EVENTS` profile; the queue is empty after the next `aver_phys_step`; a 5000-event flood keeps 4096.
  7. *Identity with a listener*: a world with the listener installed and no profile equals phase A bit for bit; two identical runs with every feature on are bit-equal, events included.
  8. *Seams*: with a fake pair rule installed the validate callbacks obey it (a character passes a body the rule rejects) and with none they do not change anything; a fake gravity function with `outAlignUp` turns `up`; a fake conveyor function moves a standing character; each is removed again by passing null.
- `NpcTest`: an NPC rests on a floor (`OnGround`), walks by `aver_phys_body_set_velocity`, is hit by a ray, is pushed by a falling crate, `ground_info` carries entity and friction; `aver_phys_remove_body` on it releases the `Character`; shutdown with 200 NPCs leaves nothing; `npc_is` is 0 for a plain body.
- `RagdollAdvTest`
  1. *Builder identity*: `builder_begin / finish` with no blocks and `create` with the same arguments, limp-falling 120 steps, give poses within 1e-4 cm; `create` after the refactor equals a recording of the pre-refactor behaviour.
  2. *Shapes*: box, cylinder, tapered, capsule parts rest at their half extents within 1 cm; `part_info` reports the shape numbers.
  3. *Mass*: explicit masses are exact (`part_info`), the rest share the remainder by volume; a total that the explicit masses reach is refused unless every part is explicit; `info` total matches.
  4. *Material*: per-part friction 0.9 against 0.05 slides at different rates on a 20 degree slope; restitution and damping reach the body.
  5. *Joints*: a hinge knee bends only about its axis and within its range (readback of the relative angle), and **bends the way the sign convention says**; a fixed joint holds within 0.2 cm; a point joint swings freely; pyramid and cone differ at the corner of the swing; a hinge with `min > 0` is refused.
  6. *Stabilize*: total mass is preserved within 1e-4 and every parent-child ratio is in [0.8, 1.2] for a lopsided chain; with 0 the masses are untouched.
  7. *Self-collision*: with `SELF_COLLISION 1` two non-adjacent limbs swung into each other are stopped, with 0 they pass; parent-child pairs never collide; `set_part_collision` toggles at runtime and reads back; `GROUP_ID` shared by two ragdolls lets them pass through each other, distinct ids and the default do not.
  8. *Motors*: with torque scale 0 on one joint that limb sags while the others hold; group strength 0 on a group drops it; the hinge and swing-twist motors both hold their target; **a powered ragdoll with fixed and point joints does not assert in the Debug build**.
  9. *Partial*: upper group POWERED and legs KINEMATIC: the legs follow a moving target within 1 cm while the upper body sags less than LIMP; switching a group to LIMP and back; `part_mode` and `info` readbacks; the velocity at a kinematic to dynamic switch is kept.
  10. *Extras*: a `DISTANCE` constraint between two hands never exceeds its max by more than 1 cm; hinge, fixed and cone extras build; they die with the ragdoll; more than 16 are refused.
  11. *Whole-ragdoll*: `set_velocity` and `add_velocity` move every dynamic part and no kinematic one; `bounds` contains every part centre and is within one radius of tight; `root` equals the first part's transform.
  12. *Mapper*: a spine with three helper bones between two parts: the helpers' orientation lies between the parts' (within 10% of the interpolation) with the mapper on, and equals the parent part's with it off; `MAPPER 2` keeps every inter-bone distance within 0.1 cm under a stretch; a mapper ragdoll in KINEMATIC mode reproduces the target pose within 0.2 cm.
  13. *Builder lifecycle*: finish consumes, a second finish is `BadHandle`, cancel frees, 512 bones is the ceiling, shutdown with an open builder leaks nothing; two identical runs are bit-equal.
- `VehicleAdvTest`
  1. *Identity*: the `VehicleTest` car built with every new call at its default drives 300 steps to poses equal to the plain build (bit-identical, tyre callback not installed).
  2. *Wheel cap*: 24 wheels build and drive when the cap is raised, 17 are refused at the default, 65 refused always; the cap call after the first wheel is refused.
  3. *Geometry*: camber, toe, caster and rake appear in `wheel_pose` with the documented signs; a car with toe-in 1 degree per wheel still drives straight within 2 degrees over 5 s; caster changes the steering self-aligning (the wheel returns to straight faster) UNVERIFIED margin.
  4. *Springs*: stiffness mode with the stiffness that matches a frequency settles within 1 cm of the frequency build; preload raises the rest height by about its value / the spring ratio; the force point on leaves the settle within 2 cm; `SUSPENSION_STIFFNESS_SCALE` 2 raises the effective frequency by `sqrt 2` in stiffness mode.
  5. *Casts*: cylinder, sphere and ray all settle within 2 cm on flat ground; the ray falls through a 3 cm gap where the cylinder bridges it; a cast on a layer that does not collide with a debris layer drives through the debris plate without lifting, the default lifts; the cast can be swapped live.
  6. *Gravity*: a car with gravity `+X` rests on a vertical wall, `VS_UP_Z` reads about 1, `flip` uprights it about the wall; reset restores the normal fall; `set_max_pitch_roll` live changes the tip-over limit on a 70 degree bank.
  7. *Solver steps*: accepted, 0 restores the world's, a vehicle with 0 is bit-identical to one never called.
  8. *Tyre model*: `ELLIPSE 1` lowers the lateral acceleration in a full-throttle turn by more than 10% against 0; `LOAD_SENSITIVITY` changes the peak lateral acceleration under downforce; `set_wheel_grip(0.2)` on one wheel yaws the car toward that side; with all defaults the callback is not installed.
  9. *Refusals*: `VMC` on a wheeled vehicle, `set_wheel_geometry` after finish, out-of-range blocks, dead handles.
- `MotorcycleTest`: a bike (2 wheels, rear driven) settles upright (roll under 1 degree after 3 s); accelerates; steering right leans right and the lean grows with speed and shrinks with radius (`|lean|` within 25% of `atan(v^2 / (g R))` for a steady circle); lean never exceeds `MAX_LEAN` + 3; `LEAN_ENABLED 0` makes it fall (`up.z < 0.3` within 3 s) and the `FLIPPED` event fires, `flip` stands it up; `STEER_LIMIT_ENABLED 0` versus 1 at 40 m/s with a hard steer differs; the abs / hold-brake aids work; the builder refuses 1, 3 wheels, no driven wheel, the `VMC` block on a car; two identical runs are bit-equal.

**`tests/vehicle`**
- `OcVehicleAdvTest`: the bike and every new record parse; write then parse is byte-identical for canonical input and **the Sedan, Tank and five presets still round-trip byte for byte**; every `validate` refusal added in 2.7.1; an old-style file (no new records) is unchanged; an unknown key in a new record warns once.
- `VehicleBuildAdvTest`: the bike builds, settles, rides 5 s straight within 1 m, and matches its own `builtinSpec`; a 6x6 with `wheelcap 24`; the raked-fork attach height settles the hub within 1 cm of the stated hub; a stiffness-mode class settles to the same hub height as its frequency twin within 1 cm; `specFromBounds("bike", size)` for three sizes.

**`tests/vehicle.scene`**: `VehicleAdvSystemTest`: a `CVehicle` bike entity (preset 6) enters, rides and exits; the two child wheels receive poses (lean visible in the chassis pose); the relay ops 20 to 25 marshal and act; `Telemetry` carries lean; a driver parked with `set_enabled` leaves the car stable and the character restored on exit; replay equality for a bike script (600 ticks, bit-identical poses, telemetry and events).

**`tests/ragdoll`**: `RagdollExtBuilderTest` (pure): every new `.ocragdoll` directive parses to the matching block; malformed lines discard the file; unknown bones are ignored; named pose groups cap at 31; a file with no new directive yields a def that builds through `create`. `RagdollExtSystemTest` (physics world, no GPU): a def with extras builds through the builder; group mode set before the lazy build applies after it; `start` with `inheritVelocity` gives the parts the character's velocity and `disableCharacter` parks and (on `stop`) re-enables it; partial group modes survive the blend; the mapper ragdoll in KINEMATIC mode keeps the animation pose; the cloth proxy sequence (`start(e, Kinematic, 0)`, then the parts moved to a layer) still works; `setGroupId`, `bounds`, `root` read the physics values; the old `start(e, mode, blend)` behaves as before.

**`tests/framework_ragdoll_ext`**: `FrameworkRagdollExtAbiTest`: version; every call returns 0 and writes nothing with no provider; with a fake provider each op marshals its arguments, `text` and the bit-pattern integers; and the same for `aver_fw_vehicle_op` in `tests/framework_vehicle`.

**`tests/game`**: `CharacterWiringTest`: a fake physics event feed and a fake `aver_fw_event_post` sink: the four posts, sender and target, `CharacterTouched` only for an `EVENTS` profile, nothing posted twice.

**`tests/editor`**: `PhysicsActorsOverlayTest`: `collect` on a scripted world (a box character, a capsule, a three-part ragdoll with a hinge and a fixed joint) yields the expected line counts per toggle and the colour by mode.

**`tests/abi`**: `AbiEnumTest` pairs the three headers with their `Native.*.cs` partials (names and parameter names) and the 3.4 constant groups with their mirrors; `VehicleExtTest` still reads 2 from `aver_phys_vehicle_abi_version()`.

**C# (`Aver.Graph.Tests`, Windows only, ctest `GraphManagedTests`)**: the three `*NodeTests` of 2.8; `CharacterApiTests` (every wrapper returns false / null without the library, `CharacterShape` factories, the settings record defaults equal the header's, `TuneCharacter` unknown key false); `RagdollApiTests` (`RagdollStartOptions` defaults, name-to-hash for groups and bones); `VehicleAdvApiTests` (the records' defaults equal the blocks', `VehiclePreset.Motorcycle`, `TryGetMotorcycleTelemetry` false without a provider). Python: `NodeCatalogCheck`, `CsharpApiIndexCheck`.

---

## 6. Headed manual checks for the owner

Use `test-content/CharacterLab`, `test-content/VehicleLab` and a project from the `Vehicle` template. Editor first, then the packaged runtime, then `--backend vulkan`. The GPU must be idle (the owner plays other games), screenshots at 1440p with a ping for remote runs.

1. **Walk the lab.** The stair flight, the kerb and the ramp behave as before the change (no regression); the crouch bar: a crouch shape swap passes under it and standing up under it is refused; a box-shaped crate character pushes and is pushed.
2. **Planet.** Walking round the small sphere keeps the feet down, the camera follows the up, a jump arcs toward the surface (gravity along up), and stepping onto the flat slab (world gravity mode) falls as expected.
3. **Surfaces.** The conveyor carries the character and a crate the same way; with the motor on, the ice strip is slippery (slow to start, slow to stop) and the mud strip is heavy; the glue floor (`slide 0`) holds; the ghost box lets the character through. `Show > Physics Actors` shows the profile colour of each.
4. **Push rules.** A statue with `canReceiveImpulses` off does not move; a boss with `canPush` off is not shoved by a moving wall; a door of crates behaves normally.
5. **Events.** Falling from 2 m and from 6 m posts `CharacterLanded` with different `landSpeedCmS` (a graph prints them); a ledge post fires `CharacterLeftGround`; the steep ramp fires `CharacterSliding`.
6. **Rigid NPCs.** 200 NPCs walking a loop, then 200 virtual characters: record milliseconds per frame on the profiler's physics row with the GPU idle (none exist today); both groups collide with a pushed crate and take a ray.
7. **Ragdoll hand-off.** The dummy dies mid-run: the capsule's velocity carries into the fall, the capsule is parked (`Show > Physics Actors` dims it), the body tumbles with plausible momentum; a hit reaction (upper body powered, legs kinematic) staggers and recovers; a hinge knee never bends backward; self-collision on lets the arms rest on the torso, off lets them clip.
8. **Mapper.** A spine with helper bones bends smoothly in a limp fall with the mapper on and kinks with it off; in `lock` mode there is no visible stretch; at an extreme pose note artefacts (expected, Jolt says so).
9. **Group ids.** Two dummies in one collision group fall through each other; in two groups they pile.
10. **Bike.** Ride the Motorbike from the Vehicle template: it accelerates through six gears, leans into turns with speed, the front fork rakes visibly over bumps, brakes and holds on a slope; `AN_BikeCrash` (lean off) drops it and the `FLIPPED` event stands it up with `Drive.Reset`; it does not creep at rest; the lean arc in `Show > Vehicles` matches the mesh. Record whether it is stable at top speed and note any tuning (the numbers are UNVERIFIED).
11. **Driver parking.** Enter and exit the sedan and the bike repeatedly: the driver does not jitter or push the car, and reappears at the exit point (the `set_enabled` path). If it does jitter, fall back to wave 10's old path and say so.
12. **Vehicle geometry and springs.** A cambered, toed car shows its wheels tilted and turned; the ice-split lane with a stiffness-mode class feels as the frequency twin; a vehicle with `CAST ray` over a plank field is cheaper and rougher than the cylinder; a car driven up the loop-the-loop wall strip with `an_vehiclegravity` sticks and comes down on the right side.
13. **Cost.** 40 parked vehicles and 8 driven with collision period 1, then with the ray cast on the far ones, and one 24-wheel truck: milliseconds per frame on the Vehicles row.
14. **Backends and hosts.** Repeat 1, 3, 7, 10 on `--backend vulkan` and in the packaged `AverEngineRuntime.exe` (content cooked: the `.ocvehicle`, `.ocragdoll`, graphs). The overlay is editor-only.
15. **Save.** Autosave mid-Play with a ragdoll running and the bike moving: reload, nothing is where it ended.
16. Full `ctest`, the graph suite via `dotnet run`, and open `docs/nodes/Character.md`, `Ragdoll.md`, `VehicleAdv.md` to confirm 25 rows documented without hand edits.

---

## 7. Open questions (each with the default chosen; proceed unless told otherwise)

1. **Large-world coordinates** (another area's work: WORLD_STATE_DEBUG phase H, which lands last, after every other wave 11 area has merged). This area's new code converts positions only through `toJolt` / `fromJolt` and holds no `RVec3` of its own beyond what Jolt returns; the character block it moves contains `RVec3` sites that sweep converts with the rest. Default: this area lands without waiting and the sweep follows.
2. **SHAPES_MATERIALS.** `MATERIAL` ids in the ground and contact blocks are 0 until that area exposes `materialIdOf(const JPH::PhysicsMaterial*)` in `PhysicsInternal.hpp`; one line then fills the field and the nodes' `material` pin starts to mean something. Default: ship with 0 and say so in the node's tooltip.
3. **Character events carry no payload to graphs** (the named-event seam from C++ has none). Default: numbers are read from `an_characterground`. A payload-carrying post would be a framework event ABI change and is not proposed.
4. **Motor off by default.** `AverCharacter.Acceleration` 0 keeps today's direct-velocity feel for every existing project. Default: off; the Character lab and the planet sample turn it on.
5. **Rigid NPCs are `up` = +Z only** (their allowed DOFs are fixed in Jolt axes). Default: documented; a planet NPC uses a virtual character.
6. **`SLIDE` below 1 is a feel knob** (a glue floor is a legitimate gameplay surface but also a stuck-character generator). Default: allowed from 0 to 1, a negative value is refused; a sticky floor is the author's problem to place carefully.
7. **Stabilize flattens mass ratios.** Default: off, and an explicit `MASS_KG` is overwritten when it is on (logged once).
8. **Self-collision default stays "none"** so every shipped ragdoll behaves as before. Default: `selfcollide off` unless an `.ocragdoll` says otherwise.
9. **Hinge sign convention** is the engine's own rotation sense about the stated axis, with the left-handed flip applied inside. Default: that, pinned by the knee test; if the headed knee bends backward the fix is one sign in the builder.
10. **The mapper is off by default** and is expected to show artefacts at extreme poses. Default: opt-in per ragdoll.
11. **Wheel cap 64** is a guess at a sensible ceiling, not a measured limit. Default: 64; headed check 13 records the cost.
12. **Motorcycle maturity.** Jolt calls the controller "still in development". Default: ship it as a third kind with the numbers of 2.6.4, mark it experimental in `docs/VEHICLES.md` and in the template README, and tune in the headed pass. No lane-following bikes (traffic skips non-wheeled specs).
13. **`.ocvehicle` version stays 1.** An older engine rejects `KIND motorcycle` with its existing error and ignores the new records. Default: acceptable (assets move forward with the engine); the alternative is `OCVEHICLE 2`, which would also refuse every old file's new siblings and is not worth it.
14. **Stand-still balance.** The lean spring holds a stopped bike upright (no side stand). Default: that; a kickstand is content, not engine.
15. **`VehicleDriver` uses `set_enabled`.** Default: yes, with the old path as the fallback; if the headed check shows a regression, revert that one function.
16. **`AbiEnumTest` `headers[12]`.** It is full; the first wave 11 area to add a header must grow it. Default: this area grows it to 32 in phase A and the others rebase.
17. **Patent notes, collected.** Every technique here is generic and long public: the friction ellipse and load sensitivity (standard textbook tyre mechanics, not a vendor tyre model), the acceleration-limited locomotion motor (the oldest idea in character controllers), conveyor surface velocity and the contact hooks (Jolt's own, MIT), per-joint motor targets (a PD pull toward a pose), `Stabilize` (Jolt's, from a published Havok talk), `SkeletonMapper` (Jolt's). What is **deliberately not built** is a ragdoll balance, stepping or behaviour-synthesis controller (the Euphoria family: live patents in dynamic motion synthesis); partial ragdolls here are mode switches per pose group, not generated motion. Stability-control patents (cars) stay out for the reason wave 10 gave. Nothing in this area needs a third-party library.
