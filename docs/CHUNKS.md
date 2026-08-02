# Chunked worlds, `.region` streaming, and the planet/media split

> **STATUS: PLAN ONLY. No code has been written.** Produced 2026-08-02 from five parallel readers
> over the tree plus an adversarial verification pass. Every load-bearing claim carries a
> `file:line` an agent actually opened; the arithmetic was recomputed independently by the
> orchestrator. Re-check before relying on any single line.
>
> The design agents of that workflow died on a session limit; this document was written by the
> orchestrator from the surviving reader evidence, which is intact.

---

## 1. Decisions already taken

These are settled, not open questions. Everything below is built on them.

| Decision | Consequence |
|---|---|
| **Chunking is opt-in per level.** Normal levels stay flat, exactly as today. | Zero risk to existing content. A level is either `FLAT` or `CHUNKED`; there is no third state and no migration. |
| **Chunk size is variable per level but FIXED within a level.** | One size per level, declared once at bake time, immutable afterwards. Different levels may differ. No mixed-resolution grid, no LOD tree of chunk sizes. |
| **Planet layer is global. Media layer is per chunk.** | Rayleigh/Mie/ozone/transmittance belong to the world, never to a chunk. Clouds, fog volumes and local weather attach to chunks. |

**Why the fixed-size-per-level rule is the right call and not a simplification.** The world→chunk
mapping becomes a shift instead of a divide when the size is a power-of-two count of metres, the
`Chunk.index` key is a plain integer triple with no scale field, and — the real reason — a
neighbour query never has to reconcile two different grids. Mixed chunk sizes within one level
would make "which chunk owns this entity" ambiguous at every boundary between the two sizes.

**Recommended sizes:** powers of two in metres — 16 m (default), 32 m, 64 m, 128 m. Non-power-of-two
should be *allowed but warned about*, because it costs a divide per query and buys nothing.

---

## 2. What a chunk actually is in this engine

This is **not** a voxel engine. It is a generational-handle ECS with an intrusive hierarchy, so a
chunk is a **spatial partition of entities**, not a block of voxels.

Entities are a 32-bit generational handle (24 index / 7 generation / bit 31 clear) over ten
hand-registered sparse-set component pools, plus parent/child/sibling links in `CHierarchy`.

**A chunk cannot be serialised by dumping components, and this is the single most important
structural finding.** Three things an entity "owns" live *outside* `World`:

| Lives outside `World` | Where it actually lives |
|---|---|
| Its gameplay class | a framework-side vector keyed by entity index |
| Its physics body | a sandbox-side `unordered_map` |
| Its GPU mesh handle | a sandbox-side map from asset id to `rhi::MeshHandle` |

So chunk save/load needs a **participation protocol** — each subsystem contributes to and restores
from a chunk — not a memcpy of pools. Design that seam first; everything else depends on it.

### Two fields that must never be written to disk

- `CMeshRenderer::material` is a **sequential intern token handed out in first-call order within
  the process** (`SceneAbi.cpp:387`). It is not stable across runs.
- A `FieldKind::String` field stores an **index into a process-local append-only pool**.

Both are id-*shaped* and both are process-local. Writing either into a `.region` produces a file
that loads correctly exactly once — in the process that wrote it. This is precisely the class of
bug that does not show up in a round-trip test performed in the same process, so the round-trip
test must fork or restart.

Asset references are safe: FNV-1a-64 over a forward-slash relative path under `Content/`
(`SandboxApp.cpp:1275-1295`). A chunk can reference meshes by id without embedding them, and those
ids survive packaging unchanged.

### Entity ownership at boundaries

An entity must belong to **exactly one** chunk or save/load is ambiguous. Rules:

- Ownership is by the entity's **origin** (its own transform position), not its bounds.
- A hierarchy belongs **entirely to the root's chunk**, whatever the children's positions. Splitting
  a hierarchy across chunks would mean a child can load without its parent, and `CHierarchy` is
  intrusive — a dangling parent link is a crash, not a glitch.
- An entity whose bounds exceed the chunk size is legal and stays in its origin chunk; it is
  registered in a per-level **oversize list** that is always resident. A 200 m bridge in a 16 m-chunk
  level lives in the oversize list. This is why small chunk sizes cost more than they look.

---

## 3. The numbers

Independently recomputed, not taken from an agent.

```
chunk (default)  16 m       = 1600 cm
region           16³ chunks = 4096 chunks = 256 m = 25600 cm per side
```

Region is **always 16³ chunks**, so region extent scales with chunk size: 16 m → 256 m region,
32 m → 512 m, 64 m → 1024 m.

### f32 precision — the hard radius

Everything spatial in this engine is f32 at every layer: `Vec3`/`Mat4`/`Transform`
(`Math.hpp:32-35, 145, 246-249`), `CLocal.xf`/`CWorld.m` composed by f32 4×4 concatenation
(`World.cpp:180`), the scene C ABI passes `float*` (`scene_abi.h:7, 77-78, 117`), C# `Vec3` is three
floats, the `PerFrame` cbuffer is f32, and **Jolt is compiled `DOUBLE_PRECISION OFF`**
(`physics.jolt/CMakeLists.txt:57`). The only f64 is authoring text, narrowed at the first hop.

| world distance | f32 ULP at 1 cm/unit |
|---|---|
| 10 km | 0.63 mm |
| 16 km | 1.25 mm |
| 100 km | 1 cm |
| 1000 km | 8 cm |
| 6360 km (planet radius) | **64 cm** |

| precision first exceeds | beyond | in 16 m chunks | in regions |
|---|---|---|---|
| 1 mm | 10.49 km | 655 | **41** |
| 1 cm | 167.77 km | 10 486 | 655 |
| 10 cm | 1342.18 km | 83 886 | 5 243 |

**Read that as a budget, not a warning.** A level inside ±10.49 km of its origin — 41 regions in
every direction, an 21 km square — has sub-millimetre precision and needs *no* rebasing, no f64, no
camera-relative rendering. That covers essentially every game level anyone will author in this
engine. **Do not build origin rebasing for slice 1.** Build the check that tells you when you have
left the safe radius, and defer the rest until a level actually needs it.

**The two precision problems are different and must not be conflated:**

- *Large streamed world* — solved by staying inside the radius, and eventually by camera-relative
  rendering (a view-matrix change, not a streaming change).
- *Planetary atmosphere* — solved by never expressing the atmosphere in world centimetres at all.
  It is already parameterised in kilometres and altitude. At 6.36e8 cm, two sample points closer
  than 64 cm are **the same float**; the atmosphere is immune only because it never sees a world
  coordinate. Keep it that way.

---

## 4. Five blockers, all confirmed by reading the code

These are not risks. They are things that do not exist today and that streaming requires.

### B1 — There is no partial-read file API. *(hard blocker)*

`modules/platform/include/aver/platform/FileSystem.hpp:26-32` is the **entire** I/O surface:
whole-file read, whole-file write. No handle, no offset, no memory mapping, no seek.

> Streaming one 16 m cube out of a 256 m region file is **impossible** with today's platform layer.

Two honest options, and the plan picks the first:

1. **Add `readFileRange(path, offset, length, out)`** to the platform layer. Small, mechanical,
   and it is the thing that makes a streaming format meaningful at all.
2. Accept whole-region reads — all 4096 chunks resident whenever any one is touched. Simpler, and
   defensible for small levels, but it makes the region size the *real* streaming granularity and
   the chunk size decorative.

### B2 — `parseAvr1` requires an exact `FileSize` match. *(hard blocker for in-place writes)*

`Avr1.cpp:242-244` rejects any file whose header `FileSize` differs from the byte count handed to
it. **A `.region` can never be appended to in place** — adding one chunk means rewriting the header
(FileSize + HeaderCrc) and the whole directory.

**Resolution: `.region` is a COOKED, READ-ONLY RUNTIME ARTEFACT.** Authoring stays in the existing
text formats; a cooker writes regions; the runtime only ever reads them. AVR1's no-append limitation
then stops mattering, because nothing appends. Editor saves rewrite-and-rename a whole region file,
which is also how you get crash safety for free.

### B3 — There is no compression, anywhere. *(design constraint)*

`Avr1.cpp:262-263` refuses any chunk with `Compression != 0`, and `writeAvr1` hardcodes 0.
`third_party/` is exactly three directories: `fonts`, `imgui`, `stb`. The only DEFLATE in the tree
is stb's PNG-internal zlib, whose *compressing* half is linked only into the sandbox exe and imgui —
not into any engine module.

**Do not write "we already have zlib" into a design.** A compressed `.region` needs a new vendored
dependency; under the permissive-only rule the candidates are zstd (BSD-3) or LZ4 (BSD-2).

**So design `.region` to be useful uncompressed.** Density comes from encoding: u16 quantisation of
chunk-local positions, palette + bit-packed indices, sorted-delta ids. `.ocland` already quantises,
so there is a precedent in-tree to follow.

### B4 — There is no job system, thread pool, or task graph. *(largest genuine gap)*

`modules/core/include/aver/core/` is `Assert, Hash, Log, Math, Prelude, Time, Types, Version`. That
is all. **Async chunk loading has no native home.**

Either build a minimal worker pool + completion queue, or accept synchronous loading on the frame
thread. Slice 1 should be synchronous and *measured*; if a region load costs 3 ms the hitch is real
but survivable at bake-time-authored sizes, and the measurement tells you how much pool you need.

### B5 — The CLR host is Win32-only and declines cleanly. *(architectural constraint on `control.fs`)*

`ScriptHost.cpp:285-304` is the `#else // !_WIN32` block: `init` sets a decline reason and returns
false. It also declines if the bridge assembly is not staged beside the executable.

> **Therefore chunk visibility must never depend on F# running.** If `control.fs` is the thing that
> decides what loads, then on a non-Windows build, or a machine without .NET, or a packaged game
> missing the bridge, the world silently does not stream — and nothing errors, because declining is
> the designed behaviour.

**`control.fs` is policy-only, with a complete native default.** F# *refines* streaming; it never
*enables* it.

---

## 5. The `.region` format

An AVR1 container with a new subtype (`RGN `), following the conventions already in the tree.

```
AVR1 header            existing 64-byte header, FileSize + HeaderCrc as today
  subtype  'RGN '
  version  1

chunk 'RHDR'           region header
  i32   originChunk[3]     region coords, in CHUNKS not metres
  u32   chunkSizeCm        the level's fixed size, e.g. 1600 -- stored so a
                           region can be validated against its level
  u32   chunkCount         how many of the 4096 slots are non-empty
  u64   levelId            FNV-1a-64 of the owning level's relative path
  u64   contentHash        of all payloads, for staleness detection

chunk 'RDIR'           sparse directory -- ONLY non-empty chunks
  repeated chunkCount times, sorted ascending by packed index:
    u16  packedIndex       x + 16*y + 16*16*z, so 0..4095 fits exactly
    u32  payloadOffset     from the start of the RPAY payload
    u32  payloadBytes
    u32  payloadCrc

chunk 'RSTR'           string table, via the existing AvrStringTable
chunk 'RPAY'           concatenated per-chunk payloads, each 16-byte aligned
```

**Sparsity is a correctness requirement, not an optimisation.** A 16³ region covers 256 m
*vertically*. In a world whose content is essentially a surface, the overwhelming majority of those
4096 slots are empty air in every region, forever. `RDIR` therefore lists only non-empty chunks —
an empty region is a header plus an empty directory, a few hundred bytes.

`u16` for the packed index is exact: 16³ = 4096 < 65536. It stays exact for any region that remains
16³ regardless of chunk size, which is why the 16³ multiplier is fixed even though chunk size is not.

**Determinism:** directory sorted by packed index, fixed padding, no timestamps, no paths — so
writing the same level twice must produce byte-identical files. That is a free, baseline-free test
(write twice, `memcmp`).

**With B1 fixed**, `RDIR` is read with one small `readFileRange`, then only the needed chunk's
payload with a second. Without B1, the whole file is read and the directory is decorative.

---

## 6. `Chunk.index`

**It is not redundant with the content index.** The existing content index maps *asset id → file*.
`Chunk.index` maps *space → region file*, which nothing in the tree does today.

```
AVR1 subtype 'CIDX'
  u64  levelId
  u32  chunkSizeCm         the level's ONE chunk size -- the authority
  u32  regionCount
  per region:
    i32  regionCoord[3]
    u64  contentHash       must equal the region's own RHDR.contentHash
    u32  chunkCount
    str  relativePath
  bounds:
    i32  minRegion[3], maxRegion[3]
```

Three jobs, each of which justifies its existence:

1. **Existence without opening files** — "is there anything at region (12, −3, 0)?" answered from
   one resident structure. With 41 regions of radius that matters.
2. **Staleness detection** — `contentHash` mismatch against the region's own header means the index
   is stale; refuse to stream and say so, rather than streaming a lie.
3. **The chunk size lives here, once.** This is the file that makes "one chunk size per level"
   enforceable rather than conventional.

Must be rebuildable from scratch by scanning region files, so a corrupt index is recoverable.

---

## 7. `control.fs` — the F# control layer

The managed boundary is **cheap**: `nethost → hostfxr → hdt_load_assembly_and_get_function_pointer`,
binding eight named methods on `Aver.Scripting.Bridge.HostBridge`, every one an
`[UnmanagedCallersOnly]` raw `__cdecl` function pointer stored by value — no delegate, no
marshalling stub, no COM. The engine already makes four managed transitions per frame. F# and C#
land in the **same collectible ALC** (`"AverScripts"`), so F# can return objects C# consumes
directly.

So per-frame F# is affordable. It is still the wrong design, because of **B5**.

**The split:**

| Native (always runs) | F# `control.fs` (optional refinement) |
|---|---|
| The residency set and its default policy (radius around camera) | Priority ordering, prefetch hints |
| All file I/O and GPU upload | Eviction policy, budget shaping |
| The per-frame hot path | Gameplay-driven pinning ("keep the boss arena resident") |
| Hysteresis and the load budget | Debug/authoring overrides |

```fsharp
// control.fs -- pure functions over a snapshot. No I/O, no engine handles.
type ChunkCoord = { X: int; Y: int; Z: int }
type StreamState = { Camera: ChunkCoord; Resident: ChunkCoord[]; Pending: ChunkCoord[]; BudgetBytes: int }

module Control =
  /// Ranked load requests, highest priority first. Native truncates to budget.
  val request : StreamState -> ChunkCoord[]
  /// Chunks that may be evicted, cheapest first. Native may ignore.
  val evict   : StreamState -> ChunkCoord[]
  /// Never evict these regardless of distance.
  val pinned  : StreamState -> ChunkCoord[]
```

Note the shapes are **arrays and plain records**, not F# lists or options — the boundary types must
be things C# consumes without friction, which was already settled for the PCG API.

Both functions are **pure over a snapshot**, which makes them testable on CPU with no engine and no
GPU: same state in, same ranking out. That is the same property that makes the PCG side testable.

---

## 8. Sky: the planet layer and the media layer

### The delta is the world mapping, not the integral

This is the finding that reshapes the whole atmosphere task, and it corrects what I said earlier in
this session.

**Planet curvature IS ALREADY PRESENT inside the scattering integral.** There is a finite
`planetRadiusKm` (`Atmosphere.cpp:39, :94`), ray-sphere intersection (`raySphere`, `:60`), a
planet-shadow test, and a sun horizon that genuinely drops with altitude. The model is mirrored
function-for-function between C++ (`Atmosphere.cpp`) and HLSL (`RHIShaders.cpp:170-353`). The
radiometry is spherical. **Do not write "the sky is a flat dome" anywhere.**

**Planet curvature is ABSENT FROM THE WORLD MAPPING.** That is the actual gap:

- `averAtmoCamAlt() { return max(gCamPos.z * gAtmoPlanet.z, 0.0); }` (`RHIShaders.cpp:293`) —
  altitude is literally `camPos.z` scaled.
- Local up is hardcoded to world **+Z** everywhere. There is no planet centre in world space, so
  moving horizontally never tilts the zenith.
- `float cosV = max(dir.z, 0.0);` (`:343`) hard-clamps the view zenith, so **the visible horizon
  never drops** however high you climb.

So "actually simulate a planet" = replace three flat-Z substitutions (camera altitude, view
cos-zenith, sun cos-zenith) with a real planet-centre vector.

> **A curved horizon cannot simply be switched on.** It was tried and reverted, because the engine's
> flat drawn ground disagreed with the model's sphere. Any curved-horizon work must be paired with
> curved ground, or it regresses. Treat these as one slice, never two.

### Free win, take it first

`atmoFitDome(fit, 0.0f, sunCos, e0, sunRadius, dome)` (`D3D12Device.cpp:2275`) — the second argument
is `altitudeKm`, **hardcoded to sea level**. The CPU dome fit feeds *every* cheap consumer of the
sky (ambient, environment reflections, fog, GI injection), so none of them change with camera
altitude at all; only the sky pass itself sees the real camera. `atmoFitDome` already takes the
parameter. **Passing the real altitude is a one-argument change**, and it is very likely gate-neutral
at ground level (where every gate camera sits) — which must be verified, not assumed.

### "Light just being the light for now" — what this explicitly does NOT touch

The sky lights the scene through exactly **four** couplings, all via the fitted two-colour dome and
a single scalar `gAmbient.r` (`D3D12Device.cpp:2191`). There is **no spherical-harmonic path, no
irradiance probe and no reflection probe anywhere in the repo**.

That makes the instruction map cleanly onto a fence: **leave `gAmbient.r`, `averSkyIrradiance` and
the dome fit alone.** All four couplings are gate-moving surfaces. A sky-lighting/IBL overhaul —
SH probes, real irradiance integration, reflection captures — is a **separate future slice**, named
here and deferred.

(One honest note for whoever does that slice: `averSkyIrradiance` is not an irradiance integral at
all. It is a single dome sample in a fudged direction, and callers deliberately omit the `1/π`
because the convention is "the sky term is already a radiance". Do not "fix" the missing `1/π` in
isolation — it is load-bearing.)

### The two layers

| | Planet layer | Media layer |
|---|---|---|
| Content | Rayleigh, Mie, ozone, transmittance, multi-scatter | clouds, fog volumes, local weather |
| Scope | **global — one per world** | **per chunk, optional, absent by default** |
| Varies with | altitude + angles | world position |
| Chunked? | **never** | **yes** |
| Where authored | world/planet settings | chunk media list in `.region` |

The planet layer is radially symmetric and keyed on `(altitude, view angle, sun angle)`. Move 16 m
sideways and every input is unchanged. Chunking it would store 4096 identical copies per region.

There is also a seam argument: aerial perspective must be **continuous across a chunk boundary**.
Bake it per chunk and two chunks that stream in 40 frames apart show a visible discontinuity along
the boundary. Computing it from the camera each frame is seamless by construction — which is
exactly why UE's aerial-perspective volume is **camera-frustum-aligned**. UE does chunk the
atmosphere; it chunks it along the view ray, not the world grid.

### On LUTs — do not cargo-cult UE

| UE table | Verdict here |
|---|---|
| Transmittance LUT | **No.** Transmittance is already analytic via a Chapman function — roughly one `exp` plus ~10 FMA, **cheaper than a texture fetch**. Adding an SRV would be a root-signature change to buy a slowdown. |
| Multi-scatter LUT | **The one real gap.** Today it is a single spectrally-flat isotropic scalar gain (1.70) that also silently stands in for ground bounce. Replacing it needs a texture → an SRV in a scene root signature that currently declares none → a root-signature change. Worth it, but cost it honestly. |
| Sky-view LUT | **Probably yes, for a different reason than UE's.** The sky is currently a full-viewport 32-step march with depth testing **disabled**, drawn *before* geometry — so it is paid at full resolution every frame with no early-Z. If chunk streaming raises geometry coverage, that is pure waste. A low-resolution sky-view LUT plus a depth-tested composite fixes the waste, not the quality. |
| Aerial-perspective volume | **Defer.** Aerial perspective already exists (`averAtmoAerial`, `RHIShaders.cpp:298`, used at `:392`). A froxel volume is an optimisation of something that works. |

---

## 9. Verification

Following this repo's culture: prefer relational and analytic oracles that need no baseline.

**No baseline needed:**

- **Writer determinism** — cook the same level twice, `memcmp` the `.region` files.
- **Round trip, ACROSS PROCESSES** — save, exit, reload, compare entity transforms and component
  values. It *must* cross a process boundary, because the two process-local intern tokens (§2) make
  a same-process round trip pass while the file is unloadable anywhere else.
- **Chunked equals whole** — load a level chunked with everything resident, and the same level flat;
  assert identical entity sets and transforms. This is the strongest single test and it needs no
  renderer.
- **Boundary conservation** — total entity count across all chunks equals the flat count. No
  duplicates, no drops. Run it with entities deliberately placed exactly on boundaries and at
  negative coordinates (where truncation-vs-floor bugs live).
- **Precision guard** — assert no authored entity lies outside the safe radius, or warn with the
  measured ULP at its distance.
- **`control.fs` purity** — same `StreamState` in, same ranking out, with no engine running.
- **Streaming with F# absent** — force `ScriptHost` to decline and assert the world still streams
  on the native default policy. This is the B5 regression test and it is the one most likely to be
  forgotten.
- **Atmosphere energy** — `furnaceRadiance`/`furnaceSun` and `PtFurnaceTest`'s analytic tolerances
  keep working unchanged, because there is still exactly one global environment to replace. Any
  planet-mapping change must leave the furnace results bit-identical; that is a free, strong oracle.

**Needs a render gate (and therefore a re-recording):** anything touching the dome fit, the four
lighting couplings, or the horizon. Budget a full `-Only Both` recording (~50 min, and only the user
can run it) for the curved-horizon slice. The `atmoFitDome` altitude fix should be gate-neutral at
ground level — verify before assuming.

---

## 10. Slices

**Slice 0 — platform prerequisites.** `readFileRange` in the platform layer (B1); decide and record
the synchronous-vs-pool answer for B4 by measuring a whole-region load.
*DONE-WHEN:* `readFileRange` reads a known byte range out of a large file and is covered by a test;
a measured region-load cost is written down.

**Slice 1 — the level declares itself.** `FLAT` (today, unchanged) vs `CHUNKED <sizeCm>` in the
level header. No streaming yet. Everything keeps working.
*DONE-WHEN:* every existing level loads bit-identically; a chunked level round-trips its declared
size; a level declaring two sizes is refused with a clear error.

**Slice 2 — the participation protocol.** The seam by which framework, physics and the mesh cache
contribute to and restore from a chunk (§2). No files yet.
*DONE-WHEN:* an in-memory partition/reassemble of a loaded level reproduces the flat entity set
exactly, hierarchies intact, oversize list correct.

**Slice 3 — `.region` + `Chunk.index`, cooked and read-only.** The format, the cooker, the reader.
*DONE-WHEN:* writer determinism (`memcmp` on two cooks); cross-process round trip; chunked-equals-
whole; boundary conservation; a corrupt CRC is refused with a named chunk rather than a crash.

**Slice 4 — native streaming.** Residency set, radius policy, hysteresis, load budget. No F# yet.
*DONE-WHEN:* a camera flight across ≥9 regions holds a bounded resident set, never drops an entity,
and the frame-time cost of a region load is measured and recorded.

**Slice 5 — `control.fs`.** Policy refinement over the native default.
*DONE-WHEN:* purity test passes; pinning demonstrably keeps a chunk resident outside the radius; and
**with the script host forced to decline, streaming is unchanged.**

**Slice 6 — atmosphere, free wins.** Real altitude into `atmoFitDome`; planet-centre vector
replacing the three flat-Z substitutions.
*DONE-WHEN:* furnace tests bit-identical; ambient/reflections/fog now track camera altitude; gate
delta measured and either zero or explained.

**Slice 7 — media layer per chunk.** Cloud volumes and local fog attach to chunks; the F# PCG
`densityVolume` path feeds them.
*DONE-WHEN:* a chunk with no media renders identically to today; media appears and disappears with
its chunk with no seam at the boundary.

**Slice 8 — curved horizon + curved ground, together.** The slice that was tried and reverted.
*DONE-WHEN:* the horizon drops measurably with altitude, the drawn ground agrees with the model's
sphere at every altitude tested, and a full baseline re-recording is green.

**Deferred, named, not planned here:** sky-lighting/IBL overhaul (SH or probes); origin rebasing /
camera-relative rendering; compression; the multi-scatter LUT.

---

## 11. What I would NOT do

1. **Do not build origin rebasing for slice 1.** 41 regions of sub-millimetre radius is more than
   any level here will use. Build the *check*, defer the machinery.
2. **Do not make streaming depend on F#.** B5 makes that a silent, platform-specific "the world is
   empty" bug with no error anywhere.
3. **Do not write a transmittance LUT.** It would be slower than the analytic Chapman form it
   replaces, and it costs a root-signature change to get there.
4. **Do not enable a curved horizon without curved ground.** It has already been tried and reverted
   for exactly this reason.
5. **Do not serialise `CMeshRenderer::material` or a `FieldKind::String` index.** Both are
   process-local; the file will load exactly once, in the process that wrote it.
6. **Do not assume "we already have zlib".** The compressing half is not linked into any engine
   module.
7. **Do not let `.region` be writable at runtime.** AVR1's exact-`FileSize` rule makes in-place
   append impossible; cooked-and-read-only turns that constraint into a non-issue instead of a
   fight.
8. **Do not "fix" the missing `1/π` in `averSkyIrradiance` in isolation.** It is load-bearing —
   callers omit it deliberately.
9. **Do not chunk the planet layer.** It has no lateral variation to partition, and baking it per
   chunk creates streaming seams that computing it from the camera does not.
