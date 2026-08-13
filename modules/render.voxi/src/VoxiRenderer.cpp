// Aver Engine — Copyright (c) 2026 Hydrogen-Isotope.
// Developed by Vectoric-Core-Systems. All rights reserved.
// Proprietary. See LICENSE.md at the repository root.
#include "aver/voxi/VoxiRenderer.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"   // light-frustum fit: Vec3 / Mat4::lookAtLH
#include "aver/pbr/MaterialSystem.hpp"
#include "aver/pbr/PbrShaders.hpp"
#include "aver/voxi/VoxiGiShaders.hpp"   // kGiSrvCount/kGiUavCount: the "typed twice" fix below

#include "VoxiShaders.hpp"

#include <algorithm>
#include <cfloat>
#include <chrono>
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

// The GI-only shadow map: ONE box fitted to the GI volume, never to the camera.
//
// IT EXISTS SO THE CASCADES DO NOT HAVE TO SERVE TWO MASTERS. Light injection (PSVoxel) samples a
// shadow map for every voxel it writes, and the voxel volume is not tied to the camera -- so
// fitCascades() used to UNION the last cascade with the GI volume to cover it. Measured, that made
// cascade 3's radius 36,744cm against a camera-fitted 9,923cm: 3.7x wider, ~14x the area, and the
// per-cascade cull then admitted EVERY draw into it (census [3, 4, 5, 27]) on every single frame.
// It also cost the visible shadows, since a cascade stretched over the whole GI volume spends its
// texels there instead of on what the camera can see.
//
// 1024 because it feeds a voxel grid that is 128 across at the Medium default (512 at Epic) -- 8
// texels per voxel edge, 2 at Epic. It is 1/16 the texels of the cascade atlas, and it renders only
// on the frames voxelizePass itself runs (giUpdateInterval, 4 by default) rather than every frame.
constexpr u32 kGiShadowSize = 1024;

// Cascade split blend: 0 is uniform slabs, 1 is logarithmic (equal ratios).
constexpr f32 kCascadeSplitLambda = 0.85f;

// How far the cascades reach, as a multiple of the camera's near plane.
constexpr f32 kShadowRangeFromNear = 4000.0f;

// How small a caster has to get, measured in this cascade's own shadow-map texels, before it stops
// being drawn into it. ONE texel is the honest floor rather than a tuned number: below it the map
// has no sample that can hold the object, so the draw cannot change the image it is drawn into.
constexpr f32 kMinShadowTexels = 1.0f;

// The draw-list cap, and therefore the instance count the TLAS is sized for.
//
// 4096 until now, which the Electric Dreams demo passes before it has finished streaming: at ~6,370
// resident entities every draw past the 4096th was dropped by submit() and returned silently, so
// those entities cast no cascade shadow, no GI shadow, and wrote nothing into the voxel grid. The
// symptom is not a missing object -- they still render, because the lit pass does not go through
// here -- but objects that light as though they were not there, which reads as a shading bug rather
// than a cap. 16384 covers the demo with room to stream; the cost is the TLAS instance array
// (64 bytes each, so 1 MB) and it is only paid when ray tracing is on.
constexpr u32 kMaxDraws = 16384;

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

// Returns the pipeline layout every Voxi raster pipeline declares.
rhi::PipelineLayout giLayout() {
    rhi::PipelineLayout l{};
    // t0 volume, t1 shadow map, t2 acceleration structure, then the flat geometry a reflection
    // ray reads after a hit: t3 vertices, t4 indices, t5 instances, t6 ray-traced shadow history,
    // t7 ray-traced reflection history (both last frame's, reprojected), t8 the GI-only shadow map.
    // The material table is BASED on this count rather than at a fixed register, so widening table 0
    // rebases it automatically.
    //
    // THIS NUMBER USED TO BE TYPED TWICE, as a bare "9" here and again in createVoxelVolume's
    // BindingSetDesc::srvCount, with nothing at compile time tying the two together -- a mismatch
    // would have been a descriptor-table error at draw time or an undefined read at t8, not a build
    // failure (both were raised 8 -> 9 together, by hand). kGiSrvCount/kGiUavCount (VoxiGiShaders.hpp)
    // exist so this is typed ONCE: that header is also what a caller merging Voxi's table 0 into its
    // own (the GPU per-cluster path; see its own comment) reserves against, so Voxi's real shape and
    // what a merged caller thinks Voxi's shape is cannot independently drift either.
    l.srvCount = kGiSrvCount;
    l.uavCount = kGiUavCount;   // u0 volume mip 0, u1 injection accumulator, u2 shadow history, u3 reflection history (this frame's)
    // Table 1: the material's textures. Based at t9, NOT t3 as this said until it was checked --
    // the root-signature builder accumulates srvBase across tables, so table 1 starts at whatever
    // srvCount above is. Anyone deriving a register number from the old comment got a wrong answer.
    l.srvCount1 = pbr::kMaterialSrvCount;
    l.constantDwords[rhi::kObjectConstantRegister] = rhi::kObjectConstantDwords;
    giSamplers(l);
    return l;
}

// Returns the prelude Voxi's HLSL compiles on top of: the RHI's shared declarations, then the
// material system's BRDF and Aver* contract. Owned by a static, because the caller borrows it.
const char* voxiShaderPrelude() {
    static const std::string s = std::string(rhi::sharedShaderPrelude()) + pbr::materialShaderPrelude();
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

// Compiles Voxi shaders and destroys every one of them when it leaves scope.
struct ShaderScope {
    explicit ShaderScope(rhi::IResourceFactory& r) : res(r) {}
    ~ShaderScope() { for (rhi::ShaderHandle h : owned) res.destroyShader(h); }
    ShaderScope(const ShaderScope&) = delete;
    ShaderScope& operator=(const ShaderScope&) = delete;

    // Compiles one entry point of Voxi's HLSL. Returns 0 on failure.
    rhi::ShaderHandle operator()(const char* entry, rhi::ShaderStage stage, u32 sm,
                                 const char* defines) {
        rhi::ShaderDesc sd;
        sd.source  = kVoxiHLSL;
        sd.prelude = voxiShaderPrelude();
        sd.entry   = entry;
        sd.stage   = stage;
        sd.minShaderModel = sm;
        sd.defines = defines;
        const rhi::ShaderHandle h = res.createShader(sd);
        if (h) owned.push_back(h);
        return h;
    }

    rhi::IResourceFactory& res;
    std::vector<rhi::ShaderHandle> owned;
};

} // namespace

// ---------------------------------------------------------------- lifecycle

// Creates every GPU resource Voxi needs. Returns false, and names the first missing handle, if the
// device cannot support the baseline.
bool VoxiRenderer::init(rhi::IDevice& device) {
    dev_ = &device;
    res_ = device.resources();
    if (!res_) {
        AVER_WARN("[Voxi] init declined: backend exposes no resource factory (no GPU support)");
        return false;
    }
    caps_ = device.caps();

    if (!materials_.init(device, giLayout().srvCount))
        AVER_WARN("[Voxi] the material system declined to initialise; draws fall back to an unbound table 1");

    createShadowResources();
    createVoxelVolume(settings_.voxelResolution);
    createPipelines();

    const char* missing = nullptr;
    if (!shadowTex_)             missing = "shadow texture";
    else if (!voxelTex_)         missing = "voxel volume";
    else if (!voxelAccumTex_)    missing = "injection accumulator";
    else if (!bindings_)         missing = "main binding set";
    else if (!clearBindings_)    missing = "clear binding set";
    else if (!resolveBindings_)  missing = "resolve binding set";
    else if (mipBindings_.size() + 1 != voxelMips_) missing = "mip binding sets (count)";
    else if (!shadowPso_)        missing = "shadow pipeline";
    else if (!voxelPso_)         missing = "voxelise pipeline";
    else if (!clearPso_)         missing = "volume clear pipeline";
    else if (!resolvePso_)       missing = "injection resolve pipeline";
    else if (!mipPso_)           missing = "mip filter pipeline";
    else if (!debugPso_)         missing = "voxel debug pipeline";
    else if (!scenePso_)         missing = "scene pipeline";
    for (usize m = 0; !missing && m < mipBindings_.size(); ++m)
        if (!mipBindings_[m]) missing = "mip binding set";

    AVER_INFO("[Voxi] init: shadow tex={} volume={} accum={} ({}^3, {} mips) bindings={}/{}/{}/+{} "
              "pipelines shadow={}/{} voxel={} voxelMs={} clear={} resolve={} mip={} debug={} scene={}/{}/{}/{}",
              shadowTex_, voxelTex_, voxelAccumTex_, voxelResBuilt_, voxelMips_,
              bindings_, clearBindings_, resolveBindings_, static_cast<u32>(mipBindings_.size()),
              // shadow is per-draw/instanced -- a zero in the second slot is shadowPass silently
              // falling back to one draw per instance, which is a 3% frame-time difference and
              // otherwise invisible. It cost an hour of misattributed measurement to learn that.
              shadowPso_, shadowInstancedPso_, voxelPso_, voxelMsPso_, clearPso_, resolvePso_, mipPso_, debugPso_,
              scenePso_, sceneMsPso_, sceneRtPso_, sceneMsRtPso_);

    if (missing) {
        AVER_ERROR("[Voxi] init FAILED: {} has a zero handle", missing);
        shutdown();
        return false;
    }

    giReady_    = true;
    rtSupported_ = sceneRtPso_ != 0;

    // Created up front, and sized for the draw-list cap, so its t2 descriptor is never rewritten
    // while a frame that may be reading it is in flight.
    if (rtSupported_) {
        tlas_ = res_->createTlas(kMaxDraws);
        if (tlas_) res_->setSrvTlas(bindings_, 2, tlas_);   // t2, per the shader's register(t2)
        else {
            AVER_WARN("[Voxi] TLAS could not be created; ray-traced sun shadows stay off");
            rtSupported_ = false;
        }
    }

    if (materials_.ready())
        device.setDefaultDrawBinding(materials_.fallbackBindingSet(), &materials_.fallbackConstants(),
                                     sizeof(pbr::MaterialConstants));

    AVER_INFO("[Voxi] ready: conservative raster {}, mesh-shader variants {}, ray-tracing variants {}",
              caps_.conservativeRaster ? "on" : "off",
              (voxelMsPso_ && sceneMsPso_) ? "built" : "absent",
              rtSupported_ ? "built" : "absent");
    return true;
}

// Destroys every resource and returns the feature to its uninitialised state.
void VoxiRenderer::shutdown() {
    reportFrameTime("run total");
    materials_.shutdown();
    if (!res_) { dev_ = nullptr; return; }
    for (rhi::BindingSetHandle s : mipBindings_) if (s) res_->destroyBindingSet(s);
    mipBindings_.clear();
    if (resolveBindings_) res_->destroyBindingSet(resolveBindings_);
    if (clearBindings_) res_->destroyBindingSet(clearBindings_);
    if (bindings_)      res_->destroyBindingSet(bindings_);
    resolveBindings_ = clearBindings_ = bindings_ = 0;

    const rhi::PipelineHandle psos[] = {shadowPso_, shadowInstancedPso_, giShadowPso_,
                                        giShadowInstancedPso_, voxelPso_, voxelMsPso_, mipPso_,
                                        clearPso_, resolvePso_, debugPso_, scenePso_, sceneMsPso_,
                                        sceneRtPso_, sceneMsRtPso_};
    for (rhi::PipelineHandle p : psos) if (p) res_->destroyPipeline(p);
    shadowPso_ = shadowInstancedPso_ = giShadowPso_ = giShadowInstancedPso_ = 0;
    voxelPso_ = voxelMsPso_ = mipPso_ = clearPso_ = resolvePso_ = debugPso_ = 0;
    scenePso_ = sceneMsPso_ = sceneRtPso_ = sceneMsRtPso_ = 0;

    if (voxelAccumTex_) res_->destroyTexture(voxelAccumTex_);
    if (voxelTex_)  res_->destroyTexture(voxelTex_);
    if (shadowTex_) res_->destroyTexture(shadowTex_);
    if (giShadowTex_) res_->destroyTexture(giShadowTex_);
    voxelAccumTex_ = voxelTex_ = shadowTex_ = giShadowTex_ = 0;

    // Acceleration structures are released with the factory itself: only the handles are dropped.
    blas_.clear();
    tlas_ = 0;

    voxelMips_ = voxelResBuilt_ = 0;
    giReady_ = rtSupported_ = rtActive_ = rtLogged_ = false;
    draws_.clear();
    drawsPrev_.clear();
    rebuiltThisFrame_.clear();
    lastBlasRebuilds_ = 0xFFFFFFFFu;
    frameTimeMs_.clear();
    frameTimeLastNs_ = 0;
    frameTimeSeen_ = 0;
    res_ = nullptr;
    dev_ = nullptr;
}

// ---------------------------------------------------------------- configuration

void VoxiRenderer::setSettings(const Settings& s) {
    // Captured BEFORE the assignment. The ray-traced history buffers are allocated on the OFF->on
    // edge and released on the on->OFF edge, and this is the only place either edge is visible --
    // onRenderTargetsChanged sees a resize, not a settings change, so waiting for one would leave a
    // quarter of a gigabyte allocated (or missing) until the window happened to change size.
    const bool wasWanted = rayTracingWanted();
    settings_ = s;
    // Applied here rather than only through the direct setters, so the editor's Rendering page and
    // the project manifest can drive them the same way every other setting already does; the direct
    // setters (setShadowRays, setPixelsPerRayTile) still own the actual clamping.
    setShadowRays(s.rtShadowRays);
    setPixelsPerRayTile(s.rtPixelsPerRayTile);
    setGiUpdateInterval(s.giUpdateInterval);

    // Guarded on a real size: before the first onRenderTargetsChanged there is nothing to create at,
    // and that call will apply the current setting itself when it arrives.
    if (rayTracingWanted() != wasWanted && rtHistWantW_ && rtHistWantH_)
        if (!ensureShadowHistory(rtHistWantW_, rtHistWantH_))
            AVER_ERROR("[Voxi] ray-traced history could not follow a ray-tracing setting change at {}x{}",
                       rtHistWantW_, rtHistWantH_);
}

// Places the GI volume: centre in world units, half-edge extent.
void VoxiRenderer::setVolume(const f32 center[3], f32 extent) {
    center_[0] = center[0]; center_[1] = center[1]; center_[2] = center[2];
    extent_ = extent;
}

// Sets the light axis the shadow cascades are fitted to. Colour and intensity are the device's, not
// this renderer's -- see the header.
void VoxiRenderer::setSunDirection(const f32 dirToLight[3]) {
    sunDir_[0] = dirToLight[0]; sunDir_[1] = dirToLight[1]; sunDir_[2] = dirToLight[2];
}

void VoxiRenderer::setDebugView(bool on) { debugView_ = on; }

// Sets the occlusion rays per pixel, clamped rather than refused: a caller asking for 0 wants the
// cheapest shadow, not no shadow at all, and rtShadow divides by this.
void VoxiRenderer::setShadowRays(u32 n) {
    const u32 clamped = n < 1 ? 1 : (n > kMaxShadowRays ? kMaxShadowRays : n);
    if (clamped != n)
        AVER_WARN("[Voxi] {} shadow rays per pixel clamped to {}", n, clamped);
    if (clamped == rtShadowRays_) return;
    rtShadowRays_ = clamped;
    AVER_INFO("[Voxi] sun occlusion rays per pixel: {}", rtShadowRays_);
}

// Rounds to the nearest power of two in [1, kMaxPixelsPerRayTile] -- the shader's per-pixel trace
// schedule is a bitmask against this, not a modulo, so a non-power-of-two tile is not an option to
// silently honour the way an odd ray count is.
void VoxiRenderer::setPixelsPerRayTile(u32 n) {
    const u32 lo = n < 1 ? 1 : (n > kMaxPixelsPerRayTile ? kMaxPixelsPerRayTile : n);
    u32 rounded = 1;
    while (rounded * 2 <= lo) rounded *= 2;
    // lo sits between `rounded` and `rounded*2`; round to whichever is closer, ties toward the
    // smaller (cheaper, less amortisation) one.
    if (rounded < kMaxPixelsPerRayTile && (lo - rounded) > (rounded * 2 - lo)) rounded *= 2;
    if (rounded != n)
        AVER_WARN("[Voxi] {} pixels-per-ray tile edge rounded to the nearest power of two, {}", n, rounded);
    if (rounded == rtPixelsPerRayTile_) return;
    rtPixelsPerRayTile_ = rounded;
    AVER_INFO("[Voxi] ray-traced shadow tile: {}x{} ({} pixels per trace)",
              rounded, rounded, rounded * rounded);
}

// Clamped to [1, kMaxGiUpdateInterval]; no rounding needed, unlike the tile-edge setters above -- this
// is a plain frame count, not a bitmask operand.
void VoxiRenderer::setGiUpdateInterval(u32 n) {
    const u32 clamped = n < 1 ? 1 : (n > kMaxGiUpdateInterval ? kMaxGiUpdateInterval : n);
    if (clamped != n)
        AVER_WARN("[Voxi] GI update interval {} clamped to {}", n, clamped);
    if (clamped == giUpdateInterval_) return;
    giUpdateInterval_ = clamped;
    AVER_INFO("[Voxi] GI volume rebuilds every {} frame(s)", giUpdateInterval_);
}

// Prints what the run's frames cost. The MEDIAN leads because a frame period is a heavy-tailed
// distribution -- one alt-tab or one shader compile drags a mean and leaves a median alone -- and
// the minimum is printed beside it because the gap between them is what says whether the machine
// was quiet enough for the reading to mean anything.
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
              "p90 {:.3f} ms -- WHOLE frame, CPU, {} sun ray(s)/pixel, rt {}",
              when, static_cast<u32>(n), med, sum / static_cast<f32>(n), s.front(), p90,
              rtShadowRays_, rtActive_ ? "active" : "off");
}

namespace {
// Row-vector transform of a point (w = 1), which is this engine's matrix convention throughout.
Vec3 xformPoint(const Vec3& p, const Mat4& m) {
    return Vec3{p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0],
                p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1],
                p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2]};
}
// Same, through a projective matrix, so the perspective divide happens.
Vec3 xformProjected(const Vec3& p, const Mat4& m) {
    const f32 x = p.x * m.m[0][0] + p.y * m.m[1][0] + p.z * m.m[2][0] + m.m[3][0];
    const f32 y = p.x * m.m[0][1] + p.y * m.m[1][1] + p.z * m.m[2][1] + m.m[3][1];
    const f32 z = p.x * m.m[0][2] + p.y * m.m[1][2] + p.z * m.m[2][2] + m.m[3][2];
    const f32 w = p.x * m.m[0][3] + p.y * m.m[1][3] + p.z * m.m[2][3] + m.m[3][3];
    const f32 inv = std::fabs(w) > 1e-9f ? 1.0f / w : 0.0f;
    return Vec3{x * inv, y * inv, z * inv};
}
// The world matrix's largest axis scale, i.e. how much it can stretch a local-space radius. Row-
// vector convention: rows 0-2 are the images of the local X/Y/Z basis vectors, so each row's length
// IS that axis's scale factor. Culling needs the max rather than a per-axis figure, because the
// bounding sphere itself is not axis-aligned -- taking the largest never lets the sphere shrink
// smaller than the mesh actually reaches along any axis.
f32 maxAxisScale(const Mat4& m) {
    const f32 sx = Vec3{m.m[0][0], m.m[0][1], m.m[0][2]}.size();
    const f32 sy = Vec3{m.m[1][0], m.m[1][1], m.m[1][2]}.size();
    const f32 sz = Vec3{m.m[2][0], m.m[2][1], m.m[2][2]}.size();
    return std::max(sx, std::max(sy, sz));
}
} // namespace

// ---------------------------------------------------------------- scene submission

// Starts a new draw list. Voxi runs a frame behind: the passes replay the previous one.
void VoxiRenderer::beginScene() {
    drawsPrev_.swap(draws_);
    draws_.clear();
}

// Records one draw into this frame's list, copying its material block.
void VoxiRenderer::submit(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                          f32 metallic, f32 roughness, rhi::BindingSetHandle drawBinding,
                          const void* drawConstants, u32 drawConstantBytes) {
    if (mesh == 0) return;
    if (draws_.size() >= kMaxDraws) {
        // Says so exactly once. The old silent return is what let a 4096 cap survive a scene with
        // 6,370 entities in it without anyone noticing the shadows were incomplete.
        if (!drawCapReported_) {
            drawCapReported_ = true;
            AVER_WARN("[Voxi] draw list is full at {} -- further entities cast no shadow and do not "
                      "voxelise this frame; raise kMaxDraws", kMaxDraws);
        }
        return;
    }
    Draw d;
    d.mesh = mesh;
    // Resolved ONCE here, not per pass: six depth passes asking the same question about the same
    // handle would be six map lookups for one answer that cannot change within a frame.
    d.depthMesh = depthProxyFn_ ? depthProxyFn_(mesh, depthProxyUser_) : 0;
    if (!d.depthMesh) d.depthMesh = mesh;
    std::memcpy(d.world, world, 16 * sizeof(f32));
    std::memcpy(d.color, baseColor, 4 * sizeof(f32));
    d.metallic = metallic;
    d.roughness = roughness;
    d.matSet = drawBinding;
    d.matBytes = drawConstantBytes < sizeof(d.mat) ? drawConstantBytes : static_cast<u32>(sizeof(d.mat));
    if (drawConstants && d.matBytes) std::memcpy(d.mat, drawConstants, d.matBytes);

    // World-space bounding sphere for shadowPass's per-cascade cull. d.boundsRadius already
    // defaults to -1 (unknown) for a backend that has no bounds to give; only overwritten below.
    f32 localCentre[3] = {};
    f32 localRadius = 0.0f;
    if (dev_ && dev_->meshBounds(mesh, localCentre, &localRadius)) {
        Mat4 w;
        std::memcpy(&w.m[0][0], world, sizeof(w.m));
        const Vec3 c = xformPoint(Vec3{localCentre[0], localCentre[1], localCentre[2]}, w);
        d.boundsCentre[0] = c.x; d.boundsCentre[1] = c.y; d.boundsCentre[2] = c.z;
        d.boundsRadius = localRadius * maxAxisScale(w);
    }
    draws_.push_back(d);
}

// ---------------------------------------------------------------- feature hooks

// Runs Voxi's frame: acceleration structures, shadow map, then voxelise and filter the volume.
void VoxiRenderer::prePass(rhi::IRenderContext& ctx) {
    if (!giReady_) return;
    ++rtFrameIndex_;   // a pure per-frame count; see the member's own comment for why
    // Sampled at the TOP of the feature's frame, so consecutive readings are one frame apart
    // whatever the passes below do. The first sample after the warm-up is discarded with the rest.
    if (frameTimeReport_) {
        const u64 now = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if (frameTimeLastNs_ != 0 && frameTimeSeen_ >= kFrameTimeWarmup)
            frameTimeMs_.push_back(static_cast<f32>(now - frameTimeLastNs_) * 1e-6f);
        frameTimeLastNs_ = now;
        ++frameTimeSeen_;
        // Reported as the run goes as well as at shutdown, so a run that is killed or that never
        // reaches shutdown still leaves a number behind.
        if (frameTimeMs_.size() && frameTimeMs_.size() % 120 == 0) reportFrameTime("running");
    }
    materials_.update();
    const f32 size = extent_ * 2.0f;
    cb_.voxelOrigin[0] = center_[0] - extent_;
    cb_.voxelOrigin[1] = center_[1] - extent_;
    cb_.voxelOrigin[2] = center_[2] - extent_;
    cb_.voxelOrigin[3] = size > 0.0f ? 1.0f / size : 0.0f;   // the shader multiplies by this
    cb_.voxelParams[0] = static_cast<f32>(voxelResBuilt_);
    cb_.voxelParams[1] = settings_.giIntensity;
    cb_.voxelParams[2] = settings_.giMaxDistance;
    // gates the cone trace -- see setConeTraceEnabled's own comment for why that toggle is ANDed in
    // here rather than folded into giEnabled(): this is the one place a caller doing the A/B
    // measurement that comment describes can turn the shader-side read off without changing whether
    // any of the passes below actually run.
    cb_.voxelParams[3] = (giEnabled() && !debugView_ && coneTraceEnabled_) ? 1.0f : 0.0f;

    // THE NESTING SHOWCASE. Everything below -- acceleration structures, the shadow cascades, the
    // GI-only shadow box, voxelise, mip filter -- used to open and close its OWN top-level marker, so
    // the GPU timing report saw five siblings with no notion that they are all really one feature's
    // one frame of work. Wrapping them in this outer scope makes every one of pushMarker's calls below
    // (inside buildAccelerationStructures/shadowPass/giShadowPass/voxelizePass/filterMips) a CHILD of
    // "Voxi GI update" instead of another top-level entry beside "scene draw" -- which is also what
    // makes this scope's own EXCLUSIVE time mean something for the first time: whatever GPU time is
    // left after subtracting all five children's inclusive time from this one's own is
    // beginShadowHistory/endShadowHistory below, the only GPU work this function issues that is not
    // already inside one of those five children's own markers. Opened HERE rather than at the top of
    // the function on purpose: everything above this line is pure CPU bookkeeping (frame-time
    // sampling, materials_.update(), writing cb_) that records no GPU command at all, so starting the
    // scope before buildAccelerationStructures means its inclusive time is real GPU work from the
    // first timestamp to the last, not CPU accounting that happens to run before the marker closes.
    rhi::ScopedGpuStat voxiGpuStat(ctx, "Voxi GI update");
    buildAccelerationStructures(ctx);   // sets rtActive_, which beginShadowHistory reads
    beginShadowHistory(ctx);
    shadowPass(ctx);                    // fitCascades(), called from here, fills curViewProj_
    if (giEnabled()) {
        // Amortised revoxelisation: rtFrameIndex_ was already incremented above, so it reads 1 on
        // this feature's very first prePass -- (rtFrameIndex_-1) % N therefore always lands on 0 for
        // frame 1, guaranteeing the volume is built at least once before anything ever samples it,
        // however large giUpdateInterval_ is. Every Nth frame after that rebuilds again; the frames in
        // between skip straight past voxelizePass/filterMips and the cone trace (gated by
        // voxelParams[3] above, unaffected by this) samples whatever the volume held last time it was
        // rebuilt. giUpdateInterval_ == 1 (the default) takes the fast path every frame, identical to
        // the code before this knob existed.
        if (giUpdateInterval_ <= 1 || ((rtFrameIndex_ - 1) % giUpdateInterval_) == 0) {
            // THE REBUILD GATE. Everything below recomputes a function of (draw list, sun, volume
            // placement); when none of those moved, the volume texture already holds the answer and
            // is still sitting there fully resolved and mip-filtered. See giSnapshotUnchanged.
            if (giSnapshotUnchanged()) {
                ++giSkipped_;
            } else {
                ++giRebuilt_;
                takeGiSnapshot();
                // BEFORE voxelizePass, and inside this gate on purpose. PSVoxel samples the GI-only
                // map through giShadowFactor, so it has to exist before injection reads it -- and it
                // is pointless to rebuild on the frames injection is skipped, which is 3 in 4 at the
                // default interval. That cadence is half the saving; the other half is that the
                // camera cascades no longer carry the volume at all (see fitCascades).
                giShadowPass(ctx);
                voxelizePass(ctx);
                filterMips(ctx);
            }
            // Said ONCE, and as a ratio rather than a feeling: "GI rebuilt 3 of 170 ticks" is the
            // difference between this gate paying for itself and it being a hash walk that never
            // hits. A run that reports 100% rebuilt is a run where the saving is zero, and that is
            // worth seeing rather than assuming.
            // REPORTED SEVERAL TIMES, NOT ONCE, and that is a correction rather than a preference.
            // The first version logged once at 64 ticks and stopped. At the default interval of 4
            // that is frame ~256 -- which on any streamed level is still mid-fill, when the draw list
            // changes every tick and the gate CANNOT match by construction. So it could only ever
            // print 0%, whatever the gate actually did later, and it did: "0 skipped of 64" on every
            // run. A measurement whose window excludes the case it is measuring is worse than none,
            // because it reads as a result.
            //
            // Now it reports at widening intervals and prints the SINCE-LAST-REPORT ratio alongside
            // the lifetime one, so the steady state is visible instead of being averaged away by the
            // loading phase it can never help with.
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
                giGateNextReport_ = ticks * 2;   // 64, 128, 256, ... -- a handful of lines, not a flood
            }
        }
    }
    endShadowHistory();
}

// Builds a bottom-level structure for every referenced mesh, then one top-level structure over the
// replayed draw list. Publishes shadowParams.z so the lit pass knows whether it may trace.
void VoxiRenderer::buildAccelerationStructures(rhi::IRenderContext& ctx) {
    rtActive_ = false;
    cb_.shadowParams[2] = 0.0f;
    if (!rtSupported_ || settings_.rayTracing == Quality::Off || drawsPrev_.empty()) return;

    rhi::ScopedGpuStat gpuStat(ctx, "Voxi acceleration structures");
    std::vector<rhi::TlasInstance> inst;
    inst.reserve(drawsPrev_.size());
    rtInstanceData_.clear();
    rtInstanceMesh_.clear();
    rebuiltThisFrame_.clear();
    u32 firstBuilds = 0;
    for (const Draw& d : drawsPrev_) {
        auto it = blas_.find(d.mesh);
        // A CACHED STRUCTURE WHOSE MESH HAS BEEN DESTROYED UNDERNEATH US. This is reachable, not
        // theoretical: the instance list is built from drawsPrev_ -- LAST frame's draws -- so a mesh
        // freed between frames is still named here, and handing its structure to the TLAS would have
        // the GPU traverse memory that has gone back to the heap. Asking the factory what the BLAS
        // is actually for catches it without every caller of destroyMesh having to remember to tell
        // this cache, which is the "one missed site" shape this renderer has been bitten by before.
        if (it != blas_.end() && it->second && res_->blasMesh(it->second) != d.mesh) {
            blas_.erase(it);
            it = blas_.end();
        }
        if (it == blas_.end()) {
            // Returns 0 for a destroyed mesh, which is then recorded and not retried.
            const rhi::BlasHandle nb = res_->createBlas(d.mesh);
            if (nb) { ctx.buildBlas(nb); ++firstBuilds; }
            // A zero is recorded too, so a mesh that cannot produce a BLAS is not retried each frame.
            it = blas_.emplace(d.mesh, nb).first;
        } else if (it->second && dev_->meshVertexBuffer(d.mesh)) {
            // A mesh whose vertices are written by compute invalidates its own structure every
            // frame. Memoising it -- which is exactly right for static geometry -- gives a skinned
            // character a ray-traced shadow with the silhouette it had when the structure was first
            // built: the character moves and the shadow does not, and nothing in the raster image
            // shows it. The skinning dispatch has already left the buffer in GeometryRead by now,
            // because a skinning feature is registered BEFORE this one.
            //
            // ONCE PER MESH PER FRAME, not once per DRAW. The draw list holds one entry per
            // instance, so a mesh drawn twice used to be rebuilt twice: the second build recomputes
            // the identical structure over the identical vertices and overwrites the first with it,
            // for a full PREFER_FAST_TRACE build and a UAV barrier of pure waste. The linear scan is
            // over the DISTINCT dynamic meshes in one frame, which is a handful.
            if (std::find(rebuiltThisFrame_.begin(), rebuiltThisFrame_.end(), d.mesh) ==
                rebuiltThisFrame_.end()) {
                ctx.buildBlas(it->second);
                rebuiltThisFrame_.push_back(d.mesh);
            }
            if (!dynamicBlasLogged_) {
                AVER_INFO("[Voxi] mesh {} has compute-written vertices; its bottom-level structure "
                          "is rebuilt every frame rather than cached", d.mesh);
                dynamicBlasLogged_ = true;
            }
        }
        const rhi::BlasHandle b = it->second;
        if (!b) continue;
        rhi::TlasInstance i;
        // Engine convention, handed over untouched: the backend owns the transpose DXR wants.
        std::memcpy(i.world, d.world, sizeof(i.world));
        i.mask = 0xFF;
        i.blas = b;
        // An index into rtInstanceData_, which a reflection ray reads to find the triangle it hit
        // and the surface's albedo. Assigned HERE, in the same loop that decides which instances
        // survive, so the two lists cannot drift -- an id assigned earlier would be wrong for every
        // instance after one whose acceleration structure failed to build.
        i.instanceId = static_cast<u32>(rtInstanceData_.size()) & rhi::kMaxTlasInstanceId;
        inst.push_back(i);

        RtInstance ri;
        std::memcpy(ri.objectToWorld, d.world, sizeof(ri.objectToWorld));
        ri.albedo[0] = d.color[0]; ri.albedo[1] = d.color[1]; ri.albedo[2] = d.color[2];
        // Filled in by buildGeometryTable, which is what knows where each mesh landed.
        ri.firstIndex = 0;
        ri.firstVertex = 0;
        rtInstanceData_.push_back(ri);
        rtInstanceMesh_.push_back(d.mesh);
    }
    // gpuStat's destructor closes the marker here -- this used to be `ctx.popMarker(); return;`, one
    // of two exits from this function that both had to remember to pop by hand. See ScopedGpuStat's
    // own comment for why that duplication was the actual bug this class exists to make impossible.
    if (inst.empty()) return;

    ctx.buildTlas(tlas_, inst.data(), static_cast<u32>(inst.size()));
    rtActive_ = true;
    cb_.shadowParams[2] = 1.0f;

    // How fast a ray-traced shadow edge softens is the SUN's angular size, not a tuned constant --
    // the disc subtends about half a degree, and rtShadow spreads its rays across exactly that.
    // Taken from the sky model rather than duplicated, so a scene that moves the sun or widens the
    // disc gets penumbrae that agree with its own sky.
    const f32 halfAngle = dev_->skyAtmosphere().sunAngularDiameterDeg * 0.5f * 0.01745329252f;
    cb_.rtParams[0] = std::tan(halfAngle);
    cb_.rtParams[1] = static_cast<f32>(rtShadowRays_);
    // Base ray bias in centimetres, scaled by view distance in the shader. Small enough not to
    // detach a contact shadow, large enough that a surface does not intersect its own rays.
    cb_.rtParams[2] = 0.05f;
    // w > 0.5 tells the lit pass it may trace a reflection ray. It is only true when the flat
    // geometry table is actually there, because a reflection that hits geometry it cannot look up
    // would read a neighbour's triangle rather than fail visibly.
    cb_.rtParams[3] = buildGeometryTable(ctx) ? 1.0f : 0.0f;
    if (!rtLogged_) {
        AVER_INFO("[Voxi] RayQuery active ({} instances, {} bottom-level structures)",
                  static_cast<u32>(inst.size()), static_cast<u32>(blas_.size()));
        rtLogged_ = true;
    }
    // WHAT THE ACCELERATION STRUCTURES COST THIS FRAME, as a count rather than an impression. Every
    // one of these is a full PREFER_FAST_TRACE build -- the most expensive mode there is -- because
    // the RHI has no refit verb, so this number IS the bill.
    //
    // It is also the only way to see a predicate go wrong. The rebuild branch is taken when
    // IDevice::meshVertexBuffer is non-zero, which its contract says means "vertices written by
    // compute, zero for every ordinary mesh"; if that ever stops being true, a static scene starts
    // rebuilding everything every frame and looks exactly the same on screen. Printed only when the
    // count changes, so a steady frame is silent.
    //
    // Keyed on BOTH halves rather than on their sum, because the interesting frame is the second
    // one: two first-time builds becoming two rebuilds is the same total and a completely different
    // statement about the cache.
    const u32 rebuilds = (static_cast<u32>(rebuiltThisFrame_.size()) << 16) | (firstBuilds & 0xFFFFu);
    if (rebuilds != lastBlasRebuilds_) {
        AVER_INFO("[Voxi] bottom-level builds this frame: {} ({} first-time, {} rebuilt) over {} "
                  "draws of {} distinct meshes",
                  firstBuilds + static_cast<u32>(rebuiltThisFrame_.size()), firstBuilds,
                  static_cast<u32>(rebuiltThisFrame_.size()), static_cast<u32>(drawsPrev_.size()),
                  static_cast<u32>(blas_.size()));
        lastBlasRebuilds_ = rebuilds;
    }
}

// Builds one orthographic light frustum per cascade, fitted to a slice of the camera's view, and
// writes the matrices and splits into cb_. Returns the usable cascade count, 0 if there is no camera.
// An order-sensitive hash of what voxelizePass would rasterise: every draw's mesh and its full
// world transform. Deliberately the same FNV-style mix buildGeometryTable uses over rtInstanceMesh_,
// and deliberately order-sensitive -- a reordered draw list produces a different injection order
// into the atomic accumulator, so it is NOT the same result and must not be treated as one.
//
// The transform is hashed as its raw float BITS, not compared with a tolerance. A cache that
// tolerates "almost the same" transform is a cache that shows the wrong lighting for a while and
// then stops; either it is the identical input or it rebuilds.
u64 VoxiRenderer::giDrawsKey() const {
    u64 key = 1469598103934665603ull;
    for (const Draw& d : drawsPrev_) {
        key ^= static_cast<u64>(d.mesh);
        key *= 1099511628211ull;
        for (u32 i = 0; i < 16; ++i) {
            u32 bits = 0;
            std::memcpy(&bits, &d.world[i], sizeof(bits));
            key ^= static_cast<u64>(bits);
            key *= 1099511628211ull;
        }
        // Colour and metallic/roughness reach PSVoxel through the object constants and land in the
        // baked radiance, so a material tweak with no movement must still rebuild.
        for (u32 i = 0; i < 4; ++i) {
            u32 bits = 0;
            std::memcpy(&bits, &d.color[i], sizeof(bits));
            key ^= static_cast<u64>(bits);
            key *= 1099511628211ull;
        }
        u32 mb = 0, rb = 0;
        std::memcpy(&mb, &d.metallic, sizeof(mb));
        std::memcpy(&rb, &d.roughness, sizeof(rb));
        key ^= (static_cast<u64>(mb) << 32) ^ static_cast<u64>(rb);
        key *= 1099511628211ull;
    }
    return key;
}

// True when every input to voxelizePass is identical to the last rebuild's.
bool VoxiRenderer::giSnapshotUnchanged() const {
    // WHICH CHECK REJECTED, said once. Two plausible causes for "never fires" were fixed on
    // reasoning alone and neither was it, which is the point at which guessing stops being
    // cheaper than measuring: the gate compares four independent things and the log said only
    // that the answer was no, never which of them said it.
    // ONE LINE PER DISTINCT REASON, not one line total. The first version latched on the first
    // rejection of any kind, which is trivially the "no snapshot yet" that fires on frame one -- so
    // it reported the one uninteresting cause and hid every real one behind it.
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
    // The whole sky struct, byte for byte. It is where sunDirection, sunColor, sunIntensity, the
    // ground albedo and the sky-light intensity all live, and PSVoxel reads every one of them
    // (directly, or through averSunRadiance/averSkyIrradiance). Comparing the bytes rather than a
    // chosen subset of fields is what keeps this correct when a field is ADDED to SkyAtmosphere --
    // a hand-picked field list would silently stop covering the new one.
    if (!dev_) return false;
    // ZERO-INITIALISED, THEN ASSIGNED -- and that two-step is the whole reason this works.
    // skyAtmosphere() returns BY VALUE, and SkyAtmosphere opens with a bool followed by padding.
    // memcmp on a raw returned copy compares that padding, which is unspecified, so the comparison
    // failed every single time and this gate never once fired: 0 skipped of 512 ticks, on a scene
    // that had been standing still for two thousand frames. Copy-assigning into a value-initialised
    // object leaves the padding at the zero both sides started from, so only the MEMBERS are
    // compared -- while keeping the property the byte comparison was chosen for, that a field added
    // to SkyAtmosphere later cannot silently fall outside the check.
    rhi::SkyAtmosphere now{};
    now = dev_->skyAtmosphere();
    // CLOUDTIME IS A CLOCK, AND IT IS WHY THIS GATE NEVER ONCE PASSED. It counts accumulated
    // seconds, so it differs on every tick by construction; comparing it byte-for-byte meant a
    // static scene under a still camera rebuilt the entire 128^3 volume every giUpdateInterval
    // frames forever, on the grounds that the sky had "changed". Measured: byte 240 of 248, 3.01e-05
    // on the snapshot against 19.19752 live.
    //
    // Normalised out of BOTH SIDES rather than compared field-by-field, so the property the byte
    // comparison was chosen for survives -- a field added to SkyAtmosphere later still cannot
    // silently fall outside the check. Only this one named field is excused, and it is excused
    // here where the reason is written down.
    rhi::SkyAtmosphere was{};
    was = giSky_;
    const f32 cloudTimeDelta = std::fabs(now.cloudTime - was.cloudTime);
    now.cloudTime = was.cloudTime = 0.0f;

    if (std::memcmp(&now, &was, sizeof(now)) != 0) {
        // WHICH BYTE, not just "something". This rejection was diagnosed twice from a plain
        // "sky/sun changed" and guessed wrong both times; the offset costs nothing to report and
        // turns a guess into an answer -- map it against the field order in rhi::SkyAtmosphere.
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

    // Excusing the clock is not the same as ignoring the clouds. A drifting layer really does change
    // how much sky reaches the ground, so the bake is allowed to go stale by a bounded amount rather
    // than indefinitely: past this, the sky counts as changed after all. Only when there are clouds
    // to drift -- a clear sky holds its bake for as long as nothing else moves.
    constexpr f32 kGiCloudStaleSeconds = 2.0f;
    if (now.cloudsEnabled && cloudTimeDelta > kGiCloudStaleSeconds) return reject(5, "clouds drifted");
    if (giDrawsKey() != giDrawsKey_) return reject(4, "draw list changed");
    return true;
}

// Records what the rebuild about to run was computed from.
void VoxiRenderer::takeGiSnapshot() {
    giDrawsKey_ = giDrawsKey();
    // Same two-step on the stored side, so both sides of the memcmp have zero padding.
    if (dev_) { giSky_ = rhi::SkyAtmosphere{}; giSky_ = dev_->skyAtmosphere(); }
    for (u32 i = 0; i < 3; ++i) giSnapCenter_[i] = center_[i];
    giSnapExtent_ = extent_;
    giSnapValid_ = true;
}

u32 VoxiRenderer::fitCascades() {
    f32 invViewProj[16] = {};
    f32 camPos[3] = {};
    // The forward matrix is captured too, into curViewProj_ -- not used here, but this is where the
    // camera is already being read, and endShadowHistory needs THIS frame's viewProj to become next
    // frame's reprojection source.
    if (!dev_ || !dev_->camera(curViewProj_, invViewProj, camPos)) return 0;

    Mat4 invVP;
    std::memcpy(&invVP.m[0][0], invViewProj, sizeof(invViewProj));
    const Vec3 eye{camPos[0], camPos[1], camPos[2]};

    // The frustum's eight world-space corners, from clip space. D3D depth is [0,1].
    Vec3 nearC[4], farC[4];
    const f32 nx[4] = {-1, 1, 1, -1};
    const f32 ny[4] = {-1, -1, 1, 1};
    for (int i = 0; i < 4; ++i) {
        nearC[i] = xformProjected(Vec3{nx[i], ny[i], 0.0f}, invVP);
        farC[i]  = xformProjected(Vec3{nx[i], ny[i], 1.0f}, invVP);
    }

    // View depth at each plane, measured along the view axis rather than radially.
    Vec3 fwd = (farC[0] + farC[1] + farC[2] + farC[3]) * 0.25f - eye;
    fwd = fwd.getSafeNormal();
    const f32 camNear = dot(nearC[0] - eye, fwd);
    const f32 camFar  = dot(farC[0] - eye, fwd);
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

        // The corners of this slice, interpolated along the frustum's own edges.
        const f32 tN = (sliceNear - camNear) / (camFar - camNear);
        const f32 tF = (sliceFar  - camNear) / (camFar - camNear);
        Vec3 corner[8];
        for (int i = 0; i < 4; ++i) {
            corner[i]     = nearC[i] + (farC[i] - nearC[i]) * tN;
            corner[i + 4] = nearC[i] + (farC[i] - nearC[i]) * tF;
        }

        Vec3 centre{0, 0, 0};
        for (const Vec3& v : corner) centre = centre + v;
        centre = centre * 0.125f;
        f32 radius = 0.0f;
        for (const Vec3& v : corner) {
            const f32 d = (v - centre).size();
            if (d > radius) radius = d;
        }
        radius = std::ceil(radius * 16.0f) / 16.0f;

        // NO UNION WITH THE GI VOLUME HERE ANY MORE. It used to widen this last cascade to cover the
        // whole volume, because light injection sampled these same cascades. Measured on the
        // ElectricDreams level that made cascade 3's radius 36,744cm against a camera-fitted
        // 9,923cm -- 3.7x wider, ~14x the area -- so the per-cascade cull admitted every draw in
        // the scene into it, every frame, and the cascade spent its texels on volume the camera
        // cannot see. giShadowPass/fitGiShadow now answer the volume separately, and every cascade
        // here is fitted to the camera and nothing else.

        // Persisted for shadowPass's per-draw cull: the same (centre, radius) this cascade's own
        // frustum and cb_.cascadeSplit are built from, before the sub-texel nudge below.
        cascadeCentre_[c][0] = centre.x; cascadeCentre_[c][1] = centre.y; cascadeCentre_[c][2] = centre.z;
        cascadeRadius_[c] = radius;

        // Snap the centre to a whole texel of this cascade, in light space.
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

        // Radial, because the cascades are fitted to bounding spheres.
        cb_.cascadeSplit[c][0] = (centre - eye).size() + radius;
        cb_.cascadeSplit[c][1] = texel * 1.5f;   // normal-offset bias, world units
        cb_.cascadeSplit[c][2] = 0.0f;
        cb_.cascadeSplit[c][3] = 0.0f;

        sliceNear = sliceFar;
    }
    return kShadowCascades;
}

// Fits the GI-only shadow map to the GI VOLUME, and writes its matrix and cull sphere.
//
// This is the half of the old cascade union that was actually needed, extracted so it stops
// distorting the camera cascades. Where fitCascades slices the view frustum and depends on where
// the camera looks, this depends on nothing but setVolume(): the same box every frame the volume
// does not move, which is also why it can be rebuilt on giUpdateInterval's cadence instead of
// every frame.
void VoxiRenderer::fitGiShadow() {
    // The volume's circumsphere -- the same expression the deleted union used, so the box covers
    // exactly what it always covered.
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

    // Snapped to a whole texel in light space, exactly as the cascades are. The volume does not
    // move with the camera, so this matters less here than it does there -- but the sun DOES move,
    // and without the snap a slow sun turn makes every injected voxel's occlusion crawl.
    const f32 texel = 2.0f * radius / static_cast<f32>(kGiShadowSize);
    Mat4 view = Mat4::lookAtLH(centre + dir * (radius * 2.0f), centre, up);
    Vec3 cLs = xformPoint(centre, view);
    cLs.x = std::floor(cLs.x / texel) * texel;
    cLs.y = std::floor(cLs.y / texel) * texel;
    const Vec3 snapped = xformPoint(cLs, view.inverse());
    view = Mat4::lookAtLH(snapped + dir * (radius * 2.0f), snapped, up);

    Mat4 proj;   // orthographic, row-vector, depth [0,1] -- same form fitCascades builds
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
    cb_.giShadowParams[3] = 0.0f;
}

// Concatenates every referenced mesh's vertices and indices into two flat buffers, and writes the
// per-instance table that says where each mesh's data starts.
//
// REBUILT ONLY WHEN THE MESH SET CHANGES. Copying every mesh every frame would cost more than the
// reflections it enables, and the geometry itself does not move -- an instance's TRANSFORM changes
// per frame and lives in the instance record, which is rewritten every frame because it is small.
bool VoxiRenderer::buildGeometryTable(rhi::IRenderContext& ctx) {
    if (!res_ || !dev_ || rtInstanceData_.empty()) return false;

    // What the table is built from: the ordered list of meshes. A cheap order-sensitive mix, so a
    // reordered draw list rebuilds rather than silently keeping offsets that no longer match.
    u64 key = 1469598103934665603ull;
    for (rhi::MeshHandle h : rtInstanceMesh_) {
        key ^= static_cast<u64>(h);
        key *= 1099511628211ull;
    }

    u32 totalVerts = 0, totalIndices = 0;
    for (usize i = 0; i < rtInstanceMesh_.size(); ++i) {
        rhi::BufferHandle vb = 0, ib = 0;
        u32 vc = 0, ic = 0;
        if (!dev_->meshGeometry(rtInstanceMesh_[i], &vb, &ib, &vc, &ic)) return false;
        rtInstanceData_[i].firstVertex = totalVerts;
        rtInstanceData_[i].firstIndex  = totalIndices;
        totalVerts   += vc;
        totalIndices += ic;
    }
    if (totalVerts == 0 || totalIndices == 0) return false;

    // The instance table is rewritten every frame -- transforms move -- so it is an upload buffer.
    // Grown as a set: every slot in the ring has to hold the largest table, or rotating onto a
    // smaller one next frame would truncate it.
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
    // Rotate BEFORE writing, so this frame never touches the buffer the previous one bound.
    rtInstanceSlot_ = (rtInstanceSlot_ + 1) % kRtInstanceRing;
    const rhi::BufferHandle inst = rtInstances_[rtInstanceSlot_];
    if (!inst) return false;
    res_->writeBuffer(inst, rtInstanceData_.data(),
                      sizeof(RtInstance) * rtInstanceData_.size(), 0);
    res_->setSrvBuffer(bindings_, 5, inst, sizeof(RtInstance),
                       static_cast<u32>(rtInstanceData_.size()), 0);

    if (key == rtGeometryKey_ && rtGeometryReady_) return true;

    // The geometry itself. Default-heap, because it is written once by copy and then read by every
    // reflection ray for as long as the mesh set holds.
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

    // BOTH transitions are explicit. D3D12 would promote a Common buffer to CopyDest by itself, but
    // the RHI tracks buffer state to catch exactly this class of mistake, and it does not model
    // promotion -- so an implicit promotion followed by an explicit walk-back is a barrier claiming
    // a state the tracker never saw it enter. Being explicit at both ends keeps the two in step.
    //
    // The walk-back matters on its own account too: the promotion lasts the rest of the COMMAND
    // LIST, so a reflection ray reading this later in the same frame would be reading a resource
    // the runtime still considers a copy destination.
    ctx.bufferBarrier(rtVerts_,   rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
    ctx.bufferBarrier(rtIndices_, rhi::ResourceState::Common, rhi::ResourceState::CopyDest);
    for (usize i = 0; i < rtInstanceMesh_.size(); ++i) {
        rhi::BufferHandle vb = 0, ib = 0;
        u32 vc = 0, ic = 0;
        if (!dev_->meshGeometry(rtInstanceMesh_[i], &vb, &ib, &vc, &ic)) return false;
        ctx.copyBuffer(rtVerts_, vb, static_cast<u64>(vc) * sizeof(rhi::MeshVertex),
                       static_cast<u64>(rtInstanceData_[i].firstVertex) * sizeof(rhi::MeshVertex), 0);
        ctx.copyBuffer(rtIndices_, ib, static_cast<u64>(ic) * sizeof(u32),
                       static_cast<u64>(rtInstanceData_[i].firstIndex) * sizeof(u32), 0);
    }
    ctx.bufferBarrier(rtVerts_,   rhi::ResourceState::CopyDest, rhi::ResourceState::Common);
    ctx.bufferBarrier(rtIndices_, rhi::ResourceState::CopyDest, rhi::ResourceState::Common);

    rtGeometryKey_ = key;
    rtGeometryReady_ = true;
    res_->setSrvBuffer(bindings_, 3, rtVerts_, sizeof(rhi::MeshVertex), totalVerts, 0);
    res_->setSrvBuffer(bindings_, 4, rtIndices_, sizeof(u32), totalIndices, 0);
    AVER_INFO("[Voxi] ray-traced reflection table: {} instances, {} vertices, {} indices",
              rtInstanceData_.size(), totalVerts, totalIndices);
    return true;
}

// Renders the replayed draw list into each cascade's quadrant of the shadow atlas, depth only.
//
// INSTANCED BY DEFAULT when shadowInstancedPso_ built (see createPipelines): every surviving draw in
// a cascade is grouped by mesh into shadowInstanceGroups_, and each group reaches the GPU as ONE
// IRenderContext::drawMeshInstanced call instead of one drawMesh() per draw. Depth-only rendering
// with a strict Less test is order-independent except for two triangles landing on EXACTLY the same
// depth at the same pixel -- regrouping by mesh cannot change a single output depth value in any
// other case, so the shadow map this produces is the same map the old per-draw loop produced. What
// it gives up: nothing quality-wise; what changes is WORK -- up to ~2,114 drawMesh calls per cascade
// collapse to at most ~30 (this scene's distinct mesh count) drawMeshInstanced calls. Falls back to
// the untouched one-draw-per-instance path (shadowPso_) if the instanced pipeline failed to build.
void VoxiRenderer::shadowPass(rhi::IRenderContext& ctx) {
    const bool useInstancing = shadowInstancedPso_ != 0;
    if ((!shadowPso_ && !shadowInstancedPso_) || drawsPrev_.empty()) { cb_.shadowParams[1] = 0.0f; return; }
    u32 cascades = fitCascades();
#ifdef AVER_VOXI_SHADOW_CASCADE_LIMIT
    // TEMPORARY, for isolating this pass's own cost -- see the cache variable of the same name in
    // this module's CMakeLists.txt. The default build sets it to kShadowCascades (4), so `cascades`
    // (already <= 4, fitCascades' own ceiling) is never actually reduced and this is a no-op unless
    // someone deliberately reconfigures with -DAVER_VOXI_SHADOW_CASCADE_LIMIT. fitCascades() still
    // runs UNCLAMPED above: it is cheap CPU work, and it is also where curViewProj_ gets captured,
    // which endShadowHistory needs every frame regardless of how many cascades get drawn.
    if (cascades > static_cast<u32>(AVER_VOXI_SHADOW_CASCADE_LIMIT))
        cascades = static_cast<u32>(AVER_VOXI_SHADOW_CASCADE_LIMIT);
#endif
    if (cascades == 0) { cb_.shadowParams[1] = 0.0f; return; }

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

    // TEMPORARY measurement instrumentation: a one-time census of what this pass actually submits,
    // per cascade. Logged ONCE, not every frame -- this asks "what does a typical frame cost", not
    // "watch it every frame". Adds one INFO line to the log and nothing to the rendered image.
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

        // WHAT THIS CASCADE CAN ACTUALLY RESOLVE. Each one spends its 2048 texels over its whole
        // fitted radius, so a far cascade's texel is metres wide while a near one's is centimetres.
        // An object smaller than a single texel cannot put a shadow into this map -- there is no
        // sample small enough to hold it -- so rasterising it is work with no possible outcome.
        //
        // This scene is the case it was written for: thousands of scattered ankle-height plants, all
        // of them sub-texel by cascade 2 and all of them being drawn into it anyway. It is a
        // PER-CASCADE test rather than a global one for the same reason -- the same plant is real
        // detail in cascade 0 and invisible in cascade 3.
        const f32 cascTexelWorld = 2.0f * cascRadius / static_cast<f32>(kShadowCascadeSize);
        const f32 minShadowDiameter = cascTexelWorld * kMinShadowTexels;

        u32 submitted = 0;
        if (useInstancing) {
            // Reset capacity, not the group list itself: the mesh set repeats cascade to cascade and
            // frame to frame, so keeping each group's (now empty) vector alive means only the FIRST
            // frame's cascades pay for growth -- see shadowInstanceGroups_'s own comment.
            for (ShadowInstanceGroup& g : shadowInstanceGroups_) g.worlds.clear();

            for (const Draw& d : drawsPrev_) {
                // Sphere-sphere overlap against this cascade's own fitted bounds: a draw whose
                // bounding sphere cannot reach the cascade's box cannot cast a shadow into it. A
                // negative boundsRadius means the backend had no bounds for this mesh -- draw it
                // regardless rather than guess.
                if (d.boundsRadius >= 0.0f) {
                    const Vec3 dc{d.boundsCentre[0], d.boundsCentre[1], d.boundsCentre[2]};
                    if (dist(dc, cascCentre) > cascRadius + d.boundsRadius) continue;
                    if (2.0f * d.boundsRadius < minShadowDiameter) continue;
                }
                ShadowInstanceGroup* group = nullptr;
                for (ShadowInstanceGroup& g : shadowInstanceGroups_)
                    if (g.mesh == d.depthMesh) { group = &g; break; }
                if (!group) { shadowInstanceGroups_.push_back({d.depthMesh, {}}); group = &shadowInstanceGroups_.back(); }
                const usize base = group->worlds.size();
                group->worlds.resize(base + 16);
                std::memcpy(group->worlds.data() + base, d.world, 16 * sizeof(f32));
                ++submitted;
            }
            for (const ShadowInstanceGroup& g : shadowInstanceGroups_) {
                if (g.worlds.empty()) continue;
                ctx.drawMeshInstanced(g.mesh, g.worlds.data(), static_cast<u32>(g.worlds.size() / 16));
            }
        } else {
            for (const Draw& d : drawsPrev_) {
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
//
// Called from prePass INSIDE the giUpdateInterval gate, so it runs on exactly the frames
// voxelizePass runs -- 1 in 4 at the Medium default. That is the second half of the win: the old
// arrangement rebuilt volume-wide shadow coverage every single frame, including the 3 in 4 on which
// no voxel was re-injected and nothing read it.
void VoxiRenderer::giShadowPass(rhi::IRenderContext& ctx) {
    const bool useInstancing = giShadowInstancedPso_ != 0;
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
    ctx.clearDepth(giShadowTex_, 1.0f);
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
        for (const Draw& d : drawsPrev_) {
            // Culled against the VOLUME's sphere, never cascadeCentre_[3]/cascadeRadius_[3] -- those
            // are camera-fitted now and have nothing to do with what the volume covers.
            if (d.boundsRadius >= 0.0f && dist(Vec3{d.boundsCentre[0], d.boundsCentre[1], d.boundsCentre[2]},
                                                giCentre) > giShadowRadius_ + d.boundsRadius) continue;
            ShadowInstanceGroup* group = nullptr;
            for (ShadowInstanceGroup& g : giShadowInstanceGroups_)
                if (g.mesh == d.depthMesh) { group = &g; break; }
            if (!group) { giShadowInstanceGroups_.push_back({d.depthMesh, {}}); group = &giShadowInstanceGroups_.back(); }
            const usize base = group->worlds.size();
            group->worlds.resize(base + 16);
            std::memcpy(group->worlds.data() + base, d.world, 16 * sizeof(f32));
            ++submitted;
        }
        for (const ShadowInstanceGroup& g : giShadowInstanceGroups_) {
            if (g.worlds.empty()) continue;
            ctx.drawMeshInstanced(g.mesh, g.worlds.data(), static_cast<u32>(g.worlds.size() / 16));
        }
    } else {
        for (const Draw& d : drawsPrev_) {
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

// Clears the accumulator, rasterises the scene into it with direct lighting applied, then resolves
// it into mip 0 of the radiance volume.
void VoxiRenderer::voxelizePass(rhi::IRenderContext& ctx) {
    const u32 res = voxelResBuilt_;
    rhi::ScopedGpuStat gpuStat(ctx, "Voxi voxelise");

    ctx.setPipeline(clearPso_);
    ctx.setBindingSet(clearBindings_);
    const u32 cg = (res + 3) / 4;
    ctx.dispatch(cg, cg, cg);
    ctx.uavBarrierTexture(voxelAccumTex_);   // injection must see the cleared accumulator

    // Prefers the mesh-shader voxelise pipeline WHENEVER the device built one, independent of
    // settings_.meshShaders. That flag is a wider, device-level switch -- it also moves the MAIN
    // scene's lit draws onto their own mesh-shader pipeline (see scenePipeline / IDevice::setMeshShaders
    // in SandboxApp.cpp) and stays off by default because turning the whole scene over to a rarely-
    // exercised path is a real behaviour change to opt into. Voxelisation has no such wrinkle: PSVoxel
    // is the SAME pixel shader either way, the pipeline state (cull none, no depth clip, conservative
    // raster, no render target) is copied from the same `vox` desc, and MSVoxel now runs the identical
    // dominant-axis projection VSVoxel+GSVoxel do -- see MSVoxel's own comment for the normal-transform
    // bug this depended on fixing first. createPipelines() already builds and validates voxelMsPso_
    // for any device that reports mesh-shader support, REGARDLESS of this setting, so `voxelMsPso_ != 0`
    // is exactly "the device already proved it can do this" and nothing more needs asking. This is the
    // path GSVoxel exists to be a fallback for, on a device with no mesh-shader tier.
    const bool useMs = voxelMsPso_ != 0;
    ctx.setPipeline(useMs ? voxelMsPso_ : voxelPso_);
    ctx.setBindingSet(bindings_);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
    ctx.setRenderTargets(nullptr, 0, 0);   // no targets at all: the pixel shader writes only the UAV
    ctx.setViewport(0, 0, res, res);
    ctx.setScissor(0, 0, res, res);

    // CULLED AGAINST THE VOLUME, which this pass did not do at all until now: it rasterised every
    // draw in drawsPrev_, including the ones streaming had brought in kilometres away. Those draws
    // were never going to survive -- the pixel shader's UAV write lands outside the 128^3 grid and
    // is dropped -- but the vertex and raster cost was paid in full before that could be discovered.
    // On a streamed scene the volume covers a small fraction of what is resident, so this is most
    // of the pass.
    //
    // The sphere is derived HERE from center_/extent_ rather than read from the giShadowCentre_/
    // giShadowRadius_ that giShadowPass culls against, even though fitGiShadow() computes the same
    // expression immediately before. Reading those would silently couple voxelisation to whether
    // the GI shadow PSO built: when it fails, fitGiShadow() never runs, the fields keep whatever
    // they last held, and the grid would under-voxelise on that device only.
    const Vec3 volCentre{center_[0], center_[1], center_[2]};
    const f32 volRadius = (extent_ > 1.0f ? extent_ : 1.0f) * 1.7320508f;
    u32 voxelSubmitted = 0, voxelCulled = 0;

    for (const Draw& d : drawsPrev_) {
        if (d.boundsRadius >= 0.0f && dist(Vec3{d.boundsCentre[0], d.boundsCentre[1], d.boundsCentre[2]},
                                            volCentre) > volRadius + d.boundsRadius) { ++voxelCulled; continue; }
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

    // Reported on a widening interval, because the ratio is the whole point of the cull and a
    // silent one would be indistinguishable from a cull that never fires. If this ever reads
    // "culled 0", the draws are arriving with boundsRadius < 0 and the cull is a no-op.
    if ((voxelCullLogs_ & (voxelCullLogs_ + 1)) == 0) {
        const u32 considered = voxelSubmitted + voxelCulled;
        AVER_INFO("[Voxi] voxelize {} draw(s), culled {} outside the volume ({:.0f}%)",
                  voxelSubmitted, voxelCulled,
                  considered ? 100.0 * static_cast<f64>(voxelCulled) / static_cast<f64>(considered) : 0.0);
    }
    ++voxelCullLogs_;

    // Reduce the atomic sums into the filterable RGBA16F volume.
    ctx.uavBarrierTexture(voxelAccumTex_);
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    ctx.setPipeline(resolvePso_);
    ctx.setBindingSet(resolveBindings_);
    ctx.dispatch(cg, cg, cg);
}

// Box-filters each level of the radiance volume into the next, then hands the whole chain back as
// a shader resource.
void VoxiRenderer::filterMips(rhi::IRenderContext& ctx) {
    rhi::ScopedGpuStat gpuStat(ctx, "Voxi mip filter");
    ctx.uavBarrierTexture(voxelTex_);
    ctx.setPipeline(mipPso_);
    const u32 res = voxelResBuilt_;
    for (u32 m = 1; m < voxelMips_; ++m) {
        // Level m-1 becomes readable while level m stays writable: a per-subresource transition.
        ctx.textureBarrier(voxelTex_, rhi::ResourceState::UnorderedAccess,
                           rhi::ResourceState::NonPixelShaderResource, m - 1);
        ctx.setBindingSet(mipBindings_[m - 1]);
        const u32 srcMip[4] = {0, 0, 0, 0};   // the single-mip SRV already rebased the Load
        ctx.setConstants(3, srcMip, 4);
        const u32 d = (res >> m) > 0 ? (res >> m) : 1u;
        const u32 g = (d + 3) / 4;
        ctx.dispatch(g, g, g);
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
    if (!giReady_) return false;
    *data = &cb_; *bytes = sizeof(cb_);
    return true;
}

// True once the feature is up: shadowing and the bounce are terms inside Voxi's lit pixel shader.
bool VoxiRenderer::overridesScenePipeline() const { return giReady_; }

// True while the debug view replaces the scene, including the backend's line draws.
bool VoxiRenderer::suppressesScene() const { return giReady_ && giEnabled() && debugView_; }

// Draws the debug raymarch over the colour target and viewport the backend already bound.
void VoxiRenderer::scenePass(rhi::IRenderContext& ctx) {
    if (!debugPso_) return;
    rhi::ScopedGpuStat gpuStat(ctx, "Voxi debug view");
    ctx.setPipeline(debugPso_);
    ctx.setBindingSet(bindings_);
    // Table 1 is bound outright: this pipeline declares it and Tier 1 populates whole tables.
    ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
    ctx.drawFullscreen();
}

// Rebuilds the pipelines that bake the sample count and the target formats, and resizes the
// ray-traced shadow history to match the new resolution.
void VoxiRenderer::onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth,
                                          u32 width, u32 height) {
    if (!res_ || !giReady_) return;
    if (!createScenePipelines(sampleCount, color, depth))
        AVER_ERROR("[Voxi] scene pipelines could not be rebuilt for {} sample(s)", sampleCount);
    // Remembered even when the call below decides to allocate nothing: setSettings needs a size to
    // create at if ray tracing is switched on later, and it is never told one.
    rtHistWantW_ = width;
    rtHistWantH_ = height;
    if (!ensureShadowHistory(width, height))
        AVER_ERROR("[Voxi] ray-traced shadow history could not be (re)created at {}x{}", width, height);
}

// (Re)creates the ray-traced shadow AND reflection histories at the given resolution. All four
// textures are destroyed and rebuilt together: one at the wrong size with another right would
// corrupt reprojection, and the two pairs share rtHistWriteIdx_/rtHistValid_ so they must always
// agree on whether a previous frame's contents exist at all.
bool VoxiRenderer::ensureShadowHistory(u32 width, u32 height) {
    if (!res_ || !bindings_ || width == 0 || height == 0) return false;

    // NOTHING AT ALL WHEN RAY TRACING IS OFF, which is the shipped default and what most projects
    // run. These four textures exist solely to carry a previous frame's traced visibility and
    // reflection colour between frames; with rayTracing at Quality::Off nothing writes them and
    // nothing reads them, and buildAccelerationStructures has already returned early for the same
    // reason. This used to allocate regardless, because the only gate above it was `giReady_` --
    // around 225 MB of VRAM at this machine's 3532x1987, held for a feature that never runs. Teardown
    // rather than a bare early-out, so switching ray tracing OFF at runtime gives the memory back
    // instead of stranding it for the process lifetime.
    if (!rayTracingWanted()) {
        const bool had = rtShadowHist_[0] || rtReflHist_[0];
        for (rhi::TextureHandle& t : rtShadowHist_) { if (t) res_->destroyTexture(t); t = 0; }
        for (rhi::TextureHandle& t : rtReflHist_)   { if (t) res_->destroyTexture(t); t = 0; }
        rtShadowHistW_ = rtShadowHistH_ = 0;
        rtHistWriteIdx_ = 0;
        rtHistValid_ = false;
        // Idempotent by construction: called from both onRenderTargetsChanged and setSettings, and
        // the handles are zeroed above, so a second call finds nothing left to destroy. Not an
        // error -- "allocated nothing because nothing needs it" is success.
        if (had) AVER_INFO("[Voxi] ray-traced history released; ray tracing is off");
        return true;
    }

    if (rtShadowHist_[0] && rtShadowHist_[1] && rtReflHist_[0] && rtReflHist_[1] &&
        rtShadowHistW_ == width && rtShadowHistH_ == height)
        return true;

    for (rhi::TextureHandle& t : rtShadowHist_) { if (t) res_->destroyTexture(t); t = 0; }
    for (rhi::TextureHandle& t : rtReflHist_)   { if (t) res_->destroyTexture(t); t = 0; }
    rtHistValid_ = false;   // the old contents, if any, belonged to a resolution that no longer exists

    rhi::TextureDesc d;
    d.dim    = rhi::TextureDim::Tex2D;
    d.width  = width;
    d.height = height;
    d.mips   = 1;
    // x = visibility, y = linear depth (view-space, centimetres) -- the depth channel is what lets
    // rtReprojectHistory tell a reprojection that lands on an already-populated texel apart from
    // one that has actually disoccluded: a silhouette edge can reproject to the SAME screen texel
    // while the surface now visible through it sits at a very different depth.
    d.format = rhi::Format::RG32Float;
    d.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
    d.initialState = rhi::ResourceState::ShaderResource;   // where a texture rests between frames
    d.debugName = "Voxi RT shadow history A";
    rtShadowHist_[0] = res_->createTexture(d);
    d.debugName = "Voxi RT shadow history B";
    rtShadowHist_[1] = res_->createTexture(d);
    if (!rtShadowHist_[0] || !rtShadowHist_[1]) return false;

    // rgb = the reflection's own shaded colour, a = linear depth of the hit, OR a NEGATIVE
    // sentinel meaning the ray missed and there is nothing here to reuse -- see
    // rtReflectionTemporal in VoxiShaders.hpp for why a miss is never reprojected (sky-by-direction
    // is cheap and highly view dependent, so it is recomputed fresh every frame regardless of
    // turn). RGBA16F rather than RGBA32F: HDR colour does not need full float precision, and this
    // halves the bandwidth of a buffer already twice the size of the shadow one.
    d.format = rhi::Format::RGBA16F;
    d.debugName = "Voxi RT reflection history A";
    rtReflHist_[0] = res_->createTexture(d);
    d.debugName = "Voxi RT reflection history B";
    rtReflHist_[1] = res_->createTexture(d);
    if (!rtReflHist_[0] || !rtReflHist_[1]) return false;

    rtShadowHistW_ = width;
    rtShadowHistH_ = height;
    rtHistWriteIdx_ = 0;
    // A starting bind, not the steady state: beginShadowHistory rebinds whichever side is which
    // every frame as the roles swap.
    res_->setUav(bindings_, 2, rtShadowHist_[0], 0);
    res_->setSrv(bindings_, 6, rtShadowHist_[1]);
    res_->setUav(bindings_, 3, rtReflHist_[0], 0);
    res_->setSrv(bindings_, 7, rtReflHist_[1]);
    return true;
}

// See VoxiRenderer.hpp for the ping-pong rationale.
void VoxiRenderer::beginShadowHistory(rhi::IRenderContext& ctx) {
    // Both default to 0: an unbound t6/u2 is Tier 1 null-filled, and the shader must not touch
    // either slot unless THIS frame actually bound them to real textures below.
    cb_.rtHistParams[0] = 0.0f;
    cb_.rtHistParams[1] = 0.0f;
    if (!shadowHistoryActive()) return;

    const u32 writeIdx = rtHistWriteIdx_;
    const u32 readIdx  = 1 - writeIdx;

    // The write texture rests as ShaderResource between frames; make it writable. The read texture,
    // if it holds a real previous frame, is still sitting in UnorderedAccess from when IT was last
    // frame's write target -- flip it back to readable. Both pairs share writeIdx/readIdx: they
    // always swap together (see the member comment in VoxiRenderer.hpp).
    ctx.textureBarrier(rtShadowHist_[writeIdx], rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    ctx.textureBarrier(rtReflHist_[writeIdx],   rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    if (rtHistValid_) {
        ctx.textureBarrier(rtShadowHist_[readIdx], rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
        ctx.textureBarrier(rtReflHist_[readIdx],   rhi::ResourceState::UnorderedAccess, rhi::ResourceState::ShaderResource);
    }

    res_->setUav(bindings_, 2, rtShadowHist_[writeIdx], 0);
    res_->setSrv(bindings_, 6, rtShadowHist_[readIdx]);
    res_->setUav(bindings_, 3, rtReflHist_[writeIdx], 0);
    res_->setSrv(bindings_, 7, rtReflHist_[readIdx]);

    // Read fresh every frame rather than cached: unlike the texture resolution, the scene viewport
    // can change (an editor panel resize) without a full onRenderTargetsChanged notification. A
    // transient failure here only disables BLENDING for this one frame -- writing the fresh sample
    // out still needs nothing from it.
    const bool haveViewport = dev_ && dev_->sceneViewport(curSceneViewport_);

    std::memcpy(cb_.prevViewProj, prevViewProj_, sizeof(prevViewProj_));
    std::memcpy(cb_.sceneViewport, prevSceneViewport_, sizeof(prevSceneViewport_));
    cb_.rtHistParams[0] = 1.0f;                                        // t6/u2 are bound to real textures
    cb_.rtHistParams[1] = (rtHistValid_ && haveViewport) ? 1.0f : 0.0f; // ...and t6 + gSceneViewport are usable
    cb_.rtHistParams[2] = static_cast<f32>(rtFrameIndex_);
    // The TILE EDGE (rtPixelsPerRayTile_) as its bit count, not the edge itself: the shader masks
    // and shifts by this rather than multiplying or taking a modulo. rtPixelsPerRayTile_ is always
    // an exact power of two -- setPixelsPerRayTile only ever rounds to one -- so this always lands
    // on an exact integer.
    u32 tileBits = 0;
    for (u32 v = rtPixelsPerRayTile_; v > 1; v >>= 1) ++tileBits;
    cb_.rtHistParams[3] = static_cast<f32>(tileBits);
}

void VoxiRenderer::endShadowHistory() {
    if (!shadowHistoryActive()) return;
    std::memcpy(prevViewProj_, curViewProj_, sizeof(curViewProj_));
    std::memcpy(prevSceneViewport_, curSceneViewport_, sizeof(curSceneViewport_));
    rtHistWriteIdx_ = 1 - rtHistWriteIdx_;
    rtHistValid_ = true;
}

// Returns the lit pipeline for this frame, or 0 to decline and let the backend use its own.
rhi::PipelineHandle VoxiRenderer::scenePipeline(bool meshShaders, bool wireframe) const {
    if (wireframe) return 0;
    if (rtActive_) return meshShaders && sceneMsRtPso_ ? sceneMsRtPso_ : sceneRtPso_;
    return meshShaders && sceneMsPso_ ? sceneMsPso_ : scenePso_;
}

// ---------------------------------------------------------------- resources

// Creates the cascaded shadow atlas.
bool VoxiRenderer::createShadowResources() {
    rhi::TextureDesc d;
    d.dim    = rhi::TextureDim::Tex2D;
    d.width  = kShadowSize;
    d.height = kShadowSize;
    // Typeless so the DSV can see D32Float while the SRV sees R32Float: one resource, two views.
    d.format = rhi::Format::R32Typeless;
    d.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::DepthStencil;
    d.initialState  = rhi::ResourceState::ShaderResource;
    d.hasClearValue = true;
    d.clearDepth    = 1.0f;
    d.debugName     = "Voxi shadow map";
    shadowTex_ = res_->createTexture(d);
    if (!shadowTex_) AVER_ERROR("[Voxi] shadow map {}^2 could not be created", kShadowSize);

    // The GI-only map: same typeless DSV/SRV trick, same clear value, one box instead of a 2x2
    // atlas -- so it needs a fraction of the texels. See kGiShadowSize.
    d.width  = kGiShadowSize;
    d.height = kGiShadowSize;
    d.debugName = "Voxi GI shadow map";
    giShadowTex_ = res_->createTexture(d);
    if (!giShadowTex_)
        AVER_WARN("[Voxi] GI-only shadow map {}^2 could not be created; indirect light will be "
                  "injected unshadowed", kGiShadowSize);

    // A WARNING, NOT A FAILURE, and only shadowTex_ gates the return. The GI map affects bounce
    // light alone; losing it should degrade the picture, not refuse to start the renderer.
    return shadowTex_ != 0;
}

// Creates the radiance volume, the injection accumulator and every binding set over them.
bool VoxiRenderer::createVoxelVolume(u32 resolution) {
    rhi::TextureDesc d;
    d.dim    = rhi::TextureDim::Tex3D;
    d.width  = resolution;
    d.height = resolution;
    d.depth  = resolution;
    d.mips   = 0;                       // full chain: mip N is the cone footprint at distance N
    d.format = rhi::Format::RGBA16F;
    d.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::UnorderedAccess;
    d.initialState = rhi::ResourceState::ShaderResource;   // where the chain rests between frames
    d.debugName    = "Voxi radiance volume";
    voxelTex_ = res_->createTexture(d);
    if (!voxelTex_) { AVER_ERROR("[Voxi] radiance volume {}^3 could not be created", resolution); return false; }

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
    ad.initialState = rhi::ResourceState::UnorderedAccess;   // where it stays: nothing reads it as an SRV
    ad.debugName    = "Voxi injection accumulator";
    voxelAccumTex_ = res_->createTexture(ad);
    if (!voxelAccumTex_) { AVER_ERROR("[Voxi] injection accumulator {}^3 could not be created", resolution); return false; }

    // The resolved mip count, never a recomputed log2.
    rhi::TextureDesc got{};
    if (!res_->textureInfo(voxelTex_, got)) { AVER_ERROR("[Voxi] textureInfo failed for the radiance volume"); return false; }
    voxelMips_     = got.mips;
    voxelResBuilt_ = resolution;

    // Main table. Each slot declares its kind because Tier 1 hardware null-fills by dimension.
    rhi::BindingSetDesc bd;
    // kGiSrvCount/kGiUavCount, matching giLayout()'s l.srvCount/l.uavCount exactly -- see
    // VoxiGiShaders.hpp's own comment: this used to be two independent literals ("9"/"4") that
    // nothing checked agreed, and is now the same two constants both sites read.
    bd.srvCount = kGiSrvCount;
    bd.uavCount = kGiUavCount;
    bd.srvKinds[0] = rhi::SlotKind::Texture3D;              // t0 volume, whole chain
    bd.srvKinds[1] = rhi::SlotKind::Texture2D;              // t1 shadow map
    bd.srvKinds[2] = rhi::SlotKind::AccelerationStructure;  // t2 TLAS, filled once one exists
    // Null-filled until the table exists. Tier 1 requires a valid descriptor of the right KIND in
    // every declared slot, so these must be declared as structured buffers even while empty.
    bd.srvKinds[3] = rhi::SlotKind::StructuredBuffer;       // t3 flat vertices
    bd.srvKinds[4] = rhi::SlotKind::StructuredBuffer;       // t4 flat indices
    bd.srvKinds[5] = rhi::SlotKind::StructuredBuffer;       // t5 per-instance records
    bd.srvKinds[6] = rhi::SlotKind::Texture2D;              // t6 ray-traced shadow history (read)
    bd.srvKinds[7] = rhi::SlotKind::Texture2D;              // t7 ray-traced reflection history (read)
    bd.srvKinds[8] = rhi::SlotKind::Texture2D;              // t8 GI-only shadow map
    bd.uavKinds[0] = rhi::SlotKind::Texture3D;              // u0 volume mip 0
    bd.uavKinds[1] = rhi::SlotKind::Texture3D;              // u1 injection accumulator
    bd.uavKinds[2] = rhi::SlotKind::Texture2D;              // u2 ray-traced shadow history (write)
    bd.uavKinds[3] = rhi::SlotKind::Texture2D;              // u3 ray-traced reflection history (write)
    bindings_ = res_->createBindingSet(bd);
    if (!bindings_) { AVER_ERROR("[Voxi] main binding set could not be created"); return false; }
    res_->setSrv(bindings_, 0, voxelTex_, rhi::kAllMips);
    if (shadowTex_) res_->setSrv(bindings_, 1, shadowTex_);
    if (giShadowTex_) res_->setSrv(bindings_, 8, giShadowTex_);   // t8, the GI-only shadow map
    res_->setUav(bindings_, 0, voxelTex_, 0);
    res_->setUav(bindings_, 1, voxelAccumTex_, 0);
    // t6/u2 (rtShadowHist_) and t7/u3 (rtReflHist_) are populated once onRenderTargetsChanged
    // creates them -- the resolution is not known this early, and the slots are declared above so
    // Tier 1 null-fills them correctly until then.

    // The clear and the resolve get UAV-only sets: while they run every mip of the volume is in
    // UnorderedAccess, so no SRV descriptor over it may be live.
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

    // One set per filter step. A single-mip source view is what makes reading level m-1 while
    // writing level m legal.
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

// THE MODULAR SEAM (Stage 3, GPU per-cluster shading parity): a caller that has merged Voxi's
// table-0 union into a binding set IT owns (see VoxiGiShaders.hpp's own comment on why, and
// SandboxApp.cpp's ensureLodMeshPipeline for the one real consumer) asks Voxi to populate the
// slots it reserved, rather than reaching into voxelTex_/shadowTex_ itself -- Voxi stays the only
// code that knows those handles are Texture3D/Texture2D or when they are still null, and the
// caller never has to learn a cluster from this side either.
//
// ONLY THE TWO SLOTS giShaderPrelude() DECLARES A SYMBOL FOR: the GI volume at srvBase, the shadow
// map at srvBase+1. The other kGiSrvCount-2 SRVs and every one of kGiUavCount's UAVs are part of
// the SAME table-0 union (a caller's layout still reserves all of them, so its register numbers
// past this pair land where giLayout()'s own do) but exist for Voxi's ray-traced/GI-only-shadow
// state, which giShaderPrelude() never declares a register for -- there is nothing to bind there
// because nothing will ever read it. `res` is passed in rather than read from res_ so this compiles
// and is callable even from a caller that only has an rhi::IResourceFactory&, not a VoxiRenderer's
// own device handle.
void VoxiRenderer::bindGiResources(rhi::IResourceFactory& res, rhi::BindingSetHandle set, u32 srvBase) const {
    // Guarded exactly like bindings_'s own population above: init() may not have finished, or the
    // volume/shadow resolution may be mid-rebuild, and Tier 1's null-fill is a defined "reads as
    // empty" for whichever of the two is not ready yet -- not a hazard, and not this function's
    // problem to report. The caller finds out nothing shadowed/bounced this frame the same way it
    // would from Voxi's own pipeline: gShadowParams.y and gVoxelParams.w, read inside the shader.
    if (voxelTex_)  res.setSrv(set, srvBase + 0, voxelTex_, rhi::kAllMips);
    if (shadowTex_) res.setSrv(set, srvBase + 1, shadowTex_);
}

// ---------------------------------------------------------------- pipelines

// Creates every pipeline the feature runs. Mesh-shader and RayQuery variants are optional.
bool VoxiRenderer::createPipelines() {
    const bool msOk = caps_.meshShaderTier > 0 && caps_.shaderModel >= 65 && caps_.dxcAvailable;

    ShaderScope compile(*res_);

    const rhi::PipelineLayout gi = giLayout();
    // Raster pipelines declare the material table, so their shaders are told which registers it
    // landed at. The compute ones must not be told: they declare no second table.
    const std::string matDefs = pbr::materialShaderDefines(gi.srvCount, kMaterialSamplerSlot);
    auto rasterDefs = [&](const char* extra) { return extra ? matDefs + ";" + extra : matDefs; };

    // --- 1. shadow map: depth only, from the sun ---
    if (const rhi::ShaderHandle vs = compile("VSShadow", rhi::ShaderStage::Vertex, kBaseSm, rasterDefs(nullptr).c_str())) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vs;                                   // no pixel shader: depth is the only output
        p.layout = gi;
        p.cull = rhi::CullMode::None;
        p.depth = {true, true, rhi::CompareOp::Less};
        p.renderTargetCount = 0;
        p.depthFormat = rhi::Format::D32Float;
        p.sampleCount = 1;                           // the shadow map is never multisampled
        p.slopeScaledDepthBias = 1.5f;
        shadowPso_ = res_->createGraphicsPipeline(p);
    }
    if (!shadowPso_) AVER_ERROR("[Voxi] shadow pipeline unavailable");

    // --- 1b. the same depth-only pass, but instanced: one DrawIndexedInstanced per mesh per cascade
    // instead of one drawMesh() per surviving draw per cascade. See VSShadowInstanced in
    // VoxiShaders.hpp and IRenderContext::drawMeshInstanced (RHIResources.hpp) for the mechanism.
    // AVER_INSTANCE_SRV must be the SAME number GraphicsPipelineDesc::instanced makes the backend
    // reserve for this exact layout -- declaredSrvCount(gi), the register right past every t-register
    // `gi` itself declares across both binding tables (see the comment above `instanced` in
    // RHIResources.hpp for why this can't be shared any other way). Optional: shadowPass() falls
    // back to shadowPso_'s one-draw-per-instance path if this failed to build.
    const std::string instDefs = rasterDefs(("AVER_INSTANCE_SRV=" + std::to_string(rhi::declaredSrvCount(gi))).c_str());
    // SM 6.0 AND DXC, OR NO INSTANCED SHADOWS AT ALL. This gate is a bug fix, not caution.
    //
    // The instanced path feeds per-instance transforms through a StructuredBuffer indexed by
    // SV_InstanceID. Under the FXC / SM 5.1 fallback (--force-caps no-dxc) that entry point still
    // COMPILES -- so shadowInstancedPso_ came back non-zero and shadowPass took the instanced branch
    // -- but the per-instance transforms did not arrive, every shadow caster rasterised with a
    // garbage world matrix, and the shadow simply was not where the geometry was.
    //
    // It failed silently and it failed as a picture, not as an error: the gate oracle caught it as
    // `shadow` reading 90,85,80 against a recorded 23,27,32, i.e. a shadowed floor pixel that had
    // become exactly as bright as the sunlit one, and the invariant "shadow/sunlit must bracket the
    // lighting" is what named it. Nothing logged, nothing crashed.
    //
    // shadowPass already falls back to shadowPso_'s one-draw-per-instance path whenever this handle
    // is 0, so refusing to build it here is the whole fix -- the SM 5.1 path goes back to exactly
    // what it did before instancing existed.
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
        shadowInstancedPso_ = res_->createGraphicsPipeline(p);
    }
    if (!shadowInstancedPso_ && instancedShadowsOk)
        AVER_WARN("[Voxi] instanced shadow pipeline unavailable; shadowPass falls back to one draw per instance");

    // --- 1c. the GI-only depth pass, plain and instanced. Same pipeline state as the cascade pair
    // above in every respect; only the entry point differs, because the matrix it transforms into
    // (gGiShadowViewProj vs gCascadeViewProj[gShadowDraw.x]) is baked per pipeline rather than
    // chosen per draw. Both are OPTIONAL: giShadowPass skips entirely without the plain one, and
    // falls back to one draw per instance without the instanced one.
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
        giShadowPso_ = res_->createGraphicsPipeline(p);
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
        giShadowInstancedPso_ = res_->createGraphicsPipeline(p);
    }
    if (!giShadowPso_)
        AVER_WARN("[Voxi] GI-only shadow pipeline unavailable; indirect light is injected unshadowed");

    // --- 2/3. voxelisation + light injection: rasterise with NO render target ---
    const rhi::ShaderHandle psVoxel = compile("PSVoxel", rhi::ShaderStage::Pixel, kBaseSm, rasterDefs(nullptr).c_str());
    rhi::GraphicsPipelineDesc vox;
    vox.layout = gi;
    vox.cull = rhi::CullMode::None;
    vox.depthClip = false;                           // a triangle outside the slab still voxelises
    // Widens rasterisation so thin geometry still covers a voxel.
    vox.conservativeRaster = caps_.conservativeRaster;
    vox.renderTargetCount = 0;                       // the pixel shader's only output is the UAV
    vox.depthFormat = rhi::Format::Unknown;
    vox.sampleCount = 1;

    const rhi::ShaderHandle vsVoxel = compile("VSVoxel", rhi::ShaderStage::Vertex, kBaseSm, rasterDefs(nullptr).c_str());
    const rhi::ShaderHandle gsVoxel = compile("GSVoxel", rhi::ShaderStage::Geometry, kBaseSm, rasterDefs(nullptr).c_str());
    if (vsVoxel && gsVoxel && psVoxel) {
        rhi::GraphicsPipelineDesc p = vox;
        p.vs = vsVoxel; p.gs = gsVoxel; p.ps = psVoxel;
        voxelPso_ = res_->createGraphicsPipeline(p);
    }
    if (!voxelPso_) AVER_ERROR("[Voxi] voxelise pipeline unavailable");

    if (msOk && psVoxel) {
        // The geometry registers sit just past whatever SRV table this pipeline declared, so they
        // are derived from the same layout the pipeline is created with.
        const std::string msDefs = rasterDefs("AVER_MS=1") + ";" + rhi::meshGeometryDefines(vox.layout);
        if (const rhi::ShaderHandle ms = compile("MSVoxel", rhi::ShaderStage::Mesh, 65, msDefs.c_str())) {
            rhi::GraphicsPipelineDesc p = vox;
            p.ms = ms; p.ps = psVoxel;
            voxelMsPso_ = res_->createGraphicsPipeline(p);
        }
        if (!voxelMsPso_) AVER_WARN("[Voxi] mesh-shader voxelise variant unavailable; the GS path stands in");
    }

    // --- 4. clear the accumulator, and reduce it into mip 0. UAV-only layouts, matching the
    //        UAV-only binding sets. ---
    if (const rhi::ShaderHandle cs = compile("CSClear", rhi::ShaderStage::Compute, kBaseSm, nullptr)) {
        rhi::ComputePipelineDesc p;
        p.cs = cs;
        p.layout.uavCount = 2;
        clearPso_ = res_->createComputePipeline(p);
    }
    if (!clearPso_) AVER_ERROR("[Voxi] volume clear pipeline unavailable");

    if (const rhi::ShaderHandle cs = compile("CSResolve", rhi::ShaderStage::Compute, kBaseSm, nullptr)) {
        rhi::ComputePipelineDesc p;
        p.cs = cs;
        p.layout.uavCount = 2;
        resolvePso_ = res_->createComputePipeline(p);
    }
    if (!resolvePso_) AVER_ERROR("[Voxi] injection resolve pipeline unavailable");

    // --- 5. mip filter: one source mip in, one destination mip out. ---
    if (const rhi::ShaderHandle cs = compile("CSMip", rhi::ShaderStage::Compute, kBaseSm, nullptr)) {
        rhi::ComputePipelineDesc p;
        p.cs = cs;
        p.layout.srvCount = 1;
        p.layout.uavCount = 1;
        p.layout.constantDwords[3] = 4;   // b3: source mip index (b0/b1 belong to the raster path)
        mipPso_ = res_->createComputePipeline(p);
    }
    if (!mipPso_) AVER_ERROR("[Voxi] mip filter pipeline unavailable");

    // --- 6-10. everything that bakes the sample count and the target formats. ---
    const bool sceneOk = createScenePipelines(dev_->sampleCount(), dev_->backbufferFormat(),
                                              dev_->depthFormat());

    return shadowPso_ && voxelPso_ && clearPso_ && mipPso_ && sceneOk;
}

// Creates the five pipelines that bake the sample count and the render-target formats: the debug
// view and the four scene lit variants. Replaces whatever was there.
bool VoxiRenderer::createScenePipelines(u32 sampleCount, rhi::Format color, rhi::Format depth) {
    const bool msOk = caps_.meshShaderTier > 0 && caps_.shaderModel >= 65 && caps_.dxcAvailable;
    const bool rtOk = caps_.rayTracingTier >= 11 && caps_.shaderModel >= 65 && caps_.dxcAvailable;

    const rhi::PipelineHandle stale[] = {debugPso_, scenePso_, sceneMsPso_, sceneRtPso_, sceneMsRtPso_};
    for (rhi::PipelineHandle p : stale) if (p) res_->destroyPipeline(p);
    debugPso_ = scenePso_ = sceneMsPso_ = sceneRtPso_ = sceneMsRtPso_ = 0;

    ShaderScope compile(*res_);
    const rhi::PipelineLayout gi = giLayout();
    // Every pipeline here is a raster one, so every shader is told the material registers.
    const std::string matDefs = pbr::materialShaderDefines(gi.srvCount, kMaterialSamplerSlot);
    auto rasterDefs = [&](const char* extra) { return extra ? matDefs + ";" + extra : matDefs; };

    // --- 6. debug: raymarch the volume to screen (shares the prelude's fullscreen triangle). ---
    const rhi::ShaderHandle vsky = compile("VSky", rhi::ShaderStage::Vertex, kBaseSm, rasterDefs(nullptr).c_str());
    if (const rhi::ShaderHandle ps = compile("PSVoxelDebug", rhi::ShaderStage::Pixel, kBaseSm, rasterDefs(nullptr).c_str()); ps && vsky) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsky; p.ps = ps;
        p.layout = gi;
        p.cull = rhi::CullMode::None;
        p.renderTargetCount = 1;
        p.renderTargets[0] = color;
        p.sampleCount = sampleCount;
        debugPso_ = res_->createGraphicsPipeline(p);
    }
    if (!debugPso_) AVER_ERROR("[Voxi] voxel debug pipeline unavailable");

    // --- 7-10. scene lit variants: the cone trace and RayQuery live inside the pixel shader. ---
    rhi::GraphicsPipelineDesc scene;
    scene.layout = gi;
    scene.cull = rhi::CullMode::None;
    scene.depth = {true, true, rhi::CompareOp::Less};
    scene.renderTargetCount = 1;
    scene.renderTargets[0] = color;
    scene.depthFormat = depth;
    scene.sampleCount = sampleCount;

    const rhi::ShaderHandle vsMain = compile("VSMain", rhi::ShaderStage::Vertex, kBaseSm, rasterDefs(nullptr).c_str());
    const rhi::ShaderHandle psVoxi = compile("PSMainVoxi", rhi::ShaderStage::Pixel, kBaseSm, rasterDefs(nullptr).c_str());
    if (vsMain && psVoxi) {
        rhi::GraphicsPipelineDesc p = scene;
        p.vs = vsMain; p.ps = psVoxi;
        scenePso_ = res_->createGraphicsPipeline(p);
    }
    if (!scenePso_) AVER_ERROR("[Voxi] scene pipeline unavailable");

    // MSMain comes from the prelude and needs AVER_MS, plus this layout's geometry registers.
    const std::string msDefs = rasterDefs("AVER_MS=1") + ";" + rhi::meshGeometryDefines(scene.layout);
    const rhi::ShaderHandle msMain = msOk ? compile("MSMain", rhi::ShaderStage::Mesh, 65, msDefs.c_str()) : 0;
    if (msMain && psVoxi) {
        rhi::GraphicsPipelineDesc p = scene;
        p.ms = msMain; p.ps = psVoxi;
        sceneMsPso_ = res_->createGraphicsPipeline(p);
    }
    if (msOk && !sceneMsPso_) AVER_WARN("[Voxi] mesh-shader scene variant unavailable");

    // RayQuery replaces the shadow-map lookup with an exact occlusion ray: a second PS at SM 6.5.
    const rhi::ShaderHandle psRt = rtOk ? compile("PSMainVoxi", rhi::ShaderStage::Pixel, 65, rasterDefs("AVER_RT=1").c_str()) : 0;
    if (vsMain && psRt) {
        rhi::GraphicsPipelineDesc p = scene;
        p.vs = vsMain; p.ps = psRt;
        sceneRtPso_ = res_->createGraphicsPipeline(p);
    }
    if (rtOk && !sceneRtPso_) AVER_WARN("[Voxi] ray-tracing scene variant unavailable");

    if (msMain && psRt) {
        rhi::GraphicsPipelineDesc p = scene;
        p.ms = msMain; p.ps = psRt;
        sceneMsRtPso_ = res_->createGraphicsPipeline(p);
    }
    if (msOk && rtOk && !sceneMsRtPso_) AVER_WARN("[Voxi] mesh-shader + ray-tracing scene variant unavailable");

    // Only the two mandatory ones: scenePipeline() falls back when the optional variants are absent.
    return debugPso_ && scenePso_;
}

} // namespace aver::voxi
