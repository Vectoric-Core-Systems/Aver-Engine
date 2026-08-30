# Aver.Render.Sr (AverSR)

The engine's upscaling subsystem. Full design in `docs/AVERSR.md`: naming conventions, module
boundaries, the licensing argument for why NVIDIA DLSS has no slot filled here, and the quality
tiers a future caller maps onto `IDevice::setRenderScale`.

The **seam** — `aver::rhi::IUpscaler`, `UpscalerNeeds`, `UpscalerInput` — lives in `Aver.RHI`
(`modules/rhi/include/aver/rhi/RHIResources.hpp`, beside `IRenderFeature`), not here. This module
provides **implementations** of that seam and links `Aver.RHI` (never the other way around): no
renderer, no other module, links `Aver.Render.Sr` to know it exists. A renderer holds an
`IUpscaler*` (via `IDevice::setUpscaler`) that may be null and calls it if it is not.

## What's here

- `include/aver/sr/AverSrSpatial.hpp` / `src/AverSrSpatial.cpp` — `SpatialUpscaler`: a
  dependency-free, non-temporal, non-learned bicubic (Catmull-Rom) resample. This is **not** FSR
  and must never be described as FSR or as "AI" anything — it is the honest built-in floor, built
  entirely from the generic RHI (`IResourceFactory` + `IRenderContext`) so it carries no
  backend-specific code of its own. It reads only the scene colour target (`needs()` answers
  `UpscalerNeeds::None`); depth, motion vectors, jitter and history are accepted by the seam but
  ignored here.
- `include/aver/sr/AverSrQuality.hpp` — `sr::Quality` (`Off`/`Quality`/`Balanced`/`Performance`),
  `renderScaleFor()` and `qualityName()` for the render-scale table in docs/AVERSR.md "Quality
  levels", plus a case-insensitive `parseQuality()` for a CLI flag or a UI combo. Header-only,
  RHI-free — a caller-facing convenience, not part of the `IUpscaler` seam itself.

## Who links this module

`sandbox` (the editor) does, as of this phase — see `sandbox/CMakeLists.txt`'s `if(TARGET
Aver.Render.Sr)` block. It is the **composition root**: the only place concrete enough to
`new aver::sr::SpatialUpscaler(...)` and hold the result behind an `IUpscaler*`, exactly as
docs/AVERSR.md's "nothing may depend on AverSR" section describes a host doing. `Aver.RHI` itself
still never links this module and never will; the seam stays the only thing a renderer sees.

Sandbox exposes an `--aversr <level>` flag and an "AverSR" combo in Editor Preferences → Display,
both driving `sr::renderScaleFor()` into the same `IDevice::setRenderScale()` `--render-scale`
already used, and constructing a live `SpatialUpscaler` once a non-`Off` level is selected. `Off`
(the default, whether or not `--aversr` was ever passed) runs none of that: no render-scale change
beyond whatever `--render-scale` itself asked for, no `SpatialUpscaler` construction, nothing under
the `[AverSR]` log tag — the same code path as a tree with no AverSR in it.

## What's NOT here yet

- **AMD FSR.** `third_party/fidelityfx-fsr` is vendored (MIT), but nothing here wraps it in an
  `IUpscaler` yet — that is the next implementation this module gains, alongside `SpatialUpscaler`,
  without `IUpscaler` itself changing.
- **A temporal (`aver::sr`-native) upscaler.** Needs render-scale, sub-pixel jitter, per-pixel
  screen-space motion vectors, depth, exposure and a camera-cut reset signal as whole-frame data —
  none of which exists yet outside Voxi's ray-traced-shadow-only reprojection. See
  `docs/AVERSR.md` "Prerequisites".
- **Backend wiring — DONE, and this section said otherwise for far too long.** The paragraph that
  stood here declared `IDevice` had no `setUpscaler`/`upscaler()` hook and that nothing called
  `SpatialUpscaler::execute()`, and it said it had been "checked again this phase". Both halves are
  false and were false when written down most recently:

      modules/rhi/include/aver/rhi/RHI.hpp:322   virtual void setUpscaler(IUpscaler* u)
      modules/rhi/include/aver/rhi/RHI.hpp:323   virtual IUpscaler* upscaler() const
      modules/rhi.d3d12/src/D3D12Device.cpp:4630 upscaler_->execute(*rhiContext_, in, presentHdrTex_);

  The D3D12 composite runs the resample in HDR before the tonemap, and VulkanDevice mirrors it. A
  non-`Off` level produces AverSR's own Catmull-Rom pixels, not a backend bilinear stretch.

  WHY THIS MATTERED RATHER THAN BEING A TYPO. The editor's own AverSR tooltip repeated the same
  claim, so the one control that meaningfully reduces frame time on a high-DPI display told anyone
  who hovered it that it did nothing. MEASURED, PTTest at 2750x1639: the ray-driven primary pass --
  the largest single span in the frame -- goes 5.0ms at Off to 2.3 / 1.7 / 1.3ms at Quality /
  Balanced / Performance. A feature is not shipped while the thing describing it says it is not.

  This is the third stale "X is not implemented" claim found in this tree in one day, after a
  shadow-denoiser comment asserting an inert change that was live at every tier, and a sky function
  documented as unread that two paths consumed. The pattern is worth more attention than any one of
  them: none was a code bug, and all three hid working behaviour. Closing the ORIGINAL gap needed a
  `setUpscaler`/`upscaler()` pair on `IDevice` (default no-op, so every existing backend is
  unchanged by construction) AND a backend's composite step reading it — concretely
  `modules/rhi.d3d12/src/D3D12Device.cpp`'s composite pass (see its own comment starting "dst is
  present-space... THIS IS the actual render-scale upscale"), which is the one place that owns the
  scene-resolution colour target as an addressable resource today. Neither file was touched this
  phase: both were out of this change's file ownership, not skipped by oversight.

## Verified this phase

Built as its own module (`AVER_MODULE_SR=ON`, the default) alongside the rest of the engine, now
with `sandbox` linking it (`sandbox/CMakeLists.txt`) and constructing `SpatialUpscaler` for real
when a non-`Off` `--aversr` level or Editor Preferences combo selection is applied. **Not** verified
by a GPU capture: `SpatialUpscaler::execute()` has still never been run against a live swapchain
(or against anything), because nothing yet calls it — see "Backend wiring" above, which is the
same gap the previous phase found, not yet closed.
