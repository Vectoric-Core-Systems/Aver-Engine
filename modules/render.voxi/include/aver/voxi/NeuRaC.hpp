// NeuRaC -- the GPU half of the Voxi radiance cache (docs/rendering/NEURAC.md).
// Owns accumulator and cell buffers, per-frame RcInfo ring, and resolve compute pipeline.
// Does NOT own scatter or lookup: those are in voxi_neurac_io.hlsli (AVER_NEURAC twin pipelines),
// reading/writing through Voxi's table 0 (t22 info, u20 accum, u21 cells).
// UAV-only buffers in COMMON state. Synchronisation via uavBarrierBuffer() between scatter/resolve.
// Frame order: beginFrame (snap cascade origins, write RcInfo) -> scatter (accum) -> recordResolve
// (accum -> cells). Not thread-safe.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/voxi/NeuRaCLayout.hpp"

namespace aver::voxi {

class NeuRaC {
public:
    // Per-frame tunables (no Settings keys yet; compile-time constants in NeuRaCLayout.hpp).
    struct Params {
        u32 sampleCap      = 64;       // Samples per cell per frame (<= kMaxCap).
        f32 alphaMin       = 0.0625f;  // Floor on temporal blend (~16 frames to settle).
        u32 ageStepFrames  = 16;       // Frames between aging a cell without samples.
    };

    // What table 0's t22/u20/u21 must hold for this frame. info changes every frame; accum/cells
    // change only when generation does. generation bumps at buffer creation only.
    struct Bindings {
        rhi::BufferHandle info = 0, accum = 0, cells = 0;
        u32 infoStride  = neurac::kInfoStride;
        u32 infoCount   = 1;
        u32 accumInts   = neurac::kAccumInts;
        u32 cellCount   = neurac::kCells;
        u32 cellStride  = neurac::kCellStride;
        u32 generation  = 0;
    };

    NeuRaC() = default;
    ~NeuRaC();
    NeuRaC(const NeuRaC&)            = delete;
    NeuRaC& operator=(const NeuRaC&) = delete;

    // Creates buffers (~75 MB), compiles voxi_neurac_resolve.hlsl (CSRcResolve, SM 6.2),
    // and builds compute pipeline and binding sets. Returns false if unsuccessful; cheap to call
    // when already valid.
    bool create(rhi::IResourceFactory& res);

    // Idempotent. Caller must have rebound table 0's descriptors away from these buffers and
    // ensured GPU is done with them.
    void destroy();

    bool valid() const { return valid_; }

    // Once per frame, before any pass binds table 0. Snaps cascade origins to floor(cam / cellSize) - 32,
    // writes RcInfo to the next ring slot (rotated before write), and if clear pending, records clear
    // dispatch. camPosCm is eye in world centimetres.
    Bindings beginFrame(rhi::IRenderContext& ctx, const f32 camPosCm[3], u32 frameIndex, const Params& p);

    // After scatter dispatch and uavBarrierBuffer(accumBuffer()): binds resolve set and dispatches one
    // thread per cell, inside ScopedGpuStat "Voxi NeuRaC resolve". Does NOT issue cells barrier after.
    void recordResolve(rhi::IRenderContext& ctx);

    // Zero every cell and accumulator at next beginFrame.
    void requestReset() { clearPending_ = true; }

    rhi::BufferHandle accumBuffer() const { return accum_; }
    rhi::BufferHandle cellsBuffer() const { return cells_; }

private:
    static constexpr u32 kRing = neurac::kInfoRing;

    rhi::IResourceFactory* res_ = nullptr;
    bool valid_        = false;
    bool clearPending_ = false;

    rhi::BufferHandle accum_ = 0;
    rhi::BufferHandle cells_ = 0;
    // Per-frame RcInfo ring (Upload, GENERIC_READ) plus one clear-all buffer written once at create.
    // Clear cannot reuse a ring slot: writeBuffer is immediate while dispatch executes later.
    rhi::BufferHandle info_[kRing] = {};
    rhi::BufferHandle clearInfo_   = 0;

    rhi::PipelineHandle pipeline_ = 0;
    // One resolve set per ring slot (t0 = that slot's info), plus clear's set. Written once and never
    // rewritten after binding (Vulkan constraint).
    rhi::BindingSetHandle sets_[kRing] = {};
    rhi::BindingSetHandle clearSet_    = 0;

    u32 slot_       = kRing - 1;   // First beginFrame rotates to 0.
    u32 generation_ = 0;

    // One-shot warmup report: kStatsAtFrame frames after cache goes live, copy cells to readback
    // (top of frame, after previous resolve finished) and log per cascade after kStatsReadDelay frames.
    // Answers "is cache filling" (a failed lookup falls back and looks identical).
    static constexpr u32 kStatsAtFrame   = 150;
    static constexpr u32 kStatsReadDelay = 4;
    void logWarmupStats();
    rhi::BufferHandle   statsReadback_ = 0;
    neurac::RcInfo statsInfo_{};
    u32 framesLive_     = 0;
    u32 statsCopyFrame_ = 0;
    u32 statsState_     = 0;              // 0 not yet, 1 copy recorded, 2 reported
};

}  // namespace aver::voxi
