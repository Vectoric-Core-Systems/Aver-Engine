# Spatial queries

Shape casts, overlaps, every-hit rays and a point query, on top of the ray / sphere cast / sphere
overlap that `physics_abi.h` and `physics_layers_abi.h` already had.

- C ABI: `modules/physics/include/aver/physics/physics_queries_abi.h`
- Implementation: `modules/physics/src/PhysicsQueries.cpp` (filters shared via `PhysicsQueryFilters.hpp`)
- C#: `Physics.BoxCast / CapsuleCast / BodyCast / OverlapBox / OverlapCapsule / OverlapBody /
  RaycastAll / BodiesAtPoint`, plus `Body.Cast` and `Body.OverlapAt`, in `scripting/csharp/Aver.Physics/Queries.cs`
- Test: `tests/physics/src/QueryTest.cpp`

Units and axes are the engine's: centimetres, +X forward, +Y right, +Z up, left-handed, quaternions
(x, y, z, w).

## The three filter arguments

Every query takes `layerMask`, `ignoreBody`, `flags`, exactly as the `_ex` queries do for the first two.

| Argument | Meaning |
| --- | --- |
| `layerMask` | Bit N = collision layer N is included. `AVER_PHYS_LAYER_MASK_ALL` for everything. |
| `ignoreBody` | One handle to skip, or 0. A dead handle means "skip nothing". |
| `flags` | `AVER_PHYS_QUERY_DEFAULT` (0) or `AVER_PHYS_QUERY_INCLUDE_SENSORS` (1). |

**Sensors are ignored by default.** A sensor is a trigger volume, not a surface, so a probe or a bullet
passes through it. Pass `AVER_PHYS_QUERY_INCLUDE_SENSORS` (C#: `QueryFlags.IncludeSensors`) to make it
count. The older `_ex` queries predate the flag and still see sensors; they are unchanged.

## Shapes

- **Box**: centre, half extents (along the engine's X, Y, Z in the box's own frame), rotation.
- **Capsule**: centre, `radius`, **total** `height` (both caps included), rotation. It stands along +Z
  before the rotation. This is how `physics_shapes_abi.h` describes capsules, so a capsule you built and
  the one you query with agree. A capsule with `height <= 2 * radius` is refused with a warning.
- **Body**: an existing body's own shape (box, sphere, capsule, convex hull, compound of those...).
  Triangle-mesh, heightfield and soft bodies cannot be the moving shape and are refused with a warning
  (they are fine as the thing being hit). A character handle is not a body here.

Rotations are normalised; an all-zero quaternion means identity.

## Casts: first hit

```c
int32_t aver_phys_box_cast(cx,cy,cz, hx,hy,hz, qx,qy,qz,qw, dx,dy,dz,
                           layerMask, ignoreBody, flags, float* outDistance, float* outPoint, float* outNormal);
int32_t aver_phys_capsule_cast(cx,cy,cz, radius, height, qx,qy,qz,qw, dx,dy,dz, ...same tail...);
int32_t aver_phys_body_cast(body, dx,dy,dz, layerMask, ignoreBody, flags, ...same outs...);
```

`(dx, dy, dz)` is a **displacement**: direction and distance in one vector, in cm. The return value is the
hit body's handle, or 0 for a miss.

- `outDistance`: how far the shape's centre travelled before touching (cm), always in `[0, |d|]`.
- `outPoint`: the contact point on the **hit body's** surface.
- `outNormal`: unit surface normal pointing back toward the cast shape (away from the hit surface).
- All three out-pointers may be null.
- A shape that starts overlapping something reports distance 0, unless the sweep is moving out of it
  (Jolt's default back-face rule, as for `aver_phys_sphere_cast`).
- `aver_phys_body_cast` casts from the body's current centre-of-mass pose and never reports the body itself;
  `ignoreBody` is a second body to skip.

## Overlaps: which bodies

```c
int32_t aver_phys_overlap_box(cx,cy,cz, hx,hy,hz, qx,qy,qz,qw, layerMask, ignoreBody, flags, int32_t* outBodies, maxCount);
int32_t aver_phys_overlap_capsule(cx,cy,cz, radius, height, qx,qy,qz,qw, ..., outBodies, maxCount);
int32_t aver_phys_overlap_body(body, x,y,z, qx,qy,qz,qw, ..., outBodies, maxCount);
```

Each writes up to `maxCount` **distinct** body handles and returns how many. A count equal to `maxCount`
may be truncated, and which bodies survive truncation is unspecified.

`aver_phys_overlap_body` answers "would it fit there?": the body's shape is placed at an arbitrary
centre-of-mass pose (the quantity `aver_phys_body_position` / `aver_phys_body_rotation` report) without
moving the body. Passing the body's current pose reproduces what it overlaps now. The body itself is
never in the result. Touching without penetrating does not count as overlap.

## Every hit along a ray

```c
int32_t aver_phys_raycast_all(ox,oy,oz, dx,dy,dz, maxDistCm, layerMask, ignoreBody, flags, maxCount,
                              int32_t* outBodies, float* outDistances, float* outPoints, float* outNormals);
```

Hits are written **nearest first**. `outPoints` and `outNormals` are packed xyz (3 floats per hit, room for
`3 * maxCount`); `outDistances` is cm along the ray. Any of the four arrays may be null. When more than
`maxCount` things are hit, the nearest `maxCount` are kept.

It is one entry per **hit**, not per body: a ray through a compound or mesh reports each face it crosses,
so a body can appear twice. A ray that starts inside a solid convex body hits it at distance 0.

## Point query

```c
int32_t aver_phys_point_query(x,y,z, layerMask, ignoreBody, flags, int32_t* outBodies, maxCount);
```

Bodies whose shape contains the point, each once. Triangle meshes and heightfields have no inside and never
match.

## C# usage

```csharp
using Aver.Physics;

// Will a 60 x 60 x 180 cm box fit through the door if it moves 2 m forward?
var hit = Physics.BoxCast(centre, new Float3(30, 30, 90), Quaternion.Identity, new Float3(200, 0, 0),
                          ignoreBody: player);
if (hit.Hit) Console.WriteLine($"blocked by {hit.Body.Handle} after {hit.Distance} cm");

// Everything a shot passes through, nearest first.
foreach (var h in Physics.RaycastAll(muzzle, aim, 5000f, flags: QueryFlags.None)) { /* ... */ }

// Could this crate be put down there?
bool free = crate.OverlapAt(spot, Quaternion.Identity).Length == 0;
```

Defaults: `layerMask = Physics.LayerMaskAll`, `ignoreBody = Body.None`, `flags = QueryFlags.None`,
`maxResults = 64`.

## Tests

`tests/physics/src/QueryTest.cpp` builds scenes from bodies that never step, so every expected number is
arithmetic on the scene. The walls sit on +X, +Y and +Z in different tests and box half extents are unequal,
because a swapped axis in the engine-to-Jolt basis change only shows itself when the wall is somewhere the
wrong axis does not look.

## Godot-style queries (G2)

Ray options (back faces, from inside, bodies/areas, 32-bit mask, exclude array), `intersect_shape`, `collide_shape`, `get_rest_info`, `cast_motion` on shape handles and `body_test_motion` are in `docs/physics/SPACE_QUERIES.md`. The queries on this page are unchanged.
