// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-3.0-only. See LICENSE.md at the repository root.
//
// The GPU half of Aver.Occlusion — three compute passes (seed, reduce, test) driven entirely through
// the generic RHI (modules/rhi), so this file has no idea whether it is running on D3D12 or Vulkan.
// See Occlusion.hpp for the design (why two-pass, why the pyramid reduction is max()) and
// OcclusionMath.hpp for the CPU reference CSTest is a hand-translation of — read that file's comments
// first; this one assumes them.
//
// Register numbers (t0/u0/b1) are scoped per root signature/PipelineLayout, not globally: each of the
// three kernels below compiles its own layout (createStaticPipelines / createPyramidResources), so a
// register reused by another pipeline elsewhere (e.g. Voxi's own t4+ -- VoxiRenderer's comment: "table
// 1 bases at srvCount, currently 4") cannot collide with these. The only real hazard would be reusing a
// register within one shader's own declarations, which none do.
#include "aver/occlusion/Occlusion.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include "aver/rhi/ShaderFiles.hpp"   // these three kernels are deployed files

namespace aver::occlusion {

using namespace aver::rhi;

namespace {

// ---------------------------------------------------------------------------------------------
// Mip 0 (CSSeed, occlusion_seed.hlsl): reduces the live, possibly-multisampled depth buffer into a
// single-sampled R32Float texel, taking the max (furthest) over however many samples cover it -- same
// conservative direction every later mip uses. AVER_HZB_SAMPLES/AVER_HZB_MS are compile-time constants
// (recompiled on a sample-count change) rather than a runtime GetDimensions query, because a
// single-sample resource can't legally be viewed as Texture2DMS and vice versa (Occlusion.hpp, point a).

// Every mip after 0: max() of up to four parent texels (fewer at an odd edge, where the same
// nearest-in-range texel is sampled twice rather than reading off the end of the source mip -- see
// the min() clamps below). ONE pipeline, reused for every (mip-1 -> mip) step in the chain: the
// binding set changes which two mips are bound, the shader itself is resolution-agnostic.

// One thread per candidate box: hand-translation of OcclusionMath.hpp's projectAabbScreenBounds +
// selectConservativeMip + conservativelyHidden, kept in the SAME ORDER as that header for side-by-side
// reading. tests/occlusion/src/OcclusionMathTest.cpp checks the CPU copy; this GPU copy is checked
// indirectly via the design doc's screenshot-diff requirement (a wrong answer = a pixel that shouldn't
// change when occlusion toggles).

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
    // kInFlight ROTATION -- MEASURED, on PTTest/NewSponza under --cam-translate: with a single-buffered
    // boxesBuf_/visBuf_/visReadback_/genUpload_/genReadback_ (CPU-written via immediate upload-heap
    // memcpy, GPU-consumed by a dispatch+copy that runs after this function returns), a later call's
    // write could land before an earlier call's GPU read of the SAME buffer retired -- a torn
    // read/write. Removing the per-frame res.waitIdle() that used to guard this (no re-buffering) made
    // two back-to-back reruns of the identical config disagree by ~50x more than with the wait
    // present -- a new self-race. (Original trigger: occlusion on vs off at the same frame count, under
    // the old always-on waitIdle, differed by 45.96% of pixels at frame 50, a broad GI darkening;
    // removing that wait collapsed the same A/B to a 0.011% noise floor -- the stall itself, not
    // occlusion, was perturbing GI/denoiser.) Rotating kInFlight independent copies (the same pattern
    // D3D12Device uses for frameCBs_/postCBs_/ring_, sized to kFrameCount==2) removes the hazard without the wait:
    // a slot is only reused kInFlight calls later, long after the GPU retired it. kInFlight=3, one
    // more than the engine's own kFrameCount==2, because this module's dispatch actually runs inside
    // onRender()'s beginFrame/endFrame span (Engine::frameStep(), Engine.cpp:280-283) -- the extra slot
    // is cheap headroom (a few KB) rather than a proven exact phase relationship.
    //
    // UNCHANGED: the "exactly one call stale" design (Occlusion.hpp's TWO-PASS section) -- testBatch()
    // still reads the immediately preceding call's answer, just from a different, rotating slot. The
    // generation-stamp/identity staleness detectors (readbackLagIsExactlyOneCall(),
    // boxIdentityChurnCount()) still catch a readback that isn't exactly one call old; rotation only
    // removed the buffer race, not the check that made removing the old wait safe to attempt.
    //
    // debugForceWaitIdle_ DEFAULTS TRUE (OPEN): a later investigation found the rotation fix alone is
    // not a clean win for GI/lighting. Measured: (1) against a converged path-traced reference, no-wait
    // rendered 3.9x too bright (mean luminance 53.30 vs 13.65); (2) toggling ONLY this flag (shaders
    // unchanged) reproduces a washed-out/plateau look. Root cause not found -- a targeted search of
    // GI/denoiser CPU<->GPU crossings came back empty; leading theory is a frame-cadence dependency
    // (e.g. giUpdateInterval's re-bake gate), not a memory race. Per this investigation's rule --
    // default to the correct image even if slower -- the wait stays on until the real mechanism is
    // found and fixed narrowly. This does NOT revive the original buffer race: kInFlight rotation
    // removes that hazard unconditionally regardless of this flag; the two fixes are independent.
    static constexpr u32 kInFlight = 3;

    // A/B switch -- see Occlusion.hpp's setDebugForceWaitIdle comment for callers (SandboxApp.cpp's
    // --occlusion-waitidle / --no-occlusion-waitidle, console var occlusion.debugForceWaitIdle).
    // Defaults TRUE (see the kInFlight member's OPEN paragraph above): re-adds the engine-wide
    // res.waitIdle() at the end of testBatch(), paid only while culling is enabled, kept until the
    // GI/cadence issue is fixed narrowly. False opts back into the faster rotated-buffers-only path.
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
        // Safe default if anything below bails early: outVisible all-1 (nothing culled), and
        // readbackLagExpected_ false ("don't trust it") until the generation check below earns true. A
        // caller gating on readbackLagIsExactlyOneCall() (Occlusion.hpp) then falls back to zero
        // culling on exactly the calls this function couldn't vouch for.
        outVisible.assign(count, 1);
        readbackLagExpected_ = false;
        if (!staticOk_ || !pyramid_ || count == 0) return;
        if (!ensureBoxCapacity(res, count)) return;

        // writeSlot is the copy THIS call's box upload/dispatch/copy targets; readSlot is the
        // immediately preceding call's copy, which this call reads back (still exactly one call stale,
        // only the physical buffer rotates). Always different slots for kInFlight > 1, so no hazard
        // between them -- only between this call's write and a write kInFlight calls ago to the SAME
        // slot, which the rotation spaces out. thisGeneration - 1 can't underflow: thisGeneration is a
        // post-increment starting from 0, so it's always >= 1 here.
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

        // Second staleness dimension -- identity, not just timing. See hashIdentityKey()
        // (OcclusionMath.hpp) for why this uses the caller's own identity fingerprint rather than
        // hashing the (dilated, motion-dependent) box bytes uploaded below.
        const bool boxesStableAcrossLag = havePrevIdentityKey_ && identityKey == prevIdentityKey_;
        prevIdentityKey_ = identityKey;
        havePrevIdentityKey_ = true;

        // Staleness detector: stamps a monotonic generation number into a tiny CPU-authored buffer and
        // round-trips it through the SAME command list, at the SAME point, as the visibility copy
        // above, so both land and retire together. One extra 8-byte writeBuffer+copyBuffer+readBuffer
        // per call, no shader change.
        //
        // WHY: every caller (today, SandboxApp.cpp) assumes its readback is from EXACTLY the
        // immediately preceding call. Comparing the generation actually read back against
        // thisGeneration-1 turns that assumption into something readbackLagIsExactlyOneCall() can
        // check and a caller can react to, instead of a silent wrong answer. Written into
        // genUpload_[writeSlot] now (not at the end) because writeSlot is a slot no earlier call is
        // still using.
        //
        // Rotated for the same reason as the box/visibility buffers: with a single genUpload_ (this
        // file's previous shape), writeBuffer's immediate CPU memcpy could land before the previous
        // call's copyBuffer (only RECORDED here, executed later) had retired, capturing the wrong
        // generation and reporting "stale" almost every frame. The old fix was an unconditional
        // waitIdle before writing the stamp -- correct, but exactly the stall the top-of-class comment
        // shows perturbing GI/denoiser. Rotating genUpload_/genReadback_ like the other buffers removes
        // the need for it.
        res.writeBuffer(genUpload_[writeSlot], &thisGeneration, sizeof(thisGeneration), 0);

        {
            ScopedGpuStat stat(ctx, "HZB test");
            if (!visBufLive_[writeSlot]) {
                ctx.bufferBarrier(visBuf_[writeSlot], ResourceState::Common, ResourceState::UnorderedAccess);
                visBufLive_[writeSlot] = true;
            }
            ctx.setPipeline(testPso_);
            ctx.setBindingSet(testSet_[writeSlot]);
            // F7: `viewport` mirrors occlusion_test.hlsl's gViewport (kept distinctly named from
            // `vp`/gViewProj so the struct can't grow two "vp"-meaning members). A NULL/degenerate
            // viewportRect (w or h <= 0, OcclusionMath.hpp's "whole target" convention) leaves
            // `viewport` zero-init, which the shader reads as gViewport.z <= 0.5 -- run no rect
            // arithmetic, the pre-F7 behaviour.
            struct TestCB { f32 vp[16]; u32 count, w, h, mips; f32 viewport[4]; } cb{};
            std::memcpy(cb.vp, viewProjCache_, sizeof(cb.vp));
            cb.count = count; cb.w = pyramidW_; cb.h = pyramidH_; cb.mips = mipCount_;
            if (viewportRect && viewportRect[2] > 0.0f && viewportRect[3] > 0.0f)
                std::memcpy(cb.viewport, viewportRect, sizeof(cb.viewport));
            // 24 dwords: 16 (gViewProj) + 4 (count/w/h/mips) + 4 (gViewport). Must match
            // occlusion_test.hlsl's TestCB and createStaticPipelines' constantDwords[1] below, or this
            // cbuffer under/over-reads.
            ctx.setConstants(1, &cb, 24);
            ctx.dispatch((count + 63) / 64, 1, 1);
            ctx.uavBarrierBuffer(visBuf_[writeSlot]);
            ctx.bufferBarrier(visBuf_[writeSlot], ResourceState::UnorderedAccess, ResourceState::CopySource);
            ctx.copyBuffer(visReadback_[writeSlot], visBuf_[writeSlot], static_cast<u64>(count) * 4);
            ctx.bufferBarrier(visBuf_[writeSlot], ResourceState::CopySource, ResourceState::UnorderedAccess);
            // genUpload_ (Upload) and genReadback_ (Readback) sit permanently in copy-legal states
            // (GENERIC_READ / COPY_DEST, created below) -- same reasoning boxesBuf_/visReadback_ rely
            // on above, so no extra barrier is needed here.
            ctx.copyBuffer(genReadback_[writeSlot], genUpload_[writeSlot], sizeof(thisGeneration));
        }

        // res.waitIdle() here is the default (debugForceWaitIdle_ defaults true -- see the OPEN
        // paragraph on this class's top comment). It no longer protects boxesBuf_/visBuf_/
        // visReadback_/genUpload_/genReadback_ -- kInFlight rotation does that unconditionally, wait or
        // not -- it exists only because the GI/lighting path still measures differently without it and
        // the cause isn't found; a last-resort, engine-wide drain paid every testBatch() call while
        // culling is enabled, until a narrower fix (a fence on just that resource) replaces it.
        // Positioned after the dispatch+copies above are RECORDED (not yet executed) and before the
        // readback below, exactly where the pre-fix build had it; draining previously submitted work
        // doesn't change which slot is read/written, only whether the CPU stalls here first.
        // M2(d): waitIdleTotalMs_/waitIdleMaxMs_/waitIdleCalls_ below measure this stall's actual cost.
        if (debugForceWaitIdle_) {
            const auto waitT0 = std::chrono::steady_clock::now();
            res.waitIdle();
            const f64 waitMs = std::chrono::duration<f64, std::milli>(
                                   std::chrono::steady_clock::now() - waitT0).count();
            waitIdleTotalMs_ += waitMs;
            waitIdleMaxMs_ = waitMs > waitIdleMaxMs_ ? waitMs : waitIdleMaxMs_;
            ++waitIdleCalls_;
            if ((waitIdleCalls_ & (waitIdleCalls_ - 1)) == 0)
                AVER_INFO("[Occlusion] GPU drain (waitIdle) {:.2f} ms this call, {:.2f} ms mean, "
                          "{:.2f} ms max over {} call(s)",
                          waitMs, waitIdleTotalMs_ / static_cast<f64>(waitIdleCalls_), waitIdleMaxMs_,
                          waitIdleCalls_);
        }

        // The first call reads nothing (there's nothing to read yet): visReadback_ on call 1 holds
        // whatever the allocation happened to contain, and reading it produced a flat "19/19 entities
        // culled (100%)" on every run's opening frame, with the generation check still marking it
        // trustworthy (garbage marked reliable). Skipping the read leaves outVisible all-1 /
        // lastCulled_ 0, which is the correct answer, not a fallback: nothing has been proven occluded
        // yet.
        //
        // minReadableGeneration_ (default 3) generalises this: this is a two-pass scheme where
        // buildPyramid() always seeds from the PREVIOUS frame's depth, so call 2 (which reads call 1's
        // dispatch) sees an empty depth target and reports every box occluded -- the same "19/19"
        // shape, one call later (verified: with only a >1 guard the opening frame correctly read 0/19
        // and the 19 just moved to call 2, identically in raster and ray-driven -- confirming it was
        // about there being no previous frame yet, not which renderer wrote depth).
        // createPyramidResources()'s resize case and ensureBoxCapacity()'s resize-growth case each
        // destroy and rebuild every slot, and the generation stamp has no way
        // to know a slot it's vouching for was just rebuilt empty, so both push this floor forward the
        // same way (std::max, monotonic). MEASURED: on the PTTest repro, the resize gap alone produced
        // 224/691 (32.4%) freshly-culled entities in one window against a baseline of 0. Deliberately
        // the more conservative of the two triggers' margins rather than proving a narrower one
        // suffices -- an extra "assume visible" frame costs a few draws, never a false cull.
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
            // Call 1 has no previous call to be stale relative to (genReadback_ holds init garbage),
            // so treating it as stale would be a false-positive on every run -- trivially fresh
            // instead; the generation check below only proves anything from call 2 onward. Correct now
            // in a way it wasn't before the read guard above: this used to vouch for a result read out
            // of an uninitialised visReadback_ (garbage marked trustworthy); now it vouches for "every
            // box visible, nothing culled", which is genuinely true here.
            readbackLagExpected_ = true;
        } else {
            u64 readGeneration = 0;
            if (res.readBuffer(genReadback_[readSlot], &readGeneration, sizeof(readGeneration), 0) &&
                readGeneration == thisGeneration - 1 && boxesStableAcrossLag) {
                readbackLagExpected_ = true;
            } else if (readGeneration == thisGeneration - 1 && !boxesStableAcrossLag) {
                // Generation matches (GPU timing was right), but the identity check above says the box
                // array itself changed shape/content since the call this answers. Counted separately
                // from a genuine timing miss so a reader of occlusionStaleReadbacks_ (SandboxApp.cpp)
                // isn't misled into re-auditing genUpload_/genReadback_'s copy ordering for a defect
                // that actually lives here.
                ++boxSetChurnedAcrossLag_;
            }
            // else: leave the always-safe default (false) -- either the readback failed, the landed
            // generation isn't the immediately preceding call's, or it was but the boxes it answered
            // for weren't (see boxSetChurnedAcrossLag_ above); the one-call-lag assumption didn't hold,
            // for one reason or another.
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
        // A second, independent way to read a buffer no real dispatch has answered for, distinct from
        // ensureBoxCapacity()'s box-buffer resize (see minReadableGeneration_'s comment, testBatch()).
        // A scene resize (render-scale change, window resize, editor viewport settling) recreates
        // pyramid_ here, but the underlying scene depth texture buildPyramid() reads often resizes at
        // the SAME moment, so the next call or two seed from a depth target nothing has rendered into
        // yet -- the same "opening frame" problem recurring mid-run. MEASURED on the PTTest repro: a
        // resize to 2049x1152 (from an initial 3532x1987, one frame after construction) made calls
        // through generation 5 each report ~100% of ~112 entities culled. Wider than the naive "+3"
        // (one buildPyramid + one testBatch) would predict, since the mechanism wasn't fully isolated;
        // the margin below is the measured safe point plus one call of headroom. Costs only a few
        // extra "assume visible" frames at a resize -- err toward safety, not precision.
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

        // Seed pipeline depends on sampleCount_, so it's (re)compiled here rather than in
        // createStaticPipelines (this file's top comment, point (a)). AVER_HZB_SAMPLES is the live
        // sample count baked in as a compile-time constant (Texture2DMS's template argument and the
        // seed kernel's unrolled tap count).
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

    // Creates (or resizes) all kInFlight copies of testBatch()'s per-call resources together -- see
    // the class's top comment for why there are several. Every copy is always the same capacity; box
    // count is a property of the scene this frame, not of which rotation slot is live.
    bool ensureBoxCapacity(IResourceFactory& res, u32 count) {
        // Stamp buffers: fixed 8 bytes (one u64) each, independent of boxCapacity_, so created ONCE
        // per slot rather than torn down when the box count grows. No dedicated init function: this is
        // the only place testBatch()'s GPU resources are lazily created, so a second such place is
        // just one more thing to remember to call.
        //
        // Guarded on genReadback_[kInFlight - 1] (the last slot's second buffer) -- same "guard on the
        // last thing the sequence creates" idiom the box buffers below use (testSet_[kInFlight - 1]),
        // so a partial failure retries the WHOLE set next time instead of reusing a half-created batch.
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
        // A real resize, about to discard whatever any slot was holding -- see minReadableGeneration_'s
        // comment (testBatch()) and createPyramidResources()'s comment (same "+6" margin) for what was
        // measured. Box-driven and depth/pyramid resizes land in the same settling window on this
        // repro, so there's no evidence they need different margins; using a smaller one here would
        // reopen the same gap. submitGeneration_ is the count of calls BEFORE this one (testBatch()
        // increments it after this runs).
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

    // kInFlight independent copies of every buffer testBatch() writes and later reads back (see the
    // top comment). Indexed by generation % kInFlight; not tied to the engine's own kFrameCount==2 --
    // kInFlight buys headroom over that floor without depending on the exact dispatch/swapchain
    // phasing.
    BufferHandle boxesBuf_[kInFlight] = {}, visBuf_[kInFlight] = {}, visReadback_[kInFlight] = {};
    BindingSetHandle testSet_[kInFlight] = {};
    u32 boxCapacity_ = 0;   // shared: every slot is always sized identically
    bool visBufLive_[kInFlight] = {};

    // The generation a read first becomes trustworthy -- see testBatch()'s read-guard comment. Starts
    // at 3 (call 1 has nothing written; call 2's dispatch ran against a not-yet-real pyramid).
    // createPyramidResources() and ensureBoxCapacity()'s resize branch each push this forward by the
    // same margin whenever they'd otherwise hand back a buffer no real dispatch answered for yet (a
    // scene resize or a box-count growth past headroom, respectively). Monotonic: std::max at both
    // call sites, so an overlapping pair of resizes takes the later, larger floor.
    u64 minReadableGeneration_ = 3;

    // Staleness detector -- see testBatch()'s comment above the genUpload_ writeBuffer call. 8 bytes
    // each, kInFlight copies of the pair; created once per slot in ensureBoxCapacity, independent of
    // boxCapacity_.
    BufferHandle genUpload_[kInFlight] = {}, genReadback_[kInFlight] = {};
    u64 submitGeneration_ = 0;      // incremented once per testBatch() call that reaches the dispatch
    bool readbackLagExpected_ = false;   // see readbackLagIsExactlyOneCall()
    bool debugForceWaitIdle_ = true;     // see setDebugForceWaitIdle(); defaults true -- see the OPEN
                                          // paragraph above this class's kInFlight member

    // M2(d): running totals for the waitIdle() stall, since nothing measured it before. Self-reported
    // wall-clock only (see the [Occlusion] GPU drain log line) -- not a GPU timestamp, not profiler-
    // validated. Reported on a power-of-two cadence, same shape as D3D12Device.cpp's shader-compile
    // report.
    f64 waitIdleTotalMs_ = 0.0, waitIdleMaxMs_ = 0.0;
    u64 waitIdleCalls_ = 0;

    // Second staleness detector -- box IDENTITY across the one-call lag, not GPU timing (see
    // testBatch()'s comment beside boxesStableAcrossLag). Catches a caller whose box array changed
    // shape/content between the call a readback answers and the call consuming it; keyed on the
    // caller-supplied identityKey, not a hash of the box bytes (OcclusionMath.hpp's hashIdentityKey()
    // explains why). CPU-only, no GPU resource.
    u64 prevIdentityKey_ = 0;
    bool havePrevIdentityKey_ = false;
    // Calls where the generation stamp matched but the identity check didn't -- the caller's own box
    // population changed, not this module racing itself. Kept separate from occlusionStaleReadbacks_-
    // style counters so the two failure modes (different files) aren't conflated in a report.
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
