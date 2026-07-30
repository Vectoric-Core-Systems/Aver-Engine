# Aver.Landscape  (`modules/landscape`)

- **Language:** C++
- **Depends on:** Core, Formats
- **Status:** stages 1–2 implemented — the whole CPU model. **Nothing is rendered yet.**

Chunked heightfield terrain: the sample grid, the node quadtree, screen-space-error LOD selection,
frustum culling, and chunk geometry with skirts.

## It has no RHI dependency, and that is the design

Not tidiness — it is what makes the terrain arithmetic checkable. `tests/landscape` links this and
`Aver.Formats` and runs on a machine with no GPU, no device and no .NET, so the quadtree, the LOD
metric, the culling and the chunk counts are all settled *before* anything tries to draw them. The
renderer half is a separate target for the same reason `Aver.Render.Voxi.Renderer` is one: it needs
the RHI, and giving this target that dependency would end the property.

## The shape

A section (one `.ocland` file — see [OcLand.hpp](../formats/include/aver/formats/OcLand.hpp)) is
subdivided into nodes that all render the **same** vertex count, 65×65 by default, covering
proportionally more source samples as the level rises. For a 1025-sample section at 64 quads/node:

| level | nodes | source quads across | stride |
|---|---|---|---|
| 0 | 16×16 = 256 | 64 | 1 |
| 1 | 8×8 = 64 | 128 | 2 |
| 2 | 4×4 = 16 | 256 | 4 |
| 3 | 2×2 = 4 | 512 | 8 |
| 4 | 1 | 1024 | 16 |

341 nodes. A constant vertex count per node makes memory, draw cost and skirt overhead predictable at
every level, and makes draw count **logarithmic in view distance** rather than linear in area.

**65, not 64**, because 2⁶+1 shares its edge row with the neighbour — two nodes at the same level agree
on their shared vertices exactly, so in the common case there is no crack at all. Cracks only appear
across a *level* change.

## Cracks: skirts, not stitched index buffers

A node's skirt hangs below its rim by 1.5× the **next coarser level's** geometric error. The gap that
needs covering is never this node's error — it is the coarser neighbour's, whose chord spans twice the
spacing and can sit below this rim. Selection is 2:1 balanced, so the coarsest possible neighbour is
exactly one level up.

Stitching was rejected on a hard constraint, not taste: it needs a variant index buffer per
neighbour-level combination — 16 per node — and every one is a separate **immutable** mesh here,
because there is no `updateMesh`. A skirt is one extra ring and is correct for all 16 at once.

`tests/landscape` measures the actual worst gap along every node's rims at the coarser stride and
requires the skirt to exceed it. Current worst ratio: **1.505**, so the bound is tight rather than
accidentally generous.

## LOD is screen-space error, not distance bands

Each node stores a build-time geometric error in cm — the largest vertical distance between a source
sample and the surface the node actually draws. Identically **0** at level 0 (every sample is a
vertex) and non-decreasing with level; both asserted. Projected as
`errorCm * projScale / distance` and compared against a pixel threshold, default 2 px.

Distance is **radial** to the node's bounding *sphere*, floored at zero. Spherical because the fit is:
planar depth would pick a different level for the same node depending on where in the frustum it sat,
and the seam would move as the camera turned.

**Hysteresis** is stateful and deliberate — a refined node stays refined until its error falls to 0.8×
the threshold. Without the gap a camera hovering on a boundary switches level every frame, and a pop
at 2 Hz is far more visible than the extra detail is worth.

## Two limits worth knowing before using it

- **The draw clamp is a hard ceiling** (default 192), not advice. The transient constant ring is 1 MiB
  and a submitted draw costs ~2 KiB across the shadow, voxelisation and lit passes; past exhaustion the
  allocator returns 0, the constant buffer is silently not bound, and shading is wrong with one logged
  error. Excess nodes are dropped in ascending screen error and the count is **reported**, never
  swallowed.
- **LOD transitions pop.** Geomorphing — the usual fix — is blocked: meshes are immutable and there is
  no `updateMesh`, so a vertex cannot be nudged toward the coarser chord over a few frames. The pop is
  bounded by the pixel threshold, which is the honest mitigation rather than a fix.

## Axes

Heights are **+Z**; sample `(ix, iy)` sits at `(origin.x + ix·s, origin.y + iy·s, h)`, so a column runs
+X and a row +Y. Jolt's heightfield is Y-up with its own row order, so the physics adapter must
transpose — stage 3. A reader who assumed they agreed would get terrain colliding ninety degrees from
where it is drawn.

See [docs/ARCHITECTURE.md](../../docs/ARCHITECTURE.md) for the module DAG.
