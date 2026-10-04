// Temporal anti-aliasing with upscale (TAAU) on the rhi::IUpscaler seam: the device jitters the
// camera by a sub-pixel Halton offset each frame (it asks needs() for Jitter), and this resolves
// those samples into an output-resolution history, reprojected with the G-buffer velocity and
// clipped to each frame's neighbourhood, then sharpened with FSR 1's RCAS.
#pragma once

#include "aver/rhi/RHIResources.hpp"
#include "aver/sr/AverSrFsr.hpp"

namespace aver::sr {

class TemporalUpscaler final : public rhi::IUpscaler {
public:
    explicit TemporalUpscaler(rhi::IResourceFactory& factory) : res_(factory), fallback_(factory) {}
    ~TemporalUpscaler() override;

    TemporalUpscaler(const TemporalUpscaler&) = delete;
    TemporalUpscaler& operator=(const TemporalUpscaler&) = delete;

    const char* name() const override { return "AverSR Temporal (TAAU + RCAS)"; }
    rhi::UpscalerNeeds needs() const override {
        return rhi::UpscalerNeeds::Jitter | rhi::UpscalerNeeds::MotionVectors | rhi::UpscalerNeeds::Depth |
               rhi::UpscalerNeeds::History;
    }
    bool isTemporal() const override { return true; }
    void reset() override { resetPending_ = true; }

    void setSharpness(f32 stops) { sharpness_ = stops < 0.0f ? 0.0f : stops; fallback_.setSharpness(stops); }
    // Edge AA applies only while falling back to FSR (no G-buffer, or a backend that cannot retarget).
    void setEdgeAa(bool on) { fallback_.setEdgeAa(on); }

    void execute(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in, rhi::TextureHandle outTarget) override;

private:
    bool ensurePipelines(rhi::Format outFormat);
    bool ensureTargets(u32 dstW, u32 dstH);
    void releaseTargets();

    rhi::IResourceFactory& res_;
    FsrUpscaler            fallback_;
    bool                   failed_ = false, fallbackLogged_ = false;

    rhi::PipelineHandle   resolve_ = 0, rcas_ = 0;
    rhi::Format           rcasFormat_ = rhi::Format::Unknown;
    rhi::BindingSetHandle resolveSet_ = 0, rcasSet_ = 0;

    rhi::TextureHandle history_[2] = {};   // output size, squashed; ping-pong
    rhi::TextureHandle scratch_ = 0;       // a generated frame's result, which history never keeps
    u32  dstW_ = 0, dstH_ = 0, srcW_ = 0, srcH_ = 0;
    u32  cur_ = 0;
    bool resetPending_ = true;

    f32 sharpness_ = 0.2f;
};

const char* taaShaderSource();

} // namespace aver::sr
