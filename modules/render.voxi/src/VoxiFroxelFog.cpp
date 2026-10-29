#include "aver/voxi/VoxiFroxelFog.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace aver::voxi {

namespace {

constexpr const char* kShaderName = "voxi_froxel.hlsl";

constexpr u32 kGroup = 8;   // [numthreads(8, 8, 1)] in both compute entries

u32 groups(u32 n) { return (n + kGroup - 1u) / kGroup; }

f32 finiteOr(f32 v, f32 fallback) { return std::isfinite(v) ? v : fallback; }

constexpr f64 kMiB = 1024.0 * 1024.0;

} // namespace

VoxiFroxelFog::Params VoxiFroxelFog::fromSettings(const Settings& s) {
    Params p;
    p.gridX         = std::clamp(s.volumetricFogGridX, 16u, 320u);
    p.gridY         = std::clamp(s.volumetricFogGridY, 9u, 180u);
    p.gridZ         = std::clamp(s.volumetricFogGridZ, 16u, 128u);
    p.maxDistanceCm = std::clamp(finiteOr(s.volumetricFogMaxDistance, 20000.0f), 100.0f, 500000.0f);
    p.density       = std::clamp(finiteOr(s.volumetricFogDensity, 0.0f), 0.0f, 0.01f);
    p.heightFog     = std::clamp(finiteOr(s.volumetricFogHeightFog, 1.0f), 0.0f, 8.0f);
    p.intensity     = std::clamp(finiteOr(s.volumetricFogIntensity, 1.0f), 0.0f, 16.0f);
    p.anisotropy    = std::clamp(finiteOr(s.volumetricFogAnisotropy, 0.6f), -0.95f, 0.95f);
    p.temporal      = std::clamp(finiteOr(s.volumetricFogTemporal, 0.85f), 0.0f, 0.98f);
    return p;
}

void VoxiFroxelFog::setTargets(u32 sampleCount, rhi::Format color, rhi::Format depth) {
    sampleCount_ = sampleCount;
    color_ = color;
    depth_ = depth;
}

bool VoxiFroxelFog::buildPipelines(rhi::IResourceFactory& res, const rhi::PipelineLayout& layout, bool rtCapable) {
    destroyPipelines();
    const std::string& source = rhi::shaderFile(kShaderName);
    if (source.empty()) {
        AVER_WARN("[Voxi] volumetric fog: voxi_froxel.hlsl is not deployed beside the executable");
        return false;
    }
    const char* prelude = rhi::sharedShaderPrelude();

    // The scatter pass: with the ray-query sun shadow when the device has it, else without. A failed RT compile
    // falls back to the plain one rather than leaving the feature dead.
    rhi::ShaderHandle cs = 0;
    for (int attempt = 0; attempt < 2 && !cs; ++attempt) {
        const bool rt = rtCapable && attempt == 0;
        rhi::ShaderDesc sd{};
        sd.source = source.c_str();
        sd.prelude = prelude;
        sd.entry = "CSFroxelScatter";
        sd.stage = rhi::ShaderStage::Compute;
        sd.minShaderModel = rt ? 65 : 60;
        sd.defines = rt ? "AVER_RT=1" : "AVER_RT=0";
        cs = res.createShader(sd);
        rtBuilt_ = rt && cs;
    }
    if (!cs) {
        AVER_WARN("[Voxi] volumetric fog: the scatter shader would not compile");
        return false;
    }
    rhi::ComputePipelineDesc pd{};
    pd.cs = cs;
    pd.layout = layout;
    scatterPso_ = res.createComputePipeline(pd);
    res.destroyShader(cs);   // the pipeline owns the bytecode now

    rhi::ShaderDesc ig{};
    ig.source = source.c_str();
    ig.prelude = prelude;
    ig.entry = "CSFroxelIntegrate";
    ig.stage = rhi::ShaderStage::Compute;
    ig.minShaderModel = 60;
    ig.defines = "AVER_RT=0";
    const rhi::ShaderHandle ics = res.createShader(ig);
    if (ics) {
        pd.cs = ics;
        integratePso_ = res.createComputePipeline(pd);
        res.destroyShader(ics);
    }
    if (!scatterPso_ || !integratePso_) {
        AVER_WARN("[Voxi] volumetric fog: the compute pipelines would not build (scatter={}, integrate={})",
                  scatterPso_, integratePso_);
        destroyPipelines();
        return false;
    }
    AVER_INFO("[Voxi] volumetric fog: compute pipelines ready (sun shadows by {})",
              rtBuilt_ ? "ray query, falling back to the cascade map" : "the cascade map");
    return true;
}

void VoxiFroxelFog::setFogVolumes(const FogVolumeBlock* block) {
    if (!block || block->header[0] < 0.5f) { volCount_ = 0; return; }
    vol_ = *block;
    volCount_ = std::min(static_cast<u32>(block->header[0] + 0.5f), kFogVolumeMax);
    vol_.header[0] = static_cast<f32>(volCount_);
}

// G6 SH3: the fog shader set. A different set (by name, source and register counts) bumps the revision, which makes the next record() rebuild the volume variants.
void VoxiFroxelFog::setFogShaders(std::vector<rhi::GodotEnvProgram> shaders) {
    if (shaders.size() > rhi::kGodotFogShaderMax) shaders.resize(rhi::kGodotFogShaderMax);
    bool same = shaders.size() == shaders_.size();
    for (usize i = 0; same && i < shaders.size(); ++i)
        same = shaders[i].source == shaders_[i].source && shaders[i].ns == shaders_[i].ns && shaders[i].regs == shaders_[i].regs && shaders[i].globalRegs == shaders_[i].globalRegs;
    if (same) {
        for (usize i = 0; i < shaders.size(); ++i) shaders_[i].values = std::move(shaders[i].values);
        return;
    }
    shaders_ = std::move(shaders);
    ++shaderRev_;
    shaderRegs_.assign(static_cast<usize>(rhi::kGodotFogShaderMax) * rhi::kGodotFogShaderRegs * 4, 0.0f);
    for (usize i = 0; i < shaders_.size(); ++i)
        std::copy(shaders_[i].values.begin(), shaders_[i].values.begin() + std::min<usize>(shaders_[i].values.size(), rhi::kGodotFogShaderRegs * 4),
                  shaderRegs_.begin() + static_cast<std::ptrdiff_t>(i * rhi::kGodotFogShaderRegs * 4));
}

void VoxiFroxelFog::setFogShaderRegisters(const std::vector<std::vector<f32>>& perShader) {
    if (shaderRegs_.size() < static_cast<usize>(rhi::kGodotFogShaderMax) * rhi::kGodotFogShaderRegs * 4)
        shaderRegs_.assign(static_cast<usize>(rhi::kGodotFogShaderMax) * rhi::kGodotFogShaderRegs * 4, 0.0f);
    for (usize i = 0; i < perShader.size() && i < rhi::kGodotFogShaderMax; ++i) {
        f32* dst = shaderRegs_.data() + i * rhi::kGodotFogShaderRegs * 4;
        const usize n = std::min<usize>(perShader[i].size(), rhi::kGodotFogShaderRegs * 4);
        std::copy(perShader[i].begin(), perShader[i].begin() + static_cast<std::ptrdiff_t>(n), dst);
    }
}

// The AVER_FOG_VOLUMES twins of the scatter and integrate entries (voxi_froxel.hlsl). Same sun-shadow path as the ordinary scatter pass (rtBuilt_), so the
// frame block's shadow mode means the same in both. Built the first frame a fog volume reaches record(); a failure is remembered until the shader files change.
bool VoxiFroxelFog::buildVolumePipelines(rhi::IResourceFactory& res, const rhi::PipelineLayout& layout) {
    const std::string& source = rhi::shaderFile(kShaderName);
    if (source.empty()) return false;
    // G6 SH3: with converted fog shaders the prelude carries their code and the variants are built with AVER_FOG_SHADERS; without, exactly as before.
    std::string composedPrelude;
    const char* prelude = rhi::sharedShaderPrelude();
    const bool withShaders = !shaders_.empty();
    if (withShaders) {
        std::vector<const rhi::GodotEnvProgram*> list;
        for (const rhi::GodotEnvProgram& p : shaders_) list.push_back(&p);
        const std::string program = rhi::godotFogProgram(list);
        if (program.empty()) { AVER_WARN("[Voxi] volumetric fog: the Godot fog shaders could not be composed; volumes are drawn without them"); }
        else { composedPrelude = std::string(prelude) + program; prelude = composedPrelude.c_str(); }
    }
    const bool shaderBuild = withShaders && !composedPrelude.empty();
    volShaderRev_ = shaderRev_;

    rhi::ShaderDesc sd{};
    sd.source = source.c_str();
    sd.prelude = prelude;
    sd.entry = "CSFroxelScatter";
    sd.stage = rhi::ShaderStage::Compute;
    sd.minShaderModel = rtBuilt_ ? 65 : 60;
    const std::string scatterDefs = std::string(rtBuilt_ ? "AVER_RT=1;AVER_FOG_VOLUMES=1" : "AVER_RT=0;AVER_FOG_VOLUMES=1") + (shaderBuild ? ";AVER_FOG_SHADERS=1" : "");
    sd.defines = scatterDefs.c_str();
    const rhi::ShaderHandle cs = res.createShader(sd);
    rhi::ShaderDesc ig{};
    ig.source = source.c_str();
    ig.prelude = prelude;
    ig.entry = "CSFroxelIntegrate";
    ig.stage = rhi::ShaderStage::Compute;
    ig.minShaderModel = 60;
    const std::string integrateDefs = std::string("AVER_RT=0;AVER_FOG_VOLUMES=1") + (shaderBuild ? ";AVER_FOG_SHADERS=1" : "");
    ig.defines = integrateDefs.c_str();
    const rhi::ShaderHandle ics = res.createShader(ig);
    rhi::ComputePipelineDesc pd{};
    pd.layout = layout;
    if (cs) { pd.cs = cs; scatterVolPso_ = res.createComputePipeline(pd); res.destroyShader(cs); }
    if (ics) { pd.cs = ics; integrateVolPso_ = res.createComputePipeline(pd); res.destroyShader(ics); }
    if (!scatterVolPso_ || !integrateVolPso_) {
        if (scatterVolPso_) res.destroyPipeline(scatterVolPso_);
        if (integrateVolPso_) res.destroyPipeline(integrateVolPso_);
        scatterVolPso_ = integrateVolPso_ = 0;
        AVER_WARN("[Voxi] volumetric fog: the fog-volume compute variants would not build; fog volumes are not drawn");
        return false;
    }
    AVER_INFO("[Voxi] volumetric fog: fog-volume compute variants ready");
    return true;
}

bool VoxiFroxelFog::buildSky(rhi::IResourceFactory& res) {
    if (skyPso_) { res.destroyPipeline(skyPso_); skyPso_ = 0; }
    skySamples_ = sampleCount_;
    skyColor_ = color_;
    skyDepth_ = depth_;
    if (!sampleCount_ || color_ == rhi::Format::Unknown) return false;   // targets not known yet

    const std::string& source = rhi::shaderFile(kShaderName);
    if (source.empty()) return false;
    const char* prelude = rhi::sharedShaderPrelude();

    rhi::ShaderDesc vd{};
    vd.source = source.c_str();
    vd.prelude = prelude;
    vd.entry = "VSky";   // the prelude's fullscreen triangle, at the far plane
    vd.stage = rhi::ShaderStage::Vertex;
    vd.defines = "AVER_FX_SKY=1";
    const rhi::ShaderHandle vs = res.createShader(vd);
    rhi::ShaderDesc pdsc = vd;
    pdsc.entry = "PSFroxelSky";
    pdsc.stage = rhi::ShaderStage::Pixel;
    const rhi::ShaderHandle ps = res.createShader(pdsc);
    if (vs && ps) {
        rhi::GraphicsPipelineDesc gd{};
        gd.vs = vs;
        gd.ps = ps;
        gd.layout.uavCount = 1;
        gd.layout.slotKindsDeclared = true;
        gd.layout.uavKinds[0] = rhi::SlotKind::Texture3D;
        gd.cull = rhi::CullMode::None;
        // Sky is the pixels still at the far plane: VSky emits z = 1, so "less or equal" is "equal" there.
        gd.depth.test = true;
        gd.depth.write = false;
        gd.depth.op = rhi::CompareOp::LessEqual;
        gd.renderTargetCount = 1;
        gd.renderTargets[0] = color_;
        gd.depthFormat = depth_;
        gd.sampleCount = sampleCount_;
        gd.blend = rhi::BlendMode::PremultipliedAlpha;
        skyPso_ = res.createGraphicsPipeline(gd);
    }
    if (vs) res.destroyShader(vs);
    if (ps) res.destroyShader(ps);
    if (!skyPso_) AVER_WARN("[Voxi] volumetric fog: the sky pipeline would not build; the sky is left unfogged");
    return skyPso_ != 0;
}

bool VoxiFroxelFog::createVolumes(rhi::IResourceFactory& res, const rhi::PipelineLayout& layout, u32 x, u32 y, u32 z,
                                  rhi::BindingSetHandle table0) {
    rhi::TextureDesc d;
    d.dim = rhi::TextureDim::Tex3D;
    d.width = x; d.height = y; d.depth = z;
    d.mips = 1;
    d.format = rhi::Format::RGBA16F;
    d.initialState = rhi::ResourceState::UnorderedAccess;
    d.bind = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
    d.debugName = "Voxi froxel fog scatter A";
    tex_[0] = res.createTexture(d);
    d.debugName = "Voxi froxel fog scatter B";
    tex_[1] = res.createTexture(d);
    // The output is only ever read by typed UAV loads (scene shaders, sky) and written by the integrate pass.
    d.bind = rhi::ResourceBind::UnorderedAccess;
    d.debugName = "Voxi froxel fog volume";
    final_ = res.createTexture(d);
    if (!tex_[0] || !tex_[1] || !final_) {
        AVER_WARN("[Voxi] volumetric fog: the {}x{}x{} volumes could not be created", x, y, z);
        destroyVolumes(table0);
        return false;
    }

    rhi::BindingSetDesc bsd;
    bsd.srvCount = 1;
    bsd.uavCount = 1;
    bsd.srvKinds[0] = rhi::SlotKind::Texture3D;
    bsd.uavKinds[0] = rhi::SlotKind::Texture3D;
    bsd.srvBaseRegister = layout.srvCount;   // table 1 sits past table 0's registers
    bsd.uavBaseRegister = layout.uavCount;
    for (u32 p = 0; p < 2; ++p) {
        sets_[p] = res.createBindingSet(bsd);
        if (!sets_[p]) { destroyVolumes(table0); return false; }
        res.setSrv(sets_[p], 0, tex_[p], rhi::kAllMips);
        res.setUav(sets_[p], 0, tex_[1 - p], 0);
    }
    rhi::BindingSetDesc sky;
    sky.uavCount = 1;
    sky.uavKinds[0] = rhi::SlotKind::Texture3D;
    skySet_ = res.createBindingSet(sky);
    if (!skySet_) { destroyVolumes(table0); return false; }
    res.setUav(skySet_, 0, final_, 0);

    if (table0) res.setUav(table0, kVolumeUavSlot, final_, 0);
    gx_ = x; gy_ = y; gz_ = z;
    parity_ = 0;
    historyValid_ = false;
    const f64 bytes = static_cast<f64>(x) * y * z * 8.0 * 3.0;
    AVER_INFO("[Voxi] volumetric fog: {}x{}x{} froxel volumes created ({:.1f} MiB)", x, y, z, bytes / kMiB);
    return true;
}

void VoxiFroxelFog::destroyVolumes(rhi::BindingSetHandle table0) {
    if (!res_) return;
    // The view outlives the buffer: u24 goes back to the placeholder before the volume is destroyed.
    if (table0 && final_ && placeholder_) res_->setUav(table0, kVolumeUavSlot, placeholder_, 0);
    for (rhi::BindingSetHandle& s : sets_) { if (s) res_->destroyBindingSet(s); s = 0; }
    if (skySet_) res_->destroyBindingSet(skySet_);
    skySet_ = 0;
    for (rhi::TextureHandle& t : tex_) { if (t) res_->destroyTexture(t); t = 0; }
    if (final_) res_->destroyTexture(final_);
    final_ = 0;
    gx_ = gy_ = gz_ = 0;
    historyValid_ = false;
}

void VoxiFroxelFog::destroyPipelines() {
    if (!res_) return;
    if (scatterPso_) res_->destroyPipeline(scatterPso_);
    if (integratePso_) res_->destroyPipeline(integratePso_);
    if (skyPso_) res_->destroyPipeline(skyPso_);
    if (scatterVolPso_) res_->destroyPipeline(scatterVolPso_);
    if (integrateVolPso_) res_->destroyPipeline(integrateVolPso_);
    scatterPso_ = integratePso_ = skyPso_ = 0;
    scatterVolPso_ = integrateVolPso_ = 0;
    volLive_ = false;
    skySamples_ = 0;
}

bool VoxiFroxelFog::ensure(rhi::IResourceFactory& res, const rhi::PipelineLayout& layout, bool rtCapable,
                           bool shaderOk, bool on, const Params& p, rhi::BindingSetHandle table0) {
    if (!on) {
        if (final_ || scatterPso_) {
            res_ = &res;
            destroyVolumes(table0);
            destroyPipelines();
            AVER_INFO("[Voxi] volumetric fog released");
        }
        return false;
    }
    res_ = &res;
    if (!shaderOk) {
        if (!unavailableLogged_) {
            unavailableLogged_ = true;
            AVER_WARN("[Voxi] voxi.volumetricFog needs shader model 6.0, DXC and typed UAV loads; it has no effect on this device");
        }
        return false;
    }

    // Shader files changed on disk (hot reload): recompile, and retry a build that failed.
    const u64 rev = rhi::shaderFileRevision();
    if (rev != builtRev_) {
        builtRev_ = rev;
        buildFailed_ = false;
        volBuildFailed_ = false;
        destroyPipelines();
    }
    layout_ = layout;
    if (!scatterPso_ && !buildFailed_) {
        if (!buildPipelines(res, layout, rtCapable)) buildFailed_ = true;
    }
    if (buildFailed_) return false;
    if (skySamples_ != sampleCount_ || skyColor_ != color_ || skyDepth_ != depth_) buildSky(res);

    params_ = p;
    const f32 maxD = std::max(p.maxDistanceCm, kNearCm * 4.0f);
    // A different depth range re-maps every slice: last frame's volume no longer lines up.
    if (std::fabs(maxD - farCm_) > 1e-3f) historyValid_ = false;
    farCm_ = maxD;
    const f32 logRange = std::log(maxD / kNearCm);
    invLogRange_ = 1.0f / logRange;

    if (final_ && (gx_ != p.gridX || gy_ != p.gridY || gz_ != p.gridZ)) destroyVolumes(table0);
    if (!placeholder_) {
        rhi::TextureDesc pd;
        pd.dim = rhi::TextureDim::Tex3D;
        pd.width = pd.height = pd.depth = 1;
        pd.mips = 1;
        pd.format = rhi::Format::RGBA16F;
        pd.bind = rhi::ResourceBind::UnorderedAccess;
        pd.initialState = rhi::ResourceState::UnorderedAccess;
        pd.debugName = "Voxi froxel fog placeholder";
        placeholder_ = res.createTexture(pd);
    }
    if (!final_) {
        // A failed create is not retried every frame; a different grid size re-arms it.
        if (failedX_ == p.gridX && failedY_ == p.gridY && failedZ_ == p.gridZ) return false;
        if (!createVolumes(res, layout, p.gridX, p.gridY, p.gridZ, table0)) {
            failedX_ = p.gridX; failedY_ = p.gridY; failedZ_ = p.gridZ;
            return false;
        }
        failedX_ = failedY_ = failedZ_ = 0;
    }
    return true;
}

void VoxiFroxelFog::record(rhi::IRenderContext& ctx, rhi::BindingSetHandle table0, const Frame& f) {
    if (!final_ || !scatterPso_ || !integratePso_ || !table0 || !f.cascadeViewProj || !f.cascadeSplit ||
        !f.viewProj || !f.voxelOrigin)
        return;

    // The ray-query path exists only in the pipeline compiled with it.
    u32 sunMode = f.sunMode;
    if (sunMode == 2 && !rtBuilt_) sunMode = 0;
    if (sunMode == 1 && !f.shadowMap) sunMode = 0;
    sunModeUsed_ = sunMode;
    if (!sunModeLogged_[sunMode]) {
        sunModeLogged_[sunMode] = true;
        static const char* const kName[3] = {"nothing (unshadowed)", "the cascade shadow map", "ray queries"};
        AVER_INFO("[Voxi] volumetric fog: sun visibility from {}", kName[sunMode]);
    }

    // G4 R8: fog volumes run the AVER_FOG_VOLUMES variants; without any, today's passes. The two store the scatter volume differently, so a switch
    // between them drops the history for a frame.
    bool useVol = false;
    if (scatterVolPso_ && volShaderRev_ != shaderRev_) {
        // The fog shader set changed: the volume variants carry the old code.
        res_->destroyPipeline(scatterVolPso_);
        if (integrateVolPso_) res_->destroyPipeline(integrateVolPso_);
        scatterVolPso_ = integrateVolPso_ = 0;
        volBuildFailed_ = false;
    }
    if (volCount_ > 0 && !volBuildFailed_ && res_) {
        if (!scatterVolPso_ || !integrateVolPso_) volBuildFailed_ = !buildVolumePipelines(*res_, layout_);
        useVol = scatterVolPso_ && integrateVolPso_;
    }
    if (useVol != volWasLive_) historyValid_ = false;
    volWasLive_ = volLive_ = useVol;

    // History is good only if the last volume was recorded the frame before, from the same camera path.
    const bool useHistory = historyValid_ && f.historyOk && lastFrame_ + 1u == f.frameIndex;

    std::memcpy(cb_.cascadeViewProj, f.cascadeViewProj, sizeof(cb_.cascadeViewProj));
    std::memcpy(cb_.cascadeSplit, f.cascadeSplit, sizeof(cb_.cascadeSplit));
    cb_.grid[0] = static_cast<f32>(gx_);
    cb_.grid[1] = static_cast<f32>(gy_);
    cb_.grid[2] = static_cast<f32>(gz_);
    cb_.grid[3] = static_cast<f32>(f.frameIndex & 0xFFFFu);
    cb_.depth[0] = kNearCm;
    cb_.depth[1] = farCm_;
    cb_.depth[2] = 1.0f / invLogRange_;
    cb_.depth[3] = invLogRange_;
    cb_.medium[0] = params_.heightFog;
    cb_.medium[1] = params_.density;
    cb_.medium[2] = params_.intensity;
    cb_.medium[3] = params_.anisotropy;
    cb_.temporal[0] = useHistory ? params_.temporal : 0.0f;
    cb_.temporal[1] = cb_.temporal[2] = cb_.temporal[3] = 0.0f;
    std::memcpy(cb_.prevViewProj, useHistory ? prevViewProj_ : f.viewProj, sizeof(cb_.prevViewProj));
    cb_.shadow[0] = static_cast<f32>(sunMode);
    cb_.shadow[1] = f.shadowAtlasTexel;
    cb_.shadow[2] = static_cast<f32>(f.cascadeCount);
    cb_.shadow[3] = f.rayBiasCm;
    for (u32 i = 0; i < 4; ++i) cb_.voxel[i] = f.voxelOrigin[i];
    cb_.voxelParams[0] = f.airVisUsable ? 1.0f : 0.0f;
    cb_.voxelParams[1] = cb_.voxelParams[2] = cb_.voxelParams[3] = 0.0f;

    rhi::ScopedGpuStat stat(ctx, "Voxi volumetric fog");
    const u32 hist = parity_, cur = 1u - parity_;
    // History is sampled, so it leaves the UAV state for the dispatches; the shadow map is read by compute.
    ctx.textureBarrier(tex_[hist], rhi::ResourceState::UnorderedAccess, rhi::ResourceState::NonPixelShaderResource);
    const bool mapRead = sunMode == 1;
    if (mapRead)
        ctx.textureBarrier(f.shadowMap, rhi::ResourceState::ShaderResource, rhi::ResourceState::NonPixelShaderResource);

    // The constants: the engine block alone, or (fog volumes) followed by the volume block the variants declare as the tail of FroxelCB.
    // G6 SH3: with fog shaders the block is followed by their registers (the variants declare gFvGd after the volumes).
    constexpr usize kGdBytes = static_cast<usize>(rhi::kGodotFogShaderMax) * rhi::kGodotFogShaderRegs * 4 * sizeof(f32);
    u8 blob[sizeof(FroxelConstants) + sizeof(FogVolumeBlock) + kGdBytes];
    const void* cbData = &cb_;
    u32 cbBytes = sizeof(cb_);
    if (useVol) {
        std::memcpy(blob, &cb_, sizeof(cb_));
        std::memcpy(blob + sizeof(cb_), &vol_, sizeof(vol_));
        cbData = blob;
        cbBytes = static_cast<u32>(sizeof(cb_) + sizeof(vol_));
        if (!shaders_.empty() && shaderRegs_.size() * sizeof(f32) >= kGdBytes) {
            const usize dim = shaders_.size() * rhi::kGodotFogShaderRegs * 4 * sizeof(f32);
            std::memset(blob + cbBytes, 0, kGdBytes);
            std::memcpy(blob + cbBytes, shaderRegs_.data(), std::min(dim, kGdBytes));
            cbBytes += static_cast<u32>(shaders_.size() * rhi::kGodotFogShaderRegs * 4 * sizeof(f32));
        }
    }

    ctx.setPipeline(useVol ? scatterVolPso_ : scatterPso_);
    ctx.setBindingSet(table0, 0);
    ctx.setBindingSet(sets_[hist], 1);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, cbData, cbBytes);
    ctx.dispatch(groups(gx_), groups(gy_), gz_);
    ctx.uavBarrierTexture(tex_[cur]);

    ctx.setPipeline(useVol ? integrateVolPso_ : integratePso_);
    ctx.setBindingSet(table0, 0);
    ctx.setBindingSet(sets_[hist], 1);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, cbData, cbBytes);
    ctx.dispatch(groups(gx_), groups(gy_), 1);
    ctx.uavBarrierTexture(final_);

    ctx.textureBarrier(tex_[hist], rhi::ResourceState::NonPixelShaderResource, rhi::ResourceState::UnorderedAccess);
    if (mapRead)
        ctx.textureBarrier(f.shadowMap, rhi::ResourceState::NonPixelShaderResource, rhi::ResourceState::ShaderResource);

    std::memcpy(prevViewProj_, f.viewProj, sizeof(prevViewProj_));
    historyValid_ = true;
    lastFrame_ = f.frameIndex;
    parity_ = cur;
}

void VoxiFroxelFog::drawSky(rhi::IRenderContext& ctx) {
    if (!skyPso_ || !skySet_ || !final_) return;
    rhi::ScopedGpuStat stat(ctx, "Voxi froxel fog sky");
    ctx.setPipeline(skyPso_);
    ctx.setBindingSet(skySet_, 0);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
    ctx.drawFullscreen();
}

void VoxiFroxelFog::destroy(rhi::BindingSetHandle table0) {
    if (!res_) return;
    destroyVolumes(table0);
    destroyPipelines();
    if (placeholder_) res_->destroyTexture(placeholder_);
    placeholder_ = 0;
    res_ = nullptr;
    builtRev_ = ~0ull;
    buildFailed_ = false;
}

} // namespace aver::voxi
