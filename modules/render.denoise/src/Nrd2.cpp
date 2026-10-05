#include "aver/render/denoise/Nrd2.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

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
};
static_assert(sizeof(Constants) == 80, "Nrd2CB: two uint4s, three float4s");

constexpr u32 kFlagBypass = 1u;

// Per pass: SRV / UAV counts matching nrd2.hlsl's register lists.
constexpr u32 kPyramidSrv = 4, kPyramidUav = 9;
constexpr u32 kResolveSrv = 16, kResolveUav = 1;
constexpr u32 kResolveParamsSrv = 15;

constexpr rhi::ResourceState kRead  = rhi::ResourceState::NonPixelShaderResource;
constexpr rhi::ResourceState kWrite = rhi::ResourceState::UnorderedAccess;

u32 tilesOf(u32 d) { return (d + 7u) / 8u; }

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
    auto drop = [&](rhi::TextureHandle& t) { if (t) res_->destroyTexture(t); t = 0; };
    drop(targets_.diffuse); drop(targets_.specular); drop(targets_.remodA); drop(targets_.remodB);
    for (u32 l = 0; l < 3; ++l) { drop(guide_[l]); drop(levelD_[l]); drop(levelS_[l]); }
    drop(lit_);
    if (tileParams_) res_->destroyBuffer(tileParams_);
    tileParams_ = 0;
    tileCapacity_ = 0;
    width_ = height_ = 0;
    recorded_ = false;
}

void Nrd2::destroy() {
    releaseTargets();
    destroyCompose();
    if (res_) {
        for (rhi::PipelineHandle* p : {&psoPyramid_, &psoParams_, &psoResolve_}) { if (*p) res_->destroyPipeline(*p); }
        for (rhi::BindingSetHandle* s : {&setPyramid_, &setParams_, &setResolve_, &setCompose_}) {
            if (*s) res_->destroyBindingSet(*s);
        }
    }
    psoPyramid_ = psoParams_ = psoResolve_ = 0;
    setPyramid_ = setParams_ = setResolve_ = setCompose_ = 0;
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

    // Phase 1: the defaults. Phase 4's network writes this buffer instead.
    ctx.bufferBarrier(tileParams_, rhi::ResourceState::Common, kWrite);
    ctx.setPipeline(psoParams_);
    ctx.setBindingSet(setParams_);
    ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
    ctx.dispatch((tiles + 63u) / 64u, 1, 1);
    ctx.bufferBarrier(tileParams_, kWrite, kRead);

    ctx.textureBarrier(lit_, kRead, kWrite);
    ctx.setPipeline(psoResolve_);
    ctx.setBindingSet(setResolve_);
    ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
    ctx.dispatch(gx, gy, 1);
    ctx.textureBarrier(lit_, kWrite, rhi::ResourceState::ShaderResource);
    ctx.bufferBarrier(tileParams_, kRead, rhi::ResourceState::Common);

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

}  // namespace aver::render::denoise
