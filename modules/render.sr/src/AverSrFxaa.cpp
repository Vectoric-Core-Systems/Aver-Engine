// FxaaResolve: AverSR's edge-detecting AA implementation of the rhi::IUpscaler seam (see
// AverSrFxaa.hpp for what it is, what it is not, and why it runs where it runs).
#include "aver/sr/AverSrFxaa.hpp"
#include "aver/core/Log.hpp"

#include <string>
#include "aver/rhi/ShaderFiles.hpp"   // this pass's HLSL is a deployed file

namespace aver::sr {

// Mirrors `cbuffer AverSrFxaaCB` in the HLSL below exactly, same shape as SpatialUpscalerCB.
struct FxaaCB {
    f32 srcSize[4];   // xy source size in texels, zw its reciprocal
};

// Own root CBV register, same reasoning as AverSrSpatial.cpp's kUpscaleConstantRegister: this
// shader never includes rhi::sharedShaderPrelude(), so b1 here is unrelated to what b1 means to a
// pipeline that does.
constexpr u32 kFxaaConstantRegister = 1;

FxaaResolve::~FxaaResolve() {
    if (pipeline_) res_.destroyPipeline(pipeline_);
    if (binding_)  res_.destroyBindingSet(binding_);
}

bool FxaaResolve::ensurePipeline(rhi::TextureHandle outTarget) {
    rhi::TextureDesc dstDesc{};
    if (!res_.textureInfo(outTarget, dstDesc)) {
        AVER_ERROR("[AverSR] FxaaResolve: outTarget handle {} has no texture info", outTarget);
        return false;
    }

    if (pipeline_ && dstDesc.format == pipelineFormat_) return true;

    if (pipeline_) { res_.destroyPipeline(pipeline_); pipeline_ = 0; }

    const char* src = fxaaResolveShaderSource();
    rhi::ShaderDesc vsd; vsd.source = src; vsd.entry = "AverSrFxaaVS";   vsd.stage = rhi::ShaderStage::Vertex; vsd.minShaderModel = 51;
    rhi::ShaderDesc psd; psd.source = src; psd.entry = "AverSrFxaaMain"; psd.stage = rhi::ShaderStage::Pixel;  psd.minShaderModel = 51;
    const rhi::ShaderHandle vs = res_.createShader(vsd);
    const rhi::ShaderHandle ps = res_.createShader(psd);

    bool ok = false;
    if (vs && ps) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vs;
        p.ps = ps;
        p.layout.srvCount = 1;
        p.layout.samplers[0] = rhi::SamplerDesc{rhi::Filter::Linear, rhi::AddressMode::Clamp};
        p.layout.samplerCount = 1;
        p.cull = rhi::CullMode::None;
        p.depthClip = false;
        p.renderTargetCount = 1;
        p.renderTargets[0] = dstDesc.format;
        p.sampleCount = 1;   // outTarget is always single-sample -- the post chain resolves MSAA first
        pipeline_ = res_.createGraphicsPipeline(p);
        ok = pipeline_ != 0;
    }
    if (vs) res_.destroyShader(vs);
    if (ps) res_.destroyShader(ps);

    if (!ok) {
        AVER_ERROR("[AverSR] FxaaResolve: pipeline unavailable");
        return false;
    }
    pipelineFormat_ = dstDesc.format;

    if (!binding_) {
        rhi::BindingSetDesc bd;
        bd.srvCount = 1;
        bd.srvKinds[0] = rhi::SlotKind::Texture2D;
        binding_ = res_.createBindingSet(bd);
        boundColor_ = 0;
        if (!binding_) {
            AVER_ERROR("[AverSR] FxaaResolve: binding set unavailable");
            res_.destroyPipeline(pipeline_);
            pipeline_ = 0;
            return false;
        }
    }
    return true;
}

void FxaaResolve::execute(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in, rhi::TextureHandle outTarget) {
    if (!in.color || !outTarget || !in.srcWidth || !in.srcHeight) return;
    if (!ensurePipeline(outTarget)) return;

    if (in.color != boundColor_) {
        res_.setSrv(binding_, 0, in.color);
        boundColor_ = in.color;
    }

    FxaaCB cb{};
    cb.srcSize[0] = static_cast<f32>(in.srcWidth);
    cb.srcSize[1] = static_cast<f32>(in.srcHeight);
    cb.srcSize[2] = 1.0f / static_cast<f32>(in.srcWidth);
    cb.srcSize[3] = 1.0f / static_cast<f32>(in.srcHeight);

    ctx.setPipeline(pipeline_);
    ctx.setBindingSet(binding_);
    ctx.setConstantBuffer(kFxaaConstantRegister, &cb, sizeof(cb));
    ctx.drawFullscreen();
}

const char* fxaaResolveShaderSource() {
    // NOT a plain function-local static: the loader owns the cache and reloadShaderFiles()
    // clears it, so a static built once here would outlive the drop and make hot reload a lie.
    // Keyed on shaderFileRevision(), exactly as sharedShaderPrelude() is.
    static std::string s;
    static u64 built = ~0ull;
    if (built != rhi::shaderFileRevision()) {
        s = rhi::shaderFile("sr_fxaa.hlsl");
        built = rhi::shaderFileRevision();
    }
    return s.c_str();
}

} // namespace aver::sr
