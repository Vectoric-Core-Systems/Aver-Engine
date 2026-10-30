# Raycast vehicle and Godot joint parameters (Fabric G2)

Two pieces for Godot's physics nodes, both opt-in: a **raycast vehicle** (Godot's `VehicleBody3D` / `VehicleWheel3D`) beside Jolt's wheeled vehicle, and a C# **joint parameter mapper** (`PinJoint3D`, `HingeJoint3D`, `SliderJoint3D`, `ConeTwistJoint3D`, `Generic6DOFJoint3D`) onto the existing Aver/Jolt joints. Jolt's `WheeledVehicleController` (`physics_vehicle_abi.h`, `physics_vehicle_ext_abi.h`) is untouched and stays the default. No existing call changed meaning; a world that never creates a raycast vehicle runs one pointer test per fixed step, and the determinism golden checkpoints are identical.

Written from the behaviour of a raycast-vehicle model; no code from Bullet or Godot. Godot's numbers are metres and its own sign conventions, so none is built in: the converter or `Aver.Fabric` passes every value (C# `RaycastWheelSettings.FromGodot` and the `*FromGodot` helpers do the unit work).

ABI: `physics_raycast_vehicle_abi.h` (`AVER_PHYS_RAYVEH_ABI_VERSION 1`, 17 exports). C#: `RaycastVehicle`, `RaycastWheelSettings`, `RaycastWheelState`, and `GodotJoints`. Source: `modules/physics/src/PhysicsRaycastVehicle.cpp`. Tests: `RaycastVehicleTest` (99 checks), `AbiEnumTest`.

## The vehicle

A raycast vehicle is an ordinary dynamic body (the chassis) plus wheels that are **not bodies**. `aver_phys_rayveh_create(chassis)` makes the vehicle (a handle that is not a body handle; the chassis must be dynamic, not a sensor, not a Jolt vehicle's chassis, and have no raycast vehicle yet); `aver_phys_rayveh_add_wheel` adds wheels from a parameter block (`AVER_PHYS_RAYW_*`, chassis-local cm). Removing the chassis body destroys the vehicle. Each fixed step, before the solver (after `vehicleStepBegin`), the vehicle runs in handle order:

1. **Suspension.** One ray per wheel from the connection point along the suspension direction, length rest + radius, against everything except the chassis and sensors whose Godot layer bits meet the wheel's mask (`aver_phys_rayveh_set_wheel_mask`). The suspension length is hit distance minus radius, clamped to [rest - max_travel, rest]. Force = (stiffness x compression / max(n.up, 0.1) - damping x speed) x chassis mass, with compression damping while the suspension shortens and relaxation damping while it extends, clamped to [0, max_force] (0 = no limit), applied as an impulse along the contact normal at the contact point. A dynamic ground gets the opposite impulse.
2. **Friction**, from the chassis velocity after the springs pushed. Per wheel: the forward direction is `normal x axle` (the axle steered about the suspension), the side direction lies in the ground plane. The side impulse cancels the sliding speed at the contact (relative to the ground's own velocity there); the forward impulse is the engine force x dt (wheels with TRACTION only), or, when the engine force is 0, the impulse that stops the wheel rolling relative to the ground, capped at brake x dt. The two are limited together by a friction circle of radius suspension force x dt x friction_slip; past it both are scaled down (skid info is that scale, below 1 in a skid). The side impulse acts at a point whose height along world up is scaled by roll_influence (0 = at the centre of mass: no roll from the tyres). The side and brake corrections are shared (side by the wheels in contact, brake by the braking wheels) so four wheels do not each cancel the whole slip.
3. **Readback** per wheel (`aver_phys_rayveh_wheel_state`, `AVER_PHYS_RAYS_*`): in contact, contact point and normal, suspension length and force, skid info, spin, rotation, rpm, forward and side speed, ground body, applied steering; and `aver_phys_rayveh_wheel_pose` (centre and rotation of the wheel mesh from the chassis' current pose, with steering and spin).

Drive is per wheel and persists until changed (`-1` = every wheel): `set_engine_force`, `set_brake`, `set_steering`. The TRACTION and STEERING flags of the block gate engine force and steering for a wheel; Godot's `VehicleBody3D` spreads its own `engine_force`, `brake` and `steering` over the flagged wheels, which is the caller's job here. A non-zero engine force wakes a sleeping chassis; a chassis that sleeps is not stepped (a brake alone does not wake it).

### Units and conversions from Godot

| Godot | ABI | Conversion |
|---|---|---|
| metres | cm | x 100 (`FromGodot`) |
| axes (+X right, +Y up, -Z forward) | (+Y right, +Z up, +X forward) | `(x, y, z) -> (-z, x, y)` (`FromGodotAxes`) |
| `engine_force` (N) | kg*cm/s^2 | x 100 (`EngineForceFromGodot`) |
| `brake` (an impulse applied every step) | a force | x ticks per second x 100 (`BrakeFromGodot`) |
| `steering` (positive = left) | positive toward the axle side (right) | negate (`SteeringFromGodot`) |
| `suspension_stiffness`, `damping_*` | mass-normalised, 1/s^2 and 1/s | unchanged: the number means the same in cm or m |
| `suspension_max_force` (N) | kg*cm/s^2 | x 100 |
| `suspension_travel` (m) | `MAX_TRAVEL` (cm) | x 100; **compression only** (see below) |
| `wheel_friction_slip`, `wheel_roll_influence` | `FRICTION_SLIP`, `ROLL_INFLUENCE` | unchanged |

`RaycastWheelSettings.DampingFromRatio(ratio, stiffness)` (= ratio x 2 x sqrt(stiffness)) is provided for the case where the Godot version in use stores damping as a ratio; **UNVERIFIED** which one the target version does, so `FromGodot` passes damping through untouched and the converter must check.

### Differences from Godot's `VehicleBody3D`, said plainly

- A **ray**, not a shape, per wheel. A wheel never sees a kerb narrower than the ray, and has no collision shape: the chassis' own collision shape is what touches the world when the suspension bottoms out.
- The ray reaches rest + radius, so a suspension never extends beyond its rest length; `MAX_TRAVEL` limits compression only (the model this follows clamps both sides, but only compression is reachable).
- Friction is solved against the pre-friction velocities with the shared corrections above rather than sequentially. Same limits, same skid behaviour; not the same numbers step for step.
- Engine force overrides the brake on a wheel while it is non-zero.
- Wheel spin is derived from the ground speed at the contact (a coasting wheel in the air slows by 0.99 per 1/60 s); a skidding or locked wheel is not modelled, and engine force does not spin a wheel in the air. It is a readback for meshes and rpm, not simulation state.
- The brake is a force. Godot's per-step impulse is frame-rate dependent; the conversion above makes it equal at the project's step rate.
- Suspension and tyre forces are explicit impulses once per fixed step (not a constraint), so a very stiff spring at a low step rate can overshoot; the stiffness is mass-normalised against the whole chassis mass for every wheel, so the sag under load is g / (wheels x stiffness) whatever the mass.
- Not provided: Godot's node-level `engine_force` / `brake` / `steering` distribution, a per-wheel `get_rpm` beyond the readback above, and wheels as bodies.

### Snapshots and determinism

The vehicle table is configuration plus per-step input, not simulation state: the forces are a pure function of the chassis state (which snapshots hold) and the engine force, brake and steering the caller sets. `RaycastVehicleTest` restores a snapshot and replays a driven run bit for bit; wheel rotation, spin and the readback numbers are not captured and restart from the next step's contact (rotation continues from the value it had, which is visual only). The vehicle table survives a restore (tested).

## Joint parameter mapping (C# `GodotJoints`)

Inputs are Godot's parameter values in Godot's units (metres, radians, N, N*m, impulse per step); geometry is Aver's (a world-space anchor in cm, world-space Aver axes - pass Godot directions through `FromGodotAxes`). Godot's right-handed Y-up space maps to Jolt's by the identity, so angles and motor velocities keep their sign. Every ignored parameter is a nullable record field: setting it adds a line to `GodotJointResult.Notes`. The target is Godot's own Jolt module, which ignores several of these as well. Limits are measured from the pose the bodies have when the joint is created (0 = that pose), as the existing joint factories do.

| Godot | Aver / Jolt | Mapping |
|---|---|---|
| `PinJoint3D` anchor | `Joint.Point` | exact |
| `PinJoint3D` `bias`, `damping`, `impulse_clamp` | none | not at all (Jolt's point constraint has no softness) |
| `HingeJoint3D` axis, `use_limit`, `limit/lower`, `limit/upper` | `Joint.Hinge` | exact when the range contains 0; otherwise widened to include 0 (noted); equal bounds build a fixed joint |
| `HingeJoint3D` `enable_motor`, `motor/target_velocity` | velocity motor | exact |
| `HingeJoint3D` `motor/max_impulse` | motor strength = impulse x ticks per second (x 10^4 for cm^2) | approximate: an impulse per step becomes a torque at the project's step rate |
| `HingeJoint3D` `bias`, `limit/bias`, `limit/softness`, `limit/relaxation` | none | not at all |
| `SliderJoint3D` axis, `linear_limit/lower_distance`, `upper_distance` | `Joint.Slider` | exact for a range containing 0 (else widened, noted); upper below lower = unlimited; equal = fixed |
| `SliderJoint3D` softness, restitution, damping; `angular_limit/*` | none | not at all (Jolt's slider locks rotation) |
| `ConeTwistJoint3D` twist axis, `swing_span`, `twist_span` | `Joint.SwingTwist` (round cone, twist +-span) | approximate: spans are read as half-angles (the Jolt module's reading; `SpansAreFullAngles` halves them if the Godot version means the whole span - UNVERIFIED which) |
| `ConeTwistJoint3D` `bias`, `softness`, `relaxation` | none | not at all |
| `Generic6DOFJoint3D` linear limits per axis | `Joint.SixDof` translation limits | exact; disabled = free; lower > upper = locked (noted) |
| `Generic6DOFJoint3D` angular limits per axis | `Joint.SixDof` rotation limits | approximate: Jolt limits rotation about its Y and Z axes as a swing, Godot as Euler angles; the two agree for small angles and single-axis use |
| `Generic6DOFJoint3D` linear and angular motors (`target_velocity`, `force_limit`) | velocity motor + motor strength | exact (N -> kg*cm/s^2, N*m -> kg*cm^2/s^2) |
| `Generic6DOFJoint3D` springs (`stiffness`, `damping`, `equilibrium_point`) | position motor with a motor spring | approximate: frequency = sqrt(k/m)/2pi and damping ratio = c / (2 sqrt(k m)) with m the reduced mass of the pair (`GodotJoints.ReducedMass`) or, for the angular axes, `EffectiveInertiaKgM2` (without it the angular spring is skipped with a note); a motor on the same axis wins |
| `Generic6DOFJoint3D` limit softness, restitution, damping, erp, force limit | none | not at all |

Bodies: the first body is the reference (a translation or angle is body B relative to body A), as in Godot. A joint to the world uses `Body.None` for B; `Joint.ExcludeCollision` (Godot's `exclude_nodes_from_collision`) is set by the caller on the result.

## Tests

- `RaycastVehicleTest` (99 checks): refusals and read-back of every call; a wheel in the air reads its rest length; a settled car's suspension sits at rest - g / (N x stiffness) with the springs carrying the weight, and the same sag at four times the mass; max_force clamps the push; engine force accelerates the car through TRACTION wheels only; a coasting car keeps rolling; the brake stops it; steering turns toward +Y for positive angles, -Y for negative, only through STEERING wheels, and the wheel pose turns with it; rpm and spin follow the ground; friction_slip caps thrust and a sideways slide against a grippy twin and a live change takes effect; roll_influence 0 keeps the body level while 1 rolls it; a dynamic slab under the car is pushed back with momentum shared; the wheel mask; an engine force wakes a sleeping chassis; two worlds agree bit for bit; a snapshot restore replays a driven run exactly.
- The joint mapper was exercised by a one-off C# harness (not checked in: the managed `Aver.Physics` assembly cannot be loaded beside the native DLL of the same name inside the repo's test projects): pin, hinge with limits and a motor reaching the limit, slider stopping at its limit, cone twist, a six-DOF spring settling at its equilibrium point, `SpringToHz`, and the `FromGodot` conversions.
- Determinism: `PhysicsDeterminismTest` golden checkpoints 100..600 are unchanged.

## Not done

- Shape casts for the wheels (rays only).
- Per-wheel suspension length in snapshots (readback only, not simulation state).
- A managed test for `GodotJoints` in the repo (see above).
- `PhysicalBone3D` and `SoftBody3D` parameter mapping (the other half of that Fabric row).
