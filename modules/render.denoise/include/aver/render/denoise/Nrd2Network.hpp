// Nrd2Network -- NRD2 phase 4 inference (docs/rendering/NRD2.md): the trained ConvNet writes the
// per-8x8-tile parameter buffer the resolve reads, in place of CSNrd2Params' defaults.
//
// Per frame (inside Nrd2::record, after the pyramid): CSNrd2Features (raw) -> CSNrd2NetIn (standardised
// in place with the weight file's input affine) -> ConvNet::recordInfer (EMA weights) -> CSNrd2NetOut
// (output affine, clamped: logits +-8, log2 sensitivities +-6, non-finite -> defaults). Only per-tile
// parameters come out (patent rule 6); the resolve stays fixed maths.
//
// Weights: the user's %LOCALAPPDATA%\AverEngine\nrd2_v1.avnn over the shipped bin/data copy, reloaded when
// the file changes (training saves a new best). Live gate from the "<file>.steps" sidecar's held-out
// val/default ratio (parameter space): on at <= 0.8, off above 0.9. Anything missing or failing: false,
// and the caller runs the defaults (warned once per reason).
#pragma once

#include <aver/core/Types.hpp>
#include <aver/render/neural/ConvNet.hpp>
#include <aver/rhi/RHI.hpp>
#include <aver/rhi/RHIResources.hpp>

#include <string>

namespace aver::render::denoise {

// The network's weight file: %LOCALAPPDATA%\AverEngine\<this> (training writes it) over bin/data/<this>.
inline constexpr const char* kNrd2WeightsFileName = "nrd2_v1.avnn";

struct Nrd2NetworkStatus {
    enum class Source : u8 { None, User, Shipped };
    Source source = Source::None;
    std::string path;
    u64 steps = 0;            // lifetime training steps behind the weights
    f32 ratio = -1.0f;        // held-out val/default (parameter space); < 0 = not measured
    bool gateOpen = false;
    bool running = false;     // drove the last NRD2 frame's parameters
    std::string problem;      // why it is not running (empty when it is)
};

class Nrd2Network {
public:
    Nrd2Network() = default;
    ~Nrd2Network();
    Nrd2Network(const Nrd2Network&)            = delete;
    Nrd2Network& operator=(const Nrd2Network&) = delete;

    void setWeightPaths(std::string user, std::string shipped);

    // Builds on first use, polls the weight file, checks the live gate. False: the defaults this frame
    // (the reason in status().problem, logged once).
    bool ready(rhi::IDevice& dev);
    // After ready(). features: CSNrd2Features' tensor (12 x 4 tilesY x 4 tilesX floats, Common;
    // standardised in place). params: the resolve's tile buffer (12 x tilesX x tilesY floats, Common on
    // entry and exit). False when it could not record; nothing was written then.
    bool record(rhi::IRenderContext& ctx, rhi::BufferHandle features, u32 featureFloats, rhi::BufferHandle params,
                u32 paramFloats, u32 tilesX, u32 tilesY, const f32 defaults[12]);
    // A frame the network did not drive (dial off, bypass): status shows it idle.
    void markIdle(const char* why);
    // Before the caller destroys or reallocates the features or params buffer.
    void invalidateBindings();
    void destroy();
    [[nodiscard]] Nrd2NetworkStatus status() const { return status_; }

    static constexpr u32 kReloadPollFrames = 120;

private:
    bool ensurePipelines(rhi::IDevice& dev);
    void reload(rhi::IDevice& dev);
    bool fail(const char* why, bool warn);

    rhi::IResourceFactory* res_ = nullptr;
    neural::ConvNet net_;
    rhi::PipelineHandle psoIn_ = 0, psoOut_ = 0;
    rhi::BindingSetHandle setIn_ = 0, setOut_ = 0;
    rhi::BufferHandle out_ = 0;
    u32 outFloats_ = 0;
    rhi::BufferHandle boundFeatures_ = 0, boundParams_ = 0;
    u32 boundFeatureFloats_ = 0, boundParamFloats_ = 0;
    u32 tilesX_ = 0, tilesY_ = 0;
    bool pipelinesTried_ = false, loaded_ = false;
    std::string user_, shipped_;
    std::string loadedPath_;
    i64 loadedStamp_ = 0;
    u32 sincePoll_ = 0;
    f32 inScale_[12] = {}, inBias_[12] = {}, outScale_[12] = {}, outBias_[12] = {};
    std::string warned_;
    Nrd2NetworkStatus status_;
};

}  // namespace aver::render::denoise
