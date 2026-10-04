# AMD FidelityFX Denoiser (FFX-DNSR)

| | |
|---|---|
| Upstream | https://github.com/GPUOpen-Effects/FidelityFX-Denoiser |
| Licence | **MIT** (see `LICENSE.txt`, Copyright (c) 2021 Advanced Micro Devices, Inc.) |
| Vendored | 2026-08-27 |
| Contents | 9 HLSL headers, 1,677 lines — the shadow denoiser and the reflection denoiser |

Header-only shader source. **No library, no binaries, no build system, no CMake, no C++ side** —
exactly the shape `third_party/fidelityfx-fsr` already has, and vendored to the same convention.
Nothing here is modified from upstream; a diff against master should be empty.

```
ffx_denoiser_shadows_util.h                 71    tile/lane addressing, from ffx_a.h
ffx_denoiser_shadows_prepare.h              53    pack the raytracer's hit mask into 8x4 tiles
ffx_denoiser_shadows_tileclassification.h  425    reprojection, moments, per-tile classification
ffx_denoiser_shadows_filter.h              273    the edge-avoiding a-trous passes
ffx_denoiser_reflections_config.h           40
ffx_denoiser_reflections_common.h          142
ffx_denoiser_reflections_prefilter.h       160
ffx_denoiser_reflections_reproject.h       368
ffx_denoiser_reflections_resolve_temporal.h 145
```

## How Aver uses it

`modules/render.denoise` (`Aver.Render.Denoise`) runs the **reflection** pipeline — reproject,
prefilter, temporal resolve — driven at roughness 1 as a diffuse denoiser, over Voxi's two noisy
signals: the sky-occlusion hit distance and the ReSTIR GI radiance. The shadow denoiser
(`ffx_denoiser_shadows_*.h`) is still unused. See `modules/render.denoise/README.md` for the host side.

It was chosen over NVIDIA NRD (investigated 2026-08-27, `docs/rendering/DENOISING.md`) because NRD
ships under the NVIDIA RTX SDKs License, whose grant is to distribute it "as incorporated in object
code format into a software application" and "without the right to sublicense". Aver is an engine
redistributed to licensees, not an application, so that licence could not flow through. This is MIT,
from the same vendor as `third_party/fidelityfx-fsr`, and `docs/ASSET_IMPORT.md` accepts MIT.

The host-implemented accessors these headers call but never define are all satisfied now: the Voxi
G-buffer supplies depth, octahedral normal + roughness and screen-space motion vectors, the previous
frame's matrices and depth come from the renderer, and the denoiser owns its history textures.
The G-buffer exists only on D3D12, so the denoiser runs only there.

## Build and deployment

The code here is **unmodified** from upstream. It is compiled at run time by DXC, through the
shader compilers' opt-in `AVER_HLSL_2018` flag-define: the vendored code uses a vector `?:` that
HLSL 2021 rejects, so that one shader (`modules/render.denoise/shaders/aver_denoise.hlsl`) is built
as HLSL 2018. The headers deploy to `bin/shaders/FidelityFX` together with `LICENSE.txt`, and the
licence is carried into `THIRD-PARTY-NOTICES.txt` by the stagers.

## Requirements worth knowing

- **16-bit types.** The shadow filter is written in `float16_t`, so it wants SM 6.2 with native 16-bit
  support. The engine already requires SM 6.5 for `RayQuery`, so this is very likely free — but it is
  a capability to *check*, not assume. (The reflection pipeline in use does not depend on it.)
- **The shadow denoiser expects a bitmasked hit buffer**, not a per-pixel float: one bit of visibility
  per pixel, packed 8x4 to a `uint` (`ffx_denoiser_shadows_prepare.h`). `rtShadow` returns a *fraction*
  of the sun disc reached, not a bit. Either the prepare pass is fed a thresholded mask — losing the
  penumbra the disc sampling exists to produce — or that pass is skipped and only the filter reused.
  This is a real design decision and it is not obviously in FFX's favour: the engine's own
  `rtShadowSpatial` already consumes a fraction directly.

## Updating

Replace the headers from a tagged upstream release and update the date above. Nothing is modified
from upstream.
