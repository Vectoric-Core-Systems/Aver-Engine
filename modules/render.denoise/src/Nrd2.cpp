#include "aver/render/denoise/Nrd2.hpp"

#include "aver/core/Log.hpp"
#include "aver/render/denoise/Nrd2Capture.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <cmath>
#include <cstring>
#include <string>

namespace aver::render::denoise {

namespace {

constexpr u32 kConstantSlot = 3;   // b3 (modules/render.neural/README.md: why not b1)

// nrd2.hlsl's Nrd2CB, byte for byte.
struct Constants {
    u32 rect[4];
    u32 tiles[4];   // x, y, flags, unused
    f32 def[12];
    f32 view[12];   // world -> view rows: right, up, forward (xyz, w unused)
};
static_assert(sizeof(Constants) == 128, "Nrd2CB: two uint4s, six float4s");

constexpr u32 kFlagBypass = 1u;

// Per pass: SRV / UAV counts matching nrd2.hlsl's register lists.
constexpr u32 kPyramidSrv = 4, kPyramidUav = 9;
constexpr u32 kResolveSrv = 16, kResolveUav = 1;
constexpr u32 kResolveParamsSrv = 15;

constexpr rhi::ResourceState kRead  = rhi::ResourceState::NonPixelShaderResource;
constexpr rhi::ResourceState kWrite = rhi::ResourceState::UnorderedAccess;

u32 tilesOf(u32 d) { return (d + 7u) / 8u; }

// The view basis from the camera's camera-relative inverse view-projection (row vectors): the eye is
// the origin, so the unprojected viewport centre is forward and the edges give right and up.
void viewBasis(rhi::IDevice* dev, f32 out[12]) {
    std::memset(out, 0, 12 * sizeof(f32));
    out[0] = 1.0f; out[5] = 1.0f; out[10] = 1.0f;
    f32 inv[16] = {};
    if (!dev || !dev->camera(nullptr, inv, nullptr)) return;
    auto unproject = [&](f32 x, f32 y, f32 r[3]) {
        const f32 v[4] = {x, y, 0.5f, 1.0f};
        f32 h[4] = {};
        for (u32 c = 0; c < 4; ++c) for (u32 k = 0; k < 4; ++k) h[c] += v[k] * inv[k * 4 + c];
        const f32 w = std::fabs(h[3]) > 1e-20f ? h[3] : 1e-20f;
        for (u32 c = 0; c < 3; ++c) r[c] = h[c] / w;
    };
    f32 c[3], xp[3], xm[3], yp[3], ym[3];
    unproject(0, 0, c); unproject(1, 0, xp); unproject(-1, 0, xm); unproject(0, 1, yp); unproject(0, -1, ym);
    const f32 axes[3][3] = {{xp[0] - xm[0], xp[1] - xm[1], xp[2] - xm[2]},
                            {yp[0] - ym[0], yp[1] - ym[1], yp[2] - ym[2]},
                            {c[0], c[1], c[2]}};
    f32 basis[12] = {};
    for (u32 a = 0; a < 3; ++a) {
        const f32 l = std::sqrt(axes[a][0] * axes[a][0] + axes[a][1] * axes[a][1] + axes[a][2] * axes[a][2]);
        if (!(l > 1e-20f) || !std::isfinite(l)) return;   // identity rather than a NaN basis
        for (u32 k = 0; k < 3; ++k) basis[a * 4 + k] = axes[a][k] / l;
    }
    std::memcpy(out, basis, sizeof(basis));
}

}  // namespace

Nrd2::~Nrd2() { destroy(); }

bool Nrd2::create(rhi::IDevice& dev) {
    destroy();
    dev_ = &dev;
    res_ = dev.resources();
    if (!res_) { dev_ = nullptr; return false; }
    const std::string& source = rhi::shaderFile("nrd2.hlsl");
    if (source.empty()) {
        AVER_WARN("[NRD2] nrd2.hlsl is not deployed beside the executable; NRD2 unavailable");
        destroy();
        return false;
    }
    auto build = [&](u32 pass, const char* entry, u32 srv, u32 uav, u32 bufferSrv, bool bufferUav) {
        const std::string defines = "AVER_NRD2_PASS=" + std::to_string(pass);
        rhi::ShaderDesc sd{};
        sd.source = source.c_str();
        sd.entry = entry;
        sd.stage = rhi::ShaderStage::Compute;
        sd.minShaderModel = 60;
        sd.defines = defines.c_str();
        const rhi::ShaderHandle cs = res_->createShader(sd);
        if (!cs) { AVER_WARN("[NRD2] {} would not compile; NRD2 unavailable", entry); return rhi::PipelineHandle(0); }
        rhi::ComputePipelineDesc pd{};
        pd.cs = cs;
        pd.layout.srvCount = srv;
        pd.layout.uavCount = uav;
        pd.layout.slotKindsDeclared = true;
        if (bufferSrv < srv) pd.layout.srvKinds[bufferSrv] = rhi::SlotKind::StructuredBuffer;
        if (bufferUav) pd.layout.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
        pd.layout.constantDwords[kConstantSlot] = 0;   // root CBV
        const rhi::PipelineHandle p = res_->createComputePipeline(pd);
        res_->destroyShader(cs);
        if (!p) AVER_WARN("[NRD2] the {} pipeline would not build; NRD2 unavailable", entry);
        return p;
    };
    psoPyramid_ = build(0, "CSNrd2Pyramid", kPyramidSrv, kPyramidUav, ~0u, false);
    psoParams_  = build(1, "CSNrd2Params", 0, 1, ~0u, true);
    psoResolve_ = build(2, "CSNrd2Resolve", kResolveSrv, kResolveUav, kResolveParamsSrv, false);
    if (!valid()) { destroy(); return false; }

    rhi::BindingSetDesc bd{};
    bd.srvCount = kPyramidSrv; bd.uavCount = kPyramidUav;
    setPyramid_ = res_->createBindingSet(bd);
    bd = {};
    bd.uavCount = 1; bd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    setParams_ = res_->createBindingSet(bd);
    bd = {};
    bd.srvCount = kResolveSrv; bd.uavCount = kResolveUav;
    bd.srvKinds[kResolveParamsSrv] = rhi::SlotKind::StructuredBuffer;
    setResolve_ = res_->createBindingSet(bd);
    bd = {};
    bd.srvCount = 1;
    setCompose_ = res_->createBindingSet(bd);
    if (!setPyramid_ || !setParams_ || !setResolve_ || !setCompose_) {
        AVER_WARN("[NRD2] binding sets could not be created; NRD2 unavailable");
        destroy();
        return false;
    }
    AVER_INFO("[NRD2] single-frame denoiser pipelines built (pyramid, tile parameters, resolve)");
    return true;
}

bool Nrd2::createCompose(rhi::Format color, const rhi::Format gbuffer[3], rhi::Format depth, u32 sampleCount) {
    destroyCompose();
    if (!res_) return false;
    const std::string& source = rhi::shaderFile("nrd2.hlsl");
    if (source.empty()) return false;
    rhi::ShaderDesc sd{};
    sd.source = source.c_str();
    sd.minShaderModel = 60;
    sd.defines = "AVER_NRD2_PASS=3";
    sd.entry = "VSNrd2Compose";
    sd.stage = rhi::ShaderStage::Vertex;
    const rhi::ShaderHandle vs = res_->createShader(sd);
    sd.entry = "PSNrd2Compose";
    sd.stage = rhi::ShaderStage::Pixel;
    const rhi::ShaderHandle ps = res_->createShader(sd);
    if (vs && ps) {
        rhi::GraphicsPipelineDesc p{};
        p.vs = vs; p.ps = ps;
        p.layout.srvCount = 1;
        p.layout.slotKindsDeclared = true;
        p.cull = rhi::CullMode::None;
        p.depth = {false, false, rhi::CompareOp::Always};
        // The backend masks targets 1..3 of a blended pipeline: only the colour is added to.
        p.blend = rhi::BlendMode::Additive;
        p.renderTargetCount = 4;
        p.renderTargets[0] = color;
        for (u32 i = 0; i < 3; ++i) p.renderTargets[1 + i] = gbuffer[i];
        p.depthFormat = depth;
        p.sampleCount = sampleCount;
        compose_ = res_->createGraphicsPipeline(p);
    }
    if (vs) res_->destroyShader(vs);
    if (ps) res_->destroyShader(ps);
    if (!compose_) AVER_WARN("[NRD2] the compose pipeline would not build; NRD2 unavailable");
    return compose_ != 0;
}

void Nrd2::destroyCompose() {
    if (res_ && compose_) res_->destroyPipeline(compose_);
    compose_ = 0;
}

void Nrd2::releaseTargets() {
    if (!res_) return;
    network_.invalidateBindings();
    auto drop = [&](rhi::TextureHandle& t) { if (t) res_->destroyTexture(t); t = 0; };
    drop(targets_.diffuse); drop(targets_.specular); drop(targets_.remodA); drop(targets_.remodB);
    for (u32 l = 0; l < 3; ++l) { drop(guide_[l]); drop(levelD_[l]); drop(levelS_[l]); }
    drop(lit_);
    if (tileParams_) res_->destroyBuffer(tileParams_);
    if (features_) res_->destroyBuffer(features_);
    tileParams_ = features_ = 0;
    tileCapacity_ = featureFloats_ = 0;
    width_ = height_ = 0;
    recorded_ = false;
}

void Nrd2::destroy() {
    if (capture_) capture_->release();
    capture_.reset();
    if (dev_ && jitterSuppressed_) dev_->setJitterSuppressed(false);
    jitterSuppressed_ = false;
    releaseTargets();
    destroyCompose();
    network_.destroy();
    if (res_) {
        for (rhi::PipelineHandle* p : {&psoPyramid_, &psoParams_, &psoResolve_, &psoFeatures_}) { if (*p) res_->destroyPipeline(*p); }
        for (rhi::BindingSetHandle* s : {&setPyramid_, &setParams_, &setResolve_, &setCompose_, &setFeatures_}) {
            if (*s) res_->destroyBindingSet(*s);
        }
    }
    psoPyramid_ = psoParams_ = psoResolve_ = psoFeatures_ = 0;
    setPyramid_ = setParams_ = setResolve_ = setCompose_ = setFeatures_ = 0;
    featuresTried_ = false;
    dev_ = nullptr;
    res_ = nullptr;
}

bool Nrd2::resize(u32 width, u32 height) {
    if (!valid() || width == 0 || height == 0) return false;
    if (width == width_ && height == height_ && lit_) return true;
    if (width == failedWidth_ && height == failedHeight_) return false;
    releaseTargets();
    bool ok = true;
    auto make = [&](rhi::Format f, u32 w, u32 h, rhi::ResourceState state, const char* name) {
        rhi::TextureDesc d{};
        d.width = w; d.height = h;
        d.format = f;
        d.bind = static_cast<rhi::ResourceBind>(static_cast<u32>(rhi::ResourceBind::ShaderResource) |
                                                static_cast<u32>(rhi::ResourceBind::UnorderedAccess));
        d.initialState = state;
        d.debugName = name;
        const rhi::TextureHandle t = res_->createTexture(d);
        if (!t) { AVER_WARN("[NRD2] {} failed to allocate at {}x{}", name, w, h); ok = false; }
        return t;
    };
    // Stage B's targets rest as UAVs (Voxi binds them in its table); everything else rests readable.
    targets_.diffuse  = make(rhi::Format::RGBA16F, width, height, kWrite, "NRD2 diffuse (D)");
    targets_.specular = make(rhi::Format::RGBA16F, width, height, kWrite, "NRD2 specular (S)");
    targets_.remodA   = make(rhi::Format::RGBA16F, width, height, kWrite, "NRD2 remodulation A");
    targets_.remodB   = make(rhi::Format::RG16F,   width, height, kWrite, "NRD2 remodulation B");
    static const char* kLevel[3][3] = {{"NRD2 guide 1/2", "NRD2 guide 1/4", "NRD2 guide 1/8"},
                                       {"NRD2 D 1/2", "NRD2 D 1/4", "NRD2 D 1/8"},
                                       {"NRD2 S 1/2", "NRD2 S 1/4", "NRD2 S 1/8"}};
    for (u32 l = 0; l < 3; ++l) {
        // Whole 8x8 groups, so a group's coarse texels always land inside (nrd2LevelSize).
        const u32 w = tilesOf(width) * (4u >> l), h = tilesOf(height) * (4u >> l);
        guide_[l]  = make(rhi::Format::RGBA16F, w, h, kRead, kLevel[0][l]);
        levelD_[l] = make(rhi::Format::RGBA16F, w, h, kRead, kLevel[1][l]);
        levelS_[l] = make(rhi::Format::RGBA16F, w, h, kRead, kLevel[2][l]);
    }
    lit_ = make(rhi::Format::RGBA16F, width, height, kRead, "NRD2 denoised lighting");
    tileCapacity_ = tilesOf(width) * tilesOf(height);
    rhi::BufferDesc bd{};
    bd.bytes = static_cast<u64>(tileCapacity_) * kNrd2TileParams * sizeof(f32);
    bd.kind = rhi::BufferKind::Default;
    bd.allowUnorderedAccess = true;
    bd.debugName = "NRD2 tile parameters";
    tileParams_ = res_->createBuffer(bd);
    if (!tileParams_) ok = false;
    if (!ok) {
        releaseTargets();
        failedWidth_ = width; failedHeight_ = height;
        return false;
    }
    failedWidth_ = failedHeight_ = 0;
    width_ = width; height_ = height;
    const f64 px = static_cast<f64>(width) * height;
    AVER_INFO("[NRD2] targets at {}x{}: {:.1f} MiB", width, height,
              px * (8 + 8 + 8 + 4 + 8 + 3 * 8 * (0.25 + 0.0625 + 0.015625)) / (1024.0 * 1024.0));
    return true;
}

bool Nrd2::record(rhi::IRenderContext& ctx, const Inputs& in) {
    recorded_ = false;
    if (!valid() || !lit_ || !in.viewZ || !in.normalRoughness) return false;
    const u32 vx = in.viewport[0], vy = in.viewport[1];
    const u32 vw = in.viewport[2], vh = in.viewport[3];
    if (!vw || !vh || vx + vw > width_ || vy + vh > height_) return false;

    rhi::ScopedGpuStat stat(ctx, "NRD2");
    // Descriptors first, every set, before any dispatch (Denoiser.cpp: a set rewritten between two
    // dispatches that use it would leave the first reading the second's resources).
    res_->setSrv(setPyramid_, 0, targets_.diffuse);
    res_->setSrv(setPyramid_, 1, targets_.specular);
    res_->setSrv(setPyramid_, 2, in.viewZ);
    res_->setSrv(setPyramid_, 3, in.normalRoughness);
    for (u32 l = 0; l < 3; ++l) {
        res_->setUav(setPyramid_, l, guide_[l], 0);
        res_->setUav(setPyramid_, 3 + l, levelD_[l], 0);
        res_->setUav(setPyramid_, 6 + l, levelS_[l], 0);
    }
    const u32 tx = tilesOf(vw), ty = tilesOf(vh), tiles = tx * ty;
    res_->setUavBuffer(setParams_, 0, tileParams_, sizeof(f32), tileCapacity_ * kNrd2TileParams, 0);
    res_->setSrv(setResolve_, 0, targets_.diffuse);
    res_->setSrv(setResolve_, 1, targets_.specular);
    res_->setSrv(setResolve_, 2, in.viewZ);
    res_->setSrv(setResolve_, 3, in.normalRoughness);
    res_->setSrv(setResolve_, 4, targets_.remodA);
    res_->setSrv(setResolve_, 5, targets_.remodB);
    for (u32 l = 0; l < 3; ++l) {
        res_->setSrv(setResolve_, 6 + l, guide_[l]);
        res_->setSrv(setResolve_, 9 + l, levelD_[l]);
        res_->setSrv(setResolve_, 12 + l, levelS_[l]);
    }
    res_->setSrvBuffer(setResolve_, kResolveParamsSrv, tileParams_, sizeof(f32), tileCapacity_ * kNrd2TileParams, 0);
    res_->setUav(setResolve_, 0, lit_, 0);
    res_->setSrv(setCompose_, 0, lit_);

    Constants cb{};
    cb.rect[0] = vx; cb.rect[1] = vy; cb.rect[2] = vw; cb.rect[3] = vh;
    cb.tiles[0] = tx; cb.tiles[1] = ty;
    cb.tiles[2] = params_.bypass ? kFlagBypass : 0u;
    std::memcpy(cb.def, params_.diffuse, sizeof(params_.diffuse));
    std::memcpy(cb.def + 6, params_.specular, sizeof(params_.specular));
    viewBasis(dev_, cb.view);

    const rhi::TextureHandle stageB[4] = {targets_.diffuse, targets_.specular, targets_.remodA, targets_.remodB};
    for (rhi::TextureHandle t : stageB) ctx.textureBarrier(t, kWrite, kRead);
    ctx.textureBarrier(in.viewZ, in.gbufferState, kRead);
    ctx.textureBarrier(in.normalRoughness, in.gbufferState, kRead);

    const u32 gx = (vw + 7u) / 8u, gy = (vh + 7u) / 8u;
    for (u32 l = 0; l < 3; ++l) {
        ctx.textureBarrier(guide_[l], kRead, kWrite);
        ctx.textureBarrier(levelD_[l], kRead, kWrite);
        ctx.textureBarrier(levelS_[l], kRead, kWrite);
    }
    ctx.setPipeline(psoPyramid_);
    ctx.setBindingSet(setPyramid_);
    ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
    ctx.dispatch(gx, gy, 1);
    for (u32 l = 0; l < 3; ++l) {
        ctx.textureBarrier(guide_[l], kWrite, kRead);
        ctx.textureBarrier(levelD_[l], kWrite, kRead);
        ctx.textureBarrier(levelS_[l], kWrite, kRead);
    }

    // Tile parameters: the network's when it can (its features from this frame's pyramid), else the defaults.
    bool net = false;
    if (params_.network && !params_.bypass) {
        if (network_.ready(*dev_)) {
            if (recordFeatures(ctx, in))
                net = network_.record(ctx, features_, featureFloats_, tileParams_, tileCapacity_ * kNrd2TileParams, tx,
                                      ty, cb.def);
            else
                network_.markIdle("the feature pass would not build");
        }
    } else {
        network_.markIdle(params_.bypass ? "bypass" : "off (voxi.nrd2Network 0)");
    }
    if (!net) {
        ctx.bufferBarrier(tileParams_, rhi::ResourceState::Common, kWrite);
        ctx.setPipeline(psoParams_);
        ctx.setBindingSet(setParams_);
        ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
        ctx.dispatch((tiles + 63u) / 64u, 1, 1);
        ctx.bufferBarrier(tileParams_, kWrite, rhi::ResourceState::Common);
    }
    ctx.bufferBarrier(tileParams_, rhi::ResourceState::Common, kRead);

    ctx.textureBarrier(lit_, kRead, kWrite);
    ctx.setPipeline(psoResolve_);
    ctx.setBindingSet(setResolve_);
    ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
    ctx.dispatch(gx, gy, 1);
    ctx.textureBarrier(lit_, kWrite, rhi::ResourceState::ShaderResource);
    ctx.bufferBarrier(tileParams_, kRead, rhi::ResourceState::Common);

    // Phase 3 capture: everything it reads is readable here. Jitter off while it holds a pose, from
    // the next frame's upload.
    if (capture_ && capture_->active()) capture_->step(ctx, *this, in);
    const bool hold = capture_ && capture_->holding();
    if (hold != jitterSuppressed_) { dev_->setJitterSuppressed(hold); jitterSuppressed_ = hold; }

    ctx.textureBarrier(in.viewZ, kRead, in.gbufferState);
    ctx.textureBarrier(in.normalRoughness, kRead, in.gbufferState);
    for (rhi::TextureHandle t : stageB) ctx.textureBarrier(t, kRead, kWrite);
    recorded_ = true;
    return true;
}

void Nrd2::recordCompose(rhi::IRenderContext& ctx) {
    if (recorded_ && compose_) {
        rhi::ScopedGpuStat stat(ctx, "NRD2 compose");
        ctx.setPipeline(compose_);
        ctx.setBindingSet(setCompose_);
        ctx.drawFullscreen();
    }
    if (recorded_) ctx.textureBarrier(lit_, rhi::ResourceState::ShaderResource, kRead);
    recorded_ = false;
}

bool Nrd2::recordFeatures(rhi::IRenderContext& ctx, const Inputs& in) {
    if (!featuresTried_) {
        featuresTried_ = true;
        const std::string& source = rhi::shaderFile("nrd2.hlsl");
        rhi::ShaderDesc sd{};
        sd.source = source.c_str();
        sd.entry = "CSNrd2Features";
        sd.stage = rhi::ShaderStage::Compute;
        sd.minShaderModel = 60;
        sd.defines = "AVER_NRD2_PASS=4";
        const rhi::ShaderHandle cs = source.empty() ? rhi::ShaderHandle(0) : res_->createShader(sd);
        if (cs) {
            rhi::ComputePipelineDesc pd{};
            pd.cs = cs;
            pd.layout.srvCount = 8;
            pd.layout.uavCount = 1;
            pd.layout.slotKindsDeclared = true;
            pd.layout.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
            pd.layout.constantDwords[kConstantSlot] = 0;
            psoFeatures_ = res_->createComputePipeline(pd);
            res_->destroyShader(cs);
        }
        rhi::BindingSetDesc bd{};
        bd.srvCount = 8; bd.uavCount = 1; bd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
        if (psoFeatures_) setFeatures_ = res_->createBindingSet(bd);
        if (!psoFeatures_ || !setFeatures_) AVER_WARN("[NRD2] CSNrd2Features would not build; no network inputs");
    }
    if (!psoFeatures_ || !setFeatures_) return false;
    const u32 tx = tilesOf(in.viewport[2]), ty = tilesOf(in.viewport[3]);
    if (tx * ty * 16u * 12u > featureFloats_) {
        network_.invalidateBindings();
        if (features_) res_->destroyBuffer(features_);
        rhi::BufferDesc bd{};
        bd.bytes = static_cast<u64>(tileCapacity_ > tx * ty ? tileCapacity_ : tx * ty) * 16u * 12u * sizeof(f32);
        bd.kind = rhi::BufferKind::Default;
        bd.allowUnorderedAccess = true;
        bd.debugName = "NRD2 features";
        features_ = res_->createBuffer(bd);
        featureFloats_ = features_ ? static_cast<u32>(bd.bytes / sizeof(f32)) : 0u;
        if (!features_) return false;
    }
    res_->setSrv(setFeatures_, 0, targets_.diffuse);
    res_->setSrv(setFeatures_, 1, targets_.specular);
    res_->setSrv(setFeatures_, 2, in.viewZ);
    res_->setSrv(setFeatures_, 3, in.normalRoughness);
    res_->setSrv(setFeatures_, 4, targets_.remodA);
    res_->setSrv(setFeatures_, 5, guide_[2]);
    res_->setSrv(setFeatures_, 6, levelD_[2]);
    res_->setSrv(setFeatures_, 7, levelS_[2]);
    res_->setUavBuffer(setFeatures_, 0, features_, sizeof(f32), featureFloats_, 0);
    Constants cb{};
    for (u32 a = 0; a < 4; ++a) cb.rect[a] = in.viewport[a];
    cb.tiles[0] = tx; cb.tiles[1] = ty;
    viewBasis(dev_, cb.view);
    rhi::ScopedGpuStat stat(ctx, "NRD2.Features");
    ctx.bufferBarrier(features_, rhi::ResourceState::Common, kWrite);
    ctx.setPipeline(psoFeatures_);
    ctx.setBindingSet(setFeatures_);
    ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
    ctx.dispatch((tx * 4u + 7u) / 8u, (ty * 4u + 7u) / 8u, 1);
    ctx.bufferBarrier(features_, kWrite, rhi::ResourceState::Common);
    return true;
}

void Nrd2::startCapture(const Nrd2CaptureConfig& cfg) {
    if (!dev_) { AVER_WARN("[NRD2] capture requested before NRD2 was created; ignored"); return; }
    if (!capture_) capture_ = std::make_unique<Nrd2Capture>(*dev_);
    capture_->start(cfg);
}

bool Nrd2::captureActive() const { return capture_ && capture_->active(); }
bool Nrd2::captureHolding() const { return capture_ && capture_->holding(); }

}  // namespace aver::render::denoise
