# Code notes: rhi

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-03 comment strip so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/rhi/include/aver/rhi/EditorLines.hpp

- Editor lines rendered after post chain (not into HDR scene): Lines used to be rasterised straight into the HDR scene target with inverse-tonemapped colour. Auto-exposure then multiplied them (x70-150 in a lit level) and bloom haloed them. Changed to match Unreal's approach: composite after tonemapping so lines show exactly their authored display colour at display resolution, unaffected by exposure, tonemap, bloom, local exposure, or AverSR upscale.

- Thick lines without hardware line width (D3D12 has 1 px limit): Each segment stored as a quad with both endpoints on all four vertices. VSEditorLine projects the pair and pushes the corner sideways in screen space by half the width, clipping against near plane first to prevent lines behind camera from flipping. One-pixel feather anti-aliases edge (premultiplied alpha).

- Occlusion without scene depth bound: Display target and scene depth differ in size under render scale. Pixel shader samples scene depth and compares unprojected distances from camera (both unprojected through gInvViewProjRel) with small tolerance. Keeps lines lying on surfaces (grid on floor, outline on mesh) visible.

- Wireframe view (IDevice::setWireframe): Draws here too. While on, device queues every scene mesh with queueWire() instead of shading; replay() draws each with FillMode::Wireframe in flat Unreal wire colour, before lines. No depth test, as in Unreal's Wireframe view.

- Handle lifecycle: Handles start at 1, never reused. A stale handle must address a dead mesh (IDevice::destroyLineMesh's invariant). Mesh rebuilt every frame costs one 24-byte slot in meshes_ per rebuild; GPU buffers do not (shelved).

- Deferred release and shelving: GPU buffers retired kFramesInFlight replays later because a draw may already be queued or in flight. Small buffer (<= kMaxSpareBytes) then shelved for later reuse. A mesh rebuilt every frame (collider overlay's moving bodies in Play) otherwise commits two upload buffers per frame AND grew factory's append-only buffer table by two slots per frame forever. Shelved buffer held for reuse; acquire() takes it only once idle >= kFramesInFlight.

- Frame interpolation in replay(): Device presents two images; same queue replayed onto both. firstOfFrame=false on second replay skips retire tick (which counts FRAMES — ticking twice would recycle a buffer GPU may still read). lastOfFrame=false on first keeps queue for second. Defaults are for one-replay frame.

## modules/rhi/include/aver/rhi/RHI.hpp

- `DeviceDesc`: The preference order is the actual selection mechanism. A backend never asked for cannot be exercised or developed against. Until now nothing wrote it -- backend choice was decided by which ones were compiled in (a `#if` in RHI.cpp).

- `DeviceCaps::rtBindlessTextures`: Separate from resourceBindingTier, not derived. Raster stays "explicit descriptor tables, not bindless" (RHIResources.hpp, floor FL 11_0/tier 1); this covers only ray tracing (DXR 1.1 + SM 6.5, always tier 3). Deriving from resourceBindingTier would also be wrong on Vulkan (that field is hardcoded 0 there).

- `DeviceCaps::shaderInt64Atomics`: Queried from the device, not derived from shaderModel. SM 6.6+ does not imply this, so it is not inferred from the DXC target — two independent D3D12 queries (OPTIONS1::Int64ShaderOps, OPTIONS9 for typed-resource). Getting it wrong either corrupts the map (assumed true, actually false) or wastes a 16 MiB spin-lock fallback buffer (assumed false); hence asked, not inferred.

- `setSimulatedDeviceLoss`: Simulated device loss (after this many presented frames). Not a CapsOverride token, though --force-caps is the obvious neighbour: that struct's fields only ever reduce a capability, but losing a device is an event, not a capability. Exists because real device loss (driver timeout/update, hardware fault) is rarely exercised honestly, so the recovery path would otherwise rot silently until the one day it happens for real.

- `kLuminanceToCdm2`: LevelSky.hpp sets sky.sunIntensity = w.sunLux / (100000/3) so the default sunIntensity 3.0 agrees with OcWorldEnv's default 100000 lux; inverted, one engine irradiance unit is 100000/3 cd/m^2, and since the renderer treats irradiance and the pre-exposure scene-linear pixel value (radiance) as the same unit throughout, one engine radiance unit is too. Named once here because PostSettings' eye adaptation needs real cd/m^2 (Krawczyk et al.'s formulas are fit to measured luminance).

- `PostSettings::bloomThreshold`: 4, not 1. MEASURED on PTTest Sponza (--frames 244, viewport mean/chroma R-B): exposure 1->3.32, 2->4.47 (peak), 3.2->4.34. Sunlit stone reads 2-6 after exposure, so at 1 a sunlit floor bloomed into a white haze across half the frame at the owner's Bloom 0.366, where UE's stays crisp with a slight glow. At 4 the sun disc, lamp bulbs and specular glints still bloom; diffuse sunlight barely does.

- `PostSettings::exposureMax`: A ceiling of 8 (copied from maxRadiance's radiance clamp below) is wrong for an exposure MULTIPLIER. MEASURED on PTTest Sponza (--frames 244, viewport mean/chroma R-B): exposure 1->19.38/3.32, 2->29.79/4.47 (peak), 3.2->38.66/4.34, 5->48.76/3.28, 8->61.61/1.40 (what auto-exposure picked) -- chroma collapses by 8, blue overtakes green above 5. `--exposure 8` reproduced the auto-exposed image within 0.02/channel, confirming the clamp was the culprit. Capping lower (tried 3) removed the washout but read "too dark": the scene needs the gain, just can't survive the CURVE -- fixed at the source via tonemap mode 2 (acesLumaTonemap: chroma 3.09 at exposure 8 vs mode 1's 1.41, same brightness). Ceiling went back to 8; range stays wide on purpose since a tighter ceiling pins the exposure across light/shade. MEASURED on NewSponza (exposureKey 0.18): adaptation wanted x73 (courtyard noon), x99 (arcade), x120 (dusk), x151 (night, lamps only) -- a 16 ceiling (2026-09-26) pinned all four. 256 = 8 stops above 1, inside a real camera's range.

- `PostSettings::exposureSpeedDark`: 1.0 -> 0.5: the scotopic slowdown below (rods up to 4x, adaptationRealism 1) stacks on this base speed, so it was lowered to keep the same real dark-adaptation time rather than compound.

- `PostSettings::adaptationRealism`: Perceptual eye adaptation (Krawczyk, Myszkowski & Seidel 2005). [0,1]: 0 = full adaptation (every view drives to exposureKey, today's default behaviour exactly); 1 = perceptual model. Scales two effects: Partial adaptation (sec. 4): keyEff = exposureKey * lerp(1, alpha(Y)/alpha(Yref), adaptationRealism), alpha(Y) = 1.03 - 2/(2 + log10(Y+1)), Yref = 100 cd/m^2. Rod-slowed dark adaptation (sec. 4): darkening gets up to 4x slower in true darkness. Implemented in post.hlsl's CSExposure (gPostEye.x).

- `PostSettings::nightVision`: Scotopic night vision (Krawczyk et al. sec. 5, after Kim et al.), "Night Vision" in panel. Below ~1 cd/m^2 rods take over from cones, draining colour and blue-shifting (Purkinje effect) as this rises toward 1; 0 leaves pixels as rendered. Runs per pixel on scene-linear radiance before exposure (post.hlsl PSComposite), so it applies regardless of auto-exposure.

- `PostSettings::meteringCenterWeight`: Centre-weighted metering (console post.meteringCenterWeight). Weights the centre up to 4x an edge pixel, like a camera's centre-weighted meter, so a bright sky or floor at the region's edge can't dominate the reading. Implemented in post.hlsl's CSHistogram (gPostEye.w).

- `PostSettings::exposureKey`: Target brightness -- the log-average of the histogram's middle band that adaptation drives every view toward. CALIBRATED AGAINST UNREAL 5 + LUMEN (2026-09-28, owner: "calibrate ... to look like the lighting in" a UE5 Sponza video) together with the metering band, tonemap and local exposure below. Display-space statistics (luma percentiles 5/25/50/75/95, mean HSV saturation) of the video's daylight shots vs headless captures of NewSponza at matching poses: balcony UE [30,72,109,150,204] .27 ours [42,67,90,130,250] .25, arch UE [3,17,42,95,228] .33 ours [20,29,50,115,225] .42 (red curtains fill ours), gallery UE [25,46,63,93,190] .36 ours [34,60,77,104,157] .41. Then HALVED to 0.125, one stop down (owner, same day, looking at it in the editor: "make the current eye exposure -1.0 the default so it would be +0.0 because default currently is too bright"). SandboxSettings' post.settingsVersion 3 doubles a stored Brightness once, so an editor already at -1.0 keeps its picture and reads +0.0.

- `PostSettings::histogramLowPercent`: 0.10 / 0.90, Unreal's own auto-exposure defaults. The old 0.30 / 0.95 kept a sunlit floor in the average, so it set the exposure and the shaded 90% of the view sat dim around it; dropping the brightest tenth exposes for the shade, and sunlit stone clips the way it does in UE. A sun disc, lamp bulb, glint or firefly still can't darken the whole view.

- `PostSettings::tonemap`: 0 = original per-channel Narkowicz/Hill approximation; 1 = the same curve between the ACES input/output matrices (colour.hlsli's acesFittedTonemap); 2 = acesLumaTonemap (tonemaps luminance, restores original chromaticity). 1 is the default since the Unreal calibration (2026-09-28, see exposureKey): UE's filmic curve runs per channel in the ACES AP1 space, which is what mode 1's matrices do, and it matched UE's saturation (balcony .285 vs UE .27; mode 0 .304) with a firmer toe. Mode 0 (the owner's 2026-09-24 choice, gentlest toe so dim indirect light stays visible) is post.tonemap 0. Mode 2's Hill RRT/ODT fit (x2 gain) has a hard black point at ~0.0016 and crushes anything below ~0.02 by ~0.2, reading physically-correct bounce light (NewSponza stone albedo ~0.1-0.2) as "no GI". Mode 1 still greys out at big exposure: MEASURED chroma (mean R-B) vs exposure, 1x->3.32, 2x->4.47, 5x->3.28, 8x->1.40 ("colours are washed out"); mode 2 at 8x holds 3.09 at the same brightness, and above 5x mode 1 also lets blue overtake green.

- `PostSettings::maxRadiance`: 8 is derived: acesTonemap(8) = 1.003 (saturates to pure white), so clamping there changes no pixel's tonemapped colour -- only bloom, which thresholds the unclamped value and so bleeds an unbounded halo off a ~42x disc. Clamping bounds the halo while the disc stays blazing white. Raise it for more bloom from very bright sources; 0 restores the unclamped behaviour exactly.

- `PostSettings::localExposureShadows/Highlights`: OFF by default since the Unreal calibration (2026-09-28, see exposureKey), matching UE, whose local exposure is also off unless asked for. Its images have deep shade under a sunlit courtyard, which shadows lifting works against (balcony darkest 5% 57 -> 52 turning both off, UE 30), and a sunlit wall is allowed to clip. Neither is in the panel or persisted; console post.localExposureShadows / post.localExposureHighlights turn them back on (the previous tuning was 0.10 / 0.5, for a no-clipping look).

- `SkyAtmosphere::cloudSeed`: Which sky this is: two levels with different seeds get different cloud fields from the same settings (lets a PCGVOLUME's seed reach the sky). 0 is the unseeded field and reproduces previous output exactly -- deliberate, so this lands without moving any recorded gate probe, and so anyone can bisect a sky change without wondering whether the seed did it.

- `GpuTimingNode`: One node of a per-pass GPU timing report -- the public mirror of D3D12Device's private GpuAccum tree (see its own comment for why it's keyed by (label, parent), not a flat list). Flat and parent-indexed here since the source data is already this shape.

- `GpuTimingReport`: Two independent "no data" axes, not collapsed into one: `supported` is the CAPABILITY axis: false means this backend cannot report timings at all (Vulkan: no machinery yet; D3D12 with timing disabled). `nodes` is then always empty. `nodes` empty (or `framesAccumulated` 0) with `supported` true is the CONTENT axis: enabled but nothing accumulated yet (frame 0, or every span this frame dropped). Lets a caller tell "ask again later" from "will never answer", which one bool couldn't.

- `VideoMemoryInfo`: "Budget" is the OS's current per-process ceiling, not the physical total -- it moves as other processes/the compositor claim their share, hence polled rather than read once.

- `IDevice::addRenderFeature`: Render-feature registration. NON-owning: the caller keeps the feature alive. Exposed so a registered IRenderFeature can override scene draws (D3D12Device::drawMesh's overridesScenePipeline). Exposed here so a CALLER too can interleave setPipeline/dispatchMeshClusters calls with ordinary drawMesh() calls, for the SUBSET of instances wanting a different draw path (per-cluster GPU LOD is the first consumer; most instances still go through drawMesh()). setPipeline() invalidates cached root-signature/PSO state as a side effect, keeping drawMesh() safe right after -- same contract IRenderFeature's override relies on.

- `IDevice::setUpscaler`: The upscaler turning scene-resolution colour into the present-resolution image, or null for none (default; must stay bit-identical to a build with no upscaler module -- docs/AVERSR.md's invariant for quality Off). Non-owning like addRenderFeature: caller keeps it alive, composition root is the only place that knows the concrete type -- lets Aver.Render.Sr be linked by the HOST alone (docs/AVERSR.md). Defaulted no-op so every IDevice implementation compiles unchanged: Vulkan backend (VulkanDevice.cpp) is mid-bring-up and is a pure virtual here would break its build for a feature it does not yet have.

- `IDevice::frameInterpolation`: NOTHING IS TIMED (NEURAFI.md §5): the GPU's own progress is the clock. The generated image is shown when its frame's work finishes; the real image one frame later, when the GPU reaches frameMidpoint() in the NEXT frame's work -- so real and generated alternate at whatever rate the scene renders, doubling it, without anything measuring a frame time.

- `IDevice::viewportAspect`: Exists because a wrong-aspect camera is invisible until measured: PtSceneView built rays for a fixed 16:9 accumulator while the editor docks at any ratio, and the straight NDC blit stretched the traced image by dstAspect/srcAspect -- 1.06x on the dockspace it was found on.

- `IDevice::setRenderScale`: Deferred to the next beginFrame() when a swapchain exists (optimisation-wave-2, C2-13): D3D12Device/VulkanDevice PARK the value and apply via their own applyPendingRenderScale() (first statement of beginFrame) -- an immediate mid-frame rebuild loses the device (see either implementation's own comment; aver-render-scale-device-loss).

- `IDevice::deviceLost`: Needed because a GPU can vanish underneath a running process (driver timeout/update, hardware fault) without the API reporting it at the call that caused it -- every later call just fails quietly, and the process carries on issuing work into a device that will never run any of it until something finally faults hard, which is the shape of "the engine crashed with no message" unless the layer that detects removal can tell the layer driving the frame. One-way and sticky: nothing here recovers a lost device (that means recreating every resource every module owns); this is the honest minimum -- stop, say so, keep the last good frame.

- `IDevice::sceneSuppressed`: drawMesh submits to every feature BEFORE honouring suppression (a suppressing feature is usually building its own scene from those submissions, so skipping submitDraw would starve it) -- so an EDITOR-ONLY draw (outline, gizmo, chrome not scene) still gets baked into that feature's output; drawMesh can't filter it out after the fact.

- `IDevice::gpuTiming`: Two frames old, on purpose: timestamps are resolved from a readback slice only readable once the GPU has caught up, which beginFrame fences on before collecting (see D3D12Device's own per-pass-timing comment) -- reading "this frame's" own timings would stall on the GPU to ask how fast the GPU was, creating the stall it reports. A caller polling once a frame reads a rolling average a couple of frames behind, not a live number. Returned by value, not a reference: source data mutates every beginFrame (spans folded in, occasionally reallocated), so a handed-out reference would go stale. This snapshot (a dozen or so short-label nodes) is cheap to copy and safe to hold.

- `IDevice::sceneDepthTexture`: The backend's OWN scene depth target, registered as an ordinary TextureHandle through the SAME resource-factory table createTexture() populates, so a caller reaches it with the generic setSrv/textureBarrier vocabulary rather than a bespoke accessor (deliberate design-review correction -- see modules/occlusion/include/aver/occlusion/Occlusion.hpp's top comment, point (c)). Declares its own sample count via sampleCount() above (multisampled needs SlotKind::Texture2DMS, not Texture2D).

- `IDevice::sceneColorBackdropTexture`: Hardware alpha blending attenuates the destination by one scalar (1-src.a), but volume absorption (Beer-Lambert) is per-channel and grows with path length -- glass is green because iron passes green, eats red. So a blended surface can go DARKER with depth via alpha, but never TINT what's behind it -- the whole reason glass here couldn't show real glass's green edge. Handing the shader the background as a texture lets it composite itself instead. One copy, taken once: a second translucent layer samples a background missing the first (glass over water reads water's backdrop, not the water) -- the standard trade (UE's distortion pass makes it too), cheaper than per-channel destination blending, which D3D12 can't offer here (four render targets bound with G-buffer on, dual-source blending needs exactly one).

- `IDevice::G-buffer`: Aver is a FORWARD renderer (PSMainVoxi returns one SV_TARGET): a shaded pixel's normal/roughness/depth live only in that invocation's registers, unreadable by any later pass. Blocks the vendored FidelityFX Denoiser (third_party/fidelityfx-denoiser's README lists ReadDepth/ReadNormals/ReadVelocity/ReadPreviousDepth as callbacks the host must supply; today the honest answer is "no" to each), FSR2/3, TAA, and SSR alike -- each needs to read what a previous pass saw, which until now nothing could hand it. Also why temporal reprojection is wrong for moving objects today (reprojects THIS frame's position through LAST frame's camera, gPrevViewProj in VoxiShaders.hpp -- correct only for static geometry; the full fix needs a per-instance previous transform this G-buffer doesn't carry, see RtInstance in VoxiRenderer.hpp). Additive and defaulted throughout: setGBufferEnabled defaults OFF, and every accessor below defaults to a safe "nothing here" value (0 for a texture, false/true for the bools, whichever is the safe reading), so never enabling it allocates none of the three targets, records no extra writes, and renders BIT-IDENTICAL to a build without this declaration. OPEN: a prior "nothing yet enables this" claim here went false within a day of being written (2026-08-29) -- SandboxApp.cpp passes `gbufferOverride_ || gbufferDebugView_ != Mode::Off`, driven by --gbuffer/--gbuffer-debug, and viewport debug reads it back; only the packaged runtime abstains. Left in as a reminder this file has a history of stale absolutes outliving what they described; a render-gate oracle (18x9 configs) and 89 headless suites both assume the accessors hold their defaults while gBufferEnabled() is false -- a backend that allocates or writes any of this while reporting false would fail both without pointing at why.

- `IDevice::gBufferVelocityTexture`: UNITS: TEXELS PER FRAME, DESTINATION TEXEL MINUS SOURCE TEXEL -- for a point shaded at THIS frame's pixel (x,y), stored (vx,vy) satisfies (x,y) - (vx,vy) == where that surface point was LAST frame. Matches UpscalerNeeds::MotionVectors' convention (RHIResources.hpp) exactly, since FSR2/3, DLSS, and FFX_DNSR_Shadows_ReadVelocity all read texel-space motion this direction -- a mismatch here would silently reproject to the wrong pixel with no compile error or crash.

- `IDevice::gBufferViewZTexture`: UNITS: VIEW-SPACE LINEAR DEPTH (shaded point's Z in view space, i.e. clip-space W pre-divide) -- deliberately NOT sceneDepthTexture()'s post-projection [0,1] value; the two relate by a non-linear "depth precision" remapping, so treating this as [0,1] depth is quietly wrong at every pixel, not a crash. FFX_DNSR_Shadows_ReadDepth/ReadPreviousDepth and any SSR pass want this linear form because it makes reconstructing a view-space position from a screen UV a single division, not a full unproject.

- `IDevice::gBufferNormalRoughnessTexture`: xy = the world-space normal, octahedral-encoded (Cigolle et al. 2014: L1-normalise, fold the lower hemisphere with p = (1-|N.yx|)*signNotZero(N.xy) when N.z<0, then p*0.5+0.5); z = roughness; w = 0 (there is no material-ID concept yet). Encode: averPackNormalRoughness (modules/render.voxi/shaders/voxi.hlsl). Decode: f = e.xy*2-1; n = (f, 1-|f.x|-|f.y|); if (n.z<0) n.xy = (1-abs(n.yx))*signNotZero(n.xy); normalize(n) (see sandbox/shaders/gbuffer_debug.hlsl and the denoiser's shader). A raw n*2-1 sample of xyz is wrong -- it gives a plausible-looking but per-pixel-wrong vector, exactly the shape of bug a casual visual check misses.

- `IDevice::gBufferHistoryInvalid`: Every temporal consumer must ask this, not infer it: getting it wrong is ONE BAD FRAME right after every cut -- invisible to a still-frame review, always visible once the camera actually moves (see this engine's own per-object reprojection bug above).

- `blackbodySrgb`: LINEAR: a caller storing this into a display-encoded field (sunColor, lightColor) must re-encode with pow(x, 1/2.2) first, or every downstream pow(x, 2.2) decode reads it twice.

## modules/rhi/include/aver/rhi/RHIResources.hpp

- **Format enum (RG16F comment)**: Screen-space motion vectors.

- **Format enum (RGB10A2Unorm comment)**: 8-bit banding concern.

- **ResourceState::GeometryRead comment**: Read state for three distinct uses (assembler, manual fetch, BLAS).

- **kMaxBindingSlots constant**: Raised to 24 for headroom.

- **PipelineLayout struct comment**: Register space 1 already spoken for.

- **TlasInstance::instanceId comment**: Use CommittedInstanceID() not CommittedInstanceIndex().

- **TlasInstance::flags comment**: Per-instance behaviour.

- **BlasGeometry struct comment**: Opaque is per-geometry.

- **IResourceFactory::setBindlessTexture**: Returns false and logs on out-of-range.

- **IResourceFactory::createBlasMulti**: One acceleration structure over multiple meshes.

- **IResourceFactory::setTlasStaticInstances**: Fixed-size prefix for millions of static instances.

- **IResourceFactory::clearSrv**: Clears slot to null-filled state.

- **IRenderContext::drawMeshInstanced**: Per-instance StructuredBuffer<float4x4> mechanism with SV_InstanceID indexing.

- **IRenderContext::dispatchMeshClusters**: Cluster-shaped dispatch.

- **IRenderContext::copyTexture**: Whole-resource, not a region.

- **IRenderContext::copyTextureToBuffer**: Backend layout, not tightly packed.

- **IRenderContext::refitTlas**: Updates when conditions match, falls back on change.

- **ScopedGpuStat class**: RAII pattern for PIX and GPU-timestamp roles.

- **IRenderFeature::submitDraw**: Blended-draw notification purpose.

- **IRenderFeature::scenePipeline**: Vertex transform must be bit-identical.

- **IRenderFeature::suppressesScene/suppressesWholeFrame**: Distinction between debug view (whole frame) vs ray-driven (primary visibility only).

- **IRenderFeature::overlayPass**: Scene depth is readable.

- **UpscalerNeeds::MotionVectors**: Texels/frame motion.

- **IUpscaler interface**: Placeholder for future DLSS integration.

- **IUpscaler::needs()**: Declared once (algorithm property, not per-frame).

- **IFrameInterpolator::generate**: Core input/output contract.

## modules/rhi/src/EditorLines.cpp

- **Binding set per-frame-in-flight strategy**: One descriptor set per frame in flight per depth-sample flavor. Prevents a descriptor write (setSrv) from reaching a draw that is still executing from an earlier frame. Same design documented in ViewportIconRenderer for its per-icon sets.

- **Wireframe view rendering**: Uses the engine's default MeshVertex layout, renders edges only (wireframe fill mode), with no depth testing or blending. Failure to compile the wireframe shaders is logged but does not affect the main line drawing.

- **Root constants declaration**: On D3D12, declaring constantDwords on the layout is critical. An undeclared slot becomes a root constant buffer view (root CBV), which caused setConstants to fail with "slot 1 declares constantDwords 0" on every draw. ActorPreview uses the same pattern.

- **Vulkan binding declaration**: Vulkan requires explicit slot-kind declaration even when the layout could be reflected. The two pixel shaders (PSEditorLine and PSEditorWire) read t0 as different kinds (Texture2D and Texture2DMS respectively), and Vulkan's reflection is not sufficient to disambiguate—hence slotKindsDeclared must be true.

- **Frame-in-flight slot advancement timing**: The frame-in-flight slot must be advanced BEFORE writing the descriptor. This is not a simple fence stall optimization; by construction, three replays separate the current write from an earlier frame still reading the previous contents of the slot being reused.

- **Constant buffer layout**: The PerObject constant buffer (shared_prelude.hlsl) is 32 dwords. In the line rendering, only a subset is used: gWorld[0..15], gBaseColor[16] = line width, gBaseColor[17] = depth test enable flag, gBaseColor[18..19] = viewport size, gMaterial[20] = target sRGB flag, gMaterial[22..23] = viewport minimum, gEmissive[28..29] = target size. Other fields are left zero.

- **Color space handling**: For sRGB targets, wireframe colors and constants are converted from linear to sRGB space via srgbToLinear before writing to constants. The target format is queried at runtime to determine the conversion (formatIsSrgb).
