#include "aver/voxi/VoxiRenderer.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"   // light-frustum fit: Vec3 / Mat4::lookAtLH
#include "aver/pbr/MaterialSystem.hpp"
#include "aver/pbr/PbrShaders.hpp"

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

// Cascade split blend: 0 is uniform slabs, 1 is logarithmic (equal ratios).
constexpr f32 kCascadeSplitLambda = 0.85f;

// How far the cascades reach, as a multiple of the camera's near plane.
constexpr f32 kShadowRangeFromNear = 4000.0f;

// The draw-list cap, and therefore the instance count the TLAS is sized for.
constexpr u32 kMaxDraws = 4096;

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
    // ray reads after a hit: t3 vertices, t4 indices, t5 instances. The material table is BASED on
    // this count rather than at a fixed register, so widening table 0 rebases it automatically.
    l.srvCount = 6;
    l.uavCount = 2;              // u0 volume mip 0, u1 injection accumulator
    l.srvCount1 = pbr::kMaterialSrvCount;   // table 1: the material's textures, based at t3
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
              "pipelines shadow={} voxel={} voxelMs={} clear={} resolve={} mip={} debug={} scene={}/{}/{}/{}",
              shadowTex_, voxelTex_, voxelAccumTex_, voxelResBuilt_, voxelMips_,
              bindings_, clearBindings_, resolveBindings_, static_cast<u32>(mipBindings_.size()),
              shadowPso_, voxelPso_, voxelMsPso_, clearPso_, resolvePso_, mipPso_, debugPso_,
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

    const rhi::PipelineHandle psos[] = {shadowPso_, voxelPso_, voxelMsPso_, mipPso_, clearPso_,
                                        resolvePso_, debugPso_, scenePso_, sceneMsPso_, sceneRtPso_,
                                        sceneMsRtPso_};
    for (rhi::PipelineHandle p : psos) if (p) res_->destroyPipeline(p);
    shadowPso_ = voxelPso_ = voxelMsPso_ = mipPso_ = clearPso_ = resolvePso_ = debugPso_ = 0;
    scenePso_ = sceneMsPso_ = sceneRtPso_ = sceneMsRtPso_ = 0;

    if (voxelAccumTex_) res_->destroyTexture(voxelAccumTex_);
    if (voxelTex_)  res_->destroyTexture(voxelTex_);
    if (shadowTex_) res_->destroyTexture(shadowTex_);
    voxelAccumTex_ = voxelTex_ = shadowTex_ = 0;

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

void VoxiRenderer::setSettings(const Settings& s) { settings_ = s; }

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
    if (mesh == 0 || draws_.size() >= kMaxDraws) return;
    Draw d;
    d.mesh = mesh;
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
    cb_.voxelParams[3] = (giEnabled() && !debugView_) ? 1.0f : 0.0f;   // gates the cone trace

    buildAccelerationStructures(ctx);
    shadowPass(ctx);
    if (giEnabled()) {
        voxelizePass(ctx);
        filterMips(ctx);
    }
}

// Builds a bottom-level structure for every referenced mesh, then one top-level structure over the
// replayed draw list. Publishes shadowParams.z so the lit pass knows whether it may trace.
void VoxiRenderer::buildAccelerationStructures(rhi::IRenderContext& ctx) {
    rtActive_ = false;
    cb_.shadowParams[2] = 0.0f;
    if (!rtSupported_ || settings_.rayTracing == Quality::Off || drawsPrev_.empty()) return;

    ctx.pushMarker("Voxi acceleration structures");
    std::vector<rhi::TlasInstance> inst;
    inst.reserve(drawsPrev_.size());
    rtInstanceData_.clear();
    rtInstanceMesh_.clear();
    rebuiltThisFrame_.clear();
    u32 firstBuilds = 0;
    for (const Draw& d : drawsPrev_) {
        auto it = blas_.find(d.mesh);
        if (it == blas_.end()) {
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
    if (inst.empty()) { ctx.popMarker(); return; }

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
    ctx.popMarker();
}

// Builds one orthographic light frustum per cascade, fitted to a slice of the camera's view, and
// writes the matrices and splits into cb_. Returns the usable cascade count, 0 if there is no camera.
u32 VoxiRenderer::fitCascades() {
    f32 invViewProj[16] = {};
    f32 camPos[3] = {};
    if (!dev_ || !dev_->camera(nullptr, invViewProj, camPos)) return 0;

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

    const Vec3 volCentre{center_[0], center_[1], center_[2]};
    const f32 volRadius = (extent_ > 1.0f ? extent_ : 1.0f) * 1.7320508f;   // the box's circumsphere

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

        // The last cascade takes the union with the GI volume: the voxelisation pass samples this
        // same map for every voxel it injects, and the volume is not tied to the camera.
        if (c == kShadowCascades - 1) {
            const Vec3 delta = volCentre - centre;
            const f32 d = delta.size();
            if (d + volRadius > radius) {                 // the volume is not already inside
                if (d + radius <= volRadius) {            // ...and the slice is inside the volume
                    centre = volCentre;
                    radius = volRadius;
                } else {
                    const f32 R = (radius + volRadius + d) * 0.5f;
                    centre = centre + delta * ((R - radius) / (d > 1e-4f ? d : 1.0f));
                    radius = R;
                }
            }
        }

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
void VoxiRenderer::shadowPass(rhi::IRenderContext& ctx) {
    if (!shadowPso_ || drawsPrev_.empty()) { cb_.shadowParams[1] = 0.0f; return; }
    const u32 cascades = fitCascades();
    if (cascades == 0) { cb_.shadowParams[1] = 0.0f; return; }

    cb_.shadowParams[0] = 1.0f / static_cast<f32>(kShadowSize);
    cb_.shadowParams[1] = 1.0f;
    cb_.shadowParams[3] = static_cast<f32>(cascades);

    ctx.pushMarker("Voxi shadow");
    ctx.textureBarrier(shadowTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::DepthWrite);
    ctx.setPipeline(shadowPso_);
    ctx.setBindingSet(bindings_);       // Tier 1: bind every declared table, read or not
    ctx.setRenderTargets(nullptr, 0, shadowTex_);
    ctx.clearDepth(shadowTex_, 1.0f);   // the whole atlas, once, before any quadrant is drawn

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

        for (const Draw& d : drawsPrev_) {
            // Sphere-sphere overlap against this cascade's own fitted bounds: a draw whose bounding
            // sphere cannot reach the cascade's box cannot cast a shadow into it. A negative
            // boundsRadius means the backend had no bounds for this mesh -- draw it regardless
            // rather than guess.
            if (d.boundsRadius >= 0.0f) {
                const Vec3 dc{d.boundsCentre[0], d.boundsCentre[1], d.boundsCentre[2]};
                if (dist(dc, cascCentre) > cascRadius + d.boundsRadius) continue;
            }
            f32 consts[rhi::kObjectConstantDwords]{};
            std::memcpy(consts, d.world, 16 * sizeof(f32));   // depth-only: nothing else is read
            ctx.setConstants(rhi::kObjectConstantRegister, consts, rhi::kObjectConstantDwords);
            // Bound even though this pass is depth-only: Tier 1 requires table 1 populated.
            if (d.matSet) ctx.setDrawBinding(d.matSet, d.mat, d.matBytes);
            else          ctx.setDrawBinding(materials_.fallbackBindingSet(),
                                             &materials_.fallbackConstants(), sizeof(pbr::MaterialConstants));
            ctx.drawMesh(d.mesh);
        }
    }

    ctx.textureBarrier(shadowTex_, rhi::ResourceState::DepthWrite, rhi::ResourceState::ShaderResource);
    ctx.popMarker();
}

// Clears the accumulator, rasterises the scene into it with direct lighting applied, then resolves
// it into mip 0 of the radiance volume.
void VoxiRenderer::voxelizePass(rhi::IRenderContext& ctx) {
    const u32 res = voxelResBuilt_;
    ctx.pushMarker("Voxi voxelise");

    ctx.setPipeline(clearPso_);
    ctx.setBindingSet(clearBindings_);
    const u32 cg = (res + 3) / 4;
    ctx.dispatch(cg, cg, cg);
    ctx.uavBarrierTexture(voxelAccumTex_);   // injection must see the cleared accumulator

    const bool useMs = settings_.meshShaders && voxelMsPso_;
    ctx.setPipeline(useMs ? voxelMsPso_ : voxelPso_);
    ctx.setBindingSet(bindings_);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
    ctx.setRenderTargets(nullptr, 0, 0);   // no targets at all: the pixel shader writes only the UAV
    ctx.setViewport(0, 0, res, res);
    ctx.setScissor(0, 0, res, res);
    for (const Draw& d : drawsPrev_) {
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
        if (useMs) ctx.dispatchMeshFor(d.mesh);
        else       ctx.drawMesh(d.mesh);
    }

    // Reduce the atomic sums into the filterable RGBA16F volume.
    ctx.uavBarrierTexture(voxelAccumTex_);
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    ctx.setPipeline(resolvePso_);
    ctx.setBindingSet(resolveBindings_);
    ctx.dispatch(cg, cg, cg);
    ctx.popMarker();
}

// Box-filters each level of the radiance volume into the next, then hands the whole chain back as
// a shader resource.
void VoxiRenderer::filterMips(rhi::IRenderContext& ctx) {
    ctx.pushMarker("Voxi mip filter");
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
    ctx.popMarker();
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
    ctx.pushMarker("Voxi debug view");
    ctx.setPipeline(debugPso_);
    ctx.setBindingSet(bindings_);
    // Table 1 is bound outright: this pipeline declares it and Tier 1 populates whole tables.
    ctx.setBindingSet(materials_.fallbackBindingSet(), 1);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
    ctx.drawFullscreen();
    ctx.popMarker();
}

// Rebuilds the pipelines that bake the sample count and the target formats.
void VoxiRenderer::onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth) {
    if (!res_ || !giReady_) return;
    if (!createScenePipelines(sampleCount, color, depth))
        AVER_ERROR("[Voxi] scene pipelines could not be rebuilt for {} sample(s)", sampleCount);
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
    bd.srvCount = 6;
    bd.uavCount = 2;
    bd.srvKinds[0] = rhi::SlotKind::Texture3D;              // t0 volume, whole chain
    bd.srvKinds[1] = rhi::SlotKind::Texture2D;              // t1 shadow map
    bd.srvKinds[2] = rhi::SlotKind::AccelerationStructure;  // t2 TLAS, filled once one exists
    // Null-filled until the table exists. Tier 1 requires a valid descriptor of the right KIND in
    // every declared slot, so these must be declared as structured buffers even while empty.
    bd.srvKinds[3] = rhi::SlotKind::StructuredBuffer;       // t3 flat vertices
    bd.srvKinds[4] = rhi::SlotKind::StructuredBuffer;       // t4 flat indices
    bd.srvKinds[5] = rhi::SlotKind::StructuredBuffer;       // t5 per-instance records
    bd.uavKinds[0] = rhi::SlotKind::Texture3D;              // u0 volume mip 0
    bd.uavKinds[1] = rhi::SlotKind::Texture3D;              // u1 injection accumulator
    bindings_ = res_->createBindingSet(bd);
    if (!bindings_) { AVER_ERROR("[Voxi] main binding set could not be created"); return false; }
    res_->setSrv(bindings_, 0, voxelTex_, rhi::kAllMips);
    if (shadowTex_) res_->setSrv(bindings_, 1, shadowTex_);
    res_->setUav(bindings_, 0, voxelTex_, 0);
    res_->setUav(bindings_, 1, voxelAccumTex_, 0);

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
