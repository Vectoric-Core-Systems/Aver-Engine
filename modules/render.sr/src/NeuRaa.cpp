// NeuRAA. See NeuRaa.hpp and docs/rendering/NEURAA_NRD.md section 3.
#include "aver/sr/NeuRaa.hpp"
#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <string>

namespace aver::sr {

namespace {

struct NeuRaaCB {   // cbuffer AverNeuRaaCB in sr_neuraa.hlsl
    u32 vp[4];
    u32 info[4];
};
static_assert(sizeof(NeuRaaCB) == 32, "mirrors the HLSL cbuffer");

constexpr u32 kConstantRegister = 1;   // b1, as every AverSR pass
constexpr u32 kSrvCount = 3, kUavCount = 4;

void declareSlots(rhi::SlotKind* srv, rhi::SlotKind* uav) {
    for (u32 i = 0; i < kSrvCount; ++i) srv[i] = rhi::SlotKind::Texture2D;
    uav[0] = rhi::SlotKind::StructuredBuffer;   // visibility
    uav[1] = rhi::SlotKind::Texture2D;          // edge codes
    uav[2] = rhi::SlotKind::Texture2D;          // debug image
    uav[3] = rhi::SlotKind::StructuredBuffer;   // tile flags
}

} // namespace

NeuRaa::~NeuRaa() {
    if (detect_) res_.destroyPipeline(detect_);
    if (set_) res_.destroyBindingSet(set_);
    releaseTargets();
}

rhi::UpscalerNeeds NeuRaa::needs() const {
    const rhi::UpscalerNeeds own =
        rhi::UpscalerNeeds::Depth | rhi::UpscalerNeeds::Normal | rhi::UpscalerNeeds::PrimaryVisibility;
    return inner_ ? (inner_->needs() | own) : own;
}

bool NeuRaa::ensurePipeline() {
    if (detect_ && set_) return true;
    if (failed_) return false;
    const std::string& src = rhi::shaderFile("sr_neuraa.hlsl");
    rhi::ShaderDesc sd{};
    sd.source = src.c_str();
    sd.entry  = "CSNeuRaaDetect";
    sd.stage  = rhi::ShaderStage::Compute;
    sd.minShaderModel = 60;
    const rhi::ShaderHandle cs = src.empty() ? 0 : res_.createShader(sd);
    if (cs) {
        rhi::ComputePipelineDesc pd{};
        pd.cs = cs;
        pd.layout.srvCount = kSrvCount;
        pd.layout.uavCount = kUavCount;
        pd.layout.constantDwords[kConstantRegister] = 0;   // root CBV
        pd.layout.slotKindsDeclared = true;
        declareSlots(pd.layout.srvKinds, pd.layout.uavKinds);
        detect_ = res_.createComputePipeline(pd);
        res_.destroyShader(cs);
    }
    if (detect_ && !set_) {
        rhi::BindingSetDesc bd{};
        bd.srvCount = kSrvCount;
        bd.uavCount = kUavCount;
        declareSlots(bd.srvKinds, bd.uavKinds);
        set_ = res_.createBindingSet(bd);
    }
    if (!detect_ || !set_) {
        failed_ = true;
        AVER_WARN("[NeuRAA] sr_neuraa.hlsl would not build; edge detection is off");
        return false;
    }
    return true;
}

void NeuRaa::releaseTargets() {
    if (edges_) res_.destroyTexture(edges_);
    if (debug_) res_.destroyTexture(debug_);
    if (tiles_) res_.destroyBuffer(tiles_);
    edges_ = debug_ = 0;
    tiles_ = 0;
    w_ = h_ = tileCount_ = 0;
}

bool NeuRaa::ensureTargets(u32 w, u32 h) {
    if (edges_ && debug_ && tiles_ && w == w_ && h == h_) return true;
    releaseTargets();
    rhi::TextureDesc d{};
    d.width = w; d.height = h;
    d.bind = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
    d.format = rhi::Format::R8Uint;
    d.initialState = rhi::ResourceState::NonPixelShaderResource;
    d.debugName = "NeuRAA edge codes";
    edges_ = res_.createTexture(d);
    d.format = rhi::Format::RGBA16F;
    d.initialState = rhi::ResourceState::ShaderResource;   // the wrapped upscaler samples it
    d.debugName = "NeuRAA debug view";
    debug_ = res_.createTexture(d);
    tileCount_ = ((w + 7u) / 8u) * ((h + 7u) / 8u);
    rhi::BufferDesc bd{};
    bd.bytes = static_cast<u64>(tileCount_) * sizeof(u32);
    bd.allowUnorderedAccess = true;
    bd.debugName = "NeuRAA tile flags";
    tiles_ = res_.createBuffer(bd);
    if (!edges_ || !debug_ || !tiles_) { releaseTargets(); return false; }
    w_ = w; h_ = h;
    return true;
}

bool NeuRaa::detect(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in) {
    const rhi::PrimaryVisibility& vis = in.visibility;
    if (in.generated || !in.canRetarget || !vis.buffer || !in.depth || !in.normalRoughness) return false;
    if (vis.viewport[0] + vis.viewport[2] > in.srcWidth || vis.viewport[1] + vis.viewport[3] > in.srcHeight)
        return false;
    if (!ensurePipeline() || !ensureTargets(in.srcWidth, in.srcHeight)) return false;

    using RS = rhi::ResourceState;
    const rhi::TextureHandle reads[3] = {in.depth, in.normalRoughness, in.color};
    for (rhi::TextureHandle t : reads) ctx.textureBarrier(t, RS::ShaderResource, RS::NonPixelShaderResource);
    ctx.textureBarrier(edges_, RS::NonPixelShaderResource, RS::UnorderedAccess);
    ctx.textureBarrier(debug_, RS::ShaderResource, RS::UnorderedAccess);

    res_.setSrv(set_, 0, in.depth);
    res_.setSrv(set_, 1, in.normalRoughness);
    res_.setSrv(set_, 2, in.color);
    res_.setUavBuffer(set_, 0, vis.buffer, 16, vis.elementCount, 0);
    res_.setUav(set_, 1, edges_, 0);
    res_.setUav(set_, 2, debug_, 0);
    res_.setUavBuffer(set_, 3, tiles_, sizeof(u32), tileCount_, 0);

    NeuRaaCB cb{};
    for (u32 i = 0; i < 4; ++i) cb.vp[i] = vis.viewport[i];
    const u32 gx = (vis.viewport[2] + 7u) / 8u, gy = (vis.viewport[3] + 7u) / 8u;
    cb.info[0] = vis.rowPitch;
    cb.info[1] = debugView_ ? 1u : 0u;
    cb.info[2] = gx;
    {
        rhi::ScopedGpuStat stat(ctx, "NeuRAA detect");
        ctx.setPipeline(detect_);
        ctx.setBindingSet(set_);
        ctx.setConstantBuffer(kConstantRegister, &cb, sizeof(cb));
        ctx.dispatch(gx, gy, 1);
    }

    ctx.textureBarrier(debug_, RS::UnorderedAccess, RS::ShaderResource);
    ctx.textureBarrier(edges_, RS::UnorderedAccess, RS::NonPixelShaderResource);
    for (rhi::TextureHandle t : reads) ctx.textureBarrier(t, RS::NonPixelShaderResource, RS::ShaderResource);
    return true;
}

// Same contract as the wrapped upscaler: outTarget arrives bound, and is left bound.
void NeuRaa::execute(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in, rhi::TextureHandle outTarget) {
    rhi::UpscalerInput next = in;
    // Phase 1 detects only for the debug view, whose image then stands in for the scene.
    if (debugView_ && detect(ctx, in)) next.color = debug_;
    rhi::IUpscaler* up = inner_ ? inner_ : static_cast<rhi::IUpscaler*>(&passthrough_);
    up->execute(ctx, next, outTarget);
}

} // namespace aver::sr
