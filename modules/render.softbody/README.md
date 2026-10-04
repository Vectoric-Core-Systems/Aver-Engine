# Aver.Render.SoftBody  (`modules/render.softbody`)

- **Language:** C++
- **Depends on:** Core, RHI, Scene, Formats, Physics
- **Option:** `AVER_MODULE_RENDER_SOFTBODY` (ON; force-disabled without both the scene and physics)

Draws a mesh whose vertices came from a simulated soft body rather than from its asset.

## Where the soft body itself lives — not here

| | |
| --- | --- |
| **The simulation** | Jolt, through `Aver.Physics`'s C ABI (`aver_phys_softbody_*`). Not a solver of our own: Jolt's `Skinned` constraint tethers a particle to where bone skinning would have put it, and it collides with the real world. |
| **Which entities are soft** | `scene::CSoftBody`, component 13. |
| **Drawing the result** | This module. |

`modules/softbody` is a **different, unbuilt** thing — see its README. It is reserved for *plastic*
deformation (permanent set, break/tear), which Jolt has no notion of.

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

## Known limit

`aver_phys_softbody_create` takes a centre, not a matrix, so an entity's **rotation and scale do not
reach the body's rest shape**. A rotated mesh simulates as though axis-aligned.
