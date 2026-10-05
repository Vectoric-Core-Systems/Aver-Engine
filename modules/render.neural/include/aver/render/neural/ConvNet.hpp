// GPU convolutional network (NRD2, docs/rendering/NRD2.md): inference and in-engine training in portable
// fp32 HLSL (shaders/aver_neural_conv.hlsl). ConvNetReference is the spec; README.md has the kernel list.
// Tensors are NCHW StructuredBuffer<float>, one buffer per tensor. The caller owns inputs, outputs,
// targets and per-position weights; the network owns weights, EMA, Adam state, activations, gradients
// and gradient partials.
#pragma once

#include <aver/core/Types.hpp>
#include <aver/render/neural/ConvNetReference.hpp>
#include <aver/render/neural/Mlp.hpp>   // IoStates
#include <aver/render/neural/WeightFile.hpp>
#include <aver/rhi/RHI.hpp>
#include <aver/rhi/RHIResources.hpp>

#include <span>
#include <string>
#include <vector>

namespace aver::render::neural {

// Infer compiles the forward kernels only; Train adds loss, backward, reduce, evaluate and Adam.
enum class ConvMode : u32 { Infer = 0, Train = 1 };

class ConvNet {
public:
    ConvNet() = default;
    ~ConvNet();
    ConvNet(const ConvNet&)            = delete;
    ConvNet& operator=(const ConvNet&) = delete;

    // Validates, compiles one pipeline per layer per kernel, allocates the weight buffers and
    // initialises them (initConvWeights). False (warned) on an invalid descriptor, a shader or a buffer.
    bool create(rhi::IDevice& dev, const ConvNetDesc& desc, const OptimiserDesc& opt, ConvMode mode = ConvMode::Infer);
    void destroy();
    [[nodiscard]] bool valid() const { return !pipes_.empty() && pipes_[0].forward != 0; }
    [[nodiscard]] ConvMode mode() const { return mode_; }

    [[nodiscard]] const ConvNetDesc& desc() const { return desc_; }
    [[nodiscard]] const OptimiserDesc& optimiser() const { return opt_; }
    [[nodiscard]] const ConvLayout& layout() const { return layout_; }
    [[nodiscard]] u32 weightCount() const { return layout_.total; }
    [[nodiscard]] TensorShape outputShape(const TensorShape& in) const;
    [[nodiscard]] u32 step() const { return step_; }

    // Sizes the network-owned tensors for every shape it will see (max over the list). Must precede the
    // first record*() call; reallocating destroys buffers, so call it only with no recorded work pending.
    bool reserve(std::span<const TensorShape> shapes);

    // Forward pass: `in` holds shape.count() floats, `out` (allowUnorderedAccess) outputShape(shape).count().
    bool recordInfer(rhi::IRenderContext& ctx, rhi::BufferHandle in, rhi::BufferHandle out, const TensorShape& shape,
                     bool useEma = true, const IoStates& states = {});

    // One Adam step (master weights; EMA follows) on weighted L2: target is outputShape(shape).count() floats,
    // posWeight n * oh * ow. lossNorm as ConvNetReference::trainBatch. Train mode only.
    bool recordTrain(rhi::IRenderContext& ctx, rhi::BufferHandle in, rhi::BufferHandle target,
                     rhi::BufferHandle posWeight, const TensorShape& shape, f32 lossNorm, const IoStates& states = {});

    // Loss per record (lossPerRecord: n floats, allowUnorderedAccess; their sum is ConvNetReference::evaluate).
    // No state change. Train mode only.
    bool recordEvaluate(rhi::IRenderContext& ctx, rhi::BufferHandle in, rhi::BufferHandle target,
                        rhi::BufferHandle posWeight, rhi::BufferHandle lossPerRecord, const TensorShape& shape,
                        f32 lossNorm, bool useEma = true, const IoStates& states = {});

    // Master = EMA = w; Adam state reset with the next record*() call. Staging ring as Mlp.
    bool uploadWeights(std::span<const f32> w);
    // AVNN v2 (WeightFile.hpp). saveWeights writes the last CPU-known copy (see Mlp, "the readback gap")
    // and the io affine when one is set; loadWeights rejects a different shape and keeps the file's affine.
    bool saveWeights(const std::string& path, bool ema = true) const;
    bool loadWeights(const std::string& path);
    [[nodiscard]] const ConvIoAffine& ioAffine() const { return io_; }
    void setIoAffine(ConvIoAffine io) { io_ = std::move(io); }

    bool recordReadback(rhi::IRenderContext& ctx);
    bool collectWeights();
    [[nodiscard]] std::span<const f32> cpuWeights(bool ema = true) const {
        return ema ? std::span<const f32>(cpuEma_) : std::span<const f32>(cpuMaster_);
    }

    bool setLearningRate(f32 lr);

    // Binding sets are cached per bound-buffer tuple; call before destroying or reallocating a buffer
    // that was passed in.
    void invalidateBindings();

    static constexpr u32 kStagingRing = 3;
    static constexpr u32 kMaxCachedSets = 128;   // whole network; beyond it sets are recycled (warn once)

private:
    struct LayerPipes {
        rhi::PipelineHandle forward = 0, actBackward = 0, backwardData = 0, backwardWeights = 0;
    };
    // One binding per slot: handle and element count (0 = unbound).
    struct Binds {
        rhi::BufferHandle srv[6] = {};
        u32 srvCount[6] = {};
        rhi::BufferHandle uav[7] = {};
        u32 uavCount[7] = {};
        bool operator==(const Binds&) const = default;
    };
    struct CachedSet {
        Binds key;
        rhi::BindingSetHandle set = 0;
    };
    struct Constants;

    rhi::PipelineHandle compile(const char* entry, u32 layer);
    rhi::BindingSetHandle bindingSet(const Binds& b);
    void flushPending(rhi::IRenderContext& ctx);
    bool fits(const TensorShape& shape, bool training) const;
    void forwardLayers(rhi::IRenderContext& ctx, rhi::BufferHandle in, rhi::BufferHandle out,
                       const TensorShape& shape, bool useEma, rhi::ResourceState inRest, rhi::ResourceState outRest);
    void fillCommon(Constants& cb) const;

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    ConvNetDesc   desc_{};
    OptimiserDesc opt_{};
    ConvLayout    layout_{};
    ConvMode      mode_ = ConvMode::Infer;
    ConvIoAffine  io_{};

    std::vector<LayerPipes> pipes_;
    rhi::PipelineHandle loss_ = 0, eval_ = 0, reduce_ = 0, adam_ = 0, reset_ = 0;

    rhi::BufferHandle weights_ = 0, ema_ = 0, m_ = 0, v_ = 0, grad_ = 0, dummy_ = 0;
    // Per layer: post-activation output (the head's only in Train mode) and dL/d(output) (Train).
    std::vector<rhi::BufferHandle> acts_, grads_;
    std::vector<u64> actCap_;   // floats each act/grad buffer holds
    rhi::BufferHandle partials_ = 0;
    u64 partialCap_ = 0;
    u32 maxRecords_ = 0;        // reserved n (eval writes one float per record)

    rhi::BufferHandle staging_[kStagingRing] = {};
    u32 stagingNext_ = 0;
    rhi::BufferHandle readback_ = 0;   // [master | ema]

    std::vector<CachedSet> sets_;
    u32  recycleNext_ = 0;
    bool warnedRecycle_ = false;
    bool warnedSize_ = false;

    std::vector<f32> cpuMaster_, cpuEma_;
    bool pendingUpload_ = false;
    bool pendingReset_  = false;
    u32  step_ = 0;
};

}  // namespace aver::render::neural
