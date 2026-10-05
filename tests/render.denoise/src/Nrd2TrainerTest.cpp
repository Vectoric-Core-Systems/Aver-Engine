// Nrd2TrainerTest: NRD2 phase 4's CPU pieces (Nrd2Trainer.hpp): the network shape, standardisation,
// patch extraction (the spec for CSNrd2Gather), core tiles versus a full-frame forward, sampling
// determinism, validation coverage and metric, dataset scan / id / held-out split, sidecar, learning rate
// and live gate. Headless: no GPU.
#include "Nrd2TestPoses.hpp"

#include "aver/core/Log.hpp"
#include "aver/render/neural/ConvNetReference.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

using namespace aver;
using namespace aver::render::denoise;
using namespace nrd2test;
namespace neural = aver::render::neural;

static int g_failures = 0;

static void check(bool cond, const std::string& what) {
    if (cond) { AVER_INFO("  ok    {}", what); return; }
    ++g_failures;
    AVER_ERROR("  FAIL  {}", what);
}

namespace {

void testNetwork() {
    AVER_INFO("-- the network");
    const neural::ConvNetDesc d = nrd2NetworkDesc();
    std::string why;
    check(neural::validate(d, &why), "nrd2NetworkDesc validates (" + why + ")");
    neural::ConvNetReference ref(d, neural::convDefaults());
    check(ref.outputShape({1, 12, 4 * 23, 4 * 17}) == neural::TensorShape{1, 12, 23, 17},
          "4 tilesY x 4 tilesX features map to tilesY x tilesX x 12 parameters");
    check(ref.outputShape({32, 12, 56, 56}) == neural::TensorShape{32, 12, 14, 14}, "a 56x56 record maps to 14x14 tiles");
    check(ref.weightCount() == 18348, "18,348 weights (docs/rendering/NRD2.md)");
}

void testStandardisation() {
    AVER_INFO("-- standardisation");
    const Nrd2Pose a = makePose("A", 0, false, 12, 9, 2, 1), b = makePose("A", 1, false, 12, 9, 2, 2);
    Nrd2StatsAccumulator acc;
    acc.addPose(a);
    acc.addPose(b);
    Nrd2Standardisation s;
    check(acc.finish(s) && acc.poses() == 2, "two poses accumulate");
    // Reference: plain sums over every texel of every frame of both poses.
    bool inOk = true, outOk = true;
    for (u32 c = 0; c < 12; ++c) {
        double s1 = 0, s2 = 0, n = 0;
        for (const Nrd2Pose* p : {&a, &b}) {
            const usize plane = static_cast<usize>(p->halfW) * p->halfH;
            for (u32 k = 0; k < p->frames; ++k)
                for (usize i = 0; i < plane; ++i) {
                    const double v = nrd2F16ToF32(p->features[(static_cast<usize>(k) * 12 + c) * plane + i]);
                    s1 += v; s2 += v * v; n += 1;
                }
        }
        const double m = s1 / n, sd = std::sqrt(std::max(s2 / n - m * m, 0.0));
        inOk = inOk && std::fabs(s.inMean[c] - m) < 1e-5 && std::fabs(s.inStd[c] - std::max(sd, 1e-3)) < 1e-5;
        double w1 = 0, w2 = 0, w = 0;
        for (const Nrd2Pose* p : {&a, &b}) {
            const usize tiles = static_cast<usize>(p->tilesX) * p->tilesY;
            for (usize t = 0; t < tiles; ++t) {
                const double wt = p->weights[(c / 6) * tiles + t], th = p->theta[c * tiles + t];
                if (wt > 0) { w1 += wt * th; w2 += wt * th * th; w += wt; }
            }
        }
        const double om = w1 / w, osd = std::sqrt(std::max(w2 / w - om * om, 0.0));
        outOk = outOk && std::fabs(s.outMean[c] - om) < 1e-5 && std::fabs(s.outStd[c] - std::max(osd, 1e-2)) < 1e-5;
    }
    check(inOk, "input mean/std per channel over every texel and frame");
    check(outOk, "output mean/std per parameter, weighted by the signal's tile weight");

    const neural::ConvIoAffine io = nrd2IoAffine(s);
    Nrd2Standardisation back;
    bool round = nrd2StandardisationFromAffine(io, back);
    for (u32 c = 0; c < 12 && round; ++c)
        round = std::fabs(back.inMean[c] - s.inMean[c]) <= 1e-5f * (1 + std::fabs(s.inMean[c])) &&
                std::fabs(back.inStd[c] - s.inStd[c]) <= 1e-5f * s.inStd[c] && back.outStd[c] == s.outStd[c] &&
                back.outMean[c] == s.outMean[c];
    check(round, "the io affine round-trips the statistics");
    neural::ConvIoAffine bad = io;
    bad.inScale.pop_back();
    check(!nrd2StandardisationFromAffine(bad, back) && !nrd2StandardisationFromAffine({}, back),
          "an absent or short affine is rejected");
    Nrd2StatsAccumulator none;
    check(!none.finish(s), "an empty accumulator does not finish");
}

void testExtract() {
    AVER_INFO("-- patch extraction (CSNrd2Gather's spec)");
    Nrd2Pose p = makePose("A", 0, false, 20, 15, 2, 3);
    const usize tiles = static_cast<usize>(p.tilesX) * p.tilesY;
    p.theta[4 * tiles + 5 * p.tilesX + 10] = std::numeric_limits<f32>::quiet_NaN();   // tile (10, 5), param 4
    Nrd2StatsAccumulator acc;
    acc.addPose(p);
    Nrd2Standardisation s;
    acc.finish(s);
    const neural::ConvIoAffine io = nrd2IoAffine(s);
    f32 ts[12], tb[12];
    nrd2TargetAffine(s, ts, tb);

    std::vector<f32> in(12 * 56 * 56), tg(12 * 14 * 14), w(12 * 14 * 14);
    const Nrd2PatchRef r{0, 1, 8, -2};   // texels x 32..87 (past the right edge), y -8..47 (past the top)
    nrd2ExtractPatch(p, io, ts, tb, r, in.data(), tg.data(), w.data());
    const usize plane = static_cast<usize>(p.halfW) * p.halfH;
    bool inputs = true;
    for (u32 c = 0; c < 12; ++c)
        for (u32 y = 0; y < 56; ++y)
            for (u32 x = 0; x < 56; ++x) {
                const i32 hx = 32 + static_cast<i32>(x), hy = -8 + static_cast<i32>(y);
                f32 want = 0.0f;
                if (hx < static_cast<i32>(p.halfW) && hy >= 0 && hy < static_cast<i32>(p.halfH))
                    want = nrd2F16ToF32(p.features[(12 + c) * plane + hy * p.halfW + hx]) * io.inScale[c] + io.inBias[c];
                inputs = inputs && in[(c * 56 + y) * 56 + x] == want;
            }
    check(inputs, "inputs: frame 1, standardised inside the pose, 0 past its edges");
    bool targets = true, weights = true, nanTile = false;
    for (u32 q = 0; q < 12; ++q)
        for (u32 ty = 0; ty < 14; ++ty)
            for (u32 tx = 0; tx < 14; ++tx) {
                const i32 gx = 8 + static_cast<i32>(tx), gy = -2 + static_cast<i32>(ty);
                const bool inside = gx < static_cast<i32>(p.tilesX) && gy >= 0 && gy < static_cast<i32>(p.tilesY);
                const bool core = tx >= 3 && tx < 11 && ty >= 3 && ty < 11;
                const usize o = (q * 14 + ty) * 14 + tx;
                f32 wantT = 0.0f, wantW = 0.0f;
                if (inside) {
                    const usize t = static_cast<usize>(gy) * p.tilesX + gx;
                    const f32 th = p.theta[q * tiles + t];
                    if (std::isfinite(th)) {
                        wantT = th * ts[q] + tb[q];
                        const f32 sw = p.weights[(q / 6) * tiles + t];
                        wantW = core && sw > 0.0f ? sw : 0.0f;
                    } else {
                        nanTile = tg[o] == 0.0f && w[o] == 0.0f;
                    }
                }
                targets = targets && tg[o] == wantT;
                weights = weights && w[o] == wantW;
            }
    check(targets, "targets: theta standardised per parameter, 0 outside the pose");
    check(weights, "weights: the signal's tile weight on the core 8x8 only (D for 0..5, S for 6..11)");
    check(nanTile, "a non-finite theta gives target 0 and weight 0");

    // Core tiles of a record equal a full-frame forward on the standardised tensor (patch fully inside).
    neural::ConvNetReference net(nrd2NetworkDesc(), neural::convDefaults());
    std::vector<f32> wts = net.weights();
    u32 k = 1;
    for (f32& v : wts) { k = k * 1664525u + 1013904223u; v = (static_cast<f32>(k >> 8) / 16777216.0f - 0.5f) * 0.3f; }
    net.setWeights(wts);
    const neural::TensorShape full{1, 12, p.halfH, p.halfW};
    std::vector<f32> fin(full.count());
    for (u32 c = 0; c < 12; ++c)
        for (usize i = 0; i < plane; ++i)
            fin[c * plane + i] = nrd2F16ToF32(p.features[c * plane + i]) * io.inScale[c] + io.inBias[c];
    const neural::TensorShape fo = net.outputShape(full);
    std::vector<f32> fout(fo.count()), pout(12 * 14 * 14);
    net.forward(full, fin, fout);
    const Nrd2PatchRef inner{0, 0, 2, 0};   // patch tiles 2..15 x 0..13: the cores' receptive field is inside
    nrd2ExtractPatch(p, io, ts, tb, inner, in.data(), nullptr, nullptr);
    net.forward({1, 12, 56, 56}, in, pout);
    bool same = true;
    for (u32 q = 0; q < 12; ++q)
        for (u32 ty = 3; ty < 11; ++ty)
            for (u32 tx = 3; tx < 11; ++tx) {
                const f32 a = pout[(q * 14 + ty) * 14 + tx];
                const f32 b = fout[(static_cast<usize>(q) * fo.h + ty) * fo.w + tx + 2];
                same = same && std::memcmp(&a, &b, 4) == 0;
            }
    check(same, "core tiles of a record are bit-identical to the full-frame forward");
}

void testSampling() {
    AVER_INFO("-- sampling");
    std::vector<Nrd2PoseInfo> poses(5);
    for (u32 i = 0; i < 5; ++i) {
        poses[i].scene = i < 3 ? "A" : "B";
        poses[i].tilesX = 20 + i; poses[i].tilesY = 11; poses[i].frames = 4;
    }
    poses[4].tilesX = 5;   // narrower than the core
    const std::vector<std::vector<u32>> byScene = {{0, 1, 2}, {3, 4}};
    std::vector<Nrd2PatchRef> a(32), b(32), c(32);
    nrd2SampleBatch(7, 11, byScene, poses, a);
    nrd2SampleBatch(7, 11, byScene, poses, b);
    nrd2SampleBatch(7, 12, byScene, poses, c);
    auto eq = [](const Nrd2PatchRef& x, const Nrd2PatchRef& y) {
        return x.pose == y.pose && x.frame == y.frame && x.tx0 == y.tx0 && x.ty0 == y.ty0;
    };
    check(std::equal(a.begin(), a.end(), b.begin(), eq), "the same counter gives the same batch");
    check(!std::equal(a.begin(), a.end(), c.begin(), eq), "the next counter gives another batch");
    bool alternate = true, inside = true;
    for (u32 i = 0; i < 32; ++i) {
        alternate = alternate && (poses[a[i].pose].scene == (i % 2 == 0 ? "A" : "B"));
        const Nrd2PoseInfo& p = poses[a[i].pose];
        const i32 cx = a[i].tx0 + 3, cy = a[i].ty0 + 3;
        inside = inside && a[i].frame < 4 && cx >= 0 && cy >= 0 &&
                 (p.tilesX < 8 ? cx == 0 : cx + 8 <= static_cast<i32>(p.tilesX)) && cy + 8 <= static_cast<i32>(p.tilesY);
    }
    check(alternate, "scenes take turns record by record (stratified)");
    check(inside, "frames in range, cores inside the grid (narrow poses pin the core at 0)");
    std::vector<Nrd2PatchRef> one(4);
    nrd2SampleBatch(7, 0, {{}, {3}}, poses, one);
    check(std::all_of(one.begin(), one.end(), [](const Nrd2PatchRef& r) { return r.pose == 3; }),
          "a scene with no resident pose is skipped");
}

void testValidation() {
    AVER_INFO("-- validation records and metric");
    const std::vector<Nrd2PatchRef> v = nrd2ValidationPatches(9, 21, 10, 0);
    check(v.size() == 3 * 2, "21 x 10 tiles -> 3 x 2 records");
    std::vector<u32> cover(21 * 10, 0);
    for (const Nrd2PatchRef& r : v)
        for (i32 y = r.ty0 + 3; y < r.ty0 + 11; ++y)
            for (i32 x = r.tx0 + 3; x < r.tx0 + 11; ++x)
                if (x >= 0 && y >= 0 && x < 21 && y < 10) ++cover[y * 21 + x];
    check(std::all_of(cover.begin(), cover.end(), [](u32 n) { return n == 1; }) && v[0].pose == 9,
          "the cores tile the grid exactly once");
    const std::vector<Nrd2PatchRef> some = nrd2ValidationPatches(0, 80, 40, 0, 7);
    check(some.size() == 7, "a cap keeps an evenly spaced subset");

    std::vector<f32> tg(12 * 196), w(12 * 196);
    f32 def[12];
    double want = 0.0;
    for (u32 q = 0; q < 12; ++q) {
        def[q] = 0.1f * q;
        for (u32 i = 0; i < 196; ++i) {
            tg[q * 196 + i] = 0.01f * i - 1.0f;
            w[q * 196 + i] = (i % 3) * 0.5f;
            const double e = static_cast<double>(def[q]) - tg[q * 196 + i];
            want += w[q * 196 + i] * e * e;
        }
    }
    check(std::fabs(nrd2DefaultLoss(def, tg.data(), w.data()) - want) < 1e-9 * want, "default loss = sum w (d' - t')^2");
}

void testDataset() {
    AVER_INFO("-- dataset scan, id, held-out split");
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "aver_nrd2_trainer_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root / "SceneA", ec);
    fs::create_directories(root / "SceneB", ec);
    bool wrote = true;
    for (u32 i = 0; i < 5; ++i)
        wrote = wrote && writeNrd2Pose((root / "SceneA" / poseName(i)).string(), makePose("SceneA", i, i == 4, 9, 8, 1, i));
    for (u32 i = 0; i < 3; ++i)
        wrote = wrote && writeNrd2Pose((root / "SceneB" / poseName(i)).string(), makePose("SceneB", i, false, 9, 8, 1, 10 + i));
    Nrd2Pose stale = makePose("SceneB", 9, false, 9, 8, 1, 3);
    stale.stageBVersion = kNrd2StageBVersion + 1;
    wrote = wrote && writeNrd2Pose((root / "SceneB" / poseName(9)).string(), stale);
    { std::ofstream junk(root / "SceneB" / "pose_010.n2p", std::ios::binary); junk << "nope"; }
    check(wrote, "synthetic pose files written");

    std::vector<std::string> problems;
    std::vector<Nrd2PoseInfo> poses = scanNrd2Dataset({root.string()}, &problems);
    check(poses.size() == 8 && problems.size() == 2, "8 poses found; the stale Stage B version and the junk file skipped");
    check(poses.size() == 8 && poses[0].scene == "SceneA" && poses[4].heldOut && poses[5].scene == "SceneB" &&
              poses[0].tilesX == 9 && poses[0].frames == 1,
          "sorted by scene and index, header facts read");
    Nrd2PoseInfo info;
    check(peekNrd2Pose((root / "SceneA" / poseName(1)).string(), info) && info.poseIndex == 1, "peek reads a header");
    const u64 id = nrd2DatasetId(poses);
    check(id == nrd2DatasetId(scanNrd2Dataset({root.string()})), "the dataset id is stable across scans");
    const u32 promoted = nrd2EnsureHeldOut(poses);
    check(promoted == 1 && poses[7].heldOut && !poses[5].heldOut, "a scene with none flagged holds out one of its poses");
    check(nrd2DatasetId(poses) != id, "the held-out split is part of the dataset id");
    {
        // A scene whose FIRST pose is held out (the real captures' layout) must not stall the walk.
        std::vector<Nrd2PoseInfo> early(6);
        for (u32 i = 0; i < 6; ++i) { early[i].scene = i < 3 ? "A" : "B"; early[i].heldOut = i == 0 || i == 3; }
        check(nrd2EnsureHeldOut(early) == 0 && early[0].heldOut && !early[1].heldOut && early[3].heldOut,
              "a scene that already holds poses out keeps them, wherever they sit");
    }
    writeNrd2Pose((root / "SceneB" / poseName(1)).string(), makePose("SceneB", 1, false, 9, 8, 1, 99));
    check(nrd2DatasetId(scanNrd2Dataset({root.string()})) != id, "changed content changes the id");
    fs::remove_all(root, ec);
}

void testSidecarAndSchedule() {
    AVER_INFO("-- sidecar, learning rate, gate");
    const std::string path = (std::filesystem::temp_directory_path() / "aver_nrd2_sidecar.steps").string();
    Nrd2Sidecar s;
    s.lifetimeSteps = 12345; s.valRatio = 0.625f; s.datasetId = 0xFEDCBA9876543210ull; s.bestRatio = 0.5f; s.evalsSinceBest = 3;
    Nrd2Sidecar r;
    check(writeNrd2Sidecar(path, s) && readNrd2Sidecar(path, r) && r.lifetimeSteps == 12345 && r.valRatio == 0.625f &&
              r.datasetId == s.datasetId && r.bestRatio == 0.5f && r.evalsSinceBest == 3,
          "the sidecar round-trips");
    { std::ofstream f(path, std::ios::trunc); f << "400\n"; }
    check(readNrd2Sidecar(path, r) && r.lifetimeSteps == 400 && r.valRatio < 0.0f, "a NeuraFI-style steps-only sidecar reads");
    std::filesystem::remove(path);
    check(nrd2LastPath("C:/x/nrd2_v1.avnn") == "C:/x/nrd2_v1.last.avnn", "the .last checkpoint sits beside the weights");
    check(nrd2LearningRate(0) == 5e-4f && std::fabs(nrd2LearningRate(1000) - 2.5e-4f) < 1e-9f &&
              nrd2LearningRate(1000000) == 2e-5f,
          "learning rate 5e-4 / (1 + steps / 1000), floored at 2e-5");
    check(nrd2GateOpen(0.8f, false) && !nrd2GateOpen(0.85f, false) && nrd2GateOpen(0.85f, true) &&
              !nrd2GateOpen(0.91f, true) && !nrd2GateOpen(-1.0f, true),
          "gate opens at <= 0.8, holds between, closes above 0.9 or unmeasured");
}

}  // namespace

int main() {
    testNetwork();
    testStandardisation();
    testExtract();
    testSampling();
    testValidation();
    testDataset();
    testSidecarAndSchedule();
    if (g_failures == 0) AVER_INFO("=== all NRD2 trainer tests passed ===");
    else                 AVER_ERROR("=== {} FAILED ===", g_failures);
    return g_failures == 0 ? 0 : 1;
}
