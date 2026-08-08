// Which chunks are resident, and the loading and unloading that keeps it that way.
//
// SYNCHRONOUS, ON THE FRAME THREAD, AND MEASURED. That is a decision, not a first draft.
// scene::World is a process-global singleton whose own header says "not thread-safe" (World.hpp:15),
// every framework side table is a process-global mutable static, and there is no job system to post
// to -- Jolt's pool is linked PRIVATE inside Aver.Physics by explicit design. So a background thread
// may not create an entity, and the honest version of slice 6 is a synchronous loader with a BUDGET,
// plus the numbers to decide whether a worker is worth building. docs/CHUNKS.md B4 says exactly this.
//
// HYSTERESIS IS NOT A REFINEMENT. With one radius, a camera sitting on the boundary loads a chunk,
// steps a centimetre, evicts it, steps back, loads it again -- forever, at whatever the frame rate
// is. Eviction has to happen further out than loading, and the gap has to be wider than the jitter.
#pragma once

#include "aver/core/Types.hpp"
#include "aver/core/Math.hpp"
#include "aver/world/BodyRegistry.hpp"
#include "aver/world/ChunkCoord.hpp"
#include "aver/world/ChunkPayload.hpp"
#include "aver/world/ChunkSource.hpp"

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"

#  include <functional>
#  include <string>
#  include <unordered_map>
#  include <vector>

namespace aver::world {

struct StreamSettings {
    i32 chunkSizeCm = kDefaultChunkSizeCm;

    // Chebyshev distance in CHUNKS. A cube rather than a sphere, because the thing being loaded is
    // a cube and a spherical radius just makes the corners inconsistent.
    i32 loadRadius = 3;
    // Must exceed loadRadius, or the boundary thrashes. Enforced in setSettings rather than trusted.
    i32 evictRadius = 5;

    // The only thing bounding the frame hitch, since the load is synchronous. Zero means unbounded,
    // which is what a "load everything now" test wants and no running game should use.
    u32 loadBudget = 2;
    u32 evictBudget = 4;

    // Vertical range, in chunks, around each source. A surface world wants far less height than
    // width, and loading a 7x7x7 cube where a 7x7x3 slab would do is 3x the work for nothing.
    i32 verticalRadius = 1;
};

struct StreamStats {
    u32 residentChunks = 0;
    u32 residentEntities = 0;
    u32 loadedThisUpdate = 0;
    u32 evictedThisUpdate = 0;
    u32 entitiesIn = 0;
    u32 entitiesOut = 0;
    u32 pendingLoads = 0;      // wanted, not yet resident -- what the budget is holding back
    u32 failedLoads = 0;       // present in the source and refused (damage, staleness)
    f64 lastLoadMs = 0.0;      // wall time of the most recent chunk materialisation
    f64 totalLoadMs = 0.0;
    u32 totalLoads = 0;
};

// Streams chunks around a set of world-space sources. Camera only in slice 6; an actor is just
// another source, which is why this takes a list rather than one position.
class ChunkStreamer {
public:
    void setSettings(const StreamSettings& s);
    const StreamSettings& settings() const { return settings_; }

    // The source is not owned; it must outlive the streamer.
    void setSource(IChunkSource* src) { source_ = src; }

    // Called for a restored entity's material and body, exactly as RestoreOptions does -- the host
    // owns both, this owns only the timing.
    RestoreOptions& restoreOptions() { return restore_; }

    // World-space positions to stay resident around.
    void setSources(const std::vector<Vec3>& positions) { sources_ = positions; }

    // One step. Loads up to `loadBudget` and evicts up to `evictBudget`, nearest first.
    //
    // Bodies belonging to evicted entities are APPENDED to `freedBodies` rather than removed here:
    // this module links Aver.Scene and not Aver.Physics, deliberately, so a build without physics
    // still has a streamer that compiles. A caller that passes null is saying it has no physics --
    // not that the bodies may be leaked.
    StreamStats update(scene::World& w, BodyRegistry& bodies, std::vector<i32>* freedBodies = nullptr);

    // Drops everything, returning the bodies whose removal is the caller's business.
    void unloadAll(scene::World& w, BodyRegistry& bodies, std::vector<i32>& freedBodies);

    bool isResident(const ChunkCoord& c) const { return resident_.find(c) != resident_.end(); }
    usize residentCount() const { return resident_.size(); }
    // Every entity this streamer created for `c`, or an empty span if it is not resident.
    const std::vector<scene::Entity>* entitiesOf(const ChunkCoord& c) const;

    const StreamStats& stats() const { return stats_; }

private:
    struct Resident {
        std::vector<scene::Entity> entities;
    };
    // Chebyshev distance in chunks from the nearest source, or a large number when there are none.
    i32 distanceToNearestSource(const ChunkCoord& c) const;

    StreamSettings settings_;
    IChunkSource* source_ = nullptr;
    RestoreOptions restore_;
    std::vector<Vec3> sources_;
    std::unordered_map<ChunkCoord, Resident> resident_;
    StreamStats stats_;
};

} // namespace aver::world

#endif // AVER_MODULE_SCENE
