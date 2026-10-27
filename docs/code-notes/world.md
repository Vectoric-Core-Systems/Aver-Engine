# Code notes: world

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-27 comment pass so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/world/include/aver/world/ChunkCook.hpp

- file header: "LARGE_WORLD 2.4.5"; the cook would go through f32 CLocal, "exactly the B8 loss past 10 km"; the pure half is Core + Formats + this module's pure files (LevelAssetLibrary, LevelFlatten, ChunkPayload/Codec/RegionFile/Index), so AverAssetC and the editor call the same function and a test needs no scene.
- file header steps: (1) LEVELINST expanded in f64 (flattenLevelInstances); (2) each placement's world AABB in f64 from mesh local bounds (CookInput::meshBounds, built-in unit meshes, or the .ocmesh under contentDir), a subtree bounded by the union of its members; (3) each ROOT and its subtree is LEVEL-GLOBAL or CHUNK-OWNED. Level-global: a className or vehiclePreset anywhere in the subtree, a level-sequence track target or bind name, anything the cook cannot bound (unknown mesh), a value the payload cannot carry; these stay in the slim level.ocworld and are instantiated by the flat path as before. Chunk-owned: everything else; a root whose reach from its own position exceeds half of CookOptions::wideCm is also WIDE (terrain tile, skyline building, bridge), still owned by its position's chunk but listed in the index with its AABB so the streamer keeps that chunk resident while the box is near a viewer. (4) owner chunk = the ROOT's f64 position split once, never f32 or the scene. (5) one payload per chunk, grouped into regions, written PACKED (byte-deterministic: two cooks memcmp equal), then the index and the slim level. (6) a report.
- file header paths: the set lives in <project>/Binaries/Chunks/<levelStem>/ as r.<x>.<y>.<z>.avrgn, world.ocindex, level.ocworld (kept).
- `kCookVersion`: "Part of the source hash." (kept)
- `CookInput::levelAsset`: "names it in the cycle check of its LEVELINST closure" (kept shortened).
- `CookInput::levelStem`: names the cook directory and (fnv1a64) the set's levelId.
- `CookInput::contentDir`: where meshes and child levels resolve (the project's Content directory).
- `CookInput::projectDir`: the set goes to <projectDir>/Binaries/Chunks/<levelStem> unless CookOptions::outDir says otherwise.
- `CookInput::library`: an editor passes its own so an unsaved child is the one cooked (kept).
- `CookInput::meshBounds`: "false = unknown. Absent, or false: the built-in meshes (cube, sphere, drone and the primitives, all taken as the unit box [-1, 1]^3), then the .ocmesh file under contentDir. A placement whose mesh stays unknown is level-global and reported."
- `CookOptions::wideCm`: "Never larger than the streamer's fixed pad (kAuthoredPadChunks chunks each way, AuthoredChunkSource.hpp), so a larger value is clamped to it." (kept shortened)
- `CookOptions::outDir`: "Empty: <projectDir>/Binaries/Chunks/<levelStem>."
- `CookReport::placementsIn`: the level's own placements. `flatPlacements`: after LEVELINST expansion. `instances`: LEVELINST records expanded. `levelGlobal`: flat placements left in the slim level. `wide`: chunk-owned roots listed in the index. `bytes`: region files on disk.
- `CookReport::chunkOwned`: flat placements in payloads (children included) (kept).
- `CookReport::globalClass` etc.: "Level-global ROOTS by the first reason that applied." (kept)
- `CookReport::beyondSidecarLimit`: chunk-owned roots farther than +-1,048,576 cm of world (0, 0): fine for chunks, a warning for the f32 sidecars (kept shortened).
- `CookReport::unknownBoundsAssets`: at most 32 named (kept).
- `CookReport::dir`: where the set was written (empty for a plan) (kept).
- `CookPlan::Region`: chunks [first, end).
- `CookPlan::chunks`: occupied chunks sorted by (region, chunk), a region's chunks are one run (kept).
- `CookPlan::wide`: sorted by (owner, box) (kept).
- `CookPlan::slim`: level-global placements (sequence targets renumbered), the level's other records, lights/decals/prefab instances composed out of its LEVELINST records, no LEVELINST records; STREAM chunks= is retained.
- `CookPlan` class comment: "BORROWS the level handed to planAuthoredCook (the placements it flattened point into it), so that level must outlive the plan." (kept)
- `planAuthoredCook`: "False with why on a cycle, a bad chunk size, or a level the cook cannot flatten." (kept shortened)
- `cookAuthoredLevel`: "Plans, then writes the set through a temporary directory and a rename (an interrupted cook leaves the previous set intact). ... `report` is filled as far as it got." (kept)
- `cookSourceHash`: "the cook version, the codec version, the options, every placement, the rest of the level, the LEVELINST records and the content of the levels they instance. A cooked set whose index carries a different hash is stale." (kept shortened)


## modules/world/include/aver/world/ChunkStreamer.hpp

- file header: "SYNCHRONOUS, ON THE FRAME THREAD, AND MEASURED. That is a decision, not a first draft." scene::World is a process-global singleton whose header says "not thread-safe" (World.hpp:15); every framework side table is a process-global mutable static; no job system to post to, since Jolt's pool is linked PRIVATE inside Aver.Physics by explicit design. The honest version of slice 6 is a synchronous loader with a BUDGET, plus the numbers to decide whether a worker is worth building. docs/CHUNKS.md B4 says exactly this.
- file header: hysteresis is not a refinement. With one radius a camera on the boundary loads a chunk, steps a centimetre, evicts it, steps back, reloads it, forever at whatever the frame rate is. Eviction must happen further out than loading, and the gap must be wider than the jitter. (kept as "hysteresis, or a camera on the boundary thrashes every frame")
- `kAllLayers`: a tower whose floors span chunk layers -1..31 is found without a Z slab.
- `StreamSettings::loadRadius`: a cube rather than a sphere, because the loaded thing is a cube and a spherical radius makes the corners inconsistent.
- `StreamSettings::loadBudget`: the only thing bounding the frame hitch since the load is synchronous. "Zero means unbounded, which is what a 'load everything now' test wants and no running game should use." (kept as "0 = unbounded (tests only)")
- `StreamSettings::verticalRadius`: a surface world wants far less height than width; loading a 7x7x7 cube where a 7x7x3 slab would do is 3x the work for nothing.
- authored block: "Every default below leaves the PCG behaviour exactly as it was."; budgets are spent in whole chunks and at least one chunk is attempted per update whatever they say, so a budget slows a load down and never stops it; the near/far rule is LevelStreaming's rule.
- `loadEntityCap`: "at most this many entities restored per update" (kept as checked after each chunk).
- `padChunks`: a chunk-owned placement that reaches into the load area from its owner chunk has its owner loaded (kept).
- `leadSeconds`: the radius alone is not enough for a moving source. At v cm/s with radius R chunks there are R*chunkSizeCm/v seconds of loaded space ahead, and the loader needs that long to fill the next ring at loadBudget chunks per frame. Past some speed the source arrives somewhere not loaded. Leading residency along the velocity buys back exactly the time the budget needs. (kept the R*chunkSizeCm/v formula)
- `sourceLimitCm`: the resident window is one f32 frame; past the sub-millimetre cliff (kSubMmCliffCm) a position is no longer exact. 0 keeps the pre-origin behaviour (every source streams wherever it is) and is the default; a floating-origin host sets kSubMmCliffCm.
- `StreamSource` class: "The camera is just the first of them -- an actor is the same shape, which is why this is a struct rather than a Vec3."
- `StreamSource::velocityCmPerSec`: centimetres per second; zero for stationary things and for the camera unless a host differentiates it.
- `StreamSource::positionCm`: ORIGIN-RELATIVE f32 centimetres (the scene frame); with a zero origin that is the absolute position (kept as "Origin-relative f32 centimetres").
- `StreamSource::world`: build one with at().
- `StreamStats::lastLoadMs`: wall time of the most recent chunk materialisation. `skippedSources`: sources past StreamSettings::sourceLimitCm this update.
- `authoredStreamSettings`: kept as written, with 12 ms near, 3 ms far, 2 ms evicting, 128 entities a tick.
- `ChunkStreamer` class: camera only in slice 6; an actor is just another source, which is why this takes a list.
- `setHost`: null (the default) is ScatterChunkHost, today's behaviour.
- `setWide`: oversize items (RegionIndex::wide); the owner is kept "however far the owner's own cell is" (kept as "kept within evictRadius").
- `pin`: keeps a chunk resident wherever the sources are (sequences, tooling).
- `restoreOptions`: applied to a restored entity's material and body exactly as RestoreOptions does.
- `update`: chunk keys, anchors and the idle early-out are all ABSOLUTE chunk coordinates, so a rebase between two updates changes nothing the streamer remembers (no chunk loads or evicts because of a shift). A restored root is placed at chunk origin - origin. Zero is the pre-origin behaviour, bit for bit. This module links Aver.Scene and not Aver.Physics, deliberately, so a build without physics still compiles. A caller that passes null is saying it has no physics, not that bodies may be leaked.
- `isResidentAt(Vec3)`: origin-relative against the origin of the last update() (kept).
- `origin()`: the origin the last update() ran against (zero before the first).
- `clampToResident`: THE BOUNDARY HOLD. When the loader has not kept up, the options are not equal: a synchronous load hitches with unbounded size; letting the mover through means it stands on a chunk that does not exist (for a character, a fall through the world). Holding at the last loaded chunk is the only recoverable option and the only one visible in a test. Backed off by marginCm so the caller does not sit on the face and re-trigger every frame. Returns `from` when even that is not resident, meaning the mover is already somewhere unloaded and moving it further cannot help.
- `residentChunks`: for hosts that enumerate, e.g. ChunkWorld building the "which entities are mine" answer an outliner or save path needs. There was no such enumerator before; querying one coordinate at a time is useless without knowing which coordinates to ask.
- `pendingPrepareCount`: chunks decoded and held back by the host's prepare gate.
- `PendingChunk`: held until Ready or no longer wanted; bounded (kept as "Bounded by kMaxPendingChunks").
- `anchors_`: one entry per source (and corridor step), rebuilt each update so a stale velocity never lingers.
- `anchorPos_`: the same anchors as f64 absolute centimetres, for the wide items' distance test.
- `lastScanAnchors_`: the anchor chunk coords the last full wanted/evict scan ran against, and whether that scan left anything undone. Together they are the whole precondition for skipping the next scan; see update()'s idle early-out for why chunk granularity is the right resolution.


## modules/world/include/aver/world/LevelFlatten.hpp

- file header, composition reused: LevelAssetLibrary::resolve already does the hard half for every consumer (child level, its LIOVERRIDEs applied, removed placements and their subtrees dropped, nested overrides moved onto the nested record). LevelPack's baker composes f32 matrices in the level's own frame for instance rows; the runtime (LevelInstances) composes through the entity hierarchy; neither is an f64 world position. This is the f64 one, using the runtime's compose rule (component-wise scale, rotation product, rotated scaled offset), and takes its children from resolve, so it is not a third copy of the override logic. (kept shortened: "LevelPack composes f32 matrices and LevelInstances composes through the entity hierarchy")
- file header, what comes out: every placement of the level, then every placement of every instance (depth first), parents before children, each with the pose instantiate() would narrow from. A ROOT carries its world position in f64 (an instance's root is composed through its instance chain; its rotation is the composed quaternion as Euler degrees, the form every consumer takes). A CHILD carries its authored parent-relative pose. Rotation and scale are the same f32 casts instantiate() makes, so a flat placement and a chunk restored from it agree bit for bit on both.
- `poseOfPlacement`: rotation from quatFromEulerDeg on the f32 casts; scale as f32 casts; position kept in f64.
- `composePose`: instantiate()'s composition of a child under a parent, with the position taken in f64 (kept shortened).
- `rotateD`: a vector rotated by a quaternion, in f64.
- `FlatPlacement::roll/pitch/yaw`: degrees, the f32 casts instantiate() makes (or the composed rotation of an instance's root).
- `FlatPlacement::id`: stable and non-zero; unique within the flat list.
- `FlatPlacement::topIndex`: index in the level's own placement list, or -1 for one that came out of an instance.
- `FlatPlacement::depth`: 0 for the level's own placements, 1 inside an instance, 2 inside a nested one, and so on.
- `FlatLevel::lights/decals/prefabInstances`: level-global records of the instances, composed into the world frame (their own level frame is gone once flattened).
- `FlatLevel::warnings`: named, one line each (kept shortened).
- `FlatLevel::instances`: LEVELINST records expanded, nested ones included. `deepest`: the deepest nesting reached (1 = a record of the level itself). `unloadedInstances`: no comment. `droppedFoliage`: FOLIAGE tables inside instances (not composable into a flat list).
- `FlatLevel::held`: keeps every child level's resolved records alive (kept).
- `flattenLevelInstances`: `level` stays the owner of its own placements; levelAsset names it for the cycle check (an instance of the level inside itself is refused); a warning for an instance that fails to load, as it is at runtime (kept shortened).


## modules/world/include/aver/world/PhysicsShapeCache.hpp

- file header: the BVH is mostly the cost of a mesh collision shape, the reason shapes are shared per mesh id at all.
- file header: aver_phys_shape_save / _restore turn a built shape into a blob and back (physics_state_abi.h). This file keeps the blobs as one AVR1 container (subtype PSHP, one chunk "DATA") per mesh at <project>/Saved/DerivedDataCache/Physics/<16 hex digits of the key>.pshp, beside the Collision cache whose triangles the shapes are built from.
- file header: a miss is never an error; a missing, short, foreign, stale or damaged file answers "not cached" and the caller builds the shape the slow way (kept shortened).
- file header: off by default via aver_phys_shape_cache_enabled(): PHYSICS.SHAPECACHE, or AVER_PHYS_SHAPE_CACHE=1 for tools. The default waits for a measurement of restore against build on the real levels (the PhysicsBenchTool shapes in tests/physics).
- file header: the file layer needs only Core and Formats; createMeshShapeCached needs the physics module and exists only where it does.
- `physicsShapeKey`: the key is FNV-1a 64 over sourceKey, the physics module's shape cache version (aver_phys_shape_cache_version) and the triangle counts. GameContent passes the key it already uses for its collision cache (mesh path, source size, source mtime, collision cache version), so a stale source invalidates both caches. The counts notice a material slot that stopped colliding: the slot filter removes triangles, so the same file builds a different shape.
- `physicsShapeCachePath`: "<dir>/<16 hex digits>.pshp" (kept).
- `storePhysicsShapeBlob`: creates the directory; "False with why on failure; the caller carries on uncached."
- `loadPhysicsShapeBlob`: "False is a miss, whatever the reason (no directory, no file, a bad container or subtype)."
- `PhysicsShapeCacheStats::hits`: shapes restored from a blob. `misses`: looked up and not found / not usable. `stores`: blobs written. `restoreMs`: total time in aver_phys_shape_restore, hits only. `buildMs`: total time building shapes on a miss.
- `createMeshShapeCached`: aver_phys_create_mesh_shape's input (engine-space cm vertices, 3 indices per triangle, no surface palette); restored when cacheDir is non-empty, key is non-zero, the cache is enabled and the blob is there, otherwise built and stored.
- `createMeshShapeFromSource`: sourceKey 0 = this mesh is not cacheable (for example no file behind it) and the cache is skipped; the key is built from sourceKey, the shape cache version and the triangle counts.
