// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
//
// The GPU half of Aver.Occlusion — three compute passes (seed, reduce, test) driven entirely through
// the generic RHI (modules/rhi), so this file has no idea whether it is running on D3D12 or Vulkan.
// See Occlusion.hpp for the design (why this module exists, why two-pass, why the pyramid reduction
// is max()) and OcclusionMath.hpp for the CPU reference the CSTest kernel below is a hand-translation
// of — READ THAT FILE'S COMMENTS FIRST; this one assumes them.
//
// ---- REGISTERS: why t0/u0 here can never collide with anything else in this engine ----
// Every HLSL block below declares its own registers starting at t0/u0/b1, and none of the three ever
// shares a root signature with any OTHER pipeline in this codebase — each is compiled from its own
// ComputePipelineDesc with its OWN, freshly-built PipelineLayout (see createStaticPipelines and
// createPyramidResources below). D3D12 (and the Vulkan descriptor-set analogue) scope register
// numbers to the root signature that DECLARES them, not to the device globally: Voxi's own scene
// pipeline runs table 1 (materials) from t4 upward (VoxiRenderer's own comment: "table 1 bases at
// srvCount, currently 4"), and that fact is simply irrelevant here, the same way it would be
// irrelevant to a completely different process. t4 in Voxi's root signature and t0 in this module's
// seed-pass root signature are two different pieces of hardware state that happen to share a decimal
// digit. The only way THIS module could collide with itself is reusing a register within ONE shader's
// own declarations, and each kernel below declares each register exactly once.
#include "aver/occlusion/Occlusion.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include "aver/rhi/ShaderFiles.hpp"   // these three kernels are deployed files

namespace aver::occlusion {

using namespace aver::rhi;

namespace {

// ---------------------------------------------------------------------------------------------
// Mip 0: reduces the LIVE, possibly-multisampled scene depth buffer into a single-sampled R32Float
// texel per pixel, taking the FURTHEST (max) of however many samples cover that pixel -- the same
// conservative direction every later mip uses (see CSReduce below and Occlusion.hpp's top comment),
// applied here to MSAA sub-pixel coverage instead of to four neighbouring texels. Compiled once per
// LIVE sample count (AVER_HZB_SAMPLES, a compile-time constant substituted by createPyramidResources
// -- recompiled whenever the scene's own sample count changes, same trigger as the MS/non-MS switch
// below) rather than querying it at runtime via GetDimensions: a fixed, compile-time sample count on
// Texture2DMS's own template argument and a loop bound BOTH known at compile time is the ordinary,
// well-trodden shape for this kind of resolve, and there was no reason to reach for the dynamic form.
// AVER_HZB_MS itself still selects Texture2DMS vs Texture2D at compile time, because a single-sample
// resource cannot legally be viewed as Texture2DMS and vice versa -- see Occlusion.hpp's top comment,
// point (a), for why a "just always use Texture2DMS" shortcut does not work.

// Every mip after 0: max() of up to four parent texels (fewer at an odd edge, where the same
// nearest-in-range texel is sampled twice rather than reading off the end of the source mip -- see
// the min() clamps below). ONE pipeline, reused for every (mip-1 -> mip) step in the chain: the
// binding set changes which two mips are bound, the shader itself is resolution-agnostic.

// One thread per candidate box: a hand-translation of OcclusionMath.hpp's projectAabbScreenBounds +
// selectConservativeMip + conservativelyHidden, kept as three clearly-separated blocks below in the
// SAME ORDER as that header so the two can be read side by side. tests/occlusion/src/
// OcclusionMathTest.cpp checks the CPU copy; this copy is checked indirectly by the screenshot-diff
// requirement this module's design doc calls for (a wrong answer here is a pixel that should not have
// changed when occlusion is toggled on).

ShaderHandle compileCS(IResourceFactory& res, const char* source, const char* entry, const char* defines) {
    ShaderDesc d;
    d.source = source;
    d.entry = entry;
    d.stage = ShaderStage::Compute;
    d.minShaderModel = 60;
    d.defines = defines;
    return res.createShader(d);
}

} // namespace

// ---------------------------------------------------------------------------------------------

class OcclusionCullerImpl final : public IOcclusionCuller {
public:
    // MEASURED, on the PTTest/NewSponza repro under --cam-translate (continuous translation, not
    // --cam-wobble's rotation -- see the commit this lands with): with the per-frame res.waitIdle()
    // that used to sit at the bottom of testBatch(), turning occlusion culling on versus off at the
    // SAME frame count produced a large, deterministic, non-noise difference in the FINAL RENDERED
    // IMAGE -- 45.96% of pixels differing by >40/765 at frame 50, a broad darkening of every GI-lit
    // surface, reproduced independently twice. CORRECTED -- this paragraph used to claim "content
    // reaching VoxiRenderer is provably identical either way (an occlusion-hidden entity still
    // submits via submitShadowOnly with the same mesh/transform/material as the visible path)".
    // That premise is false: submitShadowOnly (SandboxApp.cpp, removed by F1-F4) resolved a culled
    // entity's material as slot 0 regardless of which submesh actually owned it, while the visible
    // path split a multi-part mesh per part -- for 110 of the 142 distinct meshes PTTest actually
    // places, those two routes handed VoxiRenderer a DIFFERENT material, and sometimes a different
    // draw count, not the same content this paragraph asserted. That gap does not retract the
    // measurement below (the waitIdle A/B was run with occlusion's ROUTE difference held constant,
    // toggling only the wait), but it does mean the "nothing about WHAT gets drawn explains it"
    // conclusion was resting on a false premise, not a verified one -- see occlusion-fix-plan.md
    // section 1.4 ("occlusion-waitidle: no refuter voted on it... its supporting measurements rest
    // on OcclusionCuller.cpp:84-86, which is false for 110 of 142 meshes"). Temporarily removing the waitIdle() (reverted
    // before landing, see the commit) collapsed that same comparison to 0.011% -- the ordinary
    // run-to-run noise floor this tree documents elsewhere (aver-render-nondeterminism.md) -- which
    // is strong, direct evidence that the STALL ITSELF, not merely "occlusion is on", is what was
    // perturbing the GI/denoiser pipeline into a different (but internally consistent) result every
    // time it ran. Full-queue waitIdle() does not merely wait; forcing the CPU to fully catch up with
    // the GPU mid-frame is a real, engine-wide perturbation this module has no business causing just
    // to protect its OWN small, reused buffers.
    //
    // A bare removal is NOT safe, though, and was shown not to be: boxesBuf_/visBuf_/visReadback_/
    // genUpload_/genReadback_ were each a SINGLE instance, written by the CPU (an immediate memcpy
    // for the Upload-heap ones) and consumed by a GPU dispatch+copy that will not actually execute
    // until sometime after this function returns (see testBatch()'s own "CORRECTED" comment on why).
    // Without the wait, a later call's CPU write can land before an earlier call's GPU read of the
    // SAME buffer has retired -- a genuine torn read/write, not merely a stale one -- which is
    // consistent with a real, separately measured cost: a bare removal (no re-buffering) made TWO
    // back-to-back reruns of the IDENTICAL configuration disagree with EACH OTHER by ~50x more than
    // with the wait present, a new self-race, not a revealed old one.
    //
    // The fix kept here is the standard one for exactly this shape of hazard, already used
    // everywhere else in this codebase a per-frame GPU resource is CPU-written and GPU-consumed
    // (D3D12Device's frameCBs_[kFrameCount], postCBs_[kFrameCount], ring_[kFrameCount], etc., all
    // sized by the swapchain's own buffer count, currently 2): rotate through several independent
    // copies of each such buffer instead of reusing one, so a later call's write lands in a copy the
    // GPU finished with calls ago, never the one it might still be reading. kInFlight is 3, not the
    // engine's own kFrameCount==2: CORRECTED -- this used to justify that with "this module's
    // dispatch runs from SandboxApp's onUpdate(), which this codebase's own comments document as
    // running BEFORE that frame's device_->beginFrame()". That is backwards: Engine::frameStep()
    // (modules/runtime/src/Engine.cpp:280-283) runs onUpdate(), THEN beginFrame(), THEN onRender()
    // -- and this module's actual dispatch site, SandboxApp.cpp's occlusion box-collection walk and
    // its buildPyramid()/testBatch() calls, lives in onRender(), not onUpdate() (onUpdate only
    // reasserts this call's own debugForceWaitIdle_ switch every frame, per its own comment beside
    // that setter, specifically so a live `set occlusion.debugForceWaitIdle` is visible before THIS
    // SAME frame's testBatch() runs "once onUpdate returns" -- which already said the dispatch was
    // AFTER onUpdate, this comment just never caught up to it). So occlusion's call cadence is not
    // "one step ahead of beginFrame" as originally claimed; it runs inside the same beginFrame/
    // endFrame span the swapchain's own back-buffer index is scoped to. Nothing about kInFlight's
    // VALUE depended on the wrong half of this claim -- one extra slot of headroom over the engine's
    // own 2 (a few KB of GPU memory: boxCapacity_ entities * 32/4/4 bytes, times one extra copy)
    // remains cheap insurance against needing to prove the exact phase relationship either way --
    // only the REASON given for choosing 3 over 2 was wrong, and is corrected here rather than left
    // to mislead the next reader into "fixing" a phase-shift that was never real.
    //
    // WHAT DOES NOT CHANGE: the "exactly one call stale" design (Occlusion.hpp's TWO-PASS section) --
    // testBatch() still reads the IMMEDIATELY PRECEDING call's answer, from a DIFFERENT slot than the
    // one it is about to write, so the one-call lag every caller's motion-dilation margin is
    // calibrated for is untouched; only which physical buffer holds "the previous call's answer" now
    // rotates. The generation-stamp/identity staleness detectors (readbackLagIsExactlyOneCall(),
    // boxIdentityChurnCount()) are UNCHANGED and keep doing their job: a readback that is not exactly
    // one call old is still detected and rejected by the caller, the same safety net that already
    // existed -- this fix removes the disruptive wait, it does not remove the check that made
    // removing the wait safe to attempt at all.
    //
    // FOLLOW-UP (why debugForceWaitIdle_ defaults TRUE below, not false): a later investigation into a
    // "lighting is now flat" report re-measured this exact A/B on the same repro and found the wait's
    // removal is not the clean win the paragraphs above conclude. Two independent, controlled results:
    // (1) against a converged, static-camera PATH-TRACED reference (ground truth, no occlusion
    // involved at all), THIS file's post-fix default (no wait) rendered 3.9x too bright (mean luminance
    // 53.30 vs 13.65) -- the fix's own "converged to OFF" framing never checked OFF against a ground
    // truth, only against itself; (2) with voxi_restir.hlsli byte-for-byte UNCHANGED, toggling ONLY
    // this flag on the CURRENT (rotated-buffer) build reproduces the same washed-out/plateau-vs-
    // structured difference the flat-lighting report describes, which means whatever the removed wait
    // was incidentally doing for the GI/lighting path is NOT fully explained by the buffer race fixed
    // above -- whatever it is was never found (a targeted search of every CPU<->GPU crossing in the GI/
    // denoiser path came back empty; the leading remaining theory is a frame-CADENCE dependency, e.g.
    // giUpdateInterval's re-bake gate seeing a different effective tick rate, not a memory race). Per
    // this investigation's own rule -- do not ship a guess, default to whichever image is correct even
    // if slower -- debugForceWaitIdle_ below defaults to TRUE (pays the stall, matches the better-
    // corroborated image) until that mechanism is actually found and given its own narrow fix. This
    // does NOT revive the ORIGINAL bug: the race this class's top comment measured was a hazard in the
    // single-buffered boxes/visibility/generation resources, and kInFlight rotation (immediately below)
    // removes that hazard unconditionally, whether or not the wait also runs. The two fixes are
    // independent; only the wait's default changed.
    static constexpr u32 kInFlight = 3;

    // A-B SWITCH -- see Occlusion.hpp's own comment on setDebugForceWaitIdle for what it is for and who
    // reaches it (SandboxApp.cpp's --occlusion-waitidle / --no-occlusion-waitidle, and the matching
    // console var occlusion.debugForceWaitIdle). DEFAULTS TRUE (see the FOLLOW-UP paragraph above this
    // class's kInFlight member): true re-adds the exact res.waitIdle() call the top-of-class comment
    // describes removing, at the exact point it used to sit (end of testBatch(), after the
    // dispatch+copies are recorded) -- this is now the SHIPPING default, a known, deliberate,
    // engine-wide per-frame stall paid ONLY while occlusion culling is enabled, kept until the actual
    // GI/lighting dependency on frame cadence is found and given a narrower fix. Setting this false
    // (--no-occlusion-waitidle, or `set occlusion.debugForceWaitIdle false` live) opts back into the
    // faster, rotated-buffers-only path for A/B comparison or once that narrower fix lands.
    void setDebugForceWaitIdle(bool on) override { debugForceWaitIdle_ = on; }

    bool ensureSized(IResourceFactory& res, u32 sceneW, u32 sceneH, u32 sampleCount) override {
        if (!staticOk_) staticOk_ = createStaticPipelines(res);
        if (!staticOk_) return false;
        if (pyramid_ && sceneW == sceneW_ && sceneH == sceneH_ && sampleCount == sampleCount_)
            return true;
        sceneW_ = sceneW; sceneH_ = sceneH; sampleCount_ = sampleCount;
        if (sceneW_ == 0 || sceneH_ == 0) return false;
        return createPyramidResources(res);
    }

    void buildPyramid(IRenderContext& ctx, TextureHandle sceneDepth, const f32 viewProj[16]) override {
        std::memcpy(viewProjCache_, viewProj, sizeof(viewProjCache_));
        if (!staticOk_ || !pyramid_ || !sceneDepth) return;
        ScopedGpuStat stat(ctx, "HZB build");

        // The depth SRV changes identity across a resize even though (per D3D12Device::
        // sceneDepthTexture's own contract) the TextureHandle itself does not -- re-writing this
        // descriptor every call is one CreateShaderResourceView, not worth a dirty flag to skip.
        res_->setSrv(seedSet_, 0, sceneDepth, kAllMips);

        ctx.textureBarrier(sceneDepth, ResourceState::DepthWrite, ResourceState::NonPixelShaderResource);

        ctx.textureBarrier(pyramid_, ResourceState::NonPixelShaderResource, ResourceState::UnorderedAccess, 0);
        ctx.setPipeline(seedPso_);
        ctx.setBindingSet(seedSet_);
        const u32 seedCb[4] = {sceneW_, sceneH_, 0, 0};
        ctx.setConstants(1, seedCb, 4);
        ctx.dispatch((sceneW_ + 7) / 8, (sceneH_ + 7) / 8, 1);
        ctx.uavBarrierTexture(pyramid_);
        ctx.textureBarrier(pyramid_, ResourceState::UnorderedAccess, ResourceState::NonPixelShaderResource, 0);

        ctx.setPipeline(reducePso_);
        u32 srcW = pyramidW_, srcH = pyramidH_;
        for (u32 m = 1; m < mipCount_; ++m) {
            const u32 dstW = std::max(1u, pyramidW_ >> m), dstH = std::max(1u, pyramidH_ >> m);
            ctx.textureBarrier(pyramid_, ResourceState::NonPixelShaderResource, ResourceState::UnorderedAccess, m);
            ctx.setBindingSet(reduceSets_[m - 1]);
            const u32 reduceCb[4] = {srcW, srcH, dstW, dstH};
            ctx.setConstants(1, reduceCb, 4);
            ctx.dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);
            ctx.uavBarrierTexture(pyramid_);
            ctx.textureBarrier(pyramid_, ResourceState::UnorderedAccess, ResourceState::NonPixelShaderResource, m);
            srcW = dstW; srcH = dstH;
        }

        ctx.textureBarrier(sceneDepth, ResourceState::NonPixelShaderResource, ResourceState::DepthWrite);
    }

    void testBatch(IRenderContext& ctx, IResourceFactory& res, const Aabb* boxes, u32 count,
                   u64 identityKey, const f32 viewportRect[4], std::vector<u8>& outVisible) override {
        lastTested_ = count;
        lastCulled_ = 0;
        // The always-safe default if anything below bails out early -- and, for readbackLagExpected_,
        // the always-safe DIRECTION: "do not trust it" until the generation check near the bottom of
        // this function actually earns "true". A caller combining this with its own trust gate (see
        // readbackLagIsExactlyOneCall()'s comment in Occlusion.hpp) then falls back to zero culling on
        // exactly the calls this function could not vouch for, the same conservative bias
        // outVisible's all-1 default already uses.
        outVisible.assign(count, 1);
        readbackLagExpected_ = false;
        if (!staticOk_ || !pyramid_ || count == 0) return;
        if (!ensureBoxCapacity(res, count)) return;

        // WHICH OF THE kInFlight COPIES THIS CALL OWNS -- see this class's own top comment for why
        // there are several instead of one. `write` is the copy THIS call's box upload/dispatch/copy
        // targets; `read` is the copy the IMMEDIATELY PRECEDING call targeted, which is what this
        // call reads back (the "exactly one call stale" design is unchanged, only which physical
        // buffer holds that previous answer now rotates). The two are always different slots for any
        // kInFlight > 1, so the read below and the write further down never touch the same memory --
        // there is no ordering hazard to reason about BETWEEN them, only between one call's write and
        // a call kInFlight ago's write to that SAME slot, which is what the rotation exists to space
        // out. thisGeneration - 1 cannot underflow: thisGeneration is a post-increment of a counter
        // starting at 0, so it is always >= 1 here.
        const u64 thisGeneration = ++submitGeneration_;
        const u32 writeSlot = static_cast<u32>(thisGeneration % kInFlight);
        const u32 readSlot  = static_cast<u32>((thisGeneration - 1) % kInFlight);

        packed_.resize(static_cast<usize>(count) * 8);
        for (u32 i = 0; i < count; ++i) {
            f32* p = &packed_[static_cast<usize>(i) * 8];
            p[0] = boxes[i].min[0]; p[1] = boxes[i].min[1]; p[2] = boxes[i].min[2]; p[3] = 0.0f;
            p[4] = boxes[i].max[0]; p[5] = boxes[i].max[1]; p[6] = boxes[i].max[2]; p[7] = 0.0f;
        }
        res.writeBuffer(boxesBuf_[writeSlot], packed_.data(), static_cast<u64>(packed_.size()) * sizeof(f32), 0);

        // SECOND STALENESS DIMENSION -- IDENTITY, NOT JUST TIMING. See hashIdentityKey()'s own
        // comment (OcclusionMath.hpp) for the full gap this closes, why it takes the CALLER's own
        // identity fingerprint rather than hashing the (dilated, motion-dependent) box bytes this
        // function uploads, and what was measured about the difference.
        const bool boxesStableAcrossLag = havePrevIdentityKey_ && identityKey == prevIdentityKey_;
        prevIdentityKey_ = identityKey;
        havePrevIdentityKey_ = true;

        // STALENESS DETECTOR: stamp a monotonically-increasing generation number into a tiny
        // CPU-authored side buffer and round-trip it through the SAME command list, at the SAME
        // point, as the visibility copy below -- so both copies land in the GPU in the same
        // ExecuteCommandLists/QueueSubmit batch and retire together. Costs one extra 8-byte
        // writeBuffer + copyBuffer + readBuffer per call; no shader change, since it never touches
        // the compute pipelines.
        //
        // WHY THIS EXISTS: every caller of this module (today, only SandboxApp.cpp) that tries to
        // bound the one-call staleness testBatch() has (see the corrected "TWO-PASS" section in
        // Occlusion.hpp) is reasoning from an assumption -- "the bytes I just read back are from
        // EXACTLY the immediately preceding call, not two or more calls back." Comparing the
        // generation number actually read back against the one this exact call submitted -1 turns
        // that possibility into something a caller can check via readbackLagIsExactlyOneCall() and
        // react to loudly, instead of a silent wrong answer no screenshot diff would necessarily
        // catch. Written into genUpload_[writeSlot] now (not "at the end", as an earlier revision of
        // this file needed -- see below) because writeSlot is a copy no earlier call is still using.
        //
        // THE RACE THIS USED TO HAVE, AND WHY IT DOES NOT ANY MORE. With a single genUpload_ buffer
        // (this file's previous shape) writing the stamp here raced its own readback so reliably that
        // the detector reported "stale" on essentially every frame -- writeBuffer on an upload heap is
        // an immediate CPU memcpy, but the copyBuffer that reads it is only RECORDED here and does not
        // execute until this frame's command list is submitted, after this function returns; call N's
        // write could land before call N-1's copy (reading the SAME single buffer) had actually
        // retired, and that copy then captured N instead of the N-1 it was recorded to capture. The
        // fix at the time was to write the stamp only after an unconditional res.waitIdle() had
        // provably drained call N-1's copy -- correct, but it meant this module could not answer a
        // visibility query without a full engine-wide GPU/CPU sync every single call, which MEASURING
        // this repro (see this class's own top comment) showed was not merely slow: it was
        // deterministically perturbing the ray-driven GI/denoiser pipeline into a different rendered
        // image depending on whether occlusion happened to be enabled, unrelated to anything
        // occlusion actually culled. Rotating genUpload_/genReadback_ the same way as the box/
        // visibility buffers removes the NEED for that wait: writeSlot was last used kInFlight calls
        // ago, comfortably retired by now (see the top comment for the margin), so writing it here,
        // before this call's own copy is even recorded, is safe with no wait at all.
        res.writeBuffer(genUpload_[writeSlot], &thisGeneration, sizeof(thisGeneration), 0);

        {
            ScopedGpuStat stat(ctx, "HZB test");
            if (!visBufLive_[writeSlot]) {
                ctx.bufferBarrier(visBuf_[writeSlot], ResourceState::Common, ResourceState::UnorderedAccess);
                visBufLive_[writeSlot] = true;
            }
            ctx.setPipeline(testPso_);
            ctx.setBindingSet(testSet_[writeSlot]);
            // F7: `viewport` mirrors occlusion_test.hlsl's gViewport, NOT `vp` (gViewProj) --
            // deliberately a distinct name from the matrix field above, so this struct cannot grow
            // a second "vp"-named member with two different meanings. A NULL or degenerate
            // viewportRect (w or h <= 0, OcclusionMath.hpp's ViewportRect "whole target"
            // convention) leaves `viewport` at its `cb{}` zero-init, which the shader reads as
            // gViewport.z <= 0.5 -- "run no rect arithmetic at all", the exact pre-F7 behaviour.
            struct TestCB { f32 vp[16]; u32 count, w, h, mips; f32 viewport[4]; } cb{};
            std::memcpy(cb.vp, viewProjCache_, sizeof(cb.vp));
            cb.count = count; cb.w = pyramidW_; cb.h = pyramidH_; cb.mips = mipCount_;
            if (viewportRect && viewportRect[2] > 0.0f && viewportRect[3] > 0.0f)
                std::memcpy(cb.viewport, viewportRect, sizeof(cb.viewport));
            // 24 dwords: 16 (gViewProj) + 4 (count/w/h/mips) + 4 (gViewport) -- see
            // occlusion_test.hlsl's TestCB cbuffer comment and createStaticPipelines' matching
            // constantDwords[1] below; all three must agree or this cbuffer under- or over-reads.
            ctx.setConstants(1, &cb, 24);
            ctx.dispatch((count + 63) / 64, 1, 1);
            ctx.uavBarrierBuffer(visBuf_[writeSlot]);
            ctx.bufferBarrier(visBuf_[writeSlot], ResourceState::UnorderedAccess, ResourceState::CopySource);
            ctx.copyBuffer(visReadback_[writeSlot], visBuf_[writeSlot], static_cast<u64>(count) * 4);
            ctx.bufferBarrier(visBuf_[writeSlot], ResourceState::CopySource, ResourceState::UnorderedAccess);
            // Both genUpload_ (Upload) and genReadback_ (Readback) sit permanently in states that
            // already allow a copy (GENERIC_READ / COPY_DEST -- see their creation below), the exact
            // same reasoning boxesBuf_ and visReadback_ already rely on above, so no extra barrier is
            // needed here either.
            ctx.copyBuffer(genReadback_[writeSlot], genUpload_[writeSlot], sizeof(thisGeneration));
        }

        // res.waitIdle() HERE IS NOW THE DEFAULT (debugForceWaitIdle_ defaults true -- see the
        // FOLLOW-UP paragraph on this class's kInFlight member for why). It is NOT here to protect
        // boxesBuf_/visBuf_/visReadback_/genUpload_/genReadback_ any more -- kInFlight rotation already
        // does that unconditionally, wait or no wait -- it is here because a later investigation found
        // the GI/lighting path still looks measurably different (against a path-traced ground truth,
        // and in a same-shader, wait-only A/B) with the wait gone, and could not pin down why. This is
        // the "last resort, scoped as tightly as this module can manage" case: a real, engine-wide
        // GPU/CPU drain, paid on every testBatch() call while occlusion culling is enabled (never when
        // it is off), at whatever framerate cost that measures as on the caller's scene -- accepted
        // because a right image outweighs a fast wrong one until the actual dependency is found and
        // given its own narrow fix (a fence on just that resource, not this module's queue drain).
        // Setting debugForceWaitIdle_ false (--no-occlusion-waitidle / the console var) removes the
        // stall and returns to reading whatever the call that owned `readSlot` (the IMMEDIATELY
        // PRECEDING call, thisGeneration - 1) already left there -- possibly not yet landed, if the GPU
        // is behind, which is exactly what the generation check further down exists to catch; this
        // function has never promised a same-frame answer (see the corrected "TWO-PASS" section,
        // Occlusion.hpp) and still does not, wait or no wait.
        //
        // Positioned exactly where the pre-fix build had it: after the dispatch+copies above are
        // RECORDED (not yet executed; see the "CORRECTED" comment on why waitIdle() here still cannot
        // drain THIS call's own still-open command list) and before the readback below. Draining
        // whatever WAS already submitted -- ordinarily everything through the end of the previous
        // frame -- does not change which slot is written or read (kInFlight rotation is unconditional
        // either way), only whether the CPU stalls here first.
        if (debugForceWaitIdle_) res.waitIdle();

        // THE FIRST CALL READS NOTHING, BECAUSE THERE IS NOTHING TO READ, and reading it anyway is
        // what produced "19 of 19 tested entities culled (100.0%)" on the opening frame of every
        // single run. visReadback_ is filled only by a previous call's copyBuffer; on call 1 it holds
        // whatever the allocation happened to contain. The loop below then overwrote outVisible's
        // deliberately-safe all-1 default with that uninitialised VRAM and counted every zero byte in
        // it as a cull -- and the generation branch immediately underneath declared the result
        // TRUSTWORTHY, so a caller had no way to reject it. Whole-scene garbage, marked reliable, on
        // the first frame.
        //
        // Skipping the read leaves outVisible all-1 and lastCulled_ at 0, which is not a fallback --
        // it is the correct answer. Before anything has been tested, nothing has been proven
        // occluded, so "everything visible" is exactly true, and it is true in the conservative
        // direction that costs a frame of drawing rather than a frame of missing geometry.
        //
        // ORIGINALLY ">2, NOT >1, BECAUSE THE FIRST DISPATCH IS ALSO WORTHLESS" -- for a different
        // reason than the first readback, which is why two guards were needed even before this fix.
        // This is a two-pass occlusion scheme: buildPyramid() runs from the scene depth texture
        // BEFORE this frame's scene is drawn, so it is always reading the PREVIOUS frame's depth. On
        // the opening frame there is no previous frame, so pyramid mip 0 comes from a depth target
        // nothing has rendered into, every box tests as occluded against it, and call 2 -- which
        // reads call 1's dispatch -- reported a flat "19 of 19 tested entities culled (100.0%)". A
        // caller acting on that culls the ENTIRE SCENE for one frame.
        //
        // MEASURED, which is how that second guard was found at all: with only a >1 guard the
        // opening frame correctly read 0 of 19, and the 19 simply moved to the next report instead
        // of disappearing. Same scene, same frame count, identical in raster and ray-driven mode --
        // so it was never about which renderer wrote the depth, only about there not being one yet.
        //
        // NOW GENERALISED to minReadableGeneration_ (default 3, i.e. exactly the ">2" above) rather
        // than a bare literal, because "the opening frame's depth/pyramid is not real yet" turned out
        // to have a SECOND trigger this repro hit live and the ORIGINAL bare ">2" could not see:
        // createPyramidResources()'s own comment covers the depth/pyramid recreation case (a scene
        // resize mid-run); ensureBoxCapacity()'s resize branch covers a DIFFERENT one this fix's own
        // rotation exposed measurably -- a box-count growth that outgrows the current headroom
        // destroys and recreates every boxesBuf_/visBuf_/visReadback_/testSet_ slot, and the
        // generation stamp (genUpload_/genReadback_, deliberately sized once and never touched by
        // that resize) has no way to know the slot it is vouching for was just thrown away and
        // rebuilt empty -- "was the lag exactly one call" and "is this the same buffer that call
        // actually wrote" are different questions, and only the first one had a check. MEASURED: on
        // this repro, that gap alone produced 224 of 691 (32.4%) freshly-culled entities in one
        // report window against a baseline of 0, and the pyramid-recreation trigger (the SAME
        // resize, in practice -- see createPyramidResources()'s own comment) produced several more
        // calls each reporting close to the FULL box count culled, structurally the exact "19 of 19"
        // bug this module's history already describes, just retriggered mid-run. Both triggers push
        // the SAME floor forward by the SAME margin (createPyramidResources()'s comment has the
        // measured number and why it is wider than the original startup guard's "+3"); this is
        // deliberately the more conservative of the two requirements rather than trying to prove a
        // narrower warmup would have sufficed for the box-only case -- an extra "assume everything
        // visible" frame costs a few unnecessary draws, never a false cull.
        if (thisGeneration >= minReadableGeneration_) {
            rawVisible_.resize(count);
            if (res.readBuffer(visReadback_[readSlot], rawVisible_.data(), static_cast<u64>(count) * 4, 0)) {
                for (u32 i = 0; i < count; ++i) {
                    const bool visible = rawVisible_[i] != 0;
                    outVisible[i] = visible ? 1 : 0;
                    if (!visible) ++lastCulled_;
                }
            }
        }

        if (thisGeneration == 1) {
            // The FIRST call ever has no previous call to have been stale RELATIVE TO, and
            // genReadback_ holds whatever garbage it was created with. Calling that "stale" would be
            // a false-positive warning on every single run that ever turns culling on, for a
            // boundary condition that is not an anomaly -- so it is trivially fresh instead, and the
            // generation check below only starts proving anything from the SECOND call onward.
            //
            // AND THAT IS NOW HONEST, which it was not before. This flag used to vouch for a result
            // read out of an uninitialised visReadback_ -- it said "trust this" about garbage. The
            // guard above means call 1 no longer reads that buffer at all, so what this vouches for
            // is "every box visible, nothing culled", which is genuinely correct on a call that has
            // tested nothing yet. The flag and the data it describes finally agree.
            readbackLagExpected_ = true;
        } else {
            u64 readGeneration = 0;
            if (res.readBuffer(genReadback_[readSlot], &readGeneration, sizeof(readGeneration), 0) &&
                readGeneration == thisGeneration - 1 && boxesStableAcrossLag) {
                readbackLagExpected_ = true;
            } else if (readGeneration == thisGeneration - 1 && !boxesStableAcrossLag) {
                // GPU timing was exactly right (the generation matches), but the IDENTITY check just
                // above says the box array itself changed shape or content since the call whose
                // answer this is -- see that check's own comment. Counted separately from a genuine
                // timing miss so a future reader of occlusionStaleReadbacks_ (SandboxApp.cpp) is not
                // misled into re-auditing genUpload_/genReadback_'s buffer-copy ordering for a defect
                // that lives here instead.
                ++boxSetChurnedAcrossLag_;
            }
            // else: leave it at the always-safe default set at the top of this function (false) --
            // either the readback failed, the generation that actually landed is not the immediately
            // preceding call's, or (see boxSetChurnedAcrossLag_ above) it was but the boxes it
            // answered for were not this call's own -- the one-call-lag assumption every caller's own
            // safety margin depends on did not hold this time, for one reason or the other.
        }
    }

    void lastTestCounts(u32& culled, u32& tested) const override { culled = lastCulled_; tested = lastTested_; }

    bool readbackLagIsExactlyOneCall() const override { return readbackLagExpected_; }

    u64 boxIdentityChurnCount() const override { return boxSetChurnedAcrossLag_; }

    void releaseAll(IResourceFactory& res) {
        destroyPyramidResources(res);
        if (reducePso_) { res.destroyPipeline(reducePso_); reducePso_ = 0; }
        if (testPso_)   { res.destroyPipeline(testPso_);   testPso_ = 0; }
        // kInFlight independent copies, one per in-flight slot -- see this class's own top comment.
        for (u32 i = 0; i < kInFlight; ++i) {
            if (boxesBuf_[i])    { res.destroyBuffer(boxesBuf_[i]);    boxesBuf_[i] = 0; }
            if (visBuf_[i])      { res.destroyBuffer(visBuf_[i]);      visBuf_[i] = 0; }
            if (visReadback_[i]) { res.destroyBuffer(visReadback_[i]); visReadback_[i] = 0; }
            if (genUpload_[i])   { res.destroyBuffer(genUpload_[i]);   genUpload_[i] = 0; }
            if (genReadback_[i]) { res.destroyBuffer(genReadback_[i]); genReadback_[i] = 0; }
            if (testSet_[i])     { res.destroyBindingSet(testSet_[i]); testSet_[i] = 0; }
            visBufLive_[i] = false;
        }
        boxCapacity_ = 0;
        staticOk_ = false;
        res_ = nullptr;
    }

    IResourceFactory* res_ = nullptr;   // stashed so buildPyramid can re-set the depth SRV; NON-owning

private:
    bool createStaticPipelines(IResourceFactory& res) {
        res_ = &res;
        bool ok = true;
        if (const ShaderHandle cs = compileCS(res, rhi::shaderFile("occlusion_reduce.hlsl").c_str(), "CSReduce", nullptr)) {
            ComputePipelineDesc p;
            p.cs = cs;
            p.layout.srvCount = 1;
            p.layout.uavCount = 1;
            p.layout.constantDwords[1] = 4;   // b1: {srcW, srcH, dstW, dstH}
            reducePso_ = res.createComputePipeline(p);
        }
        if (!reducePso_) { AVER_ERROR("[Occlusion] HZB reduce pipeline unavailable"); ok = false; }

        if (const ShaderHandle cs = compileCS(res, rhi::shaderFile("occlusion_test.hlsl").c_str(), "CSTest", nullptr)) {
            ComputePipelineDesc p;
            p.cs = cs;
            p.layout.srvCount = 2;   // t0 boxes, t1 pyramid
            p.layout.uavCount = 1;   // u0 visible
            p.layout.constantDwords[1] = 24;   // b1: TestCB (float4x4 + 4 uints + gViewport float4, F7)
            testPso_ = res.createComputePipeline(p);
        }
        if (!testPso_) { AVER_ERROR("[Occlusion] HZB test pipeline unavailable"); ok = false; }
        return ok;
    }

    bool createPyramidResources(IResourceFactory& res) {
        // FOUND AND MEASURED WHILE BUILDING THIS FIX -- a SECOND, independent way to read a buffer no
        // real dispatch has answered for, distinct from ensureBoxCapacity()'s box-buffer resize (see
        // minReadableGeneration_'s own comment). A scene resize -- a render-scale change, a window
        // resize, the editor viewport settling to its real size a frame or two after launch, all
        // observed live on this exact repro -- recreates pyramid_ at the new resolution here, but
        // does NOT, by itself, mean the SCENE DEPTH TEXTURE buildPyramid() reads from is already
        // full of real content: that texture is very often resized at the SAME moment (same trigger,
        // same frame), so the next call or two are seeding the pyramid from a depth target nothing
        // has rendered into yet -- the IDENTICAL "opening frame" problem testBatch()'s own ">2"
        // history solved once, recurring every time this fires instead of only at startup.
        // MEASURED: on the PTTest repro, a resize landing here (2049x1152, settling from an initial
        // 3532x1987, one frame after the module's own construction) made testBatch() calls up
        // through generation 5 each report ~100% of the ~112 tested entities culled -- structurally
        // the exact "19 of 19 tested entities culled" bug this module's history describes, just
        // retriggered mid-run instead of only once at the top. That is a WIDER window than the naive
        // "one buildPyramid() call to see real depth, one testBatch() call to be read" count would
        // predict (which would put the margin at +3, matching the original startup guard) -- a
        // resize commonly changes the UNDERLYING SCENE DEPTH TEXTURE's own resolution at the very
        // same moment, not merely this module's pyramid, which needs its own frame to fill with
        // real content before buildPyramid() has anything real to seed from; the exact mechanism
        // was not fully isolated, so the margin below is the MEASURED safe point plus one call of
        // headroom rather than a value derived from first principles. Widening it costs nothing but
        // a few extra "assume visible" frames right at a resize; the direction to err in is safety,
        // not precision, exactly as this module's own dilation math already does.
        minReadableGeneration_ = std::max(minReadableGeneration_, submitGeneration_ + 6);
        destroyPyramidResources(res);
        pyramidW_ = sceneW_; pyramidH_ = sceneH_;

        TextureDesc td;
        td.width = pyramidW_; td.height = pyramidH_; td.mips = 0;   // 0 = full chain down to 1x1
        td.format = Format::R32Float;
        td.bind = ResourceBind::ShaderResource | ResourceBind::UnorderedAccess;
        td.initialState = ResourceState::NonPixelShaderResource;
        td.debugName = "HZB pyramid";
        pyramid_ = res.createTexture(td);
        if (!pyramid_) { AVER_ERROR("[Occlusion] HZB pyramid texture ({}x{}) could not be created", pyramidW_, pyramidH_); return false; }

        TextureDesc resolved;
        res.textureInfo(pyramid_, resolved);
        mipCount_ = resolved.mips;

        // ---- seed: depends on sampleCount_, so it is (re)compiled here rather than in
        // createStaticPipelines -- see this file's own top comment, point (a). AVER_HZB_SAMPLES is
        // the live sample count itself, baked in as a compile-time constant (Texture2DMS's own
        // template argument, and the seed kernel's unrolled tap count) -- see rhi::shaderFile("occlusion_seed.hlsl").c_str()'s comment
        // for why a compile-time count rather than a GetDimensions() query at every pixel.
        char def[48];
        std::snprintf(def, sizeof def, "AVER_HZB_MS=%d;AVER_HZB_SAMPLES=%u",
                      sampleCount_ > 1 ? 1 : 0, sampleCount_ > 1 ? sampleCount_ : 1u);
        if (const ShaderHandle cs = compileCS(res, rhi::shaderFile("occlusion_seed.hlsl").c_str(), "CSSeed", def)) {
            ComputePipelineDesc p;
            p.cs = cs;
            p.layout.srvCount = 1;
            p.layout.uavCount = 1;
            p.layout.constantDwords[1] = 4;   // b1: {srcW, srcH, pad, pad}
            seedPso_ = res.createComputePipeline(p);
        }
        if (!seedPso_) { AVER_ERROR("[Occlusion] HZB seed pipeline unavailable ({} sample(s))", sampleCount_); return false; }

        BindingSetDesc sd;
        sd.srvCount = 1; sd.uavCount = 1;
        sd.srvKinds[0] = (sampleCount_ > 1) ? SlotKind::Texture2DMS : SlotKind::Texture2D;
        sd.uavKinds[0] = SlotKind::Texture2D;
        seedSet_ = res.createBindingSet(sd);
        if (!seedSet_) { AVER_ERROR("[Occlusion] HZB seed binding set unavailable"); return false; }
        res.setUav(seedSet_, 0, pyramid_, 0);

        reduceSets_.reserve(mipCount_ > 0 ? mipCount_ - 1 : 0);
        for (u32 m = 1; m < mipCount_; ++m) {
            BindingSetDesc rd;
            rd.srvCount = 1; rd.uavCount = 1;
            rd.srvKinds[0] = SlotKind::Texture2D;
            rd.uavKinds[0] = SlotKind::Texture2D;
            const BindingSetHandle set = res.createBindingSet(rd);
            if (!set) { AVER_ERROR("[Occlusion] HZB reduce binding set for mip {} unavailable", m); return false; }
            res.setSrv(set, 0, pyramid_, m - 1);
            res.setUav(set, 0, pyramid_, m);
            reduceSets_.push_back(set);
        }

        // The pyramid handle just changed identity; the test binding set's SRV at slot 1 (if it
        // exists from a previous size) points at a destroyed texture. Force ensureBoxCapacity to
        // rebuild it on the next testBatch() call rather than trying to patch it in place here.
        boxCapacity_ = 0;
        return true;
    }

    void destroyPyramidResources(IResourceFactory& res) {
        if (seedPso_) { res.destroyPipeline(seedPso_); seedPso_ = 0; }
        if (seedSet_) { res.destroyBindingSet(seedSet_); seedSet_ = 0; }
        for (BindingSetHandle s : reduceSets_) if (s) res.destroyBindingSet(s);
        reduceSets_.clear();
        if (pyramid_) { res.destroyTexture(pyramid_); pyramid_ = 0; }
        mipCount_ = 0;
    }

    // Creates (or resizes) all kInFlight copies of testBatch()'s per-call resources together --
    // see this class's own top comment for why there are several. Every copy is always the SAME
    // capacity; there is no per-slot sizing, since box count is a property of the SCENE this frame,
    // not of which rotation slot happens to be live.
    bool ensureBoxCapacity(IResourceFactory& res, u32 count) {
        // The staleness-detector's stamp buffers: fixed at 8 bytes (one u64) each, independent of
        // boxCapacity_, so they are created ONCE per slot (guarded separately) rather than being torn
        // down and rebuilt every time the box count grows past its own headroom below. Not worth
        // their own dedicated init function: this is the only place testBatch()'s GPU-side resources
        // are lazily created at all, so a second such place would just be a second thing to remember
        // to call.
        //
        // Guarded on genReadback_[kInFlight - 1] (the LAST slot's SECOND buffer of the pair) -- the
        // same "guard on the last thing the sequence creates" idiom the box buffers below already use
        // (guarded on testSet_[kInFlight - 1]), so a call that created some slots' pairs but failed
        // partway through retries the WHOLE set next time instead of silently reusing a half-created
        // batch.
        if (!genReadback_[kInFlight - 1]) {
            for (u32 i = 0; i < kInFlight; ++i) {
                if (genUpload_[i])   { res.destroyBuffer(genUpload_[i]);   genUpload_[i] = 0; }
                if (genReadback_[i]) { res.destroyBuffer(genReadback_[i]); genReadback_[i] = 0; }

                BufferDesc gud;
                gud.bytes = sizeof(u64);
                gud.kind = BufferKind::Upload;
                gud.debugName = "HZB test generation stamp (upload)";
                genUpload_[i] = res.createBuffer(gud);

                BufferDesc grd;
                grd.bytes = sizeof(u64);
                grd.kind = BufferKind::Readback;
                grd.debugName = "HZB test generation stamp (readback)";
                genReadback_[i] = res.createBuffer(grd);

                if (!genUpload_[i] || !genReadback_[i]) {
                    AVER_ERROR("[Occlusion] HZB staleness-detector buffers unavailable -- "
                               "readbackLagIsExactlyOneCall() will report false forever, which is safe "
                               "(every caller falls back to no culling) but gives up the culling "
                               "benefit entirely");
                    return false;
                }
            }
        }
        if (count <= boxCapacity_ && testSet_[kInFlight - 1]) return true;
        // A REAL RESIZE, ABOUT TO THROW AWAY WHATEVER ANY SLOT WAS HOLDING -- see
        // minReadableGeneration_'s own comment (testBatch()) for the gap this closes, and
        // createPyramidResources()'s own comment (this same margin, "+6" not the naively-expected
        // "+3") for what was actually measured and why the wider number is kept here too: on this
        // repro a box-driven resize and the depth/pyramid resize both land within the SAME one- or
        // two-frame settling window, so there is no evidence the two need DIFFERENT margins, and
        // using the smaller, unproven one here would reopen exactly the gap this fix exists to close
        // the moment a level settles in a way that separates them. submitGeneration_ is the count of
        // calls BEFORE this one (this function runs before testBatch() increments it for the call in
        // progress).
        minReadableGeneration_ = std::max(minReadableGeneration_, submitGeneration_ + 6);
        for (u32 i = 0; i < kInFlight; ++i) {
            if (boxesBuf_[i])    { res.destroyBuffer(boxesBuf_[i]);    boxesBuf_[i] = 0; }
            if (visBuf_[i])      { res.destroyBuffer(visBuf_[i]);      visBuf_[i] = 0; }
            if (visReadback_[i]) { res.destroyBuffer(visReadback_[i]); visReadback_[i] = 0; }
            if (testSet_[i])     { res.destroyBindingSet(testSet_[i]); testSet_[i] = 0; }
            visBufLive_[i] = false;
        }

        // Headroom so a count that drifts by a handful of entities frame to frame (an actor spawned
        // or destroyed) does not reallocate every single frame -- 25% or 64, whichever is larger.
        boxCapacity_ = count + std::max<u32>(count / 4, 64);

        for (u32 i = 0; i < kInFlight; ++i) {
            BufferDesc bd;
            bd.bytes = static_cast<u64>(boxCapacity_) * 32;   // AabbGpu: 8 floats
            bd.kind = BufferKind::Upload;
            bd.debugName = "HZB test boxes";
            boxesBuf_[i] = res.createBuffer(bd);

            BufferDesc vd;
            vd.bytes = static_cast<u64>(boxCapacity_) * 4;
            vd.kind = BufferKind::Default;
            vd.allowUnorderedAccess = true;
            vd.debugName = "HZB visible";
            visBuf_[i] = res.createBuffer(vd);

            BufferDesc rd;
            rd.bytes = static_cast<u64>(boxCapacity_) * 4;
            rd.kind = BufferKind::Readback;
            rd.debugName = "HZB visible readback";
            visReadback_[i] = res.createBuffer(rd);

            BindingSetDesc sd;
            sd.srvCount = 2; sd.uavCount = 1;
            sd.srvKinds[0] = SlotKind::StructuredBuffer;
            sd.srvKinds[1] = SlotKind::Texture2D;
            sd.uavKinds[0] = SlotKind::StructuredBuffer;
            testSet_[i] = res.createBindingSet(sd);

            if (!boxesBuf_[i] || !visBuf_[i] || !visReadback_[i] || !testSet_[i]) {
                AVER_ERROR("[Occlusion] HZB test resources for {} boxes unavailable (slot {})", boxCapacity_, i);
                return false;
            }
            res.setSrvBuffer(testSet_[i], 0, boxesBuf_[i], 32, boxCapacity_, 0);
            if (pyramid_) res.setSrv(testSet_[i], 1, pyramid_, kAllMips);
            res.setUavBuffer(testSet_[i], 0, visBuf_[i], 4, boxCapacity_, 0);
        }
        return true;
    }

    u32 sceneW_ = 0, sceneH_ = 0, sampleCount_ = 0;
    u32 pyramidW_ = 0, pyramidH_ = 0, mipCount_ = 0;
    TextureHandle pyramid_ = 0;

    bool staticOk_ = false;
    PipelineHandle reducePso_ = 0, testPso_ = 0, seedPso_ = 0;
    BindingSetHandle seedSet_ = 0;
    std::vector<BindingSetHandle> reduceSets_;

    // kInFlight independent copies of every buffer a call to testBatch() writes and later reads back
    // -- see this class's own top comment for why. Indexed by (generation % kInFlight); NOT
    // double-buffered to match this engine's own swapchain (kFrameCount == 2 on D3D12) -- kInFlight
    // buys headroom over that floor without this module having to prove exactly how its own call
    // cadence lines up with the swapchain's back-buffer index (see the top comment's CORRECTED
    // paragraph on kInFlight for what this module's dispatch site actually is, and is not).
    BufferHandle boxesBuf_[kInFlight] = {}, visBuf_[kInFlight] = {}, visReadback_[kInFlight] = {};
    BindingSetHandle testSet_[kInFlight] = {};
    u32 boxCapacity_ = 0;   // shared: every slot is always sized identically
    bool visBufLive_[kInFlight] = {};

    // THE GENERATION A READ FIRST BECOMES TRUSTWORTHY -- see testBatch()'s own comment on the read
    // guard that uses this. Starts at 3, the ORIGINAL bare ">2" startup warmup (call 1 has nothing
    // written yet; call 2's dispatch ran against a not-yet-real pyramid); createPyramidResources()
    // and ensureBoxCapacity()'s resize branch each push this forward by the SAME margin whenever
    // something they own would otherwise hand back a buffer no real dispatch has answered for yet --
    // a scene resize (recreates pyramid_, and very often the scene depth texture buildPyramid()
    // reads alongside it) or a box-count growth past headroom (recreates every boxesBuf_/visBuf_/
    // visReadback_/testSet_ slot), respectively. Monotonic non-decreasing: std::max at both call
    // sites, so an overlapping pair of resizes takes the LATER (larger) floor, never regresses to
    // an earlier, already-superseded one.
    u64 minReadableGeneration_ = 3;

    // STALENESS DETECTOR -- see testBatch()'s own comment above the writeBuffer(genUpload_, ...)
    // call. 8 bytes each, kInFlight copies of the pair; created once per slot in ensureBoxCapacity,
    // independent of boxCapacity_.
    BufferHandle genUpload_[kInFlight] = {}, genReadback_[kInFlight] = {};
    u64 submitGeneration_ = 0;      // incremented once per testBatch() call that reaches the dispatch
    bool readbackLagExpected_ = false;   // see readbackLagIsExactlyOneCall()
    bool debugForceWaitIdle_ = true;     // see setDebugForceWaitIdle() / this class's own top comment
                                          // -- defaults true: see the FOLLOW-UP paragraph above kInFlight

    // SECOND STALENESS DETECTOR -- box IDENTITY across the one-call lag, not GPU timing. See
    // testBatch()'s own comment beside boxesStableAcrossLag's computation for what this catches that
    // the generation stamp above cannot (a caller whose box array changed shape or content between
    // the call this readback answers and the call consuming it) -- keyed on the CALLER-SUPPLIED
    // identityKey parameter, not a hash of the box bytes themselves (OcclusionMath.hpp's
    // hashIdentityKey() own comment explains why that distinction is the whole fix). CPU-only
    // bookkeeping, no GPU resource.
    u64 prevIdentityKey_ = 0;
    bool havePrevIdentityKey_ = false;
    // Calls where the GPU-timing check passed (the generation stamp matched) but the identity check
    // did not -- a caller's own box population changed under it, not this module racing itself. Kept
    // separate from a caller's own occlusionStaleReadbacks_-style GPU-timing counter so the two
    // failure modes, which point at different files, are not conflated in a report.
    u64 boxSetChurnedAcrossLag_ = 0;

    f32 viewProjCache_[16] = {};
    std::vector<f32> packed_;
    std::vector<u32> rawVisible_;
    u32 lastCulled_ = 0, lastTested_ = 0;
};

IOcclusionCuller* createOcclusionCuller(IResourceFactory& res) {
    auto* impl = new OcclusionCullerImpl();
    impl->res_ = &res;
    return impl;
}

void destroyOcclusionCuller(IResourceFactory& res, IOcclusionCuller* culler) {
    if (!culler) return;
    auto* impl = static_cast<OcclusionCullerImpl*>(culler);
    impl->releaseAll(res);
    delete impl;
}

} // namespace aver::occlusion
