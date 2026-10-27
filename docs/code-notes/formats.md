# Code notes: formats

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-03 comment strip so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/formats/include/aver/formats/OcProject.hpp

- `kOcProjectVersion`: The versioning policy is strict—a file claiming a higher version is rejected outright rather than read as version 1. The policy is: bump this constant only after the parser is taught what the new version means, never ahead.

- `ProjectDesc::createdWith`: This field stores the engine version that last opened the project, distinct from `engineMinVersion` (which is a floor the manifest may state). Projects that existed before this field has no value; empty is deliberately treated as "current" and adopted on open. Only projects stamped with an older series are prompted to upgrade. This avoids spurious migration prompts for projects that may be fine.

- `ProjectDesc::droneGraph` and `ProjectDesc::inputScheme`: These are project keys (file path keys), not assumed filenames. This design exists because the engine must not assume a project contains a file with any particular name. Without these keys, the drone would spawn with no graph from the editor or drone window (appearing "glitched" because it sat still), and input bindings could not be authored as data. A project stating no value still gets consistent behaviour (drone sits still, or gameplay code builds the context directly).

- `voxelResolution`: The valid range is [32, 512] clamped by `Voxi.cpp::setSettings`. Epic tier derives 512. The 512³ RGBA16F grid with mip chain is ~1.2 GiB, so 512 is a real performance choice.

- `rdStages` (ray-driven stages): The field controls the ray-driven primary's internal shape. 0 = single pass (baseline/fallback, one drawFullscreen); 1 = staged visibility → shadow → shade (D3D12 only, falls back to single pass if unavailable); 2 = staged + half-rate GI (milestone 4: checkerboards GI rays, denoiser reconstructs, deliberately changes the image vs. 1's same-image comparison). This field has no ladder rung—an absent key leaves the engine default (2) alone regardless of RT tier changes (unlike `rtRenderMode` which falls back).

- `restirHistory` (ReSTIR GI temporal history): Measured maxHistory weight impacts on Sponza: history 0 = no overshoot at rest or moving; history 1 (+8% vs. settled); history 8 (+104% vs. settled). Not tier-derived—has no ladder rung—so absent here leaves engine default (0) alone regardless of GI tier.

- `backend` (RHI backend selection): This is a preference, not a guarantee. The backend must be compiled in (AVER_RHI_VULKAN defaults ON in CMake) and must successfully create a device on the target machine; fallback occurs if unavailable. The peek is early in `SandboxApp::main()` before `setBackend` (search for RENDER.BACKEND there). This is read before the editor opens a project—far too early to apply through the per-frame render settings path.

- `frameBudgetMs`: Used to enable quality auto-tuning to hit a target frame time. A renderer that quietly retunes itself cannot be A/B tested meaningfully, so this must be disabled (≤0) for measurements.

- `averSr` and `frameInterp`: Not applied through `ProjectRenderApply.hpp`. Render.voxi must not include render.sr (module boundary per docs/AVERSR.md), so both hosts read these fields directly. The editor's own Display > AverSR choice and CLI `--aversr`/`--frame-interp` outrank the project keys.

- UI settings (msaa, meshShaders, giUpdateInterval): These were live Project Settings controls that applied immediately then vanished on reopen—`captureRenderSettingsFromUi` never read them and no manifest key existed. Setting MSAA to 8x, saving, and reopening would reset to 4x. The asymmetry with `giUpdateInterval` was worse: the flag `--gi-update-interval` existed with no manifest key, while the engine field was tier-derived, silently re-deriving over the flag mid-session with no log.

- `hasGiVolume`, `giCenter`, `giExtent`: Two sliders in Project Settings > Global Illumination were settable in the UI but never persisted. On levels where interesting geometry is not at the origin, GI placement is critical. `hasGiVolume` is a presence flag (not a sentinel) because every component of a centre is legitimately negative. The extent is a scalar (not a vector) because the volume is a cube—the renderer holds `Vec3 giCenter_` + scalar `f32 giExtent_`.

- Post-processing settings (postExposure, postBloom, postAutoExposure, postTonemap): These were settable via CLI (`--bloom`, `--exposure`, `--auto-exposure`, `--tonemap` in SandboxMain.cpp) and the editor's Post panel but had no file persistence. Shipped games always rendered with compiled defaults (exposure 1, bloom 0.06, auto-exposure on, tonemap 2 per RHI.hpp:146-217), losing all author intent. Floats use -1 as unstated, not 0, because zero is a valid and meaningful value (bloom 0 = no bloom pyramid built, a performance choice). On PTTest Sponza at exposure 8: tonemap mode 1 holds chroma 1.41 where mode 2 holds 3.09. Mode 0 (Narkowicz/Hill) is the baseline for all recorded gate baselines in scripts/.

- `hasRenderSettings()`: Bug N9—`giMode` and `denoiser` were parsed but never applied. These fields already existed in the struct but the check `hasRenderSettings()` did not include them, so `ProjectRenderApply.hpp`'s apply block was gated out entirely if only those two keys were stated, silently ignoring `RENDER.GIMODE` 1 or `RENDER.DENOISER` 1.

- Window settings (windowTitle, windowWidth, windowHeight, windowResizable, windowFullscreen): These were hardcoded (1280×720 from `platform::WindowDesc`) with no flag, key, or UI. A shipped game had no way to set its presentation beyond recompiling.

- Import settings (importScale, importConvertAxes, importGenNormals, importGenMips, importMaxTexture): Keys parse and serialize and are editable in Project Settings, but no importer option struct (GltfImportOptions, ObjImportOptions, UsdImportOptions, TextureLoadOptions, MaterialCookOptions) is constructed from ProjectDesc. `AverAssetC` takes its options from its own command line without opening the owning .ocproject. The design intent—to declare project-wide import defaults—was left incomplete.

- Stream settings (streamLoadRadius, streamEvictRadius, streamLoadBudget, streamEvictBudget, streamVerticalRadius, streamLeadSeconds): Applied in `GameStreaming::enable`, seeding `ChunkWorldSettings::stream` before a PCGVOLUME's own `radiusChunks` overrides the radii. These fields were parsed, written back, and editable in Project Settings while reaching nothing—keys whose only consumer is the file they came from. `loadBudget` is documented as "the only thing bounding the frame hitch, since the load is synchronous."

- Physics settings (gravity, fixedStep, physMaxBodies, physMaxBodyPairs, physMaxContacts, physTempAllocatorMb): Gravity uses a presence flag (not a sentinel like the RENDER.* keys) because gravity points down and components are legitimately negative. Physics ceilings (maxBodies, maxBodyPairs, maxContacts, tempAllocatorMb) were hardcoded in `PhysicsWorld.cpp::system.Init(4096, 0, 8192, 2048, ...)` with no flag, key, or UI—a project exceeding 4096 bodies could not be configured without recompiling.

- Audio mix (hasAudioMix, masterVolume, busVolume[]): Uses a presence flag because zero is the most meaningful value (a project that ships with music muted must be able to say so). The bus order matches `audio_abi.h`'s AVER_AUDIO_BUS_SFX / MUSIC / VOICE / UI.

## modules/formats/src/OcProject.cpp

### Version handling
- REFUSED rather than read as VERSION 1
- Number parsed, round-tripped, never compared
- Future OCPROJECT 2 would be read as if keys still meant today, quietly dropped what didn't understand
- .ocmat and AVR1 container refuse unsupported version; text formats were odd ones out
- Version field nobody checks is field that cannot be used
- Ceiling, not equality: older engine projects are migration system's business; only future unreadable

- **isOwnedKey list ordering**: The kOwned[] array in `isOwnedKey()` must match the order keys are emitted in `writeOcproject()`. A key appended to the owned list but missing from isOwnedKey will be stripped from the input but never replaced, causing it to be deleted on every save. Because owned keys are inserted at the first owned line while author's lines stay below, last-write-wins parsing makes the stale line win, so the change appeared to work but reverted on reload.

- **DRONE.GRAPH and INPUT.SCHEME as owned keys**: These are emitted even when empty because they are listed in isOwnedKey. Without this, the writer would strip whatever line the file had without ever replacing it, silently deleting the project's drone graph or input scheme reference on every save.

- **writeOcproject() always emits OCPROJECT header**: The function always returns a manifest with an OCPROJECT header, even if the input `existing` parameter had no header. This was not hypothetical: ProjectScaffold::manifestText passes three comment lines as `existing`, so the editor scaffolded every new project with no header. loadOcproject then refused it with "not an .ocproject: no OCPROJECT header line". The guard then deleted the half-made folder, so the user saw a creation that simply refused. The header is now prepended to ensure any caller (template, importer, migration) that passes a preamble doesn't have the same bug.


## modules/formats/include/aver/formats/OcHair.hpp

- file banner: `.ochair` is a strand HAIR GROOM. Spec: docs/plans/wave11/SOFTBODY_HAIR.md section 2.12, and docs/HAIR.md.
- container: the AVR1 container (Avr1.hpp), following OcLand/OcBt/OcFoliage: a required text chunk plus binary chunks. Strand data is binary because 20k strands of 12 points is 2.9 MB of floats. Every AUTHORED parameter is text so a diff reads and a hand edit is possible.
- chunk HEAD (required): the records below, UTF-8 text (flag Text).
- chunk STRN (optional): u32 strandCount, u32 pointCount; f32 points[pointCount * 3] (cm, mesh-local of the scalp mesh, engine axes); u32 start[strandCount + 1]; u8 material[strandCount]; f32 width[strandCount] OPTIONAL (cm; absent = the look's width).
- chunk SCLP (optional, embedded scalp so a generated groom is self-contained): u32 vertexCount, u32 triCount; f32 position[vertexCount * 3]; u32 tri[triCount * 3]; u8 joint[vertexCount * 4]; f32 weight[vertexCount * 4]. A joint is an index into SCALPJOINTS.
- HEAD text grammar: one record per line; a line that begins with white space continues the previous record; '#' starts a comment; keys and numbers are separated by white space, and a key is followed by as many numbers as it takes.
- HEAD unknown records and keys are skipped (one warning per file) so a groom written by a newer engine still loads. A newer OCHAIR number is refused.
- HEAD example records (default values shown): `OCHAIR 1`; `SCALP Meshes/Head.ocmesh [SUBMESH n]` (the mesh the roots grow from; triangles, and joints and weights if it is skinned); `SCALPJOINTS head neck_01 spine_03` (the order is the skin weight index; resolved against the skeleton at attach); `SIM iterationsPerSecond 360 maxDeltaTime 0.0333 grid 32 32 32 boundsPaddingCm 10 simFraction 0.1 resample 12 initialGravity 0 0 -981`; `MATERIAL 0 collision 1 lra 1 linearDamping 2 angularDamping 2 maxLinearCmS 1000 maxAngularRadS 50 gravity 0.1 1 0.2 0.8 friction 0.2 bendCompliance 1e-7 bendMul 1 100 100 1 stretchCompliance 1e-8 inertiaMul 10 radiusCm 0.1 0.1 0 1 worldInfluence 0 1 0 1 gridVelocity 0.05 0.01 0 1 gridDensityForce 0 globalPose 0.01 0 0 0.3 skinGlobalPose 1 0 0 0.1 gravityPreload 0 simFraction 0.1`; `LOOK 0 baseColor 0.35 0.22 0.12 tipColor 0.55 0.4 0.25 colorVariation 0.15 widthRootCm 0.12 widthTipCm 0.04 roughness 0.35 shift 0.07 tt 1 trt 1 azimuthRough 0.4 tint 0.9 0.6 0.4 scatter 0.5`; `GENERATE seed 12345 strands 18000 lengthCm 28 42 segments 12 curlCm 1.2 curlFrequency 3 clumps 220 clumpRadiusCm 1.5 clumpTightness 0.6 partDirection 0 1 0 partWidthCm 0.8 style 0 0 -1 styleWeight 0.7 mask Textures/HairMask.octex material 0`; `LOD distances 300 700 1500 strandFractions 1 0.5 0.25 0.12`; `PROXY head bone head shape sphere size 11 offset 0 2 4`; `MAXSTRANDS 40000`.
- gradients (gravity, radiusCm, worldInfluence, gridVelocity, globalPose, skinGlobalPose) are four numbers: min, max, the fraction along the strand where the value is min, and the fraction where it is max.
- units are the engine's: centimetres, seconds, engine axes. The physics ABI converts to Jolt's once.
- a `simFraction` in a MATERIAL overrides the SIM one for that material.
- LOOK and LOD are render-only. MATERIAL and SIM are the simulation.
- GENERATE is deterministic from its seed. A groom with GENERATE and no STRN is regenerated at load. With STRN, the explicit strands win and GENERATE is ignored.
- OcHairGradient: min, max, and where along the strand (0 root, 1 tip) each is reached.
- OcHairSim.resample: 0 = strands as they are, else every strand is resampled to this many vertices. initialGravity is in cm/s^2, engine axes.
- OcHairSim: the settings shared by every material of the groom.
- OcHairMaterial: the simulation parameters of one strand material (Jolt's HairSettings::Material, in engine units). simFraction < 0 means the SIM one.
- OcHairLook: how one strand material is shaded (render only).
- OcHairGenerate: the procedural groom, read by HairGenerate.hpp in Aver.Hair. segments: points per strand = segments + 1. curlCm: helix amplitude. curlFrequency: turns per 10 cm. style: the "fall" direction a long strand bends towards. mask: a texture whose red channel weights the roots; empty = uniform.
- OcHairLod: render strand fractions by distance (cm). Three distances separate four levels.
- OcHairProxy: a kinematic convex hull that follows a bone and collides with hair. sizeCm: sphere radius; capsule radius and half height; box half extents. offsetCm: from the bone, in the bone's frame.
- OcHairStrands: points in cm, scalp-mesh space, engine axes, 3 per point. width per strand in cm; empty = the look's.
- OcHairScalp: positions cm, 3 per vertex; triangles 3 per triangle; joints 4 per vertex, an index into scalpJoints; weights 4 per vertex.
- OcHairData.scalpMesh: an .ocmesh path, empty when the groom embeds its scalp. scalpSubmesh -1 = every submesh. materials are by index, and an index with no record simulates with the defaults.
- kOcHairVersion: the OCHAIR record number this build writes and the newest it reads.
- kOcHairMaxStrandVertices: Jolt keeps a strand's vertex count in 8 bits. kOcHairMaxMaterials: the material index is also 8 bits.
- valid() returns false with `why` set for: a strand of fewer than 3 or more than 255 points; a material index above 255; more strands than maxRenderStrands; a non-finite number; more than kOcHairMaxScalpJoints (16) scalp joints; malformed tables; a SCALP that is not an .ocmesh path; a gradient whose two fractions coincide; an embedded scalp that does not hold together; a groom with neither strands nor a GENERATE record.
- writeOcHair refuses (false) a groom that fails valid(), as writeOcFoliage does. Writing, parsing and writing again is byte-identical.
- ocHairHeadText: what writeOcHair puts in the HEAD chunk. Strands and scalp are untouched, so a hand-edited text can be applied to a loaded groom. parseOcHairHead fails on a bad record and leaves `out` unchanged.
- findMaterial / findLook return null when no record has that index.


## modules/formats/include/aver/formats/OcShape.hpp

- file banner: `.ocshape` is a collision SHAPE TREE, the simple collision an entity's CollisionShape component builds its rigid body from. Spec: docs/PHYSICS_SHAPES.md.
- file banner: leaves are the shapes the physics module can build (box, sphere, capsule, cylinder, tapered capsule and cylinder, triangle, plane, empty, convex hull from points or from a mesh, triangle mesh). Decorators move, re-centre or scale one child. A compound holds many children with a pose each.
- file banner: the file names the tree. physics_shape_builder_abi.h builds it (Runtime ColliderShapes.hpp is the walker), so this layer knows no Jolt and no physics.
- file banner: TEXT, like .ocsurf and .oclanes, a thing a person edits and diffs.
- file banner: units and axes are the engine's: centimetres, +X forward, +Y right, +Z up, quaternions (x, y, z, w). A capsule's height is TOTAL (both caps). A cylinder's height is FULL end to end. Both stand along +Z with the top radius at +Z.
- file banner: the example header in the removed block was a Chair: `OCSHAPE 1`, `NAME Chair`, `MASS 12` (kg; 0 or absent = density times volume), `SURFACE Wood` (default surface for leaves that name none), `NODE seat box 40 40 4`, `NODE back box 40 3 25`, `NODE leg cylinder 3 45 radius 0.5`, `NODE top compound static`, `CHILD top seat at 0 0 45`, `CHILD top back at -37 0 75 rot 0 0 0 1`, `CHILD top leg at 32 32 22`, `ROOT top`.
- grammar: `NAME <text>` (rest of the line); `MASS <kg>` (>= 0); `SURFACE <name>` (a physical surface from Surfaces.ocsurf); `NODE <id> <kind> <numbers> [options]`, one per node, id is [A-Za-z0-9_]{1,63}, unique; `CHILD <compound> <node> [at x y z] [rot qx qy qz qw] [user <n>]`, in file order; `ROOT <id>` required, the node the body is built from.
- leaf kinds (numbers in order): `box hx hy hz` (half extents); `sphere r`; `capsule r h`; `cylinder r h`; `tcapsule top bottom h`; `tcylinder top bottom h`; `triangle ax ay az bx by bz cx cy cz`; `plane nx ny nz offset half` (the points with n.x = offset; the side opposite the normal is solid; half bounds its box); `empty x y z` (no volume, centre of mass at that point); `hullpoints x y z ...` (at least four points, three numbers each); `hull mesh <path>` (the convex hull of a mesh's vertices, a content path with no spaces); `mesh <path>` (the collision mesh of a content mesh, with its slot surfaces).
- leaf options (trailing key value pairs, any order): `radius <cm>` (convex radius), `density <kg/m3>`, `tolerance <cm>` (hull), `edge <deg>` (mesh active-edge angle; negative = every edge), `quality runtime|build`, `surface <name>`.
- decorators (one child node each): `NODE <id> rt <child> [at x y z] [rot qx qy qz qw]` (rotated and translated); `NODE <id> com <child> at x y z` (the centre of mass moves by that offset, the geometry does not); `NODE <id> scale <child> sx sy sz`.
- compound: `NODE <id> compound static|mutable`, its children come from CHILD records. A mutable compound belongs to ONE body, so a node of that kind may be referenced once.
- graph is a tree: a leaf or decorator may be referenced by several parents (built once and shared). A cycle is an error.
- forward compatibility: an unknown record or unknown option key is skipped with ONE warning per file. A newer VERSION is refused.
- written canonically: parse(write(d)) is d, and a second write is byte-identical to the first.
- kOcShapeMaxNodes and kOcShapeMaxChildren had no comments.
- shapeKindWord keyword list: box, sphere, capsule, cylinder, tcapsule, tcylinder, triangle, plane, empty, hullpoints, hull (for HullMesh, followed by "mesh"), mesh, rt, com, scale, compound.
- shapeKindNumberCount: -1 for the kinds that take a path, a child or a variable count.
- ShapeNode.nums: hullpoints takes 3 numbers per point. Path is used by HullMesh and Mesh. child is for decorators. pos is `at` for RotatedTranslated and the offset for OffsetCom. rot is for RotatedTranslated, scale is for Scaled.
- ShapeNode options: a negative radius, density or tolerance means the shape's own default. quality: -1 unset, 0 runtime performance, 1 build speed. surface empty = the file's default.
- OcShapeData: nodes and children are kept in file order.
- ocShapeIdValid: id must match [A-Za-z0-9_]{1,63}.
- validateOcShape refuses on: a missing or undefined ROOT; a duplicate or invalid id; a leaf with the wrong count of numbers; a non-finite number; a value out of the ABI's range; an undefined reference; a compound with no children; a cycle; a mutable compound referenced twice; a bad surface name. The same list the editor shows and parseOcShape refuses on.
- ocShapeBuildOrder: the node indices reachable from ROOT, children before parents, each once (a shared leaf appears once). Empty when ROOT is undefined. Stops at a cycle rather than looping. A builder walks this order.
- parseOcShape: TOLERANT OF CRLF, '#' comments and blank lines; strict about everything else, and validateOcShape must pass. False on a problem with `err` set to what and (for a syntax error) which line, and `out` left EMPTY, never the partial state of a file that failed halfway.
- writeOcShape canonical form: the header, NAME / MASS / SURFACE when set, every NODE in order with only the options that are set, every CHILD in order with `at` / `rot` / `user` only when not at their defaults, then ROOT. Numbers are the shortest text that reads back as the same f32.
- saveOcShape: creates the folder; a crash mid-save keeps the old file.


## modules/formats/include/aver/formats/OcSurf.hpp

- file banner: one file per project, Content/Physics/Surfaces.ocsurf. Hosts load it when a project opens, before any level builds a shape, and register every record with the physics module.
- file banner: TEXT, like .oclanes, so the table is a thing a person edits and diffs.
- file banner: a surface is a NAME hashed to a 32-bit id. aver::surfaceIdFromName and Aver.Framework.Surface.IdOf already agree on this, so the file stores the name and nothing else that identifies it.
- example records in the removed header: `OCSURF 1`, `# everything after a # is a comment`, `SURFACE Grass friction 0.6 restitution 0.05 tag 1`, `SURFACE Ice friction 0.03`, `SURFACE Metal friction 0.35 restitution 0.2 tag 4`.
- grammar: `SURFACE <name> [friction <f>] [restitution <r>] [tag <n>]`, one per line.
- a name is [A-Za-z0-9_]{1,63}, case-sensitive, unique in the file.
- friction and restitution are unset unless stated. An unset surface DEFERS to the body it is on: it names a surface for queries and effects without changing how anything slides.
- a stated friction or restitution is clamped to 0..10. A negative value means unset.
- tag is an integer a game uses to pick a footstep group or an effect (default 0).
- at most 4096 surfaces (kOcSurfMaxSurfaces).
- forward compatibility: an unknown key, and a record other than SURFACE, is skipped with ONE warning per file, so a table written by a newer engine still loads in an older one. A newer VERSION is refused.
- parseOcSurf tolerates CRLF, '#' comments and blank lines; strict about everything else. False on a bad record, with `err` set to what and which line, and `out` left EMPTY, never the partial state of a file that failed halfway.
- parseOcSurf refuses: a first record that is not `OCSURF <n>`; a version above 1; a SURFACE with no name or an invalid one; a repeated name; a key with no value, or a non-numeric or non-finite value; more than kOcSurfMaxSurfaces records.
- a header-only file is legal (a project that names no surface). A file with no header, an empty one included, is not.
- writeOcSurf canonical form: the header, then one SURFACE line per record in order with only the keys that are set (friction and restitution when >= 0, tag when non-zero). Numbers are the shortest text that reads back as the same f32. parse(write(d)) is d, and a second write is byte-identical to the first.
- loadOcSurf: False if it cannot be opened or does not parse.
- saveOcSurf: writes `path` (creating its folder) without the truncate-on-open hazard: a crash mid-save keeps the old table.
- OcSurfRecord.friction and restitution: < 0 means unset, defer to the body.
