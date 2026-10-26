#include "aver/world/ChunkStreamer.hpp"

#if AVER_MODULE_SCENE

#  include "aver/core/Log.hpp"
#  include "aver/core/Time.hpp"

#  include <algorithm>
#  include <chrono>
#  include <cmath>

namespace aver::world {
namespace {

using SteadyClock = std::chrono::steady_clock;

f64 msSince(SteadyClock::time_point t) {
    return std::chrono::duration<f64, std::milli>(SteadyClock::now() - t).count();
}

i32 chebyshev(const ChunkCoord& a, const ChunkCoord& b) {
    const i32 dx = a.x > b.x ? a.x - b.x : b.x - a.x;
    const i32 dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    const i32 dz = a.z > b.z ? a.z - b.z : b.z - a.z;
    return dx > dy ? (dx > dz ? dx : dz) : (dy > dz ? dy : dz);
}

// One chunk the scan wants resident. `force` skips the radius test: a wide item's owner, or a pinned chunk, is wanted
// wherever its own cell is.
struct Want {
    i32 rank = 0;
    ChunkCoord c;
    bool force = false;
};

bool coordBefore(const ChunkCoord& a, const ChunkCoord& b) {
    if (a.x != b.x) return a.x < b.x;
    if (a.y != b.y) return a.y < b.y;
    return a.z < b.z;
}

} // namespace

void ChunkStreamer::setSettings(const StreamSettings& s) {
    settings_ = s;
    if (settings_.loadRadius < 0) settings_.loadRadius = 0;
    if (settings_.verticalRadius < kAllLayers) settings_.verticalRadius = 0;
    if (settings_.padChunks < 0) settings_.padChunks = 0;
    if (!chunkSizeValid(settings_.chunkSizeCm)) settings_.chunkSizeCm = kDefaultChunkSizeCm;
    // ENFORCED, not documented and hoped for. evictRadius == loadRadius is the thrash case: a source
    // on the boundary loads and evicts the same chunk on alternate frames, forever.
    if (settings_.evictRadius <= settings_.loadRadius) {
        settings_.evictRadius = settings_.loadRadius + 1;
        AVER_WARN("[Stream] evictRadius must exceed loadRadius or the boundary thrashes; raised to {}",
                  settings_.evictRadius);
    }
    haveLastScan_ = false;
}

// Each source's residency centre: where it is, plus where its own velocity takes it over
// `leadSeconds`. A stationary source anchors on itself, so this costs nothing when nothing moves.
void ChunkStreamer::rebuildAnchors(OriginCm origin) {
    anchors_.clear();
    anchorPos_.clear();
    anchors_.reserve(sources_.size());
    anchorPos_.reserve(sources_.size());
    const i32 size = settings_.chunkSizeCm;
    // The WorldPos form names its chunk exactly, but only means it when the chunk size is the one WorldPos is defined for.
    const bool exactOk = size == kWorldChunkCm;
    u32 skipped = 0;
    for (const StreamSource& s : sources_) {
        // One f32 frame: past the cliff the position is no longer exact, so the source does not stream.
        if (settings_.sourceLimitCm > 0) {
            const f64 lim = static_cast<f64>(settings_.sourceLimitCm);
            if (std::fabs(static_cast<f64>(s.positionCm.x)) > lim || std::fabs(static_cast<f64>(s.positionCm.y)) > lim) {
                ++skipped;
                continue;
            }
        }
        // always where it IS
        anchors_.push_back(s.hasWorld && exactOk ? s.world.chunk : chunkAtLocal(s.positionCm, origin, size));
        if (s.hasWorld && exactOk) {
            anchorPos_.push_back(absOf(s.world));
        } else {
            anchorPos_.push_back(Vec3d{static_cast<f64>(s.positionCm.x) + static_cast<f64>(origin.x),
                                       static_cast<f64>(s.positionCm.y) + static_cast<f64>(origin.y),
                                       static_cast<f64>(s.positionCm.z)});
        }
        const f32 t = settings_.leadSeconds > 0.0f ? settings_.leadSeconds : 0.0f;
        if (t <= 0.0f) continue;

        const f32 dx = s.velocityCmPerSec.x * t;
        const f32 dy = s.velocityCmPerSec.y * t;
        const f32 dz = s.velocityCmPerSec.z * t;
        const f32 lead = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (lead <= 0.0f) continue;

        // A CORRIDOR, NOT TWO ISLANDS. Anchoring only on the position and the lead POINT looks
        // right and is wrong the moment the lead exceeds twice the load radius: the two cubes stop
        // overlapping and the mover flies through unloaded space between them, which is worse than
        // not leading at all. The first version of this did exactly that, and only showed up once the
        // test speed was raised far enough for the gap to open.
        //
        // Stepping at one radius keeps consecutive cubes overlapping by half, so the corridor is
        // continuous however fast the source is going.
        const f32 stepCm = static_cast<f32>(settings_.loadRadius > 0 ? settings_.loadRadius : 1) *
                           static_cast<f32>(settings_.chunkSizeCm);
        // Bounded, so an absurd velocity cannot ask for thousands of anchors and stall the frame it
        // was trying to protect. Past this the lead is simply shorter than asked for -- degrading is
        // the right failure, and the boundary hold is what catches what it cannot cover.
        constexpr i32 kMaxSteps = 64;
        const i32 steps = static_cast<i32>(lead / stepCm);
        const i32 n = steps < kMaxSteps ? steps : kMaxSteps;
        for (i32 i = 1; i <= n; ++i) {
            const f32 f = static_cast<f32>(i) * stepCm / lead;
            const Vec3 p{s.positionCm.x + dx * f, s.positionCm.y + dy * f, s.positionCm.z + dz * f};
            anchors_.push_back(chunkAtLocal(p, origin, size));
            anchorPos_.push_back(Vec3d{static_cast<f64>(p.x) + static_cast<f64>(origin.x),
                                       static_cast<f64>(p.y) + static_cast<f64>(origin.y), static_cast<f64>(p.z)});
        }
    }
    stats_.skippedSources = skipped;
    if (skipped && !warnedSourceLimit_) {
        warnedSourceLimit_ = true;
        AVER_WARN("[Stream] {} source(s) are more than {} cm from the floating origin and are not streamed: the "
                  "resident window is one f32 frame", skipped, settings_.sourceLimitCm);
    }
}

i32 ChunkStreamer::distanceToNearestSource(const ChunkCoord& c) const {
    i32 best = 1 << 24;
    for (const ChunkCoord& sc : anchors_) {
        // Vertical measured separately: a surface world wants a slab, not a cube, and folding z into
        // one Chebyshev distance would make the load radius control the height too. kAllLayers has no slab at all.
        if (settings_.verticalRadius >= 0) {
            const i32 dz = sc.z > c.z ? sc.z - c.z : c.z - sc.z;
            if (dz > settings_.verticalRadius) continue;
        }
        const i32 d = chebyshev(ChunkCoord{sc.x, sc.y, c.z}, ChunkCoord{c.x, c.y, c.z});
        if (d < best) best = d;
    }
    return best;
}

f64 ChunkStreamer::wideDistanceCm(const WideItem& w) const {
    f64 best = 3.0e38;
    for (const Vec3d& a : anchorPos_) {
        const f64 dx = std::max({static_cast<f64>(w.minCm[0]) - a.x, 0.0, a.x - static_cast<f64>(w.maxCm[0])});
        const f64 dy = std::max({static_cast<f64>(w.minCm[1]) - a.y, 0.0, a.y - static_cast<f64>(w.maxCm[1])});
        const f64 d = std::sqrt(dx * dx + dy * dy);
        if (d < best) best = d;
    }
    return best;
}

const std::vector<scene::Entity>* ChunkStreamer::entitiesOf(const ChunkCoord& c) const {
    const auto it = resident_.find(c);
    return it == resident_.end() ? nullptr : &it->second.entities;
}

StreamStats ChunkStreamer::update(scene::World& w, BodyRegistry& bodies,
                                  std::vector<i32>* freedBodies, OriginCm origin) {
    origin_ = origin;
    // Restored roots are placed against this origin, and a body a host's createBody returns is kept so an eviction frees it.
    restore_.origin = origin;
    restore_.bodies = &bodies;
    stats_.loadedThisUpdate = 0;
    stats_.evictedThisUpdate = 0;
    stats_.entitiesIn = 0;
    stats_.entitiesOut = 0;
    stats_.pendingLoads = 0;
    stats_.preparePending = 0;
    stats_.failedLoads = 0;
    // Rebuilt every step, so a velocity the caller stopped updating cannot keep dragging the
    // residency somewhere the source is not going.
    rebuildAnchors(origin);
    if (!source_) return stats_;
    IChunkHost& host = host_ ? *host_ : static_cast<IChunkHost&>(scatterHost_);

    // ---- idle early-out ----
    // NOTHING MOVED AND NOTHING WAS LEFT PENDING, so neither scan below can produce a different
    // answer than it did last frame, and both are expensive: the wanted scan is
    // O(anchors x (2*loadRadius+1)^2 x (2*verticalRadius+1)) -- 1,323 candidate coordinates per
    // anchor at radius 10 -- and the eviction scan walks every resident chunk, each walk calling
    // distanceToNearestSource over every anchor. Both ran unconditionally, every frame, for every
    // field, including for a designer standing perfectly still, which the editor's own comment
    // calls out as exactly who this feature is for.
    //
    // COMPARED AT CHUNK GRANULARITY, not in centimetres: residency is a function of which CHUNK each
    // anchor is in and nothing finer, so a camera drifting within one chunk genuinely cannot change
    // either set. Comparing positions would defeat the check on any real input. (A wide item's distance is measured from the
    // positions, so its owner can be wanted up to one chunk of travel late: inside the hysteresis, and the pad.)
    //
    // THE PENDING FLAG IS WHAT MAKES THIS SAFE. A scan that hit the load budget left work undone,
    // and skipping the next one would strand those chunks unloaded forever with the camera still.
    // So the early-out only fires once the previous scan finished with nothing outstanding.
    {
        // The anchors ARE chunk coordinates now, absolute: a rebase changes nothing compared here.
        std::vector<ChunkCoord> nowAnchors = anchors_;
        if (haveLastScan_ && !lastScanHadPending_ && nowAnchors == lastScanAnchors_) {
            // Residency is unchanged, so report it rather than leaving the caller with the zeroed
            // counters at the top of this function.
            stats_.residentChunks = static_cast<u32>(resident_.size());
            stats_.residentEntities = 0;
            for (const auto& kv : resident_) stats_.residentEntities += static_cast<u32>(kv.second.entities.size());
            return stats_;
        }
        lastScanAnchors_ = std::move(nowAnchors);
        haveLastScan_ = true;
    }

    const i32 sizeCm = settings_.chunkSizeCm;
    // The pad grows both radii: the owner chunk of an item that reaches into the load area from up to padChunks away.
    const i32 loadR = settings_.loadRadius + settings_.padChunks;
    const i32 evictR = settings_.evictRadius + settings_.padChunks;
    const bool allLayers = settings_.verticalRadius == kAllLayers;
    ++updateStamp_;

    // ---- what should be resident ----
    // Gathered per source and deduplicated, sorted NEAREST FIRST so a budget spends itself on what
    // the camera is about to see rather than on whatever the iteration order happened to reach.
    std::vector<Want> wanted;
    if (!allLayers) {
        for (const ChunkCoord& sc : anchors_) {
            for (i32 dz = -settings_.verticalRadius; dz <= settings_.verticalRadius; ++dz)
                for (i32 dy = -loadR; dy <= loadR; ++dy)
                    for (i32 dx = -loadR; dx <= loadR; ++dx) {
                        const ChunkCoord c{sc.x + dx, sc.y + dy, sc.z + dz};
                        if (resident_.find(c) != resident_.end()) continue;
                        wanted.push_back(Want{distanceToNearestSource(c), c, false});
                    }
        }
    } else {
        // THE COLUMN-OCCUPANCY SCAN: ask the source what it holds in each anchor's XY square, every layer. Empty space costs
        // nothing, and a tower is found whatever layers it spans.
        for (const ChunkCoord& sc : anchors_) {
            ChunkBox box;
            box.min = ChunkCoord{sc.x - loadR, sc.y - loadR, kChunkAxisMin};
            box.max = ChunkCoord{sc.x + loadR, sc.y + loadR, kChunkAxisMax};
            source_->forEachOccupied(box, [&](const ChunkCoord& c) {
                if (resident_.find(c) != resident_.end()) return;
                wanted.push_back(Want{distanceToNearestSource(c), c, false});
            });
        }
    }
    // Oversize items: their owner chunk is wanted while the box is within the load distance of any source.
    if (!wide_.empty() && !anchorPos_.empty()) {
        const f64 loadD = static_cast<f64>(settings_.loadRadius) * static_cast<f64>(sizeCm);
        for (const WideItem& wi : wide_) {
            if (resident_.find(wi.owner) != resident_.end()) continue;
            const f64 d = wideDistanceCm(wi);
            if (d <= loadD) wanted.push_back(Want{static_cast<i32>(d / static_cast<f64>(sizeCm)), wi.owner, true});
        }
    }
    // Pinned chunks come first, wherever the sources are.
    for (const ChunkCoord& pc : pinned_)
        if (resident_.find(pc) == resident_.end()) wanted.push_back(Want{-1, pc, true});

    // Duplicates (overlapping anchors, a wide owner that is also in the radius) merge to the best rank, forced if either was.
    std::sort(wanted.begin(), wanted.end(), [](const Want& a, const Want& b) { return coordBefore(a.c, b.c); });
    {
        usize out = 0;
        for (usize i = 0; i < wanted.size(); ++i) {
            if (out > 0 && wanted[out - 1].c == wanted[i].c) {
                wanted[out - 1].rank = std::min(wanted[out - 1].rank, wanted[i].rank);
                wanted[out - 1].force = wanted[out - 1].force || wanted[i].force;
            } else {
                wanted[out++] = wanted[i];
            }
        }
        wanted.resize(out);
    }
    std::sort(wanted.begin(), wanted.end(), [](const Want& a, const Want& b) {
        if (a.rank != b.rank) return a.rank < b.rank;
        // A total order, so two sources at equal distance do not make the load order depend on which
        // was listed first -- which would make a replay of the same flight load a different sequence.
        return coordBefore(a.c, b.c);
    });

    // ---- evict, before loading ----
    // BEFORE, deliberately: eviction frees entity slots and memory that the loads below may want,
    // and doing it after would peak at resident + loaded rather than at resident.
    bool evictLeft = false;   // a budget stopped the loop with chunks still to go: the next update must not be skipped as idle
    {
        // THE DISTANCE IS CARRIED, NOT RECOMPUTED, exactly as the `wanted` list above already does.
        // It used to gather bare coords and then call distanceToNearestSource AGAIN inside the sort
        // comparator -- twice per comparison, O(n log n) times, for chunks whose distance had just
        // been computed and thrown away one line earlier. distanceToNearestSource itself loops over
        // every anchor (up to 65 of them once the lead corridor kicks in), so that was an O(log n)
        // multiplier of O(anchors) work per resident chunk, every frame.
        //
        // The eviction ORDER is unchanged -- still furthest first, so the ones least likely to be
        // wanted again go when the budget cannot take them all.
        std::unordered_set<ChunkCoord> wideKeep;
        if (!wide_.empty()) {
            const f64 keepD = static_cast<f64>(settings_.evictRadius) * static_cast<f64>(sizeCm);
            for (const WideItem& wi : wide_)
                if (resident_.find(wi.owner) != resident_.end() && wideDistanceCm(wi) <= keepD) wideKeep.insert(wi.owner);
        }
        std::vector<std::pair<i32, ChunkCoord>> gone;
        for (const auto& kv : resident_) {
            if (!pinned_.empty() && pinned_.find(kv.first) != pinned_.end()) continue;
            if (!wideKeep.empty() && wideKeep.find(kv.first) != wideKeep.end()) continue;
            const i32 d = distanceToNearestSource(kv.first);
            if (d > evictR) gone.emplace_back(d, kv.first);
        }
        std::sort(gone.begin(), gone.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            // A total order, for the same reason `wanted` needs one: equal distances must not make
            // the eviction sequence depend on unordered_map iteration order.
            return coordBefore(a.second, b.second);
        });
        std::vector<i32> sink;
        std::vector<i32>& freed = freedBodies ? *freedBodies : sink;
        const SteadyClock::time_point evictStart = SteadyClock::now();
        for (const auto& [goneDist, c] : gone) {
            (void)goneDist;
            if ((settings_.evictBudget && stats_.evictedThisUpdate >= settings_.evictBudget) ||
                (settings_.evictBudgetMs > 0.0 && stats_.evictedThisUpdate > 0 && msSince(evictStart) >= settings_.evictBudgetMs)) {
                evictLeft = true;
                break;
            }
            const auto it = resident_.find(c);
            if (it == resident_.end()) continue;
            // The host takes the entities (and, for the scatter host, their bodies FIRST, the whole subtree -- taking only the
            // root would leak every child's collision, which is the bug SandboxApp::destroyEntity has today). Bodies go OUT to
            // the caller rather than being removed here: this module links Aver.Scene and not Aver.Physics, on purpose.
            stats_.entitiesOut += host.evict(c, it->second.entities, w, bodies, freed);
            resident_.erase(it);
            ++stats_.evictedThisUpdate;
        }
    }

    // ---- load, within budget ----
    // The time budget is the near one while the nearest missing chunk is within a quarter of the load radius and the far one past
    // three quarters (LevelStreaming's rule): what is about to be seen is worth more of the frame than what is far away.
    f64 budgetMs = settings_.loadBudgetMs;
    if (budgetMs > 0.0 && !wanted.empty()) {
        const f64 farMs = settings_.loadBudgetFarMs > 0.0 ? settings_.loadBudgetFarMs : budgetMs;
        const f64 rank = static_cast<f64>(std::max(wanted.front().rank, 0));
        const f64 t = std::clamp((rank / static_cast<f64>(std::max(loadR, 1)) - 0.25) / 0.5, 0.0, 1.0);
        budgetMs = budgetMs + (farMs - budgetMs) * t;
    }
    const SteadyClock::time_point loadStart = SteadyClock::now();
    u32 attempted = 0;   // chunks decoded or restored this update: the "at least one" the time budget never overrides
    for (const Want& item : wanted) {
        if (!item.force && item.rank > loadR) continue;
        if ((settings_.loadBudget && stats_.loadedThisUpdate >= settings_.loadBudget) ||
            (budgetMs > 0.0 && attempted > 0 && msSince(loadStart) >= budgetMs) ||
            (settings_.loadEntityCap && stats_.loadedThisUpdate > 0 && stats_.entitiesIn >= settings_.loadEntityCap)) {
            ++stats_.pendingLoads;
            continue;
        }
        const auto held = pending_.find(item.c);
        if (held == pending_.end() && !source_->has(item.c)) {
            // empty space is the common case, and not a miss -- except a pinned chunk, which is held resident-and-empty
            if (item.rank < 0) resident_.emplace(item.c, Resident{});
            continue;
        }
        if (held == pending_.end() && pending_.size() >= kMaxPendingChunks && host_ != nullptr) {
            // Too many decoded chunks are waiting on their host: decode no more until some resolve.
            ++stats_.pendingLoads;
            continue;
        }

        Clock clock;
        ChunkPayload payload;
        std::string why;
        if (held != pending_.end()) {
            payload = std::move(held->second.payload);
            pending_.erase(held);
        } else if (!source_->load(item.c, payload, &why)) {
            ++stats_.failedLoads;
            // Named and said once per chunk, not once per frame: a chunk that fails is not retried,
            // because it is recorded as resident-with-nothing below.
            AVER_WARN("[Stream] chunk ({},{},{}) could not be loaded: {}",
                      item.c.x, item.c.y, item.c.z, why);
            resident_.emplace(item.c, Resident{});
            ++stats_.loadedThisUpdate;
            ++attempted;
            continue;
        }
        ++attempted;

        const ChunkPrepare prep = host.prepare(payload);
        if (prep == ChunkPrepare::Pending) {
            // Held (decoded once) and asked again next update; counted as pending so the idle early-out does not skip the retry.
            PendingChunk& pc = pending_[item.c];
            pc.payload = std::move(payload);
            pc.stamp = updateStamp_;
            ++stats_.pendingLoads;
            ++stats_.preparePending;
            continue;
        }
        if (prep == ChunkPrepare::Failed) {
            ++stats_.failedLoads;
            AVER_WARN("[Stream] chunk ({},{},{}) was refused by its host", item.c.x, item.c.y, item.c.z);
            resident_.emplace(item.c, Resident{});
            ++stats_.loadedThisUpdate;
            continue;
        }

        Resident r;
        r.entities = host.restore(payload, w, restore_);
        // A restore that produced nothing still counts as resident: retrying it every frame would
        // turn one bad chunk into a permanent stall.
        stats_.entitiesIn += static_cast<u32>(r.entities.size());
        resident_.emplace(item.c, std::move(r));
        ++stats_.loadedThisUpdate;

        const f64 ms = clock.restart() * 1000.0;
        stats_.lastLoadMs = ms;
        stats_.totalLoadMs += ms;
        ++stats_.totalLoads;
    }
    // A held payload nobody asked about this update is for a chunk that is no longer wanted: let it go.
    for (auto it = pending_.begin(); it != pending_.end();)
        it = (it->second.stamp != updateStamp_) ? pending_.erase(it) : std::next(it);

    // What the early-out above tests next frame: a scan that hit the budget must not be skipped.
    lastScanHadPending_ = stats_.pendingLoads > 0 || evictLeft;

    stats_.residentChunks = static_cast<u32>(resident_.size());
    stats_.residentEntities = 0;
    for (const auto& kv : resident_) stats_.residentEntities += static_cast<u32>(kv.second.entities.size());
    return stats_;
}

Vec3 ChunkStreamer::clampToResident(const Vec3& from, const Vec3& to, f32 marginCm) const {
    if (isResidentAt(to)) return to;

    // Binary search along the segment for the last resident point. Sixteen steps resolves a 16 m
    // chunk to a quarter of a millimetre, which is far finer than anything downstream cares about,
    // and it beats stepping the segment because the cost does not grow with how far the mover tried
    // to travel.
    Vec3 good = from;
    if (!isResidentAt(good)) return from;   // already outside; moving further cannot help

    Vec3 bad = to;
    for (int i = 0; i < 16; ++i) {
        const Vec3 mid{0.5f * (good.x + bad.x), 0.5f * (good.y + bad.y), 0.5f * (good.z + bad.z)};
        if (isResidentAt(mid)) good = mid;
        else bad = mid;
    }

    // Backed off along the direction of travel, so the mover does not come to rest exactly on the
    // face and re-enter this every frame -- the same reason eviction has hysteresis.
    const f32 dx = to.x - from.x, dy = to.y - from.y, dz = to.z - from.z;
    const f32 len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 0.0f || marginCm <= 0.0f) return good;
    const f32 k = marginCm / len;
    const Vec3 backed{good.x - dx * k, good.y - dy * k, good.z - dz * k};
    return isResidentAt(backed) ? backed : good;
}

void ChunkStreamer::unloadAll(scene::World& w, BodyRegistry& bodies, std::vector<i32>& freedBodies) {
    IChunkHost& host = host_ ? *host_ : static_cast<IChunkHost&>(scatterHost_);
    for (auto& kv : resident_) host.evict(kv.first, kv.second.entities, w, bodies, freedBodies);
    resident_.clear();
    pending_.clear();
    haveLastScan_ = false;
    stats_.residentChunks = 0;
    stats_.residentEntities = 0;
}

} // namespace aver::world

#endif // AVER_MODULE_SCENE
