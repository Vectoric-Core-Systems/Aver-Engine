#include "aver/render/neural/Mlp.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <initializer_list>

namespace aver::render::neural {

namespace {

// b3: a root CBV on a register no backend reserves. b1 is the per-object block, which Vulkan always
// folds into push constants (a CBV there read zeros); b0, b2, b4 are the engine's frame, draw and
// feature blocks.
constexpr u32 kConstantSlot = 3;

// aver_neural_mlp.hlsl's binding table, shared by every pipeline: five SRV slots (records,
// targets, count, weights, EMA) and six UAV slots (outputs, gradient accumulator, weights, EMA,
// Adam m, Adam v). A pass leaves the slots it does not use null-filled.
constexpr u32 kSrvCount = 5;
constexpr u32 kUavCount = 6;

constexpr const char* kEntry[4] = {"CSInfer", "CSTrainGrad", "CSAdam", "CSResetState"};

constexpr u32 kGroup = 64;   // [numthreads(64,1,1)], every entry point
// A dispatch is at most 65535 groups wide.
constexpr u32 kMaxGroups = 65535;

// aver_neural_mlp.hlsl's AverNeuralCB, field for field.
struct Constants {
    u32 count;
    u32 maxCount;
    u32 useCountBuf;
    u32 useEma;
    u32 weightCount;
    f32 lr;
    f32 beta1;
    f32 beta2;
    f32 eps;
    f32 bc1;
    f32 bc2;
    f32 emaDecay;
    f32 l2;
    f32 gradScale;
    f32 gradClamp;
    f32 pad0;
};
static_assert(sizeof(Constants) == 64, "AverNeuralCB is four float4s");

u32 groupsFor(u32 n) { return (n + kGroup - 1) / kGroup; }

using rhi::ResourceState;

// Moves every buffer in `list` from one state to another (skipping a no-op, which D3D12 rejects).
void moveBuffers(rhi::IRenderContext& ctx, std::initializer_list<rhi::BufferHandle> list,
                 ResourceState from, ResourceState to) {
    if (from == to) return;
    for (rhi::BufferHandle b : list) if (b) ctx.bufferBarrier(b, from, to);
}

}  // namespace

Mlp::~Mlp() { destroy(); }

bool Mlp::create(rhi::IDevice& dev, const MlpDesc& desc, const OptimiserDesc& opt) {
    destroy();
    std::string why;
    if (!validate(desc, &why) || !validate(opt, &why)) {
        AVER_WARN("[Neural] invalid network description: {}", why);
        return false;
    }
    dev_ = &dev;
    res_ = dev.resources();
    if (!res_) { dev_ = nullptr; return false; }
    desc_ = desc;
    opt_ = opt;
    layout_ = MlpLayout::make(desc);

    const std::string& source = rhi::shaderFile("aver_neural_mlp.hlsl");
    if (source.empty()) {
        AVER_WARN("[Neural] aver_neural_mlp.hlsl is not deployed beside the executable; no network");
        destroy();
        return false;
    }

    // The shape, as DXC defines: every loop bound in the kernels becomes a compile-time constant.
    const std::string defines =
        "AVER_NN_IN=" + std::to_string(desc.inputs) +
        ";AVER_NN_OUT=" + std::to_string(desc.outputs) +
        ";AVER_NN_WIDTH=" + std::to_string(desc.hiddenWidth) +
        ";AVER_NN_LAYERS=" + std::to_string(desc.hiddenLayers) +
        ";AVER_NN_HIDDEN_ACT=" + std::to_string(static_cast<u32>(desc.hidden)) +
        ";AVER_NN_OUT_ACT=" + std::to_string(static_cast<u32>(desc.output)) +
        ";AVER_NN_BIAS=" + std::to_string(desc.bias ? 1 : 0) +
        ";AVER_NN_LOSS=" + std::to_string(static_cast<u32>(opt.loss));

    for (u32 k = 0; k < kKernelCount; ++k) {
        rhi::ShaderDesc sd{};
        sd.source  = source.c_str();
        sd.entry   = kEntry[k];
        sd.stage   = rhi::ShaderStage::Compute;
        sd.minShaderModel = 60;   // no wave intrinsics, no 16-bit types: plain SM 6.0 compute
        sd.defines = defines.c_str();
        const rhi::ShaderHandle cs = res_->createShader(sd);
        if (!cs) {
            AVER_WARN("[Neural] {} would not compile; no network", kEntry[k]);
            destroy();
            return false;
        }
        rhi::ComputePipelineDesc pd{};
        pd.cs = cs;
        pd.layout.srvCount = kSrvCount;
        pd.layout.uavCount = kUavCount;
        pd.layout.constantDwords[kConstantSlot] = 0;   // root CBV
        // Declared, not reflected: Vulkan types every binding of a set layout, and a kernel that
        // leaves a slot unused would otherwise have it guessed (RHIResources.hpp's comment).
        pd.layout.slotKindsDeclared = true;
        for (u32 i = 0; i < kSrvCount; ++i) pd.layout.srvKinds[i] = rhi::SlotKind::StructuredBuffer;
        for (u32 i = 0; i < kUavCount; ++i) pd.layout.uavKinds[i] = rhi::SlotKind::StructuredBuffer;
        const rhi::PipelineHandle p = res_->createComputePipeline(pd);
        res_->destroyShader(cs);   // the pipeline owns the bytecode now
        if (!p) {
            AVER_WARN("[Neural] the {} pipeline would not build; no network", kEntry[k]);
            destroy();
            return false;
        }
        pipelines_[k] = p;
    }

    const u64 bytes = static_cast<u64>(layout_.total) * sizeof(f32);
    auto make = [&](u64 size, rhi::BufferKind kind, bool uav, const char* name) {
        rhi::BufferDesc bd{};
        bd.bytes = size;
        bd.kind = kind;
        bd.allowUnorderedAccess = uav;
        bd.debugName = name;
        return res_->createBuffer(bd);
    };
    weights_ = make(bytes, rhi::BufferKind::Default, true, "Neural weights");
    ema_     = make(bytes, rhi::BufferKind::Default, true, "Neural weights EMA");
    m_       = make(bytes, rhi::BufferKind::Default, true, "Neural Adam m");
    v_       = make(bytes, rhi::BufferKind::Default, true, "Neural Adam v");
    grad_    = make(bytes, rhi::BufferKind::Default, true, "Neural gradient accumulator");
    dummy_   = make(16, rhi::BufferKind::Default, false, "Neural count placeholder");
    if (!weights_ || !ema_ || !m_ || !v_ || !grad_ || !dummy_) {
        AVER_WARN("[Neural] could not allocate {} KiB of network buffers; no network",
                  static_cast<u64>(bytes * 5 / 1024));
        destroy();
        return false;
    }

    cpuMaster_ = initWeights(desc);
    cpuEma_ = cpuMaster_;
    pendingUpload_ = true;   // the init reaches the GPU with the first record*() call
    pendingReset_  = true;   // and so does the zeroing of m, v and the accumulator
    step_ = 0;

    AVER_INFO("[Neural] MLP {} -> {}x{} -> {}: {} weights, portable fp32", desc.inputs, desc.hiddenWidth,
              desc.hiddenLayers, desc.outputs, layout_.total);
    return true;
}

void Mlp::destroy() {
    invalidateBindings();
    if (res_) {
        for (rhi::PipelineHandle& p : pipelines_) { if (p) res_->destroyPipeline(p); p = 0; }
        for (rhi::BufferHandle* b : {&weights_, &ema_, &m_, &v_, &grad_, &dummy_, &readback_}) {
            if (*b) res_->destroyBuffer(*b);
            *b = 0;
        }
        for (rhi::BufferHandle& s : staging_) { if (s) res_->destroyBuffer(s); s = 0; }
    }
    for (rhi::PipelineHandle& p : pipelines_) p = 0;
    weights_ = ema_ = m_ = v_ = grad_ = dummy_ = readback_ = 0;
    for (rhi::BufferHandle& s : staging_) s = 0;
    stagingNext_ = 0;
    cpuMaster_.clear();
    cpuEma_.clear();
    pendingUpload_ = pendingReset_ = false;
    step_ = 0;
    warnedRecycle_ = warnedBatch_ = false;
    dev_ = nullptr;
    res_ = nullptr;
}

void Mlp::invalidateBindings() {
    for (u32 k = 0; k <= kAdam; ++k) {
        if (res_) for (CachedSet& c : sets_[k]) if (c.set) res_->destroyBindingSet(c.set);
        sets_[k].clear();
        recycleNext_[k] = 0;
    }
}

// A binding set per distinct (a, b, count, maxCount) of one kernel. `a`/`b` are the records and the
// outputs (infer) or the targets (train); the Adam kernel binds only fixed buffers and the count.
rhi::BindingSetHandle Mlp::bindingSet(Kernel k, rhi::BufferHandle a, rhi::BufferHandle b,
                                      rhi::BufferHandle count, u32 maxCount) {
    std::vector<CachedSet>& cache = sets_[k];
    for (const CachedSet& c : cache)
        if (c.key[0] == a && c.key[1] == b && c.key[2] == count && c.maxCount == maxCount) return c.set;

    CachedSet* slot = nullptr;
    if (cache.size() < kMaxCachedSets) {
        rhi::BindingSetDesc bd{};
        bd.srvCount = kSrvCount;
        bd.uavCount = kUavCount;
        for (u32 i = 0; i < kSrvCount; ++i) bd.srvKinds[i] = rhi::SlotKind::StructuredBuffer;
        for (u32 i = 0; i < kUavCount; ++i) bd.uavKinds[i] = rhi::SlotKind::StructuredBuffer;
        const rhi::BindingSetHandle set = res_->createBindingSet(bd);
        if (!set) { AVER_WARN("[Neural] createBindingSet failed"); return 0; }
        cache.push_back({});
        cache.back().set = set;
        slot = &cache.back();
    } else {
        // Recycling rewrites a set an earlier recorded dispatch may still name. Reaching here means
        // the caller cycles through more than kMaxCachedSets distinct buffer tuples per kernel,
        // which is a sign it should keep a stable set of buffers instead.
        if (!warnedRecycle_) {
            AVER_WARN("[Neural] more than {} distinct buffer sets for one kernel; recycling binding sets "
                      "(a set rewritten while an earlier dispatch is pending reads the wrong buffers)",
                      kMaxCachedSets);
            warnedRecycle_ = true;
        }
        slot = &cache[recycleNext_[k]];
        recycleNext_[k] = (recycleNext_[k] + 1) % kMaxCachedSets;
    }
    slot->key[0] = a;
    slot->key[1] = b;
    slot->key[2] = count;
    slot->maxCount = maxCount;

    const rhi::BindingSetHandle set = slot->set;
    constexpr u32 f = sizeof(f32);
    switch (k) {
        case kInfer:
            res_->setSrvBuffer(set, 0, a, f, maxCount * desc_.inputs, 0);
            res_->setSrvBuffer(set, 2, count, sizeof(u32), 1, 0);
            res_->setSrvBuffer(set, 3, weights_, f, layout_.total, 0);
            res_->setSrvBuffer(set, 4, ema_, f, layout_.total, 0);
            res_->setUavBuffer(set, 0, b, f, maxCount * desc_.outputs, 0);
            break;
        case kTrainGrad:
            res_->setSrvBuffer(set, 0, a, f, maxCount * desc_.inputs, 0);
            res_->setSrvBuffer(set, 1, b, f, maxCount * desc_.outputs, 0);
            res_->setSrvBuffer(set, 2, count, sizeof(u32), 1, 0);
            res_->setSrvBuffer(set, 3, weights_, f, layout_.total, 0);
            res_->setUavBuffer(set, 1, grad_, sizeof(i32), layout_.total, 0);
            break;
        case kAdam:
            res_->setSrvBuffer(set, 2, count, sizeof(u32), 1, 0);
            res_->setUavBuffer(set, 1, grad_, sizeof(i32), layout_.total, 0);
            res_->setUavBuffer(set, 2, weights_, f, layout_.total, 0);
            res_->setUavBuffer(set, 3, ema_, f, layout_.total, 0);
            res_->setUavBuffer(set, 4, m_, f, layout_.total, 0);
            res_->setUavBuffer(set, 5, v_, f, layout_.total, 0);
            break;
        default: break;
    }
    return set;
}

// Queued weights go up through a staging ring (a Default buffer has no CPU write), and Adam's
// state is zeroed by the reset kernel -- a fresh or re-uploaded network must not inherit moments.
void Mlp::flushPending(rhi::IRenderContext& ctx) {
    if (!pendingUpload_ && !pendingReset_) return;
    const u64 bytes = static_cast<u64>(layout_.total) * sizeof(f32);

    if (pendingUpload_) {
        rhi::BufferHandle& st = staging_[stagingNext_];
        if (!st) {
            rhi::BufferDesc bd{};
            bd.bytes = bytes;
            bd.kind = rhi::BufferKind::Upload;
            bd.debugName = "Neural weight staging";
            st = res_->createBuffer(bd);
        }
        if (!st || !res_->writeBuffer(st, cpuMaster_.data(), bytes)) {
            AVER_WARN("[Neural] weight upload staging failed; the network keeps its previous weights");
            return;
        }
        stagingNext_ = (stagingNext_ + 1) % kStagingRing;
        moveBuffers(ctx, {weights_, ema_}, ResourceState::Common, ResourceState::CopyDest);
        ctx.copyBuffer(weights_, st, bytes);
        ctx.copyBuffer(ema_, st, bytes);
        moveBuffers(ctx, {weights_, ema_}, ResourceState::CopyDest, ResourceState::Common);
        pendingUpload_ = false;
        pendingReset_ = true;
    }

    if (pendingReset_) {
        Constants cb{};
        cb.weightCount = layout_.total;
        const rhi::BindingSetHandle set = bindingSet(kAdam, 0, 0, dummy_, 0);
        if (!set) return;
        moveBuffers(ctx, {weights_, ema_, m_, v_, grad_}, ResourceState::Common, ResourceState::UnorderedAccess);
        ctx.setPipeline(pipelines_[kReset]);
        ctx.setBindingSet(set);
        ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
        ctx.dispatch(groupsFor(layout_.total), 1, 1);
        moveBuffers(ctx, {weights_, ema_, m_, v_, grad_}, ResourceState::UnorderedAccess, ResourceState::Common);
        pendingReset_ = false;
        step_ = 0;
    }
}

bool Mlp::recordInferImpl(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle outputs,
                          rhi::BufferHandle countBuffer, u32 cpuCount, u32 maxCount, bool useEma,
                          const IoStates& states) {
    if (!valid() || !records || !outputs || maxCount == 0) return false;
    if (groupsFor(maxCount) > kMaxGroups) {
        AVER_WARN("[Neural] recordInfer: maxCount {} exceeds one dispatch ({} records)", maxCount,
                  kMaxGroups * kGroup);
        return false;
    }
    if (!countBuffer && cpuCount == 0) return true;   // nothing to infer

    rhi::ScopedGpuStat gpuStat(ctx, "Neural.Infer");
    flushPending(ctx);

    const rhi::BufferHandle countBind = countBuffer ? countBuffer : dummy_;
    const rhi::BindingSetHandle set = bindingSet(kInfer, records, outputs, countBind, maxCount);
    if (!set) return false;

    Constants cb{};
    cb.count = std::min(cpuCount, maxCount);
    cb.maxCount = maxCount;
    cb.useCountBuf = countBuffer ? 1u : 0u;
    cb.useEma = useEma ? 1u : 0u;
    cb.weightCount = layout_.total;

    constexpr ResourceState kRead = ResourceState::NonPixelShaderResource;
    constexpr ResourceState kWrite = ResourceState::UnorderedAccess;
    moveBuffers(ctx, {records, countBuffer}, states.input, kRead);
    moveBuffers(ctx, {countBuffer ? 0u : dummy_}, ResourceState::Common, kRead);
    moveBuffers(ctx, {weights_, ema_}, ResourceState::Common, kRead);
    moveBuffers(ctx, {outputs}, states.output, kWrite);

    ctx.setPipeline(pipelines_[kInfer]);
    ctx.setBindingSet(set);
    ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
    ctx.dispatch(groupsFor(maxCount), 1, 1);

    moveBuffers(ctx, {outputs}, kWrite, states.output);
    moveBuffers(ctx, {weights_, ema_}, kRead, ResourceState::Common);
    moveBuffers(ctx, {countBuffer ? 0u : dummy_}, kRead, ResourceState::Common);
    moveBuffers(ctx, {records, countBuffer}, kRead, states.input);
    return true;
}

bool Mlp::recordInfer(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle outputs,
                      rhi::BufferHandle countBuffer, u32 maxCount, bool useEma, const IoStates& states) {
    if (!countBuffer) return false;   // use the CpuCount overload for a CPU-known count
    return recordInferImpl(ctx, records, outputs, countBuffer, 0, maxCount, useEma, states);
}

bool Mlp::recordInfer(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle outputs,
                      CpuCount count, bool useEma, const IoStates& states) {
    return recordInferImpl(ctx, records, outputs, 0, count.value, count.value, useEma, states);
}

bool Mlp::recordTrainImpl(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle targets,
                          rhi::BufferHandle countBuffer, u32 cpuCount, u32 maxCount,
                          const IoStates& states) {
    if (!valid() || !records || !targets || maxCount == 0) return false;
    if (groupsFor(maxCount) > kMaxGroups) {
        AVER_WARN("[Neural] recordTrain: maxCount {} exceeds one dispatch ({} records)", maxCount,
                  kMaxGroups * kGroup);
        return false;
    }
    if (!countBuffer && cpuCount == 0) return true;   // an empty batch is not a step

    if (maxCount > safeBatchLimit() && !warnedBatch_) {
        AVER_WARN("[Neural] training batch of {} records can overflow the fixed-point gradient "
                  "accumulator in the worst case (safe limit {} at gradClamp {} x gradFixedScale {}); "
                  "lower gradFixedScale or the batch",
                  maxCount, safeBatchLimit(), opt_.gradClamp, opt_.gradFixedScale);
        warnedBatch_ = true;
    }

    rhi::ScopedGpuStat gpuStat(ctx, "Neural.Train");
    flushPending(ctx);

    const rhi::BufferHandle countBind = countBuffer ? countBuffer : dummy_;
    const rhi::BindingSetHandle gset = bindingSet(kTrainGrad, records, targets, countBind, maxCount);
    const rhi::BindingSetHandle aset = bindingSet(kAdam, 0, 0, countBind, 0);
    if (!gset || !aset) return false;

    ++step_;
    Constants cb{};
    cb.count = std::min(cpuCount, maxCount);
    cb.maxCount = maxCount;
    cb.useCountBuf = countBuffer ? 1u : 0u;
    cb.weightCount = layout_.total;
    cb.lr = opt_.learningRate;
    cb.beta1 = opt_.beta1;
    cb.beta2 = opt_.beta2;
    cb.eps = opt_.epsilon;
    cb.bc1 = adamBiasCorrection(opt_.beta1, step_);
    cb.bc2 = adamBiasCorrection(opt_.beta2, step_);
    cb.emaDecay = opt_.weightEma;
    cb.l2 = opt_.l2;
    cb.gradScale = opt_.gradFixedScale;
    cb.gradClamp = opt_.gradClamp;

    constexpr ResourceState kRead = ResourceState::NonPixelShaderResource;
    constexpr ResourceState kWrite = ResourceState::UnorderedAccess;
    constexpr ResourceState kRest = ResourceState::Common;
    moveBuffers(ctx, {records, targets, countBuffer}, states.input, kRead);
    moveBuffers(ctx, {countBuffer ? 0u : dummy_}, kRest, kRead);
    moveBuffers(ctx, {weights_}, kRest, kRead);
    moveBuffers(ctx, {grad_}, kRest, kWrite);

    // Pass 1: per-record forward + backward, quantised gradients summed into the accumulator.
    ctx.setPipeline(pipelines_[kTrainGrad]);
    ctx.setBindingSet(gset);
    ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
    ctx.dispatch(groupsFor(maxCount), 1, 1);
    ctx.uavBarrierBuffer(grad_);   // every group's atomics land before Adam reads them

    // Pass 2: one thread per weight -- mean, Adam, EMA, clear.
    moveBuffers(ctx, {weights_}, kRead, kWrite);
    moveBuffers(ctx, {ema_, m_, v_}, kRest, kWrite);
    ctx.setPipeline(pipelines_[kAdam]);
    ctx.setBindingSet(aset);
    ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
    ctx.dispatch(groupsFor(layout_.total), 1, 1);

    moveBuffers(ctx, {weights_, ema_, m_, v_, grad_}, kWrite, kRest);
    moveBuffers(ctx, {countBuffer ? 0u : dummy_}, kRead, kRest);
    moveBuffers(ctx, {records, targets, countBuffer}, kRead, states.input);
    return true;
}

bool Mlp::recordTrain(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle targets,
                      rhi::BufferHandle countBuffer, u32 maxCount, const IoStates& states) {
    if (!countBuffer) return false;   // use the CpuCount overload for a CPU-known count
    return recordTrainImpl(ctx, records, targets, countBuffer, 0, maxCount, states);
}

bool Mlp::recordTrain(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle targets,
                      CpuCount count, const IoStates& states) {
    return recordTrainImpl(ctx, records, targets, 0, count.value, count.value, states);
}

// ---------------------------------------------------------------- weights

bool Mlp::setLearningRate(f32 lr) {
    if (!(lr > 0.0f) || !(lr < 3.4e38f)) return false;   // the same test as MlpReference's twin
    opt_.learningRate = lr;
    return true;
}

bool Mlp::uploadWeights(std::span<const f32> w) {
    if (!valid() || w.size() != layout_.total) return false;
    cpuMaster_.assign(w.begin(), w.end());
    cpuEma_ = cpuMaster_;
    pendingUpload_ = true;
    pendingReset_ = true;
    step_ = 0;
    return true;
}

bool Mlp::saveWeights(const std::string& path, bool ema) const {
    if (!valid()) return false;
    return saveWeightFile(path, desc_, ema ? std::span<const f32>(cpuEma_) : std::span<const f32>(cpuMaster_));
}

bool Mlp::loadWeights(const std::string& path) {
    if (!valid()) return false;
    MlpDesc fileDesc = desc_;
    std::vector<f32> w;
    if (!loadWeightFile(path, fileDesc, w)) {
        AVER_WARN("[Neural] {} is not a readable AVNN weight file", path);
        return false;
    }
    // The seed only drives init, so it is not compared; everything that shapes the maths is.
    if (fileDesc.inputs != desc_.inputs || fileDesc.outputs != desc_.outputs ||
        fileDesc.hiddenWidth != desc_.hiddenWidth || fileDesc.hiddenLayers != desc_.hiddenLayers ||
        fileDesc.hidden != desc_.hidden || fileDesc.output != desc_.output || fileDesc.bias != desc_.bias) {
        AVER_WARN("[Neural] {} holds a different network shape than this one; not loaded", path);
        return false;
    }
    return uploadWeights(w);
}

bool Mlp::recordReadback(rhi::IRenderContext& ctx) {
    if (!valid()) return false;
    const u64 bytes = static_cast<u64>(layout_.total) * sizeof(f32);
    if (!readback_) {
        rhi::BufferDesc bd{};
        bd.bytes = bytes * 2;
        bd.kind = rhi::BufferKind::Readback;
        bd.debugName = "Neural weight readback";
        readback_ = res_->createBuffer(bd);
        if (!readback_) { AVER_WARN("[Neural] readback buffer would not allocate"); return false; }
    }
    flushPending(ctx);
    moveBuffers(ctx, {weights_, ema_}, ResourceState::Common, ResourceState::CopySource);
    ctx.copyBuffer(readback_, weights_, bytes, 0, 0);
    ctx.copyBuffer(readback_, ema_, bytes, bytes, 0);
    moveBuffers(ctx, {weights_, ema_}, ResourceState::CopySource, ResourceState::Common);
    return true;
}

bool Mlp::collectWeights() {
    if (!valid() || !readback_ || pendingUpload_) return false;
    const u64 bytes = static_cast<u64>(layout_.total) * sizeof(f32);
    std::vector<f32> master(layout_.total), ema(layout_.total);
    if (!res_->readBuffer(readback_, master.data(), bytes, 0) ||
        !res_->readBuffer(readback_, ema.data(), bytes, bytes))
        return false;
    cpuMaster_ = std::move(master);
    cpuEma_ = std::move(ema);
    return true;
}

}  // namespace aver::render::neural
