#include "aver/world/ChunkPartition.hpp"

#if AVER_MODULE_SCENE

#  include "aver/scene/Components.hpp"

#  include <cmath>

namespace aver::world {
namespace {

// The root of e's hierarchy, or e itself. Guarded against a cycle rather than trusting the links:
// a malformed CHierarchy would otherwise hang the partition rather than report anything.
scene::Entity rootOf(const scene::World& w, scene::Entity e) {
    scene::Entity cur = e;
    for (u32 guard = 0; guard < 4096; ++guard) {
        const auto* h = w.component<scene::CHierarchy>(cur, scene::kComponentHierarchy);
        if (!h || h->parent == scene::kInvalidEntity) return cur;
        cur = h->parent;
    }
    return cur;
}

// Half the diagonal of a mesh renderer's local AABB, or -1 when there is nothing to measure.
f32 boundsRadiusCm(const scene::World& w, scene::Entity e) {
    const auto* mr = w.component<scene::CMeshRenderer>(e, scene::kComponentMeshRenderer);
    if (!mr) return -1.0f;
    const f32 dx = mr->aabbMax[0] - mr->aabbMin[0];
    const f32 dy = mr->aabbMax[1] - mr->aabbMin[1];
    const f32 dz = mr->aabbMax[2] - mr->aabbMin[2];
    if (dx <= 0.0f && dy <= 0.0f && dz <= 0.0f) return -1.0f;   // degenerate: not filled at load
    return 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
}

} // namespace

ChunkCoord ownerChunkOf(const scene::World& w, scene::Entity e, i32 chunkSizeCm) {
    const scene::Entity r = rootOf(w, e);
    const auto* loc = w.component<scene::CLocal>(r, scene::kComponentLocal);
    if (!loc) return ChunkCoord{};
    // A root has no parent, so its CLocal position IS its world position -- no compose pass needed,
    // which is what lets this run outside a frame.
    return splitCm(loc->xf.position, chunkSizeCm).chunk;
}

Partition partitionWorld(const scene::World& w, const PartitionOptions& opt) {
    Partition out;
    const i32 size = opt.chunkSizeCm > 0 ? opt.chunkSizeCm : kDefaultChunkSizeCm;
    const f32 oversizeCm = opt.oversizeChunks * static_cast<f32>(size);

    const u32 n = w.count();
    for (u32 i = 0; i < n; ++i) {
        const scene::Entity e = w.at(i);
        if (!w.valid(e)) continue;
        // A destroy queued this frame has not happened yet -- the entity is still live and still
        // returned by at(). Partitioning it would save something that is about to cease to exist.
        if (w.destroyPending(e)) continue;
        ++out.entityCount;

        const auto* h = w.component<scene::CHierarchy>(e, scene::kComponentHierarchy);
        if (h && h->parent != scene::kInvalidEntity) continue;   // travels with its root

        const auto* loc = w.component<scene::CLocal>(e, scene::kComponentLocal);
        if (!loc) continue;

        ++out.rootCount;
        out.chunks[splitCm(loc->xf.position, size).chunk].push_back(e);

        const f32 r = boundsRadiusCm(w, e);
        if (r < 0.0f) ++out.unknownBounds;
        else if (r > oversizeCm) out.oversize.push_back(e);
    }
    return out;
}

std::unordered_map<ChunkCoord, ChunkPayload> captureAll(const scene::World& w, const Partition& part,
                                                        i32 chunkSizeCm) {
    std::unordered_map<ChunkCoord, ChunkPayload> out;
    out.reserve(part.chunks.size());
    for (const auto& kv : part.chunks) out.emplace(kv.first, capture(w, kv.second, kv.first, chunkSizeCm));
    return out;
}

} // namespace aver::world

#endif // AVER_MODULE_SCENE
