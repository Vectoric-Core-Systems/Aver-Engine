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

## What's NOT here yet

- **AMD FSR.** `third_party/fidelityfx-fsr` is vendored (MIT), but nothing here wraps it in an
  `IUpscaler` yet — that is the next implementation this module gains, alongside `SpatialUpscaler`,
  without `IUpscaler` itself changing.
- **A temporal (`aver::sr`-native) upscaler.** Needs render-scale, sub-pixel jitter, per-pixel
  screen-space motion vectors, depth, exposure and a camera-cut reset signal as whole-frame data —
  none of which exists yet outside Voxi's ray-traced-shadow-only reprojection. See
  `docs/AVERSR.md` "Prerequisites".
- **Backend wiring.** No renderer currently calls `IDevice::setUpscaler`/`upscaler()` or
  `IUpscaler::execute()` — those hooks exist on `IDevice` (default no-op) so a future phase can
  register `SpatialUpscaler` (or FSR) in place of a backend's own inline resize, without this
  module or the seam changing again. Until that lands, every backend's existing behaviour is
  unchanged, by construction: nothing here is on any include or link path a backend already walks.

## Verified this phase

Built as its own module (`AVER_MODULE_SR=ON`, the default) alongside the rest of the engine; the
headless suites this phase's build produced are listed in the change's own report. **Not** verified
by a GPU capture: `SpatialUpscaler::execute()` has never been run against a live swapchain, because
nothing yet calls it — see "Backend wiring" above.
