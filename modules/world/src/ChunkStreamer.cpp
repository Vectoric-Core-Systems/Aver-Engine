#include "aver/world/ChunkStreamer.hpp"

#if AVER_MODULE_SCENE

#  include "aver/core/Log.hpp"
#  include "aver/core/Time.hpp"

#  include <algorithm>
#  include <cmath>

namespace aver::world {
namespace {

i32 chebyshev(const ChunkCoord& a, const ChunkCoord& b) {
    const i32 dx = a.x > b.x ? a.x - b.x : b.x - a.x;
    const i32 dy = a.y > b.y ? a.y - b.y : b.y - a.y;
    const i32 dz = a.z > b.z ? a.z - b.z : b.z - a.z;
    return dx > dy ? (dx > dz ? dx : dz) : (dy > dz ? dy : dz);
}

} // namespace

void ChunkStreamer::setSettings(const StreamSettings& s) {
    settings_ = s;
    if (settings_.loadRadius < 0) settings_.loadRadius = 0;
    if (settings_.verticalRadius < 0) settings_.verticalRadius = 0;
    if (!chunkSizeValid(settings_.chunkSizeCm)) settings_.chunkSizeCm = kDefaultChunkSizeCm;
    // ENFORCED, not documented and hoped for. evictRadius == loadRadius is the thrash case: a source
    // on the boundary loads and evicts the same chunk on alternate frames, forever.
    if (settings_.evictRadius <= settings_.loadRadius) {
        settings_.evictRadius = settings_.loadRadius + 1;
        AVER_WARN("[Stream] evictRadius must exceed loadRadius or the boundary thrashes; raised to {}",
                  settings_.evictRadius);
    }
}

// Each source's residency centre: where it is, plus where its own velocity takes it over
// `leadSeconds`. A stationary source anchors on itself, so this costs nothing when nothing moves.
void ChunkStreamer::rebuildAnchors() {
    anchors_.clear();
    anchors_.reserve(sources_.size());
    for (const StreamSource& s : sources_) {
        anchors_.push_back(s.positionCm);   // always where it IS
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
            anchors_.push_back(Vec3{s.positionCm.x + dx * f, s.positionCm.y + dy * f,
                                    s.positionCm.z + dz * f});
        }
    }
}

i32 ChunkStreamer::distanceToNearestSource(const ChunkCoord& c) const {
    i32 best = 1 << 24;
    for (const Vec3& p : anchors_) {
        const ChunkCoord sc = splitCm(p, settings_.chunkSizeCm).chunk;
        // Vertical measured separately: a surface world wants a slab, not a cube, and folding z into
        // one Chebyshev distance would make the load radius control the height too.
        const i32 dz = sc.z > c.z ? sc.z - c.z : c.z - sc.z;
        if (dz > settings_.verticalRadius) continue;
        const i32 d = chebyshev(ChunkCoord{sc.x, sc.y, c.z}, ChunkCoord{c.x, c.y, c.z});
        if (d < best) best = d;
    }
    return best;
}

const std::vector<scene::Entity>* ChunkStreamer::entitiesOf(const ChunkCoord& c) const {
    const auto it = resident_.find(c);
    return it == resident_.end() ? nullptr : &it->second.entities;
}

StreamStats ChunkStreamer::update(scene::World& w, BodyRegistry& bodies,
                                  std::vector<i32>* freedBodies) {
    stats_.loadedThisUpdate = 0;
    stats_.evictedThisUpdate = 0;
    stats_.entitiesIn = 0;
    stats_.entitiesOut = 0;
    stats_.pendingLoads = 0;
    stats_.failedLoads = 0;
    // Rebuilt every step, so a velocity the caller stopped updating cannot keep dragging the
    // residency somewhere the source is not going.
    rebuildAnchors();
    if (!source_) return stats_;

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
    // either set. Comparing positions would defeat the check on any real input.
    //
    // THE PENDING FLAG IS WHAT MAKES THIS SAFE. A scan that hit the load budget left work undone,
    // and skipping the next one would strand those chunks unloaded forever with the camera still.
    // So the early-out only fires once the previous scan finished with nothing outstanding.
    {
        std::vector<ChunkCoord> nowAnchors;
        nowAnchors.reserve(anchors_.size());
        for (const Vec3& p : anchors_) nowAnchors.push_back(splitCm(p, settings_.chunkSizeCm).chunk);
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

    // ---- what should be resident ----
    // Gathered per source and deduplicated, sorted NEAREST FIRST so a budget spends itself on what
    // the camera is about to see rather than on whatever the iteration order happened to reach.
    std::vector<std::pair<i32, ChunkCoord>> wanted;
    for (const Vec3& p : anchors_) {
        const ChunkCoord sc = splitCm(p, settings_.chunkSizeCm).chunk;
        for (i32 dz = -settings_.verticalRadius; dz <= settings_.verticalRadius; ++dz)
            for (i32 dy = -settings_.loadRadius; dy <= settings_.loadRadius; ++dy)
                for (i32 dx = -settings_.loadRadius; dx <= settings_.loadRadius; ++dx) {
                    const ChunkCoord c{sc.x + dx, sc.y + dy, sc.z + dz};
                    if (resident_.find(c) != resident_.end()) continue;
                    wanted.emplace_back(distanceToNearestSource(c), c);
                }
    }
    std::sort(wanted.begin(), wanted.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        // A total order, so two sources at equal distance do not make the load order depend on which
        // was listed first -- which would make a replay of the same flight load a different sequence.
        if (a.second.x != b.second.x) return a.second.x < b.second.x;
        if (a.second.y != b.second.y) return a.second.y < b.second.y;
        return a.second.z < b.second.z;
    });
    wanted.erase(std::unique(wanted.begin(), wanted.end(),
                             [](const auto& a, const auto& b) { return a.second == b.second; }),
                 wanted.end());

    // ---- evict, before loading ----
    // BEFORE, deliberately: eviction frees entity slots and memory that the loads below may want,
    // and doing it after would peak at resident + loaded rather than at resident.
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
        std::vector<std::pair<i32, ChunkCoord>> gone;
        for (const auto& kv : resident_) {
            const i32 d = distanceToNearestSource(kv.first);
            if (d > settings_.evictRadius) gone.emplace_back(d, kv.first);
        }
        std::sort(gone.begin(), gone.end(), [](const auto& a, const auto& b) {
            if (a.first != b.first) return a.first > b.first;
            // A total order, for the same reason `wanted` needs one: equal distances must not make
            // the eviction sequence depend on unordered_map iteration order.
            if (a.second.x != b.second.x) return a.second.x < b.second.x;
            if (a.second.y != b.second.y) return a.second.y < b.second.y;
            return a.second.z < b.second.z;
        });
        for (const auto& [goneDist, c] : gone) {
            (void)goneDist;
            if (settings_.evictBudget && stats_.evictedThisUpdate >= settings_.evictBudget) break;
            const auto it = resident_.find(c);
            if (it == resident_.end()) continue;
            for (const scene::Entity e : it->second.entities) {
                if (!w.valid(e)) continue;
                // The bodies FIRST, while the entity is still alive to be walked. detachSubtree
                // takes the whole subtree because World::destroy does -- taking only the root would
                // leak every child's collision, which is the bug SandboxApp::destroyEntity has today.
                //
                // They go OUT to the caller rather than being removed here: this module links
                // Aver.Scene and not Aver.Physics, on purpose, so a build without physics still has
                // a streamer that compiles.
                if (freedBodies) bodies.detachSubtree(w, e, *freedBodies);
                else { std::vector<i32> sink; bodies.detachSubtree(w, e, sink); }
                w.destroy(e);
                ++stats_.entitiesOut;
            }
            resident_.erase(it);
            ++stats_.evictedThisUpdate;
        }
    }

    // ---- load, within budget ----
    for (const auto& item : wanted) {
        if (item.first > settings_.loadRadius) continue;
        if (settings_.loadBudget && stats_.loadedThisUpdate >= settings_.loadBudget) {
            ++stats_.pendingLoads;
            continue;
        }
        if (!source_->has(item.second)) continue;   // empty space is the common case, and not a miss

        Clock clock;
        ChunkPayload payload;
        std::string why;
        if (!source_->load(item.second, payload, &why)) {
            ++stats_.failedLoads;
            // Named and said once per chunk, not once per frame: a chunk that fails is not retried,
            // because it is recorded as resident-with-nothing below.
            AVER_WARN("[Stream] chunk ({},{},{}) could not be loaded: {}",
                      item.second.x, item.second.y, item.second.z, why);
            resident_.emplace(item.second, Resident{});
            ++stats_.loadedThisUpdate;
            continue;
        }

        Resident r;
        r.entities = restore(payload, w, restore_);
        // A restore that produced nothing still counts as resident: retrying it every frame would
        // turn one bad chunk into a permanent stall.
        stats_.entitiesIn += static_cast<u32>(r.entities.size());
        resident_.emplace(item.second, std::move(r));
        ++stats_.loadedThisUpdate;

        const f64 ms = clock.restart() * 1000.0;
        stats_.lastLoadMs = ms;
        stats_.totalLoadMs += ms;
        ++stats_.totalLoads;
    }

    // What the early-out above tests next frame: a scan that hit the budget must not be skipped.
    lastScanHadPending_ = stats_.pendingLoads > 0;

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
    for (auto& kv : resident_)
        for (const scene::Entity e : kv.second.entities) {
            if (!w.valid(e)) continue;
            bodies.detachSubtree(w, e, freedBodies);
            w.destroy(e);
        }
    resident_.clear();
    stats_.residentChunks = 0;
    stats_.residentEntities = 0;
}

} // namespace aver::world

#endif // AVER_MODULE_SCENE
