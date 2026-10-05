// Nrd2Capture -- NRD2 phase 3 training capture and oracle fit (docs/rendering/NRD2.md, "Phase 3").
//
// Driven by Nrd2::record on every NRD2 frame; the host moves the camera between poses and freezes it
// while holding() (as for the NRD v1 capture). Per pose:
//   Settle (once) -> Travel(45) -> Hold(H): snapshots of D, S, their pyramids and the feature tensor
//   at hold frames 2/4/8/16 (guides once), fp32 split-half running means of D and S over fresh pixels
//   from frame 17 -> Fit: GPU oracle per (8x8 tile, signal), 27-start grid then Adam (or the
//   derivative-free pattern search), a few iterations per frame -> Readback(6) -> Write pose_NNN.n2p.
// Camera jitter is suppressed while holding (IDevice::setJitterSuppressed).
//
// Patent rule 4 (NEURAA_NRD.md section 7): the oracle fits free per-tile variables through the filter;
// no model is involved. The maths is nrd2_capture.hlsl, CPU twin Nrd2ResolveReference.
#pragma once

#include <aver/core/Types.hpp>
#include <aver/render/denoise/Nrd2.hpp>
#include <aver/render/denoise/Nrd2ResolveReference.hpp>
#include <aver/rhi/RHI.hpp>
#include <aver/rhi/RHIResources.hpp>

#include <string>

namespace aver::render::denoise {

struct Nrd2CaptureConfig {
    std::string dir;              // pose files land here (created)
    std::string scene;            // scene id stamped into every file
    u32 poses = 0;
    u32 hold = 256;               // hold frames per pose (at least 32)
    u32 heldOutFrom = ~0u;        // poses with index >= this are flagged held-out
    Nrd2OracleMode oracle = Nrd2OracleMode::Grad;
    u32 adamIters = 150;
    u32 patternIters = 24;
    u32 itersPerFrame = 8;        // fit iterations per frame (each = 4 snapshot passes + a step)
    u32 settleFrames = 30;
    u32 travelFrames = 45;
};

class Nrd2Capture {
public:
    explicit Nrd2Capture(rhi::IDevice& dev);
    ~Nrd2Capture();
    Nrd2Capture(const Nrd2Capture&)            = delete;
    Nrd2Capture& operator=(const Nrd2Capture&) = delete;

    void start(const Nrd2CaptureConfig& cfg);
    [[nodiscard]] bool active() const { return state_ != State::Idle; }
    [[nodiscard]] bool holding() const { return state_ == State::Hold; }

    // Inside Nrd2::record after the resolve: Stage B's targets, the G-buffer, the pyramid in
    // NonPixelShaderResource; all left as found.
    void step(rhi::IRenderContext& ctx, Nrd2& nrd2, const Nrd2::Inputs& in);
    void release();

private:
    enum class State : u8 { Idle, Settle, Travel, Hold, Fit, Readback };
    enum class FitPhase : u8 { Grid, Iterate, Final };
    static constexpr u32 kSnapshots = 4;

    bool ensurePipelines();
    bool ensureTargets(const Nrd2& nrd2, const Nrd2::Inputs& in);
    void releaseTargets();
    void snapshot(rhi::IRenderContext& ctx, Nrd2& nrd2, const Nrd2::Inputs& in, u32 k);
    void accumulate(rhi::IRenderContext& ctx, Nrd2& nrd2, const Nrd2::Inputs& in, u32 holdFrame);
    void tileStats(rhi::IRenderContext& ctx);
    void fitFrame(rhi::IRenderContext& ctx);
    void writePose();
    void abortPose(const char* why);

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    Nrd2CaptureConfig cfg_;
    State state_ = State::Idle;
    FitPhase phase_ = FitPhase::Grid;
    u32 frame_ = 0, index_ = 0, remaining_ = 0, iter_ = 0;
    bool pipelinesTried_ = false;

    // Pose geometry: render target, viewport, tiles.
    u32 rtW_ = 0, rtH_ = 0, rect_[4] = {}, tilesX_ = 0, tilesY_ = 0;
    f32 theta0_[12] = {};

    rhi::PipelineHandle psoGeo_ = 0, psoAccum_ = 0, psoStats_ = 0, psoEval_ = 0, psoGrad_ = 0, psoStep_ = 0;
    rhi::BindingSetHandle setGeo_ = 0, setAccum_ = 0, setStats_ = 0, setStep_ = 0;
    rhi::BindingSetHandle setSnap_[kSnapshots] = {};   // eval and grad (same layout), one per snapshot

    rhi::TextureHandle snapD_[kSnapshots] = {}, snapS_[kSnapshots] = {};
    rhi::TextureHandle snapLvD_[kSnapshots][3] = {}, snapLvS_[kSnapshots][3] = {};
    rhi::TextureHandle snapG_[3] = {};
    rhi::BufferHandle geo_ = 0, mean_ = 0, fit_ = 0, acc_ = 0, out_ = 0;
    rhi::BufferHandle rbFeat_[kSnapshots] = {}, rbOut_ = 0;
};

}  // namespace aver::render::denoise
