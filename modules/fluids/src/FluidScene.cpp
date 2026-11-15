// The solver-to-rasteriser join. See FluidScene.hpp.
#include "aver/fluids/FluidScene.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace aver::fluids {

namespace {

// More than this many resident volumes and a new spawn is refused.
constexpr u32 kMaxResident = 64;

// Particles within this many metres of the scene's colliders are what the field has to resolve.
constexpr f32 kSolidMarginSpacings = 2.0f;

} // namespace

FluidScene::~FluidScene() { shutdown(); }

bool FluidScene::init(rhi::IDevice& dev) {
    shutdown();
    dev_ = &dev;
    res_ = dev.resources();
    ready_ = res_ != nullptr;
    if (!ready_) {
        AVER_WARN("[Fluid] this device has no resource factory; fluid volumes cannot be spawned "
                  "this run");
    }
    return ready_;
}

void FluidScene::shutdown() {
    for (auto& kv : live_) retire(*kv.second);
    live_.clear();
    dev_ = nullptr;
    res_ = nullptr;
    ready_ = false;
    haveLastUpdate_ = false;
}

void FluidScene::retire(Resident& r) {
    if (res_) {
        for (rhi::BufferHandle& h : r.staging) {
            if (h) { res_->destroyBuffer(h); h = 0; }
        }
    }
    // destroyMesh on the draw mesh releases its vertex buffer; `source` is its own mesh handle.
    if (dev_ && r.drawMesh) dev_->destroyMesh(r.drawMesh);
    r.drawMesh = 0;
    r.vertices = 0;
    if (dev_ && r.source) dev_->destroyMesh(r.source);
    r.source = 0;
}

FluidHandle FluidScene::spawn(const fluids::FluidVolumeDesc& desc, rhi::IDevice& dev) {
    if (!ready_) return 0;
    if (live_.size() >= kMaxResident) {
        AVER_WARN("[Fluid] resident cap ({}) reached; refusing a new volume", kMaxResident);
        return 0;
    }

    fluids::FluidVolumeDesc resolved = desc;
    std::string materialConflict;
    if (!fluids::fluidResolvePhysicsMaterial(resolved, &materialConflict)) {
        AVER_ERROR("[Fluid] refusing to spawn: {}", materialConflict);
        return 0;
    }

    static bool warnedLegacy = false;
    if (!warnedLegacy && (resolved.compliance != kFluidLegacyCompliance || resolved.pressure != kFluidPressureAuto)) {
        warnedLegacy = true;
        AVER_WARN("[Fluid] compliance and pressure belong to the retired soft-body shell and are ignored; "
                  "the volume is a particle fluid now (use damping for thickness, iterations for stiffness)");
    }

    auto rp = std::make_unique<Resident>(resolved);
    Resident& r = *rp;
    const fluids::PbfSettings settings = fluids::fluidPbfSettings(resolved);

    // Voxelise the scene's static colliders around the volume. The query asks the physics world
    // whether anything touches a sphere; local metres go to world cm through the volume centre.
    {
        const f32 margin = kSolidMarginSpacings * std::max(settings.spacingM, 0.01f);
        f32 lo[3], hi[3];
        for (int a = 0; a < 3; ++a) {
            lo[a] = settings.bounds.lo[a] - margin;
            hi[a] = settings.bounds.hi[a] + margin;
        }
        const f32 cx = resolved.centreCm[0], cy = resolved.centreCm[1], cz = resolved.centreCm[2];
        r.solid.build(lo, hi, 0.5f * settings.spacingM,
                      [cx, cy, cz](const f32 c[3], f32 radiusM) {
                          int32_t body = 0;
                          return aver_phys_overlap_sphere(cx + c[0] * kFluidMToCm, cy + c[1] * kFluidMToCm,
                                                          cz + c[2] * kFluidMToCm, radiusM * kFluidMToCm,
                                                          &body, 1) > 0;
                      });
    }

    const u32 n = r.sim.init(settings, &r.solid);
    if (n == 0) {
        AVER_WARN("[Fluid] the volume has no room for a single particle (extent or colliders); refusing it");
        return 0;
    }

    const u32 maxTris = std::clamp(std::max(8u * n, FluidSurfaceMesher::surfaceBudget(settings.bounds, r.sim.spacing())),
                                   4096u, kFluidMaxSurfaceTris);
    r.mesher.configure(settings.bounds, r.sim.spacing(), maxTris);
    r.maxVerts = maxTris * 3u;
    r.packed.assign(r.maxVerts, rhi::MeshVertex{});
    for (rhi::MeshVertex& v : r.packed) { v.nz = 1.0f; }

    // The template mesh: identity indices, every vertex at one corner but the second (the opposite
    // corner), so its bounds are the volume's box and its first triangle is degenerate.
    {
        std::vector<rhi::MeshVertex> seed(r.maxVerts);
        std::vector<u32> idx(r.maxVerts);
        const f32 hx = std::fabs(resolved.halfExtentCm[0]), hy = std::fabs(resolved.halfExtentCm[1]),
                  hz = std::fabs(resolved.halfExtentCm[2]);
        for (u32 i = 0; i < r.maxVerts; ++i) {
            rhi::MeshVertex& v = seed[i];
            const f32 s = (i == 1) ? 1.0f : -1.0f;
            v.px = s * hx; v.py = s * hy; v.pz = s * hz;
            v.nx = 0.0f; v.ny = 0.0f; v.nz = 1.0f;
            v.u = 0.0f; v.v = 0.0f;
            idx[i] = i;
        }
        r.source = dev.createMesh(seed.data(), r.maxVerts, idx.data(), r.maxVerts);
    }
    if (!r.source) {
        AVER_WARN("[Fluid] this device would not upload the surface template; refusing this volume");
        return 0;
    }

    r.drawMesh = dev.createSkinTargetMesh(r.source, &r.vertices);
    if (!r.drawMesh || !r.vertices) {
        AVER_WARN("[Fluid] this device cannot make a skin-target mesh; refusing this volume");
        retire(r);
        return 0;
    }

    rhi::BufferDesc bd;
    bd.bytes = static_cast<u64>(r.maxVerts) * sizeof(rhi::MeshVertex);
    bd.kind = rhi::BufferKind::Upload;
    bd.debugName = "Fluid staging";
    bool ringOk = true;
    for (u32 slot = 0; slot < kFluidFramesInFlight; ++slot) {
        r.staging[slot] = res_->createBuffer(bd);
        if (!r.staging[slot]) { ringOk = false; break; }
    }
    if (!ringOk) {
        AVER_WARN("[Fluid] could not allocate the staging ring; refusing this volume");
        retire(r);
        return 0;
    }

    // First surface now, so the volume draws the frame after it spawns rather than once it has stepped.
    r.mesher.build(r.sim.positions().data(), n);
    stageSurface(r);

    AVER_INFO("[Fluid] volume: {} particles at {:.1f} cm spacing, {} surface triangles, solid cells {}",
              n, r.sim.spacing() * kFluidMToCm, r.mesher.triangleCount(), r.solid.solidCellCount());

    const FluidHandle h = nextHandle_++;
    live_.emplace(h, std::move(rp));
    return h;
}

void FluidScene::despawn(FluidHandle h) {
    const auto it = live_.find(h);
    if (it == live_.end()) return;
    retire(*it->second);
    live_.erase(it);
}

void FluidScene::stageSurface(Resident& r) {
    const u32 tris = r.mesher.triangleCount();
    const u32 verts = tris * 3u;
    const std::vector<f32>& P = r.mesher.positions();
    const std::vector<f32>& N = r.mesher.normals();
    for (u32 v = 0; v < verts; ++v) {
        rhi::MeshVertex& o = r.packed[v];
        o.px = P[v * 3 + 0] * kFluidMToCm; o.py = P[v * 3 + 1] * kFluidMToCm; o.pz = P[v * 3 + 2] * kFluidMToCm;
        o.nx = N[v * 3 + 0]; o.ny = N[v * 3 + 1]; o.nz = N[v * 3 + 2];
        o.u = 0.0f; o.v = 0.0f;
    }
    // Vertices the previous surface used and this one does not collapse to the centre point.
    const u32 writeVerts = std::max(verts, r.usedVerts);
    for (u32 v = verts; v < writeVerts; ++v) {
        rhi::MeshVertex& o = r.packed[v];
        o.px = o.py = o.pz = 0.0f;
        o.nx = 0.0f; o.ny = 0.0f; o.nz = 1.0f;
        o.u = 0.0f; o.v = 0.0f;
    }
    r.usedVerts = verts;
    if (writeVerts == 0) { r.stagedThisFrame = false; return; }

    if (r.mesher.truncated() && !r.warnedTruncated) {
        r.warnedTruncated = true;
        AVER_WARN("[Fluid] a volume's surface exceeds {} triangles; the excess is not drawn "
                  "(lower the particle count or the volume size)", r.maxVerts / 3u);
    }

    r.ringSlot = (r.ringSlot + 1) % kFluidFramesInFlight;
    const rhi::BufferHandle stage = r.staging[r.ringSlot];
    const u64 bytes = static_cast<u64>(writeVerts) * sizeof(rhi::MeshVertex);
    if (!stage || !res_->writeBuffer(stage, r.packed.data(), bytes)) {
        AVER_WARN("[Fluid] could not stage a volume's surface this frame; its mesh keeps last frame's shape");
        r.stagedThisFrame = false;
        return;
    }
    r.copyVerts = writeVerts;
    r.stagedThisFrame = true;
}

void FluidScene::update() {
    const auto now = std::chrono::steady_clock::now();
    f32 dt = 1.0f / 60.0f;
    if (haveLastUpdate_) dt = std::chrono::duration<f32>(now - lastUpdate_).count();
    lastUpdate_ = now;
    haveLastUpdate_ = true;
    update(std::clamp(dt, 0.0f, 0.1f));
}

void FluidScene::update(f32 dtSeconds) {
    if (!ready_) return;
    for (auto& kv : live_) {
        Resident& r = *kv.second;
        if (r.sim.advance(dtSeconds) == 0) continue;   // no whole step this frame: the surface is current
        r.mesher.build(r.sim.positions().data(), r.sim.count());
        stageSurface(r);
    }
}

rhi::MeshHandle FluidScene::drawHandle(FluidHandle h) const {
    const auto it = live_.find(h);
    return it == live_.end() ? 0 : it->second->drawMesh;
}

bool FluidScene::volumeOrigin(FluidHandle h, f32 out[3]) const {
    const auto it = live_.find(h);
    if (it == live_.end()) return false;
    const fluids::FluidVolumeDesc& d = it->second->desc;
    out[0] = d.centreCm[0];
    out[1] = d.centreCm[1];
    out[2] = d.centreCm[2];
    return true;
}

int32_t FluidScene::physicsBody(FluidHandle) const { return 0; }

u32 FluidScene::particleCount(FluidHandle h) const {
    const auto it = live_.find(h);
    return it == live_.end() ? 0u : it->second->sim.count();
}

u32 FluidScene::triangleCount(FluidHandle h) const {
    const auto it = live_.find(h);
    return it == live_.end() ? 0u : it->second->mesher.triangleCount();
}

void FluidScene::addImpulse(FluidHandle h, const f32 centreCm[3], f32 radiusCm, const f32 velCmPerS[3]) {
    const auto it = live_.find(h);
    if (it == live_.end()) return;
    Resident& r = *it->second;
    const f32 local[3] = {(centreCm[0] - r.desc.centreCm[0]) * kFluidCmToM,
                          (centreCm[1] - r.desc.centreCm[1]) * kFluidCmToM,
                          (centreCm[2] - r.desc.centreCm[2]) * kFluidCmToM};
    const f32 dv[3] = {velCmPerS[0] * kFluidCmToM, velCmPerS[1] * kFluidCmToM, velCmPerS[2] * kFluidCmToM};
    r.sim.addImpulse(local, radiusCm * kFluidCmToM, dv);
}

void FluidScene::prePass(rhi::IRenderContext& ctx) {
    if (!ready_ || live_.empty()) return;
    for (auto& kv : live_) {
        Resident& r = *kv.second;
        if (!r.stagedThisFrame) continue;   // update() had nothing new to show this frame

        const rhi::BufferHandle stage = r.staging[r.ringSlot];
        const u64 bytes = static_cast<u64>(r.copyVerts) * sizeof(rhi::MeshVertex);

        ctx.bufferBarrier(r.vertices, r.state, rhi::ResourceState::CopyDest);
        ctx.copyBuffer(r.vertices, stage, bytes);
        ctx.bufferBarrier(r.vertices, rhi::ResourceState::CopyDest, rhi::ResourceState::GeometryRead);
        r.state = rhi::ResourceState::GeometryRead;

        r.stagedThisFrame = false;   // consumed; the next copy needs a fresh stage from update()
    }
}

} // namespace aver::fluids
