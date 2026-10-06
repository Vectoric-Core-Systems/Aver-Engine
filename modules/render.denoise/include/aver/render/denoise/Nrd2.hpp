// Nrd2 -- NRD2 phase 1 (docs/rendering/NRD2.md): a single-frame denoiser of the composed, demodulated
// lighting Stage B writes. Fixed maths with per-8x8-tile parameters: the defaults, or the phase 4
// network's (Nrd2Network) when its weights pass the live gate. The spatial filter and the network never
// see history; on jitter-free frames an optional temporal stage (CSNrd2Reproject, CSNrd2Prefilter,
// CSNrd2Temporal; "Temporal stabiliser" in the doc, after FidelityFX's reflections denoiser) then blends
// its output with last frame's.
//
// Flow per frame, inside the scene pass right after Stage B's draw (D3D12 staged ray-driven only):
//   Stage B writes D, S, remodulation (targets owned here, bound by the caller as UAVs)
//   record():        pyramid of D and S (1/2, 1/4, 1/8), tile parameters (network or defaults),
//                    resolve -> D'*Rd + S'*Rs (or D', S' -> temporal stage -> the same compose)
//   recordCompose(): fullscreen additive draw of that into the bound scene colour
//
// Every pass is plain compute (fp32, no wave intrinsics, no atomics, constants at b3) so it ports to
// Vulkan when the staged ray-driven frame does.
#pragma once

#include <aver/core/Types.hpp>
#include <aver/render/denoise/Nrd2Network.hpp>
#include <aver/rhi/RHI.hpp>
#include <aver/rhi/RHIResources.hpp>

#include <memory>
#include <string>

namespace aver::render::denoise {

class Nrd2Capture;
struct Nrd2CaptureConfig;

// Bumped whenever what Stage B writes for NRD2, or the resolve the oracle fits, changes meaning (stamped into
// training datasets). 2: plane-predicted tap depth.
inline constexpr u32 kNrd2StageBVersion = 2;
// Parameters per 8x8 tile (nrd2_resolve.hlsli): for D then S, logits l1..l3 and log2 sensitivities
// depth / normal / luminance.
inline constexpr u32 kNrd2TileParams = 12;

struct Nrd2Params {
    // Pinned form of NRD v1's measured {-2, -1, 0, 0} (own, 1/2, 1/4, 1/8): own = 0.
    f32 diffuse[6]  = {1.0f, 2.0f, 2.0f, 4.5f, 3.0f, -1.0f};
    f32 specular[6] = {1.0f, 2.0f, 2.0f, 4.5f, 4.0f, -1.0f};
    bool bypass = false;   // own pixel only: the split recomposed undenoised (A/B check)
    bool network = true;   // phase 4: the trained network sets the tile parameters when it can
    bool stabilise = false;   // the temporal stage, on jitter-free frames with the Inputs below filled
    f32 stabFrames = 32.0f;   // its history length at rest, in frames (from 8 px/frame: min(8, this))
    u32 despeckle = 0;        // CSNrd2Despeckle before the pyramid: 1 D, 2 S (not while a capture runs)
    f32 despeckleCap = 2.0f;  // its cap, times the 5th brightest neighbour
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

        // Temporal stage. The defaults leave it off: it runs only when all of these are filled.
        rhi::TextureHandle velocity = 0;          // G-buffer velocity, RG16F texels/frame (destination minus source)
        f32 prevViewProj[16] = {};                // IDevice::gBufferPrevViewProj (all zero = not filled)
        f32 prevCamPos[3] = {};                   // the camera position that matrix belongs to
        bool historyValid = false;                // !IDevice::gBufferHistoryInvalid()
        bool sunMoved = false;                    // the sun changed since last frame (history shortens for 2 frames)
        f32 jitter[2] = {1.0f, 1.0f};             // this frame's TAA jitter (IDevice::taaJitter); only exactly 0, 0 stabilises
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
    // Drops the temporal history: call on a frame NRD2 did not run (it cannot see those).
    void resetHistory() { histValid_ = false; }
    // The network's weights: the user's file wins over the shipped one (Nrd2Network).
    void setNetworkWeights(std::string user, std::string shipped) {
        network_.setWeightPaths(std::move(user), std::move(shipped));
    }
    [[nodiscard]] Nrd2NetworkStatus networkStatus() const { return network_.status(); }

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
    void releaseStab();
    bool allocStab();
    [[nodiscard]] bool stabReady() const;   // the temporal stage's three pipelines and binding sets exist
    // CSNrd2Features into features_ (12 x 4 tilesX x 4 tilesY floats, rests in Common). Inputs as in
    // record() after the resolve; false when the pass would not build. inScale/inBias: the network's input
    // standardisation applied as it stores (null = the raw features, as the capture needs them).
    bool recordFeatures(rhi::IRenderContext& ctx, const Inputs& in, const f32* inScale = nullptr,
                        const f32* inBias = nullptr);

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    rhi::PipelineHandle psoPyramid_ = 0, psoParams_ = 0, psoResolve_ = 0, compose_ = 0;
    rhi::PipelineHandle psoReproject_ = 0, psoPrefilter_ = 0, psoTemporal_ = 0, psoDespeckle_ = 0;
    rhi::BindingSetHandle setPyramid_ = 0, setParams_ = 0, setResolve_ = 0, setCompose_ = 0;
    rhi::BindingSetHandle setReproject_ = 0, setPrefilter_ = 0, setTemporal_ = 0, setDespeckle_ = 0;

    Targets targets_{};
    rhi::TextureHandle guide_[3] = {}, levelD_[3] = {}, levelS_[3] = {};
    rhi::TextureHandle lit_ = 0;
    // Despeckled D and S (allocated on the first despeckled frame, rest readable); what the pyramid, resolve
    // and features read this frame (inD/inS).
    rhi::TextureHandle despD_ = 0, despS_ = 0;
    bool despeckled_ = false;
    rhi::TextureHandle inD() const { return despeckled_ ? despD_ : targets_.diffuse; }
    rhi::TextureHandle inS() const { return despeckled_ ? despS_ : targets_.specular; }
    bool allocDespeckle();
    // Temporal stage (allocated on the first stabilised frame): the resolve's D' and S' (rest as UAVs); this
    // frame's scratch (reprojected history rgb + sample count, noise estimate, 1/8 anchors, prefiltered D' and
    // S' with their noise estimate); and ping-pong history: D'' rgb + count, S'' rgb + count, view Z (m) + packed
    // normal xy, noise estimate of D and S.
    rhi::TextureHandle dRes_ = 0, sRes_ = 0;
    rhi::TextureHandle rpD_ = 0, rpS_ = 0, rpV_ = 0, anchD_ = 0, anchS_ = 0, prefD_ = 0, prefS_ = 0;
    rhi::TextureHandle histD_[2] = {}, histS_[2] = {}, histG_[2] = {}, histV_[2] = {};
    u32 histLast_ = 0;               // which of the pair the last stabilised frame wrote
    bool histValid_ = false;
    u32 histViewport_[4] = {};
    u32 sunHold_ = 0;
    rhi::BufferHandle  tileParams_ = 0;
    u32 tileCapacity_ = 0;
    rhi::PipelineHandle psoFeatures_ = 0;
    rhi::BindingSetHandle setFeatures_ = 0;
    rhi::BufferHandle features_ = 0;
    u32 featureFloats_ = 0;
    bool featuresTried_ = false;
    std::unique_ptr<Nrd2Capture> capture_;
    Nrd2Network network_;
    bool jitterSuppressed_ = false;
    u32 width_ = 0, height_ = 0;
    u32 failedWidth_ = 0, failedHeight_ = 0;
    bool recorded_ = false;
    Nrd2Params params_{};
};

}  // namespace aver::render::denoise
