// See FluidScene.hpp for the whole design. What follows is spawn/despawn/update/prePass, the private
// retire() they share, and the one small packing helper both spawn() and update() call.
#include "aver/render/FluidScene.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/core/Log.hpp"

#include <vector>

namespace aver::render {

namespace {

// A ceiling, so a runaway spawner degrades to "no new volume" rather than exhausting Jolt and GPU
// memory in silence -- SoftBodyScene's kMaxResident, and for the same reason. Set to the same value:
// a fluid volume costs about the same per instance as a soft body (one Jolt object plus its own
// staging ring), and unlike a soft body's entity, fluid volumes are typically placed by a level
// author in the low tens rather than spawned by the hundreds at runtime.
constexpr u32 kMaxResident = 64;

// Copies FluidVolume's own positions and normals straight into the interleaved layout the rasteriser
// reads. NOT a general-purpose pack: it does no space conversion and computes no normal of its own,
// because both of those are already done by the time either caller reaches this -- see spawn()'s and
// update()'s own comments for why. `out` is resized to match; UVs are written as zero rather than
// left uninitialised, since this shell has no source parameterisation to carry across.
void packFluidVerts(const std::vector<f32>& positionsCm, const std::vector<f32>& normals,
                    std::vector<rhi::MeshVertex>& out) {
    const size_t n = positionsCm.size() / 3;
    out.resize(n);
    for (size_t i = 0; i < n; ++i) {
        out[i].px = positionsCm[i * 3 + 0];
        out[i].py = positionsCm[i * 3 + 1];
        out[i].pz = positionsCm[i * 3 + 2];
        out[i].nx = normals[i * 3 + 0];
        out[i].ny = normals[i * 3 + 1];
        out[i].nz = normals[i * 3 + 2];
        out[i].u = 0.0f;
        out[i].v = 0.0f;
    }
}

} // namespace

FluidScene::~FluidScene() { shutdown(); }

bool FluidScene::init(rhi::IDevice& dev) {
    shutdown();
    dev_ = &dev;
    res_ = dev.resources();
    ready_ = res_ != nullptr;
    if (!ready_)
        AVER_WARN("[Fluid] this device has no resource factory; fluid volumes cannot be spawned "
                  "this run");
    return ready_;
}

void FluidScene::shutdown() {
    for (auto& kv : live_) retire(kv.second);
    live_.clear();
    dev_ = nullptr;
    res_ = nullptr;
    ready_ = false;
}

// Tears down one resident's physics and GPU state. Also the unwind path inside spawn() when a later
// step fails after an earlier one succeeded -- every field it touches is guarded by its own
// null/zero check, so it is safe to call on a partially-built Resident.
void FluidScene::retire(Resident& r) {
    // The physics body first: a resource-factory failure below must not leave a soft body still
    // simulating for a handle nothing will draw again.
    if (r.body) {
        aver_phys_remove_body(r.body);
        r.body = 0;
    }

    if (res_) {
        for (rhi::BufferHandle& h : r.staging) {
            if (h) { res_->destroyBuffer(h); h = 0; }
        }
    }

    // r.vertices is deliberately NOT destroyed here through the factory, for the identical reason
    // SoftBodyScene::retire states: it is the buffer createSkinTargetMesh allocated as r.drawMesh's
    // vertex stream, and destroyMesh already frees it as part of freeing the mesh.
    if (dev_ && r.drawMesh) {
        dev_->destroyMesh(r.drawMesh);
    }
    r.drawMesh = 0;
    r.vertices = 0;

    // r.source, UNLIKE SoftBodyScene's equivalent, IS ours to free -- this module built it with its
    // own createMesh call in spawn() rather than borrowing it from a host's asset cache, so nothing
    // else will ever destroy it if this does not. Freeing it AFTER r.drawMesh, not before, matters:
    // createSkinTargetMesh built r.drawMesh to share r.source's index buffer, and destroyMesh's own
    // contract is to return false and free nothing for a mesh that is "still shared" -- so freeing
    // r.source first here would just fail quietly and leak it. By the time this line runs r.drawMesh
    // is already gone and holds no more shares, so this is the call that actually releases it.
    if (dev_ && r.source) {
        dev_->destroyMesh(r.source);
    }
    r.source = 0;
}

FluidHandle FluidScene::spawn(const water::FluidVolumeDesc& desc, rhi::IDevice& dev) {
    if (!ready_) return 0;
    if (live_.size() >= kMaxResident) {
        AVER_WARN("[Fluid] resident cap ({}) reached; refusing a new volume", kMaxResident);
        return 0;
    }

    Resident r(desc);
    r.vol.generateSeedShell();

    // The render mesh's OWN coordinate frame -- what createMesh's `source` is measured in, and
    // therefore what its once-computed bounds and any BLAS built from it are measured in too -- is
    // built from seedPositionsCm(), the LOCAL shell, not positionsCm(), the WORLD one
    // generateSeedShell() also populates. Every other MeshHandle in this engine keeps its own local
    // origin near its own geometry and lets a per-draw world matrix carry it to its actual placement
    // (meshBounds's own comment calls this out explicitly: a local-space bounding sphere, "before
    // any world transform"). Baking desc.centreCm into what the RHI treats as this mesh's local
    // frame would leave that bounding sphere sitting nowhere near the geometry it exists to cull,
    // for no benefit this module needs -- the physics body below is built from this exact same LOCAL
    // array for the identical reason.
    std::vector<rhi::MeshVertex> seedVerts;
    packFluidVerts(r.vol.seedPositionsCm(), r.vol.normals(), seedVerts);

    // vol.indices() is i32 (FluidVolume's own choice, matching aver_phys_softbody_create's
    // int32_t* below); IDevice::createMesh wants u32*. The reinterpret_cast is well-defined --
    // accessing an object through a pointer to its corresponding signed/unsigned type is explicitly
    // permitted, unlike most pointer-cast aliasing -- and every index here is a small non-negative
    // count that reads as the identical value either way.
    r.source = dev.createMesh(seedVerts.data(), static_cast<u32>(seedVerts.size()),
                              reinterpret_cast<const u32*>(r.vol.indices().data()),
                              static_cast<u32>(r.vol.indices().size()));
    if (!r.source) {
        AVER_WARN("[Fluid] this device would not upload the seed shell; refusing this volume");
        return 0;
    }

    r.drawMesh = dev.createSkinTargetMesh(r.source, &r.vertices);
    if (!r.drawMesh || !r.vertices) {
        AVER_WARN("[Fluid] this device cannot make a skin-target mesh; refusing this volume");
        retire(r);
        return 0;
    }

    // The staging ring, one Upload buffer per frame-in-flight -- see this module's header for the
    // full argument (writeBuffer is immediate and unsynchronised, and this engine keeps
    // kFluidFramesInFlight frames going at once).
    rhi::BufferDesc bd;
    bd.bytes = static_cast<u64>(r.vol.vertexCount()) * sizeof(rhi::MeshVertex);
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

    const water::FluidVolumeDesc& d = r.vol.desc();
    // invMasses is NULL, deliberately: every particle keeps inverse mass 1, which means NOTHING IS
    // PINNED. A pinned particle is how a flag stays attached to its pole -- exactly the wrong
    // behaviour for a liquid, whose entire reason for existing here is that a hard enough slosh can
    // carry it clean over the basin rim and leave it there. Pinning even the bottom ring would
    // tether the whole body to its rest shape forever and defeat the one behaviour this module
    // exists to allow.
    // A desc that did not name a pressure gets one derived from its own size and subdivision. Done
    // HERE rather than inside water::FluidVolume because this is the module that owns the moment the
    // body is created -- FluidVolume deliberately knows nothing about soft bodies -- and done at all
    // because a single constant cannot be right for two pools of different depths. See
    // water::fluidPressureFor for the arithmetic.
    const f32 pressure = d.pressure < 0.0f ? water::fluidPressureFor(d) : d.pressure;
    r.body = aver_phys_softbody_create(r.vol.seedPositionsCm().data(), r.vol.vertexCount(),
                                       r.vol.indices().data(),
                                       static_cast<int32_t>(r.vol.indices().size()),
                                       nullptr,
                                       d.centreCm[0], d.centreCm[1], d.centreCm[2],
                                       d.compliance, pressure);
    if (!r.body) {
        AVER_WARN("[Fluid] Jolt refused to create a body for this volume; refusing it");
        retire(r);
        return 0;
    }

    const FluidHandle h = nextHandle_++;
    live_.emplace(h, std::move(r));
    return h;
}

void FluidScene::despawn(FluidHandle h) {
    const auto it = live_.find(h);
    if (it == live_.end()) return;
    retire(it->second);
    live_.erase(it);
}

void FluidScene::update() {
    if (!ready_) return;

    // Reused across every resident this call rather than allocated per-volume: each buffer's
    // capacity only ever grows to the largest volume touched so far. SoftBodyScene::update keeps the
    // identical shared scratch for the identical reason.
    std::vector<f32> physicsXyz;
    std::vector<rhi::MeshVertex> packed;

    for (auto& kv : live_) {
        const FluidHandle h = kv.first;
        Resident& r = kv.second;
        // Sized from the SOLVER's own count, not just trusted to still equal r.vol.vertexCount() --
        // the two are supposed to be the same particle count forever once a body is built (nothing
        // in this ABI adds or removes soft-body particles after creation), but reading the body's
        // own answer and checking it against what FluidVolume expects is what turns a silent
        // mismatch into the warning below instead of a buffer overrun.
        const int32_t bodyCount = aver_phys_softbody_vertex_count(r.body);
        const i32 expected = r.vol.vertexCount();
        physicsXyz.resize(static_cast<size_t>(bodyCount) * 3);
        const int32_t got = aver_phys_softbody_vertices(r.body, physicsXyz.data(), bodyCount);
        if (got != expected) {
            // The handle went bad, or the particle count changed, without this resident going
            // anywhere. Leave prePass nothing new to copy rather than stage a short, torn read -- the
            // mesh keeps showing its last good frame instead of collapsing toward the origin.
            AVER_WARN("[Fluid] volume {} read back {} of {} expected particles this frame; its mesh "
                      "keeps last frame's pose", h, got, expected);
            r.stagedThisFrame = false;
            continue;
        }

        // updateFromSimulation also recomputes normals from this frame's deformed shape -- the seed
        // shell's normals stop being correct the instant the body moves, which is the whole point of
        // simulating it.
        r.vol.updateFromSimulation(physicsXyz.data(), got);

        // Packed straight from FluidVolume's own positions and normals, NOT through
        // render::softBodyPackVertices. That helper exists to solve two problems neither apply here:
        // it recomputes normals by re-walking the index buffer and accumulating face normals, which
        // is exactly what updateFromSimulation just finished doing to this same deformed shape, and
        // it converts world-space positions back to mesh-local because drawMesh is about to apply a
        // further entity transform on top. Nothing here applies a further transform -- see
        // drawHandle()'s own comment for why the composition root draws this buffer with an identity
        // world matrix -- so vol.positionsCm() is used exactly as FluidVolume already produced it,
        // in WORLD space, with no conversion and no second normal computation.
        packFluidVerts(r.vol.positionsCm(), r.vol.normals(), packed);

        r.ringSlot = (r.ringSlot + 1) % kFluidFramesInFlight;
        const rhi::BufferHandle stage = r.staging[r.ringSlot];
        const u64 bytes = static_cast<u64>(packed.size()) * sizeof(rhi::MeshVertex);
        if (!stage || !res_->writeBuffer(stage, packed.data(), bytes)) {
            AVER_WARN("[Fluid] volume {} could not stage this frame's vertices; its mesh keeps "
                      "last frame's pose", h);
            r.stagedThisFrame = false;
            continue;
        }
        r.stagedThisFrame = true;
    }
}

rhi::MeshHandle FluidScene::drawHandle(FluidHandle h) const {
    const auto it = live_.find(h);
    return it == live_.end() ? 0 : it->second.drawMesh;
}

void FluidScene::prePass(rhi::IRenderContext& ctx) {
    if (!ready_ || live_.empty()) return;
    for (auto& kv : live_) {
        Resident& r = kv.second;
        if (!r.stagedThisFrame) continue;   // update() had nothing new to show this frame

        const rhi::BufferHandle stage = r.staging[r.ringSlot];
        const u64 bytes = static_cast<u64>(r.vol.vertexCount()) * sizeof(rhi::MeshVertex);

        // GeometryRead -> CopyDest for the copy, then straight back -- the only two states this
        // buffer is ever in, exactly as SoftBodyScene::prePass documents and for the same reason.
        // The round trip happens every frame a resident is staged, not once, because GeometryRead is
        // a READ state and the input assembler may have consumed the buffer in that state on any
        // prior frame.
        ctx.bufferBarrier(r.vertices, r.state, rhi::ResourceState::CopyDest);
        ctx.copyBuffer(r.vertices, stage, bytes);
        ctx.bufferBarrier(r.vertices, rhi::ResourceState::CopyDest, rhi::ResourceState::GeometryRead);
        r.state = rhi::ResourceState::GeometryRead;

        r.stagedThisFrame = false;   // consumed; the next copy needs a fresh stage from update()
    }
}

} // namespace aver::render
