// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems.
// SPDX-License-Identifier: LGPL-2.1-only. See LICENSE at the repository root.
#include "aver/voxi/VoxiRenderer.hpp"
#include "aver/core/CpuTiming.hpp"   // CpuNest(CpuSpan::VoxiSubmit) below -- Types.hpp + stdlib only,
                                     // so no AVER_MODULE_VOXI guard needed, same as Log.hpp/Math.hpp.
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"   // light-frustum fit: Vec3 / Mat4::lookAtLH
#include "aver/pbr/MaterialGraphRegistry.hpp"
#include "aver/pbr/MaterialSystem.hpp"
#include "aver/pbr/PbrShaders.hpp"
#include "aver/voxi/VoxiGiShaders.hpp"   // kGiSrvCount/kGiUavCount: the "typed twice" fix below
#include "aver/formats/IesProfile.hpp"   // kIesTableV/H: the table's one definition
#include "aver/voxi/GiVisibility.hpp"   // givis::packAmbientW/halfDim -- the C++/HLSL bit-table's one definition

#include "VoxiShaders.hpp"
#include "aver/rhi/ShaderFiles.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstddef>   // offsetof: submit() and buildLocalLights read MaterialConstants fields out of a draw's bytes
#include <cmath>
#include <cstring>
#include <string>

// Voxi's GPU side: resources, passes and pipelines, expressed only in terms of the generic RHI.
namespace aver::voxi {

namespace {

// Shadow map: four cascades in one 2x2 atlas texture.
constexpr u32 kShadowCascades   = 4;
constexpr u32 kShadowCascadeSize = 2048;
constexpr u32 kShadowSize        = kShadowCascadeSize * 2;   // 2x2 atlas
static_assert(kShadowCascades == 4, "the atlas below is laid out as 2x2");

// GI volume's own shadow map, separate from camera cascades.
constexpr u32 kGiShadowSize = 1024;

// ReSTIR GI reservoir sizing: one 32-byte GiPackedReservoir (voxi_reservoir.hlsli) per pixel, two
// ping-pong slices in one buffer, row-major per slice. The shader addresses it from gGiSurfNrmHist's
// own dimensions (voxi_restir.hlsli's giReservoirIndex), the same width/height this sizes from, so
// both sides derive the layout from one extent rather than trusting two copies of a pitch to agree.
constexpr u32 kGiReservoirBufferCount = 2;   // this frame's write slice + last frame's read slice
constexpr u32 kGiReservoirElemBytes = 32;    // sizeof(GiPackedReservoir) -- two uint4
u32 giReservoirElemCount(u32 width, u32 height) {
    return width * height * kGiReservoirBufferCount;
}

// Staged ray-driven passes: rdVisBuf_'s element size. One uint4/pixel -- see gRdVisBuf (voxi.hlsl)
// for the hit/miss encoding. Pitch = render target's exact width, so ensureRdStagedResources' size check (rdStagedW_/H_) catches any resize.
constexpr u32 kRdVisElemBytes = 16;

// Sub-stage splits (Settings::rayDrivenShadowTiles / rayDrivenGiSplit): candidate buffers
// CSRdShadowProbe/CSRdGiTrace write and CSRdShadow/CSRdGi read back.
// kRdGiCandElemBytes: sizeof(RdGiCand) (voxi_restir.hlsli) -- float3 pos, uint flags, float3 nrm,
// f2LumTraced, float3 rad, f2LumSky, float4 vis = 64 bytes, one per pixel, same row pitch as rdVisBuf_.
constexpr u32 kRdGiCandElemBytes = 64;
// kRdShadowTileElemBytes: one uint mask per 8x8 tile (ceil(W/8)*ceil(H/8) tiles, not per pixel) --
// orders of magnitude smaller than rdVisBuf_/rdGiCandBuf_.
constexpr u32 kRdShadowTileElemBytes = 4;

// Reallocates if too small or more than twice needed (hysteresis: scale swings within 2x keep the buffer).
bool perPixelBufferNeedsRealloc(u32 capacity, u32 needed) {
    return capacity < needed || needed < capacity / 2;
}

// Local lights in single-pass PSRayDriven; may disable to fit register constraints.
constexpr bool kRdSinglePassLamps = true;

// Cascade split blend: 0 is uniform slabs, 1 is logarithmic (equal ratios).
constexpr f32 kCascadeSplitLambda = 0.85f;

// How far the cascades reach, as a multiple of the camera's near plane.
constexpr f32 kShadowRangeFromNear = 4000.0f;

// How small a caster has to get, measured in this cascade's own shadow-map texels, before it stops
// being drawn into it. ONE texel is the honest floor rather than a tuned number: below it the map
// has no sample that can hold the object, so the draw cannot change the image it is drawn into.
constexpr f32 kMinShadowTexels = 1.0f;

// Draw-list cap and TLAS instance count. TLAS preallocs instance buffers at this size (8 MiB when ray tracing is on);
// other buffers grow with actual draws submitted.
constexpr u32 kMaxDraws = 65536;
static_assert(kMaxDraws - 1 <= rhi::kMaxTlasInstanceId,
              "a draw's TLAS instance id is its index; past 24 bits packTlasInstances drops it");

// TLAS instance-mask lanes: a ray's mask is ANDed with an instance's, zero result skips it. Needed
// because a shadow ray must still see a translucent pane (to attenuate through it) while a
// solid-only ray must exclude it; kRtMaskAll stays 0xFF so existing TraceRayInline calls are
// unchanged until they deliberately narrow.
// Translucent-lane instance count this build, logged once (and on change) to answer "is the pane in
// the structure" for missing-translucent-shadow debugging.
u32 tlasTranslucentThisBuild_ = 0;
u32 tlasTranslucentLogged_    = 0;
// Alpha-masked instance count, logged separately: a translucent pane lets a shadow ray attenuate
// through it, while a cutout instance lets any ray see the holes -- both give up the hardware's
// any-hit skip, so the count is the first thing a "why did foliage get expensive" question needs.
u32 tlasAlphaMaskedThisBuild_ = 0;
u32 tlasAlphaMaskedLogged_    = 0;
constexpr u32 kRtMaskOpaque      = 0x01;
constexpr u32 kRtMaskTranslucent = 0x02;
// The viewer's own first-person body -- see AVER_RT_MASK_OWNER_HIDDEN in voxi.hlsl (must match).
// Opaque geometry every ray may hit except the ray-driven primary one.
constexpr u32 kRtMaskOwnerHidden = 0x04;
constexpr u32 kRtMaskAll         = 0xFF;

// Instanced foliage's TLAS instanceId: this bit | the index of the instance's prototype's first part in
// the foliage part table. MUST MATCH AVER_RT_FOLIAGE_ID_BIT in voxi_rt.hlsli. A draw's id is its dense
// index, which kMaxDraws keeps far below this bit, so the two id spaces cannot meet.
constexpr u32 kRtFoliageIdBit = 0x800000u;
static_assert(kMaxDraws <= kRtFoliageIdBit, "a draw's instance id must never carry the foliage bit");
static_assert(kRtFoliageIdBit <= rhi::kMaxTlasInstanceId, "the foliage bit must fit the 24-bit InstanceID");
static_assert(static_cast<u64>(kMaxFoliageInstances) + kMaxDraws <= rhi::kMaxTlasInstances,
              "the foliage prefix plus every draw must fit one TLAS");

// The material system's sampler register. materialShaderDefines() is told the same number.
constexpr u32 kMaterialSamplerSlot = 2;

// Fills a layout's static samplers: s0 the radiance volume, s1 the shadow comparison, s2 the
// material system's surface sampler.
void giSamplers(rhi::PipelineLayout& l) {
    l.samplers[0].filter  = rhi::Filter::Linear;
    l.samplers[0].address = rhi::AddressMode::Clamp;
    l.samplers[0].maxLod  = FLT_MAX;
    l.samplers[1].filter  = rhi::Filter::ComparisonLinear;
    l.samplers[1].address = rhi::AddressMode::Clamp;
    l.samplers[1].compare = rhi::CompareOp::LessEqual;
    l.samplers[kMaterialSamplerSlot].filter        = rhi::Filter::Anisotropic;
    l.samplers[kMaterialSamplerSlot].address       = rhi::AddressMode::Wrap;
    l.samplers[kMaterialSamplerSlot].maxAnisotropy = 8;
    l.samplerCount = kMaterialSamplerSlot + 1;
}

// Wider than kGiSrvCount/kGiUavCount (SandboxApp.cpp's GPU path depends on those constants).
// Includes extra slots for materials, backdrops, history, local lights, foliage, radiance cache and decals.
constexpr u32 kVoxiSrvCount = kGiSrvCount + 16;

// Wider than kGiUavCount (same reasoning as kVoxiSrvCount).
// Includes history, ray-driven, and radiance-cache outputs. u11+ always declared, bound to placeholders when absent.
constexpr u32 kVoxiUavCount = kGiUavCount + 20;

// Declares slot kinds for all table 0 bindings (Vulkan requires type consistency).
void giTableKinds(rhi::SlotKind* srv, rhi::SlotKind* uav) {
    srv[0] = rhi::SlotKind::Texture3D;              // t0 volume
    srv[1] = rhi::SlotKind::Texture2D;              // t1 shadow map
    srv[2] = rhi::SlotKind::AccelerationStructure;  // t2 TLAS
    srv[3] = rhi::SlotKind::StructuredBuffer;       // t3 flat vertices
    srv[4] = rhi::SlotKind::StructuredBuffer;       // t4 flat indices
    srv[5] = rhi::SlotKind::StructuredBuffer;       // t5 per-instance records
    srv[6] = rhi::SlotKind::Texture2D;              // t6 ray-traced shadow history (read)
    srv[7] = rhi::SlotKind::Texture2D;              // t7 ray-traced reflection history (read)
    srv[8] = rhi::SlotKind::Texture2D;              // t8 GI-only shadow map
    srv[9] = rhi::SlotKind::StructuredBuffer;       // t9 material constants
    srv[10] = rhi::SlotKind::Texture2D;             // t10 blended backdrop
    srv[11] = rhi::SlotKind::Texture2D;             // t11 sky-occlusion history (read)
    srv[12] = rhi::SlotKind::Texture2D;             // t12 GI-restir surface position history (read)
    srv[13] = rhi::SlotKind::Texture2D;             // t13 GI-restir surface normal history (read)
    srv[14] = rhi::SlotKind::Texture2D;             // t14 denoised sky occlusion (read)
    srv[15] = rhi::SlotKind::Texture2D;             // t15 denoised ReSTIR GI radiance (read)
    srv[16] = rhi::SlotKind::Texture2D;             // t16 ReSTIR visibility half-res history (read)
    srv[17] = rhi::SlotKind::Texture3D;             // t17 air sky-visibility volume (read)
    srv[18] = rhi::SlotKind::StructuredBuffer;      // t18 local-light list
    srv[19] = rhi::SlotKind::Texture2D;             // t19 local-light history (read)
    srv[20] = rhi::SlotKind::StructuredBuffer;      // t20 foliage part table
    srv[21] = rhi::SlotKind::StructuredBuffer;      // t21 foliage instance descs
    srv[22] = rhi::SlotKind::StructuredBuffer;      // t22 radiance-cache cascade info
    srv[23] = rhi::SlotKind::Texture2D;             // t23 denoised reflection (read)
    srv[24] = rhi::SlotKind::StructuredBuffer;      // t24 projected decal records
    uav[0] = rhi::SlotKind::Texture3D;              // u0 volume mip 0
    uav[1] = rhi::SlotKind::Texture3D;              // u1 injection accumulator
    uav[2] = rhi::SlotKind::Texture2D;              // u2 ray-traced shadow history (write)
    uav[3] = rhi::SlotKind::Texture2D;              // u3 ray-traced reflection history (write)
    uav[4] = rhi::SlotKind::Texture2D;              // u4 sky-occlusion history (write)
    uav[5] = rhi::SlotKind::Texture2D;              // u5 sky-occlusion hit distance (write)
    uav[6] = rhi::SlotKind::StructuredBuffer;       // u6 GI-restir reservoir buffer
    uav[7] = rhi::SlotKind::Texture2D;              // u7 GI-restir surface position history (write)
    uav[8] = rhi::SlotKind::Texture2D;              // u8 GI-restir surface normal history (write)
    uav[9] = rhi::SlotKind::Texture2D;              // u9 ReSTIR GI radiance (write)
    uav[10] = rhi::SlotKind::Texture2D;             // u10 ReSTIR visibility half-res history (write)
    uav[11] = rhi::SlotKind::StructuredBuffer;      // u11 ray-driven visibility record buffer
    uav[12] = rhi::SlotKind::Texture2D;             // u12 ray-driven sun visibility (RW)
    uav[13] = rhi::SlotKind::Texture2D;             // u13 ray-driven GI indirect diffuse (RW)
    uav[14] = rhi::SlotKind::Texture2D;             // u14 ray-driven sky occlusion (RW)
    uav[15] = rhi::SlotKind::Texture2D;             // u15 ray-driven reflection (RW)
    uav[16] = rhi::SlotKind::Texture3D;             // u16 air sky-visibility volume (write)
    uav[17] = rhi::SlotKind::StructuredBuffer;      // u17 GI-trace candidate buffer
    uav[18] = rhi::SlotKind::StructuredBuffer;      // u18 shadow-probe tile verdicts
    uav[19] = rhi::SlotKind::Texture2D;             // u19 local-light history (write)
    uav[20] = rhi::SlotKind::StructuredBuffer;      // u20 radiance-cache accumulator
    uav[21] = rhi::SlotKind::StructuredBuffer;      // u21 radiance-cache cells
    uav[22] = rhi::SlotKind::StructuredBuffer;      // u22 Path Tracing progressive accumulation
    uav[23] = rhi::SlotKind::Texture2D;             // u23 reflection denoiser input (write)
    static_assert(kVoxiSrvCount == 25 && kVoxiUavCount == 24 && kGiSrvCount == 9 && kGiUavCount == 4,
                  "giTableKinds fills exactly kVoxiSrvCount SRVs and kVoxiUavCount UAVs; widen "
                  "those, never kGiSrvCount/kGiUavCount -- those two are the union SandboxApp.cpp's "
                  "cluster path reserves at its own base and fills kinds for by hand, so growing "
                  "them without widening that file in the same change is a Vulkan set/layout "
                  "type mismatch rather than a compile error");
}

// Returns the pipeline layout every Voxi raster pipeline declares. `bindlessTextures` non-zero
// appends the ray path's texture array as one more root parameter in its own register space; every
// existing caller passes 0 and gets the identical root signature as before -- the whole point of a
// defaulted argument here rather than a second function.
rhi::PipelineLayout giLayout(u32 bindlessTextures = 0) {
    rhi::PipelineLayout l{};
    // Full slot list is in giTableKinds() above. srvCount/uavCount are BASED on kVoxiSrvCount/
    // kVoxiUavCount, not fixed registers, so widening table 0 rebases everything automatically --
    // used to be typed twice (a bare "9" here and again in createVoxelVolume's BindingSetDesc, with
    // no compile-time link, so a mismatch was a descriptor-table error at draw time).
    l.srvCount = kVoxiSrvCount;
    l.uavCount = kVoxiUavCount;
    // Table 1: the material's textures, based at t(kVoxiSrvCount) -- the root-signature builder
    // accumulates srvBase across tables, so a register number derived from a comment instead of this
    // value goes wrong the moment kVoxiSrvCount grows past kGiSrvCount.
    l.srvCount1 = pbr::kMaterialSrvCount;
    l.constantDwords[rhi::kObjectConstantRegister] = rhi::kObjectConstantDwords;
    giSamplers(l);
    // Table 1's textures are ordinary Texture2D, SlotKind's default, so only table 0 needs filling
    // in. Declaring kinds at all is what stops the backend from reflecting them (which got t2 wrong).
    l.slotKindsDeclared = true;
    giTableKinds(l.srvKinds, l.uavKinds);
    l.bindlessTextureCount = bindlessTextures;
    return l;
}

// Voxi's shader body, read from shaders/voxi.hlsl (here for test access without RHI dependency).
const char* voxiHlsl() {
    // No static: the loader owns the cache and reloadShaderFiles() clears it. A static here would
    // survive a reload and hand back the shader that was read at startup for the rest of the run.
    return rhi::shaderFile("voxi.hlsl").c_str();
}

// Prelude for Voxi HLSL: RHI declarations, material system contract, and material graphs.
// Rebuilt when material graph registry revision changes (cached by revision).
const char* voxiShaderPrelude() {
    static std::string s;   // owned by a static, because the caller borrows it
    static u64 built = ~0ull;
    // Both revisions, because either can move independently: a project can register a material
    // graph, and --shader-source can rewrite the prelude underneath it.
    const u64 rev = pbr::materialGraphs().revision() * 1000003ull + rhi::shaderFileRevision();
    if (built != rev) {
        s = std::string(rhi::sharedShaderPrelude());
        const std::string& graphs = pbr::materialGraphs().hlsl();
        if (!graphs.empty()) s += "\n#define AVER_MATERIAL_GRAPH 1\n";
        s += pbr::materialShaderPrelude();
        s += graphs;
        built = rev;
    }
    return s.c_str();
}

// Writes the tail of the per-draw block: shading model, then its default parameters.
void writeShadingConstants(f32* block) {
    const u32 model = 0;   // AVER_MODEL_STANDARD
    std::memcpy(block + 24, &model, sizeof(model));   // a uint in the block, not a converted float
    block[25] = 0.04f;
    block[26] = 1.0f;
    block[27] = 0.0f;
    block[28] = block[29] = block[30] = block[31] = 0.0f;   // emissive
}

// The shader model every Voxi pipeline that is not a mesh shader or a RayQuery variant asks for.
constexpr u32 kBaseSm = 51;

// Records Voxi's shaders into a pipeline batch, which owns them (there is nothing to destroy).
struct ShaderScope {
    explicit ShaderScope(rhi::IPipelineBatch& b) : batch(b) {}

    // Records one entry point of Voxi's HLSL. The batch copies the text, so the statics behind
    // voxiHlsl() / voxiShaderPrelude() may move on afterwards.
    rhi::ShaderHandle operator()(const char* entry, rhi::ShaderStage stage, u32 sm, const char* defines) {
        rhi::ShaderDesc sd;
        sd.source  = voxiHlsl();
        sd.prelude = voxiShaderPrelude();
        sd.entry   = entry;
        sd.stage   = stage;
        sd.minShaderModel = sm;
        sd.defines = defines;
        return batch.createShader(sd);
    }

    rhi::IPipelineBatch& batch;
};

} // namespace

// Creates every GPU resource Voxi needs and starts building its pipelines. Returns false, and names the first
// missing handle, if the device cannot support the baseline. The pipelines are waited for here in synchronous
// mode; in asynchronous mode they land at a later frame boundary (pumpBuilds) and the frame is blank until then.
bool VoxiRenderer::init(rhi::IDevice& device) {
    dev_ = &device;
    res_ = device.resources();
    if (!res_) {
        AVER_WARN("[Voxi] init declined: backend exposes no resource factory (no GPU support)");
        return false;
    }
    caps_ = device.caps();
    // Only D3D12 has worker-thread builds; elsewhere the (inline) batch would just land a frame late.
    if (device.backend() != rhi::Backend::D3D12 || !rhi::asyncShaderBuildsAllowed()) asyncBuilds_ = false;

    if (!materials_.init(device, giLayout().srvCount))
        AVER_WARN("[Voxi] the material system declined to initialise; draws fall back to an unbound table 1");

    createShadowResources();
    createVoxelVolume(settings_.voxelResolution);

    wantColor_ = device.backbufferFormat();
    wantDepth_ = device.depthFormat();
    wantSamples_ = device.sampleCount();
    // Optimistic until the scene set lands (validateCore drops it if the ray-tracing variant would not build):
    // the same capability test that decides whether that variant is compiled at all.
    rtSupported_ = caps_.rayTracingTier >= 11 && caps_.shaderModel >= 65 && caps_.dxcAvailable;

    // Upgrade air visibility placeholder to real texture if setting and device allow it. The real volume also
    // needs CSAirVis, which is not built yet: validateCore tries again.
    if (!ensureAirVis())
        AVER_WARN("[Voxi] air sky-visibility volume unavailable at startup; fog stays unoccluded "
                  "(voxi.fogOcclusion has no effect until it can be created)");

    const char* missing = nullptr;
    if (!shadowTex_)             missing = "shadow texture";
    else if (!voxelTex_)         missing = "voxel volume";
    else if (!voxelAccumTex_)    missing = "injection accumulator";
    else if (!bindings_)         missing = "main binding set";
    else if (!clearBindings_)    missing = "clear binding set";
    else if (!resolveBindings_)  missing = "resolve binding set";
    else if (!airVisPlaceholder_) missing = "air sky-visibility placeholder";
    else if (mipBindings_.size() + 1 != voxelMips_) missing = "mip binding sets (count)";
    for (usize m = 0; !missing && m < mipBindings_.size(); ++m)
        if (!mipBindings_[m]) missing = "mip binding set";

    if (missing) {
        AVER_ERROR("[Voxi] init FAILED: {} has a zero handle", missing);
        shutdown();
        return false;
    }
    initialised_ = true;

    // TLAS sized for draw-list cap, rebound only on foliage reallocation.
    if (rtSupported_) {
        // rtRefitAccel is latched at init; refitOrRebuildTlas() respects current setting per frame.
        tlas_ = settings_.rtRefitAccel ? res_->createTlasUpdatable(kMaxDraws) : res_->createTlas(kMaxDraws);
        if (tlas_) res_->setSrvTlas(bindings_, 2, tlas_);   // t2, per the shader's register(t2)
        else {
            AVER_WARN("[Voxi] TLAS could not be created; ray-traced sun shadows stay off");
            rtSupported_ = false;
        }
    }

    if (materials_.ready())
        device.setDefaultDrawBinding(materials_.fallbackBindingSet(), &materials_.fallbackConstants(),
                                     sizeof(pbr::MaterialConstants));

    AVER_INFO("[Voxi] init: resources ready (shadow tex={} volume={} accum={} ({}^3, {} mips) bindings={}/{}/{}/+{}); "
              "pipelines build {}",
              shadowTex_, voxelTex_, voxelAccumTex_, voxelResBuilt_, voxelMips_,
              bindings_, clearBindings_, resolveBindings_, static_cast<u32>(mipBindings_.size()),
              asyncBuilds_ ? "in the background" : "now");

    startBuild(kPsoBase | kPsoScene);
    if (asyncBuilds_) {
        blocked_ = true;   // blank until the scene set lands; prePass re-latches this every frame
        return true;
    }
    pumpBuilds(true);
    if (failed_) {
        shutdown();
        return false;
    }
    return true;
}

// Destroys every resource and returns the feature to its uninitialised state.
void VoxiRenderer::shutdown() {
    reportFrameTime("run total");
    // Builds in flight are dropped unseen: their pipelines were never adopted, so nothing of theirs is
    // in the factory to free.
    for (std::unique_ptr<Build>& bd : builds_) bd->batch->cancel();
    builds_.clear();
    initialised_ = failed_ = blocked_ = targetsStale_ = false;
    baseReady_ = sceneAdopted_ = coreChecked_ = baseStarted_ = false;
    lazyRequested_ = 0;
    ++sceneGen_;
    // Denoiser owns device resources; destroy before res_ check below.
    denoiser_.destroy();
    nrd2_.destroy();
    denoiseAoOutput_ = 0;
    denoiseGiOutput_ = 0;
    materials_.shutdown();
    if (!res_) { dev_ = nullptr; return; }
    for (rhi::BindingSetHandle s : mipBindings_) if (s) res_->destroyBindingSet(s);
    mipBindings_.clear();
    if (resolveBindings_) res_->destroyBindingSet(resolveBindings_);
    if (clearBindings_) res_->destroyBindingSet(clearBindings_);
    if (bindings_)      res_->destroyBindingSet(bindings_);
    resolveBindings_ = clearBindings_ = bindings_ = 0;

#define AVER_VOXI_DROP(n) do { if (n) res_->destroyPipeline(n); n = 0; } while (0);
    AVER_VOXI_PSO_BASE(AVER_VOXI_DROP)
    AVER_VOXI_PSO_SCENE(AVER_VOXI_DROP)
    AVER_VOXI_PSO_RC(AVER_VOXI_DROP)
    AVER_VOXI_PSO_PT(AVER_VOXI_DROP)
    AVER_VOXI_PSO_NRD2(AVER_VOXI_DROP)
#undef AVER_VOXI_DROP

    if (voxelAccumTex_) res_->destroyTexture(voxelAccumTex_);
    if (voxelAccumPlaceholder_) res_->destroyTexture(voxelAccumPlaceholder_);
    voxelAccumPlaceholder_ = 0;
    if (airVisTex_) res_->destroyTexture(airVisTex_);
    if (airVisPlaceholder_) res_->destroyTexture(airVisPlaceholder_);
    airVisTex_ = airVisPlaceholder_ = 0;
    airVisDirty_ = false;
    ++voxelGen_;
    if (voxelTex_)  res_->destroyTexture(voxelTex_);
    if (shadowTex_) res_->destroyTexture(shadowTex_);
    if (giShadowTex_) res_->destroyTexture(giShadowTex_);
    voxelAccumTex_ = voxelTex_ = shadowTex_ = giShadowTex_ = 0;
    // Reset ephemeral bookkeeping; user-facing dials persist across re-init.
    giAccumWanted_ = false;
    giAccumRecreateFailedLogged_ = false;
    giAccumRecreateBackoffTicks_ = 0;
    giAccumRecreateBackoffNext_ = 0;
    giQuietTicks_ = 0;
    for (rhi::TextureHandle& t : rtShadowHist_) { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : rtReflHist_)   { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : rtAoHist_)     { if (t) res_->destroyTexture(t); t = 0; }
    if (rtAoHitDist_) { res_->destroyTexture(rtAoHitDist_); rtAoHitDist_ = 0; }
    for (rhi::TextureHandle& t : giSurfPosHist_) { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : giSurfNrmHist_) { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : giVisHist_)     { if (t) res_->destroyTexture(t); t = 0; }
    // Local lights: history pair, placeholders, and light list.
    for (rhi::TextureHandle& t : rdLocalHist_)   { if (t) res_->destroyTexture(t); t = 0; }
    if (rdLocalHistPlaceholder_) { res_->destroyTexture(rdLocalHistPlaceholder_); rdLocalHistPlaceholder_ = 0; }
    for (rhi::BufferHandle& b : rdLocalLights_)  { if (b) res_->destroyBuffer(b); b = 0; }
    if (rdLocalLightsPlaceholder_) { res_->destroyBuffer(rdLocalLightsPlaceholder_); rdLocalLightsPlaceholder_ = 0; }
    releaseLightAssets();
    // Decals: record ring, placeholder, images.
    for (rhi::BufferHandle& b : decalBuf_) { if (b) res_->destroyBuffer(b); b = 0; }
    if (decalPlaceholder_) { res_->destroyBuffer(decalPlaceholder_); decalPlaceholder_ = 0; }
    releaseDecalTextures();
    decalTexCpu_.clear();   // their CPU copies are dropped after upload; the host registers them again
    decalBufCapacity_ = decalBufSlot_ = decalCount_ = 0;
    decalBound_ = 0;
    cb_.decalParams[0] = 0.0f;
    rdLocalLightCapacity_ = rdLocalLightSlot_ = rdLocalLightCount_ = rdLocalLampCount_ = 0;
    rdLocalLightsBound_ = 0;
    for (u64& k : rdGridSlotKey_) k = 0;
    rdGridCacheValid_ = false;
    rdLocalOutThisFrame_ = 0;
    rdLocalHistPrimed_ = false;
    rdLocalHistFrame_ = 0;
    rdKeyFrame_ = 0;
    if (giRadiance_) { res_->destroyTexture(giRadiance_); giRadiance_ = 0; }
    if (giReservoirs_) { res_->destroyBuffer(giReservoirs_); giReservoirs_ = 0; }
    giReservoirElemCapacity_ = 0;
    giCacheFreeBuffers();
    // Staged ray-driven pass outputs need explicit destroy (not bound to any binding set slot).
    if (rdVisBuf_)    { res_->destroyBuffer(rdVisBuf_);  rdVisBuf_ = 0; }
    if (rdSunVisTex_) { res_->destroyTexture(rdSunVisTex_); rdSunVisTex_ = 0; }
    if (rdGiTex_)     { res_->destroyTexture(rdGiTex_);     rdGiTex_ = 0; }
    if (rdAoTex_)     { res_->destroyTexture(rdAoTex_);     rdAoTex_ = 0; }
    if (rdReflTex_)   { res_->destroyTexture(rdReflTex_);   rdReflTex_ = 0; }
    if (rdVisBufPlaceholder_)  { res_->destroyBuffer(rdVisBufPlaceholder_);  rdVisBufPlaceholder_ = 0; }
    if (rdSunVisPlaceholder_) { res_->destroyTexture(rdSunVisPlaceholder_); rdSunVisPlaceholder_ = 0; }
    if (rdGiPlaceholder_)     { res_->destroyTexture(rdGiPlaceholder_);     rdGiPlaceholder_ = 0; }
    if (rdAoPlaceholder_)     { res_->destroyTexture(rdAoPlaceholder_);     rdAoPlaceholder_ = 0; }
    if (rdReflPlaceholder_)   { res_->destroyTexture(rdReflPlaceholder_);   rdReflPlaceholder_ = 0; }
    if (rdReflDnIn_)          { res_->destroyTexture(rdReflDnIn_);          rdReflDnIn_ = 0; }
    if (rdReflDnPlaceholder_) { res_->destroyTexture(rdReflDnPlaceholder_); rdReflDnPlaceholder_ = 0; }
    rdReflDnBound_ = false;
    denoiseReflOutput_ = 0;
    // Sub-stage splits' buffers and placeholders.
    if (rdGiCandBuf_) { res_->destroyBuffer(rdGiCandBuf_); rdGiCandBuf_ = 0; }
    if (rdShadowTileBuf_) { res_->destroyBuffer(rdShadowTileBuf_); rdShadowTileBuf_ = 0; }
    if (rdGiCandBufPlaceholder_) { res_->destroyBuffer(rdGiCandBufPlaceholder_); rdGiCandBufPlaceholder_ = 0; }
    if (rdShadowTileBufPlaceholder_) { res_->destroyBuffer(rdShadowTileBufPlaceholder_); rdShadowTileBufPlaceholder_ = 0; }
    if (ptAccumBuf_)         { res_->destroyBuffer(ptAccumBuf_);         ptAccumBuf_ = 0; }
    if (ptAccumPlaceholder_) { res_->destroyBuffer(ptAccumPlaceholder_); ptAccumPlaceholder_ = 0; }
    ptAccumElemCapacity_ = 0;
    ptAccumValid_ = false;
    // Radiance cache: bindings_ already destroyed, so destroy buffers outright.
    rc_.destroy();
    if (rcPlaceholder_) { res_->destroyBuffer(rcPlaceholder_); rcPlaceholder_ = 0; }
    neuracLive_ = false;
    rcSlotsBound_ = false;
    rcBoundGeneration_ = 0;
    rcUnsupportedLogged_ = false;
    rcCreateFailed_ = false;
    rcTwinsTried_ = false;
    ptTwinsTried_ = false;
    nrd2Tried_ = false;
    rdVisBufElemCapacity_ = 0;
    rdGiCandBufElemCapacity_ = rdShadowTileElemCapacity_ = 0;
    rdStagedW_ = rdStagedH_ = rdStagedRowPitch_ = 0;
    rdStagedFallbackLogged_ = false;
    rdStagedRunLogged_ = false;
    giHistValid_ = false;
    giHistPrimed_ = false;
    giVisHistValid_ = false;
    giVisHistPrimed_ = false;
    rtShadowHistW_ = rtShadowHistH_ = 0;
    rtHistWriteIdx_ = 0;
    rtHistValid_ = false;
    rtHistPrimed_ = false;

    // Foliage BLASes, part table and TLAS prefix first (while tlas_ still valid).
    clearFoliage();
    if (foliagePartPlaceholder_) { res_->destroyBuffer(foliagePartPlaceholder_); foliagePartPlaceholder_ = 0; }
    if (foliageDescPlaceholder_) { res_->destroyBuffer(foliageDescPlaceholder_); foliageDescPlaceholder_ = 0; }
    blas_.clear();
    ++blasRevision_;
    tlas_ = 0;
    rtTlasTranslucent_ = 0;
    rtShadowFirstHitLogged_ = 0;
    // Invalidated: init() rebuilds TLAS from nothing, so pre-shutdown draw matching is unreliable.
    rtAccelSnapValid_ = false;
    rtMovers_.clear();
    rtMoversNow_.clear();

    voxelMips_ = voxelResBuilt_ = 0;
    giReady_ = rtSupported_ = rtActive_ = rtLogged_ = false;
    draws_.clear();
    drawsPrev_.clear();
    translucentDraws_.clear();
    translucentDrawsPrev_.clear();
    lampDraws_.clear();
    lampDrawsPrev_.clear();
    rtRefitDeferred_ = false;
    rebuiltThisFrame_.clear();
    lastBlasRebuilds_ = 0xFFFFFFFFu;
    // Next full build has no previous frame for transform carry-over.
    rtInstanceGroupKey_.clear();
    rtPrevPending_.clear();
    frameTimeMs_.clear();
    frameTimeLastNs_ = 0;
    frameTimeSeen_ = 0;
    // Zeroed so reportVramUsage() always prints at least once after re-init.
    vramReportedRadianceBytes_ = 0;
    vramReportedAccumBytes_ = 0;
    vramReportedGiCacheBytes_ = 0;
    vramReportedRdBytes_ = 0;
    vramReportedBlasBytes_ = 0;
    vramReportedTlasBytes_ = 0;
    vramReportedFoliagePrefixBytes_ = 0;
    res_ = nullptr;
    dev_ = nullptr;
}

void VoxiRenderer::setSettings(const Settings& in) {
    // Path Tracing runs inside the staged ray-driven frame (voxi_pt.hlsli), so it brings what that frame
    // needs: ray-driven primary and ReSTIR GI. The denoiser stays the user's choice (None runs none); when
    // on, it gets a long history. Both modes force the same set, so switching between ReSTIR and Reference
    // reallocates nothing; ReSTIR's visibility mode is left as the project set it (forcing it flipped the
    // history and radiance-cache resources).
    Settings s = in;
    if (in.pathTracing != Quality::Off) {
        s.rtRenderMode = 1u;
        if (s.rayDrivenStages == 0u) s.rayDrivenStages = 1u;
        if (s.globalIllumination == Quality::Off) s.globalIllumination = Quality::Low;
        s.giMode = 1u;
        s.denoiserMaxSamples = std::max(s.denoiserMaxSamples, 128u);
    }
    const bool wasPtRef = ptReferenceWanted();
    const Quality wasPt = settings_.pathTracing;
    // Capture edge states BEFORE assignment: resources allocate on OFF->on, deallocate on on->OFF.
    const bool wasWanted = rayTracingWanted();
    const bool wasAoWanted = aoHistoryWanted();
    const bool wasGiRestirWanted = giRestirWanted();
    const bool wasVisWanted = giVisHistWanted();
    const bool wasLocalHistWanted = rdLocalHistWanted();
    const u32 wasVis = giRestirVisibility_;
    const bool wasRdStagedResourcesWanted = rdStagedResourcesWanted();
    const bool wasAirVisWanted = airVisWanted();
    const bool wasPtWanted = pathTracingWanted();
    settings_ = s;
    setShadowRays(s.rtShadowRays);
    setPixelsPerRayTile(s.rtPixelsPerRayTile);
    rtShadowDenoise_ = s.rtShadowDenoise;
    rtRenderMode_    = s.rtRenderMode;
    giMode_          = s.giMode;
    // Defensive clamping to match shader masks.
    giRestirVisibility_ = s.giRestirVisibility > 4u ? 3u : s.giRestirVisibility;
    giRestirSpatialSamples_ = std::min(s.giRestirSpatialSamples, 15u);
    giRestirMaxHistory_     = std::min(s.giRestirMaxHistory, 63u);
    ptBounces_       = s.ptBounces;
    // LATCHED at init: pipelines built once, later changes require reload.
    if (!layeredBsdfLatched_) {
        layeredBsdf_ = (s.layeredBsdf != Quality::Off);
        layeredBsdfLatched_ = true;
    } else if (layeredBsdf_ != (s.layeredBsdf != Quality::Off) && !layeredBsdfWarned_) {
        // Warn once: condition doesn't clear itself.
        layeredBsdfWarned_ = true;
        AVER_WARN("[Voxi] layeredBsdf changed after the pipelines were built; it takes effect on the "
                  "next project load. The shaders compiled for this session are unchanged. "
                  "(said once)");
    }
    setGiUpdateInterval(s.giUpdateInterval);

    // A resource edge below frees what an in-flight frame may still read through a descriptor; the
    // GPU is drained first. Only on a mode change, never per frame.
    const bool resourceEdge =
        rayTracingWanted() != wasWanted || aoHistoryWanted() != wasAoWanted ||
        giRestirWanted() != wasGiRestirWanted || giVisHistWanted() != wasVisWanted ||
        rdLocalHistWanted() != wasLocalHistWanted ||
        rdStagedResourcesWanted() != wasRdStagedResourcesWanted ||
        pathTracingWanted() != wasPtWanted || airVisWanted() != wasAirVisWanted;
    if (resourceEdge && res_) res_->waitIdle();

    // A change of path-tracing method leaves every history describing the other estimator.
    if (ptReferenceWanted() != wasPtRef || (wasPt == Quality::Off) != (s.pathTracing == Quality::Off)) {
        giHistValid_ = false;
        giVisHistValid_ = false;
        rtHistValid_ = false;
        ptAccumValid_ = false;
        denoiser_.forceHistoryReset();
    }

    // A genuine visibility-mode change resets GI reservoir history, denoiser history and the half-res
    // visibility history together -- all three carry a stale answer once visMode changes (2.8's
    // precedence rule; same reasoning as setLightingLegacyBits' R0/R2/R3 branch below). Gated on
    // giRestirWanted(): nothing live to invalidate otherwise. Not resetGiHistory()/resetDenoiserHistory()
    // themselves, which are named console commands with their own log line -- this is a
    // setSettings-driven edge, so it logs its own.
    if (giRestirVisibility_ != wasVis && giRestirWanted()) {
        giHistValid_ = false;
        giVisHistValid_ = false;
        denoiser_.forceHistoryReset();
        AVER_INFO("[Voxi] ReSTIR visibility rays: {} -> {} (GI/denoiser/visibility history reset)",
                  wasVis, giRestirVisibility_);
    }

    // Guarded on a real size: before the first onRenderTargetsChanged there is nothing to create at,
    // and that call will apply the current setting itself when it arrives.
    if ((rayTracingWanted() != wasWanted || aoHistoryWanted() != wasAoWanted ||
         giRestirWanted() != wasGiRestirWanted || giVisHistWanted() != wasVisWanted ||
         rdLocalHistWanted() != wasLocalHistWanted) &&
        rtHistWantW_ && rtHistWantH_)
        if (!ensureShadowHistory(rtHistWantW_, rtHistWantH_))
            AVER_ERROR("[Voxi] ray-traced history could not follow a ray-tracing setting change at {}x{}",
                       rtHistWantW_, rtHistWantH_);

    // Staged ray-driven resources edge.
    if (rdStagedResourcesWanted() != wasRdStagedResourcesWanted && rtHistWantW_ && rtHistWantH_)
        if (!ensureRdStagedResources(rtHistWantW_, rtHistWantH_))
            AVER_ERROR("[Voxi] staged ray-driven resources could not follow a settings change at {}x{}",
                       rtHistWantW_, rtHistWantH_);

    // Path Tracing's accumulation buffer follows the mode.
    if (pathTracingWanted() != wasPtWanted) ensurePtAccum();

    // Air visibility volume: fixed resolution, no size guard.
    if (airVisWanted() != wasAirVisWanted)
        if (!ensureAirVis())
            AVER_ERROR("[Voxi] air sky-visibility volume could not follow a voxi.fogOcclusion change");
}

// Places the GI volume: centre in world units, half-edge extent.
void VoxiRenderer::setVolume(const f32 center[3], f32 extent) {
    center_[0] = center[0]; center_[1] = center[1]; center_[2] = center[2];
    extent_ = extent;
}

// Sets the light axis for shadow cascade fitting (colour/intensity from device).
void VoxiRenderer::setSunDirection(const f32 dirToLight[3]) {
    sunDir_[0] = dirToLight[0]; sunDir_[1] = dirToLight[1]; sunDir_[2] = dirToLight[2];
}

void VoxiRenderer::setDebugView(bool on) { debugView_ = on; }

void VoxiRenderer::setGiPoisonView(bool on) { giPoisonView_ = on; }

// No log: reasserted every frame, would produce noise.
void VoxiRenderer::setGiVisPathView(bool on) { giVisPathView_ = on; }

void VoxiRenderer::setBlendedGiCone(bool on) {
    if (on == blendedGiCone_) return;
    blendedGiCone_ = on;
    AVER_INFO("[Voxi] blended-fragment indirect diffuse: {} (M5 pricing switch, console/--blended-gi)",
              on ? "voxel cone gather" : "ReSTIR (default)");
}

// Guarded on actual change to avoid resetting history every frame.
void VoxiRenderer::setLightingLegacyBits(u32 bits) {
    if (bits == lightingLegacyBits_) return;
    const u32 changed = bits ^ lightingLegacyBits_;
    lightingLegacyBits_ = bits;
    AVER_INFO("[Voxi] lighting legacy bits: ring={} doubleCount={} hitSky={} reuseVis={} cones={} "
              "blendedHistory={} denoisedReadback={} (console)",
              (bits & 1u) ? 1 : 0, (bits & 2u) ? 1 : 0, (bits & 4u) ? 1 : 0, (bits & 8u) ? 1 : 0,
              (bits & 16u) ? 1 : 0, (bits & 32u) ? 1 : 0, (bits & 64u) ? 1 : 0);
    // Bits 1,4,8: ReSTIR samples/adds/reuses differently, history becomes stale on flip.
    if (changed & (1u | 4u | 8u)) { resetGiHistory(); resetDenoiserHistory(); }
    // Bit 1: reshapes sky-occlusion ray, reset AO history separately.
    if (changed & 1u) resetAoHistory();
    // Bit 32: controls what blended fragment writes, affects RT history.
    if (changed & 32u) { resetGiHistory(); resetDenoiserHistory(); resetRtHistory(); }
}

void VoxiRenderer::resetGiHistory(bool quiet) {
    giHistValid_ = false;
    giVisHistValid_ = false;
    if (!quiet) AVER_INFO("[Voxi] GI ReSTIR reservoir history reset by console command (resetgihistory); "
              "next frame uses a fresh candidate only (expect one visibly noisier frame), both "
              "ping-pong slices clean within 2 frames.");
}

void VoxiRenderer::resetRtHistory(bool quiet) {
    rtHistValid_ = false;
    if (!quiet) AVER_INFO("[Voxi] RT temporal history reset by console command (resetrthistory): shadow, "
              "reflection AND sky-occlusion reprojection are ALL invalidated -- they share one "
              "validity flag today (see rtHistValid_'s own comment, VoxiRenderer.hpp, and "
              "voxi_rt.hlsli's rtReprojectAo gate).");
}

void VoxiRenderer::resetAoHistory() {
    rtHistValid_ = false;
    AVER_INFO("[Voxi] AO/sky-occlusion history reset by console command (resetaohistory) -- this "
              "currently ALSO resets RT shadow/reflection history, since all three share one "
              "validity flag today (see resetrthistory's own log line; true independence would need "
              "new plumbing).");
}

void VoxiRenderer::resetDenoiserHistory(bool quiet) {
    denoiser_.forceHistoryReset();
    if (!quiet) AVER_INFO("[Denoise] history reset by console command (resetdenoiserhistory); both "
              "signals restart their temporal accumulation from this frame.");
}

// Clamped to [1, kMaxShadowRays].
void VoxiRenderer::setShadowRays(u32 n) {
    const u32 clamped = n < 1 ? 1 : (n > kMaxShadowRays ? kMaxShadowRays : n);
    if (clamped != n)
        AVER_WARN("[Voxi] {} shadow rays per pixel clamped to {}", n, clamped);
    if (clamped == rtShadowRays_) return;
    rtShadowRays_ = clamped;
    AVER_INFO("[Voxi] sun occlusion rays per pixel: {}", rtShadowRays_);
}

// Rounds to nearest power of two (shader's trace schedule uses bitmask).
void VoxiRenderer::setPixelsPerRayTile(u32 n) {
    const u32 lo = n < 1 ? 1 : (n > kMaxPixelsPerRayTile ? kMaxPixelsPerRayTile : n);
    u32 rounded = 1;
    while (rounded * 2 <= lo) rounded *= 2;
    if (rounded < kMaxPixelsPerRayTile && (lo - rounded) > (rounded * 2 - lo)) rounded *= 2;
    if (rounded != n)
        AVER_WARN("[Voxi] {} pixels-per-ray tile edge rounded to the nearest power of two, {}", n, rounded);
    if (rounded == rtPixelsPerRayTile_) return;
    rtPixelsPerRayTile_ = rounded;
    AVER_INFO("[Voxi] ray-traced shadow tile: {}x{} ({} pixels per trace)",
              rounded, rounded, rounded * rounded);
}

// Clamped to [1, kMaxGiUpdateInterval].
void VoxiRenderer::setGiUpdateInterval(u32 n) {
    const u32 clamped = n < 1 ? 1 : (n > kMaxGiUpdateInterval ? kMaxGiUpdateInterval : n);
    if (clamped != n)
        AVER_WARN("[Voxi] GI update interval {} clamped to {}", n, clamped);
    if (clamped == giUpdateInterval_) return;
    giUpdateInterval_ = clamped;
    AVER_INFO("[Voxi] GI volume rebuilds every {} frame(s)", giUpdateInterval_);
}

// Reassert-idiom setter; guarded on actual change.
void VoxiRenderer::setGiForceRebuild(bool on) {
    if (on == giForceRebuild_) return;
    giForceRebuild_ = on;
    if (on) AVER_INFO("[Voxi] GI rebuild gate FORCED (--gi-force-rebuild / voxi.giForceRebuild): "
                       "every tick rebuilds; the GI cache is neither read nor written");
    else    AVER_INFO("[Voxi] GI rebuild gate no longer forced");
}

// Invalidates the previous rebuild's box on any edge change.
void VoxiRenderer::setGiBoundedDispatch(bool on) {
    if (on == giBoundedDispatch_) return;
    giBoundedDispatch_ = on;
    giBoxPrevValid_ = false;
    AVER_INFO("[Voxi] bounded GI dispatch {} (voxi.giBoundedDispatch); the next rebuild runs over "
              "the full grid", on ? "on" : "off");
}

// One-tick-delay contract; recreate next prePass via manageInjectionAccumulator.
void VoxiRenderer::setGiFreeAccumulator(bool on) {
    if (on == giFreeAccumulator_) return;
    giFreeAccumulator_ = on;
    AVER_INFO("[Voxi] injection-accumulator free-after-quiet {} (voxi.giFreeAccumulator, {} quiet "
              "tick(s) before a free); turning it off recreates the accumulator on the next tick if "
              "it was freed", on ? "on" : "off", kGiAccumulatorQuietTicks);
}

// Reports median frame time (heavy-tailed distribution).
void VoxiRenderer::reportFrameTime(const char* when) {
    if (!frameTimeReport_) return;
    if (frameTimeMs_.empty()) {
        AVER_INFO("[Voxi] frame period ({}): no samples past the {}-frame warm-up", when, kFrameTimeWarmup);
        return;
    }
    std::vector<f32> s = frameTimeMs_;
    std::sort(s.begin(), s.end());
    const usize n = s.size();
    const f32 med = s[n / 2];
    const f32 p90 = s[(n * 9) / 10 < n ? (n * 9) / 10 : n - 1];
    f32 sum = 0.0f;
    for (f32 v : s) sum += v;
    AVER_INFO("[Voxi] frame period ({}): {} frames, median {:.3f} ms, mean {:.3f} ms, min {:.3f} ms, "
              "p90 {:.3f} ms -- WHOLE frame, CPU, {} of max {} sun ray(s)/pixel, rt {}",
              when, static_cast<u32>(n), med, sum / static_cast<f32>(n), s.front(), p90,
              rtShadowRaysUsed_, rtShadowRays_, rtActive_ ? "active" : "off");
}

// Reports VRAM per category (prints only on change).
void VoxiRenderer::reportVramUsage() {
    // Radiance volume: RGBA16F, summed per mip.
    u64 radianceBytes = 0;
    for (u32 m = 0; m < voxelMips_; ++m) {
        const u64 dm = mipDim(voxelResBuilt_, m);
        radianceBytes += dm * dm * dm * 8ull;
    }
    // Accumulator: freed when not in use.
    const u64 accumBytes = voxelAccumTex_
        ? static_cast<u64>(voxelResBuilt_) * voxelResBuilt_ * voxelResBuilt_ * 16ull : 0;
    // Readback + upload, sized identically by giCacheEnsureBuffers; 0 once giCacheFreeBuffers has run.
    const u64 giCacheBytes = giCacheReadback_ ? giCacheBufBytes_ * 2ull : 0;
    // Staged ray-driven: four RGBA16F targets + three StructuredBuffers.
    const u64 rdTexBytes = rdSunVisTex_ ? static_cast<u64>(rdStagedW_) * rdStagedH_ * 8ull * 4ull : 0;
    const u64 rdBufBytes = static_cast<u64>(rdVisBufElemCapacity_) * kRdVisElemBytes +
                            static_cast<u64>(rdGiCandBufElemCapacity_) * kRdGiCandElemBytes +
                            static_cast<u64>(rdShadowTileElemCapacity_) * kRdShadowTileElemBytes;
    const u64 rdBytes = rdTexBytes + rdBufBytes;
    // BLAS: scene meshes + foliage prototypes (sampled every kVramBlasResampleFrames frames).
    // TLAS: structure, scratch, instance buffers, foliage prefix.
    if (blasRevision_ != vramBlasSampledRevision_ ||
        rtFrameIndex_ - vramBlasSampledFrame_ >= kVramBlasResampleFrames) {
        u64 sceneBlasBytes = 0;
        for (const auto& kv : blas_) sceneBlasBytes += res_->blasMemoryBytes(kv.second);
        vramBlasSceneBytes_ = sceneBlasBytes;
        vramBlasSampledRevision_ = blasRevision_;
        vramBlasSampledFrame_ = rtFrameIndex_;
    }
    const u64 blasBytes = foliageBlasBytes_ + vramBlasSceneBytes_;
    const u64 tlasBytes = tlas_ ? res_->tlasMemoryBytes(tlas_) : 0;

    if (radianceBytes == vramReportedRadianceBytes_ && accumBytes == vramReportedAccumBytes_ &&
        giCacheBytes == vramReportedGiCacheBytes_ && rdBytes == vramReportedRdBytes_ &&
        blasBytes == vramReportedBlasBytes_ && tlasBytes == vramReportedTlasBytes_ &&
        foliagePrefixBytes_ == vramReportedFoliagePrefixBytes_)
        return;

    vramReportedRadianceBytes_ = radianceBytes;
    vramReportedAccumBytes_ = accumBytes;
    vramReportedGiCacheBytes_ = giCacheBytes;
    vramReportedRdBytes_ = rdBytes;
    vramReportedBlasBytes_ = blasBytes;
    vramReportedTlasBytes_ = tlasBytes;
    vramReportedFoliagePrefixBytes_ = foliagePrefixBytes_;

    const auto mib = [](u64 bytes) { return static_cast<f64>(bytes) / (1024.0 * 1024.0); };
    if (voxelAccumTex_) {
        AVER_INFO("[Voxi] VRAM by category (MiB): radiance {:.0f}, injection accumulator {:.0f}, "
                  "GI cache staging {:.0f}, ray-driven per-pixel {:.0f}, BLAS {:.0f}, TLAS {:.0f} "
                  "(incl. foliage prefix {:.0f})",
                  mib(radianceBytes), mib(accumBytes), mib(giCacheBytes), mib(rdBytes),
                  mib(blasBytes), mib(tlasBytes), mib(foliagePrefixBytes_));
    } else {
        AVER_INFO("[Voxi] VRAM by category (MiB): radiance {:.0f}, injection accumulator freed, "
                  "GI cache staging {:.0f}, ray-driven per-pixel {:.0f}, BLAS {:.0f}, TLAS {:.0f} "
                  "(incl. foliage prefix {:.0f})",
                  mib(radianceBytes), mib(giCacheBytes), mib(rdBytes),
                  mib(blasBytes), mib(tlasBytes), mib(foliagePrefixBytes_));
    }
}

namespace {
// Row-vector point transform (w=1); perspective version with divide.
Vec3 xformPoint(const Vec3& p, const Mat4& m) {
    return Vec3{p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
                p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
                p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}
Vec3 xformProjected(const Vec3& p, const Mat4& m) {
    const f32 x = p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0];
    const f32 y = p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1];
    const f32 z = p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2];
    const f32 w = p.x * m.m[0][3] + p.y * m.m[1][3] + p.z * m.m[2][3] + m.m[3][3];
    const f32 inv = std::fabs(w) > 1e-9f ? 1.0f / w : 0.0f;
    return Vec3{x * inv, y * inv, z * inv};
}
// World matrix largest axis scale for bounding sphere culling.
f32 maxAxisScale(const Mat4& m) {
    const f32 sx = Vec3{m.m[0][0], m.m[0][1], m.m[0][2]}.size();
    const f32 sy = Vec3{m.m[1][0], m.m[1][1], m.m[1][2]}.size();
    const f32 sz = Vec3{m.m[2][0], m.m[2][1], m.m[2][2]}.size();
    return std::max(sx, std::max(sy, sz));
}

// FNV-1a hash over byte range.
void fnvMix(u64& h, const void* p, usize n) {
    const u8* b = static_cast<const u8*>(p);
    for (usize i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
}

// Hash lane step: xor, multiply, fold high half.
inline u64 hashLaneStep(u64 lane, u64 word) {
    lane ^= word;
    lane *= 0x9E3779B97F4A7C15ull;
    lane ^= lane >> 29;
    return lane;
}

// Lane state of hashBytesInto: a function of the bytes alone.
void hashBytesLanes(u64 (&lane)[4], const void* p, usize bytes) {
    const u8* b = static_cast<const u8*>(p);
    lane[0] = 0x243F6A8885A308D3ull ^ static_cast<u64>(bytes);
    lane[1] = 0x13198A2E03707344ull;
    lane[2] = 0xA4093822299F31D0ull;
    lane[3] = 0x082EFA98EC4E6C89ull;
    for (; bytes >= 32; b += 32, bytes -= 32) {
        u64 w[4];
        std::memcpy(w, b, sizeof(w));
        lane[0] = hashLaneStep(lane[0], w[0]);
        lane[1] = hashLaneStep(lane[1], w[1]);
        lane[2] = hashLaneStep(lane[2], w[2]);
        lane[3] = hashLaneStep(lane[3], w[3]);
    }
    for (u32 i = 0; bytes >= 8; ++i, b += 8, bytes -= 8) {
        u64 w = 0;
        std::memcpy(&w, b, sizeof(w));
        lane[i] = hashLaneStep(lane[i], w);
    }
    if (bytes) {
        u64 w = 0;
        std::memcpy(&w, b, static_cast<usize>(bytes));
        lane[3] = hashLaneStep(lane[3], w);
    }
}

// Folds a lane state into the running hash.
inline void hashFoldLanes(u64& h, const u64 (&lane)[4]) {
    for (const u64 l : lane) { h ^= l; h *= 1099511628211ull; }
}

// Hash bytes word-at-a-time with four independent lanes.
void hashBytesInto(u64& h, const void* p, usize bytes) {
    u64 lane[4];
    hashBytesLanes(lane, p, bytes);
    hashFoldLanes(h, lane);
}

// Material key for non-authored draws (built-in SurfaceLook or fallback).
// Shares one binding-set handle; uses colour/metal/rough values as dedup key.
// High bit cleared to avoid collision with authored keys.
u64 synthMaterialKey(const f32 color[4], f32 metallic, f32 roughness) {
    u64 h = 1469598103934665603ull;
    fnvMix(h, color, sizeof(f32) * 4);
    fnvMix(h, &metallic, sizeof(f32));
    fnvMix(h, &roughness, sizeof(f32));
    return h & ~(1ull << 63);
}

// 31-bit tag of a MaterialConstants block's emissiveFactor; 0 for exactly-zero emissive (the common case).
// Authored material keys carry it so draws that differ only in emissive get their own table row.
u32 emissiveKeyTag(const void* mcBytes) {
    u32 bits[3];
    std::memcpy(bits, static_cast<const u8*>(mcBytes) + offsetof(pbr::MaterialConstants, emissiveFactor),
                sizeof(bits));
    if (!(bits[0] | bits[1] | bits[2])) return 0;
    u64 h = 1469598103934665603ull;
    fnvMix(h, bits, sizeof(bits));
    h ^= h >> 32;
    const u32 tag = static_cast<u32>(h) & 0x7FFFFFFFu;
    return tag ? tag : 1u;
}

// Mix mesh handle bits for submit()'s cache. Same shape as GameRender.cpp's mixMeshId.
constexpr u64 mixMeshId(u64 x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}
} // namespace

// Starts a new draw list; Voxi runs a frame behind.
// Clears submit()'s per-mesh cache; bounds staleness to at most one frame.
void VoxiRenderer::beginScene() {
    drawsPrev_.swap(draws_);
    ++drawLanesToken_;
    draws_.clear();
    translucentDrawsPrev_.swap(translucentDraws_);
    translucentDraws_.clear();
    lampDrawsPrev_.swap(lampDraws_);
    lampDraws_.clear();
    dropMeshSubmitCache();
}

// Clears cache on depthProxyFn_/depthProxyUser_ change.
void VoxiRenderer::setDepthProxy(DepthProxyFn fn, void* user) {
    if (fn == depthProxyFn_ && user == depthProxyUser_) return;
    depthProxyFn_ = fn;
    depthProxyUser_ = user;
    dropMeshSubmitCache();
}

// Find mesh's slot in meshSubmitCache_, evict stale entries.
// Slots from old generations are evicted the same way even for the same mesh.
VoxiRenderer::MeshSubmitCacheSlot& VoxiRenderer::meshSubmitCacheSlot(rhi::MeshHandle mesh) {
    MeshSubmitCacheSlot& slot =
        meshSubmitCache_[static_cast<u32>(mixMeshId(mesh) & (kMeshSubmitCacheSlots - 1))];
    if (slot.mesh != mesh || slot.generation != meshSubmitCacheGen_)
        slot = MeshSubmitCacheSlot{mesh, meshSubmitCacheGen_};
    return slot;
}

// Records one draw into this frame's list, copying its material block.
void VoxiRenderer::submit(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                          f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                          const void* drawConstants, u32 drawConstantBytes, bool translucent,
                          bool hiddenFromOwner, bool movable) {
    if (mesh == 0) return;
    if (draws_.size() >= kMaxDraws) {
        if (!drawCapReported_) {
            drawCapReported_ = true;
            AVER_WARN("[Voxi] draw list is full at {} -- further entities cast no shadow and do not "
                      "voxelise this frame; raise kMaxDraws", kMaxDraws);
        }
        return;
    }
    // Nested CpuNest for submit() timing.
    CpuNest voxiSubmitTiming(CpuSpan::VoxiSubmit);
    Draw& d = draws_.emplace_back();
    d.mesh = mesh;
    // Depth mesh and bounds resolved once per distinct mesh id, cached until beginScene().
    MeshSubmitCacheSlot* const cacheSlot =
        meshSubmitCacheEnabled_ ? &meshSubmitCacheSlot(mesh) : nullptr;

    if (cacheSlot) {
        if (!cacheSlot->depthProxyResolved) {
            cacheSlot->depthProxyMesh = depthProxyFn_ ? depthProxyFn_(mesh, depthProxyUser_) : 0;
            cacheSlot->depthProxyResolved = true;
        }
        d.depthMesh = cacheSlot->depthProxyMesh;
    } else {
        d.depthMesh = depthProxyFn_ ? depthProxyFn_(mesh, depthProxyUser_) : 0;
    }
    if (!d.depthMesh) d.depthMesh = mesh;
    std::memcpy(d.world, world, 16 * sizeof(f32));
    std::memcpy(d.color, baseColor, 4 * sizeof(f32));
    d.metallic = metallic;
    d.roughness = roughness;
    d.hiddenFromOwner = hiddenFromOwner;
    d.matSet = drawBinding;
    d.matBytes = drawConstantBytes < sizeof(d.mat) ? drawConstantBytes : static_cast<u32>(sizeof(d.mat));
    if (drawConstants && d.matBytes) std::memcpy(d.mat, drawConstants, d.matBytes);
    d.translucent = translucent;
    d.movable = movable;

    // World-space bounding sphere for per-cascade cull.
    // Only local-space bounds are cached; world-space transforms per entity (differs per instance).
    bool haveBounds = false;
    const f32* localCentre = nullptr;
    f32 localRadius = 0.0f;
    f32 localCentreScratch[3] = {};
    f32 localRadiusScratch = 0.0f;
    if (cacheSlot) {
        if (!cacheSlot->boundsResolved) {
            cacheSlot->haveBounds =
                dev_ && dev_->meshBounds(mesh, cacheSlot->localCentre, &cacheSlot->localRadius);
            cacheSlot->boundsResolved = true;
        }
        haveBounds = cacheSlot->haveBounds;
        localCentre = cacheSlot->localCentre;
        localRadius = cacheSlot->localRadius;
    } else {
        haveBounds = dev_ && dev_->meshBounds(mesh, localCentreScratch, &localRadiusScratch);
        localCentre = localCentreScratch;
        localRadius = localRadiusScratch;
    }
    if (haveBounds) {
        Mat4 w;
        std::memcpy(&w.m[0][0], world, sizeof(w.m));
        const Vec3 c = xformPoint(Vec3{localCentre[0], localCentre[1], localCentre[2]}, w);
        d.boundsCentre[0] = c.x; d.boundsCentre[1] = c.y; d.boundsCentre[2] = c.z;
        d.boundsRadius = localRadius * maxAxisScale(w);
    }
    // Record translucent and light-flagged facts discovered here, not per-pass.
    const u32 drawIndex = static_cast<u32>(draws_.size() - 1);
    if (d.translucent) translucentDraws_.push_back(drawIndex);
    if (d.matBytes >= sizeof(pbr::MaterialConstants)) {
        u32 matFlags = 0;
        std::memcpy(&matFlags, d.mat + offsetof(pbr::MaterialConstants, flags), sizeof(matFlags));
        if (matFlags & pbr::MaterialFlag_Light) lampDraws_.push_back(drawIndex);
    }
}

// IRenderFeature entry point for every draw. Blended draws filtered here: unfiltered,
// they would voxelise glass as opaque, cast black shadow, read as opaque to reflections.
void VoxiRenderer::submitDraw(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                              f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                              const void* drawConstants, u32 drawConstantBytes, bool blended) {
    if (blended) {
        // Translucent lane: marked non-opaque in TLAS so rays attenuate through;
        // excluded from cascade/GI shadow/voxelisation (depth-only, no transmittance).
        ++blendedDropped_;
        if ((blendedDropLogs_ & (blendedDropLogs_ + 1)) == 0) {
            AVER_INFO("[Voxi] {} translucent draw(s) routed to the RT lane so far -- in the TLAS as "
                      "non-opaque (attenuated, tinted shadows where ray tracing is active), still "
                      "out of the cascade, the GI shadow map and voxelisation, which are depth-only "
                      "and cannot express a transmittance",
                      blendedDropped_);
        }
        ++blendedDropLogs_;
        submit(mesh, world, baseColor, metallic, roughness, drawBinding, drawConstants,
               drawConstantBytes, /*translucent=*/true, /*hiddenFromOwner=*/false, submitMovable_);
        return;
    }
    submit(mesh, world, baseColor, metallic, roughness, drawBinding, drawConstants, drawConstantBytes,
           /*translucent=*/false, /*hiddenFromOwner=*/false, submitMovable_);
}

// Runs Voxi's frame: acceleration structures, shadow map, voxelise, filter volume.
void VoxiRenderer::prePass(rhi::IRenderContext& ctx) {
    rdVisWrittenThisFrame_ = false;
    // THE FRAME BOUNDARY for pipelines. Finished builds are adopted and new ones started here, before anything
    // of this frame has been recorded, so a frame sees one consistent set of handles from its first command to
    // its last. Everything below runs only when the set it needs stands; otherwise the device is told (through
    // suppressesWholeFrame) to leave the frame blank, and no pass of ours records at all.
    pumpBuilds(false);
    if (!canRecord()) return;
    ++rtFrameIndex_;
    // Sample frame time at the TOP of the feature's frame.
    if (frameTimeReport_) {
        const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if (frameTimeLastNs_ != 0 && frameTimeSeen_ >= kFrameTimeWarmup)
            frameTimeMs_.push_back(static_cast<f32>(now - frameTimeLastNs_) * 1e-6f);
        frameTimeLastNs_ = now;
        ++frameTimeSeen_;
        if (frameTimeMs_.size() && frameTimeMs_.size() % 120 == 0) reportFrameTime("running");
    }
    // Backdrop t10: re-bind only when handle changes.
    // A 0 handle MUST clear the slot, not skip it (GetDimensions() is not a validity test).
    if (dev_ && bindings_) {
        const rhi::TextureHandle bd = dev_->sceneColorBackdropTexture();
        if (bd != boundBackdrop_) {
            boundBackdrop_ = bd;
            if (bd) res_->setSrv(bindings_, 10, bd);
            else    res_->clearSrv(bindings_, 10);
        }
    }

    materials_.update();
    // Foliage BLAS dropped if its mesh was destroyed.
    for (rhi::BlasHandle b : foliageBlas_) {
        if (res_->blasMesh(b) != 0) continue;
        AVER_WARN("[Voxi] foliage dropped: one of its meshes was destroyed while the set still named it "
                  "(clearFoliage must come first)");
        clearFoliage();
        break;
    }
    refreshFoliageBindings();
    // Wireframe view: nothing below is on screen.
    if (paused_) return;
    const f32 size = extent_ * 2.0f;
    cb_.voxelOrigin[0] = center_[0] - extent_;
    cb_.voxelOrigin[1] = center_[1] - extent_;
    cb_.voxelOrigin[2] = center_[2] - extent_;
    cb_.voxelOrigin[3] = size > 0.0f ? 1.0f / size : 0.0f;
    cb_.voxelParams[0] = static_cast<f32>(voxelResBuilt_);
    cb_.voxelParams[1] = settings_.giIntensity;
    cb_.giParams[0]    = static_cast<f32>(settings_.giCones);
    // Editor view mode: viewDebug_ takes priority over unlit_.
    cb_.viewParams[0] = viewDebug_ != ViewDebug::None ? static_cast<f32>(viewDebug_) : (unlit_ ? 1.0f : 0.0f);
    // Live GI radiance ceiling (AVER_VOX_MAXRAD).
    cb_.viewParams[1] = settings_.giRadianceCeiling;
    // Whether ReSTIR GI is the chosen estimator.
    cb_.viewParams[2] = giRestirWanted() ? 1.0f : 0.0f;
    cb_.viewParams[3] = 0.0f;
    // LOCAL LIGHTS: count and flags, 0 for prePass's own passes (none of them light with lamps).
    cb_.cameraMedium[2] = 0.0f;
    cb_.cameraMedium[3] = 0.0f;
    // Coherence tile edge (sent unconditionally; shader divides by it).
    cb_.ambientParams[1] = static_cast<f32>(std::max(settings_.giSkyOcclusionTile, 1u));
    // Lighting-contrast legacy bitmask (sent unconditionally every frame).
    cb_.ambientParams[2] = static_cast<f32>(lightingLegacyBits_);
    // Refraction rides giParams' spare .yzw; mirrored in multiple places.
    cb_.giParams[1]    = static_cast<f32>(settings_.refractionMode);
    cb_.giParams[2]    = settings_.refractionStrength;
    cb_.giParams[3]    = settings_.refractionEdgeFade;
    cb_.voxelParams[2] = settings_.giMaxDistance;
    // Cone trace gate (separate from giEnabled() for A/B measurement).
    cb_.voxelParams[3] = (giEnabled() && !debugView_ && coneTraceEnabled_) ? 1.0f : 0.0f;

    // Staged ray-driven bit-field toggles (packed from scratch each frame).
    cb_.giShadowParams[3] = static_cast<f32>((settings_.rtSecondaryShadowOpaque ? 1u : 0u) |
                                              (settings_.rtSkyOcclusionHalfRate ? 2u : 0u) |
                                              (settings_.rtReflectionHalfRate  ? 4u : 0u) |
                                              (settings_.rtGiHitShadowMap      ? 8u : 0u));

    // Scope: Voxi GI update, containing all passes below.
    rhi::ScopedGpuStat voxiGpuStat(ctx, "Voxi GI update");
    // Recreate/free injection accumulator before bindings_ first binds.
    manageInjectionAccumulator(ctx);
    buildAccelerationStructures(ctx);
    // Bit 32: TLAS holds no translucent-lane instance, so sun-shadow rays take first-hit query.
    if (rtActive_) {
        const bool firstHit = rtTlasTranslucent_ == 0;
        if (firstHit)
            cb_.giShadowParams[3] = static_cast<f32>(static_cast<u32>(cb_.giShadowParams[3]) | 32u);
        const u8 state = static_cast<u8>(firstHit ? 1u : 2u);
        if (state != rtShadowFirstHitLogged_) {
            rtShadowFirstHitLogged_ = state;
            if (firstHit)
                AVER_INFO("[Voxi] sun shadows: first-hit query ON -- no translucent instance in the "
                          "acceleration structure, so there is nothing for the transmittance walk to tint");
            else
                AVER_INFO("[Voxi] sun shadows: first-hit query OFF -- {} translucent instance(s) in the "
                          "acceleration structure; shadow rays walk transmittance", rtTlasTranslucent_);
        }
    }
    buildLocalLights();
    buildDecals();
    // Radiance cache: create/destroy, record clear dispatch, rebind t22/u20/u21 BEFORE shadowPass.
    updateNeuRaC(ctx);
    // Sky-occlusion rays: gated on rtActive_ (no TLAS = nowhere to trace).
    cb_.ambientParams[0] = rtActive_ ? static_cast<f32>(std::min(settings_.giSkyOcclusionRays,
                                                                 kMaxShadowRays))
                                     : 0.0f;
    beginShadowHistory(ctx);
    shadowPass(ctx);
    if (giEnabled() && giBuild_.active) {
        // A STAGED REBUILD in progress (giBuildStep): one more slice this frame, gate untouched.
        giBuildStep(ctx);
    } else if (giEnabled()) {
        // Amortised revoxelisation: skip voxelize/filter on off frames, cone trace samples last-built volume.
        if (giUpdateInterval_ <= 1 || ((rtFrameIndex_ - 1) % giUpdateInterval_) == 0) {
            // GI rebuild gate: if nothing moved, volume texture already holds the answer.
            const bool converging = giConvergeTicks_ > 0;
            const bool gateUnchanged = giSnapshotUnchanged();
            if (gateUnchanged && !converging && !giForceRebuild_) {
                ++giSkipped_;
                if (giConvergeTicks_ == 0) ++giQuietTicks_;
                if (giCacheSettlePending_ && giQuietTicks_ >= kGiCacheDwellTicks && giCacheScheduleDump(ctx))
                    giCacheSettlePending_ = false;
            } else {
                giQuietTicks_ = 0;
                // Accumulator may be missing if freed; giSnapshotUnchanged() retries next tick.
                if (!voxelAccumTex_) {
                    giAccumWanted_ = true;
                } else {
                    ++giRebuilt_;
                    if (converging) --giConvergeTicks_;
                    else            giConvergeTicks_ = kGiConvergeTicks;
                    takeGiSnapshot();
                    // A rebuild bigger than one frame's slice is STAGED over several frames (giBuildStep):
                    // in one submission NeonDistrict's GI shadow map + voxelisation is ~1.4 s of GPU work,
                    // which with the frame around it crosses Windows' 2 s timeout (device lost on load).
                    const bool staged =
                        giBuildTriangles(0, drawsPrev_.size(), kGiBuildTrisPerFrame) > kGiBuildTrisPerFrame;
                    if (staged) {
                        giBuild_ = GiBuild{};
                        giBuild_.active = true;
                        giBuild_.tryCache = !giForceRebuild_;
                        giBuild_.settleCloudOnly = !gateUnchanged;
                        giBuildStep(ctx);
                    } else {
                        // GI-only map must exist before injection reads it.
                        giShadowPass(ctx, 0, drawsPrev_.size(), true, true);
                        // Cache read: between "gate says rebuild" and rebuild itself.
                        if (giForceRebuild_ || !giCacheRestore(ctx)) {
                            voxelizePass(ctx, 0, drawsPrev_.size(), true, true);
                            filterMips(ctx);
                            if (!gateUnchanged)
                                giCacheSettleCloudOnly_ =
                                    (giCacheSettlePending_ ? giCacheSettleCloudOnly_ : true) && giRebuildCloudOnly_;
                            giCacheSettlePending_ = true;
                        }
                    }
                }
            }
            giCacheTick();
            // Report gate efficiency as a ratio.
            const u64 ticks = giSkipped_ + giRebuilt_;
            if (ticks >= giGateNextReport_) {
                const u64 winTicks = ticks - giGateLastTicks_;
                const u64 winSkipped = giSkipped_ - giGateLastSkipped_;
                AVER_INFO("[Voxi] GI rebuild gate: {} rebuilt / {} skipped of {} tick(s) "
                          "-- {}% avoided overall, {}% since the last report",
                          giRebuilt_, giSkipped_, ticks, (giSkipped_ * 100) / ticks,
                          winTicks ? (winSkipped * 100) / winTicks : 0);
                giGateLastTicks_ = ticks;
                giGateLastSkipped_ = giSkipped_;
                giGateNextReport_ = ticks * 2;
            }
        }
    }
    // Air sky-visibility refresh: round-robin slabs, decoupled from GI rebuild gate. Idle once a full
    // cycle has run against an unchanged volume (CSAirVis reads only the volume, its origin and resolution).
    const bool airVisActive = airVisTex_ && airVisPso_ && voxelTex_ && cb_.voxelParams[3] > 0.5f;
    if (airVisActive && !airVisWasActive_) airVisDirty_ = true;
    airVisWasActive_ = airVisActive;
    if (airVisActive) {
        constexpr u32 kSlabs = kAirVisResolution / kAirVisSlabLayers;
        const f32 inputs[5] = {cb_.voxelOrigin[0], cb_.voxelOrigin[1], cb_.voxelOrigin[2], cb_.voxelOrigin[3],
                               cb_.voxelParams[0]};
        if (voxelGen_ != airVisGen_ || std::memcmp(inputs, airVisInputs_, sizeof(inputs)) != 0) {
            airVisGen_ = voxelGen_;
            std::memcpy(airVisInputs_, inputs, sizeof(inputs));
            airVisLeft_ = kSlabs;
        }
        const bool full = airVisDirty_;
        if (full || airVisLeft_ > 0) {
            ctx.textureBarrier(voxelTex_, rhi::ResourceState::ShaderResource,
                               rhi::ResourceState::NonPixelShaderResource);
            if (full) {
                dispatchAirVis(ctx, 0, kAirVisResolution);
                airVisLeft_ = 0;
            } else {
                const u32 zLo = (airVisSlab_ % kSlabs) * kAirVisSlabLayers;
                dispatchAirVis(ctx, zLo, zLo + kAirVisSlabLayers);
                --airVisLeft_;
            }
            ctx.textureBarrier(voxelTex_, rhi::ResourceState::NonPixelShaderResource,
                               rhi::ResourceState::ShaderResource);
        }
        if (!full) airVisSlab_ = (airVisSlab_ + 1) % kSlabs;
    }
    endShadowHistory();

    // Raise lamp constants for raster scene draws; u19 marked written here.
    if (!rayDrivenActive() &&
        publishLocalLights(localLightsReady(), "the raster scene draws (PSMainVoxi)")) {
        rdLocalHistFrame_ = rtFrameIndex_;
        rdLocalHistHash_ = rdLocalLightHash_;
    }
    // Report VRAM after all rebuild/free decisions.
    reportVramUsage();
}

// Recreate or free the injection accumulator for this frame, called from prePass before buildAccelerationStructures.
// A PLACEHOLDER, NOT A NULL BIND, while the accumulator is gone: bindings_/clearBindings_/resolveBindings_ all declare slot 1.
void VoxiRenderer::manageInjectionAccumulator(rhi::IRenderContext& ctx) {
    (void)ctx;   // createTexture/destroyTexture go through res_, not the command list
    if (!voxelAccumTex_ && voxelResBuilt_ && bindings_ && (giAccumWanted_ || !giFreeAccumulator_)) {
        // Recreate: either giAccumWanted_ is set or the flag is off.
        // giAccumRecreateBackoffTicks_ backs off after failed OOM allocations.
        if (giAccumRecreateBackoffTicks_) {
            --giAccumRecreateBackoffTicks_;
        } else if (createInjectionAccumulator(voxelResBuilt_)) {
            res_->setUav(bindings_, 1, voxelAccumTex_, 0);
            res_->setUav(clearBindings_, 1, voxelAccumTex_, 0);
            res_->setUav(resolveBindings_, 1, voxelAccumTex_, 0);
            giAccumWanted_ = false;
            giQuietTicks_ = 0;
            giAccumRecreateFailedLogged_ = false;
            giAccumRecreateBackoffNext_ = 0;   // the next failure, if any, starts cold again
            const f64 mib = static_cast<f64>(static_cast<u64>(voxelResBuilt_) * voxelResBuilt_ *
                                             voxelResBuilt_ * 16ull) / (1024.0 * 1024.0);
            AVER_INFO("[Voxi] injection accumulator recreated ({:.0f} MiB); the rebuild that needed "
                      "it runs on the next tick", mib);
        } else {
            if (!giAccumRecreateFailedLogged_) {
                giAccumRecreateFailedLogged_ = true;
                AVER_ERROR("[Voxi] injection accumulator could not be recreated at {}^3; GI stays "
                           "without a volume to inject into until it can be", voxelResBuilt_);
            }
            giAccumRecreateBackoffTicks_ = giAccumRecreateBackoffNext_
                ? giAccumRecreateBackoffNext_ : kGiAccumRecreateBackoffMin;
            giAccumRecreateBackoffNext_ = std::min(giAccumRecreateBackoffTicks_ * 2,
                                                    kGiAccumRecreateBackoffMax);
        }
    } else if (giFreeAccumulator_ && voxelAccumTex_ && giEnabled() && !giForceRebuild_ &&
              giConvergeTicks_ == 0 && giQuietTicks_ >= kGiAccumulatorQuietTicks) {
        // Free: rebind to placeholder first (aver-view-outlives-its-buffer.md), then destroy.
        if (!voxelAccumPlaceholder_) {
            rhi::TextureDesc pd;
            pd.dim    = rhi::TextureDim::Tex3D;
            pd.width  = 4; pd.height = 1; pd.depth = 1;
            pd.mips   = 1;
            pd.format = rhi::Format::R32Uint;
            pd.bind   = rhi::ResourceBind::UnorderedAccess;
            pd.initialState = rhi::ResourceState::UnorderedAccess;
            pd.debugName    = "Voxi injection accumulator placeholder";
            voxelAccumPlaceholder_ = res_->createTexture(pd);
        }
        if (voxelAccumPlaceholder_) {
            res_->setUav(bindings_, 1, voxelAccumPlaceholder_, 0);
            res_->setUav(clearBindings_, 1, voxelAccumPlaceholder_, 0);
            res_->setUav(resolveBindings_, 1, voxelAccumPlaceholder_, 0);
            const f64 mib = static_cast<f64>(static_cast<u64>(voxelResBuilt_) * voxelResBuilt_ *
                                             voxelResBuilt_ * 16ull) / (1024.0 * 1024.0);
            // FENCE-DEFERRED, NOT IMMEDIATE, ON BOTH BACKENDS -- and said as "queued", not "released",
            // for exactly that reason. destroyTexture retires the resource behind the fence value
            // in flight right now; both D3D12ResourceFactory::collect() and its Vulkan twin only walk
            // that retired list and Release()/vkDestroyImage the ones whose fence has already passed,
            // and neither runs on a timer -- each is a side effect of some OTHER create*/destroy* call
            // on the SAME factory. In an otherwise idle scene (which is exactly when W12 fires:
            // kGiAccumulatorQuietTicks quiet GI ticks) nothing may call the factory again for a while, so this retired texture
            // can sit un-reclaimed past the frames a process-VRAM reading was taken over -- a real gap,
            // not a rounding artefact of the reading. A fix belongs in the backends (not this module):
            // an unconditional collect() once per frame, e.g. beside D3D12Device::beginFrame's existing
            // collectGpuTiming()/collectExposureReadout() calls and the Vulkan equivalent, so a retired
            // resource is reclaimed within kFrameCount frames of retiring regardless of what else the
            // factory is asked to do that frame.
            res_->destroyTexture(voxelAccumTex_);
            voxelAccumTex_ = 0;
            AVER_INFO("[Voxi] injection accumulator freed after {} quiet GI tick(s): {:.0f} MiB "
                      "queued for release (fence-deferred)", giQuietTicks_, mib);
        }
    }
}

// Builds BLAS for each mesh and TLAS over drawsPrev_. Publishes shadowParams.z.
// rtSkipUnchangedTlas gates TLAS rebuild on rtAccelSnapshotUnchanged(); rtRefitAccel enables refits.
void VoxiRenderer::buildAccelerationStructures(rhi::IRenderContext& ctx) {
    // Read before rtActive_ resets: the device asked the same at beginFrame, so a late scene pass follows.
    const bool lateScene = wantsLateScenePass();
    rtActive_ = false;
    cb_.shadowParams[2] = 0.0f;
    rtAccelListKeyValid_ = false;
    if (!rtSupported_ || settings_.rayTracing == Quality::Off || (drawsPrev_.empty() && foliageInstances_ == 0)) return;

    // ---- The unchanged gate (Settings::rtSkipUnchangedTlas) ----
    // See rtAccelSnapshotUnchanged() for what "unchanged" checks. A match means tlas_,
    // rtInstanceData_ and every SRV bound to them (slots 2/3/4/5/9, set the last time the per-draw
    // loop actually ran -- slot 5 aside, which a mover patch re-binds) are still correct (the TRANSFORMS of movable draws aside: the key leaves
    // them out and the mover patch lane below brings them up to date), so the whole body below (the
    // per-draw loop, the material and geometry tables) is skipped -- except what's NOT a function of
    // drawsPrev_ and is read every
    // frame regardless: rtActive_, cb_.shadowParams[2], cb_.rtParams (updateRtParamsPerFrame(),
    // factored out for this reuse), cb_.rtParams[3] from rtGeometryReady_ rather than a fresh
    // buildGeometryTable() call, and -- when Settings::rtRefitAccel is on and rtDynamicMeshes_ is
    // non-empty -- the dynamic BLASes/tlas_/their rtVerts_ slices, refreshed in place by
    // refitDynamicAccelStructures() rather than left alone.
    // A build deferred by the per-frame BLAS budget must get its turn, whatever the snapshot says.
    if (settings_.rtSkipUnchangedTlas && !blasBuildsDeferred_ && rtAccelSnapshotUnchanged()) {
        // SETTLING (rtPrevPending_, VoxiRenderer.hpp): rows whose previous transform still differs from
        // their current one describe LAST frame's motion. If the object moves again this frame the patch
        // below writes a fresh prev; if it does not, prev must become equal to current NOW, or the GPU
        // keeps reporting that old motion for as long as the table goes un-uploaded. Done before the
        // patch so the patch only ever has to think about this frame's movers.
        //
        // Harmless if the patch is then Refused: the full build that follows carries forward from each
        // row's objectToWorld (what was drawn), never from its prevObjectToWorld, so a row settled here
        // loses nothing -- the full build recomputes every prev from scratch, and rebuilds
        // rtPrevPending_ with it. The settled CPU rows are simply never uploaded.
        bool settled = false;
        for (const u32 row : rtPrevPending_) {
            if (row >= rtInstanceData_.size()) continue;
            RtInstance& r = rtInstanceData_[row];
            std::memcpy(r.prevObjectToWorld, r.objectToWorld, sizeof(r.prevObjectToWorld));
            settled = true;
        }
        // Mover patch lane: rtMovers_ collects current movers; empty means lane off or no movable draws.
        // With a late scene pass the instance upload and refit wait for latePatchMovers: one per frame, fed
        // with this frame's transforms. owed: an earlier frame deferred them and its late pass never ran.
        const bool defer = lateScene;
        const bool owed = rtRefitDeferred_;
        const MoverPatch patch = rtMovers_.empty() ? MoverPatch::Unchanged : patchRtMovers(drawsPrev_, false, defer);
        if (patch != MoverPatch::Refused) {
            const bool gpuDue = patch == MoverPatch::Patched || owed;
            bool settleUploadFailed = false;
            // MaterialsOnly, and Patched unless deferred, already uploaded the instance table (settled rows included).
            const bool uploaded = patch == MoverPatch::MaterialsOnly || (patch == MoverPatch::Patched && !defer);
            if (!uploaded && !(gpuDue && defer) && (settled || owed))
                settleUploadFailed = !uploadRtInstanceTable();
            if (!settleUploadFailed) {
                rtPrevPending_.clear();
                for (const RtMover& m : rtMovers_) {
                    if (m.inst == kRtNoInstance || m.inst >= rtInstanceData_.size()) continue;
                    const RtInstance& r = rtInstanceData_[m.inst];
                    if (std::memcmp(r.prevObjectToWorld, r.objectToWorld, sizeof(r.objectToWorld)) != 0)
                        rtPrevPending_.push_back(m.inst);
                }
            }
            if (gpuDue) {
                if (defer) {
                    rtRefitDeferred_ = true;
                } else {
                    rtRefitDeferred_ = false;
                    if (!rtDynamicMeshes_.empty()) {
                        refitDynamicAccelStructures(ctx);
                    } else {
                        rhi::ScopedGpuStat gpuStat(ctx, "Voxi acceleration structures");
                        refitOrRebuildTlas(ctx);
                    }
                }
                ++rtAccelMoverPatched_;
            } else if (settings_.rtRefitAccel && !rtDynamicMeshes_.empty()) {
                refitDynamicAccelStructures(ctx);
                ++rtAccelRefitOnly_;
            } else {
                ++rtAccelSkipped_;
            }
            rtActive_ = true;
            cb_.shadowParams[2] = 1.0f;
            updateRtParamsPerFrame();
            cb_.rtParams[3] = rtGeometryReady_ ? 1.0f : 0.0f;
            reportRtAccelGate();
            return;
        }
    }
    if (settings_.rtSkipUnchangedTlas) ++rtAccelRebuilt_;
    rtRefitDeferred_ = false;   // the build below rewrites and uploads every table

    rhi::ScopedGpuStat gpuStat(ctx, "Voxi acceleration structures");
    tlasTranslucentThisBuild_ = 0;
    tlasAlphaMaskedThisBuild_ = 0;
    // tlasInstScratch_/matConstantsScratch_: hoisted members, .clear() keeps storage for reuse.
    tlasInstScratch_.clear();
    matConstantsScratch_.clear();
    tlasInstScratch_.reserve(drawsPrev_.size());
    // CARRY-FORWARD: rtInstanceData_ holds last frame's transforms until cleared; build carry-forward lookup first.
    buildRtCarryLookup();
    rtInstanceData_.clear();
    rtInstanceMesh_.clear();
    rtInstanceMatKey_.clear();
    rtInstanceGroupKey_.clear();
    rtPrevPending_.clear();
    rebuiltThisFrame_.clear();
    rtDynamicMeshes_.clear();
    // Mover patch lane's record: filled by loop, finished after it.
    const bool moverLane = rtMoverPatchActive();
    rtMovers_.clear();
    DrawMaterialMemo moverMemo{};
    u32 firstBuilds = 0;
    // Each new BLAS takes its own build scratch until the frame's fence passes, so a level's whole
    // build wave in one frame held ~2.6 GB at once on NeonDistrict. Builds past the budget wait.
    u64 blasBytesThisFrame = 0;
    u32 blasDeferred = 0;

    // CPU cost of walking drawsPrev_ and filling rtInstanceData_/rtInstanceMesh_/rtInstanceMatKey_.
    const auto accelBuildCpuStart = std::chrono::steady_clock::now();

    for (const Draw& d : drawsPrev_) {
        // Mover patch lane: record this movable draw before BLAS check (must sync with current list).
        u32 moverSlot = kRtNoInstance;
        if (moverLane && d.movable) {
            moverSlot = static_cast<u32>(rtMovers_.size());
            rtMovers_.push_back({rtDrawHash(d, moverMemo, true), static_cast<u32>(&d - drawsPrev_.data()),
                                 kRtNoInstance});
        }

        auto it = blas_.find(d.mesh);
        // Mesh destroyed beneath us: handing stale structure to TLAS would have GPU traverse freed memory.
        if (it != blas_.end() && it->second && res_->blasMesh(it->second) != d.mesh) {
            blas_.erase(it);
            ++blasRevision_;
            it = blas_.end();
            dynamicBlasRefits_.erase(d.mesh);
        }
        if (it == blas_.end()) {
            if (blasBytesThisFrame >= kBlasBuildBytesPerFrame) { ++blasDeferred; continue; }
            // createBlas returns 0 for destroyed mesh. Updatable only for compute-written mesh with rtRefitAccel on.
            const bool dynamic = settings_.rtRefitAccel && dev_->meshVertexBuffer(d.mesh) != 0;
            const rhi::BlasHandle nb = dynamic ? res_->createBlasUpdatable(d.mesh) : res_->createBlas(d.mesh);
            if (nb) {
                ctx.buildBlas(nb);
                ++firstBuilds;
                blasBytesThisFrame += res_->blasMemoryBytes(nb);   // scratch is of the same order
            }
            it = blas_.emplace(d.mesh, nb).first;
            ++blasRevision_;
        } else if (it->second && dev_->meshVertexBuffer(d.mesh)) {
            // Compute-written mesh: structure invalidated every frame. Refit if rtRefitAccel is on.
            if (std::find(rebuiltThisFrame_.begin(), rebuiltThisFrame_.end(), d.mesh) ==
                rebuiltThisFrame_.end()) {
                refitOrRebuildDynamicBlas(ctx, it->second, d.mesh);
                rebuiltThisFrame_.push_back(d.mesh);
            }
            if (!dynamicBlasLogged_) {
                AVER_INFO("[Voxi] mesh {} has compute-written vertices; its bottom-level structure is "
                          "refit (or rebuilt, if voxi.rtRefitAccel is off) every frame rather than "
                          "cached outright", d.mesh);
                dynamicBlasLogged_ = true;
            }
        }
        const rhi::BlasHandle b = it->second;
        if (!b) continue;
        // Compute-written meshes with usable BLAS: refreshed by refitDynamicAccelStructures().
        if (dev_->meshVertexBuffer(d.mesh) &&
            std::find(rtDynamicMeshes_.begin(), rtDynamicMeshes_.end(), d.mesh) == rtDynamicMeshes_.end()) {
            rtDynamicMeshes_.push_back(d.mesh);
        }
        rhi::TlasInstance i;
        std::memcpy(i.world, d.world, sizeof(i.world));
        // Three mask lanes: translucency, hiddenFromOwner, and opaque. Cannot co-occur.
        i.mask = d.translucent      ? kRtMaskTranslucent
               : d.hiddenFromOwner  ? kRtMaskOwnerHidden
                                    : kRtMaskOpaque;
        // ForceNonOpaque only for translucent: geometry is marked OPAQUE by createBlas().
        i.flags = d.translucent ? rhi::TlasInstanceFlag_ForceNonOpaque : rhi::TlasInstanceFlag_None;
        if (d.translucent) ++tlasTranslucentThisBuild_;
        i.blas = b;
        // Index into rtInstanceData_ for reflection ray hits.
        i.instanceId = static_cast<u32>(rtInstanceData_.size()) & rhi::kMaxTlasInstanceId;
        tlasInstScratch_.push_back(i);

        RtInstance ri;
        std::memcpy(ri.objectToWorld, d.world, sizeof(ri.objectToWorld));
        ri.albedo[0] = d.color[0]; ri.albedo[1] = d.color[1]; ri.albedo[2] = d.color[2];
        ri.metallic = d.metallic;
        ri.roughness = d.roughness;
        ri.firstIndex = 0;
        ri.firstVertex = 0;
        // Placeholder: buildMaterialTable() overwrites after material set is sorted.
        ri.materialIndex = 0;
        // Carried from last frame or zero if new instance.
        const u64 groupKey = rtInstanceGroupKey(d.mesh, d.matSet);
        carryPrevTransform(groupKey, d.world, ri.prevObjectToWorld);

        const u64 matKey = rtMaterialKey(d.matSet, d.mat, d.color, d.metallic, d.roughness);
        rtInstanceMatKey_.push_back(matKey);

        // Alpha-masked geometry joins non-opaque lane; ray-driven was wrong.
        // Patch onto instance already pushed (matKey known here).
        // Stay in OPAQUE lane (occludes, shadows, reflection hit), only set ForceNonOpaque for any-hit.
        if (!d.translucent &&
            (matConstantsScratch_.at(matKey).flags & pbr::MaterialFlag_AlphaMask) != 0) {
            tlasInstScratch_.back().flags |= rhi::TlasInstanceFlag_ForceNonOpaque;
            ++tlasAlphaMaskedThisBuild_;
        }

        if (moverSlot != kRtNoInstance) rtMovers_[moverSlot].inst = static_cast<u32>(rtInstanceData_.size());
        if (std::memcmp(ri.prevObjectToWorld, ri.objectToWorld, sizeof(ri.objectToWorld)) != 0)
            rtPrevPending_.push_back(static_cast<u32>(rtInstanceData_.size()));
        rtInstanceData_.push_back(ri);
        rtInstanceMesh_.push_back(d.mesh);
        rtInstanceGroupKey_.push_back(groupKey);
    }

    // Finalize mover patch lane record: sort movers or clear if none have instances.
    if (!rtMovers_.empty()) {
        const bool anyInstance = std::any_of(rtMovers_.begin(), rtMovers_.end(),
                                             [](const RtMover& m) { return m.inst != kRtNoInstance; });
        if (anyInstance) std::sort(rtMovers_.begin(), rtMovers_.end());
        else rtMovers_.clear();
    }

    // End of CPU-only work; time the below or use previous value on early return.
    lastAccelBuildCpuMs_ = std::chrono::duration<f64, std::milli>(
        std::chrono::steady_clock::now() - accelBuildCpuStart).count();

    // Exit on empty draw list; foliage alone is still worth building.
    if (tlasInstScratch_.empty() && foliageInstances_ == 0) return;

    // New foliage set's BLASes before TLAS that references them.
    if (foliageBlasPending_) {
        for (rhi::BlasHandle b : foliageBlas_) ctx.buildBlas(b);
        foliageBlasPending_ = false;
    }

    // Refits TLAS in place if rtRefitAccel allows, else full build.
    refitOrRebuildTlas(ctx);
    if (tlasTranslucentThisBuild_ && tlasTranslucentLogged_ != tlasTranslucentThisBuild_) {
        tlasTranslucentLogged_ = tlasTranslucentThisBuild_;
        AVER_INFO("[Voxi] acceleration structure: {} instance(s), {} in the translucent lane "
                  "(non-opaque, visible to shadow rays only)",
                  tlasInstScratch_.size(), tlasTranslucentThisBuild_);
    }
    if (tlasAlphaMaskedThisBuild_ && tlasAlphaMaskedLogged_ != tlasAlphaMaskedThisBuild_) {
        tlasAlphaMaskedLogged_ = tlasAlphaMaskedThisBuild_;
        AVER_INFO("[Voxi] acceleration structure: {} instance(s) alpha-masked (non-opaque, so every "
                  "ray tests the cutout instead of hitting the card)", tlasAlphaMaskedThisBuild_);
    }
    rtActive_ = true;
    cb_.shadowParams[2] = 1.0f;

    // Not a function of drawsPrev_ (see updateRtParamsPerFrame()); also set by gate's skip branch.
    updateRtParamsPerFrame();
    // MATERIAL TABLE FIRST (load-bearing order): buildGeometryTable uploads rtInstanceData_;
    // buildMaterialTable fills materialIndex. Reversed order silently indexed fallback row.
    ensureTextureTable();
    resolveFoliageMaterials();
    const bool matTableReady = buildMaterialTable(matConstantsScratch_);
    // Reflection ray may trace only if geometry table exists.
    const bool geomTableReady = buildGeometryTable(ctx);
    cb_.rtParams[3] = geomTableReady ? 1.0f : 0.0f;
    // After both tables: foliage part records carry indices into each.
    if (geomTableReady) uploadFoliagePartTable();
    // STALE-POSE FIX: refreshes compute-skinned mesh slices every frame, not just on set change.
    if (geomTableReady) refreshDynamicVertexSlices(ctx);
    if (!rtLogged_) {
        AVER_INFO("[Voxi] RayQuery active ({} instances, {} bottom-level structures)",
                  static_cast<u32>(tlasInstScratch_.size()), static_cast<u32>(blas_.size()));
        rtLogged_ = true;
    }
    blasBuildsDeferred_ = blasDeferred > 0;
    if (blasDeferred && !blasDeferLogged_) {
        AVER_INFO("[Voxi] bottom-level builds spread over frames: {:.0f} MiB this frame, {} draw(s) wait "
                  "for the next (budget {} MiB/frame)", static_cast<f64>(blasBytesThisFrame) / (1024.0 * 1024.0),
                  blasDeferred, kBlasBuildBytesPerFrame >> 20);
        blasDeferLogged_ = true;
    }
    // First-time build is always full. Dynamic mesh refits tracked separately.
    const u32 rebuilds = (static_cast<u32>(rebuiltThisFrame_.size()) << 16) | (firstBuilds & 0xFFFFu);
    if (rebuilds != lastBlasRebuilds_) {
        AVER_INFO("[Voxi] bottom-level builds this frame: {} ({} first-time, {} refit/rebuilt) over "
                  "{} draws of {} distinct meshes; lifetime dynamic BLAS {} refit / {} rebuilt, TLAS "
                  "{} refit / {} rebuilt",
                  firstBuilds + static_cast<u32>(rebuiltThisFrame_.size()), firstBuilds,
                  static_cast<u32>(rebuiltThisFrame_.size()), static_cast<u32>(drawsPrev_.size()),
                  static_cast<u32>(blas_.size()), rtDynamicBlasRefits_, rtDynamicBlasRebuilds_,
                  rtTlasRefits_, rtTlasRebuilds_);
        lastBlasRebuilds_ = rebuilds;
    }

    // Gate bookkeeping: negligible cost next to TLAS build. Turning on mid-session primes after one full build.
    takeRtAccelSnapshot();
    // Patch re-sends rtInstanceData_ as left by buildGeometryTable, safe only over complete tables.
    // If geometry/material table incomplete, forget snapshot and retry full build next tick.
    if (!rtMovers_.empty() && (!geomTableReady || !matTableReady)) {
        rtMovers_.clear();
        rtAccelSnapValid_ = false;
    }
    if (settings_.rtSkipUnchangedTlas) reportRtAccelGate();
}

// cb_.rtParams[0..2]: sun's angular size (tangent), shadow ray count, ray bias.
// Not a function of drawsPrev_; read every frame by shadowPass() / PSRayDriven.
void VoxiRenderer::updateRtParamsPerFrame() {
    // Angular softness = sun's angular size; disc subtends ~0.5 deg.
    const f32 discDeg = dev_->skyAtmosphere().sunAngularDiameterDeg;
    const f32 halfAngle = discDeg * 0.5f * 0.01745329252f;
    cb_.rtParams[0] = std::tan(halfAngle);
    // Ray count based on disc: ceil(disc / 0.3 deg), at least 2.
    rtShadowRaysUsed_ = std::min(rtShadowRays_, std::max(2u, static_cast<u32>(std::ceil(discDeg / 0.3f))));
    cb_.rtParams[1] = static_cast<f32>(rtShadowRaysUsed_);
    // Base bias in cm, scaled by view distance in shader. Avoids contact-shadow detach and self-intersection.
    cb_.rtParams[2] = 0.05f;
}

// True when buildAccelerationStructures() must run per-draw loop regardless of rtAccelDrawsKey().
// Cheap check: unordered_map lookup and at most one virtual call per distinct mesh.
// rtAccelMeshChecked_ is a lossy direct-mapped filter; collision re-checks, never skips unchecked mesh.
bool VoxiRenderer::rtAccelMustForceRebuild() const {
    // A slot counts as 0 unless stamped this call, so the table is never cleared per call.
    if (rtAccelMeshChecked_.size() != kRtAccelMeshCheckSlots)
        rtAccelMeshChecked_.assign(kRtAccelMeshCheckSlots, {});
    if (++rtAccelMeshStamp_ == 0) {
        rtAccelMeshChecked_.assign(kRtAccelMeshCheckSlots, {});
        rtAccelMeshStamp_ = 1;
    }
    for (const Draw& d : drawsPrev_) {
        MeshCheckSlot& slot =
            rtAccelMeshChecked_[static_cast<u32>(mixMeshId(d.mesh) & (kRtAccelMeshCheckSlots - 1))];
        const rhi::MeshHandle checked = slot.stamp == rtAccelMeshStamp_ ? slot.mesh : 0;
        if (checked == d.mesh) continue;
        slot.mesh = d.mesh;
        slot.stamp = rtAccelMeshStamp_;
        // Compute-skinned mesh: BLAS needs refresh every call; freezing silently shows stale pose.
        // Only forces loop when rtRefitAccel is off; when on, refit-only pass keeps current.
        if (!settings_.rtRefitAccel && dev_ && dev_->meshVertexBuffer(d.mesh)) return true;
        // Destroyed-and-reused mesh handle: BLAS is stale, key-only gate would leave TLAS dangling.
        const auto it = blas_.find(d.mesh);
        if (it != blas_.end() && it->second && res_->blasMesh(it->second) != d.mesh) return true;
    }
    return false;
}

// Lane cache entry for drawsPrev_[i]; an entry left from an earlier list is reset on first touch.
VoxiRenderer::DrawLanes* VoxiRenderer::drawLanes(usize i) const {
    if (drawLanes_.size() < drawsPrev_.size()) drawLanes_.resize(drawsPrev_.size());
    DrawLanes& e = drawLanes_[i];
    if (e.token != drawLanesToken_) { e.token = drawLanesToken_; e.have = 0; }
    return &e;
}

// d.world's bytes folded into `h`, reusing the draw's cached lanes when given.
void VoxiRenderer::hashWorldInto(u64& h, const Draw& d, DrawLanes* lanes) const {
    if (!lanes) { hashBytesInto(h, d.world, sizeof(d.world)); return; }
    if (!(lanes->have & DrawLanes::kWorld)) {
        hashBytesLanes(lanes->world, d.world, sizeof(d.world));
        lanes->have |= DrawLanes::kWorld;
    }
    hashFoldLanes(h, lanes->world);
}

// One draw's material identity into hash `h`, for rtAccelDrawsKey() and GI keys.
// 176-byte constant block hashed word-at-a-time; two MaterialSystem lookups per binding set, not per draw.
// d.matSet alone is not enough: in-place material edits bypass revision bumps; hash d.mat + texture set instead.
// skipEmissive hashes the block with emissiveFactor zeroed (mover lane: an emissive-only change keeps identity).
void VoxiRenderer::hashDrawMaterialInto(u64& h, const Draw& d, DrawMaterialMemo& memo, bool skipEmissive,
                                        DrawLanes* lanes) const {
    DrawMaterialMemoSlot& slot = memo[d.matSet & (kDrawMaterialMemoSlots - 1)];
    if (slot.state == 0 || slot.set != d.matSet) {
        slot.set = d.matSet;
        const bool owned = materials_.ownsBindingSet(d.matSet) &&
                           d.matSet != materials_.fallbackBindingSet();
        slot.state = 1u | (owned ? 2u : 0u);
        slot.texHash = 0;
        if (owned) {
            if (const auto* tex = materials_.textures(d.matSet)) {
                slot.state |= 4u;
                u64 th = 1469598103934665603ull;
                hashBytesInto(th, tex->data(), sizeof(*tex));
                slot.texHash = th;
            }
        }
    }
    const bool authored = (slot.state & 2u) != 0;
    h ^= static_cast<u64>(d.matSet); h *= 1099511628211ull;
    h ^= authored ? 1ull : 0ull; h *= 1099511628211ull;
    if (authored) {
        if (skipEmissive) {
            u8 bytes[sizeof(d.mat)];
            std::memcpy(bytes, d.mat, sizeof(bytes));
            std::memset(bytes + offsetof(pbr::MaterialConstants, emissiveFactor), 0, sizeof(f32) * 3);
            hashBytesInto(h, bytes, sizeof(bytes));
        } else if (lanes) {
            if (!(lanes->have & DrawLanes::kMat)) {
                hashBytesLanes(lanes->mat, d.mat, sizeof(d.mat));
                lanes->have |= DrawLanes::kMat;
            }
            hashFoldLanes(h, lanes->mat);
        } else {
            hashBytesInto(h, d.mat, sizeof(d.mat));
        }
        if (slot.state & 4u) { h ^= slot.texHash; h *= 1099511628211ull; }
    } else {
        // UNAUTHORED: per-draw loop builds constants from color/metallic/roughness alone.
        for (u32 i = 0; i < 4; ++i) {
            u32 bits = 0;
            std::memcpy(&bits, &d.color[i], sizeof(bits));
            h ^= static_cast<u64>(bits);
            h *= 1099511628211ull;
        }
        u32 mb = 0, rb = 0;
        std::memcpy(&mb, &d.metallic, sizeof(mb));
        std::memcpy(&rb, &d.roughness, sizeof(rb));
        h ^= (static_cast<u64>(mb) << 32) ^ static_cast<u64>(rb);
        h *= 1099511628211ull;
    }
}

// Mover patch lane active: movable draws' transforms bypass the key, patched in by patchRtMovers() instead.
// Needs both rtSkipUnchangedTlas and rtRefitAccel; with either off, all worlds stay in key.
bool VoxiRenderer::rtMoverPatchActive() const {
    return settings_.rtRefitAccel && settings_.rtSkipUnchangedTlas;
}

// One draw's term of rtAccelDrawsKey(): mesh, world, translucent/hiddenFromOwner flags, material.
// moverLane: drops movable draw's world, hashes movable bit instead (keeps changed movers in gate).
u64 VoxiRenderer::rtDrawHash(const Draw& d, DrawMaterialMemo& memo, bool moverLane, DrawLanes* lanes) const {
    const bool mover = moverLane && d.movable;
    u64 h = 1469598103934665603ull;
    h ^= static_cast<u64>(d.mesh); h *= 1099511628211ull;
    // 16 world floats as raw bits, eight bytes at a time.
    if (!mover) hashWorldInto(h, d, lanes);
    // Two flags hashed as bools.
    h ^= (d.translucent ? 1ull : 0ull) | (d.hiddenFromOwner ? 2ull : 0ull) | (mover ? 4ull : 0ull);
    h *= 1099511628211ull;

    // A mover's emissive stays out of its identity: patchRtMovers() re-keys the material row instead.
    hashDrawMaterialInto(h, d, memo, mover, lanes);

    // Finalize: decorrelate low bits before addition.
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

// Order-independent: sums per-draw hash (occlusion culling reshuffles drawsPrev_ every frame).
// Covers everything per-draw loop reads: mesh, world, translucent/hiddenFromOwner flags, material.
// Exception: movable draw world while mover patch lane on (hashes movable bit instead).
// rtAccelListKeyValid_ holds copy computed since buildAccelerationStructures() last began.
u64 VoxiRenderer::rtAccelDrawsKey(bool reuseListKey) const {
    if (!(reuseListKey && rtAccelListKeyValid_)) {
        u64 key = 0;
        u64 counted = 0;
        DrawMaterialMemo matMemo{};
        const bool moverLane = rtMoverPatchActive();
        rtMoversNow_.clear();
        u32 drawIndex = 0;
        for (const Draw& d : drawsPrev_) {
            const u64 h = rtDrawHash(d, matMemo, moverLane, drawLanes(drawIndex));
            // Also collects current movers for patchRtMovers() (avoids rescanning whole list).
            if (moverLane && d.movable) rtMoversNow_.push_back({h, drawIndex, kRtNoInstance});
            key += h;
            ++counted;
            ++drawIndex;
        }
        // Count mixed in rather than added (see GI key comment).
        key ^= counted * 1099511628211ull;
        rtAccelListKey_ = key;
        rtAccelListKeyValid_ = true;
    }
    // Foliage as one more term: new set or material edit both rebuild.
    return rtAccelListKey_ + foliageKey();
}

// True when nothing buildAccelerationStructures() reads has changed since last build.
// tlas_/rtInstanceData_/SRVs still correct except for movable draw transforms (patched by patchRtMovers).
bool VoxiRenderer::rtAccelSnapshotUnchanged() const {
    const auto reject = [this](u32 bit, const char* which) {
        if (!(rtAccelGateWhyMask_ & (1u << bit))) {
            rtAccelGateWhyMask_ |= (1u << bit);
            AVER_INFO("[Voxi] RT accel-structure gate rejected on: {}", which);
        }
        return false;
    };
    if (!rtAccelSnapValid_) return reject(0, "no snapshot yet (expected once)");
    if (rtAccelMustForceRebuild())
        return reject(1, "compute-skinned mesh present without a refit path (voxi.rtRefitAccel off), "
                         "or a cached BLAS handle went stale");
    if (rtAccelDrawsKey() != rtAccelKey_) return reject(2, "draw list changed");
    return true;
}

// Records the build's input. Mirrors takeGiSnapshot(): non-const half of gate, called once build happened.
void VoxiRenderer::takeRtAccelSnapshot() {
    // Reuse draw-list half computed by gate this frame; drawsPrev_ unchanged since.
    rtAccelKey_ = rtAccelDrawsKey(/*reuseListKey=*/true);
    rtAccelSnapValid_ = true;
}

// Mover patch lane's gate-hit half: updates tlasInstScratch_ and rtInstanceData_ with current movable-draw transforms.
// Verifies before writing: movers must pair off identity-for-identity. Refusal leaves both unchanged.
// Pairing by identity: reshuffled but unchanged list still matches. TIE-BREAK BY NEAREST: movers stay in instances.
// Only changed instances written; unchanged movers returns MoverPatch::Unchanged.
VoxiRenderer::MoverPatch VoxiRenderer::patchRtMovers(const std::vector<Draw>& list, bool late, bool deferGpu) {
    const auto refuse = [this](const char* why) {
        if (!(rtAccelGateWhyMask_ & (1u << 3))) {
            rtAccelGateWhyMask_ |= (1u << 3);
            AVER_INFO("[Voxi] RT accel-structure mover patch refused: {} -- running the full build instead", why);
        }
        return MoverPatch::Refused;
    };
    if (!late && !rtAccelListKeyValid_) return refuse("no key pass this frame");
    if (rtMoversNow_.size() != rtMovers_.size()) return refuse("the movable draw count changed");
    const usize instances = tlasInstScratch_.size();
    if (rtInstanceData_.size() != instances) return refuse("the instance tables are out of step");

    std::sort(rtMoversNow_.begin(), rtMoversNow_.end());
    for (usize k = 0; k < rtMovers_.size(); ++k) {
        if (rtMoversNow_[k].id != rtMovers_[k].id)
            return refuse("a movable draw's mesh, material or flags changed");
        if (rtMovers_[k].inst != kRtNoInstance && rtMovers_[k].inst >= instances)
            return refuse("a recorded instance index is out of range");
    }

    // Pair by nearest translation within runs of equal identity. Fast path: run size 1. Large runs (>64) use draw order.
    constexpr usize kMaxPairRun = 64;
    for (usize a = 0; a < rtMovers_.size();) {
        usize b = a + 1;
        while (b < rtMovers_.size() && rtMovers_[b].id == rtMovers_[a].id) ++b;
        const usize m = b - a;
        if (m > 1 && m <= kMaxPairRun) {
            u32 draws[kMaxPairRun];
            bool drawTaken[kMaxPairRun] = {};
            u32 chosen[kMaxPairRun];
            bool instDone[kMaxPairRun] = {};
            for (usize j = 0; j < m; ++j) { draws[j] = rtMoversNow_[a + j].draw; chosen[j] = kRtNoInstance; }
            // Instances with no BLAS take whatever is left.
            for (usize j = 0; j < m; ++j) instDone[j] = rtMovers_[a + j].inst == kRtNoInstance;
            for (;;) {
                f32 bestD2 = 0.0f;
                usize bi = m, bd = m;
                for (usize j = 0; j < m; ++j) {
                    if (instDone[j]) continue;
                    const f32* iw = tlasInstScratch_[rtMovers_[a + j].inst].world;
                    for (usize q = 0; q < m; ++q) {
                        if (drawTaken[q]) continue;
                        const f32* dw = list[draws[q]].world;
                        const f32 dx = dw[12] - iw[12], dy = dw[13] - iw[13], dz = dw[14] - iw[14];
                        const f32 d2 = dx * dx + dy * dy + dz * dz;
                        if (bi == m || d2 < bestD2) { bestD2 = d2; bi = j; bd = q; }
                    }
                }
                if (bi == m) break;
                chosen[bi] = static_cast<u32>(bd);
                instDone[bi] = true;
                drawTaken[bd] = true;
            }
            usize freeDraw = 0;
            for (usize j = 0; j < m; ++j) {
                if (chosen[j] != kRtNoInstance) continue;
                while (drawTaken[freeDraw]) ++freeDraw;
                drawTaken[freeDraw] = true;
                chosen[j] = static_cast<u32>(freeDraw);
            }
            for (usize j = 0; j < m; ++j) rtMoversNow_[a + j].draw = draws[chosen[j]];
        }
        a = b;
    }

    bool changed = false;
    bool matChanged = false;
    for (usize k = 0; k < rtMovers_.size(); ++k) {
        const u32 inst = rtMovers_[k].inst;
        if (inst == kRtNoInstance) continue;
        const Draw& md = list[rtMoversNow_[k].draw];
        // Emissive is not in a mover's identity: re-key its material row from the paired draw.
        const u64 matKey = rtMaterialKey(md.matSet, md.mat, md.color, md.metallic, md.roughness);
        if (inst < rtInstanceMatKey_.size() && rtInstanceMatKey_[inst] != matKey) {
            rtInstanceMatKey_[inst] = matKey;
            matChanged = true;
        }
        const f32* world = md.world;
        rhi::TlasInstance& ti = tlasInstScratch_[inst];
        if (std::memcmp(ti.world, world, sizeof(ti.world)) == 0) continue;
        changed = true;
        RtInstance& ri = rtInstanceData_[inst];
        // World becomes prev first, then new world into both.
        std::memcpy(ri.prevObjectToWorld, ri.objectToWorld, sizeof(ri.prevObjectToWorld));
        std::memcpy(ti.world, world, sizeof(ti.world));
        std::memcpy(ri.objectToWorld, world, sizeof(ri.objectToWorld));
    }
    if (!changed && !matChanged) return MoverPatch::Unchanged;
    if (matChanged) {
        // Drop rows nothing uses (an animated emissive makes a new key every frame), then re-row: every
        // instance and foliage part gets its index rewritten before the uploads. The TLAS is untouched.
        std::vector<u64> live(rtInstanceMatKey_);
        live.insert(live.end(), foliagePartMatKey_.begin(), foliagePartMatKey_.end());
        std::sort(live.begin(), live.end());
        for (auto it = matConstantsScratch_.begin(); it != matConstantsScratch_.end();) {
            if (std::binary_search(live.begin(), live.end(), it->first)) ++it;
            else it = matConstantsScratch_.erase(it);
        }
        if (!buildMaterialTable(matConstantsScratch_)) return refuse("the material table could not be rebuilt");
        if (rtGeometryReady_) uploadFoliagePartTable();
    }
    if (changed && deferGpu) return MoverPatch::Patched;
    if (!uploadRtInstanceTable()) return refuse("the instance table could not be uploaded");
    return changed ? MoverPatch::Patched : MoverPatch::MaterialsOnly;
}

// Ray-driven frames record at endFrame (D3D12), so latePatchMovers sees this frame's draw list.
bool VoxiRenderer::wantsLateScenePass() const {
    return rayDrivenActive() && !debugViewActive() && rtMoverPatchActive();
}

// The TLAS and instance table are built in prePass from LAST frame's draws (this frame's are submitted
// after it). A moving object would then be traced one frame behind the camera, by a distance that
// changes with every frame time: jitter against everything that is current. Called from the late
// scene pass, this re-pairs the movers against this frame's draws_ and refits before any ray is traced.
void VoxiRenderer::latePatchMovers(rhi::IRenderContext& ctx) {
    if (!rtActive_) return;
    MoverPatch patch = MoverPatch::Unchanged;
    if (rtMoverPatchActive() && !rtMovers_.empty() && !draws_.empty()) {
        DrawMaterialMemo memo{};
        rtMoversNow_.clear();
        u32 drawIndex = 0;
        for (const Draw& d : draws_) {
            if (d.movable) rtMoversNow_.push_back({rtDrawHash(d, memo, true), drawIndex, kRtNoInstance});
            ++drawIndex;
        }
        patch = patchRtMovers(draws_, true, true);
    }
    // Refit when this pass patched, or when prePass patched and left the upload and refit to here.
    if (patch != MoverPatch::Patched && !rtRefitDeferred_) return;
    rtRefitDeferred_ = false;
    if (patch == MoverPatch::Patched) {
        rtPrevPending_.clear();
        for (const RtMover& m : rtMovers_) {
            if (m.inst == kRtNoInstance || m.inst >= rtInstanceData_.size()) continue;
            const RtInstance& r = rtInstanceData_[m.inst];
            if (std::memcmp(r.prevObjectToWorld, r.objectToWorld, sizeof(r.objectToWorld)) != 0)
                rtPrevPending_.push_back(m.inst);
        }
    }
    if (patch != MoverPatch::MaterialsOnly) uploadRtInstanceTable();
    if (!rtDynamicMeshes_.empty()) {
        refitDynamicAccelStructures(ctx);
    } else {
        rhi::ScopedGpuStat gpuStat(ctx, "Voxi acceleration structures (movers)");
        refitOrRebuildTlas(ctx);
    }
}

// Widening-interval "N rebuilt / M refit-only / M skipped / M mover-patched" report.
// rtAccelRefitOnly_ and rtAccelMoverPatched_ count as "avoided full rebuild" alongside rtAccelSkipped_.
void VoxiRenderer::reportRtAccelGate() {
    const u64 avoidedNow = rtAccelSkipped_ + rtAccelRefitOnly_ + rtAccelMoverPatched_;
    const u64 ticks = avoidedNow + rtAccelRebuilt_;
    if (ticks < rtAccelGateNextReport_) return;
    const u64 winTicks   = ticks - rtAccelGateLastTicks_;
    const u64 winAvoided = avoidedNow - (rtAccelGateLastSkipped_ + rtAccelGateLastRefitOnly_ +
                                         rtAccelGateLastMoverPatched_);
    AVER_INFO("[Voxi] RT accel-structure gate: {} full rebuild / {} refit-only / {} skipped / {} mover-patched "
              "of {} tick(s) -- {}% avoided a full rebuild overall, {}% since the last report",
              rtAccelRebuilt_, rtAccelRefitOnly_, rtAccelSkipped_, rtAccelMoverPatched_, ticks,
              (avoidedNow * 100) / ticks,
              winTicks ? (winAvoided * 100) / winTicks : 0);
    rtAccelGateLastTicks_ = ticks;
    rtAccelGateLastSkipped_ = rtAccelSkipped_;
    rtAccelGateLastRefitOnly_ = rtAccelRefitOnly_;
    rtAccelGateLastMoverPatched_ = rtAccelMoverPatched_;
    rtAccelGateNextReport_ = ticks * 2;   // 64, 128, 256, ... -- a handful of lines, not a flood
}

// Refits or rebuilds dynamic BLAS: refit when eligible under budget, otherwise full rebuild.
// See RHIResources.hpp's refitBlas contract.
void VoxiRenderer::refitOrRebuildDynamicBlas(rhi::IRenderContext& ctx, rhi::BlasHandle blas,
                                             rhi::MeshHandle mesh) {
    u32& refits = dynamicBlasRefits_[mesh];
    const bool tryRefit = settings_.rtRefitAccel && refits < kDynamicBlasRefitsPerRebuild;
    const bool refitted = tryRefit && ctx.refitBlas(blas);
    if (refitted) { ++refits; ++rtDynamicBlasRefits_; }
    else {
        // Full build only if tryRefit is false; refitted false with tryRefit true means
        // ctx.refitBlas already did its own fallback.
        if (!tryRefit) ctx.buildBlas(blas);
        refits = 0;
        ++rtDynamicBlasRebuilds_;
    }
}

// Same shape for tlas_: see RHIResources.hpp's refitTlas contract.
// Shared by full-build and refit-only paths, so both use one rebuild schedule.
bool VoxiRenderer::refitOrRebuildTlas(rhi::IRenderContext& ctx) {
    const u32 count = static_cast<u32>(tlasInstScratch_.size());
    const bool tryRefit = settings_.rtRefitAccel && tlasRefitStreak_ < kTlasRefitsPerRebuild;
    const bool refitted = tryRefit && ctx.refitTlas(tlas_, tlasInstScratch_.data(), count);
    if (refitted) { ++tlasRefitStreak_; ++rtTlasRefits_; }
    else {
        if (!tryRefit) ctx.buildTlas(tlas_, tlasInstScratch_.data(), count);
        tlasRefitStreak_ = 0;
        ++rtTlasRebuilds_;
    }
    // rtTlasTranslucent_ counts what tlas_ now holds (valid only after build/refit).
    rtTlasTranslucent_ = tlasTranslucentThisBuild_;
    return refitted;
}

// Refit-only pass (Settings::rtRefitAccel): keeps every dynamic BLAS and tlas_ current
// with no per-draw loop; transforms are updated in tlasInstScratch_ just before this call.
void VoxiRenderer::refitDynamicAccelStructures(rhi::IRenderContext& ctx) {
    rhi::ScopedGpuStat gpuStat(ctx, "Voxi acceleration structures");
    for (rhi::MeshHandle mesh : rtDynamicMeshes_) {
        const auto it = blas_.find(mesh);
        if (it == blas_.end() || !it->second) continue;   // gone stale; next full build drops it
        refitOrRebuildDynamicBlas(ctx, it->second, mesh);
    }
    refitOrRebuildTlas(ctx);
    refreshDynamicVertexSlices(ctx);
}

// Groups an instance by (mesh, drawBinding) for previous-transform carry-forward.
// See VoxiRenderer.hpp declaration comment. Same FNV-1a hash as giDrawsKey().
u64 VoxiRenderer::rtInstanceGroupKey(rhi::MeshHandle mesh, rhi::BindingSetHandle matSet) const {
    u64 key = 1469598103934665603ull;
    key ^= static_cast<u64>(mesh);   key *= 1099511628211ull;
    key ^= static_cast<u64>(matSet); key *= 1099511628211ull;
    return key;
}

// Bucket for a 64-bit key in a power-of-two table: murmur finaliser for uniform distribution.
static inline usize rtCarryBucket(u64 k, usize mask) {
    k ^= k >> 33; k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33; k *= 0xc4ceb9fe1a85ec53ull;
    k ^= k >> 33;
    return static_cast<usize>(k) & mask;
}

// Hash of an instance's identity for exact-match index: group + 16 floats' raw bits.
// -0.0 and NaN payloads are treated as different transforms.
static inline u64 rtCarryExactHash(u64 groupKey, const f32* world) {
    u64 h = groupKey;
    hashBytesInto(h, world, sizeof(f32) * 16);
    return h;
}

// Builds carry-forward lookup from last frame's rtInstanceData_ / rtInstanceGroupKey_.
// Left empty when there is nothing trustworthy to carry (first build, reset, or size mismatch).
void VoxiRenderer::buildRtCarryLookup() {
    rtCarryEntries_.clear();
    rtCarryGroups_.clear();
    const usize n = rtInstanceData_.size();
    if (n == 0 || rtInstanceGroupKey_.size() != n) {
        rtCarryGroupBuckets_.clear();
        rtCarryExactBuckets_.clear();
        return;
    }
    usize cap = 16;
    while (cap < n * 2) cap <<= 1;   // load factor <= 0.5
    const usize mask = cap - 1;
    rtCarryGroupBuckets_.assign(cap, kRtCarryNone);
    rtCarryExactBuckets_.assign(cap, kRtCarryNone);
    rtCarryEntries_.reserve(n);
    for (usize i = 0; i < n; ++i) {
        const u64 key = rtInstanceGroupKey_[i];
        // Find or open this row's group.
        const usize gb = rtCarryBucket(key, mask);
        u32 g = rtCarryGroupBuckets_[gb];
        while (g != kRtCarryNone && rtCarryGroups_[g].key != key) g = rtCarryGroups_[g].nextInBucket;
        if (g == kRtCarryNone) {
            g = static_cast<u32>(rtCarryGroups_.size());
            RtCarryGroup ng;
            ng.key = key;
            ng.nextInBucket = rtCarryGroupBuckets_[gb];
            rtCarryGroups_.push_back(ng);
            rtCarryGroupBuckets_[gb] = g;
        }
        RtCarryEntry e;
        std::memcpy(e.world, rtInstanceData_[i].objectToWorld, sizeof(e.world));
        e.group = g;
        e.nextInGroup = rtCarryGroups_[g].head;
        const usize eb = rtCarryBucket(rtCarryExactHash(key, e.world), mask);
        e.nextExact = rtCarryExactBuckets_[eb];
        const u32 idx = static_cast<u32>(rtCarryEntries_.size());
        rtCarryEntries_.push_back(e);
        rtCarryGroups_[g].head = idx;
        ++rtCarryGroups_[g].unused;
        rtCarryExactBuckets_[eb] = idx;
    }
}

// Carries previous transform via: (a) exact match (same world), (b) nearest by translation
// within kRtCarryMaxScan and kRtMaxCarryCm, or (c) current world (new/no match).
// Each row matched at most once.
void VoxiRenderer::carryPrevTransform(u64 groupKey, const f32* world, f32* outPrev) {
    std::memcpy(outPrev, world, sizeof(f32) * 16);   // (c)
    if (rtCarryEntries_.empty()) return;
    const usize mask = rtCarryGroupBuckets_.size() - 1;
    u32 g = rtCarryGroupBuckets_[rtCarryBucket(groupKey, mask)];
    while (g != kRtCarryNone && rtCarryGroups_[g].key != groupKey) g = rtCarryGroups_[g].nextInBucket;
    if (g == kRtCarryNone || rtCarryGroups_[g].unused == 0) return;
    RtCarryGroup& grp = rtCarryGroups_[g];

    // (a) exact
    const usize eb = rtCarryBucket(rtCarryExactHash(groupKey, world), mask);
    u32* link = &rtCarryExactBuckets_[eb];
    while (*link != kRtCarryNone) {
        RtCarryEntry& e = rtCarryEntries_[*link];
        if (e.group == g && std::memcmp(e.world, world, sizeof(e.world)) == 0) {
            e.used = true;
            --grp.unused;
            *link = e.nextExact;   // unlink
            return;
        }
        link = &e.nextExact;
    }

    // (b) nearest translation among unmatched rows
    if (grp.unused > kRtCarryMaxScan) return;
    u32 best = kRtCarryNone;
    f32 bestD2 = kRtMaxCarryCm * kRtMaxCarryCm;
    for (u32 i = grp.head; i != kRtCarryNone; i = rtCarryEntries_[i].nextInGroup) {
        const RtCarryEntry& e = rtCarryEntries_[i];
        if (e.used) continue;
        const f32 dx = e.world[12] - world[12], dy = e.world[13] - world[13], dz = e.world[14] - world[14];
        const f32 d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= bestD2) { bestD2 = d2; best = i; }
    }
    if (best == kRtCarryNone) return;
    RtCarryEntry& e = rtCarryEntries_[best];
    e.used = true;
    --grp.unused;
    // Unlink from exact chain so later exact probe cannot hand it out again.
    u32* el = &rtCarryExactBuckets_[rtCarryBucket(rtCarryExactHash(groupKey, e.world), mask)];
    while (*el != kRtCarryNone && *el != best) el = &rtCarryEntries_[*el].nextExact;
    if (*el == best) *el = e.nextExact;
    std::memcpy(outPrev, e.world, sizeof(f32) * 16);
}

// Determines if voxelizePass would rasterise this draw: order-independent hash key input.
// Hashed as raw float bits, not compared with tolerance.
bool VoxiRenderer::giVoxelisedDraw(const Draw& d) const {
    if (d.translucent) return false;
    if (d.movable) return false;   // Moving in Play: rebuilds volume every frame it moves.
    if (dev_ && dev_->meshVertexBuffer(d.mesh)) return false;
    if (d.boundsRadius < 0.0f) return true;   // no bounds stated: assume it counts
    const Vec3 volCentre{center_[0], center_[1], center_[2]};
    const f32 volRadius = (extent_ > 1.0f ? extent_ : 1.0f) * 1.7320508f;
    return dist(Vec3{d.boundsCentre[0], d.boundsCentre[1], d.boundsCentre[2]}, volCentre)
           <= volRadius + d.boundsRadius;
}

// Hash key of injected draws. Order-independent via commutative sum.
// Each draw's hash finalised (xor-shift-multiply) before adding to decorrelate addends.
// Count folded in at end to distinguish N from N+1.
u64 VoxiRenderer::giDrawsKey() const {
    u64 key = 0;
    u64 counted = 0;
    DrawMaterialMemo matMemo{};
    for (usize di = 0; di < drawsPrev_.size(); ++di) {
        const Draw& d = drawsPrev_[di];
        // Skip translucent, movable, skinned, out-of-volume (see giVoxelisedDraw).
        if (!giVoxelisedDraw(d)) continue;
        DrawLanes* lanes = drawLanes(di);
        u64 h = 1469598103934665603ull;
        // depthMesh, not mesh: voxelizePass draws depthMesh.
        h ^= static_cast<u64>(d.depthMesh);
        h *= 1099511628211ull;
        hashWorldInto(h, d, lanes);
        // Material lands in baked radiance (PSVoxel reads it).
        hashDrawMaterialInto(h, d, matMemo, false, lanes);
        // Finalise before adding (splitmix64's avalanche).
        h ^= h >> 33; h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull;
        h ^= h >> 33;
        key += h;
        ++counted;
    }
    // Count mixed in to distinguish list length changes.
    key ^= counted * 1099511628211ull;
    return key;
}

// Splits giDrawsKey inputs into independent axes for "which axis changed" reporting.
// Same skips and finalise as giDrawsKey, order-independent for same reason.
void VoxiRenderer::giDrawsSubKeys(u64& count, u64& mesh, u64& world, u64& mat) const {
    count = mesh = world = mat = 0;
    auto mix = [](u64 h) {
        h ^= h >> 33; h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull;
        h ^= h >> 33; return h;
    };
    DrawMaterialMemo matMemo{};
    for (usize di = 0; di < drawsPrev_.size(); ++di) {
        const Draw& d = drawsPrev_[di];
        if (!giVoxelisedDraw(d)) continue;
        DrawLanes* lanes = drawLanes(di);
        ++count;
        mesh += mix(1469598103934665603ull ^ static_cast<u64>(d.depthMesh));
        u64 w = 1469598103934665603ull;
        hashWorldInto(w, d, lanes);
        // Mesh folded into world axis too: without it, swapped transforms read unchanged.
        w ^= static_cast<u64>(d.depthMesh); w *= 1099511628211ull;
        world += mix(w);
        u64 m = 1469598103934665603ull;
        hashDrawMaterialInto(m, d, matMemo, false, lanes);
        m ^= static_cast<u64>(d.depthMesh); m *= 1099511628211ull;
        mat += mix(m);
    }
}

// True when every input to voxelizePass is identical to the last rebuild.
bool VoxiRenderer::giSnapshotUnchanged() const {
    // Which check rejected, said once per reason.
    giRebuildCloudOnly_ = false;
    const auto reject = [this](u32 bit, const char* which) {
        if (!(giGateWhyMask_ & (1u << bit))) {
            giGateWhyMask_ |= (1u << bit);
            AVER_INFO("[Voxi] GI rebuild gate rejected on: {}", which);
        }
        return false;
    };
    if (!giSnapValid_) return reject(0, "no snapshot yet (expected once)");
    if (giSnapExtent_ != extent_) return reject(1, "volume extent changed");
    for (u32 i = 0; i < 3; ++i) if (giSnapCenter_[i] != center_[i]) return reject(2, "volume centre changed");
    if (giSnapVoxelSky_ != voxelSkyInjected()) return reject(6, "ReSTIR GI toggled (volume sky in/out)");
    // Sky struct byte-for-byte: sunDirection, sunColor, sunIntensity, ground albedo, sky-light intensity.
    // Comparing bytes keeps this correct if SkyAtmosphere gains fields.
    if (!dev_) return false;
    // Zero-initialised then assigned for correct padding comparison.
    rhi::SkyAtmosphere now{};
    now = dev_->skyAtmosphere();
    // cloudTime is a clock and rebuilds volume every giUpdateInterval frames on static scene.
    rhi::SkyAtmosphere was{};
    was = giSky_;
    const f32 cloudTimeDelta = std::fabs(now.cloudTime - was.cloudTime);
    now.cloudTime = was.cloudTime = 0.0f;

    if (std::memcmp(&now, &was, sizeof(now)) != 0) {
        // Which byte changed: map against rhi::SkyAtmosphere field order.
        const auto* a = reinterpret_cast<const u8*>(&now);
        const auto* b = reinterpret_cast<const u8*>(&was);
        usize off = 0;
        while (off < sizeof(now) && a[off] == b[off]) ++off;
        f32 fa = 0, fb = 0;
        if (off + sizeof(f32) <= sizeof(now)) {
            std::memcpy(&fa, a + off, sizeof(f32));
            std::memcpy(&fb, b + off, sizeof(f32));
        }
        if (!(giGateWhyMask_ & (1u << 3)))
            AVER_INFO("[Voxi] GI rebuild gate: sky/sun differs first at byte {} of {} "
                      "(as f32: now {} vs snapshot {})", off, sizeof(now), fa, fb);
        return reject(3, "sky/sun changed");
    }

    if (giDrawsKey() != giDrawsKey_) {
        // Which axis changed (see giDrawsSubKeys).
        u64 c = 0, m = 0, w = 0, mt = 0;
        giDrawsSubKeys(c, m, w, mt);
        ++giDrawsRejects_;
        if (c  != giDrawsCount_)    ++giDrawsCountMoved_;
        if (m  != giDrawsMeshKey_)  ++giDrawsMeshMoved_;
        if (w  != giDrawsWorldKey_) ++giDrawsWorldMoved_;
        if (mt != giDrawsMatKey_)   ++giDrawsMatMoved_;
        if (c != giDrawsCount_ && giDrawsDiffReports_ < 3) {
            ++giDrawsDiffReports_;
            std::vector<rhi::MeshHandle> now2;
            for (const Draw& d : drawsPrev_) {
                if (d.translucent || d.movable) continue;
                if (dev_ && dev_->meshVertexBuffer(d.mesh)) continue;
                now2.push_back(d.depthMesh);
            }
            std::sort(now2.begin(), now2.end());
            std::vector<rhi::MeshHandle> added, removed;
            std::set_difference(now2.begin(), now2.end(), giSnapMeshes_.begin(), giSnapMeshes_.end(),
                                std::back_inserter(added));
            std::set_difference(giSnapMeshes_.begin(), giSnapMeshes_.end(), now2.begin(), now2.end(),
                                std::back_inserter(removed));
            std::string a, r;
            for (rhi::MeshHandle h : added)   { a += std::to_string(h); a += ' '; }
            for (rhi::MeshHandle h : removed) { r += std::to_string(h); r += ' '; }
            AVER_INFO("[Voxi] GI draw-list delta: +{} mesh(es) [{}] -{} mesh(es) [{}]",
                      added.size(), a.empty() ? "-" : a.c_str(), removed.size(), r.empty() ? "-" : r.c_str());

            // Why a handle left: still in drawsPrev_ -> giVoxelisedDraw rejected it;
            // gone from drawsPrev_ -> never reached submitDraw (upstream issue).
            std::string why;
            for (rhi::MeshHandle h : removed) {
                const Draw* found = nullptr;
                for (const Draw& d : drawsPrev_) if (d.depthMesh == h) { found = &d; break; }
                why += std::to_string(h);
                if (!found) { why += "=not-submitted "; continue; }
                if (found->translucent) { why += "=translucent "; continue; }
                if (found->movable) { why += "=movable "; continue; }
                if (dev_ && dev_->meshVertexBuffer(found->mesh)) { why += "=skinned "; continue; }
                if (found->boundsRadius >= 0.0f) {
                    const Vec3 volCentre{center_[0], center_[1], center_[2]};
                    const f32 volRadius = (extent_ > 1.0f ? extent_ : 1.0f) * 1.7320508f;
                    const f32 dd = dist(Vec3{found->boundsCentre[0], found->boundsCentre[1],
                                             found->boundsCentre[2]}, volCentre);
                    char buf[96];
                    std::snprintf(buf, sizeof buf, "=outside(d %.0f > r %.0f+%.0f) ",
                                  static_cast<f64>(dd), static_cast<f64>(volRadius),
                                  static_cast<f64>(found->boundsRadius));
                    why += buf;
                    continue;
                }
                why += "=submitted-but-unclassified ";
            }
            if (!removed.empty())
                AVER_INFO("[Voxi] GI draw-list departures: {}", why);
        }
        if (giDrawsRejects_ >= giDrawsNextReport_) {
            AVER_INFO("[Voxi] GI draw-list axes over {} rejection(s): count moved {}x, mesh set {}x, "
                      "world transforms {}x, material values {}x  (last count {} vs {})",
                      giDrawsRejects_, giDrawsCountMoved_, giDrawsMeshMoved_, giDrawsWorldMoved_,
                      giDrawsMatMoved_, c, giDrawsCount_);
            giDrawsNextReport_ *= 2;
        }
        return reject(4, "draw list changed");
    }

    // Clouds tested last: cloud drift changes sky reaching ground (bounded staleness).
    // 30 seconds stale is acceptable; 2 seconds forced ~18 MB revoxelisation every 2 sec.
    constexpr f32 kGiCloudStaleSeconds = 30.0f;
    if (now.cloudsEnabled && cloudTimeDelta > kGiCloudStaleSeconds) {
        giRebuildCloudOnly_ = true;
        return reject(5, "clouds drifted");
    }
    return true;
}

// Records what the rebuild was computed from (for takeGiSnapshot).
// The GI derived-data cache: rebuild gate answers "changed since last bake this run";
// this answers same question across runs.
void VoxiRenderer::setGiCacheDir(const std::string& dir) {
    if (dir == giCacheDir_) return;
    giCacheDir_ = dir;
    // New directory = new project: clear what was tried against old one.
    giCacheTried_ = false;
    giCacheTriedKey_ = fmt::GiCacheKey{};
    giCacheKnownKeys_.clear();
}

// Key describing volume after takeGiSnapshot.
// Sky hashed same way gate compares it: byte-for-byte minus cloud clock.
fmt::GiCacheKey VoxiRenderer::giCacheKey() const {
    fmt::GiCacheKey k;
    k.drawsKey = giDrawsKey_;
    rhi::SkyAtmosphere sky = giSky_;
    sky.cloudTime = 0.0f;
    const u8* p = reinterpret_cast<const u8*>(&sky);
    u64 h = 1469598103934665603ull;
    for (usize i = 0; i < sizeof(sky); ++i) { h ^= p[i]; h *= 1099511628211ull; }
    // Folded into sky's key: with ReSTIR GI chosen, PSVoxel leaves sky out.
    // Cached volume from other rule would otherwise match. Only for sky-less bake.
    if (!giSnapVoxelSky_) { h ^= 0x5Cu; h *= 1099511628211ull; }
    // Bake rule version: change to volume contents must miss older entries.
    // Version 2: per-channel bounce gain capped.
    constexpr u64 kGiBakeRuleVersion = 2;
    h ^= kGiBakeRuleVersion; h *= 1099511628211ull;
    k.skyKey = h;
    for (u32 i = 0; i < 3; ++i) k.centre[i] = giSnapCenter_[i];
    k.extent     = giSnapExtent_;
    k.resolution = voxelResBuilt_;
    k.mipCount   = voxelMips_;
    return k;
}

// Sizes staging buffers and works out mip layout.
// Backend layout, not file's: D3D12 pads copy rows to 256 bytes, Vulkan packs tight.
bool VoxiRenderer::giCacheEnsureBuffers() {
    if (giCacheUnsupported_ || !res_ || !voxelTex_ || voxelMips_ == 0) return false;

    rhi::TextureCopyFootprint fp{};
    if (!res_->textureCopyFootprint(voxelTex_, 0, fp)) {
        AVER_INFO("[Voxi] GI cache disabled: this backend cannot copy a texture to a buffer");
        giCacheUnsupported_ = true;
        return false;
    }

    u64 total = 0;
    giCacheMipOffsets_.assign(voxelMips_, 0);
    for (u32 m = 0; m < voxelMips_; ++m) {
        rhi::TextureCopyFootprint f{};
        if (!res_->textureCopyFootprint(voxelTex_, m, f)) { giCacheUnsupported_ = true; return false; }
        // 512-aligned per mip: D3D12 wants 512-byte boundary, Vulkan wants 4. Take stricter.
        total = (total + 511ull) & ~511ull;
        giCacheMipOffsets_[m] = total;
        total += f.totalBytes;
    }
    if (total == 0) return false;
    if (giCacheReadback_ && giCacheBufBytes_ >= total) return true;

    if (giCacheReadback_) { res_->destroyBuffer(giCacheReadback_); giCacheReadback_ = 0; }
    if (giCacheUpload_)   { res_->destroyBuffer(giCacheUpload_);   giCacheUpload_ = 0; }

    rhi::BufferDesc rb;
    rb.bytes = total;
    rb.kind  = rhi::BufferKind::Readback;
    rb.debugName = "gi cache readback";
    giCacheReadback_ = res_->createBuffer(rb);

    rhi::BufferDesc ub;
    ub.bytes = total;
    ub.kind  = rhi::BufferKind::Upload;
    ub.debugName = "gi cache upload";
    giCacheUpload_ = res_->createBuffer(ub);

    if (!giCacheReadback_ || !giCacheUpload_) {
        AVER_WARN("[Voxi] GI cache disabled: could not allocate {} KB of staging", total / 1024);
        // Prevent partial-success leak: giCacheUnsupported_ stops this function, so free the one allocated.
        if (giCacheReadback_) { res_->destroyBuffer(giCacheReadback_); giCacheReadback_ = 0; }
        if (giCacheUpload_)   { res_->destroyBuffer(giCacheUpload_);   giCacheUpload_ = 0; }
        giCacheUnsupported_ = true;
        return false;
    }
    giCacheBufBytes_ = total;
    return true;
}

// Releases staging buffers after use (restore's upload or dump's readback done).
// Safe to call immediately after recording copy; giCacheEnsureBuffers re-allocates on demand.
void VoxiRenderer::giCacheFreeBuffers() {
    if (!res_ || (!giCacheReadback_ && !giCacheUpload_)) return;
    if (giCacheReadback_) { res_->destroyBuffer(giCacheReadback_); giCacheReadback_ = 0; }
    if (giCacheUpload_)   { res_->destroyBuffer(giCacheUpload_);   giCacheUpload_ = 0; }
    giCacheBufBytes_ = 0;
}

// Tries to fill voxelTex_ from disk. Returns true if volume now holds cached answer
// and caller should skip voxelizePass/filterMips.
bool VoxiRenderer::giCacheRestore(rhi::IRenderContext& ctx) {
    if (giCacheDir_.empty() || giCacheUnsupported_) return false;
    const fmt::GiCacheKey key = giCacheKey();
    if (key.resolution == 0 || key.mipCount == 0) return false;

    // Once per key: miss must not re-open absent file every rebuild (rebuilds are expensive frames).
    if (giCacheTried_ && giCacheTriedKey_ == key) return false;
    giCacheTried_ = true;
    giCacheTriedKey_ = key;

    // Buffer is part of cache: in-RAM bake not yet on disk must HIT (write-behind).
    fmt::GiCacheEntry entry;
    bool fromBuffer = false;
    for (const fmt::GiCacheEntry& held : giCachePendingEntries_) {
        if (held.key != key) continue;
        entry = held;
        fromBuffer = true;
        break;
    }

    const std::string path = giCacheDir_ + "\\" + fmt::giCacheFileName(key);
    std::string why;
    if (!fromBuffer && !fmt::loadGiCache(path, entry, &why)) return false;
    // File name is hash: collision possible. Compare key entry actually carries.
    if (entry.key != key) return false;
    if (!giCacheEnsureBuffers()) return false;

    // Expand file's tightly-packed mips into backend's footprint layout.
    for (u32 m = 0; m < voxelMips_; ++m) {
        rhi::TextureCopyFootprint f{};
        if (!res_->textureCopyFootprint(voxelTex_, m, f)) return false;
        const u64 srcBase = fmt::giCacheMipOffset(entry.key, m);
        const u32 tightRow = f.rowBytes;
        if (f.rowPitch == tightRow) {
            res_->writeBuffer(giCacheUpload_, entry.voxels.data() + srcBase,
                              static_cast<u64>(tightRow) * f.rows * f.depth, giCacheMipOffsets_[m]);
        } else {
            // Row-by-row: padding is real, straight copy would slide rows into padding.
            for (u32 z = 0; z < f.depth; ++z)
                for (u32 y = 0; y < f.rows; ++y) {
                    const u64 src = srcBase + (static_cast<u64>(z) * f.rows + y) * tightRow;
                    const u64 dst = giCacheMipOffsets_[m] + (static_cast<u64>(z) * f.rows + y) * f.rowPitch;
                    res_->writeBuffer(giCacheUpload_, entry.voxels.data() + src, tightRow, dst);
                }
        }
    }

    ++voxelGen_;
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::CopyDest);
    for (u32 m = 0; m < voxelMips_; ++m)
        ctx.copyBufferToTexture(voxelTex_, m, giCacheUpload_, giCacheMipOffsets_[m]);
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::CopyDest, rhi::ResourceState::ShaderResource);
    // Copy recorded (not yet run); freeing here is fence-safe. Both buffers freed (upload unused, hold ~1170 MiB idle).
    giCacheFreeBuffers();

    AVER_INFO("[Voxi] GI cache HIT: restored a {}^3 volume from {}", key.resolution,
              fmt::giCacheFileName(key));
    // Already cached: don't copy/rewrite just-read file.
    giCacheRememberKey(key);
    // Restore writes whole volume; invalidate previous box for clean rebuild.
    giBoxPrevValid_ = false;
    return true;
}

// Ceiling on one entry (absolute, not RAM-budget fraction).
// Over this, entry dropped and rebuilt: costs ~one rebuild vs seconds of disk write.
static constexpr u64 kMaxCachedGiEntryBytes = 256ull * 1024ull * 1024ull;

// Warns once about oversized entry (not cached, rebuilt on every open).
void VoxiRenderer::giCacheWarnOversize(u64 bytes) {
    if (giCacheOversizeWarned_) return;
    giCacheOversizeWarned_ = true;
    AVER_WARN("[Voxi] a {}^3 GI volume is {} MB, over the {} MB per-entry ceiling, so it is "
              "NOT cached and every open re-voxelises instead -- which costs about one frame. "
              "Writing it would block for seconds per bake and keep up to 8 copies on disk. "
              "Lower RENDER.VOXELRES if you want the cache back; raising the cache budget "
              "does not help, it only makes the flush bigger.",
              voxelResBuilt_, bytes / (1024 * 1024), kMaxCachedGiEntryBytes / (1024 * 1024));
}

bool VoxiRenderer::giCacheKeyKnown(const fmt::GiCacheKey& k) const {
    for (const fmt::GiCacheKey& known : giCacheKnownKeys_)
        if (known == k) return true;
    return false;
}

void VoxiRenderer::giCacheRememberKey(const fmt::GiCacheKey& k) {
    if (giCacheKeyKnown(k)) return;
    constexpr usize kKnownKeys = 64;   // Matches giCacheFlush's kGiCacheKeepFiles.
    if (giCacheKnownKeys_.size() >= kKnownKeys) giCacheKnownKeys_.erase(giCacheKnownKeys_.begin());
    giCacheKnownKeys_.push_back(k);
}

// Schedules readback of settled volume (called from gate's skip branch).
// Nothing written yet (see countdown).
bool VoxiRenderer::giCacheScheduleDump(rhi::IRenderContext& ctx) {
    if (giCacheDir_.empty() || giCacheUnsupported_) return true;
    if (giCacheDumpCountdown_) return false;   // one in flight is enough

    // Cloud-only rebuild not worth file: key zeroes cloudTime, so bake with only cloud change
    // carries identical key while holding different voxels (non-deterministic).
    if (giCacheSettleCloudOnly_) return true;

    giCachePendingKey_ = giCacheKey();
    if (giCachePendingKey_.resolution == 0) return true;
    // Restored or already queued: nothing new to write.
    if (giCacheKeyKnown(giCachePendingKey_)) return true;

    // Ceiling tested here, before copy (not after, avoiding expensive non-cached path).
    if (fmt::giCacheTotalBytes(giCachePendingKey_) > kMaxCachedGiEntryBytes) {
        giCacheWarnOversize(fmt::giCacheTotalBytes(giCachePendingKey_));
        return true;
    }

    if (!giCacheEnsureBuffers()) return true;

    // Barrier: filterMips leaves in ShaderResource; restore it after copy.
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::CopySource);
    for (u32 m = 0; m < voxelMips_; ++m)
        ctx.copyTextureToBuffer(giCacheReadback_, giCacheMipOffsets_[m], voxelTex_, m);
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::CopySource, rhi::ResourceState::ShaderResource);

    giCacheDumpCountdown_ = kGiCacheReadbackDelay;
    giCacheRememberKey(giCachePendingKey_);
    return true;
}

// Ticks countdown and writes file when GPU past copy.
void VoxiRenderer::giCacheTick() {
    if (!giCacheDumpCountdown_) return;
    if (--giCacheDumpCountdown_) return;
    if (!res_ || !giCacheReadback_) return;

    // Compact backend layout back to tightly-packed file layout.
    fmt::GiCacheEntry entry;
    entry.key = giCachePendingKey_;
    entry.voxels.resize(static_cast<usize>(fmt::giCacheTotalBytes(entry.key)));

    for (u32 m = 0; m < entry.key.mipCount && m < giCacheMipOffsets_.size(); ++m) {
        rhi::TextureCopyFootprint f{};
        if (!res_->textureCopyFootprint(voxelTex_, m, f)) return;
        const u64 dstBase = fmt::giCacheMipOffset(entry.key, m);
        const u32 tightRow = f.rowBytes;
        if (f.rowPitch == tightRow) {
            if (!res_->readBuffer(giCacheReadback_, entry.voxels.data() + dstBase,
                                  static_cast<u64>(tightRow) * f.rows * f.depth, giCacheMipOffsets_[m]))
                return;
        } else {
            for (u32 z = 0; z < f.depth; ++z)
                for (u32 y = 0; y < f.rows; ++y) {
                    const u64 dst = dstBase + (static_cast<u64>(z) * f.rows + y) * tightRow;
                    const u64 src = giCacheMipOffsets_[m] + (static_cast<u64>(z) * f.rows + y) * f.rowPitch;
                    if (!res_->readBuffer(giCacheReadback_, entry.voxels.data() + dst, tightRow, src))
                        return;
                }
        }
    }

    // Buffered, not written: see giCachePendingEntries_ (losing entry costs one rebuild).
    // Replace same-key entry rather than accumulate duplicates.
    const u64 bytes = static_cast<u64>(entry.voxels.size());

    // Belt and braces: giCacheScheduleDump refuses oversize readback; reaching here oversize
    // means key changed mid-flight. Without this, would write gigabyte file ceiling prevents.
    if (bytes > kMaxCachedGiEntryBytes) {
        giCacheWarnOversize(bytes);
        return;
    }

    bool replaced = false;
    for (fmt::GiCacheEntry& held : giCachePendingEntries_) {
        if (held.key != entry.key) continue;
        giCachePendingBytes_ -= static_cast<u64>(held.voxels.size());
        held = std::move(entry);
        giCachePendingBytes_ += bytes;
        replaced = true;
        break;
    }
    if (!replaced) {
        giCachePendingBytes_ += bytes;
        giCachePendingEntries_.push_back(std::move(entry));
    }

    if (giCachePendingBytes_ > giCacheRamBudget_) {
        AVER_INFO("[Voxi] GI cache buffer over budget ({} MB of {} MB) -- flushing",
                  giCachePendingBytes_ / (1024 * 1024), giCacheRamBudget_ / (1024 * 1024));
        giCacheFlush();
    }
    // Readback done: both staging buffers finished (see giCacheFreeBuffers).
    giCacheFreeBuffers();
}

// Lowers budget below held amount: flushes now (not on next bake).
void VoxiRenderer::setGiCacheRamBudget(u64 bytes) {
    giCacheRamBudget_ = bytes;
    if (giCachePendingBytes_ > giCacheRamBudget_) giCacheFlush();
}

// Writes all held entries, sweeps once, empties buffer.
// One sweep for whole batch (not per file): sweep stats entire directory, so per-write swept 10x.
u32 VoxiRenderer::giCacheFlush() {
    if (giCachePendingEntries_.empty()) return 0;
    if (giCacheDir_.empty()) {
        // No project: drop buffer rather than grow forever.
        giCachePendingEntries_.clear();
        giCachePendingBytes_ = 0;
        return 0;
    }

    u32 wrote = 0;
    u64 wroteBytes = 0;
    for (const fmt::GiCacheEntry& entry : giCachePendingEntries_) {
        const std::string path = giCacheDir_ + "\\" + fmt::giCacheFileName(entry.key);
        std::string why;
        if (!fmt::saveGiCache(path, entry, &why)) {
            AVER_WARN("[Voxi] GI cache could not be written: {}", why);
            continue;
        }
        ++wrote;
        wroteBytes += static_cast<u64>(entry.voxels.size());
    }
    giCachePendingEntries_.clear();
    giCachePendingBytes_ = 0;

    // Bounded: every bake writes new file, none overwritten. Raised from 8, now byte-bounded too.
    // Count alone can't distinguish 64 small from 64 huge entries.
    constexpr u32 kGiCacheKeepFiles = 64;
    constexpr u64 kGiCacheKeepBytes = 1536ull * 1024ull * 1024ull;   // 1.5 GB
    const u32 swept = wrote ? fmt::giCacheSweep(giCacheDir_, kGiCacheKeepFiles, kGiCacheKeepBytes) : 0;
    if (wrote)
        AVER_INFO("[Voxi] GI cache FLUSHED {} entr(ies), {} KB{}", wrote, wroteBytes / 1024,
                  swept ? (", swept " + std::to_string(swept) + " older entr(ies)") : std::string());
    return wrote;
}

void VoxiRenderer::takeGiSnapshot() {
    giDrawsSubKeys(giDrawsCount_, giDrawsMeshKey_, giDrawsWorldKey_, giDrawsMatKey_);
    // Mesh multiset itself: giSnapshotUnchanged says which draws came/went, not just count moved.
    giSnapMeshes_.clear();
    for (const Draw& d : drawsPrev_) {
        if (!giVoxelisedDraw(d)) continue;
        giSnapMeshes_.push_back(d.depthMesh);
    }
    std::sort(giSnapMeshes_.begin(), giSnapMeshes_.end());
    giDrawsKey_ = giDrawsKey();
    // Two-step on stored side: both sides have zero padding.
    if (dev_) { giSky_ = rhi::SkyAtmosphere{}; giSky_ = dev_->skyAtmosphere(); }
    for (u32 i = 0; i < 3; ++i) giSnapCenter_[i] = center_[i];
    giSnapExtent_ = extent_;
    giSnapVoxelSky_ = voxelSkyInjected();
    giSnapValid_ = true;
}

u32 VoxiRenderer::fitCascades() {
    f32 invViewProjRel[16] = {};
    f32 camPos[3] = {};
    // Forward matrix captured into curViewProj_ for endShadowHistory's reprojection source.
    if (!dev_ || !dev_->camera(curViewProj_, invViewProjRel, camPos)) return 0;
    std::memcpy(curInvViewProjRel_, invViewProjRel, sizeof(curInvViewProjRel_));
    std::memcpy(curCamPos_, camPos, sizeof(curCamPos_));

    Mat4 invVPRel;
    std::memcpy(&invVPRel.m[0][0], invViewProjRel, sizeof(invViewProjRel));
    const Vec3 eye{camPos[0], camPos[1], camPos[2]};

    // Frustum's eight corners as offsets from the eye (camera-relative).
    Vec3 nearR[4], farR[4];
    const f32 nx[4] = {-1, 1, 1, -1};
    const f32 ny[4] = {-1, -1, 1, 1};
    for (int i = 0; i < 4; ++i) {
        nearR[i] = xformProjected(Vec3{nx[i], ny[i], 0.0f}, invVPRel);
        farR[i]  = xformProjected(Vec3{nx[i], ny[i], 1.0f}, invVPRel);
    }

    // View depth at each plane, measured along the view axis.
    Vec3 fwd = (farR[0] + farR[1] + farR[2] + farR[3]) * 0.25f;
    fwd = fwd.getSafeNormal();
    const f32 camNear = dot(nearR[0], fwd);
    const f32 camFar  = dot(farR[0], fwd);
    if (!(camFar > camNear + 1e-3f)) return 0;

    const f32 zNear = camNear;
    const f32 reach = camNear * kShadowRangeFromNear;
    const f32 zFar  = camFar < reach ? camFar : reach;
    if (!(zFar > zNear)) return 0;

    Vec3 dir = Vec3{sunDir_[0], sunDir_[1], sunDir_[2]}.getSafeNormal();
    if (dir.sizeSquared() < 0.5f) {
        const rhi::SkyAtmosphere def{};
        dir = Vec3{def.sunDirection[0], def.sunDirection[1], def.sunDirection[2]}.getSafeNormal();
    }
    const Vec3 up = std::fabs(dir.z) > 0.95f ? Vec3{1, 0, 0} : Vec3{0, 0, 1};

    f32 sliceNear = zNear;
    for (u32 c = 0; c < kShadowCascades; ++c) {
        const f32 p = static_cast<f32>(c + 1) / static_cast<f32>(kShadowCascades);
        const f32 logSplit = zNear * std::pow(zFar / zNear, p);
        const f32 uniSplit = zNear + (zFar - zNear) * p;
        f32 sliceFar = kCascadeSplitLambda * logSplit + (1.0f - kCascadeSplitLambda) * uniSplit;

        // Corners of this slice, interpolated along the frustum's edges, relative to the eye.
        const f32 tN = (sliceNear - camNear) / (camFar - camNear);
        const f32 tF = (sliceFar  - camNear) / (camFar - camNear);
        Vec3 corner[8];
        for (int i = 0; i < 4; ++i) {
            corner[i]     = nearR[i] + (farR[i] - nearR[i]) * tN;
            corner[i + 4] = nearR[i] + (farR[i] - nearR[i]) * tF;
        }

        Vec3 centreRel{0, 0, 0};
        for (const Vec3& v : corner) centreRel = centreRel + v;
        centreRel = centreRel * 0.125f;
        f32 radius = 0.0f;
        for (const Vec3& v : corner) {
            const f32 d = (v - centreRel).size();
            if (d > radius) radius = d;
        }
        radius = std::ceil(radius * 16.0f) / 16.0f;

        // Absolute position for light-space matrix and per-draw cull.
        const Vec3 centre = eye + centreRel;

        // Fitted to camera alone, not union with GI volume.
        cascadeCentre_[c][0] = centre.x; cascadeCentre_[c][1] = centre.y; cascadeCentre_[c][2] = centre.z;
        cascadeRadius_[c] = radius;

        // Snap centre to a whole texel in light space.
        const f32 texel = 2.0f * radius / static_cast<f32>(kShadowCascadeSize);
        Mat4 view = Mat4::lookAtLH(centre + dir * (radius * 2.0f), centre, up);
        Vec3 cLs = xformPoint(centre, view);
        cLs.x = std::floor(cLs.x / texel) * texel;
        cLs.y = std::floor(cLs.y / texel) * texel;
        const Vec3 snapped = xformPoint(cLs, view.inverse());
        view = Mat4::lookAtLH(snapped + dir * (radius * 2.0f), snapped, up);

        Mat4 proj;   // orthographic, row-vector, depth [0,1]
        proj.m[0][0] = 1.0f / radius;
        proj.m[1][1] = 1.0f / radius;
        proj.m[2][2] = 1.0f / (radius * 4.0f);
        proj.m[3][2] = 0.0f;
        proj.m[3][3] = 1.0f;
        const Mat4 lvp = view * proj;
        std::memcpy(cb_.cascadeViewProj[c], &lvp.m[0][0], sizeof(lvp.m));

        // Radial distance for sphere-fitted cascade.
        cb_.cascadeSplit[c][0] = centreRel.size() + radius;
        cb_.cascadeSplit[c][1] = texel * 1.5f;   // normal-offset bias, world units
        cb_.cascadeSplit[c][2] = 0.0f;
        cb_.cascadeSplit[c][3] = 0.0f;

        sliceNear = sliceFar;
    }
    return kShadowCascades;
}

// Fits the GI-only shadow map to the GI volume's bounding box.
// Rebuilt on giUpdateInterval's cadence, independent of camera movement.
void VoxiRenderer::fitGiShadow() {
    // Volume's circumsphere.
    const Vec3 centre{center_[0], center_[1], center_[2]};
    const f32 radius = (extent_ > 1.0f ? extent_ : 1.0f) * 1.7320508f;

    giShadowCentre_[0] = centre.x; giShadowCentre_[1] = centre.y; giShadowCentre_[2] = centre.z;
    giShadowRadius_ = radius;

    Vec3 dir = Vec3{sunDir_[0], sunDir_[1], sunDir_[2]}.getSafeNormal();
    if (dir.sizeSquared() < 0.5f) {
        const rhi::SkyAtmosphere def{};
        dir = Vec3{def.sunDirection[0], def.sunDirection[1], def.sunDirection[2]}.getSafeNormal();
    }
    const Vec3 up = std::fabs(dir.z) > 0.95f ? Vec3{1, 0, 0} : Vec3{0, 0, 1};

    // Snap to whole texel to prevent crawling occlusion when sun moves.
    const f32 texel = 2.0f * radius / static_cast<f32>(kGiShadowSize);
    Mat4 view = Mat4::lookAtLH(centre + dir * (radius * 2.0f), centre, up);
    Vec3 cLs = xformPoint(centre, view);
    cLs.x = std::floor(cLs.x / texel) * texel;
    cLs.y = std::floor(cLs.y / texel) * texel;
    const Vec3 snapped = xformPoint(cLs, view.inverse());
    view = Mat4::lookAtLH(snapped + dir * (radius * 2.0f), snapped, up);

    Mat4 proj;   // orthographic, row-vector, depth [0,1]
    proj.m[0][0] = 1.0f / radius;
    proj.m[1][1] = 1.0f / radius;
    proj.m[2][2] = 1.0f / (radius * 4.0f);
    proj.m[3][2] = 0.0f;
    proj.m[3][3] = 1.0f;
    const Mat4 lvp = view * proj;
    std::memcpy(cb_.giShadowViewProj, &lvp.m[0][0], sizeof(lvp.m));

    cb_.giShadowParams[0] = 1.0f / static_cast<f32>(kGiShadowSize);
    cb_.giShadowParams[1] = 1.0f;          // usable; giShadowPass zeroes it when it cannot run
    cb_.giShadowParams[2] = texel * 1.5f;  // normal-offset bias, world units
    // [3] carries staged ray-driven bit-field toggles packed in prePass and must not be clobbered here.
}

// Builds the per-instance mesh table from all referenced meshes' vertices and indices.
// Rebuilt only when the mesh set changes.
bool VoxiRenderer::buildGeometryTable(rhi::IRenderContext& ctx) {
    if (!res_ || !dev_ || (rtInstanceData_.empty() && foliageParts_.empty())) return false;

    // One entry per distinct mesh, not per instance. Sorted for stable indexing across frame reorders.
    rtGeomMeshes_.assign(rtInstanceMesh_.begin(), rtInstanceMesh_.end());
    rtGeomMeshes_.insert(rtGeomMeshes_.end(), foliageMeshes_.begin(), foliageMeshes_.end());
    std::sort(rtGeomMeshes_.begin(), rtGeomMeshes_.end());
    rtGeomMeshes_.erase(std::unique(rtGeomMeshes_.begin(), rtGeomMeshes_.end()), rtGeomMeshes_.end());

    u64 key = 1469598103934665603ull;
    for (rhi::MeshHandle h : rtGeomMeshes_) {
        key ^= static_cast<u64>(h);
        key *= 1099511628211ull;
    }

    rtGeomFirstVertex_.clear();
    rtGeomFirstIndex_.clear();
    rtGeomCopiesVerts_.clear();
    rtGeomVertSlice_.clear();
    // Rebuilt alongside rtGeomFirstVertex_/rtGeomCopiesVerts_, swapped in only when this returns true.
    rtDynamicVertexSlices_.clear();
    rtDynamicVertexSlicesPending_.clear();
    rtGeomFirstVertex_.reserve(rtGeomMeshes_.size());
    rtGeomFirstIndex_.reserve(rtGeomMeshes_.size());
    rtGeomCopiesVerts_.reserve(rtGeomMeshes_.size());
    u32 totalVerts = 0, totalIndices = 0;
    for (rhi::MeshHandle h : rtGeomMeshes_) {
        rhi::BufferHandle vb = 0, ib = 0;
        u32 vc = 0, ic = 0;
        if (!dev_->meshGeometry(h, &vb, &ib, &vc, &ic)) return false;
        // Shared vertices: createMeshSharingVertices and createPosedPartMesh hand out handles naming the same vertex buffer.
        const u64 sliceKey = (static_cast<u64>(vb) << 32) | vc;   // both u32: exact, no collisions
        const auto [sit, fresh] = rtGeomVertSlice_.try_emplace(sliceKey, totalVerts);
        rtGeomFirstVertex_.push_back(sit->second);
        rtGeomFirstIndex_.push_back(totalIndices);
        rtGeomCopiesVerts_.push_back(fresh ? 1u : 0u);
        // Compute-written mesh slice: record for refreshDynamicVertexSlices to re-copy every frame.
        if (fresh && dev_->meshVertexBuffer(h)) rtDynamicVertexSlicesPending_.push_back({vb, vc, sit->second});
        if (fresh) totalVerts += vc;
        totalIndices += ic;
    }

    // Every instance points at its mesh's slice. Rewritten every frame as instance list changes.
    for (usize i = 0; i < rtInstanceMesh_.size(); ++i) {
        const auto it = std::lower_bound(rtGeomMeshes_.begin(), rtGeomMeshes_.end(), rtInstanceMesh_[i]);
        if (it == rtGeomMeshes_.end() || *it != rtInstanceMesh_[i]) return false;
        const usize slot = static_cast<usize>(it - rtGeomMeshes_.begin());
        rtInstanceData_[i].firstVertex = rtGeomFirstVertex_[slot];
        rtInstanceData_[i].firstIndex  = rtGeomFirstIndex_[slot];
    }
    // Foliage parts point at their slices the same way.
    for (usize i = 0; i < foliageParts_.size(); ++i) {
        const auto it = std::lower_bound(rtGeomMeshes_.begin(), rtGeomMeshes_.end(), foliageParts_[i].mesh);
        if (it == rtGeomMeshes_.end() || *it != foliageParts_[i].mesh) return false;
        const usize slot = static_cast<usize>(it - rtGeomMeshes_.begin());
        foliagePartData_[i].firstVertex = rtGeomFirstVertex_[slot];
        foliagePartData_[i].firstIndex  = rtGeomFirstIndex_[slot];
    }
    if (totalVerts == 0 || totalIndices == 0) return false;

    // Instance table goes up every call.
    if (!uploadRtInstanceTable()) return false;

    if (key == rtGeometryKey_ && rtGeometryReady_) {
        // Same set, same layout as GPU -- pending slices are valid.
        rtDynamicVertexSlices_.swap(rtDynamicVertexSlicesPending_);
        return true;
    }

    // Geometry buffers. Default-heap: written once, read by every reflection ray.
    if (rtVertCapacity_ < totalVerts) {
        if (rtVerts_) res_->destroyBuffer(rtVerts_);
        rhi::BufferDesc d;
        d.bytes = static_cast<u64>(totalVerts) * sizeof(rhi::MeshVertex);
        d.kind  = rhi::BufferKind::Default;
        d.debugName = "rt vertices";
        rtVerts_ = res_->createBuffer(d);
        rtVertCapacity_ = rtVerts_ ? totalVerts : 0;
    }
    if (rtIndexCapacity_ < totalIndices) {
        if (rtIndices_) res_->destroyBuffer(rtIndices_);
        rhi::BufferDesc d;
        d.bytes = static_cast<u64>(totalIndices) * sizeof(u32);
        d.kind  = rhi::BufferKind::Default;
        d.debugName = "rt indices";
        rtIndices_ = res_->createBuffer(d);
        rtIndexCapacity_ = rtIndices_ ? totalIndices : 0;
    }
    if (!rtVerts_ || !rtIndices_) return false;

    // Explicit state transitions for RHI tracking accuracy.
    ctx.bufferBarrier(rtVerts_,   rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
    ctx.bufferBarrier(rtIndices_, rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
    // Two copies per distinct mesh.
    u32 distinctSlices = 0;
    for (usize m = 0; m < rtGeomMeshes_.size(); ++m) {
        rhi::BufferHandle vb = 0, ib = 0;
        u32 vc = 0, ic = 0;
        if (!dev_->meshGeometry(rtGeomMeshes_[m], &vb, &ib, &vc, &ic)) return false;
        if (rtGeomCopiesVerts_[m]) {
            ++distinctSlices;
            // Compute-written mesh: filled every frame by refreshDynamicVertexSlices, not here.
            if (!dev_->meshVertexBuffer(rtGeomMeshes_[m])) {
                ctx.copyBuffer(rtVerts_, vb, static_cast<u64>(vc) * sizeof(rhi::MeshVertex),
                               static_cast<u64>(rtGeomFirstVertex_[m]) * sizeof(rhi::MeshVertex), 0);
            }
        }
        ctx.copyBuffer(rtIndices_, ib, static_cast<u64>(ic) * sizeof(u32),
                       static_cast<u64>(rtGeomFirstIndex_[m]) * sizeof(u32), 0);
    }
    ctx.bufferBarrier(rtVerts_,   rhi::ResourceState::CopyDest, rhi::ResourceState::Common);
    ctx.bufferBarrier(rtIndices_, rhi::ResourceState::CopyDest, rhi::ResourceState::Common);

    rtGeometryKey_ = key;
    rtGeometryReady_ = true;
    rtDynamicVertexSlices_.swap(rtDynamicVertexSlicesPending_);
    res_->setSrvBuffer(bindings_, 3, rtVerts_, sizeof(rhi::MeshVertex), totalVerts, 0);
    res_->setSrvBuffer(bindings_, 4, rtIndices_, sizeof(u32), totalIndices, 0);
    AVER_INFO("[Voxi] ray-traced reflection table: {} instances + {} foliage part(s) over {} distinct "
              "mesh(es) ({} vertex slice(s)), {} vertices, {} indices",
              rtInstanceData_.size(), foliageParts_.size(), rtGeomMeshes_.size(), distinctSlices, totalVerts,
              totalIndices);
    return true;
}

// Instance table rewritten every frame (transforms move). Upload buffer ring: every slot holds the largest table.
bool VoxiRenderer::uploadRtInstanceTable() {
    if (rtInstanceData_.empty()) return true;
    if (rtInstanceCapacity_ < rtInstanceData_.size()) {
        for (u32 i = 0; i < kRtInstanceRing; ++i) {
            if (rtInstances_[i]) res_->destroyBuffer(rtInstances_[i]);
            rhi::BufferDesc d;
            d.bytes = sizeof(RtInstance) * rtInstanceData_.size();
            d.kind  = rhi::BufferKind::Upload;
            d.debugName = "rt instances";
            rtInstances_[i] = res_->createBuffer(d);
            if (!rtInstances_[i]) { rtInstanceCapacity_ = 0; return false; }
        }
        rtInstanceCapacity_ = static_cast<u32>(rtInstanceData_.size());
    }
    // Rotate before writing so this frame never touches the buffer the previous frame reads.
    rtInstanceSlot_ = (rtInstanceSlot_ + 1) % kRtInstanceRing;
    const rhi::BufferHandle inst = rtInstances_[rtInstanceSlot_];
    if (!inst) return false;
    res_->writeBuffer(inst, rtInstanceData_.data(),
                      sizeof(RtInstance) * rtInstanceData_.size(), 0);
    res_->setSrvBuffer(bindings_, 5, inst, sizeof(RtInstance),
                       static_cast<u32>(rtInstanceData_.size()), 0);
    return true;
}

// Re-copies every compute-written mesh's current vertex buffer into its rtVerts_ slice.
// Runs after buildGeometryTable on full path or directly on refit-only path.
void VoxiRenderer::refreshDynamicVertexSlices(rhi::IRenderContext& ctx) {
    if (rtDynamicVertexSlices_.empty() || !rtVerts_) return;
    // Same barrier pattern as buildGeometryTable for RHI buffer-state tracking.
    ctx.bufferBarrier(rtVerts_, rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
    for (const DynamicVertexSlice& s : rtDynamicVertexSlices_) {
        // Posed buffer in GeometryRead after skinning dispatch.
        ctx.bufferBarrier(s.vb, rhi::ResourceState::GeometryRead, rhi::ResourceState::CopySource);
        ctx.copyBuffer(rtVerts_, s.vb, static_cast<u64>(s.vertexCount) * sizeof(rhi::MeshVertex),
                       static_cast<u64>(s.firstVertex) * sizeof(rhi::MeshVertex), 0);
        ctx.bufferBarrier(s.vb, rhi::ResourceState::CopySource, rhi::ResourceState::GeometryRead);
    }
    ctx.bufferBarrier(rtVerts_, rhi::ResourceState::CopyDest, rhi::ResourceState::Common);
}

// Makes one texture resident in the ray path's bindless table and returns its index.
// Append-only and memoised: the same texture asked twice returns the same index.
u32 VoxiRenderer::residentTexture(rhi::TextureHandle h) {
    if (!h || !rtTexTable_) return pbr::kUnboundTexture;
    if (const auto it = rtTexIndex_.find(h); it != rtTexIndex_.end()) return it->second;
    if (rtTexNext_ >= kRtTextureCapacity) {
        // Said once only.
        if (!rtTexLogged_) {
            rtTexLogged_ = true;
            AVER_WARN("[Voxi] ray-traced texture table full at {} distinct textures; further "
                      "materials keep their factor colour instead of sampling. Raise "
                      "kRtTextureCapacity if a real project needs more.", kRtTextureCapacity);
        }
        return pbr::kUnboundTexture;
    }
    const u32 index = rtTexNext_;
    // RHI refuses out-of-range index; refusal leaves slot unbound.
    if (!res_->setBindlessTexture(rtTexTable_, index, h)) return pbr::kUnboundTexture;
    ++rtTexNext_;
    rtTexIndex_.emplace(h, index);
    return index;
}

// Creates the bindless texture table on first need, once only.
void VoxiRenderer::ensureTextureTable() {
    if (rtTexTableTried_ || !res_) return;
    rtTexTableTried_ = true;
    if (!caps_.rtBindlessTextures) return;   // silent: the caps line already said so once
    rtTexTable_ = res_->createBindlessTextureTable(kRtTextureCapacity);
    if (!rtTexTable_)
        AVER_WARN("[Voxi] no bindless texture table; ray hits will shade from material factors "
                  "alone, as they did before textured ray hits existed");
}

// Builds dense material table from per-draw records. Re-uploads only when content changes.
// Sorted by key, not first-seen order, for stable indices across frame reorders.
bool VoxiRenderer::buildMaterialTable(const std::unordered_map<u64, pbr::MaterialConstants>& matConstantsByKey) {
    // Distinct keys, sorted: std::map would give this for free, but sorting the "tens not thousands" keys once here is faster.
    std::vector<u64> sortedKeys;
    sortedKeys.reserve(matConstantsByKey.size());
    for (const auto& [k, mc] : matConstantsByKey) { (void)mc; sortedKeys.push_back(k); }
    std::sort(sortedKeys.begin(), sortedKeys.end());

    // Index 0 is always the fallback sentinel for unresolved materials.
    rtMaterialData_.clear();
    rtMaterialData_.reserve(sortedKeys.size() + 1);
    rtMaterialData_.push_back(materials_.fallbackConstants());

    std::unordered_map<u64, u32> indexByKey;
    indexByKey.reserve(sortedKeys.size());
    for (u64 k : sortedKeys) {
        indexByKey.emplace(k, static_cast<u32>(rtMaterialData_.size()));
        rtMaterialData_.push_back(matConstantsByKey.at(k));
    }

    // Lookup pass: every instance gets its final dense index, lockstep with buildAccelerationStructures.
    for (usize i = 0; i < rtInstanceData_.size(); ++i) {
        const auto it = indexByKey.find(rtInstanceMatKey_[i]);
        rtInstanceData_[i].materialIndex = (it != indexByKey.end()) ? it->second : 0u;
    }
    // Foliage parts, keyed by resolveFoliageMaterials.
    for (usize i = 0; i < foliagePartData_.size(); ++i) {
        const auto it = indexByKey.find(foliagePartMatKey_[i]);
        foliagePartData_[i].materialIndex = (it != indexByKey.end()) ? it->second : 0u;
    }

    // ---- re-upload only when the content actually differs from what the GPU already holds ----
    const bool changed = rtMaterialData_.size() != rtMaterialUploaded_.size() ||
        std::memcmp(rtMaterialData_.data(), rtMaterialUploaded_.data(),
                    rtMaterialData_.size() * sizeof(pbr::MaterialConstants)) != 0;
    const bool needGrow = rtMaterialCapacity_ < rtMaterialData_.size();
    if (!changed && !needGrow && rtMaterialsReady_) return true;   // GPU content already correct

    if (needGrow) {
        // Upload-heap ring, matching rtInstances_ exactly.
        for (u32 i = 0; i < kRtInstanceRing; ++i) {
            if (rtMaterials_[i]) res_->destroyBuffer(rtMaterials_[i]);
            rhi::BufferDesc d;
            d.bytes = sizeof(pbr::MaterialConstants) * rtMaterialData_.size();
            d.kind  = rhi::BufferKind::Upload;
            d.debugName = "rt materials";
            rtMaterials_[i] = res_->createBuffer(d);
            if (!rtMaterials_[i]) {
                rtMaterialCapacity_ = 0;
                rtMaterialsReady_ = false;
                if (!rtMaterialAllocFailLogged_) {
                    AVER_ERROR("[Voxi] material table buffer could not be (re)created at {} elements; "
                               "ray hits read stale or null-filled material data until this succeeds",
                               rtMaterialData_.size());
                    rtMaterialAllocFailLogged_ = true;
                }
                return false;
            }
        }
        rtMaterialCapacity_ = static_cast<u32>(rtMaterialData_.size());
    }

    // Rotate before writing so this build never touches the slot a previous frame reads.
    rtMaterialSlot_ = (rtMaterialSlot_ + 1) % kRtInstanceRing;
    res_->writeBuffer(rtMaterials_[rtMaterialSlot_], rtMaterialData_.data(),
                      sizeof(pbr::MaterialConstants) * rtMaterialData_.size(), 0);
    res_->setSrvBuffer(bindings_, 9, rtMaterials_[rtMaterialSlot_], sizeof(pbr::MaterialConstants),
                       static_cast<u32>(rtMaterialData_.size()), 0);
    const usize prevUploadedCount = rtMaterialUploaded_.size();
    rtMaterialUploaded_ = rtMaterialData_;
    rtMaterialsReady_ = true;

    // Log when material count changes, not every frame.
    if (rtMaterialData_.size() != prevUploadedCount) {
        const usize tableBytes    = rtMaterialData_.size() * sizeof(pbr::MaterialConstants);
        const usize residentBytes = static_cast<usize>(rtMaterialCapacity_) * sizeof(pbr::MaterialConstants) *
                                    kRtInstanceRing;
        AVER_INFO("[Voxi] ray-traced material table: {} distinct material(s) (was {}; index 0 the "
                  "fallback sentinel) = {} bytes, x{} ring slots = {} bytes resident. Re-uploaded only "
                  "when its content changes, not every frame.",
                  rtMaterialData_.size(), prevUploadedCount, tableBytes, kRtInstanceRing, residentBytes);
    }
    return true;
}

// AUTHORED means matSet is one of materials_'s own binding sets, not its fallback.
// Key off material set or synthesized from color/metallic/roughness.
u64 VoxiRenderer::rtMaterialKey(rhi::BindingSetHandle matSet, const void* authoredBytes, const f32 color[4],
                                f32 metallic, f32 roughness) {
    const bool authored = materials_.ownsBindingSet(matSet) && matSet != materials_.fallbackBindingSet();
    // Authored: bit 63, emissive tag in bits 32..62, set handle in the low 32. Constants come from the
    // first draw seen with that exact key, so a per-draw emissive change gets its own row.
    static_assert(sizeof(rhi::BindingSetHandle) <= sizeof(u32), "authored key packs the set handle in 32 bits");
    const u64 matKey = authored ? ((1ull << 63) | (static_cast<u64>(emissiveKeyTag(authoredBytes)) << 32) |
                                   static_cast<u64>(matSet))
                                : synthMaterialKey(color, metallic, roughness);
    if (matConstantsScratch_.find(matKey) != matConstantsScratch_.end()) return matKey;
    pbr::MaterialConstants mc;
    if (authored) {
        // Real per-material bytes.
        std::memcpy(&mc, authoredBytes, sizeof(mc));

        // Textures resolved to indices in the bindless table.
        if (const auto* tex = materials_.textures(matSet)) {
            for (u32 t = 0; t < pbr::kTextureSlotCount; ++t)
                mc.texIndex[t] = residentTexture((*tex)[t]);
        }
    } else {
        // The fallback material, as raster binds for this draw. The draw's colour, metallic and roughness
        // reach the shader as its PER-DRAW terms (RtInstance -> rtDrawTerms), applied once like raster's
        // gBaseColor/gMaterial; folding them into the factors too squared them on every ray hit. Only
        // the alpha stays here: the shadow walk reads coverage from the material.
        (void)metallic; (void)roughness;
        mc = materials_.fallbackConstants();
        mc.baseColorFactor[3] = color[3];
    }
    matConstantsScratch_.emplace(matKey, mc);
    return matKey;
}

// ---- INSTANCED FOLIAGE -- see setFoliage's declaration for the whole scheme ----

void VoxiRenderer::setFoliage(std::vector<FoliagePrototype> prototypes, std::vector<FoliageInstance> instances) {
    clearFoliage();
    if (prototypes.empty() || instances.empty()) return;
    if (!res_ || !rtSupported_ || !tlas_) {
        AVER_WARN("[Voxi] foliage refused: {} instance(s) of {} prototype(s) -- foliage is ray-traced only, and "
                  "this renderer has no acceleration structure (no ray tracing on this device, or not initialised)",
                  instances.size(), prototypes.size());
        return;
    }
    if (instances.size() > kMaxFoliageInstances) {
        AVER_WARN("[Voxi] foliage: {} instances truncated to kMaxFoliageInstances ({})",
                  instances.size(), kMaxFoliageInstances);
        instances.resize(kMaxFoliageInstances);
    }

    // One multi-geometry BLAS per prototype; parts join in geometry order.
    std::vector<u32> protoFirstPart(prototypes.size(), 0);
    std::vector<rhi::BlasHandle> protoBlas(prototypes.size(), 0);
    std::vector<rhi::BlasGeometry> geoms;
    u32 droppedParts = 0, droppedProtos = 0;
    for (usize p = 0; p < prototypes.size(); ++p) {
        const u32 first = static_cast<u32>(foliageParts_.size());
        geoms.clear();
        for (const FoliagePart& fp : prototypes[p].parts) {
            if (!fp.mesh || geoms.size() == kMaxFoliagePartsPerPrototype) { ++droppedParts; continue; }
            FoliagePartState s;
            s.mesh = fp.mesh;
            s.material = fp.material;
            s.matSet = fp.material ? materials_.bindingSet(fp.material) : 0;
            std::memcpy(s.color, fp.color, sizeof(s.color));
            s.metallic = fp.metallic;
            s.roughness = fp.roughness;
            // Opacity per geometry: alpha-masked parts reach cutout test, others skip.
            const pbr::MaterialConstants& mc = fp.material ? materials_.constants(fp.material)
                                                           : materials_.fallbackConstants();
            geoms.push_back({fp.mesh, (mc.flags & pbr::MaterialFlag_AlphaMask) == 0});
            foliageParts_.push_back(s);
        }
        // First part's row fits in kRtFoliageIdBit.
        const bool fits = first + geoms.size() <= kRtFoliageIdBit;
        const rhi::BlasHandle b = (!geoms.empty() && fits)
            ? res_->createBlasMulti(geoms.data(), static_cast<u32>(geoms.size())) : 0;
        if (!b) {
            foliageParts_.resize(first);
            ++droppedProtos;
            continue;
        }
        protoFirstPart[p] = first;
        protoBlas[p] = b;
        foliageBlas_.push_back(b);
    }

    std::vector<rhi::TlasInstance> tlasInst;
    tlasInst.reserve(instances.size());
    u32 droppedInstances = 0;
    for (const FoliageInstance& fi : instances) {
        if (fi.prototype >= protoBlas.size() || !protoBlas[fi.prototype]) { ++droppedInstances; continue; }
        rhi::TlasInstance t;
        // Back to 4x4 form (0,0,0,1) 4th column restored.
        for (u32 r = 0; r < 4; ++r) {
            for (u32 c = 0; c < 3; ++c) t.world[r * 4 + c] = fi.world[r * 3 + c];
            t.world[r * 4 + 3] = r == 3 ? 1.0f : 0.0f;
        }
        t.mask = kRtMaskOpaque;   // the lane every ray type asks for, the primary ray included
        t.blas = protoBlas[fi.prototype];
        t.instanceId = kRtFoliageIdBit | protoFirstPart[fi.prototype];
        // Two-sided: leaf cards visible from behind.
        t.flags = rhi::TlasInstanceFlag_TriangleCullDisable;
        tlasInst.push_back(t);
    }
    // Caller's copy is ours.
    instances.clear();
    instances.shrink_to_fit();

    const u64 tlasBytesBefore = res_->tlasMemoryBytes(tlas_);
    if (tlasInst.empty() ||
        !res_->setTlasStaticInstances(tlas_, tlasInst.data(), static_cast<u32>(tlasInst.size()))) {
        AVER_WARN("[Voxi] foliage refused: the acceleration structure took none of it ({} instance(s) left to "
                  "place after dropping {} prototype(s) and {} instance(s))",
                  tlasInst.size(), droppedProtos, droppedInstances);
        clearFoliage();
        return;
    }
    const u64 tlasBytesAfter = res_->tlasMemoryBytes(tlas_);
    foliagePrefixBytes_ = tlasBytesAfter > tlasBytesBefore ? tlasBytesAfter - tlasBytesBefore : 0;
    for (rhi::BlasHandle b : foliageBlas_) foliageBlasBytes_ += res_->blasMemoryBytes(b);
    foliageInstances_ = static_cast<u32>(tlasInst.size());

    for (const FoliagePartState& s : foliageParts_) foliageMeshes_.push_back(s.mesh);
    std::sort(foliageMeshes_.begin(), foliageMeshes_.end());
    foliageMeshes_.erase(std::unique(foliageMeshes_.begin(), foliageMeshes_.end()), foliageMeshes_.end());
    // Part records with geometry and material indices, objectToWorld identity.
    foliagePartData_.assign(foliageParts_.size(), RtInstance{});
    for (usize i = 0; i < foliageParts_.size(); ++i) {
        RtInstance& r = foliagePartData_[i];
        for (u32 k = 0; k < 16; ++k) r.objectToWorld[k] = (k % 5 == 0) ? 1.0f : 0.0f;
        // Foliage is static: previous transform is current.
        std::memcpy(r.prevObjectToWorld, r.objectToWorld, sizeof(r.prevObjectToWorld));
        r.albedo[0] = foliageParts_[i].color[0];
        r.albedo[1] = foliageParts_[i].color[1];
        r.albedo[2] = foliageParts_[i].color[2];
        r.metallic  = foliageParts_[i].metallic;
        r.roughness = foliageParts_[i].roughness;
    }
    foliagePartMatKey_.assign(foliageParts_.size(), 0);
    foliageBlasPending_ = true;
    foliageBindingsDirty_ = true;
    ++foliageGeneration_;
    AVER_INFO("[Voxi] foliage: {} prototype(s), {} part(s), {} instance(s) -- BLAS {:.1f} MiB, TLAS prefix "
              "{:.1f} MiB (dropped: {} prototype(s), {} part(s), {} instance(s))",
              foliageBlas_.size(), foliageParts_.size(), foliageInstances_,
              static_cast<f64>(foliageBlasBytes_) / (1024.0 * 1024.0),
              static_cast<f64>(foliagePrefixBytes_) / (1024.0 * 1024.0),
              droppedProtos, droppedParts, droppedInstances);
}

// Clears foliage. Prefix removed first; destroy calls are fence-deferred.
void VoxiRenderer::clearFoliage() {
    if (foliageBlas_.empty() && foliageParts_.empty() && foliageInstances_ == 0) return;
    if (res_) {
        if (tlas_) res_->setTlasStaticInstances(tlas_, nullptr, 0);
        for (rhi::BlasHandle b : foliageBlas_) res_->destroyBlas(b);
        for (rhi::BufferHandle& b : foliagePartBuf_) { if (b) res_->destroyBuffer(b); b = 0; }
    }
    if (foliageInstances_)
        AVER_INFO("[Voxi] foliage cleared: {} instance(s) of {} prototype(s) released",
                  foliageInstances_, foliageBlas_.size());
    foliageParts_.clear();
    foliageBlas_.clear();
    foliageMeshes_.clear();
    foliagePartData_.clear();
    foliagePartUploaded_.clear();
    foliagePartMatKey_.clear();
    foliageInstances_ = 0;
    foliageBlasBytes_ = foliagePrefixBytes_ = 0;
    foliagePartCapacity_ = foliagePartSlot_ = 0;
    foliageBlasPending_ = false;
    foliageBindingsDirty_ = true;
    ++foliageGeneration_;
}

VoxiRenderer::FoliageStats VoxiRenderer::foliageStats() const {
    FoliageStats s;
    s.prototypes = static_cast<u32>(foliageBlas_.size());
    s.parts = static_cast<u32>(foliageParts_.size());
    s.instances = foliageInstances_;
    s.gpuBytes = foliageBlasBytes_ + foliagePrefixBytes_ +
                 static_cast<u64>(foliagePartCapacity_) * sizeof(RtInstance) * kRtInstanceRing;
    return s;
}

void VoxiRenderer::resolveFoliageMaterials() {
    for (usize i = 0; i < foliageParts_.size(); ++i) {
        const FoliagePartState& p = foliageParts_[i];
        // Same constants block a draw's submit-time capture reads.
        const pbr::MaterialConstants& mc = p.material ? materials_.constants(p.material)
                                                      : materials_.fallbackConstants();
        foliagePartMatKey_[i] = rtMaterialKey(p.matSet, &mc, p.color, p.metallic, p.roughness);
    }
}

// Part table compared byte-for-byte: only moves when geometry slice or material index changed.
void VoxiRenderer::uploadFoliagePartTable() {
    if (foliagePartData_.empty() || !bindings_) return;
    const bool changed = foliagePartData_.size() != foliagePartUploaded_.size() ||
        std::memcmp(foliagePartData_.data(), foliagePartUploaded_.data(),
                    foliagePartData_.size() * sizeof(RtInstance)) != 0;
    if (!changed) return;
    if (foliagePartCapacity_ < foliagePartData_.size()) {
        // Rebind before destroying to prevent dangling views.
        res_->setSrvBuffer(bindings_, 20, foliagePartPlaceholder_, sizeof(RtInstance), 1, 0);
        foliagePartUploaded_.clear();
        foliagePartCapacity_ = 0;
        for (rhi::BufferHandle& b : foliagePartBuf_) { if (b) res_->destroyBuffer(b); b = 0; }
        for (rhi::BufferHandle& b : foliagePartBuf_) {
            rhi::BufferDesc d;
            d.bytes = sizeof(RtInstance) * foliagePartData_.size();
            d.kind  = rhi::BufferKind::Upload;
            d.debugName = "Voxi foliage part table";
            b = res_->createBuffer(d);
            if (!b) {
                AVER_ERROR("[Voxi] foliage part table ({} parts) could not be created; foliage hits read the "
                           "placeholder until it can", foliagePartData_.size());
                for (rhi::BufferHandle& c : foliagePartBuf_) { if (c) res_->destroyBuffer(c); c = 0; }
                return;
            }
        }
        foliagePartCapacity_ = static_cast<u32>(foliagePartData_.size());
    }
    // Rotate before writing so this build never touches the slot an in-flight frame may read.
    foliagePartSlot_ = (foliagePartSlot_ + 1) % kRtInstanceRing;
    res_->writeBuffer(foliagePartBuf_[foliagePartSlot_], foliagePartData_.data(),
                      sizeof(RtInstance) * foliagePartData_.size(), 0);
    res_->setSrvBuffer(bindings_, 20, foliagePartBuf_[foliagePartSlot_], sizeof(RtInstance),
                       static_cast<u32>(foliagePartData_.size()), 0);
    foliagePartUploaded_ = foliagePartData_;
}

void VoxiRenderer::refreshFoliageBindings() {
    if (!foliageBindingsDirty_ || !res_ || !bindings_) return;
    foliageBindingsDirty_ = false;
    // t2: TLAS structure reallocated; named before this frame's build fills it.
    if (tlas_) res_->setSrvTlas(bindings_, 2, tlas_);
    const rhi::BufferHandle descs = foliageInstances_ ? res_->tlasStaticInstanceBuffer(tlas_) : 0;
    if (descs) res_->setSrvBuffer(bindings_, 21, descs, rhi::kTlasInstanceDescBytes, foliageInstances_, 0);
    else       res_->setSrvBuffer(bindings_, 21, foliageDescPlaceholder_, rhi::kTlasInstanceDescBytes, 1, 0);
    // t20: real table once uploadFoliagePartTable sends one.
    if (!foliagePartUploaded_.empty())
        res_->setSrvBuffer(bindings_, 20, foliagePartBuf_[foliagePartSlot_], sizeof(RtInstance),
                           static_cast<u32>(foliagePartUploaded_.size()), 0);
    else
        res_->setSrvBuffer(bindings_, 20, foliagePartPlaceholder_, sizeof(RtInstance), 1, 0);
}

// Same finalised-sum shape as rtAccelDrawsKey. Material reads from material system's dense table.
u64 VoxiRenderer::foliageKey() const {
    if (foliageGeneration_ == 0) return 0;   // never set: key is what it was before foliage existed
    u64 h = 1469598103934665603ull;
    h ^= foliageGeneration_; h *= 1099511628211ull;
    const pbr::MaterialConstants* table = materials_.gpuMaterialTable();
    const u32 rows = materials_.gpuMaterialCount();
    for (const FoliagePartState& p : foliageParts_) {
        h ^= static_cast<u64>(p.matSet); h *= 1099511628211ull;
        const u32 row = materials_.gpuMaterialIndex(p.material);
        if (p.material && table && row < rows) fnvMix(h, &table[row], sizeof(pbr::MaterialConstants));
        if (const auto* tex = materials_.textures(p.matSet)) fnvMix(h, tex->data(), sizeof(*tex));
    }
    h ^= h >> 33; h *= 0xff51afd7ed558ccdull;
    h ^= h >> 33; h *= 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 33;
    return h;
}

static_assert(kEngineUnitCdm2 == rhi::kLuminanceToCdm2, "SceneLight.hpp mirrors the engine's radiance unit");
static_assert(fmt::kIesTableV == 64 && fmt::kIesTableH == 32, "AVER_IES_V/H in aver_lights.hlsli mirror these");

void VoxiRenderer::setSceneLights(const SceneLight* lights, u32 count) {
    sceneLights_.assign(lights, lights ? lights + count : lights);
}

bool VoxiRenderer::registerIesProfile(u64 id, const u16* tableHalf, f32 peakOverMean) {
    if (!id || !tableHalf) return false;
    if (lightAssetsCpu_.count(id)) return true;
    LightAssetCpu a;
    a.width = fmt::kIesTableH;
    a.height = fmt::kIesTableV;
    a.ies = true;
    a.peakOverMean = peakOverMean > 0.0f ? peakOverMean : 1.0f;
    a.bytes.resize(static_cast<usize>(a.width) * a.height * sizeof(u16));
    std::memcpy(a.bytes.data(), tableHalf, a.bytes.size());
    lightAssetsCpu_.emplace(id, std::move(a));
    return true;
}

bool VoxiRenderer::registerCookie(u64 id, u32 width, u32 height, const u8* rgba8) {
    if (!id || !rgba8 || width == 0 || height == 0) return false;
    if (lightAssetsCpu_.count(id)) return true;
    LightAssetCpu a;
    a.width = width;
    a.height = height;
    a.bytes.assign(rgba8, rgba8 + static_cast<usize>(width) * height * 4u);
    lightAssetsCpu_.emplace(id, std::move(a));
    return true;
}

// Makes a registered asset resident in the ray path's bindless table, creating its texture on first use.
u32 VoxiRenderer::lightAssetIndex(u64 id) {
    if (!id || !res_) return pbr::kUnboundTexture;
    const auto cpu = lightAssetsCpu_.find(id);
    if (cpu == lightAssetsCpu_.end()) return pbr::kUnboundTexture;
    LightAssetGpu& gpu = lightAssetsGpu_[id];
    if (gpu.failed) return pbr::kUnboundTexture;
    if (!gpu.tex) {
        ensureTextureTable();
        if (!rtTexTable_) return pbr::kUnboundTexture;   // no bindless: lights keep their plain shape
        const LightAssetCpu& a = cpu->second;
        rhi::TextureDesc d;
        d.width = a.width;
        d.height = a.height;
        d.format = a.ies ? rhi::Format::R16F : rhi::Format::RGBA8UnormSrgb;
        d.bind = rhi::ResourceBind::ShaderResource;
        d.initialState = rhi::ResourceState::ShaderResource;
        const void* levels[1] = {a.bytes.data()};
        d.initialData = levels;
        d.initialDataCount = 1;
        d.initialRowPitch = a.width * (a.ies ? 2u : 4u);
        d.debugName = a.ies ? "Voxi light IES profile" : "Voxi light cookie";
        gpu.tex = res_->createTexture(d);
        if (!gpu.tex) {
            gpu.failed = true;
            AVER_WARN("[Voxi] light asset {:016x} could not be uploaded; the light renders without it", id);
            return pbr::kUnboundTexture;
        }
    }
    if (gpu.index == pbr::kUnboundTexture) gpu.index = residentTexture(gpu.tex);
    return gpu.index;
}

void VoxiRenderer::releaseLightAssets() {
    if (res_)
        for (auto& kv : lightAssetsGpu_)
            if (kv.second.tex) res_->destroyTexture(kv.second.tex);
    lightAssetsGpu_.clear();
}

// ---- projected decals ---------------------------------------------------------------------------------

static_assert(kDecalUnboundTexture == pbr::kUnboundTexture, "SceneDecal.hpp mirrors the material system's unbound index");

void VoxiRenderer::setSceneDecals(const SceneDecal* decals, u32 count) {
    sceneDecals_.assign(decals, decals ? decals + count : decals);
}

bool VoxiRenderer::registerDecalTexture(u64 id, u32 width, u32 height, const u8* rgba8, DecalImageKind kind) {
    if (!id || !rgba8 || width == 0 || height == 0) return false;
    if (decalTexCpu_.count(id)) return true;
    DecalTexCpu t;
    t.width = width;
    t.height = height;
    t.kind = kind;
    t.levels.emplace_back(rgba8, rgba8 + static_cast<usize>(width) * height * 4u);   // level 0 only: mips at upload
    decalTexCpu_.emplace(id, std::move(t));
    return true;
}

// Makes a registered decal image resident in the ray path's bindless table, uploading it (with its mip
// chain) on first use. The CPU copy is dropped once the texture exists.
u32 VoxiRenderer::decalTextureIndex(u64 id) {
    if (!id || !res_) return pbr::kUnboundTexture;
    const auto cpu = decalTexCpu_.find(id);
    if (cpu == decalTexCpu_.end()) return pbr::kUnboundTexture;
    DecalTexGpu& gpu = decalTexGpu_[id];
    if (gpu.failed) return pbr::kUnboundTexture;
    if (!gpu.tex) {
        ensureTextureTable();
        if (!rtTexTable_) return pbr::kUnboundTexture;   // no bindless table: textured decals are skipped
        DecalTexCpu& a = cpu->second;
        if (a.levels.empty()) { gpu.failed = true; return pbr::kUnboundTexture; }
        const std::vector<std::vector<u8>> levels = buildDecalMips(a.levels.front().data(), a.width, a.height, a.kind);
        std::vector<const void*> ptrs;
        ptrs.reserve(levels.size());
        for (const std::vector<u8>& l : levels) ptrs.push_back(l.data());
        rhi::TextureDesc d;
        d.width = a.width;
        d.height = a.height;
        d.mips = static_cast<u32>(levels.size());
        d.format = a.kind == DecalImageKind::Colour ? rhi::Format::RGBA8UnormSrgb : rhi::Format::RGBA8Unorm;
        d.bind = rhi::ResourceBind::ShaderResource;
        d.initialState = rhi::ResourceState::ShaderResource;
        d.initialData = ptrs.data();
        d.initialDataCount = static_cast<u32>(ptrs.size());
        d.debugName = "Voxi decal image";
        gpu.tex = res_->createTexture(d);
        if (!gpu.tex) {
            gpu.failed = true;
            AVER_WARN("[Voxi] decal image {:016x} could not be uploaded; decals using it are not drawn", id);
            return pbr::kUnboundTexture;
        }
        a.levels.clear();
        a.levels.shrink_to_fit();
    }
    if (gpu.index == pbr::kUnboundTexture) gpu.index = residentTexture(gpu.tex);
    return gpu.index;
}

void VoxiRenderer::releaseDecalTextures() {
    if (res_)
        for (auto& kv : decalTexGpu_)
            if (kv.second.tex) res_->destroyTexture(kv.second.tex);
    decalTexGpu_.clear();
}

// Culls this frame's decals, packs them eye-relative, sorts them into paint order and uploads the list
// to t24. Zero decals leave the placeholder bound and cb_.decalParams[0] at 0, so the shaders skip
// every decal call.
void VoxiRenderer::buildDecals() {
    cb_.decalParams[0] = 0.0f;
    decalCount_ = 0;
    decalData_.clear();
    decalCand_.clear();
    if (!res_ || !bindings_) return;

    if (!sceneDecals_.empty()) {
        f32 vp[16], eye[3] = {};
        if (!(dev_ && dev_->camera(vp, nullptr, eye))) eye[0] = eye[1] = eye[2] = 0.0f;
        for (const SceneDecal& d : sceneDecals_) {
            if (!(d.opacity > 0.0f) || !std::isfinite(d.opacity)) continue;
            f32 c[3], r = 0.0f;
            decalBounds(d, c, r);
            const f32 dx = c[0] - eye[0], dy = c[1] - eye[1], dz = c[2] - eye[2];
            const f32 dist = std::sqrt(dx * dx + dy * dy + dz * dz);
            SceneDecal e = d;
            e.opacity *= decalDistanceFade(dist, d.fadeDistanceCm);
            if (!(e.opacity > 0.0f)) continue;

            // Every image the decal names must be resident, or it would draw as a flat tinted box.
            DecalTextureIndices tx;
            bool resident = true;
            if (d.baseId)   { tx.base   = decalTextureIndex(d.baseId);   resident = resident && tx.base   != kDecalUnboundTexture; }
            if (d.normalId) { tx.normal = decalTextureIndex(d.normalId); resident = resident && tx.normal != kDecalUnboundTexture; }
            if (d.ormId)    { tx.orm    = decalTextureIndex(d.ormId);    resident = resident && tx.orm    != kDecalUnboundTexture; }
            if (!resident) continue;

            DecalCand cand{};
            if (!packSceneDecal(e, eye, tx, cand.packed)) continue;
            cand.importance = r / std::max(dist, 1.0f);
            cand.dist = dist;
            cand.order = d.sortOrder;
            decalCand_.push_back(cand);
        }
    }
    if (decalCand_.size() > kMaxSceneDecals) {
        // Nearest and largest survive; the rest are too small or far to be worth a pixel test.
        std::partial_sort(decalCand_.begin(), decalCand_.begin() + kMaxSceneDecals, decalCand_.end(),
                          [](const DecalCand& a, const DecalCand& b) { return a.importance > b.importance; });
        decalCand_.resize(kMaxSceneDecals);
    }
    std::stable_sort(decalCand_.begin(), decalCand_.end(), [](const DecalCand& a, const DecalCand& b) {
        return decalPaintsBefore(a.order, a.dist, b.order, b.dist);
    });
    decalData_.reserve(decalCand_.size());
    for (const DecalCand& c : decalCand_) decalData_.push_back(c.packed);

    if (!decalData_.empty()) {
        // Grown straight to the cap on first need, every slot or none (a half-built ring would bind a
        // null buffer on its missing turns).
        if (decalBufCapacity_ < kMaxSceneDecals) {
            bool ok = true;
            for (u32 i = 0; i < kRtInstanceRing; ++i) {
                if (decalBuf_[i]) res_->destroyBuffer(decalBuf_[i]);
                rhi::BufferDesc bd;
                bd.bytes = sizeof(PackedDecal) * kMaxSceneDecals;
                bd.kind = rhi::BufferKind::Upload;
                bd.debugName = "Voxi decals";
                decalBuf_[i] = res_->createBuffer(bd);
                ok = ok && decalBuf_[i] != 0;
            }
            decalBufCapacity_ = ok ? kMaxSceneDecals : 0;
        }
        if (decalBufCapacity_ >= decalData_.size()) {
            // Rotate BEFORE writing, so this frame never touches the buffer the previous one bound.
            decalBufSlot_ = (decalBufSlot_ + 1) % kRtInstanceRing;
            const rhi::BufferHandle buf = decalBuf_[decalBufSlot_];
            const u32 count = static_cast<u32>(decalData_.size());
            res_->writeBuffer(buf, decalData_.data(), sizeof(PackedDecal) * count, 0);
            res_->setSrvBuffer(bindings_, 24, buf, sizeof(PackedDecal), count, 0);
            decalBound_ = buf;
            decalCount_ = count;
            cb_.decalParams[0] = static_cast<f32>(count);
            if (!decalLoggedRun_) {
                decalLoggedRun_ = true;
                AVER_INFO("[Voxi] decals running: {} this frame{}", count,
                          rtTexTable_ ? "" : " (flat only: no bindless texture table, textured decals are skipped)");
            }
        } else if (!decalFailLogged_) {
            decalFailLogged_ = true;
            AVER_ERROR("[Voxi] decal list buffer could not be created; decals are not drawn (said once)");
        }
    }
    if (decalCount_ == 0 && decalPlaceholder_ && decalBound_ != decalPlaceholder_) {
        res_->setSrvBuffer(bindings_, 24, decalPlaceholder_, sizeof(PackedDecal), 1, 0);
        decalBound_ = decalPlaceholder_;
    }
}

// Packs this frame's scene lights into the candidate list the lamp list is cut from.
void VoxiRenderer::appendSceneLightCandidates(const f32 eye[3]) {
    for (const SceneLight& s : sceneLights_) {
        if (!(s.intensityCd > 0.0f) || !std::isfinite(s.intensityCd)) continue;
        SceneLightAssets a;
        if (s.iesId) {
            const u32 idx = lightAssetIndex(s.iesId);
            if (idx != pbr::kUnboundTexture) {
                a.iesIndex = static_cast<f32>(idx);
                a.iesPeakOverMean = lightAssetsCpu_[s.iesId].peakOverMean;
            }
        }
        if (s.cookieId) {
            const u32 idx = lightAssetIndex(s.cookieId);
            if (idx != pbr::kUnboundTexture) a.cookieIndex = static_cast<f32>(idx);
        }
        RdLocalLightCand c{};
        c.fromDraw = false;
        f32 e1m = 0.0f;
        c.light = packSceneLight(s, a, &e1m);
        const f32 dx = (s.pos[0] - eye[0]) * 0.01f;   // cm -> m
        const f32 dy = (s.pos[1] - eye[1]) * 0.01f;
        const f32 dz = (s.pos[2] - eye[2]) * 0.01f;
        c.importance = e1m / std::max(dx * dx + dy * dy + dz * dz, 1.0f);
        rdLocalLightCand_.push_back(c);
    }
}

// Local lights (lamps): non-empty when any draw has MaterialFlag_Light with lightIntensity > 0.
// Range scales with emissive factor and bounding-sphere radius; lit to kLocalLightRangeCutoff of sun units.
void VoxiRenderer::buildLocalLights() {
    rdLocalLightCount_ = 0;
    rdLocalLampCount_ = 0;
    rdLocalLightHash_ = 0;
    rdLocalLightsCarryAll_ = false;
    rdLocalLightData_.clear();
    rdLocalLightCand_.clear();
    cb_.decalParams[1] = 0.0f;   // no light grid until one is uploaded
    cb_.decalParams[2] = static_cast<f32>(std::clamp(settings_.lightRaysPerBlock, 1u, 8u));
    cb_.decalParams[3] = static_cast<f32>(std::clamp(settings_.lightRaysPerHit, 1u, 8u));
    rdGridKeyNow_ = 0;
    if (!res_ || !bindings_) return;

    f32 vp[16], eye[3] = {};
    const bool wanted = settings_.localLights && rtActive_ && dev_ &&
                        dev_->backend() == rhi::Backend::D3D12 && dev_->camera(vp, nullptr, eye);
    // Every draw whose material asks to be a light.
    u32 flagged = 0;
    // Cutoff below display precision: kLocalLightRangeCutoff / pi ~ 3e-4 of lamp radiance.
    constexpr f32 kLocalLightRangeCutoff = 0.001f;
    // submit() listed the light-flagged draws; with none this loop does not run.
    if (wanted) {
        for (const u32 lampIndex : lampDrawsPrev_) {
            const Draw& d = drawsPrev_[lampIndex];
            f32 intensity = 0.0f;
            std::memcpy(&intensity, d.mat + offsetof(pbr::MaterialConstants, lightIntensity),
                        sizeof(intensity));
            if (!(intensity > 0.0f) || !std::isfinite(intensity)) continue;   // also rejects NaN
            ++flagged;
            if (d.boundsRadius < 0.0f) continue;
            if (!materials_.ownsBindingSet(d.matSet) || d.matSet == materials_.fallbackBindingSet())
                continue;
            pbr::MaterialConstants mc;
            std::memcpy(&mc, d.mat, sizeof(mc));

            f32 col[3] = {std::max(mc.emissiveFactor[0], 0.0f), std::max(mc.emissiveFactor[1], 0.0f),
                          std::max(mc.emissiveFactor[2], 0.0f)};
            const f32 peak = std::max(col[0], std::max(col[1], col[2]));
            // L is the glow's own radiance; peak <= 0 falls back to white.
            f32 L;
            if (peak > 0.0f && std::isfinite(peak)) {
                L = peak;
                for (f32& c : col) c /= peak;
            } else {
                L = 1.0f;
                col[0] = col[1] = col[2] = 1.0f;
            }
            const f32 maxColour = std::max(col[0], std::max(col[1], col[2]));   // always 1, post-normalise
            const f32 radius = std::max(d.boundsRadius, 1.0f);
            // E1m: irradiance at 1 metre for a sphere of radiance L and this radius.
            const f32 E1m = kPi * L * (radius * radius) * 1e-4f;
            const f32 output = E1m * intensity;
            // min(max(...)), HLSL clamp's order: a sphere wider than 12.5 m still gets 50 m, not more.
            const f32 range = std::min(std::max(100.0f * std::sqrt(output * maxColour / kLocalLightRangeCutoff),
                                                radius * 4.0f),
                                       5000.0f);

            RdLocalLightCand c{};
            c.fromDraw = true;
            c.light.axisKind[2] = 1.0f;   // kind 0 (sphere); the axis is unused
            c.light.shape[2] = c.light.shape[3] = -1.0f;   // no IES, no cookie
            c.light.right[0] = 1.0f;
            c.light.posRadius[0] = d.boundsCentre[0];
            c.light.posRadius[1] = d.boundsCentre[1];
            c.light.posRadius[2] = d.boundsCentre[2];
            c.light.posRadius[3] = radius;
            c.light.radianceRange[0] = col[0] * output;
            c.light.radianceRange[1] = col[1] * output;
            c.light.radianceRange[2] = col[2] * output;
            c.light.radianceRange[3] = range;
            const f32 dx = (d.boundsCentre[0] - eye[0]) * 0.01f;   // cm -> m
            const f32 dy = (d.boundsCentre[1] - eye[1]) * 0.01f;
            const f32 dz = (d.boundsCentre[2] - eye[2]) * 0.01f;
            c.importance = output / std::max(dx * dx + dy * dy + dz * dz, 1.0f);
            rdLocalLightCand_.push_back(c);
        }
        appendSceneLightCandidates(eye);
    }

    // Byte order of the light itself: a total order independent of where a draw sat in the list.
    auto canonicalLess = [](const RdLocalLight& a, const RdLocalLight& b) {
        return std::memcmp(&a, &b, sizeof(RdLocalLight)) < 0;
    };
    // EVERY EMITTER IS A LIGHT (docs/rendering/UNIFIED_LIGHTS.md phase 3): no 32 cap. Ordered by importance from the
    // camera (ties canonical, so draw order never decides); the first kMaxLocalLights are the raster path's working
    // set (rdLocalLightCount_). A light grid (below) bounds what any one point loops over.
    if (rdLocalLightCand_.size() > kMaxListLights) {
        std::partial_sort(rdLocalLightCand_.begin(), rdLocalLightCand_.begin() + kMaxListLights, rdLocalLightCand_.end(),
                          [&](const RdLocalLightCand& a, const RdLocalLightCand& b) {
                              if (a.importance != b.importance) return a.importance > b.importance;
                              return canonicalLess(a.light, b.light);
                          });
        rdLocalLightCand_.resize(kMaxListLights);
    } else {
        std::sort(rdLocalLightCand_.begin(), rdLocalLightCand_.end(),
                  [&](const RdLocalLightCand& a, const RdLocalLightCand& b) {
                      if (a.importance != b.importance) return a.importance > b.importance;
                      return canonicalLess(a.light, b.light);
                  });
    }
    rdLocalLightData_.reserve(rdLocalLightCand_.size() + 1);
    for (const RdLocalLightCand& c : rdLocalLightCand_) rdLocalLightData_.push_back(c.light);
    rdLocalLampCount_ = static_cast<u32>(rdLocalLightData_.size());
    // THE SUN IS AN ENTRY LIKE ANY OTHER: a directional light after the lamps whenever ray tracing runs. Not at night
    // (no radiance), as rdSunLit() never was.
    if (rtActive_ && dev_ && dev_->backend() == rhi::Backend::D3D12) {
        f32 rad[3] = {};
        if (dev_->sunRadianceLinear(rad) && (rad[0] > 0.0f || rad[1] > 0.0f || rad[2] > 0.0f)) {
            const rhi::SkyAtmosphere sky = dev_->skyAtmosphere();
            RdLocalLight sun{};
            for (int a = 0; a < 3; ++a) {
                sun.axisKind[a] = sky.sunDirection[a];
                sun.radianceRange[a] = rad[a];
            }
            sun.axisKind[3] = static_cast<f32>(kLightKindDirectional);
            sun.posRadius[3] = std::tan(sky.sunAngularDiameterDeg * 0.5f * 0.017453292f);
            sun.shape[2] = sun.shape[3] = -1.0f;   // no IES, no cookie
            sun.right[0] = 1.0f;
            rdLocalLightData_.push_back(sun);
        }
    }
    const u32 lightCount = static_cast<u32>(rdLocalLightData_.size());

    // ---- THE LIGHT GRID, stored after the lights in the same buffer (t18) as floats, no new binding ----
    // Header record: posRadius = grid min (cm) + cell size, radianceRange = dims + cell-table base record,
    // axisKind = up to three directional (global) light indices + their count, shape.x = index-pool base record.
    // Cell table: per cell (offset into the pool, count), two floats. Pool: light indices as floats. Each cell keeps
    // its kMaxLightsPerCell most important lights (irradiance at the cell centre); directional lights reach every
    // point and are not in the cells. Indices and offsets are exact in floats (well below 2^24).
    cb_.decalParams[1] = 0.0f;
    if (lightCount > 0) {
        std::vector<u32> globals;
        f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (u32 i = 0; i < lightCount; ++i) {
            const RdLocalLight& l = rdLocalLightData_[i];
            if (static_cast<u32>(l.axisKind[3] + 0.5f) % 8u == kLightKindDirectional) {
                if (globals.size() < 3) globals.push_back(i);
                continue;
            }
            for (int a = 0; a < 3; ++a) {
                lo[a] = std::min(lo[a], l.posRadius[a] - l.radianceRange[3]);
                hi[a] = std::max(hi[a], l.posRadius[a] + l.radianceRange[3]);
            }
        }
        // Bounded around the camera: a light influence beyond kLightGridHalfExtent is not worth cells.
        for (int a = 0; a < 3; ++a) {
            lo[a] = std::max(lo[a], eye[a] - kLightGridHalfExtent);
            hi[a] = std::min(hi[a], eye[a] + kLightGridHalfExtent);
        }
        u32 dim[3] = {1, 1, 1};
        f32 cell = 100.0f;
        const bool anyLocal = lo[0] < hi[0] && lo[1] < hi[1] && lo[2] < hi[2];
        if (anyLocal) {
            const f32 ext = std::max(hi[0] - lo[0], std::max(hi[1] - lo[1], hi[2] - lo[2]));
            cell = std::max(ext / static_cast<f32>(kLightGridMaxDim), 50.0f);
            for (int a = 0; a < 3; ++a)
                dim[a] = std::clamp(static_cast<u32>(std::ceil((hi[a] - lo[a]) / cell)), 1u, kLightGridMaxDim);
        } else {
            lo[0] = lo[1] = lo[2] = 0.0f;
        }
        const u32 cells = dim[0] * dim[1] * dim[2];
        constexpr u32 kF = sizeof(RdLocalLight) / sizeof(f32);   // 20 floats a record
        const u32 headerRec = lightCount;
        // The grid depends only on the ordered non-directional lights, its box and the globals: a still camera (or a
        // clamp that does not bind) reuses last frame's records. The sun moving does not invalidate it.
        u64 key = 1469598103934665603ull;
        auto mix = [&key](u64 v) { key = (key ^ v) * 1099511628211ull; key ^= key >> 29; };
        for (u32 i = 0; i < lightCount; ++i) {
            const RdLocalLight& l = rdLocalLightData_[i];
            if (static_cast<u32>(l.axisKind[3] + 0.5f) % 8u == kLightKindDirectional) continue;
            u64 w[sizeof(RdLocalLight) / 8];
            std::memcpy(w, &l, sizeof(w));
            for (const u64 x : w) mix(x);
        }
        for (int a = 0; a < 3; ++a) {
            u32 b;
            std::memcpy(&b, &lo[a], 4); mix(b);
            std::memcpy(&b, &hi[a], 4); mix(b);
            mix(dim[a]);
        }
        mix(anyLocal ? 1u : 0u);
        mix(lightCount);
        for (const u32 g : globals) mix(g);
        if (rdGridCacheValid_ && rdGridCacheKey_ == key) {
            rdLocalLightData_.insert(rdLocalLightData_.end(), rdGridCache_.begin(), rdGridCache_.end());
            cb_.decalParams[1] = static_cast<f32>(headerRec);
        } else {
        // Counting-sort layout, scratch reused across frames (no per-cell vectors).
        thread_local std::vector<u32> start, cursor, boxes;
        thread_local std::vector<std::pair<f32, u32>> entries;
        start.assign(static_cast<usize>(anyLocal ? cells : 0u) + 1, 0u);
        boxes.clear();
        entries.clear();
        auto cellImp = [&](const RdLocalLight& l, u32 x, u32 y, u32 z) {
            const f32 lum = 0.2126f * l.radianceRange[0] + 0.7152f * l.radianceRange[1] + 0.0722f * l.radianceRange[2];
            const f32 r2 = std::max(l.posRadius[3] * l.posRadius[3], 1.0f);
            const f32 cx = lo[0] + (x + 0.5f) * cell - l.posRadius[0];
            const f32 cy = lo[1] + (y + 0.5f) * cell - l.posRadius[1];
            const f32 cz = lo[2] + (z + 0.5f) * cell - l.posRadius[2];
            // Distance to the cell's nearest point, so a light inside the cell ranks first.
            const f32 h = cell * 0.866f;
            const f32 d = std::max(std::sqrt(cx * cx + cy * cy + cz * cz) - h, 0.0f);
            return lum * 1e4f / std::max(d * d, r2);
        };
        if (anyLocal) {
            boxes.assign(static_cast<usize>(lightCount) * 6, 0u);   // c0[3], c1[3]; c0 > c1 = not in the grid
            for (u32 i = 0; i < lightCount; ++i) {
                const RdLocalLight& l = rdLocalLightData_[i];
                u32* bx = &boxes[static_cast<usize>(i) * 6];
                bx[0] = bx[1] = bx[2] = 1u; bx[3] = bx[4] = bx[5] = 0u;
                if (static_cast<u32>(l.axisKind[3] + 0.5f) % 8u == kLightKindDirectional) continue;
                const f32 range = l.radianceRange[3];
                u32 c0[3], c1[3];
                bool inside = true;
                for (int a = 0; a < 3; ++a) {
                    const f32 a0 = (l.posRadius[a] - range - lo[a]) / cell, a1 = (l.posRadius[a] + range - lo[a]) / cell;
                    if (a1 < 0.0f || a0 >= static_cast<f32>(dim[a])) { inside = false; break; }
                    c0[a] = static_cast<u32>(std::max(a0, 0.0f));
                    c1[a] = std::min(static_cast<u32>(std::max(a1, 0.0f)), dim[a] - 1);
                }
                if (!inside) continue;
                for (int a = 0; a < 3; ++a) { bx[a] = c0[a]; bx[3 + a] = c1[a]; }
                for (u32 z = c0[2]; z <= c1[2]; ++z)
                    for (u32 y = c0[1]; y <= c1[1]; ++y)
                        for (u32 x = c0[0]; x <= c1[0]; ++x) ++start[((z * dim[1] + y) * dim[0] + x) + 1];
            }
            for (u32 c = 0; c < cells; ++c) start[c + 1] += start[c];
            entries.resize(start[cells]);
            cursor.assign(start.begin(), start.end() - 1);
            for (u32 i = 0; i < lightCount; ++i) {
                const u32* bx = &boxes[static_cast<usize>(i) * 6];
                if (bx[0] > bx[3] || bx[1] > bx[4] || bx[2] > bx[5]) continue;
                const RdLocalLight& l = rdLocalLightData_[i];
                for (u32 z = bx[2]; z <= bx[5]; ++z)
                    for (u32 y = bx[1]; y <= bx[4]; ++y)
                        for (u32 x = bx[0]; x <= bx[3]; ++x)
                            entries[cursor[(z * dim[1] + y) * dim[0] + x]++] = {cellImp(l, x, y, z), i};
            }
        }
        // Encode.
        std::vector<f32> table(static_cast<usize>(cells) * 2, 0.0f), pool;
        pool.reserve(entries.size());
        for (u32 c = 0; c < (anyLocal ? cells : 0u); ++c) {
            auto* b = entries.data() + start[c];
            u32 n = start[c + 1] - start[c];
            if (n > kMaxLightsPerCell) {
                std::partial_sort(b, b + kMaxLightsPerCell, b + n,
                                  [](const auto& a, const auto& b2) { return a.first != b2.first ? a.first > b2.first : a.second < b2.second; });
                n = kMaxLightsPerCell;
            }
            table[c * 2] = static_cast<f32>(pool.size());
            table[c * 2 + 1] = static_cast<f32>(n);
            for (u32 k = 0; k < n; ++k) pool.push_back(static_cast<f32>(b[k].second));
        }
        const u32 tableRec  = headerRec + 1;
        const u32 tableRecs = static_cast<u32>((table.size() + kF - 1) / kF);
        const u32 poolRec   = tableRec + tableRecs;
        const u32 poolRecs  = static_cast<u32>((pool.size() + kF - 1) / kF);
        RdLocalLight header{};
        for (int a = 0; a < 3; ++a) header.posRadius[a] = lo[a];
        header.posRadius[3] = cell;
        for (int a = 0; a < 3; ++a) header.radianceRange[a] = static_cast<f32>(anyLocal ? dim[a] : 0u);
        header.radianceRange[3] = static_cast<f32>(tableRec);
        for (u32 g = 0; g < 3; ++g) header.axisKind[g] = g < globals.size() ? static_cast<f32>(globals[g]) : -1.0f;
        header.axisKind[3] = static_cast<f32>(globals.size());
        header.shape[0] = static_cast<f32>(poolRec);
        rdLocalLightData_.push_back(header);
        const usize base = rdLocalLightData_.size();
        rdLocalLightData_.resize(base + tableRecs + poolRecs);
        f32* raw = reinterpret_cast<f32*>(rdLocalLightData_.data());
        std::memset(raw + base * kF, 0, (tableRecs + poolRecs) * sizeof(RdLocalLight));
        std::memcpy(raw + static_cast<usize>(tableRec) * kF, table.data(), table.size() * sizeof(f32));
        std::memcpy(raw + static_cast<usize>(poolRec) * kF, pool.data(), pool.size() * sizeof(f32));
        cb_.decalParams[1] = static_cast<f32>(headerRec);
        rdGridCache_.assign(rdLocalLightData_.begin() + headerRec, rdLocalLightData_.end());
        rdGridCacheKey_ = key;
        rdGridCacheValid_ = true;
        }
        rdGridKeyNow_ = key;
    }

    if (!rdLocalLightData_.empty()) {
        // Grown when the list outgrows it, with headroom, never shrunk.
        if (rdLocalLightCapacity_ < rdLocalLightData_.size()) {
            // Every slot or none: a half-built ring would bind a null buffer on its missing turns.
            const u32 want = static_cast<u32>(rdLocalLightData_.size() * 3 / 2 + 64);
            bool ok = true;
            for (u32 i = 0; i < kRtInstanceRing; ++i) {
                if (rdLocalLights_[i]) res_->destroyBuffer(rdLocalLights_[i]);
                rhi::BufferDesc bd;
                bd.bytes = sizeof(RdLocalLight) * want;
                bd.kind  = rhi::BufferKind::Upload;
                bd.debugName = "Voxi light list + grid";
                rdLocalLights_[i] = res_->createBuffer(bd);
                ok = ok && rdLocalLights_[i] != 0;
            }
            rdLocalLightCapacity_ = ok ? want : 0;
            for (u64& k : rdGridSlotKey_) k = 0;
        }
        if (rdLocalLightCapacity_ >= rdLocalLightData_.size()) {
            // Rotate BEFORE writing, so this frame never touches the buffer the previous one bound.
            rdLocalLightSlot_ = (rdLocalLightSlot_ + 1) % kRtInstanceRing;
            const rhi::BufferHandle buf = rdLocalLights_[rdLocalLightSlot_];
            const u32 records = static_cast<u32>(rdLocalLightData_.size());
            // The grid tail (up to ~3 MB) is rewritten only when this ring slot does not already hold it.
            u32 writeRecords = records;
            if (rdGridKeyNow_ != 0 && cb_.decalParams[1] > 0.5f) {
                const u32 headRecords = static_cast<u32>(cb_.decalParams[1]);
                if (rdGridSlotKey_[rdLocalLightSlot_] == rdGridKeyNow_) writeRecords = headRecords;
                else rdGridSlotKey_[rdLocalLightSlot_] = rdGridKeyNow_;
            }
            res_->writeBuffer(buf, rdLocalLightData_.data(), sizeof(RdLocalLight) * writeRecords, 0);
            res_->setSrvBuffer(bindings_, 18, buf, sizeof(RdLocalLight), records, 0);
            rdLocalLightsBound_ = buf;
            // The raster path's working set: the first lamps (the list is importance-ordered), never the sun.
            rdLocalLightCount_ = std::min(rdLocalLampCount_, kMaxLocalLights);
            // Lamp visibility history is valid while the SET of lamps is the same: an order-independent sum of
            // per-lamp hashes, since the list is ordered by importance from the camera and reorders as it moves (an
            // ordered hash threw the history away on every camera move). The sun is left out: it moves every frame.
            u64 h = 0;
            const u8* bytes = reinterpret_cast<const u8*>(rdLocalLightData_.data());
            for (u32 l = 0; l < rdLocalLampCount_; ++l) {
                u64 one = 1469598103934665603ull;
                for (usize i = 0; i < sizeof(RdLocalLight); i += 8) {
                    u64 w;
                    std::memcpy(&w, bytes + l * sizeof(RdLocalLight) + i, 8);
                    one = (one ^ w) * 1099511628211ull;
                    one ^= one >> 29;
                }
                h += one;
            }
            h ^= rdLocalLampCount_; h *= 1099511628211ull;
            rdLocalLightHash_ = h;
        } else {
            cb_.decalParams[1] = 0.0f;
            if (!rdLocalLightsFailLogged_) {
                rdLocalLightsFailLogged_ = true;
                AVER_ERROR("[Voxi] light list buffer could not be created; lamps stay unlit (said once)");
            }
        }
    }
    // Scene lights have no emissive geometry to drop, so only the lamp-flagged draws count here.
    u32 drawLamps = 0;
    for (const RdLocalLightCand& c : rdLocalLightCand_) drawLamps += c.fromDraw ? 1u : 0u;
    // Every flagged lamp is a list entry (no cap below kMaxListLights), so GI may drop an emitter's own emission.
    const bool listed = cb_.decalParams[1] > 0.5f;
    rdLocalLightsCarryAll_ = listed && rdLocalLampCount_ > 0 && drawLamps > 0 && drawLamps == flagged;
    // Empty (or the ring failed): the placeholder, rebound only when t18 names something else.
    if (!listed && rdLocalLightsPlaceholder_ && rdLocalLightsBound_ != rdLocalLightsPlaceholder_) {
        res_->setSrvBuffer(bindings_, 18, rdLocalLightsPlaceholder_, sizeof(RdLocalLight), 1, 0);
        rdLocalLightsBound_ = rdLocalLightsPlaceholder_;
    }
}

// Local lights (lamps): see the header comment. History trusted only when a scene pass wrote it LAST
// frame (rdLocalHistFrame_) under the SAME light list (rdLocalHistHash_) and the shadow history's
// reprojection is usable this frame (rtHistParams.y > 0; its 0.5 "sun moved" state is still usable
// here since lamps don't depend on the sun). Which mode wrote it doesn't matter: all three write the
// same quantity into the same pair.
bool VoxiRenderer::publishLocalLights(bool live, const char* pass) {
    // The list is published whatever the lamp pass does: it carries the sun, and secondary hits trace their own
    // shadows. `live` (bit 4) says the visible-surface lamp visibility for this pass exists.
    cb_.cameraMedium[2] = static_cast<f32>(rdLocalLightCount_);
    if (!live) {
        cb_.cameraMedium[3] = rdLocalLightsCarryAll_ ? 2.0f : 0.0f;
        return false;
    }
    const bool histValid = cb_.rtHistParams[1] > 0.25f && rdLocalHistFrame_ != 0 &&
                           rdLocalHistFrame_ + 1u == rtFrameIndex_ &&
                           rdLocalHistHash_ == rdLocalLightHash_;
    cb_.cameraMedium[2] = static_cast<f32>(rdLocalLightCount_);
    // Two bits: 1 = history valid; 2 = every lamp-flagged draw is a live light this frame, so the GI
    // estimators may drop an emitter's own emission (rdLocalLightsCarryAll_).
    // Bit 8: last frame's CSRdShadow wrote the light keys in u19 (the sun's shadow history is reused only under them).
    const bool keysValid = rdKeyFrame_ != 0 && rdKeyFrame_ + 1u == rtFrameIndex_;
    cb_.cameraMedium[3] = (histValid ? 1.0f : 0.0f) + (rdLocalLightsCarryAll_ ? 2.0f : 0.0f) + 4.0f +
                          (keysValid ? 8.0f : 0.0f);
    if (!rdLocalLightsRunLogged_) {
        rdLocalLightsRunLogged_ = true;
        AVER_INFO("[Voxi] local lights running: {} lamp(s) this frame, shaded by {}",
                  rdLocalLampCount_, pass);
    }
    return true;
}

VoxiRenderer::ShadowInstanceGroup& VoxiRenderer::shadowGroupFor(std::vector<ShadowInstanceGroup>& groups,
                                                                std::unordered_map<rhi::MeshHandle, u32>& index,
                                                                rhi::MeshHandle mesh) {
    const auto [it, added] = index.try_emplace(mesh, static_cast<u32>(groups.size()));
    if (added) groups.push_back({mesh, {}});
    return groups[it->second];
}

// Renders the replayed draw list into each cascade's quadrant of the shadow atlas, depth only.
// Instanced by default when shadowInstancedPso_ built: every surviving draw is grouped by mesh into
// shadowInstanceGroups_, one drawMeshInstanced call per group instead of one drawMesh per draw.
// Depth-only with a strict Less test is order-independent (except two triangles at exactly the same
// depth), so regrouping by mesh gives the identical shadow map for a fraction of the work -- up to
// ~2,114 drawMesh calls per cascade collapse to at most ~30 (this scene's distinct mesh count). Falls
// back to one-draw-per-instance (shadowPso_) if the instanced pipeline failed to build.
void VoxiRenderer::shadowPass(rhi::IRenderContext& ctx) {
    const bool useInstancing = shadowInstancedPso_ != 0;
    if ((!shadowPso_ && !shadowInstancedPso_) || drawsPrev_.empty()) { cb_.shadowParams[1] = 0.0f; return; }
    u32 cascades = fitCascades();
#ifdef AVER_VOXI_SHADOW_CASCADE_LIMIT
    if (cascades > static_cast<u32>(AVER_VOXI_SHADOW_CASCADE_LIMIT))
        cascades = static_cast<u32>(AVER_VOXI_SHADOW_CASCADE_LIMIT);
#endif
    if (cascades == 0) { cb_.shadowParams[1] = 0.0f; return; }

    // Atlas has no reader while ray tracing is on; shadowParams[1] = 0 prevents stale atlas reads.
    if (rtActive_) { cb_.shadowParams[1] = 0.0f; return; }

    cb_.shadowParams[0] = 1.0f / static_cast<f32>(kShadowSize);
    cb_.shadowParams[1] = 1.0f;
    cb_.shadowParams[3] = static_cast<f32>(cascades);

    rhi::ScopedGpuStat gpuStat(ctx, "Voxi shadow");
    ctx.textureBarrier(shadowTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::DepthWrite);
    ctx.setPipeline(useInstancing ? shadowInstancedPso_ : shadowPso_);
    ctx.setBindingSet(bindings_);       // Tier 1: bind every declared table, read or not
    ctx.setRenderTargets(nullptr, 0, shadowTex_);
    ctx.clearDepth(shadowTex_, 1.0f);   // the whole atlas, once, before any quadrant is drawn

    // Depth-only, no pixel shader bound in either pipeline: material CONTENT is never read, so one
    // binding for the whole pass satisfies Tier 1's "table 1 must hold something" requirement for
    // every draw regardless of which draw's own material it actually names.
    ctx.setDrawBinding(materials_.fallbackBindingSet(), &materials_.fallbackConstants(), sizeof(pbr::MaterialConstants));

    static bool sShadowCensusLogged = false;
    u32 censusPerCascade[kShadowCascades] = {};

    for (u32 c = 0; c < cascades; ++c) {
        cb_.shadowDraw[0] = static_cast<f32>(c);
        ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));

        const u32 qx = (c & 1u) * kShadowCascadeSize;
        const u32 qy = (c >> 1) * kShadowCascadeSize;
        // Viewport and scissor both: the scissor confines the cascade to its own quadrant.
        ctx.setViewport(qx, qy, kShadowCascadeSize, kShadowCascadeSize);
        ctx.setScissor(qx, qy, kShadowCascadeSize, kShadowCascadeSize);

        const Vec3 cascCentre{cascadeCentre_[c][0], cascadeCentre_[c][1], cascadeCentre_[c][2]};
        const f32 cascRadius = cascadeRadius_[c];

        // Each cascade resolves differently: far cascades have metre-wide texels, near ones centimetres.
        const f32 cascTexelWorld = 2.0f * cascRadius / static_cast<f32>(kShadowCascadeSize);
        const f32 minShadowDiameter = cascTexelWorld * kMinShadowTexels;

        u32 submitted = 0;
        if (useInstancing) {
            // Reset capacity, not the group list itself: the mesh set repeats cascade to cascade and
            // frame to frame, so keeping each group's (now empty) vector alive means only the FIRST
            // frame's cascades pay for growth -- see shadowInstanceGroups_'s own comment.
            for (ShadowInstanceGroup& g : shadowInstanceGroups_) g.worlds.clear();

            for (const Draw& d : drawsPrev_) {
                // TRANSLUCENT DRAWS ARE NOT IN THIS DEPTH-ONLY PASS: it binds no pixel shader and a
                // depth map has no channel for a transmittance. They go to the TLAS instead, where a
                // shadow ray can attenuate through them. See submitDraw.
                if (d.translucent) continue;
                // Sphere-sphere overlap against this cascade's own fitted bounds: a draw whose
                // bounding sphere cannot reach the cascade's box cannot cast a shadow into it. A
                // negative boundsRadius means the backend had no bounds for this mesh -- draw it
                // regardless rather than guess.
                if (d.boundsRadius >= 0.0f) {
                    const Vec3 dc{d.boundsCentre[0], d.boundsCentre[1], d.boundsCentre[2]};
                    if (dist(dc, cascCentre) > cascRadius + d.boundsRadius) continue;
                    if (2.0f * d.boundsRadius < minShadowDiameter) continue;
                }
                ShadowInstanceGroup& group = shadowGroupFor(shadowInstanceGroups_, shadowGroupIndex_, d.depthMesh);
                const usize base = group.worlds.size();
                group.worlds.resize(base + 16);
                std::memcpy(group.worlds.data() + base, d.world, 16 * sizeof(f32));
                ++submitted;
            }
            for (const ShadowInstanceGroup& g : shadowInstanceGroups_) {
                if (g.worlds.empty()) continue;
                ctx.drawMeshInstanced(g.mesh, g.worlds.data(), static_cast<u32>(g.worlds.size() / 16));
            }
        } else {
            for (const Draw& d : drawsPrev_) {
                // Translucent draws excluded (depth-only pass) -- see the note above, first branch.
                if (d.translucent) continue;
                if (d.boundsRadius >= 0.0f) {
                    const Vec3 dc{d.boundsCentre[0], d.boundsCentre[1], d.boundsCentre[2]};
                    if (dist(dc, cascCentre) > cascRadius + d.boundsRadius) continue;
                    if (2.0f * d.boundsRadius < minShadowDiameter) continue;
                }
                f32 consts[rhi::kObjectConstantDwords]{};
                std::memcpy(consts, d.world, 16 * sizeof(f32));   // depth-only: nothing else is read
                ctx.setConstants(rhi::kObjectConstantRegister, consts, rhi::kObjectConstantDwords);
                ctx.drawMesh(d.depthMesh);
                ++submitted;
            }
        }
        censusPerCascade[c] = submitted;
    }

    if (!sShadowCensusLogged) {
        u32 total = 0;
        for (u32 c = 0; c < kShadowCascades; ++c) total += censusPerCascade[c];
        AVER_INFO("[Voxi] shadowPass census (once): {} draws available this frame, {} cascade(s) run, "
                  "per-cascade draws submitted = [{}, {}, {}, {}], total submitted = {}",
                  static_cast<u32>(drawsPrev_.size()), cascades,
                  censusPerCascade[0], censusPerCascade[1], censusPerCascade[2], censusPerCascade[3],
                  total);
        sShadowCensusLogged = true;
    }

    ctx.textureBarrier(shadowTex_, rhi::ResourceState::DepthWrite, rhi::ResourceState::ShaderResource);
}

// Renders the GI-only shadow map: one box over the GI volume, for light injection alone.
void VoxiRenderer::giShadowPass(rhi::IRenderContext& ctx, usize begin, usize end, bool first, bool last) {
    (void)last;
    const bool useInstancing = giShadowInstancedPso_ != 0;
    end = std::min(end, drawsPrev_.size());
    if ((!giShadowPso_ && !giShadowInstancedPso_) || drawsPrev_.empty()) {
        cb_.giShadowParams[1] = 0.0f;   // unusable: giShadowFactor falls back to fully lit
        return;
    }
    fitGiShadow();                       // sets giShadowParams[1] = 1 and the cull sphere

    rhi::ScopedGpuStat gpuStat(ctx, "Voxi GI shadow");
    ctx.textureBarrier(giShadowTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::DepthWrite);
    ctx.setPipeline(useInstancing ? giShadowInstancedPso_ : giShadowPso_);
    ctx.setBindingSet(bindings_);        // Tier 1: bind every declared table, read or not
    ctx.setRenderTargets(nullptr, 0, giShadowTex_);
    if (first) ctx.clearDepth(giShadowTex_, 1.0f);
    ctx.setViewport(0, 0, kGiShadowSize, kGiShadowSize);
    ctx.setScissor(0, 0, kGiShadowSize, kGiShadowSize);
    // Depth-only, no pixel shader: material content is never read, so one binding satisfies Tier 1
    // for every draw -- the same reasoning shadowPass states above.
    ctx.setDrawBinding(materials_.fallbackBindingSet(), &materials_.fallbackConstants(),
                       sizeof(pbr::MaterialConstants));
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));

    const Vec3 giCentre{giShadowCentre_[0], giShadowCentre_[1], giShadowCentre_[2]};

    u32 submitted = 0;
    if (useInstancing) {
        for (ShadowInstanceGroup& g : giShadowInstanceGroups_) g.worlds.clear();
        for (usize di = begin; di < end; ++di) {
            const Draw& d = drawsPrev_[di];
            // Translucent draws excluded (depth-only pass) -- see shadowPass's note on the same skip.
            // Movable ones too: this map lights the bake, and a shadow baked from something that
            // keeps moving would stay where it was when the volume was last built.
            if (d.translucent || d.movable) continue;
            // Culled against the VOLUME's sphere, never cascadeCentre_[3]/cascadeRadius_[3] -- those
            // are camera-fitted now and have nothing to do with what the volume covers.
            if (d.boundsRadius >= 0.0f && dist(Vec3{d.boundsCentre[0], d.boundsCentre[1], d.boundsCentre[2]},
                                                giCentre) > giShadowRadius_ + d.boundsRadius) continue;
            ShadowInstanceGroup& group = shadowGroupFor(giShadowInstanceGroups_, giShadowGroupIndex_, d.depthMesh);
            const usize base = group.worlds.size();
            group.worlds.resize(base + 16);
            std::memcpy(group.worlds.data() + base, d.world, 16 * sizeof(f32));
            ++submitted;
        }
        for (const ShadowInstanceGroup& g : giShadowInstanceGroups_) {
            if (g.worlds.empty()) continue;
            ctx.drawMeshInstanced(g.mesh, g.worlds.data(), static_cast<u32>(g.worlds.size() / 16));
        }
    } else {
        for (usize di = begin; di < end; ++di) {
            const Draw& d = drawsPrev_[di];
            // Translucent and movable draws excluded, as in the instanced loop above.
            if (d.translucent || d.movable) continue;
            if (d.boundsRadius >= 0.0f && dist(Vec3{d.boundsCentre[0], d.boundsCentre[1], d.boundsCentre[2]},
                                                giCentre) > giShadowRadius_ + d.boundsRadius) continue;
            f32 consts[rhi::kObjectConstantDwords]{};
            std::memcpy(consts, d.world, 16 * sizeof(f32));
            ctx.setConstants(rhi::kObjectConstantRegister, consts, rhi::kObjectConstantDwords);
            ctx.drawMesh(d.depthMesh);
            ++submitted;
        }
    }

    static bool sGiCensusLogged = false;
    if (!sGiCensusLogged) {
        AVER_INFO("[Voxi] giShadowPass census (once): {} draws available, {} submitted into one "
                  "{}^2 box of radius {:.0f}cm, every {} frame(s)",
                  static_cast<u32>(drawsPrev_.size()), submitted, kGiShadowSize, giShadowRadius_,
                  giUpdateInterval_);
        sGiCensusLogged = true;
    }

    ctx.textureBarrier(giShadowTex_, rhi::ResourceState::DepthWrite, rhi::ResourceState::ShaderResource);
}

u64 VoxiRenderer::giBuildTriangles(usize begin, usize end, u64 stopAt) const {
    u64 tris = 0;
    end = std::min(end, drawsPrev_.size());
    for (usize i = begin; i < end && tris <= stopAt; ++i) {
        const Draw& d = drawsPrev_[i];
        if (d.translucent || d.movable || !dev_ || dev_->meshVertexBuffer(d.mesh)) continue;
        u32 indices = 0;
        if (dev_->meshGeometry(d.depthMesh, nullptr, nullptr, nullptr, &indices)) tris += indices / 3u;
    }
    return tris;
}

usize VoxiRenderer::giBuildSliceEnd(usize begin) const {
    const usize n = drawsPrev_.size();
    u64 tris = 0;
    usize i = begin;
    while (i < n) {
        tris += giBuildTriangles(i, i + 1, ~0ull);
        ++i;
        if (tris >= kGiBuildTrisPerFrame) break;
    }
    return i;
}

// One slice of a staged GI rebuild (see GiBuild): the GI shadow map first, then the injection, then
// the resolve and mips on the last slice. Restarts when the volume changes under it.
void VoxiRenderer::giBuildStep(rhi::IRenderContext& ctx) {
    GiBuild& b = giBuild_;
    const usize n = drawsPrev_.size();
    if (!voxelAccumTex_ || (b.res && b.res != voxelResBuilt_)) {
        b = GiBuild{};
        giConvergeTicks_ = std::max(giConvergeTicks_, 1u);   // the gate rebuilds on the next tick
        return;
    }
    b.res = voxelResBuilt_;
    ++b.frames;
    b.cursor = std::min(b.cursor, n);
    const usize end = giBuildSliceEnd(b.cursor);
    const bool first = b.cursor == 0, last = end >= n;
    if (b.phase == 0) {
        giShadowPass(ctx, b.cursor, end, first, last);
        b.cursor = end;
        if (last) {
            b.phase = 1;
            b.cursor = 0;
            if (b.tryCache && giCacheRestore(ctx)) b = GiBuild{};   // a cached volume replaces the injection
        }
        return;
    }
    voxelizePass(ctx, b.cursor, end, first, last);
    b.cursor = end;
    if (!last) return;
    filterMips(ctx);
    if (b.settleCloudOnly)
        giCacheSettleCloudOnly_ = (giCacheSettlePending_ ? giCacheSettleCloudOnly_ : true) && giRebuildCloudOnly_;
    giCacheSettlePending_ = true;
    if (!giBuildLogged_) {
        giBuildLogged_ = true;
        AVER_INFO("[Voxi] GI rebuild staged over {} frame(s) ({} draws, at most {} triangles a frame) so no "
                  "single GPU submission runs long (said once)", b.frames, n, kGiBuildTrisPerFrame);
    }
    b = GiBuild{};
}

// Clears the accumulator, rasterises the scene into it with direct lighting applied, then resolves
// it into mip 0 of the radiance volume.
void VoxiRenderer::voxelizePass(rhi::IRenderContext& ctx, usize begin, usize end, bool first, bool last) {
    end = std::min(end, drawsPrev_.size());
    if (!voxelAccumTex_) {
        static bool sNoAccumWarned = false;
        if (!sNoAccumWarned) {
            sNoAccumWarned = true;
            AVER_WARN("[Voxi] voxelizePass skipped: no injection accumulator this tick "
                      "(should be unreachable -- see the W12 gate in prePass)");
        }
        return;
    }
    const u32 res = voxelResBuilt_;
    rhi::ScopedGpuStat gpuStat(ctx, "Voxi voxelise");

    // ---- W3 pre-pass: which part of the grid THIS rebuild's injection can possibly touch ----
    // Over every draw, on the rebuild's first slice; later slices of a staged rebuild reuse it.
    VoxelBox drawsBox = giBuildDrawsBox_;
    bool anyUnbounded = giBuildAnyUnbounded_;
    if (first) {
        drawsBox = VoxelBox{};
        anyUnbounded = false;
        const f32 origin[3] = {center_[0] - extent_, center_[1] - extent_, center_[2] - extent_};
        for (const Draw& d : drawsPrev_) {
            if (d.translucent) continue;
            if (dev_ && dev_->meshVertexBuffer(d.mesh)) continue;
            if (!giVoxelisedDraw(d)) continue;

            f32 lmin[3], lmax[3];
            const bool haveAabb = d.boundsRadius >= 0.0f && dev_ &&
                                  dev_->meshBoundsAabb(d.depthMesh, lmin, lmax);
            if (!haveAabb) { anyUnbounded = true; continue; }

            // Row-major, row-vector -- the identical convention submit() already builds a Mat4 from
            // (see submit()'s own w.m/xformPoint use) -- transforming all eight local AABB corners
            // into world space.
            Mat4 w;
            std::memcpy(&w.m[0][0], d.world, sizeof(w.m));
            f32 wmin[3] = {1e30f, 1e30f, 1e30f}, wmax[3] = {-1e30f, -1e30f, -1e30f};
            for (int c = 0; c < 8; ++c) {
                const Vec3 corner{(c & 1) ? lmax[0] : lmin[0], (c & 2) ? lmax[1] : lmin[1],
                                  (c & 4) ? lmax[2] : lmin[2]};
                const Vec3 wc = xformPoint(corner, w);
                wmin[0] = std::fmin(wmin[0], wc.x); wmax[0] = std::fmax(wmax[0], wc.x);
                wmin[1] = std::fmin(wmin[1], wc.y); wmax[1] = std::fmax(wmax[1], wc.y);
                wmin[2] = std::fmin(wmin[2], wc.z); wmax[2] = std::fmax(wmax[2], wc.z);
            }
            // Pad 2 voxels: conservative raster (vox.conservativeRaster below) can light a voxel a
            // triangle only grazes, just outside the triangle's own tight AABB.
            drawsBox = unionBox(drawsBox, voxelBoxFromWorldAabb(wmin, wmax, origin, extent_ * 2.0f,
                                                                res, 2u));
        }
        giBuildDrawsBox_ = drawsBox;
        giBuildAnyUnbounded_ = anyUnbounded;
    }

    // ---- W3 box selection: bounded, or full, and why ----
    const bool boxMovedOrResized = giBoxRes_ != res ||
        giBoxCentre_[0] != center_[0] || giBoxCentre_[1] != center_[1] || giBoxCentre_[2] != center_[2] ||
        giBoxExtent_ != extent_;
    bool full; const char* giBoxReason;
    if      (!giBoundedDispatch_) { full = true;  giBoxReason = "full: bounded dispatch off"; }
    else if (!giBoxPrevValid_)    { full = true;  giBoxReason = "full: no previous box"; }
    else if (anyUnbounded)        { full = true;  giBoxReason = "full: unbounded draw"; }
    else if (boxMovedOrResized)   { full = true;  giBoxReason = "full: volume moved or resized"; }
    else                           { full = false; giBoxReason = "bounded"; }
    if (first)
        giDispatchBox0_ = full ? fullVoxelBox(res)   // filterMips derives each mip level's box from this
                               : alignOutward(unionBox(drawsBox, giBoxPrevDraws_), 4u, res);
    const VoxelBox box0 = giDispatchBox0_;

    if (first) {
        ctx.setPipeline(clearPso_);
        ctx.setBindingSet(clearBindings_);
        const GiDispatchConstants k = dispatchConstants(box0, 0);
        ctx.setConstants(3, &k, kGiDispatchConstantDwords);
        u32 g[3];
        dispatchGroups(box0, 4u, g);
        if (g[0] && g[1] && g[2]) ctx.dispatch(g[0], g[1], g[2]);
    }
    ctx.uavBarrierTexture(voxelAccumTex_);   // injection must see the cleared accumulator

    // Prefers the mesh-shader voxelise pipeline whenever the device built one, independent of
    // settings_.meshShaders (a wider switch that also moves the main scene's lit draws onto their
    // own mesh-shader pipeline, off by default as a real behaviour change). Voxelisation has no such
    // wrinkle: same `vox` desc, same PSVoxel pixel shader either way, MSVoxel running the identical
    // dominant-axis projection VSVoxel+GSVoxel do (see MSVoxel's comment for the normal-transform bug
    // this depended on fixing first). recordBasePipelines() builds voxelMsPso_ regardless of
    // the setting, so `voxelMsPso_ != 0` alone means the device proved it can do this; GSVoxel is the
    // fallback for no mesh-shader tier.
    const bool useMs = voxelMsPso_ != 0;
    ctx.setPipeline(useMs ? voxelMsPso_ : voxelPso_);
    ctx.setBindingSet(bindings_);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
    ctx.setRenderTargets(nullptr, 0, 0);   // no targets at all: the pixel shader writes only the UAV
    ctx.setViewport(0, 0, res, res);
    ctx.setScissor(0, 0, res, res);

    // CULLED AGAINST THE VOLUME, which this pass did not do at all until now -- it rasterised every
    // draw in drawsPrev_ including ones streaming had brought in kilometres away; those never
    // survived (the pixel shader's UAV write lands outside the 128^3 grid and is dropped) but paid
    // the full vertex/raster cost first. On a streamed scene the volume covers a small fraction of
    // what's resident, so this cull is most of the pass. The test is giVoxelisedDraw's.
    u32 voxelSubmitted = 0, voxelCulled = 0, voxelSkinned = 0, voxelMovable = 0;

    for (usize di = begin; di < end; ++di) {
        const Draw& d = drawsPrev_[di];
        // Translucent draws excluded (depth-only pass) -- see shadowPass's note on the same skip.
        if (d.translucent) continue;
        // A COMPUTE-SKINNED MESH IS EXCLUDED, DELIBERATELY -- a trade, not a fix. giDrawsKey hashes
        // mesh/transform/material, never the vertex buffer a skinning dispatch rewrites every frame,
        // so once a character's TRANSFORM settles the gate reports "unchanged" and its indirect-light
        // contribution freezes at whatever pose the last rebuild saw. Measured on a real rig: 2
        // rebuilt / 62 skipped of 64 ticks straight through a pose transition.
        // The BLAS cache fixes the same defect per-mesh by rebuilding every frame -- NOT available
        // here, since voxelisation is one volume: treating a skinned draw as always-changed would
        // force a full revoxelisation whenever any character is on screen (measured: the ~96% of GI
        // rebuilds this gate normally avoids, on top of the 17.3 ms GI already costs while skipping
        // them). So the interim state is absence, not a silent freeze:
        // a skinned character bounces no indirect light (matches PtSceneView::submitDraw). Real fix
        // is partial revoxelisation, which voxelizePass doesn't support today.
        if (dev_ && dev_->meshVertexBuffer(d.mesh)) { ++voxelSkinned; continue; }
        // Counted apart from the bounds cull for the same reason skinned is: absence, not a cull.
        if (d.movable) { ++voxelMovable; continue; }
        // The bounds half of giVoxelisedDraw, kept inline only for the census counters below -- the
        // predicate is the definition and must not drift from it.
        if (!giVoxelisedDraw(d)) { ++voxelCulled; continue; }
        ++voxelSubmitted;
        f32 consts[rhi::kObjectConstantDwords];
        std::memcpy(consts, d.world, 16 * sizeof(f32));
        std::memcpy(consts + 16, d.color, 4 * sizeof(f32));
        consts[20] = d.metallic; consts[21] = d.roughness; consts[22] = 0.0f; consts[23] = 0.0f;
        writeShadingConstants(consts);
        ctx.setConstants(rhi::kObjectConstantRegister, consts, rhi::kObjectConstantDwords);
        // The material captured at submit time, so the injection shades the same surface the lit
        // pass does.
        if (d.matSet) ctx.setDrawBinding(d.matSet, d.mat, d.matBytes);
        else          ctx.setDrawBinding(materials_.fallbackBindingSet(),
                                         &materials_.fallbackConstants(), sizeof(pbr::MaterialConstants));
        if (useMs) ctx.dispatchMeshFor(d.depthMesh);
        else       ctx.drawMesh(d.depthMesh);
    }

    // A staged rebuild's earlier slices end here: the accumulator keeps its sums for the next one.
    if (!last) return;

    if ((voxelCullLogs_ & (voxelCullLogs_ + 1)) == 0) {
        const u32 considered = voxelSubmitted + voxelCulled;
        AVER_INFO("[Voxi] voxelize {} draw(s), culled {} outside the volume ({:.0f}%), "
                  "{} skinned draw(s) excluded (they contribute no GI -- see voxelizePass), "
                  "{} movable in Play; "
                  "injected-draw box {:.1f}% of the {}^3 grid, clear/resolve/mip over {:.1f}% ({})",
                  voxelSubmitted, voxelCulled,
                  considered ? 100.0 * static_cast<f64>(voxelCulled) / static_cast<f64>(considered) : 0.0,
                  voxelSkinned, voxelMovable, gridFraction(drawsBox, res) * 100.0, res,
                  gridFraction(box0, res) * 100.0, giBoxReason);
    }
    ++voxelCullLogs_;

    // Reduce the atomic sums into the filterable RGBA16F volume.
    ++voxelGen_;
    ctx.uavBarrierTexture(voxelAccumTex_);
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    ctx.setPipeline(resolvePso_);
    ctx.setBindingSet(resolveBindings_);
    {
        const GiDispatchConstants k = dispatchConstants(box0, 0);
        ctx.setConstants(3, &k, kGiDispatchConstantDwords);
        u32 g[3];
        dispatchGroups(box0, 4u, g);
        if (g[0] && g[1] && g[2]) ctx.dispatch(g[0], g[1], g[2]);
    }

    // Records this rebuild's OWN draws box (not box0, which also folds in the previous one) for the
    // next union -- the chain must span exactly one previous rebuild, not accumulate indefinitely.
    // giBoxPrevValid_ false whenever anyUnbounded fired: an untrustworthy box now must not be trusted
    // as "last rebuild's box" next time either.
    giBoxPrevDraws_ = drawsBox;
    giBoxPrevValid_ = !anyUnbounded;
    giBoxRes_ = res;
    giBoxCentre_[0] = center_[0]; giBoxCentre_[1] = center_[1]; giBoxCentre_[2] = center_[2];
    giBoxExtent_ = extent_;
}

// Box-filters each level of the radiance volume into the next, then hands the whole chain back as
// a shader resource.
void VoxiRenderer::filterMips(rhi::IRenderContext& ctx) {
    rhi::ScopedGpuStat gpuStat(ctx, "Voxi mip filter");
    ++voxelGen_;
    ctx.uavBarrierTexture(voxelTex_);
    ctx.setPipeline(mipPso_);
    const u32 res = voxelResBuilt_;
    for (u32 m = 1; m < voxelMips_; ++m) {
        // Level m-1 becomes readable while level m stays writable: a per-subresource transition.
        ctx.textureBarrier(voxelTex_, rhi::ResourceState::UnorderedAccess,
                           rhi::ResourceState::NonPixelShaderResource, m - 1);
        ctx.setBindingSet(mipBindings_[m - 1]);
        // Bounded to the box THIS rebuild's clear/resolve touched at mip 0 (giDispatchBox0_),
        // rounded to this level via mipBox(): CSMip's 2x2x2 footprint means a dest voxel outside
        // mipBox(box0, m, res) has its whole footprint outside box0 at mip m-1. srcMip stays 0
        // regardless -- the single-mip SRV this binding set declares already rebases the Load to
        // level m-1, so the shader never needs to know which real mip that is.
        const VoxelBox mb = mipBox(giDispatchBox0_, m, res);
        const GiDispatchConstants k = dispatchConstants(mb, 0);
        ctx.setConstants(3, &k, kGiDispatchConstantDwords);
        u32 g[3];
        dispatchGroups(mb, 4u, g);
        if (g[0] && g[1] && g[2]) ctx.dispatch(g[0], g[1], g[2]);
        ctx.uavBarrierTexture(voxelTex_);
    }
    // The coarsest level is never a source, so the loop never demoted it: reconcile it here to make
    // the whole-resource transition below legal.
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::UnorderedAccess,
                       rhi::ResourceState::NonPixelShaderResource, voxelMips_ - 1);
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::NonPixelShaderResource,
                       rhi::ResourceState::ShaderResource);
}

// Hands the backend Voxi's per-frame constant block.
bool VoxiRenderer::sceneConstants(const void** data, u32* bytes) const {
    if (!canRecord()) return false;
    *data = &cb_; *bytes = sizeof(cb_);
    return true;
}

// Whether the blended draw reads the backdrop texture.
bool VoxiRenderer::blendedDrawReadsBackdrop(const void* materialConstants, u32 bytes) const {
    if (bytes < sizeof(pbr::MaterialConstants) || !materialConstants) return true;   // unknown layout: assume yes
    const auto& mat = *static_cast<const pbr::MaterialConstants*>(materialConstants);
    return mat.attenuationDistance > 0.0f || mat.graphId != 0;
}

// True once the feature is up: shadowing and the bounce are terms inside Voxi's lit pixel shader.
bool VoxiRenderer::overridesScenePipeline() const { return canRecord(); }

// True while the debug view replaces the scene, including the backend's line draws.
// Two reasons to replace the scene, not interchangeable -- see shadowHistoryActive() in the header,
// which asks only about the first.
// The table the blended pipeline expects bound. The device asks during its replay, since this
// feature is not on the stack then -- see IRenderFeature::sceneBindlessTable.
rhi::BindlessTableHandle VoxiRenderer::sceneBindlessTable() const { return rtTexTable_; }

// While the scene set builds (blocked_, latched at prePass) Voxi claims the scene and records nothing: the
// frame is blank rather than drawn by a half-built renderer.
bool VoxiRenderer::suppressesScene() const { return blocked_ || targetsStale_ || debugViewActive() || rayDrivenActive(); }

// Debug raymarch has no depth; ray-driven writes real depth so sky lands on ray misses.
bool VoxiRenderer::suppressesWholeFrame() const { return blocked_ || targetsStale_ || debugViewActive(); }

// Draws the scene pass replacement (debug view or ray-driven); debug wins if both are active.
void VoxiRenderer::scenePass(rhi::IRenderContext& ctx) {
    ptRanThisFrame_ = false;
    if (!canRecord()) return;   // blank frame, or the targets changed under a late scene pass
    translucentInPath_ = false;
    // Recorded late (wantsLateScenePass) this frame's draws exist; bring the movers up to date first.
    if ((!draws_.empty() || rtRefitDeferred_) && !debugViewActive() && rayDrivenActive()) latePatchMovers(ctx);
    if (pathTracingWanted() && !debugViewActive()) {
        const char* why = nullptr;
        const bool staged = rayDrivenActive() && rdStagedActive(&why);
        if (!staged && !ptFallbackLogged_) {
            ptFallbackLogged_ = true;
            AVER_WARN("[Voxi] Path Tracing needs the staged ray-driven frame (D3D12, ray tracing on), but {}; "
                      "rendering without it (said once)",
                      why ? why : "ray-driven primary visibility is not active");
        }
    }
    if (!debugViewActive() && rayDrivenActive()) {
        // Staged ray-driven passes checked first, before any single-pass state is touched, so
        // rayDrivenStages == 0 reaches the single-pass code exactly as before this feature existed --
        // rdStagedActive() returns false on its first line then, `reason` stays null, and nothing
        // below runs differently. See the header's rdStagedActive() comment for every condition it
        // checks.
        const char* stagedFallbackReason = nullptr;
        if (rdStagedActive(&stagedFallbackReason)) {
            if (!rdStagedRunLogged_) {
                rdStagedRunLogged_ = true;
                AVER_INFO("[Voxi] staged ray-driven passes running: visibility -> lighting (shadow, GI, "
                          "sky occlusion, reflections) -> shade (GPU spans 'Voxi RD visibility', "
                          "'Voxi RD lighting stages', 'Voxi ray-driven primary')");
            }
            recordStagedRayDriven(ctx);
            return;
        }
        // Logged once, only when there WAS a reason to give: rdStagedActive() leaves `reason` null
        // when nothing was requested or nothing needs staging, which isn't a fallback worth a line.
        // A non-D3D12 backend is an expected, known limitation (staged is not ported yet) that every
        // Vulkan session hits now that staged is the default -- INFO, not a warning. Anything else
        // (a pipeline that failed to compile, a missing resource) is a real fault and stays WARN.
        if (stagedFallbackReason && !rdStagedFallbackLogged_) {
            rdStagedFallbackLogged_ = true;
            if (dev_ && dev_->backend() != rhi::Backend::D3D12)
                AVER_INFO("[Voxi] staged ray-driven passes are D3D12-only; this backend runs the "
                          "single-pass ray-driven primary (said once)");
            else
                AVER_WARN("[Voxi] voxi.rayDrivenStages (1 or 2) requested the staged ray-driven passes, "
                          "but {}; falling back to the single-pass ray-driven primary (said once)",
                          stagedFallbackReason);
        }
        // Its own marker, so the go/no-go against the rasteriser is a subtraction between two named
        // spans rather than a difference of whole frames.
        rhi::ScopedGpuStat rayStat(ctx, "Voxi ray-driven primary");
        const bool gbufBound = pickGbuf(rayDrivenPso_, rayDrivenGbufPso_) == rayDrivenGbufPso_;
        const rhi::PipelineHandle rdPso =
            gbufBound ? (rayDrivenTexGbufPso_ ? rayDrivenTexGbufPso_ : rayDrivenGbufPso_)
                      : (rayDrivenTexPso_     ? rayDrivenTexPso_     : rayDrivenPso_);
        const bool lamps = publishLocalLights(kRdSinglePassLamps && localLightsReady(),
                                              "the single-pass PSRayDriven");
        ctx.setPipeline(rdPso);
        ctx.setBindingSet(bindings_);
        ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
        // A no-op on the untextured pipeline, which declares no bindless table -- so this does not
        // need to be inside the branch above.
        ctx.setBindlessTable(rtTexTable_);
        ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
        ctx.drawFullscreen();
        if (lamps) {
            rdLocalHistFrame_ = rtFrameIndex_;
            rdLocalHistHash_ = rdLocalLightHash_;
        }
        return;
    }
    if (!debugPso_) return;
    rhi::ScopedGpuStat gpuStat(ctx, "Voxi debug view");
    ctx.setPipeline(debugPso_);
    ctx.setBindingSet(bindings_);
    // Table 1 is bound outright: this pipeline declares it and Tier 1 populates whole tables.
    ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
    ctx.drawFullscreen();
}

bool VoxiRenderer::primaryVisibility(rhi::PrimaryVisibility& out) const {
    if (!rdVisWrittenThisFrame_ || !rdVisBuf_ || !rdStagedRowPitch_) return false;
    out.buffer       = rdVisBuf_;
    out.rowPitch     = rdStagedRowPitch_;
    out.elementCount = rdVisBufElemCapacity_;
    for (u32 i = 0; i < 4; ++i) out.viewport[i] = curSceneViewport_[i] > 0.0f ? static_cast<u32>(curSceneViewport_[i]) : 0u;
    return out.viewport[2] && out.viewport[3];
}

// Staged ray-driven passes: records visibility, then optional probe/trace, then shadow/GI/sky-occ/refl.
void VoxiRenderer::recordStagedRayDriven(rhi::IRenderContext& ctx) {
    const bool gbufBound = pickGbuf(rayDrivenPso_, rayDrivenGbufPso_) == rayDrivenGbufPso_;
    // NRD2: Stage B's variant that writes the demodulated split (ensureNrd2 built it this frame).
    const bool nrd2 = nrd2Frame_ && gbufBound && rayDrivenSplitNrd2Pso_ != 0;
    const rhi::PipelineHandle stageBPso = nrd2 ? rayDrivenSplitNrd2Pso_
                                        : gbufBound ? rayDrivenSplitTexGbufPso_ : rayDrivenSplitTexPso_;

    // Dispatch from scene viewport, not full render target; buffers sized to full target.
    const u32 dispatchW = curSceneViewport_[2] > 0.0f ? static_cast<u32>(curSceneViewport_[2]) : 0u;
    const u32 dispatchH = curSceneViewport_[3] > 0.0f ? static_cast<u32>(curSceneViewport_[3]) : 0u;
    const u32 gx = (dispatchW + 7u) / 8u;   // every staged compute stage declares [numthreads(8,8,1)]
    const u32 gy = (dispatchH + 7u) / 8u;

    // Row pitch rides viewParams.w for these uploads only.
    cb_.viewParams[3] = static_cast<f32>(rdStagedRowPitch_);

    // Decide local lights here, before stages that read the count.
    // UNIFIED_LIGHTS.md: CSRdShadow resolves every light the visible surface uses (the sun is one list entry) and
    // writes the pixel's light slots to u19; no separate lamp pass. The list is published for the hits.
    // `live` still means "lamps present" for the glass replay, which reads a lamp visibility from the slots' .a.
    const bool localLights = publishLocalLights(localLightsReady() && gx && gy, "CSRdShadow (the visible surface's lights)");

    {
        rhi::ScopedGpuStat stat(ctx, "Voxi RD visibility");
        ctx.setPipeline(rdVisCsPso_);
        ctx.setBindingSet(bindings_);
        ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
        ctx.setBindlessTable(rtTexTable_);
        ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
        if (gx && gy) ctx.dispatch(gx, gy, 1);
    }
    ctx.uavBarrierBuffer(rdVisBuf_);
    rdVisWrittenThisFrame_ = gx && gy;

    const bool perStage = settings_.rayDrivenStageTiming;
    auto stageBegin = [&](const char* label) { if (perStage) ctx.pushMarker(label); };
    auto stageEnd = [&](rhi::TextureHandle written) {
        if (!perStage) return;
        ctx.uavBarrierTexture(written);
        ctx.popMarker();
    };
    // The buffer overload S1/G1 need: same shape, uavBarrierBuffer instead of uavBarrierTexture,
    // since gRdShadowTiles/gRdGiCand are StructuredBuffers, not textures.
    auto stageEndBuffer = [&](rhi::BufferHandle written) {
        if (!perStage) return;
        ctx.uavBarrierBuffer(written);
        ctx.popMarker();
    };

    // Path Tracing, Reference mode: Stage B traces each pixel's whole path itself, so the GI, sky-occlusion,
    // lamp and reflection stages have nothing to give it (lamps are still published for its next-event pick).
    if (pathTracingWanted() && !ptTwinsTried_) requestBuild(kPsoPt);
    const bool ptRef = ptReferenceWanted() && rdPtRefCsPso_ != 0;
    if (ptRef) ptRanThisFrame_ = true;
    // Without its pass Stage B must not take the reference branch (bit 4 of ptBounceParams.x).
    else cb_.ptBounceParams[0] = static_cast<f32>(static_cast<u32>(cb_.ptBounceParams[0]) & ~16u);

    // Reflection register/filter split: optional sub-stage C.
    const bool reflSplit = !ptRef && !nrd2Frame_ && (cb_.shadowParams[2] > 0.5f && cb_.rtParams[3] > 0.5f) &&
                            settings_.rayDrivenReflSplit && rdReflSplitCsPso_ != 0 && rdReflFilterCsPso_ != 0;
    if (reflSplit && !rdReflSplitRunLogged_) {
        rdReflSplitRunLogged_ = true;
        AVER_INFO("[Voxi] reflection register/filter split running: CSRdRefl (R1) traces the "
                  "reflection ray alone, CSRdReflFilter (R2) runs rtReflectionSpatial's history "
                  "gather in its own pass");
    } else if (cb_.shadowParams[2] > 0.5f && cb_.rtParams[3] > 0.5f && settings_.rayDrivenReflSplit &&
               !reflSplit && !rdReflSplitFallbackLogged_) {
        rdReflSplitFallbackLogged_ = true;
        AVER_WARN("[Voxi] voxi.rayDrivenReflSplit requested the reflection register/filter split, "
                  "but the split or filter pipeline did not compile; behaving as the unsplit "
                  "CSRdRefl (said once)");
    }

    // GI candidate samples the GI-only shadow map; transitions back to ShaderResource after.
    const bool giHitShadowMap = settings_.rtGiHitShadowMap && giShadowTex_ != 0;
    if (giHitShadowMap)
        ctx.textureBarrier(giShadowTex_, rhi::ResourceState::ShaderResource,
                           rhi::ResourceState::NonPixelShaderResource);
    // Sun shadow history read side; transitions back after.
    const rhi::TextureHandle shadowHistRead = rtShadowHist_[rtHistWriteIdx_];
    if (shadowHistRead)
        ctx.textureBarrier(shadowHistRead, rhi::ResourceState::ShaderResource,
                           rhi::ResourceState::NonPixelShaderResource);
    // Local lights history read side; transitions back after the group.
    const rhi::TextureHandle localHistRead =
        localLights ? (rdLocalOutThisFrame_ == rdLocalHist_[0] ? rdLocalHist_[1] : rdLocalHist_[0]) : 0;
    if (localHistRead)
        ctx.textureBarrier(localHistRead, rhi::ResourceState::ShaderResource,
                           rhi::ResourceState::NonPixelShaderResource);
    // RADIANCE CACHE: bit 128 of ambientParams.w gates the NeuRaC twin.
    const bool rcBit = (static_cast<u32>(cb_.ambientParams[3]) & 128u) != 0u;
    bool usedCacheTwin = false;
    // PATH TRACING: the AVER_PT_PATHS twins take the GI-candidate and reflection dispatches.
    const bool pt = pathTracingWanted();
    bool skyOccRan = false, reflRan = false;   // CSRdHalfFill's inputs
    {
        // Wraps every dispatch below -- see this function's comment on why no barrier or timestamp
        // sits between the four lighting stages (S1/G1 just below are the one exception).
        if (!perStage) ctx.pushMarker("Voxi RD lighting stages");

        // ReSTIR GI is chosen AND cone trace is gated on.
        const bool giDispatch = !ptRef && cb_.voxelParams[3] > 0.5f && cb_.giRestirParams[0] > 0.5f;
        // Half-rate ReSTIR GI: gated on rayDrivenStages==2, checkerboard variant compiled, and something
        // to rebuild the skipped half: FidelityFX's history, or under NRD2 CSRdHalfFill from this frame.
        const bool nrd2GiFill = nrd2Frame_ && settings_.nrd2HalfRateGi && rdHalfFillCsPso_ != 0;
        const bool giCb = giDispatch && settings_.rayDrivenStages == 2u && rdGiCbCsPso_ != 0 &&
                          (denoiseGiRanThisFrame_ || nrd2GiFill);
        giCbWrittenThisFrame_ = giCb;
        giCbParityWritten_    = denoiseFrame_ & 1u;
        if (giCb && !rdGiCbRunLogged_) {
            rdGiCbRunLogged_ = true;
            AVER_INFO("[Voxi] half-rate ReSTIR GI running: CSRdGi traces one checkerboard half each "
                      "frame, the denoiser (or under NRD2, CSRdHalfFill) reconstructs the rest");
        } else if (settings_.rayDrivenStages == 2u && !giCb && !rdGiCbFallbackLogged_ &&
                   !(nrd2Frame_ && !settings_.nrd2HalfRateGi)) {
            rdGiCbFallbackLogged_ = true;
            const char* why =
                !giDispatch ? "ReSTIR GI is not the diffuse estimator this frame"
                : (!denoiseGiRanThisFrame_ && !nrd2Frame_) ? "the GI denoiser did not run this frame "
                                                             "(denoiser off, or unavailable)"
                : (nrd2Frame_ && !rdHalfFillCsPso_) ? "NRD2's half-rate fill did not compile"
                                      : "the checkerboard CSRdGi variant did not compile";
            AVER_INFO("[Voxi] voxi.rayDrivenStages 2 requested half-rate ReSTIR GI, but {}; behaving as "
                      "rayDrivenStages 1 for the GI stage (said once)", why);
        }

        // ---- Sub-stage splits (rayDrivenShadowTiles / rayDrivenGiSplit) ----
        const bool shadowTiles = settings_.rayDrivenShadowTiles && rdShadowProbeCsPso_ && rdShadowTiledCsPso_;
        // GI split: giDispatch + setting + trace/split pair; disabled if NeuRaC viewer is active.
        const bool neuracViewing = rcBit && (neuracView_ & 7u) != 0u;
        const bool giSplit = giDispatch && settings_.rayDrivenGiSplit && !neuracViewing &&
                              (giCb ? (rdGiTraceCbCsPso_ && rdGiSplitCbCsPso_)
                                    : (rdGiTraceCsPso_ && rdGiSplitCsPso_));
        if (shadowTiles && !rdShadowTilesRunLogged_) {
            rdShadowTilesRunLogged_ = true;
            AVER_INFO("[Voxi] shadow probe/tile split running: CSRdShadowProbe traces one ray per 8x8 "
                      "tile, CSRdShadow skips its own ray wherever its tile's 3x3 neighbourhood agrees");
        } else if (settings_.rayDrivenShadowTiles && !shadowTiles && !rdShadowTilesFallbackLogged_) {
            rdShadowTilesFallbackLogged_ = true;
            AVER_WARN("[Voxi] voxi.rayDrivenShadowTiles requested the shadow probe/tile split, but the "
                      "probe or tiled shadow pipeline did not compile; behaving as the unsplit "
                      "CSRdShadow (said once)");
        }
        if (giSplit && !rdGiSplitRunLogged_) {
            rdGiSplitRunLogged_ = true;
            AVER_INFO("[Voxi] GI candidate-trace split running: CSRdGiTrace traces the fresh ReSTIR GI "
                      "candidate, CSRdGi resamples/shades from the stored result");
        } else if (giDispatch && settings_.rayDrivenGiSplit && !giSplit && !neuracViewing &&
                   !rdGiSplitFallbackLogged_) {
            rdGiSplitFallbackLogged_ = true;
            AVER_WARN("[Voxi] voxi.rayDrivenGiSplit requested the GI candidate-trace split, but the {} "
                      "GI-trace/split pipeline pair did not compile; behaving as the unsplit CSRdGi "
                      "(said once)", giCb ? "checkerboard" : "plain");
        }

        // S1: CSRdShadowProbe, one 8x8-tile group per probe, dispatched over the SAME (gx, gy) as the
        // shadow stage below -- writes gRdShadowTiles (u18).
        if (shadowTiles) {
            stageBegin("Voxi RD shadow probe stage");
            ctx.setPipeline(rdShadowProbeCsPso_);
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            if (gx && gy) ctx.dispatch(gx, gy, 1);
            stageEndBuffer(rdShadowTileBuf_);
        }
        // The shadow stage reads the probe's tiles; CSRdGiTrace (G1) does not, so it is recorded after this barrier
        // and overlaps the shadow stage, its own barrier waiting until CSRdGi reads gRdGiCand.
        if (shadowTiles) ctx.uavBarrierBuffer(rdShadowTileBuf_);
        // G1: CSRdGiTrace, writing gRdGiCand (u17) -- compacted to the traced half's pixels alone in
        // checkerboard mode (giCb), the identical parity bit CSRdGi's own checkerboard dispatch below
        // carries. Copied here rather than shared.
        if (giSplit) {
            stageBegin("Voxi RD GI trace stage");
            const rhi::PipelineHandle g1Rc = giCb ? rdGiTracePtRcCbCsPso_ : rdGiTracePtRcCsPso_;
            const bool g1PtRc = pt && rcBit && g1Rc != 0;
            const rhi::PipelineHandle g1Pt =
                g1PtRc ? g1Rc
                       : pt ? (giCb ? rdGiTracePtCbCsPso_ : rdGiTracePtCsPso_) : rhi::PipelineHandle(0);
            const rhi::PipelineHandle g1Twin =
                (!g1Pt && rcBit) ? (giCb ? rdGiTraceCacheCbCsPso_ : rdGiTraceCacheCsPso_) : rhi::PipelineHandle(0);
            if (g1Twin || g1PtRc) usedCacheTwin = true;
            if (g1Pt) ptRanThisFrame_ = true;
            ctx.setPipeline(g1Pt ? g1Pt : g1Twin ? g1Twin : (giCb ? rdGiTraceCbCsPso_ : rdGiTraceCsPso_));
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            u32 g1x = gx;
            if (giCb) {
                // Half the COLUMNS, rounded up, not half the threads: covers ceil(dispatchW/2).
                g1x = ((dispatchW + 1u) / 2u + 7u) / 8u;
                cb_.viewParams[3] = static_cast<f32>(rdStagedRowPitch_ | (giCbParityWritten_ << 16));
                ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
                cb_.viewParams[3] = static_cast<f32>(rdStagedRowPitch_);   // restore before any later upload
            } else {
                ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            }
            if (g1x && gy) ctx.dispatch(g1x, gy, 1);
            stageEndBuffer(rdGiCandBuf_);
        }
        stageBegin("Voxi RD shadow stage");
        ctx.setPipeline(shadowTiles ? rdShadowTiledCsPso_ : rdShadowCsPso_);
        ctx.setBindingSet(bindings_);
        ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
        ctx.setBindlessTable(rtTexTable_);
        ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
        if (gx && gy) ctx.dispatch(gx, gy, 1);
        if (localLights && gx && gy) rdKeyFrame_ = rtFrameIndex_;
        if (giSplit) ctx.uavBarrierBuffer(rdGiCandBuf_);   // G1's candidates, for CSRdGi below
        // CSRdTailVis reads CSRdShadow's exact light (gRdLocalOut.x) and depth (gRdSunVisTex.a), then writes the tail's
        // shadow fraction (gRdLocalOut.a, next frame's lamp history); CSRdTailFilter its 5x5 (.z).
        if (localLights && rdTailVisCsPso_ && rdTailFilterCsPso_ && rdLocalOutThisFrame_ && gx && gy) {
            stageEnd(rdSunVisTex_);
            stageBegin("Voxi RD tail lights");
            ctx.uavBarrierTexture(rdLocalOutThisFrame_);
            ctx.uavBarrierTexture(rdSunVisTex_);
            ctx.setPipeline(rdTailVisCsPso_);
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            ctx.dispatch(((dispatchW + 1u) / 2u + 7u) / 8u, ((dispatchH + 1u) / 2u + 7u) / 8u, 1);
            // The 5x5 reads the neighbours' fractions.
            ctx.uavBarrierTexture(rdLocalOutThisFrame_);
            ctx.setPipeline(rdTailFilterCsPso_);
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            ctx.dispatch(gx, gy, 1);
            rdLocalHistFrame_ = rtFrameIndex_;
            rdLocalHistHash_ = rdLocalLightHash_;
        }
        if (perStage && rdLocalOutThisFrame_) ctx.uavBarrierTexture(rdLocalOutThisFrame_);   // the light slots
        stageEnd(rdSunVisTex_);

        if (giDispatch) {
            stageBegin("Voxi RD GI stage");
            const rhi::PipelineHandle giRc = giCb ? rdGiPtRcCbCsPso_ : rdGiPtRcCsPso_;
            const bool giPtRc = !giSplit && pt && rcBit && giRc != 0;
            if (giPtRc) usedCacheTwin = true;
            const rhi::PipelineHandle giPt =
                giPtRc ? giRc
                       : (!giSplit && pt) ? (giCb ? rdGiPtCbCsPso_ : rdGiPtCsPso_) : rhi::PipelineHandle(0);
            const rhi::PipelineHandle giTwin =
                giPt ? giPt
                     : (!giSplit && rcBit) ? (giCb ? rdGiCacheCbCsPso_ : rdGiCacheCsPso_)
                                           : rhi::PipelineHandle(0);
            if (giTwin && !giPt) usedCacheTwin = true;
            if (giPt) ptRanThisFrame_ = true;
            ctx.setPipeline(giTwin ? giTwin
                                   : giSplit ? (giCb ? rdGiSplitCbCsPso_ : rdGiSplitCsPso_)
                                             : (giCb ? rdGiCbCsPso_     : rdGiCsPso_));
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            if (giCb) {
                // Bit 16 carries the checkerboard parity, for this one upload only.
                cb_.viewParams[3] = static_cast<f32>(rdStagedRowPitch_ | (giCbParityWritten_ << 16));
                ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
                cb_.viewParams[3] = static_cast<f32>(rdStagedRowPitch_);   // restore before any later upload
            } else {
                ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            }
            if (gx && gy) ctx.dispatch(gx, gy, 1);
            stageEnd(rdGiTex_);
        }
        // Reference path tracing: one path per pixel into gRdGiTex, which Stage B shades the pixel with.
        if (ptRef) {
            stageBegin("Voxi RD reference path");
            ctx.setPipeline(rdPtRefCsPso_);
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            if (gx && gy) ctx.dispatch(gx, gy, 1);
            stageEnd(rdGiTex_);
        }
        // FRAME MIDPOINT (IDevice::frameMidpoint): with frame interpolation the previous frame's real
        // image is presented once the GPU gets here -- after the GI stage.
        if (dev_) dev_->frameMidpoint();

        // CSRdSkyOcc: CPU mirror of PSRayDriven's own sky-occlusion condition.
        skyOccRan = !ptRef && cb_.ambientParams[0] > 0.5f &&
                    (cb_.giRestirParams[0] > 0.5f || cb_.voxelParams[3] <= 0.5f);
        if (skyOccRan) {
            stageBegin("Voxi RD sky occlusion stage");
            ctx.setPipeline(rdSkyOccCsPso_);
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            if (gx && gy) ctx.dispatch(gx, gy, 1);
            stageEnd(rdAoTex_);
        }

        // CSRdRefl: CPU mirror of PSRayDriven's reflection-block condition's reflections-enabled half.
        reflRan = !ptRef && cb_.shadowParams[2] > 0.5f && cb_.rtParams[3] > 0.5f;
        if (reflRan) {
            stageBegin("Voxi RD reflection stage");
            // Over the radiance cache when it is live: a reflected surface it covers needs no more rays.
            const rhi::PipelineHandle reflPtRc =
                (pt && rcBit) ? (reflSplit ? rdReflSplitPtRcCsPso_ : rdReflPtRcCsPso_) : rhi::PipelineHandle(0);
            const rhi::PipelineHandle reflPt =
                reflPtRc ? reflPtRc : pt ? (reflSplit ? rdReflSplitPtCsPso_ : rdReflPtCsPso_) : rhi::PipelineHandle(0);
            ctx.setPipeline(reflPt ? reflPt : reflSplit ? rdReflSplitCsPso_ : rdReflCsPso_);
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            if (gx && gy) ctx.dispatch(gx, gy, 1);
            stageEnd(rdReflTex_);
        }
        if (!perStage) ctx.popMarker();
    }
    if (ptRanThisFrame_ && !ptRunLogged_) {
        ptRunLogged_ = true;
        AVER_INFO("[Voxi] Path Tracing running: {}-vertex paths for ReSTIR GI and reflections, sun and "
                  "lamps lit at every vertex", ptBounces_);
    }
    if (giHitShadowMap)
        ctx.textureBarrier(giShadowTex_, rhi::ResourceState::NonPixelShaderResource,
                           rhi::ResourceState::ShaderResource);
    if (shadowHistRead)
        ctx.textureBarrier(shadowHistRead, rhi::ResourceState::NonPixelShaderResource,
                           rhi::ResourceState::ShaderResource);
    if (localHistRead)
        ctx.textureBarrier(localHistRead, rhi::ResourceState::NonPixelShaderResource,
                           rhi::ResourceState::ShaderResource);
    // RADIANCE CACHE RESOLVE: after the lighting group closes, the resolve reads exactly what the
    // group's twins wrote. usedCacheTwin is only ever set inside a giDispatch branch.
    if (usedCacheTwin && rc_.valid()) {
        ctx.uavBarrierBuffer(rc_.accumBuffer());
        rc_.recordResolve(ctx);
        ctx.uavBarrierBuffer(rc_.cellsBuffer());
    }
    // Stage B's reads of gRdSunVisTex/gRdGiTex/gRdAoTex/gRdReflTex must see whichever of the four
    // dispatches above wrote them -- all four barriers sit here, unconditionally.
    ctx.uavBarrierTexture(rdSunVisTex_);
    ctx.uavBarrierTexture(rdGiTex_);
    ctx.uavBarrierTexture(rdAoTex_);
    ctx.uavBarrierTexture(rdReflTex_);
    // Local lights: Stage B reads gRdLocalOut through u19 the way it reads u12 above.
    if (rdLocalOutThisFrame_) ctx.uavBarrierTexture(rdLocalOutThisFrame_);   // CSRdShadow's light slots
    // The GI surface-normal history (u8) has two writers: CSRdGi and Stage B's own miss branch.
    const u32 giNrmWrite = 1u - rtHistWriteIdx_;
    if (cb_.voxelParams[3] > 0.5f && cb_.giRestirParams[0] > 0.5f && giSurfNrmHist_[giNrmWrite])
        ctx.uavBarrierTexture(giSurfNrmHist_[giNrmWrite]);

    // NRD2 half rate: CSRdHalfFill fills each skipped checkerboard half from this frame's traced one.
    if (nrd2Frame_ && rdHalfFillCsPso_ && gx && gy) {
        const u32 hrBits = static_cast<u32>(cb_.giShadowParams[3]);
        const bool fillGi   = giCbWrittenThisFrame_;
        const bool fillAo   = (hrBits & 128u) != 0u && skyOccRan;
        const bool fillRefl = (hrBits & 4u) != 0u && reflRan;
        if (fillGi || fillAo || fillRefl) {
            rhi::ScopedGpuStat stat(ctx, "Voxi RD half-rate fill");
            ctx.uavBarrierTexture(nrd2_.targets().diffuse);    // CSRdShadow's normal guide (u3)
            ctx.uavBarrierTexture(nrd2_.targets().specular);   // CSRdRefl's skip marks and hit distances (u23)
            ctx.setPipeline(rdHalfFillCsPso_);
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            cb_.viewParams[3] = static_cast<f32>(rdStagedRowPitch_ | (giCbParityWritten_ << 16) |
                                                 (fillGi ? 1u << 17 : 0u) | (fillAo ? 1u << 18 : 0u) |
                                                 (fillRefl ? 1u << 19 : 0u));
            ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            cb_.viewParams[3] = static_cast<f32>(rdStagedRowPitch_);
            ctx.dispatch(gx, gy, 1);
            if (fillGi) ctx.uavBarrierTexture(rdGiTex_);
            if (fillAo) ctx.uavBarrierTexture(rdAoTex_);
            if (fillRefl) ctx.uavBarrierTexture(rdReflTex_);
        }
    }

    // Sub-stage C, R2: CSRdReflFilter -- reruns rtReflectionSpatial against R1's gRtReflHistOut write.
    if (reflSplit) {
        // 1u - rtHistWriteIdx_, not rtHistWriteIdx_: same reasoning as giNrmWrite above, for u3.
        ctx.uavBarrierTexture(rtReflHist_[1u - rtHistWriteIdx_]);
        {
            rhi::ScopedGpuStat stat(ctx, "Voxi RD reflection filter");
            ctx.setPipeline(rdReflFilterCsPso_);
            ctx.setBindingSet(bindings_);
            ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
            ctx.setBindlessTable(rtTexTable_);
            ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
            if (gx && gy) ctx.dispatch(gx, gy, 1);
        }
        ctx.uavBarrierTexture(rdReflTex_);   // R2's own write, before Stage B reads gRdReflTex
    }

    {
        rhi::ScopedGpuStat stat(ctx, "Voxi ray-driven primary");   // same span name the single pass
                                                                    // uses, so an A/B across the switch
                                                                    // is a like-for-like GPU-time
                                                                    // comparison of just this draw.
        // TRANSLUCENCY IN THE PATH (ptBounceParams.w): Stage B composites every translucent surface the
        // primary ray crosses over the opaque background (voxi.hlsl), and the device then skips their
        // blended replay (blendedDrawsResolvedInScene). Not with the eye inside a medium: that case keeps
        // the replay's eye-inside refraction.
        translucentInPath_ = settings_.translucencyInPath && rtTlasTranslucent_ > 0 && cb_.cameraMedium[0] < 0.5f;
        cb_.ptBounceParams[3] = translucentInPath_ ? 1.0f : 0.0f;
        // NRD2: CSRdRefl wrote S (u23); Stage B reads its hit distance and overwrites it.
        if (nrd2) ctx.uavBarrierTexture(nrd2_.targets().specular);
        ctx.setPipeline(stageBPso);
        ctx.setBindingSet(bindings_);
        ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
        ctx.setBindlessTable(rtTexTable_);
        ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
        ctx.drawFullscreen();
    }
    // NRD2 right after Stage B, so the sky dome, blended replay, particles and post see the result.
    if (nrd2) {
        render::denoise::Nrd2Params np;
        std::memcpy(np.diffuse, settings_.nrd2Params, sizeof(np.diffuse));
        std::memcpy(np.specular, settings_.nrd2Params + 6, sizeof(np.specular));
        np.bypass = settings_.nrd2Bypass;
        // Off while Path Tracing accumulates (ptBounceParams.y == 2): the network never sees history.
        np.network = settings_.nrd2Network && !(pathTracingWanted() && cb_.ptBounceParams[1] > 1.5f);
        np.stabilise  = settings_.nrd2Stab;
        np.stabFrames = static_cast<f32>(settings_.nrd2StabFrames);
        np.despeckle  = settings_.nrd2Despeckle;
        np.despeckleCap = settings_.nrd2DespeckleCap;
        np.speckle      = settings_.nrd2Speckle;
        np.blurRadius   = settings_.nrd2BlurRadius;
        np.coarseCap    = settings_.nrd2CoarseCap;
        np.combineRef   = settings_.nrd2CombineRef;
        np.midCap       = settings_.nrd2MidCap;
        nrd2_.setParams(np);
        render::denoise::Nrd2::Inputs in;
        in.viewZ           = dev_->gBufferViewZTexture();
        in.normalRoughness = dev_->gBufferNormalRoughnessTexture();
        in.gbufferState    = rhi::ResourceState::RenderTarget;
        for (u32 a = 0; a < 4; ++a) in.viewport[a] = static_cast<u32>(curSceneViewport_[a]);
        // Traced this frame (the capture's fresh pixels): ReSTIR GI's checkerboard drives D, the glossy
        // reflections' (giShadowParams.w bit 4, parity rtHistParams.z) drives S.
        in.halfRate = (giCbWrittenThisFrame_ ? 1u : 0u) | ((giCbParityWritten_ & 1u) << 1) |
                      ((static_cast<u32>(cb_.giShadowParams[3]) & 4u) != 0u ? 4u : 0u) |
                      ((static_cast<u32>(cb_.rtHistParams[2]) & 1u) << 3);
        // Stabiliser inputs. prevCamPos_ already holds this frame's eye here; the fold only needs one point
        // used consistently for the previous matrix and the eye offset.
        in.velocity = dev_->gBufferVelocityTexture();
        dev_->gBufferPrevViewProj(in.prevViewProj);
        std::memcpy(in.prevCamPos, prevCamPos_, sizeof(in.prevCamPos));
        in.historyValid = !dev_->gBufferHistoryInvalid();
        in.sunMoved     = rtHistSunMoved();
        dev_->taaJitter(in.jitter);
        if (nrd2CapturePending_ && nrd2_.valid()) {
            nrd2CapturePending_ = false;
            nrd2_.startCapture(nrd2CaptureCfg_);
        }
        if (nrd2_.record(ctx, in)) {
            nrd2_.recordCompose(ctx);
            if (!nrd2RunLogged_) {
                nrd2RunLogged_ = true;
                AVER_INFO("[Voxi] NRD2 running: single-frame denoising of the composed lighting, Voxi's "
                          "histories and FidelityFX off (GPU spans 'NRD2', 'NRD2 compose')");
            }
        }
    }
    cb_.ptBounceParams[3] = 0.0f;
    // What stays on a frame CSRdGi traced GI at half rate, is bit 17 plus the parity in bit 16.
    cb_.viewParams[3] = giCbWrittenThisFrame_
                      ? static_cast<f32>((1u << 17) | (giCbParityWritten_ << 16))
                      : 0.0f;
    // Local lights: cameraMedium z/w deliberately NOT reset here -- the blended replay lights its
    // panes with the same lamps, so its ReSTIR GI must drop a lamp's emission exactly as the opaque
    // stages did.

    // Bit 16, set last: tells the blended replay that gRdSunVisTex/gRdGiTex/gRdAoTex/gRdReflTex hold
    // THIS frame's values, so a translucent pixel on the opaque surface the staged passes already lit
    // may reuse them (Settings::blendedReuseStagedLighting). Self-clearing: this is the ONLY place
    // that sets the bit.
    if (settings_.blendedReuseStagedLighting) {
        cb_.giShadowParams[3] = static_cast<f32>(static_cast<u32>(cb_.giShadowParams[3]) | 16u);
    }
}

// Remembers the new targets, resizes the ray-traced histories to match, and asks for scene pipelines that bake
// the new sample count and formats. This can run in the middle of a frame (a setting applied from the UI), so
// the pipelines are neither built nor swapped here: targetsStale_ stops every recorder at once, and pumpBuilds
// starts the build at the next frame boundary. In synchronous mode it rebuilds on the spot, as it always did.
void VoxiRenderer::onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                          u32 width, u32 height) {
    if (!res_ || !initialised_) return;
    wantSamples_ = sampleCount;
    wantColor_ = color;
    wantDepth_ = depth;
    if (sceneAdopted_ && (builtSamples_ != sampleCount || builtColor_ != color || builtDepth_ != depth))
        targetsStale_ = true;
    // Remembered even when the call below decides to allocate nothing: setSettings needs a size to
    // create at if ray tracing is switched on later.
    rtHistWantW_ = width;
    rtHistWantH_ = height;
    if (!ensureShadowHistory(width, height))
        AVER_ERROR("[Voxi] ray-traced shadow history could not be (re)created at {}x{}", width, height);
    if (!ensureRdStagedResources(width, height))
        AVER_ERROR("[Voxi] staged ray-driven resources could not be (re)created at {}x{}", width, height);
    if (!asyncBuilds_) pumpBuilds(true);
}

// Local lights: points t19/u19 back at the placeholder, THEN destroys the pair -- never outlive.
// Idempotent.
void VoxiRenderer::releaseLocalHistory() {
    if (res_ && bindings_ && rdLocalHistPlaceholder_ && (rdLocalHist_[0] || rdLocalHist_[1])) {
        res_->setSrv(bindings_, 19, rdLocalHistPlaceholder_);
        res_->setUav(bindings_, 19, rdLocalHistPlaceholder_, 0);
    }
    for (rhi::TextureHandle& t : rdLocalHist_) { if (t && res_) res_->destroyTexture(t); t = 0; }
    rdLocalOutThisFrame_ = 0;
    rdLocalHistPrimed_ = false;
    rdLocalHistFrame_ = 0;
    rdKeyFrame_ = 0;
}

// (Re)creates the ray-traced shadow AND reflection histories at the given resolution. All four
// textures are destroyed and rebuilt together (mismatched sizes would corrupt reprojection).
bool VoxiRenderer::ensureShadowHistory(u32 width, u32 height) {
    if (!res_ || !bindings_ || width == 0 || height == 0) return false;

    // Ray tracing off (shipped default): release these four textures, go unused at Quality::Off.
    if (!rayTracingWanted()) {
        const bool had = rtShadowHist_[0] || rtReflHist_[0] || rtAoHist_[0] || giSurfPosHist_[0] ||
                          giVisHist_[0] || rdLocalHist_[0];
        // LOCAL LIGHTS (LAMPS): rdLocalHistWanted() requires rayTracingWanted(), so the pair goes too.
        releaseLocalHistory();
        for (rhi::TextureHandle& t : rtShadowHist_)  { if (t) res_->destroyTexture(t); t = 0; }
        for (rhi::TextureHandle& t : rtReflHist_)    { if (t) res_->destroyTexture(t); t = 0; }
        for (rhi::TextureHandle& t : rtAoHist_)      { if (t) res_->destroyTexture(t); t = 0; }
        for (rhi::TextureHandle& t : giSurfPosHist_) { if (t) res_->destroyTexture(t); t = 0; }
        for (rhi::TextureHandle& t : giSurfNrmHist_) { if (t) res_->destroyTexture(t); t = 0; }
        // U1/2.11: giVisHistWanted() requires giRestirWanted() requires rayTracingWanted().
        for (rhi::TextureHandle& t : giVisHist_)     { if (t) res_->destroyTexture(t); t = 0; }
        if (giRadiance_) { res_->destroyTexture(giRadiance_); giRadiance_ = 0; }
        if (rtAoHitDist_) { res_->destroyTexture(rtAoHitDist_); rtAoHitDist_ = 0; }
        if (giReservoirs_) { res_->destroyBuffer(giReservoirs_); giReservoirs_ = 0; }
        giReservoirElemCapacity_ = 0;
        giHistValid_ = false;
        giHistPrimed_ = false;
        giVisHistValid_ = false;
        giVisHistPrimed_ = false;
        rtShadowHistW_ = rtShadowHistH_ = 0;
        rtHistWriteIdx_ = 0;
        rtHistValid_ = false;
        rtHistPrimed_ = false;
        if (had) AVER_INFO("[Voxi] ray-traced history released; ray tracing is off");
        return true;
    }

    if (rtShadowHist_[0] && rtShadowHist_[1] && rtReflHist_[0] && rtReflHist_[1] &&
        // rtAoHitDist_ is in this test since the three are created together.
        (rtAoHist_[0] && rtAoHist_[1] && rtAoHitDist_) == aoHistoryWanted() &&
        (giSurfPosHist_[0] && giSurfPosHist_[1] && giSurfNrmHist_[0] && giSurfNrmHist_[1] &&
         giReservoirs_) == giRestirWanted() &&
        // U1/2.11: giVisHistWanted() is stricter than giRestirWanted().
        (giVisHist_[0] && giVisHist_[1]) == giVisHistWanted() &&
        (rdLocalHist_[0] && rdLocalHist_[1]) == rdLocalHistWanted() &&
        rtShadowHistW_ == width && rtShadowHistH_ == height)
        return true;

    for (rhi::TextureHandle& t : rtShadowHist_)  { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : rtReflHist_)    { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : rtAoHist_)      { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : giSurfPosHist_) { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : giSurfNrmHist_) { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : giVisHist_)     { if (t) res_->destroyTexture(t); t = 0; }
    releaseLocalHistory();
    if (giRadiance_) { res_->destroyTexture(giRadiance_); giRadiance_ = 0; }
    giHistValid_ = false;
    giHistPrimed_ = false;
    giVisHistValid_ = false;
    giVisHistPrimed_ = false;
    // Was missing here (present in the other two teardowns).
    if (rtAoHitDist_) { res_->destroyTexture(rtAoHitDist_); rtAoHitDist_ = 0; }
    rtHistValid_ = false;
    rtHistPrimed_ = false;

    rhi::TextureDesc d;
    d.dim    = rhi::TextureDim::Tex2D;
    d.width  = width;
    d.height = height;
    d.mips   = 1;
    // x = visibility, y = linear depth (view-space, cm).
    d.format = rhi::Format::RG32Float;
    d.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
    d.initialState = rhi::ResourceState::ShaderResource;   // where a texture rests between frames
    d.debugName = "Voxi RT shadow history A";
    rtShadowHist_[0] = res_->createTexture(d);
    d.debugName = "Voxi RT shadow history B";
    rtShadowHist_[1] = res_->createTexture(d);
    if (!rtShadowHist_[0] || !rtShadowHist_[1]) return false;

    // rgb = shaded reflection colour, a = hit depth OR miss sentinel. RGBA16F not 32F.
    d.format = rhi::Format::RGBA16F;
    d.debugName = "Voxi RT reflection history A";
    rtReflHist_[0] = res_->createTexture(d);
    d.debugName = "Voxi RT reflection history B";
    rtReflHist_[1] = res_->createTexture(d);
    if (!rtReflHist_[0] || !rtReflHist_[1]) return false;

    // LOCAL LIGHTS (LAMPS): lamp visibility history, same size/ping-pong as rtShadowHist_.
    if (rdLocalHistWanted()) {
        rhi::TextureDesc ld;
        ld.dim    = rhi::TextureDim::Tex2D;
        ld.width  = width;
        ld.height = height;
        ld.mips   = 1;
        ld.format = rhi::Format::RGBA16F;
        ld.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
        ld.initialState = rhi::ResourceState::ShaderResource;
        ld.debugName = "Voxi RD local-light history A";
        rdLocalHist_[0] = res_->createTexture(ld);
        ld.debugName = "Voxi RD local-light history B";
        rdLocalHist_[1] = res_->createTexture(ld);
        if (!rdLocalHist_[0] || !rdLocalHist_[1]) {
            releaseLocalHistory();
            if (!rdLocalHistFailLogged_) {
                rdLocalHistFailLogged_ = true;
                AVER_WARN("[Voxi] local-light history could not be created at {}x{}; lamps stay unlit "
                          "(said once)", width, height);
            }
        } else {
            rdLocalHistFailLogged_ = false;
        }
    }

    // Back to RG32Float: inheriting RGBA16F would give the depth channel only 8cm precision.
    if (aoHistoryWanted()) {
        d.format = rhi::Format::RG32Float;
        d.debugName = "Voxi RT sky-occlusion history A";
        rtAoHist_[0] = res_->createTexture(d);
        d.debugName = "Voxi RT sky-occlusion history B";
        rtAoHist_[1] = res_->createTexture(d);
        if (!rtAoHist_[0] || !rtAoHist_[1]) return false;

        // The raw hit distance, alongside the pair under the same condition (rtAoHitDist_).
        // Rests in UnorderedAccess and is never barriered, unlike the three pairs above.
        d.format = rhi::Format::R16Unorm;
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName = "Voxi RT sky-occlusion hit distance";
        rtAoHitDist_ = res_->createTexture(d);
        if (!rtAoHitDist_) return false;
        d.initialState = rhi::ResourceState::ShaderResource;   // restored for anything added below

    }

    // ---- The denoiser is created for either signal, not only for sky occlusion ----
    if (aoHistoryWanted() || giRestirWanted()) {
        // Created ONCE beside the signals it filters. False is not fatal, Voxi falls back to its hand-written filter.
        if (!denoiser_.valid()) denoiser_.create(*dev_);
    }

    // ---- ReSTIR GI: the previous-surface pair(s) and the reservoir buffer, together ----
    if (giRestirWanted()) {
        d.format = rhi::Format::RG32Float;
        d.debugName = "Voxi GI-restir surface position history A";
        giSurfPosHist_[0] = res_->createTexture(d);
        d.debugName = "Voxi GI-restir surface position history B";
        giSurfPosHist_[1] = res_->createTexture(d);
        if (!giSurfPosHist_[0] || !giSurfPosHist_[1]) return false;
        d.debugName = "Voxi GI-restir surface normal history A";
        giSurfNrmHist_[0] = res_->createTexture(d);
        d.debugName = "Voxi GI-restir surface normal history B";
        giSurfNrmHist_[1] = res_->createTexture(d);
        if (!giSurfNrmHist_[0] || !giSurfNrmHist_[1]) return false;
        d.initialState = rhi::ResourceState::ShaderResource;   // restored for anything added below

        // The radiance the denoiser filters. RGBA16F: rgb is linear HDR radiance (a unused).
        d.format = rhi::Format::RGBA16F;
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName = "Voxi ReSTIR GI radiance (to the denoiser)";
        giRadiance_ = res_->createTexture(d);
        if (!giRadiance_) return false;
        d.initialState = rhi::ResourceState::ShaderResource;

        // ---- U1/2.11: the sixth pair, the half-resolution ReSTIR VISIBILITY history (t16/u10) ----
        if (giVisHistWanted()) {
            const u32 vw = givis::halfDim(width), vh = givis::halfDim(height);
            rhi::TextureDesc vd;
            vd.dim    = rhi::TextureDim::Tex2D;
            vd.width  = vw;
            vd.height = vh;
            vd.mips   = 1;
            vd.format = rhi::Format::RGBA16F;
            vd.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
            vd.initialState = rhi::ResourceState::ShaderResource;
            vd.debugName = "Voxi ReSTIR visibility history A";
            giVisHist_[0] = res_->createTexture(vd);
            vd.debugName = "Voxi ReSTIR visibility history B";
            giVisHist_[1] = res_->createTexture(vd);
            if (!giVisHist_[0] || !giVisHist_[1]) {
                if (giVisHist_[0]) { res_->destroyTexture(giVisHist_[0]); giVisHist_[0] = 0; }
                if (giVisHist_[1]) { res_->destroyTexture(giVisHist_[1]); giVisHist_[1] = 0; }
                if (!giVisHistFailLogged_) {
                    giVisHistFailLogged_ = true;
                    AVER_WARN("[Voxi] ReSTIR half-resolution visibility history could not be created "
                              "at {}x{}; Half resolution runs as Full (said once)", vw, vh);
                }
            } else {
                giVisHistFailLogged_ = false;
                const f64 pairMiB = static_cast<f64>(vw) * static_cast<f64>(vh) * 8.0 * 2.0 /
                                   (1024.0 * 1024.0);
                AVER_INFO("[Voxi] ReSTIR half-resolution visibility history: 2 x {}x{} RGBA16F = "
                          "{:.1f} MiB", vw, vh, pairMiB);
            }
        } else if (giVisHist_[0]) {
            // giRestirVisibility left HalfResolution without the whole-history teardown at the top.
            res_->destroyTexture(giVisHist_[0]);
            res_->destroyTexture(giVisHist_[1]);
            giVisHist_[0] = giVisHist_[1] = 0;
        }

        // Reallocated only when it no longer fits or holds more than twice what this size needs.
        // This is the one GI-restir resource large enough to matter (32 bytes x two ping-pong copies x every pixel).
        const u32 elemCount = giReservoirElemCount(width, height);
        if (perPixelBufferNeedsRealloc(giReservoirElemCapacity_, elemCount)) {
            const u32 prevCapacity = giReservoirElemCapacity_;
            if (giReservoirs_) res_->destroyBuffer(giReservoirs_);
            rhi::BufferDesc bd;
            bd.bytes = static_cast<u64>(elemCount) * kGiReservoirElemBytes;
            bd.kind  = rhi::BufferKind::Default;
            bd.allowUnorderedAccess = true;
            bd.debugName = "Voxi ReSTIR GI reservoirs";
            giReservoirs_ = res_->createBuffer(bd);
            giReservoirElemCapacity_ = giReservoirs_ ? elemCount : 0;
            if (!giReservoirs_) return false;
            AVER_INFO("[Voxi] ReSTIR GI reservoir buffer: {} element(s) x {} B = {:.1f} MiB for {}x{} "
                      "(was {:.1f} MiB)", elemCount, kGiReservoirElemBytes,
                      static_cast<f64>(bd.bytes) / (1024.0 * 1024.0), width, height,
                      static_cast<f64>(prevCapacity) * kGiReservoirElemBytes / (1024.0 * 1024.0));
        }
    } else {
        // giMode (or ray tracing) was switched off without the whole-history teardown at the top.
        if (giReservoirs_) {
            res_->destroyBuffer(giReservoirs_);
            giReservoirs_ = 0;
            giReservoirElemCapacity_ = 0;
        }
        if (giVisHist_[0]) {
            res_->destroyTexture(giVisHist_[0]);
            res_->destroyTexture(giVisHist_[1]);
            giVisHist_[0] = giVisHist_[1] = 0;
        }
    }

    rtShadowHistW_ = width;
    rtShadowHistH_ = height;
    rtHistWriteIdx_ = 0;
    // A starting bind, not the steady state: beginShadowHistory rebinds whichever side is which.
    res_->setUav(bindings_, 2, rtShadowHist_[0], 0);
    res_->setSrv(bindings_, 6, rtShadowHist_[1]);
    res_->setUav(bindings_, 3, rtReflHist_[0], 0);
    res_->setSrv(bindings_, 7, rtReflHist_[1]);
    if (rtAoHist_[0]) {
        res_->setUav(bindings_, 4, rtAoHist_[0], 0);
        res_->setSrv(bindings_, 11, rtAoHist_[1]);
    }
    if (rtAoHitDist_) res_->setUav(bindings_, 5, rtAoHitDist_, 0);
    if (giSurfPosHist_[0]) {
        res_->setUav(bindings_, 7, giSurfPosHist_[0], 0);
        res_->setSrv(bindings_, 12, giSurfPosHist_[1]);
    }
    if (giSurfNrmHist_[0]) {
        res_->setUav(bindings_, 8, giSurfNrmHist_[0], 0);
        res_->setSrv(bindings_, 13, giSurfNrmHist_[1]);
    }
    if (giVisHist_[0]) {
        res_->setUav(bindings_, 10, giVisHist_[0], 0);
        res_->setSrv(bindings_, 16, giVisHist_[1]);
    }
    if (rdLocalHist_[0] && rdLocalHist_[1]) {
        res_->setUav(bindings_, 19, rdLocalHist_[0], 0);
        res_->setSrv(bindings_, 19, rdLocalHist_[1]);
    }
    // The reservoir StructuredBuffer is bound once, like rtAoHitDist_ above.
    if (giReservoirs_)
        res_->setUavBuffer(bindings_, 6, giReservoirs_, kGiReservoirElemBytes,
                           giReservoirElemCapacity_, 0);
    return true;
}

// Staged ray-driven passes: (re)creates or releases resources at the given size. Simpler than
// ensureShadowHistory above -- no ping-pong, every texture recreated outright on a size change.
// u23 to a 1x1 stand-in: CSRdRefl reads its size as "not denoising" and falls back.
bool VoxiRenderer::bindReflDnPlaceholder() {
    if (!res_ || !bindings_) return false;
    if (!rdReflDnPlaceholder_) {
        rhi::TextureDesc pd;
        pd.dim    = rhi::TextureDim::Tex2D;
        pd.width  = 1; pd.height = 1; pd.mips = 1;
        pd.format = rhi::Format::RGBA16F;
        pd.bind   = rhi::ResourceBind::UnorderedAccess;
        pd.initialState = rhi::ResourceState::UnorderedAccess;
        pd.debugName    = "Voxi reflection denoiser input placeholder";
        rdReflDnPlaceholder_ = res_->createTexture(pd);
        if (!rdReflDnPlaceholder_) return false;
    }
    res_->setUav(bindings_, 23, rdReflDnPlaceholder_, 0);
    return true;
}

// NRD2 asked for and possible this frame: a staged ray-driven D3D12 frame with its G-buffer, one sample
// a pixel. Not Reference path tracing, which keeps its own accumulation and stays undenoised.
void VoxiRenderer::startNrd2Capture(const render::denoise::Nrd2CaptureConfig& cfg) {
    nrd2CaptureCfg_ = cfg;
    nrd2CapturePending_ = true;
    if (denoiserMode(settings_) != 2u)
        AVER_WARN("[NRD2] capture requested but the Denoiser setting is not NRD2 (--denoiser 2); it waits for NRD2");
}

bool VoxiRenderer::nrd2Wanted() const {
    return settings_.denoiser && settings_.denoiserKind == 2u && !ptReferenceWanted() && dev_ && res_ &&
           bindings_ && dev_->backend() == rhi::Backend::D3D12 && dev_->sampleCount() == 1 &&
           rayDrivenActive() && rdStagedWanted() && !debugViewActive() && rdStagedW_ && rdStagedH_ &&
           rayDrivenSplitTexGbufPso_ != 0;
}

// Builds, and waits for, the variants otherwise made on first use: NRD2 (its pipelines, the compose draw and
// Stage B's AVER_NRD2 variant), the Path Tracing twins and the NeuRaC twins. Outside a frame only.
void VoxiRenderer::buildAllVariants() {
    if (!res_ || !initialised_) return;
    lazyRequested_ |= kPsoRc | kPsoPt | kPsoNrd2;
    pumpBuilds(true);
}

// NRD2 asked for and the frame can use it: built (once per scene set), then sized. False: this frame runs
// without it (FidelityFX if it is valid), including while its pipelines are still building.
bool VoxiRenderer::ensureNrd2() {
    if (!nrd2Tried_) {
        requestBuild(kPsoNrd2);   // synchronous mode has built and adopted it when this returns
        if (!nrd2Tried_) return false;
    }
    if (buildInFlight(kPsoNrd2)) return false;
    if (!nrd2_.valid() || !nrd2_.composeValid() || !rayDrivenSplitNrd2Pso_) {
        if (!nrd2FallbackLogged_) {
            nrd2FallbackLogged_ = true;
            AVER_WARN("[Voxi] NRD2 is unavailable ({}); the denoiser setting runs FidelityFX instead (said once)",
                      !nrd2_.valid() ? "its compute passes did not build"
                      : !nrd2_.composeValid() ? "its compose draw did not build"
                                              : "Stage B's AVER_NRD2 variant did not compile");
        }
        return false;
    }
    // The G-buffer it reads must match the staged targets (a resize lands on both within a frame).
    rhi::TextureDesc gz{};
    const rhi::TextureHandle vz = dev_->gBufferViewZTexture();
    if (!vz || !dev_->gBufferNormalRoughnessTexture() || !res_->textureInfo(vz, gz) ||
        gz.width != rdStagedW_ || gz.height != rdStagedH_)
        return false;
    return nrd2_.resize(rdStagedW_, rdStagedH_);
}

// Stage B's NRD2 targets into the table-0 slots an NRD2 frame leaves unused (voxi.hlsl's gNrd2*Out):
// u2 and u3 (shadow/reflection history writes, off with rtHistParams.x 0), u9 (FidelityFX's GI input;
// CSRdGi skips its write there under NRD2) and u23 (CSRdRefl's raw sample, then S).
void VoxiRenderer::bindNrd2Targets() {
    const render::denoise::Nrd2::Targets& t = nrd2_.targets();
    res_->setUav(bindings_, 2, t.remodB, 0);
    res_->setUav(bindings_, 3, t.diffuse, 0);
    res_->setUav(bindings_, 9, t.remodA, 0);
    res_->setUav(bindings_, 23, t.specular, 0);
    rdReflDnBound_ = false;   // u23 is not rdReflDnIn_ this frame
    nrd2Bound_ = true;
}

bool VoxiRenderer::ensureRdStagedResources(u32 width, u32 height) {
    if (!res_ || !bindings_) return false;

    // Not wanted, or nothing to size against yet: release every resource and fall back to placeholders.
    if (!rdStagedResourcesWanted() || width == 0 || height == 0) {
        const bool had = rdVisBuf_ != 0 || rdSunVisTex_ != 0 || rdGiTex_ != 0 || rdAoTex_ != 0 ||
                         rdReflTex_ != 0 || rdGiCandBuf_ != 0 || rdShadowTileBuf_ != 0;
        if (had) {
            if (!rdVisBufPlaceholder_) {
                rhi::BufferDesc pd;
                pd.bytes = kRdVisElemBytes;
                pd.kind  = rhi::BufferKind::Default;
                pd.allowUnorderedAccess = true;
                pd.debugName = "Voxi ray-driven visibility buffer placeholder";
                rdVisBufPlaceholder_ = res_->createBuffer(pd);
            }
            if (!rdSunVisPlaceholder_) {
                rhi::TextureDesc pd;
                pd.dim    = rhi::TextureDim::Tex2D;
                pd.width  = 1; pd.height = 1; pd.mips = 1;
                pd.format = rhi::Format::RGBA16F;
                pd.bind   = rhi::ResourceBind::UnorderedAccess;
                pd.initialState = rhi::ResourceState::UnorderedAccess;
                pd.debugName    = "Voxi ray-driven sun visibility placeholder";
                rdSunVisPlaceholder_ = res_->createTexture(pd);
            }
            if (!rdGiPlaceholder_) {
                rhi::TextureDesc pd;
                pd.dim    = rhi::TextureDim::Tex2D;
                pd.width  = 1; pd.height = 1; pd.mips = 1;
                pd.format = rhi::Format::RGBA16F;
                pd.bind   = rhi::ResourceBind::UnorderedAccess;
                pd.initialState = rhi::ResourceState::UnorderedAccess;
                pd.debugName    = "Voxi ray-driven GI indirect diffuse placeholder";
                rdGiPlaceholder_ = res_->createTexture(pd);
            }
            if (!rdAoPlaceholder_) {
                rhi::TextureDesc pd;
                pd.dim    = rhi::TextureDim::Tex2D;
                pd.width  = 1; pd.height = 1; pd.mips = 1;
                pd.format = rhi::Format::RGBA16F;
                pd.bind   = rhi::ResourceBind::UnorderedAccess;
                pd.initialState = rhi::ResourceState::UnorderedAccess;
                pd.debugName    = "Voxi ray-driven sky occlusion placeholder";
                rdAoPlaceholder_ = res_->createTexture(pd);
            }
            if (!rdReflPlaceholder_) {
                rhi::TextureDesc pd;
                pd.dim    = rhi::TextureDim::Tex2D;
                pd.width  = 1; pd.height = 1; pd.mips = 1;
                pd.format = rhi::Format::RGBA16F;
                pd.bind   = rhi::ResourceBind::UnorderedAccess;
                pd.initialState = rhi::ResourceState::UnorderedAccess;
                pd.debugName    = "Voxi ray-driven reflection placeholder";
                rdReflPlaceholder_ = res_->createTexture(pd);
            }
            if (!rdGiCandBufPlaceholder_) {
                rhi::BufferDesc pd;
                pd.bytes = kRdGiCandElemBytes;
                pd.kind  = rhi::BufferKind::Default;
                pd.allowUnorderedAccess = true;
                pd.debugName = "Voxi ray-driven GI-trace candidate buffer placeholder";
                rdGiCandBufPlaceholder_ = res_->createBuffer(pd);
            }
            if (!rdShadowTileBufPlaceholder_) {
                rhi::BufferDesc pd;
                pd.bytes = kRdShadowTileElemBytes;
                pd.kind  = rhi::BufferKind::Default;
                pd.allowUnorderedAccess = true;
                pd.debugName = "Voxi ray-driven shadow-probe tile buffer placeholder";
                rdShadowTileBufPlaceholder_ = res_->createBuffer(pd);
            }
            // Rebind before destroy (aver-view-outlives-its-buffer).
            if (rdVisBufPlaceholder_) res_->setUavBuffer(bindings_, 11, rdVisBufPlaceholder_, kRdVisElemBytes, 1, 0);
            if (rdSunVisPlaceholder_) res_->setUav(bindings_, 12, rdSunVisPlaceholder_, 0);
            if (rdGiPlaceholder_)     res_->setUav(bindings_, 13, rdGiPlaceholder_, 0);
            if (rdAoPlaceholder_)     res_->setUav(bindings_, 14, rdAoPlaceholder_, 0);
            if (rdReflPlaceholder_)   res_->setUav(bindings_, 15, rdReflPlaceholder_, 0);
            if (rdGiCandBufPlaceholder_)     res_->setUavBuffer(bindings_, 17, rdGiCandBufPlaceholder_, kRdGiCandElemBytes, 1, 0);
            if (rdShadowTileBufPlaceholder_) res_->setUavBuffer(bindings_, 18, rdShadowTileBufPlaceholder_, kRdShadowTileElemBytes, 1, 0);
            if (rdVisBuf_)    { res_->destroyBuffer(rdVisBuf_);  rdVisBuf_ = 0; }
            if (rdSunVisTex_) { res_->destroyTexture(rdSunVisTex_); rdSunVisTex_ = 0; }
            if (rdGiTex_)     { res_->destroyTexture(rdGiTex_);     rdGiTex_ = 0; }
            if (rdAoTex_)     { res_->destroyTexture(rdAoTex_);     rdAoTex_ = 0; }
            if (rdReflTex_)   { res_->destroyTexture(rdReflTex_);   rdReflTex_ = 0; }
            if (rdReflDnIn_) {
                if (bindReflDnPlaceholder()) rdReflDnBound_ = false;
                res_->destroyTexture(rdReflDnIn_);
                rdReflDnIn_ = 0;
            }
            if (rdGiCandBuf_)     { res_->destroyBuffer(rdGiCandBuf_);     rdGiCandBuf_ = 0; }
            if (rdShadowTileBuf_) { res_->destroyBuffer(rdShadowTileBuf_); rdShadowTileBuf_ = 0; }
            rdVisBufElemCapacity_ = 0;
            rdGiCandBufElemCapacity_ = rdShadowTileElemCapacity_ = 0;
            rdStagedW_ = rdStagedH_ = rdStagedRowPitch_ = 0;
            AVER_INFO("[Voxi] staged ray-driven resources released");
        }
        return ensurePtAccum();
    }

    // Row pitch is exactly the render target's width. Every resource already matches -- nothing to do.
    const u32 pitch = width;
    const u32 elemCount = pitch * height;
    // Tile count for rdShadowTileBuf_: one uint per 8x8 tile of the full render target.
    const u32 tilesX = (width + 7u) / 8u;
    const u32 tilesY = (height + 7u) / 8u;
    const u32 tileElemCount = tilesX * tilesY;
    if (rdStagedW_ == width && rdStagedH_ == height && rdSunVisTex_ && rdGiTex_ && rdAoTex_ &&
        rdReflTex_ && !perPixelBufferNeedsRealloc(rdVisBufElemCapacity_, elemCount) &&
        !perPixelBufferNeedsRealloc(rdGiCandBufElemCapacity_, elemCount) &&
        !perPixelBufferNeedsRealloc(rdShadowTileElemCapacity_, tileElemCount))
        return ensurePtAccum();

    // Texture resizes recreate the texture outright (unlike buffers, which can grow).
    if (rdSunVisTex_ && (rdStagedW_ != width || rdStagedH_ != height)) {
        res_->destroyTexture(rdSunVisTex_);
        rdSunVisTex_ = 0;
    }
    if (!rdSunVisTex_) {
        // UAV only, no SRV binding.
        rhi::TextureDesc d;
        d.dim    = rhi::TextureDim::Tex2D;
        d.width  = width;
        d.height = height;
        d.mips   = 1;
        d.format = rhi::Format::RGBA16F;
        d.bind   = rhi::ResourceBind::UnorderedAccess;
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName    = "Voxi ray-driven sun visibility";
        rdSunVisTex_ = res_->createTexture(d);
        if (!rdSunVisTex_) return false;
        res_->setUav(bindings_, 12, rdSunVisTex_, 0);
    }

    // GI and AO textures: recreate on size change; UAV only.
    if (rdGiTex_ && (rdStagedW_ != width || rdStagedH_ != height)) {
        res_->destroyTexture(rdGiTex_);
        rdGiTex_ = 0;
    }
    if (!rdGiTex_) {
        rhi::TextureDesc d;
        d.dim    = rhi::TextureDim::Tex2D;
        d.width  = width;
        d.height = height;
        d.mips   = 1;
        d.format = rhi::Format::RGBA16F;
        d.bind   = rhi::ResourceBind::UnorderedAccess;
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName    = "Voxi ray-driven GI indirect diffuse";
        rdGiTex_ = res_->createTexture(d);
        if (!rdGiTex_) return false;
        res_->setUav(bindings_, 13, rdGiTex_, 0);
    }
    if (rdAoTex_ && (rdStagedW_ != width || rdStagedH_ != height)) {
        res_->destroyTexture(rdAoTex_);
        rdAoTex_ = 0;
    }
    if (!rdAoTex_) {
        rhi::TextureDesc d;
        d.dim    = rhi::TextureDim::Tex2D;
        d.width  = width;
        d.height = height;
        d.mips   = 1;
        d.format = rhi::Format::RGBA16F;
        d.bind   = rhi::ResourceBind::UnorderedAccess;
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName    = "Voxi ray-driven sky occlusion";
        rdAoTex_ = res_->createTexture(d);
        if (!rdAoTex_) return false;
        res_->setUav(bindings_, 14, rdAoTex_, 0);
    }

    // Reflection texture: recreate on size change; UAV only.
    if (rdReflTex_ && (rdStagedW_ != width || rdStagedH_ != height)) {
        res_->destroyTexture(rdReflTex_);
        rdReflTex_ = 0;
    }
    if (!rdReflTex_) {
        rhi::TextureDesc d;
        d.dim    = rhi::TextureDim::Tex2D;
        d.width  = width;
        d.height = height;
        d.mips   = 1;
        d.format = rhi::Format::RGBA16F;
        d.bind   = rhi::ResourceBind::UnorderedAccess;
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName    = "Voxi ray-driven reflection";
        rdReflTex_ = res_->createTexture(d);
        if (!rdReflTex_) return false;
        res_->setUav(bindings_, 15, rdReflTex_, 0);
    }
    // The reflection denoiser's input: SRV too, the denoiser reads it next frame. Bound per frame.
    if (rdReflDnIn_ && (rdStagedW_ != width || rdStagedH_ != height)) {
        if (rdReflDnBound_ && bindReflDnPlaceholder()) rdReflDnBound_ = false;
        res_->destroyTexture(rdReflDnIn_);
        rdReflDnIn_ = 0;
    }
    if (!rdReflDnIn_) {
        rhi::TextureDesc d;
        d.dim    = rhi::TextureDim::Tex2D;
        d.width  = width;
        d.height = height;
        d.mips   = 1;
        d.format = rhi::Format::RGBA16F;
        d.bind   = static_cast<rhi::ResourceBind>(static_cast<u32>(rhi::ResourceBind::ShaderResource) |
                                                  static_cast<u32>(rhi::ResourceBind::UnorderedAccess));
        d.initialState = rhi::ResourceState::UnorderedAccess;
        d.debugName    = "Voxi reflection denoiser input";
        rdReflDnIn_ = res_->createTexture(d);   // optional: without it reflections stay undenoised
    }

    // Visibility buffer: reallocated only when capacity no longer fits or overshoots by >2x.
    if (perPixelBufferNeedsRealloc(rdVisBufElemCapacity_, elemCount)) {
        const u32 prevCapacity = rdVisBufElemCapacity_;
        if (rdVisBuf_) res_->destroyBuffer(rdVisBuf_);
        rhi::BufferDesc bd;
        bd.bytes = static_cast<u64>(elemCount) * kRdVisElemBytes;
        bd.kind  = rhi::BufferKind::Default;
        bd.allowUnorderedAccess = true;
        bd.debugName = "Voxi ray-driven visibility records";
        rdVisBuf_ = res_->createBuffer(bd);
        rdVisBufElemCapacity_ = rdVisBuf_ ? elemCount : 0;
        if (!rdVisBuf_) return false;
        AVER_INFO("[Voxi] ray-driven visibility record buffer: {} element(s) x {} B = {:.1f} MiB for "
                  "{}x{} (row pitch {}; was {:.1f} MiB)", elemCount, kRdVisElemBytes,
                  static_cast<f64>(bd.bytes) / (1024.0 * 1024.0), width, height, pitch,
                  static_cast<f64>(prevCapacity) * kRdVisElemBytes / (1024.0 * 1024.0));
    }
    res_->setUavBuffer(bindings_, 11, rdVisBuf_, kRdVisElemBytes, rdVisBufElemCapacity_, 0);

    // Sub-stage split buffers: allocated unconditionally.
    if (perPixelBufferNeedsRealloc(rdGiCandBufElemCapacity_, elemCount)) {
        const u32 prevCapacity = rdGiCandBufElemCapacity_;
        if (rdGiCandBuf_) res_->destroyBuffer(rdGiCandBuf_);
        rhi::BufferDesc bd;
        bd.bytes = static_cast<u64>(elemCount) * kRdGiCandElemBytes;
        bd.kind  = rhi::BufferKind::Default;
        bd.allowUnorderedAccess = true;
        bd.debugName = "Voxi ray-driven GI-trace candidates";
        rdGiCandBuf_ = res_->createBuffer(bd);
        rdGiCandBufElemCapacity_ = rdGiCandBuf_ ? elemCount : 0;
        if (!rdGiCandBuf_) return false;
        AVER_INFO("[Voxi] ray-driven GI-trace candidate buffer: {} element(s) x {} B = {:.1f} MiB for "
                  "{}x{} (row pitch {}; was {:.1f} MiB)", elemCount, kRdGiCandElemBytes,
                  static_cast<f64>(bd.bytes) / (1024.0 * 1024.0), width, height, pitch,
                  static_cast<f64>(prevCapacity) * kRdGiCandElemBytes / (1024.0 * 1024.0));
    }
    res_->setUavBuffer(bindings_, 17, rdGiCandBuf_, kRdGiCandElemBytes, rdGiCandBufElemCapacity_, 0);

    if (perPixelBufferNeedsRealloc(rdShadowTileElemCapacity_, tileElemCount)) {
        if (rdShadowTileBuf_) res_->destroyBuffer(rdShadowTileBuf_);
        rhi::BufferDesc bd;
        bd.bytes = static_cast<u64>(tileElemCount) * kRdShadowTileElemBytes;
        bd.kind  = rhi::BufferKind::Default;
        bd.allowUnorderedAccess = true;
        bd.debugName = "Voxi ray-driven shadow-probe tiles";
        rdShadowTileBuf_ = res_->createBuffer(bd);
        rdShadowTileElemCapacity_ = rdShadowTileBuf_ ? tileElemCount : 0;
        if (!rdShadowTileBuf_) return false;
        AVER_INFO("[Voxi] ray-driven shadow-probe tile buffer: {} tile(s) x {} B = {:.1f} KiB for "
                  "{}x{} ({}x{} tiles)", tileElemCount, kRdShadowTileElemBytes,
                  static_cast<f64>(bd.bytes) / 1024.0, width, height, tilesX, tilesY);
    }
    res_->setUavBuffer(bindings_, 18, rdShadowTileBuf_, kRdShadowTileElemBytes, rdShadowTileElemCapacity_, 0);

    rdStagedW_ = width;
    rdStagedH_ = height;
    rdStagedRowPitch_ = pitch;
    return ensurePtAccum();
}

// Path Tracing's accumulation buffer (u22): two float4 planes per staged pixel while Path Tracing is
// wanted (the composed colour uses the first; under NRD2, D the first and S the second), released otherwise. Called wherever the staged resources are sized, and on a Path Tracing edge.
bool VoxiRenderer::ensurePtAccum() {
    if (!res_ || !bindings_) return false;
    const bool want = pathTracingWanted() && rdStagedResourcesWanted() && rdStagedRowPitch_ && rdStagedH_;
    const u32 elems = want ? 2u * rdStagedRowPitch_ * rdStagedH_ : 0u;
    if (ptAccumBuf_ && (!want || ptAccumElemCapacity_ < elems)) {
        // Rebind before destroy (aver-view-outlives-its-buffer).
        if (ptAccumPlaceholder_) res_->setUavBuffer(bindings_, 22, ptAccumPlaceholder_, kPtAccumElemBytes, 1, 0);
        res_->destroyBuffer(ptAccumBuf_);
        ptAccumBuf_ = 0;
        ptAccumElemCapacity_ = 0;
    }
    ptAccumValid_ = ptAccumValid_ && ptAccumBuf_ != 0;
    if (!want || ptAccumBuf_) return true;
    rhi::BufferDesc d;
    d.bytes = static_cast<u64>(elems) * kPtAccumElemBytes;
    d.kind  = rhi::BufferKind::Default;
    d.allowUnorderedAccess = true;
    d.debugName = "Voxi Path Tracing accumulation";
    ptAccumBuf_ = res_->createBuffer(d);
    if (!ptAccumBuf_) {
        AVER_WARN("[Voxi] Path Tracing accumulation buffer ({} pixels) could not be created; the image "
                  "will not accumulate", elems);
        return false;
    }
    ptAccumElemCapacity_ = elems;
    ptAccumValid_ = false;
    res_->setUavBuffer(bindings_, 22, ptAccumBuf_, kPtAccumElemBytes, elems, 0);
    return true;
}

// See VoxiRenderer.hpp for the ping-pong rationale.
void VoxiRenderer::beginShadowHistory(rhi::IRenderContext& ctx) {
    // Latch GI signals before anything else (state persists into this frame's denoise dispatch).
    denoiseGiInputHalfRate_  = giCbWrittenThisFrame_;
    denoiseGiHalfRateParity_ = giCbParityWritten_;
    giCbWrittenThisFrame_ = false;
    ++denoiseFrame_;
    rhi::ScopedGpuStat historyStat(ctx, "Voxi shadow history");
    // Clear all slots before the early return (unbound slots must not carry stale data).
    cb_.rtHistParams[0] = 0.0f;
    cb_.rtHistParams[1] = 0.0f;
    cb_.rtDenoiseParams[3] = 0.0f;
    cb_.giRestirParams[0] = 0.0f;
    rdLocalOutThisFrame_ = 0;
    nrd2Frame_ = false;
    // Poison-view flag published here so it reaches all giMode values.
    cb_.giRestirParams[3] = giPoisonView_ ? 1.0f : 0.0f;
    // Packed ambient-param word, recomputed each frame with current visibility state.
    const u32 wireVisMode = giRestirVisibility_ == 4u ? 2u : giRestirVisibility_;
    const u32 ambW = givis::packAmbientW(wireVisMode, /*histBound=*/false, /*histValid=*/false,
                                         blendedGiCone_, dev_ && dev_->backend() == rhi::Backend::D3D12,
                                         giVisPathView_, giRestirSpatialSamples_, giRestirMaxHistory_,
                                         /*neurac=*/neuracLive_, neuracLive_ ? neuracView_ : 0u);
    cb_.ambientParams[3] = static_cast<f32>(ambW);
    if (!shadowHistoryActive()) {
        // Skipped frame: clear history validity to prevent stale reprojection.
        rtHistValid_ = false;
        giHistValid_ = false;
        giVisHistValid_ = false;
        denoiser_.forceHistoryReset();
        return;
    }

    const u32 writeIdx = rtHistWriteIdx_;
    const u32 readIdx  = 1 - writeIdx;

    // Transition history textures: write side to UnorderedAccess, read side (if primed) to ShaderResource.
    ctx.textureBarrier(rtShadowHist_[writeIdx], rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    ctx.textureBarrier(rtReflHist_[writeIdx],   rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    if (rtAoHist_[writeIdx]) ctx.textureBarrier(rtAoHist_[writeIdx], rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    if (rtHistPrimed_) {
        ctx.textureBarrier(rtShadowHist_[readIdx], rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
        ctx.textureBarrier(rtReflHist_[readIdx],   rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
        if (rtAoHist_[readIdx]) ctx.textureBarrier(rtAoHist_[readIdx], rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
    }

    res_->setUav(bindings_, 2, rtShadowHist_[writeIdx], 0);
    res_->setSrv(bindings_, 6, rtShadowHist_[readIdx]);
    res_->setUav(bindings_, 3, rtReflHist_[writeIdx], 0);
    res_->setSrv(bindings_, 7, rtReflHist_[readIdx]);
    // Ambient pair: only when all three resources exist (created together).
    if (rtAoHist_[writeIdx] && rtAoHist_[readIdx] && rtAoHitDist_) {
        res_->setUav(bindings_, 4, rtAoHist_[writeIdx], 0);
        res_->setSrv(bindings_, 11, rtAoHist_[readIdx]);
        cb_.rtDenoiseParams[3] = 1.0f;
    }

    // Local lights: same swap pattern as shadow pair.
    if (rdLocalHist_[writeIdx] && rdLocalHist_[readIdx]) {
        ctx.textureBarrier(rdLocalHist_[writeIdx], rhi::ResourceState::ShaderResource,
                           rhi::ResourceState::UnorderedAccess);
        if (rdLocalHistPrimed_)
            ctx.textureBarrier(rdLocalHist_[readIdx], rhi::ResourceState::UnorderedAccess,
                               rhi::ResourceState::ShaderResource);
        res_->setUav(bindings_, 19, rdLocalHist_[writeIdx], 0);
        res_->setSrv(bindings_, 19, rdLocalHist_[readIdx]);
        rdLocalOutThisFrame_ = rdLocalHist_[writeIdx];
    }

    // ReSTIR GI: fourth/fifth pair, swapped together, separate validity.
    if (giSurfPosHist_[writeIdx] && giSurfPosHist_[readIdx] &&
        giSurfNrmHist_[writeIdx] && giSurfNrmHist_[readIdx]) {
        ctx.textureBarrier(giSurfPosHist_[writeIdx], rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
        ctx.textureBarrier(giSurfNrmHist_[writeIdx], rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
        if (giHistPrimed_) {
            ctx.textureBarrier(giSurfPosHist_[readIdx], rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
            ctx.textureBarrier(giSurfNrmHist_[readIdx], rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
        }
        res_->setUav(bindings_, 7, giSurfPosHist_[writeIdx], 0);
        res_->setSrv(bindings_, 12, giSurfPosHist_[readIdx]);
        res_->setUav(bindings_, 8, giSurfNrmHist_[writeIdx], 0);
        res_->setSrv(bindings_, 13, giSurfNrmHist_[readIdx]);
        cb_.giRestirParams[0] = 1.0f;
        // Sun move voids reservoirs for the frame it moves (radiance from old sun would carry forward).
        cb_.giRestirParams[1] = (giHistValid_ && !rtHistSunJumped()) ? 1.0f : 0.0f;
        cb_.giRestirParams[2] = static_cast<f32>(writeIdx);
        if (giRadiance_) res_->setUav(bindings_, 9, giRadiance_, 0);

        // Sixth pair (visibility history): separate condition, own primed flag.
        if (giVisHist_[writeIdx] && giVisHist_[readIdx]) {
            ctx.textureBarrier(giVisHist_[writeIdx], rhi::ResourceState::ShaderResource,
                               rhi::ResourceState::UnorderedAccess);
            if (giVisHistPrimed_)
                ctx.textureBarrier(giVisHist_[readIdx], rhi::ResourceState::UnorderedAccess,
                                   rhi::ResourceState::ShaderResource);
            res_->setUav(bindings_, 10, giVisHist_[writeIdx], 0);
            res_->setSrv(bindings_, 16, giVisHist_[readIdx]);
            // Recompute ambientParams.w with histBound/histValid now known.
            const u32 ambW2 = givis::packAmbientW(wireVisMode, /*histBound=*/true,
                                                  giVisHistValid_, blendedGiCone_,
                                                  dev_ && dev_->backend() == rhi::Backend::D3D12,
                                                  giVisPathView_, giRestirSpatialSamples_,
                                                  giRestirMaxHistory_,
                                                  /*neurac=*/neuracLive_, neuracLive_ ? neuracView_ : 0u);
            cb_.ambientParams[3] = static_cast<f32>(ambW2);
        }
    }

    // Denoiser dispatch on LAST frame's signals (rtAoHitDist_ and giRadiance_ not yet written).
    denoiseAoOutput_ = 0;
    denoiseGiOutput_ = 0;
    denoiseGiRanThisFrame_ = false;
    bool denoiseRecorded = false;
    // Under MSAA the backend resolves its multisampled G-buffer; one that cannot reports it unwritten.
    const bool gbufWritten = dev_ && dev_->gBufferWritten();
    // NRD2 replaces FidelityFX and every Voxi history for the frame (docs/rendering/NRD2.md).
    nrd2Frame_ = nrd2Wanted() && gbufWritten && ensureNrd2();
    if (dev_ && dev_->gBufferEnabled() && !gbufWritten && !denoiseWarnedMsaa_) {
        AVER_WARN("[Denoise] denoising is OFF: the G-buffer is enabled but this backend cannot write it "
                  "at MSAA {}x. Set MSAA to 1 (voxi.msaa 1) to denoise.", dev_->sampleCount());
        denoiseWarnedMsaa_ = true;
    }
    // Skip AO signal when nothing reads it (under ray-driven primary visibility).
    const bool denoiseGiSignal = giRadiance_ && giRestirWanted() && !ptReferenceWanted();
    const bool denoiseAoSignal = rtAoHitDist_ != 0 &&
                                 (!rayDrivenActive() || (lightingLegacyBits_ & 32u) != 0u);
    // Reflections: last frame's CSRdRefl wrote rdReflDnIn_ (u23 was bound to it).
    // ONE FidelityFX decision per frame: dispatch below, and whether this frame's CSRdRefl feeds it (u23).
    // Denoiser None runs no denoiser: the G-buffer can still be on for TAA or frame interpolation.
    const bool ffxFrame = !nrd2Frame_ && settings_.denoiser && denoiser_.valid() && gbufWritten;
    const bool reflDnWanted = ffxFrame && settings_.denoiseReflections && rdReflDnIn_ != 0;
    const bool denoiseReflSignal = reflDnWanted && rdReflDnBound_;
    denoiseReflOutput_ = 0;
    if (ffxFrame && (denoiseAoSignal || denoiseGiSignal || denoiseReflSignal)) {
        render::denoise::Denoiser::Inputs in;
        in.viewZ           = dev_->gBufferViewZTexture();
        in.motionVectors   = dev_->gBufferVelocityTexture();
        in.normalRoughness = dev_->gBufferNormalRoughnessTexture();
        in.gbufferState    = rhi::ResourceState::RenderTarget;
        in.occlusion   = denoiseAoSignal ? rtAoHitDist_ : 0;
        in.radiance    = denoiseGiSignal ? giRadiance_ : 0;
        in.signalState = rhi::ResourceState::UnorderedAccess;
        rhi::TextureDesc sizeDesc{};
        const rhi::TextureHandle sizeFrom = rtAoHitDist_ ? rtAoHitDist_ : giRadiance_ ? giRadiance_ : rdReflDnIn_;
        const bool haveSize = res_->textureInfo(sizeFrom, sizeDesc);
        const u32 tw = haveSize ? sizeDesc.width : 0u, th = haveSize ? sizeDesc.height : 0u;

        // Denoiser rect vs signal/G-buffer size mismatch guard (render-scale changes between frames).
        rhi::TextureDesc gzDesc{}, gmvDesc{}, gnrDesc{};
        const bool haveGbufSizes = in.viewZ && in.motionVectors && in.normalRoughness &&
                                   res_->textureInfo(in.viewZ, gzDesc) &&
                                   res_->textureInfo(in.motionVectors, gmvDesc) &&
                                   res_->textureInfo(in.normalRoughness, gnrDesc);
        const bool gbufSizeOk = haveSize && haveGbufSizes &&
                                gzDesc.width == tw && gzDesc.height == th &&
                                gmvDesc.width == tw && gmvDesc.height == th &&
                                gnrDesc.width == tw && gnrDesc.height == th;
        // Warned once per mismatch episode; clears when sizes agree again.
        if (haveSize && haveGbufSizes && !gbufSizeOk) {
            if (!denoiseWarnedInputSizeMismatch_) {
                denoiseWarnedInputSizeMismatch_ = true;
                AVER_WARN("[Denoise] input size mismatch ({}x{} signal vs {}x{} G-buffer); skipping this "
                          "frame", tw, th, gzDesc.width, gzDesc.height);
            }
        } else if (gbufSizeOk) {
            denoiseWarnedInputSizeMismatch_ = false;
        }
        rhi::TextureDesc reflDesc{};
        const bool reflSizeOk = denoiseReflSignal && res_->textureInfo(rdReflDnIn_, reflDesc) &&
                                reflDesc.width == tw && reflDesc.height == th;
        if (reflSizeOk) {
            in.reflection = rdReflDnIn_;
            std::memcpy(in.invViewProjRel, prevInvViewProjRel_, sizeof(in.invViewProjRel));
            std::memcpy(in.prevViewProj, prev2ViewProj_, sizeof(in.prevViewProj));
            std::memcpy(in.camPos, prevCamPos_, sizeof(in.camPos));
            std::memcpy(in.viewport, prevSceneViewport_, sizeof(in.viewport));
            std::memcpy(in.prevViewport, prev2SceneViewport_, sizeof(in.prevViewport));
        }
        if (gbufSizeOk && tw && th && denoiser_.resize(tw, th)) {
            render::denoise::Denoiser::Frame frame;
            frame.runReflection = reflSizeOk;
            frame.runOcclusion = denoiseAoSignal;
            frame.runRadiance  = denoiseGiSignal;
            frame.resetHistory = !rtHistValid_;
            frame.radianceHalfRate       = denoiseGiSignal && denoiseGiInputHalfRate_;
            frame.radianceHalfRateParity = denoiseGiHalfRateParity_;
            // Clamp denoiser history during sun movement.
            if (rtHistSunMoved()) denoiseSunMovingHold_ = 2u;
            else if (denoiseSunMovingHold_ > 0u) --denoiseSunMovingHold_;
            render::denoise::Denoiser::Tuning tuning;
            tuning.maxSamples        = settings_.denoiserMaxSamples;
            tuning.historyClipWeight = settings_.denoiserHistoryClipWeight;
            if (denoiseSunMovingHold_ > 0u)
                tuning.maxSamples = std::min(tuning.maxSamples, settings_.denoiserSunMovingSamples);
            denoiser_.setTuning(tuning);
            rhi::ScopedGpuStat denoiseStat(ctx, "Voxi denoise");
            if (denoiser_.record(ctx, frame, in)) {
                denoiseRecorded = true;
                denoiseAoOutput_ = denoiser_.output(render::denoise::Signal::Occlusion);
                denoiseGiOutput_ = denoiser_.output(render::denoise::Signal::Radiance);
                denoiseGiRanThisFrame_ = denoiseGiOutput_ != 0;
                denoiseReflOutput_ = denoiser_.output(render::denoise::Signal::Reflection);
            }
        }
    }
    // Denoiser history reset on skipped dispatch (next active frame must restart reprojection).
    if (!denoiseRecorded) denoiser_.forceHistoryReset();
    // Bind every frame including as nothing (clearSrv makes "not denoised THIS frame" distinct).
    if (denoiseAoOutput_) res_->setSrv(bindings_, 14, denoiseAoOutput_);
    else                  res_->clearSrv(bindings_, 14);
    if (denoiseGiOutput_) res_->setSrv(bindings_, 15, denoiseGiOutput_);
    else                  res_->clearSrv(bindings_, 15);
    if (denoiseReflOutput_) res_->setSrv(bindings_, 23, denoiseReflOutput_);
    else                    res_->clearSrv(bindings_, 23);
    // This frame's CSRdRefl writes the denoiser's input only while it can run next frame (reflDnWanted).
    // Leaving NRD2: u23 (and u9) still hold its targets and must be rebound whatever was wanted before.
    const bool nrd2Unbind = nrd2Bound_ && !nrd2Frame_;
    if (!nrd2Frame_) nrd2_.resetHistory();
    if (reflDnWanted != rdReflDnBound_ || !rdReflDnPlaceholder_ || nrd2Unbind) {
        if (reflDnWanted) res_->setUav(bindings_, 23, rdReflDnIn_, 0);
        else              bindReflDnPlaceholder();
        rdReflDnBound_ = reflDnWanted;
    }
    if (nrd2Unbind) {
        // u2/u3 were rebound to the histories at the top of this function.
        if (giRadiance_)               res_->setUav(bindings_, 9, giRadiance_, 0);
        else if (rdReflDnPlaceholder_) res_->setUav(bindings_, 9, rdReflDnPlaceholder_, 0);
        nrd2Bound_ = false;
    }
    if (nrd2Frame_) bindNrd2Targets();

    // Read fresh every frame (scene viewport can change without full notification).
    const bool haveViewport = dev_ && dev_->sceneViewport(curSceneViewport_);

    std::memcpy(cb_.prevViewProj, prevViewProj_, sizeof(prevViewProj_));
    std::memcpy(cb_.sceneViewport, prevSceneViewport_, sizeof(prevSceneViewport_));
    if (haveViewport) std::memcpy(cb_.sceneViewportCur, curSceneViewport_, sizeof(curSceneViewport_));
    else              std::memset(cb_.sceneViewportCur, 0, sizeof(cb_.sceneViewportCur));

    // Translucent volume containing the eye (property of camera, not surface).
    cb_.cameraMedium[0] = cb_.cameraMedium[1] = 0.0f;
    cb_.causticMin[3] = 0.0f;
    {
        f32 vp[16], eye[3] = {};
        // Only tests eye against volume's AABB; drawsPrev_ holds current frame's draws.
        if (dev_ && dev_->camera(vp, nullptr, eye)) {
            // Only translucent draws visited (in list order as recorded by submit()).
            for (const u32 drawIndex : translucentDrawsPrev_) {
                const Draw& d = drawsPrev_[drawIndex];
                if (d.boundsRadius < 0.0f) continue;
                const pbr::MaterialConstants* mc =
                    reinterpret_cast<const pbr::MaterialConstants*>(d.mat);
                if (d.matBytes < sizeof(pbr::MaterialConstants)) continue;
                if (mc->flags & pbr::MaterialFlag_TwoSided) continue;

                // Box from mesh extents transformed through world matrix (conservative when rotated).
                f32 lmin[3], lmax[3];
                if (!dev_->meshBoundsAabb(d.mesh, lmin, lmax)) continue;
                f32 wmin[3] = {1e30f, 1e30f, 1e30f}, wmax[3] = {-1e30f, -1e30f, -1e30f};
                for (int c = 0; c < 8; ++c) {
                    const f32 lx = (c & 1) ? lmax[0] : lmin[0];
                    const f32 ly = (c & 2) ? lmax[1] : lmin[1];
                    const f32 lz = (c & 4) ? lmax[2] : lmin[2];
                    for (int a = 0; a < 3; ++a) {
                        const f32 v = d.world[a] * lx + d.world[4 + a] * ly + d.world[8 + a] * lz + d.world[12 + a];
                        wmin[a] = std::fmin(wmin[a], v);
                        wmax[a] = std::fmax(wmax[a], v);
                    }
                }

                // Publish caustic caster (eye can see caustics from outside the volume).
                if (cb_.causticMin[3] == 0.0f) {
                    for (int a = 0; a < 3; ++a) { cb_.causticMin[a] = wmin[a]; cb_.causticMax[a] = wmax[a]; }
                    cb_.causticMin[3] = 1.0f;
                    cb_.causticMax[3] = settings_.causticStrength;
                }

                if (eye[0] < wmin[0] || eye[0] > wmax[0] ||
                    eye[1] < wmin[1] || eye[1] > wmax[1] ||
                    eye[2] < wmin[2] || eye[2] > wmax[2]) continue;
                cb_.cameraMedium[0] = 1.0f;
                cb_.cameraMedium[1] = mc->ior > 1.0f ? mc->ior : 1.0f;
                break;
            }
        }
    }
    // Sun movement invalidates history (reprojection would be geometrically correct but temporally stale).
    const bool sunMoved = rtHistSunMoved();
    cb_.rtHistParams[0] = 1.0f;
    // Three states: 1=usable, 0.5=usable but sun changed this frame, 0=not usable.
    cb_.rtHistParams[1] = (rtHistValid_ && haveViewport) ? (sunMoved ? 0.5f : 1.0f) : 0.0f;
    cb_.rtHistParams[2] = static_cast<f32>(rtFrameIndex_);
    // Tile edge as bit count (shader masks/shifts instead of multiply/modulo).
    u32 tileBits = 0;
    for (u32 v = rtPixelsPerRayTile_; v > 1; v >>= 1) ++tileBits;
    cb_.rtHistParams[3] = static_cast<f32>(tileBits);
    if (nrd2Frame_) {
        // Single-frame: every Voxi history takes its raw path (no reprojection, no write) -- shadow,
        // reflection, sky occlusion, lamps. Half rate stays, as checkerboards filled from this frame:
        // bit 4 glossy reflections and bit 128 sky occlusion (CSRdHalfFill, which bit 64 enables and
        // CSRdShadow's normal guide serves), bit 256 lamps (Stage B's 5x5). Bit 2 (8x8 tiles) needs history.
        cb_.rtHistParams[0] = 0.0f;
        cb_.rtHistParams[1] = 0.0f;
        cb_.rtDenoiseParams[3] = 0.0f;
        u32 bits = static_cast<u32>(cb_.giShadowParams[3]);
        const bool fill = rdHalfFillCsPso_ != 0;
        const bool refl = fill && settings_.nrd2HalfRateRefl && (bits & 4u) != 0u;
        const bool ao   = fill && settings_.nrd2HalfRateAo && (bits & 2u) != 0u;
        bits &= ~(2u | 4u);
        if (refl) bits |= 4u;
        if (ao) bits |= 128u;
        if (fill && (refl || ao || settings_.nrd2HalfRateGi)) bits |= 64u;
        bits |= 512u;   // shader-visible NRD2 frame (rtNrd2Frame)
        cb_.giShadowParams[3] = static_cast<f32>(bits);
    } else if (denoiserMode(settings_) == 0u) {
        // Denoiser None: nothing filters. History "not usable" (the state every reset uses) turns off every
        // reprojection read, so no temporal blend, spatial filter, half-rate fill or lamp history runs, while the
        // reflection lobe and the sun's jitter stay. Bit 1024 drops the tail's 5x5 (rtFiltersOff). ReSTIR's
        // reservoir reuse stays: it is the GI estimator. The tail's 2x2 block sharing is part of its estimator.
        cb_.rtHistParams[1] = 0.0f;
        cb_.giShadowParams[3] = static_cast<f32>(static_cast<u32>(cb_.giShadowParams[3]) | 1024u);
    }
    // Spatial filter radius; blend amount pinned at 0 (loop runs but result discarded via constant).
    // x: bounces in the low four bits, bit 4 = Reference mode (voxi_pt.hlsli ptReferenceMode).
    cb_.ptBounceParams[0] = static_cast<f32>((pathTracingWanted() ? std::clamp(ptBounces_, 1u, 8u) : 1u) |
                                             (ptReferenceWanted() ? 16u : 0u));
    // PROGRESSIVE ACCUMULATION (Stage B, u22): y = 0 off, 1 restart, 2 keep averaging; z = frame cap.
    // Restarts whenever something that lights the whole image changes: the camera, the sun, the
    // viewport or a setting. A moving object only restarts its own pixels (shader). A lamp change
    // (moving traffic, a flicker) only shortens the average to kPtAccumLampFrames for that frame.
    cb_.ptBounceParams[1] = 0.0f;
    cb_.ptBounceParams[2] = static_cast<f32>(ptReferenceWanted() ? kPtAccumRefFrames : kPtAccumMaxFrames);
    if (pathTracingWanted() && ptAccumBuf_ && dev_) {
        f32 key[kPtAccumKeyFloats] = {};
        f32 eyeK[3] = {};
        const bool haveCam = dev_->camera(key, nullptr, eyeK);
        const rhi::SkyAtmosphere skyK = dev_->skyAtmosphere();
        for (int a = 0; a < 3; ++a) {
            key[16 + a] = eyeK[a];
            key[19 + a] = skyK.sunDirection[a];
            key[22 + a] = skyK.sunColor[a];
        }
        for (int a = 0; a < 4; ++a) key[25 + a] = curSceneViewport_[a];
        // The settings that change the lit image, field by field (a whole-struct compare would see padding).
        const f32 lit[] = {
            static_cast<f32>(settings_.pathTracing), static_cast<f32>(settings_.ptBounces),
            static_cast<f32>(settings_.ptMode),
            static_cast<f32>(settings_.globalIllumination), static_cast<f32>(settings_.rayTracing),
            settings_.giIntensity, settings_.giMaxDistance, static_cast<f32>(settings_.rtShadowRays),
            static_cast<f32>(settings_.giMode), static_cast<f32>(settings_.giRestirMaxHistory),
            static_cast<f32>(settings_.giRestirSpatialSamples), static_cast<f32>(settings_.refractionMode),
            settings_.refractionStrength, settings_.fogOcclusion ? 1.0f : 0.0f,
            static_cast<f32>(denoiserMode(settings_)), static_cast<f32>(settings_.msaa)};
        static_assert(31 + sizeof(lit) / sizeof(f32) <= kPtAccumKeyFloats, "kPtAccumKeyFloats holds the key");
        std::memcpy(&key[31], lit, sizeof(lit));
        const bool same = haveCam && ptAccumValid_ && std::memcmp(key, ptAccumKey_, sizeof(key)) == 0;
        cb_.ptBounceParams[1] = same ? 2.0f : 1.0f;
        if (same && rdLocalLightHash_ != ptAccumLampHash_)
            cb_.ptBounceParams[2] = static_cast<f32>(kPtAccumLampFrames);
        std::memcpy(ptAccumKey_, key, sizeof(key));
        ptAccumLampHash_ = rdLocalLightHash_;
        ptAccumValid_ = haveCam;
    } else {
        ptAccumValid_ = false;
    }
    cb_.rtDenoiseParams[0] = static_cast<f32>(rtShadowDenoise_);
    cb_.rtDenoiseParams[1] = rtShadowDenoise_ > 0 ? 1.0f : 0.0f;
    // Motion taper: off by default (temporal accumulation already removes variance).
    cb_.rtDenoiseParams[2] = rtDenoiseMotionTaper_;
}

// Has the sun moved since the frame the history was written? Only checks direction/colour/intensity.
bool VoxiRenderer::rtHistSunMoved() const {
    if (!dev_) return false;
    const rhi::SkyAtmosphere sky = dev_->skyAtmosphere();
    for (int i = 0; i < 3; ++i) {
        if (sky.sunDirection[i] != rtHistSunDir_[i]) return true;
        if (sky.sunColor[i]     != rtHistSunColor_[i]) return true;
    }
    return sky.sunIntensity != rtHistSunIntensity_;
}

bool VoxiRenderer::rtHistSunJumped() const {
    // Distinguishes one-frame slider drag from abrupt value/level changes (10 deg, 20% thresholds).
    constexpr f32 kGiSunJumpDeg = 10.0f;
    constexpr f32 kGiSunJumpRel = 0.2f;
    if (!dev_) return false;
    if (rtHistSunIntensity_ < 0.0f) return true;
    const rhi::SkyAtmosphere sky = dev_->skyAtmosphere();
    f32 dotDir = 0.0f, lenA = 0.0f, lenB = 0.0f;
    for (int i = 0; i < 3; ++i) {
        dotDir += sky.sunDirection[i] * rtHistSunDir_[i];
        lenA   += sky.sunDirection[i] * sky.sunDirection[i];
        lenB   += rtHistSunDir_[i] * rtHistSunDir_[i];
    }
    const f32 lens = std::sqrt(lenA * lenB);
    if (lens < 1e-12f) return true;
    if (dotDir / lens < std::cos(kGiSunJumpDeg * 3.14159265f / 180.0f)) return true;
    auto relChange = [](f32 now, f32 was) {
        const f32 base = std::max(std::abs(was), 1e-4f);
        return std::abs(now - was) / base;
    };
    if (relChange(sky.sunIntensity, rtHistSunIntensity_) > kGiSunJumpRel) return true;
    for (int i = 0; i < 3; ++i)
        if (relChange(sky.sunColor[i], rtHistSunColor_[i]) > kGiSunJumpRel) return true;
    return false;
}

void VoxiRenderer::endShadowHistory() {
    if (!shadowHistoryActive()) return;
    // Record the sun this frame's history was accumulated under.
    if (dev_) {
        const rhi::SkyAtmosphere sky = dev_->skyAtmosphere();
        for (int i = 0; i < 3; ++i) { rtHistSunDir_[i] = sky.sunDirection[i]; rtHistSunColor_[i] = sky.sunColor[i]; }
        rtHistSunIntensity_ = sky.sunIntensity;
    }
    std::memcpy(prev2ViewProj_, prevViewProj_, sizeof(prevViewProj_));
    std::memcpy(prev2SceneViewport_, prevSceneViewport_, sizeof(prevSceneViewport_));
    std::memcpy(prevViewProj_, curViewProj_, sizeof(curViewProj_));
    std::memcpy(prevSceneViewport_, curSceneViewport_, sizeof(curSceneViewport_));
    std::memcpy(prevInvViewProjRel_, curInvViewProjRel_, sizeof(curInvViewProjRel_));
    std::memcpy(prevCamPos_, curCamPos_, sizeof(curCamPos_));
    rtHistWriteIdx_ = 1 - rtHistWriteIdx_;
    // An NRD2 frame wrote no history; the next frame without it must not reproject.
    rtHistValid_ = !nrd2Frame_;
    rtHistPrimed_ = true;
    if (rdLocalOutThisFrame_) rdLocalHistPrimed_ = true;
    // Validity mirrors giRestirParams[0] (set in beginShadowHistory).
    if (cb_.giRestirParams[0] > 0.5f) { giHistValid_ = true; giHistPrimed_ = true; }
    if (cb_.giRestirParams[0] > 0.5f && (static_cast<u32>(cb_.ambientParams[3]) & 4u)) {
        giVisHistValid_ = true;
        giVisHistPrimed_ = true;
    }
}

// Pick between plain and G-buffer pipelines (dev_.gBufferEnabled() must be checked directly per header).
rhi::PipelineHandle VoxiRenderer::pickGbuf(rhi::PipelineHandle plain, rhi::PipelineHandle gbuf) const {
    if (!gbuf || !dev_ || !dev_->gBufferEnabled()) return plain;
    // Under MSAA the backend binds multisampled twins and resolves them; one that cannot says so.
    if (!dev_->gBufferWritten()) return plain;
    return gbuf;
}

// NeuRaC radiance cache frame entry (staged ray-driven passes on D3D12 only; elsewhere behaves as HalfResolution).
void VoxiRenderer::updateNeuRaC(rhi::IRenderContext& ctx) {
    neuracLive_ = false;
    if (!neuracWanted()) {
        if (rc_.valid() || rcSlotsBound_) teardownNeuRaC();
        rcCreateFailed_ = false;
        rcUnsupportedLogged_ = false;
        return;
    }
    if (!res_ || !dev_) return;

    const char* why = nullptr;
    if (dev_->backend() != rhi::Backend::D3D12)
        why = "the backend is not D3D12 (the cache runs inside the staged compute passes)";
    else if (!rdStagedWanted())
        why = "voxi.rayDrivenStages is 0 (the cache needs the staged ray-driven passes)";
    else if (!rayDrivenActive())
        why = "ray-driven primary visibility is not active (voxi.rtRenderMode / ray tracing)";
    if (why) {
        if (!rcUnsupportedLogged_) {
            rcUnsupportedLogged_ = true;
            AVER_INFO("[Voxi] ReSTIR visibility mode Cached (NeuRaC) requested, but {}; "
                      "behaving as HalfResolution (said once)", why);
        }
        if (rc_.valid() || rcSlotsBound_) teardownNeuRaC();
        return;
    }

    if (!rcTwinsTried_) requestBuild(kPsoRc);
    const bool anyTwin = rdGiCacheCsPso_ || rdGiCacheCbCsPso_ || rdGiTraceCacheCsPso_ ||
                         rdGiTraceCacheCbCsPso_;
    if (!anyTwin) return;

    if (!rc_.valid()) {
        if (rcCreateFailed_) return;
        if (!rc_.create(*res_)) {
            rcCreateFailed_ = true;
            return;
        }
        giHistValid_ = false;
    }

    f32 vp[16], eye[3] = {};
    if (!dev_->camera(vp, nullptr, eye)) return;
    const NeuRaC::Params params{};
    const NeuRaC::Bindings b = rc_.beginFrame(ctx, eye, rtFrameIndex_, params);
    if (!b.info || !b.accum || !b.cells) return;

    res_->setSrvBuffer(bindings_, 22, b.info, b.infoStride, b.infoCount, 0);
    if (!rcSlotsBound_ || b.generation != rcBoundGeneration_) {
        res_->setUavBuffer(bindings_, 20, b.accum, 4, b.accumInts, 0);
        res_->setUavBuffer(bindings_, 21, b.cells, b.cellStride, b.cellCount, 0);
        rcBoundGeneration_ = b.generation;
        rcSlotsBound_ = true;
        AVER_INFO("[Voxi] NeuRaC live: {} accumulator ints, {} cells of {} B", b.accumInts,
                  b.cellCount, b.cellStride);
    }
    neuracLive_ = true;
}

// Release cache: unbind t22/u20/u21, then destroy (bound descriptor outliving its buffer faults GPU).
void VoxiRenderer::teardownNeuRaC() {
    neuracLive_ = false;
    if (res_ && bindings_ && rcSlotsBound_) {
        res_->clearSrv(bindings_, 22);
        if (!rcPlaceholder_) {
            rhi::BufferDesc pd;
            pd.bytes = 64;
            pd.kind  = rhi::BufferKind::Default;
            pd.allowUnorderedAccess = true;
            pd.debugName = "Voxi NeuRaC placeholder";
            rcPlaceholder_ = res_->createBuffer(pd);
        }
        if (rcPlaceholder_) {
            res_->setUavBuffer(bindings_, 20, rcPlaceholder_, 4, 16, 0);
            res_->setUavBuffer(bindings_, 21, rcPlaceholder_, 32, 2, 0);
        }
    }
    rc_.destroy();
    rcSlotsBound_ = false;
    rcBoundGeneration_ = 0;
}

// Whether this frame's ray-driven primary runs staged instead of the single PSRayDriven draw.
bool VoxiRenderer::rdStagedActive(const char** reason) const {
    if (!rdStagedWanted()) return false;
    if (!rayDrivenActive()) return false;
    // Recording compute inside the scene pass is Vulkan-illegal; only D3D12 verified.
    if (!(dev_ && dev_->backend() == rhi::Backend::D3D12)) {
        if (reason) *reason = "the backend is not D3D12 (recording compute inside the scene pass is "
                              "Vulkan-illegal)";
        return false;
    }
    if (!rdVisCsPso_ || !rdShadowCsPso_) {
        if (reason) *reason = "the staged compute pipelines (CSRdVisibility/CSRdShadow) did not compile";
        return false;
    }
    if (!rdVisBuf_ || !rdSunVisTex_ || !rdGiTex_ || !rdAoTex_ || !rdReflTex_) {
        if (reason) *reason = "the visibility record buffer or one of the sun visibility/GI/sky "
                              "occlusion/reflection textures is absent";
        return false;
    }
    // Shadow history must be active; scene viewport must be valid.
    if (!shadowHistoryActive() || !(curSceneViewport_[2] > 0.0f && curSceneViewport_[3] > 0.0f)) {
        if (reason) *reason = "the ray-traced history or this frame's scene viewport is not available";
        return false;
    }
    // Staged mode requires the textured ray-driven pipeline.
    const bool gbufBound = pickGbuf(rayDrivenPso_, rayDrivenGbufPso_) == rayDrivenGbufPso_;
    const rhi::PipelineHandle texPso   = gbufBound ? rayDrivenTexGbufPso_      : rayDrivenTexPso_;
    const rhi::PipelineHandle splitPso = gbufBound ? rayDrivenSplitTexGbufPso_ : rayDrivenSplitTexPso_;
    if (!texPso) {
        if (reason) *reason = "the untextured ray-driven pipeline is in use, not the textured one Stage "
                              "B needs";
        return false;
    }
    if (!splitPso) {
        if (reason) *reason = "Stage B's AVER_RD_SPLIT pipeline variant did not compile";
        return false;
    }
    // ReSTIR GI dispatch fires only when both cb_ values are active.
    if (cb_.voxelParams[3] > 0.5f && cb_.giRestirParams[0] > 0.5f && !rdGiCsPso_) {
        if (reason) *reason = "ReSTIR GI is active this frame but CSRdGi did not compile";
        return false;
    }
    if (cb_.ambientParams[0] > 0.5f && (cb_.giRestirParams[0] > 0.5f || cb_.voxelParams[3] <= 0.5f) &&
        !rdSkyOccCsPso_) {
        if (reason) *reason = "sky occlusion is active this frame but CSRdSkyOcc did not compile";
        return false;
    }
    // Reflections dispatch fires only when both cb_ values are active.
    if (cb_.shadowParams[2] > 0.5f && cb_.rtParams[3] > 0.5f && !rdReflCsPso_) {
        if (reason) *reason = "ray-traced reflections are active this frame but CSRdRefl did not compile";
        return false;
    }
    return true;
}

// Returns the lit pipeline for this frame, or 0 to decline and let the backend use its own.
rhi::PipelineHandle VoxiRenderer::scenePipeline(bool meshShaders, bool depthPrepassed,
                                                bool blended) const {
    if (!canRecord()) return 0;
    // Blended is answered first: glass outranks a prepass. If blended fails to create, drops the draw.
    if (blended) {
        // Blended draws never write the G-buffer: opaque surface behind is the denoiser's geometry.
        if (rtActive_ && sceneRtBlendedTexPso_ && !meshShaders) return sceneRtBlendedTexPso_;
        if (rtActive_) return meshShaders && sceneMsRtBlendedPso_ ? sceneMsRtBlendedPso_ : sceneRtBlendedPso_;
        return meshShaders && sceneMsBlendedPso_ ? sceneMsBlendedPso_ : sceneBlendedPso_;
    }
    // LessEqual/no-write twin for prepassed draws. Mesh shaders never take this: backends guarantee this is false.
    if (depthPrepassed && !meshShaders) {
        if (rtActive_ && sceneRtPsoPrepassed_) return pickGbuf(sceneRtPsoPrepassed_, sceneRtPsoPrepassedGbuf_);
        if (!rtActive_ && scenePsoPrepassed_)  return pickGbuf(scenePsoPrepassed_, scenePsoPrepassedGbuf_);
    }
    if (rtActive_)
        return pickGbuf(meshShaders && sceneMsRtPso_ ? sceneMsRtPso_ : sceneRtPso_,
                        meshShaders && sceneMsRtGbufPso_ ? sceneMsRtGbufPso_ : sceneRtGbufPso_);
    return pickGbuf(meshShaders && sceneMsPso_ ? sceneMsPso_ : scenePso_,
                    meshShaders && sceneMsGbufPso_ ? sceneMsGbufPso_ : sceneGbufPso_);
}

// Depth-only prepass pipeline. Returns 0 if the LessEqual/no-write twin does not exist.
rhi::PipelineHandle VoxiRenderer::depthPrepassPipeline() const {
    if (!canRecord()) return 0;
    const rhi::PipelineHandle twin = rtActive_ ? sceneRtPsoPrepassed_ : scenePsoPrepassed_;
    return twin ? depthPrepassPso_ : 0;
}

// Creates the cascaded shadow atlas.
bool VoxiRenderer::createShadowResources() {
    rhi::TextureDesc d;
    d.dim    = rhi::TextureDim::Tex2D;
    d.width  = kShadowSize;
    d.height = kShadowSize;
    // Typeless so DSV sees D32Float while SRV sees R32Float.
    d.format = rhi::Format::R32Typeless;
    d.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::DepthStencil;
    d.initialState  = rhi::ResourceState::ShaderResource;
    d.hasClearValue = true;
    d.clearDepth    = 1.0f;
    d.debugName     = "Voxi shadow map";
    shadowTex_ = res_->createTexture(d);
    if (!shadowTex_) AVER_ERROR("[Voxi] shadow map {}^2 could not be created", kShadowSize);

    // GI-only map: smaller single box.
    d.width  = kGiShadowSize;
    d.height = kGiShadowSize;
    d.debugName = "Voxi GI shadow map";
    giShadowTex_ = res_->createTexture(d);
    if (!giShadowTex_)
        AVER_WARN("[Voxi] GI-only shadow map {}^2 could not be created; indirect light will be "
                  "injected unshadowed", kGiShadowSize);

    // GI map affects bounce light alone; losing it degrades picture, not a failure.
    return shadowTex_ != 0;
}

// Creates the radiance volume, injection accumulator, and binding sets.
bool VoxiRenderer::createInjectionAccumulator(u32 resolution) {
    // Four uints per voxel (r, g, b, fragment count) interleaved along x: R32_UINT is the only typed
    // format D3D12 guarantees UAV atomics on.
    rhi::TextureDesc ad;
    ad.dim    = rhi::TextureDim::Tex3D;
    ad.width  = resolution * 4;
    ad.height = resolution;
    ad.depth  = resolution;
    ad.mips   = 1;
    ad.format = rhi::Format::R32Uint;
    ad.bind   = rhi::ResourceBind::UnorderedAccess;
    ad.initialState = rhi::ResourceState::UnorderedAccess;
    ad.debugName    = "Voxi injection accumulator";
    voxelAccumTex_ = res_->createTexture(ad);
    if (!voxelAccumTex_) { AVER_ERROR("[Voxi] injection accumulator {}^3 could not be created", resolution); return false; }
    return true;
}

bool VoxiRenderer::createVoxelVolume(u32 resolution) {
    rhi::TextureDesc d;
    d.dim    = rhi::TextureDim::Tex3D;
    d.width  = resolution;
    d.height = resolution;
    d.depth  = resolution;
    d.mips   = 0;                       // full chain: mip N is cone footprint at distance N
    d.format = rhi::Format::RGBA16F;
    d.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
    d.initialState = rhi::ResourceState::ShaderResource;
    d.debugName    = "Voxi radiance volume";
    ++voxelGen_;
    voxelTex_ = res_->createTexture(d);
    if (!voxelTex_) { AVER_ERROR("[Voxi] radiance volume {}^3 could not be created", resolution); return false; }

    if (!createInjectionAccumulator(resolution)) return false;

    // Resolved mip count.
    rhi::TextureDesc got{};
    if (!res_->textureInfo(voxelTex_, got)) { AVER_ERROR("[Voxi] textureInfo failed for the radiance volume"); return false; }
    voxelMips_     = got.mips;
    voxelResBuilt_ = resolution;
    AVER_INFO("[Voxi] GI voxel volume {}^3", resolution);
    // Volume memory: radiance RGBA16F = 8 B/texel per mip; accumulator 16 B/voxel.
    {
        u64 radianceVoxels = 0;
        for (u32 m = 0; m < voxelMips_; ++m) {
            const u64 dm = mipDim(resolution, m);
            radianceVoxels += dm * dm * dm;
        }
        const f64 radianceMiB = static_cast<f64>(radianceVoxels * 8ull) / (1024.0 * 1024.0);
        const f64 accumMiB = static_cast<f64>(static_cast<u64>(resolution) * resolution * resolution *
                                              16ull) / (1024.0 * 1024.0);
        AVER_INFO("[Voxi] GI volume memory (computed from the texture descriptions, not queried): "
                  "radiance {:.0f} MiB, injection accumulator {:.0f} MiB", radianceMiB, accumMiB);
    }

    // Main table: kVoxiSrvCount/kGiUavCount matching giLayout().
    rhi::BindingSetDesc bd;
    bd.srvCount = kVoxiSrvCount;
    bd.uavCount = kVoxiUavCount;
    giTableKinds(bd.srvKinds, bd.uavKinds);
    bindings_ = res_->createBindingSet(bd);
    if (!bindings_) { AVER_ERROR("[Voxi] main binding set could not be created"); return false; }
    res_->setSrv(bindings_, 0, voxelTex_, rhi::kAllMips);
    if (shadowTex_) res_->setSrv(bindings_, 1, shadowTex_);
    if (giShadowTex_) res_->setSrv(bindings_, 8, giShadowTex_);
    res_->setUav(bindings_, 0, voxelTex_, 0);
    res_->setUav(bindings_, 1, voxelAccumTex_, 0);
    // Occlusion-aware fog: t17/u16 bound to placeholder until ensureAirVis() upgrades them.
    {
        rhi::TextureDesc pd;
        pd.dim    = rhi::TextureDim::Tex3D;
        pd.width  = pd.height = pd.depth = 1;
        pd.mips   = 1;
        pd.format = rhi::Format::R16F;
        pd.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
        pd.initialState = rhi::ResourceState::ShaderResource;
        pd.debugName    = "Voxi air sky-visibility placeholder";
        airVisPlaceholder_ = res_->createTexture(pd);
        if (!airVisPlaceholder_) { AVER_ERROR("[Voxi] air sky-visibility placeholder could not be created"); return false; }
    }
    res_->setSrv(bindings_, 17, airVisPlaceholder_, rhi::kAllMips);
    res_->setUav(bindings_, 16, airVisPlaceholder_, 0);
    // Local lights: t18/t19/u19 get placeholders; buildLocalLights()/ensureShadowHistory() upgrade them.
    if (!rdLocalLightsPlaceholder_) {
        rhi::BufferDesc pd;
        pd.bytes = sizeof(RdLocalLight);
        pd.kind  = rhi::BufferKind::Default;
        pd.debugName = "Voxi local-light list placeholder";
        rdLocalLightsPlaceholder_ = res_->createBuffer(pd);
        if (!rdLocalLightsPlaceholder_) { AVER_ERROR("[Voxi] local-light list placeholder could not be created"); return false; }
    }
    if (!rdLocalHistPlaceholder_) {
        rhi::TextureDesc pd;
        pd.dim    = rhi::TextureDim::Tex2D;
        pd.width  = 1; pd.height = 1; pd.mips = 1;
        pd.format = rhi::Format::RGBA16F;
        pd.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
        pd.initialState = rhi::ResourceState::ShaderResource;
        pd.debugName    = "Voxi local-light history placeholder";
        rdLocalHistPlaceholder_ = res_->createTexture(pd);
        if (!rdLocalHistPlaceholder_) { AVER_ERROR("[Voxi] local-light history placeholder could not be created"); return false; }
    }
    res_->setSrvBuffer(bindings_, 18, rdLocalLightsPlaceholder_, sizeof(RdLocalLight), 1, 0);
    rdLocalLightsBound_ = rdLocalLightsPlaceholder_;
    // Decals: t24 holds a one-record stand-in until buildDecals() has a real list.
    if (!decalPlaceholder_) {
        rhi::BufferDesc pd;
        pd.bytes = sizeof(PackedDecal);
        pd.kind  = rhi::BufferKind::Default;
        pd.debugName = "Voxi decal list placeholder";
        decalPlaceholder_ = res_->createBuffer(pd);
        if (!decalPlaceholder_) { AVER_ERROR("[Voxi] decal list placeholder could not be created"); return false; }
    }
    res_->setSrvBuffer(bindings_, 24, decalPlaceholder_, sizeof(PackedDecal), 1, 0);
    decalBound_ = decalPlaceholder_;
    res_->setSrv(bindings_, 19, rdLocalHistPlaceholder_);
    res_->setUav(bindings_, 19, rdLocalHistPlaceholder_, 0);
    // Path Tracing's accumulation (u22): a one-element stand-in until ensurePtAccum sizes the real one.
    if (!ptAccumPlaceholder_) {
        rhi::BufferDesc pd;
        pd.bytes = kPtAccumElemBytes;
        pd.kind  = rhi::BufferKind::Default;
        pd.allowUnorderedAccess = true;
        pd.debugName = "Voxi Path Tracing accumulation placeholder";
        ptAccumPlaceholder_ = res_->createBuffer(pd);
        if (!ptAccumPlaceholder_) { AVER_ERROR("[Voxi] Path Tracing accumulation placeholder could not be created"); return false; }
    }
    res_->setUavBuffer(bindings_, 22, ptAccumPlaceholder_, kPtAccumElemBytes, 1, 0);
    // Instanced foliage: t20/t21 placeholders; refreshFoliageBindings() swaps in real pair.
    if (!foliagePartPlaceholder_) {
        rhi::BufferDesc pd;
        pd.bytes = sizeof(RtInstance);
        pd.kind  = rhi::BufferKind::Default;
        pd.debugName = "Voxi foliage part table placeholder";
        foliagePartPlaceholder_ = res_->createBuffer(pd);
        if (!foliagePartPlaceholder_) { AVER_ERROR("[Voxi] foliage part table placeholder could not be created"); return false; }
    }
    if (!foliageDescPlaceholder_) {
        rhi::BufferDesc pd;
        pd.bytes = rhi::kTlasInstanceDescBytes;
        pd.kind  = rhi::BufferKind::Default;
        pd.debugName = "Voxi foliage instance desc placeholder";
        foliageDescPlaceholder_ = res_->createBuffer(pd);
        if (!foliageDescPlaceholder_) { AVER_ERROR("[Voxi] foliage instance desc placeholder could not be created"); return false; }
    }
    res_->setSrvBuffer(bindings_, 20, foliagePartPlaceholder_, sizeof(RtInstance), 1, 0);
    res_->setSrvBuffer(bindings_, 21, foliageDescPlaceholder_, rhi::kTlasInstanceDescBytes, 1, 0);
    // t6/u2, t7/u3, t11/u4 populated by onRenderTargetsChanged (unknown resolution early).

    // Clear and resolve get UAV-only sets.
    rhi::BindingSetDesc cd;
    cd.uavCount = 2;
    cd.uavKinds[0] = rhi::SlotKind::Texture3D;
    cd.uavKinds[1] = rhi::SlotKind::Texture3D;
    clearBindings_ = res_->createBindingSet(cd);
    if (!clearBindings_) { AVER_ERROR("[Voxi] clear binding set could not be created"); return false; }
    res_->setUav(clearBindings_, 0, voxelTex_, 0);
    res_->setUav(clearBindings_, 1, voxelAccumTex_, 0);

    resolveBindings_ = res_->createBindingSet(cd);
    if (!resolveBindings_) { AVER_ERROR("[Voxi] resolve binding set could not be created"); return false; }
    res_->setUav(resolveBindings_, 0, voxelTex_, 0);
    res_->setUav(resolveBindings_, 1, voxelAccumTex_, 0);

    // One set per mip filter step: single-mip source view makes reading level m-1 while writing level m legal.
    mipBindings_.reserve(voxelMips_ ? voxelMips_ - 1 : 0);
    for (u32 m = 1; m < voxelMips_; ++m) {
        rhi::BindingSetDesc md;
        md.srvCount = 1;
        md.uavCount = 1;
        md.srvKinds[0] = rhi::SlotKind::Texture3D;
        md.uavKinds[0] = rhi::SlotKind::Texture3D;
        const rhi::BindingSetHandle s = res_->createBindingSet(md);
        if (!s) { AVER_ERROR("[Voxi] mip binding set {} could not be created", m); return false; }
        res_->setSrv(s, 0, voxelTex_, m - 1);
        res_->setUav(s, 0, voxelTex_, m);
        mipBindings_.push_back(s);
    }
    return true;
}

// Creates or releases airVisTex_ against airVisWanted(). Idempotent: returns true once existence matches.
bool VoxiRenderer::ensureAirVis() {
    if (!res_ || !bindings_) return false;
    const bool wanted = airVisWanted();
    if (wanted == (airVisTex_ != 0)) return true;

    if (!wanted) {
        // Rebind before destroy (view outlives buffer rule).
        if (airVisPlaceholder_) {
            res_->setSrv(bindings_, 17, airVisPlaceholder_, rhi::kAllMips);
            res_->setUav(bindings_, 16, airVisPlaceholder_, 0);
        }
        res_->destroyTexture(airVisTex_);
        airVisTex_ = 0;
        airVisDirty_ = false;
        AVER_INFO("[Voxi] air sky-visibility volume released; voxi.fogOcclusion is off (placeholder "
                  "bound -- fog reads full sky visibility everywhere, exactly as before this feature "
                  "existed)");
        return true;
    }

    rhi::TextureDesc d;
    d.dim    = rhi::TextureDim::Tex3D;
    d.width  = d.height = d.depth = kAirVisResolution;
    d.mips   = 1;
    // R16F: smallest format for both typed UAV store and SRV.
    d.format = rhi::Format::R16F;
    d.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
    d.initialState = rhi::ResourceState::ShaderResource;
    d.debugName    = "Voxi air sky-visibility volume";
    airVisTex_ = res_->createTexture(d);
    if (!airVisTex_) {
        AVER_ERROR("[Voxi] air sky-visibility volume ({}^3) could not be created; fog stays "
                   "unoccluded (placeholder stays bound at t17/u16)", kAirVisResolution);
        return false;
    }
    res_->setSrv(bindings_, 17, airVisTex_, rhi::kAllMips);
    res_->setUav(bindings_, 16, airVisTex_, 0);
    // Fresh texture holds uninitialized data; mark it dirty to fill before shade pass reads it.
    airVisDirty_ = true;
    AVER_INFO("[Voxi] air sky-visibility volume created ({}^3, {:.1f} KiB); CSAirVis fills it once, "
              "this frame or next, before any shade pass reads it", kAirVisResolution,
              static_cast<f64>(static_cast<u64>(kAirVisResolution) * kAirVisResolution *
                               kAirVisResolution * 2ull) / 1024.0);
    return true;
}

// Dispatches CSAirVis over the volume (for occlusion-aware fog).
void VoxiRenderer::dispatchAirVis(rhi::IRenderContext& ctx, u32 zLo, u32 zHi) {
    rhi::ScopedGpuStat stat(ctx, "Voxi air visibility");
    ctx.textureBarrier(airVisTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    ctx.setPipeline(airVisPso_);
    ctx.setBindingSet(bindings_);
    // Table 1 bound (material table).
    ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
    // Cell box rides MipCB (b3); set before every dispatch.
    VoxelBox box;
    box.lo[0] = 0; box.lo[1] = 0; box.lo[2] = zLo;
    box.hi[0] = kAirVisResolution; box.hi[1] = kAirVisResolution; box.hi[2] = zHi;
    const GiDispatchConstants k = dispatchConstants(box, 0);
    ctx.setConstants(3, &k, kGiDispatchConstantDwords);
    constexpr u32 kGroupsXY = (kAirVisResolution + 7u) / 8u;
    if (zHi > zLo) ctx.dispatch(kGroupsXY, kGroupsXY, zHi - zLo);
    ctx.uavBarrierTexture(airVisTex_);
    ctx.textureBarrier(airVisTex_, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
    if (zLo == 0 && zHi >= kAirVisResolution) airVisDirty_ = false;
}

// Binds GI resources (volume and shadow map) into caller's binding set.
void VoxiRenderer::bindGiResources(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase) const {
    // Tier 1 null-fills until resources exist; reader learns "nothing shadowed/bounced" same way as Voxi's pipeline.
    if (voxelTex_)  res.setSrv(set, srvBase + 0, voxelTex_, rhi::kAllMips);
    if (shadowTex_) res.setSrv(set, srvBase + 1, shadowTex_);
}

// Records the Base group: the pipelines that do not bake the render-target formats.
void VoxiRenderer::recordBasePipelines(rhi::IPipelineBatch& b, PsoLocal& out) {
    const bool msOk = caps_.meshShaderTier > 0 && caps_.shaderModel >= 65 && caps_.dxcAvailable;

    ShaderScope compile(b);

    const rhi::PipelineLayout gi = giLayout();
    const std::string matDefs = pbr::materialShaderDefines(gi.srvCount, kMaterialSamplerSlot,
                                                          layeredBsdf_);
    auto rasterDefs = [&](const char* extra) { return extra ? matDefs + ";" + extra : matDefs; };

    AVER_INFO("[Voxi] scene pipelines compiling with the {} shading model",
              layeredBsdf_ ? "LAYERED (base BRDF + coat lobe)" : "standard BRDF");

    // --- 1. shadow map: depth only, from the sun ---
    if (const rhi::ShaderHandle vs = compile("VSShadow", rhi::ShaderStage::Vertex, kBaseSm, rasterDefs(nullptr).c_str())) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vs;
        p.layout = gi;
        p.cull = rhi::CullMode::None;
        p.depth = {true, true, rhi::CompareOp::Less};
        p.renderTargetCount = 0;
        p.depthFormat = rhi::Format::D32Float;
        p.sampleCount = 1;
        p.slopeScaledDepthBias = 1.5f;
        out.shadowPso_ = b.createGraphicsPipeline(p);
    }

    // --- 1b. instanced depth pass: one DrawIndexedInstanced per mesh instead of one drawMesh() per draw.
    const std::string instDefs = rasterDefs(("AVER_INSTANCE_SRV=" + std::to_string(rhi::declaredSrvCount(gi))).c_str());
    // SM 6.0 AND DXC required; FXC/SM 5.1 failed silently with garbage world matrices.
    const bool instancedShadowsOk = caps_.shaderModel >= 60 && caps_.dxcAvailable;
    if (!instancedShadowsOk)
        AVER_INFO("[Voxi] instanced shadows off (SM {}, DXC {}); using one draw per instance",
                  caps_.shaderModel, caps_.dxcAvailable ? "yes" : "no");
    if (instancedShadowsOk)
    if (const rhi::ShaderHandle vsInst = compile("VSShadowInstanced", rhi::ShaderStage::Vertex, kBaseSm, instDefs.c_str())) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsInst;
        p.layout = gi;
        p.instanced = true;
        p.cull = rhi::CullMode::None;
        p.depth = {true, true, rhi::CompareOp::Less};
        p.renderTargetCount = 0;
        p.depthFormat = rhi::Format::D32Float;
        p.sampleCount = 1;
        p.slopeScaledDepthBias = 1.5f;
        out.shadowInstancedPso_ = b.createGraphicsPipeline(p);
    }

    // --- 1c. GI-only depth pass (plain and instanced).
    if (const rhi::ShaderHandle vsGi = compile("VSGiShadow", rhi::ShaderStage::Vertex, kBaseSm,
                                               rasterDefs(nullptr).c_str())) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsGi;
        p.layout = gi;
        p.cull = rhi::CullMode::None;
        p.depth = {true, true, rhi::CompareOp::Less};
        p.renderTargetCount = 0;
        p.depthFormat = rhi::Format::D32Float;
        p.sampleCount = 1;
        p.slopeScaledDepthBias = 1.5f;
        out.giShadowPso_ = b.createGraphicsPipeline(p);
    }
    if (instancedShadowsOk)
    if (const rhi::ShaderHandle vsGiInst = compile("VSGiShadowInstanced", rhi::ShaderStage::Vertex,
                                                   kBaseSm, instDefs.c_str())) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsGiInst;
        p.layout = gi;
        p.instanced = true;
        p.cull = rhi::CullMode::None;
        p.depth = {true, true, rhi::CompareOp::Less};
        p.renderTargetCount = 0;
        p.depthFormat = rhi::Format::D32Float;
        p.sampleCount = 1;
        p.slopeScaledDepthBias = 1.5f;
        out.giShadowInstancedPso_ = b.createGraphicsPipeline(p);
    }

    // --- 2/3. voxelisation + light injection: rasterise with no render target ---
    const rhi::ShaderHandle psVoxel = compile("PSVoxel", rhi::ShaderStage::Pixel, kBaseSm, rasterDefs(nullptr).c_str());
    rhi::GraphicsPipelineDesc vox;
    vox.layout = gi;
    vox.cull = rhi::CullMode::None;
    vox.depthClip = false;
    vox.conservativeRaster = caps_.conservativeRaster;
    vox.renderTargetCount = 0;
    vox.depthFormat = rhi::Format::Unknown;
    vox.sampleCount = 1;

    const rhi::ShaderHandle vsVoxel = compile("VSVoxel", rhi::ShaderStage::Vertex, kBaseSm, rasterDefs(nullptr).c_str());
    const rhi::ShaderHandle gsVoxel = compile("GSVoxel", rhi::ShaderStage::Geometry, kBaseSm, rasterDefs(nullptr).c_str());
    if (vsVoxel && gsVoxel && psVoxel) {
        rhi::GraphicsPipelineDesc p = vox;
        p.vs = vsVoxel; p.gs = gsVoxel; p.ps = psVoxel;
        out.voxelPso_ = b.createGraphicsPipeline(p);
    }

    if (msOk && psVoxel) {
        const std::string msDefs = rasterDefs("AVER_MS=1") + ";" + rhi::meshGeometryDefines(vox.layout);
        if (const rhi::ShaderHandle ms = compile("MSVoxel", rhi::ShaderStage::Mesh, 65, msDefs.c_str())) {
            rhi::GraphicsPipelineDesc p = vox;
            p.ms = ms; p.ps = psVoxel;
            out.voxelMsPso_ = b.createGraphicsPipeline(p);
        }
    }

    // --- 4. clear the accumulator, and reduce it into mip 0 ---
    if (const rhi::ShaderHandle cs = compile("CSClear", rhi::ShaderStage::Compute, kBaseSm, nullptr)) {
        rhi::ComputePipelineDesc p;
        p.cs = cs;
        p.layout.uavCount = 2;
        // b3: root constant block for dispatch bounds (set before every dispatch).
        p.layout.constantDwords[3] = kGiDispatchConstantDwords;
        out.clearPso_ = b.createComputePipeline(p);
    }

    if (const rhi::ShaderHandle cs = compile("CSResolve", rhi::ShaderStage::Compute, kBaseSm, nullptr)) {
        rhi::ComputePipelineDesc p;
        p.cs = cs;
        p.layout.uavCount = 2;
        p.layout.constantDwords[3] = kGiDispatchConstantDwords;
        out.resolvePso_ = b.createComputePipeline(p);
    }

    // --- 5. mip filter: one source mip in, one destination mip out ---
    if (const rhi::ShaderHandle cs = compile("CSMip", rhi::ShaderStage::Compute, kBaseSm, nullptr)) {
        rhi::ComputePipelineDesc p;
        p.cs = cs;
        p.layout.srvCount = 1;
        p.layout.uavCount = 1;
        // b3: source mip + dispatch box (expanded from 4 dwords to full block).
        p.layout.constantDwords[3] = kGiDispatchConstantDwords;
        out.mipPso_ = b.createComputePipeline(p);
    }

    // --- 5b. CSAirVis (air sky-visibility volume march, for occlusion-aware fog) ---
    // Compiled against full Voxi layout (reads t0 via gVoxelSamp, writes u16).
    // SM 6.0 required; gated on instancedShadowsOk for clean fallback.
    if (instancedShadowsOk)
    if (const rhi::ShaderHandle csAirVis = compile("CSAirVis", rhi::ShaderStage::Compute, 60,
                                                    rasterDefs(nullptr).c_str())) {
        rhi::ComputePipelineDesc p;
        p.cs = csAirVis;
        p.layout = gi;
        p.layout.constantDwords[3] = kGiDispatchConstantDwords;
        out.airVisPso_ = b.createComputePipeline(p);
    }
}

// Records the Scene group: everything that bakes the sample count and render-target formats, the
// material graphs or the shader text.
void VoxiRenderer::recordScenePipelines(rhi::IPipelineBatch& b, PsoLocal& out, u32 sampleCount,
                                        rhi::Format color, rhi::Format depth) {
    const bool msOk = caps_.meshShaderTier > 0 && caps_.shaderModel >= 65 && caps_.dxcAvailable;
    const bool rtOk = caps_.rayTracingTier >= 11 && caps_.shaderModel >= 65 && caps_.dxcAvailable;

    ShaderScope compile(b);
    const rhi::PipelineLayout gi = giLayout();
    const std::string matDefs = pbr::materialShaderDefines(gi.srvCount, kMaterialSamplerSlot,
                                                          layeredBsdf_);
    auto rasterDefs = [&](const char* extra) { return extra ? matDefs + ";" + extra : matDefs; };

    // --- 6. debug: raymarch the volume to screen ---
    const rhi::ShaderHandle vsky = compile("VSky", rhi::ShaderStage::Vertex, kBaseSm, rasterDefs(nullptr).c_str());
    if (const rhi::ShaderHandle ps = compile("PSVoxelDebug", rhi::ShaderStage::Pixel, kBaseSm, rasterDefs(nullptr).c_str()); ps && vsky) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsky; p.ps = ps;
        p.layout = gi;
        p.cull = rhi::CullMode::None;
        p.renderTargetCount = 1;
        p.renderTargets[0] = color;
        p.sampleCount = sampleCount;
        out.debugPso_ = b.createGraphicsPipeline(p);
    }

    // --- 7-10. scene lit variants: cone trace and RayQuery live inside pixel shader ---
    rhi::GraphicsPipelineDesc scene;
    scene.layout = gi;
    scene.cull = rhi::CullMode::None;
    scene.depth = {true, true, rhi::CompareOp::Less};
    scene.renderTargetCount = 1;
    scene.renderTargets[0] = color;
    scene.depthFormat = depth;
    scene.sampleCount = sampleCount;

    // G-buffer twin: four targets (velocity, view-space depth, world normal + roughness).
    rhi::GraphicsPipelineDesc sceneGbuf = scene;
    sceneGbuf.renderTargetCount = 4;
    sceneGbuf.renderTargets[1] = rhi::Format::RG16F;
    sceneGbuf.renderTargets[2] = rhi::Format::R32Float;
    sceneGbuf.renderTargets[3] = rhi::Format::RGB10A2Unorm;

    const rhi::ShaderHandle vsMain = compile("VSMain", rhi::ShaderStage::Vertex, kBaseSm, rasterDefs(nullptr).c_str());
    const rhi::ShaderHandle psVoxi = compile("PSMainVoxi", rhi::ShaderStage::Pixel, kBaseSm, rasterDefs(nullptr).c_str());
    if (vsMain && psVoxi) {
        rhi::GraphicsPipelineDesc p = scene;
        p.vs = vsMain; p.ps = psVoxi;
        out.scenePso_ = b.createGraphicsPipeline(p);
    }

    // PSMainVoxi + AVER_GBUFFER changes return type (four SV_TARGETs); needs separate ShaderHandle.
    const rhi::ShaderHandle psVoxiGbuf =
        compile("PSMainVoxi", rhi::ShaderStage::Pixel, kBaseSm, rasterDefs("AVER_GBUFFER=1").c_str());
    if (vsMain && psVoxiGbuf) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.vs = vsMain; p.ps = psVoxiGbuf;
        out.sceneGbufPso_ = b.createGraphicsPipeline(p);
    }

    // Mesh-shader variants.
    const std::string msDefs = rasterDefs("AVER_MS=1") + ";" + rhi::meshGeometryDefines(scene.layout);
    const rhi::ShaderHandle msMain = msOk ? compile("MSMain", rhi::ShaderStage::Mesh, 65, msDefs.c_str()) : 0;
    if (msMain && psVoxi) {
        rhi::GraphicsPipelineDesc p = scene;
        p.ms = msMain; p.ps = psVoxi;
        out.sceneMsPso_ = b.createGraphicsPipeline(p);
    }

    if (msMain && psVoxiGbuf) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.ms = msMain; p.ps = psVoxiGbuf;
        out.sceneMsGbufPso_ = b.createGraphicsPipeline(p);
    }

    // RayQuery replaces shadow-map lookup with exact occlusion ray.
    const auto rdAblateDefs = [&]() -> std::string {
        return rdAblate_ ? (";AVER_RD_ABLATE=" + std::to_string(rdAblate_)) : std::string();
    };
    // Single-pass ray-driven variants only; voxi.rt* toggles are compile-time constants there.
    static constexpr const char* kRdSinglePassDefs =
        kRdSinglePassLamps ? ";AVER_RD_SINGLE_PASS=1" : ";AVER_RD_SINGLE_PASS=1;AVER_RD_SINGLE_PASS_LAMPS=0";
    const rhi::ShaderHandle psRt = rtOk ? compile("PSMainVoxi", rhi::ShaderStage::Pixel, 65, rasterDefs("AVER_RT=1").c_str()) : 0;
    if (vsMain && psRt) {
        rhi::GraphicsPipelineDesc p = scene;
        p.vs = vsMain; p.ps = psRt;
        out.sceneRtPso_ = b.createGraphicsPipeline(p);
    }

    // Same shader with AVER_RT and AVER_GBUFFER: PSMainVoxi nests both #if blocks.
    const rhi::ShaderHandle psRtGbuf =
        rtOk ? compile("PSMainVoxi", rhi::ShaderStage::Pixel, 65,
                       rasterDefs("AVER_RT=1;AVER_GBUFFER=1").c_str())
             : 0;
    if (vsMain && psRtGbuf) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.vs = vsMain; p.ps = psRtGbuf;
        out.sceneRtGbufPso_ = b.createGraphicsPipeline(p);
    }

    // Ray-driven primary visibility: traces camera ray instead of marching volume. Writes SV_DEPTH.
    const rhi::ShaderHandle psRayDriven =
        rtOk ? compile("PSRayDriven", rhi::ShaderStage::Pixel, 65,
                       rasterDefs((std::string("AVER_RT=1") + kRdSinglePassDefs + rdAblateDefs()).c_str()).c_str()) : 0;
    if (vsky && psRayDriven) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsky; p.ps = psRayDriven;
        p.layout = gi;
        p.cull = rhi::CullMode::None;
        p.depth = {true, true, rhi::CompareOp::Always};
        p.renderTargetCount = 1;
        p.renderTargets[0] = color;
        p.depthFormat = depth;
        p.sampleCount = sampleCount;
        out.rayDrivenPso_ = b.createGraphicsPipeline(p);
    }

    // Textured variant: bindless table is a root-signature difference (can't be runtime toggle).
    ensureTextureTable();
    if (rtOk && rtTexTable_) {
        const rhi::PipelineLayout giTex = giLayout(kRtTextureCapacity);
        const std::string bindlessDefs = "AVER_RT=1;AVER_RT_BINDLESS=1;AVER_RT_TEX_CAPACITY=" +
                                         std::to_string(kRtTextureCapacity);
        const rhi::ShaderHandle vskyTex = compile("VSky", rhi::ShaderStage::Vertex, kBaseSm,
                                                  rasterDefs(bindlessDefs.c_str()).c_str());
        const rhi::ShaderHandle psTex = compile("PSRayDriven", rhi::ShaderStage::Pixel, 65,
                                                rasterDefs((bindlessDefs + kRdSinglePassDefs + rdAblateDefs()).c_str()).c_str());
        if (vskyTex && psTex) {
            rhi::GraphicsPipelineDesc p;
            p.vs = vskyTex; p.ps = psTex;
            p.layout = giTex;
            p.cull = rhi::CullMode::None;
            p.depth = {true, true, rhi::CompareOp::Always};
            p.renderTargetCount = 1;
            p.renderTargets[0] = color;
            p.depthFormat = depth;
            p.sampleCount = sampleCount;
            out.rayDrivenTexPso_ = b.createGraphicsPipeline(p);
        }

        // Textured G-buffer twin: same shader/bindless layout plus AVER_GBUFFER=1.
        const rhi::ShaderHandle psTexGbuf =
            compile("PSRayDriven", rhi::ShaderStage::Pixel, 65,
                    rasterDefs((bindlessDefs + ";AVER_GBUFFER=1" + kRdSinglePassDefs + rdAblateDefs()).c_str()).c_str());
        if (vskyTex && psTexGbuf) {
            rhi::GraphicsPipelineDesc p;
            p.vs = vskyTex; p.ps = psTexGbuf;
            p.layout = giTex;
            p.cull = rhi::CullMode::None;
            // Fullscreen pass: depth Always/write-on (no prior depth to test against).
            p.depth = {true, true, rhi::CompareOp::Always};
            p.renderTargetCount = 4;
            p.renderTargets[0] = color;
            p.renderTargets[1] = rhi::Format::RG16F;
            p.renderTargets[2] = rhi::Format::R32Float;
            p.renderTargets[3] = rhi::Format::RGB10A2Unorm;
            p.depthFormat = depth;
            p.sampleCount = sampleCount;
            out.rayDrivenTexGbufPso_ = b.createGraphicsPipeline(p);
        }

        // ---- Staged ray-driven passes (SM 6.6 for derivatives; root signature shared via giTex layout). ----
        const std::string csDefs = bindlessDefs + rdAblateDefs();
        const rhi::ShaderHandle csVis = compile("CSRdVisibility", rhi::ShaderStage::Compute, 65,
                                                rasterDefs(csDefs.c_str()).c_str());
        if (csVis) {
            rhi::ComputePipelineDesc p;
            p.cs = csVis;
            p.layout = giTex;
            out.rdVisCsPso_ = b.createComputePipeline(p);
        }
        // SM 6.6: compute-shader derivatives exist only from 6.6; below that, compile fails.
        // AVER_RD_SHADOW_KEYED: the sun's shadow history is only reused under the light it was traced for.
        const rhi::ShaderHandle csShadow =
            compile("CSRdShadow", rhi::ShaderStage::Compute, 66,
                    rasterDefs((csDefs + ";AVER_RD_SHADOW_KEYED=1").c_str()).c_str());
        if (csShadow) {
            rhi::ComputePipelineDesc p;
            p.cs = csShadow;
            p.layout = giTex;
            out.rdShadowCsPso_ = b.createComputePipeline(p);
        }
        // SUB-STAGE SPLIT A: CSRdShadowProbe, one group per 8x8 tile, writes to gRdShadowTiles (u18).
        const rhi::ShaderHandle csShadowProbe = compile("CSRdShadowProbe", rhi::ShaderStage::Compute, 66,
                                                        rasterDefs(csDefs.c_str()).c_str());
        if (csShadowProbe) {
            rhi::ComputePipelineDesc p;
            p.cs = csShadowProbe;
            p.layout = giTex;
            out.rdShadowProbeCsPso_ = b.createComputePipeline(p);
        }
        // CSRdShadow + AVER_RD_SHADOW_TILES=1: ORs 3x3 probe neighbourhood, skips per-pixel ray where probes agree.
        const rhi::ShaderHandle csShadowTiled =
            compile("CSRdShadow", rhi::ShaderStage::Compute, 66,
                    rasterDefs((csDefs + ";AVER_RD_SHADOW_TILES=1;AVER_RD_SHADOW_KEYED=1").c_str()).c_str());
        if (csShadowTiled) {
            rhi::ComputePipelineDesc p;
            p.cs = csShadowTiled;
            p.layout = giTex;
            out.rdShadowTiledCsPso_ = b.createComputePipeline(p);
        }
        // CSRdTailVis (the tail lights' shadow fraction) and CSRdTailFilter (its 5x5), after CSRdShadow.
        const rhi::ShaderHandle csTailVis = compile("CSRdTailVis", rhi::ShaderStage::Compute, 66,
                                                    rasterDefs(csDefs.c_str()).c_str());
        if (csTailVis) {
            rhi::ComputePipelineDesc p;
            p.cs = csTailVis;
            p.layout = giTex;
            out.rdTailVisCsPso_ = b.createComputePipeline(p);
        }
        const rhi::ShaderHandle csTailFilter = compile("CSRdTailFilter", rhi::ShaderStage::Compute, 66,
                                                       rasterDefs(csDefs.c_str()).c_str());
        if (csTailFilter) {
            rhi::ComputePipelineDesc p;
            p.cs = csTailFilter;
            p.layout = giTex;
            out.rdTailFilterCsPso_ = b.createComputePipeline(p);
        }
        // CSRdGi and CSRdSkyOcc: same SM 6.6 requirement as csShadow.
        const rhi::ShaderHandle csGi = compile("CSRdGi", rhi::ShaderStage::Compute, 66,
                                               rasterDefs(csDefs.c_str()).c_str());
        if (csGi) {
            rhi::ComputePipelineDesc p;
            p.cs = csGi;
            p.layout = giTex;
            out.rdGiCsPso_ = b.createComputePipeline(p);
        }
        // CSRdGi + AVER_GI_CHECKERBOARD=1: optional variant; rdStagedActive() ignores if not compiled.
        const rhi::ShaderHandle csGiCb = compile("CSRdGi", rhi::ShaderStage::Compute, 66,
                                                 rasterDefs((csDefs + ";AVER_GI_CHECKERBOARD=1").c_str()).c_str());
        if (csGiCb) {
            rhi::ComputePipelineDesc p;
            p.cs = csGiCb;
            p.layout = giTex;
            out.rdGiCbCsPso_ = b.createComputePipeline(p);
        }
        // SUB-STAGE SPLIT B: CSRdGiTrace, writes gRdGiCand (u17); checkerboard twin compacted to traced half's pixels.
        const rhi::ShaderHandle csGiTrace = compile("CSRdGiTrace", rhi::ShaderStage::Compute, 66,
                                                    rasterDefs(csDefs.c_str()).c_str());
        if (csGiTrace) {
            rhi::ComputePipelineDesc p;
            p.cs = csGiTrace;
            p.layout = giTex;
            out.rdGiTraceCsPso_ = b.createComputePipeline(p);
        }
        const rhi::ShaderHandle csGiTraceCb =
            compile("CSRdGiTrace", rhi::ShaderStage::Compute, 66,
                    rasterDefs((csDefs + ";AVER_GI_CHECKERBOARD=1").c_str()).c_str());
        if (csGiTraceCb) {
            rhi::ComputePipelineDesc p;
            p.cs = csGiTraceCb;
            p.layout = giTex;
            out.rdGiTraceCbCsPso_ = b.createComputePipeline(p);
        }
        // CSRdGi + AVER_GI_SPLIT=1: loads gRdGiCand instead of tracing; mirrors plain/checkerboard pair.
        const rhi::ShaderHandle csGiSplit =
            compile("CSRdGi", rhi::ShaderStage::Compute, 66,
                    rasterDefs((csDefs + ";AVER_GI_SPLIT=1").c_str()).c_str());
        if (csGiSplit) {
            rhi::ComputePipelineDesc p;
            p.cs = csGiSplit;
            p.layout = giTex;
            out.rdGiSplitCsPso_ = b.createComputePipeline(p);
        }
        const rhi::ShaderHandle csGiSplitCb =
            compile("CSRdGi", rhi::ShaderStage::Compute, 66,
                    rasterDefs((csDefs + ";AVER_GI_SPLIT=1;AVER_GI_CHECKERBOARD=1").c_str()).c_str());
        if (csGiSplitCb) {
            rhi::ComputePipelineDesc p;
            p.cs = csGiSplitCb;
            p.layout = giTex;
            out.rdGiSplitCbCsPso_ = b.createComputePipeline(p);
        }
        const rhi::ShaderHandle csSkyOcc = compile("CSRdSkyOcc", rhi::ShaderStage::Compute, 66,
                                                   rasterDefs(csDefs.c_str()).c_str());
        if (csSkyOcc) {
            rhi::ComputePipelineDesc p;
            p.cs = csSkyOcc;
            p.layout = giTex;
            out.rdSkyOccCsPso_ = b.createComputePipeline(p);
        }
        // CSRdRefl: ray-traced reflection lighting stage, same SM 6.6 requirement as csShadow.
        const rhi::ShaderHandle csRefl = compile("CSRdRefl", rhi::ShaderStage::Compute, 66,
                                                 rasterDefs(csDefs.c_str()).c_str());
        if (csRefl) {
            rhi::ComputePipelineDesc p;
            p.cs = csRefl;
            p.layout = giTex;
            out.rdReflCsPso_ = b.createComputePipeline(p);
        }
        // SUB-STAGE C: R1 traces and writes PENDING marker; R2 (CSRdReflFilter) finishes the compose.
        const rhi::ShaderHandle csReflSplit =
            compile("CSRdRefl", rhi::ShaderStage::Compute, 66,
                    rasterDefs((csDefs + ";AVER_RD_REFL_SPLIT=1").c_str()).c_str());
        if (csReflSplit) {
            rhi::ComputePipelineDesc p;
            p.cs = csReflSplit;
            p.layout = giTex;
            out.rdReflSplitCsPso_ = b.createComputePipeline(p);
        }
        const rhi::ShaderHandle csReflFilter = compile("CSRdReflFilter", rhi::ShaderStage::Compute, 66,
                                                        rasterDefs(csDefs.c_str()).c_str());
        if (csReflFilter) {
            rhi::ComputePipelineDesc p;
            p.cs = csReflFilter;
            p.layout = giTex;
            out.rdReflFilterCsPso_ = b.createComputePipeline(p);
        }
        // Stage B: pixel shader with AVER_RD_SPLIT=1; reads precomputed visibility, reuses vskyTex.
        const rhi::ShaderHandle psSplit =
            compile("PSRayDriven", rhi::ShaderStage::Pixel, 65,
                    rasterDefs((csDefs + ";AVER_RD_SPLIT=1").c_str()).c_str());
        if (vskyTex && psSplit) {
            rhi::GraphicsPipelineDesc p;
            p.vs = vskyTex; p.ps = psSplit;
            p.layout = giTex;
            p.cull = rhi::CullMode::None;
            p.depth = {true, true, rhi::CompareOp::Always};
            p.renderTargetCount = 1;
            p.renderTargets[0] = color;
            p.depthFormat = depth;
            p.sampleCount = sampleCount;
            out.rayDrivenSplitTexPso_ = b.createGraphicsPipeline(p);
        }
        const rhi::ShaderHandle psSplitGbuf =
            compile("PSRayDriven", rhi::ShaderStage::Pixel, 65,
                    rasterDefs((csDefs + ";AVER_GBUFFER=1;AVER_RD_SPLIT=1").c_str()).c_str());
        if (vskyTex && psSplitGbuf) {
            rhi::GraphicsPipelineDesc p;
            p.vs = vskyTex; p.ps = psSplitGbuf;
            p.layout = giTex;
            p.cull = rhi::CullMode::None;
            p.depth = {true, true, rhi::CompareOp::Always};
            p.renderTargetCount = 4;
            p.renderTargets[0] = color;
            p.renderTargets[1] = rhi::Format::RG16F;
            p.renderTargets[2] = rhi::Format::R32Float;
            p.renderTargets[3] = rhi::Format::RGB10A2Unorm;
            p.depthFormat = depth;
            p.sampleCount = sampleCount;
            out.rayDrivenSplitTexGbufPso_ = b.createGraphicsPipeline(p);
        }
        // Log once when staged pipelines are ready.

        // Blended variant with textured bindless table (for glass reflections).
        const rhi::ShaderHandle vsMainTex = compile("VSMain", rhi::ShaderStage::Vertex, kBaseSm,
                                                    rasterDefs(bindlessDefs.c_str()).c_str());
        // AVER_BLENDED_PASS: this variant leaves out decals (they skip translucent surfaces anyway); with
        // them compiled in it hung the RX 7800 XT at NewSponza Night's default view (docs/rendering/DECALS.md).
        const std::string blendedDefs = bindlessDefs + ";AVER_BLENDED_PASS=1";
        const rhi::ShaderHandle psMainTex = compile("PSMainVoxi", rhi::ShaderStage::Pixel, 65,
                                                    rasterDefs(blendedDefs.c_str()).c_str());
        if (vsMainTex && psMainTex) {
            rhi::GraphicsPipelineDesc p = scene;
            p.vs = vsMainTex; p.ps = psMainTex;
            p.layout = giTex;
            p.blend = rhi::BlendMode::PremultipliedAlpha;   // see sceneBlendedPso_ for why
            p.depth.test = true;
            p.depth.write = false;
            out.sceneRtBlendedTexPso_ = b.createGraphicsPipeline(p);
        }

    }

    // PSRayDriven's own G-buffer twin: RayDrivenGBufferOut (five targets); built with Always/write-on depth.
    const rhi::ShaderHandle psRayDrivenGbuf =
        rtOk ? compile("PSRayDriven", rhi::ShaderStage::Pixel, 65,
                       rasterDefs((std::string("AVER_RT=1;AVER_GBUFFER=1") + kRdSinglePassDefs + rdAblateDefs()).c_str()).c_str())
             : 0;
    if (vsky && psRayDrivenGbuf) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsky; p.ps = psRayDrivenGbuf;
        p.layout = gi;
        p.cull = rhi::CullMode::None;
        p.depth = {true, true, rhi::CompareOp::Always};
        p.renderTargetCount = 4;
        p.renderTargets[0] = color;
        p.renderTargets[1] = rhi::Format::RG16F;
        p.renderTargets[2] = rhi::Format::R32Float;
        p.renderTargets[3] = rhi::Format::RGB10A2Unorm;
        p.depthFormat = depth;
        p.sampleCount = sampleCount;
        out.rayDrivenGbufPso_ = b.createGraphicsPipeline(p);
    }

    if (msMain && psRt) {
        rhi::GraphicsPipelineDesc p = scene;
        p.ms = msMain; p.ps = psRt;
        out.sceneMsRtPso_ = b.createGraphicsPipeline(p);
    }

    if (msMain && psRtGbuf) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.ms = msMain; p.ps = psRtGbuf;
        out.sceneMsRtGbufPso_ = b.createGraphicsPipeline(p);
    }

    // Blended twins: PremultipliedAlpha + depth write off.
    if (vsMain && psVoxi) {
        rhi::GraphicsPipelineDesc p = scene;
        p.vs = vsMain; p.ps = psVoxi;
        p.blend = rhi::BlendMode::PremultipliedAlpha;
        p.depth.test = true;    // inherited from `scene`
        p.depth.write = false;
        out.sceneBlendedPso_ = b.createGraphicsPipeline(p);
    }


    if (msMain && psVoxi) {
        rhi::GraphicsPipelineDesc p = scene;
        p.ms = msMain; p.ps = psVoxi;
        p.blend = rhi::BlendMode::PremultipliedAlpha;   // see sceneBlendedPso_ above for why
        p.depth.test = true;    // same test-on/write-off state as sceneBlendedPso_ above
        p.depth.write = false;
        out.sceneMsBlendedPso_ = b.createGraphicsPipeline(p);
    }


    if (vsMain && psRt) {
        rhi::GraphicsPipelineDesc p = scene;
        p.vs = vsMain; p.ps = psRt;
        p.blend = rhi::BlendMode::PremultipliedAlpha;   // see sceneBlendedPso_ above for why
        p.depth.test = true;    // same test-on/write-off state as sceneBlendedPso_ above
        p.depth.write = false;
        out.sceneRtBlendedPso_ = b.createGraphicsPipeline(p);
    }


    if (msMain && psRt) {
        rhi::GraphicsPipelineDesc p = scene;
        p.ms = msMain; p.ps = psRt;
        p.blend = rhi::BlendMode::PremultipliedAlpha;   // see sceneBlendedPso_ above for why
        p.depth.test = true;    // same test-on/write-off state as sceneBlendedPso_ above
        p.depth.write = false;
        out.sceneMsRtBlendedPso_ = b.createGraphicsPipeline(p);
    }


    // Depth prepass and two scene variants that trust it.
    const rhi::ShaderHandle psDepthPrepass =
        compile("PSDepthPrepass", rhi::ShaderStage::Pixel, kBaseSm, rasterDefs(nullptr).c_str());
    if (vsMain && psDepthPrepass) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsMain;
        p.ps = psDepthPrepass;
        p.layout = gi;                 // Same table-0 shape as colour pass.
        p.cull = rhi::CullMode::None;   // matches `scene.cull`.
        p.depth = {true, true, rhi::CompareOp::Less};
        p.renderTargetCount = 0;        // depth-only pass.
        p.depthFormat = depth;
        p.sampleCount = sampleCount;
        out.depthPrepassPso_ = b.createGraphicsPipeline(p);
    }

    // Colour-pass twins: LessEqual depth test with writes OFF. LessEqual allows bit-identical geometry.
    if (vsMain && psVoxi) {
        rhi::GraphicsPipelineDesc p = scene;
        p.vs = vsMain; p.ps = psVoxi;
        p.depth = {true, false, rhi::CompareOp::LessEqual};
        out.scenePsoPrepassed_ = b.createGraphicsPipeline(p);
    }

    if (vsMain && psVoxiGbuf) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.vs = vsMain; p.ps = psVoxiGbuf;
        p.depth = {true, false, rhi::CompareOp::LessEqual};
        out.scenePsoPrepassedGbuf_ = b.createGraphicsPipeline(p);
    }

    if (rtOk && vsMain && psRt) {
        rhi::GraphicsPipelineDesc p = scene;
        p.vs = vsMain; p.ps = psRt;
        p.depth = {true, false, rhi::CompareOp::LessEqual};
        out.sceneRtPsoPrepassed_ = b.createGraphicsPipeline(p);
    }

    if (rtOk && vsMain && psRtGbuf) {
        rhi::GraphicsPipelineDesc p = sceneGbuf;
        p.vs = vsMain; p.ps = psRtGbuf;
        p.depth = {true, false, rhi::CompareOp::LessEqual};
        out.sceneRtPsoPrepassedGbuf_ = b.createGraphicsPipeline(p);
    }

}

// Records the radiance-cache twins: AVER_NEURAC=1 variants of CSRdGi / CSRdGiTrace, plain and checkerboard.
void VoxiRenderer::recordRcTwins(rhi::IPipelineBatch& b, PsoLocal& out) {
    const bool rtOk = caps_.rayTracingTier >= 11 && caps_.shaderModel >= 66 && caps_.dxcAvailable;
    if (!rtOk || !rtTexTable_) {
        AVER_WARN("[Voxi] NeuRaC twins not built: needs ray tracing, shader model 6.6 and the "
                  "bindless texture table (said once per pipeline build)");
        return;
    }
    ShaderScope compile(b);
    const rhi::PipelineLayout giTex = giLayout(kRtTextureCapacity);
    const std::string matDefs = pbr::materialShaderDefines(giLayout().srvCount, kMaterialSamplerSlot,
                                                          layeredBsdf_);
    const std::string bindlessDefs = "AVER_RT=1;AVER_RT_BINDLESS=1;AVER_RT_TEX_CAPACITY=" +
                                     std::to_string(kRtTextureCapacity);
    const std::string csDefs = bindlessDefs + (rdAblate_ ? (";AVER_RD_ABLATE=" + std::to_string(rdAblate_))
                                                          : std::string());
    const auto build = [&](const char* entry, const char* extra, rhi::PipelineHandle& slot) {
        const std::string defs = matDefs + ";" + csDefs + ";AVER_NEURAC=1" + extra;
        rhi::ComputePipelineDesc p;
        p.cs = compile(entry, rhi::ShaderStage::Compute, 66, defs.c_str());
        p.layout = giTex;
        slot = b.createComputePipeline(p);
    };
    build("CSRdGi",      "",                        out.rdGiCacheCsPso_);
    build("CSRdGi",      ";AVER_GI_CHECKERBOARD=1", out.rdGiCacheCbCsPso_);
    build("CSRdGiTrace", "",                        out.rdGiTraceCacheCsPso_);
    build("CSRdGiTrace", ";AVER_GI_CHECKERBOARD=1", out.rdGiTraceCacheCbCsPso_);
}

// Records the Path Tracing twins: AVER_PT_PATHS=1 variants of CSRdGi / CSRdGiTrace (plain and checkerboard)
// and CSRdRefl (plain and split), the same over the radiance cache, and the Reference mode's CSRdPtRef.
void VoxiRenderer::recordPtTwins(rhi::IPipelineBatch& b, PsoLocal& out) {
    const bool rtOk = caps_.rayTracingTier >= 11 && caps_.shaderModel >= 66 && caps_.dxcAvailable;
    if (!rtOk || !rtTexTable_) {
        AVER_WARN("[Voxi] Path Tracing pipelines not built: needs ray tracing, shader model 6.6 and the "
                  "bindless texture table (said once per pipeline build)");
        return;
    }
    ShaderScope compile(b);
    const rhi::PipelineLayout giTex = giLayout(kRtTextureCapacity);
    const std::string matDefs = pbr::materialShaderDefines(giLayout().srvCount, kMaterialSamplerSlot,
                                                          layeredBsdf_);
    const std::string csDefs = "AVER_RT=1;AVER_RT_BINDLESS=1;AVER_RT_TEX_CAPACITY=" +
                               std::to_string(kRtTextureCapacity) +
                               (rdAblate_ ? (";AVER_RD_ABLATE=" + std::to_string(rdAblate_)) : std::string());
    const auto build = [&](const char* entry, const char* extra, rhi::PipelineHandle& slot) {
        const std::string defs = matDefs + ";" + csDefs + ";AVER_PT_PATHS=1" + extra;
        rhi::ComputePipelineDesc p;
        p.cs = compile(entry, rhi::ShaderStage::Compute, 66, defs.c_str());
        p.layout = giTex;
        slot = b.createComputePipeline(p);
    };
    build("CSRdGi",      "",                        out.rdGiPtCsPso_);
    build("CSRdGi",      ";AVER_GI_CHECKERBOARD=1", out.rdGiPtCbCsPso_);
    build("CSRdGiTrace", "",                        out.rdGiTracePtCsPso_);
    build("CSRdGiTrace", ";AVER_GI_CHECKERBOARD=1", out.rdGiTracePtCbCsPso_);
    build("CSRdRefl",    "",                        out.rdReflPtCsPso_);
    build("CSRdRefl",    ";AVER_RD_REFL_SPLIT=1",   out.rdReflSplitPtCsPso_);
    // Over the radiance cache, plain and half-rate checkerboard.
    build("CSRdGi",      ";AVER_NEURAC=1",          out.rdGiPtRcCsPso_);
    build("CSRdGiTrace", ";AVER_NEURAC=1",          out.rdGiTracePtRcCsPso_);
    build("CSRdGi",      ";AVER_NEURAC=1;AVER_GI_CHECKERBOARD=1", out.rdGiPtRcCbCsPso_);
    build("CSRdGiTrace", ";AVER_NEURAC=1;AVER_GI_CHECKERBOARD=1", out.rdGiTracePtRcCbCsPso_);
    build("CSRdPtRef",   "",                        out.rdPtRefCsPso_);
    build("CSRdRefl",    ";AVER_NEURAC=1",                         out.rdReflPtRcCsPso_);
    build("CSRdRefl",    ";AVER_NEURAC=1;AVER_RD_REFL_SPLIT=1",   out.rdReflSplitPtRcCsPso_);
}

// Records the NRD2 group: its compute set when it has none yet, its compose draw, and Voxi's own Stage B
// NRD2 variant and half-rate fill. The compose draw and Stage B bake the formats of the scene set.
void VoxiRenderer::recordNrd2Pipelines(rhi::IPipelineBatch& b, Build& bd) {
    const rhi::Format gbuf[3] = {rhi::Format::RG16F, rhi::Format::R32Float, rhi::Format::RGB10A2Unorm};
    bd.nrd2Compute = !nrd2_.valid();
    if (!nrd2_.recordBuild(*dev_, b, bd.nrd2Compute, true, bd.color, gbuf, bd.depth, bd.samples, bd.nrd2Plan))
        return;
    if (!rtTexTable_) return;
    ShaderScope compile(b);
    const std::string matDefs = pbr::materialShaderDefines(giLayout().srvCount, kMaterialSamplerSlot,
                                                          layeredBsdf_);
    const std::string bindless = "AVER_RT=1;AVER_RT_BINDLESS=1;AVER_RT_TEX_CAPACITY=" +
                                 std::to_string(kRtTextureCapacity);
    const std::string ablate = rdAblate_ ? (";AVER_RD_ABLATE=" + std::to_string(rdAblate_)) : std::string();
    {
        rhi::GraphicsPipelineDesc p;
        p.vs = compile("VSky", rhi::ShaderStage::Vertex, kBaseSm, (matDefs + ";" + bindless).c_str());
        p.ps = compile("PSRayDriven", rhi::ShaderStage::Pixel, 65,
                       (matDefs + ";" + bindless + ablate + ";AVER_GBUFFER=1;AVER_RD_SPLIT=1;AVER_NRD2=1").c_str());
        p.layout = giLayout(kRtTextureCapacity);
        p.cull = rhi::CullMode::None;
        p.depth = {true, true, rhi::CompareOp::Always};
        p.renderTargetCount = 4;
        p.renderTargets[0] = bd.color;
        p.renderTargets[1] = rhi::Format::RG16F;
        p.renderTargets[2] = rhi::Format::R32Float;
        p.renderTargets[3] = rhi::Format::RGB10A2Unorm;
        p.depthFormat = bd.depth;
        p.sampleCount = bd.samples;
        bd.local.rayDrivenSplitNrd2Pso_ = b.createGraphicsPipeline(p);
    }
    // Optional: without it NRD2 frames trace every feature at full rate.
    rhi::ComputePipelineDesc p;
    p.cs = compile("CSRdHalfFill", rhi::ShaderStage::Compute, 66, (matDefs + ";" + bindless + ablate).c_str());
    p.layout = giLayout(kRtTextureCapacity);
    bd.local.rdHalfFillCsPso_ = b.createComputePipeline(p);
}

// ---------------------------------------------------------------- pipeline builds
// docs/rendering/ASYNC_SHADERS.md. A build is recorded on this thread into a pipeline batch (a pure list of
// requests, copied), run by workers (or inline in synchronous mode), and adopted here, on this thread, at a
// frame boundary: prePass before it records anything, or init() / a synchronous request outside a frame.
// Nothing on a worker touches this renderer.

bool VoxiRenderer::buildInFlight(unsigned groups) const {
    for (const std::unique_ptr<Build>& b : builds_)
        if (b->groups & groups) return true;
    return false;
}

bool VoxiRenderer::shaderBuildProgress(u32& done, u32& total) const {
    done = total = 0;
    for (const std::unique_ptr<Build>& b : builds_) {
        u32 d = 0, t = 0;
        b->batch->progress(d, t);
        done += d;
        total += t;
    }
    return !builds_.empty();
}

// A lazy group (NRD2, Path Tracing, NeuRaC twins) is wanted. Started at the next frame boundary; in
// synchronous mode built and adopted before this returns.
void VoxiRenderer::requestBuild(unsigned group) {
    lazyRequested_ |= group;
    // Mid-frame in synchronous mode: build what was asked for, but leave the scene set to the next boundary.
    if (!asyncBuilds_) pumpBuilds(true, false);
}

void VoxiRenderer::startBuild(unsigned groups) {
    if (groups & kPsoScene) {
        // The scene generation moves on: whatever was recorded against the old one is dropped when it
        // lands, and builds that carry nothing else are not worth finishing.
        ++sceneGen_;
        for (const std::unique_ptr<Build>& o : builds_)
            if (!(o->groups & kPsoBase)) o->batch->cancel();
        // A rebuild covers the lazy groups that are live, so they swap with the set they were built for.
        if (rcTwinsTried_) groups |= kPsoRc;
        if (ptTwinsTried_) groups |= kPsoPt;
        if (nrd2Tried_)    groups |= kPsoNrd2;
    }
    std::unique_ptr<Build> bd = std::make_unique<Build>();
    bd->groups = groups;
    bd->sceneGen = sceneGen_;
    bd->samples = wantSamples_;
    bd->color = wantColor_;
    bd->depth = wantDepth_;
    bd->batch = rhi::createPipelineBatch(*res_, asyncBuilds_);
    rhi::IPipelineBatch& b = *bd->batch;
    if (groups & kPsoBase) {
        baseStarted_ = true;
        recordBasePipelines(b, bd->local);
    }
    if (groups & kPsoScene) {
        ensureTextureTable();
        recSamples_ = bd->samples;
        recColor_ = bd->color;
        recDepth_ = bd->depth;
        scenePipelineGraphRev_ = pbr::materialGraphs().revision();
        scenePipelineShaderRev_ = rhi::shaderFileRevision();
        recordScenePipelines(b, bd->local, bd->samples, bd->color, bd->depth);
    }
    if (groups & kPsoRc) { rcTwinsTried_ = true; recordRcTwins(b, bd->local); }
    if (groups & kPsoPt) { ptTwinsTried_ = true; recordPtTwins(b, bd->local); }
    if (groups & kPsoNrd2) { nrd2Tried_ = true; recordNrd2Pipelines(b, *bd); }
    bd->batch->start();
    builds_.push_back(std::move(bd));
}

// What is due now: the scene set again when the targets, the material graphs or the shader text moved from
// what the last one was recorded against, and the lazy groups something asked for. Frame boundary only.
void VoxiRenderer::startDueBuilds(bool sceneDue) {
    const u64 graphRev = pbr::materialGraphs().revision();
    const u64 shaderRev = rhi::shaderFileRevision();
    const bool targetsMoved = recSamples_ != wantSamples_ || recColor_ != wantColor_ || recDepth_ != wantDepth_;
    if (sceneDue && (targetsMoved || scenePipelineGraphRev_ != graphRev || scenePipelineShaderRev_ != shaderRev)) {
        if (targetsMoved)
            AVER_INFO("[Voxi] rebuilding scene pipelines for the new render targets ({} sample(s))", wantSamples_);
        else if (scenePipelineGraphRev_ != graphRev)
            AVER_INFO("[Voxi] rebuilding scene pipelines for {} material graph(s)", pbr::materialGraphs().count());
        else
            AVER_INFO("[Voxi] rebuilding scene pipelines: shader files changed (revision {})", shaderRev);
        startBuild(kPsoScene);
        return;
    }
    // The lazy groups are keyed to the scene set: only once it stands and nothing replaces it.
    if (!giReady_ || buildInFlight(kPsoBase | kPsoScene)) return;
    unsigned due = 0;
    if (!rcTwinsTried_ && ((lazyRequested_ & kPsoRc) || giRestirVisibility_ == 4u)) due |= kPsoRc;
    if (!ptTwinsTried_ && ((lazyRequested_ & kPsoPt) || pathTracingWanted())) due |= kPsoPt;
    if (!nrd2Tried_ && (lazyRequested_ & kPsoNrd2)) due |= kPsoNrd2;
    due &= ~(buildInFlight(kPsoRc) ? kPsoRc : 0u) & ~(buildInFlight(kPsoPt) ? kPsoPt : 0u) &
           ~(buildInFlight(kPsoNrd2) ? kPsoNrd2 : 0u);
    if (due) startBuild(due);
}

// Adopts what finished and starts what is due. `wait` blocks until every build has landed (init, synchronous
// mode, buildAllVariants); the frame boundary never waits.
void VoxiRenderer::pumpBuilds(bool wait, bool sceneDue) {
    if (!initialised_ || failed_ || !res_) return;
    for (;;) {
        for (usize i = 0; i < builds_.size();) {
            Build& bd = *builds_[i];
            if (wait) bd.batch->waitFinished();
            if (!bd.batch->finished()) { ++i; continue; }
            std::unique_ptr<Build> done = std::move(builds_[i]);
            builds_.erase(builds_.begin() + static_cast<isize>(i));
            adoptBuild(*done);
            if (failed_) break;
        }
        if (failed_) break;
        startDueBuilds(sceneDue);
        if (!wait || builds_.empty()) break;
    }
    refreshReady();
}

// Takes a finished build's pipelines. A group replaces its previous handles together, so the frame that
// follows sees all of the new or all of the old.
void VoxiRenderer::adoptBuild(Build& bd) {
    rhi::IPipelineBatch& b = *bd.batch;
    // The scene generation moved on since this was recorded: only the base group is still good.
    unsigned apply = bd.groups;
    if (bd.sceneGen != sceneGen_) apply &= kPsoBase;
    if (!apply) {
        b.cancel();   // never adopted: the batch frees what it built
        return;
    }
    b.adopt();
    // Built but not taken: straight back to the factory.
    const auto tossGroups = [&](unsigned groups) {
        const auto toss = [&](rhi::PipelineHandle local) {
            if (const rhi::PipelineHandle h = b.resolve(local)) res_->destroyPipeline(h);
        };
#define AVER_VOXI_TOSS(n) toss(bd.local.n);
        if (groups & kPsoScene) { AVER_VOXI_PSO_SCENE(AVER_VOXI_TOSS) }
        if (groups & kPsoRc)    { AVER_VOXI_PSO_RC(AVER_VOXI_TOSS) }
        if (groups & kPsoPt)    { AVER_VOXI_PSO_PT(AVER_VOXI_TOSS) }
        if (groups & kPsoNrd2)  { AVER_VOXI_PSO_NRD2(AVER_VOXI_TOSS) nrd2_.discardBuild(b, bd.nrd2Plan); }
#undef AVER_VOXI_TOSS
    };
    // The scene generation moved on since this was recorded.
    tossGroups(bd.groups & ~apply);
    // A rebuild for the same targets (material graph, edited shader) whose core pipeline would not build: the
    // set that stands keeps drawing, as it did before sets were swapped whole. Nothing is retried until the
    // next change, since the revisions it was recorded against are already noted.
    if ((apply & kPsoScene) && sceneAdopted_ && builtSamples_ == bd.samples && builtColor_ == bd.color &&
        builtDepth_ == bd.depth && (!b.resolve(bd.local.scenePso_) || !b.resolve(bd.local.debugPso_))) {
        AVER_ERROR("[Voxi] scene pipelines could not be rebuilt; the previous pipelines keep drawing");
        tossGroups(apply);
        apply &= kPsoBase;
        if (!apply) return;
    }

    // A new scene set replaces the lazy groups too: those that were live are in this build, the rest are 0.
    const unsigned replace = apply | ((apply & kPsoScene) ? (kPsoRc | kPsoPt | kPsoNrd2) : 0u);
#define AVER_VOXI_TAKE(n) { if (n) res_->destroyPipeline(n); n = b.resolve(bd.local.n); }
    if (replace & kPsoBase)  { AVER_VOXI_PSO_BASE(AVER_VOXI_TAKE) baseReady_ = true; }
    if (replace & kPsoScene) { AVER_VOXI_PSO_SCENE(AVER_VOXI_TAKE) }
    if (replace & kPsoRc)    { AVER_VOXI_PSO_RC(AVER_VOXI_TAKE) }
    if (replace & kPsoPt)    { AVER_VOXI_PSO_PT(AVER_VOXI_TAKE) }
    if (replace & kPsoNrd2)  { AVER_VOXI_PSO_NRD2(AVER_VOXI_TAKE) }
#undef AVER_VOXI_TAKE

    if (apply & kPsoNrd2) {
        const bool ok = nrd2_.finishBuild(b, bd.nrd2Plan, bd.nrd2Compute, true);
        if (ok && rayDrivenSplitNrd2Pso_)
            AVER_INFO("[Voxi] NRD2 ready (Stage B split variant, pyramid/resolve passes, compose draw)");
    } else if (replace & kPsoNrd2) {
        nrd2_.destroyCompose();   // it baked the old formats
    }
    if (apply & kPsoScene) {
        builtSamples_ = bd.samples;
        builtColor_ = bd.color;
        builtDepth_ = bd.depth;
        sceneSampleCount_ = bd.samples;
        sceneColorFmt_ = bd.color;
        sceneDepthFmt_ = bd.depth;
        sceneAdopted_ = true;
        rcTwinsTried_ = (apply & kPsoRc) != 0;
        ptTwinsTried_ = (apply & kPsoPt) != 0;
        nrd2Tried_ = (apply & kPsoNrd2) != 0;
    }
    reportPsoResults(bd.local, apply);

    if (!coreChecked_ && baseReady_ && sceneAdopted_) validateCore();
    // A lazy group landing under a running frame changes which estimator or denoiser it uses next: start
    // those histories over (a scene landing does it itself, in refreshReady).
    if (!failed_ && giReady_ && (apply & (kPsoRc | kPsoPt | kPsoNrd2)) && !(apply & kPsoScene)) resetHistoriesForNewPipelines();
}

// The checks init() used to make once the pipelines existed.
void VoxiRenderer::validateCore() {
    const char* missing = !shadowPso_  ? "shadow pipeline"
                        : !voxelPso_   ? "voxelise pipeline"
                        : !clearPso_   ? "volume clear pipeline"
                        : !resolvePso_ ? "injection resolve pipeline"
                        : !mipPso_     ? "mip filter pipeline"
                        : !debugPso_   ? "voxel debug pipeline"
                        : !scenePso_   ? "scene pipeline"
                                       : nullptr;
    AVER_INFO("[Voxi] pipelines: shadow={}/{} voxel={} voxelMs={} clear={} resolve={} mip={} debug={} "
              "scene={}/{}/{}/{} blended={}/{}/{}/{} airVis={}",
              shadowPso_, shadowInstancedPso_, voxelPso_, voxelMsPso_, clearPso_, resolvePso_, mipPso_, debugPso_,
              scenePso_, sceneMsPso_, sceneRtPso_, sceneMsRtPso_,
              sceneBlendedPso_, sceneMsBlendedPso_, sceneRtBlendedPso_, sceneMsRtBlendedPso_, airVisPso_);
    if (missing) {
        AVER_ERROR("[Voxi] init FAILED: {} has a zero handle", missing);
        failed_ = true;
        return;
    }
    coreChecked_ = true;
    // The ray-tracing variant is what rtSupported_ stands for; init assumed it from the device's capabilities.
    if (rtSupported_ && !sceneRtPso_) {
        AVER_WARN("[Voxi] the ray-tracing scene variant did not build; ray tracing stays off");
        rtSupported_ = false;
        tlas_ = 0;
    }
    // CSAirVis exists now, so the fog-occlusion volume can be made.
    if (!ensureAirVis())
        AVER_WARN("[Voxi] air sky-visibility volume unavailable; fog stays unoccluded "
                  "(voxi.fogOcclusion has no effect until it can be created)");
    AVER_INFO("[Voxi] ready: conservative raster {}, mesh-shader variants {}, ray-tracing variants {}, "
              "blended (glass) variant {}, G-buffer variant {} (off by default -- dev_->gBufferEnabled() "
              "is what turns it on per frame; see pickGbuf())",
              caps_.conservativeRaster ? "on" : "off",
              (voxelMsPso_ && sceneMsPso_) ? "built" : "absent",
              rtSupported_ ? "built" : "absent",
              sceneBlendedPso_ ? "built" : "absent",
              sceneGbufPso_ ? "built" : "absent");
}

// Recomputes whether the renderer can draw. The only place giReady_ and the blank-frame latch change, always
// at a frame boundary (or outside a frame): a frame sees one value from its first command to its last.
void VoxiRenderer::refreshReady() {
    const bool formatsOk = sceneAdopted_ && builtSamples_ == wantSamples_ && builtColor_ == wantColor_ &&
                           builtDepth_ == wantDepth_;
    const bool ready = initialised_ && !failed_ && coreChecked_ && formatsOk;
    targetsStale_ = sceneAdopted_ && !formatsOk;
    if (ready && !giReady_) {
        giReady_ = true;
        onSceneLanded();
    } else if (!ready) {
        giReady_ = false;
    }
    blocked_ = initialised_ && !failed_ && !giReady_;
}

// The scene set stands after not standing: every history was written under a different (or no) renderer.
void VoxiRenderer::onSceneLanded() {
    AVER_INFO("[Voxi] scene pipelines ready; the frame draws from here");
    resetHistoriesForNewPipelines();
    if (dev_) {
        dev_->noteSceneCut();
        if (rhi::IUpscaler* u = dev_->upscaler()) u->reset();
    }
}

void VoxiRenderer::resetHistoriesForNewPipelines() {
    resetGiHistory(true);
    resetRtHistory(true);
    resetDenoiserHistory(true);
    ptAccumValid_ = false;
    nrd2_.resetHistory();
}

// Says which requested pipelines did not build (a request is a nonzero local handle; the result a zero one).
void VoxiRenderer::reportPsoResults(const PsoLocal& l, unsigned groups) const {
    const auto note = [](rhi::PipelineHandle asked, rhi::PipelineHandle got, bool error, const char* text) {
        if (!asked || got) return;
        if (error) AVER_ERROR("[Voxi] {}", text);
        else       AVER_WARN("[Voxi] {}", text);
    };
    if (groups & kPsoBase) {
        note(l.shadowPso_, shadowPso_, true, "shadow pipeline unavailable");
        note(l.shadowInstancedPso_, shadowInstancedPso_, false,
             "instanced shadow pipeline unavailable; shadowPass falls back to one draw per instance");
        note(l.giShadowPso_, giShadowPso_, false,
             "GI-only shadow pipeline unavailable; indirect light is injected unshadowed");
        note(l.giShadowInstancedPso_, giShadowInstancedPso_, false, "instanced GI-only shadow pipeline unavailable");
        note(l.voxelPso_, voxelPso_, true, "voxelise pipeline unavailable");
        note(l.voxelMsPso_, voxelMsPso_, false, "mesh-shader voxelise variant unavailable; the GS path stands in");
        note(l.clearPso_, clearPso_, true, "volume clear pipeline unavailable");
        note(l.resolvePso_, resolvePso_, true, "injection resolve pipeline unavailable");
        note(l.mipPso_, mipPso_, true, "mip filter pipeline unavailable");
        if (!airVisPso_)
            AVER_WARN("[Voxi] air sky-visibility pipeline unavailable (SM {}, DXC {}); "
                      "voxi.fogOcclusion has no effect on this device (placeholder stays bound)",
                      caps_.shaderModel, caps_.dxcAvailable ? "yes" : "no");
    }
    if (groups & kPsoScene) {
        note(l.debugPso_, debugPso_, true, "voxel debug pipeline unavailable");
        note(l.scenePso_, scenePso_, true, "scene pipeline unavailable");
        note(l.sceneGbufPso_, sceneGbufPso_, false, "G-buffer scene pipeline unavailable; the G-buffer stays off even if requested");
        note(l.sceneMsPso_, sceneMsPso_, false, "mesh-shader scene variant unavailable");
        note(l.sceneMsGbufPso_, sceneMsGbufPso_, false, "mesh-shader G-buffer scene variant unavailable");
        note(l.sceneRtPso_, sceneRtPso_, false, "ray-tracing scene variant unavailable");
        note(l.sceneRtGbufPso_, sceneRtGbufPso_, false, "ray-tracing G-buffer scene variant unavailable");
        note(l.rayDrivenPso_, rayDrivenPso_, false, "ray-driven primary-visibility pass unavailable");
        note(l.rayDrivenTexPso_, rayDrivenTexPso_, false,
             "textured ray-driven pass unavailable; hits will shade from material factors alone");
        note(l.rayDrivenTexGbufPso_, rayDrivenTexGbufPso_, false,
             "textured G-buffer ray-driven pass unavailable; --gbuffer will fall back to the flat-albedo G-buffer pipeline");
        note(l.sceneRtBlendedTexPso_, sceneRtBlendedTexPso_, false,
             "textured blended (glass) variant unavailable; reflections in glass will stay flat");
        note(l.rayDrivenGbufPso_, rayDrivenGbufPso_, false, "ray-driven G-buffer primary-visibility pass unavailable");
        note(l.sceneMsRtPso_, sceneMsRtPso_, false, "mesh-shader + ray-tracing scene variant unavailable");
        note(l.sceneMsRtGbufPso_, sceneMsRtGbufPso_, false, "mesh-shader + ray-tracing G-buffer scene variant unavailable");
        note(l.sceneBlendedPso_, sceneBlendedPso_, false, "blended scene pipeline unavailable; translucent materials will not draw");
        note(l.sceneMsBlendedPso_, sceneMsBlendedPso_, false, "mesh-shader blended scene variant unavailable");
        note(l.sceneRtBlendedPso_, sceneRtBlendedPso_, false, "ray-traced blended scene variant unavailable");
        note(l.sceneMsRtBlendedPso_, sceneMsRtBlendedPso_, false, "mesh-shader + ray-traced blended scene variant unavailable");
        note(l.depthPrepassPso_, depthPrepassPso_, false, "depth prepass pipeline unavailable; --depth-prepass will have no effect");
        note(l.scenePsoPrepassed_, scenePsoPrepassed_, false, "prepassed scene pipeline unavailable; --depth-prepass will have no effect");
        note(l.scenePsoPrepassedGbuf_, scenePsoPrepassedGbuf_, false,
             "prepassed G-buffer scene pipeline unavailable; --depth-prepass and the G-buffer will not combine even though each works alone");
        note(l.sceneRtPsoPrepassed_, sceneRtPsoPrepassed_, false,
             "prepassed ray-traced scene pipeline unavailable; ray tracing keeps its normal depth state under --depth-prepass");
        note(l.sceneRtPsoPrepassedGbuf_, sceneRtPsoPrepassedGbuf_, false,
             "prepassed ray-traced G-buffer scene pipeline unavailable; ray tracing keeps its normal depth state under --depth-prepass with the G-buffer on, same as without it");
        if (l.rdVisCsPso_) {
            if (rdVisCsPso_ && rdShadowCsPso_ && rayDrivenSplitTexPso_)
                AVER_INFO("[Voxi] staged ray-driven passes ready for voxi.rayDrivenStages ({} texture slots, "
                          "G-buffer twin {}, GI stage {}, sky occlusion stage {}, reflection stage {}, "
                          "half-rate GI checkerboard stage {}, shadow-tile sub-stage {}, GI-trace "
                          "sub-stage {}, reflection register/filter sub-stage {})",
                          kRtTextureCapacity,
                          rayDrivenSplitTexGbufPso_ ? "ready" : "unavailable",
                          rdGiCsPso_ ? "ready" : "unavailable", rdSkyOccCsPso_ ? "ready" : "unavailable",
                          rdReflCsPso_ ? "ready" : "unavailable", rdGiCbCsPso_ ? "ready" : "unavailable",
                          (rdShadowProbeCsPso_ && rdShadowTiledCsPso_) ? "ready" : "unavailable",
                          (rdGiTraceCsPso_ && rdGiSplitCsPso_) ? "ready" : "unavailable",
                          (rdReflSplitCsPso_ && rdReflFilterCsPso_) ? "ready" : "unavailable");
            else
                AVER_WARN("[Voxi] staged ray-driven passes unavailable (visibility cs {}, shadow cs {}, "
                          "split pixel shader {}); voxi.rayDrivenStages 1 or 2 falls back to the single pass",
                          rdVisCsPso_ ? "ready" : "missing", rdShadowCsPso_ ? "ready" : "missing",
                          rayDrivenSplitTexPso_ ? "ready" : "missing");
        }
    }
    if ((groups & kPsoRc) && l.rdGiCacheCsPso_) {
        const u32 built = (rdGiCacheCsPso_ ? 1u : 0u) + (rdGiCacheCbCsPso_ ? 1u : 0u) +
                          (rdGiTraceCacheCsPso_ ? 1u : 0u) + (rdGiTraceCacheCbCsPso_ ? 1u : 0u);
        if (built == 4u)
            AVER_INFO("[Voxi] NeuRaC twin pipelines ready (CSRdGi/CSRdGiTrace x plain/checkerboard)");
        else
            AVER_WARN("[Voxi] NeuRaC twin pipelines: {} of 4 compiled; a variant without its twin "
                      "runs as plain HalfResolution", built);
    }
    if ((groups & kPsoPt) && l.rdGiPtCsPso_) {
        if (!rdPtRefCsPso_)
            AVER_WARN("[Voxi] the reference path tracing pass did not compile; Reference mode runs as ReSTIR");
        if (!rdGiPtRcCsPso_ || !rdGiTracePtRcCsPso_)
            AVER_WARN("[Voxi] the Path Tracing pipelines over the radiance cache did not compile; paths run "
                      "without it");
        const bool ok = rdGiPtCsPso_ && rdGiPtCbCsPso_ && rdGiTracePtCsPso_ && rdGiTracePtCbCsPso_ &&
                        rdReflPtCsPso_ && rdReflSplitPtCsPso_;
        if (ok) AVER_INFO("[Voxi] Path Tracing pipelines ready (ReSTIR GI paths and reflection paths)");
        else    AVER_WARN("[Voxi] some Path Tracing pipelines did not compile; those stages run as ordinary "
                          "ray-driven passes");
    }
    if ((groups & kPsoNrd2) && l.rdHalfFillCsPso_ && !rdHalfFillCsPso_)
        AVER_WARN("[Voxi] NRD2's half-rate fill (CSRdHalfFill) did not compile; NRD2 frames trace "
                  "GI, reflections and sky occlusion at full rate");
}

} // namespace aver::voxi
