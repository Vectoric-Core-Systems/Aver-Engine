# Denoising

**The denoiser is AMD FidelityFX Denoiser (MIT)**, vendored at `third_party/fidelityfx-denoiser` and
driven by `modules/render.denoise` (`aver::render::denoise::Denoiser`). It runs FidelityFX's
reflection pipeline at roughness 1 as a diffuse denoiser over two Voxi signals, the sky-occlusion hit
distance and the ReSTIR GI radiance, on D3D12 only. It reads the D3D12 G-buffer (velocity, view Z,
normal + roughness). Staged ray-driven reflections go through its reflection pipeline as designed
(section 4, Denoise Reflections, on by default); shadows, and reflections elsewhere, use the
hand-written filters in section 3.

---

## 1. Fitting FidelityFX to Voxi's units (2026-10-04)

The vendored headers are tuned for AMD's sample: depth in [0,1] and reflection radiance near 1.
Voxi feeds centimetre view Z and ReSTIR GI radiance that is often near 0.01. Two of the filter's
mechanisms were silently off because of that. The headers stay unmodified; `aver_denoise.hlsl`
adapts its inputs instead.

- **The spatial pass did nothing.** The prefilter's edge-stopping weight is
  `exp(-|zc - zn| * zc * 4)`. At zc = 1000 cm, any neighbour more than a hair off the centre depth
  weighs zero. The host callback now hands it `sqrt(10 ln(z + 1))`, for which the same formula
  becomes `exp(-20 |dz| / z)`: a relative test, e^-1 at 5%.
- **Variance and firefly rejection assumed brighter input.** A first pass, `CSDenoiseScale` (colour
  only, one 16x16 group), reduces last frame's 8x8 averages to a frame scale that brings the mean
  luminance to 0.25. It eases 20% per frame in log space, is clamped to [0.01, 1e4], and is stored in a
  1x1 R32F texture. Radiance is multiplied by the scale on its way in and divided on its way out to
  history and to the 8x8 average, so stored values stay in scene units.
- **Half-rate reconstruction blurred silhouettes.** A skipped checkerboard pixel averaged its four
  traced neighbours blindly. They are now weighted by relative depth (`exp(-32 |dz|/z)`) and normal
  agreement (`dot^8`), with the plain mean kept as the fallback when no neighbour agrees.

Voxi-side history fixes landed with it:

- **Denoised GI on disocclusion** uses this frame's ReSTIR estimate, blended in by the valid bilinear
  weight, instead of last frame's value at this pixel (another surface's light) (`voxi_restir.hlsli`).
- **Rough reflections at speed** keep some history: the fresh weight at speed falls from 1.0 (near
  mirror) to 0.35 (roughness 0.5) (`voxi.hlsl`).
- **Ray-traced AO** eases its history weight toward 0.75 when a change exceeds the trace's own noise
  (0.75 / sqrt(rays)), so a passing occluder no longer trails for ~30 frames (`voxi_rt.hlsli`). Same
  idea as `rtShadowChanged` in [MOVING_OBJECTS_HISTORY.md](MOVING_OBJECTS_HISTORY.md).

## 2. Brightness: FidelityFX's own config (2026-10-04/05)

Measured GI-only on linear values: a capture minus the same frame at `voxi.giIntensity 0`, sRGB
decoded, so direct light, sky and editor chrome cancel. (The earlier "under 1% bias" figure for the
0.25 scale came from averaging 8-bit sRGB values of the whole frame, which hides most of the GI and
reads noisy images darker; it was wrong.)

With the readback bypassed the frame's GI equals the raw ReSTIR estimate exactly, so the loss is in
FidelityFX's output. As vendored it returned **0.49** of its input's mean (0.66 with the half-rate
checkerboard) on NeonDistrict Day: its 8x8 "average" weights each pixel by `exp(-0.3 luma)` and its
neighbour weight by `exp(-0.6 |avg - x|)`, and both pull a skewed 1-spp signal toward its dark values.

`aver_denoise.hlsl` therefore supplies its own copy of FidelityFX's config (the header is guarded by
`FFX_DNSR_REFLECTIONS_CONFIG`) with the vendored values except:

| constant | vendored | Aver | why |
|---|---|---|---|
| `AVG_RADIANCE_LUMINANCE_WEIGHT` | 0.3 | 0.0 | plain 8x8 mean |
| `RADIANCE_WEIGHT_BIAS` | 0.6 | 0.2 | see below |
| `RADIANCE_WEIGHT_VARIANCE_K` | 0.1 | 0.0 | |
| `Settings::denoiserHistoryClipWeight` | 0.5 | 4.0 | the narrow box clipped bright samples |

Both weights fully off gave 0.79, plus the clip at 4 gave 0.84. Fully off, though, a night
path-traced signal, whose light arrives mostly as rare bright samples, turned into soft blotches in
motion (NeonDistrict Night, moving camera). A radiance weight of 0.2 keeps about 70% of the recovered
light, about 2x the vendored setting, with the blotches mostly gone. Ruled out by measurement as the
blotch source: the NeuRaC cache read, ReSTIR reuse, the history clip, a prefilter variance floor, and
an input firefly clamp (16x the frame mean already removed over half the night GI).

Still open: the remaining loss (~0.84 by day), suspected in the variance-guided prefilter; the
reflection, shadow and AO histories read the nearest texel rather than a bilinear footprint;
`denoiserSunMovingSamples` below 8 has no visible effect.

## 3. The hand-written filters

| | |
|---|---|
| Spatial shadow filter | `rtShadowSpatial` (`voxi_rt.hlsli`). Gaussian gather over the shadow history with a plane-distance accept/reject. Radius 2 at Low/Medium/High, 1 at Epic, off at Off. |
| Spatial reflection filter | `rtReflectionSpatial`. Radius from roughness, so it needs no dial and is a no-op on a mirror. |
| Temporal reprojection | `rtReprojectHistory` / `rtReprojectReflection`: this frame's world position through last frame's view-projection, with per-instance previous transforms for movers. |
| Ray-tile amortisation | `rtPixelsPerRayTile` (1 at every tier: every pixel traces every frame). |

Reflection work done for these filters (2026-08-27):

- **`rtReflection` traces a real roughness lobe**, a cone of `tan = rough^2` (the GGX alpha) on the
  same nested disc sequence as the sun-disc shadow. At roughness 0 it reduces to the mirror ray.
- **Temporal accumulation on the untiled reflection path**, only where the lobe adds variance.
- **Gaussian rather than flat kernels**: a box filter rings as a square-edged halo around a bright
  feature.
- **The reflection cutoff is roughness 0.75**, with the fade a seam-hider across the last quarter.

## 4. Reflections through the denoiser (2026-10-05)

`Signal::Reflection`, compiled as `AVER_DNSR_REFLECTION`: real roughness from the G-buffer (glossy up
to 0.75, mirror below 0.1), the hit distance in the input's alpha, and FidelityFX's parallax
reprojection through the reflected point (camera block in `AverDenoiseCB`: the G-buffer frame's
camera-relative inverse view-projection and the frame before's view-projection, kept by Voxi one
frame deeper than its own history). `CSRdRefl` writes one raw sample + hit distance to u23
(glossy pixels at half rate on a pixel checkerboard, skipped pixels marked a = -1 and rebuilt by the
denoiser from their traced neighbours) and shows last frame's result from t23, read where the
reflected point was on last frame's screen. Off (`voxi.denoiseReflections 0`), Voxi's own reflection
history and filter run as before. Raster and Vulkan paths do not denoise reflections.

**FidelityFX's prefilter is skipped for reflections.** Its radiance weights dropped sparse bright
reflections: NeonDistrict Night moving, road patches kept 25-35% of their light (whole frame -2.6%).
Bypassed, the light is kept (-1%) and the speckle still goes, since the temporal pass along the
reflected point is what removes it. A 4x or 16x wider history clip changed nothing.

NeonDistrict Night, render scale 0.5, moving (`--cam-wander 1.5 2.3`), GI off:

| | speckles (per mille) | frame light | GPU total |
|---|---|---|---|
| Voxi's filter | 1.11 | 0.01121 | 45.9 ms |
| denoiser, full rate | 0.28 | 0.01091 | 54.9 ms |
| denoiser, half rate, no prefilter (shipped) | 0.28 | 0.01110 | 48.3 ms |
| no reflection rays at all | 0.21 | -- | -- |

Still, total image against the reference path tracer: 0.982 Voxi's filter, 0.979 denoised. Cost
about +1.5 to 2.5 ms (run-to-run spread is ~2 ms); the reflection targets add ~74 MB at 1766x994.

## 5. Specular anti-aliasing (2026-10-05)

The wet-road speckle left after section 4 was ALIASING, not noise: present on a still frame, gone
without reflection rays, unchanged with deterministic hit shading. The road's normal map scatters
near-mirror rays across a city of small bright lights, one unjittered ray per pixel, so no denoiser
had varying samples to average. Fixed at the surface with Toksvig: normal-map mips now average as
vectors WITHOUT renormalising (`fmt::generateMipChain`, length floored at 0.25; the texture cache
key retires old normal-map entries), `AverMaps/AverAuthored::normalLen` carries the filtered length,
and `averComposeSurface` widens GGX alpha^2 by (1 - len) / len for raster and ray hits alike.
Assets cooked before this keep renormalised mips until re-imported.

NeonDistrict Night moving, GI off: speckles 1.11 -> 0.35 per mille alone, 0.26 with reflection
denoising; no measurable cost; whole still frame 11.77 -> 11.75 (8-bit mean, tonemapped).

## Sources

- [AMD FidelityFX Denoiser](https://gpuopen.com/fidelityfx-denoiser/) and its
  [1.3 manual](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/denoiser/)
- [GPUOpen-Effects/FidelityFX-Denoiser](https://github.com/GPUOpen-Effects/FidelityFX-Denoiser)
- In-tree: `third_party/fidelityfx-denoiser/README.md`, `modules/render.denoise/README.md`,
  `docs/ASSET_IMPORT.md` (licence policy)
