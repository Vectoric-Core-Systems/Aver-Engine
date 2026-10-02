// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
#pragma once
#include "aver/core/Types.hpp"

// Voxi's render-feature settings: anti-aliasing, global illumination, ray tracing, path tracing.
// Core-only, no RHI dependency: the host pushes device capabilities in and reads settings back out.
namespace aver::voxi {

#if defined(_WIN32)
#  if defined(AVER_VOXI_BUILD)
#    define AVER_VOXI_API __declspec(dllexport)
#  else
#    define AVER_VOXI_API __declspec(dllimport)
#  endif
#else
#  define AVER_VOXI_API
#endif

// Multisample count.
enum class Msaa : u32 { Off = 1, X2 = 2, X4 = 4, X8 = 8 };

// Shared quality ladder for the trace-based features. Off means "don't run this pass".
enum class Quality : u32 { Off = 0, Low = 1, Medium = 2, High = 3, Epic = 4 };

// The features Voxi owns settings for.
enum class Feature : u32 { Msaa = 0, GlobalIllumination, RayTracing, PathTracing, MeshShaders,
                          LayeredBsdf, Count };

// Whether a feature is usable: implemented and supported, declared but not implemented yet, or
// refused by the device.
enum class Status : u32 {
    Ready = 0,
    NotImplemented,
    Unsupported,
};

// What the host's GPU can do. Pushed in by the runtime; mirrors rhi::DeviceCaps.
struct DeviceInfo {
    u32 msaaMask = 1;         // bit N set => N samples supported
    u32 maxMsaaSamples = 1;
    u32 rayTracingTier = 0;   // 0 none, 10 DXR 1.0, 11 DXR 1.1
    bool computeShaders = false;
    bool typedUavLoads = false;
    bool conservativeRaster = false;
    u32 shaderModel = 50;      // 60 = SM 6.0, 65 = SM 6.5
    u32 meshShaderTier = 0;    // 0 = none, 1 = Tier 1
    bool dxcAvailable = false; // DXIL compiler present
    // The denoiser can run on this backend (see RenderSettingsResolver.hpp's
    // DisableReason::RequiresDenoiserBackend): it reads the G-buffer, which only D3D12 provides.
    // Computed by the host as `backend() == rhi::Backend::D3D12` at the same two call sites
    // (SandboxApp.cpp, GameApp.cpp); this struct only carries the answer so it stays free of any RHI
    // dependency.
    bool denoiserSupported = false;
};

// The renderer quality settings, as requested. Clamped to the device by Renderer::setSettings.
struct Settings {
    Msaa    msaa               = Msaa::X4;
    Quality globalIllumination = Quality::Medium;
    // ON BY DEFAULT AT MEDIUM; rtShadowRays/rtPixelsPerRayTile below are Medium's rungs by
    // construction (see those fields).
    //
    // MEASURED (Release, ElectricDreams, windowed 1600x900, --no-vsync, --frames 200, whole-frame
    // median): 11.86 ms with --no-rt, 18.44 ms at Medium's rungs (+55%, the cheapest honest way to
    // have ray-traced sun shadows at all); the naive version (flip the tier, leave knobs at Epic's
    // 4 rays) measures 23.39 ms, ~2x Off. One GPU (RX 7800 XT), one window size -- the relative
    // ladder should hold elsewhere, the absolute ms won't.
    //
    // BENCHMARK TRAP (fallen into twice): --project is POSITIONAL, not a flag (`Sandbox.exe
    // --project <path>` silently opens nothing), and a CREATEDWITH mismatch raises a blocking modal
    // -- both give a clean, empty-editor "benchmark" with plausible numbers and no error. Check the
    // log for "scene walk ... over 0 entities" before trusting any number here; honest baseline is
    // 14 entities, ~18 ms.
    //
    // GI RUNAWAY TRAP (cost an afternoon to bisect): the GI gather has no clamp, so bright light on
    // large saturated surfaces can flood the frame with their colour (three 1.8 m pure-red spheres
    // under a 100,000-lux sun did this; RT merely exposed it by lighting them brighter than the
    // voxel-cone path did -- scaling the spheres down fixed it). Scene defect, but answering with a
    // red screen instead of a clamp is a tracked renderer bug, not hardware-specific.
    Quality rayTracing         = Quality::Medium;
    Quality pathTracing        = Quality::Off;

    // A layered BSDF alongside the standard BRDF (not instead of it): Off is today's
    // metallic-roughness Cook-Torrance response byte for byte; higher rungs add a coat lobe (and
    // later, further layers).
    //
    // Quality, not bool, because a coat's own GGX + split-sum env term is not free -- projects need
    // the cheap (Off) option; Off is not a broken feature, it's a real supported default, same
    // status as pathTracing's Off.
    //
    // NOT LIVE-SWITCHABLE: VoxiRenderer compiles ~20 raster PSOs at init with no disk cache;
    // compiling a second matrix for layered would double that for every project, even ones that
    // never enable it. Read once before pipelines build; changing it needs a project reload -- free
    // when Off matters more than instant.
    Quality layeredBsdf        = Quality::Off;
    bool    meshShaders        = false;

    // Cubic voxel grid edge; memory and per-voxel GPU cost are O(this^3). Defaults to Medium's rung
    // (128). Derived from globalIllumination on a tier change when left unchanged (see
    // voxelResolutionForQuality/setSettings); set explicitly, to a value different from what's
    // currently active, in the same call to override.
    u32 voxelResolution = 128;

    // Total cones the diffuse gather traces, including the axial one. Defaults to Medium's rung (6),
    // derived from globalIllumination on a tier change like voxelResolution above.
    //
    // THE GI SETTING THAT ACTUALLY COSTS: volume BUILD (voxelResolution/giUpdateInterval) measures
    // 0.4-0.5 ms; per-pixel GATHER measures 1.3 ms and was hardcoded to six regardless of tier, so
    // turning GI down bought nothing either, and raising it to High made the frame SLOWER with no
    // way to spend the budget (6.0 -> 6.4 ms, bigger volume, same sample count). Cost is
    // LINEAR here, ~0.22 ms/cone (24-step march bound unreachable at diffuse aperture -- cones exit
    // early), so this is the one GI number worth laddering.
    //
    // Ladder: Low 3, Medium 6, High 9, Epic 13 (giConesForQuality in Voxi.cpp; measured FirstPerson
    // Low 3.3 ms - Epic 5.4 ms, 0.21 ms/cone, confirms the figure above). A "two cones moved a probe
    // by 2/255" claim elsewhere in this tree predates this ladder and was never re-measured against it.
    u32 giCones         = 6;
    // Sky-visibility rays the AMBIENT term traces per pixel (--gi-sky-occlusion-rays N). 0 = estimate
    // from the cone gather -- optimistic in enclosed geometry (widened cones see through thin walls:
    // Sponza shadowed pixels read [25,26,30] vs path-traced [7,7,7], blue-biased by leaked sky).
    // Under ReSTIR GI (no cone gather) 0 means NO occlusion. Attenuates only the SKY term; bounced
    // light is the GI estimator's.
    //
    // Derived from rayTracing on a tier change: 0 at Low (it rasterises), 1 at Medium/High/Epic
    // (default 1, Medium's rung). Accumulated against a reprojected history (rtSkyOcclusionTemporal)
    // rather than retraced every frame -- cosine-distributed so lanes walk unrelated BVH nodes,
    // unlike the coherent sun rays (~0.017 ms each).
    //
    // Sweep (Sponza, "Voxi ray-driven primary"): 0 rays 10.92 ms/[20,20,22], 1 ray 12.25 ms/[11,11,13],
    // 4 rays 15.58 ms/[11,11,13] -- first ray buys the correction, more buy nothing. Medium at the
    // default half rate: +0.5 ms, frame mean 33.4 -> 16.1 (Epic 16.2).
    u32 giSkyOcclusionRays = 1;
    // Edge, in pixels, of the square SHARING one sky-occlusion ray direction. 1 = fresh rotation per
    // pixel (pre-dial behaviour).
    //
    // WHY: the ray is cosine-distributed, so neighbouring lanes descend unrelated BVH nodes and the
    // wave runs at its unluckiest lane's speed. Sharing azimuth across a tile makes lanes trace
    // near-PARALLEL rays touching the same nodes/cache lines. MEASURED (Sponza, marginal cost of 1->4
    // rays): tile 1 +3.56 ms, tile 2 +2.85 ms, tile 4 +2.21 ms -- a 38% cut, funding the extra samples.
    //
    // PRICE: CORRELATED noise inside a tile rather than independent per-pixel noise -- right for
    // low-frequency AO, wrong for anything sharp (do not reuse for shadows/reflections).
    //
    // Runtime, not a #define (was compile-time-only, unsweepable, same gap giSkyOcclusionRays had).
    // Rides gAmbientParams.y, a row already reserved for it.
    u32 giSkyOcclusionTile = 1;
    f32 giIntensity     = 1.0f;
    // Caustics: how strongly light focused by a water surface brightens what's beneath. 0 = off (the
    // shader's own branch then costs nothing measurable). Not on the quality ladder -- it's a LOOK,
    // not a fidelity rung: caustics at Low and none at Epic would just be the same scene lit
    // differently, not a fidelity step.
    f32 causticStrength = 0.6f;
    f32 giMaxDistance   = 4000.0f;  // centimetres

    // GI radiance ceiling before tonemap; mirrored to the shader as AVER_VOX_MAXRAD (voxi.hlsl /
    // voxi_gi.hlsli) via FrameConstants::viewParams.y. 16.0 is not headroom -- acesTonemap
    // (rhi/shaders/color.hlsli) is already flat white by x=4-5, so anything pinned here paints solid
    // white. Shared by the raw ReSTIR GI estimate, its denoised readback and the cone-gather
    // estimator. DEFAULT MUST STAY 16.0: every image this renderer has produced was already clamped
    // there as a compile-time #define; change only via a project or voxi.giRadianceCeiling.
    // Lowering it can fix a poisoned-but-finite white patch, but also dims a legitimate bright bounce
    // near 16 -- indistinguishable from this number alone, so voxi.giPoisonView marks a ceiling HIT in
    // its own colour instead (red/green ReSTIR, voxi_restir.hlsli; violet specular, voxi.hlsl -- B1/F5,
    // not giMode-gated; magenta/cyan/yellow/orange/blue are the separate isnan/isinf guards).
    f32 giRadianceCeiling = 16.0f;

    // ---- refraction: how a translucent surface BENDS what is behind it ----
    // Absorption (attenuationColor) decides what COLOUR survives a medium; refraction decides where
    // it comes FROM -- independent (glass is green from iron, distorts from ior != 1). Reachable only
    // because the blended pass now has the scene behind it as a texture.
    //   Off (0)          background sampled straight through -- pre-existing behaviour.
    //   ScreenSpace (1)  offset by the refracted view direction x ray-measured thickness; nearly free
    //                    (reuses the backdrop copy) but limited to what the camera saw -- can reach
    //                    off-screen or the front of the glass (refractionEdgeFade hides that).
    //   RayTraced (2)    a refracted ray through the TLAS, hit point projected back to screen; fixes
    //                    the geometry at the cost of a ray on the frame's bottleneck path.
    // refractionForQuality derives this from rayTracing on a tier change (Off->Off, Low/Medium->
    // ScreenSpace, High/Epic->RayTraced); `= 1` here must equal Medium's rung or the derivation never
    // fires by default.
    u32 refractionMode     = 1;
    // Multiplies the offset. 1.0 is the physical bend for the material's own ior; below that trades
    // correctness for calm, above it exaggerates. A knob rather than a constant because the honest
    // answer depends on how thick the authored geometry is relative to the scene.
    f32 refractionStrength = 1.0f;
    // How far from the screen edge the offset is faded out, as a fraction of the smaller dimension.
    // 0 disables the fade and lets the artefact show, which is occasionally what you want to see.
    f32 refractionEdgeFade = 0.15f;

    // ---- ray-traced sun shadow: rays per trace, and how many pixels amortise one trace ----
    // Occlusion rays per pixel when this pixel traces this frame. Clamped to
    // [1, VoxiRenderer::kMaxShadowRays].
    // Derived from rayTracing on a tier change (ladder::rtShadowRays, QualityLadder.hpp; see it for why
    // High/Epic moved up from 2/4): Low 1, Medium 1, High 4, Epic 8. Default is 1 because the default
    // TIER is Medium and the derivation only fires on a tier CHANGE -- a struct default that disagrees
    // with its own tier's rung is never reached (this field once defaulted to 4 under rayTracing=Off,
    // so switching RT on by default would silently have run Epic's count under Medium's name). Every
    // tier-derived field below shares this same constraint; only pointed back to here from now on.
    u32 rtShadowRays = 1;
    // Edge length of the square tile one traced shadow pixel is amortised over via temporal history:
    // 1 = every pixel traces every frame (bit-identical to no denoiser); N>1 = one pixel per NxN tile
    // traces per frame, rotating so each gets a turn every N*N frames, others reuse a reprojected
    // history sample. Must be a power of two (VoxiRenderer::setPixelsPerRayTile rounds to nearest), so
    // the per-pixel schedule is a bitmask against pixel coords rather than a modulo, and N*N stays a
    // clean power of two (1/4/16/64/256 for edges 1/2/4/8/16). Clamped to
    // [1, VoxiRenderer::kMaxPixelsPerRayTile]. Governs only the RT sun-shadow/reflection history while
    // rayTracing != Off; the voxel cone-trace GI cost is separate (giUpdateInterval).
    // Derived from rayTracing on a tier change: 1 at every rung -- no temporal denoising anywhere, the
    // honest default for a renderer under evaluation (a temporal denoiser hides its own artefacts as
    // readily as the tracer's).
    // MEASURED (Release, ElectricDreams, 1600x900, --no-vsync, --frames 200, whole-frame median):
    // rays1/tile4 18.26 ms, rays1/tile2 18.49 ms, rays1/tile1 18.44 ms, rays2/tile1 19.99 ms,
    // rays4/tile1 23.39 ms, vs 11.86 ms with --no-rt. The three tile widths at one ray span only
    // 0.23 ms (noise); one ray to four costs 4.95 ms -- amortisation saturates immediately, ray count
    // is where the money is.
    // LOW WAS 4 (widest amortisation the still-camera table justified), but a still camera can't see
    // what temporal amortisation spends under motion: shadows visibly trail the caster. Wobbling-camera
    // diff: tile1 vs tile4 differs only 0.14 ms moving / 0.09 static (noise), while the trail is 0.80%
    // of pixels over threshold at tile1 vs tile2 alone (tile2->tile4 adds only 0.04% more) -- fully
    // present by tile2, no partial-credit rung available; Low was the one tier that shipped this trail.
    // Low is 1 now for the same reason Medium is: the frame time was never real.
    // giUpdateInterval/voxelResolution keep Low's wider rungs since those cost real measured time
    // under motion with no visible artifact at any width tested.
    u32 rtPixelsPerRayTile = 1;

    // How many frames apart the GI volume is re-voxelised: 1 revoxelises and re-filters every frame
    // (the original always-fresh behaviour). N>1 reuses the previous frame's volume for N-1 frames,
    // amortising voxelizePass/filterMips at the cost of indirect light lagging scene changes by up to
    // N-1 frames -- a latency trade, not a resolution one. Clamped to [1, kMaxGiUpdateInterval].
    // Derived from globalIllumination on a tier change (ladder::giUpdateInterval, QualityLadder.hpp):
    // Low 4, Medium 2, High 1, Epic 1. Default 2 since the default tier is Medium (see rtShadowRays
    // above). Set it explicitly in the same call that changes the tier to override the derived value.
    // MEDIUM MOVED 4 -> 1 (4 is what makes lighting trail the camera) -> 2. An earlier revision claimed
    // interval 1 left 187.9 ms on the table against 104.5 ms at interval 4 on a still camera;
    // re-measured, intervals 1/2/4/8 gave 18.54/18.47/18.50/18.46 ms -- noise on a STILL camera, so
    // that old figure is refuted (quoted here rather than deleted). Under a WOBBLING camera the
    // "Voxi GI update" span alone went 36.00 ms at interval 1 to 16.57 ms at interval 4
    // (aver-gi-update-dominates-under-motion.md; Sponza, --cam-wobble 15 50, ray-driven + AverSR
    // Balanced) -- real motion cost, though that run also used RT-shadow tile 4 and beginShadowHistory
    // sits inside the same measured span (VoxiRenderer.cpp), so crediting the whole gap to this field
    // is UNCONFIRMED, and lag AT interval 4 is itself UNMEASURED (the note this evidence comes from
    // says so directly, "both untested").
    // LOW IS NOW 4 (not 8) on that motion evidence; 8 is UNMEASURED either way. MEDIUM IS NOW 2,
    // UNMEASURED FOR BOTH COST AND LAG AT THIS RUNG: at interval 2 the volume rebuilds every other
    // frame, so lag is at most one frame, and a still scene converges identically to interval 1
    // either way (setSettings' own derivation comment, Voxi.cpp) -- this narrows rather than reverses
    // the 4->1 move (4 still trails visibly), asking whether Medium (the tier most projects run) should
    // pay interval 1's cost for an unshown lag.
    u32 giUpdateInterval = 2;

    // ---- WHICH ESTIMATOR ANSWERS THE DIFFUSE BOUNCE: the voxel cone gather, or ReSTIR GI ----
    // 0 = cone gather (DEFAULT, and every build before this field existed). 1 = ReSTIR GI: one traced
    // candidate per pixel, reused via Aver's own spatio-temporal resampling (giSpatioTemporalReuse
    // in voxi_restir.hlsli, GiReservoir in voxi_reservoir.hlsli; see giRestirIndirect in voxi.hlsl
    // -- formerly NVIDIA RTXDI, replaced by in-house code written from the published papers). One
    // fused pass: the fresh candidate, one temporal tap and up to 8 spatial taps
    // (reuse.numSamples), all read from last frame's reservoir slice.
    // NOT ON THE QUALITY LADDER (unlike giCones/voxelResolution/giUpdateInterval, which scale ONE
    // estimator): this SWITCHES estimators -- deterministic clipmap march vs. stochastic ray +
    // temporal reuse (far less per-frame tracing noise, at the cost of a biased, history-dependent
    // estimate that can lag a moving light or disoccluding camera), not a Low-to-Epic rung.
    // setSettings only clamps/range-checks it (Voxi.cpp), never derives it.
    // DEFAULT IS 0 AND MUST STAY 0: VoxiRenderer::giRestirWanted() gates the actual switch (also
    // requires RT hardware AND the rayTracing tier AND the globalIllumination tier on; with no RT a
    // project never allocates the reservoir buffer or the previous-surface history this needs).
    // THE SHADER READS THE EFFECTIVE VALUE, NOT THIS RAW FIELD (an earlier revision of this comment
    // said shader call sites "branch on this value directly", which is stale): giMode_ is read only by
    // giRestirWanted() (VoxiRenderer.hpp), which resets gGiRestirParams.x to 0 every frame and gates
    // whether it is set to 1 (the constant PSMainVoxi/PSRayDriven actually branch on) this frame
    // (VoxiRenderer.cpp). A value the device/tier can't honour is never seen by a shader, which is
    // why it's stored exactly as requested rather than clamped -- RenderSettingsResolver.hpp's
    // resolve() computes the effective value the UI/console show. Unlike voxelResolution/giCones,
    // this default is "byte-identical to every image before ReSTIR GI existed", independent of
    // globalIllumination's tier.
    u32 giMode = 0;
    // ---- ReSTIR GI VISIBILITY: how much of F2 (candidate-hit sky) and F3 (reuse visibility) -- the
    // contrast fix's two per-pixel rays, cb4b48df -- each globalIllumination rung pays for ----
    // NoRay restores cb4b48df's pre-fix over-brightness; Reconstructed replaces both rays with one
    // voxel-cone march the diffuse gather already pays for (no extra ray); HalfResolution traces
    // exact visibility on 1-in-4 pixels/frame, reconstructing the rest from a depth/normal-aware
    // neighbourhood (falls back to tracing when invalid, so worst case = Full's cost); Full traces
    // every pixel every frame (today's behaviour).
    // See ladder::giRestirVisibility (QualityLadder.hpp) for per-rung reasoning.
    // Derived from globalIllumination on a tier change like giCones/voxelResolution/giUpdateInterval;
    // default 2 (HalfResolution) since the default tier is Medium (see rtShadowRays above).
    // Composes with, not replaced by, the legacy bits: voxi.legacyRestirHitSky/
    // legacyRestirReuseVisibility (console-only, never persisted) force NoRay for their OWN ray
    // regardless of this field -- a set legacy bit always wins for that ray, no ownership collision.
    // Stored exactly as requested, resolved at read time (like giMode); Resolution::
    // giRestirVisibility.effective deliberately EQUALS requested always -- see that field's comment
    // for why the usual resolve-to-clamped rule would be wrong here.
    enum class RestirVisibility : u32 { NoRay = 0, Reconstructed = 1, HalfResolution = 2, Full = 3 };
    u32 giRestirVisibility = 2;   // must equal ladder::giRestirVisibility(Quality::Medium)

    // ---- BISECTING THE SAME FADE FROM THE OTHER SIDE: SPLIT REUSE APART, THEN TIGHTEN IT ----
    // OPEN: ReSTIR GI reads brighter while moving, settling darker over ~1s after stopping. Ruled
    // out: auto-exposure, the denoiser (NRD then), sky-occlusion rays, the F2 voxel bounce, voxel rebuild rate, Half vs
    // Full visibility, the spatial-reuse motion discount (3dbc9a42, reverted 8daed7f1), reservoir age
    // and the moving-camera history cap (both measured WORSE). What removes the fade: giRestirMaxHistory
    // 0, and voxi.debugResetHistoryEveryFrame 1 (c08c76d2), which clears the reservoir history every
    // frame -- disabling BOTH temporal and spatial reuse at once (spatial neighbours are read from
    // the same reservoir slice the temporal tap reads). So the carrier is reuse itself; open
    // question here and below: which half, and whether the reuse tolerances (giIsSimilarSurface,
    // literals 0.1 relative depth / 0.5 normal cos in voxi_restir.hlsli) are simply too loose.
    // 15 = AUTO (today's motion-discount numSamples, unchanged -- byte-identical image, fade included).
    // 0 disables spatial reuse outright (temporal only -- isolates whether a fade is spatial). 1..8
    // pin the tap count regardless of motion, overriding the discount's lerp(2.0,1.0,motionT);
    // clamped to 8, the ceiling the fused pass was stability-tested against (that lerp's K*M margin
    // analysis, voxi_restir.hlsli).
    // Packed at gAmbientParams.w bits 12-15 (four bits, not three, since 15 must be a value no real
    // 0..8 count collides with) -- see givis::packAmbientW (GiVisibility.hpp), shared byte-for-byte
    // with voxi_restir.hlsli's own pack/decode.
    // Debug/tuning only, like the two thresholds below: no manifest key, no Settings UI -- a
    // bisection tool, not a shipped dial. Console: voxi.giRestirSpatialSamples.
    u32 giRestirSpatialSamples = 15;

    // ---- WHAT THE CAPTURES NARROWED IT TO: THE WEIGHTING WHILE A RESERVOIR IS YOUNG ----
    // Measured headless on Sponza (camera translating, stopped at a known frame), viewport mean
    // luminance at +3 frames vs settled: baseline 0.0965 -> 0.0892 (+8.2% too bright, gone by ~+25
    // frames); tightened reuse tolerances identical (neighbour test innocent); spatial reuse off
    // still +6.9% (not the carrier); moving-age cap made it WORSE (+24%). Decisive: reuse off
    // entirely sits at 0.0889, the SETTLED value -- a partially-converged reservoir reads brighter
    // than both the no-reuse and converged estimates: a weighting error while M is small, not stale
    // radiance. maxHistory is the knob over that weighting (a former bias-correction mode
    // knob no longer exists; giFinalizeWeight's MIS normalisation is fixed).
    // maxHistory: reuse.maxHistory, cap on the M a neighbour reservoir carries into the combine;
    // 1 was the old value (602d1b06 lowered it from 8 to kill a load-time overshoot). Console-only,
    // default reproduces today's image; packed at gAmbientParams.w bits 18-22.
    // DEFAULT 0 IS THE CAMERA-MOTION FADE FIX: 1 (old default) overshoots +8% and decays over ~25
    // frames (the fade); 8 overshoots +104%; 0 does not overshoot. Everything else is innocent:
    // spatial half, reuse tolerances, the (since removed) bias-correction mode, the Jacobian,
    // reservoir age (capping it made it WORSE: +24%, or +62% while moving only) -- every restart
    // re-forms the chain from single-sample reservoirs whose RIS weight has huge variance, which is
    // what flashes. WHAT 0 COSTS: nothing detectable -- settled brightness unchanged (0.0893 vs
    // 0.0892), grain/flicker at rest identical (0.00597/0.00057 vs 0.00594/0.00056), mid-motion
    // slightly better, moving image sits at settled brightness instead of 5% above it: the denoiser
    // (NRD when measured) already supplies the smoothing this reuse was meant to provide. 1 restores the old behaviour for A/B.
    // Console: voxi.giRestirMaxHistory. Packed at bits 18-22.
    u32 giRestirMaxHistory = 0;

    // ---- THE DENOISER OVER THE SKY OCCLUSION AND THE ReSTIR GI RADIANCE ----
    // AMD FidelityFX Denoiser (MIT) through Aver.Render.Denoise -- see modules/render.denoise.
    // Off by default: ON is a real cost the user chooses, not one a denoiser helps itself to. It
    // needs the thin G-buffer written (velocity, view Z, normal/roughness -- three more targets,
    // ~54 MB at 1080p) that nothing else in this engine turns on; this field is that agreement.
    // Requires MSAA 1 -- D3D12's rule: every target in one OMSetRenderTargets call shares a sample
    // count, and the G-buffer's three are always single-sample, so above 1x they clear without
    // writing and every denoiser input is blank. A denoiser fed blank inputs doesn't fail -- it
    // returns a confident, uniformly wrong image -- so VoxiRenderer skips the pass and warns once at
    // WARN; MSAA 8x makes this a no-op (the UI says so next to the checkbox).
    // D3D12 only, because the G-buffer it reads is.
    bool denoiser = false;

    // ---- The denoiser's live dials (render::denoise::Denoiser::Tuning) ----
    // Applied every frame the denoiser records, so a console change takes effect next frame with no
    // teardown and no history loss.
    // History length: the cap on each pixel's accumulated sample count. Longer is smoother and
    // slower to follow a change. [1, 255]; 32 is FidelityFX's own reference value. Console:
    // voxi.denoiserMaxSamples.
    u32   denoiserMaxSamples = 32;
    // How tightly the reprojected history is clipped to this frame's neighbourhood statistics before
    // it is blended in: smaller rejects stale history sooner (less ghosting, more noise), larger keeps
    // more of it. (0, 4]; 0.5 is FidelityFX's own reference value. Console:
    // voxi.denoiserHistoryClipWeight.
    float denoiserHistoryClipWeight = 0.5f;
    // Caps the history length on every frame the sun moves and the one after. Under NRD at full
    // history ReSTIR GI kept the old sun's bounce light for ~1s after a drag stopped (MEASURED on
    // NewSponza, 40-deg azimuth drag @1deg/frame vs settled: mean +3.6 on 14.6 one frame after,
    // +1.1@26 frames, denoiser off +0.25), so the history restarts short under the new sun and regrows
    // by one sample a frame -- the drag stays denoised, just less smoothly. A value >=
    // denoiserMaxSamples turns it off. Not re-measured with this denoiser. Console:
    // voxi.denoiserSunMovingSamples.
    u32   denoiserSunMovingSamples = 4;

    // SPATIAL denoise radius for the ray-traced sun shadow, in pixels. 0 = off (unfiltered per-pixel
    // rays); N>0 averages a (2N+1)^2 neighbourhood of the shadow history, weighted by depth agreement
    // with this pixel's surface plane. Default 2 -- see WHY 0 WAS ONCE THE DEFAULT below.
    // NOT THE SAME AS rtPixelsPerRayTile above: that amortises over TIME (a reprojected value from
    // frames ago -- converges still, falls apart moving; a soft penumbra collapses to flat
    // fully-shadowed under ~1deg yaw over 40 frames). This averages over SPACE with no history, so
    // nothing goes stale or gets poisoned by motion. Independent, combinable, but fail differently.
    // THE PROBLEM IT IS FOR: at one ray/pixel (Low, Medium) the shadow term is a hard 0 or 1 --
    // dithered, not soft. Probe whose converged answer is 34,36,40: one ray reads 61,59,59 forever (a
    // pure function of the pixel); 16 rays reach 34,36,40 but cost 30.55 ms against 18.66. Averaging
    // neighbours is cheaper: rtShadow jitters the ray ORIGIN across the pixel footprint, so neighbours
    // already sample different parts of the same receiver and their mean is a real area estimate.
    // WHY 0 WAS ONCE THE DEFAULT: while rasterisation (PSMainVoxi, 4x MSAA) was the default
    // primary-visibility path, smoothing an already-antialiased image was redundant polish, not a fix.
    // RAY-DRIVEN PRIMARY VISIBILITY (rtRenderMode) CHANGED THE TRADE AT MEDIUM+ -- Low stays the
    // exception (D3; ladder::rtRenderMode), still rasterising at 4x MSAA -- Low keeps its one shadow
    // ray/pixel, same count as Medium, just fired from PSMainVoxi rather than the ray-driven shader,
    // so MSAA still resolves its dithering; only the primary-visibility method changes at Low. The
    // ray pass itself has no per-triangle coverage, so at Medium/High/Epic it runs single-sample with
    // nothing softening the hard 0/1 any more, and this filter (+0.02 ms at the widest rung vs +1.69
    // ms for one more traced ray, VoxiRenderer.cpp) fixes that to within one code of a sixteen-ray
    // reference on a still camera. ACCEPTED IN EXCHANGE: the gather centre reprojects through LAST
    // frame's camera to stay aligned with the shadow history (rtShadowSpatial, VoxiShaders.hpp), so
    // under motion it can walk off the true surface -- six-degree wobble measured 12-32 codes extra
    // darkening, growing with radius. Bounded (unlike the old rtPixelsPerRayTile Low=4's unbounded "collapses to flat"), but
    // invisible to a benchmark that never pans -- already burned twice by that blind spot
    // (giUpdateInterval's lag, that old Low=4 rung). Any future change here needs a MOVING-camera
    // probe.
    // Derived from rayTracing on a tier change: Low 2, Medium 2, High 1 (was 2), Epic 1 (ladder::
    // rtShadowDenoise, QualityLadder.hpp). Default 2 since the default tier is Medium (see
    // rtShadowRays above) -- a trap this file has already fallen into in both directions with
    // giUpdateInterval.
    u32 rtShadowDenoise = 2;

    // ---- ray-driven rendering ---------------------------------------------------------------
    // WHICH THING FINDS THE FIRST SURFACE: 0 = the rasteriser (every version before this setting
    // existed), 1 = a primary ray per pixel. Everything downstream is unchanged -- PSMainVoxi already
    // traces the shadow, evaluates the material and traces a reflection in ONE invocation
    // (VoxiShaders.hpp:781-826), so this swaps out the one stage that was still fixed-function.
    // MEASURED BEFORE IT WAS BUILT: ElectricDreams, 4x MSAA, 2750x1639, Release -- raster primary
    // visibility + shading is 9.2 ms of `scene draw` with RT/GI off, one extra shadow ray costs 1.6 ms
    // at the same resolution; a primary ray had to fit inside that gap to be worth having.
    // 1 FROM MEDIUM UP; LOW RASTERISES (D3, retuned) -- BY EXPLICIT PRODUCT DECISION ("the Wavefront
    // Primary rays model"), not a leftover experiment. Evidence for Low is partial: at overview
    // cameras raster is slower (ElectricDreams 20.55 vs 8.05 ms; PTTest w/ Path Tracing off, 13.51 vs
    // 8.04 ms); a close-up case once favouring raster (9.02 vs 24.04 ms) is UNCONFIRMED -- that run
    // also had Path Tracing on, silently taking over. D3 stands regardless, as the decision made with
    // this evidence, not a claim raster is faster at Low. See ladder::rtRenderMode/
    // rtRenderModeForQuality (QualityLadder.hpp) for why Off and Low both answer 0, for different
    // reasons: Off because there is no RT hardware path to assume, Low because the product decision
    // deliberately excludes it.
    // WHAT DEFAULTING TO IT TRADES AWAY:
    //   - HARDWARE EARLY-Z: rasterisation discards an occluded fragment before its shader runs, free.
    //     A ray pays full BVH traversal to discover the same hit was hidden, every pixel, every frame.
    //   - MSAA: the ray pass is one fullscreen triangle -- no per-triangle coverage, so it always runs
    //     single-sample vs. the raster path's 4x default; visibly noisier, independent of the RT
    //     sun-shadow speckle documented elsewhere.
    //   - TEXTURE: the primary ray returns flat albedo per instance; a rasterised frame samples one.
    // Applies to everyone by default now, not only whoever went looking for a switch.
    u32 rtRenderMode = 1;

    // ---- staged ray-driven passes (milestone 1 split; milestone 4 adds half-rate GI) -----------
    // PSRayDriven is still ONE fullscreen pixel shader tracing the primary ray, reconstructing the
    // surface, and running the sun-shadow ray, ReSTIR GI, reflections, sky occlusion and shading in a
    // single invocation. This field picks WHICH SHAPE that work runs in -- 0/1 compute the same thing;
    // 2 deliberately changes the image.
    // 0 = SINGLE PASS (baseline and fallback): the one drawFullscreen. 1 = STAGED: split
    // into a visibility compute pass (traces the primary ray, writes a per-pixel record), a shadow
    // compute pass (reconstructs the surface, runs the sun-shadow ray), then PSRayDriven reading both.
    // D3D12 ONLY in this milestone -- falls back to single pass and logs once if a pipeline fails to
    // compile, resources are absent, a non-textured PSO is in use, or the backend isn't D3D12, so an
    // opted-in project never silently renders nothing.
    // 2 = STAGED + HALF-RATE GI (milestone 4): same staged path as 1, but ReSTIR GI traces only HALF
    // the pixels/frame (a checkerboard) and the denoiser reconstructs the untraced half -- trades GI
    // quality/latency for speed. Only differs from 1 while giMode==1 AND denoiser is actually
    // denoising (voxel cone gather has no GI stage to checkerboard; without the denoiser nothing
    // fills the untraced half), so 2 behaves as 1 in either case (logged once). Same restriction/fallback as 1.
    // DEFAULT 2 since 2026-09-27 (was 0). MEASURED on NewSponza (RX 7800 XT, 3532x1987 capture,
    // whole-frame GPU ms): gallery single 23.5 / staged 14.1 / half-rate 12.7; court 28.4 / 15.6 /
    // 14.2. Image: staged vs single MAD 0.16-0.17 (same image); half-rate vs staged at fixed exposure
    // MAD 0.22-0.24 still (-0.4%), 0.65-0.84 in motion (noise, not bias). A project's RENDER.RDSTAGES
    // still wins; only projects without the key take this default.
    // NOT TIER-DERIVED, like giRestirMaxHistory below. Meaningful only while rtRenderMode resolves
    // to primary rays (Resolution::rayDrivenStages). Console: voxi.rayDrivenStages.
    u32 rayDrivenStages = 2;

    // DIAGNOSTIC ONLY: times each staged lighting pass (shadow, GI, sky occlusion, reflections) in
    // its own GPU span with a UAV barrier after it, in place of the one "Voxi RD lighting stages"
    // span they normally share. The barriers stop the four overlapping, so the per-stage sum reads
    // somewhat HIGHER than the shared span -- this answers "which stage costs what", never "what
    // does the frame cost". No effect on the image. Console: voxi.rayDrivenStageTiming.
    bool rayDrivenStageTiming = false;

    // SUB-STAGE SPLIT A: the sun-shadow trace, in two passes. MEASURED (staged mode 1, Epic): the
    // shadow stage costs 4.47 ms of a 19.6 ms frame tracing rtShadowRays (8 at Epic) per non-sky
    // pixel -- but most of a frame is fully lit or blocked, where all rays would agree. CSRdShadowProbe
    // traces ONE ray per 8x8 tile first; CSRdShadow ORs its tile's 3x3 neighbourhood and skips
    // per-pixel rays wherever every probe agrees. NEAR-IDENTICAL IMAGE, not a quality trade (unlike
    // rayDrivenStages==2): a uniform region's filter sees one ray's noise instead of eight's. ON BY
    // DEFAULT; falls back to unsplit CSRdShadow (never the single-pass primary) if either pipeline
    // fails to compile. Only while rayDrivenStages is 1 or 2. Console: voxi.rayDrivenShadowTiles.
    bool rayDrivenShadowTiles = true;

    // SUB-STAGE SPLIT B: CSRdGi's candidate trace, in two passes. MEASURED: the GI stage costs
    // 5.38 ms (4.43 ms already half-rate via rayDrivenStages==2's checkerboard) of the same 19.6 ms
    // frame; checkerboard still dispatches every lane (half return immediately, wave never compacted).
    // CSRdGiTrace carries the candidate trace (giTraceInitialCandidate + material eval) into its own
    // pass over a COMPACTED dispatch in checkerboard mode; CSRdGi resamples/shades from the stored
    // candidate. SAME IMAGE as rayDrivenStages==1 -- changes which pass traces the ray, not the
    // estimator, so the saving is occupancy/compaction, not quality. ON BY DEFAULT (same A/B reason as
    // rayDrivenShadowTiles); falls back to unsplit CSRdGi (never the single-pass primary) if the
    // matching pair fails to compile. Only while rayDrivenStages is 1 or 2. Console: voxi.rayDrivenGiSplit.
    bool rayDrivenGiSplit = true;

    // SUB-STAGE SPLIT C: CSRdRefl's register-heavy ray plus its bandwidth-heavy spatial history
    // gather (rtReflectionSpatial, up to a 7x7 depth-tested gather of last frame's history), in two
    // passes. MEASURED: the reflection stage costs 3.65 ms of the same frame the other splits
    // measure -- one thread pays for a reflection ray, a nested sun-shadow ray, a full material shade
    // AND the dense spatial gather, the shape that made splitting shade out of the megakernel pay off
    // (34 -> 1.8 ms) originally. CSRdRefl compiled a second time (AVER_RD_REFL_SPLIT=1) traces the ray
    // and writes a PENDING marker instead of composing; CSRdReflFilter runs rtReflectionSpatial alone
    // and finishes the compose. SAME IMAGE as unsplit -- round-trips through the same RGBA16F history
    // texture, not a quality trade. ON BY DEFAULT (same reason as the other two splits); falls back
    // to unsplit CSRdRefl (never the single-pass primary) if either pipeline fails to compile. Only
    // while rayDrivenStages is 1 or 2. Console: voxi.rayDrivenReflSplit.
    bool rayDrivenReflSplit = true;

    // LOCAL LIGHTS (LAMPS): a material with lightIntensity > 0 turns every draw using it into a small
    // sphere light (draw's bounding sphere, tinted by emissive colour), lit through the sun's BRDF
    // (diffuse and specular, the lobe widened by the lamp's angular size), with one stochastic shadow
    // ray toward one lamp per pixel, its visibility accumulated through the sun shadow's own
    // reprojection. lightIntensity MULTIPLIES what the material's glow/size already cast, in sun
    // units (SkyAtmosphere::sunIntensity) -- 1 = that, 2 = double. At most 32 lamps/frame
    // (brightest-and-nearest by lit output / squared distance). Works
    // in every D3D12 scene mode (raster, megakernel, staged). Translucent draws are unshadowed except
    // where staged replay proves they sit on a lit decal surface, borrowing its visibility. No lamps =
    // 0 count (skipped branch, staged pass not dispatched); off frees both history textures. Console:
    // voxi.localLights.
    bool localLights = true;

    // ---- staged ray-driven bit-field toggles (cb_.giShadowParams.w / gGiShadowParams.w) ---------
    // Four independent RUNTIME toggles VoxiRenderer::prePass packs into cb_.giShadowParams[3] every
    // frame (bits 1/2/4/8) -- the GI-only shadow map's params row's fourth component, formerly unused
    // (see FrameConstants::giShadowParams, VoxiRenderer.hpp). HLSL decodes it as
    // `uint bits = (uint)gGiShadowParams.w`. A FIFTH BIT (16, blendedReuseStagedLighting, below)
    // shares the row but is ORed in separately by VoxiRenderer::recordStagedRayDriven only on a
    // staged frame, not packed here -- it answers "are the staged textures even this frame's, right
    // now", not a quality-for-cost trade like these four.
    // T1-T3 ON BY DEFAULT: measured on the owner's NewSponza view (staged mode 1, 300 frames,
    // --gpu-timing): together 15.6 -> 13.9 ms/frame, still-image MAD 0.46 vs all off. Moving camera
    // (--cam-wobble 40 24, stopped at frame 100 vs settled): MAD 4.58 -> 4.70, pixels >16 codes
    // 2.67% -> 2.73%, specks 0.100% -> 0.109%, no 8-px tile structure.
    // STAGED MODES ONLY: single-pass (rayDrivenStages 0) compiles T1 on, T2-T4 off, ignoring these
    // four -- with all four live its pixel shader lost the device on AMD (rtGiShadowBits(),
    // voxi_rt.hlsli, has the measurement).

    // T1 (bit 1): the sun-shadow ray fired FROM A SECONDARY HIT (rtReflection's hit, ReSTIR GI's
    // candidate hit) normally walks rtShadow's full transmittance loop (up to 8 steps, RAY_FLAG_NONE,
    // AVER_RT_MASK_ALL) to tint light through glass. ON: both fire ONE ray instead
    // (RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, opaque+cutouts mask). TRADE: translucent instances
    // stop casting a shadow for these two rays, reading fully lit through glass. Primary shadows
    // (camera cascades, shadow probe) untouched. Console: voxi.rtSecondaryShadowOpaque. MEASURED
    // alone: GI trace 3.88 -> 3.38 ms, reflection 3.14 -> 2.73 ms; still image MAD 0.09.
    bool rtSecondaryShadowOpaque = true;

    // T2 (bit 2): rtSkyOcclusionTemporal skips rtAmbientTraced for an entire 8x8 TILE on this frame's
    // skip parity wherever that tile's reprojected history is valid -- the temporal blend keeps the
    // reprojection as the estimate, still written to history and spatially filtered. Whole tiles skip
    // together (a whole compute wave), not per-pixel checkerboard (which leaves every wave half
    // occupied and saves nothing -- the same lesson half-rate GI learned before its own compaction). A
    // pixel with no valid history always traces. MEASURED: sky occlusion costs 0.73 ms of the staged
    // mode 1, 15.4 ms frame. Console: voxi.rtSkyOcclusionHalfRate. MEASURED alone:
    // 0.72 -> 0.47 ms; still image MAD 0.40.
    bool rtSkyOcclusionHalfRate = true;

    // T3 (bit 4): rtReflectionTemporalEx skips its trace for a ROUGH pixel (lobeRough > 0 -- mirrors
    // always retrace, since a reprojected mirror reflection is visibly wrong the instant the camera
    // moves) on a skip-parity tile whose reflection history reprojects validly, reusing it as this
    // frame's colour (same as the existing tiled "not my turn" branch). MEASURED: reflection trace
    // costs 3.11 ms (+0.39 ms filter) of the staged mode 1, 15.4 ms frame. Console:
    // voxi.rtReflectionHalfRate. MEASURED alone: reflection trace 3.14 -> 2.17 ms; still image
    // MAD 0.04.
    bool rtReflectionHalfRate = true;

    // T4 (bit 8): sun visibility at ReSTIR GI's candidate HIT (giTraceInitialCandidate) comes from the
    // GI-only shadow map (same box the GI volume's light injection samples via giShadowFactor) instead
    // of a shadow ray; the ray still fires where the map can't answer (outside its box, or unusable
    // this frame). PROTOTYPE MEASURE on NewSponza, staged mode 1: GI trace 3.38 -> 2.68 ms (mode 2:
    // 1.65 -> 1.28 ms); still image MAD 1.61, +1.2
    // brighter -- 19 cm map texels let a little bounce light through under column capitals/bases that
    // the ray blocks (a smaller normal offset didn't change that). RE-MEASURED 2026-09-27 (NewSponza,
    // whole frame): gallery 11.03 -> 10.41 ms, court -0.57 ms; MAD 0.18 still / 0.34 moving -- the
    // 1.61 no longer reproduces, so ON by default. Revert: voxi.rtGiHitShadowMap false. Console:
    // voxi.rtGiHitShadowMap.
    bool rtGiHitShadowMap = true;

    // ---- BIT 16: REUSE THE STAGED RAY-DRIVEN LIGHTING FOR A TRANSLUCENT DRAW ON THE SAME SURFACE ----
    // Rides the same row as T1-T4 but shaped differently (see the toggle-block header). Trades: a
    // translucent pixel over an opaque surface the staged passes already lit THIS frame -- e.g.
    // NewSponza's floor dirt decal (a BLEND-translucent alpha-0.35 layer a fraction of a cm above the
    // floor) -- would otherwise have PSMainVoxi's translucent branch re-light it from scratch (its own
    // sun-shadow, ReSTIR GI candidate + shadow, sky-occlusion, and for rough surfaces a reflection
    // ray) instead of reading the ray-driven passes already computed at that pixel earlier in the
    // frame. ON reads gRdSunVisTex/gRdGiTex/gRdAoTex/gRdReflTex instead of re-tracing, gated PER PIXEL
    // on gRdSunVisTex.a's stored depth agreeing with this pixel's own -- i.e. the surface actually
    // sits on the one the staged passes lit, not merely near it in screen space. Excluded regardless:
    // a draw whose material reads the captured backdrop instead (glass, water -- attenuationDistance
    // or a material graph; IRenderFeature::blendedDrawReadsBackdrop), which wants its own lighting.
    // ON BY DEFAULT. MEASURED (NewSponza, staged mode 1, two floor-decal draws): blended replay
    // 0.39 -> 0.17 ms at the saved camera, 0.18 -> 0.14 ms at the standard view, still image diff
    // 0.01/0.04 vs off. Quad-uniform in the shader, so a 2x2 quad straddling a decal edge traces whole.
    // Console: voxi.blendedReuseStagedLighting.
    bool blendedReuseStagedLighting = true;

    // ---- the acceleration-structure "unchanged" gate ------------------------------------------
    // MEASURED on the owner's static NewSponza scene: the "Voxi acceleration structures" GPU span
    // costs 0.42 ms every frame -- a from-scratch ctx.buildTlas (PREFER_FAST_TRACE) plus an
    // unconditional instance-buffer rewrite/upload, recomputing the identical answer on an unmoved
    // scene; skipping the whole thing outright is still cheaper than even a refit (Settings::
    // rtRefitAccel), which is why this gate exists as a separate setting rather than being subsumed
    // by that one. Same trick as the GI rebuild gate (which has no Settings field of its own;
    // giUpdateInterval only amortises it): hash what buildAccelerationStructures() reads from the
    // draw list, and if nothing moved, leave tlas_/rtInstanceData_/their bound SRVs exactly as they
    // are. See VoxiRenderer::rtAccelSnapshotUnchanged() for what "unchanged" checks and the one thing
    // that still forces a real rebuild regardless (a cached BLAS handle the resource factory no
    // longer attributes to its mesh) -- a compute-skinned mesh present forces it too, but only while
    // Settings::rtRefitAccel below is off; on, this gate instead runs a lighter refit-only pass for
    // it (VoxiRenderer::refitDynamicAccelStructures) rather than a plain skip.
    // MOVER PATCH LANE (needs this and rtRefitAccel both on): a draw flagged Draw::movable (Play's
    // animated props, the pawn) has its world matrix left OUT of the key, so moving alone no longer
    // rejects the gate. A hit then runs VoxiRenderer::patchRtMovers(), which writes each mover's current
    // transform into the TLAS instance list and the ray-hit instance table, and the TLAS is refit
    // (same periodic full rebuild as rtRefitAccel). The movable bit stays in the key, so a draw that
    // starts or stops moving, and any change to mesh, material or flags, still forces the full build.
    // With either setting off the key hashes every world as before. The report line counts these
    // ticks as "mover-patched".
    // ON BY DEFAULT: it only ever skips work whose output is bit-identical -- not a quality trade --
    // so turning it off costs frame time and buys nothing measurable. Console: voxi.rtSkipUnchangedTlas.
    bool rtSkipUnchangedTlas = true;

    // ---- in-place update (refit) for the ray-tracing acceleration structures ------------------
    // ON: tlas_ and each compute-skinned mesh's BLAS are created updatable and refit in place instead of
    // fully rebuilt; a skinned mesh alone no longer forces the whole per-draw loop every frame (the gate
    // above runs a refit-only pass instead of a plain skip). Periodic full rebuilds every
    // kDynamicBlasRefitsPerRebuild / kTlasRefitsPerRebuild refits (VoxiRenderer.hpp), since a refit traces
    // worse as the pose drifts. OFF: the old behaviour exactly (full builds, no ALLOW_UPDATE).
    // UNMEASURED (engine runs were off when this landed, 2026-09-27). The ALLOW_UPDATE allocation is
    // LATCHED when a structure is created (like layeredBsdf); toggling live only changes whether a refit is
    // attempted. Console: voxi.rtRefitAccel.
    bool rtRefitAccel = true;

    // ---- path tracing -----------------------------------------------------------------------
    // WHERE RAY TRACING ENDS AND PATH TRACING BEGINS: RAY TRACING is discrete rays answering one
    // question (shadowed? what does this mirror see? what surface is this pixel?) -- one hit, direct
    // lighting; PATH TRACING is the multi-bounce solve. Separate settings (rayTracing/pathTracing
    // above) because separately useful, priced and supported.
    // Renamed from `rtBounces` (derived from the rayTracing tier, which let `pathTracing = Off`
    // projects run a path tracer while the setting read "off"); bounces now belong to pathTracing.
    // 1 = NO extra bounces (one hit = ray tracing). Above 1 is path tracing; VoxiRenderer refuses to
    // spend it while pathTracing is Off regardless of what's stored here (enforced in ptBounceParams).
    u32 ptBounces = 1;

    // ---- occlusion-aware fog: the air sky-visibility volume -----------------------------------
    // WHAT THIS FIXES: shared_prelude.hlsl's height fog (averFogFactor/averFogInscatter/
    // averApplyFogEx) and the aerial-perspective term add in-scattered SKY light along the
    // camera-to-surface path with no regard for what's between -- correct outdoors, wrong indoors.
    // MEASURED on Sponza's arcade: fog alone adds ~6% of sky radiance
    // over a 30 m corridor, brighter than the bounce-lit walls beneath it (a flat blue veil, not air).
    // With fog off, ReSTIR GI already matches the path-traced reference within 8% -- fog's own error.
    // ON BY DEFAULT. Off is exactly today's fog byte for byte (VoxiRenderer binds a 1x1x1 placeholder
    // at t17/u16; voxiAirVisibility() returns 1 unconditionally when it sees one) -- turning this off
    // only reverts to the pre-existing over-bright indoor fog, never a downgrade vs any earlier build.
    // WHY NOT SURFACE AO (reverted -- aver-fog-skyvis-failed.md): fog is a property of the
    // camera-to-SURFACE PATH, not the surface's hemisphere, and AO's accumulated history flashes white
    // on disocclusion (a fast pan resets it before reconverging) -- unaffordable on a still capture.
    // Instead: a WORLD-SPACE volume with no per-pixel history or jitter -- CSAirVis marches fixed
    // hemisphere directions through the SAME voxel grid the GI cone gather reads, so motion can't
    // flash it.
    // COST: one more compute pass, CSAirVis, over a FIXED 32^3 volume (VoxiRenderer::
    // kAirVisResolution), independent of Settings::voxelResolution (a small fixed grid suffices for
    // path occlusion, unlike the GI radiance volume). Refreshes one slab of z-layers per frame,
    // round-robin (a full 48^3 pass measured ~10 ms; sky visibility only changes with geometry); the
    // shade-side read is eight fixed texture taps down the existing fog ray, no extra ray of its own.
    // DEVICE-GATED, NOT JUST SETTING-GATED: needs SM 6.0 and DXC (VoxiRenderer::airVisWanted());
    // without either the placeholder stays bound and this setting has no effect -- the same fallback
    // shape Settings::rayDrivenStages has for its staged compute pipelines.
    bool fogOcclusion = true;
};

// Process-wide settings service. Single instance shared by the editor, the runtime and the C ABI.
class AVER_VOXI_API Renderer {
public:
    // Returns the process-wide instance.
    static Renderer& get();

    // Records what the device can do and re-clamps the current settings against it.
    void setDeviceInfo(const DeviceInfo& info);
    const DeviceInfo& deviceInfo() const { return device_; }

    const Settings& settings() const { return settings_; }
    // Applies what is legal for this device; unsupported requests are clamped, not silently kept.
    void setSettings(const Settings& s);

    // Returns whether a feature is usable on this device.
    Status status(Feature f) const;
    // Returns a readable reason for a feature's status.
    const char* statusText(Feature f) const;
    bool available(Feature f) const { return status(f) == Status::Ready; }
    // Returns whether this feature's refusal was already logged by setSettings' refuse() lambda
    // (Voxi.cpp) since the last setDeviceInfo call (refusalLogged_ resets there) -- lets a load-time
    // contradiction report (RenderSettingsResolver.hpp's manifestContradictions) skip re-warning about
    // a device limitation already logged, instead of saying it twice from two call sites.
    bool refusalLogged(Feature f) const { return (refusalLogged_ & (1u << static_cast<u32>(f))) != 0; }

    // Returns true once after settings.msaa changes, then clears the flag.
    bool consumeMsaaDirty();

    // ---- per-history reset requests: EditorConsole.hpp raises these, SandboxApp.cpp consumes and
    // forwards them to VoxiRenderer once a frame (mirrors consumeMsaaDirty's shape) -- this singleton
    // has no path to VoxiRenderer's private instance, only the console raises and only SandboxApp owns
    // the renderer to forward to. resetaohistory is an honest ALIAS of resetrthistory today
    // (VoxiRenderer::resetAoHistory has the full reason); requestAoHistoryReset exists so the two
    // commands stay textually distinct, in case that stops being true later.
    void requestGiHistoryReset()  { giHistoryResetRequested_ = true; }
    void requestRtHistoryReset()  { rtHistoryResetRequested_ = true; }
    void requestAoHistoryReset()  { aoHistoryResetRequested_ = true; }
    void requestDenoiserHistoryReset() { denoiserHistoryResetRequested_ = true; }
    // Each returns true once after its matching request*Reset() call, then clears itself -- same
    // one-shot contract as consumeMsaaDirty().
    bool consumeGiHistoryResetRequest();
    bool consumeRtHistoryResetRequest();
    bool consumeAoHistoryResetRequest();
    bool consumeDenoiserHistoryResetRequest();

    // Returns a feature's display name.
    static const char* featureName(Feature f);
    // Returns a quality level's display name.
    static const char* qualityName(Quality q);
    // Voxel grid edge a GI quality tier resolves to (setSettings derives voxelResolution on a tier
    // change, only when the caller left it untouched -- see setSettings/Voxi.cpp for the ladder).
    static u32 voxelResolutionForQuality(Quality q);
    // Total cones for the diffuse gather, including the axial one. See Settings::giCones.
    static u32 giConesForQuality(Quality q);
    // U1: how much of F2/F3's cost each GI tier pays for. See Settings::giRestirVisibility.
    static u32 giRestirVisibilityForQuality(Quality q);
    static u32 refractionForQuality(Quality q);
    // Revoxelisation interval a GI quality tier resolves to (same "only if untouched" derivation
    // rule as the grid edge above); Epic is 1 -- always fresh, unchanged by this.
    static u32 giUpdateIntervalForQuality(Quality q);
    // The RT sun-shadow rungs, mirroring giUpdateIntervalForQuality: applied by setSettings when the
    // rayTracing tier changes and the field arrives unchanged.
    static u32 rtShadowRaysForQuality(Quality q);
    // Sky-visibility rays per pixel for a ray-tracing tier. High and Epic, not Epic alone -- see
    // Settings::giSkyOcclusionRays and ladder::giSkyOcclusionRays (QualityLadder.hpp).
    static u32 giSkyOcclusionRaysForQuality(Quality q);
    // Sky-occlusion ray coherence tile for a ray-tracing tier; see Settings::giSkyOcclusionTile.
    static u32 giSkyOcclusionTileForQuality(Quality q);
    static u32 rtPixelsPerRayTileForQuality(Quality q);
    static u32 rtShadowDenoiseForQuality(Quality q);
    // 1 for every RT-capable tier except Low, which rasterises by explicit product decision (D3) -- 0
    // for Off/Low, 1 for Medium/High/Epic. See ladder::rtRenderMode for why Off and Low share a value
    // for different reasons.
    static u32 rtRenderModeForQuality(Quality q);
    // Derived from the PATH TRACING tier, not the ray-tracing one. See Settings::ptBounces.
    static u32 ptBouncesForQuality(Quality q);

private:
    Renderer() = default;
    Settings settings_{};
    DeviceInfo device_{};
    bool msaaDirty_ = true;
    // One-shot request flags for the five reset* console commands -- see requestGiHistoryReset() and
    // its siblings above. False by default: nothing is reset merely by the process starting up.
    bool giHistoryResetRequested_  = false;
    bool rtHistoryResetRequested_  = false;
    bool aoHistoryResetRequested_  = false;
    bool denoiserHistoryResetRequested_ = false;
    u32 refusalLogged_ = 0;   // one bit per Feature: its refusal has already been logged
};

} // namespace aver::voxi
