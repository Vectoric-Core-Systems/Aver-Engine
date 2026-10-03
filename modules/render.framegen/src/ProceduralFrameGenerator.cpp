#include "aver/framegen/ProceduralFrameGenerator.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <cmath>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace aver::framegen {

namespace {

constexpr const char* kShaderName = "framegen.hlsl";
constexpr u32 kGroup = 8;           // framegen.hlsl's FG_GROUP
constexpr u32 kConstantSlot = 1;    // b1: FgConstants
constexpr u32 kGatherDwords = 4;
constexpr u32 kTrajDwords = 8;      // FgConstants with FG_TRAJ's extra four
constexpr u32 kRecordFloats = 14;   // framegen.hlsl's FG_RECORD
constexpr u32 kOutputFloats = 2;    // FG_OUTPUT

struct FgConstants {
    u32 size[2];
    u32 last;
    u32 traj;
    u32 mode;       // FG_TRAJ only from here
    u32 frame;
    u32 samples;
    u32 pad;
};
static_assert(sizeof(FgConstants) == kTrajDwords * 4, "FgConstants mirrors framegen.hlsl's cbuffer");

using RS = rhi::ResourceState;

// The trajectory network: the record and output sizes are framegen.hlsl's; two hidden layers of 32.
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

ProceduralFrameGenerator::~ProceduralFrameGenerator() {
    releaseTargets();
    mlp_.destroy();
    for (rhi::PipelineHandle* p : {&gatherPso_, &fillPso_, &featuresPso_, &accelPso_, &clearCountPso_,
                                   &trainRecordsPso_}) {
        if (*p) res_.destroyPipeline(*p);
        *p = 0;
    }
}

bool ProceduralFrameGenerator::ensurePipelines() {
    if (gatherPso_ && fillPso_) return true;
    if (pipelinesFailed_) return false;
    auto fail = [&](const char* why) {
        AVER_WARN("[FrameGen] {}; frame generation stays off", why);
        pipelinesFailed_ = true;
        return false;
    };
    const std::string& source = rhi::shaderFile(kShaderName);
    if (source.empty()) return fail("framegen.hlsl is not deployed beside the executable");

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
        pd.layout.constantDwords[kConstantSlot] = kGatherDwords;
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
    if (!gatherPso_ || !fillPso_) return fail("the frame generation shaders would not compile");
    AVER_INFO("[FrameGen] procedural interpolation ready (gather + 2 full-resolution fill passes)");
    return true;
}

// The four trajectory passes share one layout: t0-t7 motion and depth of frames N..N-3, u0 inference
// records, u1 the acceleration image, u2-u4 training records/targets/count, u5 the network's outputs.
bool ProceduralFrameGenerator::ensureTrajectory() {
    if (featuresPso_ && accelPso_ && clearCountPso_ && trainRecordsPso_ && mlp_.valid()) return true;
    if (trajectoryFailed_) return false;
    auto fail = [&](const char* why) {
        AVER_WARN("[FrameGen] {}; the trajectory stays a straight line", why);
        trajectoryFailed_ = true;
        return false;
    };
    const std::string& source = rhi::shaderFile(kShaderName);
    if (source.empty()) return fail("framegen.hlsl is not deployed beside the executable");
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
        pd.layout.constantDwords[kConstantSlot] = kTrajDwords;
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
        std::error_code ec;
        if (!weightsPath_.empty() && std::filesystem::exists(weightsPath_, ec)) {
            weightsLoaded_ = mlp_.loadWeights(weightsPath_);
            AVER_INFO("[FrameGen] trajectory network weights {} from {}",
                      weightsLoaded_ ? "loaded" : "NOT loaded (shape mismatch?)", weightsPath_);
        }
        AVER_INFO("[FrameGen] trajectory network {}->{}x{}->{} ready; {}", kRecordFloats, 32, 2, kOutputFloats,
                  weightsLoaded_ ? "using the loaded weights"
                                 : "the analytic acceleration stands in until it has trained");
    }
    return true;
}

void ProceduralFrameGenerator::releaseTargets() {
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

bool ProceduralFrameGenerator::ensureTargets(u32 w, u32 h) {
    if (w == w_ && h == h_ && out_) return true;
    releaseTargets();
    const u32 qw = (w + 1) / 2, qh = (h + 1) / 2;

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
    histColor_ = make(w, h, rhi::Format::RGBA16F, false, RS::NonPixelShaderResource, "FrameGen N-1 colour");
    histVel_   = make(w, h, rhi::Format::RG16F, false, RS::NonPixelShaderResource, "FrameGen N-1 motion");
    histZ_     = make(w, h, rhi::Format::R32Float, false, RS::NonPixelShaderResource, "FrameGen N-1 view depth");
    histVel2_  = make(w, h, rhi::Format::RG16F, false, RS::NonPixelShaderResource, "FrameGen N-2 motion");
    histZ2_    = make(w, h, rhi::Format::R32Float, false, RS::NonPixelShaderResource, "FrameGen N-2 view depth");
    histVel3_  = make(w, h, rhi::Format::RG16F, false, RS::NonPixelShaderResource, "FrameGen N-3 motion");
    histZ3_    = make(w, h, rhi::Format::R32Float, false, RS::NonPixelShaderResource, "FrameGen N-3 view depth");
    out_       = make(w, h, rhi::Format::RGBA16F, true, RS::ShaderResource, "FrameGen generated frame");
    tmp_       = make(w, h, rhi::Format::RGBA16F, true, RS::ShaderResource, "FrameGen fill scratch");
    accel_     = make(qw, qh, rhi::Format::RG16F, true, RS::NonPixelShaderResource, "FrameGen acceleration");
    auto buffer = [&](u64 bytes, const char* name) {
        rhi::BufferDesc bd;
        bd.bytes = bytes;
        bd.kind = rhi::BufferKind::Default;
        bd.allowUnorderedAccess = true;
        bd.debugName = name;
        return res_.createBuffer(bd);
    };
    const u64 qCount = static_cast<u64>(qw) * qh;
    records_    = buffer(qCount * kRecordFloats * 4, "FrameGen trajectory records");
    netOut_     = buffer(qCount * kOutputFloats * 4, "FrameGen trajectory network outputs");
    trainRec_   = buffer(static_cast<u64>(kTrainSamples) * kRecordFloats * 4, "FrameGen training records");
    trainTgt_   = buffer(static_cast<u64>(kTrainSamples) * kOutputFloats * 4, "FrameGen training targets");
    trainCount_ = buffer(16, "FrameGen training count");
    auto readback = [&](u64 bytes, const char* name) {
        rhi::BufferDesc bd;
        bd.bytes = bytes;
        bd.kind = rhi::BufferKind::Readback;
        bd.debugName = name;
        return res_.createBuffer(bd);
    };
    evalRec_   = readback(static_cast<u64>(kTrainSamples) * kRecordFloats * 4, "FrameGen evaluation records");
    evalTgt_   = readback(static_cast<u64>(kTrainSamples) * kOutputFloats * 4, "FrameGen evaluation targets");
    evalCount_ = readback(16, "FrameGen evaluation count");
    if (!histColor_ || !histVel_ || !histZ_ || !histVel2_ || !histZ2_ || !histVel3_ || !histZ3_ || !out_ ||
        !tmp_ || !accel_ || !records_ || !netOut_ || !trainRec_ || !trainTgt_ || !trainCount_ || !evalRec_ ||
        !evalTgt_ || !evalCount_) {
        AVER_WARN("[FrameGen] could not create the {}x{} targets", w, h);
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
        AVER_WARN("[FrameGen] could not create the binding sets");
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
    const f64 mib = (static_cast<f64>(w) * h * (8 + 4 + 4 + 8 + 8 + 2 * (4 + 4)) +
                     static_cast<f64>(qCount) * (4 + 4 * (kRecordFloats + kOutputFloats))) / (1024.0 * 1024.0);
    AVER_INFO("[FrameGen] targets {}x{} ({:.1f} MiB)", w, h, mib);
    return true;
}

// Scores one read-back training batch three ways, as the error in the predicted in-between POSITION
// (0.125 |a - a_true| pixels, the quadratic's own term): a straight line (a = 0), the analytic
// acceleration (v - v', exact for constant acceleration), and the network (the CPU twin running the
// EMA weights inference uses). The batch was trained on once, so the network's figure is slightly
// flattering; the comparison is still the one that says whether it earns its cost.
void ProceduralFrameGenerator::evaluateBatch() {
    u32 count = 0;
    if (!res_.readBuffer(evalCount_, &count, 4, 0)) return;
    if (count > kTrainSamples) count = kTrainSamples;
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
        auto err = [&](f64 ax, f64 ay) { return 0.125 * s * std::sqrt((ax - t[0]) * (ax - t[0]) + (ay - t[1]) * (ay - t[1])); };
        errLinear += err(0.0, 0.0);
        errAnalytic += err(static_cast<f64>(r[0]) - r[2], static_cast<f64>(r[1]) - r[3]);
        errNet += err(out[0], out[1]);
    }
    const f64 n = static_cast<f64>(count);
    AVER_INFO("[FrameGen] trajectory error at step {} over {} two-frame spans (mean in-between position "
              "error, px): straight line {:.3f}, analytic {:.3f}, network {:.3f}",
              trainSteps_, count, errLinear / n, errAnalytic / n, errNet / n);
}

// Frame N's inputs keep their handles from frame to frame (the device's own textures), so the sets are
// rewritten only when one changes -- a resize, after which the device has idled the GPU.
void ProceduralFrameGenerator::bindInputs(const rhi::FrameGenInput& in) {
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

rhi::TextureHandle ProceduralFrameGenerator::generate(rhi::IRenderContext& ctx, const rhi::FrameGenInput& in) {
    if (!in.color || !in.velocity || !in.viewZ || !in.width || !in.height) return 0;
    if (!ensurePipelines()) return 0;
    if (!ensureTargets(in.width, in.height)) return 0;
    bindInputs(in);
    ++frame_;

    // A readback recorded kSaveEverySteps steps ago has certainly finished (frames in flight are 2).
    if (saveAtFrame_ && frame_ >= saveAtFrame_) {
        saveAtFrame_ = 0;
        if (mlp_.collectWeights()) {
            evaluateBatch();
            if (!weightsPath_.empty()) {
                const bool ok = mlp_.saveWeights(weightsPath_);
                AVER_INFO("[FrameGen] trajectory network: {} steps trained, weights {} {}", trainSteps_,
                          ok ? "saved to" : "could NOT be saved to", weightsPath_);
            }
        }
    }

    const bool cut = in.sceneCut || !historyValid_;
    if (cut) histDepth_ = 0;
    const bool trajOk = (trajectory_ != Trajectory::Linear || training_) && ensureTrajectory();
    const bool bend = trajOk && trajectory_ != Trajectory::Linear && !cut;
    const bool train = trajOk && training_ && histDepth_ >= 3;
    const bool neural = bend && trajectory_ == Trajectory::Neural && networkReady();

    const u32 gx = (in.width + kGroup - 1) / kGroup, gy = (in.height + kGroup - 1) / kGroup;
    const u32 qgx = (qw_ + kGroup - 1) / kGroup, qgy = (qh_ + kGroup - 1) / kGroup;
    FgConstants k{{in.width, in.height}, 0, bend ? 1u : 0u, neural ? 1u : 0u, frame_, kTrainSamples, 0};

    // Frame N becomes compute-readable (a pixel-shader read state does not cover compute).
    ctx.textureBarrier(in.color, RS::ShaderResource, RS::NonPixelShaderResource);
    ctx.textureBarrier(in.velocity, RS::RenderTarget, RS::NonPixelShaderResource);
    ctx.textureBarrier(in.viewZ, RS::RenderTarget, RS::NonPixelShaderResource);
    const rhi::BufferHandle bufs[] = {records_, netOut_, trainRec_, trainTgt_, trainCount_};
    if (bend || train)
        for (rhi::BufferHandle b : bufs) ctx.bufferBarrier(b, RS::Common, RS::UnorderedAccess);

    rhi::TextureHandle result = 0;
    if (!cut) {
        rhi::ScopedGpuStat stat(ctx, "Frame generation");
        if (bend) {
            if (neural) {
                ctx.setPipeline(featuresPso_);
                ctx.setBindingSet(trajSet_);
                ctx.setConstants(kConstantSlot, &k, kTrajDwords);
                ctx.dispatch(qgx, qgy, 1);
                ctx.uavBarrierBuffer(records_);
                render::neural::IoStates io;
                io.input = RS::UnorderedAccess;
                io.output = RS::UnorderedAccess;
                mlp_.recordInfer(ctx, records_, netOut_, render::neural::CpuCount{qw_ * qh_}, true, io);
                ctx.uavBarrierBuffer(netOut_);
                if (!loggedNetworkLive_) {
                    loggedNetworkLive_ = true;
                    AVER_INFO("[FrameGen] trajectory network live ({})",
                              weightsLoaded_ ? "loaded weights" : "trained this session");
                }
            }
            ctx.textureBarrier(accel_, RS::NonPixelShaderResource, RS::UnorderedAccess);
            ctx.setPipeline(accelPso_);
            ctx.setBindingSet(trajSet_);
            ctx.setConstants(kConstantSlot, &k, kTrajDwords);
            ctx.dispatch(qgx, qgy, 1);
            ctx.textureBarrier(accel_, RS::UnorderedAccess, RS::NonPixelShaderResource);
        }

        ctx.textureBarrier(out_, RS::ShaderResource, RS::UnorderedAccess);
        ctx.setPipeline(gatherPso_);
        ctx.setBindingSet(gatherSet_);
        ctx.setConstants(kConstantSlot, &k, kGatherDwords);
        ctx.dispatch(gx, gy, 1);

        // Fill, twice: out_ -> tmp_ -> out_.
        ctx.textureBarrier(out_, RS::UnorderedAccess, RS::NonPixelShaderResource);
        ctx.textureBarrier(tmp_, RS::ShaderResource, RS::UnorderedAccess);
        ctx.setPipeline(fillPso_);
        ctx.setBindingSet(fillSetA_);
        ctx.setConstants(kConstantSlot, &k, kGatherDwords);
        ctx.dispatch(gx, gy, 1);

        ctx.textureBarrier(tmp_, RS::UnorderedAccess, RS::NonPixelShaderResource);
        ctx.textureBarrier(out_, RS::NonPixelShaderResource, RS::UnorderedAccess);
        k.last = 1;
        ctx.setBindingSet(fillSetB_);
        ctx.setConstants(kConstantSlot, &k, kGatherDwords);
        ctx.dispatch(gx, gy, 1);
        k.last = 0;

        ctx.textureBarrier(out_, RS::UnorderedAccess, RS::ShaderResource);
        ctx.textureBarrier(tmp_, RS::NonPixelShaderResource, RS::ShaderResource);
        result = out_;
    }

    // ---- training: one Adam step on records from frames N..N-3 (the shader says how) ----
    if (train) {
        rhi::ScopedGpuStat stat(ctx, "Frame generation training");
        ctx.setPipeline(clearCountPso_);
        ctx.setBindingSet(trajSet_);
        ctx.setConstants(kConstantSlot, &k, kTrajDwords);
        ctx.dispatch(1, 1, 1);
        ctx.uavBarrierBuffer(trainCount_);
        ctx.setPipeline(trainRecordsPso_);
        ctx.setBindingSet(trajSet_);
        ctx.setConstants(kConstantSlot, &k, kTrajDwords);
        ctx.dispatch((kTrainSamples + 63) / 64, 1, 1);
        ctx.uavBarrierBuffer(trainRec_);
        ctx.uavBarrierBuffer(trainTgt_);
        ctx.uavBarrierBuffer(trainCount_);
        render::neural::IoStates io;
        io.input = RS::UnorderedAccess;
        io.output = RS::UnorderedAccess;
        if (mlp_.recordTrain(ctx, trainRec_, trainTgt_, trainCount_, kTrainSamples, io)) {
            ++trainSteps_;
            if (trainSteps_ == kWarmSteps && !weightsLoaded_)
                AVER_INFO("[FrameGen] trajectory network warmed up ({} steps); Neural now uses it", trainSteps_);
            if (trainSteps_ % kSaveEverySteps == 0 && mlp_.recordReadback(ctx)) {
                // The batch just trained on, for evaluateBatch (copied while it is still current).
                const rhi::BufferHandle src[] = {trainRec_, trainTgt_, trainCount_};
                for (rhi::BufferHandle b : src) ctx.bufferBarrier(b, RS::UnorderedAccess, RS::CopySource);
                ctx.copyBuffer(evalRec_, trainRec_, static_cast<u64>(kTrainSamples) * kRecordFloats * 4);
                ctx.copyBuffer(evalTgt_, trainTgt_, static_cast<u64>(kTrainSamples) * kOutputFloats * 4);
                ctx.copyBuffer(evalCount_, trainCount_, 4);
                for (rhi::BufferHandle b : src) ctx.bufferBarrier(b, RS::CopySource, RS::UnorderedAccess);
                saveAtFrame_ = frame_ + 4;
            }
        }
    }
    if (bend || train)
        for (rhi::BufferHandle b : bufs) ctx.bufferBarrier(b, RS::UnorderedAccess, RS::Common);

    // ---- frame N becomes the previous frame; with training, the motion history shifts down ----
    ctx.textureBarrier(in.color, RS::NonPixelShaderResource, RS::CopySource);
    ctx.textureBarrier(in.velocity, RS::NonPixelShaderResource, RS::CopySource);
    ctx.textureBarrier(in.viewZ, RS::NonPixelShaderResource, RS::CopySource);
    if (training_ && trajOk) {
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

}  // namespace aver::framegen
