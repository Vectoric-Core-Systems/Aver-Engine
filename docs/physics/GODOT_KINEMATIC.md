# Kinematic character: Godot's `move_and_slide` (Fabric G2)

Godot's `CharacterBody3D.move_and_slide` as a controller on a **kinematic rigid body**, beside (not instead of) the CharacterVirtual-based character API, which is unchanged. Opt-in throughout: a body has no controller until `aver_phys_kc_create`, no existing call changed meaning, the determinism golden checkpoints are identical, and the controller table is empty (and untouched by the step) in a world that never uses it.

ABI: `physics_kinematic_abi.h` (`AVER_PHYS_KINEMATIC_ABI_VERSION 1`). C#: `KinematicCharacter` (Godot-like names: `Velocity`, `UpDirection`, `MoveAndSlide`, `IsOnFloor`, `IsOnWall`, `IsOnCeiling`, `GetFloorNormal`, `GetWallNormal`, `GetPlatformVelocity`, `GetRealVelocity`, `GetLastMotion`, `GetSlideCollision(i)`, ...), `KinematicCharacterOptions`. Source: `modules/physics/src/PhysicsKinematicCharacter.cpp`. Tests: `KinematicCharacterTest` (66 checks), `AbiEnumTest`.

Units and axes are the engine's: centimetres, cm/s, radians, +Z up, left-handed. Godot's numbers are metres: the converter or `Aver.Fabric` converts and passes every option. The native creation defaults are the header's (listed there, equal to Godot's numbers in cm) and `KinematicCharacterOptions.Godot` restates them; both only describe this new object, no existing call depends on them.

## How a body is moved

The script sets `velocity` (gravity included: Godot applies none), then calls `aver_phys_kc_move_and_slide(kc, dt)`. Every move is `aver_phys_body_test_motion` (recovery to half the margin, a shape cast, the contacts at the point of contact) followed by a teleport of the body (`aver_phys_body_set_position`). The body is never given a velocity, so dynamic bodies are not pushed by it (as with Godot's CharacterBody3D); what it collides with is the body's own filter (16-layer matrix, 32-bit layer/mask, exceptions, joint exclusion). Sensors never block. Shape: anything convex or compound; the body must be kinematic and not a mesh, heightfield or soft body.

`dt` is the caller's step. Call it where Godot calls it: from the per-physics-tick script, before `aver_phys_step`.

## What one call does (grounded mode)

1. **Platform carry.** If the last call ended on a floor (or wall) and the body it stood on has a Godot layer bit in `platform_floor_layers` (or `platform_wall_layers`), the platform's velocity at the body's position is read from the live body and the body is moved by it first, with the platform excluded. A body that was on a rotating platform gets the rotation at its own position.
2. **Slide loop**, up to `max_slides` times: move by the motion (`velocity * dt` the first time); classify every contact (up to 6) against `up`: within `floor_max_angle + 0.01` rad of `up` is floor, of `-up` is ceiling, anything else is wall. Then:
   - a ceiling platform pushing down gives the body at least its downward speed;
   - floor and `floor_stop_on_slope` and a velocity pointing straight down: the small travel is undone, velocity zeroed, the loop ends (standing still on a slope);
   - on the first attempt with `floor_stop_on_slope`, only the approach to the surface is done and the motion is kept whole, so the second attempt sees the motion head-on; the first attempt also drops the sideways part of a recovery that is within the margin (Godot's cancel-sliding), which is what keeps a body from creeping down a slope;
   - wall and `floor_block_on_wall` and the motion heading into the wall: a body that was on the floor does not climb it (small travel is cancelled, then it is snapped back to the floor) and only the lateral motion goes on;
   - `floor_constant_speed` going up a slope: the remaining motion is rescaled so the same distance is covered along the slope as on the flat;
   - otherwise the remainder and the velocity are slid along the contact normal, and a ceiling hit with `slide_on_ceiling` off kills the upward velocity instead.
3. **Floor snap.** If the body was on the floor, is not on it now and is not moving up, it is moved down by at most `max(floor_snap_length, safe_margin)` onto a floor surface.
4. **Velocity clean-up.** Leaning on a wall only: the horizontal velocity follows the wall's slope. On the floor and not moving up: the vertical part of the velocity is removed (gravity does not accumulate on the ground).
5. **Platform leave.** In the air with no floor or wall contact: `add velocity` adds the platform's last velocity, `add upward velocity` drops a downward component first, `do nothing` leaves the velocity alone.

Floating mode (`MOTION_MODE` 1) classifies every contact as a wall and does only the slide loop: the first slide keeps the speed, a head-on hit within `wall_min_slide_angle` of the velocity stops instead of sliding, the velocity itself is not modified.

## Results

`aver_phys_kc_state` (floor / wall / ceiling bits; the `_only` variants are `state == bit`), `aver_phys_kc_info` (floor normal, wall normal, platform velocity and body, real velocity = position change / dt, last motion, position delta, collision count) and `aver_phys_kc_collision(i)` (point, normal, collider and its velocity at the point, remainder, travel, depth, angle to `up`, entity stamp, sub-shape). A floor, wall or ceiling flag comes from any of the contacts of a move, not only the one that stopped it, so a body walking into a wall over a floor reports both.

## Differences from Godot, on purpose or by limit

- Collisions are recorded one per move (the primary contact); `get_slide_collision(i)` in Godot is the same, but the classification looks at all of a move's contacts.
- The body is teleported, so a kinematic character does not push or carry dynamic bodies, and a dynamic body resting on it is not carried. Moving the body for the physics step instead would need a velocity on it; not done.
- Collider velocity comes from the contact; for the platform carry it is the live body's velocity at the character's position.
- A body can be moved by the engine's own `body_set_position` between calls; call `aver_phys_kc_clear` after a teleport so the platform and floor state are not carried across it.
- Not in physics snapshots: the settings and results are script-side state. A rollback layer re-issues `velocity` and the call; the body's pose is in the snapshot.
- Not implemented: Godot's axis locks, `get_floor_angle` with a custom up (computed in C# from the floor normal), `move_and_collide` as a controller call (use `aver_phys_body_test_motion` and `Body.TestMotion`), pushing rigid bodies, `PLATFORM_ON_LEAVE` for a platform that was removed in the same frame.
- The loop is written from Godot's documented behaviour; edge cases at corners (two walls and a floor at once, very thin ledges) can differ from Godot in the last centimetre. The test pins the documented cases, not Godot's exact numbers.
