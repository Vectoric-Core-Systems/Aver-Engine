# Volumetric fog (light shafts)

A camera-aligned froxel volume of sun light scattered by the air, shadowed by the scene, added on top of the existing exponential height fog. Code: `modules/render.voxi/src/VoxiFroxelFog.cpp` + `include/aver/voxi/VoxiFroxelFog.hpp` (resources, passes), `shaders/voxi_froxel.hlsl` (the compute passes and the sky draw), `shaders/voxi_froxel.hlsli` (grid maths and lookup, shared with `voxi.hlsl`). Settings are `Settings::volumetricFog*` (`Voxi.hpp`); console vars are `voxi.volumetricFog` and `voxi.volFog*`.

**Off by default, and off costs nothing**: no texture, pipeline or binding exists, and every shade site fails one uniform test (`gShadowDraw.w`) before doing anything. Every existing level renders exactly as before.

## 1. What it is, and what it is not

| | |
|---|---|
| The height fog (`SkyAtmosphere` `fogDensity`/`fogHeight`/`falloff`, `averApplyFogAirVis`) | Unchanged. Still analytic, per pixel, and still owns the extinction of its own density and its sky-coloured in-scatter. |
| The froxel volume | Adds the **sun**: light scattered toward the eye along the view ray, with the scene's shadows, through the height fog's density (`volumetricFogHeightFog`) and an optional uniform medium (`volumetricFogDensity`). The uniform medium also dims what is behind it and scatters sky light. |

So the two never double-count. The height fog's extinction is applied once, analytically; the volume's transmittance is that of the *uniform* medium only. A pixel is `fogged * T + S` where `fogged` is the colour after the analytic fog.

With `volumetricFogDensity = 0` and no authored height fog there is nothing to scatter and the effect is invisible; with `volumetricFogDensity = 0` inside an authored fog it is shafts through that fog alone.

## 2. The volume

`160 x 90 x 64` by default (`volumetricFogGridX/Y/Z`, clamped 16-320 / 9-180 / 16-128). x/y span the camera's NDC; z is exponential in **view depth** from 10 cm to `volumetricFogMaxDistance` (default 200 m). Three `RGBA16F` volumes: two scatter volumes ping-ponged for history, and the integrated output. 160x90x64 is 7.4 MiB each, 22 MiB in all (logged at creation). Past the maximum distance, surfaces and the sky read the last slice.

The grid is built and read through the **jittered** `gViewProj`, so the temporal-AA sub-pixel jitter cancels exactly between the pass and the shade site.

## 3. The passes

Recorded in `VoxiRenderer::prePass`, after the acceleration structure and shadow map are final (`recordFroxelFog`, GPU span "Voxi volumetric fog"). They are their own compute shaders, not part of `voxi.hlsl`'s translation unit, so they cannot grow `PSRayDriven`/`PSMainVoxi`.

1. **`CSFroxelScatter`**, one thread per froxel.
   - Reads the medium at a point jittered inside the slice (a per-froxel offset advanced by the golden ratio each frame, so the history averages the slice's depth range): `sigma = heightFog * share + uniform`.
   - Sun visibility: a ray query to the sun against the TLAS when it is built (ray-tracing frames), else the cascade shadow map (raster frames), else unshadowed. Cutouts count as solid; translucent instances are ignored.
   - Source radiance per unit extinction: `E_sun * HG(cos, g) * visibility * intensity`, plus, for the uniform part, the fog's sky colour (`averFogInscatterRef`, tinted by `fogColor`, scaled by the air-visibility volume when `voxi.fogOcclusion` has built one) weighted `sigma_uniform / sigma`. `E_sun` is `averSunRadiance()`, the same irradiance the surfaces are lit with.
   - Blended with last frame's result, reprojected through the previous view-projection (`volumetricFogTemporal` = history weight). History is dropped for a frame the volume was not recorded the frame before, after a resize, and on a camera cut when the G-buffer is on.
2. **`CSFroxelIntegrate`**, one thread per (x, y) column, front to back over z. Exact integration of a constant source over each slice: `S += T_total * L * (1 - exp(-sigma dt))`. Output texel z = (`S`, `T_uniform`) from the camera to the **far** boundary of slice z, so the shade sites interpolate boundary values and the first row blends in from "nothing yet".
3. **`PSFroxelSky`** (from `VoxiRenderer::transparentPass`): the sky is drawn by the device after the scene and no shade site reaches it, so a fullscreen draw at the far plane (depth `<=` 1.0, the same pixels the sky fills) applies the volume's last slice. Premultiplied blend: `rgb = S`, `a = 1 - T`.

## 4. Where it is applied

`voxi.hlsl` calls `voxiFroxelCompose` / `voxiFroxelApply` right after each existing fog site, using the pixel's world position (so no depth texture is needed):

- `PSMainVoxi`, opaque: `radiance = radiance * T + S`.
- `PSMainVoxi`, blended (glass, water): folded into the pane's own `(ext, in)` fog pair, so the backdrop behind it keeps the volume at *its* depth.
- `PSRayDriven` and the staged Stage B, including the NRD2 split (`nrdFogExt`/`nrdFogIn` take `T` and `S`, so the denoised lighting is remodulated under the same extinction).
- `PSRayDriven`'s in-path glass and water (`rdTranslucentPath`): `rdFogGlass` applies the volume at the first crossing to what the glass adds, and the opaque surface or sky behind keeps the volume of its own depth. Over the sky that pixel leaves the far plane, so the sky term takes the far slice there itself.

The volume is read as typed **UAV loads** (`RWTexture3D<float4>` at `u24` of Voxi's main table 0, trilinear by hand, 8 loads), because table 0 is full on the SRV side (`kMaxBindingSlots` = 25). It rests in `UnorderedAccess` for the whole frame, so neither the scene pass nor `transparentPass` needs a barrier. The depth mapping (`near`, `1/ln(far/near)`) and the "volume is live" flag travel in `gShadowDraw.yzw`, which the depth-only pass leaves unused (it reads `.x`); `cbuffer VoxiFrame` is untouched, so its three mirrors stay valid.

Needs `DeviceCaps::typedUavLoads`, SM 6.0 and DXC (the same gate as the NRD2 converge pass). Ray-query shadows additionally need DXR 1.1 and SM 6.5; without them the scatter pass is compiled without the ray query and uses the cascade map.

## 5. Settings

| `Settings` field | console var | default | |
|---|---|---|---|
| `volumetricFog` | `voxi.volumetricFog` | off | live; off frees everything |
| `volumetricFogDensity` | `voxi.volFogDensity` | 0.00002 /cm | uniform extinction (0-0.01); 0.001 is thick mist |
| `volumetricFogHeightFog` | `voxi.volFogHeightFog` | 1 | share of the authored height fog the shafts see (0-8) |
| `volumetricFogIntensity` | `voxi.volFogIntensity` | 1 | scales the sun's scattered light only (0-16) |
| `volumetricFogAnisotropy` | `voxi.volFogAnisotropy` | 0.6 | Henyey-Greenstein g (-0.95 to 0.95) |
| `volumetricFogTemporal` | `voxi.volFogTemporal` | 0.85 | history weight (0-0.98) |
| `volumetricFogMaxDistance` | `voxi.volFogDistance` | 20000 cm | where the volume ends |
| `volumetricFogGridX/Y/Z` | `voxi.volFogGridX/Y/Z` | 160 / 90 / 64 | re-creates the volume |

The switch and the uniform density are persisted in the project manifest since G4 R8: `RENDER.VOLFOG 0|1` and `RENDER.VOLFOGDENSITY <per cm>` (`OcProject.hpp`, `ProjectRenderApply.hpp`; docs/formats/FORMAT_SPECS.md). The other settings are not persisted, and none is in the Details panel; console and `Settings` only.

## 6. Known limits

- **Not fogged**: the water and particle shaders (they apply the analytic fog only, in their own pixel shaders), and secondary hits (reflections, GI bounce).
- **In-path glass and the analytic fog**: `rdFogGlass` applies only the volume. The analytic height fog and aerial (which the blended replay does apply) are left off in-path glass, because over long distances their in-scatter converges to the fog colour, not the horizon sky: open sea would darken into a band under the horizon (measured on Caldera: extinction 0.08-0.24 at the far sea, in-scatter a darker blue than the sky). Fixing that convergence is a fog-model change, separate from this volume.
- **Glass over sky**: the sky pass applies the far slice to every pixel still at the far plane. A blended pane in front of the sky is drawn before it and does not write depth, so it takes a little of the sky's fog.
- **Resolution**: 16 px froxels at 2560x1440 soften shaft edges; raise `GridX/Y` or shorten the distance to sharpen.
- **Foliage**: cutouts block the sun's ray as whole cards, so shafts through trees are darker than real ones.
- **Cost on AMD**: the shade sites add eight UAV loads to the `PSRayDriven`/Stage B pixel shaders. The off path is one uniform branch. If the register-pressure hangs ever show up with the volume on, the loads can move into a small post-pass without touching the volume.

## 7. Local fog volumes (G4 R8)

Godot's `FogVolume` + `FogMaterial` as shaped regions of extra fog inside this volume. R8 of [G4_RENDER.md](https://github.com/Vectoric-Core-Systems/Fabric-Connector/blob/main/docs/plans/G4_RENDER.md) has the contract; the pieces:

- **Data**: `framework_envfx_abi.h` (`aver_fw_fogvol_*`, C#: `Aver.Framework.EnvFx`). A script or a converted level (`Aver.Fabric.FogVolume`) publishes a box / ellipsoid / cylinder / cone / world volume with density per metre (negative carves), albedo, emission, height falloff and edge fade. `Runtime/include/aver/game/EnvFxFeed.hpp` (both hosts, after the light feed) packs the 16 nearest the camera into a `FogVolumeBlock` (`aver/core/FogVolume.hpp`) and hands it to `VoxiRenderer::setFogVolumes`. With none registered nothing is pushed and the passes below are today's.
- **Shader**: `voxi_froxel.hlsl` compiled with `AVER_FOG_VOLUMES=1` (both compute entries; built the first frame a volume exists, `VoxiFroxelFog::buildVolumePipelines`). The block is a tail of `FroxelCB` (97 float4). Per froxel the scatter pass sums the volumes' signed extinction at the jittered point and adds `albedo * density * (sun + sky light) + emission * density` to the radiance; this variant stores *(radiance per cm, extinction per cm without the analytic height fog)* so the transmittance the shade sites read leaves the height fog out, as before, and the integrate pass adds the height fog's extinction back at the slice's middle for the in-scatter. Switching between the ordinary and the volume passes drops the temporal history for a frame.
- **Needs this fog on** (`RENDER.VOLFOG 1`, `voxi.volumetricFog`): as in Godot, where a FogVolume needs `volumetric_fog_enabled`.
- **Limits**: 16 volumes per frame; the density is uniform inside the shape (no 3D density texture, no custom fog shader); edge fade and height falloff follow Godot's documented behaviour, not its code; a negative volume cannot make the medium thinner than zero; the nearest-first cut ignores a volume's size.
