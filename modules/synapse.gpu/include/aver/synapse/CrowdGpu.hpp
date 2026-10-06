#pragma once
// The GPU crowd backend: the same ORCA solve as the CPU one, one thread per agent, for crowds in
// the thousands. A separate module so Aver.Synapse stays free of the RHI.
//
// HOW IT RUNS. The CPU packs the agents and a uniform grid (CrowdGrid) into upload buffers, the
// compute shader writes one avoided velocity per agent, and the CPU reads them back about three
// frames later. So the answer is a few frames old: agents react to where their neighbours WERE.
// The sim's contact pass still runs every step on the CPU, so agents never overlap; only the
// planning lags. This is what the backend trades for scale (see docs/AI_CROWDS_HEARING_COVER.md).
//
// It is an IRenderFeature because the RHI only hands out a command context during a frame.
// Register it with IDevice::addRenderFeature after init(). Until init() succeeds, available() is
// false and CrowdSim keeps using its CPU solver.
//
// init() compiles HLSL at runtime, so a green C++ build proves nothing about the shader.
#include "aver/rhi/RHI.hpp"
#include "aver/synapse/Crowd.hpp"

#include <vector>

namespace aver::synapse {

// One agent as the shader reads it. 48 bytes; mirrored by GpuAgent in synapse_crowd.hlsl.
struct GpuAgentPacked {
    f32 pos[2];
    f32 vel[2];
    f32 pref[2];        // the biased preferred velocity (sidestep already applied)
    f32 radius;
    f32 maxSpeed;
    f32 priority;
    f32 stuckSec;
    u32 flags;
    u32 id;
};
static_assert(sizeof(GpuAgentPacked) == 48, "GpuAgent in synapse_crowd.hlsl mirrors this byte for byte");

// The shader's neighbour/wall array bounds. Settings above these are clamped.
inline constexpr u32 kGpuMaxNeighbors = 16;
inline constexpr u32 kGpuMaxWalls = 8;

class GpuCrowdBackend final : public ICrowdBackend, public rhi::IRenderFeature {
public:
    ~GpuCrowdBackend() override;

    // Both base interfaces want a name; one override serves both.
    const char* name() const override { return "gpu-orca"; }

    // False leaves the object inert (no device, or the shader did not compile).
    bool init(rhi::IDevice& dev);
    void shutdown();

    bool available() const override { return pipeline_ != 0; }
    bool solve(const CrowdSolveInput& in, CrowdSolveOutput& out) override;
    void prePass(rhi::IRenderContext& ctx) override;

    // Re-upload the wall mask on the next job (after a rebake into the same buffer).
    void invalidateWalls() { navStamp_ = nullptr; }

    // Frames from submit to answer, for tuning and the debug overlay. 0 until one has completed.
    u32 lastLatencyFrames() const { return lastLatency_; }
    u32 completedJobs() const { return completed_; }

private:
    enum class State { Idle, Dispatch, Copy, Done };
    // The RHI's readBuffer synchronises nothing and the device is double buffered; three frames
    // covers it with one to spare (render.pcg's VolumeBuilder uses the same figure).
    static constexpr u32 kReadbackFrames = 3;

    rhi::IResourceFactory* res_ = nullptr;
    rhi::ShaderHandle cs_ = 0;
    rhi::PipelineHandle pipeline_ = 0;
    rhi::BindingSetHandle set_ = 0;
    rhi::BufferHandle agentsBuf_ = 0, cellStartBuf_ = 0, itemsBuf_ = 0, wallsBuf_ = 0;
    rhi::BufferHandle outBuf_ = 0, readbackBuf_ = 0;
    rhi::ResourceState outState_ = rhi::ResourceState::Common;
    u32 agentCap_ = 0;
    u32 wallCap_ = 0;

    State state_ = State::Idle;
    u32 waited_ = 0;
    u32 lastLatency_ = 0;
    u32 completed_ = 0;

    // The job in flight.
    u32 jobCount_ = 0;
    std::vector<u32> jobIds_;
    std::vector<V2> result_;
    std::vector<f32> readback_;
    // Root constants for the dispatch (CrowdCB in synapse_crowd.hlsl), captured at submit.
    static constexpr u32 kCbDwords = 20;
    u32 cb_[kCbDwords] = {};

    const fmt::OcNavData* navStamp_ = nullptr;
    u64 navCells_ = 0;
    u32 hasWalls_ = 0;

    CrowdGrid grid_;
    std::vector<GpuAgentPacked> packed_;
    std::vector<u32> blocked_;

    bool ensureCapacity(u32 agents, u64 walls);
    void destroyBuffers();
};

} // namespace aver::synapse
