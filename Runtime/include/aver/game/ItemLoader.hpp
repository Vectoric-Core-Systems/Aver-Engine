// The MECHANISM half of authored-level streaming (docs/plans/wave11/LARGE_WORLD.md 2.4.1): given a batch of placement
// records, make their entities, bodies and meshes; given a resident unit, take them all away again. It owns no residency
// policy -- which units are wanted, in what order, within what time budget, with what pinning and write-back, is the
// caller's: game::LevelStreaming (the editable path) and, from the cook, game::AuthoredStreaming.
//
// What it carries, moved out of LevelStreaming unchanged: mesh acquire and release with the content cache's refcounts, the
// shared Jolt mesh-shape cache, slot-material binding, the instantiate call, the kinematic bodies of animated placements,
// the COMPONENT apply, and the teardown (bodies, entities children first, meshes).
//
// A UNIT is whatever the caller says it is -- a root and its descendants for LevelStreaming, a chunk for the cooked path --
// named by a u32 the caller picks. The loader keeps each unit's entities, bodies, acquired meshes and animated bodies.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcWorld.hpp"

#include <unordered_map>
#include <vector>

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/world/LevelInstance.hpp"

namespace aver { class HitchMarks; }
namespace aver::rhi { class IDevice; }

namespace aver::game {

class GameContent;

class ItemLoader {
public:
    // One placement of a batch. `rec` must stay valid for the call. `parent` is an index into the same batch (parents before
    // children, as OcWorldData has them), -1 for a root: the caller has already remapped, because only it knows which of a
    // placement's ancestors are part of this batch.
    struct Entry {
        const fmt::OcWorldPlacement* rec = nullptr;
        u32 tag = 0;      // the caller's id for this placement, echoed back in Loaded
        u32 item = 0;     // the unit that owns it
        i32 parent = -1;
    };
    // One entity the batch made.
    struct Loaded {
        scene::Entity entity = scene::kInvalidEntity;
        u32 tag = 0;
        u32 item = 0;
        i32 body = -1;    // the body the placement created (static, or kinematic for an animated one), -1 none
        const fmt::OcWorldPlacement* rec = nullptr;
    };

    // `opt` is what a full load would pass to world::instantiate (materials, ground, bounds, triangles); its progress
    // callback is ignored. The loader keeps a copy, with meshShapes pointed at its own cache.
    void begin(GameContent& content, rhi::IDevice& device, const world::InstantiateOptions& opt);
    // Releases every physics mesh shape and forgets every unit. Units still resident are the caller's to evict first.
    void end();
    bool active() const { return content_ != nullptr; }

    // Acquires the batch's meshes (once per unit), binds their materials, instantiates the records into the process-global
    // World and files the results under their units. `origin` is the floating origin the placements are narrowed against
    // (InstantiateOptions::origin); zero is today's absolute placement. `hm`, when given, gets the "meshes", "materials" and
    // "instantiate" marks. COMPONENT records are NOT applied here: the caller does its own bookkeeping first, then
    // applyComponents (the order the editor's hooks were written against).
    std::vector<Loaded> load(const std::vector<Entry>& entries, world::OriginCm origin = {}, HitchMarks* hm = nullptr);
    // Applies the COMPONENT records of a batch, in batch order. Warnings only.
    void applyComponents(const std::vector<Loaded>& loaded);

    // Tears one unit down: removes its bodies, destroys its entities (children first, so no destroy sees a parent already
    // gone), releases its meshes and forgets its kinematic bodies. `hm` gets "evict:destroy" and "evict:releaseMesh".
    // The caller's own hooks and maps are done before this; entitiesOf() is empty afterwards.
    void evict(scene::World& world, u32 item, HitchMarks* hm = nullptr);

    // ---- units the host made itself (the editor's new placements) ----
    // The unit now holds an entity the host created, and one reference on its mesh when the cache will give one.
    void adopt(u32 item, scene::Entity e, u64 meshObjectId);
    // The host destroyed `e` and its body: drop them from the unit (nothing here will remove that body again).
    void forget(u32 item, scene::Entity e);
    // Releases the unit's mesh references and nothing else.
    void releaseMeshes(u32 item);

    const std::vector<scene::Entity>& entitiesOf(u32 item) const;
    bool resident(u32 item) const { return items_.count(item) != 0; }

    // Kinematic bodies of resident animated placements, for world::driveKinematicBodies.
    const std::vector<world::AnimatedBody>& animatedBodies() const { return animated_; }

    // Physics mesh shapes by mesh id, shared by every batch; the ones whose mesh is no longer loaded are released.
    void releaseUnusedShapes();

    // ---- the content cache's prefetch, so a policy never reaches past the loader ----
    void prefetchTick();
    void prefetch(u64 meshObjectId, f32 priority);
    bool ready(u64 meshObjectId) const;

private:
    struct ItemState {
        std::vector<scene::Entity> entities;
        std::vector<std::pair<scene::Entity, i32>> bodies;
        std::vector<u64> meshes;                       // acquired while resident
        std::vector<world::AnimatedBody> animated;
    };
    void bindMeshMaterials(u64 meshId);
    void rebuildAnimated();

    GameContent* content_ = nullptr;
    rhi::IDevice* device_ = nullptr;
    world::InstantiateOptions opt_;
    std::unordered_map<u32, ItemState> items_;
    std::vector<world::AnimatedBody> animated_;
    // Physics shapes by mesh id, shared by every batch; released once the mesh is unloaded.
    std::unordered_map<u64, i32> meshShapes_;
};

} // namespace aver::game

#endif // AVER_MODULE_SCENE
