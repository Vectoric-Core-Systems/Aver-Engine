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

#include <string>
#include <utility>

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
    // The trained network's weights (bin/data/neuraa_v1.bin). Without them the baseline blend runs.
    void setWeightsPath(std::string path) { weightsPath_ = std::move(path); weightsTried_ = false; }
    [[nodiscard]] bool networkLoaded() const { return net_ != 0; }

    // Training capture (NEURAA_NRD.md section 3): `count` still poses, each an unjittered base frame
    // and a 64-sample jittered reference, written to `dir` as neuraa_<n>.bin. The host moves the
    // camera between poses and holds it still while captureHolding() is true.
    void startCapture(const std::string& dir, u32 count);
    [[nodiscard]] bool captureActive() const { return cap_.state != CapState::Idle; }
    [[nodiscard]] bool captureHolding() const {
        return cap_.state != CapState::Idle && cap_.state != CapState::Travel;
    }
    bool jitterOverride(f32& x, f32& y) override;

    const char* name() const override { return "NeuRAA (edge AA)"; }
    rhi::UpscalerNeeds needs() const override;
    bool isTemporal() const override { return inner_ && inner_->isTemporal(); }
    void reset() override { if (inner_) inner_->reset(); }

    void execute(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in, rhi::TextureHandle outTarget) override;

private:
    bool ensurePipelines();
    void loadWeights();
    bool ensureTargets(u32 w, u32 h);
    void releaseTargets();
    // Records detection, then the resolve when enabled; returns the image for the wrapped
    // upscaler, or 0 when this frame has nothing to work from.
    rhi::TextureHandle run(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in);

    enum class CapState : u8 { Idle, Travel, Settle, Base, Reference, Readback };
    // Readbacks, in file order: base colour, reference mean, view Z, edge codes, edge distances, and
    // NeuRAA's own resolved frame (valid when it was enabled for the capture).
    static constexpr u32 kCapArrays = 6;
    struct Capture {
        std::string dir;
        u32 remaining = 0, index = 0, frame = 0;
        CapState state = CapState::Idle;
        u32 w = 0, h = 0, vp[4] = {};
        bool resolved = false;
        rhi::TextureHandle accum = 0;   // RGBA32F sum of the reference frames
        rhi::BufferHandle  rb[kCapArrays] = {};
        rhi::TextureCopyFootprint fp[kCapArrays] = {};
    };
    void captureStep(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in);
    bool captureTargets(const rhi::UpscalerInput& in);
    void captureCopy(rhi::IRenderContext& ctx, u32 slot, rhi::TextureHandle t, rhi::ResourceState rest);
    void captureWrite();
    void captureRelease();

    rhi::IResourceFactory& res_;
    SpatialUpscaler        passthrough_;
    rhi::IUpscaler*        inner_ = nullptr;
    bool                   enabled_ = false;
    bool                   debugView_ = false;
    bool                   failed_ = false;

    rhi::PipelineHandle   detect_ = 0, resolve_ = 0, accum_ = 0;
    rhi::BindingSetHandle detectSet_ = 0, resolveSet_ = 0, accumSet_ = 0;
    Capture               cap_;

    std::string        weightsPath_;
    bool               weightsTried_ = false;
    rhi::BufferHandle  net_ = 0;          // the weights file body, as floats (sr_neuraa.hlsl's gNet)
    u32                netFloats_ = 0;
    rhi::BufferHandle  netPlaceholder_ = 0;   // one float, bound while no weights are loaded

    rhi::TextureHandle edges_ = 0;    // R8Uint edge code per pixel (NEURAA_NRD.md section 3)
    rhi::TextureHandle dist_  = 0;    // RGBA8 own-triangle edge distance toward each neighbour
    rhi::TextureHandle debug_ = 0;    // RGBA16F edge-class image, replaces the scene in the debug view
    rhi::TextureHandle aa_    = 0;    // RGBA16F the scene with edges resolved
    rhi::BufferHandle  tiles_ = 0;    // one uint per 8x8 tile: 1 when it holds an edge
    u32 w_ = 0, h_ = 0, tileCount_ = 0;
};

} // namespace aver::sr
