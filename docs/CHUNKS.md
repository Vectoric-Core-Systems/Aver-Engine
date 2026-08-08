# Chunked worlds, region streaming, and generation as you go

> **STATUS: PLAN ONLY. No code has been written.**
>
> This is revision 2, dated 2026-08-08. It **supersedes** the 2026-08-02 revision, which is in git
> history at `0fcfcaa`. Revision 1 was written from reader evidence after its design agents died;
> this revision re-derived every load-bearing claim from the code with a second fan-out plus an
> adversarial fact-check over every `file:line` the readers cited. Nothing was carried over from
> revision 1 on trust — where the two disagree, this document says so explicitly.
>
> Three of revision 1's settled decisions are **overturned** here, and it missed four blockers, one
> of which is fatal to the feature as it scoped it. See §2.

---

## 1. What changed from revision 1, and why

| Revision 1 said | Revision 2 says | Because |
|---|---|---|
| Region is **16³ chunks** (256 m) | Region is **1024³ chunks**, chunk coords `-512..+511` | The user's coordinate hierarchy. 16³ survives — as the *group*, one level down (§5). |
| `.region` is an **AVR1 container** | `.region` is **its own mutable container**; AVR1 stays for immutable assets | AVR1 pins `DirOffset` to 64 with payloads immediately after (`Avr1.cpp:162-175`), so growing the directory displaces every payload. And `parseAvr1` copies *and* xxHash64-verifies **every** payload on open (`Avr1.cpp:280-282`). Both are disqualifying for an archive you seek into and mutate. |
| `.region` is **cooked, read-only** | `.region` is **runtime-writable**, sector-allocated | The world generates as the player moves. Read-only was revision 1's way of dodging AVR1's constraints; dropping AVR1 removes the reason. |
| Level is `FLAT` **or** `CHUNKED` | Coordinate hierarchy is **universal**; *residency* is opt-in | Two load paths means the non-default one rots. And flat is the **oracle** for testing chunked (§10). |
| "Do not build origin rebasing" | Still true for now — but rebasing now has a **known anchor** (the region) and a **hard constraint** (X/Y only, never Z) | §4. |

Revision 1's blockers B1 (no partial read), B2 (AVR1 exact `FileSize`), B3 (no compression), B4 (no
job system) and B5 (CLR is Win32-only and declines) all **stand, verified**. B2's specifics are worse
than stated: `FileSize` sits *inside* the CRC32C range covering bytes `0x00..0x3B`
(`Avr1.cpp:212-213, :244`), so any growth rewrites the header *and* recomputes its CRC.

---

## 2. Four blockers revision 1 missed

### B6 — There is no `IDevice::destroyMesh`. *(fatal as revision 1 was scoped)*

`modules/rhi/include/aver/rhi/RHI.hpp:281-284` declares `createMesh` and nothing that releases one.
**Every mesh ever created lives until the device is destroyed.**

Streaming in without streaming out is not streaming; it is a slow memory leak with a camera attached.
This is not a tuning problem, it is a missing interface, and the tree already shows two subsystems
that hit it and gave up:

- `LandscapeRenderer` capped its mesh cache instead of evicting (`LandscapeRenderer.cpp:96-102`), and
  `forgetAll()` clears the handle map **without freeing anything** (`LandscapeRenderer.hpp:45`) — one
  vertex and one index buffer leaked per node, permanently.
- `SkinnedScene` recycles rather than frees, explicitly because there is no `destroyMesh`
  (`SkinnedScene.hpp:94-97`), and silently degrades past 256 resident entities (`SkinnedScene.cpp:13, :63`).

Worse for a streaming world: mesh geometry lives in **UPLOAD-heap** memory permanently
(`D3D12Device.cpp:2177-2187`), i.e. CPU-visible write-combined, read over PCIe every frame. Thousands
of chunk meshes there is the wrong steady state independently of the leak.

### B7 — Entity slots are exhaustible and are never recycled. *(threatens generate-as-you-go)*

An entity is 24 index bits + 7 generation bits (`Entity.hpp:21-25`). A slot serves generations 1..127
and is then **retired forever** (`World.cpp:246-252`; `tests/scene/src/SceneTest.cpp:130-131` pins
this). Generation does not wrap.

A world that spawns and despawns as the player moves burns slots permanently. Two side tables make it
worse, because both are high-water-mark structures that never shrink:

- `classOwners()` is a dense `std::vector` indexed by entity index (`FrameworkAbi.cpp:179`) — sized
  by the highest index ever used, not by the live count.
- `CName` offsets point into `World::Impl::nameBlob`, an append-only `std::string` that is **never
  compacted** (`World.cpp:95, :386-396`). Every `create()` and every `setName()` appends; `retireSlot`
  never touches it.

So a long procedural session grows memory monotonically even at constant live-entity count.
**Design consequence in §9: do not destroy and respawn actors on chunk transitions.**

### B8 — The level writer already loses precision past ~10 km, before f32 is involved.

`OcWorld.cpp:22-27` formats every number with `%.6g` — six significant digits. A position of
`1234567` cm is written `1.23457e+06` and reads back as `1234570`. **A 12.3 km coordinate moves 3 cm
every time the editor saves.**

This is the strongest independent argument for the hierarchy that exists in the tree: with
`(region, chunk, local)`, the number that gets text-formatted is a local offset of at most four
digits, and `%.6g` is exact for it. The hierarchy fixes a live bug, not just a hypothetical one.

The `f64` in `OcWorldData` (`OcWorld.hpp:19`) is decorative — every consumer narrows to f32 at the
point of use (`GameLevel.cpp:47-50`, `SandboxApp.cpp:5511-5514`). Widening the format alone changes
nothing observable.

### B9 — There are two diverging copies of the level load path.

`modules/runtime.game/src/GameLevel.cpp:37-175` (game) and `sandbox/src/SandboxApp.cpp:5464-5561`
(editor) are independent implementations of the same thing, and they already disagree: the editor
applies `SUN`/`SKY` at load (`:5551`) and drives the cloud layer from a `PCGVOLUME` named `"Sky"`
(`:5497-5506`); the game applies only `FOG` and defers sky to `GameApp` (`GameLevel.cpp:87-95`). Each
narrows f64→f32 in its own near-identical loop.

**Every slice below would have to be written twice.** Unifying them is slice 0 for that reason alone.

---

## 3. Decisions

### 3.1 The coordinate hierarchy is universal; residency is opt-in

Revision 1's `FLAT` vs `CHUNKED` level flag is the right thing to *show an author* and the wrong thing
to build, because it implies two load paths and the non-default one rots.

These are two independent axes:

| | storage | residency |
|---|---|---|
| today | text `.ocworld` | everything, forever |
| streamed | binary region files | by distance, generated on demand |

- **The hierarchy is universal.** `regionOf(pos)` / `chunkOf(pos)` are pure functions with no storage
  cost. A flat level lives in region `(0,0,0)`, occupies a handful of chunks, and never notices.
  One answer in the codebase to "which chunk owns this entity".
- **Residency is opt-in.** Flat = the residency set is "all", computed once, never revisited. Same
  materialisation code, one branch higher up.

**Flat is the oracle.** The strongest test in this plan is "load chunked with everything resident,
load flat, assert identical entity sets and transforms". That test only exists while flat exists.
Delete flat and you are left comparing streaming against itself. This is a testing argument, not a
compatibility one, and it is why flat stays permanently rather than until migration finishes.

Secondary: the gate baseline is 18 probes × 9 configurations, bit-exact, keyed to existing levels.
Migrating them all means re-recording at exactly the moment an unchanged reference is most valuable.

### 3.2 Module split

Three tiers, following the tree's existing `option(AVER_MODULE_*)` convention with the
force-off-and-explain pattern at `CMakeLists.txt:134-136` and `:152-154`:

1. **Always compiled — `modules/world`.** The coordinate types and mapping. This module is
   **README-only today and absent from the build** (`modules/world/README.md:9-11`), so it is
   greenfield, and its README already claims streaming as its scope. Roughly 200 lines, no I/O.
   *Not* optional: it is a value type like `Vec3`, and behind a flag every caller needs an `#if`.
2. **Always compiled — `modules/platform`.** Handle-based ranged file I/O. Not streaming-specific;
   any large-asset loader wants it.
3. **`AVER_MODULE_STREAM`, optional, default OFF until proven.** Region format, sector allocator,
   residency set, generation scheduler, eviction. `MCP` (`CMakeLists.txt:56`) is the precedent for
   default-OFF.

`STREAM` hard-depends on `SCENE` → force OFF with a message, as `FRAMEWORK` already does.
`PHYSICS` and `FRAMEWORK` are **soft**: they register as participants if present, so the protocol
must tolerate their absence.

**Rule: with the module compiled out, a level declaring itself streamed must refuse to load with a
named error.** Never load empty. Revision 1 worries about this for F# declining at runtime; a compile
flag is worse, because there is no runtime signal at all.

### 3.3 Naming: `chunk` is already taken

`AVER_MODULE_LANDSCAPE` is *"chunked heightfield terrain, LOD and chunk geometry"*
(`CMakeLists.txt:50`), and inside it "chunk" means a quadtree node's fixed-vertex mesh tile
(`ChunkMesh.hpp:17-23`, 4485 vertices per node regardless of level). That is a different concept at a
different scale.

Namespace rather than rename: `world::ChunkCoord` vs the existing `landscape` chunk. Renaming
existing code is churn for no behaviour, and landscape chunks will eventually want to be a
*participant* in a streaming chunk — which is a reason to keep the names distinct, not merge them.

---

## 4. Coordinates

### 4.1 The hierarchy

```
global chunk coord   gc  : i32   = floorDiv(worldCm, chunkSizeCm)
region coord         r   : i32   = (gc + 512) >> 10          // arithmetic shift: floors correctly
chunk within region  cl  : i16   = ((gc + 512) & 1023) - 512 // in [-512, +511]
local offset         lo  : f32   = worldCm - gc * chunkSizeCm, in [0, chunkSizeCm)
```

Region `r` covers `gc ∈ [1024r - 512, 1024r + 511]`, so **region 0 is centred on the world origin**.
That is what "+512 to −512" buys: the origin sits in the middle of its region, not on a seam, and an
existing flat level at the origin is nowhere near a boundary.

Local offset is `[0, chunkSizeCm)` — deliberately unsigned — so the truncate-vs-floor bug class exists
in exactly one function (`floorDiv`) instead of at every call site. The tree already has this bug:
the bounded PCG sampler truncates toward zero while the infinite one floors (`PcgVolume.cpp:50-52` vs
`:78-80`), and `PcgShaders.hpp:64-66` admits it becomes wrong "the moment a volume is centred on the
origin".

### 4.2 Bit budget — exact, not approximate

```
cl ∈ [-512, +511]      = 1024 values = exactly 10 bits signed, per axis
3 axes                 = 30 bits, fits u32 with 2 spare
  group  = top 6 bits/axis  -> 64³  = 262 144 groups   (18 bits)
  slot   = low 4 bits/axis  -> 16³  = 4 096 slots      (12 bits)
                                       18 + 12 = 30 ✓
```

The inner 16³ group is revision 1's region, unchanged. That work is not wasted; it moved down a level.

### 4.3 Precision — the actual reason for the hierarchy

Derived independently, not quoted from revision 1:

| max \|coord\| | f32 ULP at 1 cm/unit |
|---|---|
| 5.24 km | 0.625 mm |
| **8.19 km** (512 × 16 m) | **0.625 mm** |
| 10.486 km (2²⁰ cm) | 1.25 mm — the cliff |
| 16.38 km | 1.25 mm |
| 100 km | 10 mm |

f32 holds sub-millimetre out to `2²⁰ cm = 10.486 km` and no further. So:

> **Constraint: `512 × chunkSizeCm < 2²⁰ cm`, i.e. `chunkSizeCm ≤ 2047`.**
> 16 m (1600 cm) is the largest round size that qualifies; 20.48 m is the first that fails.

**Default: `chunkSizeCm = 1600` (16 m), confirmed 2026-08-08.** It is per-level and the `.ocindex`
stores it (§6), so a level may choose otherwise within the constraint above. `OcWorld.hpp:43` already
defaults `OcPcgVolume::cellSizeCm` to 1600 "one 16 m chunk per cell", so the two agree by default
rather than by coincidence.

The cost of 1600 over a power-of-two-centimetre size (1024 cm would be the largest that qualifies) is
that world→chunk is a real integer division rather than an arithmetic shift, so `floorDiv` has to
handle negatives correctly instead of getting it free from `>>`. That is one function with one test,
against an authoring size nobody has to think about.

Inside a chunk, precision is ~1.2 µm. **That is the payoff**: stored positions are uniformly
micron-accurate at any distance from the origin, unbounded — whereas absolute f32 degrades linearly
and `%.6g` (B8) degrades faster still.

### 4.4 Rebasing: not yet, but the anchor and the constraint are now known

The runtime is absolute f32 end to end — `Vec3`/`Transform` (`Math.hpp:32-33, :246-249`), `CLocal.xf`
(`Components.hpp:23-26`), `float*` across both C ABIs (`scene_abi.h:5-7`, `framework_abi.h:111-112`),
and Jolt built single-precision by an explicit documented decision (`physics.jolt/CMakeLists.txt:47-55`).
There is **no** origin-shift, floating-origin or camera-relative code anywhere — verified by a
case-insensitive sweep over nine directories that returned zero hits.

Revision 1's advice stands: **build the check, defer the machinery.** But two things are now settled
that were not:

1. **The anchor is the region.** When rebasing does land, the float origin snaps to a region origin.
   Rebases then happen at fixed, known, testable points that are already in the file format, rather
   than at an arbitrary hysteresis radius.
2. **Rebase X and Y only. Never Z.** `averAtmoCamAlt()` (`RHIShaders.cpp:327`) treats `gCamPos.z * 1e-5`
   as altitude in km above a 6360 km planet. Shift Z and the atmosphere, the aerial-perspective
   integral (`:332-341`) and the CPU-baked fog reference (`D3D12Device.cpp:2624`) all silently compute
   the wrong altitude. `RHIShaders.cpp:320-326` records that Z crossing zero already broke the
   ray-sphere intersection once. Worlds here are surface-based; vertical extent does not need rebasing.

Two further costs to budget when it happens, both verified:

- `prevViewProj_` (`VoxiRenderer.cpp:1019, :1035`) reprojects the RT shadow and reflection histories.
  After a shift it describes the *old* origin. Fixable exactly — compose the shift translation into it
  — but it must be designed in, not discovered.
- `PerFrameCB` is hand-mirrored field-for-field across `D3D12Device.cpp:516-540`, `RHIShaders.cpp:99-136`
  and every consumer's own copy, with nothing checking the layout (`RHIShaders.cpp:120-123` warns about
  exactly this). Adding an origin field touches all of them in lockstep.

Also already true and worth knowing: **cloud noise degrades with distance from the origin today**.
`D3D12Device.cpp:304` feeds absolute `wpos * (1/featureSize)` into a `floor`/`frac` lattice at
`:280-281`; far out, `frac()` loses mantissa bits. That is a live symptom of the absolute scheme.

---

## 5. The region file

**Not AVR1.** AVR1 stays what it is — an excellent immutable asset container. A region file is a
mutable, seekable, sector-allocated archive, which is a different thing, and forcing one into the
other buys nothing (see §1).

Suffix `.avrgn`. Anvil's structure, one level deeper, because 1024³ = 1 073 741 824 slots makes
Anvil's single dense table impossible:

```
sector 0,1   HEADER x2      magic 'ARGN', version, chunkSizeCm, regionCoord[3], levelId,
                            worldSeed, generatorVersion, groupTableSector, groupCount,
                            sectorCount, freeListSector, serial, crc32c
                            -- TWO copies with a monotonic `serial`; reader takes the higher
                               one that passes CRC. This is the crash safety Anvil lacks.
             GROUP TABLE    sparse, sorted by groupIndex, only occupied groups:
                              u32 groupIndex (18 bits used) | u32 groupDirSector
             GROUP DIR      dense, 4096 slots, one per group:
                              u32 firstSector | u32 byteLength     -> 32 KiB exactly
             FREE LIST      sorted extents { u32 firstSector, u32 sectorCount }
             PAYLOADS       4 KiB sectors
```

**Why two levels, in numbers.** For a fully explored surface region (1024×1024 columns × ~8 vertical):

| layout | resident cost |
|---|---|
| flat dense table | 8.00 GiB — impossible |
| flat sparse directory, 16 B/entry | 128 MiB — too big to hold |
| **sparse 64³ groups → dense 16³ dirs** | **~128 KiB resident + 32 KiB on demand** |

Steady-state lookup is two cached reads plus one ranged read.

**Writing.** Allocate from the free list → write payload → flush → patch the group dir entry (8 bytes)
→ flush → write the header with `serial + 1`. A crash at any point leaves the previous `serial` valid
and loses at most the one chunk in flight.

**Determinism, honestly scoped.** Revision 1 claimed writer determinism unconditionally. That cannot
hold for a file mutated at runtime, because sector allocation depends on history. So:
- a **cooked** region (written fresh) must be byte-identical across runs — `memcmp` two cooks;
- a **compacted** region is the canonical form, and compaction of any region must equal a fresh cook.

That is a stronger test than revision 1's, and it is still baseline-free.

**Uncompressed by design.** B3 stands — the only DEFLATE in the tree is stb's PNG-internal zlib, whose
compressing half is not linked into any engine module. Density comes from encoding: u16-quantised
chunk-local positions, a palette plus bit-packed indices, sorted-delta ids. `.ocland` already
quantises, so there is an in-tree precedent.

### 5.1 What a chunk payload may **never** contain

This list is longer than revision 1's and every entry is verified. Each is an id-*shaped*,
process-local value: written to disk, it produces a file that loads correctly exactly once, in the
process that wrote it.

| Never serialise | Why | Store instead |
|---|---|---|
| `CMeshRenderer::material` | `static_cast<int32_t>(table.size()) + 1` from a DLL-local map (`SceneAbi.cpp:377-390`, table at `:64-67`). Its value is a pure function of the order names were first seen **in this process**; `GameContent::registerBuiltins` interns seven at startup (`GameContent.cpp:109-117`) and `GameLevel::load` interns more per placement (`GameLevel.cpp:61`). | the material **name** |
| any `FieldKind::String` field | eight bytes indexing a DLL-local `std::vector<std::string>` (`SceneAbi.cpp:70-73`, minted `:262`). The framework already treats this as radioactive and replays strings at spawn (`FrameworkAbi.cpp:457-468, :510`). | the string |
| `CName::offset` / `len` | offsets into the never-compacted `nameBlob` (`World.cpp:95, :386-396`) | the name |
| **component ids and field ids** | registration-order counters (`World.cpp:453, :523`). `Builtins.cpp:94-96` spells out the hazard: inserting a component anywhere but the end shifts every later id. | a FourCC per component, resolved on load |
| `World::at()` indices | dense over live entities and **shift on every flush** (swap-and-pop, `World.cpp:234-243`) | never a stable identity |

Asset references are safe: FNV-1a-64 over a forward-slash `Content/`-relative path, and those ids
survive packaging unchanged.

**And a trap on the way in:** `World::addComponent` hands back **zero-filled** bytes
(`ComponentPool.cpp:63`), so default member initialisers never run. This has already caused the
`CMeshRenderer` visible-bit bug once, which is why `CAnimator`'s flags are negated
(`Components.hpp:98-114`). A chunk loader that attaches components without memcpy'ing defaults
produces invisible, non-animating entities.

---

## 6. The index

`<level>.ocindex`, sorted by region Morton code — which is both the ordering the user asked for and
what makes it binary-searchable.

```
levelId  u64
chunkSizeCm u32        -- the authority; one size per level, enforced here
worldSeed u64
generatorVersion u32   -- persisted regions were produced by SOME generator; a mismatch must be
                          detectable rather than silently mixing two worlds
per region (Morton-sorted):
  i32  regionCoord[3]
  u64  contentHash     -- must equal the region header's own
  u32  chunkCount
  str  relativePath
bounds: i32 minRegion[3], maxRegion[3]
```

Four jobs: existence without opening files; staleness detection; the chunk size lives here once; and
generator-version skew. Must be rebuildable by scanning region files, so a corrupt index is
recoverable rather than fatal.

---

## 7. Generation as you go

**Store deltas, not baselines.** A chunk's content is `f(worldSeed, chunkCoord)` unless a persisted
override exists. Then regions stay near-empty until something is modified, eviction of an unmodified
chunk is free (regenerate rather than save), and the save file is proportional to what the player
actually changed.

Four verified constraints shape this:

1. **Generation must be CPU-side for anything authoritative.** GPU-vs-CPU agreement is **one ULP, not
   bit-exact**, because DXC fuses multiply-add where MSVC under `/fp:precise` does not
   (`GameApp.cpp:256-265`, `PcgShaders.hpp:8-12`). Use the GPU volume for *visual* fields (clouds);
   use the CPU path for *placement*. A scheme that hashes generated chunks cannot compare the two
   bitwise.
2. **`pcg::sampleInfinite` has zero callers and is untested.** `tests/pcg` exercises only the bounded
   `sampleDensity` and the `PCGVOLUME` round trip. The "infinite" spec is built at level load and then
   never evaluated. Building on it means being its first caller — so it needs a test first.
3. **The GPU volume path cannot be a per-chunk generator as it stands.** Readback is 3 frames by
   contract (`PcgVolume.hpp:107-113`), `VolumeBuilder` is **single-slot** — `request()` overwrites
   `spec_` and resets state (`PcgVolume.cpp:177-180`) — and it reallocates both GPU buffers whenever
   the byte size changes (`:145-175`).
4. **F# and `PcgApply` only run inside `Sandbox.exe`** (`SandboxApp.cpp:5406`). `GameApp` does not
   reference the script host. A shipped game that generates as it moves needs that wiring — and by
   B5, generation must still work with the host declining.

Determinism itself is real and good: splitmix32 is mirrored bit-for-bit across F#, C++ and HLSL, with
a per-voxel GPU-vs-CPU check under `--pcg-volume-test`.

---

## 8. Threading

Revision 1's B4 stands, with detail. Six `std::thread` sites exist (WASAPI, MCP, the directory
watcher, IDE scan, two ToolsMenu jobs); none is a pool. Jolt's `JobSystemThreadPool` is linked
**PRIVATE** inside `Aver.Physics.dll` by explicit design (`modules/physics/CMakeLists.txt:6-15`), so
reusing it means creating an exported seam the CMake comment deliberately foreclosed.

The hard constraints:

- **`scene::World` is a process-global, non-thread-safe singleton** (`World.hpp:15-19`). So is every
  framework side table (`FrameworkAbi.cpp:137-232`). **A background thread may not touch spawn,
  destroy, or the scene at all.**
- **The RHI declares no thread-safety contract anywhere** — whether `createMesh` may be called off the
  frame thread is currently *undefined*, not allowed or forbidden. That has to be decided and written
  down before any background upload.
- There is **one DIRECT queue** (`D3D12Device.cpp:1528-1530`); no copy queue, no async compute.
- `kFrameCount` is **2** (`D3D12Device.cpp:32`) — no 3-deep pipelining to hide latency.
- `waitForGpu()` is a full drain reachable from ordinary-looking calls, including
  `reconcileClearValue` at the top of *every* `beginFrame` (`D3D12Device.cpp:2301, :2131`).
- Every texture upload creates a fresh fence, allocator, command list and Win32 event and blocks
  INFINITE (`D3D12Device.cpp:3819-3853`).

**Therefore: the worker produces plain POD; the frame thread materialises.** A worker does file I/O
and generation and hands back a flat placement list (asset id, transform, material *name*, flags). The
frame thread turns that into entities under a per-frame budget. One worker to start, measured — and
sized in awareness that Jolt already takes `hardware_concurrency() - 2` (`PhysicsWorld.cpp:267-269`).

Copy the drain contract from `Win32DirectoryWatcher` (`:104-114, :128-133, :232-241`): push under a
mutex, move-append, clear, explicit overflow flag. It is the one proven cross-thread queue in the tree.

---

## 9. Actors and residency

Three distinct problems live under "actors leaving loaded range". Separating them matters, because
one of them is dangerous.

### 9.1 An actor approaches an unloaded chunk

Predictive request along velocity, N chunks ahead, where N covers worst-case load latency at current
speed. Every actor tagged as a streaming source contributes to the residency set; the camera is just
one source, not a special case.

If the target chunk is still not resident when the actor arrives: **hold the actor at the boundary**.
The alternatives are a synchronous load (a frame hitch of unbounded size) or letting it through (a
fall through the world). A soft wall is recoverable and visible in a test.

### 9.2 An actor leaves all loaded range

The actor is owned by its chunk. When the chunk evicts, the actor serialises into the chunk's delta
and despawns; when the chunk reloads, it comes back.

Cost to budget: this needs a lifecycle hook that does not exist. The dispatch table is exactly ten
entries (`ManagedDispatch.cs:14-29`, mirrored in `framework_hooks.h`) and none concerns spatial
residency. Adding one bumps `AVER_FW_DISPATCH_VERSION` and breaks the managed binding until it is
regenerated.

There is a ready-made near-miss: `AVER_FW_END_TRAVEL` (`framework_hooks.h:31`) — *"level travel; the
current level's actors are torn down"* — is **defined and dispatched by nothing**. Worth reusing
rather than inventing a parallel concept.

### 9.3 Slot churn — the one that is dangerous

By **B7**, every serialise/despawn/respawn cycle burns a generation, and a slot dies after 127 lives,
permanently. An actor that crosses a chunk boundary back and forth 127 times retires its slot forever.
Meanwhile `classOwners()` and `nameBlob` grow to the high-water mark and never shrink.

> **So slice 1 does not stream actors at all.** Placements (the many) stream; actors (the few) stay
> resident, with an explicit cap and a logged warning when it is approached. §9.2 is deferred until
> either a suspend-without-destroy state or generation recycling exists.

This is a real narrowing of scope and it is deliberate: the alternative is a feature that works in a
ten-minute test and degrades over a long session in a way no test would catch.

### 9.4 Other verified traps on the unload path

- **`aver_fw_end_play` destroys every entity whose class handle is non-zero** (`FrameworkAbi.cpp:796-802`),
  including anything a streaming system spawned and was tracking itself.
- **`World::destroy` is deferred and takes the whole subtree** (`World.cpp:256-280`, `:346-354`).
  Between the call and the next flush the entity is still live and still returned by `aver_scene_at`;
  only `destroyPending(e)` distinguishes it. `GameRender.cpp:51` tests it; `Game.AllActors`
  (`Game.cs:56-64`) does not. Chunk churn exercises this every frame.
- **Side tables are cleaned by different owners at different times**, and several assume the
  framework's destroy path: `pawnByController`/`controllerByPawn` only in `destroyActor`
  (`FrameworkAbi.cpp:556-564`), `AnimSystem::posed_` only in tick, `SkinnedScene::live_` only in
  update. A chunk unloader calling `scene::World::destroy` directly — which both existing unload paths
  do (`GameLevel.cpp:197`, `SandboxApp.cpp:2503`) — leaves them stale.
- **`SandboxApp::destroyEntity` cleans only the entity it was given** (`:2500-2513`) while
  `World::destroy` retires the subtree — children's labels and static bodies leak. A live bug today; a
  constant one under streaming.
- **There is no entity→physics-body link at all.** `aver_phys_add_dynamic_box` returns an opaque int
  and takes no entity (`physics_abi.h:46-58`); `GameLevel` keeps a parallel `std::vector<int32_t>`
  with no association (`GameLevel.hpp:74`); the editor keeps a separate map (`SandboxApp.cpp:5271`).
  **Streaming a chunk out cannot find the bodies belonging to its entities.** This must be fixed in
  the participation protocol, not worked around.
- **Physics runs in metres while the engine runs in centimetres** (`cmToM`, `PhysicsWorld.cpp:366, 454, 623`),
  and `Convert.hpp:19` divides in f32 — a second, independent precision ceiling.

---

## 10. Verification

Baseline-free oracles first, as this repo prefers.

**No baseline needed:**

- **Chunked equals flat** — everything resident, both paths, identical entity sets and transforms.
  The strongest single test; needs no renderer. This is why flat stays (§3.1).
- **Cook determinism** — two cooks, `memcmp`. And **compaction equals a fresh cook**, which extends
  the property to runtime-mutated files (§5).
- **Round trip across a process boundary** — save, exit, reload, compare. It **must** fork or restart:
  the five process-local values in §5.1 make a same-process round trip pass while the file is
  unloadable anywhere else. This is the single easiest test to get wrong.
- **Boundary conservation** — total entities across chunks equals the flat count, with entities placed
  deliberately *on* boundaries and at negative coordinates, where floor-vs-truncate bugs live. The
  tree already has one such bug (`PcgVolume.cpp:50-52` vs `:78-80`).
- **Crash safety** — truncate a region mid-write at each sector boundary; assert the previous `serial`
  loads and exactly the in-flight chunk is lost.
- **Precision guard** — assert no authored entity lies outside `2²⁰ cm`, or warn with the measured ULP.
- **Slot budget** — run a long synthetic streaming session; assert retired-index count and `nameBlob`
  growth stay bounded. This is the B7 regression test and the one most likely to be skipped.
- **Streaming with F# absent** — force `ScriptHost` to decline; assert the world still streams.
- **Streaming with `AVER_MODULE_STREAM` off** — assert a streamed level refuses to load with a named
  error rather than loading empty.

**Needs a render gate re-recording:** nothing in slices 0–7, if they are done correctly — no slice
before floating origin should move a pixel in an existing flat level, and that is itself the test.

---

## 11. Slices

Each states what proves it done. Ordering is by dependency, not by value.

**Slice 0 — unify the two level load paths. DONE 2026-08-08.** The shared loop is
`aver::world::instantiate` in the new `modules/world`, which both hosts link; the rotation contract is
`world::LevelTransform.hpp`, which the editor re-exports. `saveLevel` now starts from the parsed
header rather than a default-constructed one, so `SPAWN`, `BUILD`, `ID` and the sun's `lux` ride
through, and it writes the surface back as a NAME via the new `aver_scene_material_name`
(scene ABI 1.0 → 1.1, additive).

*Evidence:* `LevelInstanceTest` — 111 assertions, new, and the first coverage the load path has ever
had. All 32 headless suites pass. **Gates were run before and after and are byte-for-byte identical**
— all 18 probes, same raw RGB, same verdict — so the 17 failures are entirely the pre-existing
atmosphere drift from `94b6736` and this slice moves nothing. `AVER_BUILD_GAME=OFF` builds clean,
which is the claim that the editor does not depend on the game runtime.

*Known-unfixed and pre-existing:* `AVER_MODULE_SCENE=OFF` does not build, at HEAD and after, with an
identical error set in `GameApp.cpp`, `GameContent.cpp`, `SkinSceneTest.cpp` and `SandboxApp.cpp` —
none of them level-loading code. `Aver.World` itself compiles cleanly in that configuration.

**Slice 1 — the coordinate hierarchy.** `modules/world` becomes real: `RegionCoord`, `ChunkCoord`,
`floorDiv`, packing, the `chunkSizeCm ≤ 2047` check. Pure, no I/O.
*Done when:* exhaustive round-trip over a coordinate sweep including negatives and exact boundaries;
the bit-packing identities in §4.2 hold by test, not by comment; the precision guard fires.

**Slice 2 — ranged file I/O in `modules/platform`.** Open/seek/read-at/write-at/flush/close, plus
delete and rename (neither exists today — region compaction and temp-then-swap have no primitive).
*Done when:* a known byte range reads out of a large file; a write-at does not truncate; a
crash-simulated partial write is detectable.

**Slice 3 — `IDevice::destroyMesh` (B6).** Independently valuable; also fixes
`LandscapeRenderer::forgetAll`'s permanent leak and lets `SkinnedScene` stop hoarding.
*Done when:* create/destroy in a loop holds steady GPU memory; the landscape cache evicts instead of
capping; gates unmoved.

**Slice 4 — the participation protocol.** The seam by which framework, physics and the mesh cache
contribute to and restore from a chunk. **Includes the entity→body link**, which does not exist (§9.4).
No files yet.
*Done when:* an in-memory partition/reassemble reproduces the flat entity set exactly, hierarchies
intact, oversize list correct, and every body is accounted for on unload.

**Slice 5 — `.avrgn` + `.ocindex`, cooked and read-only.** Format, cooker, reader.
*Done when:* cook determinism; cross-process round trip; chunked-equals-flat; boundary conservation; a
corrupt CRC is refused naming the chunk rather than crashing.

**Slice 6 — native streaming residency, camera only.** Residency set, radius policy, hysteresis, load
budget. No generation, no writes, no F#.
*Done when:* a camera flight across ≥9 regions holds a bounded resident set, never drops an entity,
and the per-region load cost is measured and written down.

**Slice 7 — runtime writes.** Sector allocator, free list, double-header crash safety, compaction.
*Done when:* the crash-safety test passes at every sector boundary; compaction equals a fresh cook;
the directory watcher does not feed the engine's own writes back into asset reload
(`Win32DirectoryWatcher.cpp:24-26` — it will, by default).

**Slice 8 — generation as you go.** Delta-from-baseline; CPU generation path; `sampleInfinite`'s first
test and first caller.
*Done when:* the same seed and coord generate identically across processes and platforms; an
unmodified chunk round-trips to *nothing on disk*; a modified one persists exactly its delta.

**Slice 9 — actor-driven residency.** Actors as streaming sources, predictive next-chunk load, the
boundary hold. **Not** actor despawn (§9.3).
*Done when:* an actor driving at maximum speed never reaches a non-resident chunk; when starved, it
holds at the boundary rather than falling.

**Slice 10 — floating origin, X/Y only.** Region-anchored, with `prevViewProj_` compensation and the
`PerFrameCB` layout change.
*Done when:* a flight of 100 km holds sub-millimetre precision; the sky is bit-identical across a
rebase; RT history shows no reprojection glitch on the rebase frame.

**Deferred and named:** GI clipmap (the volume is a fixed 24 m box at the origin —
`SandboxApp.cpp:5400`, and `GameApp` never calls `setVolume` at all, so a shipped game gets GI only
near the origin); actor despawn/respawn pending B7; `control.fs` policy refinement; compression;
curved horizon + curved ground.

---

## 12. What I would not do

1. **Do not start before slice 0.** Two load paths means every slice is written twice, and they are
   already diverging.
2. **Do not stream anything in before `destroyMesh` exists.** B6 turns streaming into a leak.
3. **Do not stream actors in slice 1.** B7 makes it degrade over a session in a way no short test
   catches.
4. **Do not force the region format into AVR1.** It is a good immutable container and a bad mutable
   one; the mismatch is structural, not a limitation to work around.
5. **Do not serialise a material token, a string index, a `CName` offset, a component id or a field
   id.** Five process-local values, all id-shaped, all load exactly once (§5.1).
6. **Do not add a record to `.ocworld` and assume it survives.** The parser skips unknown records
   (`OcWorld.hpp:90-91`) and the editor's save rebuilds the file from `OcWorldData` — a new record
   parses cleanly on an old binary and is **silently dropped on the next save**. `PCGVOLUME` already
   hit this and is worked around by stashing it verbatim at load (`SandboxApp.cpp:5478, :5683`).
7. **Do not rebase Z.** The atmosphere reads world Z as altitude (§4.4).
8. **Do not touch the scene or the framework from a worker thread.** Process-global, not thread-safe,
   no locking anywhere (§8).
9. **Do not assume "we already have zlib".** The compressing half is not linked into any engine module.
10. **Do not build origin rebasing before slice 10.** Region 0 alone is 16 km of sub-millimetre space;
    nothing authored here needs more yet.
