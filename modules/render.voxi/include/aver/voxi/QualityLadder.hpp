// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#pragma once
#include "aver/voxi/Voxi.hpp"

// THE QUALITY LADDER: every Off..Epic rung Renderer::setSettings derives a knob from, one function per
// knob, with the reasoning directly above it -- keeping switch and rationale together avoids the
// "torn pair" bug this project has hit twice (giCones, rtPixelsPerRayTile below), where an edit
// orphaned a comment from the function it justified.
//
// Voxi.cpp's Renderer::XForQuality bodies are one-line forwards (`return ladder::X(q);`), kept because
// they're the settings service's exported API and this header isn't; Voxi.cpp includes this header, so
// the static_asserts at the bottom run on every build of Aver.Render.Voxi.
namespace aver::voxi::ladder {

// ================================================================= Global Illumination ===========

// VOXEL GRID EDGE per GI tier (Renderer::setSettings, Voxi.cpp, derives it only on a tier CHANGE, and
// only if voxelResolution was left untouched). Doubling the edge is 8x memory/cost (volume is edge
// CUBED). Medium keeps the long-standing fixed default (128, so tuned projects see no change); Off
// shares Low's cheapest grid since voxelizePass/filterMips skip while GI is disabled
// (VoxiRenderer::prePass), so a smaller grid there costs only bytes, not frame time. VRAM ONLY,
// radiance volume (RGBA16F, full mips) + R32_UINT injection accumulator:
//   Off / Low (64): ~6 MB   Medium (128): ~50 MB (default)   High (256): ~400 MB
//   Epic (512): ~3.2 GB -- steepest rung by far, only for a GPU with headroom to spare
// FRAME-TIME COST PER RUNG IS UNMEASURED -- the volume is built once at VoxiRenderer::init and never
// resized (createVoxelVolume), so no probe has timed 256^3/512^3 against the 128^3 default.
constexpr u32 voxelResolution(Quality q) {
    switch (q) {
        case Quality::Off:
        case Quality::Low:    return 64;
        case Quality::Medium: return 128;
        case Quality::High:   return 256;
        case Quality::Epic:   return 512;
        default:              return 128;
    }
}

// PER-PIXEL DIFFUSE GATHER CONE COUNT -- the GI knob that actually costs anything. globalIllumination
// already derives voxelResolution and giUpdateInterval (volume BUILD, 0.4-0.5 ms); the per-pixel
// GATHER, measured at 1.3 ms and the larger half, was a hardcoded six at every tier -- turning GI down
// bought almost nothing, and High made the frame SLOWER for no benefit (6.0 -> 6.4 ms: a bigger volume
// to sample, same sample count). See Settings::giCones (Voxi.hpp). MEDIUM IS SIX, the old hardcoded
// value for every tier -- the struct default must equal it or the derivation never fires (only runs on
// a tier CHANGE; see the static_asserts at file end). This function is the header intro's own
// torn-pair example: it once sat, misplaced, above voxelResolution's comment instead of its own.
//
// EVIDENCE: ~0.22 ms/cone, from bisecting the gather pass directly and confirmed against the ladder's
// own scene-draw times (FirstPerson range): Low (3) 3.3 ms, Medium (6) 4.0 ms, High (9) 4.6 ms, Epic
// (13) 5.4 ms -- 2.1 ms over 10 cones = 0.21 ms/cone, agreeing independently. The gather AVERAGES
// (normalised by cosine weight), so fewer cones is a coarser estimate, not a darker one.
//
// NOT REPEATED HERE: VoxiShaders.hpp's PSRayDriven cost note claims two cones cost only 2/255 on a
// probe -- but that measured the OLD six-cones-everywhere behaviour, not this ladder's 3-cone Low rung,
// and nothing here has retested it. Do not cite it as settled for a rung below Low without a fresh
// probe against this derivation (aver-unbacked-verification.md).
constexpr u32 giCones(Quality q) {
    switch (q) {
        case Quality::Off:    return 6;   // inert: nothing gathers with GI off
        case Quality::Low:    return 3;
        case Quality::Medium: return 6;
        case Quality::High:   return 9;
        case Quality::Epic:   return 13;
        default:              return 6;
    }
}

// FRAMES BETWEEN GI RE-VOXELISATIONS. 1 = every frame; N>1 reuses the previous voxelise+filter
// (VoxiRenderer::voxelizePass/filterMips) for N-1 frames, trading indirect-light LATENCY (up to N-1
// frames) for amortised cost, not resolution. See Settings::giUpdateInterval (Voxi.hpp); struct default
// must equal Medium's rung below.
//
// MEDIUM MOVED 4 -> 1 -> 2. An earlier claim that interval 1 cost 187.9 ms vs 104.5 ms at interval 4 is
// REFUTED: re-measured (Release, ElectricDreams, 1600x900, --no-vsync, --frames 200), intervals 1/2/4/8
// median 18.54/18.47/18.50/18.46 ms -- 0.09 ms spread, noise on a STILL camera.
//
// BUT under a wobbling camera the "Voxi GI update" span went 36.00 ms (interval 1) to 16.57 ms
// (interval 4) (aver-gi-update-dominates-under-motion.md; Sponza, --cam-wobble 15 50, ray-driven +
// AverSR Balanced) -- amortisation DOES cost real time in motion. That run also amortised RT shadows
// (tile 4), and beginShadowHistory sits in the same span, so crediting the whole gap to this knob alone
// is UNCONFIRMED; lag at interval 4 is itself UNMEASURED (the source note says so directly: "both
// untested").
//
// MEDIUM'S CURRENT RUNG (2) IS UNMEASURED FOR COST AND LAG under motion; this doesn't reopen 4 vs 1
// above -- 4 still visibly trails. Known, not measured: the volume rebuilds every other frame, so lag
// is at most one frame, and a still scene converges to interval 1's image either way
// (Renderer::setSettings, Voxi.cpp). LOW MOVED 8 (kMaxGiUpdateInterval, the clamp's widest) -> 4 by the
// same still-camera reasoning just refuted for Medium; 4 leans on the motion evidence above, and 8
// remains UNMEASURED under motion.
//
// HIGH AND EPIC STAY AT 1: Epic means bit-identical to always-fresh; High has no motion evidence to
// justify trading against.
constexpr u32 giUpdateInterval(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;   // GI is not running; the value is inert either way
        case Quality::Low:    return 4;
        case Quality::Medium: return 2;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;   // always fresh -- bit-identical to the old behaviour
        default:              return 1;   // an unknown tier must not silently degrade lighting
    }
}

// RESTIR GI VISIBILITY (U1): how much of F2 (candidate-hit sky) and F3 (reuse visibility) -- the
// contrast fix's two per-pixel rays, cb4b48df -- each rung pays for. See Settings::giRestirVisibility
// (Voxi.hpp) for what NoRay/Reconstructed/HalfResolution/Full change in the shader; struct default (2,
// HalfResolution) must equal Medium's rung below. HIGH AND EPIC (Full) ARE YOUR DECISION (U1), named
// explicitly, and byte-identical to what cb4b48df shipped -- this ladder's "no compromise" rungs
// elsewhere too (RayTraced refraction, widest cone/shadow-ray counts).
//
// LOW AND MEDIUM ARE MINE, BOTH UNMEASURED:
//   Low = Reconstructed (1): RT Low rasterises (rtRenderMode returns 0), so F2/F3 would otherwise cost
//   once per shaded FRAGMENT (PTTest's depth prepass is off, PTTest.ocproject:21, multiplying overdraw);
//   Reconstructed traces no extra ray, only the diffuse gather's own cone march (still paid per
//   fragment -- optimisation-wave-2 plan section 2.10 F has the remaining cost, UNMEASURED). NoRay was
//   rejected: it reintroduces the over-brightness cb4b48df's contrast fix removes, at the tier that
//   fix touches AO least.
//   Medium = HalfResolution (2): ray-driven (rtRenderMode returns 1), cost once per PIXEL; traces exact
//   visibility on 1-in-4 pixels at rest (<=0.5 rays/pixel vs Full's 2), and a pixel with no valid
//   history traces as Full, so the worst frame costs no more than Full's. Neither rung is timed; the
//   plan's section 5 by-hand verification settles both before shipping.
//
// OFF RETURNS 3 (Full), INERT: with GI Off, ReSTIR never runs (RequiresRestirGi, RenderSettingsResolver
// .hpp), so this value is unread; Full is the safe no-op answer, same convention as giCones(Off) = 6.
constexpr u32 giRestirVisibility(Quality q) {
    switch (q) {
        case Quality::Off:    return 3;   // inert: ReSTIR GI is not running (RequiresRestirGi)
        case Quality::Low:    return 1;   // Reconstructed -- mine, UNMEASURED (RT Low rasterises)
        case Quality::Medium: return 2;   // HalfResolution -- mine, UNMEASURED (Medium is ray-driven)
        case Quality::High:   return 3;   // Full -- your decision (U1)
        case Quality::Epic:   return 3;   // Full -- your decision (U1)
        default:              return 3;   // an unknown tier must not silently under-correct the contrast fix
    }
}

// ================================================================= Ray Tracing ====================

// WHICH REFRACTION MODE THE TIER ASKS FOR. RayTraced only at the top two rungs -- it spends a ray per
// translucent pixel on the pass that is already the bottleneck; ScreenSpace is nearly free (reuses the
// absorption path's backdrop copy) and is the sensible middle. Off at Quality::Off keeps the "tier off
// means feature off" contract every other knob honours. See Settings::refractionMode (Voxi.hpp), and
// RenderSettingsResolver.hpp for what a RayTraced REQUEST resolves to when the device/tier can't honour
// it (ScreenSpace, one rung down -- not this ladder's Off rung). MEDIUM IS THE DEFAULT TIER, so its
// rung must equal Settings::refractionMode's own default or the change-gated derivation in
// Renderer::setSettings never fires on a default device.
//
// EVIDENCE the rungs differ in output: 31.00/31.51/12.40% of pixels changed between the three modes
// (aver-refraction.md). Cost of RayTraced (mode 2) is UNMEASURED.
constexpr u32 refraction(Quality q) {
    switch (q) {
        case Quality::Off:    return 0;   // Off
        case Quality::Low:    return 1;   // ScreenSpace
        case Quality::Medium: return 1;   // ScreenSpace  <- Settings::refractionMode's default
        case Quality::High:   return 2;   // RayTraced
        case Quality::Epic:   return 2;   // RayTraced
        default:              return 1;
    }
}

// OCCLUSION RAYS PER PIXEL for the ray-traced sun shadow when this pixel traces. Clamped to [1,
// VoxiRenderer::kMaxShadowRays] (Renderer::setSettings). Struct default must equal Medium's rung below
// (Settings::rtShadowRays, Voxi.hpp). LOW AND MEDIUM ARE BOTH 1: each extra ray under the raster path
// costs ~1.55 ms (ElectricDreams 1600x900, 18.44/19.99/23.39 ms at 1/2/4 rays; full table at
// Settings::rtPixelsPerRayTile, Voxi.hpp). Low now runs that same raster path (D3, rtRenderMode below)
// and keeps the cheapest count.
//
// HIGH IS 4 (was 2), EPIC IS 8 (was 4) -- retuned, moved up a step.
//   COST: a different path -- inside ray-driven primary, an extra SUN ray costs ~0.017 ms
//   (Settings::giSkyOcclusionRays, Voxi.hpp; Sponza), ~2 orders cheaper than a raster ray (coherent,
//   shared BVH traversal). Cost of replaying it under the blended pass a second time is UNMEASURED.
//   QUALITY: EVIDENCE only for the OLD config (raster, unfiltered, still camera): penumbra error
//   +27/+27/+9/0 codes at 1/2/4/8 rays (aver-denoiser-findings.md, predates ray-driven-default and any
//   rtShadowDenoise radius). Under TODAY's config (ray-driven + spatial filter, under motion) it is
//   UNMEASURED. That note's "don't change the default tier without the owner" concerns Medium (left at
//   1); High/Epic are not the default tier, and this retune is the owner's own direction (D1).
constexpr u32 rtShadowRays(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;   // RT is not running; the value is inert either way
        case Quality::Low:    return 1;
        case Quality::Medium: return 1;
        case Quality::High:   return 4;
        case Quality::Epic:   return 8;
        default:              return 1;   // an unknown tier must not silently cost more
    }
}

// EDGE LENGTH of the square tile one traced pixel is amortised over via the ray-traced shadow's
// temporal history: 1 = every pixel traces every frame (bit-identical to no denoiser); N>1 = one pixel
// per NxN tile traces per frame, rotating, while the rest reuse a reprojected history sample. See
// Settings::rtPixelsPerRayTile (Voxi.hpp) for the full contract and measured table.
// 1 AT EVERY RUNG, LOW INCLUDED. Amortisation measured 0.14 ms moving / 0.09 ms static against tile 1 --
// inside the noise -- while the motion trail is already visible on 0.80% of pixels at tile 2 alone
// (tile 4 adds only 0.04% more; fully present by tile 2, no partial-credit rung): a fault anyone moving
// the camera can see, traded for milliseconds nobody could measure.
//
// LOW WAS 4 ("widest amortisation that pays"), measured only on a STILL camera, which can't see what
// the tile actually spends: visible shadow trail during Play-in-Editor. Same trap as giUpdateInterval's
// Medium rung above -- both hide their cost from a benchmark that never pans.
//
// TORN PAIR, CAUGHT BY THE GATE SUITE not a person: this function said 4 for Low while Voxi.hpp's field
// comment said 1. The amortisation stays available at any tier via RENDER.RTPIXELSPERRAY /
// --rt-pixels-per-ray (both outrank the manifest) -- a decision, not a silent preset default.
constexpr u32 rtPixelsPerRayTile(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;
        case Quality::Low:    return 1;
        case Quality::Medium: return 1;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        default:              return 1;   // an unknown tier must not silently reintroduce the history
    }
}

// SPATIAL DENOISE RADIUS, in pixels, for the ray-traced sun shadow. 0 = unfiltered; N>0 averages a
// (2N+1)^2 shadow-history neighbourhood, weighted by depth agreement with this pixel's plane. See
// Settings::rtShadowDenoise (Voxi.hpp) for the mechanism and how it differs from the TEMPORAL
// rtPixelsPerRayTile knob above.
// COST IS NOT THE DISCRIMINATOR (true at every rung): tap count is (2*radius+1)^2, measured at +0.02 ms
// for the full 49-tap radius-3 kernel vs +1.69 ms for one more traced ray (VoxiRenderer.cpp) -- ~85x
// cheaper even at the widest rung. What varies by tier is raw noise to hide vs how much drift -- 12 to
// 32 codes under a six-degree wobble, growing with radius (Settings::rtShadowDenoise, Voxi.hpp) -- is
// worth risking.
//
// LOW AND MEDIUM ARE BOTH 2: both trace one ray (rtShadowRays above), so the raw term is a hard dither.
// UNMEASURED AT LOW (filter runs per FRAGMENT, multiplying tap count by overdraw under Low's rasterised
// path, D3) -- kept at 2 because MSAA only softens silhouettes, not an interior one-ray penumbra
// (whether PSMainVoxi shades per pixel or per MSAA sample is itself UNCONFIRMED).
//
// HIGH IS NOW 1 (was 2), JOINING EPIC: High traces four rays now, real variance reduction over Medium's
// one; no probe compares radius 1 vs 2 at four rays under motion (UNMEASURED). Drift scales with RADIUS
// not ray count, so more rays affords less drift risk -- same logic keeps Epic (eight rays) off 0: real
// variance remains even at eight rays, and radius 1 is a nearly-free improvement. An earlier claim that
// Epic's touch was justified by "nothing here converges below sixteen rays" is WITHDRAWN: contradicted
// by rtShadowRays' own row (eight rays reach the sixteen-ray value at one pixel -- one pixel, not a
// converged image).
//
// NONE OF THE FOUR RUNGS REACH THE CLAMP MAX OF 3: radius 3 measured "within one code of sixteen rays"
// at OLD ray counts (VoxiShaders.hpp, rtShadowSpatial), but tops out the 12-32 code drift range too. Not
// a silent default trade; 3 stays reachable via explicit override, same as voxelResolution.
constexpr u32 rtShadowDenoise(Quality q) {
    switch (q) {
        case Quality::Off:    return 0;   // RT is not running; the filter has nothing to filter
        case Quality::Low:    return 2;
        case Quality::Medium: return 2;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        default:              return 0;   // an unknown tier must not silently start smearing shadows
    }
}

// WHICH THING FINDS THE FIRST SURFACE: 0 = rasteriser, 1 = a primary ray per pixel. See
// Settings::rtRenderMode (Voxi.hpp) for the mechanism and what the ray-driven default trades away.
//
// 1 FROM MEDIUM UP; 0 AT OFF (no acceleration structure to trace against) AND AT LOW (EXPLICIT PRODUCT
// DECISION, D3, not a hardware gap -- the owner wants the ray-driven "Wavefront Primary rays" path live
// from Medium up, Low kept on the rasteriser deliberately).
//
// D3'S EVIDENCE IS PARTIAL, NOT A CLAIM RASTER IS FASTER: at overview cameras raster measures
// slower (ElectricDreams 20.55 vs 8.05 ms; PTTest, PT off, 13.51 vs 8.04 ms) -- partly this struct's 4x
// MSAA default, which raster pays and single-sample ray-driven doesn't (an old pre-texture-split
// measurement put MSAA's own cost at 4.2 ms, 16.0 vs 11.8; today's share is UNMEASURED). A close-up case
// once favoured raster (9.02 vs 24.04 ms) is UNCONFIRMED -- that run also had Path Tracing silently
// running instead of ray-driven, so it wasn't the comparison claimed. Low CAN be slower than Medium at
// an overview camera; D3 is the owner's decision made with that in view, not a speed claim.
constexpr u32 rtRenderMode(Quality q) {
    switch (q) {
        case Quality::Off:    return 0;   // no acceleration structure to trace against
        case Quality::Low:    return 0;   // D3: deliberately excluded from the ray-driven default
        case Quality::Medium: return 1;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        default:              return 0;   // unknown tier keeps the pre-existing rasteriser path
    }
}

// SKY-VISIBILITY RAYS the AMBIENT term traces per pixel. 0 = estimate from the cone gather (Low only).
// See Settings::giSkyOcclusionRays (Voxi.hpp) for why the rays exist and the Sponza error measurements.
// HIGH AND EPIC, NOT EPIC ALONE (an earlier revision said "Epic only" -- stale once accumulation
// against a reprojected history (rtSkyOcclusionTemporal, rtAoHist_) dropped the ray count 4 -> 1): the
// cost became low enough for High too. Rungs below still return 0 because these rays ADD to a frame
// already tracing shadow rays and cone-marching a volume.
//
// ONE RAY, NOT FOUR: unlike rtShadowRays' coherent sun rays (shared BVH nodes, nearly free), this ray is
// cosine-distributed over the hemisphere and expensive -- one measures 5.37 ms on Sponza, more than four
// sun rays together. Sample count instead comes from FRAMES: ~10 effective samples at weight 0.9 through
// the reprojected history, with independent per-pixel noise that averages away. WHY IT MATTERS: every
// tier returning 0 falls back to the cone gather's own occlusion, MEASURED far too open on Sponza --
// disabling the rays brightens the darkest 81% of the frame 2.6x (mean 20.71 -> 31.92).
//
// MEDIUM TRACES IT TOO (2026-09-27): Medium runs ReSTIR GI, whose cone-gather occlusion is a hardcoded
// 1.0 (no AO at all), rendering twice as bright as Epic (Sponza arcade, AverSR Performance: mean 33.4 vs
// 16.2). One ray at half rate brings it to 16.1 (MAD 0.31 vs Epic) for +0.5 ms (11.78 -> 12.28 ms). LOW
// STAYS ON THE CONE GATHER (RT Low rasterises, nothing to trace against). FIRST SWEEP establishing 0/1/1
// (Sponza, 112 entities, RENDER.RAYTRACING 4, MSAA 2, --no-vsync, --gpu-timing): 0 rays 10.92 ms (probe
// 20,20,22), 1 ray 12.25 ms (probe 11,11,13), 4 rays 15.58 ms (same probe) -- one ray buys the
// correction; three more cost 3.33 ms for no visible change (path-traced reference 7,7,7).
constexpr u32 giSkyOcclusionRays(Quality q) {
    switch (q) {
        case Quality::Off:    return 0;   // no acceleration structure; shader falls back to cone gather
        case Quality::Low:    return 0;
        case Quality::Medium: return 1;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        default:              return 0;   // an unknown tier must not silently cost more
    }
}

// EDGE, IN PIXELS, of the square sharing one sky-occlusion ray direction. Kept at 1 (inert) everywhere,
// now that one accumulated ray replaced the four incoherent rays this tile once made affordable
// (Settings::giSkyOcclusionTile, Voxi.hpp, has the mechanism and the coherence gain the tile used to buy).
// 1 EVERYWHERE, EPIC INCLUDED: the tile traded correlated noise (a 4x4 block of identical AO) for
// coherence across four incoherent rays; with one ray there's nothing left to make coherent. KEPT AS A
// DIAL, not deleted: measurements still stand (+1.98 ms for four rays at tile 4 vs tile 1), and a future
// many-ray term could want it. 1 is the identity, floor(p/1) == p.
constexpr u32 giSkyOcclusionTile(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;
        case Quality::Low:    return 1;
        case Quality::Medium: return 1;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        default:              return 1;   // an unknown tier gets the un-correlated, un-amortised path
    }
}

// ================================================================= Path Tracing ===================

// PATH-TRACING BOUNCE BUDGET. Off = 1 (one hit, direct lighting -- ray tracing, not path tracing); each
// rung above buys bounces. See Settings::ptBounces (Voxi.hpp) for why it's keyed on pathTracing rather
// than the ray-tracing tier. EVIDENCE: 4.7/5.9/6.3 ms at 1/2/4 bounces, outdoors (ElectricDreams,
// 2750x1639). Three bounces is UNMEASURED (interpolated, not timed). Stops at 4: bounces beyond the
// second cost 0.4 ms and changed nothing measurable outdoors (paths escape to sky); a closed interior
// would differ, and whether one exists to test against is UNCONFIRMED.
constexpr u32 ptBounces(Quality q) {
    switch (q) {
        case Quality::Off:    return 1;   // one hit, direct lighting: ray tracing, not path tracing
        case Quality::Low:    return 2;
        case Quality::Medium: return 2;
        case Quality::High:   return 3;
        case Quality::Epic:   return 4;
        default:              return 1;   // an unknown tier must not silently start bouncing
    }
}

// ================================================================= AverSR ==========================

// AVERSR (U2): the spatial-upscale level each OverallQuality rung defaults to. Not laddered like
// globalIllumination/rayTracing/pathTracing above -- AverSR is a PRESENT-SIZE upscale every one of
// those renders BEHIND (smaller internal resolution), so it composes with all of them rather than
// belonging to one. Scalability.hpp's header has why it stays out of voxi::Settings and Custom
// detection (module boundary: render.voxi must never include render.sr); optimisation-wave-2 plan
// section 3.3 has the precedence chain (Display choice, manifest, CLI each outrank this default --
// Scalability.hpp's resolveAverSrLevel is that chain minus the picker UI).
//
// kAverSrOff/Quality/Balanced/Performance mirror aver::sr::Quality's numbering (AverSrQuality.hpp:
// Off=0, Quality=1, Balanced=2, Performance=3) byte for byte, restated rather than included since this
// module must not depend on render.sr -- static_asserts in SandboxApp.cpp/GameMain.cpp (under
// AVER_MODULE_SR) check the two enums agree, catching a reordering at build time.
inline constexpr u32 kAverSrOff = 0, kAverSrQuality = 1, kAverSrBalanced = 2, kAverSrPerformance = 3;

// LEVEL EACH RUNG DEFAULTS TO. Off returns native (kAverSrOff): with GI/RT/PT all off there is nothing
// left to buy back, and an explicit Off shouldn't silently render smaller anyway.
// SUSPECT EVIDENCE (aver-aversr-measured.md; optimisation-wave-2 plan section 3.1): PTTest, 2026-08-31,
// 200 frames -- Quality -4.0 ms/-21.6% edge sharpness; Balanced -4.9/-25.3%; Performance -5.6/-29.4%.
// Baseline was contaminated by a second renderer running concurrently (plan C13) and predates ReSTIR
// and NRD (pixel-bound additions Epic carries today), so absolute SAVINGS are expected higher than
// recorded (UNMEASURED; plan section 5.0/5.2 settles it); the DELTAS below lean on this table too and
// would shift with a corrected baseline.
//
// EPIC AND HIGH TAKE THE GENTLEST LEVEL (Quality, 1) on that evidence: Quality->Balanced trades 0.9 ms
// for 3.7 points of edge sharpness, Balanced->Performance trades 0.7 ms for 4.1 more -- diminishing
// returns. Both are this ladder's "image quality" rungs elsewhere too (Full ReSTIR, RayTraced
// refraction, widest cone/shadow-ray counts), so gentlest AverSR for the same reason, not a separate one.
//
// MEDIUM TAKES BALANCED (2), LOW TAKES PERFORMANCE (3): Low rasterises at this struct's 4x MSAA default
// (rtRenderMode returns 0), keeping real geometric AA at a quarter the internal pixels even at AverSR's
// most aggressive level -- it can afford to spend the most on upscaling because, unlike every ray-driven
// rung above it, its MSAA isn't being spent on nothing (single-sample; rtRenderMode's own note).
constexpr u32 averSrLevel(Quality q) {
    switch (q) {
        case Quality::Off:    return kAverSrOff;           // nothing left to buy back at Off
        case Quality::Low:    return kAverSrPerformance;   // widest -- MSAA already carries the AA cost
        case Quality::Medium: return kAverSrBalanced;
        case Quality::High:   return kAverSrQuality;        // gentlest -- an image-quality rung
        case Quality::Epic:   return kAverSrQuality;        // gentlest -- an image-quality rung
        default:              return kAverSrOff;            // an unknown tier must not silently downscale
    }
}

} // namespace aver::voxi::ladder

namespace aver::voxi {

// TIER-DERIVATION HAZARD, ENFORCED AT COMPILE TIME: Renderer::setSettings derives a knob from its tier
// only ON A CHANGE (Voxi.cpp), so if a struct default and its default tier's ladder rung ever disagree,
// a freshly constructed Settings{} silently ships the wrong number and nothing corrects it (the tier
// never "changes" from its own default). Shipped twice already (giUpdateInterval's history above,
// rtPixelsPerRayTile's torn pair), both caught by a person rereading two files -- these asserts catch it
// at build time instead. C++20 (modules/render.voxi/CMakeLists.txt); Settings is an aggregate of
// literal members, so `Settings{}.x` is a constant expression and every ladder:: function above is
// constexpr. Voxi.cpp includes this header, so every build of Aver.Render.Voxi runs these.
static_assert(Settings{}.globalIllumination == Quality::Medium, "GI's default tier moved");
static_assert(Settings{}.rayTracing         == Quality::Medium, "RT's default tier moved");
static_assert(Settings{}.pathTracing        == Quality::Off,    "PT's default tier moved");

static_assert(Settings{}.voxelResolution  == ladder::voxelResolution(Quality::Medium),
              "voxelResolution's struct default no longer matches GI Medium's ladder rung");
static_assert(Settings{}.giCones          == ladder::giCones(Quality::Medium),
              "giCones' struct default no longer matches GI Medium's ladder rung");
static_assert(Settings{}.giUpdateInterval == ladder::giUpdateInterval(Quality::Medium),
              "giUpdateInterval's struct default no longer matches GI Medium's ladder rung");
static_assert(Settings{}.giRestirVisibility == ladder::giRestirVisibility(Quality::Medium),
              "giRestirVisibility's struct default no longer matches GI Medium's ladder rung");

static_assert(Settings{}.rtShadowRays       == ladder::rtShadowRays(Quality::Medium),
              "rtShadowRays' struct default no longer matches RT Medium's ladder rung");
static_assert(Settings{}.rtPixelsPerRayTile == ladder::rtPixelsPerRayTile(Quality::Medium),
              "rtPixelsPerRayTile's struct default no longer matches RT Medium's ladder rung");
static_assert(Settings{}.rtShadowDenoise    == ladder::rtShadowDenoise(Quality::Medium),
              "rtShadowDenoise's struct default no longer matches RT Medium's ladder rung");
static_assert(Settings{}.rtRenderMode       == ladder::rtRenderMode(Quality::Medium),
              "rtRenderMode's struct default no longer matches RT Medium's ladder rung");
static_assert(Settings{}.refractionMode     == ladder::refraction(Quality::Medium),
              "refractionMode's struct default no longer matches RT Medium's ladder rung");
static_assert(Settings{}.giSkyOcclusionRays == ladder::giSkyOcclusionRays(Quality::Medium),
              "giSkyOcclusionRays' struct default no longer matches RT Medium's ladder rung");
static_assert(Settings{}.giSkyOcclusionTile == ladder::giSkyOcclusionTile(Quality::Medium),
              "giSkyOcclusionTile's struct default no longer matches RT Medium's ladder rung");

static_assert(Settings{}.ptBounces == ladder::ptBounces(Quality::Off),
              "ptBounces' struct default no longer matches PT Off's ladder rung");

} // namespace aver::voxi
