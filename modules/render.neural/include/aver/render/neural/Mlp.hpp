// GPU MLP network for radiance cache and frame interpolation (docs/rendering/NEURAC.md).
// Infers and trains entirely on device; see aver_neural_mlp.hlsl and README.md.
// Buffers managed by caller; network owns weights, EMA, Adam state, gradients.
#pragma once

#include <aver/core/Types.hpp>
#include <aver/render/neural/MlpReference.hpp>
#include <aver/rhi/RHI.hpp>
#include <aver/rhi/RHIResources.hpp>

#include <span>
#include <string>
#include <vector>

namespace aver::render::neural {

// Which kernel family is running (v1 uses only PortableFp32; others are planned backends).
enum class Backend : u32 { PortableFp32 = 0 };

// Record count on the CPU. Distinct type to avoid ambiguity with BufferHandle (both u32).
struct CpuCount { u32 value = 0; };

// Resource states the caller's buffers rest in before/after kernel calls.
struct IoStates {
    rhi::ResourceState input  = rhi::ResourceState::Common;
    rhi::ResourceState output = rhi::ResourceState::Common;
};

class Mlp {
public:
    Mlp() = default;
    ~Mlp();
    Mlp(const Mlp&)            = delete;
    Mlp& operator=(const Mlp&) = delete;

    // Validates descriptors, compiles pipelines (infer, train-gradient, Adam, reset),
    // allocates weight/EMA/Adam/accumulator buffers, and initializes with He-uniform seeded by desc.seed.
    // False (warned once) when descriptors are out of range, shaders won't compile, or buffers won't build.
    bool create(rhi::IDevice& dev, const MlpDesc& desc, const OptimiserDesc& opt);
    void destroy();
    [[nodiscard]] bool valid() const { return pipelines_[0] != 0; }
    [[nodiscard]] Backend backend() const { return Backend::PortableFp32; }

    [[nodiscard]] const MlpDesc& desc() const { return desc_; }
    [[nodiscard]] const OptimiserDesc& optimiser() const { return opt_; }
    // Floats in the flat weight array (biases included), in the order MlpReference.hpp documents.
    [[nodiscard]] u32 weightCount() const { return layout_.total; }
    // Largest safe batch before accumulator overflow (recordTrain warns above it).
    [[nodiscard]] u32 safeBatchLimit() const { return neural::safeBatchLimit(opt_); }

    // Learning-rate schedule: step size for the next recordTrain (travels in constants). False if non-finite or <= 0.
    bool setLearningRate(f32 lr);

    // ---- inference -------------------------------------------------------------------------
    // records: StructuredBuffer<float> of maxCount * inputs floats.
    // outputs: RW structured float buffer (allowUnorderedAccess) of maxCount * outputs floats.
    // countBuffer: uint buffer; element 0 is live record count (GPU-written). maxCount bounds dispatch.
    // useEma: read smoothed weights (default, for rendering) or raw master weights.
    // False when invalid or maxCount is zero/too large.
    bool recordInfer(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle outputs,
                     rhi::BufferHandle countBuffer, u32 maxCount, bool useEma = true,
                     const IoStates& states = {});
    // Same with CPU-known count (no count buffer read).
    bool recordInfer(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle outputs,
                     CpuCount count, bool useEma = true, const IoStates& states = {});

    // ---- training --------------------------------------------------------------------------
    // One optimiser step over the batch. targets: maxCount * outputs floats.
    // Uses MEAN gradient over live count. Always trains master weights; EMA follows.
    bool recordTrain(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle targets,
                     rhi::BufferHandle countBuffer, u32 maxCount, const IoStates& states = {});
    bool recordTrain(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle targets,
                     CpuCount count, const IoStates& states = {});

    // ---- weights ---------------------------------------------------------------------------
    // Replace master and EMA with `w`, reset Adam moments and step counter, queue GPU upload.
    // Staging is a ring of kStagingRing buffers; safe unless kStagingRing more uploads queue before first finishes.
    bool uploadWeights(std::span<const f32> w);

    // Binary file (see MlpReference.hpp). saveWeights writes the last CPU-known copy
    // (init, last upload/loadWeights, or last collectWeights), NOT the live GPU state.
    // `ema` picks which set: EMA (default, for inference) or master weights.
    bool saveWeights(const std::string& path, bool ema = true) const;
    bool loadWeights(const std::string& path);

    // Readback: recordReadback() copies GPU weights; collectWeights() reads them after GPU finishes.
    bool recordReadback(rhi::IRenderContext& ctx);
    bool collectWeights();

    // The CPU-known copies (what saveWeights writes).
    [[nodiscard]] std::span<const f32> cpuWeights(bool ema = true) const {
        return ema ? std::span<const f32>(cpuEma_) : std::span<const f32>(cpuMaster_);
    }

    // Binding sets cached per (buffers, maxCount). Call invalidateBindings() if caller destroys/reallocates buffers.
    void invalidateBindings();

    static constexpr u32 kStagingRing = 3;
    static constexpr u32 kMaxCachedSets = 16;   // per kernel; beyond it sets are recycled (warn once)

private:
    enum Kernel : u32 { kInfer = 0, kTrainGrad = 1, kAdam = 2, kReset = 3, kKernelCount = 4 };

    rhi::BindingSetHandle bindingSet(Kernel k, rhi::BufferHandle a, rhi::BufferHandle b,
                                     rhi::BufferHandle count, u32 maxCount);
    // Uploads queued weights and resets Adam state before any kernel that reads either.
    void flushPending(rhi::IRenderContext& ctx);
    bool recordInferImpl(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle outputs,
                         rhi::BufferHandle countBuffer, u32 cpuCount, u32 maxCount, bool useEma,
                         const IoStates& states);
    bool recordTrainImpl(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle targets,
                         rhi::BufferHandle countBuffer, u32 cpuCount, u32 maxCount,
                         const IoStates& states);

    struct CachedSet {
        rhi::BufferHandle key[3] = {};
        u32 maxCount = 0;
        rhi::BindingSetHandle set = 0;
    };

    rhi::IDevice*          dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    MlpDesc       desc_{};
    OptimiserDesc opt_{};
    MlpLayout     layout_{};

    rhi::PipelineHandle pipelines_[kKernelCount] = {};
    // Weights (master), EMA, Adam m and v, int gradient accumulator: RWStructuredBuffers.
    rhi::BufferHandle weights_ = 0, ema_ = 0, m_ = 0, v_ = 0, grad_ = 0;
    // Stand-in bound when count lives on the CPU (never read in that case).
    rhi::BufferHandle dummy_ = 0;
    rhi::BufferHandle staging_[kStagingRing] = {};
    u32 stagingNext_ = 0;
    rhi::BufferHandle readback_ = 0;   // [master | ema]

    std::vector<CachedSet> sets_[kAdam + 1];
    u32  recycleNext_[kAdam + 1] = {};
    bool warnedRecycle_ = false;
    bool warnedBatch_ = false;

    std::vector<f32> cpuMaster_, cpuEma_;
    bool pendingUpload_ = false;   // cpuMaster_/cpuEma_ must reach the GPU
    bool pendingReset_  = false;   // Adam moments and the accumulator must be zeroed
    u32  step_ = 0;                // Adam steps taken since the last upload
};

}  // namespace aver::render::neural
