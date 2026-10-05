// Nrd2 -- NRD2 phase 1 (docs/rendering/NRD2.md): a strictly single-frame denoiser of the composed,
// demodulated lighting Stage B writes. No history of any kind. Fixed maths with per-8x8-tile
// parameters (defaults now; phase 4's network writes the same buffer).
//
// Flow per frame, inside the scene pass right after Stage B's draw (D3D12 staged ray-driven only):
//   Stage B writes D, S, remodulation (targets owned here, bound by the caller as UAVs)
//   record():        pyramid of D and S (1/2, 1/4, 1/8), tile parameters, resolve -> D'*Rd + S'*Rs
//   recordCompose(): fullscreen additive draw of that into the bound scene colour
//
// Every pass is plain compute (fp32, no wave intrinsics, no atomics, constants at b3) so it ports to
// Vulkan when the staged ray-driven frame does.
#pragma once

#include <aver/core/Types.hpp>
#include <aver/rhi/RHI.hpp>
#include <aver/rhi/RHIResources.hpp>

#include <memory>

namespace aver::render::denoise {

class Nrd2Capture;
struct Nrd2CaptureConfig;

// Bumped whenever what Stage B writes for NRD2 changes meaning (stamped into training datasets).
inline constexpr u32 kNrd2StageBVersion = 1;
// Parameters per 8x8 tile (nrd2_resolve.hlsli): for D then S, logits l1..l3 and log2 sensitivities
// depth / normal / luminance.
inline constexpr u32 kNrd2TileParams = 12;

struct Nrd2Params {
    // Pinned form of NRD v1's measured {-2, -1, 0, 0} (own, 1/2, 1/4, 1/8): own = 0.
    f32 diffuse[6]  = {1.0f, 2.0f, 2.0f, 4.5f, 3.0f, -1.0f};
    f32 specular[6] = {1.0f, 2.0f, 2.0f, 4.5f, 4.0f, -1.0f};
    bool bypass = false;   // own pixel only: the split recomposed undenoised (A/B check)
};

class Nrd2 {
public:
    Nrd2() = default;
    ~Nrd2();
    Nrd2(const Nrd2&)            = delete;
    Nrd2& operator=(const Nrd2&) = delete;

    // Stage B's targets, at render-target size. Rest in UnorderedAccess (the caller binds them as
    // UAVs). Remod A: rgb Rd, a Rs.r; remod B (RG16F): Rs.gb.
    struct Targets {
        rhi::TextureHandle diffuse = 0, specular = 0, remodA = 0, remodB = 0;
    };

    struct Inputs {
        rhi::TextureHandle viewZ = 0;             // G-buffer, R32Float cm
        rhi::TextureHandle normalRoughness = 0;   // G-buffer, averPackNormalRoughness
        rhi::ResourceState gbufferState = rhi::ResourceState::RenderTarget;
        u32 viewport[4] = {};                     // scene viewport x y w h, render-target pixels
        // Which pixels were traced this frame (the capture's fresh mask): bit 0 D on ReSTIR GI's
        // checkerboard, bit 1 its parity (traced where ((x ^ y ^ parity) & 1) == 0); bits 2/3 the same
        // for S on the glossy reflections' checkerboard. 0 = everything traced.
        u32 halfRate = 0;
    };

    // Compute pipelines. False (said once) when nrd2.hlsl will not compile.
    bool create(rhi::IDevice& dev);
    void destroy();
    [[nodiscard]] bool valid() const { return psoPyramid_ != 0 && psoParams_ != 0 && psoResolve_ != 0; }

    // The compose draw: blends into target 0 of the scene pass's colour + three G-buffer targets.
    bool createCompose(rhi::Format color, const rhi::Format gbuffer[3], rhi::Format depth, u32 sampleCount);
    void destroyCompose();
    [[nodiscard]] bool composeValid() const { return compose_ != 0; }

    // Targets for this render-target size. Idempotent at an unchanged size.
    bool resize(u32 width, u32 height);
    [[nodiscard]] const Targets& targets() const { return targets_; }

    void setParams(const Nrd2Params& p) { params_ = p; }

    // Pyramid, parameters, resolve. Stage B's targets in UnorderedAccess and the G-buffer in
    // `gbufferState` on entry; both are back in those states on return.
    bool record(rhi::IRenderContext& ctx, const Inputs& in);
    // Adds the resolved lighting into the bound colour target. Only after a successful record().
    void recordCompose(rhi::IRenderContext& ctx);

    // Phase 3 training capture (Nrd2Capture.hpp); it steps inside record().
    void startCapture(const Nrd2CaptureConfig& cfg);
    [[nodiscard]] bool captureActive() const;
    // The host freezes its camera while this is true.
    [[nodiscard]] bool captureHolding() const;

private:
    friend class Nrd2Capture;
    void releaseTargets();
    // CSNrd2Features into features_ (12 x 4 tilesX x 4 tilesY floats, rests in Common). Inputs as in
    // record() after the resolve; false when the pass would not build.
    bool recordFeatures(rhi::IRenderContext& ctx, const Inputs& in);

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    rhi::PipelineHandle psoPyramid_ = 0, psoParams_ = 0, psoResolve_ = 0, compose_ = 0;
    rhi::BindingSetHandle setPyramid_ = 0, setParams_ = 0, setResolve_ = 0, setCompose_ = 0;

    Targets targets_{};
    rhi::TextureHandle guide_[3] = {}, levelD_[3] = {}, levelS_[3] = {};
    rhi::TextureHandle lit_ = 0;
    rhi::BufferHandle  tileParams_ = 0;
    u32 tileCapacity_ = 0;
    rhi::PipelineHandle psoFeatures_ = 0;
    rhi::BindingSetHandle setFeatures_ = 0;
    rhi::BufferHandle features_ = 0;
    u32 featureFloats_ = 0;
    bool featuresTried_ = false;
    std::unique_ptr<Nrd2Capture> capture_;
    bool jitterSuppressed_ = false;
    u32 width_ = 0, height_ = 0;
    u32 failedWidth_ = 0, failedHeight_ = 0;
    bool recorded_ = false;
    Nrd2Params params_{};
};

}  // namespace aver::render::denoise
