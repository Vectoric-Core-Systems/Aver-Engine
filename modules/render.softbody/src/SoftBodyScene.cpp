// The draw side of soft bodies: pack what the system publishes into vertices, keep one skin-target mesh per
// piece (rebuilt when its topology epoch changes), copy it into place before the scene pass. See SoftBodyScene.hpp.
#include "aver/render/SoftBodyScene.hpp"

#include "aver/core/Log.hpp"
#include "aver/scene/Components.hpp"

#include <algorithm>
#include <set>

namespace aver::render {

// ---- the pure half ---------------------------------------------------------------------------------

// One vertex's position carried through worldToLocal. Mat4 is ROW-VECTOR here: local = worldPos * M, so
// component j = sum_i(world[i] * M.m[i][j]) + M.m[3][j] -- the translation ROW, not a column. Getting this
// backwards is silent at the origin and only visible once the transform carries a rotation and a translation
// together, which is why the test for it exercises exactly that combination.
namespace {
Vec3 transformPoint(const f32* worldXyz, const Mat4& worldToLocal) {
    const f32 wx = worldXyz[0], wy = worldXyz[1], wz = worldXyz[2];
    Vec3 out;
    out.x = wx * worldToLocal.m[0][0] + wy * worldToLocal.m[1][0] + wz * worldToLocal.m[2][0] + worldToLocal.m[3][0];
    out.y = wx * worldToLocal.m[0][1] + wy * worldToLocal.m[1][1] + wz * worldToLocal.m[2][1] + worldToLocal.m[3][1];
    out.z = wx * worldToLocal.m[0][2] + wy * worldToLocal.m[1][2] + wz * worldToLocal.m[2][2] + worldToLocal.m[3][2];
    return out;
}
static_assert(sizeof(Vec3) == 3 * sizeof(f32), "softBodyPackPieces reads Vec3 arrays as packed floats");
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
        // Zeroed, not left uninitialised: the accumulation below only ever ADDS.
        out[i].nx = out[i].ny = out[i].nz = 0.0f;
    }

    // ---- normals: accumulate each triangle's RAW (unnormalised) face normal onto its three vertices ----
    // The raw cross product is the whole trick for the degenerate-triangle requirement: a zero-area triangle
    // has a zero-length cross product regardless of its vertices, and there is no normalisation inside the loop
    // to divide that zero by. The final per-vertex normalisation uses getSafeNormal, which returns zero below
    // its tolerance rather than dividing by a near-zero length, so a vertex touched only by degenerate
    // triangles comes out zero rather than NaN.
    std::vector<Vec3> accum(vertexCount, Vec3{0, 0, 0});
    const u32 triCount = indexCount / 3;
    for (u32 t = 0; t < triCount; ++t) {
        const u32 a = indices[t * 3 + 0], b = indices[t * 3 + 1], c = indices[t * 3 + 2];
        // A corrupt index would read and write past `out` and `accum`: skip the triangle (one wrong normal is
        // visible and debuggable, a heap corruption is not).
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

void softBodyPackPieces(const Vec3* worldPositions, u32 vertexCount,
                        const Mat4& worldToLocal,
                        const u32* indices, u32 indexCount,
                        const f32* uvs,
                        f32 scale01, const Vec3& centroidWorld,
                        rhi::MeshVertex* out) {
    if (scale01 >= 0.9999f) {
        softBodyPackVertices(reinterpret_cast<const f32*>(worldPositions), vertexCount, worldToLocal, indices, indexCount, uvs, out);
        return;
    }
    const f32 s = std::max(0.0f, scale01);
    std::vector<f32> shrunk(static_cast<usize>(vertexCount) * 3);
    for (u32 i = 0; i < vertexCount; ++i) {
        const Vec3 p = centroidWorld + (worldPositions[i] - centroidWorld) * s;
        shrunk[i * 3 + 0] = p.x; shrunk[i * 3 + 1] = p.y; shrunk[i * 3 + 2] = p.z;
    }
    softBodyPackVertices(shrunk.data(), vertexCount, worldToLocal, indices, indexCount, uvs, out);
}

// ==== the feature ====

SoftBodyScene::~SoftBodyScene() { shutdown(); }

bool SoftBodyScene::init(rhi::IDevice& dev) {
    shutdown();
    dev_ = &dev;
    res_ = dev.resources();
    ready_ = res_ != nullptr;
    if (!ready_)
        AVER_WARN("[SoftBody] this device has no resource factory; soft-body entities will draw their authored mesh at rest for this run");
    return ready_;
}

void SoftBodyScene::setResolvers(SoftBodyMeshFn, SoftBodyPathFn path, void* user) {
    // The mesh resolver is not needed any more; the path resolver feeds the system's decode caches.
    if (path) system().setPathResolver(path, user);
}

void SoftBodyScene::destroyNow(Retired& t) {
    // The draw mesh first: it gives the source its index-buffer share back. Its vertex buffer is freed with it.
    if (dev_ && t.drawMesh) dev_->destroyMesh(t.drawMesh);
    if (dev_ && t.source) dev_->destroyMesh(t.source);
    if (res_)
        for (rhi::BufferHandle& h : t.staging)
            if (h) { res_->destroyBuffer(h); h = 0; }
    t = Retired{};
}

void SoftBodyScene::retire(Resident& r) {
    Retired t;
    t.drawMesh = r.drawMesh;
    t.source = r.source;
    for (u32 i = 0; i < kSoftBodyFramesInFlight; ++i) t.staging[i] = r.staging[i];
    t.framesLeft = kSoftBodyFramesInFlight + 1;
    if (t.drawMesh || t.source) retired_.push_back(t);
    r.drawMesh = 0; r.source = 0; r.vertices = 0;
    for (rhi::BufferHandle& h : r.staging) h = 0;
}

void SoftBodyScene::ageRetired() {
    for (usize i = 0; i < retired_.size();) {
        if (--retired_[i].framesLeft == 0) {
            destroyNow(retired_[i]);
            retired_.erase(retired_.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

void SoftBodyScene::shutdown() {
    for (auto& kv : live_) retire(kv.second);
    live_.clear();
    byEntity_.clear();
    for (Retired& t : retired_) destroyNow(t);
    retired_.clear();
    dev_ = nullptr;
    res_ = nullptr;
    ready_ = false;
    simulatedLastFrame_ = 0;
    rebuildsLastFrame_ = 0;
}

bool SoftBodyScene::buildResident(Resident& r, const softbody::PieceView& v, rhi::IDevice& dev) {
    const fmt::OcMeshData* md = system().meshData(v.meshId);
    if (!md || v.vertexCount == 0 || v.indexCount == 0) return false;

    // The source mesh holds the authored vertices of this piece (UVs and rest normals through sourceVertex).
    std::vector<rhi::MeshVertex> rest(v.vertexCount);
    r.uvs.assign(static_cast<usize>(v.vertexCount) * 2, 0.0f);
    const u32 authored = md->vertexCount();
    for (u32 i = 0; i < v.vertexCount; ++i) {
        const u32 s = v.sourceVertex[i] < authored ? v.sourceVertex[i] : 0u;
        rhi::MeshVertex& o = rest[i];
        o.px = md->positions[s * 3 + 0]; o.py = md->positions[s * 3 + 1]; o.pz = md->positions[s * 3 + 2];
        if (md->normals.size() >= static_cast<usize>(s) * 3 + 3) { o.nx = md->normals[s * 3]; o.ny = md->normals[s * 3 + 1]; o.nz = md->normals[s * 3 + 2]; }
        else { o.nx = 0.0f; o.ny = 0.0f; o.nz = 1.0f; }
        if (md->uvs.size() >= static_cast<usize>(s) * 2 + 2) { o.u = md->uvs[s * 2]; o.v = md->uvs[s * 2 + 1]; }
        else { o.u = 0.0f; o.v = 0.0f; }
        r.uvs[i * 2] = o.u; r.uvs[i * 2 + 1] = o.v;
    }
    r.source = dev.createMesh(rest.data(), v.vertexCount, v.indices, v.indexCount);
    if (!r.source) return false;
    r.drawMesh = dev.createSkinTargetMesh(r.source, &r.vertices);
    if (!r.drawMesh || !r.vertices) {
        dev.destroyMesh(r.source);
        r.source = r.drawMesh = 0;
        return false;
    }

    // The staging ring, one Upload buffer per frame in flight.
    rhi::BufferDesc bd;
    bd.bytes = static_cast<u64>(v.vertexCount) * sizeof(rhi::MeshVertex);
    bd.kind = rhi::BufferKind::Upload;
    bd.debugName = "SoftBody staging";
    for (u32 slot = 0; slot < kSoftBodyFramesInFlight; ++slot) {
        r.staging[slot] = res_->createBuffer(bd);
        if (!r.staging[slot]) {
            retire(r);
            return false;
        }
    }
    r.state = rhi::ResourceState::Common;
    r.epoch = v.epoch;
    r.vertexCount = v.vertexCount;
    r.entity = v.entity;
    r.scratch.assign(v.vertexCount, rhi::MeshVertex{});
    r.packedThisFrame = false;
    r.packedOnce = false;
    return true;
}

void SoftBodyScene::update(scene::World& world, rhi::IDevice& dev) {
    simulatedLastFrame_ = 0;
    rebuildsLastFrame_ = 0;
    ageRetired();
    if (!ready_) return;

    u32 count = 0;
    const softbody::PieceView* views = system().pieces(count);
    const u32 rebuildCap = std::max(1u, system().options().maxRebuildsPerFrame);
    std::set<Key> seen;

    for (u32 i = 0; i < count; ++i) {
        const softbody::PieceView& v = views[i];
        const Key key{v.entity, v.piece};
        seen.insert(key);

        auto it = live_.find(key);
        if (it == live_.end()) {
            if (rebuildsLastFrame_ >= rebuildCap) continue;
            Resident r;
            if (!buildResident(r, v, dev)) continue;   // the entity keeps drawing its authored mesh
            ++rebuildsLastFrame_;
            it = live_.emplace(key, std::move(r)).first;
            byEntity_[v.entity] = key;
        } else if (it->second.epoch != v.epoch) {
            // A tear or a release changed the topology: a fresh mesh, the old one retired behind the GPU.
            if (rebuildsLastFrame_ >= rebuildCap) continue;   // the old mesh stays, frozen, for a frame
            retire(it->second);
            Resident r;
            if (!buildResident(r, v, dev)) {
                byEntity_.erase(v.entity);
                live_.erase(it);
                continue;
            }
            ++rebuildsLastFrame_;
            it->second = std::move(r);
        }

        Resident& r = it->second;
        if (r.vertexCount != v.vertexCount) continue;   // an epoch still waiting for its rebuild
        if (v.frozen && r.packedOnce) continue;          // the last packed vertices are still current

        Mat4 worldToLocal = Mat4::identity();
        if (v.useEntityWorld) {
            if (const auto* cw = world.component<scene::CWorld>(v.entity, scene::kComponentWorld)) worldToLocal = cw->m.inverse();
        } else {
            worldToLocal = Mat4::translation(Vec3{-v.origin.x, -v.origin.y, -v.origin.z});
        }
        softBodyPackPieces(v.world, v.vertexCount, worldToLocal, v.indices, v.indexCount, r.uvs.data(), v.scale01, v.centroid,
                           r.scratch.data());
        r.packedThisFrame = true;
        r.packedOnce = true;
        ++simulatedLastFrame_;

        // Culling bounds follow the simulation (the SkinnedScene rule): without them a flung piece is culled by
        // the asset box at its old place.
        if (auto* mr = world.component<scene::CMeshRenderer>(v.entity, scene::kComponentMeshRenderer)) {
            Vec3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
            for (const rhi::MeshVertex& mv : r.scratch) {
                lo = {std::min(lo.x, mv.px), std::min(lo.y, mv.py), std::min(lo.z, mv.pz)};
                hi = {std::max(hi.x, mv.px), std::max(hi.y, mv.py), std::max(hi.z, mv.pz)};
            }
            const f32 pad = 1.0f;
            mr->aabbMin[0] = lo.x - pad; mr->aabbMin[1] = lo.y - pad; mr->aabbMin[2] = lo.z - pad;
            mr->aabbMax[0] = hi.x + pad; mr->aabbMax[1] = hi.y + pad; mr->aabbMax[2] = hi.z + pad;
        }
    }

    // Residents whose piece is gone.
    for (auto it = live_.begin(); it != live_.end();) {
        if (seen.count(it->first)) { ++it; continue; }
        const auto be = byEntity_.find(it->first.first);
        if (be != byEntity_.end() && be->second == it->first) byEntity_.erase(be);
        retire(it->second);
        it = live_.erase(it);
    }
}

rhi::MeshHandle SoftBodyScene::drawHandle(scene::Entity e) const {
    // Asked once per drawn entity by both hosts: a level with no soft bodies has nothing resident.
    if (live_.empty()) return 0;
    const auto be = byEntity_.find(e);
    if (be == byEntity_.end()) return 0;
    const auto it = live_.find(be->second);
    return it == live_.end() ? 0 : it->second.drawMesh;
}

void SoftBodyScene::prePass(rhi::IRenderContext& ctx) {
    if (!ready_ || live_.empty()) return;
    for (auto& kv : live_) {
        Resident& r = kv.second;
        if (!r.packedThisFrame || !r.drawMesh) continue;   // update() had nothing new to show

        r.ringSlot = (r.ringSlot + 1) % kSoftBodyFramesInFlight;
        const rhi::BufferHandle stage = r.staging[r.ringSlot];
        const u64 bytes = static_cast<u64>(r.scratch.size()) * sizeof(rhi::MeshVertex);
        if (!stage || !res_->writeBuffer(stage, r.scratch.data(), bytes)) {
            r.packedThisFrame = false;
            continue;
        }
        // GeometryRead -> CopyDest for the copy and straight back, every frame a resident is packed: the
        // input assembler may have consumed the buffer in a read state on any prior frame.
        ctx.bufferBarrier(r.vertices, r.state, rhi::ResourceState::CopyDest);
        ctx.copyBuffer(r.vertices, stage, bytes);
        ctx.bufferBarrier(r.vertices, rhi::ResourceState::CopyDest, rhi::ResourceState::GeometryRead);
        r.state = rhi::ResourceState::GeometryRead;
        r.packedThisFrame = false;
    }
}

} // namespace aver::render
