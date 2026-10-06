# Scene lights: point, spot and rectangular area lights, IES profiles, cookies

`scene::CLight` is read by the renderers. Three kinds (point, spot, rectangle), an optional IES
photometric profile, and an optional cookie texture. They are lit and shadowed the way the emissive
lamps already were, through the staged ray-driven local-light path, and the reference path tracer
(`PtSceneView`) next-event-estimates the same lights from the same shader code.

This page is the design record: units, the rectangle model, what is approximated, and where each piece
lives. Verification status is at the end.

## Where things live

| Piece | File |
|---|---|
| Component, kinds, flags | `modules/scene/include/aver/scene/Components.hpp` (`CLight`) |
| Entities -> world-space lights | `modules/scene/include/aver/scene/LightGather.hpp` |
| IES (LM-63) parser, symmetry unfolding, GPU table bake | `modules/formats/include/aver/formats/IesProfile.hpp` |
| `.ocworld` `LIGHT` record | `modules/formats/include/aver/formats/OcWorld.hpp` (`OcLight`), `docs/formats/FORMAT_SPECS.md` section 11 |
| Packing a light for the GPU, CPU mirror of the rectangle maths | `modules/render.voxi/include/aver/voxi/SceneLight.hpp` |
| Shared HLSL (evaluation, IES, cookie, rectangle form factor) | `modules/render.pt/shaders/aver_lights.hlsli` |
| Voxi: light list, assets, shadow rays | `VoxiRenderer.cpp` (`buildLocalLights`, `appendSceneLightCandidates`, `lightAssetIndex`), `voxi_rt.hlsli`, `voxi.hlsl`, `voxi_pt.hlsli` |
| Reference path tracer | `modules/render.pt` (`PathTracer::setLights`, `ptDirectLights` in `pt_pathtrace.hlsl`) |
| Editor Details section, level IO, per-frame feed | `sandbox/src/LightDetails.hpp`, `LightLevelIo.hpp`, `SceneLightFeed.hpp` |

The HLSL lives in `render.pt/shaders` (always built) because both Voxi and the path tracer include it;
Voxi can be configured out, the path tracer cannot.

## The component

`CLight` grew six fields at the end. The component pool zero-fills, so **zero means default** for every
new field and a light saved before they existed reads the same.

| Field | Meaning |
|---|---|
| `kind` | 0 point, 1 spot, 2 directional (the sun owns those; the lamp renderer ignores them), 3 rectangle |
| `colour`, `intensityLux` | linear colour; **candela**: lux at 1 m on axis. (Named lux historically; for a local light the physical quantity is luminous intensity.) |
| `rangeCm` | 0 derives it from the intensity (fades to 0.1 percent of the sun, capped at 50 m); otherwise as authored |
| `innerCos`, `outerCos` | spot cone half-angles as cosines. `outerCos` also frames a cookie on any kind |
| `widthCm`, `heightCm` | rectangle extent along local +Y and +Z; 0 means 100 cm |
| `iesProfile`, `cookie` | `ObjectId` (fnv1a64 of the content-relative path) of the `.ies` / image; 0 none |
| `flags` | `kLightNoShadows`, `kLightIesPeak` |
| `sourceRadiusCm` | point/spot emitter radius, drives penumbra softness; 0 means 1 cm |

**Every light emits along the entity's local +X** (the engine's forward). A rectangle's emitting face
is its +X side; IES nadir (0 degrees vertical) is +X; IES horizontal 0 is +Y. A downlight is
therefore an entity pitched 90 degrees down.

### Units

One engine radiance unit is `kLuminanceToCdm2` = 100000/3 cd/m2 (so the 100000 lux sun is 3.0).

- Point/spot: irradiance at 1 m = `candela / kLuminanceToCdm2`, falling as 1/d2 (d in cm, clamped at
  the emitter radius) and windowed by `(1 - (d/range)^4)^2` like the emissive lamps. This is the
  same law the emissive lamp path uses, so a lamp material and a `CLight` of equal intensity agree.
- Rectangle: a one-sided Lambertian panel of **radiance** `L = candela / area / kLuminanceToCdm2`,
  so `I = L * A` along the normal and a distant panel converges to a point light of the same candela.

## Rectangle lights

**Irradiance.** Diffuse irradiance from a Lambertian polygon is exact and closed form: the vector
irradiance is `1/2 * sum(theta_i * n_i)` over the edges (Lambert; the same formula Arvo and Heitz et
al. use for polygonal lights). `aversLightEval` clips the quad to the hemisphere above the shading
normal (Sutherland-Hodgman, at most 5 vertices), evaluates the sum, and hands the shader its
direction (the flux-weighted direction to the panel) and its length (so `N . dir * |vec| * L` is the
irradiance exactly). The caller's normal multiply is unchanged, so the BRDF code needs no special
case. The edge cross products use `a x (b - a)` on unnormalised vertices, which keeps the digits for
a small distant panel; normalising first leaves a percent-level error at 50 m / 20 cm.

Chosen over LTC: it is exact for the diffuse term with no lookup tables to ship or validate, and the
panel's specular response uses that same centroid direction with the lobe widened by the panel's
angular size (as emissive lamps widen theirs). That is an approximation: a glossy surface shows one
highlight at the centroid, not the panel's shape. LTC would give a shape-correct highlight and is the
upgrade if that matters.

**Shadows.** The penumbra is shape-correct because the shadow ray goes to a point sampled **on the
rectangle**: a radical-inverse / golden-ratio pair (`rdLocalRectSample`) rotated per pixel, stepped
one sample per time the pixel traces, and accumulated by the existing lamp-visibility history. Sampling
is uniform in area; a sample that falls below the receiver's horizon (it contributes no light) is
mirrored through the panel's centre. Solid-angle (spherical rectangle) sampling would lower the variance of a very
close, very large panel; area sampling is unbiased for the visible fraction and was chosen over it
because the visibility is accumulated over many frames anyway and the code is a third the size.

**Not modelled:** emission textures on the panel, two-sided panels, a panel being visible as geometry
(give it an emissive mesh; with `lightIntensity` 0 so it does not also become a sphere lamp).

## IES profiles

`fmt::parseIes` reads ANSI/IES LM-63 (1986 to 2019 headers) **photometric type C**, with `TILT=NONE`,
`INCLUDE` or a named tilt file. Types A and B are refused with a message asking for a Type C export.
The horizontal symmetry is unfolded (single angle, 0-90 quadrant, 0-180 bilateral, 90-270 bilateral,
0-360); outside the file's vertical range the profile is 0, so a downlight file lights nothing above
the horizon.

`fmt::bakeIesTable` resamples to a 64 (vertical, 0-180 degrees inclusive) x 32 (horizontal, wrapping)
table of relative intensity and uploads it as an `R16F` texture. **The table is normalised so its sphere
average is 1**, so `intensityLux` keeps its meaning as the average intensity: the light has the
luminous flux `4 pi * candela` lumens whatever the profile's shape, and a narrow beam is correspondingly
brighter on axis. `kLightIesPeak` instead scales the profile so its brightest direction is exactly
the light's intensity (the UE-style reading). The file's own lumen rating and ballast factor are
ignored; only its shape and candela multiplier matter.

The table is a bindless texture in the ray path's table (`gRtTextures` for Voxi, `gPtTextures` for the
path tracer), so it needs the bindless table; without one an IES light is plain.

## Cookies

An sRGB RGBA8 image projected through the light's frustum: half-angle from `outerCos` (60 degrees for a
point or rectangle with a degenerate cone), looking down the +X axis with +Y as the image's right and +Z as its top. Outside
the image the light is blocked. Sampled at mip 0 (no mip chain, so a large cookie on a distant surface
aliases), multiplied into the light's colour.

## The light list and its limits

Scene lights join the emissive-material lamps in one list, **32 per frame in all**, ranked by 1 m
irradiance over distance squared from the camera (`kMaxLocalLights`). The record is 80 bytes
(`AverLightRec`, five `float4`). The existing machinery applies unchanged: one stochastic shadow ray per
pixel per turn at a light picked by unshadowed luminance, accumulated with reprojection history, with
visibility shared across lights.

- **Direct light only.** Emissive lamps also reach the GI estimators because their glow is geometry
  the bounce rays hit; a `CLight` has none, so it lights surfaces it can see but contributes no
  indirect bounce. To get bounce from a panel, give it an emissive mesh as well (and
  `lightIntensity` 0 on that material so it does not become a second, sphere-shaped light).
- Lights flagged no-shadow are never picked for a ray and are added unshadowed.
- The history is keyed on the byte hash of the list, so **a moving light restarts its visibility
  history every frame**: shadows from a moving light are noisier than from a static one.
- D3D12 only, and only with ray tracing on (`settings.localLights`), like emissive lamps. Without ray
  tracing there are no local lights at all. The raster scene draws (`PSMainVoxi`) light and shadow
  from the same list when ray tracing is on.
- The single-pass ray-driven megakernel (`AVER_RD_SINGLE_PASS`) sits at the register limit, so it compiles
  `AVER_LIGHTS_SIMPLE`: point and spot lights only; rectangles, IES profiles and cookies need the staged path.
- Asset ids are immutable for a session: editing an `.ies` or cookie on disk needs a restart.
- Vulkan is not ported (local lights are D3D12 only today).

## Path tracer agreement

`PathTracer::setLights` takes the same 80-byte records. `ptDirectLights` picks one light by unshadowed
luminance at every diffuse hit, fires one shadow ray at a point on its emitter (a disc for point/spot,
a uniform point on a rectangle) and divides by the pick probability, through the same BRDF as the sun
(`ptDirectBrdfCos`, factored out of `ptDirectSun` with identical arithmetic). Both integrators call
`aversLightEval`, so units, falloff, cone, IES, cookie and the rectangle form factor are one
implementation. Voxi's ray-driven Path Tracing mode (`voxi_pt.hlsli`) uses the same functions through
`ptLampLight`. `PtSceneView` restarts accumulation when the light set changes.

## Host wiring

The renderers take lights from the host; nothing reaches into the scene by itself.

- Each frame, before submitting draws: `editor::SceneLightFeed::update(world, voxiRenderer, ptSceneView, contentDir)`
  (`sandbox/src/SceneLightFeed.hpp`). It gathers `CLight`s (`scene::gatherLights`), loads and registers each
  IES / cookie once (`VoxiRenderer::registerIesProfile` / `registerCookie`, `PtSceneView::lightTexture`),
  and calls `VoxiRenderer::setSceneLights` and `PtSceneView::setLights`.
- Levels: `editor::lightFromRecord` / `transformFromRecord` on load and `recordFromLight` on save
  (`sandbox/src/LightLevelIo.hpp`).

## Editor

Add Component > Light (or the Light section of any entity that has one): type, colour, intensity, range,
cone (degrees), rectangle size, source radius, shadows, IES profile and cookie pickers listing the
project's `.ies` / image files, and the "Intensity = IES peak" switch. Levels save lights as `LIGHT`
records (`docs/formats/FORMAT_SPECS.md` section 11).

## Verification

Built only by the integration step; nothing here has run on a GPU when this page was written.

- `LightFormatTest`: IES parse, symmetry, normalisation, half floats, refusals; `LIGHT` round trip.
- `SceneLightTest`: unit conversion and packing; the rectangle form factor against closed forms
  (distant point limit, infinite panel = pi, vertical wall = pi/2 by horizon clipping, one-sidedness);
  the HLSL's record layout and constants against the header.
- `SceneTest`: the new `CLight` fields and `gatherLights`.
- `LightDetailsTest`: the editor's asset listing, ids, cones and level records.
- Still to check by eye on a device: shader compile (`VoxiShaderCompileTest`), a rectangle's penumbra
  shape against `Path Tracing`, an IES downlight's scallops on a wall, a cookie's orientation.
