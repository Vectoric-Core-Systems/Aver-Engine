// FsrUpscaler: AMD FSR 1 (EASU + RCAS) on the rhi::IUpscaler seam. See AverSrFsr.hpp.
#include "aver/sr/AverSrFsr.hpp"
#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"
#include "aver/sr/SrConfine.hpp"

#include <cmath>
#include <cstring>
#include <string>

namespace aver::sr {

namespace {

// Matches `cbuffer AverSrFsrCB` in sr_fsr1.hlsl.
struct FsrCB {
    u32 con0[4], con1[4], con2[4], con3[4];
    f32 srcSize[4];
};
static_assert(sizeof(FsrCB) == 80, "AverSrFsrCB is five 16-byte rows");

constexpr u32 kFsrConstantRegister = 3;   // b3, as every AverSR pass: Vulkan makes b1 push constants, never a CBV

u32 bitsOf(f32 v) { u32 u; std::memcpy(&u, &v, sizeof u); return u; }

// ffx_fsr1.h's FsrEasuCon, for an input that fills its whole texture.
void easuConstants(FsrCB& cb, f32 inW, f32 inH, f32 outW, f32 outH) {
    cb.con0[0] = bitsOf(inW / outW);
    cb.con0[1] = bitsOf(inH / outH);
    cb.con0[2] = bitsOf(0.5f * inW / outW - 0.5f);
    cb.con0[3] = bitsOf(0.5f * inH / outH - 0.5f);
    cb.con1[0] = bitsOf(1.0f / inW);
    cb.con1[1] = bitsOf(1.0f / inH);
    cb.con1[2] = bitsOf( 1.0f / inW);
    cb.con1[3] = bitsOf(-1.0f / inH);
    cb.con2[0] = bitsOf(-1.0f / inW);
    cb.con2[1] = bitsOf( 2.0f / inH);
    cb.con2[2] = bitsOf( 1.0f / inW);
    cb.con2[3] = bitsOf( 2.0f / inH);
    cb.con3[0] = bitsOf(0.0f);
    cb.con3[1] = bitsOf( 4.0f / inH);
    cb.con3[2] = cb.con3[3] = 0;
}

} // namespace

FsrUpscaler::~FsrUpscaler() {
    for (rhi::PipelineHandle p : {prep_[0], prep_[1], easu_, rcas_}) if (p) res_.destroyPipeline(p);
    for (rhi::BindingSetHandle b : {prepSet_, easuSet_, rcasSet_}) if (b) res_.destroyBindingSet(b);
    releaseTargets();
}

bool FsrUpscaler::ensurePipelines(rhi::Format outFormat) {
    if (prep_[0] && rcas_ && rcasFormat_ == outFormat) return true;
    if (rcas_) { res_.destroyPipeline(rcas_); rcas_ = 0; }

    const char* src = fsrShaderSource();
    auto build = [&](const char* entry, const char* defines, rhi::Format fmt) -> rhi::PipelineHandle {
        rhi::ShaderDesc vsd; vsd.source = src; vsd.entry = "AverSrFsrVS"; vsd.stage = rhi::ShaderStage::Vertex;
        vsd.minShaderModel = 60; vsd.defines = "AVER_HLSL_2018";
        rhi::ShaderDesc psd; psd.source = src; psd.entry = entry; psd.stage = rhi::ShaderStage::Pixel;
        psd.minShaderModel = 60; psd.defines = defines;
        const rhi::ShaderHandle vs = res_.createShader(vsd);
        const rhi::ShaderHandle ps = res_.createShader(psd);
        rhi::PipelineHandle p = 0;
        if (vs && ps) {
            rhi::GraphicsPipelineDesc d;
            d.vs = vs; d.ps = ps;
            d.layout.srvCount = 1;
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
    if (!prep_[0]) prep_[0] = build("AverSrFsrPrepPS", "AVER_HLSL_2018", rhi::Format::RGBA16F);
    if (!prep_[1]) prep_[1] = build("AverSrFsrPrepPS", "AVER_HLSL_2018;AVER_FSR_EDGE_AA=1", rhi::Format::RGBA16F);
    if (!easu_)    easu_    = build("AverSrFsrEasuPS", "AVER_HLSL_2018", rhi::Format::RGBA16F);
    rcas_ = build("AverSrFsrRcasPS", "AVER_HLSL_2018", outFormat);
    rcasFormat_ = outFormat;

    auto set = [&](rhi::BindingSetHandle& s) {
        if (s) return;
        rhi::BindingSetDesc bd;
        bd.srvCount = 1;
        bd.srvKinds[0] = rhi::SlotKind::Texture2D;
        s = res_.createBindingSet(bd);
    };
    set(prepSet_); set(easuSet_); set(rcasSet_);
    return prep_[0] && prep_[1] && easu_ && rcas_ && prepSet_ && easuSet_ && rcasSet_;
}

void FsrUpscaler::releaseTargets() {
    if (squashed_) res_.destroyTexture(squashed_);
    if (upscaled_) res_.destroyTexture(upscaled_);
    squashed_ = upscaled_ = 0;
    easuBound_ = rcasBound_ = 0;
    srcW_ = srcH_ = dstW_ = dstH_ = 0;
}

bool FsrUpscaler::ensureTargets(u32 srcW, u32 srcH, u32 dstW, u32 dstH) {
    if (squashed_ && upscaled_ && srcW == srcW_ && srcH == srcH_ && dstW == dstW_ && dstH == dstH_) return true;
    releaseTargets();
    auto make = [&](u32 w, u32 h, const char* name) {
        rhi::TextureDesc d{};
        d.width = w; d.height = h;
        d.format = rhi::Format::RGBA16F;
        d.bind = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::RenderTarget;
        d.initialState = rhi::ResourceState::ShaderResource;
        d.debugName = name;
        return res_.createTexture(d);
    };
    squashed_ = make(srcW, srcH, "AverSR FSR source");
    upscaled_ = make(dstW, dstH, "AverSR FSR EASU");
    if (!squashed_ || !upscaled_) { releaseTargets(); return false; }
    srcW_ = srcW; srcH_ = srcH; dstW_ = dstW; dstH_ = dstH;
    return true;
}

void FsrUpscaler::draw(rhi::IRenderContext& ctx, rhi::PipelineHandle p, rhi::BindingSetHandle set,
                       rhi::TextureHandle src, rhi::TextureHandle& bound, const void* cb, u32 w, u32 h,
                       const PxRect* sc) {
    if (src != bound) { res_.setSrv(set, 0, src); bound = src; }
    ctx.setViewport(0, 0, w, h);
    if (sc) ctx.setScissor(sc->x0, sc->y0, sc->x1 - sc->x0, sc->y1 - sc->y0);
    else    ctx.setScissor(0, 0, w, h);
    ctx.setPipeline(p);
    ctx.setBindingSet(set);
    ctx.setConstantBuffer(kFsrConstantRegister, cb, sizeof(FsrCB));
    ctx.drawFullscreen();
}

// The caller has outTarget bound as the render target with a destination-size viewport, and
// transitions nothing; this binds its own two targets in between and leaves outTarget bound again.
void FsrUpscaler::execute(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in, rhi::TextureHandle outTarget) {
    if (!in.color || !outTarget || !in.srcWidth || !in.srcHeight || !in.dstWidth || !in.dstHeight) return;
    if (!in.canRetarget) { fallback_.execute(ctx, in, outTarget); return; }   // single pass only
    rhi::TextureDesc outDesc{};
    if (failed_ || !res_.textureInfo(outTarget, outDesc) || !ensurePipelines(outDesc.format) ||
        !ensureTargets(in.srcWidth, in.srcHeight, in.dstWidth, in.dstHeight)) {
        if (!failed_) AVER_WARN("[AverSR] FSR 1 is unavailable; falling back to the Catmull-Rom upscale");
        failed_ = true;
        fallback_.execute(ctx, in, outTarget);
        return;
    }

    FsrCB cb{};
    cb.srcSize[0] = static_cast<f32>(in.srcWidth);
    cb.srcSize[1] = static_cast<f32>(in.srcHeight);
    cb.srcSize[2] = 1.0f / cb.srcSize[0];
    cb.srcSize[3] = 1.0f / cb.srcSize[1];
    constexpr auto kRead = rhi::ResourceState::ShaderResource;
    constexpr auto kWrite = rhi::ResourceState::RenderTarget;

    // Only the displayed rect: RCAS writes it, EASU its 1-texel ring, prep what EASU reads.
    PxRect rcasR, easuR, prepR;
    const bool confine = displayedDst(in, rcasR);
    if (confine) {
        easuR = growRect(rcasR, 1, in.dstWidth, in.dstHeight);
        prepR = srcReadOf(easuR, in, kEasuReach);
    }

    // 1. Edge AA (optional) + squash, at source size.
    ctx.textureBarrier(squashed_, kRead, kWrite);
    ctx.setRenderTargets(&squashed_, 1, 0);
    draw(ctx, prep_[edgeAa_ ? 1 : 0], prepSet_, in.color, prepBound_, &cb, in.srcWidth, in.srcHeight, confine ? &prepR : nullptr);
    ctx.textureBarrier(squashed_, kWrite, kRead);

    // 2. EASU to output size.
    easuConstants(cb, static_cast<f32>(in.srcWidth), static_cast<f32>(in.srcHeight),
                  static_cast<f32>(in.dstWidth), static_cast<f32>(in.dstHeight));
    ctx.textureBarrier(upscaled_, kRead, kWrite);
    ctx.setRenderTargets(&upscaled_, 1, 0);
    draw(ctx, easu_, easuSet_, squashed_, easuBound_, &cb, in.dstWidth, in.dstHeight, confine ? &easuR : nullptr);
    ctx.textureBarrier(upscaled_, kWrite, kRead);

    // 3. RCAS (ffx_fsr1.h's FsrRcasCon: con.x = 2^-sharpness) and un-squash, into outTarget.
    cb.con0[0] = bitsOf(std::exp2(-sharpness_));
    cb.con0[1] = cb.con0[2] = cb.con0[3] = 0;
    ctx.setRenderTargets(&outTarget, 1, 0);
    draw(ctx, rcas_, rcasSet_, upscaled_, rcasBound_, &cb, in.dstWidth, in.dstHeight, confine ? &rcasR : nullptr);
    if (confine) ctx.setScissor(0, 0, in.dstWidth, in.dstHeight);
}

const char* fsrShaderSource() {
    // Keyed on shaderFileRevision() so hot reload rebuilds it (see spatialUpscaleShaderSource()).
    static std::string s;
    static u64 built = ~0ull;
    if (built != rhi::shaderFileRevision()) {
        s = rhi::shaderFile("sr_fsr1.hlsl");
        built = rhi::shaderFileRevision();
    }
    return s.c_str();
}

} // namespace aver::sr
