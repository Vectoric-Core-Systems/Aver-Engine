// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
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
    // D3D12 with the NVIDIA denoiser library actually built in AND willing to run on this backend --
    // see RenderSettingsResolver.hpp's DisableReason::RequiresNrd. Computed by the host at the same
    // two call sites that already know both halves (`backend() == rhi::Backend::D3D12 &&
    // render::nrd::Denoiser::available()`); this struct only carries the answer, it does not derive
    // it, so this core-only library still depends on nothing RHI-shaped.
    bool nrdSupported = false;
};

// The renderer quality settings, as requested. Clamped to the device by Renderer::setSettings.
struct Settings {
    Msaa    msaa               = Msaa::X4;
    Quality globalIllumination = Quality::Medium;
    // ON BY DEFAULT, AT MEDIUM, AND THE TWO KNOBS BELOW ARE MEDIUM'S RUNGS BY CONSTRUCTION -- see
    // rtShadowRays/rtPixelsPerRayTile for why that sentence is load-bearing rather than decorative.
    //
    // MEASURED COST. Release build, ElectricDreams, windowed at the editor's default 1600x900,
    // --no-vsync, --frames 200, whole-frame median: 11.86 ms with --no-rt, 18.44 ms at this tier's
    // rungs. +55%, and the cheapest honest way to have ray-traced sun shadows at all -- the naive
    // version of this change (flip the tier, leave the knobs at Epic's 4 rays) measures 23.39 ms,
    // almost exactly double the Off baseline. One machine, one GPU, one window size: the RELATIVE
    // ladder should hold anywhere, the absolute milliseconds are this card's (RX 7800 XT) and they
    // move with resolution, so quote the window when quoting the number.
    //
    // HOW TO MEASURE THIS WITHOUT MEASURING NOTHING, because that is the trap and it has now been
    // fallen into twice. The .ocproject path is POSITIONAL -- there is no --project flag, so
    // `Sandbox.exe --project <path>` silently opens no project at all. And even given the path
    // correctly, a project whose CREATEDWITH names an older series raises a modal and waits, so a
    // --frames run scores an EMPTY editor. Both failures look exactly like a successful benchmark:
    // plausible milliseconds, no error, a screenshot nobody opened. The tell is in the log --
    // "scene walk ... over 0 entities" means nothing loaded, and the honest baseline here is 14
    // entities and about 18 ms. Read that line before believing any number out of this ladder.
    //
    // A KNOWN WAY TO MAKE THIS LOOK BROKEN, recorded because it cost an afternoon to bisect: brighter
    // direct light on a surface means more INDIRECT light bounced off it, and the GI here does not
    // clamp what it gathers. Enough large, saturated, brightly-lit geometry and the bounce runs away
    // and floods the frame with that surface's colour -- three 1.8-metre pure-red spheres under a
    // 100,000-lux sun did exactly that, and turning RT on was merely what pushed it over, since it
    // lights those spheres more brightly than the voxel-cone path did. Scaling them down fixed it.
    // The scene was unreasonable; that the renderer answers it with a red screen rather than a clamp
    // is still the renderer's defect, and it is tracked. Nothing about it is hardware-specific.
    Quality rayTracing         = Quality::Medium;
    Quality pathTracing        = Quality::Off;

    // A LAYERED BSDF ALONGSIDE THE STANDARD BRDF, not instead of it. Off is today's
    // metallic-roughness Cook-Torrance response, byte for byte; the rungs above it add a coat lobe
    // over the existing base and, later, further layers.
    //
    // A Quality rather than a bool because the layers genuinely ladder: a coat evaluated with its own
    // GGX and split-sum environment term is not free, and a project should be able to ask for the
    // cheap version. Off is not "the feature is broken", it is a real, supported, and currently
    // default answer -- the same shape pathTracing has.
    //
    // NOT LIVE-SWITCHABLE, and that is a deliberate limitation rather than an oversight.
    // VoxiRenderer builds twenty-odd raster PSOs at init, compiled through DXC at runtime with no
    // disk cache; compiling a second matrix for a layered variant would double that on every launch
    // of every project, including ones that never turn this on. So the value is read once, before
    // the pipelines are built, and changing it takes a project reload. Making it free when Off
    // matters more than making it instant.
    Quality layeredBsdf        = Quality::Off;
    bool    meshShaders        = false;

    // Cubic voxel grid edge; the volume's memory and per-voxel GPU cost are both O(this^3). Defaults
    // to Medium's rung (128) below. Renderer::setSettings derives this from globalIllumination
    // whenever the tier changes and this field arrives unchanged -- see voxelResolutionForQuality
    // and setSettings. Set it explicitly (a different value than what's currently active, in the
    // same call that changes the tier) to override the tier's rung.
    u32 voxelResolution = 128;

    // HOW MANY CONES THE DIFFUSE GATHER TRACES, total, including the axial one along the normal.
    // Defaults to Medium's rung (6) below, and derived from globalIllumination on a tier change
    // exactly as voxelResolution above it is.
    //
    // THIS IS THE GI SETTING THAT ACTUALLY COSTS ANYTHING, and until now the tier did not touch it.
    // globalIllumination derived voxelResolution and giUpdateInterval, both of which move the
    // volume BUILD -- measured at 0.4-0.5 ms -- while the per-pixel GATHER, measured at 1.3 ms and
    // by far the larger half, was a hardcoded six for every tier. Turning GI down bought almost
    // nothing, and turning it up to High made the frame SLOWER with no way to spend the budget
    // (6.0 -> 6.4 ms: a bigger volume to sample, same number of samples).
    //
    // Cost is LINEAR in this and independent of the march length -- measured at about 0.22 ms per
    // cone, with the 24-step loop bound unreachable at the diffuse aperture because the cones exit
    // early. So this is the one GI number worth putting on a ladder.
    //
    // ON THE LADDER NOW: Low 3, Medium 6, High 9, Epic 13. See giConesForQuality in Voxi.cpp for the
    // per-rung reasoning and the measured per-tier cost (FirstPerson range, scene draw: Low 3.3 ms
    // through Epic 5.4 ms, 0.21 ms/cone, confirming the 0.22 ms/cone figure above from a second
    // experiment) -- and for why a "two cones moved a probe by 2/255" claim living elsewhere in this
    // tree is deliberately not repeated here as settled: it predates this ladder and was never
    // re-measured against it.
    u32 giCones         = 6;
    // Sky-visibility rays the AMBIENT term traces per pixel. 0 means "estimate it from the cone
    // gather", which is what this renderer has always done and what Low and Medium still do.
    //
    // WHY THE RAYS EXIST. `diffAmbient` (material_prelude.hlsl) multiplies the FULL sky irradiance by
    // the cone gather's occlusion, and that estimate is optimistic in enclosed geometry: six 60-degree
    // cones marching a voxel volume see through thin walls once they widen into coarse mips. MEASURED
    // on Sponza against a converged path-traced reference: shadowed pixels read [25,26,30] where the
    // reference says [7,7,7], and blue-biased (B > R) because what leaks in is sky. In an OPEN scene
    // the same estimate is approximately right, which is why this never showed up on the flat test map
    // and why it cannot be found by looking at one.
    //
    // DERIVED FROM rayTracing on a tier change, like rtShadowRays: 0 at Low and Medium, ONE at High
    // and Epic. It was Epic-only, and four rays, until the term accumulated against a reprojected
    // history (rtSkyOcclusionTemporal, rtAoHist_): one accumulated ray is both quieter than four
    // correlated ones and cheaper, which is what let it come down a rung. THE DEFAULT IS 0 BECAUSE
    // THE DEFAULT TIER IS Medium -- the derivation only fires when the tier
    // CHANGES, so a struct whose default contradicts its own tier never reaches the rung it claims.
    // rtShadowRays documents the same trap; it is written down twice because it has bitten once.
    //
    // ONE, NOT FOUR, and this sentence said four until the two were read side by side --
    // giSkyOcclusionRaysForQuality has always returned 1. The number matters more than a typo
    // normally would, because the obvious analogy is exactly the wrong one: rtShadowRays WAS 4 at
    // Epic when this measurement was taken -- it is 8 now (see ladder::rtShadowRays,
    // QualityLadder.hpp), but the four-ray figure below is what was actually measured and is left
    // as measured rather than rescaled to match -- and copying that number here reads as harmless.
    // It is not. Those four sun rays all point at the sun, walk the same BVH nodes and cost 0.05 ms
    // between them; THIS ray is cosine-distributed over the hemisphere, so neighbouring lanes
    // descend unrelated parts of the tree and the wave runs at the speed of its unluckiest lane. One
    // of them measures 5.37 ms -- more than the sun's four together. Four here would be roughly
    // 21 ms on a 50 ms frame. See rtSkyOcclusion in voxi.hlsl, which states the coherence argument
    // at the ray itself.
    //
    // THE COUNT IS NOW SWEEPABLE: --gi-sky-occlusion-rays N. It was not when this field landed --
    // there was no flag and no manifest key -- so the one dial the shader names as the lever for
    // this ray had never been measured at any value but its default.
    //
    // AND THE FIRST SWEEP SETTLES BOTH QUESTIONS. Sponza (112 entities, RENDER.RAYTRACING 4, MSAA 2),
    // --no-vsync, --gpu-timing, one camera, only this count differing:
    //
    //     rays   "Voxi ray-driven primary"   probe
    //     0      10.92 ms                    20,20,22
    //     1      12.25 ms                    11,11,13
    //     4      15.58 ms                    11,11,13
    //
    // COUNT IS THE LEVER HERE, exactly as rtSkyOcclusion predicts and exactly UNLIKE the sun ray:
    // the first ray costs 1.33 ms and each of rays 2-4 costs a further ~1.11 ms, against ~0.017 ms
    // for an extra SUN ray. That is a ~65x per-ray gap between a coherent ray and an incoherent one,
    // measured on one scene in one pass, and it is the reason this rung is 1 rather than 4.
    //
    // ONE RAY ALREADY BUYS THE CORRECTION. The probe is IDENTICAL at 1 and at 4, so three more rays
    // bought 3.33 ms and no visible change; while going from 0 to 1 moved a shadowed pixel from
    // 20,20,22 to 11,11,13, most of the way to the converged path-traced reference's 7,7,7. The
    // estimate is noisy at one sample and it does not matter, because what it is correcting is a
    // large systematic over-brightness, not a small random one.
    //
    // NOT A REPLACEMENT FOR THE CONE GATHER, which still supplies the bounced light (`ind.diffuse`).
    // This only replaces the scalar the SKY is attenuated by, and closes rather less than half the
    // measured gap: with ambient removed entirely the same pixel reads [15,14,15], so the remainder
    // is the bounce term and is a separate question.
    u32 giSkyOcclusionRays = 0;
    // Edge, in pixels, of the square that SHARES one sky-occlusion ray direction. 1 is a fresh
    // rotation per pixel and is what this renderer did before the dial was wired up.
    //
    // WHY IT EXISTS: the ray is cosine-distributed over the hemisphere, so neighbouring lanes descend
    // unrelated parts of the BVH and the wave runs at the speed of its unluckiest lane. Sharing the
    // azimuth across a tile makes those lanes trace near-PARALLEL rays that touch the same nodes and
    // the same cache lines. MEASURED on Sponza, marginal cost of going from 1 ray to 4:
    //
    //     tile 1  +3.56 ms      tile 2  +2.85 ms      tile 4  +2.21 ms
    //
    // -- a 38% cut in what an extra ray costs, which is what buys the extra SAMPLES below.
    //
    // THE PRICE IS CORRELATED NOISE inside a tile rather than independent noise per pixel. That is
    // the right trade for ambient occlusion, which is low-frequency by nature, and exactly the wrong
    // one for anything with sharp detail -- do not reuse this dial for a shadow or a reflection.
    //
    // RUNTIME, NOT A #define. It was a compile-time constant with no plumbing at all, which meant the
    // one lever the shader names for this ray could not be swept by anyone -- the same gap
    // giSkyOcclusionRays had. Rides gAmbientParams.y, a row already reserved for it.
    u32 giSkyOcclusionTile = 1;
    f32 giIntensity     = 1.0f;
    // CAUSTICS: how strongly light focused by a water surface brightens what is beneath it.
    // 0 switches the term off entirely (and the shader's own branch then costs nothing measurable).
    // Not on the quality ladder: it is a LOOK, not a fidelity rung -- a pool with caustics at Low
    // and none at Epic would be the same scene lit differently, which is not what a tier means.
    f32 causticStrength = 0.6f;
    f32 giMaxDistance   = 4000.0f;  // centimetres

    // THE CEILING GI RADIANCE IS CLAMPED TO before the tonemap, mirrored to the shader as
    // AVER_VOX_MAXRAD (voxi.hlsl / voxi_gi.hlsli) via FrameConstants::viewParams.y. 16.0 is not a
    // headroom number, it is a SYMPTOM's shape: acesTonemap (rhi/shaders/color.hlsli) floors NaN/
    // negative input at zero but is already flat WHITE by roughly x = 4-5, so any finite value pinned
    // at this ceiling paints solid white. voxi.giPoisonView DOES have a dedicated ceiling-hit colour
    // for this (red for the raw ReSTIR GI estimate, green for its NRD-denoised readback, both
    // voxi_restir.hlsli; violet for the ray-traced specular indirect term, voxi.hlsl -- B1/F5, not
    // giMode-gated); it is only the five NON-FINITE guards (magenta/cyan/yellow/orange/blue) that flag
    // isnan/isinf rather than "clamped". Both the raw ReSTIR GI estimate and its NRD-denoised readback
    // (voxi_restir.hlsli) and the cone-gather estimator (voxi_gi.hlsli) share this one ceiling.
    //
    // DEFAULT MUST STAY 16.0 -- this is the value every image this renderer has ever produced was
    // already clamped to as a compile-time #define; moving it changes nothing until a project or the
    // console (voxi.giRadianceCeiling) asks for a different number.
    //
    // LOWERING IT is the by-hand tool this field exists for: it can remove a white patch that turns
    // out to be a poisoned-but-finite value pinned at the ceiling, but it also dims any legitimately
    // bright bounce that happens to be near 16 -- there is no way to tell the two apart from this
    // number alone, which is why voxi.giPoisonView paints a ceiling HIT in its own colour (red/green,
    // voxi_restir.hlsli; violet, voxi.hlsl -- B1/F5) rather than asking this dial to double as a
    // diagnostic.
    f32 giRadianceCeiling = 16.0f;

    // ---- refraction: how a translucent surface BENDS what is behind it ----
    //
    // Absorption (attenuationColor) decides what COLOUR survives a medium; refraction decides where
    // it comes FROM. They are independent: glass is green because of iron and distorts because
    // ior != 1, and a pane can do either without the other. This is the second half, and it is only
    // reachable at all because the blended pass now has the scene behind it as a texture.
    //
    //   Off (0)          the background is sampled straight through -- what shipped before this.
    //   ScreenSpace (1)  the sample is OFFSET by the refracted view direction, scaled by the
    //                    ray-measured thickness. Nearly free, since it reuses the backdrop copy.
    //                    Its limit is the copy's: the offset can reach off-screen or pick up
    //                    something in FRONT of the glass, because a screen-space image only holds
    //                    what the camera saw. refractionEdgeFade exists to hide that.
    //   RayTraced (2)    a refracted ray is traced through the TLAS and its HIT POINT is projected
    //                    back to screen to choose the sample. That fixes the geometry -- the bend
    //                    follows real surfaces rather than a flat screen offset -- and costs a ray
    //                    on the path that is already the frame's bottleneck.
    //
    // ON A LADDER, AND THE DEFAULT MATCHES THE DEFAULT TIER'S RUNG. refractionForQuality derives
    // this from rayTracing (Off->Off, Low/Medium->ScreenSpace, High/Epic->RayTraced), and the
    // derivation fires only on a TIER CHANGE -- so `= 1` here must equal the Medium rung or the
    // derivation would never run on a default-configured device and the ladder would be dead code.
    u32 refractionMode     = 1;
    // Multiplies the offset. 1.0 is the physical bend for the material's own ior; below that trades
    // correctness for calm, above it exaggerates. A knob rather than a constant because the honest
    // answer depends on how thick the authored geometry is relative to the scene.
    f32 refractionStrength = 1.0f;
    // How far from the screen edge the offset is faded out, as a fraction of the smaller dimension.
    // 0 disables the fade and lets the artefact show, which is occasionally what you want to see.
    f32 refractionEdgeFade = 0.15f;

    // ---- ray-traced sun shadow: rays per trace, and how many pixels amortise one trace ----
    // Occlusion rays per pixel, when this pixel traces this frame. Clamped to
    // [1, VoxiRenderer::kMaxShadowRays].
    //
    // DERIVED FROM rayTracing on a tier change, exactly as giUpdateInterval is derived from
    // globalIllumination: Low 1, Medium 1, High 4, Epic 8 -- see ladder::rtShadowRays
    // (QualityLadder.hpp) for the per-rung reasoning, including why High and Epic moved up from
    // 2 and 4. THE DEFAULT IS 1 BECAUSE THE DEFAULT TIER IS Medium -- the derivation only fires
    // when the tier CHANGES, so a struct whose defaults contradict its own tier never reaches the
    // rung it claims. That is not hypothetical here: this field defaulted to 4 while rayTracing
    // defaulted to Off, so the moment RT was switched on by default it would have run Epic's ray
    // count under Medium's name.
    u32 rtShadowRays = 1;
    // Edge length of the square tile a single traced pixel is amortised over via the ray-traced
    // shadow's temporal history: 1 = every pixel traces every frame (bit-identical to no denoiser
    // at all); N>1 = one pixel in each NxN tile traces per frame, rotating which one so every pixel
    // gets its own turn every N*N frames, and every OTHER pixel reuses a reprojected history sample
    // instead of tracing. MUST be a power of two -- VoxiRenderer::setPixelsPerRayTile rounds to the
    // nearest one -- so the per-pixel schedule is a bitmask against the pixel coordinate rather than
    // a modulo, and the pixel COUNT one ray covers (N*N) is a clean power of two throughout: 1, 4,
    // 16, 64, 256 for tile edges 1, 2, 4, 8, 16. Clamped to [1, VoxiRenderer::kMaxPixelsPerRayTile].
    // NOTE: this governs the RT (DXR RayQuery) sun-shadow/reflection history and only has any effect
    // while rayTracing != Quality::Off; it does nothing to the voxel cone-trace GI cost below, which
    // is governed instead by giUpdateInterval.
    //
    // DERIVED FROM rayTracing on a tier change: 1 at every rung -- Low, Medium, High, Epic. Defaulting
    // to 1 for the same by-construction reason rtShadowRays defaults to 1 -- Medium's rung, because
    // Medium is the default tier, and now every other rung's rung as well. It was not always every
    // rung; see LOW WAS 4 below for why that changed.
    //
    // ALL FOUR TIERS ARE 1, WHICH MEANS NO TEMPORAL DENOISING ANYWHERE. 1 is "every pixel traces every
    // frame", which the paragraph above calls bit-identical to no denoiser at all: no tiling, no
    // reprojected history, no temporal blend. What you see is what was traced this frame. It costs
    // more than the amortised rungs and it is the honest default for a renderer people are evaluating,
    // because a temporal denoiser hides its own artefacts as readily as the tracer's.
    //
    // Measured, so the ladder is not guesswork. Release build, ElectricDreams, windowed at 1600x900,
    // --no-vsync, --frames 200, whole-frame median: rays 1 / tile 4 = 18.26 ms, rays 1 / tile 2 =
    // 18.49 ms, rays 1 / tile 1 = 18.44 ms, rays 2 / tile 1 = 19.99 ms, rays 4 / tile 1 = 23.39 ms,
    // against 11.86 ms with --no-rt.
    //
    // NOTE THE SHAPE, because it is what makes tile 1 defensible as the default rather than merely
    // preferable: the three tile widths at one ray span 0.23 ms -- they are the same number inside
    // the run-to-run noise -- while going from one ray to four costs 4.95 ms. Amortisation saturates
    // immediately and the ray count is where all of the money is; the temporal history was never
    // buying real frame time on this scene, at any tile width.
    //
    // LOW WAS 4, "the widest amortisation that pays," reasoned from exactly the still-camera table
    // above: since tile cost is noise, take the widest tile the clamp allows and bank whatever the
    // noise floor grudgingly gives up. That reasoning measured frame-time cost and only frame-time
    // cost, on a still camera -- and a still camera cannot see what a temporal-history amortisation
    // actually spends. It spends motion: with the camera moving, shadows visibly trail the thing
    // casting them, and Low was the one tier that shipped it. Measured this session with a wobbling
    // camera against a ground-crop pixel diff: tile 1 vs tile 4 differs 0.14 ms moving / 0.09 ms
    // static (noise, matching the still-camera table above), while the visible trail is already 0.80%
    // of pixels over threshold at tile 1 vs tile 2 alone, and tile 2 vs tile 4 adds only another
    // 0.04% on top of that -- the artifact is fully present by tile 2, so there is no partial-credit
    // rung between "visible trail" and "none" to fall back to. Low is 1 now for the same reason Medium
    // already was above: the frame time this bought was never real, and it was the one Low-specific
    // amortisation that traded a fault anyone moving the camera can see for milliseconds nobody could
    // measure. giUpdateInterval below and voxelResolution above both stay at Low's wider rungs -- each
    // costs real, measured time under motion and neither one produces a visible artifact at any
    // width tested.
    u32 rtPixelsPerRayTile = 1;

    // How many frames apart the GI volume is re-voxelised: 1 (the default) revoxelises and re-filters
    // every frame, identical to the original always-fresh behaviour. N>1 reuses the previous frame's
    // voxelised+filtered volume for the N-1 frames in between, amortising the voxelise-rasterise pass
    // and the mip filter chain (VoxiRenderer::voxelizePass / filterMips) at the cost of the indirect
    // lighting lagging scene changes by up to N-1 frames -- a visible latency trade, not a resolution
    // one. Clamped to [1, VoxiRenderer::kMaxGiUpdateInterval].
    //
    // DERIVED FROM globalIllumination on a tier change, exactly as voxelResolution above is: Low 4,
    // Medium 2, High 1, Epic 1 -- see ladder::giUpdateInterval (QualityLadder.hpp) for the per-rung
    // switch. Set it explicitly in the same call that changes the tier to override the derived value.
    //
    // THE DEFAULT IS 2 BECAUSE THE DEFAULT TIER IS Medium, and the two have to agree by construction
    // -- the derivation only fires when the tier CHANGES, so a struct whose defaults contradict each
    // other never reaches the rung it claims. voxelResolution's 128 is Medium's rung for exactly this
    // reason. This field has now moved more than once for that same reason: it was 1 while Medium
    // derived to 4 (below), then briefly consistent at 1 while Medium derived to 1, and is now 2
    // while Medium derives to 2 -- see MEDIUM IS NOW 2 below for why it moved again.
    //
    // MEDIUM MOVED 4 -> 1 BECAUSE 4 IS WHAT MAKES LIGHTING TRAIL THE CAMERA, and the frame time it was
    // buying is not there to buy. An earlier revision of this comment claimed interval 1 left 187.9 ms
    // on the table against 104.5 ms at interval 4. Re-measured on the same scene (Release,
    // ElectricDreams, 1600x900, --no-vsync, --frames 200): intervals 1, 2, 4 and 8 give medians of
    // 18.54, 18.47, 18.50 and 18.46 ms -- a 0.09 ms spread across the whole range, which is noise on a
    // STILL camera. Whatever made revoxelisation the bottleneck when that pair of numbers was taken is
    // no longer true, and the figure outlived it; it is quoted here as refuted rather than quietly
    // deleted.
    //
    // WHAT A STILL CAMERA CANNOT SEE, MEASURED SEPARATELY: under a wobbling camera the "Voxi GI
    // update" span itself (not the whole frame) went 36.00 ms at interval 1 to 16.57 ms at interval 4
    // (aver-gi-update-dominates-under-motion.md; Sponza, --cam-wobble 15 50, ray-driven + AverSR
    // Balanced) -- EVIDENCE that amortising this pass costs real time under motion, contradicting the
    // still-camera table above. That run also used tile 4 on the unrelated RT shadow amortisation, and
    // beginShadowHistory sits inside the same measured span (VoxiRenderer.cpp), so crediting the whole
    // gap to giUpdateInterval alone is UNCONFIRMED rather than settled. Lag at interval 4 is itself
    // UNMEASURED -- the note this evidence comes from says so directly ("both untested").
    //
    // LOW IS NOW 4, NOT 8 (kMaxGiUpdateInterval, the widest the clamp allows). The move to 4 leans on
    // the EVIDENCE immediately above; a further step to 8 is UNMEASURED in both directions -- this
    // file has no cost or lag figure for interval 8 under motion, only the still-camera table, which
    // this whole comment has already shown cannot rank these rungs against each other.
    //
    // MEDIUM IS NOW 2. UNMEASURED FOR BOTH COST AND LAG AT THIS SPECIFIC RUNG: nothing above was taken
    // AT interval 2 under motion, and this comment says so rather than borrowing the interval-4 number
    // as if it applied here. What is known rather than measured: at interval 2 the volume rebuilds
    // every other frame, so indirect light can lag a moving scene by at most one frame before the next
    // rebuild catches it up, and a still scene converges to exactly the same image interval 1 produces
    // either way (setSettings' own derivation comment). This does not reverse the 4 -> 1 reasoning
    // above -- 4 still trails visibly and the frame time it bought was still noise -- it reopens a
    // narrower question on the other side of "always fresh": whether Medium, the tier most projects
    // actually run, should pay 1's full cost for a lag this file has no evidence is visible at 2.
    u32 giUpdateInterval = 2;

    // ---- WHICH ESTIMATOR ANSWERS THE DIFFUSE BOUNCE: the voxel cone gather, or RTXDI ReSTIR GI ----
    // 0 = cone gather (DEFAULT, and every build before this field existed). 1 = ReSTIR GI: one
    // traced candidate per pixel, reused across frames through the vendored RTXDI SDK's temporal
    // resampling (third_party/rtxdi -- RTXDI_GITemporalResampling, RTXDI_GIReservoir; see
    // giRestirIndirect in voxi.hlsl for the call and RAB_* implementations it needed).
    //
    // NOT ON THE QUALITY LADDER above (giCones, voxelResolution, giUpdateInterval): those all scale
    // ONE estimator up and down a ladder of the SAME kind of answer. This SWITCHES estimators --
    // deterministic clipmap march vs. stochastic ray + temporal reuse -- which is an authoring
    // decision with its own trade (far less per-frame ray-tracing noise, at the cost of a biased,
    // history-dependent estimate that can lag a moving light or a disoccluding camera) rather than a
    // rung between Low and Epic. So setSettings never DERIVES this from globalIllumination the way
    // it derives giCones etc. on a tier change; it only clamps and range-checks it (Voxi.cpp).
    //
    // THE DEFAULT IS 0 AND MUST STAY 0 for the reason restated at every other field on this page
    // that has already been bitten by its opposite: VoxiRenderer::giRestirWanted() gates the actual
    // switch (it also requires ray-tracing hardware AND the rayTracing tier to be on, so a project
    // with no RT never allocates the reservoir buffer or the previous-surface history this needs).
    //
    // WHAT THE SHADER ACTUALLY READS IS THE EFFECTIVE VALUE, NOT THIS RAW FIELD -- an earlier
    // revision of this comment said the shader-side call sites "branch on this value directly", which
    // is stale. giMode_ itself is read only by giRestirWanted() (VoxiRenderer.hpp); the per-frame
    // constant buffer resets gGiRestirParams.x to 0 every frame and sets it to 1 only inside the
    // block already gated on giRestirWanted()'s own ReSTIR history textures (VoxiRenderer.cpp).
    // PSMainVoxi and PSRayDriven both branch on that constant, gGiRestirParams.x, never on this
    // field. So a value this field holds that the device or the tier cannot honour is never seen by
    // a shader regardless of whether setSettings clamps it -- which is what lets it be stored exactly
    // as requested (RenderSettingsResolver.hpp's resolve() computes the effective value the UI and
    // the console show; see its own prerequisite table for the chain: ray-tracing hardware, the
    // rayTracing tier, and the globalIllumination tier all have to agree before giMode=1 does
    // anything). So unlike voxelResolution/giCones, THIS default is not merely "the tier's own rung",
    // it is "byte-identical to every image this renderer produced before ReSTIR GI existed", and it
    // stays that way regardless of what tier globalIllumination is set to.
    //
    // SCOPE, STATED RATHER THAN LEFT FOR SOMEONE TO DISCOVER BY READING THE SHADER: candidate
    // generation + RTXDI SPATIO-TEMPORAL resampling -- temporal reuse AND a spatial pass, not merely
    // the former. STALE UNTIL THIS WAVE: an earlier revision of this comment said "NO SPATIAL reuse"
    // and named RTXDI_GISpatialResampling / RTXDI_GISpatioTemporalResampling
    // (third_party/rtxdi/Include/Rtxdi/GI/SpatialResampling.hlsli, SpatioTemporalResampling.hlsli) as
    // "vendored and unused" -- true of the plain spatial variant, which this file genuinely never
    // calls, but not of the spatio-temporal one it does: giRestirIndirect's own
    // RTXDI_GISpatioTemporalResampling call (voxi_restir.hlsli) runs 1-2 spatial taps alongside its
    // temporal ones (stparams.numSamples, voxi_restir.hlsli:944-947, :969) -- voxi_restir.hlsli's own
    // header comment already states this correction (:61-69); this field's comment had simply drifted
    // from it.
    u32 giMode = 0;
    // ---- ReSTIR GI VISIBILITY: how much of F2 (candidate-hit sky) and F3 (reuse visibility) -- the
    // contrast fix's two per-pixel rays, cb4b48df -- each rung of globalIllumination pays for ----
    //
    // U1's setting. NoRay restores cb4b48df's pre-fix behaviour outright for both rays (the legacy
    // over-brightness that fix exists to remove); Reconstructed replaces both with one voxel-cone march
    // the diffuse gather already pays for, so it costs no extra ray at all; HalfResolution traces exact
    // visibility on one pixel in four per frame and reconstructs the rest from a depth/normal-aware
    // neighbourhood, with a pixel lacking a valid reconstruction falling back to tracing (so its worst
    // frame costs what Full costs, never more); Full traces every pixel every frame -- today's
    // behaviour, byte for byte. See ladder::giRestirVisibility (QualityLadder.hpp) for the per-rung
    // reasoning and RenderSettingsResolver.hpp's Resolution::giRestirVisibility for how a UI reads it.
    //
    // DERIVED FROM globalIllumination ON A TIER CHANGE, exactly like giCones/voxelResolution/
    // giUpdateInterval above -- and for the identical reason THE DEFAULT IS 2 (HalfResolution): THE
    // DEFAULT TIER IS Medium, the derivation only fires on a tier CHANGE, and a struct default that
    // disagrees with its own tier's rung would never reach the value it claims (the same trap
    // giUpdateInterval and rtShadowRays document above, restated here because this field can fall into
    // it exactly as easily).
    //
    // COMPOSES WITH THE LEGACY BITS, NOT REPLACED BY THEM: voxi.legacyRestirHitSky and
    // voxi.legacyRestirReuseVisibility (console-only switches, never persisted) each force NoRay for
    // their OWN ray regardless of what this field asks for -- a set legacy bit always wins, for that one
    // ray only. This field never writes that slot and the two never collide over ownership of it.
    //
    // STORED EXACTLY AS REQUESTED, RESOLVED AT READ TIME -- like giMode just above, and only meaningful
    // while giMode itself resolves to ReSTIR: RenderSettingsResolver.hpp's resolve() computes
    // Resolution::giRestirVisibility.effective, which deliberately EQUALS requested always (see that
    // field's own comment there for why "fixing" that to match giMode/rtRenderMode/denoiser's usual
    // rule would be wrong for this one).
    enum class RestirVisibility : u32 { NoRay = 0, Reconstructed = 1, HalfResolution = 2, Full = 3 };
    u32 giRestirVisibility = 2;   // must equal ladder::giRestirVisibility(Quality::Medium)

    // ---- BISECTING THE SAME FADE FROM THE OTHER SIDE: SPLIT REUSE APART, THEN TIGHTEN IT ----
    //
    // STATE OF THE BISECTION, so the next reader does not have to reconstruct it from commit
    // messages: ReSTIR GI reads brighter while the camera moves and settles darker over about a
    // second after it stops. Ruled out by hand: auto-exposure, the NRD denoiser, sky-occlusion rays,
    // the F2 voxel bounce, voxel rebuild rate, Half vs Full visibility, the spatial-reuse motion
    // discount (3dbc9a42, reverted 8daed7f1), the reservoir age and the moving-camera history cap (both
    // measured WORSE). What removes the fade is giRestirMaxHistory 0, and voxi.debugResetHistoryEveryFrame 1
    // (c08c76d2), which clears the ReSTIR reservoir history every frame -- and that disables BOTH
    // temporal reuse and spatial reuse at once, since RTXDI reads its spatial neighbours out of the
    // same previous-frame reservoir buffer temporal resampling writes. So the carrier is reuse
    // itself, and the next question this field and the two thresholds below exist to answer is which
    // half, and whether the reuse tolerances (voxi_restir.hlsli's RTXDI_IsValidNeighbor test) are
    // simply too loose to begin with.
    //
    // 15 MEANS AUTO: leave the existing motion discount's own numSamples computation
    // (voxi_restir.hlsli) exactly alone, byte-identical to today's image, fade included. 0 disables
    // spatial reuse OUTRIGHT -- temporal reuse only, so a fade that survives this setting cannot be
    // coming from the spatial half. 1..8 pin the tap count regardless of camera motion, overriding
    // the discount's own lerp(2.0, 1.0, motionT); clamped to 8 downstream because that is the ceiling
    // the fused temporal+spatial pass was ever stability-tested against (see that lerp's own K*M
    // margin analysis, voxi_restir.hlsli).
    //
    // FOUR BITS, NOT THREE: a real count only needs 0..8, but 15 has to be a value NO real count
    // will ever collide with, so the packed field needs one more bit than "0..8" alone would.
    // Bits 12-15 of gAmbientParams.w -- see
    // givis::packAmbientW (GiVisibility.hpp) for the pack/decode this shares byte-for-byte with
    // voxi_restir.hlsli.
    //
    // DEBUG/TUNING ONLY, LIKE THE TWO THRESHOLDS BELOW: no manifest key, no Settings UI. This is a
    // bisection tool for one open question, not a shipped quality dial -- console: voxi.giRestirSpatialSamples.
    u32 giRestirSpatialSamples = 15;

    // ---- WHAT THE CAPTURES NARROWED IT TO: THE WEIGHTING WHILE A RESERVOIR IS YOUNG ----
    //
    // Measured headless on Sponza (camera translating, then stopped at a known frame), viewport
    // mean luminance at +3 frames after the stop versus settled: baseline 0.0965 -> 0.0892 (+8.2%
    // too bright, gone by ~+25 frames), tightened reuse tolerances IDENTICAL (+8.2%, so the
    // neighbour test is innocent), spatial reuse off still +6.9% (so the spatial half is not the
    // carrier), and the moving-age cap made it WORSE (+24%). The decisive number: reuse switched
    // off entirely sits at 0.0889, which is the SETTLED value -- so a partially-converged reservoir
    // reads brighter than BOTH the no-reuse estimate and the converged one. That is a weighting
    // error while M is small, not stale radiance, and these two dials are the two knobs RTXDI
    // exposes over that weighting.
    //
    // biasCorrection: RTXDI_BIAS_CORRECTION_OFF (0, plain 1/M normalisation), BASIC (1, today's
    // value and what voxi_restir.hlsli's own RTXDI_GI_ALLOWED_BIAS_CORRECTION compiles) or
    // RAY_TRACED (2 -- NOT compiled today; selecting it without flipping that #define and writing
    // the RAB_GetConservativeVisibility the spatial half needs would simply behave as BASIC).
    // maxHistory: stparams.maxHistoryLength, the cap on how much M a temporal reservoir may carry
    // into the combine; 1 is today's value (602d1b06 lowered it from 8 to kill the load-time
    // overshoot, which is this same mechanism seen from a cold start rather than from motion).
    // Both are console-only bisection dials: no manifest key, no Settings UI, defaults reproduce
    // today's image exactly. Packed at gAmbientParams.w bits 16-17 and 18-23.
    // giRestirMaxHistory: RTXDI's stparams.maxHistoryLength -- how much M a previous-frame
    // reservoir may carry into the combine, i.e. how much weight ReSTIR gives what it already
    // believes over what it sampled this frame.
    //
    // DEFAULT 0, AND THAT IS THE CAMERA-MOTION FADE FIX. Measured headless on Sponza (camera
    // translating, stopped at a known frame, viewport mean at +3 frames after the stop against
    // settled): 1 -- the old default -- overshoots +8% and decays over ~25 frames, which is the
    // fade; 8 overshoots +104%; 0 does not overshoot at all. Everything else measured innocent:
    // the spatial half (+6.9% with it off), the reuse tolerances, the bias-correction mode, the
    // Jacobian, and the reservoir age (capping it made the overshoot WORSE, +24%, as did capping
    // history only while moving, +62%) -- every restart re-forms the chain out of single-sample
    // reservoirs whose RIS weight has enormous variance, and that is what flashes.
    //
    // WHAT 0 COSTS, MEASURED RATHER THAN ASSUMED: nothing detectable. Settled brightness is
    // unchanged (0.0893 against 0.0892), grain and flicker at rest are identical (0.00597/0.00057
    // against 0.00594/0.00056), and mid-motion both are slightly BETTER while the moving image
    // sits at the settled brightness instead of 5% above it. NRD is doing the smoothing this
    // reuse was supposed to provide, which is why removing it is free here.
    //
    // 1 RESTORES THE OLD BEHAVIOUR for A/B. Console: voxi.giRestirMaxHistory. Packed at
    // gAmbientParams.w bits 18-22.
    u32 giRestirMaxHistory = 0;

    // ---- NVIDIA NRD, DENOISING THE SKY OCCLUSION AND THE ReSTIR GI RADIANCE ----
    //
    // Off by default, and ON IS A REAL COST the user is choosing rather than one a denoiser helped
    // itself to: NRD needs the thin G-buffer (velocity, view Z, normal/roughness -- three more render
    // targets, ~54 MB at 1080p) and it needs them WRITTEN, and nothing else in this engine turns that
    // on. A renderer that silently allocated them because a filter wanted them would be spending a
    // frame budget nobody agreed to, so this field is the agreement.
    //
    // IT REQUIRES MSAA 1, and that is D3D12's rule rather than a choice made here: every target in
    // one OMSetRenderTargets call must share a sample count, and the G-buffer's three are always
    // single-sample, so above 1x the backend clears them without writing and every NRD input would be
    // blank. A denoiser fed blank inputs does not fail -- it returns a confident, uniformly wrong
    // image -- so VoxiRenderer skips the pass instead and says so once at WARN. Turning this on at
    // MSAA 8x is therefore a no-op, which is why the UI says so next to the checkbox.
    //
    // D3D12 only; see modules/render.nrd for why (NRD wants register space 1, Vulkan refuses it).
    bool denoiser = false;

    // ---- REBLUR history/prepass tuning -- LIVE dials over render.nrd::Denoiser::ReblurTuning ----
    // Three of ReblurTuning's fields (hitDistA/B/C and enableAntiFirefly are NOT here -- they are
    // unit-conversion constants the engine owns, not a look anyone should be turning by hand) exposed
    // so the REBLUR_DIFFUSE denoiser's history depth and pre-pass blur can be swept without a rebuild.
    // VoxiRenderer applies these every time setSettings() runs (already once a frame), via
    // nrd::SetDenoiserSettings -- NRD.h documents that call as needing "at least once per denoiser,
    // not necessarily on each frame", so re-issuing it here takes effect on the NEXT frame without
    // tearing the NRD instance (and its accumulated history) down and recreating it.
    //
    // DEFAULTS ARE TODAY'S HARDCODED VALUES, UNCHANGED: VoxiRenderer used to construct a
    // default-initialised ReblurTuning{} once at NRD creation and never touch it again; these three
    // fields default to exactly the numbers ReblurTuning{} already carried, so nothing about the
    // image moves until one of them is set to something else.
    //
    // Ranges are NRD's own (third_party/nrd/Include/NRDSettings.h's ReblurSettings), not guessed.
    float reblurDiffusePrepassBlurRadius = 30.0f;
    // [0; REBLUR_MAX_HISTORY_FRAME_NUM=63] per NRDSettings.h. History depth in frames, not dispatch
    // count -- see ReblurTuning's own comment for why this is latency/noise, not a pass toggle.
    u32   reblurMaxAccumulatedFrameNum = 30;
    // [0; REBLUR_MAX_HISTORY_FRAME_NUM=63] per NRDSettings.h ("0 disables the stabilization pass";
    // a value >= maxAccumulatedFrameNum is clamped down to it BY NRD ITSELF, not by this engine --
    // today's defaults (63 here, 30 above) are exactly such a pair, left exactly as they already
    // were).
    u32   reblurMaxStabilizedFrameNum = 63;
    // The residual-noise dials -- see render.nrd::Denoiser::ReblurTuning for what each does and the
    // NRD guidance behind exposing it. Defaults are NRD's own; REBLUR_DIFFUSE (index 1) only.
    bool  reblurAntiFirefly = true;   // NRD's own default; see ReblurTuning::enableAntiFirefly
    float reblurFireflySuppressorScale = 2.0f;   // see ReblurTuning::fireflySuppressorMinRelativeScale
    float reblurAntilagSigmaScale = 2.0f;
    float reblurAntilagSensitivity = 3.0f;
    float reblurMinHitDistanceWeight = 0.1f;
    float reblurFastHistoryClampSigma = 2.0f;
    u32   reblurMaxFastAccumulatedFrameNum = 6;
    u32   reblurHistoryFixFrameNum = 3;
    float reblurMinBlurRadius = 1.0f;
    // 10, NOT NRD's 30 -- THE ONE DIAL HERE THAT MEASURED A WIN. REBLUR spreads a fresh history over
    // this radius with a sparse kernel, and after motion that sparse pattern is the grain. PTTest
    // gallery, frame after a 30-degree sweep vs settled at the same pose, final image: 1-px grain
    // 0.761 -> 0.645, 99th percentile 7.04 -> 5.47; NRD's GI alone 2.444 -> 1.965. Still frame MAD
    // 0.34, no brightness shift, still-camera GI noise +1.5%. 7 bought slightly more in motion and
    // cost +7% at rest; 15 bought half as much.
    float reblurMaxBlurRadius = 10.0f;
    // WHICH CAMERA NRD IS TOLD ITS INPUTS WERE RENDERED WITH. NRD runs in beginShadowHistory, before
    // this frame's scene pass, so every input it reads (view Z, motion vectors, normals, radiance) was
    // written by LAST frame's pixel shader. true hands it last frame's camera as current and the one
    // before as previous -- the pair those inputs were actually made with. false (the DEFAULT) is the
    // wiring that shipped: this frame's camera, one frame ahead of its own data.
    //
    // DEFAULT FALSE BECAUSE THE CONSISTENT PAIRING BOUGHT NOTHING MEASURABLE. PTTest gallery, NRD's GI
    // alone: after a 30-degree sweep it moved 1-px grain 2.444 -> 2.504, mid-sweep 4.47% -> 4.61%, and
    // the per-pixel difference between the two is unstructured speckle with no ghost either way --
    // REBLUR reprojects by the motion vectors, which are right under both, and uses the matrices only
    // for its plane and parallax tests. Kept as a dial because the analysis is sound and the
    // difference may show on translation-heavy motion this was not measured on.
    bool  nrdCameraMatchesInputs = false;

    // SPATIAL denoise radius for the ray-traced sun shadow, in pixels. 0 is off: the shadow term is
    // whatever this pixel's own rays returned, unfiltered. N > 0 averages a (2N+1)^2 neighbourhood
    // of the shadow history, weighted by how well each neighbour's stored depth agrees with this
    // pixel's surface plane. The default is 2, not 0 -- see WHY 0 WAS THE HONEST DEFAULT below for
    // why that changed.
    //
    // WHY THIS EXISTS, AND WHY IT IS NOT THE TILE KNOB ABOVE. rtPixelsPerRayTile amortises over
    // TIME: a pixel reuses a reprojected value it computed frames ago. That converges beautifully
    // on a still camera and falls apart the moment one moves -- measured, at a penumbra probe: the
    // soft edge collapses to flat fully-shadowed under about one degree of yaw over forty frames.
    // This averages over SPACE instead, and keeps no history at all, so there is nothing to go
    // stale and camera motion cannot poison it. The two are independent and can be combined, but
    // they fail in completely different ways and should not be reasoned about as one setting.
    //
    // THE PROBLEM IT IS FOR. At one ray per pixel -- which is what Low and Medium both run -- the
    // shadow term is a hard 0 or 1, so a penumbra is not soft, it is dithered. Measured at a probe
    // whose converged answer is 34,36,40: one ray reads 61,59,59 and never improves, because the
    // ray is a pure function of the pixel and repeats forever. Sixteen rays reach 34,36,40 and cost
    // 30.55 ms against 18.66. Averaging the neighbours instead is the cheap way to the same place,
    // because rtShadow jitters the ray ORIGIN across the pixel footprint -- so neighbouring pixels
    // are already sampling different parts of the same receiver, and their mean is a real area
    // estimate rather than a blur.
    //
    // WHY 0 WAS THE HONEST DEFAULT, AND WHY IT ISN'T ANY MORE. This stayed 0 at every tier while
    // rasterisation (PSMainVoxi) was the default primary-visibility path, because PSMainVoxi ran at
    // this struct's own MSAA default of 4x and a smoothing filter stacked on an already-antialiased
    // image is redundant polish, not a fix -- the honest thing to ship as a default was raw, unhidden
    // noise, because a filter can smear as readily as it can clean up, and there was no evaluated hole
    // to justify accepting that risk. Same philosophy rtPixelsPerRayTile states outright a few dozen
    // lines above: "a temporal denoiser hides its own artefacts as readily as the tracer's." It was
    // sound while it was written.
    //
    // RAY-DRIVEN PRIMARY VISIBILITY (Settings::rtRenderMode) CHANGED THE TRADE AT MEDIUM AND ABOVE,
    // not the philosophy, and Low is now the deliberate exception to it (D3; see ladder::rtRenderMode,
    // QualityLadder.hpp): Low still rasterises primary visibility through PSMainVoxi, at this struct's
    // own 4x MSAA default. The ray pass is one fullscreen triangle with no per-triangle coverage, so
    // wherever rtRenderMode IS 1 -- Medium, High and Epic -- it always runs at a single sample
    // regardless of Settings::msaa (see rtRenderMode's own "WHAT DEFAULTING TO IT TRADES AWAY" list),
    // and there is no antialiasing pass quietly softening anything any more AT THOSE TIERS. Low keeps
    // its shadow ray -- one ray per pixel, the same count as Medium -- but fires it from inside the
    // rasterised PSMainVoxi rather than the ray-driven pixel shader: the PRIMARY VISIBILITY method is
    // what changes at Low, not whether the shadow ray exists. So the raw sun-shadow term is a hard 0
    // or 1 at Medium and High (see THE PROBLEM IT IS FOR, above), with 4x MSAA gone at those tiers to
    // soften it, while Low's own dithering is still quietly resolved by the MSAA pass it kept. Leaving
    // the filter off no longer shows an evaluator the renderer's honest raw fidelity at Medium and
    // above; it shows them a defect a filter this cheap (+0.02 ms at the widest rung the clamp allows,
    // against +1.69 ms for one more traced ray -- VoxiRenderer.cpp) already fixes to within one code
    // of a sixteen-ray reference on a still camera.
    //
    // WHAT IS ACCEPTED IN EXCHANGE, HONESTLY, rather than left for someone to discover by eye: the
    // gather centre is reprojected through LAST frame's camera to stay aligned with the shadow history
    // texture (rtShadowSpatial, VoxiShaders.hpp), so under camera motion it can walk slightly off the
    // true receiving surface. Measured with a six-degree wobble: 12 to 32 codes of extra darkening,
    // increasing with radius. That is a real, bounded smear -- not the unbounded "collapses to flat
    // fully-shadowed" failure rtPixelsPerRayTile's old Low=4 rung produced -- but it is not nothing,
    // and a benchmark that never pans cannot see it: this project has already been burned twice by
    // exactly that blind spot (giUpdateInterval's camera-trail lag, rtPixelsPerRayTile's Low=4 rung).
    // Any future change to these rungs must be checked against a MOVING-camera penumbra probe, not a
    // parked one, before it ships.
    //
    // DERIVED FROM rayTracing on a tier change, like the two knobs above: Low 2, Medium 2, High 1
    // (was 2), Epic 1. See ladder::rtShadowDenoise (QualityLadder.hpp) for the per-rung reasoning,
    // including why High moved down to join Epic. THE DEFAULT IS 2 BECAUSE THE DEFAULT TIER IS
    // Medium -- the derivation only fires when the tier CHANGES, so a struct default that contradicts
    // its own tier never reaches the rung it claims -- a trap this file has already fallen into in
    // both directions with giUpdateInterval.
    u32 rtShadowDenoise = 2;

    // ---- ray-driven rendering ---------------------------------------------------------------
    // WHICH THING FINDS THE FIRST SURFACE: 0 = the rasteriser (every version of this engine
    // before this setting existed), 1 = a primary ray per pixel. Everything downstream of that
    // first hit is unchanged -- PSMainVoxi already traces the sun shadow, evaluates the material
    // and traces a reflection in ONE pixel-shader invocation (VoxiShaders.hpp:781-826), so this is
    // not "fusing passes", it is swapping out the one stage that is still fixed-function.
    //
    // MEASURED BEFORE IT WAS BUILT, which is why the number to beat is written down here:
    // ElectricDreams at 4x MSAA, 2750x1639, Release -- raster primary visibility plus material
    // shading is 9.2 ms of `scene draw` with RT and GI off, and one additional shadow ray costs
    // 1.6 ms at the same resolution. A primary ray has to fit inside that difference to be worth
    // having.
    //
    // 1 FROM MEDIUM UP; LOW RASTERISES INSTEAD (D3, retuned) -- BY EXPLICIT PRODUCT DECISION, not an
    // experiment left behind a flag. The user calls the ray-driven path "the Wavefront Primary rays
    // model" and has decided it is the default render path from Medium up; Low is deliberately kept
    // on the rasteriser. THE EVIDENCE FOR LOW IS PARTIAL, NOT SETTLED: at overview cameras raster
    // measures slower than ray-driven (ElectricDreams 20.55 vs 8.05 ms; PTTest with Path Tracing
    // pinned off, 13.51 vs 8.04 ms), so Low can be slower than Medium at some cameras; a close-up
    // case that once favoured raster (9.02 vs 24.04 ms) is UNCONFIRMED, because the run that produced
    // it also had Path Tracing on, which silently took the frame over instead of ray-driven. D3
    // stands regardless -- it is the user's decision, made with this evidence in view, not a claim
    // that raster is faster at Low. See ladder::rtRenderMode (QualityLadder.hpp) for the switch and
    // rtRenderModeForQuality for why Off and Low both answer 0, for two different reasons: Off
    // because there is no ray-tracing hardware path to assume there, Low because the product decision
    // deliberately excludes it.
    //
    // WHAT DEFAULTING TO IT TRADES AWAY, written down here rather than left for someone to
    // rediscover by eye, because whoever turns this on deserves to know what they traded:
    //   - HARDWARE EARLY-Z. Rasterisation can discard an occluded fragment before its shader ever
    //     runs, for free. A ray has no equivalent -- it pays the full BVH traversal to discover
    //     the same hit was hidden, on every pixel, every frame.
    //   - MSAA. The ray pass is one fullscreen triangle -- there is no per-triangle coverage for
    //     hardware multisampling to resolve, so it always runs at a single sample. The raster path
    //     it replaces runs at this struct's own default of 4x (see Settings::msaa above). The
    //     image is visibly noisier per pixel as a direct result, independent of and in addition to
    //     the RT sun-shadow speckle documented elsewhere.
    //   - TEXTURE. The primary ray returns flat albedo per instance; nothing in that path samples
    //     a texture yet. A rasterised frame does.
    // None of that is softened here because it does not need to be: it is the honest cost of a
    // primary ray today, and it now applies to everyone by default rather than to whoever went
    // looking for a switch.
    u32 rtRenderMode = 1;

    // ---- staged ray-driven passes (milestone 1 split; milestone 4 adds half-rate GI) -----------
    // PSRayDriven above is still ONE fullscreen pixel shader that traces the primary ray,
    // reconstructs the surface, runs the sun-shadow ray, ReSTIR GI, reflections, sky occlusion
    // and shading in a single invocation. This field chooses WHICH SHAPE that work runs in --
    // for 0 and 1 that changes nothing about what gets computed; 2 (below) deliberately does.
    //
    // 0 = SINGLE PASS, THE DEFAULT AND THE COMPARISON BASELINE: today's one drawFullscreen,
    // byte-for-byte unchanged. 1 = STAGED: the same work split into a visibility compute pass
    // (traces the primary ray, writes a per-pixel visibility record), a shadow compute pass
    // (reconstructs the surface from that record and runs the sun-shadow ray, writing sun
    // visibility), then the existing PSRayDriven fullscreen draw reading both instead of tracing
    // and shadowing itself. D3D12 ONLY IN THIS MILESTONE: the renderer falls back to single pass
    // and logs the reason once when staged is requested but anything it needs is missing (a
    // staged pipeline failed to compile, its resources are absent, a non-textured ray-driven PSO
    // is in use, or the active backend is not D3D12), so an opted-in project never silently
    // renders nothing.
    //
    // 2 = STAGED + HALF-RATE GI (milestone 4): the same staged path as 1, but the ReSTIR GI stage
    // traces only HALF the pixels each frame -- NRD's own checkerboard pattern, which half
    // alternates with frame parity -- and REBLUR reconstructs the untraced half from the traced
    // one and history. UNLIKE 1, THIS DELIBERATELY CHANGES THE IMAGE: it trades GI quality and
    // latency for speed, so it is a separate value rather than a flag on 1 -- 1 stays the
    // same-image comparison baseline and 2 is the quality/speed trade. It only differs from 1
    // while ReSTIR GI is the active diffuse estimator (Settings::giMode == 1) AND the NRD denoiser
    // (Settings::denoiser) is actually denoising it: with the voxel cone gather (giMode == 0) there
    // is no ReSTIR GI stage to checkerboard, and without NRD nothing would fill the untraced half --
    // the untraced pixels display REBLUR's reconstruction -- so in either case 2 behaves as 1 and
    // says so once in the log. Same D3D12-only restriction and same single-pass fallback as 1.
    //
    // NOT ONE OF THE TIER-DERIVED KNOBS: like Settings::giRestirMaxHistory below, this has no
    // ladder rung to fall back to -- 1 exists to A/B the split against the single pass it
    // replaces and 2 is an explicit speed/quality choice, neither is a quality tier. Only
    // meaningful while rtRenderMode itself resolves to primary rays (RenderSettingsResolver.hpp's
    // Resolution::rayDrivenStages). Console: voxi.rayDrivenStages.
    u32 rayDrivenStages = 0;

    // DIAGNOSTIC ONLY: times each staged lighting pass (shadow, GI, sky occlusion, reflections) in
    // its own GPU span with a UAV barrier after it, in place of the one "Voxi RD lighting stages"
    // span they normally share. The barriers stop the four overlapping, so the per-stage sum reads
    // somewhat HIGHER than the shared span -- this answers "which stage costs what", never "what
    // does the frame cost". No effect on the image. Console: voxi.rayDrivenStageTiming.
    bool rayDrivenStageTiming = false;

    // SUB-STAGE SPLIT A: the sun-shadow trace, in two passes instead of one. MEASURED (staged mode
    // 1, Epic): the shadow stage alone costs 4.47 ms of a 19.6 ms frame, tracing rtShadowRays (8 at
    // Epic) disc rays for every non-sky pixel -- but most of a frame is fully lit or fully blocked,
    // where all 8 rays would agree. CSRdShadowProbe traces ONE ray per 8x8 tile first; CSRdShadow
    // then ORs its own tile's 3x3 neighbourhood and skips its per-pixel rays entirely wherever every
    // probe in it agrees, reusing that single verdict instead. NEAR-IDENTICAL IMAGE, not a quality
    // trade the way rayDrivenStages == 2 is: a uniformly-lit or uniformly-blocked region's temporal/
    // spatial filter sees one ray's worth of noise in place of eight's, invisible in practice. ON BY
    // DEFAULT so the split is what ships, not what has to be opted into; falls back to the unsplit
    // CSRdShadow (never the single-pass primary) whenever either new pipeline fails to compile. Only
    // meaningful while rayDrivenStages is 1 or 2. Console: voxi.rayDrivenShadowTiles.
    bool rayDrivenShadowTiles = true;

    // SUB-STAGE SPLIT B: CSRdGi's own candidate trace, in two passes instead of one. MEASURED: the GI
    // stage costs 5.38 ms (4.43 ms already, half-rate via rayDrivenStages == 2's checkerboard) of the
    // same 19.6 ms frame, and checkerboard mode still dispatches every lane -- half of them return
    // immediately, so the wave is never compacted. CSRdGiTrace carries the candidate trace
    // (giTraceInitialCandidate plus the material eval) into its own pass, over a COMPACTED dispatch in
    // checkerboard mode (only the traced half's pixels, not every lane of a half-idle wave); CSRdGi
    // then resamples/shades from that stored candidate instead of tracing its own. SAME IMAGE as
    // rayDrivenStages == 1 in every mode -- this changes which pass traces the ray, not the estimator
    // -- so the saving is occupancy/compaction, not a quality trade. ON BY DEFAULT for the identical
    // A/B-visibility reason rayDrivenShadowTiles gives above; falls back to the unsplit CSRdGi (never
    // the single-pass primary) whenever the matching trace/split pipeline pair for this frame's mode
    // (plain or checkerboard) fails to compile. Only meaningful while rayDrivenStages is 1 or 2.
    // Console: voxi.rayDrivenGiSplit.
    bool rayDrivenGiSplit = true;

    // SUB-STAGE SPLIT C: CSRdRefl's own register-heavy ray plus its bandwidth-heavy spatial history
    // gather (rtReflectionSpatial, up to a 7x7 depth-tested gather of last frame's history), in two
    // passes instead of one. MEASURED: the reflection stage costs 3.65 ms of the same frame the other
    // two splits' own comments measure, one thread paying for a reflection ray, a nested sun-shadow ray
    // and a full material shade at the hit AND the dense spatial gather -- the exact shape that made
    // splitting shade out of the megakernel pay (34 -> 1.8 ms) in the first place. CSRdRefl compiled a
    // second time (AVER_RD_REFL_SPLIT=1) traces the ray and writes a PENDING marker instead of
    // composing wherever a history is bound to gather against; CSRdReflFilter then runs
    // rtReflectionSpatial alone and finishes the compose. SAME IMAGE as the unsplit CSRdRefl -- the
    // centre value round-trips through the same RGBA16F reflection history texture it is already
    // written to, same precision as the neighbours and the final RGBA16F output -- not a quality trade
    // the way rayDrivenStages == 2 is. ON BY DEFAULT for the identical A/B-visibility reason
    // rayDrivenShadowTiles/rayDrivenGiSplit give above; falls back to the unsplit CSRdRefl (never the
    // single-pass primary) whenever either new pipeline fails to compile. Only meaningful while
    // rayDrivenStages is 1 or 2. Console: voxi.rayDrivenReflSplit.
    bool rayDrivenReflSplit = true;

    // ---- staged ray-driven bit-field toggles (cb_.giShadowParams.w / gGiShadowParams.w) ---------
    // Three independent RUNTIME toggles packed into one integer bit-field riding the fourth
    // component of the GI-only shadow map's params row -- see FrameConstants::giShadowParams's own
    // comment (VoxiRenderer.hpp), which used to say that component was unused. VoxiRenderer::prePass
    // packs these three bools into cb_.giShadowParams[3] every frame (bit 1/2/4 below); the HLSL side
    // decodes it as `uint bits = (uint)gGiShadowParams.w`. Each is OFF BY DEFAULT in this change --
    // they will be A/B-measured headlessly on the owner's NewSponza view and only flipped on if they
    // measure well.

    // T1 (bit 1): the sun-shadow ray fired FROM A SECONDARY HIT -- rtReflection's hit and ReSTIR GI's
    // candidate hit -- normally walks rtShadow's full transmittance loop (up to 8 steps, RAY_FLAG_NONE,
    // AVER_RT_MASK_ALL) so it can tint light through glass. With this on, both call sites fire ONE ray
    // instead (RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH against the opaque-including-cutouts mask the
    // primary/visibility rays already use). TRADE: translucent (glass/water) instances stop casting a
    // shadow for these two secondary rays -- they read fully lit through glass rather than tinted.
    // Primary shadows (the camera cascades, the shadow probe pass) are not touched. Console:
    // voxi.rtSecondaryShadowOpaque.
    bool rtSecondaryShadowOpaque = false;

    // T2 (bit 2): rtSkyOcclusionTemporal skips its rtAmbientTraced call for an entire 8x8 TILE on this
    // frame's skip parity, wherever that tile's reprojected history is valid this frame -- the
    // temporal blend keeps the reprojection as the fresh estimate instead, still written back to
    // history and still spatially filtered. Whole tiles skip together (a whole compute wave), not a
    // per-pixel checkerboard -- a per-pixel pattern leaves every wave half occupied and saves nothing,
    // the same lesson half-rate GI measured before its own compaction. A pixel with no valid history
    // always traces. MEASURED: sky occlusion costs 0.73 ms of the staged mode 1, 15.4 ms frame.
    // Console: voxi.rtSkyOcclusionHalfRate.
    bool rtSkyOcclusionHalfRate = false;

    // T3 (bit 4): rtReflectionTemporalEx skips its rtReflection trace for a ROUGH pixel (lobeRough > 0
    // -- mirrors always retrace, since a reprojected mirror reflection is visibly wrong the instant
    // the camera moves) on a skip-parity tile whose reflection history reprojects validly: the
    // reprojected history becomes this frame's colour, same as the existing tiled "not my turn"
    // branch already does. MEASURED: reflection trace costs 3.11 ms (+0.39 ms filter) of the staged
    // mode 1, 15.4 ms frame. Console: voxi.rtReflectionHalfRate.
    bool rtReflectionHalfRate = false;

    // ---- the acceleration-structure "unchanged" gate ------------------------------------------
    // MEASURED on the owner's static NewSponza scene: the "Voxi acceleration structures" GPU span
    // costs 0.42 ms every single frame -- a from-scratch ctx.buildTlas (PREFER_FAST_TRACE, no update
    // flags; the RHI has no refit verb) plus an unconditional instance-buffer rewrite and upload,
    // recomputing the identical answer on a scene that has not moved. Same trick as the GI rebuild
    // gate (Settings has no equivalent field for that one; giUpdateInterval only amortises it): hash
    // what buildAccelerationStructures() reads from the draw list, and if nothing moved, leave tlas_,
    // rtInstanceData_ and every SRV bound to them exactly as they are. See
    // VoxiRenderer::rtAccelSnapshotUnchanged()'s own comment for exactly what "unchanged" checks and
    // the two things that force a real rebuild regardless (a compute-skinned mesh present, since its
    // BLAS is refreshed every frame; a cached BLAS handle the resource factory no longer attributes to
    // its mesh).
    //
    // ON BY DEFAULT: it can only ever skip work whose output would be bit-identical, the same
    // "only ever skips an identical answer" guarantee the GI gate gives, so unlike a staged/split
    // dial this is not a quality trade to weigh -- turning it off costs frame time and buys nothing
    // measurable in return. Console: voxi.rtSkipUnchangedTlas.
    bool rtSkipUnchangedTlas = true;

    // ---- path tracing -----------------------------------------------------------------------
    // WHERE RAY TRACING ENDS AND PATH TRACING BEGINS, because this file already draws that line
    // and this setting was on the wrong side of it. RAY TRACING is discrete rays answering a
    // specific question -- is this point in shadow, what does this mirror see, what surface does
    // this pixel see -- and every one of those is one hit and direct lighting. PATH TRACING is the
    // multi-bounce light-transport solve. They are separate settings (rayTracing / pathTracing
    // above) because they are separately useful, separately priced and separately supported.
    //
    // This was `rtBounces`, derived from the rayTracing tier, which meant a project with
    // `pathTracing = Off` could be running a path tracer -- a setting reading "off" while the
    // thing it names is on. Bounces belong to pathTracing and are derived from it.
    //
    // 1 means NO extra bounces: one hit, direct lighting, which is ray tracing. Above 1 is path
    // tracing, and VoxiRenderer refuses to spend it while pathTracing is Off regardless of what
    // is stored here -- see ptBounceParams, which is where that is enforced rather than trusted.
    u32 ptBounces = 1;

    // ---- occlusion-aware fog: the air sky-visibility volume -----------------------------------
    // WHAT THIS FIXES. shared_prelude.hlsl's height fog (averFogFactor/averFogInscatter/
    // averApplyFogEx) and the aerial-perspective term both add in-scattered SKY light along the
    // camera-to-surface path with no regard for what is actually between the two -- correct outdoors,
    // where the air really does see the sky, and wrong inside an enclosed space, where it mostly does
    // not. MEASURED on Sponza's arcade: fog alone adds roughly 6% of sky radiance over a 30 m indoor
    // corridor, brighter than the bounce-lit walls it is layered over, which reads as a flat blue veil
    // rather than air. With fog off, ReSTIR GI already matches the path-traced reference within 8% --
    // this is fog's own error, not the GI estimator's.
    //
    // ON BY DEFAULT. Off is exactly today's fog, byte for byte (VoxiRenderer binds a 1x1x1 placeholder
    // at t17/u16 and voxiAirVisibility() -- voxi.hlsl -- returns 1 unconditionally whenever it sees
    // one, which is the identical "no data, assume open" answer this feature does not otherwise
    // change): turning this off is never a downgrade in image quality relative to every build before
    // this field existed, only a reversion to the pre-existing over-bright indoor fog.
    //
    // WHY NOT SURFACE AO (a previous attempt, reverted -- see aver-fog-skyvis-failed.md). Fog is a
    // property of the camera-to-SURFACE PATH, not the surface's own hemisphere, and the temporally-
    // accumulated AO history this would have reused flashes white on disocclusion (a fast camera pan
    // resets AO to "open" before it reconverges) -- an artifact fog, which is visible on every frame a
    // still image is captured from, cannot afford. The volume this field switches on instead is
    // WORLD-SPACE and has no per-pixel history and no jitter of any kind: CSAirVis (voxi.hlsl) marches
    // fixed hemisphere directions through the SAME voxel grid the GI cone gather already reads. World
    // space and recomputed from the current voxels, never accumulated, so camera motion cannot make
    // it flash.
    //
    // COST. One more compute pass, CSAirVis, over a FIXED 32^3 volume (VoxiRenderer::
    // kAirVisResolution) independent of Settings::voxelResolution -- see that constant's own comment
    // for why a small fixed grid is enough for path occlusion where the GI radiance volume itself
    // needs far more. It refreshes one slab of z-layers per frame, round-robin (a full 48^3 pass
    // measured ~10 ms, and voxel rebuilds are frequent under motion, while sky visibility only changes
    // with geometry), and the shade-side read (voxiAirVisibility()) is eight fixed texture taps down
    // the existing fog ray, no extra ray of its own.
    //
    // DEVICE-GATED, NOT JUST SETTING-GATED: CSAirVis needs SM 6.0 and DXC (VoxiRenderer::
    // airVisWanted()); a device without either keeps the placeholder bound and this setting has no
    // effect, the identical fallback shape Settings::rayDrivenStages already has for its own staged
    // compute pipelines.
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
    // Returns whether this feature's refusal has already been logged once by setSettings' refuse()
    // lambda (Voxi.cpp) since the last setDeviceInfo call -- refusalLogged_ is reset there. Exists so
    // a load-time contradiction report elsewhere (RenderSettingsResolver.hpp's manifestContradictions,
    // read by the manifest loaders) can skip warning about a device limitation refuse() already told
    // the log about, rather than saying the same thing twice from two different call sites.
    bool refusalLogged(Feature f) const { return (refusalLogged_ & (1u << static_cast<u32>(f))) != 0; }

    // Returns true once after settings.msaa changes, then clears the flag.
    bool consumeMsaaDirty();

    // ---- per-history reset requests: EditorConsole.hpp raises these, SandboxApp.cpp consumes and
    // forwards them to the live VoxiRenderer once a frame (mirrors consumeMsaaDirty's own shape
    // exactly) -- this settings-service singleton has no path to VoxiRenderer's private instance
    // itself, only the console does the raising and only SandboxApp owns the renderer to forward to.
    // resetaohistory is an honest ALIAS of resetrthistory today (VoxiRenderer::resetAoHistory's own
    // comment has the full reason); requestAoHistoryReset exists anyway so the two commands stay
    // textually distinct all the way through, in case that stops being true later.
    void requestGiHistoryReset()  { giHistoryResetRequested_ = true; }
    void requestRtHistoryReset()  { rtHistoryResetRequested_ = true; }
    void requestAoHistoryReset()  { aoHistoryResetRequested_ = true; }
    void requestNrdHistoryReset() { nrdHistoryResetRequested_ = true; }
    // Each returns true once after its matching request*Reset() call, then clears itself -- same
    // one-shot contract as consumeMsaaDirty().
    bool consumeGiHistoryResetRequest();
    bool consumeRtHistoryResetRequest();
    bool consumeAoHistoryResetRequest();
    bool consumeNrdHistoryResetRequest();

    // Returns a feature's display name.
    static const char* featureName(Feature f);
    // Returns a quality level's display name.
    static const char* qualityName(Quality q);
    // Returns the voxel grid edge a GI quality tier resolves to when setSettings derives
    // voxelResolution from a tier change -- see setSettings and Voxi.cpp for the ladder and why it
    // only ever applies when the caller left voxelResolution untouched.
    static u32 voxelResolutionForQuality(Quality q);
    // Total cones for the diffuse gather, including the axial one. See Settings::giCones.
    static u32 giConesForQuality(Quality q);
    // U1: how much of F2/F3's cost each GI tier pays for. See Settings::giRestirVisibility.
    static u32 giRestirVisibilityForQuality(Quality q);
    static u32 refractionForQuality(Quality q);
    // Returns the revoxelisation interval a GI quality tier resolves to, derived by setSettings on a
    // tier change under exactly the same "only if the caller left it untouched" rule as the grid edge
    // above. Epic is 1 -- always fresh -- so the top tier's indirect light is unchanged by this.
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
    // 1 for every tier that runs ray tracing at all EXCEPT Low, which rasterises primary visibility
    // by explicit product decision (D3) -- 0 for Off and for Low, 1 for Medium, High and Epic. See
    // ladder::rtRenderMode (QualityLadder.hpp) for why Off and Low answer the same value for two
    // different reasons.
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
    bool nrdHistoryResetRequested_ = false;
    u32 refusalLogged_ = 0;   // one bit per Feature: its refusal has already been logged
};

} // namespace aver::voxi
