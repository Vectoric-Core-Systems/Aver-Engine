// Level instances at run time: realises the LEVELINST records of a level (and of the levels they place, to a depth
// of 8) as entities under an instance root, streams them as units, tracks overrides as a diff against the child
// asset, and feeds packed instances to the foliage path. docs/LEVEL_INSTANCES.md section 2.5.
//
// SHARED BY THE EDITOR AND THE PACKAGED GAME, like LevelStreaming. It depends on no framework, prefab, GPU or
// GameContent type: everything host-specific arrives through ContentPort / Hooks / PackedSink, so a test runs it
// with a fake host.
//
// NAMING TRAP: world::LevelInstance is the RESULT of world::instantiate(). It has nothing to do with this class.
#pragma once
#include "aver/core/Math.hpp"
#include "aver/core/Types.hpp"
#include "aver/formats/OcInstances.hpp"
#include "aver/formats/OcLevelInst.hpp"
#include "aver/formats/OcWorld.hpp"

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#if AVER_MODULE_SCENE
#  include "aver/scene/LevelInstanceLink.hpp"
#  include "aver/scene/World.hpp"
#  include "aver/world/LevelAssetLibrary.hpp"
#  include "aver/world/LevelInstance.hpp"
#  include "aver/world/LevelPack.hpp"
#  include "aver/world/PlacementStreamer.hpp"

namespace aver::game {

// The content the host provides, behind std::function so tests fake it. A mesh is acquired before the entities that
// show it exist and released when they go; `ready` says the read is finished (a unit loads only when every mesh it
// needs is ready).
struct ContentPort {
    std::function<bool(u64 mesh)> acquire;  std::function<void(u64 mesh)> release;
    std::function<void(u64 mesh)> prefetch; std::function<bool(u64 mesh)> ready;
    std::function<i32(u64 mesh)> takeShape;   // the mesh's prefetched physics shape, now the caller's; 0 when none
    std::function<bool()> buildMaterial;      // builds one material the acquires made; false when none wait
    std::string contentDir;   // absolute; child FOLIAGE tables and commits resolve against it
    std::string projectDir;   // absolute; Binaries/LevelPacks lives under it
};

class LevelInstances {
public:
    struct Hooks {
        std::function<void(scene::Entity, i32 body, const fmt::OcWorldPlacement*)> created;   // editor bookkeeping; null placement for a light/decal/prefab root
        std::function<void(scene::Entity)> destroying;
        std::function<scene::Entity(const fmt::OcPrefabInstance&, const Transform& local, scene::Entity parent)> spawnPrefab;
        std::function<scene::Entity(const fmt::OcWorldPlacement&, const Transform& world, scene::Entity parent)> spawnClass;
        std::function<void(scene::Entity)> destroyClass;                 // the aver_fw_destroy path, so OnEndPlay runs
        std::function<bool(scene::Entity, fmt::OcWorldData& into, i32 parentIndex)> captureEntity;   // editor: placement/light/decal record from a live entity (commit)
        std::function<std::vector<fmt::OcPrefabInstance>(scene::Entity root)> capturePrefabs;
    };
    // game::LevelFoliage in the hosts; a fake in tests. `owner` is unique per (instance, kind).
    struct PackedSink {
        std::function<void(u64 owner, fmt::OcInstanceData rowsInWorldSpace, std::vector<std::string> groupMaterial)> set;
        std::function<void(u64 owner)> clear;
    };
    // Value type, mirrors prefab::PrefabState: by instance id, enough to recreate an instance after an undo.
    struct LevelInstanceState {
        struct Item {
            u32 instanceId = 0;
            bool present = true;              // false: this state has no such instance
            fmt::OcLevelInstance rec;         // transform and overrides as of capture
            scene::Entity parent = scene::kInvalidEntity;
        };
        std::vector<Item> items;
    };

    LevelInstances();
    ~LevelInstances();
    LevelInstances(const LevelInstances&) = delete;
    LevelInstances& operator=(const LevelInstances&) = delete;

    // Residency callback: (root, loaded, entity count). The host relays it to aver_fw_event_post (2.5.3 step 11).
    std::function<void(scene::Entity, bool, u32)> onResidency;

    void init(world::LevelAssetLibrary& lib, scene::World& w);
    // Realises every LEVELINST of `level` (eager, or as streamed units, 2.6). `opt` is what a full load passes to
    // world::instantiate (materials, bounds, triangles, meshShapes); progress is ignored.
    void begin(const fmt::OcWorldData& level, const ContentPort&, const world::InstantiateOptions& opt,
               const world::PlacementStreamSettings* levelStream, Hooks, PackedSink);
    void end();                                                  // unrealise everything, release meshes and bodies
    void tick(const std::vector<Vec3>& viewers);                 // streaming, deferred rebuilds, once a frame
    void spawnPendingClasses();                                  // the late class pass (GameLevel::spawnClassPlacements calls it)
    void rescan();                                               // rebuild the table from CLevelInstanceLink after a save restore

    // ---- run time / script ----
    scene::Entity spawn(const fmt::OcLevelInstance&, scene::Entity parent = scene::kInvalidEntity, bool runtimeOnly = true);
    bool destroy(scene::Entity root);
    scene::Entity rootOf(scene::Entity e) const;                 // kInvalidEntity when not a member
    bool isMember(scene::Entity e) const;
    scene::Entity topRootOf(scene::Entity e) const;              // the outermost instance root (a plain click picks this)
    scene::Entity find(scene::Entity root, std::string_view idPath) const;
    bool setLoaded(scene::Entity root, bool loaded);             // manual residency; pins while loaded by hand
    bool loaded(scene::Entity root) const;
    bool reload(scene::Entity root);                             // re-realise from the library, overrides kept
    bool reloadAsset(const std::string& path);                   // every instance of it, nested ones too
    bool setPacked(scene::Entity root, bool packed);             // true when the effective state now equals the request
    bool bounds(scene::Entity root, AABB& out) const;
    u32 count() const;  scene::Entity at(u32 i) const;
    // ---- editor ----
    bool beginEdit(scene::Entity root);  bool commitEdit(scene::Entity root, std::string* why);  bool discardEdit(scene::Entity root);
    bool editing(scene::Entity root) const;  bool anyDirtyEdit() const;
    bool computeOverrides(scene::Entity root, std::vector<fmt::OcPrefabOverride>& out) const;   // live fields vs the child's records
    bool setOverrides(scene::Entity root, const std::vector<fmt::OcPrefabOverride>& ov);
    bool revertOverride(scene::Entity root, const fmt::OcPrefabOverride& which);
    bool revertAll(scene::Entity root);
    std::vector<fmt::OcLevelInstance> captureRecords(const std::function<bool(scene::Entity)>& include = {}) const;   // saveLevel
    scene::Entity createFromRecord(const fmt::OcLevelInstance&, scene::Entity parent, std::string* why);
    bool breakInstance(scene::Entity root);                      // members become ordinary entities, reparented to root's parent at their world pose
    LevelInstanceState captureState(const std::vector<u32>& instanceIds);   // undo
    bool restoreState(const LevelInstanceState&);
    void pin(scene::Entity root, bool on);
    const std::vector<world::AnimatedBody>& animatedBodies() const;
    bool extentBounds(Vec3& lo, Vec3& hi) const;                 // union of known instance bounds, for GameLevel::placementBounds
    void setPackedRenderOk(bool ok);                             // host: rtRenderMode is ray-driven primary (2.8.4)
    struct Stats { u32 instances, loaded, entities, bodies, packedRows; };  Stats stats() const;

    // ---- small accessors the editor and the ABI use ----
    u32 instanceIdOf(scene::Entity root) const;                  // 0 when not an instance root
    scene::Entity rootOfId(u32 instanceId) const;
    const fmt::OcLevelInstance* recordOf(scene::Entity root) const;
    bool setRecordName(scene::Entity root, const std::string& name);
    bool setRecordStream(scene::Entity root, i32 stream);        // -1 follow the host level, 0 resident, 1 streamed
    u32 assetCount(scene::Entity root) const;                    // placements in the child asset (the Details warning above 5000)
    std::string assetOf(scene::Entity root) const;
    u64 assetIdOf(scene::Entity root) const;
    // Why a packed instance is (or is not) showing its pack: "packed", "no pack", "pack stale", "unpacked: renderer",
    // "unpacked: overrides", "unpacked: editing", or "" for an unpacked one.
    std::string packState(scene::Entity root) const;
    bool isRuntimeOnly(scene::Entity root) const;
    bool isNested(scene::Entity root) const;
    // The editor's per-member collide flag (a Details checkbox) feeds the override diff.
    void setMemberCollide(scene::Entity member, bool collide);

private:
    struct Impl;
    std::unique_ptr<Impl> d_;
    world::LevelAssetLibrary* lib_ = nullptr;
    scene::World* world_ = nullptr;
    ContentPort port_;
    bool classesReady_ = false;
};

} // namespace aver::game

#endif // AVER_MODULE_SCENE
