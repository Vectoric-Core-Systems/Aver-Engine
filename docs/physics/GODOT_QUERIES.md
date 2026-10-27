# Godot-style space-state queries and test motion (Fabric G2)

Godot 4's `PhysicsDirectSpaceState3D` (`intersect_ray`, `intersect_shape`, `collide_shape`, `get_rest_info`, `cast_motion`) and `PhysicsServer3D.body_test_motion`, as new explicit ABI options. Nothing existing changed: the earlier queries in `physics_queries_abi.h`, `aver_phys_raycast`, the `_ex` and `_mask` forms keep their meaning, the determinism golden checkpoints are identical, and the new functions only read the world (no state to snapshot).

ABI: the "Godot-style space-state queries" section of `physics_queries_abi.h`. C#: `SpaceState.Raycast`, `IntersectShape`, `CollideShape`, `GetRestInfo`, `CastMotion`, `Body.TestMotion`, and the `RayCast3D` / `ShapeCast3D` helper objects (`SpaceState.cs`). Tests: `tests/physics/src/SpaceQueryTest.cpp`. Units are the engine's (cm, +Z up, left-handed); Godot's metres are the converter's job.

## No Godot default is built in

Every option is an argument. A `flags` of `AVER_PHYS_SQ_NONE` collides with nothing. What the converter or `Aver.Fabric` passes for a Godot default:

| Godot | Aver |
|---|---|
| `collide_with_bodies` (true) / `collide_with_areas` (false) | `AVER_PHYS_SQ_BODIES` / `AVER_PHYS_SQ_AREAS` (a sensor is an Area3D) |
| `hit_back_faces` (true) | `AVER_PHYS_SQ_BACK_FACES` |
| `hit_from_inside` (false) | `AVER_PHYS_SQ_FROM_INSIDE` |
| `recovery_as_collision` (false) | `AVER_PHYS_SQ_RECOVERY_AS_COLLISION` |
| `collision_mask` (1) | `collisionMask`, matched against a body's layer bits (`aver_phys_body_set_collision_filter`; a body without one has the bit of its 16-layer user layer) |
| `exclude` | `excludeBodies` + `excludeCount` (body or character handles; unresolved handles are skipped) |
| `margin` (0 for shape queries, 0.001 m for motion) | `marginCm` |

## Ray

`aver_phys_space_raycast` runs Jolt's ray cast with `RayCastSettings` taken from the flags: back faces apply to triangle meshes and to the exit of a convex body (`mBackFaceModeTriangles` and `mBackFaceModeConvex`), and `mTreatConvexAsSolid` is the from-inside option. A ray that starts inside a convex body and leaves it reports nothing without back faces, the exit face with them, and distance 0 with a zero normal under from-inside. Meshes and heightfields have no inside. The normal is the outward surface normal, also for a back-face hit. Jolt's own default for convex shapes is *solid*; the flag makes it explicit rather than inheriting it.

## Shape queries

The shape is a `physics_shape_builder_abi.h` handle at `pose7` (its origin; the engine adds the centre-of-mass offset). A mesh, heightfield or plane cannot be the query shape.

- `intersect_shape`: every `(body, sub-shape)` pair overlapped within the margin, deepest first (ties by handle, then sub-shape id), so results are deterministic. The margin is Jolt's `mMaxSeparationDistance`: colliders closer than it count.
- `collide_shape`: the same pairs with contact points on both sides, normal and signed depth (negative inside the margin).
- `get_rest_info`: the first of those, plus the collider's velocity at the point (zero for a sensor or a static body).
- `cast_motion`: a shape cast with Jolt's shrunken-shape mode (a long, shallow approach to a flat face is otherwise missed). *Unsafe* is the fraction at which the shape first touches. *Safe* is that fraction backed off by `margin / cos(contact angle)` (the cosine floored at 0.05), clamped to `[0, unsafe]`: the point where the shape is still a margin from the surface. With margin 0 they are equal. A collider the shape already overlaps by more than 0.05 cm is ignored for a hit at fraction 0 (Godot documents this; use `collide_shape` for those). Not covered: a collider within the margin of the END pose that the shape does not touch is not reported (Godot's inflated test would); recovery in `body_test_motion` repairs that on the next call.

## `aver_phys_body_test_motion`

The base of `move_and_collide`, `test_move` and the next package's `move_and_slide`. The body is not moved; the caller applies `TRAVEL`.

1. **Recovery.** Up to four passes: collect contacts within the margin, and solve them one at a time against the push already chosen (coplanar duplicates and corners do not overshoot) so the shape ends half a margin from each surface. A body resting in contact is therefore lifted by half the margin, as Godot's recovery does. `RECOVERY` reports it alone; `TRAVEL` includes it.
2. **Cast** from the recovered pose, as above. `collided` means `safe < 1`, or recovery had something to push out of and `RECOVERY_AS_COLLISION` was set.
3. **Contacts** at the point of contact (margin plus 0.05 cm of slack), the contact that stopped the shape first, then deepest first, up to `maxContacts`. Standing on a floor and walking into a wall reports both. `DEPTH` is the margin-inflated overlap (`penetration + margin`, floored at 0), Godot's convention.

The pair filter is the body's own, applied as the simulation applies it: the 16-layer matrix for its user layer, then the Godot layer/mask, collision exceptions and joint exclusion (the body's Jolt collision group), plus `excludeBodies`; sensors only with `AREAS`. A static body may be tested; a character, soft body or mesh/heightfield body is refused (-1).

## Limits

A CharacterVirtual's inner body is a candidate like any body, matched by its 16-layer bit (it has no Godot filter, see `GODOT_FILTERING.md`); a character handle works in `excludeBodies`, but a character cannot be the tested body. `cast_motion` and `test_motion` use one shape cast, not Godot's per-shape binary search; they agree where Godot's does, to Jolt's tolerance. Queries read the world, so call them between steps.
