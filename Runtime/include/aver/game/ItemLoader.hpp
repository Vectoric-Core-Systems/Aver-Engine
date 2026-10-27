// Mechanism half of authored-level streaming: given placement records, makes their entities, bodies and meshes; given a
// resident unit, takes them all away. No residency policy here: callers (game::LevelStreaming, game::AuthoredStreaming) choose
// the units, their order, budgets and pinning. A unit is named by a u32 the caller picks.
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
    // `rec` must stay valid for the call. `parent` indexes this batch (parents before children), -1 for a root; the caller remaps it.
    struct Entry {
        const fmt::OcWorldPlacement* rec = nullptr;
        u32 tag = 0;
        u32 item = 0;
        i32 parent = -1;
    };
    struct Loaded {
        scene::Entity entity = scene::kInvalidEntity;
        u32 tag = 0;
        u32 item = 0;
        i32 body = -1;    // -1 = none
        const fmt::OcWorldPlacement* rec = nullptr;
    };

    // Keeps a copy of `opt` with meshShapes pointed at its own cache; the progress callback is ignored.
    void begin(GameContent& content, rhi::IDevice& device, const world::InstantiateOptions& opt);
    // Releases every physics mesh shape and forgets every unit. The caller evicts resident units first.
    void end();
    bool active() const { return content_ != nullptr; }

    // `origin` narrows placements (InstantiateOptions::origin; zero = absolute). `hm` gets the "meshes", "materials" and "instantiate"
    // marks. COMPONENT records are not applied here: the caller does its own bookkeeping, then calls applyComponents (the order
    // the editor's hooks expect).
    std::vector<Loaded> load(const std::vector<Entry>& entries, world::OriginCm origin = {}, HitchMarks* hm = nullptr);
    // In batch order. Failures only warn.
    void applyComponents(const std::vector<Loaded>& loaded);

    // Removes the unit's bodies, destroys its entities children first, releases its meshes and forgets its kinematic bodies.
    // `hm` gets "evict:destroy" and "evict:releaseMesh".
    void evict(scene::World& world, u32 item, HitchMarks* hm = nullptr);

    // Units the host made itself (editor placements). adopt: the unit holds an entity the host created, plus one mesh
    // reference when the cache gives one. forget: the host destroyed `e` and its body, so nothing here removes that body again.
    void adopt(u32 item, scene::Entity e, u64 meshObjectId);
    void forget(u32 item, scene::Entity e);
    void releaseMeshes(u32 item);

    const std::vector<scene::Entity>& entitiesOf(u32 item) const;
    bool resident(u32 item) const { return items_.count(item) != 0; }

    const std::vector<world::AnimatedBody>& animatedBodies() const { return animated_; }

    // Shapes whose mesh is no longer loaded are released.
    void releaseUnusedShapes();

    // Content cache prefetch, so a policy never reaches past the loader.
    void prefetchTick();
    void prefetch(u64 meshObjectId, f32 priority);
    bool ready(u64 meshObjectId) const;

private:
    struct ItemState {
        std::vector<scene::Entity> entities;
        std::vector<std::pair<scene::Entity, i32>> bodies;
        std::vector<u64> meshes;
        std::vector<world::AnimatedBody> animated;
    };
    void bindMeshMaterials(u64 meshId);
    void rebuildAnimated();

    GameContent* content_ = nullptr;
    rhi::IDevice* device_ = nullptr;
    world::InstantiateOptions opt_;
    std::unordered_map<u32, ItemState> items_;
    std::vector<world::AnimatedBody> animated_;
    // Shared by every batch; released once the mesh is unloaded.
    std::unordered_map<u64, i32> meshShapes_;
};

} // namespace aver::game

#endif // AVER_MODULE_SCENE
