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
- `include/aver/sr/AverSrFsr.hpp` / `src/AverSrFsr.cpp` / `shaders/sr_fsr1.hlsl` — `FsrUpscaler`:
  **AMD FSR 1** (`third_party/fidelityfx-fsr`, MIT) — EASU edge-adaptive upscale then RCAS
  sharpening, with optional FXAA-class edge AA on the source first. Three fullscreen passes; FSR
  expects display-range input and this runs before the tonemap, so the chain squashes HDR with
  `c / (1 + max(c))` and undoes it after RCAS. The editor uses it for every AverSR level, for a
  manual render scale below 1 (which used to be a plain bilinear stretch), and for edge AA at native
  scale; the packaged runtime uses it for its AverSR levels. Falls back to `SpatialUpscaler` if its
  pipelines will not build, and on a backend whose caller cannot let it bind its own targets
  (`UpscalerInput::canRetarget`, false on Vulkan today). Measured on NeonDistrict_Day at 0.5 scale:
  85% of native edge sharpness versus 64% for the bilinear stretch.
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

**Backend wiring is done** (this README once claimed otherwise, and the editor's own AverSR tooltip repeated the claim, so the one control that meaningfully reduces frame time on a high-DPI display told anyone who hovered it that it did nothing). `IDevice` has the hook — `RHI.hpp:315-316`, `virtual void setUpscaler(IUpscaler* u)` / `virtual IUpscaler* upscaler() const` (default no-op, so every existing backend is unchanged by construction) — and a backend's composite step reads it: `upscaler_->execute(*rhiContext_, in, presentHdrTex_)` at `modules/rhi.d3d12/src/D3D12Device.cpp:4845` (the composite pass is the one place that owns the scene-resolution colour target as an addressable resource) and `modules/rhi.vulkan/src/VulkanDevice.cpp:3827`. Line numbers drift with unrelated edits; if they no longer match, grep for `setUpscaler`/`upscaler()` in `RHI.hpp` and `upscaler_->execute` in both device files. The D3D12 composite runs the resample in HDR before the tonemap, and VulkanDevice mirrors it. A non-`Off` level produces AverSR's own Catmull-Rom pixels, not a backend bilinear stretch.

*Measured* at 2750x1639 PTTest: the ray-driven primary pass (largest single span) goes 5.0ms at Off to 2.3 / 1.7 / 1.3ms at Quality / Balanced / Performance.

This was the third stale "X is not implemented" claim found in this tree in one day, after a shadow-denoiser comment asserting an inert change that was live at every tier, and a sky function documented as unread that two paths consumed. None was a code bug, and all three hid working behaviour — a feature is not shipped while the thing describing it says it is not.

Built as its own module (`AVER_MODULE_SR=ON`, the default); `sandbox` links it (`sandbox/CMakeLists.txt`) and constructs `SpatialUpscaler` for real when a non-`Off` `--aversr` level or Editor Preferences combo selection is applied. **What is still true:** those PTTest numbers measure *frame time*, not pixels — `SpatialUpscaler::execute()` has still never been proven correct against a live swapchain by a GPU capture (`docs/AVERSR.md` says the same).

## What's NOT here yet

- **A temporal (`aver::sr`-native) upscaler.** Needs render-scale, sub-pixel jitter, per-pixel
  screen-space motion vectors, depth, exposure and a camera-cut reset signal as whole-frame data —
  none of which exists yet outside Voxi's ray-traced-shadow-only reprojection. See
  `docs/AVERSR.md` "Prerequisites".
