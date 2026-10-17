# Aver.Render.SoftBody  (`modules/render.softbody`)

- **Language:** C++
- **Depends on:** Core, RHI, Scene, Formats, SoftBody, SoftBody.Scene
- **Option:** `AVER_MODULE_RENDER_SOFTBODY` (ON; force-disabled without the scene)

Draws cloth and destructible pieces whose vertices came from a simulated soft body rather than from the
asset. **Draw only**: it creates no bodies and runs no physics.

## Where the soft body itself lives - not here

| | |
| --- | --- |
| **The simulation** | Jolt, through `Aver.Physics`' `aver_phys_sb_*` C ABI (`physics_softbody_abi.h`). |
| **Which entities are soft, damage, tearing, pieces** | `Aver.SoftBody.Scene` (`SoftBodySystem`), see `docs/SOFTBODY.md`. |
| **Drawing the result** | This module: `SoftBodyScene` reads `SoftBodySystem::pieces()`. |

## The one idea

A soft body is a per-**entity** set of vertices that differ from the asset's, and the renderer
already has a seam for exactly that: `IDevice::createSkinTargetMesh` mints a mesh over a
GPU-writable vertex buffer, and both renderers substitute that `MeshHandle` at the draw. GPU
skinning has used it for every character since it shipped. Nothing below this module changes.

## What is genuinely different from skinning

Skinning writes its vertices with a compute shader, straight into the buffer the input assembler
reads. Jolt simulates on the **CPU**, so three things follow, and they are the whole content of the
module:

- **The pixels have to travel.** `writeBuffer` refuses a Default-heap buffer, so the route is an
  Upload staging buffer, `copyTexture`-style, `copyBuffer`'d across under a barrier.
- **The staging must be ringed**, `kSoftBodyFramesInFlight` deep. `writeBuffer` is immediate and
  unsynchronised; one buffer would be rewritten while the GPU was still reading it.
- **The producer emits bare positions**, in *world* space, while `rhi::MeshVertex` is
  `{pos, normal, uv}` interleaved and `drawMesh` applies the entity transform on top.
  `softBodyPackVertices` closes both gaps and is a **free function** precisely so the space
  conversion, the recomputed normals and the UV passthrough are checkable with no GPU —
  `tests/render.softbody` does exactly that.

## Epochs and retirement

A tear, a release or an island split changes a piece's topology, which bumps its epoch. `SoftBodyScene`
then builds a new source mesh (rest vertices copied from the base mesh through the piece's
`sourceVertex` map so UVs and normals are right), a new skin-target mesh and a fresh staging ring, and
puts the old three on a retire list destroyed `kSoftBodyFramesInFlight + 1` frames later. Rebuilds count
against the system's `maxRebuildsPerFrame`. Each frame the piece's vertex AABB is written into
`CMeshRenderer` so a flung piece is not culled at its old place. Pieces shrink toward their centroid
(`scale01`) while despawning.

The packaged game builds a `SoftBodyScene` too and passes it as `DrawWorldOptions::softBodies`.
