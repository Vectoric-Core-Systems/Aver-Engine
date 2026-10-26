#include "aver/game/LevelStreaming.hpp"
#include "aver/core/HitchMarks.hpp"

#if AVER_MODULE_SCENE
#  include "aver/game/GameContent.hpp"
#  include "aver/core/Log.hpp"
#  include "aver/world/LevelTransform.hpp"

#  include <algorithm>
#  include <chrono>
#  include <cfloat>

namespace aver::game {

namespace {
// A placement with no known bounds (no aabb= and its mesh not loaded) never leaves: residency cannot
// be judged without them, and unloading what might be under the viewer is the worse failure.
constexpr f32 kUnknownExtentCm = 1.0e9f;

// itemOf_ value of a placement with no item: a class placement, which the host loads and the streamer never sees.
constexpr u32 kNoItem = ~0u;

// A placement pose with the POSITION in f64: the record is absolute f64 text, and an f32 position quantises it past ~10 km.
struct PoseD {
    world::Vec3d position;
    Quat rotation;
    Vec3 scale;
};

PoseD localOf(const fmt::OcWorldPlacement& p) {
    PoseD xf;
    xf.position = world::Vec3d{p.x, p.y, p.z};
    xf.rotation = world::quatFromEulerDeg(Vec3{static_cast<f32>(p.roll), static_cast<f32>(p.pitch),
                                               static_cast<f32>(p.yaw)});
    xf.scale = Vec3{static_cast<f32>(p.sx), static_cast<f32>(p.sy), static_cast<f32>(p.sz)};
    return xf;
}

world::Vec3d add(const world::Vec3d& a, const Vec3& b) { return world::Vec3d{a.x + b.x, a.y + b.y, a.z + b.z}; }
} // namespace

void LevelStreaming::begin(const fmt::OcWorldData& w, GameContent& content, rhi::IDevice& device,
                           const world::InstantiateOptions& opt, Hooks hooks) {
    content_ = &content;
    device_ = &device;
    loader_.begin(content, device, opt);
    hooks_ = std::move(hooks);
    records_ = w.placements;
    removed_.assign(records_.size(), 0);
    itemOf_.assign(records_.size(), kNoItem);
    baseline_.assign(records_.size(), world::PlacementBaseline{});
    hasBaseline_.assign(records_.size(), 0);
    items_.clear();
    rootPlacements_.clear();
    placementOf_.clear();
    entityOf_.clear();

    // Roots own their descendants (parents precede children in OcWorldData). A child of a class
    // placement loads as a root, as world::instantiate already treats it.
    for (u32 i = 0; i < records_.size(); ++i) {
        const fmt::OcWorldPlacement& p = records_[i];
        if (!p.className.empty()) continue;
        const bool child = p.parent >= 0 && static_cast<u32>(p.parent) < i && itemOf_[static_cast<u32>(p.parent)] != kNoItem;
        if (child) {
            const u32 it = itemOf_[static_cast<u32>(p.parent)];
            itemOf_[i] = it;
            items_[it].placements.push_back(i);
        } else {
            itemOf_[i] = static_cast<u32>(items_.size());
            Item item;
            item.placements.push_back(i);
            items_.push_back(std::move(item));
            rootPlacements_.push_back(i);
        }
    }

    world::PlacementStreamSettings s;
    s.cellCm = w.stream.cellCm;
    s.loadCm = w.stream.loadCm;
    s.evictCm = std::max(w.stream.evictCm, w.stream.loadCm + 100.0f);
    std::vector<world::AabbD> bounds;
    bounds.reserve(items_.size());
    usize unknown = 0;
    for (const Item& it : items_) {
        bounds.push_back(itemBounds(it));
        if (bounds.back().max.x - bounds.back().min.x >= static_cast<f64>(kUnknownExtentCm)) ++unknown;
    }
    streamer_.build(bounds, s);
    active_ = true;
    AVER_INFO("[LevelStream] {} placement(s) in {} root(s); cell {:.0f} m, load {:.0f} m, evict {:.0f} m; "
              "{} root(s) with unknown bounds stay resident",
              records_.size(), items_.size(), s.cellCm / 100.0f, s.loadCm / 100.0f, s.evictCm / 100.0f, unknown);
}

void LevelStreaming::end(scene::World& world) {
    if (!active_) return;
    for (u32 i = 0; i < items_.size(); ++i)
        if (items_[i].loaded) evictItem(world, i);
    loader_.releaseUnusedShapes();
    loader_.end();
    active_ = false;
    records_.clear();
    baseline_.clear();
    hasBaseline_.clear();
    items_.clear();
    placementOf_.clear();
    entityOf_.clear();
}

bool LevelStreaming::worldBounds(u32 placement, world::Vec3d& outMin, world::Vec3d& outMax) const {
    const fmt::OcWorldPlacement& p = records_[placement];
    if (p.hasBounds) {
        outMin = world::Vec3d{p.boundsMin[0], p.boundsMin[1], p.boundsMin[2]};
        outMax = world::Vec3d{p.boundsMax[0], p.boundsMax[1], p.boundsMax[2]};
        return true;
    }
    const std::pair<Vec3, Vec3>* lb = content_ ? content_->boundsFor(p.objectId) : nullptr;
    if (!lb) return false;
    // World transform through the parent chain, composed as world::instantiate composes it, with the position in f64.
    PoseD xf = localOf(p);
    for (i32 q = p.parent; q >= 0 && static_cast<usize>(q) < records_.size(); q = records_[static_cast<usize>(q)].parent) {
        const PoseD pw = localOf(records_[static_cast<usize>(q)]);
        PoseD out;
        out.scale = Vec3{pw.scale.x * xf.scale.x, pw.scale.y * xf.scale.y, pw.scale.z * xf.scale.z};
        out.rotation = pw.rotation * xf.rotation;
        const Vec3 scaled{static_cast<f32>(xf.position.x) * pw.scale.x, static_cast<f32>(xf.position.y) * pw.scale.y,
                          static_cast<f32>(xf.position.z) * pw.scale.z};
        out.position = add(pw.position, pw.rotation.rotate(scaled));
        xf = out;
    }
    outMin = world::Vec3d{DBL_MAX, DBL_MAX, DBL_MAX};
    outMax = world::Vec3d{-DBL_MAX, -DBL_MAX, -DBL_MAX};
    for (int c = 0; c < 8; ++c) {
        const Vec3 l{(c & 1) ? lb->second.x : lb->first.x, (c & 2) ? lb->second.y : lb->first.y,
                     (c & 4) ? lb->second.z : lb->first.z};
        const Vec3 s{l.x * xf.scale.x, l.y * xf.scale.y, l.z * xf.scale.z};
        const world::Vec3d v = add(xf.position, xf.rotation.rotate(s));
        outMin = world::Vec3d{std::min(outMin.x, v.x), std::min(outMin.y, v.y), std::min(outMin.z, v.z)};
        outMax = world::Vec3d{std::max(outMax.x, v.x), std::max(outMax.y, v.y), std::max(outMax.z, v.z)};
    }
    return true;
}

world::AabbD LevelStreaming::itemBounds(const Item& it) const {
    world::AabbD b;
    b.min = world::Vec3d{DBL_MAX, DBL_MAX, DBL_MAX};
    b.max = world::Vec3d{-DBL_MAX, -DBL_MAX, -DBL_MAX};
    bool any = false;
    for (const u32 pi : it.placements) {
        if (removed(pi)) continue;
        world::Vec3d lo, hi;
        if (!worldBounds(pi, lo, hi)) {
            const f64 u = static_cast<f64>(kUnknownExtentCm);
            b.min = world::Vec3d{-u, -u, -u};
            b.max = world::Vec3d{u, u, u};
            return b;
        }
        b.min = world::Vec3d{std::min(b.min.x, lo.x), std::min(b.min.y, lo.y), std::min(b.min.z, lo.z)};
        b.max = world::Vec3d{std::max(b.max.x, hi.x), std::max(b.max.y, hi.y), std::max(b.max.z, hi.z)};
        any = true;
    }
    if (!any) b.min = b.max = world::Vec3d{0, 0, 0};
    return b;
}

void LevelStreaming::noteBaseline(u32 placement, scene::Entity e) {
    // Item roots only: a child's record is parent-relative and origin-free. The baseline is what the entity actually has now (a ground-snapped
    // root differs from the narrowed record in Z), so an untouched entity compares equal bit for bit.
    if (placement >= records_.size() || itemOf_[placement] == kNoItem) return;
    const Item& it = items_[itemOf_[placement]];
    if (it.placements.empty() || it.placements.front() != placement) return;
    const auto* loc = scene::World::instance().component<scene::CLocal>(e, scene::kComponentLocal);
    if (!loc) return;
    const fmt::OcWorldPlacement& p = records_[placement];
    baseline_[placement] = world::PlacementBaseline{world::Vec3d{p.x, p.y, p.z}, loc->xf.position};
    hasBaseline_[placement] = 1;
}

void LevelStreaming::onOriginShift(const world::ShiftEvent& ev) {
    origin_ = ev.newOrigin;
    // Records and bounds are absolute, so nothing loads or evicts. The baselines follow the entities: the very f32 subtraction
    // World::shiftRoots applied to them, so an entity nobody moved still compares equal to its baseline.
    for (u32 i = 0; i < baseline_.size(); ++i)
        if (hasBaseline_[i]) world::rebaseBaseline(baseline_[i], ev.deltaF);
}

bool LevelStreaming::recordPositionOf(scene::Entity e, const Vec3& currentLocal, world::Vec3d& outCm) const {
    const auto it = placementOf_.find(e);
    if (it == placementOf_.end() || it->second >= hasBaseline_.size() || !hasBaseline_[it->second]) return false;
    outCm = world::writeBackCm(baseline_[it->second], currentLocal, origin_);
    return true;
}

void LevelStreaming::loadItems(const std::vector<u32>& items) {   // instantiate() creates into the process-global World
    HitchMarks hm(items.size() == 1 && !items_[items[0]].placements.empty()
                      ? records_[items_[items[0]].placements.front()].asset.c_str() : "load batch", 0.25);
    // One instantiate for the batch: a sub-level of the items' placements, parents remapped.
    std::vector<ItemLoader::Entry> entries;
    std::unordered_map<u32, i32> slotOf;
    for (const u32 item : items) {
        for (const u32 pi : items_[item].placements) {
            if (removed(pi)) continue;
            const fmt::OcWorldPlacement& p = records_[pi];
            const auto par = p.parent >= 0 ? slotOf.find(static_cast<u32>(p.parent)) : slotOf.end();
            ItemLoader::Entry en;
            en.rec = &p;
            en.tag = pi;
            en.item = item;
            en.parent = par != slotOf.end() ? par->second : -1;
            slotOf[pi] = static_cast<i32>(entries.size());
            entries.push_back(en);
        }
    }
    const std::vector<ItemLoader::Loaded> loadedBatch = loader_.load(entries, origin_, &hm);
    std::vector<scene::Entity> loaded;
    std::vector<u32> loadedPlacement;
    std::vector<i32> loadedBody;
    for (const ItemLoader::Loaded& l : loadedBatch) {
        loadedBody.push_back(l.body);
        placementOf_[l.entity] = l.tag;
        entityOf_[l.tag] = l.entity;
        noteBaseline(l.tag, l.entity);
        loaded.push_back(l.entity);
        loadedPlacement.push_back(l.tag);
    }
    for (const u32 item : items) {
        items_[item].loaded = true;
        streamer_.markLoaded(item);
    }
    hm.mark("bookkeeping");
    // COMPONENT records of the batch, before the host sees it.
    loader_.applyComponents(loadedBatch);
    hm.mark("components");
    if (hooks_.afterLoad && !loaded.empty()) hooks_.afterLoad(loaded, loadedPlacement, loadedBody);
    hm.mark("hook");
}

void LevelStreaming::evictItem(scene::World& world, u32 item, bool writeBack) {
    Item& it = items_[item];
    HitchMarks hm(it.placements.empty() ? "evict" : records_[it.placements.front()].asset.c_str(), 0.25);
    // A copy: the loader empties its list in evict(), after the maps below are cleaned.
    const std::vector<scene::Entity> entities = loader_.entitiesOf(item);
    for (const scene::Entity e : entities) {
        const auto pl = placementOf_.find(e);
        if (pl != placementOf_.end() && hooks_.beforeEvict && world.valid(e)) hooks_.beforeEvict(e, pl->second, writeBack);
    }
    hm.mark("evict:hook");
    for (auto e = entities.rbegin(); e != entities.rend(); ++e) {
        const auto pl = placementOf_.find(*e);
        if (pl != placementOf_.end()) { entityOf_.erase(pl->second); placementOf_.erase(pl); }
    }
    loader_.evict(world, item, &hm);   // bodies, entities (children first), meshes, kinematic bodies
    it.loaded = false;
    streamer_.markEvicted(item);
}

void LevelStreaming::tick(scene::World& world, const std::vector<Vec3>& viewersLocal) {
    if (!active_) return;
    HitchMarks hm("level stream tick", 0.25);
    // The bounds are absolute f64; the viewers arrive in the scene frame. At a zero origin `x + 0.0` is x.
    std::vector<world::Vec3d> viewers;
    viewers.reserve(viewersLocal.size());
    for (const Vec3& v : viewersLocal)
        viewers.push_back(world::Vec3d{static_cast<f64>(v.x) + static_cast<f64>(origin_.x), static_cast<f64>(v.y) + static_cast<f64>(origin_.y),
                                       static_cast<f64>(v.z)});
    std::vector<u32> toLoad, toEvict;
    streamer_.update(viewers, toLoad, toEvict);
    hm.mark("streamer");
    lastLoaded_ = 0;
    lastEvicted_ = 0;
    using Clock = std::chrono::steady_clock;
    auto msSince = [](Clock::time_point t) { return std::chrono::duration<f64, std::milli>(Clock::now() - t).count(); };
    // Evictions are past the evict distance and never urgent: furthest first, within kEvictMs; the
    // streamer reports the rest again next tick.
    const auto evictStart = Clock::now();
    for (const u32 item : toEvict) {
        if (lastEvicted_ > 0 && msSince(evictStart) >= kEvictMs) break;
        if (item < items_.size() && items_[item].loaded) { evictItem(world, item); ++lastEvicted_; }
    }
    if (lastEvicted_) loader_.releaseUnusedShapes();
    hm.mark("evict");

    // Pinned roots load now; the rest once their meshes are read.
    std::vector<u32> now, ready;
    f32 nearestMissing = FLT_MAX;
    // Every request first, then readiness: a request may trim finished reads, never one just checked.
    loader_.prefetchTick();
    for (const u32 item : toLoad) {
        if (item >= items_.size() || items_[item].loaded || streamer_.pinned(item)) continue;
        const f32 dist = static_cast<f32>(streamer_.distance(item, viewers));
        for (const u32 pi : items_[item].placements)
            if (!removed(pi)) loader_.prefetch(records_[pi].objectId, dist);
    }
    for (const u32 item : toLoad) {
        if (item >= items_.size() || items_[item].loaded) continue;
        if (streamer_.pinned(item)) { now.push_back(item); continue; }
        bool read = true;
        for (const u32 pi : items_[item].placements)
            if (!removed(pi)) read = read && loader_.ready(records_[pi].objectId);
        nearestMissing = std::min(nearestMissing, static_cast<f32>(streamer_.distance(item, viewers)));
        if (read) ready.push_back(item);
    }
    hm.mark("prefetch");
    if (!now.empty()) {
        loadItems(now);
        lastLoaded_ += static_cast<u32>(now.size());
    }
    hm.mark("pinned");
    // Progress guarantee: when roots wait but none has all its meshes read for ~30 ticks (the prefetch pool
    // can saturate with partial sets after a teleport), the nearest waiting root loads with a direct read.
    stalledTicks_ = (ready.empty() && now.empty() && nearestMissing < FLT_MAX) ? stalledTicks_ + 1u : 0u;
    if (ready.empty() && stalledTicks_ > 30u) {
        for (const u32 item : toLoad)
            if (item < items_.size() && !items_[item].loaded && !streamer_.pinned(item)) { ready.push_back(item); break; }
        stalledTicks_ = 0;
    }
    if (ready.empty()) return;
    const f32 loadCm = streamer_.settings().loadCm;
    const f64 t = std::clamp((static_cast<f64>(nearestMissing) / loadCm - 0.25) / 0.5, 0.0, 1.0);
    const f64 budgetMs = kNearLoadMs + (kFarLoadMs - kNearLoadMs) * t;
    const auto start = Clock::now();
    std::vector<u32> one(1);
    for (usize i = 0; i < ready.size(); ++i) {
        if (i > 0 && msSince(start) >= budgetMs) break;
        one[0] = ready[i];
        loadItems(one);
        ++lastLoaded_;
    }
    hm.mark("loads");
}

const fmt::OcWorldPlacement* LevelStreaming::recordOf(scene::Entity e) const {
    const auto it = placementOf_.find(e);
    return it == placementOf_.end() ? nullptr : &records_[it->second];
}

void LevelStreaming::reload(scene::World& world, const std::vector<scene::Entity>& es) {
    std::vector<u32> items;
    for (const scene::Entity e : es) {
        const auto pl = placementOf_.find(e);
        if (pl == placementOf_.end()) continue;
        const u32 item = itemOf_[pl->second];
        if (std::find(items.begin(), items.end(), item) == items.end()) items.push_back(item);
    }
    for (const u32 item : items)
        if (items_[item].loaded) evictItem(world, item, false);
}

i32 LevelStreaming::placementOf(scene::Entity e) const {
    const auto it = placementOf_.find(e);
    return it == placementOf_.end() ? -1 : static_cast<i32>(it->second);
}

scene::Entity LevelStreaming::entityOf(u32 placement) const {
    const auto it = entityOf_.find(placement);
    return it == entityOf_.end() ? scene::kInvalidEntity : it->second;
}

void LevelStreaming::pin(u32 placement, bool on) {
    if (placement < itemOf_.size() && itemOf_[placement] != kNoItem) streamer_.pin(itemOf_[placement], on);
}

void LevelStreaming::updateRecord(u32 placement, fmt::OcWorldPlacement p, bool keepParent) {
    if (placement >= records_.size()) return;
    if (keepParent) p.parent = records_[placement].parent;
    records_[placement] = p;
    // Fresh bounds from the loaded mesh when there is one, so the saved .ocstream follows the edit.
    records_[placement].hasBounds = false;
    world::Vec3d lo, hi;
    if (worldBounds(placement, lo, hi)) {
        fmt::OcWorldPlacement& r = records_[placement];
        r.hasBounds = true;   // the file stores f32 boxes: a streaming bound is good to a centimetre at 100 km, which is all it needs
        r.boundsMin[0] = static_cast<f32>(lo.x); r.boundsMin[1] = static_cast<f32>(lo.y); r.boundsMin[2] = static_cast<f32>(lo.z);
        r.boundsMax[0] = static_cast<f32>(hi.x); r.boundsMax[1] = static_cast<f32>(hi.y); r.boundsMax[2] = static_cast<f32>(hi.z);
    } else if (p.hasBounds) {
        records_[placement].hasBounds = true;   // mesh not loaded: keep the bounds it had
    }
    if (itemOf_[placement] != kNoItem) streamer_.setBounds(itemOf_[placement], itemBounds(items_[itemOf_[placement]]));
}

u32 LevelStreaming::addRecord(const fmt::OcWorldPlacement& p, scene::Entity e) {
    const u32 pi = static_cast<u32>(records_.size());
    records_.push_back(p);
    records_.back().parent = -1;
    removed_.push_back(0);
    baseline_.push_back(world::PlacementBaseline{});
    hasBaseline_.push_back(0);
    Item item;
    item.placements.push_back(pi);
    item.loaded = true;
    const u32 idx = static_cast<u32>(items_.size());
    loader_.adopt(idx, e, p.objectId);   // the entity, and a reference on its mesh when the cache gives one
    itemOf_.push_back(idx);
    rootPlacements_.push_back(pi);
    items_.push_back(std::move(item));
    placementOf_[e] = pi;
    entityOf_[pi] = e;
    updateRecord(pi, records_[pi]);
    noteBaseline(pi, e);
    streamer_.add(itemBounds(items_[idx]));   // the streamer numbers items in add order, as items_ does
    streamer_.markLoaded(idx);
    streamer_.pin(idx, true);
    return pi;
}

void LevelStreaming::removeRecord(u32 placement) {
    if (placement >= records_.size() || removed(placement)) return;
    const u32 item = itemOf_[placement];
    if (item == kNoItem) { removed_[placement] = 1; return; }
    Item& it = items_[item];
    const bool isRoot = !it.placements.empty() && it.placements.front() == placement;
    // The placement and every later one in the item whose parent chain reaches it.
    for (const u32 pi : it.placements) {
        bool under = pi == placement;
        for (i32 q = records_[pi].parent; !under && q >= 0; q = records_[static_cast<usize>(q)].parent)
            under = static_cast<u32>(q) == placement;
        if (!under) continue;
        removed_[pi] = 1;
        const auto e = entityOf_.find(pi);
        if (e != entityOf_.end()) {
            // The host destroyed the entity and its body; nothing here may remove that body again.
            const scene::Entity gone = e->second;
            placementOf_.erase(gone);
            loader_.forget(item, gone);   // its entity, body and kinematic body
            entityOf_.erase(e);
        }
    }
    if (isRoot) {
        loader_.releaseMeshes(item);
        it.loaded = false;
        streamer_.remove(item);
    } else {
        streamer_.setBounds(item, itemBounds(it));
    }
}

} // namespace aver::game

#endif // AVER_MODULE_SCENE
