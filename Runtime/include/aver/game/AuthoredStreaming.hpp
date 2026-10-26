// Streaming of a COOKED authored level (docs/plans/wave11/LARGE_WORLD.md 2.4.6): the level's placements live in region files
// under <project>/Binaries/Chunks/<level>/, the chunk streamer keeps the chunks around the viewer resident, and an authored
// host turns each decoded chunk into entities through game::ItemLoader (collision, animators, slot materials, COMPONENT
// records exactly as the flat path makes them).
//
// WHAT IT OWNS: a world::ChunkWorld over the cooked set (no generator), an ItemLoader, and the host between them. WHAT IT DOES NOT:
// the level-global records (lights, water, classes, sequence targets, ...) -- those are the slim level.ocworld the cook wrote, which
// GameLevel loads whole through the flat path before this starts.
//
// THE CHOICE (resolve): a level whose STREAM header names `chunks=<dir>` streams from it when the set opens, is a cooked set and
// its source hash matches the level as it is now; otherwise (no set, damaged, stale) the level streams the old way and one line
// says why. Never serve a stale set.
#pragma once
#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/formats/OcWorld.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#if AVER_MODULE_SCENE
#  include "aver/game/ItemLoader.hpp"
#  include "aver/scene/World.hpp"
#  include "aver/world/ChunkWorld.hpp"
#  include "aver/world/WorldOrigin.hpp"

namespace aver::rhi { class IDevice; }

namespace aver::game {

class GameContent;

class AuthoredStreaming {
public:
    AuthoredStreaming();
    ~AuthoredStreaming();
    AuthoredStreaming(const AuthoredStreaming&) = delete;
    AuthoredStreaming& operator=(const AuthoredStreaming&) = delete;

    struct Hooks {
        // After a chunk's entities are made: the entities, the placement id each came from (CookPlan's PLID, stable across a
        // re-cook) and its body (-1 none).
        std::function<void(const std::vector<scene::Entity>&, const std::vector<u64>&, const std::vector<i32>&)> afterLoad;
    };

    // ---- the choice ----
    struct Choice {
        bool use = false;        // stream from `dir`
        bool stale = false;      // a set exists but the source moved on since it was cooked
        std::string dir;         // the cook directory
        std::string why;         // when !use: no set, damaged, or stale (empty when the level names none)
        fmt::OcWorldData slim;   // when use: the level without what the chunks carry, streaming switched off; load THIS flat
    };
    // `chunksPath` is the level's STREAM chunks= value (project-relative, or absolute). `source` is the level as authored (the
    // loaded .ocworld), `levelAsset` its content-relative path. Reads the cook's index and slim level; parses nothing else.
    static Choice resolve(const std::string& projectDir, const std::string& contentDir, const std::string& chunksPath,
                          const std::string& levelAsset, const fmt::OcWorldData& source);

    // ---- lifetime ----
    // Opens the cooked set in `cookDir` and starts streaming it. `opt` is what a full load would pass to world::instantiate
    // (materials, ground, bounds, triangles); `stream` is the level's STREAM header (load and evict distances become radii in
    // chunks). False with `why` when the set does not open.
    bool begin(const std::string& cookDir, GameContent& content, rhi::IDevice& device, const world::InstantiateOptions& opt,
               const fmt::OcStreamSettings& stream, Hooks hooks = {}, std::string* why = nullptr);
    // Destroys everything this streamed in and releases its meshes.
    void end(scene::World& world);
    bool active() const { return active_; }

    // One step around `viewers` (scene-frame cm; the first is the camera, whose velocity is differenced here). Mesh reads are
    // made on worker threads; what stays here (GPU buffers, entities, bodies) runs nearest first within the streamer's
    // millisecond budgets. Call once a frame, before World::flush.
    void tick(scene::World& world, const std::vector<Vec3>& viewers);
    // After a teleport the next tick reports zero velocity instead of one huge one.
    void resetVelocityTracking() { haveLastPos_ = false; }

    // Keeps a chunk resident wherever the viewers are (sequences, tooling).
    void pin(const world::ChunkCoord& c, bool on = true);

    // ---- the floating origin ----
    // The camera and every viewer are origin-relative; chunk keys are absolute, so a shift loads and evicts nothing. A shift only
    // has to tell this class the new origin and move the one position it remembers.
    // Called by the origin service's streaming participant (order 50), after the scene and the host members have shifted.
    void onOriginShift(const world::ShiftEvent& ev);
    // Level load / teleport without a shift (WorldOrigin::reset): the origin changes and the velocity history is dropped.
    void setOrigin(world::OriginCm o) { origin_ = o; haveLastPos_ = false; }
    world::OriginCm origin() const { return origin_; }

    // True when `e` was made by a resident chunk: scenery the save path skips and the editor treats as unselectable, the
    // same distinction GameStreaming::owns draws for PCG scatter.
    bool owns(scene::Entity e) const;

    // Kinematic bodies of resident animated placements, for world::driveKinematicBodies.
    const std::vector<world::AnimatedBody>& animatedBodies() const { return loader_.animatedBodies(); }

    // ---- counters ----
    usize residentChunks() const { return world_.streamer().residentCount(); }
    usize residentEntities() const { return world_.streamedEntities().size(); }
    usize wideItems() const { return world_.streamer().wide().size(); }
    const world::StreamStats& stats() const { return stats_; }
    const world::ChunkWorld& chunkWorld() const { return world_; }
    // A chunk decoded, still waiting for its meshes.
    usize waitingChunks() const { return world_.streamer().pendingPrepareCount(); }

private:
    class Host;
    struct Prepared {
        std::vector<fmt::OcWorldPlacement> recs;   // the chunk's placements, f64 absolute for roots, parent-relative for children
        std::vector<u64> plids;                    // parallel: the cook's placement ids
        std::vector<u64> meshes;                   // distinct mesh object ids
        u32 waits = 0;                             // prepare() calls that answered Pending
        u32 stamp = 0;                             // the tick that last asked
    };
    f32 priorityOf(const world::ChunkCoord& c) const;

    bool active_ = false;
    world::OriginCm origin_{};
    Vec3 lastPos_{};
    bool haveLastPos_ = false;
    std::chrono::steady_clock::time_point lastTick_{};
    u32 tickStamp_ = 0;
    Hooks hooks_;
    world::ChunkWorld world_;
    ItemLoader loader_;
    std::unique_ptr<Host> host_;
    std::unordered_map<world::ChunkCoord, Prepared> prepared_;
    std::unordered_map<world::ChunkCoord, u32> itemOf_;   // resident chunk -> the ItemLoader unit holding its entities
    u32 nextItem_ = 1;
    std::vector<Vec3> viewers_;
    world::StreamStats stats_;
};

} // namespace aver::game

#endif // AVER_MODULE_SCENE
