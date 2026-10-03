#include "aver/neurafi/NeuraFI.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace aver::neurafi {

namespace {

constexpr const char* kShaderName = "neurafi.hlsl";
constexpr u32 kGroup = 8;           // neurafi.hlsl's FG_GROUP
constexpr u32 kConstantSlot = 1;    // b1: FgConstants
constexpr u32 kConstantDwords = 8;  // FgConstants, the same for every pass
constexpr u32 kRecordFloats = 14;   // neurafi.hlsl's FG_RECORD
constexpr u32 kOutputFloats = 2;    // FG_OUTPUT

struct FgConstants {
    u32 size[2];
    u32 last;
    u32 traj;
    u32 mode;
    u32 frame;
    u32 samples;
    u32 block;      // acceleration-image block size (pixels per texel, each axis)
};
static_assert(sizeof(FgConstants) == kConstantDwords * 4, "FgConstants mirrors neurafi.hlsl's cbuffer");

using RS = rhi::ResourceState;

// The trajectory network: the record and output sizes are neurafi.hlsl's; two hidden layers of 32.
render::neural::MlpDesc networkDesc() {
    render::neural::MlpDesc d;
    d.inputs = kRecordFloats;
    d.outputs = kOutputFloats;
    d.hiddenWidth = 32;
    d.hiddenLayers = 2;
    d.hidden = render::neural::Activation::ReLU;
    d.output = render::neural::Activation::None;
    d.seed = 0x46474E31u;   // "FGN1"
    return d;
}

}  // namespace

NeuraFI::~NeuraFI() {
    releaseTargets();
    mlp_.destroy();
    for (rhi::PipelineHandle* p : {&gatherPso_, &fillPso_, &featuresPso_, &accelPso_, &clearCountPso_,
                                   &trainRecordsPso_}) {
        if (*p) res_.destroyPipeline(*p);
        *p = 0;
    }
}

bool NeuraFI::ensurePipelines() {
    if (gatherPso_ && fillPso_) return true;
    if (pipelinesFailed_) return false;
    auto fail = [&](const char* why) {
        AVER_WARN("[NeuraFI] {}; frame interpolation stays off", why);
        pipelinesFailed_ = true;
        return false;
    };
    const std::string& source = rhi::shaderFile(kShaderName);
    if (source.empty()) return fail("neurafi.hlsl is not deployed beside the executable");

    auto build = [&](const char* entry, const char* define, u32 srvs, bool sampler) -> rhi::PipelineHandle {
        rhi::ShaderDesc sd{};
        sd.source = source.c_str();
        sd.entry = entry;
        sd.stage = rhi::ShaderStage::Compute;
        sd.minShaderModel = 60;
        sd.defines = define;
        const rhi::ShaderHandle cs = res_.createShader(sd);
        if (!cs) return 0;
        rhi::ComputePipelineDesc pd{};
        pd.cs = cs;
        pd.layout.srvCount = srvs;
        pd.layout.uavCount = 1;
        pd.layout.slotKindsDeclared = true;   // every slot is a Texture2D (the default kind)
        pd.layout.constantDwords[kConstantSlot] = kConstantDwords;
        if (sampler) {
            pd.layout.samplers[0] = rhi::SamplerDesc{rhi::Filter::Linear, rhi::AddressMode::Clamp};
            pd.layout.samplerCount = 1;
        }
        const rhi::PipelineHandle p = res_.createComputePipeline(pd);
        res_.destroyShader(cs);
        return p;
    };
    gatherPso_ = build("CSFgGather", "FG_GATHER=1", 7, true);
    fillPso_   = build("CSFgFill", "FG_FILL=1", 1, false);
    if (!gatherPso_ || !fillPso_) return fail("the frame interpolation shaders would not compile");
    AVER_INFO("[NeuraFI] procedural interpolation ready (gather + 2 full-resolution fill passes)");
    return true;
}

// The four trajectory passes share one layout: t0-t7 motion and depth of frames N..N-3, u0 inference
// records, u1 the acceleration image, u2-u4 training records/targets/count, u5 the network's outputs.
bool NeuraFI::ensureTrajectory() {
    if (featuresPso_ && accelPso_ && clearCountPso_ && trainRecordsPso_ && mlp_.valid()) return true;
    if (trajectoryFailed_) return false;
    auto fail = [&](const char* why) {
        AVER_WARN("[NeuraFI] {}; the trajectory stays a straight line", why);
        trajectoryFailed_ = true;
        return false;
    };
    const std::string& source = rhi::shaderFile(kShaderName);
    if (source.empty()) return fail("neurafi.hlsl is not deployed beside the executable");
    auto build = [&](const char* entry) -> rhi::PipelineHandle {
        rhi::ShaderDesc sd{};
        sd.source = source.c_str();
        sd.entry = entry;
        sd.stage = rhi::ShaderStage::Compute;
        sd.minShaderModel = 60;
        sd.defines = "FG_TRAJ=1";
        const rhi::ShaderHandle cs = res_.createShader(sd);
        if (!cs) return 0;
        rhi::ComputePipelineDesc pd{};
        pd.cs = cs;
        pd.layout.srvCount = 8;
        pd.layout.uavCount = 6;
        pd.layout.slotKindsDeclared = true;
        pd.layout.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
        pd.layout.uavKinds[1] = rhi::SlotKind::Texture2D;
        pd.layout.uavKinds[2] = rhi::SlotKind::StructuredBuffer;
        pd.layout.uavKinds[3] = rhi::SlotKind::StructuredBuffer;
        pd.layout.uavKinds[4] = rhi::SlotKind::StructuredBuffer;
        pd.layout.uavKinds[5] = rhi::SlotKind::StructuredBuffer;
        pd.layout.constantDwords[kConstantSlot] = kConstantDwords;
        const rhi::PipelineHandle p = res_.createComputePipeline(pd);
        res_.destroyShader(cs);
        return p;
    };
    if (!featuresPso_) featuresPso_ = build("CSFgFeatures");
    if (!accelPso_) accelPso_ = build("CSFgAccel");
    if (!clearCountPso_) clearCountPso_ = build("CSFgClearCount");
    if (!trainRecordsPso_) trainRecordsPso_ = build("CSFgTrainRecords");
    if (!featuresPso_ || !accelPso_ || !clearCountPso_ || !trainRecordsPso_)
        return fail("the trajectory shaders would not compile");

    if (!mlp_.valid()) {
        render::neural::OptimiserDesc opt;
        if (!mlp_.create(dev_, networkDesc(), opt)) return fail("the trajectory network could not be created");
        // The user's own trained weights win; otherwise the ones shipped with the engine. Training only
        // ever writes the user's file.
        std::error_code ec;
        std::string from;
        if (!weightsPath_.empty() && std::filesystem::exists(weightsPath_, ec)) from = weightsPath_;
        else if (!shippedWeightsPath_.empty() && std::filesystem::exists(shippedWeightsPath_, ec))
            from = shippedWeightsPath_;
        if (!from.empty()) {
            weightsLoaded_ = mlp_.loadWeights(from);
            // The lifetime step count the learning-rate schedule continues from (absent: 0).
            // Sidecar: "steps [quadratic-error network-error]" -- the smoothed scores the gate last
            // judged by. Without them the network waits to be judged again (the quadratic stands in).
            if (weightsLoaded_) {
                std::ifstream steps(from + ".steps");
                if (!(steps >> priorSteps_)) priorSteps_ = 0;
                f32 quad = 0.0f, net = 0.0f;
                if (steps >> quad >> net) {
                    errEma_[1] = quad;
                    errEma_[2] = net;
                    evals_ = kEvalsToJudge;
                    beatsQuadratic_ = net < quad;
                }
            }
            AVER_INFO("[NeuraFI] network weights {} from {} ({} steps behind them)",
                      weightsLoaded_ ? "loaded" : "NOT loaded (shape mismatch?)", from, priorSteps_);
        } else {
            AVER_INFO("[NeuraFI] NeuraFI: no trained weights at '{}' or '{}'; starting from the quadratic",
                      weightsPath_, shippedWeightsPath_);
        }
        if (!weightsLoaded_) {
            // A FRESH network starts as exactly the analytic path: its output layer (the correction) is
            // zeroed, so it can only learn improvements on the quadratic. MEASURED without this: the
            // random He-init correction started at 0.47 px against the quadratic's 0.087 and was still
            // worse (0.13) 2,500 steps later. The hidden layers keep their init, so gradients reach them
            // as soon as the output layer moves.
            const render::neural::MlpLayout lay = render::neural::MlpLayout::make(networkDesc());
            std::vector<f32> w(mlp_.cpuWeights(false).begin(), mlp_.cpuWeights(false).end());
            const u32 last = lay.layers - 1;
            for (u32 i = 0; i < lay.layerSize[last]; ++i) w[lay.wOffset[last] + i] = 0.0f;
            mlp_.uploadWeights(w);
        }
        AVER_INFO("[NeuraFI] network {}->{}x{}->{} ready; {}", kRecordFloats, 32, 2, kOutputFloats,
                  beatsQuadratic_ ? "the loaded weights beat the quadratic and are used"
                                  : "the analytic acceleration stands in until the live gate has judged it on this motion");
    }
    return true;
}

void NeuraFI::releaseTargets() {
    // The network caches binding sets on our buffers: drop them before the buffers go.
    mlp_.invalidateBindings();
    for (rhi::BindingSetHandle* s : {&gatherSet_, &fillSetA_, &fillSetB_, &trajSet_}) {
        if (*s) res_.destroyBindingSet(*s);
        *s = 0;
    }
    for (rhi::TextureHandle* t : {&histColor_, &histVel_, &histZ_, &histVel2_, &histZ2_, &histVel3_, &histZ3_,
                                  &out_, &tmp_, &accel_}) {
        if (*t) res_.destroyTexture(*t);
        *t = 0;
    }
    for (rhi::BufferHandle* b : {&records_, &netOut_, &trainRec_, &trainTgt_, &trainCount_, &evalRec_, &evalTgt_,
                                 &evalCount_}) {
        if (*b) res_.destroyBuffer(*b);
        *b = 0;
    }
    boundColor_ = boundVel_ = boundZ_ = 0;
    w_ = h_ = qw_ = qh_ = 0;
    historyValid_ = false;
    histDepth_ = 0;
}

bool NeuraFI::ensureTargets(u32 w, u32 h) {
    if (w == w_ && h == h_ && out_) return true;
    releaseTargets();
    // The acceleration image's block: the smallest power of two keeping it within kMaxAccelTexels, so
    // the network's per-frame cost stays roughly flat at any scene size (2x2 up to ~1080p).
    u32 block = 2;
    while (static_cast<u64>((w + block - 1) / block) * ((h + block - 1) / block) > kMaxAccelTexels) block *= 2;
    const u32 qw = (w + block - 1) / block, qh = (h + block - 1) / block;

    auto make = [&](u32 tw, u32 th, rhi::Format f, bool uav, RS state, const char* name) {
        rhi::TextureDesc td{};
        td.width = tw;
        td.height = th;
        td.format = f;
        td.bind = uav ? (rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess)
                      : rhi::ResourceBind::ShaderResource;
        td.initialState = state;
        td.debugName = name;
        return res_.createTexture(td);
    };
    histColor_ = make(w, h, rhi::Format::RGBA16F, false, RS::NonPixelShaderResource, "FrameInterp N-1 colour");
    histVel_   = make(w, h, rhi::Format::RG16F, false, RS::NonPixelShaderResource, "FrameInterp N-1 motion");
    histZ_     = make(w, h, rhi::Format::R32Float, false, RS::NonPixelShaderResource, "FrameInterp N-1 view depth");
    histVel2_  = make(w, h, rhi::Format::RG16F, false, RS::NonPixelShaderResource, "FrameInterp N-2 motion");
    histZ2_    = make(w, h, rhi::Format::R32Float, false, RS::NonPixelShaderResource, "FrameInterp N-2 view depth");
    histVel3_  = make(w, h, rhi::Format::RG16F, false, RS::NonPixelShaderResource, "FrameInterp N-3 motion");
    histZ3_    = make(w, h, rhi::Format::R32Float, false, RS::NonPixelShaderResource, "FrameInterp N-3 view depth");
    out_       = make(w, h, rhi::Format::RGBA16F, true, RS::ShaderResource, "FrameInterp generated frame");
    tmp_       = make(w, h, rhi::Format::RGBA16F, true, RS::ShaderResource, "FrameInterp fill scratch");
    accel_     = make(qw, qh, rhi::Format::RG16F, true, RS::NonPixelShaderResource, "FrameInterp acceleration");
    auto buffer = [&](u64 bytes, const char* name) {
        rhi::BufferDesc bd;
        bd.bytes = bytes;
        bd.kind = rhi::BufferKind::Default;
        bd.allowUnorderedAccess = true;
        bd.debugName = name;
        return res_.createBuffer(bd);
    };
    const u64 qCount = static_cast<u64>(qw) * qh;
    records_    = buffer(qCount * kRecordFloats * 4, "FrameInterp trajectory records");
    netOut_     = buffer(qCount * kOutputFloats * 4, "FrameInterp trajectory network outputs");
    trainRec_   = buffer(static_cast<u64>(kTrainSamples) * kRecordFloats * 4, "FrameInterp training records");
    trainTgt_   = buffer(static_cast<u64>(kTrainSamples) * kOutputFloats * 4, "FrameInterp training targets");
    trainCount_ = buffer(16, "FrameInterp training count");
    auto readback = [&](u64 bytes, const char* name) {
        rhi::BufferDesc bd;
        bd.bytes = bytes;
        bd.kind = rhi::BufferKind::Readback;
        bd.debugName = name;
        return res_.createBuffer(bd);
    };
    evalRec_   = readback(static_cast<u64>(kTrainSamples) * kRecordFloats * 4, "FrameInterp evaluation records");
    evalTgt_   = readback(static_cast<u64>(kTrainSamples) * kOutputFloats * 4, "FrameInterp evaluation targets");
    evalCount_ = readback(16, "FrameInterp evaluation count");
    if (!histColor_ || !histVel_ || !histZ_ || !histVel2_ || !histZ2_ || !histVel3_ || !histZ3_ || !out_ ||
        !tmp_ || !accel_ || !records_ || !netOut_ || !trainRec_ || !trainTgt_ || !trainCount_ || !evalRec_ ||
        !evalTgt_ || !evalCount_) {
        AVER_WARN("[NeuraFI] could not create the {}x{} targets", w, h);
        releaseTargets();
        return false;
    }

    rhi::BindingSetDesc gd;
    gd.srvCount = 7;
    gd.uavCount = 1;
    gatherSet_ = res_.createBindingSet(gd);
    rhi::BindingSetDesc fd;
    fd.srvCount = 1;
    fd.uavCount = 1;
    fillSetA_ = res_.createBindingSet(fd);
    fillSetB_ = res_.createBindingSet(fd);
    rhi::BindingSetDesc td;
    td.srvCount = 8;
    td.uavCount = 6;
    td.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    td.uavKinds[1] = rhi::SlotKind::Texture2D;
    td.uavKinds[2] = rhi::SlotKind::StructuredBuffer;
    td.uavKinds[3] = rhi::SlotKind::StructuredBuffer;
    td.uavKinds[4] = rhi::SlotKind::StructuredBuffer;
    td.uavKinds[5] = rhi::SlotKind::StructuredBuffer;
    trajSet_ = res_.createBindingSet(td);
    if (!gatherSet_ || !fillSetA_ || !fillSetB_ || !trajSet_) {
        AVER_WARN("[NeuraFI] could not create the binding sets");
        releaseTargets();
        return false;
    }
    res_.setSrv(gatherSet_, 3, histColor_);
    res_.setSrv(gatherSet_, 4, histVel_);
    res_.setSrv(gatherSet_, 5, histZ_);
    res_.setSrv(gatherSet_, 6, accel_);
    res_.setUav(gatherSet_, 0, out_, 0);
    res_.setSrv(fillSetA_, 0, out_);
    res_.setUav(fillSetA_, 0, tmp_, 0);
    res_.setSrv(fillSetB_, 0, tmp_);
    res_.setUav(fillSetB_, 0, out_, 0);
    res_.setSrv(trajSet_, 2, histVel_);
    res_.setSrv(trajSet_, 3, histZ_);
    res_.setSrv(trajSet_, 4, histVel2_);
    res_.setSrv(trajSet_, 5, histZ2_);
    res_.setSrv(trajSet_, 6, histVel3_);
    res_.setSrv(trajSet_, 7, histZ3_);
    res_.setUavBuffer(trajSet_, 0, records_, 4, static_cast<u32>(qCount * kRecordFloats), 0);
    res_.setUav(trajSet_, 1, accel_, 0);
    res_.setUavBuffer(trajSet_, 2, trainRec_, 4, kTrainSamples * kRecordFloats, 0);
    res_.setUavBuffer(trajSet_, 3, trainTgt_, 4, kTrainSamples * kOutputFloats, 0);
    res_.setUavBuffer(trajSet_, 4, trainCount_, 4, 4, 0);
    res_.setUavBuffer(trajSet_, 5, netOut_, 4, static_cast<u32>(qCount * kOutputFloats), 0);

    w_ = w;
    h_ = h;
    qw_ = qw;
    qh_ = qh;
    block_ = block;
    const f64 mib = (static_cast<f64>(w) * h * (8 + 4 + 4 + 8 + 8 + 2 * (4 + 4)) +
                     static_cast<f64>(qCount) * (4 + 4 * (kRecordFloats + kOutputFloats))) / (1024.0 * 1024.0);
    AVER_INFO("[NeuraFI] targets {}x{} ({:.1f} MiB)", w, h, mib);
    return true;
}

// Scores one read-back training batch three ways, as the error in the predicted in-between POSITION
// (0.125 |a - a_true| pixels, the quadratic's own term): a straight line (a = 0), the analytic
// acceleration (v - v', exact for constant acceleration), and the network (the CPU twin running the
// EMA weights inference uses). The batch was trained on once, so the network's figure is slightly
// flattering; the comparison is still the one that says whether it earns its cost.
void NeuraFI::evaluateBatch() {
    u32 counts[2] = {};   // records written (before the cap), outliers rejected
    if (!res_.readBuffer(evalCount_, counts, 8, 0)) return;
    const u32 rejected = counts[1];
    u32 count = counts[0] > kTrainSamples ? kTrainSamples : counts[0];
    if (count == 0) return;
    std::vector<f32> rec(static_cast<usize>(count) * kRecordFloats), tgt(static_cast<usize>(count) * kOutputFloats);
    if (!res_.readBuffer(evalRec_, rec.data(), rec.size() * 4, 0) ||
        !res_.readBuffer(evalTgt_, tgt.data(), tgt.size() * 4, 0))
        return;
    render::neural::MlpReference net(networkDesc(), render::neural::OptimiserDesc{});
    if (!net.setWeights(mlp_.cpuWeights(true))) return;
    f64 errLinear = 0.0, errAnalytic = 0.0, errNet = 0.0;
    for (u32 i = 0; i < count; ++i) {
        const f32* r = &rec[static_cast<usize>(i) * kRecordFloats];
        const f32* t = &tgt[static_cast<usize>(i) * kOutputFloats];
        const f64 s = std::exp2(static_cast<f64>(r[5]) * 8.0);   // record [5] is log2(s)/8
        f32 out[kOutputFloats];
        net.forward(std::span<const f32>(r, kRecordFloats), std::span<f32>(out, kOutputFloats));
        // Everything is compared as a CORRECTION to the analytic acceleration, which is what the target
        // is: the analytic path predicts 0, a straight line predicts minus the analytic acceleration
        // (a = 0 overall), and the network predicts its output.
        auto err = [&](f64 cx, f64 cy) { return 0.125 * s * std::sqrt((cx - t[0]) * (cx - t[0]) + (cy - t[1]) * (cy - t[1])); };
        errLinear += err(-(static_cast<f64>(r[0]) - r[2]), -(static_cast<f64>(r[1]) - r[3]));
        errAnalytic += err(0.0, 0.0);
        errNet += err(out[0], out[1]);
    }
    const f64 n = static_cast<f64>(count);
    const f32 now[3] = {static_cast<f32>(errLinear / n), static_cast<f32>(errAnalytic / n),
                        static_cast<f32>(errNet / n)};
    for (u32 i = 0; i < 3; ++i)
        errEma_[i] = evals_ == 0 ? now[i] : errEma_[i] + kEvalSmoothing * (now[i] - errEma_[i]);
    ++evals_;
    // The gate: judged only once the smoothed scores rest on a few checks.
    // Hysteresis: in only at kGateEnter of the quadratic's error, out as soon as it is worse -- without
    // it the path flapped every few checks while the two scores were within noise of each other.
    if (evals_ >= kEvalsToJudge) {
        const bool beats = beatsQuadratic_ ? errEma_[2] <= errEma_[1] : errEma_[2] < kGateEnter * errEma_[1];
        if (beats != beatsQuadratic_)
            AVER_INFO("[NeuraFI] {} (smoothed: network {:.3f} px, quadratic {:.3f} px)",
                      beats ? "now in use" : "set aside, the quadratic is better", errEma_[2], errEma_[1]);
        beatsQuadratic_ = beats;
    }
    // Logged at the save cadence (every 5th check), so a long session's log stays readable -- and the
    // session's FIRST check, taken after only kEvalEverySteps steps of adapting to this session's motion:
    // with loaded weights that is the nearest thing to a held-out test of how they generalise.
    ++sessionEvals_;
    if (saveOnCollect_ || sessionEvals_ == 1 || (!training_ && sessionEvals_ % 5 == 0))
        AVER_INFO("[NeuraFI] check at step {} ({} lifetime, learning rate {:.2e}), mean in-between "
                  "position error px -- this batch: straight {:.3f}, analytic {:.3f}, network {:.3f}; smoothed: "
                  "analytic {:.3f}, network {:.3f}; {} records, {} seam outliers rejected",
                  trainSteps_, priorSteps_ + trainSteps_, learningRateAt(priorSteps_ + trainSteps_), now[0],
                  now[1], now[2], errEma_[1], errEma_[2], count, rejected);
}

NeuraFI::TrainingStatus NeuraFI::trainingStatus() const {
    TrainingStatus s;
    s.lifetimeSteps = priorSteps_ + trainSteps_;
    s.sessionSteps = trainSteps_;
    s.learningRate = learningRateAt(s.lifetimeSteps);
    s.networkInUse = trajectory_ == Trajectory::Neural && mlp_.valid() && networkReady();
    s.networkBeatsQuadratic = beatsQuadratic_;
    s.evaluated = evals_ > 0;
    s.errLinear = errEma_[0];
    s.errAnalytic = errEma_[1];
    s.errNetwork = errEma_[2];
    return s;
}

// Frame N's inputs keep their handles from frame to frame (the device's own textures), so the sets are
// rewritten only when one changes -- a resize, after which the device has idled the GPU.
void NeuraFI::bindInputs(const rhi::FrameInterpInput& in) {
    if (in.color != boundColor_) { res_.setSrv(gatherSet_, 0, in.color); boundColor_ = in.color; }
    if (in.velocity != boundVel_) {
        res_.setSrv(gatherSet_, 1, in.velocity);
        res_.setSrv(trajSet_, 0, in.velocity);
        boundVel_ = in.velocity;
    }
    if (in.viewZ != boundZ_) {
        res_.setSrv(gatherSet_, 2, in.viewZ);
        res_.setSrv(trajSet_, 1, in.viewZ);
        boundZ_ = in.viewZ;
    }
}

rhi::TextureHandle NeuraFI::generate(rhi::IRenderContext& ctx, const rhi::FrameInterpInput& in) {
    if (!in.color || !in.velocity || !in.viewZ || !in.width || !in.height) return 0;
    if (!ensurePipelines()) return 0;
    if (!ensureTargets(in.width, in.height)) return 0;
    bindInputs(in);
    ++frame_;

    // A readback recorded four frames ago has certainly finished (frames in flight are 2).
    if (collectAtFrame_ && frame_ >= collectAtFrame_) {
        collectAtFrame_ = 0;
        if (!collectReadsWeights_ || mlp_.collectWeights()) {
            evaluateBatch();
            if (saveOnCollect_ && !weightsPath_.empty()) {
                bool ok = mlp_.saveWeights(weightsPath_);
                if (ok) {
                    std::ofstream steps(weightsPath_ + ".steps", std::ios::trunc);
                    steps << (priorSteps_ + trainSteps_);
                    if (evals_ >= kEvalsToJudge) steps << ' ' << errEma_[1] << ' ' << errEma_[2];
                    steps << '\n';
                    ok = static_cast<bool>(steps);
                }
                AVER_INFO("[NeuraFI] network: {} steps trained ({} lifetime), weights {} {}",
                          trainSteps_, priorSteps_ + trainSteps_, ok ? "saved to" : "could NOT be saved to",
                          weightsPath_);
            }
        }
    }

    const bool cut = in.sceneCut || !historyValid_;
    if (cut) histDepth_ = 0;
    const bool trajOk = (trajectory_ != Trajectory::Linear || training_) && ensureTrajectory();
    const bool bend = trajOk && trajectory_ != Trajectory::Linear && !cut;
    // THE LIVE GATE: whenever Learned is chosen, the three-frame check records are built from THIS
    // user's own frames and the network is scored on them, trained or not. MEASURED why: weights
    // trained on fast jitter scored 0.262 px on slow wide pans the quadratic got to 0.010 -- a verdict
    // from someone else's motion does not transfer. Training, when on, is one more step on the same
    // records.
    const bool keepHistory = trajOk && (training_ || trajectory_ == Trajectory::Neural);
    const bool records = keepHistory && histDepth_ >= 3;
    const bool train = records && training_;
    const bool neural = bend && trajectory_ == Trajectory::Neural && networkReady();

    const u32 gx = (in.width + kGroup - 1) / kGroup, gy = (in.height + kGroup - 1) / kGroup;
    const u32 qgx = (qw_ + kGroup - 1) / kGroup, qgy = (qh_ + kGroup - 1) / kGroup;
    FgConstants k{{in.width, in.height}, 0, bend ? 1u : 0u, neural ? 1u : 0u, frame_, kTrainSamples, block_};

    // Frame N becomes compute-readable (a pixel-shader read state does not cover compute).
    ctx.textureBarrier(in.color, RS::ShaderResource, RS::NonPixelShaderResource);
    ctx.textureBarrier(in.velocity, RS::RenderTarget, RS::NonPixelShaderResource);
    ctx.textureBarrier(in.viewZ, RS::RenderTarget, RS::NonPixelShaderResource);
    const rhi::BufferHandle bufs[] = {records_, netOut_, trainRec_, trainTgt_, trainCount_};
    if (bend || records)
        for (rhi::BufferHandle b : bufs) ctx.bufferBarrier(b, RS::Common, RS::UnorderedAccess);

    rhi::TextureHandle result = 0;
    if (!cut) {
        rhi::ScopedGpuStat stat(ctx, "Frame interpolation");
        if (bend) {
            if (neural) {
                ctx.setPipeline(featuresPso_);
                ctx.setBindingSet(trajSet_);
                ctx.setConstants(kConstantSlot, &k, kConstantDwords);
                ctx.dispatch(qgx, qgy, 1);
                ctx.uavBarrierBuffer(records_);
                render::neural::IoStates io;
                io.input = RS::UnorderedAccess;
                io.output = RS::UnorderedAccess;
                mlp_.recordInfer(ctx, records_, netOut_, render::neural::CpuCount{qw_ * qh_}, true, io);
                ctx.uavBarrierBuffer(netOut_);
                if (!loggedNetworkLive_) {
                    loggedNetworkLive_ = true;
                    AVER_INFO("[NeuraFI] network live ({})",
                              weightsLoaded_ ? "loaded weights" : "trained this session");
                }
            }
            ctx.textureBarrier(accel_, RS::NonPixelShaderResource, RS::UnorderedAccess);
            ctx.setPipeline(accelPso_);
            ctx.setBindingSet(trajSet_);
            ctx.setConstants(kConstantSlot, &k, kConstantDwords);
            ctx.dispatch(qgx, qgy, 1);
            ctx.textureBarrier(accel_, RS::UnorderedAccess, RS::NonPixelShaderResource);
        }

        ctx.textureBarrier(out_, RS::ShaderResource, RS::UnorderedAccess);
        ctx.setPipeline(gatherPso_);
        ctx.setBindingSet(gatherSet_);
        ctx.setConstants(kConstantSlot, &k, kConstantDwords);
        ctx.dispatch(gx, gy, 1);

        // Fill, twice: out_ -> tmp_ -> out_.
        ctx.textureBarrier(out_, RS::UnorderedAccess, RS::NonPixelShaderResource);
        ctx.textureBarrier(tmp_, RS::ShaderResource, RS::UnorderedAccess);
        ctx.setPipeline(fillPso_);
        ctx.setBindingSet(fillSetA_);
        ctx.setConstants(kConstantSlot, &k, kConstantDwords);
        ctx.dispatch(gx, gy, 1);

        ctx.textureBarrier(tmp_, RS::UnorderedAccess, RS::NonPixelShaderResource);
        ctx.textureBarrier(out_, RS::NonPixelShaderResource, RS::UnorderedAccess);
        k.last = 1;
        ctx.setBindingSet(fillSetB_);
        ctx.setConstants(kConstantSlot, &k, kConstantDwords);
        ctx.dispatch(gx, gy, 1);
        k.last = 0;

        ctx.textureBarrier(out_, RS::UnorderedAccess, RS::ShaderResource);
        ctx.textureBarrier(tmp_, RS::NonPixelShaderResource, RS::ShaderResource);
        result = out_;
    }

    // ---- the check records from frames N..N-3 (the shader says how): scored always, trained on if on ----
    if (records) {
        rhi::ScopedGpuStat stat(ctx, train ? "Frame interpolation training" : "Frame interpolation check");
        ctx.setPipeline(clearCountPso_);
        ctx.setBindingSet(trajSet_);
        ctx.setConstants(kConstantSlot, &k, kConstantDwords);
        ctx.dispatch(1, 1, 1);
        ctx.uavBarrierBuffer(trainCount_);
        ctx.setPipeline(trainRecordsPso_);
        ctx.setBindingSet(trajSet_);
        ctx.setConstants(kConstantSlot, &k, kConstantDwords);
        ctx.dispatch((kTrainSamples + 63) / 64, 1, 1);
        ctx.uavBarrierBuffer(trainRec_);
        ctx.uavBarrierBuffer(trainTgt_);
        ctx.uavBarrierBuffer(trainCount_);
        if (train) {
            render::neural::IoStates io;
            io.input = RS::UnorderedAccess;
            io.output = RS::UnorderedAccess;
            mlp_.setLearningRate(learningRateAt(priorSteps_ + trainSteps_));
            if (mlp_.recordTrain(ctx, trainRec_, trainTgt_, trainCount_, kTrainSamples, io)) {
                ++trainSteps_;
                if (trainSteps_ == kWarmSteps && !weightsLoaded_)
                    AVER_INFO("[NeuraFI] network warmed up ({} steps); used while it beats the "
                              "quadratic", trainSteps_);
            }
        }
        // Every kEvalEverySteps frames of records: copy this batch out for evaluateBatch. While training,
        // the weights moved on the GPU and are read back too; otherwise the CPU copies are current.
        ++checkFrames_;
        if (checkFrames_ % kEvalEverySteps == 0 && collectAtFrame_ == 0 && (!training_ || mlp_.recordReadback(ctx))) {
            const rhi::BufferHandle src[] = {trainRec_, trainTgt_, trainCount_};
            for (rhi::BufferHandle b : src) ctx.bufferBarrier(b, RS::UnorderedAccess, RS::CopySource);
            ctx.copyBuffer(evalRec_, trainRec_, static_cast<u64>(kTrainSamples) * kRecordFloats * 4);
            ctx.copyBuffer(evalTgt_, trainTgt_, static_cast<u64>(kTrainSamples) * kOutputFloats * 4);
            ctx.copyBuffer(evalCount_, trainCount_, 8);   // records written, outliers rejected
            for (rhi::BufferHandle b : src) ctx.bufferBarrier(b, RS::CopySource, RS::UnorderedAccess);
            collectAtFrame_ = frame_ + 4;
            collectReadsWeights_ = training_;
            saveOnCollect_ = training_ && checkFrames_ % kSaveEverySteps == 0;
        }
    }
    if (bend || records)
        for (rhi::BufferHandle b : bufs) ctx.bufferBarrier(b, RS::UnorderedAccess, RS::Common);

    // ---- frame N becomes the previous frame; for the check records, the motion history shifts down ----
    ctx.textureBarrier(in.color, RS::NonPixelShaderResource, RS::CopySource);
    ctx.textureBarrier(in.velocity, RS::NonPixelShaderResource, RS::CopySource);
    ctx.textureBarrier(in.viewZ, RS::NonPixelShaderResource, RS::CopySource);
    if (keepHistory) {
        // N-2 -> N-3, then N-1 -> N-2 (oldest first, so nothing is overwritten before it is copied).
        const rhi::TextureHandle shift[][2] = {
            {histVel3_, histVel2_}, {histZ3_, histZ2_}, {histVel2_, histVel_}, {histZ2_, histZ_}};
        for (const auto& s : shift) {
            ctx.textureBarrier(s[1], RS::NonPixelShaderResource, RS::CopySource);
            ctx.textureBarrier(s[0], RS::NonPixelShaderResource, RS::CopyDest);
            ctx.copyTexture(s[0], s[1]);
            ctx.textureBarrier(s[0], RS::CopyDest, RS::NonPixelShaderResource);
            ctx.textureBarrier(s[1], RS::CopySource, RS::NonPixelShaderResource);
        }
        histDepth_ = histDepth_ < 3 ? histDepth_ + 1 : 3;
    } else {
        histDepth_ = 1;   // only N-1 is current once this frame is stored
    }
    ctx.textureBarrier(histColor_, RS::NonPixelShaderResource, RS::CopyDest);
    ctx.textureBarrier(histVel_, RS::NonPixelShaderResource, RS::CopyDest);
    ctx.textureBarrier(histZ_, RS::NonPixelShaderResource, RS::CopyDest);
    ctx.copyTexture(histColor_, in.color);
    ctx.copyTexture(histVel_, in.velocity);
    ctx.copyTexture(histZ_, in.viewZ);
    ctx.textureBarrier(histColor_, RS::CopyDest, RS::NonPixelShaderResource);
    ctx.textureBarrier(histVel_, RS::CopyDest, RS::NonPixelShaderResource);
    ctx.textureBarrier(histZ_, RS::CopyDest, RS::NonPixelShaderResource);
    ctx.textureBarrier(in.color, RS::CopySource, RS::ShaderResource);
    ctx.textureBarrier(in.velocity, RS::CopySource, RS::RenderTarget);
    ctx.textureBarrier(in.viewZ, RS::CopySource, RS::RenderTarget);
    historyValid_ = true;
    return result;
}

}  // namespace aver::neurafi
