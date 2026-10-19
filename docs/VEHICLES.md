# Vehicles

Drivable cars, trucks and tanks in a level: a vehicle described as data (`.ocvehicle`), built into Jolt's vehicle constraint, driven by a player or a script,
and presented with wheels, sound, smoke, damage, cameras and a HUD. This is the wave 10 feature that replaced the single built-in car with a Chaos-Vehicles-class
system. Design and decisions: [plans/wave10/VEHICLES.md](plans/wave10/VEHICLES.md). Where this page and the code disagree, the code is right; the plan's
"Deviations" section lists the places the implementation chose differently on purpose.

**Status.** Written against the frozen design in one pass. At the time of writing nothing here has been compiled or run: every test margin in `tests/physics`
(`VehicleExtTest`, `TrackedVehicleTest`) and `tests/vehicle.scene` is a guess the first run will confirm or correct, and the template's graphs and meshes
have not been seen in a window. The generators check what they can (the graphs against the node reference, the meshes' bounds against the specs).

## The shape

| Piece | Where | What it is |
|---|---|---|
| Physics ABI v2 | `modules/physics`, `physics_vehicle_ext_abi.h` | Everything a real vehicle needs beyond the 14 functions of `physics_vehicle_abi.h`: torque curve, gearbox, per-axle differentials and anti-roll bars, tyre curves, tracked vehicles, compound chassis, driver aids in the fixed step, telemetry, events |
| `Aver.Vehicle` | `modules/vehicle` | The vehicle as data: `VehicleSpec`, the `.ocvehicle` reader and canonical writer, `validate`, the built-in sedan and tank, `specFromBounds` (the traffic presets), `buildVehicle` (spec to physics calls), a parse cache. Pure: Core, Formats, Physics |
| `Aver.Vehicle.Scene` | `modules/vehicle.scene` | `CVehicle` and `VehiclePhysicsSystem`: builds vehicles from a level, writes their poses, moves the wheel visuals, reads telemetry and events, enter and exit, damage, LOD. Sound, effects, crash damage, cameras and bones leave through sinks the host installs |
| Traffic | `modules/world`, `Runtime/src/GameLevel.cpp` | `world::VehicleSystem` now builds its cars with `specFromBounds` and `buildVehicle`; a placement that carries a `CVehicle` is left to the new system |
| Framework relay | `framework_vehicle_abi.h`, `VehicleAbi.cpp` | `aver_fw_vehicle_*`: one provider function, 13 operations, for C# and Aver Node |
| C# and Aver Node | `Aver.Framework`, `Aver.Physics`, `Aver.Graph` | `Entity` members, `Vehicles`, `VehicleTelemetry`, `DrivingContext`, `VehicleHud`; the `Vehicle` node family (20 rows) |
| Editor | `sandbox/src/Vehicle*` | The Vehicle Details section, the `.ocvehicle` editor tab, `Show > Vehicles`, New Vehicle in the Content Browser |
| Content | `templates/Vehicle`, `test-content/VehicleLab` | The template (a sedan and a tank, no C#) and the engine test lab |

The solver is Jolt's `WheeledVehicleController` and `TrackedVehicleController`. `Aver.Vehicle` does not simulate; it describes a vehicle and configures Jolt. A vehicle that
sets nothing beyond what the old built-in car set builds exactly the Jolt objects that car built (same engine curve, ratios, differentials, tyre curves), so existing
traffic and C# cars behave as before.

## Try it

- **The template.** New Project > Vehicle. Walk to the sedan or the tank and press E; F gets out. `templates/Vehicle/Content/README.md` has the keys.
- **The lab.** Open `test-content/VehicleLab`, play, select a vehicle, Details > Vehicle > Drive. `test-content/VehicleLab/README.md` lists the checks (6x4 truck, ice-split lane, brake strip, drop, slalom, crash wall, a skinned car whose wheels are bones, a tank with a manual box).
- **From C#.** `Vehicles.Spawn("Vehicles/Sedan.ocvehicle", position)` returns an `Entity`; `DrivingContext.Push()` binds the nine `Drive.*` actions; `entity.EnterVehicle(driver)`.
- **From a graph.** `AN_VehicleSpawn`, `AN_VehicleEnter`, `AN_VehicleDrive`, `AN_VehicleTelemetry` and the rest of the Vehicle family ([nodes/Vehicle.md](nodes/Vehicle.md)).

## Physics ABI v2

`modules/physics/include/aver/physics/physics_vehicle_ext_abi.h`, `AVER_PHYS_VEHICLE_ABI_VERSION 2`. The v1 header is unchanged. Conventions are v1's: centimetres,
kilograms, N*m, degrees for steering and slip angle, rad/s for wheel speed; a call returns 1 or 0 (`aver_phys_last_error` says why); a parameter block may be shorter
than its defined length (the rest keep their values) and is refused when longer; builder calls are refused after `aver_phys_vehicle_finish`, live calls before it.

| Group | Functions |
|---|---|
| Builder | `set_kind` (wheeled or tracked), `set_chassis_boxes` (a compound chassis), `set_engine_ex`, `set_torque_curve`, `set_gearbox`, `axle_count`, `wheel_axle`, `set_axle`, `set_center_diff`, `set_wheel_ex`, `set_wheel_curves`, `set_track`, `set_assists`, `set_aero`, `set_collision_period` |
| Live | `tune`, `set_gearbox_mode`, `set_gear`, `shift`, `flip` |
| Read | `state`, `wheel_state`, `event_count`, `event_get` |

Parameter blocks are indexed by the `AVER_PHYS_V*` macros: `VE` engine, `VG` gearbox, `VA` axle, `VW` wheel, `VT` track, `VD` driver aids, `VU` live-tune keys, `VS`/`VSI` vehicle
floats and ints, `VWS`/`VWSI` wheel floats and ints, `VEV` events. The C# side (`Aver.Physics/VehicleExt.cs`) mirrors them as internal enums that `AbiEnumTest` pairs with the header.

Things worth knowing:

- **Axles.** Wheels on opposite sides within 5 cm of the same x are an axle. Axle 0 is the front (largest x), whatever order the wheels were added in.
- **Differentials.** Jolt has no hard lock: mode 2 writes a limited-slip ratio of 1.01. The default `DIFF_MODE` is 1 (limited slip at 1.4), Jolt's own, not 0 (open), so a vehicle
  that never calls `set_axle` is identical to a v1 one. A spec says `diff open` when it means open.
- **Driver aids run in the fixed step.** With `VD_ENABLED` set, `aver_phys_vehicle_set_input` stores the pedals and the step loop turns them into what Jolt sees: rate-limited and
  speed-sensitive steering, brake-to-reverse, a hold brake at a standstill, ABS, traction control, aero drag and downforce, manual clutch timing. With it off (the default) the pedals
  are forwarded at once, as v1 does. Telemetry and events are read after `system.Update`.
- **Tracked vehicles.** `WheelSettingsTV` and `TrackedVehicleController`. The driver input is `(forward, leftRatio, rightRatio, brake)`; ratios are lifted to at least 0.05 in magnitude
  so the box never decouples, and the vehicle pivots when standing. Every wheel must be on exactly one track.
- **Compound chassis.** `set_chassis_boxes` rebuilds the body under the same handle (a `StaticCompoundShape` inside `OffsetCenterOfMassShape`) rather than calling `SetShape`.
- **Slip** is signed with a 1 m/s denominator floor and clamped to [-1, 8].
- **Events** (kinds `AVER_PHYS_VEV_*`): skid begin and end, land, impact, shift, flipped, assist. Impacts are noted in the contact listener from the points' velocities (under a mutex) and merged
  and sorted in the step, so they are in a stable order.
- **Determinism.** `VehicleEntry` lives in a `std::map` (handle order); the step hooks walk vehicles in that order.

## `.ocvehicle`

OC text, UTF-8, `#` starts a comment, one record per line, the first record is `OCVEHICLE 1`. An unknown record or key is skipped with one warning per file; a newer version is
refused; a malformed number for a known key refuses the file with the line number. Lengths are cm, angles degrees, positions relative to the vehicle's origin, which is the **bottom
centre**, +X forward, +Y right, +Z up. Anything a file does not say is the physics ABI's default.

| Record | Fields |
|---|---|
| `NAME` | text to the end of the line |
| `KIND` | `wheeled` or `tracked` |
| `BODY` | `half x y z clearance c com x y z mass kg pitchroll deg` |
| `BOX` | `cx cy cz hx hy hz`, up to 8; when present they replace BODY's box |
| `ENGINE` | `torque minrpm maxrpm inertia damping` (inertia 0 = scaled to the torque as Jolt's default engine is) |
| `CURVE` | pairs `x y`: engine speed over the rev limit, fraction of peak torque; 2 to 32 points, ascending in 0..1 |
| `GEARBOX` | `auto` or `manual`, `forward r r ...`, `reverse r ...` (positive magnitudes), `switch clutchrelease latency up down clutch` |
| `CENTER` | `open`, `lsd <ratio>` or `locked`: between the driven axles of an all-wheel drive |
| `AXLE <n>` | `final diff open\|lsd\|locked split lsd share antiroll`; axle 0 is the front |
| `TYRE <name>` | `long <slip mu>... lat <deg mu>...` |
| `WHEELCLASS <name>` | `radius width susp <min> <max> hz damping brake handbrake inertia spindamping tyre <name>` |
| `WHEEL <name>` | `class <c> hub x y z steer deg driven 0\|1 handbrake 0\|1 brake <Nm> node <entity> bone <bone>`; the hub is the **settled** wheel centre (z equal to the radius puts the tyre bottom on the ground) |
| `TRACK left\|right` | `wheels i i ... driven i inertia damping brake final steergain pivot` |
| `ASSIST` | `abs absslip absrelease tcs tcsslip steerin steerout throttlein throttleout brakein reversebrake reversespeed holdbrake holdspeed skidslip skidangle` |
| `STEERCURVE` | up to four `speed fraction` pairs, cm/s |
| `AERO` | `drag downforce`, N per (m/s)^2 |
| `SEAT` | `driver x y z exit x y z` |
| `AUDIO` | `engine <idle loop> idle <rpm> <high loop> high <rpm> pitchlow pitchhigh skid <path> impact <path> shift <path>` |
| `EFFECT` | `skid <path> rate <per second> impact <path> count <n>` |
| `DAMAGE` | `health crash energy radius shell <entity>` |
| `CAMERA` | `modes chase hood cockpit arm eye x y z hood x y z` |
| `HUD` | a content path |

`writeOcVehicle` is canonical and idempotent (shortest round-trip floats, defaults omitted where the reader's default is the physics default): `write(parse(write(spec)))` is `write(spec)`.
`validate` refuses a spec the physics ABI would: no wheels or more than 16, an unresolved class or tyre, no driven wheel (wheeled), a track without wheels or with a wheel on two tracks
or none (tracked), non-finite numbers, curves that do not ascend.

## `Aver.Vehicle`

`VehicleSpec` and its parts (`VehicleSpec.hpp`); `parseOcVehicle`, `writeOcVehicle`, `validate`, `findClass`, `findTyre`, `SpecCache` (`OcVehicle.hpp`); `presetName`, `presetIndex`,
`builtinSpec("sedan" | "tank")`, `specFromBounds(preset, sizeCm, spec)` (`VehiclePresets.hpp`); `buildVehicle(spec, origin, built, why)`, `springSagCm` (`VehicleBuild.hpp`).

- `specFromBounds` reproduces the old traffic table's formulas for car, van, truck, bus and sports in the same operator order (a test compares the physics calls bit for bit), plus the tank.
- `buildVehicle` makes the v1 calls first and the v2 calls only for data that differs from the defaults. A wheel's attachment is lowered by the static spring sag so the **settled** wheel
  centre lands on the hub the spec states.
- The traffic system (`world::VehicleSystem`) builds its cars through these two functions. A `vehicle <preset>` token still means a preset; `vehicle Vehicles/Bus.ocvehicle` names an asset
  (resolved through `setSpecSource`). Traffic refuses a tracked spec with one log line.

## `Aver.Vehicle.Scene`

`CVehicle` is a 96-byte component registered at runtime (the `CCloth` pattern; `Components.hpp` ids and the scene ABI do not change). Attach through
`VehiclePhysicsSystem::attach`, never `addComponent` directly: the storage arrives zero-filled, and zero-filled storage is the useful state.

| Field | Meaning |
|---|---|
| `asset`, `preset`, `flags` | The `.ocvehicle` ObjectId (0 = preset: car van truck bus sports tank); negative-sense flags `noactions`, `noaudio`, `noeffects`, `nowheels`, positive `indestructible` |
| `inThrottle inSteer inBrake inHandbrake` | Written by a script or by the driver; they hold until set again |
| `powerScale gripScale brakeScale damageScale` | Zero reads as 1 |
| OUT `driver vehicle outState outGear outSpeedCmS outForwardCmS outRpm outHealth01 outThrottle outSteer outBrake outWheelsOnGround` | Read-only; `outState` bits `kVehicleState*` |

What the system does each frame (`prePhysics` before `aver_phys_step`, `postPhysics` after it, inside the host's PLAYING gate):

1. **Builds** vehicles lazily: collects the entities with a `CVehicle` in id order, resolves the spec (an `.ocvehicle` through the host's spec source, or a preset sized from the entity's mesh
   bounds) and builds at most **two** per frame, so a level with sixty cars does not stall its first frame. An entity with a `CRigidBody` or an object-clip `CAnimator`, a non-unit scale, or no
   spec is refused with one warning. A physics vehicle is not scaled.
2. **Drives**: a driver's `Drive.*` actions become pedals (only on change), the power, grip and brake scales become live tunes, flip and repair requests are served.
3. **Writes the entity pose every frame in C++**: no script timer copies it. Only when it changed.
4. **Moves the wheels**: each `WHEEL ... node X` child gets the hub position and the steer-then-spin rotation relative to the vehicle; each `WHEEL ... bone B` sets the rig inputs
   `wheel_<B>_pos` and `wheel_<B>_rot` through a sink (`Anim.Scene`'s control rig `setInput`). Suspension travel needs no code: it is the hub moving in the pose. A tracked vehicle also publishes
   `track_left` and `track_right` speeds.
5. **Reads telemetry** into `CVehicle`'s OUT fields and a `Telemetry` record, drains the physics events and turns them into system events.
6. **Presents**: engine loops (the nearest `audibleVehicles`, default 8, within 5000 cm: idle and high loops crossfaded by RPM, a skid voice, one-shots), skid smoke and impact sparks, hard-impact
   damage and the crash sink.

**Driving.** `enter(vehicle, driver)` hides the driver's meshes, zeroes the physics character's gravity and strength (both restored on exit), parks the driver in the seat every frame, and asks
the camera sink to point the view at the vehicle's rig. `exit` puts the driver at the spec's `SEAT exit` point. `Drive.Exit` and `Drive.CameraNext` are read from the action source.

**Events** (`VehicleEventKind`): 1 to 7 are the physics kinds (skid begin, skid end, land, impact, shift, flipped, assist), 8 entered, 9 exited, 10 crash, 11 destroyed. Each carries a sequence
number. The notify sink raises them to graphs as the named events `VehicleCrash`, `VehicleSkid`, `VehicleLanded`, `VehicleFlipped`, `VehicleShift`, `VehicleEntered`, `VehicleExited`.

**Damage.** A spec with `DAMAGE` has health; an impact whose closing speed exceeds `crash` (default 600 cm/s) costs health, and past `crash` also fires the crash sink with the energy
`0.5 * reducedMass * v^2 * energy`. At zero health the vehicle is destroyed (events, no input). `Repair` restores it. `damage()` accepts a negative amount to give health back.

**LOD.** A driverless vehicle beyond `lodMidCm` (6000) casts its wheels every second step and beyond `lodFarCm` (15000) every fourth; a driven one always every step. Past `maxVehicles` (64) a
vehicle waits unbuilt.

**Sinks** (`setAudioPort`, `setEffectSink`, `setCrashSink`, `setCameraSink`, `setNotifySink`, `setRigInputSink`, `setActionSource`, `setSpecSource`, `setBoundsSource`): the module links none of audio,
particles, soft bodies, cameras or animation, so a host installs what it has. With no sink the feature is silent, not broken.

**Determinism.** Inputs to the simulation are the pedals, the gear requests and the ordered tune, flip and repair calls. Every loop is ascending by entity id then wheel index; events carry a per-system
sequence number; nothing reads a clock or a random number. On one binary and one instruction set the same input stream reproduces the same poses, telemetry and events (a test runs it twice and compares
bits). Cross-platform lockstep is **not** claimed: Jolt is built without `CROSS_PLATFORM_DETERMINISTIC`. The audio and effect sinks are presentation and are excluded.

## Host wiring

The editor's Play and the packaged game wire the same things (`Runtime/include/aver/game/GameSystemsWiring.hpp`, called from `GameApp` and the sandbox):

- `registerGameSystems` registers `CVehicle`, installs the system into the world, sets the framework provider (`aver_fw_set_vehicle_provider(&vehicleProvider, &vehiclePhysicsSystem())`)
  and installs the sinks: the action source over `aver_fw_action_find/value2/held/pressed` (cached by name), the audio port over `aver_audio_*`, the effect sink (`spawnEffectAt`), the crash sink
  into `softbody::softBodySystem().impact`, the camera sink into `camera::cameraRigSystem()` (attach the VehicleChase and Seat presets to the rig entities the system made, `setViewTarget(rig 0, blend 0.6 s)`,
  clear on exit), the notify sink (graph events), the bone sink into `anim::controlRigSystem().setInput`, and the spec cache over the content resolver.
- `tickVehiclesPre(dt, viewPos)` and `tickVehiclesPost(dt)` run beside the traffic system's calls; `resetGameSystems` clears the system.
- The editor seeds `PlayMobility` with `vehiclePhysicsSystem().entities()` so a Play snapshot restores the vehicles' poses on Stop, and a save made during Play writes where each vehicle was
  placed, not where it has driven to.

Input: one project has one input scheme. The template binds the nine `Drive.*` actions (`Drive.Throttle`, `Brake`, `Steer`, `Handbrake`, `ShiftUp`, `ShiftDown`, `Reset`, `Exit`, `CameraNext`) with the on-foot
actions; the vehicle system reads them only while a driver exists and ignores the rest.

## Framework relay

`framework_vehicle_abi.h`, `AVER_FW_VEHICLE_ABI_VERSION 1`, own version. Aver.Framework does not link the vehicle module, so one provider function is installed and 13 operations forward to it; with no
provider every call returns 0. In this ABI `vehicle` **is the entity**; the physics handle appears only as spawn's second output. Integers travel as the bit pattern of an int32 in a float slot (an
entity id has 31 significant bits and a float keeps 24). Calls apply immediately (the physics world is idle between steps); telemetry is the last fixed step's.

Operations: spawn, enter, exit, shift, set gear, set manual, flip, repair, damage, tune, telemetry, wheel, events. Telemetry is read as `AVER_FW_VEH_TF_*` floats (19), `TI_*` ints (13), wheel `WF_*` (18) and
`WI_*` (4). `CVehicle` itself (adding it, reading OUT fields, writing the input fields) goes through the generic scene ABI.

## C#

Everything below is in `Aver.Framework` unless noted. Nothing here crashes in a process with no native library; the members answer false.

| Member | Does |
|---|---|
| `Vehicles.Spawn(asset or preset, position, yaw)`, `Vehicles.SpawnFromText(ocvehicleText, ...)` | Makes an entity with a `CVehicle` and builds it now |
| `Vehicles.CreateCar(...)`, `CarSpec` | The older API, unchanged in shape: a default car by integer handle. `CarSpec.ToOcVehicle()` writes its `.ocvehicle`; `Car.SetInput` goes through the entity now and the 60 Hz timer is gone |
| `Vehicles.DrainEvents(Span<VehicleEvent>)` | The event stream |
| `entity.AddVehicle(preset or asset)`, `IsVehicle` | The component |
| `entity.SetVehicleInput(throttle, steer, brake, handbrake)`, `TryGetVehicleTelemetry`, `TryGetWheelTelemetry(i)` | Driving and reading |
| `entity.ShiftVehicle`, `SetVehicleGear`, `SetVehicleManualGearbox`, `EnterVehicle(driver)`, `ExitVehicle`, `FlipVehicle`, `RepairVehicle`, `DamageVehicle`, `TuneVehicle(key, value)`, `VehicleDriver` | Control |
| `DrivingContext` | The nine `Drive.*` actions with the default keyboard and gamepad bindings; `Push()` and `Pop()` at priority 100. `DrivingContext.DefaultBindings` is pure data |
| `VehicleHud` | A binding of a `.ocui` layout's widgets to telemetry, `FormatSpeed(cmPerS, miles)`, `FormatGear` |
| `Aver.Physics.Vehicle` | The v2 builder and live calls (`SetEngine`, `SetTorqueCurve`, `SetGearbox`, `SetAxle`, `SetCenterDifferential`, `SetWheelCurves`, `SetTrack`, `SetAssists`, `SetAero`, `Tune`, ...) and the `VehicleState`, `WheelState` and `VehiclePhysicsEvent` records |

Tuning keys (case-insensitive; `TuneVehicle`, `AN_VehicleTune`, the Details sliders): `power`, `brake`, `grip`, `suspension`, `steer`, `drag`, `downforce`, `abs` (0 or 1), `tcs` (0 or 1), `manual` (0 or 1), `collisionperiod` (1 to 8).
An unknown key returns false and logs once.

## Aver Node

The `Vehicle` family has 20 rows. The six of version 1 (`AN_VehicleCreateCar`, `SetInput`, `GetState`, `SetPose`, `Destroy`, `WheelContact`) keep their keys, pins and defaults and hold a vehicle by its integer physics handle.
The fourteen of version 2 hold it by entity: `AN_VehicleSpawn` (attribute `asset`), `Drive`, `Shift`, `SetGear`, `GearMode`, `Enter`, `Exit`, `Flip`, `Tune` (attribute `key`), `Repair`, `Telemetry`, `WheelState`, `Driver`, `FromEntity`
(maps an entity to the handle the old rows take). Rows: `Aver.Graph/GraphVehicleNodeTable.cs`; palette `sandbox/src/GraphNodeDefsVehicle.hpp`. Events reach graphs through `AN_OnEvent` entries named as above.

## Level records

`COMPONENT Vehicle asset=Vehicles/Sedan.ocvehicle preset=car power=1 grip=1 brake=1 damage=1 noactions=0 noaudio=0 noeffects=0 nowheels=0 indestructible=0`.
Canonical form: `asset` (or `asset#` with 16 hex digits when the editor cannot turn the id back into a path); `preset` always when there is no asset, with an asset only when it is not `car`; then each scale and
flag only when it is not at its default. Runtime state (driver, health, gear, damage taken) is never saved: a loaded level starts with an intact, unoccupied vehicle. Applying a record to an entity
that has a `vehicle` placement token, a `CRigidBody` or an object-clip `CAnimator` is refused with one warning. The format is in [formats/FORMAT_SPECS.md](formats/FORMAT_SPECS.md).

## Editor

- **Add Component > Vehicle** on a placed mesh (not for traffic cars, which use the `vehicle` token). The Details section has an asset picker, a preset combo (choosing one drops the asset), **Create .ocvehicle** (writes
  `<mesh dir>/<mesh>.ocvehicle` from the preset sized to the mesh and assigns it), the four scales and the flags. In Play it shows RPM, gear (with shift buttons when manual), speed in km/h, the applied pedals, health,
  ABS and TCS lamps, live tuning sliders and a per-wheel table; buttons **Drive** (a ghost driver, or the Play pawn), **Exit**, **Flip**, **Repair**, **Stop**.
- **The `.ocvehicle` editor** (double-click one; Content Browser > New Vehicle writes the sedan): Body, Engine (an editable torque curve and the power curve computed beside it), Gearbox (ratios and each gear's top speed
  at the rev limit), Axles and drive (the derived layout label FWD, RWD, AWD, 4x4, 6x4, Tracked, and differentials), Wheels (a table and a preview over the selected entity), Tyres (the two friction curves), Assists,
  Aero, Audio, Effects, Damage, Camera. A validation list shows `validate`'s error and a few advisories. Save is refused while there is an error. Edits apply to vehicles built **after** the save.
- **Show > Vehicles**: suspension (green to red by compression), tyre forces (red longitudinal, blue lateral; 1 cm per 100 N), slip rings (hot when skidding), collision boxes and centre of mass, velocity, labels.
  The packaged game has no line overlay and none was added; its text overlay is the HUD.

## Tests

| Target | Covers |
|---|---|
| `tests/physics` `VehicleExtTest`, `TrackedVehicleTest` | v2: identity with v1, curves, gearbox, differentials, aids, telemetry, events, tracked, compound chassis |
| `tests/vehicle` `OcVehicleTest`, `VehicleBuildTest` | The format, validation, presets (golden values), the cache, the builder's calls |
| `tests/vehicle.scene` | The component, lazy builds and their budget, pose, visuals, bones, driving, telemetry and events, audio, LOD, replay |
| `tests/framework_vehicle` | The relay's packing and its no-provider behaviour |
| `tests/editor` `VehicleDetailsModelTest`, `VehicleEditorModelTest`, `TemplateScaffoldTest` | The Details and editor rules, the template scaffolds |
| `Aver.Graph.Tests` `VehicleNodeTests`, `VehicleApiTests`, `DrivingContextTests` | The node rows, the C# surface, the default bindings |
| `AbiEnumTest` | The v2 header against `VehicleExt.cs` |

## What is not here

No AI drivers and no traffic changes beyond the switch to the shared builder. No networking or replication. No saved vehicle state. No per-placement friction in the level format (a surface needs a Rigid Body added by hand;
the lab's README shows how). No number readouts on a graph-made HUD (graphs have no number-to-text node). No hard-locked differential (Jolt has none). Cross-platform determinism is not claimed.
