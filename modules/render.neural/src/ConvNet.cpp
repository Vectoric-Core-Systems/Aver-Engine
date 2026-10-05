#include "aver/render/neural/ConvNet.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <optional>

// Pattern: Mlp.cpp. Kernels and their bindings: shaders/aver_neural_conv.hlsl.

namespace aver::render::neural {

namespace {

constexpr u32 kConstantSlot = 3;   // b3, as Mlp (README: why not b1)
constexpr u32 kSrvCount = 6;
constexpr u32 kUavCount = 7;
constexpr u32 kGroup = 64;
constexpr u32 kMaxGroups = 65535;
// Tiling, mirrored by aver_neural_conv.hlsl. CSConvForward: outputs per thread along x (a group covers
// 8 * fwdPx(k) x 8 outputs x kConvCoBlock channels). CSConvBackwardData: inputs per thread along x (a multiple
// of every stride). CSConvBackwardWeights: a group covers kBwCoBlock output x kBwCiBlock input channels.
constexpr u32 fwdPx(u32 kernel) { return kernel == 1 ? 2u : 4u; }
constexpr u32 kBwdPx = 2;
constexpr u32 kBwCoBlock = 16;
constexpr u32 kBwCiBlock = 4;

using rhi::ResourceState;
constexpr ResourceState kRest  = ResourceState::Common;
constexpr ResourceState kRead  = ResourceState::NonPixelShaderResource;
constexpr ResourceState kWrite = ResourceState::UnorderedAccess;

u32 divUp(u32 a, u32 b) { return (a + b - 1u) / b; }

// Floats the loss weight tensor holds for a head shape.
u32 weightFloats(const TensorShape& head, ConvLossWeight kind) {
    return static_cast<u32>(kind == ConvLossWeight::PerElement ? head.count() : head.n * head.planeCount());
}

// Layer l's input shape for a network input shape.
TensorShape layerInput(const ConvLayout& L, const TensorShape& in, u32 layer) {
    TensorShape s = in;
    for (u32 l = 0; l < layer; ++l) s = L.outDims(l, s);
    return s;
}

// A caller buffer and the state it rests in; network buffers rest in Common.
struct Rest {
    rhi::BufferHandle b = 0;
    ResourceState s = kRest;
};

using Callers = std::array<Rest, 4>;

ResourceState restOf(rhi::BufferHandle b, const Callers& callers) {
    for (const Rest& r : callers) if (r.b && r.b == b) return r.s;
    return kRest;
}

void moveBuffers(rhi::IRenderContext& ctx, std::initializer_list<rhi::BufferHandle> list, ResourceState from,
                 ResourceState to) {
    if (from == to) return;
    for (rhi::BufferHandle b : list) if (b) ctx.bufferBarrier(b, from, to);
}

}  // namespace

// aver_neural_conv.hlsl's AverNeuralCB: the common 64 bytes, then the conv fields.
struct ConvNet::Constants {
    u32 count, maxCount, useCountBuf, useEma, weightCount;
    f32 lr, beta1, beta2, eps, bc1, bc2, emaDecay, l2, gradScale, gradClamp, pad0;
    u32 n, inH, inW, outH, outW, tilesX, tilesY, elemCount;
    u32 wOffset, bOffset, layerSize, partialCount;
    f32 lossNorm;
    u32 groupsX, weightPerElem, pad2;
};

ConvNet::~ConvNet() { destroy(); }

rhi::PipelineHandle ConvNet::compile(const char* entry, u32 layer) {
    const std::string& source = rhi::shaderFile("aver_neural_conv.hlsl");
    if (source.empty()) return 0;
    const ConvLayerDesc& L = desc_.layers[layer];
    const std::string defines =
        "AVER_CONV_CIN=" + std::to_string(L.cin) + ";AVER_CONV_COUT=" + std::to_string(L.cout) +
        ";AVER_CONV_K=" + std::to_string(L.kernel) + ";AVER_CONV_STRIDE=" + std::to_string(L.stride) +
        ";AVER_CONV_ACT=" + std::to_string(static_cast<u32>(L.act)) + ";AVER_CONV_BIAS=" + std::to_string(L.bias ? 1 : 0) +
        ";AVER_CONV_CO_BLOCK=" + std::to_string(kConvCoBlock) + ";AVER_CONV_CI_CHUNK=" + std::to_string(kConvCiChunk) +
        ";AVER_CONV_PX=" + std::to_string(fwdPx(L.kernel)) + ";AVER_CONV_BPX=" + std::to_string(kBwdPx) +
        ";AVER_CONV_BW_COB=" + std::to_string(kBwCoBlock) + ";AVER_CONV_BW_CIC=" + std::to_string(kBwCiBlock) +
        ";AVER_CONV_ENTRY_" + entry + "=1";

    rhi::ShaderDesc sd{};
    sd.source = source.c_str();
    sd.entry = entry;
    sd.stage = rhi::ShaderStage::Compute;
    sd.minShaderModel = 60;
    sd.defines = defines.c_str();
    const rhi::ShaderHandle cs = res_->createShader(sd);
    if (!cs) {
        AVER_WARN("[Neural] {} (layer {}) would not compile; no conv network", entry, layer);
        return 0;
    }
    rhi::ComputePipelineDesc pd{};
    pd.cs = cs;
    pd.layout.srvCount = kSrvCount;
    pd.layout.uavCount = kUavCount;
    pd.layout.constantDwords[kConstantSlot] = 0;   // root CBV
    pd.layout.slotKindsDeclared = true;
    for (u32 i = 0; i < kSrvCount; ++i) pd.layout.srvKinds[i] = rhi::SlotKind::StructuredBuffer;
    for (u32 i = 0; i < kUavCount; ++i) pd.layout.uavKinds[i] = rhi::SlotKind::StructuredBuffer;
    const rhi::PipelineHandle p = res_->createComputePipeline(pd);
    res_->destroyShader(cs);
    if (!p) AVER_WARN("[Neural] the {} pipeline (layer {}) would not build; no conv network", entry, layer);
    return p;
}

bool ConvNet::create(rhi::IDevice& dev, const ConvNetDesc& desc, const OptimiserDesc& opt, ConvMode mode) {
    destroy();
    std::string why;
    if (!validate(desc, &why) || !validate(opt, &why)) {
        AVER_WARN("[Neural] invalid conv network description: {}", why);
        return false;
    }
    dev_ = &dev;
    res_ = dev.resources();
    if (!res_) { dev_ = nullptr; return false; }
    desc_ = desc;
    opt_ = opt;
    mode_ = mode;
    layout_ = ConvLayout::make(desc);
    const u32 layers = layout_.layers;
    const bool train = mode == ConvMode::Train;

    if (rhi::shaderFile("aver_neural_conv.hlsl").empty()) {
        AVER_WARN("[Neural] aver_neural_conv.hlsl is not deployed beside the executable; no conv network");
        destroy();
        return false;
    }

    std::vector<LayerPipes> pipes(layers);
    bool ok = true;
    for (u32 l = 0; l < layers && ok; ++l) {
        ok = (pipes[l].forward = compile("CSConvForward", l)) != 0;
        if (!train || !ok) continue;
        if (desc.layers[l].act == Activation::ReLU) ok = (pipes[l].actBackward = compile("CSConvActBackward", l)) != 0;
        if (ok && l > 0) ok = (pipes[l].backwardData = compile("CSConvBackwardData", l)) != 0;
        if (ok) ok = (pipes[l].backwardWeights = compile("CSConvBackwardWeights", l)) != 0;
    }
    pipes_ = std::move(pipes);
    if (ok && train) {
        const u32 head = layers - 1;
        ok = (loss_ = compile("CSConvLossL2", head)) != 0 && (eval_ = compile("CSConvEvalReduce", head)) != 0 &&
             (reduce_ = compile("CSConvReduceGrad", head)) != 0 && (adam_ = compile("CSAdam", head)) != 0 &&
             (reset_ = compile("CSResetState", head)) != 0;
    }
    if (!ok) { destroy(); return false; }

    const u64 bytes = static_cast<u64>(layout_.total) * sizeof(f32);
    auto make = [&](u64 size, bool uav, const char* name) {
        rhi::BufferDesc bd{};
        bd.bytes = size;
        bd.kind = rhi::BufferKind::Default;
        bd.allowUnorderedAccess = uav;
        bd.debugName = name;
        return res_->createBuffer(bd);
    };
    weights_ = make(bytes, true, "ConvNet weights");
    ema_     = make(bytes, true, "ConvNet weights EMA");
    dummy_   = make(16, false, "ConvNet count placeholder");
    if (train) {
        m_    = make(bytes, true, "ConvNet Adam m");
        v_    = make(bytes, true, "ConvNet Adam v");
        grad_ = make(bytes, true, "ConvNet gradient accumulator");
    }
    if (!weights_ || !ema_ || !dummy_ || (train && (!m_ || !v_ || !grad_))) {
        AVER_WARN("[Neural] could not allocate the conv network's weight buffers; no conv network");
        destroy();
        return false;
    }
    acts_.assign(layers, 0);
    grads_.assign(layers, 0);
    actCap_.assign(layers, 0);

    cpuMaster_ = initConvWeights(desc);
    cpuEma_ = cpuMaster_;
    pendingUpload_ = true;
    pendingReset_ = train;
    step_ = 0;

    AVER_INFO("[Neural] ConvNet {} layers, {} -> {} channels: {} weights, portable fp32 ({})", layers,
              desc.inChannels, desc.layers.back().cout, layout_.total, train ? "train" : "infer");
    return true;
}

void ConvNet::destroy() {
    invalidateBindings();
    if (res_) {
        for (LayerPipes& lp : pipes_)
            for (rhi::PipelineHandle p : {lp.forward, lp.actBackward, lp.backwardData, lp.backwardWeights})
                if (p) res_->destroyPipeline(p);
        for (rhi::PipelineHandle p : {loss_, eval_, reduce_, adam_, reset_}) if (p) res_->destroyPipeline(p);
        for (rhi::BufferHandle b : {weights_, ema_, m_, v_, grad_, dummy_, partials_, readback_})
            if (b) res_->destroyBuffer(b);
        for (rhi::BufferHandle b : acts_) if (b) res_->destroyBuffer(b);
        for (rhi::BufferHandle b : grads_) if (b) res_->destroyBuffer(b);
        for (rhi::BufferHandle b : staging_) if (b) res_->destroyBuffer(b);
    }
    pipes_.clear();
    loss_ = eval_ = reduce_ = adam_ = reset_ = 0;
    weights_ = ema_ = m_ = v_ = grad_ = dummy_ = partials_ = readback_ = 0;
    acts_.clear();
    grads_.clear();
    actCap_.clear();
    partialCap_ = 0;
    maxRecords_ = 0;
    for (rhi::BufferHandle& s : staging_) s = 0;
    stagingNext_ = 0;
    cpuMaster_.clear();
    cpuEma_.clear();
    io_ = {};
    pendingUpload_ = pendingReset_ = false;
    step_ = 0;
    warnedRecycle_ = warnedSize_ = false;
    dev_ = nullptr;
    res_ = nullptr;
}

TensorShape ConvNet::outputShape(const TensorShape& in) const { return layerInput(layout_, in, layout_.layers); }

bool ConvNet::reserve(std::span<const TensorShape> shapes) {
    if (!valid()) return false;
    const u32 layers = layout_.layers;
    const bool train = mode_ == ConvMode::Train;
    std::vector<u64> need(layers, 0);
    u64 partialNeed = 0;
    u32 records = 0;
    for (const TensorShape& s : shapes) {
        if (s.c != desc_.inChannels || s.n < 1 || s.h < 1 || s.w < 1) {
            AVER_WARN("[Neural] ConvNet::reserve: shape {}x{}x{}x{} does not match {} input channels", s.n, s.c, s.h,
                      s.w, desc_.inChannels);
            return false;
        }
        records = std::max(records, s.n);
        TensorShape cur = s;
        for (u32 l = 0; l < layers; ++l) {
            const TensorShape o = layout_.outDims(l, cur);
            const u32 tiles = divUp(o.w, kConvTile) * divUp(o.h, kConvTile);
            const bool dims = s.n <= kMaxGroups && tiles <= kMaxGroups &&
                              divUp(o.w, kConvTile) * divUp(o.c, kConvCoBlock) <= kMaxGroups &&
                              divUp(cur.w, kConvTile) * divUp(cur.c, kConvCoBlock) <= kMaxGroups;
            if (!dims) {
                AVER_WARN("[Neural] ConvNet::reserve: shape {}x{}x{}x{} exceeds one dispatch", s.n, s.c, s.h, s.w);
                return false;
            }
            need[l] = std::max<u64>(need[l], o.count());
            partialNeed = std::max<u64>(partialNeed, static_cast<u64>(s.n) * tiles * layout_.layerSize[l]);
            cur = o;
        }
    }

    auto make = [&](u64 floats, const char* name) {
        rhi::BufferDesc bd{};
        bd.bytes = floats * sizeof(f32);
        bd.kind = rhi::BufferKind::Default;
        bd.allowUnorderedAccess = true;
        bd.debugName = name;
        return res_->createBuffer(bd);
    };
    bool changed = false, ok = true;
    for (u32 l = 0; l < layers; ++l) {
        const bool wantAct = train || l + 1 < layers;
        if (!wantAct || need[l] <= actCap_[l]) continue;
        changed = true;
        for (rhi::BufferHandle* b : {&acts_[l], &grads_[l]}) {
            if (*b) res_->destroyBuffer(*b);
            *b = 0;
        }
        acts_[l] = make(need[l], "ConvNet activations");
        if (train) grads_[l] = make(need[l], "ConvNet gradients");
        actCap_[l] = need[l];
        ok = ok && acts_[l] && (!train || grads_[l]);
    }
    if (train && partialNeed > partialCap_) {
        changed = true;
        if (partials_) res_->destroyBuffer(partials_);
        partials_ = make(partialNeed, "ConvNet gradient partials");
        partialCap_ = partialNeed;
        ok = ok && partials_;
    }
    maxRecords_ = std::max(maxRecords_, records);
    if (changed) invalidateBindings();
    if (!ok) {
        AVER_WARN("[Neural] ConvNet::reserve: tensor buffers would not allocate");
        for (u64& c : actCap_) c = 0;
        partialCap_ = 0;
    }
    return ok;
}

bool ConvNet::fits(const TensorShape& shape, bool training) const {
    if (shape.c != desc_.inChannels || shape.n < 1 || shape.h < 1 || shape.w < 1 || shape.n > maxRecords_) return false;
    TensorShape cur = shape;
    for (u32 l = 0; l < layout_.layers; ++l) {
        const TensorShape o = layout_.outDims(l, cur);
        const bool wantAct = training || l + 1 < layout_.layers;
        if (wantAct && o.count() > actCap_[l]) return false;
        if (training) {
            const u64 tiles = static_cast<u64>(divUp(o.w, kConvTile)) * divUp(o.h, kConvTile);
            if (static_cast<u64>(shape.n) * tiles * layout_.layerSize[l] > partialCap_) return false;
        }
        cur = o;
    }
    return true;
}

void ConvNet::invalidateBindings() {
    if (res_) for (CachedSet& c : sets_) if (c.set) res_->destroyBindingSet(c.set);
    sets_.clear();
    recycleNext_ = 0;
}

rhi::BindingSetHandle ConvNet::bindingSet(const Binds& b) {
    for (const CachedSet& c : sets_) if (c.key == b) return c.set;

    CachedSet* slot = nullptr;
    if (sets_.size() < kMaxCachedSets) {
        rhi::BindingSetDesc bd{};
        bd.srvCount = kSrvCount;
        bd.uavCount = kUavCount;
        for (u32 i = 0; i < kSrvCount; ++i) bd.srvKinds[i] = rhi::SlotKind::StructuredBuffer;
        for (u32 i = 0; i < kUavCount; ++i) bd.uavKinds[i] = rhi::SlotKind::StructuredBuffer;
        const rhi::BindingSetHandle set = res_->createBindingSet(bd);
        if (!set) { AVER_WARN("[Neural] createBindingSet failed"); return 0; }
        sets_.push_back({});
        sets_.back().set = set;
        slot = &sets_.back();
    } else {
        if (!warnedRecycle_) {
            AVER_WARN("[Neural] ConvNet: more than {} distinct buffer sets; recycling binding sets "
                      "(a set rewritten while an earlier dispatch is pending reads the wrong buffers)", kMaxCachedSets);
            warnedRecycle_ = true;
        }
        slot = &sets_[recycleNext_];
        recycleNext_ = (recycleNext_ + 1) % kMaxCachedSets;
    }
    slot->key = b;
    for (u32 i = 0; i < kSrvCount; ++i)
        if (b.srv[i]) res_->setSrvBuffer(slot->set, i, b.srv[i], 4, b.srvCount[i], 0);
    for (u32 i = 0; i < kUavCount; ++i)
        if (b.uav[i]) res_->setUavBuffer(slot->set, i, b.uav[i], 4, b.uavCount[i], 0);
    return slot->set;
}

void ConvNet::fillCommon(Constants& cb) const {
    static_assert(sizeof(Constants) == 128, "AverNeuralCB with the conv fields is eight float4s");
    cb.count = 1;   // conv Adam: liveCount 1 (the loss is already normalised)
    cb.maxCount = 1;
    cb.useCountBuf = 0;
    cb.weightCount = layout_.total;
    cb.lr = opt_.learningRate;
    cb.beta1 = opt_.beta1;
    cb.beta2 = opt_.beta2;
    cb.eps = opt_.epsilon;
    cb.emaDecay = opt_.weightEma;
    cb.l2 = opt_.l2;
    cb.gradScale = opt_.gradFixedScale;
    cb.gradClamp = opt_.gradClamp;
}

// Records dispatches with buffer states tracked across them: a buffer moves only when the next dispatch needs
// another state, a buffer written again as a UAV gets a UAV barrier, and finish() (or the destructor) returns
// every touched buffer to where it rests. Between dependent dispatches that is one barrier per buffer, not a
// round trip through Common for every binding.
struct ConvNet::Recorder {
    rhi::IRenderContext& ctx;
    Callers callers;
    std::vector<Rest> cur;   // touched buffers and their current state

    Recorder(rhi::IRenderContext& c, const Callers& rest) : ctx(c), callers(rest) {}
    ~Recorder() { finish(); }
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    void need(rhi::BufferHandle b, ResourceState s) {
        Rest* r = nullptr;
        for (Rest& c : cur) if (c.b == b) { r = &c; break; }
        if (!r) { cur.push_back({b, restOf(b, callers)}); r = &cur.back(); }
        if (r->s != s) {
            ctx.bufferBarrier(b, r->s, s);
            r->s = s;
        } else if (s == kWrite) {
            ctx.uavBarrierBuffer(b);
        }
    }

    template <class B, class C>
    void operator()(rhi::PipelineHandle p, rhi::BindingSetHandle set, const B& b, const C& cb, u32 gx, u32 gy, u32 gz) {
        for (u32 i = 0; i < kSrvCount; ++i) if (b.srv[i]) need(b.srv[i], kRead);
        for (u32 i = 0; i < kUavCount; ++i) if (b.uav[i]) need(b.uav[i], kWrite);
        ctx.setPipeline(p);
        ctx.setBindingSet(set);
        ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
        ctx.dispatch(gx, gy, gz);
    }

    void finish() {
        for (const Rest& r : cur) {
            const ResourceState rest = restOf(r.b, callers);
            if (r.s != rest) ctx.bufferBarrier(r.b, r.s, rest);
        }
        cur.clear();
    }
};

namespace {

// 1D grid of 64-thread groups: (gx, gy) with gx <= 65535.
void grid1d(u32 threads, u32 groupSize, u32& gx, u32& gy) {
    const u32 groups = std::max(1u, divUp(threads, groupSize));
    gx = std::min(groups, kMaxGroups);
    gy = divUp(groups, gx);
}

}  // namespace

void ConvNet::flushPending(rhi::IRenderContext& ctx) {
    if (!pendingUpload_ && !pendingReset_) return;
    const u64 bytes = static_cast<u64>(layout_.total) * sizeof(f32);

    if (pendingUpload_) {
        rhi::BufferHandle& st = staging_[stagingNext_];
        if (!st) {
            rhi::BufferDesc bd{};
            bd.bytes = bytes;
            bd.kind = rhi::BufferKind::Upload;
            bd.debugName = "ConvNet weight staging";
            st = res_->createBuffer(bd);
        }
        if (!st || !res_->writeBuffer(st, cpuMaster_.data(), bytes)) {
            AVER_WARN("[Neural] conv weight upload staging failed; the network keeps its previous weights");
            return;
        }
        stagingNext_ = (stagingNext_ + 1) % kStagingRing;
        moveBuffers(ctx, {weights_, ema_}, kRest, ResourceState::CopyDest);
        ctx.copyBuffer(weights_, st, bytes);
        ctx.copyBuffer(ema_, st, bytes);
        moveBuffers(ctx, {weights_, ema_}, ResourceState::CopyDest, kRest);
        pendingUpload_ = false;
        pendingReset_ = mode_ == ConvMode::Train;
    }

    if (pendingReset_) {
        Binds b;
        b.srv[2] = dummy_; b.srvCount[2] = 1;
        b.uav[1] = grad_;  b.uav[4] = m_;  b.uav[5] = v_;
        b.uavCount[1] = b.uavCount[4] = b.uavCount[5] = layout_.total;
        const rhi::BindingSetHandle set = bindingSet(b);
        if (!set) return;
        Constants cb{};
        fillCommon(cb);
        Recorder run(ctx, Callers{});
        run(reset_, set, b, cb, divUp(layout_.total, kGroup), 1, 1);
        pendingReset_ = false;
        step_ = 0;
    }
}

void ConvNet::forwardLayers(Recorder& run, rhi::BufferHandle in, rhi::BufferHandle out, const TensorShape& shape,
                            bool useEma) {
    const u32 layers = layout_.layers;
    TensorShape cur = shape;
    for (u32 l = 0; l < layers; ++l) {
        const TensorShape o = layout_.outDims(l, cur);
        const bool toCaller = l + 1 == layers && out;
        Binds b;
        b.srv[0] = l == 0 ? in : acts_[l - 1];
        b.srvCount[0] = l == 0 ? static_cast<u32>(cur.count()) : static_cast<u32>(actCap_[l - 1]);
        b.srv[3] = weights_; b.srvCount[3] = layout_.total;
        b.srv[4] = ema_;     b.srvCount[4] = layout_.total;
        b.uav[0] = toCaller ? out : acts_[l];
        b.uavCount[0] = toCaller ? static_cast<u32>(o.count()) : static_cast<u32>(actCap_[l]);
        const rhi::BindingSetHandle set = bindingSet(b);
        if (!set) return;

        Constants cb{};
        fillCommon(cb);
        cb.useEma = useEma ? 1u : 0u;
        cb.n = cur.n; cb.inH = cur.h; cb.inW = cur.w; cb.outH = o.h; cb.outW = o.w;
        cb.wOffset = layout_.wOffset[l];
        cb.bOffset = layout_.bOffset[l];
        cb.layerSize = layout_.layerSize[l];
        run(pipes_[l].forward, set, b, cb, divUp(o.w, kConvTile * fwdPx(desc_.layers[l].kernel)) * divUp(o.c, kConvCoBlock),
            divUp(o.h, kConvTile), cur.n);
        cur = o;
    }
}

bool ConvNet::recordInfer(rhi::IRenderContext& ctx, rhi::BufferHandle in, rhi::BufferHandle out,
                          const TensorShape& shape, bool useEma, const IoStates& states) {
    if (!valid() || !in || !out) return false;
    if (!fits(shape, false)) {
        if (!warnedSize_) {
            AVER_WARN("[Neural] ConvNet::recordInfer: shape {}x{}x{}x{} was not reserved", shape.n, shape.c, shape.h, shape.w);
            warnedSize_ = true;
        }
        return false;
    }
    std::optional<rhi::ScopedGpuStat> gpuStat;
    if (gpuStats_) gpuStat.emplace(ctx, "Neural.ConvInfer");
    flushPending(ctx);
    Recorder run(ctx, Callers{{{in, states.input}, {out, states.output}}});
    forwardLayers(run, in, out, shape, useEma);
    return true;
}

bool ConvNet::recordTrain(rhi::IRenderContext& ctx, rhi::BufferHandle in, rhi::BufferHandle target,
                          rhi::BufferHandle posWeight, const TensorShape& shape, f32 lossNorm, const IoStates& states,
                          ConvLossWeight weightKind) {
    if (!valid() || mode_ != ConvMode::Train || !in || !target || !posWeight || !(lossNorm > 0.0f)) return false;
    if (!fits(shape, true)) {
        if (!warnedSize_) {
            AVER_WARN("[Neural] ConvNet::recordTrain: shape {}x{}x{}x{} was not reserved", shape.n, shape.c, shape.h, shape.w);
            warnedSize_ = true;
        }
        return false;
    }
    std::optional<rhi::ScopedGpuStat> gpuStat;
    if (gpuStats_) gpuStat.emplace(ctx, "Neural.ConvTrain");
    flushPending(ctx);

    const u32 layers = layout_.layers;
    TensorShape shapes[kConvMaxLayers + 1];
    shapes[0] = shape;
    for (u32 l = 0; l < layers; ++l) shapes[l + 1] = layout_.outDims(l, shapes[l]);
    const TensorShape& head = shapes[layers];

    Recorder run(ctx, Callers{{{in, states.input}, {target, states.input}, {posWeight, states.input}}});
    forwardLayers(run, in, 0, shape, false);

    // dL/d(head output).
    {
        Binds b;
        b.srv[0] = acts_[layers - 1]; b.srvCount[0] = static_cast<u32>(actCap_[layers - 1]);
        b.srv[1] = target;            b.srvCount[1] = static_cast<u32>(head.count());
        b.srv[5] = posWeight;         b.srvCount[5] = weightFloats(head, weightKind);
        b.uav[0] = grads_[layers - 1]; b.uavCount[0] = static_cast<u32>(actCap_[layers - 1]);
        const rhi::BindingSetHandle set = bindingSet(b);
        if (!set) return false;
        Constants cb{};
        fillCommon(cb);
        cb.n = head.n; cb.outH = head.h; cb.outW = head.w;
        cb.elemCount = static_cast<u32>(head.count());
        cb.lossNorm = lossNorm;
        cb.weightPerElem = weightKind == ConvLossWeight::PerElement ? 1u : 0u;
        u32 gx = 0, gy = 0;
        grid1d(cb.elemCount, kGroup, gx, gy);
        cb.groupsX = gx;
        run(loss_, set, b, cb, gx, gy, 1);
    }

    // Layers last to first: activation backward, weight partials, ordered reduce, then dX for the layer below.
    for (u32 l = layers; l-- > 0;) {
        const TensorShape& is = shapes[l];
        const TensorShape& os = shapes[l + 1];
        const ConvLayerDesc& L = desc_.layers[l];
        Constants base{};
        fillCommon(base);
        base.n = is.n; base.inH = is.h; base.inW = is.w; base.outH = os.h; base.outW = os.w;
        base.tilesX = divUp(os.w, kConvTile);
        base.tilesY = divUp(os.h, kConvTile);
        base.wOffset = layout_.wOffset[l];
        base.bOffset = layout_.bOffset[l];
        base.layerSize = layout_.layerSize[l];
        base.partialCount = os.n * base.tilesX * base.tilesY;

        if (L.act == Activation::ReLU) {
            Binds b;
            b.srv[0] = acts_[l];  b.srvCount[0] = static_cast<u32>(actCap_[l]);
            b.uav[0] = grads_[l]; b.uavCount[0] = static_cast<u32>(actCap_[l]);
            const rhi::BindingSetHandle set = bindingSet(b);
            if (!set) return false;
            Constants cb = base;
            cb.elemCount = static_cast<u32>(os.count());
            u32 gx = 0, gy = 0;
            grid1d(cb.elemCount, kGroup, gx, gy);
            cb.groupsX = gx;
            run(pipes_[l].actBackward, set, b, cb, gx, gy, 1);
        }
        {
            Binds b;
            b.srv[0] = l == 0 ? in : acts_[l - 1];
            b.srvCount[0] = l == 0 ? static_cast<u32>(is.count()) : static_cast<u32>(actCap_[l - 1]);
            b.srv[1] = grads_[l];  b.srvCount[1] = static_cast<u32>(actCap_[l]);
            b.uav[6] = partials_;  b.uavCount[6] = static_cast<u32>(partialCap_);
            const rhi::BindingSetHandle set = bindingSet(b);
            if (!set) return false;
            const u32 blocks = divUp(L.cout, kBwCoBlock) * divUp(L.cin, kBwCiBlock);
            run(pipes_[l].backwardWeights, set, b, base, blocks, base.tilesX * base.tilesY, os.n);
        }
        {
            Binds b;
            b.uav[1] = grad_;     b.uavCount[1] = layout_.total;
            b.uav[6] = partials_; b.uavCount[6] = static_cast<u32>(partialCap_);
            const rhi::BindingSetHandle set = bindingSet(b);
            if (!set) return false;
            Constants cb = base;
            u32 gx = 0, gy = 0;
            grid1d(cb.layerSize, kGroup, gx, gy);
            cb.groupsX = gx;
            run(reduce_, set, b, cb, gx, gy, 1);
        }
        if (l > 0) {
            Binds b;
            b.srv[0] = grads_[l];     b.srvCount[0] = static_cast<u32>(actCap_[l]);
            b.srv[3] = weights_;      b.srvCount[3] = layout_.total;
            b.uav[0] = grads_[l - 1]; b.uavCount[0] = static_cast<u32>(actCap_[l - 1]);
            const rhi::BindingSetHandle set = bindingSet(b);
            if (!set) return false;
            run(pipes_[l].backwardData, set, b, base, divUp(is.w, kConvTile * kBwdPx) * divUp(is.c, kConvCoBlock),
                divUp(is.h, kConvTile), is.n);
        }
    }

    // Adam over the whole accumulator (liveCount 1).
    {
        Binds b;
        b.srv[2] = dummy_; b.srvCount[2] = 1;
        b.uav[1] = grad_; b.uav[2] = weights_; b.uav[3] = ema_; b.uav[4] = m_; b.uav[5] = v_;
        for (u32 i = 1; i <= 5; ++i) b.uavCount[i] = layout_.total;
        const rhi::BindingSetHandle set = bindingSet(b);
        if (!set) return false;
        ++step_;
        Constants cb{};
        fillCommon(cb);
        cb.bc1 = adamBiasCorrection(opt_.beta1, step_);
        cb.bc2 = adamBiasCorrection(opt_.beta2, step_);
        run(adam_, set, b, cb, divUp(layout_.total, kGroup), 1, 1);
    }
    return true;
}

bool ConvNet::recordEvaluate(rhi::IRenderContext& ctx, rhi::BufferHandle in, rhi::BufferHandle target,
                             rhi::BufferHandle posWeight, rhi::BufferHandle lossPerRecord, const TensorShape& shape,
                             f32 lossNorm, bool useEma, const IoStates& states, ConvLossWeight weightKind) {
    if (!valid() || mode_ != ConvMode::Train || !in || !target || !posWeight || !lossPerRecord || !(lossNorm > 0.0f))
        return false;
    if (!fits(shape, true)) {
        if (!warnedSize_) {
            AVER_WARN("[Neural] ConvNet::recordEvaluate: shape {}x{}x{}x{} was not reserved", shape.n, shape.c, shape.h,
                      shape.w);
            warnedSize_ = true;
        }
        return false;
    }
    std::optional<rhi::ScopedGpuStat> gpuStat;
    if (gpuStats_) gpuStat.emplace(ctx, "Neural.ConvEvaluate");
    flushPending(ctx);
    Recorder run(ctx, Callers{{{in, states.input}, {target, states.input}, {posWeight, states.input},
                               {lossPerRecord, states.output}}});
    forwardLayers(run, in, 0, shape, useEma);

    const u32 layers = layout_.layers;
    const TensorShape head = outputShape(shape);
    Binds b;
    b.srv[0] = acts_[layers - 1]; b.srvCount[0] = static_cast<u32>(actCap_[layers - 1]);
    b.srv[1] = target;            b.srvCount[1] = static_cast<u32>(head.count());
    b.srv[5] = posWeight;         b.srvCount[5] = weightFloats(head, weightKind);
    b.uav[0] = lossPerRecord;     b.uavCount[0] = head.n;
    const rhi::BindingSetHandle set = bindingSet(b);
    if (!set) return false;
    Constants cb{};
    fillCommon(cb);
    cb.n = head.n; cb.outH = head.h; cb.outW = head.w;
    cb.lossNorm = lossNorm;
    cb.weightPerElem = weightKind == ConvLossWeight::PerElement ? 1u : 0u;
    u32 gx = 0, gy = 0;
    grid1d(head.n, 1, gx, gy);   // one group per record
    cb.groupsX = gx;
    run(eval_, set, b, cb, gx, gy, 1);
    return true;
}

// ---------------------------------------------------------------- weights

bool ConvNet::setLearningRate(f32 lr) {
    if (!(lr > 0.0f) || !(lr < 3.4e38f)) return false;
    opt_.learningRate = lr;
    return true;
}

bool ConvNet::uploadWeights(std::span<const f32> w) {
    if (!valid() || w.size() != layout_.total) return false;
    cpuMaster_.assign(w.begin(), w.end());
    cpuEma_ = cpuMaster_;
    pendingUpload_ = true;
    pendingReset_ = mode_ == ConvMode::Train;
    step_ = 0;
    return true;
}

bool ConvNet::saveWeights(const std::string& path, bool ema) const {
    if (!valid()) return false;
    const bool hasIo = !io_.inScale.empty() || !io_.inBias.empty() || !io_.outScale.empty() || !io_.outBias.empty();
    return saveConvWeightFile(path, desc_, ema ? std::span<const f32>(cpuEma_) : std::span<const f32>(cpuMaster_),
                              hasIo ? &io_ : nullptr);
}

bool ConvNet::loadWeights(const std::string& path) {
    if (!valid()) return false;
    ConvNetDesc fd = desc_;
    std::vector<f32> w;
    ConvIoAffine io;
    if (!loadConvWeightFile(path, fd, w, &io)) {
        AVER_WARN("[Neural] {} is not a readable AVNN v2 conv weight file", path);
        return false;
    }
    bool same = fd.inChannels == desc_.inChannels && fd.layers.size() == desc_.layers.size();
    for (usize l = 0; same && l < fd.layers.size(); ++l) {
        const ConvLayerDesc &a = fd.layers[l], &b = desc_.layers[l];
        same = a.cin == b.cin && a.cout == b.cout && a.kernel == b.kernel && a.stride == b.stride && a.act == b.act &&
               a.bias == b.bias;
    }
    if (!same) {
        AVER_WARN("[Neural] {} holds a different conv network shape than this one; not loaded", path);
        return false;
    }
    if (!uploadWeights(w)) return false;
    io_ = std::move(io);
    return true;
}

bool ConvNet::recordReadback(rhi::IRenderContext& ctx) {
    if (!valid()) return false;
    const u64 bytes = static_cast<u64>(layout_.total) * sizeof(f32);
    if (!readback_) {
        rhi::BufferDesc bd{};
        bd.bytes = bytes * 2;
        bd.kind = rhi::BufferKind::Readback;
        bd.debugName = "ConvNet weight readback";
        readback_ = res_->createBuffer(bd);
        if (!readback_) { AVER_WARN("[Neural] conv readback buffer would not allocate"); return false; }
    }
    flushPending(ctx);
    moveBuffers(ctx, {weights_, ema_}, kRest, ResourceState::CopySource);
    ctx.copyBuffer(readback_, weights_, bytes, 0, 0);
    ctx.copyBuffer(readback_, ema_, bytes, bytes, 0);
    moveBuffers(ctx, {weights_, ema_}, ResourceState::CopySource, kRest);
    return true;
}

bool ConvNet::collectWeights() {
    if (!valid() || !readback_ || pendingUpload_) return false;
    const u64 bytes = static_cast<u64>(layout_.total) * sizeof(f32);
    std::vector<f32> master(layout_.total), ema(layout_.total);
    if (!res_->readBuffer(readback_, master.data(), bytes, 0) || !res_->readBuffer(readback_, ema.data(), bytes, bytes))
        return false;
    cpuMaster_ = std::move(master);
    cpuEma_ = std::move(ema);
    return true;
}

}  // namespace aver::render::neural
