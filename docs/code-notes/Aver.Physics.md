# Code notes: Aver.Physics

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-27 comment pass so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## scripting/csharp/Aver.Physics/CollisionShape.cs

- file banner: every shape is upright along +Z (a cylinder's height is its full height, a capsule's height is total).
- `ShapeParams`: a negative ConvexRadiusCm uses the default of 5 cm (a triangle's default is 0).
- `ShapeParams`: entries a shape does not use are ignored; the hull tolerance, edge angle, build quality and triangles per leaf apply to hulls and meshes.
- `ShapeParams.ConvexRadiusCm`: Jolt clamps the convex radius to the smallest extent.
- `ShapeParams.DensityKgM3`: density of a convex leaf; it gives the mass of a body made with no mass.
- `ShapeParams.HullToleranceCm`: how far a hull point may sit outside the hull before it is kept.
- `ShapeParams.FavorBuildSpeed`: mesh builds faster and queries slower (procedural and destruction meshes).
- `ShapeParams.MaxTrianglesPerLeaf`: 1 to 8.
- `Plane`: halfExtentCm must be 1 to 1e6 and bounds the broadphase box.
- `Mesh`: palette holds up to 32 surface ids; triPalette is one byte per triangle indexing it; triUserData is one value per triangle.
- `Compound`: a mutable compound belongs to the first body made from it.
- `CreateBody`: a mesh with a mass can be dynamic but collides with convex shapes only (not other meshes or heightfields).


## scripting/csharp/Aver.Physics/Hair.cs

- file banner: hair space is the space of the scalp mesh (units are cm and engine axes throughout; kept).
- `HairMaterialParam`: a gradient's four floats are taken from its index (min, max, min fraction, max fraction).
- `HairSettingsBuilder.SetScalp`: joints and weights are 4 per vertex and index the scalp joint list.
- `HairSettingsBuilder.Finish`: resample 0 keeps every strand as it is; "slow for a large groom" (kept as "slow: call from a worker").


## scripting/csharp/Aver.Physics/PhysicsSettings.cs

- file banner: defaults are Jolt's own values converted to centimetres, so a game that never calls anything here simulates exactly as before.
- `PhysicsSetting` (enum summary): setting one applies to the running world at once and is remembered across a world rebuild (the second part is now on `SetSetting` as "survives a rebuild").
- `PhysicsConfigKey` (enum summary): a set is remembered as pending; it applies when the world is next created, or at once through Physics.RebuildWorld.


## scripting/csharp/Aver.Physics/PhysicsSnapshot.cs

- file banner: a snapshot holds what the simulation derives between inputs (bodies, contacts, constraints, characters, vehicle driver-aid state, the fixed-step accumulator).
- file banner: restores are all-or-nothing; a body, character, vehicle or joint removed since the capture, or ANY constraint added or removed, refuses the restore and changes nothing.
- file banner: inputs (pedals set this frame, forces, kinematic targets, tune values) are not state; a rollback layer re-issues them for each resimulated tick.
- file banner: a snapshot restores only into a world built the same way (handles change when a level reloads), and a single-precision snapshot is refused by a double-precision build.
- `PhysicsSnapshot` (class summary): Dispose releases it; the native table is main-thread only.
- `PhysicsSnapshot.Capture`: with reuse, the buffer is overwritten in place with no allocation once warm; if the reuse handle died with an earlier world it becomes a new snapshot; null is reported via LastResult and the physics error.
- `PhysicsSnapshot.FromBytes`: LastResult says which check failed.
- `PhysicsStateRing` (class summary): the entries newer than a rewound tick are dropped because that history never happened (kept as "Rewind drops the newer entries").
- `Physics.StepExact`: "the replay primitive".
- shape cache banner: a mesh or heightfield shape to bytes and back, so a host can keep the BVH on disk.


## scripting/csharp/Aver.Physics/PhysicsStats.cs

- file header: the update errors are what Jolt reports when a limit is too small.
- `ProfileRow` Name param: the scope's name ("PhysicsStep", "Update", "FindCollisions", ...).
- `ProfileRow` CallsPerStep param: how often the scope ran per fixed step.
- `ProfileRow` Depth param: nesting depth, 0 for a root.
- `Physics.ProfileEnable`: while off a profile scope costs one branch; the table freezes when disabled.
- `Physics.ProfileRows`: empty until the profiler has run for about five fixed steps; Jolt's job scopes appear under one "Worker jobs" row whose times are CPU time summed over the worker threads.


## scripting/csharp/Aver.Physics/SoftBodyExt.cs

- file header: the wrapper covers tetrahedral volumes, long range attachments, Cosserat rods, Jolt's own constraint generator, pressure, grab and release, contact policy, sensors, a collision group and buoyancy; a body that never uses anything here behaves as before.
- `SoftBodyExtParam`: defaults are pressure 0, skin enabled, skin distance scale 1, group 0, mass scales 1, fluid density 0, fluid drag 1, buoyancy flags WorldWater.
- `SoftBodyExtParam`: the header's "Units are engine centimetres, kg, kPa and engine axes throughout" is now shortened to "in engine units (cm, kg, kPa, engine axes)".


## scripting/csharp/Aver.Physics/VehicleExt.cs

- file header: the extension covers the torque curve, gearbox, differentials, tyre curves, tracked vehicles, driver aids, aero, live tuning, telemetry and events.
- `GearboxTiming` summary: the defaults Jolt uses are 0.5 s to switch, 0.3 s to release the clutch, 0.5 s before another switch, up at 60 percent and down at 20 percent of the rev range, and clutch strength scaled to the engine (0 means "scale with the engine's torque").
- `Vehicle.SetChassisShape`: mass and centre of mass are unchanged.
- `Vehicle.DrainEvents`: the queue is cleared at the top of the next physics step, so call it once per step.
- section dividers "Building (before Finish)", "Live (before or after Finish)" and "After Finish": phase labels only; the phase rules are kept in the member summaries.
