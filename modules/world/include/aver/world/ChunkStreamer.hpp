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
#include "aver/world/AuthoredChunkSource.hpp"
#include "aver/world/BodyRegistry.hpp"
#include "aver/world/ChunkCoord.hpp"
#include "aver/world/ChunkHost.hpp"
#include "aver/world/ChunkPayload.hpp"
#include "aver/world/ChunkSource.hpp"
#include "aver/world/RegionIndex.hpp"
#include "aver/world/WorldPos.hpp"

#if AVER_MODULE_SCENE
#  include "aver/scene/World.hpp"

#  include <cmath>
#  include <functional>
#  include <string>
#  include <unordered_map>
#  include <unordered_set>
#  include <vector>

namespace aver::world {

// StreamSettings::verticalRadius for "every layer": the wanted set is the occupied chunks of each source's XY square, found with
// IChunkSource::forEachOccupied, instead of a (2R+1)^2 x (2V+1) cube. A tower whose floors span chunk layers -1..31 is found
// without a Z slab. Only a source that answers forEachOccupied cheaply (RegionChunkSource, AuthoredChunkSource) should be used so.
inline constexpr i32 kAllLayers = -1;

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
    // kAllLayers (-1) means every layer that holds something.
    i32 verticalRadius = 1;

    // ---- authored (cooked) levels, LARGE_WORLD 2.4.6. Every default below leaves the PCG behaviour exactly as it was. ----
    //
    // Wall-clock budgets in milliseconds, beside the counts above (0 = none). Spent in whole chunks, and at least one chunk is
    // attempted per update whatever they say, so a budget slows a load down and never stops it. The NEAR load budget applies
    // while the nearest missing chunk is within a quarter of the load radius, the FAR one past three quarters, linearly between
    // (LevelStreaming's rule); a far budget of 0 means "the same as near".
    f64 loadBudgetMs = 0.0;
    f64 loadBudgetFarMs = 0.0;
    f64 evictBudgetMs = 0.0;
    // At most this many entities restored per update, checked after each chunk (one chunk can overshoot it); 0 = no cap.
    u32 loadEntityCap = 0;
    // Added to both radii, so a chunk-owned placement that reaches into the load area from its owner chunk (up to
    // kAuthoredPadChunks chunks away) has its owner loaded. 0 for PCG scatter.
    i32 padChunks = 0;

    // How far AHEAD of a moving source to keep resident, in seconds of its own velocity.
    //
    // THE RADIUS ALONE IS NOT ENOUGH FOR A MOVING SOURCE, and the arithmetic says why: a source at
    // v cm/s with a radius of R chunks has R*chunkSizeCm/v seconds of loaded space in front of it,
    // and the loader needs that long to fill the next ring at `loadBudget` chunks per frame. Past
    // some speed it does not, and the source arrives somewhere that has not loaded. Leading the
    // residency along the velocity buys back exactly the time the budget needs.
    f32 leadSeconds = 1.5f;

    // A source farther than this from the floating origin on X or Y is not streamed (one warning): the resident window is one
    // f32 frame, and past the sub-millimetre cliff (kSubMmCliffCm) a position is no longer exact. 0 keeps the pre-origin
    // behaviour -- every source streams wherever it is -- which is the default; a host that runs the floating origin
    // sets kSubMmCliffCm.
    i64 sourceLimitCm = 0;
};

// One thing the world stays loaded around. The camera is just the first of them -- an actor is the
// same shape, which is the whole reason this is a struct rather than a Vec3.
struct StreamSource {
    // ORIGIN-RELATIVE f32 centimetres (the scene frame). With a zero origin that is the absolute position, as it always was.
    Vec3 positionCm;
    // Centimetres per second. Zero for anything stationary, and for the camera unless a host
    // bothers to differentiate it.
    Vec3 velocityCmPerSec{0, 0, 0};
    // The WorldPos form: when `hasWorld`, `world` is the source's exact position and names its chunk directly, with no f64 sum
    // on the way; positionCm is then only its origin-relative shadow, used for the look-ahead corridor. Build one with at().
    bool     hasWorld = false;
    WorldPos world{};

    static StreamSource at(const WorldPos& p, OriginCm o, const Vec3& velocityCmPerSec = Vec3{0, 0, 0}) {
        StreamSource s;
        s.positionCm = localOf(p, o);
        s.velocityCmPerSec = velocityCmPerSec;
        s.hasWorld = true;
        s.world = p;
        return s;
    }
};

struct StreamStats {
    u32 residentChunks = 0;
    u32 residentEntities = 0;
    u32 loadedThisUpdate = 0;
    u32 evictedThisUpdate = 0;
    u32 entitiesIn = 0;
    u32 entitiesOut = 0;
    u32 pendingLoads = 0;      // wanted, not yet resident -- what the budget is holding back
    u32 preparePending = 0;    // of those, chunks decoded and waiting for their host (IChunkHost::prepare said Pending)
    u32 failedLoads = 0;       // present in the source and refused (damage, staleness, the host's Failed)
    u32 skippedSources = 0;    // sources past StreamSettings::sourceLimitCm this update
    f64 lastLoadMs = 0.0;      // wall time of the most recent chunk materialisation
    f64 totalLoadMs = 0.0;
    u32 totalLoads = 0;
};

// The settings a cooked authored level streams with: whole columns, the fixed pad, and the millisecond budgets LevelStreaming has
// (12 ms near, 3 ms far, 2 ms evicting, 128 entities a tick). `loadCm` / `evictCm` are the level's STREAM distances (cm); they
// become radii in chunks, rounded up, with the eviction radius always past the load radius.
inline StreamSettings authoredStreamSettings(i32 chunkSizeCm, f32 loadCm, f32 evictCm) {
    StreamSettings s;
    s.chunkSizeCm = chunkSizeCm;
    const f32 size = static_cast<f32>(chunkSizeCm > 0 ? chunkSizeCm : kDefaultChunkSizeCm);
    s.loadRadius = static_cast<i32>(std::ceil(loadCm / size));
    if (s.loadRadius < 1) s.loadRadius = 1;
    s.evictRadius = static_cast<i32>(std::ceil(evictCm / size));
    if (s.evictRadius <= s.loadRadius) s.evictRadius = s.loadRadius + 1;
    s.loadBudget = 0;
    s.evictBudget = 0;
    s.verticalRadius = kAllLayers;
    s.padChunks = kAuthoredPadChunks;
    s.loadBudgetMs = 12.0;
    s.loadBudgetFarMs = 3.0;
    s.evictBudgetMs = 2.0;
    s.loadEntityCap = 128;
    return s;
}

// Streams chunks around a set of world-space sources. Camera only in slice 6; an actor is just
// another source, which is why this takes a list rather than one position.
class ChunkStreamer {
public:
    void setSettings(const StreamSettings& s);
    const StreamSettings& settings() const { return settings_; }

    // The source is not owned; it must outlive the streamer.
    void setSource(IChunkSource* src) { source_ = src; }

    // What makes a decoded chunk into entities and takes it away again. Not owned. Null (the default) is ScatterChunkHost: today's
    // behaviour. Set it before the first update; changing it with chunks resident would evict them through the wrong host.
    void setHost(IChunkHost* host) { host_ = host; }
    IChunkHost* host() const { return host_; }

    // Oversize items (RegionIndex::wide): a wide item's OWNER chunk is wanted while the item's box is within the load distance
    // (loadRadius chunks) of any source, and kept while it is within the evict distance, however far the owner's own cell is.
    void setWide(const std::vector<WideItem>& wide) { wide_ = wide; haveLastScan_ = false; }
    const std::vector<WideItem>& wide() const { return wide_; }

    // Keeps a chunk resident wherever the sources are (sequences, tooling). A pinned chunk loads first, is never evicted, and
    // stays resident-and-empty when the source has nothing for it. Unpinning lets the usual distance rule take it again.
    void pin(const ChunkCoord& c, bool on = true) {
        if (on) pinned_.insert(c); else pinned_.erase(c);
        haveLastScan_ = false;
    }
    bool pinned(const ChunkCoord& c) const { return pinned_.find(c) != pinned_.end(); }

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

    // One step. Loads up to `loadBudget` (and within `loadBudgetMs`, and `loadEntityCap` entities) and evicts up to `evictBudget`
    // (within `evictBudgetMs`), nearest first.
    //
    // `origin` is the floating origin the scene is in; the sources are origin-relative (see StreamSource). Chunk keys, the
    // anchors and the idle early-out are all ABSOLUTE chunk coordinates, so a rebase between two updates changes nothing the
    // streamer remembers: no chunk loads or evicts because of a shift. A restored root is placed at chunk origin - origin.
    // Zero is the pre-origin behaviour, bit for bit.
    //
    // Bodies belonging to evicted entities are APPENDED to `freedBodies` rather than removed here:
    // this module links Aver.Scene and not Aver.Physics, deliberately, so a build without physics
    // still has a streamer that compiles. A caller that passes null is saying it has no physics --
    // not that the bodies may be leaked.
    StreamStats update(scene::World& w, BodyRegistry& bodies, std::vector<i32>* freedBodies = nullptr,
                       OriginCm origin = {});

    // Drops everything, returning the bodies whose removal is the caller's business.
    void unloadAll(scene::World& w, BodyRegistry& bodies, std::vector<i32>& freedBodies);

    bool isResident(const ChunkCoord& c) const { return resident_.find(c) != resident_.end(); }
    // Whether the chunk containing a position is loaded. The Vec3 is origin-relative, against the origin of the last update().
    bool isResidentAt(const Vec3& worldCm) const {
        return isResident(chunkAtLocal(worldCm, origin_, settings_.chunkSizeCm));
    }
    bool isResidentAt(const WorldPos& p) const { return isResident(p.chunk); }
    // The origin the last update() ran against (zero before the first).
    OriginCm origin() const { return origin_; }

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
    // Every chunk currently resident, for a host that needs to enumerate rather than query one
    // coordinate at a time -- e.g. ChunkWorld building the "which entities are mine" answer an
    // outliner or a save path needs. There was no such enumerator before this; querying one
    // coordinate at a time is useless to a caller that does not already know which coordinates to
    // ask about.
    std::vector<ChunkCoord> residentChunks() const {
        std::vector<ChunkCoord> out;
        out.reserve(resident_.size());
        for (const auto& kv : resident_) out.push_back(kv.first);
        return out;
    }
    // Chunks decoded and held back by the host's prepare gate.
    usize pendingPrepareCount() const { return pending_.size(); }

    const StreamStats& stats() const { return stats_; }

private:
    struct Resident {
        std::vector<scene::Entity> entities;
    };
    // A payload the host said Pending for, held until it says Ready (or the chunk is no longer wanted). Bounded.
    struct PendingChunk {
        ChunkPayload payload;
        u32 stamp = 0;
    };
    static constexpr usize kMaxPendingChunks = 64;

    static ChunkCoord chunkAtLocal(const Vec3& p, OriginCm o, i32 chunkSizeCm) {
        return splitCm(static_cast<f64>(p.x) + static_cast<f64>(o.x), static_cast<f64>(p.y) + static_cast<f64>(o.y),
                       static_cast<f64>(p.z), chunkSizeCm).chunk;
    }
    // Chebyshev distance in chunks from the nearest source, or a large number when there are none.
    i32 distanceToNearestSource(const ChunkCoord& c) const;
    // XY distance (cm) from the nearest anchor position to a wide item's box, 0 inside it; huge when there are no anchors.
    f64 wideDistanceCm(const WideItem& w) const;
    // Where a source's residency is centred: its position, plus its velocity over `leadSeconds`, as ABSOLUTE chunk
    // coordinates. One entry per source (and corridor step), rebuilt each update so a stale velocity never lingers.
    std::vector<ChunkCoord> anchors_;
    // The same anchors as f64 absolute centimetres, for the wide items' distance test.
    std::vector<Vec3d> anchorPos_;
    // The anchor CHUNK COORDS the last full wanted/evict scan ran against, and whether that scan
    // left anything undone. Together they are the whole precondition for skipping the next one --
    // see update()'s idle early-out for why chunk granularity is the right resolution to compare at.
    std::vector<ChunkCoord> lastScanAnchors_;
    bool lastScanHadPending_ = true;   // true so the very first update always scans
    bool haveLastScan_ = false;
    void rebuildAnchors(OriginCm origin);

    StreamSettings settings_;
    IChunkSource* source_ = nullptr;
    IChunkHost* host_ = nullptr;
    ScatterChunkHost scatterHost_;
    std::vector<WideItem> wide_;
    std::unordered_set<ChunkCoord> pinned_;
    std::unordered_map<ChunkCoord, PendingChunk> pending_;
    u32 updateStamp_ = 0;
    RestoreOptions restore_;
    OriginCm origin_{};
    bool warnedSourceLimit_ = false;
    std::vector<StreamSource> sources_;
    std::unordered_map<ChunkCoord, Resident> resident_;
    StreamStats stats_;
};

} // namespace aver::world

#endif // AVER_MODULE_SCENE
