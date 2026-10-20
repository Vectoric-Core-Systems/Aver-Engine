# Shapes, physical materials and terrain collision on Jolt (wave 11, SHAPES_MATERIALS)

Design contract for the implementing agents. Owner decisions in force: integrate the rest of Jolt Physics, large-world coordinates (`JPH_DOUBLE_PRECISION`) and Jolt 5.6 Hair are IN, Jolt ObjectStream and PhysicsScene files are OUT. Nothing here is built or run. Rules: upgrade the existing systems and never add parallel ones, no new third-party code beyond enabling code already vendored in Jolt (this area needs none: every shape class used below is already compiled, and the collision-mesh simplifier is the already vendored meshoptimizer), lean comments, D3D12 and Vulkan parity (this area adds no GPU work: the one overlay is `rhi::LineVertex` lines both backends already draw), never delete files, ABI changes additive (new functions, new headers). Format and depth follow `docs/plans/wave10/VEHICLES.md`.

Every claim in section 1 was read in the tree on 2026-10-19 (HEAD `3000c220`) with a `file:line`; line numbers are pointers, grep the symbol when one is off. **UNVERIFIED** marks anything reasoned from reading but not confirmed by running it. Every number that is a tuning value is an UNVERIFIED starting value until the headed pass in section 6. The survey the task quoted is right on every physics claim and silent on six facts that change the design; they are listed in 1.8.

**Scope.** Rigid-body collision shapes and what a surface is made of: generic shape handles and the shapes Jolt has that the ABI does not reach, compounds (static and mutable), decorators, runtime shape and scale change, shape introspection, dynamic mesh bodies, physical materials ("surfaces") on meshes, heightfields and every shape, reported from raycasts, contacts, character ground and vehicle wheels, and a heightfield that can be non-square, have holes, carry materials and be edited at run time. Not here: collision filtering and queries beyond the ray (QUERIES area), debug rendering of every shape type (the debug-draw area; sections 2.4 and 2.12 use a triangle read-out instead), large-world position widening (LARGE_WORLD area; every position below is a float array and gains a `_d` twin only if that area defines the convention), joints, ragdolls, soft bodies, vehicles beyond one chassis call.

---

## 0. Decisions in one screen

1. **One shape-handle table for everything.** Today a "shape handle" means "a static triangle mesh" (`PhysicsWorld.cpp:174-183`). The table becomes generic (`PhysicsShapeTable.cpp`): every builder in this area returns a handle in the same space, `aver_phys_release_mesh_shape` and `aver_phys_add_mesh_shape_body` keep working and now accept any handle, and `aver_phys_shape_release` is the neutral name for the same release. No second table.
2. **Three new headers, 53 new functions, nothing existing is edited in signature.** `physics_shape_builder_abi.h` (32, `AVER_PHYS_SHAPE_ABI_VERSION 1`), `physics_surface_abi.h` (15, `AVER_PHYS_SURFACE_ABI_VERSION 1`), `physics_terrain_abi.h` (6, `AVER_PHYS_TERRAIN_ABI_VERSION 1`). `physics_abi.h`, `physics_shapes_abi.h` and `Native.cs` are not edited. Existing creators (`aver_phys_add_*_capsule`, `_cylinder`, `_tapered_capsule`, `_compound_boxes`, `_convex_hull`, `_mesh`, `_heightfield`) are re-routed through the new internal builders so there is one implementation of each shape, and are pinned bit-for-bit by tests.
3. **Shape kinds added:** tapered cylinder, triangle, plane, empty; convex radius and hull tolerance as parameters; rotated-translated, offset-centre-of-mass and scaled decorators for any shape; static compound of any shapes with per-child position, rotation and user data; mutable compound (single owner, add/remove/move children at run time). Meshes gain active-edge angle, build quality, triangles per leaf, per-triangle user data and per-triangle surfaces.
4. **A surface is a name hashed to a 32-bit id, the id the engine already uses.** `Aver.Framework.Surface.IdOf` (FNV-1a over UTF-8, never 0) and the graph rows `an_surfaceid` and `an_landscapesurface` already hand scripts that id (`Landscape.cs:131-145`, `GraphTerrainNodeTable.cs:19-20`). The physics registry keys on exactly that hash, so a raycast on a mesh, a contact on a crate and `Landscape.Sample` on the terrain all answer in one id space. No dense or per-world ids; the registry is process-wide and outlives worlds like the shape table does.
5. **Materials are Jolt `PhysicsMaterial` objects.** One shared `AverSurface : JPH::PhysicsMaterial` per id holds friction, restitution (negative = defer to the body) and a user tag, so editing a surface edits every shape that uses it. Shapes carry them where Jolt already has a slot: one per convex leaf, a palette of up to 32 per mesh (Jolt packs the index in 5 bits, `MeshShape.h:212`), up to 256 per heightfield (`HeightFieldShape.h:255`). Compounds resolve per leaf. A body-level label (for shapes with no material) lives in bits 32 to 63 of `Body::UserData`; the entity id uses the low 32 bits and every reader truncates to `int32` (sites in 1.3).
6. **Friction and restitution become sub-shape aware through Jolt's own hook.** `PhysicsSystem::SetCombineFriction/Restitution` are today unset for bodies (only the vehicle controller sets its own). A captureless function installed at `aver_phys_init` replaces `body.GetFriction()` with the leaf material's value when it has one and keeps Jolt's combine rule (`sqrt(f1 f2)`, `max(r1 r2)`). With no surface ever interned the function takes the body values, so every existing project is bit-identical. The landscape surface sampler still runs after it and still wins (`PhysicsWorld.cpp:132-147`).
7. **Hits report what was hit.** New, additive: `aver_phys_raycast_detail` (body, entity, raw sub-shape id, surface, friction, restitution, tag, compound child and its user data, mesh triangle user data, heightfield cell), `aver_phys_subshape_info` (the same from any body and sub-shape id), `aver_phys_contact_detail` (both surfaces, closing speed, applied friction and restitution, index-aligned with `aver_phys_contact_get`), `aver_phys_character_ground_detail`, `aver_phys_vehicle_wheel_surface`. The shape casts keep their current outputs (open question 12).
8. **Runtime shape and scale change.** `aver_phys_body_set_shape` and `aver_phys_body_set_scale` wrap `BodyInterface::SetShape`, which **replaces** mass by shape volume times density when asked to update mass properties (`Body.cpp:117-125`), so the wrapper always passes false and re-applies the old mass itself. Scale is absolute, relative to the unscaled shape, and goes through `Shape::ScaleShape` (which bakes shapes that cannot be wrapped, `Shape.cpp:233-254`).
9. **Dynamic mesh bodies exist, with Jolt's limits stated.** A mesh body may be dynamic or kinematic when created from a shape with a mass: inertia from the local bounds box, centre of mass moved to the bounds centre. It collides with convex shapes (boxes, spheres, capsules, hulls, compounds of them) and **not with other meshes or heightfields** (Jolt has no mesh-mesh collision; `physics_queries_abi.h:34-36` already says so). `aver_phys_body_set_motion_type(dynamic)` on a mass-less body stays refused (`PhysicsBody.cpp:177-188`).
10. **Introspection:** `aver_phys_shape_info` (type, volume, mass at its density, centre of mass, local bounds, inner radius, principal inertia with its frame, child and leaf counts, triangle count, bytes) and `aver_phys_body_triangles` (the collision triangles of a body inside a region, in world centimetres, with a surface per triangle), which is the feed for a shape-accurate overlay and for tests.
11. **Heightfield v2 is its own header.** Non-square (padded with no-collision to Jolt's square), holes (NaN or `FLT_MAX` samples), per-cell surface palette, block size and bits per sample, edit headroom, and in-place `set_heights` / `set_cell_surfaces` that clone the shape, edit the clone and `SetShape` it (Jolt's documented race-free route, `HeightFieldShape.h` class comment). `aver_phys_add_heightfield` keeps its exact bits.
12. **Vehicle chassis from any shape:** `aver_phys_vehicle_set_chassis_shape(v, shape)` beside `aver_phys_vehicle_set_chassis_boxes` (`physics_vehicle_ext_abi.h:221`). Two one-line edits make wheels read the leaf material's friction (`PhysicsVehicle.cpp:512`, `PhysicsVehicleExt.cpp:347`).
13. **Engine wiring follows data already in the tree.** `.ocmat` gains an optional `PHYSSURFACE <name>` record (extras, omitted when empty); `GameContent::collisionMeshFor` (which already reads each slot's `.ocmat` to drop alpha-masked slots, `GameContent.cpp:1499-1508`) now also groups triangles by slot surface and hands the physics mesh a palette; `LevelInstance` passes it through a new optional callback; the landscape gives its heightfield a per-cell dominant-layer palette and registers each layer's surface and friction. A project table `Content/Physics/Surfaces.ocsurf` names surfaces and their numbers.
14. **Authoring shapes:** a text asset `.ocshape` (a shape tree) and a runtime-registered companion component `CCollisionShape` (32 bytes) beside `CRigidBody`, honoured by `PhysicsSceneSync` (so sensors can finally be any shape). **Editor Play only, as `CRigidBody` is** (1.6); the packaged game gets shapes through C# and scripts.
15. **C# and Aver Node.** `Aver.Physics`: `CollisionShape`, `ShapeChild`, `ShapeParams`, `ShapeInfo`, `Surfaces`, `SurfaceHit`, `Terrain`, and `Body` members. `Aver.Framework`: `PhysicalSurfaces`, `Entity` surface members. Family `PhysicalSurface` v1: 7 rows, written to the wave 9 C contract.
16. **Editor:** Add Component "Collision Shape", a Details section, an `.ocshape` tab, a Project Settings "Physical Surfaces" table, a "Physical Surface" combo in the material inspector, and a shape-accurate wire for the selected and moving bodies. **Not in v1, said plainly:** convex decomposition of a mesh (no new third-party code; `hull` leaves are built from authored points or a mesh's vertices), mesh-mesh collision, a live sub-shape id after a mutable compound edit, a material per heightfield sample (it is per cell), sub-shape detail from shape casts and overlaps, an impact-sound system (surfaces and tags are the data; mapping them to sounds is a data table), Unreal's friction-combine modes, and moving the packaged game to build `CRigidBody` bodies.
17. **Size XL, seven phases** (section 4). A and B carry the physics risk, D the widest wiring.

---

## 1. What exists

### 1.1 Shapes

| Fact | Where |
|---|---|
| A "shape handle" is a static triangle mesh only: `aver_phys_create_mesh_shape`, `_release_mesh_shape`, `_add_mesh_shape_body` (static body, optional scale through `ScaleShape`) | `physics_abi.h:333-358`; table `PhysicsWorld.cpp:174-183`, create `:898-935`, release `:937-940`, body `:962-986` |
| The handle table is process-wide, mutex-guarded, **not cleared at shutdown**, with its own counter (a shape handle and a body handle can be the same integer; only the function tells them apart) | `PhysicsWorld.cpp:168-176` |
| Rigid shapes the ABI reaches: box, sphere (`PhysicsWorld.cpp:500-530`), convex hull (`:839-852`), capsule, cylinder, tapered capsule, compound of boxes (`PhysicsShapes.cpp:98-230`), mesh and heightfield (static) | `physics_abi.h:46-58,312-319,363-365`; `physics_shapes_abi.h:46-93` |
| **Not reachable:** tapered cylinder, triangle, plane, empty, mutable compound. Jolt compiles all of them (`Collision/Shape/{TaperedCylinderShape,TriangleShape,PlaneShape,EmptyShape,MutableCompoundShape}.cpp`) | grep of `modules/physics/src` finds no use |
| Compound is boxes only, **identity rotation per box** by design (`compound.AddShape(offset, JPH::Quat::sIdentity(), ...)`), offsets and half extents in the body frame | `PhysicsShapes.cpp:81`; `physics_shapes_abi.h:83-84` |
| `RotatedTranslatedShape`, `OffsetCenterOfMassShape` and `ScaledShape` exist **inside** the module (vehicle chassis, ragdoll parts, `ScaleShape` for placements) and are not exposed | `PhysicsVehicle.cpp:326-328,424-428`; `PhysicsRagdoll.cpp:271`; `PhysicsWorld.cpp:971` |
| No creator takes a convex radius, a density or a material: every Jolt default is used (`cDefaultConvexRadius` 0.05 m; box and cylinder clamp it to their smallest extent, so thin shapes work: `BoxShape.cpp:55-58`, `CylinderShape.cpp:90-94`) | `PhysicsShapes.cpp:109-209`; `PhysicsSettings.h:16` |
| Hull tolerance is Jolt's 1 mm and the hull is refused under 4 points; mesh active-edge angle (5 degrees), build quality (runtime performance) and triangles per leaf (8) are defaults | `PhysicsWorld.cpp:841,847,884,926`; `MeshShape.h:56,69,78`; `ConvexHullShape.h:36` |
| Mesh vertices are converted with the axis map and the winding reversed (the map has determinant -1); out-of-range triangles are skipped with a warning; the same loop is written twice | `PhysicsWorld.cpp:855-890` and `:898-935` |
| `Shape::ScaleShape` accepts any non-zero scale, mirrors included, and **bakes** a shape that cannot be wrapped (non-uniform sphere) | `Shape.cpp:233-265`; the engine uses it once, `PhysicsWorld.cpp:971` |
| Introspection today is `aver_phys_body_aabb` only; the reason (no shape tree in the ABI) is in its comment | `PhysicsWorld.cpp:562-585` |
| Vehicle chassis: boxes only (1 to 8), rebuilt under the same handle; mass and centre of mass kept | `physics_vehicle_ext_abi.h:221`; `PhysicsVehicle.cpp:88,401-448` |

### 1.2 Bodies, mass and shape change

| Fact | Where |
|---|---|
| `addBody(shape, centre, dynamic, mass, sensor, layer)` creates at identity rotation; the position is the shape **origin**, while `aver_phys_body_position` reports the **centre of mass** (they differ for any compound or offset shape; the 94 cm rest height in `ShapeTest` is a centre-of-mass number) | `PhysicsWorld.cpp:210-236`, `:555-560`; `PhysicsShapes.cpp:51-71` (comment) |
| No creator takes a rotation, a motion type, a layer or a sensor flag beyond box and sphere; `PhysicsSceneSync` applies them after creation and refuses sensors that are not box or sphere | `PhysicsSceneSync.cpp:87-133` |
| Dynamic mesh bodies are refused: `add_mesh` is static only, `set_motion_type(dynamic)` refuses a shape with no mass, `giveMotionProperties` gives a kinematic mesh a 1000 kg placeholder | `physics_abi.h:316,153-154`; `PhysicsBody.cpp:128-141,177-188` |
| Jolt does not enforce `MustBeStatic`; it simply has no collision function for mesh vs mesh or mesh vs heightfield | `Shape.h:207`; `MeshShape.h:93-94`; `physics_queries_abi.h:34-36` |
| There is no `SetShape` and no scale change at run time. Jolt's `SetShape(update mass = true)` **replaces** mass and inertia from the new shape's volume and density; it does not keep the old mass | `Body.cpp:117-143`; `BodyInterface.cpp:298-326` |
| A static body switched to movable is rebuilt under the same handle with a new `BodyID`; the user data is copied (`GetBodyCreationSettings`) | `PhysicsBody.cpp:75-155` |

### 1.3 Materials and surfaces

| Fact | Where |
|---|---|
| `PhysicsMaterial` is used nowhere in the module; no shape gets one; `Shape::GetMaterial` is never called. A surface today is a **body's** friction (`aver_phys_body_set_friction`) | grep of `modules/physics/src`; `physics_abi.h:216-220` |
| `SetCombineFriction` / `SetCombineRestitution` are never set for bodies. In this Jolt they are **plain function pointers** taking `(Body, SubShapeID, Body, SubShapeID)`, so a hook is a captureless function reading globals | `ContactConstraintManager.h:53-63,554-555`; the only `SetCombineFriction` in the tree is the wheel controller's, `PhysicsVehicle.cpp:510` |
| The one existing per-point friction mechanism is the **terrain surface sampler**: a body flagged `surfaceSampled` has the other body's friction combined with a host-sampled value in `OnContactAdded`/`OnContactPersisted`, after Jolt's own combine | `physics_water_abi.h:48-57`; `PhysicsWorld.cpp:107-147`; host `GameLandscape.cpp:624-634` |
| A surface is a name in four places already, none connected to physics: landscape `LAYER ... surface <name> friction restitution` (`LayerInfo`, `GroundInfo`), `Aver.Framework.Surface.IdOf(name)` = FNV-1a 32 over UTF-8 (0 for empty, a zero hash becomes 1), the graph rows `an_surfaceid` and `an_landscapesurface`, and `.oclanes` `SURFACE <id> <name> grip rolling ...` (traffic only) | `GameLandscape.hpp:248-262`; `Landscape.cs:131-145`; `GraphTerrainNodeTable.cs:19-20`; `FORMAT_SPECS.md:422-427,824-826` |
| The entity stamp is `Body::SetUserData(uint64(entity))`; every reader truncates to `int32` | writes `PhysicsWorld.cpp:779,783`, `PhysicsSoftBody.cpp:829`; reads `PhysicsWorld.cpp:822`, `PhysicsLayers.cpp:159`, `PhysicsVehicleExt.cpp:348`, `PhysicsCharacter.cpp:509` |

### 1.4 Queries, events, character, vehicle

| Fact | Where |
|---|---|
| `aver_phys_raycast` reads `hit.mSubShapeID2` but only to get a normal; nothing reports a sub-shape, a material, a triangle or a compound child. `raycast_ex` and `raycast_all` are the same | `PhysicsWorld.cpp:790-834` (`:825`); `PhysicsLayers.cpp:121-170`; `PhysicsQueries.cpp:265` |
| Contacts record bodies, point and normal; the Jolt manifold carries both sub-shape ids and the listener drops them. Contacts are cleared at the top of each `aver_phys_step` | `PhysicsInternal.hpp:161`; `PhysicsWorld.cpp:84-104`; `physics_abi.h:429-436` |
| Character ground: state, normal, position, body, velocity. Jolt also keeps the ground material and sub-shape id (`GetGroundMaterial`, `GetGroundSubShapeID`) | `PhysicsCharacter.cpp:193-240`; `CharacterBase.h:116-125` |
| Wheel contact telemetry reports the contact body, entity and `SURFACE_FRICTION = body.GetFriction()`; Jolt's wheel has `GetContactSubShapeID` | `PhysicsVehicleExt.cpp:347-348`; `Wheel.h:82` |
| The wheel grip is `min(1, surface.GetFriction() / 0.2) * GRIP_SCALE` | `PhysicsVehicle.cpp:79,510-515` |

### 1.5 Heightfield

| Fact | Where |
|---|---|
| `aver_phys_add_heightfield(samples, sampleCount, spacingCm, cx, cy, cz)`: square only, every sample passed through, Jolt defaults for block size 2, 8 bits per sample, active-edge 5 degrees, offset zero, scale `(spacing, 1, spacing)`, no materials, no holes, static | `PhysicsWorld.cpp:989-1010`; defaults `HeightFieldShape.h:91,96,110` |
| Orientation: sample (column x, row y) lands at engine `(cx - y*spacing, cy + x*spacing, cz + height)` | `physics_abi.h:360-362` |
| Jolt limits: sample count is rounded up to a block multiple and padded no-collision; 14 bits per axis (16384 samples); a hole is `FLT_MAX`; materials are `uint8` indices into a list of up to 256; `SetHeights` needs block-aligned rectangles, re-compresses a border, clamps to the encoded range `[mOffset.y, mOffset.y + mScale.y * 65534]`, and Jolt's advice is to edit a `Clone()` and `SetShape` it | `HeightFieldShape.h:23,29,53-60,128-140,200-256` |
| Consumer: the landscape builds one heightfield body per section and, on failure, logs "built" anyway because `add_heightfield` returns 0 and the check is `landscapeBody_ >= 0` (a defect, fixed in phase D) | `GameLandscape.cpp:68-92`; `PhysicsBridge.cpp:5-30` |

### 1.6 Engine consumers of collision geometry

| Fact | Where |
|---|---|
| `GameContent::collisionMeshFor(id)`: drops alpha-masked and translucent material slots (`collisionSlotCollides`, which reads each slot's `.ocmat`), welds by position, simplifies with meshoptimizer toward 2 cm with `LockBorder`, may take Trifactor's coarsest LOD when nothing was dropped, disk-caches under `Saved/DerivedDataCache/Collision` (`kCollisionCacheVersion = 1`). The result is positions and indices only | `GameContent.hpp:195-211`; `GameContent.cpp:907-1010,1040-1130,1499-1559` |
| `LevelInstance` builds one mesh shape per unique mesh id through that callback and one scaled static body per placement; falls back to `aver_phys_add_mesh`, then to a fitted box | `LevelInstance.cpp:184-239`; `LevelInstance.hpp:182`; `GameLevel.cpp:193-201`; the editor's callbacks `SandboxMcp.cpp:750`, `SandboxSelection.cpp:1057` |
| **`CRigidBody` bodies are built by the sandbox only** (`syncPhysicsFromScene`, called when Play starts), not by the packaged Runtime; shape kinds are box, sphere, capsule, cylinder; sensors only box or sphere. **Nothing in C++ writes a dynamic body's pose back to its entity** (grep of `kComponentRigidBody` finds creation and one possession teleport): UNVERIFIED that no C# path does | `PhysicsSceneSync.cpp:47-133,256-283`; `SandboxPlay.cpp:152,1379`; `Components.hpp:268-290` |
| Material inspector sliders live in `SandboxPanels.cpp:280` area; `.ocmat` extras are `OcMatExtras` (shader, cull, parent, `graphRef`), parsed at `OcMat.cpp:191,258`, written at `:641-644` | `OcMat.hpp:13-33` |

### 1.7 C# and Aver Node

| Fact | Where |
|---|---|
| `Aver.Physics.Shapes` has capsule, cylinder, tapered capsule, box compound; `Physics.AddConvexHull/AddMesh/AddHeightfield`; `Physics.Raycast*` returns `RaycastHit(Body, Entity, Point, Normal)`; `Body` and `Physics` are `partial` | `Shapes.cs`, `Physics.cs:43-62,120-152,262-295`, `Body.cs:21`, `Queries.cs:185` |
| The parity test pairs headers with `Native*.cs` files by row and checks every C# import against the header (one direction: an export without an import is allowed) | `AbiEnumTest.cpp:551-580,935-970` |
| `Aver.Framework.Physics` is a shim, not `partial`; `RaycastHit` there adds `Entity`; the landscape answers ground queries by name through `Landscape.Sample` | `Physics.cs:86-108,160`; `Landscape.cs:62-99` |

### 1.8 What the survey got right, and what it did not say

Right and confirmed: generic handles (1.1), the missing shapes, boxes-only compound with no rotation, no mutable compound, convex radius and hull tolerance and mesh build options at defaults, dynamic mesh refusal, no `SetShape` or scale change, no introspection, boxes-only vehicle chassis, no materials anywhere, heightfield square without holes, materials, edit or options, and no material or sub-shape in hits, contacts or ground.

Refined or missing, each changes the design:

1. **Decorators already exist inside the module** (1.1); exposing them is a wrapper, not new Jolt code.
2. **The id space is already decided** by `Surface.IdOf` and two graph rows (1.3); a dense id would split it.
3. **`SetShape(update mass)` throws the mass away** (1.2); the wrapper must re-apply it.
4. **The combine hooks are function pointers** (1.3); no capture, globals only.
5. **Terrain already has per-point friction** through the sampler (1.3); it must keep winning, and the heightfield's own cell materials add identity (names for FX), not a second friction path.
6. **`CRigidBody` exists only in the editor's Play** and has no pose write-back (1.6); a shape component inherits both facts. Also noted while reading: the landscape's "built" log fires on a failed build (1.5).

---

## 2. Design

### 2.1 Architecture and the Unreal mapping

```
 callers: C++ hosts, C# (Aver.Physics / Aver.Framework), Aver Node, PhysicsSceneSync
        |
 physics_shape_builder_abi.h  physics_surface_abi.h  physics_terrain_abi.h        (new, additive)
        |
 PhysicsShapeTable (handle -> Ref<Shape>)   PhysicsSurface (registry, AverSurface, combine hook)
 PhysicsShapeBuilder (makeBox ... makeMesh)  PhysicsShapeBody (from_shape, set_shape, scale, compound edit)
        |
 existing creators (capsule, cylinder, compound boxes, hull, mesh, heightfield) re-routed here
        |
 Jolt: Convex/Compound/Decorated shapes, MeshShape, HeightFieldShape, PhysicsMaterial, BodyInterface
```

| Unreal | Here |
|---|---|
| Body Setup with simple collision shapes (box, sphere, capsule, convex) | `.ocshape` tree + `CCollisionShape` beside `CRigidBody`; `CollisionShape` handles in C# |
| Complex collision as simple / use complex | mesh leaf; dynamic mesh bodies with the stated limits |
| Physical Material asset (friction, restitution, surface type) | a surface: `.ocsurf` record, `AverSurface`, `PHYSSURFACE` on the `.ocmat` slot |
| Phys Material Mask / landscape layer physical material | heightfield cell palette from the dominant paint layer; the terrain sampler keeps blended friction |
| `FHitResult::PhysMaterial`, `GetSurfaceType` | `SurfaceHit`, `aver_phys_raycast_detail`, contact and ground detail |
| Skeletal and static mesh "Body Instance" scale | `aver_phys_body_set_scale` |
| Landscape heightfield with holes (visibility layer), runtime modification | NaN holes, `aver_phys_heightfield_set_heights` |

### 2.2 Shape handles: conventions, leaves, decorators, compounds

**Conventions.** Engine units and axes as the rest of the ABI: centimetres, +X forward, +Y right, +Z up, left-handed, quaternions `(x, y, z, w)`. A `quatXyzw` of NULL is identity; a `posCm` of NULL is zero. Half extents and radii are sizes: the axis permutation applies, the sign flip does not. A capsule's height is TOTAL (both caps), a cylinder's and tapered cylinder's is FULL end to end, matching `physics_shapes_abi.h:17-24`; all stand along +Z, `topRadius` at +Z. Handles come from the one table; a handle of 0 is invalid. **A shape is not a body**: handing a shape handle to a body function (or the reverse) resolves in the wrong table by design, as it already does for meshes; the functions are distinct and the numbers may coincide. Builders and `release` are safe from any thread (level streaming builds on workers); body functions are main-thread only. A shape stays alive while a body uses it, whatever the table says (Jolt reference counting), so releasing right after `body_from_shape` is correct and expected.

**Parameter block `SP`** (`float p[n]`, any prefix, more than defined is refused, unset entries keep the default; a negative value means "the shape's own default"):

| Index | Name | Unit | Default | Used by |
|---|---|---|---|---|
| 0 | `CONVEX_RADIUS` | cm | 5 (triangle 0) | box, cylinder, tapered cylinder, hull, triangle; clamped by Jolt to the smallest extent |
| 1 | `DENSITY` | kg/m^3 | 1000 | every convex leaf and hull (mass when a body is made with `massKg <= 0`) |
| 2 | `HULL_TOLERANCE` | cm | 0.1 | hull (Jolt `mHullTolerance` is 1 mm) |
| 3 | `EDGE_ANGLE_DEG` | degrees | 5 | mesh; negative = every edge active (then bodies touching it want `BP_ENHANCED_EDGES`) |
| 4 | `BUILD_QUALITY` | 0 or 1 | 0 | mesh: 0 favour runtime performance, 1 favour build speed (procedural and destruction meshes) |
| 5 | `MAX_TRIS_PER_LEAF` | 1..8 | 8 | mesh |

`COUNT` = 6. The surface is a separate `int32_t surface` argument (a 32-bit hash cannot ride in a float block); 0 means none.

**Leaves** (all return a handle or 0; `aver_phys_last_error` says why):

- `box(hx, hy, hz)`, `sphere(r)`, `capsule(r, height)`, `cylinder(r, height)`, `tapered_capsule(top, bottom, height)` (half-heights derived exactly as `PhysicsShapes.cpp:174-180`), **`tapered_cylinder(top, bottom, height)`** (`TaperedCylinderShapeSettings(halfHeight, top, bottom, convexRadius)`; Jolt returns a `CylinderShape` when the radii are equal).
- **`triangle(abc[9])`**: Jolt's convex `TriangleShape` (a thin convex), vertices through `toJolt`, convex radius default 0. Not a mesh; for one-off blades, ramps and test geometry.
- **`plane(normal[3], offsetCm, halfExtentCm, surface)`**: the points with `n . x = offset` form the plane and the side opposite the normal is solid (Jolt: negative half space; its constant is `-offset/100`). `halfExtentCm` bounds the broadphase box (Jolt default 1000 m is far too big for a 100 m level; the ABI requires a value, 1 to 1e6). Static or kinematic bodies only (`MustBeStatic`).
- **`empty(com[3])`**: no volume, collides with nothing; the placeholder shape, and the way to make a kinematic anchor for a joint.
- `convex_hull(points, count, surface, p, n)`: at least 4 non-coplanar points; more than Jolt's 256-vertex limit is reduced by Jolt (`MaxVerticesReached` is accepted, as the existing creator does implicitly); the hull tolerance and convex radius come from `SP`.
- `mesh(vertices, vertexCount, indices, indexCount, triPaletteIndex, palette, paletteCount, triUserData, p, n)`: the existing conversion and winding (`PhysicsWorld.cpp:855-890`) moved into one `buildMeshShape` that `aver_phys_add_mesh`, `aver_phys_create_mesh_shape` and this call share. `palette` is up to **32** surface ids, `triPaletteIndex` one byte per input triangle indexing it (NULL = all default); triangles skipped for bad indices or degeneracy are dropped from all three arrays in lockstep (Jolt's `Sanitize` also removes duplicate and degenerate triangles; each triangle's material index and user data travel with it). A palette over 32 is refused with the limit in the log (open question 8). `triUserData` (one `int32` per triangle, NULL = none) turns on Jolt's per-triangle user data (about 25% more memory) and is what `HITI_TRIANGLE` returns, typically the source triangle index.

**Decorators and compounds** (children are shape handles):

- `rotated_translated(inner, pos, quat)`, `offset_com(inner, offset)` (the centre of mass moves by `offset`, the geometry does not), `scaled(inner, scale[3])` (`Shape::ScaleShape`: a scale the shape cannot wrap is baked into an equivalent shape when Jolt can and refused when it cannot; zero components are refused; the engine's axis permutation without sign changes, as `PhysicsWorld.cpp:945-961` derives).
- `compound(kind, children[], posCm[], quatXyzw[], childUserData[], count)`: `kind` is `AVER_PHYS_COMPOUND_STATIC` (0) or `_MUTABLE` (1). Per-child position and rotation in the compound's frame, per-child `uint32` user data (Jolt's `SubShapeSettings::mUserData`, read back as `HITI_CHILD_USER_DATA`). Count 1 to 65536 (Jolt may refuse earlier when the packed sub-shape id would exceed 32 bits; its message is logged). The compound **origin** is the frame the children were given in; its centre of mass is Jolt's mass-weighted one. A **mutable** compound is **single-owner**: the first `body_from_shape` binds it to that body and a second use is refused (`InvalidArgument`), because editing a shape two bodies share would need two notifications and Jolt's own advice is one owner. A static compound has no such rule.

**Handle functions:** `release(shape)` (same as `aver_phys_release_mesh_shape`), `count()` (diagnostic: live handles), `info(shape, ...)`, `of_body(body)` (a new handle on the body's current shape, caller releases; refused for a character or a soft body).

### 2.3 `physics_shape_builder_abi.h`

```c
#define AVER_PHYS_SHAPE_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_shape_abi_version(void);                                   /* 1 */

/* ---- leaves ---- */
AVER_PHYS_API int32_t aver_phys_shape_box(float hx, float hy, float hz, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_sphere(float radius, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_capsule(float radius, float height, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_cylinder(float radius, float height, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_tapered_capsule(float topRadius, float bottomRadius, float height, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_tapered_cylinder(float topRadius, float bottomRadius, float height, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_triangle(const float* abcXyz, int32_t surface, const float* p, int32_t n);          /* 9 floats */
AVER_PHYS_API int32_t aver_phys_shape_plane(const float* normalXyz, float offsetCm, float halfExtentCm, int32_t surface);
AVER_PHYS_API int32_t aver_phys_shape_empty(const float* comXyz);
AVER_PHYS_API int32_t aver_phys_shape_convex_hull(const float* pointsXyz, int32_t count, int32_t surface, const float* p, int32_t n);
AVER_PHYS_API int32_t aver_phys_shape_mesh(const float* verticesXyz, int32_t vertexCount, const int32_t* indices, int32_t indexCount,
                                           const uint8_t* triPaletteIndex, const int32_t* palette, int32_t paletteCount,
                                           const int32_t* triUserData, const float* p, int32_t n);

/* ---- decorators and compounds ---- */
#define AVER_PHYS_COMPOUND_STATIC  0
#define AVER_PHYS_COMPOUND_MUTABLE 1
AVER_PHYS_API int32_t aver_phys_shape_rotated_translated(int32_t inner, const float* posCm, const float* quatXyzw);
AVER_PHYS_API int32_t aver_phys_shape_offset_com(int32_t inner, const float* offsetCm);
AVER_PHYS_API int32_t aver_phys_shape_scaled(int32_t inner, const float* scaleXyz);
AVER_PHYS_API int32_t aver_phys_shape_compound(int32_t kind, const int32_t* children, const float* posCm, const float* quatXyzw,
                                               const int32_t* childUserData, int32_t count);

/* ---- handles and introspection ---- */
AVER_PHYS_API int32_t aver_phys_shape_release(int32_t shape);
AVER_PHYS_API int32_t aver_phys_shape_count(void);
AVER_PHYS_API int32_t aver_phys_shape_info(int32_t shape, float* outF, int32_t nF, int32_t* outI, int32_t nI);      /* AVER_PHYS_SI_* / _SII_* */
AVER_PHYS_API int32_t aver_phys_shape_of_body(int32_t body);

/* ---- bodies ---- */
AVER_PHYS_API int32_t aver_phys_body_from_shape(int32_t shape, const float* originCm, const float* quatXyzw,
                                                int32_t motionType, float massKg, const float* p, int32_t n);       /* AVER_PHYS_BP_* */
AVER_PHYS_API int32_t aver_phys_body_set_shape(int32_t body, int32_t shape, int32_t keepMass);
AVER_PHYS_API int32_t aver_phys_body_set_scale(int32_t body, const float* scaleXyz, int32_t keepMass);
AVER_PHYS_API int32_t aver_phys_body_scale(int32_t body, float* outXyz);
AVER_PHYS_API int32_t aver_phys_body_origin(int32_t body, float* outXyz);
AVER_PHYS_API int32_t aver_phys_body_set_enhanced_edges(int32_t body, int32_t on);
AVER_PHYS_API int32_t aver_phys_body_compound_add(int32_t body, int32_t child, const float* posCm, const float* quatXyzw, int32_t userData);  /* index or -1 */
AVER_PHYS_API int32_t aver_phys_body_compound_remove(int32_t body, int32_t index);
AVER_PHYS_API int32_t aver_phys_body_compound_set_child(int32_t body, int32_t index, const float* posCm, const float* quatXyzw);
AVER_PHYS_API int32_t aver_phys_body_compound_count(int32_t body);                                                 /* -1 when not a compound */
AVER_PHYS_API int32_t aver_phys_body_triangles(int32_t body, const float* regionMin, const float* regionMax,
                                               float* outXyz, int32_t* outSurface, int32_t maxTriangles);
AVER_PHYS_API int32_t aver_phys_vehicle_set_chassis_shape(int32_t v, int32_t shape);
```

`aver_phys_body_compound_add` (the new child index, or -1) and `aver_phys_body_compound_count` (-1 when the body is not a compound) follow the precedent of `aver_phys_body_motion_type`: a value where 0 is a real answer, -1 for failure, `aver_phys_last_error` set.

**Info blocks.** `SI` floats: `0 VOLUME_CM3`, `1 MASS_KG` (volume times density; 0 for a mesh), `2..4 COM` (shape-local cm), `5..7 BOUNDS_MIN`, `8..10 BOUNDS_MAX` (re-sorted per axis after the axis map, as `aver_phys_body_aabb` does), `11 INNER_RADIUS`, `12..14 INERTIA_PRINCIPAL` (kg cm^2 at that mass), `15..18 INERTIA_FRAME` (quaternion x y z w, principal frame in shape space), `COUNT 19`. `SII` ints: `0 TYPE` (`AVER_PHYS_SHAPE_TYPE_*`: UNKNOWN 0, BOX 1, SPHERE 2, CAPSULE 3, CYLINDER 4, TAPERED_CAPSULE 5, TAPERED_CYLINDER 6, CONVEX_HULL 7, TRIANGLE 8, PLANE 9, EMPTY 10, MESH 11, HEIGHTFIELD 12, STATIC_COMPOUND 13, MUTABLE_COMPOUND 14, ROTATED_TRANSLATED 15, SCALED 16, OFFSET_COM 17, SOFT_BODY 18), `1 CHILDREN` (direct children of a compound, 0 otherwise), `2 LEAVES` (recursive), `3 TRIANGLES` (`Shape::GetStats`), `4 BYTES` (saturated to int32), `5 MUST_BE_STATIC`, `6 HAS_MASS`, `COUNT 7`. Units: Jolt reports m^3 and kg m^2; the ABI multiplies by 1e6 and 1e4. The inertia conversion (a similarity transform under the axis map, principal order kept, the frame through `fromJolt(Quat)`) is **UNVERIFIED** until `ShapeBuilderTest` reconstructs a box's engine-axis tensor from it.

### 2.4 Bodies from shapes, shape and scale change, compound edit, dynamic meshes

**`BP` block** (`float p[n]`): `0 LAYER` (0), `1 SENSOR` (0), `2 INERTIA_FROM_BOUNDS` (0; forced to 1 for a mass-less shape asked to be dynamic or kinematic with a mass), `3 ENHANCED_EDGES` (0, `BodyCreationSettings::mEnhancedInternalEdgeRemoval`), `4 ACTIVATE` (1), `COUNT 5`.

`body_from_shape(shape, originCm, quat, motionType, massKg, p, n)`:

- The body is created at `originCm` (the shape **origin**) with `quat`, registered in the same three tables `addBody` fills (`bodies`, `byId`, `sensors`), and returns an ordinary body handle. `aver_phys_body_position` still reports the centre of mass; **`aver_phys_body_origin` reports the origin** and is what any pose write-back must use for compounds (open question 7).
- Motion type uses `AVER_PHYS_MOTION_*`. `massKg > 0` sets `CalculateInertia` with that mass; `<= 0` takes the shape's volume times density. `MustBeStatic` shapes (plane, heightfield, and meshes without a mass) are refused as dynamic; a **mesh** with `massKg > 0` is accepted as dynamic or kinematic: the shape is wrapped in an `OffsetCenterOfMassShape` that puts the centre of mass at the local bounds centre, and mass and inertia are `SetMassAndInertiaOfSolidBox(boundsSize, massKg / boundsVolume)`. A concave mesh's inertia is therefore a box's: documented as rough.
- A sensor uses the same path as `aver_phys_add_sensor_*` (moving broadphase half), for **any** shape, which removes the box-or-sphere restriction (`PhysicsSceneSync.cpp:87-96`).
- The warn-once body limit message is the existing `warnBodyLimit`.

`body_set_shape(body, shape, keepMass)`: refused for a character, a soft body, a body in a vehicle or ragdoll (their owners rebuild shapes), and a `MustBeStatic` shape on a non-static body. It calls `BodyInterface::SetShape(id, shape, false, Activate)` and, for a body with motion properties, then sets the mass properties itself from `shape->GetMassProperties()`: with `keepMass` the old mass (`ScaleToMass`), without it the shape's. Jolt's `UpdateCenterOfMassInternal` already shifts the body so its **centre-of-mass world position is unchanged** when the new shape's centre of mass differs, so a swap does not teleport a resting body (UNVERIFIED for the sleeping-neighbour case; `ShapeBuilderTest` covers the awake one).

`body_set_scale(body, scale, keepMass)`: **absolute** (the shape at scale 1 is the reference). If the body's shape is a `ScaledShape` (a placement made by `aver_phys_add_mesh_shape_body`, or a previous call), the inner shape is unwrapped first; then `inner->ScaleShape(permuted scale)` and the same swap as `set_shape`. Mass follows volume by default (density constant); `keepMass` holds it. `body_scale` reads the current scale (1,1,1 when unscaled). Scale permutation: `jolt = (sy, sz, sx)`, no sign changes, derived in `PhysicsWorld.cpp:945-961`.

`body_compound_*`: valid only for a body whose shape is (possibly inside decorators) a `MutableCompoundShape` it owns. `add` calls `AddShape`, `remove` `RemoveShape`, `set_child` `ModifyShape`; each then `AdjustCenterOfMass()` and `NotifyShapeChanged(id, previousCom, false, Activate)` and re-applies the mass as above. Removing or inserting **invalidates every raw sub-shape id** taken before (child indices shift); the header says so, and `HITI_CHILD` is an index, not an id.

**Dynamic mesh limits, repeated where a caller will meet them:** a dynamic or kinematic mesh body collides with convex shapes only. Dropped onto a mesh floor or a heightfield it falls through. Use hulls or compounds of convex pieces for anything that must land on terrain; there is no automatic decomposition (no new third-party code).

**`body_triangles(body, regionMin, regionMax, out, outSurface, maxTriangles)`**: the body's collision triangles intersecting the world box (NULL region = the body's AABB), 9 floats each in engine centimetres, one surface id per triangle (0 for none), through `Shape::GetTrianglesStart/Next` with the body's transform; returns the count, truncated at `maxTriangles` (a heightfield or a big mesh needs a region). Convex shapes return their tessellation. Winding is Jolt's, mirrored by the axis map: edges are right, normals from winding are not guaranteed. This is not Jolt's `DebugRenderer` (that is the debug-draw area's).

**Vehicle chassis.** `aver_phys_vehicle_set_chassis_shape(v, shape)` rebuilds the chassis body under the same handle exactly as `aver_phys_vehicle_set_chassis_boxes` does (`PhysicsVehicle.cpp:401-448`: new `BodyCreationSettings` with the old pose and user data, mass from `VehicleEntry::massKg`, the centre of mass forced to the builder's request with `OffsetCenterOfMassShape(shape, com - shape->GetCenterOfMass())`, old body removed, `byId` and the handle's `bodies` entry rewritten, `boxes` cleared). Refused after `_finish`, for a mass-less shape and for a `MustBeStatic` one. The shape is in the vehicle origin frame (bottom centre of the car). It lives in `PhysicsShapeBody.cpp` and uses only `findVehicle` and `VehicleEntry`, so `PhysicsVehicle.cpp` is not touched for it.

### 2.5 Introspection and the existing creators

`aver_phys_shape_of_body(body)` + `aver_phys_shape_info` answers "what is this body made of" for every body the old creators made. The old creators are re-routed: `aver_phys_add_static_capsule(...)` becomes `makeCapsule(r, h, surface 0, defaults)` then `addBody`; `buildBoxCompound` in `PhysicsShapes.cpp` becomes a call to the shared static-compound builder with identity rotations and boxes made by `makeBox`; `add_convex_hull` and `add_mesh` call `makeHull` / `buildMeshShape` with defaults. The pin: `ShapeTest`, `PhysicsTest`, `QueryTest`, `LayerTest`, `RagdollTest` and `VehicleTest` stay green unchanged, and a new test builds each old shape both ways and compares 300 steps of poses to 1e-5 cm.

### 2.6 `physics_surface_abi.h`

**Identity.** `aver_phys_surface_id(utf8Name)` is a pure function: FNV-1a 32 over the UTF-8 bytes, 0 for NULL or empty, a zero hash becomes 1, returned as `int32` (so "Ice" is -714267872 and "Grass" is 1479220765: golden values pinned in tests, computed from the algorithm in `Landscape.cs`). The same function is an inline in `Aver.Core` (`SurfaceId.hpp`) for hosts without the physics module, and `Aver.Framework.Surface.IdOf` is made to call `Aver.Physics.Surfaces.IdOf` (same code, one definition). Names are case-sensitive.

**Registry** (process-wide, mutex-guarded writes, lock-free reads of the numbers, not cleared by `aver_phys_shutdown`):

- `register(utf8Name, friction, restitution, tag)`: upsert by name; returns the id, or 0 when the hash collides with a different registered name (refused, logged; practically never). Friction and restitution below 0 mean "defer to the body"; both clamp to 0..10. `tag` is an opaque `int32` for games (a footstep group, an FX class).
- `tune(surface, friction, restitution, tag)`: change the numbers of an id (registered or only interned by a shape). Live: shapes share the object, so every body on that surface changes at its next new contact (existing contact constraints keep the value they were created with until they are rebuilt; UNVERIFIED how fast Jolt rebuilds a persisting pair, measured in section 6).
- `info(surface, &friction, &restitution, &tag, nameBuf, nameCap)`: 1 when the id is known (registered, or interned because a shape was built with it, in which case the name is empty and the numbers are -1/-1/0).
- `count()`, `at(index)` (dense, for editors), `reset()` (clears names and numbers back to unset; objects stay alive for the shapes holding them).

**Interning.** Building any shape with a non-zero surface id (leaf argument, mesh palette, heightfield palette, body label) interns an `AverSurface` for it, so ids never need registering first. Registration only adds the name and numbers.

**Resolution of "the surface at this sub-shape of this body":** (1) `body.GetShape()->GetMaterial(subShapeId)`; if it is an `AverSurface`, its id; else (2) the body label (`UserData >> 32`), which `aver_phys_body_set_surface` writes; else (3) 0. Characters and soft bodies resolve to 0 by (1) and (2); the character ground is the exception and uses Jolt's own ground material (detail blocks below).

**Friction and restitution rule (the combine hook).** At `aver_phys_init`, after `system.Init`, install two captureless functions with `SetCombineFriction` / `SetCombineRestitution`. For each side: the leaf material's number when the material is an `AverSurface` with a non-negative value, else the body's own. Friction combines as `sqrt(f1 * f2)` and restitution as `max(r1, r2)`: Jolt's own rules, unchanged (open question 4). While the registry has never interned an id (one relaxed atomic load) the functions evaluate exactly Jolt's default expressions on the body values, so a project that never touches surfaces runs the same arithmetic bit for bit. A body label does **not** change contact friction (it is identity); `aver_phys_body_set_surface(body, surface, copyPhysics = 1)` additionally copies the registered friction and restitution into the body once, through the existing setters, for the common "make this crate rubber" call.

**The body label** lives in bits 32 to 63 of `Body::UserData`. `aver_phys_set_entity` is changed (shared edit, `PhysicsWorld.cpp:774-787`) to preserve the upper half: `userData = (userData & 0xFFFFFFFF00000000) | uint32(entity)`; the cast through `uint32` also fixes a negative entity sign-extending into the label. Every entity read truncates to `int32` and ignores the upper bits (sites in 1.3); `PhysicsSoftBody.cpp:829` writes the whole word for a soft body, which has no label and is left alone. Because the label is in the body, it survives the static-to-movable rebuild (`GetBodyCreationSettings` copies `mUserData`) and `aver_phys_vehicle_set_chassis_*`.

**Precedence with the landscape sampler.** The listener's `applySurfaceFriction` runs after Jolt's combine and overwrites `mCombinedFriction` for a flagged terrain body, so it wins over a heightfield cell material on the same contact; the cell material still supplies identity to every query. The landscape registers each layer's `surface` name with its `friction` and `restitution` **only if the name is not already registered**, so an authored `.ocsurf` is authoritative (2.8).

```c
#define AVER_PHYS_SURFACE_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_surface_abi_version(void);                                   /* 1 */
AVER_PHYS_API int32_t aver_phys_surface_id(const char* utf8Name);
AVER_PHYS_API int32_t aver_phys_surface_register(const char* utf8Name, float friction, float restitution, int32_t tag);
AVER_PHYS_API int32_t aver_phys_surface_tune(int32_t surface, float friction, float restitution, int32_t tag);
AVER_PHYS_API int32_t aver_phys_surface_info(int32_t surface, float* outFriction, float* outRestitution, int32_t* outTag,
                                             uint8_t* outName, int32_t nameCap);       /* UTF-8, NUL-terminated; C# byte[] */
AVER_PHYS_API int32_t aver_phys_surface_count(void);
AVER_PHYS_API int32_t aver_phys_surface_at(int32_t index);
AVER_PHYS_API void    aver_phys_surface_reset(void);
AVER_PHYS_API int32_t aver_phys_body_set_surface(int32_t body, int32_t surface, int32_t copyPhysics);
AVER_PHYS_API int32_t aver_phys_body_surface(int32_t body);                                  /* -1 for a dead handle, 0 = none */

AVER_PHYS_API int32_t aver_phys_raycast_detail(float ox, float oy, float oz, float dx, float dy, float dz, float maxDistCm,
                                               uint32_t layerMask, int32_t ignoreBody, int32_t flags,
                                               float* outF, int32_t nF, int32_t* outI, int32_t nI);                    /* body or 0 */
AVER_PHYS_API int32_t aver_phys_subshape_info(int32_t body, int32_t subShape, float* outF, int32_t nF, int32_t* outI, int32_t nI);
AVER_PHYS_API int32_t aver_phys_contact_detail(int32_t index, float* outF, int32_t nF, int32_t* outI, int32_t nI);
AVER_PHYS_API int32_t aver_phys_character_ground_detail(int32_t ch, float* outF, int32_t nF, int32_t* outI, int32_t nI);
AVER_PHYS_API int32_t aver_phys_vehicle_wheel_surface(int32_t v, int32_t wheel, float* outF, int32_t nF, int32_t* outI, int32_t nI);
```

(That is 15 functions with the version call.) `layerMask`, `ignoreBody` and `flags` mean what they mean in `physics_queries_abi.h` (sensors skipped unless `AVER_PHYS_QUERY_INCLUDE_SENSORS`); `aver_phys_raycast_detail` is the same cast as `aver_phys_raycast_ex` plus the sensor flag and the detail blocks; it reuses the filters already shared in `PhysicsQueryFilters.hpp` (`MaskLayerFilter`, `handleForBodyId`, `ignoredIdOf`, `QueryBodyFilter`) rather than adding a fourth copy.

**Detail blocks.** Hit floats `AVER_PHYS_HITF_*`: `0..2 POINT`, `3..5 NORMAL`, `6 DISTANCE_CM`, `7 FRICTION` (the resolved surface's value, -1 when it defers), `8 RESTITUTION`, `COUNT 9`. Hit ints `AVER_PHYS_HITI_*`: `0 BODY`, `1 ENTITY`, `2 SUB_SHAPE` (the raw 32-bit `SubShapeID` value; valid only while the shape is unchanged), `3 SURFACE`, `4 TAG`, `5 CHILD` (top-level compound child index, -1 none), `6 CHILD_USER_DATA`, `7 TRIANGLE` (mesh per-triangle user data when the mesh stores it, else -1), `8 CELL_X`, `9 CELL_Y` (heightfield cell, physics order, else -1), `COUNT 10`. `subshape_info` writes the same blocks from any `(body, subShape)` with `POINT/NORMAL/DISTANCE` zero. Contact floats `AVER_PHYS_CONF_*`: `0 CLOSING_SPEED_CMS` (relative speed at the contact point along the normal, sampled in `OnContactAdded` before the solver), `1 FRICTION`, `2 RESTITUTION` (as applied, after the sampler), `COUNT 3`; contact ints `AVER_PHYS_CONI_*`: `0 SUB_SHAPE_A`, `1 SUB_SHAPE_B`, `2 SURFACE_A`, `3 SURFACE_B`, `COUNT 4`; `contact_detail(i)` is index-aligned with `aver_phys_contact_get(i)` and cleared with it. Character ground and wheel surface use the hit blocks (body, sub-shape and surface from Jolt's ground fields and the wheel's `GetContactBodyID/SubShapeID`; the character's ground material is read from `GetGroundMaterial`; a heightfield built without a cell palette answers surface 0, and `PhysicalSurfaces.Beneath` then asks `Landscape.Sample` for an entity-less ground body).

**Contact bookkeeping.** `ContactEvent` (`PhysicsInternal.hpp:161`) gains `subA, subB, surfA, surfB, closing, friction, restitution`; `OnContactAdded` fills them (one virtual call per side; no allocation). This is the only listener change.

### 2.7 Meshes with materials, and `physics_terrain_abi.h`

**Mesh palette** is described in 2.2. The surfaces of a mesh live in Jolt's per-triangle material index (5 bits in the packed triangle flags), so there is no extra memory for them; `mPerTriangleUserData` is the only optional cost.

**Heightfield.**

```c
#define AVER_PHYS_TERRAIN_ABI_VERSION 1
AVER_PHYS_API int32_t aver_phys_terrain_abi_version(void);                                   /* 1 */
AVER_PHYS_API int32_t aver_phys_heightfield_shape(const float* samples, int32_t cols, int32_t rows, float spacingCm,
                                                  const uint8_t* cellPaletteIndex, const int32_t* palette, int32_t paletteCount,
                                                  const float* p, int32_t n);                                         /* AVER_PHYS_HP_* */
AVER_PHYS_API int32_t aver_phys_heightfield_set_heights(int32_t body, int32_t x0, int32_t y0, int32_t w, int32_t h,
                                                        const float* heights, int32_t flags);
AVER_PHYS_API int32_t aver_phys_heightfield_get_heights(int32_t body, int32_t x0, int32_t y0, int32_t w, int32_t h, float* outHeights);
AVER_PHYS_API int32_t aver_phys_heightfield_set_cell_surfaces(int32_t body, int32_t x0, int32_t y0, int32_t w, int32_t h,
                                                              const uint8_t* cellPaletteIndex, const int32_t* palette, int32_t paletteCount);
AVER_PHYS_API int32_t aver_phys_heightfield_info(int32_t body, float* outF, int32_t nF, int32_t* outI, int32_t nI);   /* AVER_PHYS_HFI_* / _HII_* */
```

- `heightfield_shape` returns a shape handle; `aver_phys_body_from_shape(shape, origin, quat, STATIC, 0, ...)` makes the body. `samples` is `cols * rows` row-major in the **v1 physics order**, so sample (column x, row y) lands at engine `(cx - y*spacing, cy + x*spacing, cz + height)` exactly as `aver_phys_add_heightfield` documents (`physics_abi.h:360-362`), and the landscape bridge is reused unchanged. A sample that is NaN or `FLT_MAX` is a **hole** (Jolt's no-collision value; a hole removes both triangles of every cell touching the sample).
- **Non-square.** Jolt is square: the shape is built `N x N` with `N = max(cols, rows)` rounded up to a block multiple, the extra samples no-collision. Memory is `N^2` samples whatever the rectangle (an 8:1 strip costs 8x; open question 9). `cols`, `rows` must be 2 to 16384 (14 bits per axis, `HeightFieldShape.h:29`); `spacingCm > 0`.
- `cellPaletteIndex` is `(cols-1) * (rows-1)` bytes (NULL = no cell materials) indexing `palette` (up to 256 surface ids). The Jolt material list is built with `mMaterials[i] = AverSurface(palette[i])`.
- **`HP` block:** `0 BLOCK_SIZE` (2, range 2 to 8: bigger saves memory and slows queries), `1 BITS_PER_SAMPLE` (8, 1 to 16), `2 EDGE_ANGLE_DEG` (5), `3 EDIT_HEADROOM_CM` (0), `COUNT 4`. **Quantisation.** Jolt compresses relative to each block's range, then edits clamp to the global range fixed at creation. With headroom 0 the range is the sample min to max, so a crater below the lowest sample cannot be represented; `EDIT_HEADROOM_CM` widens `mMinHeightValue/mMaxHeightValue` by that much each way. The default (no `p`) is Jolt's, which is `aver_phys_add_heightfield`'s, and `aver_phys_add_heightfield` is rebuilt on this path with an empty `p` and pinned bit for bit by `TerrainTest`.
- **Edit.** `set_heights(body, x0, y0, w, h, heights, flags)` takes any rectangle of samples (NaN = hole). The body's shape must be a `HeightFieldShape` (directly; a scaled heightfield is refused). The implementation aligns the rectangle outward to block multiples, reads the existing values for the margin with `GetHeights`, overlays the new ones, calls `Clone()`, `SetHeights` on the clone (with the world's `TempAllocator`) and `BodyInterface::SetShape(id, clone, false, DontActivate)`; with `AVER_PHYS_HF_WAKE` (flag 1) it then wakes dynamic bodies in the changed AABB (`ActivateBodiesInAABox`) so a ball resting on a flattened hill falls. The clone costs a copy of the field (about N^2 bytes at 8 bits, UNVERIFIED) per call: batch an edit per frame. Precision loss at the left and top margin is Jolt's (`SetHeights` comment). Values outside the encoded range are clamped, not refused: the call returns 1 and `aver_phys_heightfield_info` reports `HFI_CLAMPED_TOTAL`, the running count of clamped samples since the shape was made, which a test or a sculpt tool can diff before and after.
- `get_heights` returns decompressed values (so slightly different from the input, `HeightFieldShape.h` says so), holes as `FLT_MAX`.
- `set_cell_surfaces` rewrites the cell palette for a rectangle through `SetMaterials` (same clone-and-swap); more than 256 total materials is refused (Jolt returns false).
- `info`: floats `AVER_PHYS_HFI_*`: `0 SPACING_CM`, `1 MIN_HEIGHT_CM`, `2 MAX_HEIGHT_CM` (the encodable range), `3 CLAMPED_TOTAL`, `COUNT 4`; ints `AVER_PHYS_HII_*`: `0 COLS`, `1 ROWS`, `2 PADDED`, `3 BLOCK_SIZE`, `4 BITS`, `5 MATERIALS`, `COUNT 6`.

The landscape `rebuildCollision` is unchanged in structure (it still rebuilds on a stale flag at Play start); the gain is that gameplay can now carve a crater at run time without a rebuild, and that cell identity exists.

### 2.8 Surfaces in the engine: `.ocmat`, `.ocsurf`, collision meshes, levels, landscape

**`.ocmat` `PHYSSURFACE <name>`** (extras, `OcMatExtras::physSurface`, parsed beside `GRAPHREF` at `OcMat.cpp:258`, written beside it at `:641-644` only when non-empty, so every existing file round-trips byte for byte). The name is `[A-Za-z0-9_ ]{1,63}`; the material inspector gets a "Physical Surface" combo (names from the project table plus free text) in a "Physics" group next to the slot controls (`SandboxPanels.cpp:280` neighbourhood). FORMAT_SPECS gains a row.

**`Content/Physics/Surfaces.ocsurf`** (OC text, one project table, `Aver.Formats` `OcSurf.hpp`; hosts load it when a project opens, before any level, and re-load it when the editor saves it):

```
OCSURF 1
SURFACE Grass  friction 0.6  restitution 0.05  tag 1
SURFACE Ice    friction 0.03
SURFACE Metal  friction 0.35 restitution 0.2   tag 4
```

Records are one per line; unknown keys are ignored with one warning, a newer version is refused, `friction`/`restitution` default to unset (defer to the body), `tag` to 0, names unique, at most 4096. Loading calls `register` (authored numbers win over anything a layer registered earlier: loaded first, and layers use find-or-add). There is no sound or FX field: audio and particles are chosen by the game from `surface` or `tag` through the existing data-table feature.

**Collision meshes.** `GameContent::CollisionMesh` gains `std::vector<u8> triPalette` and `std::vector<i32> palette`. `collisionSlotSurface(slotName)` reads the slot's `.ocmat` `physSurface` exactly where `collisionSlotCollides` already does (loaded desc or a direct file parse, `GameContent.cpp:1145-1170,1499-1508`), caching per slot. `buildCollisionMesh` (a) keeps today's single-pass path **byte-identical** when no kept slot names a surface, and (b) otherwise groups kept triangles by surface id and runs weld + `simplifyCollisionMesh` once per group with the existing `LockBorder`, so group borders are preserved and no crack opens between two surfaces; the groups' outputs are concatenated, each triangle's palette byte set from its group (palette capped at 32; a slot beyond that falls into surface 0 with one warning). Trifactor's LOD pick is skipped when surfaces are present (it has no slot identity). The per-group simplification is **UNVERIFIED** for visual quality at borders; fallback if it cracks: simplify whole, then classify each output triangle by the nearest source triangle's slot (a cost of a spatial lookup, stated and not chosen). `kCollisionCacheVersion` goes to 2 and the cache file gains the two arrays (a version-1 file is a miss and rebuilds).

**Levels.** `InstantiateOptions` gains an optional `localTriangleSurfacesFor(meshId, const u8*& palette, u32& triCount, const i32*& surfaces, u32& surfaceCount)`; `addStaticPlacementBody` calls `aver_phys_shape_mesh` instead of `aver_phys_create_mesh_shape` when it answers true (the shared-shape cache by mesh id is unchanged), and the three hosts that set `localTrianglesFor` (`GameLevel.cpp:193`, `SandboxMcp.cpp:750`, `SandboxSelection.cpp:1057`) set the new one from the same `CollisionMesh`. A host that sets nothing behaves as today (`LevelInstanceTest`'s existing case).

**Landscape.** `toPhysicsHeightfield` gains `cellPalette` and `palette`: each cell's dominant paint layer (the layer with the largest weight averaged over its four corners, the same weights `sampleGround` reads) becomes the palette index, the palette being the layers' `surfaceName()` hashed with `Surface.IdOf` (at most 8 layers, so at most 8 entries). `GameLandscape::rebuildCollision` calls `aver_phys_heightfield_shape` + `aver_phys_body_from_shape` when the section has layers and `aver_phys_add_heightfield` otherwise, registers each layer's surface (find-or-add), and fixes the failed-build log (`landscapeBody_ >= 0` becomes `> 0`, `GameLandscape.cpp:87`). The sampler path stays. Runtime painting does not update the cells (open question 15).

**Hosts.** `game::loadPhysicalSurfaces(contentDir)` (Runtime) is called from both hosts beside the landscape setup; `game::registerGameSystems` registers the `CollisionShape` component (2.9) so levels that name it load everywhere.

### 2.9 `.ocshape` and `CCollisionShape`

**`.ocshape`** (OC text, header `OCSHAPE 1`, `Aver.Formats` `OcShape.hpp`; records one per line, `#` comments, unknown keys warn once, newer version refused, written canonically so write-then-parse is byte-identical):

```
OCSHAPE 1
NAME Chair
MASS 12                          # kg; 0 or absent = density times volume
SURFACE Wood                     # default surface for leaves that name none
NODE seat  box 40 40 4
NODE back  box 40 3 25
NODE leg   cylinder 3 45 radius 0.5
NODE top   compound static
CHILD top seat at 0 0 45
CHILD top back at -37 0 75 rot 0 0 0 1
CHILD top leg  at 32 32 22
CHILD top leg  at 32 -32 22
CHILD top leg  at -32 32 22
CHILD top leg  at -32 -32 22
ROOT top
```

Leaf `NODE <id> <kind> <numbers>` with `kind`: `box hx hy hz`, `sphere r`, `capsule r h`, `cylinder r h`, `tcapsule top bottom h`, `tcylinder top bottom h`, `triangle ax ay az bx by bz cx cy cz`, `plane nx ny nz offset half`, `empty x y z`, `hullpoints x y z ...`, `hull mesh <content path>` (the mesh's vertices, welded, as the hull points; built from `GameContent` at load, not cooked: open question 16), `mesh <content path>` (the collision mesh of `GameContent`, with slot surfaces). Optional trailing pairs: `radius <cm>` (convex radius), `density <kg/m3>`, `tolerance <cm>`, `edge <deg>`, `quality runtime|build`, `surface <name>`. Decorators: `NODE <id> rt <child> at x y z rot qx qy qz qw`, `NODE <id> com <child> at x y z`, `NODE <id> scale <child> sx sy sz`; compounds `NODE <id> compound static|mutable`, children by `CHILD <compound> <node> [at x y z] [rot qx qy qz qw] [user <n>]` in file order. `ROOT <id>`; the graph must be a tree (a node used twice is allowed for leaves and decorators, built once and shared; a cycle is an error). `validate` runs on parse and in the editor: numbers finite and in the ABI's ranges, every reference defined, a mutable compound not under another compound twice, plane and mesh only under a static or kinematic body (checked at body creation).

**Builder.** `game::buildColliderShape(const fmt::ShapeSpec&, MeshSource&, i32& outShape, std::string& why)` (Runtime, `ColliderShapes.hpp`) walks the tree and issues the builder calls of 2.3; `MeshSource` is the seam to `GameContent` for `hull mesh`/`mesh` leaves, injected in tests.

**`CCollisionShape`** (runtime-registered, the `CMotionMatcher` / `CCloth` pattern: no `Components.hpp` growth, no scene-ABI change; 32 bytes, zero-filled storage is valid):

```cpp
inline constexpr u32 kCollisionShapeIgnoreScale = 0x1;   // negative sense: zero-filled applies the entity's world scale
struct CCollisionShape {                // "CollisionShape"
  u64 asset = 0;                        //  0 .ocshape ObjectId; 0 = none (CRigidBody's own shape stands)
  i32 surface = 0;                      //  8 body label: Surface.IdOf of the name; 0 = none
  u32 flags = 0;                        // 12
  i32 outShape = 0;                     // 16 OUT: the shape handle the body was built from
  f32 outMassKg = 0;                    // 20 OUT
  f32 outVolumeCm3 = 0;                 // 24 OUT
  u32 reserved = 0;                     // 28
};
static_assert(sizeof(CCollisionShape) == 32, "CCollisionShape must be padding-free");
```

It requires a `CRigidBody` on the same entity (motion type, mass, friction, restitution, gravity, damping and layer still come from it; `canAttach` refuses without one). In `PhysicsSceneSync` pass 1, `createShapeBody`/`createSensorBody` consult it first: build the shape, `ScaleShape` it by the entity's world scale unless `kCollisionShapeIgnoreScale`, `aver_phys_body_from_shape` with `rb.motionType`, `rb.massKg` (the `.ocshape` `MASS` when `rb.massKg` is 0), `rb.collisionLayer` and `BP_SENSOR` for `rb.isSensor`, then the same material setters as today, `aver_phys_body_set_surface(body, surface, 0)`, and fill the OUT fields. The CRigidBody pose write-back gap (1.6) is not fixed here; any fix must read `aver_phys_body_origin`. Level record: `COMPONENT CollisionShape asset=Physics/Chair.ocshape surface=Wood noscale=0`, canonical form omits defaults; a surface the table does not know is written `surface=#<hex id>` and read back the same; `kPlacedComponentTypes` and `OcWorld.cpp`'s list gain the name and `LevelComponents` an `applyCollisionShape`/capture case beside `applyCloth`. Runtime state is never saved.

### 2.10 C# API

`Aver.Physics` (leaf; new files `Native.ShapeBuilder.cs`, `Native.Surface.cs`, `Native.Terrain.cs`, `CollisionShape.cs`, `ShapeEnums.cs`, `Surfaces.cs`, `SurfaceHits.cs`, `Terrain.cs`, `BodyShape.cs`). Native files copy header names and parameter names character for character (the parity test compares both).

```csharp
namespace Aver.Physics;

public readonly record struct ShapeParams(float ConvexRadiusCm = -1, float DensityKgM3 = 1000, float HullToleranceCm = 0.1f,
                                          float EdgeAngleDeg = 5, bool FavorBuildSpeed = false, int MaxTrianglesPerLeaf = 8);
public readonly record struct ShapeChild(CollisionShape Shape, Float3 Position, Quaternion Rotation, int UserData = 0);
public readonly record struct ShapeInfo(ShapeType Type, float VolumeCm3, float MassKg, Float3 CenterOfMass, Float3 BoundsMin, Float3 BoundsMax,
                                        float InnerRadius, Float3 InertiaPrincipal, Quaternion InertiaFrame,
                                        int Children, int Leaves, int Triangles, long Bytes, bool MustBeStatic, bool HasMass);

public readonly struct CollisionShape : IEquatable<CollisionShape>          // a handle; 0 is invalid; Release() when done
{
    public int Handle { get; }  public bool IsValid { get; }  public static CollisionShape None { get; }
    public static CollisionShape Box(Float3 half, int surface = 0, ShapeParams? p = null);
    public static CollisionShape Sphere(float radius, int surface = 0);
    public static CollisionShape Capsule(float radius, float height, int surface = 0);          // height TOTAL
    public static CollisionShape Cylinder(float radius, float height, int surface = 0, ShapeParams? p = null);
    public static CollisionShape TaperedCapsule(float top, float bottom, float height, int surface = 0);
    public static CollisionShape TaperedCylinder(float top, float bottom, float height, int surface = 0, ShapeParams? p = null);
    public static CollisionShape Triangle(Float3 a, Float3 b, Float3 c, int surface = 0, ShapeParams? p = null);
    public static CollisionShape Plane(Float3 normal, float offsetCm, float halfExtentCm, int surface = 0);
    public static CollisionShape Empty(Float3 centerOfMass);
    public static CollisionShape ConvexHull(Float3[] points, int surface = 0, ShapeParams? p = null);
    public static CollisionShape Mesh(Float3[] vertices, int[] indices, byte[]? triPalette = null, int[]? palette = null,
                                      int[]? triUserData = null, ShapeParams? p = null);
    public CollisionShape RotatedTranslated(Float3 position, Quaternion rotation);
    public CollisionShape OffsetCenterOfMass(Float3 offset);
    public CollisionShape Scaled(Float3 scale);
    public static CollisionShape Compound(ShapeChild[] children, bool mutable = false);
    public ShapeInfo Info { get; }  public bool Release();
    public Body CreateBody(Float3 origin, Quaternion rotation, MotionType motion, float massKg = 0, int layer = 0,
                           bool sensor = false, bool enhancedEdges = false);
}
```

`Body` (partial, in `BodyShape.cs`): `CollisionShape Shape()` (new handle), `bool SetShape(CollisionShape, bool keepMass = true)`, `bool SetScale(Float3, bool keepMass = false)`, `Float3 Scale`, `Float3 Origin`, `bool SetEnhancedEdges(bool)`, `int AddChild(CollisionShape, Float3, Quaternion, int userData = 0)` (-1 on failure), `bool RemoveChild(int)`, `bool SetChildTransform(int, Float3, Quaternion)`, `int ChildCount`, `int Surface`, `bool SetSurface(int surface, bool copyPhysics = false)`, `int Triangles(Float3 min, Float3 max, Span<float> xyz, Span<int> surfaces)`. `Vehicle.SetChassisShape(CollisionShape)` in `VehicleExt.cs` (one method, the only edit to that file).

```csharp
public static class Surfaces       // process-wide registry
{
    public static int IdOf(string? name);                                   // = Aver.Framework.Surface.IdOf; golden values in tests
    public static int Register(string name, float friction = -1, float restitution = -1, int tag = 0);
    public static bool Tune(int surface, float friction, float restitution, int tag);
    public static bool TryGet(int surface, out SurfaceInfo info);           // Name, Friction, Restitution, Tag
    public static string NameOf(int surface);  public static int Count { get; }  public static int At(int index);  public static void Reset();
}
public readonly record struct SurfaceHit(bool Hit, Body Body, int Entity, Float3 Point, Float3 Normal, float DistanceCm, int SurfaceId,
                                         float Friction, float Restitution, int Tag, int SubShape, int Child, int ChildUserData,
                                         int Triangle, int CellX, int CellY);
// Physics (partial): RaycastDetail(origin, direction, maxDistanceCm, layerMask, ignoreBody, QueryFlags), SubShapeInfo(Body, int subShape),
//   ContactDetails (IReadOnlyList<ContactDetail(SubShapeA, SubShapeB, SurfaceA, SurfaceB, ClosingSpeedCmS, Friction, Restitution)>, index-aligned with Contacts)
// CharacterBody.GroundDetail, Vehicle.WheelSurface(int wheel)
public static class Terrain
{
    public static CollisionShape Heightfield(float[] samples, int cols, int rows, float spacingCm, byte[]? cellPalette = null, int[]? palette = null,
                                             int blockSize = 2, int bitsPerSample = 8, float edgeAngleDeg = 5, float editHeadroomCm = 0);
    public static bool SetHeights(Body body, int x0, int y0, int w, int h, float[] heights, bool wakeBodies = true);   // NaN = hole
    public static bool GetHeights(Body body, int x0, int y0, int w, int h, float[] output);
    public static bool SetCellSurfaces(Body body, int x0, int y0, int w, int h, byte[] cellPalette, int[] palette);
    public static TerrainInfo Info(Body body);
}
```

`Aver.Framework` (scripts; engine Vec3/Entity types): `PhysicalSurfaces.Raycast(Vec3 origin, Vec3 direction, float maxDistanceCm, uint layerMask = All)` returns `SurfaceHit` (Framework flavour: `Entity`, `Vec3`, `string SurfaceName`), `PhysicalSurfaces.Beneath(Entity e, float probeCm = 150)` (the entity's character ground if it has one, else a ray down from its position; when the hit body has no entity and a landscape is loaded the answer comes from `Landscape.Sample` so terrain reports its dominant layer name even if the heightfield was built without cells), `PhysicalSurfaces.Find(name)`, `Register`, `Tune`, `Info`; `Entity.PhysicalSurface` / `Entity.SetPhysicalSurface(id, copyPhysics)` / `Entity.SetBodyScale(Vec3, keepMass)` over the entity's `CRigidBody.body`. `Surface.IdOf` keeps its signature and now delegates; `Surface.NameOf(int)` is added (`Landscape.cs` shared edit, `Surface` made `partial`).

### 2.11 Aver Node: family `PhysicalSurface` v1

Wave 9 C contract exactly: `GraphPhysicalSurfaceNodeTable.cs` (a `PhysicalSurfaceTable()` method), `sandbox/src/GraphNodeDefsPhysicalSurface.hpp` (`appendPhysicalSurfaceNodeDefs`, auto-discovered by `cmake/GraphNodeFamilies.cmake`), owner methods `internal static` in `SurfaceGraph.cs` over `PhysicalSurfaces`, `PhysicalSurfaceNodeTests` `[AutoRun]` built from `NodeRowKit`, category `Physics`. Failures return false or 0 and log once per node id; nothing throws. Nothing is edited in `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs` or `Program.cs`. Entities are ints, names are attributes, the surface is the int id `an_surfaceid` already produces. `docs/nodes/PhysicalSurface.md` is generated by `scripts/node-catalog.py --write`.

| Key | Kind | Pins (besides `exec` / `then`) | Attr | Wraps |
|---|---|---|---|---|
| `an_raycastsurface` | Exec | in `originX originY originZ dirX dirY dirZ`, `maxDist=10000`, `layerMask:i=-1`; out `hit:b`, `entity:i`, `pointX pointY pointZ`, `normalX normalY normalZ`, `distance`, `surfaceId:i`, `friction`, `restitution`, `tag:i` | | `PhysicalSurfaces.Raycast` |
| `an_surfacebeneath` | Exec | in `entity:i`, `probeCm=150`; out `found:b`, `surfaceId:i`, `friction`, `restitution`, `tag:i` | | `PhysicalSurfaces.Beneath` |
| `an_surfaceinfo` | Pure | in `surfaceId:i`; out `found:b`, `friction`, `restitution`, `tag:i` | | `Surfaces.TryGet` |
| `an_setbodysurface` | Exec | in `entity:i`, `surfaceId:i`, `copyPhysics:b=false`; out `success:b` | | `Entity.SetPhysicalSurface` |
| `an_registersurface` | Exec | in `friction=-1`, `restitution=-1`, `tag:i=0`; out `surfaceId:i`, `success:b` | `name` | `PhysicalSurfaces.Register` |
| `an_tunesurface` | Exec | in `surfaceId:i`, `friction`, `restitution`, `tag:i`; out `success:b` | | `PhysicalSurfaces.Tune` |
| `an_setbodyscale` | Exec | in `entity:i`, `scaleX scaleY scaleZ`, `keepMass:b=false`; out `success:b` | | `Entity.SetBodyScale` (physics only: pair with the existing transform-scale node) |

The raycast row is Exec for the reason the existing `Raycast` is (`docs/AVER_NODE_NODES.md:215-260`: a cast per pull is too expensive to leave to pure evaluation). Impact and footstep events are not added (open question 17).

### 2.12 Editor

- **Add Component "Collision Shape"** (a `PlacedKind` row, `PlacedComponentModel.hpp`): requires a mesh or any entity with `CRigidBody`; `makeNewRecord` seeds nothing; `AttachContext` gains `hasRigidBody` use (it exists) and `canAttach` refuses without a Rigid Body with one line of reason.
- **Details section** (`CollisionShapeDetailsModel.hpp` ImGui-free and tested; `CollisionShapeDetailsSection.cpp` the widgets, one dispatch block in `SandboxComponentDetails.cpp`): asset picker, surface combo (table names), "No scale" flag, **Create .ocshape from the Rigid Body** (writes the current box/sphere/capsule/cylinder as a one-node tree), read-outs in Play (shape handle, volume, mass, centre of mass, bounds, from `aver_phys_shape_info`), a "Surface under cursor" probe button that fires `aver_phys_raycast_detail` from the viewport pick ray.
- **`.ocshape` tab** (`OcShapeEditorModel.hpp` pure, `OcShapeEditor.{hpp,cpp}`): a node tree (add leaf, wrap in decorator, add to compound, reorder, delete), numeric fields, a validation list that is `fmt::validate`'s output, undo through the snapshot undo the other asset editors use, and a preview that builds the shape into a temporary static body on a reserved layer and draws it with the wire overlay.
- **Project Settings > Physics > Surfaces** (`PhysicalSurfaceTableModel.hpp`, `PhysicalSurfacePanel.cpp`): the `.ocsurf` as an editable table (name, friction, restitution, tag), duplicate-name and hash-collision checks, save writes the file and calls `register`/`tune`, so friction edits are live in Play. The material inspector gets the Physical Surface combo (2.8).
- **Show > Collision Shapes** (`CollisionShapeOverlay.{hpp,cpp}`, the `SoftBodyDebugOverlay` pattern: `collect` into `rhi::LineVertex`, drawn under `!noEditorChrome_`): for the **selected entity's body and every moving body**, the edges of `aver_phys_body_triangles` in the body's region, coloured by surface hash (a stable hue), compound child bounds as boxes, centre of mass and origin crosses. Static bodies keep the existing AABB overlay (`SandboxViewport.cpp:39-125`), whose 20,000-body optimisations are not disturbed; both backends draw the same lines.
- **COFF discipline:** every piece lives in its own translation unit; `SandboxComponentDetails.cpp`, `SandboxApp.cpp`, `SandboxRender.cpp` and `SandboxShell.cpp` get a few lines each. `GraphNodeDefsPhysicalSurface.hpp` adds 7 `push_back`s to the translation unit holding the node catalog; the implementing agent records the Debug object's section count before and after (the cap note in the memory index) and moves family headers to a second catalog unit if within 5% of the cap.

### 2.13 Content

`test-content/PhysicsShapesLab/` (engine test content, not a demo; `make_shapes_lab.py`, stdlib Python writing OBJ sources and text assets, committed output imported with `AverAssetC`, `SOURCES.md` says no third-party assets): `Physics/Surfaces.ocsurf` (Grass, Ice, Rubber, Metal, Gravel), `Materials/*.ocmat` with `PHYSSURFACE`, a two-slot floor mesh (ice left, rubber right), a ramp mesh for per-triangle surfaces, `Physics/Chair.ocshape` (static compound), `Physics/Barrel.ocshape` (tapered cylinder), `Physics/GrowCrate.ocshape` (mutable compound), a hull-from-mesh prop, a dynamic mesh prop (a closed teapot-like OBJ), a plane ramp, a heightfield level (`.ocland`) with a hole and two paint layers, and `Maps/Lab.ocworld` placing them with `COMPONENT CollisionShape` records, plus `README.md` with the owner checks of section 6.

### 2.14 Determinism, threading, and what is claimed

Shape and registry construction is deterministic (no clock, no randomness); the combine functions read immutable-between-steps data; `tune` and `set_surface` are between-step calls (a `tune` during `aver_phys_step` from another thread is a data race the header forbids, as for the layer matrix). Heightfield edits are between-step calls on the thread that steps. The same binary and the same call order reproduce the same bodies; cross-platform lockstep is not claimed (Jolt here is not `CROSS_PLATFORM_DETERMINISTIC`). A project that creates no surface and calls no new function is bit-identical to today, and a test pins that (section 5). The shape table is not cleared at shutdown (as the mesh table is not): a caller that wants a clean slate releases its handles; leaked handles outlive a world exactly like leaked mesh shapes do.

---

## 3. FILE OWNERSHIP

### 3.1 Created (only this area touches them)

Physics
- modules/physics/include/aver/physics/physics_shape_builder_abi.h
- modules/physics/include/aver/physics/physics_surface_abi.h
- modules/physics/include/aver/physics/physics_terrain_abi.h
- modules/physics/src/PhysicsShapeTable.hpp
- modules/physics/src/PhysicsShapeTable.cpp
- modules/physics/src/PhysicsShapeBuilder.cpp
- modules/physics/src/PhysicsShapeBody.cpp
- modules/physics/src/PhysicsMeshBuild.hpp
- modules/physics/src/PhysicsMeshBuild.cpp
- modules/physics/src/PhysicsSurface.hpp
- modules/physics/src/PhysicsSurface.cpp
- modules/physics/src/PhysicsTerrain.cpp

Core and Formats
- modules/core/include/aver/core/SurfaceId.hpp
- modules/formats/include/aver/formats/OcShape.hpp
- modules/formats/src/OcShape.cpp
- modules/formats/include/aver/formats/OcSurf.hpp
- modules/formats/src/OcSurf.cpp

Runtime
- Runtime/include/aver/game/ColliderShapes.hpp
- Runtime/src/ColliderShapes.cpp
- Runtime/include/aver/game/PhysicalSurfaces.hpp
- Runtime/src/PhysicalSurfaces.cpp

C# Aver.Physics
- scripting/csharp/Aver.Physics/Native.ShapeBuilder.cs
- scripting/csharp/Aver.Physics/Native.Surface.cs
- scripting/csharp/Aver.Physics/Native.Terrain.cs
- scripting/csharp/Aver.Physics/CollisionShape.cs
- scripting/csharp/Aver.Physics/ShapeEnums.cs
- scripting/csharp/Aver.Physics/Surfaces.cs
- scripting/csharp/Aver.Physics/SurfaceHits.cs
- scripting/csharp/Aver.Physics/Terrain.cs
- scripting/csharp/Aver.Physics/BodyShape.cs

C# Framework, Graph, tests
- scripting/csharp/Aver.Framework/PhysicalSurfaces.cs
- scripting/csharp/Aver.Framework/SurfaceGraph.cs
- scripting/csharp/Aver.Framework/EntitySurface.cs
- scripting/csharp/Aver.Graph/GraphPhysicalSurfaceNodeTable.cs
- sandbox/src/GraphNodeDefsPhysicalSurface.hpp
- scripting/csharp/Aver.Graph.Tests/PhysicalSurfaceNodeTests.cs
- scripting/csharp/Aver.Graph.Tests/PhysicsShapeApiTests.cs
- docs/nodes/PhysicalSurface.md (generated)

Editor
- sandbox/src/CollisionShapeDetailsModel.hpp
- sandbox/src/CollisionShapeDetailsSection.cpp
- sandbox/src/OcShapeEditorModel.hpp
- sandbox/src/OcShapeEditor.hpp
- sandbox/src/OcShapeEditor.cpp
- sandbox/src/PhysicalSurfaceTableModel.hpp
- sandbox/src/PhysicalSurfacePanel.cpp
- sandbox/src/CollisionShapeOverlay.hpp
- sandbox/src/CollisionShapeOverlay.cpp

Tests
- tests/physics/src/ShapeBuilderTest.cpp
- tests/physics/src/SurfaceTest.cpp
- tests/physics/src/TerrainTest.cpp
- tests/formats/src/OcShapeTest.cpp
- tests/formats/src/OcSurfTest.cpp
- tests/game/src/PhysicalSurfacesTest.cpp
- tests/game/src/CollisionMeshSurfaceTest.cpp
- tests/game/src/ColliderShapesTest.cpp
- tests/editor/src/CollisionShapeDetailsModelTest.cpp
- tests/editor/src/OcShapeEditorModelTest.cpp
- tests/editor/src/PhysicalSurfaceTableModelTest.cpp

Content and docs
- test-content/PhysicsShapesLab/ (generator, README, SOURCES.md, Content/ as in 2.13)
- docs/PHYSICS_SHAPES.md

### 3.2 Modified, owned by this area

- modules/physics/src/PhysicsShapes.cpp (re-routed through the shared builders; entry points and behaviour unchanged)
- modules/landscape/include/aver/landscape/PhysicsBridge.hpp
- modules/landscape/src/PhysicsBridge.cpp
- Runtime/src/GameLandscape.cpp
- Runtime/include/aver/game/GameLandscape.hpp
- Runtime/src/GameContent.cpp
- Runtime/include/aver/game/GameContent.hpp
- modules/world/include/aver/world/LevelInstance.hpp
- modules/world/src/LevelInstance.cpp
- Runtime/src/GameLevel.cpp
- sandbox/src/PhysicsSceneSync.cpp
- scripting/csharp/Aver.Physics/VehicleExt.cs (one method)
- scripting/csharp/Aver.Framework/Landscape.cs (`Surface` partial, delegation, `NameOf`)
- tests/world/src/LevelInstanceTest.cpp (two cases)

### 3.3 SHARED (another wave 11 area, the integrator or an earlier wave also touches these; every edit is small and additive)

- modules/physics/src/PhysicsInternal.hpp
- modules/physics/src/PhysicsWorld.cpp
- modules/physics/src/PhysicsVehicle.cpp
- modules/physics/src/PhysicsVehicleExt.cpp
- modules/physics/CMakeLists.txt
- modules/physics/include/aver/physics/physics_abi.h (comment cross-reference only, no signature change)
- scripting/csharp/Aver.Physics/Native.cs (not edited; listed so nobody adds the new imports there)
- tests/abi/src/AbiEnumTest.cpp
- tests/physics/CMakeLists.txt
- tests/formats/CMakeLists.txt, tests/game/CMakeLists.txt, tests/editor/CMakeLists.txt
- modules/formats/CMakeLists.txt, Runtime/CMakeLists.txt, sandbox/CMakeLists.txt
- modules/formats/include/aver/formats/OcMat.hpp, modules/formats/src/OcMat.cpp
- modules/formats/src/OcWorld.cpp
- Runtime/include/aver/game/GameSystemsWiring.hpp
- Runtime/include/aver/game/LevelComponents.hpp, Runtime/src/LevelComponents.cpp
- Runtime/src/GameApp.cpp (project-open call)
- sandbox/src/SandboxInit.cpp (project-open call), sandbox/src/SandboxMcp.cpp, sandbox/src/SandboxSelection.cpp (the new level callback)
- sandbox/src/SandboxApp.hpp, SandboxApp.cpp, SandboxRender.cpp, SandboxShell.cpp
- sandbox/src/SandboxComponentDetails.cpp, SandboxContentBrowser.cpp, PlacedComponentModel.hpp
- sandbox/src/SandboxPanels.cpp (material inspector combo)
- docs/ABI.md, docs/MODULES.md, docs/SCRIPTING_API.md, docs/formats/FORMAT_SPECS.md, docs/physics/JOINTS_AND_BODIES.md, CHANGELOG.md

| Shared file | Edit made by this area | Who else |
|---|---|---|
| `PhysicsInternal.hpp` | `ContactEvent` gains seven fields; declaration of `warnBodyLimit()` (defined in `PhysicsWorld.cpp` today, not declared); nothing else (the new internals have their own `PhysicsShapeTable.hpp` / `PhysicsSurface.hpp`) | any area adding world state |
| `PhysicsWorld.cpp` | (a) `g_meshShapes`, `meshShapeFor`, `aver_phys_create_mesh_shape`, `_release_mesh_shape` delegate to `PhysicsShapeTable`; (b) `aver_phys_add_mesh` / `create_mesh_shape` call `buildMeshShape` (net deletion of the duplicated loop); (c) `aver_phys_add_heightfield` calls the terrain builder; (d) `aver_phys_set_entity` preserves the label half; (e) `OnContactAdded` fills the contact detail; (f) two `SetCombine*` calls after `system.Init`; (g) `aver_phys_add_mesh_shape_body` accepts any handle | large-world, queries, soft-body areas edit the same neighbourhood |
| `PhysicsVehicle.cpp:512`, `PhysicsVehicleExt.cpp:347` | the wheel grip and the telemetry read the leaf material's friction through `surfaceFrictionOf(body, subShapeId, fallback)` (declared in `PhysicsSurface.hpp`) instead of `body.GetFriction()` | VEHICLES (wave 10, shipped) |
| `modules/physics/CMakeLists.txt` | the nine new `.cpp` files | every physics area |
| `AbiEnumTest.cpp` | three `kAbis` rows (each new header with its own `Native.*.cs` and prefix `aver_phys_`), and `kGroups` rows for the index blocks and enums: `AVER_PHYS_SHAPE_TYPE_` (`ShapeType`), `AVER_PHYS_SP_` (`SpIndex`), `AVER_PHYS_SI_` (`SiIndex`), `AVER_PHYS_SII_` (`SiiIndex`), `AVER_PHYS_BP_` (`BpIndex`), `AVER_PHYS_HITF_` (`HitFloat`), `AVER_PHYS_HITI_` (`HitInt`), `AVER_PHYS_CONF_` (`ContactFloat`), `AVER_PHYS_CONI_` (`ContactInt`), `AVER_PHYS_HP_` (`HpIndex`), `AVER_PHYS_HFI_` (`TerrainFloat`), `AVER_PHYS_HII_` (`TerrainInt`), all against `ShapeEnums.cs` | every ABI area |
| `OcMat.hpp/.cpp` | `OcMatExtras::physSurface`, one parse branch, one write branch | materials |
| `OcWorld.cpp`, `LevelComponents.*`, `GameSystemsWiring.hpp` | the `CollisionShape` name in the COMPONENT list, apply/capture, registration | every area adding a component |
| `GameApp.cpp`, `SandboxInit.cpp` | one `game::loadPhysicalSurfaces` call each at project open | all |
| `SandboxMcp.cpp`, `SandboxSelection.cpp` | set `localTriangleSurfacesFor` beside `localTrianglesFor` | none expected |
| the Sandbox UI files | overlay members, Show menu item, Add Component row, one details dispatch block, one content-browser entry, the material combo | every editor feature |

Not touched: `scene_abi.h`, `Components.hpp`, `framework_abi.h`, `modules/rhi*`, every shader, `modules/render*`, `GraphNodeDefs.hpp`, `GraphGameSystemNodeTable.cs`, `Program.cs`, `physics_shapes_abi.h`, `Native.cs`.

---

## 4. Phased implementation

Each phase ends with its tests green and the tree buildable. Sizes: A L, B L, C M, D L, E M, F M, G S. Overall **XL**. Never rebuild over the owner's running editor; headed runs are the owner's. The implementing agents use Sonnet workflows with the fewest stages.

**Phase A: shape handles, builders, bodies (L).** `PhysicsShapeTable` (generic table, mesh functions delegating), `PhysicsShapeBuilder` (all leaves, decorators, compounds, `shape_info`), `PhysicsMeshBuild` (the single mesh builder; palette, options), `PhysicsShapeBody` (`from_shape`, `set_shape`, `set_scale`, `origin`, compound edit, `body_triangles`, `vehicle_set_chassis_shape`), `PhysicsShapes.cpp` re-routing, C# `CollisionShape` / `BodyShape` / enums, `AbiEnumTest` rows. Exit: `ShapeBuilderTest` green; `ShapeTest`, `PhysicsTest`, `QueryTest`, `LayerTest`, `RagdollTest`, `VehicleTest`, `VehicleExtTest` unchanged and green; the old-vs-new equivalence test green.

**Phase B: surfaces (L).** Registry, `AverSurface`, ids in `UserData`, the combine functions, contact detail, `raycast_detail`, `subshape_info`, character ground and wheel surface, the two vehicle one-liners, `Core/SurfaceId.hpp`, C# `Surfaces` / `SurfaceHits`, `Surface.IdOf` delegation. Exit: `SurfaceTest` green; `SurfaceFrictionTest` and `WaterSamplerTest` unchanged and green; `VehicleExtTest` green; the "no surface means bit-identical" test green.

**Phase C: terrain and dynamic meshes (M).** `PhysicsTerrain.cpp`, `aver_phys_add_heightfield` re-routed, dynamic mesh bodies, C# `Terrain`. Exit: `TerrainTest` green; the heightfield equivalence test green.

**Phase D: engine data (L).** `OcSurf`, `OcMat` `PHYSSURFACE`, `PhysicalSurfaces` loader, collision-mesh groups and cache v2, `LevelInstance` callback and the three hosts, landscape cells, the `landscapeBody_` fix, `Framework.PhysicalSurfaces`, `SurfaceGraph`, the 7 rows, palette, tests, `--write` for `docs/nodes/PhysicalSurface.md`. Exit: `OcSurfTest`, `CollisionMeshSurfaceTest`, `PhysicalSurfacesTest`, the `LevelInstance` case, `GameLandscapeLayersTest` (extended), `PhysicalSurfaceNodeTests`, `NodeCatalogCheck`, `CsharpApiIndexCheck` green.

**Phase E: `.ocshape` and `CCollisionShape` (M).** `OcShape`, `ColliderShapes`, the component, record apply/capture, `PhysicsSceneSync`. Exit: `OcShapeTest`, `ColliderShapesTest`, `LevelComponentsTest` (extended), `OcWorldComponentsTest` (extended) green.

**Phase F: editor (M).** Details model and section, `.ocshape` tab, surfaces panel, material combo, overlay. Exit: the three editor model tests green; the Debug sandbox object section count recorded.

**Phase G: lab and docs (S).** Generator, committed content, `docs/PHYSICS_SHAPES.md`, the doc edits of 3.3, `module-guard-audit.py`, `docs-check.py`. Exit: the lab level loads headless; full `ctest` green.

---

## 5. Tests

Everything below runs without a GPU or a window. `*Test` executables are picked up by the root walk; each phase adds only an `add_executable`.

**`tests/physics` (link `Aver.Physics Aver.Core`)**

- `ShapeBuilderTest`: (1) *leaves rest where they should*: each leaf dropped onto a static floor settles with its centre at the expected half-height within 1 cm (box, sphere, capsule, cylinder, tapered capsule, **tapered cylinder with the large radius on the floor and the small one up**, which also pins `topRadius` at +Z). (2) *plane*: a static plane at `z = 40` holds a sphere at 40 + r; a dynamic body from a plane is refused (`Unsupported`); a normal tilted 45 degrees slides a box. (3) *triangle*: a thin convex triangle holds a sphere. (4) *empty*: a dynamic body with an empty shape falls through a floor; a joint-less kinematic empty stays. (5) *decorators*: a capsule rotated 90 degrees about Y rests at height r; `offset_com` moves `aver_phys_body_position` by the offset while `aver_phys_body_origin` does not; `scaled` (2,1,1) doubles the AABB along X, a mirror (1,1,-1) of a mesh still collides, zero scale and a non-representable scale are refused or baked as 2.2 says. (6) *static compound with rotation*: three boxes, one rotated 90 degrees, AABB equal to the hand-computed one; the 94 cm style centre-of-mass number for an asymmetric compound matches the rotated and the unrotated spelling; the identity-rotation compound equals `aver_phys_add_static_compound_boxes` poses after 300 steps (1e-5). (7) *child user data*: `raycast_detail` through each child reports `CHILD` and `CHILD_USER_DATA`. (8) *mutable compound*: add a box and the AABB grows and a ray through the new child hits; remove it and it misses; the first child's world position is unchanged by the centre-of-mass adjustment; mass is preserved with `keepMass`; a second body from the same handle is refused. (9) *set_shape*: sphere to box with `keepMass` keeps `aver_phys_body_mass` (0.1%) and the centre-of-mass world position; without it the mass is density times volume; a vehicle chassis, a character and a soft body are refused. (10) *set_scale*: scale 2 then 2 gives an AABB 2x, not 4x; mass scales by 8 unless `keepMass`; a body made by `aver_phys_add_mesh_shape_body` (already scaled) is re-scaled absolutely. (11) *introspection*: box volume and mass at density, bounds, centre of mass, compound child and leaf counts, mesh triangle count, `HAS_MASS` for a mesh, and the inertia frame reconstructs the engine-axis tensor of a (2,3,4) box (the UNVERIFIED conversion). (12) *mesh options*: a palette of 33 is refused; per-triangle user data comes back through a ray; `EDGE_ANGLE_DEG` -1 builds and a ball crosses a coplanar seam without a ghost bump when `BP_ENHANCED_EDGES` is set; `FavorBuildSpeed` builds and ray results equal the default build. (13) *hull*: convex radius 0 versus 5 on a 3 cm box rests at the same height; a 300-point cloud builds; fewer than 4 points is refused. (14) *dynamic mesh*: a closed box mesh with mass 10 falls onto a static box floor and rests within 1 cm of the expected height, its angular response is non-zero for an off-centre impulse, it falls **through** a static mesh floor (the documented limit, pinned so a Jolt change is noticed), and `set_motion_type(dynamic)` on a mass-less mesh is still refused. (15) *vehicle chassis*: `set_chassis_shape` with a two-box compound settles at the same rest height as `set_chassis_boxes` with the same boxes (0.5 cm), and is refused after `_finish`. (16) *lifetime*: release then use is `BadHandle`; a body outlives its released shape; `aver_phys_shape_count` returns to its start after a build-and-release loop; the old `release_mesh_shape` releases a new-style handle. (17) *determinism*: two fresh worlds run the same script and compare bits. (18) *old creators unchanged*: each pre-existing creator built through its old entry and through the new builders gives equal poses after 300 steps.
- `SurfaceTest`: (1) golden ids ("Ice" -714267872, "Grass" 1479220765, "Rubber" 1606840731, "Wood" 81868168) equal the managed hash; empty and NULL give 0. (2) *registry*: `register` is idempotent by name, `info` returns name and numbers, an interned-only id has an empty name and -1/-1/0, `count`/`at` enumerate, `reset` clears numbers but shapes keep working, and the registry survives `shutdown` and `init`. (3) *leaf material*: two boxes with the same body friction, one on "Ice" (0.02) and one on "Rubber" (1.0), given 500 cm/s along a floor, stop at least 5x apart. (4) *per-triangle*: a two-triangle-group floor mesh (ice left, rubber right): boxes sliding on each side stop at the distances of (3); `raycast_detail` at each half reports the right `SURFACE`, `FRICTION`, `TAG` and `TRIANGLE`. (5) *restitution*: a ball on a "Bouncy" (0.9) leaf rebounds higher than on an unset one by the max rule. (6) *defer*: a surface with friction -1 gives positions bit-equal to no surface after 300 steps. (7) *tune is live*: lowering "Rubber" mid-test lengthens the next slide. (8) *body label*: `set_surface` sets `aver_phys_body_surface`; the entity stamp survives it and it survives `set_entity` (the preserve-upper-half rule) and a static-to-dynamic rebuild; a negative entity does not corrupt the label; `copyPhysics` writes the body friction. (9) *contacts*: a box dropped 2 m reports a contact whose `CLOSING_SPEED_CMS` is within 10% of `sqrt(2 g h)`, both surfaces, and applied friction equal to the hook's combine; `contact_detail` index-aligns with `contact_get` and clears with it. (10) *character ground*: a capsule standing on each half of the two-surface floor reports the right surface; a raw heightfield without a cell palette reports 0 and with one reports its cell's surface. (11) *vehicle*: a car with one side on ice and one on tarmac (two boxes with materials) shows lower `SURFACE_FRICTION` on the ice wheels and a lower lateral grip; `aver_phys_vehicle_wheel_surface` names each. (12) *sampler precedence*: with a surface sampler installed, a flagged body's contact friction is the sampler's regardless of leaf materials (`SurfaceFrictionTest`'s setup plus a material). (13) *compound leaf*: a compound of two leaves with different surfaces reports each through `subshape_info`. (14) *bit identity*: a scene with no surfaces run 300 steps with the hook installed equals the recorded poses of the same scene run by the pre-change binary (a golden table written by the first run in the repo and compared thereafter).
- `TerrainTest`: (1) *equivalence*: `aver_phys_add_heightfield` and `heightfield_shape` + `body_from_shape` with empty `p` give identical ray heights at 64 points (bit equal). (2) *non-square*: 40x16 field, a ray inside hits at the right height, a ray in the padded area and outside the rectangle misses, a ball rolls to the edge and falls. (3) *holes*: NaN and `FLT_MAX` samples remove the four touching cells; a ray through a hole misses. (4) *cell surfaces*: a palette of 3 over a 16x16 field; rays report the expected `SURFACE`, `CELL_X`, `CELL_Y`; `set_cell_surfaces` changes them; more than 256 total is refused. (5) *quantisation*: with bits 4 versus 16, the maximum height error against the input stays under the documented bound (range / 2^bits within a block). (6) *edit*: `set_heights` raising a 5x5 region by 100 cm moves a ray hit there by 100 within quantisation; headroom 0 clamps a -500 cm crater to the old minimum and reports it in `HFI_CLAMPED_TOTAL`, headroom 600 allows it; a ball resting on a flattened region falls when `HF_WAKE` is passed and stays asleep when it is not; the old body handle still works after the clone swap. (7) *get_heights* round-trips within quantisation, holes as `FLT_MAX`. (8) *refusals*: 1 column, 16385 samples, spacing <= 0, a palette of 257, an edit on a non-heightfield body. (9) *block size*: 2 versus 8 give the same ray results to quantisation.

**`tests/abi`**: `AbiEnumTest` pairs each new header with its `Native.*.cs` (names, parameter names, types including `uint8_t*` as `byte[]`) and the index blocks with `ShapeEnums.cs`.

**`tests/formats`**: `OcShapeTest` (parse the example of 2.9; write-then-parse byte-identical; every `validate` refusal: undefined reference, cycle, non-finite number, unknown kind, mutable compound used twice, newer version; unknown keys warn once). `OcSurfTest` (parse, defaults, duplicate name, 4097 records, round trip). `OcMatTerrainTest`/`MaterialTest` (extended): `PHYSSURFACE` parses and writes, an old file round-trips byte for byte.

**`tests/game`**: `PhysicalSurfacesTest` (load a table, authored numbers beat a layer's find-or-add, landscape layers register their surfaces, the sampler path still answers). `CollisionMeshSurfaceTest` (a two-slot mesh, one slot with `PHYSSURFACE`: the palette has two entries and each triangle's byte matches its slot; group borders have no crack: the welded vertex set of the two groups at the seam is identical; a mesh with no surface slot produces the version-1 result byte for byte; cache version 2 round-trips and a version-1 file is a miss). `ColliderShapesTest` (the example `.ocshape` builds the expected shape type, counts and mass; `hull mesh` through an injected `MeshSource`; a mutable compound under a body works). `GameLandscapeLayersTest` (extended): the dominant-layer cell palette equals the weights' argmax, and a failed heightfield build now warns. `LevelComponentsTest` (extended): `CollisionShape` apply and capture, canonical form omits defaults, `surface=#hex` round trip; `tests/formats` `OcWorldComponentsTest` (extended) reads and writes the record.

**`tests/world`** (`LevelInstanceTest.cpp`): a `LevelInstance` case where `localTriangleSurfacesFor` is set and the placed body reports the surfaces through `raycast_detail`, and one where it is not set (today's behaviour, shapes cached once per mesh id).

**`tests/editor`**: `CollisionShapeDetailsModelTest` (canAttach rules, record keys, canonical omission, Create-from-rigid-body output), `OcShapeEditorModelTest` (add, wrap, reorder, delete keep a valid tree; validation list equals `fmt::validate`; undo bookkeeping), `PhysicalSurfaceTableModelTest` (duplicate names, hash collision, save text equals `OcSurf` output).

**C# (`Aver.Graph.Tests`, Windows only, ctest `GraphManagedTests`)**: `PhysicalSurfaceNodeTests` (the 7 rows through `NodeRowKit.CheckRows`, `CheckDefaultPins`, `CheckIlValid`, `CheckCompilerSplit`; exec rows refused by the pure compiler; methods return false rather than throw with no native library). `PhysicsShapeApiTests` (bare process: `ShapeParams` packing, `Surfaces.IdOf` equals `Aver.Framework.Surface.IdOf` on 50 names including empty, non-ASCII and the golden four, the array-length guards, `ShapeChild` flattening). Python: `NodeCatalogCheck`, `CsharpApiIndexCheck`.

---

## 6. Headed manual checks for the owner

Use `test-content/PhysicsShapesLab`. Editor first, then the packaged runtime for items 8 and 11, then `--backend vulkan`. The GPU must be idle (the owner plays other games); screenshots at 1440p with a ping for remote runs.

1. **Shapes drop.** Play `Lab.ocworld`: the chair (static compound), barrel (tapered cylinder), plane ramp, hull prop and the grow crate fall or sit as built; `Show > Collision Shapes` wires match the art, compound children show as boxes, centre-of-mass and origin crosses differ for the chair.
2. **Surfaces by ray.** The probe button in the Collision Shape Details reports Ice on the left half of the two-slot floor and Rubber on the right; the ramp's per-triangle surfaces change along its length; the log prints the surface name.
3. **Sliding.** The scripted push (a graph in the lab) slides a crate on each half: the Ice side goes at least 5x farther; with `Project Settings > Physics > Surfaces` open, lowering Rubber friction while it plays changes the next slide.
4. **Character ground.** Walking the lab character from the grass layer onto gravel: `an_surfacebeneath` prints Grass then Gravel; the landscape's blended friction still governs the slide on the slope (terrain sampler precedence).
5. **Vehicle.** The lab car driven across an ice-left, tarmac-right lane: it yaws toward the grippy side under braking, and `Show > Vehicles` slip rings read per wheel.
6. **Grow and swap.** `an_setbodyscale` on the crate: it grows without teleporting, the mass follows (or not with keep), a stack above it shifts; the grow crate's mutable compound adds a child on a key and removes it on another.
7. **Dynamic mesh.** The mesh prop bounces on a convex floor and falls through the mesh floor (the documented limit): confirm both and that the README says so.
8. **Packaged runtime.** Cook and run `AverEngineRuntime.exe`: the `.ocsurf`, the `.ocmat` `PHYSSURFACE` lines and the level's collision surfaces are present, and a C# or graph surface query on the floor answers as in the editor.
9. **Heightfield.** The lab landscape: a hole you can fall through; a runtime crater from a key (set_heights) that a ball rolls into and a resting ball falls into; the paint layers report their surfaces by ray; no seam artefacts at tile borders.
10. **Collision-mesh quality.** Compare the collision triangles of a two-surface mesh with and without surfaces (`aver_phys_body_triangles` overlay): no gaps along the surface border; record the triangle counts and the build time (the first load, then a cache hit).
11. **Cost.** With the GPU idle and 200 crates in contact, record physics ms per step with a project that registers no surface, with five surfaces and per-triangle materials on the floor; report the numbers (none exist today).
12. **Backends.** Items 1, 2 and 9 on `--backend vulkan` (the overlay is lines on both).
13. Full `ctest`, the graph suite via `dotnet run`, and open `docs/nodes/PhysicalSurface.md` to confirm 7 rows documented without hand edits.

---

## 7. Open questions (each with the default chosen; proceed unless told otherwise)

1. **Large-world.** Every position and origin in this ABI is a float array. Default: unchanged; when the LARGE_WORLD area defines its double-precision convention (a `_d` twin or a switched signature) `body_from_shape`, `body_origin`, `raycast_detail` and `body_triangles` follow it in the same pass, which is mechanical because shape-local data stays float.
2. **Mesh vs mesh and mesh vs heightfield** do not collide in Jolt. Default: ship dynamic mesh bodies with that stated limit; no convex decomposition (it needs V-HACD or similar, third-party code the rules exclude); authors build hull compounds, and `hull mesh` gives a one-piece approximation.
3. **Surface id scheme.** Default: the existing FNV hash; a collision between two different names is refused at registration, which is one in billions per pair and detected, not silent.
4. **Friction combine mode.** Default: Jolt's geometric mean and max, unchanged; Unreal's average, min and multiply modes are not added (not requested, and each changes every project's feel). If wanted, one global `aver_phys_set_combine_mode` is a small addition to the same hook.
5. **Leaf material replaces body friction (per side), it does not multiply.** Default: replace; the body's own number is the fallback, so an unset surface behaves exactly like today.
6. **Per-surface-group simplification of collision meshes** keeps borders by `LockBorder` but is UNVERIFIED visually. Default: as designed; the stated fallback is whole-mesh simplify plus nearest-triangle classification.
7. **Pose write-back for dynamic `CRigidBody`** appears to be absent everywhere (1.6, UNVERIFIED). Default: not fixed in this area; `aver_phys_body_origin` exists so the fix is correct for compounds. If the owner wants dynamic authored bodies to move visibly, it is a separate small item for the editor's Play tick.
8. **Mesh palette cap of 32** (Jolt's 5-bit field). Default: refuse beyond 32 with the limit named; `triUserData` carries an unlimited per-triangle id for callers that need more identity than materials.
9. **Non-square heightfields are padded** to Jolt's square: an 8:1 strip costs 8x memory. Default: pad and document; auto-tiling into several square shapes in a static compound is the follow-up if a real DEM strip is ever used.
10. **Holes as NaN or `FLT_MAX`** rather than a separate mask. Default: that; it matches Jolt and costs no extra array.
11. **A mutable compound is single-owner.** Default: first `body_from_shape` binds it; sharing would need a clone path (reconstructing settings from `SubShape::GetPositionCOM`), not worth it for the use cases.
12. **Shape casts, overlaps and `raycast_all` do not report sub-shape detail.** Default: only `raycast_detail`, `subshape_info`, contacts, ground and wheels in this wave. Adding the same blocks to the sphere, box and capsule casts is a mechanical follow-up once QUERIES settles its signatures.
13. **Scale is absolute and mass follows volume** unless `keepMass`. Default: that (the Unreal Body Instance behaviour); relative scaling is two lines for a caller.
14. **Raw sub-shape ids are not stable** across a mutable-compound edit, a `set_shape`, a heightfield edit or a release of the shape. Default: documented in the header; callers re-query. `CHILD` and `CELL_*` are the stable-per-shape identities.
15. **Runtime landscape painting does not update heightfield cells.** Default: cells are set at the collision rebuild; `aver_phys_heightfield_set_cell_surfaces` is there for a game that wants it after a paint stroke.
16. **`hull mesh` / `mesh` leaves in an `.ocshape` are built at load** from `GameContent`, not cooked. Default: rebuild at load (cheap for hulls, a DDC entry for meshes can reuse the collision cache); a cooked `.occol` per shape is the follow-up.
17. **Impact and footstep events by surface.** Default: not added. The data (surface, tag, closing speed) is exposed through `ContactDetails`, `Beneath` and the rows; mapping surfaces to sounds or particles is a data table in game content, not an engine system, and an auto-raised `SurfaceImpact` event would need a per-frame contact walk in every project.
18. **`CCollisionShape` is editor-Play only** because `CRigidBody` is. Default: same; lifting `syncPhysicsFromScene` into the Runtime library so the packaged game builds authored bodies is a separate item (and the one that would make shapes authored in a level work in a shipped game without C#).
19. **Debug rendering.** Default: the triangle read-out and its overlay for selected and moving bodies; Jolt's `DebugRenderer` (compiled in Debug and Release, `Jolt.cmake:915-917`) belongs to the debug-draw area, and its shape-aware overlay can replace this one without an ABI change.
20. **Patent notes, collected.** Physical materials, compound shapes, mutable compounds, height-field collision, per-cell materials and hole masks are standard, long-published collision techniques; everything here is Jolt's own implementation behind engine plumbing, no vendor algorithm is copied. The collision-mesh simplifier is the already vendored meshoptimizer (MIT). Nothing in this area needs a third-party library.
