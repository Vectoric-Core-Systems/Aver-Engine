// ParticleRenderer implementation: the shaders and pipelines, the CPU tier's uploads, the cross-emitter order, the per-draw
// constants and the draws (instanced from the CPU, indirect from the GPU tier). See ParticleRenderer.hpp.
#include "aver/particles/ParticleRenderer.hpp"
#include "ParticleShaders.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"
#include "aver/scene/LightGather.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aver::particles {
namespace {

// The per-draw block (b3). Mirrors cbuffer ParticleDraw in particles.hlsl field for field.
struct ParticleDrawCB {
    f32 viewProj[16];     // this pass's view-projection (row-major, as rhi::PerFrameCB uploads it)
    f32 eye[4];
    f32 camRight[4];      // xyz camera right, w alignment (0 camera, 1 velocity, 2 axis)
    f32 camUp[4];         // xyz camera up, w velocity stretch
    f32 axis[4];          // xyz alignment axis (unit), w 1 when the instance list is an order list
    f32 size[4];          // x sizeStart, y sizeEnd, z size curve stops, w mesh scale
    f32 sizeT[4];
    f32 sizeV[4];
    f32 colA[4];
    f32 colB[4];
    f32 colT[4];
    f32 colV[4][4];
    f32 subuv[4];         // cols, rows, frames, fps
    f32 flags[4];         // has texture, SubUV blend, fog, lighting
    f32 trail[4];         // ring points, fade, linked capacity
    f32 misc[4];          // additive, mesh orientation, colour curve stops, kind
    ParticleLight lights[kParticleLightCount];
};
static_assert(sizeof(ParticleDrawCB) == 22 * 16 + kParticleLightCount * sizeof(ParticleLight), "ParticleDrawCB drifted from particles.hlsl");
static_assert(sizeof(ParticleDrawCB) % 16 == 0, "a constant block is whole float4 rows");

// Applies the row-vector camera-relative inverse view-projection (rhi::PerFrameCB::invViewProjRel) to an NDC point and
// dehomogenises it. The result is an offset FROM THE EYE.
Vec3 unprojectPoint(const f32 ivpRel[16], f32 ndcX, f32 ndcY, f32 ndcZ) {
    const f32 v[4] = {ndcX, ndcY, ndcZ, 1.0f};
    f32 r[4] = {0, 0, 0, 0};
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i)
            r[j] += v[i] * ivpRel[i * 4 + j];
    const f32 invW = r[3] != 0.0f ? 1.0f / r[3] : 1.0f;
    return {r[0] * invW, r[1] * invW, r[2] * invW};
}

// The camera's world RIGHT and UP axes from invViewProjRel alone. Exact, not a first-order approximation: with the NDC depth
// held fixed, view-space Z is fixed, so NDC x is linear in view-space X and a finite step recovers a vector exactly parallel to
// the camera's right axis (see the 0.7 derivation, kept in git history). Camera-relative, so the points sit a few
// centimetres from the eye however far the camera is from the origin.
void cameraBasis(const f32 invViewProjRel[16], Vec3& outRight, Vec3& outUp) {
    const Vec3 center = unprojectPoint(invViewProjRel, 0.0f, 0.0f, 0.5f);
    const Vec3 rightP = unprojectPoint(invViewProjRel, 0.5f, 0.0f, 0.5f);
    const Vec3 upP    = unprojectPoint(invViewProjRel, 0.0f, 0.5f, 0.5f);
    outRight = (rightP - center).getSafeNormal();
    outUp    = (upP - center).getSafeNormal();
}

void setVec4(f32 out[4], f32 x, f32 y, f32 z, f32 w) { out[0] = x; out[1] = y; out[2] = z; out[3] = w; }

constexpr u32 alignUp(u32 v, u32 a) { return (v + a - 1u) / a * a; }

bool sortWanted(const ParticleEffect& fx) {
    if (fx.render == RenderKind::Ribbon && fx.linked) return false;
    return fx.sort == SortMode::Depth || (fx.sort == SortMode::Auto && fx.blend != rhi::BlendMode::Additive);
}

} // namespace

// ------------------------------------------------------------------------------------------------ frustum
bool ParticleRenderer::sphereInFrustum(const f32 vp[16], const Vec3& c, f32 radius) {
    // Row-vector clip = p * M, so a plane is a combination of M's COLUMNS: column j is (M[0][j], M[1][j], M[2][j], M[3][j]).
    const auto col = [&](int j, f32 out[4]) { for (int i = 0; i < 4; ++i) out[i] = vp[i * 4 + j]; };
    f32 c0[4], c1[4], c2[4], c3[4];
    col(0, c0); col(1, c1); col(2, c2); col(3, c3);
    f32 planes[6][4];
    for (int k = 0; k < 4; ++k) {
        planes[0][k] = c3[k] + c0[k];   // left
        planes[1][k] = c3[k] - c0[k];   // right
        planes[2][k] = c3[k] + c1[k];   // bottom
        planes[3][k] = c3[k] - c1[k];   // top
        planes[4][k] = c2[k];           // near (depth 0..1)
        planes[5][k] = c3[k] - c2[k];   // far
    }
    for (const auto& p : planes) {
        const f32 len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (len < 1e-8f) continue;
        if (p[0] * c.x + p[1] * c.y + p[2] * c.z + p[3] < -radius * len) return false;
    }
    return true;
}

// ------------------------------------------------------------------------------------------------ lifecycle
bool ParticleRenderer::init(rhi::IDevice& device) {
    shutdown();
    dev_ = &device;
    res_ = device.resources();
    if (!res_) {
        AVER_WARN("[Particles] init declined: backend exposes no resource factory");
        dev_ = nullptr;
        return false;
    }
    if (!device.caps().indirect) {
        AVER_WARN("[Particles] init declined: the backend reports no instanced / indirect draw support");
        res_ = nullptr;
        dev_ = nullptr;
        return false;
    }
    meshes_.init(res_);

    // The unit quad every sprite and ribbon instance expands.
    {
        const f32 corners[8] = {-1, -1, 1, -1, 1, 1, -1, 1};
        const u32 idx[6] = {0, 1, 2, 0, 2, 3};
        rhi::BufferDesc vb;
        vb.bytes = sizeof corners;
        vb.kind = rhi::BufferKind::Upload;
        vb.debugName = "Aver.Particles quad vb";
        quadVb_ = res_->createBuffer(vb);
        rhi::BufferDesc ib;
        ib.bytes = sizeof idx;
        ib.kind = rhi::BufferKind::Upload;
        ib.debugName = "Aver.Particles quad ib";
        quadIb_ = res_->createBuffer(ib);
        rhi::BufferDesc db;
        db.bytes = 4096;
        db.kind = rhi::BufferKind::Upload;
        db.debugName = "Aver.Particles dummy";
        dummyBuf_ = res_->createBuffer(db);
        if (!quadVb_ || !quadIb_ || !dummyBuf_ || !res_->writeBuffer(quadVb_, corners, sizeof corners) ||
            !res_->writeBuffer(quadIb_, idx, sizeof idx)) {
            AVER_ERROR("[Particles] the quad template could not be created");
            shutdown();
            return false;
        }
        std::vector<u8> zero(4096, 0);
        res_->writeBuffer(dummyBuf_, zero.data(), zero.size());
    }
    // Valid descriptors for the slots a draw does not use (D3D12 tier 1 needs one in every declared slot).
    {
        const u8 white[4] = {255, 255, 255, 255};
        const void* data[1] = {white};
        rhi::TextureDesc td;
        td.format = rhi::Format::RGBA8Unorm;
        td.bind = rhi::ResourceBind::ShaderResource;
        td.initialState = rhi::ResourceState::ShaderResource;
        td.initialData = data;
        td.initialDataCount = 1;
        td.debugName = "Aver.Particles white";
        whiteTex_ = res_->createTexture(td);
        td.debugName = "Aver.Particles dummy 2d";
        dummyTex2d_ = res_->createTexture(td);
        td.dim = rhi::TextureDim::Tex3D;
        td.depth = 1;
        td.debugName = "Aver.Particles dummy 3d";
        dummyTex3d_ = res_->createTexture(td);
        if (!whiteTex_ || !dummyTex2d_ || !dummyTex3d_) {
            AVER_ERROR("[Particles] the dummy textures could not be created");
            shutdown();
            return false;
        }
    }
    if (!buildShaders()) {
        shutdown();
        return false;
    }
    if (gpuEnabled_) gpu_.init(device);
    pipelineSampleCount_ = device.sampleCount();
    pipelineColor_ = device.backbufferFormat();
    pipelineDepth_ = device.depthFormat();
    buildGiShader();
    buildPipelines(pipelineSampleCount_, pipelineColor_, pipelineDepth_);
    installGpuTier();
    AVER_INFO("[Particles] ready ({} tier)", gpu_.usable() ? "CPU + GPU" : "CPU");
    return true;
}

void ParticleRenderer::shutdown() {
    if (system_) system_->setGpuTier(nullptr);
    gpu_.shutdown();
    if (res_) {
        destroyPipelines();
        for (rhi::ShaderHandle& v : vs_) { if (v) res_->destroyShader(v); v = 0; }
        if (ps_) res_->destroyShader(ps_);
        if (giPs_) res_->destroyShader(giPs_);
        ps_ = giPs_ = 0;
        for (u32 f = 0; f < kFramesInFlight; ++f) {
            for (rhi::BindingSetHandle s : sets_[f]) res_->destroyBindingSet(s);
            sets_[f].clear();
            for (rhi::BufferHandle* b : {&particleRing_[f], &orderRing_[f], &trailRing_[f]}) {
                if (*b) res_->destroyBuffer(*b);
                *b = 0;
            }
        }
        for (rhi::BufferHandle* b : {&quadVb_, &quadIb_, &dummyBuf_}) {
            if (*b) res_->destroyBuffer(*b);
            *b = 0;
        }
        for (rhi::TextureHandle* t : {&whiteTex_, &dummyTex2d_, &dummyTex3d_}) {
            if (*t) res_->destroyTexture(*t);
            *t = 0;
        }
    }
    meshes_.shutdown();
    particleCap_ = orderCap_ = trailCap_ = 0;
    setCursor_ = 0;
    res_ = nullptr;
    dev_ = nullptr;
}

void ParticleRenderer::installGpuTier() {
    if (system_) system_->setGpuTier(gpu_.usable() ? &gpu_ : nullptr);
}

void ParticleRenderer::setSystem(ParticleSystem* system) {
    if (system_ && system_ != system) system_->setGpuTier(nullptr);
    system_ = system;
    installGpuTier();
}

void ParticleRenderer::setGpuEnabled(bool enabled) {
    gpuEnabled_ = enabled;
    if (!dev_) return;
    if (!enabled && gpu_.usable()) {
        if (system_) system_->setGpuTier(nullptr);
        gpu_.shutdown();
    } else if (enabled && !gpu_.usable()) {
        gpu_.init(*dev_);
        installGpuTier();
    }
}

// ------------------------------------------------------------------------------------------------ shaders and pipelines
bool ParticleRenderer::buildShaders() {
    const std::string src = rhi::shaderFile("particles.hlsl");
    if (src.empty()) {
        AVER_ERROR("[Particles] particles.hlsl was not found");
        return false;
    }
    rhi::ShaderDesc sd;
    sd.source = src.c_str();
    sd.prelude = rhi::sharedShaderPrelude();   // gViewProj, gCamPos, the fog and sky helpers
    sd.stage = rhi::ShaderStage::Vertex;
    const char* entries[3] = {"ParticleSpriteVS", "ParticleRibbonVS", "ParticleMeshVS"};
    for (int i = 0; i < 3; ++i) {
        sd.entry = entries[i];
        vs_[i] = res_->createShader(sd);
    }
    sd.entry = "ParticlePS";
    sd.stage = rhi::ShaderStage::Pixel;
    ps_ = res_->createShader(sd);
    if (!vs_[0] || !vs_[1] || !vs_[2] || !ps_) {
        AVER_ERROR("[Particles] the particle shaders failed to compile");
        return false;
    }
    return true;
}

bool ParticleRenderer::buildGiShader() {
    if (giPs_) { res_->destroyShader(giPs_); giPs_ = 0; }
    if (!giSeam_.prepare || !res_) return false;
    std::string giPrelude, giDefines;
    if (!giSeam_.prepare(kGiSrvBase, kGiSamplerBase, rhi::kFeatureFrameConstantRegister, &giPrelude, &giDefines, giSeam_.user))
        return false;   // "nothing available right now" is not an error: every particle draws without the GI term until a later rebuild
    const std::string src = rhi::shaderFile("particles.hlsl");
    const std::string prelude = std::string(rhi::sharedShaderPrelude()) + giPrelude;
    const std::string defines = "AVER_PARTICLES_GI=1;" + giDefines;
    rhi::ShaderDesc sd;
    sd.source = src.c_str();
    sd.prelude = prelude.c_str();
    sd.defines = defines.c_str();
    sd.entry = "ParticlePS";
    sd.stage = rhi::ShaderStage::Pixel;
    giPs_ = res_->createShader(sd);
    if (!giPs_) AVER_WARN("[Particles] the GI pixel shader failed to build; lit emitters draw with the unshadowed sun and the sky");
    else AVER_INFO("[Particles] GI sampling pixel shader ready");
    return giPs_ != 0;
}

void ParticleRenderer::setGiSeam(const GiSeam& seam) {
    giSeam_ = seam;
    if (!dev_) return;
    destroyPipelines();
    buildGiShader();
    buildPipelines(pipelineSampleCount_, pipelineColor_, pipelineDepth_);
}

void ParticleRenderer::destroyPipelines() {
    if (res_)
        for (auto& kv : pipelines_)
            if (kv.second) res_->destroyPipeline(kv.second);
    pipelines_.clear();
}

rhi::PipelineHandle ParticleRenderer::makePipeline(RenderKind kind, bool additive, bool gi) {
    rhi::GraphicsPipelineDesc gd;
    gd.vs = vs_[static_cast<int>(kind)];
    gd.ps = gi ? giPs_ : ps_;
    if (kind == RenderKind::Mesh) {
        // position(3) normal(3) uv(2), split across 2D + 1D formats: the RHI has no three-component float vertex format.
        gd.vertexLayout.stride = 32;
        gd.vertexLayout.attribCount = 5;
        gd.vertexLayout.attribs[0] = {rhi::VertexSemantic::Position, 0, rhi::Format::RG32Float, 0};
        gd.vertexLayout.attribs[1] = {rhi::VertexSemantic::Position, 1, rhi::Format::R32Float, 8};
        gd.vertexLayout.attribs[2] = {rhi::VertexSemantic::Normal, 0, rhi::Format::RG32Float, 12};
        gd.vertexLayout.attribs[3] = {rhi::VertexSemantic::Normal, 1, rhi::Format::R32Float, 20};
        gd.vertexLayout.attribs[4] = {rhi::VertexSemantic::TexCoord, 0, rhi::Format::RG32Float, 24};
    } else {
        gd.vertexLayout.stride = 8;
        gd.vertexLayout.attribCount = 1;
        gd.vertexLayout.attribs[0] = {rhi::VertexSemantic::Position, 0, rhi::Format::RG32Float, 0};
    }
    // ONE table-0 shape for every pipeline: t0 particles, t1 order, t2 trail points, t3 texture, t4 / t5 the GI pair.
    gd.layout.srvCount = kDrawSrvCount;
    gd.layout.slotKindsDeclared = true;
    gd.layout.srvKinds[0] = gd.layout.srvKinds[1] = gd.layout.srvKinds[2] = rhi::SlotKind::StructuredBuffer;
    gd.layout.srvKinds[3] = rhi::SlotKind::Texture2D;
    gd.layout.srvKinds[kGiSrvBase] = rhi::SlotKind::Texture3D;       // the GI volume
    gd.layout.srvKinds[kGiSrvBase + 1] = rhi::SlotKind::Texture2D;   // the shadow map
    gd.layout.samplerCount = kGiSamplerBase + 2;
    gd.layout.samplers[0].filter = rhi::Filter::Linear;
    gd.layout.samplers[0].address = rhi::AddressMode::Clamp;
    gd.layout.samplers[kGiSamplerBase + 0].filter = rhi::Filter::Linear;
    gd.layout.samplers[kGiSamplerBase + 0].address = rhi::AddressMode::Clamp;
    gd.layout.samplers[kGiSamplerBase + 1].filter = rhi::Filter::ComparisonLinear;
    gd.layout.samplers[kGiSamplerBase + 1].address = rhi::AddressMode::Clamp;
    gd.layout.samplers[kGiSamplerBase + 1].compare = rhi::CompareOp::LessEqual;
    gd.layout.constantDwords[kSimConstantRegister] = 0;                    // b3: the per-draw block, a root CBV
    gd.layout.constantDwords[rhi::kFeatureFrameConstantRegister] = 0;     // b4: the GI frame constants, a root CBV

    // DEPTH-TESTED, DEPTH-WRITE OFF: DECIDED 1's whole point, and the exact contract IRenderFeature::transparentPass documents.
    gd.depth.test = true;
    gd.depth.write = false;
    gd.cull = rhi::CullMode::None;
    gd.renderTargetCount = 1;
    gd.renderTargets[0] = pipelineColor_;
    gd.depthFormat = pipelineDepth_;
    // MSAA IS BAKED INTO THE PIPELINE AT CREATION: the scene target's sample count, rebuilt whenever it changes.
    gd.sampleCount = pipelineSampleCount_;
    gd.blend = additive ? rhi::BlendMode::Additive : rhi::BlendMode::PremultipliedAlpha;
    return res_->createGraphicsPipeline(gd);
}

rhi::PipelineHandle ParticleRenderer::pipelineFor(RenderKind kind, bool additive, bool wantGi) {
    const bool gi = wantGi && giPs_ != 0;
    const u32 key = (static_cast<u32>(kind) << 4) | (additive ? 2u : 0u) | (gi ? 1u : 0u);
    auto it = pipelines_.find(key);
    if (it != pipelines_.end()) return it->second;
    const rhi::PipelineHandle p = makePipeline(kind, additive, gi);
    if (!p) AVER_ERROR("[Particles] pipeline build failed (kind {}, {}, {})", static_cast<int>(kind), additive ? "additive" : "premultiplied", gi ? "GI" : "plain");
    else if (kind != RenderKind::Sprite || gi) AVER_INFO("[Particles] pipeline built (kind {}, {}, {})", static_cast<int>(kind), additive ? "additive" : "premultiplied", gi ? "GI" : "plain");
    pipelines_[key] = p;
    return p;
}

bool ParticleRenderer::buildPipelines(u32 sampleCount, rhi::Format color, rhi::Format depth) {
    pipelineSampleCount_ = sampleCount;
    pipelineColor_ = color;
    pipelineDepth_ = depth;
    if (!res_ || !vs_[0] || !ps_) return false;
    // The common ones eagerly; ribbon, mesh and GI variants on first use.
    bool ok = pipelineFor(RenderKind::Sprite, false, false) != 0;
    ok = pipelineFor(RenderKind::Sprite, true, false) != 0 && ok;
    if (giPs_) {
        pipelineFor(RenderKind::Sprite, false, true);
        pipelineFor(RenderKind::Sprite, true, true);
    }
    if (ok) AVER_INFO("[Particles] pipelines (re)built: {}x MSAA", sampleCount);
    return ok;
}

void ParticleRenderer::onRenderTargetsChanged(u32 sampleCount, rhi::Format color, rhi::Format depth, u32 width, u32 height) {
    (void)width; (void)height;
    if (sampleCount == pipelineSampleCount_ && color == pipelineColor_ && depth == pipelineDepth_ && !pipelines_.empty()) return;
    destroyPipelines();
    buildPipelines(sampleCount, color, depth);
}

// ------------------------------------------------------------------------------------------------ rings and sets
bool ParticleRenderer::ensureRings(usize particles, usize orders, usize trails) {
    const auto grow = [&](rhi::BufferHandle (&ring)[kFramesInFlight], usize& cap, usize need, usize stride, const char* name) {
        if (need <= cap && ring[0]) return true;
        const usize newCap = std::max<usize>({need, cap * 2, 4096});
        for (u32 f = 0; f < kFramesInFlight; ++f) {
            if (ring[f]) res_->destroyBuffer(ring[f]);
            rhi::BufferDesc bd;
            bd.bytes = static_cast<u64>(newCap) * stride;
            bd.kind = rhi::BufferKind::Upload;
            bd.debugName = name;
            ring[f] = res_->createBuffer(bd);
            if (!ring[f]) { AVER_ERROR("[Particles] {} allocation failed", name); return false; }
        }
        cap = newCap;
        return true;
    };
    return grow(particleRing_, particleCap_, particles, sizeof(Particle), "Aver.Particles particle ring") &&
           grow(orderRing_, orderCap_, orders, sizeof(u32), "Aver.Particles order ring") &&
           grow(trailRing_, trailCap_, trails, 16, "Aver.Particles trail ring");
}

rhi::BindingSetHandle ParticleRenderer::acquireSet() {
    std::vector<rhi::BindingSetHandle>& pool = sets_[frame_];
    if (setCursor_ < pool.size()) return pool[setCursor_++];
    rhi::BindingSetDesc bd;
    bd.srvCount = kDrawSrvCount;
    bd.srvKinds[0] = bd.srvKinds[1] = bd.srvKinds[2] = rhi::SlotKind::StructuredBuffer;
    bd.srvKinds[3] = rhi::SlotKind::Texture2D;
    bd.srvKinds[kGiSrvBase] = rhi::SlotKind::Texture3D;
    bd.srvKinds[kGiSrvBase + 1] = rhi::SlotKind::Texture2D;
    const rhi::BindingSetHandle s = res_->createBindingSet(bd);
    if (!s) return 0;
    res_->setSrv(s, 3, whiteTex_);
    res_->setSrv(s, kGiSrvBase, dummyTex3d_);
    res_->setSrv(s, kGiSrvBase + 1, dummyTex2d_);
    pool.push_back(s);
    ++setCursor_;
    return s;
}

// ------------------------------------------------------------------------------------------------ prePass
void ParticleRenderer::prePass(rhi::IRenderContext& ctx) {
    if (!system_ || !dev_ || !gpu_.usable()) return;
    // A floating-origin shift since the last frame moves the resident GPU state before anything simulates (LARGE_WORLD 2.2.6).
    gpu_.recordShifts(ctx, system_->gpuShifts());
    std::vector<GpuJob>& jobs = system_->gpuJobs();
    if (jobs.empty()) return;

    ParticleGpu::View view;
    f32 vp[16] = {}, ivp[16] = {}, cp[3] = {};
    if (dev_->camera(vp, ivp, cp)) {
        std::memcpy(view.viewProj, vp, sizeof vp);
        view.camPos[0] = cp[0]; view.camPos[1] = cp[1]; view.camPos[2] = cp[2];
        view.valid = true;
    }
    const Vec3 camPos{cp[0], cp[1], cp[2]};
    for (GpuJob& j : jobs) {
        j.visible = true;
        if (!view.valid) continue;
        const Vec3 c{j.center[0], j.center[1], j.center[2]};
        if (j.cullDistance > 0.0f && dist(c, camPos) > j.cullDistance) j.visible = false;
        else if (j.boundsRadius > 0.0f && !sphereInFrustum(vp, c, j.boundsRadius)) j.visible = false;
    }
    system_->forEachInstance([&](const InstanceView& v) {
        if (v.gpuHandle == 0 || !v.effect || v.effect->render != RenderKind::Mesh) return;
        if (const ParticleMeshCache::Entry* e = meshes_.get(v.effect->meshId, meshFn_, meshUser_)) gpu_.setIndexCount(v.gpuHandle, e->indexCount);
    });
    gpu_.record(ctx, jobs, view);
    jobs.clear();
}

// ------------------------------------------------------------------------------------------------ transparentPass
void ParticleRenderer::transparentPass(rhi::IRenderContext& ctx) {
    if (!system_ || !res_ || !dev_) return;
    Camera cam;
    if (!dev_->camera(cam.viewProj, cam.invViewProjRel, cam.pos)) return;
    views_.clear();
    bool anyLit = false;
    system_->forEachInstance([&](const InstanceView& v) {
        views_.push_back(v);
        anyLit = anyLit || (v.effect && effectiveLighting(*v.effect) == Lighting::Lit);
    });
    if (views_.empty()) return;
    const std::vector<scene::WorldLight>* lights = nullptr;
    if (anyLit) {
        lightScratch_.clear();
        scene::gatherLights(scene::World::instance(), lightScratch_);
        lights = &lightScratch_;
    }
    drawViews(ctx, views_, cam, lights);
}

// ------------------------------------------------------------------------------------------------ drawViews
void ParticleRenderer::drawViews(rhi::IRenderContext& ctx, const std::vector<InstanceView>& views, const Camera& cam,
                                 const std::vector<scene::WorldLight>* lights) {
    drawStats_ = DrawStats{};
    if (!res_ || views.empty()) return;

    Vec3 right, up;
    cameraBasis(cam.invViewProjRel, right, up);
    if (right.sizeSquared() < 0.5f || up.sizeSquared() < 0.5f) return;   // a degenerate camera this frame
    const Vec3 camPos{cam.pos[0], cam.pos[1], cam.pos[2]};
    const f32 viewRow[4] = {cam.viewProj[3], cam.viewProj[7], cam.viewProj[11], cam.viewProj[15]};

    struct Item {
        const InstanceView* v;
        f32 dist;
        ParticleGpu::DrawBuffers gb;
        bool gpu;
        u32 n;            // CPU particles to upload
        u32 ring;         // CPU trail points per particle
        bool sorted;
    };
    std::vector<Item> items;
    items.reserve(views.size());
    usize needParticles = 0, needOrders = 0, needTrails = 0;
    for (const InstanceView& v : views) {
        const ParticleEffect* fx = v.effect;
        if (!fx) continue;
        Item it{};
        it.v = &v;
        if (v.gpuHandle != 0) {
            if (!gpu_.drawBuffers(v.gpuHandle, it.gb)) continue;
            it.gpu = true;
        } else {
            if (!v.particles || v.particles->empty()) continue;
            if (v.linked && v.aliveHint == 0) continue;
            it.n = static_cast<u32>(v.particles->size());
            it.ring = (fx->render == RenderKind::Ribbon && !v.linked && v.trail) ? v.trailRing : 0u;
            it.sorted = sortWanted(*fx) && it.n > 1;
            needParticles += alignUp(it.n, 4u);
            if (it.sorted) needOrders += alignUp(it.n, 64u);
            if (it.ring) needTrails += alignUp(it.n * it.ring, 16u);
        }
        const f32 d = dist(v.center, camPos);
        if (fx->cullDistanceCm > 0.0f && d > fx->cullDistanceCm) { ++drawStats_.culled; continue; }
        if (fx->boundsRadiusCm > 0.0f && !sphereInFrustum(cam.viewProj, v.center, fx->boundsRadiusCm)) { ++drawStats_.culled; continue; }
        it.dist = d;
        items.push_back(it);
    }
    if (items.empty()) return;
    // Across emitters: back to front by the bounds centre, ties by authored order (stable). Particles of different emitters are
    // never interleaved.
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.dist > b.dist; });

    frame_ = (frame_ + 1) % kFramesInFlight;
    setCursor_ = 0;
    if (!ensureRings(needParticles + 4, needOrders + 64, needTrails + 16)) return;
    usize particleCursor = 0, orderCursor = 0, trailCursor = 0;

    ctx.pushMarker("Aver.Particles");
    rhi::PipelineHandle bound = 0;
    for (Item& it : items) {
        const InstanceView& v = *it.v;
        const ParticleEffect& fx = *v.effect;
        const bool linked = v.linked;

        // ---- the mesh, the texture, the pipeline ----
        const ParticleMeshCache::Entry* mesh = nullptr;
        if (fx.render == RenderKind::Mesh) {
            mesh = meshes_.get(fx.meshId, meshFn_, meshUser_);
            if (!mesh) continue;
        }
        const rhi::TextureHandle tex = (fx.textureId != 0 && textureFn_) ? textureFn_(fx.textureId, textureUser_) : 0;
        const Lighting lighting = effectiveLighting(fx);
        const bool additive = fx.blend == rhi::BlendMode::Additive;
        const rhi::PipelineHandle pso = pipelineFor(fx.render, additive, lighting != Lighting::Unlit);
        if (!pso) continue;
        const bool giPipeline = lighting != Lighting::Unlit && giPs_ != 0;

        // ---- buffers ----
        rhi::BindingSetHandle set = acquireSet();
        if (!set) continue;
        u32 instanceCount = 0;
        bool useOrder = false;
        if (it.gpu) {
            res_->setSrvBuffer(set, 0, it.gb.particles, sizeof(Particle), it.gb.particleCount, it.gb.particleFirst);
            res_->setSrvBuffer(set, 1, it.gb.order, 4, it.gb.orderCount, it.gb.orderFirst);
            res_->setSrvBuffer(set, 2, it.gb.trail, 16, it.gb.trailCount, it.gb.trailFirst);
            useOrder = !it.gb.linked;
        } else {
            const std::vector<Particle>& ps = *v.particles;
            particleCursor = alignUp(static_cast<u32>(particleCursor), 4u);
            const u32 pFirst = static_cast<u32>(particleCursor);
            if (!res_->writeBuffer(particleRing_[frame_], ps.data(), static_cast<u64>(it.n) * sizeof(Particle), static_cast<u64>(pFirst) * sizeof(Particle))) continue;
            particleCursor += it.n;
            res_->setSrvBuffer(set, 0, particleRing_[frame_], sizeof(Particle), it.n, pFirst);

            if (it.sorted) {
                orderScratch_.resize(it.n);
                depthScratch_.resize(it.n);
                for (u32 i = 0; i < it.n; ++i) {
                    const Vec3& p = ps[i].position;
                    depthScratch_[i] = p.x * viewRow[0] + p.y * viewRow[1] + p.z * viewRow[2] + viewRow[3];
                    orderScratch_[i] = i;
                }
                std::sort(orderScratch_.begin(), orderScratch_.end(), [&](u32 a, u32 b) {
                    return depthScratch_[a] > depthScratch_[b] || (depthScratch_[a] == depthScratch_[b] && a < b);
                });
                orderCursor = alignUp(static_cast<u32>(orderCursor), 64u);
                const u32 oFirst = static_cast<u32>(orderCursor);
                if (!res_->writeBuffer(orderRing_[frame_], orderScratch_.data(), static_cast<u64>(it.n) * 4u, static_cast<u64>(oFirst) * 4u)) continue;
                orderCursor += it.n;
                res_->setSrvBuffer(set, 1, orderRing_[frame_], 4, it.n, oFirst);
                useOrder = true;
            } else {
                res_->setSrvBuffer(set, 1, dummyBuf_, 4, 1024, 0);
            }
            if (it.ring) {
                trailCursor = alignUp(static_cast<u32>(trailCursor), 16u);
                const u32 tFirst = static_cast<u32>(trailCursor);
                const u32 tCount = it.n * it.ring;
                if (v.trail->size() < tCount ||
                    !res_->writeBuffer(trailRing_[frame_], v.trail->data(), static_cast<u64>(tCount) * 16u, static_cast<u64>(tFirst) * 16u)) continue;
                trailCursor += tCount;
                res_->setSrvBuffer(set, 2, trailRing_[frame_], 16, tCount, tFirst);
            } else {
                res_->setSrvBuffer(set, 2, dummyBuf_, 16, 256, 0);
            }
            instanceCount = fx.render == RenderKind::Ribbon ? (linked ? it.n : it.n * it.ring) : it.n;
        }
        res_->setSrv(set, 3, tex ? tex : whiteTex_);

        // ---- the per-draw block ----
        ParticleDrawCB cb{};
        std::memcpy(cb.viewProj, cam.viewProj, sizeof cb.viewProj);
        setVec4(cb.eye, camPos.x, camPos.y, camPos.z, 0.0f);
        setVec4(cb.camRight, right.x, right.y, right.z, static_cast<f32>(fx.align));
        setVec4(cb.camUp, up.x, up.y, up.z, fx.stretch);
        const Vec3 ax = fx.alignAxis.getSafeNormal();
        setVec4(cb.axis, ax.x, ax.y, ax.z, useOrder ? 1.0f : 0.0f);
        setVec4(cb.size, fx.sizeStart, fx.sizeEnd, static_cast<f32>(fx.sizeCurve.n >= 2 ? fx.sizeCurve.n : 0),
                mesh ? 1.0f / mesh->diameter : 1.0f);
        for (int k = 0; k < 4; ++k) {
            cb.sizeT[k] = fx.sizeCurve.t[k];
            cb.sizeV[k] = fx.sizeCurve.v[k][0];
            cb.colT[k] = fx.colorCurve.t[k];
            for (int c = 0; c < 4; ++c) cb.colV[k][c] = fx.colorCurve.v[k][c];
        }
        for (int c = 0; c < 4; ++c) { cb.colA[c] = fx.colorStart[c]; cb.colB[c] = fx.colorEnd[c]; }
        setVec4(cb.subuv, std::max<f32>(fx.subuvCols, 1), std::max<f32>(fx.subuvRows, 1), std::max<f32>(fx.subuvFrames, 1), fx.subuvFps);
        setVec4(cb.flags, tex ? 1.0f : 0.0f, fx.subuvBlend ? 1.0f : 0.0f, fx.fog ? 1.0f : 0.0f, static_cast<f32>(static_cast<int>(lighting)));
        const u32 ring = it.gpu ? it.gb.trailRing : it.ring;
        setVec4(cb.trail, static_cast<f32>(ring), fx.trailFade, linked ? static_cast<f32>(it.gpu ? it.gb.capacity : it.n) : 0.0f, 0.0f);
        setVec4(cb.misc, additive ? 1.0f : 0.0f, static_cast<f32>(fx.meshOrient), static_cast<f32>(fx.colorCurve.n >= 2 ? fx.colorCurve.n : 0),
                static_cast<f32>(static_cast<int>(fx.render)));
        if (lighting == Lighting::Lit && lights) selectParticleLights(*lights, v.center, fx.boundsRadiusCm, cb.lights);

        // ---- the GI term ----
        const void* giData = nullptr;
        u32 giBytes = 0;
        if (giPipeline && giSeam_.bind) giSeam_.bind(*res_, set, kGiSrvBase, &giData, &giBytes, giSeam_.user);

        // ---- record ----
        if (pso != bound) { ctx.setPipeline(pso); bound = pso; }
        ctx.setBindingSet(set, 0);
        ctx.setConstantBuffer(kSimConstantRegister, &cb, static_cast<u32>(sizeof cb));
        if (giPipeline && giData) ctx.setConstantBuffer(rhi::kFeatureFrameConstantRegister, giData, giBytes);
        if (mesh) {
            ctx.setVertexBuffer(mesh->vb, 32);
            ctx.setIndexBuffer(mesh->ib, rhi::Format::R32Uint);
        } else {
            ctx.setVertexBuffer(quadVb_, 8);
            ctx.setIndexBuffer(quadIb_, rhi::Format::R32Uint);
        }
        if (it.gpu) {
            ctx.drawIndexedIndirect(it.gb.args, it.gb.argsOffset);
            ++drawStats_.gpuDraws;
        } else {
            ctx.drawIndexedInstanced(mesh ? mesh->indexCount : 6u, instanceCount, 0, 0, 0);
            drawStats_.cpuParticles += it.n;
        }
        ++drawStats_.draws;
    }
    ctx.popMarker();
}

} // namespace aver::particles
