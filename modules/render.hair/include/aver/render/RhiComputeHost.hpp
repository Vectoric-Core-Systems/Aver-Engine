#pragma once
// RhiComputeHost -- the function table Aver.Physics' hair simulation runs its compute work through, implemented over Aver's RHI.
// docs/plans/wave11/SOFTBODY_HAIR.md 2.10, docs/HAIR.md.
//
// WHAT IT IS. Jolt 5.6's Hair records its whole simulation step as compute commands (SetShader, SetBuffer, Dispatch ...) into a ComputeQueue it is
// handed. Aver.Physics wraps that queue in an adapter (HairComputeHost.cpp) that forwards every call, one to one, to a plain-C table
// (AverPhysComputeHostV1, physics_hair_abi.h). This class is the other end of the table: it RECORDS those calls, and replays them into the
// frame's command stream with the RHI. Jolt's own DX12 and Vulkan compute backends stay off (they want a private queue and a private resource
// state tracker; see the design for why that cannot share a device with this renderer), so D3D12 and Vulkan parity is one code path.
//
// THREADS. The table is called from whatever thread runs aver_phys_hair_settings_finish (possibly a worker) and aver_phys_hair_update (the main
// thread). The callbacks therefore touch CPU memory only, under one mutex: handles are numbers, buffers are CPU descriptors, a dispatch is a
// record. Every GPU object is made, written and destroyed on the RENDER thread, by replay(), beginFrame() and the accessors below.
//
// HOW A STEP REACHES THE GPU. aver_phys_hair_update records ops; aver_phys_hair_flush calls execute(), which SEALS the open ops as a batch; the
// render feature calls replay(ctx) from prePass, which runs every sealed batch in order and clears them. At most kHairHostMaxPendingBatches wait;
// beyond that the oldest is dropped (counted), which only happens when nothing is rendering.
//
//   * Buffers are made lazily at first use. read-only buffers (BUFFER, UPLOAD) and read-write ones are Default-heap RHI buffers; the bytes of an
//     UPLOAD or BUFFER (and the initial data of an RW one) reach them by a copy from a pooled Upload staging buffer at the point of first use, so
//     the binding sets that name them are STABLE and never rewritten (the rule SkinnedMeshGpu's ring comment exists to enforce).
//   * map(write) on an UPLOAD or CONSTANT buffer is copy-on-write: an op that bound the earlier contents keeps them, so Jolt may rewrite its
//     materials and constants every update while an older step is still waiting to be replayed.
//   * A CONSTANT buffer is copied into the recorded op by value and handed to setConstantBuffer at replay (b3: gContext or gWind, b4: gIterationContext).
//   * Barriers: every buffer's state is tracked; a dispatch that reads moves it to NonPixelShaderResource, one that writes to UnorderedAccess, and a
//     read-write bind with Jolt's EBarrier::Yes adds a UAV barrier when the buffer was already written in this frame. endFrame() returns every
//     buffer to Common, which is what the RHI requires at the end of a frame (SkinnedMeshGpu's rule).
//   * map(read) on a READBACK buffer returns null: a GPU host cannot hand the CPU data the step has not computed yet, so aver_phys_hair_read_positions
//     is refused with this host. Tests use recordReadback() and a standalone submission instead.
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
inline constexpr u64 kHairHostPoolBufferBytes = 1u << 20;   // a staging buffer at least this big is destroyed after use, not pooled

struct RhiComputeHostStats {
    u32 pendingBatches = 0;
    u32 droppedBatches = 0;        // since init
    u32 dispatchesLastReplay = 0;
    u32 uploadsLastReplay = 0;
    u32 hostBuffers = 0;           // live host buffers
    u32 gpuBuffers = 0;            // of which have an RHI buffer
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

    // Binds the host to a device. False (inert, nothing created) when the device has no resource factory or no compute shaders.
    bool init(rhi::IDevice& dev);
    // Destroys every RHI object. Host handles the physics layer still holds become dead (their callbacks return 0 / do nothing). Call after
    // aver_phys_hair_set_compute_host(nullptr) or aver_phys_shutdown, never while the physics layer still has hair instances.
    void shutdown();
    bool ready() const { return ready_; }

    // The table to give aver_phys_hair_set_compute_host. `user` points at the host's private state; the table is valid until shutdown().
    const AverPhysComputeHostV1& table() const { return table_; }

    // ---- render thread ----
    // Once per frame, before replay(): retires dead resources, ages staging buffers, purges binding sets that name a released buffer.
    void beginFrame();
    // Runs every sealed batch in order. Pipelines compile on first use (a one-off hitch); a kernel that fails to compile is skipped with one log
    // line and the hair that needs it stays frozen.
    void replay(rhi::IRenderContext& ctx);
    // Returns every buffer this host made to Common (call from the feature's overlayPass, after the scene pass has read them).
    void endFrame(rhi::IRenderContext& ctx);

    // The RHI buffer behind a host handle, made on demand; 0 for an unknown handle. Its state is tracked by this host: change it only through
    // ensureState().
    rhi::BufferHandle gpuBuffer(AverPhysHostHandle h);
    // Records the barrier that moves a buffer to `state` (no-op when it is there). Only meaningful between replay() and endFrame().
    void ensureState(rhi::IRenderContext& ctx, AverPhysHostHandle h, rhi::ResourceState state);
    // True once a replayed dispatch has written the buffer (a render positions buffer is garbage before that).
    bool written(AverPhysHostHandle h) const;
    // Element count and stride the physics layer gave the buffer, 0 for an unknown handle.
    u64 bufferCount(AverPhysHostHandle h) const;
    u32 bufferStride(AverPhysHostHandle h) const;

    // Tests: records "copy this buffer into a new Readback buffer" (state is restored) and returns the Readback buffer, which the caller reads
    // after the submission has finished (IResourceFactory::readBuffer) and destroys. 0 on failure.
    rhi::BufferHandle recordReadback(rhi::IRenderContext& ctx, AverPhysHostHandle h, u64* outBytes = nullptr);

    RhiComputeHostStats stats() const;

    // Defined in RhiComputeHost.cpp; public only so the C callbacks of the table (free functions there) can name it. `user` in the table points at it.
    struct Impl;

private:
    std::unique_ptr<Impl> impl_;
    AverPhysComputeHostV1 table_{};
    bool ready_ = false;
};

} // namespace aver::render
