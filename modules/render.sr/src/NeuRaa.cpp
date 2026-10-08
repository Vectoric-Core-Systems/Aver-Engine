// NeuRAA. See NeuRaa.hpp and docs/rendering/NEURAA_NRD.md section 3.
#include "aver/sr/NeuRaa.hpp"
#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

namespace aver::sr {

namespace {

struct NeuRaaCB {   // cbuffer AverNeuRaaCB in sr_neuraa.hlsl
    u32 vp[4];
    u32 info[4];
};
static_assert(sizeof(NeuRaaCB) == 32, "mirrors the HLSL cbuffer");

constexpr u32 kConstantRegister = 1;   // b1, as every AverSR pass

// Pass 0: t0 viewZ, t1 normal, t2 colour; u0 visibility, u1 edges, u2 debug, u3 tiles, u4 distances.
constexpr u32 kDetectSrv = 3, kDetectUav = 5;
void detectSlots(rhi::SlotKind* srv, rhi::SlotKind* uav) {
    for (u32 i = 0; i < kDetectSrv; ++i) srv[i] = rhi::SlotKind::Texture2D;
    uav[0] = rhi::SlotKind::StructuredBuffer;
    uav[1] = rhi::SlotKind::Texture2D;
    uav[2] = rhi::SlotKind::Texture2D;
    uav[3] = rhi::SlotKind::StructuredBuffer;
    uav[4] = rhi::SlotKind::Texture2D;
}
// Pass 1: t0 viewZ, t1 colour, t2 edges, t3 distances, t4 network weights; u0 output.
constexpr u32 kResolveSrv = 5, kResolveUav = 1;
void resolveSlots(rhi::SlotKind* srv, rhi::SlotKind* uav) {
    for (u32 i = 0; i < 4; ++i) srv[i] = rhi::SlotKind::Texture2D;
    srv[4] = rhi::SlotKind::StructuredBuffer;
    uav[0] = rhi::SlotKind::Texture2D;
}

// The weights file written by the training script: "NRAW", version 1, then the layer sizes.
constexpr u32 kNetMagic = 0x5741524Eu, kNetIn = 36, kNetH1 = 32, kNetH2 = 32, kNetOut = 9;
constexpr u32 kNetFloats = 2 * kNetIn + kNetH1 * kNetIn + kNetH1 + kNetH2 * kNetH1 + kNetH2 + kNetOut * kNetH2 + kNetOut;
// Pass 2 (capture): t0 colour; u0 the reference mean.
constexpr u32 kAccumSrv = 1, kAccumUav = 1;
void accumSlots(rhi::SlotKind* srv, rhi::SlotKind* uav) {
    srv[0] = rhi::SlotKind::Texture2D;
    uav[0] = rhi::SlotKind::Texture2D;
}

// Capture timing, in rendered frames: travel to a new pose, let it settle, one base frame, the
// reference frames (an 8x8 grid of sub-pixel offsets), then wait for the readback to land.
constexpr u32 kCapTravel = 45, kCapSettle = 4, kCapRefFrames = 64, kCapReadbackDelay = 6;

} // namespace

NeuRaa::~NeuRaa() {
    for (rhi::PipelineHandle p : {detect_, resolve_, accum_}) if (p) res_.destroyPipeline(p);
    for (rhi::BindingSetHandle b : {detectSet_, resolveSet_, accumSet_}) if (b) res_.destroyBindingSet(b);
    releaseTargets();
    captureRelease();
    for (rhi::BufferHandle b : {net_, netPlaceholder_}) if (b) res_.destroyBuffer(b);
}

void NeuRaa::loadWeights() {
    if (weightsTried_) return;
    weightsTried_ = true;
    if (net_) { res_.destroyBuffer(net_); net_ = 0; }
    if (weightsPath_.empty()) return;
    std::ifstream f(weightsPath_, std::ios::binary);
    u32 header[6] = {};
    std::vector<f32> w(kNetFloats);
    if (!f.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != kNetMagic || header[1] != 1u ||
        header[2] != kNetIn || header[3] != kNetH1 || header[4] != kNetH2 || header[5] != kNetOut ||
        !f.read(reinterpret_cast<char*>(w.data()), static_cast<std::streamsize>(w.size() * sizeof(f32)))) {
        AVER_WARN("[NeuRAA] no usable network weights at {}; the baseline blend runs", weightsPath_);
        return;
    }
    rhi::BufferDesc bd{};
    bd.bytes = w.size() * sizeof(f32);
    bd.kind = rhi::BufferKind::Upload;
    bd.debugName = "NeuRAA network weights";
    net_ = res_.createBuffer(bd);
    if (!net_ || !res_.writeBuffer(net_, w.data(), bd.bytes, 0)) {
        if (net_) res_.destroyBuffer(net_);
        net_ = 0;
        return;
    }
    netFloats_ = kNetFloats;
    AVER_INFO("[NeuRAA] network loaded: {}", weightsPath_);
}

rhi::UpscalerNeeds NeuRaa::needs() const {
    const rhi::UpscalerNeeds own =
        rhi::UpscalerNeeds::Depth | rhi::UpscalerNeeds::Normal | rhi::UpscalerNeeds::PrimaryVisibility;
    return inner_ ? (inner_->needs() | own) : own;
}

bool NeuRaa::ensurePipelines() {
    if (detect_ && resolve_ && accum_ && detectSet_ && resolveSet_ && accumSet_) return true;
    if (failed_) return false;
    const std::string& src = rhi::shaderFile("sr_neuraa.hlsl");
    auto build = [&](const char* entry, const char* defines, u32 srvs, u32 uavs,
                     void (*slots)(rhi::SlotKind*, rhi::SlotKind*), rhi::BindingSetHandle& set) {
        rhi::ShaderDesc sd{};
        sd.source = src.c_str();
        sd.entry  = entry;
        sd.stage  = rhi::ShaderStage::Compute;
        sd.minShaderModel = 60;
        sd.defines = defines;
        const rhi::ShaderHandle cs = src.empty() ? 0 : res_.createShader(sd);
        rhi::PipelineHandle p = 0;
        if (cs) {
            rhi::ComputePipelineDesc pd{};
            pd.cs = cs;
            pd.layout.srvCount = srvs;
            pd.layout.uavCount = uavs;
            pd.layout.constantDwords[kConstantRegister] = 0;   // root CBV
            pd.layout.slotKindsDeclared = true;
            slots(pd.layout.srvKinds, pd.layout.uavKinds);
            p = res_.createComputePipeline(pd);
            res_.destroyShader(cs);
        }
        if (p && !set) {
            rhi::BindingSetDesc bd{};
            bd.srvCount = srvs;
            bd.uavCount = uavs;
            slots(bd.srvKinds, bd.uavKinds);
            set = res_.createBindingSet(bd);
        }
        return p;
    };
    if (!detect_)  detect_  = build("CSNeuRaaDetect", "AVER_NEURAA_PASS=0", kDetectSrv, kDetectUav, detectSlots, detectSet_);
    if (!resolve_) resolve_ = build("CSNeuRaaResolve", "AVER_NEURAA_PASS=1", kResolveSrv, kResolveUav, resolveSlots, resolveSet_);
    if (!accum_)   accum_   = build("CSNeuRaaAccum", "AVER_NEURAA_PASS=2", kAccumSrv, kAccumUav, accumSlots, accumSet_);
    if (!detect_ || !resolve_ || !accum_ || !detectSet_ || !resolveSet_ || !accumSet_) {
        failed_ = true;
        AVER_WARN("[NeuRAA] sr_neuraa.hlsl would not build; NeuRAA is off");
        return false;
    }
    return true;
}

void NeuRaa::releaseTargets() {
    for (rhi::TextureHandle* t : {&edges_, &dist_, &debug_, &aa_}) {
        if (*t) res_.destroyTexture(*t);
        *t = 0;
    }
    if (tiles_) res_.destroyBuffer(tiles_);
    tiles_ = 0;
    w_ = h_ = tileCount_ = 0;
}

bool NeuRaa::ensureTargets(u32 w, u32 h) {
    if (edges_ && dist_ && debug_ && aa_ && tiles_ && w == w_ && h == h_) return true;
    releaseTargets();
    auto make = [&](rhi::Format f, rhi::ResourceState state, const char* name) {
        rhi::TextureDesc d{};
        d.width = w; d.height = h;
        d.bind = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
        d.format = f;
        d.initialState = state;
        d.debugName = name;
        return res_.createTexture(d);
    };
    edges_ = make(rhi::Format::R8Uint, rhi::ResourceState::NonPixelShaderResource, "NeuRAA edge codes");
    dist_  = make(rhi::Format::RGBA8Unorm, rhi::ResourceState::NonPixelShaderResource, "NeuRAA edge distances");
    // The wrapped upscaler samples these two in its pixel shader.
    debug_ = make(rhi::Format::RGBA16F, rhi::ResourceState::ShaderResource, "NeuRAA debug view");
    aa_    = make(rhi::Format::RGBA16F, rhi::ResourceState::ShaderResource, "NeuRAA resolved");
    tileCount_ = ((w + 7u) / 8u) * ((h + 7u) / 8u);
    rhi::BufferDesc bd{};
    bd.bytes = static_cast<u64>(tileCount_) * sizeof(u32);
    bd.allowUnorderedAccess = true;
    bd.debugName = "NeuRAA tile flags";
    tiles_ = res_.createBuffer(bd);
    if (!edges_ || !dist_ || !debug_ || !aa_ || !tiles_) { releaseTargets(); return false; }
    w_ = w; h_ = h;
    return true;
}

rhi::TextureHandle NeuRaa::run(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in) {
    const rhi::PrimaryVisibility& vis = in.visibility;
    if (in.generated || !in.canRetarget || !vis.buffer || !in.depth || !in.normalRoughness) return 0;
    if (vis.viewport[0] + vis.viewport[2] > in.srcWidth || vis.viewport[1] + vis.viewport[3] > in.srcHeight)
        return 0;
    if (!ensurePipelines() || !ensureTargets(in.srcWidth, in.srcHeight)) return 0;

    using RS = rhi::ResourceState;
    const bool resolve = enabled_ && !debugView_;
    const rhi::TextureHandle reads[3] = {in.depth, in.normalRoughness, in.color};
    for (rhi::TextureHandle t : reads) ctx.textureBarrier(t, RS::ShaderResource, RS::NonPixelShaderResource);

    NeuRaaCB cb{};
    for (u32 i = 0; i < 4; ++i) cb.vp[i] = vis.viewport[i];
    const u32 gx = (vis.viewport[2] + 7u) / 8u, gy = (vis.viewport[3] + 7u) / 8u;
    cb.info[0] = vis.rowPitch;
    cb.info[1] = debugView_ ? 1u : 0u;
    cb.info[2] = gx;

    res_.setSrv(detectSet_, 0, in.depth);
    res_.setSrv(detectSet_, 1, in.normalRoughness);
    res_.setSrv(detectSet_, 2, in.color);
    res_.setUavBuffer(detectSet_, 0, vis.buffer, 16, vis.elementCount, 0);
    res_.setUav(detectSet_, 1, edges_, 0);
    res_.setUav(detectSet_, 2, debug_, 0);
    res_.setUavBuffer(detectSet_, 3, tiles_, sizeof(u32), tileCount_, 0);
    res_.setUav(detectSet_, 4, dist_, 0);
    // The network only while no TAA is blending history (NEURAA_NRD.md section 7, item 2).
    const bool taaAtRest = inner_ && inner_->isTemporal() && !in.cameraMoving;
    if (resolve) {
        loadWeights();
        if (!net_ && !netPlaceholder_) {
            rhi::BufferDesc pd{};
            pd.bytes = sizeof(f32);
            pd.kind = rhi::BufferKind::Upload;
            pd.debugName = "NeuRAA weights placeholder";
            netPlaceholder_ = res_.createBuffer(pd);
        }
        res_.setSrv(resolveSet_, 0, in.depth);
        res_.setSrv(resolveSet_, 1, in.color);
        res_.setSrv(resolveSet_, 2, edges_);
        res_.setSrv(resolveSet_, 3, dist_);
        if (net_) res_.setSrvBuffer(resolveSet_, 4, net_, sizeof(f32), netFloats_, 0);
        else      res_.setSrvBuffer(resolveSet_, 4, netPlaceholder_, sizeof(f32), 1, 0);
        res_.setUav(resolveSet_, 0, aa_, 0);
    }

    for (rhi::TextureHandle t : {edges_, dist_}) ctx.textureBarrier(t, RS::NonPixelShaderResource, RS::UnorderedAccess);
    ctx.textureBarrier(debug_, RS::ShaderResource, RS::UnorderedAccess);
    {
        rhi::ScopedGpuStat stat(ctx, "NeuRAA detect");
        ctx.setPipeline(detect_);
        ctx.setBindingSet(detectSet_);
        ctx.setConstantBuffer(kConstantRegister, &cb, sizeof(cb));
        ctx.dispatch(gx, gy, 1);
    }
    ctx.textureBarrier(debug_, RS::UnorderedAccess, RS::ShaderResource);
    for (rhi::TextureHandle t : {edges_, dist_}) ctx.textureBarrier(t, RS::UnorderedAccess, RS::NonPixelShaderResource);

    if (resolve) {
        cb.info[3] = (net_ && !taaAtRest) ? 1u : 0u;
        ctx.textureBarrier(aa_, RS::ShaderResource, RS::UnorderedAccess);
        rhi::ScopedGpuStat stat(ctx, "NeuRAA resolve");
        ctx.setPipeline(resolve_);
        ctx.setBindingSet(resolveSet_);
        ctx.setConstantBuffer(kConstantRegister, &cb, sizeof(cb));
        ctx.dispatch((in.srcWidth + 7u) / 8u, (in.srcHeight + 7u) / 8u, 1);
        ctx.textureBarrier(aa_, RS::UnorderedAccess, RS::ShaderResource);
    }

    for (rhi::TextureHandle t : reads) ctx.textureBarrier(t, RS::NonPixelShaderResource, RS::ShaderResource);
    return debugView_ ? debug_ : resolve ? aa_ : 0;
}

// ---- training capture ------------------------------------------------------------------------

void NeuRaa::startCapture(const std::string& dir, u32 count) {
    cap_.dir = dir;
    cap_.remaining = count;
    cap_.index = 0;
    cap_.frame = 0;
    cap_.state = count ? CapState::Travel : CapState::Idle;
    AVER_INFO("[NeuRAA] training capture: {} poses into {}", count, dir);
}

bool NeuRaa::jitterOverride(f32& x, f32& y) {
    switch (cap_.state) {
        case CapState::Settle: case CapState::Base: case CapState::Readback:
            x = y = 0.0f;
            return true;
        case CapState::Reference: {
            const u32 i = cap_.frame % kCapRefFrames;
            x = (static_cast<f32>(i % 8u) + 0.5f) / 8.0f - 0.5f;
            y = (static_cast<f32>(i / 8u) + 0.5f) / 8.0f - 0.5f;
            return true;
        }
        default:
            return false;
    }
}

void NeuRaa::captureRelease() {
    if (cap_.accum) res_.destroyTexture(cap_.accum);
    cap_.accum = 0;
    for (rhi::BufferHandle& b : cap_.rb) { if (b) res_.destroyBuffer(b); b = 0; }
    cap_.w = cap_.h = 0;
}

bool NeuRaa::captureTargets(const rhi::UpscalerInput& in) {
    if (cap_.accum && cap_.w == in.srcWidth && cap_.h == in.srcHeight) return true;
    captureRelease();
    rhi::TextureDesc d{};
    d.width = in.srcWidth; d.height = in.srcHeight;
    d.format = rhi::Format::RGBA16F;
    d.bind = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
    d.initialState = rhi::ResourceState::NonPixelShaderResource;
    d.debugName = "NeuRAA capture reference";
    cap_.accum = res_.createTexture(d);
    const rhi::TextureHandle src[kCapArrays] = {in.color, cap_.accum, in.depth, edges_, dist_, aa_};
    for (u32 i = 0; i < kCapArrays; ++i) {
        if (!src[i] || !res_.textureCopyFootprint(src[i], 0, cap_.fp[i])) { captureRelease(); return false; }
        rhi::BufferDesc bd{};
        bd.bytes = cap_.fp[i].totalBytes;
        bd.kind = rhi::BufferKind::Readback;
        bd.debugName = "NeuRAA capture readback";
        cap_.rb[i] = res_.createBuffer(bd);
        if (!cap_.rb[i]) { captureRelease(); return false; }
    }
    cap_.w = in.srcWidth; cap_.h = in.srcHeight;
    return true;
}

void NeuRaa::captureCopy(rhi::IRenderContext& ctx, u32 slot, rhi::TextureHandle t, rhi::ResourceState rest) {
    ctx.textureBarrier(t, rest, rhi::ResourceState::CopySource);
    ctx.copyTextureToBuffer(cap_.rb[slot], 0, t, 0);
    ctx.textureBarrier(t, rhi::ResourceState::CopySource, rest);
}

// File: "NRAA" (u32 0x4141524E), version 2, width, height, viewport x y w h, reference frame count,
// resolved-frame valid (all u32), then the six arrays in Capture order, rows tightly packed: base colour
// RGBA16F, reference mean RGBA16F, view Z R32F, edge code R8Uint, edge distances RGBA8, resolved RGBA16F.
void NeuRaa::captureWrite() {
    char name[32];
    std::snprintf(name, sizeof(name), "neuraa_%03u.bin", cap_.index);
    const std::string path = cap_.dir + "/" + name;
    std::ofstream f(path, std::ios::binary);
    const u32 header[10] = {0x4141524Eu, 2u, cap_.w, cap_.h, cap_.vp[0], cap_.vp[1], cap_.vp[2], cap_.vp[3],
                            kCapRefFrames, cap_.resolved ? 1u : 0u};
    f.write(reinterpret_cast<const char*>(header), sizeof(header));
    std::vector<u8> buf;
    for (u32 i = 0; i < kCapArrays && f; ++i) {
        const rhi::TextureCopyFootprint& fp = cap_.fp[i];
        buf.resize(static_cast<size_t>(fp.totalBytes));
        if (!res_.readBuffer(cap_.rb[i], buf.data(), fp.totalBytes, 0)) { f.setstate(std::ios::failbit); break; }
        for (u32 r = 0; r < fp.rows; ++r)
            f.write(reinterpret_cast<const char*>(buf.data()) + static_cast<size_t>(r) * fp.rowPitch, fp.rowBytes);
    }
    if (f) AVER_INFO("[NeuRAA] capture {} written: {}", cap_.index, path);
    else   AVER_WARN("[NeuRAA] capture {} could not be written to {}", cap_.index, path);
}

void NeuRaa::captureStep(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in) {
    using RS = rhi::ResourceState;
    switch (cap_.state) {
        case CapState::Travel:
            if (++cap_.frame >= kCapTravel) { cap_.state = CapState::Settle; cap_.frame = 0; }
            break;
        case CapState::Settle:
            if (!in.cameraMoving && ++cap_.frame >= kCapSettle) { cap_.state = CapState::Base; cap_.frame = 0; }
            break;
        case CapState::Base: {
            // The unjittered frame: edges and distances as NeuRAA sees them, plus colour and depth.
            const bool savedDebug = debugView_;
            debugView_ = false;
            cap_.resolved = run(ctx, in) != 0;   // enabled: NeuRAA's own result is captured too
            debugView_ = savedDebug;
            const bool ok = !in.cameraMoving && in.visibility.buffer && in.depth && edges_ && captureTargets(in);
            if (!ok) { cap_.state = CapState::Travel; cap_.frame = 0; break; }   // try another pose
            for (u32 i = 0; i < 4; ++i) cap_.vp[i] = in.visibility.viewport[i];
            captureCopy(ctx, 0, in.color, RS::ShaderResource);
            captureCopy(ctx, 2, in.depth, RS::ShaderResource);
            captureCopy(ctx, 3, edges_, RS::NonPixelShaderResource);
            captureCopy(ctx, 4, dist_, RS::NonPixelShaderResource);
            captureCopy(ctx, 5, aa_, RS::ShaderResource);
            cap_.state = CapState::Reference;
            cap_.frame = 0;
            break;
        }
        case CapState::Reference: {
            NeuRaaCB cb{};
            cb.info[2] = cap_.frame + 1u;
            cb.info[3] = cap_.frame == 0 ? 1u : 0u;
            res_.setSrv(accumSet_, 0, in.color);
            res_.setUav(accumSet_, 0, cap_.accum, 0);
            ctx.textureBarrier(in.color, RS::ShaderResource, RS::NonPixelShaderResource);
            ctx.textureBarrier(cap_.accum, RS::NonPixelShaderResource, RS::UnorderedAccess);
            ctx.setPipeline(accum_);
            ctx.setBindingSet(accumSet_);
            ctx.setConstantBuffer(kConstantRegister, &cb, sizeof(cb));
            ctx.dispatch((in.srcWidth + 7u) / 8u, (in.srcHeight + 7u) / 8u, 1);
            ctx.textureBarrier(cap_.accum, RS::UnorderedAccess, RS::NonPixelShaderResource);
            ctx.textureBarrier(in.color, RS::NonPixelShaderResource, RS::ShaderResource);
            if (++cap_.frame >= kCapRefFrames) {
                captureCopy(ctx, 1, cap_.accum, RS::NonPixelShaderResource);
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
                if (cap_.state == CapState::Idle) AVER_INFO("[NeuRAA] training capture finished");
            }
            break;
        default:
            break;
    }
}

// Same contract as the wrapped upscaler: outTarget arrives bound, and is left bound.
void NeuRaa::execute(rhi::IRenderContext& ctx, const rhi::UpscalerInput& in, rhi::TextureHandle outTarget) {
    if (cap_.state != CapState::Idle && !in.generated && in.canRetarget && ensurePipelines()) captureStep(ctx, in);
    rhi::UpscalerInput next = in;
    if (enabled_ || debugView_)
        if (const rhi::TextureHandle img = run(ctx, in)) next.color = img;
    rhi::IUpscaler* up = inner_ ? inner_ : static_cast<rhi::IUpscaler*>(&passthrough_);
    up->execute(ctx, next, outTarget);
}

} // namespace aver::sr
