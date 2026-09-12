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
                   u64 identityKey, std::vector<u8>& outVisible) override {
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

        packed_.resize(static_cast<usize>(count) * 8);
        for (u32 i = 0; i < count; ++i) {
            f32* p = &packed_[static_cast<usize>(i) * 8];
            p[0] = boxes[i].min[0]; p[1] = boxes[i].min[1]; p[2] = boxes[i].min[2]; p[3] = 0.0f;
            p[4] = boxes[i].max[0]; p[5] = boxes[i].max[1]; p[6] = boxes[i].max[2]; p[7] = 0.0f;
        }
        res.writeBuffer(boxesBuf_, packed_.data(), static_cast<u64>(packed_.size()) * sizeof(f32), 0);

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
        // the compute pipelines (see genUpload_/genReadback_'s own comment below for why a plain
        // buffer-to-buffer copy is enough and CSTest does not need to know this exists).
        //
        // WHY THIS EXISTS: every caller of this module (today, only SandboxApp.cpp) that tries to
        // bound the one-call staleness testBatch() has (see the corrected "TWO-PASS" section in
        // Occlusion.hpp) is reasoning from an assumption -- "the bytes I just read back are from
        // EXACTLY the immediately preceding call, not two or more calls back." That assumption has
        // held in every trace read while building this fix, but nothing in this module previously
        // PROVED it frame to frame; boxesBuf_/visBuf_/visReadback_ are single-buffered and reused by
        // every call with no per-call-in-flight fencing beyond the waitIdle() below, so a caller that
        // skips a call, a device hiccup, or a future change to this file's own submission order could
        // silently stretch the lag past one call with no symptom louder than "occlusion culls a
        // little more than it should have." Comparing the generation number actually read back
        // against the one this exact call submitted -1 turns that possibility into something a
        // caller can check via readbackLagIsExactlyOneCall() and react to loudly, instead of a silent wrong
        // answer no screenshot diff would necessarily catch.
        // THE STAMP IS WRITTEN AT THE *END* OF THIS FUNCTION, NOT HERE, AND THAT IS THE WHOLE FIX.
        //
        // It used to be written right here, and that raced with its own readback so reliably that
        // the detector reported "stale" on essentially every frame -- which, because a caller treats
        // that as "do not trust the answer", meant occlusion culling was DISABLED PERMANENTLY while
        // appearing to be enabled. The warning was real; what it was detecting was itself.
        //
        // The race: genUpload_ is a single buffer and writeBuffer on an upload heap is an immediate
        // CPU memcpy, but copyBuffer below is recorded and does not execute until this frame's
        // command list is submitted -- after this function returns. So call N's write landed in the
        // buffer BEFORE waitIdle() had drained call N-1's copy, and that copy, reading at GPU
        // execute time, captured N instead of the N-1 it was recorded to capture. The readback then
        // disagreed with thisGeneration - 1 by exactly one, forever.
        //
        // Writing the stamp after waitIdle() and after the readback closes it with no new
        // synchronisation: by then the previous copy has provably retired, so the buffer is free,
        // and this call's own copy -- recorded below but executed at submit -- still reads the value
        // this call wants it to carry.
        const u64 thisGeneration = ++submitGeneration_;

        {
            ScopedGpuStat stat(ctx, "HZB test");
            if (!visBufLive_) {
                ctx.bufferBarrier(visBuf_, ResourceState::Common, ResourceState::UnorderedAccess);
                visBufLive_ = true;
            }
            ctx.setPipeline(testPso_);
            ctx.setBindingSet(testSet_);
            struct TestCB { f32 vp[16]; u32 count, w, h, mips; } cb{};
            std::memcpy(cb.vp, viewProjCache_, sizeof(cb.vp));
            cb.count = count; cb.w = pyramidW_; cb.h = pyramidH_; cb.mips = mipCount_;
            ctx.setConstants(1, &cb, 20);
            ctx.dispatch((count + 63) / 64, 1, 1);
            ctx.uavBarrierBuffer(visBuf_);
            ctx.bufferBarrier(visBuf_, ResourceState::UnorderedAccess, ResourceState::CopySource);
            ctx.copyBuffer(visReadback_, visBuf_, static_cast<u64>(count) * 4);
            ctx.bufferBarrier(visBuf_, ResourceState::CopySource, ResourceState::UnorderedAccess);
            // Both genUpload_ (Upload) and genReadback_ (Readback) sit permanently in states that
            // already allow a copy (GENERIC_READ / COPY_DEST -- see their creation below), the exact
            // same reasoning boxesBuf_ and visReadback_ already rely on above, so no extra barrier is
            // needed here either.
            ctx.copyBuffer(genReadback_, genUpload_, sizeof(thisGeneration));
        }

        // CORRECTED -- this used to claim testBatch() "promises a CPU-visible answer... in the SAME
        // frame the copy above was recorded." It does not, and cannot with this RHI: waitIdle() (see
        // Occlusion.hpp's corrected "THE ONE COST" section) only drains GPU work ALREADY SUBMITTED
        // via a previous ExecuteCommandLists/QueueSubmit -- it does not, and structurally cannot,
        // close/submit/wait-for THIS call's own dispatch+copy, which are still sitting unexecuted in
        // the caller's still-open frame command list at this exact point. So this stall waits for
        // whatever WAS already submitted (ordinarily: everything through the end of the PREVIOUS
        // frame) to finish, and the bytes readBuffer() below then reads are whatever the PREVIOUS
        // successful testBatch() call's copy wrote -- whichever call that was; see the generation
        // check just below for how a caller confirms it was exactly one call back rather than
        // assuming so. A real same-frame answer would need a NEW "flush this frame's own command
        // list, then resume recording into it" RHI primitive that does not exist today, on either
        // backend; adding one is out of this module's scope. This stall is real and measured (see the
        // commit this lands with for the actual number on Electric Dreams) -- it is just paying for a
        // stale answer, not a fresh one.
        res.waitIdle();

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
        // AND IT IS > 2, NOT > 1, BECAUSE THE FIRST DISPATCH IS ALSO WORTHLESS -- for a different
        // reason than the first readback, which is why both guards are needed. This is a two-pass
        // occlusion scheme: buildPyramid() runs from the scene depth texture BEFORE this frame's
        // scene is drawn, so it is always reading the PREVIOUS frame's depth. On the opening frame
        // there is no previous frame, so pyramid mip 0 comes from a depth target nothing has
        // rendered into, every box tests as occluded against it, and call 2 -- which reads call 1's
        // dispatch -- reported a flat "19 of 19 tested entities culled (100.0%)". A caller acting on
        // that culls the ENTIRE SCENE for one frame.
        //
        // MEASURED, which is how the second guard was found at all: with only the > 1 guard the
        // opening frame correctly read 0 of 19, and the 19 simply moved to the next report instead
        // of disappearing. Same scene, same frame count, identical in raster and ray-driven mode --
        // so it was never about which renderer wrote the depth, only about there not being one yet.
        if (thisGeneration > 2) {
            rawVisible_.resize(count);
            if (res.readBuffer(visReadback_, rawVisible_.data(), static_cast<u64>(count) * 4, 0)) {
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
            if (res.readBuffer(genReadback_, &readGeneration, sizeof(readGeneration), 0) &&
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

        // AND ONLY NOW THE STAMP -- see the long comment where thisGeneration is computed. The copy
        // recorded above has not executed yet (it goes out with this frame's command list, after
        // this function returns), so it will read exactly this value; and the PREVIOUS call's copy
        // has provably retired, because waitIdle() above drained it and the readback just consumed
        // its result. Both halves of the race are closed by position alone, with no extra fence and
        // no second buffer.
        res.writeBuffer(genUpload_, &thisGeneration, sizeof(thisGeneration), 0);
    }

    void lastTestCounts(u32& culled, u32& tested) const override { culled = lastCulled_; tested = lastTested_; }

    bool readbackLagIsExactlyOneCall() const override { return readbackLagExpected_; }

    u64 boxIdentityChurnCount() const override { return boxSetChurnedAcrossLag_; }

    void releaseAll(IResourceFactory& res) {
        destroyPyramidResources(res);
        if (reducePso_) { res.destroyPipeline(reducePso_); reducePso_ = 0; }
        if (testPso_)   { res.destroyPipeline(testPso_);   testPso_ = 0; }
        if (boxesBuf_)    { res.destroyBuffer(boxesBuf_);    boxesBuf_ = 0; }
        if (visBuf_)      { res.destroyBuffer(visBuf_);      visBuf_ = 0; }
        if (visReadback_) { res.destroyBuffer(visReadback_); visReadback_ = 0; }
        if (genUpload_)   { res.destroyBuffer(genUpload_);   genUpload_ = 0; }
        if (genReadback_) { res.destroyBuffer(genReadback_); genReadback_ = 0; }
        if (testSet_)     { res.destroyBindingSet(testSet_); testSet_ = 0; }
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
            p.layout.constantDwords[1] = 20;   // b1: TestCB (float4x4 + 4 uints)
            testPso_ = res.createComputePipeline(p);
        }
        if (!testPso_) { AVER_ERROR("[Occlusion] HZB test pipeline unavailable"); ok = false; }
        return ok;
    }

    bool createPyramidResources(IResourceFactory& res) {
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

    bool ensureBoxCapacity(IResourceFactory& res, u32 count) {
        // The staleness-detector's stamp buffers: fixed at 8 bytes (one u64), independent of
        // boxCapacity_, so they are created ONCE (guarded separately) rather than being torn down
        // and rebuilt every time the box count grows past its own headroom below. Not worth their
        // own dedicated init function: this is the only place testBatch()'s GPU-side resources are
        // lazily created at all, so a second such place would just be a second thing to remember to
        // call.
        //
        // Guarded on genReadback_ (the SECOND of the pair), not genUpload_ (the first) -- the same
        // "guard on the last thing the sequence creates" idiom the box buffers below already use
        // (guarded on testSet_, their own last step), so a call that got genUpload_ but failed on
        // genReadback_ retries BOTH next time instead of silently reusing a half-created pair with
        // genReadback_ stuck at 0 forever.
        if (!genReadback_) {
            if (genUpload_) { res.destroyBuffer(genUpload_); genUpload_ = 0; }
            BufferDesc gud;
            gud.bytes = sizeof(u64);
            gud.kind = BufferKind::Upload;
            gud.debugName = "HZB test generation stamp (upload)";
            genUpload_ = res.createBuffer(gud);

            BufferDesc grd;
            grd.bytes = sizeof(u64);
            grd.kind = BufferKind::Readback;
            grd.debugName = "HZB test generation stamp (readback)";
            genReadback_ = res.createBuffer(grd);

            if (!genUpload_ || !genReadback_) {
                AVER_ERROR("[Occlusion] HZB staleness-detector buffers unavailable -- "
                           "readbackLagIsExactlyOneCall() will report false forever, which is safe (every "
                           "caller falls back to no culling) but gives up the culling benefit entirely");
                return false;
            }
        }
        if (count <= boxCapacity_ && testSet_) return true;
        if (boxesBuf_)    { res.destroyBuffer(boxesBuf_);    boxesBuf_ = 0; }
        if (visBuf_)      { res.destroyBuffer(visBuf_);      visBuf_ = 0; }
        if (visReadback_) { res.destroyBuffer(visReadback_); visReadback_ = 0; }
        if (testSet_)     { res.destroyBindingSet(testSet_); testSet_ = 0; }
        visBufLive_ = false;

        // Headroom so a count that drifts by a handful of entities frame to frame (an actor spawned
        // or destroyed) does not reallocate every single frame -- 25% or 64, whichever is larger.
        boxCapacity_ = count + std::max<u32>(count / 4, 64);

        BufferDesc bd;
        bd.bytes = static_cast<u64>(boxCapacity_) * 32;   // AabbGpu: 8 floats
        bd.kind = BufferKind::Upload;
        bd.debugName = "HZB test boxes";
        boxesBuf_ = res.createBuffer(bd);

        BufferDesc vd;
        vd.bytes = static_cast<u64>(boxCapacity_) * 4;
        vd.kind = BufferKind::Default;
        vd.allowUnorderedAccess = true;
        vd.debugName = "HZB visible";
        visBuf_ = res.createBuffer(vd);

        BufferDesc rd;
        rd.bytes = static_cast<u64>(boxCapacity_) * 4;
        rd.kind = BufferKind::Readback;
        rd.debugName = "HZB visible readback";
        visReadback_ = res.createBuffer(rd);

        BindingSetDesc sd;
        sd.srvCount = 2; sd.uavCount = 1;
        sd.srvKinds[0] = SlotKind::StructuredBuffer;
        sd.srvKinds[1] = SlotKind::Texture2D;
        sd.uavKinds[0] = SlotKind::StructuredBuffer;
        testSet_ = res.createBindingSet(sd);

        if (!boxesBuf_ || !visBuf_ || !visReadback_ || !testSet_) {
            AVER_ERROR("[Occlusion] HZB test resources for {} boxes unavailable", boxCapacity_);
            return false;
        }
        res.setSrvBuffer(testSet_, 0, boxesBuf_, 32, boxCapacity_, 0);
        if (pyramid_) res.setSrv(testSet_, 1, pyramid_, kAllMips);
        res.setUavBuffer(testSet_, 0, visBuf_, 4, boxCapacity_, 0);
        return true;
    }

    u32 sceneW_ = 0, sceneH_ = 0, sampleCount_ = 0;
    u32 pyramidW_ = 0, pyramidH_ = 0, mipCount_ = 0;
    TextureHandle pyramid_ = 0;

    bool staticOk_ = false;
    PipelineHandle reducePso_ = 0, testPso_ = 0, seedPso_ = 0;
    BindingSetHandle seedSet_ = 0, testSet_ = 0;
    std::vector<BindingSetHandle> reduceSets_;

    BufferHandle boxesBuf_ = 0, visBuf_ = 0, visReadback_ = 0;
    u32 boxCapacity_ = 0;
    bool visBufLive_ = false;

    // STALENESS DETECTOR -- see testBatch()'s own comment above the writeBuffer(genUpload_, ...)
    // call. 8 bytes each; created once in ensureBoxCapacity, independent of boxCapacity_.
    BufferHandle genUpload_ = 0, genReadback_ = 0;
    u64 submitGeneration_ = 0;      // incremented once per testBatch() call that reaches the dispatch
    bool readbackLagExpected_ = false;   // see readbackLagIsExactlyOneCall()

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
