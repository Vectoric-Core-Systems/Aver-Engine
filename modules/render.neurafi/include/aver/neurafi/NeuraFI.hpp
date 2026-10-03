// NeuraFI -- frame interpolation (docs/rendering/NEURAFI.md): the
// rhi::IFrameInterpolator the hosts install on the device.
//
// WHAT IT OWNS: the previous real frame (colour, motion, view depth -- copied after every generate()),
// the generated image and a scratch image for the fill, and compute pipelines built from
// shaders/neurafi.hlsl. Everything is scene resolution and recreated on a size change, which also
// forgets the previous frame.
//
// THE TRAJECTORY (Â§3.5). The gather follows either straight lines (Linear: milestone 1, the default),
// a quadratic path through three frames with an analytic acceleration (Quadratic), or the same path
// with the acceleration predicted by a small network (Neural: Aver.Render.Neural's Mlp). The network
// only ever outputs an acceleration -- never colour, a weight, a mask or a confidence -- and is trained
// in-engine, self-supervised, on purely geometric targets taken from ordinary real frames (training
// keeps two more frames of motion history). Neural uses the analytic acceleration until the network has
// weights (loaded, or kWarmSteps steps) AND its measured error beats the analytic one (the gate, below).
//
// Backend-agnostic: only the generic RHI. NOT THREAD-SAFE; generate() records into the context the
// device is recording the frame with.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/render/neural/Mlp.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <string>

namespace aver::neurafi {

enum class Trajectory : u32 { Linear = 0, Quadratic = 1, Neural = 2 };

// What visualisation() shows (neurafi.hlsl's vizColour has the colour legend). Values are the shader's
// gViz.
enum class Visualisation : u32 {
    None = 0,
    Sources = 1,       // which real frame each pixel came from (orange N, blue N-1), holes magenta
    Confidence = 2,    // the blend's confidence as heat, holes magenta
    PathBend = 3,      // how far the curved path moves the in-between point off the straight line
    NetworkShare = 4,  // how much of that bend the network added
};

class NeuraFI final : public rhi::IFrameInterpolator {
public:
    // `dev` must have resources() (the hosts check) and outlive this.
    explicit NeuraFI(rhi::IDevice& dev) : dev_(dev), res_(*dev.resources()) {}
    ~NeuraFI() override;
    NeuraFI(const NeuraFI&)            = delete;
    NeuraFI& operator=(const NeuraFI&) = delete;

    const char* name() const override { return "Aver frame interpolation"; }
    rhi::TextureHandle generate(rhi::IRenderContext& ctx, const rhi::FrameInterpInput& in) override;
    void reset() override { historyValid_ = false; histDepth_ = 0; }

    void setTrajectory(Trajectory t) { trajectory_ = t; }
    Trajectory trajectory() const { return trajectory_; }

    // THE VISUALISATION: a scene-resolution image of display-ready colours (alpha 1), written by the
    // gather while `v` is not None, for the host to draw over its viewport. `bendFullScalePx` is the
    // path bend (PathBend, NetworkShare) shown at full heat. visualisation() is 0 until one was made.
    // vizCount() advances with every generate() that wrote it, so a host can tell a fresh image from
    // one left over from before frame interpolation paused. Rests in ShaderResource (pixel-readable).
    void setVisualisation(Visualisation v, f32 bendFullScalePx) {
        viz_ = v;
        vizScale_ = bendFullScalePx > 1e-3f ? bendFullScalePx : 1e-3f;
    }
    rhi::TextureHandle visualisation() const { return vizTex_; }
    u64 vizCount() const { return vizCount_; }
    // In-engine training of the trajectory network, every frame frame interpolation runs.
    void setTraining(bool on) { training_ = on; }
    // Where the user's trained weights are loaded from at first use and saved to while training
    // ("" = never). Takes precedence over the shipped weights.
    void setWeightsPath(std::string path) { weightsPath_ = std::move(path); }
    // The weights trained by the engine's makers and shipped with it (bin/data), read-only: loaded when
    // the user has none of their own. Its ".steps" sidecar carries the gate's verdict, so a user who
    // never trains still gets the network wherever it was measured to beat the quadratic.
    void setShippedWeightsPath(std::string path) { shippedWeightsPath_ = std::move(path); }
    // The file name both live under. "_v2": the network predicts a CORRECTION to the analytic
    // acceleration since v2; a v1 file (the whole acceleration) has the same shape and must not load.
    static constexpr const char* kWeightsFileName = "neurafi_v2.avnn";

    // Steps of Adam this session; the network is used once this reaches kWarmSteps (or weights loaded).
    static constexpr u32 kWarmSteps = 1500;
    static constexpr u32 kSaveEverySteps = 500;
    static constexpr u32 kEvalEverySteps = 100;   // evaluateBatch's cadence
    static constexpr u32 kTrainSamples = 2000;   // records per step; within Mlp::safeBatchLimit() (2047)

    // THE GATE. One batch's score is noisy (measured: the network ranged 0.045-0.180 px against a steady
    // 0.11 for the quadratic), so both errors are smoothed (EMA, kEvalSmoothing) and the network is
    // used only while ITS smoothed error is below the quadratic's, after kEvalsToJudge checks. The
    // verdict is saved beside the weights, so a session that does not train still knows it. Learned
    // is therefore never worse than Quadratic on the measured data.
    static constexpr f32 kEvalSmoothing = 0.2f;
    static constexpr u32 kEvalsToJudge = 3;
    static constexpr f32 kGateEnter = 0.9f;   // must be 10% better than the quadratic to switch in

    // THE LEARNING-RATE SCHEDULE. Constant-rate Adam kept chasing each new batch: the measured error
    // rose from 0.047 to 0.090 px over 2,000 more steps at 1e-3. Inverse decay over the network's
    // LIFETIME step count (saved beside the weights, so a new session continues where the last
    // stopped instead of restarting hot): lr = kLearningRate / (1 + steps / kDecaySteps), floored.
    static constexpr f32 kLearningRate = 5e-4f;
    static constexpr f32 kDecaySteps = 1000.0f;
    static constexpr f32 kLearningRateFloor = 2e-5f;
    static f32 learningRateAt(u64 lifetimeSteps) {
        const f32 lr = kLearningRate / (1.0f + static_cast<f32>(lifetimeSteps) / kDecaySteps);
        return lr > kLearningRateFloor ? lr : kLearningRateFloor;
    }

    // What the editor shows while it trains.
    struct TrainingStatus {
        u64 lifetimeSteps = 0;   // including the steps behind the loaded weights
        u32 sessionSteps = 0;
        f32 learningRate = 0.0f;
        bool networkInUse = false;
        bool networkBeatsQuadratic = false;   // the gate's verdict
        bool evaluated = false;  // the three errors below are the smoothed evaluateBatch scores
        f32 errLinear = 0.0f, errAnalytic = 0.0f, errNetwork = 0.0f;   // px
    };
    TrainingStatus trainingStatus() const;

private:
    bool ensurePipelines();
    bool ensureTrajectory();            // its pipelines and the network; false = Linear only
    bool ensureTargets(u32 w, u32 h);   // true = usable; recreating forgets the previous frame
    void releaseTargets();
    void bindInputs(const rhi::FrameInterpInput& in);
    bool networkReady() const { return (weightsLoaded_ || trainSteps_ >= kWarmSteps) && beatsQuadratic_; }

    rhi::IDevice& dev_;
    rhi::IResourceFactory& res_;
    bool pipelinesFailed_ = false;      // said once; generate() then returns 0 for good
    bool trajectoryFailed_ = false;     // said once; the trajectory then stays Linear
    rhi::PipelineHandle gatherPso_ = 0, fillPso_ = 0;
    rhi::PipelineHandle featuresPso_ = 0, accelPso_ = 0, clearCountPso_ = 0, trainRecordsPso_ = 0;

    u32 w_ = 0, h_ = 0, qw_ = 0, qh_ = 0;   // scene size; acceleration-image size
    u32 block_ = 2;                         // scene pixels per acceleration texel, each axis
    static constexpr u64 kMaxAccelTexels = 262144;   // ~512x512: the network's per-frame record budget
    // The previous real frame (N-1); with training also N-2 and N-3 (copied down each frame). Rest in
    // NonPixelShaderResource.
    rhi::TextureHandle histColor_ = 0, histVel_ = 0, histZ_ = 0;
    rhi::TextureHandle histVel2_ = 0, histZ2_ = 0, histVel3_ = 0, histZ3_ = 0;
    // The generated image and the fill's ping-pong partner. Rest in ShaderResource.
    rhi::TextureHandle out_ = 0, tmp_ = 0;
    // The acceleration image, one texel per block_ x block_ pixels, and the network's share of it (written
    // only for the NetworkShare visualisation). Both rest in NonPixelShaderResource.
    rhi::TextureHandle accel_ = 0, accelNet_ = 0;
    // The visualisation (see setVisualisation). Rests in ShaderResource.
    rhi::TextureHandle vizTex_ = 0;
    Visualisation viz_ = Visualisation::None;
    f32 vizScale_ = 0.5f;
    u64 vizCount_ = 0;
    // Network I/O, all UAV structured float buffers (count: uint). Common between frames.
    rhi::BufferHandle records_ = 0, netOut_ = 0, trainRec_ = 0, trainTgt_ = 0, trainCount_ = 0;
    // gather: t0-t2 frame N, t3-t5 the previous frame, t6 acceleration, t7 its network share, u0 out_,
    // u1 the visualisation.
    // fillA: out_ -> tmp_; fillB: tmp_ -> out_.  traj: see neurafi.hlsl's trajectory section.
    rhi::BindingSetHandle gatherSet_ = 0, fillSetA_ = 0, fillSetB_ = 0, trajSet_ = 0;
    rhi::TextureHandle boundColor_ = 0, boundVel_ = 0, boundZ_ = 0;   // what gatherSet_/trajSet_ hold

    render::neural::Mlp mlp_;
    Trajectory trajectory_ = Trajectory::Linear;
    bool training_ = false;
    std::string weightsPath_;
    std::string shippedWeightsPath_;
    bool weightsLoaded_ = false;
    u32 trainSteps_ = 0;                // this session
    u64 priorSteps_ = 0;                // behind the loaded weights (weightsPath_ + ".steps")
    f32 errEma_[3] = {};                // evaluateBatch, smoothed: straight, analytic, network (px)
    u32 evals_ = 0;                     // includes the checks behind loaded scores
    u32 sessionEvals_ = 0;
    bool beatsQuadratic_ = false;       // the gate (see kEvalSmoothing)
    u32 frame_ = 0;
    u32 collectAtFrame_ = 0;            // collect (evaluate, maybe save) the readback recorded then
    bool saveOnCollect_ = false;
    bool collectReadsWeights_ = false;  // training: the GPU weights were read back too
    u32 checkFrames_ = 0;               // frames that built check records
    bool loggedNetworkLive_ = false;
    // THE EVIDENCE: with each weight save, the batch just trained on is read back too and scored on the
    // CPU (MlpReference, the network's twin) against the two procedural paths -- see evaluateBatch.
    rhi::BufferHandle evalRec_ = 0, evalTgt_ = 0, evalCount_ = 0;   // Readback
    void evaluateBatch();

    bool historyValid_ = false;
    u32 histDepth_ = 0;                 // consecutive real frames of motion history held (0..3)
};

}  // namespace aver::neurafi
