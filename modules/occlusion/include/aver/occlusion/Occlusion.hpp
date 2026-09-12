// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
//
// Aver.Occlusion — a hierarchical-Z occlusion culler, and NOTHING else. It takes a depth buffer and
// a batch of world-space AABBs and answers, per box, "did every draw so far already cover this box's
// screen footprint with something closer?" It has no idea what a box belongs to (an entity, a chunk,
// a cluster — this module's own signature never spells any of those words), which is what makes it
// usable from more than one caller: SandboxApp.cpp's per-entity scene walk is the first, and a future
// landscape-tile or cluster culler is free to be the second without this module changing at all.
//
// WHY ITS OWN MODULE. The design review that asked for this was explicit that occlusion must NOT
// live in three obvious-looking places:
//   - the RHI (modules/rhi), which must never learn about features — an HZB pyramid is a rendering
//     TECHNIQUE, not a hardware primitive, and the RHI's job stops at giving a technique the pieces
//     it needs (a texture it can create, a compute pipeline it can dispatch, a barrier it can record).
//   - modules/render.voxi, which would wire occlusion to global illumination for no reason beyond
//     "that's where the other GPU passes are" — occlusion has nothing to do with light transport, and
//     a project that ships without Voxi (AVER_MODULE_VOXI=OFF, which this engine treats as a build
//     that must still assemble) must still be able to cull.
//   - modules/trifactor, which would couple occlusion to virtualized geometry specifically, when the
//     first thing this module culls is neither virtualized nor geometry-shaped from its point of view
//     — it is 6,370 plain CMeshRenderer entities, and the SECOND thing it culls (a landscape tile, a
//     cluster, whatever comes next) must not need this module rewritten to accept it.
//
// THE TWO PIECES THE HEADER-LEVEL COMMENT ABOVE PROMISES:
//   - "a pass that builds the pyramid": buildPyramid() below.
//   - "something that takes bounds and a view and answers can this be seen": testBatch() below. The
//     view is consumed AT buildPyramid() time (the pyramid IS a function of one particular view); by
//     the time testBatch() runs, "the view" is simply "whichever pyramid buildPyramid() built last".
//
// ---------------------------------------------------------------------------------------------------
// HOW THE PYRAMID IS BUILT (see OcclusionCuller.cpp for the HLSL these three passes actually run):
//
// Mip 0 is SEEDED from the live scene depth buffer, not copied from it. Two things make this more
// than a reformat:
//   (a) MSAA. The scene depth buffer defaults to 4 samples on both backends (D3D12Device's
//       kDefaultSampleCount), while the only precedent for a depth-as-SRV trick in this tree (Voxi's
//       own shadow map, VoxiRenderer.cpp createShadowResources) is forced single-sampled and always
//       has been — nothing in this engine has ever read a MULTISAMPLED depth buffer from a shader
//       before this. The seed pass declares a REAL Texture2DMS in HLSL (CSSeed in OcclusionCuller.cpp)
//       and reduces its N samples with max(), the same conservative direction the rest of the pyramid
//       uses (see below) — collapsing to ANY single sample, or to their average, could report a pixel
//       as closer than its farthest-covered sample and manufacture a false cull at a foliage edge,
//       which is exactly the content this scene is made of.
//   (b) the resource has to become TYPELESS to be read at all. D3D12Device's depth buffer used to be
//       created directly as DXGI_FORMAT_D32_FLOAT, which can only ever be VIEWED as D32_FLOAT — no
//       SRV of any format is legal on it. It is now created R32_TYPELESS (D3D12Device.cpp's
//       kDepthResourceFormat), with an explicit DSV desc pinning the depth VIEW back to D32_FLOAT —
///      the identical "one resource, two views" trick Voxi's shadow map already uses, just extended
//       to a multisampled resource for the first time. See D3D12Device::createDepthBuffer's own
//       comment for the mechanics.
//
// Every mip AFTER 0 is the CONSERVATIVE reduction of its four parents: max(), meaning FURTHEST from
// the camera under this engine's depth convention (near = 0, far = 1 — see D3D12Device's depth-clear
// value of 1.0f). This is not an arbitrary choice of "how to downsample": a coarse cell's stored value
// has to be at least as far as the CLOSEST occluder anywhere under its footprint, or a box that is
// hidden everywhere but one corner of that cell would read as hidden everywhere. max() of the four
// children's own (already-conservative) values is exactly that bound, applied recursively — see
// OcclusionMath.hpp's conservativelyHidden for where the bound is finally spent, and
// tests/occlusion/src/OcclusionMathTest.cpp for a synthetic pyramid where getting this backwards
// (min() instead of max()) is caught by a box popping invisible that a screenshot diff would call a
// bug, not a tradeoff.
//
// ---------------------------------------------------------------------------------------------------
// TWO-PASS, AND WHY BOTH PASSES SHARE ONE FRAME (INTENT — see the correction below for what this
// RHI actually delivers today). Single-pass occlusion — cull THIS frame against a pyramid built from
// LAST frame's depth — produces FALSE CULLS the instant the camera moves fast enough that something
// enters view between the two frames: nothing in last frame's depth buffer could possibly have drawn
// it, so the pyramid reports it hidden regardless of where it actually is now. That failure mode is
// an object popping OUT of existence, which is worse than the milliseconds two-pass spends to avoid
// it. The caller (SandboxApp.cpp) is the one that actually runs both passes, because it is the one
// holding the entity list and the "was this entity drawn last frame" bit this module deliberately
// does not store (see the header comment above on what this module does not know) — the SHAPE is:
//   1. draw the entities that were visible last frame, with THIS frame's camera and transforms —
//      this is ordinary rendering, no different from a build with occlusion off, and it is what
//      seeds the depth buffer buildPyramid() reads.
//   2. buildPyramid() — this file's own pass, from the depth that step 1 just wrote.
//   3. testBatch() over EVERY entity (not only the ones step 1 skipped) — see its own comment for why
//      re-testing the ones already drawn is not wasted work, it is how an entity that has become
//      occluded since last frame gets DEMOTED for next frame rather than staying in the "always draw"
//      set forever.
//   4. draw whichever of step 3's PREVIOUSLY-HIDDEN entities it says might now be visible.
//
// THE CORRECTION: steps 1-4 above describe what the CALLER records, in that order, into one frame's
// command list. It does NOT describe when the GPU actually RUNS them relative to when testBatch()'s
// CPU readback happens — and those are not the same moment. testBatch() (OcclusionCuller.cpp) USED TO
// end with IResourceFactory::waitIdle() to get an answer at all (see "THE ONE COST" below for why that
// is CORRECTED too, and OcclusionCullerImpl's own top comment in OcclusionCuller.cpp for the measured
// reason it no longer does) — but even with the wait gone, the STRUCTURAL lag this paragraph describes
// has not changed, because that RHI limitation has not: it has no way to close, submit and wait for
// the CURRENT frame's own still-open, still-recording command list, because no
// such "flush and resume this same list" primitive exists anywhere in modules/rhi. So step 2's
// buildPyramid() dispatches and step 3's testBatch() dispatch+copy, all recorded into THIS frame's
// list, have not reached the GPU by the time testBatch() reads its answer back — what comes back is
// whichever dispatch+copy the PREVIOUS frame's testBatch() call recorded, which by then has had a
// full frame to retire. In other words: this design's two passes still SHARE one recorded frame, but
// the answer a caller reads out of them is (at best) exactly one frame older than that. That is
// precisely the single-pass failure mode this section opened by describing — camera motion within
// that one frame of lag can still make something newly-visible read as hidden — just deferred by one
// frame rather than eliminated. SandboxApp.cpp's own comment above its box-collection loop is where
// the actual mitigation now lives: a motion-based margin on each tested box, backed by a threshold
// that disables culling outright for a frame whose camera motion (or whose readback itself, per
// OcclusionCuller.cpp's generation-stamp check below) is too large to trust the one-frame-old answer
// for. Verified by reading D3D12Device::waitForGpu() and VulkanDevice::waitForGpu() — both are a bare
// queue Signal+wait with no ExecuteCommandLists/QueueSubmit of their own, so this is not a D3D12-only
// gap; a Vulkan build has exactly the same staleness, and inherits the same SandboxApp.cpp mitigation
// automatically, since none of it is backend-specific.
//
// ---------------------------------------------------------------------------------------------------
// THE ONE COST THIS DESIGN USED TO NOT HIDE, AND NO LONGER PAYS (CORRECTED A SECOND TIME — this
// section used to claim a same-frame answer; then that it cost a real, unconditional GPU/CPU stall to
// get even a stale one; neither is true any more, and the reason is worth keeping because the first
// "fix" attempted here made things WORSE, not better). testBatch() still cannot hand the caller a
// CPU-visible answer for THIS frame's own pyramid before step 4 needs to decide what to draw — this
// engine's RHI (modules/rhi) exposes exactly one GPU/CPU synchronisation primitive a feature module can
// reach for, IResourceFactory::waitIdle(), a full stop that only drains ALREADY-SUBMITTED work, and
// still has no mid-frame "flush and resume" primitive that would make a same-frame answer possible. THE
// STALENESS IS THEREFORE UNCHANGED AND STRUCTURAL, not something this correction removes. What
// changed is HOW testBatch() gets a stale-but-safe answer without a stall: it used to call waitIdle()
// on every single call specifically to protect its own small, single-buffered boxes/visibility/
// generation-stamp resources from a CPU write racing a GPU read of the SAME memory (see
// OcclusionCullerImpl's own top comment, OcclusionCuller.cpp, for the measurement that found this
// stall — not merely slow, but a real, DETERMINISTIC perturbation of the unrelated ray-driven GI/
// denoiser pipeline, present whenever occlusion was enabled and gone when it was not). Removing the
// wait outright, with no other change, made a DIFFERENT thing worse: those single-buffered resources
// then raced for real (a later call's CPU write landing before an earlier call's GPU read had
// retired), measured as a ~50x increase in run-to-run image instability with occlusion on. The actual
// fix rotates several independent copies of each such resource (kInFlight, OcclusionCuller.cpp) so a
// later call's write always lands in a copy the GPU finished with calls ago — the same double/triple-
// buffering idiom this codebase already uses for every OTHER per-frame CPU-written GPU resource
// (D3D12Device's frameCBs_[kFrameCount] and friends) — which removes the NEED for a wait rather than
// removing the wait's own effects by force. The one-call-stale design this file describes above is
// exactly preserved: a caller still gets, at best, the answer from one call ago, gated by the same
// readbackLagIsExactlyOneCall() safety net this header already documents below.
#pragma once
#include "aver/occlusion/OcclusionMath.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <vector>

namespace aver::occlusion {

// One box, as the caller already has it: world-space min/max corners, row-major cm, +Z up (this
// engine's convention throughout — see MeshVertex's own note in RHI.hpp).
struct Aabb {
    f32 min[3];
    f32 max[3];
};

// The narrow interface this module promises: build the pyramid, then answer visibility for a batch
// of boxes against it. Knows nothing about what a box belongs to — see this header's top comment.
class IOcclusionCuller {
public:
    virtual ~IOcclusionCuller() = default;

    // (Re)sizes the pyramid for the scene's current render target, a no-op when nothing changed.
    // Call this every frame before buildPyramid(); it is cheap when sceneWidth/sceneHeight/
    // sampleCount already match what the pyramid was built for. Returns false when the pyramid could
    // not be (re)built — a compute-pipeline compile failure, most likely — in which case buildPyramid
    // and testBatch both become no-ops (buildPyramid logs once; testBatch marks every box "visible",
    // the always-safe answer) until a later call succeeds.
    virtual bool ensureSized(rhi::IResourceFactory& res, u32 sceneWidth, u32 sceneHeight,
                             u32 sampleCount) = 0;

    // Builds the pyramid from `sceneDepth` (IDevice::sceneDepthTexture()) exactly as it stands right
    // now — see this header's top comment for why that has to be AFTER the "visible last frame" set
    // has been drawn and BEFORE anything else is drawn this frame. Transitions `sceneDepth` to a
    // shader-readable state and back to DepthWrite before returning, so the caller's own depth writes
    // can resume immediately afterwards. Opens its own ScopedGpuStat("HZB build") span.
    virtual void buildPyramid(rhi::IRenderContext& ctx, rhi::TextureHandle sceneDepth,
                              const f32 viewProj[16]) = 0;

    // DEBUG / A-B SWITCH. When `on` is true, testBatch() re-adds the unconditional res.waitIdle()
    // this file's own top comment ("THE ONE COST...") describes removing -- i.e. it restores the
    // EXACT pre-fix behaviour (a full GPU/CPU drain at the end of every testBatch() call) at runtime,
    // with no rebuild, so a live session can A/B "wait restored" against "wait removed" directly
    // against each other (SandboxApp.cpp's --occlusion-waitidle / --no-occlusion-waitidle CLI flags
    // and the occlusion.debugForceWaitIdle console var reach this without editing code).
    //
    // THIS BASE CLASS's default is a false no-op (safe for a future second implementer with nothing
    // to restore, who is not obligated to add a real one) -- but OcclusionCullerImpl, the only
    // implementation in this tree, OVERRIDES the default to TRUE (OcclusionCuller.cpp, see the
    // FOLLOW-UP comment above its kInFlight member). A follow-up investigation into a "lighting looks
    // flat" report found the no-wait path measurably worse against a path-traced ground truth and, in
    // a same-shader controlled A/B, found the wait's removal alone reproduces the flatness -- without
    // ever pinning down which GI/lighting resource actually depends on it. Per this module's own
    // rule (do not ship a guess; default to whichever image is correct even if slower), the shipping
    // default now pays the stall until that dependency is found and given a narrower fix of its own.
    // Safe to call every frame; NOT PURE.
    virtual void setDebugForceWaitIdle(bool on) { (void)on; }

    // Tests every box in `boxes` against the pyramid buildPyramid() most recently built, and fills
    // outVisible with one byte per box (non-zero = "the pyramid could not prove this is hidden; draw
    // it" — see OcclusionMath.hpp's conservativelyHidden for what that promise actually rests on).
    // DOES NOT BLOCK BY ITS OWN DESIGN (CORRECTED — it used to, unconditionally, via a wait this
    // header's top comment now explains was removed) — but OcclusionCullerImpl currently blocks
    // again ANYWAY, by default, via the debugForceWaitIdle_ switch just above (see its own comment
    // for why): read this paragraph as describing the design's floor, not this build's shipping
    // behaviour. With that switch off, the bytes it hands back are read WITHOUT waiting for the GPU,
    // on the premise the answer from one call ago has ordinarily had a full call's worth of GPU time
    // to retire by now — and "available" was never "current" even when this did block: the bytes it hands
    // back are, at best, from the PREVIOUS call to this function, not this one — see the corrected
    // "TWO-PASS" section above and readbackLagIsExactlyOneCall() below for how a caller checks whether
    // even that one-call lag held, INCLUDING the case (now more frequent than when this function
    // blocked, precisely because it no longer forces the GPU to catch up) where the GPU had not
    // actually finished and the bytes are stale by more than one call — that case was always possible
    // and is caught the same way either way, only its frequency changed. Opens its own
    // ScopedGpuStat("HZB test") span, SEPARATE from "HZB build"
    // — the two cost different things and a caller measuring "does this pay for itself" needs to see
    // them apart.
    //
    // `identityKey` is the caller's OWN fingerprint of which logical boxes these are, in which
    // order — e.g. a hash of the entity ids `boxes` came from (OcclusionMath.hpp's
    // hashIdentityKey()), NOT a hash of the box bytes themselves. See that function's own comment
    // for why: this module cannot tell "the population changed" apart from "the caller re-dilated
    // every box's bounds this frame" by looking at coordinates alone, and SandboxApp.cpp's own
    // motion-safety margin (see its "MOTION-SAFE TRUST GATE" comment) makes the latter happen on
    // nearly every frame of camera motion. Comparing THIS against the identityKey the immediately
    // preceding call was given is how readbackLagIsExactlyOneCall()'s IDENTITY half is decided.
    virtual void testBatch(rhi::IRenderContext& ctx, rhi::IResourceFactory& res,
                           const Aabb* boxes, u32 count, u64 identityKey, std::vector<u8>& outVisible) = 0;

    // STALENESS DETECTOR, ACROSS TWO INDEPENDENT DIMENSIONS. testBatch() answers a question one
    // call late (see above); this answers "was that lag actually safe to apply" — which takes BOTH
    // of the following, not just the first:
    //   (a) TIMING: was it EXACTLY one call late, the lag every caller's own safety margin assumes,
    //       or worse? Trivially true for the very FIRST testBatch() call ever made (there is no
    //       earlier call for it to have been stale relative to — calling that an anomaly would be a
    //       false-positive warning on every run that ever turns culling on); after that, true only
    //       when the bytes testBatch() most recently handed back are provably the ones the
    //       IMMEDIATELY PRECEDING testBatch() call produced (see OcclusionCuller.cpp's
    //       generation-stamp comment on testBatch() for the mechanism — a second, tiny CPU-authored
    //       buffer round-tripped through the SAME command list as the visibility copy, costing one
    //       8-byte writeBuffer/copyBuffer/readBuffer per call).
    //   (b) IDENTITY: even when (a) holds, were the PREVIOUS call's boxes the SAME boxes — same
    //       count, same order, same content — as THIS call's? This module matches box[i] one call to
    //       box[i] the next by bare array index (see the header comment above), and every caller
    //       today rebuilds that array fresh each frame from whichever entities currently pass its
    //       own gates — so a population that streamed an entity in or out between the two calls this
    //       lag spans breaks the correspondence just as surely as a GPU-timing miss would, with no
    //       timing anomaly to show for it. OcclusionCuller.cpp's testBatch() checks this against the
    //       caller-supplied `identityKey` parameter (OcclusionMath.hpp's hashIdentityKey()) rather
    //       than by hashing the box bytes it uploads — CORRECTED: an earlier revision hashed the
    //       packed bytes instead, which found a real gap (a run against a live streaming scene
    //       disagreed exactly twice, both during initial load, and never again once the population
    //       settled) but opened a bigger one: SandboxApp.cpp's own motion-safety margin (see its
    //       "MOTION-SAFE TRUST GATE" comment) changes those same bytes on nearly every frame the
    //       camera moves, for a population that has not changed at all, and hashing them could not
    //       tell the two apart — MEASURED at 244 of 255 tested frames under a gentle 5-degree/30-frame
    //       --cam-wobble, i.e. this check reporting "stale" almost continuously while the camera was
    //       simply moving. Hashing the caller's own entity-id sequence instead answers the identity
    //       question directly and is insensitive to what the caller does with each box's bounds.
    // From the second call on, OcclusionCullerImpl's own answer defaults to false (distrust) and
    // only earns "true" once BOTH checks actually pass -- the same conservative bias testBatch()'s
    // own outVisible all-1 default already uses.
    //
    // NOT PURE, and this base-class default is DELIBERATELY THE OPPOSITE BIAS from the concrete
    // implementation above: "true" (trust unconditionally) preserves this interface's pre-existing
    // behaviour for a future second implementer that never calls testBatch() with a non-empty batch,
    // or has no cheap way to stamp one, and is therefore not obligated to add this at all — such an
    // implementer's callers should not be silently downgraded to "never cull" by a base-class default
    // they never asked for. A caller wanting the conservative bias for real gets it from
    // OcclusionCullerImpl, the only implementation that exists today (see createOcclusionCuller
    // below).
    // NAMED FOR WHAT IT MEASURES, NOT FOR WHAT A CALLER HOPES. True does NOT mean the answer is
    // current -- it means the answer is stale by EXACTLY ONE testBatch() call, which is the lag this
    // design has and cannot remove (see the TWO-PASS section above). It was briefly called
    // lastReadbackWasFresh(), which returned true for data it had just proved was a frame old; that
    // is the same species of confidently-wrong name as the "SAME frame" comment whose falseness is
    // the whole reason this method exists, so it did not survive review.
    //
    // False means EITHER the lag was something other than one call (older, newer, unreadable) OR
    // the lag was exactly one call but the box population changed under it -- either way, the case
    // no mitigation is calibrated for, and every caller must answer by not culling.
    virtual bool readbackLagIsExactlyOneCall() const { return true; }

    // Boxes testBatch() said "hidden" for, and the total it was asked about, across the MOST RECENT
    // testBatch() call only (not accumulated) — a caller wanting a running total (SandboxApp.cpp's
    // periodic log line) accumulates these itself, once per frame, the same way every other counter
    // in that file already does.
    virtual void lastTestCounts(u32& culled, u32& tested) const = 0;

    // Calls where readbackLagIsExactlyOneCall() returned false specifically because of the IDENTITY
    // check (b) above, not the timing check (a) -- i.e. the GPU generation stamp matched but the box
    // population itself changed shape or content under the lag. Cumulative since this culler was
    // created, never reset, the same shape as a caller's own running totals (SandboxApp.cpp's
    // occlusionStaleReadbacks_) rather than a per-call value, since a caller wanting to separate "the
    // GPU timing raced" from "my own box array changed under me" in its own periodic report needs a
    // number to diff against, not just a instantaneous bool. NOT PURE, default 0, same reasoning as
    // readbackLagIsExactlyOneCall()'s own default: a future second implementer with no box-identity
    // concept to report is not obligated to add one.
    virtual u64 boxIdentityChurnCount() const { return 0; }
};

// Owns nothing the caller does not already own (the resource factory, the depth texture) except its
// OWN pyramid texture, compute pipelines and the small readback buffer testBatch() uses. Destroy with
// destroyOcclusionCuller before the IResourceFactory that created its resources goes away.
IOcclusionCuller* createOcclusionCuller(rhi::IResourceFactory& res);
void destroyOcclusionCuller(rhi::IResourceFactory& res, IOcclusionCuller* culler);

} // namespace aver::occlusion
