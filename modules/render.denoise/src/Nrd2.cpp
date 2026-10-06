#include "aver/render/denoise/Nrd2.hpp"

#include "aver/core/Log.hpp"
#include "aver/render/denoise/Nrd2Capture.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <initializer_list>
#include <string>

namespace aver::render::denoise {

namespace {

constexpr u32 kConstantSlot = 3;   // b3 (modules/render.neural/README.md: why not b1)

// nrd2.hlsl's Nrd2CB, byte for byte.
struct Constants {
    u32 rect[4];
    u32 tiles[4];   // x, y, flags, despeckle mask
    f32 def[12];
    f32 view[12];   // world -> view rows: right, up, forward (xyz); w of right and up: tan of the half FOV
    f32 inScale[12], inBias[12];   // the network's input standardisation (kFlagStandardise)
    f32 prevVP[16];                // previous view-projection about the previous eye, rows
    f32 camDelta[4];               // eye - previous eye
    f32 stab[4];                   // history frames at rest, cap at speed, despeckle cap
};
static_assert(sizeof(Constants) == 320, "Nrd2CB: two uint4s, eighteen float4s");

constexpr u32 kFlagBypass = 1u, kFlagStabilise = 2u, kFlagHistory = 4u, kFlagStandardise = 8u;

// Per pass: SRV / UAV counts matching nrd2.hlsl's register lists.
constexpr u32 kPyramidSrv = 4, kPyramidUav = 9;
constexpr u32 kResolveSrv = 16, kResolveUav = 3;
constexpr u32 kResolveParamsSrv = 15;
constexpr u32 kReprojectSrv = 9, kReprojectUav = 5;
constexpr u32 kPrefilterSrv = 7, kPrefilterUav = 2;
constexpr u32 kTemporalSrv = 10, kTemporalUav = 5;
constexpr u32 kDespeckleSrv = 3, kDespeckleUav = 2;
constexpr f32 kStabNFast = 8.0f, kStabNSunMoved = 2.0f, kStabNMax = 64.0f;

constexpr rhi::ResourceState kRead  = rhi::ResourceState::NonPixelShaderResource;
constexpr rhi::ResourceState kWrite = rhi::ResourceState::UnorderedAccess;

u32 tilesOf(u32 d) { return (d + 7u) / 8u; }

rhi::TextureHandle makeTexture(rhi::IResourceFactory* res, rhi::Format f, u32 w, u32 h, rhi::ResourceState state,
                               const char* name) {
    rhi::TextureDesc d{};
    d.width = w; d.height = h;
    d.format = f;
    d.bind = static_cast<rhi::ResourceBind>(static_cast<u32>(rhi::ResourceBind::ShaderResource) |
                                            static_cast<u32>(rhi::ResourceBind::UnorderedAccess));
    d.initialState = state;
    d.debugName = name;
    const rhi::TextureHandle t = res->createTexture(d);
    if (!t) AVER_WARN("[NRD2] {} failed to allocate at {}x{}", name, w, h);
    return t;
}

// The view basis from the camera's camera-relative inverse view-projection (row vectors): the eye is
// the origin, so the unprojected viewport centre is forward and the edges give right and up. The w of
// right and up carry tan of the half FOV (a symmetric perspective frustum). False: identity, no FOV.
bool viewBasis(rhi::IDevice* dev, f32 out[12]) {
    std::memset(out, 0, 12 * sizeof(f32));
    out[0] = 1.0f; out[5] = 1.0f; out[10] = 1.0f;
    f32 inv[16] = {};
    if (!dev || !dev->camera(nullptr, inv, nullptr)) return false;
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
    f32 basis[12] = {}, len[3] = {};
    for (u32 a = 0; a < 3; ++a) {
        const f32 l = std::sqrt(axes[a][0] * axes[a][0] + axes[a][1] * axes[a][1] + axes[a][2] * axes[a][2]);
        if (!(l > 1e-20f) || !std::isfinite(l)) return false;   // identity rather than a NaN basis
        for (u32 k = 0; k < 3; ++k) basis[a * 4 + k] = axes[a][k] / l;
        len[a] = l;
    }
    // Both edge points sit at the centre's view depth, so half their separation over it is the tangent.
    basis[3] = 0.5f * len[0] / len[2];
    basis[7] = 0.5f * len[1] / len[2];
    std::memcpy(out, basis, sizeof(basis));
    return true;
}

// The previous view-projection (row-major, row vectors, absolute world) as the stabiliser's rows about the
// previous eye: clip = (offset from the eye, 1) * rows. Folded in double, the absolute matrix cancels
// badly far from the origin. False for an unfilled (zero) or non-finite matrix.
bool prevViewProjRel(const f32 m[16], const f32 eye[3], f32 out[16]) {
    bool any = false;
    for (u32 i = 0; i < 16; ++i) { if (!std::isfinite(m[i])) return false; any = any || m[i] != 0.0f; }
    if (!any) return false;
    std::memcpy(out, m, 12 * sizeof(f32));
    for (u32 j = 0; j < 4; ++j) {
        f64 t = m[12 + j];
        for (u32 k = 0; k < 3; ++k) t += static_cast<f64>(eye[k]) * static_cast<f64>(m[k * 4 + j]);
        out[12 + j] = static_cast<f32>(t);
        if (!std::isfinite(out[12 + j])) return false;
    }
    return true;
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
        if (!cs) { AVER_WARN("[NRD2] {} would not compile", entry); return rhi::PipelineHandle(0); }
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
        if (!p) AVER_WARN("[NRD2] the {} pipeline would not build", entry);
        return p;
    };
    psoPyramid_ = build(0, "CSNrd2Pyramid", kPyramidSrv, kPyramidUav, ~0u, false);
    psoParams_  = build(1, "CSNrd2Params", 0, 1, ~0u, true);
    psoResolve_ = build(2, "CSNrd2Resolve", kResolveSrv, kResolveUav, kResolveParamsSrv, false);
    if (!valid()) { destroy(); return false; }
    // Optional: without them NRD2 stays single-frame.
    psoReproject_ = build(5, "CSNrd2Reproject", kReprojectSrv, kReprojectUav, ~0u, false);
    psoPrefilter_ = build(6, "CSNrd2Prefilter", kPrefilterSrv, kPrefilterUav, ~0u, false);
    psoTemporal_  = build(7, "CSNrd2Temporal", kTemporalSrv, kTemporalUav, ~0u, false);
    psoDespeckle_ = build(8, "CSNrd2Despeckle", kDespeckleSrv, kDespeckleUav, ~0u, false);

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
    if (psoReproject_ && psoPrefilter_ && psoTemporal_) {
        bd = {};
        bd.srvCount = kReprojectSrv; bd.uavCount = kReprojectUav;
        setReproject_ = res_->createBindingSet(bd);
        bd = {};
        bd.srvCount = kPrefilterSrv; bd.uavCount = kPrefilterUav;
        setPrefilter_ = res_->createBindingSet(bd);
        bd = {};
        bd.srvCount = kTemporalSrv; bd.uavCount = kTemporalUav;
        setTemporal_ = res_->createBindingSet(bd);
        if (!(setReproject_ && setPrefilter_ && setTemporal_))
            AVER_WARN("[NRD2] the temporal stage's binding sets could not be created; single-frame only");
    }
    if (psoDespeckle_) {
        bd = {};
        bd.srvCount = kDespeckleSrv; bd.uavCount = kDespeckleUav;
        setDespeckle_ = res_->createBindingSet(bd);
    }
    if (!setPyramid_ || !setParams_ || !setResolve_ || !setCompose_) {
        AVER_WARN("[NRD2] binding sets could not be created; NRD2 unavailable");
        destroy();
        return false;
    }
    AVER_INFO("[NRD2] denoiser pipelines built (pyramid, tile parameters, resolve{})",
              stabReady() ? ", temporal stage" : "");
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

bool Nrd2::stabReady() const {
    return psoReproject_ && psoPrefilter_ && psoTemporal_ && setReproject_ && setPrefilter_ && setTemporal_;
}

void Nrd2::releaseStab() {
    histValid_ = false;
    if (!res_) return;
    for (rhi::TextureHandle* t : {&dRes_, &sRes_, &rpD_, &rpS_, &rpV_, &anchD_, &anchS_, &prefD_, &prefS_,
                                  &histD_[0], &histD_[1], &histS_[0], &histS_[1], &histG_[0], &histG_[1],
                                  &histV_[0], &histV_[1]}) {
        if (*t) res_->destroyTexture(*t);
        *t = 0;
    }
}

bool Nrd2::allocStab() {
    if (dRes_) return true;
    const rhi::Format f = rhi::Format::RGBA16F, f2 = rhi::Format::RG16F;
    bool ok = true;
    auto make = [&](rhi::Format fmt, u32 w, u32 h, rhi::ResourceState state, const char* name) {
        const rhi::TextureHandle t = makeTexture(res_, fmt, w, h, state, name);
        if (!t) ok = false;
        return t;
    };
    // D' and S' rest as UAVs (the resolve writes them, the temporal stage reads them in between); everything
    // else rests readable. The anchors are one texel per 8x8 tile.
    dRes_ = make(f, width_, height_, kWrite, "NRD2 resolved D");
    sRes_ = make(f, width_, height_, kWrite, "NRD2 resolved S");
    rpD_ = make(f, width_, height_, kRead, "NRD2 reprojected D");
    rpS_ = make(f, width_, height_, kRead, "NRD2 reprojected S");
    rpV_ = make(f2, width_, height_, kRead, "NRD2 reprojected noise");
    anchD_ = make(f, tilesOf(width_), tilesOf(height_), kRead, "NRD2 anchor D");
    anchS_ = make(f, tilesOf(width_), tilesOf(height_), kRead, "NRD2 anchor S");
    prefD_ = make(f, width_, height_, kRead, "NRD2 prefiltered D");
    prefS_ = make(f, width_, height_, kRead, "NRD2 prefiltered S");
    for (u32 i = 0; i < 2; ++i) {
        histD_[i] = make(f, width_, height_, kRead, "NRD2 history D");
        histS_[i] = make(f, width_, height_, kRead, "NRD2 history S");
        histG_[i] = make(f, width_, height_, kRead, "NRD2 history Z and normal");
        histV_[i] = make(f2, width_, height_, kRead, "NRD2 history noise");
    }
    if (!ok) {
        releaseStab();
        return false;
    }
    histValid_ = false;
    AVER_INFO("[NRD2] temporal stage targets at {}x{}: {:.1f} MiB", width_, height_,
              static_cast<f64>(width_) * height_ * 108 / (1024.0 * 1024.0));
    return true;
}

bool Nrd2::allocDespeckle() {
    if (despD_ && despS_) return true;
    despD_ = makeTexture(res_, rhi::Format::RGBA16F, width_, height_, kRead, "NRD2 despeckled D");
    despS_ = makeTexture(res_, rhi::Format::RGBA16F, width_, height_, kRead, "NRD2 despeckled S");
    if (despD_ && despS_) return true;
    if (despD_) res_->destroyTexture(despD_);
    if (despS_) res_->destroyTexture(despS_);
    despD_ = despS_ = 0;
    return false;
}

void Nrd2::releaseTargets() {
    if (!res_) return;
    network_.invalidateBindings();
    releaseStab();
    auto drop = [&](rhi::TextureHandle& t) { if (t) res_->destroyTexture(t); t = 0; };
    drop(targets_.diffuse); drop(targets_.specular); drop(targets_.remodA); drop(targets_.remodB);
    for (u32 l = 0; l < 3; ++l) { drop(guide_[l]); drop(levelD_[l]); drop(levelS_[l]); }
    drop(lit_);
    drop(despD_); drop(despS_);
    despeckled_ = false;
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
        for (rhi::PipelineHandle* p : {&psoPyramid_, &psoParams_, &psoResolve_, &psoFeatures_, &psoReproject_,
                                       &psoPrefilter_, &psoTemporal_, &psoDespeckle_}) {
            if (*p) res_->destroyPipeline(*p);
        }
        for (rhi::BindingSetHandle* s : {&setPyramid_, &setParams_, &setResolve_, &setCompose_, &setFeatures_,
                                         &setReproject_, &setPrefilter_, &setTemporal_, &setDespeckle_}) {
            if (*s) res_->destroyBindingSet(*s);
        }
    }
    psoPyramid_ = psoParams_ = psoResolve_ = psoFeatures_ = psoReproject_ = psoPrefilter_ = psoTemporal_ = 0;
    psoDespeckle_ = 0;
    setPyramid_ = setParams_ = setResolve_ = setCompose_ = setFeatures_ = 0;
    setReproject_ = setPrefilter_ = setTemporal_ = setDespeckle_ = 0;
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
        const rhi::TextureHandle t = makeTexture(res_, f, w, h, state, name);
        if (!t) ok = false;
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
    // History survives only a frame that stabilised on the same viewport (set again below).
    const bool hadHist = histValid_ && std::memcmp(histViewport_, in.viewport, sizeof(histViewport_)) == 0;
    histValid_ = false;
    if (!valid() || !lit_ || !in.viewZ || !in.normalRoughness) return false;
    const u32 vx = in.viewport[0], vy = in.viewport[1];
    const u32 vw = in.viewport[2], vh = in.viewport[3];
    if (!vw || !vh || vx + vw > width_ || vy + vh > height_) return false;

    // The temporal stage runs on jitter-free frames with a continuous previous frame (TAAU owns the rest).
    if (in.sunMoved) sunHold_ = 2u;
    else if (sunHold_ > 0u) --sunHold_;
    if (!params_.stabilise) releaseStab();
    Constants cb{};
    const bool basisOk = viewBasis(dev_, cb.view);
    f32 eye[3] = {};
    bool stab = params_.stabilise && !params_.bypass && stabReady() && in.velocity && in.historyValid &&
                in.jitter[0] == 0.0f && in.jitter[1] == 0.0f && !(capture_ && capture_->active()) && basisOk &&
                dev_->camera(nullptr, nullptr, eye) && prevViewProjRel(in.prevViewProj, in.prevCamPos, cb.prevVP);
    if (stab && !allocStab()) stab = false;
    // Captures keep Stage B's raw values (the training data).
    despeckled_ = params_.despeckle != 0u && !params_.bypass && psoDespeckle_ && setDespeckle_ &&
                  !(capture_ && capture_->active()) && allocDespeckle();

    rhi::ScopedGpuStat stat(ctx, "NRD2");
    // Descriptors first, every set, before any dispatch (Denoiser.cpp: a set rewritten between two
    // dispatches that use it would leave the first reading the second's resources).
    if (despeckled_) {
        res_->setSrv(setDespeckle_, 0, targets_.diffuse);
        res_->setSrv(setDespeckle_, 1, targets_.specular);
        res_->setSrv(setDespeckle_, 2, in.viewZ);
        res_->setUav(setDespeckle_, 0, despD_, 0);
        res_->setUav(setDespeckle_, 1, despS_, 0);
    }
    res_->setSrv(setPyramid_, 0, inD());
    res_->setSrv(setPyramid_, 1, inS());
    res_->setSrv(setPyramid_, 2, in.viewZ);
    res_->setSrv(setPyramid_, 3, in.normalRoughness);
    for (u32 l = 0; l < 3; ++l) {
        res_->setUav(setPyramid_, l, guide_[l], 0);
        res_->setUav(setPyramid_, 3 + l, levelD_[l], 0);
        res_->setUav(setPyramid_, 6 + l, levelS_[l], 0);
    }
    const u32 tx = tilesOf(vw), ty = tilesOf(vh), tiles = tx * ty;
    res_->setUavBuffer(setParams_, 0, tileParams_, sizeof(f32), tileCapacity_ * kNrd2TileParams, 0);
    res_->setSrv(setResolve_, 0, inD());
    res_->setSrv(setResolve_, 1, inS());
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
    // Without the temporal stage the resolve never writes u1/u2; lit_ stands in so the set stays valid.
    res_->setUav(setResolve_, 1, stab ? dRes_ : lit_, 0);
    res_->setUav(setResolve_, 2, stab ? sRes_ : lit_, 0);
    res_->setSrv(setCompose_, 0, lit_);
    const u32 rd = histLast_, wr = 1u - histLast_;
    if (stab) {
        res_->setSrv(setReproject_, 0, dRes_);
        res_->setSrv(setReproject_, 1, sRes_);
        res_->setSrv(setReproject_, 2, in.viewZ);
        res_->setSrv(setReproject_, 3, in.normalRoughness);
        res_->setSrv(setReproject_, 4, in.velocity);
        res_->setSrv(setReproject_, 5, histD_[rd]);
        res_->setSrv(setReproject_, 6, histS_[rd]);
        res_->setSrv(setReproject_, 7, histG_[rd]);
        res_->setSrv(setReproject_, 8, histV_[rd]);
        res_->setUav(setReproject_, 0, rpD_, 0);
        res_->setUav(setReproject_, 1, rpS_, 0);
        res_->setUav(setReproject_, 2, rpV_, 0);
        res_->setUav(setReproject_, 3, anchD_, 0);
        res_->setUav(setReproject_, 4, anchS_, 0);
        res_->setSrv(setPrefilter_, 0, dRes_);
        res_->setSrv(setPrefilter_, 1, sRes_);
        res_->setSrv(setPrefilter_, 2, in.viewZ);
        res_->setSrv(setPrefilter_, 3, in.normalRoughness);
        res_->setSrv(setPrefilter_, 4, rpV_);
        res_->setSrv(setPrefilter_, 5, anchD_);
        res_->setSrv(setPrefilter_, 6, anchS_);
        res_->setUav(setPrefilter_, 0, prefD_, 0);
        res_->setUav(setPrefilter_, 1, prefS_, 0);
        res_->setSrv(setTemporal_, 0, prefD_);
        res_->setSrv(setTemporal_, 1, prefS_);
        res_->setSrv(setTemporal_, 2, in.viewZ);
        res_->setSrv(setTemporal_, 3, in.normalRoughness);
        res_->setSrv(setTemporal_, 4, targets_.remodA);
        res_->setSrv(setTemporal_, 5, targets_.remodB);
        res_->setSrv(setTemporal_, 6, rpD_);
        res_->setSrv(setTemporal_, 7, rpS_);
        res_->setSrv(setTemporal_, 8, anchD_);
        res_->setSrv(setTemporal_, 9, anchS_);
        res_->setUav(setTemporal_, 0, lit_, 0);
        res_->setUav(setTemporal_, 1, histD_[wr], 0);
        res_->setUav(setTemporal_, 2, histS_[wr], 0);
        res_->setUav(setTemporal_, 3, histG_[wr], 0);
        res_->setUav(setTemporal_, 4, histV_[wr], 0);
    }

    cb.rect[0] = vx; cb.rect[1] = vy; cb.rect[2] = vw; cb.rect[3] = vh;
    cb.tiles[0] = tx; cb.tiles[1] = ty;
    cb.tiles[2] = (params_.bypass ? kFlagBypass : 0u) | (stab ? kFlagStabilise : 0u) | (stab && hadHist ? kFlagHistory : 0u);
    cb.tiles[3] = despeckled_ ? (params_.despeckle & 3u) : 0u;
    cb.stab[2] = std::max(params_.despeckleCap, 1.0f);
    std::memcpy(cb.def, params_.diffuse, sizeof(params_.diffuse));
    std::memcpy(cb.def + 6, params_.specular, sizeof(params_.specular));
    if (stab) {
        for (u32 k = 0; k < 3; ++k) cb.camDelta[k] = static_cast<f32>(static_cast<f64>(eye[k]) - static_cast<f64>(in.prevCamPos[k]));
        f32 nStill = std::min(std::max(params_.stabFrames, 1.0f), kStabNMax);
        if (sunHold_ > 0u) nStill = std::min(nStill, kStabNSunMoved);
        cb.stab[0] = nStill;
        cb.stab[1] = std::min(kStabNFast, nStill);
    }

    const rhi::TextureHandle stageB[4] = {targets_.diffuse, targets_.specular, targets_.remodA, targets_.remodB};
    for (rhi::TextureHandle t : stageB) ctx.textureBarrier(t, kWrite, kRead);
    ctx.textureBarrier(in.viewZ, in.gbufferState, kRead);
    ctx.textureBarrier(in.normalRoughness, in.gbufferState, kRead);

    const u32 gx = (vw + 7u) / 8u, gy = (vh + 7u) / 8u;
    if (despeckled_) {
        rhi::ScopedGpuStat despStat(ctx, "NRD2.Despeckle");
        ctx.textureBarrier(despD_, kRead, kWrite);
        ctx.textureBarrier(despS_, kRead, kWrite);
        ctx.setPipeline(psoDespeckle_);
        ctx.setBindingSet(setDespeckle_);
        ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
        ctx.dispatch(gx, gy, 1);
        ctx.textureBarrier(despD_, kWrite, kRead);
        ctx.textureBarrier(despS_, kWrite, kRead);
    }
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
            if (recordFeatures(ctx, in, network_.inScale(), network_.inBias()))
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
    ctx.bufferBarrier(tileParams_, kRead, rhi::ResourceState::Common);

    if (stab) {
        ctx.textureBarrier(dRes_, kWrite, kRead);
        ctx.textureBarrier(sRes_, kWrite, kRead);
        ctx.textureBarrier(in.velocity, in.gbufferState, kRead);
        auto pass = [&](const char* name, rhi::PipelineHandle pso, rhi::BindingSetHandle set,
                        std::initializer_list<rhi::TextureHandle> out) {
            rhi::ScopedGpuStat passStat(ctx, name);
            for (rhi::TextureHandle t : out) ctx.textureBarrier(t, kRead, kWrite);
            ctx.setPipeline(pso);
            ctx.setBindingSet(set);
            ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
            ctx.dispatch(gx, gy, 1);
            for (rhi::TextureHandle t : out) ctx.textureBarrier(t, kWrite, kRead);
        };
        pass("NRD2.Reproject", psoReproject_, setReproject_, {rpD_, rpS_, rpV_, anchD_, anchS_});
        pass("NRD2.Prefilter", psoPrefilter_, setPrefilter_, {prefD_, prefS_});
        pass("NRD2.Temporal", psoTemporal_, setTemporal_, {histD_[wr], histS_[wr], histG_[wr], histV_[wr]});
        ctx.textureBarrier(dRes_, kRead, kWrite);
        ctx.textureBarrier(sRes_, kRead, kWrite);
        histLast_ = wr;
        histValid_ = true;
        std::memcpy(histViewport_, in.viewport, sizeof(histViewport_));
    }
    ctx.textureBarrier(lit_, kWrite, rhi::ResourceState::ShaderResource);

    // Phase 3 capture: everything it reads is readable here. Jitter off while it holds a pose, from
    // the next frame's upload.
    if (capture_ && capture_->active()) capture_->step(ctx, *this, in);
    const bool hold = capture_ && capture_->holding();
    if (hold != jitterSuppressed_) { dev_->setJitterSuppressed(hold); jitterSuppressed_ = hold; }

    ctx.textureBarrier(in.viewZ, kRead, in.gbufferState);
    ctx.textureBarrier(in.normalRoughness, kRead, in.gbufferState);
    if (stab) ctx.textureBarrier(in.velocity, kRead, in.gbufferState);
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

bool Nrd2::recordFeatures(rhi::IRenderContext& ctx, const Inputs& in, const f32* inScale, const f32* inBias) {
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
    res_->setSrv(setFeatures_, 0, inD());
    res_->setSrv(setFeatures_, 1, inS());
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
    if (inScale && inBias) {
        cb.tiles[2] = kFlagStandardise;
        std::memcpy(cb.inScale, inScale, sizeof(cb.inScale));
        std::memcpy(cb.inBias, inBias, sizeof(cb.inBias));
    }
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
