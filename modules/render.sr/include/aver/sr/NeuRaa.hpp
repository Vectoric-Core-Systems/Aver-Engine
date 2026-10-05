// NeuRAA: single-frame edge anti-aliasing driven by ray-traced primary visibility
// (docs/rendering/NEURAA_NRD.md section 3). It wraps the device's upscaler: it runs on the scene
// image first, then hands the result to the wrapped upscaler (or a plain resample when none).
//
// Today: edge detection, the edge-class debug view, and the no-network baseline blend
// (distance-to-edge from the visibility buffer's barycentrics). Ray-driven frames only; without a
// visibility record it passes the image through untouched.
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
    // The anti-aliasing itself (the project's RENDER.NEURAA).
    void setEnabled(bool on) { enabled_ = on; }
    // Replaces the scene image with edge classes (red silhouette, yellow depth step, cyan crease).
    void setDebugView(bool on) { debugView_ = on; }

    const char* name() const override { return "NeuRAA (edge AA)"; }
    rhi::UpscalerNeeds needs() const override;
    bool isTemporal() const override { return inner_ && inner_->isTemporal(); }
    void reset() override { if (inner_) inner_->reset(); }

    void execute(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in, rhi::TextureHandle outTarget) override;

private:
    bool ensurePipelines();
    bool ensureTargets(u32 w, u32 h);
    void releaseTargets();
    // Records detection, then the resolve when enabled; returns the image for the wrapped
    // upscaler, or 0 when this frame has nothing to work from.
    rhi::TextureHandle run(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in);

    rhi::IResourceFactory& res_;
    SpatialUpscaler        passthrough_;
    rhi::IUpscaler*        inner_ = nullptr;
    bool                   enabled_ = false;
    bool                   debugView_ = false;
    bool                   failed_ = false;

    rhi::PipelineHandle   detect_ = 0, resolve_ = 0;
    rhi::BindingSetHandle detectSet_ = 0, resolveSet_ = 0;

    rhi::TextureHandle edges_ = 0;    // R8Uint edge code per pixel (NEURAA_NRD.md section 3)
    rhi::TextureHandle dist_  = 0;    // RGBA8 own-triangle edge distance toward each neighbour
    rhi::TextureHandle debug_ = 0;    // RGBA16F edge-class image, replaces the scene in the debug view
    rhi::TextureHandle aa_    = 0;    // RGBA16F the scene with edges resolved
    rhi::BufferHandle  tiles_ = 0;    // one uint per 8x8 tile: 1 when it holds an edge
    u32 w_ = 0, h_ = 0, tileCount_ = 0;
};

} // namespace aver::sr
