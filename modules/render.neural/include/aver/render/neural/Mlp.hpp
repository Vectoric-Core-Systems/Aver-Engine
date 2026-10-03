// Mlp -- Aver.Render.Neural's GPU network: a small fully connected MLP that infers and trains
// entirely on the device, in portable fp32 HLSL (see shaders/aver_neural_mlp.hlsl and README.md).
//
// WHAT IT IS FOR. The radiance cache (docs/rendering/RADIANCE_CACHE.md) and, later, frame
// interpolation need tiny networks that run INSIDE the frame: records produced by one compute pass,
// answers consumed by the next, training steps interleaved with rendering, and no CPU round trip.
// So the interface is "structured buffers in, structured buffers out, recorded into the caller's
// IRenderContext", exactly the shape of Aver.Render.Denoise -- RHI-only, no Voxi types, and the
// caller owns every buffer it feeds in. The network owns its weights, EMA copy, Adam state and
// gradient accumulator.
//
// RUNTIME-COMPILED HLSL through the engine's shader compiler, one pipeline set per network shape
// (the shape arrives as DXC defines). create() is the only place that can fail on a device that
// cannot run it; it says so once at WARN and returns false, and the caller runs without a network.
//
// THE CPU TWIN. MlpReference.hpp implements the same maths on the CPU and is the spec for the HLSL.
// The descriptor types live in that header (RHI-free, so the reference and its test need no device)
// and are re-exported here by inclusion.
//
// RESOURCE STATES. Nothing in this RHI transitions implicitly. The network's own buffers rest in
// Common between calls. A CALLER's buffers (records, targets, count, outputs) are Default-kind
// buffers (never Upload: those cannot be barriered) that the caller says the state of via IoStates;
// each record*() moves them to what its kernels need and puts them back before it returns.
//
// FRAMES IN FLIGHT. Two things in here are written by the CPU while the GPU may still be reading an
// earlier frame, and neither is fenced (the RHI exposes no fence to a module): the weight-upload
// staging buffers, and the binding sets reused across calls. Each is handled as safely as the RHI
// allows and the residual hazard is documented where it lives (uploadWeights, invalidateBindings).
#pragma once

#include <aver/core/Types.hpp>
#include <aver/render/neural/MlpReference.hpp>
#include <aver/rhi/RHI.hpp>
#include <aver/rhi/RHIResources.hpp>

#include <span>
#include <string>
#include <vector>

namespace aver::render::neural {

// Which kernel family is running. Only the first exists in v1; the rest are the planned backends
// (design doc section 6/7), declared here so the numeric values are settled and callers can already
// switch on backend():
//   PortableFp16     = 1   packed fp16 HLSL, gated on a native-16-bit-ops caps query
//   VulkanCoopMatrix = 2   VK_KHR_cooperative_matrix (RDNA3 WMMA, NVIDIA tensor cores)
//   D3D12LinAlg      = 3   SM 6.10 linear-algebra intrinsics, when retail
enum class Backend : u32 { PortableFp32 = 0 };

// A record count that lives on the CPU. A distinct type, not a bare u32, because BufferHandle is
// itself a u32: recordInfer(ctx, rec, out, 128u, true) would otherwise be ambiguous between "128 is
// a count" and "128 is a buffer handle".
struct CpuCount { u32 value = 0; };

// The states the caller's buffers rest in. Inputs (records, targets, the count buffer) and the
// output buffer are each moved to what the kernels need and back. A producer that leaves its
// buffers UnorderedAccess says so here; everything at rest in Common needs nothing.
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

    // Validates the descriptors, compiles the four pipelines (infer, train-gradient, Adam, reset),
    // allocates the weight / EMA / Adam / accumulator buffers, and computes the deterministic
    // He-uniform init (initWeights(desc), seeded by desc.seed), which is uploaded on the first
    // record*() call. EMA starts equal to the master weights. False -- said once at WARN -- when
    // the descriptors are out of range, a shader will not compile, or a pipeline or buffer will not
    // build; the caller then runs without a network.
    bool create(rhi::IDevice& dev, const MlpDesc& desc, const OptimiserDesc& opt);
    void destroy();
    [[nodiscard]] bool valid() const { return pipelines_[0] != 0; }
    [[nodiscard]] Backend backend() const { return Backend::PortableFp32; }

    [[nodiscard]] const MlpDesc& desc() const { return desc_; }
    [[nodiscard]] const OptimiserDesc& optimiser() const { return opt_; }
    // Floats in the flat weight array (biases included), in the order MlpReference.hpp documents.
    [[nodiscard]] u32 weightCount() const { return layout_.total; }
    // The largest batch for which an accumulator overflow is impossible even in the worst case
    // (MlpReference.hpp safeBatchLimit). recordTrain warns once above it.
    [[nodiscard]] u32 safeBatchLimit() const { return neural::safeBatchLimit(opt_); }

    // ---- inference -------------------------------------------------------------------------
    // records: a StructuredBuffer<float> of maxCount * inputs floats.
    // outputs: an RW structured float buffer (created with allowUnorderedAccess) of
    //          maxCount * outputs floats.
    // countBuffer: a uint buffer whose element 0 is the LIVE record count, written by the GPU, so a
    //          producer pass can decide how many records exist. maxCount bounds both the dispatch
    //          and the count (a larger live count is clamped to it); threads past the count exit
    //          without touching the buffers.
    // useEma: read the smoothed weights (the default; what a renderer should show) or the raw ones.
    // False when invalid or when maxCount is zero or too large to dispatch.
    bool recordInfer(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle outputs,
                     rhi::BufferHandle countBuffer, u32 maxCount, bool useEma = true,
                     const IoStates& states = {});
    // The same with the count known on the CPU: no count buffer is read.
    bool recordInfer(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle outputs,
                     CpuCount count, bool useEma = true, const IoStates& states = {});

    // ---- training --------------------------------------------------------------------------
    // One optimiser step over the batch. targets: maxCount * outputs floats. Zeroes nothing the
    // caller owns; consumes the accumulator it fills itself. The step uses the MEAN gradient over
    // the live count. Always trains the master weights; EMA follows. Same count conventions as
    // recordInfer. The training loss is not read back (v1: no GPU->CPU path suits a per-step
    // value); evaluate the loss through whatever the caller already measures.
    bool recordTrain(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle targets,
                     rhi::BufferHandle countBuffer, u32 maxCount, const IoStates& states = {});
    bool recordTrain(rhi::IRenderContext& ctx, rhi::BufferHandle records, rhi::BufferHandle targets,
                     CpuCount count, const IoStates& states = {});

    // ---- weights ---------------------------------------------------------------------------
    // Replaces master AND EMA with `w` (weightCount() floats), resets Adam's moments and step
    // counter, and queues the upload for the next record*() call (a CPU write to a staging buffer,
    // then a GPU copy -- there is no way to write a Default buffer from the CPU).
    //
    // FRAME-IN-FLIGHT NOTE: the staging buffer is written immediately and unsynchronised. Staging
    // is a ring of kStagingRing buffers, so an upload is safe unless kStagingRing more uploads are
    // queued while the first is still executing -- not a pattern anything here produces.
    bool uploadWeights(std::span<const f32> w);

    // Binary file, see MlpReference.hpp for the format. saveWeights writes the last CPU-KNOWN copy
    // of the weights -- the initial init, the last upload / loadWeights, or the last
    // collectWeights() -- NOT the live GPU state, because the RHI gives a module no fence and so no
    // safe moment to read the GPU's weights back on its own (see recordReadback). `ema` picks which
    // set: the EMA (default; what inference uses) or the master weights.
    // loadWeights requires the file's shape to equal this network's (seed excepted) and uploads.
    bool saveWeights(const std::string& path, bool ema = true) const;
    bool loadWeights(const std::string& path);

    // THE READBACK GAP, AND HOW TO CLOSE IT. recordReadback() records a GPU copy of master+EMA into
    // a readback buffer; once the caller KNOWS the GPU has finished that frame (the engine's
    // waitIdle, or enough frames later), collectWeights() reads it into the CPU copies so the next
    // saveWeights() writes trained weights. Refuses (false) while an upload is still pending, since
    // the CPU copies are then the newer ones.
    bool recordReadback(rhi::IRenderContext& ctx);
    bool collectWeights();

    // The CPU-known copies (what saveWeights writes).
    [[nodiscard]] std::span<const f32> cpuWeights(bool ema = true) const {
        return ema ? std::span<const f32>(cpuEma_) : std::span<const f32>(cpuMaster_);
    }

    // BINDING SETS ARE CACHED per (buffers, maxCount) because a set rewritten between two recorded
    // dispatches would hand the first one the second's buffers. The cache holds a set bound to a
    // caller's buffers; if the caller DESTROYS or REALLOCATES such a buffer, the set still points at
    // the dead descriptor and a later dispatch faults the device. Call this first -- it drops the
    // cached sets (it destroys nothing the caller owns).
    void invalidateBindings();

    static constexpr u32 kStagingRing = 3;
    static constexpr u32 kMaxCachedSets = 16;   // per kernel; beyond it sets are recycled (WARN once)

private:
    enum Kernel : u32 { kInfer = 0, kTrainGrad = 1, kAdam = 2, kReset = 3, kKernelCount = 4 };

    rhi::BindingSetHandle bindingSet(Kernel k, rhi::BufferHandle a, rhi::BufferHandle b,
                                     rhi::BufferHandle count, u32 maxCount);
    // Uploads queued weights and resets Adam state, before any kernel that reads either.
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
    // Weights (master), EMA, Adam m and v, the int gradient accumulator: all RWStructuredBuffers.
    rhi::BufferHandle weights_ = 0, ema_ = 0, m_ = 0, v_ = 0, grad_ = 0;
    // A 16-byte stand-in bound to the count slot when the count lives on the CPU, so the slot's
    // descriptor is always valid. Never read in that case.
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
