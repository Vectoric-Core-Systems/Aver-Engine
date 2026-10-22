# PhysicsShapesLab

Engine test content for shapes, physical materials and terrain collision (`docs/PHYSICS_SHAPES.md`, design `docs/plans/wave11/SHAPES_MATERIALS.md`).
Not a demo game: the visible things are the built-in unit cube standing in for each shape (what you SEE is not what collides; turn on
**Show > Collision Shapes** to see that), plus four small procedural meshes. Nothing is third-party (`SOURCES.md`).

## Before the first run

The four meshes are written as OBJ under `Content/Source` and must be cooked once:

    python make_shapes_lab.py              # rewrites everything below, then cooks the meshes with build/bin/AverAssetC.exe
    python make_shapes_lab.py --no-import  # rewrite only; cook by hand: AverAssetC convert Content/Source/<Name>.obj --out-dir Content/Meshes --base <Name>

(`--assetc <path>` points at another AverAssetC.) Until they are cooked the placements that name them (`TwoSlotFloor`, `Ramp`, `RockHull`,
and the two mesh props) log a missing-mesh warning and are skipped; everything else works. The committed content here was written with
`--no-import`, so the cooked meshes are the owner's first step.

Then open the project, open `Maps/Lab.ocworld`, press Play. The spawn is at (-600, 0) looking along +X; distances are centimetres.

## What is where

A placement's `COMPONENT CollisionShape asset=... motion=dynamic` gives it the shape of an `.ocshape` and makes it fall (no level file carries a
Rigid Body, so the record creates a default one; the **Motion** combo of **Details > Collision Shape** changes its motion type, and the `.ocshape`
`MASS` is its mass). The stand-in cubes carry `noscale=1`: a body is otherwise built at the entity's world scale, and a cube scaled to 40 would make a 40 x chair.

**The cubes do not move.** `CRigidBody` has no pose write-back (design open question 7): a dynamic body falls, slides and settles INVISIBLY while its
stand-in stays where it was placed. Turn on **Show > Collision Shapes** and watch the wire; the yellow and cyan crosses are the body's centre of mass and origin.

| Where | What | Checks |
|---|---|---|
| y = -1000 | **Chair**: a static compound of two boxes and four cylinders (`Physics/Chair.ocshape`), 12 kg, surface Wood | Falls and settles on its legs. Show > Collision Shapes draws the seat, back and legs; the yellow cross (centre of mass) is away from the cyan one (origin) |
| y = -600 | **Barrel**: a tapered cylinder, 30 cm at the top, 25 cm at the bottom (`Barrel.ocshape`) | Settles on its small end (the wide radius is +Z) |
| y = -200 | **GrowCrate**: a mutable compound of a core box and an arm (`GrowCrate.ocshape`) | The arm is a child you can add and remove at run time from C# (`Body.AddChild` / `RemoveChild`); the body keeps its mass |
| y = 200 | **RockHull**: the convex hull of `Meshes/LabRock.ocmesh` (`RockHull.ocshape`, `hull mesh`) | Settles on a hull face; the wire is the hull, not the 8-triangle rock |
| y = 600 | **MeshPropOnFloor**: the closed drum as a DYNAMIC TRIANGLE MESH (`MeshProp.ocshape`) | Falls onto the floor box and rests: a dynamic mesh collides with convex shapes |
| y = 1000 | **PlaneRamp**: a static plane tilted 30 degrees (`PlaneRamp.ocshape`) and **PlaneCrate** on it | The wire is a large tilted slab; the crate slides down it |
| (2000, 0) | **TwoSlotFloor**: an 800 x 400 cm slab, ice on the left half, rubber on the right (slots `M_LabIce`, `M_LabRubber`, each naming its surface with `PHYSSURFACE`) | **Details > Collision Shape > Probe below the entity** on a prop standing on it: Ice on the left, Rubber on the right. **MeshPropOnMesh** is dropped from 3 m: it falls THROUGH the floor (mesh vs mesh does not collide in Jolt; the documented limit) |
| (3200, 0) | **Ramp**: 900 cm long, falling about 20 degrees towards +X, three 100 cm lanes (Ice, Rubber, Metal) with a **Crate** at the top of each | The ice crate slides away, the metal crate creeps, the rubber crate holds. With **Project Settings > Physics > Surfaces** open, lower Rubber's friction while it plays: the next slide changes (Save registers the numbers) |

## Owner checks (section 6 of the design, as far as this lab goes)

1. **Shapes drop.** The chair, barrel, grow crate, hull rock and mesh prop fall and settle as above; the wire matches the art; the plane ramp's crate slides.
2. **Surfaces by ray.** The probe button reports Ice, Rubber, and on the ramp each lane's surface; with a surface the table does not name it prints `#` and the hex id.
3. **Sliding.** The three crates on the ramp; edit and Save the table while it plays.
4. **Dynamic mesh.** The mesh prop rests on the floor box and falls through the two-slot floor.
5. **Collision-mesh quality.** `aver_phys_body_triangles` via the overlay on the ramp and the two-slot floor: the surface borders (Ice | Rubber) have no gap; compare the triangle counts with and without `PHYSSURFACE` lines in the `.ocmat` files (delete a line, press Play again: the cache notices the changed material).
6. **Packaged runtime.** Not covered by this lab: `CRigidBody` bodies, and so `CollisionShape` bodies, are built by the editor's Play only. The packaged game gets shapes through C# (`Aver.Physics.CollisionShape`), and its mesh surfaces and landscape cells through the level.
7. `--backend vulkan`: the overlay is lines on both backends.

Not in this lab (and said plainly): a heightfield level with a hole and paint layers (`.ocland` needs the landscape editor; the heightfield is covered headlessly by
`tests/physics/src/TerrainTest.cpp` and `tests/game/src/PhysicalSurfacesTest.cpp`), a scripted push, the vehicle on a split-friction lane (see `test-content/VehicleLab`), and the
cost measurement with 200 crates (item 11 of the design).

## What the generator writes

`make_shapes_lab.py` writes `Content/Source/*.obj` (one `usemtl` per material slot), `Content/Physics/Surfaces.ocsurf`, `Content/Materials/M_Lab*.ocmat` (three of
them with a `PHYSSURFACE` record), `Content/Physics/*.ocshape`, `Content/Maps/Lab.ocworld` and `Game.ocproject`.
