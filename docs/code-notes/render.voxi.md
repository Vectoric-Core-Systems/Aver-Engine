# Code notes: render.voxi

Design decisions, measurements and history that used to live in code comments. Moved here in the
2026-10-03 comment strip so the code stays readable. Each section names the source file; symbols in
backticks are where the knowledge applies. Measurements are as originally recorded and may be stale.


## modules/render.voxi/include/aver/voxi/GiDispatchBounds.hpp

- **Architecture**: VoxiRenderer accumulates the union of every draw's voxel-space box using voxelBoxFromWorldAabb + unionBox, pads the mip-0 result outward with alignOutward, and derives each coarser mip's box with mipBox before CSClear/CSResolve/CSMip dispatch. This header supplies arithmetic only; deciding when to fall back to the full grid (first build, resolution change, moved/resized volume, unbounded draw, cache restore) reads VoxiRenderer's state and belongs there.

- **VoxelBox::empty() semantics**: There is no single canonical "the empty box" bit pattern every function must agree to produce. Empty is true the moment any ONE axis is degenerate (lo >= hi there). Every function already treats any degenerate axis as "no voxels on this axis, so none in the box" and reads correctly regardless of what the other two axes hold.

- **Alignment rationale for CSClear/CSResolve**: Both are [numthreads(4,4,4)]. Without alignment, a lo or hi that is not a multiple of 4 would pass unchanged (per-thread guard is correct on its own), but the two passes' dispatched group rectangles would only approximately agree with the box. A boundary voxel could end up resolved without being cleared, or vice versa, purely from rounding group counts independently. Aligning the BOX itself once, before either dispatch, removes that seam.

- **mipBox exactness**: CSMip's box filter reads a 2x2x2 block of source level for every destination texel. A mip-0 voxel at index h-1 (last inside half-open box ending at h) is read by exactly destination texel (h-1)>>mip. Floor/ceil rounding of a half-open range under power-of-two division is exactly "smallest destination range whose footprints cover source range" — neither wider (would ask CSMip to filter texels whose entire 2x2x2 sits outside source, reading uninteresting data) nor narrower (would drop real source texels). Therefore the mip box is not a safe overestimate; it is the SMALLEST box with no gap, which is why alignOutward is applied only once before mip 0, never per mip level (rounding outward twice would be redundant).

- **Non-finite inputs**: Non-finite AABB corners (from degenerate mesh or unset AABB) or non-positive volumeSize returns fullVoxelBox(res). The honest answer to "where does this draw touch the grid" is "cannot tell", which gets the same treatment as an unbounded draw in the W3 spec — the rebuild covers everything.

- **GiDispatchConstants layout**: Byte-for-byte mirror of voxi.hlsl's cbuffer MipCB (register b3). HLSL cbuffer packing places gSrcMip at byte 0 (4 B) and gBoxLo at byte 4 (12 B, fitting in same 16-B slot since 4..15 does not straddle 16). gBoxHi starts a fresh 16-B slot at byte 16; gSlabZ fills last 4 B. No partial-empty slots, struct needs no compiler-inserted padding; all members 4-byte aligned, total already multiple of 4.

## modules/render.voxi/include/aver/voxi/GiVisibility.hpp

- **Unification rationale**: This header is unified with voxi_restir.hlsli to prevent divergent bit definitions. The bit packing is read/written by both C++ (VoxiRenderer.cpp) and HLSL (voxi_restir.hlsli). packAmbientW is the single source of truth; GiVisibilityTest checks the C++ version against the shader's decode arithmetic to catch silent bit drifts.

- **Float representation constraint**: gAmbientParams.w is a float32 that encodes a u32. Float32 holds integers exactly only to 2^24; bit 24 is the boundary. Using bits beyond 23 causes silent rounding of low fields instead of failing gracefully. This was caught when a debug dial briefly sat at bit 24 and its capture showed as a no-op.

- **halfDim rounding logic**: The formula (d + 1) / 2 makes an odd full-resolution edge round UP to the next half-resolution texel. This matches the mip-chain halving pattern (max(dim >> 1, 1)). Without the +1, a 2x2 block with one full-resolution row/column left over would be lost.

- **tracedPixel pattern**: The four cyclic positions {(0,0), (1,1), (1,0), (0,1)} define which pixels trace rays each frame vs reconstruct from history. The pattern cycles every 4 frames using frame & 3u (bitwise AND, not modulo) to match the codebase's cyclic-schedule convention (tileMask/turnMask in voxi_rt.hlsli).

- **nDot pre-saturation contract**: The reconstructWeight function expects nDot already saturated to [0,1], matching the shader's saturate(dot(...)) before pow(). The function does not re-saturate, so a raw unclamped dot product (which can be negative) gets the "opposite normals reject" behaviour automatically: pow() of a value <= 0 at a non-integer exponent evaluates to the function's explicit floor at 0.0, avoiding undefined-behaviour territory.

- **spatialSamples field semantics**: Bits 12-15 (4 bits, range 0..15) hold Settings::giRestirSpatialSamples. 15 is a sentinel meaning AUTO (leave the camera-motion discount's own count alone). 0 disables spatial reuse (temporal only). 1..8 pin the count. Four bits are needed because 15 must be distinct from every real count.

- **maxHistory field scaling**: Bits 18-22 hold the cap on temporal history M. The camera-motion fade scales with this value: 0 = no overshoot, 1 = +8%, 8 = +104% (measured on NeonDistrict).

- **neurac visibility mode**: RestirVisibility::Cached = 4 would silently decode as 0 (NoRay) if packed as a fifth mode value (the shader decodes as & 3u). Instead, the CPU packs Cached as mode 2 (HalfResolution) plus bit 128 (neurac). This way the existing halfBound/tracedPx/rec.valid logic makes tracing decisions unchanged; only AVER_NEURAC twin pipelines read the neurac bit.

## modules/render.voxi/include/aver/voxi/NeuRaC.hpp

- **Ownership model**: NeuRaC owns the accumulator, cells, RcInfo ring, and resolve compute pipeline. It does NOT own scatter or lookup operations; those are HLSL in voxi_neurac_io.hlsli (AVER_NEURAC twin pipelines), reading/writing through table 0 (t22 info, u20 accum, u21 cells).

- **Synchronisation model**: Both big buffers (accum, cells) are UAV-only in COMMON state, so they never need state transitions. This is deliberate for a barrier-free staged lighting group (same reason rdGiCandBuf_/rdVisBuf_ are UAV-only). The only synchronisation is uavBarrierBuffer(), called between scatter and resolve and after resolve.

- **Frame order invariant**: Reads in frame N see the cache resolved at the end of frame N-1. beginFrame snaps cascade origins and writes RcInfo to the next ring slot (rotated BEFORE write because writeBuffer is immediate but GPU dispatch is deferred). recordResolve runs after scatter finishes.

- **RcInfo ring design**: The ring has one slot per frame-in-flight. The clear-all buffer is separate from the ring because writeBuffer is immediate while dispatch executes later: reusing a ring slot would make the GPU see the wrong flag value (either the final contents if flipped back, or a frame's own resolve wiping its own samples if left set).

- **Binding set invariant**: One resolve set per ring slot (t0 = that slot's info) plus one for clear. Written once at create and never rewritten after binding—this is a Vulkan constraint on ringed binding sets.

- **Warmup stats purpose**: kStatsAtFrame frames after cache goes live, beginFrame copies cells to a readback buffer (at frame top, after the previous resolve finished). kStatsReadDelay frames later (past frames-in-flight limit), logWarmupStats reports per cascade: how many cells hold valid samples, mean n_eff/age/planarity, and mean ambient light. This one-shot report (readback buffer freed after) answers "is the cache filling"; a failed lookup falls back to HalfResolution and looks identical on screen, so this is the only way to verify cache activity.

- **alphaMin temporal blend**: The 0.0625f default (~16 frames to settle) is a tunable for how quickly the cache adapts to lighting changes. This is per-frame via Params, not a Settings key, because the value is not yet measured to justify a manifest dial.

- **ageStepFrames cadence**: Cells without new samples age one step per this many frames (default 16). This trades off freshness against noise and bandwidth.

## modules/render.voxi/include/aver/voxi/NeuRaCLayout.hpp

- **Header synchronization architecture**: Three parties must agree byte-for-byte on layouts: NeuRaC.cpp (writes buffers), voxi_neurac*.hlsli / voxi_neurac_resolve.hlsl (reads/writes cells), and NeuRaCTest (CPU-side arithmetic validation). Since HLSL cannot include C++, the header declares constants mirrored as #defines in HLSL; NeuRaCTest greps those literals against the C++ header to catch drifts that would silently corrupt fixed-point sums.

- **Cache design**: Three camera-centred 64³ cascades with cell sizes 25/100/400 cm (world units). ReSTIR GI's second-bounce rays scatter fixed-point first-order SH of incident radiance into an accumulator via integer InterlockedAdd (order-independent). Once per frame, resolve folds the per-frame mean into cells; pixels that did not trace read the cells. Cache is trained live during rendering.

- **Accumulator layout rationale**: Counts occupy contiguous region [0, kCells) separate from payload starting at kCells. Resolve early-outs on count == 0, so most cells are empty; streaming 3 MB of counts is far cheaper than touching 786k sparse 64-byte lines to discover emptiness.

- **Headroom proof**: Worst single sample component: L = kLMax, basis term ≤ 0.488603 (L1 terms), weight = π / kMinCos. Per-cell cap (kMaxCap) bounds sum per epoch. Worst SH sum ≈ 1.03e9; worst normal sum ≈ 4.2e6; both fit int32 with 2x safety margin. static_assert prevents silent overflow.

- **Window origin snapping**: min-corner world cell = floor(camera_position / cellSize) - kHalfRes. Scrolled-in cells retain their previous occupant's tag until overwritten; metadata's tag field distinguishes old from current residents.

- **Normal-field semantics**: n_eff is a 4-bit field (0..15), where 0 = empty cell (no samples). Age field also 4-bit, in units of ageStepFrames. Validity: n_eff > 0 && tag == rcTagPack(current world cell) && age < kAgeMax.

## modules/render.voxi/include/aver/voxi/QualityLadder.hpp

- `voxelResolution`: Volume memory cost per rung: Off/Low (64): ~6 MB, Medium (128): ~50 MB (default), High (256): ~400 MB, Epic (512): ~3.2 GB. Frame-time cost per rung is unmeasured (volume built once at VoxiRenderer::init, never resized).

- `giCones`: Per-pixel diffuse gather cone count. Measured at ~0.22 ms/cone across the gather pass (verified independently via scene-draw times). Medium set to 6 (old hardcoded default at all tiers). The torn-pair bug example: function once misplaced above voxelResolution's comment.

- `giUpdateInterval`: History of this knob: Medium moved 4->1->2 over multiple passes. Still-camera measurement (Release, ElectricDreams, 1600x900, --no-vsync, --frames 200) showed intervals 1/2/4/8 median 18.54/18.47/18.50/18.46 ms (0.09 ms spread, noise). Under wobbling camera (Sponza, --cam-wobble 15 50, ray-driven + AverSR Balanced): 36.00 ms (interval 1) to 16.57 ms (interval 4). Low moved 8->4 (kMaxGiUpdateInterval clamp was widest). Interval 4 visibly trails vs interval 1 but is unmeasured for cost/lag under motion.

- `giRestirVisibility`: Low (Reconstructed) and Medium (HalfResolution) choices are unmeasured. Low: RT Low rasterises, so F2/F3 cost per fragment (PTTest depth prepass off). Reconstructed traces no extra ray. Medium: ray-driven, cost per pixel; traces exact visibility on 1-in-4 pixels at rest (<=0.5 rays/pixel vs Full's 2). Plan section 5 by-hand verification settles both before shipping. High/Epic (Full) are owner's decision (U1). No rung returns 4 (Cached radiance cache) until measured against HalfResolution and Full on owner's GPU.

- `refraction`: Evidence that three modes differ in output: 31.00/31.51/12.40% pixels changed between modes (aver-refraction.md). RayTraced mode cost is unmeasured.

- `rtShadowRays`: Low and Medium: each extra raster ray costs ~1.55 ms (ElectricDreams 1600x900: 18.44/19.99/23.39 ms at 1/2/4 rays). High retuned from 2->4, Epic from 4->8 (both moved up one step). Inside ray-driven primary, one sun ray costs ~0.017 ms (Sponza), ~2 orders cheaper than raster ray (coherent BVH). Quality evidence from old config (raster, unfiltered, still camera): penumbra error +27/+27/+9/0 codes at 1/2/4/8 rays. Under today's config (ray-driven + spatial filter, motion) it is unmeasured. Retuning is owner's direction (D1).

- `rtPixelsPerRayTile`: Always 1 at every rung. Amortisation measured 0.14 ms moving / 0.09 ms static against tile 1 (inside noise), but motion trail already visible on 0.80% pixels at tile 2 alone (tile 4 adds only 0.04% more). Torn pair: function said 4 for Low while Voxi.hpp field comment said 1. Available via RENDER.RTPIXELSPERRAY / --rt-pixels-per-ray overrides.

- `rtShadowDenoise`: Tap count is (2*radius+1)^2. Cost comparison: +0.02 ms for full 49-tap radius-3 kernel vs +1.69 ms for one more traced ray (~85x cheaper even at widest). Noise/drift trade: 12 to 32 codes under six-degree wobble, growing with radius. Low/Medium both 2 (trace one ray, hard dither); unmeasured at Low (filter per fragment multiplies tap count by overdraw). High moved from 2->1 (now traces four rays, real variance reduction over Medium's one). Drift scales with radius not ray count. Epic stays at 1 (eight rays); radius 1 is nearly-free improvement. Claim about "converges below sixteen rays" is withdrawn (contradicted by rtShadowRays). Radius 3 measured "within one code of sixteen rays" at old ray counts but tops drift range.

- `rtRenderMode`: Partial evidence for rasteriser vs ray-driven. At overview cameras: raster slower (ElectricDreams 20.55 vs 8.05 ms; PTTest 13.51 vs 8.04 ms). Low's 4x MSAA default contributes; old pre-texture-split measurement put MSAA cost at 4.2 ms (16.0 vs 11.8 ms). Close-up case once favoured raster (9.02 vs 24.04 ms) is unconfirmed (Path Tracing was running instead of ray-driven). Low can be slower at overview camera. Decision (D3) to keep Low on rasteriser is owner's, not a speed claim.

- `giSkyOcclusionRays`: Per-pixel sky-visibility rays for ambient AO. One ray measured 5.37 ms on Sponza (more than four sun rays). Historical: earlier revision said "Epic only" but dropped to 1 ray after accumulation against reprojected history. Medium added 2026-09-27: ReSTIR GI cone-gather occlusion hardcoded 1.0 (no AO), rendering twice as bright as Epic (Sponza arcade, AverSR Performance: mean 33.4 vs 16.2). One ray at half rate brings to 16.1 (MAD 0.31 vs Epic) for +0.5 ms (11.78->12.28 ms). Without rays, cone gather's occlusion measured far too open on Sponza (disabling brightens darkest 81% of frame 2.6x: mean 20.71->31.92). First sweep (Sponza, 112 entities, RENDER.RAYTRACING 4, MSAA 2, --no-vsync, --gpu-timing): 0 rays 10.92 ms, 1 ray 12.25 ms, 4 rays 15.58 ms. One ray buys correction; three more cost 3.33 ms for no visible change vs path-traced reference.

- `giSkyOcclusionTile`: Always 1 everywhere (inert with single ray). Tile traded correlated noise (4x4 block identical AO) for coherence across four incoherent rays. Kept as dial (old measurements +1.98 ms for four rays at tile 4 vs tile 1); future many-ray term could want it.

- `ptBounces`: Path-tracing bounce budget. Measured 4.7/5.9/6.3 ms at 1/2/4 bounces outdoors (ElectricDreams, 2750x1639). Three bounces unmeasured (interpolated). Stops at 4: bounces beyond second cost 0.4 ms and changed nothing measurable outdoors (paths escape to sky). Closed interior would differ; existence is unconfirmed.

- `averSrLevel`: AverSR spatial upscale. Evidence: PTTest 2026-08-31, 200 frames -- Quality -4.0 ms/-21.6% edge sharpness; Balanced -4.9/-25.3%; Performance -5.6/-29.4%. Baseline contaminated by second renderer running concurrently (plan C13) and predates ReSTIR and denoiser (pixel-bound additions Epic carries today). Absolute savings expected higher. Quality->Balanced trades 0.9 ms for 3.7 edge sharpness points; Balanced->Performance trades 0.7 ms for 4.1 more (diminishing returns). Epic/High (Quality) are gentlest "image quality" rungs. Low has 4x MSAA, real geometric AA at quarter internal pixels; can afford most aggressive upscale (Performance).

- **Torn pair bug history** (Lines 7-9, 47, 205): Two shipped bugs caught by person rereading two files: giUpdateInterval struct default moved in voxi::Settings, rtPixelsPerRayTile comment disagreed with Voxi.hpp field (function said 4, field said 1). Static asserts added to catch this at build time.

## modules/render.voxi/include/aver/voxi/Voxi.hpp

### globalIllumination field
- Measured Release ElectricDreams 1600x900 --no-vsync --frames 200: 11.86 ms with --no-rt, 18.44 ms at Medium (55% more, cheapest honest way to have ray-traced sun shadows). Naive version (flip tier, leave knobs at Epic's 4 rays) = 23.39 ms (~2x Off). One GPU RX 7800 XT, one window size -- relative ladder holds elsewhere, absolute ms won't.
- Benchmark trap (fallen into twice): --project is POSITIONAL not a flag, and CREATEDWITH mismatch raises blocking modal. Both give clean empty-editor "benchmark" with plausible numbers and no error. Check log for "scene walk ... over 0 entities" before trusting numbers; honest baseline is 14 entities ~18 ms.
- GI runaway trap (cost an afternoon to bisect): GI gather has no clamp, so bright light on large saturated surfaces floods frame (three 1.8 m pure-red spheres under 100,000-lux sun did this; RT merely exposed it by lighting them brighter than voxel-cone path; scaling spheres down fixed it). Scene defect, but red screen instead of clamp is tracked renderer bug.

### giCones field
- THE GI SETTING THAT ACTUALLY COSTS: volume BUILD (voxelResolution/giUpdateInterval) measures 0.4-0.5 ms; per-pixel GATHER measures 1.3 ms and was hardcoded to six regardless of tier, so turning GI down bought nothing and raising to High made frame SLOWER with no way to spend budget (6.0 -> 6.4 ms, bigger volume, same sample count). Cost is LINEAR ~0.22 ms/cone (24-step march bound unreachable at diffuse aperture; cones exit early), so this is the one GI number worth laddering.
- Measured FirstPerson: Low 3.3 ms - Epic 5.4 ms, 0.21 ms/cone (confirms LINEAR figure). "Two cones moved probe by 2/255" claim elsewhere predates this ladder and was never re-measured.

### giSkyOcclusionRays field
- 0 = estimate from cone gather. Optimistic in enclosed geometry (widened cones see through thin walls: Sponza shadowed pixels read [25,26,30] vs path-traced [7,7,7], blue-biased by leaked sky).
- Sweep (Sponza, "Voxi ray-driven primary"): 0 rays 10.92 ms/[20,20,22], 1 ray 12.25 ms/[11,11,13], 4 rays 15.58 ms/[11,11,13] -- first ray buys correction, more buy nothing. Medium at default half rate: +0.5 ms, frame mean 33.4 -> 16.1 (Epic 16.2).

### giSkyOcclusionTile field
- WHY: ray is cosine-distributed so neighbouring lanes descend unrelated BVH nodes and wave runs at unluckiest lane's speed. Sharing azimuth across tile makes lanes trace near-PARALLEL rays touching same nodes/cache lines.
- MEASURED (Sponza, marginal cost 1->4 rays): tile 1 +3.56 ms, tile 2 +2.85 ms, tile 4 +2.21 ms -- 38% cut funding extra samples.
- PRICE: CORRELATED noise inside tile rather than independent per-pixel -- right for low-frequency AO, wrong for anything sharp. Do not reuse for shadows/reflections.
- Was compile-time-only, unsweepable. Rides gAmbientParams.y.

### giRadianceCeiling field
- 16.0 is not headroom: acesTonemap (rhi/shaders/color.hlsli) is flat white by x=4-5, so anything pinned here paints solid white.
- Shared by raw ReSTIR GI estimate, its denoised readback and cone-gather estimator.
- Lowering can fix poisoned-but-finite white patch, but dims legitimate bright bounce near 16 (indistinguishable alone). voxi.giPoisonView marks ceiling HIT in own colour instead (red/green ReSTIR voxi_restir.hlsli; violet specular voxi.hlsl B1/F5 not giMode-gated; magenta/cyan/yellow/orange/blue separate isnan/isinf guards).

### refractionMode field
- Off (0): background sampled straight through, pre-existing behaviour.
- ScreenSpace (1): offset by refracted view direction x ray-measured thickness; nearly free (reuses backdrop copy) but limited to what camera saw; can reach off-screen or front of glass (refractionEdgeFade hides).
- RayTraced (2): refracted ray through TLAS, hit point projected back to screen; fixes geometry at cost of ray on frame's bottleneck path.
- refractionForQuality derives this from rayTracing tier change (Off->Off, Low/Medium->ScreenSpace, High/Epic->RayTraced); = 1 here must equal Medium's rung or derivation never fires by default.

### rtShadowRays field
- Derived from rayTracing tier change: Low 1, Medium 1, High 4, Epic 8.
- Default is 1 because default TIER is Medium and derivation only fires on tier CHANGE. Struct default disagreeing with tier's rung never reached (this field once defaulted to 4 under rayTracing=Off, so switching RT on by default would silently run Epic's count under Medium's name).
- Every tier-derived field below shares this constraint; only pointed back to here from now on.

### rtPixelsPerRayTile field
- Derived from rayTracing tier change: 1 at every rung (no temporal denoising anywhere, honest default for renderer under evaluation; temporal denoiser hides its own artefacts as readily as tracer's).
- MEASURED (Release, ElectricDreams, 1600x900, --no-vsync, --frames 200, whole-frame median): rays1/tile4 18.26 ms, rays1/tile2 18.49 ms, rays1/tile1 18.44 ms, rays2/tile1 19.99 ms, rays4/tile1 23.39 ms vs 11.86 ms --no-rt. Three tile widths at one ray span 0.23 ms (noise); one ray to four costs 4.95 ms -- amortisation saturates immediately, ray count where money is.
- LOW WAS 4 (widest amortisation still-camera table justified), but still camera can't see temporal amortisation spends under motion: shadows visibly trail caster.
- Wobbling-camera diff: tile1 vs tile4 differs 0.14 ms moving / 0.09 static (noise), while trail is 0.80% pixels over threshold at tile1 vs tile2 alone (tile2->tile4 adds 0.04% more) -- fully present by tile2, no partial-credit rung; Low was one tier that shipped trail.
- Low is 1 now for same reason Medium is: frame time was never real. giUpdateInterval/voxelResolution keep Low's wider rungs since those cost real measured time under motion with no visible artifact.

### giUpdateInterval field
- Derived from globalIllumination tier change: Low 4, Medium 2, High 1, Epic 1. Default 2 since default tier Medium. Set explicitly to override derived value.
- MEDIUM MOVED 4 -> 1 (4 makes lighting trail camera) -> 2. Earlier revision claimed interval 1 left 187.9 ms on table against 104.5 ms at interval 4 on still camera; re-measured intervals 1/2/4/8 gave 18.54/18.47/18.50/18.46 ms -- noise on STILL camera, so old figure refuted (quoted rather than deleted).
- Under WOBBLING camera "Voxi GI update" span alone went 36.00 ms at interval 1 to 16.57 ms at interval 4 (aver-gi-update-dominates-under-motion.md; Sponza, --cam-wobble 15 50, ray-driven + AverSR Balanced) -- real motion cost, though that run also used RT-shadow tile 4 and beginShadowHistory sits inside measured span (VoxiRenderer.cpp), so crediting whole gap to this field is UNCONFIRMED and lag AT interval 4 itself UNMEASURED.
- LOW IS NOW 4 (not 8) on motion evidence; 8 is UNMEASURED either way.
- MEDIUM IS NOW 2, UNMEASURED FOR BOTH COST AND LAG AT THIS RUNG: at interval 2 volume rebuilds every other frame so lag at most one frame, and still scene converges identically to interval 1 either way (setSettings' own derivation comment, Voxi.cpp) -- narrows rather than reverses 4->1 move (4 still trails visibly), asking whether Medium (tier most projects run) should pay interval 1's cost for unshown lag.

### giMode field
- 0 = cone gather (DEFAULT, every build before field existed). 1 = ReSTIR GI: one traced candidate per pixel, reused via Aver's own spatio-temporal resampling (giSpatioTemporalReuse in voxi_restir.hlsli, GiReservoir in voxi_reservoir.hlsli; see giRestirIndirect in voxi.hlsl, written in-house from published papers). One fused pass: fresh candidate, one temporal tap and up to 8 spatial taps (reuse.numSamples), all read from last frame's reservoir slice.
- NOT ON QUALITY LADDER: this SWITCHES estimators (deterministic clipmap march vs stochastic ray + temporal reuse; far less per-frame tracing noise at cost of biased, history-dependent estimate that can lag moving light or disoccluding camera), not a Low-to-Epic rung.
- setSettings only clamps/range-checks it (Voxi.cpp), never derives it.
- DEFAULT IS 0 AND MUST STAY 0: VoxiRenderer::giRestirWanted() gates actual switch (requires RT hardware AND rayTracing tier AND globalIllumination tier on; with no RT project never allocates reservoir buffer or previous-surface history this needs).
- THE SHADER READS THE EFFECTIVE VALUE, NOT THIS RAW FIELD: earlier revision said shader call sites "branch on this value directly" which is stale. giMode_ is read only by giRestirWanted() (VoxiRenderer.hpp) which resets gGiRestirParams.x to 0 every frame and gates whether set to 1 (constant PSMainVoxi/PSRayDriven actually branch on) this frame (VoxiRenderer.cpp). Value device/tier can't honour never seen by shader, why stored exactly as requested rather than clamped -- RenderSettingsResolver.hpp's resolve() computes effective value UI/console show. Unlike voxelResolution/giCones, this default is "byte-identical to every image before ReSTIR GI existed" independent of globalIllumination's tier.

### giRestirVisibility field
- NoRay restores cb4b48df's pre-fix over-brightness.
- Reconstructed replaces both F2/F3 rays with one voxel-cone march diffuse gather already pays for (no extra ray).
- HalfResolution traces exact visibility on 1-in-4 pixels/frame, reconstructing rest from depth/normal-aware neighbourhood (falls back to tracing when invalid, worst case = Full's cost).
- Full traces every pixel every frame (today's behaviour).
- Cached (4, radiance cache stage 1, docs/rendering/NEURAC.md) traces exactly HalfResolution pixels, but those pixels also TRAIN camera-centred world-space SH cache with their F2 ray, untraced pixels READ that cache instead of sky-ratio reconstruction. STAGED ray-driven, D3D12-only feature (rtRenderMode 1 with rayDrivenStages >= 1): single-pass ray-driven, raster path and Vulkan never compile cache code, so there Cached behaves exactly as HalfResolution (soft DisableReason::RequiresStagedRayDriven says so in UI). On GPU wire mode stays 2 plus bit 128 of gAmbientParams.w (GiVisibility.hpp) -- shader's 2-bit mode field cannot hold 4.
- See ladder::giRestirVisibility (QualityLadder.hpp) for per-rung reasoning.
- Derived from globalIllumination tier change like giCones/voxelResolution/giUpdateInterval; default 2 (HalfResolution) since default tier Medium.
- Composes with, not replaced by, legacy bits: voxi.legacyRestirHitSky/legacyRestirReuseVisibility (console-only, never persisted) force NoRay for their OWN ray regardless of this field -- set legacy bit always wins for that ray, no ownership collision.
- Stored exactly as requested, resolved at read time (like giMode); Resolution::giRestirVisibility.effective deliberately EQUALS requested always -- see that field's comment for why usual resolve-to-clamped rule would be wrong.

### giRestirSpatialSamples field
- OPEN: ReSTIR GI reads brighter while moving, settling darker over ~1s after stopping. Ruled out: auto-exposure, denoiser (the one in use then), sky-occlusion rays, F2 voxel bounce, voxel rebuild rate, Half vs Full visibility, spatial-reuse motion discount (3dbc9a42 reverted 8daed7f1), reservoir age and moving-camera history cap (both measured WORSE). What removes fade: giRestirMaxHistory 0, and voxi.debugResetHistoryEveryFrame 1 (c08c76d2) which clears reservoir history every frame -- disabling BOTH temporal and spatial reuse at once (spatial neighbours read from same reservoir slice temporal tap reads). So carrier is reuse itself; open question: which half, and whether reuse tolerances (giIsSimilarSurface, literals 0.1 relative depth / 0.5 normal cos in voxi_restir.hlsli) simply too loose.
- 15 = AUTO (today's motion-discount numSamples, unchanged -- byte-identical image, fade included).
- 0 disables spatial reuse outright (temporal only -- isolates whether fade is spatial).
- 1..8 pin tap count regardless of motion, overriding discount's lerp(2.0,1.0,motionT); clamped to 8 ceiling fused pass stability-tested against.
- Packed at gAmbientParams.w bits 12-15 (four bits, not three, since 15 must be value no real 0..8 count collides with) -- see givis::packAmbientW (GiVisibility.hpp), shared byte-for-byte with voxi_restir.hlsli's own pack/decode.
- Debug/tuning only like two thresholds below: no manifest key, no Settings UI -- bisection tool, not shipped dial.

### giRestirMaxHistory field
- MEASURED headless on Sponza (camera translating, stopped at known frame), viewport mean luminance +3 frames vs settled: baseline 0.0965 -> 0.0892 (+8.2% too bright, gone by ~+25 frames); tightened reuse tolerances identical (neighbour test innocent); spatial reuse off still +6.9% (not carrier); moving-age cap made it WORSE (+24%). Decisive: reuse off entirely sits at 0.0889 SETTLED value -- partially-converged reservoir reads brighter than both no-reuse and converged estimates: weighting error while M small, not stale radiance. maxHistory is knob over that weighting (former bias-correction mode knob no longer exists; giFinalizeWeight's MIS normalisation fixed).
- maxHistory: reuse.maxHistory cap on M neighbour reservoir carries into combine. 1 was old value (602d1b06 lowered from 8 to kill load-time overshoot).
- DEFAULT 0 IS CAMERA-MOTION FADE FIX: 1 (old default) overshoots +8% decays over ~25 frames (fade); 8 overshoots +104%; 0 does not overshoot. Everything else innocent: spatial half, reuse tolerances, (since removed) bias-correction mode, Jacobian, reservoir age (capping made WORSE +24%, +62% while moving only) -- every restart re-forms chain from single-sample reservoirs whose RIS weight has huge variance which flashes. WHAT 0 COSTS: nothing detectable -- settled brightness unchanged (0.0893 vs 0.0892), grain/flicker at rest identical (0.00597/0.00057 vs 0.00594/0.00056), mid-motion slightly better, moving image sits at settled brightness instead 5% above: denoiser (the one in use when measured) already supplies smoothing this reuse meant to provide. 1 restores old behaviour for A/B.

### denoiser field
- AMD FidelityFX Denoiser (MIT) through Aver.Render.Denoise.
- Off by default: ON is real cost user chooses, not one denoiser helps itself to.
- Needs thin G-buffer written (velocity, view Z, normal/roughness -- three more targets, ~54 MB at 1080p) nothing else engine turns on; field is that agreement.
- MSAA: works since 2026-10-05; the D3D12 backend resolves its multisampled G-buffer (nearest sample) before the denoiser reads it. VoxiRenderer gates on IDevice::gBufferWritten() and warns once only when a backend cannot write it.
- D3D12 only because G-buffer it reads is.

### denoiserMaxSamples field
- History length: cap on each pixel's accumulated sample count. Longer smoother slower to follow change. [1, 255]; 32 is FidelityFX's own reference value.

### denoiserHistoryClipWeight field
- How tightly reprojected history clipped to this frame's neighbourhood statistics before blended: smaller rejects stale history sooner (less ghosting, more noise), larger keeps more. (0, 4]; 0.5 is FidelityFX's own reference value.

### denoiserSunMovingSamples field
- Caps history length every frame sun moves and one after. Under the earlier denoiser at full history ReSTIR GI kept old sun's bounce light ~1s after drag stopped (MEASURED NewSponza, 40-deg azimuth drag @1deg/frame vs settled: mean +3.6 on 14.6 one frame after, +1.1@26 frames, denoiser off +0.25), so history restarts short under new sun regrows by one sample frame -- drag stays denoised just less smoothly. Value >= denoiserMaxSamples turns off. Not re-measured with this denoiser.

### rtShadowDenoise field
- SPATIAL denoise radius for ray-traced sun shadow, pixels. 0 = off (unfiltered per-pixel rays); N>0 averages (2N+1)^2 neighbourhood shadow history, weighted by depth agreement with surface plane. Default 2.
- NOT THE SAME AS rtPixelsPerRayTile: that amortises over TIME (reprojected value frames ago -- converges still falls apart moving; soft penumbra collapses flat fully-shadowed under ~1deg yaw 40 frames). This averages over SPACE with no history so nothing stale/poisoned by motion. Independent combinable but fail differently.
- THE PROBLEM IT IS FOR: at one ray/pixel (Low, Medium) shadow term hard 0 or 1 -- dithered not soft. Probe converged answer 34,36,40: one ray reads 61,59,59 forever (pure function pixel); 16 rays reach 34,36,40 but cost 30.55 ms vs 18.66. Averaging neighbours cheaper: rtShadow jitters ray ORIGIN across pixel footprint so neighbours already sample different parts same receiver mean real area estimate.
- WHY 0 WAS ONCE DEFAULT: while rasterisation (PSMainVoxi, 4x MSAA) was default primary-visibility path, smoothing already-antialiased image redundant polish not fix.
- RAY-DRIVEN PRIMARY VISIBILITY (rtRenderMode) CHANGED TRADE AT MEDIUM+ -- Low stays exception (D3; ladder::rtRenderMode) still rasterising 4x MSAA -- Low keeps one shadow ray/pixel same count Medium just fired from PSMainVoxi rather ray-driven shader so MSAA still resolves dithering; only primary-visibility method changes at Low. Ray pass itself no per-triangle coverage so at Medium/High/Epic runs single-sample nothing softening hard 0/1 anymore, this filter (+0.02 ms widest rung vs +1.69 ms one more traced ray, VoxiRenderer.cpp) fixes within one code sixteen-ray reference on still camera. ACCEPTED IN EXCHANGE: gather centre reprojects through LAST frame's camera stay aligned shadow history (rtShadowSpatial, VoxiShaders.hpp) so under motion can walk off true surface -- six-degree wobble measured 12-32 codes extra darkening growing with radius. Bounded (unlike old rtPixelsPerRayTile Low=4's unbounded "collapses flat") but invisible benchmark never pans -- already burned twice that blind spot (giUpdateInterval's lag, that old Low=4 rung). Any future change needs MOVING-camera probe.
- Derived from rayTracing tier change: Low 2, Medium 2, High 1 (was 2), Epic 1 (ladder::rtShadowDenoise, QualityLadder.hpp). Default 2 since default tier Medium -- trap file already fallen into both directions with giUpdateInterval.

### rtRenderMode field
- WHICH THING FINDS FIRST SURFACE: 0 = rasteriser (every version before setting existed), 1 = primary ray per pixel. Everything downstream unchanged -- PSMainVoxi already traces shadow evaluates material traces reflection in ONE invocation (VoxiShaders.hpp:781-826) so swaps out one stage still fixed-function.
- MEASURED BEFORE IT WAS BUILT: ElectricDreams, 4x MSAA, 2750x1639, Release -- raster primary visibility + shading 9.2 ms of `scene draw` RT/GI off, one extra shadow ray 1.6 ms same resolution; primary ray had fit inside gap worth having.
- 1 FROM MEDIUM UP; LOW RASTERISES (D3, retuned) -- BY EXPLICIT PRODUCT DECISION ("Wavefront Primary rays model") not leftover experiment. Evidence for Low partial: overview cameras raster slower (ElectricDreams 20.55 vs 8.05 ms; PTTest w/ Path Tracing off 13.51 vs 8.04 ms); close-up case once favouring raster (9.02 vs 24.04 ms) is UNCONFIRMED -- run also had Path Tracing on silently taking over. D3 stands regardless decision made with this evidence not claim raster faster at Low. See ladder::rtRenderMode/rtRenderModeForQuality (QualityLadder.hpp) why Off and Low both answer 0 different reasons: Off because no RT hardware path assume, Low because product decision deliberately excludes.
- WHAT DEFAULTING TO IT TRADES AWAY: HARDWARE EARLY-Z rasterisation discards occluded fragment before shader runs free. Ray pays full BVH traversal discover same hit hidden every pixel every frame. MSAA: ray pass one fullscreen triangle -- no per-triangle coverage so always runs single-sample vs raster path's 4x default; visibly noisier independent RT sun-shadow speckle documented elsewhere. TEXTURE: primary ray returns flat albedo per instance; rasterised frame samples one.

### rayDrivenStages field
- PSRayDriven still ONE fullscreen pixel shader tracing primary ray reconstructing surface running sun-shadow ray ReSTIR GI reflections sky occlusion shading single invocation. Field picks WHICH SHAPE work runs in -- 0/1 compute same thing; 2 deliberately changes image.
- 0 = SINGLE PASS (baseline fallback): one drawFullscreen.
- 1 = STAGED: split into visibility compute pass (traces primary ray writes per-pixel record), shadow compute pass (reconstructs surface runs sun-shadow ray) then PSRayDriven reading both. D3D12 ONLY this milestone -- falls back single pass logs once pipeline fails compile resources absent non-textured PSO use backend isn't D3D12 so opted-in project never silently renders nothing.
- 2 = STAGED + HALF-RATE GI (milestone 4): same staged path 1 but ReSTIR GI traces only HALF pixels/frame (checkerboard) denoiser reconstructs untraced half -- trades GI quality/latency speed. Only differs 1 while giMode==1 AND denoiser actually denoising (voxel cone gather no GI stage checkerboard; without denoiser nothing fills untraced half) so 2 behaves 1 either case (logged once). Same restriction/fallback 1.
- DEFAULT 2 since 2026-09-27 (was 0). MEASURED NewSponza (RX 7800 XT, 3532x1987 capture, whole-frame GPU ms): gallery single 23.5 / staged 14.1 / half-rate 12.7; court 28.4 / 15.6 / 14.2. Image: staged vs single MAD 0.16-0.17 (same image); half-rate vs staged fixed exposure MAD 0.22-0.24 still (-0.4%), 0.65-0.84 motion (noise not bias). Project's RENDER.RDSTAGES still wins; only projects without key take default.
- NOT TIER-DERIVED like giRestirMaxHistory below. Meaningful only while rtRenderMode resolves primary rays (Resolution::rayDrivenStages).

### rayDrivenShadowTiles field
- SUB-STAGE SPLIT A: sun-shadow trace two passes. MEASURED (staged mode 1, Epic): shadow stage costs 4.47 ms 19.6 ms frame tracing rtShadowRays (8 at Epic) per non-sky pixel -- most frame fully lit/blocked where all rays agree. CSRdShadowProbe traces ONE ray per 8x8 tile first; CSRdShadow ORs tile's 3x3 neighbourhood skips per-pixel rays wherever every probe agrees. NEAR-IDENTICAL IMAGE not quality trade (unlike rayDrivenStages==2): uniform region's filter sees one ray's noise instead eight's.

### rayDrivenGiSplit field
- SUB-STAGE SPLIT B: CSRdGi's candidate trace two passes. MEASURED: GI stage costs 5.38 ms (4.43 ms already half-rate via rayDrivenStages==2's checkerboard) same 19.6 ms frame; checkerboard still dispatches every lane (half return immediately, wave never compacted). CSRdGiTrace carries candidate trace (giTraceInitialCandidate + material eval) own pass COMPACTED dispatch checkerboard mode; CSRdGi resamples/shades stored candidate. SAME IMAGE rayDrivenStages==1 -- changes which pass traces ray not estimator so saving occupancy/compaction not quality.

### rayDrivenReflSplit field
- SUB-STAGE SPLIT C: CSRdRefl's register-heavy ray plus bandwidth-heavy spatial history gather (rtReflectionSpatial up to 7x7 depth-tested gather last frame's history) two passes. MEASURED: reflection stage costs 3.65 ms same frame other splits measure -- one thread pays reflection ray nested sun-shadow ray full material shade AND dense spatial gather shape made splitting shade megakernel pay off (34 -> 1.8 ms) originally. CSRdRefl compiled second time (AVER_RD_REFL_SPLIT=1) traces ray writes PENDING marker instead composing; CSRdReflFilter runs rtReflectionSpatial alone finishes compose. SAME IMAGE unsplit -- round-trips same RGBA16F history texture not quality trade.

### localLights field
- Material with lightIntensity > 0 turns every draw using it small sphere light (draw's bounding sphere tinted emissive colour) lit through sun's BRDF (diffuse specular lobe widened lamp's angular size) one stochastic shadow ray one lamp per pixel visibility accumulated sun shadow's own reprojection.
- lightIntensity MULTIPLIES material's glow/size already cast sun units (SkyAtmosphere::sunIntensity) -- 1 = that 2 = double.
- At most 32 lamps/frame (brightest-and-nearest lit output / squared distance).
- Works every D3D12 scene mode (raster megakernel staged).
- Translucent draws unshadowed except staged replay proves sit lit decal surface borrowing visibility.
- No lamps = 0 count (skipped branch staged pass not dispatched); off frees both history textures.

### rtSecondaryShadowOpaque field (T1)
- T1 (bit 1): sun-shadow ray fired FROM SECONDARY HIT (rtReflection's hit ReSTIR GI's candidate hit) normally walks rtShadow's full transmittance loop (up to 8 steps RAY_FLAG_NONE AVER_RT_MASK_ALL) tint light through glass. ON: both fire ONE ray instead (RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH opaque+cutouts mask). TRADE: translucent instances stop casting shadow these two rays reading fully lit through glass. Primary shadows (camera cascades shadow probe) untouched.
- MEASURED alone: GI trace 3.88 -> 3.38 ms reflection 3.14 -> 2.73 ms; still image MAD 0.09.

### rtSkyOcclusionHalfRate field (T2)
- T2 (bit 2): rtSkyOcclusionTemporal skips rtAmbientTraced entire 8x8 TILE this frame's skip parity wherever tile's reprojected history valid -- temporal blend keeps reprojection estimate still written history spatially filtered. Whole tiles skip together (whole compute wave) not per-pixel checkerboard (leaves every wave half occupied saves nothing -- same lesson half-rate GI learned own compaction). Pixel no valid history always traces.
- MEASURED: sky occlusion costs 0.73 ms staged mode 1 15.4 ms frame. MEASURED alone: 0.72 -> 0.47 ms; still image MAD 0.40.

### rtReflectionHalfRate field (T3)
- T3 (bit 4): rtReflectionTemporalEx skips trace ROUGH pixel (lobeRough > 0 -- mirrors always retrace since reprojected mirror reflection visibly wrong instant camera moves) skip-parity tile whose reflection history reprojects validly reusing as this frame's colour (same existing tiled "not my turn" branch).
- MEASURED: reflection trace costs 3.11 ms (+0.39 ms filter) staged mode 1 15.4 ms frame. MEASURED alone: reflection trace 3.14 -> 2.17 ms; still image MAD 0.04.

### rtGiHitShadowMap field (T4)
- T4 (bit 8): sun visibility ReSTIR GI's candidate HIT (giTraceInitialCandidate) comes GI-only shadow map (same box GI volume's light injection samples via giShadowFactor) instead shadow ray; ray still fires map can't answer (outside box or unusable this frame).
- PROTOTYPE MEASURE NewSponza staged mode 1: GI trace 3.38 -> 2.68 ms (mode 2: 1.65 -> 1.28 ms); still image MAD 1.61 +1.2 brighter -- 19 cm map texels let little bounce light through under column capitals/bases ray blocks (smaller normal offset didn't change). RE-MEASURED 2026-09-27 (NewSponza whole frame): gallery 11.03 -> 10.41 ms court -0.57 ms; MAD 0.18 still / 0.34 moving -- 1.61 no longer reproduces so ON default.

### blendedReuseStagedLighting field (BIT 16)
- BIT 16: REUSE STAGED RAY-DRIVEN LIGHTING FOR TRANSLUCENT DRAW ON SAME SURFACE.
- Rides same row T1-T4 but shaped differently (see toggle-block header).
- Trades: translucent pixel over opaque surface staged passes already lit THIS frame -- e.g. NewSponza's floor dirt decal (BLEND-translucent alpha-0.35 layer fraction cm above floor) -- would otherwise have PSMainVoxi's translucent branch re-light from scratch (own sun-shadow ReSTIR GI candidate + shadow sky-occlusion rough surfaces reflection ray) instead reading ray-driven passes already computed pixel earlier frame.
- ON reads gRdSunVisTex/gRdGiTex/gRdAoTex/gRdReflTex instead re-tracing gated PER PIXEL gRdSunVisTex.a's stored depth agreeing this pixel's own -- i.e. surface actually sits one staged passes lit not merely near screen space. Excluded regardless: draw material reads captured backdrop instead (glass water -- attenuationDistance or material graph; IRenderFeature::blendedDrawReadsBackdrop) wants own lighting.
- ON BY DEFAULT. MEASURED (NewSponza staged mode 1 two floor-decal draws): blended replay 0.39 -> 0.17 ms saved camera 0.18 -> 0.14 ms standard view still image diff 0.01/0.04 vs off. Quad-uniform shader so 2x2 quad straddling decal edge traces whole.

### rtSkipUnchangedTlas field
- MEASURED owner's static NewSponza scene: "Voxi acceleration structures" GPU span costs 0.42 ms every frame -- from-scratch ctx.buildTlas (PREFER_FAST_TRACE) plus unconditional instance-buffer rewrite/upload recomputing identical answer unmoved scene; skipping whole thing outright cheaper than even refit (Settings::rtRefitAccel) why gate exists separate setting rather subsumed by that one.
- Same trick GI rebuild gate (no Settings field; giUpdateInterval only amortises): hash what buildAccelerationStructures() reads draw list if nothing moved leave tlas_/rtInstanceData_/bound SRVs exactly are.
- See VoxiRenderer::rtAccelSnapshotUnchanged() what "unchanged" checks and one thing still forces real rebuild regardless (cached BLAS handle resource factory no longer attributes mesh) -- compute-skinned mesh present forces too but only while Settings::rtRefitAccel below off; on gate instead runs lighter refit-only pass (VoxiRenderer::refitDynamicAccelStructures) rather plain skip.
- MOVER PATCH LANE (needs this and rtRefitAccel both on): draw flagged Draw::movable (Play's animated props pawn) world matrix left OUT key so moving alone no longer rejects gate. Hit then runs VoxiRenderer::patchRtMovers() writes each mover's current transform TLAS instance list ray-hit instance table TLAS refit (same periodic full rebuild rtRefitAccel). Movable bit stays key so draw starts/stops moving any change mesh material flags still forces full build.
- With either setting off key hashes every world before. Report line counts these ticks "mover-patched".
- ON BY DEFAULT: only ever skips work output bit-identical -- not quality trade -- so turning off costs frame time buys nothing measurable.

### rtRefitAccel field
- ON: tlas_ and each compute-skinned mesh's BLAS created updatable refit in place instead fully rebuilt; skinned mesh alone no longer forces whole per-draw loop every frame (gate above runs refit-only pass instead plain skip).
- Periodic full rebuilds every kDynamicBlasRefitsPerRebuild / kTlasRefitsPerRebuild refits (VoxiRenderer.hpp) since refit traces worse pose drifts.
- OFF: old behaviour exactly (full builds no ALLOW_UPDATE).
- UNMEASURED (engine runs off when landed 2026-09-27). ALLOW_UPDATE allocation LATCHED structure created (like layeredBsdf); toggling live only changes whether refit attempted.

### ptBounces field
- WHERE RAY TRACING ENDS AND PATH TRACING BEGINS: RAY TRACING discrete rays answering one question (shadowed? what mirror see? what surface this pixel?) -- one hit direct lighting; PATH TRACING multi-bounce solve. Separate settings (rayTracing/pathTracing above) because separately useful priced supported.
- Renamed `rtBounces` (derived rayTracing tier letting `pathTracing = Off` projects run path tracer while setting read "off"); bounces now belong pathTracing.
- 1 = NO extra bounces (one hit = ray tracing). Above 1 path tracing; VoxiRenderer refuses spend it while pathTracing is Off regardless what's stored (enforced ptBounceParams).

### fogOcclusion field
- WHAT THIS FIXES: shared_prelude.hlsl's height fog (averFogFactor/averFogInscatter/averApplyFogEx) and aerial-perspective term add in-scattered SKY light along camera-to-surface path no regard what's between -- correct outdoors wrong indoors.
- MEASURED Sponza's arcade: fog alone adds ~6% sky radiance over 30 m corridor brighter bounce-lit walls beneath (flat blue veil not air). With fog off ReSTIR GI already matches path-traced reference within 8% -- fog's own error.
- ON BY DEFAULT. Off exactly today's fog byte byte (VoxiRenderer binds 1x1x1 placeholder t17/u16; voxiAirVisibility() returns 1 unconditionally sees one) -- turning off only reverts pre-existing over-bright indoor fog never downgrade vs earlier build.
- WHY NOT SURFACE AO (reverted -- aver-fog-skyvis-failed.md): fog property camera-to-SURFACE PATH not surface's hemisphere AO's accumulated history flashes white disocclusion (fast pan resets before reconverging) -- unaffordable still capture. Instead: WORLD-SPACE volume no per-pixel history or jitter -- CSAirVis marches fixed hemisphere directions through SAME voxel grid GI cone gather reads so motion can't flash it.
- COST: one more compute pass CSAirVis FIXED 32^3 volume (VoxiRenderer::kAirVisResolution) independent Settings::voxelResolution (small fixed grid suffices path occlusion unlike GI radiance volume). Refreshes one slab z-layers per frame round-robin (full 48^3 pass measured ~10 ms; sky visibility only changes geometry); shade-side read eight fixed texture taps down existing fog ray no extra ray own.
- DEVICE-GATED NOT JUST SETTING-GATED: needs SM 6.0 and DXC (VoxiRenderer::airVisWanted()); without either placeholder stays bound this setting no effect -- same fallback shape Settings::rayDrivenStages staged compute pipelines.

### Renderer class methods
- refusalLogged: returns whether feature's refusal already logged setSettings' refuse() lambda since last setDeviceInfo call -- lets load-time contradiction report (RenderSettingsResolver.hpp's manifestContradictions) skip re-warning device limitation already logged instead saying twice two call sites.
- voxelResolutionForQuality: setSettings derives voxelResolution tier change only when caller left it untouched.
- giConesForQuality: total cones diffuse gather. See Settings::giCones.
- giRestirVisibilityForQuality: how much F2/F3's cost each GI tier pays.
- refractionForQuality: derived rayTracing tier change (Off->Off Low/Medium->ScreenSpace High/Epic->RayTraced).
- giUpdateIntervalForQuality: same "only untouched" derivation rule grid edge above; Epic 1 -- always fresh unchanged.
- rtShadowRaysForQuality: RT sun-shadow rungs mirroring giUpdateIntervalForQuality applied setSettings rayTracing tier changes field arrives unchanged.
- giSkyOcclusionRaysForQuality: sky-visibility rays per pixel ray-tracing tier. High and Epic not Epic alone.
- giSkyOcclusionTileForQuality: sky-occlusion ray coherence tile ray-tracing tier.
- rtRenderModeForQuality: 1 every RT-capable tier except Low rasterises explicit product decision (D3) -- 0 Off/Low 1 Medium/High/Epic. See ladder::rtRenderMode why Off and Low share value different reasons.
- ptBouncesForQuality: derived PATH TRACING tier not ray-tracing one.

## modules/render.voxi/include/aver/voxi/VoxiRenderer.hpp

### Gate info: Passed OK (569 -> 76 comment lines)

### Removed knowledge kept here:

####### Class overview
- Voxi is a GPU render feature managing voxel cone-traced global illumination, cascaded directional shadow map, and DXR 1.1 inline RayQuery sun shadows
- Owned resources: GI volume, shadow atlas, acceleration structures (BLAS/TLAS), lit pipelines
- Aver.Render.Voxi (settings DLL) must not gain RHI dependency; RHI use lives here

####### Foliage system
- Up to millions of instances of few prototypes, loaded from .ocinst files
- TRACED (primary visibility, shadow, reflections, GI) but never rasterised, voxelised, or path-traced
- kMaxFoliageInstances = 8M bounded by TLAS capacity (rhi::kMaxTlasInstances, prefix + draws) and static prefix's 64 B/instance descriptor buffer (~512 MiB)

####### GI cache and settings
- GI cache directory is project-relative "<project>\\DerivedDataCache\\GI"; empty disables cache entirely
- setGiCacheDir is a setter, not project lookup; same reason setVolume is a setter — render.voxi knows nothing about projects/manifests

####### Ray tracing settings
- setPixelsPerRayTile: rounds to nearest power of 2 in [1, kMaxPixelsPerRayTile]; 1 (default) traces every pixel every frame, bit-identical to having no denoiser
- setGiUpdateInterval: 1 (default) rebuilds every frame, bit-identical to original always-fresh behaviour; N>1 reuses previous volume N-1 frames while cone trace still runs every frame
- Settings::rtSkipUnchangedTlas gate: skips rebuild that would be bit-identical; measured 0.42 ms/frame on owner's static NewSponza scene — buys from-scratch ctx.buildTlas + unconditional instance-buffer rewrite/upload every frame regardless of motion

####### Measurement/optimisation dials (M4/W3/W12)
- M4: forces every GI tick past snapshot gate, bypasses on-disk GI cache
- W3: bounds dispatch to draw list's changed box instead of whole grid
- W12: frees injection accumulator after kGiAccumulatorQuietTicks quiet ticks; allocator hoisted from createVoxelVolume for separate recreation without descriptor/logging duplication
- W10: buildAccelerationStructures' per-build scratch (tlasInstScratch_, matConstantsScratch_) hoisted to avoid frame reallocation; .clear()'d at build start (keeps storage)

####### BLAS/TLAS management and acceleration structures
- Translucent instances tracked in rtTlasTranslucent_ (set by refitOrRebuildTlas); Zero turns on gGiShadowParams.w bit 32 in prePass for first-hit fast path in rtShadowEx
- One BLAS per mesh: compute-written meshes (IDevice::meshVertexBuffer gated on GpuMesh::computeWritten) refit/rebuild every frame; static meshes created once and never touched
- FIXED: used to gate on meshVertexBuffer returning GpuMesh::vbBuffer (true of every mesh since a3022e0), forcing needless per-frame rebuild on static geometry; now gates on computeWritten (set only by createSkinTargetMesh, cleared by destroyMesh)
- Dynamic BLAS refits periodically forced rebuild: kDynamicBlasRefitsPerRebuild = 30 ticks (refit traces worse further pose drifts from build it refit from)
- TLAS periodic rebuild: kTlasRefitsPerRebuild = 30 ticks

####### THE UNCHANGED GATE (rtAccelSnapshotUnchanged)
- Same trick as GI rebuild gate: hash what per-draw loop reads from drawsPrev_; if nothing moved, leave tlas_/rtInstanceData_ alone
- Movable draw's transform not hashed, patched separately (THE MOVER PATCH LANE)
- rtAccelMustForceRebuild() checks: compute-skinned mesh present, or cached BLAS resource factory no longer attributes to its mesh
- rtAccelListKey_ remembered from gate compute to avoid recomputing in takeRtAccelSnapshot() — at ~42,000 draws, second pass is most of a millisecond for nothing
- rtAccelMeshChecked_: direct-mapped filter 16384 slots hold ~3000 distinct meshes of large level with few collisions; zeroed per call, 0 never a submitted mesh

####### THE MOVER PATCH LANE (rtMoverPatchActive)
- Moving draw no longer forces whole per-draw loop when gate and Settings::rtRefitAccel both on
- In Play session: route-animated props, pawn, viewmodel move every frame
- rtAccelDrawsKey() used to hash every draw's world matrix — ONE moving draw rejected gate and forced full per-draw loop, material/geometry tables, TLAS pack over ~51,000 draws in NeonDistrict (ESTIMATED ~25 ms CPU/frame, UNMEASURED)
- PlayMobility marks Draw::movable draws; key leaves world matrix OUT, keeps mesh/flags/material/movable bit (see rtDrawHash)
- Every full build records per movable draw what instance it became (rtMovers_)
- Gate hit: patchRtMovers() writes each mover's CURRENT world into tlasInstScratch_ and rtInstanceData_, re-uploads instance table, caller refits TLAS (same periodic rebuild as ever)
- World feeds TlasInstance::world, RtInstance::objectToWorld, RtInstance::prevObjectToWorld (patch fills from old objectToWorld as overwritten; rtPrevPending_ settles back to equal on later frame so stopped mover stops reporting motion)
- Instance bounds, material rows, geometry slices, instance mask/flags not functions of world; key still gates them
- Recorded mover matched by IDENTITY (rtDrawHash without world), not index — key is commutative sum, so reshuffled list (occlusion culling) still matches
- patchRtMovers() breaks ties for movers with equal identity by NEAREST translation to instance's current world (not draw order, which culling reshuffles)
- Not covered: authored draw's per-draw colour/metallic/roughness not in identity; per-draw tint on mover (only show-culled debug view does that) keeps last full-build value until next one

####### RtInstance ABI
- 160 bytes total: 64 (objectToWorld) + 4 (firstIndex) + 4 (firstVertex) + 12 (albedo) + 4 (metallic) + 4 (roughness) + 4 (materialIndex) + 64 (prevObjectToWorld)
- HAND-MAINTAINED: HLSL packs structured-buffer element by POSITION not name; VoxiShaders.hpp RtInstance mirror must match exact position/name — mismatch shifts every field after and corrupts every ray hit with no compile error
- Only byte-count static_assert guards it (assert only guards struct and HLSL mirror, not stride)
- materialIndex: dense index into this frame's rtMaterials_ (t9, gRtMaterials in VoxiShaders.hpp); index 0 always fallback
- prevObjectToWorld filled by carry-forward (full build) or patchRtMovers (mover patch); foliage parts and placeholder carry own (static)

####### Ray tracing instance ring
- Instance table is RING on upload heap, rewritten every frame while GPU may still read previous copy (writeBuffer unsynchronised memcpy)
- Without it, reflection samples transform mid-motion, smearing instances between positions, only while ray tracing on
- One buffer per frame in flight breaks overlap; 3 matches PcgVolume's readback window so needn't be re-derived if device ever triple buffers

####### Ray tracing texture capacity
- rtTextureCapacity = 4096: generous against real projects (ElectricDreams resolves tens) while costing 6% of shared 65536-descriptor heap
- Exhaustion refuses and logs; does not wrap

####### Ray tracing shadows and temporal amortisation
- rtShadowRays_: occlusion rays per pixel toward sun's disc, 4 default (1 gives hard aliased edge, cost linear)
- First knob to turn down if ray tracing costs too much (recorded TDR history on this machine keeps default low)
- Ceiling — rays actually fired (rtShadowRaysUsed_) follow sun disc's size (see updateRtParamsPerFrame)
- rtPixelsPerRayTile_: tile edge for temporal amortisation (1 traces every pixel every frame)

####### Settings and compilation
- layeredBsdf_: whether coat lobe compiled. Latched at first setSettings and never again — renderer builds ~20 raster PSOs at init through DXC with no disk cache, so later change means recompiling all mid-session (project-level decision; changing needs project reload)
- layeredBsdfWarned_: logged once per process, not per frame — mismatch is standing condition for session rest, so logging every applySettings call (measured ~30 copies ordinary run) is noise

####### Frame-period sampler
- Measures wall-clock period between successive prePass calls (WHOLE frame — editor UI, voxelise pass, post chain, not just Voxi's share)
- CPU period (only tracks GPU cost while GPU-bound)
- Run with --no-vsync or every reading is refresh interval
- Exists so "cost is linear in ray count" is measurement not assertion — changing one thing and re-reading same number is measurement, profiler capture that can't be checked into repo is not

####### G-buffer twins
- Every scene-lit pipeline (eight opaque/blended combinations, two prepassed, rayDrivenPso_) gets twin with identical vertex/mesh-shader stage and fixed-function state
- Pixel shader recompiled with AVER_GBUFFER=1 (changing RETURN TYPE per VoxiShaders.hpp #if blocks: GBufferOut/RayDrivenGBufferOut), renderTargetCount 4 instead of 1
- Built in createScenePipelines()'s "Gbuf" step
- PSO's render-target count/formats fixed at creation both backends — needs second pipeline not branch inside one
- Three extra formats fixed constants (RG16F/R32Float/RGB10A2Unorm, SV_TARGET1/2/3) already fixed by IDevice::gBufferVelocityTexture()/gBufferViewZTexture()/gBufferNormalRoughnessTexture() in RHI.hpp, not derived from onRenderTargetsChanged
- Property of G-buffer feature, not swapchain/MSAA state
- Optional exactly like non-Gbuf twins (0 = couldn't build: older shader model, no DXC, compile failure — pickGbuf() falls back to plain, never losing base pipeline)
- Built unconditionally alongside plain twins, gated on same device caps (msOk/rtOk)
- Runtime on/off is pickGbuf()'s question (dev_->gBufferEnabled()), asked fresh every call — flipping switch needs no rebuild

####### Depth prepass variants
- No mesh-shader twins: prepass only offered to plain drawMesh() path (SandboxApp.cpp)
- scenePipeline() never needs prepassed sceneMsPso_/sceneMsRtPso_ variant

####### Blended pipeline twins
- Blended twins (sceneBlendedPso_, sceneMsBlendedPso_, sceneRtBlendedPso_, sceneMsRtBlendedPso_): same compiled vsMain/msMain/psVoxi/psRt binaries
- GraphicsPipelineDesc identical to opaque variant except blend = PremultipliedAlpha, depth.write = false
- Premultiplied so PSMainVoxi's blended branch returns full-strength specular + coverage-weighted diffuse instead of attenuating both by same alpha (see that branch VoxiShaders.hpp)
- Shading (shadow lookup, cone-traced indirect) same code path opaque draw gets
- Casting shadow, appearing in reflection hit, injecting radiance not — submitDraw() drops blended draw before reaches draws_ (deliberate now; before `blended` existed every draw was opaque by construction, exclusion accidental)
- All four exist (mirroring mesh-shader x ray-tracing axes) because unlike depthPrepassPso_'s twins, no single known caller to narrow set against


- `DynamicVertexSlice`: Re-copies compute-written slices every frame because they change while their key (mesh/binding) doesn't. This handles a stale-pose fix for compute-skinned geometry.

- `buildGeometryTable()`: Builds/refreshes the flat table for this frame's draw list; returns false when it could not be made, which is the signal to fall back to cone-traced reflections.

- `uploadRtInstanceTable()`: Writes rtInstanceData_ into the next ring slot (growing the ring first if needed) and binds it at t5. Shared by buildGeometryTable and the mover patch lane. Returns true when there is nothing to send (foliage-only frame) or slot is bound; false when a buffer could not be created.

- Material table ring buffer strategy: rtMaterials_ is an upload-heap ring (same as rtInstances_). Fresh index assigned every build. Exact content comparison (byte-for-byte memcmp of rtMaterialData_ vs rtMaterialUploaded_) IS the revision signal because there is no revision counter upstream to trust instead.

- Per-draw loop's material key resolution (rtInstanceMatKey_): Grows in lockstep with rtInstanceData_ during buildAccelerationStructures' per-draw loop. Second pass (buildMaterialTable) sorts distinct keys, builds rtMaterialData_ in that order, re-uploads only when content differs, writes final index into rtInstanceData_[i].materialIndex.

- `rtMaterialKey()`: One surface's key and (first time this build sees the key) its constants into matConstantsScratch_. Textures are made resident here. Shared by per-draw loop and foliage parts (resolveFoliageMaterials), so both key and resolve a material the same way.

- Foliage synchronization: BLASes are allocated by setFoliage (it has no render context) and built by the next buildAccelerationStructures just before tlas_ rebuild. t2/t20/t21 must be rewritten before any ray reads them: setTlasStaticInstances reallocates TLAS and replaces desc buffer, clearFoliage frees part table. Written early in prePass (never from setFoliage itself, which can run mid-frame; Vulkan forbids that write, D3D12 would hand this frame an unbuilt TLAS).

- Local lights ring buffer: writeBuffer is an unsynchronised memcpy into mapped memory, and the list is rewritten every frame while GPU may still read the previous frame's copy. Rotating before the write keeps this frame off the slot the last one bound. At most 32 per frame, sorted by 1-metre irradiance over max(camera distance², 1).

- Lamps inclusion strategy: True when EVERY draw whose material asks to be a light made this frame's list (none cut by 32-light cap, none without bounds). Only then may GI estimators leave a lamp-flagged hit's own emission out; a flagged lamp that missed the list would otherwise get neither direct light nor glow in GI, and go dark.

- Previous-frame transforms carry-forward: Two writers: patchRtMovers() copies current objectToWorld into prevObjectToWorld just before writing new world; full build carries last frame's drawn transforms forward by (mesh, drawBinding) group + nearest match. RT instanceId is a POSITION in this frame's replay, not a stable identity (insert/drop an earlier draw and every id after renames a different instance).

- GROUP KEY for carry-forward: (mesh, drawBinding). giDrawsKey() folds whole list into one hash, mesh alone collides on instanced props. Within a group, instances told apart by WHERE they were, not ordinal (ordinal shifts for whole group when one instance spawns/despawns, which is exactly the failure the old ordinal tracker had).

- Exact matching in carry-forward: FNV-1a hash of (group, 16 floats' raw bits). Common case (instance exactly where it was) is O(1) average; used entry is unlinked from its exact chain so chain only holds unmatched rows.

- Carry-forward distance threshold (kRtMaxCarryCm = 2000 cm): Nearest match farther than this is a different object that happens to share a mesh/material, not same one moved. Wrong motion is worse than none (the fallback, zero).

- Carry-forward linear scan limit (kRtCarryMaxScan = 64): Nearest-translation matching is linear scan over group's unmatched rows. Only runs when ≤64 unmatched rows in a group; larger groups are O(n²) so assume instances that didn't match exactly are more likely new than moved.

- Draw material memoization (DrawMaterialMemo): One binding set's answers to hashDrawMaterialInto's two MaterialSystem lookups, remembered for ONE key computation. City has ~42,000 draws over few hundred materials; each draw used to pay two unordered lookups per key, 2-3 keys per frame. LOCAL TO ONE CALL by construction; never a member because answers only valid while material system not touched, and outliving call could hide material edit.

- Mesh submit cache rationale: submit() re-resolves two mesh-only questions (depthProxyFn_ answer and meshBounds local-space) on every one of ~16,000 calls a frame, including frustum-culled entities, though scenes rarely have >few hundred distinct meshes. A MEMBER (not local) because GameRender.cpp's equivalent is emptied at drawWorld() start, but submit() runs once per entity with no enclosing call to scope it.

- Mesh submit cache size (kMeshSubmitCacheSlots = 2048): Deviation from 80730751's 64-slot precedent. GameRender's 64 slots hold camera-relevant species, but submit() sees renderer's WHOLE draw population. 256 slots measured ~86% hit rate on city level (~42,000 draws over ~2,900 distinct meshes); 2048 slots is ~74 KB on this object (zero allocation like 80730751), clearing is free via generation stamp.

- Mesh cache direct mapping: Raw mesh id is masked only after mixMeshId() re-mixes it (see that function's own comment for why this file's reason differs from GameRender.cpp's despite identical shape).

- Mesh cache eviction: Finds mesh's slot, evicting a different mesh's leftover answers first (same hazard/fix as GameRender.cpp). A hit never reads a previous occupant's fields under the new key.

- Depth proxy caching: Caches depthProxyFn_'s RAW return, not submit()'s substituted answer. submit() still runs its own "0 means no proxy" substitution against cached value, so hit reproduces uncached call bit-for-bit for same fn/user pair.

- Bounds caching detail: Caches meshBounds' LOCAL-SPACE answer only, before submit() transforms through world into instance's world-space sphere. Caching transformed result would hand every instance of a mesh the first instance's world-space bounds (correctness bug that reads like culling tuning problem).

- shadowInstanceGroups_: shadowPass() scratch -- culled draws grouped by mesh so every instance of one mesh reaches GPU in single drawMeshInstanced() call. A member (not cascade-local) so capacity survives cascade-to-cascade and frame-to-frame; clearing worlds (not erasing group) makes that stick.

- giShadowInstanceGroups_ rationale: Same grouping for GI-only pass. Own vector rather than shared scratch because giShadowPass and shadowPass run in same frame and would otherwise stamp on each other.

- shadowGroupIndex_/giShadowGroupIndex_: mesh → group's index. Groups only ever appended, never erased, so index stays valid for renderer's lifetime. Finding draw's group used to be linear walk of every group per draw per cascade: on city level (~3,500 distinct meshes, ~51,000 draws) was ~90 M comparisons per cascade before single triangle drawn.

- FrameConstants mirror invariant (CRITICAL): Mirrors `cbuffer VoxiFrame : register(b4)` field for field. voxi.hlsl and voxi_gi.hlsli repeat every field by hand with no guard. Appending without appending there reads garbage; inserting in middle shifts every field after, silently, everywhere. giRestirParams appended both places for this reason.

- cameraMedium[4] fields: x = in blended volume (1), y = ior. Needed because closed volume seen from within has no front faces (every points away from eye), so back-face discard that keeps water box from compositing four alpha coats would delete surface once you swim under it. z = light count (float), w = two bits (history valid, carries emitters).

- causticMin/causticMax: THE WATER VOLUME THAT CASTS CAUSTICS, world centimetres. min.xyz/max.xyz AABB, min.w = 1 when exists, max.w = strength. Volume's top (max.z) is surface light refracts through; everything in footprint below that height lit through it.

- giShadowParams.w bit-field toggles: bit 1 rtSecondaryShadowOpaque, bit 2 rtSkyOcclusionHalfRate, bit 4 rtReflectionHalfRate, bit 8 rtHitShadowMap. Bit 16 different in kind: staged textures hold values AND blendedReuseStagedLighting on. Bit 32 automatic: ORed by prePass right after buildAccelerationStructures when tlas_ holds no translucent (rtTlasTranslucent_ == 0).

- ambientParams[4] bit packing (z and w decoded as u32, not re-interpretation): bits 0-1 (& 3) Settings::giRestirVisibility (0 NoRay, 1 Reconstructed, 2 HalfRate, 3 Full; Cached (4) packs as 2 + bit 128), bit 4 (& 4) half-res ReSTIR visibility pair bound, bit 8 (& 8) t16 holds real previous frame, bit 16 (& 16) W6/M5 setBlendedGiCone (cone gather for blended), bit 32 (& 32) backend replays translucent THIS frame (D3D12 only), bit 64 (& 64) setGiVisPathView debug view, bit 128 (& 128) radiance cache live (neuracLive_), bits 12-15 (>>12 & 15) Settings::giRestirSpatialSamples (15 = auto), bits 18-22 (>>18 & 31) Settings::giRestirMaxHistory.

- viewParams[4]: x = mode (0 normal, 1 unlit flat albedo, 2-5 ViewDebug enum). Composed as `viewDebug_ != ViewDebug::None ? f32(viewDebug_) : (unlit_ ? 1 : 0)` so two dropdown families stay mutually exclusive. PSRayDriven decodes with `(uint)(gViewParams.x + 0.5)`; PSMainVoxi never reads it (ray hit has no per-draw cbuffer).

- giRadianceCeiling (viewParams.y): Settings::giRadianceCeiling (AVER_VOX_MAXRAD in voxi.hlsl/voxi_gi.hlsli). 0 reads as "unset", falls back to shader's 16.0 default. A repurposed bit, packing/size unchanged.

- giRestirParams.x: 1 while giMode==1 is ACTUALLY running this frame (giRestirWanted()). Not raw copy of Settings::giMode; reservoir/surface-history pair only allocated when giRestirWanted() true. Branching on raw setting would read null-filled t12/u6/u7 on device that can't run it.

- giRestirParams.y: 1 once surface-history pair holds real previous frame (own flag, not gRtHistParams.y, because giHistValid_ wrong the one frame pair freshly created while shadow/reflection pair already isn't).

- giRestirParams.z: Which of reservoir buffer's two array slices this frame writes, sharing rtHistWriteIdx_'s cadence.

- giRestirParams.w: ReSTIR-GI poison debug view (giPoisonView_/setGiPoisonView). Repurposed bit so struct size/offsets unchanged. Published unconditionally near top of beginShadowHistory.

- GI rebuild gate strategy: giSnapshotUnchanged() hashes what result depends on (same trick buildGeometryTable uses via rtGeometryKey_) and reuses volume when nothing moved -- not approximation, identical answer. Not static/dynamic split (correctness reasons) or on-disk cache. PSVoxel bakes LIGHTING (albedo * sun * visibility + sky) into each voxel, so static voxel goes stale when dynamic occluder crosses sun's path over it (invisible to static/dynamic split). On-disk cache has no readback and key includes sun (live editor slider).

- GI gate limitation: Streaming new chunks or dragging time-of-day slider changes inputs every tick (no win there). Gate gates whole pass only.

- giDrawsKey_ axis measurement: giDrawsKey_ alone says only "different" (order-independent hash expected to stop rejects under camera rotation but changed skip rate by two points). Split into independent axes: giDrawsCount_, giDrawsMeshKey_, giDrawsWorldKey_, giDrawsMatKey_ to measure, not guess.

- voxelSkyInjected(): Whether PSVoxel bakes sky into volume (negation of cb_.viewParams[2]). THE SETTING, not whether ReSTIR GI actually ran this frame. Gate and GI cache key both carry it: volume baked with sky is different answer from without.

- GI carry-forward lookup member containers: Cleared and refilled each build so steady 42-51k-draw scene reuses capacity. Exact matches go through hash of (group, 16 floats' raw bits), so common case is O(1) average.

- giBoxPrevValid_: False when box cannot be trusted: no rebuild yet, last rebuild had unbounded draw, or volume restored from on-disk cache (giCacheRestore sets false on hit). Forces NEXT rebuild's box0 to full grid.

- giBoxRes_/giBoxCentre_/giBoxExtent_: Volume resolution/centre/extent the box was recorded under. Move, resize, or rebuild at different resolution since invalidates box even though giBoxPrevValid_ still true.


- `giFreeAccumulator_`: freed because the accumulator is the single largest idle GI allocation (2048 MiB at Epic's 512^3 volume size), promoting a former measurement-only toggle to shipped default
- `giAccumRecreateBackoff` constants: ~0.5s at 60 Hz for min, ~30s for max, implemented exponential backoff doubling on each consecutive failure (capped)
- `kGiAccumulatorQuietTicks` (240 until 2026-10-04, now 60 so it is gone before a level load's BLAS wave settles): "long enough that an idle session is done lighting, short enough that scrubbing a timeline doesn't recreate every few seconds"; empirically determined tuning
- `giSnapshotUnchanged avoids ~93% of rebuilds` within a run (cache hit rate within a single level load), but remembers nothing across level loads
- `giCacheKey_` storage: keyed on whole key not just drawsKey alone -- a camera move changes centre without changing draws, would rebuild from scratch even though same volume cached minutes earlier
- `giCacheReadbackDelay = 4 frames`: longer than deepest frame-in-flight for GPU command copy
- `kGiCacheDwellTicks = 120`: ~6 ticks settling, rest dwell; prevents every pause in a sun drag queuing an ~18 MB entry for intermediate state (owner session wrote 11 files on exit, 7 intermediate drag positions)
- Write-behind buffer: without it, every completed bake was ~18 MB file write on the frame it finished; nudging sun for a minute meant tens of writes and directory sweep after each
- `giCacheRamBudget_ = 256 MiB`: ~14 entries at ~18 MiB per 128^3 volume, more than a session produces
- `giVoxelisedDraw` one definition: giDrawsKey/giDrawsSubKeys/voxelizePass used to hand-copy three predicates and drifted on volume-bounds cull
- `giSurfPosHist_` / `giSurfNrmHist_` split: RG32Float precision matters because giReconnectionJacobian's partial-Jacobian terms are distance-squared RATIOS; RGBA16F's ~11-bit mantissa relative error comparable to that ratio at few thousand centimetres scene extent
- Packed normal sentinel: 0 means "nothing written here" (sky miss or before pair existed); write side nudges exact-zero encoding to 1 (explicit sentinel rather than trusting fresh allocation to read zero)
- `giVisHist_` half-resolution: 2.10/E writes one full-resolution pixel per 2x2 block per frame, so one texel per block suffices
- Temporal denoiser history reset: denoiser blends ~90% of previous frame's visibility with purely GEOMETRIC validity test (reprojection + depth) that ignores sun movement; with still camera and moving sun, shadow keeps ~90% of value traced against OLD sun direction
- Ray-driven temporal textures: 4 textures (2x RG32Float + 2x RGBA16F) at scene render size = 32 bytes/pixel between them; 144 MB at 2750x1639, 225 MB at 3532x1987 idle allocation when ray tracing off
- `rdStagedFallbackLogged_` / `rdStagedRunLogged_`: start-up fallback (RT history not ready yet) is not the last word; logs first frame staged passes actually record separately
- Milestone 4 checkerboard sub-stage: half-rate CSRdGi when rayDrivenStages == 2, denoiseGiRanThisFrame tracks if denoiser actually produced output so CSRdGi can decide whether to skip pixels

## modules/render.voxi/shaders/voxi.hlsl

### gGiShadowParams bit field
Removed detailed bit flag documentation (32+ lines of explanation about Settings toggles). Key: bits 1/2/4/8/16/32 control ray-driven optimization trades measured against cost, defaults since 896c5187/2026-09-27; assembled by VoxiRenderer::prePass and recordStagedRayDriven.

### gAmbientParams legacy bits
Removed detailed documentation of legacy A/B test bits (R0-W6/M5, 12+ lines). These are for comparing against pre-fix behavior across different render modes.

### AVER_VOX_FEEDBACK
Removed measurement data: "1.0 and 8.0 were measured (8.0 too hot); 3.0 checked once against Sponza path tracer (c6d1a750, scripts/pt-compare.ps1)".

### AVER_VOX_MAX_BOUNCE_GAIN
Removed detailed explanation about energy runaway: "with x3 compensation, anything reflecting over a third of channel gained energy each rebuild; a sun drag (rebuild/frame) drove NewSponza's curtains room to solid red".

### TLAS instance masks
Removed: measured probe values (43,33,28 ray-driven vs 206,215,218 --rt-render-mode 0, same pose).

### Occlusion-aware fog section
Removed design discussion about why screen-space AO approach failed (aver-fog-skyvis-failed.md reference; detailed explanation of camera motion flashing).

### STAGED RAY-DRIVEN PASSES
Removed milestone/version history references and detailed pass descriptions. Kept essential: CSRdVisibility traces, CSRdShadow resolves shadow.

### gRdSunVisTex
Removed detailed explanation of history mechanics and blended replay reuse logic. Kept: "resolved sun visibility, rgb=tinted transmittance, alpha=linear view depth".

### LOCAL LIGHTS section
Removed detailed history mechanics (ping-ponging with sun, depth validation against previous viewport, etc.).

### lamp HISTORY reads
Removed SUB-PIXEL vs SNAPPED discussion (2026-09-28 date, measurement about motion artifacts with velocity magnification).

### rdSurfaceRoughness
Removed extensive explanation about:
- Why transcription from PSRayDriven is necessary (register pressure trade)
- Detailed slot descriptions (slot 0/1/2/3/4)
- Layer1 blending logic with slope normals
- Kept: roughness computation from material samples

### modules/render.voxi/shaders/voxi.hlsl part01

### averVolumeTransmittance
Removed detailed explanation of old fluid shader method and why ray-based approach was needed (depthCm guessing, side-face artifacts).

### VOLUME COMPOSITE formula
Removed: complete algebra derivation (final = specular + diffuse*a + bgRefr*T*(1-a), bit-identity proofs at T==1 and Refraction OFF).

### Refraction mode (eta)
Removed: detailed history about backFace gate vs gCameraMedium (cost at 41 degrees with glass panes becoming dark slabs).

### NDC projection
Removed: measurement data about viewport rect bug ("100% of rail's pixels landed >60px away, proving this was projection not refraction").

### Edge fade refraction
Removed: explanation about why average not bare texel (references to rdLocalLightsVisibility accumulation comment).

### Two-sample backdrop composite
Removed: magenta artifacts history ("41773 magenta pixels at 45-degree pool camera with refraction on"). Removed: full algebra derivation.

### TIR (total internal reflection)
Removed: measurement at 25 degrees underwater (0.59 mean vs 18.8 expected). Removed: comparison with material_prelude pane case.

### Blended replay double-write
Removed: cross-references to C9 finding and multiple file references (voxi_rt.hlsli, voxi_restir.hlsli).

### Clamping reflection values
Removed: detailed explanation of runaway ray persistence (85% weight reprojection, 7x7 spatial gather, NaN handling).

### CSAirVis dispatch
Removed: layout/descriptor details, measured cost (48^3 pass ~10 ms). Removed: detailed scheduling explanation.


### averVolumeThickness design rationale
Measures actual thickness along the ray instead of guessing from depth and pitch. Old fluid shader used `depthCm / max(abs(V.z), 0.15)` from constant floor height, which was wrong on side faces: e.g., 8.7m path on the pit's side of PTTest, blowing it white. This function measures the back-side geometry directly. Both lanes work: either the volume's back face or opaque geometry inside (rock, pool floor) can end the ray. Returns 0 when nothing hits (unbounded volume shouldn't absorb infinitely).

### Refraction and TIR (total internal reflection)
At n=1.33 (water), critical angle is 48.75 degrees. Past that, the underside becomes a mirror showing pool floor instead of sky. This is physically correct behavior, not a bug. The refract() function returns 0 when no transmitted ray exists (TIR case); normalizing it would be NaN.

### averBlendedOutputBackdrop implementation
Uses backdrop as correction, not replacement. Final colour should be `specular + diffuse*alpha + dst*T*(1-alpha)`. Hardware premultiplied blend gives `src.rgb + dst*(1-src.a)`, so: `src.rgb = specular + diffuse*alpha + bg*(1-alpha)*(T-1)`. The `(T-1)` term is negative, subtracting the light the medium absorbed per channel—exactly what one blend alpha cannot express. When T==1 (no absorption), result is bit-for-bit identical (no-absorption regression test). Stacked translucency: `bg` is the scene captured BEFORE any translucent draw, so a second layer doesn't erase the first. Backdrop is a soft dependency; falls back to scalar composite when missing.

### Reflection spatial denoiser versus rtShadowSpatial
Three key differences: (1) Radius from roughness, not constant (mirror=0 means no gather), (2) Untraced neighbours skipped, not counted black (gRtReflHist vacate sentinel is negative alpha), (3) No luminance weight (SVGF needs per-pixel variance, which engine doesn't track for history). Geometry weights only. Crease term (AVER_GBUFFER_HISTORY) copied verbatim from rtShadowSpatial.

### Reflection temporal history decisions
Below AVER_REFL_MIRROR_ROUGH, surface is a mirror throughout: no jitter, no temporal history, no spatial filter. Earlier version gated temporal blend on `rough > 0.0` while claiming smooth surface "still takes fresh value outright"—but those disagreed for every near-mirror (glass at 0.05), getting 85% history blend that adds lag, smearing glass behind moving camera. Deriving all three behaviors from ONE cutoff is the point: jittered-but-unfiltered is noise, filtered-but-unjittered is blur—they must agree. No history texture: lobe stays closed, avoiding flicker; rough=0 reduces to exact mirror ray (pre-change behavior).

### Tiled reflection rendering cadence
Tiles are 8x8, viewport-relative, with parity alternating by frame count so a tile that skips this frame traces next. This amortises with shadow rays on the same cadence. Skip-trace gate is lower cost than full trace for rough surfaces. Velocity-discounted blend: far-slid sample is same surface but different point; full trust smears a comet tail.

### G-buffer additive feature
WHY IT EXISTS: no motion vectors, no G-buffer—PSMainVoxi returned one SV_TARGET, normal/roughness/albedo living only in registers. Blocked vendored FidelityFX denoiser, FSR 2/3, TAA and screen-space reflections. Temporal reprojection was wrong for moving geometry (rtReprojectHistory transforms THIS frame's wpos through LAST frame's camera, valid only for static surfaces). THIS SLICE ONLY WRITES THE TARGETS—wiring a consumer (FFX denoiser, FSR3, TAA) is out of scope on purpose: unread-but-written is deliberate here.

### Velocity mapping and viewport rect subtlety
THIS frame's clip position must land in THIS frame's viewport rect (gViewProj paired with gSceneViewportCur), not last frame's (gPrevViewProj with gSceneViewport). The editor can redock the 3D view between frames. Falls back to gSceneViewport when device had no current rect yet (gSceneViewportCur.w == 0). Motion of a surface: wpos through this frame's camera minus wposPrev through last frame's captures both camera motion and object motion. Raster (PSMainVoxi) uses camera-only motion; ray-driven reads RtInstance::prevObjectToWorld instead.

### Air visibility determinism
Computed with 8 fixed sample points, no jitter, no per-pixel temporal history (tried before, reverted for flashing on camera motion). Sample starts at fog's start distance, not camera (fog before that contributes nothing). Height-fog density weighting: sample deep under fog outvotes one near ceiling. Outside volume counts as open sky, matching CSAirVis's march rule.

### Octahedral normal packing
10 bits per axis keeps angular error ~0.1 degrees, far below denoiser edge-stopping or debug view visibility. Fold lower hemisphere over diagonals, map [-1,1] to [0,1]. Decode is in modules/render.denoise/shaders/aver_denoise.hlsl (dnsrDecodeNormal) and sandbox/shaders/gbuffer_debug.hlsl—change the three together if modified.

### Poison colour system (debug sentinel)
Seven sentinel colours from giRestirIndirect (magenta, cyan, yellow, orange, blue, red, green) mark different guards firing. PSMainVoxi/PSRayDriven add an eighth (violet) for specular term ceiling clamp. All built from 0.0/0.5/1.0 alone (exact in IEEE754); real shading cannot produce them by coincidence. Precedence: giRestirIndirect colours always win over violet. Only meaningful right after giRestirIndirect call.


### PSMainVoxi shader design decisions
- Normal flip: uses dot(N,V)<0 via averVertexOf, not SV_IsFrontFace, to reuse consistent backFace logic that could disagree with a second notion.
- Single-sided blended surfaces: measured water box came out ~4 coats thick (alpha 0.02), darkening pit from (50.7,51.4,49.2) to (36.6,41.9,46.1). Author-gated on twosided flag; M_Glass sets it, water doesn't.
- Eye inside volume: inverted logic (discard backfaces when outside, frontfaces when inside) rather than switched off; disabling on false positive would resurrect the four-coats bug.
- Decal depth tolerance for reuse: 1 cm flat plus 0.4% of depth. gRdSunVisTex is RGBA16F whose mantissa steps ~2 cm at 30 m; tighter flat would reject true matches to storage error.
- Decal plane test for grazing views: depth gap grows as distance*h/camera_height; from standing eye (~170 cm) depth test failed every floor-decal pixel past a few metres. Measured on NewSponza 2026-09-28: blended replay 0.28 ms in editor vs 4.09 ms in PIE. Now uses normal plane reconstruction from quad neighbors.
- Quad-uniform: rtShadowTemporal/rtSkyOcclusionTemporal take screen-space derivatives internally, undefined unless all four pixels of 2x2 quad take same branch. One quad spanning decal edge traces as whole.
- Reflection roughness threshold: 0.75 rough chosen because past that, lobe wide enough that one ray cannot close near-hemispherical integral (spatial kernel saturates at radius 3; temporal rejects under motion). Below 0.5 is full strength; fade only across last quarter (0.5->0.75).
- Sky visibility: when tier doesn't support ray tracing, cone gather estimate used instead. Without guard on rtSkyOcclusion (which names gScene), non-RT entry points (VSShadow, PSVoxel, CSResolve) fail on undeclared identifier.
- Sky ownership: giRestirIndirect's traced miss gave pixel its sky; adding ind4.ambient on top double-counts. Subtraction avoids zeroing FmsEms (which measured worse due to throwing away multi-scatter compensation).
- FOG decision for translucent: fogged differently than water (which is already atmospheric stand-in). Double-fogging bug: outc.rgb*T + inscatter*(1+(1-a)) at alpha 0.12 nearly twice the haze (NeonDistrict, 750 glass placements). Fixed: subtracting (1-a)*fog(0) scales in-scatter by coverage.
- Backdrop path: captured for volume surfaces (glass/water); tinted/bent background not light the pane made, so re-added unfogged.
- Air in-scatter weight: 1-bgWeight (1-alpha except on TIR return).

### PSMainVoxi / PSRayDriven sync invariants
- Both paths agree on: sun shadow (SSS push), material evaluation, shading (direct+indirect).
- PSMainVoxi does: cone trace fallback, voxel GI.
- PSRayDriven does: ray-traced reflection with CSRdRefl decision, simplified material (GRAPH disabled, normal perturbation compiled out, path-traced bounce instead of cone).
- Textured correction: under AVER_RT_BINDLESS hit samples baseColor, metal-rough, normal, occlusion, emissive, slope-blended second layer with real gradient footprint. Texture2DArray is in register space 1.
- Blended draws: never reach PSRayDriven; routed into TRANSLUCENT LANE (kRtMaskTranslucent) for shadow attenuation. Primary ray traces AVER_RT_MASK_OPAQUE only. Glass drawn via blended replay at PSMainVoxi.

### Debug view helpers
- viewDebugHash: well-mixing 32-bit integer hash (Chris Wellons' lowbias32); three xorshift/multiply rounds enough so adjacent indices land on unrelated hues not gradient ramp.
- viewDebugHashCombine: feeds first hash output back through hash with second id XORed in (same shape rtHash uses for pixel+salt on floats).
- viewDebugHueColor: fixed saturation/value (0.65, 0.85) so every id equally legible; no derivatives, no dynamic indexing.
- viewDebugHeatRamp: log (not linear) because 400x span (50cm to 20km, centimeters units) would crush near-camera surfaces into same blue. Blue->cyan->green->yellow->red.
- viewDebugDistanceColor: log2 scale with kNearCm=50, kFarCm=20000.

### Ray-driven visibility (PSRayDriven)
- Hardware early-Z tradeoff: rasterizer culls hidden before shader; ray pays full traversal for same answer. Mode exists to measure this trade.
- Derivatives on ray hits: undefined (same ddx/ddy landmine rtShadow avoids). Use SampleLevel/SampleGrad, never plain Sample().
- Surface reconstruction: barycentric interpolation + rotation-only normal transform (no inverse transpose) so agreement with mirror rendering.
- G-buffer contract: both return sites fill every AVER_GBUFFER channel; must not sit empty by default (ray-driven is default now).
- Sky miss: writes 0-packed-normal sentinel giLoadPrevSurface tests for (no stale reprojection surface).

## modules/render.voxi/shaders/voxi_neurac.hlsli

- **Include isolation**: voxi_neurac.hlsli includes only pure math: constants (mirrored AVER_RC_* from header), RcInfo/RcCell layouts, addressing, SH basis, and packing. Declares NO resources; voxi_neurac_io.hlsli (separate) handles scatter/lookup resource binding (t22/u20/u21). This separation lets resolve shader include pure math exactly as scene shaders do, with different resource bindings.

- **Usage scope**: voxi_restir.hlsli includes it only under `#if AVER_NEURAC` (four lazily compiled staged-compute twin pipelines). Every other variant never sees it.

- **Tag aliasing bounds**: 3 × 8 bits of (worldCell >> 6) per axis = 256³ tag space. Toroidal address (worldCell & 63) cannot distinguish cells 64 apart; tag distinguishes them up to 256×64 cells before aliasing, bounded by age field lifecycle.

- **Fixed-point accumulation**: SH terms scaled by 2^15, normal sums by 2^16. Worst-case single sample ≈ 491 (L=32, basis 0.488603, π/0.1 weight). With 64-sample cap per cell per frame, SH worst-case ≈ 1.03e9 (2x margin under int32 max).

## modules/render.voxi/shaders/voxi_neurac_io.hlsli

- `rcScatter`: The ray is cosine-sampled about N, so pdf = cos/PI with cos = cosDir2; the unbiased projection weight is PI/cos. The floor at AVER_RC_MIN_COS under-weights the 1% grazing samples (P(cos < c) = c^2) instead of letting 1/cos blow the fixed-point headroom.
- `rcScatter`: indY's semantics are inherited from F2 (sky on miss, voxel volume on hit, 0 outside).
- `rcLookup`: The technique is related to F2's shell-straddle fix—looks half a cell out along N.
- `rcLookup`: Weighting combines trilinear * normal agreement * planarity * observation count * freshness.
- `rcLookup`: A cascade contributes confidence * edge-fade of `remaining`; a cold or edge-adjacent fine cascade hands the rest to the next coarser one.
- `rcLookup`: The F2 sky-ratio formula fills whatever remains uncovered.
- `rcDebugColour`: Cell edges are darkened within 4% of a boundary on the two axes the surface runs along (the axis closest to N would darken a whole face).
- Visualization modes: (1) cached light—rcLookup's irradiance/PI alone (black where nothing is cached); (2) coverage—confidence (red=fallback does it all, green=cache does it all); (3) cascade—finest holding the point (cyan 25, yellow 100, orange 400 units); (4) cell state—green=observations (n_eff), red=age, dark violet=empty or stale.
- The lookup's last confidence (1 - fallback fraction) is stored in gRcLastConf for the F2 path debug view.

## modules/render.voxi/shaders/voxi_neurac_resolve.hlsl

- ~90% of cells are typically empty, so the per-frame cost of an idle cell is dominated by the single count read.
- Implementation source is NeuRaCLayout.hpp for the geometry numbers; see NeuRaC.hpp and docs/rendering/NEURAC.md for full context.
- World cell calculation uses bitwise AND on negative values: `int & on a negative value is two's complement, which is exactly the modulo wanted`.
- Alpha (blend rate) design: a fresh cell converges at 1/k (the exact running mean), a mature one keeps a floor (alpha minimum from info.hdr1.x) so a lighting change still shows.
- Alpha convergence is per frame, not per sample.

## modules/render.voxi/shaders/voxi_restir.hlsli

- **Cosine floor firefly fix**: AVER_GI_MIN_COS (0.05) bounds the weight W at ~63 in the worst case (500x reduction). At the old 1e-4 threshold, ~31,400 weight became invisible only in the single-candidate case (cancels in the estimator's receiver cosine). Once reused spatiotemporally at a different surface's target pdf, the unbounded weight spreads as a bright speck through 30+ frames. Discards 0.25% of directions (within 3 degrees of tangent plane where Lambertian lobe carries least weight). Rejected, not clamped — clamping pdf while keeping sample biases the estimator dark.

- **Metal-rough map reading for glTF hits**: The initial candidate now reads the hit's own material maps instead of factors alone. glTF factors MULTIPLY their maps, not average them. Sponza example: metallicFactor 1.0 with near-zero metalness in the metal-rough map's blue channel would read every textured hit as pure white metal (kdAlbedo 0, no diffuse/multi-bounce). Measured on PTTest NewSponza (linear means, sun 85.6 deg, gallery pose, against 1-bounce path tracer baseline with --tonemap 0, fog off): single bounce factor-only 0.0168 whole / 0.00338 inner wall vs. PT 0.0095 / 0.0012; factor-only multi-bounce added only 0.0004 to a wall PT lights 85% by multi-bounce (PT 4 bounces 0.0080 vs 1 bounce 0.0012).

- **Diffuse ambient without specular environment term**: Adding `ind.specular = averSkyRadianceCheap(reflect(...))` through averShadeIndirect flattened Sponza's interior to uniform grey wash. Root cause: `specEnv = FssEss * ind.specular * specOcc` in averIndirectTerms (material_prelude.hlsl) is NOT scaled by ind.ambientScale like diffuse ambient, so secondary hits read full unoccluded sky while their diffuse half was correctly scaled down — interior dominance made this wrong. The alternative (proper metalness-weighted sky specular, scaled/occluded like diffuse) remains open but needs different machinery.

- **F2 diffuse visibility**: The F2 path (second-bounce diffuse) now owns its own visibility via a second cosine ray instead of unoccluded sky. gAmbientParams.z bit 4 TRUE keeps legacy unoccluded read (HEAD, A/B only); FALSE (corrected default) traces the ray: miss reads sky (visibility-tested via SH); hit inside GI volume reads that voxel's radiance (third bounce, missing before); hit outside volume contributes nothing (real geometry outside voxelised region is occluded). Cosine-weighted sample (streamSalt 0.71) makes PI and cosine cancel, yielding kd_y * L(w2) directly per PSVoxel's injection convention. giIntensity deliberately not applied here (applied once to whole estimate at giRestirIndirect to avoid doubling).

- **Voxel shell straddle fix**: The GI volume stores one-voxel-thick SHELLS (mean radiance=1, empty=0) not solid fill. Sampling exactly on a hit puts the trilinear tap astride the shell, blending lit texel with unlit neighbours (the shell's far side or hollow interior), darkening result 0.5-0.75. Fix: pull lookup half a voxel back along -dir2 (incoming direction, stands in for hit surface normal toward room). voxelWorldF2 derived from gVoxelOrigin.w/gVoxelParams.x (same as PSVoxelDebug/CSResolve, voxi.hlsl:3441 and voxi_gi.hlsli:210).

- **GI hit shadow map measurement**: NewSponza, staged mode 1, GI trace 3.38 -> 2.68 ms with map. Prototype's ~1% brightening under column capitals (19 cm texels) no longer reproduces (MAD 0.18). Enabled by default (Settings::rtGiHitShadowMap).

- **Diffuse ambient gap in enclosed spaces**: averSkyIrradiance(s.N) has no notion of occlusion, so a second-bounce point under an overhang read full open-sky irradiance as if standing outside. This was deliberately kept for the legacy path (gAmbientParams.z bit 4 TRUE) because the corrected path (bit 4 FALSE) traces occlusion and was the source of improvement.

- **Half-res reconstruction tunables**: AVER_GI_VIS_HIST_WEIGHT 0.8 (EMA weight at rest, lerped to 0.5 under motion); AVER_GI_VIS_RHO_MAX 4.0 (ceiling on sky ratio from EMA); AVER_GI_VIS_NORMAL_POW 8.0 (normal similarity exponent); AVER_GI_VIS_PLANE_TOL_REL 0.02 (relative to depth); AVER_GI_VIS_PLANE_TOL_CM 1.0 (flat floor cm). Mirrored in GiVisibility.hpp; GiVisibilityTest syncs both.

- **Candidate texel cone footprint**: AVER_GI_HIT_TEX_CONE (0.1) is tan(cone half-angle) x ray length. Wide on purpose: diffuse bounce needs only the hit's local average colour/metalness. Mip 0 is the throughput trap; coarser mip is cheaper and no less right on average (see rtReflection's own comment for throughput measurement context).

- **Visibility cache (AVER_NEURAC)**: In dual-pipeline mode (CSRdGi/CSRdGiTrace), cold cache degrades to HalfResolution (path-2 sky-ratio fill) rather than black. rcLookup returns cosine-convolved irradiance/PI (the units F2 multiplies by kdAlbedo); `rem` is the fraction no cascade vouched for. Trained by every traced pixel's second-bounce ray (one Monte Carlo sample of incident radiance, cosine-sampled).

- **Occupancy floor for voxel lookup**: AVER_GI_VOX_MIN_OCC (0.05) prevents dividing by near-zero coverage when normalising voxel radiance by occupancy alpha. Below this threshold, the lookup falls back to 0 (no occupied voxel left to recover a radiance from).

## modules/render.voxi/shaders/voxi_rt.hlsli

- File header (lines 1-34): Extensive documentation of file structure, dependencies, and forward declarations. Preserved condensed header describing content only.

- RtInstance struct: Removed detailed history about materialIndex repurposing `pad` and struct size changes (96 → 160 bytes). Kept note that prevObjectToWorld must match C++ side.

- Foliage instance system: Removed verbose explanation of TLAS static prefix, InstanceID bit layout, and how part indices work. Condensed to layout definition only.

- rtLoadInstance function: Removed detailed explanation of foliage descriptor reconstruction and row-vector convention. Kept only what the code doesn't say (foliage static, prevObjectToWorld unused).

- RtMaterial struct comments: Removed history about materialIndex replacing pad, field ordering requirements for StructuredBuffer. Kept field documentation inline where useful.

- averRtSampleSlot section (lines 188-244 in original): Removed ~57 lines of AVER_RD_ABLATE measurement documentation including:
  - Ray-driven primary cost measurements (~6.7ms vs 7.82ms raster, PTTest scene)
  - Per-ablation mode explanations with timing deltas
  - GPU timing methodology (bracketing, diffing terms)
  - List of 13 ablation modes with scene/cost details
  - Notes about mode interactions and non-additive deltas
  Preserved only: mode definitions (required by code), warning that non-zero values render wrong.

- averRtUvGrad function: Removed ~50 lines of explanation about implicit derivatives being invalid at ray hits, ddx/ddy replacement via rdRayDx/rdRayDy, hitT scaling. Condensed to one-liner describing purpose.

- averRtSurfaceUV function: Removed mention of specific scene (PTTest floor/concrete, ElectricDreams terrain) where world-UV issue was discovered.

- averRtPerturbNormal function: Removed long explanation about RtVertex carrying no tangent stream, MikkTSpace QTangent, interpolation vs per-triangle flatness trade-off.

- averRtCutoutPolicy section (lines 420-447 in original): Removed ~40 lines of cutout cost analysis:
  - Jungle Ruins scene measurements (4M plants, 112 ms GPU, two per-thread knobs)
  - Per-ray-type cost breakdowns (GI 24.6→13.2 ms, sky occlusion 10.0→5.0, reflections 18.0→9.4, frame 112→80 ms)
  - Image quality metrics (mean within 1/255, MAD 2.4)
  - Rejected alternative (sampling alpha at mip ray cone covers, measured 6→57 ms sun shadow regression)
  Preserved: policy mechanism (budget, solidCutouts flags), brief note on alpha-masked instances needing FORCE_NON_OPAQUE.

- History textures section (lines 515-573 in original): Removed verbose explanations of:
  - Ping-pong texture mechanics and why needed
  - Hit distance encoding and denoiser curve normalization
  - Per-frame signal definition and why occlusion alone insufficient
  - Denoiser relationship to AO history
  Preserved: what each texture holds (visibility/depth pairs, hit distance encoding).

- rtHash function: Removed explanation of spatial-only determinism and gate oracle bit-exactness.

- rtRadicalInverse2/rtDiscSample functions: Removed ~40 lines of sampler explanation:
  - Nested vs sqrt((k+0.5)/n) comparison
  - Golden angle + radical inverse + area mapping rationale
  - Exactness on every adapter (reversebits + IEEE round-to-nearest)
  Preserved: one-liner about what each does.

- rtHemiDiscSample function: Removed lengthy explanation of:
  - BUG THIS REPLACES (45-degree ring determinism on rtDiscSample(0))
  - Per-pixel-only hash problems temporal accumulation can't fix
  - Malley's method derivation
  - idx nesting and streamSalt collision prevention rationale
  - Frame determinism (frameIdx effects on --frames N runs)
  Preserved: cosine-weighted hemisphere purpose and sequence nesting property.

- rtShadowEx section (lines 683-726 in original): Removed ~50 lines of FIRST-HIT FAST PATH explanation:
  - Binary shadow property (no transmittance walk benefit)
  - Jungle Ruins measurements (12,494 entities, RX 7800 XT, 16.2 ms / 9.8 ms at 6.7°, 7.4 / 4.3 ms at 59°)
  - Fast path vs full walk savings (UNMEASURED)
  - Cutout rule replication and cast-shadow test matching
  Preserved: bit 32 enabling ACCEPT_FIRST_HIT, opaque-lane specialization.

- Transmittance walk section (lines 741-772 in original): Removed ~30 lines explaining:
  - NO ACCEPT_FIRST_HIT consequence (binary vs transmissive)
  - Glass attenuation vs wall stop difference
  - Cost note (shadow rays walk to opaque/end even without panes)
  - Two-model vs three-model tradeoff (two slots picked not array to avoid register spillage)
  - Measured register spillage (7.96ms→9.95ms on wave-bound sun shadow)
  - Min/max t pairing explanation
  Preserved: two models (volume vs per-crossing), slot bounds as code comment.

- Volume absorption explanation (lines 757-772): Removed ~15 lines on:
  - One vs two vs more hits interpretation
  - Pool floor under water example
  - Concave/overlapping geometry outer-span heuristic
  Preserved: slot count as comment (two scalars, not array).

- rtShadowRayStart / rtShadowRay0 helpers: Removed explanation of factoring for caller drift protection and hot-loop optimization.

- rtShadowOpaque function: Removed ~8 lines explaining:
  - T1 (Settings::rtSecondaryShadowOpaque) purpose
  - Trade-off (no translucent exclusion, no tint)
  - PRIMARY sun shadow keeps tint distinction
  Preserved: function purpose (cheap secondary hit shadow) and cost trade (BVH vs transmittance).

## modules/render.voxi/src/Voxi.cpp

- `Renderer::setDeviceInfo()`: Records device capabilities and re-clamps settings against them.

- Refuse lambda logic: The refuse() function distinguishes between two failure reasons when logging unsupported features: Device-unsupported (hardware cannot run it) vs NotImplemented (engine does not yet implement it on any GPU). The old wording blamed hardware for both, so NotImplemented features read as "your card is too old", potentially misdirecting users to hardware upgrades when the issue was missing engine code.

- `Renderer::setSettings()` giMode storage: `giMode` (ReSTIR GI flag) is now stored exactly as requested (unclamped) and resolved at read-time by `RenderSettingsResolver.hpp`. This is safe because downstream code never reads the raw field to decide if ReSTIR GI runs; only `VoxiRenderer::giRestirWanted()` reads `giMode_`, and the shader branches on `gGiRestirParams.x`, a per-frame constant gated on history texture availability. Keeping the unclamped request lets UI and console show the selection as left by the user rather than silently discarding it when RT goes off/on.

- Tier derivation pattern: When caller changes GI/ray-tracing tier and leaves the derived field exactly as it currently is (common case: editor Quality combo alone, or project manifest with `giQuality` but not `voxelResolution`), the derived field is set from the new tier. Explicit requests in the same call (value differs from current) always win. This pattern applies to `voxelResolution`, `giUpdateInterval`, `giCones`, `giRestirVisibility`, RT shadow knobs, and `ptBounces`.

- giUpdateInterval measurement history: Revoxelisation was measured at 108 ms of a 229 ms frame (47% of frame time, largest single cost). Dropping to one rebuild in four took frame time from 121.1 to 104.5 ms on the Electric Dreams scene. Epic tier stays at interval 1 (every frame) to maintain bit-identical indirect light compared to always-fresh behaviour; cheaper tiers buy speed with temporal latency (indirect light lags scene changes by up to N-1 frames, static scene converges identically).

- giSkyOcclusionRays keying: Keyed on `rayTracing` tier (not `globalIllumination`) because it costs a ray and requires acceleration structure, not GI voxel grid. A project raising GI quality on hardware without ray tracing must not pay for rays.

- giRadianceCeiling sentinel: Lower bound deliberately > 0 because shader macro (voxi.hlsl/voxi_gi.hlsli) falls back to engine default 16.0 when value is exactly 0 (sentinel for uninitialized FrameConstants). Allowing `set voxi.giRadianceCeiling 0` through here would collide with sentinel and silently do nothing instead of enforcing near-zero ceiling user requested.

- rtRenderMode typo handling: Clamp invalid values to 1 (ray-traced), not 0 (raster), to avoid silent renderer swap from a typo. Clamp to 1 rather than 0 because renderer swap is bigger behaviour change than a typo deserves.

- rayDrivenStages modes: Three valid modes — 0 (single pass), 1 (staged), 2 (staged + half-rate GI). All three draw the same scene, so typo clamping to default 2 cannot swap renderers (unlike rtRenderMode).

- giMode modes: Two valid modes — 0 (cone gather), 1 (ReSTIR GI). Typo clamps to default 0 (cones) rather than silently landing on ReSTIR.

- giRestirVisibility clamping: Valid values 0-4. Typos clamp to 3 (Full), never 0 (NoRay would silently reintroduce over-brightness that commit cb4b48df's contrast fix was designed to remove). Experimental cache mode 4 is legal and passes through; only values above 4 clamp to 3.

- giRestirSpatialSamples packing: 15 (AUTO) through 0 (temporal only) are all legitimate; packed into exactly 4 bits (bits 12-15) by `givis::packAmbientW`.

- giRestirMaxHistory packing: Per-neighbour M cap (reuse.maxHistory); 31 is limit of 5-bit packing in `gAmbientParams.w` bits 18-22.

- denoiserMaxSamples: Zero history length would divide by zero inside FidelityFX's accumulation; 255 is defensive ceiling (history that long no longer follows scene changes). Clip weight must stay positive (zero clips history to a point).

- refractionMode typo handling: Clamp invalid values to 1 (ScreenSpace), not 0 (Off), to avoid silently turning refraction off over a typo (bigger behaviour change than typo warrants). The HARDWARE question — can this device actually run ray-traced refraction — is a separate concern resolved by `resolve()` in `RenderSettingsResolver.hpp`, not this range check.

- Feature interaction resolution: Everything in `setSettings()` above the resolve() call clamps fields against their own valid range or feature support. Finer-grained interaction checking happens in `resolve()`: e.g., ReSTIR GI needs RT hardware AND RT tier on AND GI tier on; ray-driven primary visibility and ray-traced refraction each need RT hardware AND RT tier on. `giMode` is stored unclamped and resolved by readers; `rtRenderMode` and `refractionMode` are still clamped in settings_ itself so raw readers (e.g., A2 self-contradiction check in SandboxApp.cpp) see the same honest value UI and console see.

- `Renderer::status()` reasoning: Switch statement and per-feature reasoning lifted to `RenderSettingsResolver.hpp::featureStatus()` so same device-capability answer is reachable without live Renderer instance (e.g., manifest apply before commit, unit test with hand-built device).

- `*ForQuality()` one-liners: All *ForQuality functions are one-line forwards to `aver::voxi::ladder` functions in `QualityLadder.hpp`. Switch statements and reasoning that used to sit directly above each (one measured table, one retuning history, one post-mortem per function) moved to QualityLadder.hpp so edit to rung value and justifying comment cannot drift apart (history: `ladder::rtPixelsPerRayTile` drift happened here once). These stay in Voxi.cpp as one-liners only because `Renderer::*ForQuality` is shared library's exported, P/Invoke-adjacent API surface and QualityLadder.hpp is not.

## modules/render.voxi/src/VoxiRenderer.cpp

- `kGiShadowSize`: GI-only shadow map (1024) was a workaround to separate camera cascades. Old union-with-last-cascade approach: cascade 3 measured at 36,744cm radius (camera-fitted: 9,923cm); 3.7x wider, ~14x area. Admitted every draw into per-cascade cull (census [3,4,5,27]) on streamed scenes, wasting texels. 1024 feeds 128-voxel grid at Medium (512 at Epic), 8 texels/edge (2 at Epic).

- `perPixelBufferNeedsRealloc()`: Buffers used to only grow, causing memory bloat on render-scale changes. Jungle Ruins capture: 860 MiB at 3532x1987 while scene rendered at 2049x1152 (render scale 0.58), which needs only 289 MiB. Factor-of-two hysteresis keeps the buffer during small scale swings.

- `kRdSinglePassLamps`: Ray-driven single-pass megakernel sits at AMD driver's register limit. Lost device before ec35bb5a due to register pressure. Lamps removable without touching shader via AVER_RD_SINGLE_PASS_LAMPS define. False keeps light count at 0 for single pass.

- `kMaxDraws` (65536): Was 4096, caused Electric Dreams passes mid-stream (~6,370 resident entities) to drop draws silently—entities rendered elsewhere but cast no cascade/GI shadow or wrote nothing to voxel grid (shading bug, not missing object). Was 16384 until Intel's Jungle Ruins (12,494 entities, many with multiple submeshes) filled it every frame. TLAS instance buffers prealloc at this count (64 B/instance = 8 MiB per frame when ray tracing on).

- `kVoxiSrvCount`/`kVoxiUavCount` slot inventory (23 SRV / 22 UAV):
  - t9: Dense per-frame material table (ray hits index via RtInstance::materialIndex)
  - t10: Blended-pass backdrop (opaque scene copied before translucency replay, glass tints per-channel not through blend alpha)
  - t12/t13, u7/u8: ReSTIR GI surface position/normal history (split because rhi::Format lacks 4-channel 32-bit float)
  - t14: Denoised sky occlusion (SRV often absent—tests GetDimensions() not cbuffer flag)
  - t16, u10: Half-res ReSTIR visibility history (bound only when giVisHistWanted())
  - t17, u16: Air sky-visibility volume (fixed 32³, sampled to attenuate fog in-scatter)
  - t18/t19/u19: Local lights (lamps) and visibility history
  - t20/t21: Instanced foliage part table and instance descs
  - t22: Radiance-cache cascade info (RcInfo, 80 B × 1, AVER_NEURAC twin pipelines only)
  - u6: ReSTIR GI reservoir buffer (GiPackedReservoir, 32 B/element, never rebound mid-session)
  - u11/u12: Staged ray-driven visibility record + sun-visibility; UAV-only (no SRV twin)
  - u17/u18: Sub-stage split candidate buffers (Settings::rayDrivenShadowTiles/rayDrivenGiSplit)
  - u20/u21: Radiance-cache accum + cells (UAV-only, updated through giCacheFreeBuffers)

- `giTableKinds()`: SandboxApp.cpp's GPU per-cluster path reserves kGiSrvCount/kGiUavCount (the first 9 SRV / 4 UAV slots). Voxi sizes its own table wider to avoid silent register rebasing. Tier 1 hardware requires a valid descriptor of the right KIND in every declared slot (why voxelAccumPlaceholder_ and placeholders for history pairs exist).

- `voxelAccumPlaceholder_`: Stands in for voxelAccumTex_ during W12's free branch. Not owned by binding set slot lifetime, so needs explicit destroy in shutdown() or leaks on every W12 free.

- Buffer management: Before fix, giReservoirs_/rdVisBuf_/rdGiCandBuf_/rdShadowTileBuf_ only grew, causing Jungle Ruins (3532x1987, scene render 2049x1152 at 0.58 scale) to hold 860 MiB for first three buffers vs. 289 MiB needed.

- `rtAccelSnapValid_`: Must be invalidated when TLAS is destroyed without draws changing (prevent stale snapshot matching when init() rebuilds from nothing).

- `vramReportedRadianceBytes_` etc.: Zeroed after shutdown so reportVramUsage() prints at least once on re-init. Comparison is "did the number change", not "did init happen", so without zeroing a re-init at unchanged resolution wouldn't report.

- `setSettings()` resource edges: OFF→on allocates, on→OFF deallocates. setSettings is the only place these edges are visible (onRenderTargetsChanged only sees resize). Each flag (giMode, fogOcclusion, rayDrivenStages, localLights) has independent on/off unrelated to ray-tracing tier, so each needs its own edge capture.

- `giRestirVisibility_` defensive clamp: Clamped even though Settings::clamp() already does it. Typo lands on Full(3), never on 0/NoRay (which is what unclamped would silently decode as through shader's `& 3u` mask from 2.9's bit table). 4 (Cached) is legal here but never reaches shader as 4 (beginShadowHistory packs as mode 2 plus bit 128).

- `layeredBsdf_` LATCHED at init: Pipelines built once; later changes require project reload. Not re-read every frame like refitOrRebuildTlas's rtRefitAccel.

- Lighting legacy bits history reset: Bits 1/4/8 (R0/R2/R3) reshape ReSTIR samples/adds/reuses, making history on one side stale (same reasoning as resetGiHistory comment). Bit 1 also reshapes sky-occlusion ray. Bit 32 (W6/M5) gates whether blended fragment writes shadow/reflection/AO histories; unlike 1/4/8, affects what histories themselves hold, so RT history rides along.


### Shadow atlas reader behavior
- The atlas has no reader while ray tracing is on, in both raster and ray-driven mode
- gShadowTex is sampled only at shadowSampleCascade (voxi.hlsl:1585) via shadowFactor, reached only on gShadowParams.z <= 0.5
- shadowParams[2] and rtActive_ are written together in buildAccelerationStructures, ensuring they never disagree
- Measured on PTTest Sponza, fixed camera: "Voxi shadow" is 5.57ms at four cascades, 2.07ms at two, on ~47ms frame
- Setting shadowParams[1] = 0 prevents stale atlas reads after skipping the pass

### Compute-skinned mesh exclusion from voxelization
- Skinned draws excluded from GI injection deliberately; giDrawsKey hashes mesh/transform/material, never dynamic vertices
- Once a character's transform settles, gate reports "unchanged" and indirect-light contribution freezes at last rebuild pose
- Measured on real rig: 2 rebuilt / 62 skipped of 64 ticks straight through a pose transition
- BLAS cache fixes same defect per-mesh by rebuilding every frame, but not available here since voxelization is one volume
- Treating skinned as always-changed would force full revoxelization whenever any character is on screen (measured: 96% of GI rebuilds normally avoided)
- Interim state is absence (no indirect light), not silent freeze; real fix is partial revoxelization not supported yet

### GI volume box selection (W3)
- Union with giBoxPrevDraws_ (last rebuild's box): mip 0 outside drawsBox_k still holds whatever drawsBox_(k-1) wrote there
- Voxel a draw stopped touching needs re-clearing THIS rebuild or keeps showing stale light
- Anything outside drawsBox_k UNION drawsBox_(k-1) already correct (0) by induction
- giBoxPrevValid_ says induction's base case holds

### Ray-driven staged passes architecture
- VERIFIED against D3D12Device.cpp (only backend rdStagedActive lets through)
- Compute pipeline bound via ctx.setPipeline gets SAME device per-frame constants as graphics pipeline
- D3D12ResourceFactory::rootSignature reserves root CBV parameter for every constant slot unconditionally for BOTH compute and graphics
- Switching back to graphics PSO reconfigures whole root signature and calls bindDeclaredRootCbvs() again
- D3D12RenderContext::dispatch is cmdList_->Dispatch(gx, gy, gz) with no render-target-state precondition
- uavBarrierBuffer/uavBarrierTexture emit plain D3D12_RESOURCE_BARRIER_TYPE_UAV
- No resource-state transition needed because gRdVisBuf/gRdSunVisTex/gRdGiTex/gRdAoTex/gRdReflTex stay in UnorderedAccess and read through SAME UAV registers
- CSRdShadow/CSRdGi/CSRdSkyOcc/CSRdRefl write DISJOINT resources by contract
- All four read only fenced visibility record, last frame's histories, and this frame's denoiser outputs

### Local lights (lamps) in ray-driven
- Raised before draw shades lamps when compiled with lamp term (kRdSinglePassLamps)
- Staged adds requirement: CSRdLocalLights compiled
- Lamps left raised for blended replay, which lights its panes from same list

### Texturing and ray-driven G-buffer
- Texturing and G-buffer are independent axes now: four pipelines, not three
- Used to read `rayDrivenTexPso_ != 0 && !dev_->gBufferEnabled()`, true to pipelines that existed but meant --gbuffer turned texturing off
- Any A/B across that flag attributed texturing difference to G-buffer
- pickGbuf() is single authority on whether four targets bind (declines when twin never compiled, before init(), at MSAA > 1)
- Falls back within same target count: if textured G-buffer pipeline didn't compile, fall back to FLAT G-buffer, not textured single-target


### manageInjectionAccumulator (lines 1-99)
- Injection accumulator allocation uses backoff strategy on OOM: giAccumRecreateBackoffTicks_ counts down from exponential backoff (min 0, max kGiAccumRecreateBackoffMax) to avoid repeated failing allocs that cost CPU. On driver level, repeated allocations at multi-hundred-MiB size may not even be free.
- Placeholder rebinding is load-bearing: every binding set must be rebound before resource destruction (aver-view-outlives-its-buffer.md lesson).
- Accumulator freed after giQuietTicks_ >= kGiAccumulatorQuietTicks with both giForceRebuild_ off and giConvergeTicks_ == 0. Two counters kept separate: forced rebuilds always need accumulator; converging bakes need it regardless of quiet ticks.
- Destruction is fence-deferred on both D3D12 and Vulkan: both backends only reclaim retired resources inside create/destroy factory calls. Idle scenes may leave retired texture unclaimed past process-VRAM readings. Fix belongs in backend (collect() once per frame, e.g., beside D3D12Device::beginFrame).

### buildAccelerationStructures (lines 101-519)
- Unchanged gate (rtSkipUnchangedTlas) skips per-draw loop when draw list, BLAS cache, dynamic mesh state unchanged. SRVs bound slots: 2/3/4/5/9 (slot 5 rebound by mover patch).
- Movable draw transforms omitted from gate key, delivered instead by patchRtMovers() (mover patch lane). With lane off (either setting false), every moving draw closes gate; behavior before lane existed.
- CARRY-FORWARD: rtInstanceData_ holds last frame's drawn transforms (including mover patches) until cleared; carry-forward lookup built first, then clears reset the data.
- Mover patch lane record (rtMovers_): filled by per-draw loop, sorted after. Empty list = plain gate hit, or lane off, or no movable draws with instances.
- CPU cost of draw-list walk measured from start of population to loop close (not BLAS/TLAS GPU recording, which is timed by gpuStat). See lastAccelBuildCpuMs().
- Dynamic mesh BLAS: compute-written meshes invalidate structures every frame (skinned pose would freeze). Only refits if rtRefitAccel on; otherwise forces full per-draw loop.
- Instance IDs (i.instanceId) assigned during loop, same pass deciding which instances survive, so lists can't drift.
- Alpha-masked geometry (foliage, grates) was traced as solid sheet, not holes. Fixed by setting ForceNonOpaque flag while staying in OPAQUE lane (occludes, shadows, reflection, just tests cutout).
- Material table order is load-bearing: buildMaterialTable() must run before buildGeometryTable(). Reversed order silently indexed fallback row. Found by coat weight 1 changing nothing in default mode but everything in others.
- Texture table before material table: buildMaterialTable() calls residentTexture() for indices.
- Foliage parts' materials join same table, keyed exactly as draw's.
- STALE-POSE FIX (refreshDynamicVertexSlices): compute-skinned mesh's slice refreshed every frame table is usable, not only on set change. Otherwise holds whatever pose the slice had when first laid out.
- TLAS refits on one-in-kTlasRefitsPerRebuild schedule (refitOrRebuildDynamicBlas). Dynamic mesh sets off its own refit share of rebuiltThisFrame_.
- Mover patch pairing: movers without instances in last build have nothing to move (inst stays kRtNoInstance).

### updateRtParamsPerFrame (lines 521-541)
- Shadow ray count = ceil(disc diameter / 0.3 deg), minimum 2. Measured on Sponza: 1/2/4/8 rays gave bit-identical frames at ~0.5deg; 8 cost 0.66ms over 2 rays.
- Ray bias 0.05 cm base, scaled by view distance in shader. Small enough not to detach contact shadows; large enough surface doesn't intersect its own rays.

### rtAccelMustForceRebuild (lines 543-574)
- Checks per-distinct-mesh via rtAccelMeshChecked_ lossy direct-mapped filter. City scenario: ~42,000 draws over ~2,900 meshes; mesh already checked this call is skipped.
- Compute-skinned mesh alone forces rebuild only when rtRefitAccel is off. When on, gate's refit-only pass (refitDynamicAccelStructures) keeps dynamic BLAS/TLAS/vertex slice current without loop.

### hashDrawMaterialInto (lines 576-638)
- d.matSet alone is not enough: MaterialSystem::gpuMaterialRevision() moves only on material factor/texture changes (not in-place edits); pbr::materialGraphs().revision() moves only when GRAPH-generated HLSL changes, not factor/texture refs. d.mat carries MaterialConstants captured by submitDraw() this frame (live edits included). Textures resolved by materials_.textures(); d.mat alone can't detect texture reassignments (packMaterial always leaves texIndex at unbound placeholder; only this renderer's residentTexture() fills it downstream).
- Whole d.mat block hashed, not just d.matBytes, because tail is zero; keeps exactly as strict as byte-serial walk.
- UNAUTHORED materials: per-draw loop builds constants from color/metallic/roughness; same three fields both callers hash.

### rtMoverPatchActive (lines 640-649)
- Mover patch lane requires both rtSkipUnchangedTlas and rtRefitAccel. With either off, all worlds stay in key (behavior before lane existed, for A/B parity).

### rtDrawHash (lines 651-679)
- Mover patch lane drops movable draw's world, hashes movable bit instead. Bit keeps draws that start/stop moving (PlayMobility flips, Play begin/end) from matching old self, so gate closes and full build records/forgets them.
- World floats hashed as raw bits (including -0.0, NaN payloads); eight bytes at a time.
- Two flags (translucent, hiddenFromOwner) hashed as bools rather than mask/flag bits.
- Finalized before addition using avalanche (FNV's last step leaves low bits correlated; plain addition of correlated values collides more readily).

### rtAccelDrawsKey (lines 681-720)
- Order-independent (sums per-draw hash): occlusion culling reorders drawsPrev_ every frame; reshuffled but unchanged list still matches.
- Covers: mesh, world, translucent/hiddenFromOwner flags, material. Exception: movable world when mover patch lane on.
- Draw-list half expensive (~42,000 draws); reuseListKey (in takeRtAccelSnapshot call, straight after gate computed same thing) returns gate's copy.
- Foliage term always recomputed (see declaration for why).

### rtAccelSnapshotUnchanged (lines 722-742)
- Modeled on giSnapshotUnchanged(): same "reject once per reason, log it" shape; split from takeRtAccelSnapshot (only called once rebuild decided).

### patchRtMovers (lines 753-872)
- TIE-BREAK BY NEAREST: within runs of equal id (same mesh/material/flags), greedy pairing by nearest translation. Equal identity means same structure content; only which interchangeable instance carries which world. Occlusion culling reshuffles every frame; two identical cars would swap transforms reporting each other's huge motion without this. Runs almost always size 1 (fast path). Runs >64 use draw order (cubic pairing expensive).
- Only movers whose world really differs from tlasInstScratch_ written. Frame with none = MoverPatch::Unchanged (paused Play = editor cost).
- rtInstanceData_ re-sent whole (one memcpy to ring slot): ring's three slots hold table up to three writes old; patching rows in place would need per-slot bookkeeping and partial-range RHI write (Vulkan's writeBuffer flushes whole mapping).
- Motion vector = prev frame world → new world (prev set first, then new into both).
- Pairing by identity: each draw's world goes into which instance. Keeps correct when list reshuffled (occlusion culling). Current movers (rtMoversNow_) collected by key pass, sorted into recorded order, must pair off recorded movers identity-for-identity. Key's sum makes mismatch near-impossible once matched; pairing adds instance assignment and guards against lane switched off or record stale between passes.

### reportRtAccelGate (lines 874-898)
- Widening-interval report (64, 128, 256, ... ticks): fixed-window report lands mid-load-in describing unwanted phase; widening window + lifetime ratio shows both phases.
- rtAccelRefitOnly_ (lighter pass when dynamic mesh kept current, no per-draw loop) and rtAccelMoverPatched_ (matched movers moved and patched, no loop) count as "avoided full rebuild" alongside rtAccelSkipped_ for percentage, broken out separately.


- `resetAoHistory`: Sky-occlusion, RT shadow, and reflection history share a single validity flag (`rtHistValid_`); independence would require new plumbing to track them separately.

- `setGiForceRebuild` (M4): Forces every tick to rebuild the GI volume; the cache is neither read nor written while forcing. Used for A/B measurements to establish cost of a bake in isolation.

- `setGiBoundedDispatch` (W3): The bounded-dispatch toggle invalidates the previous rebuild's bounding box whether toggled on or off; a state transition from OFF→ON leaves a stale box.

- `setGiFreeAccumulator` (W12): One-tick-delay contract with `manageInjectionAccumulator`; when turning the setting off, the accumulator is not recreated immediately but on the next tick if it was freed.

- `mixMeshId`: MurmurHash3's fmix64 shape kept for consistency with GameRender.cpp, though the mesh handle (small, dense, monotonically increasing) distributes evenly without mixing — kept only for shape-parity.

- `reportVramUsage`: Measured every frame and only logged on change to reduce spam; BLAS/TLAS sizes are sampled at intervals (every `kVramBlasResampleFrames` frames) to amortise the query cost.

- `beginScene`: Mesh submit cache staleness is bounded to exactly one frame (same window as the draw-list swap), preventing misattribution of depth-proxy/bounds answers across mesh re-uploads without external callbacks. Cache clearing every frame costs one generation bump.

- `submit`: Depth mesh and local bounds are resolved once per distinct mesh ID from cache (populated by `meshSubmitCacheSlot`), amortising per-mesh work across all instances. Without this cache (via `meshSubmitCacheEnabled_` A/B), submitting kMaxDraws would repeat the same mesh lookups up to six times (one per depth pass), example: 64.2ms → 5.4ms reduction (commit a7ff716d).

- `submit` bounding sphere caching: Local-space bounds are cached; world-space bounds are always computed per entity because `world` differs per instance (caching transformed bounds would hand all instances of a mesh the first instance's world bounds).

- `prePass` frame-time sampling: Discards the first `kFrameTimeWarmup` frames to let the system settle; the first sample after warm-up is discarded with the rest. Reported every 120 frames while running and again at shutdown, so killed runs still leave a number behind.

- `prePass` backdrop t10: Bound only on handle change. GetDimensions() is not a validity test on a dangling descriptor; a 0 handle MUST call clearSrv (not skip) to write the null view so shader takes its no-backdrop path. Forgetting this crashed the GPU on every window resize (D3D12Device::resize).

- `prePass` view-mode constant: `viewDebug_` takes priority over `unlit_` (editor keeps them mutually exclusive). Measured as a 2.79% pixel difference at unchanged mean luminance (raster chrome moving, not the scene) when a debug view was unlit not lit.

- `prePass` GI rebuild gate: Convergence ticks exist because `PSVoxel` re-emits the previous bake, so light accumulates one bounce per rebuild, not per frame. The gate alone skips 96–98% of ticks on a still camera, which starved the feedback term of a second bounce. Adding the feedback term alone moved isolated GI from 52.1% to 37.0% contribution; a rebuild that changed something schedules a few more ticks; the series converges geometrically and the gate goes quiet.

- `prePass` first-hit query (bit 32): TLAS holds no translucent-lane instance on most frames, so sun-shadow rays take the first-hit query instead of transmittance walk (same image, faster). This bit is set automatically, not a settings toggle (changes cost, not image).

- `prePass` GI gate efficiency reporting: Widening report intervals (64, 128, 256...), not fixed tick count — a fixed window lands mid-stream on a streamed level (draw list changes every tick, gate can't match) and always prints "0 skipped of 64" (measurement window excludes the case it measures).

- `prePass` air sky-visibility: Refreshed every frame round-robin one slab of `kAirVisSlabLayers` z-layers at a time, turning the whole volume over every `kAirVisResolution/kAirVisSlabLayers` frames at flat, small cost. A dirty volume (just created or GI just came back on) is filled whole first.

- `prePass` lamp constants for raster: Constants raised HERE (after all prePass uploads and before scenePass), so the backend's `PSMainVoxi` draws read them through `sceneConstants()`. Nothing later reads u19 back (blended-replay reuse is staged-only), so no UAV barrier follows.


- `giSnapshotUnchanged`: GI rebuild gate has four independent rejection reasons: extent/centre/sky change and draw list change. Cloud drift detection is tested last to distinguish "only clouds moved" from geometry changes, enabling cache decisions. Clouds tested over 30-second stale threshold (not 2 seconds which caused ~18 MB revoxelisation every 2 seconds on ElectricDreams; 2 seconds was dominant source of 439 MB disk writes in 900-frame run).

- `giDrawsKey`: Hash is order-independent via commutative sum. Each draw's hash finalised (xor-shift-multiply avalanche) before adding to decorrelate addends. Without the count, a list gaining one draw and losing another summing same total would read unchanged. Finaliser is splitmix64's, not FNV's last step, because FNV leaves neighboring inputs correlated in low bits.

- `giVoxelisedDraw`: Bounds test was missing from hashes and absence was exact failure: draw beyond volume re-keyed volume every tick while voxelizePass culled it anyway (all gate cost, no benefit). Most costly on streamed levels with props kilometres away. Sphere from centre_/extent_, not giShadowCentre_/giShadowRadius_, so voxelisation independent of GI shadow PSO build state.

- `giCacheScheduleDump`: Ceiling (256 MB) tested before copy, not after (previous path cost 1170 MB GPU copy + allocations then dropped on size test). PTTest/Sponza with --cam-wobble shows gate rejects ~72% of ticks moving, so old approach ran ~100 times in 151 frames (marked as "Voxi GI update" time, 10.79 ms moving vs 0.66 ms still). Ceiling prevents gigabyte-per-bake writes that would read as hangs.

- `giCacheFlush`: Sweep happens once for whole batch, not per-file (sweep stats entire directory). Previous per-write approach meant ten-bake session did ten directory walks to reach same end state. Now bounded by count (64 files) and bytes (1.5 GB of derived data).

- `giCacheRestorer`: Mip layout expansion from file's tightly-packed format to backend's padded format (D3D12 pads rows to 256 bytes, Vulkan packs tight). Both asked (via textureCopyFootprint), not assumed, keeping file portable and tightly packed.

- `rtCarryPrevTransform`: Carries previous transform via three routes: (a) exact match (same transform down to float bits, -0.0 and NaN are different), (b) nearest translation within kRtCarryMaxScan scan limit and kRtMaxCarryCm distance, or (c) current world (new or no trustworthy match). Each row matched at most once. Order-independent for same reason as giDrawsKey (linear scan within group, not across list).

- `buildRtCarryLookup`: Load factor kept <= 0.5 via power-of-two growth. Groups and entries both fit in same table size; allocation happens once in steady scene (~42-51k rows, so nothing allocated after first build).


- `fitCascades` shadow camera: Camera-relative inverse keeps values at 2 cm near-plane offset, preventing ~2 cm error at cloud altitude (f32 ulp 0.0156 cm at 2e5 cm). D3D depth is [0,1].

- `fitCascades`: GI volume union removed from cascade fitting. See kGiShadowSize for the measured cost of the old union. Camera cascades now fitted to camera alone, GI cascade separate. giShadowPass/fitGiShadow answer the volume separately.

- `buildGeometryTable` history: Used to give every INSTANCE its own geometry slice, copying identical geometry per instance: ~522 MB of vertex buffer for ~120 distinct meshes at 769 instances (16.3M vertices, 11.5M indices, two copyBuffer calls each) in ElectricDreams, scaling to ~3.5 GB/13,310 copies at 6,655 instances. Voxi acceleration structures cost 9.1ms at 759 instances, 64.2ms at 6,571, ~9.7us/instance both times (linear per-instance cost, not a TLAS build). Now uses one entry per distinct mesh. Skinned meshes (createPosedPartMesh) used to put whole posed buffer in twice; now consolidated. First-appearance order changed every frame due to occlusionOrder_ reorder, but sorting makes offsets stable.

- `buildGeometryTable` compute-written slices: Per-draw copy only runs when mesh SET changes. refreshDynamicVertexSlices re-copies current posed buffer every frame for skinned meshes (STALE-POSE FIX). This fixes correctness on every refit path regardless of Settings::rtRefitAccel.

- `buildMaterialTable`: MaterialSystem exposes no dirty count/generation number, so table compares actual bytes instead. Entry at index 0 is always fallback sentinel for unresolved materials (reserved for future resolution paths that might fail).

- `buildMaterialTable` logging: Previously used one-shot log that printed the first warm-up build's count (8 before level materials streamed) and never again, reading as "table capped at 8" during Jungle Ruins investigation. Now logs whenever material count changes, not every frame or merely on reorder. Realistic material count is tens, at sizeof(pbr::MaterialConstants) bytes each, times three ring slots.

- `residentTexture`: Texture table is append-only and memoised. Same texture asked twice returns same index, sizing by distinct IMAGES not materials (forty materials sharing one albedo = one slot). Nothing ever freed.

- `residentTexture` logging: Said once only per session when table is full, to avoid per-material per-build spam.

- `buildLocalLights`: Cutoff at 0.001 below display precision: kLocalLightRangeCutoff / pi ~ 3e-4 of lamp's radiance. White Lambertian surface receives this much of lamp radiance at this cutoff. Lighting enabled only on ray-traced scene modes (raster, single-pass, staged all light with the list); publishLocalLights decides per pass.

- `buildLocalLights` history: Previous design used absolute brightness (intensity 2, cutoff 0.01, ~14m ranges) costing 1.6ms of 16ms frame on NewSponza's 22 lamps. 32m range put nearly every lamp in every pixel's two loops. E1m for small bulb ~0.1, so 0.001 keeps range near same ~14m; 0.01 would cut off at ~3m. Per-pixel loop must end somewhere, capped at most 50m.

- `buildLocalLights` design: lightIntensity is MULTIPLIER on emitter's glow and size. glow = emissiveFactor peak channel as Lambertian sphere radiance L, size = bounding-sphere radius r. E1m = pi * L * r^2 (r in metres) = irradiance sphere casts at 1m, in sun units (SkyAtmosphere::sunIntensity). So 1 lights exactly what glow/size cast, 2 lights twice that, lamp and sun compose on same scale.

- `buildLocalLights` colour: emissiveFactor normalised to max component 1. White when all zero, so lamp with no emissive factor still has L and colour to light with.

- `buildLocalLights` range: Where irradiance E1m * lightIntensity / d^2 falls to kLocalLightRangeCutoff of sun units: d = sqrt(E1m * lightIntensity / kLocalLightRangeCutoff) metres. Clamped to at least four radii (lamp always lights surroundings) and at most 50m.

- `buildLocalLights` ordering: drawsPrev_ reorders every frame via occlusionOrder_, but shader's light pick doesn't care about order. Kept set is re-sorted by its own bytes before upload. Uploaded list (rdLocalLightHash_) is function of SET alone, surviving a camera move that doesn't change which lamps are in it.

- `foliageParts_` geometry: One multi-geometry BLAS per prototype; parts join in geometry order. Hit's GeometryIndex() plus prototype's first part IS the part's row in foliageParts_.

- `foliagePartData_` opacity: Opacity per geometry decided here once. Alpha-masked parts reach cutout test (averRtCandidateOpaque), every other part keeps the any-hit skip.

- `foliageParts_` constraint: First part's row must fit in kRtFoliageIdBit (23 bits).

- `foliage` shader references: objectToWorld never read (shader takes instance's own transform) -- identity. prevObjectToWorld same as objectToWorld (foliage is static). Shader takes instance's own transforms, not record's.

- `uploadFoliagePartTable`: Rebinds placeholder before destroying old ring to prevent dangling views in flight.

- `foliageKey` computation: Same finalised-sum shape as rtAccelDrawsKey(). Material half reads each part's material from material system's dense table as of frame's update() (the live bytes) and binding set textures. These are the two things hashDrawMaterialInto watches for a draw.


- `scenePass` lighting pipeline: The GI candidate's hit samples from the GI-only shadow map; CSRdShadowProbe and CSRdGiTrace perform sub-stage splits for probes and GI traces respectively, writing tile counts and candidate buffers that the main shadow and GI stages then read.

- `giDispatch` condition: This variable was previously introduced between the shadow and GI dispatches (S1 and G1) as part of the sub-stage split architecture. It checks whether ReSTIR GI is the chosen diffuse estimator AND cone trace is gated on. The decision reads the same `cb_` fields the shader tests, so CPU and GPU can't disagree. `rdStagedActive()` already refused the frame if the condition holds but `rdGiCsPso_` is 0, so reaching here means the pipeline exists.

- Checkerboard GI half-rate tracing: giCb gates on three things beyond giDispatch: the setting asking for it (`rayDrivenStages == 2`), the checkerboard variant compilation, and the denoiser running (`denoiseGiRanThisFrame_`). Tracing only half pixels is safe only when the denoiser reconstructs the other half. The `giCbParityWritten_` parity bit latches from `denoiseFrame_` (which alternates) and is handed to next frame's denoiser dispatch so the two agree by construction.

- Ray-driven visibility semantics: giHitShadowMap visits the GI shadow map from NonPixelShaderResource (compute-readable) back to ShaderResource (pixel-readable) within the barrier-free group, because voxelise and blended replay read it as PIXEL_SHADER_RESOURCE. The shadow history's read side (`t6` / `shadowHistRead`) is read by CSRdShadow's reprojection and CSRdLocalLights from COMPUTE state, then back to ShaderResource. Local lights history (`t19`) is similarly transitioned for the CSRdLocalLights compute stage.

- ReSTIR GI frame-midpoint timing: The GPU presents the previous real frame at the frame midpoint (`dev_->frameMidpoint()`) once the GPU reaches the end of the GI stage on Sponza (approximately 7 of 15 ms), placing it near the middle of the frame. The stages after this point are independent of the GI stage's output until the barriers below, so splitting the submission here changes nothing they see. This is critical for frame interpolation timing.

- Local lights history: The local-lights stage (`CSRdLocalLights`) only runs if there are lamps. It marks the history written only when the dispatch actually runs, tracking whether lamps exist in the scene. The reprojection and depth test addresses the lamp visibility history texel-for-texel with the shadow history, so they must have the same size and ping-pong together.

- ReSTIR GI surface history updates: The GI surface-normal history (`u8`) has two writers: `CSRdGi` writes hit pixels inside `giRestirIndirect`, and Stage B's miss branch writes the sky sentinel. Texels are disjoint, but the write order is stated explicitly rather than left implicit because both write to `u8`.

- Reflection temporal filtering sub-stage: Sub-stage C, R2 (CSRdReflFilter) reruns `rtReflectionSpatial` against R1's `gRtReflHistOut` write. R2 depends on R1's write to the SAME resource (not disjoint), so it needs a real barrier and gets its own always-on GPU stat span outside the barrier-free "Voxi RD lighting stages" group.

- GI view parameters bit 17 and 16: On a frame where CSRdGi traced GI at half rate, bits 17 and 16 of `cb_.viewParams[3]` persist for the REST of the frame. Scene draws after the ray-driven primary (PSMainVoxi's blended replay) read `cb_` through `sceneConstants()`, and `giRestirIndirect`'s denoiser-input write uses bit 16 to write only the traced half, so the denoiser's reconstruction never mixes a blended replay's value into a skipped pixel. `prePass` zeroes it next frame.

- Ray-traced shadow history lifecycle: All four textures (shadow, reflection, AO, GI surface pair) are destroyed and rebuilt together because mismatched sizes would corrupt reprojection. They share `rtHistWriteIdx_`/`rtHistValid_`, so they must agree on whether previous frame contents exist. When ray tracing is off (shipped default), these are released to save ~225 MB VRAM. The teardown is not a bare early-out, so toggling ray tracing off at runtime actually frees the memory.

- Surface history depth precision: The reflection history uses RGBA16F instead of 32F because HDR colour needs no full-float precision, halving bandwidth on a buffer already twice the shadow one's size. The GI surface position and normal histories are RG32Float instead of RGBA16F to preserve depth precision for the disocclusion test at clip-space w distances (tens of thousands on terrain).

- Sky occlusion history and hit distance: The `rtAoHitDist_` raw hit distance rests in UnorderedAccess and is never barriered, unlike the three ping-pong pairs. Nothing in Voxi reads this; it is written whole every frame by the only pass that touches it, and the eventual consumer (outside this file) makes its own transition when it exists. A round trip to ShaderResource would cost two barriers for no one.

- ReSTIR visibility half-resolution requirement: Half-resolution ReSTIR VISIBILITY history (`giVisHist_`) is only created while `giVisHistWanted()` is true, which is stricter than `giRestirWanted()`. Full/Reconstructed/NoRay modes never touch this pair. When allocation fails, the renderer falls back to Full-resolution with no extra plumbing because the shader's bit-4 decode runs Half as Full.

- Reservoir buffer reallocation strategy: The GI-restir reservoir buffer is reallocated only when it no longer fits or holds more than twice what the current size needs (`perPixelBufferNeedsRealloc` check). A small resize requires no new buffer, while render scale applied after start-up gives back the present-resolution allocation (e.g., 431.6 → 145.1 MiB at 0.58 of 3532x1987). The old buffer goes through `destroyBuffer` (retires it behind fence), and the new one is bound through the versioned binding set so in-flight frames read the old descriptor copy.

- Staged ray-driven resource structure: Unlike ensureShadowHistory, staged ray-driven resources have no ping-pong pair. Every texture is recreated outright on a size change, and every buffer is reallocated only past `perPixelBufferNeedsRealloc`'s 2x band. The row pitch is exactly the render target's width (one element per scanline times height). Tile count for the shadow probe tile buffer is ceil(W/8) x ceil(H/8), matching CSRdShadowProbe's dispatch math.


### Sun movement and ReSTIR history invalidity
Sun movement voids ReSTIR GI reservoirs because a reservoir keeps the radiance it was shaded with. Reused samples after a sun move would still carry the old sun's lighting, leading to temporally incorrect images. Measured on PTTest NewSponza (fixed camera and exposure, arcade wall, radiance x1000): sun angular change 71.7→47.2 degrees left yielding 12.2 luminance three frames later vs settled 1.8, taking 40–150 frames to recover even with denoiser off at all history depths. History void only on the frame of movement (rtHistSunJumped uses 10-degree threshold) to restart each pixel from fresh candidates under new sun. At reuse history 1: recovery 9.7/2.6/1.7 at +3/+17/+42 frames (was 12.2/8.7/3.8 before void).

### Ray-driven AO denoiser signal suppression
Under ray-driven primary visibility (PSRayDriven), the ambient occlusion denoiser signal is suppressed because:
- PSRayDriven sets denoisedAoUsable = false at the call site
- No opaque draw reaches PSMainVoxi (suppressesScene() blocks the raster pass)
- Glass reaches it via blended replay (D3D12 only) but gAverHistoryWrite is false there unless legacy bit 32 re-enables it
- Exception: GameWater's unauthored fallback (opaque material, blended draw) still reads gDenoisedAo, accumulating its own occlusion (pixel change requiring simulated fluids + volume with no .ocmat)

### ReSTIR GI surface history split
Position and normal histories are split into two separate textures not because the design wanted two, but because rhi::Format has no four-channel 32-bit float. Both textures swap on the same writeIdx/readIdx and must agree frame-to-frame; one logical surface split only for RHI format constraints.

### G-buffer under MSAA
Until 2026-10-05 the denoiser was disabled under MSAA because the single-sample G-buffer could not be bound beside the multisampled scene target. The D3D12 backend now writes multisampled twins and resolves them to the nearest sample (rhi.d3d12 notes); pickGbuf and the denoiser gate ask IDevice::gBufferWritten().

### Milestone 4 latch ordering
In beginShadowHistory, denoiseGiInputHalfRate_ and denoiseGiHalfRateParity_ latch LAST frame's half-rate state (giCbWrittenThisFrame_/giCbParityWritten_ from the previous frame's recordStagedRayDriven dispatch) before anything else, including the early return. The denoiser's dispatch reads these to know if the radiance it's filtering came from half-resolution traces. These state values must be latched before they're overwritten by this frame's dispatch, which runs later.

### Shadow filter radius measurements
Spatial filter for shadow rays: 49 taps (radius 3) measured at +0.02 ms cost on ElectricDreams 1600x900 vs one extra ray at +1.69 ms in the same batch. Radius is ~85x cheaper than the ray it replaces, making it a quality knob rather than a performance optimization. Pinned at cost 0 currently (loop runs but result discarded via constant) pending penumbra probe.

### Denoiser history lag and frame ordering
Denoiser dispatch over LAST frame's signals (rtAoHitDist_ and giRadiance_ not yet written this frame). Only ordering possible: signal targets haven't been written yet, so dispatch filters the previous frame's measurement. Same one-frame lag every temporal filter here already has (e.g. u4/t11 pair, other history textures).

### Visibility history independent of surface history
giVisHistValid_ and giVisHistPrimed_ track the sixth history pair (visibility, u10/t16) separately from surface history (positions/normals, u7/t12 + u8/t13). giVisHistWanted() is narrower than giRestirWanted(), so the visibility pair can start/stop independently. When both pairs run, ambientParams.w recomputed with both histBound=true and histValid=giVisHistValid_ (unknown initially, set only after beginShadowHistory confirms binding).

### Radiance cache (NeuRaC) supported only in staged compute
Cache runs only in staged ray-driven compute passes on D3D12 (CSRdGi and CSRdGiTrace with AVER_NEURAC=1, the only code that scatters/reads). Recording compute inside scene pass is Vulkan-illegal. Anywhere else, Cached silently behaves as HalfResolution with wire mode 2, logged once.

### Cached visibility mode 2 wire encoding
Cached mode (giRestirVisibility_=4) wires as mode 2 (HalfResolution) to shader, allowing `& 3u` decode to read it as HalfResolution. Bit 128 (neuracLive_, set by updateNeuRaC) added on top; only AVER_NEURAC twins read that bit, others ignore it.


- `rdStagedActive()`: Staged ray-driven passes extended by milestone 2 (rdGiCsPso_/rdSkyOccCsPso_) and milestone 3 (rdReflCsPso_). Each condition documents why it blocks a frame (reason parameter for console output).

- Blended draws and G-buffer: Blended draws never write the G-buffer (slots 1-3: velocity, viewZ, normal/roughness), leaving opaque surface's data for the denoiser. Previously, blended PSOs wrote these slots, and D3D12 applied RenderTarget[0]'s premultiplied blend to all targets, causing the pane's normal (w=0) to ADD to opaque, distorting reprojection and causing 30-frame history trails in the denoiser. Fixed by binding one-target pipelines (glass only) instead.

- `scenePipeline()` depth-prepass bug history: LessEqual/no-write twin is required for prepassed draws. FXC/SM 5.1 fallback caused 0.1% of pixels to be discarded because Less rejects equal depth, making prepassed draws appear invisible. Bug: ordinary pipeline still fell through when meshShaders=true for a prepassed draw, despite it already writing depth. Fix: backends now guarantee meshShaders=false for prepassed draws.

- Shadow creation: GI-only map (giShadowTex_) is a warning, not failure — affects bounce light only. Main shadow map (shadowTex_) is required.

- Injection accumulator: Four uints per voxel (r, g, b, fragment count) interleaved along x; R32_UINT is the only typed format D3D12 guarantees UAV atomics on. One z-slab deep per voxelizePass (voxelizationPass parameter).

- Voxel volume memory: Computed from texture descriptions, not queried (no RHI accessor exposes actual cost). Radiance: RGBA16F = 8 B/texel per mip. Accumulator: 4 B/texel = 16 B/voxel (four channels interleaved along x). mipDim() floors like backend mip-chain sizing.

- Binding set placeholders: Multiple resource categories use 1x1x1 or single-element placeholders until ensured by called-out functions (ensureAirVis, buildLocalLights, ensureShadowHistory, refreshFoliageBindings). This prevents null-fill on Tier 1 hardware until real resources exist. View outlives buffer rule: rebind placeholders BEFORE destroying old resources.

- Air visibility (occlusion-aware fog): t17/u16 bound to placeholder (1x1x1 R16F) until ensureAirVis() upgrades them. CSAirVis fills the real texture once before any shade pass reads it. airVisDirty_ flag tracks this necessity.

- Local lights: t18/t19/u19 bound to placeholders until buildLocalLights() or ensureShadowHistory() upgrade them.

- Foliage: t20/t21 bound to placeholders until refreshFoliageBindings() swaps in real buffers.

- Pipeline creation history: createScenePipelines() had a pipeline leak bug where overwriting member variables without destroying old handles caused one-per-resize leak. Fixed by explicitly destroying all stale PSOs at function start. Three major categories were affected: base scene variants, G-buffer twins, and ray-driven/staged-ray-driven compute pipelines.

- Instanced shadows: Requires SM 6.0 AND DXC (not FXC/SM 5.1). FXC failure was silent — per-instance transforms never arrived, rasterising with garbage world matrices. Gate oracle caught this via "shadow/sunlit must bracket the lighting" invariant. Refusal to build the PSO when preconditions fail is the whole fix (fallback to one-draw-per-instance already exists).

- Ray-driven variants: Single-pass PSRayDriven megakernel lost the device on AMD when all four voxi.rt* cost toggles were live (rtGiShadowBits). Staged compute and PSMainVoxi don't take these toggles; kRdSinglePassLamps rides the same define string across all four single-pass compiles.

- Textured ray-driven pipeline: Bindless texture table is a root-signature difference, cannot be a runtime toggle. Built only when both hardware AND ensureTextureTable() confirm descriptors are real; failures fall back to flat-albedo pipeline, itself falling back to rasteriser — three levels of fallback, none a black screen.

- Mesh-shader and RayQuery support: MSMain, PSMainVoxi with defines (AVER_MS, AVER_RT, AVER_GBUFFER) — optional variants. MSVoxel and RayQuery variants use same pattern; fallbacks exist if compilation fails.

- CSAirVis gating: Compiled against full Voxi layout (reads t0's mip chain via gVoxelSamp, writes u16). Requires SM 6.0, gated on instancedShadowsOk for clean fallback to placeholder when unavailable.

- Compute pipeline constant blocks: b3 (MipCB) — root constant block shared by CSClear/CSResolve/CSMip (8 dwords for dispatch box). All three must agree on size. Unset root constant block is this project's recorded TDR class — must set before every dispatch.

- Shader registration: Render pipelines declare material table, so shaders are told register locations. Compute pipelines must NOT be told (declare no second table).


- **Staged ray-driven pipelines architecture**: All staged compute/pixel shaders share `giTex` layout and `csDefs` defines to enable D3D12 root-signature deduplication across ray-driven pipelines (keyed on layout content, not compute-vs-graphics distinction). This is verified read-only in D3D12Device.cpp.

- **SM 6.6 requirement**: CSRdVisibility/CSRdShadow/CSRdGi/CSRdSkyOcc/CSRdRefl and their variants (split/checkerboard/split-checkerboard) require SM 6.6 because they use ddx/ddy derivatives in compute shaders (voxi_rt.hlsli, voxi_restir.hlsli). Compute-shader derivatives exist only from SM 6.6. The [numthreads(8,8,1)] layout forms 2x2 thread blocks matching pixel-shader quad neighbourhoods, so the filter sees the same derivatives as the colour pass would. Below SM 6.6, compile fails and rdStagedActive() keeps the single-pass path.

- **Staged variant purposes**: CSRdGiTrace is the trace candidate that CSRdGi used to run inline. CSRdShadowProbe tests one 8x8 tile at a time, querying the same surface and rtShadow footprint as csShadow. CSRdReflSplit writes a PENDING marker instead of composing; CSRdReflFilter finishes the compose same frame via rtReflectionSpatial. CSRdLocalLights reprojects like csShadow (same SM 6.6/layout); optional: a failure leaves lamps unlit (rdStagedActive() ignores if missing).

- **Textured ray-driven for glass**: Glass (blended) is routed through PSMainVoxi (scenePipeline with blended=true), not PSRayDriven. Without textured bindings in the ray-driven pipeline, gRtTextures doesn't exist, so rtReflection #ifdef compiles to the flat path and glass reflections stay Lambertian against a textured world. The textured RT-blended variant (sceneRtBlendedTexPso_) adds bindless to PSMainVoxi to fix this.

- **Blended depth state**: PSRayDriven's G-buffer twin (rayDrivenGbufPso_) uses Always/write-on like rayDrivenPso_, not derived from sceneGbuf's depth state, because a fullscreen pass has no prior depth to test against.

- **PremultipliedAlpha rationale**: Blended scene pipelines use PremultipliedAlpha (dst=src.rgb+dst.rgb*(1-a)), not straight AlphaBlend (dst=src.rgb*a+dst.rgb*(1-a)). AlphaBlend attenuates specular by alpha too, so a 0.2-alpha pane showed its own sky/GI reflection at 20% strength, making glass look like flat cartoon tint. A real dielectric transmits the background and reflects at full strength (two different weights). With PremultipliedAlpha, PSMainVoxi packs rgb=specular+diffuse*alpha, so only the diffuse lobe is attenuated here.

- **Blended in TLAS**: Blended draws are in the TLAS with mask kRtMaskTranslucent, flagged FORCE_NON_OPAQUE (so glass casts shadows). Excluded: voxelisation/shadow cascade/GI shadow map (depth-only by construction) and reflection/primary rays (by MASK via AVER_RT_MASK_OPAQUE; widening that mask is all it would take to let glass reflect glass). Glass still receives indirect light and shadows cast by opaque surfaces.

- **Depth prepass with LessEqual**: Colour-pass twins trust the prepass with LessEqual depth (not Equal), which is deliberate. With bit-identical geometry, nothing from the same instance can be strictly less than what the prepass found, so LessEqual behaves like Equal without a new CompareOp. Prepass reads gMaterialFlags before it knows whether a clip is needed, so opaque materials waste a branch+cbuffer read (far cheaper than shadow/cone-trace/fog).
