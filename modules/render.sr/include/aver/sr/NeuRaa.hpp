// NeuRAA: single-frame edge anti-aliasing driven by ray-traced primary visibility
// (docs/rendering/NEURAA_NRD.md section 3). It wraps the device's upscaler: it runs on the scene
// image first, then hands the result to the wrapped upscaler (or a plain resample when none).
//
// Phase 1: edge detection and the edge-class debug view only; the scene image is untouched
// unless the debug view is on.
#pragma once

#include "aver/rhi/RHIResources.hpp"
#include "aver/sr/AverSrSpatial.hpp"

namespace aver::sr {

class NeuRaa final : public rhi::IUpscaler {
public:
    explicit NeuRaa(rhi::IResourceFactory& factory) : res_(factory), passthrough_(factory) {}
    ~NeuRaa() override;

    NeuRaa(const NeuRaa&) = delete;
    NeuRaa& operator=(const NeuRaa&) = delete;

    // The upscaler that runs after NeuRAA; null means a 1:1 resample. Not owned.
    void setInner(rhi::IUpscaler* inner) { inner_ = inner; }
    [[nodiscard]] rhi::IUpscaler* inner() const { return inner_; }
    // Replaces the scene image with edge classes (red silhouette, yellow depth step, cyan crease).
    void setDebugView(bool on) { debugView_ = on; }

    const char* name() const override { return "NeuRAA (edge AA)"; }
    rhi::UpscalerNeeds needs() const override;
    bool isTemporal() const override { return inner_ && inner_->isTemporal(); }
    void reset() override { if (inner_) inner_->reset(); }

    void execute(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in, rhi::TextureHandle outTarget) override;

private:
    bool ensurePipeline();
    bool ensureTargets(u32 w, u32 h);
    void releaseTargets();
    // Records the detection pass; false when this frame has nothing to detect from.
    bool detect(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in);

    rhi::IResourceFactory& res_;
    SpatialUpscaler        passthrough_;
    rhi::IUpscaler*        inner_ = nullptr;
    bool                   debugView_ = false;
    bool                   failed_ = false;

    rhi::PipelineHandle   detect_ = 0;
    rhi::BindingSetHandle set_ = 0;
    u64                   shaderRevision_ = ~0ull;

    rhi::TextureHandle edges_ = 0;    // R8Uint edge code per pixel (NEURAA_NRD.md section 3)
    rhi::TextureHandle debug_ = 0;    // RGBA16F edge-class image, replaces the scene in the debug view
    rhi::BufferHandle  tiles_ = 0;    // one uint per 8x8 tile: 1 when it holds an edge
    u32 w_ = 0, h_ = 0, tileCount_ = 0;
};

} // namespace aver::sr
