// RadianceCache -- the GPU half of the Voxi radiance cache, stage 1 (docs/rendering/RADIANCE_CACHE.md).
//
// WHAT THIS CLASS OWNS: the accumulator and cell buffers, a small ring of per-frame RcInfo upload
// buffers, and the compute pipeline that RESOLVES the accumulator into the cells once per frame. It does
// NOT own the scatter or the lookup: those are HLSL inside Voxi's own ray-driven compute passes
// (voxi_radiance_cache_io.hlsli, compiled only into the AVER_RADIANCE_CACHE twin pipelines), reading and
// writing the buffers through Voxi's binding table 0 (t22 info, u20 accumulator, u21 cells). VoxiRenderer
// therefore calls beginFrame() BEFORE any pass binds table 0, rebinds those three slots to what
// beginFrame() returns, and calls recordResolve() after the scatter dispatch has finished.
//
// BOTH BIG BUFFERS ARE UAV-ONLY. They are created in COMMON and touched only through UAV descriptors, so
// they never need a state transition, which matters because the staged lighting group is deliberately
// barrier-free (the same reason rdGiCandBuf_/rdVisBuf_ are UAV-only). The only synchronisation is
// uavBarrierBuffer(), which the caller issues between the scatter and the resolve and after the resolve.
//
// FRAME ORDER (reads in frame N see the cache resolved at the end of N-1):
//   beginFrame      snap cascade origins, write this frame's RcInfo, (rarely) clear everything
//   ... trace twin  scatters into the accumulator, reads the cells
//   recordResolve   accumulator -> cells, accumulator zeroed for the next frame
//
// NOT THREAD-SAFE, and every method that records takes the context the renderer is recording into.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/voxi/RadianceCacheLayout.hpp"

namespace aver::voxi {

class RadianceCache {
public:
    // Per-frame tunables the renderer passes down (no Settings keys yet: cascade geometry is a
    // compile-time constant in RadianceCacheLayout.hpp, and these three are not worth a manifest key
    // until someone measures a reason to move them).
    struct Params {
        u32 sampleCap      = 64;       // samples per cell per frame that may contribute (<= kMaxCap)
        f32 alphaMin       = 0.0625f;  // floor on the temporal blend: ~16 frames to settle a lighting change
        u32 ageStepFrames  = 16;       // a cell that gets no samples ages one step per this many frames
    };

    // What table 0's t22/u20/u21 must hold for this frame. `info` changes EVERY frame (it is the ring
    // slot beginFrame just wrote), so the renderer rebinds t22 each frame; accum and cells change only
    // when `generation` does.
    struct Bindings {
        rhi::BufferHandle info = 0, accum = 0, cells = 0;
        u32 infoStride  = radiancecache::kInfoStride;
        u32 infoCount   = 1;
        u32 accumInts   = radiancecache::kAccumInts;
        u32 cellCount   = radiancecache::kCells;
        u32 cellStride  = radiancecache::kCellStride;
        u32 generation  = 0;   // bumps when the accum/cells handles change (create), never otherwise
    };

    RadianceCache() = default;
    ~RadianceCache();
    RadianceCache(const RadianceCache&)            = delete;
    RadianceCache& operator=(const RadianceCache&) = delete;

    // Creates the buffers (~75 MB), compiles voxi_radiance_cache_resolve.hlsl (entry CSRcResolve, SM 6.2),
    // builds the compute pipeline and the resolve binding sets. False = logged and left off, nothing
    // half-built is kept. Cheap to call when already valid (returns true).
    bool create(rhi::IResourceFactory& res);

    // Idempotent. The CALLER has already rebound table 0's t22/u20/u21 away from these buffers (a bound
    // descriptor outlives its buffer and faults the GPU), and the GPU is done with them.
    void destroy();

    bool valid() const { return valid_; }

    // Once per frame, before any pass binds table 0 and outside the scene pass. Snaps each cascade's
    // origin to floor(cam / cellSize) - 32, writes this frame's RcInfo into the NEXT ring slot (rotated
    // before the write: writeBuffer is immediate and unsynchronised, so a slot an in-flight frame still
    // reads must never be rewritten), and, if a clear is pending (fresh buffers or requestReset()),
    // records the clear-all dispatch itself. `camPosCm` is the eye in world centimetres.
    Bindings beginFrame(rhi::IRenderContext& ctx, const f32 camPosCm[3], u32 frameIndex, const Params& p);

    // After the scatter dispatch AND a uavBarrierBuffer(accumBuffer()): binds the resolve set for this
    // frame's ring slot and dispatches one thread per cell, inside ScopedGpuStat "Voxi radiance cache
    // resolve". Does NOT issue the cells barrier after it; the caller does.
    void recordResolve(rhi::IRenderContext& ctx);

    // Zero every cell and the accumulator at the next beginFrame. (A teleport needs no reset: a window
    // that shifts by 64+ cells makes every tag mismatch and the cells read as empty.)
    void requestReset() { clearPending_ = true; }

    rhi::BufferHandle accumBuffer() const { return accum_; }
    rhi::BufferHandle cellsBuffer() const { return cells_; }

private:
    static constexpr u32 kRing = radiancecache::kInfoRing;

    rhi::IResourceFactory* res_ = nullptr;
    bool valid_        = false;
    bool clearPending_ = false;

    rhi::BufferHandle accum_ = 0;
    rhi::BufferHandle cells_ = 0;
    // The per-frame RcInfo ring (Upload, GENERIC_READ for life) plus ONE more buffer, written once at
    // create with flags = CLEAR_ALL. The clear cannot reuse a ring slot: writeBuffer is immediate while
    // the dispatch executes later, so flipping a ring slot's flag back after recording the clear would
    // make the GPU see the final contents, and leaving it set would make this frame's resolve wipe the
    // frame's own samples.
    rhi::BufferHandle info_[kRing] = {};
    rhi::BufferHandle clearInfo_   = 0;

    rhi::PipelineHandle pipeline_ = 0;
    // One resolve set per ring slot (t0 = that slot's info), plus the clear's. Written once at create and
    // never again, so no set is ever written after being bound (Vulkan's ringed sets forbid that).
    rhi::BindingSetHandle sets_[kRing] = {};
    rhi::BindingSetHandle clearSet_    = 0;

    u32 slot_       = kRing - 1;   // the first beginFrame rotates to 0
    u32 generation_ = 0;

    // ---- ONE-SHOT WARM-UP REPORT ----
    // kStatsAtFrame frames after the cache goes live, beginFrame copies the cells into a readback buffer
    // (at the top of the frame: the previous frame's resolve has finished writing them and the buffer has
    // decayed to COMMON, so the copy needs no barrier), and kStatsReadDelay frames later -- past the
    // frames-in-flight limit, so the copy has completed -- logs per cascade how many cells hold valid
    // samples, their mean n_eff/age/planarity and their mean ambient light. Said once per create; the
    // readback buffer (24 MiB) is freed straight after. It answers "is the cache actually filling", which
    // nothing else on screen can: a lookup that never finds a valid cell falls back to HalfResolution's
    // formula and looks exactly like it.
    static constexpr u32 kStatsAtFrame   = 150;
    static constexpr u32 kStatsReadDelay = 4;
    void logWarmupStats();
    rhi::BufferHandle   statsReadback_ = 0;
    radiancecache::RcInfo statsInfo_{};   // the RcInfo current when the copy was recorded
    u32 framesLive_     = 0;
    u32 statsCopyFrame_ = 0;
    u32 statsState_     = 0;              // 0 not yet, 1 copy recorded, 2 reported
};

}  // namespace aver::voxi
