// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#pragma once
#include "aver/voxi/Voxi.hpp"

// THE QUALITY LADDER: every Off..Epic rung Renderer::setSettings can derive a knob to, in one place,
// with the reasoning that justifies each rung's value living directly above it.
//
// THIS HEADER USED TO BE TEN SEPARATE Renderer::XForQuality BODIES SCATTERED ACROSS Voxi.cpp, each
// with its own reasoning comment sitting wherever that function happened to land relative to its
// neighbours -- and at least one of those pairs was already TORN apart by an unrelated insertion (see
// giConesForQuality's own "NOTE ON WHERE THIS COMMENT USED TO LIVE" below for one instance this
// project already caught and fixed, and rtPixelsPerRayTileForQuality's "LOW WAS 4" for a second one
// the gate suite caught instead of a person). Concentrating both the switch statement and its
// reasoning here, one function at a time, removes the whole class of bug where an edit two functions
// away silently orphans the comment that used to justify it.
//
// Voxi.cpp's Renderer::XForQuality bodies are now one-line forwards to these (`return ladder::X(q);`),
// kept only because they are the settings service's public, exported API and this header is not --
// Voxi.cpp includes this header, so the static_asserts at the bottom run on every build of
// Aver.Render.Voxi, not merely on whatever includes this file directly.
namespace aver::voxi::ladder {

// ================================================================= Global Illumination ===========

// THE VOXEL GRID EDGE each GI quality tier resolves to (see Renderer::setSettings, Voxi.cpp, for when
// this actually applies -- only on a tier CHANGE, and only if the caller left voxelResolution
// untouched in the same call). Doubling the edge is an 8x jump in both memory and per-voxel GPU cost
// -- the volume is resolution CUBED -- so this ladder is deliberately conservative: Medium keeps the
// long-standing fixed default (128) so any project already tuned around Medium sees no change; Off
// and Low share the cheapest grid, since Off's volume is otherwise idle VRAM (voxelizePass/filterMips
// are skipped whenever GI is disabled -- see VoxiRenderer::prePass -- so a smaller grid there costs
// nothing in frame time, only in bytes reserved).
//
// EVIDENCE IS VRAM ONLY. Approximate size for the radiance volume (RGBA16F, full mip chain) plus the
// R32_UINT injection accumulator that sits beside it, at each rung:
//   Off / Low (64):   ~6 MB
//   Medium    (128):  ~50 MB   (today's fixed default, unchanged)
//   High      (256):  ~400 MB
//   Epic      (512):  ~3.2 GB  -- by far the steepest rung; only for a GPU with gigabytes to spare
// FRAME-TIME COST PER RUNG IS UNMEASURED. The volume is built once, at VoxiRenderer::init, and never
// resized afterward (createVoxelVolume is only called there) -- so nothing in this ladder has ever
// timed what a 256^3 or 512^3 grid actually costs per voxelise/filter pass against the 128^3 default;
// only its footprint on the card is on record.
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

// THE PER-PIXEL DIFFUSE GATHER'S CONE COUNT -- the GI setting that actually costs anything, and until
// this ladder existed the one number globalIllumination did not touch. globalIllumination already
// derives voxelResolution above and giUpdateInterval below, both of which move the volume BUILD
// (measured at 0.4-0.5 ms); the per-pixel GATHER, measured at 1.3 ms and by far the larger half, was a
// hardcoded six for every tier. Turning GI down bought almost nothing, and turning it up to High made
// the frame SLOWER with no way to spend the extra budget (6.0 -> 6.4 ms: a bigger volume to sample,
// same number of samples). See Settings::giCones (Voxi.hpp) for the field itself.
//
// MEDIUM IS SIX, WHICH IS WHAT EVERY TIER USED TO TRACE. The default rung is deliberately the old
// hardcoded number: this ladder changes what the LADDER does, not what a default project looks like,
// and the struct default must equal the default tier's rung or the derivation never fires at all (it
// only runs on a tier CHANGE) -- see the static_asserts at the bottom of this file.
//
// (NOTE ON WHERE THIS COMMENT USED TO LIVE: in Voxi.cpp this block once sat above
// voxelResolutionForQuality by mistake -- giConesForQuality had been inserted ahead of that function
// without moving that function's own VRAM-table comment down with it, leaving the table describing
// voxel grid memory attached to a function about cone count. It stayed that way for one stage of this
// feature's history before being caught: trace the code, not the comment above it. Both comments now
// sit directly above their own function, in this header, which is the whole reason this header
// exists.)
//
// EVIDENCE, not a guess: about 0.22 ms per cone, first measured by bisecting the gather pass directly,
// and confirmed a second way against the ladder itself -- FirstPerson range, scene draw per tier: Low
// (3 cones) 3.3 ms, Medium (6) 4.0 ms, High (9) 4.6 ms, Epic (13) 5.4 ms; a 2.1 ms spread across 10
// cones is 0.21 ms/cone, independently agreeing with the bisection. The gather is a weighted AVERAGE,
// normalised by the sum of the cosine weights, so changing the count changes how well the hemisphere
// is sampled rather than how bright the result is: fewer cones is a coarser estimate of the same
// quantity, not a darker one.
//
// A NUMBER DELIBERATELY NOT REPEATED HERE, because this file cannot back it: a comment elsewhere in
// this tree (VoxiShaders.hpp, PSRayDriven's cost investigation, a file this one does not own) states
// "a prior measurement found dropping to two cones moved a probe by 2/255" and reads that as room to
// cut further below Low's 3-cone rung. That claim predates this ladder -- whatever it measured, it
// measured against the OLD behaviour, six hardcoded cones at every tier, not against the 3-cone Low
// rung this file now actually ships -- and neither the bisection nor the FirstPerson table above
// reproduces or contradicts it, because neither was run to test a two-cone rung. Restating it here as
// settled would be exactly the trap this project's own memory keeps a standing note against
// (aver-unbacked-verification.md): a described test result nobody re-ran against the code as it
// stands today. If two cones is ever proposed as a rung below Low, it needs its own probe capture
// against THIS derivation, not a number carried over from before the derivation existed.
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

// HOW MANY FRAMES APART THE GI VOLUME IS RE-VOXELISED. 1 revoxelises and re-filters every frame; N>1
// reuses the previous frame's voxelised+filtered volume for the N-1 frames in between, amortising the
// voxelise-rasterise pass and the mip filter chain (VoxiRenderer::voxelizePass/filterMips) at the cost
// of indirect lighting lagging scene changes by up to N-1 frames -- a visible LATENCY trade, not a
// resolution one. See Settings::giUpdateInterval (Voxi.hpp) for the field itself and why the struct
// default has to equal this ladder's Medium rung.
//
// MEDIUM MOVED 4 -> 1 -> 2, in that order, and each move has its own evidence.
//
// 4 -> 1 (the first move): 4 IS WHAT MAKES LIGHTING TRAIL THE CAMERA, and the frame time it was buying
// is not there to buy. An earlier revision of this comment claimed interval 1 left 187.9 ms on the
// table against 104.5 ms at interval 4. Re-measured on the same scene (Release, ElectricDreams,
// 1600x900, --no-vsync, --frames 200): intervals 1, 2, 4 and 8 give medians of 18.54, 18.47, 18.50 and
// 18.46 ms -- a 0.09 ms spread across the whole range, which is noise on a STILL camera. Whatever made
// revoxelisation the bottleneck when that pair of numbers was taken is no longer true, and the figure
// outlived it; it is quoted here as refuted rather than quietly deleted.
//
// WHAT A STILL CAMERA CANNOT SEE, measured separately: under a wobbling camera the "Voxi GI update"
// span itself (not the whole frame) went 36.00 ms at interval 1 to 16.57 ms at interval 4
// (aver-gi-update-dominates-under-motion.md; Sponza, --cam-wobble 15 50, ray-driven + AverSR Balanced)
// -- EVIDENCE that amortising this pass costs real time under motion, contradicting the still-camera
// table above. That run also used tile 4 on the unrelated RT shadow amortisation, and
// beginShadowHistory sits inside the same measured span (VoxiRenderer.cpp), so crediting the whole gap
// to giUpdateInterval alone is UNCONFIRMED rather than settled. Lag at interval 4 itself is UNMEASURED
// -- the note this evidence comes from says so directly ("both untested").
//
// 1 -> 2 (the second move, MEDIUM'S CURRENT RUNG): UNMEASURED FOR BOTH COST AND LAG AT THIS SPECIFIC
// RUNG. Nothing above was taken AT interval 2 under motion; this ladder does not borrow the interval-4
// number as if it applied here. What is known rather than measured: at interval 2 the volume rebuilds
// every other frame, so indirect light can lag a moving scene by at most one frame before the next
// rebuild catches it up, and a still scene converges to exactly the same image interval 1 produces
// either way (Renderer::setSettings' own derivation comment, Voxi.cpp). This does not reverse the 4 ->
// 1 conclusion above: 4 still visibly trails and the frame time it bought was still noise. It reopens
// a narrower question on the other side of "always fresh" -- whether Medium, the tier most projects
// actually run, should pay interval 1's full revoxelisation cost every frame for a lag this file has
// no evidence is visible at 2.
//
// LOW MOVED 8 -> 4. Interval 8 (kMaxGiUpdateInterval, the widest the clamp allows) was picked by the
// same still-camera reasoning the 4 -> 1 move above rejects; 4 leans on the EVIDENCE two paragraphs up.
// A further step to 8 is UNMEASURED in both directions -- no cost or lag figure exists for interval 8
// under motion, only the still-camera table, which this whole ladder has already shown cannot rank
// these rungs against each other.
//
// HIGH AND EPIC STAY AT 1. Epic means "do not compromise", so its indirect light remains bit-identical
// to the always-fresh behaviour every tier used to have. High is one rung down from that and this
// ladder has no evidence its cost is worth trading against -- unlike Low and Medium, nothing above
// documents a motion case where High's users would accept the lag.
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

// ================================================================= Ray Tracing ====================

// WHICH REFRACTION MODE THE TIER ASKS FOR. RayTraced only at the top two rungs, because it spends a
// ray per translucent pixel on the pass that is already the frame's bottleneck; ScreenSpace is nearly
// free (it reuses the backdrop copy the absorption path already takes) and is therefore the sensible
// middle. Off at Quality::Off keeps the "tier off means feature off" contract every other knob here
// honours -- with no ray tracing there is no thickness to bend by anyway. See Settings::refractionMode
// (Voxi.hpp) for what each mode actually does, and RenderSettingsResolver.hpp for what happens to a
// RayTraced REQUEST on a device or tier that cannot honour it (it does not fall back to this ladder's
// Off rung -- it resolves to ScreenSpace, one rung down from what was asked for).
//
// MEDIUM IS THE DEFAULT TIER, so its rung must equal Settings::refractionMode's own default or the
// change-gated derivation in Renderer::setSettings can never fire on a default device.
//
// EVIDENCE the rungs differ in output: 31.00 / 31.51 / 12.40% of pixels changed between the three
// modes (aver-refraction.md). Cost of RayTraced (mode 2) specifically is UNMEASURED.
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

// OCCLUSION RAYS PER PIXEL for the ray-traced sun shadow, when this pixel traces this frame. Clamped
// to [1, VoxiRenderer::kMaxShadowRays] by Renderer::setSettings. See Settings::rtShadowRays (Voxi.hpp)
// for the field and why the struct default must equal Medium's rung below.
//
// LOW AND MEDIUM ARE BOTH 1. EVIDENCE that each extra ray under the raster path costs about 1.55 ms
// (ElectricDreams 1600x900, 18.44 / 19.99 / 23.39 ms at 1 / 2 / 4 rays -- see Settings::
// rtPixelsPerRayTile, Voxi.hpp, for the full table). Low now runs that same raster path (D3, see
// ladder::rtRenderMode below) and keeps the cheapest ray count for the same reason it always did.
//
// HIGH IS 4 (was 2) AND EPIC IS 8 (was 4) -- both retuned, both moved up a step.
//   COST: EVIDENCE, but for a different path than Low/Medium's raster one: inside the ray-driven
//   primary an extra SUN ray costs about 0.017 ms (Settings::giSkyOcclusionRays, Voxi.hpp; Sponza, one
//   pass) -- roughly two orders of magnitude cheaper than a raster-path ray, because the sun's rays
//   are coherent and share BVH traversal. The cost of replaying that under the blended (translucent)
//   pass a second time is UNMEASURED.
//   QUALITY: EVIDENCE, but only for the OLD configuration -- raster, unfiltered, a still camera, one
//   pixel: penumbra error of +27 / +27 / +9 / 0 codes at 1 / 2 / 4 / 8 rays (aver-denoiser-findings.md,
//   written before ray-driven primary visibility became the default and before rtShadowDenoise ever
//   ran a nonzero radius -- "no spatial filter anywhere" at the time). Under TODAY's configuration --
//   ray-driven primary visibility plus this tier's own spatial filter, under camera motion -- the same
//   question is UNMEASURED. That earlier note's "do not change the default tier without the owner"
//   concerns Medium, which this retune leaves at 1; High and Epic are not the default tier, and this
//   retune is the owner's own direction (D1).
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

// EDGE LENGTH of the square tile a single traced pixel is amortised over via the ray-traced shadow's
// temporal history: 1 = every pixel traces every frame (bit-identical to no denoiser at all); N>1 =
// one pixel in each NxN tile traces per frame, rotating which one, while every other pixel reuses a
// reprojected history sample. See Settings::rtPixelsPerRayTile (Voxi.hpp) for the full contract and
// the measured table this ladder is built from.
//
// 1 AT EVERY RUNG, Low included: every pixel traces every frame, which is bit-identical to no denoiser
// at all -- no tiling, no reprojected history, no temporal blend. Amortisation measured 0.14 ms moving
// / 0.09 ms static against tile 1, which is inside the noise, while the motion trail it buys is
// already visible on 0.80% of pixels at tile 2 alone (tile 2 vs tile 4 adds only another 0.04%, so the
// artifact is already fully present by tile 2 -- there is no partial-credit rung to fall back to). It
// traded a fault anyone moving the camera can see for milliseconds nobody could measure.
//
// LOW WAS 4, "one traced pixel per 4x4, the widest amortisation that pays." That reasoning measured
// only frame-time cost, on a still camera, and concluded the widest tile the clamp allows was free
// money. It was free money on a parked camera -- but a still-camera benchmark cannot see what the tile
// actually spends: shadows visibly trailing the camera during Play-in-Editor, on the one tier that
// shipped it. This is the same trap that caught giUpdateInterval's Medium rung above (a still-camera
// benchmark reads amortisation as free because the thing it costs, motion, is exactly what it doesn't
// measure), and it is not a coincidence that it caught this knob the same way -- both are
// temporal-history amortisations, and both hide their cost from a benchmark that never pans.
//
// THIS FUNCTION SAID 4 FOR LOW WHILE Voxi.hpp's field comment said 1, for one stretch of this
// project's history -- a torn pair of exactly the kind this codebase's gate baselines warn about: the
// conclusion was written down in one place and the code was left behind in the other. The gate suite
// caught it, not a person rereading both files. A project that wants the amortisation at any tier can
// still ask: RENDER.RTPIXELSPERRAY and --rt-pixels-per-ray are both honoured, and both outrank the
// manifest properly. Opting in is a decision; a preset doing it silently is not.
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

// SPATIAL DENOISE RADIUS, in pixels, for the ray-traced sun shadow. 0 is off: the shadow term is
// whatever this pixel's own rays returned, unfiltered. N > 0 averages a (2N+1)^2 neighbourhood of the
// shadow history, weighted by depth agreement with this pixel's surface plane -- see
// Settings::rtShadowDenoise (Voxi.hpp) for the full mechanism, why it exists, and what it is not (the
// TEMPORAL rtPixelsPerRayTile knob above).
//
// COST IS NOT THE DISCRIMINATOR, and saying so once saves repeating it at every rung: the filter is a
// Gaussian gather whose tap count is (2*radius+1)^2, EVIDENCE measured at +0.02 ms for the full 49-tap
// radius-3 kernel against +1.69 ms for one more traced ray (VoxiRenderer.cpp) -- about eighty-five
// times cheaper than the ray it stands in for, at the WIDEST rung the clamp allows. What varies by
// tier is how much raw noise there is to hide, and how much of the EVIDENCE drift -- 12 to 32 codes
// under a six-degree wobble, growing with radius (Settings::rtShadowDenoise, Voxi.hpp) -- is worth
// risking to hide it.
//
// LOW AND MEDIUM ARE BOTH 2. Both trace exactly one ray (ladder::rtShadowRays above), so their raw
// shadow term is a hard dither (Settings::rtShadowDenoise's "THE PROBLEM IT IS FOR"). UNMEASURED FOR
// BOTH COST AND VISUAL NEED AT LOW SPECIFICALLY: this filter runs per FRAGMENT inside the shading
// shader, so its tap count multiplies by overdraw, and neither that cost nor the visual case for
// keeping it at Low's now-rasterised primary-visibility path (D3) has been measured. It is kept at 2
// on the reasoning Settings::rtShadowDenoise itself states: the shadow term is evaluated in the pixel
// shader, and MSAA has only ever softened it "at every silhouette" -- an interior penumbra at one ray
// stays dithered under MSAA regardless of sample count. Whether PSMainVoxi shades per pixel or per
// sample under MSAA is itself UNCONFIRMED, which is exactly why this stays a kept assumption rather
// than a measured one.
//
// HIGH IS NOW 1 (was 2), JOINING EPIC. High traces four rays now (ladder::rtShadowRays above), real
// variance reduction over Medium's one, and this retune's quality case for the move is UNMEASURED --
// no probe has compared radius 1 against radius 2 at four ray-driven rays under motion. The reasoning
// it leans on instead: drift scales with RADIUS, not with how many rays fed the centre sample, so a
// tier tracing more rays has less raw noise to hide and can afford to risk less drift doing it. The
// same reasoning keeps Epic, at eight rays, from going all the way to 0: eight rays still carry real
// per-pixel variance, and a light touch (radius 1) is a real, nearly-free improvement this ladder is
// not willing to refuse just because the noisier tiers below it need a wider kernel.
//
// A CLAIM THIS COMMENT USED TO MAKE IS WITHDRAWN: an earlier revision said "nothing in this file's
// measurements converges below sixteen [rays]" to justify Epic's own light touch. That is contradicted
// at one pixel by rtShadowRays' own EVIDENCE row for eight rays reaching the sixteen-ray reference
// value directly (Settings::rtShadowRays, Voxi.hpp) -- one converged pixel is not a converged image,
// but the sentence claimed more certainty than this file has ever measured, and it is dropped rather
// than repeated.
//
// NONE OF THE FOUR RUNGS REACH THE CLAMP'S MAXIMUM OF 3. Radius 3 is where "within one code of the
// sixteen-ray answer" was measured (VoxiShaders.hpp, rtShadowSpatial) at the OLD ray counts -- but it
// is also where the wobble-drift measurement tops out, at the worst end of the 12-to-32-code range.
// Buying the last increment of stillness-accuracy at the largest recorded motion cost is not a trade
// this ladder makes silently, by default, on every tier; 3 stays reachable through an explicit
// override for whoever wants it, the same way an explicit voxelResolution request already overrides
// its own tier's derived rung.
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

// WHICH THING FINDS THE FIRST SURFACE: 0 = the rasteriser, 1 = a primary ray per pixel. See
// Settings::rtRenderMode (Voxi.hpp) for the full mechanism and what defaulting to the ray-driven path
// trades away.
//
// 1 FROM MEDIUM UP; 0 AT OFF AND AT LOW, for two different reasons. Off has no acceleration structure
// to trace against, so 1 would claim a mode the renderer has no path to run. Low answers 0 by
// EXPLICIT PRODUCT DECISION (D3, this retune) rather than a hardware gap -- the user calls the
// ray-driven path "the Wavefront Primary rays model" and wants it live from Medium up, with Low kept
// on the rasteriser deliberately.
//
// D3'S EVIDENCE IS PARTIAL, NOT A CLAIM THAT RASTER IS FASTER. At overview cameras raster measures
// slower than ray-driven: ElectricDreams 20.55 vs 8.05 ms; PTTest with Path Tracing pinned off, 13.51
// vs 8.04 ms. Part of that gap is this struct's own 4x MSAA default, which the raster path pays and
// the single-sample ray pass does not; an old, pre-texture-split measurement at a different resolution
// put raster's own MSAA cost at 4.2 ms (16.0 vs 11.8 ms, 4x vs 1x), but today's share of the gap is
// UNMEASURED. A close-up case that once favoured raster (9.02 vs 24.04 ms) is UNCONFIRMED: the run
// that produced it also had Path Tracing on, which silently took the frame over instead of ray-driven,
// so the comparison was never raster-vs-ray-driven at all. On the record this ladder actually has, Low
// can be slower than Medium at an overview camera -- D3 is the user's decision made with that in view,
// not a rung chosen because it measured faster.
constexpr u32 rtRenderMode(Quality q) {
    switch (q) {
        // No ray-tracing hardware path is guaranteed here -- there is nothing to traverse, and
        // claiming 1 would be a mode the renderer has no way to actually run.
        case Quality::Off:    return 0;
        // Deliberately excluded from the ray-driven default (D3) -- see the comment above.
        case Quality::Low:    return 0;
        case Quality::Medium: return 1;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        // An unknown tier must not silently swap which thing finds the first surface out from under a
        // caller who did not ask for that; fall back to the path every version of this engine before
        // this setting ran.
        default:              return 0;
    }
}

// SKY-VISIBILITY RAYS the AMBIENT term traces per pixel. 0 means "estimate it from the cone gather",
// which is what this renderer has always done and what Low and Medium still do. See
// Settings::giSkyOcclusionRays (Voxi.hpp) for why the rays exist and the Sponza measurements behind
// the estimator's own error.
//
// HIGH AND EPIC, NOT EPIC ALONE -- an earlier revision of this comment said "Epic only", which was
// already stale the day accumulation against a reprojected history (rtSkyOcclusionTemporal, rtAoHist_)
// let the ray count drop from four to one: at one accumulated ray the cost is low enough that High can
// afford the term that used to be reserved for Epic. The zero below High is still the whole point
// rather than caution: these rays are an ADDITION to a frame that already traces shadow rays and
// cone-marches a volume, so every rung that cannot afford another ray per pixel keeps the free cone
// estimate instead of paying for one.
//
// ONE RAY, NOT FOUR -- EVIDENCE, and worth restating precisely because the obvious analogy is exactly
// the wrong one: rtShadowRays' sun rays are coherent (they all point at the sun and walk the same BVH
// nodes), so an extra one there is nearly free, while this ray is cosine-distributed over the
// hemisphere and genuinely expensive -- one measures 5.37 ms on Sponza, more than four coherent sun
// rays cost together. The sample count comes from FRAMES instead: ~10 effective samples at weight 0.9
// through the reprojected history, more than four raw rays ever gave, with independent per-pixel noise
// that averages away instead of correlated noise that stacks into a visible block.
//
// WHY THIS MATTERS BEYOND NOISE: every tier that returns 0 here falls back to the voxel cone gather's
// own occlusion, and MEASURED on Sponza that fallback is far too open -- turning the rays off
// brightens the darkest 81% of the frame by 2.6x (mean luminance 20.71 -> 31.92). That is why shadows
// only ever looked properly dark once this term existed at all.
//
// LOW AND MEDIUM STAY ON THE CONE GATHER. The ray is cosine-distributed over the hemisphere, so
// neighbouring lanes walk unrelated parts of the BVH and it is genuinely expensive -- accumulation
// fixes its VARIANCE, not its traversal cost. A budget tier should not pay it. THE FIRST SWEEP that
// established the 0/1/1 shape (Sponza, 112 entities, RENDER.RAYTRACING 4, MSAA 2, --no-vsync,
// --gpu-timing, one camera): 0 rays 10.92 ms (probe 20,20,22), 1 ray 12.25 ms (probe 11,11,13), 4 rays
// 15.58 ms (same probe, 11,11,13) -- one ray already buys the correction; three more bought 3.33 ms
// and no visible change, against a converged path-traced reference of 7,7,7.
constexpr u32 giSkyOcclusionRays(Quality q) {
    switch (q) {
        // OFF MUST BE 0, and not merely "inert": with no acceleration structure there is nothing to
        // trace against, and the shader falls back to the cone gather's occlusion on this value.
        case Quality::Off:    return 0;
        case Quality::Low:    return 0;
        case Quality::Medium: return 0;
        case Quality::High:   return 1;
        case Quality::Epic:   return 1;
        default:              return 0;   // an unknown tier must not silently cost more
    }
}

// EDGE, IN PIXELS, of the square that shares one sky-occlusion ray direction. Kept at 1 -- inert --
// everywhere that traces no such ray, and everywhere that does, now that one accumulated ray has
// replaced the four incoherent ones this tile once made affordable. See Settings::giSkyOcclusionTile
// (Voxi.hpp) for the full mechanism and the measured coherence gain a tile used to buy.
//
// 1 EVERYWHERE NOW, EPIC INCLUDED. The tile existed to make four incoherent hemisphere rays affordable
// by pointing neighbouring lanes the same way, and its price was correlated noise -- exactly what a
// 4x4 block of identical ambient occlusion looks like on screen. With one accumulated ray there is
// nothing left to make coherent. KEPT AS A DIAL rather than deleted: the measurements behind it still
// stand (+1.98 ms for four rays at tile 4 against one at tile 1) and a future term tracing many
// incoherent rays could want it. 1 is the identity, floor(p/1) == p.
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

// THE PATH-TRACING BOUNCE BUDGET. Off is 1 -- one hit, direct lighting, which is ray tracing and not
// path tracing at all -- and every rung above it buys bounces. See Settings::ptBounces (Voxi.hpp) for
// why this is keyed on pathTracing rather than the ray-tracing tier.
//
// EVIDENCE: 4.7 / 5.9 / 6.3 ms at 1 / 2 / 4 bounces, outdoors (ElectricDreams, 2750x1639). Three
// bounces is UNMEASURED -- interpolated between the 2- and 4-bounce figures, not timed directly. The
// ladder stops at 4 because bounces beyond the second cost 0.4 ms and changed nothing measurable
// outdoors -- most paths escape to sky and terminate. A closed interior would price them differently,
// and whether this engine has one to test against at all is itself UNCONFIRMED.
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

} // namespace aver::voxi::ladder

namespace aver::voxi {

// TIER-DERIVATION HAZARD, ENFORCED AT COMPILE TIME. Renderer::setSettings only derives a knob from its
// tier ON A CHANGE (see that function's own comments, Voxi.cpp) -- so if a struct default and its
// default tier's ladder rung ever disagree, a freshly constructed Settings{} silently ships the wrong
// number for its own advertised tier, and nothing will ever correct it, because the tier never
// "changes" away from its own default. This project has already shipped that exact bug more than once
// (see ladder::giUpdateInterval's own history above, and ladder::rtPixelsPerRayTile's "torn pair") --
// both times caught by a person rereading two files side by side. These asserts make the person
// unnecessary for THIS specific mistake: a future rung move that forgets to move its matching struct
// default now fails the build instead of waiting for the next bug hunt.
//
// C++20 is set (modules/render.voxi/CMakeLists.txt) and Settings is an aggregate of literal members
// (enums, u32, f32, bool -- nothing with a non-trivial constructor), so `Settings{}.x` is a constant
// expression; every ladder:: function above is constexpr for the same reason. Voxi.cpp includes this
// header, so every build of Aver.Render.Voxi runs these, not merely whatever happens to include this
// file directly.
static_assert(Settings{}.globalIllumination == Quality::Medium, "GI's default tier moved");
static_assert(Settings{}.rayTracing         == Quality::Medium, "RT's default tier moved");
static_assert(Settings{}.pathTracing        == Quality::Off,    "PT's default tier moved");

static_assert(Settings{}.voxelResolution  == ladder::voxelResolution(Quality::Medium),
              "voxelResolution's struct default no longer matches GI Medium's ladder rung");
static_assert(Settings{}.giCones          == ladder::giCones(Quality::Medium),
              "giCones' struct default no longer matches GI Medium's ladder rung");
static_assert(Settings{}.giUpdateInterval == ladder::giUpdateInterval(Quality::Medium),
              "giUpdateInterval's struct default no longer matches GI Medium's ladder rung");

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
