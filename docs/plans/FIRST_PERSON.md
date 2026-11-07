# First-person rendering

Meshes flagged first person, such as a held weapon and arms, draw with their own field of view and with their depth squeezed into the nearest sliver of the depth range. They never clip into the world, and the world never draws over them. They are lit and shadowed where they really are. This is the engine's equivalent of Unreal 5.5's First Person Rendering. Converted Godot games reach it through Fabric, which recognises Godot's depth-push shader trick.

## Decisions (owner, 2026-11-07)

- **Ray-driven mode:** first-person meshes are kept out of the primary rays. They draw as a raster overlay after the ray-driven resolve, with the raster shader, lit by the shadow cascades and voxel GI. Step 6 starts with a spike to see how close that lighting gets.
- **Shadows:** first-person meshes receive shadows but cast none by default. A per-mesh flag opts in to casting (from their true position).
- **Authoring:** two ways, both supported. A per-mesh flag, or a first-person root entity whose whole subtree counts as flagged.

## How it works

- **Depth:** Z is not reversed. The depth range is [0, 1] with compare LESS (`CameraProjection.hpp`), so near is 0.
- **Projection:**
  - The first-person matrix is `firstPersonViewProj(view, fpLens, aspect, slab)`: the first-person lens's projection with clip z scaled by `slab`. With row vectors, that is column 2 times `slab`, plus `zmin * w` when the slab does not start at 0.
  - The viewmodel then occupies `z_ndc` in [0, slab]. The default slab is 0.01; with a 2 cm near plane, everything farther than about 2.02 cm is behind it.
  - With no first-person view set, the matrix equals the main view-projection, so the flag does nothing.
- **Per-view settings** (`PlayView` and SubViewport lenses):
  - `fpFovRad`, default 70 degrees, the same axis as the main FOV;
  - `fpNearCm`, default the main near;
  - `fpFarCm`, default 300;
  - `fpDepthSlab`, default 0.01.
  - Jitter is applied as it is for the main matrix.
- **Shader:**
  - The per-frame constants gain `gFpViewProj` and `gFpPrevViewProj`.
  - One helper, `averScenePos(wp)`, picks it when the draw's first-person bit is set. The bit is bit 31 of the object block's `_objPad`, above the 20 inverted layer bits.
  - Every scene vertex entry point routes `o.pos` through the helper: `VSMain`, `VSMainFx`, `VSMainEmb`, and the mesh and amplification paths.
  - `o.wpos` and the normals stay true world values, so lighting, shadows, fog and GI sampling see the real position.
  - The G-buffer's view Z is computed from `wpos` and stays true.
- **Flags:** `CMeshRenderer::renderFlags` gains `kRenderFirstPerson` (0x20) and `kRenderFirstPersonCasts` (0x40). Zero means off; the pool zero-fills.
- **Scene walk** (`GameRender.cpp`), for flagged meshes:
  - skip the main-frustum cull and the occlusion test;
  - submit with no shadow unless the cast flag is set;
  - exclude from GI voxelisation;
  - keep out of the TLAS unless the cast flag is set, and then use the owner-hidden lane (seen by secondary rays only).
  - A first-person root is resolved with the same ancestor walk the owner-hide uses.
- **Motion vectors:** a rigidly attached viewmodel has no screen motion under camera rotation. Velocity is `FP(cur)` minus `FP(prev)` applied to the same view-space point. Exact per-vertex animation velocity needs a previous-world matrix per draw, which is a follow-up. First-person pixels are marked in the G-buffer (`normalRoughness.w`, currently 0) for TAA and denoiser history.
- **Path tracer:** reference views leave first-person meshes out by default.

## Steps

1. Matrix helper, flags, `PlayView` fields. Headless tests in CameraProjectionTest:
   - a first-person point lands in [0, slab];
   - a world point at the near plane stays behind it;
   - the default matrix equals the main one;
   - flag packing and the known-flags mask.
2. Raster plumbing on D3D12 and Vulkan: `IDevice::setFirstPersonCamera` (defaulted), the per-frame fields, the object-block bit, and `averScenePos` in every scene VS. Shaders compile headless on both backends. Headed check: a flagged box inside a wall stays visible and whole.
3. Scene walk: culling, shadow and GI defaults, the first-person root. Recording-device tests through the submit `decide` hook.
4. Motion vectors and the G-buffer mark. Headed A/B with the camera spinning (no smear, no TAA ghosting).
5. Voxi paths: shadow receive, opt-in cast, GI exclusion, TLAS lane. Draw-list unit tests like the noRay/noShadow ones.
6. Ray-driven overlay, after a lighting spike. Also an audit of everything that reads the depth buffer as a position (particles, depth of field, editor lines, froxel apply, TAA reprojection).
7. Path-tracer exclusion, with an option to include.
8. C# (`Entity.SetFirstPerson`, `SetFirstPersonCastsShadow`) and the Details panel checkboxes.
9. Fabric: recognise a `POSITION.z` write that pushes depth toward the near plane (Godot's clip space is reversed Z), drop the write and flag the meshes that use the material. Other POSITION writes stay refused. The first-person FOV defaults to the camera's.
