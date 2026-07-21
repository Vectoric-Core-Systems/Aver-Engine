#include "aver/voxi/VoxiRenderer.hpp"
#include "aver/core/Log.hpp"
#include "aver/core/Math.hpp"   // light-frustum fit: Vec3 / Mat4::lookAtLH
#include "aver/pbr/PbrShaders.hpp"

#include "VoxiShaders.hpp"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>

// Voxi's GPU resources, expressed only in terms of the generic RHI. Nothing here names a backend
// type; everything is a handle from IResourceFactory.
//
// This step CREATES the resources and stops. prePass() returns immediately and the feature does not
// override the scene pipeline, so the backend keeps producing exactly the image it produced before.
// The evidence that the interface is sufficient is the "[Voxi] init:" line: every object Voxi needs
// has a non-zero handle, or the first one that does not is named.
namespace aver::voxi {

namespace {

// Core feature-level 11_0 sizing, so shadowed light injection works on every DX12 GPU rather than
// only where ray tracing does.
constexpr u32 kShadowSize = 2048;

// The draw-list cap, and therefore the instance count the TLAS is sized for. One owner: a TLAS
// sized for fewer instances than submit() will accept silently drops the overflow at build time.
constexpr u32 kMaxDraws = 4096;

// s0 samples the radiance volume, s1 is the shadow map's comparison sampler. Both are static
// samplers on the pipeline (the Tier-1-friendly choice), so every pipeline that declares the
// three-SRV table has to carry them.
void giSamplers(rhi::PipelineLayout& l) {
    l.samplers[0].filter  = rhi::Filter::Linear;
    l.samplers[0].address = rhi::AddressMode::Clamp;
    // Left unclamped on purpose: traceCone reads a fractional mip that grows with cone distance,
    // and any clamp here makes the tail of the chain unreachable — which reads as "GI fades out
    // too early", never as an error.
    l.samplers[0].maxLod  = FLT_MAX;
    l.samplers[1].filter  = rhi::Filter::ComparisonLinear;
    l.samplers[1].address = rhi::AddressMode::Clamp;
    l.samplers[1].compare = rhi::CompareOp::LessEqual;
    l.samplerCount = 2;
}

// The layout every Voxi raster pipeline declares. Identical shapes share one cached root signature,
// which is what lets the binding set stay live while the backend switches between them.
rhi::PipelineLayout giLayout() {
    rhi::PipelineLayout l{};
    l.srvCount = 3;              // t0 volume, t1 shadow map, t2 acceleration structure
    l.uavCount = 2;              // u0 volume mip 0, u1 injection accumulator
    l.constantDwords[1] = 24;    // b1: world (16) + base colour (4) + material (4)
    giSamplers(l);
    return l;
}

// Everything Voxi's HLSL is compiled on top of: the RHI's shared declarations, then the material
// system's BRDF and Aver* contract. That order is the architecture -- the material text uses the
// constant-buffer layouts and vertex structures the shared prelude declares, and Voxi's own source
// then calls the material.
//
// Joined ONCE into a static string because ShaderDesc::prelude is a BORROWED pointer read at
// pipeline creation, and onRenderTargetsChanged rebuilds every pipeline on each MSAA change. A
// temporary would dangle and, being freed heap that nothing has reused yet, would usually still
// compile - the worst kind of bug to go looking for.
const char* voxiShaderPrelude() {
    static const std::string s = std::string(rhi::sharedShaderPrelude()) + pbr::materialShaderPrelude();
    return s.c_str();
}

// Shader blobs are CPU-side only: a pipeline copies what it needs at creation. Collecting them
// means the whole batch is released on every exit path, including the early returns a failed
// rebuild takes.
struct ShaderScope {
    explicit ShaderScope(rhi::IResourceFactory& r) : res(r) {}
    ~ShaderScope() { for (rhi::ShaderHandle h : owned) res.destroyShader(h); }
    ShaderScope(const ShaderScope&) = delete;
    ShaderScope& operator=(const ShaderScope&) = delete;

    rhi::ShaderHandle operator()(const char* entry, rhi::ShaderStage stage, u32 sm,
                                 const char* defines) {
        rhi::ShaderDesc sd;
        sd.source  = kVoxiHLSL;
        // ONE owner for the shared cbuffer layouts and vertex structures, and ONE owner for the
        // BRDF. Copying either here would create a cross-module ABI with no compiler behind it.
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

bool VoxiRenderer::init(rhi::IDevice& device) {
    dev_ = &device;
    // A backend with no GPU support returns nullptr here. Declining is the contract: the engine
    // then runs without the feature instead of failing outright.
    res_ = device.resources();
    if (!res_) {
        AVER_WARN("[Voxi] init declined: backend exposes no resource factory (no GPU support)");
        return false;
    }
    caps_ = device.caps();

    createShadowResources();
    createVoxelVolume(settings_.voxelResolution);
    createPipelines();

    // The mesh-shader and ray-tracing variants are optional by design, so they are reported but
    // never required. Everything else is a hard requirement for the passes that follow.
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

    // One readable line naming every handle, so a primitive the interface cannot express shows up
    // as a zero here rather than as a black screen three steps from now.
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

    // The TLAS is created HERE rather than on the frame it is first needed, so its descriptor can
    // be written into t2 before a single frame has been recorded. A shader-visible descriptor that
    // an in-flight frame may still be reading must not be rewritten, and creating it up front is
    // the only way to guarantee that without tracking the fence by hand. It is sized for the
    // draw-list cap, so a per-frame rebuild never reallocates and the address never moves.
    if (rtSupported_) {
        tlas_ = res_->createTlas(kMaxDraws);
        if (tlas_) res_->setSrvTlas(bindings_, 2, tlas_);   // t2, per the shader's register(t2)
        else {
            // Non-fatal: everything else Voxi does works without ray tracing.
            AVER_WARN("[Voxi] TLAS could not be created; ray-traced sun shadows stay off");
            rtSupported_ = false;
        }
    }

    AVER_INFO("[Voxi] ready: conservative raster {}, mesh-shader variants {}, ray-tracing variants {}",
              caps_.conservativeRaster ? "on" : "off",
              (voxelMsPso_ && sceneMsPso_) ? "built" : "absent",
              rtSupported_ ? "built" : "absent");
    return true;
}

void VoxiRenderer::shutdown() {
    if (!res_) { dev_ = nullptr; return; }
    // Binding sets point at the textures, so they go first; destruction is deferred behind the
    // GPU fence by contract, which is why no explicit wait is needed here.
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

    // IResourceFactory exposes no destroyBlas/destroyTlas: acceleration structures are released
    // with the factory itself, so only the handles are dropped here.
    blas_.clear();
    tlas_ = 0;

    voxelMips_ = voxelResBuilt_ = 0;
    giReady_ = rtSupported_ = rtActive_ = rtLogged_ = false;
    draws_.clear();
    drawsPrev_.clear();
    res_ = nullptr;
    dev_ = nullptr;
}

// ---------------------------------------------------------------- configuration

void VoxiRenderer::setSettings(const Settings& s) { settings_ = s; }

void VoxiRenderer::setVolume(const f32 center[3], f32 extent) {
    center_[0] = center[0]; center_[1] = center[1]; center_[2] = center[2];
    extent_ = extent;
}

void VoxiRenderer::setSun(const f32 dirToLight[3], const f32 color[3], f32 ambient) {
    sunDir_[0] = dirToLight[0]; sunDir_[1] = dirToLight[1]; sunDir_[2] = dirToLight[2];
    sunColor_[0] = color[0]; sunColor_[1] = color[1]; sunColor_[2] = color[2];
    ambient_ = ambient;
}

void VoxiRenderer::setDebugView(bool on) { debugView_ = on; }

// ---------------------------------------------------------------- scene submission

// Voxi runs a frame behind: the passes replay the PREVIOUS frame's draw list, so the app does not
// have to submit geometry before the feature's own passes run.
void VoxiRenderer::beginScene() {
    drawsPrev_.swap(draws_);
    draws_.clear();
}

void VoxiRenderer::submit(rhi::MeshHandle mesh, const f32 world[16], const f32 baseColor[4],
                          f32 metallic, f32 roughness) {
    if (mesh == 0 || draws_.size() >= kMaxDraws) return;
    Draw d;
    d.mesh = mesh;
    std::memcpy(d.world, world, 16 * sizeof(f32));
    std::memcpy(d.color, baseColor, 4 * sizeof(f32));
    d.metallic = metallic;
    d.roughness = roughness;
    draws_.push_back(d);
}

// ---------------------------------------------------------------- feature hooks

void VoxiRenderer::prePass(rhi::IRenderContext& ctx) {
    if (!giReady_) return;
    // Volume placement is needed by the injection AND by the lit pass's cone trace, so it is
    // refreshed every frame regardless of which passes below actually run.
    const f32 size = extent_ * 2.0f;
    cb_.voxelOrigin[0] = center_[0] - extent_;
    cb_.voxelOrigin[1] = center_[1] - extent_;
    cb_.voxelOrigin[2] = center_[2] - extent_;
    cb_.voxelOrigin[3] = size > 0.0f ? 1.0f / size : 0.0f;   // the shader multiplies by this
    cb_.voxelParams[0] = static_cast<f32>(voxelResBuilt_);
    cb_.voxelParams[1] = settings_.giIntensity;
    cb_.voxelParams[2] = settings_.giMaxDistance;
    cb_.voxelParams[3] = (giEnabled() && !debugView_) ? 1.0f : 0.0f;   // gates the cone trace

    // First, because everything after it records draws and barriers: an acceleration-structure
    // build wants a clean command stream, and the flag it publishes into b4 must be settled before
    // the backend copies these constants after prePass returns.
    buildAccelerationStructures(ctx);
    shadowPass(ctx);
    if (giEnabled()) {
        voxelizePass(ctx);
        filterMips(ctx);
    }
}

// Bottom-level structures for every referenced mesh, then one top-level structure over the
// replayed draw list. Recomputed from scratch every frame — a scene that loses its geometry, or
// whose meshes have no buildable BLAS, must fall back to the shadow map rather than trace a
// structure nothing filled.
void VoxiRenderer::buildAccelerationStructures(rhi::IRenderContext& ctx) {
    rtActive_ = false;
    cb_.shadowParams[2] = 0.0f;   // the lit pass reads this as "may I trace?"
    if (!rtSupported_ || settings_.rayTracing == Quality::Off || drawsPrev_.empty()) return;

    ctx.pushMarker("Voxi acceleration structures");
    std::vector<rhi::TlasInstance> inst;
    inst.reserve(drawsPrev_.size());
    for (const Draw& d : drawsPrev_) {
        auto it = blas_.find(d.mesh);
        if (it == blas_.end()) {
            const rhi::BlasHandle nb = res_->createBlas(d.mesh);
            // The build must run on a command list, which is why it happens here rather than in
            // createBlas. The backend records the UAV barrier that publishes it.
            if (nb) ctx.buildBlas(nb);
            // A zero is recorded too: a mesh that cannot produce a BLAS (no indices, allocation
            // failure) must be remembered as such, or every frame retries it and re-logs the error.
            it = blas_.emplace(d.mesh, nb).first;
        }
        const rhi::BlasHandle b = it->second;
        if (!b) continue;
        rhi::TlasInstance i;
        // ENGINE convention, handed over untouched: the BACKEND owns the 3x4 column-vector
        // transpose DXR wants. Pre-transposing here leaves the raster image perfectly correct and
        // puts every ray somewhere else — a defect only a cast-shadow probe can see.
        std::memcpy(i.world, d.world, sizeof(i.world));
        i.mask = 0xFF;
        i.blas = b;
        inst.push_back(i);
    }
    if (inst.empty()) { ctx.popMarker(); return; }

    // buildTlas records the UAV barrier after the build. That barrier is the ONLY synchronisation
    // between this write and the RayQuery reads later in the same command list.
    ctx.buildTlas(tlas_, inst.data(), static_cast<u32>(inst.size()));
    rtActive_ = true;
    cb_.shadowParams[2] = 1.0f;
    if (!rtLogged_) {
        AVER_INFO("[Voxi] RayQuery active ({} instances, {} bottom-level structures)",
                  static_cast<u32>(inst.size()), static_cast<u32>(blas_.size()));
        rtLogged_ = true;
    }
    ctx.popMarker();
}

// Fit an orthographic light frustum around the volume, so the map's resolution is spent exactly
// where the GI samples it.
void VoxiRenderer::shadowPass(rhi::IRenderContext& ctx) {
    // No geometry means no shadow map: flag it disabled so the lit pass does not sample a stale one.
    if (!shadowPso_ || drawsPrev_.empty()) { cb_.shadowParams[1] = 0.0f; return; }

    const Vec3 centre{center_[0], center_[1], center_[2]};
    const f32 r = extent_ > 1.0f ? extent_ : 1.0f;
    Vec3 dir = Vec3{sunDir_[0], sunDir_[1], sunDir_[2]}.getSafeNormal();
    if (dir.sizeSquared() < 0.5f) dir = Vec3{0.3f, 0.4f, 0.85f}.getSafeNormal();
    const Vec3 eye = centre + dir * (r * 2.0f);              // sunDir_ points TOWARD the light
    const Vec3 up = std::fabs(dir.z) > 0.95f ? Vec3{1, 0, 0} : Vec3{0, 0, 1};
    const Mat4 view = Mat4::lookAtLH(eye, centre, up);
    Mat4 proj;                                                // orthographic, row-vector convention
    proj.m[0][0] = 1.0f / r; proj.m[1][1] = 1.0f / r;
    proj.m[2][2] = 1.0f / (r * 4.0f); proj.m[3][2] = 0.0f; proj.m[3][3] = 1.0f;
    const Mat4 lvp = view * proj;
    std::memcpy(cb_.lightViewProj, &lvp.m[0][0], sizeof(cb_.lightViewProj));
    cb_.shadowParams[0] = 1.0f / static_cast<f32>(kShadowSize);
    cb_.shadowParams[1] = 1.0f;

    ctx.pushMarker("Voxi shadow");
    ctx.textureBarrier(shadowTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::DepthWrite);
    ctx.setPipeline(shadowPso_);
    ctx.setBindingSet(bindings_);       // Tier 1: bind every declared table, read or not
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
    ctx.setRenderTargets(nullptr, 0, shadowTex_);
    ctx.clearDepth(shadowTex_, 1.0f);
    // Both, and not just the viewport: the scissor is independent, and the editor leaves one set to
    // its dock rect, which would silently clip the map.
    ctx.setViewport(0, 0, kShadowSize, kShadowSize);
    ctx.setScissor(0, 0, kShadowSize, kShadowSize);
    for (const Draw& d : drawsPrev_) {
        f32 consts[24]{};
        std::memcpy(consts, d.world, 16 * sizeof(f32));   // depth-only: colour/material unused
        ctx.setConstants(1, consts, 24);
        ctx.drawMesh(d.mesh);
    }
    ctx.textureBarrier(shadowTex_, rhi::ResourceState::DepthWrite, rhi::ResourceState::ShaderResource);
    ctx.popMarker();
}

// Clear mip 0, then rasterise the scene into the volume with direct lighting already applied.
void VoxiRenderer::voxelizePass(rhi::IRenderContext& ctx) {
    const u32 res = voxelResBuilt_;
    ctx.pushMarker("Voxi voxelise");

    // Injection only touches the voxels its triangles cover, so without a clear a voxel lit once
    // stays lit and moving geometry drags a radiance trail behind it. Only the accumulator needs
    // it: CSResolve below writes every cell of mip 0 unconditionally, and CSMip fully overwrites
    // every coarser level.
    ctx.setPipeline(clearPso_);
    ctx.setBindingSet(clearBindings_);
    const u32 cg = (res + 3) / 4;
    ctx.dispatch(cg, cg, cg);
    ctx.uavBarrierTexture(voxelAccumTex_);   // injection must see the cleared accumulator

    // Mesh shaders remove the geometry shader from voxelisation entirely, which is the point of the
    // variant: GS is emulated on every AMD GCN part.
    const bool useMs = settings_.meshShaders && voxelMsPso_;
    ctx.setPipeline(useMs ? voxelMsPso_ : voxelPso_);
    ctx.setBindingSet(bindings_);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
    ctx.setRenderTargets(nullptr, 0, 0);   // no targets at all: the pixel shader writes only the UAV
    ctx.setViewport(0, 0, res, res);
    ctx.setScissor(0, 0, res, res);
    // Deliberately NOT skipped when the list is empty: a scene emptied of geometry must end up with
    // a cleared volume rather than keeping the last frame's radiance for ever.
    for (const Draw& d : drawsPrev_) {
        f32 consts[24];
        std::memcpy(consts, d.world, 16 * sizeof(f32));
        std::memcpy(consts + 16, d.color, 4 * sizeof(f32));
        consts[20] = d.metallic; consts[21] = d.roughness; consts[22] = 0.0f; consts[23] = 0.0f;
        ctx.setConstants(1, consts, 24);
        if (useMs) ctx.dispatchMeshFor(d.mesh);
        else       ctx.drawMesh(d.mesh);
    }

    // Reduce the atomic sums into the filterable RGBA16F volume. Every fragment's contribution is
    // already in the accumulator by the time this runs, so the result depends on WHICH fragments
    // covered a voxel and not on the order the GPU happened to retire them in -- which is the whole
    // reason the accumulator exists.
    ctx.uavBarrierTexture(voxelAccumTex_);
    // The volume leaves ShaderResource HERE and not a line earlier. Nothing above writes it: the
    // clear zeroes the accumulator and injection only adds to it, so the resolve is the first pass
    // that needs it writable. Taking it early left the whole-chain SRV in the injection pass's own
    // binding set pointing at a resource in UnorderedAccess -- legal only because that descriptor
    // is never read, which is not a property worth relying on.
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::ShaderResource, rhi::ResourceState::UnorderedAccess);
    ctx.setPipeline(resolvePso_);
    ctx.setBindingSet(resolveBindings_);
    ctx.dispatch(cg, cg, cg);
    ctx.popMarker();
}

// Box-filter each level into the next. Averaging radiance AND occupancy is what lets one wide cone
// step read a single blurry sample instead of marching every voxel.
void VoxiRenderer::filterMips(rhi::IRenderContext& ctx) {
    ctx.pushMarker("Voxi mip filter");
    ctx.uavBarrierTexture(voxelTex_);
    ctx.setPipeline(mipPso_);
    const u32 res = voxelResBuilt_;
    for (u32 m = 1; m < voxelMips_; ++m) {
        // Level m-1 becomes readable while level m stays writable. Per-SUBRESOURCE transitions are
        // what make reading and writing one resource in a single dispatch legal.
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
    // The coarsest level is never a source, so the loop never demoted it. Reconciling it here is
    // what makes the whole-resource transition below legal -- and this is still correct when the
    // chain has one mip and the loop body never ran.
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::UnorderedAccess,
                       rhi::ResourceState::NonPixelShaderResource, voxelMips_ - 1);
    ctx.textureBarrier(voxelTex_, rhi::ResourceState::NonPixelShaderResource,
                       rhi::ResourceState::ShaderResource);
    ctx.popMarker();
}

bool VoxiRenderer::sceneConstants(const void** data, u32* bytes) const {
    if (!giReady_) return false;
    *data = &cb_; *bytes = sizeof(cb_);
    return true;
}

// Unconditional once the feature is up: shadowing and the cone-traced bounce are terms inside the
// lit pixel shader, so declining any frame would silently swap the whole scene to the backend's
// unshadowed shading rather than merely turning GI off.
bool VoxiRenderer::overridesScenePipeline() const { return giReady_; }

// The debug view is a GI diagnostic, so it needs GI actually running: with the volume switched off
// the raymarch would show whatever the last enabled frame left behind.
bool VoxiRenderer::suppressesScene() const { return giReady_ && giEnabled() && debugView_; }

// The colour target and the viewport are already bound by the backend; the raymarch owns nothing of
// its own. prePass has forced the cone-trace gate off (voxelParams.w), so the lit pass — which is
// not recorded at all this frame — could not also trace.
void VoxiRenderer::scenePass(rhi::IRenderContext& ctx) {
    if (!debugPso_) return;
    ctx.pushMarker("Voxi debug view");
    ctx.setPipeline(debugPso_);
    ctx.setBindingSet(bindings_);
    ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, &cb_, sizeof(cb_));
    ctx.drawFullscreen();
    ctx.popMarker();
}

void VoxiRenderer::onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth) {
    if (!res_ || !giReady_) return;
    if (!createScenePipelines(sampleCount, color, depth))
        AVER_ERROR("[Voxi] scene pipelines could not be rebuilt for {} sample(s)", sampleCount);
}

rhi::PipelineHandle VoxiRenderer::scenePipeline(bool meshShaders, bool wireframe) const {
    // Wireframe has no mesh-shader variant and Voxi builds no wireframe pipeline at all, so decline
    // and let the backend draw it: a wireframe view wants geometry, not shading.
    if (wireframe) return 0;
    // MS + RayQuery falls back to the IA RayQuery pipeline when that variant failed to build, which
    // is explicitly non-fatal.
    if (rtActive_) return meshShaders && sceneMsRtPso_ ? sceneMsRtPso_ : sceneRtPso_;
    return meshShaders && sceneMsPso_ ? sceneMsPso_ : scenePso_;
}

// ---------------------------------------------------------------- resources

bool VoxiRenderer::createShadowResources() {
    rhi::TextureDesc d;
    d.dim    = rhi::TextureDim::Tex2D;
    d.width  = kShadowSize;
    d.height = kShadowSize;
    // Typeless so the DSV can see D32Float while the SRV sees R32Float — one resource, two views.
    d.format = rhi::Format::R32Typeless;
    d.bind   = rhi::ResourceBind::ShaderResource | rhi::ResourceBind::DepthStencil;
    // Created readable, which is what makes frame 0's opening ShaderResource -> DepthWrite barrier
    // honest AND leaves a legally readable SRV on any frame whose shadow pass early-outs.
    d.initialState  = rhi::ResourceState::ShaderResource;
    // Without a matching clear value the target loses fast clear and warns every frame.
    d.hasClearValue = true;
    d.clearDepth    = 1.0f;
    d.debugName     = "Voxi shadow map";
    shadowTex_ = res_->createTexture(d);
    if (!shadowTex_) AVER_ERROR("[Voxi] shadow map {}^2 could not be created", kShadowSize);
    return shadowTex_ != 0;
}

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

    // The injection accumulator. Four uints per voxel (r, g, b, fragment count) interleaved along x,
    // because R32_UINT is the only typed format D3D12 guarantees UAV atomics on and a Tex3D cannot
    // carry four of them in one texel.
    rhi::TextureDesc ad;
    ad.dim    = rhi::TextureDim::Tex3D;
    ad.width  = resolution * 4;
    ad.height = resolution;
    ad.depth  = resolution;
    ad.mips   = 1;
    ad.format = rhi::Format::R32Uint;
    ad.bind   = rhi::ResourceBind::UnorderedAccess;
    ad.initialState = rhi::ResourceState::UnorderedAccess;   // where it stays: nothing ever reads it as an SRV
    ad.debugName    = "Voxi injection accumulator";
    voxelAccumTex_ = res_->createTexture(ad);
    if (!voxelAccumTex_) { AVER_ERROR("[Voxi] injection accumulator {}^3 could not be created", resolution); return false; }

    // The RESOLVED mip count, never a recomputed log2: it drives the descriptor loop, the
    // per-subresource barrier sequence and the dispatch loop, and an off-by-one there makes the
    // closing whole-resource transition illegal.
    rhi::TextureDesc got{};
    if (!res_->textureInfo(voxelTex_, got)) { AVER_ERROR("[Voxi] textureInfo failed for the radiance volume"); return false; }
    voxelMips_     = got.mips;
    voxelResBuilt_ = resolution;

    // Main table: everything the lit pass, the voxelisation pass and the shadow pass read. Each
    // slot declares its KIND because Tier 1 hardware null-fills by dimension, and a null descriptor
    // whose dimension disagrees with the shader is undefined behaviour on hardware nobody here owns.
    rhi::BindingSetDesc bd;
    bd.srvCount = 3;
    bd.uavCount = 2;
    bd.srvKinds[0] = rhi::SlotKind::Texture3D;              // t0 volume, whole chain
    bd.srvKinds[1] = rhi::SlotKind::Texture2D;              // t1 shadow map
    bd.srvKinds[2] = rhi::SlotKind::AccelerationStructure;  // t2 TLAS, filled once one exists
    bd.uavKinds[0] = rhi::SlotKind::Texture3D;              // u0 volume mip 0
    bd.uavKinds[1] = rhi::SlotKind::Texture3D;              // u1 injection accumulator
    bindings_ = res_->createBindingSet(bd);
    if (!bindings_) { AVER_ERROR("[Voxi] main binding set could not be created"); return false; }
    res_->setSrv(bindings_, 0, voxelTex_, rhi::kAllMips);
    if (shadowTex_) res_->setSrv(bindings_, 1, shadowTex_);
    res_->setUav(bindings_, 0, voxelTex_, 0);
    res_->setUav(bindings_, 1, voxelAccumTex_, 0);

    // The clear and the resolve get their own UAV-only sets and their own pipeline layout. While
    // they run, every mip of the volume is in UnorderedAccess, so no SRV descriptor over the volume
    // may be live.
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

    // One set per filter step. A SINGLE-MIP source view is what makes reading level m-1 while
    // writing level m legal: a whole-chain SRV would demand every level be readable at once.
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

bool VoxiRenderer::createPipelines() {
    // Optional geometry path, gated exactly as the backend gates its own: mesh shaders and RayQuery
    // share the same D3D12 Ultimate floor.
    const bool msOk = caps_.meshShaderTier > 0 && caps_.shaderModel >= 65 && caps_.dxcAvailable;

    ShaderScope compile(*res_);

    const rhi::PipelineLayout gi = giLayout();

    // --- 1. shadow map: depth only, from the sun ---
    if (const rhi::ShaderHandle vs = compile("VSShadow", rhi::ShaderStage::Vertex, 60, nullptr)) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vs;                                   // no pixel shader: depth is the only output
        p.layout = gi;
        p.cull = rhi::CullMode::None;
        p.depth = {true, true, rhi::CompareOp::Less};
        p.renderTargetCount = 0;
        p.depthFormat = rhi::Format::D32Float;
        p.sampleCount = 1;                           // the shadow map is never multisampled
        // Constant bias is meaningless against a float depth buffer, so the slope-scaled term
        // carries the whole job of separating a surface from its own shadow.
        p.slopeScaledDepthBias = 1.5f;
        shadowPso_ = res_->createGraphicsPipeline(p);
    }
    if (!shadowPso_) AVER_ERROR("[Voxi] shadow pipeline unavailable");

    // --- 2/3. voxelisation + light injection: rasterise with NO render target ---
    const rhi::ShaderHandle psVoxel = compile("PSVoxel", rhi::ShaderStage::Pixel, 60, nullptr);
    rhi::GraphicsPipelineDesc vox;
    vox.layout = gi;
    vox.cull = rhi::CullMode::None;
    vox.depthClip = false;                           // a triangle outside the slab still voxelises
    // Widens rasterisation so thin geometry still covers a voxel. Silently dropped where the
    // hardware cannot do it, so the cap is checked rather than assumed.
    vox.conservativeRaster = caps_.conservativeRaster;
    vox.renderTargetCount = 0;                       // the pixel shader's only output is the UAV
    vox.depthFormat = rhi::Format::Unknown;
    vox.sampleCount = 1;

    const rhi::ShaderHandle vsVoxel = compile("VSVoxel", rhi::ShaderStage::Vertex, 60, nullptr);
    const rhi::ShaderHandle gsVoxel = compile("GSVoxel", rhi::ShaderStage::Geometry, 60, nullptr);
    if (vsVoxel && gsVoxel && psVoxel) {
        rhi::GraphicsPipelineDesc p = vox;
        p.vs = vsVoxel; p.gs = gsVoxel; p.ps = psVoxel;
        voxelPso_ = res_->createGraphicsPipeline(p);
    }
    if (!voxelPso_) AVER_ERROR("[Voxi] voxelise pipeline unavailable");

    if (msOk && psVoxel) {
        // The mesh-shader variant exists to delete the geometry shader, which is emulated (and
        // slow) on every AMD GCN part.
        // The geometry registers are the LAYOUT's, not a constant: the backend puts them just past
        // whatever SRV table this pipeline declared, so they must be derived from the same layout
        // the pipeline is created with, or the mesh shader reads the wrong descriptor with nothing
        // to say so.
        const std::string msDefs = "AVER_MS=1;" + rhi::meshGeometryDefines(vox.layout);
        if (const rhi::ShaderHandle ms = compile("MSVoxel", rhi::ShaderStage::Mesh, 65, msDefs.c_str())) {
            rhi::GraphicsPipelineDesc p = vox;
            p.ms = ms; p.ps = psVoxel;
            voxelMsPso_ = res_->createGraphicsPipeline(p);
        }
        if (!voxelMsPso_) AVER_WARN("[Voxi] mesh-shader voxelise variant unavailable; the GS path stands in");
    }

    // --- 4. clear the accumulator, and reduce it into mip 0. UAV-only layouts, matching the
    //        UAV-only binding sets. ---
    if (const rhi::ShaderHandle cs = compile("CSClear", rhi::ShaderStage::Compute, 60, nullptr)) {
        rhi::ComputePipelineDesc p;
        p.cs = cs;
        p.layout.uavCount = 2;
        clearPso_ = res_->createComputePipeline(p);
    }
    if (!clearPso_) AVER_ERROR("[Voxi] volume clear pipeline unavailable");

    if (const rhi::ShaderHandle cs = compile("CSResolve", rhi::ShaderStage::Compute, 60, nullptr)) {
        rhi::ComputePipelineDesc p;
        p.cs = cs;
        p.layout.uavCount = 2;
        resolvePso_ = res_->createComputePipeline(p);
    }
    if (!resolvePso_) AVER_ERROR("[Voxi] injection resolve pipeline unavailable");

    // --- 5. mip filter: one source mip in, one destination mip out. ---
    if (const rhi::ShaderHandle cs = compile("CSMip", rhi::ShaderStage::Compute, 60, nullptr)) {
        rhi::ComputePipelineDesc p;
        p.cs = cs;
        p.layout.srvCount = 1;
        p.layout.uavCount = 1;
        p.layout.constantDwords[3] = 4;   // b3: source mip index (b0/b1 belong to the raster path)
        mipPso_ = res_->createComputePipeline(p);
    }
    if (!mipPso_) AVER_ERROR("[Voxi] mip filter pipeline unavailable");

    // --- 6-10. everything that bakes the sample count and the target formats, against whatever the
    // device is using right now. Same call the backend makes again whenever those change. ---
    const bool sceneOk = createScenePipelines(dev_->sampleCount(), dev_->backbufferFormat(),
                                              dev_->depthFormat());

    return shadowPso_ && voxelPso_ && clearPso_ && mipPso_ && sceneOk;
}

// Split out of createPipelines because these five bake the sample count and the render-target
// formats: MSAA is a runtime setting, and a pipeline whose SampleDesc disagrees with the bound
// target is rejected at DRAW time, a long way from anything that looks like its cause.
bool VoxiRenderer::createScenePipelines(u32 sampleCount, rhi::Format color, rhi::Format depth) {
    // RayQuery shares the D3D12 Ultimate floor with mesh shaders; both variants are optional and
    // their absence is reported, never fatal.
    const bool msOk = caps_.meshShaderTier > 0 && caps_.shaderModel >= 65 && caps_.dxcAvailable;
    const bool rtOk = caps_.rayTracingTier >= 11 && caps_.shaderModel >= 65 && caps_.dxcAvailable;

    // Destroyed before the replacements are built, not after: on a rebuild these handles are the
    // only reference to the old pipelines, and the factory defers the release behind the GPU fence
    // anyway, so there is nothing to be gained by keeping them alive across the creation.
    const rhi::PipelineHandle stale[] = {debugPso_, scenePso_, sceneMsPso_, sceneRtPso_, sceneMsRtPso_};
    for (rhi::PipelineHandle p : stale) if (p) res_->destroyPipeline(p);
    debugPso_ = scenePso_ = sceneMsPso_ = sceneRtPso_ = sceneMsRtPso_ = 0;

    ShaderScope compile(*res_);
    const rhi::PipelineLayout gi = giLayout();

    // --- 6. debug: raymarch the volume to screen (shares the prelude's fullscreen triangle). ---
    const rhi::ShaderHandle vsky = compile("VSky", rhi::ShaderStage::Vertex, 60, nullptr);
    if (const rhi::ShaderHandle ps = compile("PSVoxelDebug", rhi::ShaderStage::Pixel, 60, nullptr); ps && vsky) {
        rhi::GraphicsPipelineDesc p;
        p.vs = vsky; p.ps = ps;
        p.layout = gi;
        p.cull = rhi::CullMode::None;
        p.renderTargetCount = 1;
        p.renderTargets[0] = color;
        // No depth at all, which is why the debug view needs no depthFormat: it replaces the scene
        // rather than sharing a target with it.
        p.sampleCount = sampleCount;
        debugPso_ = res_->createGraphicsPipeline(p);
    }
    if (!debugPso_) AVER_ERROR("[Voxi] voxel debug pipeline unavailable");

    // --- 7-10. scene lit variants. The cone trace and RayQuery live inside the pixel shader, so
    // these are whole pipelines rather than an extra pass. ---
    rhi::GraphicsPipelineDesc scene;
    scene.layout = gi;
    scene.cull = rhi::CullMode::None;
    scene.depth = {true, true, rhi::CompareOp::Less};
    scene.renderTargetCount = 1;
    scene.renderTargets[0] = color;
    scene.depthFormat = depth;
    scene.sampleCount = sampleCount;

    const rhi::ShaderHandle vsMain = compile("VSMain", rhi::ShaderStage::Vertex, 60, nullptr);
    const rhi::ShaderHandle psVoxi = compile("PSMainVoxi", rhi::ShaderStage::Pixel, 60, nullptr);
    if (vsMain && psVoxi) {
        rhi::GraphicsPipelineDesc p = scene;
        p.vs = vsMain; p.ps = psVoxi;
        scenePso_ = res_->createGraphicsPipeline(p);
    }
    if (!scenePso_) AVER_ERROR("[Voxi] scene pipeline unavailable");

    // MSMain comes from the prelude and needs AVER_MS to exist at all, plus the geometry registers
    // for the layout these pipelines declare — see the MSVoxel site for why they cannot be literals.
    const std::string msDefs = "AVER_MS=1;" + rhi::meshGeometryDefines(scene.layout);
    const rhi::ShaderHandle msMain = msOk ? compile("MSMain", rhi::ShaderStage::Mesh, 65, msDefs.c_str()) : 0;
    if (msMain && psVoxi) {
        rhi::GraphicsPipelineDesc p = scene;
        p.ms = msMain; p.ps = psVoxi;
        sceneMsPso_ = res_->createGraphicsPipeline(p);
    }
    if (msOk && !sceneMsPso_) AVER_WARN("[Voxi] mesh-shader scene variant unavailable");

    // RayQuery replaces the shadow-map lookup with an exact occlusion ray; it is a second PS
    // compiled at SM 6.5, so a device without DXR still gets the shadow-mapped variant.
    const rhi::ShaderHandle psRt = rtOk ? compile("PSMainVoxi", rhi::ShaderStage::Pixel, 65, "AVER_RT=1") : 0;
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

    // Only the two mandatory ones: the mesh-shader and RayQuery variants are optional by design and
    // scenePipeline() falls back when they are absent.
    return debugPso_ && scenePso_;
}

} // namespace aver::voxi
