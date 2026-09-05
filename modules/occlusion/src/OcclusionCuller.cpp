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
                   std::vector<u8>& outVisible) override {
        lastTested_ = count;
        lastCulled_ = 0;
        outVisible.assign(count, 1);   // the always-safe default if anything below bails out early
        if (!staticOk_ || !pyramid_ || count == 0) return;
        if (!ensureBoxCapacity(res, count)) return;

        packed_.resize(static_cast<usize>(count) * 8);
        for (u32 i = 0; i < count; ++i) {
            f32* p = &packed_[static_cast<usize>(i) * 8];
            p[0] = boxes[i].min[0]; p[1] = boxes[i].min[1]; p[2] = boxes[i].min[2]; p[3] = 0.0f;
            p[4] = boxes[i].max[0]; p[5] = boxes[i].max[1]; p[6] = boxes[i].max[2]; p[7] = 0.0f;
        }
        res.writeBuffer(boxesBuf_, packed_.data(), static_cast<u64>(packed_.size()) * sizeof(f32), 0);

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
        }

        // See Occlusion.hpp's top comment ("THE ONE COST THIS DESIGN DOES NOT HIDE") for why this
        // stall exists at all: testBatch() promises a CPU-visible answer before it returns, in the
        // SAME frame the copy above was recorded, and waitIdle() is the only synchronisation primitive
        // this module's RHI (IResourceFactory) exposes to reach for. Measured, not assumed -- see the
        // commit this lands with for the actual number on Electric Dreams.
        res.waitIdle();

        rawVisible_.resize(count);
        if (res.readBuffer(visReadback_, rawVisible_.data(), static_cast<u64>(count) * 4, 0)) {
            for (u32 i = 0; i < count; ++i) {
                const bool visible = rawVisible_[i] != 0;
                outVisible[i] = visible ? 1 : 0;
                if (!visible) ++lastCulled_;
            }
        }
    }

    void lastTestCounts(u32& culled, u32& tested) const override { culled = lastCulled_; tested = lastTested_; }

    void releaseAll(IResourceFactory& res) {
        destroyPyramidResources(res);
        if (reducePso_) { res.destroyPipeline(reducePso_); reducePso_ = 0; }
        if (testPso_)   { res.destroyPipeline(testPso_);   testPso_ = 0; }
        if (boxesBuf_)    { res.destroyBuffer(boxesBuf_);    boxesBuf_ = 0; }
        if (visBuf_)      { res.destroyBuffer(visBuf_);      visBuf_ = 0; }
        if (visReadback_) { res.destroyBuffer(visReadback_); visReadback_ = 0; }
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
