#pragma once
// RhiComputeHost -- the function table Aver.Physics' hair simulation runs its compute work through, implemented over the RHI.
// docs/plans/wave11/SOFTBODY_HAIR.md 2.10, docs/HAIR.md.
// Records Jolt's hair compute commands (AverPhysComputeHostV1) and replays them into the frame's command stream. Callbacks touch
// CPU memory only, under one mutex; GPU objects are made, written and destroyed on the render thread.
// aver_phys_hair_flush seals ops as a batch; replay() in prePass runs sealed batches in order. Past kHairHostMaxPendingBatches the
// oldest is dropped (counted), which only happens when nothing is rendering.
// Buffers are made lazily and filled through a pooled upload copy at first use, so binding sets are never rewritten. map(write) on
// UPLOAD or CONSTANT is copy-on-write. endFrame() returns every buffer to Common. map(read) on READBACK returns null, so
// aver_phys_hair_read_positions is refused here.
#include "aver/core/Types.hpp"
#include "aver/physics/physics_hair_abi.h"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <memory>
#include <vector>

namespace aver::render {

inline constexpr u32 kHairHostMaxPendingBatches = 3;
inline constexpr u32 kHairHostFramesInFlight = 3;     // matches kSoftBodyFramesInFlight
inline constexpr u32 kHairHostMaxBindings = 16;       // per kernel (the widest has 13)
inline constexpr u64 kHairHostPoolBufferBytes = 1u << 20;   // at least this big: destroyed after use, not pooled

struct RhiComputeHostStats {
    u32 pendingBatches = 0;
    u32 droppedBatches = 0;        // since init
    u32 dispatchesLastReplay = 0;
    u32 uploadsLastReplay = 0;
    u32 hostBuffers = 0;
    u32 gpuBuffers = 0;
    u32 bindingSets = 0;
    u32 pipelines = 0;
    u64 gpuBytes = 0;
};

class RhiComputeHost {
public:
    RhiComputeHost();
    ~RhiComputeHost();
    RhiComputeHost(const RhiComputeHost&) = delete;
    RhiComputeHost& operator=(const RhiComputeHost&) = delete;

    // False (inert, nothing created) when the device has no resource factory or compute shaders.
    bool init(rhi::IDevice& dev);
    // Call after aver_phys_hair_set_compute_host(nullptr) or aver_phys_shutdown, never while hair instances remain.
    // Host handles still held by the physics layer become dead: their callbacks return 0 or do nothing.
    void shutdown();
    bool ready() const { return ready_; }

    // Valid until shutdown(). `user` points at the host's private state.
    const AverPhysComputeHostV1& table() const { return table_; }

    // Once per frame, before replay(): retires dead resources, ages staging buffers, purges binding sets naming a released buffer.
    void beginFrame();
    // Pipelines compile on first use; a kernel that fails is skipped with one log line and its hair stays frozen.
    void replay(rhi::IRenderContext& ctx);
    // Call from overlayPass, after the scene pass has read the buffers.
    void endFrame(rhi::IRenderContext& ctx);

    // Use ensureState() to change the tracked state.
    rhi::BufferHandle gpuBuffer(AverPhysHostHandle h);
    // Only meaningful between replay() and endFrame().
    void ensureState(rhi::IRenderContext& ctx, AverPhysHostHandle h, rhi::ResourceState state);
    // A render positions buffer is garbage until a replayed dispatch has written it.
    bool written(AverPhysHostHandle h) const;
    u64 bufferCount(AverPhysHostHandle h) const;
    u32 bufferStride(AverPhysHostHandle h) const;

    // Tests: records a copy into a new Readback buffer (state restored) and returns it. Read it after the submission has
    // finished (IResourceFactory::readBuffer) and destroy it. 0 on failure.
    rhi::BufferHandle recordReadback(rhi::IRenderContext& ctx, AverPhysHostHandle h, u64* outBytes = nullptr);

    RhiComputeHostStats stats() const;

    // Defined in RhiComputeHost.cpp; public so the table's C callbacks can name it.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    AverPhysComputeHostV1 table_{};
    bool ready_ = false;
};

} // namespace aver::render
