#include "aver/render/denoise/Nrd2Capture.hpp"

#include "aver/core/Log.hpp"
#include "aver/render/denoise/Nrd2Dataset.hpp"
#include "aver/render/neural/NeuralOptimiser.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace aver::render::denoise {

namespace {

constexpr u32 kConstantSlot = 3;
constexpr u32 kSnapFrame[4] = {2, 4, 8, 16};   // hold frames whose raw frame becomes a snapshot
constexpr u32 kMeanStart = 17;                 // the converged mean starts after the last snapshot
constexpr u32 kMinHold = 32;
constexpr u32 kReadbackDelay = 6;
constexpr u32 kRec = 32, kOutPlanes = 18, kGeoPlanes = 6, kMeanPlanes = 16;

// nrd2_capture.hlsl's flags and pass numbers.
constexpr u32 kFlagReset = 1u, kFlagHalf = 2u, kFlagFirst = 1u;
enum Pass : u32 { PassGeo = 0, PassAccum = 1, PassStats = 2, PassEval = 3, PassGrad = 4, PassStep = 5 };
enum StepMode : u32 { StepGrid = 0, StepAdam = 1, StepPattern = 2, StepFinalLoss = 3, StepFinal = 4 };

// Nrd2CapCB, byte for byte.
struct Constants {
    u32 rect[4];
    u32 tiles[4];   // x, y, flags, unused
    u32 mode[4];    // step mode or candidate set, candidate count, frames K, unused
    f32 def[12];
    f32 adam[4];    // lr, beta1, beta2, epsilon
    f32 adam2[4];   // bias corrections, lambda, split-half r0
};
static_assert(sizeof(Constants) == 128, "Nrd2CapCB: three uint4s, five float4s");

constexpr rhi::ResourceState kRead  = rhi::ResourceState::NonPixelShaderResource;
constexpr rhi::ResourceState kUav   = rhi::ResourceState::UnorderedAccess;
constexpr rhi::ResourceState kCommon = rhi::ResourceState::Common;

u32 tilesOf(u32 d) { return (d + 7u) / 8u; }

}  // namespace

Nrd2Capture::Nrd2Capture(rhi::IDevice& dev) : dev_(&dev), res_(dev.resources()) {}

Nrd2Capture::~Nrd2Capture() { release(); }

void Nrd2Capture::start(const Nrd2CaptureConfig& cfg) {
    cfg_ = cfg;
    if (cfg_.hold < kMinHold) cfg_.hold = kMinHold;
    cfg_.itersPerFrame = std::max(cfg_.itersPerFrame, 1u);
    index_ = 0;
    remaining_ = cfg_.poses;
    frame_ = 0;
    state_ = cfg_.poses ? State::Settle : State::Idle;
    AVER_INFO("[NRD2] capture: {} poses of scene '{}' (hold {} frames, oracle {}) into {}", cfg_.poses, cfg_.scene,
              cfg_.hold, cfg_.oracle == Nrd2OracleMode::Grad ? "grad" : "grid", cfg_.dir);
}

bool Nrd2Capture::ensurePipelines() {
    if (pipelinesTried_) return psoStep_ != 0;
    pipelinesTried_ = true;
    if (!res_) return false;
    const std::string& source = rhi::shaderFile("nrd2_capture.hlsl");
    if (source.empty()) {
        AVER_WARN("[NRD2] nrd2_capture.hlsl is not deployed beside the executable; capture unavailable");
        return false;
    }
    // Slots from `sbFrom` on are StructuredBuffers (SRVs); every UAV is one.
    auto layout = [](auto& l, u32 srv, u32 uav, u32 sbFrom) {
        l.srvCount = srv;
        l.uavCount = uav;
        for (u32 i = sbFrom; i < srv; ++i) l.srvKinds[i] = rhi::SlotKind::StructuredBuffer;
        for (u32 i = 0; i < uav; ++i) l.uavKinds[i] = rhi::SlotKind::StructuredBuffer;
    };
    bool ok = true;
    auto build = [&](u32 pass, const char* entry, u32 srv, u32 uav, u32 sbFrom) {
        const std::string defines = "AVER_NRD2C_PASS=" + std::to_string(pass);
        rhi::ShaderDesc sd{};
        sd.source = source.c_str();
        sd.entry = entry;
        sd.stage = rhi::ShaderStage::Compute;
        sd.minShaderModel = 60;
        sd.defines = defines.c_str();
        const rhi::ShaderHandle cs = res_->createShader(sd);
        rhi::PipelineHandle p = 0;
        if (cs) {
            rhi::ComputePipelineDesc pd{};
            pd.cs = cs;
            layout(pd.layout, srv, uav, sbFrom);
            pd.layout.slotKindsDeclared = true;
            pd.layout.constantDwords[kConstantSlot] = 0;   // root CBV
            p = res_->createComputePipeline(pd);
            res_->destroyShader(cs);
        }
        if (!p) { AVER_WARN("[NRD2] {} would not build; capture unavailable", entry); ok = false; }
        return p;
    };
    auto set = [&](u32 srv, u32 uav, u32 sbFrom) {
        rhi::BindingSetDesc bd{};
        layout(bd, srv, uav, sbFrom);
        const rhi::BindingSetHandle s = res_->createBindingSet(bd);
        if (!s) ok = false;
        return s;
    };
    psoGeo_   = build(PassGeo, "CSNrd2CapGeo", 3, 1, 3);
    psoAccum_ = build(PassAccum, "CSNrd2CapAccum", 3, 1, 3);
    psoStats_ = build(PassStats, "CSNrd2CapTileStats", 2, 1, 0);
    psoEval_  = build(PassEval, "CSNrd2CapEval", 13, 2, 11);
    psoGrad_  = build(PassGrad, "CSNrd2CapGrad", 13, 2, 11);
    psoStep_  = build(PassStep, "CSNrd2CapStep", 0, 3, 0);
    if (ok) {
        setGeo_ = set(3, 1, 3);
        setAccum_ = set(3, 1, 3);
        setStats_ = set(2, 1, 0);
        setStep_ = set(0, 3, 0);
        for (rhi::BindingSetHandle& s : setSnap_) s = set(13, 2, 11);
    }
    if (!ok) {
        AVER_WARN("[NRD2] capture pipelines incomplete; capture unavailable");
        release();
        pipelinesTried_ = true;
        return false;
    }
    AVER_INFO("[NRD2] capture and oracle pipelines built");
    return true;
}

void Nrd2Capture::releaseTargets() {
    if (!res_) return;
    auto dropT = [&](rhi::TextureHandle& t) { if (t) res_->destroyTexture(t); t = 0; };
    auto dropB = [&](rhi::BufferHandle& b) { if (b) res_->destroyBuffer(b); b = 0; };
    for (u32 k = 0; k < kSnapshots; ++k) {
        dropT(snapD_[k]); dropT(snapS_[k]);
        for (u32 l = 0; l < 3; ++l) { dropT(snapLvD_[k][l]); dropT(snapLvS_[k][l]); }
        dropB(rbFeat_[k]);
    }
    for (rhi::TextureHandle& t : snapG_) dropT(t);
    for (rhi::BufferHandle* b : {&geo_, &mean_, &fit_, &acc_, &out_, &rbOut_}) dropB(*b);
    rtW_ = rtH_ = tilesX_ = tilesY_ = 0;
    std::memset(rect_, 0, sizeof(rect_));
}

void Nrd2Capture::release() {
    releaseTargets();
    if (res_) {
        for (rhi::PipelineHandle* p : {&psoGeo_, &psoAccum_, &psoStats_, &psoEval_, &psoGrad_, &psoStep_})
            if (*p) res_->destroyPipeline(*p);
        for (rhi::BindingSetHandle* s : {&setGeo_, &setAccum_, &setStats_, &setStep_})
            if (*s) res_->destroyBindingSet(*s);
        for (rhi::BindingSetHandle& s : setSnap_) if (s) res_->destroyBindingSet(s);
    }
    psoGeo_ = psoAccum_ = psoStats_ = psoEval_ = psoGrad_ = psoStep_ = 0;
    setGeo_ = setAccum_ = setStats_ = setStep_ = 0;
    for (rhi::BindingSetHandle& s : setSnap_) s = 0;
    pipelinesTried_ = false;
    state_ = State::Idle;
}

bool Nrd2Capture::ensureTargets(const Nrd2& nrd2, const Nrd2::Inputs& in) {
    const u32 tx = tilesOf(in.viewport[2]), ty = tilesOf(in.viewport[3]);
    if (snapD_[0] && rtW_ == nrd2.width_ && rtH_ == nrd2.height_ && tilesX_ == tx && tilesY_ == ty &&
        std::memcmp(rect_, in.viewport, sizeof(rect_)) == 0)
        return true;
    releaseTargets();
    bool ok = true;
    auto tex = [&](u32 w, u32 h, const char* name) {
        rhi::TextureDesc d{};
        d.width = w; d.height = h;
        d.format = rhi::Format::RGBA16F;
        d.bind = rhi::ResourceBind::ShaderResource;
        d.initialState = kRead;
        d.debugName = name;
        const rhi::TextureHandle t = res_->createTexture(d);
        if (!t) ok = false;
        return t;
    };
    auto buf = [&](u64 floats, rhi::BufferKind kind, const char* name) {
        rhi::BufferDesc bd{};
        bd.bytes = floats * sizeof(f32);
        bd.kind = kind;
        bd.allowUnorderedAccess = kind == rhi::BufferKind::Default;
        bd.debugName = name;
        const rhi::BufferHandle b = res_->createBuffer(bd);
        if (!b) ok = false;
        return b;
    };
    const u32 W = nrd2.width_, H = nrd2.height_;
    const u64 np = static_cast<u64>(in.viewport[2]) * in.viewport[3];
    const u64 tiles = static_cast<u64>(tx) * ty;
    const u64 feats = tiles * 16u * kNrd2FeatureCount;
    for (u32 k = 0; k < kSnapshots; ++k) {
        snapD_[k] = tex(W, H, "NRD2 capture D");
        snapS_[k] = tex(W, H, "NRD2 capture S");
        for (u32 l = 0; l < 3; ++l) {
            const u32 lw = tilesOf(W) * (4u >> l), lh = tilesOf(H) * (4u >> l);
            snapLvD_[k][l] = tex(lw, lh, "NRD2 capture D level");
            snapLvS_[k][l] = tex(lw, lh, "NRD2 capture S level");
        }
        rbFeat_[k] = buf(feats, rhi::BufferKind::Readback, "NRD2 capture features readback");
    }
    for (u32 l = 0; l < 3; ++l) snapG_[l] = tex(tilesOf(W) * (4u >> l), tilesOf(H) * (4u >> l), "NRD2 capture guide");
    geo_  = buf(kGeoPlanes * np, rhi::BufferKind::Default, "NRD2 capture guides");
    mean_ = buf(kMeanPlanes * np, rhi::BufferKind::Default, "NRD2 capture means");
    fit_  = buf(tiles * 2u * kRec, rhi::BufferKind::Default, "NRD2 oracle state");
    acc_  = buf(tiles * 2u * kRec, rhi::BufferKind::Default, "NRD2 oracle sums");
    out_  = buf(tiles * kOutPlanes, rhi::BufferKind::Default, "NRD2 oracle result");
    rbOut_ = buf(tiles * kOutPlanes, rhi::BufferKind::Readback, "NRD2 oracle readback");
    if (!ok) {
        AVER_WARN("[NRD2] capture targets failed to allocate at {}x{}", W, H);
        releaseTargets();
        return false;
    }
    rtW_ = W; rtH_ = H; tilesX_ = tx; tilesY_ = ty;
    std::memcpy(rect_, in.viewport, sizeof(rect_));

    // Capture-owned resources only: written once per allocation.
    const u32 npx = static_cast<u32>(np), recs = static_cast<u32>(tiles * 2u * kRec);
    for (u32 k = 0; k < kSnapshots; ++k) {
        const rhi::BindingSetHandle s = setSnap_[k];
        res_->setSrv(s, 0, snapD_[k]);
        res_->setSrv(s, 1, snapS_[k]);
        for (u32 l = 0; l < 3; ++l) {
            res_->setSrv(s, 2 + l, snapG_[l]);
            res_->setSrv(s, 5 + l, snapLvD_[k][l]);
            res_->setSrv(s, 8 + l, snapLvS_[k][l]);
        }
        res_->setSrvBuffer(s, 11, geo_, sizeof(f32), kGeoPlanes * npx, 0);
        res_->setSrvBuffer(s, 12, mean_, sizeof(f32), kMeanPlanes * npx, 0);
        res_->setUavBuffer(s, 0, fit_, sizeof(f32), recs, 0);
        res_->setUavBuffer(s, 1, acc_, sizeof(f32), recs, 0);
    }
    res_->setSrvBuffer(setStats_, 0, geo_, sizeof(f32), kGeoPlanes * npx, 0);
    res_->setSrvBuffer(setStats_, 1, mean_, sizeof(f32), kMeanPlanes * npx, 0);
    res_->setUavBuffer(setStats_, 0, fit_, sizeof(f32), recs, 0);
    res_->setUavBuffer(setStep_, 0, fit_, sizeof(f32), recs, 0);
    res_->setUavBuffer(setStep_, 1, acc_, sizeof(f32), recs, 0);
    res_->setUavBuffer(setStep_, 2, out_, sizeof(f32), static_cast<u32>(tiles * kOutPlanes), 0);
    const f64 mib = (static_cast<f64>(W) * H * 8.0 * 2.0 * (1.0 + 0.25 + 0.0625 + 0.015625) * kSnapshots +
                     static_cast<f64>(np) * (kGeoPlanes + kMeanPlanes) * 4.0 + static_cast<f64>(feats) * 4.0 * kSnapshots) /
                    (1024.0 * 1024.0);
    AVER_INFO("[NRD2] capture targets at {}x{} (viewport {}x{}, {}x{} tiles): {:.0f} MiB", W, H, in.viewport[2],
              in.viewport[3], tx, ty, mib);
    return true;
}

void Nrd2Capture::abortPose(const char* why) {
    AVER_WARN("[NRD2] capture pose {} restarted: {}", index_, why);
    state_ = State::Travel;
    frame_ = 0;
}

void Nrd2Capture::snapshot(rhi::IRenderContext& ctx, Nrd2& nrd2, const Nrd2::Inputs& in, u32 k) {
    auto copy = [&](rhi::TextureHandle dst, rhi::TextureHandle src) {
        ctx.textureBarrier(src, kRead, rhi::ResourceState::CopySource);
        ctx.textureBarrier(dst, kRead, rhi::ResourceState::CopyDest);
        ctx.copyTexture(dst, src);
        ctx.textureBarrier(dst, rhi::ResourceState::CopyDest, kRead);
        ctx.textureBarrier(src, rhi::ResourceState::CopySource, kRead);
    };
    copy(snapD_[k], nrd2.targets_.diffuse);
    copy(snapS_[k], nrd2.targets_.specular);
    for (u32 l = 0; l < 3; ++l) {
        copy(snapLvD_[k][l], nrd2.levelD_[l]);
        copy(snapLvS_[k][l], nrd2.levelS_[l]);
    }
    if (k == 0) {
        for (u32 l = 0; l < 3; ++l) copy(snapG_[l], nrd2.guide_[l]);
        res_->setSrv(setGeo_, 0, in.viewZ);
        res_->setSrv(setGeo_, 1, in.normalRoughness);
        res_->setSrv(setGeo_, 2, nrd2.targets_.diffuse);
        const u32 np = rect_[2] * rect_[3];
        res_->setUavBuffer(setGeo_, 0, geo_, sizeof(f32), kGeoPlanes * np, 0);
        Constants cb{};
        std::memcpy(cb.rect, rect_, sizeof(cb.rect));
        cb.tiles[0] = tilesX_; cb.tiles[1] = tilesY_;
        ctx.bufferBarrier(geo_, kCommon, kUav);
        ctx.setPipeline(psoGeo_);
        ctx.setBindingSet(setGeo_);
        ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
        ctx.dispatch((rect_[2] + 7u) / 8u, (rect_[3] + 7u) / 8u, 1);
        ctx.bufferBarrier(geo_, kUav, kCommon);
    }
    if (!nrd2.recordFeatures(ctx, in)) {
        AVER_WARN("[NRD2] capture stopped: the feature pass is unavailable");
        state_ = State::Idle;   // targets go at the next start or release (in flight this frame)
        return;
    }
    const u64 bytes = static_cast<u64>(tilesX_) * tilesY_ * 16u * kNrd2FeatureCount * sizeof(f32);
    ctx.bufferBarrier(nrd2.features_, kCommon, rhi::ResourceState::CopySource);
    ctx.copyBuffer(rbFeat_[k], nrd2.features_, bytes);
    ctx.bufferBarrier(nrd2.features_, rhi::ResourceState::CopySource, kCommon);
}

void Nrd2Capture::accumulate(rhi::IRenderContext& ctx, Nrd2& nrd2, const Nrd2::Inputs& in, u32 h) {
    res_->setSrv(setAccum_, 0, nrd2.targets_.diffuse);
    res_->setSrv(setAccum_, 1, nrd2.targets_.specular);
    res_->setSrv(setAccum_, 2, in.viewZ);
    const u32 np = rect_[2] * rect_[3];
    res_->setUavBuffer(setAccum_, 0, mean_, sizeof(f32), kMeanPlanes * np, 0);
    Constants cb{};
    std::memcpy(cb.rect, rect_, sizeof(cb.rect));
    cb.tiles[0] = tilesX_; cb.tiles[1] = tilesY_;
    // Halves alternate in frame pairs: a checkerboard pixel traced every other frame lands in both.
    cb.tiles[2] = (h == kMeanStart ? kFlagReset : 0u) | (((h >> 1) & 1u) ? kFlagHalf : 0u) | ((in.halfRate & 15u) << 2);
    ctx.bufferBarrier(mean_, kCommon, kUav);
    ctx.setPipeline(psoAccum_);
    ctx.setBindingSet(setAccum_);
    ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
    ctx.dispatch((rect_[2] + 7u) / 8u, (rect_[3] + 7u) / 8u, 1);
    ctx.bufferBarrier(mean_, kUav, kCommon);
}

void Nrd2Capture::tileStats(rhi::IRenderContext& ctx) {
    Constants cb{};
    std::memcpy(cb.rect, rect_, sizeof(cb.rect));
    cb.tiles[0] = tilesX_; cb.tiles[1] = tilesY_;
    std::memcpy(cb.def, theta0_, sizeof(cb.def));
    cb.adam2[3] = kNrd2SplitHalfR0;
    ctx.bufferBarrier(geo_, kCommon, kRead);
    ctx.bufferBarrier(mean_, kCommon, kRead);
    ctx.bufferBarrier(fit_, kCommon, kUav);
    ctx.setPipeline(psoStats_);
    ctx.setBindingSet(setStats_);
    ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
    ctx.dispatch(tilesX_, tilesY_, 1);
    ctx.bufferBarrier(fit_, kUav, kCommon);
    ctx.bufferBarrier(mean_, kRead, kCommon);
    ctx.bufferBarrier(geo_, kRead, kCommon);
}

// One frame of the oracle: a bounded number of short dispatches (no single long one).
void Nrd2Capture::fitFrame(rhi::IRenderContext& ctx) {
    const Nrd2OracleDesc od{};   // the CPU twin's hyperparameters
    const bool grad = cfg_.oracle == Nrd2OracleMode::Grad;
    const u32 tiles = tilesX_ * tilesY_;
    Constants cb{};
    std::memcpy(cb.rect, rect_, sizeof(cb.rect));
    cb.tiles[0] = tilesX_; cb.tiles[1] = tilesY_;
    cb.mode[2] = kSnapshots;
    std::memcpy(cb.def, theta0_, sizeof(cb.def));
    cb.adam[0] = od.lr; cb.adam[1] = od.beta1; cb.adam[2] = od.beta2; cb.adam[3] = od.adamEps;
    cb.adam2[2] = od.lambda; cb.adam2[3] = kNrd2SplitHalfR0;

    rhi::ScopedGpuStat stat(ctx, "NRD2.Oracle");
    ctx.bufferBarrier(geo_, kCommon, kRead);
    ctx.bufferBarrier(mean_, kCommon, kRead);
    for (rhi::BufferHandle b : {fit_, acc_, out_}) ctx.bufferBarrier(b, kCommon, kUav);

    auto overSnapshots = [&](rhi::PipelineHandle pso, u32 set, u32 count) {
        cb.mode[0] = set; cb.mode[1] = count;
        for (u32 k = 0; k < kSnapshots; ++k) {
            cb.tiles[2] = k == 0 ? kFlagFirst : 0u;
            ctx.setPipeline(pso);
            ctx.setBindingSet(setSnap_[k]);
            ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
            ctx.dispatch(tilesX_, tilesY_, 1);
            ctx.uavBarrierBuffer(acc_);
        }
    };
    auto stepPass = [&](u32 mode, u32 t) {
        cb.mode[0] = mode;
        cb.tiles[2] = 0;
        cb.adam2[0] = t ? neural::adamBiasCorrection(od.beta1, t) : 1.0f;
        cb.adam2[1] = t ? neural::adamBiasCorrection(od.beta2, t) : 1.0f;
        ctx.setPipeline(psoStep_);
        ctx.setBindingSet(setStep_);
        ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
        ctx.dispatch((tiles * 2u + 63u) / 64u, 1, 1);
        ctx.uavBarrierBuffer(fit_);
        ctx.uavBarrierBuffer(out_);
    };

    bool done = false;
    switch (phase_) {
        case FitPhase::Grid:
            overSnapshots(psoEval_, 0, kNrd2GridStarts);
            stepPass(StepGrid, 0);
            phase_ = FitPhase::Iterate;
            iter_ = 0;
            break;
        case FitPhase::Iterate: {
            const u32 total = grad ? cfg_.adamIters : cfg_.patternIters;
            const u32 n = std::min(cfg_.itersPerFrame, total - std::min(iter_, total));
            for (u32 i = 0; i < n; ++i) {
                ++iter_;
                if (grad) { overSnapshots(psoGrad_, 0, 0); stepPass(StepAdam, iter_); }
                else      { overSnapshots(psoEval_, 1, kNrd2PatternCandidates); stepPass(StepPattern, 0); }
            }
            if (iter_ >= total) phase_ = FitPhase::Final;
            break;
        }
        case FitPhase::Final:
            if (grad) { overSnapshots(psoGrad_, 0, 0); stepPass(StepFinalLoss, 0); }
            else      stepPass(StepFinal, 0);
            done = true;
            break;
    }
    if (done) {
        ctx.bufferBarrier(out_, kUav, rhi::ResourceState::CopySource);
        ctx.copyBuffer(rbOut_, out_, static_cast<u64>(tiles) * kOutPlanes * sizeof(f32));
        ctx.bufferBarrier(out_, rhi::ResourceState::CopySource, kCommon);
    } else {
        ctx.bufferBarrier(out_, kUav, kCommon);
    }
    ctx.bufferBarrier(fit_, kUav, kCommon);
    ctx.bufferBarrier(acc_, kUav, kCommon);
    ctx.bufferBarrier(mean_, kRead, kCommon);
    ctx.bufferBarrier(geo_, kRead, kCommon);
    if (done) { state_ = State::Readback; frame_ = 0; }
}

void Nrd2Capture::writePose() {
    const u32 tiles = tilesX_ * tilesY_;
    Nrd2Pose pose;
    pose.stageBVersion = kNrd2StageBVersion;
    pose.scene = cfg_.scene;
    pose.poseIndex = index_;
    pose.heldOut = index_ >= cfg_.heldOutFrom;
    pose.tilesX = tilesX_; pose.tilesY = tilesY_;
    pose.halfW = 4 * tilesX_; pose.halfH = 4 * tilesY_;
    pose.frames = kSnapshots;
    pose.channels = kNrd2FeatureCount;
    const usize perFrame = static_cast<usize>(tiles) * 16u * kNrd2FeatureCount;
    std::vector<f32> tmp(std::max<usize>(perFrame, static_cast<usize>(tiles) * kOutPlanes));
    pose.features.resize(perFrame * kSnapshots);
    bool ok = true;
    for (u32 k = 0; k < kSnapshots && ok; ++k) {
        ok = res_->readBuffer(rbFeat_[k], tmp.data(), perFrame * sizeof(f32), 0);
        for (usize i = 0; ok && i < perFrame; ++i) pose.features[k * perFrame + i] = nrd2F32ToF16(tmp[i]);
    }
    if (ok) ok = res_->readBuffer(rbOut_, tmp.data(), static_cast<u64>(tiles) * kOutPlanes * sizeof(f32), 0);
    if (!ok) { AVER_WARN("[NRD2] capture pose {}: readback failed; not written", index_); return; }
    pose.theta.assign(tmp.begin(), tmp.begin() + 12 * tiles);
    pose.weights.assign(tmp.begin() + 12 * tiles, tmp.begin() + 14 * tiles);
    pose.losses.assign(tmp.begin() + 14 * tiles, tmp.begin() + 18 * tiles);

    // Summary over weighted tiles: oracle vs default data loss.
    f64 lo[2] = {}, ld[2] = {};
    u32 n[2] = {}, wins[2] = {};
    for (u32 s = 0; s < 2; ++s)
        for (u32 t = 0; t < tiles; ++t) {
            if (!(pose.weights[s * tiles + t] > 0.0f)) continue;
            const f32 o = pose.losses[s * tiles + t], d = pose.losses[(2 + s) * tiles + t];
            ++n[s]; lo[s] += o; ld[s] += d;
            if (o < d) ++wins[s];
        }

    std::error_code ec;
    std::filesystem::create_directories(cfg_.dir, ec);
    char name[32];
    std::snprintf(name, sizeof(name), "pose_%03u.n2p", index_);
    const std::string path = (std::filesystem::path(cfg_.dir) / name).string();
    std::string why;
    if (!writeNrd2Pose(path, pose, &why)) {
        AVER_WARN("[NRD2] capture pose {} could not be written to {}: {}", index_, path, why);
        return;
    }
    AVER_INFO("[NRD2] capture pose {} written: {}{} | D: oracle beats default on {}/{} tiles, mean loss {:.4g} vs "
              "{:.4g} | S: {}/{}, {:.4g} vs {:.4g}",
              index_, path, pose.heldOut ? " (held out)" : "", wins[0], n[0], n[0] ? lo[0] / n[0] : 0.0,
              n[0] ? ld[0] / n[0] : 0.0, wins[1], n[1], n[1] ? lo[1] / n[1] : 0.0, n[1] ? ld[1] / n[1] : 0.0);
}

void Nrd2Capture::step(rhi::IRenderContext& ctx, Nrd2& nrd2, const Nrd2::Inputs& in) {
    if (state_ == State::Idle) return;
    if (!ensurePipelines()) { state_ = State::Idle; return; }
    rhi::ScopedGpuStat stat(ctx, "NRD2.Capture");
    switch (state_) {
        case State::Settle:
            if (++frame_ >= cfg_.settleFrames) { state_ = State::Travel; frame_ = 0; }
            break;
        case State::Travel:
            if (++frame_ >= cfg_.travelFrames) {
                if (!ensureTargets(nrd2, in)) {
                    AVER_WARN("[NRD2] capture stopped: no memory for its targets");
                    release();
                    return;
                }
                std::memcpy(theta0_, nrd2.params_.diffuse, sizeof(nrd2.params_.diffuse));
                std::memcpy(theta0_ + 6, nrd2.params_.specular, sizeof(nrd2.params_.specular));
                state_ = State::Hold;
                frame_ = 0;
            }
            break;
        case State::Hold: {
            const u32 h = ++frame_;
            if (rtW_ != nrd2.width_ || rtH_ != nrd2.height_ || std::memcmp(rect_, in.viewport, sizeof(rect_)) != 0) {
                abortPose("the viewport changed during the hold");
                break;
            }
            for (u32 k = 0; k < kSnapshots; ++k)
                if (h == kSnapFrame[k]) snapshot(ctx, nrd2, in, k);
            if (state_ != State::Hold) break;
            if (h >= kMeanStart) accumulate(ctx, nrd2, in, h);
            if (h >= cfg_.hold) {
                tileStats(ctx);
                state_ = State::Fit;
                phase_ = FitPhase::Grid;
                iter_ = 0;
            }
            break;
        }
        case State::Fit:
            fitFrame(ctx);
            break;
        case State::Readback:
            if (++frame_ >= kReadbackDelay) {
                writePose();
                ++index_;
                frame_ = 0;
                if (--remaining_) {
                    state_ = State::Travel;
                } else {
                    AVER_INFO("[NRD2] capture finished: {} poses in {}", index_, cfg_.dir);
                    releaseTargets();
                    state_ = State::Idle;
                }
            }
            break;
        default:
            break;
    }
}

}  // namespace aver::render::denoise
