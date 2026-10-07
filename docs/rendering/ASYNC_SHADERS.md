# Asynchronous shader and pipeline builds

Opening a project must not block the editor. On D3D12 the interactive editor builds Voxi's pipelines on worker
threads: the viewport is blank with a "Compiling shaders N of M" notification while the scene set builds, and the
scene appears when it lands. `--sync-shaders` restores the fully synchronous path (the same code, waited for where
it is asked). Vulkan, captures (`--frames`, `--headless`, `--play-test`), tests, the runtime and the
`--warm-shaders` process stay synchronous.

## Shape

```
main thread                          worker pool (D3D12PipelineBatch)
-----------                          --------------------------------
record requests into a batch   -->   compile shader  (DXC instance per thread, blob cache; no driver calls)
  (copies of every desc/string)      results into the batch's OWN slots
start()
  ...frames keep running...
frame boundary (prePass top):
  finished()? adopt(): create the PSOs HERE, move them into the factory table, swap the group, reset histories
```

* `rhi::IPipelineBatch` (RHIResources.hpp): `createShader` / `createGraphicsPipeline` / `createComputePipeline`
  record; `start`, `finished`, `progress`, `waitFinished`, `adopt`, `resolve`, `cancel`. Handles in a batch are
  batch-local. `rhi::createPipelineBatch(res, async)` returns the D3D12 pool batch, or an **inline** batch that runs
  every request on the calling thread through the factory's ordinary create calls (Vulkan, `--sync-shaders`).
  Recording code is the same either way.
* Shaders are worker tasks. Pipeline states are created on the owner thread in `adopt()`: with workers inside
  `Create*PipelineState` beside frame submission the RX 7800 XT lost the device (DRED: every command list
  complete), so workers never call the driver.
* `VoxiRenderer` builds five **groups** (VoxiPsoSet.hpp): Base, Scene, Rc (NeuRaC twins), Pt (Path Tracing twins),
  Nrd2. A group is recorded into one batch and replaces its handles together. NRD2's own pipelines go through the
  same batch (`Nrd2::recordBuild` / `finishBuild`).

## What went wrong before, and how this rules it out

The earlier attempt lost the device ~20-30 s after a Project Browser open (RX 7800 XT). The hung pass was never
captured, so each plausible cause is excluded by construction:

1. **Worker vs GPU-object lifetime.** A worker reads only its own immutable request, the device caps, the
   root-signature cache (`std::deque` + mutex, entries never erased, so the raw pointers in `RhiPipeline` live as
   long as the factory) and its batch's shader blobs. It never touches `shaders_`, `pipelines_`, `retired_`, a
   handle table, or `collect()`. A pipeline reaches `pipelines_` only in `adopt()` on the main thread; the deque
   there already keeps pointers stable. Strings are copied when recorded (the shader-file cache is also locked and
   the DXC include handler copies), so a hot reload cannot pull text from under a compile. Old pipelines are freed
   with `destroyPipeline` on the main thread, which retires them behind the frame fence as it always did.
2. **Frames that "draw nothing" but run other passes.** While the scene set is not ready Voxi claims the scene
   (`suppressesScene` and `suppressesWholeFrame`, latched in `prePass`): the device leaves the frame blank, skips
   sky, transparency and frame interpolation, and Voxi's own `prePass`, `scenePass`, `sceneConstants`,
   `scenePipeline` and `depthPrepassPipeline` all stop on one predicate (`canRecord`). No Voxi history is read,
   written or transitioned, so every resource is exactly where the last recorded frame left it. When the set lands
   (`onSceneLanded`) the GI, RT, AO, denoiser, NRD2 and Path Tracing histories are reset, the device is told of a
   scene cut (frame interpolation) and the upscaler is reset. Not reset: auto-exposure, which adapts to the blank
   frames and re-converges in about a second.
3. **Handles change mid-frame.** Adoption and starting are done in one function, `pumpBuilds`, called at the top of
   `prePass` before the frame has recorded anything of Voxi's, or outside a frame (init, synchronous mode,
   `buildAllVariants`). `onRenderTargetsChanged` (which can fire from the UI mid-frame) only records the new
   formats and sets `targetsStale_`, which stops every recorder at once; it never builds or swaps.
4. **Partial sets and the 0 handle.** A group is adopted whole: if the scene generation moved on while it built, the
   whole group is dropped, never a part. A scene rebuild also rebuilds the lazy groups that were live, in the same
   batch, so Scene/NRD2/PT/RC swap together. While the first set builds nothing is bound. And
   `D3D12RenderContext::setPipeline` with a handle that has no pipeline now drops every following draw and
   dispatch (`pipeInvalid_`) instead of letting them run on the previously bound PSO.
5. **Shared compiler state.** DXC instances are per thread (`thread_local`); the compiler's counters are atomic;
   `ShaderFiles` is locked; each batch fixes the shader-corpus hash and file revision when it is recorded and
   skips the blob-cache write if the files were reloaded since, so old text is never filed under the new key.
6. **Project open from the UI pass.** `BrowserAction::Open` -> `applyProject` runs inside a frame and may call
   `setSettings` (which can `waitIdle` and recreate resources). Builds are never started or adopted there: lazy
   requests only set a flag (`requestBuild`), and `pumpBuilds` runs at the next `prePass`. A result is tagged with
   the scene generation (`sceneGen_`); a target, material-graph or shader-text change bumps it, cancels batches not
   carrying the base group, and starts a new Scene build, so a stale result is discarded and the right one follows.

## States

* Initial: `init()` creates resources, starts Base+Scene, returns. `blocked_` until the set lands.
* Target change (resize, sample count): the old set is unusable (formats are baked in), so the frame goes blank
  again until the new set lands.
* Material-graph or shader-file change: the old set keeps drawing; the new one swaps in whole when ready. If its
  scene or debug pipeline would not build (a shader edit with an error), the set that stands is kept.
* Lazy groups (NRD2, Path Tracing, NeuRaC twins): requested by the code that wants them, built in the background;
  until they land the frame uses what it already used when they were absent (FidelityFX, ordinary ray-driven
  passes), and histories are reset when they arrive.
* Core set fails to build: Voxi stands down (`failed_`) and the device draws with its own pipeline.

## Not covered

Other modules' pipelines (particles, water, AverSR, NeuraFI, UI, the FidelityFX denoiser, the Path Tracer view) are
still created synchronously where they were; they are small and mostly cache hits. The first use of NRD2's
`CSNrd2Features` (network inputs) is also still lazy and synchronous.

Tests: `tests/rhi/src/PipelineBatchTest.cpp` (WARP) runs concurrent shader and pipeline creation against the main
thread's own creates, failure isolation, the inline batch, and cancel/drop.
