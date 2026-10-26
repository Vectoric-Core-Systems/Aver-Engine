// What the host does with a chunk the streamer has decoded (LARGE_WORLD 2.4.6): the seam between "which chunks are resident"
// (ChunkStreamer, policy) and "what a resident chunk IS in the scene" (the host, mechanism).
//
// THREE STEPS, so a host can make a chunk wait for what it needs:
//   prepare   called with the decoded payload, and again every update while it answers Pending. The authored host starts mesh
//             prefetch here and says Pending until the meshes are read, so a building never appears without its mesh. Failed
//             records the chunk resident-and-empty (as an unreadable chunk is), so it is not retried every frame.
//   restore   makes the entities. Called only after Ready.
//   evict     takes them away again, bodies and all.
//
// ScatterChunkHost is today's behaviour, unchanged: Ready always, restore() of ChunkPayload.hpp, and the eviction loop the
// streamer always had. PCG scatter and every existing test run through it, which is what keeps them bit-identical.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/world/BodyRegistry.hpp"
#include "aver/world/ChunkCoord.hpp"
#include "aver/world/ChunkPayload.hpp"

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"

#  include <vector>

namespace aver::world {

enum class ChunkPrepare : u8 { Ready, Pending, Failed };

class IChunkHost {
public:
    virtual ~IChunkHost() = default;

    virtual ChunkPrepare prepare(const ChunkPayload& p) = 0;

    // The entities of the chunk, in the order the host wants them held. `opt` carries the floating origin the scene is in
    // (opt.origin) and the body registry; a host that does not use them ignores them.
    virtual std::vector<scene::Entity> restore(const ChunkPayload& p, scene::World& w, const RestoreOptions& opt) = 0;

    // Destroys `entities` (the list restore() returned for chunk `c`) and everything that belongs to them. Bodies the registry
    // knows go to `freedBodies` for the caller to remove from the simulation. Returns how many entities were destroyed.
    virtual u32 evict(const ChunkCoord& c, const std::vector<scene::Entity>& entities, scene::World& w, BodyRegistry& bodies,
                      std::vector<i32>& freedBodies) = 0;
};

class ScatterChunkHost final : public IChunkHost {
public:
    ChunkPrepare prepare(const ChunkPayload&) override { return ChunkPrepare::Ready; }

    std::vector<scene::Entity> restore(const ChunkPayload& p, scene::World& w, const RestoreOptions& opt) override {
        return ::aver::world::restore(p, w, opt);
    }

    u32 evict(const ChunkCoord&, const std::vector<scene::Entity>& entities, scene::World& w, BodyRegistry& bodies,
              std::vector<i32>& freedBodies) override {
        u32 destroyed = 0;
        for (const scene::Entity e : entities) {
            if (!w.valid(e)) continue;
            // The bodies FIRST, while the entity is still alive to be walked; the whole subtree, because World::destroy takes it.
            bodies.detachSubtree(w, e, freedBodies);
            w.destroy(e);
            ++destroyed;
        }
        return destroyed;
    }
};

} // namespace aver::world

#endif // AVER_MODULE_SCENE
