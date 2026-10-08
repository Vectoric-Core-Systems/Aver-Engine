// TemporalUpscaler: TAAU + RCAS. See AverSrTaa.hpp.
#include "aver/sr/AverSrTaa.hpp"
#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <cmath>
#include <cstring>
#include <string>

namespace aver::sr {

namespace {

struct TaaCB {   // cbuffer AverSrTaaCB in sr_taa.hlsl
    f32 src[4], dst[4], jit[4];
};
struct RcasCB {  // cbuffer AverSrFsrCB in sr_fsr1.hlsl; RCAS reads only con0.x
    u32 con0[4], con1[4], con2[4], con3[4];
    f32 srcSize[4];
};
static_assert(sizeof(TaaCB) == 48 && sizeof(RcasCB) == 80, "mirrors the HLSL cbuffers");

constexpr u32 kConstantRegister = 3;   // b3, as every AverSR pass: Vulkan makes b1 push constants, never a CBV

u32 bitsOf(f32 v) { u32 u; std::memcpy(&u, &v, sizeof u); return u; }

} // namespace

TemporalUpscaler::~TemporalUpscaler() {
    if (resolve_) res_.destroyPipeline(resolve_);
    if (rcas_) res_.destroyPipeline(rcas_);
    if (resolveSet_) res_.destroyBindingSet(resolveSet_);
    if (rcasSet_) res_.destroyBindingSet(rcasSet_);
    releaseTargets();
}

bool TemporalUpscaler::ensurePipelines(rhi::Format outFormat) {
    if (resolve_ && rcas_ && rcasFormat_ == outFormat) return true;
    if (rcas_) { res_.destroyPipeline(rcas_); rcas_ = 0; }
    auto build = [&](const char* src, const char* vsEntry, const char* psEntry, const char* defines,
                     rhi::Format fmt) -> rhi::PipelineHandle {
        rhi::ShaderDesc vsd; vsd.source = src; vsd.entry = vsEntry; vsd.stage = rhi::ShaderStage::Vertex;
        vsd.minShaderModel = 60; vsd.defines = defines;
        rhi::ShaderDesc psd; psd.source = src; psd.entry = psEntry; psd.stage = rhi::ShaderStage::Pixel;
        psd.minShaderModel = 60; psd.defines = defines;
        const rhi::ShaderHandle vs = res_.createShader(vsd);
        const rhi::ShaderHandle ps = res_.createShader(psd);
        rhi::PipelineHandle p = 0;
        if (vs && ps) {
            rhi::GraphicsPipelineDesc d;
            d.vs = vs; d.ps = ps;
            d.layout.srvCount = (std::strcmp(psEntry, "AverSrTaaResolvePS") == 0) ? 4 : 1;
            d.layout.samplers[0] = rhi::SamplerDesc{rhi::Filter::Linear, rhi::AddressMode::Clamp};
            d.layout.samplerCount = 1;
            d.cull = rhi::CullMode::None;
            d.depthClip = false;
            d.renderTargetCount = 1;
            d.renderTargets[0] = fmt;
            d.sampleCount = 1;
            p = res_.createGraphicsPipeline(d);
        }
        if (vs) res_.destroyShader(vs);
        if (ps) res_.destroyShader(ps);
        return p;
    };
    if (!resolve_) resolve_ = build(taaShaderSource(), "AverSrTaaVS", "AverSrTaaResolvePS", nullptr, rhi::Format::RGBA16F);
    rcas_ = build(fsrShaderSource(), "AverSrFsrVS", "AverSrFsrRcasPS", "AVER_HLSL_2018", outFormat);
    rcasFormat_ = outFormat;
    if (!resolveSet_) {
        rhi::BindingSetDesc bd;
        bd.srvCount = 4;
        for (u32 s = 0; s < 4; ++s) bd.srvKinds[s] = rhi::SlotKind::Texture2D;
        resolveSet_ = res_.createBindingSet(bd);
    }
    if (!rcasSet_) {
        rhi::BindingSetDesc bd;
        bd.srvCount = 1;
        bd.srvKinds[0] = rhi::SlotKind::Texture2D;
        rcasSet_ = res_.createBindingSet(bd);
    }
    return resolve_ && rcas_ && resolveSet_ && rcasSet_;
}

void TemporalUpscaler::releaseTargets() {
    for (rhi::TextureHandle& t : history_) { if (t) res_.destroyTexture(t); t = 0; }
    if (scratch_) res_.destroyTexture(scratch_);
    scratch_ = 0;
    dstW_ = dstH_ = 0;
}

bool TemporalUpscaler::ensureTargets(u32 dstW, u32 dstH) {
    if (history_[0] && history_[1] && scratch_ && dstW == dstW_ && dstH == dstH_) return true;
    releaseTargets();
    auto make = [&](const char* name) {
        rhi::TextureDesc d{};
        d.width = dstW; d.height = dstH;
        d.format = rhi::Format::RGBA16F;
        d.bind = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::RenderTarget;
        d.initialState = rhi::ResourceState::ShaderResource;
        d.debugName = name;
        return res_.createTexture(d);
    };
    history_[0] = make("AverSR TAA history A");
    history_[1] = make("AverSR TAA history B");
    scratch_    = make("AverSR TAA generated");
    if (!history_[0] || !history_[1] || !scratch_) { releaseTargets(); return false; }
    dstW_ = dstW; dstH_ = dstH;
    resetPending_ = true;
    return true;
}

// Same contract as FsrUpscaler::execute: outTarget arrives bound, and is left bound.
void TemporalUpscaler::execute(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in, rhi::TextureHandle outTarget) {
    if (!in.color || !outTarget || !in.srcWidth || !in.srcHeight || !in.dstWidth || !in.dstHeight) return;
    // TAA accumulates only while the camera is still: a moving camera gets the spatial upscale, and
    // history restarts once it stops (blending reprojected history is what smears in motion).
    if (in.cameraMoving) {
        fallback_.execute(ctx, in, outTarget);
        resetPending_ = true;
        return;
    }
    rhi::TextureDesc outDesc{};
    const bool ready = in.canRetarget && in.motionVectors && in.depth && !failed_ &&
                       res_.textureInfo(outTarget, outDesc) && ensurePipelines(outDesc.format) &&
                       ensureTargets(in.dstWidth, in.dstHeight);
    if (!ready) {
        if (!fallbackLogged_) {
            AVER_INFO("[AverSR] temporal AA needs the G-buffer and a D3D12 device; using FSR 1 instead");
            fallbackLogged_ = true;
        }
        if (in.canRetarget && in.motionVectors && in.depth && !ready) failed_ = true;   // pipelines/targets
        fallback_.execute(ctx, in, outTarget);
        resetPending_ = true;
        return;
    }
    fallbackLogged_ = false;
    if (in.srcWidth != srcW_ || in.srcHeight != srcH_) { srcW_ = in.srcWidth; srcH_ = in.srcHeight; resetPending_ = true; }

    const u32 prev = cur_, next = 1u - cur_;
    const rhi::TextureHandle target = in.generated ? scratch_ : history_[next];

    TaaCB cb{};
    cb.src[0] = static_cast<f32>(in.srcWidth);  cb.src[1] = static_cast<f32>(in.srcHeight);
    cb.src[2] = 1.0f / cb.src[0];               cb.src[3] = 1.0f / cb.src[1];
    cb.dst[0] = static_cast<f32>(in.dstWidth);  cb.dst[1] = static_cast<f32>(in.dstHeight);
    cb.dst[2] = 1.0f / cb.dst[0];               cb.dst[3] = 1.0f / cb.dst[1];
    cb.jit[0] = in.jitterX; cb.jit[1] = in.jitterY;
    cb.jit[2] = resetPending_ ? 1.0f : 0.0f;
    cb.jit[3] = 1.0f;

    res_.setSrv(resolveSet_, 0, in.color);
    res_.setSrv(resolveSet_, 1, in.motionVectors);
    res_.setSrv(resolveSet_, 2, in.depth);
    res_.setSrv(resolveSet_, 3, history_[prev]);

    constexpr auto kRead = rhi::ResourceState::ShaderResource;
    constexpr auto kWrite = rhi::ResourceState::RenderTarget;
    ctx.textureBarrier(target, kRead, kWrite);
    ctx.setRenderTargets(&target, 1, 0);
    ctx.setViewport(0, 0, in.dstWidth, in.dstHeight);
    ctx.setScissor(0, 0, in.dstWidth, in.dstHeight);
    ctx.setPipeline(resolve_);
    ctx.setBindingSet(resolveSet_);
    ctx.setConstantBuffer(kConstantRegister, &cb, sizeof(cb));
    ctx.drawFullscreen();
    ctx.textureBarrier(target, kWrite, kRead);
    if (!in.generated) { cur_ = next; resetPending_ = false; }

    // RCAS (FsrRcasCon: con.x = 2^-sharpness) un-squashes into outTarget.
    RcasCB rc{};
    rc.con0[0] = bitsOf(std::exp2(-sharpness_));
    res_.setSrv(rcasSet_, 0, target);
    ctx.setRenderTargets(&outTarget, 1, 0);
    ctx.setViewport(0, 0, in.dstWidth, in.dstHeight);
    ctx.setScissor(0, 0, in.dstWidth, in.dstHeight);
    ctx.setPipeline(rcas_);
    ctx.setBindingSet(rcasSet_);
    ctx.setConstantBuffer(kConstantRegister, &rc, sizeof(rc));
    ctx.drawFullscreen();
}

const char* taaShaderSource() {
    static std::string s;
    static u64 built = ~0ull;
    if (built != rhi::shaderFileRevision()) {
        s = rhi::shaderFile("sr_taa.hlsl");
        built = rhi::shaderFileRevision();
    }
    return s.c_str();
}

} // namespace aver::sr
