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

#include <algorithm>

namespace aver::voxi {

class NeuRaC {
public:
    // Per-frame tunables (no Settings keys yet; compile-time constants in NeuRaCLayout.hpp).
    struct Params {
        u32 sampleCap      = 64;       // Samples per cell per frame, finest cascade (<= kMaxCap).
        f32 alphaMin       = 0.0625f;  // Floor on temporal blend (~16 frames to settle).
        u32 ageStepFrames  = 16;       // Frames between aging a cell without samples.
        u32 coarseCap1     = neurac::kDefaultCoarseCap1;   // Per-cell cap, cascade 1.
        u32 coarseCap2     = neurac::kDefaultCoarseCap2;   // Per-cell cap, cascade 2.
        f32 netStrength    = 0.0f;     // Stage-2 trust strength; 0 = stage-1 lookup weights exactly.
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

    // Creates buffers (~79 MB), compiles voxi_neurac_resolve.hlsl (CSRcResolve, SM 6.2),
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

    // The floating origin moved by (dxSteps, dySteps) region steps of 1,638,400 cm (LARGE_WORLD 2.2.6). Every cell's world
    // coordinate moved by a whole number of 64-cell tori, so the toroidal texels still hold the same light; only cascade 2's
    // tag byte moved (NeuRaCLayout.hpp). The next beginFrame carries kFlagRetagCoarse and the resolve re-labels those cells,
    // so the cache survives instead of relearning (the fallback was requestReset). Steps accumulate until a frame consumes them.
    void noteOriginShift(i64 dxSteps, i64 dySteps) { retagStepsX_ += dxSteps; retagStepsY_ += dySteps; }

    // Raise the resolve's blend floor to alphaFloor for the next `frames` frames: old light decays
    // instead of clearing. Overlapping requests keep the longer span and the higher floor.
    void requestRelax(u32 frames, f32 alphaFloor) {
        if (frames == 0) return;
        if (relaxFrames_ == 0) relaxAlpha_ = 0.0f;
        relaxFrames_ = std::max(relaxFrames_, frames);
        relaxAlpha_  = std::max(relaxAlpha_, std::min(std::max(alphaFloor, 0.0f), 1.0f));
    }

    rhi::BufferHandle accumBuffer() const { return accum_; }
    rhi::BufferHandle cellsBuffer() const { return cells_; }
    // This frame's RcInfo (valid after beginFrame; the ring slot it wrote).
    rhi::BufferHandle infoBuffer() const { return valid_ ? info_[slot_] : rhi::BufferHandle{0}; }
    // The eye (world cm) beginFrame was given.
    const f32* eyeCm() const { return eyeCm_; }

private:
    static constexpr u32 kRing = neurac::kInfoRing;

    rhi::IResourceFactory* res_ = nullptr;
    bool valid_        = false;
    bool clearPending_ = false;
    u32  relaxFrames_  = 0;       // frames left at relaxAlpha_
    f32  relaxAlpha_   = 0.0f;
    f32  eyeCm_[3]     = {};
    i64  retagStepsX_  = 0;       // origin steps since the last frame wrote RcInfo; see noteOriginShift
    i64  retagStepsY_  = 0;

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
