# Aver.World  (`modules/world`)

- **Language:** C++
- **Depends on:** Core, Formats, and — when the tree has them — Scene and Physics
- **Status:** partial. `.ocworld` placement instantiation is here; region streaming is not yet.

The level/world runtime: `.ocworld`/`.ocmap` loading, object-reference placements, surface/ground/killz
env, spawn, streaming; wires Scene↔Render↔Physics.

## What is here today

| File | What it owns |
|---|---|
| `LevelTransform.hpp` | The `.ocworld` rotation encoding: Euler degrees ↔ quaternion. Header-only, **Core-only**. |
| `LevelInstance.hpp/.cpp` | `instantiate()` — parsed placements into live scene entities, plus their static bodies. |

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

`docs/CHUNKS.md` is the plan. Slice 1 adds the coordinate hierarchy — `RegionCoord`, `ChunkCoord`,
and the region → chunk → local-float mapping — here, alongside `LevelTransform.hpp` and with the same
Core-only dependency.
