// Nrd2TrainerGpuTest: NRD2 phase 4 on a real device (WARP by default; `hw` the adapter, `vulkan` the Vulkan
// backend). Synthetic pose files -> Nrd2Trainer driven one IDevice::runStandaloneCompute per frame:
//   a) a short session finishes, validates, saves the best weights, the .last checkpoint and sidecars
//   b) the held-out ratio it saved matches a CPU recomputation (nrd2ExtractPatch + ConvNetReference): the
//      GPU gather and the parameter-space metric agree with their spec
//   c) Nrd2Network's inference (standardise, ConvNet, de-standardise + clamp) matches the CPU twin; the
//      live gate keeps it off above 0.9
//   d) a second session resumes from .last (lifetime steps continue); a cancelled one saves .last
// Skips (77) when the backend or runStandaloneCompute is unavailable.
#include "Nrd2TestPoses.hpp"

#include "aver/core/Log.hpp"
#include "aver/render/denoise/Nrd2Network.hpp"
#include "aver/render/neural/ConvNetReference.hpp"
#include "aver/render/neural/WeightFile.hpp"
#include "aver/rhi/RHI.hpp"
#include "aver/rhi/RHIResources.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

using namespace aver;
using namespace aver::render::denoise;
using namespace nrd2test;
namespace neural = aver::render::neural;
namespace fs = std::filesystem;

namespace {

int g_failures = 0;
constexpr int kSkip = 77;

void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok  {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

constexpr u32 kTilesX = 16, kTilesY = 11, kFrames = 2;

// Steps the trainer one standalone submission at a time until it is no longer active (bounded).
Nrd2TrainStatus run(rhi::IDevice& dev, Nrd2Trainer& t, u32 maxFrames, u32 cancelAfterTrainingFrames = 0) {
    u32 training = 0;
    for (u32 f = 0; f < maxFrames && t.active(); ++f) {
        if (!dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) { t.step(ctx); })) break;
        const Nrd2TrainStatus s = t.status();
        if (s.phase == Nrd2TrainStatus::Phase::Loading) std::this_thread::sleep_for(std::chrono::milliseconds(2));
        if (s.phase == Nrd2TrainStatus::Phase::Training && cancelAfterTrainingFrames && ++training == cancelAfterTrainingFrames)
            t.cancel();
    }
    const Nrd2TrainStatus s = t.status();
    dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) { t.step(ctx); });   // joins the worker, frees the GPU side
    return s;
}

// CPU held-out ratio of a weight file: EMA weights in the file, records as the trainer builds them.
f32 cpuRatio(const std::string& weights, const std::vector<Nrd2PoseInfo>& poses) {
    neural::ConvNetDesc d = nrd2NetworkDesc();
    std::vector<f32> w;
    neural::ConvIoAffine io;
    Nrd2Standardisation s;
    if (!neural::loadConvWeightFile(weights, d, w, &io) || !nrd2StandardisationFromAffine(io, s)) return -1.0f;
    neural::ConvNetReference net(d, neural::convDefaults());
    net.setWeights(w);
    f32 ts[12], tb[12], def[12], defStd[12];
    nrd2TargetAffine(s, ts, tb);
    nrd2DefaultParams(def);
    for (u32 p = 0; p < 12; ++p) defStd[p] = def[p] * ts[p] + tb[p];
    f64 sumNet = 0.0, sumDef = 0.0;
    std::vector<f32> in(12 * 56 * 56), tg(12 * 196), wt(12 * 196), out(12 * 196);
    for (u32 i = 0; i < poses.size(); ++i) {
        if (!poses[i].heldOut) continue;
        Nrd2Pose p;
        if (!readNrd2Pose(poses[i].path, p)) return -1.0f;
        for (const Nrd2PatchRef& r : nrd2ValidationPatches(i, p.tilesX, p.tilesY, 0)) {
            nrd2ExtractPatch(p, io, ts, tb, r, in.data(), tg.data(), wt.data());
            net.forward({1, 12, 56, 56}, in, out, true);
            for (usize k = 0; k < out.size(); ++k) {
                const f64 e = static_cast<f64>(out[k]) - tg[k];
                sumNet += wt[k] * e * e;
            }
            sumDef += nrd2DefaultLoss(defStd, tg.data(), wt.data());
        }
    }
    return sumDef > 0.0 ? static_cast<f32>(sumNet / sumDef) : -1.0f;
}

void runTrainer(rhi::IDevice& dev) {
    const fs::path root = fs::temp_directory_path() / "aver_nrd2_gpu_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "data" / "SceneA", ec);
    fs::create_directories(root / "data" / "SceneB", ec);
    bool wrote = true;
    for (u32 i = 0; i < 4; ++i)
        wrote = wrote && writeNrd2Pose((root / "data" / "SceneA" / poseName(i)).string(),
                                       makePose("SceneA", i, i == 3, kTilesX, kTilesY, kFrames, 1 + i));
    for (u32 i = 0; i < 3; ++i)
        wrote = wrote && writeNrd2Pose((root / "data" / "SceneB" / poseName(i)).string(),
                                       makePose("SceneB", i, i == 2, kTilesX, kTilesY, kFrames, 20 + i));
    check(wrote, "synthetic dataset written (2 scenes, 5 training + 2 held-out poses)");
    const std::string weights = (root / "nrd2_v1.avnn").string();

    Nrd2TrainConfig cfg;
    cfg.dirs = {(root / "data").string()};
    cfg.weightsPath = weights;
    cfg.steps = 120;
    cfg.batch = 4;
    cfg.validateEvery = 40;
    cfg.saveEvery = 40;
    cfg.swapEvery = 10;
    cfg.vramBudget = 600ull * 1024;   // a small cache: 3 of the 5 training poses resident, the rest stream
    cfg.resume = false;

    // ---- a) a session
    {
        Nrd2Trainer t(dev);
        check(t.start(cfg) && t.active(), "the trainer starts");
        const Nrd2TrainStatus s = run(dev, t, 4000);
        AVER_INFO("  session: phase {}, {} steps, {} evaluations, last ratio {:.4f}, best {:.4f}, loss {:.4f}, {} resident: {}",
                  static_cast<int>(s.phase), s.sessionSteps, s.evaluations, static_cast<double>(s.lastRatio),
                  static_cast<double>(s.bestRatio), static_cast<double>(s.loss), s.residentPoses, s.message);
        check(s.phase == Nrd2TrainStatus::Phase::Finished && s.sessionSteps == 120 && s.lifetimeSteps == 120,
              "the session finishes at the step target");
        check(s.evaluations == 3 && s.trainPoses == 5 && s.heldOutPoses == 2, "validated every 40 steps on the held-out poses");
        check(s.residentPoses < 5, "the pose cache streamed (fewer residents than training poses)");
        check(s.bestRatio > 0.0f && s.bestRatio < 1.0f && std::isfinite(s.loss) && s.loss >= 0.0f,
              "the network beats the default parameters on held-out tiles (parameter space)");
        check(s.scenes.size() == 2 && s.scenes[0].ratio > 0.0f && s.scenes[1].ratio > 0.0f, "a ratio per scene");
    }
    Nrd2Sidecar best, last;
    check(fs::exists(weights) && readNrd2Sidecar(weights + ".steps", best) && best.valRatio > 0.0f,
          "best weights and sidecar saved");
    check(fs::exists(nrd2LastPath(weights)) && readNrd2Sidecar(nrd2LastPath(weights) + ".steps", last) &&
              last.lifetimeSteps == 120 && last.datasetId == best.datasetId,
          ".last checkpoint and sidecar saved at the final step");

    // ---- b) the saved ratio against the CPU twin
    {
        std::vector<Nrd2PoseInfo> poses = scanNrd2Dataset(cfg.dirs);
        const f32 cpu = cpuRatio(weights, poses);
        AVER_INFO("  held-out ratio: GPU {:.6f}, CPU {:.6f}", static_cast<double>(best.valRatio), static_cast<double>(cpu));
        check(cpu > 0.0f && std::fabs(cpu - best.valRatio) <= 1e-3f * cpu,
              "the GPU gather + metric give the CPU twin's held-out ratio (rel 1e-3)");
    }

    // ---- c) inference through Nrd2Network
    {
        const std::string inferPath = (root / "infer.avnn").string();
        fs::copy_file(weights, inferPath, fs::copy_options::overwrite_existing, ec);
        Nrd2Sidecar open = best;
        open.valRatio = 0.5f;
        writeNrd2Sidecar(inferPath + ".steps", open);

        Nrd2Pose p;
        readNrd2Pose((root / "data" / "SceneA" / poseName(3)).string(), p);
        const usize plane = static_cast<usize>(p.halfW) * p.halfH, tiles = static_cast<usize>(p.tilesX) * p.tilesY;
        std::vector<f32> raw(12 * plane);
        for (usize i = 0; i < raw.size(); ++i) raw[i] = nrd2F16ToF32(p.features[i]);

        rhi::IResourceFactory& res = *dev.resources();
        auto buf = [&](u64 bytes, rhi::BufferKind k, bool uav) {
            rhi::BufferDesc bd{};
            bd.bytes = bytes; bd.kind = k; bd.allowUnorderedAccess = uav; bd.debugName = "Nrd2TrainerGpuTest";
            return res.createBuffer(bd);
        };
        const rhi::BufferHandle feat = buf(raw.size() * 4, rhi::BufferKind::Default, true);
        const rhi::BufferHandle up = buf(raw.size() * 4, rhi::BufferKind::Upload, false);
        const rhi::BufferHandle params = buf(12 * tiles * 4, rhi::BufferKind::Default, true);
        const rhi::BufferHandle rb = buf(12 * tiles * 4, rhi::BufferKind::Readback, false);
        f32 def[12];
        nrd2DefaultParams(def);

        Nrd2Network net;
        net.setWeightPaths(inferPath, "");
        bool ready = false, recorded = false;
        const bool ran = feat && up && params && rb && dev.runStandaloneCompute([&](rhi::IRenderContext& ctx) {
            ready = net.ready(dev);
            // The live path standardises in CSNrd2Features; the network reads standardised features.
            if (ready) {
                std::vector<f32> stdz(raw.size());
                for (usize i = 0; i < raw.size(); ++i) {
                    const usize c = i / plane;
                    const f32 v = raw[i] == raw[i] ? raw[i] : 0.0f;   // NaN -> 0 first, as the features pass
                    stdz[i] = std::fabs(v) < 3.0e38f ? v * net.inScale()[c] + net.inBias()[c] : 0.0f;
                }
                res.writeBuffer(up, stdz.data(), stdz.size() * 4);
            }
            ctx.bufferBarrier(feat, rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
            ctx.copyBuffer(feat, up, raw.size() * 4);
            ctx.bufferBarrier(feat, rhi::ResourceState::CopyDest, rhi::ResourceState::Common);
            if (ready)
                recorded = net.record(ctx, feat, static_cast<u32>(raw.size()), params, static_cast<u32>(12 * tiles),
                                      p.tilesX, p.tilesY, def);
            ctx.bufferBarrier(params, rhi::ResourceState::Common, rhi::ResourceState::CopySource);
            ctx.copyBuffer(rb, params, 12 * tiles * 4);
            ctx.bufferBarrier(params, rhi::ResourceState::CopySource, rhi::ResourceState::Common);
        });
        check(ran && ready && recorded && net.status().running && net.status().gateOpen &&
                  net.status().source == Nrd2NetworkStatus::Source::User,
              "Nrd2Network loads the weights, opens the gate (0.5) and records");
        std::vector<f32> got(12 * tiles);
        res.readBuffer(rb, got.data(), got.size() * 4, 0);

        neural::ConvNetDesc d = nrd2NetworkDesc();
        std::vector<f32> w;
        neural::ConvIoAffine io;
        neural::loadConvWeightFile(inferPath, d, w, &io);
        neural::ConvNetReference ref(d, neural::convDefaults());
        ref.setWeights(w);
        std::vector<f32> x(raw.size()), y(12 * tiles);
        for (usize i = 0; i < raw.size(); ++i) x[i] = raw[i] * io.inScale[i / plane] + io.inBias[i / plane];
        ref.forward({1, 12, p.halfH, p.halfW}, x, y, true);
        f32 worst = 0.0f;
        bool varied = false;
        for (usize i = 0; i < y.size(); ++i) {
            const u32 q = static_cast<u32>(i / tiles);
            const f32 lim = (q % 6) < 3 ? 8.0f : 6.0f;
            const f32 want = std::clamp(y[i] * io.outScale[q] + io.outBias[q], -lim, lim);
            worst = std::max(worst, std::fabs(got[i] - want) / (1e-4f + 1e-4f * std::fabs(want)));
            varied = varied || std::fabs(got[i] - def[q]) > 1e-3f;
        }
        AVER_INFO("  inference: worst error {:.3f} of the tolerance", static_cast<double>(worst));
        check(worst <= 1.0f && varied, "network tile parameters match the CPU twin (rel/abs 1e-4) and differ from the defaults");

        Nrd2Sidecar closed = open;
        closed.valRatio = 0.95f;
        writeNrd2Sidecar(inferPath + ".steps", closed);
        Nrd2Network gated;
        gated.setWeightPaths(inferPath, "");
        bool readyClosed = true;
        dev.runStandaloneCompute([&](rhi::IRenderContext&) { readyClosed = gated.ready(dev); });
        check(!readyClosed && !gated.status().gateOpen && !gated.status().running, "the live gate keeps it off at 0.95");
        Nrd2Network none;
        none.setWeightPaths((root / "missing.avnn").string(), "");
        bool readyNone = true;
        dev.runStandaloneCompute([&](rhi::IRenderContext&) { readyNone = none.ready(dev); });
        check(!readyNone && none.status().source == Nrd2NetworkStatus::Source::None, "no weights: the defaults run");
        res.waitIdle();
        for (rhi::BufferHandle b : {feat, up, params, rb}) if (b) res.destroyBuffer(b);
    }

    // ---- d) resume, then cancel
    {
        Nrd2TrainConfig c2 = cfg;
        c2.steps = 40;
        c2.resume = true;
        Nrd2Trainer t(dev);
        check(t.start(c2), "a resumed session starts");
        const Nrd2TrainStatus s = run(dev, t, 2000);
        check(s.phase == Nrd2TrainStatus::Phase::Finished && s.lifetimeSteps == 160 && s.sessionSteps == 40,
              "it resumes from .last: lifetime steps continue (120 + 40)");
    }
    {
        Nrd2TrainConfig c3 = cfg;
        c3.steps = 100000;
        c3.resume = true;
        Nrd2Trainer t(dev);
        t.start(c3);
        const Nrd2TrainStatus s = run(dev, t, 4000, 5);
        Nrd2Sidecar sc;
        check(s.phase == Nrd2TrainStatus::Phase::Cancelled && readNrd2Sidecar(nrd2LastPath(weights) + ".steps", sc) &&
                  sc.lifetimeSteps == s.lifetimeSteps && s.lifetimeSteps > 160,
              "a cancelled session saves .last where it stopped");
    }
    fs::remove_all(root, ec);
}

}  // namespace

int main(int argc, char** argv) {
    AVER_INFO("=== Nrd2TrainerGpuTest ===");
    bool wantVulkan = false, hardware = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "vulkan") == 0) wantVulkan = true;
        if (std::strcmp(argv[i], "hw") == 0) hardware = true;
    }
    rhi::DeviceDesc desc;
    desc.useWarp = !hardware;
    desc.preferred[0] = wantVulkan ? rhi::Backend::Vulkan : rhi::Backend::D3D12;
    desc.preferred[1] = rhi::Backend::Null;
    desc.preferredCount = 2;
    rhi::IDevice* dev = rhi::createDevice(desc);
    if (!dev || dev->backend() == rhi::Backend::Null || !dev->resources()) {
        AVER_WARN("  SKIP  no {} device on this machine", wantVulkan ? "Vulkan" : "D3D12");
        if (dev) rhi::destroyDevice(dev);
        return kSkip;
    }
    if (!dev->runStandaloneCompute([](rhi::IRenderContext&) {})) {
        AVER_WARN("  SKIP  IDevice::runStandaloneCompute is unavailable on this backend");
        rhi::destroyDevice(dev);
        return kSkip;
    }
    AVER_INFO("device: {} ({})", dev->adapterName(), rhi::backendName(dev->backend()));
    runTrainer(*dev);
    rhi::destroyDevice(dev);
    if (g_failures != 0) {
        AVER_ERROR("=== Nrd2TrainerGpuTest FAILED === ({} failure(s))", g_failures);
        return 1;
    }
    AVER_INFO("=== Nrd2TrainerGpuTest PASSED ===");
    return 0;
}
