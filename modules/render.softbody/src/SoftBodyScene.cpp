// The scene join: walk the soft-body entities, give each its own simulated mesh, dispatch the
// staging copy, release. See SoftBodyScene.hpp for the whole design -- what follows here is only
// softBodyPackVertices, the pure half, plus the includes and namespace the class half (appended
// below the marker) also needs.
#include "aver/render/SoftBodyScene.hpp"
#include "aver/scene/Components.hpp"
#include "aver/physics/physics_abi.h"
#include "aver/core/Log.hpp"

#include <string>
#include <vector>

#include <algorithm>

namespace aver::render {

// ---- the pure half ---------------------------------------------------------------------------------

// One vertex's position carried through worldToLocal, per the header's documented convention:
// Mat4 is ROW-VECTOR here, so local = worldPos * M means component j = sum_i(world[i] * M.m[i][j])
// + M.m[3][j] -- the translation row, not a translation COLUMN. Getting this backwards is exactly
// the bug a column-vector engine's muscle memory writes, and it is silent at the origin (both
// conventions agree there) and only visible once the transform carries both a rotation and a
// translation together, which is why the test for this exercises precisely that combination.
namespace {
Vec3 transformPoint(const f32* worldXyz, const Mat4& worldToLocal) {
    const f32 wx = worldXyz[0], wy = worldXyz[1], wz = worldXyz[2];
    Vec3 out;
    out.x = wx * worldToLocal.m[0][0] + wy * worldToLocal.m[1][0] + wz * worldToLocal.m[2][0] +
            worldToLocal.m[3][0];
    out.y = wx * worldToLocal.m[0][1] + wy * worldToLocal.m[1][1] + wz * worldToLocal.m[2][1] +
            worldToLocal.m[3][1];
    out.z = wx * worldToLocal.m[0][2] + wy * worldToLocal.m[1][2] + wz * worldToLocal.m[2][2] +
            worldToLocal.m[3][2];
    return out;
}
} // namespace

void softBodyPackVertices(const f32* worldPositionsXyz, u32 vertexCount,
                          const Mat4& worldToLocal,
                          const u32* indices, u32 indexCount,
                          const f32* sourceUvs,
                          rhi::MeshVertex* out) {
    // ---- positions and uvs, one pass, no dependency on the topology below ----
    for (u32 i = 0; i < vertexCount; ++i) {
        const Vec3 local = transformPoint(worldPositionsXyz + i * 3, worldToLocal);
        out[i].px = local.x; out[i].py = local.y; out[i].pz = local.z;
        out[i].u = sourceUvs ? sourceUvs[i * 2 + 0] : 0.0f;
        out[i].v = sourceUvs ? sourceUvs[i * 2 + 1] : 0.0f;
        // Zeroed here, not left uninitialised: the accumulation below only ever ADDS, and a vertex
        // touched by zero triangles (should not happen, but nothing here promises the index buffer
        // is exhaustive) must end up as an honest all-zero normal rather than whatever garbage the
        // caller's allocation happened to hold.
        out[i].nx = out[i].ny = out[i].nz = 0.0f;
    }

    // ---- normals: accumulate each triangle's RAW (unnormalised) face normal onto its three
    // vertices, normalise once at the end ----
    //
    // The raw, un-normalised cross product is the whole trick for the degenerate-triangle
    // requirement. A triangle with zero area has a zero-length cross product REGARDLESS of its
    // vertex positions -- there is no normalisation step inside this loop to divide that zero by,
    // so a degenerate triangle contributes the zero vector and nothing here can produce a NaN.
    // (The tempting alternative -- normalise each face normal before accumulating, so a fully
    // planar shape doesn't have large triangles outvoting small ones -- was rejected for exactly
    // that reason: it divides by the same zero-length vector the degenerate case is defined by.)
    // The FINAL per-vertex normalisation uses Vec3::getSafeNormal, which returns zero below its
    // tolerance rather than dividing by a near-zero length -- so a vertex touched only by
    // degenerate triangles, whose accumulated sum is exactly zero, comes out zero rather than NaN
    // there either.
    std::vector<Vec3> accum(vertexCount, Vec3{0, 0, 0});
    const u32 triCount = indexCount / 3;
    for (u32 t = 0; t < triCount; ++t) {
        const u32 a = indices[t * 3 + 0], b = indices[t * 3 + 1], c = indices[t * 3 + 2];
        // A corrupt index this far downstream would otherwise read and write past `out`'s and
        // `accum`'s allocations -- a heap corruption that surfaces symptoms nowhere near this
        // function. Skipping the triangle instead costs one wrong-looking normal, which is visible
        // on screen and therefore debuggable; the alternative is not.
        if (a >= vertexCount || b >= vertexCount || c >= vertexCount) continue;
        const Vec3 pa{out[a].px, out[a].py, out[a].pz};
        const Vec3 pb{out[b].px, out[b].py, out[b].pz};
        const Vec3 pc{out[c].px, out[c].py, out[c].pz};
        const Vec3 face = cross(pb - pa, pc - pa);
        accum[a] += face; accum[b] += face; accum[c] += face;
    }
    for (u32 i = 0; i < vertexCount; ++i) {
        const Vec3 n = accum[i].getSafeNormal();
        out[i].nx = n.x; out[i].ny = n.y; out[i].nz = n.z;
    }
}

// ==== the feature ==== (SoftBodyScene class implementation goes below this line)

namespace {

// A ceiling, so a runaway spawner degrades to authored-mesh draws instead of exhausting Jolt and
// GPU memory in silence -- the same shape as SkinnedScene's own kMaxResident, and for the same
// reason (CSoftBody's own comment says outright that nothing downstream enforces a budget; this is
// that enforcement). Set far lower than skinning's 256: a soft body is a whole Jolt simulation
// object plus its own staging ring, not a shared dispatch against one pose buffer, so it costs
// much more per instance.
constexpr u32 kMaxResident = 64;

} // namespace

SoftBodyScene::~SoftBodyScene() { shutdown(); }

bool SoftBodyScene::init(rhi::IDevice& dev) {
    shutdown();
    dev_ = &dev;
    res_ = dev.resources();
    ready_ = res_ != nullptr;
    if (!ready_)
        AVER_WARN("[SoftBody] this device has no resource factory; soft-body entities will draw "
                  "their authored mesh at rest for this run");
    return ready_;
}

void SoftBodyScene::shutdown() {
    decoded_.clear();
    // Retire while res_/dev_ are still valid -- retire() needs both to free a resident's buffers
    // and its skin-target mesh.
    for (auto& kv : live_) retire(kv.second);
    live_.clear();
    softable_.clear();
    dev_ = nullptr;
    res_ = nullptr;
    ready_ = false;
    simulatedLastFrame_ = 0;
}

// Tears down one resident's physics and GPU state. Also doubles as the unwind path inside
// resident() when a later step fails after an earlier one succeeded -- every field it touches is
// safe to call on a partially-built Resident because each is guarded by its own null/zero check.
void SoftBodyScene::retire(Resident& r) {
    // The physics body first: a resource-factory failure below must not leave a soft body still
    // simulating for an entity nothing will draw again.
    if (r.body) {
        aver_phys_remove_body(r.body);
        r.body = 0;
    }

    if (res_) {
        for (rhi::BufferHandle& h : r.staging) {
            if (h) { res_->destroyBuffer(h); h = 0; }
        }
    }

    // r.vertices is deliberately NOT destroyed here through the factory. It is the buffer
    // createSkinTargetMesh allocated as r.drawMesh's vertex stream, and IDevice::destroyMesh already
    // frees it as part of freeing the mesh (D3D12Device::destroyMesh releases m.vbBuffer, which is
    // the same handle) -- destroying it a second time here would be a double free of a handle the
    // factory has already recycled for something else.
    if (dev_ && r.drawMesh) {
        dev_->destroyMesh(r.drawMesh);
    }
    r.drawMesh = 0;
    r.vertices = 0;
}

// Finds or builds this entity's residency. Null is never an error by itself -- an entity whose mesh
// has not resolved yet, or whose device cannot make a skin-target mesh, is a supported outcome and
// simply draws its authored mesh instead (see the class header's own contract on this).
SoftBodyScene::Resident* SoftBodyScene::resident(scene::World& world, scene::Entity e, rhi::IDevice& dev) {
    if (const auto it = live_.find(e); it != live_.end()) return &it->second;
    if (live_.size() >= kMaxResident) return nullptr;
    if (!res_ || !mesh_ || !path_) return nullptr;

    // update() only calls resident() for an entity it already found a live, non-disabled CSoftBody
    // on, but this method does not trust its only caller for that -- a second one appearing later
    // must not be able to build a body with no compliance to read.
    auto* sb = world.component<scene::CSoftBody>(e, scene::kComponentSoftBody);
    if (!sb) return nullptr;

    const auto* mr = world.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
    if (!mr || mr->mesh == 0) return nullptr;   // nothing authored yet; not a failure, just early
    const u64 meshId = mr->mesh;

    // Meshes this device or this data already failed to soften, reported once rather than every
    // frame forever -- SkinnedScene's skinnable_ table, same reasoning.
    if (const auto it = softable_.find(meshId); it != softable_.end() && !it->second) return nullptr;

    const rhi::MeshHandle source = mesh_(meshId, user_);
    if (!source) return nullptr;   // the asset itself has not resolved yet; try again next frame

    // Decoded on first use and kept, so a second entity naming the same mesh costs nothing. The
    // path resolver is the host's; the decode and the cache are ours -- see the header for why that
    // split rather than the other one.
    auto dit = decoded_.find(meshId);
    if (dit == decoded_.end()) {
        const std::string path = path_ ? path_(meshId, user_) : std::string();
        fmt::OcMeshData decodedMesh;
        std::string why;
        if (path.empty() || !fmt::loadOcMesh(path, decodedMesh, &why)) {
            AVER_WARN("[SoftBody] mesh {} would not load ({}); entity {} keeps drawing its authored "
                      "mesh", meshId, why.empty() ? "no path" : why, e);
            softable_[meshId] = false;
            return nullptr;
        }
        dit = decoded_.emplace(meshId, std::move(decodedMesh)).first;
    }
    const fmt::OcMeshData* md = &dit->second;
    if (md->positions.empty() || md->indices.empty()) {
        AVER_WARN("[SoftBody] mesh {} has no positions or triangles to simulate; entity {} keeps "
                  "drawing its authored mesh", meshId, e);
        softable_[meshId] = false;
        return nullptr;
    }

    Resident r;
    r.drawMesh = dev.createSkinTargetMesh(source, &r.vertices);
    if (!r.drawMesh || !r.vertices) {
        AVER_WARN("[SoftBody] this device cannot make a skin-target mesh; entity {} keeps drawing "
                  "its authored mesh", e);
        softable_[meshId] = false;
        return nullptr;
    }

    // The staging ring, one Upload buffer per frame-in-flight -- writeBuffer is immediate and
    // unsynchronised (RHIResources.hpp's own words), and this engine keeps kSoftBodyFramesInFlight
    // frames going at once, so one buffer would be overwritten while the GPU was still copying out
    // of it. See this module's header for the full argument.
    rhi::BufferDesc bd;
    bd.bytes = static_cast<u64>(md->vertexCount()) * sizeof(rhi::MeshVertex);
    bd.kind = rhi::BufferKind::Upload;
    bd.debugName = "SoftBody staging";
    bool ringOk = true;
    for (u32 slot = 0; slot < kSoftBodyFramesInFlight; ++slot) {
        r.staging[slot] = res_->createBuffer(bd);
        if (!r.staging[slot]) { ringOk = false; break; }
    }
    if (!ringOk) {
        AVER_WARN("[SoftBody] could not allocate the staging ring for entity {}; it keeps drawing "
                  "its authored mesh", e);
        retire(r);   // unwinds drawMesh/vertices and whatever ring slots did succeed
        softable_[meshId] = false;
        return nullptr;
    }

    // The body is built from the asset's own bind-pose positions -- untouched by this entity's
    // transform -- and then offset into world space by cx/cy/cz, because aver_phys_softbody_create
    // has no matrix to take, only a centre. Rotation and non-uniform scale on the entity are
    // therefore NOT applied to the rest shape; only its translation reaches the body. That is a
    // real limit of the ABI as it stands today, not an oversight here.
    const auto* cw = world.component<scene::CWorld>(e, scene::kComponentWorld);
    const Vec3 center = cw ? Vec3{cw->m.m[3][0], cw->m.m[3][1], cw->m.m[3][2]} : Vec3{};

    // indices is u32 in OcMeshData and int32_t in the physics ABI; reinterpret_cast between them is
    // well-defined here (accessing an object through a pointer to the corresponding signed/unsigned
    // type is explicitly permitted, unlike most pointer-cast aliasing) and every index is a small
    // non-negative count that means the same thing read either way.
    // SKINNED WHEN THE MESH HAS A RIG, PLAIN WHEN IT DOES NOT, and that branch is the whole reason
    // CSoftBody::maxDistanceCm exists. Without it this module could only ever make free-floating
    // cloth: aver_phys_softbody_create takes no max distance, so a max distance set by an author
    // would have been silently ignored -- a dial connected to nothing, which is precisely the
    // "declared but unread" defect this codebase keeps producing. Review caught it as a dead field
    // before it shipped.
    //
    // The skinned path tethers every vertex to where ordinary bone skinning would have put it, free
    // to move up to maxDistanceCm and no further. That IS soft body on a skeletal mesh; the plain
    // path below is the degenerate case for a mesh with no rig to hang off.
    if (md->hasSkin()) {
        // OcMeshData stores joints as u16 and the ABI takes int32_t, so this widening cannot be a
        // reinterpret_cast the way the index array can -- it needs a real conversion.
        std::vector<int32_t> joints(md->joints.begin(), md->joints.end());
        const u32 boneCount = 1u + *std::max_element(md->joints.begin(), md->joints.end());
        r.body = aver_phys_softbody_create_skinned(
            md->positions.data(), static_cast<int32_t>(md->vertexCount()),
            reinterpret_cast<const int32_t*>(md->indices.data()),
            static_cast<int32_t>(md->indices.size()),
            nullptr, joints.data(), md->weights.data(),
            static_cast<int32_t>(fmt::kOcMeshInfluences), static_cast<int32_t>(boneCount),
            scene::softBodyMaxDistanceCm(*sb), /*backStopDistanceCm*/ -1.0f,
            center.x, center.y, center.z, sb->compliance);
        r.skinned = r.body != 0;
    }
    if (!r.body) {
        r.body = aver_phys_softbody_create(md->positions.data(), static_cast<int32_t>(md->vertexCount()),
                                           reinterpret_cast<const int32_t*>(md->indices.data()),
                                           static_cast<int32_t>(md->indices.size()),
                                           nullptr, center.x, center.y, center.z,
                                           sb->compliance, 0.0f);
        r.skinned = false;
    }
    if (!r.body) {
        AVER_WARN("[SoftBody] Jolt refused to create a body from mesh {} for entity {}; it keeps "
                  "drawing its authored mesh", meshId, e);
        retire(r);
        softable_[meshId] = false;
        return nullptr;
    }

    softable_[meshId] = true;
    r.meshId = meshId;
    r.vertexCount = md->vertexCount();
    r.source = md;
    r.scratch.resize(r.vertexCount);

    // The component owns the handle from here: retire() is driven off the component's own
    // disappearance in update(), and a save or an inspector reading CSoftBody.body should see the
    // live handle rather than the zero it was attached with.
    sb->body = r.body;

    return &live_.emplace(e, std::move(r)).first->second;
}

void SoftBodyScene::update(scene::World& world, rhi::IDevice& dev) {
    if (!ready_) return;

    // ---- retire residency whose entity, component, or DISABLED state says it should not exist ----
    //
    // kSoftBodyDisabled retires the resident rather than merely skipping its pack, because the flag
    // means "not simulated, not drawn from the simulation" (Components.hpp's own words on it) -- and
    // a Jolt body left stepping every frame for an entity nothing is reading from would be exactly
    // the declared-but-never-read state this codebase has shipped more than once before. Re-enabling
    // costs one fresh aver_phys_softbody_create the next time the entity qualifies, which is cheap
    // next to a physics step running for nothing every frame in between.
    std::vector<scene::Entity> dead;
    for (const auto& kv : live_) {
        const scene::Entity e = kv.first;
        const auto* sb = world.component<scene::CSoftBody>(e, scene::kComponentSoftBody);
        if (!world.valid(e) || world.destroyPending(e) || !sb || (sb->flags & scene::kSoftBodyDisabled))
            dead.push_back(e);
    }
    for (scene::Entity e : dead) {
        const auto it = live_.find(e);
        retire(it->second);
        live_.erase(it);
    }

    // ---- reconcile the living, and pack this frame's vertices ----
    simulatedLastFrame_ = 0;
    // Reused across every resident this frame rather than allocated per-entity: its capacity only
    // ever grows to the largest soft body touched so far.
    std::vector<f32> worldXyz;
    // THE CSoftBody POOL, NOT EVERY ENTITY. Walking world.count() paid a destroyPending and a sparse-set
    // probe for each of a level's tens of thousands of entities every frame, to find the few that
    // carry this component; the pool is dense over exactly those. entityAt() is the full generational
    // handle, the same value world.at() gives, and a retired entity has already left the pool
    // (World::flush drops it from every pool). resident() below only ever writes INTO a CSoftBody, so
    // the pool cannot shift under the walk, and the size is re-read each pass regardless.
    scene::ComponentPool* bodies = world.pool(scene::kComponentSoftBody);
    if (!bodies) return;
    for (usize i = 0; i < bodies->size(); ++i) {
        const scene::Entity e = bodies->entityAt(i);
        if (world.destroyPending(e)) continue;

        const auto* sb = static_cast<const scene::CSoftBody*>(bodies->dataAt(i));
        if (sb->flags & scene::kSoftBodyDisabled) continue;

        Resident* r = resident(world, e, dev);
        if (!r) continue;

        worldXyz.resize(static_cast<size_t>(r->vertexCount) * 3);
        const int32_t got = aver_phys_softbody_vertices(r->body, worldXyz.data(),
                                                         static_cast<int32_t>(r->vertexCount));
        if (got != static_cast<int32_t>(r->vertexCount)) {
            // The handle went bad, or the particle count changed, without the entity or its
            // component going anywhere. Leave prePass nothing new to write rather than stage a
            // short, torn read -- the mesh keeps showing its last good frame instead of collapsing
            // toward the origin.
            AVER_WARN("[SoftBody] entity {} read back {} of {} particles this frame; its mesh keeps "
                      "last frame's pose", e, got, r->vertexCount);
            r->packedThisFrame = false;
            continue;
        }

        // drawMesh applies the entity's own world matrix on top of whatever vertices it is handed
        // (this class's header states it outright), so the packed vertices must already be back in
        // MESH-LOCAL space before they reach it -- otherwise the transform lands twice and the mesh
        // renders at double its actual displacement from the origin. CWorld is read directly rather
        // than through World::worldMatrix() because update() runs AFTER World::flush() by contract,
        // so the cached matrix is already current; recomposing it again here would just repeat work
        // flush() already did this frame.
        const auto* cw = world.component<scene::CWorld>(e, scene::kComponentWorld);
        const Mat4 worldToLocal = cw ? cw->m.inverse() : Mat4::identity();

        softBodyPackVertices(worldXyz.data(), r->vertexCount, worldToLocal,
                             r->source->indices.data(), static_cast<u32>(r->source->indices.size()),
                             r->source->uvs.empty() ? nullptr : r->source->uvs.data(),
                             r->scratch.data());
        r->packedThisFrame = true;
        ++simulatedLastFrame_;
    }
}

rhi::MeshHandle SoftBodyScene::drawHandle(scene::Entity e) const {
    // Asked once per drawn entity by both hosts, and a level with no soft bodies has nothing
    // resident: answer before hashing the handle.
    if (live_.empty()) return 0;
    const auto it = live_.find(e);
    return it == live_.end() ? 0 : it->second.drawMesh;
}

void SoftBodyScene::prePass(rhi::IRenderContext& ctx) {
    if (!ready_ || live_.empty()) return;
    for (auto& kv : live_) {
        Resident& r = kv.second;
        if (!r.packedThisFrame) continue;   // update() had nothing new to show this frame

        r.ringSlot = (r.ringSlot + 1) % kSoftBodyFramesInFlight;
        const rhi::BufferHandle stage = r.staging[r.ringSlot];
        const u64 bytes = static_cast<u64>(r.scratch.size()) * sizeof(rhi::MeshVertex);
        if (!stage || !res_->writeBuffer(stage, r.scratch.data(), bytes)) {
            r.packedThisFrame = false;
            continue;
        }

        // GeometryRead -> CopyDest for the copy, then straight back -- the only two states this
        // buffer is ever in (see the class header's own note on why). The round trip happens every
        // frame a resident is packed, not once, because GeometryRead is a READ state and the input
        // assembler may have consumed the buffer in that state on any prior frame.
        ctx.bufferBarrier(r.vertices, r.state, rhi::ResourceState::CopyDest);
        ctx.copyBuffer(r.vertices, stage, bytes);
        ctx.bufferBarrier(r.vertices, rhi::ResourceState::CopyDest, rhi::ResourceState::GeometryRead);
        r.state = rhi::ResourceState::GeometryRead;

        r.packedThisFrame = false;   // consumed; the next write needs a fresh pack from update()
    }
}

} // namespace aver::render
