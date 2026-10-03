// ProceduralFrameGenerator -- frame interpolation (docs/rendering/FRAME_INTERPOLATION.md): the
// rhi::IFrameGenerator the hosts install on the device.
//
// WHAT IT OWNS: the previous real frame (colour, motion, view depth -- copied after every generate()),
// the generated image and a scratch image for the fill, and compute pipelines built from
// shaders/framegen.hlsl. Everything is scene resolution and recreated on a size change, which also
// forgets the previous frame.
//
// THE TRAJECTORY (§3.5). The gather follows either straight lines (Linear: milestone 1, the default),
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

namespace aver::framegen {

enum class Trajectory : u32 { Linear = 0, Quadratic = 1, Neural = 2 };

class ProceduralFrameGenerator final : public rhi::IFrameGenerator {
public:
    // `dev` must have resources() (the hosts check) and outlive this.
    explicit ProceduralFrameGenerator(rhi::IDevice& dev) : dev_(dev), res_(*dev.resources()) {}
    ~ProceduralFrameGenerator() override;
    ProceduralFrameGenerator(const ProceduralFrameGenerator&)            = delete;
    ProceduralFrameGenerator& operator=(const ProceduralFrameGenerator&) = delete;

    const char* name() const override { return "Aver frame interpolation"; }
    rhi::TextureHandle generate(rhi::IRenderContext& ctx, const rhi::FrameGenInput& in) override;
    void reset() override { historyValid_ = false; histDepth_ = 0; }

    void setTrajectory(Trajectory t) { trajectory_ = t; }
    Trajectory trajectory() const { return trajectory_; }
    // In-engine training of the trajectory network, every frame frame generation runs.
    void setTraining(bool on) { training_ = on; }
    // Where the trained weights are loaded from at first use and saved to while training ("" = never).
    void setWeightsPath(std::string path) { weightsPath_ = std::move(path); }

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
    void bindInputs(const rhi::FrameGenInput& in);
    bool networkReady() const { return (weightsLoaded_ || trainSteps_ >= kWarmSteps) && beatsQuadratic_; }

    rhi::IDevice& dev_;
    rhi::IResourceFactory& res_;
    bool pipelinesFailed_ = false;      // said once; generate() then returns 0 for good
    bool trajectoryFailed_ = false;     // said once; the trajectory then stays Linear
    rhi::PipelineHandle gatherPso_ = 0, fillPso_ = 0;
    rhi::PipelineHandle featuresPso_ = 0, accelPso_ = 0, clearCountPso_ = 0, trainRecordsPso_ = 0;

    u32 w_ = 0, h_ = 0, qw_ = 0, qh_ = 0;
    // The previous real frame (N-1); with training also N-2 and N-3 (copied down each frame). Rest in
    // NonPixelShaderResource.
    rhi::TextureHandle histColor_ = 0, histVel_ = 0, histZ_ = 0;
    rhi::TextureHandle histVel2_ = 0, histZ2_ = 0, histVel3_ = 0, histZ3_ = 0;
    // The generated image and the fill's ping-pong partner. Rest in ShaderResource.
    rhi::TextureHandle out_ = 0, tmp_ = 0;
    // Quarter-resolution acceleration image. Rests in NonPixelShaderResource.
    rhi::TextureHandle accel_ = 0;
    // Network I/O, all UAV structured float buffers (count: uint). Common between frames.
    rhi::BufferHandle records_ = 0, netOut_ = 0, trainRec_ = 0, trainTgt_ = 0, trainCount_ = 0;
    // gather: t0-t2 frame N, t3-t5 the previous frame, t6 acceleration, u0 out_.
    // fillA: out_ -> tmp_; fillB: tmp_ -> out_.  traj: see framegen.hlsl's trajectory section.
    rhi::BindingSetHandle gatherSet_ = 0, fillSetA_ = 0, fillSetB_ = 0, trajSet_ = 0;
    rhi::TextureHandle boundColor_ = 0, boundVel_ = 0, boundZ_ = 0;   // what gatherSet_/trajSet_ hold

    render::neural::Mlp mlp_;
    Trajectory trajectory_ = Trajectory::Linear;
    bool training_ = false;
    std::string weightsPath_;
    bool weightsLoaded_ = false;
    u32 trainSteps_ = 0;                // this session
    u64 priorSteps_ = 0;                // behind the loaded weights (weightsPath_ + ".steps")
    f32 errEma_[3] = {};                // evaluateBatch, smoothed: straight, analytic, network (px)
    u32 evals_ = 0;
    bool beatsQuadratic_ = false;       // the gate (see kEvalSmoothing)
    u32 frame_ = 0;
    u32 collectAtFrame_ = 0;            // collect (evaluate, maybe save) the readback recorded then
    bool saveOnCollect_ = false;
    bool loggedNetworkLive_ = false;
    // THE EVIDENCE: with each weight save, the batch just trained on is read back too and scored on the
    // CPU (MlpReference, the network's twin) against the two procedural paths -- see evaluateBatch.
    rhi::BufferHandle evalRec_ = 0, evalTgt_ = 0, evalCount_ = 0;   // Readback
    void evaluateBatch();

    bool historyValid_ = false;
    u32 histDepth_ = 0;                 // consecutive real frames of motion history held (0..3)
};

}  // namespace aver::framegen
