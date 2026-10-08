// AMD FidelityFX Super Resolution 1 (third_party/fidelityfx-fsr, MIT) on the rhi::IUpscaler seam:
// EASU (edge-adaptive upscale) then RCAS (contrast-adaptive sharpening), with optional FXAA-class edge
// AA on the source first. Spatial: no depth, motion vectors, jitter or history.
#pragma once

#include "aver/rhi/RHIResources.hpp"
#include "aver/sr/AverSrSpatial.hpp"

namespace aver::sr {

class FsrUpscaler final : public rhi::IUpscaler {
public:
    // `factory` must outlive every execute(). Pipelines and targets are built on first use.
    explicit FsrUpscaler(rhi::IResourceFactory& factory) : res_(factory), fallback_(factory) {}
    ~FsrUpscaler() override;

    FsrUpscaler(const FsrUpscaler&) = delete;
    FsrUpscaler& operator=(const FsrUpscaler&) = delete;

    const char* name() const override { return "AverSR FSR 1 (EASU + RCAS)"; }

    // Edge AA on the source before the upscale. Also what makes this worth running at 1:1 scale.
    void setEdgeAa(bool on) { edgeAa_ = on; }
    bool edgeAa() const { return edgeAa_; }
    // RCAS strength in stops: 0 is maximum sharpening, each +1 halves it. AMD's default is 0.2.
    void setSharpness(f32 stops) { sharpness_ = stops < 0.0f ? 0.0f : stops; }

    void execute(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in, rhi::TextureHandle outTarget) override;

private:
    bool ensurePipelines(rhi::Format outFormat);
    bool ensureTargets(u32 srcW, u32 srcH, u32 dstW, u32 dstH);
    void releaseTargets();
    void draw(rhi::IRenderContext& ctx, rhi::PipelineHandle p, rhi::BindingSetHandle set,
              rhi::TextureHandle src, rhi::TextureHandle& bound, const void* cb, u32 w, u32 h);

    rhi::IResourceFactory& res_;
    SpatialUpscaler        fallback_;   // used if FSR's pipelines will not build
    bool                   failed_ = false;

    rhi::PipelineHandle   prep_[2] = {};   // [edgeAa]
    rhi::PipelineHandle   easu_ = 0, rcas_ = 0;
    rhi::Format           rcasFormat_ = rhi::Format::Unknown;
    rhi::BindingSetHandle prepSet_ = 0, easuSet_ = 0, rcasSet_ = 0;
    rhi::TextureHandle    prepBound_ = 0, easuBound_ = 0, rcasBound_ = 0;

    rhi::TextureHandle squashed_ = 0;   // source size: edge-AA'd, squashed input
    rhi::TextureHandle upscaled_ = 0;   // output size: EASU result
    u32 srcW_ = 0, srcH_ = 0, dstW_ = 0, dstH_ = 0;

    bool edgeAa_ = false;
    f32  sharpness_ = 0.2f;
};

// The HLSL for all three passes (sr_fsr1.hlsl, which includes the vendored FSR headers).
const char* fsrShaderSource();

} // namespace aver::sr
