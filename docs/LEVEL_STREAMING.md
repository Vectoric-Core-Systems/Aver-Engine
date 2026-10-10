# Level streaming (authored levels)

Streams an authored `.ocworld` -- its PLACE/PLACEG placements, the meshes they use and its FOLIAGE
instances -- by distance from the viewer, in the editor (fully editable) and the runtime. Separate from
chunk streaming (`docs/CHUNKS.md`), which streams PCG scatter through region files; region files cannot
carry what a placement carries (triangle collision, animation, classes, slot materials) and would need
rewriting on every edit.

**The `.ocworld` stays the only source of truth.** Streaming is a residency policy over its parsed
placements, never a second copy of them.

## 1. Format

`.ocworld` keeps only what is authored:
- Header record `STREAM cell=<cm> load=<cm> evict=<cm> [lazy=<dir>[,<dir>...]] [data=<path>]` turns
  streaming on (`OcWorldData::stream`: enabled, cellCm, loadCm, evictCm, lazyDirs, dataPath). `lazy=` names
  the content folders whose meshes upload on demand; `data=` the generated data, relative to the project root.
- PLACE/PLACEG/CHILD/CHILDG `id=<16 hex>` -> `OcWorldPlacement::placementId`: stable per-placement identity.
  Written only when set, so other levels round-trip unchanged.

GENERATED data lives under the project's `Binaries/Streaming` and is never authored (`OcStream.hpp`):
- `<level>.ocstream` (text): `SOURCE` (hashPlacements of the level it was made from -- a mismatch marks
  it out of date), `FOLIAGE <content .ocinst> <cell-sorted copy>` per table, and `B <id> <world AABB>` per
  placement. Bounds are matched to placements by id at load (`OcWorldPlacement::hasBounds/boundsMin/Max`,
  runtime-only).
- `<level>/<table>.ocinst`: each FOLIAGE table re-sorted into cells (`ICEL`/`IRUN` chunks,
  `cellOcInstances`); the Content original is untouched.

The editor rewrites the `.ocstream` on every save of a streamed level (bounds from the records and live
meshes). World Settings > Level Streaming > Regenerate Streaming Data (and `AverAssetC stream`) rebuild it
all from the mesh files (`fmt::bakeOcStream`). A placement with no bounds (data missing or out of date)
stays loaded rather than being guessed at.

## 2. Residency -- `world::PlacementStreamer` (modules/world, no scene/GPU dependency)

```cpp
struct PlacementStreamSettings { f32 cellCm = 6400, loadCm = 25000, evictCm = 30000;
                                 u32 loadBudget = 128, evictBudget = 2048; };
class PlacementStreamer {
public:
    // Item i = one ROOT placement (with its descendants); bounds are the subtree's world AABB.
    void build(const std::vector<Aabb>& bounds, const PlacementStreamSettings& s);
    u32 add(const Aabb& b);                 // a new root (editor create); returns its item index
    void setBounds(u32 item, const Aabb& b);// moved/edited
    void remove(u32 item);                  // deleted; never reported again
    void pin(u32 item, bool on);            // pinned: loaded now, never evicted (selection, undo, gizmo)
    // Viewers in world cm. Appends items to load (nearest first, <= loadBudget) and to evict
    // (furthest first, <= evictBudget). Caller instantiates/destroys and then calls markLoaded/markEvicted.
    void update(const std::vector<Vec3>& viewers, std::vector<u32>& toLoad, std::vector<u32>& toEvict);
    void markLoaded(u32 item); void markEvicted(u32 item);
    bool resident(u32 item) const; usize residentCount() const;
};
```
Distance is horizontal (XY) from the viewer to the item's AABB (0 inside it), so an 800 m terrain tile
stays resident while the viewer is over or near it. A uniform grid of `cellCm` cells lists every item
whose AABB overlaps the cell (big items appear in many cells); a query visits cells within `evictCm`.

## 3. Meshes on demand -- GameContent

Meshes under a level's `lazy=` folders are indexed at project open but not uploaded:
- `GameContent::loadProjectMeshes` first reads the header records of `Content/Maps/*.ocworld` (stops at
  the first PLACE) and collects every `STREAM lazy=` folder; `.ocmesh` files under them are indexed
  (`contentIndex_`) and skipped.
- `bool acquireMesh(rhi::IDevice&, u64 id)`: refcount++; uploads on first acquire with the same code
  path the eager loader uses (refactored into one per-mesh function: parts, bounds, slot materials,
  collision source). `void releaseMesh(rhi::IDevice&, u64 id)`: refcount--; at 0 the GPU mesh and its
  parts are destroyed through the device's deferred-destruction path (never mid-frame on in-flight data).
- `bool meshLoaded(u64 id) const`. A draw of an unloaded mesh is skipped, never a fallback cube.
- `void prefetchMesh(u64 id)` / `bool meshReady(u64 id) const`: two worker threads read the `.ocmesh`
  and its collision disk cache ahead of `acquireMesh`, which then only uploads. The workers touch no
  engine state; finished reads nobody acquired are dropped oldest first past 512.

## 4. Foliage

`game::loadLevelFoliage` keeps the parsed tables on the CPU. With cells, only cells whose AABB is within
the level's `loadCm` (evict at `evictCm`) of the viewer are pushed through `setFoliage`; the push is
redone only when the resident cell set changes, at most every 0.25 s. Prototype meshes are acquired
for groups in resident cells and released when none remain.

## 5. Bake -- `AverAssetC stream` / Regenerate

`AverAssetC stream <level.ocworld> --content-dir <dir> [--project-dir <dir>] [--cell m] [--load m]
[--evict m] [--foliage-cell m]` sets the STREAM record and ids in the level and writes its generated data
(section 1); the editor's Regenerate Streaming Data button runs the same `fmt::bakeOcStream` after saving.

## 6. Hosts

Runtime (GameLevel/GameApp) and editor (Sandbox) share one driver, `game::LevelStreaming`
(Runtime): it owns the PlacementStreamer, the placement->entity map, mesh acquire/release and the
filtered instantiate of a set of roots (hierarchies whole; same InstantiateOptions as a full load), and
destroys entities and their bodies on eviction. Hosts call `tick(viewers)` once a frame.

Hitch budget (2026-10-10; before it, every tick that loaded its 128 roots took 150-370 ms on Caldera,
and in the Debug editor single roots took 30-200 ms (collision built on the main thread) and terrain
tiles 0.6-6.7 s):
- Off the main thread (GameContent's three prefetch workers, nearest request first): the `.ocmesh`
  read, vertex conversion, the per-material part split, the collision mesh (read from the disk cache,
  or on a first visit built and written there; material slots are judged from their `.ocmat` files)
  and the Jolt mesh shape (its BVH build; `aver_phys_create_mesh_shape` is thread-safe). A root loads
  once every mesh it names is prepared; a mesh with no prefetch yet (the 512-job queue was full, and
  the farthest queued job only gives way to a nearer one) is not ready. Pinned roots load at once.
- The editor's mesh hook, for streamed meshes, shares LOD0's vertex buffer across the LOD ladder and
  skips the cluster data only `--lod-per-cluster` and `--lod-cluster-stats` read.
- On the main thread, budgeted: GPU buffer creation, material binding, entities and bodies. Loads run
  nearest first one root at a time within 12 ms per tick while the nearest missing root is within a
  quarter of the load distance (a level opening, a teleport), falling to 3 ms at three quarters.
  Evictions (furthest first) get 2 ms per tick; the rest wait for the next tick.
- Physics mesh shapes are shared across batches (`InstantiateOptions::meshShapes`, owned by
  LevelStreaming) and released when their mesh unloads.
- Render side (Voxi), all staged over frames:
  - The ray-traced geometry table keeps each mesh's range while it stays in the set and appends only
    new meshes (it re-copied every mesh, ~1 GB on Caldera, whenever one arrived). New meshes are copied
    from the upload heap 16 MB a frame, in pieces; their instances are masked out of every ray until
    complete. A repack (the table out of room) moves held meshes from the old buffers, GPU to GPU.
  - First-time BLAS builds are capped at 8 MB a frame. A mesh over 262k triangles (a terrain tile) is
    traced as BLASes over 131k-triangle ranges of its index buffer (`BlasGeometry::firstIndex/
    indexCount`), each a first build under that cap, appearing as it is built.
  - Measured on a Caldera flight: the frame a 2M-triangle tile arrived was 300 ms of GPU (one BLAS,
    then ~260 ms of copies); now its acceleration-structure work is ~7 ms a frame. Same final image.
- The editor logs `[Hitch]` lines (frame time, stream time, loaded/evicted/resident, foliage) when a
  frame passes 100 ms or a streaming step 40 ms.

Editor rules (fully editable; sandbox/src/SandboxLevelStream.cpp):
- Before an entity is evicted its live state is written back into the placement record (transform,
  material, flags, name, vehicle), so edits survive eviction and the record is what is saved. Not during
  Play: what streamed in during Play reloads from its record when Play stops.
- `saveLevel` writes every placement record: live ones refreshed from their entity, the rest as stored,
  then the `.ocstream`. An entity made in the session (drop, duplicate, undo of a delete) becomes a record
  at once; a deleted one loses its record. A reparented or new child is saved as a root at its world pose.
- Pinned (never evicted): what is selected (released on deselect), every entity an undo entry names, and
  every object with a sequence track. Sequence tracks of objects not loaded when the level opened are kept
  aside and written back on save.
- World Settings > Level Streaming: on/off, cell/load/unload distances, Regenerate Streaming Data, a
  loaded-count readout, and Save and Reopen to apply a change.
- Known limits: streamed `vehicle` placements park (they are not driven in Play); the outliner lists only
  what is loaded.
