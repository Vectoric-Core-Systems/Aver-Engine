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
// Then ConvNet against ConvNetReference on the NRD2-shaped net (12 -> 16 -> 16 -> 32 -> 32 -> 12, odd sizes):
//   a) forward per layer and end to end   rel 1e-5 / abs 1e-6   (+ evaluate vs ConvNetReference::evaluate)
//   b) 1 and 10 recordTrain steps vs trainBatch: master and EMA weights rel 1e-4
//   c) forward twice and a 10-step training rerun: bit-identical
//   d) a toy task (1/4-resolution mean) learned on the GPU: held-out loss falls
#include "aver/core/Log.hpp"
#include "aver/render/neural/ConvNet.hpp"
#include "aver/render/neural/ConvNetReference.hpp"
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

// ================================================================ ConvNet

// Device buffers the conv section owns: Default + Upload + Readback per tensor, freed after a waitIdle.
struct GpuTensor {
    rhi::BufferHandle buf = 0, up = 0, rb = 0;
    u64 bytes = 0;
};

struct TensorPool {
    rhi::IResourceFactory* res = nullptr;
    std::vector<GpuTensor> all;
    ~TensorPool() {
        if (!res) return;
        res->waitIdle();
        for (const GpuTensor& t : all)
            for (rhi::BufferHandle b : {t.buf, t.up, t.rb}) if (b) res->destroyBuffer(b);
    }
    // A tensor of `floats`; with data, the Upload copy is written now (copied up by uploadTensor).
    GpuTensor make(usize floats, bool uav, const std::vector<f32>* data = nullptr) {
        GpuTensor t;
        t.bytes = static_cast<u64>(floats) * sizeof(f32);
        t.buf = makeBuffer(*res, t.bytes, rhi::BufferKind::Default, uav, "ConvParity tensor");
        t.up = makeBuffer(*res, t.bytes, rhi::BufferKind::Upload, false, "ConvParity upload");
        t.rb = makeBuffer(*res, t.bytes, rhi::BufferKind::Readback, false, "ConvParity readback");
        if (data && t.up) res->writeBuffer(t.up, data->data(), t.bytes);
        all.push_back(t);
        return t;
    }
};

void uploadTensor(rhi::IRenderContext& ctx, const GpuTensor& t) {
    using rhi::ResourceState;
    ctx.bufferBarrier(t.buf, ResourceState::Common, ResourceState::CopyDest);
    ctx.copyBuffer(t.buf, t.up, t.bytes);
    ctx.bufferBarrier(t.buf, ResourceState::CopyDest, ResourceState::Common);
}

void copyToReadback(rhi::IRenderContext& ctx, const GpuTensor& t) {
    using rhi::ResourceState;
    ctx.bufferBarrier(t.buf, ResourceState::Common, ResourceState::CopySource);
    ctx.copyBuffer(t.rb, t.buf, t.bytes);
    ctx.bufferBarrier(t.buf, ResourceState::CopySource, ResourceState::Common);
}

std::vector<f32> readTensor(rhi::IResourceFactory& res, const GpuTensor& t) {
    std::vector<f32> v(t.bytes / sizeof(f32), 0.0f);
    if (!res.readBuffer(t.rb, v.data(), t.bytes, 0)) v.clear();
    return v;
}

bool bitsEqual(std::span<const f32> a, std::span<const f32> b) {
    return a.size() == b.size() && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(f32)) == 0);
}

ConvLayerDesc convLayer(u32 cin, u32 cout, u32 k, u32 s, Activation a) {
    ConvLayerDesc l;
    l.cin = cin; l.cout = cout; l.kernel = k; l.stride = s; l.act = a; l.bias = true;
    return l;
}

// The NRD2 encoder (docs/rendering/NRD2.md).
ConvNetDesc nrd2Desc() {
    ConvNetDesc d;
    d.inChannels = 12;
    d.layers = {convLayer(12, 16, 3, 2, Activation::ReLU), convLayer(16, 16, 3, 1, Activation::ReLU),
                convLayer(16, 32, 3, 2, Activation::ReLU), convLayer(32, 32, 3, 1, Activation::ReLU),
                convLayer(32, 12, 1, 1, Activation::None)};
    d.seed = 11;
    return d;
}

// Random non-zero weights everywhere (the zero-initialised head would hide the last layer): He-uniform
// bounds per layer, biases in [-0.1, 0.1].
std::vector<f32> randomConvWeights(const ConvNetDesc& d, Lcg& rng) {
    const ConvLayout L = ConvLayout::make(d);
    std::vector<f32> w(L.total);
    for (u32 l = 0; l < L.layers; ++l) {
        const f32 bound = std::sqrt(6.0f / static_cast<f32>(L.cin[l] * L.kernel[l] * L.kernel[l]));
        for (u32 k = 0; k < L.layerSize[l]; ++k) {
            const bool bias = L.wOffset[l] + k >= L.bOffset[l];
            w[L.wOffset[l] + k] = rng.next() * (bias ? 0.1f : bound);
        }
    }
    return w;
}

// NeuralConvTest's toy task: a smooth field seen through 4 gains; the target is its 1/4-resolution mean.
void toyConvBatch(Lcg& rng, u32 n, u32 size, std::vector<f32>& in, std::vector<f32>& target, std::vector<f32>& pw) {
    const u32 cells = size / 4;
    const f32 gain[4] = {1.0f, 0.8f, 0.6f, 0.4f};
    in.assign(static_cast<usize>(n) * 4 * size * size, 0.0f);
    target.assign(static_cast<usize>(n) * 4 * cells * cells, 0.0f);
    pw.assign(static_cast<usize>(n) * cells * cells, 1.0f);
    for (u32 r = 0; r < n; ++r) {
        const f32 a = 0.075f + 0.045f * rng.next(), b = 0.075f + 0.045f * rng.next(), ph = 3.14f * (1.0f + rng.next());
        for (u32 c = 0; c < 4; ++c) {
            f32* plane = &in[(static_cast<usize>(r) * 4 + c) * size * size];
            for (u32 y = 0; y < size; ++y)
                for (u32 x = 0; x < size; ++x)
                    plane[y * size + x] =
                        0.5f + gain[c] * 0.5f * std::sin(a * static_cast<f32>(y) + b * static_cast<f32>(x) + ph);
            f32* tp = &target[(static_cast<usize>(r) * 4 + c) * cells * cells];
            for (u32 cy = 0; cy < cells; ++cy)
                for (u32 cx = 0; cx < cells; ++cx) {
                    f32 s = 0.0f;
                    for (u32 y = 0; y < 4; ++y)
                        for (u32 x = 0; x < 4; ++x) s += plane[(cy * 4 + y) * size + cx * 4 + x];
                    tp[cy * cells + cx] = s / 16.0f;
                }
        }
    }
}

void runConvParity(rhi::IDevice& dev) {
    AVER_INFO("-- ConvNet (NRD2 shape) vs ConvNetReference");
    rhi::IResourceFactory& res = *dev.resources();
    TensorPool pool;
    pool.res = &res;

    const ConvNetDesc desc = nrd2Desc();
    const OptimiserDesc opt = convDefaults();
    const TensorShape shape{2, 12, 41, 57};
    ConvNetReference ref(desc, opt);
    const TensorShape headShape = ref.outputShape(shape);
    check(headShape == TensorShape{2, 12, 11, 15}, "the NRD2 net maps 2x12x41x57 to 2x12x11x15");

    Lcg rng;
    rng.s = 2024u;
    const std::vector<f32> weights = randomConvWeights(desc, rng);
    std::vector<f32> input(shape.count()), target(headShape.count());
    std::vector<f32> posW(static_cast<usize>(headShape.n) * headShape.planeCount());
    for (f32& v : input) v = rng.next();
    for (f32& v : target) v = 0.5f * rng.next();
    for (usize i = 0; i < posW.size(); ++i) posW[i] = (i % 7 == 3) ? 0.0f : 0.5f + 0.5f * rng.next();
    const f32 lossNorm = static_cast<f32>(headShape.n * headShape.planeCount());
    check(ref.setWeights(weights), "ConvNetReference takes the random weights");

    std::vector<std::vector<f32>> acts;
    std::vector<f32> cpuOut(headShape.count());
    ref.forward(shape, input, cpuOut, false, &acts);

    const GpuTensor gIn = pool.make(input.size(), false, &input);
    const GpuTensor gTarget = pool.make(target.size(), false, &target);
    const GpuTensor gPosW = pool.make(posW.size(), false, &posW);
    const GpuTensor gOut = pool.make(cpuOut.size(), true);
    const GpuTensor gLoss = pool.make(shape.n, true);
    if (!gIn.buf || !gIn.up || !gTarget.buf || !gPosW.buf || !gOut.buf || !gLoss.buf || !gOut.rb || !gLoss.rb) {
        check(false, "conv test buffers allocate");
        return;
    }
    check(dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
              uploadTensor(ctx, gIn);
              uploadTensor(ctx, gTarget);
              uploadTensor(ctx, gPosW);
          }),
          "conv test data uploaded");

    // ---- a) forward, layer by layer: each layer as a one-layer network fed the CPU's previous activations
    {
        const ConvLayout L = ConvLayout::make(desc);
        bool allOk = true;
        f32 worst = 0.0f;
        TensorShape cur = shape;
        for (u32 l = 0; l < L.layers; ++l) {
            ConvNetDesc one;
            one.inChannels = desc.layers[l].cin;
            one.layers = {desc.layers[l]};
            const TensorShape os = L.outDims(l, cur);
            ConvNet net;
            const bool created = net.create(dev, one, opt, ConvMode::Infer);
            const std::span<const f32> slice(&weights[L.wOffset[l]], L.layerSize[l]);
            const std::vector<f32>& lin = l == 0 ? input : acts[l - 1];
            const GpuTensor tin = pool.make(lin.size(), false, &lin);
            const GpuTensor tout = pool.make(os.count(), true);
            const TensorShape shapes[1] = {cur};
            bool recorded = false;
            const bool ran = created && net.reserve(shapes) && net.uploadWeights(slice) &&
                             dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
                                 uploadTensor(ctx, tin);
                                 recorded = net.recordInfer(ctx, tin.buf, tout.buf, cur);
                                 copyToReadback(ctx, tout);
                             });
            const std::vector<f32> got = readTensor(res, tout);
            const Compare c = compare(got, acts[l], 1e-5f, 1e-6f);
            const bool ok = ran && recorded && c.finite && c.worstRatio <= 1.0f;
            AVER_INFO("  layer {}: {}x{}x{} -> {}x{}x{}, max abs error {:.3g} ({:.2f} of the tolerance)", l, cur.c, cur.h,
                      cur.w, os.c, os.h, os.w, static_cast<double>(c.maxAbs), static_cast<double>(c.worstRatio));
            allOk = allOk && ok;
            worst = std::max(worst, c.maxAbs);
            if (!ok) check(false, "conv layer " + std::to_string(l) + " forward matches the reference");
            cur = os;
        }
        char err[32];
        std::snprintf(err, sizeof err, "%.3g", static_cast<double>(worst));
        check(allOk, std::string("every layer's forward matches ConvNetReference (rel 1e-5, abs 1e-6; max abs error ") +
                         err + ")");
    }

    // ---- a) end to end, and c) a bit-identical rerun
    ConvNet infer;
    const TensorShape shapes[1] = {shape};
    check(infer.create(dev, desc, opt, ConvMode::Infer) && infer.reserve(shapes) && infer.uploadWeights(weights),
          "ConvNet::create (Infer), reserve, uploadWeights");
    std::vector<f32> run1, run2;
    for (std::vector<f32>* dst : {&run1, &run2}) {
        bool recorded = false;
        const bool ran = dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
            recorded = infer.recordInfer(ctx, gIn.buf, gOut.buf, shape);
            copyToReadback(ctx, gOut);
        });
        *dst = ran && recorded ? readTensor(res, gOut) : std::vector<f32>{};
    }
    check(std::any_of(run1.begin(), run1.end(), [](f32 v) { return std::fabs(v) > 1e-3f; }),
          "conv outputs are not all zero");
    expectClose("end-to-end forward matches ConvNetReference::forward (rel 1e-5, abs 1e-6)", run1, cpuOut, 1e-5f, 1e-6f);
    check(!run1.empty() && bitsEqual(run1, run2), "two GPU forward runs are bit-identical");

    // ---- b) training: evaluate, then 1 step, then 10
    ConvNet train;
    check(train.create(dev, desc, opt, ConvMode::Train) && train.reserve(shapes) && train.uploadWeights(weights),
          "ConvNet::create (Train), reserve, uploadWeights");
    {
        bool recorded = false;
        const bool ran = dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
            recorded = train.recordEvaluate(ctx, gIn.buf, gTarget.buf, gPosW.buf, gLoss.buf, shape, lossNorm, false);
            copyToReadback(ctx, gLoss);
        });
        const std::vector<f32> per = ran && recorded ? readTensor(res, gLoss) : std::vector<f32>{};
        f32 sum = 0.0f;
        for (f32 v : per) sum += v;
        const f32 want = ref.evaluate(shape, input, target, posW, lossNorm, false);
        AVER_INFO("  evaluate: GPU {:.7f}, CPU {:.7f}", static_cast<double>(sum), static_cast<double>(want));
        check(per.size() == shape.n && std::fabs(sum - want) <= 1e-4f * std::fabs(want) + 1e-7f,
              "recordEvaluate's per-record losses sum to ConvNetReference::evaluate (rel 1e-4)");
    }

    // One step per submission: the Vulkan backend allocates constant descriptors per dispatch (README).
    const auto trainSteps = [&](u32 steps, std::vector<f32>& master, std::vector<f32>& ema) {
        bool ok = true;
        for (u32 s = 0; s < steps && ok; ++s) {
            bool recorded = false, rb = true;
            ok = dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
                recorded = train.recordTrain(ctx, gIn.buf, gTarget.buf, gPosW.buf, shape, lossNorm);
                if (s + 1 == steps) rb = train.recordReadback(ctx);
            }) && recorded && rb;
        }
        ok = ok && train.collectWeights();
        const std::span<const f32> m = train.cpuWeights(false), e = train.cpuWeights(true);
        master.assign(m.begin(), m.end());
        ema.assign(e.begin(), e.end());
        return ok;
    };

    std::vector<f32> m1, e1, m10, e10;
    check(trainSteps(1, m1, e1), "GPU training step 1 ran and read back");
    ref.trainBatch(shape, input, target, posW, lossNorm);
    f32 moved = 0.0f;
    for (usize i = 0; i < m1.size() && i < weights.size(); ++i) moved = std::max(moved, std::fabs(m1[i] - weights[i]));
    check(moved > 1e-5f, "the GPU step moved the weights");
    // abs 2e-5 as the MLP section: a gradient on a fixed-point rounding boundary can move Adam's first step.
    expectClose("after 1 step, master weights match trainBatch (rel 1e-4)", m1, ref.weights(), 1e-4f, 2e-5f);
    expectClose("after 1 step, EMA weights match trainBatch (rel 1e-4)", e1, ref.ema(), 1e-4f, 2e-5f);

    check(trainSteps(9, m10, e10), "GPU training steps 2..10 ran and read back");
    for (u32 s = 1; s < 10; ++s) ref.trainBatch(shape, input, target, posW, lossNorm);
    check(train.step() == 10 && ref.step() == 10, "ten Adam steps counted on both sides");
    expectClose("after 10 steps, master weights match trainBatch (rel 1e-4)", m10, ref.weights(), 1e-4f, 2e-5f);
    expectClose("after 10 steps, EMA weights match trainBatch (rel 1e-4)", e10, ref.ema(), 1e-4f, 2e-5f);

    // ---- c) the same 10 steps again from the same upload: bit-identical
    {
        std::vector<f32> m10b, e10b;
        check(train.uploadWeights(weights) && trainSteps(10, m10b, e10b), "a second 10-step GPU run");
        check(bitsEqual(m10, m10b) && bitsEqual(e10, e10b), "two 10-step GPU training runs are bit-identical");
    }

    // ---- d) the toy task learned on the GPU
    {
        ConvNetDesc toy;
        toy.inChannels = 4;
        toy.layers = {convLayer(4, 8, 3, 2, Activation::ReLU), convLayer(8, 8, 3, 2, Activation::ReLU),
                      convLayer(8, 4, 1, 1, Activation::None)};
        toy.seed = 3;
        OptimiserDesc toyOpt = convDefaults();
        toyOpt.learningRate = 5e-3f;
        constexpr u32 size = 32, cells = 8, batch = 2, batches = 4, evalN = 4, steps = 150;
        const TensorShape bShape{batch, 4, size, size}, eShape{evalN, 4, size, size};
        const f32 bNorm = static_cast<f32>(batch * cells * cells), eNorm = static_cast<f32>(evalN * cells * cells);

        Lcg trng;
        trng.s = 777u;
        std::vector<GpuTensor> bIn, bTgt, bPw;
        std::vector<std::vector<f32>> cIn(batches), cTgt(batches), cPw(batches);
        for (u32 k = 0; k < batches; ++k) {
            toyConvBatch(trng, batch, size, cIn[k], cTgt[k], cPw[k]);
            bIn.push_back(pool.make(cIn[k].size(), false, &cIn[k]));
            bTgt.push_back(pool.make(cTgt[k].size(), false, &cTgt[k]));
            bPw.push_back(pool.make(cPw[k].size(), false, &cPw[k]));
        }
        std::vector<f32> evIn, evTgt, evPw;
        toyConvBatch(trng, evalN, size, evIn, evTgt, evPw);
        const GpuTensor gEvIn = pool.make(evIn.size(), false, &evIn);
        const GpuTensor gEvTgt = pool.make(evTgt.size(), false, &evTgt);
        const GpuTensor gEvPw = pool.make(evPw.size(), false, &evPw);
        const GpuTensor gEvLoss = pool.make(evalN, true);

        ConvNet net;
        const TensorShape toyShapes[2] = {bShape, eShape};
        const bool made = net.create(dev, toy, toyOpt, ConvMode::Train) && net.reserve(toyShapes);
        check(made, "toy ConvNet (Train) created and reserved");
        if (!made) return;
        check(dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
                  for (u32 k = 0; k < batches; ++k) {
                      uploadTensor(ctx, bIn[k]);
                      uploadTensor(ctx, bTgt[k]);
                      uploadTensor(ctx, bPw[k]);
                  }
                  uploadTensor(ctx, gEvIn);
                  uploadTensor(ctx, gEvTgt);
                  uploadTensor(ctx, gEvPw);
              }),
              "toy data uploaded");

        const auto evalLoss = [&](bool useEma) {
            bool recorded = false;
            const bool ran = dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
                recorded = net.recordEvaluate(ctx, gEvIn.buf, gEvTgt.buf, gEvPw.buf, gEvLoss.buf, eShape, eNorm, useEma);
                copyToReadback(ctx, gEvLoss);
            });
            if (!ran || !recorded) return -1.0f;
            f32 s = 0.0f;
            for (f32 v : readTensor(res, gEvLoss)) s += v;
            return s;
        };
        const f32 before = evalLoss(false);
        bool ok = true;
        for (u32 s = 0; s < steps && ok; ++s) {
            const u32 k = s % batches;
            bool recorded = false;
            ok = dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
                recorded = net.recordTrain(ctx, bIn[k].buf, bTgt[k].buf, bPw[k].buf, bShape, bNorm);
            }) && recorded;
        }
        const f32 after = evalLoss(false), afterEma = evalLoss(true);

        ConvNetReference cpu(toy, toyOpt);
        for (u32 s = 0; s < steps; ++s) {
            const u32 k = s % batches;
            cpu.trainBatch(bShape, cIn[k], cTgt[k], cPw[k], bNorm);
        }
        const f32 cpuAfter = cpu.evaluate(eShape, evIn, evTgt, evPw, eNorm, false);
        AVER_INFO("  toy (GPU): held-out loss {:.5f} -> {:.5f} (EMA {:.5f}) after {} steps; CPU twin {:.5f}",
                  static_cast<double>(before), static_cast<double>(after), static_cast<double>(afterEma), steps,
                  static_cast<double>(cpuAfter));
        check(ok, "toy training steps ran");
        check(before > 0.05f && std::isfinite(after) && after >= 0.0f, "toy losses are finite; the untrained net has a real loss");
        check(after * 5.0f < before, "the GPU learned the toy task (held-out loss falls at least 5x)");
        check(std::fabs(after - cpuAfter) <= 0.05f * cpuAfter + 1e-4f, "GPU and CPU toy runs land on the same loss (5%)");
    }
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
    runConvParity(*dev);
    rhi::destroyDevice(dev);

    if (g_failures != 0) {
        AVER_ERROR("=== NeuralGpuParityTest FAILED === ({} failure(s))", g_failures);
        return 1;
    }
    AVER_INFO("=== NeuralGpuParityTest PASSED ===");
    return 0;
}
