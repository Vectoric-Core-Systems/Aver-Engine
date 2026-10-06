#include "aver/render/denoise/Nrd2Network.hpp"

#include "aver/core/Log.hpp"
#include "aver/render/denoise/Nrd2Trainer.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <cstring>
#include <filesystem>

namespace aver::render::denoise {

namespace {

constexpr u32 kConstantSlot = 3;
constexpr u32 kCh = 12;

constexpr rhi::ResourceState kCommon = rhi::ResourceState::Common;
constexpr rhi::ResourceState kRead   = rhi::ResourceState::NonPixelShaderResource;
constexpr rhi::ResourceState kWrite  = rhi::ResourceState::UnorderedAccess;

// nrd2_net.hlsl's Nrd2NetCB, byte for byte.
struct NetCB {
    u32 tiles[4];   // x: tiles
    f32 scale[12], bias[12], def[12];
};
static_assert(sizeof(NetCB) == 160, "Nrd2NetCB: one uint4, nine float4s");

i64 stampOf(const std::string& path) {
    std::error_code ec;
    const auto t = std::filesystem::last_write_time(path, ec);
    return ec ? 0 : static_cast<i64>(t.time_since_epoch().count());
}

}  // namespace

Nrd2Network::~Nrd2Network() { destroy(); }

void Nrd2Network::setWeightPaths(std::string user, std::string shipped) {
    user_ = std::move(user);
    shipped_ = std::move(shipped);
    loadedPath_.clear();
    loadedStamp_ = 0;
    sincePoll_ = 0;
}

void Nrd2Network::destroy() {
    net_.destroy();
    if (res_) {
        if (psoOut_) res_->destroyPipeline(psoOut_);
        if (setOut_) res_->destroyBindingSet(setOut_);
        if (out_) res_->destroyBuffer(out_);
    }
    psoOut_ = 0;
    setOut_ = 0;
    out_ = 0;
    outFloats_ = 0;
    invalidateBindings();
    tilesX_ = tilesY_ = 0;
    pipelinesTried_ = loaded_ = false;
    loadedPath_.clear();
    loadedStamp_ = 0;
    sincePoll_ = 0;
    res_ = nullptr;
    status_ = {};
}

void Nrd2Network::invalidateBindings() {
    net_.invalidateBindings();
    boundFeatures_ = boundParams_ = 0;
    boundFeatureFloats_ = boundParamFloats_ = 0;
}

bool Nrd2Network::fail(const char* why, bool warn) {
    status_.running = false;
    status_.problem = why;
    if (warned_ != why) {
        warned_ = why;
        if (warn) AVER_WARN("[NRD2] network not used: {}; default tile parameters", why);
        else AVER_INFO("[NRD2] network not used: {}; default tile parameters", why);
    }
    return false;
}

void Nrd2Network::markIdle(const char* why) {
    status_.running = false;
    status_.problem = why;
}

bool Nrd2Network::ensurePipelines(rhi::IDevice& dev) {
    if (pipelinesTried_) return psoOut_ && setOut_;
    pipelinesTried_ = true;
    res_ = dev.resources();
    if (!res_) return false;
    const std::string& source = rhi::shaderFile("nrd2_net.hlsl");
    if (source.empty()) return false;
    rhi::ShaderDesc sd{};
    sd.source = source.c_str();
    sd.entry = "CSNrd2NetOut";
    sd.stage = rhi::ShaderStage::Compute;
    sd.minShaderModel = 60;
    sd.defines = "AVER_NRD2_NET_PASS=1";
    const rhi::ShaderHandle cs = res_->createShader(sd);
    if (!cs) return false;
    rhi::ComputePipelineDesc pd{};
    pd.cs = cs;
    pd.layout.srvCount = 1;
    pd.layout.uavCount = 1;
    pd.layout.slotKindsDeclared = true;
    pd.layout.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
    pd.layout.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    pd.layout.constantDwords[kConstantSlot] = 0;   // root CBV
    psoOut_ = res_->createComputePipeline(pd);
    res_->destroyShader(cs);
    rhi::BindingSetDesc bd{};
    bd.srvCount = 1;
    bd.uavCount = 1;
    bd.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
    bd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    if (psoOut_) setOut_ = res_->createBindingSet(bd);
    return psoOut_ && setOut_;
}

void Nrd2Network::reload(rhi::IDevice& dev) {
    std::error_code ec;
    const bool haveUser = !user_.empty() && std::filesystem::exists(user_, ec);
    const bool haveShipped = !shipped_.empty() && std::filesystem::exists(shipped_, ec);
    const std::string& path = haveUser ? user_ : shipped_;
    const i64 stamp = (haveUser || haveShipped) ? stampOf(path) : 0;
    if ((haveUser || haveShipped) && path == loadedPath_ && stamp == loadedStamp_) return;
    if (!haveUser && !haveShipped) {
        loaded_ = false;
        loadedPath_.clear();
        status_.source = Nrd2NetworkStatus::Source::None;
        status_.path.clear();
        status_.steps = 0;
        status_.ratio = -1.0f;
        status_.gateOpen = false;
        return;
    }
    loadedPath_ = path;
    loadedStamp_ = stamp;
    loaded_ = false;
    if (!net_.valid() && !net_.create(dev, nrd2NetworkDesc(), neural::convDefaults(), neural::ConvMode::Infer)) return;
    net_.setGpuStats(false);   // "NRD2 network" brackets it
    const auto tryLoad = [&](const std::string& p) {
        if (!net_.loadWeights(p)) return false;
        Nrd2Standardisation s;
        if (!nrd2StandardisationFromAffine(net_.ioAffine(), s)) {
            AVER_WARN("[NRD2] {} has no input/output standardisation; not used", p);
            return false;
        }
        const neural::ConvIoAffine& io = net_.ioAffine();
        for (u32 c = 0; c < kCh; ++c) {
            inScale_[c] = io.inScale[c];
            inBias_[c] = io.inBias[c];
            outScale_[c] = io.outScale[c];
            outBias_[c] = io.outBias[c];
        }
        return true;
    };
    bool ok = tryLoad(path);
    std::string from = path;
    if (!ok && haveUser && haveShipped) {
        ok = tryLoad(shipped_);
        from = shipped_;
    }
    if (!ok) return;
    Nrd2Sidecar sc;
    const bool haveSc = readNrd2Sidecar(from + ".steps", sc);
    const bool wasOpen = status_.gateOpen && status_.path == from;
    status_.source = from == user_ ? Nrd2NetworkStatus::Source::User : Nrd2NetworkStatus::Source::Shipped;
    status_.path = from;
    status_.steps = haveSc ? sc.lifetimeSteps : 0;
    status_.ratio = haveSc ? sc.valRatio : -1.0f;
    status_.gateOpen = nrd2GateOpen(status_.ratio, wasOpen);
    loaded_ = true;
    warned_.clear();
    AVER_INFO("[NRD2] network weights {} ({} steps, held-out val/default {:.3f}): live gate {}", from, status_.steps,
              static_cast<double>(status_.ratio), status_.gateOpen ? "open" : "closed (needs <= 0.8)");
}

bool Nrd2Network::ready(rhi::IDevice& dev) {
    if (!ensurePipelines(dev)) return fail("its passes (nrd2_net.hlsl) would not build", true);
    if (sincePoll_++ % kReloadPollFrames == 0) reload(dev);
    if (!loaded_)
        return loadedPath_.empty() ? fail("no trained weights (nrd2_v1.avnn)", false)
                                   : fail("the weight file would not load (or the network would not build)", true);
    if (!status_.gateOpen) return fail("its held-out ratio is above the live gate", false);
    return true;
}

bool Nrd2Network::record(rhi::IRenderContext& ctx, rhi::BufferHandle features, u32 featureFloats,
                         rhi::BufferHandle params, u32 paramFloats, u32 tilesX, u32 tilesY, const f32 defaults[12]) {
    if (!loaded_ || !status_.gateOpen || !psoOut_) return fail("not ready", true);
    const u32 tiles = tilesX * tilesY;
    if (!features || !params || !tiles || featureFloats < 16u * kCh * tiles || paramFloats < kCh * tiles)
        return fail("its buffers are smaller than the frame", true);

    if (tilesX != tilesX_ || tilesY != tilesY_) {
        net_.invalidateBindings();
        const neural::TensorShape shapes[1] = {{1, kCh, 4 * tilesY, 4 * tilesX}};
        if (!net_.reserve(shapes)) { tilesX_ = tilesY_ = 0; return fail("its tensors would not allocate", true); }
        if (kCh * tiles > outFloats_) {
            if (out_) res_->destroyBuffer(out_);
            rhi::BufferDesc bd{};
            bd.bytes = static_cast<u64>(kCh) * tiles * sizeof(f32);
            bd.allowUnorderedAccess = true;
            bd.debugName = "NRD2 network output";
            out_ = res_->createBuffer(bd);
            outFloats_ = out_ ? kCh * tiles : 0;
            boundParams_ = 0;
            if (!out_) { tilesX_ = tilesY_ = 0; return fail("its output would not allocate", true); }
        }
        tilesX_ = tilesX;
        tilesY_ = tilesY;
    }
    if (features != boundFeatures_ || featureFloats != boundFeatureFloats_) {
        net_.invalidateBindings();
        boundFeatures_ = features;
        boundFeatureFloats_ = featureFloats;
    }
    if (params != boundParams_ || paramFloats != boundParamFloats_) {
        res_->setSrvBuffer(setOut_, 0, out_, sizeof(f32), outFloats_, 0);
        res_->setUavBuffer(setOut_, 0, params, sizeof(f32), paramFloats, 0);
        boundParams_ = params;
        boundParamFloats_ = paramFloats;
    }

    rhi::ScopedGpuStat stat(ctx, "NRD2 network");
    NetCB cb{};
    cb.tiles[0] = tiles;
    std::memcpy(cb.def, defaults, sizeof(cb.def));

    const neural::TensorShape shape{1, kCh, 4 * tilesY, 4 * tilesX};
    if (!net_.recordInfer(ctx, features, out_, shape, true)) return fail("the network could not record", true);

    std::memcpy(cb.scale, outScale_, sizeof(outScale_));
    std::memcpy(cb.bias, outBias_, sizeof(outBias_));
    ctx.bufferBarrier(out_, kCommon, kRead);
    ctx.bufferBarrier(params, kCommon, kWrite);
    ctx.setPipeline(psoOut_);
    ctx.setBindingSet(setOut_);
    ctx.setConstantBuffer(kConstantSlot, &cb, sizeof(cb));
    ctx.dispatch((tiles + 63u) / 64u, 1, 1);
    ctx.bufferBarrier(params, kWrite, kCommon);
    ctx.bufferBarrier(out_, kRead, kCommon);
    status_.running = true;
    status_.problem.clear();
    return true;
}

}  // namespace aver::render::denoise
