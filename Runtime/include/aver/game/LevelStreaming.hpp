// Streams an authored level's placements by distance from the viewer (docs/LEVEL_STREAMING.md).
// Shared by the runtime and the editor: owns the placement records (the level's source of truth while
// it streams), the residency policy, mesh acquire/release and the filtered instantiate.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcWorld.hpp"

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"
#  include "aver/world/LevelInstance.hpp"
#  include "aver/world/PlacementStreamer.hpp"

namespace aver::rhi { class IDevice; }

namespace aver::game {

class GameContent;

class LevelStreaming {
public:
    struct Hooks {
        // After a batch is instantiated: the new entities, the placement each came from and its body (-1 none).
        std::function<void(const std::vector<scene::Entity>&, const std::vector<u32>&, const std::vector<i32>&)> afterLoad;
        // Before an entity is destroyed by eviction; `writeBack` false when its live state must not be
        // kept (reset after Play). The editor writes the record back here.
        std::function<void(scene::Entity, u32 placement, bool writeBack)> beforeEvict;
    };

    // Takes the level's placements as the record set. `opt` is what a full load would pass to
    // world::instantiate (materials, ground, bounds, triangles); its progress callback is ignored.
    // Class placements are left to the host (never streamed).
    void begin(const fmt::OcWorldData& w, GameContent& content, rhi::IDevice& device,
               const world::InstantiateOptions& opt, Hooks hooks = {});
    // Destroys everything this streamed in and releases its meshes.
    void end(scene::World& world);
    bool active() const { return active_; }

    // One step: loads and evicts around `viewers` (world cm). Meshes are read on worker threads first;
    // loads run nearest first within a per-tick time budget, larger while something close is missing.
    void tick(scene::World& world, const std::vector<Vec3>& viewers);

    // ---- records (the editor's view) ----
    const std::vector<fmt::OcWorldPlacement>& placements() const { return records_; }
    bool removed(u32 placement) const { return placement < removed_.size() && removed_[placement]; }
    // Replaces a record (live edit write-back) and refreshes its root's bounds from mesh bounds. The
    // record keeps its parent unless `keepParent` is false (a reparented entity saved at its world pose).
    void updateRecord(u32 placement, fmt::OcWorldPlacement p, bool keepParent = true);
    const fmt::OcWorldPlacement* recordOf(scene::Entity e) const;
    // Unloads the items holding these entities without writing them back; they reload from their
    // records as residency asks (what Play moved goes back to where the level put it).
    void reload(scene::World& world, const std::vector<scene::Entity>& es);
    // A new placement whose entity the host already created; returns its index. Resident and pinned
    // until it is saved and unpinned by the host.
    u32 addRecord(const fmt::OcWorldPlacement& p, scene::Entity e);
    // Deletes a record (the host destroyed its entity). Children's records are removed with it.
    void removeRecord(u32 placement);

    // ---- entity mapping ----
    bool owns(scene::Entity e) const { return placementOf_.count(e) != 0; }
    i32 placementOf(scene::Entity e) const;
    scene::Entity entityOf(u32 placement) const;
    // Keeps the root item holding this placement loaded (selection, gizmo, undo).
    void pin(u32 placement, bool on);
    void unpinAll();

    // Kinematic bodies of resident animated placements, for world::driveKinematicBodies.
    const std::vector<world::AnimatedBody>& animatedBodies() const { return animated_; }

    usize residentRoots() const { return streamer_.residentCount(); }
    usize rootCount() const { return rootPlacements_.size(); }
    usize residentEntities() const { return placementOf_.size(); }
    // What the last tick() did: roots loaded and evicted.
    u32 lastLoaded() const { return lastLoaded_; }
    u32 lastEvicted() const { return lastEvicted_; }

private:
    struct Item {
        std::vector<u32> placements;   // root first, then descendants, parents before children
        std::vector<scene::Entity> entities;
        std::vector<std::pair<scene::Entity, i32>> bodies;
        std::vector<u64> meshes;       // acquired while resident
        bool loaded = false;
    };
    world::Aabb itemBounds(const Item& it) const;
    bool worldBounds(u32 placement, Vec3& outMin, Vec3& outMax) const;
    void loadItems(scene::World& world, const std::vector<u32>& items);
    void evictItem(scene::World& world, u32 item, bool writeBack = true);
    void bindMeshMaterials(u64 meshId);
    void rebuildAnimated();
    void releaseUnusedShapes();

    // Load time per tick: kNearLoadMs while the nearest missing root is within a quarter of the load
    // distance, falling to kFarLoadMs at three quarters.
    static constexpr f64 kNearLoadMs = 25.0, kFarLoadMs = 4.0;

    bool active_ = false;
    u32 lastLoaded_ = 0, lastEvicted_ = 0;
    GameContent* content_ = nullptr;
    rhi::IDevice* device_ = nullptr;
    world::InstantiateOptions opt_;
    Hooks hooks_;
    std::vector<fmt::OcWorldPlacement> records_;
    std::vector<u8> removed_;
    std::vector<u32> itemOf_;            // placement -> item
    std::vector<u32> rootPlacements_;    // item -> root placement
    std::vector<Item> items_;
    world::PlacementStreamer streamer_;
    std::unordered_map<u32, u32> placementOf_;          // entity raw -> placement
    std::unordered_map<u32, scene::Entity> entityOf_;   // placement -> entity
    std::unordered_map<u32, std::vector<world::AnimatedBody>> animatedByItem_;
    std::vector<world::AnimatedBody> animated_;
    std::unordered_map<u64, bool> materialsBound_;
    // Physics shapes by mesh id, shared by every batch; released once the mesh is unloaded.
    std::unordered_map<u64, i32> meshShapes_;
};

} // namespace aver::game

#endif // AVER_MODULE_SCENE
