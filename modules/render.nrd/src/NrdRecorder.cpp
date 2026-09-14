#include "aver/render/nrd/NrdRecorder.hpp"

#include "aver/core/Log.hpp"

#include <cstring>

namespace aver::render::nrd {

namespace {

// M6: bytes per texel for the plain (non-block-compressed) formats NRD's pools and OUT_* targets are
// ever built from -- see PoolTexture's own comment on `format` in NrdDenoiser.hpp: NRD's format list
// is wider than this RHI's, and Unknown is how that gap already reports itself. No existing helper
// answers this (checked modules/rhi/include for a bytes-per-texel or format-size function; there is
// none), so it is restated here rather than exported: nothing outside this one memory report needs it
// yet, and a second copy of a table this small is cheaper than a shared header neither side has asked
// for. 0 means "cannot size this one" -- a block-compressed format (never actually requested here; a
// UAV-writable compute target could not be block-compressed in the first place) or Unknown -- and the
// caller must count it separately rather than guess, per the C-7 log contract.
u32 bytesPerTexel(rhi::Format f) {
    switch (f) {
        case rhi::Format::R8Unorm:
        case rhi::Format::R8Uint:         return 1;
        case rhi::Format::RG8Unorm:
        case rhi::Format::R16Unorm:
        case rhi::Format::R16Uint:
        case rhi::Format::R16F:           return 2;
        case rhi::Format::RGBA8Unorm:
        case rhi::Format::RGBA8UnormSrgb:
        case rhi::Format::R32Float:
        case rhi::Format::R32Uint:
        case rhi::Format::D32Float:
        case rhi::Format::R32Typeless:
        case rhi::Format::RG16F:
        case rhi::Format::RGB10A2Unorm:   return 4;
        case rhi::Format::RGBA16F:
        case rhi::Format::RG32Float:      return 8;
        case rhi::Format::BC1Unorm:
        case rhi::Format::BC1UnormSrgb:
        case rhi::Format::BC3Unorm:
        case rhi::Format::BC3UnormSrgb:
        case rhi::Format::BC5Unorm:
        case rhi::Format::BC7Unorm:
        case rhi::Format::BC7UnormSrgb:
        case rhi::Format::Unknown:
        default:                           return 0;
    }
}

} // namespace

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
    // WHICH OUT_* TARGETS resize() WILL HAVE TO ALLOCATE, decided here because this is the only place
    // that is ever told. See the members' own comment for why those targets are ours rather than
    // NRD's, and why allocating the radiance one for a caller that never asked for REBLUR_DIFFUSE
    // would be 8 bytes a pixel of waste.
    for (u32 i = 0; i < count; ++i) {
        if (kinds[i] == DenoiserKind::ReblurDiffuseOcclusion) wantDiffHitDist_    = true;
        if (kinds[i] == DenoiserKind::ReblurDiffuse)          wantDiffRadHitDist_ = true;
    }
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
    if (outDiffHitDistTex_)    { res_->destroyTexture(outDiffHitDistTex_);    outDiffHitDistTex_ = 0; }
    if (outDiffRadHitDistTex_) { res_->destroyTexture(outDiffRadHitDistTex_); outDiffRadHitDistTex_ = 0; }
    if (mvScratch_)            { res_->destroyTexture(mvScratch_);            mvScratch_ = 0; }
    for (Slot& s : slots_) if (s.set) res_->destroyBindingSet(s.set);
    slots_.clear();
    outDiffHitDist_ = 0;
    outDiffRadHitDist_ = 0;
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
    wantDiffHitDist_ = wantDiffRadHitDist_ = false;
}

// ---------------------------------------------------------------------------------------------
// resize: the pools. NRD names a format and a downsample factor per texture and then refers to
// them by index forever.
// ---------------------------------------------------------------------------------------------
bool Recorder::resize(u32 width, u32 height) {
    if (!valid() || width == 0 || height == 0) return false;
    if (width == width_ && height == height_ && !permanent_.empty()) return true;
    // A POOL THAT FAILED TO ALLOCATE AT A GIVEN SIZE WILL FAIL AGAIN AT THAT SIZE, and a caller
    // that asks once per frame turns one real problem into an unreadable wall of identical log
    // lines -- which is exactly what an unmapped NRD format did on the first run of this pass.
    // Remembering the size that failed makes the report happen once and the retry happen only when
    // something actually changed.
    if (width == failedWidth_ && height == failedHeight_) return false;

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
        if (!t) { releasePools(); failedWidth_ = width; failedHeight_ = height; return false; }
        permanent_.push_back(t);
    }
    for (u32 i = 0; i < lay.transientPoolSize; ++i) {
        const rhi::TextureHandle t = make(lay.transientPool[i], "NRD transient", i);
        if (!t) { releasePools(); failedWidth_ = width; failedHeight_ = height; return false; }
        transient_.push_back(t);
    }
    // ---- and the OUT_* targets, which are the integration's own (see the members' comment) ----
    // Full resolution and never downsampled: NRD reports a downsampleFactor for its POOL textures
    // and says nothing about these, because an output is the application's to size -- and every pass
    // that writes one is dispatched over the full rect.
    auto makeOut = [&](rhi::Format f, const char* what) -> rhi::TextureHandle {
        rhi::TextureDesc d{};
        d.width  = width_;
        d.height = height_;
        d.format = f;
        d.bind   = static_cast<rhi::ResourceBind>(
                       static_cast<u32>(rhi::ResourceBind::ShaderResource) |
                       static_cast<u32>(rhi::ResourceBind::UnorderedAccess));
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName = what;
        const rhi::TextureHandle t = res_->createTexture(d);
        if (!t) AVER_WARN("[NRD] {} failed to allocate at {}x{}", what, width_, height_);
        return t;
    };
    if (wantDiffHitDist_) {
        outDiffHitDistTex_ = makeOut(rhi::Format::R16Unorm, "NRD OUT_DIFF_HITDIST");
        if (!outDiffHitDistTex_) { releasePools(); failedWidth_ = width; failedHeight_ = height; return false; }
    }
    if (wantDiffRadHitDist_) {
        outDiffRadHitDistTex_ = makeOut(rhi::Format::RGBA16F, "NRD OUT_DIFF_RADIANCE_HITDIST");
        if (!outDiffRadHitDistTex_) { releasePools(); failedWidth_ = width; failedHeight_ = height; return false; }
    }
    failedWidth_ = failedHeight_ = 0;   // this size works; a later one may still not

    // M6: pool memory, COMPUTED FROM THE TEXTURE DESCRIPTIONS ABOVE, not queried from the device --
    // this RHI has no per-resource residency query, so the only honest number to report is exactly
    // what this function itself just asked createTexture() for, at the same downsample factor per
    // pool entry `make()` used above. Logged once per successful (re)allocation, i.e. exactly the
    // calls that reach here rather than the idempotent early return at the top of resize() -- a
    // caller that asks every frame with an unchanged size never sees a repeat of this line.
    {
        u64 bytes = 0;
        u32 unknownCount = 0;
        auto addPool = [&](const PoolTexture* pool, u32 poolSize) {
            for (u32 i = 0; i < poolSize; ++i) {
                const PoolTexture& pt = pool[i];
                const u32 f = pt.downsampleFactor ? pt.downsampleFactor : 1u;
                const u32 w = (width_ + f - 1) / f, h = (height_ + f - 1) / f;
                const u32 bpt = bytesPerTexel(pt.format);
                if (bpt) bytes += static_cast<u64>(w) * h * bpt;
                else     ++unknownCount;
            }
        };
        addPool(lay.permanentPool, lay.permanentPoolSize);
        addPool(lay.transientPool, lay.transientPoolSize);
        // The OUT_* targets are full resolution (see makeOut() above), and their formats are fixed
        // constants of this file rather than an NRD PoolTexture -- makeOut()'s own two call sites
        // name them (rhi::Format::R16Unorm, rhi::Format::RGBA16F) -- so they are sized through the
        // same bytesPerTexel() table rather than a second, separately-maintained pair of numbers.
        if (outDiffHitDistTex_)
            bytes += static_cast<u64>(width_) * height_ * bytesPerTexel(rhi::Format::R16Unorm);
        if (outDiffRadHitDistTex_)
            bytes += static_cast<u64>(width_) * height_ * bytesPerTexel(rhi::Format::RGBA16F);
        AVER_INFO("[NRD] pools: {} permanent + {} transient texture(s) at {}x{}, {:.1f} MiB computed "
                  "from their formats ({} of unknown size not counted)",
                  lay.permanentPoolSize, lay.transientPoolSize, width_, height_,
                  static_cast<f64>(bytes) / (1024.0 * 1024.0), unknownCount);
    }

    return true;
}

rhi::TextureHandle Recorder::poolTexture(SlotRole role, u32 index, const Inputs& in) const {
    switch (role) {
        case SlotRole::InViewZ:               return in.viewZ;
        // THE SCRATCH, NOT THE CALLER'S TEXTURE -- see mvScratch_ in the header. REBLUR_DIFFUSE writes
        // this slot, so handing over the engine's velocity target would both remove the device (no UAV
        // flag on a render target) and corrupt a buffer the rest of the frame reads.
        case SlotRole::InMotionVectors:       return mvScratch_ ? mvScratch_ : in.motionVectors;
        case SlotRole::InNormalRoughness:     return in.normalRoughness;
        case SlotRole::InDiffuseHitDistance:  return in.diffuseHitDist;
        case SlotRole::InDiffuseRadianceHitDistance: return in.diffuseRadianceHitDist;
        case SlotRole::PermanentPool:         return index < permanent_.size() ? permanent_[index] : 0;
        case SlotRole::TransientPool:         return index < transient_.size() ? transient_[index] : 0;
        // AN OUT_* SLOT IGNORES `index` ON PURPOSE. NRD leaves indexInPool unwritten for every
        // non-pool resource type, so it arrives here as 0 for every dispatch; honouring it is what
        // aliased both outputs onto permanent pool texture 0 and removed the device. The members'
        // comment in the header has the full account. NRD also re-reads these targets between its own
        // passes (Reblur_DiffuseOcclusion.hpp defines DIFF_TEMP1 as OUT_DIFF_HITDIST outright), which
        // is why they are created with both binds rather than write-only.
        case SlotRole::OutDiffuseHitDistance:         return outDiffHitDistTex_;
        case SlotRole::OutDiffuseRadianceHitDistance: return outDiffRadHitDistTex_;
        default:                              return 0;
    }
}

// ---------------------------------------------------------------------------------------------
// record: plan, then write every descriptor table, then record every dispatch.
// ---------------------------------------------------------------------------------------------
bool Recorder::record(rhi::IRenderContext& ctx, const FrameSettings& settings, const Inputs& in,
                      const u32* denoiserIndices, u32 indexCount) {
    if (!valid() || permanent_.empty()) return false;
    // THE G-BUFFER TRIO IS ALWAYS REQUIRED; the two signal textures are not. Which signals a frame
    // needs is decided by WHICH DENOISERS it selected, and the plan is what knows that -- so a null
    // signal is caught per slot as the tables are filled below (a slot resolving to no texture is
    // already a named failure there), not guessed at up front. Demanding both here would refuse a
    // perfectly good occlusion-only frame from a caller that produces no radiance texture.
    if (!in.viewZ || !in.motionVectors || !in.normalRoughness) {
        AVER_WARN("[NRD] record called with a null G-buffer input; skipping the pass");
        return false;
    }
    // ---- NRD'S OWN COPY OF IN_MV, because NRD WRITES IN_MV (see mvScratch_ in the header) ----
    // Allocated from the caller's own velocity texture so the format can never disagree -- a
    // mismatched CopyResource is undefined on a release runtime rather than a failed call -- with only
    // the bind flags and the resting state overridden. Lazy rather than in resize() because resize()
    // is never told the velocity format; released with the pools, so a resolution change rebuilds it.
    if (!mvScratch_) {
        rhi::TextureDesc md{};
        if (!res_->textureInfo(in.motionVectors, md)) {
            AVER_WARN("[NRD] could not read the motion-vector texture's description; skipping the pass");
            return false;
        }
        md.bind = static_cast<rhi::ResourceBind>(
                      static_cast<u32>(rhi::ResourceBind::ShaderResource) |
                      static_cast<u32>(rhi::ResourceBind::UnorderedAccess));
        md.initialState = rhi::ResourceState::UnorderedAccess;
        md.debugName    = "NRD IN_MV scratch (NRD writes this)";
        mvScratch_      = res_->createTexture(md);
        if (!mvScratch_) {
            AVER_WARN("[NRD] could not allocate the IN_MV scratch copy; skipping the pass");
            return false;
        }
    }
    // BRACKETED AND PUT BACK, because the caller's velocity texture is a RENDER TARGET that the frame
    // is about to clear and draw into. Leaving it in CopySource is not a silent inefficiency -- the
    // very next ClearRenderTargetView on it is invalid (debug layer #538) and on a release runtime
    // that is undefined rather than reported. The pass borrows the texture for one CopyResource and
    // hands it back in exactly the state it was found in.
    ctx.textureBarrier(in.motionVectors, rhi::ResourceState::RenderTarget, rhi::ResourceState::CopySource);
    ctx.textureBarrier(mvScratch_, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::CopyDest);
    ctx.copyTexture(mvScratch_, in.motionVectors);
    ctx.textureBarrier(mvScratch_, rhi::ResourceState::CopyDest, rhi::ResourceState::UnorderedAccess);
    ctx.textureBarrier(in.motionVectors, rhi::ResourceState::CopySource, rhi::ResourceState::RenderTarget);

    if (!denoiser_.setFrameSettings(settings)) {
        AVER_WARN("[NRD] rejected this frame's settings");
        return false;
    }

    const Dispatch* plan = nullptr;
    u32 planCount = 0;
    if (!denoiser_.dispatches(denoiserIndices, indexCount, plan, planCount) || planCount == 0)
        return false;

    const InstanceLayout lay = denoiser_.layout();

    // CLEARED PER FRAME, because an output handle is a fact about THIS plan. A caller that ran both
    // denoisers last frame and only the occlusion one this frame would otherwise still be handed
    // last frame's radiance texture, and would read a filtered image nothing had refreshed.
    outDiffHitDist_    = 0;
    outDiffRadHitDist_ = 0;

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
                if (sb.role == SlotRole::OutDiffuseRadianceHitDistance) outDiffRadHitDist_ = t;
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
        // constantsUnchanged IS DELIBERATELY IGNORED, and ignoring it is not laziness -- honouring
        // it removes the device.
        //
        // NRD sets that flag when a dispatch's constant bytes are identical to the previous one's,
        // and skipping the upload looks like free bandwidth. It is not, because a D3D12 root
        // argument is not a property of the command list, it is a property of the CURRENTLY BOUND
        // ROOT SIGNATURE: SetPipelineState with a pipeline whose root signature differs INVALIDATES
        // every root argument, this root CBV included. Two consecutive dispatches with identical
        // constants but different pipelines therefore leave the second one reading an UNBOUND CBV,
        // which is undefined -- and on this hardware it is a GPU fault, surfacing as
        // DXGI_ERROR_DEVICE_REMOVED (0x887A0005) and taking the whole post chain down with it.
        //
        // MEASURED, not theorised: the occlusion denoiser alone (11 pipelines) never hit the case
        // and ran for days; adding REBLUR_DIFFUSE took the plan to 34 dispatches and the device died
        // on the first frame. This engine already has one recorded TDR whose cause was an unbound
        // root CBV -- that is the same failure, reached a different way.
        //
        // The correct optimisation, if this upload ever measures, is to track the last bound
        // pipeline and honour the flag only when it has not changed. It has not measured.
        if (dp.constants && dp.constantsSize)
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
