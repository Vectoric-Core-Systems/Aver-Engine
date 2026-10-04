# Chunked worlds, region streaming, and generation as you go

Coordinate hierarchy, region files, and chunk streaming are implemented and tested. Editor and runtime both load levels through a unified path (`aver::world::instantiate`). All nine slices (0–9) are complete and verified with baseline-free tests; floating origin (slice 10) remains future work (no rebasing code exists in the tree).

Citation caveat: `SandboxApp.cpp` was split (2026-09-16, `sandbox/CMakeLists.txt:9-11`), so the editor-side `SandboxApp.cpp:NNNN` citations below are stale (the symbol is still the anchor); `Runtime/` citations are in the right file. Where this document contrasts the editor with `GameApp` (`Runtime/src/GameApp.cpp`), the editor now calls the runtime library for level load, streaming and landscape, so each claimed editor-vs-game divergence needs re-checking before relying on it. Written 2026-08-08 (supersedes the 2026-08-02 revision, git `0fcfcaa`) from code re-derivation plus an adversarial fact-check of every `file:line`; where it disagrees with that earlier revision, the table below says so.

---

## 1. What changed from earlier revisions

| Earlier claim | Current state | Why |
|---|---|---|
| Region is **16³ chunks** (256 m) | Region is **1024³ chunks** | 16³ survives as the *group*, one level down (§5). |
| `.region` is **AVR1 container** | `.region` is **own mutable format**; AVR1 stays for immutable assets | AVR1 pins `DirOffset` to 64 with payloads immediately after (`Avr1.cpp:162-175`), so growing the directory displaces every payload; and `parseAvr1` copies *and* xxHash64-verifies **every** payload on open (`Avr1.cpp:280-282`). Both disqualify it for an archive you seek into and mutate. |
| `.region` is **read-only** | **Runtime-writable**, sector-allocated | The world generates as the player moves. Read-only was the earlier way of dodging AVR1's constraints; dropping AVR1 removes the reason. |
| Level is `FLAT` **or** `CHUNKED` | Coordinate hierarchy is **universal**, residency is **opt-in** | Two load paths means the non-default one rots; flat is the **oracle** for testing chunked (§9). |
| "Do not build origin rebasing" | Still true — rebasing has **known anchor** (region) and **hard constraint** (X/Y only) | §4.4. |

### Blockers

B1 (no partial read), B2 (AVR1 exact `FileSize`), B3 (no compression), B4 (no job system) and B5 (CLR is Win32-only and declines) stood, verified. **B1 is closed** by slice 2's `aver::File`; B3, B4, B5 remain open by design. B2's specifics are worse than first stated: `FileSize` sits *inside* the CRC32C range covering bytes `0x00..0x3B` (`Avr1.cpp:212-213, :244`), so any growth rewrites the header *and* recomputes its CRC. Four more were missed at first; B6 was fatal to the feature as scoped.

**B6 — there was no `IDevice::destroyMesh`** (closed by slice 3; `RHI.hpp` now declares it at `:438`). `RHI.hpp:281-284` declared `createMesh` and nothing that releases one, so every mesh lived until the device was destroyed. Streaming in without streaming out is a slow memory leak with a camera attached. Two subsystems had already hit it and given up:
- `LandscapeRenderer` capped its mesh cache instead of evicting (`LandscapeRenderer.cpp:96-102`), and `forgetAll()` cleared the handle map **without freeing anything** (`LandscapeRenderer.hpp:45`): one vertex and one index buffer leaked per node.
- `SkinnedScene` recycles rather than frees, explicitly because there was no `destroyMesh` (`SkinnedScene.hpp:94-97`), and silently degrades past 256 resident entities (`SkinnedScene.cpp:13, :63`).

Independently, mesh geometry lives in **UPLOAD-heap** memory permanently (`D3D12Device.cpp:2177-2187`), CPU-visible write-combined and read over PCIe every frame; thousands of chunk meshes there is the wrong steady state regardless of the leak.

**B7 — entity slots are exhaustible and never recycled** (still open; threatens generate-as-you-go). An entity is 24 index bits + 7 generation bits (`Entity.hpp:21-25`). A slot serves generations 1..127 and is then **retired forever** (`World.cpp:246-252`; `tests/scene/src/SceneTest.cpp:130-131` pins this); generation does not wrap. A world that spawns and despawns as the player moves burns slots permanently. Two high-water-mark side tables never shrink: `classOwners()` is a dense `std::vector` indexed by entity index (`FrameworkAbi.cpp:179`), and `CName` offsets point into `World::Impl::nameBlob`, an append-only `std::string` **never compacted** (`World.cpp:95, :386-396`; every `create()`/`setName()` appends, `retireSlot` never touches it). A long procedural session grows memory monotonically even at constant live-entity count. **Consequence (§8): do not destroy and respawn actors on chunk transitions.**

**B8 — the level writer loses precision past ~10 km before f32 is involved.** `OcWorld.cpp:22-27` (now `modules/formats/src/OcWorld.cpp:24`, still `"%.6g"`) formats every number with six significant digits: `1234567` cm is written `1.23457e+06` and reads back `1234570`, so **a 12.3 km coordinate moves 3 cm every time the editor saves**. This is the strongest independent argument for the hierarchy: with `(region, chunk, local)` the text-formatted number is a local offset of at most four digits, which `%.6g` holds exactly. The `f64` in `OcWorldData` (`OcWorld.hpp:19`) is decorative, since every consumer narrows to f32 at the point of use (`GameLevel.cpp:47-50`, `SandboxApp.cpp:5511-5514`); widening the format alone changes nothing observable.

**B9 — two diverging copies of the level load path** (closed by slice 0). `Runtime/src/GameLevel.cpp:37-175` (game) and `sandbox/src/SandboxApp.cpp:5464-5561` (editor) were independent implementations that already disagreed: the editor applied `SUN`/`SKY` at load (`:5551`) and drove the cloud layer from a `PCGVOLUME` named `"Sky"` (`:5497-5506`); the game applied only `FOG` and deferred sky to `GameApp` (`GameLevel.cpp:87-95`). Each narrowed f64 to f32 in its own near-identical loop. Every slice would have had to be written twice, hence slice 0.

---

## 2. Design decisions

### 2.1 The coordinate hierarchy is universal; residency is opt-in

A flat level and a streamed level differ only in the residency set — compute it once or on demand. The hierarchy is free: `regionOf(pos)` / `chunkOf(pos)` take no storage. All levels live in region coordinates; streaming is an opt-in filter on what is resident.

**Flat is the test oracle.** Load the same level both ways (chunked with everything resident, and flat) and assert identical entity sets and transforms. This is the strongest test; delete flat and you are left comparing streaming against itself. This is a testing argument, not a compatibility one: flat stays permanently, not until migration finishes. Secondary: the gate baseline is 18 probes x 9 configurations, bit-exact, keyed to existing levels; migrating them all would mean re-recording exactly when an unchanged reference is most valuable.

### 2.2 Module split

Three tiers, following the tree's `option(AVER_MODULE_*)` convention and its force-off-and-explain pattern (`CMakeLists.txt:134-136`, `:152-154`):

1. **`modules/world`** — Always compiled. Coordinate types and mapping; no I/O (~200 lines at design time). *Not* optional: it is a value type like `Vec3`, and behind a flag every caller needs an `#if`. It was README-only and absent from the build at design time (`modules/world/README.md:9-11`); it is built now and also holds the level-instance and streaming code.
2. **`modules/platform`** — Always compiled. Ranged file I/O via `aver::File`; not streaming-specific, any large-asset loader wants it.
3. **`AVER_MODULE_STREAM`** — Planned optional, default OFF until proven (`MCP`, `CMakeLists.txt:56`, is the default-OFF precedent): region format, sector allocator, residency, generation, eviction. As built, no such flag exists in any `CMakeLists.txt`: `Aver.World` is unconditional (`modules/world/CMakeLists.txt`) and holds the streaming code, so the rule below is unexercised.

`STREAM` hard-depends on `SCENE` (force OFF with a message, as `FRAMEWORK` does). `PHYSICS` and `FRAMEWORK` are soft: they register as participants if present, so the protocol must tolerate their absence.

**Rule:** A level declaring itself streamed must refuse to load with a named error if the module is compiled out. Never silently load empty; a compile flag is worse than F# declining at runtime (B5) because there is no runtime signal at all.

### 2.3 Naming: `chunk` is already taken

`AVER_MODULE_LANDSCAPE` (*"chunked heightfield terrain, LOD and chunk geometry"*, `CMakeLists.txt:50`) defines "chunk" as a quadtree node's fixed-vertex mesh tile (`ChunkMesh.hpp:17-23`, 4485 vertices per node regardless of level). World streaming uses the same name at a different scale. Namespace rather than rename (renaming is churn for no behaviour): `world::ChunkCoord` vs `landscape` chunk. Landscape chunks will eventually participate in a streaming chunk — reason to keep names distinct.

---

## 3. Coordinates

### 3.1 The hierarchy

```
global chunk coord   gc  : i32   = floorDiv(worldCm, chunkSizeCm)
region coord         r   : i32   = (gc + 512) >> 10
chunk within region  cl  : i16   = ((gc + 512) & 1023) - 512    // [-512, +511]
local offset         lo  : f32   = worldCm - gc * chunkSizeCm, in [0, chunkSizeCm)
```

Region `r` covers `gc ∈ [1024r - 512, 1024r + 511]`, so region 0 is centred on the world origin: the origin sits mid-region, not on a seam, and an existing flat level at the origin is nowhere near a boundary. Local offset is `[0, chunkSizeCm)` — deliberately unsigned — so the truncate-vs-floor bug lives in exactly one function (`floorDiv`) instead of at every call site. The codebase already has this bug: the bounded PCG sampler truncates toward zero while the infinite one floors (`PcgVolume.cpp:50-52` vs `:78-80`), and `PcgShaders.hpp:64-66` admits it becomes wrong "the moment a volume is centred on the origin".

### 3.2 Bit budget — exact

```
cl ∈ [-512, +511]      = 1024 values = exactly 10 bits signed per axis
3 axes                 = 30 bits, fits u32 with 2 spare
  group  = top 6 bits/axis  -> 64³  = 262 144 groups   (18 bits)
  slot   = low 4 bits/axis  -> 16³  = 4 096 slots      (12 bits)
                                       18 + 12 = 30 ✓
```

The inner 16³ group is the earlier design's whole region, moved down a level.

### 3.3 Precision

| max \|coord\| | f32 ULP at 1 cm/unit |
|---|---|
| 5.24 km | 0.625 mm |
| **8.19 km** (512 × 16 m) | **0.625 mm** |
| 10.486 km (2²⁰ cm) | 1.25 mm — the cliff |
| 16.38 km | 1.25 mm |
| 100 km | 10 mm |

f32 holds sub-millimetre out to `2²⁰ cm = 10.486 km` and no further. Constraint: `512 × chunkSizeCm < 2²⁰ cm`, so `chunkSizeCm ≤ 2047`: 16 m (1600 cm) is the largest round size that qualifies, 20.48 m the first that fails. Default: `chunkSizeCm = 1600`, confirmed 2026-08-08; `OcWorld.hpp:43` already defaults `OcPcgVolume::cellSizeCm` to 1600 ("one 16 m chunk per cell"), so the two agree by default. It is per-level, stored in `.ocindex` (§5), so levels may choose otherwise within the constraint. The cost of 1600 over a power-of-two size (1024 cm is the largest that qualifies) is that world to chunk is a real integer division, so `floorDiv` must handle negatives instead of getting it free from `>>`: one function, one test.

Stored region-relative positions hold **0.625 mm** in f32; absolute f32 loses that property at any distance. Inside a chunk, precision is ~1.2 µm: stored positions are uniformly micron-accurate at any distance from the origin, whereas absolute f32 degrades linearly and `%.6g` (B8) faster still.

### 3.4 Rebasing: not yet, but the anchor and constraint are known

The runtime is absolute f32 end to end: `Vec3`/`Transform` (`Math.hpp:32-33, :246-249`), `CLocal.xf` (`Components.hpp:23-26`), `float*` across both C ABIs (`scene_abi.h:5-7`, `framework_abi.h:111-112`), and Jolt built single-precision by explicit documented decision (`physics.jolt/CMakeLists.txt:47-55`). No origin-shift, floating-origin or camera-relative code exists (verified by a case-insensitive sweep over nine directories). Build the check, defer the machinery; two things are settled: **the anchor is the region** (the float origin snaps to a region origin, so rebases happen at fixed, known, testable points already in the file format, not at an arbitrary hysteresis radius), and:

**Rebase X and Y only. Never Z.** `averAtmoCamAlt()` (`RHIShaders.cpp:327`) treats `gCamPos.z * 1e-5` as altitude in km above a 6360 km planet; shifting Z breaks the atmospheric integral (aerial perspective, `:332-341`) and the CPU-baked fog reference (`D3D12Device.cpp:2624`). `RHIShaders.cpp:320-326` records that Z crossing zero already broke the ray-sphere intersection once. Worlds here are surface-based; vertical extent does not need rebasing.

Two verified implementation costs when rebasing lands: (1) `prevViewProj_` (`VoxiRenderer.cpp:1019, :1035`) reprojects the RT shadow and reflection histories and describes the *old* origin after a shift; fixable exactly by composing the shift translation into it, but it must be designed in, not discovered; (2) `PerFrameCB` is hand-mirrored field-for-field across `D3D12Device.cpp:516-540`, `RHIShaders.cpp:99-136` and every consumer's own copy, with nothing checking the layout (`RHIShaders.cpp:120-123` warns about exactly this), so an origin field touches all of them in lockstep.

Cloud noise degrades with distance from the origin today. `D3D12Device.cpp:304` feeds absolute `wpos * (1/featureSize)` into a lattice; it goes through a `floor`/`frac` lattice (`:280-281`) and far out `frac()` loses mantissa bits. A live symptom of the absolute scheme.

---

## 4. The region file

Not AVR1. AVR1 stays an excellent immutable asset container; a region file is a mutable, seekable, sector-allocated archive, and forcing one into the other buys nothing (§1). Suffix `.avrgn`. Anvil's structure, one level deeper, because 1024³ = 1 073 741 824 slots makes Anvil's single dense table impossible:

```
sector 0,1   HEADER x2      magic 'ARGN', version, chunkSizeCm, regionCoord[3], levelId,
                            worldSeed, generatorVersion, groupTableSector, groupCount,
                            sectorCount, freeListSector, serial, crc32c
                            -- TWO copies with a monotonic `serial`; reader takes the higher
                               one that passes CRC (the crash safety Anvil lacks)
             GROUP TABLE    sparse, sorted by groupIndex, only occupied groups:
                              u32 groupIndex (18 bits used) | u32 groupDirSector
             GROUP DIR      dense, 4096 slots, one per group:
                              u32 firstSector | u32 byteLength | u32 crc32c    -> 12 bytes
             FREE LIST      sorted extents { u32 firstSector, u32 sectorCount }
             PAYLOADS       4 KiB sectors
```

**Why two levels, in numbers.** For a fully explored surface region:

| layout | resident cost |
|---|---|
| flat dense table | 8.00 GiB — impossible |
| flat sparse directory, 16 B/entry | 128 MiB — too big to hold |
| **sparse 64³ groups → dense 16³ dirs** | **~128 KiB resident + 32 KiB on demand** |

Steady-state lookup is two cached reads plus one ranged read. (Sizes in the table assume a fully explored surface region of 1024×1024 columns × ~8 vertical.)

**Writing.** Allocate from the free list, write payload, flush, patch the group dir entry, flush, write the header with `serial + 1`. A crash at any point leaves the previous `serial` valid and loses at most the one chunk in flight.

**Determinism, honestly scoped.** Writer determinism cannot hold unconditionally for a file mutated at runtime, because sector allocation depends on history. So a **cooked** region (written fresh) must be byte-identical across runs (`memcmp` two cooks), and a **compacted** region is the canonical form: compaction of any region must equal a fresh cook. Baseline-free.

**Uncompressed by design.** B3 stands: the only DEFLATE in the tree is stb's PNG-internal zlib, whose compressing half is not linked into any engine module. Density comes from encoding: u16-quantised chunk-local positions, a palette plus bit-packed indices, sorted-delta ids; `.ocland` already quantises, an in-tree precedent.

**Crash safety.** Claiming it loosely is how a format ends up like Anvil. A payload is never written over live data, so a crash mid-payload leaks sectors rather than corrupting any; the directory entry is patched and synced, and a torn entry fails its CRC and is refused by name — one chunk lost, region usable. The header goes last to the copy that is *not* live, with `serial + 1`. A crash between directory and header leaves the new chunk visible under a header one step stale — data correct, index's staleness check fires. That is degradation, not corruption. Making it atomic needs a double-buffered directory, which is real cost for a case that degrades safely.

### 4.1 What a chunk payload may **never** contain

Each entry is an id-shaped, process-local value: written to disk, it produces a file that loads correctly exactly once, in the writing process.

| Never serialise | Why | Store instead |
|---|---|---|
| `CMeshRenderer::material` | `static_cast<int32_t>(table.size()) + 1` from a DLL-local map (`SceneAbi.cpp:377-390`) — a pure function of the order names were first seen in *this process*; `GameContent::registerBuiltins` interns seven at startup (`GameContent.cpp:109-117`) and `GameLevel::load` interns more per placement (`GameLevel.cpp:61`); table at `SceneAbi.cpp:64-67`. | the material **name** |
| any `FieldKind::String` field | eight bytes indexing a DLL-local `std::vector<std::string>` (`SceneAbi.cpp:70-73`, minted `:262`); the framework already treats this as radioactive and replays strings at spawn (`FrameworkAbi.cpp:457-468, :510`). | the string |
| `CName::offset` / `len` | offsets into the never-compacted `nameBlob` (`World.cpp:95, :386-396`). | the name |
| component ids and field ids | registration-order counters (`World.cpp:453, :523`); `Builtins.cpp:94-96` spells out the hazard: inserting a component anywhere but the end shifts every later id. | a FourCC per component, resolved on load |
| `World::at()` indices | dense over live entities and **shift on every flush** (swap-and-pop, `World.cpp:234-243`). | never a stable identity |

Asset references are safe: FNV-1a-64 over a forward-slash `Content/`-relative path; those ids survive packaging unchanged.

**Trap on the way in:** `World::addComponent` hands back **zero-filled** bytes (`ComponentPool.cpp:63`), so default member initialisers never run. A chunk loader that attaches components without memcpy'ing defaults produces invisible, non-animating entities. This already caused the `CMeshRenderer` visible-bit bug once, which is why `CAnimator`'s flags are negated (`Components.hpp:98-114`).

---

## 5. The index

`<level>.ocindex`, sorted lexicographically by (x, y, z) (the original design said Morton code; see slice 5 for why not).

```
levelId  u64
chunkSizeCm u32        -- authority; one size per level, enforced here
worldSeed u64
generatorVersion u32   -- persisted regions produced by SOME generator; mismatch must be detectable
per region:
  i32  regionCoord[3]
  u64  contentHash     -- must equal the region header's own
  u32  chunkCount
  str  relativePath
bounds: i32 minRegion[3], maxRegion[3]
```

Four jobs: existence without opening files; staleness detection; chunk size lives here once; generator-version skew detection. Rebuildable by scanning region files, so a corrupt index is recoverable.

---

## 6. Generation as you go

**Store deltas, not baselines.** A chunk's content is `f(worldSeed, chunkCoord)` unless a persisted override exists. Regions stay near-empty until modified; eviction of an unmodified chunk is free; the save file is proportional to what changed.

Four verified constraints:

1. **Generation must be CPU-side for anything authoritative.** GPU-vs-CPU agreement is one ULP, not bit-exact, because DXC fuses multiply-add where MSVC under `/fp:precise` does not (`GameApp.cpp:256-265`, `PcgShaders.hpp:8-12`). Use the GPU volume for *visual* fields (clouds); use the CPU path for *placement*.
2. **`pcg::sampleInfinite` had zero callers and no test** (`tests/pcg` exercised only the bounded `sampleDensity` and the `PCGVOLUME` round trip; the "infinite" spec was built at level load and never evaluated). Slice 8 made it the first caller and gave it its first test.
3. **The GPU volume path cannot be a per-chunk generator as it stands.** Readback is 3 frames by contract (`PcgVolume.hpp:107-113`); `VolumeBuilder` is **single-slot** — `request()` overwrites `spec_` and resets state (`PcgVolume.cpp:177-180`) — and it reallocates both GPU buffers whenever the byte size changes (`:145-175`).
4. **F# and `PcgApply` only run inside `Sandbox.exe`** (`SandboxApp.cpp:5406`). `GameApp` does not reference the script host. A shipped game that generates as it moves needs that wiring, and by B5 generation must still work with the host declining.

Determinism itself is real: splitmix32 is mirrored bit-for-bit across F#, C++ and HLSL, with a per-voxel GPU-vs-CPU check under `--pcg-volume-test`.

---

## 7. Threading

Revision 1's blocker B4 stands. Six `std::thread` sites exist (WASAPI, MCP, the directory watcher, IDE scan, two ToolsMenu jobs); none is a pool. Jolt's `JobSystemThreadPool` is linked **PRIVATE** inside `Aver.Physics.dll` by explicit design (`modules/physics/CMakeLists.txt:6-15`), so reusing it means creating an exported seam the CMake comment deliberately foreclosed.

Hard constraints:

- **`scene::World` is a process-global, non-thread-safe singleton** (`World.hpp:15-19`). So is every framework side table (`FrameworkAbi.cpp:137-232`). A background thread may not touch spawn, destroy, or the scene.
- **The RHI declares no thread-safety contract** — whether `createMesh` may be called off-frame is currently undefined. That must be decided before any background upload.
- There is **one DIRECT queue** (`D3D12Device.cpp:1528-1530`); no copy queue, no async compute.
- `kFrameCount` is **2** (`D3D12Device.cpp:32`); no 3-deep pipelining to hide latency.
- `waitForGpu()` is a full drain reachable from ordinary calls, including `reconcileClearValue` at the top of *every* `beginFrame` (`D3D12Device.cpp:2301, :2131`).
- Every texture upload creates a fresh fence, allocator, command list and Win32 event and blocks INFINITE (`D3D12Device.cpp:3819-3853`).

**Therefore: the worker produces plain POD; the frame thread materialises.** A worker does file I/O and generation and hands back a flat placement list (asset id, transform, material *name*, flags). The frame thread turns that into entities under a per-frame budget. One worker to start, measured — and sized in awareness that Jolt already takes `hardware_concurrency() - 2` (`PhysicsWorld.cpp:267-269`).

Copy the drain contract from `Win32DirectoryWatcher` (`:104-114, :128-133, :232-241`): push under a mutex, move-append, clear, explicit overflow flag. It is the one proven cross-thread queue in the tree.

---

## 8. Actors and residency

### 8.1 An actor approaches an unloaded chunk

Predictive request along velocity, N chunks ahead, where N covers worst-case load latency at current speed. Every actor tagged as a streaming source contributes to the residency set; the camera is just one source.

If the target chunk is still not resident when the actor arrives: **hold the actor at the boundary.** The alternatives are synchronous load (unbounded frame hitch) or letting it through (fall through the world). Holding is recoverable and visible in a test.

### 8.2 An actor leaves all loaded range

The actor is owned by its chunk. When the chunk evicts, the actor serialises into the chunk's delta and despawns; when the chunk reloads, it comes back.

Cost: this needs a lifecycle hook that does not exist. The dispatch table is exactly ten entries (`ManagedDispatch.cs:14-29`, mirrored in `framework_hooks.h`); none concerns spatial residency; adding one bumps `AVER_FW_DISPATCH_VERSION` and breaks the managed binding until it is regenerated.

A ready-made near-miss: `AVER_FW_END_TRAVEL` (`framework_hooks.h:31`) — *"level travel; the current level's actors are torn down"* — is **defined and dispatched by nothing**. Worth reusing rather than inventing a parallel concept.

### 8.3 Slot churn — the one that is dangerous

By blocker B7, every serialise/despawn/respawn cycle burns a generation, and a slot dies after 127 lives, permanently. An actor crossing a chunk boundary back and forth 127 times retires its slot forever. Meanwhile `classOwners()` and `nameBlob` grow to the high-water mark and never shrink.

**Slice 1 does not stream actors at all.** Placements stream; actors stay resident, with an explicit cap and logged warning when approached. Actor despawn is deferred until either a suspend-without-destroy state or generation recycling exists.

This is deliberate: the alternative works in a ten-minute test and degrades over a long session in a way no test would catch.

### 8.4 Other verified traps on the unload path

- **`aver_fw_end_play` destroys every entity whose class handle is non-zero** (`FrameworkAbi.cpp:796-802`), including anything a streaming system spawned and was tracking itself.
- **`World::destroy` is deferred and takes the whole subtree** (`World.cpp:256-280`, `:346-354`). Between the call and the next flush the entity is still live and still returned by `aver_scene_at`; only `destroyPending(e)` distinguishes it. `GameRender.cpp:51` tests it; `Game.AllActors` (`Game.cs:56-64`) does not. Chunk churn exercises this every frame.
- **Side tables are cleaned by different owners at different times** — `pawnByController` only in `destroyActor` (`FrameworkAbi.cpp:556-564`), `AnimSystem::posed_` only in tick, `SkinnedScene::live_` only in update. A chunk unloader calling `scene::World::destroy` directly, which both existing unload paths do (`GameLevel.cpp:197`, `SandboxApp.cpp:2503`), leaves them stale.
- **`SandboxApp::destroyEntity` cleans only the entity it was given** (`:2500-2513`) while `World::destroy` retires the subtree — children's labels and static bodies leak. A live bug today; a constant one under streaming.
- **There is no entity→physics-body link at all.** `aver_phys_add_dynamic_box` returns an opaque int and takes no entity (`physics_abi.h:46-58`); `GameLevel` keeps a parallel `std::vector<int32_t>` with no association (`GameLevel.hpp:74`); the editor keeps a separate map (`SandboxApp.cpp:5271`). **Streaming a chunk out cannot find the bodies belonging to its entities.** Must be fixed in the participation protocol, not worked around.
- **Physics runs in metres while the engine runs in centimetres** (`cmToM`, `PhysicsWorld.cpp:366, 454, 623`), and `Convert.hpp:19` divides in f32 — a second independent precision ceiling.

---

## 9. Verification

Baseline-free oracles first.

**No baseline needed:**

- **Chunked equals flat** — everything resident, both paths, identical entity sets and transforms. The strongest single test. This is why flat stays.
- **Cook determinism** — two cooks, `memcmp`. And **compaction equals a fresh cook**, extending the property to runtime-mutated files.
- **Round trip across a process boundary** — save, exit, reload, compare. **Must fork or restart:** the five process-local values make a same-process round trip pass while the file is unloadable anywhere else.
- **Boundary conservation** — total entities across chunks equals the flat count, with entities deliberately placed *on* boundaries and at negative coordinates where floor-vs-truncate bugs live (the tree already has one: `PcgVolume.cpp:50-52` vs `:78-80`).
- **Crash safety** — truncate a region mid-write at each sector boundary; assert the previous `serial` loads and exactly the in-flight chunk is lost.
- **Precision guard** — assert no authored entity lies outside `2²⁰ cm`, or warn with the measured ULP.
- **Slot budget** — run a long synthetic streaming session; assert retired-index count and `nameBlob` growth stay bounded. This is the B7 regression test and the one most likely to be skipped.
- **Streaming with F# absent** — force `ScriptHost` to decline; assert the world still streams.
- **Streaming with `AVER_MODULE_STREAM` off** — assert a streamed level refuses to load with a named error rather than loading empty.

**Needs a render gate re-recording:** nothing in slices 0–7, if done correctly; no slice before floating origin should move a pixel in an existing flat level, and that is itself the test.

---

## 10. Slices

Each states what proves it done. Ordering is by dependency.

**Slice 0 — unify the two level load paths. DONE.** The shared loop is `aver::world::instantiate`; the rotation contract is `world::LevelTransform.hpp`, which the editor re-exports. `saveLevel` now starts from the parsed header rather than a default-constructed one. `SPAWN`, `BUILD`, `ID` and the sun's `lux` ride through; the surface writes back as a NAME via `aver_scene_material_name` (scene ABI 1.0 → 1.1, additive).

*Evidence:* `LevelInstanceTest` — 111 assertions, new, the first coverage the load path ever had. All 32 headless suites pass. Gates run before and after are byte-for-byte identical — all 18 probes, same raw RGB, same verdict — so the 17 failures are entirely the pre-existing atmosphere drift from `94b6736` and this slice moves nothing. `AVER_BUILD_GAME=OFF` builds clean (the editor does not depend on the game runtime). *Pre-existing unfixed:* `AVER_MODULE_SCENE=OFF` does not build at HEAD and after, with an identical error set in `GameApp.cpp`, `GameContent.cpp`, `SkinSceneTest.cpp` and `SandboxApp.cpp` (none level-loading code); `Aver.World` itself compiles cleanly in that configuration.

**Slice 1 — the coordinate hierarchy. DONE.** `modules/world/include/aver/world/ChunkCoord.hpp`: coordinate types, `floorDiv`/`floorMod`, packing, `splitCm`, `toWorldCm`, `toRegionRelativeCm`, `chunkSizeValid`, `withinSubMillimetre`. Header-only, Core-only, no I/O: a value header like `Vec3`, which is why `ChunkCoordTest` links only `Aver.Core` and would catch anything leaking into it.

*Evidence:* `ChunkCoordTest` — 45 assertions covering ~400 000 individual comparisons (`floorDiv` against a floating-point reference over 10 001 × 6 divisors; region round trip exhaustively over four regions). Packing exhaustive over all 1024 values plus all 64 sign corners; `splitCm` every 100 cm across four regions. The §3.2 identities are `static_assert`s, so they fail the **build**, not a test run. All 33 suites pass. Purely additive — no shipped binary changed, gates unaffected.

*Two numbers, now measured:*
- A region-relative coordinate holds **0.625 mm** in f32 at worst, sampled every 16 m out to 100 km.
- At 100 km an absolute f32 **loses 5 mm** of a position the split reconstructs exactly.

*Deliberately not here:* a `CHUNKED` record in `.ocworld`. The earlier design put the level declaration in this slice; making the hierarchy universal and residency opt-in (§2.1) leaves nothing for a level to declare yet. Chunk size becomes authored data with the index, in slice 5.

**Slice 2 — ranged file I/O in `modules/platform`. DONE.** `aver::File` — RAII native handle with `readAt`/`writeAt`/`setSize`/`sync` in three modes none of which truncate, plus `deleteFile` and `renameFile`. Closes blocker B1.

*Why native handles, not `std::fstream`:* positional I/O needs no seek-between-read-and-write dance, and `std::ostream::flush()` reaches the OS page cache and stops there. `sync()` is `FlushFileBuffers`/`fsync`. Crash safety is an *ordering* argument — payload on disk before the directory entry — and without a real barrier that ordering is fiction, because the OS may write the two back in either order.

*Evidence:* `FileRangeTest` — 72 assertions against real files. a fake backend would prove nothing about whether a write at a 4 GiB offset lands where asked. `writeAt` leaves the file length alone; a `Read`-mode handle refuses writes; a read running one byte past EOF **fails** rather than half-succeeding; gaps and grown regions read as zeros; a file truncated mid-record fails the whole-record read while its surviving prefix still reads. The **>4 GiB path really ran**, via a sparse file: an 8-byte write at 4 GiB + 4 KiB reads back, and offset 4096 — where a 32-bit truncation would have landed — is still zero (the check skips loudly rather than lying if the filesystem declines sparse files). The surviving-prefix property is what slice 5 layers a CRC on. All 34 suites pass. **Zero deleted lines.**

*One deliberate non-suppression:* `File::open` traces in every mode, including `Create`. The trace answers "did the package read outside itself"; tracing only reading modes was defensible but would leave a new open path uncovered and falsify `FileSystem.hpp`'s claim to be the whole API. If slice 7's runtime-written regions trip `verify-game.ps1`, that is a real question about where they belong.

**Slice 3 — `IDevice::destroyMesh`. DONE. Closes B6.** Plus `IResourceFactory::destroyBlas`/`blasMesh` (non-pure: `tests/render.ui` and `tests/render.actorpreview` each implement that interface with a mock, and a new `= 0` would break both). `LandscapeRenderer` now evicts least-recently-used instead of capping; `forgetAll` frees.

*Three hazards, all real, all handled:*
- **A BLAS holds the mesh's GPU addresses.** `destroyMesh` destroys any structure built from that mesh first; unlike a raster draw, traversing freed memory faults the device rather than drawing a hole. `createBlas` now refuses a destroyed mesh — the slot exists, so the old bounds test passed and the build would have proceeded over a cleared vertex view.
- **`createSkinTargetMesh` shares its source's index buffer.** `GpuMesh` records `ibOwned`/`ibShares`/`ibSource`; a skin target never frees borrowed indices; a source with live sharers refuses to be destroyed, loudly, rather than pulling triangles out from under them.
- **Voxi builds its TLAS from `drawsPrev_`** — last frame's draws — so a mesh freed between frames is still named there. Its cache asks the factory what a cached BLAS is actually for and drops the entry itself, rather than needing every `destroyMesh` caller to tell it.

**The handle is never recycled.** The slot is cleared and kept; a stale handle addresses a dead mesh and draws nothing. Recycling would make it address a *different live* mesh and silently draw wrong geometry — much harder to notice than a hole. The cost is a few dozen bytes per destroyed mesh against the megabytes reclaimed; if churn ever makes that matter the fix is a generation in the handle, not bare reuse.

*Evidence:* `LandscapeEvictTest` — 35 assertions, headless, using a counting `IDevice`. It turns "the cache leaks GPU memory" into the arithmetic property `created − destroyed == live`. Measured over a 24-frame camera sweep with a 32-node cache: **118 created, 86 destroyed, 32 resident**, conservation exact, root never evicted, `forgetAll` ending at `created == destroyed`. All 35 suites pass.

*Gate status:* A sweep was started and then abandoned at the user's request; **it must not be re-run without asking, because it fronts an editor window.** What it returned before stopping: **16 of 18 probes byte-identical** to the recorded before-picture; `sunlit` was flagged `[BAD-PROBE tiny-rect 96x24]` and `penumbra-rt`'s confirm read degenerate at `45x24`: the collapsed-probe-rectangle artefact of a disturbed window, not a rendering change. **Inconclusive for those two probes.** The static argument stands: nothing in the engine calls `destroyMesh` yet; landscape has no engine consumer; `drawMesh`'s new `alive` test is true for every live mesh; Voxi's new staleness test is false for every live BLAS. A confirming sweep is owed whenever it suits the user.

**Slice 4 — the participation protocol. DONE.** `ChunkPayload` (portable per-entity description + `capture`/`restore`), `ChunkPartition` (`ownerChunkOf`, `partitionWorld`, `captureAll`), `BodyRegistry` (entity→body link). No files yet.

*The three ownership rules are enforced and tested:* by the **root's origin**, never bounds; **hierarchies travel whole** — `CHierarchy` is intrusive, so a child without its parent is a dangling handle rather than a hole; **oversize is reported**, not handled, since a 200 m bridge in a 16 m world must stay resident regardless of distance (bounds overlap and origins do not, hence root origin).

*Where the coordinate hierarchy pays off:* a captured root's position is stored **chunk-local**, `[0, chunkSizeCm)`. For an entity 9 km out — absolute f32 grid **0.625 mm**, stored-offset grid **0.00061 mm**, ~1000× finer.

A four-digit chunk-local number is also exact through the text writer's `%.6g`, where an absolute one is not (B8).

*`BodyRegistry` exists because the association did not:* `aver_phys_add_static_box` returns an opaque int and takes no entity, so `GameLevel` kept a body vector with no entity link, the editor its own map, and a character's capsule is a private C# field. Its `detachSubtree` takes the whole subtree, because `World::destroy` does (`SandboxApp::destroyEntity` walking only the entity it was handed is a live child-body leak today). Payload carries transform, name, objectId, tags, mesh renderer and static body.

*Evidence:* `ChunkPartitionTest` — 41 assertions. Round trip over a fixture: negative coordinates, exact chunk boundaries (1599/1600/−1600/−1), hierarchy with children several chunks from the root, entity 9 km out. **Worst world-position drift: 0.000000 cm** (the fixture uses whole centimetres, exact in f32 below 2²⁴, so this proves the round trip lossless rather than that the split recovered anything; the 9 km magnitude figure above is where the split earns its place). All 36 suites pass. Purely additive — zero deleted lines; nothing outside `modules/world` and `tests/world` touched, so gates were not run.

*Deliberately not yet done:* the hosts still keep their own body tables; wiring belongs with streaming that needs it. A **gameplay class** is not captured — level placements never become framework actors today (`aver_fw_class_of` returns 0 for all).

**Slice 5 — `.avrgn` + `.ocindex`, cooked and read-only. DONE.** `ChunkCodec`, `RegionFile`, `RegionIndex`.

*Evidence:* `RegionFileTest` — 66 assertions against real files. **Two cooks of the same content, fed in opposite orders, are byte-identical**; a damaged payload is refused **naming the chunk**; header copy A destroyed and the region still opens on copy B; both headers destroyed and it is refused; corrupt index is rejected by checksum **before any field is trusted**. All 37 suites pass.

*Two deliberate deviations from design:*
- The group directory entry is **12 bytes**, not 8 — `{firstSector, byteLength, crc32c}`. 4096 × 12 = 48 KiB = 12 sectors exactly. The per-chunk CRC makes "refused, naming the chunk" possible, and a region is up to 16 km across, so "this file is corrupt" is not actionable.
- The index is sorted **lexicographically by (x, y, z)**, not by Morton code. Morton buys locality in the *file*, which matters when entries are paged; this index is fully resident, so it buys nothing, a Morton key over three signed 32-bit axes is 96 bits of complexity, and lexicographic is equally binary-searchable and deterministic.

*Not yet done:* the **cross-process** round trip. Everything here runs in one process; the five process-local values all round-trip perfectly inside the process that minted them; the codec refuses to carry any of them by construction, but that is an argument, not a measurement, until a test forks (settled by slice 8). §9 is emphatic that this is the single easiest test to get wrong. **Chunked-equals-flat** also waits on slice 6.

**Slice 6 — native streaming residency, camera only. DONE.** `IChunkSource` / `RegionChunkSource` (index + region files, LRU over open handles) and `ChunkStreamer`.

**Synchronous, on the frame thread, measured** — B4's answer. `scene::World` is a process-global singleton; every framework side table is a process-global mutable static; Jolt's pool is linked PRIVATE. A background thread may not create an entity. The honest slice 6 is a budgeted synchronous loader plus the numbers to decide whether a worker is worth building.

**Hysteresis is enforced.** `setSettings` raises `evictRadius` when it does not exceed `loadRadius`, and says so. Equal radii is the thrash case: a source on the boundary loads a chunk, moves a centimetre, evicts it, moves back, forever.

*Evidence:* `ChunkStreamTest` — 40 assertions, headless, over a fixture spanning **9 region files**.
- **CHUNKED EQUALS FLAT.** Build flat, snapshot, partition, cook, destroy, stream back: 177 entities, **every world position bit-identical**, every surface name preserved.
- **A flight across all nine regions** delivers each one's content over **9 distinct regions**, with the open-file cache staying within its cap of 8.
- **A bounded resident set** across an 80-step there-and-back sweep: 225 loads, 207 evictions, peak 27 chunks against a wanted set of 147.
- **Jitter across a chunk boundary causes zero churn** once settled.
- **A stale index is refused, not served** — a region whose header hash disagrees with the index is rejected naming staleness.

*The measurement this slice asks to be written down:* **chunk materialisation averaged 0.0066 ms** over 225 loads, 0.0078 ms worst. That is a small fixture — payloads of 3–24 entities — and it scales with entity count, not chunk count, so it is a floor. On this evidence a worker thread is **not** yet justified; revisit when a chunk carries real content.

*Slot churn (B7's threat), measured:* after 225 loads and 207 evictions, **0 entity slots retired**. The 7-bit generation is nowhere near exhaustion at this scale, but the flight lasts seconds and B7 is about sessions, so this bounds nothing about a long one.

All 38 suites pass. Purely additive within `modules/world` and `tests/world`; gates unaffected.

**Slice 7 — runtime writes. DONE.** `RegionWriter`: sector allocator with coalescing free list, in-place chunk add/replace/remove, alternating durable headers, compaction.

**The format went to version 2.** v1 pinned the group table at sector 2 with the directories immediately after, so a chunk landing in a group that does not exist yet needs a new directory *and* a longer table, and a table that cannot move displaces everything behind it: the same mistake that disqualified AVR1, one level up. It went unnoticed while the format was cook-only, because a cook lays the file out once. `groupTableSector`, `freeListSector` and `freeListCount` are header fields now. No v1 file exists outside a test temp directory, so the bump refuses them rather than carrying a compat path.

**What crash safety does and does not promise.** A payload is never written over live data. The directory entry is patched and synced, and a torn entry fails its CRC and is refused by name — one chunk lost, region usable. The header goes last to the copy that is *not* live, with `serial + 1`. A crash between directory and header leaves the new chunk visible under a header one step stale — data correct, index's staleness check fires. This is not a transaction. Making it atomic needs a double-buffered directory, real cost for a case that degrades safely — so it is named rather than pretended away.

*Evidence:* `RegionWriteTest` — 46 assertions.
- **Crash safety at EVERY sector boundary**, not a sampled few, because a crash does not sample. 72 truncations: 69 opened, 3 refused, 36 chunks served, **0 wrong**. The property is not "a truncated region opens" but "never serves something plausible and incorrect".
- **COMPACTION EQUALS A FRESH COOK, byte for byte** (72 sectors → 71). The format's only determinism guarantee once it mutates: two files that reached the same content by different write sequences differ, and compaction maps them onto the canonical form. Done through a temp file and a rename, so a crash during the one operation that touches every byte leaves the original intact.
- Freed sectors are **reused rather than abandoned**; removing a chunk twice fails rather than double-freeing; the two header copies genuinely differ after a write.

All 39 suites pass. Purely additive; gates unaffected.

*The directory-watcher clause is not yet applicable.* `Win32DirectoryWatcher` does watch `FILE_NOTIFY_CHANGE_LAST_WRITE` and would feed the engine's own region writes back into asset reload, but **nothing in either host writes a region yet** — the streamer is not wired into `SandboxApp` or `GameApp`. The guard belongs with that wiring.

**Slice 8 — generation as you go. DONE.** `GeneratedChunkSource`, `LayeredChunkSource`, `persistChunk`. **First caller and first test of `pcg::sampleInfinite`.**

**CPU, and not for performance.** GPU and CPU agree to one ULP, not bit-exactly. One ULP is invisible in a cloud and fatal here: a density landing either side of the threshold places an entity in one process and not the other. `coverageBias` is pinned to 1 and `coverageFloor` to 0, making `sampleInfinite`'s final `std::pow` the identity: the only operation in it whose result IEEE 754 does not fix across libm implementations, so it is removed rather than hoped about.

*Evidence:* `ChunkGenerateTest` — 32 assertions.
- **CROSS-PROCESS.** The test re-executes itself; the child generates the same chunks and cooks a region, and the parent reads that region back. Same digest in both processes, every chunk decodes identically, and the surface comes back as its **name**.
- **An unmodified chunk persists to nothing** (`Unchanged`, save file stays empty); a modified one is `Stored`; reverting the edit gives `OverrideRemoved`, so a save does not grow monotonically as a player edits and un-edits.
- Different seeds give different worlds; positions are chunk-local; off-surface chunks are empty so `has()` is instant.

*Two defects the tests caught:*
- **The generator produced solid-or-empty chunks** at `featureSizeCm = 6400` against a 1600 cm chunk: every candidate falls in one noise cell, so all 25 hashed identically (75 entities in exactly 3 of 6 chunks, 25 each) and every other assertion passed anyway. Fixed to 1600; the test now requires at least one **partially filled** chunk (now 4 of 6 non-empty, 3 partial, 89 entities).
- **The self-spawning test hung** — forward-slash path `./build/...` will not run on `cmd.exe`, and the marker was command-line argv. A test that launches itself is one quoting mistake from a fork bomb. The marker is an **inherited environment variable** now, so a process that has it can never spawn another.

All 40 suites pass. Purely additive within `modules/world` and `tests/world`; gates unaffected.

*Not done here:* F# rules driving generation. `PcgApply` and every F# rule run only inside `Sandbox.exe`; `GameApp` does not reference the script host. The native generator above is that default; F# refining it is slice 5 of the control layer.

**Slice 9 — actor-driven residency. DONE.** `StreamSource` (position + velocity), predictive lead along velocity, `clampToResident` (boundary hold). The camera is just one source; an actor is the same shape.

**A corridor, not two islands.** Anchoring on the position and the lead *point* breaks the moment the lead exceeds twice the load radius: the two cubes stop overlapping and the mover flies through unloaded space *between* them. Anchors now step along the segment at one radius each, so consecutive cubes overlap by half however fast the source moves, bounded at 64 steps so an absurd velocity degrades the lead rather than stalling the frame; a mover already outside is left where it is rather than teleported.

**The hold, and why it is the only acceptable answer.** When the loader has not kept up the three options are not equal: synchronous load is unbounded frame hitch; letting the mover through is a fall through the world. Holding at the last loaded chunk is the only recoverable, visible outcome. It backs off by a margin so the mover does not rest exactly on the face and re-trigger every frame — the same reasoning as eviction hysteresis.

*Evidence:* `ChunkActorStreamTest` — 17 assertions, with a **control**.
- At **600 m/s over 600 frames: 0 arrivals in unloaded space.**
- The same flight with the lead and the budget removed: **375 arrivals**. Without that control, "never arrives unloaded" could just mean the radius was generous and the prediction untested: at the 120 m/s the test first used, a budget of 1 kept up unaided and the lead did nothing.
- The hold is stable; a move inside loaded space is returned unchanged; a stranded mover is left alone.
- Three actors in separate neighbourhoods each get the ground under them *and* the space ahead.

*One number worth keeping:* **warm-up took 54 updates** before the wanted set was satisfied. Nothing is resident on frame zero, so a budget of 8 cannot fill a neighbourhood before a 600 m/s mover has left it (the first run measured exactly 2 arrivals in unloaded space, both in the opening frames). Spawning a player and moving at full speed in the same frame is not a case the streamer can serve, so the test warms up as a game does at level load rather than loosening the steady-state assertion.

All 41 suites pass. Purely additive within `modules/world` and `tests/world`; gates unaffected.

*Still deliberately excluded (§8.3):* actor **despawn** on leaving all loaded range. B7's 127-lives slot budget makes destroy/respawn churn a session-length leak.

**Slice 10 — floating origin, X/Y only.** Region-anchored, with `prevViewProj_` compensation and the `PerFrameCB` layout change.

*Done when:* a flight of 100 km holds sub-millimetre precision; the sky is bit-identical across a rebase; RT history shows no reprojection glitch on the rebase frame.

**Deferred and named:** GI clipmap (the volume is a fixed 24 m box at the origin — `SandboxApp.cpp:5400`; `GameApp` never calls `setVolume`); actor despawn/respawn pending B7; `control.fs` policy refinement; compression; curved horizon and ground.

---

## 11. What not to do

1. **Do not start before slice 0.** Two load paths means every slice is written twice, and they are already diverging.
2. **Do not stream anything in before `destroyMesh` exists.** B6 turns streaming into a leak.
3. **Do not stream actors in slice 1.** B7 makes it degrade over a session in a way no short test catches.
4. **Do not force the region format into AVR1.** It is a good immutable container and a bad mutable one.
5. **Do not serialise a material token, a string index, a `CName` offset, a component id or a field id.** Five process-local values, all load exactly once (§4.1).
6. **Do not add a record to `.ocworld` and assume it survives.** The parser skips unknown records (`OcWorld.hpp:90-91`); the editor's save rebuilds the file from `OcWorldData` — a new record parses cleanly on an old binary and is **silently dropped on the next save**. `PCGVOLUME` already hit this and is worked around by stashing it verbatim at load (`SandboxApp.cpp:5478, :5683`).
7. **Do not rebase Z.** The atmosphere reads world Z as altitude (§3.4).
8. **Do not touch the scene or the framework from a worker thread.** Process-global, not thread-safe, no locking anywhere (§7).
9. **Do not assume "we already have zlib".** The compressing half is not linked into any engine module.
10. **Do not build origin rebasing before slice 10.** Region 0 alone is 16 km of sub-millimetre space; nothing authored here needs more yet.
