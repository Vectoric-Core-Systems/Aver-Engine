// ConcurrentPipelineTest -- shaders and compute pipelines created from several threads at once (WARP).
//
// Pipeline builds run on a worker while the render thread keeps using the factory (async shader compilation), so
// IResourceFactory::threadSafePipelineCreation() promises that createShader / createComputePipeline / destroyShader
// may race. This drives four threads through DXC (SM 6.2, so every compile goes through the per-thread DXC
// instances) and the factory's tables at once, and checks every handle is non-zero and distinct. A race in the
// handle tables or a shared DXC instance shows here as a duplicate handle, a failed create, or a crash.
//
// SKIPPED where a prerequisite is absent (no D3D12, no DXC, a factory that is not thread-safe).
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace aver;

int main() {
    AVER_INFO("=== ConcurrentPipelineTest ===");
    rhi::DeviceDesc desc;
    desc.useWarp = true;
    desc.preferred[0] = rhi::Backend::D3D12;
    desc.preferred[1] = rhi::Backend::Null;
    desc.preferredCount = 2;
    rhi::IDevice* dev = rhi::createDevice(desc);
    if (!dev || dev->backend() == rhi::Backend::Null) {
        AVER_WARN("  SKIP  no D3D12 device (not even WARP)");
        if (dev) rhi::destroyDevice(dev);
        return 0;
    }
    rhi::IResourceFactory* res = dev->resources();
    const rhi::DeviceCaps caps = dev->caps();
    if (!res || !res->threadSafePipelineCreation() || !caps.dxcAvailable || caps.shaderModel < 62) {
        AVER_WARN("  SKIP  needs a thread-safe factory and DXC with SM 6.2");
        rhi::destroyDevice(dev);
        return 0;
    }

    constexpr int kThreads = 4, kEach = 12;
    std::vector<rhi::PipelineHandle> pipes(kThreads * kEach, 0);
    std::vector<rhi::ShaderHandle> shaders(kThreads * kEach, 0);
    std::atomic<int> failures{0};
    const char* src =
        "RWStructuredBuffer<uint> gOut : register(u0);\n"
        "[numthreads(8, 1, 1)] void CSMain(uint3 t : SV_DispatchThreadID) { gOut[t.x] = t.x * VALUE + 1u; }\n";
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kEach; ++i) {
                const int slot = t * kEach + i;
                const std::string defines = "VALUE=" + std::to_string(slot + 3) + "u";   // a distinct compile each
                rhi::ShaderDesc sd{};
                sd.source = src;
                sd.entry = "CSMain";
                sd.stage = rhi::ShaderStage::Compute;
                sd.minShaderModel = 62;
                sd.defines = defines.c_str();
                const rhi::ShaderHandle cs = res->createShader(sd);
                if (!cs) { ++failures; continue; }
                rhi::ComputePipelineDesc pd{};
                pd.cs = cs;
                pd.layout.uavCount = 1;
                const rhi::PipelineHandle p = res->createComputePipeline(pd);
                if (!p) ++failures;
                shaders[slot] = cs;
                pipes[slot] = p;
                if (i % 3 == 0) { res->destroyShader(cs); shaders[slot] = 0; }   // destroy races creates too
            }
        });
    }
    for (std::thread& th : threads) th.join();

    int bad = failures.load();
    std::vector<rhi::PipelineHandle> sorted = pipes;
    std::sort(sorted.begin(), sorted.end());
    const bool zero = !sorted.empty() && sorted.front() == 0;
    const bool dup = std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end();
    if (zero) { ++bad; AVER_ERROR("  FAIL  a pipeline handle is 0 (a create failed)"); }
    if (dup)  { ++bad; AVER_ERROR("  FAIL  two creates returned the same pipeline handle"); }
    if (!zero && !dup) AVER_INFO("  ok    {} pipelines from {} threads, all distinct", pipes.size(), kThreads);

    for (const rhi::PipelineHandle p : pipes) if (p) res->destroyPipeline(p);
    for (const rhi::ShaderHandle s : shaders) if (s) res->destroyShader(s);
    rhi::destroyDevice(dev);
    if (bad) { AVER_ERROR("=== ConcurrentPipelineTest FAILED ({}) ===", bad); return 1; }
    AVER_INFO("=== ConcurrentPipelineTest PASSED ===");
    return 0;
}
