// AverSR's built-in floor: a dependency-free, non-temporal, non-learned resample. See
// docs/AVERSR.md for what AverSR is as a whole and modules/render.sr/README.md for this module.
#pragma once

#include "aver/rhi/RHIResources.hpp"

namespace aver::sr {

// A bicubic (Catmull-Rom) resample in x and y, sharper than a plain bilinear stretch without
// needing depth, motion vectors, a jitter offset or a history buffer -- needs() answers None and
// means it. Built entirely from the generic RHI (rhi::IResourceFactory + rhi::IRenderContext), the
// same portability every render-feature module already has, so it carries no backend-specific code
// of its own.
//
// THIS IS NOT FSR, AND IT MUST NOT BE DESCRIBED AS FSR OR AS "AI" ANYTHING. It is plain image
// resampling: the honest floor AverSR has whether or not a vendored upscaler is registered instead,
// and what proves the seam in Aver.RHI is real rather than aspirational -- an implementation with
// nothing backend-specific in it, compiled against rhi::IUpscaler like anything else would be.
class SpatialUpscaler final : public rhi::IUpscaler {
public:
    // Builds its pipeline and binding set lazily, from `factory`, on first execute() -- the same
    // pattern render-feature modules already use for their own pipelines (see
    // VoxiRenderer::createPipelines), just with one pipeline instead of a dozen, and deferred
    // rather than upfront because the output render-target FORMAT (needed to create the pipeline)
    // is not known until the first real `outTarget` is handed to execute(). `factory` must outlive
    // every execute() call.
    explicit SpatialUpscaler(rhi::IResourceFactory& factory) : res_(factory) {}
    ~SpatialUpscaler() override;

    SpatialUpscaler(const SpatialUpscaler&) = delete;
    SpatialUpscaler& operator=(const SpatialUpscaler&) = delete;

    const char* name() const override { return "AverSR Spatial (Catmull-Rom)"; }
    rhi::UpscalerNeeds needs() const override { return rhi::UpscalerNeeds::None; }
    bool isTemporal() const override { return false; }

    void execute(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in, rhi::TextureHandle outTarget) override;

private:
    bool ensurePipeline(rhi::TextureHandle outTarget);

    rhi::IResourceFactory& res_;
    rhi::PipelineHandle    pipeline_ = 0;
    rhi::BindingSetHandle  binding_  = 0;
    rhi::Format            pipelineFormat_ = rhi::Format::Unknown;   // outTarget's format when pipeline_ was built
    rhi::TextureHandle     boundColor_     = 0;                     // which scene texture binding_ currently names
};

// The HLSL for SpatialUpscaler's one pass: a fullscreen triangle VS (SV_VertexID, no vertex buffer
// -- the same trick rhi::postShaderSource()'s PostVS uses) and a Catmull-Rom bicubic PS.
// Self-contained: no rhi::sharedShaderPrelude(), its own tiny constant buffer -- this is a leaf
// image-resampling operation, not a scene shader, and has no business depending on scene state.
const char* spatialUpscaleShaderSource();

} // namespace aver::sr
