#include "aver/render/denoise/Denoiser.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace aver::render::denoise {

namespace {

// b1: Voxi-style root CBV. Slot 0 is the engine's per-frame block (PipelineLayout's own comment).
constexpr u32 kConstantSlot = 1;

// Shader bindings per pass, matching aver_denoise.hlsl's register lists.
constexpr u32 kSrvCount[7] = {10, 7, 10, 5, 14, 4, 4};
constexpr u32 kUavCount[7] = {4, 2, 2, 1, 2, 3, 1};
constexpr const char* kEntry[7] = {"CSDenoiseReproject", "CSDenoisePrefilter", "CSDenoiseResolve",
                                   "CSDenoiseScale", "CSDenoiseNrdResolve", "CSDenoiseNrdPyramid",
                                   "CSDenoiseCapture"};
// The frame-scale texture's SRV slot in each FidelityFX pass (DNSR_SCALE_SLOT).
constexpr u32 kScaleSrv[3] = {9, 6, 9};

// aver_denoise.hlsl's AverDenoiseCB, byte for byte.
struct Constants {
    u32 size[2];
    f32 invSize[2];
    u32 flags;
    u32 maxSamples;
    f32 historyClipWeight;
    f32 temporalStability;
};
static_assert(sizeof(Constants) == 32, "AverDenoiseCB is two float4s");

constexpr u32 kFlagReset        = 1u;
constexpr u32 kFlagHalfRate     = 2u;
constexpr u32 kFlagHalfRateOdd  = 4u;
constexpr u32 kFlagNrdSpatial   = 8u;
constexpr u32 kFlagCaptureReset = 16u;
constexpr u32 kFlagNrdNetwork   = 32u;

// NRD's weights file (tools/nrd/nrd_train.py): "NRDW", version 1, then the layer sizes.
constexpr u32 kNetMagic = 0x5744524Eu, kNetIn = 16, kNetH = 16, kNetOut = 4;
constexpr u32 kNetFloats = 2 * kNetIn + kNetH * kNetIn + kNetH + kNetH * kNetH + kNetH + kNetOut * kNetH + kNetOut;
constexpr u32 kNetSrv = 13;   // pass 4's t13

// Pass 4 binds a structured buffer at t13; every other slot of every pass is a 2D texture.
void declareSlots(u32 pass, rhi::SlotKind* srv) {
    for (u32 i = 0; i < kSrvCount[pass]; ++i) srv[i] = rhi::SlotKind::Texture2D;
    if (pass == 4) srv[kNetSrv] = rhi::SlotKind::StructuredBuffer;
}

// Training capture timing, in recorded frames: travel to a new pose, then hold. The raw input is taken
// at four hold frames (fresh reservoirs, as in motion); the mean runs over the hold frames after that.
constexpr u32 kCapTravel = 45, kCapMeanFrames = 256, kCapReadbackDelay = 6;
constexpr u32 kCapBaseFrame[4] = {2, 4, 8, 16};

// FidelityFX's reduction writes one average per 8x8 group.
u32 averageDim(u32 d) { return (d + 7u) / 8u; }

}  // namespace

Denoiser::~Denoiser() { destroy(); }

bool Denoiser::create(rhi::IDevice& dev) {
    destroy();
    dev_ = &dev;
    res_ = dev.resources();
    if (!res_) { dev_ = nullptr; return false; }

    const std::string& source = rhi::shaderFile("aver_denoise.hlsl");
    if (source.empty()) {
        AVER_WARN("[Denoise] aver_denoise.hlsl is not deployed beside the executable; running undenoised");
        destroy();
        return false;
    }
    u32 built = 0;
    for (u32 pass = 0; pass < kPassCount; ++pass) {
        for (u32 scalar = 0; scalar < ((pass == Scale || pass == NrdCapture) ? 1u : 2u); ++scalar) {
            // AVER_HLSL_2018 is consumed by the shader compiler (D3D12Device.cpp): FidelityFX's
            // headers are written against HLSL 2018 and are vendored unmodified.
            const std::string defines = "AVER_HLSL_2018;AVER_DNSR_PASS=" + std::to_string(pass) +
                                        ";AVER_DNSR_SCALAR=" + std::to_string(scalar);
            rhi::ShaderDesc sd{};
            sd.source  = source.c_str();
            sd.entry   = kEntry[pass];
            sd.stage   = rhi::ShaderStage::Compute;
            sd.minShaderModel = 62;   // DXC, not FXC: the headers use min16float and binary literals
            sd.defines = defines.c_str();
            const rhi::ShaderHandle cs = res_->createShader(sd);
            if (!cs && pass >= NrdResolve) {
                AVER_WARN("[Denoise] {} ({}) would not compile; Neural Denoise unavailable", kEntry[pass],
                          scalar ? "one channel" : "colour");
                continue;
            }
            if (!cs) {
                AVER_WARN("[Denoise] {} ({}) would not compile; running undenoised", kEntry[pass],
                          scalar ? "one channel" : "colour");
                destroy();
                return false;
            }
            rhi::ComputePipelineDesc pd{};
            pd.cs = cs;
            pd.layout.srvCount = kSrvCount[pass];
            pd.layout.uavCount = kUavCount[pass];
            pd.layout.slotKindsDeclared = true;
            declareSlots(pass, pd.layout.srvKinds);
            for (u32 u = 0; u < kUavCount[pass]; ++u) pd.layout.uavKinds[u] = rhi::SlotKind::Texture2D;
            pd.layout.constantDwords[kConstantSlot] = 0;   // root CBV
            pd.layout.samplerCount = 1;
            pd.layout.samplers[0].filter  = rhi::Filter::Linear;
            pd.layout.samplers[0].address = rhi::AddressMode::Clamp;
            const rhi::PipelineHandle p = res_->createComputePipeline(pd);
            res_->destroyShader(cs);   // the pipeline owns the bytecode now
            if (!p && pass >= NrdResolve) {
                AVER_WARN("[Denoise] the {} pipeline would not build; Neural Denoise unavailable", kEntry[pass]);
                continue;
            }
            if (!p) {
                AVER_WARN("[Denoise] the {} pipeline would not build; running undenoised", kEntry[pass]);
                destroy();
                return false;
            }
            pipelines_[pass * 2 + scalar] = p;
            ++built;
        }
    }
    // Neural Denoise needs both variants; a half-built pair would only cover one signal.
    for (u32 pass : {u32(NrdResolve), u32(NrdPyramid)}) {
        if (pipelines_[pass * 2] && pipelines_[pass * 2 + 1]) continue;
        for (u32 v = 0; v < 2; ++v) {
            rhi::PipelineHandle& p = pipelines_[pass * 2 + v];
            if (p) { res_->destroyPipeline(p); p = 0; --built; }
        }
    }
    AVER_INFO("[Denoise] AMD FidelityFX Denoiser (reflection pipeline, diffuse use): {} pipelines built", built);
    return true;
}

void Denoiser::releaseTargets() {
    if (!res_) return;
    auto drop = [&](rhi::TextureHandle& t) { if (t) res_->destroyTexture(t); t = 0; };
    for (SignalTargets& s : sig_) {
        for (u32 i = 0; i < 2; ++i) { drop(s.history[i]); drop(s.varHistory[i]); drop(s.sampleCount[i]); }
        drop(s.reprojected);
        drop(s.average);
        drop(s.variance);
        drop(s.prefiltered);
        drop(s.prefilteredVar);
        drop(s.scale);
        for (rhi::TextureHandle& l : s.nrdLevel) drop(l);
        for (rhi::BindingSetHandle& b : s.sets) { if (b) res_->destroyBindingSet(b); b = 0; }
        s.parity = 0;
        s.historyState[0] = s.historyState[1] = rhi::ResourceState::NonPixelShaderResource;
    }
    drop(depthHistory_);
    drop(normalHistory_);
    for (rhi::TextureHandle& o : output_) o = 0;
    for (bool& r : ranLast_) r = false;
    forceHistoryReset();
}

void Denoiser::loadWeights() {
    if (weightsTried_ || !res_) return;
    weightsTried_ = true;
    if (net_) { res_->destroyBuffer(net_); net_ = 0; }
    if (weightsPath_.empty()) return;
    std::ifstream f(weightsPath_, std::ios::binary);
    u32 header[6] = {};
    std::vector<f32> w(kNetFloats);
    if (!f.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != kNetMagic || header[1] != 1u ||
        header[2] != kNetIn || header[3] != kNetH || header[4] != kNetH || header[5] != kNetOut ||
        !f.read(reinterpret_cast<char*>(w.data()), static_cast<std::streamsize>(w.size() * sizeof(f32)))) {
        AVER_WARN("[Denoise] no usable NRD weights at {}; Neural Denoise runs FidelityFX's resolve", weightsPath_);
        return;
    }
    rhi::BufferDesc bd{};
    bd.bytes = w.size() * sizeof(f32);
    bd.kind = rhi::BufferKind::Upload;
    bd.debugName = "NRD network weights";
    net_ = res_->createBuffer(bd);
    if (!net_ || !res_->writeBuffer(net_, w.data(), bd.bytes, 0)) {
        if (net_) res_->destroyBuffer(net_);
        net_ = 0;
        return;
    }
    netFloats_ = kNetFloats;
    AVER_INFO("[Denoise] NRD network loaded: {}", weightsPath_);
}

void Denoiser::destroy() {
    for (rhi::BufferHandle* b : {&net_, &netPlaceholder_}) { if (res_ && *b) res_->destroyBuffer(*b); *b = 0; }
    weightsTried_ = false;
    captureRelease();
    releaseTargets();
    if (res_) for (rhi::PipelineHandle& p : pipelines_) { if (p) res_->destroyPipeline(p); p = 0; }
    for (rhi::PipelineHandle& p : pipelines_) p = 0;
    dev_ = nullptr;
    res_ = nullptr;
    width_ = height_ = 0;
}

bool Denoiser::resize(u32 width, u32 height) {
    if (!valid() || width == 0 || height == 0) return false;
    if (width == width_ && height == height_ && depthHistory_) return true;
    if (width == failedWidth_ && height == failedHeight_) return false;

    releaseTargets();
    width_ = width;
    height_ = height;

    // Every target rests in NonPixelShaderResource -- the state the compute passes read in -- and
    // is moved to UnorderedAccess only around the dispatch that writes it.
    bool ok = true;
    auto make = [&](rhi::Format f, u32 w, u32 h, bool uav, const char* name) -> rhi::TextureHandle {
        rhi::TextureDesc d{};
        d.width  = w;
        d.height = h;
        d.format = f;
        d.bind   = uav ? static_cast<rhi::ResourceBind>(static_cast<u32>(rhi::ResourceBind::ShaderResource) |
                                                        static_cast<u32>(rhi::ResourceBind::UnorderedAccess))
                       : rhi::ResourceBind::ShaderResource;
        d.initialState = rhi::ResourceState::NonPixelShaderResource;
        d.debugName = name;
        const rhi::TextureHandle t = res_->createTexture(d);
        if (!t) {
            AVER_WARN("[Denoise] {} failed to allocate at {}x{}", name, w, h);
            ok = false;
        }
        return t;
    };

    for (u32 s = 0; s < kSignalCount && ok; ++s) {
        SignalTargets& t = sig_[s];
        const bool colour = s == static_cast<u32>(Signal::Radiance);
        const rhi::Format value = colour ? rhi::Format::RGBA16F : rhi::Format::R16F;
        const char* tag = colour ? "radiance" : "occlusion";
        // Names live for the texture's lifetime only as debug labels the backend copies.
        const std::string n = std::string("Denoise ") + tag;
        t.history[0]     = make(value, width, height, true, (n + " history A").c_str());
        t.history[1]     = make(value, width, height, true, (n + " history B").c_str());
        t.varHistory[0]  = make(rhi::Format::R16F, width, height, true, (n + " variance history A").c_str());
        t.varHistory[1]  = make(rhi::Format::R16F, width, height, true, (n + " variance history B").c_str());
        t.sampleCount[0] = make(rhi::Format::R16F, width, height, true, (n + " sample count A").c_str());
        t.sampleCount[1] = make(rhi::Format::R16F, width, height, true, (n + " sample count B").c_str());
        t.reprojected    = make(value, width, height, true, (n + " reprojected").c_str());
        t.average        = make(value, averageDim(width), averageDim(height), true, (n + " 8x8 average").c_str());
        t.variance       = make(rhi::Format::R16F, width, height, true, (n + " variance").c_str());
        t.prefiltered    = make(value, width, height, true, (n + " prefiltered").c_str());
        t.prefilteredVar = make(rhi::Format::R16F, width, height, true, (n + " prefiltered variance").c_str());
        t.scale          = make(rhi::Format::R32Float, 1, 1, true, (n + " scale").c_str());
        for (u32 l = 0; l < 3; ++l) {
            const u32 div = 2u << l;
            t.nrdLevel[l] = make(rhi::Format::RGBA16F, (width + div - 1) / div, (height + div - 1) / div, true,
                                 (n + " NRD level " + std::to_string(l + 1)).c_str());
        }
        for (u32 p = 0; p < kPassCount && ok; ++p) {
            rhi::BindingSetDesc bd{};
            bd.srvCount = kSrvCount[p];
            bd.uavCount = kUavCount[p];
            declareSlots(p, bd.srvKinds);
            t.sets[p] = res_->createBindingSet(bd);
            if (!t.sets[p]) { AVER_WARN("[Denoise] createBindingSet failed"); ok = false; }
        }
    }
    if (ok) depthHistory_  = make(rhi::Format::R32Float, width, height, false, "Denoise view-Z history");
    if (ok) normalHistory_ = make(rhi::Format::RGB10A2Unorm, width, height, false, "Denoise normal history");

    if (!ok) {
        releaseTargets();
        width_ = height_ = 0;
        failedWidth_ = width;
        failedHeight_ = height;
        return false;
    }
    failedWidth_ = failedHeight_ = 0;
    // Value targets: 4 full-resolution colour (8 B) or one-channel (2 B) per signal, plus six R16F.
    const f64 px = static_cast<f64>(width) * height;
    AVER_INFO("[Denoise] targets at {}x{}: {:.1f} MiB", width, height,
              px * ((4 * 8 + 6 * 2) + (4 * 2 + 6 * 2) + 4 + 4) / (1024.0 * 1024.0));
    return true;
}

bool Denoiser::recordSignal(rhi::IRenderContext& ctx, u32 s, rhi::TextureHandle input,
                            const Inputs& in, u32 flags, bool neuralResolve, bool neuralSpatial) {
    SignalTargets& t = sig_[s];
    const u32 cur = t.parity, prev = 1u - t.parity;
    const u32 scalar = s == static_cast<u32>(Signal::Occlusion) ? 1u : 0u;

    // Descriptors first, every set, before any dispatch -- a set rewritten between two dispatches
    // that use it would leave the first reading the second's resources.
    for (rhi::BindingSetHandle set : t.sets) {
        res_->setSrv(set, 0, in.viewZ);
        res_->setSrv(set, 1, in.normalRoughness);
        res_->setSrv(set, 2, in.motionVectors);
        res_->setSrv(set, 3, input);
    }
    rhi::BindingSetHandle rp = t.sets[Reproject];
    res_->setSrv(rp, 4, depthHistory_);
    res_->setSrv(rp, 5, normalHistory_);
    res_->setSrv(rp, 6, t.history[prev]);
    res_->setSrv(rp, 7, t.varHistory[prev]);
    res_->setSrv(rp, 8, t.sampleCount[prev]);
    res_->setUav(rp, 0, t.reprojected, 0);
    res_->setUav(rp, 1, t.average, 0);
    res_->setUav(rp, 2, t.variance, 0);
    res_->setUav(rp, 3, t.sampleCount[cur], 0);
    rhi::BindingSetHandle pf = t.sets[Prefilter];
    res_->setSrv(pf, 4, t.variance);
    res_->setSrv(pf, 5, t.average);
    res_->setUav(pf, 0, t.prefiltered, 0);
    res_->setUav(pf, 1, t.prefilteredVar, 0);
    rhi::BindingSetHandle rs = t.sets[Resolve];
    res_->setSrv(rs, 4, t.prefiltered);
    res_->setSrv(rs, 5, t.reprojected);
    res_->setSrv(rs, 6, t.prefilteredVar);
    res_->setSrv(rs, 7, t.sampleCount[cur]);
    res_->setSrv(rs, 8, t.average);
    res_->setUav(rs, 0, t.history[cur], 0);
    res_->setUav(rs, 1, t.varHistory[cur], 0);
    // NRD's resolve: the same bindings as FidelityFX's (phase 1 of NEURAA_NRD.md section 8).
    rhi::BindingSetHandle nr = t.sets[NrdResolve];
    res_->setSrv(nr, 4, t.prefiltered);
    res_->setSrv(nr, 5, t.reprojected);
    res_->setSrv(nr, 6, t.prefilteredVar);
    res_->setSrv(nr, 7, t.sampleCount[cur]);
    res_->setSrv(nr, 8, t.average);
    res_->setSrv(nr, 9, t.scale);
    res_->setUav(nr, 0, t.history[cur], 0);
    res_->setUav(nr, 1, t.varHistory[cur], 0);
    rhi::BindingSetHandle py = t.sets[NrdPyramid];
    for (u32 l = 0; l < 3; ++l) {
        res_->setSrv(nr, 10 + l, t.nrdLevel[l]);
        res_->setUav(py, l, t.nrdLevel[l], 0);
    }
    if (neuralResolve) loadWeights();
    if (!net_ && !netPlaceholder_) {
        rhi::BufferDesc pd{};
        pd.bytes = sizeof(f32);
        pd.kind = rhi::BufferKind::Upload;
        pd.debugName = "NRD weights placeholder";
        netPlaceholder_ = res_->createBuffer(pd);
    }
    if (net_) res_->setSrvBuffer(nr, kNetSrv, net_, sizeof(f32), netFloats_, 0);
    else      res_->setSrvBuffer(nr, kNetSrv, netPlaceholder_, sizeof(f32), 1, 0);
    for (u32 p = 0; p < Scale; ++p) res_->setSrv(t.sets[p], kScaleSrv[p], t.scale);
    rhi::BindingSetHandle sc = t.sets[Scale];
    res_->setSrv(sc, 4, t.average);   // still last frame's: Scale records before Reproject
    res_->setUav(sc, 0, t.scale, 0);

    // Neural Denoise: NRD's spatial resolve with the network when its weights loaded (radiance only;
    // they were trained on it), or with fixed weights under the developer switch; otherwise
    // FidelityFX's resolve through NRD's pass.
    const bool nrd = neuralResolve && pipelines_[NrdResolve * 2 + scalar] != 0;
    const bool network = nrd && net_ != 0 && s == static_cast<u32>(Signal::Radiance);
    const bool nrdSpatial = nrd && (neuralSpatial || network) && pipelines_[NrdPyramid * 2 + scalar] != 0;
    if (nrdSpatial) flags |= kFlagNrdSpatial;
    if (nrdSpatial && network) flags |= kFlagNrdNetwork;

    Constants cb{};
    cb.size[0] = width_;
    cb.size[1] = height_;
    cb.invSize[0] = 1.0f / static_cast<f32>(width_);
    cb.invSize[1] = 1.0f / static_cast<f32>(height_);
    cb.flags = flags;
    cb.maxSamples = tuning_.maxSamples ? tuning_.maxSamples : 1u;
    cb.historyClipWeight = tuning_.historyClipWeight;
    cb.temporalStability = 0.0f;
    const u32 gx = (width_ + 7u) / 8u, gy = (height_ + 7u) / 8u;
    constexpr rhi::ResourceState kRead = rhi::ResourceState::NonPixelShaderResource;
    constexpr rhi::ResourceState kWrite = rhi::ResourceState::UnorderedAccess;

    // Last frame's output was left pixel-readable for Voxi; the reproject pass reads it in compute.
    if (t.historyState[prev] != kRead) {
        ctx.textureBarrier(t.history[prev], t.historyState[prev], kRead);
        t.historyState[prev] = kRead;
    }

    auto dispatch = [&](Pass pass, std::initializer_list<rhi::TextureHandle> outs) {
        for (rhi::TextureHandle o : outs) ctx.textureBarrier(o, kRead, kWrite);
        ctx.setPipeline(pipelines_[pass * 2 + scalar]);
        ctx.setBindingSet(t.sets[pass]);
        ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
        if (pass == Scale) ctx.dispatch(1, 1, 1);
        else               ctx.dispatch(gx, gy, 1);
        for (rhi::TextureHandle o : outs) ctx.textureBarrier(o, kWrite, kRead);
    };
    if (!scalar) dispatch(Scale, {t.scale});
    dispatch(Reproject, {t.reprojected, t.average, t.variance, t.sampleCount[cur]});
    dispatch(Prefilter, {t.prefiltered, t.prefilteredVar});
    // history[cur] rests in NonPixelShaderResource here: it was last frame's `prev`.
    if (t.historyState[cur] != kRead) {
        ctx.textureBarrier(t.history[cur], t.historyState[cur], kRead);
        t.historyState[cur] = kRead;
    }
    if (nrdSpatial) dispatch(NrdPyramid, {t.nrdLevel[0], t.nrdLevel[1], t.nrdLevel[2]});
    dispatch(nrd ? NrdResolve : Resolve, {t.history[cur], t.varHistory[cur]});

    // Handed to Voxi, whose pixel shaders read it this frame.
    ctx.textureBarrier(t.history[cur], kRead, rhi::ResourceState::ShaderResource);
    t.historyState[cur] = rhi::ResourceState::ShaderResource;
    output_[s] = t.history[cur];
    t.parity = prev;
    return true;
}

bool Denoiser::record(rhi::IRenderContext& ctx, const Frame& frame, const Inputs& in) {
    for (rhi::TextureHandle& o : output_) o = 0;
    if (!valid() || !depthHistory_) return false;
    if (!in.viewZ || !in.motionVectors || !in.normalRoughness) {
        AVER_WARN("[Denoise] record called with a null G-buffer input; skipping the pass");
        return false;
    }
    const bool run[kSignalCount] = {frame.runOcclusion && in.occlusion != 0,
                                    frame.runRadiance && in.radiance != 0};
    if (!run[0] && !run[1]) {
        for (bool& r : ranLast_) r = false;
        return false;
    }

    rhi::ScopedGpuStat gpuStat(ctx, "Denoise");
    constexpr rhi::ResourceState kRead = rhi::ResourceState::NonPixelShaderResource;
    const rhi::TextureHandle gbuf[3] = {in.viewZ, in.normalRoughness, in.motionVectors};
    const rhi::TextureHandle signal[kSignalCount] = {in.occlusion, in.radiance};
    for (rhi::TextureHandle g : gbuf) ctx.textureBarrier(g, in.gbufferState, kRead);
    for (u32 s = 0; s < kSignalCount; ++s)
        if (run[s]) ctx.textureBarrier(signal[s], in.signalState, kRead);

    for (u32 s = 0; s < kSignalCount; ++s) {
        if (!run[s]) continue;
        u32 flags = 0;
        // A signal that sat out last frame restarts: its history holds pre-gap content that one
        // frame's motion vectors cannot reproject.
        if (frame.resetHistory || stale_[s] || !ranLast_[s]) flags |= kFlagReset;
        if (s == static_cast<u32>(Signal::Radiance) && frame.radianceHalfRate) {
            flags |= kFlagHalfRate;
            if (frame.radianceHalfRateParity & 1u) flags |= kFlagHalfRateOdd;
        }
        recordSignal(ctx, s, signal[s], in, flags, frame.neuralResolve, frame.neuralSpatial);
        stale_[s] = false;
    }

    if (cap_.state != CapState::Idle && run[static_cast<u32>(Signal::Radiance)]) captureStep(ctx, frame, in);

    // This frame's G-buffer becomes next frame's history, after every pass has read the old one.
    ctx.textureBarrier(in.viewZ, kRead, rhi::ResourceState::CopySource);
    ctx.textureBarrier(in.normalRoughness, kRead, rhi::ResourceState::CopySource);
    ctx.textureBarrier(depthHistory_, kRead, rhi::ResourceState::CopyDest);
    ctx.textureBarrier(normalHistory_, kRead, rhi::ResourceState::CopyDest);
    ctx.copyTexture(depthHistory_, in.viewZ);
    ctx.copyTexture(normalHistory_, in.normalRoughness);
    ctx.textureBarrier(depthHistory_, rhi::ResourceState::CopyDest, kRead);
    ctx.textureBarrier(normalHistory_, rhi::ResourceState::CopyDest, kRead);
    ctx.textureBarrier(in.viewZ, rhi::ResourceState::CopySource, in.gbufferState);
    ctx.textureBarrier(in.normalRoughness, rhi::ResourceState::CopySource, in.gbufferState);
    ctx.textureBarrier(in.motionVectors, kRead, in.gbufferState);
    for (u32 s = 0; s < kSignalCount; ++s)
        if (run[s]) ctx.textureBarrier(signal[s], kRead, in.signalState);

    for (u32 s = 0; s < kSignalCount; ++s) ranLast_[s] = run[s];
    return true;
}

// ---- NRD training capture ---------------------------------------------------------------------

void Denoiser::startCapture(const std::string& dir, u32 count) {
    cap_.dir = dir;
    cap_.remaining = count;
    cap_.index = 0;
    cap_.frame = 0;
    cap_.state = count ? CapState::Travel : CapState::Idle;
    AVER_INFO("[Denoise] NRD training capture: {} poses into {}", count, dir);
}

void Denoiser::captureRelease() {
    if (res_ && cap_.accum) res_->destroyTexture(cap_.accum);
    cap_.accum = 0;
    for (rhi::BufferHandle& b : cap_.rb) { if (res_ && b) res_->destroyBuffer(b); b = 0; }
    cap_.w = cap_.h = 0;
}

bool Denoiser::captureTargets(const Inputs& in) {
    if (cap_.accum && cap_.w == width_ && cap_.h == height_) return true;
    captureRelease();
    rhi::TextureDesc d{};
    d.width = width_; d.height = height_;
    d.format = rhi::Format::RGBA16F;
    d.bind = static_cast<rhi::ResourceBind>(static_cast<u32>(rhi::ResourceBind::ShaderResource) |
                                            static_cast<u32>(rhi::ResourceBind::UnorderedAccess));
    d.initialState = rhi::ResourceState::NonPixelShaderResource;
    d.debugName = "NRD capture mean";
    cap_.accum = res_->createTexture(d);
    const rhi::TextureHandle src[kCapArrays] = {in.radiance, in.radiance, in.radiance, in.radiance,
                                                in.viewZ, in.normalRoughness, cap_.accum};
    for (u32 i = 0; i < kCapArrays; ++i) {
        if (!src[i] || !res_->textureCopyFootprint(src[i], 0, cap_.fp[i])) { captureRelease(); return false; }
        rhi::BufferDesc bd{};
        bd.bytes = cap_.fp[i].totalBytes;
        bd.kind = rhi::BufferKind::Readback;
        bd.debugName = "NRD capture readback";
        cap_.rb[i] = res_->createBuffer(bd);
        if (!cap_.rb[i]) { captureRelease(); return false; }
    }
    cap_.w = width_; cap_.h = height_;
    return true;
}

void Denoiser::captureCopy(rhi::IRenderContext& ctx, u32 slot, rhi::TextureHandle t, rhi::ResourceState rest) {
    ctx.textureBarrier(t, rest, rhi::ResourceState::CopySource);
    ctx.copyTextureToBuffer(cap_.rb[slot], 0, t, 0);
    ctx.textureBarrier(t, rhi::ResourceState::CopySource, rest);
}

// File: "NRDC" (u32 0x4344524E), version 1, width, height, base count 4, the four base flags (bit 0
// half-rate input, bit 1 parity), all u32; then, rows tightly packed: four raw radiance frames RGBA16F
// (alpha = GI hit distance), view Z R32F, normal RGB10A2, converged mean RGBA16F (alpha = fresh samples).
void Denoiser::captureWrite() {
    char name[32];
    std::snprintf(name, sizeof(name), "nrd_%03u.bin", cap_.index);
    const std::string path = cap_.dir + "/" + name;
    std::ofstream f(path, std::ios::binary);
    const u32 header[9] = {0x4344524Eu, 1u, cap_.w, cap_.h, kCapBases,
                           cap_.baseFlags[0], cap_.baseFlags[1], cap_.baseFlags[2], cap_.baseFlags[3]};
    f.write(reinterpret_cast<const char*>(header), sizeof(header));
    std::vector<u8> buf;
    for (u32 i = 0; i < kCapArrays && f; ++i) {
        const rhi::TextureCopyFootprint& fp = cap_.fp[i];
        buf.resize(static_cast<size_t>(fp.totalBytes));
        if (!res_->readBuffer(cap_.rb[i], buf.data(), fp.totalBytes, 0)) { f.setstate(std::ios::failbit); break; }
        for (u32 r = 0; r < fp.rows; ++r)
            f.write(reinterpret_cast<const char*>(buf.data()) + static_cast<size_t>(r) * fp.rowPitch, fp.rowBytes);
    }
    if (f) AVER_INFO("[Denoise] NRD capture {} written: {}", cap_.index, path);
    else   AVER_WARN("[Denoise] NRD capture {} could not be written to {}", cap_.index, path);
}

// Called inside record(), with the G-buffer and the radiance signal in NonPixelShaderResource.
void Denoiser::captureStep(rhi::IRenderContext& ctx, const Frame& frame, const Inputs& in) {
    constexpr rhi::ResourceState kRead = rhi::ResourceState::NonPixelShaderResource;
    switch (cap_.state) {
        case CapState::Travel:
            if (++cap_.frame >= kCapTravel) { cap_.state = CapState::Hold; cap_.frame = 0; }
            break;
        case CapState::Hold: {
            const u32 h = ++cap_.frame;
            if (!captureTargets(in) || !pipelines_[NrdCapture * 2]) { cap_.state = CapState::Idle; break; }
            for (u32 b = 0; b < kCapBases; ++b) {
                if (h != kCapBaseFrame[b]) continue;
                captureCopy(ctx, b, in.radiance, kRead);
                cap_.baseFlags[b] = (frame.radianceHalfRate ? 1u : 0u) | ((frame.radianceHalfRateParity & 1u) << 1);
            }
            const u32 first = kCapBaseFrame[kCapBases - 1];
            if (h == first) {
                captureCopy(ctx, kCapBases, in.viewZ, kRead);
                captureCopy(ctx, kCapBases + 1, in.normalRoughness, kRead);
            }
            if (h >= first) {
                SignalTargets& t = sig_[static_cast<u32>(Signal::Radiance)];
                rhi::BindingSetHandle set = t.sets[NrdCapture];
                res_->setSrv(set, 0, in.viewZ);
                res_->setSrv(set, 1, in.normalRoughness);
                res_->setSrv(set, 2, in.motionVectors);
                res_->setSrv(set, 3, in.radiance);
                res_->setUav(set, 0, cap_.accum, 0);
                Constants cb{};
                cb.size[0] = width_; cb.size[1] = height_;
                cb.invSize[0] = 1.0f / static_cast<f32>(width_); cb.invSize[1] = 1.0f / static_cast<f32>(height_);
                cb.flags = (h == first ? kFlagCaptureReset : 0u) |
                           (frame.radianceHalfRate ? kFlagHalfRate : 0u) |
                           ((frame.radianceHalfRate && (frame.radianceHalfRateParity & 1u)) ? kFlagHalfRateOdd : 0u);
                ctx.textureBarrier(cap_.accum, kRead, rhi::ResourceState::UnorderedAccess);
                ctx.setPipeline(pipelines_[NrdCapture * 2]);
                ctx.setBindingSet(set);
                ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
                ctx.dispatch((width_ + 7u) / 8u, (height_ + 7u) / 8u, 1);
                ctx.textureBarrier(cap_.accum, rhi::ResourceState::UnorderedAccess, kRead);
            }
            if (h >= first + kCapMeanFrames - 1) {
                captureCopy(ctx, kCapBases + 2, cap_.accum, kRead);
                cap_.state = CapState::Readback;
                cap_.frame = 0;
            }
            break;
        }
        case CapState::Readback:
            if (++cap_.frame >= kCapReadbackDelay) {
                captureWrite();
                ++cap_.index;
                cap_.frame = 0;
                cap_.state = --cap_.remaining ? CapState::Travel : CapState::Idle;
                if (cap_.state == CapState::Idle) AVER_INFO("[Denoise] NRD training capture finished");
            }
            break;
        default:
            break;
    }
}

}  // namespace aver::render::denoise
