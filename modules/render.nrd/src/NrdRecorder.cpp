#include "aver/render/nrd/NrdRecorder.hpp"

#include "aver/core/Log.hpp"

#include <cstring>

namespace aver::render::nrd {

Recorder::~Recorder() { destroy(); }

// ---------------------------------------------------------------------------------------------
// create: the instance, and one compute pipeline per shader NRD embedded.
// ---------------------------------------------------------------------------------------------
bool Recorder::create(rhi::IDevice& dev, const DenoiserKind* kinds, u32 count) {
    destroy();
    if (!Denoiser::available()) {
        AVER_INFO("[NRD] not in this build (AVER_WITH_NRD=OFF); the hand-written filter stands");
        return false;
    }
    // THE BACKEND TEST IS FIRST AND IT IS NOT A FORMALITY -- see this class's header comment. On
    // Vulkan every pipeline below would fail anyway, but it would fail as eleven confusing
    // createComputePipeline errors rather than as one sentence naming the actual reason.
    if (dev.backend() != rhi::Backend::D3D12) {
        AVER_INFO("[NRD] denoising is D3D12-only (NRD wants its constant buffer and samplers in "
                  "register space 1, which Vulkan deliberately refuses -- see "
                  "VulkanResourceFactory::descriptorLayout). Running undenoised on this backend.");
        return false;
    }
    dev_ = &dev;
    res_ = dev.resources();
    if (!res_) return false;
    if (!denoiser_.create(kinds, count)) {
        AVER_WARN("[NRD] could not create an instance for {} denoiser(s)", count);
        return false;
    }

    const InstanceLayout lay = denoiser_.layout();
    // SPACE, SAMPLERS AND COUNTS COME FROM NRD, never from a constant here. D3D12 validates the
    // shader against the root signature at CreateComputePipelineState, so a layout that disagrees
    // with the DXIL is rejected by name -- which is exactly what PrecompiledShaderTest leans on.
    for (u32 i = 0; i < lay.pipelineCount; ++i) {
        const PipelineInfo& p = lay.pipelines[i];
        if (!p.dxil.valid()) {
            AVER_WARN("[NRD] pipeline {} ('{}') embedded no DXIL", i, p.debugName ? p.debugName : "?");
            destroy();
            return false;
        }
        u32 srv = 0, uav = 0;
        for (u32 r = 0; r < p.rangeCount; ++r) {
            if (p.ranges[r].cls == ResourceClass::StorageTexture) uav += p.ranges[r].count;
            else                                                  srv += p.ranges[r].count;
        }
        if (srv > rhi::kMaxBindingSlots || uav > rhi::kMaxBindingSlots) {
            AVER_WARN("[NRD] pipeline {} wants {} SRV / {} UAV, past this RHI's {} per table",
                      i, srv, uav, rhi::kMaxBindingSlots);
            destroy();
            return false;
        }

        rhi::ShaderDesc sd{};
        sd.stage        = rhi::ShaderStage::Compute;
        sd.bytecode     = p.dxil.data;
        sd.bytecodeSize = p.dxil.size;
        const rhi::ShaderHandle cs = res_->createShader(sd);
        if (!cs) { AVER_WARN("[NRD] createShader failed for pipeline {}", i); destroy(); return false; }

        rhi::PipelineLayout pl{};
        pl.srvCount = srv;
        pl.uavCount = uav;
        // constantDwords stays 0 for every slot, which is what declares slot 0 a ROOT CBV rather
        // than root constants -- the only workable choice here, since NRD's constant block runs to
        // ~912 bytes and a whole root signature is 256.
        pl.constantSpace = lay.binding.constantBufferAndSamplersSpace;
        pl.samplerSpace  = lay.binding.constantBufferAndSamplersSpace;
        pl.samplerCount  = lay.binding.samplerCount < 4 ? lay.binding.samplerCount : 4;
        for (u32 s = 0; s < pl.samplerCount; ++s) {
            pl.samplers[s].filter  = lay.binding.samplers[s] == SamplerKind::LinearClamp
                                       ? rhi::Filter::Linear : rhi::Filter::Point;
            pl.samplers[s].address = rhi::AddressMode::Clamp;
        }

        rhi::ComputePipelineDesc cpd{};
        cpd.cs     = cs;
        cpd.layout = pl;
        const rhi::PipelineHandle pipe = res_->createComputePipeline(cpd);
        res_->destroyShader(cs);   // the PSO owns the bytecode now
        if (!pipe) {
            AVER_WARN("[NRD] createComputePipeline failed for '{}'", p.debugName ? p.debugName : "?");
            destroy();
            return false;
        }
        pipelines_.push_back(pipe);
    }

    AVER_INFO("[NRD] {} pipeline(s) built, {} permanent + {} transient pool texture(s) to allocate, "
              "constants up to {} B in space {}",
              lay.pipelineCount, lay.permanentPoolSize, lay.transientPoolSize,
              lay.binding.constantBufferMaxDataSize, lay.binding.constantBufferAndSamplersSpace);
    return true;
}

void Recorder::releasePools() {
    if (!res_) return;
    for (rhi::TextureHandle t : permanent_) if (t) res_->destroyTexture(t);
    for (rhi::TextureHandle t : transient_) if (t) res_->destroyTexture(t);
    permanent_.clear();
    transient_.clear();
    for (Slot& s : slots_) if (s.set) res_->destroyBindingSet(s.set);
    slots_.clear();
    outDiffHitDist_ = 0;
}

void Recorder::destroy() {
    releasePools();
    if (res_) for (rhi::PipelineHandle p : pipelines_) if (p) res_->destroyPipeline(p);
    pipelines_.clear();
    denoiser_.destroy();
    dev_ = nullptr;
    res_ = nullptr;
    width_ = height_ = 0;
    historyStale_ = true;
    loggedPlanSize_ = 0;
}

// ---------------------------------------------------------------------------------------------
// resize: the pools. NRD names a format and a downsample factor per texture and then refers to
// them by index forever.
// ---------------------------------------------------------------------------------------------
bool Recorder::resize(u32 width, u32 height) {
    if (!valid() || width == 0 || height == 0) return false;
    if (width == width_ && height == height_ && !permanent_.empty()) return true;

    releasePools();
    width_ = width;
    height_ = height;
    // A REALLOCATION IS A HISTORY RESET, and saying so here rather than hoping the caller remembers
    // is the point of the flag: NRD's permanent pool IS its temporal history, and handing it a
    // fresh (undefined) texture while telling it the history is valid is how a denoiser produces
    // confident garbage for the first several frames.
    historyStale_ = true;

    const InstanceLayout lay = denoiser_.layout();
    auto make = [&](const PoolTexture& pt, const char* what, u32 i) -> rhi::TextureHandle {
        const u32 f = pt.downsampleFactor ? pt.downsampleFactor : 1u;
        rhi::TextureDesc d{};
        d.width  = (width_  + f - 1) / f;
        d.height = (height_ + f - 1) / f;
        d.format = pt.format;
        // BOTH BINDS, because NRD genuinely uses each pool texture both ways -- it writes one pass's
        // output through a UAV and reads it as an SRV in the next, which is why the barriers below
        // are UAV barriers rather than state transitions.
        d.bind   = static_cast<rhi::ResourceBind>(
                       static_cast<u32>(rhi::ResourceBind::ShaderResource) |
                       static_cast<u32>(rhi::ResourceBind::UnorderedAccess));
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName = what;
        const rhi::TextureHandle t = res_->createTexture(d);
        if (!t) AVER_WARN("[NRD] {} pool texture {} ({}) failed to allocate", what, i,
                          pt.nrdFormatName ? pt.nrdFormatName : "?");
        return t;
    };
    for (u32 i = 0; i < lay.permanentPoolSize; ++i) {
        const rhi::TextureHandle t = make(lay.permanentPool[i], "NRD permanent", i);
        if (!t) { releasePools(); return false; }
        permanent_.push_back(t);
    }
    for (u32 i = 0; i < lay.transientPoolSize; ++i) {
        const rhi::TextureHandle t = make(lay.transientPool[i], "NRD transient", i);
        if (!t) { releasePools(); return false; }
        transient_.push_back(t);
    }
    return true;
}

rhi::TextureHandle Recorder::poolTexture(SlotRole role, u32 index, const Inputs& in) const {
    switch (role) {
        case SlotRole::InViewZ:               return in.viewZ;
        case SlotRole::InMotionVectors:       return in.motionVectors;
        case SlotRole::InNormalRoughness:     return in.normalRoughness;
        case SlotRole::InDiffuseHitDistance:  return in.diffuseHitDist;
        case SlotRole::PermanentPool:         return index < permanent_.size() ? permanent_[index] : 0;
        case SlotRole::TransientPool:         return index < transient_.size() ? transient_[index] : 0;
        // OUT_DIFF_HITDIST is a pool texture like any other as far as NRD is concerned -- it hands
        // back whichever permanent slot holds the result. It is resolved by the caller reading
        // outputDiffuseHitDistance() after record(), which is set from whatever this dispatch bound.
        case SlotRole::OutDiffuseHitDistance: return index < permanent_.size() ? permanent_[index] : 0;
        default:                              return 0;
    }
}

// ---------------------------------------------------------------------------------------------
// record: plan, then write every descriptor table, then record every dispatch.
// ---------------------------------------------------------------------------------------------
bool Recorder::record(rhi::IRenderContext& ctx, const FrameSettings& settings, const Inputs& in,
                      const u32* denoiserIndices, u32 indexCount) {
    if (!valid() || permanent_.empty()) return false;
    if (!in.viewZ || !in.motionVectors || !in.normalRoughness || !in.diffuseHitDist) {
        AVER_WARN("[NRD] record called with a null input texture; skipping the pass");
        return false;
    }
    if (!denoiser_.setFrameSettings(settings)) {
        AVER_WARN("[NRD] rejected this frame's settings");
        return false;
    }

    const Dispatch* plan = nullptr;
    u32 planCount = 0;
    if (!denoiser_.dispatches(denoiserIndices, indexCount, plan, planCount) || planCount == 0)
        return false;

    const InstanceLayout lay = denoiser_.layout();

    // ---- descriptor tables FIRST, every one of them, before a single dispatch is recorded ----
    //
    // THE ORDERING IS LOAD-BEARING AND IT IS THE WHOLE REASON slots_ EXISTS. A binding set is a
    // descriptor table the GPU reads when the command list EXECUTES, not when it is recorded --
    // so mutating one table between two dispatches that both use it does not give those dispatches
    // different bindings, it gives BOTH of them the last write. One table per dispatch, all filled
    // up front, is what makes the plan mean what it says. filterMips() in VoxiRenderer follows the
    // same rule for the same reason (mipBindings_ is an array, not a handle that gets rewritten).
    if (slots_.size() != planCount) {
        for (Slot& s : slots_) if (s.set) res_->destroyBindingSet(s.set);
        slots_.assign(planCount, Slot{});
    }
    for (u32 d = 0; d < planCount; ++d) {
        const Dispatch& dp = plan[d];
        u32 srv = 0, uav = 0;
        for (u32 b = 0; b < dp.bindingCount; ++b)
            (dp.bindings[b].cls == ResourceClass::StorageTexture ? uav : srv)++;

        Slot& slot = slots_[d];
        if (!slot.set || slot.srvCount != srv || slot.uavCount != uav) {
            if (slot.set) res_->destroyBindingSet(slot.set);
            rhi::BindingSetDesc bd{};
            bd.srvCount        = srv;
            bd.uavCount        = uav;
            bd.srvBaseRegister = lay.binding.resourcesBaseRegister;
            bd.uavBaseRegister = lay.binding.resourcesBaseRegister;
            slot.set      = res_->createBindingSet(bd);
            slot.srvCount = srv;
            slot.uavCount = uav;
            if (!slot.set) { AVER_WARN("[NRD] createBindingSet failed for dispatch {}", d); return false; }
        }

        u32 sIdx = 0, uIdx = 0;
        for (u32 b = 0; b < dp.bindingCount; ++b) {
            const SlotBinding& sb = dp.bindings[b];
            const rhi::TextureHandle t = poolTexture(sb.role, sb.poolIndex, in);
            if (!t) {
                AVER_WARN("[NRD] dispatch '{}' slot {} (role {}, pool index {}) resolved to no "
                          "texture", dp.name ? dp.name : "?", b, (u32)sb.role, sb.poolIndex);
                return false;
            }
            if (sb.cls == ResourceClass::StorageTexture) {
                res_->setUav(slot.set, uIdx++, t, 0);
                if (sb.role == SlotRole::OutDiffuseHitDistance) outDiffHitDist_ = t;
            } else {
                res_->setSrv(slot.set, sIdx++, t);
            }
        }
    }

    if (loggedPlanSize_ != planCount) {
        AVER_INFO("[NRD] plan: {} dispatch(es) at {}x{}{}", planCount, width_, height_,
                  settings.resetHistory ? " (history reset this frame)" : "");
        loggedPlanSize_ = planCount;
    }

    // ---- now the work ----
    // EVERY POOL TEXTURE IS LEFT IN UnorderedAccess BETWEEN DISPATCHES, and a UAV barrier -- not a
    // state transition -- is what orders them. NRD reads its pools through SRVs and writes them
    // through UAVs within one frame, often the same texture in consecutive passes; a full
    // transition per slot would be both wrong (the resource genuinely stays writable) and far more
    // barriers than the dependency needs. A uav barrier after each dispatch says exactly what is
    // true: whatever the next dispatch reads, this one has finished writing.
    rhi::ScopedGpuStat gpuStat(ctx, "NRD denoise");
    for (u32 d = 0; d < planCount; ++d) {
        const Dispatch& dp = plan[d];
        if (dp.pipelineIndex >= pipelines_.size()) {
            AVER_WARN("[NRD] dispatch {} names pipeline {}, past the {} built", d,
                      dp.pipelineIndex, (u32)pipelines_.size());
            return false;
        }
        ctx.setPipeline(pipelines_[dp.pipelineIndex]);
        ctx.setBindingSet(slots_[d].set);
        // constantsUnchanged is NRD telling us the bytes are identical to the previous dispatch's.
        // Honoured rather than ignored: a root CBV upload is a real copy, and NRD sets this often.
        if (dp.constants && dp.constantsSize && !dp.constantsUnchanged)
            ctx.setConstantBuffer(lay.binding.constantBufferRegister, dp.constants, dp.constantsSize);
        ctx.dispatch(dp.groupsX ? dp.groupsX : 1u, dp.groupsY ? dp.groupsY : 1u, 1u);
        for (u32 b = 0; b < dp.bindingCount; ++b) {
            if (dp.bindings[b].cls != ResourceClass::StorageTexture) continue;
            const rhi::TextureHandle t = poolTexture(dp.bindings[b].role, dp.bindings[b].poolIndex, in);
            if (t) ctx.uavBarrierTexture(t);
        }
    }

    historyStale_ = false;
    return true;
}

}  // namespace aver::render::nrd
