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

## Why this and not NVIDIA NRD

Investigated 2026-08-27; the full write-up is `docs/rendering/DENOISING.md`. In short: NRD is
technically excellent and **is** royalty-free to incorporate, but it ships under the NVIDIA RTX SDKs
License (SPDX `NOASSERTION`), whose grant is to distribute it *"as incorporated in object code format
into a software application"* and **"without the right to sublicense"**. Aver is not an application —
it is an engine redistributed to licensees who build applications — so every licensee would need their
own grant from NVIDIA and Aver's EULA would have to flow NVIDIA's terms through to each of them.
`docs/ASSET_IMPORT.md` accepts MIT/BSD/Apache-2.0/zlib/CC0/CC-BY only.

This is MIT. It costs nothing legally, it is the same vendor as an existing dependency, and its
shadow denoiser is built for precisely our case: **at most one jittered shadow ray per pixel**, which
is what `rtShadow` traces.

## THIS CANNOT RUN YET, AND THAT IS THE POINT OF THIS SECTION

It is vendored ahead of the work it needs, deliberately and with that stated, rather than discovered
later by someone wondering why it is inert. Extracted from the source itself, these are the
accessors the **host** must implement — they are called but never defined by these headers:

| callback | what it needs | does Aver have it? |
|---|---|---|
| `FFX_DNSR_Shadows_ReadDepth(pos)` | depth as a readable texture | **no** |
| `FFX_DNSR_Shadows_ReadNormals(pos)` | world normal as a readable texture | **no** |
| `FFX_DNSR_Shadows_ReadVelocity(pos)` | **screen-space motion vectors** | **no** |
| `FFX_DNSR_Shadows_ReadPreviousDepth(pos)` | last frame's depth | **no** |
| `FFX_DNSR_Shadows_ReadHistory(uv)` | the denoiser's own history | yes, Voxi has one |
| `FFX_DNSR_Shadows_GetReprojectionMatrix()` | previous view-projection | yes, `gPrevViewProj` |

Aver is a **forward** renderer: `PSMainVoxi` returns a single `SV_TARGET`, and normal, roughness and
albedo exist only in that shader's registers — never in a texture a compute pass could read. And
nothing in this engine produces motion vectors at all; `rhi::UpscalerNeeds::MotionVectors` is declared
in `RHIResources.hpp` and its own comment says so.

So the prerequisite is a **thin G-buffer plus motion vectors** — roughly `RG16F` velocity, `R32F`
view-space depth and `RGB10A2` normal+roughness, about 54 MB and 12 bytes per pixel at 2750x1639,
against the ~144 MB the RT histories already cost. Dynamic objects additionally need a previous-frame
transform per instance, which `RtInstance` does not carry today.

That one piece of work also unlocks **FSR 2/3**, **TAA**, **SSR**, and fixes temporal reprojection for
moving objects — which is broken right now, since reprojection transforms *this* frame's world
position through *last* frame's camera and is therefore correct only for geometry that did not move.

It matters more than it did: ray-driven primary visibility is now the default render path, and a
fullscreen ray pass has **no MSAA**, so the default has no antialiasing until something temporal exists.

## Two further requirements worth knowing before wiring it

- **16-bit types.** The shadow filter is written in `float16_t`, so it wants SM 6.2 with native 16-bit
  support. The engine already requires SM 6.5 for `RayQuery`, so this is very likely free — but it is
  a capability to *check*, not assume.
- **The shadow denoiser expects a bitmasked hit buffer**, not a per-pixel float: one bit of visibility
  per pixel, packed 8x4 to a `uint` (`ffx_denoiser_shadows_prepare.h`). `rtShadow` returns a *fraction*
  of the sun disc reached, not a bit. Either the prepare pass is fed a thresholded mask — losing the
  penumbra the disc sampling exists to produce — or that pass is skipped and only the filter reused.
  **This is a real design decision and it is not obviously in FFX's favour**: the engine's own
  `rtShadowSpatial` already consumes a fraction directly.

## Updating

Replace the headers from a tagged upstream release and update the date above. Nothing is modified
from upstream.
