# Fabric G4: rendering basics (contracts)

G4 is the rendering half of the Fabric Connector (`FABRIC_CONNECTOR.md`, "Rendering"): the features a converted Godot 4 project expects,
built into the engine, mapped by `AverAssetC fabric` and exposed by `Aver.Fabric` under Godot's property names. Aver's look stays Aver's.

Rules every package follows (the engine cannot be run, so nothing here has been seen on a GPU):

- Defaults unchanged: a project or level that does not use a feature runs today's pipelines, today's shader variants and today's constants. New
  shader code sits behind a compile-time define in a NEW variant that is built only when something uses it.
- Nothing is added to `PSRayDriven`, the `AVER_RD_SINGLE_PASS` variants or the RT shadow-ray code.
- Vulkan parity: whatever D3D12 gets is wired in `rhi.vulkan` too.
- Every GPU-visible change adds steps to "Headed checks for the owner" at the end of this file.

One section per package. Later packages append below the last section; they do not edit an earlier package's contract without saying so.

---

## R1: vertex colour, tangent and UV2 at runtime

Status: code, converter and headless tests done; the GPU side is unseen (see the headed checks).

### The second vertex stream

`rhi::MeshVertex` (32 B: position, normal, uv0) is unchanged. It is the HLSL `MeshVtx`, the D3D12 `kMeshInputLayout`, the DXR BLAS vertex
layout and the bindless RT geometry table's element, and it stays that.

A mesh that has any of vertex colour, an authored tangent or a second UV set carries an OPTIONAL second vertex buffer, input slot 1,
`rhi::MeshVertexExtra` (`modules/rhi/include/aver/rhi/MeshVertexExtra.hpp`), 16 bytes per vertex:

| Offset | Size | Format | Meaning |
|---|---|---|---|
| 0 | 4 | `RGBA8Unorm` | vertex colour, LINEAR, 255 = 1.0. Quantised to 1/255; values over 1 clamp (no HDR vertex colour) |
| 4 | 4 | `RGBA8Unorm` | tangent: xyz = unit tangent * 0.5 + 0.5; w = 255 (handedness +1), 0 (handedness -1), about 128 (no tangent at this vertex) |
| 8 | 8 | `RG32Float` | UV2 (`TEXCOORD1`), full precision because a lightmap UV needs it |

A mesh without any of them has no second buffer, binds nothing new and uses today's pipelines.
`IDevice::setMeshExtra(mesh, extras, vertexCount)` attaches the stream (once, right after `createMesh` / `adoptMesh` and before
`createMeshSharingVertices`); `IDevice::meshHasExtra` asks. A vertex-sharing mesh (LOD level, per-material part, depth proxy) borrows its
root's stream exactly as it borrows the vertices, so `destroyMesh`'s refusal while sharers live protects it. A compute-written mesh
(skin target, posed part, soft body) never gets one: skinned meshes ignore colour, tangent and UV2 for now.

Absent values inside a stream that exists: colour 255,255,255,255 (white), tangent "none", UV2 (0,0).

### The tangent contract (engine space)

For a vertex with normal `N` and stored tangent `T` (xyz) and `w`:

- `T` is the unit direction in which the stored UV0 `u` increases, orthogonal to `N`;
- `cross(N, T) * w` is the direction in which the stored UV0 `v` increases (`w` is exactly +1 or -1);
- `w == 0` (all four components 0) means "this vertex has no tangent", and the shader falls back to the derived frame for it.

This is exactly the frame `averPerturbNormal` solves for from screen-space derivatives (`T` along d(position)/du, `B` along d(position)/dv, both
as stored), so supplying the tangent changes how the frame is obtained, not what it means. Importers compute `w` in engine space after the axis
map instead of copying the source's sign: glTF's bitangent `cross(N, T) * w` points at DECREASING v (green is up in the image and v runs down it),
the engine's points at increasing v, and the axis map has determinant -1 on top of that. `fmt::packTangent` (OcMesh.hpp) is the one function that
turns (normal, tangent, "direction of increasing v") into the stored 4-vector; `fmt::generateTangents` builds them from UV0 for meshes whose source had
none (the converter's primitive meshes).

### `.ocmesh`

`OcMeshData` gains `colors` (4 f32 per vertex, linear), `tangents` (4 f32) and `uvs1` (2 f32); each is empty or exactly one entry per vertex
(`hasColor()` / `hasTangent()` / `hasUv1()`, `valid()` refuses a half-sized one). The file gains one optional chunk, `VXEX` (FORMAT_SPECS 5.11):
colour RGBA8, tangent 4 x snorm16 (w exactly +-1), UV1 2 x f32, each block padded to 16 B, written only when present. MHDR flag bits `HasColor`
(0), `HasUV1` (1) and the new `HasTangent` (12) follow the chunk; `UVChannelCount` is 2 with UV1.

Compatibility: a mesh without the streams writes exactly the chunks and `contentVersion` (1) it always wrote (`MeshExtraTest` rewrites a loaded old
mesh byte-identically); the reader treats a missing VXEX as no streams and an older reader skips an unknown chunk. A VXEX whose vertex count
disagrees with the mesh, or that is cut short, fails the load. `simplifyMesh` and `AverAssetC`'s mesh merge keep the streams in step with the vertices
they compact or append (a part without a stream gets white / "no tangent" / UV 0).

### Importers

| Source | Colour | Tangent | UV2 |
|---|---|---|---|
| glTF | `COLOR_0` (VEC3 or VEC4, any component type) | `TANGENT` (needs `NORMAL`); `w` re-derived as above | `TEXCOORD_1` |
| FBX (ufbx) | vertex colour layer | the file's tangent + bitangent (both) | the second UV set (v flipped like the first) |
| OBJ | `v x y z r g b` | - | - |

A primitive without an attribute pads the others (white / none / 0) so a mesh of several primitives keeps its streams 1:1. The importers no longer
report "vertex colour" and "a second UV set" as dropped; a third UV set and glTF `TEXCOORD_2` still are. USD is not covered. Material
`texCoord: 1` (a texture sampled with the second set) is still reported and sampled with UV0; R6 wires UV2 into materials.

### Materials

`pbr::MaterialDesc::vertexColor` (`.ocmat` `FLAGS vertexcolor=1`, written only when on) sets `MaterialFlag_VertexColor` (bit 19) in the packed
material constants (`AVER_MAT_VERTEX_COLOR` in `material_prelude.hlsl`). It means "multiply the albedo (rgb) and the opacity by the vertex colour".
It does nothing on a mesh without a colour stream, and nothing in any shader variant without `AVER_MESH_EXTRA`.

### Shaders

`AVER_MESH_EXTRA` (compile-time, defaults to 0) in `shared_prelude.hlsl` / `material_prelude.hlsl`:

- `VSIn` gains `col : COLOR0`, `tng : TANGENT0`, `uv2 : TEXCOORD1` AFTER the three engine attributes (Vulkan locations are positional, so the order
  is part of the contract and is mirrored by the vertex layout `VoxiRenderer::createExtraScenePipelines` declares); `VSOut` gains `vcol`, `tanWS`,
  `uv2`. `VSMain` carries the colour and UV2 through and transforms the tangent by the world matrix as a direction, folding the world matrix's
  mirroring (negative determinant) into the handedness. `AverVertex` carries `color`, `tangentWS`, `uv2` (set by `averVertexOf`; the hand-built vertices of
  ray hits and voxelisation leave them unset and never reach the code that reads them).
- `averEvalMaterial` (stock path): `baseColor *= color.rgb; opacity *= color.a` under `AVER_MAT_VERTEX_COLOR`.
- `averBuildSurface`: `averPerturbNormalAuthored` builds the normal-map frame from the vertex tangent (`T` orthogonalised against the geometric normal,
  `B = cross(N_geo, T) * w`, applied to the face-forwarded normal) wherever `w != 0`; the derived frame is evaluated unconditionally (its
  derivatives need uniform control flow) and wins for "no tangent" vertices and for world-aligned UVs.

Pipelines (`VoxiRenderer`): the first frame a mesh with the stream is drawn, `scenePipelineExtra` (a new `IRenderFeature` hook, default 0) marks it
wanted and the mesh draws as any mesh; `prePass` then compiles the `AVER_MESH_EXTRA` twins of `VSMain` / `PSMainVoxi` once: plain, G-buffer and their
depth-prepassed LessEqual twins, for the raster set or the ray-traced-raster set, whichever `rtActive_` says. Nothing is compiled while no such mesh exists. The D3D12 and
Vulkan devices ask for the twin when the mesh `hasExtra` and use it on the input-assembler path (a mesh-shader draw fetches `MeshVertex` only, so a mesh with
the stream takes the input-assembler path). `RENDER.MESHEXTRA 0` (`Settings::meshExtraStreams`) turns the twins off: the stream is then bound and ignored.
A pipeline whose layout reads slot 1 refuses (logs, skips) a draw of a mesh without the stream.
The depth prepass keeps its rule that the colour pass uses the SAME vertex shader as the depth pass (LessEqual needs bit-identical depth): a mesh with
the stream is prepassed through `VSMain` + `PSDepthPrepass` compiled with `AVER_MESH_EXTRA` (`IRenderFeature::depthPrepassPipelineExtra`, whose
cut-out multiplies in the vertex colour's alpha under the material flag, as the colour pass does), and `depthPrepassPipelineExtra` answers only while
the extra prepassed colour twin exists, so the two can never disagree. Editing `shared_prelude.hlsl` / `material_prelude.hlsl` / `voxi.hlsl` changes the
D3D12 shader blob cache keys, so the first run after this change recompiles every Voxi shader once.

RHI additions for the layout: `VertexAttrib::slot`, `VertexLayout::stride1`, `VertexSemantic::Tangent`, `Format::RGB32Float` (a vertex attribute
format, appended). `drawMesh` binds two vertex buffers for a mesh with the stream; a pipeline that does not read slot 1 ignores it.

### What does not see it (documented limits)

- Ray-traced paths read `MeshVertex` only: the bindless RT geometry table, hit shading, reflections, GI, the path tracer. RT hits do not see vertex colour,
  authored tangents or UV2 yet; a mesh whose colour is meant to show in reflections will not.
- **RAY-DRIVEN MODE IS A RT PATH.** With `RENDER.RTRENDERMODE 1` (the default from the Medium ray-tracing tier up, which is what an RT-capable card such as the
  RX 7800 XT runs) the first surface of every pixel is found by a ray (`PSRayDriven`) and the raster scene draws are suppressed, so a mesh's vertex colour
  and authored tangent do NOT show on its primary surface in that mode. They show in the rasteriser path: `RENDER.RTRENDERMODE 0`, the Low ray-tracing tier,
  ray tracing off, or Vulkan without ray queries. The headed checks below set `RENDER.RTRENDERMODE 0` for that reason.
- Blended (translucent) draws, the mesh-shader path, the fallback backend pipeline, the editor's wireframe and the depth prepass shader do not use the
  stream (the first two draw with the ordinary pipelines; the prepass writes the same depth because the position is identical).
- Material graphs get the authored tangent (they call `averBuildSurface`) but not the vertex colour multiply; a `VertexColor` graph node is not added.
- Skinned and soft-body meshes ignore the stream.
- `PSRayDriven` is untouched.

### Converter and runtime (Godot)

- `StandardMaterial3D` / `ORMMaterial3D` `vertex_color_use_as_albedo` is written as the `.ocmat` flag and rated converted; `vertex_color_is_srgb` is
  rated approximated (the colour is multiplied as stored, linear). `Aver.Fabric` `BaseMaterial3D` exposes `VertexColorUseAsAlbedo` and
  `VertexColorIsSrgb`.
- Imported model files keep their streams through `convertImportedModel`. The eight primitive mesh classes now carry generated tangents (Godot's
  `PrimitiveMesh` arrays have tangents); they have no colour and no UV2 (Godot adds UV2 only for `add_uv2`).
- Not done: a Godot `ArrayMesh` built from binary surfaces, `SurfaceTool`, `custom0..3` arrays, `add_uv2` lightmap UVs for primitives.

### Tests (all headless)

`MeshExtraTest` (71 checks; packing, tangent contract, `.ocmesh` old and new, glTF on `tests/fabric/data/ExtraGame/models/quad.gltf`, OBJ colour,
simplifier), `FabricMeshExtraTest` (33; the converter on `tests/fabric/data/ExtraGame`), `ProjectRenderApplyTest` (`RENDER.MESHEXTRA`),
`VoxiSpirvCompileTest` (29 variants: the six `mesh extra` ones, VSMain / PSDepthPrepass / PSMainVoxi raster, G-buffer and ray-traced, beside default twins), `Aver.Fabric.Tests` ("material vertex colour"), and the unchanged
`MeshTest`, `GltfTest`, `MorphGltfTest`, `ImportTest`, `FbxImportTest`, `TrifactorTest`, `FabricConvertTest` (144).

---

## R2: omni and spot lights without ray tracing, on Vulkan, with shadow maps; light calibration

Status: code, converter, runtime and headless tests done; nothing here has run on a GPU (see the headed checks).

### Before and after

Before, an omni or spot `CLight` (and every emissive-lamp light) existed only in the D3D12 staged ray-driven pass: `buildLocalLights` published
the list only with ray tracing active and `computeInScenePass`, so with ray tracing off, below the RT tier and on Vulkan a converted Godot scene had
no lights but the sun. R2 adds a second, independent path for **point and spot `CLight`s**: a capped list shaded in the raster scene pixel
shader, with shadow maps from an atlas. Rectangle lights, IES profiles, cookies and the emissive-material lamps stay on the ray-traced path
only (the raster path counts and skips rectangles; it ignores IES and cookies). Directional `CLight`s were never read by the lamp renderer.

### When the path is live

`VoxiRenderer::localRasterWanted()`, decided every `prePass` after the acceleration structures are built and before the ray-traced list:

| `RENDER.LOCALRASTER` | `Settings::localLightsRaster` | Meaning |
|---|---|---|
| absent / `0` | 0, auto | live in frames the plain raster set draws (ray tracing off, no ray queries, Vulkan, Low tier with no TLAS) when at least one point or spot light exists. **With ray tracing active nothing changes**: the ray-traced list, pipelines and constants are today's. |
| `1` | 1, on | additionally live in a frame the rasteriser draws with ray tracing **active** (`RENDER.RTRENDERMODE 0`, or any frame where `rayDrivenActive()` is false). The ray-traced light list is then NOT published (`buildLocalLights` skips it), so an emissive-material lamp has no light in that frame. Ray-driven frames (`RENDER.RTRENDERMODE 1`, the default from the Medium RT tier) keep the ray-traced list: PSRayDriven draws the primaries and is untouched. |
| `2` | 2, off | never; exactly today's behaviour. |

`RENDER.LOCALSHADOWS 0` (`Settings::localLightShadows`, default on) draws the lights without shadow maps. Both keys are owned keys of the `.ocproject`
(`ProjectDesc::localRaster` / `localShadows`, `-1` = unstated; capture writes them only when they differ from the defaults or were already stated).

No scene light, no change: with no point or spot light in the scene nothing is compiled, `sceneConstants()` returns the engine block alone and every
pipeline is today's.

### Pipelines and the constant block

`AVER_LOCAL_RASTER` (compile-time, default 0, in `voxi.hlsl`) is read by the **pixel shader only**. `createLocalRasterPipelines(rt)` compiles, the first
frame the path is wanted, the AVER_LOCAL_RASTER twins of `PSMainVoxi`: plain, G-buffer and their depth-prepassed LessEqual twins, for the plain raster set
(`rt` false) and, only for the opt-in, the ray-traced raster set. `VSMain` is the ordinary one, so a prepassed draw still matches depth. A mesh that carries
the R1 second stream gets `extraSceneLocal_` twins (compiled the frame after such a mesh draws while the path is live). **Not covered, and drawn with
the ordinary pipelines (no raster lights on them):** blended (glass, water) draws, mesh-shader draws, the editor wireframe. `PSRayDriven`, the
`AVER_RD_SINGLE_PASS` variants and the shadow-ray code are untouched.

The lights reach the shader as a **tail of cbuffer `VoxiFrame` (b4)**, present only in the AVER_LOCAL_RASTER variants: `rl::Block` (`RasterLights.hpp`),
`gRlHeader`, `gRlLight[32 * 7]`, `gRlTileVP[24]`, `gRlTileUv[24]`, 5520 bytes after the engine's 752. While the path is live `VoxiRenderer::sceneConstants()` hands the
device `cb_` followed by the block as one buffer (`localCbBlob_`); otherwise it hands `cb_` alone. No new constant slot, no RHI change for the data. The
three mirrors of `FrameConstants` (VoxiRenderer.hpp, voxi.hlsl, voxi_gi.hlsli) are **unchanged**: the tail is outside them and exists in one variant of
one file. The constants `AVER_RL_MAX_LIGHTS 32`, `AVER_RL_LIGHT_VECS 7`, `AVER_RL_MAX_TILES 24` in `voxi.hlsl` mirror `rl::kMaxLights`, `kLightVecs`, `kMaxTiles` (`RasterLightsTest` compares the text).

One light is seven `float4`s (`voxi_locallights_raster.hlsli` header comment is the layout): position and emitter radius; colour times the irradiance at 1 m (engine
units, distance fade applied) and range; axis and spot flag; decay exponent, cos outer, cos inner (Aver curve) or 1 / angle attenuation (Godot), flags; specular scale
and shadow weight; first tile, tile count, constant bias (cm), normal bias (texels); world texel size per unit distance, filter radius, near, far. Flags: `kFlagGodot` 1,
`kFlagNegative` 2, `kFlagShadowed` 4, `kFlagCube` 8.

**The atlas is bound to t19**, the slot the ray-driven local-light history uses. The path is live only in frames PSRayDriven and the staged ray-driven passes do not run, so nothing reads that SRV
then: the pass binds the atlas there after `beginShadowHistory` and, when the path stops, gives the history's read half (or the placeholder) back; the staged passes rebind their own pair every frame they run.
No new binding-table slot was added: table 0 stays at 25 SRVs / 27 UAVs, so no pipeline layout, material table base or Vulkan set layout moved.

### The light list (`rl::build`)

Culling, cap, ranking and the shadow budget are CPU-side and tested (`RasterLightsTest`):

1. rectangles skipped (counted); lights whose distance fade is 0 and lights with no intensity dropped; the range sphere against the camera frustum (planes from the row-vector
   view-projection, depth [0,1]);
2. ranked by intensity at 1 m times colour times fade over squared distance (floored at 50 cm), stable; the first **32** kept, the rest counted as `overCap`;
3. shadow tiles in rank order for lights with the cast-shadows switch on and a non-zero shadow fade: **at most 8 shadowed lights and 24 tiles** per frame. A spot is one tile; an omni, or a spot
   wider than 75 degrees, is a cube of six face tiles. The atlas is 4096 x 4096 depth (R32, 64 MB, created the first frame a tile is needed) in a 16 x 16 grid of 256 px cells; the first two
   shadowed lights get the large size (spot 1024, cube face 512), the rest the next size down (spot 512, face 256). `AtlasPacker` places squares on multiples of their size, first fit. A light
   that does not fit is still lit, without a shadow (`shadowDenied`).

`localShadowPass` renders each tile with the **existing** depth-only shadow pipelines (`shadowInstancedPso_` / `shadowPso_`, `VSShadow`): the tile's matrix goes into
`cascadeViewProj[0]` of a copy of the frame block, so the sun's cascade matrices in `cb_` are not disturbed. Draws are culled by bounding sphere against the light's range;
translucent and no-shadow draws are skipped, as in the sun pass. The shader samples a 3x3 PCF with the comparison sampler s1, after moving the receiver along its normal
(`shadow_normal_bias` texels) and toward the light (`shadow_bias` x 10 cm); the filter radius is `1 + min(light_size cm / 10, 3)` texels. Soft shadows from `light_size` are an approximation:
a wider filter, not a penumbra.

### Godot's curves and parameters

Lines are from Godot's `scene_forward_lights_inc.glsl` and `light_storage.cpp` (4.4, MIT, read for behaviour; nothing copied):

- omni and spot distance: `get_omni_attenuation(d, 1/range, decay) = max(1 - d/range, 0) * max(d, 0.0001)^-decay`, `d` in metres; decay is `omni_attenuation` / `spot_attenuation` (default 1; 2 is inverse square).
  `rl::distanceFactor` is this with `d` in cm over 100; near the lamp it is clamped at the emitter radius (1 cm minimum) instead of Godot's 0.0001 m.
- spot cone: `scos = max(cos, cos(spot_angle)); rim = max(1e-4, (1 - scos) / (1 - cos(spot_angle))); f = 1 - rim^(1 / spot_angle_attenuation)` (`rl::coneFactor`).
- energy: `light_energy * pi` for omni, spot and directional alike (non-physical units); diffuse carries `1/pi`.
- specular: `specular_amount = light_specular * 2`, multiplying only the specular terms.
- Aver lights (no `kLightGodot` flag) keep today's law exactly: `(1 m / d)^2 * (1 - (d/R)^4)^2` and the smoothstep cone between the inner and outer cosines, decay 2.
- Not implemented: Godot's `light_projector`, `shadow_blur` (read as `light_size`), `omni_shadow_mode` (always cube), `shadow_reverse_cull_face`, per-light cull masks, a baked mode, `light_indirect_energy`
  (local lights give no indirect light on either path), `light_volumetric_fog_energy`.

### Calibration: Godot energy 1 against Aver's physical units

Godot (non-physical units) multiplies a light's energy by pi and its diffuse BRDF carries 1/pi, so a white surface facing a light of energy E returns E. Aver's diffuse BRDF carries 1/pi as well,
and its lights are irradiance in engine radiance units, one unit being `kLuminanceToCdm2 = 100000/3` cd/m^2 (so the 100000 lux sun is 3.0). To reproduce "returns E" the irradiance must be pi * E units:

- **sun** `lux = energy * pi * 100000 / 3 = energy * 104719.755`;
- **omni and spot** `candela = energy * 104719.755`, because the irradiance at 1 m of a point source is `candela / (100000/3)` units and Godot's attenuation is relative to 1 m (at 1 m with decay `k` Godot's factor is the window alone);
- a white-ish albedo 0.8 under an energy-1 omni at 1 m (decay 1, window 1 - 1/range) therefore returns `0.8 * (1 - 1/range)` in both engines, before Aver's exposure and tonemap, which are Aver's own.

This replaces the placeholder `Light3D.LuxPerEnergy = 100000` and the converter's `kLuxPerEnergy = 100000` (3.0 vs pi: 4.7 percent brighter, and the lamps used lux where they need candela at 1 m). The constant
lives in `aver/formats/GodotLight.hpp` (`kLuxPerEnergy`, `kCandelaPerEnergy`, plus the two documented curves as functions), is repeated in `Light3D` (`LuxPerEnergy`, `CandelaPerEnergy`) and the converter takes it
from the header; `RasterLightsTest`, `FabricLightsTest` and the managed "lights: calibration ..." test each pin it to 104719.755. A real Godot render was not available to compare pixel values: the calibration
is derived from the documented formulas and Godot's source above, not measured. The ray-traced path reads the same candela with Aver's inverse square, so an energy-1 omni is the same at 1 m and
differs with distance (Godot's default decay 1 is flatter): rated approximated.

### One sun

The renderers take the sun from the sky state alone: the level's `SUN` record (`LevelSky`, `Sky.SetSun*`). A `CLight` of kind 2 was never drawn (`makeWorldLight` refuses it). So the double sun was not a
double render, but the `DirectionalLight3D` node carried a second, inert component and a script changing `LightEnergy` changed nothing. Now:

- the **`SUN` record drives the sun**. The converter writes it from the first `DirectionalLight3D` in scene order (`lux = energy * 104719.755`, colour, direction, angular distance);
- `DirectionalLight3D` adds **no light component**. The first directional light to enter the tree is `DirectionalLight3D.Sun`; **after load**, changes to its energy and colour and every transform change are
  forwarded to `Sky.SetSunIntensity` / `SetSunColor` / `SetSunDirection` (the sun overrides: a request, released by `Sky.ClearOverrides()`; in the editor they apply during Play only). At load nothing is pushed (the record holds the same values);
- every further `DirectionalLight3D` is placed with its properties and lights nothing; the converter reports it (`approximated`: "only the first DirectionalLight3D ... is the sun").
`Sky.Set*` sets absolute values, so a day cycle that rotates the sun node needs no other code. Not forwarded: `light_angular_distance`, the shadow cascade settings.

### Vulkan

All of it is backend-neutral RHI except one thing. The long-standing Vulkan defect (docs/VULKAN.md "the shadow cascade map is never written") is that a depth-only draw issued from inside the scene's dynamic-rendering scope
joins that scope and lands in the scene's attachments, so the local-light atlas would have received nothing and the tile viewports (up to 4096 px) would have been drawn into the scene's depth buffer. R2 adds
`IRenderContext::setTargetsIsolated(bool)` (default no-op; D3D12 needs nothing): while on, `VulkanDevice::pushRenderScope(..., isolated)` suspends the scene scope (it resumes with LOAD on its next draw) and opens a
temporary scope over the targets named by `setRenderTargets`, which `popRenderScope` closes. The local-light shadow pass is the only caller; **the sun's and the GI-only cascade passes keep the old behaviour**, so Vulkan
images that do not use these lights do not change (the cascade defect itself is NOT fixed by this). Costs one begin/end rendering per tile and mesh group.
`VoxiSpirvCompileTest` compiles the AVER_LOCAL_RASTER pixel shader (plain, G-buffer, mesh extra, and the two ray-traced-raster rows) to SPIR-V.

### What was not done

- The shader was compiled to SPIR-V and, in the same test, to DXIL (`ps_6_0` for the plain rasters as the D3D12 backend does, `ps_6_5` for the ray-traced ones) by DXC with no device. `VoxiShaderCompileTest`, `PrecompiledShaderTest` and `ShaderIncludeResolveTest` create a device (WARP or the adapter) and were not run; the lazily built R2 variants are not among what `buildAllVariants` compiles anyway.
- Glass, water and other blended draws, mesh-shader draws, skinned and soft-body meshes' shadow casting beyond what the existing shadow pipelines already do, rectangle lights, IES and cookies on the raster path.
- The `LIGHT` record of the `.ocworld` and the editor's light panel do not carry the Godot block (`CLight::attenuation`, `spotAttenuation`, `specularScale`, `shadowBias`, `shadowNormalBias`, `fadeBeginCm`, `fadeLengthCm`,
  `fadeShadowCm`, flags `kLightGodot` / `kLightNegative`). They are set at run time through the component field ABI (`Aver.Fabric` does) and an editor save of a level drops them for plain lights; a converted level re-pushes them from its Props.
- Local lights give no indirect light on either path.
- The ray-traced path ignores `light_specular`, `light_negative`, the curves, the fade and the shadow biases.

### Tests (all headless)

`RasterLightsTest` (115 checks: layout, Godot curves against the documented formulas, calibration, fade, culling and the cap, packing, tile budget and atlas, tile matrices, the HLSL text), `SceneTest` (the new
`CLight` fields and their gather), `ProjectRenderApplyTest` (`RENDER.LOCALRASTER`, `RENDER.LOCALSHADOWS`), `FabricLightsTest` (24; the converter on `tests/fabric/data/LightsGame`: the SUN lux, light Props, one sun, the per-property
ratings), `FabricConvertTest` (144, the sun now 1.5 x 104719.755), `Aver.Fabric.Tests` ("lights: calibration and the values pushed to the engine", "lights: one sun, owned by the sky"),
`VoxiSpirvCompileTest` (34 SPIR-V variants, 5 new, plus a 7-variant DXIL pass for D3D12) and the unchanged `SceneLightTest`, `LevelLightsTest`, `LightTreeTest`, `FabricMeshExtraTest`.

---

## R3: Godot's tonemappers, display adjustments and the WorldEnvironment mapping

Status: code, converter, runtime and headless tests done; nothing here has run on a GPU (see the headed checks).

### What was added

| Piece | Where |
|---|---|
| Tonemap modes 3..7 (Godot linear, Reinhard, filmic, ACES, AgX) with a white point; brightness / contrast / saturation after the tonemap | `modules/rhi/shaders/color.hlsli` (behind `AVER_POST_TONEMAP_EXT`), `post.hlsl` `PSComposite`, CPU twin and reference `modules/rhi/include/aver/rhi/Tonemap.hpp` |
| `PostSettings::tonemapWhite`, `adjustBrightness`, `adjustContrast`, `adjustSaturation` | `RHI.hpp`; `PostCB::ext` (one row, 304 B) filled by `fillPostLens` |
| Selection of the composite variant | `rhi::postNeedsExtComposite(post)`; `D3D12Device::compositePsoFor`, `VulkanDevice::compositePsoFor` |
| Project keys | `RENDER.TONEMAP 0..7`, `RENDER.TONEMAPWHITE`, `RENDER.ADJUSTBRIGHTNESS`, `RENDER.ADJUSTCONTRAST`, `RENDER.ADJUSTSATURATION` (`OcProject`, applied by `rhi::applyProjectTonemapKeys` in both hosts; the editor's Project Settings > Post page shows them) |
| Level selection | post-volume keys `tonemap` (now 0..7), `tonemapwhite`, `adjbrightness`, `adjcontrast`, `adjsaturation`; the converter writes one infinite volume named `WorldEnvironment` |
| Panorama sky | `modules/formats/.../PanoramaSky.hpp`: a Radiance `.hdr` reader and `fitSkyDome` (zenith, horizon, ground albedo, blend exponent by elevation) |
| Godot parameter mapping | `modules/formats/.../GodotEnvironment.hpp` (tonemap offset, energy scaling, fog and height-fog units, depth of field) |
| Converter | `readEnvironment` in `tools/fabric/FabricScene.cpp`, `postKeysOf`, `FabricProject.cpp`, `environmentObject` in `FabricResources.cpp` |
| Runtime | `scripting/csharp/Aver.Fabric/Nodes/Environment3D.cs`: `WorldEnvironment`, `Environment`, `Sky`, `SkyMaterial`, `ProceduralSkyMaterial`, `PhysicalSkyMaterial`, `PanoramaSkyMaterial`, `CameraAttributes`, `CameraAttributesPractical`, `CameraAttributesPhysical`, `EnvironmentMapping` |

### The default path is unchanged

`PostSettings` defaults (`tonemap` 1, white 1, adjustments 1) select no new code: `postNeedsExtComposite` is false for tonemap 0..2 and for identity adjustments, so
the ordinary `PSComposite` permutations are compiled with exactly their old defines and used. The new code is in a SEPARATE composite compiled with
`AVER_POST_TONEMAP_EXT` (D3D12: `[bloom][autoExp]` with the lens defines; Vulkan: `[lut][bloom][autoExp]`), built lazily the first frame it is wanted; if its build fails
the ordinary composite is used and a warning is logged once. Inside the shaders, `averTonemap` and every other `color.hlsli` function are unchanged; the Godot
functions, `gPostExt` (the new `PostCB` row) and the changed `PSComposite` lines exist only under the define, so the preprocessed text of the ordinary variants is
what it was (`TonemapTest` pins the old function and line as text). Editing `color.hlsli` and `post.hlsl` changes the shader blob cache keys, so the first run recompiles
the post and the shared shaders once. Nothing was added to `PSRayDriven`, the `AVER_RD_SINGLE_PASS` variants, the RT shadow-ray code, `VoxiFrame` or `rhi::MeshVertex`.

### Order of the post chain, and what the adjustments are

`... exposure, bloom, grade (linear) -> tonemap -> gamma encode -> [Godot brightness, contrast, saturation] -> grading LUT -> colour vision -> grain`. Godot's `apply_bcs`
runs on the sRGB-encoded colour after the tonemap, so the adjustments are in display space (unlike Aver's own `saturation` / `contrast` fields, which are linear and
before the tonemap); the LUT, which Godot's `adjustment_color_correction` also is, stays after them. A Godot colour-correction texture is not converted (grade with a
post volume's `.cube`).

### The formulas (Godot 4.5 `tonemap.glsl`, read for behaviour; `Tonemap.hpp` is written out from them)

- Reinhard (extended): `c * (1 + c / white^2) / (1 + c)`. At Godot's default white 1 it is the identity (so a default Reinhard is a clipped linear, as in Godot).
- Filmic: Hable's curve with an exposure bias of 2 baked into A (0.22 x 4) and B (0.30 x 2), C 0.10, D 0.20, E 0.01, F 0.30; divided by the same curve at the white point.
- ACES: Stephen Hill's RRT+ODT fit with an exposure bias of 1.8 in the input matrix; the white point is scaled by the bias, run through the same rational curve and divides the result. (Aver's own `acesFittedTonemap` is the same fit with a gain of 2 and no white point.)
- AgX: sRGB to Rec.2020, the AgX inset, log2 encode over [-12.4739, 4.0261], the 6th-order sigmoid fit (0.021 x + 4.0111 x^2 - 25.682 x^3 + 70.359 x^4 - 74.778 x^5 + 27.069 x^6), `pow(., 2.4)`, the outset, Rec.2020 to sRGB; no white point.
- Linear: the colour; the display encode clips.
- Unverified against Godot's shader text: the fetched read of the shader gave constants and structure but not the matrices; the AgX matrices are the published
  three.js / Troy Sobotka set (each row sums to 1, so a neutral stays neutral, which `TonemapTest` checks), and the ACES white scaling by the bias is from the structure of
  Godot's code. A real Godot render to compare against was not available. The output encode is Aver's gamma 2.2, not Godot's sRGB piecewise curve.

### The WorldEnvironment mapping

| Godot | Aver | Rating |
|---|---|---|
| `tonemap_mode`, `tonemap_exposure`, `tonemap_white` | `RENDER.TONEMAP` 3 + mode, `RENDER.EXPOSURE` (times the camera attributes' multiplier), `RENDER.TONEMAPWHITE`; and the post volume | converted |
| `adjustment_enabled` / `brightness` / `contrast` / `saturation` | `RENDER.ADJUST*`, post volume | converted |
| `adjustment_color_correction` | none | unsupported |
| no CameraAttributes (or practical without auto exposure) | `RENDER.AUTOEXPOSURE 0`: Godot's exposure is fixed | converted |
| `CameraAttributesPractical.auto_exposure_enabled` | `RENDER.AUTOEXPOSURE 1`; scale and speed not mapped | approximated |
| `CameraAttributesPractical.exposure_multiplier` | multiplies the exposure | converted |
| Practical DOF (`dof_blur_far/near_*`, `dof_blur_amount`) | Aver's physical lens focused at the far (else near) distance, f-stop `0.4 / amount` | approximated |
| `CameraAttributesPhysical` focus distance, focal length, aperture | the physical lens straight across; the physical exposure (aperture, shutter, ISO) is not turned into an exposure | converted / approximated |
| `glow_enabled`, `glow_intensity`, `glow_hdr_threshold` | `bloom`, `bloomthreshold` (when stated); levels, strength, bloom and blend mode not modelled | approximated |
| `background_mode` colour / clear colour | an authored dome of one colour (times `background_energy_multiplier`) | approximated |
| `ProceduralSkyMaterial` | the authored dome: top and horizon colours times `sky_energy_multiplier`, ground horizon colour times `ground_energy_multiplier` as the ground albedo; curves, sun disc, cover textures not modelled | approximated |
| `PhysicalSkyMaterial` | Aver's physical sky: `mie_coefficient x turbidity / 0.05` scales Mie scatter and extinction, `ground_color` the ground albedo, `energy_multiplier` the sky light | approximated |
| `PanoramaSkyMaterial` | the panorama fitted onto the dome, see below | approximated |
| `sky_rotation`, `sky_custom_fov` | none: the dome depends on elevation only | approximated |
| `ambient_light_*` | the sky light multiplier (as before) | approximated |
| `fog_enabled`, `fog_density`, `fog_light_color` | `FOG` density per metre / 100 per cm, colour | approximated |
| `fog_mode` depth, `fog_depth_begin` / `end` | fog start and a density reaching about 95% at the end | approximated |
| `fog_height`, `fog_height_density` | fog height and falloff per cm | approximated |
| `volumetric_fog_enabled` | distance fog at `volumetric_fog_density` | approximated |
| `ssao_*`, `ssr_*`, `ssil_*`, `sdfgi_*` | none; Aver's ray-traced GI, AO and reflections cover them when ray tracing is on | unsupported |

### Panorama sky: what it is and is not

The renderers do NOT sample the HDRI. The ray-driven pass (`PSRayDriven`, the default from the Medium ray-tracing tier) draws the primary sky itself and may not be
touched, the rays that miss call `skyColor`, and the rasteriser's `PSky` is a fixed device pipeline without a texture slot on either backend. So the panorama is
read at conversion time (`.hdr` Radiance, or an 8-bit image as sRGB; `.exr` is not read and falls back to the physical sky with a report line) and fitted onto the
authored dome: the zenith cap (above 60 degrees) and horizon band (0 to 15 degrees) averages, the lower hemisphere's mean over the upper's as the ground albedo, and the
blend exponent that best follows the luminance by elevation (a 2x2 solve per exponent, since the dome reaches its `horizon` colour only straight down). Its detail, its sun and
its rotation are lost; its brightness by height and its colour are kept, and every path (raster, ray-driven, reflections, GI ambient) sees the same dome. A real
panorama-sampling sky would need a texture slot in the device scene root signature and the Vulkan set layout, plus a variant of the ray-driven sky, and is the next step.

### Runtime (Aver.Fabric)

`WorldEnvironment.Environment` / `.CameraAttributes` and the resource classes carry Godot's property names and defaults. At load the level already holds the values (SKY, FOG,
the `WorldEnvironment` post volume), so entering the tree pushes nothing; a property changed afterwards is forwarded: the post keys to the script post layer
`fabric.environment` (`PostFx`), fog density / colour / shape and the sky light to the `Sky` overrides. There is no override for the sky dome's colours, so a runtime change
to the background or the sky material is stored but not shown. `Aver.Fabric.Environment` and `Aver.Fabric.Sky` shadow `System.Environment` and the framework's `Sky`
inside the assembly (the calls there are now qualified); converted scripts that name `Environment` get a `using` alias from the script converter like `Timer` does.

### Tests (all headless)

`TonemapTest` (191 checks: the curves against reference values in double precision, hand values, white-point behaviour, the display adjustments, which composite is selected, the
`PostCB` packing, the project keys, the HLSL text), `PanoramaSkyTest` (38: the `.hdr` reader flat and run-length, the dome fit including recovering a model dome's exponent, the
parameter mapping, the project keys), `FabricEnvTest` (45: the converter on `tests/fabric/data/EnvGame` and a generated panorama, and `LightsGame` for the unchanged default),
`FabricConvertTest` (144), `VoxiSpirvCompileTest` (39 variants; 5 post-composite rows to SPIR-V and 2 to DXIL), `Aver.Fabric.Tests` (`EnvironmentTests`, and the converted
`ConvertGame` scene's WorldEnvironment; 2280 checks), and the unchanged `PostLensTest`, `ColourVisionTest`, `PostVolumesTest`, `CubeLutTest`, `FormatTest`, `FabricLightsTest`,
`FabricMeshExtraTest`, `FabricE2ETest`, `FabricImportTest`, `FrameworkWorldFxAbiTest`, `ProjectRenderApplyTest`, `RasterLightsTest`, `MeshExtraTest`.
`VoxiShaderCompileTest`, `PrecompiledShaderTest`, `ShaderIncludeResolveTest` and `ColourVisionShaderTest` create a device and were not run.

---

## R4: camera projections, culling masks and visibility ranges

Status: code, converter, runtime and headless tests done; nothing here has run on a GPU (see the headed checks).

### The data (all appended, all zero = today's behaviour)

World::addComponent hands back zero-filled storage, so every mask is stored INVERTED (the layers it excludes; `aver/scene/RenderMasks.hpp`,
`storeMask` / `loadMask` / `masksOverlap`): 0 is "every layer", and a real mask of 0 is 0xFFFFF. Layers and masks are 20 bits, as Godot's.

| Component | New fields | Meaning |
|---|---|---|
| `CMeshRenderer` | `layersInv` | `VisualInstance3D.layers` |
| | `visBeginCm`, `visEndCm`, `visBeginMarginCm`, `visEndMarginCm`, `visFade` | `visibility_range_*` in cm (0 = no bound); the fade mode is carried but not drawn |
| | `rangeState` | runtime, written by the scene walk: the last verdict (the hysteresis memory) |
| `CLight` | `cullInv`, `shadowCasterInv` | `light_cull_mask`, `shadow_caster_mask` |
| `CCamera` | `flags` (`kCameraKeepWidth`, `kCameraDrivesView`), `projection` (0 perspective, 1 orthographic, 2 frustum), `sizeCm`, `offsetXCm`, `offsetYCm`, `hOffsetCm`, `vOffsetCm`, `cullInv` | `Camera3D` lens, offsets and cull mask |

`CCamera` is read for the lens only when `kCameraDrivesView` is set, and only on the entity that is the view node (`aver_fw_set_view_entity`; `Camera3D.MakeCurrent`
does that). Every other camera, every pawn view and every rig is exactly as before: the camera rig system reads `fovYRad` / `nearCm` / `farCm` of its own entity and
nothing else, and a rig that is active keeps overriding the field of view and clip planes (a rig and an orthographic camera node are not combined).

### Projection

`aver/core/CameraProjection.hpp` (header-only, pure): `CameraProjection {kind, fovRad, sizeCm, offsetXCm/YCm, keepWidth, nearCm, farCm}`, `projectionMatrix(p, aspect)`,
`perspectiveOffCenterLH`, `orthographicOffCenterLH`, `projectionHalfExtents`, `isOrthographicViewProj`, `projectToNdc`, `unprojectRay`. Godot's rules: `keep_aspect` names the
dimension `fov` / `size` describes (height by default); `size` is metres across the view for an orthogonal camera and across the window **at the near plane** for a frustum, whose
`frustum_offset` shifts that window; `h_offset` / `v_offset` slide the camera along its own right and up axes (a pose change, applied in `drivePlayCamera`, not a lens shift). A default
`CameraProjection` (perspective, keep height) builds `Mat4::perspectiveLH` itself, so the default camera is bit-identical (`CameraProjectionTest` compares the bytes).
`game::PlayView` carries the lens (`projection`, `sizeCm`, `offsetXCm/YCm`, `keepWidth`, `cullInv`); `pushCamera` is the one place the matrix is built, for the editor and the game.

What an orthographic or off-centre view means for each consumer of the projection:

| Consumer | Orthographic | Off-centre frustum |
|---|---|---|
| Raster scene (`VSMain`, `gViewProj`), frustum culling in `drawWorld`, shadow-cull spheres, TAA / AverSR jitter, reprojection, picking (`viewportRay` already unprojected both ends) | exact: they use the matrix, not a field of view | exact |
| Shadow cascade fit (`fitCascades`) | the corners come from the inverse matrix; the reach is 300 m (or the far plane) instead of 4000 x near | exact |
| Ray-driven primary pass (`PSRayDriven`) | **gated off**: `rayDrivenActive()` is false while the view-projection has no view depth in its w column (`isOrthographicViewProj`, refreshed at the top of `prePass`), so the rasteriser draws the primaries (`willSuppressSceneThisFrame` agrees). PSRayDriven is untouched | exact (rays through the apex) |
| Sky, aerial perspective, height fog march, froxel fog (`averViewRayDir` at the far plane) | **approximate**: the "view ray" of a pixel is the offset of its far-plane point, so the sky reads as one direction across the screen | exact rays, but the froxel grid assumes a symmetric frustum |
| Specular and ray-traced reflection / GI ray origins in raster mode (`V = camPos - position`) | **approximate**: the view vector converges on the eye instead of being parallel | exact |
| NRD2 / NeuRaC view basis (`tan` of a symmetric half fov), landscape LOD (`projScale` from `fovYRad`) | **approximate**: finite, plausible, not exact | approximate (symmetric assumption) |

The first orthographic frame logs `[Voxi] orthographic camera: the rasteriser draws the primary view ...`.

### Layers and cull masks

- **Camera `cull_mask`** (`DrawWorldOptions::cameraCullInv`, from `PlayView::cullInv`): an entity whose `layersInv` shares no layer with it is treated exactly like an owner-hidden mesh: no
  raster draw, but it is still submitted to Voxi's shadow cascades, GI and ray-traced geometry (`hiddenFromOwner` on the direct route; in ray-driven mode it is out of the primary view
  only). That is Godot's semantic: a camera filter, not a visibility switch. Both the depth-prepass walk and the colour walk set it, so they agree.
- **`light_cull_mask`**, raster local-light path only: `drawWorld` hands the instance's layers to the device (`IDevice::setDrawLayers`, a sticky, written to the per-object block's
  `_objPad` word on D3D12 and Vulkan, 0 unless an entity has layers) and `rlShade` skips a light whose inverted mask excludes them (record word `v4.z`). The ray-traced light list
  and the sun ignore it; Godot's own VoxelGI / SDFGI / LightmapGI ignore it too.
- **`shadow_caster_mask`**, raster local-light path: `Draw::layersInv` (`VoxiRenderer::setSubmitLayers`, set around both delivery routes) filters casters in the local-shadow pass
  (record word `v4.w`, read by the CPU). The sun's cascades and the ray-traced shadows ignore it.
- Only `drawWorld` knows layers. Foliage instances, instance sets, landscape, water, particles, decals and skinned/soft-body substitutes drawn by their own passes are on every layer.

### Visibility ranges

`drawWorld` skips an entity outside `[visBeginCm, visEndCm]` (`scene::rangeShown`): distance from the viewer (`DrawWorldOptions::viewerPos`, the host's eye) to the entity's world origin
(Godot measures to the instance's origin too, not its bounds). It is skipped outright, before any lookup: no draw, no shadow, no GI, no ray-traced geometry, as a hidden instance in Godot.
A margin is a hysteresis: a visible instance stays so until it has crossed a threshold by the margin, a hidden one reappears only after crossing back by the margin; the first verdict
uses the plain thresholds. The prepass and colour walks evaluate the same distance and give the same verdict (`CameraProjectionTest` checks the idempotence). **Fade modes self and
dependencies are a hard cut** (a dithered fade needs a per-draw alpha in the scene pixel shader; reported approximated); `visibility_parent` is not modelled. The editor's viewport
applies ranges from its own camera as well.

### Current camera

`Camera3D.MakeCurrent` / `Current` set the engine's view node; `ApplyCamera` pushes the lens and the cull mask. Godot makes the first camera current when none is marked, so the converter
adds `current = true` to the first `Camera3D` of a level in which no camera is marked (reported converted). The level's `CAMERA` record still comes from the current (else first) camera.
The runtime class does not promote a camera by itself.

### Converter and runtime

`Camera3D` keys: `projection` (perspective converted; orthogonal and frustum approximated, with the reasons above), `keep_aspect`, `fov`, `near`, `far`, `size`, `frustum_offset`,
`h_offset`, `v_offset`, `cull_mask`, `current` converted; `doppler_tracking` unsupported. `layers` and `visibility_range_*` converted (`visibility_range_fade_mode` other than disabled
approximated); `light_cull_mask` and `shadow_caster_mask` approximated (raster local lights only; was unsupported). `Aver.Fabric`: `Camera3D` (`Projection`, `KeepAspect`, `Size`,
`FrustumOffset`, `CullMask`, `HOffset`, `VOffset` push on change; Godot's projection / unprojection maths now cover frustum and the offsets), `VisualInstance3D.Layers`,
`GeometryInstance3D.VisibilityRange*`, `Light3D.LightCullMask` / `ShadowCasterMask`.

### Tests (all headless)

`CameraProjectionTest` (68: reference matrices, keep_aspect, frustum offsets, unprojection round trips for all three projections, `isOrthographicViewProj`, mask truth tables, the renderer's
copy of the mask rule against the scene's, visibility-range hysteresis and idempotence, the light record's mask packing, the shader text), `FabricCameraTest` (27; the converter on
`tests/fabric/data/CameraGame`), `SceneTest` (the new fields), `FabricLightsTest`, `FabricConvertTest`, `RasterLightsTest`, `Aver.Fabric.Tests` ("cameras: ...", "layers, visibility ranges and light
cull masks: ...").

### Not done

Dithered visibility fades, `visibility_parent`; layers for the non-`drawWorld` passes; masks on the ray-traced light and shadow paths; orthographic sky / fog / froxel / NRD2 basis; combining an
orthographic camera node with a camera rig; SubViewport, split screen and several simultaneous cameras (one view node drives the view; R5 adds secondary views, below).

---

## R5: SubViewport, ViewportTexture and several cameras

Status: code, converter, runtime and headless tests done; nothing here has run on a GPU (see the headed checks).

### The decision: a secondary view is NOT a second Voxi

The renderer's global illumination, ray-traced shadows, ReSTIR reservoirs, NRD2 history and exposure are one set of per-renderer state: the voxel GI volume and its cache, the
denoiser and ReSTIR histories, the auto-exposure histogram, the TAA / AverSR history and the G-buffer are all singletons of the frame. A second camera running through them would
write the main view's history with another view's pixels (the mistake `docs/ACTOR_EDITOR.md` section 3 ruled out for the actor preview, Option B). So a secondary view is the
**cheap raster configuration**, drawn by its own render feature into its own colour and depth targets before the scene pass, and the main view's frame is not touched:

| | main view | sub-viewport view |
|---|---|---|
| Pipelines | Voxi scene PSOs, ray-driven or raster | `ActorPreview` mesh pipeline with the new `SceneViewPS` (one pixel shader, sample count 1) |
| Lighting | Aver's: sun with cascaded or ray-traced shadows, local lights, GI, reflections, fog, sky | one sun and a hemisphere fill from the level's sky (`game::sceneViewLighting`); no shadows, no GI, no reflections, no fog |
| Materials | the full PBR model, textures, graphs | a flat colour per draw: the material's average base colour (`MaterialSystem::averageBaseColor`: factor times the mean of the map) plus its emissive factor; no textures, normal maps or graphs |
| Post | the whole chain | none: tonemapped (ACES) and gamma-encoded in the pixel shader, or linear (below) |
| Not drawn | | blended / translucent draws (glass, water as a material), terrain, foliage and instance sets, water surfaces, particles, decals, sky dome |

Why flat colours and not the material pipeline: the actor preview's material pipeline owns a second `pbr::MaterialSystem`, and `MaterialSystem::update()` *consumes* the library's
dirty flags ("this is the one consumer"). A second consumer would steal the main renderer's material updates; every view would also upload its own copy of every texture. The
average colour is what `MaterialSystem` already offers "for something that cannot sample a texture".

### The registry and the cost guard (`aver/core/SubViewport.hpp`, pure)

`SubViewRegistry` owns the rules; `modules/framework/src/SubViewAbi.cpp` holds the one process-wide instance behind `framework_subview_abi.h` (own version 1, the `framework_worldfx_abi.h`
shape: scripts publish, the host reads and draws each frame).

- **A view exists only because something created it** (`aver_fw_subview_create`, named by a KEY). With none, `SubViewportHost::update` returns before doing anything and no texture,
  pipeline or feature exists. A level without a `SubViewport` runs today's code.
- **Budget** (`SubViewBudget`): at most `RENDER.SUBVIEWPORTS` views alive (default 4, at most 16, `0` turns the feature off: every create is refused and the node logs one warning);
  at most `RENDER.SUBVIEWRENDERS` drawn per frame (default 2); the summed pixels of all views at most `RENDER.SUBVIEWPIXELS` megapixels (default 8): over it, every view shrinks by
  the same linear factor, aspect kept, floor 16 px, ceiling 4096 px a side. `scaling_3d_scale` multiplies the size first.
- **Update modes** use Godot's numbers (`SubViewport.UpdateMode`): disabled 0, once 1, when visible 2, when parent visible 3, always 4. *Once* draws one picture and then reads as disabled;
  setting once again requests another. *When visible* draws while something shows the texture: a material the converter found (the `_material_consumers` Prop), a
  `SubViewportContainer` on screen, or a script's `AddUiConsumer`. *When parent visible* follows `Node3D` / `CanvasItem` visibility of the ancestors. A view with no camera is never due (a
  *once* view does not spend its picture on nothing). `RequestRender` forces one picture whatever the mode.
- **Scheduling** (`plan`): the views due this frame, those still waiting for their first picture first, then the least recently drawn (round robin), at most `rendersPerFrame`. The
  rest wait; `aver_fw_subview_deferred` counts them. **Clear modes** (`always`, `never`, `once`): the first picture always clears; `never` and `once` then keep the picture.
- **Texture bookkeeping**: the host publishes the colour target (`aver_fw_subview_host_set_texture`); a rebuilt target bumps `generation`.

### The host (`game::SubViewportHost`, `Runtime/src/SubViewportHost.cpp`)

Called once a frame in **`onUpdate`, after the camera is pushed and before the device's `beginFrame`** (the views' features draw in `beginFrame`'s `prePass`, so draw lists and cameras
must be ready by then; there is no frame of lag for the draw list). Both hosts: `GameApp::onUpdate` and the editor's `SandboxApp::Update` (Play). For each view the registry plans:

1. the feature exists (`ActorPreview::create(device, w, h, studioChrome=false)`, registered as an ordinary `IRenderFeature`) and has the effective size (a resize waits for the GPU; do not animate a view's size);
2. the camera: `game::viewCameraOf` reads the camera entity's world matrix (row 3 the eye, row 0 forward, row 2 up, the play camera's own convention) and its `CCamera` (lens kind,
   fov, clip planes, size, offsets, cull mask, `h_offset` / `v_offset`) at the view's aspect;
3. the draw list: **`drawWorld` with the new `DrawWorldPass::View`**: the same entity walk, mesh lookup, part split, skinning, soft body and material ladder as the primary view
   (so there is one place that decides what an entity draws), culled by *this* camera's frustum and cull mask, delivered to `DrawWorldOptions::onViewDraw` and nowhere else: no
   device call, no Voxi submission, no `SceneDrawStats`, no `onEntityDelivered`, and the visibility-range hysteresis memory is read but not written (the primary walk owns it);
4. `SceneViewParams` (view-projection, eye, sun, fill, clear colour, clear flag, encoding) and `setEnabled(true)`. A feature the registry did not plan this frame is disabled and records nothing (its picture stays).

**Texture hand-out.** *Materials*: the converter writes `@rt:viewport.<key>` into the .ocmat texture slot; `MaterialSystem` resolves `@rt:` names before any host resolver
(`setRuntimeTexture`, not owned). The host sets the name to the colour target and `MaterialLibrary::touch()`es every material whose slots name it, so the next `MaterialSystem::update()`
rewrites their binding sets (also after a resize and on destroy, which clears the name and touches again). *Scripts / UI*: `aver_fw_subview_texture(id)` is the `rhi::TextureHandle`
widened to 64 bits, which is what `aver_ui_widget_set_texture` takes. *The screen*: `aver_fw_subview_set_screen_rect` puts a view over the window; the host appends one textured rectangle
per such view at the start of the frame's UI list (the HUD draws over it). `UiRenderer::dropBinding` is now public and is called with a texture just before it is rebuilt or destroyed.

**Colour encoding.** A target is RGBA8 UNORM. A view that only materials read stores LINEAR values (a material samples its albedo as linear and the frame's own tonemap and gamma follow);
a view the UI shows or that is on screen stores tonemapped, gamma-encoded display values. When both read it, display values win (a material then shows it brighter than a linear
view would). `transparent_bg` clears to (0, 0, 0, 0), premultiplied for the UI.

### The framework ABI (`framework_subview_abi.h`)

Script side: `create(key, w, h)` (idempotent per key; 0 = refused), `destroy`, `find`, `set_size`, `set_update`, `set_clear`, `set_transparent`, `set_own_world`, `set_camera(entity)`,
`set_scale(scale, msaa)`, `set_parent_visible`, `set_clear_color`, `consumer(kind, +-1)`, `request_render`, `set_screen_rect`, `info`, `texture`. Host side: `configure`, `count`, `id_at`,
`plan`, `host_info`, `screen`, `key`, `host_set_texture`, `refused`, `deferred`, `reset`, `tick` (the first tick after play state falls back to `AVER_FW_PLAY_EDITOR` drops every view).
C#: `Aver.Framework.SubViews` (`SubViewUpdate`, `SubViewClear`, `SubViewInfo`; the two enums are checked against the header by `AbiEnumTest`).

### Project keys

`RENDER.SUBVIEWPORTS`, `RENDER.SUBVIEWRENDERS`, `RENDER.SUBVIEWPIXELS` (`ProjectDesc::subViewports` / `subViewRenders` / `subViewPixels`; unstated = -1 = the defaults; owned keys of the
`.ocproject`). Read by both hosts when the project opens (`game::subViewportSettingsOf`); they are manifest keys, not `voxi::Settings`.

### Converter and runtime (Godot)

- `SubViewport` is placed as `SubViewport` (rated approximated, with the reason above) with Godot's own properties as Props and two private ones: `_viewport_key`
  (`<level>:<path from the level's root>`, e.g. `main:Main/Hud/MapView`) and `_material_consumers` (how many materials the converter found that show it, written back after the level is
  emitted because the view usually comes before the mesh). `SubViewportContainer` is placed as `SubViewportContainer`.
- **ViewportTexture**: in a `StandardMaterial3D` / `ORMMaterial3D` `albedo_texture` or `emission_texture` it becomes `ImportedTexture::runtimePath` = `@rt:viewport.<key>` and the cook writes it into the
  slot unchanged (no image copied). The path is resolved against the root of the scene the *using node* was authored in (for a node of an instanced scene, the instance), so two instances
  of one scene get two views and two materials (the key is part of the material cache key). An unresolvable path, or one naming a node that is not a SubViewport, is reported
  unsupported and leaves the slot empty. **Unshaded** (`shading_mode` 0) additionally writes the picture as the emissive map at factor 1 (approximated: it is still lit). A
  `ViewportTexture` held as a node or script value is kept as `Object(ViewportTexture, "viewport_path": NodePath(...))`.
- **Cameras**: a `Camera3D` below a SubViewport is the view's camera and is excluded from the level's `CAMERA` record and from the "first camera becomes current" rule. Its `current` is
  written as the private `_viewport_current` so that applying the property before the node is in the tree cannot take the window's view; at run time `Camera3D.MakeCurrent()` inside a
  SubViewport routes to the viewport (`Camera3D.OwningSubViewport`), and with none marked the first camera below (tree order) draws the view.
- Ratings: `size`, `render_target_update_mode`, `render_target_clear_mode`, `transparent_bg`, `scaling_3d_scale`, container `stretch` / `stretch_shrink` converted; `own_world_3d`, `msaa_3d`,
  `scaling_3d_mode`, `disable_3d` (no camera given) and the shadow-atlas / AA options approximated; `world_3d`, GUI, input, picking, 2D, audio-listener and debug-draw options unsupported.
- `Aver.Fabric`: `SubViewport` (Godot's properties and enums; `GetTexture()` gives the same `ViewportTexture` every time; `RequestRender()`), `ViewportTexture` (`ViewportPath`, `GetSize()`,
  `NativeHandle`, `Generation`, `MaterialReference`), `SubViewportContainer` (`Stretch`, `StretchShrink`, `GetGlobalRect()`; its rectangle is its `Position` / `Size`, else what its anchors and
  offsets make of the parent's size or the window's), `Viewport.GetTexture()` (null for the window), `Texture2D.GetSize()`, `BaseMaterial3D.AlbedoTexture` / `EmissionTexture` (stored only), and
  `Node.GetViewport()` now returns the nearest viewport above a node. A view is made when the node enters the tree and destroyed when it leaves; without an engine host nothing is made.

### What does not see it (documented limits)

- A secondary view has no global illumination, shadows, reflections, fog, sky, post, blended draws, terrain, foliage, water, particles or decals; materials are flat colours. It is a
  security-camera / minimap / portrait picture, not a second Aver image.
- `own_world_3d` and a separate `World3D` are not supported (one world). Input is not forwarded into a view. Audio listeners are not per view.
- Several simultaneous views cost: one world walk on the CPU and one draw per visible mesh on the GPU per view per frame, plus 5 MB of targets per 1280 x 720 view. Resizing a view
  waits for the GPU. The budget exists for these reasons.
- The main renderer's modes (ray-driven, Vulkan, Low tier) are irrelevant to a view: it is always the raster feature, on D3D12 and on Vulkan.
- A view drawn from a camera whose entity is destroyed keeps its last picture and logs once.
- The GPU side was written against a recording device and DXC (SPIR-V and DXIL compile of `SceneViewPS`); no device ran it.

### Tests (all headless)

`SubViewportTest` (the registry: numbers, create / key / budget, sizes and the pixel cap, every update mode, the per-frame budget and round robin, clear modes, encoding, texture generations), `FrameworkSubViewAbiTest`
(the C ABI through the DLL), `SubViewportHostTest` (the camera entity's view, the lighting map, the settings clamp, **`DrawWorldPass::View` against the primary walk** — frustum, cull mask, visibility range, hidden
flag, no device calls, no counters, hysteresis untouched — and `SubViewportHost` end to end against a recording device: nothing created without a view, planning, textures to scripts and materials, resize,
budget, on-screen list, destroy), `ActorPreviewTest` (the scene-view pass: no chrome, clear flags, camera block, per-draw block, one extra pipeline, `studioChrome=false`), `VoxiSpirvCompileTest` (`SceneViewPS`
and its studio twin to SPIR-V, and to DXIL for D3D12), `ProjectRenderApplyTest` (the three keys), `AbiEnumTest` (update / clear modes), `FabricViewportTest` (the converter on `tests/fabric/data/ViewportGame`),
`Aver.Fabric.Tests` ("sub-viewport: ..." 8 cases).

---

## R6: StandardMaterial3D / ORMMaterial3D features

Status: code, converter, runtime and headless tests done; nothing here has run on a GPU (see the headed checks).

### The shape of it

A material gets a `MaterialFx` (`modules/render.pbr/include/aver/pbr/MaterialFx.hpp`, a member `MaterialDesc::fx`). Everything in it defaults to off. A material whose `fx.active()` is
false is every material before R6: `packMaterial` produces the same 192-byte `MaterialConstants`, the draw goes through the same pipelines and the shaders compile the same code.
A material with fx instead:

1. gets `MaterialFlag_Fx` (bit 20) in its flags;
2. has a **64-byte `MaterialExtConstants`** appended to its b2 draw constants (192 + 64 = 256 = `rhi::kMaxDrawConstantBytes`), packed by `packMaterialFx`; `MaterialSystem::constantBytes(h)` is 192 or
   256 and the three hosts' draws (`drawWorld`, the editor's placeholder loop, the level walk's `rs.matBytes`) hand the device that many bytes. The ray-traced material table, GI, the voxeliser
   and everything else keyed on `MaterialConstants` read the first 192 bytes and never see the tail (`VoxiRenderer::submit` already clamped to 192);
3. is routed by a **24-bit variant key** (`fxVariantKey`; dword 0 of the tail is `(render priority + 128) | key << 8`, which the RHI reads through `rhi::drawMaterialFxKey` / `Priority`). Key 0 means
   "no pipeline change". The key names the compile-time pixel features (`fxkey::kUvx`, `kTri`, `kRim`, `kAniso`, `kParallax`, `kBacklight`, `kDetail`, `kSurf`, `kModes`, `kHash`, `kFade`,
   `kUnshaded`), the late-Z switch (`kLateZ`), the vertex stage (`kVertex`) and the fixed-function state (cull 2 bits, blend 3 bits, depth draw 2 bits, no depth test).

Why a tail and not a bigger block: the 192-byte block is mirrored in `material_prelude.hlsl`, `voxi_rt.hlsli`'s `RtMaterial`, `VoxiShaders.hpp` and the layout tests, and the ray-traced
material table is an array of it; growing it would change every scene's RT stride. A tail exists only for materials that use fx.

### Pipelines (Voxi, D3D12 and Vulkan)

`IRenderFeature::scenePipelineFx(key, meshExtra, blended)` (default 0) is asked by both devices' `drawMesh` (opaque path, never for a prepassed draw, input-assembler path) and by the blended
replay (per draw: the pipeline can change between two translucent draws). `VoxiRenderer` answers from `fxScenes_`: the first draw that asks marks the key wanted, `prePass` builds it
(`createFxPipelines`) the next frame, and until then and if the build fails the draw uses the ordinary pipeline. One entry per (key, ray-traced-raster set, second-vertex-stream mesh):
`lit`, `litGbuf`, and `blended`. Built from the raster entry points only: `PSMainVoxi` compiled with `AVER_MATX=1` plus the key's `AVER_MATX_*` defines, and `VSMainFx` instead of `VSMain` when
the key has `kVertex`. Nothing is added to `PSRayDriven`, the `AVER_RD_SINGLE_PASS` variants or the shadow-ray code. Mesh-shader draws, the depth prepass, the G-buffer's blended
twin and the local-light twins have no fx variant (the raster local lights do not light an fx draw; documented limit). `RENDER.MATERIALFX 0` (`Settings::materialFx`) makes `scenePipelineFx`
answer 0 always: every material draws as before and the tail is ignored.

State from the key: cull (`FxCull::Back` = `CullMode::Back`, `Front`, else none, the engine's long-standing state), depth write on for opaque and off for translucent unless `depth_draw_mode` says
otherwise, depth test off for `no_depth_test`. Two blend states were added to `rhi::BlendMode` (appended, D3D12 and Vulkan): `Subtractive` (`dst - src`, REV_SUBTRACT) and `Multiplicative`
(`dst * src`); add is the existing `Additive`. Mix and premultiplied alpha are the engine's premultiplied blend (premult is approximated).

**Depth prepass and early Z.** An fx draw is neither in the frame-wide depth prepass nor in the per-draw depth-only path (`DrawLook::fx` in `drawWorld`): the prepass shaders disagree with a
variant that moves vertices, transforms UVs or discards by noise, and back-face culling would leave holes (the prepass culls nothing). `PSMainVoxi` forces early depth; a variant that discards
(alpha hash, fades, and any alpha-masked fx material) is compiled without it (`AVER_MATX_LATEZ`, the `AVER_EARLYZ` macro in `voxi.hlsl`). The default scene shader expands to the same
`[earlydepthstencil]`.

**Sort order.** `render_priority` sorts the translucent replay (priority first, then back to front); opaque draws have no order.

### Shaders

`material_prelude.hlsl`: the `AverMaterial` cbuffer gains 16 scalar dwords under `AVER_MATX` (scalars, so HLSL packs them as the C++ struct lays them out; `MaterialFxTest` compares the order),
and the stock `averEvalMaterial` is the `#else` of an fx one that adds, in this order: UV transform, parallax, the five maps (triplanar replaces the bound base maps), detail, the uv2
maps, vertex colour, emission multiply, distance fade, alpha hash, normal, composition and the mask clip, then the surface terms (ao light affect, anisotropy, rim, backlight, specular and
diffuse modes, unshaded). `voxi.hlsl`: `VSMainFx` (billboard, fixed size, grow), the late-Z macro, disable-receive-shadows after the material evaluation, the multiply blend's early return in the
translucent branch, and `gAverFxPixel` (SV_Position.xy for dither). Material graphs and RT hits never run any of it. The variants compile to SPIR-V and DXIL (`VoxiSpirvCompileTest`).

| Feature | What the shader does |
|---|---|
| uv1 scale/offset, uv2 scale/offset | `UV * scale + offset` before the maps are sampled; the uv2 maps (detail on UV2, AO and emission on UV2) use the R1 second vertex stream's UV2, else UV1 |
| uv1 triplanar (object or world), sharpness | object-space (or world) position in metres times `uv1_scale` plus offset; three projections of the bound base maps blended by `pow(abs(n), sharpness)`; normal map by the axis-swizzled UDN blend; the detail normal is not combined with it. uv2 triplanar: stored, unsupported |
| heightmap, deep parallax | offset mapping from the heightmap's R (the layer-1 metal-rough slot); deep = steep parallax with the layer count by view angle; scale/100 of the UV |
| detail albedo / normal / blend / uv layer | layer-1 base colour and normal slots, mix/add/sub/mul over the base; the mask texture is not read |
| rim, rim tint | `pow(1 - n.v, (1 - rough) * 16) * rim * mix(1, albedo, tint)` times the sun's radiance and visibility, added as glow (sun only) |
| backlight | `backlight * albedo * saturate(-n.l)` times the sun's radiance, added as glow |
| anisotropy | the lobe is widened (`rough *= 1 + 0.5 |a|`); not stretched along the tangent (the shared BRDF is isotropic) |
| emission operator multiply, emission/AO on UV2, ao_light_affect | emission times albedo; the maps sampled with UV2; `kdAlbedo *= lerp(1, ao, affect)` |
| specular disabled, diffuse lambert wrap | F0, F and the grazing term zeroed; the subsurface wrap term stands in for wrap. Toon specular/diffuse and Burley are the engine's lobes |
| disable ambient / receive shadows | `s.occlusion = 0`; `sun.visibility = 1` |
| alpha hash, alpha antialiasing | a stable noise threshold on an opaque surface; antialiasing compares it with a coverage that follows the alpha's screen derivative (no MSAA) |
| distance fade (alpha, pixel dither, object dither) | `saturate((d - min) / (max - min))` (reversed when min > max) as opacity (blended material) or a screen-door dither |
| unshaded | `s.display = true`: the albedo and glow reach the target (premultiplied when blended) |
| billboard (enabled, Y, particles' first frame), keep scale, fixed size, grow | `VSMainFx`: the mesh's axes (engine -X, Y, Z = Godot Z, X, Y) onto the camera's backward, right, up; offsets from the origin scale with view depth; vertices along the normal |
| blend add / sub / mul, depth draw, no depth test, cull | pipeline state from the key; multiply writes `lerp(1, albedo, alpha)` into a multiplicative blend |

Existing material fields carry the rest: clearcoat -> `coatWeight` / `coatRoughness` (the layered BSDF, `RENDER.LAYEREDBSDF`), subsurface scattering and its transmittance -> `subsurface*`,
refraction -> `transmission` 0.85 and `ior = 1 + 5 * refraction_scale`, `metallic_specular` -> `reflectance = 0.08 * value`, alpha scissor -> the masked mode with the threshold.

### `.ocmat` and project keys

One new record, `FX key=value ...` (FORMAT_SPECS 7), written only when `fx.active()` or the cull is stated; only values that differ from the defaults are written. Vectors are comma separated.
Keys: `uv1scale uv1offset uv2scale uv2offset uv1tri uv2tri uv1world uv2world uv1sharp uv2sharp cull(back|front|none) blend(add|sub|mul|mix|premult) depthdraw(always|never) nodepthtest priority
unshaded alphaext(hash|aa) hashscale shadowtoopacity billboard(on|y|particles) keepscale fixedsize grow growcm frames loop fade(alpha|dither|object) fademin fademax rim rimamount rimtint aniso
anisoamount height heightscale deepparallax layers flipt flipb invertheight backlight backlightcolor detail detailblend detailuv emitmul emituv2 aouv2 aoaffect spec(toon|off)
diffuse(lambert|wrap|toon) noambient noshadows`. An older reader skips the record; an older file loads unchanged. The records that were stored and never read are now wired up: `SHADER unlit`
= unshaded, `CULL front` = front cull, `BLEND additive` = the add blend; **`CULL back` stays what it always was** (the editor wrote it into every file and the engine drew without
culling), so back-face culling is `FX cull=back`. The detail albedo, heightmap and detail normal ride the layer-1 `TEX` slots of a material that is not slope blended.
`RENDER.MATERIALFX 0/1` (`ProjectDesc::materialFx`, `Settings::materialFx`, default 1, captured only when off) switches the variants.

### Converter and runtime

`materialAsset` (tools/fabric/FabricResources.cpp) builds the FX record text and the existing fields, `ImportedMaterial` carries `fxRecord`, `detailAlbedoTex`, `heightTex`, `detailNormalTex`,
`reflectance`, `coatWeight/Roughness`, `transmission`, `ior`, and `cookMaterials` applies them (`parseOcmatFx`). **Every converted material now carries `cull=back`** (Godot's default), so it draws
through a variant pipeline with back-face culling; the rating table is the test (`FabricMaterialFxTest`). Ratings:

- converted: uv1/uv2 scale and offset, uv1 triplanar (+ world, sharpness), cull_mode, transparency alpha/scissor, blend add/sub/mul, depth_draw_mode, no_depth_test, render_priority, shading_mode
  unshaded, disable_receive_shadows, billboard_mode (enabled, Y), billboard_keep_scale, fixed_size, grow + grow_amount, distance fade (pixel dither, object dither), heightmap, detail, emission operator,
  emission/AO on UV2, metallic_specular, specular disabled, diffuse lambert;
- approximated: alpha hash and antialiasing, alpha depth pre-pass, blend premult, billboard particles (first frame), distance fade pixel alpha on an opaque material, rim, anisotropy, backlight,
  clearcoat, refraction, subsurface (and its transmittance), deep parallax and heightmap_scale, diffuse burley/wrap/toon, specular toon, disable_ambient_light, ao_light_affect, per-vertex shading,
  texture_filter, texture_repeat, emission_intensity;
- unsupported (with the reason in the report): proximity fade, disable_fog, shadow_to_opacity, uv2 triplanar, the detail mask, the rim / clearcoat / anisotropy-flow / backlight / subsurface /
  refraction textures, depth_test inverted, next_pass, stencil, bent normals, MSDF, point size, z-clip scale and fov override.

`Aver.Fabric`: `BaseMaterial3D` has every property above under Godot's names and enums (`TransparencyEnum`, `CullModeEnum`, ...; metres), `FxRecord()` builds the record the converter writes, `Reflectance`,
and `ORMMaterial3D` exists (`OrmTexture`). The engine draws the baked `.ocmat`: a value changed at run time is stored and **not pushed** (as `AlbedoTexture`); there is no pbr ABI entry for fx.

### What does not see it (documented limits)

- Ray-traced hits (reflections, GI, the path tracer) and material graphs shade the base block only. **Ray-driven mode draws primaries with `PSRayDriven`, so fx do not appear there**: use
  `RENDER.RTRENDERMODE 0`, ray tracing off or Vulkan without ray queries (as R1). The headed checks set it.
- An fx draw is not in the depth prepass and not lit by the raster local lights (R2); its shadow and its GI contribution are the ordinary mesh's (a billboard or grown mesh casts the unmoved shape).
- Billboards are not tangent-correct (no authored tangent), the particle sheet shows frame 0, the node's scale is dropped without `billboard_keep_scale`.
- Alpha-to-coverage needs MSAA; the stand-in is a dither. Triplanar and parallax cost extra samples; the all-features variant is the heaviest pixel shader in the scene set.
- Mirrored instances (negative scale) flip winding, which a culling variant shows inside out.

### Tests (all headless)

`MaterialFxTest` (102 checks: inactive fx is today's block, the key per feature, the 64-byte tail dword by dword, half floats, the FX record round trip and the legacy wiring, the library's sanitising,
the shader text against the struct), `FabricMaterialFxTest` (47: the converter on `tests/fabric/data/MatGame`, every material and the report), `VoxiSpirvCompileTest` (21 new rows: each feature
group, all features, G-buffer, ray-traced raster, second stream, local lights, multiply, `VSMainFx` with and without the stream, each pixel row also to DXIL), `Aver.Fabric.Tests`
("material features: ..."), and the unchanged material and converter suites (`MaterialTest`, `OcMatTerrainTest`, `HairMaterialTest`, `FabricConvertTest`, ...).

---

## R7: geometry nodes and runtime mesh building

Status: code, converter, runtime and headless tests done; the GPU upload of a script-built mesh is unseen (see the headed checks). No shader, no pipeline, no constant block and no rhi type changed:
every R7 mesh reaches the renderer as an ordinary `.ocmesh`-shaped `OcMeshData` through the existing `GameContent::uploadMesh` path, so a project that uses none of it runs exactly today's code.

### The shape of it

| Godot | In the engine |
|---|---|
| `ArrayMesh`, `SurfaceTool`, `ImmediateMesh`, `MeshDataTool` (scripts) | Aver.Fabric keeps the surfaces in Godot numbers and hands each to the **runtime-mesh ABI** (`framework_rtmesh_abi.h`); an entity draws the mesh at the path `@rtmesh/<key>` |
| `MultiMesh` + `MultiMeshInstance3D` | an instance table in the same ABI; the engine **bakes the visible instances into one runtime mesh** (`fmt::bakeInstances`), at most once a frame |
| a stored `ArrayMesh` in a `.tscn` / `.tres` | the converter decodes Godot's packed surface into an `.ocmesh` (`modules/formats.godot/src/GodotMesh.cpp`) |
| `TubeTrailMesh`, `RibbonTrailMesh` | generated as static straight shapes (approximated); `TextMesh` unsupported |
| `CSG*` trees | baked at **convert time** with a BSP boolean (`modules/formats/src/Csg.cpp`) into one `.ocmesh` on the root; `use_collision` adds a static body child with the triangles |
| `GridMap` + `MeshLibrary` | the stored cells become one `MultiMeshInstance3D` child per used item and one static body with a shape per cell shape |
| `Sprite3D` | a quad `.ocmesh` (size, frame, flips, region baked into geometry and UVs) plus a baked `.ocmat` (modulate, billboard, alpha cut, sidedness, shading) |
| `Label3D` | the class exists with Godot's properties; **nothing is drawn** (no world-space text renderer) |

Godot's array and mesh conventions the code relies on, so a later package does not rediscover them: front faces are **clockwise** and the engine's axis map has determinant -1, so a Godot triangle list goes to the
engine **in the order it is stored** (the primitive generators write counter-clockwise triangles and `toOcMesh` reverses them to the same result); `generate_normals` therefore uses `cross(c - a, b - a)`.

### The runtime-mesh ABI (`modules/framework/include/aver/framework/framework_rtmesh_abi.h`, own version 1)

Same shape as the sub-viewport ABI (R5): the script publishes, the host reads. Engine space (centimetres, +Z up). All state is file-static in `RuntimeMeshAbi.cpp`, single game thread.

- `aver_fw_rtmesh_create(key)` / `destroy` / `find` / `clear` / `add_surface(id, vertexCount, pos, nrm, uv, col, uv1, tan, indexCount, idx, material)` / `info` / `object_id` / `key`. A mesh is named by a key; the id a
  mesh renderer holds is `fnv1a64("@rtmesh/" + key)`, exactly how every asset id is made, so `Entity.SetMesh("@rtmesh/<key>")` needs nothing new. Streams follow R1 (colour, UV1, tangent with handedness; absent = null);
  absent normals are computed (area-weighted smooth); `assembleSurfaces` pads a stream only some surfaces have (white / zero / "no tangent"), one submesh and one material slot per surface.
- `aver_fw_multimesh_create(key)` / `destroy` / `set_source(path)` / `set_flags(colors, custom)` / `set_count` / `set_visible(n | -1)` / `set_transform(i, t12)` / `set_color` / `set_custom` / `set_buffer` / `info` / `mesh`.
  `t12` is the `.ocinst` row-vector matrix with its 4th column dropped. The source is a content-relative `.ocmesh` (read from `aver_fw_content_dir()`, never a path that climbs out or names a drive) or `@rtmesh/<key>`.
  The baked mesh is the runtime mesh `multimesh/<key>`. A per-instance colour **multiplies the mesh's vertex colour** (the material needs the R1 `vertexcolor` flag, as Godot needs `vertex_color_use_as_albedo`);
  custom data x and y become the second UV set (z and w are not carried). A mirrored instance is re-wound. Normals use the inverse transpose, tangents turn with the instance and flip handedness under a mirror.
- Host side: `aver_fw_rtmesh_pump()` (bakes the changed MultiMeshes; also drops everything when a play session ends), `take_dirty(ids, max)`, `take_removed(objectIds, max)`, `view(id, &AverFwRtMeshView)` (pointers to the built arrays and
  the submesh table), `configure(maxMeshes, maxVertices)`, `count`, `reset`. Defaults: 256 meshes, 2,000,000 vertices per mesh (a MultiMesh lowers its visible count to fit and logs once; `info` says how many were drawn).
  No project key sets these yet (`aver_fw_rtmesh_configure` is exported and no host calls it).

Host glue (unrun): `GameContent::syncRuntimeMeshes(device)` (Runtime/src/GameContent.cpp) pumps, unloads what was removed (`unloadToPending`), and for each dirty mesh builds an `OcMeshData` from the view and uploads it with
`uploadMesh` under the object id (an update replaces the mesh: the old handles wait in the release queue). It runs with `lazyLoading_` set, so the same hooks that build ray-traced geometry for a streamed mesh run for it.
It is called once a frame from `GameApp` (before the sub-viewport update) and from `SandboxApp::onUpdate` (the editor's update, before its sub-viewport update); with nothing in the registry it does nothing.

### Decoding Godot's packed ArrayMesh (`GodotMesh.hpp`)

`decodeArrayMeshSurface` reads a surface dictionary (`format`, `vertex_count`, `vertex_data`, `attribute_data`, `skin_data`, `index_data`, `index_count`, `primitive`, `aabb`, `uv_scale`) the way Godot 4.4 stores it
(`mesh_surface_make_offsets_from_format`, `_get_array_from_surface`; read for behaviour). Format bits: vertex 1, normal 2, tangent 4, colour 8, UV 16, UV2 32, custom 0..3 at bits 6..9 (type in 3 bits at shifts 13, 16, 19, 22),
bones 1024, weights 2048, index 4096; flags 2D vertices bit 25, 8 bone weights bit 27, compress attributes bit 29; format version in bit 35 and up (4.2+ writes 1).

- `vertex_data` = all positions (3 floats; 2 floats with 2D vertices; four u16 relative to the AABB with compressed attributes, the fourth being the tangent-frame angle), then the normal / tangent block
  (normal = octahedral 2 x u16; tangent = octahedral 2 x u16 with the handedness in the half of the second component; with compressed attributes the "normal" word is the rotation axis of the frame and there is no tangent word).
- `attribute_data`, interleaved per vertex in the order colour (4 x u8), UV, UV2 (2 floats, or 2 x u16 compressed with `uv_scale`), then custom 0..3 by their format (skipped by size, **not decoded**).
- `skin_data`: per vertex 4 (or 8) u16 bones then the same number of u16 weights. Decoded, then **dropped** by the converter (the mesh is drawn static; reported approximated).
- Indices are u16 up to 65536 vertices, else u32. A surface with no index flag draws the vertices in order. A strip is expanded to triangles (noted). Points and lines are refused. LODs and blend shapes are noted and ignored.
- Not covered: Godot 4.0 / 4.1 surfaces (no version bit) are read with the current layout and the surface says so; the compressed normal / tangent reading is checked against hand-computed rotations only (the encoder is not reimplemented).
- The converter writes colours as stored, UV2 as UV1, and an authored tangent through the glTF rule (`packTangent`); with no authored tangent it generates them from the UVs.

### CSG (`Csg.hpp`) and what the converter does with it

`csg::combine(a, b, op)` is the classic BSP construction (split polygons by a node's plane, clip each solid against the other, invert for the cut). Tolerance 1e-5 m, polygons convex, vertices carry a normal, a UV and
the polygon's material index. The converter folds a root CSG node's children **in order**: `result = own geometry; for each CSG child: result = combine(result, child transformed by its local transform, child.operation)`.
`invert_faces` flips a node's own faces before it is combined. Primitives: box, sphere (`radial_segments` around, `rings + 1` bands), cylinder / cone (`sides`), torus (`sides` per cross-section, `ring_sides` around the axis),
polygon extrusion (depth mode, from z = 0 to z = -depth) and spin (`spin_degrees`, `spin_sides`), `CSGMesh3D` (a converted mesh read back into Godot space). Path extrusion is **not baked** (reported). `snap` snaps vertices to the grid
(default 1 mm); `calculate_tangents` generates tangents; materials per primitive become the mesh's material slots. Facet counts follow Godot's parameters but the triangulation is not Godot's.
The root node keeps its class (`CSGCombiner3D`, `CSGBox3D`, ...) and gets `mesh`; Aver.Fabric's `CSGShape3D` derives from `MeshInstance3D`, so the root draws that mesh. The CSG nodes below a root are **not placed** (reported "folded").
`use_collision` adds a child `StaticBody3D` named `_CSGCollision` (layer and mask from the CSG node) with one `CollisionShape3D` holding the baked triangles as a `ConcavePolygonShape3D`.

### GridMap

Cells come from `data["cells"]`: three ints per cell, key `x | y << 16` (signed 16 bits each), `z` (signed 16 bits), value `item | orientation << 16 | layer << 21` (item `0xFFFF` = empty). `gdGridBasis` is Godot's table of the 24
orthogonal bases (`Basis(xx, xy, xz, yx, ...)`, rows as Godot lists them; the test checks distinctness, orthonormality and determinant +1, and pins a few indices). Cell transform = `Transform3D(basis * cell_scale, cell * cell_size + offset)`
with `offset = cell_size / 2` per axis where `cell_center_*` is on (the defaults), times the item's `mesh_transform`; a shape's world transform is the cell transform times the shape's local transform. One `MultiMeshInstance3D`
child `Item<id>_<name>` per used item; one `_GridCollision` body (`collision_layer`/`mask` of the GridMap) with `Shape<k>` children, at most 4096 (more are dropped and reported). Octants, navigation and
`set_cell_item` redrawing are not modelled: `GridMap.SetCellItem` at run time changes the stored cells and the query methods only.

### Sprite3D and Label3D

The converter reads the texture size from the PNG header (`IHDR`); any other format falls back to 64 x 64 (reported). Quad size = frame size (region, divided by `hframes` / `vframes`) times `pixel_size`, centred unless `centered` is off, shifted by `offset`;
`flip_h` / `flip_v` swap the UV ranges; `axis` X / Y turns the quad (approximated). The material is a `StandardMaterial3D` built from `modulate`, `transparent` + `alpha_cut` (discard = alpha scissor, hash, opaque pre-pass), `shaded`, `double_sided`
(`cull_mode` disabled), `billboard`, `no_depth_test`, `fixed_size`, `render_priority`, so billboards, fades and cull come from R6. Changing a Sprite3D property at run time changes the stored property only.
`Label3D` is a property holder with `IsDrawn == false`.

### Aver.Fabric (Godot names)

`ArrayMesh` (`AddSurfaceFromArrays`, `SurfaceGetArrays`, `ClearSurfaces`, `SurfaceRemove`, `SurfaceSet/GetMaterial`, names, `GetAabb`), `ImmediateMesh` (`SurfaceBegin` ... `SurfaceEnd`, `ClearSurfaces`), `SurfaceTool` (`Begin`, `Set*`,
`AddVertex`, `AddIndex`, `AddTriangleFan`, `Index`, `Deindex`, `GenerateNormals`, `GenerateTangents`, `SetMaterial`, `Commit`, `CommitToArrays`, `CreateFrom`, `AppendFrom`), `MeshDataTool`, `MultiMesh` (`Mesh`, `UseColors`, `UseCustomData`,
`InstanceCount`, `VisibleInstanceCount`, `Buffer`, `Set/GetInstanceTransform|Color|CustomData`, `GetAabb`), `MultiMeshInstance3D`, `MeshLibrary`, `GridMap`, the CSG family, `SpriteBase3D`, `Sprite3D`, `Label3D`. `MeshInstance3D.Mesh` accepts an `ArrayMesh` or
`ImmediateMesh` (it draws `@rtmesh/...`). Backends are swappable (`RuntimeMeshBackend.Override`) so the tests record the calls. `FabricSpace.ToAverMatrix12` and `ToAverTangent` are the two conversions (a Godot affine to the instance matrix; a Godot
normal + tangent + handedness to the R1 tangent). `SurfaceTool.GenerateTangents` accumulates tangents from the UV derivatives (handedness so that `cross(N, T) * w` points at decreasing v), not MikkTSpace; custom channels, bone indices and weights,
blend shapes and LODs are accepted and not kept.

### What does not see it (documented limits)

- A runtime or MultiMesh mesh is an ordinary mesh: **ray-driven mode draws it like any other mesh** (the vertex colour from a per-instance colour needs the raster path, `RENDER.RTRENDERMODE 0`, as R1).
- A MultiMesh re-uploads the whole baked mesh whenever its table changes (once a frame at most): a per-frame animated table of thousands of instances costs a CPU merge and a GPU upload every frame. There is no GPU instance buffer; no per-instance culling or LOD.
  Instance transforms are not an engine-level instance count: shadows and global illumination see the merged mesh.
- `ImmediateMesh` re-sends the surface on every `SurfaceEnd`. Lines and points are held and never drawn.
- Two meshes built with the same key would be one mesh: Aver.Fabric uses the Godot instance id in the key, so this only matters to a caller of the ABI.
- Runtime-made `Sprite3D` / `Label3D` draw nothing; the converter bakes sprites. Runtime `MultiMesh.Mesh` of a file mesh needs it under the project's content folder (a `res://` path).
- CSG: boolean results are as exact as the BSP's tolerance; coplanar overlapping faces are handled (tested), self-intersecting or open input is not repaired. Large `CSGMesh3D` inputs are slow (the BSP is recursive-free but quadratic in the worst case).
- `TubeTrailMesh` / `RibbonTrailMesh` are generated centred along Y and straight; the curve and the particle bend are not applied. `TextMesh` is not converted.

### Tests (all headless)

`CsgTest` (48: volumes, bounds and areas of the primitives and of union / intersection / subtraction incl. coplanar faces, a bored cube, a sphere cut from a box, materials kept, chained operations, mirror transform, the soup),
`MeshBuildTest` (49: surface assembly and padding and refusals, smooth normals, instance baking: transforms, colours, custom data, visible count, budget, mirrors, non-uniform scale normals, tangents),
`FrameworkRtMeshAbiTest` (83: the C ABI through the Aver.Framework DLL: keys, budgets, surfaces, view, dirty and removed queues, MultiMesh from a runtime mesh and from an `.ocmesh` file, refusals),
`GodotMeshTest` (55: ArrayMesh decode from hand-built byte arrays: uncompressed, compressed, 16/32-bit indices, strips, skin, custom data, refusals, octahedral coding, GridMap cells and the 24 bases),
`FabricGeometryTest` (72: the converter on `tests/fabric/data/GeomGame`: ArrayMesh, MultiMesh, trail meshes, CSG volumes and collision, Sprite3D quad and material, Label3D, GridMap items and shapes, the report),
`Aver.Fabric.Tests` ("geometry: ..." 11 cases: the classes, ArrayMesh pushes, SurfaceTool, ImmediateMesh, MeshDataTool, the space conversions, MultiMesh table and text, MeshLibrary, GridMap, CSG / Sprite / Label properties, no engine),
and the unchanged `FabricConvertTest`, `FabricMeshExtraTest`, `FabricMaterialFxTest`, `FabricCameraTest`, `FabricLightsTest`, `FabricEnvTest`, `FabricViewportTest`, `SeparationTest`, `PathSeparatorTest`.

---

## R8: fog volumes, reflection probes, and the non-RT fallbacks

Status: fog volumes are complete from the Godot node to the compute shader (headless tests plus DXC compiles of every variant; the GPU side is unseen, see the headed checks). Reflection probes are DATA ONLY: the
node, the registry, the maths and the converter exist; the renderer captures and samples no environment. SSAO and SSR got no new pass (below says why). LightmapGI, VoxelGI, LightmapProbe and SDFGI are kept and rated;
Aver has no lightmap baker or sampler. This is the package where the honest answer to several asks is "Aver already does this another way" or "not done", and the sections say which.

### What each ask became

| Ask | Result |
|---|---|
| ReflectionProbe, box projection, captured at load, sampled by the raster path | **Data, maths and queries only.** `Aver.Fabric.ReflectionProbe` publishes box, origin offset, intensity, blend, max distance, box projection, interior, ambient and masks to the registry (`framework_envfx_abi.h`); `aver/core/ReflectionProbe.hpp` holds the oriented-box influence, the smallest-box-first selection with blending, the box projection and the octahedral / cube-face maps; scripts query `ReflectionProbe.Select` / `ProjectDirection`. **No capture and no sampling**: see "Why probes are not drawn". |
| SSAO fallback for non-RT tiers | **No new pass.** The raster scene pixel shader already gets an occlusion term without ray tracing: `coneTracedIndirect` returns the voxel volume's occlusion (`ao`) whenever voxel GI is on (`RENDER.GI`), and `ind4.occlusion` multiplies the ambient with it. With GI off there is none, and a screen-space pass cannot supply one in the scene pass (no depth before shading except the optional prepass; the post chain's depth is the D3D12 G-buffer only). The converter now rates `ssao_*` approximated, saying that. |
| SSR fallback | **Reported, not built**: non-RT reflections are the voxel cone trace (`traceCone`, then the sky) inside the GI volume; ray-traced reflections replace them when ray tracing is on. `ssr_*` is rated approximated for that. |
| FogVolume (box, ellipsoid, cone, cylinder, world; density, albedo, emission) | **Done**, inside Voxi's volumetric fog (froxel) as a compute variant. Section below. |
| LightmapGI, baked sampling | **Not possible today**: `OcLightmap` is storage only ("nothing in this engine samples a lightmap on the GPU yet", `OcLightmap.hpp`) and the editor says "No lightmap baker yet". Godot's `.lmbake` is a binary resource the converter does not read. The node is kept and every key rated; the light is Aver's real-time GI. There are no Aver "lightmap bake settings" to map onto. |
| VoxelGI, SDFGI, LightmapProbe | Kept and rated **approximated**: Aver's voxel / ray-traced GI is always real time around the camera (`RENDER.GI`, `RENDER.VOXELRES`, `RENDER.GIDISTANCE`). The converter does not force a GI setting. |
| Converter and `Aver.Fabric` classes | `FogVolume`, `FogMaterial`, `ReflectionProbe`, `LightmapGI`, `LightmapGIData`, `LightmapProbe`, `VoxelGI`, `VoxelGIData`, `Texture3D`, and `RenderingServer.FogVolumeShape`. |

### Local fog volumes

**Pieces.** `aver/core/FogVolume.hpp` (pure: `FogVolumeDesc`, the affine inverse, per-shape signed distance, `fogv::density`, `FogVolumeBlock` packing) -> `framework_envfx_abi.h` / `EnvFxAbi.cpp` (the registry, 64 volumes,
`aver_fw_fogvol_pack` nearest the camera first, a revision counter) -> `Runtime/include/aver/game/EnvFxFeed.hpp` (both hosts, once a frame after the light feed; a no-op while the registry is empty) ->
`VoxiRenderer::setFogVolumes` -> `VoxiFroxelFog::setFogVolumes` / `record` -> `voxi_froxel.hlsl` under `AVER_FOG_VOLUMES`. C#: `Aver.Framework.EnvFx`, `Aver.Fabric.FogVolume` / `FogMaterial`.

**Block.** `FogVolumeBlock`: 16 bytes of header (count in x) then 16 volumes x 6 float4 (`kFogVolumeMax`, `kFogVolumeVecs`; `EnvFxTest` pins the HLSL's `AVER_FV_MAX 16` / `AVER_FV_VECS 6` and the tail's size). Per volume:
three rows world -> unit box (xyz coefficients, w offset, already divided by the half size, so the shape lives in [-1, 1]); `x` extinction per cm (Godot's density per metre / 100), `y` edge fade, `z` height
falloff per cm, `w` shape; albedo rgb and the volume's lowest world Z; emission rgb and the smallest half size. Appended as a TAIL of `FroxelCB` (496 B + 1552 B) only in the variant; the ordinary
`FroxelConstants` and the 496-byte assert are untouched, and `VoxiFrame` is not involved (its three mirrors do not move).

**Shapes** (unit-box signed distance, negative inside): box `max|u| - 1`; ellipsoid `|u| - 1`; cylinder `max(|u.xy| - 1, |u.z| - 1)` with the axis on the local Z (Godot's Y); cone `max(|u.xy| - (1 - u.z)/2, |u.z| - 1)` with the tip at local +Z and the
base at -Z; world has no surface. **The cone's tip direction and the exact edge-fade and height-falloff formulas are NOT read from Godot's code** (the fetched shader files do not contain them; they live in the default
FogMaterial shader): edge fade is a linear ramp over the outer `edge_fade` fraction of the half size, height falloff is `exp(-falloff x metres above the volume's lowest point)`. Both are rated approximated.

**Axis map.** A volume's engine half sizes are `(size.z, size.x, size.y) x 50` cm (engine X is Godot -Z, Y is Godot X, Z is Godot Y), the matrix is `FabricSpace.ToAverMatrix12(GlobalTransform)` (it carries node scale), so a node scaled in
Godot scales the volume. The node republishes on a change of size, shape, material (`changed`), visibility and global transform (compared once a frame, so a volume under a moving parent follows it).

**Shader.** `CSFroxelScatter` / `CSFroxelIntegrate` with `AVER_FOG_VOLUMES=1` (built by `VoxiFroxelFog::buildVolumePipelines` the first frame a volume reaches `record`; SM 6.0, or 6.5 for the ray-query sun like the ordinary pass). The
ordinary pass stores (radiance per unit extinction, total extinction). The volume variant stores (radiance per cm, extinction per cm WITHOUT the analytic height fog): the height fog's extinction is applied once, analytically, at the shade
sites, so the transmittance they read must leave it out; the integrate pass recomputes it at the slice's middle to attenuate the in-scatter (small error against the scatter pass's jittered point, only in this variant). Scattering
`albedo x positive density x (sun + sky light)` and emission `emission x positive density` are added to the radiance; a negative volume subtracts from the extinction only. Switching variants drops the history for a frame. If the
variants would not compile the volumes are not drawn and the ordinary passes run. Not touched: `PSRayDriven`, the ray-driven single-pass variants, the RT shadow-ray code, `voxi.hlsl`, `VoxiFrame`.

**Needs the fog on.** Fog volumes live inside volumetric fog (`RENDER.VOLFOG 1`, `voxi.volumetricFog`), as a Godot FogVolume needs `volumetric_fog_enabled`. New project keys (`OcProject.hpp`, `ProjectRenderApply.hpp`,
docs/formats/FORMAT_SPECS.md): `RENDER.VOLFOG 0|1` and `RENDER.VOLFOGDENSITY <per cm>` (clamped [0, 0.01]); absent = today (off). The converter writes `RENDER.VOLFOG 1` and `RENDER.VOLFOGDENSITY 0` when the start scene's
Environment has `volumetric_fog_enabled` (the density already rides in the level's fog, so the uniform medium stays 0 and nothing is counted twice). The log line `[EnvFx] N fog volume(s) drawn` says when the feed pushes some.

**Limits.** 16 volumes drawn per frame (nearest the centre); uniform density inside the shape (`density_texture` is unsupported, so is a custom fog `ShaderMaterial`); emission is not lit or occluded; the 160 x 90 x 64 froxel grid
softens small volumes; fog volumes are not seen in reflections or by the water and particle shaders (volumetric fog's own limits); the sky pass applies the far slice of the combined volume.

### Why probes are not drawn

A probe needs (1) a capture of the scene from the probe point, (2) a prefiltered store, (3) sampling in the raster scene shader, on D3D12 and Vulkan. What is there and what is missing:

- Cube textures: `rhi::TextureDim` is `Tex2D` / `Tex3D`; there is no cube or array dimension (R2's omni shadow is six tiles of a 2D atlas for the same reason). An octahedral 2D map (`probe::octEncode`) avoids it.
- Capture: R5's sub-viewport can draw a view into a texture (flat colours, one sun, no GI), six times; a compute pass would resample the six faces into an octahedral map with a roughness chain. Neither the six-view scheduling nor the
  resolve pass exists.
- Sampling: the scene shader's binding table 0 is full (25 SRVs; `giTableKinds`). R2 borrowed t19 in frames where the ray-driven history does not run. A probe would need the same trick (t23, the denoised-reflection read, is also
  idle in a plain raster frame) plus a second cbuffer tail after R2's, and its own twin set of `PSMainVoxi` variants for plain, G-buffer, prepassed and mesh-extra draws (the local-light twins would double).
- What it would add: in a non-RT frame with voxel GI on, the reflection already comes from `traceCone` through the scene's own radiance volume, so a probe mainly adds reach beyond the GI volume and a cheaper lookup. With GI off,
  reflections are the sky only, and that is the case a probe fixes.

None of that can be checked without a GPU, and it would touch the shared pipeline-variant machinery (`createLocalRasterPipelines`, `localCbBlob_`). It is left as a documented next step. The maths a sampler needs is done and
tested (`ReflectionProbe.hpp`): influence with blend distance, oriented boxes, box projection (`boxProject`), smallest-first selection into at most four probes whose weights plus the sky's rest sum to one, the octahedral and
cube-face maps and `roughnessToMip`. The host logs once `[EnvFx] N reflection probe(s) registered; the renderer samples no captured environment yet`.

### Converter and runtime

`tools/fabric`: `FogVolume`, `ReflectionProbe`, `LightmapGI`, `LightmapProbe`, `VoxelGI` are supported classes (no longer carriers; the node classes exist in Aver.Fabric). `FogMaterial` is held as an inline `Object(FogMaterial, ...)` value like
the environment resources; every key is rated at the resource (density, albedo converted; emission, height_falloff, edge_fade approximated; density_texture unsupported). Per-node and per-property ratings are in `FabricScene.cpp`
(R8 block). The Environment's `ssao_*`, `ssr_*`, `ssil_*` and `sdfgi_*` are now **approximated** with what covers each (they were unsupported). `EnvInfo::volumetricFog` and the project keys above carry volumetric fog.
`Aver.Fabric` (`Nodes/EnvFx3D.cs`): see the table; FogVolume and ReflectionProbe publish through `IEnvFxBackend` (swappable by tests); LightmapGI, VoxelGI and LightmapProbe keep their properties and publish nothing.

### Tests (all headless)

`EnvFxTest` (the maths: affine inverse, shapes, edge fade, height falloff, carving, packing and the 16-volume cut, the HLSL text; probe influence, box projection against hand-computed hits including a yawed box and an origin
offset, selection and blending, masks, max distance, the octahedral and cube-face round trips), `FrameworkEnvFxAbiTest` (the C ABI: create / find / set / enable / destroy, caps, refusals, density query, pack order and revision,
probe select and project), `ProjectRenderApplyTest` (`RENDER.VOLFOG`, `RENDER.VOLFOGDENSITY`), `FabricEnvVolTest` (38 checks: the converter on `tests/fabric/data/EnvVolGame`, the report, no key for EnvGame / LightsGame),
`FabricEnvTest` / `FabricConvertTest` (their ssao / sdfgi expectations moved to approximated), `VoxiSpirvCompileTest` (68 variants: 6 froxel rows, ordinary and volume, to SPIR-V and DXIL), `Aver.Fabric.Tests` ("env fx: ..." 6 cases
and the converter's own output instantiating and publishing).

---

## Headed checks for the owner

Nothing below has been run on a GPU. Do them in this order; each says what correct looks like. Back out with `RENDER.MESHEXTRA 0` in the `.ocproject`
(then the stream is bound and ignored and every mesh draws through the old pipelines) and report which step differed.

### R1

Setting for every step: `RENDER.RTRENDERMODE 0` in the `.ocproject` (the rasteriser finds the first surface; ray-driven mode ignores the stream, see the limits
above). Run each step once with ray tracing off (the plain raster twin) and once with `RENDER.RAYTRACING` Medium or higher (the ray-traced-raster twin).

0. First, with a project that has no mesh carrying the stream (any existing level, e.g. NewSponza_Night or NeonDistrict): nothing at all should differ,
   and the log should NOT contain "second-vertex-stream scene variants built". This is the defaults-unchanged check.
1. Convert the fixture: `AverAssetC fabric tests/fabric/data/ExtraGame <out> --scenes-only`, open `<out>/ExtraGame/ExtraGame.ocproject`, load
   `Maps/main.ocworld`. Expected log line on the first frame: `[Voxi] second-vertex-stream scene variants built (raster)` (or `ray-traced raster`
   with ray tracing on). No warning "unavailable".
2. The imported quad (`Main/Model/Quad`) is a one-metre square. Its material is the glTF default. It has colour but the default material does not ask for it,
   so it must look exactly like the same quad with `RENDER.MESHEXTRA 0`.
3. Make the quad's material read the colour: copy the converted `Materials/ExtraGame_tinted.ocmat` (`FLAGS ... vertexcolor=1`) onto it (the editor's material
   slot, or edit the quad `.ocmesh` slot name). Correct: a red / green / blue / half-transparent-white gradient across the square, each corner its colour,
   alpha 0.5 at the white corner (the material must be blended for the alpha to show; the albedo is multiplied either way). With `RENDER.MESHEXTRA 0` the
   quad is plain white.
4. Tangent frame: give the quad (or any glTF with TANGENT, e.g. a Blender export with a normal map) a normal-mapped material. Compare lighting against
   `RENDER.MESHEXTRA 0` (derived frame): bumps must light from the same side. The two should agree to within shading noise; a mirrored look (bumps
   inverted) means the handedness sign is wrong for that source, so report the file. Check one mirrored placement (scale -1 on one axis) too.
5. Primitives: any converted BoxMesh / SphereMesh with a normal-mapped material. They now carry generated tangents; they should look the same as before (the
   derived frame was already exact for them). Sphere poles are the place to look: no pinched or black normal-map artifacts at the poles.
6. Depth prepass on (`RENDER.DEPTHPREPASS 1`) with an alpha-masked material on a mesh with colour: no missing or doubled surfaces (the `prepassed` extra twin).
7. G-buffer / AverSR / NRD2 on: nothing missing in the denoiser inputs on the quad (the `G-buffer` extra twin).
8. Vulkan (`--backend vulkan`): steps 1-3 again. Vulkan matches attributes by position, so a wrong order shows as scrambled colours or a flipped bump.
9. AMD RX 7800 XT: the extra `PSMainVoxi` ray-traced twin is about 3% larger than the ordinary one (SPIR-V size, 556 KB against 541 KB). Step 1 with ray
   tracing ON is the one to watch for a device-lost on the first frame such a mesh is drawn.
10. Ray-driven mode (`RENDER.RTRENDERMODE 1`, the default): nothing changes, the quad draws exactly as with `RENDER.MESHEXTRA 0` and no extra variant is built
    (no "second-vertex-stream" log line). That is expected until the ray paths read the stream.

### R2

Fixture: `AverAssetC fabric tests/fabric/data/LightsGame <out> --scenes-only`, then open `<out>/LightsGame/LightsGame.ocproject` (a floor, a crate, a sun and a second directional light, an omni `Lamp` with a shadow,
a negative spot `Torch` with no shadow and a 45 degree spot `Beam` with a shadow). Back out with `RENDER.LOCALRASTER 2` (today's behaviour exactly) and report which step differed. Every log line below
is in the `[Voxi]` prefix and appears in `Output Log` / the log file.

0. Defaults unchanged, first: open an existing project whose level has `CLight`s (or any converted level) with the default settings on the RT card (ray tracing on, ray-driven mode). It must look exactly as before and the log must
   NOT contain "raster local-light scene variants built" or "raster local lights:". Then open a level with no `CLight` at all with `RENDER.RAYTRACING Off`: also none of those lines, and no difference.
1. D3D12, `RENDER.RAYTRACING Off` in the `.ocproject`. Open `Maps/main.ocworld`. Expected log on the first frames: `raster local-light scene variants built (raster)`, then
   `raster local lights: 3 lit, 2 with shadow maps (7 tiles), 3 offered, ...` (Lamp 6 tiles, Beam 1; Torch has no shadow). Look at the floor and the crate: the omni `Lamp` (3 m up, 6 m range) lights a pool of light round
   its foot with the crate's shadow cast away from it on all sides (cube shadow: no seam line across the floor where two faces meet); the `Beam` lights an oval on the floor and the crate's shadow falls behind it; the `Torch`
   (negative, unshadowed, at the origin) leaves a darker patch round the origin instead of a bright one and never a black or negative hole (the pixel clamps at zero). Walk toward and away from the lamp: it fades out at 40 m with
   `distance_fade_enabled` (move a copy of the lamp 45 m out to see it gone).
2. Shadow quality: no light on the crate's own faces (self-shadowing acne), no gap between the crate and its shadow (peter-panning). The converted biases are `shadow_bias 0.3` -> 3 cm and `shadow_normal_bias 4` texels on the Lamp; if the
   crate floats or acne shows, report the light and distance. `RENDER.LOCALSHADOWS 0`: same lights, no shadows, no log line about tiles.
3. Godot's falloff: change the Lamp to `omni_attenuation = 1.0` (Godot's default) and `0.0` in the `.tscn`, convert again: 1.0 falls off linearly-ish and reaches further than 2.0 at the same energy, 0.0 is flat bright until the
   range edge then fades. The brightness at 1 m from the lamp should be close for all three. A white surface at 1 m under `light_energy 1`, `omni_attenuation 1`, range 5 m should read about 0.8 of its albedo before exposure/tonemap
   (`(1 - 1/5) * energy`); report a large mismatch with the exposure and the tonemapper in the report.
4. Specular: `light_specular = 0` on the Lamp: the highlight on a glossy floor vanishes and the diffuse pool stays. Default (0.5) matches an unflagged light's highlight.
5. Vulkan (`--backend vulkan`, `RENDER.RAYTRACING Off`): steps 1-2 again. Watch the validation layer: the shadow tiles are drawn inside temporary dynamic-rendering scopes (`setTargetsIsolated`); a "render pass" or
   "attachment" validation error on the first frame with a shadowed light is the thing to report. The scene must be intact after the shadow pass (no missing floor, no depth corruption: the tiles are 512-1024 px squares drawn at
   the top-left of a 4096 px target, far outside the scene viewport if they landed in the wrong attachments).
6. Prepass and G-buffer: `RENDER.DEPTHPREPASS 1`, then the denoiser/AverSR with the G-buffer on, with the lights live: no missing or doubled surfaces, the lights still present (the prepassed and G-buffer twins).
7. A mesh with vertex colour (R1's `ExtraGame` quad) near the Lamp: it is lit by the Lamp (the mesh-extra twin with the lights), the log says `second-vertex-stream scene variants built (raster, raster local lights)`.
8. The opt-in with ray tracing on (the one to watch on the RX 7800 XT): `RENDER.RTRENDERMODE 0` and `RENDER.LOCALRASTER 1`, ray tracing on. Log: `raster local-light scene variants built (ray-traced raster)` and the state line ends with
   `[ray tracing active: opt-in raster path replaces the ray-traced light list]`. The Lamp, Beam and Torch light the scene as in step 1. **This is the AVER_RT PSMainVoxi variant plus the light loop (SPIR-V 565 KB against 541 KB);
   a device-lost on the first frame it is drawn is the thing to report.** Then `RENDER.RTRENDERMODE 1` (the default) with `RENDER.LOCALRASTER 1`: the opt-in does nothing (ray-driven frames keep the ray-traced lights), no raster line appears.
9. The sun: in a script (or the console) set a `DirectionalLight3D`'s `LightEnergy` to 0.2 after load, then rotate it: the sun dims and the shadow direction follows. The second directional light (`Moon`) changes nothing. At load the sun is
   exactly the level's: 2.0 x 104719.755 lux, colour (1, 0.95, 0.9).

### R3

Back out with `RENDER.TONEMAP 1` (Aver's default) and no `RENDER.ADJUST*` keys (the ordinary composite is then the only one built) and report which step differed. The
post chain runs on both backends; the new composite is logged once when built: `[RHI.D3D12] Godot tonemapper composite built (bloom B, auto-exposure A)` /
`[RHI.Vulkan] Godot tonemapper composite built (lut L, bloom B, auto-exposure A)`; `... composite failed to compile/build; the project's ordinary tonemapper is used` is the failure line.
Console: `set post.tonemap N` (0..7), the editor's Project Settings > Rendering > Post page (tone curve, tone white, brightness, contrast, saturation).

0. Defaults unchanged, first: open any existing project or level (NewSponza_Night, NeonDistrict) with its own settings. It must look exactly as before and the log must NOT contain "Godot tonemapper composite built".
1. Convert the fixture: `AverAssetC fabric tests/fabric/data/EnvGame <out> --scenes-only` (the panorama scene needs a `sky.hdr` beside it; skip `Maps/panorama` for this step). Open `<out>/EnvGame/EnvGame.ocproject`, load
   `Maps/main.ocworld`. The project states `RENDER.TONEMAP 7` (AgX), `RENDER.TONEMAPWHITE 6`, `RENDER.AUTOEXPOSURE 0`, `RENDER.BLOOM 0.7` and `RENDER.ADJUST*`; the log shows the composite built line. Correct: the image is
   tonemapped with no eye adaptation (it does not slowly re-expose when you look at the sky); AgX looks softer in the highlights and keeps saturated colours from clipping to white earlier than the ACES default does.
2. Compare the modes on one view with the console: `set post.tonemap 3` (linear: hard clip, the brightest areas flat white, mid-greys unchanged), `4` (Reinhard at white 1 is the SAME as linear; set `RENDER.TONEMAPWHITE 4` in the project and Reinhard rolls off
   toward white at 4), `5` (filmic: a contrasty shoulder, middle grey a little darker than ACES), `6` (ACES with white: close to Aver's matrixed ACES `1` but brighter in the highlights), `7` (AgX). `0`..`2` must be
   exactly the old curves and switch the log to no new composite (the existing composite is reused; the EXT one stays built but is not drawn).
3. Display adjustments: with the fixture's `RENDER.ADJUSTBRIGHTNESS 1.2 / CONTRAST 1.1 / SATURATION 0.8` the image is a little brighter, a little more contrasty and a little grey. Set each to 1 (or remove the keys) and the picture returns to
   the plain AgX result. Saturation 0 is a grey image, contrast 0 a flat 0.5 grey, brightness 0 black. A grading LUT (a post volume's `.cube`) still applies after these.
4. Level selection: the level carries a `WorldEnvironment` post volume (Outliner > Post Process Volumes): `tonemap`, `tonemapwhite`, `exposure`, `autoexposure`, `bloom`, the adjustments and the DOF keys. Load a second converted level with a different
   `tonemap_mode`: the tonemapper follows the level. Change the volume's Tonemap in the Details panel (0..7) and the picture changes live.
5. Fog and sky of the fixture: depth fog (`fog_mode` 1) starts at 20 m and thickens to nearly opaque by 120 m; height fog is a faint extra thickening near the ground (3 m) that thins upward. The sky is a blue-to-pale dome brighter than the
   unscaled colours (energy 2).
6. Camera attributes: the far blur (`dof_blur_far_enabled`, 8 m, amount 0.2) is a lens: it needs the D3D12 G-buffer, so it shows on D3D12 only (Vulkan has no G-buffer and skips DOF). Objects well beyond 8 m blur softly; nearer ones stay sharp
   (a physical lens also softens objects much nearer than the focus, which Godot's far-only blur does not).
7. Panorama sky: convert the fixture with a real equirectangular `sky.hdr` and open `Maps/panorama.ocworld`. Correct: a smooth dome whose colours and brightness by height follow the HDRI (bright sky above, darker toward the floor), the same
   in raster and in ray-driven mode (and in reflections); NOT the HDRI's clouds or sun, which are not drawn. This is the approximation.
8. Vulkan (`--backend vulkan`): steps 1 to 3 again. A validation error on the first frame with a Godot tonemapper (descriptor set layout of the post pipeline, `gPostExt`) is the thing to report.
9. Scripts: in a converted project, set `GetNode<WorldEnvironment>("Env").Environment.TonemapMode` to another value at run time: the tonemapper changes on the next frame (post layer `fabric.environment`); change
   `FogDensity` and the fog follows. Nothing changes at load.

### R4

Fixture: `AverAssetC fabric tests/fabric/data/CameraGame <out> --scenes-only`, then `<out>/CameraGame/CameraGame.ocproject` (a level `Maps/main` with three cameras, an `Ortho` one marked
current, two meshes with layers and a visibility range and an omni light with masks; a level `Maps/nocurrent` with two unmarked cameras). The fixture has no floor and no sun of its own: add a
floor and a few boxes (or open the camera nodes in a level you already have) so there is something to see. The lens is applied in Play (the editor's free-fly camera is untouched); the shipped
game does the same. Back out by removing the `Camera3D` nodes (or `current`): no other setting is involved, and a project that never sets a projection, layer or range runs today's code.

0. Defaults unchanged, first: any existing project or level (NewSponza_Night, NeonDistrict, a converted Godot project with ordinary perspective cameras): identical image, no
   `orthographic camera` log line, and a log with `RENDER.RTRENDERMODE 1` (default) still shows the ray-driven pass. Check the first-person pawn's body is still hidden from its own camera
   (owner-hide shares the route the camera cull mask uses).
1. Orthographic: make `Ortho` current (it is). Expected log line once: `[Voxi] orthographic camera: the rasteriser draws the primary view ...`. Correct: parallel projection, objects keep their
   size with distance, the view is 12 m wide (`size` 12 with the fixture's `keep_aspect = 0`, keep width: the height follows the window aspect; with `keep_aspect = 1` it would be 12 m tall), no
   perspective convergence of parallel edges. Walls and shadows are right; the sky is one flat colour across the screen (known approximation). With ray tracing ON and `RENDER.RTRENDERMODE 1` the image
   must still be drawn (not black, no device loss): that is the ray-driven pass being skipped for an orthographic view. Switch back to a perspective camera: the ray-driven pass returns with no restart.
2. Orthographic with the sun on: cascaded shadows cover the visible area (not cut at about 80 m); no shadow acne or flicker when the camera moves. TAA / AverSR on: no shimmer beyond a perspective view's.
3. Frustum: make `Frust` current (`Camera3D.Current = true` from a script, or move `current` in the `.tscn`). Correct: a perspective view whose centre is shifted (the vanishing point is not the
   screen centre; with `frustum_offset (0.3, -0.1)` and size 2 m the shift is a tenth of the window); sky and fog look normal.
4. keep_aspect and offsets: `Persp` (fov 90, keep width): the field of view is 90 degrees across the window, not top to bottom; resize the window and the width stays 90. `h_offset` / `v_offset` on
   `Ortho` slide the view 0.5 m right and 0.25 m down.
5. Camera cull mask: `Ortho` has `cull_mask 6` (layers 2 and 3). Put a default box (layer 1) and the `Near` box (layer 2) in view: the layer 1 box is gone from the view, the layer 2 box is drawn.
   The hidden box STILL throws its shadow on the floor and still shows in reflections / bounce light (Godot's behaviour). Prepass on (`RENDER.DEPTHPREPASS 1`): no hole where the hidden box would be.
6. Visibility range: `Near` is visible from 5 m to 40 m (begin margin 1 m, end margin 2 m, fade mode 1, which is a hard cut here). Walk away from it: it vanishes at about 42 m and
   comes back at about 38 m on the way in (the end margin is a hysteresis), and its shadow goes with it. Walk up to it: it is hidden closer than about 4 m and appears again past about 6 m
   when you back off. No flicker at the thresholds. `Plain` (end 100 m, no margin) disappears at 100 m. With `RENDER.DEPTHPREPASS 1` no hole where it was.
7. Light cull mask (raster local lights: `RENDER.RAYTRACING Off`, or `RENDER.LOCALRASTER 1` with `RENDER.RTRENDERMODE 0`): `Lamp` has `light_cull_mask 2` and `shadow_caster_mask 4`. Boxes on layer 2 are
   lit by it, boxes on layer 1 only by the sun/ambient; boxes that are not on layer 3 (4) cast no shadow from it. Remove the masks (or set 0xFFFFF): the light affects everything again.
8. Vulkan (`--backend vulkan`): steps 1, 3, 5 and 7 again. A validation error about push constants or a wrong layer test (the light affecting the wrong boxes) would be the per-object `_objPad` word.
9. AMD RX 7800 XT: the raster local-light pixel shader gained one uint test per light (`_objPad`, the mask word); a device loss on the first frame of step 7 with the ray-traced raster opt-in
   (`RENDER.LOCALRASTER 1`, ray tracing on, `RENDER.RTRENDERMODE 0`) is the thing to report. Step 1 on this card: the orthographic frame is drawn by the raster path with ray tracing active.
10. `Maps/nocurrent`: neither camera is marked; the converter marks `First` (fov 50) current, and the view starts there, not at the player's own camera. The log of the conversion has the
    `no camera in this scene is marked current` line.

### R5

Fixture: `AverAssetC fabric tests/fabric/data/ViewportGame <out> --scenes-only`, then open `<out>/ViewportGame/ViewportGame.ocproject` and Play `Maps/main` (a floor, a sun, five boxes in a row: `Plain`
at x = -3, `Screen` at 0, `Shaded` at 3, `Lost` at 6, `Wrong` at 9; the level camera at (0, 1, 6); a `SubViewport` whose `SecurityCam` is at (1, 2, 12); a `Hud` `SubViewportContainer` filling the window
with a `MapView` and an orthographic `MapCam` (which sits at the origin and sees little: it is there for the on-screen path). The views exist only in Play. Back out with `RENDER.SUBVIEWPORTS 0` in the
`.ocproject` (every create is refused, one warning per node, and the level draws exactly as before) and report which step differed. Log lines are `[SubViewport]` / `[Preview]`.

0. Defaults unchanged, first: Play any existing level or converted project with no SubViewport (NewSponza_Night, NeonDistrict, `CameraGame`). It must look exactly as before and the log must NOT contain
   `[SubViewport]` or `scene-view pipeline built`. This is the cost guard: no view, no work.
1. D3D12, Play `Maps/main`. Expected log: `[SubViewport] N view(s) live; secondary views are drawn by the cheap raster feature ...`, then `view 'main:Main/SubViewport' (id 1): 160x90 target created`
   (320x180 at `scaling_3d_scale` 0.5), `[Preview] scene-view pipeline built`. The `Hud` container fills the window (anchors 0..1), stretch 2: `MapView` is `main:Main/Hud/MapView`, resized to
   half the window; its picture is drawn over the whole window behind the HUD (an orthographic view from the origin looking along +X: mostly the grey clear colour, the edge of the floor). Correct: a rectangle of flat-shaded
   boxes under a sun, no shadows, no reflections, no fog, no sky (a flat grey background, Godot's default clear colour 0.3).
2. The `Screen` box (unshaded) and the `Shaded` box show the security camera's picture: the row of boxes seen from (1, 2, 12) at 40 degrees, flat colours (`Plain` red, `Screen` and `Shaded`
   white-ish: a view's own picture has no average colour, so they read white; `Lost` and `Wrong` white: their texture slots are empty). `Screen` glows (emissive) in addition to being lit; `Shaded` only lit.
   The picture is sharper than 160x90 suggests only if the box is far away; it is bilinear-filtered. If a box shows the picture too bright or too dark, report it with the exposure (a view that only materials read
   stores LINEAR values; the main view applies its own exposure and tonemap on top).
3. The security view keeps up with the world: a script (or the editor's console) that moves a box, or a rigid body falling in the level, moves in the picture the next frame (the walk is before `beginFrame`,
   no lag). The picture does not show the box's shadow or the sky: that is the documented limit.
4. Update modes: set `render_target_update_mode` of the SubViewport to 0 in the `.tscn` and convert: the screen stays white (never drawn). 1 (once): one picture, then it freezes. 2 (when visible): drawn, because
   the converter counted two materials (`_material_consumers 2`). 3 and 4: drawn. `RequestRender()` on a disabled view draws exactly one picture.
5. Budget: add four more SubViewports (any cameras) with update mode 4. The fifth refused view logs `refused` (`RENDER.SUBVIEWPORTS`, default 4); with `RENDER.SUBVIEWRENDERS 1` only one view is
   drawn a frame (the others update at a lower rate, round robin; none starves); `RENDER.SUBVIEWPIXELS 0.05` shrinks every view by the same factor. Frame time with 4 always-views over a
   large level (NeonDistrict): each view costs a world walk on the CPU and one draw per visible mesh; report the cost per view.
6. Resize: set `Size` of the SubViewport from a script once the game runs. One hitch (the target is rebuilt after a GPU wait), then the new size; the screen box keeps showing the picture, and the
   log has `resized to`. A resize every frame is not supported.
7. Transparent background: `transparent_bg true` (the fixture): the clear is (0, 0, 0, 0); on the `Hud` container the picture's uncovered areas show the world behind, not black. Clear mode never / once: with
   `render_target_clear_mode` 1 the previous picture stays under new draws (a smear); 2 clears only the first picture.
8. Cameras: the level camera is `MainCamera`; the player's view does not jump to `SecurityCam` at load (it would if `current` had reached the window). Make `Spare` current from a script
   (`MakeCurrent`): the screen shows `Spare`'s view (the origin, looking -Z... at an empty floor edge) and the main view is unchanged.
9. Vulkan (`--backend vulkan`): steps 1, 2 and 7. The view features draw render passes before the scene pass; a validation error on the first frame with a view (render target state, the b4 camera block, the
   push-constant range of the preview pipeline) is the thing to report. The scene must be intact afterwards.
10. AMD RX 7800 XT: the extra pass is raster only (no ray queries, nothing added to `PSRayDriven`). A device loss on the first frame a view is drawn, or when a view is created while ray-driven mode is on, is the
    thing to report. Run step 2 once with `RENDER.RTRENDERMODE 0` and once with the default.
11. Leaving Play in the editor drops every view (`[SubViewport]` destroyed, the materials fall back to white); pressing Play again makes them again with no leak (the log shows `created` once per Play).

### R6

Fixture: `AverAssetC fabric tests/fabric/data/MatGame <out> --scenes-only`, open `<out>/MatGame/MatGame.ocproject`, `Maps/main`: eleven boxes (`Default`, `Uv`, `State`, `Hash`, `Scissor`, `Mul`, `Vertex`,
`Fade`, `Surface`, `Height`, `Orm`), all at the origin (spread them out in the editor first; the fixture has no floor, sun or camera, so add a floor, a sun and look at them). Setting for every step:
`RENDER.RTRENDERMODE 0` in the `.ocproject` (ray-driven mode draws primaries with `PSRayDriven`, which never sees these features), and run each step with ray tracing off and once with it on.
Back out with `RENDER.MATERIALFX 0` (every material draws as before, the extension is ignored) and report which step differed. Log lines are `[Voxi] material variant 0x... built`.

0. Defaults unchanged, first: open an existing project or level that has no FX record anywhere (NewSponza_Night, NeonDistrict). It must look exactly as before and the log must NOT contain
   `material variant`. This is the defaults-unchanged check (no fx material = no tail, no variant, no new pipeline).
1. Every **converted** material carries `cull=back`, so the whole fixture draws through variants with back-face culling. First look: nothing is inside out or missing. A box seen from
   outside shows its three front faces; a hole where a face should be, or the inside of the far faces showing, means the winding of that mesh is wrong for culling (report the mesh: the primitive
   meshes and the glTF importer were written to reverse winding with the axis map, and nothing had ever culled them). Check a mirrored placement (scale -1 on one axis): it will look inside out (known).
2. `Default` (`cull=back`, nothing else): it must look like the same box with `RENDER.MATERIALFX 0`, bar the culled back faces. Log: `material variant 0x000001 built (raster)`.
3. `Uv` (uv1 scale (2,3), offset (0.25,0.5), world triplanar sharpness 4): give it a texture with an obvious pattern and a surface that is not axis aligned. Correct: the pattern is projected along the
   three world axes and blended by the normal (sharper blend at 4), tiled 2 x 3 per metre; no stretching on the faces. With `uv1_triplanar` off, the pattern is the mesh UV tiled 2 x 3 and shifted by
   a quarter and a half. **V direction:** if the offset moves the pattern the wrong way up, report it (the importers' UV convention was not checked against Godot's).
4. `State` (alpha blend + add blend, no depth test, depth draw always, priority 5, unshaded, both faces): the box adds its albedo (times 0.5) onto what is behind it, drawn over everything (no depth
   test), lit by nothing. Put a second translucent material (`Mul`, priority 0) overlapping it: the priority-5 one draws after (on top of) it whatever the distance. `Mul` darkens what is behind it by
   its albedo (0.5, 0.5, 1) weighted by 0.8: a blue-ish dark filter, no light added, no fog tint.
5. `Hash` (alpha hash, scale 2, antialiasing): the surface is opaque with a noisy dissolve at alpha 0.6, stable when the camera moves (it is a texture-space noise), no hole in the depth buffer under the
   discarded pixels (objects behind the dissolved parts show through). `Scissor` (cut-off 0.3, front-face culling): cut-out edge at the threshold; you see the INSIDE of the box (front faces culled).
6. `Vertex` (billboard Y, keep scale, fixed size, grow 2 cm): the box turns about the vertical axis to face the camera and keeps the same apparent size as you walk away (1 m of depth is the authored
   size); it is 2 cm fatter. Its shadow and any depth prepass are the plain box (documented). No stretching at oblique angles. Enable `RENDER.DEPTHPREPASS 1` and check the box neither vanishes nor z-fights.
7. `Fade` (alpha + distance fade pixel dither 2 m to 20 m): a screen-door dissolve that is fully gone closer than 2 m and fully solid beyond 20 m; no depth hole; no fade for the proximity fade (unsupported).
8. `Surface` (rim, backlight, anisotropy, clearcoat, refraction, subsurface, emission multiply, ao affect, specular off, diffuse wrap, no ambient, no shadows): a lit orange box with no specular highlight, a rim
   glow at grazing angles (sun side only), a wrap-lit terminator, shadows from other objects NOT darkening it, no sky ambient (dark where the sun does not reach). The clearcoat shows only with
   `RENDER.LAYEREDBSDF` on. Refraction (transmission 0.85) makes it translucent: with `RENDER.REFRACTION` on, the backdrop bends.
9. `Height` (heightmap deep parallax scale 8, detail multiply on UV2): with a real height texture the surface appears to have depth that shifts with the view angle; the detail multiplies the base; the
   detail needs the mesh's second UV set (a primitive has none: UV1 is used). Flip tangent flips the shift direction left-right.
10. `Orm` (uv1 scale 5, both faces): five tiles per metre; both sides draw.
11. Vulkan (`--backend vulkan`): steps 2, 4, 5, 6 again. Validation errors to report: the blend factors of `Subtractive` / `Multiplicative`, a push-constant or descriptor layout mismatch on the first fx
    draw, the vertex stage reading the b2 block (`VSMainFx` is the first vertex shader that does).
12. AMD RX 7800 XT with ray tracing ON and `RENDER.RTRENDERMODE 0` (the ray-traced raster fx variants): `PSMainVoxi` with `AVER_RT=1` plus `AVER_MATX` features is larger than the ordinary one
    (DXIL of the all-features row: 130 KB against 254 KB for the plain RT variant, because unshaded folds the rest away; single-feature rows are about 3 to 10% larger). A device loss on the first
    frame a feature variant is drawn with ray tracing on is the thing to report, with the log's `material variant` line and which material (try `Uv`, `Height`, `Surface` one at a time).
13. Ray-driven mode (`RENDER.RTRENDERMODE 1`, the default): the boxes draw through `PSRayDriven` and show none of the features (cull, UV transforms, billboards, ...); that is expected. The log has
    no `material variant` line (the raster scene draws are suppressed).

### R7

Fixture: `AverAssetC fabric tests/fabric/data/GeomGame <out> --scenes-only`, first replacing `<in>/textures/sprite.png` with a real 64 x 32 PNG (the checked-in file is a text stand-in; the converter reads the
size from the PNG header), then open `<out>/GeomGame/GeomGame.ocproject`, `Maps/main`. Everything sits at or near the origin: spread the nodes out in the editor, add a floor, a sun and a camera, and use
`RENDER.RTRENDERMODE 0` for the steps that say so (R1's reason: ray-driven mode draws primaries by ray, which does not matter for plain meshes but does for vertex colour). There are NO new `RENDER.*` keys: the
only back-out is not using the nodes. Log lines to look for: `[Mesh] ... @rtmesh/...` uploads, `[MultiMesh] ...` warnings, `[RuntimeMesh] ...` warnings.

0. Defaults unchanged, first: open an existing project or level that has none of these nodes (NewSponza_Night, NeonDistrict). It must look exactly as before and the log must contain no `@rtmesh` line. Every R7 path hangs
   off `GameContent::syncRuntimeMeshes`, which does nothing while the registry is empty.
1. `Array` (a stored `ArrayMesh`, one quad, material `red` with vertex colour as albedo): a quad 1 m x 1 m standing in the plane the Godot XY plane maps to (engine X-forward means it faces the -X side). Seen from the front it is
   solid red (white vertex colour times the red albedo); from behind it culls out with the material's `cull=back` (Godot's winding). **If it is visible only from behind, the winding is wrong for this path: report it**
   (the decoded order is kept as stored on purpose, see "The shape of it"). Move the sun: the normal is +Z in Godot terms, so the lit side faces the camera side described above.
2. `Many` (a MultiMesh of three 0.5 m boxes at x = 0, 2, 4 m, colours white / green / blue, the mesh material `vcol` = white albedo with vertex colour as albedo): three boxes in a row along engine +Y (Godot +X), white, green, blue
   (the colour multiplies the vertex colour, which the material takes as albedo). In ray-driven mode they are white (vertex colour is not seen there); with `RENDER.RTRENDERMODE 0` they are coloured. All three cast one merged shadow.
   One mesh upload, not three: the log shows a single `@rtmesh/multimesh/...` line.
3. Script-built mesh. Add this C# script to any node in the level (needs the Aver.Fabric runtime) and Play: a triangle 1 m wide and 1 m tall appears (default material: the vertex colour shows only on a material with `vertexcolor`, so give it `material_override` of `vcol` to see orange), and the log has one `@rtmesh/array/...` upload.
   ```csharp
   public override void _Ready()
   {
       var st = new SurfaceTool();
       st.Begin(Mesh.PrimitiveType.Triangles);
       st.SetColor(new Color(1, 0.3f, 0)); st.SetUV(new Vector2(0, 1)); st.AddVertex(new Vector3(0, 0, 0));
       st.SetUV(new Vector2(0, 0)); st.AddVertex(new Vector3(0, 1, 0));
       st.SetUV(new Vector2(1, 0)); st.AddVertex(new Vector3(1, 1, 0));
       st.GenerateNormals(); st.GenerateTangents();
       AddChild(new MeshInstance3D { Mesh = st.Commit() });
   }
   ```
   Then call `mesh.ClearSurfaces()` and re-add in `_Process` once per second: the mesh changes in place (one upload per change; the old buffers wait three frames in the release queue). A GPU hang or a mesh that goes
   invisible after the first update is the thing to report.
4. `Csg` (a 2 m box with a 2 m box subtracted from one corner, collision on layer 4): a cube with a one-eighth notch cut out of the +X, +Y, +Z corner of Godot space; flat shaded, one material. Walk a physics body onto it: it collides
   (the triangles of the baked mesh). `Cut` (a sphere intersected with a 4 x 4 x 1 slab, placed 5 m along Godot X): a thick disc with a rim, no holes; zoom in on the cut edge for cracks or T-junction sparkle (report).
5. `Sprite` (needs the real PNG): a billboarded quad (it turns about the vertical axis only), the RIGHT half of the texture (`hframes = 2`, `frame = 1`), 16 m across (pixel size 0.5 x 32 px), transparent where the PNG is. Back out of the alpha:
   the material's `transparency` is alpha blend.
6. `Label` draws NOTHING. That is expected; the report calls it unsupported.
7. `Grid` (cells at (0,0,0), (1,0,0) turned by orientation 10, (-1,0,0), and a pillar at (0,1,2)): three 2 m blocks in a row (the middle one rotated a half turn about Godot Y, which a cube does not show; swap in an asymmetric mesh in
   `library.tres` to see it) and a half-metre-offset pillar; walking into a block stops you (the `_GridCollision` body). Block faces touching in a row must not show an inner face flicker.
8. `Tube` and `Ribbon`: a 2 m tall tube (radius 0.25) and a crossed ribbon pair, both straight and centred. `Text` has no mesh.
9. Vulkan (`--backend vulkan`): steps 1, 2 and 3 again. Nothing R7 adds is API-specific (only ordinary meshes), so a difference from D3D12 is a pre-existing Vulkan mesh-path difference worth reporting.
10. AMD RX 7800 XT: no shader, pipeline or constant changed, so a device loss here would be the mesh upload path itself (a staged adopt of a mesh made on the main thread, `uploadMesh` with `cpu == nullptr`) or the ray-traced
    geometry built for a mesh that appears mid-frame by the acquired hook; report the log lines around it.

### R8

Fixture: `AverAssetC fabric tests/fabric/data/EnvVolGame <out> --scenes-only`, then open `<out>/EnvVolGame/EnvVolGame.ocproject`, `Maps/main`. The level holds a WorldEnvironment with volumetric fog on and no geometry: add a floor, a few boxes
(a doorway or pillars the sun can cast through), a sun (the level has none; the fixture is only the fog nodes) and a camera, and place the nodes (`Mist` at (2, 1, 3) m is a 4 x 2 x 6 m cylinder with a FogMaterial; `Haze` is a
world volume with the default material; `Custom` is a box whose ShaderMaterial was dropped). The converter wrote `RENDER.VOLFOG 1` and `RENDER.VOLFOGDENSITY 0`. Back out with `RENDER.VOLFOG 0` (the fog and the volumes
vanish) or by deleting the FogVolume nodes (then the ordinary froxel passes run, which are today's). Log lines to look for: `volumetric fog: fog-volume compute variants ready`, `volumetric fog: the fog-volume compute variants
would not build`, `[EnvFx] N fog volume(s) drawn`, `[EnvFx] N reflection probe(s) registered`. Scripts run only in Play.

0. Defaults unchanged, first: open an existing project or level with no FogVolume (NewSponza_Night, NeonDistrict), once with `voxi.volumetricFog 0` (the default) and once with it on. Nothing may differ from before and the log
   must contain neither `fog-volume compute variants` nor `[EnvFx]`: the variants are built, and the feed pushes, only while a volume is registered.
1. `Mist` (Play so the node publishes): inside the cylinder the air is visibly foggy, a soft-edged column: the sun scatters in it (light shafts through the boxes), the sky tints it, the far end of the volume is hazier than
   the near end when the sun is behind it. Outside it the level's own fog is unchanged. The column's axis is the node's up axis (Godot Y). If it lies on its side or is the wrong size, report which of X / Y / Z is wrong (the half
   sizes are `(size.z, size.x, size.y) / 2` metres on engine X, Y, Z).
2. `height_falloff 0.4`: the column thins going up (e^-0.4 per metre above its lowest point); at 5 m up it is about 14 percent. `edge_fade 0.3`: the outer 30 percent of the radius ramps from nothing to full; set 0.01 for a hard edge.
   `emission (0.1, 0, 0)`: a faint red self-glow that does not depend on the sun (turn the sun off to see it alone). **Report if the glow is white or much brighter than 0.1 x density**: the unit is radiance per unit density.
3. `Haze` (world volume, density 1 per metre): the whole view fogs at 1 per metre, a thick white mist everywhere inside the volumetric fog's range (20000 cm by default). Delete it to go back.
4. Carving: add a second FogVolume (a sphere) with a FogMaterial of density -2 inside `Mist`: a hole of clear air appears; the fog never goes below clear (no brighter-than-sky halo). Report any halo.
5. Many: add 20 volumes in a row. 16 draw (the nearest the camera): walking along the row, the far ones appear as you approach; there is no flicker as the set changes.
6. Moving parent: parent a `Mist` to a node that moves (a script or an animation): the column follows within a frame.
7. Visibility: `Visible = false` on a volume makes it vanish; true brings it back.
8. Temporal: with a volume on, move the camera quickly: the fog does not smear or flash worse than the ordinary volumetric fog does. Toggle a volume off and on: one frame without history, then stable.
9. Vulkan (`--backend vulkan`): steps 0 to 3 again. Volumetric fog needs typed UAV loads and DXC; if it is unavailable the existing warning says so and nothing draws.
10. AMD RX 7800 XT: the new code is a compute variant of the froxel passes only (no pixel shader, no `PSRayDriven`, no register-pressure site). A device loss with a volume registered would be the scatter pass reading a 2 KB
    constant tail; report the log around it and whether `Mist` alone (one volume) loses the device.
11. Reflection probes: place `Probe` (Play) and fly through it: **nothing changes on screen**; the log says `[EnvFx] 1 reflection probe(s) registered; the renderer samples no captured environment yet`. A script can call
    `ReflectionProbe.Select(point, camera, out var rest)`: inside the box with the default 1 m blend it returns the probe with weight 1 and rest 0; 0.5 m from a wall, weight 0.5.
12. LightmapGI / VoxelGI / LightmapProbe: they place and change nothing. Lighting is Aver's GI as before.
