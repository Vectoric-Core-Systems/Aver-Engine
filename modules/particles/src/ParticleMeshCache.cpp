// ParticleMeshCache: one vertex / index buffer pair per mesh id, built from the host's plain arrays. See ParticleRenderer.hpp.
#include "aver/particles/ParticleRenderer.hpp"

#include "aver/core/Log.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace aver::particles {

void ParticleMeshCache::shutdown() {
    if (res_) {
        for (auto& kv : entries_) {
            if (kv.second.vb) res_->destroyBuffer(kv.second.vb);
            if (kv.second.ib) res_->destroyBuffer(kv.second.ib);
        }
    }
    entries_.clear();
    res_ = nullptr;
}

const ParticleMeshCache::Entry* ParticleMeshCache::get(u64 meshId, ParticleMeshFn fn, void* user) {
    if (meshId == 0 || !res_) return nullptr;
    auto it = entries_.find(meshId);
    if (it != entries_.end()) return it->second.failed ? nullptr : &it->second;
    Entry e;
    e.failed = true;   // until proven otherwise; a refused mesh is not retried every frame
    ParticleMeshData d;
    if (!fn || !fn(meshId, d, user)) {
        AVER_WARN("[Particles] mesh particles: no mesh for id {} (no resolver, or the host could not load it)", meshId);
        entries_[meshId] = e;
        return nullptr;
    }
    const usize vcount = d.positions.size() / 3;
    const usize tris = d.indices.size() / 3;
    if (vcount == 0 || tris == 0 || tris > kMaxTriangles || d.normals.size() < vcount * 3) {
        AVER_WARN("[Particles] mesh particles: mesh {} refused ({} vertices, {} triangles; the limit is {} triangles and normals are required)",
                  meshId, vcount, tris, kMaxTriangles);
        entries_[meshId] = e;
        return nullptr;
    }
    for (u32 idx : d.indices) {
        if (idx >= vcount) {
            AVER_WARN("[Particles] mesh particles: mesh {} has an index past its vertices", meshId);
            entries_[meshId] = e;
            return nullptr;
        }
    }
    std::vector<f32> verts(vcount * 8);
    f32 lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (usize i = 0; i < vcount; ++i) {
        for (int k = 0; k < 3; ++k) {
            verts[i * 8 + static_cast<usize>(k)] = d.positions[i * 3 + static_cast<usize>(k)];
            verts[i * 8 + 3 + static_cast<usize>(k)] = d.normals[i * 3 + static_cast<usize>(k)];
            lo[k] = std::fmin(lo[k], d.positions[i * 3 + static_cast<usize>(k)]);
            hi[k] = std::fmax(hi[k], d.positions[i * 3 + static_cast<usize>(k)]);
        }
        verts[i * 8 + 6] = d.uvs.size() >= (i + 1) * 2 ? d.uvs[i * 2] : 0.0f;
        verts[i * 8 + 7] = d.uvs.size() >= (i + 1) * 2 ? d.uvs[i * 2 + 1] : 0.0f;
    }
    const f32 dx = hi[0] - lo[0], dy = hi[1] - lo[1], dz = hi[2] - lo[2];
    e.diameter = std::max(std::sqrt(dx * dx + dy * dy + dz * dz), 1e-3f);

    rhi::BufferDesc vb;
    vb.bytes = verts.size() * sizeof(f32);
    vb.kind = rhi::BufferKind::Upload;
    vb.debugName = "Aver.Particles mesh vertices";
    e.vb = res_->createBuffer(vb);
    rhi::BufferDesc ib;
    ib.bytes = d.indices.size() * sizeof(u32);
    ib.kind = rhi::BufferKind::Upload;
    ib.debugName = "Aver.Particles mesh indices";
    e.ib = res_->createBuffer(ib);
    if (!e.vb || !e.ib || !res_->writeBuffer(e.vb, verts.data(), vb.bytes) || !res_->writeBuffer(e.ib, d.indices.data(), ib.bytes)) {
        AVER_WARN("[Particles] mesh particles: buffers for mesh {} could not be created", meshId);
        if (e.vb) res_->destroyBuffer(e.vb);
        if (e.ib) res_->destroyBuffer(e.ib);
        e.vb = e.ib = 0;
        entries_[meshId] = e;
        return nullptr;
    }
    e.indexCount = static_cast<u32>(d.indices.size());
    e.failed = false;
    return &(entries_[meshId] = e);
}

} // namespace aver::particles
