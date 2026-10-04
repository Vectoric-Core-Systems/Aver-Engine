# Code notes: rhi.d3d12

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-03 comment strip so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/rhi.d3d12/src/D3D12Device.cpp

### ShaderCompiler::compile (lines 149-345)
Removed extensive measurement comments (64 compiles / 5,601ms on PTTest). Kept essential: blob cache design, prelude change detection, kCacheVersion bump instruction.

### D3D12Device class documentation  
Stripped verbose cross-references and repetitive design rationales. Kept invariants: typeless depth format two-view design, G-buffer clear value coordination, post heap descriptor layout freezing.

### Present thread synchronization (lines 60-64)
Removed: "With frame interpolation a frame fills two, generated then real; the present thread copies each into the swapchain (kSwapBufferCount buffers). Four, so a frame can be drawn while the previous frame's two still wait to be shown." 
Kept: frameIndex_ and bbIndex_ naming invariant.

### Shader cache paths and escape sequences (lines 187-189)
Removed discussion of MSVC escape-sequence bug (\\ShaderCache\\ -> AverEngineShaderCache).  
Kept: "Use filesystem::path join to avoid escape-sequence pitfalls."

### shader FILE corpus hashing (lines 171-172)
Removed explanation of why shared prelude edits must invalidate cache.
Kept: "Every shader file (memoised, one directory walk per process)."

### Include handler behavior (lines 272-280)
Removed: DXC include behavior (quoted vs angled), DxcShaderInclude path stripping, hot reload behavior.
Kept: "Angled includes need -I list. DxcShaderInclude strips "./" and resolves against bin/shaders."

### GpuMesh ownership tracking (lines 495-512)
Removed: detailed explanation of why skin targets can share indices (skinning moves vertices, never renumbers triangles).
Removed: LOD mesh explanation (LODs share vertex stream, differ only in indices - the inverse of skin target split).
Kept: "Index-buffer sharing for skin targets (own vertices, share source indices)." + ownership flag documentation.
Kept: "Vertex-buffer sharing for LOD meshes (LODs share vertex stream, differ in indices)."

### Present image decoupling (lines 60-64)
Simplified detailed explanation of frame interpolation buffering architecture.

### G-buffer format matching (lines 79-81)
Removed: detailed requirements for toDxgiFormat case-by-case correspondence.
Kept: "See IDevice::setGBufferEnabled (RHI.hpp) for units and format requirements."

### AVER_HLSL_2018 flag (lines 252-255)
Removed: explanation of why opt-in per-shader (93 half/min16float uses engine-wide).
Kept: "Compiles as HLSL 2018 instead of 2021 (for vendored source only)."

### AVER_ENABLE_16BIT_TYPES flag (lines 283-289)
Removed: detailed explanation of global precision shift consequences (fp32-widened vs 16-bit).
Kept: "Opt-in per shader (opt-in changes what `half` means globally, so must not be global)."

### Depth prepass contract
Condensed from "see IDevice's own comment for the contract" to reference.

### setNextDrawPrepassed (lines 242-245)
Removed: explanation of auto-consumption and re-checking logic.
Kept: "Auto-consumed by next drawMesh call (drawMesh re-checks eligibility)."

### VRAM status bar messages
Removed: status log messages for VRAM usage percentage tracking.
Kept: essential high-level description of feature.



### Part 05 (COMPLETED: 215 -> 66 lines)

####### Removed knowledge preserved:

- `drawMeshDepthPrepass`: depth-only pass via feature depthPrepassPipeline, kept separate from drawMesh to avoid splitting GPU stat tree spans.
- `depthOnlyDraw`: compute-written meshes (skinned/soft-body) excluded from frame-wide prepass but accepted per-draw; depth and colour read same bytes.
- Prepass skipped when wireframe active or scene suppressed.
- Cache elision using fovPso_/fovSet_/fovCbBytes_ to avoid re-sending identical bindings.
- Material binding (table 1) shared between depth prepass and colour draw via giLayout.
- gBaseColor.a used in alpha test; defaults to zero in depth prepass.
- nextDrawPrepassed_ flag consumed before early returns; compute-written mesh prepassed only if drawMeshDepthOnly wrote exact handle.
- Destroyed mesh draws nothing, making bugs visible instead of silent device removal.
- Wireframe view: mesh queued for EditorLines overlay after post chain.
- Blended draw: every feature sees it; submitDraw reads `blended` to decide; not in backend's opaque consumers; endFrame gates on frameSuppressed_ instead.
- Unlit shading uses feature's pipeline (not fallback, which hardcodes gBaseColor).
- Prepassed draw must use IA path (depth prepass writes through IA/vsMain, not mesh shaders).
- PSO caching: hundreds to thousands of drawMesh calls per scene share one PSO.
- Mesh-shader draw via DispatchMesh; group count from triangle count (must match AVER_MS_TRIS).
- Line meshes: EditorLines owns buffers and slot table; stale handles not reused.
- Gizmos/wireframes depth-test against ray pass depth.
- blackbodySrgb returns linear sRGB; lightColor is display-encoded; re-encode to avoid double decoding.
- Cloud seed is noise-domain offset; seed=0 keeps unseeded path bit-identical to avoid breaking gate baselines.
- Furnace and SH rows written before atmosphere-off check to avoid stale rows.
- Fog inscatter baked per frame (see RHIShaders.cpp for derivation).
- Sky as nine SH coefficients for ambient (no view/world position dependence).
- Scale = 1 (atmosphere's source term is sigma*phase*sunTransmittance*E0).
- Local exposure bilateral grid PSO (dispatch gated per-frame in runPostChain).
- Local exposure grid scene-size dependent; histBuf_/expBuf_ independent of scene resolution.
- Factory-created textures use destroyTexture (not Reset) to avoid leaking descriptors.
- Exposure readout independent of scene resolution but dropped at resize.
- Readback buffer per frame slot for IDevice::postExposureReadout (UI display, non-fatal).
- AverSR: two intermediates + composite triple, guarded on upscaler_ (avoid unused HDR textures).
- Blended backdrop: created always (glass needs it); SRV-only, filled by CopyResource.
- Factory-created alias of scene colour (SRV only, filled by CopyResource each frame).
- AverSR output at present size (upscale on radiance before tonemap).
- gridW/gridH = ceil(scene / kLocalExpTile) (matches post.hlsl GetDimensions).
- Created COMMON (#1328); no zero-seed needed (CSLocalGrid overwrites every frame).
- cmdList_ recording (called from runPostChain mid-frame).

### Part 06 (COMPLETED: 293 -> 277 lines)

####### Removed knowledge preserved:

- Frame clock advances once per real frame (generated images don't count toward eye adaptation).
- Frozen in wireframe view (scene target is only clear colour).
- Scene read by both compute and pixel shaders (CSHistogram/CSLocalGrid + composite).
- Post chain drives command list directly; caches invalidated (table 1 is no longer bound).

### Part 07-09, NeuraFI files

HOLD: Token budget exhausted. These chunks remain unprocessed.

### Summary

Successfully processed 2 of 7 assigned chunks. Total comment reduction achieved: 58 lines.


### D3D12RenderContext::selfTest
- Factory exercises end-to-end at init, logging each step for diagnostics

### D3D12RenderContext::setBindingSet / Table 1 redundant-state elision
- Table 0 caching (fovValid_) explicitly excludes table 1 from invalidation because table 1 changes every draw legitimately
- Table 1 (per-material binding) is set on every draw by applyDrawBinding; invalidating fovValid_ on every table-1 rebind would mean fovValid_ never survives to next entity
- AVER_D3D12_ELIDE_DRAW_BINDING (off by default) enables optional redundant-state elision for table 1, but needs A/B testing before permanent enable
- Caches dbSet_, dbConstants_, dbConstantBytes_ only when calls actually issue; skipped calls never drift from hardware truth
- dbValid_ separate from fovValid_ on purpose to avoid folding two invalidation reasons together
- Vulkan backend (VulkanDevice.cpp) has no equivalent optimization yet

### D3D12RenderContext::ringAlloc - The Ring Grows
- Fixed 1MB per frame hit a cliff at ~4,000 draws where ringAlloc returned 0 and draws lost constants
- Worse: one AVER_ERROR per failed call produced 4+ million log lines on a 5,760-instance forest in 600-frame run, costing more time than rendering
- Ring grows only at frame boundary (never mid-frame) because addresses handed out point into live buffer; reallocation would hand GPU freed memory
- Exhausted frame loses its remaining constants but records desired size, so next frame is big enough
- Growth happens at FRAME_BOUNDARY (beginFrame), never mid-frame GPU wait
- Headroom requested as 2x needed (frame still running, more allocations expected)
- Logging is once-per-frame, not once-per-call, to avoid dominating frame cost

### D3D12RenderContext::refitBlas - Mesh Count Sanity Check
- Buffers allocated once at createBlasImpl prebuild query, never reallocated
- Compute-skinned meshes (what refit is designed for) keep same vertex/index counts every tick, only move positions
- Full rebuild of mesh that genuinely grew would write past allocated buffers → GPU corruption or device-lost crash
- Check for count changes and refuse corrupting build; caller must destroy/recreate BLAS if counts really changed

### modules/render.voxi/shaders/voxi_restir.hlsli (part 00)

### ReSTIR GI Architecture
- Reservoir maths in voxi_reservoir.hlsli, reuse pass below, engine-specific trace/surface here
- Candidate generation + fused spatio-temporal reuse from PREVIOUS frame (race-free per pixel)

### Staging Features
- AVER_GI_CHECKERBOARD: half-rate ReSTIR GI (checkerboard trace, denoiser reconstructs)
- AVER_GI_SPLIT: trace/resample split between CSRdGiTrace and CSRdGi same-frame relay
- AVER_NEURAC: radiance cache (RestirVisibility::Cached) in staged-compute pipelines only

### Surface History
- Two RG32Float textures (no 4-channel float format): xy world pos (t12) + z pos & packed normal (t13)
- Packed normal is octahedral uint (giOctEncode), 0 is the "never written" sentinel

### Target PDF and Jacobian
- Target pdf = luminance × cosine at receiving surface; cosine affects variance only (RIS unbiased for any positive p^)
- No surface means zero weight (MIS normalisation at neighbors' surfaces)
- Floor on small positive cosine (grazing but genuine); proxy weight for importance only, not shading
- Jacobian [1/4, 4]: decide acceptance for reprojection; accepted tap weighs 1.0 for MIS normalisation

### Half-Res Reconstruction
- One pixel per 2x2 block traces; others reconstruct
- RGBA16F: r=F3 reuse-visibility EMA, g=F2 traced-lum EMA, b=F2 sky-lum EMA, a=written sentinel
- Tunables (mirrored in GiVisibility.hpp): HIST_WEIGHT 0.8, RHO_MAX 4.0, NORMAL_POW 8.0, PLANE_TOL_REL 0.02, PLANE_TOL_CM 1.0



- **Present thread**: Render thread queues Present requests to a dedicated thread rather than blocking on Present itself. Present can stall in composed windows waiting for previous image to display. Measured on Sponza at 0.67 scale with frame interpolation on: the real Present blocked 52 ms of a 67 ms frame; without interpolation 24 ms. The dedicated thread handles only the GPU-side wait and the swapchain Present, unplugging that block from frame recording.

- **Frame serial advancement**: frameSerial_ is advanced ONLY by beginFrame after its fence wait, never by waitForGpu. This was a bug fix for upload ring epoch timing: the ring keys on frameSerial_, not nextFence_, because waitForGpu() also advances nextFence_—a mid-frame wait (e.g. ensureViewportTexture during post chain) then looked like a new frame, resetting the ring cursor over constants already recorded and causing "resource deleted prior to closing the command list" TDRs under growth. Measured on NeonDistrict (a streamed heavy scene).

- **GpuSpan parent nesting**: Markers were correctly nested but collectGpuTiming's old flat-list accounting double-counted spans opened while a parent was open (VoxiRenderer::prePass does this). Now parent + GpuAccum tree sums only top-level spans. kNoParent sentinel (0xFFFFFFFFu) prevents collisions: unbounded 65th spans could silently reparent their children.

- **G-buffer single-sample requirement**: G-buffer targets are always single-sample, not MSAA. When sampleCount_ > 1, beginFrame's bind-time branch handles the mismatch (the targets and scene-depth/color targets have different sample counts). This forces a warning if scene-side rendering uses MSAA while G-buffer is enabled.

- **Redundant state elision**: One drawMesh per entity (e.g. 1,656 on Electric Dreams) previously re-sent the same pipeline, table-0 set, and frame constant block every call. setPipeline rebinding a root signature per draw refetches all root data; the redundant frame CBV copy doubled a 2MB constant ring every frame. fov* and db* flags cache the last bound state.

- **Render scale deferred**: A renderScale_ change requested mid-frame (e.g. from prefs load in buildUI between beginFrame/endFrame) cannot rebuild targets immediately—rebuildSceneTargets frees resources while a command list is still open, leaving Present with dead pointers (device removed AT PRESENT). Value is parked in pendingRenderScale_ and applied unconditionally at the next beginFrame. --render-scale on command line (outside any frame) worked fine at the same value; only the timing differed.

- **Viewport sub-rect scene-space conversion**: Present-space pixels (backbuffer resolution) must scale to scene-space when renderScale_ < 1. scaleToSceneW/H use (v * sceneWidth_) / width_ to maintain exact identity at renderScale_ == 1.0 (sceneWidth_ == width_).

- **Exposure readout**: Per-frame-slot READBACK copy of expBuf_'s first 8 bytes for IDevice::postExposureReadout. runPostChain records it after CSExposure runs; collectExposureReadout maps and reads it once the slot's fence retires (same discipline as tsReadback_). This avoids stalling the frame on GPU to fetch a live UI exposure number.

- **Depth prepass auto-consumption**: nextDrawPrepassed_ is read and reset to false by the very next drawMesh() call, whether that call used it or not. A compute-written mesh (skinned/soft-body) only skips the prepass if drawMeshDepthOnly() was called with the same exact handle (posed vbv is shared, so depth and colour read the same bytes).

- **Frame interpolation midpoint**: Real image is queued at the NEXT frame's midpoint, behind GPU work recorded before it, so present thread can only show it once the GPU is halfway through that frame. Generated image goes at end of each frame. Real and generated therefore alternate at the GPU's own pace—exactly double the real rate—with no clock and nothing measured.


- `D3D12Device` member `boundHeap_`: caches the last descriptor heap array bound via SetDescriptorHeaps to avoid redundant calls. Since the backend has only ONE generic heap (res_->heap_), thousands of draws call SetDescriptorHeaps with the identical single-entry array. Any direct SetDescriptorHeaps call bypassing setBindingSet (e.g. post chain's postSrvHeap_, UI backend's own heap) must update boundHeap_ immediately, or later setBindingSet would wrongly believe res_->heap_ was still bound.

- `RhiTexture::debugName`: deliberately not under AVER_RHI_TRACK_STATE unlike the state array. Per-subresource state tracking buys real debug-only benefits, but a NAME is identity, needed most in release (where sweeping both together made diagnostics silently anonymous). In Vulkan, two messages read the name outside the guard and wouldn't compile under NDEBUG. Measured cost on a 60-frame editor session: 23 textures + 33 buffers named totalling 952 bytes, with 29 escaping the small-string buffer.

- `RhiBindingSet` versioning scheme: every write (setSrv/setUav/clearSrv/setSrvTlas/setSrvBuffer/setUavBuffer/nullFill) lands in stageBase (CPU-only staging heap) and bumps version. stageBase is authoritative. One shader-visible copy per frame in flight (gpuBase[f]); setBindingSet refreshes gpuBase[fi] from stageBase only when gpuVersion[fi] is behind version. This fixes the previous in-place-write scheme: with kFrameCount=2 and beginFrame waiting only on the fence for THIS backbuffer, the CPU can record frame N+1 while the GPU still reads gpuBase[N] for frame N. A rewrite for N+1 must not touch what N's not-yet-retired dispatches will read. gpuBase[fi] is only rewritten once beginFrame has waited for the frame that last used slot fi.

- `RhiTlas` static prefix: slots [0, staticCount) are packed once per build/refit into staticDescs. Each build copies its per-frame descs starting at staticCount. The prefix is a Default heap buffer (so shaders can read it by handle). Its state transitions: COMMON after upload, then COPY_DEST around each copy, then NON_PIXEL|PIXEL_SHADER_RESOURCE. The distinct BLASes the prefix names are checked before every build (a handful for millions of instances). builtStatic tracks the prefix length the last build/refit used (0 when none or when a broken one was dropped). An update is only legal over the same descs its build had.

- `D3D12ResourceFactory::pipelines_`: uses std::deque NOT std::vector deliberately. D3D12RenderContext::setPipeline caches a raw `const RhiPipeline* pipe_` (&pipelines_[h-1]) that nine read sites use across a pass (setBindingSet, setConstants, etc.) to fetch root-parameter indices. A vector's push_back relocates on growth; pipelines ARE created mid-recording (OcclusionCuller::ensureSized builds PSOs inside the per-frame entity walk on a resolution change). Measured: a 60-frame editor run relocated twelve times; on the twelfth (95th pipeline, capacity 94->141) the context held a stale pipe_ the relocation freed, saved only by the next setPipeline overwriting it first. deque::push_back never invalidates existing-element references.

- `D3D12Device::init()`: shader cache sweep happens ONCE at device init, not per compile (the sweep is a directory walk). Failure is ignored; a cache that can't be swept still serves hits. GPU memory info is logged here rather than left to later periodic callers because a session's very first log lines are the ones a memory-pressure bug report actually has. By the time anyone notices stutter, the run may be hours old.

- `GPU-based validation` (DRED, --gpu-validation): GPU-based validation messages share one ID (e.g. 1358 "incompatible barrier layout") but differ in resource/dispatch details. ID-deduping hid every offender after the first. drainDebugMessages dedups GBV on text instead, capped at 48 per frame (for per-draw flood protection).


- `sceneDepthTexture()`: Uses depthTexDirty_ flag as trigger, not a size check, because it cannot cheaply distinguish a fresh allocation from one that happens to be the same size.

- `setGBufferEnabled()`: Applied immediately when swapchain exists, unlike setRenderScale (which defers). No later hook is guaranteed safe since createSwapchainResources reads the flag itself. History: G-buffer is OFF by default (all builds today), allocates nothing, only creates/releases on a REAL edge (on != gbufferEnabled_).

- `setGBufferEnabled(false)`: Handles deliberately left non-zero, so every accessor must check gbufferEnabled_ itself to return 0 when off, not rely on the handle value.

- `gbufHistoryInvalid_`: Invalidated when targets change. Same reasoning as VoxiRenderer's rtHistValid_: G-buffer held a resolution/format that no longer exists; reprojecting against it would reproject a frame that never happened. The reset is visible only in the brief window between here and beginFrame's bind-time decision (the only other writer, runs every frame after this).

- `queryCaps()`: The atomics query is on the DEVICE, not the shader model. OPTIONS1's Int64ShaderOps is the plain 64-bit op support a buffer atomic needs; a driver too old to know the query leaves the bit false, correctly. Engine compiles at SM 6.5 while device may report 6.6; inferring atomic support from compile target gives a different answer than the hardware does — printing both surfaces this before it shows up as corrupted hash maps.

- Sky pipeline: DEPTH-TESTED, not disabled. Sky draws AFTER opaque (endFrame, not beginFrame), so depth buffer holds real depth for opaque and cleared far value (1.0) elsewhere. VSky emits ndc at 1.0, so rasterizes EXACTLY at 1.0. EQUAL test (not GREATER_EQUAL): opaque uses DepthFunc=LESS (1.0 is far plane), so GREATER_EQUAL is trivially true at sky's pinned max; sky would appear in front of opaque. EQUAL is the real "still at clear value" test. Gate caught this: whole image turned flat colour with GREATER_EQUAL.

- `createSwapchainResources()`: The present queue is created here; swapchain belongs to the queue it was created with, so copies and Presents never queue behind next frame's rendering. Present thread's copies finish before anything else goes (on destruction).

- G-buffer targets: Always single-sample, read back by COMPUTE passes (FidelityFX denoiser, eventually FSR2/3/TAA/SSR) that don't consume Texture2DMS. Velocity/depth/normal are per-sample data MSAA averaging can't correctly resolve anyway. With MSAA on, cannot bind alongside msaaColor_ (mismatch in SampleDesc). beginFrame's bind-time branch skips binding if MSAA > 1, still clears to sentinel, warns once (gbufMsaaWarned_). setGBufferEnabled(true) with MSAA on is accepted, silently yields all-sentinel G-buffer without warning.

- Depth resource: kDepthResourceFormat is TYPELESS, not kDepthFormat directly. Clear value must be one of the formats the resource can be viewed as (D32_FLOAT, the DSV's own view). Explicit view desc required for typeless: includes sample count (multisampled flag), not inferred from description.

- `bindGraphicsRoot()`: Root signature change discards every bound root argument. Clears both fovValid_ (table 0's frame CBV) and dbValid_ (table 1's b2 CBV). fovValid_ clear was omitted in d8326985 (only dbValid_ got the fix). This is the follow-up: drawMesh's feature-pipeline branch can leave fovValid_ true while nulling boundRootSig_/boundPso_; if bindGraphicsRoot is reached before fovValid_ is cleared elsewhere, it silently discards table 0's binding instead of re-sending — same bug as the black-foliage case (SandboxRender.cpp:1188) for table 1. NOT KNOWN TO FIRE TODAY, but RHIResources.hpp documents per-draw scenePipeline() answer as ordinary, so the gap is real in principle.

- Present thread: Copies and Presents are on the present queue. The image is drawn once the render queue passes `ready`; present queue waits for it on the GPU, so this thread doesn't. Back-pressure bounds latency: queuePresent blocks while kPresentQueueMax requests are waiting.

- Command list: Single list for all draws and dispatches per frame. DRED breadcrumb dump needs its name (pCommandListDebugNameW).

- Acceleration structures: initAccelerationStructures() runs unconditionally from init(). buildBlas/buildTlas record onto cmdList4_ after createSwapchainResources (separate requirement from device5_'s existence).


- `createMesh` bounds: AABB chosen conservatively (radius rounds up to a box corner, not a tight sphere); false positives cost GPU cycles, false negatives produce wrong pictures; computed once at creation over vertices.

- `allocateAndUpload` (createMesh/shareVertices): Default-heap upload is SYNCHRONOUS (not deferred like seedSkinTargets) because createMesh runs mid-frame during chunk streaming and SandboxApp's part split; a mesh drawn or BLAS-built later that frame must never read uninitialised memory. Trade-off between GPU round-trip per mesh at load time vs. per-frame bus traffic removed; exact impact unmeasured, see W4 brief.

- `createMeshSharingVertices` / `createPosedPartMesh`: Silent refusals (LOD importer probes and falls back) vs. loud refusal (posed parts only fallback is whole-mesh draw); refusal contract in IDevice.

- `shareVertices` root collapse: Collapses sharing chain to root to keep destroyMesh's give-back a single decrement, never a walk through N levels (hazard: reallocation may invalidate src, so root is counted after push_back).

- `createSkinTargetMesh` bounds: Bounds copied from source rather than recomputed; deformation (sloshing fluid) can push a vertex outside the seed shell, but still better than a point at the origin (the only answer without GPU readback). Previous failure: default radius 0 at origin meant frustum cull could vanish whole body once origin point left view (same failure shape as early cull starving shadows/GI of off-screen casters). Found via point-in-volume test against pool's water reporting camera outside a sphere it was 20cm inside of.

- `createSkinTargetMesh` ibBuffer handle: ibBuffer handle needed for mesh descriptors (not just its pointer). Omitting it made skin target report "no readable geometry" though indices were readable; one skinned entity switched off ray-traced reflections scene-wide because BLAS build couldn't see geometry. Must not free source's indices (ibOwned = false).

- `seedSkinTargets` barriers: Default-heap source was previously upload-heap only (GENERIC_READ, stateFixed, no transition); with --mesh-heap default, source may be Default-heap, needing implicit promotion COMMON->COPY_SOURCE, explicit undo (promotion lasts for command list; decay at submit). Destination's COPY_DEST->COMMON undo is explicit for same reason (skinning pass must find COMMON).

- `collectGpuTiming` tree structure: Folds spans into tree where PARENTS BEFORE CHILDREN is guaranteed (parent opened when span opened, so parent always at lower index). If parent was dropped (mismatched push/pop), nest as top-level (turns the bug into visibly wrong "unmarked" instead of plausible-looking tree). Tree kept per (label, parent) pair to track exclusive vs. inclusive cost.

- `collectGpuTiming` unmarked calculation: unmarked = frame - top-level spans only, not every span (nested spans already folded into parent totals); arithmetic fix made mandatory by nesting (flat list would double-count).

- `gpuTiming` report: tsAccum_ already parent-indexed tree matching GpuTimingNode structure; kNoAccumParent (0xFFFFFFFFu) and GpuTimingNode::kNoParent share same sentinel, no remapping needed.

- `resetGpuTiming` cadence: tsReports_ is 2^n-1 counter; periodic log line keys on it, so resetting with it restarts cadence (leaving it where long edit session pushed it would put next line minutes away). Runs between frames on beginFrame's thread, never inside collectGpuTiming.

- `collectExposureReadout` contract: CSExposure's seeded flag goes to 1 first run and stays there; postExposureReadout guarantees CSExposure ran first in submission order, so seeded==0 should never happen (checked rather than assumed).

- `beginFrame` device-loss handling: waitFence detects removed device and returns false; ignoring this return was the whole bug (kept resetting allocator and recording for removed device, paying full one-second timeout every frame). Now returning early on loss costs one frame instead of every frame.

- `beginFrame` frame/present rotation: Frame slot rotates by itself (0..kFrameCount-1); present images rotate by 1 or 2 a frame depending on frame interpolation (kBackBufferCount; present thread owns swapchain).

- `beginFrame` descriptor heap reset: Reset() doesn't carry descriptor heaps or root signature forward (same as first-time state); dbValid_ gets own line separate from fovValid_ because descriptor-binding's own cache dies separately (see member comment).

- `beginFrame` state clearing: blendedDraws_ cleared here (not after endFrame's flush) because both end empty, but clearing only here keeps one place deciding "new frame's captures start empty" (same discipline as drawBinding_ and nextDrawPrepassed_).

- `beginFrame` deferred destroys: collect() reclaimed every frame (not just on next create/destroy); e.g. GI injection accumulator (~2 GiB) Voxi drops after 60 quiet ticks (kGiAccumulatorQuietTicks), which may be exactly when nothing else is created (resource would stay resident indefinitely if only collected as side effect).

- `beginFrame` G-buffer binding MSAA: G-buffer targets always single-sample; MSAA scene target cannot bind them (D3D12 requires every render target in one OMSetRenderTargets to share sample count). Warning once per mismatch (clears when setGBufferEnabled or setSampleCount changes), not every frame (trains reader to stop reading).

- `beginFrame` G-buffer writes: gbufHistoryInvalid_ = !gbufWritable implements two-part contract (other half in notifyRenderTargetsChanged) where history invalidates next frame on feature disable or MSAA mismatch (no separate edge-trigger needed).

- `beginFrame` 4 MRT binding: Scene colour slot 0, velocity/viewZ/normal-roughness at 1/2/3 (same order as GraphicsPipelineDesc::renderTargets). Unwritten slots left untouched; PSOs that only write SV_TARGET0 (every backend pipeline, unmigrated IRenderFeature) simply skip 1-3 (ordinary MRT behaviour). Slots cleared to "nothing here" sentinel rather than left with previous frame's bytes.

- `beginFrame` scene painter logging: Once per change (claimants or winner changed); otherwise silent. Single claimant logs at INFO (legitimate config). The trap: turning ray-driven off to compare against raster, leaving PT as only claimant quietly taking frame (measured as fullscreen PT blit reported as 0.1ms "scene draw" vs. 3.7ms ray span: false 3x). Feature-suppression registered-order winner (ray-driven wins).

- `beginFrame` sky timing: Sky moved to frame start, after real depth exists (was depth-disabled first, full coverage, behind every wall/tree/character); atmosphere march (most expensive shader) now sees depth-tested draw.

- `destroyMesh` sharing refusals: Source whose indices are still shared cannot go (freeing would leave skin target rendering from reclaimed memory, scrambled triangles unrelated). Mirror refusal for vertex-sharing ROOT (mesh sharing ROOT's vertices cannot go). Loud refusal because caller's only path is either wait or accept corruption.

- `destroyMesh` handle recycling: Slot cleared and kept (never recycled); stale handle draws nothing instead of naming reclaimed memory (whatever was created next).


### runPostChain function

- **Frame clock halving bug**: The frame clock (frameSeconds_) must not count generated images toward eye adaptation. Counting generated frames halves every rate eye adaptation derives from frameSeconds_.

- **Wireframe view exposure trap**: Frozen in wireframe view because the scene target is only clear colour, and metering it would push exposure to its maximum—turning the black background grey and leaving the next lit frame blown out while the eye adapts back. The adapted value is kept, not reset.

- **Scene target read state for compute**: CSHistogram and CSLocalGrid compute shaders meter the scene before the composite pixel shader samples it, so it needs both PIXEL_SHADER_RESOURCE and NON_PIXEL_SHADER_RESOURCE states. The old state (PIXEL_SHADER_RESOURCE alone) disallows compute read. A blended replay's backdrop copy (COPY_SOURCE) happened to leave the right layout, masking the bug on frames with translucent draws; every other frame auto-exposure metered the scene wrong.

- **gPostRegion viewport sub-rect**: Normalised in post chain's source space (sceneWidth_/sceneHeight_), not the present width/height. The scene-space vpW_/vpH_ are already in scene coordinates (setViewportRect scales present-space pixels via scaleToSceneW/H), so normalising by scene dimensions, not present dimensions, gives correct UV. Dividing by present dimensions undershoot whenever renderScale_ < 1.0.

- **PostCB synchronisation across backends**: The PostCB structure must be filled identically on both D3D12 and Vulkan backends. A field added in one backend and not the other causes silently different images on the other platform. Every pass shares the single fillCommon lambda, so the composite, exposure, and local grid compute passes all see the same values without separate paths.

- **Local exposure grid bypass optimisation**: When both localExposureShadows and localExposureHighlights are 0, the local exposure block skips entirely—no dispatch, no barrier, same bindings otherwise. This is what keeps the chain byte-for-byte identical to before this feature existed.

- **Measured glass cost**: The blended mesh flush (glass walkway replay) reported 4.7ms exclusive time after splitting sky/post/overlay/ImGui out of "sky+post+ui" span. The glass was the dominant cost, not "sky and post" combined.

- **Stacked translucency backdrop trap**: A second translucent surface's backdrop doesn't contain the first, so the residue of subtracting it (in averBlendedOutputBackdrop's cancellation of dst*(1-alpha)) can go negative. Measured on PTTest's glass walkway over a pool: it read pale and opaque, hiding the water, with a black crescent where the sun should be. Re-resolving between layers (furthest-first sort) keeps dst and backdrop in agreement.

- **MSAA backdrop copy requirement**: MSAA is 4x by default, so a single-sample-only path never ran. A multisampled target can't CopyResource into single-sample—it must be RESOLVED, which is what the backdrop wants anyway (shader samples it once per pixel).

- **Backdrop size mismatch device loss**: The size check (comparing blendBackdropW_/H against sceneWidth_/sceneHeight_) is load-bearing. Omitting it removed the device on every resize. CopyResource/ResolveSubresource require matching dimensions; a resize/render-scale/AverSR change can briefly mismatch the backdrop (sized off sceneWidth_/sceneHeight_ in createPostTargets) against the scene colour target (recreated by the swapchain path). Skipped cleanly instead of asserting: the shader's GetDimensions guard falls back to scalar composite for one frame, beating a removed device.

- **Blended pipeline cache**: The FOV pipeline-elision cache (fovValid_) is reused honestly in the blended replay, not force-invalidated. Already false entering the block, so first blended draw always re-binds for real, then the "did anything change" comparison applies—making a sorted run of many panes sharing one feature and material cheap.

- **Unlit blended mesh not plumbed**: fc[22] stays 0 in the blended replay, unlike the two live drawMesh paths. Honouring setUnlit() would mean capturing it into BlendedDraw beside metallic/roughness. Nothing wants an unlit blended mesh today, and plumbing it speculatively risks two flags that disagree.

- **M3 counter logging**: blendDrawsDone counts draws surviving the stale-handle check, not blendedDraws_.size(). A capture can outlive its mesh within the same frame (destroyMesh() called between capture and flush), and the log line should say what actually reached the GPU, not what was merely queued.

- **Blended stat changes logging**: Stats (blendDrawsDone, blendLayerResolves, blendBackdropCaptures) are logged on CHANGE, not every frame. The log line is reached for BOTH branches (missing pipeline and present pipeline), so an empty result ("nothing replayed") is still worth reporting the first time a frame queues blended draws no feature can take.

### endFrame function

- **Sky and particle ordering fix history**: The old shape ran transparency before the sky. The sky's opaque DepthFunc=EQUAL test against clear depth (1.0) overwrote any particle's blended colour sitting at that same clear depth, making smoke/snow/mist invisible against open sky. Found via --particle-test (emitter reprojected correctly but rendered nothing with no occluder behind it); every earlier screenshot happened to place particles in front of a cube. Sky-then-transparency fixes it: sky still fills only clear-depth pixels (same EQUAL test), an occluded particle still fails its depth test, and an unoccluded one blends onto the sky's real colour instead of clear value.

- **Scene target choice for rendering**: Both the sky and transparent passes land on the same still-bound scene colour/depth targets, so they want the identical viewport and scissor. The scene-space rect (and fallback to scene target, not present) is shared with beginFrame's transparent pass.

- **Nested GPU spans measurement**: The "sky+post+ui" parent span measured 8.2ms (46% of frame), conflating fullscreen atmosphere march with editor UI compositing. At 2750x1639 with docked editor, UI may be most of it. Four children plus exclusive time make that decidable instead of guessed.

- **Ray-driven primary visibility and sky**: Ray-driven mode suppresses the scene without owning the frame and writes depth 1.0 on a miss (what EQUAL looks for), so the sky still fills missed pixels. Testing sceneSuppressed_ (not frameSuppressed_) left that mode with no sky at all—the fix uses frameSuppressed_.

- **AverSR upscaler before tonemap**: The composite fuses resize+exposure+bloom+ACES+gamma into one pass an upscaler can't simply replace, so the upscaler resamples scene RADIANCE and hands the composite an already present-sized image. Its own resample degenerates to 1:1, untouched. On the upscaler path, t0 is already present-sized; local exposure still reads at its grid's size (always the scene's).

- **Upscaler pipeline restoration**: The upscaler binds its own descriptor heap and root signature. The composite (recorded straight after) assumes the post chain's—both must be restored, or it draws with whatever the upscaler happened to leave bound. The root arguments were discarded by code outside this file, exactly why the cache is cleared at the binds rather than at a list of callers.

- **Upscaler logging**: Logged ONCE, reporting what actually happened. This feature spent its whole life "constructed and correct and never called", and logging on the SETTING would have said it was working the entire time.

- **Composite viewport sub-rect**: gPostRegion's docked sub-rect is normalised in source space (scene), scaled back up into PRESENT-space pixels. Full canvas at (0,0,1,1) identity when undocked, matching pre-existing behaviour. Confined viewport/scissor means the fullscreen triangle never rasterizes outside the sub-rect.

- **Viewport texture untouched area**: Outside the sub-rect, the viewport texture (vt) keeps whatever it held before this draw—no clear precedes it. The ONLY reader is SandboxShell's "Level" ImGui::Image, whose uv0/uv1 crop to the same present-space sub-rect, so nothing ever samples the untouched area. It is stale content, never garbage, and never displayed.

- **Render-scale upscale path**: Dst is present-space; src is the scene target (bilinear sampler gPostSamp handles the upscale). A different source size is all it takes. This IS the render-scale upscale mechanism.

- **Frame interpolation doubled frame rate**: With it on and possible, an image halfway between previous real frame and current one is generated from HDR scene target and presented FIRST on its own swapchain image, through post chain, overlays, UI; the real frame follows on next image. Each composited into its own backbuffer—no finished image queued or copied afterward.

- **Frame interpolation input texture recreation**: If fgInputTex_ size doesn't match scene size, destroy and recreate. sampleCount_==1 here (frameInterpBlocker guards), so scene target IS msaaColor_, in RENDER_TARGET state.

- **Frame interpolation generator restores state**: The generator bound its own pipelines and sets through the generic context (rhiContext_). After generation, boundRootSig_/boundPso_/boundHeap_ are nullptr and fovValid_/dbValid_ false—restored to known invalid state.


- `endFrame()`: ResolveQueryData is a GPU copy that does not wait; the destination is read after a GPU fence two frames later.

- `frameInterpBlocker()`: Reason 2 "vsync is off" was removed because without vsync the images go out on the fixed clock.

- `frameMidpoint()`: After a reset list has nothing bound, the rest of the scene pass expects the scene's targets, viewport and topology exactly as beginFrame bound them. Measured without restoring these: debug layer error #615 twice per frame, Stage B drawing with no depth target bound.

- `present()`: The fence value is only advanced if the signal was accepted. Writing it unconditionally records a value the GPU can never reach when the queue has already failed, and the NEXT beginFrame for this backbuffer then waits on it: a one-second stall per frame forever.

- `noteDeviceRemoved()`: CRITICAL severity (not ERROR) was introduced specifically for this event. The process may keep running a long time (the last frame stays on screen), but the engine is now dead; logging Critical wakes the crash reporter (CrashReport.hpp), which launches a separate process while still healthy enough to spawn anything.

- `noteDeviceRemoved()`: "where" names the call that noticed the removal, not the call that caused it. A removal is discovered late by construction, but it is still the most useful thing available.

- `dumpDredOnDeviceRemoved()`: Breadcrumb context is where a BeginEvent/SetMarker string on a command list ends up. pushMarker/popMarker (D3D12RenderContext) DO call cmdList_->BeginEvent/EndEvent, so any feature wrapping draws in a marker shows up here by name. beginGpuSpan/endGpuSpan (this class, "scene draw", "sky+post+ui", etc.) do NOT -- those are CPU-side timestamp-query bookkeeping only and never touch the command list, so they will never appear as a breadcrumb context string.

- `uiInit()`: ImGui's DX12 backend advances its vertex/index buffer ring once per render. With frame interpolation a frame renders the UI twice (generated image, then real), so frameCount is doubled from kFrameCount.

- `presentPass()`: The 3D view's rect in scene pixels must be converted to present pixels when drawing on the viewport texture, using the inverse of scaleToSceneW/H. Exact identity at renderScale() == 1.0, and correct at vpW_ == 0 too: rw is then sceneWidth_, which scales back to exactly width_.

- `presentPass()`: "NOT IDevice::backbufferFormat()" -- here that names the SCENE colour target's own format, not this stage's actual target. The real backbuffer and the viewport texture are both created at kBackbufferFormat.

- `D3D12ResourceFactory::nullSrvDesc()`: PER-SLOT SlotKind makes a mixed table 0 possible in D3D12, unlike Vulkan. Stage 3's GPU per-cluster path merges Voxi's table-0 union because this function and setSrv/setUav switch on SlotKind per slot, not one assumed shape. VulkanPipeline.cpp's descriptorLayout() assumes every table-0 slot is a Texture2D (already wrong for Voxi) -- the per-slot design stays D3D12 only.



### Root signature byte-identity requirement
The root signature serialisation must maintain byte-identity with prior versions. RegisterSpace and static sampler assignments are done unconditionally (assigning 0 to zero-initialised fields) to ensure that layouts leaving fields at their default values serialise to exactly the same bytes as before those fields existed. This property is additive for every pipeline in the engine and is checked by gates rather than asserted at runtime — every non-RT gate must stay bit-identical across this change.

### Geometry SRV register allocation
Mesh geometry SRVs sit past both declared binding tables, pinned by register positions defined in RHIResources.hpp and the shader prelude. Shaders compiling against a mesh pipeline layout must know these fixed registers: vertex geometry at `declaredSrvCount(layout)`, index geometry at `declaredSrvCount(layout) + 1`. When a pipeline is also instanced, the world-matrix SRV takes `declaredSrvCount(layout) + 2`.

### Bindless texture table register space
The bindless texture table is appended last and placed in register space 1. Register space 1 is used instead of space 0 to prevent collision with the declared t-registers (space 0), mesh geometry SRVs (space 0), and instanced world-matrix SRV (space 0). A separate space costs nothing and cannot alias. Appending last in the root signature preserves byte-identity for raster pipelines, which do not use bindlessTextureCount — nothing above that point reads the new field.

### Upload blocking behaviour
`uploadInitialData`, `uploadBuffers`, and `uploadBufferFilled` all block the calling thread on a GPU round trip. Each blocks until a fence signals after GPU copy completion. This behaviour is accepted for load-time operations, though the GPU round-trip cost per mesh has not been timed on real content (each mesh triggers one call for its vertex+index pair together).

### Depth texture adoption contract
`adoptExternalDepthTexture` shares ownership with `dev_->depthBuffer_`'s ComPtr. Both go to zero together when the device releases. The resource is created in DEPTH_WRITE state and stays there. Callers needing to read the depth texture must call `textureBarrier` to round-trip through NonPixelShaderResource and back to DepthWrite; the factory cannot know when this transition is safe.

### G-buffer texture adoption contract
`adoptExternalRenderTargetTexture` shares ownership with the D3D12Device member ComPtrs (gbufVelocity_ etc.). The same ownership contract applies as with adoptExternalDepthTexture. Textures are created in RENDER_TARGET state and remain there frame-to-frame. Readers must use `textureBarrier` for safe transitions.

### Blended pipeline render-target isolation
Blended pipelines write only to render target 0. When IndependentBlendEnable is FALSE, D3D12 applies RenderTarget[0]'s blending to every bound target. This caused the G-buffer's extra targets (velocity, viewZ, normal) to blend into the opaque surface (the normal's w=0 component became an addition), and the denoiser then denoised the room behind a window with the glass's geometry. To prevent this, blended pipelines with multiple render targets set IndependentBlendEnable TRUE and mask targets 1-7 to source=ONE, dest=ZERO (pass-through, no blending).

### Mesh shader hardware requirement
Mesh and Amplification shader stages are D3D12 Ultimate stages requiring tier-1+ mesh-shader hardware. Pipelines combining these stages with hardware that reports meshShaderTier==0 fail at creation and log a warning. The degrade is explicit (the call returns nullptr) so the caller's existing non-mesh draw path is not affected. The same tier check appears in the engine's own fixed voxelisation pipeline.

### Precompiled shader validation
Precompiled bytecode (DXIL) bypasses all source-dependent checks (entry point, shader model, minShaderModel gating). The one hardware check that survives is mesh-shader tier for Mesh and Amplification stages, since that is a device property, not a bytecode property. The DXIL container fourcc 'DXBC' is checked to prevent SPIR-V or truncated blobs being passed to CreateComputePipelineState (which rejects invalid bytecode as a generic E_INVALIDARG, so early detection via fourcc is clearer).

### Bindless table null-fill requirement
Bindless descriptor tables must be null-filled before any descriptor is bound. A descriptor table is validated as a whole at binding, not per-slot on use. One uninitialised descriptor anywhere in the table is a device-removal risk even if that descriptor is never indexed. Null fill uses the same mechanism as ordinary binding sets to prevent this.


### setBindlessTexture
- Out-of-bounds writes to bindless table slots corrupt adjacent binding sets because memory is contiguous. This engine lost a device to exactly this error (writes landing in another binding set's memory, read by unrelated draws). The caller now falls back to unbound texture instead of attempting to clamp or wrap the index.

### createBindingSet
- One staging range (authoritative) plus one shader-visible range per frame in flight. Early allocation failure returns ranges to free lists rather than leaking them behind a fence that will never retire them. See RhiBindingSet comment for detailed lifecycle.

### blasInputs functions
- Geometry description is shared by prebuild query, build, and update. All three must agree on Flags/NumDescs/geometry or D3D12 either rejects the update or sizes the scratch incorrectly.

### toInstanceDesc
- Engine uses row-major row-vector matrices (v*M); DXR expects 3x4 column-vector [R|T] layout. Flag mapping values are chosen to match D3D12_RAYTRACING_INSTANCE_FLAGS one-for-one, enforced as explicit mask-and-assign (not blind cast) so engine flags without D3D12 twins fail to compile rather than silently passing unknown bits to the driver.

### createBlasImpl / createBlasUpdatable
- Updatable structures allocate their own scratch covering both build and update operations. Non-updatable structures use shared build scratch. A destroyed mesh is invalid even when handle is in-range because slot exists but vertex view is cleared.

### createBlasMulti
- One acceleration structure over multiple meshes. Sized by prebuild query over all geometries at once.

### setTlasStaticInstances  
- Slot indices are fixed: dropping a prefix instance would hand every later instance its neighbor's transform. Distinct BLASes are gathered and pinned (never compacted) because their addresses are baked into a persistent descriptor buffer. Refusal leaves TLAS exactly as it was.

### destroyBlas
- The slot is cleared and kept, not removed, so a stale BlasHandle names something dead rather than something else's structure. blasMesh() returning 0 signals destruction and lets dependent caches (like VoxiRenderer) notice expiry on their own.

### setUav
- CreateUnorderedAccessView on a texture without ResourceBind::UnorderedAccess flag does not fail at call time; instead D3D12 triggers RemoveDevice with DXGI_ERROR_INVALID_CALL, crashing the whole adapter, the upload ring, and all later CreateRootSignature calls. Early validation check here prevents device removal and converts it into a named diagnostic.

### clearSrv
- Must write to staging heap, not live GPU range, because frames in flight may still be reading the old descriptor from the GPU copy. Can't race a GPU read: only setBindingSet ever copies out of staging. This is not a no-op: leaving an old descriptor in the GPU copy is what crashes when the texture is destroyed.

### viewFitsBuffer
- Buffer overrun views cause shaders to walk off the end, surfacing as DXGI_ERROR_DEVICE_HUNG on unrelated threads with no reference to the descriptor. Early validation prevents this by checking view bounds against buffer size and reporting the slot/count/stride/firstElement in a named diagnostic.

### blasForMesh
- Linear scan, proportionate because blases_ has one entry per distinct ray-traced mesh, walked when a feature first meets it (not per frame). `built` flag makes the result safe to hand over. Destroyed structures clear both `mesh` and `built` so dead slots exclude themselves.

### destroyBlasForMesh
- Destroys all structures built from a mesh, including multi-mesh structures if any geometry names it.

### destroyShader
- shrink_to_fit after clear() because clear() alone leaves capacity allocated. Precompiled shader bytes are the whole point of this branch existing; a library's hundreds of permutations can be megabytes.
