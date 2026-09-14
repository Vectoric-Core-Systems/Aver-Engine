# Aver.World  (`modules/world`)

- **Language:** C++
- **Depends on:** Core, Formats, Platform (for `aver::File`, ranged durable I/O), Render.Pcg (for
  `pcg::sampleInfinite`, the chunk generator's noise), and — when the tree has them — Scene and
  Physics. This line used to list only Core/Formats/Scene/Physics; Platform and Render.Pcg were added
  as unconditional deps for the region and generation work (`modules/world/CMakeLists.txt`).
- **Status:** far along. `.ocworld` placement instantiation, region streaming, generation-as-you-go
  and runtime region writes are all here — slices 0–9 of `docs/CHUNKS.md` are DONE and `modules/world`
  on disk backs that up. This line used to say "region streaming is not yet"; that was true when
  written and has not been since slice 6 (`84178f6`). What is still open is slice 10, floating-origin
  rebasing: no rebasing code exists anywhere in this module (or elsewhere in the tree) at the time of
  this check, so a level far from the coordinate origin still simulates in full precision, unrebased.

The level/world runtime: `.ocworld`/`.ocmap` loading, object-reference placements, surface/ground/killz
env, spawn, streaming; wires Scene↔Render↔Physics.

## What is here today

| File | What it owns |
|---|---|
| `ChunkCoord.hpp` | The region → chunk → local-float coordinate hierarchy. Header-only, **Core-only**. |
| `LevelTransform.hpp` | The `.ocworld` rotation encoding: Euler degrees ↔ quaternion. Header-only, **Core-only**. |
| `LevelInstance.hpp/.cpp` | `instantiate()` — parsed placements into live scene entities, plus their static bodies. |
| `ChunkPayload.hpp/.cpp` | A chunk's contents with **no process-local value in it**, and `capture`/`restore`. |
| `ChunkPartition.hpp/.cpp` | Which chunk owns which entity, and capturing a whole world at once. |
| `BodyRegistry.hpp` | Entity → physics body. This association exists nowhere else in the engine. |
| `ChunkCodec.hpp/.cpp` | A `ChunkPayload` as bytes and back — deterministic by construction, no process-local value, so cooking the same level twice can be `memcmp`'d. |
| `RegionFile.hpp/.cpp` | The `.avrgn` region archive: one file per 1024³ chunks, seekable, checksummed, crash-detectable, and NOT an AVR1 container (AVR1 is a bad fit for something seeked into and later mutated). |
| `RegionIndex.hpp/.cpp` | `.ocindex` — which regions a level has, without opening any of them; carries each region's content hash and the level's one chunk size. |
| `ChunkSource.hpp/.cpp` | The interface a chunk's contents come through, so residency logic can be tested with no files on disk at all; `LayeredChunkSource` composes a generated baseline with saved overrides. |
| `ChunkStreamer.hpp/.cpp` | Which chunks are resident and the load/unload that keeps it that way. Synchronous on the frame thread with a budget, on purpose — `scene::World` is not thread-safe and there is no job system to post to. |
| `ChunkGenerator.hpp/.cpp` | Generating a chunk's contents from nothing but a seed and coordinate — CPU, not GPU, so the same chunk is reproducible bit-for-bit rather than to within a GPU/CPU ULP. |
| `ScatterPalette.hpp/.cpp` | Turns a level's declared `SCATTER` records into the runtime palette `ChunkGenerator` consumes; the one place both hosts call it. |
| `ChunkWorld.hpp/.cpp` | Binds a `ChunkStreamer` to a generator-plus-overrides source stack and a scene behind one per-frame call; owns the `BodyRegistry`. Gated whole on `AVER_MODULE_SCENE`, like everything it is built from. |

`ChunkCoord.hpp` is a **value header**, like `Vec3`. It is not behind a module switch and must not
become one: if asking "which chunk is this in" needed an `#if`, every caller would grow one. Its test
links `Aver.Core` and nothing else, which is what keeps that true.

Both existed **twice** before this module did, once in `modules/runtime.game/src/GameLevel.cpp` and
once in `sandbox/src/SandboxApp.cpp`. That was deliberate at the time — `modules/runtime.game`'s own
CMakeLists says so: *"The lift is a COPY … De-duplication is a later slice, proven by the gates
staying identical."* This module is that slice.

The two copies had already drifted: the editor applied `SUN`/`SKY` at load and drove the cloud layer
from a `PCGVOLUME` named `"Sky"`, while the game applied only `FOG`. Each narrowed `f64`→`f32` in its
own near-identical loop. Chunk ownership would have had to be added to both, correctly, forever.

## What stays with the hosts

`instantiate()` deliberately does **not** own sky application, camera framing, selection, the
editor's label table, or the game's resolved PCG field specs. Those are host policy, and they read
this function's output rather than living inside it. Material *resolution* is a host job too — the
game resolves through `GameContent`, the editor into its own map — so the module only says *when*,
through `InstantiateOptions::bindMaterial`, and never links a material system.

## Why it is unconditional and not part of Aver.Runtime.Game

Both hosts link it. Putting it in `Aver.Runtime.Game` would make the editor depend on
`AVER_BUILD_GAME`, and the root `CMakeLists.txt` calls those two switches *"opposites — one is the
editor, one is what a shipped game runs, and a tree may reasonably want either alone."*

A tree with `AVER_MODULE_SCENE=OFF` still configures and links this; `LevelInstance.cpp` compiles to
nothing, the same way `GameLevel.cpp` does.

## Next

`docs/CHUNKS.md` is the plan. This section used to say slices 0–4 were done and slice 5 (the
`.avrgn`/`.ocindex` region format) was next; slices 0 through 9 are now all DONE — region files,
streaming residency, runtime writes and generation-as-you-go are built and are the files listed
above, not a plan any more. Blockers B1 (no partial read) and B6 (no `IDevice::destroyMesh`) are
closed; B2 (AVR1's exact `FileSize`, though `.avrgn` itself is not an AVR1 container), B3 (no
compression) and B4 (no job system — `ChunkStreamer` stays synchronous because of it) remain open
per `docs/CHUNKS.md` §1.

What is actually next is **slice 10, floating-origin rebasing**: `docs/CHUNKS.md` calls it "the one
genuine exception" to everything else being done, and this module already has an anchor (the region)
and a constraint (X/Y only, never Z) decided for it — but no code that does it.
