// Which chunks are resident, and the loading and unloading that keeps it that way.
// Synchronous on the frame thread: scene::World is not thread-safe and Jolt's pool is private to Aver.Physics,
// so a worker thread may not create entities.
// Eviction radius must exceed the load radius (hysteresis), or a camera on the boundary thrashes every frame.
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

// verticalRadius = kAllLayers: the wanted set is the occupied chunks of each source's XY square (forEachOccupied).
// Only for sources that answer forEachOccupied cheaply.
inline constexpr i32 kAllLayers = -1;

struct StreamSettings {
    i32 chunkSizeCm = kDefaultChunkSizeCm;

    // Chebyshev distance in chunks (a cube, not a sphere).
    i32 loadRadius = 3;
    // Must exceed loadRadius or the boundary thrashes. Enforced in setSettings.
    i32 evictRadius = 5;

    // Per-update caps; the only bound on the frame hitch of a synchronous load. 0 = unbounded (tests only).
    u32 loadBudget = 2;
    u32 evictBudget = 4;

    // Chunks above and below each source. kAllLayers (-1) = every layer that holds something.
    i32 verticalRadius = 1;

    // Authored (cooked) levels, LARGE_WORLD 2.4.6. The defaults below keep PCG behaviour unchanged.
    // Wall-clock budgets in ms (0 = none), spent in whole chunks, at least one chunk per update. The near budget applies
    // within a quarter of loadRadius, the far one past three quarters, linear between; far 0 = near.
    f64 loadBudgetMs = 0.0;
    f64 loadBudgetFarMs = 0.0;
    f64 evictBudgetMs = 0.0;
    // Checked after each chunk, so one chunk can overshoot; 0 = no cap.
    u32 loadEntityCap = 0;
    // Added to both radii so an owner chunk up to kAuthoredPadChunks away is loaded. 0 for PCG.
    i32 padChunks = 0;

    // Lead residency ahead of a moving source by this many seconds of its velocity. The radius alone is not enough:
    // the loader needs R * chunkSizeCm / v seconds to fill the next ring at loadBudget chunks per frame.
    f32 leadSeconds = 1.5f;

    // Sources farther than this from the origin on X or Y are skipped (one warning). 0 = no limit (pre-origin behaviour).
    // A floating-origin host sets kSubMmCliffCm.
    i64 sourceLimitCm = 0;
};

// Something the world stays loaded around (a camera or an actor).
struct StreamSource {
    // Origin-relative f32 centimetres.
    Vec3 positionCm;
    Vec3 velocityCmPerSec{0, 0, 0};
    // When hasWorld, world is exact and names the chunk; positionCm is then only the look-ahead shadow.
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
    u32 pendingLoads = 0;      // wanted, not yet resident (held back by the budget)
    u32 preparePending = 0;    // of those, decoded and waiting on IChunkHost::prepare
    u32 failedLoads = 0;       // refused (damage, staleness, host Failed)
    u32 skippedSources = 0;
    f64 lastLoadMs = 0.0;
    f64 totalLoadMs = 0.0;
    u32 totalLoads = 0;
};

// Settings for a cooked level: whole-column residency, the fixed pad, and LevelStreaming's budgets (12 ms near, 3 ms far,
// 2 ms evicting, 128 entities a tick). loadCm / evictCm are STREAM distances in cm, rounded up to chunks; evict is always past load.
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

// Streams chunks around world-space sources; an actor is just another source.
class ChunkStreamer {
public:
    void setSettings(const StreamSettings& s);
    const StreamSettings& settings() const { return settings_; }

    // Not owned; must outlive the streamer.
    void setSource(IChunkSource* src) { source_ = src; }

    // Not owned. Null = ScatterChunkHost. Set before the first update: changing it with chunks resident evicts through the wrong host.
    void setHost(IChunkHost* host) { host_ = host; }
    IChunkHost* host() const { return host_; }

    // A wide item's owner chunk is wanted while its box is within loadRadius of a source, and kept within evictRadius.
    void setWide(const std::vector<WideItem>& wide) { wide_ = wide; haveLastScan_ = false; }
    const std::vector<WideItem>& wide() const { return wide_; }

    // Pinned chunks load first and are never evicted; they stay resident-and-empty if the source has nothing. Unpinning hands them back to the distance rule.
    void pin(const ChunkCoord& c, bool on = true) {
        if (on) pinned_.insert(c); else pinned_.erase(c);
        haveLastScan_ = false;
    }
    bool pinned(const ChunkCoord& c) const { return pinned_.find(c) != pinned_.end(); }

    // Material and body for restored entities; the host owns both, the streamer owns timing.
    RestoreOptions& restoreOptions() { return restore_; }

    void setSources(const std::vector<StreamSource>& sources) { sources_ = sources; }
    // Stationary convenience, for a caller that has only positions.
    void setSources(const std::vector<Vec3>& positions) {
        sources_.clear();
        sources_.reserve(positions.size());
        for (const Vec3& p : positions) sources_.push_back(StreamSource{p});
    }

    // One step: loads up to loadBudget (within loadBudgetMs and loadEntityCap), evicts up to evictBudget, nearest first.
    // origin: chunk keys and anchors are absolute, so a rebase between updates loads and evicts nothing.
    // Evicted bodies are appended to freedBodies, not removed here (this module does not link Aver.Physics).
    StreamStats update(scene::World& w, BodyRegistry& bodies, std::vector<i32>* freedBodies = nullptr,
                       OriginCm origin = {});

    // Drops everything, returning the bodies whose removal is the caller's business.
    void unloadAll(scene::World& w, BodyRegistry& bodies, std::vector<i32>& freedBodies);

    bool isResident(const ChunkCoord& c) const { return resident_.find(c) != resident_.end(); }
    // The Vec3 is origin-relative, against the origin of the last update().
    bool isResidentAt(const Vec3& worldCm) const {
        return isResident(chunkAtLocal(worldCm, origin_, settings_.chunkSizeCm));
    }
    bool isResidentAt(const WorldPos& p) const { return isResident(p.chunk); }
    OriginCm origin() const { return origin_; }

    // Where a mover may end this step, given what is loaded. If `to` is not resident, returns the furthest point from->to
    // that is, backed off by marginCm; `from` when even that is unloaded. Holding is the only recoverable option:
    // a synchronous load hitches, and a character on an unloaded chunk falls through the world.
    Vec3 clampToResident(const Vec3& from, const Vec3& to, f32 marginCm = 1.0f) const;
    usize residentCount() const { return resident_.size(); }
    // Every entity created for c, or an empty span if it is not resident.
    const std::vector<scene::Entity>* entitiesOf(const ChunkCoord& c) const;
    // Every resident chunk, for hosts that enumerate (ChunkWorld ownership queries for outliner and save paths).
    std::vector<ChunkCoord> residentChunks() const {
        std::vector<ChunkCoord> out;
        out.reserve(resident_.size());
        for (const auto& kv : resident_) out.push_back(kv.first);
        return out;
    }
    usize pendingPrepareCount() const { return pending_.size(); }

    const StreamStats& stats() const { return stats_; }

private:
    struct Resident {
        std::vector<scene::Entity> entities;
    };
    // Payload the host said Pending for, held until Ready or no longer wanted. Bounded by kMaxPendingChunks.
    struct PendingChunk {
        ChunkPayload payload;
        u32 stamp = 0;
    };
    static constexpr usize kMaxPendingChunks = 64;

    static ChunkCoord chunkAtLocal(const Vec3& p, OriginCm o, i32 chunkSizeCm) {
        return splitCm(static_cast<f64>(p.x) + static_cast<f64>(o.x), static_cast<f64>(p.y) + static_cast<f64>(o.y),
                       static_cast<f64>(p.z), chunkSizeCm).chunk;
    }
    // Chebyshev distance in chunks; large if there are no sources.
    i32 distanceToNearestSource(const ChunkCoord& c) const;
    // XY distance (cm) from the nearest anchor to a wide item's box; 0 inside; huge with no anchors.
    f64 wideDistanceCm(const WideItem& w) const;
    // Residency centre per source: position plus velocity * leadSeconds, as absolute chunk coordinates. Rebuilt each update.
    std::vector<ChunkCoord> anchors_;
    std::vector<Vec3d> anchorPos_;
    // Anchors the last full scan ran against. With lastScanHadPending_ they gate update()'s idle early-out.
    std::vector<ChunkCoord> lastScanAnchors_;
    bool lastScanHadPending_ = true;   // true: the first update always scans
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
