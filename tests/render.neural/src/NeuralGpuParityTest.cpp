// NeuralGpuParityTest -- the GPU MLP kernels against MlpReference, their CPU twin, on a real device.
//
// Why it exists: NeuralMlpTest checks the maths on the CPU only; nothing ran the HLSL. Here the same
// weights and records go through Mlp (inference, one training step) and through MlpReference, and the
// two must agree. Skips (77) when the backend is absent or cannot run IDevice::runStandaloneCompute.
//
// Arguments: `vulkan` selects the Vulkan backend (default D3D12); `hw` uses the adapter instead of WARP
// (D3D12 only -- Vulkan has no WARP and always uses hardware).
//   a) inference outputs vs MlpReference::forward   rel 1e-5 / abs 1e-6
//   b) one recordTrain step: master weights (and EMA) vs MlpReference::trainBatch   rel 1e-4
//   c) the same inference run twice: bit-identical
#include "aver/core/Log.hpp"
#include "aver/render/neural/Mlp.hpp"
#include "aver/render/neural/MlpReference.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <span>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::render::neural;

namespace {

int g_failures = 0;

void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok  {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

// ctest's SKIP_RETURN_CODE (root CMakeLists.txt, beside AVER_CTEST_VULKAN_TARGETS).
constexpr int kSkip = 77;

constexpr u32 kIn = 8, kOut = 3, kCount = 64;

// Deterministic uniform [-1, 1) from a 32-bit LCG, so every machine feeds both sides the same data.
struct Lcg {
    u32 s = 12345u;
    f32 next() {
        s = s * 1664525u + 1013904223u;
        return static_cast<f32>(s >> 8) * (2.0f / 16777216.0f) - 1.0f;
    }
};

struct Compare {
    f32 maxAbs = 0.0f;
    f32 worstRatio = 0.0f;   // largest err / (abs + rel * |want|); <= 1 passes
    bool finite = true;
};

Compare compare(std::span<const f32> got, std::span<const f32> want, f32 rel, f32 abs) {
    Compare c;
    if (got.size() != want.size()) { c.finite = false; c.worstRatio = 1e30f; return c; }
    for (usize i = 0; i < got.size(); ++i) {
        if (!std::isfinite(got[i]) || !std::isfinite(want[i])) { c.finite = false; continue; }
        const f32 err = std::fabs(got[i] - want[i]);
        c.maxAbs = std::max(c.maxAbs, err);
        c.worstRatio = std::max(c.worstRatio, err / (abs + rel * std::fabs(want[i])));
    }
    return c;
}

bool expectClose(const std::string& what, std::span<const f32> got, std::span<const f32> want, f32 rel, f32 abs) {
    const Compare c = compare(got, want, rel, abs);
    const bool ok = c.finite && c.worstRatio <= 1.0f;
    char err[32];
    std::snprintf(err, sizeof err, "%.3g", static_cast<double>(c.maxAbs));
    check(ok, what + " (max abs error " + err + ")");
    return ok;
}

struct Buffers {
    rhi::IResourceFactory* res = nullptr;
    rhi::BufferHandle records = 0, targets = 0, outputs = 0, upRecords = 0, upTargets = 0, readback = 0;

    ~Buffers() {
        if (!res) return;
        res->waitIdle();
        for (rhi::BufferHandle b : {records, targets, outputs, upRecords, upTargets, readback})
            if (b) res->destroyBuffer(b);
    }
};

rhi::BufferHandle makeBuffer(rhi::IResourceFactory& res, u64 bytes, rhi::BufferKind kind, bool uav, const char* name) {
    rhi::BufferDesc bd{};
    bd.bytes = bytes;
    bd.kind = kind;
    bd.allowUnorderedAccess = uav;
    bd.debugName = name;
    return res.createBuffer(bd);
}

// Everything past device creation; false counts as a failure through g_failures.
void runParity(rhi::IDevice& dev) {
    using rhi::ResourceState;
    rhi::IResourceFactory& res = *dev.resources();

    MlpDesc desc{};
    desc.inputs = kIn;
    desc.outputs = kOut;
    desc.hiddenWidth = 16;
    desc.hiddenLayers = 2;
    desc.hidden = Activation::ReLU;
    desc.output = Activation::None;
    desc.bias = true;
    desc.seed = 7;
    const OptimiserDesc opt{};

    Mlp mlp;
    if (!mlp.create(dev, desc, opt)) {
        check(false, "Mlp::create (shaders compile, pipelines and buffers build)");
        return;
    }
    check(true, "Mlp::create (shaders compile, pipelines and buffers build)");
    check(kCount <= mlp.safeBatchLimit(), "the test batch fits the fixed-point accumulator");

    // Records and targets: a smooth function of the inputs, so training has a real gradient everywhere.
    std::vector<f32> records(static_cast<usize>(kCount) * kIn), targets(static_cast<usize>(kCount) * kOut);
    Lcg rng;
    for (f32& v : records) v = rng.next();
    for (u32 r = 0; r < kCount; ++r) {
        const f32* in = &records[static_cast<usize>(r) * kIn];
        targets[static_cast<usize>(r) * kOut + 0] = 0.5f * in[0] - 0.25f * in[1] + 0.1f;
        targets[static_cast<usize>(r) * kOut + 1] = in[2] * in[3] + 0.2f * in[4];
        targets[static_cast<usize>(r) * kOut + 2] = 0.3f - 0.4f * in[5] + 0.15f * in[6] * in[7];
    }

    const u64 recBytes = static_cast<u64>(records.size()) * sizeof(f32);
    const u64 outBytes = static_cast<u64>(targets.size()) * sizeof(f32);
    Buffers b;
    b.res = &res;
    b.records   = makeBuffer(res, recBytes, rhi::BufferKind::Default, false, "Parity records");
    b.targets   = makeBuffer(res, outBytes, rhi::BufferKind::Default, false, "Parity targets");
    b.outputs   = makeBuffer(res, outBytes, rhi::BufferKind::Default, true, "Parity outputs");
    b.upRecords = makeBuffer(res, recBytes, rhi::BufferKind::Upload, false, "Parity records upload");
    b.upTargets = makeBuffer(res, outBytes, rhi::BufferKind::Upload, false, "Parity targets upload");
    b.readback  = makeBuffer(res, outBytes, rhi::BufferKind::Readback, false, "Parity readback");
    if (!b.records || !b.targets || !b.outputs || !b.upRecords || !b.upTargets || !b.readback ||
        !res.writeBuffer(b.upRecords, records.data(), recBytes) ||
        !res.writeBuffer(b.upTargets, targets.data(), outBytes)) {
        check(false, "test buffers allocate and upload");
        return;
    }

    // ---- 0) copies alone: Upload -> Default -> Readback, so a compute failure is not mistaken for one --
    {
        const rhi::BufferHandle rb = makeBuffer(res, recBytes, rhi::BufferKind::Readback, false, "Parity copy readback");
        std::vector<f32> back(records.size(), 0.0f);
        const bool ran = rb && dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
            ctx.bufferBarrier(b.records, ResourceState::Common, ResourceState::CopyDest);
            ctx.copyBuffer(b.records, b.upRecords, recBytes);
            ctx.bufferBarrier(b.records, ResourceState::CopyDest, ResourceState::CopySource);
            ctx.copyBuffer(rb, b.records, recBytes);
            ctx.bufferBarrier(b.records, ResourceState::CopySource, ResourceState::Common);
        });
        check(ran && res.readBuffer(rb, back.data(), recBytes, 0) &&
                  std::memcmp(back.data(), records.data(), recBytes) == 0,
              "copy round trip (Upload -> Default -> Readback) returns the uploaded bytes");
        if (rb) { res.waitIdle(); res.destroyBuffer(rb); }
    }

    // The CPU twin starts from the exact weights the GPU network was given.
    const std::span<const f32> initialView = mlp.cpuWeights(false);
    const std::vector<f32> initial(initialView.begin(), initialView.end());
    MlpReference ref(desc, opt);
    check(ref.setWeights(initial), "MlpReference takes the network's initial weights");

    // One standalone submission: optionally the data upload, then inference, copied to the readback.
    const auto infer = [&](bool upload, std::vector<f32>& out) {
        bool recorded = false;
        const bool ran = dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
            if (upload) {
                ctx.bufferBarrier(b.records, ResourceState::Common, ResourceState::CopyDest);
                ctx.bufferBarrier(b.targets, ResourceState::Common, ResourceState::CopyDest);
                ctx.copyBuffer(b.records, b.upRecords, recBytes);
                ctx.copyBuffer(b.targets, b.upTargets, outBytes);
                ctx.bufferBarrier(b.records, ResourceState::CopyDest, ResourceState::Common);
                ctx.bufferBarrier(b.targets, ResourceState::CopyDest, ResourceState::Common);
            }
            recorded = mlp.recordInfer(ctx, b.records, b.outputs, CpuCount{kCount}, false);
            ctx.bufferBarrier(b.outputs, ResourceState::Common, ResourceState::CopySource);
            ctx.copyBuffer(b.readback, b.outputs, outBytes);
            ctx.bufferBarrier(b.outputs, ResourceState::CopySource, ResourceState::Common);
        });
        out.assign(targets.size(), 0.0f);
        return ran && recorded && res.readBuffer(b.readback, out.data(), outBytes, 0);
    };

    // ---- a) inference vs the CPU forward -------------------------------------------------------
    std::vector<f32> gpu1, gpu2;
    check(infer(true, gpu1), "GPU inference ran (standalone submission, readback)");

    std::vector<f32> cpu(targets.size());
    for (u32 r = 0; r < kCount; ++r)
        ref.forward(std::span<const f32>(&records[static_cast<usize>(r) * kIn], kIn),
                    std::span<f32>(&cpu[static_cast<usize>(r) * kOut], kOut), false);
    const bool nonTrivial = std::any_of(gpu1.begin(), gpu1.end(), [](f32 v) { return std::fabs(v) > 1e-4f; });
    check(nonTrivial, "GPU outputs are not all zero");
    expectClose("inference matches MlpReference::forward (rel 1e-5, abs 1e-6)", gpu1, cpu, 1e-5f, 1e-6f);

    // ---- c) the same dispatch again: bit-identical ---------------------------------------------
    check(infer(false, gpu2), "GPU inference ran a second time");
    check(gpu1.size() == gpu2.size() &&
              std::memcmp(gpu1.data(), gpu2.data(), gpu1.size() * sizeof(f32)) == 0,
          "two GPU inference runs are bit-identical");

    // ---- b) one training step vs trainBatch ----------------------------------------------------
    bool trained = false, readback = false;
    const bool ran = dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
        trained = mlp.recordTrain(ctx, b.records, b.targets, CpuCount{kCount});
        readback = mlp.recordReadback(ctx);
    });
    check(ran && trained && readback, "GPU training step and weight readback ran");
    check(mlp.collectWeights(), "collectWeights after the run");

    const f32 loss = ref.trainBatch(records, targets, kCount);
    check(std::isfinite(loss), "MlpReference::trainBatch ran");

    const std::span<const f32> gpuMaster = mlp.cpuWeights(false);
    const std::span<const f32> gpuEma = mlp.cpuWeights(true);
    f32 moved = 0.0f;
    for (usize i = 0; i < initial.size() && i < gpuMaster.size(); ++i)
        moved = std::max(moved, std::fabs(gpuMaster[i] - initial[i]));
    check(moved > 1e-5f, "the GPU step moved the weights");
    // abs 2e-5 on top of 1e-4 relative: Adam's first step is ~lr * sign(g), so a weight whose
    // gradient sits on a fixed-point rounding boundary can differ by up to lr in one direction only.
    expectClose("master weights match MlpReference::trainBatch (rel 1e-4)", gpuMaster, ref.weights(), 1e-4f, 2e-5f);
    expectClose("EMA weights match MlpReference::trainBatch (rel 1e-4)", gpuEma, ref.ema(), 1e-4f, 2e-5f);
}

}  // namespace

int main(int argc, char** argv) {
    AVER_INFO("=== NeuralGpuParityTest ===");

    bool wantVulkan = false, hardware = false, debug = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "vulkan") == 0) wantVulkan = true;
        if (std::strcmp(argv[i], "hw") == 0) hardware = true;
        if (std::strcmp(argv[i], "debug") == 0) debug = true;   // validation / debug layer
    }

    rhi::DeviceDesc desc;
    desc.useWarp = !hardware;
    desc.enableDebug = debug;
    desc.preferred[0] = wantVulkan ? rhi::Backend::Vulkan : rhi::Backend::D3D12;
    desc.preferred[1] = rhi::Backend::Null;
    desc.preferredCount = 2;   // no fallback to the other backend: an absent one must report SKIPPED
    rhi::IDevice* dev = rhi::createDevice(desc);
    if (!dev || dev->backend() == rhi::Backend::Null) {
        AVER_WARN("  SKIP  no {} device on this machine", wantVulkan ? "Vulkan" : "D3D12");
        if (dev) rhi::destroyDevice(dev);
        return kSkip;
    }
    if (!dev->resources()) {
        AVER_WARN("  SKIP  this device exposes no resource factory");
        rhi::destroyDevice(dev);
        return kSkip;
    }
    AVER_INFO("device: {} ({})", dev->adapterName(), rhi::backendName(dev->backend()));

    // Probe: an empty recording tells "backend cannot run standalone compute" from a real failure.
    if (!dev->runStandaloneCompute([](rhi::IRenderContext&) {})) {
        AVER_WARN("  SKIP  IDevice::runStandaloneCompute is unavailable on this backend");
        rhi::destroyDevice(dev);
        return kSkip;
    }

    runParity(*dev);
    rhi::destroyDevice(dev);

    if (g_failures != 0) {
        AVER_ERROR("=== NeuralGpuParityTest FAILED === ({} failure(s))", g_failures);
        return 1;
    }
    AVER_INFO("=== NeuralGpuParityTest PASSED ===");
    return 0;
}
