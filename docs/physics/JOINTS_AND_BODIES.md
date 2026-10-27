# Joints and per-body extras

What `physics_joints_abi.h` gained (breakable joints, soft limits, motor springs) and the new
`physics_body_ext_abi.h` (locked axes, continuous collision, velocity caps, sleep permission, sensor).
C# mirrors: `Joint.SetBreak / SetLimitSpring / SetMotorSpring`, `Physics.JointBreaks()`, and the
`Body` members in `BodyExt.cs`. Test: `tests/physics/src/JointBreakTest.cpp`.
The same header also carries Godot's `_integrate_forces`, custom integrator and static-body constant velocity (Fabric G2; angular values in radians there):
`docs/PHYSICS_SHAPES.md` section 11, `BodyGodot.cs`, `tests/physics/src/BodyGodotTest.cpp`.

Units are the engine's (cm, +X forward / +Y right / +Z up, left-handed) with three deliberate exceptions,
each called out in the header: break thresholds are SI (N, N*m), the max angular velocity is degrees per
second, and a lock is on WORLD axes.

## Breakable joints

```c
aver_phys_joint_set_break(joint, maxForceN, maxTorqueNm);   // 0 = that part never checked; 0/0 = unbreakable
aver_phys_joint_break_count(); aver_phys_joint_break_get(i, &joint, &forceN, &torqueNm);
aver_phys_joint_break_clear();
```

After every fixed step (`afterStepJoints`, called by `aver_phys_step`) each armed joint's applied impulse
for that step is read from Jolt (`GetTotalLambda*` of its constraint type), divided by the fixed step to
give a force, and compared with the thresholds. Linear part: what holds the anchor points together.
Angular part: what holds the rotation. Motor and limit lambdas add along the free axis. Past either
threshold the joint is disabled (`SetEnabled(false)`) and one event is queued. A disabled joint is skipped,
so one break is one event; `aver_phys_joint_set_enabled(joint, 1)` repairs it and it stays armed.

Every type reports a load except rack-and-pinion (one lambda mixes force and torque); `set_break` refuses it.

**Event queue: persists until drained.** Contacts clear at the start of each `aver_phys_step`, but a single
step call can run several fixed steps and a caller may poll less often than it steps, so a break must
outlive both. Read with count/get, then `break_clear` (C#: `Physics.DrainJointBreaks()` does both). The
queue keeps the newest 1024. The event may name a joint removed since.

Peak load in a step counts: an impact breaks a joint that carries the same weight at rest. A 10 kg
weight pulls ~98 N. Set the threshold above the impacts the joint should survive.

## Soft limits and motor springs

```c
aver_phys_joint_set_limit_spring(joint, frequencyHz, damping);   // 0 Hz = hard again
aver_phys_joint_set_motor_spring(joint, axis, frequencyHz, damping);
```

A limit spring makes a limit give like a spring instead of a wall: hinge, slider, distance, and the three
translation limits of a six-DOF joint (Jolt has no spring for its rotation limits, so those stay hard).
`damping` is a ratio, 1 critical. The motor spring is the missing half of `set_motor`: a POSITION motor
closes on its target as a spring, and Jolt's default (2 Hz, damping 1) was the only value reachable.
Six-DOF per-axis motor targets themselves were already reachable through `set_motor`; the motor spring
and `set_motor_strength` complete the per-axis set.

## Per-body extras

| Call | Notes |
|---|---|
| `set_locked_axes(body, mask)` / `locked_axes` | `AVER_PHYS_LOCK_TRANSLATION_X/Y/Z`, `AVER_PHYS_LOCK_ROTATION_X/Y/Z`, engine axes, set = frozen. Replaces the previous mask. |
| `set_ccd(body, on)` / `ccd` | Jolt `LinearCast` vs `Discrete`. Off by default. |
| `set_max_linear_velocity` / `set_max_angular_velocity` and getters | cm/s and deg/s. A faster body is clamped immediately. |
| `set_allow_sleeping(body, on)` / `allow_sleeping` | Turning it off also wakes the body. |
| `set_sensor(body, on)` / `sensor` | Any body, including static. |

**Locked axes.** Jolt takes allowed degrees of freedom on its WORLD axes through
`MotionProperties::SetMassProperties(allowedDOFs, massProperties)`. The engine-to-Jolt axis permutation is
derived from `Convert.hpp`'s `toJoltUnit` rather than written out, and tested on each axis separately
(a lock that landed on the wrong axis passes any single-axis test). The call rebuilds the mass properties
from the shape scaled to the body's current mass, so mass and inertia are kept. Freezing all three
translations makes Jolt store inverse mass 0, so the mass is remembered in a side table (cleared at
shutdown via `clearBodyExtState`, reached from `destroyAllJoints`) and restored on unlock. Locking all six
is refused (that is a static body). Dynamic bodies with a massive shape only. A lock is not carried across
the rebuild `aver_phys_body_set_motion_type` does for a static body made movable.

**Sensor.** `SetIsSensor` plus moving the body to the moving broad-phase half (a sensor sits there even
when static, as `addBody` places `aver_phys_add_sensor_*`), and the `World::sensors` table the contact
listener consults is updated under the event mutex.

## Bodies made from shapes

A body made by `aver_phys_body_from_shape` (`docs/PHYSICS_SHAPES.md`) is an ordinary body: every call on this page works on it. Two things differ from the older creators.
`aver_phys_body_position` reports the centre of mass while `aver_phys_body_origin` reports the shape's origin (they differ for any compound or offset shape), and `aver_phys_body_set_shape` /
`aver_phys_body_set_scale` re-apply the body's mass themselves, because Jolt's own shape swap would replace it by volume times density. A body label (`aver_phys_body_set_surface`)
lives in the upper half of the same user-data word the entity stamp uses, which `aver_phys_set_entity` preserves.

## Collision exclusion (G2)

`aver_phys_joint_set_exclude_collision(joint, 1)` stops the joint's two bodies colliding with each other (Godot's `exclude_nodes_from_collision`). Off for every joint by default. See `GODOT_FILTERING.md`.
