// The draw side of strand hair. See HairScene.hpp.
#include "aver/render/HairScene.hpp"

#include "aver/core/Log.hpp"
#include "aver/rhi/ShaderFiles.hpp"
#include "aver/scene/Components.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace aver::render {

namespace {

// b3 holds RibbonCB; AverHairRibbons.hlsl declares t0 (points), t1 (render positions) and u0 (the vertex buffer).
constexpr u32 kRibbonConstantSlot = 3;
constexpr u32 kRibbonGroupSize = 64;

} // namespace

HairScene::HairScene() = default;

HairScene::~HairScene() { shutdown(); }

bool HairScene::init(rhi::IDevice& dev) {
    shutdown();
    dev_ = &dev;
    res_ = dev.resources();
    if (!res_ || !dev.caps().computeShaders) {
        AVER_WARN("[Hair] this device cannot run compute shaders; strand hair is not simulated or drawn");
        res_ = nullptr;
        dev_ = nullptr;
        return false;
    }
    if (!host_.init(dev)) {
        res_ = nullptr;
        dev_ = nullptr;
        return false;
    }

    rhi::ShaderDesc sd;
    sd.source = rhi::shaderFile("AverHairRibbons.hlsl").c_str();
    sd.entry = "CSRibbons";
    sd.stage = rhi::ShaderStage::Compute;
    sd.minShaderModel = 60;
    cs_ = res_->createShader(sd);
    if (!cs_) {
        AVER_ERROR("[Hair] the ribbon shader would not compile; strand hair is off");
        shutdown();
        return false;
    }
    rhi::ComputePipelineDesc pd;
    pd.cs = cs_;
    pd.layout.srvCount = 2;
    pd.layout.uavCount = 1;
    pd.layout.slotKindsDeclared = true;
    pd.layout.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
    pd.layout.srvKinds[1] = rhi::SlotKind::StructuredBuffer;
    pd.layout.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    pd.layout.constantDwords[kRibbonConstantSlot] = 0;   // a root CBV
    pipeline_ = res_->createComputePipeline(pd);
    if (!pipeline_) {
        AVER_ERROR("[Hair] the ribbon pipeline could not be created; strand hair is off");
        shutdown();
        return false;
    }
    ready_ = true;
    AVER_INFO("[Hair] strand hair ready (GPU simulation host, ribbon pass, {} levels)", kRibbonLodLevels);
    return true;
}

void HairScene::destroyNow(Retired& t) {
    // Parts first: a mesh cannot be destroyed while a part shares its vertex buffer.
    if (dev_)
        for (rhi::MeshHandle& m : t.meshes)
            if (m && m != kPartFailed) dev_->destroyMesh(m);
    if (res_) {
        if (t.set) res_->destroyBindingSet(t.set);
        if (t.buffer) res_->destroyBuffer(t.buffer);
    }
    if (t.material) pbr::MaterialLibrary::get().destroy(t.material);
    t = Retired{};
}

void HairScene::retire(Resident& r) {
    Retired t;
    for (u32 i = 0; i < 3; ++i) t.meshes[i] = r.part[i];
    t.meshes[3] = r.draw;
    t.set = r.set;
    t.framesLeft = kHairRetireFrames;
    if (t.meshes[3] || t.set) retired_.push_back(t);
    r = Resident{};
}

void HairScene::retireGroom(Groom& g) {
    Retired t;
    t.meshes[4] = g.source;
    t.buffer = g.points;
    t.material = g.material;
    t.framesLeft = kHairRetireFrames;
    if (t.meshes[4] || t.buffer || t.material) retired_.push_back(t);
    g.source = 0;
    g.points = 0;
    g.material = 0;
}

void HairScene::age() {
    for (usize i = 0; i < retired_.size();) {
        if (--retired_[i].framesLeft == 0) {
            destroyNow(retired_[i]);
            retired_.erase(retired_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void HairScene::shutdown() {
    for (auto& kv : live_) retire(kv.second);
    live_.clear();
    for (auto& kv : grooms_) retireGroom(kv.second);
    grooms_.clear();
    for (Retired& t : retired_) destroyNow(t);
    retired_.clear();
    failed_.clear();
    host_.shutdown();
    if (res_) {
        if (pipeline_) res_->destroyPipeline(pipeline_);
        if (cs_) res_->destroyShader(cs_);
    }
    pipeline_ = 0;
    cs_ = 0;
    dev_ = nullptr;
    res_ = nullptr;
    ready_ = false;
    buildsLast_ = dispatchesLast_ = refused_ = 0;
}

// ================================================================================ residency

bool HairScene::buildGroom(Groom& g, const hair::HairView& v, rhi::IDevice& dev) {
    RibbonInput in;
    in.strandStartCount2 = v.strandTable;
    in.strandCount = v.renderStrands;
    in.widthRootCm = v.widthRootCm;
    in.widthTipCm = v.widthTipCm;
    in.seed = v.seed;
    for (u32 i = 0; i < kRibbonLodLevels; ++i) in.lodFractions[i] = v.lodFractions[i];
    std::string why;
    if (!buildRibbonLayout(in, g.layout, &why)) {
        AVER_WARN("[Hair] groom 0x{:016X}: no ribbon layout: {}", v.key, why);
        return false;
    }

    rhi::BufferDesc bd;
    bd.bytes = static_cast<u64>(g.layout.points.size()) * sizeof(RibbonPoint);
    bd.kind = rhi::BufferKind::Upload;
    bd.debugName = "Aver.Hair points";
    g.points = res_->createBuffer(bd);
    if (!g.points || !res_->writeBuffer(g.points, g.layout.points.data(), bd.bytes)) {
        AVER_WARN("[Hair] groom 0x{:016X}: the point buffer could not be made", v.key);
        return false;
    }

    // The source mesh: zeroed vertices (the ribbon pass writes them) and the ribbon indices every instance's skin target shares.
    {
        std::vector<rhi::MeshVertex> zero(g.layout.vertexCount());
        g.source = dev.createMesh(zero.data(), g.layout.vertexCount(), g.layout.indices.data(), static_cast<u32>(g.layout.indices.size()));
    }
    if (!g.source) {
        AVER_WARN("[Hair] groom 0x{:016X}: the ribbon mesh could not be made", v.key);
        return false;
    }

    // The material of the groom: its look as a Hair material.
    pbr::MaterialDesc d;
    d.name = v.materialName;
    d.shadingModel = pbr::ShadingModel::Hair;
    for (int i = 0; i < 3; ++i) {
        d.baseColorFactor[i] = v.look.baseColor[i];
        d.hair.tipColor[i] = v.look.tipColor[i];
        d.hair.tint[i] = v.look.tint[i];
    }
    d.baseColorFactor[3] = 1.0f;
    d.hair.colorVariation = v.look.colorVariation;
    d.hair.roughness = v.look.roughness;
    d.hair.shift = v.look.shift;
    d.hair.tt = v.look.tt;
    d.hair.trt = v.look.trt;
    d.hair.azimuthRough = v.look.azimuthRough;
    d.hair.scatter = v.look.scatter;
    d.twoSided = true;
    d.castShadow = (v.flags & hair::kHairNoShadow) == 0;
    g.material = pbr::MaterialLibrary::get().create(d);
    if (g.material && bindFn_) bindFn_(v.materialToken, g.material, bindUser_);

    g.key = v.key;
    g.bytes = bd.bytes + static_cast<u64>(g.layout.vertexCount()) * sizeof(rhi::MeshVertex) + g.layout.indices.size() * sizeof(u32);
    return true;
}

bool HairScene::buildResident(Resident& r, Groom& g, const hair::HairView& v, rhi::IDevice& dev) {
    r = Resident{};
    r.draw = dev.createSkinTargetMesh(g.source, &r.vertices);
    if (!r.draw || !r.vertices) {
        AVER_WARN("[Hair] entity {}: this device cannot make a skin-target mesh for the ribbons", v.entity);
        r = Resident{};
        return false;
    }
    r.key = v.key;
    r.owner = v.entity;
    r.drawEntity = v.drawEntity;
    r.vertexCount = g.layout.vertexCount();
    r.bytes = static_cast<u64>(r.vertexCount) * sizeof(rhi::MeshVertex) + (g.layout.indices.size() * sizeof(u32) * 9) / 10;
    r.state = rhi::ResourceState::Common;
    return true;
}

void HairScene::ensureSet(Resident& r, const Groom& g, const hair::HairView& v) {
    if (r.set && r.setRenderBuffer == v.renderBuffer) return;
    if (r.set) {
        Retired t;
        t.set = r.set;
        t.framesLeft = kHairRetireFrames;
        retired_.push_back(t);
        r.set = 0;
    }
    const rhi::BufferHandle render = host_.gpuBuffer(v.renderBuffer);
    if (!render) return;
    rhi::BindingSetDesc sd;
    sd.srvCount = 2;
    sd.uavCount = 1;
    sd.srvKinds[0] = rhi::SlotKind::StructuredBuffer;
    sd.srvKinds[1] = rhi::SlotKind::StructuredBuffer;
    sd.uavKinds[0] = rhi::SlotKind::StructuredBuffer;
    r.set = res_->createBindingSet(sd);
    if (!r.set) return;
    res_->setSrvBuffer(r.set, 0, g.points, sizeof(RibbonPoint), static_cast<u32>(g.layout.points.size()), 0);
    res_->setSrvBuffer(r.set, 1, render, 12u, std::max<u32>(v.renderVertices, 1u), 0);
    res_->setUavBuffer(r.set, 0, r.vertices, static_cast<u32>(sizeof(rhi::MeshVertex)), r.vertexCount, 0);
    r.setRenderBuffer = v.renderBuffer;
}

rhi::MeshHandle HairScene::levelMesh(Resident& r, const Groom& g, u32 level, rhi::IDevice& dev) {
    if (level == 0 || level >= kRibbonLodLevels) return r.draw;
    rhi::MeshHandle& p = r.part[level - 1];
    if (p == kPartFailed) return r.draw;
    if (!p) {
        p = dev.createPosedPartMesh(r.draw, g.layout.indices.data(), g.layout.lodIndices[level]);
        if (!p) {
            p = kPartFailed;
            AVER_WARN("[Hair] entity {}: level {} part mesh could not be made; the full groom is drawn at that distance", r.owner, level);
            return r.draw;
        }
    }
    return p;
}

void HairScene::showDraw(scene::World& world, scene::Entity draw, bool visible) {
    if (!world.valid(draw)) return;
    auto* mr = world.component<scene::CMeshRenderer>(draw, scene::kComponentMeshRenderer);
    if (!mr) return;
    const bool now = (mr->flags & scene::kMeshRendererVisible) != 0;
    if (now == visible) return;
    if (visible) mr->flags |= scene::kMeshRendererVisible;
    else mr->flags &= ~scene::kMeshRendererVisible;
    mr->dirty = 1;
}

void HairScene::update(scene::World& world, rhi::IDevice& dev) {
    ++frame_;
    buildsLast_ = 0;
    dispatchesLast_ = 0;
    age();
    if (!ready_) return;

    u32 count = 0;
    const hair::HairView* views = system().views(count);

    // The camera: its place and how big a pixel is at one centimetre. The projection's second column carries 1 / tan(fov / 2) (a jitter only adds a
    // pixel's fraction of the depth column to it), so the pixel footprint is 2 / (that * the scene viewport height).
    f32 vp[16] = {}, inv[16] = {}, cam[3] = {};
    const bool haveCam = dev.camera(vp, inv, cam);
    f32 rect[4] = {0, 0, 0, 0};
    const f32 height = dev.sceneViewport(rect) && rect[3] > 1.0f ? rect[3] : 1080.0f;
    const f32 yScale = std::sqrt(vp[1] * vp[1] + vp[5] * vp[5] + vp[9] * vp[9]);
    const f32 pixelAt1cm = yScale > 1.0e-6f ? 2.0f / (yScale * height) : 0.0f;

    u64 totalBytes = 0;
    for (const auto& kv : grooms_) totalBytes += kv.second.bytes;
    for (const auto& kv : live_) totalBytes += kv.second.bytes;
    const u64 budget = static_cast<u64>(opt_.vramBudgetMB) * 1024ull * 1024ull;

    std::unordered_set<scene::Entity> seen;
    for (u32 i = 0; i < count; ++i) {
        const hair::HairView& v = views[i];
        if (v.drawEntity == scene::kInvalidEntity || !world.valid(v.drawEntity) || v.renderBuffer == 0 || v.renderStrands == 0) continue;
        seen.insert(v.drawEntity);

        // ---- the groom's static data ----
        if (failed_.count(v.key)) continue;
        auto git = grooms_.find(v.key);
        if (git == grooms_.end()) {
            if (buildsLast_ >= opt_.maxBuildsPerFrame) continue;
            Groom g;
            if (totalBytes + v.estimatedBytes > budget) {
                ++refused_;
                failed_[v.key] = 1;
                AVER_WARN("[Hair] groom 0x{:016X} would take {} MB and the hair budget is {} MB; it is not drawn", v.key,
                          v.estimatedBytes / (1024 * 1024), opt_.vramBudgetMB);
                continue;
            }
            if (!buildGroom(g, v, dev)) {
                retireGroom(g);
                failed_[v.key] = 1;
                continue;
            }
            totalBytes += g.bytes;
            ++buildsLast_;
            git = grooms_.emplace(v.key, std::move(g)).first;
        }
        Groom& g = git->second;
        g.idleFrames = 0;

        // ---- this instance ----
        auto rit = live_.find(v.drawEntity);
        if (rit != live_.end() && (rit->second.key != v.key)) {
            if (const auto old = grooms_.find(rit->second.key); old != grooms_.end() && old->second.refs > 0) --old->second.refs;
            retire(rit->second);
            live_.erase(rit);
            rit = live_.end();
        }
        if (rit == live_.end()) {
            if (buildsLast_ >= opt_.maxBuildsPerFrame) continue;
            Resident r;
            if (!buildResident(r, g, v, dev)) { failed_[v.key] = 1; continue; }
            totalBytes += r.bytes;
            ++buildsLast_;
            ++g.refs;
            rit = live_.emplace(v.drawEntity, std::move(r)).first;
        }
        Resident& r = rit->second;
        r.owner = v.entity;
        ensureSet(r, g, v);

        // ---- the level and the constants ----
        r.level = std::min<u32>(v.lodLevel, kRibbonLodLevels - 1);
        const f32 fraction = hair::hairDrawFraction(v.lodFractions, r.level, v.densityScale);
        r.current = levelMesh(r, g, r.level, dev);

        const Mat4 inverseWorld = v.world.inverse();
        RibbonConstants c;
        c.cam[0] = cam[0] * inverseWorld.m[0][0] + cam[1] * inverseWorld.m[1][0] + cam[2] * inverseWorld.m[2][0] + inverseWorld.m[3][0];
        c.cam[1] = cam[0] * inverseWorld.m[0][1] + cam[1] * inverseWorld.m[1][1] + cam[2] * inverseWorld.m[2][1] + inverseWorld.m[3][1];
        c.cam[2] = cam[0] * inverseWorld.m[0][2] + cam[1] * inverseWorld.m[1][2] + cam[2] * inverseWorld.m[2][2] + inverseWorld.m[3][2];
        c.pixelAt1cm = pixelAt1cm;
        c.pointCount = g.layout.lodPoints[r.level];
        c.frameSeed = frame_;
        c.widthScale = v.widthScale * ribbonLodWidthBoost(fraction);
        c.minPixels = opt_.minPixels;
        c.keepRank = fraction;
        r.constants = c;
        r.groups = (c.pointCount + kRibbonGroupSize - 1) / kRibbonGroupSize;
        r.renderHandle = v.renderBuffer;
        r.rayTraced = (v.flags & hair::kHairRayTraced) != 0;
        r.noShadow = (v.flags & hair::kHairNoShadow) != 0;

        // The ribbons mean something once the simulation has written the render positions at least once.
        r.dispatch = haveCam && v.visible && v.settled && r.set && r.groups > 0 && host_.written(v.renderBuffer);
        r.shown = r.dispatch;
        showDraw(world, v.drawEntity, r.shown);
    }

    // Residents whose instance is gone.
    for (auto it = live_.begin(); it != live_.end();) {
        if (seen.count(it->first)) { ++it; continue; }
        if (const auto gi = grooms_.find(it->second.key); gi != grooms_.end() && gi->second.refs > 0) --gi->second.refs;
        retire(it->second);
        it = live_.erase(it);
    }
    // Grooms nobody uses for a while.
    for (auto it = grooms_.begin(); it != grooms_.end();) {
        if (it->second.refs == 0 && ++it->second.idleFrames > 120) {
            retireGroom(it->second);
            failed_.erase(it->first);
            it = grooms_.erase(it);
        } else {
            ++it;
        }
    }
    // A failed groom is retried once nothing is drawing it (the asset may have been fixed and reloaded).
    for (auto it = failed_.begin(); it != failed_.end();) {
        bool drawn = false;
        for (u32 i = 0; i < count && !drawn; ++i) drawn = views[i].key == it->first;
        it = drawn ? std::next(it) : failed_.erase(it);
    }
}

rhi::MeshHandle HairScene::drawHandle(scene::Entity e) const {
    if (live_.empty()) return 0;
    const auto it = live_.find(e);
    if (it == live_.end() || !it->second.shown) return 0;
    return it->second.current;
}

bool HairScene::inRayTracing(scene::Entity drawEntity) const {
    const auto it = live_.find(drawEntity);
    return it != live_.end() && it->second.rayTraced;
}

bool HairScene::castsShadow(scene::Entity drawEntity) const {
    const auto it = live_.find(drawEntity);
    return it == live_.end() || !it->second.noShadow;
}

// ================================================================================ per frame

void HairScene::prePass(rhi::IRenderContext& ctx) {
    if (!ready_) return;
    // The "Hair" region of --gpu-timing: the replayed simulation batches and the ribbon dispatches, both recorded below.
    rhi::ScopedGpuStat hairGpuStat(ctx, "Hair");
    host_.beginFrame();
    host_.replay(ctx);

    bool opened = false;
    for (auto& kv : live_) {
        Resident& r = kv.second;
        if (!r.dispatch || !r.set || r.groups == 0) continue;
        if (!opened) { ctx.pushMarker("Aver.Hair.Ribbons"); opened = true; }
        host_.ensureState(ctx, r.renderHandle, rhi::ResourceState::NonPixelShaderResource);
        if (r.state != rhi::ResourceState::UnorderedAccess) {
            ctx.bufferBarrier(r.vertices, r.state, rhi::ResourceState::UnorderedAccess);
            r.state = rhi::ResourceState::UnorderedAccess;
        }
        ctx.setPipeline(pipeline_);
        ctx.setBindingSet(r.set, 0);
        ctx.setConstantBuffer(kRibbonConstantSlot, &r.constants, static_cast<u32>(sizeof(RibbonConstants)));
        ctx.dispatch(r.groups, 1, 1);
        // Readable by the input assembler, the shadow pass and an acceleration-structure build, which a posed mesh meets in one frame.
        ctx.bufferBarrier(r.vertices, rhi::ResourceState::UnorderedAccess, rhi::ResourceState::GeometryRead);
        r.state = rhi::ResourceState::GeometryRead;
        ++dispatchesLast_;
    }
    if (opened) ctx.popMarker();
}

void HairScene::overlayPass(rhi::IRenderContext& ctx, u32, u32) {
    if (!ready_) return;
    // EVERY resident, dispatched this frame or not: a buffer left in GeometryRead at submit makes next frame's barrier claim a state the hardware dropped.
    for (auto& kv : live_) {
        Resident& r = kv.second;
        if (r.vertices && r.state != rhi::ResourceState::Common) {
            ctx.bufferBarrier(r.vertices, r.state, rhi::ResourceState::Common);
            r.state = rhi::ResourceState::Common;
        }
    }
    host_.endFrame(ctx);
}

HairSceneStats HairScene::stats() const {
    HairSceneStats s;
    s.grooms = static_cast<u32>(grooms_.size());
    s.instances = static_cast<u32>(live_.size());
    for (const auto& kv : live_) s.drawn += kv.second.shown ? 1u : 0u;
    s.dispatches = dispatchesLast_;
    s.buildsLastFrame = buildsLast_;
    s.refused = refused_;
    for (const auto& kv : grooms_) s.bytes += kv.second.bytes;
    for (const auto& kv : live_) s.bytes += kv.second.bytes;
    s.host = host_.stats();
    return s;
}

} // namespace aver::render
