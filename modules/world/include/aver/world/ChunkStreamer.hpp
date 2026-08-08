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

    // How far AHEAD of a moving source to keep resident, in seconds of its own velocity.
    //
    // THE RADIUS ALONE IS NOT ENOUGH FOR A MOVING SOURCE, and the arithmetic says why: a source at
    // v cm/s with a radius of R chunks has R*chunkSizeCm/v seconds of loaded space in front of it,
    // and the loader needs that long to fill the next ring at `loadBudget` chunks per frame. Past
    // some speed it does not, and the source arrives somewhere that has not loaded. Leading the
    // residency along the velocity buys back exactly the time the budget needs.
    f32 leadSeconds = 1.5f;
};

// One thing the world stays loaded around. The camera is just the first of them -- an actor is the
// same shape, which is the whole reason this is a struct rather than a Vec3.
struct StreamSource {
    Vec3 positionCm;
    // Centimetres per second. Zero for anything stationary, and for the camera unless a host
    // bothers to differentiate it.
    Vec3 velocityCmPerSec{0, 0, 0};
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

    // What to stay resident around.
    void setSources(const std::vector<StreamSource>& sources) { sources_ = sources; }
    // Stationary convenience, for a caller that has only positions.
    void setSources(const std::vector<Vec3>& positions) {
        sources_.clear();
        sources_.reserve(positions.size());
        for (const Vec3& p : positions) sources_.push_back(StreamSource{p});
    }

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
    // Whether the chunk containing a world position is loaded.
    bool isResidentAt(const Vec3& worldCm) const {
        return isResident(splitCm(worldCm, settings_.chunkSizeCm).chunk);
    }

    // Where a mover is ALLOWED to end up this step, given what is loaded.
    //
    // THE BOUNDARY HOLD. When the loader has not kept up, something has to give, and the three
    // options are not equal: a synchronous load means a frame hitch of unbounded size; letting the
    // mover through means it stands on a chunk that does not exist, which for a character is a fall
    // through the world. Holding it at the last loaded chunk is the only one that is recoverable and
    // the only one that is visible in a test.
    //
    // Returns `to` when the destination is resident. Otherwise returns the furthest point along
    // from->to that still is, backed off by `marginCm` so the caller does not sit exactly on the
    // face and re-trigger this every frame. Returns `from` when even that is not resident -- which
    // means the mover is already somewhere unloaded and moving it further cannot help.
    Vec3 clampToResident(const Vec3& from, const Vec3& to, f32 marginCm = 1.0f) const;
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
    // Where a source's residency is centred: its position, plus its velocity over `leadSeconds`.
    // One entry per source, rebuilt each update so a stale velocity never lingers.
    std::vector<Vec3> anchors_;
    void rebuildAnchors();

    StreamSettings settings_;
    IChunkSource* source_ = nullptr;
    RestoreOptions restore_;
    std::vector<StreamSource> sources_;
    std::unordered_map<ChunkCoord, Resident> resident_;
    StreamStats stats_;
};

} // namespace aver::world

#endif // AVER_MODULE_SCENE
