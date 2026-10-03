#include "aver/framegen/ProceduralFrameGenerator.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <string>

namespace aver::framegen {

namespace {

constexpr const char* kShaderName = "framegen.hlsl";
constexpr u32 kGroup = 8;   // framegen.hlsl's FG_GROUP
constexpr u32 kConstantSlot = 1;   // b1: FgConstants
constexpr u32 kConstantDwords = 4;

struct FgConstants { u32 size[2]; u32 last; u32 pad; };
static_assert(sizeof(FgConstants) == kConstantDwords * 4, "FgConstants mirrors framegen.hlsl's cbuffer");

}  // namespace

ProceduralFrameGenerator::~ProceduralFrameGenerator() {
    releaseTargets();
    if (gatherPso_) res_.destroyPipeline(gatherPso_);
    if (fillPso_) res_.destroyPipeline(fillPso_);
}

bool ProceduralFrameGenerator::ensurePipelines() {
    if (gatherPso_ && fillPso_) return true;
    if (pipelinesFailed_) return false;
    auto fail = [&](const char* why) {
        AVER_WARN("[FrameGen] {}; frame generation stays off", why);
        pipelinesFailed_ = true;
        return false;
    };
    const std::string& source = rhi::shaderFile(kShaderName);
    if (source.empty()) return fail("framegen.hlsl is not deployed beside the executable");

    auto build = [&](const char* entry, const char* define, u32 srvs, bool sampler) -> rhi::PipelineHandle {
        rhi::ShaderDesc sd{};
        sd.source = source.c_str();
        sd.entry = entry;
        sd.stage = rhi::ShaderStage::Compute;
        sd.minShaderModel = 60;
        sd.defines = define;
        const rhi::ShaderHandle cs = res_.createShader(sd);
        if (!cs) return 0;
        rhi::ComputePipelineDesc pd{};
        pd.cs = cs;
        pd.layout.srvCount = srvs;
        pd.layout.uavCount = 1;
        pd.layout.slotKindsDeclared = true;   // every slot is a Texture2D (the default kind)
        pd.layout.constantDwords[kConstantSlot] = kConstantDwords;
        if (sampler) {
            pd.layout.samplers[0] = rhi::SamplerDesc{rhi::Filter::Linear, rhi::AddressMode::Clamp};
            pd.layout.samplerCount = 1;
        }
        const rhi::PipelineHandle p = res_.createComputePipeline(pd);
        res_.destroyShader(cs);
        return p;
    };
    gatherPso_ = build("CSFgGather", "FG_GATHER=1", 6, true);
    fillPso_   = build("CSFgFill", "FG_FILL=1", 1, false);
    if (!gatherPso_ || !fillPso_) return fail("the frame generation shaders would not compile");
    AVER_INFO("[FrameGen] procedural interpolation ready (gather + 2 full-resolution fill passes)");
    return true;
}

void ProceduralFrameGenerator::releaseTargets() {
    for (rhi::BindingSetHandle* s : {&gatherSet_, &fillSetA_, &fillSetB_}) {
        if (*s) res_.destroyBindingSet(*s);
        *s = 0;
    }
    for (rhi::TextureHandle* t : {&histColor_, &histVel_, &histZ_, &out_, &tmp_}) {
        if (*t) res_.destroyTexture(*t);
        *t = 0;
    }
    boundColor_ = boundVel_ = boundZ_ = 0;
    w_ = h_ = 0;
    historyValid_ = false;
}

bool ProceduralFrameGenerator::ensureTargets(u32 w, u32 h) {
    if (w == w_ && h == h_ && out_) return true;
    releaseTargets();

    auto make = [&](rhi::Format f, bool uav, rhi::ResourceState state, const char* name) {
        rhi::TextureDesc td{};
        td.width = w;
        td.height = h;
        td.format = f;
        td.bind = uav ? (rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess)
                      : rhi::ResourceBind::ShaderResource;
        td.initialState = state;
        td.debugName = name;
        return res_.createTexture(td);
    };
    using RS = rhi::ResourceState;
    histColor_ = make(rhi::Format::RGBA16F, false, RS::NonPixelShaderResource, "FrameGen previous colour");
    histVel_   = make(rhi::Format::RG16F, false, RS::NonPixelShaderResource, "FrameGen previous motion");
    histZ_     = make(rhi::Format::R32Float, false, RS::NonPixelShaderResource, "FrameGen previous view depth");
    out_       = make(rhi::Format::RGBA16F, true, RS::ShaderResource, "FrameGen generated frame");
    tmp_       = make(rhi::Format::RGBA16F, true, RS::ShaderResource, "FrameGen fill scratch");
    if (!histColor_ || !histVel_ || !histZ_ || !out_ || !tmp_) {
        AVER_WARN("[FrameGen] could not create the {}x{} targets", w, h);
        releaseTargets();
        return false;
    }

    rhi::BindingSetDesc gd;
    gd.srvCount = 6;
    gd.uavCount = 1;
    gatherSet_ = res_.createBindingSet(gd);
    rhi::BindingSetDesc fd;
    fd.srvCount = 1;
    fd.uavCount = 1;
    fillSetA_ = res_.createBindingSet(fd);
    fillSetB_ = res_.createBindingSet(fd);
    if (!gatherSet_ || !fillSetA_ || !fillSetB_) {
        AVER_WARN("[FrameGen] could not create the binding sets");
        releaseTargets();
        return false;
    }
    res_.setSrv(gatherSet_, 3, histColor_);
    res_.setSrv(gatherSet_, 4, histVel_);
    res_.setSrv(gatherSet_, 5, histZ_);
    res_.setUav(gatherSet_, 0, out_, 0);
    res_.setSrv(fillSetA_, 0, out_);
    res_.setUav(fillSetA_, 0, tmp_, 0);
    res_.setSrv(fillSetB_, 0, tmp_);
    res_.setUav(fillSetB_, 0, out_, 0);

    w_ = w;
    h_ = h;
    AVER_INFO("[FrameGen] targets {}x{} ({:.1f} MiB)", w, h,
              static_cast<f64>(w) * h * (8 + 4 + 4 + 8 + 8) / (1024.0 * 1024.0));
    return true;
}

// Frame N's three inputs keep their handles from frame to frame (the device's own textures), so the set
// is rewritten only when one changes -- a resize, after which the device has idled the GPU.
void ProceduralFrameGenerator::bindInputs(const rhi::FrameGenInput& in) {
    if (in.color != boundColor_) { res_.setSrv(gatherSet_, 0, in.color); boundColor_ = in.color; }
    if (in.velocity != boundVel_) { res_.setSrv(gatherSet_, 1, in.velocity); boundVel_ = in.velocity; }
    if (in.viewZ != boundZ_) { res_.setSrv(gatherSet_, 2, in.viewZ); boundZ_ = in.viewZ; }
}

rhi::TextureHandle ProceduralFrameGenerator::generate(rhi::IRenderContext& ctx, const rhi::FrameGenInput& in) {
    if (!in.color || !in.velocity || !in.viewZ || !in.width || !in.height) return 0;
    if (!ensurePipelines()) return 0;
    if (!ensureTargets(in.width, in.height)) return 0;
    bindInputs(in);

    using RS = rhi::ResourceState;
    const u32 gx = (in.width + kGroup - 1) / kGroup;
    const u32 gy = (in.height + kGroup - 1) / kGroup;
    FgConstants k{{in.width, in.height}, 0, 0};

    // Frame N becomes compute-readable for the gather (a pixel-shader read state does not cover compute).
    ctx.textureBarrier(in.color, RS::ShaderResource, RS::NonPixelShaderResource);
    ctx.textureBarrier(in.velocity, RS::RenderTarget, RS::NonPixelShaderResource);
    ctx.textureBarrier(in.viewZ, RS::RenderTarget, RS::NonPixelShaderResource);

    rhi::TextureHandle result = 0;
    if (historyValid_ && !in.sceneCut) {
        rhi::ScopedGpuStat stat(ctx, "Frame generation");
        ctx.textureBarrier(out_, RS::ShaderResource, RS::UnorderedAccess);
        ctx.setPipeline(gatherPso_);
        ctx.setBindingSet(gatherSet_);
        ctx.setConstants(kConstantSlot, &k, kConstantDwords);
        ctx.dispatch(gx, gy, 1);

        // Fill, twice: out_ -> tmp_ -> out_.
        ctx.textureBarrier(out_, RS::UnorderedAccess, RS::NonPixelShaderResource);
        ctx.textureBarrier(tmp_, RS::ShaderResource, RS::UnorderedAccess);
        ctx.setPipeline(fillPso_);
        ctx.setBindingSet(fillSetA_);
        ctx.setConstants(kConstantSlot, &k, kConstantDwords);
        ctx.dispatch(gx, gy, 1);

        ctx.textureBarrier(tmp_, RS::UnorderedAccess, RS::NonPixelShaderResource);
        ctx.textureBarrier(out_, RS::NonPixelShaderResource, RS::UnorderedAccess);
        k.last = 1;
        ctx.setBindingSet(fillSetB_);
        ctx.setConstants(kConstantSlot, &k, kConstantDwords);
        ctx.dispatch(gx, gy, 1);

        ctx.textureBarrier(out_, RS::UnorderedAccess, RS::ShaderResource);
        ctx.textureBarrier(tmp_, RS::NonPixelShaderResource, RS::ShaderResource);
        result = out_;
    }

    // Frame N becomes the previous frame.
    ctx.textureBarrier(in.color, RS::NonPixelShaderResource, RS::CopySource);
    ctx.textureBarrier(in.velocity, RS::NonPixelShaderResource, RS::CopySource);
    ctx.textureBarrier(in.viewZ, RS::NonPixelShaderResource, RS::CopySource);
    ctx.textureBarrier(histColor_, RS::NonPixelShaderResource, RS::CopyDest);
    ctx.textureBarrier(histVel_, RS::NonPixelShaderResource, RS::CopyDest);
    ctx.textureBarrier(histZ_, RS::NonPixelShaderResource, RS::CopyDest);
    ctx.copyTexture(histColor_, in.color);
    ctx.copyTexture(histVel_, in.velocity);
    ctx.copyTexture(histZ_, in.viewZ);
    ctx.textureBarrier(histColor_, RS::CopyDest, RS::NonPixelShaderResource);
    ctx.textureBarrier(histVel_, RS::CopyDest, RS::NonPixelShaderResource);
    ctx.textureBarrier(histZ_, RS::CopyDest, RS::NonPixelShaderResource);
    ctx.textureBarrier(in.color, RS::CopySource, RS::ShaderResource);
    ctx.textureBarrier(in.velocity, RS::CopySource, RS::RenderTarget);
    ctx.textureBarrier(in.viewZ, RS::CopySource, RS::RenderTarget);
    historyValid_ = true;
    return result;
}

}  // namespace aver::framegen
