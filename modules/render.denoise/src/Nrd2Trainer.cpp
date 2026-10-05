#include "aver/render/denoise/Nrd2Trainer.hpp"

#include "aver/core/Log.hpp"
#include "aver/render/denoise/Nrd2.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>

// Design and contract: Nrd2Trainer.hpp; GPU side nrd2_net.hlsl (CSNrd2Gather), network ConvNet.

namespace aver::render::denoise {

namespace fs = std::filesystem;
using neural::ConvIoAffine;
using neural::ConvLayerDesc;
using neural::ConvLossWeight;
using neural::ConvNetDesc;
using neural::TensorShape;

namespace {

constexpr u32 kCh = 12;
constexpr u32 kConstantSlot = 3;
constexpr u32 kNoPose = ~0u;
constexpr f32 kEvalUnit = 0.35f;        // a validation batch (forward only) in train-step units
constexpr u32 kTrainLossEvery = 50;     // steps between training-loss readbacks
constexpr u32 kGatherGroups = (kNrd2PatchTexels * kNrd2PatchTexels + 63u) / 64u;
constexpr const char* kSpan = "NRD2 training";

constexpr rhi::ResourceState kCommon = rhi::ResourceState::Common;
constexpr rhi::ResourceState kRead   = rhi::ResourceState::NonPixelShaderResource;
constexpr rhi::ResourceState kWrite  = rhi::ResourceState::UnorderedAccess;

// nrd2_net.hlsl's Nrd2GatherCB, byte for byte.
struct GatherCB {
    u32 geo[4];
    u32 base[4];
    i32 origin[4];
    f32 inScale[12], inBias[12], tScale[12], tBias[12];
};
static_assert(sizeof(GatherCB) == 240, "Nrd2GatherCB: three int4s, twelve float4s");

bool finite(f32 v) { return std::isfinite(v); }

// fp16 -> fp32 for every bit pattern (nrd2F16ToF32), built once.
const f32* halfTable() {
    static const std::vector<f32> table = [] {
        std::vector<f32> t(65536);
        for (u32 i = 0; i < 65536; ++i) t[i] = nrd2F16ToF32(static_cast<u16>(i));
        return t;
    }();
    return table.data();
}

u64 mix64(u64 x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

ConvLayerDesc layer(u32 cin, u32 cout, u32 k, u32 s, neural::Activation a) {
    ConvLayerDesc l;
    l.cin = cin; l.cout = cout; l.kernel = k; l.stride = s; l.act = a; l.bias = true;
    return l;
}

bool sameShape(const ConvNetDesc& a, const ConvNetDesc& b) {
    if (a.inChannels != b.inChannels || a.layers.size() != b.layers.size()) return false;
    for (usize l = 0; l < a.layers.size(); ++l) {
        const ConvLayerDesc &x = a.layers[l], &y = b.layers[l];
        if (x.cin != y.cin || x.cout != y.cout || x.kernel != y.kernel || x.stride != y.stride || x.act != y.act ||
            x.bias != y.bias)
            return false;
    }
    return true;
}

// Slot words for a pose with `frames` feature frames: fp16 pairs, then theta and weights as f32 bits.
u64 slotWords(const Nrd2PoseInfo& p, u32 frames) {
    const u64 halves = static_cast<u64>(frames) * kCh * p.halfW * p.halfH;
    return (halves + 1) / 2 + 14ull * p.tilesX * p.tilesY;
}

bool writeText(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << text;
    return static_cast<bool>(f);
}

}  // namespace

// ---------------------------------------------------------------- the network and its records

ConvNetDesc nrd2NetworkDesc() {
    using neural::Activation;
    ConvNetDesc d;
    d.inChannels = kCh;
    d.layers = {layer(12, 16, 3, 2, Activation::ReLU), layer(16, 16, 3, 1, Activation::ReLU),
                layer(16, 32, 3, 2, Activation::ReLU), layer(32, 32, 3, 1, Activation::ReLU),
                layer(32, 12, 1, 1, Activation::None)};
    d.seed = 0x4E524432u;   // "NRD2"
    return d;
}

void nrd2DefaultParams(f32 out[12]) {
    const Nrd2Params p{};
    std::memcpy(out, p.diffuse, sizeof(p.diffuse));
    std::memcpy(out + 6, p.specular, sizeof(p.specular));
}

ConvIoAffine nrd2IoAffine(const Nrd2Standardisation& s) {
    ConvIoAffine io;
    io.inScale.resize(kCh); io.inBias.resize(kCh); io.outScale.resize(kCh); io.outBias.resize(kCh);
    for (u32 c = 0; c < kCh; ++c) {
        io.inScale[c] = 1.0f / s.inStd[c];
        io.inBias[c] = -s.inMean[c] / s.inStd[c];
        io.outScale[c] = s.outStd[c];
        io.outBias[c] = s.outMean[c];
    }
    return io;
}

bool nrd2StandardisationFromAffine(const ConvIoAffine& io, Nrd2Standardisation& s) {
    if (io.inScale.size() != kCh || io.inBias.size() != kCh || io.outScale.size() != kCh || io.outBias.size() != kCh)
        return false;
    Nrd2Standardisation t;
    for (u32 c = 0; c < kCh; ++c) {
        if (!(io.inScale[c] > 0.0f) || !finite(io.inScale[c]) || !finite(io.inBias[c]) || !(io.outScale[c] > 0.0f) ||
            !finite(io.outScale[c]) || !finite(io.outBias[c]))
            return false;
        t.inStd[c] = 1.0f / io.inScale[c];
        t.inMean[c] = -io.inBias[c] * t.inStd[c];
        t.outStd[c] = io.outScale[c];
        t.outMean[c] = io.outBias[c];
    }
    s = t;
    return true;
}

void nrd2TargetAffine(const Nrd2Standardisation& s, f32 scale[12], f32 bias[12]) {
    for (u32 p = 0; p < kCh; ++p) {
        scale[p] = 1.0f / s.outStd[p];
        bias[p] = -s.outMean[p] / s.outStd[p];
    }
}

void Nrd2StatsAccumulator::addPose(const Nrd2Pose& p) {
    const f32* h2f = halfTable();
    const usize plane = static_cast<usize>(p.halfW) * p.halfH;
    for (u32 k = 0; k < p.frames; ++k)
        for (u32 c = 0; c < kCh; ++c) {
            const u16* src = &p.features[(static_cast<usize>(k) * kCh + c) * plane];
            f64 s1 = 0.0, s2 = 0.0, n = 0.0;
            for (usize i = 0; i < plane; ++i) {
                const f32 v = h2f[src[i]];
                if (!finite(v)) continue;
                s1 += v;
                s2 += static_cast<f64>(v) * v;
                n += 1.0;
            }
            in1_[c] += s1; in2_[c] += s2; inN_[c] += n;
        }
    const usize tiles = static_cast<usize>(p.tilesX) * p.tilesY;
    for (usize t = 0; t < tiles; ++t)
        for (u32 q = 0; q < kCh; ++q) {
            const f32 th = p.theta[q * tiles + t];
            const f32 w = p.weights[(q / 6u) * tiles + t];
            if (!finite(th) || !finite(w) || !(w > 0.0f)) continue;
            out1_[q] += static_cast<f64>(w) * th;
            out2_[q] += static_cast<f64>(w) * th * th;
            outW_[q] += w;
        }
    ++poses_;
}

bool Nrd2StatsAccumulator::finish(Nrd2Standardisation& out) const {
    if (poses_ == 0) return false;
    f32 def[12];
    nrd2DefaultParams(def);
    for (u32 c = 0; c < kCh; ++c) {
        const f64 n = inN_[c];
        const f64 m = n > 0.0 ? in1_[c] / n : 0.0;
        const f64 var = n > 0.0 ? std::max(in2_[c] / n - m * m, 0.0) : 1.0;
        out.inMean[c] = static_cast<f32>(m);
        out.inStd[c] = std::max(static_cast<f32>(std::sqrt(var)), 1e-3f);
        const f64 w = outW_[c];
        if (w > 0.0) {
            const f64 om = out1_[c] / w;
            out.outMean[c] = static_cast<f32>(om);
            out.outStd[c] = std::max(static_cast<f32>(std::sqrt(std::max(out2_[c] / w - om * om, 0.0))), 1e-2f);
        } else {
            out.outMean[c] = def[c];
            out.outStd[c] = 1.0f;
        }
    }
    return true;
}

void nrd2ExtractPatch(const Nrd2Pose& pose, const ConvIoAffine& io, const f32 tScale[12], const f32 tBias[12],
                      const Nrd2PatchRef& r, f32* input, f32* target, f32* weight) {
    const f32* h2f = halfTable();
    constexpr u32 P = kNrd2PatchTexels, T = kNrd2PatchTiles;
    if (input) {
        const usize plane = static_cast<usize>(pose.halfW) * pose.halfH;
        const usize base = static_cast<usize>(r.frame) * kCh * plane;
        for (u32 c = 0; c < kCh; ++c)
            for (u32 y = 0; y < P; ++y)
                for (u32 x = 0; x < P; ++x) {
                    const i32 hx = r.tx0 * 4 + static_cast<i32>(x), hy = r.ty0 * 4 + static_cast<i32>(y);
                    const bool inside = hx >= 0 && hy >= 0 && hx < static_cast<i32>(pose.halfW) &&
                                        hy < static_cast<i32>(pose.halfH) && r.frame < pose.frames;
                    f32 v = 0.0f;
                    if (inside) {
                        const f32 f = h2f[pose.features[base + c * plane + static_cast<usize>(hy) * pose.halfW + hx]];
                        v = finite(f) ? f * io.inScale[c] + io.inBias[c] : 0.0f;
                    }
                    input[(static_cast<usize>(c) * P + y) * P + x] = v;
                }
    }
    if (!target && !weight) return;
    const usize tiles = static_cast<usize>(pose.tilesX) * pose.tilesY;
    for (u32 ty = 0; ty < T; ++ty)
        for (u32 tx = 0; tx < T; ++tx) {
            const i32 gx = r.tx0 + static_cast<i32>(tx), gy = r.ty0 + static_cast<i32>(ty);
            const bool inside = gx >= 0 && gy >= 0 && gx < static_cast<i32>(pose.tilesX) && gy < static_cast<i32>(pose.tilesY);
            const bool core = tx >= kNrd2CoreOffset && tx < kNrd2CoreOffset + kNrd2CoreTiles && ty >= kNrd2CoreOffset &&
                              ty < kNrd2CoreOffset + kNrd2CoreTiles;
            const usize tile = inside ? static_cast<usize>(gy) * pose.tilesX + gx : 0;
            for (u32 p = 0; p < kCh; ++p) {
                f32 t = 0.0f, w = 0.0f;
                if (inside) {
                    const f32 th = pose.theta[p * tiles + tile];
                    if (finite(th)) {
                        t = th * tScale[p] + tBias[p];
                        const f32 tw = pose.weights[(p / 6u) * tiles + tile];
                        w = (core && finite(tw) && tw > 0.0f) ? tw : 0.0f;
                    }
                }
                const usize o = (static_cast<usize>(p) * T + ty) * T + tx;
                if (target) target[o] = t;
                if (weight) weight[o] = w;
            }
        }
}

// ---------------------------------------------------------------- dataset files

bool peekNrd2Pose(const std::string& path, Nrd2PoseInfo& info, std::string* why) {
    auto fail = [&](const char* w) { if (why) *why = w; return false; };
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("cannot open the file");
    u32 h[12] = {};
    if (!f.read(reinterpret_cast<char*>(h), sizeof(h))) return fail("truncated header");
    if (h[0] != kNrd2PoseMagic) return fail("not an N2PS file");
    if (h[1] != kNrd2PoseVersion) return fail("unsupported version");
    Nrd2PoseInfo p;
    p.path = path;
    p.stageBVersion = h[2]; p.poseIndex = h[3]; p.heldOut = h[4] != 0;
    p.halfW = h[5]; p.halfH = h[6]; p.tilesX = h[7]; p.tilesY = h[8]; p.frames = h[9];
    const u32 channels = h[10], sceneBytes = h[11];
    if (h[4] > 1) return fail("bad held-out flag");
    if (sceneBytes > 1024) return fail("bad scene id");
    if (!p.tilesX || !p.tilesY || p.halfW != 4 * p.tilesX || p.halfH != 4 * p.tilesY || !p.frames || p.frames > 64 ||
        channels != kNrd2FeatureCount)
        return fail("inconsistent shape");
    p.scene.resize(sceneBytes);
    if (sceneBytes && !f.read(p.scene.data(), sceneBytes)) return fail("truncated scene id");
    const u64 tiles = static_cast<u64>(p.tilesX) * p.tilesY;
    const u64 want = 48 + ((sceneBytes + 3ull) & ~3ull) + static_cast<u64>(p.frames) * channels * p.halfW * p.halfH * 2 +
                     tiles * (12 + 2 + kNrd2PoseLosses) * 4 + 4;
    std::error_code ec;
    const u64 size = fs::file_size(path, ec);
    if (ec || size != want) return fail("size disagrees with the shape");
    f.seekg(static_cast<std::streamoff>(size - 4));
    if (!f.read(reinterpret_cast<char*>(&p.crc), 4)) return fail("cannot read the CRC");
    info = std::move(p);
    return true;
}

std::vector<Nrd2PoseInfo> scanNrd2Dataset(const std::vector<std::string>& dirs, std::vector<std::string>* problems) {
    std::vector<std::string> files;
    auto isPose = [](const fs::path& p) {
        const std::string n = p.filename().string();
        return n.size() > 9 && n.rfind("pose_", 0) == 0 && p.extension() == ".n2p";
    };
    for (const std::string& d : dirs) {
        std::error_code ec;
        if (!fs::is_directory(d, ec)) {
            if (problems) problems->push_back(d + ": not a folder");
            continue;
        }
        for (const fs::directory_entry& e : fs::directory_iterator(d, ec)) {
            if (e.is_regular_file(ec) && isPose(e.path())) files.push_back(e.path().string());
            else if (e.is_directory(ec))
                for (const fs::directory_entry& s : fs::directory_iterator(e.path(), ec))
                    if (s.is_regular_file(ec) && isPose(s.path())) files.push_back(s.path().string());
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    std::vector<Nrd2PoseInfo> out;
    for (const std::string& f : files) {
        Nrd2PoseInfo info;
        std::string why;
        if (!peekNrd2Pose(f, info, &why)) {
            if (problems) problems->push_back(f + ": " + why);
            continue;
        }
        if (info.stageBVersion != kNrd2StageBVersion) {
            if (problems)
                problems->push_back(f + ": captured with Stage B version " + std::to_string(info.stageBVersion) +
                                    " (this build writes " + std::to_string(kNrd2StageBVersion) + ")");
            continue;
        }
        out.push_back(std::move(info));
    }
    std::stable_sort(out.begin(), out.end(), [](const Nrd2PoseInfo& a, const Nrd2PoseInfo& b) {
        if (a.scene != b.scene) return a.scene < b.scene;
        if (a.poseIndex != b.poseIndex) return a.poseIndex < b.poseIndex;
        return a.path < b.path;
    });
    return out;
}

u64 nrd2DatasetId(const std::vector<Nrd2PoseInfo>& poses) {
    u64 h = 0xCBF29CE484222325ull;
    auto add = [&](const void* data, usize n) {
        const u8* b = static_cast<const u8*>(data);
        for (usize i = 0; i < n; ++i) { h ^= b[i]; h *= 0x100000001B3ull; }
    };
    for (const Nrd2PoseInfo& p : poses) {
        add(p.scene.data(), p.scene.size());
        const u8 zero = 0, held = p.heldOut ? 1 : 0;
        add(&zero, 1);
        add(&p.poseIndex, 4);
        add(&held, 1);
        add(&p.crc, 4);
    }
    return h;
}

u32 nrd2EnsureHeldOut(std::vector<Nrd2PoseInfo>& poses) {
    u32 promoted = 0;
    for (usize i = 0; i < poses.size();) {
        usize j = i;
        bool any = false;
        for (; j < poses.size() && poses[j].scene == poses[i].scene; ++j) any = any || poses[j].heldOut;
        const usize n = j - i;
        if (!any && n >= 2) {
            for (usize k = 0; k < n; ++k)
                if (k % 4 == 3 || (n < 4 && k == n - 1)) { poses[i + k].heldOut = true; ++promoted; }
        }
        i = j;
    }
    return promoted;
}

void nrd2SampleBatch(u64 seed, u64 counter, const std::vector<std::vector<u32>>& residentByScene,
                     const std::vector<Nrd2PoseInfo>& poses, std::span<Nrd2PatchRef> out) {
    std::vector<u32> live;
    for (u32 s = 0; s < residentByScene.size(); ++s) if (!residentByScene[s].empty()) live.push_back(s);
    for (usize i = 0; i < out.size(); ++i) {
        out[i] = Nrd2PatchRef{};
        if (live.empty()) continue;
        const u64 k = counter * out.size() + i;
        const std::vector<u32>& list = residentByScene[live[k % live.size()]];
        const u64 h0 = mix64(seed * 0xD1B54A32D192ED03ull ^ mix64(k));
        const u64 h1 = mix64(h0), h2 = mix64(h1);
        Nrd2PatchRef r;
        r.pose = list[h0 % list.size()];
        const Nrd2PoseInfo& p = poses[r.pose];
        const u64 spanX = p.tilesX >= kNrd2CoreTiles ? p.tilesX - kNrd2CoreTiles + 1 : 1;
        const u64 spanY = p.tilesY >= kNrd2CoreTiles ? p.tilesY - kNrd2CoreTiles + 1 : 1;
        r.tx0 = static_cast<i32>(h1 % spanX) - static_cast<i32>(kNrd2CoreOffset);
        r.ty0 = static_cast<i32>((h1 >> 32) % spanY) - static_cast<i32>(kNrd2CoreOffset);
        r.frame = static_cast<u32>(h2 % std::max(p.frames, 1u));
        out[i] = r;
    }
}

std::vector<Nrd2PatchRef> nrd2ValidationPatches(u32 pose, u32 tilesX, u32 tilesY, u32 frame, u32 cap) {
    const u32 nx = (tilesX + kNrd2CoreTiles - 1) / kNrd2CoreTiles, ny = (tilesY + kNrd2CoreTiles - 1) / kNrd2CoreTiles;
    std::vector<Nrd2PatchRef> all;
    all.reserve(static_cast<usize>(nx) * ny);
    for (u32 iy = 0; iy < ny; ++iy)
        for (u32 ix = 0; ix < nx; ++ix)
            all.push_back({pose, frame, static_cast<i32>(ix * kNrd2CoreTiles) - static_cast<i32>(kNrd2CoreOffset),
                           static_cast<i32>(iy * kNrd2CoreTiles) - static_cast<i32>(kNrd2CoreOffset)});
    if (cap == 0 || all.size() <= cap) return all;
    std::vector<Nrd2PatchRef> some;
    for (u32 k = 0; k < cap; ++k) some.push_back(all[static_cast<usize>(k) * all.size() / cap]);
    return some;
}

f64 nrd2DefaultLoss(const f32 defStd[12], const f32* target, const f32* weight) {
    constexpr usize plane = kNrd2PatchTiles * kNrd2PatchTiles;
    f64 s = 0.0;
    for (u32 p = 0; p < kCh; ++p)
        for (usize i = 0; i < plane; ++i) {
            const f64 e = static_cast<f64>(defStd[p]) - target[p * plane + i];
            s += static_cast<f64>(weight[p * plane + i]) * e * e;
        }
    return s;
}

f32 nrd2LearningRate(u64 lifetimeSteps) {
    const f32 lr = 5e-4f / (1.0f + static_cast<f32>(lifetimeSteps) / 1000.0f);
    return std::max(lr, 2e-5f);
}

bool nrd2GateOpen(f32 ratio, bool wasOpen) {
    if (!(ratio >= 0.0f) || !finite(ratio)) return false;
    if (ratio <= kNrd2GateOpen) return true;
    if (ratio > kNrd2GateClose) return false;
    return wasOpen;
}

bool writeNrd2Sidecar(const std::string& path, const Nrd2Sidecar& s) {
    char line[160];
    std::snprintf(line, sizeof line, "%llu %.6f %016llx %.6f %u\n", static_cast<unsigned long long>(s.lifetimeSteps),
                  static_cast<double>(s.valRatio), static_cast<unsigned long long>(s.datasetId),
                  static_cast<double>(s.bestRatio), s.evalsSinceBest);
    return writeText(path, line);
}

bool readNrd2Sidecar(const std::string& path, Nrd2Sidecar& s) {
    std::ifstream f(path);
    if (!f) return false;
    Nrd2Sidecar r;
    unsigned long long steps = 0, id = 0;
    if (!(f >> steps)) return false;
    r.lifetimeSteps = steps;
    if (f >> r.valRatio) {
        if (f >> std::hex >> id) {
            r.datasetId = id;
            f >> std::dec;
            if (f >> r.bestRatio) f >> r.evalsSinceBest;
        }
    }
    if (!finite(r.valRatio)) r.valRatio = -1.0f;
    if (!finite(r.bestRatio)) r.bestRatio = -1.0f;
    s = r;
    return true;
}

std::string nrd2LastPath(const std::string& weightsPath) {
    const std::string ext = ".avnn";
    if (weightsPath.size() > ext.size() && weightsPath.compare(weightsPath.size() - ext.size(), ext.size(), ext) == 0)
        return weightsPath.substr(0, weightsPath.size() - ext.size()) + ".last.avnn";
    return weightsPath + ".last";
}

// ---------------------------------------------------------------- the GPU session

struct Nrd2Trainer::Slot {
    rhi::BufferHandle buf = 0;
    rhi::BindingSetHandle set = 0;
    u64 words = 0;
    u32 pose = kNoPose;
    u32 halfW = 0, halfH = 0, tilesX = 0, tilesY = 0, frames = 0, thetaBase = 0, weightBase = 0;
};

// A pose read by the worker, laid out as a slot; validation packs carry their records and default loss.
struct Nrd2Trainer::Packed {
    u32 pose = kNoPose;
    bool validation = false;
    std::string error;   // non-empty: the read failed, data is empty
    u32 halfW = 0, halfH = 0, tilesX = 0, tilesY = 0, frames = 0, thetaBase = 0, weightBase = 0;
    std::vector<u32> data;
    std::vector<Nrd2PatchRef> refs;
    f64 defaultLoss = 0.0, weightSum = 0.0;
};

// A training-loss readback in flight.
struct Nrd2Trainer::Pending {
    rhi::BufferHandle rb = 0;
    u64 due = 0;
    u32 n = 0;
    u64 step = 0;
    bool busy = false;
};

Nrd2Trainer::Nrd2Trainer(rhi::IDevice& dev) : dev_(dev), res_(dev.resources()) {}

Nrd2Trainer::~Nrd2Trainer() {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stopWorker_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    releaseGpu();
}

bool Nrd2Trainer::active() const {
    std::lock_guard<std::mutex> lk(mutex_);
    const auto p = status_.phase;
    return p == Nrd2TrainStatus::Phase::Loading || p == Nrd2TrainStatus::Phase::Training ||
           p == Nrd2TrainStatus::Phase::Validating || p == Nrd2TrainStatus::Phase::Stopping;
}

Nrd2TrainStatus Nrd2Trainer::status() const {
    std::lock_guard<std::mutex> lk(mutex_);
    return status_;
}

void Nrd2Trainer::setMessage(std::string m) {
    std::lock_guard<std::mutex> lk(mutex_);
    status_.message = std::move(m);
}

bool Nrd2Trainer::start(const Nrd2TrainConfig& cfg) {
    if (active()) return false;
    if (!res_) { AVER_WARN("[NRD2] training needs a GPU resource factory"); return false; }
    if (cfg.dirs.empty() || cfg.weightsPath.empty()) {
        AVER_WARN("[NRD2] training needs a dataset folder and a weights path");
        return false;
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stopWorker_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
    releaseGpu();

    cfg_ = cfg;
    cfg_.batch = std::clamp(cfg_.batch, 1u, 64u);
    cfg_.validateEvery = std::max(cfg_.validateEvery, 1u);
    cfg_.saveEvery = std::max(cfg_.saveEvery, cfg_.validateEvery);
    cfg_.swapEvery = std::max(cfg_.swapEvery, 1u);
    cfg_.maxStepsPerFrame = std::max(cfg_.maxStepsPerFrame, 1u);
    cfg_.patience = std::max(cfg_.patience, 1u);

    poses_.clear(); scenes_.clear(); sceneOf_.clear(); train_.clear(); held_.clear();
    resumeWeights_.clear();
    resume_ = {};
    datasetId_ = 0;
    trainSlots_ = 0;
    frame_ = 0; rotation_ = 0; fifo_ = 0; valUploaded_ = 0; sampleCounter_ = 0;
    mode_ = Mode::Train;
    valNext_ = valTotal_ = 0;
    valWork_.clear(); valRefs_.clear(); valDefault_.clear(); valWeight_.clear();
    valDue_ = weightsDue_ = 0;
    cancel_ = stopping_ = false;
    lastWeights_.clear();
    lastWeightsStep_ = lastSavedStep_ = 0;
    sessionSteps_ = 0;
    lastValidated_ = lastSwapped_ = ~0u;
    trainFilled_ = 0;
    msPerUnit_ = lastTimingSum_ = unitsRecorded_ = lastUnits_ = 0.0;
    lastTimingFrames_ = 0;
    timingOk_ = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        stopWorker_ = false;
        prepared_ = prepareFailed_ = false;
        requests_.clear();
        loaded_.clear();
        status_ = Nrd2TrainStatus{};
        status_.phase = Nrd2TrainStatus::Phase::Loading;
        status_.targetSteps = cfg_.steps;
        status_.message = "Scanning the dataset";
    }
    std::string dirs;
    for (const std::string& d : cfg_.dirs) dirs += (dirs.empty() ? "" : ", ") + d;
    AVER_INFO("[NRD2] training: {} steps this session, batch {}, dataset {}; weights {}", cfg_.steps, cfg_.batch, dirs,
              cfg_.weightsPath);
    thread_ = std::thread([this] { worker(); });
    return true;
}

void Nrd2Trainer::cancel() {
    std::lock_guard<std::mutex> lk(mutex_);
    cancel_ = true;
}

// ---- worker thread

std::unique_ptr<Nrd2Trainer::Packed> Nrd2Trainer::pack(u32 pose, bool validation, std::string& why) {
    auto p = std::make_unique<Packed>();
    p->pose = pose;
    p->validation = validation;
    Nrd2Pose data;
    if (!readNrd2Pose(poses_[pose].path, data, &why)) {
        p->error = why;
        return p;
    }
    const Nrd2PoseInfo& info = poses_[pose];
    if (data.tilesX != info.tilesX || data.tilesY != info.tilesY || data.frames != info.frames) {
        p->error = why = "changed on disk since the scan";
        return p;
    }
    const u32 frames = validation ? 1u : data.frames;
    const usize halves = static_cast<usize>(frames) * kCh * data.halfW * data.halfH;
    const usize tiles = static_cast<usize>(data.tilesX) * data.tilesY;
    p->halfW = data.halfW; p->halfH = data.halfH; p->tilesX = data.tilesX; p->tilesY = data.tilesY;
    p->frames = frames;
    p->thetaBase = static_cast<u32>((halves + 1) / 2);
    p->weightBase = p->thetaBase + static_cast<u32>(12 * tiles);
    p->data.assign((halves + 1) / 2 + 14 * tiles, 0u);
    for (usize i = 0; i < halves; ++i) p->data[i >> 1] |= static_cast<u32>(data.features[i]) << ((i & 1) * 16);
    std::memcpy(&p->data[p->thetaBase], data.theta.data(), 12 * tiles * sizeof(f32));
    std::memcpy(&p->data[p->weightBase], data.weights.data(), 2 * tiles * sizeof(f32));
    if (validation) {
        p->refs = nrd2ValidationPatches(pose, data.tilesX, data.tilesY, 0, cfg_.valPatchCap);
        std::vector<f32> tgt(kCh * kNrd2PatchTiles * kNrd2PatchTiles), w(tgt.size());
        for (const Nrd2PatchRef& r : p->refs) {
            nrd2ExtractPatch(data, io_, tScale_, tBias_, r, nullptr, tgt.data(), w.data());
            p->defaultLoss += nrd2DefaultLoss(defStd_, tgt.data(), w.data());
            for (f32 v : w) p->weightSum += v;
        }
    }
    return p;
}

bool Nrd2Trainer::prepare(std::string& why) {
    std::vector<std::string> problems;
    poses_ = scanNrd2Dataset(cfg_.dirs, &problems);
    for (const std::string& p : problems) AVER_WARN("[NRD2] dataset: skipped {}", p);
    if (poses_.empty()) { why = "no usable pose files in the dataset folders"; return false; }
    if (const u32 promoted = nrd2EnsureHeldOut(poses_))
        AVER_INFO("[NRD2] {} pose(s) held out for validation (their scenes had none flagged)", promoted);
    datasetId_ = nrd2DatasetId(poses_);

    sceneOf_.resize(poses_.size());
    std::vector<std::vector<u32>> trainByScene;
    for (u32 i = 0; i < poses_.size(); ++i) {
        if (scenes_.empty() || scenes_.back() != poses_[i].scene) {
            scenes_.push_back(poses_[i].scene);
            trainByScene.emplace_back();
        }
        sceneOf_[i] = static_cast<u32>(scenes_.size() - 1);
        if (poses_[i].heldOut) held_.push_back(i);
        else trainByScene.back().push_back(i);
    }
    // Rotation order: scenes take turns, so any resident window is stratified.
    for (usize k = 0;; ++k) {
        bool any = false;
        for (const std::vector<u32>& s : trainByScene)
            if (k < s.size()) { train_.push_back(s[k]); any = true; }
        if (!any) break;
    }
    if (train_.empty()) { why = "every pose is held out; nothing to train on"; return false; }
    if (held_.empty()) { why = "no held-out poses to validate on"; return false; }

    // Resume from the last checkpoint when it was trained on this exact dataset.
    bool resumed = false;
    const std::string last = nrd2LastPath(cfg_.weightsPath);
    std::error_code ec;
    if (cfg_.resume && fs::exists(last, ec)) {
        Nrd2Sidecar sc;
        if (readNrd2Sidecar(last + ".steps", sc) && sc.datasetId == datasetId_) {
            ConvNetDesc d = nrd2NetworkDesc();
            std::vector<f32> w;
            ConvIoAffine io;
            if (neural::loadConvWeightFile(last, d, w, &io) && sameShape(d, nrd2NetworkDesc()) &&
                nrd2StandardisationFromAffine(io, std_)) {
                resumeWeights_ = std::move(w);
                resume_ = sc;
                io_ = std::move(io);
                resumed = true;
                AVER_INFO("[NRD2] resuming from {} ({} lifetime steps, best held-out ratio {:.3f})", last,
                          sc.lifetimeSteps, static_cast<double>(sc.bestRatio));
            } else {
                AVER_WARN("[NRD2] {} would not load as an NRD2 checkpoint; training from scratch", last);
            }
        } else {
            AVER_INFO("[NRD2] {} belongs to another dataset; training from scratch", last);
        }
    }
    if (!resumed) {
        Nrd2StatsAccumulator acc;
        for (usize i = 0; i < train_.size(); ++i) {
            {
                std::lock_guard<std::mutex> lk(mutex_);
                if (stopWorker_) { why = "stopped"; return false; }
                status_.message = "Measuring the dataset (" + std::to_string(i + 1) + "/" + std::to_string(train_.size()) + ")";
            }
            Nrd2Pose p;
            std::string w;
            if (readNrd2Pose(poses_[train_[i]].path, p, &w)) acc.addPose(p);
            else AVER_WARN("[NRD2] {}: {}; left out of the statistics", poses_[train_[i]].path, w);
        }
        if (!acc.finish(std_)) { why = "no training pose could be read"; return false; }
        io_ = nrd2IoAffine(std_);
    }
    nrd2TargetAffine(std_, tScale_, tBias_);
    f32 def[12];
    nrd2DefaultParams(def);
    for (u32 p = 0; p < kCh; ++p) defStd_[p] = def[p] * tScale_[p] + tBias_[p];

    // Slots: validation (one frame each) within a third of the budget, training in the rest.
    trainSlotBytes_ = valSlotBytes_ = 0;
    for (u32 i : train_) trainSlotBytes_ = std::max(trainSlotBytes_, slotWords(poses_[i], poses_[i].frames) * 4);
    for (u32 i : held_) valSlotBytes_ = std::max(valSlotBytes_, slotWords(poses_[i], 1) * 4);
    const u64 maxVal = std::max<u64>(1, (cfg_.vramBudget / 3) / std::max<u64>(valSlotBytes_, 1));
    if (held_.size() > maxVal) {
        std::vector<std::vector<u32>> byScene(scenes_.size());
        for (u32 i : held_) byScene[sceneOf_[i]].push_back(i);
        std::vector<u32> keep;
        for (usize k = 0; keep.size() < maxVal; ++k) {
            bool any = false;
            for (const std::vector<u32>& s : byScene)
                if (k < s.size() && keep.size() < maxVal) { keep.push_back(s[k]); any = true; }
            if (!any) break;
        }
        AVER_INFO("[NRD2] validating on {} of {} held-out poses (the rest would not fit the {} MiB budget)", keep.size(),
                  held_.size(), cfg_.vramBudget >> 20);
        std::sort(keep.begin(), keep.end());
        held_ = std::move(keep);
    }
    const u64 valBytes = held_.size() * valSlotBytes_;
    const u64 left = cfg_.vramBudget > valBytes ? cfg_.vramBudget - valBytes : 0;
    trainSlots_ = static_cast<u32>(std::clamp<u64>(left / std::max<u64>(trainSlotBytes_, 1), 1, train_.size()));
    {
        std::lock_guard<std::mutex> lk(mutex_);
        status_.trainPoses = static_cast<u32>(train_.size());
        status_.heldOutPoses = static_cast<u32>(held_.size());
        status_.lifetimeSteps = resume_.lifetimeSteps;
        status_.bestRatio = resume_.bestRatio;
        status_.sinceBest = resume_.evalsSinceBest;
        status_.message = "Loading poses";
    }
    AVER_INFO("[NRD2] dataset {:016x}: {} scenes, {} training and {} held-out poses; {} of the training poses resident "
              "at a time ({:.1f} MiB each){}",
              datasetId_, scenes_.size(), train_.size(), held_.size(), trainSlots_, static_cast<double>(trainSlotBytes_) / 1048576.0,
              trainSlots_ < train_.size() ? ", streamed" : "");
    return true;
}

void Nrd2Trainer::worker() {
    std::string why;
    const bool ok = prepare(why);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        prepared_ = ok;
        prepareFailed_ = !ok;
        if (!ok) status_.message = why;
    }
    cv_.notify_all();
    if (!ok) return;

    // Validation poses first (in held_ order), then training poses on request. At most two packed poses
    // wait for upload at any time.
    auto waitRoom = [&]() {
        std::unique_lock<std::mutex> lk(mutex_);
        cv_.wait(lk, [&] { return stopWorker_ || loaded_.size() < 2; });
        return !stopWorker_;
    };
    for (u32 i : held_) {
        if (!waitRoom()) return;
        std::string w;
        std::unique_ptr<Packed> p = pack(i, true, w);
        std::lock_guard<std::mutex> lk(mutex_);
        loaded_.push_back(std::move(p));
    }
    for (;;) {
        u32 pose = kNoPose;
        {
            std::unique_lock<std::mutex> lk(mutex_);
            cv_.wait(lk, [&] { return stopWorker_ || (!requests_.empty() && loaded_.size() < 2); });
            if (stopWorker_) return;
            pose = requests_.front();
            requests_.pop_front();
        }
        std::string w;
        std::unique_ptr<Packed> p = pack(pose, false, w);
        std::lock_guard<std::mutex> lk(mutex_);
        loaded_.push_back(std::move(p));
    }
}

void Nrd2Trainer::requestLoad(u32 pose) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        requests_.push_back(pose);
    }
    cv_.notify_all();
}

std::unique_ptr<Nrd2Trainer::Packed> Nrd2Trainer::takeLoaded() {
    std::unique_ptr<Packed> p;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (loaded_.empty()) return p;
        p = std::move(loaded_.front());
        loaded_.pop_front();
    }
    cv_.notify_all();
    return p;
}

// ---- frame thread

bool Nrd2Trainer::setupGpu(std::string& why) {
    neural::OptimiserDesc opt = neural::convDefaults();
    opt.weightEma = 0.995f;
    opt.learningRate = nrd2LearningRate(resume_.lifetimeSteps);
    if (!net_.create(dev_, nrd2NetworkDesc(), opt, neural::ConvMode::Train)) { why = "the network would not build"; return false; }
    net_.setGpuStats(false);   // one "NRD2 training" span instead of one per step
    net_.setIoAffine(io_);
    if (!resumeWeights_.empty() && !net_.uploadWeights(resumeWeights_)) { why = "the checkpoint would not upload"; return false; }
    const TensorShape shapes[1] = {TensorShape{cfg_.batch, kCh, kNrd2PatchTexels, kNrd2PatchTexels}};
    if (!net_.reserve(shapes)) { why = "the network's tensors would not allocate"; return false; }

    const std::string& source = rhi::shaderFile("nrd2_net.hlsl");
    if (source.empty()) { why = "nrd2_net.hlsl is not deployed beside the executable"; return false; }
    rhi::ShaderDesc sd{};
    sd.source = source.c_str();
    sd.entry = "CSNrd2Gather";
    sd.stage = rhi::ShaderStage::Compute;
    sd.minShaderModel = 60;
    sd.defines = "AVER_NRD2_NET_PASS=0";
    const rhi::ShaderHandle cs = res_->createShader(sd);
    if (cs) {
        rhi::ComputePipelineDesc pd{};
        pd.cs = cs;
        pd.layout.srvCount = 1;
        pd.layout.uavCount = 3;
        pd.layout.slotKindsDeclared = true;
        pd.layout.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
        for (u32 i = 0; i < 3; ++i) pd.layout.uavKinds[i] = rhi::SlotKind::StructuredBuffer;
        pd.layout.constantDwords[kConstantSlot] = 0;   // root CBV
        gatherPso_ = res_->createComputePipeline(pd);
        res_->destroyShader(cs);
    }
    if (!gatherPso_) { why = "CSNrd2Gather would not build"; return false; }

    auto buffer = [&](u64 bytes, rhi::BufferKind kind, bool uav, const char* name) {
        rhi::BufferDesc bd{};
        bd.bytes = bytes;
        bd.kind = kind;
        bd.allowUnorderedAccess = uav;
        bd.debugName = name;
        return res_->createBuffer(bd);
    };
    const u64 inFloats = static_cast<u64>(cfg_.batch) * kCh * kNrd2PatchTexels * kNrd2PatchTexels;
    const u64 tFloats = static_cast<u64>(cfg_.batch) * kCh * kNrd2PatchTiles * kNrd2PatchTiles;
    batchIn_ = buffer(inFloats * 4, rhi::BufferKind::Default, true, "NRD2 training inputs");
    batchTgt_ = buffer(tFloats * 4, rhi::BufferKind::Default, true, "NRD2 training targets");
    batchW_ = buffer(tFloats * 4, rhi::BufferKind::Default, true, "NRD2 training weights");
    lossBuf_ = buffer(static_cast<u64>(cfg_.batch) * 4, rhi::BufferKind::Default, true, "NRD2 loss per record");
    if (!batchIn_ || !batchTgt_ || !batchW_ || !lossBuf_) { why = "batch buffers would not allocate"; return false; }
    stagingBytes_ = std::max(trainSlotBytes_, valSlotBytes_);
    for (rhi::BufferHandle& s : staging_) {
        s = buffer(stagingBytes_, rhi::BufferKind::Upload, false, "NRD2 pose staging");
        if (!s) { why = "pose staging buffers would not allocate"; return false; }
    }
    pending_.assign(4, Pending{});
    for (Pending& p : pending_) {
        p.rb = buffer(static_cast<u64>(cfg_.batch) * 4, rhi::BufferKind::Readback, false, "NRD2 training loss readback");
        if (!p.rb) { why = "readback buffers would not allocate"; return false; }
    }

    auto makeSlot = [&](u64 bytes, const char* name, Slot& s) {
        s.buf = buffer(bytes, rhi::BufferKind::Default, false, name);
        if (!s.buf) return false;
        rhi::BindingSetDesc bd{};
        bd.srvCount = 1;
        bd.uavCount = 3;
        bd.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
        for (u32 i = 0; i < 3; ++i) bd.uavKinds[i] = rhi::SlotKind::StructuredBuffer;
        s.set = res_->createBindingSet(bd);
        if (!s.set) return false;
        s.words = bytes / 4;
        res_->setSrvBuffer(s.set, 0, s.buf, 4, static_cast<u32>(s.words), 0);
        res_->setUavBuffer(s.set, 0, batchIn_, 4, static_cast<u32>(inFloats), 0);
        res_->setUavBuffer(s.set, 1, batchTgt_, 4, static_cast<u32>(tFloats), 0);
        res_->setUavBuffer(s.set, 2, batchW_, 4, static_cast<u32>(tFloats), 0);
        return true;
    };
    valSlots_.assign(held_.size(), Slot{});
    for (Slot& s : valSlots_)
        if (!makeSlot(valSlotBytes_, "NRD2 held-out pose", s)) { why = "the held-out pose cache would not allocate"; return false; }
    slots_.clear();
    for (u32 i = 0; i < trainSlots_; ++i) {
        Slot s;
        if (!makeSlot(trainSlotBytes_, "NRD2 training pose", s)) {
            if (s.buf) res_->destroyBuffer(s.buf);
            if (s.set) res_->destroyBindingSet(s.set);
            break;
        }
        slots_.push_back(s);
    }
    if (slots_.empty()) { why = "the training pose cache would not allocate"; return false; }
    if (slots_.size() < trainSlots_) {
        AVER_WARN("[NRD2] only {} of {} training pose slots allocated; streaming more", slots_.size(), trainSlots_);
        trainSlots_ = static_cast<u32>(slots_.size());
    }
    gpuReady_ = true;
    return true;
}

bool Nrd2Trainer::upload(rhi::IRenderContext& ctx, Slot& slot, const Packed& p) {
    const u64 bytes = p.data.size() * 4;
    if (bytes > slot.words * 4 || bytes > stagingBytes_) {
        AVER_WARN("[NRD2] {} is larger than its slot; skipped", poses_[p.pose].path);
        return false;
    }
    const rhi::BufferHandle st = staging_[frame_ % 3];
    if (!res_->writeBuffer(st, p.data.data(), bytes, 0)) return false;
    ctx.bufferBarrier(slot.buf, kCommon, rhi::ResourceState::CopyDest);
    ctx.copyBuffer(slot.buf, st, bytes);
    ctx.bufferBarrier(slot.buf, rhi::ResourceState::CopyDest, kCommon);
    slot.pose = p.pose;
    slot.halfW = p.halfW; slot.halfH = p.halfH; slot.tilesX = p.tilesX; slot.tilesY = p.tilesY;
    slot.frames = p.frames; slot.thetaBase = p.thetaBase; slot.weightBase = p.weightBase;
    return true;
}

void Nrd2Trainer::gather(rhi::IRenderContext& ctx, const Slot& slot, const Nrd2PatchRef& r, u32 record) {
    GatherCB cb{};
    cb.geo[0] = slot.halfW; cb.geo[1] = slot.halfH; cb.geo[2] = slot.tilesX; cb.geo[3] = slot.tilesY;
    cb.base[0] = std::min(r.frame, slot.frames - 1) * kCh * slot.halfW * slot.halfH;
    cb.base[1] = slot.thetaBase;
    cb.base[2] = slot.weightBase;
    cb.base[3] = record;
    cb.origin[0] = r.tx0; cb.origin[1] = r.ty0;
    for (u32 c = 0; c < kCh; ++c) {
        cb.inScale[c] = io_.inScale[c];
        cb.inBias[c] = io_.inBias[c];
        cb.tScale[c] = tScale_[c];
        cb.tBias[c] = tBias_[c];
    }
    ctx.setPipeline(gatherPso_);
    ctx.setBindingSet(slot.set);
    ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
    ctx.dispatch(kGatherGroups, 1, 1);
}

// Gathers `items` into records 0..n-1 of the batch buffers, which rest in Common.
void Nrd2Trainer::gatherBatch(rhi::IRenderContext& ctx, const std::vector<std::pair<const Slot*, Nrd2PatchRef>>& items) {
    std::vector<rhi::BufferHandle> used;
    for (const auto& it : items)
        if (std::find(used.begin(), used.end(), it.first->buf) == used.end()) used.push_back(it.first->buf);
    for (rhi::BufferHandle b : used) ctx.bufferBarrier(b, kCommon, kRead);
    for (rhi::BufferHandle b : {batchIn_, batchTgt_, batchW_}) ctx.bufferBarrier(b, kCommon, kWrite);
    for (usize i = 0; i < items.size(); ++i) gather(ctx, *items[i].first, items[i].second, static_cast<u32>(i));
    for (rhi::BufferHandle b : {batchIn_, batchTgt_, batchW_}) ctx.bufferBarrier(b, kWrite, kCommon);
    for (rhi::BufferHandle b : used) ctx.bufferBarrier(b, kRead, kCommon);
}

bool Nrd2Trainer::trainStep(rhi::IRenderContext& ctx) {
    std::vector<std::vector<u32>> byScene(scenes_.size());
    std::vector<const Slot*> slotOf(poses_.size(), nullptr);
    for (const Slot& s : slots_)
        if (s.pose != kNoPose) {
            byScene[sceneOf_[s.pose]].push_back(s.pose);
            slotOf[s.pose] = &s;
        }
    std::vector<Nrd2PatchRef> refs(cfg_.batch);
    nrd2SampleBatch(cfg_.seed, sampleCounter_++, byScene, poses_, refs);
    std::vector<std::pair<const Slot*, Nrd2PatchRef>> items;
    for (const Nrd2PatchRef& r : refs)
        if (slotOf[r.pose]) items.push_back({slotOf[r.pose], r});
    if (items.empty() || items.size() != refs.size()) return false;   // no resident pose
    gatherBatch(ctx, items);

    const u64 lifetime = resume_.lifetimeSteps + sessionSteps_;
    const f32 lr = nrd2LearningRate(lifetime);
    net_.setLearningRate(lr);
    const TensorShape shape{cfg_.batch, kCh, kNrd2PatchTexels, kNrd2PatchTexels};
    const f32 norm = static_cast<f32>(cfg_.batch * kCh * kNrd2CoreTiles * kNrd2CoreTiles);
    if (sessionSteps_ % kTrainLossEvery == 0) {
        for (Pending& p : pending_) {
            if (p.busy) continue;
            if (net_.recordEvaluate(ctx, batchIn_, batchTgt_, batchW_, lossBuf_, shape, norm, false, {},
                                    ConvLossWeight::PerElement)) {
                ctx.bufferBarrier(lossBuf_, kCommon, rhi::ResourceState::CopySource);
                ctx.copyBuffer(p.rb, lossBuf_, static_cast<u64>(cfg_.batch) * 4);
                ctx.bufferBarrier(lossBuf_, rhi::ResourceState::CopySource, kCommon);
                p.busy = true;
                p.due = frame_ + kReadbackDelay;
                p.n = cfg_.batch;
                p.step = lifetime;
            }
            break;
        }
    }
    if (!net_.recordTrain(ctx, batchIn_, batchTgt_, batchW_, shape, norm, {}, ConvLossWeight::PerElement)) {
        end(Nrd2TrainStatus::Phase::Failed, "a training step could not be recorded");
        return false;
    }
    ++sessionSteps_;
    unitsRecorded_ += 1.0;
    std::lock_guard<std::mutex> lk(mutex_);
    status_.sessionSteps = sessionSteps_;
    status_.lifetimeSteps = resume_.lifetimeSteps + sessionSteps_;
    status_.learningRate = lr;
    status_.progress = cfg_.steps ? std::min(1.0f, static_cast<f32>(sessionSteps_) / static_cast<f32>(cfg_.steps)) : 1.0f;
    return true;
}

void Nrd2Trainer::startValidation(rhi::IRenderContext& ctx) {
    if (valWork_.empty()) {
        for (u32 s = 0; s < valSlots_.size(); ++s)
            for (u32 r = 0; r < valRefs_[s].size(); ++r) valWork_.push_back({s, r});
        valTotal_ = static_cast<u32>(valWork_.size());
        if (valTotal_ && res_) {
            if (valRb_) res_->destroyBuffer(valRb_);
            rhi::BufferDesc bd{};
            bd.bytes = static_cast<u64>(valTotal_) * 4;
            bd.kind = rhi::BufferKind::Readback;
            bd.debugName = "NRD2 validation readback";
            valRb_ = res_->createBuffer(bd);
        }
    }
    lastValidated_ = sessionSteps_;
    if (!valTotal_ || !valRb_) {
        end(Nrd2TrainStatus::Phase::Failed, "no held-out records to validate on");
        return;
    }
    mode_ = Mode::Validate;
    valNext_ = 0;
    // Weights stay put until the results are in (training pauses), so this readback is what is judged.
    if (net_.recordReadback(ctx)) {
        weightsDue_ = frame_ + kReadbackDelay;
        weightsStep_ = resume_.lifetimeSteps + sessionSteps_;
    }
    std::lock_guard<std::mutex> lk(mutex_);
    status_.phase = Nrd2TrainStatus::Phase::Validating;
    status_.message = "Validating on the held-out poses";
}

void Nrd2Trainer::validateBatch(rhi::IRenderContext& ctx) {
    const u32 n = std::min(cfg_.batch, valTotal_ - valNext_);
    std::vector<std::pair<const Slot*, Nrd2PatchRef>> items;
    for (u32 i = 0; i < n; ++i) {
        const auto [s, r] = valWork_[valNext_ + i];
        items.push_back({&valSlots_[s], valRefs_[s][r]});
    }
    gatherBatch(ctx, items);
    const TensorShape shape{n, kCh, kNrd2PatchTexels, kNrd2PatchTexels};
    if (net_.recordEvaluate(ctx, batchIn_, batchTgt_, batchW_, lossBuf_, shape, 1.0f, true, {},
                            ConvLossWeight::PerElement)) {
        ctx.bufferBarrier(lossBuf_, kCommon, rhi::ResourceState::CopySource);
        ctx.copyBuffer(valRb_, lossBuf_, static_cast<u64>(n) * 4, static_cast<u64>(valNext_) * 4, 0);
        ctx.bufferBarrier(lossBuf_, rhi::ResourceState::CopySource, kCommon);
    }
    valNext_ += n;
    unitsRecorded_ += kEvalUnit;
    if (valNext_ >= valTotal_) valDue_ = frame_ + kReadbackDelay;
}

bool Nrd2Trainer::saveWeights(const std::string& path, bool ema, const Nrd2Sidecar& sc) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    if (!net_.saveWeights(tmp, ema)) return false;
    if (!writeNrd2Sidecar(path + ".steps", sc)) return false;
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

void Nrd2Trainer::finishValidation(const std::vector<f32>& losses) {
    std::vector<Nrd2SceneRatio> sc(scenes_.size());
    for (u32 s = 0; s < scenes_.size(); ++s) sc[s].scene = scenes_[s];
    f64 wSum = 0.0;
    for (u32 v = 0; v < valSlots_.size(); ++v) {
        if (valSlots_[v].pose == kNoPose) continue;
        Nrd2SceneRatio& r = sc[sceneOf_[valSlots_[v].pose]];
        r.def += valDefault_[v];
        ++r.poses;
        wSum += valWeight_[v];
    }
    for (u32 i = 0; i < valWork_.size() && i < losses.size(); ++i) {
        const u32 slot = valWork_[i].first;
        if (valSlots_[slot].pose != kNoPose) sc[sceneOf_[valSlots_[slot].pose]].net += losses[i];
    }
    f64 net = 0.0, def = 0.0;
    std::string perScene;
    for (Nrd2SceneRatio& r : sc) {
        if (!r.poses) continue;
        r.ratio = r.def > 0.0 ? static_cast<f32>(r.net / r.def) : -1.0f;
        net += r.net;
        def += r.def;
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s%s %.3f", perScene.empty() ? "" : ", ", r.scene.c_str(), static_cast<double>(r.ratio));
        perScene += buf;
    }
    const f32 ratio = def > 0.0 && std::isfinite(net) ? static_cast<f32>(net / def) : -1.0f;
    const u64 lifetime = resume_.lifetimeSteps + sessionSteps_;
    f32 loss = -1.0f, best = -1.0f;
    u32 since = 0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        loss = status_.loss;
        best = status_.bestRatio;
        since = status_.sinceBest;
    }
    const bool improved = ratio >= 0.0f && (best < 0.0f || ratio < best);
    if (improved) { best = ratio; since = 0; }
    else ++since;
    AVER_INFO("[NRD2] train step {} lr {:.2e} loss {:.4f} | val {:.4f} (default {:.4f}, ratio {:.3f}){}; per scene: {}",
              lifetime, static_cast<double>(nrd2LearningRate(lifetime)), static_cast<double>(loss),
              wSum > 0.0 ? net / wSum : 0.0, wSum > 0.0 ? def / wSum : 0.0, static_cast<double>(ratio),
              improved ? " best" : "", perScene);

    const bool haveWeights = net_.cpuWeights(true).size() == net_.weightCount() && weightsStep_ == lifetime;
    if (haveWeights) {
        const std::span<const f32> m = net_.cpuWeights(false);
        lastWeights_.assign(m.begin(), m.end());
        lastWeightsStep_ = lifetime;
    }
    if (improved && haveWeights) {
        Nrd2Sidecar s;
        s.lifetimeSteps = lifetime;
        s.valRatio = ratio;
        s.datasetId = datasetId_;
        s.bestRatio = ratio;
        if (!saveWeights(cfg_.weightsPath, true, s)) AVER_WARN("[NRD2] could not save {}", cfg_.weightsPath);
    }
    const bool done = sessionSteps_ >= cfg_.steps || since >= cfg_.patience || cancel_;
    if (haveWeights && (sessionSteps_ - lastSavedStep_ >= cfg_.saveEvery || done)) {
        Nrd2Sidecar s;
        s.lifetimeSteps = lifetime;
        s.valRatio = ratio;
        s.datasetId = datasetId_;
        s.bestRatio = best;
        s.evalsSinceBest = since;
        if (saveWeights(nrd2LastPath(cfg_.weightsPath), false, s)) lastSavedStep_ = sessionSteps_;
        else AVER_WARN("[NRD2] could not save {}", nrd2LastPath(cfg_.weightsPath));
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        status_.lastRatio = ratio;
        status_.bestRatio = best;
        status_.sinceBest = since;
        ++status_.evaluations;
        status_.scenes = sc;
        status_.phase = Nrd2TrainStatus::Phase::Training;
        status_.message = "Training";
    }
    mode_ = Mode::Train;
    if (cancel_) end(Nrd2TrainStatus::Phase::Cancelled, "cancelled");
    else if (since >= cfg_.patience)
        end(Nrd2TrainStatus::Phase::Finished, "stopped early: " + std::to_string(cfg_.patience) +
                                                  " validations without improvement");
    else if (sessionSteps_ >= cfg_.steps) end(Nrd2TrainStatus::Phase::Finished, "reached the step target");
}

void Nrd2Trainer::collect() {
    for (Pending& p : pending_) {
        if (!p.busy || frame_ < p.due) continue;
        p.busy = false;
        std::vector<f32> v(p.n);
        if (res_->readBuffer(p.rb, v.data(), static_cast<u64>(p.n) * 4, 0)) {
            f64 s = 0.0;
            for (f32 x : v) s += x;
            std::lock_guard<std::mutex> lk(mutex_);
            status_.loss = static_cast<f32>(s);
        }
    }
    if (weightsDue_ && frame_ >= weightsDue_) {
        weightsDue_ = 0;
        if (!net_.collectWeights()) weightsStep_ = ~0ull;
    }
    if (valDue_ && frame_ >= valDue_) {
        valDue_ = 0;
        std::vector<f32> losses(valTotal_, 0.0f);
        if (!res_->readBuffer(valRb_, losses.data(), static_cast<u64>(valTotal_) * 4, 0)) {
            end(Nrd2TrainStatus::Phase::Failed, "the validation readback failed");
            return;
        }
        finishValidation(losses);
    }
}

void Nrd2Trainer::measureGpu() {
    const rhi::GpuTimingReport r = dev_.gpuTiming();
    if (!r.supported || r.framesAccumulated == 0) { timingOk_ = false; return; }
    f64 perFrame = 0.0;
    for (const rhi::GpuTimingNode& n : r.nodes) if (n.label == kSpan) perFrame += n.ms;
    const f64 sum = perFrame * r.framesAccumulated;
    if (r.framesAccumulated < lastTimingFrames_ || sum + 1e-9 < lastTimingSum_) {   // the device's totals were reset
        lastTimingFrames_ = r.framesAccumulated;
        lastTimingSum_ = sum;
        lastUnits_ = unitsRecorded_;
        return;
    }
    if (r.framesAccumulated - lastTimingFrames_ < 16) return;
    const f64 dUnits = unitsRecorded_ - lastUnits_, dMs = sum - lastTimingSum_;
    if (dUnits >= 1.0 && dMs > 0.0) {
        const f64 ms = dMs / dUnits;
        msPerUnit_ = msPerUnit_ > 0.0 ? 0.7 * msPerUnit_ + 0.3 * ms : ms;
        timingOk_ = true;
    }
    lastTimingFrames_ = r.framesAccumulated;
    lastTimingSum_ = sum;
    lastUnits_ = unitsRecorded_;
}

void Nrd2Trainer::end(Nrd2TrainStatus::Phase phase, std::string why) {
    Nrd2TrainStatus s = status();
    if (phase == Nrd2TrainStatus::Phase::Finished || phase == Nrd2TrainStatus::Phase::Cancelled) {
        Nrd2Sidecar best;
        const bool have = readNrd2Sidecar(cfg_.weightsPath + ".steps", best) && best.datasetId == datasetId_;
        AVER_INFO("[NRD2] training {}: {} ({} steps this session, {} lifetime); best held-out ratio {:.3f}; weights {} "
                  "(live gate {})",
                  phase == Nrd2TrainStatus::Phase::Finished ? "finished" : "cancelled", why, sessionSteps_,
                  resume_.lifetimeSteps + sessionSteps_, static_cast<double>(s.bestRatio),
                  have ? cfg_.weightsPath : std::string("not saved"),
                  have && nrd2GateOpen(best.valRatio, false) ? "open" : "closed");
    } else {
        AVER_WARN("[NRD2] training failed: {}", why);
    }
    {
        std::lock_guard<std::mutex> lk(mutex_);
        status_.phase = phase;
        status_.message = why;
        stopWorker_ = true;
    }
    cv_.notify_all();
    stopping_ = true;
}

void Nrd2Trainer::releaseGpu() {
    net_.destroy();
    if (res_) {
        for (std::vector<Slot>* v : {&slots_, &valSlots_})
            for (Slot& s : *v) {
                if (s.set) res_->destroyBindingSet(s.set);
                if (s.buf) res_->destroyBuffer(s.buf);
            }
        for (rhi::BufferHandle b : {batchIn_, batchTgt_, batchW_, lossBuf_, valRb_}) if (b) res_->destroyBuffer(b);
        for (rhi::BufferHandle b : staging_) if (b) res_->destroyBuffer(b);
        for (Pending& p : pending_) if (p.rb) res_->destroyBuffer(p.rb);
        if (gatherPso_) res_->destroyPipeline(gatherPso_);
    }
    slots_.clear();
    valSlots_.clear();
    pending_.clear();
    batchIn_ = batchTgt_ = batchW_ = lossBuf_ = valRb_ = 0;
    for (rhi::BufferHandle& b : staging_) b = 0;
    gatherPso_ = 0;
    gpuReady_ = false;
}

void Nrd2Trainer::step(rhi::IRenderContext& ctx) {
    if (stopping_) {
        // The worker is told to stop in end(); join it and free the GPU side once, outside its lock.
        stopping_ = false;
        if (thread_.joinable()) thread_.join();
        releaseGpu();
        return;
    }
    Nrd2TrainStatus::Phase phase;
    bool prepared = false, failed = false, cancel = false;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        phase = status_.phase;
        prepared = prepared_;
        failed = prepareFailed_;
        cancel = cancel_;
    }
    using Phase = Nrd2TrainStatus::Phase;
    if (phase != Phase::Loading && phase != Phase::Training && phase != Phase::Validating && phase != Phase::Stopping) return;
    ++frame_;

    if (phase == Phase::Loading) {
        if (failed) { end(Phase::Failed, status().message); return; }
        if (cancel) { end(Phase::Cancelled, "cancelled while loading"); return; }
        if (!prepared) return;
        if (!gpuReady_) {
            std::string why;
            if (!setupGpu(why)) { end(Phase::Failed, why); return; }
            valRefs_.assign(valSlots_.size(), {});
            valDefault_.assign(valSlots_.size(), 0.0);
            valWeight_.assign(valSlots_.size(), 0.0);
            for (u32 i = 0; i < trainSlots_ && i < train_.size(); ++i) requestLoad(train_[i]);
            rotation_ = std::min<u32>(trainSlots_, static_cast<u32>(train_.size()));
        }
        // One upload per frame: held-out poses first, then the initial training residents.
        std::unique_ptr<Packed> p = takeLoaded();
        if (p) {
            rhi::ScopedGpuStat stat(ctx, kSpan);
            if (p->validation) {
                const u32 v = valUploaded_++;
                if (p->error.empty() && v < valSlots_.size() && upload(ctx, valSlots_[v], *p)) {
                    valRefs_[v] = std::move(p->refs);
                    valDefault_[v] = p->defaultLoss;
                    valWeight_[v] = p->weightSum;
                } else {
                    AVER_WARN("[NRD2] held-out pose {} left out: {}", poses_[p->pose].path,
                              p->error.empty() ? std::string("upload failed") : p->error);
                }
            } else if (p->error.empty() && trainFilled_ < slots_.size() && upload(ctx, slots_[trainFilled_], *p)) {
                ++trainFilled_;
            } else {
                AVER_WARN("[NRD2] training pose {} left out: {}", poses_[p->pose].path,
                          p->error.empty() ? std::string("upload failed") : p->error);
                if (rotation_ < train_.size()) requestLoad(train_[rotation_++]);
                else trainSlots_ = std::max(trainFilled_, 1u);
            }
        }
        const bool valDone = valUploaded_ >= valSlots_.size();
        if (valDone && trainFilled_ >= std::min<u32>(trainSlots_, static_cast<u32>(slots_.size())) && trainFilled_ > 0) {
            slots_.resize(trainFilled_);
            trainSlots_ = trainFilled_;
            if (train_.size() > trainSlots_) requestLoad(train_[rotation_ % train_.size()]);   // prefetch the first swap
            std::lock_guard<std::mutex> lk(mutex_);
            status_.phase = Phase::Training;
            status_.residentPoses = trainSlots_;
            status_.message = "Training";
            AVER_INFO("[NRD2] training started: {} poses resident, {} held-out records", trainSlots_,
                      [&] { usize n = 0; for (const auto& r : valRefs_) n += r.size(); return n; }());
        }
        return;
    }

    measureGpu();
    collect();
    if (stopping_) return;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (status_.phase != Phase::Training && status_.phase != Phase::Validating && status_.phase != Phase::Stopping)
            return;
    }
    if (cancel && mode_ == Mode::Train && !valDue_ && !weightsDue_) {
        // Judge and checkpoint where it stands, then stop (finishValidation sees cancel_).
        if (sessionSteps_ > 0 && lastValidated_ != sessionSteps_) {
            std::lock_guard<std::mutex> lk(mutex_);
            status_.phase = Phase::Stopping;
        } else {
            saveOnExit();   // the weights just judged, unless .last already holds them
            end(Phase::Cancelled, "cancelled");
            return;
        }
    }

    const f64 budget = timingOk_ && msPerUnit_ > 0.0
                           ? std::clamp(static_cast<f64>(cfg_.gpuBudgetMs) / msPerUnit_, static_cast<f64>(kEvalUnit),
                                        static_cast<f64>(cfg_.maxStepsPerFrame))
                           : 2.0;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        status_.stepsPerFrame = static_cast<f32>(budget);
    }
    rhi::ScopedGpuStat stat(ctx, kSpan);
    f64 used = 0.0;
    bool uploaded = false;
    while (used < budget && !stopping_) {
        if (mode_ == Mode::Validate) {
            if (valNext_ >= valTotal_) break;   // waiting for the readback
            validateBatch(ctx);
            used += kEvalUnit;
            continue;
        }
        if (valDue_ || weightsDue_) break;
        const bool due = sessionSteps_ > 0 && lastValidated_ != sessionSteps_ &&
                         (sessionSteps_ % cfg_.validateEvery == 0 || sessionSteps_ >= cfg_.steps || cancel);
        if (due) {
            startValidation(ctx);
            continue;
        }
        if (cancel || sessionSteps_ >= cfg_.steps) break;
        // Streaming: one resident replaced on a fixed step schedule (so sampling stays deterministic).
        if (train_.size() > trainSlots_ && sessionSteps_ > 0 && sessionSteps_ % cfg_.swapEvery == 0 &&
            lastSwapped_ != sessionSteps_) {
            if (uploaded) break;
            std::unique_ptr<Packed> p = takeLoaded();
            if (!p) break;   // the next pose is still on its way from disk
            uploaded = true;
            Slot& s = slots_[fifo_];
            if (p->error.empty()) {
                const u32 before = s.pose;
                s.pose = kNoPose;
                if (upload(ctx, s, *p)) fifo_ = (fifo_ + 1) % trainSlots_;
                else s.pose = before;
            } else {
                AVER_WARN("[NRD2] training pose {} skipped: {}", poses_[p->pose].path, p->error);
            }
            lastSwapped_ = sessionSteps_;
            rotation_ = (rotation_ + 1) % static_cast<u32>(train_.size());
            requestLoad(train_[rotation_]);
            continue;
        }
        if (!trainStep(ctx)) break;
        used += 1.0;
    }
}

void Nrd2Trainer::saveOnExit() {
    if (lastWeights_.empty() || lastWeightsStep_ == 0 || sessionSteps_ == lastSavedStep_ || !net_.valid()) return;
    // The master weights of the last validation readback (cpuWeights(false) still holds them).
    Nrd2Sidecar s;
    s.lifetimeSteps = lastWeightsStep_;
    const Nrd2TrainStatus st = status();
    s.valRatio = st.lastRatio;
    s.datasetId = datasetId_;
    s.bestRatio = st.bestRatio;
    s.evalsSinceBest = st.sinceBest;
    if (saveWeights(nrd2LastPath(cfg_.weightsPath), false, s)) {
        lastSavedStep_ = sessionSteps_;
        AVER_INFO("[NRD2] checkpoint at step {} saved to {}", lastWeightsStep_, nrd2LastPath(cfg_.weightsPath));
    }
}

}  // namespace aver::render::denoise
