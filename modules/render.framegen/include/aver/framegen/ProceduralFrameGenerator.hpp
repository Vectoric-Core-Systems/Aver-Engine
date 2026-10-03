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
// keeps two more frames of motion history). Until it has trained weights (loaded from the weights file,
// or after kWarmSteps steps) Neural uses the analytic acceleration.
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
    static constexpr u32 kSaveEverySteps = 500;   // and evaluateBatch's cadence
    static constexpr u32 kTrainSamples = 2000;   // records per step; within Mlp::safeBatchLimit() (2047)

private:
    bool ensurePipelines();
    bool ensureTrajectory();            // its pipelines and the network; false = Linear only
    bool ensureTargets(u32 w, u32 h);   // true = usable; recreating forgets the previous frame
    void releaseTargets();
    void bindInputs(const rhi::FrameGenInput& in);
    bool networkReady() const { return weightsLoaded_ || trainSteps_ >= kWarmSteps; }

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
    u32 trainSteps_ = 0;
    u32 frame_ = 0;
    u32 saveAtFrame_ = 0;               // collect + save the readback recorded kSaveEverySteps ago
    bool loggedNetworkLive_ = false;
    // THE EVIDENCE: with each weight save, the batch just trained on is read back too and scored on the
    // CPU (MlpReference, the network's twin) against the two procedural paths -- see evaluateBatch.
    rhi::BufferHandle evalRec_ = 0, evalTgt_ = 0, evalCount_ = 0;   // Readback
    void evaluateBatch();

    bool historyValid_ = false;
    u32 histDepth_ = 0;                 // consecutive real frames of motion history held (0..3)
};

}  // namespace aver::framegen
