# Async shader compilation

Status: built 2026-10-07 (D3D12). Vulkan keeps the synchronous path until its factory is made thread-safe.

## Why
Every Voxi pipeline was compiled on the editor's main thread. With a cold shader cache, which happens after every
new build because the cache key hashes every shader file, that meant:
- project open: the init set plus the scene set, about 46 compiles;
- the first ReSTIR PT switch: 13 heavy compute shaders, compiled inside frame recording;
- the first NRD2 frame: NRD2's passes, its compose draw, and Voxi's NRD2 Stage B.

Each blocked for minutes, and Windows declared the editor not responding (AppHangB1).

## How it works
- **RHI** (`IResourceFactory::threadSafePipelineCreation`, D3D12 only):
  - `createShader`, `createGraphicsPipeline`, `createComputePipeline` and `destroyShader` may run on a worker.
  - DXC: one `IDxcUtils`/`IDxcCompiler3` per thread.
  - Table appends and handle lookups sit under a short mutex. Compiles and `Create*PipelineState` run outside it.
  - `collect()` and `destroyPipeline` stay on the owner thread.
  - `ShaderFiles` is locked, and a reload retires shader texts instead of freeing them.
- **VoxiRenderer:** one build worker. Each job captures a snapshot of the shader prelude when it is queued; its
  completion runs on the main thread at the top of `prePass`.
  - **Exclusive builds** (the init set, the scene set, NRD2): the build writes the handles directly. While it runs,
    `pipelinesBuilding()` makes every render-feature hook draw nothing, so the viewport is black and nothing reads
    the handles. A rebuild asked for meanwhile (new formats, a shader or material-graph change) is honoured when the
    build lands.
  - **Twins** (Path Tracing, NeuRaC; table-driven, `twinSpecs`): built into locals and adopted on the main thread. A
    scene rebuild in the meantime (`sceneGen_`) makes them stale, so they are destroyed. The consumers run their
    plain variants until the twins land, so ReSTIR PT renders as ReSTIR RT for those seconds.
  - `buildAllVariants` (the warm-up and `VoxiShaderCompileTest`) waits for the queue (`finishPipelineBuilds`).
- **Editor:**
  - A "Compiling shaders N of M" notification shows while an exclusive build runs. M is the last build's compile
    count, so the first build of a session just counts.
  - The startup splash no longer waits for a scene that cannot draw yet.
  - The mode-switch "hold until the warm-up finishes" is gone.
  - The `--warm-shaders` process remains as a silent pre-filler of the cache.
- **Also:**
  - A resize, render-scale change or G-buffer toggle no longer rebuilds the scene set when its formats are
    unchanged. It used to be built twice at startup.
  - D3D12 `drawMesh`/`drawFullscreen` skip when no pipeline is bound, instead of drawing with the previous PSO.

## Not done
- **Vulkan:** the same changes, in `VulkanShaderCompiler` and `VulkanResourceFactory`, the latter including the
  descriptor-layout pointer into a vector.
- **Small synchronous builds:**
  - the path tracer's init and `PtSceneView`;
  - the FidelityFX `Denoiser`;
  - AverSR, NeuRAA and NeuraFI (lazy, 1-2 pipelines each);
  - NRD2's network pass;
  - particles, water, editor lines.
- **Rebuilds go black:** a rebuild shows black instead of keeping the previous image. Keeping the old set drawing
  would need a pending copy of every handle.
