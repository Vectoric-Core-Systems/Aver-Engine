// Streams a cooked authored level: region files under <project>/Binaries/Chunks/<level>/ feed the chunk streamer, and an
// ItemLoader turns each chunk into entities. Level-global records stay in the slim level.ocworld, loaded flat by GameLevel.
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
        // Each chunk's entities, their cook placement ids (PLID) and bodies (-1 none).
        std::function<void(const std::vector<scene::Entity>&, const std::vector<u64>&, const std::vector<i32>&)> afterLoad;
    };

    struct Choice {
        bool use = false;
        bool stale = false;      // a set exists but the source changed since the cook
        std::string dir;
        std::string why;         // when !use: no set, damaged, or stale
        fmt::OcWorldData slim;   // when use: load THIS flat; streaming is switched off in it
    };
    // Streams only if the set opens and its source hash matches the level as it is now. Never serve a stale set.
    static Choice resolve(const std::string& projectDir, const std::string& contentDir, const std::string& chunksPath,
                          const std::string& levelAsset, const fmt::OcWorldData& source);

    // `stream` distances become radii in chunks. False with `why` when the set does not open.
    bool begin(const std::string& cookDir, GameContent& content, rhi::IDevice& device, const world::InstantiateOptions& opt,
               const fmt::OcStreamSettings& stream, Hooks hooks = {}, std::string* why = nullptr);
    // Destroys everything streamed in and releases its meshes.
    void end(scene::World& world);
    bool active() const { return active_; }

    // Viewer positions are scene-frame cm; the first is the camera, whose velocity is differenced here.
    // Mesh reads run on workers; GPU work runs nearest first within the millisecond budgets. Call before World::flush.
    void tick(scene::World& world, const std::vector<Vec3>& viewers);
    // After a teleport: the next tick reports zero velocity.
    void resetVelocityTracking() { haveLastPos_ = false; }

    // Keeps a chunk resident wherever the viewers are.
    void pin(const world::ChunkCoord& c, bool on = true);

    // Chunk keys are absolute, so a shift loads and evicts nothing. Called by the streaming origin participant (order 50).
    void onOriginShift(const world::ShiftEvent& ev);
    // Level load or teleport without a shift (WorldOrigin::reset).
    void setOrigin(world::OriginCm o) { origin_ = o; haveLastPos_ = false; }
    world::OriginCm origin() const { return origin_; }

    // True when a resident chunk made `e`: save skips it and the editor cannot select it.
    bool owns(scene::Entity e) const;

    // Kinematic bodies of resident animated placements, for world::driveKinematicBodies.
    const std::vector<world::AnimatedBody>& animatedBodies() const { return loader_.animatedBodies(); }

    usize residentChunks() const { return world_.streamer().residentCount(); }
    usize residentEntities() const { return world_.streamedEntities().size(); }
    usize wideItems() const { return world_.streamer().wide().size(); }
    const world::StreamStats& stats() const { return stats_; }
    const world::ChunkWorld& chunkWorld() const { return world_; }
    // Decoded, still waiting for meshes.
    usize waitingChunks() const { return world_.streamer().pendingPrepareCount(); }

private:
    class Host;
    struct Prepared {
        std::vector<fmt::OcWorldPlacement> recs;   // f64 absolute for roots, parent-relative for children
        std::vector<u64> plids;                    // parallel: the cook's placement ids
        std::vector<u64> meshes;
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
    std::unordered_map<world::ChunkCoord, u32> itemOf_;   // resident chunk -> its ItemLoader unit
    u32 nextItem_ = 1;
    std::vector<Vec3> viewers_;
    world::StreamStats stats_;
};

} // namespace aver::game

#endif // AVER_MODULE_SCENE
