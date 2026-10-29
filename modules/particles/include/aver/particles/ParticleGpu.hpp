// GPU tier of the particle system: pooled per-instance buffers, simulation and sort kernels, stats readback. Generic RHI only.
// record() runs every instance phase by phase in prePass (one barrier per phase, not per instance); compute stays out of transparentPass because Vulkan forbids it inside dynamic rendering.
// Kernels use 32-bit atomics and groupshared only: one HLSL source for D3D12 and Vulkan.
#pragma once
#include "aver/particles/ParticleSortNetwork.hpp"
#include "aver/particles/ParticleSystem.hpp"
#include "aver/rhi/GodotEnvPrograms.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <array>
#include <string>
#include <vector>

namespace aver::particles {

// G6 SH3: the converted Godot particles shaders by name, fed by the host every time the framework's registry moves. An effect names one in
// ParticleEffect::godotShader. Process-wide, like the material graph registry; empty by default, and an empty registry changes nothing.
void setGodotParticlePrograms(std::vector<rhi::GodotEnvProgram> programs);
const rhi::GodotEnvProgram* findGodotParticleProgram(const std::string& name);
u64 godotParticleProgramRevision();
// The GPU tier needs this many float4s of state per slot for a Godot shader's emitter (see particles_sim.hlsl, PGD_VECS).
inline constexpr u32 kGodotParticleStateVecs = 5;

// A buffer and the state it was last left in; the RHI never transitions implicitly.
struct TrackedBuffer {
    rhi::BufferHandle h = 0;
    rhi::ResourceState st = rhi::ResourceState::Common;
};

// Per-instance ranges suballocated from pages, one set per capacity class (256 x 4^c particles, c = 0..6). Range offsets are multiples of 256 (Vulkan storage-buffer alignment).
class ParticleGpuPool {
public:
    static constexpr u32 kClassCount = 7;
    static constexpr u32 kCountersStride = 64;
    static constexpr u32 kArgsStride = 64;

    struct Page {
        bool live = false;
        u32 cls = 0, capacity = 0, perPage = 1, used = 0;
        TrackedBuffer particles, dead, alive, sortKeys, counters, args;
        rhi::BufferHandle readback[3] = {};
        std::vector<u8> taken;
        u64 bytes = 0;
        bool sortable = false;
    };
    struct Range {
        u32 page = ~0u, index = 0, capacity = 0;
        bool valid() const { return page != ~0u; }
    };

    static u32 classCapacity(u32 cls) { return 256u << (2u * cls); }
    // The smallest class holding `particles`, or -1 above the largest.
    static int classFor(u32 particles);

    void init(rhi::IResourceFactory* res) { res_ = res; }
    void shutdown();
    bool acquire(u32 particles, Range& out);
    void release(const Range& r);
    Page& page(u32 i) { return pages_[i]; }
    const Page& page(u32 i) const { return pages_[i]; }
    u32 pageCount() const { return static_cast<u32>(pages_.size()); }
    u64 bytes() const { return bytes_; }

private:
    bool makePage(Page& p, u32 cls);
    void destroyPage(Page& p);
    rhi::BufferHandle make(u64 bytes, rhi::BufferKind kind, bool uav, const char* name, u64& total);

    rhi::IResourceFactory* res_ = nullptr;
    std::vector<Page> pages_;
    u64 bytes_ = 0;
};

class ParticleGpu final : public IParticleGpuTier {
public:
    // Builds kernels and layouts. False (tier unusable) when the device lacks a resource factory, compute or indirect support, or a kernel fails to compile.
    bool init(rhi::IDevice& device);
    void shutdown();

    bool usable() const override { return ready_; }
    u32 acquire(const ParticleEffect& fx, u32 parent) override;
    void release(u32 handle) override;
    u32 aliveApprox(u32 handle) const override;
    u64 allocatedBytes() const override { return pool_.bytes() + dedicatedBytes_; }
    u32 capacityFor(const ParticleEffect& fx) const override;

    // Camera for sort and culling; viewProj is row-major.
    struct View {
        f32 viewProj[16] = {};
        f32 camPos[3] = {};
        bool valid = false;
    };

    // Records every visible job. Instances without a job keep their particles frozen and are not drawn. Returns the instances that ran.
    u32 record(rhi::IRenderContext& ctx, std::vector<GpuJob>& jobs, const View& view);

    // Runs before record() in prePass. Applies queued floating-origin shifts (CSShift, one dispatch per instance: particle slots, trail points, pending event records) and consumes `shifts`.
    void recordShifts(rhi::IRenderContext& ctx, std::vector<GpuShift>& shifts);

    // For mesh particles: the index count the draw arguments carry (default 6, the quad template).
    void setIndexCount(u32 handle, u32 indexCount);

    // What the draw needs for one instance, valid only when the instance ran this frame.
    struct DrawBuffers {
        rhi::BufferHandle particles = 0, order = 0, trail = 0, args = 0;
        u32 particleFirst = 0, particleCount = 0;
        u32 orderFirst = 0, orderCount = 0;
        u32 trailFirst = 0, trailCount = 0;
        u64 argsOffset = 0;
        u32 capacity = 0;
        u32 trailRing = 0;          // trail points per particle
        bool linked = false;
    };
    bool drawBuffers(u32 handle, DrawBuffers& out) const;

    // Bumped by each record(). Buffers are left in rest states, so the draw needs no barrier.
    u64 frame() const { return frame_; }

    // Counters block of a handle (kGpuCounterCount uints) from the readback three frames ago.
    bool counters(u32 handle, u32* out, u32 count) const;

private:
    struct Instance {
        bool live = false;
        ParticleGpuPool::Range range;
        u32 capacity = 0;
        u32 poolCapacity = 0;
        bool linked = false, trail = false, sorted = false, hasEvents = false;
        u32 gdKernels = 0;          // G6 SH3: 1-based index into gdKernels_ when the emitter carries a Godot particles shader (trailBuf then holds its state)
        u32 trailRing = 0;
        u32 indexCount = 6;
        u32 parent = 0;
        TrackedBuffer eventsBuf;
        TrackedBuffer trailBuf;
        rhi::BindingSetHandle set = 0;
        u32 readIndex = 0;          // which alive list is read this frame
        bool needsInit = true;
        u64 ranFrame = 0;
        u64 firstRan = 0;           // the first frame it ran (the stats readback lags three frames behind)
        u32 lastAliveWrite = 0;
    };
    struct Item {
        GpuJob* job;
        Instance* inst;
        u32 handle;
    };

    Instance* find(u32 handle);
    const Instance* find(u32 handle) const;
    void to(rhi::IRenderContext& ctx, TrackedBuffer& b, rhi::ResourceState target);
    void barrier(rhi::IRenderContext& ctx, TrackedBuffer& b);
    void recordRound(rhi::IRenderContext& ctx, std::vector<Item>& items, const View& view);
    void recordSort(rhi::IRenderContext& ctx, std::vector<Item>& items, const View& view);
    bool probeCopy(rhi::IRenderContext& ctx, u32 handle, rhi::PipelineHandle pipe, const GpuSimCB& cb, rhi::BufferHandle dst, u32 words);
    void refreshStats();
    bool buildKernels();
    rhi::PipelineHandle kernel(const char* source, const char* entry, rhi::PipelineLayout layout, rhi::ShaderStage = rhi::ShaderStage::Compute);

    rhi::IDevice* dev_ = nullptr;
    rhi::IResourceFactory* res_ = nullptr;
    bool ready_ = false;
    ParticleGpuPool pool_;
    u64 dedicatedBytes_ = 0;
    u64 frame_ = 0;

    TrackedBuffer dummy_;
    // G6 SH3: the spawn and update kernels compiled with one Godot particles shader (built lazily, by program name; rebuilt when the program text changes).
    struct GdKernels { std::string name; u64 revision = 0; rhi::PipelineHandle spawn = 0, update = 0; bool failed = false; };
    std::vector<GdKernels> gdKernels_;
    u32 gdKernelsFor(const std::string& name);                       // 1-based index, 0 = none / not buildable
    static bool usesTrailBuf(const Instance& in) { return in.trail || in.gdKernels != 0; }
    rhi::PipelineHandle csInit_ = 0, csBegin_ = 0, csSpawn_ = 0, csArgsUpdate_ = 0, csUpdate_ = 0, csFinish_ = 0, csProbe_ = 0, csRngProbe_ = 0;
    rhi::PipelineHandle csSortKeys_ = 0, csSortLocal_ = 0, csSortGlobal_ = 0, csSortLocalMerge_ = 0, csSortFinal_ = 0;
    rhi::PipelineHandle csShift_ = 0;   // optional: a stale deploy without CSShift only loses the origin shift, not the tier
    bool shiftWarned_ = false;

    std::vector<Instance> instances_;
    std::vector<u32> freeHandles_;
    // CPU copy of each page's counters block, refreshed from the readback ring once per frame.
    std::vector<std::vector<u32>> counterCache_;
    std::vector<u32> pageEpoch_;
    bool sortWarned_ = false;

public:
    enum class Dump { Particles, SurvivorList, Counters, DeadList, Events };
    u64 dumpBytes(u32 handle, Dump what) const;
    // Copies one of the instance's regions into `dst` (a Readback buffer of at least dumpBytes()), leaving every buffer in the state record() expects. Returns the bytes copied.
    u64 dump(rhi::IRenderContext& ctx, u32 handle, Dump what, rhi::BufferHandle dst);
    // Runs CSProbe with `cb` against the instance's binding set and copies the 64 uints it writes into `dst`.
    bool probe(rhi::IRenderContext& ctx, u32 handle, const GpuSimCB& cb, rhi::BufferHandle dst);
    // Runs CSRngProbe: gReq float4 i (i < 32) holds (seed bits, stream bits); the 32 hashes (top 24 bits) are copied into `dst`.
    bool probeRng(rhi::IRenderContext& ctx, u32 handle, const GpuSimCB& cb, rhi::BufferHandle dst);
};

} // namespace aver::particles
