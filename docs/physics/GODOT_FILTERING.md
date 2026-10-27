# Godot-style collision filtering (Fabric G2)

Opt-in alternative to the 16-layer symmetric matrix, so a Godot scene's `collision_layer` / `collision_mask`, `add_collision_exception_with` and a joint's `exclude_nodes_from_collision` behave as they do under Godot's Jolt module. Nothing changes for a project that never calls these functions: no existing call changed meaning, the determinism golden checkpoints are identical, and a world that never uses the feature creates none of its state.

ABI: `physics_layers_abi.h` (layer/mask, exceptions, mask queries) and `physics_joints_abi.h` (joint exclusion). C#: `Body.SetCollisionFilter`, `Body.CollisionFilter`, `Body.ClearCollisionFilter`, `Body.AddCollisionException` and friends, `Physics.RaycastMask`, `OverlapSphereMask`, `SphereCastMask`, `BodiesCanCollide`, `Joint.ExcludeCollision`. Tests: `tests/physics/src/GodotFilterTest.cpp`.

## The rule

Two bodies collide when `(A.mask & B.layer) != 0 || (B.mask & A.layer) != 0`. One side wanting the other is enough (the matrix is "both agree"). Layer 0 is hit by no mask, mask 0 wants nothing. Bits are Godot's: bit N is layer N+1.

## How it coexists with the matrix

- The object-layer matrix is consulted first by Jolt; both must allow a pair. Its default is "everything collides", so for a filtered pair the mask rule decides.
- The mask rule applies only when **both** bodies carry a Godot filter. A pair with an unfiltered body is decided by the matrix alone (so unfiltered legacy content is untouched by a filtered neighbour).
- Static against static never collides.
- Units: the engine never hard-codes Godot's default layer 1 / mask 1; the converter or `Aver.Fabric` sets them explicitly.

## Where it lives in Jolt, and why

A filtered or excepted body carries a Jolt `CollisionGroup` whose filter is ours. Jolt asks `GroupFilter::CanCollide` in its broad-phase pair finder (and in the CCD path), so a pair that must not collide is rejected before the narrow phase and costs one table read per candidate pair. Alternatives rejected:

- `ObjectLayerPairFilter`: object layers are 16 bits (`userLayer * 2 + moving`); 32+32 bits do not fit.
- `SimShapeFilter` / `OnContactValidate`: run per shape pair in the narrow phase after the pair has already cost broad-phase work, and need a handle lookup per body.
- Body user data: the high half already carries the surface label (PhysicsSurface.cpp).

The group id is deliberately the *invalid* group, and the sub-group id is a **slot** in a world-side table (`layer`, `mask`, "uses the mask rule"). Foreign group filters (ragdoll `GroupFilterTable`, builder soft bodies) compare group ids, so an invalid id can never clash with them, whatever the mask value. The group travels in `BodyCreationSettings`, so a body rebuilt under the same handle (static to movable) keeps its filter and exceptions with no rebinding code. The cost: a body has one collision group, so a ragdoll part or grouped soft body cannot also take a Godot filter (the setters return 0). The planned wave 11 group API (`CONTACTS_FILTERS.md` 2.4.5) would share this slot, not add a second group.

## Exceptions

`aver_phys_body_add_collision_exception(a, b)`: symmetric, works on unfiltered bodies (they get a slot with "no mask rule"), stays until removed or until either body is removed. They live in a hash keyed by the two slots; the filter looks one up only when the table is non-empty. Exceptions apply to rigid-body simulation, not to queries and not to `CharacterVirtual` movement.

## Joint exclusion

`aver_phys_joint_set_exclude_collision(joint, 1)` adds a joint-owned reference to the same table: counted per joint (two joints on a pair need both removed), ended by `aver_phys_joint_remove`, independent of `aver_phys_joint_set_enabled`, and separate from the user's exception. Default off for every joint, so existing joints behave as before; the Godot import turns it on per joint (Godot's default is on). A joint to the world has nothing to exclude.

## Queries

`aver_phys_raycast_mask`, `_overlap_sphere_mask`, `_sphere_cast_mask` take a 32-bit mask: a body is hit when `mask & layerBits != 0`, where `layerBits` is the body's Godot layer, or the single bit of its 16-layer user layer if it has no filter. They are `_ex` twins (sensors are not skipped, no exceptions). `GodotMaskBodyFilter` in `PhysicsGodotFilter.hpp` is the reusable Jolt `BodyFilter` for other queries that grow a mask.

## Behaviour notes

- Changing a filter, adding or removing an exception, or toggling a joint's exclusion wakes the affected non-static bodies, so a sleeper resting on something it no longer collides with falls.
- Setters are refused (return 0, `AbiError::Unsupported`) while `aver_phys_step` runs: the tables are read lock-free by Jolt workers.
- Not part of physics snapshots (`physics_state_abi.h`), like the layer matrix: it is configuration. A restore does not revert filters set after the snapshot. Per-body bits do not enter `aver_phys_state_hash`.
- `CharacterVirtual` is not filtered by layer/mask; it keeps the matrix. Godot's `CharacterBody3D` is meant to be re-implemented on `body_cast`/`test_move` (a separate Fabric row), which will use the mask queries.
- Existing `_ex`, `_q` and `queries` functions take the 16-bit matrix mask and are unchanged.
