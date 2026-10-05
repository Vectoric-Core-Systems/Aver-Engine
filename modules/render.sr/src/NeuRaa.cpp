// NeuRAA. See NeuRaa.hpp and docs/rendering/NEURAA_NRD.md section 3.
#include "aver/sr/NeuRaa.hpp"
#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <initializer_list>
#include <string>

namespace aver::sr {

namespace {

struct NeuRaaCB {   // cbuffer AverNeuRaaCB in sr_neuraa.hlsl
    u32 vp[4];
    u32 info[4];
};
static_assert(sizeof(NeuRaaCB) == 32, "mirrors the HLSL cbuffer");

constexpr u32 kConstantRegister = 1;   // b1, as every AverSR pass

// Pass 0: t0 viewZ, t1 normal, t2 colour; u0 visibility, u1 edges, u2 debug, u3 tiles, u4 distances.
constexpr u32 kDetectSrv = 3, kDetectUav = 5;
void detectSlots(rhi::SlotKind* srv, rhi::SlotKind* uav) {
    for (u32 i = 0; i < kDetectSrv; ++i) srv[i] = rhi::SlotKind::Texture2D;
    uav[0] = rhi::SlotKind::StructuredBuffer;
    uav[1] = rhi::SlotKind::Texture2D;
    uav[2] = rhi::SlotKind::Texture2D;
    uav[3] = rhi::SlotKind::StructuredBuffer;
    uav[4] = rhi::SlotKind::Texture2D;
}
// Pass 1: t0 viewZ, t1 colour, t2 edges, t3 distances; u0 output.
constexpr u32 kResolveSrv = 4, kResolveUav = 1;
void resolveSlots(rhi::SlotKind* srv, rhi::SlotKind* uav) {
    for (u32 i = 0; i < kResolveSrv; ++i) srv[i] = rhi::SlotKind::Texture2D;
    uav[0] = rhi::SlotKind::Texture2D;
}

} // namespace

NeuRaa::~NeuRaa() {
    if (detect_) res_.destroyPipeline(detect_);
    if (resolve_) res_.destroyPipeline(resolve_);
    if (detectSet_) res_.destroyBindingSet(detectSet_);
    if (resolveSet_) res_.destroyBindingSet(resolveSet_);
    releaseTargets();
}

rhi::UpscalerNeeds NeuRaa::needs() const {
    const rhi::UpscalerNeeds own =
        rhi::UpscalerNeeds::Depth | rhi::UpscalerNeeds::Normal | rhi::UpscalerNeeds::PrimaryVisibility;
    return inner_ ? (inner_->needs() | own) : own;
}

bool NeuRaa::ensurePipelines() {
    if (detect_ && resolve_ && detectSet_ && resolveSet_) return true;
    if (failed_) return false;
    const std::string& src = rhi::shaderFile("sr_neuraa.hlsl");
    auto build = [&](const char* entry, const char* defines, u32 srvs, u32 uavs,
                     void (*slots)(rhi::SlotKind*, rhi::SlotKind*), rhi::BindingSetHandle& set) {
        rhi::ShaderDesc sd{};
        sd.source = src.c_str();
        sd.entry  = entry;
        sd.stage  = rhi::ShaderStage::Compute;
        sd.minShaderModel = 60;
        sd.defines = defines;
        const rhi::ShaderHandle cs = src.empty() ? 0 : res_.createShader(sd);
        rhi::PipelineHandle p = 0;
        if (cs) {
            rhi::ComputePipelineDesc pd{};
            pd.cs = cs;
            pd.layout.srvCount = srvs;
            pd.layout.uavCount = uavs;
            pd.layout.constantDwords[kConstantRegister] = 0;   // root CBV
            pd.layout.slotKindsDeclared = true;
            slots(pd.layout.srvKinds, pd.layout.uavKinds);
            p = res_.createComputePipeline(pd);
            res_.destroyShader(cs);
        }
        if (p && !set) {
            rhi::BindingSetDesc bd{};
            bd.srvCount = srvs;
            bd.uavCount = uavs;
            slots(bd.srvKinds, bd.uavKinds);
            set = res_.createBindingSet(bd);
        }
        return p;
    };
    if (!detect_)  detect_  = build("CSNeuRaaDetect", "AVER_NEURAA_PASS=0", kDetectSrv, kDetectUav, detectSlots, detectSet_);
    if (!resolve_) resolve_ = build("CSNeuRaaResolve", "AVER_NEURAA_PASS=1", kResolveSrv, kResolveUav, resolveSlots, resolveSet_);
    if (!detect_ || !resolve_ || !detectSet_ || !resolveSet_) {
        failed_ = true;
        AVER_WARN("[NeuRAA] sr_neuraa.hlsl would not build; NeuRAA is off");
        return false;
    }
    return true;
}

void NeuRaa::releaseTargets() {
    for (rhi::TextureHandle* t : {&edges_, &dist_, &debug_, &aa_}) {
        if (*t) res_.destroyTexture(*t);
        *t = 0;
    }
    if (tiles_) res_.destroyBuffer(tiles_);
    tiles_ = 0;
    w_ = h_ = tileCount_ = 0;
}

bool NeuRaa::ensureTargets(u32 w, u32 h) {
    if (edges_ && dist_ && debug_ && aa_ && tiles_ && w == w_ && h == h_) return true;
    releaseTargets();
    auto make = [&](rhi::Format f, rhi::ResourceState state, const char* name) {
        rhi::TextureDesc d{};
        d.width = w; d.height = h;
        d.bind = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
        d.format = f;
        d.initialState = state;
        d.debugName = name;
        return res_.createTexture(d);
    };
    edges_ = make(rhi::Format::R8Uint, rhi::ResourceState::NonPixelShaderResource, "NeuRAA edge codes");
    dist_  = make(rhi::Format::RGBA8Unorm, rhi::ResourceState::NonPixelShaderResource, "NeuRAA edge distances");
    // The wrapped upscaler samples these two in its pixel shader.
    debug_ = make(rhi::Format::RGBA16F, rhi::ResourceState::ShaderResource, "NeuRAA debug view");
    aa_    = make(rhi::Format::RGBA16F, rhi::ResourceState::ShaderResource, "NeuRAA resolved");
    tileCount_ = ((w + 7u) / 8u) * ((h + 7u) / 8u);
    rhi::BufferDesc bd{};
    bd.bytes = static_cast<u64>(tileCount_) * sizeof(u32);
    bd.allowUnorderedAccess = true;
    bd.debugName = "NeuRAA tile flags";
    tiles_ = res_.createBuffer(bd);
    if (!edges_ || !dist_ || !debug_ || !aa_ || !tiles_) { releaseTargets(); return false; }
    w_ = w; h_ = h;
    return true;
}

rhi::TextureHandle NeuRaa::run(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in) {
    const rhi::PrimaryVisibility& vis = in.visibility;
    if (in.generated || !in.canRetarget || !vis.buffer || !in.depth || !in.normalRoughness) return 0;
    if (vis.viewport[0] + vis.viewport[2] > in.srcWidth || vis.viewport[1] + vis.viewport[3] > in.srcHeight)
        return 0;
    if (!ensurePipelines() || !ensureTargets(in.srcWidth, in.srcHeight)) return 0;

    using RS = rhi::ResourceState;
    const bool resolve = enabled_ && !debugView_;
    const rhi::TextureHandle reads[3] = {in.depth, in.normalRoughness, in.color};
    for (rhi::TextureHandle t : reads) ctx.textureBarrier(t, RS::ShaderResource, RS::NonPixelShaderResource);

    NeuRaaCB cb{};
    for (u32 i = 0; i < 4; ++i) cb.vp[i] = vis.viewport[i];
    const u32 gx = (vis.viewport[2] + 7u) / 8u, gy = (vis.viewport[3] + 7u) / 8u;
    cb.info[0] = vis.rowPitch;
    cb.info[1] = debugView_ ? 1u : 0u;
    cb.info[2] = gx;

    res_.setSrv(detectSet_, 0, in.depth);
    res_.setSrv(detectSet_, 1, in.normalRoughness);
    res_.setSrv(detectSet_, 2, in.color);
    res_.setUavBuffer(detectSet_, 0, vis.buffer, 16, vis.elementCount, 0);
    res_.setUav(detectSet_, 1, edges_, 0);
    res_.setUav(detectSet_, 2, debug_, 0);
    res_.setUavBuffer(detectSet_, 3, tiles_, sizeof(u32), tileCount_, 0);
    res_.setUav(detectSet_, 4, dist_, 0);
    if (resolve) {
        res_.setSrv(resolveSet_, 0, in.depth);
        res_.setSrv(resolveSet_, 1, in.color);
        res_.setSrv(resolveSet_, 2, edges_);
        res_.setSrv(resolveSet_, 3, dist_);
        res_.setUav(resolveSet_, 0, aa_, 0);
    }

    for (rhi::TextureHandle t : {edges_, dist_}) ctx.textureBarrier(t, RS::NonPixelShaderResource, RS::UnorderedAccess);
    ctx.textureBarrier(debug_, RS::ShaderResource, RS::UnorderedAccess);
    {
        rhi::ScopedGpuStat stat(ctx, "NeuRAA detect");
        ctx.setPipeline(detect_);
        ctx.setBindingSet(detectSet_);
        ctx.setConstantBuffer(kConstantRegister, &cb, sizeof(cb));
        ctx.dispatch(gx, gy, 1);
    }
    ctx.textureBarrier(debug_, RS::UnorderedAccess, RS::ShaderResource);
    for (rhi::TextureHandle t : {edges_, dist_}) ctx.textureBarrier(t, RS::UnorderedAccess, RS::NonPixelShaderResource);

    if (resolve) {
        ctx.textureBarrier(aa_, RS::ShaderResource, RS::UnorderedAccess);
        rhi::ScopedGpuStat stat(ctx, "NeuRAA resolve");
        ctx.setPipeline(resolve_);
        ctx.setBindingSet(resolveSet_);
        ctx.setConstantBuffer(kConstantRegister, &cb, sizeof(cb));
        ctx.dispatch((in.srcWidth + 7u) / 8u, (in.srcHeight + 7u) / 8u, 1);
        ctx.textureBarrier(aa_, RS::UnorderedAccess, RS::ShaderResource);
    }

    for (rhi::TextureHandle t : reads) ctx.textureBarrier(t, RS::NonPixelShaderResource, RS::ShaderResource);
    return debugView_ ? debug_ : resolve ? aa_ : 0;
}

// Same contract as the wrapped upscaler: outTarget arrives bound, and is left bound.
void NeuRaa::execute(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in, rhi::TextureHandle outTarget) {
    rhi::UpscalerInput next = in;
    if (enabled_ || debugView_)
        if (const rhi::TextureHandle img = run(ctx, in)) next.color = img;
    rhi::IUpscaler* up = inner_ ? inner_ : static_cast<rhi::IUpscaler*>(&passthrough_);
    up->execute(ctx, next, outTarget);
}

} // namespace aver::sr
