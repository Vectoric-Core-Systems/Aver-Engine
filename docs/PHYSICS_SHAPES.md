# Physics shapes, physical surfaces and terrain collision

Wave 11, SHAPES_MATERIALS. What a body is made of and what its surface is made of, on Jolt: every collision shape Jolt has behind one handle table,
physical materials ("surfaces") on shapes, meshes and heightfields reported from rays, contacts, character ground and wheels, a heightfield that can be non-square,
have holes, carry materials and be edited at run time, and the authoring that sits on it (`.ocshape`, `.ocsurf`, `PHYSSURFACE`, the `CollisionShape` component, the editor).
The design is `docs/plans/wave11/SHAPES_MATERIALS.md`; this page is what was built and where it differs from it.

Units and axes are the engine's everywhere: centimetres, +X forward, +Y right, +Z up, left-handed, quaternions `(x, y, z, w)`. Nothing here is
new third-party code (`docs/plans/wave11/SHAPES_MATERIALS.md` section 2.14): every shape class was already compiled into Jolt, and the collision-mesh simplifier is the
already vendored meshoptimizer.

## 1. Shape handles

One process-wide, mutex-guarded handle table holds every shape (`modules/physics/src/PhysicsShapeTable.cpp`). `aver_phys_release_mesh_shape` and
`aver_phys_add_mesh_shape_body` keep working and accept any handle; `aver_phys_shape_release` is the neutral name. A shape handle is not a body handle: the two tables
are separate, so hand a shape to a shape function and a body to a body function. Builders and release are safe from any thread (level streaming builds on workers); body
functions are main thread only. A body keeps its shape alive, so releasing right after `aver_phys_body_from_shape` is correct. The table is not cleared at shutdown.

`modules/physics/include/aver/physics/physics_shape_builder_abi.h` (32 functions, `AVER_PHYS_SHAPE_ABI_VERSION 1`):

| Kind | Calls |
|---|---|
| Leaves | `box`, `sphere`, `capsule` (height TOTAL), `cylinder` (height FULL), `tapered_capsule`, `tapered_cylinder`, `triangle` (a thin convex), `plane` (static or kinematic only), `empty`, `convex_hull`, `mesh` |
| Decorators | `rotated_translated`, `offset_com` (the centre of mass moves, the geometry does not), `scaled` (Jolt's `ScaleShape`: baked when it can be, refused when it cannot) |
| Compounds | `compound(kind, ...)`, static or mutable, per-child position, rotation and user data, 1 to 65536 children |
| Handles | `release`, `count`, `info`, `of_body` |
| Bodies | `body_from_shape`, `body_set_shape`, `body_set_scale`, `body_scale`, `body_origin`, `body_set_enhanced_edges`, `body_compound_add/remove/set_child/count`, `body_triangles`, `vehicle_set_chassis_shape` |

Leaves take a surface id (0 = none) and an optional parameter block `SP` (convex radius, density, hull tolerance, mesh edge angle, build quality, triangles per leaf; a negative entry means the shape's own default).
A mesh takes a palette of up to 32 surface ids, one palette byte per triangle and optional per-triangle user data.

The existing creators (`aver_phys_add_*_capsule`, `_cylinder`, `_tapered_capsule`, `_compound_boxes`, `_convex_hull`, `_mesh`, `_heightfield`) are re-routed through the same builders, so there is
one implementation of each shape; `tests/physics/src/ShapeBuilderTest.cpp` pins the old and the new routes to the same motion.

**Bodies from shapes.** The body is created at the shape ORIGIN; `aver_phys_body_position` still reports the centre of mass, `aver_phys_body_origin` the origin, and any pose write-back of a compound or offset shape must use the origin.
A shape with no mass of its own (a mesh) may be dynamic or kinematic when given a mass: its centre of mass moves to the bounds centre and its inertia is a box's. A dynamic mesh collides with convex shapes
only and falls through meshes and heightfields (Jolt has no mesh-mesh collision); a plane and a heightfield cannot be dynamic. A sensor can be any shape.

**Changing a shape.** `body_set_shape` and `body_set_scale` wrap `BodyInterface::SetShape` with the mass handled by the wrapper (Jolt's own mass update replaces the mass by volume times density, so the wrapper passes false and re-applies):
`keepMass` keeps the old mass, otherwise the new shape's. The body ORIGIN stays where it was; a new centre of mass moves the centre, not the pose a model hangs from. Scale is absolute, relative to the unscaled shape. Both are refused for a
character, a soft body, a vehicle chassis and a ragdoll part. Mutable-compound edits need a body whose shape IS the mutable compound it owns (a mutable compound belongs to one body); each keeps the mass; adding or removing invalidates raw sub-shape ids taken before.

**Introspection.** `aver_phys_shape_info` gives type, volume, mass at its density, centre of mass, bounds, inner radius, principal inertia with its frame, child, leaf and triangle counts, bytes. `aver_phys_body_triangles` returns the body's collision triangles
inside a region, in world centimetres, one surface per triangle; the editor overlay and the tests read it.

## 2. Physical surfaces

A surface is a NAME hashed to a 32-bit id (FNV-1a over UTF-8, never 0; `modules/core/include/aver/core/SurfaceId.hpp`, `Surfaces.IdOf` in C#, the `an_surfaceid` node). The physics registry keys on that same hash, so a ray on a mesh, a contact on a crate and `Landscape.Sample` on the terrain
answer in one id space. `modules/physics/include/aver/physics/physics_surface_abi.h` (15 functions, `AVER_PHYS_SURFACE_ABI_VERSION 1`):

- **Registry** (process-wide, survives world shutdown): `register` (upsert by name; refuses a hash collision with a different name), `tune`, `info`, `count`, `at`, `reset`. Friction and restitution below 0 defer to the body; both clamp to 0..10. `tag` is an opaque integer for games (a footstep group, an FX class).
- **Materials.** Building any shape with a surface id interns one shared `AverSurface` (a Jolt `PhysicsMaterial`) for it, so ids need no registering first and editing a surface edits every shape that uses it. A material sits where Jolt has a slot: one per convex leaf, a palette of 32 per mesh, 256 per heightfield.
- **Friction and restitution** go through Jolt's own combine hook, installed at `aver_phys_init`: a side with a leaf surface that states a number uses it instead of the body's; Jolt's rules (`sqrt(f1 f2)` and `max(r1 r2)`) combine the two. With no surface ever interned the hook computes exactly Jolt's default expressions, so a project that never touches a surface is bit-identical to before. The landscape's own surface sampler still runs after it and still wins.
- **Body label.** `aver_phys_body_set_surface(body, surface, copyPhysics)` stores a surface id in the upper 32 bits of the body's user data (the entity id keeps the lower 32; `aver_phys_set_entity` preserves the label). A label is identity for queries; it does not change contact friction, `copyPhysics` writes the registered numbers into the body once.
- **Hits report what was hit.** `aver_phys_raycast_detail` (body, entity, raw sub-shape id, surface, friction, restitution, tag, compound child and its user data, mesh triangle user data, heightfield cell), `aver_phys_subshape_info`, `aver_phys_contact_detail` (index-aligned with `aver_phys_contact_get`: both surfaces, closing speed, applied friction and restitution), `aver_phys_character_ground_detail`, `aver_phys_vehicle_wheel_surface`. The wheels read the leaf material's friction. Shape casts keep their outputs.

**Engine data.**

- `Content/Physics/Surfaces.ocsurf` (`modules/formats/include/aver/formats/OcSurf.hpp`): `SURFACE <name> [friction f] [restitution r] [tag n]`. Both hosts load it when a project opens, before any level builds a shape (`Runtime/include/aver/game/PhysicalSurfaces.hpp`: `loadPhysicalSurfaces`); authored numbers beat what a landscape layer registers (layers use find-or-add).
- `.ocmat` gains `PHYSSURFACE <name>` (`OcMatExtras::physSurface`; written only when set, so every older file round-trips byte for byte). `GameContent::collisionMeshFor` reads each kept slot's surface, groups triangles by surface, simplifies each group with its borders locked (no crack opens at a surface border), and hands the physics mesh a palette (32 entries; more fall into surface 0 with a warning). The collision cache is version 2 and stores a per-slot signature, so editing a slot's `PHYSSURFACE` or alpha mode rebuilds it. The material is read from the compiled `Binaries/Materials/<slot>.ocmat` first, then `Content/Materials`: a Compile C# regenerates the first and drops the record, so author a lasting one in Content.
- `LevelInstance` passes the palette through `InstantiateOptions::localTriangleSurfacesFor`; the landscape gives its heightfield a per-cell dominant-layer palette.

## 3. Terrain

`modules/physics/include/aver/physics/physics_terrain_abi.h` (6 functions, `AVER_PHYS_TERRAIN_ABI_VERSION 1`): `heightfield_shape` returns a shape handle (make the body with `aver_phys_body_from_shape`); non-square fields are padded with no-collision to Jolt's square;
a NaN or `FLT_MAX` sample is a hole (a hole at a sample removes the two cells on its diagonal whole and half of the other two, Jolt's rule); `cellPaletteIndex` is one byte per cell into a palette of up to 256 surfaces; `HP` sets block size, bits per sample, edge angle and edit headroom.
`set_heights` and `set_cell_surfaces` clone the shape, edit the clone and `SetShape` it (Jolt's race-free route); values outside the encoded range clamp and are counted (`HFI_CLAMPED_TOTAL`); `AVER_PHYS_HF_WAKE` wakes dynamic bodies over the area. `aver_phys_add_heightfield` is built through the same code with an empty parameter block and keeps its exact bits.

## 4. `.ocshape` and the CollisionShape component

`.ocshape` (`modules/formats/include/aver/formats/OcShape.hpp`, grammar in its header and in `docs/formats/FORMAT_SPECS.md`) is a text shape tree: leaves, decorators (`rt`, `com`, `scale`), static or mutable compounds with `CHILD` records, one `ROOT`.
`game::buildColliderShape` (`Runtime/include/aver/game/ColliderShapes.hpp`) walks it children first and issues the builder calls, releasing every handle but the root's. `hull mesh` and `mesh` leaves read geometry through a `ColliderMeshSource` (a host gives `GameContent::collisionMeshFor`); they are built at load, not cooked. A mesh leaf's slot surfaces win over the leaf's `surface`, which wins over the file's default.

`CCollisionShape` (32 bytes, registered at run time like `CCloth`, no `Components.hpp` growth) sits beside `CRigidBody`: the body supplies motion type, mass, friction, restitution, damping and layer, the component the shape and a surface label. `COMPONENT CollisionShape asset=Physics/Chair.ocshape surface=Wood motion=dynamic noscale=1`;
canonical writing omits defaults and spells a surface the registry does not name `#<hex id>`. Two things differ from the design and are deliberate: no level file carries a `CRigidBody` (it is created by the editor's Add Component or a script), so applying the record attaches a default (static) one when the entity has none (removing the component takes back that zero-size body, `removeCollisionShape`, so the undo of Add Component is exact), and a `motion` key (`kinematic`, `dynamic`; an absent key is `static`, like every canonical default, so undo of a motion change restores it) sets its motion type so a level can say "this falls".
**The editor's Play builds these bodies and only the editor's Play** (`sandbox/src/PhysicsSceneSync.cpp`), as it does for `CRigidBody`; the packaged game builds shapes through C# (`Aver.Physics.CollisionShape`). A body from a shape takes the entity's world scale unless `noscale`, the `.ocshape` `MASS` when the Rigid Body names none, and keeps its shape handle in `outShape` until Play stops.
**Nothing writes a `CRigidBody`'s pose back to its entity** (design open question 7, still open): a dynamic body falls, slides and settles invisibly and its mesh stays where it was placed; Show > Collision Shapes draws the body. `aver_phys_body_origin` is what a write-back must read.

## 5. Editor

- **Add Component > Collision Shape** (a mesh placement; it attaches a default static Rigid Body itself, because the editor has no UI to add one and no level file carries one; a Vehicle is refused, it is its own body) and the **Details section** (`sandbox/src/CollisionShapeDetailsSection.cpp`, rules in `sandbox/src/CollisionShapeDetailsModel.hpp`): the asset row, **Create .ocshape from the Rigid Body**, a surface combo, a **Motion** combo (static, kinematic, dynamic: the one number of the Rigid Body the level record keeps), "Ignore entity scale", Play-time read-outs of the built shape, and a surface probe (`aver_phys_raycast_detail` from the viewport centre or straight down from the entity).
- **`.ocshape` tab** (`sandbox/src/OcShapeEditor.cpp`, structural rules in `sandbox/src/OcShapeEditorModel.hpp`): the node outline, the selected node's numbers and options, wrap in a decorator, add to a compound, reorder, rename, delete with cascade, an issues list that is the format's validation, snapshot undo, and a wire preview of the built shape.
- **Project Settings > Physics > Surfaces** (`sandbox/src/PhysicalSurfacePanel.cpp`, rules in `sandbox/src/PhysicalSurfaceTableModel.hpp`): the table of `Surfaces.ocsurf`; Save writes the file and registers the rows, so friction edits are live in Play. The material inspector gets a **Physical Surface** combo that patches the `PHYSSURFACE` line of the `.ocmat` file the collision build reads, leaving every other byte of the file alone.
- **Show > Collision Shapes** (`sandbox/src/CollisionShapeOverlay.cpp`): the edges of the collision triangles of the selected entity's body and of every moving body, tinted by surface (a stable hue per id), with a yellow cross at the centre of mass and a cyan one at the origin. Static bodies keep Show > Colliders. Compound child bounds are not drawn (the ABI reports a shape's total bounds, not each child's).

## 6. C# and Aver Node

`Aver.Physics`: `CollisionShape` (`scripting/csharp/Aver.Physics/CollisionShape.cs`), `ShapeParams`, `ShapeChild`, `ShapeInfo`, the `Body` members of `BodyShape.cs` (`Shape()`, `SetShape`, `SetScale`, `Origin`, `AddChild`, `RemoveChild`, `ChildCount`, ...), `Surfaces`, `SurfaceHit` and `Physics.RaycastDetail` (`Surfaces.cs`, `SurfaceHits.cs`), `Terrain` (`Terrain.cs`).
`Aver.Framework`: `PhysicalSurfaces` (`scripting/csharp/Aver.Framework/PhysicalSurfaces.cs`: `Raycast`, `Beneath`, `Find`, `Register`, `Tune`, `Info`) and the `Entity` members of `EntitySurface.cs`. Family `PhysicalSurface` v1 in Aver Node has seven rows: `an_raycastsurface`, `an_surfacebeneath`, `an_surfaceinfo`, `an_setbodysurface`, `an_registersurface`, `an_tunesurface`, `an_setbodyscale`.

## 7. Limits, said plainly

No convex decomposition of a mesh (no new third-party code): `hull mesh` gives a one-piece approximation and authors build compounds of hulls. No mesh-mesh or mesh-heightfield collision (Jolt has none). A raw sub-shape id is not stable across a mutable-compound edit, a `set_shape`, a heightfield edit or a release; `CHILD` and `CELL_*` are the stable identities.
A material per heightfield sample does not exist (it is per cell), and runtime painting does not update the cells (`set_cell_surfaces` is there for a game that wants it). Shape casts and overlaps do not report sub-shape detail. There is no impact-sound system: surface, tag and closing speed are the data, mapping them to sounds is a data table in game content.
Non-square heightfields cost the square's memory. Large-world positions follow the LARGE_WORLD area's convention when it lands; every position here is a float array.

## 8. Tests and content

`tests/physics/src/ShapeBuilderTest.cpp`, `SurfaceTest.cpp`, `TerrainTest.cpp` (physics); `tests/formats/src/OcShapeTest.cpp`, `OcSurfTest.cpp`; `tests/game/src/ColliderShapesTest.cpp`, `PhysicalSurfacesTest.cpp`, `CollisionMeshSurfaceTest.cpp`;
`tests/editor/src/CollisionShapeDetailsModelTest.cpp`, `OcShapeEditorModelTest.cpp`, `PhysicalSurfaceTableModelTest.cpp`; `scripting/csharp/Aver.Graph.Tests/PhysicalSurfaceNodeTests.cs` and `PhysicsShapeApiTests.cs`. Every test margin is an unverified starting value until the first run.
`test-content/PhysicsShapesLab/` is the engine test content (`make_shapes_lab.py` writes it; its README lists the owner checks, and says which of the design's checks it cannot offer).
