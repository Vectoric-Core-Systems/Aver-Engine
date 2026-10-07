// PipelineBatchTest -- IPipelineBatch against a real D3D12 device (WARP, so no GPU is required): shader
// compilation and pipeline creation on worker threads, concurrent with the main thread's own creates.
//
// WHY A TEST. The asynchronous path's failure mode is not a wrong answer, it is a data race that corrupts a
// handle table or a root-signature cache once in a few hundred runs, and then a GPU hang far from the cause
// (docs/rendering/ASYNC_SHADERS.md). So this runs the shape that would expose it: many requests on the pool
// while the main thread keeps creating shaders and pipelines of its own, sharing one root-signature cache.
// Assertions:
//   - every request lands, with distinct factory handles, and resolve() is 0 before adopt();
//   - a shader that fails to compile fails only the pipelines that name it;
//   - progress is monotone and ends at its total; finished() never precedes the last request;
//   - the inline batch (synchronous mode) agrees with the asynchronous one;
//   - cancelling a batch in flight, and dropping it, neither crashes nor leaks into the factory.
// Written without running it (no engine runs in the authoring session): the first run is the test of the test.
#include "aver/core/Log.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <algorithm>
#include <chrono>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace aver;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

static constexpr int kSkip = 77;   // ctest's SKIP_RETURN_CODE; see ShaderIncludeResolveTest.cpp

// One tiny compute shader per variant: the define changes the text, so each is a distinct compile.
static const char* kSource =
    "RWStructuredBuffer<uint> o : register(u0);\n"
    "[numthreads(1, 1, 1)] void main() { o[0] = VARIANT; }\n";

static rhi::ComputePipelineDesc computeDesc(rhi::ShaderHandle cs, u32 uavs) {
    rhi::ComputePipelineDesc p{};
    p.cs = cs;
    p.layout.uavCount = uavs;
    return p;
}

// Records `n` variants; returns the local pipeline handles. `brokenAt` (or -1) gets a shader that cannot compile.
static std::vector<rhi::PipelineHandle> record(rhi::IPipelineBatch& b, int n, int brokenAt, u32 uavs) {
    std::vector<rhi::PipelineHandle> out;
    for (int i = 0; i < n; ++i) {
        const std::string defines = "VARIANT=" + std::to_string(i + 1);
        rhi::ShaderDesc sd{};
        sd.source = i == brokenAt ? "this is not hlsl" : kSource;
        sd.entry = "main";
        sd.stage = rhi::ShaderStage::Compute;
        sd.defines = defines.c_str();   // the batch copies it
        out.push_back(b.createComputePipeline(computeDesc(b.createShader(sd), uavs)));
    }
    return out;
}

int main() {
    AVER_INFO("=== PipelineBatchTest ===");

    rhi::DeviceDesc desc;
    desc.useWarp = true;
    desc.preferred[0] = rhi::Backend::D3D12;
    desc.preferred[1] = rhi::Backend::Null;
    desc.preferredCount = 2;
    rhi::IDevice* dev = rhi::createDevice(desc);
    if (!dev || dev->backend() == rhi::Backend::Null) {
        AVER_WARN("  SKIP  no D3D12 device on this machine (not even WARP)");
        if (dev) rhi::destroyDevice(dev);
        return kSkip;
    }
    rhi::IResourceFactory* res = dev->resources();
    if (!res) {
        AVER_WARN("  SKIP  this device exposes no resource factory");
        rhi::destroyDevice(dev);
        return kSkip;
    }

    // ---- 1. an asynchronous batch, with the main thread creating its own meanwhile ----------------------
    {
        std::unique_ptr<rhi::IPipelineBatch> batch = rhi::createPipelineBatch(*res, true);
        const int kN = 32;
        const std::vector<rhi::PipelineHandle> local = record(*batch, kN, /*brokenAt=*/7, /*uavs=*/1);
        check(batch->resolve(local[0]) == 0, "resolve() is 0 before the batch ran");
        batch->start();

        // The main thread's own creates share the root-signature cache with the workers (1 and 2 UAVs).
        std::vector<rhi::PipelineHandle> mine;
        for (int i = 0; i < 12; ++i) {
            const std::string defines = "VARIANT=" + std::to_string(1000 + i);
            rhi::ShaderDesc sd{};
            sd.source = kSource;
            sd.entry = "main";
            sd.stage = rhi::ShaderStage::Compute;
            sd.defines = defines.c_str();
            const rhi::ShaderHandle cs = res->createShader(sd);
            const rhi::PipelineHandle p = cs ? res->createComputePipeline(computeDesc(cs, 1 + (i & 1))) : 0;
            if (cs) res->destroyShader(cs);
            mine.push_back(p);
        }
        check(std::all_of(mine.begin(), mine.end(), [](rhi::PipelineHandle p) { return p != 0; }),
              "the main thread's own creates succeed while the batch runs");

        u32 lastDone = 0;
        bool monotone = true;
        const auto t0 = std::chrono::steady_clock::now();
        while (!batch->finished() && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(120)) {
            u32 done = 0, total = 0;
            batch->progress(done, total);
            monotone = monotone && done >= lastDone && done <= total;
            lastDone = done;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        check(batch->finished(), "the batch finishes");
        check(monotone, "progress never goes backwards or past its total");
        u32 done = 0, total = 0;
        batch->progress(done, total);
        check(total == 2u * kN && done == total, "progress ends at total (shaders + pipelines)");

        batch->adopt();
        std::set<rhi::PipelineHandle> distinct;
        int built = 0;
        for (int i = 0; i < kN; ++i) {
            const rhi::PipelineHandle h = batch->resolve(local[i]);
            if (i == 7) check(h == 0, "the pipeline whose shader would not compile resolves to 0");
            else if (h) { ++built; distinct.insert(h); }
        }
        check(built == kN - 1, "every other pipeline built");
        check(distinct.size() == static_cast<usize>(built), "factory handles are distinct");
        for (rhi::PipelineHandle p : mine) distinct.insert(p);
        check(distinct.size() == static_cast<usize>(built) + mine.size(), "and distinct from the main thread's");
        for (rhi::PipelineHandle h : distinct) res->destroyPipeline(h);
    }

    // ---- 2. the inline batch (synchronous mode) agrees --------------------------------------------------
    {
        std::unique_ptr<rhi::IPipelineBatch> batch = rhi::createPipelineBatch(*res, false);
        const std::vector<rhi::PipelineHandle> local = record(*batch, 6, /*brokenAt=*/2, 1);
        batch->start();
        check(batch->finished(), "an inline batch is finished when start() returns");
        batch->adopt();
        int built = 0;
        for (usize i = 0; i < local.size(); ++i) {
            const rhi::PipelineHandle h = batch->resolve(local[i]);
            if (i == 2) check(h == 0, "inline: the broken shader's pipeline resolves to 0");
            else if (h) { ++built; res->destroyPipeline(h); }
        }
        check(built == 5, "inline: every other pipeline built");
    }

    // ---- 3. cancelling in flight, and dropping, are safe; two batches at once ---------------------------
    {
        std::unique_ptr<rhi::IPipelineBatch> a = rhi::createPipelineBatch(*res, true);
        std::unique_ptr<rhi::IPipelineBatch> b = rhi::createPipelineBatch(*res, true);
        record(*a, 24, -1, 1);
        const std::vector<rhi::PipelineHandle> lb = record(*b, 24, -1, 2);
        a->start();
        b->start();
        a->cancel();
        a.reset();   // dropped with tasks in flight: they hold the shared state, never the batch
        b->waitFinished();
        b->adopt();
        int built = 0;
        for (rhi::PipelineHandle l : lb)
            if (const rhi::PipelineHandle h = b->resolve(l)) { ++built; res->destroyPipeline(h); }
        check(built == 24, "a second batch is unaffected by the first being cancelled and dropped");
    }

    // ---- 4. a batch dropped without ever starting, and one with nothing in it ---------------------------
    {
        std::unique_ptr<rhi::IPipelineBatch> unstarted = rhi::createPipelineBatch(*res, true);
        record(*unstarted, 4, -1, 1);
        unstarted.reset();
        std::unique_ptr<rhi::IPipelineBatch> empty = rhi::createPipelineBatch(*res, true);
        empty->start();
        empty->waitFinished();
        check(empty->finished(), "an empty batch finishes at once");
    }

    rhi::destroyDevice(dev);
    if (g_failures) {
        AVER_ERROR("=== PipelineBatchTest FAILED === ({} failure(s))", g_failures);
        return 1;
    }
    AVER_INFO("=== PipelineBatchTest passed ===");
    return 0;
}
